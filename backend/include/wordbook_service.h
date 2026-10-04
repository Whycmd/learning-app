// 单词本业务服务层（v1.2.0-T003）
// 职责：单词本 CRUD、词条管理、导入导出
// 设计原则：wordbook_entry 仅存 knowledge_id 引用，词条内容复用 knowledge_points
#pragma once
#include "db.h"
#include "json.h"
#include "auth.h"
#include "knowledge_service.h"
#include <string>
#include <vector>
#include <map>
#include <ctime>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <random>
#include <unistd.h>

namespace eng {

class WordbookService {
  Db& db_;
  KnowledgeService& kpSvc_;
  std::string wordTmpDir_;
public:
  WordbookService(Db& db, KnowledgeService& kpSvc, const std::string& wordTmp)
    : db_(db), kpSvc_(kpSvc), wordTmpDir_(wordTmp) {}

  // ============ 单词本 CRUD ============

  // 列表
  JsonValue list() {
    auto rows = db_.query(
      "SELECT id, user_id, name, description, share_code, is_public, entry_count, "
      "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at "
      "FROM wordbook WHERE user_id=" + std::to_string(RequestContext::currentUserId()) + " ORDER BY id DESC"
    );
    JsonArray arr;
    for (auto& r : rows) {
      JsonObject o;
      o["id"] = JsonValue::makeInt(std::stoll(r["id"]));
      o["name"] = JsonValue::makeString(r["name"]);
      o["description"] = r["description"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["description"]);
      o["share_code"] = r["share_code"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["share_code"]);
      o["is_public"] = JsonValue::makeInt(std::stoi(r["is_public"]));
      o["entry_count"] = JsonValue::makeInt(std::stoi(r["entry_count"]));
      o["created_at"] = JsonValue::makeString(r["created_at"]);
      arr.push_back(JsonValue::makeObject(o));
    }
    return JsonValue::makeObject({{"data", JsonValue::makeArray(arr)}});
  }

  // 创建
  struct CreateResult { int64_t id=0; bool ok=false; std::string error; };
  CreateResult create(const std::string& name, const std::string& description="") {
    CreateResult r;
    if (name.empty()) { r.error = "name required"; return r; }
    std::ostringstream sql;
    sql << "INSERT INTO wordbook (user_id, name, description) VALUES ("
        << RequestContext::currentUserId() << ", '"
        << Db::escape(name) << "', "
        << (description.empty() ? "NULL" : "'" + Db::escape(description) + "'") << ")";
    try {
      auto wr = db_.exec(sql.str());
      r.id = wr.last_insert_id;
      r.ok = true;
    } catch (const std::exception& e) {
      r.error = e.what();
    }
    return r;
  }

  // 详情（含词条列表）
  JsonValue detail(int64_t id) {
    auto rows = db_.query(
      "SELECT id, user_id, name, description, share_code, is_public, entry_count, "
      "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at "
      "FROM wordbook WHERE id=" + std::to_string(id)
      + " AND user_id=" + std::to_string(RequestContext::currentUserId())
    );
    if (rows.empty()) return JsonValue::makeNull();
    auto& r = rows[0];
    JsonObject o;
    o["id"] = JsonValue::makeInt(std::stoll(r["id"]));
    o["name"] = JsonValue::makeString(r["name"]);
    o["description"] = r["description"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["description"]);
    o["share_code"] = r["share_code"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["share_code"]);
    o["is_public"] = JsonValue::makeInt(std::stoi(r["is_public"]));
    o["entry_count"] = JsonValue::makeInt(std::stoi(r["entry_count"]));
    o["created_at"] = JsonValue::makeString(r["created_at"]);
    // 词条
    o["entries"] = entries(id, "");
    return JsonValue::makeObject(o);
  }

