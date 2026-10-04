// EFLLex CEFR 词频服务（v1.3.0 文章模块）
// 职责：启动时加载 backend/data/efllex.tsv 到内存哈希表，O(1) 查询单词 CEFR 等级
// 难度分析：统计文章词汇的 CEFR 分布，判定生词列表
// 设计：.h 声明 + .cpp 实现，避免头文件臃肿（15,000 词条哈希表 + 文件 I/O）
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace eng {

// CEFR 等级比较：A1 < A2 < B1 < B2 < C1
// 返回 -1/0/1 或 -2（未知等级）
int cefrCompare(const std::string& a, const std::string& b);

// 难度分析结果
struct DifficultyReport {
  int total_words = 0;             // 文章总词数（去重前）
  int unique_words = 0;            // 去重后词数
  std::unordered_map<std::string, int> level_counts;  // 各 CEFR 等级词数
  std::unordered_map<std::string, std::string> vocab_cefr;    // 单词→CEFR（归一化小写）
  std::vector<std::string> new_words;       // 生词列表（cefr > user_cefr+1）
  std::vector<std::string> edge_words;      // 边缘词列表（cefr == user_cefr+1）
  std::string article_cefr;        // 文章整体 CEFR 等级（众数）
  std::string error;               // 错误信息（空=成功）
};

class ArticlesVocabService {
public:
  ArticlesVocabService();
  ~ArticlesVocabService();

  // 加载 EFLLex TSV 文件到内存（返回实际加载的词条数）
  // 启动时调用一次，线程安全
  int loadEfllEx(const std::string& tsv_path);

  // 查询单词的 CEFR 等级（返回 "" 表示未知）
  // 输入可混合大小写，内部归一化小写
  std::string getWordCefrLevel(const std::string& word) const;

  // 对文章内容做难度分析
  // content: 文章正文
  // user_cefr: 用户 CEFR 等级（如 "B1"），用于生词判定
  DifficultyReport analyzeDifficulty(const std::string& content,
                                     const std::string& user_cefr = "B1") const;

  // 获取已加载的词条数（健康检查用）
  size_t vocabCount() const;

  // CEFR 等级排序（A1=0, A2=1, B1=2, B2=3, C1=4, -1=未知）
  static int cefrRank(const std::string& level);

  // 归一化单词：转小写、去除首尾标点、保留中间字母和连字符（public 供 Service 层调用）
  static std::string normalize(const std::string& raw);

  // 判断词性是否为学习关注的类型（前缀匹配 Penn Treebank 标签）
  // 保留 NN*（名词）/VB*（动词）/JJ*（形容词）/RB*（副词）
  static bool isLearningTag(const std::string& tag);

  // TSV 行 split（按 tab 分割，取前 n 列）
  static std::vector<std::string> splitTab(const std::string& line, size_t max_cols);

  // 内部存储
  std::unordered_map<std::string, std::string> word_cefr_map_;  // 小写单词 → CEFR
  mutable std::mutex mu_;                                        // 查询也加锁（防御未来并发）
  bool loaded_ = false;
};

} // namespace eng
