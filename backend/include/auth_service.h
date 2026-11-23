// 认证业务服务层（v1.3.0-T001 多考生类型）
// 职责：注册 / 登录 / 登出 / 会话解析 / 用户资料更新 / 学习者身份
// 会话 Cookie（由 main.cpp handler 写入）：eng_session=<token>; Path=/; HttpOnly; Max-Age=2592000
#pragma once

#include "db.h"
#include "auth.h"
#include "prompts.h"
#include <string>
#include <vector>
#include <map>

namespace eng
{

class AuthService
{
  Db& db_;

public:
  // 0 = 未登录（无账号）；1 = 系统账号，仅供测试使用
  static constexpr int NO_USER = 0;
  static constexpr int SYSTEM_USER_ID = 1;
  static constexpr int SESSION_DAYS = 30;

  AuthService(Db& db) : db_(db) {}

  struct Result
  {
    bool ok = false;
    int code = 400;
    std::string error;
    int64_t user_id = 0;
    std::string username;
    std::string display_name;
    std::string learner_identity;
    std::string ui_mode;
    std::string token;
  };

  // ---- 注册（v1.3.0 新增 learner_identity 参数）----

  Result regist(const std::string& username, const std::string& password,
                const std::string& displayName, const std::string& learnerIdentity = "")
  {
    Result r;
    if (!validUsername(username, r.error)) { r.code = 400; return r; }
    if (password.size() < 6 || password.size() > 100)
    {
      r.code = 400; r.error = "密码长度需为 6-100 个字符"; return r;
    }
    // 重名检查
    auto dup = db_.query("SELECT id FROM users WHERE username='" + Db::escape(username) + "' LIMIT 1");
    if (!dup.empty()) { r.code = 409; r.error = "用户名已存在"; return r; }

    std::string salt = Auth::newSalt();
    std::string hash = Auth::hashPassword(password, salt);
    std::string dn = displayName.empty() ? username : displayName;
    std::string li = learnerIdentity.empty() ? prompts::DEFAULT_LEARNER_IDENTITY : learnerIdentity;
    try
    {
      auto wr = db_.exec(
        "INSERT INTO users (username, password_hash, salt, display_name, learner_identity) VALUES ('"
        + Db::escape(username) + "','" + hash + "','" + salt + "','" + Db::escape(dn)
        + "','" + Db::escape(li) + "')"
      );
      r.ok = true;
      r.user_id = wr.last_insert_id;
      r.username = username;
      r.display_name = dn;
      r.learner_identity = li;
    }
    catch (const std::exception& e)
    {
      r.code = 500; r.error = e.what();
    }
    return r;
  }

  // ---- 登录 ----

  Result login(const std::string& username, const std::string& password)
  {
    Result r;
    auto rows = db_.query(
      "SELECT id, password_hash, salt, display_name FROM users WHERE username='"
      + Db::escape(username) + "' LIMIT 1"
    );
    if (rows.empty() || !Auth::verifyPassword(password, rows[0]["salt"], rows[0]["password_hash"]))
    {
      r.code = 401; r.error = "用户名或密码错误"; return r;
    }
    r.ok = true;
    r.user_id = std::stoll(rows[0]["id"]);
    r.username = username;
    r.display_name = rows[0]["display_name"];

    // 创建会话
    std::string token = Auth::newSessionToken();
    try
    {
      db_.exec(
        "INSERT INTO user_sessions (token, user_id, expires_at) VALUES ('"
        + token + "'," + std::to_string(r.user_id) + ", DATE_ADD(NOW(), INTERVAL "
        + std::to_string(SESSION_DAYS) + " DAY))"
      );
      db_.exec("UPDATE users SET last_login_at=NOW() WHERE id=" + std::to_string(r.user_id));
      r.token = token;
    }
    catch (const std::exception& e)
    {
      r.ok = false; r.code = 500; r.error = e.what();
    }
    return r;
  }

  // ---- 登出 ----

  bool logout(const std::string& token)
  {
    if (token.empty()) return true;
    try { db_.exec("DELETE FROM user_sessions WHERE token='" + Db::escape(token) + "'"); }
    catch (...) { return false; }
    return true;
  }

