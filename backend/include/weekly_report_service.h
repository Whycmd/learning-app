// 学情周报服务（v1.5.1-T003 WEEKLY-REPORT）
// 职责：按自然周实时聚合课规/复习/错题/重练/词表数据，生成规则建议；AI 建议异步生成并缓存
// 配置：config/report_rules.json（人工可改）
#pragma once

#include "db.h"
#include "json.h"
#include "ai_client.h"
#include "ai_queue_service.h"
#include "prompts.h"
#include "error_service.h"
#include "action_service.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace eng {

class WeeklyReportService
{
public:
  struct AiResult
  {
    bool ok = false;
    int httpStatus = 200;
    std::string error;
    long long taskId = 0;
    std::string status = "none";
    std::string text;
    bool cached = false;
  };

  WeeklyReportService(Db& db, AiClient& ai, AiQueueService& queue, ActionService& actions,
                      const std::string& configPath = "config/report_rules.json")
    : db_(db), ai_(ai), queue_(queue), actions_(actions)
  {
    loadConfig(configPath);
  }

  // ========== 周区间 ==========
  // 任意日期归一到自然周周一；格式非法返回空串
  std::string normalizeWeek(const std::string& date)
  {
    std::string d = trim(date);
    if (!std::regex_match(d, std::regex("\\d{4}-\\d{2}-\\d{2}"))) return "";
    auto rows = db_.query(
      "SELECT DATE_FORMAT(DATE_SUB('" + Db::escape(d) + "', INTERVAL WEEKDAY('" +
      Db::escape(d) + "') DAY),'%Y-%m-%d') AS wk");
    return rows.empty() ? "" : rows[0]["wk"];
  }

  std::string weekEnd(const std::string& weekStart)
  {
    auto rows = db_.query(
      "SELECT DATE_FORMAT(DATE_ADD('" + Db::escape(weekStart) +
      "', INTERVAL 6 DAY),'%Y-%m-%d') AS wd");
    return rows.empty() ? "" : rows[0]["wd"];
  }

  // 上周周一
  std::string lastWeekStart()
  {
    auto rows = db_.query(
      "SELECT DATE_FORMAT(DATE_SUB(DATE_SUB(CURDATE(), INTERVAL WEEKDAY(CURDATE()) DAY),"
      " INTERVAL 7 DAY),'%Y-%m-%d') AS wk");
    return rows.empty() ? "" : rows[0]["wk"];
  }

  // ========== 首页提示：上周是否有记录 ==========
  JsonValue summary(int uid)
  {
    std::string wk = lastWeekStart();
    JsonObject o;
    o["week_start"] = JsonValue::makeString(wk);
    o["week_end"] = JsonValue::makeString(weekEnd(wk));
    o["has_data"] = JsonValue::makeBool(hasData(uid, wk));
    return JsonValue::makeObject(o);
  }

