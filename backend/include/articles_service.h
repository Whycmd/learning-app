// 文章业务 Service（v1.3.0 文章模块）
// 职责：文章 CRUD、难度分析、级联清理
// 依赖：Db（数据库操作）、ArticlesVocabService（EFLLex 加载 + 难度分析）
#pragma once

#include "db.h"
#include "json.h"
#include "articles_vocab_service.h"
#include "auth.h"
#include <string>
#include <vector>
#include <map>

namespace eng {

class ArticlesService {
  Db& db_;
  ArticlesVocabService& vocabSvc_;
public:
  ArticlesService(Db& db, ArticlesVocabService& vocab)
    : db_(db), vocabSvc_(vocab) {}

  // ============ 文章 CRUD ============

  // 创建文章（自动运行难度分析 + 写入 article_sentences + article_vocabulary）
  struct CreateResult { int64_t id=0; bool ok=false; std::string error; };
  CreateResult create(const std::string& title,
                      const std::string& content,
                      const std::string& category = "other",
                      const std::string& cefr_level = "",  // 空则自动计算
                      const std::string& import_method = "text",
                      const std::string& source_url = "",
                      const std::string& source_name = "",
                      int user_id = 0);  // 0 则取 RequestContext

  // 列表（分页 + 筛选）
  // 返回 JsonObject：{"data":[...], "total":N, "page":N, "size":N}
  JsonValue list(int page = 1, int size = 20,
                 const std::string& category_filter = "",
                 const std::string& cefr_filter = "",
                 bool only_mine = false,
                 int user_id = 0);

  // 详情（含分句列表和词汇难度表）
  JsonValue detail(int64_t id, int user_id = 0);

  // 更新（只修改传入的非空字段）
  struct UpdateResult { bool ok=false; std::string error; };
  UpdateResult update(int64_t id, const JsonObject& fields, int user_id = 0);

  // 删除（级联清理 sentences/vocabulary/quizzes）
  UpdateResult remove(int64_t id, int user_id = 0);

  // 难度报告（含生词列表）
  JsonValue difficultyReport(int64_t id, int user_id = 0);

  // ============ 内部辅助 ============

  // 从文章内容自动分句（规则分句，不调 AI）
  // 结果写入 article_sentences 表
  int splitAndStoreSentences(int64_t article_id, const std::string& content);

  // 词汇分析：逐词查 EFLLex，写入 article_vocabulary 表
  // is_new_word 按当前用户 CEFR 等级判定
  int analyzeAndStoreVocabulary(int64_t article_id, const std::string& content, int user_id);

private:
  // 解析 CEFR 等级字符串（验证合法性）
  static bool isValidCefr(const std::string& s);

  // 归一化 category
  static std::string normalizeCategory(const std::string& c);

  // 归一化 import_method
  static std::string normalizeImportMethod(const std::string& m);

  // 简单英文分句：按 .!? 结尾 + 空格 + 大写开头切分
  // 处理缩写（Mr./Mrs./Dr./St./U.S./U.K. 等）
  static std::vector<std::string> simpleSplitSentences(const std::string& content);
};

} // namespace eng