  // ---- 注销账号（仅普通注册用户）----
  // 级联删除该用户的知识点及其全部子表、学情、单词本、会话
  Result deregister(int userId)
  {
    Result r;
    if (userId <= SYSTEM_USER_ID) { r.code = 403; r.error = "游客账号不可注销"; return r; }
    std::string uid = std::to_string(userId);
    bool ok = db_.transaction([&]() -> bool
    {
      // 学情（按 user_id）
      db_.exec("DELETE FROM user_learning WHERE user_id=" + uid);
      // 单词本（按 user_id；条目通过 wordbook_id 级联）
      db_.exec("DELETE FROM wordbook_entry WHERE wordbook_id IN (SELECT id FROM wordbook WHERE user_id=" + uid + ")");
      db_.exec("DELETE FROM wordbook WHERE user_id=" + uid);
      // 词表（v1.5.0-T003）
      // v1.5.0-T005：先清组合成员指针，避免 wordlists 删除后残留悬空引用
      db_.exec("DELETE FROM wordlist_children WHERE parent_id IN (SELECT id FROM wordlists WHERE user_id=" + uid + ")"
               " OR (child_type='list' AND child_id IN (SELECT id FROM wordlists WHERE user_id=" + uid + "))"
               " OR (child_type='word' AND child_id IN (SELECT id FROM wordlist_items WHERE wordlist_id IN (SELECT id FROM wordlists WHERE user_id=" + uid + ")))");
      db_.exec("DELETE FROM wordlist_items WHERE wordlist_id IN (SELECT id FROM wordlists WHERE user_id=" + uid + ")");
      db_.exec("DELETE FROM wordlists WHERE user_id=" + uid);
      // 用户全部 AI 任务（含 ref_id=0 的词表生成任务）
      db_.exec("DELETE FROM ai_task_items WHERE task_id IN (SELECT id FROM ai_tasks WHERE user_id=" + uid + ")");
      db_.exec("DELETE FROM ai_tasks WHERE user_id=" + uid);
      // 知识点子表（按 knowledge_id）
      db_.exec("DELETE FROM kp_definitions WHERE knowledge_id IN (SELECT id FROM knowledge_points WHERE user_id=" + uid + ")");
      db_.exec("DELETE FROM kp_collocations WHERE knowledge_id IN (SELECT id FROM knowledge_points WHERE user_id=" + uid + ")");
      db_.exec("DELETE FROM kp_synonyms WHERE knowledge_id IN (SELECT id FROM knowledge_points WHERE user_id=" + uid + ")");
      db_.exec("DELETE FROM kp_antonyms WHERE knowledge_id IN (SELECT id FROM knowledge_points WHERE user_id=" + uid + ")");
      db_.exec("DELETE FROM kp_extras WHERE knowledge_id IN (SELECT id FROM knowledge_points WHERE user_id=" + uid + ")");
      db_.exec("DELETE FROM ai_tasks WHERE ref_id IN (SELECT id FROM knowledge_points WHERE user_id=" + uid + ")");
      db_.exec("DELETE FROM knowledge_points WHERE user_id=" + uid);
      // 每日小测与词币（v1.2.0-T007）
      db_.exec("DELETE FROM daily_quiz WHERE user_id=" + uid);
      db_.exec("DELETE FROM user_coin WHERE user_id=" + uid);
      // v1.5.1-T001：课规与本周任务卡
      db_.exec("DELETE FROM user_weekly_goal WHERE user_id=" + uid);
      db_.exec("DELETE FROM user_goals WHERE user_id=" + uid);
      // 会话与账号本身
      db_.exec("DELETE FROM user_sessions WHERE user_id=" + uid);
      db_.exec("DELETE FROM users WHERE id=" + uid);
      return true;
    });
    if (ok) { r.ok = true; r.user_id = userId; }
    else { r.code = 500; r.error = "注销失败"; }
    return r;
  }

  // ---- 会话解析 ----

  // 返回 user_id；无效/过期 → NO_USER（0）
  int resolveSession(const std::string& token)
  {
    if (token.empty()) return NO_USER;
    auto rows = db_.query(
      "SELECT user_id FROM user_sessions WHERE token='" + Db::escape(token)
      + "' AND expires_at > NOW() LIMIT 1"
    );
    if (rows.empty()) return NO_USER;
    int uid = std::stoi(rows[0]["user_id"]);
    return uid > NO_USER ? uid : NO_USER;
  }

