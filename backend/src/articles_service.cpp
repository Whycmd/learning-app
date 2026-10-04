// 文章业务 Service 实现
#include "../include/articles_service.h"
#include <regex>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <iostream>

namespace eng {

// ============ 静态辅助 ============

bool ArticlesService::isValidCefr(const std::string& s) {
  static const std::unordered_set<std::string> valid = {"A1","A2","B1","B2","C1"};
  return valid.count(s) > 0;
}

std::string ArticlesService::normalizeCategory(const std::string& c) {
  static const std::unordered_set<std::string> valid = {
    "news","history","literature","science","culture","professional","other"
  };
  return valid.count(c) > 0 ? c : "other";
}

std::string ArticlesService::normalizeImportMethod(const std::string& m) {
  static const std::unordered_set<std::string> valid = {
    "url","text","image","ai_generated","ai_recommended","manual"
  };
  return valid.count(m) > 0 ? m : "text";
}

std::vector<std::string> ArticlesService::simpleSplitSentences(const std::string& content) {
  // 分句规则：
  // 1. 以 .!? 结尾 + 空白 + 大写字母开头 → 句子边界
  // 2. 特殊缩写（Mr./Mrs./Ms./Dr./St./Jr./Sr./Prof./Rev./vs./etc./e.g./i.e./U.S./U.K.）后不切分
  std::vector<std::string> sentences;
  std::string cur;

  // 缩写白名单（小写）
  static const std::unordered_set<std::string> ABBREVIATIONS = {
    "mr.", "mrs.", "ms.", "dr.", "st.", "jr.", "sr.", "prof.", "rev.",
    "vs.", "etc.", "e.g.", "i.e.", "u.s.", "u.k.", "u.n.", "u.a.",
    "inc.", "ltd.", "co.", "corp.", "dept.", "vol.", "no.", "fig.",
    "ph.d.", "m.d.", "b.a.", "b.s.", "m.a.", "m.s.",
    "a.m.", "p.m.", "ad", "bc", "ce", "bce"
  };

  for (size_t i = 0; i < content.size(); i++) {
    char c = content[i];
    cur += c;

    // 检查句子边界
    if ((c == '.' || c == '!' || c == '?') && i + 2 < content.size()) {
      char next1 = content[i+1];
      char next2 = content[i+2];

      // 下一个字符必须是空白
      if (!std::isspace(static_cast<unsigned char>(next1))) continue;

      // 空白后必须是大写字母或引号
      bool startNew = false;
      if (std::isupper(static_cast<unsigned char>(next2)) || next2 == '"' || next2 == '\'' || next2 == '(' || next2 == '[') {
        startNew = true;
      }

      if (startNew) {
        // 检查是不是缩写末尾
        std::string lower_cur = cur;
        std::transform(lower_cur.begin(), lower_cur.end(), lower_cur.begin(), ::tolower);
        // 取最后 5 个字符查缩写表
        size_t check_len = std::min<size_t>(5, lower_cur.size());
        std::string tail = lower_cur.substr(lower_cur.size() - check_len);

        bool is_abbrev = false;
        for (auto& abbr : ABBREVIATIONS) {
          if (tail.size() >= abbr.size()) {
            if (tail.compare(tail.size() - abbr.size(), abbr.size(), abbr) == 0) {
              is_abbrev = true;
              break;
            }
          }
        }

        // 另外：单字母（如 "I." "A." "B."）通常是缩写
        // 但如果前面有空格+单字母+点，大概率是缩写
        size_t before_dot = cur.find_last_not_of(". ");
        if (before_dot != std::string::npos && before_dot >= 1) {
          // 简单启发式：如果句号前是单个大写字母 + 空格，跳过
          std::string::size_type prev_space = cur.find_last_of(' ', before_dot);
          if (prev_space != std::string::npos) {
            std::string word = cur.substr(prev_space + 1, before_dot - prev_space);
            if (word.size() == 1 && std::isupper(static_cast<unsigned char>(word[0]))) {
              is_abbrev = true;
            }
          }
        }

        if (!is_abbrev) {
          // 切分
          // 去掉末尾的空白
          std::string trimmed = cur;
          while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
            trimmed.pop_back();
          if (!trimmed.empty()) sentences.push_back(trimmed);
          cur.clear();
          // 跳过已处理的空白
          i++;  // 跳过 next1（空白）
        }
      }
    }
  }

  // 最后一段
  std::string trimmed = cur;
  while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
    trimmed.pop_back();
  if (!trimmed.empty()) sentences.push_back(trimmed);

  return sentences;
}

// ============ 创建 ============

ArticlesService::CreateResult ArticlesService::create(
    const std::string& title,
    const std::string& content,
    const std::string& category,
    const std::string& cefr_level,
    const std::string& import_method,
    const std::string& source_url,
    const std::string& source_name,
    int user_id)
{
  CreateResult r;

  if (title.empty() || content.empty()) {
    r.error = "title and content required";
    return r;
  }

  int uid = user_id > 0 ? user_id : RequestContext::currentUserId();
  std::string cat = normalizeCategory(category);
  std::string im = normalizeImportMethod(import_method);

  // 字数统计（粗略：按空格分割）
  int word_count = 0;
  for (char c : content) {
    if (c == ' ') word_count++;
  }
  word_count = std::max(word_count, 1);

  // 计算 CEFR 等级（如果没指定）
  std::string effective_cefr = cefr_level;
  if (effective_cefr.empty() || !isValidCefr(effective_cefr)) {
    auto report = vocabSvc_.analyzeDifficulty(content, "B1");
    if (!report.article_cefr.empty()) {
      effective_cefr = report.article_cefr;
    } else {
      effective_cefr = "B1";  // 默认
    }
  }

  // 拼接 SQL
  std::ostringstream sql;
  sql << "INSERT INTO articles (user_id, title, content, source_url, source_name, category, import_method, cefr_level, word_count) VALUES ("
      << uid << ", '"
      << Db::escape(title) << "', '"
      << Db::escape(content) << "', "
      << (source_url.empty() ? "NULL" : "'" + Db::escape(source_url) + "'") << ", "
      << (source_name.empty() ? "NULL" : "'" + Db::escape(source_name) + "'") << ", '"
      << cat << "', '"
      << im << "', '"
      << effective_cefr << "', "
      << word_count << ")";

  try {
    auto wr = db_.exec(sql.str());
    r.id = wr.last_insert_id;
    r.ok = true;
  } catch (const std::exception& e) {
    r.error = e.what();
    return r;
  }

  // 分句
  splitAndStoreSentences(r.id, content);

  // 词汇分析
  analyzeAndStoreVocabulary(r.id, content, uid);

  return r;
}

// ============ 列表 ============

JsonValue ArticlesService::list(int page, int size,
                                const std::string& category_filter,
                                const std::string& cefr_filter,
                                bool only_mine,
                                int user_id) {
  if (page < 1) page = 1;
  if (size < 1) size = 20;
  if (size > 100) size = 100;

  int uid = user_id > 0 ? user_id : RequestContext::currentUserId();

  std::ostringstream where;
  bool first = true;
  auto addCond = [&](const std::string& cond) {
    where << (first ? " WHERE " : " AND ") << cond;
    first = false;
  };

  if (only_mine) {
    addCond("user_id=" + std::to_string(uid));
  } else {
    // 默认返回：我的 + 公共（user_id=1 游客的文章视为公共）
    addCond("(user_id=" + std::to_string(uid) + " OR user_id=1)");
  }

  if (!category_filter.empty() && category_filter != "all") {
    addCond("category='" + Db::escape(category_filter) + "'");
  }
  if (!cefr_filter.empty() && cefr_filter != "all" && isValidCefr(cefr_filter)) {
    addCond("cefr_level='" + Db::escape(cefr_filter) + "'");
  }

  // 总数
  auto countRows = db_.query("SELECT COUNT(*) AS cnt FROM articles" + where.str());
  int total = 0;
  if (!countRows.empty()) {
    try { total = std::stoi(countRows[0]["cnt"]); } catch(...) {}
  }

  // 分页查询
  int offset = (page - 1) * size;
  std::ostringstream sql;
  sql << "SELECT id, user_id, title, source_name, category, cefr_level, word_count, import_method, read_at, "
      << "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at "
      << "FROM articles" << where.str()
      << " ORDER BY id DESC LIMIT " << size << " OFFSET " << offset;

  auto rows = db_.query(sql.str());
  JsonArray arr;
  for (auto& r : rows) {
    JsonObject o;
    o["id"] = JsonValue::makeInt(std::stoll(r["id"]));
    o["user_id"] = JsonValue::makeInt(std::stoll(r["user_id"]));
    o["title"] = JsonValue::makeString(r["title"]);
    o["source_name"] = r["source_name"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["source_name"]);
    o["category"] = JsonValue::makeString(r["category"]);
    o["cefr_level"] = JsonValue::makeString(r["cefr_level"]);
    o["word_count"] = JsonValue::makeInt(std::stoi(r["word_count"]));
    o["import_method"] = JsonValue::makeString(r["import_method"]);
    o["read_at"] = r["read_at"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["read_at"]);
    o["created_at"] = JsonValue::makeString(r["created_at"]);
    arr.push_back(JsonValue::makeObject(o));
  }

  JsonObject result;
  result["data"] = JsonValue::makeArray(arr);
  result["total"] = JsonValue::makeInt(total);
  result["page"] = JsonValue::makeInt(page);
  result["size"] = JsonValue::makeInt(size);
  return JsonValue::makeObject(result);
}

// ============ 详情 ============

JsonValue ArticlesService::detail(int64_t id, int user_id) {
  int uid = user_id > 0 ? user_id : RequestContext::currentUserId();

  auto rows = db_.query("SELECT * FROM articles WHERE id=" + std::to_string(id));
  if (rows.empty()) return JsonValue::makeNull();

  auto& r = rows[0];
  JsonObject o;
  for (auto& kv : r) {
    if (kv.first == "id" || kv.first == "user_id" || kv.first == "word_count") {
      o[kv.first] = JsonValue::makeInt(std::stoll(kv.second.empty() ? "0" : kv.second));
    } else if (kv.first == "read_at" || kv.first == "created_at" || kv.first == "updated_at") {
      o[kv.first] = kv.second.empty() ? JsonValue::makeNull() : JsonValue::makeString(kv.second);
    } else if (kv.second.empty()) {
      o[kv.first] = JsonValue::makeNull();
    } else {
      o[kv.first] = JsonValue::makeString(kv.second);
    }
  }

  // 分句
  auto senRows = db_.query(
    "SELECT id, sentence_no, sentence_en, cefr_level FROM article_sentences "
    "WHERE article_id=" + std::to_string(id) + " ORDER BY sentence_no");
  JsonArray sentences;
  for (auto& s : senRows) {
    JsonObject so;
    so["id"] = JsonValue::makeInt(std::stoll(s["id"]));
    so["sentence_no"] = JsonValue::makeInt(std::stoi(s["sentence_no"]));
    so["sentence_en"] = JsonValue::makeString(s["sentence_en"]);
    so["cefr_level"] = s["cefr_level"].empty() ? JsonValue::makeNull() : JsonValue::makeString(s["cefr_level"]);
    sentences.push_back(JsonValue::makeObject(so));
  }
  o["sentences"] = JsonValue::makeArray(sentences);

  // 词汇（只返回对当前用户有意义的，避免过大）
  auto vocabRows = db_.query(
    "SELECT word, cefr_level, occurrence, is_new_word FROM article_vocabulary "
    "WHERE article_id=" + std::to_string(id) + " "
    "AND (user_id=" + std::to_string(uid) + " OR user_id IS NULL) "
    "ORDER BY occurrence DESC LIMIT 200");
  JsonArray vocab;
  for (auto& v : vocabRows) {
    JsonObject vo;
    vo["word"] = JsonValue::makeString(v["word"]);
    vo["cefr_level"] = JsonValue::makeString(v["cefr_level"]);
    vo["occurrence"] = JsonValue::makeInt(std::stoi(v["occurrence"]));
    vo["is_new_word"] = JsonValue::makeInt(std::stoi(v["is_new_word"]));
    vocab.push_back(JsonValue::makeObject(vo));
  }
  o["vocabulary"] = JsonValue::makeArray(vocab);

  return JsonValue::makeObject(o);
}

// ============ 更新 ============

ArticlesService::UpdateResult ArticlesService::update(int64_t id, const JsonObject& fields, int user_id) {
  UpdateResult r;
  int uid = user_id > 0 ? user_id : RequestContext::currentUserId();

  // 检查归属
  auto check = db_.query("SELECT user_id FROM articles WHERE id=" + std::to_string(id));
  if (check.empty()) { r.error = "not found"; return r; }
  // 允许更新自己的文章 或 公共文章（user_id=1）
  if (std::stoi(check[0]["user_id"]) != uid && std::stoi(check[0]["user_id"]) != 1) {
    r.error = "forbidden"; return r;
  }

  std::vector<std::string> sets;
  auto it = fields.find("title");
  if (it != fields.end() && it->second.isString()) sets.push_back("title='" + Db::escape(it->second.asString()) + "'");

  it = fields.find("content");
  if (it != fields.end() && it->second.isString()) sets.push_back("content='" + Db::escape(it->second.asString()) + "'");

  it = fields.find("category");
  if (it != fields.end() && it->second.isString()) {
    std::string nc = normalizeCategory(it->second.asString());
    sets.push_back("category='" + nc + "'");
  }

  it = fields.find("cefr_level");
  if (it != fields.end() && it->second.isString() && isValidCefr(it->second.asString())) {
    sets.push_back("cefr_level='" + it->second.asString() + "'");
  }

  it = fields.find("source_url");
  if (it != fields.end()) {
    sets.push_back(it->second.isNull() ? "source_url=NULL" : "source_url='" + Db::escape(it->second.asString()) + "'");
  }

  it = fields.find("source_name");
  if (it != fields.end()) {
    sets.push_back(it->second.isNull() ? "source_name=NULL" : "source_name='" + Db::escape(it->second.asString()) + "'");
  }

  if (sets.empty()) { r.ok = true; return r; }

  try {
    db_.exec("UPDATE articles SET " + [&]{
      std::ostringstream o;
      for (size_t i = 0; i < sets.size(); i++) {
        if (i) o << ", ";
        o << sets[i];
      }
      return o.str();
    }() + " WHERE id=" + std::to_string(id));
    r.ok = true;
  } catch (const std::exception& e) {
    r.error = e.what();
  }
  return r;
}

// ============ 删除（级联清理） ============

ArticlesService::UpdateResult ArticlesService::remove(int64_t id, int user_id) {
  UpdateResult r;
  int uid = user_id > 0 ? user_id : RequestContext::currentUserId();

  auto check = db_.query("SELECT user_id FROM articles WHERE id=" + std::to_string(id));
  if (check.empty()) { r.error = "not found"; return r; }
  if (std::stoi(check[0]["user_id"]) != uid) { r.error = "forbidden"; return r; }

  try {
    db_.exec("DELETE FROM article_sentences WHERE article_id=" + std::to_string(id));
    db_.exec("DELETE FROM article_vocabulary WHERE article_id=" + std::to_string(id));
    db_.exec("DELETE FROM article_quizzes WHERE article_id=" + std::to_string(id));
    db_.exec("DELETE FROM articles WHERE id=" + std::to_string(id));
    r.ok = true;
  } catch (const std::exception& e) {
    r.error = e.what();
  }
  return r;
}

// ============ 难度报告 ============

JsonValue ArticlesService::difficultyReport(int64_t id, int user_id) {
  int uid = user_id > 0 ? user_id : RequestContext::currentUserId();

  auto rows = db_.query(
    "SELECT a.cefr_level AS article_cefr, a.content FROM articles a WHERE a.id=" + std::to_string(id));
  if (rows.empty()) return JsonValue::makeNull();

  std::string content = rows[0]["content"];
  std::string article_cefr = rows[0]["article_cefr"];

  // 从 users 表推断用户 CEFR（简单实现：默认 B1）
  // 后续可扩展：根据 learner_identity 动态推断
  std::string user_cefr = "B1";

  // 重新跑一次难度分析（因为 article_vocabulary 是全局标记，
  // 生词判定需要按当前用户等级重新计算）
  auto report = vocabSvc_.analyzeDifficulty(content, user_cefr);

  JsonObject o;
  o["article_id"] = JsonValue::makeInt(id);
  o["article_cefr"] = JsonValue::makeString(article_cefr);
  o["user_cefr"] = JsonValue::makeString(user_cefr);
  o["total_words"] = JsonValue::makeInt(report.total_words);
  o["unique_words"] = JsonValue::makeInt(report.unique_words);
  o["analysis_cefr"] = JsonValue::makeString(report.article_cefr);

  // 各等级词数
  JsonObject level_counts;
  static const char* LEVELS[] = {"A1","A2","B1","B2","C1"};
  for (auto lv : LEVELS) {
    auto it = report.level_counts.find(lv);
    level_counts[lv] = JsonValue::makeInt(it == report.level_counts.end() ? 0 : it->second);
  }
  o["level_counts"] = JsonValue::makeObject(level_counts);

  // 词汇 CEFR 映射
  JsonObject vocab_cefr_map;
  for (auto& kv : report.vocab_cefr) {
    vocab_cefr_map[kv.first] = JsonValue::makeString(kv.second);
  }
  o["vocab_cefr"] = JsonValue::makeObject(vocab_cefr_map);

  // 生词列表
  JsonArray new_words;
  for (auto& w : report.new_words) new_words.push_back(JsonValue::makeString(w));
  o["new_words"] = JsonValue::makeArray(new_words);

  // 边缘词列表
  JsonArray edge_words;
  for (auto& w : report.edge_words) edge_words.push_back(JsonValue::makeString(w));
  o["edge_words"] = JsonValue::makeArray(edge_words);

  if (!report.error.empty()) o["error"] = JsonValue::makeString(report.error);

  return JsonValue::makeObject(o);
}

// ============ 分句 + 词汇分析（内部） ============

int ArticlesService::splitAndStoreSentences(int64_t article_id, const std::string& content) {
  auto sentences = simpleSplitSentences(content);

  // 先清空旧分句（可能是更新时重跑）
  db_.exec("DELETE FROM article_sentences WHERE article_id=" + std::to_string(article_id));

  int no = 1;
  for (auto& sen : sentences) {
    // 每句查 CEFR 等级（用难度分析取该句众数）
    std::string sen_cefr = "B1";
    auto sr = vocabSvc_.analyzeDifficulty(sen, "B1");
    if (!sr.article_cefr.empty()) sen_cefr = sr.article_cefr;

    std::ostringstream sql;
    sql << "INSERT INTO article_sentences (article_id, sentence_no, sentence_en, cefr_level) VALUES ("
        << article_id << ", " << no << ", '"
        << Db::escape(sen) << "', '"
        << sen_cefr << "')";
    try { db_.exec(sql.str()); } catch (...) {}
    no++;
  }

  return sentences.size();
}

int ArticlesService::analyzeAndStoreVocabulary(int64_t article_id, const std::string& content, int user_id) {
  // 推断用户 CEFR（默认 B1）
  std::string user_cefr = "B1";
  int user_rank = ArticlesVocabService::cefrRank(user_cefr);

  auto report = vocabSvc_.analyzeDifficulty(content, user_cefr);

  // 清空旧词汇
  db_.exec("DELETE FROM article_vocabulary WHERE article_id=" + std::to_string(article_id)
           + " AND (user_id=" + std::to_string(user_id) + " OR user_id IS NULL)");

  // 统计位置（首次出现位置）和出现次数
  std::unordered_map<std::string, int> first_pos;
  std::unordered_map<std::string, int> occurrence;

  // 简单扫一遍 content 找到每个词首次出现位置
  std::string cur;
  int pos = 0;
  for (int i = 0; i < (int)content.size(); i++) {
    char c = content[i];
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '\'' || c == '-' || c == '_') {
      if (cur.empty()) pos = i;  // 首次出现位置
      cur += c;
    } else {
      if (cur.size() >= 2) {
        std::string norm = ArticlesVocabService::normalize(cur);
        if (!norm.empty()) {
          if (first_pos.find(norm) == first_pos.end()) first_pos[norm] = pos;
          occurrence[norm]++;
        }
      }
      cur.clear();
    }
  }
  if (cur.size() >= 2) {
    std::string norm = ArticlesVocabService::normalize(cur);
    if (!norm.empty()) {
      if (first_pos.find(norm) == first_pos.end()) first_pos[norm] = pos;
      occurrence[norm]++;
    }
  }

  // 写入
  for (auto& pair : report.vocab_cefr) {
    const std::string& word = pair.first;
    const std::string& level = pair.second;

    auto it_occ = occurrence.find(word);
    int occ = it_occ == occurrence.end() ? 1 : it_occ->second;

    auto it_pos = first_pos.find(word);
    int fpos = it_pos == first_pos.end() ? 0 : it_pos->second;

    // 判定是否生词
    int wr = ArticlesVocabService::cefrRank(level);
    bool is_new = (wr > user_rank + 1);

    std::ostringstream sql;
    sql << "INSERT INTO article_vocabulary (article_id, word, position, occurrence, cefr_level, is_new_word, user_id) VALUES ("
        << article_id << ", '"
        << Db::escape(word) << "', "
        << fpos << ", "
        << occ << ", '"
        << level << "', "
        << (is_new ? 1 : 0) << ", "
        << user_id << ")";
    try { db_.exec(sql.str()); } catch (...) {}
  }

  return (int)report.vocab_cefr.size();
}

} // namespace eng