  // 更新
  bool update(int64_t id, const JsonValue& v, std::string& err) {
    // 检查归属
    auto chk = db_.query("SELECT id FROM wordbook WHERE id=" + std::to_string(id)
                         + " AND user_id=" + std::to_string(RequestContext::currentUserId()));
    if (chk.empty()) { err = "not found"; return false; }
    JsonArray sets;
    if (auto* name = v.find("name")) {
      if (!name->isString() || name->asString().empty()) { err = "name required"; return false; }
      sets.push_back(JsonValue::makeString("name='" + Db::escape(name->asString()) + "'"));
    }
    if (auto* desc = v.find("description")) {
      if (desc->isNull()) sets.push_back(JsonValue::makeString("description=NULL"));
      else sets.push_back(JsonValue::makeString("description='" + Db::escape(desc->asString()) + "'"));
    }
    if (auto* pub = v.find("is_public")) {
      sets.push_back(JsonValue::makeString("is_public=" + std::string(pub->asInt() ? "1" : "0")));
    }
    if (auto* sc = v.find("share_code")) {
      if (sc->isNull() || sc->asString().empty()) sets.push_back(JsonValue::makeString("share_code=NULL"));
      else sets.push_back(JsonValue::makeString("share_code='" + Db::escape(sc->asString()) + "'"));
    }
    if (sets.empty()) { err = "no fields to update"; return false; }
    std::ostringstream sql;
    sql << "UPDATE wordbook SET ";
    for (size_t i = 0; i < sets.size(); i++) { if (i) sql << ","; sql << sets[i].asString(); }
    sql << " WHERE id=" << id;
    try { db_.exec(sql.str()); return true; }
    catch (const std::exception& e) { err = e.what(); return false; }
  }

  // 删除（级联清理词条关联）
  bool remove(int64_t id, std::string& err) {
    auto chk = db_.query("SELECT id FROM wordbook WHERE id=" + std::to_string(id)
                         + " AND user_id=" + std::to_string(RequestContext::currentUserId()));
    if (chk.empty()) { err = "not found"; return false; }
    bool txOk = db_.transaction([&]() -> bool {
      db_.exec("DELETE FROM wordbook_entry WHERE wordbook_id=" + std::to_string(id));
      db_.exec("DELETE FROM wordbook WHERE id=" + std::to_string(id));
      return true;
    });
    if (!txOk) { err = "transaction failed"; return false; }
    return true;
  }

  // ============ 词条管理 ============

  // 词条列表
  JsonValue entries(int64_t wbId, const std::string& search="") {
    std::ostringstream sql;
    sql << "SELECT we.id AS entry_id, we.knowledge_id, we.display_order, "
           "kp.standard_content, kp.user_input_content, kp.difficulty, kp.master_require, kp.is_solidified, "
           "ul.proficiency_level "
           "FROM wordbook_entry we "
           "JOIN knowledge_points kp ON kp.id=we.knowledge_id "
           "LEFT JOIN user_learning ul ON ul.knowledge_id=we.knowledge_id AND ul.user_id="
           << RequestContext::currentUserId() << " "
           "WHERE we.wordbook_id=" << wbId;
    if (!search.empty()) {
      std::string esc = Db::escape(search);
      sql << " AND (kp.standard_content LIKE '%" << esc << "%' "
          << "OR kp.user_input_content LIKE '%" << esc << "%')";
    }
    sql << " ORDER BY we.display_order ASC, we.id ASC";
    auto rows = db_.query(sql.str());
    JsonArray arr;
    for (auto& r : rows) {
      JsonObject o;
      o["entry_id"] = JsonValue::makeInt(std::stoll(r["entry_id"]));
      o["knowledge_id"] = JsonValue::makeInt(std::stoll(r["knowledge_id"]));
      o["standard_content"] = r["standard_content"].empty()
        ? JsonValue::makeString(r["user_input_content"])
        : JsonValue::makeString(r["standard_content"]);
      o["difficulty"] = r["difficulty"].empty() ? JsonValue::makeNull() : JsonValue::makeInt(std::stoi(r["difficulty"]));
      o["master_require"] = r["master_require"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["master_require"]);
      o["is_solidified"] = JsonValue::makeInt(std::stoi(r["is_solidified"]));
      o["proficiency_level"] = r["proficiency_level"].empty() ? JsonValue::makeInt(0) : JsonValue::makeInt(std::stoi(r["proficiency_level"]));
      arr.push_back(JsonValue::makeObject(o));
    }
    return JsonValue::makeArray(arr);
  }

