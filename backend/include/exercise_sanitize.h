// v1.5.3-T004 追加：练习题目统一清洗
// 职责：题型白名单、选项前缀清洗、空位标记规范化、缺题面兜底、多空题归一化
// 说明：只处理 AI 返回的题目对象，不读写数据库、不调用 AI
#pragma once
#include "json.h"
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace eng {
namespace exsan {

inline bool isKnownType(const std::string& t) {
  return t == "word_match" || t == "word_form_fill" || t == "context_blank_fill" ||
         t == "cn_to_en_translation" || t == "cloze_choice" || t == "sentence_imitation" ||
         t == "long_reading_cloze" || t == "full_cloze";
}

inline bool isChoiceType(const std::string& t) {
  return t == "word_match" || t == "cloze_choice";
}

inline bool isObjective(const std::string& t) {
  return isChoiceType(t) || t == "word_form_fill" || t == "context_blank_fill" ||
         t == "long_reading_cloze" || t == "full_cloze";
}

inline std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

inline bool hasCjk(const std::string& s) {
  for (unsigned char c : s) if (c >= 0xE0) return true;
  return false;
}

// 去掉选项自带前缀：A. / A、 / A) / A． / a. 等
inline std::string stripOptionPrefix(const std::string& raw) {
  std::string s = trim(raw);
  if (s.size() >= 2) {
    char c = s[0];
    bool letter = (c >= 'A' && c <= 'D') || (c >= 'a' && c <= 'd');
    if (letter) {
      char d = s[1];
      if (d == '.' || d == ')' || d == ':') {
        s = trim(s.substr(2));
      } else if ((unsigned char)d == 0xE3 && s.size() >= 4 &&
                 (unsigned char)s[2] == 0x80 && (unsigned char)s[3] == 0x81) {
        // 顿号“、”
        s = trim(s.substr(4));
      } else if ((unsigned char)d == 0xEF && s.size() >= 4) {
        // 全角句号“．”或全角括号，前缀为 3 字节
        if ((unsigned char)s[2] == 0xBC && ((unsigned char)s[3] == 0x8E || (unsigned char)s[3] == 0x89)) {
          s = trim(s.substr(4));
        }
      }
    }
  }
  return s;
}

// 统一选项文本（不带字母，由页面/导出脚本加字母）
inline std::string cleanOption(const std::string& raw) {
  return stripOptionPrefix(raw);
}

// 把 [[1]]、[[ 2 ]] 替换为指定文本
inline std::string replaceMarkers(const std::string& s, const std::string& with) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '[' && i + 1 < s.size() && s[i + 1] == '[') {
      size_t j = i + 2;
      while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
      size_t numStart = j;
      while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
      size_t numEnd = j;
      while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
      if (numEnd > numStart && j + 1 < s.size() && s[j] == ']' && s[j + 1] == ']') {
        out += with;
        i = j + 2;
        continue;
      }
    }
    out += s[i++];
  }
  return out;
}

// 多空标记：[[1]] -> "______ (1)"，保留空位编号
inline std::string replaceNumberedMarkers(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '[' && i + 1 < s.size() && s[i + 1] == '[') {
      size_t j = i + 2;
      while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
      size_t numStart = j;
      while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
      size_t numEnd = j;
      while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
      if (numEnd > numStart && j + 1 < s.size() && s[j] == ']' && s[j + 1] == ']') {
        out += "______ (" + s.substr(numStart, numEnd - numStart) + ")";
        i = j + 2;
        continue;
      }
    }
    out += s[i++];
  }
  return out;
}

inline int countMarkers(const std::string& s) {
  int n = 0;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '[' && i + 1 < s.size() && s[i + 1] == '[') {
      size_t j = i + 2;
      while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
      size_t numStart = j;
      while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
      size_t numEnd = j;
      while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
      if (numEnd > numStart && j + 1 < s.size() && s[j] == ']' && s[j + 1] == ']') {
        n++;
        i = j + 2;
        continue;
      }
    }
    i++;
  }
  return n;
}

// 单空选择题：只保留下划线，不带编号；多空题保留编号
inline std::string singleChoiceBlank(const std::string& s) {
  return countMarkers(s) > 1 ? replaceNumberedMarkers(s) : replaceMarkers(s, "______");
}

// 词形/语境填空：[[n]] 换下划线；词形题把基础词紧跟空位
inline std::string fillBlankStem(const std::string& s, const std::string& baseWord) {
  std::string out = replaceMarkers(s, "______");
  if (!baseWord.empty() && out.find("(" + baseWord + ")") == std::string::npos) {
    size_t p = out.find("______");
    if (p != std::string::npos) out.insert(p + 6, " (" + baseWord + ")");
  }
  return out;
}