  // ========== 周报聚合 ==========
  JsonValue report(int uid, const std::string& week, bool includeWordlist)
  {
    std::string wk = normalizeWeek(week);
    if (wk.empty()) wk = lastWeekStart();

    Metrics m = metricsFor(uid, wk);
    std::vector<std::string> rules = buildRules(m);
    std::vector<std::string> days = weekDays(wk);

    JsonObject o;
    o["week_start"] = JsonValue::makeString(m.weekStart);
    o["week_end"] = JsonValue::makeString(weekEnd(m.weekStart));
    o["has_data"] = JsonValue::makeBool(m.hasData);
    o["summary_line"] = JsonValue::makeString(summaryLine(m));

    JsonObject goal;
    goal["total"] = JsonValue::makeInt(m.goalTotal);
    goal["done"] = JsonValue::makeInt(m.goalDone);
    goal["rate"] = JsonValue::makeInt(m.goalTotal > 0 ? m.goalDone * 100 / m.goalTotal : 0);
    o["goal"] = JsonValue::makeObject(goal);

    JsonArray trend;
    for (size_t i = 0; i < days.size(); ++i)
    {
      JsonObject d;
      d["date"] = JsonValue::makeString(days[i]);
      d["count"] = JsonValue::makeInt(i < m.trend.size() ? m.trend[i] : 0);
      trend.push_back(JsonValue::makeObject(d));
    }
    o["trend"] = JsonValue::makeArray(trend);

    JsonObject wrong;
    wrong["total_words"] = JsonValue::makeInt(m.wrongTotal);
    JsonObject byType;
    for (auto& kv : m.wrongByType)
      byType[kv.first] = JsonValue::makeInt(kv.second);
    wrong["by_type"] = JsonValue::makeObject(byType);
    o["wrong"] = JsonValue::makeObject(wrong);

    o["retry_pending"] = JsonValue::makeInt(m.retryPending);

    JsonArray ruleArr;
    for (auto& r : rules) ruleArr.push_back(JsonValue::makeString(r));
    o["rules"] = JsonValue::makeArray(ruleArr);

    o["wordlist"] = includeWordlist ? wordlistProgress(uid) : wordlistDisabled();
    JsonValue ai = aiState(uid, m.weekStart);
    o["ai"] = ai;
    // v1.5.1-T005：规则 + AI 建议统一成可锚定条目（动作由代码判定）
    o["suggestions"] = buildSuggestions(rules, ai.getString("text"));
    return JsonValue::makeObject(o);
  }

  // ========== 提交 AI 生成 ==========
  AiResult submitAi(int uid, const std::string& week)
  {
    AiResult r;
    std::string wk = normalizeWeek(week);
    if (wk.empty())
    {
      r.httpStatus = 400;
      r.error = "week_start 格式应为 YYYY-MM-DD";
      return r;
    }

    auto rows = db_.query(
      "SELECT status,task_id,ai_text FROM weekly_report_ai WHERE user_id=" +
      std::to_string(uid) + " AND week_start='" + Db::escape(wk) + "' LIMIT 1");
    if (!rows.empty())
    {
      std::string st = rows[0]["status"];
      long long tid = rows[0]["task_id"].empty() ? 0 : std::stoll(rows[0]["task_id"]);
      std::string text = normalizeStored(rows[0]["ai_text"]);
      if (st == "done" && !trim(text).empty())
      {
        r.ok = true; r.cached = true; r.status = "done"; r.text = text; r.taskId = tid;
        return r;
      }
      if ((st == "queued" || st == "running") && tid > 0)
      {
        std::string ts = taskStatus(tid);
        if (ts == "queued" || ts == "running")
        {
          r.ok = true; r.cached = true; r.status = ts; r.taskId = tid;
          return r;
        }
        if (ts == "done")
        {
          r.ok = true; r.cached = true; r.status = "done"; r.text = text; r.taskId = tid;
          return r;
        }
      }
    }

    auto cr = queue_.create(uid, "weekly_report", "report",
                            std::vector<AiQueueService::NewItem>{{0, wk}});
    if (!cr.ok)
    {
      r.httpStatus = cr.httpStatus;
      r.error = cr.error;
      return r;
    }

    db_.exec(
      "INSERT INTO weekly_report_ai (user_id,week_start,status,task_id,ai_text,error_msg)"
      " VALUES (" + std::to_string(uid) + ",'" + Db::escape(wk) + "','queued'," +
      std::to_string(cr.taskId) + ",'','')"
      " ON DUPLICATE KEY UPDATE status='queued',task_id=VALUES(task_id),error_msg=''");

    r.ok = true;
    r.taskId = cr.taskId;
    r.status = "queued";
    return r;
  }

