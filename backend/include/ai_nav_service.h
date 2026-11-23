// AI 导航（v1.5.1-T005 HOME-NAV）
// 职责：全站导航问答、动作锚点、课规提案（确认后才写入 user_goals）
// 约束：AI 只负责文本；是否可跳、跳哪里、提案数值均由代码与配置判定
#pragma once

#include "action_service.h"
#include "ai_client.h"
#include "db.h"
#include "dev_intake_service.h"
#include "goal_service.h"
#include "prompts.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace eng {

class AiNavService
{
public:
  struct Result
  {
    bool ok = false;
    int httpStatus = 200;
    std::string error;
    JsonValue data;
  };

  AiNavService(Db& db, AiClient& ai, GoalService& goals, ActionService& actions,
               const std::string& configPath = "config/ai_nav.json")
    : db_(db), ai_(ai), goals_(goals), actions_(actions)
  {
    loadConfig(configPath);
  }

  // v1.5.2-T001 DEV-INTAKE：需求/Bug 沉淀服务（未注入时导航不记录台账）
  void setDevIntake(DevIntakeService* svc) { devIntake_ = svc; }

  // 欢迎语 + 快捷问题 + 当前页情境问题
  JsonValue meta(const std::string& page, bool guest) const
  {
    JsonObject o;
    o["welcome"] = JsonValue::makeString(
        guest ? cfgStr("welcome_guest", cfgStr("welcome", "你好，我是圣殿导航。"))
              : cfgStr("welcome", "你好，我是圣殿导航。"));
    JsonArray qs;
    for (const auto& q : quick_) qs.push_back(JsonValue::makeString(q));
    o["quick_questions"] = JsonValue::makeArray(qs);
    o["context_question"] = JsonValue::makeString(contextQuestion(page));
    return JsonValue::makeObject(o);
  }

  // 历史（登录用户 7 天；游客为空，由前端本页保存）
  JsonValue history(int uid, const std::string& page)
  {
    JsonValue m = meta(page, uid <= 0);
    JsonObject o;
    for (const auto& kv : m.asObject()) o[kv.first] = kv.second;

    JsonArray arr;
    if (uid > 0)
    {
      cleanup(uid);
      auto rows = db_.query(
          "SELECT * FROM (SELECT id, role, content, action_json, proposal_json, proposal_status,"
          " DATE_FORMAT(created_at,'%Y-%m-%d %H:%i') AS created_at FROM ai_nav_messages"
          " WHERE user_id=" + std::to_string(uid) +
          " ORDER BY id DESC LIMIT " + std::to_string(maxHistory_) + ") t ORDER BY id ASC");
      for (const auto& r : rows) arr.push_back(rowToJson(r));
    }
    o["messages"] = JsonValue::makeArray(arr);
    return JsonValue::makeObject(o);
  }