  // 用户展示信息（/api/auth/me）
  // v1.3.0：返回中新增 learner_identity；v1.5.0-T004：新增 ui_mode
  std::map<std::string, std::string> userInfo(int userId)
  {
    std::map<std::string, std::string> info;
    if (userId <= NO_USER) return info;
    auto rows = db_.query(
      "SELECT username, display_name, learner_identity, ui_mode FROM users WHERE id=" + std::to_string(userId) + " LIMIT 1"
    );
    if (rows.empty()) return info;
    info["username"] = rows[0]["username"];
    info["display_name"] = rows[0]["display_name"];
    info["learner_identity"] = rows[0]["learner_identity"];
    info["ui_mode"] = rows[0]["ui_mode"];
    return info;
  }

  // v1.3.0-T001：获取用户学习者身份（未登录或空值 → 返回默认）
  // 供 5 处 AI 调用注入 {LEARNER_IDENTITY} 占位符
  std::string learnerIdentity(int userId)
  {
    if (userId <= NO_USER) return prompts::DEFAULT_LEARNER_IDENTITY;
    auto rows = db_.query(
      "SELECT learner_identity FROM users WHERE id=" + std::to_string(userId) + " LIMIT 1"
    );
    if (rows.empty()) return prompts::DEFAULT_LEARNER_IDENTITY;
    std::string id = rows[0]["learner_identity"];
    return id.empty() ? prompts::DEFAULT_LEARNER_IDENTITY : id;
  }

  // v1.3.0-T001：更新用户资料（display_name / learner_identity 均可为空，空则不改）
  // v1.5.0-T004：新增 ui_mode，只接受 function / plan，空则不改
  Result updateProfile(int userId, const std::string& displayName,
                       const std::string& learnerIdentity, const std::string& uiMode = "")
  {
    Result r;
    if (userId <= NO_USER) { r.code = 403; r.error = "游客账号不可修改资料"; return r; }
    if (!uiMode.empty() && uiMode != "function" && uiMode != "plan")
    {
      r.code = 400; r.error = "界面模式仅支持 function 或 plan"; return r;
    }
    std::vector<std::string> sets;
    if (!displayName.empty())
      sets.push_back("display_name='" + Db::escape(displayName) + "'");
    if (!learnerIdentity.empty())
      sets.push_back("learner_identity='" + Db::escape(learnerIdentity) + "'");
    if (!uiMode.empty())
      sets.push_back("ui_mode='" + Db::escape(uiMode) + "'");
    if (sets.empty()) { r.code = 400; r.error = "未提供任何修改项"; return r; }
    try
    {
      db_.exec("UPDATE users SET " + joinCsv(sets) + " WHERE id=" + std::to_string(userId));
      r.ok = true;
      r.user_id = userId;
      if (!displayName.empty()) r.display_name = displayName;
      if (!learnerIdentity.empty()) r.learner_identity = learnerIdentity;
      if (!uiMode.empty()) r.ui_mode = uiMode;
    }
    catch (const std::exception& e)
    {
      r.code = 500; r.error = e.what();
    }
    return r;
  }

  // 清理过期会话（可在启动时调用）
  int purgeExpired()
  {
    try
    {
      auto wr = db_.exec("DELETE FROM user_sessions WHERE expires_at <= NOW()");
      return static_cast<int>(wr.affected_rows);
    }
    catch (...) { return 0; }
  }

private:
  // 用户名：3-20 字符，字母/数字/下划线/点/中文，无空白
  static bool validUsername(const std::string& u, std::string& err)
  {
    if (u.size() < 3 || u.size() > 20) { err = "用户名长度需为 3-20 个字符"; return false; }
    for (char c : u)
    {
      bool asciiOk = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                      || (c >= '0' && c <= '9') || c == '_' || c == '.');
      bool utf8Lead = static_cast<unsigned char>(c) >= 0x80; // 中文字符的 UTF-8 首字节
      if (!asciiOk && !utf8Lead) { err = "用户名仅支持中英文、数字、下划线和点"; return false; }
    }
    return true;
  }

  // 辅助：vector<string> 用逗号拼成 "a='x', b='y'" 形式
  static std::string joinCsv(const std::vector<std::string>& parts)
  {
    std::string r;
    for (size_t i = 0; i < parts.size(); ++i)
    {
      if (i > 0) r += ',';
      r += parts[i];
    }
    return r;
  }
};

} // namespace eng