  // 添加词条（支持两种模式：knowledge_ids / new_words）
  struct AddResult { int added=0; int skipped=0; int failed=0; std::string error; };
  AddResult addEntries(int64_t wbId, const JsonValue& v) {
    AddResult r;
    auto chk = db_.query("SELECT id FROM wordbook WHERE id=" + std::to_string(wbId)
                         + " AND user_id=" + std::to_string(RequestContext::currentUserId()));
    if (chk.empty()) { r.error = "wordbook not found"; return r; }

    std::vector<int64_t> kpIds;

    // 模式1: knowledge_ids
    if (auto* ids = v.find("knowledge_ids")) {
      if (!ids->isArray()) { r.error = "knowledge_ids must be array"; return r; }
      for (auto& el : ids->asArray()) {
        int64_t kid = (int64_t)el.asInt();
        auto exists = db_.query("SELECT id FROM knowledge_points WHERE id=" + std::to_string(kid));
        if (!exists.empty()) kpIds.push_back(kid);
        else r.skipped++;
      }
    }

    // 模式2: new_words
    if (auto* words = v.find("new_words")) {
      if (!words->isArray()) { r.error = "new_words must be array"; return r; }
      for (auto& el : words->asArray()) {
        std::string word = el.isString() ? el.asString() : "";
        if (word.empty()) continue;
        auto cr = kpSvc_.create(word);
        if (cr.ok) kpIds.push_back(cr.id);
        else r.failed++;
      }
    }

    if (kpIds.empty()) { r.error = "no valid entries"; return r; }

    bool txOk = db_.transaction([&]() -> bool {
      int maxOrder = 0;
      auto ordRows = db_.query("SELECT COALESCE(MAX(display_order),0) AS mo FROM wordbook_entry WHERE wordbook_id=" + std::to_string(wbId));
      if (!ordRows.empty()) maxOrder = std::stoi(ordRows[0]["mo"]);

      for (auto kid : kpIds) {
        try {
          auto wr = db_.exec(
            "INSERT INTO wordbook_entry (wordbook_id, knowledge_id, display_order) VALUES ("
            + std::to_string(wbId) + "," + std::to_string(kid) + "," + std::to_string(maxOrder++) + ")"
          );
          if (wr.affected_rows > 0) r.added++;
          else r.skipped++; // 唯一键冲突 -> 跳过
        } catch (...) {
          r.skipped++;
        }
      }
      // 更新 entry_count 冗余字段
      db_.exec(
        "UPDATE wordbook SET entry_count=(SELECT COUNT(*) FROM wordbook_entry WHERE wordbook_id=" + std::to_string(wbId) + ") WHERE id=" + std::to_string(wbId)
      );
      return true;
    });
    if (!txOk) { r.error = "transaction failed"; }
    return r;
  }

  // 移除词条
  bool removeEntry(int64_t entryId, std::string& err) {
    auto chk = db_.query(
      "SELECT we.id FROM wordbook_entry we "
      "JOIN wordbook wb ON wb.id=we.wordbook_id "
      "WHERE we.id=" + std::to_string(entryId) + " AND wb.user_id="
      + std::to_string(RequestContext::currentUserId())
    );
    if (chk.empty()) { err = "not found"; return false; }
    bool txOk = db_.transaction([&]() -> bool {
      db_.exec("DELETE FROM wordbook_entry WHERE id=" + std::to_string(entryId));
      db_.exec(
        "UPDATE wordbook SET entry_count=(SELECT COUNT(*) FROM wordbook_entry WHERE wordbook_id=(SELECT wordbook_id FROM wordbook_entry WHERE id=" + std::to_string(entryId) + ")) WHERE id=(SELECT wordbook_id FROM wordbook_entry WHERE id=" + std::to_string(entryId) + ")"
      );
      return true;
    });
    // 上面的 UPDATE 在 DELETE 之后 wordbook_entry 已无该 entryId，改用直接更新
    if (txOk) {
      // 查出所属 wordbook_id 再更新
      // 注意：entry 已删除，这里需要之前查到的信息，但因为是单条操作，简化处理：更新全部 wordbook 的 entry_count
      auto allWb = db_.query("SELECT id FROM wordbook");
      for (auto& row : allWb) {
        int64_t wid = std::stoll(row["id"]);
        int cnt = 0;
        auto cntRows = db_.query("SELECT COUNT(*) AS c FROM wordbook_entry WHERE wordbook_id=" + std::to_string(wid));
        if (!cntRows.empty()) cnt = std::stoi(cntRows[0]["c"]);
        db_.exec("UPDATE wordbook SET entry_count=" + std::to_string(cnt) + " WHERE id=" + std::to_string(wid));
      }
    }
    return txOk;
  }