  Result send(int uid, const std::string& guestId, const std::string& text,
              const std::string& page,
              const std::vector<std::pair<std::string, std::string>>& clientHistory)
  {
    Result r;
    std::string msg = trim(text);
    if (msg.empty())
    {
      r.httpStatus = 400;
      r.error = "请输入你想问的内容";
      return r;
    }
    if (utf8Length(msg) > maxChars_)
    {
      r.httpStatus = 400;
      r.error = "消息太长了，请控制在 " + std::to_string(maxChars_) + " 字以内";
      return r;
    }
    if (uid <= 0 && !allowGuest(guestId))
    {
      r.httpStatus = 429;
      r.error = "问得有点快，请稍后再试";
      return r;
    }

    bool wantPlan = wantsPlan(msg);
    JsonValue action = wantPlan ? JsonValue() : actions_.match(msg, page);
    JsonValue proposal;
    std::string reply;
    bool userSaved = false;

    if (wantPlan)
    {
      proposal = buildProposal(msg);
      reply = proposalReply(proposal);
    }
    else if (!action.isNull())
    {
      // 用户明确要求动作 → 直接跳转，不调用 AI
      reply = "已为你跳转：" + action.getString("label");
    }
    else
    {
      NavAnswer a = askAi(uid, msg, page, clientHistory);
      reply = a.reply;
      JsonValue fromReply = actions_.match(reply, page);
      if (!fromReply.isNull()) action = fromReply;
      // v1.5.2-T001 DEV-INTAKE：判定有效时先落用户消息取来源 id，再沉淀台账并拼回执
      if (!a.judgment.isNull() && devIntake_)
      {
        long long srcId = 0;
        if (uid > 0) srcId = saveMessage(uid, "user", msg, JsonValue(), JsonValue(), "none");
        std::string receipt = devIntake_->recordFromNav(uid, page, msg, srcId, a.judgment);
        if (!receipt.empty()) reply += "\n" + receipt;
        if (uid > 0) userSaved = true;
      }
    }

    if (uid > 0)
    {
      if (!userSaved) saveMessage(uid, "user", msg, JsonValue(), JsonValue(), "none");
      long long aid = saveMessage(uid, "assistant", reply, action, proposal,
                                  proposal.isNull() ? "none" : "proposed");
      JsonObject d;
      d["id"] = JsonValue::makeInt((long long)aid);
      d["role"] = JsonValue::makeString("assistant");
      d["content"] = JsonValue::makeString(reply);
      d["action"] = action;
      d["proposal"] = proposal;
      d["proposal_status"] = JsonValue::makeString(proposal.isNull() ? "none" : "proposed");
      d["created_at"] = JsonValue::makeString(nowText());
      r.data = JsonValue::makeObject(d);
    }
    else
    {
      JsonObject d;
      d["id"] = JsonValue::makeInt(0);
      d["role"] = JsonValue::makeString("assistant");
      d["content"] = JsonValue::makeString(reply);
      d["action"] = action;
      d["proposal"] = proposal;
      d["proposal_status"] = JsonValue::makeString(proposal.isNull() ? "none" : "proposed");
      d["created_at"] = JsonValue::makeString(nowText());
      r.data = JsonValue::makeObject(d);
    }
    r.ok = true;
    return r;
  }

  Result confirm(int uid, long long msgId, const std::string& startWhen)
  {
    Result r;
    if (uid <= 0)
    {
      r.httpStatus = 401;
      r.error = "请先登录后再确认课规";
      return r;
    }
    cleanup(uid);
    auto rows = db_.query(
        "SELECT proposal_json, proposal_status,"
        " (created_at < (NOW() - INTERVAL " + std::to_string(ttlHours_) + " HOUR)) AS stale"
        " FROM ai_nav_messages WHERE id=" + std::to_string(msgId) +
        " AND user_id=" + std::to_string(uid) + " LIMIT 1");
    if (rows.empty())
    {
      r.httpStatus = 404;
      r.error = "提案不存在";
      return r;
    }
    std::string status = rows[0]["proposal_status"];
    if (status == "confirmed")
    {
      r.ok = true;
      r.data = JsonValue::makeObject(JsonObject{{"id", JsonValue::makeInt(msgId)}});
      return r;
    }
    if (status != "proposed")
    {
      r.httpStatus = 400;
      r.error = status == "expired" ? "提案已过期，请重新生成" : "提案已放弃";
      return r;
    }
    if (rows[0]["stale"] == "1")
    {
      db_.exec("UPDATE ai_nav_messages SET proposal_status='expired' WHERE id=" + std::to_string(msgId));
      r.httpStatus = 400;
      r.error = "提案已过期，请重新生成";
      return r;
    }

    JsonValue p;
    try { p = parseJson(rows[0]["proposal_json"]); }
    catch (...)
    {
      r.httpStatus = 500;
      r.error = "提案内容损坏，请重新生成";
      return r;
    }

    int days = clampInt((int)p.getInt("period_days", 7), 1, 365);
    int offset = (startWhen == "tomorrow") ? 1 : 0;
    auto d = db_.query("SELECT DATE_FORMAT(DATE_ADD(CURDATE(), INTERVAL " +
                       std::to_string(days + offset) + " DAY),'%Y-%m-%d') AS d");
    std::string deadline = d.empty() ? "" : d[0]["d"];

    JsonObject goal;
    goal["name"] = JsonValue::makeString(p.getString("name", "我的课规"));
    goal["goal_type"] = JsonValue::makeString(p.getString("goal_type", "review_items"));
    goal["target_desc"] = JsonValue::makeString(p.getString("target_desc", ""));
    goal["period_days"] = JsonValue::makeInt(days);
    goal["daily_minutes"] = JsonValue::makeInt(clampInt((int)p.getInt("daily_minutes", 20), 1, 600));
    goal["daily_new_words"] = JsonValue::makeInt(clampInt((int)p.getInt("daily_new_words", 10), 0, 200));
    goal["deadline"] = JsonValue::makeString(deadline);
    goal["parse_source"] = JsonValue::makeString("ai_nav");
    goal["raw_text"] = JsonValue::makeString(p.getString("raw_text", ""));

    std::string err;
    int goalId = goals_.create(uid, JsonValue::makeObject(goal), err);
    if (goalId <= 0)
    {
      r.httpStatus = 500;
      r.error = err.empty() ? "创建课规失败" : err;
      return r;
    }
    db_.exec("UPDATE ai_nav_messages SET proposal_status='confirmed' WHERE id=" + std::to_string(msgId));

    JsonObject o;
    o["message_id"] = JsonValue::makeInt(msgId);
    o["goal_id"] = JsonValue::makeInt(goalId);
    o["deadline"] = JsonValue::makeString(deadline);
    r.ok = true;
    r.data = JsonValue::makeObject(o);
    return r;
  }

