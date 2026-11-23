// v1.5.0-T003 WORDLISTS：两层词表索引（册 → 单元 → 单词）+ 用户词表
// 职责：内置词表 seed、词表 CRUD、词条管理、范围包合并、我的单词查询、全局搜索
// 设计：词条自带 word/meaning/pos；knowledge_id 仅作可选关联，不复制知识点内容
#pragma once
#include "db.h"
#include "json.h"
#include "auth.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace eng {

class WordlistService {
public:
  struct ItemInput {
    std::string word;
    std::string meaning;
    std::string pos;
  };

  struct CreateResult {
    int64_t id = 0;
    int added = 0;
    int skipped = 0;
    bool existing = false;
    bool ok = false;
    std::string error;
  };

  struct AddResult {
    int added = 0;
    int skipped = 0;
    int64_t copied_id = 0;          // 共用词表自动转私有时的新本 id
    std::string copied_title;       // 新本标题
    bool ok = false;
    std::string error;
  };

  struct SeedResult {
    int lists = 0;
    int items = 0;
    std::string error;
  };

  // v1.5.0-T005：组合成员指针（list=单词本/组合，word=单个词条）
  struct MemberInput {
    std::string type;
    int64_t id = 0;
  };

  struct ExpandWord {
    int64_t item_id = 0;
    std::string word;
    std::string meaning;
    std::string pos;
    int64_t knowledge_id = 0;
  };

  struct ExpandResult {
    std::vector<ExpandWord> words;
    std::string error;
    bool ok = false;
  };

  explicit WordlistService(Db& db) : db_(db) {}

  // ============ 内置词表 seed ============

  SeedResult seedBuiltin(const std::string& tsvPath) {
    SeedResult result;
    std::ifstream f(tsvPath);
    if (!f) {
      result.error = "seed file not found: " + tsvPath;
      return result;
    }

    struct Group {
      std::string textbook, volume, unit;
      std::vector<ItemInput> items;
    };
    std::vector<Group> groups;
    std::map<std::string, size_t> index;
    std::string line;

    std::getline(f, line); // header
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      auto cols = splitTab(line);
      if (cols.size() < 5) continue;
      std::string key = cols[0] + "\x1f" + cols[1] + "\x1f" + cols[2];
      auto it = index.find(key);
      if (it == index.end()) {
        Group g;
        g.textbook = cols[0];
        g.volume = cols[1];
        g.unit = cols[2];
        index[key] = groups.size();
        groups.push_back(g);
        it = index.find(key);
      }
      ItemInput item;
      item.word = cols[4];
      groups[it->second].items.push_back(item);
    }

