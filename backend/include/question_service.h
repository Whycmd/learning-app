// 出题与批改服务层（阶段四）
// 职责：AI 出题（6大题型）、分层出题、页数限制、习题 Word 生成、自动批改
// 铁律：习题不入库、不缓存、不复用、仅实时生成
#pragma once
#include "db.h"
#include "ai_client.h"
#include "knowledge_service.h"
#include "learning_service.h"
#include "json.h"
#include "prompts.h"
#include "article_excerpt.h"
#include "exercise_sanitize.h"
#include <utility>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <fstream>
#include <ctime>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <cctype>

namespace eng {

class QuestionService {
  Db& db_;
  AiClient& ai_;
  KnowledgeService& kpSvc_;
  LearningService& learnSvc_;
  std::string wordTmpDir_;
  std::string rootDir_;

public:
  QuestionService(Db& db, AiClient& ai, KnowledgeService& kp, LearningService& ls,
                  const std::string& wordTmp, const std::string& root)
    : db_(db), ai_(ai), kpSvc_(kp), learnSvc_(ls), wordTmpDir_(wordTmp), rootDir_(root) {}

  // 题型定义
  static const std::vector<std::string>& allTypes() {
    static const std::vector<std::string> types = {
      "word_match",          // 词义匹配综合题
      "word_form_fill",       // 词性转换填空
      "context_blank_fill",   // 语境选词填空
      "cn_to_en_translation", // 中译英翻译
      "cloze_choice",         // 词汇辨析完形选择
      "sentence_imitation",   // 例句仿写
      "long_reading_cloze",   // 长篇阅读语法填空
      "full_cloze"            // 完整完形填空
    };
    return types;
  }

  // 页数限制公式
  // 按日：max(1, min(2, ceil(kp_count * 0.5)))
  // 按周：max(3, min(5, ceil(kp_count * 1.0)))
  static int calcPageLimit(int kpCount, const std::string& mode) {
    int pages = (mode == "weekly")
      ? (int)std::ceil((double)kpCount * 1.0)
      : (int)std::ceil((double)kpCount * 0.5);
    if (mode == "weekly") return std::max(3, std::min(5, pages));
    return std::max(1, std::min(2, pages));
  }

  // 分层出题：根据 master_require 选择题型
  // 会读 → 重辨析理解: word_match, cloze_choice, context_blank_fill
  // 会写 → 重变形输出: word_form_fill, context_blank_fill, cn_to_en_translation, sentence_imitation
  static std::vector<std::string> selectTypesForMode(const std::string& mode, const std::string& masterRequire) {
    if (mode == "weekly") return allTypes(); // 按周全题型
    // 按日抽题型（基于掌握要求分层）
    if (masterRequire == "会读") {
      return {"word_match", "cloze_choice"};
    } else {
      return {"word_form_fill", "cn_to_en_translation"};
    }
  }

  struct GenerateResult {
    bool ok=false;
    std::string exerciseId;
    std::string wordPath;
    std::vector<int> knowledgeIds;
    std::string mode;
    int pageLimit=0;
    std::vector<std::string> exerciseTypes;
    JsonValue exercises;   // AI 生成的习题 JSON
    std::string error;
  };