// 依据答案文本/字母定位选项字母
inline std::string letterOfAnswer(const std::string& answer, const JsonArray& opts) {
  std::string a = trim(answer);
  if (a.size() == 1) {
    char c = (char)std::toupper((unsigned char)a[0]);
    if (c >= 'A' && c <= 'D') return std::string(1, c);
  }
  std::string stripped = stripOptionPrefix(a);
  auto lower = [](std::string x) {
    for (auto& c : x) c = (char)std::tolower((unsigned char)c);
    return x;
  };
  for (size_t i = 0; i < opts.size() && i < 4; ++i) {
    if (!opts[i].isString()) continue;
    std::string o = stripOptionPrefix(opts[i].asString());
    if (!o.empty() && lower(o) == lower(stripped)) return std::string(1, (char)('A' + i));
  }
  return "";
}

// 选项数组清洗：至少 4 项，去掉自带字母前缀
inline bool cleanOptions4(const JsonValue& item, JsonArray& out) {
  const JsonValue* ops = item.find("options");
  if (!ops || !ops->isArray() || ops->asArray().size() < 4) return false;
  out.clear();
  for (auto& op : ops->asArray()) {
    if ((int)out.size() >= 4) break;
    if (!op.isString()) continue;
    std::string text = cleanOption(op.asString());
    if (text.empty()) continue;
    out.push_back(JsonValue::makeString(text));
  }
  return out.size() == 4;
}

// 识别模型把字母选项当文本的坏数据：["A","B","C","D"]
inline bool isLetterBank(const JsonArray& opts) {
  if (opts.size() != 4) return false;
  for (auto& o : opts) {
    if (!o.isString() || o.asString().size() != 1) return false;
    char c = o.asString()[0];
    if (c < 'A' || c > 'D') return false;
  }
  return true;
}

inline std::string joinAnswers(const std::vector<std::pair<int, std::string>>& items) {
  std::string out;
  for (auto& kv : items) {
    if (!out.empty()) out += "  ";
    out += std::to_string(kv.first) + ". " + kv.second;
  }
  return out;
}

// 扫描 [[n]] 空位编号
inline void collectMarkerNos(const std::string& s, std::set<int>& out) {
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '[' && i + 1 < s.size() && s[i + 1] == '[') {
      size_t j = i + 2;
      while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
      size_t numStart = j;
      while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
      size_t numEnd = j;
      while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
      if (numEnd > numStart && j + 1 < s.size() && s[j] == ']' && s[j + 1] == ']') {
        out.insert(std::stoi(s.substr(numStart, numEnd - numStart)));
        i = j + 2;
        continue;
      }
    }
    i++;
  }
}

// 删除没有对应答案的空位标记，避免留下无法作答的空
inline std::string removeOrphanMarkers(const std::string& s, const std::set<int>& keep) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '[' && i + 1 < s.size() && s[i + 1] == '[') {
      size_t j = i + 2;
      while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
      size_t numStart = j;
      while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
      size_t numEnd = j;
      while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
      if (numEnd > numStart && j + 1 < s.size() && s[j] == ']' && s[j + 1] == ']') {
        int no = std::stoi(s.substr(numStart, numEnd - numStart));
        if (keep.count(no)) out += s.substr(i, j + 2 - i);
        i = j + 2;
        continue;
      }
    }
    out += s[i++];
  }
  return out;
}