    for (auto& g : groups) {
      std::string title = g.volume + " " + g.unit;
      int sortNo = sortNoFor(g.volume, g.unit);
      bool ok = db_.transaction([&]() -> bool {
        std::ostringstream sql;
        sql << "INSERT INTO wordlists (user_id,title,source,textbook,volume,unit,item_count,sort_no) VALUES (1,'"
            << Db::escape(title) << "','builtin','" << Db::escape(g.textbook) << "','"
            << Db::escape(g.volume) << "','" << Db::escape(g.unit) << "',"
            << (int)g.items.size() << "," << sortNo << ") "
            << "ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id), title=VALUES(title), "
            << "textbook=VALUES(textbook), volume=VALUES(volume), unit=VALUES(unit), "
            << "item_count=VALUES(item_count), sort_no=VALUES(sort_no)";
        auto wr = db_.exec(sql.str());
        int64_t listId = wr.last_insert_id;
        if (listId <= 0) return false;

        // 分批 upsert，避免单条 SQL 过长
        for (size_t start = 0; start < g.items.size(); start += 100) {
          size_t end = std::min(g.items.size(), start + 100);
          std::ostringstream ins;
          ins << "INSERT INTO wordlist_items (wordlist_id,word,normalized_word,order_no) VALUES ";
          for (size_t i = start; i < end; ++i) {
            if (i > start) ins << ",";
            std::string norm = normalizeWord(g.items[i].word);
            ins << "(" << listId << ",'" << Db::escape(g.items[i].word) << "','"
                << Db::escape(norm) << "'," << (int)(i + 1) << ")";
          }
          ins << " ON DUPLICATE KEY UPDATE word=VALUES(word), normalized_word=VALUES(normalized_word), "
              << "order_no=VALUES(order_no)";
          db_.exec(ins.str());
        }

        // 文件删词后清理陈旧词条（只清内置词表，不影响 knowledge_id 关联的其它词表）
        std::ostringstream del;
        del << "DELETE FROM wordlist_items WHERE wordlist_id=" << listId << " AND normalized_word NOT IN (";
        for (size_t i = 0; i < g.items.size(); ++i) {
          if (i) del << ",";
          del << "'" << Db::escape(normalizeWord(g.items[i].word)) << "'";
        }
        del << ")";
        if (!g.items.empty()) db_.exec(del.str());

        db_.exec("UPDATE wordlists SET item_count=(SELECT COUNT(*) FROM wordlist_items WHERE wordlist_id="
                 + std::to_string(listId) + ") WHERE id=" + std::to_string(listId));
        return true;
      });
      if (!ok) {
        result.error = "seed failed: " + title;
        continue;
      }
      result.lists++;
      result.items += (int)g.items.size();
    }
    return result;
  }

  // ============ 只读接口 ============

  JsonValue list(const std::string& source) {
    int uid = RequestContext::currentUserId();
    if (uid > 0) recountDirty(uid);
    std::ostringstream sql;
    sql << "SELECT " << listColumns() << "FROM wordlists WHERE ";
    if (source == "builtin") {
      sql << "source='builtin' ";
    } else if (source == "range") {
      if (uid <= 0) return JsonValue::makeObject({{"data", JsonValue::makeArray({})}});
      sql << "user_id=" << uid << " AND kind='combo' ";
    } else if (source == "user") {
      if (uid <= 0) return JsonValue::makeObject({{"data", JsonValue::makeArray({})}});
      sql << "user_id=" << uid << " AND source='user' AND kind='book' ";
    } else {
      sql << "(source='builtin' OR (source='user' AND user_id=" << uid << ")) ";
    }
    sql << "ORDER BY CASE WHEN source='builtin' THEN 0 ELSE 1 END, sort_no ASC, id DESC";
    auto rows = db_.query(sql.str());
    JsonArray arr;
    for (auto& r : rows) arr.push_back(mapList(r));
    return JsonValue::makeObject({{"data", JsonValue::makeArray(arr)}});
  }

  JsonValue detail(int64_t id) {
    auto rows = db_.query("SELECT " + listColumns() + "FROM wordlists WHERE id=" + std::to_string(id)
                          + readableWhere());
    if (rows.empty()) return JsonValue::makeNull();
    if (field(rows[0], "count_dirty", "0") == "1" && recount(id)) {
      rows = db_.query("SELECT " + listColumns() + "FROM wordlists WHERE id=" + std::to_string(id)
                       + readableWhere());
      if (rows.empty()) return JsonValue::makeNull();
    }
    JsonValue v = mapList(rows[0]);
    // 普通单词本也可承载子成员（整本加入后形成层级），因此只要有成员就返回
    JsonValue kids = children(id);
    bool hasKids = kids.isArray() && !kids.asArray().empty();
    if (field(rows[0], "kind", "book") == "combo" || hasKids) {
      JsonObject o = v.asObject();
      o["children"] = kids;
      return JsonValue::makeObject(o);
    }
    return v;
  }

  JsonValue items(int64_t id, int page, int size, const std::string& search) {
    if (!readable(id)) return JsonValue::makeNull();
    if (page < 1) page = 1;
    if (size < 1) size = 20;
    if (size > 200) size = 200;

    std::ostringstream where;
    where << " WHERE wi.wordlist_id=" << id;
    if (!search.empty()) {
      where << " AND wi.normalized_word LIKE '%" << Db::escape(normalizeWord(search)) << "%'";
    }

    auto countRows = db_.query("SELECT COUNT(*) AS c FROM wordlist_items wi" + where.str());
    int total = countRows.empty() ? 0 : std::stoi(countRows[0]["c"]);

    std::ostringstream sql;
    sql << "SELECT wi.id,wi.word,wi.meaning,wi.pos,wi.order_no,wi.knowledge_id,wi.is_solidified,"
           "kp.standard_content,ul.proficiency_level,"
           "IFNULL(kp.id,0) AS own_kp_id,IFNULL(kp.is_solidified,0) AS own_solidified,"
           "(SELECT d.meaning FROM kp_definitions d WHERE d.knowledge_id=kp.id ORDER BY d.id LIMIT 1) AS kp_meaning "
           "FROM wordlist_items wi "
           "LEFT JOIN knowledge_points kp ON kp.user_id="
        << RequestContext::currentUserId()
        << " AND kp.normalized_key=wi.normalized_word "
           "LEFT JOIN user_learning ul ON ul.knowledge_id=kp.id AND ul.user_id="
        << RequestContext::currentUserId()
        << where.str()
        << " ORDER BY wi.order_no ASC, wi.id ASC LIMIT " << size << " OFFSET " << (page - 1) * size;

    auto rows = db_.query(sql.str());
    JsonArray arr;
    for (auto& r : rows) {
      JsonObject o;
      o["item_id"] = JsonValue::makeInt((int)std::stoll(r["id"]));
      o["word"] = JsonValue::makeString(r["word"]);
      std::string meaning = r["meaning"];
      if (meaning.empty()) meaning = r["kp_meaning"];
      o["meaning"] = meaning.empty() ? JsonValue::makeNull() : JsonValue::makeString(meaning);
      o["pos"] = r["pos"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["pos"]);
      o["order_no"] = JsonValue::makeInt(std::stoi(r["order_no"]));
      std::string own = field(r, "own_kp_id", "0");
      o["knowledge_id"] = (own.empty() || own == "0")
          ? JsonValue::makeNull() : JsonValue::makeInt(std::stoi(own));
      o["is_solidified"] = JsonValue::makeInt(std::stoi(field(r, "own_solidified", "0")));
      o["proficiency_level"] = JsonValue::makeInt(r["proficiency_level"].empty() ? 0 : std::stoi(r["proficiency_level"]));
      arr.push_back(JsonValue::makeObject(o));
    }

    JsonObject res;
    res["data"] = JsonValue::makeArray(arr);
    res["total"] = JsonValue::makeInt(total);
    res["page"] = JsonValue::makeInt(page);
    res["size"] = JsonValue::makeInt(size);
    return JsonValue::makeObject(res);
  }

  JsonValue search(const std::string& q, int limit) {
    JsonArray arr;
    std::string key = normalizeWord(q);
    if (key.empty()) return JsonValue::makeObject({{"data", JsonValue::makeArray(arr)}});
    if (limit < 1) limit = 20;
    if (limit > 100) limit = 100;

    std::ostringstream sql;
    sql << "SELECT wi.word,wi.meaning,wi.pos,wl.id AS wordlist_id,wl.title,wl.textbook,wl.volume,wl.unit "
           "FROM wordlist_items wi JOIN wordlists wl ON wl.id=wi.wordlist_id "
           "WHERE wi.normalized_word LIKE '%" << Db::escape(key) << "%' "
        << readableWhere("wl")
        << " ORDER BY CASE WHEN wl.source='builtin' THEN 0 ELSE 1 END, wl.sort_no ASC, wi.order_no ASC LIMIT "
        << limit;

    auto rows = db_.query(sql.str());
    for (auto& r : rows) {
      JsonObject o;
      o["wordlist_id"] = JsonValue::makeInt((int)std::stoll(r["wordlist_id"]));
      o["title"] = JsonValue::makeString(r["title"]);
      o["textbook"] = r["textbook"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["textbook"]);
      o["volume"] = r["volume"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["volume"]);
      o["unit"] = r["unit"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["unit"]);
      o["word"] = JsonValue::makeString(r["word"]);
      o["meaning"] = r["meaning"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["meaning"]);
      o["pos"] = r["pos"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["pos"]);
      arr.push_back(JsonValue::makeObject(o));
    }
    return JsonValue::makeObject({{"data", JsonValue::makeArray(arr)}});
  }

  // 我的单词：原有 knowledge_points 存储，排除已被本人任一单词本引用者
  JsonValue myWords(const std::string& sort, const std::string& order,
                    const std::string& q, int page, int size) {
    int uid = RequestContext::currentUserId();
    if (uid <= 0) {
      JsonObject empty;
      empty["data"] = JsonValue::makeArray({});
      empty["total"] = JsonValue::makeInt(0);
      return JsonValue::makeObject(empty);
    }
    if (page < 1) page = 1;
    if (size < 1) size = 20;
    if (size > 200) size = 200;

    std::ostringstream where;
    where << " WHERE kp.user_id=" << uid
          << " AND NOT EXISTS (SELECT 1 FROM wordbook_entry we JOIN wordbook wb ON wb.id=we.wordbook_id "
             "WHERE we.knowledge_id=kp.id AND wb.user_id=" << uid << ")";
    if (!q.empty()) {
      where << " AND (kp.standard_content LIKE '%" << Db::escape(q) << "%' OR kp.user_input_content LIKE '%"
            << Db::escape(q) << "%')";
    }

    auto countRows = db_.query("SELECT COUNT(*) AS c FROM knowledge_points kp" + where.str());
    int total = countRows.empty() ? 0 : std::stoi(countRows[0]["c"]);

    std::string dir = (order == "asc") ? "ASC" : "DESC";
    std::string orderBy;
    if (sort == "alpha") orderBy = "COALESCE(NULLIF(kp.standard_content,''),kp.user_input_content) " + dir + ", kp.create_time DESC";
    else if (sort == "mastery") orderBy = "proficiency_level " + dir + ", kp.create_time DESC";
    else orderBy = "kp.create_time " + dir;

    std::ostringstream sql;
    sql << "SELECT kp.id,kp.standard_content,kp.user_input_content,kp.is_solidified,"
           "DATE_FORMAT(kp.create_time,'%Y-%m-%d %H:%i:%s') AS create_time,"
           "IFNULL(ul.proficiency_level,0) AS proficiency_level,"
           "(SELECT d.meaning FROM kp_definitions d WHERE d.knowledge_id=kp.id ORDER BY d.id LIMIT 1) AS meaning "
           "FROM knowledge_points kp "
           "LEFT JOIN user_learning ul ON ul.knowledge_id=kp.id AND ul.user_id=" << uid
        << where.str()
        << " ORDER BY " << orderBy << " LIMIT " << size << " OFFSET " << (page - 1) * size;

    auto rows = db_.query(sql.str());
    JsonArray arr;
    for (auto& r : rows) {
      std::string word = r["standard_content"].empty() ? r["user_input_content"] : r["standard_content"];
      JsonObject o;
      o["knowledge_id"] = JsonValue::makeInt((int)std::stoll(r["id"]));
      o["word"] = JsonValue::makeString(word);
      o["meaning"] = r["meaning"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["meaning"]);
      o["proficiency_level"] = JsonValue::makeInt(std::stoi(r["proficiency_level"]));
      o["is_solidified"] = JsonValue::makeInt(std::stoi(r["is_solidified"]));
      o["create_time"] = JsonValue::makeString(r["create_time"]);
      arr.push_back(JsonValue::makeObject(o));
    }

    JsonObject res;
    res["data"] = JsonValue::makeArray(arr);
    res["total"] = JsonValue::makeInt(total);
    res["page"] = JsonValue::makeInt(page);
    res["size"] = JsonValue::makeInt(size);
    return JsonValue::makeObject(res);
  }

  std::vector<int64_t> myWordIds() {
    std::vector<int64_t> ids;
    int uid = RequestContext::currentUserId();
    if (uid <= 0) return ids;
    auto rows = db_.query(
      "SELECT kp.id FROM knowledge_points kp WHERE kp.user_id=" + std::to_string(uid)
      + " AND NOT EXISTS (SELECT 1 FROM wordbook_entry we JOIN wordbook wb ON wb.id=we.wordbook_id "
        "WHERE we.knowledge_id=kp.id AND wb.user_id=" + std::to_string(uid) + ") ORDER BY kp.id");
    for (auto& r : rows) ids.push_back(std::stoll(r["id"]));
    return ids;
  }

  // ============ 写接口 ============

  CreateResult create(const std::string& title, const std::string& description, const JsonValue& v) {
    CreateResult r;
    std::string cleanTitle = trim(title);
    if (cleanTitle.empty()) { r.error = "title required"; return r; }
    if (cleanTitle.size() > 255) { r.error = "title too long"; return r; }

    std::vector<ItemInput> items;
    collectItems(v, items);
    if (items.size() > 2000) { r.error = "too many words (max 2000)"; return r; }
    dedupe(items);

    int uid = RequestContext::currentUserId();
    if (uid <= 0) { r.error = "login required"; return r; }
    return createInternal(uid, cleanTitle, description, "user", "", items, 0);
  }

  bool update(int64_t id, const JsonValue& v, std::string& err) {
    if (!ownedUserList(id)) { err = "not found"; return false; }
    JsonArray sets;
    if (auto* title = v.find("title")) {
      std::string t = trim(title->isString() ? title->asString() : "");
      if (t.empty()) { err = "title required"; return false; }
      sets.push_back(JsonValue::makeString("title='" + Db::escape(t) + "'"));
    }
    if (auto* desc = v.find("description")) {
      if (desc->isNull()) sets.push_back(JsonValue::makeString("description=NULL"));
      else sets.push_back(JsonValue::makeString("description='" + Db::escape(desc->asString()) + "'"));
    }
    if (sets.empty()) { err = "no fields to update"; return false; }
    std::ostringstream sql;
    sql << "UPDATE wordlists SET ";
    for (size_t i = 0; i < sets.size(); ++i) { if (i) sql << ","; sql << sets[i].asString(); }
    sql << " WHERE id=" << id;
    try { db_.exec(sql.str()); return true; }
    catch (const std::exception& e) { err = e.what(); return false; }
  }

  bool remove(int64_t id, std::string& err) {
    if (!ownedUserList(id)) { err = "not found"; return false; }
    // 先记录引用本条目的组合，删除后重算它们的词数，避免悬空指针
    auto parentRows = db_.query(
      "SELECT DISTINCT parent_id FROM wordlist_children WHERE "
      "(child_type='list' AND child_id=" + std::to_string(id) + ") OR "
      "(child_type='word' AND child_id IN (SELECT id FROM wordlist_items WHERE wordlist_id="
      + std::to_string(id) + "))");
    bool ok = db_.transaction([&]() -> bool {
      db_.exec("DELETE FROM wordlist_children WHERE "
               "(child_type='list' AND child_id=" + std::to_string(id) + ") OR "
               "(child_type='word' AND child_id IN (SELECT id FROM wordlist_items WHERE wordlist_id="
               + std::to_string(id) + "))");
      // 本单词本作为父级持有的成员行一并清理，避免悬空
      db_.exec("DELETE FROM wordlist_children WHERE parent_id=" + std::to_string(id));
      db_.exec("DELETE FROM wordlist_items WHERE wordlist_id=" + std::to_string(id));
      db_.exec("DELETE FROM wordlists WHERE id=" + std::to_string(id));
      for (auto& r : parentRows) {
        int64_t pid = std::stoll(r["parent_id"]);
        markAncestorsDirty(pid);
        recount(pid);
      }
      return true;
    });
    if (!ok) { err = "transaction failed"; return false; }
    return true;
  }

  AddResult addItems(int64_t id, const JsonValue& v) {
    AddResult r;
    if (!ownedUserList(id)) { r.error = "not found"; return r; }
    std::vector<ItemInput> items;
    collectItems(v, items);
    if (items.size() > 2000) { r.error = "too many words (max 2000)"; return r; }
    dedupe(items);
    if (items.empty()) { r.error = "no valid entries"; return r; }

    bool ok = db_.transaction([&]() -> bool {
      int maxOrder = 0;
      auto ord = db_.query("SELECT COALESCE(MAX(order_no),0) AS mo FROM wordlist_items WHERE wordlist_id=" + std::to_string(id));
      if (!ord.empty()) maxOrder = std::stoi(ord[0]["mo"]);
      for (auto& item : items) {
        try {
          auto wr = db_.exec(
            "INSERT IGNORE INTO wordlist_items (wordlist_id,word,normalized_word,meaning,pos,order_no) VALUES ("
            + std::to_string(id) + ",'" + Db::escape(item.word) + "','" + Db::escape(normalizeWord(item.word)) + "',"
            + (item.meaning.empty() ? "NULL" : "'" + Db::escape(item.meaning) + "'") + ","
            + (item.pos.empty() ? "NULL" : "'" + Db::escape(item.pos) + "'") + ","
            + std::to_string(++maxOrder) + ")");
          if (wr.affected_rows > 0) r.added++; else r.skipped++;
        } catch (...) { r.skipped++; }
      }
      recount(id);
      markAncestorsDirty(id);
      return true;
    });
    if (!ok) { r.error = "transaction failed"; return r; }
    r.ok = true;
    return r;
  }

  // 整本加入：把 sourceId 整本作为层级成员挂到 targetId 下（只存指针，不复制词条）
  // 普通单词本与合并本都可承载成员；重复加入由 uk_child 幂等去重
  // 共用/社区词表不能直接挂成成员 → 先按「复制到自建词表」转成私有本，再挂该私有本
  AddResult importWhole(int64_t sourceId, int64_t targetId) {
    AddResult r;
    if (sourceId <= 0 || targetId <= 0 || sourceId == targetId) { r.error = "不能加入自身"; return r; }
    if (!readable(sourceId)) { r.error = "来源单词本不存在"; return r; }
    if (!ownedUserList(targetId)) { r.error = "目标单词本不存在"; return r; }

    ExpandResult ex = expand(sourceId);
    if (!ex.ok) { r.error = ex.error; return r; }

    int64_t memberId = sourceId;
    auto srcRows = db_.query("SELECT source,title FROM wordlists WHERE id=" + std::to_string(sourceId));
    if (!srcRows.empty() && srcRows[0]["source"] != "user") {
      // 同一本共用词表只保留一份私有副本：已复制过就直接复用
      std::string scopeKey = "copy_of:" + std::to_string(sourceId);
      int uid = RequestContext::currentUserId();
      auto exRows = db_.query("SELECT id FROM wordlists WHERE user_id=" + std::to_string(uid)
                              + " AND source='user' AND scope_key='" + Db::escape(scopeKey) + "'");
      if (!exRows.empty()) {
        memberId = std::stoll(exRows[0]["id"]);
      } else {
        CreateResult cp = copy(sourceId);
        if (!cp.ok || cp.id <= 0) { r.error = cp.error.empty() ? "复制到自建词表失败" : cp.error; return r; }
        db_.exec("UPDATE wordlists SET scope_key='" + Db::escape(scopeKey) + "' WHERE id="
                 + std::to_string(cp.id));
        memberId = cp.id;
        r.copied_id = cp.id;
        r.copied_title = srcRows[0]["title"] + "（副本）";
      }
    }

    std::string err;
    std::vector<MemberInput> members;
    MemberInput m;
    m.type = "list";
    m.id = memberId;
    members.push_back(m);
    bool ok = db_.transaction([&]() -> bool { return addMembers(targetId, members, err); });
    if (!ok) { r.error = err.empty() ? "transaction failed" : err; return r; }
    r.added = (int)ex.words.size();
    r.skipped = 0;
    r.ok = true;
    return r;
  }

  bool removeItem(int64_t itemId, std::string& err) {
    auto rows = db_.query(
      "SELECT wi.id,wi.wordlist_id FROM wordlist_items wi JOIN wordlists wl ON wl.id=wi.wordlist_id "
      "WHERE wi.id=" + std::to_string(itemId) + " AND wl.user_id="
      + std::to_string(RequestContext::currentUserId()) + " AND wl.source='user'");
    if (rows.empty()) { err = "not found"; return false; }
    int64_t listId = std::stoll(rows[0]["wordlist_id"]);
    auto parentRows = db_.query(
      "SELECT DISTINCT parent_id FROM wordlist_children WHERE child_type='word' AND child_id="
      + std::to_string(itemId));
    bool ok = db_.transaction([&]() -> bool {
      db_.exec("DELETE FROM wordlist_children WHERE child_type='word' AND child_id="
               + std::to_string(itemId));
      db_.exec("DELETE FROM wordlist_items WHERE id=" + std::to_string(itemId));
      recount(listId);
      for (auto& r : parentRows) {
        int64_t pid = std::stoll(r["parent_id"]);
        markAncestorsDirty(pid);
        recount(pid);
      }
      return true;
    });
    if (!ok) { err = "transaction failed"; return false; }
    return true;
  }

  CreateResult copy(int64_t id) {
    CreateResult r;
    auto rows = db_.query(
      "SELECT id,title FROM wordlists WHERE id=" + std::to_string(id) + readableWhere());
    if (rows.empty()) { r.error = "not found"; return r; }
    std::string title = rows[0]["title"] + "（副本）";
    std::vector<ItemInput> items;
    auto itemRows = db_.query(
      "SELECT word,meaning,pos FROM wordlist_items WHERE wordlist_id=" + std::to_string(id)
      + " ORDER BY order_no ASC, id ASC");
    for (auto& row : itemRows) {
      ItemInput item;
      item.word = row["word"];
      item.meaning = row["meaning"];
      item.pos = row["pos"];
      items.push_back(item);
    }
    int uid = RequestContext::currentUserId();
    if (uid <= 0) { r.error = "login required"; return r; }
    return createInternal(uid, title, "", "user", "", items, 0);
  }

  CreateResult rangePackage(const JsonValue& v) {
    CreateResult r;
    int uid = RequestContext::currentUserId();
    if (uid <= 0) { r.error = "login required"; return r; }

    std::vector<int64_t> listIds;
    std::vector<std::string> unitKeys;
    auto* units = v.find("units");
    if (units && units->isArray()) {
      for (auto& u : units->asArray()) {
        if (!u.isObject()) continue;
        std::string textbook = u.getString("textbook", "人教版（2019）");
        std::string volume = u.getString("volume", "");
        std::string unit = u.getString("unit", "");
        if (volume.empty() || unit.empty()) continue;
        auto rows = db_.query(
          "SELECT id FROM wordlists WHERE source='builtin' AND textbook='" + Db::escape(textbook)
          + "' AND volume='" + Db::escape(volume) + "' AND unit='" + Db::escape(unit) + "' LIMIT 1");
        if (!rows.empty()) {
          listIds.push_back(std::stoll(rows[0]["id"]));
          unitKeys.push_back(volume + "/" + unit);
        }
      }
    }
    auto* ids = v.find("wordlist_ids");
    if (ids && ids->isArray()) {
      for (auto& el : ids->asArray()) {
        int64_t lid = (int64_t)el.asInt();
        if (lid <= 0) continue;
        auto rows = db_.query("SELECT id FROM wordlists WHERE id=" + std::to_string(lid) + readableWhere());
        if (!rows.empty()) listIds.push_back(lid);
      }
    }
    if (listIds.empty()) { r.error = "no units selected"; return r; }

    std::sort(unitKeys.begin(), unitKeys.end());
    std::sort(listIds.begin(), listIds.end());
    listIds.erase(std::unique(listIds.begin(), listIds.end()), listIds.end());

    std::ostringstream scope;
    if (!unitKeys.empty()) {
      scope << "units:";
      for (size_t i = 0; i < unitKeys.size(); ++i) { if (i) scope << "|"; scope << unitKeys[i]; }
    } else {
      scope << "ids:";
      for (size_t i = 0; i < listIds.size(); ++i) { if (i) scope << "|"; scope << listIds[i]; }
    }

    auto existing = db_.query(
      "SELECT id,item_count FROM wordlists WHERE user_id=" + std::to_string(uid)
      + " AND scope_key='" + Db::escape(scope.str()) + "' LIMIT 1");
    if (!existing.empty()) {
      r.id = std::stoll(existing[0]["id"]);
      r.added = std::stoi(existing[0]["item_count"]);
      r.existing = true;
      r.ok = true;
      return r;
    }

    std::vector<ItemInput> items;
    std::ostringstream in;
    for (size_t i = 0; i < listIds.size(); ++i) { if (i) in << ","; in << listIds[i]; }
    auto rows = db_.query(
      "SELECT word,meaning,pos FROM wordlist_items WHERE wordlist_id IN (" + in.str()
      + ") ORDER BY FIELD(wordlist_id," + in.str() + "), order_no ASC, id ASC");
    for (auto& row : rows) {
      ItemInput item;
      item.word = row["word"];
      item.meaning = row["meaning"];
      item.pos = row["pos"];
      items.push_back(item);
    }
    dedupe(items);

    if (!unitKeys.empty() && unitKeys.size() <= 2) {
      std::string t = unitKeys.size() == 1 ? unitKeys[0] : unitKeys.front() + " 等 " + std::to_string(unitKeys.size()) + " 个单元";
      std::replace(t.begin(), t.end(), '/', ' ');
      r = createInternal(uid, t, "跨单元合并", "user", scope.str(), items, 0, "combo");
    } else {
      std::string title = trim(v.getString("title", ""));
      if (title.empty()) title = "范围包 · " + std::to_string(unitKeys.empty() ? listIds.size() : unitKeys.size()) + " 个单元";
      r = createInternal(uid, title, "跨单元合并", "user", scope.str(), items, 0, "combo");
    }
    // 新范围包同时写入成员指针：兼容旧词条展示，又可按组合递归展开
    if (r.ok && !r.existing && r.id > 0) {
      std::vector<MemberInput> members;
      for (auto lid : listIds) members.push_back({"list", lid});
      std::string err;
      if (!addMembers(r.id, members, err)) { r.ok = false; r.error = err; }
    }
    return r;
  }

  // ============ v1.5.0-T005：组合成员与递归展开 ============

  // 组合成员列表；非组合返回空数组，不可读返回 null
  JsonValue children(int64_t id) {
    if (!readable(id)) return JsonValue::makeNull();
    auto rows = db_.query(
      "SELECT c.id,c.child_type,c.child_id,c.order_no,"
      "CASE WHEN c.child_type='list' THEN wl.title ELSE wi.word END AS title,"
      "IFNULL(wl.kind,'') AS kind,IFNULL(wl.source,'') AS source,"
      "IFNULL(wl.item_count,0) AS item_count,"
      "IFNULL(wi.meaning,'') AS meaning,IFNULL(wi.pos,'') AS pos "
      "FROM wordlist_children c "
      "LEFT JOIN wordlists wl ON c.child_type='list' AND wl.id=c.child_id "
      "LEFT JOIN wordlist_items wi ON c.child_type='word' AND wi.id=c.child_id "
      "WHERE c.parent_id=" + std::to_string(id) + " ORDER BY c.order_no ASC, c.id ASC");
    JsonArray arr;
    for (auto& r : rows) {
      JsonObject o;
      o["member_id"] = JsonValue::makeInt((int)std::stoll(r["id"]));
      o["child_type"] = JsonValue::makeString(r["child_type"]);
      o["child_id"] = JsonValue::makeInt((int)std::stoll(r["child_id"]));
      o["order_no"] = JsonValue::makeInt(std::stoi(r["order_no"]));
      o["title"] = r["title"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["title"]);
      o["kind"] = r["kind"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["kind"]);
      o["source"] = r["source"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["source"]);
      o["item_count"] = JsonValue::makeInt(std::stoi(r["item_count"]));
      o["meaning"] = r["meaning"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["meaning"]);
      o["pos"] = r["pos"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["pos"]);
      arr.push_back(JsonValue::makeObject(o));
    }
    return JsonValue::makeArray(arr);
  }

  // 追加组合成员：{items:[{type:'list'|'word', id}]}
  AddResult addChildren(int64_t id, const JsonValue& v) {
    AddResult r;
    if (!ownedUserList(id)) { r.error = "not found"; return r; }
    std::vector<MemberInput> members;
    collectMembers(v.find("items"), members);
    if (members.empty()) { r.error = "no valid members"; return r; }
    std::string err;
    bool ok = db_.transaction([&]() -> bool { return addMembers(id, members, err); });
    if (!ok) { r.error = err.empty() ? "transaction failed" : err; return r; }
    r.added = (int)members.size();
    r.ok = true;
    return r;
  }

  // 移除组合成员（memberId = 成员行 id）
  bool removeChild(int64_t memberId, std::string& err) {
    int uid = RequestContext::currentUserId();
    auto rows = db_.query(
      "SELECT c.id,c.parent_id FROM wordlist_children c JOIN wordlists wl ON wl.id=c.parent_id "
      "WHERE c.id=" + std::to_string(memberId) + " AND wl.user_id=" + std::to_string(uid)
      + " AND wl.source='user'");
    if (rows.empty()) { err = "not found"; return false; }
    int64_t parentId = std::stoll(rows[0]["parent_id"]);
    bool ok = db_.transaction([&]() -> bool {
      db_.exec("DELETE FROM wordlist_children WHERE id=" + std::to_string(memberId));
      markAncestorsDirty(parentId);
      recount(parentId);
      return true;
    });
    if (!ok) { err = "transaction failed"; return false; }
    return true;
  }

  // 调整成员顺序：{ids:[member_id...]}
  bool reorderChildren(int64_t id, const JsonValue& v, std::string& err) {
    if (!ownedUserList(id)) { err = "not found"; return false; }
    auto* ids = v.find("ids");
    if (!ids || !ids->isArray() || ids->asArray().empty()) { err = "no members"; return false; }
    bool ok = db_.transaction([&]() -> bool {
      int order = 0;
      for (auto& el : ids->asArray()) {
        int64_t mid = el.asInt();
        if (mid <= 0) continue;
        db_.exec("UPDATE wordlist_children SET order_no=" + std::to_string(++order)
                 + " WHERE id=" + std::to_string(mid) + " AND parent_id=" + std::to_string(id));
      }
      markAncestorsDirty(id);
      recount(id);
      return true;
    });
    if (!ok) { err = "transaction failed"; return false; }
    return true;
  }

  // 新建组合：{title, description, members:[{type,id}]}；只存成员指针，不复制词条
  CreateResult compose(const JsonValue& v) {
    CreateResult r;
    int uid = RequestContext::currentUserId();
    if (uid <= 0) { r.error = "login required"; return r; }
    std::string title = trim(v.getString("title", ""));
    if (title.empty()) { r.error = "title required"; return r; }
    if (title.size() > 255) { r.error = "title too long"; return r; }
    std::vector<MemberInput> members;
    collectMembers(v.find("members"), members);
    if (members.empty()) { r.error = "no members selected"; return r; }
    std::string desc = trim(v.getString("description", ""));
    std::string err;
    bool ok = db_.transaction([&]() -> bool {
      std::ostringstream sql;
      sql << "INSERT INTO wordlists (user_id,title,source,kind,description,item_count,sort_no) VALUES ("
          << uid << ",'" << Db::escape(title) << "','user','combo',"
          << (desc.empty() ? "NULL" : "'" + Db::escape(desc) + "'") << ",0,0)";
      auto wr = db_.exec(sql.str());
      r.id = wr.last_insert_id;
      if (r.id <= 0) { err = "transaction failed"; return false; }
      return addMembers(r.id, members, err);
    });
    if (!ok) { r.error = err.empty() ? "transaction failed" : err; return r; }
    r.added = itemCountOf(r.id);
    r.ok = true;
    return r;
  }

  // 递归展开：去重 + 防环 + 限深 32
  ExpandResult expand(int64_t id) {
    ExpandResult r;
    if (!readable(id)) { r.error = "not found"; return r; }
    std::set<int64_t> path;
    std::set<std::string> seen;
    std::string err;
    expandInto(id, 0, path, seen, r.words, err);
    if (!err.empty()) { r.error = err; r.words.clear(); return r; }
    r.ok = true;
    return r;
  }

  JsonValue expandJson(int64_t id) {
    ExpandResult r = expand(id);
    if (!r.ok) {
      JsonObject e;
      e["error"] = JsonValue::makeString(r.error);
      return JsonValue::makeObject(e);
    }
    JsonArray arr;
    for (auto& w : r.words) {
      JsonObject o;
      o["item_id"] = JsonValue::makeInt((int)w.item_id);
      o["word"] = JsonValue::makeString(w.word);
      o["meaning"] = w.meaning.empty() ? JsonValue::makeNull() : JsonValue::makeString(w.meaning);
      o["pos"] = w.pos.empty() ? JsonValue::makeNull() : JsonValue::makeString(w.pos);
      o["knowledge_id"] = w.knowledge_id > 0 ? JsonValue::makeInt((int)w.knowledge_id) : JsonValue::makeNull();
      arr.push_back(JsonValue::makeObject(o));
    }
    JsonObject o;
    o["data"] = JsonValue::makeArray(arr);
    o["unique_count"] = JsonValue::makeInt((int)r.words.size());
    return JsonValue::makeObject(o);
  }