  // ========== 队列 runner：kind=weekly_report ==========
  bool runTask(int uid, long long taskId)
  {
    auto items = db_.query(
      "SELECT id,input_text FROM ai_task_items WHERE task_id=" + std::to_string(taskId) +
      " AND status='pending' ORDER BY item_index,id");
    bool allOk = true;
    for (auto& it : items)
    {
      long long itemId = std::stoll(it["id"]);
      std::string wk = normalizeWeek(it["input_text"]);
      db_.exec("UPDATE ai_task_items SET status='running',started_at=NOW() WHERE id=" +
               std::to_string(itemId));
      if (wk.empty())
      {
        failItem(uid, itemId, wk, "周起始日期非法");
        allOk = false;
        continue;
      }

      Metrics m = metricsFor(uid, wk);
      std::vector<std::string> rules = buildRules(m);
      std::string rulesText;
      for (auto& r : rules) rulesText += "- " + r + "\n";

      std::string user = prompts::buildTemplate(
        prompts::WEEKLY_REPORT_USER,
        {{"{WEEK_START}", m.weekStart},
         {"{GOAL_DONE}", std::to_string(m.goalDone)},
         {"{GOAL_TOTAL}", std::to_string(m.goalTotal)},
         {"{WRONG_TOTAL}", std::to_string(m.wrongTotal)},
         {"{WRONG_TOP}", topWrongText(m)},
         {"{RETRY_PENDING}", std::to_string(m.retryPending)},
         {"{DAYS_ACTIVE}", std::to_string(m.activeDays)},
         {"{RULES}", rulesText}});
      std::vector<std::map<std::string, std::string>> msgs = {
        {{"role", "system"}, {"content", prompts::WEEKLY_REPORT_SYSTEM}},
        {{"role", "user"}, {"content", user}}};

      auto resp = ai_.postChat(msgs, true, 300, 0.5, "weekly_report");
      if (!resp.ok)
      {
        failItem(uid, itemId, wk, resp.body.empty() ? "AI 请求失败" : resp.body.substr(0, 180));
        allOk = false;
        continue;
      }

      std::string text = adviceText(resp);
      if (text.empty())
      {
        failItem(uid, itemId, wk, "AI 返回内容为空");
        allOk = false;
        continue;
      }

      db_.exec(
        "INSERT INTO weekly_report_ai (user_id,week_start,status,task_id,ai_text,error_msg)"
        " VALUES (" + std::to_string(uid) + ",'" + Db::escape(wk) + "','done'," +
        std::to_string(taskId) + ",'" + Db::escape(text) + "','')"
        " ON DUPLICATE KEY UPDATE status='done',task_id=VALUES(task_id),"
        "ai_text=VALUES(ai_text),error_msg=''");
      db_.exec("UPDATE ai_task_items SET status='done',finished_at=NOW() WHERE id=" +
               std::to_string(itemId));
    }
    return allOk;
  }

private:
  struct Metrics
  {
    std::string weekStart;
    bool hasData = false;
    int goalTotal = 0, goalDone = 0;
    std::vector<int> trend;
    int wrongTotal = 0;
    std::map<std::string, int> wrongByType;
    int retryPending = 0;
    int activeDays = 0;
  };

  Db& db_;
  AiClient& ai_;
  AiQueueService& queue_;
  ActionService& actions_;
  JsonValue cfg_;

  // ---------- 配置 ----------
  void loadConfig(const std::string& path)
  {
    std::ifstream f(path);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    try { cfg_ = parseJson(ss.str()); } catch (...) { cfg_ = JsonValue(); }
  }

  static std::string trim(const std::string& s)
  {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
  }

  static std::string cfgText(const JsonValue& cfg, const std::string& key, const std::string& def)
  {
    std::string v = cfg.getString(key);
    return v.empty() ? def : v;
  }

  long long queryInt(const std::string& sql)
  {
    auto rows = db_.query(sql);
    if (rows.empty() || rows[0].empty()) return 0;
    const std::string& v = rows[0].begin()->second;
    if (v.empty()) return 0;
    try { return std::stoll(v); } catch (...) { return 0; }
  }

  // 一次取回周内 7 天日期
  std::vector<std::string> weekDays(const std::string& weekStart)
  {
    std::vector<std::string> out;
    auto rows = db_.query(
      "SELECT DATE_FORMAT(DATE_ADD('" + Db::escape(weekStart) +
      "', INTERVAL n DAY),'%Y-%m-%d') AS d FROM ("
      "SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3"
      " UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6) x ORDER BY n");
    for (auto& r : rows) out.push_back(r["d"]);
    return out;
  }

