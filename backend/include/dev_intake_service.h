// 开发者台账（v1.5.2-T001 DEV-INTAKE）
// 职责：AI 导航判定处置落表（dev_backlog/dev_bugs）、精确去重计数、回执拼接、看板列表/更新/汇总（仅 admin）
// 约束：判定规则在 prompts.h AI_NAV_SYSTEM；处置表与回执文案在 config/dev_intake.json；本文件只做代码侧判定与 SQL
#pragma once

#include "db.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace eng {

class DevIntakeService
{
public:
  struct OpResult
  {
    bool ok = false;
    int httpStatus = 200;
    std::string error;
    JsonValue data;
  };

  explicit DevIntakeService(Db& db, const std::string& configPath = "config/dev_intake.json")
    : db_(db)
  {
    loadConfig(configPath);
  }

  // ---------- 沉淀（由 AiNavService 在拿到判定 JSON 后调用；任何失败都不抛异常、不阻断对话） ----------
  // 返回值：回执文本（已含编号，空串=无回执）；拼接到导航 reply 末尾
  std::string recordFromNav(int uid, const std::string& page, const std::string& text,
                            long long sourceMessageId, const JsonValue& j)
  {
    try
    {
      std::string kind = j.getString("kind");
      if (kind != "feature_request" && kind != "bug" && kind != "out_of_scope") return "";

      std::string title = trim(j.getString("title"));
      if (title.empty()) return "";
      if (utf8Length(title) > 60) title = keepChars(title, 60);

      double conf = readConfidence(j);
      bool low = conf < threshold_;
      std::string detail = utf8Length(text) > 500 ? keepChars(text, 500) : text;
      std::string norm = normalizeTitle(title);

      if (kind == "bug")
      {
        std::string category = validCategory(j.getString("category"),
                                             { "ui", "api", "data", "export", "content" }, "content");
        long long id = upsert("dev_bugs", kind, norm, uid, sourceMessageId, title, detail,
                              category, low ? "triage" : "new", conf, page);
        if (id <= 0) return "";
        return low ? fill(receiptTriage_, id) : fill(receiptBug_, id);
      }

      // feature_request / out_of_scope 都进 dev_backlog
      std::string category = kind == "out_of_scope" ? "enhancement"
                             : validCategory(j.getString("category"), { "feature", "enhancement" }, "enhancement");
      std::string scope = kind == "out_of_scope" ? "out_of_scope" : "in_scope";
      std::string status = low ? "triage" : (kind == "out_of_scope" ? "rejected" : "new");
      long long id = upsert("dev_backlog", kind, norm, uid, sourceMessageId, title, detail,
                            category, status, conf, "", scope);
      if (id <= 0) return "";
      if (low) return fill(receiptTriage_, id);
      if (kind == "out_of_scope") return ""; // 已拒绝：不附编号
      return fill(receiptBacklog_, id);
    }
    catch (...)
    {
      return ""; // 入库失败降级：对话不阻断、不显示编号
    }
  }

  // ---------- 权限 ----------
  bool isAdmin(int uid)
  {
    if (uid <= 0) return false;
    try
    {
      auto rows = db_.query("SELECT role FROM users WHERE id=" + std::to_string(uid) + " LIMIT 1");
      return !rows.empty() && rows[0]["role"] == "admin";
    }
    catch (...) { return false; }
  }

  // ---------- 看板列表（调用方已确认 admin） ----------
  JsonValue backlogList(const std::string& status, const std::string& scopeFit,
                        const std::string& priority)
  {
    std::string sql =
        "SELECT id,user_id,title,detail,category,scope_fit,confidence,occurrences,status,priority,"
        "admin_note,DATE_FORMAT(created_at,'%Y-%m-%d %H:%i') AS created_at,"
        "DATE_FORMAT(last_seen_at,'%Y-%m-%d %H:%i') AS last_seen_at FROM dev_backlog WHERE 1=1";
    sql += eqIf("status", status, /*defaultExcluded=*/"rejected");
    sql += eqIf("scope_fit", scopeFit, "");
    sql += eqIf("priority", priority, "");
    sql += " ORDER BY occurrences DESC, last_seen_at DESC LIMIT 200";
    return listToJson(db_.query(sql));
  }

