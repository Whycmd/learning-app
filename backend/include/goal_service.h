// v1.5.1-T001 GOAL-TASKS：课规服务
// 职责：课规 CRUD、唯一「正在执行」、删除后自动提升、到期自动延长、自由文本模板解析
// 配置：config/goal_templates.json（人工可改）
#pragma once

#include "db.h"
#include "json.h"
#include <fstream>
#include <sstream>
#include <string>
#include <regex>

namespace eng {

class GoalService
{
  Db& db_;
  JsonValue templates_;

  void loadTemplates(const std::string& path)
  {
    std::ifstream f(path);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    try { templates_ = parseJson(ss.str()); } catch (...) { templates_ = JsonValue(); }
  }

  const JsonValue* templateFor(const std::string& goalType) const
  {
    const JsonValue* arr = templates_.find("templates");
    if (!arr || !arr->isArray()) return nullptr;
    for (auto& t : arr->asArray())
    {
      if (t.getString("goal_type") == goalType) return &t;
    }
    return nullptr;
  }

  static int tokenDigit(const std::string& s)
  {
    static const char* cn[] = {"一", "二", "两", "三", "四", "五", "六", "七", "八", "九"};
    static const int val[] = {1, 2, 2, 3, 4, 5, 6, 7, 8, 9};
    int total = 0;
    for (int i = 0; i < 10; ++i)
    {
      if (s.find(cn[i]) != std::string::npos) total += val[i];
    }
    return total;
  }

  static int tokenToInt(const std::string& s)
  {
    if (s.empty()) return 0;
    bool digits = true;
    for (char c : s) if (c < '0' || c > '9') digits = false;
    if (digits) return std::stoi(s);
    size_t pos = s.find("十");
    if (pos != std::string::npos)
    {
      std::string left = s.substr(0, pos);
      std::string right = s.substr(pos + 3);
      int l = left.empty() ? 1 : tokenDigit(left);
      int r = right.empty() ? 0 : tokenDigit(right);
      return l * 10 + r;
    }
    return tokenDigit(s);
  }

public:
  GoalService(Db& db, const std::string& templatesPath = "config/goal_templates.json")
    : db_(db)
  {
    loadTemplates(templatesPath);
  }

  JsonArray getTemplates() const
  {
    const JsonValue* arr = templates_.find("templates");
    return (arr && arr->isArray()) ? arr->asArray() : JsonArray{};
  }

  // 当前自然周周一（YYYY-MM-DD）
  std::string currentWeekMonday()
  {
    auto rows = db_.query("SELECT DATE_FORMAT(DATE_SUB(CURDATE(), INTERVAL WEEKDAY(CURDATE()) DAY),'%Y-%m-%d') AS wk");
    return rows.empty() ? std::string() : rows[0]["wk"];
  }