  std::string taskStatus(long long taskId)
  {
    auto rows = db_.query("SELECT status FROM ai_tasks WHERE id=" + std::to_string(taskId));
    return rows.empty() ? "" : rows[0]["status"];
  }

  // ---------- 聚合 ----------
  bool hasData(int uid, const std::string& wk)
  {
    std::string u = std::to_string(uid);
    if (queryInt("SELECT COUNT(*) FROM user_weekly_goal WHERE user_id=" + u +
                 " AND week_key='" + Db::escape(wk) + "'") > 0) return true;
    if (queryInt("SELECT COUNT(*) FROM error_attributions WHERE user_id=" + u +
                 " AND created_at>='" + Db::escape(wk) + "' AND created_at<DATE_ADD('" +
                 Db::escape(wk) + "', INTERVAL 7 DAY)") > 0) return true;
    return queryInt("SELECT COUNT(*) FROM user_learning WHERE user_id=" + u +
                    " AND last_review_time>='" + Db::escape(wk) + "' AND last_review_time<DATE_ADD('" +
                    Db::escape(wk) + "', INTERVAL 7 DAY)") > 0;
  }

  Metrics metricsFor(int uid, const std::string& wk)
  {
    Metrics m;
    m.weekStart = wk;
    m.hasData = hasData(uid, wk);
    std::string u = std::to_string(uid);

    // 课规完成
    auto g = db_.query(
      "SELECT COUNT(*) AS total, COALESCE(SUM(achieved=1),0) AS done"
      " FROM user_weekly_goal WHERE user_id=" + u + " AND week_key='" + Db::escape(wk) + "'");
    if (!g.empty())
    {
      m.goalTotal = g[0]["total"].empty() ? 0 : std::stoi(g[0]["total"]);
      m.goalDone = g[0]["done"].empty() ? 0 : std::stoi(g[0]["done"]);
    }

    // 7 天复习趋势（与 review-calendar 同口径：last_review_time）
    std::map<std::string, int> daily;
    auto tr = db_.query(
      "SELECT DATE_FORMAT(last_review_time,'%Y-%m-%d') AS d, COUNT(*) AS c"
      " FROM user_learning WHERE user_id=" + u +
      " AND last_review_time>='" + Db::escape(wk) + "'"
      " AND last_review_time<DATE_ADD('" + Db::escape(wk) + "', INTERVAL 7 DAY)"
      " GROUP BY DATE_FORMAT(last_review_time,'%Y-%m-%d')");
    for (auto& r : tr) daily[r["d"]] = std::stoi(r["c"]);
    std::vector<std::string> days = weekDays(wk);
    for (auto& d : days)
    {
      int c = daily[d];
      m.trend.push_back(c);
      if (c > 0) m.activeDays++;
    }

    // 错题分布（周内按词去重）
    auto wt = db_.query(
      "SELECT error_type, COUNT(DISTINCT knowledge_id) AS c FROM error_attributions"
      " WHERE user_id=" + u + " AND created_at>='" + Db::escape(wk) +
      "' AND created_at<DATE_ADD('" + Db::escape(wk) + "', INTERVAL 7 DAY)"
      " GROUP BY error_type");
    for (auto& r : wt)
    {
      int c = r["c"].empty() ? 0 : std::stoi(r["c"]);
      m.wrongByType[r["error_type"]] += c;
      m.wrongTotal += c;
    }

    // 待重练（当前队列）
    m.retryPending = (int)queryInt(
      "SELECT COUNT(*) FROM retry_items WHERE user_id=" + u + " AND status='queued'");
    return m;
  }

  std::string summaryLine(const Metrics& m)
  {
    std::string goal = m.goalTotal > 0
      ? "课规完成 " + std::to_string(m.goalDone) + "/" + std::to_string(m.goalTotal)
      : "未设课规";
    return goal + " · 错题 " + std::to_string(m.wrongTotal) + " 词 · 待重练 " +
           std::to_string(m.retryPending);
  }