  JsonValue bugList(const std::string& status, const std::string& severity,
                    const std::string& category)
  {
    std::string sql =
        "SELECT id,user_id,title,detail,category,page_path,error_code,severity,confidence,occurrences,"
        "status,duplicate_of,admin_note,DATE_FORMAT(created_at,'%Y-%m-%d %H:%i') AS created_at,"
        "DATE_FORMAT(last_seen_at,'%Y-%m-%d %H:%i') AS last_seen_at FROM dev_bugs WHERE 1=1";
    sql += eqIf("status", status, /*defaultExcluded=*/"duplicate");
    sql += eqIf("severity", severity, "");
    sql += eqIf("category", category, "");
    sql += " ORDER BY FIELD(severity,'blocker','major','minor'), occurrences DESC, last_seen_at DESC LIMIT 200";
    return listToJson(db_.query(sql));
  }

  // ---------- 更新（白名单字段 + 枚举校验） ----------
  OpResult updateBacklog(int id, const JsonValue& body)
  {
    static const std::vector<std::string> kStatus =
        { "new", "triage", "accepted", "planned", "shipped", "rejected" };
    return update("dev_backlog", id, body,
                  { { "status", kStatus }, { "priority", { "P0", "P1", "P2" } } },
                  { "admin_note" });
  }

  OpResult updateBug(int id, const JsonValue& body)
  {
    static const std::vector<std::string> kStatus =
        { "new", "triage", "confirmed", "fixing", "fixed", "closed", "duplicate" };
    static const std::vector<std::string> kSeverity = { "blocker", "major", "minor" };
    OpResult r = update("dev_bugs", id, body,
                        { { "status", kStatus }, { "severity", kSeverity } },
                        { "admin_note" });
    if (!r.ok) return r;

    // duplicate_of：仅允许指向已存在的 Bug id（不设外键，代码校验）
    const JsonValue* dup = body.find("duplicate_of");
    if (dup && !dup->isNull())
    {
      long long target = dup->asInt();
      if (target <= 0 || target == id) return fail(400, "duplicate_of 无效");
      auto rows = db_.query("SELECT id FROM dev_bugs WHERE id=" + std::to_string(target) + " LIMIT 1");
      if (rows.empty()) return fail(400, "duplicate_of 指向的记录不存在");
      db_.exec("UPDATE dev_bugs SET duplicate_of=" + std::to_string(target) + " WHERE id=" + std::to_string(id));
    }
    r.ok = true;
    return r;
  }

  // ---------- 汇总（页签徽标 + 页面可访问性探测） ----------
  JsonValue summary()
  {
    JsonObject o;
    long long backlogTotal = 0, bugsTotal = 0, triageTotal = 0;
    JsonValue b = statusCounts("dev_backlog");
    JsonValue g = statusCounts("dev_bugs");
    for (auto& kv : b.asObject())
    {
      if (kv.first != "rejected") backlogTotal += kv.second.asInt();
      if (kv.first == "triage") triageTotal += kv.second.asInt();
    }
    for (auto& kv : g.asObject())
    {
      if (kv.first != "duplicate") bugsTotal += kv.second.asInt();
      if (kv.first == "triage") triageTotal += kv.second.asInt();
    }
    o["backlog"] = b;
    o["bugs"] = g;
    o["backlog_total"] = JsonValue::makeInt(backlogTotal);
    o["bugs_total"] = JsonValue::makeInt(bugsTotal);
    o["triage_total"] = JsonValue::makeInt(triageTotal);
    return JsonValue::makeObject(o);
  }

private:
  Db& db_;

  double threshold_ = 0.6;
  std::string receiptBacklog_ = "您的建议我帮你记下来了（编号 #B{id}）。开发者安排更新时会逐条看这些反馈，合适的就会做进应用。";
  std::string receiptBug_ = "这个问题已经记录（编号 #G{id}），开发者看到会尽快排查，修复会随之后的版本更新生效。这段时间不影响你正常使用其他功能，抱歉给你添麻烦了。";
  std::string receiptTriage_ = "你的反馈已记下（编号 #T{id}）。这条需要开发者确认一下具体情况，确认后就会安排处理。";

  void loadConfig(const std::string& path)
  {
    std::ifstream f(path);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    JsonValue cfg;
    try { cfg = parseJson(ss.str()); }
    catch (...) { return; }
    if (const JsonValue* t = cfg.find("confidence_threshold")) threshold_ = numFrom(*t);
    if (threshold_ <= 0 || threshold_ > 1) threshold_ = 0.6;
    std::string v;
    if (!(v = cfg.getString("receipt_backlog")).empty()) receiptBacklog_ = v;
    if (!(v = cfg.getString("receipt_bug")).empty()) receiptBug_ = v;
    if (!(v = cfg.getString("receipt_triage")).empty()) receiptTriage_ = v;
  }

  // ---------- 判定辅助 ----------
  static double numFrom(const JsonValue& v)
  {
    if (v.isNull()) return 0.0;
    if (v.isString())
    {
      try { return std::stod(v.asString()); }
      catch (...) { return 0.0; }
    }
    return v.asDouble();
  }