  // 自由文本 → 结构化草案（规则解析，不调 AI、不落库）
  JsonValue parseText(const std::string& text) const
  {
    std::string goalType;
    if (text.find("小测") != std::string::npos || text.find("测试") != std::string::npos) goalType = "quiz_count";
    else if (text.find("新词") != std::string::npos || text.find("单词") != std::string::npos) goalType = "new_words";
    else if (text.find("复习") != std::string::npos) goalType = "review_items";

    int target = 0;
    std::smatch m;
    std::regex numRe("(\\d+)\\s*(条|个|次)?");
    if (std::regex_search(text, m, numRe)) target = std::stoi(m[1].str());

    int days = 7;
    std::regex dayRe("([0-9]+|[一二两三四五六七八九十]+)\\s*天");
    std::regex weekRe("([0-9]+|[一二两三四五六七八九十]+)\\s*周");
    std::regex monthRe("([0-9]+|[一二两三四五六七八九十]+)\\s*月");
    if (std::regex_search(text, m, dayRe)) days = tokenToInt(m[1].str());
    else if (std::regex_search(text, m, weekRe)) days = tokenToInt(m[1].str()) * 7;
    else if (std::regex_search(text, m, monthRe)) days = tokenToInt(m[1].str()) * 30;

    int minutes = 20;
    std::regex minRe("(\\d+)\\s*分钟");
    if (std::regex_search(text, m, minRe)) minutes = std::stoi(m[1].str());

    int newWords = 10;
    std::regex nwRe("(\\d+)\\s*个?\\s*新词");
    if (std::regex_search(text, m, nwRe)) newWords = std::stoi(m[1].str());

    std::string name = "我的课规";
    const JsonValue* tpl = templateFor(goalType);
    if (tpl) name = tpl->getString("name", name);
    if (target <= 0 && tpl) target = (int)tpl->getInt("target", 30);
    if (target <= 0) target = 30;
    if (goalType.empty()) goalType = "review_items";

    JsonObject o;
    o["name"] = JsonValue::makeString(name);
    o["goal_type"] = JsonValue::makeString(goalType);
    o["target"] = JsonValue::makeInt(target);
    o["period_days"] = JsonValue::makeInt(days);
    o["daily_minutes"] = JsonValue::makeInt(minutes);
    o["daily_new_words"] = JsonValue::makeInt(newWords);
    o["target_desc"] = JsonValue::makeString(name + " · " + std::to_string(days) + " 天");
    o["parse_source"] = JsonValue::makeString("template");
    o["templates"] = JsonValue::makeArray(getTemplates());
    return JsonValue::makeObject(o);
  }

  // 到期自动延长（按原周期顺延，记录 previous_deadline）
  void autoRenew(int uid)
  {
    std::string u = std::to_string(uid);
    auto rows = db_.query(
      "SELECT id FROM user_goals WHERE user_id=" + u
      + " AND status='active' AND deadline < CURDATE()");
    for (auto& row : rows)
    {
      std::string id = row["id"];
      for (int guard = 0; guard < 520; ++guard)
      {
        auto p = db_.query(
          "SELECT GREATEST(1, DATEDIFF(deadline, COALESCE(previous_deadline, start_date))) AS p,"
          " deadline < CURDATE() AS overdue FROM user_goals WHERE id=" + id + " AND user_id=" + u);
        if (p.empty() || p[0]["overdue"] != "1") break;
        std::string period = p[0]["p"];
        db_.exec(
          "UPDATE user_goals SET previous_deadline=deadline,"
          " deadline=DATE_ADD(deadline, INTERVAL " + period + " DAY),"
          " end_date=DATE_ADD(deadline, INTERVAL " + period + " DAY),"
          " auto_renewed_at=NOW()"
          " WHERE id=" + id + " AND user_id=" + u);
      }
    }
  }