  Result discard(int uid, long long msgId)
  {
    Result r;
    auto rows = db_.query(
        "SELECT proposal_status FROM ai_nav_messages WHERE id=" + std::to_string(msgId) +
        " AND user_id=" + std::to_string(uid) + " LIMIT 1");
    if (rows.empty())
    {
      r.httpStatus = 404;
      r.error = "提案不存在";
      return r;
    }
    if (rows[0]["proposal_status"] == "proposed")
    {
      db_.exec("UPDATE ai_nav_messages SET proposal_status='discarded' WHERE id=" + std::to_string(msgId));
    }
    r.ok = true;
    r.data = JsonValue::makeObject(JsonObject{{"id", JsonValue::makeInt(msgId)}});
    return r;
  }

  // 清空登录用户的导航对话（游客对话只存前端，不落库）
  bool clear(int uid)
  {
    if (uid <= 0) return false;
    db_.exec("DELETE FROM ai_nav_messages WHERE user_id=" + std::to_string(uid));
    return true;
  }

private:
  Db& db_;
  AiClient& ai_;
  GoalService& goals_;
  ActionService& actions_;
  DevIntakeService* devIntake_ = nullptr;

  JsonValue cfg_;
  std::vector<std::string> quick_;
  int maxChars_ = 500;
  int ratePerMinute_ = 6;
  int sessionDays_ = 7;
  int maxHistory_ = 40;
  int ctxMessages_ = 6;
  int ttlHours_ = 24;
  int maxTokens_ = 600;
  double temperature_ = 0.5;

  std::mutex rateMu_;
  std::map<std::string, std::deque<time_t>> rate_;

  // ---------- 小工具 ----------
  static std::string trim(const std::string& s)
  {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
  }

  static int utf8Length(const std::string& s)
  {
    int n = 0;
    for (unsigned char c : s)
    {
      if ((c & 0xC0) != 0x80) n++;
    }
    return n;
  }

  static int clampInt(int v, int lo, int hi)
  {
    return v < lo ? lo : (v > hi ? hi : v);
  }

  static std::string q(const std::string& s)
  {
    return "'" + Db::escape(s) + "'";
  }

  static std::string nowText()
  {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return std::string(buf);
  }

  std::string cfgStr(const std::string& key, const std::string& def) const
  {
    std::string v = cfg_.getString(key);
    return v.empty() ? def : v;
  }

  int cfgInt(const std::string& key, int def) const
  {
    long long v = cfg_.getInt(key, def);
    return (int)v;
  }

  void loadConfig(const std::string& path)
  {
    std::ifstream f(path);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    try { cfg_ = parseJson(ss.str()); }
    catch (...) { return; }

    maxChars_ = std::max(50, cfgInt("max_chars", 500));
    ratePerMinute_ = std::max(1, cfgInt("rate_per_minute", 6));
    sessionDays_ = std::max(1, cfgInt("session_days", 7));
    maxHistory_ = std::max(1, cfgInt("max_history", 40));
    ctxMessages_ = std::max(0, cfgInt("ctx_messages", 6));
    ttlHours_ = std::max(1, cfgInt("proposal_ttl_hours", 24));
    maxTokens_ = std::max(64, cfgInt("max_tokens", 600));
    temperature_ = 0.5;

    const JsonValue* arr = cfg_.find("quick_questions");
    if (arr && arr->isArray())
    {
      for (const auto& v : arr->asArray())
      {
        if (v.isString() && !v.asString().empty()) quick_.push_back(v.asString());
      }
    }
  }

