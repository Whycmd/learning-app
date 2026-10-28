// 每日小测服务层（v1.2.0-T007）
// 职责：AI 生成 5 题全客观小测（3 已固化 + 1 熟词生义 + 1 新知）、
//       本地秒批、已固化题联动熟练度、考后固化候选、历史记录
// 约定：词币仅预留（coins_earned 恒为 0，不读写 user_coin）
#pragma once

#include "db.h"
#include "ai_client.h"
#include "knowledge_service.h"
#include "learning_service.h"
#include "treasury_service.h"
#include "json.h"
#include "prompts.h"
#include <utility>

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace eng {

class QuizService
{
  Db& db_;
  AiClient& ai_;
  KnowledgeService& kpSvc_;
  LearningService& learnSvc_;
  TreasuryService& treasurySvc_;

public:
  QuizService(Db& db, AiClient& ai, KnowledgeService& kp, LearningService& ls, TreasuryService& ts)
    : db_(db), ai_(ai), kpSvc_(kp), learnSvc_(ls), treasurySvc_(ts) {}

  // ---------- 发起小测（AI 实时生成） ----------
  struct StartResult
  {
    bool ok = false;
    int code = 400;
    std::string error;
    int64_t quizId = 0;
    int attemptNo = 0;
    std::string quizDate;
    JsonArray questions; // 不含 answer
  };

  StartResult start()
  {
    StartResult r;
    int uid = RequestContext::currentUserId();

    // 当日第几测
    auto cntRows = db_.query(
      "SELECT COUNT(*) AS c FROM daily_quiz WHERE user_id=" + std::to_string(uid)
      + " AND quiz_date=CURDATE()");
    r.attemptNo = cntRows.empty() ? 1 : std::stoi(cntRows[0]["c"]) + 1;

    // 已固化知识点池（随机抽 6 个）
    auto poolRows = db_.query(
      "SELECT id FROM knowledge_points WHERE user_id=" + std::to_string(uid)
      + " AND is_solidified=1 ORDER BY RAND() LIMIT 6");
    if (poolRows.empty())
    {
      r.error = "请先录入并固化至少 1 个知识点";
      return r;
    }

    JsonArray kpDetails;
    std::vector<int64_t> poolIds;
    double diffSum = 0;
    int diffN = 0;
    for (auto& row : poolRows)
    {
      int64_t kid = std::stoll(row["id"]);
      JsonValue d = kpSvc_.detail(kid);
      if (d.isNull()) continue;
      poolIds.push_back(kid);
      kpDetails.push_back(d);
      if (d.find("difficulty") && !d.find("difficulty")->isNull())
      {
        diffSum += d.find("difficulty")->asInt();
        diffN++;
      }
    }
    if (kpDetails.empty())
    {
      r.error = "no solidified knowledge points found";
      r.code = 500;
      return r;
    }
    int avgDiff = diffN ? (int)std::round(diffSum / diffN) : 3;

    // AI 提示词：使用 prompts.h 模板（注释与提示词内容统一在 prompts.h 维护）
    std::string userPrompt = prompts::buildTemplate(
      prompts::DAILY_QUIZ_USER,
      {
        {"{AVG_DIFF}", std::to_string(avgDiff)},
        {"{LEARNER_IDENTITY}", prompts::getLearnerIdentity(db_, uid)},
        {"{KP_JSON}",  JsonValue::makeArray(kpDetails).dump()}
      }
    );

    std::vector<std::map<std::string, std::string>> msgs =
    {
      {{"role", "system"}, {"content", prompts::DAILY_QUIZ_SYSTEM}},
      {{"role", "user"}, {"content", userPrompt}}
    };
    auto resp = ai_.postChat(msgs, true, 1600, 0.6);
    if (!resp.ok)
    {
      r.code = 500;
      r.error = "AI generation failed, please retry";
      return r;
    }

    JsonValue aiJson;
    try
    {
      aiJson = resp.extractedJson.empty() ? JsonValue::makeNull()
                                          : parseJson(resp.extractedJson);
    }
    catch (const std::exception&)
    {
      r.code = 500;
      r.error = "AI returned invalid JSON, please retry";
      return r;
    }
    // 兼容 {"questions":[...]} 包装与裸数组 [...] 两种返回
    const JsonValue* qArr = aiJson.isArray() ? &aiJson : aiJson.find("questions");
    if (!qArr || !qArr->isArray() || qArr->asArray().size() != 5)
    {
      std::cerr << "[quiz] bad question count, raw extracted: "
                << resp.extractedJson << std::endl;
      r.code = 500;
      r.error = "AI 未返回 5 道题，请重试";
      return r;
    }

    // 逐题校验并规范化
    JsonArray questions;
    for (int i = 0; i < 5; ++i)
    {
      JsonValue q = qArr->asArray()[i];
      std::string category = q.getString("category");
      std::string type = q.getString("type");
      std::string question = q.getString("question");
      std::string answer = q.getString("answer");
      if (category != "solidified" && category != "familiar_meaning" && category != "new_kp")
      {
        r.code = 500; r.error = "题目 category 非法，请重试"; return r;
      }
      if (type != "cloze_choice" && type != "word_form_fill")
      {
        r.code = 500; r.error = "题目 type 非法，请重试"; return r;
      }
      if (question.empty() || answer.empty())
      {
        r.code = 500; r.error = "题干或答案缺失，请重试"; return r;
      }
      // 题干必须含挖空标记
      if (question.find("____") == std::string::npos)
      {
        r.code = 500; r.error = "AI 题目未按要求挖空，请重试"; return r;
      }

      JsonArray normOpts;
      std::string correctText;
      if (type == "cloze_choice")
      {
        const JsonValue* opts = q.find("options");
        if (!opts || !opts->isArray() || opts->asArray().size() != 4)
        {
          r.code = 500; r.error = "选择题必须含 4 个选项，请重试"; return r;
        }
        std::string ans = toUpper(trim(answer));
        if (ans.size() != 1 || ans[0] < 'A' || ans[0] > 'D')
        {
          r.code = 500; r.error = "选择题 answer 必须为 A/B/C/D，请重试"; return r;
        }
        answer = ans;
        for (auto& op : opts->asArray())
        {
          std::string t = trim(op.isString() ? op.asString() : op.dump());
          // 清洗 "A) " / "A. " / "A: " 等编号前缀
          if (t.size() >= 2 && (t[0] >= 'A' && t[0] <= 'D' || t[0] >= 'a' && t[0] <= 'd'))
          {
            char c = t[1];
            if (c == ')' || c == '.' || c == ':' || t.substr(1, 3) == "\xE3\x80\x81")
              t = trim(t.substr(c == ')' || c == '.' || c == ':' ? 2 : 4));
          }
          if (t.empty())
          {
            r.code = 500; r.error = "存在空选项，请重试"; return r;
          }
          normOpts.push_back(JsonValue::makeString(t));
        }
        // 选项不得重复
        for (int a = 0; a < 4; ++a)
          for (int b = a + 1; b < 4; ++b)
            if (toLower(normOpts[a].asString()) == toLower(normOpts[b].asString()))
            {
              r.code = 500; r.error = "选择题选项存在重复，请重试"; return r;
            }
        correctText = normOpts[ans[0] - 'A'].asString();
      }
      else
      {
        answer = trim(answer);
        if (answer.find(' ') != std::string::npos)
        {
          r.code = 500; r.error = "填空题答案必须是单个单词，请重试"; return r;
        }
        correctText = answer;
      }

      // 防漏题：正确答案不得在题干中出现（大小写不敏感）
      if (iContainsWord(question, correctText))
      {
        r.code = 500; r.error = "AI 题目题干泄露了答案，请重试"; return r;
      }

      JsonObject o;
      o["no"] = JsonValue::makeInt(i + 1);
      o["category"] = JsonValue::makeString(category);
      o["type"] = JsonValue::makeString(type);
      o["question"] = JsonValue::makeString(question);
      if (type == "cloze_choice")
      {
        o["options"] = JsonValue::makeArray(normOpts);
      }
      o["answer"] = JsonValue::makeString(answer);

      if (category == "solidified")
      {
        int64_t kid = q.getInt("knowledge_id", 0);
        bool inPool = false;
        for (int64_t id : poolIds) if (id == kid) inPool = true;
        if (!inPool) kid = poolIds[i % poolIds.size()]; // AI 乱填 ID 时兜底
        o["knowledge_id"] = JsonValue::makeInt(kid);
      }
      else
      {
        std::string word = trim(q.getString("word"));
        if (word.empty() || word.find(' ') != std::string::npos)
        {
          r.code = 500; r.error = "第 4/5 题 word 必须是单个单词，请重试"; return r;
        }
        std::string meaning = trim(q.getString("meaning"));
        std::string example = trim(q.getString("example"));
        if (meaning.empty())
        {
          r.code = 500; r.error = "第 4/5 题缺少 meaning，请重试"; return r;
        }
        // 答案必须就是 word 本身（或其语法变形），防止考点丢失
        if (!isWordForm(correctText, word))
        {
          std::cerr << "[quiz] rejected raw AI output: " << aiJson.dump() << std::endl;
          r.code = 500; r.error = "第 4/5 题答案必须考查所给单词，请重试"; return r;
        }
        // example 退化自愈：含下划线 → 用答案词补全；照抄题干 → 丢弃
        if (!example.empty() && example.find("____") != std::string::npos)
        {
          size_t p = example.find('_');
          size_t e = p;
          while (e < example.size() && example[e] == '_') ++e;
          example = example.substr(0, p) + correctText + example.substr(e);
        }
        if (example == question) example = "";
        // 仍不含该词：降级为空（前端自动隐藏）
        if (!example.empty() && !exampleContainsWord(example, word)) example = "";
        o["word"] = JsonValue::makeString(word);
        o["meaning"] = JsonValue::makeString(meaning);
        o["example"] = JsonValue::makeString(example);
      }
      questions.push_back(JsonValue::makeObject(o));
    }

    // 落库（题目快照含答案）
    std::string qJson = JsonValue::makeArray(questions).dump();
    auto wr = db_.exec(
      "INSERT INTO daily_quiz (user_id, attempt_no, quiz_date, questions, status, start_time) "
      "VALUES (" + std::to_string(uid) + "," + std::to_string(r.attemptNo)
      + ",CURDATE(),'" + Db::escape(qJson) + "','ongoing',NOW())");
    r.quizId = wr.last_insert_id;

    // 响应剥离 answer
    for (auto& q : questions)
    {
      JsonObject o = q.asObject();
      o.erase("answer");
      r.questions.push_back(JsonValue::makeObject(o));
    }

    auto dateRows = db_.query("SELECT DATE_FORMAT(CURDATE(),'%Y-%m-%d') AS d");
    r.quizDate = dateRows.empty() ? "" : dateRows[0]["d"];
    r.ok = true;
    return r;
  }

  // ---------- 提交批改（本地秒批） ----------
  struct SubmitResult
  {
    bool ok = false;
    int code = 400;
    std::string error;
    int score = 0;
    int correctCount = 0;
    bool alreadyDone = false;
    JsonArray results;
    JsonArray candidates;
  };

  SubmitResult submit(int64_t quizId, const JsonValue& answers, bool syncLearning = true)
  {
    SubmitResult r;
    int uid = RequestContext::currentUserId();

    auto rows = db_.query(
      "SELECT id, user_id, questions, answers, status FROM daily_quiz WHERE id="
      + std::to_string(quizId) + " LIMIT 1");
    if (rows.empty())
    {
      r.code = 404; r.error = "quiz not found"; return r;
    }
    if (std::stoi(rows[0]["user_id"]) != uid)
    {
      r.code = 404; r.error = "quiz not found"; return r;
    }
    if (rows[0]["status"] == "done")
    {
      r.alreadyDone = true;
      syncLearning = false;
    }

    JsonValue stored = parseJson(rows[0]["questions"]);
    if (!stored.isArray())
    {
      r.code = 500; r.error = "stored questions corrupted"; return r;
    }

    // 用户作答映射 no -> answer
    std::map<int, std::string> userAns;
    if (answers.isArray())
    {
      for (auto& a : answers.asArray())
      {
        int no = (int)a.getInt("no", 0);
        if (const JsonValue* ua = a.find("user_answer"))
          userAns[no] = ua->isString() ? ua->asString() : ua->dump();
      }
    }

    JsonArray answerLog;
    int correctN = 0;
    for (auto& q : stored.asArray())
    {
      int no = (int)q.getInt("no", 0);
      std::string type = q.getString("type");
      std::string correctAnswer = q.getString("answer");
      std::string ua = userAns.count(no) ? trim(userAns[no]) : "";

      bool correct = !ua.empty() && answerMatches(type, ua, correctAnswer);
      if (correct) correctN++;

      JsonObject res;
      res["no"] = JsonValue::makeInt(no);
      res["correct"] = JsonValue::makeBool(correct);
      res["correct_answer"] = JsonValue::makeString(correctAnswer);
      res["analysis"] = JsonValue::makeString(
        ua.empty() ? "未作答。"
                   : correct ? "回答正确。"
                             : "回答错误，正确答案为 " + correctAnswer + "。");
      r.results.push_back(JsonValue::makeObject(res));

      JsonObject log;
      log["no"] = JsonValue::makeInt(no);
      log["user_answer"] = JsonValue::makeString(ua);
      answerLog.push_back(JsonValue::makeObject(log));

      // 已固化题联动熟练度（错题 -1，任答复习次数 +1）
      if (syncLearning && q.getString("category") == "solidified")
      {
        int64_t kid = q.getInt("knowledge_id", 0);
        if (kid > 0) learnSvc_.onAnswerResult((int)kid, correct);
      }
    }

    r.correctCount = correctN;
    r.score = (int)std::round(100.0 * correctN / 5);

    // 固化候选（第 4/5 题）
    for (auto& q : stored.asArray())
    {
      std::string category = q.getString("category");
      if (category != "familiar_meaning" && category != "new_kp") continue;
      std::string word = q.getString("word");
      auto dup = db_.query(
        "SELECT id FROM knowledge_points WHERE user_id=" + std::to_string(uid)
        + " AND standard_content='" + Db::escape(word) + "' AND is_solidified=1 LIMIT 1");
      JsonObject c;
      c["no"] = JsonValue::makeInt(q.getInt("no", 0));
      c["category"] = JsonValue::makeString(category);
      c["word"] = JsonValue::makeString(word);
      c["meaning"] = JsonValue::makeString(q.getString("meaning"));
      c["example"] = JsonValue::makeString(q.getString("example"));
      c["already_solidified"] = JsonValue::makeBool(!dup.empty());
      if (!dup.empty()) c["knowledge_id"] = JsonValue::makeInt(std::stoll(dup[0]["id"]));
      r.candidates.push_back(JsonValue::makeObject(c));
    }

    // 首次提交：更新记录
    if (!r.alreadyDone)
    {
      std::string logJson = JsonValue::makeArray(answerLog).dump();
      db_.exec(
        "UPDATE daily_quiz SET status='done', score=" + std::to_string(r.score)
        + ", correct_count=" + std::to_string(r.correctCount)
        + ", answers='" + Db::escape(logJson) + "', finish_time=NOW() WHERE id="
        + std::to_string(quizId));

      // 词币入账
      treasurySvc_.apply(5, "quiz_done", (int)quizId);
      if (r.correctCount == 5)
        treasurySvc_.apply(10, "quiz_perfect", (int)quizId);

      // 每日复习入账（首笔小测提交也算）
      treasurySvc_.apply(5, "review_done", 0);
    }

    r.ok = true;
    return r;
  }

  // ---------- 考后固化熟词生义/新知 ----------
  struct SolidifyItemResult
  {
    bool ok = false;
    int code = 400;
    std::string error;
    int64_t knowledgeId = 0;
    bool duplicated = false;
    std::string word;
  };

  SolidifyItemResult solidifyItem(int64_t quizId, int no)
  {
    SolidifyItemResult r;
    int uid = RequestContext::currentUserId();

    auto rows = db_.query(
      "SELECT user_id, questions FROM daily_quiz WHERE id="
      + std::to_string(quizId) + " LIMIT 1");
    if (rows.empty() || std::stoi(rows[0]["user_id"]) != uid)
    {
      r.code = 404; r.error = "quiz not found"; return r;
    }

    JsonValue stored = parseJson(rows[0]["questions"]);
    const JsonValue* target = nullptr;
    for (auto& q : stored.asArray())
    {
      if ((int)q.getInt("no", 0) == no) { target = &q; break; }
    }
    if (!target)
    {
      r.error = "question not found"; return r;
    }
    std::string category = target->getString("category");
    if (category != "familiar_meaning" && category != "new_kp")
    {
      r.error = "该题不可固化"; return r;
    }

    std::string word = target->getString("word");
    std::string meaning = target->getString("meaning");
    r.word = word;

    // 已固化同词 → 直接返回
    auto dup = db_.query(
      "SELECT id FROM knowledge_points WHERE user_id=" + std::to_string(uid)
      + " AND standard_content='" + Db::escape(word) + "' AND is_solidified=1 LIMIT 1");
    if (!dup.empty())
    {
      r.ok = true; r.duplicated = true;
      r.knowledgeId = std::stoll(dup[0]["id"]);
      return r;
    }

    // 固化的是「单词 + 目标义项」知识点
    std::string remark = (category == "familiar_meaning"
                          ? "【每日小测·熟词生义】" : "【每日小测·新知】")
                         + meaning;

    // 复用已有未固化同词记录，避免重复
    int64_t kpId = 0;
    auto uns = db_.query(
      "SELECT id FROM knowledge_points WHERE user_id=" + std::to_string(uid)
      + " AND user_input_content='" + Db::escape(word)
      + "' AND is_solidified=0 ORDER BY id DESC LIMIT 1");
    if (!uns.empty()) kpId = std::stoll(uns[0]["id"]);
    else
    {
      auto c = kpSvc_.create(word, remark);
      if (!c.ok) { r.code = 500; r.error = c.error; return r; }
      kpId = c.id;
    }

    std::string err;
    auto result = kpSvc_.solidifyOne(kpId, word, remark, err);
    if (!result.ok)
    {
      r.code = 500; r.error = err.empty() ? "solidify failed" : err; return r;
    }
    r.ok = true;
    r.knowledgeId = kpId;

    // 词币：固化扣费（镌刻）
    treasurySvc_.apply(-1, "solidify_cost", (int)kpId);

    return r;
  }

  // ---------- 历史记录 ----------
  JsonValue history()
  {
    int uid = RequestContext::currentUserId();
    auto rows = db_.query(
      "SELECT id, attempt_no, DATE_FORMAT(quiz_date,'%Y-%m-%d') AS quiz_date, "
      "score, correct_count, status, questions, "
      "DATE_FORMAT(start_time,'%Y-%m-%d %H:%i') AS start_time, "
      "DATE_FORMAT(finish_time,'%Y-%m-%d %H:%i') AS finish_time "
      "FROM daily_quiz WHERE user_id=" + std::to_string(uid)
      + " ORDER BY id DESC LIMIT 50");

    JsonArray data;
    int todayCount = 0, todayFinished = 0;
    auto todayRows = db_.query(
      "SELECT DATE_FORMAT(CURDATE(),'%Y-%m-%d') AS d");
    std::string today = todayRows.empty() ? "" : todayRows[0]["d"];

    for (auto& row : rows)
    {
      JsonObject o;
      o["id"] = JsonValue::makeInt(std::stoll(row["id"]));
      o["attempt_no"] = JsonValue::makeInt(std::stoi(row["attempt_no"]));
      o["quiz_date"] = JsonValue::makeString(row["quiz_date"]);
      o["score"] = row["score"].empty() ? JsonValue::makeNull()
                                       : JsonValue::makeInt(std::stoi(row["score"]));
      o["correct_count"] = JsonValue::makeInt(std::stoi(row["correct_count"]));
      o["status"] = JsonValue::makeString(row["status"]);
      o["start_time"] = JsonValue::makeString(row["start_time"]);
      o["finish_time"] = JsonValue::makeString(row["finish_time"]);
      // 未完成测验附带题目（剥离答案），供前端「继续」
      if (row["status"] == "ongoing")
      {
        JsonArray qs;
        for (auto& q : parseJson(row["questions"]).asArray())
        {
          JsonObject qo = q.asObject();
          qo.erase("answer");
          qs.push_back(JsonValue::makeObject(qo));
        }
        o["questions"] = JsonValue::makeArray(qs);
      }
      data.push_back(JsonValue::makeObject(o));

      if (row["quiz_date"] == today)
      {
        todayCount++;
        if (row["status"] == "done") todayFinished++;
      }
    }

    JsonObject out;
    out["data"] = JsonValue::makeArray(data);
    out["today_count"] = JsonValue::makeInt(todayCount);
    out["today_finished"] = JsonValue::makeInt(todayFinished);
    return JsonValue::makeObject(out);
  }

private:
  // 批改规则（与 QuestionService 一致）
  static bool answerMatches(const std::string& type,
                            const std::string& userAnswer,
                            const std::string& correctAnswer)
  {
    if (type == "cloze_choice")
      return toUpper(trim(userAnswer)) == toUpper(trim(correctAnswer));
    return toLower(trim(userAnswer)) == toLower(trim(correctAnswer));
  }

  static std::string trim(const std::string& s)
  {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
  }
  // 大小写不敏感判断 word 是否作为完整词出现在 text 中（短语则子串匹配）
  static bool iContainsWord(std::string text, std::string word)
  {
    if (word.empty()) return false;
    text = toLower(text);
    word = toLower(word);
    bool phrase = word.find(' ') != std::string::npos;
    if (phrase) return text.find(word) != std::string::npos;
    auto isAlpha = [](char c) { return c >= 'a' && c <= 'z'; };
    size_t p = 0;
    while ((p = text.find(word, p)) != std::string::npos)
    {
      bool beforeOk = p == 0 || !isAlpha(text[p - 1]);
      size_t e = p + word.size();
      bool afterOk = e >= text.size() || !isAlpha(text[e]);
      if (beforeOk && afterOk) return true;
      ++p;
    }
    return false;
  }
  // 例句含词判定：允许规则屈折形式（runs/running、phenomena 词干近似）
  static bool exampleContainsWord(const std::string& example, std::string word)
  {
    std::string e = toLower(example);
    word = toLower(word);
    if (e.find(word) != std::string::npos) return true;
    if (word.size() >= 6 && e.find(word.substr(0, word.size() - 2)) != std::string::npos)
      return true;
    return false;
  }
  // 判断 candidate 是否为 base 的语法变形（runs/running、said、abundance 等）
  static bool isWordForm(const std::string& candidate, const std::string& base)
  {
    std::string a = toLower(candidate);
    std::string b = toLower(base);
    if (a == b || a.find(b) != std::string::npos) return true;
    // 长度需接近（屈折变化最多 +4 / -2）
    if (a.size() > b.size() + 4 || a.size() + 2 < b.size()) return false;
    size_t pref = 0;
    while (pref < a.size() && pref < b.size() && a[pref] == b[pref]) ++pref;
    return pref >= (b.size() >= 4 ? 3 : b.size() - 1);
  }
  static std::string toUpper(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
  }
  static std::string toLower(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
  }
};

} // namespace eng
