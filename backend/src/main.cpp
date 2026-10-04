// 英语学习软件 C++ 后端入口
// HTTP server + 路由注册（阶段二：知识点+AI固化 / 阶段三：学情+选题 / 阶段四：出题+批改）
#include "../include/http_server.h"
#include "../include/db.h"
#include "../include/ai_client.h"
#include "../include/knowledge_service.h"
#include "../include/learning_service.h"
#include "../include/question_service.h"
#include "../include/maintenance_service.h"
#include "../include/wordbook_service.h"
#include "../include/ai_task_service.h"
#include "../include/auth.h"
#include "../include/auth_service.h"
#include "../include/quiz_service.h"
#include "../include/articles_vocab_service.h"
#include "../include/articles_service.h"
#include "../include/treasury_service.h"
#include <iostream>
#include <fstream>
#include <string>
#include <utility>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

using namespace eng;

static Db g_db;
static AiClient g_ai;
static KnowledgeService* g_kpSvc = nullptr;
static LearningService* g_learnSvc = nullptr;
static QuestionService* g_qSvc = nullptr;
static MaintenanceService* g_maintSvc = nullptr;
static WordbookService* g_wbSvc = nullptr;
static AiTaskService* g_aiTaskSvc = nullptr;
static AuthService* g_authSvc = nullptr;
static QuizService* g_quizSvc = nullptr;
static TreasuryService* g_treasurySvc = nullptr;
static ArticlesVocabService* g_articlesVocabSvc = nullptr;
static ArticlesService* g_articlesSvc = nullptr;
static HttpServer* g_server = nullptr;

static HttpResponse jsonRes(int status, const std::string& body) {
  HttpResponse r; r.status=status; r.body=body;
  return r;
}
static HttpResponse jsonOk(const std::string& body) { return jsonRes(200, body); }
static HttpResponse jsonErr(int code, const std::string& msg) {
  return jsonRes(code, std::string("{\"error\":\"") + Db::escape(msg) + "\"}");
}

static HttpResponse handleHealth(const HttpRequest& req) {
  bool dbOk = true, aiOk = false;
  try { g_db.query("SELECT 1"); } catch (...) { dbOk = false; }
  try { aiOk = g_ai.ping(); } catch (...) {}
  bool degraded = g_ai.isDegraded();
  // AI 任务统计
  std::string aiStats = "{}";
  try {
    auto st = g_aiTaskSvc ? g_aiTaskSvc->stats() : AiTaskService(g_db).stats();
    std::ostringstream oss;
    oss << "{\"pending\":" << st.pending << ",\"failed\":" << st.failed << ",\"done\":" << st.done << "}";
    aiStats = oss.str();
  } catch (...) {}
  std::ostringstream o;
  o << "{\"status\":\"ok\",\"db\":" << (dbOk?"true":"false")
    << ",\"ai\":" << (aiOk?"true":"false")
    << ",\"ai_degraded\":" << (degraded?"true":"false")
    << ",\"ai_provider\":\"" << providerTypeName(g_ai.getProviderType()) << "\""
    << ",\"ai_model\":\"" << Db::escape(g_ai.config().chat_model) << "\""
    << ",\"ai_tasks\":" << aiStats << "}";
  return jsonOk(o.str());
}

static HttpResponse handleKpCreate(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  if (!v.isObject()) return jsonErr(400, "not object");
  std::string content = v.getString("user_input_content");
  std::string remark = v.getString("remark");
  if (content.empty()) return jsonErr(400, "user_input_content required");
  auto r = g_kpSvc->create(content, remark);
  if (!r.ok) return jsonErr(500, r.error);
  std::ostringstream o;
  o << "{\"id\":" << r.id << ",\"ok\":true,\"is_solidified\":0}";
  return jsonOk(o.str());
}

static HttpResponse handleKpList(const HttpRequest& req) {
  int page = 1, size = 20;
  try { page = std::stoi(req.queryParam("page").empty() ? "1" : req.queryParam("page")); } catch(...) {}
  try { size = std::stoi(req.queryParam("size").empty() ? "20" : req.queryParam("size")); } catch(...) {}
  bool onlyUnsolidified = req.queryParam("unsolidified") == "1" || req.queryParam("unsolidified") == "true";
  auto rows = g_kpSvc->list(page, size, onlyUnsolidified);
  JsonArray a;
  for (auto& r : rows) {
    JsonObject o;
    for (auto& kv : r) {
      if (kv.first == "id" || kv.first == "difficulty" || kv.first == "is_solidified") {
        o[kv.first] = kv.second.empty() ? JsonValue::makeNull() : JsonValue::makeInt(std::stoll(kv.second));
      } else {
        o[kv.first] = kv.second.empty() ? JsonValue::makeNull() : JsonValue::makeString(kv.second);
      }
    }
    a.push_back(JsonValue::makeObject(o));
  }
  return jsonOk(JsonValue::makeObject({{"data", JsonValue::makeArray(a)}}).dump());
}

static HttpResponse handleKpDetail(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  auto v = g_kpSvc->detail(id);
  if (v.isNull()) return jsonErr(404, "not found");
  return jsonOk(v.dump());
}