private:
  Db& db_;

  static std::vector<std::string> splitTab(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
      size_t pos = s.find('\t', start);
      if (pos == std::string::npos) { out.push_back(s.substr(start)); break; }
      out.push_back(s.substr(start, pos - start));
      start = pos + 1;
    }
    return out;
  }

  static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
  }

  static std::string normalizeWord(const std::string& s) {
    std::string out;
    bool prevSpace = false;
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    for (size_t i = b; i < e; ++i) {
      unsigned char c = (unsigned char)s[i];
      if (std::isspace(c)) {
        if (!prevSpace) out.push_back(' ');
        prevSpace = true;
      } else {
        out.push_back((char)std::tolower(c));
        prevSpace = false;
      }
    }
    return out;
  }

  static int unitIndex(const std::string& unit) {
    if (unit.rfind("Welcome", 0) == 0) return 0;
    if (unit.rfind("Unit", 0) == 0) {
      try { return std::stoi(trim(unit.substr(4))); } catch (...) {}
    }
    return 100;
  }

  static int sortNoFor(const std::string& volume, const std::string& unit) {
    static const std::map<std::string, int> vols = {
      {"必修第一册", 1}, {"必修第二册", 2}, {"必修第三册", 3},
      {"选择性必修第一册", 4}, {"选择性必修第二册", 5},
      {"选择性必修第三册", 6}, {"选择性必修第四册", 7}
    };
    auto it = vols.find(volume);
    int vi = it == vols.end() ? 99 : it->second;
    return vi * 100 + unitIndex(unit);
  }

  static std::string readableWhere(const std::string& alias = "wordlists") {
    int uid = RequestContext::currentUserId();
    return " AND (" + alias + ".source='builtin' OR " + alias + ".user_id=" + std::to_string(uid) + ")";
  }

  bool readable(int64_t id) {
    auto rows = db_.query("SELECT id FROM wordlists WHERE id=" + std::to_string(id) + readableWhere());
    return !rows.empty();
  }

  bool ownedUserList(int64_t id) {
    int uid = RequestContext::currentUserId();
    if (uid <= 0) return false;
    auto rows = db_.query(
      "SELECT id FROM wordlists WHERE id=" + std::to_string(id) + " AND user_id="
      + std::to_string(uid) + " AND source='user'");
    return !rows.empty();
  }

  // ---------- v1.5.0-T005：组合辅助 ----------

  static std::string listColumns() {
    return "id,title,source,kind,count_dirty,textbook,volume,unit,scope_key,description,sort_no,item_count,"
           "DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s') AS created_at,"
           "DATE_FORMAT(updated_at,'%Y-%m-%d %H:%i:%s') AS updated_at ";
  }

  static std::string field(const std::map<std::string, std::string>& r, const std::string& k,
                           const std::string& def = "") {
    auto it = r.find(k);
    return it == r.end() ? def : it->second;
  }

  std::string kindOf(int64_t id) {
    auto rows = db_.query("SELECT kind FROM wordlists WHERE id=" + std::to_string(id));
    return rows.empty() ? "" : rows[0]["kind"];
  }

  int itemCountOf(int64_t id) {
    auto rows = db_.query("SELECT item_count FROM wordlists WHERE id=" + std::to_string(id));
    return rows.empty() ? 0 : std::stoi(rows[0]["item_count"]);
  }

  static void collectMembers(const JsonValue* arr, std::vector<MemberInput>& out) {
    if (!arr || !arr->isArray()) return;
    for (auto& el : arr->asArray()) {
      MemberInput m;
      if (el.isObject()) {
        m.type = el.getString("type", "");
        m.id = el.getInt("id", 0);
      }
      if (!m.type.empty() && m.id > 0) out.push_back(m);
    }
  }

  // child 是否为 parent 的祖先（或就是 parent），用于防环
  bool reaches(int64_t from, int64_t target, std::set<int64_t>& seen) {
    if (from == target) return true;
    if (!seen.insert(from).second) return false;
    auto rows = db_.query(
      "SELECT parent_id FROM wordlist_children WHERE child_type='list' AND child_id="
      + std::to_string(from));
    for (auto& r : rows) {
      if (reaches(std::stoll(r["parent_id"]), target, seen)) return true;
    }
    return false;
  }

  bool validateMember(int64_t parentId, const MemberInput& m, std::string& err) {
    if (m.type == "list") {
      auto rows = db_.query("SELECT id,source,user_id FROM wordlists WHERE id=" + std::to_string(m.id));
      if (rows.empty()) { err = "成员单词本不存在"; return false; }
      if (rows[0]["source"] != "builtin" &&
          std::stoi(rows[0]["user_id"]) != RequestContext::currentUserId()) {
        err = "无权使用该单词本"; return false;
      }
      if (parentId > 0) {
        std::set<int64_t> seen;
        if (reaches(parentId, m.id, seen)) { err = "检测到组合环路"; return false; }
      }
      return true;
    }
    if (m.type == "word") {
      auto rows = db_.query(
        "SELECT wi.id FROM wordlist_items wi JOIN wordlists wl ON wl.id=wi.wordlist_id "
        "WHERE wi.id=" + std::to_string(m.id) + readableWhere("wl"));
      if (rows.empty()) { err = "成员单词不存在"; return false; }
      return true;
    }
    err = "成员类型无效";
    return false;
  }

  bool addMembers(int64_t parentId, const std::vector<MemberInput>& members, std::string& err) {
    for (auto& m : members) {
      if (!validateMember(parentId, m, err)) return false;
    }
    int maxOrder = 0;
    auto ord = db_.query("SELECT COALESCE(MAX(order_no),0) AS mo FROM wordlist_children WHERE parent_id="
                         + std::to_string(parentId));
    if (!ord.empty()) maxOrder = std::stoi(ord[0]["mo"]);
    for (auto& m : members) {
      db_.exec("INSERT IGNORE INTO wordlist_children (parent_id,child_type,child_id,order_no) VALUES ("
               + std::to_string(parentId) + ",'" + Db::escape(m.type) + "'," + std::to_string(m.id) + ","
               + std::to_string(++maxOrder) + ")");
    }
    db_.exec("UPDATE wordlists SET count_dirty=1 WHERE id=" + std::to_string(parentId));
    markAncestorsDirty(parentId);
    recount(parentId);
    return true;
  }

  // 祖先生效链置脏，保证嵌套组合词数正确
  void markAncestorsDirty(int64_t id) {
    std::set<int64_t> seen;
    std::vector<int64_t> stack{id};
    while (!stack.empty()) {
      int64_t cur = stack.back();
      stack.pop_back();
      if (!seen.insert(cur).second) continue;
      auto rows = db_.query(
        "SELECT parent_id FROM wordlist_children WHERE child_type='list' AND child_id="
        + std::to_string(cur));
      for (auto& r : rows) {
        int64_t pid = std::stoll(r["parent_id"]);
        if (seen.count(pid)) continue;
        db_.exec("UPDATE wordlists SET count_dirty=1 WHERE id=" + std::to_string(pid));
        stack.push_back(pid);
      }
    }
  }

  void expandInto(int64_t listId, int depth, std::set<int64_t>& path, std::set<std::string>& seen,
                  std::vector<ExpandWord>& out, std::string& err) {
    if (!err.empty()) return;
    if (depth > 32) { err = "组合层级超过 32 层"; return; }
    if (path.count(listId)) { err = "检测到组合环路"; return; }
    auto rows = db_.query("SELECT kind FROM wordlists WHERE id=" + std::to_string(listId));
    if (rows.empty()) { err = "词表不存在"; return; }

    path.insert(listId);
    if (rows[0]["kind"] != "combo") {
      auto items = db_.query(
        "SELECT id,word,normalized_word,meaning,pos,knowledge_id FROM wordlist_items "
        "WHERE wordlist_id=" + std::to_string(listId) + " ORDER BY order_no ASC, id ASC");
      for (auto& it : items) pushWord(it, seen, out);
    }
    // 普通单词本也可能整本加入过别的单词本，成员同样参与展开去重
    {
      auto kids = db_.query(
        "SELECT child_type,child_id FROM wordlist_children WHERE parent_id="
        + std::to_string(listId) + " ORDER BY order_no ASC, id ASC");
      for (auto& k : kids) {
        if (!err.empty()) break;
        if (k["child_type"] == "list") {
          expandInto(std::stoll(k["child_id"]), depth + 1, path, seen, out, err);
        } else {
          auto w = db_.query(
            "SELECT id,word,normalized_word,meaning,pos,knowledge_id FROM wordlist_items WHERE id="
            + k["child_id"] + " LIMIT 1");
          if (!w.empty()) pushWord(w[0], seen, out);
        }
      }
    }
    path.erase(listId);
  }

  static void pushWord(const std::map<std::string, std::string>& row, std::set<std::string>& seen,
                       std::vector<ExpandWord>& out) {
    std::string key = field(row, "normalized_word");
    if (key.empty()) key = normalizeWord(field(row, "word"));
    if (key.empty() || !seen.insert(key).second) return;
    ExpandWord w;
    w.item_id = std::stoll(row.at("id"));
    w.word = field(row, "word");
    w.meaning = field(row, "meaning");
    w.pos = field(row, "pos");
    std::string kid = field(row, "knowledge_id");
    w.knowledge_id = kid.empty() ? 0 : std::stoll(kid);
    out.push_back(w);
  }

  // 重算词数：无成员的本 = 自身词条数；组合或带成员的本 = 展开去重后的词数
  bool recount(int64_t id) {
    std::string kind = kindOf(id);
    if (kind.empty()) return false;
    auto kc = db_.query("SELECT COUNT(*) AS c FROM wordlist_children WHERE parent_id="
                        + std::to_string(id));
    bool hasKids = !kc.empty() && std::stoi(kc[0]["c"]) > 0;
    int count = 0;
    if (kind == "combo" || hasKids) {
      ExpandResult r = expand(id);
      if (!r.ok) return false;
      count = (int)r.words.size();
    } else {
      auto c = db_.query("SELECT COUNT(*) AS c FROM wordlist_items WHERE wordlist_id="
                         + std::to_string(id));
      if (!c.empty()) count = std::stoi(c[0]["c"]);
    }
    db_.exec("UPDATE wordlists SET item_count=" + std::to_string(count)
             + ", count_dirty=0 WHERE id=" + std::to_string(id));
    return true;
  }

  void recountDirty(int uid) {
    auto rows = db_.query(
      "SELECT id FROM wordlists WHERE user_id=" + std::to_string(uid)
      + " AND count_dirty=1");
    for (auto& r : rows) recount(std::stoll(r["id"]));
  }

  static JsonValue mapList(const std::map<std::string, std::string>& r) {
    JsonObject o;
    o["id"] = JsonValue::makeInt((int)std::stoll(r.at("id")));
    o["title"] = JsonValue::makeString(r.at("title"));
    o["source"] = JsonValue::makeString(r.at("source"));
    o["kind"] = JsonValue::makeString(field(r, "kind", "book"));
    o["count_dirty"] = JsonValue::makeInt(std::stoi(field(r, "count_dirty", "0")));
    o["textbook"] = r.at("textbook").empty() ? JsonValue::makeNull() : JsonValue::makeString(r.at("textbook"));
    o["volume"] = r.at("volume").empty() ? JsonValue::makeNull() : JsonValue::makeString(r.at("volume"));
    o["unit"] = r.at("unit").empty() ? JsonValue::makeNull() : JsonValue::makeString(r.at("unit"));
    o["description"] = r.at("description").empty() ? JsonValue::makeNull() : JsonValue::makeString(r.at("description"));
    o["scope_key"] = r.at("scope_key").empty() ? JsonValue::makeNull() : JsonValue::makeString(r.at("scope_key"));
    o["sort_no"] = JsonValue::makeInt(std::stoi(r.at("sort_no")));
    o["item_count"] = JsonValue::makeInt(std::stoi(r.at("item_count")));
    o["created_at"] = JsonValue::makeString(r.at("created_at"));
    o["updated_at"] = JsonValue::makeString(r.at("updated_at"));
    return JsonValue::makeObject(o);
  }

  static void collectItems(const JsonValue& v, std::vector<ItemInput>& out) {
    auto* items = v.find("items");
    if (items && items->isArray()) {
      for (auto& el : items->asArray()) {
        ItemInput item;
        if (el.isString()) {
          item.word = trim(el.asString());
        } else if (el.isObject()) {
          item.word = trim(el.getString("word", ""));
          item.meaning = trim(el.getString("meaning", ""));
          item.pos = trim(el.getString("pos", ""));
        }
        if (!item.word.empty()) out.push_back(item);
      }
    }

    auto* words = v.find("words");
    if (words && words->isArray()) {
      for (auto& el : words->asArray()) {
        ItemInput item;
        if (el.isString()) item.word = trim(el.asString());
        else if (el.isObject()) {
          item.word = trim(el.getString("word", ""));
          item.meaning = trim(el.getString("meaning", ""));
          item.pos = trim(el.getString("pos", ""));
        }
        if (!item.word.empty()) out.push_back(item);
      }
    }

    auto* text = v.find("text");
    if (text && text->isString()) {
      std::istringstream lines(text->asString());
      std::string line;
      while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto cols = splitTab(line);
        ItemInput item;
        item.word = trim(cols.empty() ? "" : cols[0]);
        if (cols.size() > 1) item.meaning = trim(cols[1]);
        if (cols.size() > 2) item.pos = trim(cols[2]);
        if (!item.word.empty()) out.push_back(item);
      }
    }
  }

  static void dedupe(std::vector<ItemInput>& items) {
    std::set<std::string> seen;
    std::vector<ItemInput> out;
    for (auto& item : items) {
      std::string key = normalizeWord(item.word);
      if (key.empty() || seen.count(key)) continue;
      seen.insert(key);
      out.push_back(item);
    }
    items.swap(out);
  }

  CreateResult createInternal(int uid, const std::string& title, const std::string& description,
                              const std::string& source, const std::string& scopeKey,
                              const std::vector<ItemInput>& inputItems, int sortNo,
                              const std::string& kind = "book") {
    CreateResult r;
    bool ok = db_.transaction([&]() -> bool {
      std::ostringstream sql;
      sql << "INSERT INTO wordlists (user_id,title,source,kind,scope_key,description,item_count,sort_no) VALUES ("
          << uid << ",'" << Db::escape(title) << "','" << Db::escape(source) << "','" << Db::escape(kind) << "',"
          << (scopeKey.empty() ? "NULL" : "'" + Db::escape(scopeKey) + "'") << ","
          << (description.empty() ? "NULL" : "'" + Db::escape(description) + "'") << ","
          << (int)inputItems.size() << "," << sortNo << ")";
      auto wr = db_.exec(sql.str());
      r.id = wr.last_insert_id;
      if (r.id <= 0) return false;

      int order = 0;
      for (auto& item : inputItems) {
        ++order;
        try {
          auto iw = db_.exec(
            "INSERT INTO wordlist_items (wordlist_id,word,normalized_word,meaning,pos,order_no) VALUES ("
            + std::to_string(r.id) + ",'" + Db::escape(item.word) + "','"
            + Db::escape(normalizeWord(item.word)) + "',"
            + (item.meaning.empty() ? "NULL" : "'" + Db::escape(item.meaning) + "'") + ","
            + (item.pos.empty() ? "NULL" : "'" + Db::escape(item.pos) + "'") + ","
            + std::to_string(order) + ")");
          if (iw.affected_rows > 0) r.added++; else r.skipped++;
        } catch (...) { r.skipped++; }
      }
      db_.exec("UPDATE wordlists SET item_count=" + std::to_string(r.added)
               + " WHERE id=" + std::to_string(r.id));
      return true;
    });
    if (!ok) { r.error = "transaction failed"; return r; }
    r.ok = true;
    return r;
  }
};

} // namespace eng