  // ============ 导出 ============

  // 生成分享识别码（6位大写字母+数字）
  static std::string genShareCode() {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::mt19937 rng((unsigned int)std::time(nullptr));
    std::uniform_int_distribution<int> dist(0, 35);
    std::string code;
    for (int i = 0; i < 6; i++) code += chars[dist(rng)];
    return code;
  }

  // 导出 JSON（完整知识点数据）
  std::string exportJson(int64_t wbId, std::string& err) {
    auto wb = db_.query("SELECT id, name, description, entry_count FROM wordbook WHERE id="
                        + std::to_string(wbId) + " AND user_id="
                        + std::to_string(RequestContext::currentUserId()));
    if (wb.empty()) { err = "wordbook not found"; return ""; }
    if (std::stoi(wb[0]["entry_count"]) > 1000) { err = "too many entries (max 1000)"; return ""; }

    // 获取知识点 ID 列表
    auto entryRows = db_.query(
      "SELECT knowledge_id FROM wordbook_entry WHERE wordbook_id=" + std::to_string(wbId)
      + " ORDER BY display_order ASC, id ASC LIMIT 1000"
    );

    // 构造导出 JSON
    JsonObject root;
    root["version"] = JsonValue::makeString("1.0");
    root["name"] = JsonValue::makeString(wb[0]["name"]);
    root["description"] = wb[0]["description"].empty() ? JsonValue::makeNull() : JsonValue::makeString(wb[0]["description"]);
    root["exported_at"] = JsonValue::makeString(""); // 将在写完前填入时间戳

    JsonArray entriesArr;
    for (auto& r : entryRows) {
      int64_t kpId = std::stoll(r["knowledge_id"]);
      auto detail = kpSvc_.detail(kpId);
      if (detail.isNull()) continue;
      entriesArr.push_back(detail);
    }
    root["entries"] = JsonValue::makeArray(entriesArr);
    root["exported_at"] = JsonValue::makeString(std::to_string(::time(nullptr)));

    // 生成分享识别码（如果没有）
    std::string shareCode;
    auto existing = db_.query("SELECT share_code FROM wordbook WHERE id=" + std::to_string(wbId));
    if (!existing.empty() && !existing[0]["share_code"].empty()) {
      shareCode = existing[0]["share_code"];
    } else {
      // 生成唯一识别码
      for (int tries = 0; tries < 10; tries++) {
        std::string code = genShareCode();
        auto dup = db_.query("SELECT id FROM wordbook WHERE share_code='" + Db::escape(code) + "'");
        if (dup.empty()) {
          db_.exec("UPDATE wordbook SET share_code='" + Db::escape(code) + "', is_public=1 WHERE id=" + std::to_string(wbId));
          shareCode = code;
          break;
        }
      }
    }
    root["share_code"] = JsonValue::makeString(shareCode);

    // 写文件
    std::string path = wordTmpDir_ + "/wordbook_" + std::to_string(wbId) + "_" + std::to_string(::time(nullptr)) + ".json";
    std::ofstream f(path);
    if (!f) { err = "cannot write json file"; return ""; }
    f << JsonValue::makeObject(root).dump();
    f.close();
    return path;
  }

  // 导出 Word
  std::string exportWord(int64_t wbId, std::string& err) {
    auto wb = db_.query("SELECT id, name, entry_count FROM wordbook WHERE id="
                        + std::to_string(wbId) + " AND user_id="
                        + std::to_string(RequestContext::currentUserId()));
    if (wb.empty()) { err = "wordbook not found"; return ""; }

    auto entryRows = db_.query(
      "SELECT we.knowledge_id FROM wordbook_entry we "
      "JOIN knowledge_points kp ON kp.id=we.knowledge_id "
      "WHERE we.wordbook_id=" + std::to_string(wbId) + " AND kp.is_solidified=1 "
      "ORDER BY we.display_order ASC, we.id ASC"
    );
    if (entryRows.empty()) { err = "no solidified entries"; return ""; }

    std::vector<int64_t> ids;
    for (auto& r : entryRows) ids.push_back(std::stoll(r["knowledge_id"]));

    // 复用 KnowledgeService 的批量导出
    return kpSvc_.exportBatchWord(ids);
  }

