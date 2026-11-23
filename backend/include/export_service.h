// 导出中心服务（v1.5.0-T009）
// 职责：导出前汇总统计、游客/当次讲解导出（只读，不调 AI、不写 ai_usage_daily / guest_trials）
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>

#include "db.h"
#include "json.h"
#include "wordlist_service.h"

namespace eng {

class ExportService
{
public:
  ExportService(Db& db, WordlistService& wlSvc, const std::string& wordTmpDir,
                const std::string& root)
    : db_(db), wlSvc_(wlSvc), wordTmpDir_(wordTmpDir), root_(root) {}

  // [BUSINESS] 导出中心汇总：精讲 / 单词本 / 定制练习三类 count + chars
  JsonValue summary(int uid)
  {
    JsonObject root;
    root["kp"] = kpStat(uid);
    root["wordbook"] = wordbookStat(uid);
    root["custom_exercise"] = customExerciseStat(uid);
    return JsonValue::makeObject(root);
  }

  struct FileResult
  {
    bool ok = false;
    int httpStatus = 200;
    std::string error;
    std::string path;
  };

  // [BUSINESS] D12：讲解导出。
  // 游客：仅本人最新一条 status='done'，否则 403「只能导出当前结果」；
  // 登录用户：仅本人讲解（D13 的后端兜底，未入池提示由前端处理）。
  FileResult exportExplanation(int64_t explainId, int uid, const std::string& guestId)
  {
    FileResult r;
    auto rows = db_.query(
      "SELECT id,user_id,IFNULL(guest_id,'') AS guest_id,IFNULL(input_text,'') AS input_text,"
      "IFNULL(summary,'') AS summary,IFNULL(content_md,'') AS content_md,status,"
      "DATE_FORMAT(created_at,'%Y-%m-%d') AS created FROM explanations WHERE id=" +
      std::to_string(explainId));
    if (rows.empty()) { r.httpStatus = 404; r.error = "讲解不存在"; return r; }
    auto& row = rows[0];
    int owner = 0;
    try { owner = std::stoi(row.at("user_id")); } catch (...) {}
    if (owner > 0) {
      if (owner != uid) { r.httpStatus = 404; r.error = "讲解不存在"; return r; }
    } else if (guestId.empty() || row.at("guest_id") != guestId) {
      r.httpStatus = 404; r.error = "讲解不存在"; return r;
    }
    if (row.at("status") != "done") { r.httpStatus = 409; r.error = "讲解尚未完成"; return r; }
    if (row.at("content_md").empty()) { r.httpStatus = 409; r.error = "讲解内容为空"; return r; }
    if (owner <= 0) {
      auto latest = db_.query(
        "SELECT MAX(id) AS m FROM explanations WHERE guest_id='" + Db::escape(guestId) +
        "' AND status='done'");
      int64_t latestId = 0;
      if (!latest.empty() && !latest[0].at("m").empty()) {
        try { latestId = std::stoll(latest[0].at("m")); } catch (...) {}
      }
      if (latestId != explainId) { r.httpStatus = 403; r.error = "只能导出当前结果"; return r; }
    }

    std::string tmpPath = wordTmpDir_ + "/explain_" + std::to_string(explainId) + "_" +
                          std::to_string(::time(nullptr)) + ".json";
    {
      JsonObject o;
      o["id"] = JsonValue::makeInt(explainId);
      o["input_text"] = JsonValue::makeString(row.at("input_text"));
      o["summary"] = JsonValue::makeString(row.at("summary"));
      o["content_md"] = JsonValue::makeString(row.at("content_md"));
      o["created_at"] = JsonValue::makeString(row.at("created"));
      std::ofstream f(tmpPath);
      if (!f) { r.httpStatus = 500; r.error = "导出临时文件写入失败"; return r; }
      f << JsonValue::makeObject(o).dump();
    }

    const char* nodeEnv = std::getenv("NODE_BIN");
    std::string nodeBin = nodeEnv ? nodeEnv : "node";
    std::string cmd = nodeBin + " " + root_ + "/scripts/gen_word_kp.js explain " + tmpPath +
                      " " + wordTmpDir_;
    int rc = std::system(cmd.c_str());
    if (rc != 0) { r.httpStatus = 500; r.error = "导出服务暂不可用"; return r; }
    std::string outPath = wordTmpDir_ + "/explanation_" + std::to_string(explainId) + ".docx";
    if (::access(outPath.c_str(), F_OK) != 0) {
      r.httpStatus = 500; r.error = "导出服务暂不可用"; return r;
    }
    r.ok = true;
    r.path = outPath;
    return r;
  }