  // 错因排序：取数量最多的错因
  std::string topType(const Metrics& m, int& maxC)
  {
    static const char* order[] = {"spelling", "meaning", "collocation", "grammar", "unknown"};
    std::string top;
    maxC = 0;
    for (auto& t : order)
    {
      auto it = m.wrongByType.find(t);
      if (it != m.wrongByType.end() && it->second > maxC)
      {
        maxC = it->second;
        top = t;
      }
    }
    return top;
  }

  std::string topWrongText(const Metrics& m)
  {
    int maxC = 0;
    std::string t = topType(m, maxC);
    if (t.empty()) return "无";
    return ErrorService::errorLabel(t) + "（" + std::to_string(maxC) + " 词）";
  }

  std::vector<std::string> buildRules(const Metrics& m)
  {
    std::vector<std::string> out;
    if (!m.hasData)
    {
      out.push_back(cfgText(cfg_, "no_data", "本周暂无学习记录：下周从每天 10 个复习词开始。"));
      return out;
    }

    if (m.goalTotal == 0)
    {
      out.push_back(cfgText(cfg_, "no_goal", "本周还没有课规：下周先定一个小目标。"));
    }
    else if (m.goalDone < m.goalTotal)
    {
      std::string t = cfgText(cfg_, "goal_unfinished",
        "课规完成 {DONE}/{TOTAL}：下周把目标调成能稳定完成的量。");
      t = prompts::replace(t, "{DONE}", std::to_string(m.goalDone));
      t = prompts::replace(t, "{TOTAL}", std::to_string(m.goalTotal));
      out.push_back(t);
    }

    if (!m.wrongByType.empty())
    {
      int maxC = 0;
      std::string type = topType(m, maxC);
      std::string advice;
      const JsonValue* ea = cfg_.find("error_type_advice");
      if (ea && ea->isObject()) advice = ea->getString(type);
      if (advice.empty())
        advice = "错题主要集中：" + topWrongText(m) + "，下周针对该类错因专项练习。";
      out.push_back(advice);
    }

    if (m.retryPending > 0)
    {
      std::string t = cfgText(cfg_, "retry_pending", "还有 {COUNT} 个词待重练：下周先清空重练队列。");
      t = prompts::replace(t, "{COUNT}", std::to_string(m.retryPending));
      out.push_back(t);
    }

    if (out.empty())
      out.push_back(cfgText(cfg_, "good_week", "本周节奏不错：下周保持同样强度。"));
    return out;
  }

  // v1.5.1-T005：规则 + AI 建议逐条锚定；同一动作整页只挂一个（首个保留），未命中保持纯文本
  JsonValue buildSuggestions(const std::vector<std::string>& rules, const std::string& aiText)
  {
    struct Item
    {
      std::string text;
      std::string source;
      JsonValue action;
    };
    std::vector<Item> items;
    auto collect = [&](const std::string& raw, const std::string& source)
    {
      std::string text = trim(raw);
      while (!text.empty() && (text[0] == '-' || text[0] == '*' || text[0] == '#'))
        text = trim(text.substr(1));
      if (text.empty()) return;
      Item it;
      it.text = text;
      it.source = source;
      it.action = actions_.match(text);
      items.push_back(it);
    };

    for (const auto& r : rules) collect(r, "rule");
    std::stringstream ss(aiText);
    std::string line;
    while (std::getline(ss, line)) collect(line, "ai");

    // 能力级去重：同一动作整页只保留第一个（复习与重练是不同动作，各自保留）
    std::map<std::string, bool> used;
    for (auto& it : items)
    {
      if (it.action.isNull()) continue;
      std::string id = it.action.getString("id");
      if (used.find(id) != used.end()) { it.action = JsonValue(); continue; }
      used[id] = true;
    }

    JsonArray out;
    for (const auto& it : items)
    {
      JsonObject o;
      o["text"] = JsonValue::makeString(it.text);
      o["source"] = JsonValue::makeString(it.source);
      o["action"] = it.action;
      out.push_back(JsonValue::makeObject(o));
    }
    return JsonValue::makeArray(out);
  }

  // ---------- 词表进度 ----------
  JsonValue wordlistDisabled()
  {
    JsonObject o;
    o["enabled"] = JsonValue::makeBool(false);
    o["bound"] = JsonValue::makeBool(false);
    return JsonValue::makeObject(o);
  }