  // ============ 导入 ============

  struct ImportResult { int64_t wordbook_id=0; int imported=0; int merged=0; std::string error; };

  // 从 JSON 导入（完整知识点数据）
  ImportResult importFromJson(const JsonValue& root, const std::string& defaultName="") {
    ImportResult r;
    if (!root.isObject()) { r.error = "json must be object"; return r; }

    std::string name = root.isString() ? "" : "";
    if (auto* n = root.find("name")) name = n->asString();
    if (name.empty()) name = defaultName.empty() ? "导入单词本" : defaultName;

    std::string description;
    if (auto* d = root.find("description")) description = d->asString();

    auto* entries = root.find("entries");
    if (!entries || !entries->isArray()) { r.error = "entries array required"; return r; }

    // 限制导入数量
    auto& arr = entries->asArray();
    if (arr.size() > 500) { r.error = "too many entries (max 500)"; return r; }

    bool txOk = db_.transaction([&]() -> bool {
      // 创建单词本
      auto cr = create(name, description);
      if (!cr.ok) { r.error = "create wordbook failed: " + cr.error; return false; }
      r.wordbook_id = cr.id;

      int maxOrder = 0;
      for (auto& entry : arr) {
        if (!entry.isObject()) continue;
        int64_t kpId = 0;

        // 提取 standard_content
        std::string stdContent = entry.getString("standard_content");
        if (stdContent.empty()) stdContent = entry.getString("user_input_content");
        if (stdContent.empty()) continue;

        // 查重：本地已固化知识点存在相同 standard_content
        auto dup = db_.query(
          "SELECT id FROM knowledge_points WHERE standard_content='" + Db::escape(stdContent) + "' AND is_solidified=1 LIMIT 1"
        );
        if (!dup.empty()) {
          kpId = std::stoll(dup[0]["id"]);
          r.merged++;
        } else {
          // 写入新知识点（含子表）
          KnowledgeService::CreateResult cr2;
          cr2 = kpSvc_.create(stdContent);
          if (!cr2.ok) continue;
          kpId = cr2.id;
          // 直接写入固化数据（绕过 AI，使用导入的完整数据）
          std::string subErr;
          kpSvc_.applySolidify(kpId, stdContent, entry, subErr);
          r.imported++;
        }

        // 关联到单词本
        try {
          db_.exec(
            "INSERT INTO wordbook_entry (wordbook_id, knowledge_id, display_order) VALUES ("
            + std::to_string(r.wordbook_id) + "," + std::to_string(kpId) + "," + std::to_string(maxOrder++) + ")"
          );
        } catch (...) {
          // 已存在则跳过
        }
      }

      // 更新 entry_count
      db_.exec(
        "UPDATE wordbook SET entry_count=(SELECT COUNT(*) FROM wordbook_entry WHERE wordbook_id="
        + std::to_string(r.wordbook_id) + ") WHERE id=" + std::to_string(r.wordbook_id)
      );
      return true;
    });

    if (!txOk && r.wordbook_id > 0) {
      // 回滚单词本（级联清理词条）
      db_.exec("DELETE FROM wordbook_entry WHERE wordbook_id=" + std::to_string(r.wordbook_id));
      db_.exec("DELETE FROM wordbook WHERE id=" + std::to_string(r.wordbook_id));
      r.wordbook_id = 0;
    }
    return r;
  }

  // 从 JSON 文件路径导入
  ImportResult importFromFilePath(const std::string& path) {
    std::ifstream f(path);
    if (!f) { ImportResult r; r.error = "cannot open file"; return r; }
    std::ostringstream ss; ss << f.rdbuf();
    JsonValue v;
    try { v = parseJson(ss.str()); }
    catch (const std::exception& e) { ImportResult r; r.error = std::string("json parse failed: ") + e.what(); return r; }
    return importFromJson(v);
  }
};

} // namespace eng