  // [BUSINESS] D09/D10/D21：词表库单词卡导出。
  // 递归包含组合子本，按 normalized_word 去重（expand 已处理），仅单词 + 释义。
  FileResult exportWordlistCards(int64_t id)
  {
    FileResult r;
    auto rows = db_.query("SELECT title FROM wordlists WHERE id=" + std::to_string(id));
    if (rows.empty()) { r.httpStatus = 404; r.error = "词表不存在"; return r; }
    auto ex = wlSvc_.expand(id);
    if (!ex.ok) { r.httpStatus = 404; r.error = "词表不存在"; return r; }
    if (ex.words.empty()) { r.httpStatus = 400; r.error = "暂无可导出内容"; return r; }
    if (ex.words.size() > 300) { r.httpStatus = 400; r.error = "单词过多，请拆分后导出"; return r; }

    JsonArray arr;
    for (auto& w : ex.words) {
      JsonObject o;
      o["word"] = JsonValue::makeString(w.word);
      o["meaning"] = JsonValue::makeString(w.meaning);
      arr.push_back(JsonValue::makeObject(o));
    }
    JsonObject payload;
    payload["title"] = JsonValue::makeString(rows[0].at("title"));
    payload["id"] = JsonValue::makeInt(id);
    payload["words"] = JsonValue::makeArray(arr);

    std::string tmpPath = wordTmpDir_ + "/wl_cards_" + std::to_string(id) + "_" +
                          std::to_string((int64_t)::time(nullptr)) + ".json";
    {
      std::ofstream f(tmpPath);
      if (!f) { r.httpStatus = 500; r.error = "导出临时文件写入失败"; return r; }
      f << JsonValue::makeObject(payload).dump();
    }
    const char* nodeEnv = std::getenv("NODE_BIN");
    std::string nodeBin = nodeEnv ? nodeEnv : "node";
    std::string cmd = nodeBin + " " + root_ + "/scripts/gen_word_wordbook.js wordlist " +
                      std::to_string(id) + " " + tmpPath + " " + wordTmpDir_;
    int rc = std::system(cmd.c_str());
    if (rc != 0) { r.httpStatus = 500; r.error = "导出服务暂不可用"; return r; }

    std::string outPath = wordTmpDir_ + "/wordlist_" + std::to_string(id) + ".docx";
    if (::access(outPath.c_str(), F_OK) != 0) {
      r.httpStatus = 500; r.error = "导出服务暂不可用"; return r;
    }
    r.ok = true;
    r.path = outPath;
    return r;
  }

private:
  Db& db_;
  WordlistService& wlSvc_;
  std::string wordTmpDir_;
  std::string root_;

  // 与 wordlist_items.normalized_word 同规则：去首尾空白、合并空白、ASCII 小写
  static std::string normalizeWord(const std::string& s)
  {
    std::string out;
    bool prevSpace = false;
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    for (size_t i = b; i < e; ++i)
    {
      unsigned char c = (unsigned char)s[i];
      if (std::isspace(c))
      {
        if (!prevSpace) out.push_back(' ');
        prevSpace = true;
      }
      else
      {
        out.push_back((char)std::tolower(c));
        prevSpace = false;
      }
    }
    return out;
  }

  // UTF-8 码点数（用于字符数估算）
  static size_t charLen(const std::string& s)
  {
    size_t n = 0;
    for (unsigned char c : s)
    {
      if ((c & 0xC0) != 0x80) n++;
    }
    return n;
  }

  static JsonValue statRow(const Row& r)
  {
    auto val = [&](const char* key) -> int64_t
    {
      auto it = r.find(key);
      if (it == r.end() || it->second.empty()) return 0;
      try { return std::stoll(it->second); } catch (...) { return 0; }
    };
    JsonObject o;
    o["count"] = JsonValue::makeInt(val("c"));
    o["chars"] = JsonValue::makeInt(val("n"));
    return JsonValue::makeObject(o);
  }

  // 精讲：仅统计已固化知识点，字符数取正文主体
  JsonValue kpStat(int uid)
  {
    auto rows = db_.query(
      "SELECT COUNT(*) AS c, "
      "IFNULL(SUM(CHAR_LENGTH(IFNULL(NULLIF(standard_content,''), user_input_content))),0) AS n "
      "FROM knowledge_points WHERE user_id=" + std::to_string(uid) + " AND is_solidified=1");
    return rows.empty() ? JsonValue::makeNull() : statRow(rows[0]);
  }

  // 单词本：当前用户全部单词本（含组合、子本）递归展开，按 normalized 去重
  JsonValue wordbookStat(int uid)
  {
    std::map<std::string, size_t> uniq;
    auto lists = db_.query(
      "SELECT id FROM wordlists WHERE user_id=" + std::to_string(uid) + " ORDER BY id ASC");
    for (auto& r : lists)
    {
      auto ex = wlSvc_.expand(std::stoll(r["id"]));
      if (!ex.ok) continue;
      for (auto& w : ex.words)
      {
        std::string key = normalizeWord(w.word);
        if (key.empty()) continue;
        size_t n = charLen(w.word) + charLen(w.meaning);
        auto it = uniq.find(key);
        if (it == uniq.end() || it->second < n) uniq[key] = n;
      }
    }
    size_t chars = 0;
    for (auto& kv : uniq) chars += kv.second;
    JsonObject o;
    o["count"] = JsonValue::makeInt((int64_t)uniq.size());
    o["chars"] = JsonValue::makeInt((int64_t)chars);
    return JsonValue::makeObject(o);
  }

  // 定制练习：仅已完成
  JsonValue customExerciseStat(int uid)
  {
    auto rows = db_.query(
      "SELECT COUNT(*) AS c, IFNULL(SUM(CHAR_LENGTH(IFNULL(exercise_json,''))),0) AS n "
      "FROM custom_exercises WHERE user_id=" + std::to_string(uid) + " AND status='done'");
    return rows.empty() ? JsonValue::makeNull() : statRow(rows[0]);
  }
};

} // namespace eng
