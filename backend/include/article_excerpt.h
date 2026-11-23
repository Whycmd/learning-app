// 站内文章节选取文工具
// 用途：练习出题时优先提供站内文章节选，文章不足时由 AI 自选语篇兜底
#pragma once
#include "db.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace eng {

inline void replaceAll(std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

inline std::string articleHtmlToText(const std::string& html) {
  std::string s = html;
  replaceAll(s, "<br>", "\n");
  replaceAll(s, "<br/>", "\n");
  replaceAll(s, "<br />", "\n");
  replaceAll(s, "</p>", "\n");
  replaceAll(s, "</P>", "\n");
  replaceAll(s, "&nbsp;", " ");
  replaceAll(s, "&amp;", "&");
  replaceAll(s, "&lt;", "<");
  replaceAll(s, "&gt;", ">");
  replaceAll(s, "&quot;", "\"");
  replaceAll(s, "&#39;", "'");
  // 去掉剩余标签
  std::string out;
  out.reserve(s.size());
  bool inTag = false;
  for (char c : s) {
    if (c == '<') { inTag = true; continue; }
    if (c == '>') { inTag = false; continue; }
    if (!inTag) out += c;
  }
  // 压缩空白，保留段落换行
  std::string cleaned;
  cleaned.reserve(out.size());
  bool lastSpace = false;
  for (char c : out) {
    if (c == '\r') continue;
    if (c == '\n') {
      while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
      if (!cleaned.empty() && cleaned.back() != '\n') cleaned += '\n';
      lastSpace = false;
      continue;
    }
    if (c == ' ' || c == '\t') {
      if (!lastSpace && !cleaned.empty() && cleaned.back() != '\n') { cleaned += ' '; lastSpace = true; }
      continue;
    }
    cleaned += c;
    lastSpace = false;
  }
  while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '\n')) cleaned.pop_back();
  return cleaned;
}

// 从 articles 表取一篇公共或本人文章，优先文学类；返回「标题 + 正文节选」
// 无文章、表不存在或查询失败时返回空串，由调用方注入兜底说明。
inline std::string pickArticleExcerpt(Db& db, int uid, size_t maxChars = 1600) {
  try {
    std::ostringstream sql;
    sql << "SELECT title, content FROM articles "
        << "WHERE (visibility='public' OR user_id=" << uid << ") "
        << "AND content IS NOT NULL AND CHAR_LENGTH(content)>=80 "
        << "ORDER BY (category='literature') DESC, RAND() LIMIT 1";
    Rows rows = db.query(sql.str());
    if (rows.empty()) return "";
    const Row& row = rows.front();
    std::string title = row.count("title") ? row.at("title") : "";
    std::string text = articleHtmlToText(row.count("content") ? row.at("content") : "");
    if (text.empty()) return "";

    if (text.size() > maxChars) {
      size_t cut = maxChars;
      // 尽量在句末截断，避免半句
      size_t best = std::string::npos;
      for (size_t i = maxChars; i > maxChars / 2; --i) {
        if (text[i] == '.' || text[i] == '!' || text[i] == '?') { best = i + 1; break; }
      }
      if (best != std::string::npos) cut = best;
      text = text.substr(0, cut);
    }

    std::string out = "文章标题：" + title + "\n正文节选：\n" + text;
    return out;
  } catch (...) {
    return "";
  }
}

} // namespace eng