  // 原子任务1+2：AI 出题
  GenerateResult generate(const std::vector<int>& inputIds, const std::string& mode) {
    GenerateResult r;
    r.mode = (mode == "weekly") ? "weekly" : "daily";

    if (inputIds.empty()) { r.error = "no knowledge points selected"; return r; }

    // v1.5.0-T005：出题只取手工录入的知识点，教材词(origin='wordlist')不参与
    std::vector<int> knowledgeIds = manualIds(inputIds);
    if (knowledgeIds.empty()) { r.error = "no manual knowledge points selected"; return r; }
    r.knowledgeIds = knowledgeIds;

    // 生成唯一习题 ID（时间戳 + 随机后缀）
    std::string exId = "ex_" + std::to_string((int64_t)::time(nullptr)) + "_" + std::to_string(knowledgeIds.size());

    // 加载知识点详情
    JsonArray kpArray;
    std::string masterReqs;
    for (int kpId : knowledgeIds) {
      auto d = kpSvc_.detail(kpId);
      if (d.isNull()) continue;
      kpArray.push_back(d);
      std::string mr = d.getString("master_require");
      if (!mr.empty()) { if (masterReqs.empty()) masterReqs = mr; }
    }
    if (kpArray.empty()) { r.error = "no solidified knowledge points found"; return r; }

    // 页数限制
    r.pageLimit = calcPageLimit((int)knowledgeIds.size(), r.mode);

    // 确定题型
    std::string primaryMaster = masterReqs.empty() ? "会写" : masterReqs;
    // 构造 AI 提示：使用 prompts.h 模板（注释与提示词内容统一在 prompts.h 维护）
    std::string modeCn = (r.mode == "weekly") ? "按周（全题型）" : "按日（分层抽题）";
    std::string articleExcerpt = pickArticleExcerpt(db_, RequestContext::currentUserId());
    if (articleExcerpt.empty()) articleExcerpt = "（站内暂无可用文章节选，请按下方兜底规则自选语篇。）";
    std::string userPrompt = prompts::buildTemplate(
      prompts::EXERCISE_GENERATE_USER,
      {
        {"{MODE_CN}",    modeCn},
        {"{MASTER_REQ}", primaryMaster},
        {"{PAGE_LIMIT}", std::to_string(r.pageLimit)},
        {"{LEARNER_IDENTITY}", prompts::getLearnerIdentity(db_, RequestContext::currentUserId())},
        {"{ARTICLE_EXCERPT}", articleExcerpt},
        {"{KP_JSON}",    JsonValue::makeArray(kpArray).dump()}
      }
    );

    std::vector<std::map<std::string,std::string>> msgs = {
      {{"role","system"},{"content",prompts::EXERCISE_GENERATE_SYSTEM}},
      {{"role","user"},{"content",userPrompt}}
    };

    // 调用 AI（异常兜底）
    auto resp = ai_.postChat(msgs, true, 2048, 0.4, "exercise_generate");
    if (!resp.ok) { r.error = "AI generation failed: " + resp.body; return r; }

    JsonValue aiJson;
    try {
      if (!resp.extractedJson.empty()) aiJson = parseJson(resp.extractedJson);
      else { r.error = "AI returned no JSON"; return r; }
    } catch (const std::exception& e) {
      r.error = std::string("JSON parse failed: ") + e.what();
      return r;
    }
    auto extractArray = [](const JsonValue& root) -> JsonArray {
      if (!root.isObject()) return {};
      for (const char* key : {"exercises", "questions", "items"}) {
        auto* v = root.find(key);
        if (!v) continue;
        if (v->isArray()) return v->asArray();
        if (v->isObject()) {
          for (const char* k2 : {"exercises", "questions", "items"}) {
            auto* v2 = v->find(k2);
            if (v2 && v2->isArray()) return v2->asArray();
          }
        }
      }
      return {};
    };

    // 统一清洗：无效题丢弃，选项前缀与空位标记规范化
    JsonArray raw = aiJson.isArray() ? aiJson.asArray() : extractArray(aiJson);
    JsonArray cleaned;
    for (auto& ex : raw) {
      JsonValue n = exsan::normalizeItem(ex);
      if (n.isObject()) cleaned.push_back(n);
    }

    // 无效题按缺失数量补生成 1 轮
    if (!raw.empty() && cleaned.size() < raw.size()) {
      int missing = (int)raw.size() - (int)cleaned.size();
      std::string supplement = userPrompt +
        "\n\n【补充要求】上一轮有 " + std::to_string(missing) +
        " 道题未通过格式校验，请只补充生成 " + std::to_string(missing) + " 道有效题目，题型不要与已有效题目重复。";
      std::vector<std::map<std::string,std::string>> msgs2 = {
        {{"role","system"},{"content",prompts::EXERCISE_GENERATE_SYSTEM}},
        {{"role","user"},{"content",supplement}}
      };
      auto resp2 = ai_.postChat(msgs2, true, 2048, 0.4, "exercise_generate");
      if (resp2.ok && !resp2.extractedJson.empty()) {
        try {
          JsonValue j2 = parseJson(resp2.extractedJson);
          JsonArray raw2 = j2.isArray() ? j2.asArray() : extractArray(j2);
          for (auto& ex : raw2) {
            if (cleaned.size() >= raw.size()) break;
            JsonValue n = exsan::normalizeItem(ex);
            if (n.isObject()) cleaned.push_back(n);
          }
        } catch (...) {}
      }
    }

    if (cleaned.empty()) { r.error = "AI 返回题目格式无效"; return r; }

    JsonObject root = aiJson.isObject() ? aiJson.asObject() : JsonObject{};
    root["exercises"] = JsonValue::makeArray(cleaned);
    if (cleaned.size() < raw.size()) {
      root["notice"] = JsonValue::makeString("部分题目生成失败，已保留有效题目，可重新生成。");
    }
    r.exercises = JsonValue::makeObject(root);

    // v1.5.0-T009 D07：出题只返回题目，Word 改为点导出时生成
    r.ok = true;
    r.exerciseId = exId;

    std::set<std::string> typeSet;
    for (auto& ex : cleaned) {
      auto* t = ex.find("type");
      if (t && t->isString() && !t->asString().empty()) typeSet.insert(t->asString());
    }
    for (auto& t : typeSet) r.exerciseTypes.push_back(t);

    if (r.exerciseTypes.empty()) {
      if (r.mode == "weekly") {
        r.exerciseTypes = {"word_match","word_form_fill","context_blank_fill","cn_to_en_translation",
                           "cloze_choice","sentence_imitation","long_reading_cloze","full_cloze"};
      } else {
        r.exerciseTypes = {"word_match","word_form_fill","context_blank_fill"};
      }
    }

    return r;
  }