  static double readConfidence(const JsonValue& j)
  {
    const JsonValue* c = j.find("confidence");
    return c ? numFrom(*c) : 0.0;
  }

  static std::string validCategory(const std::string& v, const std::vector<std::string>& allowed,
                                   const std::string& def)
  {
    return std::find(allowed.begin(), allowed.end(), v) != allowed.end() ? v : def;
  }

  // 归一化：小写、去标点（仅保留字母数字与 CJK）、合并空白
  static std::string normalizeTitle(const std::string& s)
  {
    std::string out;
    bool space = false;
    for (unsigned char c : s)
    {
      if (c >= 0x80) { out += (char)c; space = false; continue; } // UTF-8 多字节原样保留（含中文）
      if (std::isalnum(c)) { out += (char)std::tolower(c); space = false; continue; }
      if (c == ' ' || c == '\t' || c == '\n') space = true;
      // 其他标点直接丢弃
    }
    if (space && !out.empty() && out.back() != ' ') out += ' ';
    return out;
  }

  static std::string fill(const std::string& tpl, long long id)
  {
    std::string out = tpl;
    size_t p;
    while ((p = out.find("{id}")) != std::string::npos)
      out.replace(p, 4, std::to_string(id));
    return out;
  }

  // ---------- 落表：精确去重（D3/D7：命中只 occurrences+1、更新 last_seen_at，不改状态） ----------
  long long upsert(const std::string& table, const std::string& kind, const std::string& norm,
                   int uid, long long sourceMessageId, const std::string& title,
                   const std::string& detail, const std::string& category, const std::string& status,
                   double conf, const std::string& page = "", const std::string& scope = "in_scope")
  {
    std::string keySql = "SHA1(CONCAT(" + q(kind) + ",'|'," + q(norm) + "))";
    auto found = db_.query("SELECT id FROM " + table + " WHERE dedupe_key=" + keySql + " LIMIT 1");
    if (!found.empty())
    {
      long long id = std::stoll(found[0]["id"]);
      bump(table, id);
      return id;
    }
    std::string cols = "source_message_id,user_id,title,detail,category,confidence,dedupe_key,status";
    char confBuf[16];
    std::snprintf(confBuf, sizeof(confBuf), "%.2f", conf < 0 ? 0.0 : (conf > 1 ? 1.0 : conf));
    std::string vals = (sourceMessageId > 0 ? std::to_string(sourceMessageId) : "NULL") + "," +
                       std::to_string(uid < 0 ? 0 : uid) + "," + q(title) + "," + q(detail) + "," +
                       q(category) + "," + confBuf + "," + keySql + "," + q(status);
    if (table == "dev_bugs")
    {
      cols += ",page_path";
      vals += "," + q(page);
    }
    else
    {
      cols += ",scope_fit";
      vals += "," + q(scope);
    }
    try
    {
      auto res = db_.exec("INSERT INTO " + table + " (" + cols + ") VALUES (" + vals + ")");
      if (res.last_insert_id > 0) return (long long)res.last_insert_id;
    }
    catch (...)
    {
      // 并发唯一键冲突：重查后按重复路径处理
      auto again = db_.query("SELECT id FROM " + table + " WHERE dedupe_key=" + keySql + " LIMIT 1");
      if (!again.empty())
      {
        long long id = std::stoll(again[0]["id"]);
        bump(table, id);
        return id;
      }
      return -1;
    }
    auto after = db_.query("SELECT id FROM " + table + " WHERE dedupe_key=" + keySql + " LIMIT 1");
    return after.empty() ? -1 : std::stoll(after[0]["id"]);
  }

  void bump(const std::string& table, long long id)
  {
    db_.exec("UPDATE " + table + " SET occurrences=occurrences+1, last_seen_at=NOW() WHERE id=" +
             std::to_string(id));
  }

  // ---------- 查询/更新小工具 ----------
  static std::string q(const std::string& s) { return "'" + Db::escape(s) + "'"; }

  static std::string eqIf(const std::string& col, const std::string& v, const std::string& defaultExcluded)
  {
    if (!v.empty()) return " AND " + col + "=" + q(v);
    if (!defaultExcluded.empty()) return " AND " + col + "<>" + q(defaultExcluded);
    return "";
  }