  // ---------- 页面与情境问题 ----------
  static std::string pageKey(const std::string& page)
  {
    if (page.find("learning-center") != std::string::npos) return "weekly";
    if (page.find("wrongbook") != std::string::npos) return "wrongbook";
    if (page.find("review") != std::string::npos) return "review";
    if (page.find("kp-") != std::string::npos) return "kp";
    if (page.find("wordbook") != std::string::npos) return "wordbook";
    if (page.find("plan") != std::string::npos) return "plan";
    if (page.find("daily-quiz") != std::string::npos) return "quiz";
    return "index";
  }

  std::string contextQuestion(const std::string& page) const
  {
    std::string key = pageKey(page);
    const JsonValue* map = cfg_.find("context_questions");
    if (map && map->isObject())
    {
      std::string v = map->getString(key);
      if (!v.empty()) return v;
      std::string d = map->getString("default");
      if (!d.empty()) return d;
    }
    return "这个页面能做什么";
  }

  static std::string pageHint(const std::string& page)
  {
    std::string key = pageKey(page);
    if (key == "review") return "用户当前在「每日复习」页。";
    if (key == "wrongbook") return "用户当前在「错题本」页。";
    if (key == "weekly") return "用户当前在「学情周报」页。";
    if (key == "kp") return "用户当前在「知识点」页。";
    if (key == "wordbook") return "用户当前在「词表库」页。";
    if (key == "plan") return "用户当前在「课规」页。";
    if (key == "quiz") return "用户当前在「每日小测」页。";
    return "用户当前在「首页」。";
  }

  // ---------- 意图与提案 ----------
  static bool containsAny(const std::string& text, const std::vector<std::string>& keys)
  {
    for (const auto& k : keys)
    {
      if (text.find(k) != std::string::npos) return true;
    }
    return false;
  }

  static bool wantsPlan(const std::string& text)
  {
    return containsAny(text, {"排个计划", "排计划", "定个目标", "定目标", "制定计划",
                              "安排计划", "学习计划", "帮我规划", "规划一下", "定个课规"});
  }

  JsonValue buildProposal(const std::string& text)
  {
    JsonValue draft = goals_.parseText(text);
    JsonObject p;
    p["name"] = JsonValue::makeString(draft.getString("name", "我的课规"));
    p["goal_type"] = JsonValue::makeString(draft.getString("goal_type", "review_items"));
    p["target"] = JsonValue::makeInt(clampInt((int)draft.getInt("target", 30), 1, 10000));
    p["period_days"] = JsonValue::makeInt(clampInt((int)draft.getInt("period_days", 7), 1, 365));
    p["daily_minutes"] = JsonValue::makeInt(clampInt((int)draft.getInt("daily_minutes", 20), 1, 600));
    p["daily_new_words"] = JsonValue::makeInt(clampInt((int)draft.getInt("daily_new_words", 10), 0, 200));
    p["target_desc"] = JsonValue::makeString(draft.getString("target_desc", ""));
    p["activate_from"] = JsonValue::makeString("today");
    p["raw_text"] = JsonValue::makeString(text);
    auto e = db_.query("SELECT DATE_FORMAT(DATE_ADD(NOW(), INTERVAL " + std::to_string(ttlHours_) +
                       " HOUR),'%Y-%m-%d %H:%i') AS e");
    p["expires_at"] = JsonValue::makeString(e.empty() ? "" : e[0]["e"]);
    return JsonValue::makeObject(p);
  }

  std::string proposalReply(const JsonValue& p)
  {
    std::string text = "给你排了一份课规：" + p.getString("target_desc") + "，每天 "
                       + std::to_string((int)p.getInt("daily_minutes", 20)) + " 分钟，新词 "
                       + std::to_string((int)p.getInt("daily_new_words", 10)) + " 个。"
                       + cfgStr("proposal_hint", "确认后生效。");
    return text;
  }