  // v1.5.0-T009 D07/D08：复习卷按需导出（空白卷，不调 AI）
  struct ExportResult {
    bool ok=false;
    int httpStatus=500;
    std::string path, error;
  };

  ExportResult exportPaper(const JsonValue& payload) {
    ExportResult r;
    if (!payload.isObject()) { r.httpStatus = 400; r.error = "invalid payload"; return r; }
    const JsonValue* arr = payload.find("exercises");
    if (!arr || !arr->isArray() || arr->asArray().empty()) {
      r.httpStatus = 400; r.error = "exercises required"; return r;
    }
    JsonObject o = payload.asObject();
    o["kind"] = JsonValue::makeString("review");

    std::string exId = "ex_" + std::to_string((int64_t)::time(nullptr));
    std::string tmpPath = wordTmpDir_ + "/export_paper_" + exId + ".json";
    {
      std::ofstream f(tmpPath);
      if (!f) { r.error = "export temp write failed"; return r; }
      f << JsonValue::makeObject(o).dump();
    }

    std::string scriptPath = rootDir_ + "/scripts/gen_word_exercise.js";
    const char* nodeEnv = std::getenv("NODE_BIN");
    std::string nodeBin = nodeEnv ? nodeEnv : "node";
    std::string cmd = nodeBin + " " + scriptPath + " " + exId + " " + tmpPath + " " + wordTmpDir_;
    int rc = std::system(cmd.c_str());
    if (rc != 0) { r.error = "export service unavailable"; return r; }

    std::string outPath = wordTmpDir_ + "/exercise_" + exId + ".docx";
    if (::access(outPath.c_str(), F_OK) != 0) { r.error = "export file not found"; return r; }
    r.ok = true;
    r.path = outPath;
    return r;
  }

  // 原子任务4：自动批改
  // 客观题：本地代码比对；主观题：AI 批改
  struct GradeResult {
    bool ok=false;
    int score=0;
    bool correct=false;
    std::string errorAnalysis;
    std::string referenceAnswer;
    bool isObjective=false;
    std::string error;
  };

