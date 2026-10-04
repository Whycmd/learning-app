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
#include <utility>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <fstream>
#include <ctime>
#include <unistd.h>
#include <algorithm>

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

  // 6 大题型定义
  static const std::vector<std::string>& allTypes() {
    static const std::vector<std::string> types = {
      "word_match",          // 词义匹配综合题
      "word_form_fill",       // 词性转换填空
      "context_blank_fill",   // 语境选词填空
      "cn_to_en_translation", // 中译英翻译
      "cloze_choice",         // 词汇辨析完形选择
      "sentence_imitation"    // 例句仿写
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
  GenerateResult generate(const std::vector<int>& knowledgeIds, const std::string& mode) {
    GenerateResult r;
    r.knowledgeIds = knowledgeIds;
    r.mode = (mode == "weekly") ? "weekly" : "daily";

    if (knowledgeIds.empty()) { r.error = "no knowledge points selected"; return r; }

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
    std::string userPrompt = prompts::buildTemplate(
      prompts::EXERCISE_GENERATE_USER,
      {
        {"{MODE_CN}",    modeCn},
        {"{MASTER_REQ}", primaryMaster},
        {"{PAGE_LIMIT}", std::to_string(r.pageLimit)},
        {"{LEARNER_IDENTITY}", prompts::getLearnerIdentity(db_, RequestContext::currentUserId())},
        {"{KP_JSON}",    JsonValue::makeArray(kpArray).dump()}
      }
    );

    std::vector<std::map<std::string,std::string>> msgs = {
      {{"role","system"},{"content",prompts::EXERCISE_GENERATE_SYSTEM}},
      {{"role","user"},{"content",userPrompt}}
    };

    // 调用 AI（异常兜底）
    auto resp = ai_.postChat(msgs, true, 2048, 0.4);
    if (!resp.ok) { r.error = "AI generation failed: " + resp.body; return r; }

    JsonValue aiJson;
    try {
      if (!resp.extractedJson.empty()) aiJson = parseJson(resp.extractedJson);
      else { r.error = "AI returned no JSON"; return r; }
    } catch (const std::exception& e) {
      r.error = std::string("JSON parse failed: ") + e.what();
      return r;
    }
    r.exercises = aiJson;

    // 生成 Word 文档
    std::string jsonStr = aiJson.dump();
    // 补充元信息
    JsonObject metaObj;
    if (aiJson.isObject()) {
      metaObj = aiJson.asObject();
    }
    metaObj["mode"] = JsonValue::makeString(r.mode);
    JsonArray kpMeta;
    for (auto& kp : kpArray) kpMeta.push_back(kp);
    metaObj["knowledge_points"] = JsonValue::makeArray(kpMeta);
    jsonStr = JsonValue::makeObject(metaObj).dump();

    std::string tmpPath = wordTmpDir_ + "/ex_input_" + exId + ".json";
    std::ofstream f(tmpPath);
    f << jsonStr;
    f.close();

    std::string scriptPath = rootDir_ + "/scripts/gen_word_exercise.js";
    std::string cmd = "/Users/junwang/.local/bin/node " + scriptPath + " " + exId + " " + tmpPath + " " + wordTmpDir_;
    int rc = std::system(cmd.c_str());
    if (rc != 0) { r.error = "word generation failed (exit=" + std::to_string(rc) + ")"; return r; }

    std::string outPath = wordTmpDir_ + "/exercise_" + exId + ".docx";
    if (::access(outPath.c_str(), F_OK) != 0) { r.error = "word file not found after generation"; return r; }

    r.ok = true;
    r.exerciseId = exId;
    r.wordPath = outPath;

    // 从 AI 返回的 exercises 里提取题型（去重）
    // AI 返回的可能是 {"exercises":[...]} 或 {"exercises":{"exercises":[...]}} 嵌套
    // 且每道题的 type 字段可能叫 "type"、"exercise_type" 等
    std::set<std::string> typeSet;
    auto collectFromArray = [&](const JsonValue& arr) {
      if (!arr.isArray()) return;
      for (auto& ex : arr.asArray()) {
        if (!ex.isObject()) continue;
        auto* t = ex.find("type");
        if (t) { std::string tv = t->asString(); if (!tv.empty()) typeSet.insert(tv); }
      }
    };
    if (aiJson.isObject()) {
      for (const char* key : {"exercises", "questions", "items"}) {
        auto* v = aiJson.find(key);
        if (v) {
          if (v->isArray()) collectFromArray(*v);
          else if (v->isObject()) {
            for (const char* k2 : {"exercises", "questions", "items"}) {
              auto* v2 = v->find(k2);
              if (v2 && v2->isArray()) collectFromArray(*v2);
            }
          }
          break;
        }
      }
    }
    for (auto& t : typeSet) r.exerciseTypes.push_back(t);

    // 如果从 AI 响应里还是没提取到（AI 可能不给 type 字段），按 mode 给默认值
    if (r.exerciseTypes.empty()) {
      if (r.mode == "weekly") {
        r.exerciseTypes = {"word_match","word_form_fill","context_blank_fill","cn_to_en_translation","cloze_choice","sentence_imitation"};
      } else {
        r.exerciseTypes = {"word_match","word_form_fill","context_blank_fill"};
      }
    }

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

    auto resp = ai_.postChat(msgs, true, 512, 0.2);
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