  JsonValue wordlistProgress(int uid)
  {
    std::string u = std::to_string(uid);
    JsonObject o;
    o["enabled"] = JsonValue::makeBool(true);

    auto g = db_.query(
      "SELECT wordlist_id FROM user_goals WHERE user_id=" + u +
      " AND is_current=1 AND status='active' ORDER BY id DESC LIMIT 1");
    int wlId = (g.empty() || g[0]["wordlist_id"].empty()) ? 0 : std::stoi(g[0]["wordlist_id"]);
    if (wlId <= 0)
    {
      o["bound"] = JsonValue::makeBool(false);
      o["guide"] = JsonValue::makeString("当前课规未绑定词表，绑定后这里会显示掌握进度。");
      return JsonValue::makeObject(o);
    }

    auto wl = db_.query(
      "SELECT id,title,source,textbook,volume,unit,scope_key FROM wordlists WHERE id=" +
      std::to_string(wlId) + " LIMIT 1");
    if (wl.empty())
    {
      o["bound"] = JsonValue::makeBool(false);
      o["guide"] = JsonValue::makeString("绑定词表不存在，请重新绑定。");
      return JsonValue::makeObject(o);
    }

    o["bound"] = JsonValue::makeBool(true);
    o["wordlist_id"] = JsonValue::makeInt(wlId);
    o["title"] = JsonValue::makeString(wl[0]["title"]);

    int total = 0, mastered = 0;
    listProgress(wlId, uid, total, mastered);
    o["total"] = JsonValue::makeInt(total);
    o["mastered"] = JsonValue::makeInt(mastered);

    JsonArray units;
    const std::string scope = wl[0]["scope_key"];
    if (scope.rfind("units:", 0) == 0)
    {
      std::stringstream ss(scope.substr(6));
      std::string key;
      while (std::getline(ss, key, '|') && units.size() < 3)
      {
        size_t pos = key.find('/');
        if (pos == std::string::npos) continue;
        std::string vol = key.substr(0, pos), unit = key.substr(pos + 1);
        auto ur = db_.query(
          "SELECT id,title,unit FROM wordlists WHERE source='builtin' AND volume='" +
          Db::escape(vol) + "' AND unit='" + Db::escape(unit) + "' LIMIT 1");
        if (ur.empty()) continue;
        int ut = 0, um = 0;
        listProgress(std::stoi(ur[0]["id"]), uid, ut, um);
        JsonObject uo;
        uo["unit"] = JsonValue::makeString(ur[0]["unit"]);
        uo["total"] = JsonValue::makeInt(ut);
        uo["mastered"] = JsonValue::makeInt(um);
        units.push_back(JsonValue::makeObject(uo));
      }
    }
    else if (!trim(wl[0]["unit"]).empty())
    {
      JsonObject uo;
      uo["unit"] = JsonValue::makeString(wl[0]["unit"]);
      uo["total"] = JsonValue::makeInt(total);
      uo["mastered"] = JsonValue::makeInt(mastered);
      units.push_back(JsonValue::makeObject(uo));
    }
    o["units"] = JsonValue::makeArray(units);
    return JsonValue::makeObject(o);
  }

  void listProgress(int listId, int uid, int& total, int& mastered)
  {
    std::string u = std::to_string(uid);
    auto rows = db_.query(
      "SELECT COUNT(*) AS total, COALESCE(SUM(EXISTS("
      "SELECT 1 FROM knowledge_points kp JOIN user_learning ul ON ul.knowledge_id=kp.id"
      " WHERE kp.user_id=" + u + " AND kp.normalized_key=wi.normalized_word"
      " AND ul.user_id=" + u + " AND ul.proficiency_level>=5)),0) AS mastered"
      " FROM wordlist_items wi WHERE wi.wordlist_id=" + std::to_string(listId));
    total = (rows.empty() || rows[0]["total"].empty()) ? 0 : std::stoi(rows[0]["total"]);
    mastered = (rows.empty() || rows[0]["mastered"].empty()) ? 0 : std::stoi(rows[0]["mastered"]);
  }