  // 批改单题
  // type: 题型，userAnswer: 用户答案，correctAnswer: 正确答案（客观题），referenceAnswer: 参考答案（主观题）
  GradeResult grade(const std::string& type, const std::string& userAnswer,
                    const std::string& correctAnswer, const std::string& referenceAnswer) {
    GradeResult r;
    if (userAnswer.empty()) { r.error = "empty answer"; return r; }

    // 长篇阅读语法填空 / 完整完形填空：多空数组逐空比对（本地批改）
    if (type == "long_reading_cloze" || type == "full_cloze") {
      r.isObjective = true;
      std::vector<std::string> gotVals, refVals;
      try {
        JsonValue gu = parseJson(userAnswer);
        JsonValue gr = parseJson(correctAnswer);
        if (gu.isArray()) for (auto& x : gu.asArray()) gotVals.push_back(x.isString() ? x.asString() : "");
        if (gr.isArray()) for (auto& x : gr.asArray()) refVals.push_back(x.isString() ? x.asString() : "");
      } catch (...) {}
      if (refVals.empty()) { r.error = "missing blanks answer"; return r; }
      auto normBlank = [](const std::string& s) {
        std::string out;
        for (char c : s) {
          if (c == '"' || c == '\'' || c == '.' || c == ',' || c == ';') continue;
          if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (!out.empty() && out.back() != ' ') out += ' '; continue; }
          out += (char)std::tolower((unsigned char)c);
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
      };
      int correctCount = 0;
      std::string wrongNos, allRefs;
      for (size_t i = 0; i < refVals.size(); ++i) {
        std::string got = i < gotVals.size() ? gotVals[i] : "";
        std::string ref = refVals[i];
        bool ok = !normBlank(got).empty() && normBlank(got) == normBlank(ref);
        if (ok) correctCount++;
        else {
          if (!wrongNos.empty()) wrongNos += "、";
          wrongNos += std::to_string(i + 1);
        }
        allRefs += (allRefs.empty() ? "" : "  ") + std::to_string(i + 1) + ". " + ref;
      }
      r.ok = true;
      r.score = (int)(correctCount * 100 / (int)refVals.size());
      r.correct = correctCount == (int)refVals.size();
      r.errorAnalysis = r.correct ? "全部填对。" : ("第 " + wrongNos + " 空错误；正确答案：" + allRefs);
      r.referenceAnswer = allRefs;
      return r;
    }

    // 客观题（本地比对）
    if (type == "word_match" || type == "cloze_choice") {
      r.isObjective = true;
      std::string ua = toUpper(trim(userAnswer));
      std::string ca = toUpper(trim(correctAnswer));
      // 支持选项字母（A/B/C/D）或文本匹配
      if (ua == ca) {
        r.ok = true; r.score = 100; r.correct = true;
        r.errorAnalysis = "回答正确。";
      } else {
        r.ok = true; r.score = 0; r.correct = false;
        r.errorAnalysis = "回答错误，正确答案为 " + correctAnswer + "。";
      }
      r.referenceAnswer = correctAnswer;
      return r;
    }

    if (type == "word_form_fill" || type == "context_blank_fill") {
      r.isObjective = true;
      std::string ua = toLower(trim(userAnswer));
      std::string ca = toLower(trim(correctAnswer));
      if (ua == ca) {
        r.ok = true; r.score = 100; r.correct = true;
        r.errorAnalysis = "填空正确。";
      } else {
        r.ok = true; r.score = 0; r.correct = false;
        r.errorAnalysis = "填空错误，正确答案为 " + correctAnswer + "。";
      }
      r.referenceAnswer = correctAnswer;
      return r;
    }

    // 主观题（AI 批改）：使用 prompts.h 模板（注释与提示词内容统一在 prompts.h 维护）
    std::string refAns = referenceAnswer.empty() ? "(无固定标准答案)" : referenceAnswer;
    std::string userPrompt = prompts::buildTemplate(
      prompts::SUBJECTIVE_GRADE_USER,
      {
        {"{TYPE}",     type},
        {"{USER_ANS}", userAnswer},
        {"{REF_ANS}",  refAns},
        {"{LEARNER_IDENTITY}", prompts::getLearnerIdentity(db_, RequestContext::currentUserId())},
      }
    );

    std::vector<std::map<std::string,std::string>> msgs = {
      {{"role","system"},{"content",prompts::SUBJECTIVE_GRADE_SYSTEM}},
      {{"role","user"},{"content",userPrompt}}
    };

    auto resp = ai_.postChat(msgs, true, 512, 0.2, "grade");
    if (!resp.ok) { r.error = "AI grading failed: " + resp.body; return r; }

    JsonValue json;
    try {
      if (!resp.extractedJson.empty()) json = parseJson(resp.extractedJson);
      else { r.error = "AI returned no JSON"; return r; }
    } catch (const std::exception& e) {
      r.error = std::string("JSON parse failed: ") + e.what();
      return r;
    }

    r.ok = true;
    r.score = (int)json.getInt("score", 60);
    r.correct = json.getBool("correct", false);
    r.errorAnalysis = json.getString("error_analysis");
    r.referenceAnswer = json.getString("reference_answer", referenceAnswer);
    r.isObjective = false;
    return r;
  }