  // ---------- AI ----------
  struct NavAnswer
  {
    std::string reply;
    JsonValue judgment; // 判定 JSON（answerable 也算有效判定）；空=解析失败或旧格式，降级不记录
  };

  NavAnswer askAi(int uid, const std::string& text, const std::string& page,
                  const std::vector<std::pair<std::string, std::string>>& clientHistory)
  {
    std::string fallback = cfgStr("fallback_reply", "AI 暂时没回应，请先试试每日复习或每日小测。");
    try
    {
      std::vector<std::map<std::string, std::string>> msgs;
      std::string sys = prompts::buildTemplate(
          prompts::AI_NAV_SYSTEM,
          {{"{PAGE}", pageHint(page)},
           {"{LEARNER_IDENTITY}", prompts::getLearnerIdentity(db_, uid)}});
      msgs.push_back({{"role", "system"}, {"content", sys}});

      std::vector<std::pair<std::string, std::string>> ctx = clientHistory;
      if (uid > 0)
      {
        auto rows = db_.query(
            "SELECT role, content FROM (SELECT id, role, content FROM ai_nav_messages"
            " WHERE user_id=" + std::to_string(uid) + " ORDER BY id DESC LIMIT " +
            std::to_string(ctxMessages_) + ") t ORDER BY id ASC");
        ctx.clear();
        for (const auto& r : rows) ctx.push_back({r.at("role"), r.at("content")});
      }
      for (const auto& c : ctx)
      {
        if (c.second.empty()) continue;
        msgs.push_back({{"role", c.first == "assistant" ? "assistant" : "user"},
                        {"content", c.second}});
      }
      msgs.push_back({{"role", "user"}, {"content", text}});

      auto resp = ai_.postChat(msgs, false, maxTokens_, temperature_, "ai_nav");
      if (!resp.ok) return {fallback, JsonValue()};

      // v1.5.2-T001：先尝试判定 JSON 契约（prompts.h AI_NAV_SYSTEM），失败降级原纯文本路径
      JsonValue judgment;
      std::string out;
      if (!resp.extractedJson.empty())
      {
        try
        {
          JsonValue j = parseJson(resp.extractedJson);
          if (j.isObject())
          {
            std::string kind = j.getString("kind");
            if (kind == "answerable" || kind == "feature_request" || kind == "bug" ||
                kind == "out_of_scope")
            {
              judgment = j;
              out = trim(j.getString("reply"));
            }
          }
        }
        catch (...) {}
      }
      if (out.empty()) out = plainText(resp);
      if (out.empty()) return {fallback, JsonValue()};
      if (utf8Length(out) > 800) out = keepChars(out, 800);
      return {out, judgment};
    }
    catch (...)
    {
      return {fallback, JsonValue()};
    }
  }

  static std::string keepChars(const std::string& s, int n)
  {
    int count = 0;
    size_t i = 0;
    while (i < s.size() && count < n)
    {
      unsigned char c = (unsigned char)s[i];
      if ((c & 0xC0) != 0x80) count++;
      i++;
      while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    }
    return s.substr(0, i);
  }

  static std::string plainText(const AiResponse& r)
  {
    if (!r.extractedJson.empty())
    {
      try
      {
        JsonValue j = parseJson(r.extractedJson);
        if (j.isString()) return trim(j.asString());
        if (j.isObject())
        {
          for (const char* k : {"reply", "answer", "text", "content"})
          {
            std::string v = j.getString(k);
            if (!v.empty()) return trim(v);
          }
          const JsonValue* arr = j.find("answer");
          if (arr && arr->isArray())
          {
            std::string joined;
            for (const auto& v : arr->asArray())
            {
              if (!v.isString()) continue;
              if (!joined.empty()) joined += "\n";
              joined += v.asString();
            }
            if (!joined.empty()) return trim(joined);
          }
        }
      }
      catch (...) {}
    }
    return bodyContent(r.body);
  }