  JsonArray list(int uid)
  {
    autoRenew(uid);
    auto rows = db_.query(
      "SELECT id, name, raw_text, goal_type, target_desc,"
      " DATE_FORMAT(deadline,'%Y-%m-%d') AS deadline,"
      " DATE_FORMAT(previous_deadline,'%Y-%m-%d') AS previous_deadline,"
      " DATE_FORMAT(start_date,'%Y-%m-%d') AS start_date,"
      " DATE_FORMAT(activate_week,'%Y-%m-%d') AS activate_week,"
      " daily_minutes, daily_new_words, wordlist_id, is_current, parse_source, status,"
      " DATEDIFF(CURDATE(), previous_deadline) AS expired_days,"
      " DATE_FORMAT(auto_renewed_at,'%Y-%m-%d %H:%i') AS auto_renewed_at"
      " FROM user_goals WHERE user_id=" + std::to_string(uid)
      + " AND status='active' ORDER BY is_current DESC, id DESC");

    JsonArray arr;
    for (auto& r : rows)
    {
      int ed = r["expired_days"].empty() ? 0 : std::stoi(r["expired_days"]);
      if (ed < 0) ed = 0;
      JsonObject o;
      o["id"] = JsonValue::makeInt(std::stoi(r["id"]));
      o["name"] = JsonValue::makeString(r["name"]);
      o["raw_text"] = JsonValue::makeString(r["raw_text"]);
      o["goal_type"] = JsonValue::makeString(r["goal_type"]);
      o["target_desc"] = JsonValue::makeString(r["target_desc"]);
      o["deadline"] = JsonValue::makeString(r["deadline"]);
      o["start_date"] = JsonValue::makeString(r["start_date"]);
      o["activate_week"] = JsonValue::makeString(r["activate_week"]);
      o["daily_minutes"] = JsonValue::makeInt(std::stoi(r["daily_minutes"]));
      o["daily_new_words"] = JsonValue::makeInt(std::stoi(r["daily_new_words"]));
      o["wordlist_id"] = JsonValue::makeInt(std::stoi(r["wordlist_id"]));
      o["is_current"] = JsonValue::makeBool(r["is_current"] == "1");
      o["parse_source"] = JsonValue::makeString(r["parse_source"]);
      o["status"] = JsonValue::makeString(r["status"]);
      o["expired_days"] = JsonValue::makeInt(ed);
      o["auto_renewed_at"] = JsonValue::makeString(r["auto_renewed_at"]);
      arr.push_back(JsonValue::makeObject(o));
    }
    return arr;
  }

  int currentGoalId(int uid)
  {
    auto rows = db_.query(
      "SELECT id FROM user_goals WHERE user_id=" + std::to_string(uid)
      + " AND is_current=1 AND status='active' LIMIT 1");
    return rows.empty() ? 0 : std::stoi(rows[0]["id"]);
  }

  // 新建课规：数量不设上限；新建即成为「正在执行」
  int create(int uid, const JsonValue& v, std::string& err)
  {
    std::string name = v.getString("name");
    if (name.empty()) name = "我的课规";
    std::string goalType = v.getString("goal_type", "review_items");
    std::string targetDesc = v.getString("target_desc");
    std::string rawText = v.getString("raw_text");
    std::string parseSource = v.getString("parse_source", "manual");
    int minutes = (int)v.getInt("daily_minutes", 20);
    int newWords = (int)v.getInt("daily_new_words", 10);
    int wordlistId = (int)v.getInt("wordlist_id", 0);
    int periodDays = (int)v.getInt("period_days", 7);
    if (periodDays < 1) periodDays = 7;
    if (minutes < 1) minutes = 20;
    if (newWords < 0) newWords = 0;

    std::string deadline = v.getString("deadline");
    if (deadline.empty())
    {
      auto d = db_.query("SELECT DATE_FORMAT(DATE_ADD(CURDATE(), INTERVAL " + std::to_string(periodDays) + " DAY),'%Y-%m-%d') AS d");
      deadline = d.empty() ? "" : d[0]["d"];
    }
    if (!std::regex_match(deadline, std::regex("\\d{4}-\\d{2}-\\d{2}")))
    {
      err = "deadline 格式应为 YYYY-MM-DD";
      return 0;
    }

    std::string wk = currentWeekMonday();
    std::string u = std::to_string(uid);
    int id = 0;
    db_.transaction([&]() -> bool
    {
      db_.exec("UPDATE user_goals SET is_current=0 WHERE user_id=" + u);
      auto wr = db_.exec(
        "INSERT INTO user_goals (user_id, name, raw_text, goal_type, target_desc, deadline,"
        " daily_minutes, daily_new_words, wordlist_id, is_current, parse_source, status,"
        " start_date, activate_week, end_date)"
        " VALUES (" + u + ",'" + Db::escape(name) + "','" + Db::escape(rawText) + "','"
        + Db::escape(goalType) + "','" + Db::escape(targetDesc) + "','" + Db::escape(deadline) + "',"
        + std::to_string(minutes) + "," + std::to_string(newWords) + "," + std::to_string(wordlistId)
        + ",1,'" + Db::escape(parseSource) + "','active',CURDATE(),'" + Db::escape(wk) + "','"
        + Db::escape(deadline) + "')");
      id = (int)wr.last_insert_id;
      return id > 0;
    });
    if (id <= 0) err = "创建课规失败";
    return id;
  }

