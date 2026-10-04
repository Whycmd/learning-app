// EFLLex CEFR 词频服务实现
#include "../include/articles_vocab_service.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <iostream>

namespace eng {

// ============ 工具函数 ============

int cefrCompare(const std::string& a, const std::string& b) {
  auto rank = ArticlesVocabService::cefrRank;
  int ra = rank(a), rb = rank(b);
  if (ra < 0 || rb < 0) return -2;  // 有未知
  return (ra < rb) ? -1 : (ra > rb) ? 1 : 0;
}

// ============ 静态辅助 ============

int ArticlesVocabService::cefrRank(const std::string& level) {
  static const std::unordered_map<std::string, int> ranks = {
    {"A1", 0}, {"A2", 1}, {"B1", 2}, {"B2", 3}, {"C1", 4}
  };
  auto it = ranks.find(level);
  return it == ranks.end() ? -1 : it->second;
}

std::string ArticlesVocabService::normalize(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '\'' || c == '-' || c == '_') {
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  // 去掉首尾的 ' - _
  while (!out.empty() && (out.front() == '\'' || out.front() == '-' || out.front() == '_'))
    out.erase(out.begin());
  while (!out.empty() && (out.back() == '\'' || out.back() == '-' || out.back() == '_'))
    out.pop_back();
  return out;
}

bool ArticlesVocabService::isLearningTag(const std::string& tag) {
  // Penn Treebank 标签前缀匹配：名词/动词/形容词/副词
  if (tag.rfind("NN", 0) == 0) return true;  // NN/NNS/NNP/NNPS
  if (tag.rfind("VB", 0) == 0) return true;  // VB/VBD/VBG/VBN/VBP/VBZ
  if (tag.rfind("JJ", 0) == 0) return true;  // JJ/JJR/JJS
  if (tag.rfind("RB", 0) == 0) return true;  // RB/RBR/RBS
  return false;
}

std::vector<std::string> ArticlesVocabService::splitTab(const std::string& line, size_t max_cols) {
  std::vector<std::string> result;
  result.reserve(max_cols);
  std::string cur;
  for (char c : line) {
    if (c == '\t') {
      if (result.size() + 1 >= max_cols) {
        // 剩余内容合并到最后一列，不再继续分割
        cur += c;
      } else {
        result.push_back(std::move(cur));
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  result.push_back(std::move(cur));
  while (result.size() < max_cols) result.push_back("");
  return result;
}

// ============ 加载 ============

ArticlesVocabService::ArticlesVocabService() = default;
ArticlesVocabService::~ArticlesVocabService() = default;

int ArticlesVocabService::loadEfllEx(const std::string& tsv_path) {
  std::ifstream fin(tsv_path);
  if (!fin) {
    std::cerr << "[articles_vocab] cannot open " << tsv_path << std::endl;
    return 0;
  }

  std::lock_guard<std::mutex> lock(mu_);
  word_cefr_map_.clear();

  std::string line;
  int line_no = 0;
  int loaded = 0;
  bool first = true;

  while (std::getline(fin, line)) {
    line_no++;
    if (line.empty()) continue;

    // 跳过表头
    if (first) { first = false; continue; }

    // 只取前 7 列
    auto cols = splitTab(line, 7);
    if (cols.size() < 7) continue;

    std::string raw_word = cols[0];
    std::string tag      = cols[1];
    std::string f_a1     = cols[2];
    std::string f_a2     = cols[3];
    std::string f_b1     = cols[4];
    std::string f_b2     = cols[5];
    std::string f_c1     = cols[6];

    // 词性过滤：只保留 NN/VB/JJ/RB 前缀
    if (!isLearningTag(tag)) continue;

    // 归一化单词
    std::string norm = normalize(raw_word);
    if (norm.empty()) continue;

    // 按 A1→C1 顺序取首个词频 > 0 的等级
    // 词频可能是浮点数（如 0.5559），只要 > 0.01 就算有意义
    auto firstPositive = [](const std::vector<std::string>& freqs) -> std::string {
      static const char* LEVELS[] = {"A1", "A2", "B1", "B2", "C1"};
      for (size_t i = 0; i < freqs.size() && i < 5; i++) {
        try {
          double f = std::stod(freqs[i]);
          if (f > 0.01) return LEVELS[i];
        } catch (...) {}
      }
      return "";  // 所有等级词频都是 0
    };

    std::string level = firstPositive({f_a1, f_a2, f_b1, f_b2, f_c1});
    if (level.empty()) continue;

    // 如果已存在该词，取更低（更保守）的等级
    auto it = word_cefr_map_.find(norm);
    if (it == word_cefr_map_.end()) {
      word_cefr_map_[norm] = level;
      loaded++;
    } else {
      // 保留更低的等级（A1 < B1）
      if (cefrRank(level) < cefrRank(it->second)) {
        it->second = level;
      }
    }
  }

  loaded_ = true;
  std::cerr << "[articles_vocab] loaded " << loaded << " entries from " << tsv_path << std::endl;
  return loaded;
}

// ============ 查询 ============

std::string ArticlesVocabService::getWordCefrLevel(const std::string& word) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::string norm = normalize(word);
  if (norm.empty()) return "";

  auto it = word_cefr_map_.find(norm);
  if (it != word_cefr_map_.end()) return it->second;

  // 复数/时态简化查询（running → run, better → good）
  // 只做简单的后缀剥离，覆盖最常见的不规则形式
  // 这是轻量级回退——大多数复数/时态在 EFLLex 里已有独立条目
  static const std::vector<std::pair<std::string, std::string>> IRREGULAR = {
    {"running", "run"}, {"ran", "run"}, {"runs", "run"},
    {"went", "go"}, {"gone", "go"}, {"goes", "go"},
    {"did", "do"}, {"done", "do"}, {"does", "do"},
    {"was", "be"}, {"were", "be"}, {"been", "be"}, {"being", "be"}, {"am", "be"}, {"are", "be"}, {"is", "be"},
    {"made", "make"}, {"makes", "make"}, {"making", "make"},
    {"took", "take"}, {"taken", "take"}, {"taking", "take"}, {"takes", "take"},
    {"said", "say"}, {"says", "say"}, {"saying", "say"},
    {"got", "get"}, {"gotten", "get"}, {"gets", "get"}, {"getting", "get"},
    {"came", "come"}, {"comes", "come"}, {"coming", "come"},
    {"knew", "know"}, {"known", "know"}, {"knows", "know"}, {"knowing", "know"},
    {"thought", "think"}, {"thinks", "think"}, {"thinking", "think"},
    {"saw", "see"}, {"seen", "see"}, {"sees", "see"}, {"seeing", "see"},
    {"gave", "give"}, {"given", "give"}, {"gives", "give"}, {"giving", "give"},
    {"found", "find"}, {"finds", "find"}, {"finding", "find"},
    {"told", "tell"}, {"tells", "tell"}, {"telling", "tell"},
    {"became", "become"}, {"becomes", "become"}, {"becoming", "become"},
    {"left", "leave"}, {"leaves", "leave"}, {"leaving", "leave"},
    {"meant", "mean"}, {"means", "mean"}, {"meaning", "mean"},
    {"met", "meet"}, {"meets", "meet"}, {"meeting", "meet"},
    {"paid", "pay"}, {"pays", "pay"}, {"paying", "pay"},
    {"brought", "bring"}, {"brings", "bring"}, {"bringing", "bring"},
    {"bought", "buy"}, {"buys", "buy"}, {"buying", "buy"},
    {"built", "build"}, {"builds", "build"}, {"building", "build"},
    {"fell", "fall"}, {"fallen", "fall"}, {"falls", "fall"}, {"falling", "fall"},
    {"felt", "feel"}, {"feels", "feel"}, {"feeling", "feel"},
    {"fought", "fight"}, {"fights", "fight"}, {"fighting", "fight"},
    {"forgot", "forget"}, {"forgotten", "forget"}, {"forgets", "forget"}, {"forgetting", "forget"},
    {"taught", "teach"}, {"teaches", "teach"}, {"teaching", "teach"},
    {"caught", "catch"}, {"catches", "catch"}, {"catching", "catch"},
    {"chose", "choose"}, {"chosen", "choose"}, {"chooses", "choose"}, {"choosing", "choose"},
    {"drew", "draw"}, {"drawn", "draw"}, {"draws", "draw"}, {"drawing", "draw"},
    {"drove", "drive"}, {"driven", "drive"}, {"drives", "drive"}, {"driving", "drive"},
    {"wore", "wear"}, {"worn", "wear"}, {"wears", "wear"}, {"wearing", "wear"},
    {"won", "win"}, {"wins", "win"}, {"winning", "win"},
    {"wrote", "write"}, {"written", "write"}, {"writes", "write"}, {"writing", "write"},
    {"spoke", "speak"}, {"spoken", "speak"}, {"speaks", "speak"}, {"speaking", "speak"},
    {"broke", "break"}, {"broken", "break"}, {"breaks", "break"}, {"breaking", "break"},
    {"stole", "steal"}, {"stolen", "steal"}, {"steals", "steal"}, {"stealing", "steal"},
    {"threw", "throw"}, {"thrown", "throw"}, {"throws", "throw"}, {"throwing", "throw"},
    {"flew", "fly"}, {"flown", "fly"}, {"flies", "fly"}, {"flying", "fly"},
    {"grew", "grow"}, {"grown", "grow"}, {"grows", "grow"}, {"growing", "grow"},
    {"shook", "shake"}, {"shaken", "shake"}, {"shakes", "shake"}, {"shaking", "shake"},
    {"swam", "swim"}, {"swum", "swim"}, {"swims", "swim"}, {"swimming", "swim"},
    {"ate", "eat"}, {"eaten", "eat"}, {"eats", "eat"}, {"eating", "eat"},
    {"drank", "drink"}, {"drunk", "drink"}, {"drinks", "drink"}, {"drinking", "drink"},
    {"sang", "sing"}, {"sung", "sing"}, {"sings", "sing"}, {"singing", "sing"},
    {"rode", "ride"}, {"ridden", "ride"}, {"rides", "ride"}, {"riding", "ride"},
    {"wept", "weep"}, {"weeps", "weep"}, {"weeping", "weep"},
    {"said", "say"}, {"says", "say"}, {"saying", "say"},
    // 常见比较级/最高级简化
    {"better", "good"}, {"best", "good"},
    {"worse", "bad"}, {"worst", "bad"},
    {"more", "much"}, {"most", "much"},
    {"less", "little"}, {"least", "little"},
    {"further", "far"}, {"furthest", "far"},
    {"farther", "far"}, {"farthest", "far"},
    // 常见名词复数简化（简化：去掉 s/es）
    {"children", "child"}, {"men", "man"}, {"women", "woman"},
    {"teeth", "tooth"}, {"feet", "foot"}, {"geese", "goose"},
    {"mice", "mouse"}, {"lice", "louse"}, {"oxen", "ox"},
    {"people", "person"}, {"phenomena", "phenomenon"},
    {"criteria", "criterion"}, {"media", "medium"},
    {"data", "datum"}, {"bacteria", "bacterium"},
    // 简单规则剥离（以 ing/ed/es 结尾）
  };

  // 先查不规则表
  for (auto& pair : IRREGULAR) {
    if (norm == pair.first) {
      auto it2 = word_cefr_map_.find(pair.second);
      if (it2 != word_cefr_map_.end()) return it2->second;
    }
  }

  // 简单规则剥离：复数/时态后缀
  if (norm.size() > 4) {
    if (norm.size() > 3 && norm.back() == 's' && norm[norm.size()-2] != 's') {
      auto it3 = word_cefr_map_.find(norm.substr(0, norm.size()-1));
      if (it3 != word_cefr_map_.end()) return it3->second;
    }
    if (norm.size() > 4 && norm.compare(norm.size()-2, 2, "ed") == 0) {
      auto it3 = word_cefr_map_.find(norm.substr(0, norm.size()-2));
      if (it3 != word_cefr_map_.end()) return it3->second;
    }
    if (norm.size() > 4 && norm.compare(norm.size()-3, 3, "ing") == 0) {
      auto it3 = word_cefr_map_.find(norm.substr(0, norm.size()-3));
      if (it3 != word_cefr_map_.end()) return it3->second;
      // running → run 的特殊情况（双写末尾字母）
      if (norm.size() > 5) {
        auto it4 = word_cefr_map_.find(norm.substr(0, norm.size()-4));
        if (it4 != word_cefr_map_.end()) return it4->second;
      }
    }
  }

  return "";  // 未知
}

size_t ArticlesVocabService::vocabCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return word_cefr_map_.size();
}

// ============ 难度分析 ============

DifficultyReport ArticlesVocabService::analyzeDifficulty(const std::string& content,
                                                         const std::string& user_cefr) const {
  DifficultyReport report;

  if (!loaded_) {
    report.error = "EFLLex not loaded";
    return report;
  }

  // 分词：按非字母字符分割
  std::unordered_map<std::string, int> freq;
  std::string cur;
  int total = 0;
  for (char c : content) {
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '\'' || c == '-' || c == '_') {
      cur += c;
    } else {
      if (cur.size() >= 2) {
        std::string norm = normalize(cur);
        if (!norm.empty()) {
          freq[norm]++;
          total++;
        }
      }
      cur.clear();
    }
  }
  if (cur.size() >= 2) {
    std::string norm = normalize(cur);
    if (!norm.empty()) {
      freq[norm]++;
      total++;
    }
  }

  report.total_words = total;
  report.unique_words = (int)freq.size();

  // 逐词查 CEFR
  for (auto& pair : freq) {
    std::string level = getWordCefrLevel(pair.first);
    if (!level.empty()) {
      report.vocab_cefr[pair.first] = level;
      report.level_counts[level] += pair.second;
    }
    // level 为空表示不在 NN/VB/JJ/RB 范围内或 EFLLex 里没有，不统计到难度分布
  }

  // 判定生词和边缘词
  int user_rank = cefrRank(user_cefr);
  for (auto& pair : report.vocab_cefr) {
    int wr = cefrRank(pair.second);
    if (wr < 0) continue;
    if (wr > user_rank + 1) {
      report.new_words.push_back(pair.first);
    } else if (wr == user_rank + 1) {
      report.edge_words.push_back(pair.first);
    }
  }

  // 文章整体 CEFR 等级：众数（哪个等级出现次数最多）
  std::string mode_level;
  int max_count = 0;
  static const char* ORDER[] = {"A1", "A2", "B1", "B2", "C1"};
  for (auto lv : ORDER) {
    auto it = report.level_counts.find(lv);
    if (it != report.level_counts.end() && it->second > max_count) {
      max_count = it->second;
      mode_level = lv;
    }
  }
  // 如果没有任何词命中 EFLLex（极少见），默认 B1
  report.article_cefr = mode_level.empty() ? "B1" : mode_level;

  return report;
}

} // namespace eng