  // ---------- AI 状态 ----------
  JsonValue aiState(int uid, const std::string& wk)
  {
    JsonObject o;
    o["status"] = JsonValue::makeString("none");
    o["text"] = JsonValue::makeString("");
    o["task_id"] = JsonValue::makeInt(0);
    auto rows = db_.query(
      "SELECT status,task_id,ai_text,error_msg,DATE_FORMAT(updated_at,'%Y-%m-%d %H:%i') AS updated_at"
      " FROM weekly_report_ai WHERE user_id=" + std::to_string(uid) +
      " AND week_start='" + Db::escape(wk) + "' LIMIT 1");
    if (rows.empty()) return JsonValue::makeObject(o);

    std::string st = rows[0]["status"];
    long long tid = rows[0]["task_id"].empty() ? 0 : std::stoll(rows[0]["task_id"]);
    if ((st == "queued" || st == "running") && tid > 0)
    {
      std::string ts = taskStatus(tid);
      if (ts == "done") st = "done";
      else if (ts == "failed" || ts == "partial") st = "failed";
      else if (!ts.empty()) st = ts;
    }
    o["status"] = JsonValue::makeString(st);
    o["text"] = JsonValue::makeString(normalizeStored(rows[0]["ai_text"]));
    o["task_id"] = JsonValue::makeInt((int)tid);
    o["error"] = JsonValue::makeString(rows[0]["error_msg"]);
    o["updated_at"] = JsonValue::makeString(rows[0]["updated_at"]);
    return JsonValue::makeObject(o);
  }

  // 每条建议一行；兼容字符串数组与对象数组
  static std::string joinAdvice(const JsonValue& arr)
  {
    std::string out;
    for (auto& el : arr.asArray())
    {
      std::string s;
      if (el.isString()) s = trim(el.asString());
      else
      {
        s = trim(el.getString("text"));
        if (s.empty()) s = trim(el.getString("advice"));
        if (s.empty()) s = trim(el.getString("content"));
      }
      if (s.empty()) continue;
      if (!out.empty()) out += "\n";
      out += s;
    }
    return out;
  }

  // 兼容模型常见返回形状：{"advice":[]} / {"answer":[]} / {"suggestions":[]} / 顶层数组 / 纯字符串
  static std::string adviceText(const AiResponse& resp)
  {
    return adviceTextFromRaw(resp.extractedJson.empty() ? resp.body : resp.extractedJson);
  }

  // 历史缓存可能存的是原始 JSON，读取时再归一化一次
  static std::string normalizeStored(const std::string& text)
  {
    std::string t = trim(text);
    if (!t.empty() && (t[0] == '{' || t[0] == '[')) return adviceTextFromRaw(t);
    return text;
  }

  static std::string adviceTextFromRaw(const std::string& raw)
  {
    try
    {
      JsonValue j = parseJson(raw);
      if (j.isArray())
      {
        std::string out = joinAdvice(j);
        if (!out.empty()) return out;
      }
      for (const char* key : {"advice", "answer", "suggestions"})
      {
        const JsonValue* arr = j.find(key);
        if (arr && arr->isArray())
        {
          std::string out = joinAdvice(*arr);
          if (!out.empty()) return out;
        }
      }
      for (const char* key : {"text", "answer", "advice"})
      {
        std::string one = trim(j.getString(key));
        if (!one.empty()) return one;
      }
    }
    catch (...) {}
    return trim(raw);
  }

  void failItem(int uid, long long itemId, const std::string& wk, const std::string& err)
  {
    std::string e = err.size() > 200 ? err.substr(0, 200) : err;
    if (!wk.empty())
    {
      db_.exec("UPDATE weekly_report_ai SET status='failed',error_msg='" + Db::escape(e) +
               "' WHERE user_id=" + std::to_string(uid) + " AND week_start='" +
               Db::escape(wk) + "'");
    }
    db_.exec("UPDATE ai_task_items SET status='failed',error='" + Db::escape(e) +
             "',finished_at=NOW() WHERE id=" + std::to_string(itemId));
  }
};

} // namespace eng