  bool update(int uid, int id, const JsonValue& v, std::string& err)
  {
    auto rows = db_.query(
      "SELECT DATE_FORMAT(deadline,'%Y-%m-%d') AS deadline FROM user_goals WHERE id="
      + std::to_string(id) + " AND user_id=" + std::to_string(uid) + " LIMIT 1");
    if (rows.empty()) { err = "课规不存在"; return false; }

    std::string name = v.getString("name");
    std::string targetDesc = v.getString("target_desc");
    std::string deadline = v.getString("deadline");
    if (!deadline.empty() && !std::regex_match(deadline, std::regex("\\d{4}-\\d{2}-\\d{2}")))
    {
      err = "deadline 格式应为 YYYY-MM-DD";
      return false;
    }
    if (name.empty()) name = "我的课规";

    std::string sets = "name='" + Db::escape(name) + "'"
      + ",target_desc='" + Db::escape(targetDesc) + "'"
      + ",daily_minutes=" + std::to_string((int)v.getInt("daily_minutes", 20))
      + ",daily_new_words=" + std::to_string((int)v.getInt("daily_new_words", 10))
      + ",wordlist_id=" + std::to_string((int)v.getInt("wordlist_id", 0));
    std::string gt = v.getString("goal_type");
    if (!gt.empty()) sets += ",goal_type='" + Db::escape(gt) + "'";
    if (!deadline.empty())
    {
      // 修改期限时重置 previous_deadline
      sets += ",deadline='" + Db::escape(deadline) + "',end_date='" + Db::escape(deadline)
        + "',previous_deadline=NULL";
    }
    db_.exec("UPDATE user_goals SET " + sets + " WHERE id=" + std::to_string(id)
             + " AND user_id=" + std::to_string(uid));
    return true;
  }

  // 删除（不可恢复）；删除正在执行的课规后自动提升最近创建的一个
  bool remove(int uid, int id)
  {
    auto rows = db_.query(
      "SELECT is_current FROM user_goals WHERE id=" + std::to_string(id)
      + " AND user_id=" + std::to_string(uid) + " LIMIT 1");
    if (rows.empty()) return false;
    bool wasCurrent = rows[0]["is_current"] == "1";
    std::string u = std::to_string(uid);
    db_.transaction([&]() -> bool
    {
      db_.exec("DELETE FROM user_goals WHERE id=" + std::to_string(id) + " AND user_id=" + u);
      if (wasCurrent)
      {
        auto next = db_.query(
          "SELECT id FROM user_goals WHERE user_id=" + u
          + " AND status='active' ORDER BY id DESC LIMIT 1");
        if (!next.empty())
          db_.exec("UPDATE user_goals SET is_current=1 WHERE id=" + next[0]["id"] + " AND user_id=" + u);
      }
      return true;
    });
    return true;
  }

  // 设为「正在执行」
  bool setCurrent(int uid, int id)
  {
    auto rows = db_.query(
      "SELECT id FROM user_goals WHERE id=" + std::to_string(id)
      + " AND user_id=" + std::to_string(uid) + " AND status='active' LIMIT 1");
    if (rows.empty()) return false;
    std::string u = std::to_string(uid);
    return db_.transaction([&]() -> bool
    {
      db_.exec("UPDATE user_goals SET is_current=0 WHERE user_id=" + u);
      db_.exec("UPDATE user_goals SET is_current=1 WHERE id=" + std::to_string(id) + " AND user_id=" + u);
      return true;
    });
  }
};

} // namespace eng