  static int utf8Length(const std::string& s)
  {
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
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

  static std::string trim(const std::string& s)
  {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
  }

  JsonValue statusCounts(const std::string& table)
  {
    JsonObject o;
    for (const auto& r : db_.query("SELECT status, COUNT(*) AS c FROM " + table + " GROUP BY status"))
      o[r.at("status")] = JsonValue::makeInt(std::stoll(r.at("c")));
    return JsonValue::makeObject(o);
  }

  static JsonValue listToJson(const std::vector<std::map<std::string, std::string>>& rows)
  {
    JsonArray arr;
    for (const auto& r : rows)
    {
      JsonObject o;
      o["id"] = JsonValue::makeInt(std::stoll(r.at("id")));
      o["user_id"] = JsonValue::makeInt(std::stoll(r.count("user_id") ? r.at("user_id") : "0"));
      o["title"] = JsonValue::makeString(r.count("title") ? r.at("title") : "");
      o["detail"] = JsonValue::makeString(r.count("detail") ? r.at("detail") : "");
      o["category"] = JsonValue::makeString(r.count("category") ? r.at("category") : "");
      o["confidence"] = JsonValue::makeDouble(r.count("confidence") ? std::atof(r.at("confidence").c_str()) : 0);
      o["occurrences"] = JsonValue::makeInt(std::stoll(r.count("occurrences") ? r.at("occurrences") : "1"));
      o["status"] = JsonValue::makeString(r.count("status") ? r.at("status") : "");
      o["priority"] = JsonValue::makeString(r.count("priority") ? r.at("priority") : "");
      o["scope_fit"] = JsonValue::makeString(r.count("scope_fit") ? r.at("scope_fit") : "");
      o["page_path"] = JsonValue::makeString(r.count("page_path") ? r.at("page_path") : "");
      o["severity"] = JsonValue::makeString(r.count("severity") ? r.at("severity") : "");
      o["error_code"] = JsonValue::makeString(r.count("error_code") ? r.at("error_code") : "");
      o["duplicate_of"] = JsonValue::makeInt(r.count("duplicate_of") && !r.at("duplicate_of").empty()
                                                 && r.at("duplicate_of") != "NULL"
                                             ? std::stoll(r.at("duplicate_of")) : 0);
      o["admin_note"] = JsonValue::makeString(r.count("admin_note") ? r.at("admin_note") : "");
      o["created_at"] = JsonValue::makeString(r.count("created_at") ? r.at("created_at") : "");
      o["last_seen_at"] = JsonValue::makeString(r.count("last_seen_at") ? r.at("last_seen_at") : "");
      arr.push_back(JsonValue::makeObject(o));
    }
    return JsonValue::makeObject(JsonObject{ { "items", JsonValue::makeArray(arr) } });
  }

  struct EnumField { std::string name; std::vector<std::string> allowed; };

  // 白名单更新：body 只认 enums + freeText 中的字段；空字符串=清除（priority/admin_note 可为 NULL）
  OpResult update(const std::string& table, int id, const JsonValue& body,
                  const std::vector<EnumField>& enums, const std::vector<std::string>& freeText)
  {
    if (id <= 0) return fail(400, "invalid id");
    auto exists = db_.query("SELECT id FROM " + table + " WHERE id=" + std::to_string(id) + " LIMIT 1");
    if (exists.empty()) return fail(404, "记录不存在");

    std::vector<std::string> sets;
    for (const auto& ef : enums)
    {
      const JsonValue* v = body.find(ef.name);
      if (!v || !v->isString()) continue;
      std::string val = trim(v->asString());
      if (val.empty()) continue;
      if (std::find(ef.allowed.begin(), ef.allowed.end(), val) == ef.allowed.end())
        return fail(400, ef.name + " 取值不合法");
      sets.push_back(ef.name + "=" + q(val));
    }
    for (const auto& name : freeText)
    {
      const JsonValue* v = body.find(name);
      if (!v || !v->isString()) continue;
      std::string val = trim(v->asString());
      if (utf8Length(val) > 500) val = keepChars(val, 500);
      sets.push_back(name + (val.empty() ? "=NULL" : "=" + q(val)));
    }
    // priority 允许显式清除
    if (const JsonValue* p = body.find("priority"); p && p->isString() && trim(p->asString()).empty())
      sets.push_back("priority=NULL");

    if (sets.empty()) return fail(400, "没有可更新的字段");
    db_.exec("UPDATE " + table + " SET " + join(sets, ",") + " WHERE id=" + std::to_string(id));
    OpResult r;
    r.ok = true;
    r.data = JsonValue::makeObject(JsonObject{ { "id", JsonValue::makeInt(id) } });
    return r;
  }

  static std::string join(const std::vector<std::string>& parts, const std::string& sep)
  {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) { if (i) out += sep; out += parts[i]; }
    return out;
  }

  static OpResult fail(int status, const std::string& msg)
  {
    OpResult r;
    r.httpStatus = status;
    r.error = msg;
    return r;
  }
};

} // namespace eng