  // 从上游 HTTP 响应体提取 "content"（兼容 \u 转义）
  static std::string bodyContent(const std::string& body)
  {
    size_t pos = body.find("\"content\":\"");
    if (pos == std::string::npos) return "";
    pos += std::string("\"content\":\"").size();
    std::string out;
    while (pos < body.size())
    {
      char c = body[pos];
      if (c == '\\' && pos + 1 < body.size())
      {
        char e = body[pos + 1];
        if (e == 'n') { out += '\n'; pos += 2; }
        else if (e == 't') { out += '\t'; pos += 2; }
        else if (e == 'r') { out += '\r'; pos += 2; }
        else if (e == '"') { out += '"'; pos += 2; }
        else if (e == '\\') { out += '\\'; pos += 2; }
        else if (e == 'u' && pos + 5 < body.size())
        {
          try
          {
            int code = std::stoi(body.substr(pos + 2, 4), nullptr, 16);
            appendUtf8(out, code);
            pos += 6;
          }
          catch (...)
          {
            out += e;
            pos += 2;
          }
        }
        else { out += e; pos += 2; }
      }
      else if (c == '"') break;
      else { out += c; pos++; }
    }
    return trim(out);
  }

  static void appendUtf8(std::string& out, int code)
  {
    if (code < 0x80) out += (char)code;
    else if (code < 0x800)
    {
      out += (char)(0xC0 | (code >> 6));
      out += (char)(0x80 | (code & 0x3F));
    }
    else
    {
      out += (char)(0xE0 | (code >> 12));
      out += (char)(0x80 | ((code >> 6) & 0x3F));
      out += (char)(0x80 | (code & 0x3F));
    }
  }

  // ---------- 会话与限流 ----------
  bool allowGuest(const std::string& gid)
  {
    if (gid.empty()) return true;
    std::lock_guard<std::mutex> lk(rateMu_);
    time_t now = std::time(nullptr);
    auto& dq = rate_[gid];
    while (!dq.empty() && now - dq.front() > 60) dq.pop_front();
    if ((int)dq.size() >= ratePerMinute_) return false;
    dq.push_back(now);
    if (rate_.size() > 2000)
    {
      for (auto it = rate_.begin(); it != rate_.end();)
      {
        if (it->second.empty()) it = rate_.erase(it);
        else ++it;
      }
    }
    return true;
  }

  void cleanup(int uid)
  {
    db_.exec("DELETE FROM ai_nav_messages WHERE user_id=" + std::to_string(uid) +
             " AND created_at < (NOW() - INTERVAL " + std::to_string(sessionDays_) + " DAY)");
    db_.exec("UPDATE ai_nav_messages SET proposal_status='expired' WHERE user_id=" +
             std::to_string(uid) + " AND proposal_status='proposed' AND created_at < (NOW() - INTERVAL "
             + std::to_string(ttlHours_) + " HOUR)");
  }

  long long saveMessage(int uid, const std::string& role, const std::string& content,
                        const JsonValue& action, const JsonValue& proposal,
                        const std::string& status)
  {
    std::string actionJson = action.isNull() ? "" : action.dump();
    std::string proposalSql = proposal.isNull() ? "NULL" : q(proposal.dump());
    auto res = db_.exec(
        "INSERT INTO ai_nav_messages (user_id, role, content, action_json, proposal_json, proposal_status)"
        " VALUES (" + std::to_string(uid) + "," + q(role) + "," + q(content) + ","
        + q(actionJson) + "," + proposalSql + "," + q(status) + ")");
    return res.last_insert_id;
  }

  static JsonValue rowToJson(const std::map<std::string, std::string>& r)
  {
    JsonObject o;
    o["id"] = JsonValue::makeInt(std::stoll(r.at("id")));
    o["role"] = JsonValue::makeString(r.at("role"));
    o["content"] = JsonValue::makeString(r.at("content"));
    o["created_at"] = JsonValue::makeString(r.at("created_at"));

    JsonValue action;
    if (r.count("action_json") && !r.at("action_json").empty())
    {
      try { action = parseJson(r.at("action_json")); }
      catch (...) {}
    }
    o["action"] = action;

    JsonValue proposal;
    std::string status = r.count("proposal_status") ? r.at("proposal_status") : "none";
    if (r.count("proposal_json") && !r.at("proposal_json").empty())
    {
      try { proposal = parseJson(r.at("proposal_json")); }
      catch (...) {}
    }
    if (!proposal.isNull())
    {
      JsonObject p = proposal.asObject();
      p["status"] = JsonValue::makeString(status);
      proposal = JsonValue::makeObject(p);
    }
    o["proposal"] = proposal;
    o["proposal_status"] = JsonValue::makeString(status);
    return JsonValue::makeObject(o);
  }
};

} // namespace eng