// 返回清洗后的题目对象；无效题目返回 Null
inline JsonValue normalizeItem(const JsonValue& it) {
  if (!it.isObject()) return JsonValue::makeNull();
  std::string type = trim(it.getString("type"));
  if (!isKnownType(type)) return JsonValue::makeNull();

  JsonObject o = it.asObject();
  std::string word = trim(it.getString("word"));
  std::string stem = trim(it.getString("stem"));
  std::string question = trim(it.getString("question"));
  std::string passage = trim(it.getString("passage"));
  std::string prompt = trim(it.getString("prompt"));
  std::string ans = trim(it.getString("answer"));
  if (ans.empty()) ans = trim(it.getString("reference_answer"));
  if (ans.empty() && type == "sentence_imitation") ans = trim(it.getString("sample"));
  if (stem.empty()) stem = question;
  if (question.empty()) question = stem;

  // ---- 完整完形填空：短文 + 多空选择题 ----
  if (type == "full_cloze") {
    if (passage.empty()) return JsonValue::makeNull();
    JsonArray outQs;
    std::set<int> validNos;
    std::vector<std::pair<int, std::string>> ansPairs;
    int expectNo = 1;
    const JsonValue* qs = it.find("questions");
    if (qs && qs->isArray() && qs->asArray().size() >= 4) {
      for (auto& q : qs->asArray()) {
        if (!q.isObject()) continue;
        int no = (int)q.getInt("no", expectNo);
        JsonArray opts;
        if (!cleanOptions4(q, opts)) continue;
        if (isLetterBank(opts)) continue;
        std::string letter = letterOfAnswer(q.getString("answer"), opts);
        if (letter.empty()) continue;
        JsonObject qo;
        qo["no"] = JsonValue::makeInt(no);
        qo["options"] = JsonValue::makeArray(opts);
        qo["answer"] = JsonValue::makeString(letter);
        qo["explanation"] = JsonValue::makeString(q.getString("explanation"));
        outQs.push_back(JsonValue::makeObject(qo));
        validNos.insert(no);
        ansPairs.push_back({no, letter});
        expectNo = no + 1;
      }
    } else {
      // 兼容模型更易遵循的结构：options 放在每个空内部，options[0] 为正确答案
      const JsonValue* bl = it.find("blanks");
      if (!bl || !bl->isArray()) return JsonValue::makeNull();
      for (auto& b : bl->asArray()) {
        if (!b.isObject()) continue;
        int no = (int)b.getInt("no", expectNo);
        JsonArray opts;
        if (!cleanOptions4(b, opts)) continue;
        if (isLetterBank(opts)) continue;
        std::string answerText = trim(b.getString("answer"));
        if (answerText.find("[[") != std::string::npos) continue;
        std::string letter = letterOfAnswer(answerText, opts);
        if (letter.empty()) letter = "A";  // 约定 options[0] 是正确答案
        JsonObject qo;
        qo["no"] = JsonValue::makeInt(no);
        qo["options"] = JsonValue::makeArray(opts);
        qo["answer"] = JsonValue::makeString(letter);
        qo["explanation"] = JsonValue::makeString(b.getString("explanation"));
        outQs.push_back(JsonValue::makeObject(qo));
        validNos.insert(no);
        ansPairs.push_back({no, letter});
        expectNo = no + 1;
      }
    }
    if (outQs.size() < 4) return JsonValue::makeNull();
    if (stem.empty()) stem = "阅读下面短文，从每题所给的 A、B、C、D 四个选项中选出最佳选项。";
    o["stem"] = JsonValue::makeString(stem);
    o["question"] = JsonValue::makeString(stem);
    o["passage"] = JsonValue::makeString(replaceNumberedMarkers(removeOrphanMarkers(passage, validNos)));
    o["questions"] = JsonValue::makeArray(outQs);
    o["answer"] = JsonValue::makeString(joinAnswers(ansPairs));
    o["word"] = JsonValue::makeString(word);
    o["explanation"] = JsonValue::makeString(it.getString("explanation"));
    return JsonValue::makeObject(o);
  }

  // ---- 长篇阅读语法填空：保留 [[n]] 供页面换输入框 ----
  if (type == "long_reading_cloze") {
    const JsonValue* bl = it.find("blanks");
    if (!bl || !bl->isArray() || passage.empty()) return JsonValue::makeNull();
    JsonArray outBl;
    std::set<int> validNos;
    std::vector<std::pair<int, std::string>> ansPairs;
    int expectNo = 1;
    for (auto& b : bl->asArray()) {
      if (!b.isObject()) continue;
      int no = (int)b.getInt("no", expectNo);
      std::string ba = trim(b.getString("answer"));
      if (ba.empty() || ba.find("[[") != std::string::npos) continue;
      JsonObject bo;
      bo["no"] = JsonValue::makeInt(no);
      bo["answer"] = JsonValue::makeString(ba);
      std::string hint = trim(b.getString("hint"));
      if (!hint.empty()) bo["hint"] = JsonValue::makeString(hint);
      bo["explanation"] = JsonValue::makeString(b.getString("explanation"));
      outBl.push_back(JsonValue::makeObject(bo));
      validNos.insert(no);
      ansPairs.push_back({no, ba});
      expectNo = no + 1;
    }
    if (outBl.size() < 4) return JsonValue::makeNull();
    if (stem.empty()) stem = "根据短文内容，在空白处填入适当的词或所给词的正确形式。";
    o["stem"] = JsonValue::makeString(stem);
    o["question"] = JsonValue::makeString(stem);
    o["passage"] = JsonValue::makeString(removeOrphanMarkers(passage, validNos));
    o["blanks"] = JsonValue::makeArray(outBl);
    o["answer"] = JsonValue::makeString(joinAnswers(ansPairs));
    o["word"] = JsonValue::makeString(word);
    o["explanation"] = JsonValue::makeString(it.getString("explanation"));
    return JsonValue::makeObject(o);
  }

  // ---- 词义匹配 / 词汇辨析选择 ----
  if (isChoiceType(type)) {
    JsonArray opts;
    if (!cleanOptions4(it, opts)) return JsonValue::makeNull();
    if (isLetterBank(opts)) return JsonValue::makeNull();
    std::string letter = letterOfAnswer(ans, opts);
    if (letter.empty()) return JsonValue::makeNull();
    if (type == "cloze_choice") {
      if (passage.empty() && stem.empty()) return JsonValue::makeNull();
      stem = stem.empty() ? "" : singleChoiceBlank(stem);
      passage = passage.empty() ? "" : singleChoiceBlank(passage);
    } else {
      // 词义匹配无语境句时，用近义词选择题干兜底
      if (stem.empty() && passage.empty()) {
        if (word.empty()) return JsonValue::makeNull();
        stem = "请选出与 “" + word + "” 意思最接近的选项。";
      } else if (!stem.empty()) {
        stem = singleChoiceBlank(stem);
      }
      if (!passage.empty()) passage = singleChoiceBlank(passage);
      if (stem.empty() && !passage.empty()) {
        stem = "阅读短文，选出与句中目标词意思最接近的选项。";
      }
    }
    o["stem"] = JsonValue::makeString(stem);
    o["question"] = JsonValue::makeString(stem);
    if (!passage.empty()) o["passage"] = JsonValue::makeString(passage);
    else o.erase("passage");
    o["options"] = JsonValue::makeArray(opts);
    o["answer"] = JsonValue::makeString(letter);
    o["word"] = JsonValue::makeString(word);
    o["explanation"] = JsonValue::makeString(it.getString("explanation"));
    return JsonValue::makeObject(o);
  }

  // ---- 词性转换填空 ----
  if (type == "word_form_fill") {
    if (ans.empty()) return JsonValue::makeNull();
    std::string body = stem.empty() ? passage : stem;
    if (body.empty()) return JsonValue::makeNull();
    std::string out = fillBlankStem(body, word);
    if (out.find("______") == std::string::npos) return JsonValue::makeNull();
    o["stem"] = JsonValue::makeString(out);
    o["question"] = JsonValue::makeString(out);
    o["answer"] = JsonValue::makeString(ans);
    o["word"] = JsonValue::makeString(word);
    o["explanation"] = JsonValue::makeString(it.getString("explanation"));
    return JsonValue::makeObject(o);
  }

  // ---- 语境选词填空 ----
  if (type == "context_blank_fill") {
    if (ans.empty()) return JsonValue::makeNull();
    std::string body = stem.empty() ? passage : stem;
    if (body.empty()) return JsonValue::makeNull();
    std::string out = replaceMarkers(body, "______");
    if (out.find("______") == std::string::npos) return JsonValue::makeNull();
    o["stem"] = JsonValue::makeString(out);
    o["question"] = JsonValue::makeString(out);
    o["answer"] = JsonValue::makeString(ans);
    o["word"] = JsonValue::makeString(word);
    o["explanation"] = JsonValue::makeString(it.getString("explanation"));
    return JsonValue::makeObject(o);
  }

  // ---- 中译英 ----
  if (type == "cn_to_en_translation") {
    if (ans.empty()) return JsonValue::makeNull();
    std::string cn = prompt;
    if (cn.empty() && hasCjk(word) && !hasCjk(stem)) cn = word;
    if (cn.empty()) cn = stem;
    if (cn.empty()) cn = word;
    if (cn.empty()) return JsonValue::makeNull();
    if (stem.empty() || !hasCjk(stem)) stem = "请将下列中文翻译成英文：" + cn;
    o["prompt"] = JsonValue::makeString(cn);
    o["stem"] = JsonValue::makeString(stem);
    o["question"] = JsonValue::makeString(stem);
    o["answer"] = JsonValue::makeString(ans);
    o["word"] = JsonValue::makeString(word);
    o["explanation"] = JsonValue::makeString(it.getString("explanation"));
    return JsonValue::makeObject(o);
  }

  // ---- 例句仿写 ----
  if (type == "sentence_imitation") {
    if (ans.empty()) return JsonValue::makeNull();
    std::string body = prompt.empty() ? stem : prompt;
    if (body.empty()) body = passage;
    if (body.empty()) return JsonValue::makeNull();
    o["stem"] = JsonValue::makeString(body);
    o["question"] = JsonValue::makeString(body);
    o["prompt"] = JsonValue::makeString(prompt.empty() ? body : prompt);
    o["answer"] = JsonValue::makeString(ans);
    o["word"] = JsonValue::makeString(word);
    o["explanation"] = JsonValue::makeString(it.getString("explanation"));
    return JsonValue::makeObject(o);
  }

  return JsonValue::makeNull();
}

// 批量清洗：丢弃无效题，最多保留 cap 道
inline JsonValue normalizeItems(const JsonArray& arr, int cap) {
  JsonArray out;
  for (auto& it : arr) {
    JsonValue n = normalizeItem(it);
    if (!n.isObject()) continue;
    out.push_back(n);
    if ((int)out.size() >= cap) break;
  }
  JsonObject root;
  root["items"] = JsonValue::makeArray(out);
  return JsonValue::makeObject(root);
}

}  // namespace exsan
}  // namespace eng