  // 批量批改并同步学情
  struct BatchGradeResult {
    bool ok=false;
    int totalScore=0;
    int correctCount=0;
    int totalCount=0;
    std::vector<std::map<std::string,std::string>> details;
    std::string error;
  };

  // answers: [{type, user_answer, correct_answer, reference_answer, knowledge_id}]
  BatchGradeResult batchGrade(const JsonValue& answers) {
    BatchGradeResult r;
    if (!answers.isArray()) { r.error = "answers must be array"; return r; }

    r.totalCount = (int)answers.asArray().size();
    int totalScore = 0;
    std::map<int, bool> kpCorrect; // 按知识点汇总是否全对

    for (auto& ans : answers.asArray()) {
      std::string type = ans.getString("type");
      std::string userAns = ans.getString("user_answer");
      std::string correctAns = ans.getString("correct_answer");
      std::string refAns = ans.getString("reference_answer");
      int kpId = (int)ans.getInt("knowledge_id", 0);
      if (type == "long_reading_cloze" || type == "full_cloze") {
        if (const JsonValue* ua = ans.find("user_answers")) { if (ua->isArray()) userAns = ua->dump(); }
        if (const JsonValue* ca = ans.find("correct_answers")) { if (ca->isArray()) correctAns = ca->dump(); }
      }

      auto gr = grade(type, userAns, correctAns, refAns);
      std::map<std::string,std::string> d;
      d["type"] = type;
      d["knowledge_id"] = std::to_string(kpId);
      d["score"] = std::to_string(gr.score);
      d["correct"] = gr.correct ? "true" : "false";
      d["is_objective"] = gr.isObjective ? "true" : "false";
      d["error_analysis"] = gr.errorAnalysis;
      d["reference_answer"] = gr.referenceAnswer;
      r.details.push_back(d);

      totalScore += gr.score;
      if (gr.correct) r.correctCount++;
      if (kpId > 0) {
        if (kpCorrect.find(kpId) == kpCorrect.end()) kpCorrect[kpId] = true;
        if (!gr.correct) kpCorrect[kpId] = false;
      }

      // 同步学情：答题结果
      if (kpId > 0 && gr.ok) {
        learnSvc_.onAnswerResult(kpId, gr.correct);
      }
    }

    r.ok = true;
    r.totalScore = r.totalCount > 0 ? (totalScore / r.totalCount) : 0;
    return r;
  }

private:
  // v1.5.0-T005：过滤出当前用户手工录入的知识点 id
  std::vector<int> manualIds(const std::vector<int>& ids) {
    std::vector<int> out;
    if (ids.empty()) return out;
    std::ostringstream in;
    for (size_t i = 0; i < ids.size(); ++i) { if (i) in << ","; in << ids[i]; }
    auto rows = db_.query(
      "SELECT id FROM knowledge_points WHERE user_id=" + std::to_string(RequestContext::currentUserId())
      + " AND origin='manual' AND id IN (" + in.str() + ")");
    for (auto& row : rows) out.push_back(std::stoi(row["id"]));
    return out;
  }

  static std::string toUpper(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::toupper);
    return r;
  }
  static std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
  }
  static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
  }
};

} // namespace eng