static HttpResponse handleKpSolidifyAll(const HttpRequest& req) {
  // 查询所有未固化的知识点ID，一次AI调用批量固化
  auto rows = g_db.query("SELECT id FROM knowledge_points WHERE is_solidified=0 AND user_id="
                         + std::to_string(RequestContext::currentUserId()) + " ORDER BY id");
  std::vector<int64_t> ids;
  for (auto& row : rows) ids.push_back(std::stoll(row["id"]));
  if (ids.empty()) return jsonOk("{\"total\":0,\"success\":0,\"failed\":0}");
  auto r = g_kpSvc->solidifyBatch(ids);
  JsonObject o;
  o["total"] = JsonValue::makeInt((int)ids.size());
  o["success"] = JsonValue::makeInt(r.success);
  o["failed"] = JsonValue::makeInt(r.failed);
  o["error"] = JsonValue::makeString(r.error);
  if (r.success > 0) {
    int uid = RequestContext::currentUserId();
    g_treasurySvc->checkAndAwardMasterySeals(uid);
    g_treasurySvc->checkAndAwardTitle(uid);
  }
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleKpExportBatchWord(const HttpRequest& req) {
  auto rows = g_db.query("SELECT id FROM knowledge_points WHERE is_solidified=1 AND user_id="
                         + std::to_string(RequestContext::currentUserId()) + " ORDER BY id");
  std::vector<int64_t> ids;
  for (auto& row : rows) ids.push_back(std::stoll(row["id"]));
  if (ids.empty()) return jsonErr(404, "no solidified knowledge points");
  std::string wordPath = g_kpSvc->exportBatchWord(ids);
  if (wordPath.empty()) return jsonErr(500, "batch word generation failed");
  std::ostringstream o;
  o << "{\"ok\":true,\"word_path\":\"" << Db::escape(wordPath) << "\",\"count\":" << ids.size() << "}";
  return jsonOk(o.str());
}

static HttpResponse handleKpSolidifyOne(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  // 取知识点
  auto rows = g_db.query("SELECT id, user_input_content, remark, is_solidified FROM knowledge_points WHERE id="
                         + std::to_string(id) + " AND user_id="
                         + std::to_string(RequestContext::currentUserId()));
  if (rows.empty()) return jsonErr(404, "not found");
  if (rows[0]["is_solidified"] == "1") return jsonErr(409, "already solidified");
  std::string err;
  bool ok = g_kpSvc->solidifyOne(id, rows[0]["user_input_content"], rows[0]["remark"], err);
  if (!ok) return jsonErr(500, err);
  int uid = RequestContext::currentUserId();
  g_treasurySvc->checkAndAwardMasterySeals(uid);
  g_treasurySvc->checkAndAwardTitle(uid);
  std::string wordPath = g_kpSvc->exportWord(id);
  std::ostringstream o;
  o << "{\"id\":" << id << ",\"ok\":true,\"word_path\":\"" << Db::escape(wordPath) << "\"}";
  return jsonOk(o.str());
}

static HttpResponse handleKpExportWord(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  auto rows = g_db.query("SELECT is_solidified FROM knowledge_points WHERE id=" + std::to_string(id)
                         + " AND user_id=" + std::to_string(RequestContext::currentUserId()));
  if (rows.empty()) return jsonErr(404, "not found");
  if (rows[0]["is_solidified"] != "1") return jsonErr(409, "not solidified yet");
  std::string wordPath = g_kpSvc->exportWord(id);
  if (wordPath.empty()) return jsonErr(500, "word generation failed");
  std::ostringstream o;
  o << "{\"id\":" << id << ",\"word_path\":\"" << Db::escape(wordPath) << "\"}";
  return jsonOk(o.str());
}

static HttpResponse handleKpMerge(const HttpRequest& req) {
  std::string srcStr = req.pathParam("src");
  std::string dstStr = req.pathParam("dst");
  int64_t srcId = 0, dstId = 0;
  try { srcId = std::stoll(srcStr); dstId = std::stoll(dstStr); }
  catch(...) { return jsonErr(400, "invalid ids"); }
  if (srcId == dstId) return jsonErr(400, "src and dst must differ");
  bool ok = g_kpSvc->merge(srcId, dstId);
  return jsonOk(std::string("{\"ok\":") + (ok?"true":"false") + "}");
}

static HttpResponse handleKpSplit(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  int64_t newId = g_kpSvc->split(id);
  if (newId == 0) return jsonErr(500, "split failed");
  std::ostringstream o;
  o << "{\"new_id\":" << newId << "}";
  return jsonOk(o.str());
}

static HttpResponse handleKpUpdateStd(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch(const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ")+e.what()); }
  std::string ns = v.getString("standard_content");
  if (ns.empty()) return jsonErr(400, "standard_content required");
  bool ok = g_kpSvc->updateStandardContent(id, ns);
  return jsonOk(std::string("{\"ok\":") + (ok?"true":"false") + "}");
}

// ============ 阶段三：学情 & 选题 ============

static HttpResponse handleLearnGet(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  auto v = g_learnSvc->getOrCreate(id);
  if (v.isNull()) return jsonErr(404, "knowledge point not found or not solidified");
  return jsonOk(v.dump());
}

static HttpResponse handleLearnSetProficiency(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch(const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ")+e.what()); }
  int level = (int)v.getInt("proficiency_level", -1);
  if (level < 0 || level > 5) return jsonErr(400, "proficiency_level must be 0-5");
  bool ok = g_learnSvc->setProficiency(id, level);
  return jsonOk(std::string("{\"ok\":") + (ok?"true":"false") + "}");
}

static HttpResponse handleLearnSetMode(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch(const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ")+e.what()); }
  std::string mode = v.getString("review_mode");
  if (mode != "daily" && mode != "weekly") return jsonErr(400, "review_mode must be daily or weekly");
  bool ok = g_learnSvc->setReviewMode(id, mode);
  return jsonOk(std::string("{\"ok\":") + (ok?"true":"false") + "}");
}

static HttpResponse handleLearnAnswer(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch(const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ")+e.what()); }
  bool correct = v.getBool("is_correct", false);
  auto r = g_learnSvc->onAnswerResult(id, correct);
  if (!r.ok) return jsonErr(500, r.error);
  std::ostringstream o;
  o << "{\"ok\":true,\"new_proficiency\":" << r.newProficiency
    << ",\"proficiency_name\":\"" << LearningService::proficiencyName(r.newProficiency) << "\"}";
  return jsonOk(o.str());
}

static HttpResponse handleLearnCompleteReview(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  auto r = g_learnSvc->completeReview(id);
  if (!r.ok) return jsonErr(500, r.error);
  int uid = RequestContext::currentUserId();
  g_treasurySvc->advanceWeeklyProgress(uid, "review_items", 1);
  std::ostringstream o;
  o << "{\"ok\":true,\"new_proficiency\":" << r.newProficiency
    << ",\"next_review_time\":\"" << Db::escape(r.nextReviewTime) << "\"}";
  return jsonOk(o.str());
}

static HttpResponse handleLearnCandidatePool(const HttpRequest& req) {
  auto v = g_learnSvc->getCandidatePool();
  return jsonOk(v.dump());
}

static HttpResponse handleLearnSelect(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch(const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ")+e.what()); }
  auto* arr = v.find("knowledge_ids");
  if (!arr || !arr->isArray()) return jsonErr(400, "knowledge_ids array required");
  std::vector<int> ids;
  for (auto& el : arr->asArray()) {
    ids.push_back((int)el.asInt());
  }
  auto r = g_learnSvc->onQuestionSelected(ids);
  if (!r.ok) return jsonErr(500, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["updated_count"] = JsonValue::makeInt((int)r.updatedIds.size());
  JsonArray details;
  for (int kpId : r.updatedIds) {
    auto& d = r.details[kpId];
    JsonObject dObj;
    dObj["knowledge_id"] = JsonValue::makeInt(kpId);
    dObj["new_proficiency"] = JsonValue::makeInt(d.first);
    dObj["next_review_time"] = JsonValue::makeString(d.second);
    details.push_back(JsonValue::makeObject(dObj));
  }
  o["details"] = JsonValue::makeArray(details);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleLearnStats(const HttpRequest& req) {
  auto v = g_learnSvc->getStats();
  return jsonOk(v.dump());
}

// v1.3.0-T108：最近 90 天每日复习次数 + 连续打卡
static HttpResponse handleLearnReviewCalendar(const HttpRequest& req) {
  int days = 90;
  try { days = std::stoi(req.queryParam("days").empty() ? "90" : req.queryParam("days")); } catch(...) {}
  if (days < 7) days = 7;
  if (days > 365) days = 365;
  auto v = g_learnSvc->getReviewCalendar(days);
  return jsonOk(v.dump());
}

// ============ 阶段四：AI 出题 & 自动批改 ============

static HttpResponse handleExerciseGenerate(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch(const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ")+e.what()); }
  auto* arr = v.find("knowledge_ids");
  if (!arr || !arr->isArray()) return jsonErr(400, "knowledge_ids array required");
  std::vector<int> ids;
  for (auto& el : arr->asArray()) ids.push_back((int)el.asInt());
  std::string mode = v.getString("mode", "daily");
  auto r = g_qSvc->generate(ids, mode);
  if (!r.ok) return jsonErr(500, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["exercise_id"] = JsonValue::makeString(r.exerciseId);
  o["word_path"] = JsonValue::makeString(r.wordPath);
  o["mode"] = JsonValue::makeString(r.mode);
  o["page_limit"] = JsonValue::makeInt(r.pageLimit);
  JsonArray types;
  for (auto& t : r.exerciseTypes) types.push_back(JsonValue::makeString(t));
  o["exercise_types"] = JsonValue::makeArray(types);
  if (r.exercises.isObject()) o["exercises"] = r.exercises;
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleGrade(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch(const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ")+e.what()); }
  // 单题批改
  std::string type = v.getString("type");
  std::string userAns = v.getString("user_answer");
  std::string correctAns = v.getString("correct_answer");
  std::string refAns = v.getString("reference_answer");
  if (type.empty() || userAns.empty()) return jsonErr(400, "type and user_answer required");
  auto r = g_qSvc->grade(type, userAns, correctAns, refAns);
  if (!r.ok) return jsonErr(500, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["score"] = JsonValue::makeInt(r.score);
  o["correct"] = JsonValue::makeBool(r.correct);
  o["is_objective"] = JsonValue::makeBool(r.isObjective);
  o["error_analysis"] = JsonValue::makeString(r.errorAnalysis);
  o["reference_answer"] = JsonValue::makeString(r.referenceAnswer);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleBatchGrade(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch(const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ")+e.what()); }
  auto* answers = v.find("answers");
  if (!answers || !answers->isArray()) return jsonErr(400, "answers array required");
  auto r = g_qSvc->batchGrade(*answers);
  if (!r.ok) return jsonErr(500, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["total_score"] = JsonValue::makeInt(r.totalScore);
  o["correct_count"] = JsonValue::makeInt(r.correctCount);
  o["total_count"] = JsonValue::makeInt(r.totalCount);
  JsonArray details;
  for (auto& d : r.details) {
    JsonObject dObj;
    for (auto& kv : d) {
      if (kv.first == "score" || kv.first == "knowledge_id") {
        dObj[kv.first] = JsonValue::makeInt(std::stoll(kv.second.empty() ? "0" : kv.second));
      } else if (kv.first == "correct" || kv.first == "is_objective") {
        dObj[kv.first] = JsonValue::makeBool(kv.second == "true");
      } else {
        dObj[kv.first] = JsonValue::makeString(kv.second);
      }
    }
    details.push_back(JsonValue::makeObject(dObj));
  }
  o["details"] = JsonValue::makeArray(details);
  return jsonOk(JsonValue::makeObject(o).dump());
}

// ============ 阶段六：定时任务 & 维护 ============

static HttpResponse handleBackup(const HttpRequest& req) {
  auto r = g_maintSvc->backupDatabase();
  if (!r.ok) return jsonErr(500, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["path"] = JsonValue::makeString(r.path);
  o["size"] = JsonValue::makeInt(r.sizeBytes);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleListBackups(const HttpRequest& req) {
  return jsonOk(g_maintSvc->listBackups().dump());
}

static HttpResponse handleResetLearning(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int id = 0;
  try { id = std::stoi(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  bool ok = g_maintSvc->resetLearning(id);
  return jsonOk(std::string("{\"ok\":") + (ok?"true":"false") + "}");
}

static HttpResponse handleClearAllLearning(const HttpRequest& req) {
  bool ok = g_maintSvc->clearAllLearning();
  return jsonOk(std::string("{\"ok\":") + (ok?"true":"false") + "}");
}

// ============ v1.2.0-T003：单词本系统 ============

static HttpResponse handleWbList(const HttpRequest& req) {
  return jsonOk(g_wbSvc->list().dump());
}

static HttpResponse handleWbCreate(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  if (!v.isObject()) return jsonErr(400, "not object");
  std::string name = v.getString("name");
  std::string desc = v.getString("description");
  auto r = g_wbSvc->create(name, desc);
  if (!r.ok) return jsonErr(400, r.error);
  std::ostringstream o;
  o << "{\"ok\":true,\"id\":" << r.id << "}";
  return jsonOk(o.str());
}

static HttpResponse handleWbDetail(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  auto v = g_wbSvc->detail(id);
  if (v.isNull()) return jsonErr(404, "not found");
  return jsonOk(v.dump());
}

static HttpResponse handleWbUpdate(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  if (!v.isObject()) return jsonErr(400, "not object");
  std::string err;
  bool ok = g_wbSvc->update(id, v, err);
  if (!ok) return jsonErr(400, err);
  return jsonOk("{\"ok\":true}");
}

static HttpResponse handleWbDelete(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  std::string err;
  bool ok = g_wbSvc->remove(id, err);
  if (!ok) return jsonErr(404, err);
  return jsonOk("{\"ok\":true}");
}

static HttpResponse handleWbEntries(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  auto chk = g_db.query("SELECT id FROM wordbook WHERE id=" + std::to_string(id)
                        + " AND user_id=" + std::to_string(RequestContext::currentUserId()));
  if (chk.empty()) return jsonErr(404, "not found");
  std::string search = req.queryParam("search");
  JsonObject o;
  o["data"] = g_wbSvc->entries(id, search);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWbAddEntries(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  if (!v.isObject()) return jsonErr(400, "not object");
  auto r = g_wbSvc->addEntries(id, v);
  if (!r.error.empty() && r.added == 0) return jsonErr(400, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["added"] = JsonValue::makeInt(r.added);
  o["skipped"] = JsonValue::makeInt(r.skipped);
  o["failed"] = JsonValue::makeInt(r.failed);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWbRemoveEntry(const HttpRequest& req) {
  std::string idStr = req.pathParam("eid");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid entry id"); }
  std::string err;
  bool ok = g_wbSvc->removeEntry(id, err);
  if (!ok) return jsonErr(404, err);
  return jsonOk("{\"ok\":true}");
}

static HttpResponse handleWbExportJson(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  std::string err;
  std::string path = g_wbSvc->exportJson(id, err);
  if (path.empty()) return jsonErr(400, err);
  std::ostringstream o;
  o << "{\"ok\":true,\"json_path\":\"" << Db::escape(path) << "\"}";
  return jsonOk(o.str());
}

static HttpResponse handleWbExportWord(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  std::string err;
  std::string path = g_wbSvc->exportWord(id, err);
  if (path.empty()) return jsonErr(400, err);
  std::ostringstream o;
  o << "{\"ok\":true,\"word_path\":\"" << Db::escape(path) << "\"}";
  return jsonOk(o.str());
}

static HttpResponse handleWbImport(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  if (!v.isObject()) return jsonErr(400, "not object");

  // 支持两种模式：json_data 内嵌 / file_path 文件路径
  WordbookService::ImportResult r;
  if (auto* jsonData = v.find("json_data")) {
    r = g_wbSvc->importFromJson(*jsonData);
  } else if (auto* fp = v.find("file_path")) {
    r = g_wbSvc->importFromFilePath(fp->asString());
  } else if (auto* sc = v.find("share_code")) {
    // 通过识别码导入：先查本地是否有
    auto rows = g_db.query(
      "SELECT id FROM wordbook WHERE share_code='" + Db::escape(sc->asString()) + "'"
    );
    if (rows.empty()) return jsonErr(404, "share code not found locally (market not implemented)");
    // 本地有 -> 导出再导入（避免重复）
    std::string err;
    std::string path = g_wbSvc->exportJson(std::stoll(rows[0]["id"]), err);
    if (path.empty()) return jsonErr(500, err);
    r = g_wbSvc->importFromFilePath(path);
  } else {
    return jsonErr(400, "json_data or file_path or share_code required");
  }

  if (!r.error.empty() && r.wordbook_id == 0) return jsonErr(400, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["wordbook_id"] = JsonValue::makeInt(r.wordbook_id);
  o["imported"] = JsonValue::makeInt(r.imported);
  o["merged"] = JsonValue::makeInt(r.merged);
  return jsonOk(JsonValue::makeObject(o).dump());
}

// ============ v1.2.0-T002：AI 任务管理 ============

static HttpResponse handleAiTaskList(const HttpRequest& req) {
  std::string status = req.queryParam("status");
  auto tasks = g_aiTaskSvc->list(status);
  JsonArray arr;
  for (auto& t : tasks) {
    JsonObject o;
    o["id"] = JsonValue::makeInt(t.id);
    o["task_type"] = JsonValue::makeString(t.task_type);
    o["ref_id"] = JsonValue::makeInt(t.ref_id);
    o["status"] = JsonValue::makeString(t.status);
    o["retry_times"] = JsonValue::makeInt(t.retry_times);
    o["error_msg"] = t.error_msg.empty() ? JsonValue::makeNull() : JsonValue::makeString(t.error_msg);
    o["create_time"] = JsonValue::makeString(t.create_time);
    arr.push_back(JsonValue::makeObject(o));
  }
  JsonObject o;
  auto st = g_aiTaskSvc->stats();
  o["stats"] = JsonValue::makeObject({
    {"pending", JsonValue::makeInt(st.pending)},
    {"running", JsonValue::makeInt(st.running)},
    {"failed", JsonValue::makeInt(st.failed)},
    {"done", JsonValue::makeInt(st.done)}
  });
  o["data"] = JsonValue::makeArray(arr);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleAiTaskRetry(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t taskId = 0;
  try { taskId = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  // 查出 ref_id 和 task_type
  auto rows = g_db.query("SELECT ref_id, task_type FROM ai_tasks WHERE id=" + std::to_string(taskId));
  if (rows.empty()) return jsonErr(404, "task not found");
  int64_t refId = std::stoll(rows[0]["ref_id"]);
  std::string taskType = rows[0]["task_type"];
  if (taskType == "solidify_kp") {
    auto kp = g_db.query("SELECT user_input_content, remark FROM knowledge_points WHERE id=" + std::to_string(refId));
    if (kp.empty()) return jsonErr(404, "knowledge point not found");
    g_db.exec("UPDATE ai_tasks SET status='running' WHERE id=" + std::to_string(taskId));
    std::string err;
    bool ok = g_kpSvc->solidifyOne(refId, kp[0]["user_input_content"], kp[0]["remark"], err);
    std::ostringstream o;
    o << "{\"ok\":" << (ok?"true":"false") << ",\"error\":\"" << Db::escape(err) << "\"}";
    return jsonOk(o.str());
  }
  return jsonErr(400, "unsupported task type: " + taskType);
}

static HttpResponse handleAiTaskRetryAll(const HttpRequest& req) {
  auto tasks = g_aiTaskSvc->findRetryable("solidify_kp");
  int success = 0, failed = 0;
  for (auto taskId : tasks) {
    auto rows = g_db.query("SELECT ref_id FROM ai_tasks WHERE id=" + std::to_string(taskId));
    if (rows.empty()) continue;
    int64_t refId = std::stoll(rows[0]["ref_id"]);
    auto kp = g_db.query("SELECT user_input_content, remark FROM knowledge_points WHERE id=" + std::to_string(refId));
    if (kp.empty()) continue;
    g_db.exec("UPDATE ai_tasks SET status='running' WHERE id=" + std::to_string(taskId));
    std::string err;
    if (g_kpSvc->solidifyOne(refId, kp[0]["user_input_content"], kp[0]["remark"], err)) success++;
    else failed++;
  }
  std::ostringstream o;
  o << "{\"ok\":true,\"success\":" << success << ",\"failed\":" << failed << "}";
  return jsonOk(o.str());
}

static HttpResponse handleAiTaskDelete(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t taskId = 0;
  try { taskId = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  bool ok = g_aiTaskSvc->remove(taskId);
  return jsonOk(std::string("{\"ok\":") + (ok?"true":"false") + "}");
}

// ============ v1.2.0-T004：多用户认证 ============

static HttpResponse handleAuthRegister(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  if (!v.isObject()) return jsonErr(400, "not object");
  auto r = g_authSvc->regist(v.getString("username"), v.getString("password"),
                              v.getString("display_name"), v.getString("learner_identity"));
  if (!r.ok) return jsonErr(r.code, r.error);
  std::ostringstream o;
  o << "{\"ok\":true,\"user_id\":" << r.user_id << "}";
  return jsonOk(o.str());
}

static HttpResponse handleAuthLogin(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  if (!v.isObject()) return jsonErr(400, "not object");
  auto r = g_authSvc->login(v.getString("username"), v.getString("password"));
  if (!r.ok) return jsonErr(r.code, r.error);
  std::ostringstream o;
  o << "{\"ok\":true,\"user\":{\"id\":" << r.user_id
    << ",\"username\":\"" << Db::escape(r.username)
    << "\",\"display_name\":\"" << Db::escape(r.display_name) << "\"}}";
  HttpResponse resp = jsonOk(o.str());
  resp.extraHeaders["Set-Cookie"] =
    "eng_session=" + r.token + "; Path=/; HttpOnly; Max-Age=2592000";
  return resp;
}

static HttpResponse handleAuthLogout(const HttpRequest& req) {
  std::string token = Auth::cookieValue(req.header("cookie"), "eng_session");
  g_authSvc->logout(token);
  HttpResponse resp = jsonOk("{\"ok\":true}");
  resp.extraHeaders["Set-Cookie"] = "eng_session=; Path=/; HttpOnly; Max-Age=0";
  return resp;
}

static HttpResponse handleAuthDeregister(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 1) return jsonErr(403, "游客账号不可注销");
  std::string token = Auth::cookieValue(req.header("cookie"), "eng_session");
  auto r = g_authSvc->deregister(uid);
  if (!r.ok) return jsonErr(r.code, r.error);
  g_authSvc->logout(token);
  HttpResponse resp = jsonOk("{\"ok\":true}");
  resp.extraHeaders["Set-Cookie"] = "eng_session=; Path=/; HttpOnly; Max-Age=0";
  return resp;
}

static HttpResponse handleAuthMe(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  std::ostringstream o;
  if (uid <= 1)
  {
    o << "{\"user_id\":1,\"guest\":true}";
  }
  else
  {
    auto info = g_authSvc->userInfo(uid);
    if (info.empty()) { o << "{\"user_id\":1,\"guest\":true}"; }
    else
    {
      o << "{\"user_id\":" << uid
        << ",\"guest\":false,\"username\":\"" << Db::escape(info["username"])
        << "\",\"display_name\":\"" << Db::escape(info["display_name"]) << "\""
        << ",\"learner_identity\":\"" << Db::escape(info["learner_identity"]) << "\"}";
    }
  }
  return jsonOk(o.str());
}

// v1.3.0-T001：更新用户资料（display_name / learner_identity）
static HttpResponse handleAuthProfile(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 1) return jsonErr(403, "游客账号不可修改资料");
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  if (!v.isObject()) return jsonErr(400, "not object");
  auto r = g_authSvc->updateProfile(uid, v.getString("display_name"), v.getString("learner_identity"));
  if (!r.ok) return jsonErr(r.code, r.error);
  std::ostringstream o;
  o << "{\"ok\":true";
  if (!r.display_name.empty()) o << ",\"display_name\":\"" << Db::escape(r.display_name) << "\"";
  if (!r.learner_identity.empty()) o << ",\"learner_identity\":\"" << Db::escape(r.learner_identity) << "\"";
  o << "}";
  return jsonOk(o.str());
}

// v1.3.0-T001：获取学习者身份建议清单（字典表）
static HttpResponse handleLearnerIdentities(const HttpRequest& req) {
  try
  {
    auto rows = g_db.query(
      "SELECT name FROM learner_identities WHERE enabled=1 ORDER BY sort ASC, id ASC"
    );
    JsonArray arr;
    for (auto& row : rows)
      arr.push_back(JsonValue::makeString(row["name"]));
    JsonObject o;
    o["data"] = JsonValue::makeArray(arr);
    return jsonOk(JsonValue::makeObject(o).dump());
  }
  catch (const std::exception& e) { return jsonErr(500, e.what()); }
}

// v1.2.0-T006：播放复习 — 返回当前用户已固化词条列表（供 TTS 朗读队列使用）
static HttpResponse handlePlaybackQueue(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  auto rows = g_db.query(
    "SELECT id, standard_content AS word FROM knowledge_points "
    "WHERE user_id=" + std::to_string(uid) + " AND is_solidified=1 "
    "ORDER BY id DESC LIMIT 200");
  JsonArray arr;
  for (auto& row : rows) {
    JsonObject item;
    item["id"] = JsonValue::makeInt(std::stoll(row["id"]));
    item["word"] = JsonValue::makeString(row["word"]);
    arr.push_back(JsonValue::makeObject(item));
  }
  JsonObject o;
  o["data"] = JsonValue::makeArray(arr);
  o["total"] = JsonValue::makeInt((int)rows.size());
  return jsonOk(JsonValue::makeObject(o).dump());
}

// ============ v1.2.0-T007：每日小测 ============

static HttpResponse handleQuizStart(const HttpRequest& req) {
  auto r = g_quizSvc->start();
  if (!r.ok) return jsonErr(r.code, r.error);
  JsonObject o;
  o["quiz_id"] = JsonValue::makeInt(r.quizId);
  o["attempt_no"] = JsonValue::makeInt(r.attemptNo);
  o["quiz_date"] = JsonValue::makeString(r.quizDate);
  o["questions"] = JsonValue::makeArray(r.questions);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleQuizSubmit(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  int64_t quizId = v.getInt("quiz_id", 0);
  if (quizId <= 0) return jsonErr(400, "quiz_id required");
  const JsonValue* answers = v.find("answers");
  if (!answers || !answers->isArray()) return jsonErr(400, "answers array required");
  auto r = g_quizSvc->submit(quizId, *answers);
  if (!r.ok) return jsonErr(r.code, r.error);
  JsonObject o;
  o["quiz_id"] = JsonValue::makeInt(quizId);
  o["score"] = JsonValue::makeInt(r.score);
  o["correct_count"] = JsonValue::makeInt(r.correctCount);
  o["results"] = JsonValue::makeArray(r.results);
  o["solidify_candidates"] = JsonValue::makeArray(r.candidates);
  // 词币：今日实际入账
  int uid = RequestContext::currentUserId();
  if (!r.alreadyDone) g_treasurySvc->advanceWeeklyProgress(uid, "quiz_count", 1);
  int coinsEarned = r.alreadyDone ? 0 : g_treasurySvc->getTodayEarned(uid);
  o["coins_earned"] = JsonValue::makeInt(coinsEarned);
  o["coins_note"] = JsonValue::makeString("每日首次小测 +5，满分再加 +10；同日复习 +5；见圣殿账册");
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleQuizSolidifyItem(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  int64_t quizId = v.getInt("quiz_id", 0);
  int no = (int)v.getInt("no", 0);
  if (quizId <= 0 || no <= 0) return jsonErr(400, "quiz_id and no required");
  auto r = g_quizSvc->solidifyItem(quizId, no);
  if (!r.ok) return jsonErr(r.code, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["knowledge_id"] = JsonValue::makeInt(r.knowledgeId);
  o["duplicated"] = JsonValue::makeBool(r.duplicated);
  o["word"] = JsonValue::makeString(r.word);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleQuizHistory(const HttpRequest& req) {
  return jsonOk(g_quizSvc->history().dump());
}

// ============ v1.3.0-T005：点金圣殿 ============

static HttpResponse handleTreasury(const HttpRequest& req)
{
  int uid = RequestContext::currentUserId();
  auto [balance, totalEarned, totalSpent] = g_treasurySvc->getBalance(uid);
  JsonObject o;
  o["balance"] = JsonValue::makeInt(balance);
  o["total_earned"] = JsonValue::makeInt(totalEarned);
  o["total_spent"] = JsonValue::makeInt(totalSpent);
  o["txns"] = JsonValue::makeArray(g_treasurySvc->getTxns(uid, 10));
  o["seals"] = JsonValue::makeArray(g_treasurySvc->getSeals(uid));
  o["current_title"] = JsonValue::makeString(g_treasurySvc->getCurrentTitle(uid));
  o["mastery_count"] = JsonValue::makeInt(g_treasurySvc->getMasteryCount(uid));
  o["solidify_count"] = JsonValue::makeInt(g_treasurySvc->getSolidifyCount(uid));
  o["weekly_goal"] = g_treasurySvc->getWeeklyGoal(uid);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleTreasurySpend(const HttpRequest& req)
{
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  int amount = (int)v.getInt("amount", 0);
  std::string reason = v.getString("reason");
  int ref_id = (int)v.getInt("ref_id", 0);
  if (amount <= 0) return jsonErr(400, "amount must be positive");
  if (reason.empty()) return jsonErr(400, "reason required");
  int uid = RequestContext::currentUserId();
  bool ok = g_treasurySvc->apply(-amount, reason, ref_id);
  if (!ok) return jsonErr(400, "insufficient balance");
  auto [newBal, newEarned, newSpent] = g_treasurySvc->getBalance(uid);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["balance"] = JsonValue::makeInt(newBal);
  o["total_earned"] = JsonValue::makeInt(newEarned);
  o["total_spent"] = JsonValue::makeInt(newSpent);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleTreasuryStats(const HttpRequest& req)
{
  int uid = RequestContext::currentUserId();
  JsonObject o;
  o["mastery_count"] = JsonValue::makeInt(g_treasurySvc->getMasteryCount(uid));
  o["solidify_count"] = JsonValue::makeInt(g_treasurySvc->getSolidifyCount(uid));
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleTreasuryWeeklyGoal(const HttpRequest& req)
{
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  std::string goal_type = v.getString("goal_type");
  int target = (int)v.getInt("target", 0);
  if (goal_type.empty() || target <= 0) return jsonErr(400, "goal_type and target required");
  bool ok = g_treasurySvc->setWeeklyGoal(RequestContext::currentUserId(), goal_type, target);
  if (!ok) return jsonErr(400, "invalid goal_type");
  JsonObject o;
  o["ok"] = JsonValue::makeBool(ok);
  if (ok) o["weekly_goal"] = g_treasurySvc->getWeeklyGoal(RequestContext::currentUserId());
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleCleanupTempWord(const HttpRequest& req) {
  int maxAge = 3600;
  try { maxAge = std::stoi(req.queryParam("max_age").empty() ? "3600" : req.queryParam("max_age")); } catch(...) {}
  auto r = g_maintSvc->cleanupTempWord(maxAge);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["deleted"] = JsonValue::makeInt(r.deleted);
  o["skipped"] = JsonValue::makeInt(r.skipped);
  JsonArray files;
  for (auto& f : r.deletedFiles) files.push_back(JsonValue::makeString(f));
  o["deleted_files"] = JsonValue::makeArray(files);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleCleanupStartup(const HttpRequest& req) {
  auto r = g_maintSvc->cleanupStartupExercises();
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["deleted"] = JsonValue::makeInt(r.deleted);
  JsonArray files;
  for (auto& f : r.deletedFiles) files.push_back(JsonValue::makeString(f));
  o["deleted_files"] = JsonValue::makeArray(files);
  return jsonOk(JsonValue::makeObject(o).dump());
}

// ============ v1.3.0 文章模块 ============

static HttpResponse handleArticleList(const HttpRequest& req) {
  int page = 1, size = 20;
  try { page = std::stoi(req.queryParam("page").empty() ? "1" : req.queryParam("page")); } catch(...) {}
  try { size = std::stoi(req.queryParam("size").empty() ? "20" : req.queryParam("size")); } catch(...) {}
  std::string cat = req.queryParam("category");
  std::string cefr = req.queryParam("cefr_level");
  return jsonOk(g_articlesSvc->list(page, size, cat, cefr).dump());
}

static HttpResponse handleArticleMy(const HttpRequest& req) {
  int page = 1, size = 20;
  try { page = std::stoi(req.queryParam("page").empty() ? "1" : req.queryParam("page")); } catch(...) {}
  try { size = std::stoi(req.queryParam("size").empty() ? "20" : req.queryParam("size")); } catch(...) {}
  return jsonOk(g_articlesSvc->list(page, size, "", "", true).dump());
}

static HttpResponse handleArticleCreate(const HttpRequest& req) {
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch(const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  if (!v.isObject()) return jsonErr(400, "not object");
  auto r = g_articlesSvc->create(
    v.getString("title"),
    v.getString("content"),
    v.getString("category", "other"),
    v.getString("cefr_level", ""),
    v.getString("import_method", "text"),
    v.getString("source_url", ""),
    v.getString("source_name", "")
  );
  if (!r.ok) return jsonErr(400, r.error);
  std::ostringstream o;
  o << "{\"ok\":true,\"id\":" << r.id << "}";
  return jsonOk(o.str());
}

static HttpResponse handleArticleDetail(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  auto v = g_articlesSvc->detail(id);
  if (v.isNull()) return jsonErr(404, "not found");
  return jsonOk(v.dump());
}

static HttpResponse handleArticleUpdate(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch(const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  if (!v.isObject()) return jsonErr(400, "not object");
  auto r = g_articlesSvc->update(id, v.asObject());
  if (!r.ok) return jsonErr(400, r.error);
  return jsonOk("{\"ok\":true}");
}

static HttpResponse handleArticleDelete(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  auto r = g_articlesSvc->remove(id);
  if (!r.ok) return jsonErr(400, r.error);
  return jsonOk("{\"ok\":true}");
}

static HttpResponse handleArticleDifficultyReport(const HttpRequest& req) {
  std::string idStr = req.pathParam("id");
  int64_t id = 0;
  try { id = std::stoll(idStr); } catch(...) { return jsonErr(400, "invalid id"); }
  auto v = g_articlesSvc->difficultyReport(id);
  if (v.isNull()) return jsonErr(404, "not found");
  return jsonOk(v.dump());
}


static HttpResponse handleStatic(const HttpRequest& req) {
  std::string p = req.path;
  if (p == "/" || p.empty()) p = "/index.html";
  // 防目录穿越
  if (p.find("..") != std::string::npos) return jsonErr(400, "bad path");
  std::string full = "/Users/junwang/Desktop/王浩源/英语/英语学习应用/frontend" + p;
  std::ifstream f(full, std::ios::binary);
  if (!f) {
    // 尝试 /pages/...
    full = "/Users/junwang/Desktop/王浩源/英语/英语学习应用/frontend/pages" + p;
    f.clear(); f.open(full, std::ios::binary);
    if (!f) return jsonErr(404, "not found: " + p);
  }
  std::ostringstream ss; ss << f.rdbuf();
  HttpResponse r;
  r.body = ss.str();
  if (full.size() > 5 && full.substr(full.size()-5) == ".html") r.contentType = "text/html; charset=utf-8";
  else if (full.size() > 4 && full.substr(full.size()-4) == ".css") r.contentType = "text/css; charset=utf-8";
  else if (full.size() > 3 && full.substr(full.size()-3) == ".js") r.contentType = "application/javascript; charset=utf-8";
  else if (full.size() > 6 && full.substr(full.size()-6) == ".woff2") r.contentType = "font/woff2";
  else if (full.size() > 5 && full.substr(full.size()-5) == ".woff") r.contentType = "font/woff";
  else r.contentType = "application/octet-stream";
  return r;
}

// Word 文件下载
static HttpResponse handleWordDownload(const HttpRequest& req) {
  std::string p = req.queryParam("path");
  if (p.empty() || p.find("..") != std::string::npos) return jsonErr(400, "bad path");
  // 限制只能下载 word_tmp 下
  if (p.find("word_tmp/") == std::string::npos && p.find("/word_tmp/") == std::string::npos)
    return jsonErr(400, "path must be under word_tmp");
  std::ifstream f(p, std::ios::binary);
  if (!f) return jsonErr(404, "file not found");
  std::ostringstream ss; ss << f.rdbuf();
  HttpResponse r;
  r.body = ss.str();
  r.contentType = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
  // 文件名
  size_t pos = p.find_last_of('/');
  std::string fn = (pos == std::string::npos) ? "file.docx" : p.substr(pos+1);
  r.extraHeaders["Content-Disposition"] = "attachment; filename=\"" + fn + "\"";
  return r;
}

void sigHandler(int sig) {
  std::cerr << "[server] signal " << sig << ", shutting down" << std::endl;
  if (g_server) { /* 不优雅，直接退 */ }
  _exit(0);
}

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, sigHandler);
  signal(SIGTERM, sigHandler);

  std::string root = "/Users/junwang/Desktop/王浩源/英语/英语学习应用";
  int port = 8088;
  std::string host = "127.0.0.1";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--port" && i+1 < argc) port = std::stoi(argv[++i]);
    else if (a == "--host" && i+1 < argc) host = argv[++i];
    else if (a == "--root" && i+1 < argc) root = argv[++i];
  }

  // 加载配置
  DbConfig dbCfg = Db::loadFromJsonFile(root + "/config/db.json");
  AiConfig aiCfg = AiClient::loadFromJsonFile(root + "/config/ai.json");
  g_ai.setConfig(aiCfg);

  // 连接数据库（带重试）
  for (int i = 1; i <= 5; i++) {
    try { g_db.connect(dbCfg); break; }
    catch (const std::exception& e) {
      std::cerr << "[db] connect attempt " << i << " failed: " << e.what() << std::endl;
      if (i == 5) { std::cerr << "[db] giving up" << std::endl; return 1; }
      sleep(2);
    }
  }
  std::cerr << "[db] connected to " << dbCfg.host << ":" << dbCfg.port << "/" << dbCfg.database << std::endl;

  std::string wordTmp = root + "/word_tmp";
  ::mkdir(wordTmp.c_str(), 0755);
  KnowledgeService kpSvc(g_db, g_ai, wordTmp);
  g_kpSvc = &kpSvc;
  LearningService learnSvc(g_db);
  g_learnSvc = &learnSvc;
  QuestionService qSvc(g_db, g_ai, kpSvc, learnSvc, wordTmp, root);
  g_qSvc = &qSvc;
  std::string backupDir = root + "/backups";
  MaintenanceService maintSvc(g_db, backupDir, wordTmp);
  g_maintSvc = &maintSvc;
  WordbookService wbSvc(g_db, kpSvc, wordTmp);
  g_wbSvc = &wbSvc;
  AiTaskService aiTaskSvc(g_db);
  g_aiTaskSvc = &aiTaskSvc;
  AuthService authSvc(g_db);
  g_authSvc = &authSvc;
  authSvc.purgeExpired();
  TreasuryService treasurySvc(g_db, root + "/config/rewards.json");
  g_treasurySvc = &treasurySvc;
  QuizService quizSvc(g_db, g_ai, kpSvc, learnSvc, treasurySvc);
  g_quizSvc = &quizSvc;
  // v1.3.0 文章模块：Service 初始化 + EFLLex 加载
  ArticlesVocabService articlesVocabSvc;
  g_articlesVocabSvc = &articlesVocabSvc;
  std::string efllexPath = root + "/backend/data/efllex.tsv";
  int efllexCount = articlesVocabSvc.loadEfllEx(efllexPath);
  ArticlesService articlesSvc(g_db, articlesVocabSvc);
  g_articlesSvc = &articlesSvc;
  std::cerr << "[articles] EFLLex loaded: " << efllexCount
            << " entries (" << articlesVocabSvc.vocabCount() << " vocab map size)" << std::endl;
  // 启动时清理习题临时文件
  auto cleanup = maintSvc.cleanupStartupExercises();
  std::cerr << "[maint] startup cleanup: deleted " << cleanup.deleted << " exercise files" << std::endl;

  HttpServer server(host, port);
  g_server = &server;

  // 路由注册
  server.Get("/api/health", handleHealth);
  server.Post("/api/kp", handleKpCreate);
  server.Get("/api/kp", handleKpList);
  server.Get("/api/kp/:id", handleKpDetail);
  server.Post("/api/kp/solidify", handleKpSolidifyAll);
  server.Post("/api/kp/:id/solidify", handleKpSolidifyOne);
  server.Post("/api/kp/:id/export-word", handleKpExportWord);
  server.Post("/api/kp/export-batch-word", handleKpExportBatchWord);
  server.Post("/api/kp/:src/merge/:dst", handleKpMerge);
  server.Post("/api/kp/:id/split", handleKpSplit);
  server.Put("/api/kp/:id/standard", handleKpUpdateStd);
  server.Get("/api/word/download", handleWordDownload);
  // 阶段三：学情 & 选题（具体路径必须在 :id 之前注册）
  server.Get("/api/learn/candidates", handleLearnCandidatePool);
  server.Get("/api/learn/stats", handleLearnStats);
  server.Get("/api/learn/review-calendar", handleLearnReviewCalendar);
  server.Post("/api/learn/select", handleLearnSelect);
  server.Get("/api/learn/:id", handleLearnGet);
  server.Put("/api/learn/:id/proficiency", handleLearnSetProficiency);
  server.Put("/api/learn/:id/mode", handleLearnSetMode);
  server.Post("/api/learn/:id/answer", handleLearnAnswer);
  server.Post("/api/learn/:id/complete-review", handleLearnCompleteReview);
  // 阶段四：AI 出题 & 批改
  server.Post("/api/exercise/generate", handleExerciseGenerate);
  server.Post("/api/exercise/grade", handleGrade);
  server.Post("/api/exercise/batch-grade", handleBatchGrade);
  // 阶段六：定时任务 & 维护
  server.Post("/api/maint/backup", handleBackup);
  server.Get("/api/maint/backups", handleListBackups);
  server.Post("/api/maint/reset-learning/:id", handleResetLearning);
  server.Post("/api/maint/clear-all-learning", handleClearAllLearning);
  server.Post("/api/maint/cleanup-temp-word", handleCleanupTempWord);
  server.Post("/api/maint/cleanup-startup", handleCleanupStartup);
  // v1.2.0-T003：单词本系统
  server.Get("/api/wordbook", handleWbList);
  server.Post("/api/wordbook", handleWbCreate);
  server.Get("/api/wordbook/:id", handleWbDetail);
  server.Put("/api/wordbook/:id", handleWbUpdate);
  server.Delete("/api/wordbook/:id", handleWbDelete);
  server.Get("/api/wordbook/:id/entries", handleWbEntries);
  server.Post("/api/wordbook/:id/entries", handleWbAddEntries);
  server.Delete("/api/wordbook/entries/:eid", handleWbRemoveEntry);
  server.Post("/api/wordbook/:id/export-json", handleWbExportJson);
  server.Post("/api/wordbook/:id/export-word", handleWbExportWord);
  server.Post("/api/wordbook/import", handleWbImport);
  // v1.2.0-T002：AI 任务管理
  server.Get("/api/ai/tasks", handleAiTaskList);
  server.Post("/api/ai/tasks/:id/retry", handleAiTaskRetry);
  server.Post("/api/ai/tasks/retry-all", handleAiTaskRetryAll);
  server.Delete("/api/ai/tasks/:id", handleAiTaskDelete);
  // v1.2.0-T004：多用户认证
  server.Post("/api/auth/register", handleAuthRegister);
  server.Post("/api/auth/login", handleAuthLogin);
  server.Post("/api/auth/logout", handleAuthLogout);
  server.Post("/api/auth/deregister", handleAuthDeregister);
  server.Get("/api/auth/me", handleAuthMe);
  server.Put("/api/auth/profile", handleAuthProfile);
  server.Get("/api/learner-identities", handleLearnerIdentities);
  // v1.2.0-T006：播放复习
  server.Get("/api/playback/queue", handlePlaybackQueue);
  // v1.2.0-T007：每日小测
  server.Post("/api/quiz/daily", handleQuizStart);
  server.Post("/api/quiz/daily/submit", handleQuizSubmit);
  server.Post("/api/quiz/solidify-item", handleQuizSolidifyItem);
  server.Get("/api/quiz/daily/history", handleQuizHistory);
  // v1.3.0-T005：点金圣殿
  server.Get("/api/treasury", handleTreasury);
  server.Post("/api/treasury/spend", handleTreasurySpend);
  server.Get("/api/treasury/stats", handleTreasuryStats);
  server.Post("/api/treasury/weekly-goal", handleTreasuryWeeklyGoal);
  // v1.3.0 文章模块
  server.Get("/api/article/my", handleArticleMy);
  server.Get("/api/article", handleArticleList);
  server.Post("/api/article", handleArticleCreate);
  server.Get("/api/article/:id", handleArticleDetail);
  server.Put("/api/article/:id", handleArticleUpdate);
  server.Delete("/api/article/:id", handleArticleDelete);
  server.Get("/api/article/:id/difficulty-report", handleArticleDifficultyReport);

  // 每个请求路由前：解析 Cookie → 设置当前用户身份上下文
  server.setBeforeDispatch([&](const HttpRequest& req) {
    std::string token = Auth::cookieValue(req.header("cookie"), "eng_session");
    RequestContext::userId = authSvc.resolveSession(token);
  });

  // 静态前端（兜底）
  server.Get("/", handleStatic);
  server.Get("/index.html", handleStatic);
  server.Get("/pages/*", handleStatic);
  server.Get("/static/*", handleStatic);

  server.start();
  return 0;
}
