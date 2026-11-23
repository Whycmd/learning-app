// 英语学习软件 C++ 后端入口
// HTTP server + 路由注册（阶段二：知识点+AI固化 / 阶段三：学情+选题 / 阶段四：出题+批改）
#include "../include/http_server.h"
#include "../include/db.h"
#include "../include/ai_client.h"
#include "../include/knowledge_service.h"
#include "../include/solidify_job.h"
#include "../include/ai_queue_service.h"
#include "../include/learning_service.h"
#include "../include/question_service.h"
#include "../include/maintenance_service.h"
#include "../include/wordbook_service.h"
#include "../include/wordlist_service.h"
#include "../include/export_service.h"
#include "../include/ops_service.h"
#include "../include/prompts.h"
#include "../include/ai_task_service.h"
#include "../include/auth.h"
#include "../include/auth_service.h"
#include "../include/quiz_service.h"
#include "../include/articles_vocab_service.h"
#include "../include/articles_service.h"
#include "../include/treasury_service.h"
#include "../include/goal_service.h"
#include "../include/explain_service.h"
#include "../include/custom_exercise_service.h"
#include "../include/error_service.h"
#include "../include/weekly_report_service.h"
#include "../include/action_service.h"
#include "../include/ai_nav_service.h"
#include "../include/dev_intake_service.h"
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
#include <utility>
#include <cctype>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>

using namespace eng;

// 项目根目录：由 --root / APP_ROOT / 可执行文件位置 / 当前目录推导（见 resolveRoot）
static std::string g_root = ".";

static Db g_db;
static AiClient g_ai;
static KnowledgeService* g_kpSvc = nullptr;
static SolidifyJobService* g_jobSvc = nullptr;
static AiQueueService* g_queueSvc = nullptr;
static LearningService* g_learnSvc = nullptr;
static QuestionService* g_qSvc = nullptr;
static MaintenanceService* g_maintSvc = nullptr;
static OpsService* g_opsSvc = nullptr;
static WordbookService* g_wbSvc = nullptr;
static WordlistService* g_wlSvc = nullptr;
static AiTaskService* g_aiTaskSvc = nullptr;
static AuthService* g_authSvc = nullptr;
static QuizService* g_quizSvc = nullptr;
static TreasuryService* g_treasurySvc = nullptr;
static GoalService* g_goalSvc = nullptr;
static ArticlesVocabService* g_articlesVocabSvc = nullptr;
static ArticlesService* g_articlesSvc = nullptr;
// v1.5.0-T006：AI 讲解 + 定制练习
static ExplainService* g_explainSvc = nullptr;
static CustomExerciseService* g_customExSvc = nullptr;
// v1.5.1-T002：错题归因与重练
static ErrorService* g_errorSvc = nullptr;
// v1.5.1-T003：学情周报
static WeeklyReportService* g_weeklySvc = nullptr;
// v1.5.1-T005：动作词典 + AI 导航
static ActionService* g_actionSvc = nullptr;
static AiNavService* g_aiNavSvc = nullptr;
static DevIntakeService* g_devSvc = nullptr;
// v1.5.0-T009：导出中心
static ExportService* g_exportSvc = nullptr;
static HttpServer* g_server = nullptr;
static int g_wordlistDailyLimit = 5;

static HttpResponse jsonRes(int status, const std::string& body) {
  HttpResponse r; r.status=status; r.body=body;
  return r;
}
static HttpResponse jsonOk(const std::string& body) { return jsonRes(200, body); }
static HttpResponse jsonErr(int code, const std::string& msg) {
  return jsonRes(code, std::string("{\"error\":\"") + Db::escape(msg) + "\"}");
}

// v1.5.0-T001：写接口登录拦截，未登录（uid<=0）返回 401
static Handler requireLogin(Handler handler)
{
  return [h = std::move(handler)](const HttpRequest& req) -> HttpResponse
  {
    if (RequestContext::currentUserId() <= 0) return jsonErr(401, "请先登录");
    return h(req);
  };
}

// ===== v1.5.0-T006：AI 讲解 + 定制练习 + 空闲队列 =====

// 游客标识：无 Cookie 时生成并回写（仅讲解/练习读写入接口使用）
static std::string guestCookieFor(const HttpRequest& req, std::string& setCookie)
{
  std::string gid = Auth::cookieValue(req.header("cookie"), "eng_guest_id");
  if (gid.empty()) {
    gid = Auth::randomHex(12);
    setCookie = "eng_guest_id=" + gid + "; Path=/; HttpOnly; Max-Age=31536000; SameSite=Lax";
  }
  return gid;
}

static JsonValue explainViewToJson(const ExplainService::View& v)
{
  JsonObject o;
  o["id"] = JsonValue::makeInt(v.id);
  o["input_text"] = JsonValue::makeString(v.inputText);
  o["input_type"] = JsonValue::makeString(v.inputType);
  o["identity"] = JsonValue::makeString(v.identity);
  o["depth"] = JsonValue::makeString(v.depth);
  o["status"] = JsonValue::makeString(v.status);
  o["summary"] = JsonValue::makeString(v.summary);
  o["content_md"] = JsonValue::makeString(v.contentMd);
  o["error"] = JsonValue::makeString(v.error);
  o["created_at"] = JsonValue::makeString(v.createdAt);
  o["task_id"] = JsonValue::makeInt(v.taskId);
  o["l1_hit"] = JsonValue::makeBool(v.l1KpId > 0);
  o["queue_level"] = JsonValue::makeInt(v.queueLevel);
  o["position"] = JsonValue::makeInt(v.position);
  o["eta_sec"] = JsonValue::makeInt(v.etaSec);
  if (!v.resultJson.empty()) {
    try { o["result"] = parseJson(v.resultJson); } catch (...) {}
  }
  return JsonValue::makeObject(o);
}

static bool idleHintHit(int etaSec)
{
  return g_explainSvc && etaSec >= g_explainSvc->cfgIdleHintEta();
}

static HttpResponse handleExplainCreate(const HttpRequest& req)
{
  if (!g_explainSvc) return jsonErr(500, "service unavailable");
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  std::string text = v.getString("text");
  if (text.empty()) text = v.getString("input");
  std::string depth = v.getString("depth", "standard");
  int level = (int)v.getInt("queue_level", 0);
  int uid = RequestContext::currentUserId();
  std::string setCookie, gid;
  if (uid <= 0) gid = guestCookieFor(req, setCookie);
  auto r = g_explainSvc->submit(uid, gid, text, depth, level);

  HttpResponse resp;
  if (!r.ok) {
    resp = jsonErr(r.httpStatus, r.error);
  } else {
    JsonObject o;
    o["ok"] = JsonValue::makeBool(true);
    o["id"] = JsonValue::makeInt(r.explainId);
    o["task_id"] = JsonValue::makeInt(r.taskId);
    o["l1_hit"] = JsonValue::makeBool(r.l1Hit);
    o["queue_level"] = JsonValue::makeInt(r.queueLevel);
    o["position"] = JsonValue::makeInt(r.position);
    o["eta_sec"] = JsonValue::makeInt(r.etaSec);
    o["hint_transfer"] = JsonValue::makeBool(!r.l1Hit && idleHintHit(r.etaSec));
    if (r.l1Hit && !r.resultJson.empty()) {
      try { o["result"] = parseJson(r.resultJson); } catch (...) {}
    }
    resp = jsonOk(JsonValue::makeObject(o).dump());
  }
  if (!setCookie.empty()) resp.extraHeaders["Set-Cookie"] = setCookie;
  return resp;
}

static HttpResponse handleExplainGet(const HttpRequest& req)
{
  if (!g_explainSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  std::string gid = Auth::cookieValue(req.header("cookie"), "eng_guest_id");
  ExplainService::View v;
  if (!g_explainSvc->get(id, RequestContext::currentUserId(), gid, v)) return jsonErr(404, "讲解不存在");
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["item"] = explainViewToJson(v);
  o["hint_transfer"] = JsonValue::makeBool(v.status == "queued" && idleHintHit(v.etaSec));
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleExplainHistory(const HttpRequest& req)
{
  if (!g_explainSvc) return jsonErr(500, "service unavailable");
  int limit = 20, offset = 0;
  try { if (!req.queryParam("limit").empty()) limit = std::min(50, std::stoi(req.queryParam("limit"))); } catch (...) {}
  try { if (!req.queryParam("offset").empty()) offset = std::max(0, std::stoi(req.queryParam("offset"))); } catch (...) {}
  auto list = g_explainSvc->history(RequestContext::currentUserId(), limit, offset);
  JsonArray arr;
  for (auto& v : list) arr.push_back(explainViewToJson(v));
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["items"] = JsonValue::makeArray(arr);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleExplainRegenerate(const HttpRequest& req)
{
  if (!g_explainSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  JsonValue v;
  if (!req.body.empty()) { try { v = parseJson(req.body); } catch (...) {} }
  std::string depth = v.getString("depth", "");
  int level = (int)v.getInt("queue_level", 0);
  int uid = RequestContext::currentUserId();
  std::string setCookie, gid;
  if (uid <= 0) gid = guestCookieFor(req, setCookie);
  auto r = g_explainSvc->regenerate(id, uid, gid, depth, level);
  HttpResponse resp;
  if (!r.ok) {
    resp = jsonErr(r.httpStatus, r.error);
  } else {
    JsonObject o;
    o["ok"] = JsonValue::makeBool(true);
    o["id"] = JsonValue::makeInt(r.explainId);
    o["task_id"] = JsonValue::makeInt(r.taskId);
    o["queue_level"] = JsonValue::makeInt(r.queueLevel);
    o["position"] = JsonValue::makeInt(r.position);
    o["eta_sec"] = JsonValue::makeInt(r.etaSec);
    resp = jsonOk(JsonValue::makeObject(o).dump());
  }
  if (!setCookie.empty()) resp.extraHeaders["Set-Cookie"] = setCookie;
  return resp;
}

static HttpResponse handleExplainSaveKp(const HttpRequest& req)
{
  if (!g_explainSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  auto r = g_explainSvc->saveKp(id, RequestContext::currentUserId());
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["kp_id"] = JsonValue::makeInt(r.kpId);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleExplainToWordbook(const HttpRequest& req)
{
  if (!g_explainSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  JsonValue v;
  if (req.body.empty()) return jsonErr(400, "empty body");
  try { v = parseJson(req.body); } catch (...) { return jsonErr(400, "invalid JSON"); }
  int64_t wbId = v.getInt("wordbook_id", 0);
  if (wbId <= 0) return jsonErr(400, "wordbook_id required");
  ExplainService::View ev;
  if (!g_explainSvc->get(id, RequestContext::currentUserId(), "", ev)) return jsonErr(404, "讲解不存在");
  JsonObject body;
  JsonArray words;
  words.push_back(JsonValue::makeString(ev.inputText));
  body["new_words"] = JsonValue::makeArray(words);
  if (ev.l1KpId > 0) {
    JsonArray ids;
    ids.push_back(JsonValue::makeInt(ev.l1KpId));
    body["knowledge_ids"] = JsonValue::makeArray(ids);
  }
  auto r = g_wbSvc->addEntries(wbId, JsonValue::makeObject(body));
  if (r.added == 0 && !r.error.empty()) return jsonErr(400, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["added"] = JsonValue::makeInt(r.added);
  o["skipped"] = JsonValue::makeInt(r.skipped);
  return jsonOk(JsonValue::makeObject(o).dump());
}

// ===== v1.5.0-T008：精讲追问 =====
static HttpResponse handleExplainFollowupCreate(const HttpRequest& req)
{
  if (!g_explainSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch (...) { return jsonErr(400, "invalid JSON"); }
  std::string question = v.getString("question");
  int level = (int)v.getInt("queue_level", 0);
  int uid = RequestContext::currentUserId();
  std::string setCookie, gid;
  if (uid <= 0) gid = guestCookieFor(req, setCookie);
  auto r = g_explainSvc->submitFollowup(id, uid, gid, question, level);
  HttpResponse resp;
  if (!r.ok) {
    resp = jsonErr(r.httpStatus, r.error);
  } else {
    JsonObject o;
    o["ok"] = JsonValue::makeBool(true);
    o["id"] = JsonValue::makeInt(r.id);
    o["task_id"] = JsonValue::makeInt(r.taskId);
    o["position"] = JsonValue::makeInt(r.position);
    o["eta_sec"] = JsonValue::makeInt(r.etaSec);
    resp = jsonOk(JsonValue::makeObject(o).dump());
  }
  if (!setCookie.empty()) resp.extraHeaders["Set-Cookie"] = setCookie;
  return resp;
}

static HttpResponse handleExplainFollowupList(const HttpRequest& req)
{
  if (!g_explainSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  std::string gid = Auth::cookieValue(req.header("cookie"), "eng_guest_id");
  std::vector<ExplainService::FollowupView> list;
  if (!g_explainSvc->listFollowups(id, RequestContext::currentUserId(), gid, list))
    return jsonErr(404, "讲解不存在");
  JsonArray arr;
  for (auto& f : list) {
    JsonObject o;
    o["id"] = JsonValue::makeInt(f.id);
    o["question"] = JsonValue::makeString(f.question);
    o["answer_md"] = JsonValue::makeString(f.answerMd);
    o["status"] = JsonValue::makeString(f.status);
    o["merged"] = JsonValue::makeBool(f.merged == 1);
    o["merged_kp_id"] = JsonValue::makeInt(f.mergedKpId);
    o["task_id"] = JsonValue::makeInt(f.taskId);
    o["position"] = JsonValue::makeInt(f.position);
    o["eta_sec"] = JsonValue::makeInt(f.etaSec);
    o["created_at"] = JsonValue::makeString(f.createdAt);
    arr.push_back(JsonValue::makeObject(o));
  }
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["items"] = JsonValue::makeArray(arr);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleExplainFollowupMerge(const HttpRequest& req)
{
  if (!g_explainSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0, fid = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  try { fid = std::stoll(req.pathParam("fid")); } catch (...) { return jsonErr(400, "invalid fid"); }
  auto r = g_explainSvc->mergeFollowup(id, fid, RequestContext::currentUserId());
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["kp_id"] = JsonValue::makeInt(r.kpId);
  o["already"] = JsonValue::makeBool(r.already);
  return jsonOk(JsonValue::makeObject(o).dump());
}

// 加急 / 转空闲队列
static HttpResponse handleAiTaskExpedite(const HttpRequest& req)
{
  if (!g_queueSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  auto r = g_queueSvc->expedite(id, RequestContext::currentUserId());
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["queue_level"] = JsonValue::makeInt(0);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleAiTaskDefer(const HttpRequest& req)
{
  if (!g_queueSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  JsonValue v;
  if (!req.body.empty()) { try { v = parseJson(req.body); } catch (...) {} }
  int level = (int)v.getInt("level", 1);
  auto r = g_queueSvc->defer(id, RequestContext::currentUserId(), level);
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["queue_level"] = JsonValue::makeInt(level);
  return jsonOk(JsonValue::makeObject(o).dump());
}

// ===== 定制练习 =====
static HttpResponse handleCustomExerciseCreate(const HttpRequest& req)
{
  if (!g_customExSvc) return jsonErr(500, "service unavailable");
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue spec;
  try { spec = parseJson(req.body); } catch (...) { return jsonErr(400, "invalid JSON"); }
  int uid = RequestContext::currentUserId();
  std::string setCookie, gid;
  if (uid <= 0) gid = guestCookieFor(req, setCookie);
  int level = (int)spec.getInt("queue_level", 0);
  auto r = g_customExSvc->submit(uid, gid, spec, level);
  HttpResponse resp;
  if (!r.ok) {
    resp = jsonErr(r.httpStatus, r.error);
  } else {
    JsonObject o;
    o["ok"] = JsonValue::makeBool(true);
    o["id"] = JsonValue::makeInt(r.id);
    o["task_id"] = JsonValue::makeInt(r.taskId);
    o["reused"] = JsonValue::makeBool(r.reused);
    o["queue_level"] = JsonValue::makeInt(r.queueLevel);
    o["position"] = JsonValue::makeInt(r.position);
    o["eta_sec"] = JsonValue::makeInt(r.etaSec);
    o["hint_transfer"] = JsonValue::makeBool(!r.reused && idleHintHit(r.etaSec));
    resp = jsonOk(JsonValue::makeObject(o).dump());
  }
  if (!setCookie.empty()) resp.extraHeaders["Set-Cookie"] = setCookie;
  return resp;
}

static HttpResponse handleCustomExerciseGet(const HttpRequest& req)
{
  if (!g_customExSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  std::string gid = Auth::cookieValue(req.header("cookie"), "eng_guest_id");
  CustomExerciseService::View v;
  if (!g_customExSvc->get(id, RequestContext::currentUserId(), gid, v)) return jsonErr(404, "练习不存在");
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["id"] = JsonValue::makeInt(v.id);
  o["status"] = JsonValue::makeString(v.status);
  o["spec_json"] = JsonValue::makeString(v.specJson);
  o["difficulty"] = JsonValue::makeString(v.difficulty);
  o["style"] = JsonValue::makeString(v.style);
  o["item_count"] = JsonValue::makeInt(v.itemCount);
  o["queue_level"] = JsonValue::makeInt(v.queueLevel);
  o["position"] = JsonValue::makeInt(v.position);
  o["eta_sec"] = JsonValue::makeInt(v.etaSec);
  o["task_id"] = JsonValue::makeInt(v.taskId);
  o["error"] = JsonValue::makeString(v.error);
  o["created_at"] = JsonValue::makeString(v.createdAt);
  if (!v.exerciseJson.empty()) {
    try {
      JsonValue ex = parseJson(v.exerciseJson);
      if (auto* its = ex.find("items")) {
        if (its->isArray()) {
          // 下发题目时隐藏答案与解析，但保留阅读短文、多空结构与选择题选项
          JsonArray pub;
          for (auto& it : its->asArray()) {
            JsonObject q;
            q["type"] = JsonValue::makeString(it.getString("type"));
            q["word"] = JsonValue::makeString(it.getString("word"));
            q["stem"] = JsonValue::makeString(it.getString("stem"));
            if (auto* ops = it.find("options")) q["options"] = *ops;
            if (auto* ps = it.find("passage")) q["passage"] = *ps;
            if (auto* pr = it.find("prompt")) q["prompt"] = *pr;
            if (auto* sp = it.find("sample")) q["sample"] = *sp;
            if (auto* bl = it.find("blanks")) {
              if (bl->isArray()) {
                JsonArray safe;
                for (auto& b : bl->asArray()) {
                  if (!b.isObject()) continue;
                  JsonObject bo;
                  bo["no"] = JsonValue::makeInt(b.getInt("no", (int)safe.size() + 1));
                  std::string hint = b.getString("hint");
                  if (!hint.empty()) bo["hint"] = JsonValue::makeString(hint);
                  safe.push_back(JsonValue::makeObject(bo));
                }
                q["blanks"] = JsonValue::makeArray(safe);
              }
            }
            if (auto* qs = it.find("questions")) {
              if (qs->isArray()) {
                JsonArray safe;
                for (auto& one : qs->asArray()) {
                  if (!one.isObject()) continue;
                  JsonObject qo;
                  qo["no"] = JsonValue::makeInt(one.getInt("no", (int)safe.size() + 1));
                  if (auto* ops = one.find("options")) qo["options"] = *ops;
                  safe.push_back(JsonValue::makeObject(qo));
                }
                q["questions"] = JsonValue::makeArray(safe);
              }
            }
            pub.push_back(JsonValue::makeObject(q));
          }
          o["items"] = JsonValue::makeArray(pub);
          if (auto* nt = ex.find("notice")) o["notice"] = *nt;
        }
      }
    } catch (...) {}
  }
  if (!v.gradeJson.empty()) {
    try { o["grade"] = parseJson(v.gradeJson); } catch (...) {}
  }
  o["hint_transfer"] = JsonValue::makeBool(v.status == "queued" && idleHintHit(v.etaSec));
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleCustomExerciseGrade(const HttpRequest& req)
{
  if (!g_customExSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  JsonValue v;
  if (req.body.empty()) return jsonErr(400, "empty body");
  try { v = parseJson(req.body); } catch (...) { return jsonErr(400, "invalid JSON"); }
  std::string gid = Auth::cookieValue(req.header("cookie"), "eng_guest_id");
  auto r = g_customExSvc->grade(id, RequestContext::currentUserId(), gid, v);
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["score"] = JsonValue::makeInt(r.score);
  try { o["grade"] = parseJson(r.gradeJson); } catch (...) {}
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleCustomExerciseHistory(const HttpRequest& req)
{
  if (!g_customExSvc) return jsonErr(500, "service unavailable");
  int limit = 20, offset = 0;
  try { if (!req.queryParam("limit").empty()) limit = std::min(50, std::stoi(req.queryParam("limit"))); } catch (...) {}
  try { if (!req.queryParam("offset").empty()) offset = std::max(0, std::stoi(req.queryParam("offset"))); } catch (...) {}
  auto list = g_customExSvc->history(RequestContext::currentUserId(), limit, offset);
  JsonArray arr;
  for (auto& v : list) {
    JsonObject o;
    o["id"] = JsonValue::makeInt(v.id);
    o["status"] = JsonValue::makeString(v.status);
    o["difficulty"] = JsonValue::makeString(v.difficulty);
    o["style"] = JsonValue::makeString(v.style);
    o["item_count"] = JsonValue::makeInt(v.itemCount);
    o["created_at"] = JsonValue::makeString(v.createdAt);
    o["spec_json"] = JsonValue::makeString(v.specJson);
    if (!v.gradeJson.empty()) { try { o["grade"] = parseJson(v.gradeJson); } catch (...) {} }
    arr.push_back(JsonValue::makeObject(o));
  }
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["items"] = JsonValue::makeArray(arr);
  return jsonOk(JsonValue::makeObject(o).dump());
}

// v1.5.0-T009 D08：定制练习导出空白卷（latestOnly 用于游客当次导出）
static HttpResponse exportCustomExercise(const HttpRequest& req, bool latestOnly)
{
  if (!g_customExSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  std::string gid = Auth::cookieValue(req.header("cookie"), "eng_guest_id");
  auto r = g_customExSvc->exportWord(id, RequestContext::currentUserId(), gid, latestOnly);
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  std::ostringstream out;
  out << "{\"ok\":true,\"word_path\":\"" << Db::escape(r.path) << "\"}";
  return jsonOk(out.str());
}

static HttpResponse handleCustomExerciseExport(const HttpRequest& req) {
  return exportCustomExercise(req, false);
}

static HttpResponse handleExportGuestExercise(const HttpRequest& req) {
  return exportCustomExercise(req, true);
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
    << ",\"ai_providers\":" << g_ai.providersJson()
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
  bool joinStudy = v.getBool("join_study", false);
  auto r = g_kpSvc->create(content, remark);
  if (!r.ok) return jsonErr(500, r.error);
  int learning = 0;
  if (joinStudy && g_learnSvc->enrollKnowledge((int)r.id)) learning = 1;
  std::ostringstream o;
  o << "{\"id\":" << r.id << ",\"ok\":true,\"is_solidified\":0,\"visibility\":\"private\""
    << ",\"duplicate\":" << (r.duplicate ? "true" : "false")
    << ",\"learning_created\":" << learning << "}";
  return jsonOk(o.str());
}

// v1.5.0-T008：词条公开/转私人（仅在用户点按提示条时调用）
static HttpResponse handleKpVisibility(const HttpRequest& req) {
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch (...) { return jsonErr(400, "invalid JSON"); }
  bool pub = v.getBool("public", false);
  int uid = RequestContext::currentUserId();
  g_db.exec("UPDATE knowledge_points SET visibility='" + std::string(pub ? "public" : "private")
            + "' WHERE id=" + std::to_string(id) + " AND user_id=" + std::to_string(uid));
  auto rows = g_db.query("SELECT visibility FROM knowledge_points WHERE id=" + std::to_string(id)
                         + " AND user_id=" + std::to_string(uid));
  if (rows.empty()) return jsonErr(404, "词条不存在");
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["visibility"] = JsonValue::makeString(rows[0]["visibility"]);
  return jsonOk(JsonValue::makeObject(o).dump());
}

// v1.5.0-T008：查重（同一用户、同一归一化键）
static HttpResponse handleKpCheck(const HttpRequest& req) {
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  std::string text = v.getString("text");
  if (text.empty()) return jsonErr(400, "text required");
  auto r = g_kpSvc->check(text, RequestContext::currentUserId());
  JsonObject o;
  o["exists"] = JsonValue::makeBool(r.exists);
  o["id"] = r.exists ? JsonValue::makeInt((int)r.id) : JsonValue::makeNull();
  o["is_solidified"] = JsonValue::makeInt(r.is_solidified);
  o["visibility"] = JsonValue::makeString(r.visibility);
  o["normalized_key"] = JsonValue::makeString(r.normalized_key);
  return jsonOk(JsonValue::makeObject(o).dump());
}

// v1.5.0-T005：批量录入预览（去重后待新建数 + 30 条询问）
static HttpResponse handleKpPreview(const HttpRequest& req) {
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  auto* arr = v.find("words");
  if (!arr || !arr->isArray()) return jsonErr(400, "words required");
  std::vector<std::string> words;
  for (auto& el : arr->asArray()) {
    if (el.isString()) words.push_back(el.asString());
    else if (el.isObject()) words.push_back(el.getString("word", ""));
  }
  auto p = g_kpSvc->preview(words);
  JsonObject o;
  o["input"] = JsonValue::makeInt(p.input);
  o["unique"] = JsonValue::makeInt(p.unique);
  o["existing"] = JsonValue::makeInt(p.existing);
  o["to_create"] = JsonValue::makeInt(p.to_create);
  o["need_confirm"] = JsonValue::makeBool(p.need_confirm);
  return jsonOk(JsonValue::makeObject(o).dump());
}

// v1.5.0-T005：批量录入（粘贴/文件/AI/教材升级）
static HttpResponse handleKpBulk(const HttpRequest& req) {
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  auto* arr = v.find("words");
  if (!arr || !arr->isArray()) return jsonErr(400, "words required");
  std::vector<KnowledgeService::BulkItem> items;
  for (auto& el : arr->asArray()) {
    KnowledgeService::BulkItem it;
    if (el.isString()) {
      it.word = el.asString();
    } else if (el.isObject()) {
      it.word = el.getString("word", "");
      it.meaning = el.getString("meaning", "");
      it.pos = el.getString("pos", "");
      it.item_id = el.getInt("item_id", 0);
    }
    if (!it.word.empty()) items.push_back(it);
  }
  if (items.empty()) return jsonErr(400, "words empty");
  bool joinStudy = v.getBool("join_study", false);
  bool confirm = v.getBool("confirm", false);
  int64_t sourceRef = v.getInt("source_ref", 0);
  auto r = g_kpSvc->bulk(items, joinStudy, sourceRef, confirm);
  if (r.error == "need_confirm") {
    return jsonErr(400, "本次将新建超过 30 个单词，请确认是否加入学习");
  }
  if (!r.ok) return jsonErr(500, r.error);
  JsonObject o;
  o["created"] = JsonValue::makeInt(r.created);
  o["skipped"] = JsonValue::makeInt(r.skipped);
  o["learning_created"] = JsonValue::makeInt(r.learning_created);
  o["definitions_written"] = JsonValue::makeInt(r.definitions_written);
  return jsonOk(JsonValue::makeObject(o).dump());
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

static JsonValue jobViewJson(const SolidifyJobView& v)
{
  JsonObject o;
  o["job_id"] = JsonValue::makeString(v.id);
  o["status"] = JsonValue::makeString(v.status);
  o["total"] = JsonValue::makeInt(v.total);
  o["done"] = JsonValue::makeInt(v.done);
  o["failed"] = JsonValue::makeInt(v.failed);
  o["progress"] = JsonValue::makeInt(v.progress);
  o["queue_position"] = JsonValue::makeInt(v.queuePosition);
  o["eta_sec"] = JsonValue::makeInt(v.etaSec);
  o["create_time"] = JsonValue::makeString(v.createTime);
  o["update_time"] = JsonValue::makeString(v.updateTime);
  if (!v.error.empty()) o["error"] = JsonValue::makeString(v.error);
  JsonArray ids;
  for (int64_t id : v.completedIds) ids.push_back(JsonValue::makeInt(id));
  o["completed_ids"] = JsonValue::makeArray(ids);
  JsonArray failed;
  for (auto& f : v.failedItems)
  {
    JsonObject fo;
    fo["knowledge_id"] = JsonValue::makeInt(f.knowledgeId);
    fo["input"] = JsonValue::makeString(f.input);
    fo["error"] = JsonValue::makeString(f.error);
    failed.push_back(JsonValue::makeObject(fo));
  }
  o["failed_items"] = JsonValue::makeArray(failed);
  return JsonValue::makeObject(o);
}

static HttpResponse handleKpSolidifyAsync(const HttpRequest& req)
{
  auto rows = g_db.query("SELECT id FROM knowledge_points WHERE is_solidified=0 AND origin='manual' AND user_id="
                         + std::to_string(RequestContext::currentUserId()) + " ORDER BY id");
  std::vector<int64_t> ids;
  for (auto& row : rows) ids.push_back(std::stoll(row["id"]));
  if (ids.empty())
    return jsonOk("{\"job_id\":\"\",\"total\":0,\"done\":0,\"failed\":0,\"status\":\"done\",\"reused\":false}");
  auto r = g_jobSvc->createOrReuse(RequestContext::currentUserId(), ids);
  if (r.code != 200) return jsonErr(r.code, r.error);
  JsonObject o;
  o["job_id"] = JsonValue::makeString(r.jobId);
  o["total"] = JsonValue::makeInt(r.total);
  o["done"] = JsonValue::makeInt(0);
  o["failed"] = JsonValue::makeInt(0);
  o["status"] = JsonValue::makeString(r.status);
  o["reused"] = JsonValue::makeBool(r.reused);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleKpSolidifyJobGet(const HttpRequest& req)
{
  SolidifyJobView v;
  if (!g_jobSvc->get(req.pathParam("job_id"), RequestContext::currentUserId(), v))
    return jsonErr(404, "job not found");
  return jsonOk(jobViewJson(v).dump());
}

static HttpResponse handleKpSolidifyJobList(const HttpRequest& req)
{
  int limit = 20, offset = 0;
  try { limit = std::stoi(req.queryParam("limit")); } catch (...) {}
  try { offset = std::stoi(req.queryParam("offset")); } catch (...) {}
  auto jobs = g_jobSvc->list(RequestContext::currentUserId(), limit, offset);
  JsonArray arr;
  for (auto& v : jobs) arr.push_back(jobViewJson(v));
  return jsonOk(JsonValue::makeObject({{"data", JsonValue::makeArray(arr)}}).dump());
}

static HttpResponse handleKpSolidifyJobResume(const HttpRequest& req)
{
  auto r = g_jobSvc->resume(req.pathParam("job_id"), RequestContext::currentUserId());
  if (!r.ok) return jsonErr(r.code, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["job_id"] = JsonValue::makeString(req.pathParam("job_id"));
  o["status"] = JsonValue::makeString(r.status);
  return jsonOk(JsonValue::makeObject(o).dump());
}

// v1.5.0-T002：通用 AI 任务接口 /api/ai-tasks
static JsonValue aiTaskJson(const AiTaskView& t)
{
  JsonObject o;
  o["id"] = JsonValue::makeInt((int)t.id);
  o["kind"] = JsonValue::makeString(t.kind);
  o["lane"] = JsonValue::makeString(t.lane);
  o["status"] = JsonValue::makeString(t.status);
  o["total"] = JsonValue::makeInt(t.total);
  o["done"] = JsonValue::makeInt(t.done);
  o["failed"] = JsonValue::makeInt(t.failed);
  o["progress"] = JsonValue::makeInt(t.progress);
  o["retry_count"] = JsonValue::makeInt(t.retryCount);
  o["queue_position"] = JsonValue::makeInt(t.position);
  o["eta_sec"] = JsonValue::makeInt(t.etaSec);
  o["create_time"] = JsonValue::makeString(t.createdAt);
  o["started_at"] = JsonValue::makeString(t.startedAt);
  o["finished_at"] = JsonValue::makeString(t.finishedAt);
  o["model_used"] = JsonValue::makeString(t.modelUsed);
  if (!t.resultRef.empty()) o["result_ref"] = JsonValue::makeString(t.resultRef);
  if (!t.error.empty()) o["error"] = JsonValue::makeString(t.error);
  JsonArray failed;
  for (auto& f : t.failedItems)
  {
    JsonObject fo;
    fo["item_index"] = JsonValue::makeInt(f.index);
    fo["ref_id"] = JsonValue::makeInt((int)f.refId);
    fo["input"] = JsonValue::makeString(f.input);
    fo["error"] = JsonValue::makeString(f.error);
    failed.push_back(JsonValue::makeObject(fo));
  }
  o["failed_items"] = JsonValue::makeArray(failed);
  return JsonValue::makeObject(o);
}

static HttpResponse handleAiTaskListNew(const HttpRequest& req)
{
  int limit = 20, offset = 0;
  try { if (!req.queryParam("limit").empty()) limit = std::stoi(req.queryParam("limit")); } catch (...) {}
  try { if (!req.queryParam("offset").empty()) offset = std::stoi(req.queryParam("offset")); } catch (...) {}
  auto tasks = g_queueSvc->list(RequestContext::currentUserId(), req.queryParam("kind"), limit, offset);
  JsonArray arr;
  for (auto& t : tasks) arr.push_back(aiTaskJson(t));
  return jsonOk(JsonValue::makeObject({{"data", JsonValue::makeArray(arr)}}).dump());
}

static HttpResponse handleAiTaskGetNew(const HttpRequest& req)
{
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  AiTaskView t;
  if (!g_queueSvc->get(id, RequestContext::currentUserId(), t)) return jsonErr(404, "task not found");
  return jsonOk(aiTaskJson(t).dump());
}

static HttpResponse handleAiTaskRetryNew(const HttpRequest& req)
{
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  auto r = g_queueSvc->retry(id, RequestContext::currentUserId());
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["id"] = JsonValue::makeInt((int)id);
  o["status"] = JsonValue::makeString("queued");
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleKpSolidifyAll(const HttpRequest& req) {
  // 查询所有未固化的知识点ID，一次AI调用批量固化（教材词已固化，仅手工录入参与）
  auto rows = g_db.query("SELECT id FROM knowledge_points WHERE is_solidified=0 AND origin='manual' AND user_id="
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
  if (!r.failed_reasons.empty()) {
    JsonArray arr;
    for (auto& s : r.failed_reasons) arr.push_back(JsonValue::makeString(s));
    o["failed_reasons"] = JsonValue::makeArray(arr);
  }
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
  auto result = g_kpSvc->solidifyOne(id, rows[0]["user_input_content"], rows[0]["remark"], err);
  if (!result.ok) return jsonErr(500, result.error.empty() ? err : result.error);
  int uid = RequestContext::currentUserId();
  g_treasurySvc->checkAndAwardMasterySeals(uid);
  g_treasurySvc->checkAndAwardTitle(uid);
  std::string wordPath = g_kpSvc->exportWord(result.effective_id);
  std::ostringstream o;
  o << "{\"id\":" << result.effective_id << ",\"ok\":true,\"word_path\":\"" << Db::escape(wordPath) << "\"}";
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

// v1.5.0-T005：正在学习列表（filter=all|due|learning；sort=due|proficiency|recent）
static HttpResponse handleLearnStudying(const HttpRequest& req) {
  int page = 1, size = 20;
  try { page = std::stoi(req.queryParam("page").empty() ? "1" : req.queryParam("page")); } catch(...) {}
  try { size = std::stoi(req.queryParam("size").empty() ? "20" : req.queryParam("size")); } catch(...) {}
  if (page < 1) page = 1;
  if (size < 1) size = 20;
  if (size > 200) size = 200;
  auto v = g_learnSvc->studying(req.queryParam("filter"), req.queryParam("sort"), page, size);
  return jsonOk(v.dump());
}

// v1.5.0-T005：学习汇总 {to_learn, learning, mastered, due_today}
static HttpResponse handleLearnSummary(const HttpRequest& req) {
  auto v = g_learnSvc->summary();
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

// v1.5.0-T009 D07：复习卷按需导出（前端当前试卷 JSON，不调 AI）
static HttpResponse handleExerciseExport(const HttpRequest& req) {
  if (!g_qSvc) return jsonErr(500, "service unavailable");
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch (...) { return jsonErr(400, "invalid JSON"); }
  auto r = g_qSvc->exportPaper(v);
  if (!r.ok) return jsonErr(r.httpStatus, "导出服务暂不可用");
  std::ostringstream o;
  o << "{\"ok\":true,\"word_path\":\"" << Db::escape(r.path) << "\"}";
  return jsonOk(o.str());
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
  // 长篇阅读语法填空 / 完整完形填空：多空答案以数组提交，序列化后交给 QuestionService 逐空批改
  if (type == "long_reading_cloze" || type == "full_cloze") {
    if (const JsonValue* ua = v.find("user_answers")) {
      if (ua->isArray()) userAns = ua->dump();
    }
    if (const JsonValue* ca = v.find("correct_answers")) {
      if (ca->isArray()) correctAns = ca->dump();
    }
  }
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

  // v1.5.1-T002：答错时写入错题归因并返回
  int kid = (int)v.getInt("knowledge_id", 0);
  if (!r.correct && kid > 0) {
    ErrorService::WrongInput wi;
    wi.userId = RequestContext::currentUserId();
    wi.knowledgeId = kid;
    wi.sourceType = v.getString("source_type", "exercise");
    wi.sourceId = v.getString("source_id");
    wi.questionNo = (int)v.getInt("question_no", 0);
    wi.questionType = v.getString("question_type", type);
    wi.userAnswer = userAns;
    wi.correctAnswer = correctAns.empty() ? refAns : correctAns;
    if (const JsonValue* opts = v.find("options")) wi.options = *opts;
    auto a = g_errorSvc->recordWrong(wi);
    if (a.created) {
      JsonObject ao;
      ao["id"] = JsonValue::makeInt(a.id);
      ao["error_type"] = JsonValue::makeString(a.errorType);
      ao["error_label"] = JsonValue::makeString(ErrorService::errorLabel(a.errorType));
      ao["reason"] = JsonValue::makeString(a.reason);
      ao["source"] = JsonValue::makeString(a.source);
      ao["pending"] = JsonValue::makeBool(a.pending);
      o["attribution"] = JsonValue::makeObject(ao);
    }
  }
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

// ============ v1.5.1-T002：错题归因与重练 ============

static bool parseRetryId(const HttpRequest& req, long long& id) {
  try { id = std::stoll(req.pathParam("id")); return id > 0; }
  catch (...) { return false; }
}

static HttpResponse handleErrorAttributionGet(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  long long id = 0;
  if (!parseRetryId(req, id)) return jsonErr(400, "invalid id");
  auto v = g_errorSvc->getAttribution(uid, id);
  if (v.isNull()) return jsonErr(404, "not found");
  return jsonOk(v.dump());
}

static HttpResponse handleErrorAttributionUpdate(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  long long id = 0;
  if (!parseRetryId(req, id)) return jsonErr(400, "invalid id");
  JsonValue v;
  try { v = parseJson(req.body); } catch (...) { return jsonErr(400, "invalid JSON"); }
  std::string type = v.getString("error_type");
  if (!ErrorService::isValidType(type)) return jsonErr(400, "invalid error_type");
  bool ok = g_errorSvc->updateAttributionType(uid, id, type);
  return jsonOk(std::string("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static HttpResponse handleWrongbookSummary(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  return jsonOk(g_errorSvc->summary(uid).dump());
}

static HttpResponse handleWrongbookWords(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  int days = 0, page = 1, size = 20;
  try { if (!req.queryParam("days").empty()) days = std::stoi(req.queryParam("days")); } catch (...) {}
  try { if (!req.queryParam("page").empty()) page = std::stoi(req.queryParam("page")); } catch (...) {}
  try { if (!req.queryParam("size").empty()) size = std::stoi(req.queryParam("size")); } catch (...) {}
  bool includeArchived = req.queryParam("include_archived") == "1";
  std::string keyword = req.queryParam("keyword");
  if (keyword.empty()) keyword = req.queryParam("q");
  auto v = g_errorSvc->wrongbookWords(uid, req.queryParam("error_type"),
      req.queryParam("source_type"), days, keyword, includeArchived, page, size);
  return jsonOk(v.dump());
}

// v1.5.1-T003：学情周报（实时聚合 + AI 文本缓存）
static HttpResponse handleWeeklyReport(const HttpRequest& req)
{
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  if (!g_weeklySvc) return jsonErr(500, "service unavailable");
  if (req.queryParam("summary") == "1") return jsonOk(g_weeklySvc->summary(uid).dump());
  bool includeWordlist = req.queryParam("include_wordlist") == "1";
  return jsonOk(g_weeklySvc->report(uid, req.queryParam("week"), includeWordlist).dump());
}

static HttpResponse handleWeeklyReportAi(const HttpRequest& req)
{
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  if (!g_weeklySvc) return jsonErr(500, "service unavailable");
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (...) { return jsonErr(400, "invalid JSON"); }
  auto r = g_weeklySvc->submitAi(uid, v.getString("week_start"));
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["task_id"] = JsonValue::makeInt((int)r.taskId);
  o["status"] = JsonValue::makeString(r.status);
  o["cached"] = JsonValue::makeBool(r.cached);
  o["text"] = JsonValue::makeString(r.text);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleAiNavSend(const HttpRequest& req)
{
  if (!g_aiNavSvc) return jsonErr(500, "service unavailable");
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (...) { return jsonErr(400, "invalid JSON"); }

  int uid = RequestContext::currentUserId();
  std::string setCookie, gid;
  if (uid <= 0) gid = guestCookieFor(req, setCookie);

  std::vector<std::pair<std::string, std::string>> history;
  const JsonValue* h = v.find("history");
  if (h && h->isArray())
  {
    int n = 0;
    for (const auto& m : h->asArray())
    {
      if (n++ >= 6) break;
      std::string content = m.getString("content");
      if (content.empty()) continue;
      history.push_back({m.getString("role"), content});
    }
  }

  std::string text = v.getString("text");
  if (text.empty()) text = v.getString("message");
  std::string page = v.getString("page");
  auto r = g_aiNavSvc->send(uid, gid, text, page, history);

  HttpResponse resp;
  if (!r.ok)
  {
    resp = jsonErr(r.httpStatus, r.error);
  }
  else
  {
    JsonObject o;
    o["ok"] = JsonValue::makeBool(true);
    o["reply"] = r.data;
    o["meta"] = g_aiNavSvc->meta(page, uid <= 0);
    resp = jsonOk(JsonValue::makeObject(o).dump());
  }
  if (!setCookie.empty()) resp.extraHeaders["Set-Cookie"] = setCookie;
  return resp;
}

static HttpResponse handleAiNavHistory(const HttpRequest& req)
{
  if (!g_aiNavSvc) return jsonErr(500, "service unavailable");
  int uid = RequestContext::currentUserId();
  std::string setCookie, gid;
  if (uid <= 0) gid = guestCookieFor(req, setCookie);

  JsonValue h = g_aiNavSvc->history(uid, req.queryParam("page"));
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  for (const auto& kv : h.asObject()) o[kv.first] = kv.second;
  HttpResponse resp = jsonOk(JsonValue::makeObject(o).dump());
  if (!setCookie.empty()) resp.extraHeaders["Set-Cookie"] = setCookie;
  return resp;
}

static HttpResponse handleAiNavClear(const HttpRequest&)
{
  if (!g_aiNavSvc) return jsonErr(500, "service unavailable");
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "请先登录");
  g_aiNavSvc->clear(uid);
  return jsonOk("{\"ok\":true}");
}

static HttpResponse handleAiNavConfirm(const HttpRequest& req)
{
  if (!g_aiNavSvc) return jsonErr(500, "service unavailable");
  long long id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) {}
  if (id <= 0) return jsonErr(400, "invalid id");
  std::string when = "today";
  if (!req.body.empty())
  {
    try { when = parseJson(req.body).getString("activate_from", "today"); }
    catch (...) {}
  }
  auto r = g_aiNavSvc->confirm(RequestContext::currentUserId(), id, when);
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  for (const auto& kv : r.data.asObject()) o[kv.first] = kv.second;
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleAiNavDiscard(const HttpRequest& req)
{
  if (!g_aiNavSvc) return jsonErr(500, "service unavailable");
  long long id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) {}
  if (id <= 0) return jsonErr(400, "invalid id");
  auto r = g_aiNavSvc->discard(RequestContext::currentUserId(), id);
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  return jsonOk("{\"ok\":true}");
}

// ---------- v1.5.2-T001 DEV-INTAKE：开发者台账（仅 admin） ----------
// 未登录 401；已登录非 admin 403（不泄露条目数量）
static int devGuard()
{
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return 401;
  if (!g_devSvc || !g_devSvc->isAdmin(uid)) return 403;
  return 0;
}
static HttpResponse devDenied(int code)
{
  return jsonErr(code, code == 401 ? "login required" : "此页面仅开发者可访问");
}

static HttpResponse handleDevBacklogList(const HttpRequest& req)
{
  if (int code = devGuard()) return devDenied(code);
  return jsonOk(g_devSvc->backlogList(req.queryParam("status"),
                                      req.queryParam("scope_fit"),
                                      req.queryParam("priority")).dump());
}

static HttpResponse handleDevBacklogUpdate(const HttpRequest& req)
{
  if (int code = devGuard()) return devDenied(code);
  int id = 0;
  try { id = std::stoi(req.pathParam("id")); } catch (...) {}
  JsonValue body;
  try { body = parseJson(req.body); } catch (...) { return jsonErr(400, "invalid json"); }
  auto r = g_devSvc->updateBacklog(id, body);
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  return jsonOk(r.data.dump());
}

static HttpResponse handleDevBugList(const HttpRequest& req)
{
  if (int code = devGuard()) return devDenied(code);
  return jsonOk(g_devSvc->bugList(req.queryParam("status"),
                                  req.queryParam("severity"),
                                  req.queryParam("category")).dump());
}

static HttpResponse handleDevBugUpdate(const HttpRequest& req)
{
  if (int code = devGuard()) return devDenied(code);
  int id = 0;
  try { id = std::stoi(req.pathParam("id")); } catch (...) {}
  JsonValue body;
  try { body = parseJson(req.body); } catch (...) { return jsonErr(400, "invalid json"); }
  auto r = g_devSvc->updateBug(id, body);
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  return jsonOk(r.data.dump());
}

static HttpResponse handleDevSummary(const HttpRequest&)
{
  if (int code = devGuard()) return devDenied(code);
  return jsonOk(g_devSvc->summary().dump());
}

static HttpResponse handleWrongbookWordDetail(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  int kid = 0;
  try { kid = std::stoi(req.pathParam("kid")); } catch (...) { return jsonErr(400, "invalid id"); }
  auto v = g_errorSvc->wordDetail(uid, kid);
  if (v.isNull()) return jsonErr(404, "not found");
  return jsonOk(v.dump());
}

static HttpResponse handleRetryItems(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  bool dueOnly = req.queryParam("due") == "1" || req.queryParam("due_only") == "1";
  return jsonOk(g_errorSvc->retryQueue(uid, dueOnly).dump());
}

static HttpResponse handleRetryAdvance(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  long long id = 0;
  if (!parseRetryId(req, id)) return jsonErr(400, "invalid id");
  bool ok = g_errorSvc->advance(uid, id);
  return jsonOk(std::string("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static HttpResponse handleRetrySkip(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  long long id = 0;
  if (!parseRetryId(req, id)) return jsonErr(400, "invalid id");
  bool ok = g_errorSvc->skip(uid, id);
  return jsonOk(std::string("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static HttpResponse handleRetryComplete(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  long long id = 0;
  if (!parseRetryId(req, id)) return jsonErr(400, "invalid id");
  bool correct = false;
  if (!req.body.empty()) {
    try {
      JsonValue v = parseJson(req.body);
      correct = v.find("is_correct") ? v.getBool("is_correct", false) : v.getBool("correct", false);
    }
    catch (...) { return jsonErr(400, "invalid JSON"); }
  }
  auto v = g_errorSvc->complete(uid, id, correct);
  if (!v.getBool("ok", false)) return jsonErr(400, v.getString("error", "重练失败"));
  return jsonOk(v.dump());
}

static HttpResponse handleRetryArchive(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  long long id = 0;
  if (!parseRetryId(req, id)) return jsonErr(400, "invalid id");
  bool ok = g_errorSvc->archive(uid, id);
  return jsonOk(std::string("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static HttpResponse handleRetryRestore(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  long long id = 0;
  if (!parseRetryId(req, id)) return jsonErr(400, "invalid id");
  bool ok = g_errorSvc->restore(uid, id);
  return jsonOk(std::string("{\"ok\":") + (ok ? "true" : "false") + "}");
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

// v1.5.0-T011：AI 成本统计（登录用户看自己；系统账号 user_id=1 可加 scope=all 看全局）
static HttpResponse handleOpsAiCost(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");
  int days = 7;
  try { if (!req.queryParam("days").empty()) days = std::stoi(req.queryParam("days")); } catch (...) {}
  bool all = (uid == 1) && req.queryParam("scope") == "all";
  return jsonOk(g_opsSvc->aiCost(uid, days, all).dump());
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

// v1.5.0-T009 D09/D10：词表库单词卡导出（递归子本去重）
static HttpResponse handleWlExportWord(const HttpRequest& req) {
  if (!g_exportSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  auto r = g_exportSvc->exportWordlistCards(id);
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  std::ostringstream o;
  o << "{\"ok\":true,\"word_path\":\"" << Db::escape(r.path) << "\"}";
  return jsonOk(o.str());
}

// v1.5.0-T009：导出中心汇总（登录）
static HttpResponse handleExportsSummary(const HttpRequest& req) {
  if (!g_exportSvc) return jsonErr(500, "service unavailable");
  return jsonOk(g_exportSvc->summary(RequestContext::currentUserId()).dump());
}

// v1.5.0-T009 D12：讲解导出（游客仅当次最新一条 done；不写历史、不计 AI 次数）
static HttpResponse handleExportExplanation(const HttpRequest& req) {
  if (!g_exportSvc) return jsonErr(500, "service unavailable");
  int64_t id = 0;
  try { id = std::stoll(req.pathParam("id")); } catch (...) { return jsonErr(400, "invalid id"); }
  std::string gid = Auth::cookieValue(req.header("cookie"), "eng_guest_id");
  auto r = g_exportSvc->exportExplanation(id, RequestContext::currentUserId(), gid);
  if (!r.ok) return jsonErr(r.httpStatus, r.error);
  std::ostringstream o;
  o << "{\"ok\":true,\"word_path\":\"" << Db::escape(r.path) << "\"}";
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
    auto result = g_kpSvc->solidifyOne(refId, kp[0]["user_input_content"], kp[0]["remark"], err);
    std::ostringstream o;
    o << "{\"ok\":" << (result.ok?"true":"false") << ",\"error\":\"" << Db::escape(err) << "\"}";
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
    auto result = g_kpSvc->solidifyOne(refId, kp[0]["user_input_content"], kp[0]["remark"], err);
    if (result.ok) success++;
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

  // v1.5.0-T006-T7：登录保留游客本次讲解/练习（keep=1，一次性挂到本人历史）
  if (v.getBool("keep", false))
  {
    std::string gid = Auth::cookieValue(req.header("cookie"), "eng_guest_id");
    if (!gid.empty())
    {
      std::string g = Db::escape(gid);
      g_db.exec("UPDATE explanations SET user_id=" + std::to_string(r.user_id) +
                ", guest_id=NULL WHERE user_id=0 AND guest_id='" + g + "'");
      g_db.exec("UPDATE custom_exercises SET user_id=" + std::to_string(r.user_id) +
                ", guest_id=NULL WHERE user_id=0 AND guest_id='" + g + "'");
    }
  }

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
  if (uid <= 0) return jsonErr(403, "游客账号不可注销");
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
  if (uid <= 0)
  {
    o << "{\"user_id\":0,\"guest\":true,\"ui_mode\":\"function\"}";
  }
  else
  {
    auto info = g_authSvc->userInfo(uid);
    if (info.empty()) { o << "{\"user_id\":0,\"guest\":true,\"ui_mode\":\"function\"}"; }
    else
    {
      o << "{\"user_id\":" << uid
        << ",\"guest\":false,\"username\":\"" << Db::escape(info["username"])
        << "\",\"display_name\":\"" << Db::escape(info["display_name"]) << "\""
        << ",\"learner_identity\":\"" << Db::escape(info["learner_identity"]) << "\""
        << ",\"ui_mode\":\"" << Db::escape(info["ui_mode"].empty() ? "function" : info["ui_mode"]) << "\"}";
    }
  }
  return jsonOk(o.str());
}

// v1.3.0-T001：更新用户资料（display_name / learner_identity）
// v1.5.0-T004：新增 ui_mode（function / plan）
static HttpResponse handleAuthProfile(const HttpRequest& req) {
  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(403, "游客账号不可修改资料");
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); } catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  if (!v.isObject()) return jsonErr(400, "not object");
  auto r = g_authSvc->updateProfile(uid, v.getString("display_name"), v.getString("learner_identity"), v.getString("ui_mode"));
  if (!r.ok) return jsonErr(r.code, r.error);
  std::ostringstream o;
  o << "{\"ok\":true";
  if (!r.display_name.empty()) o << ",\"display_name\":\"" << Db::escape(r.display_name) << "\"";
  if (!r.learner_identity.empty()) o << ",\"learner_identity\":\"" << Db::escape(r.learner_identity) << "\"";
  if (!r.ui_mode.empty()) o << ",\"ui_mode\":\"" << Db::escape(r.ui_mode) << "\"";
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

  // v1.5.1-T002：已固化题答错写入错题归因（未作答不计）
  if (!r.alreadyDone && g_errorSvc) {
    JsonArray attrs;
    for (auto& res : r.results) {
      if (res.getBool("correct", true)) continue;
      int kid = (int)res.getInt("knowledge_id", 0);
      std::string ua = res.getString("user_answer");
      if (kid <= 0 || ua.empty()) continue;
      ErrorService::WrongInput wi;
      wi.userId = uid;
      wi.knowledgeId = kid;
      wi.sourceType = "quiz";
      wi.sourceId = std::to_string(quizId);
      wi.questionNo = (int)res.getInt("no", 0);
      wi.questionType = res.getString("question_type");
      wi.userAnswer = ua;
      wi.correctAnswer = res.getString("correct_answer");
      if (const JsonValue* opts = res.find("options")) wi.options = *opts;
      auto a = g_errorSvc->recordWrong(wi);
      if (a.created) {
        JsonObject ao;
        ao["no"] = JsonValue::makeInt(wi.questionNo);
        ao["id"] = JsonValue::makeInt(a.id);
        ao["error_type"] = JsonValue::makeString(a.errorType);
        ao["error_label"] = JsonValue::makeString(ErrorService::errorLabel(a.errorType));
        ao["reason"] = JsonValue::makeString(a.reason);
        ao["pending"] = JsonValue::makeBool(a.pending);
        attrs.push_back(JsonValue::makeObject(ao));
      }
    }
    o["attributions"] = JsonValue::makeArray(attrs);
  }
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
  JsonObject o;
  // 先结算本周任务卡（可能自动发放全完成奖励与印章），再读取余额/流水/印章
  o["weekly_goal"] = g_treasurySvc->getWeeklyGoal(uid);
  o["weekly_plans"] = JsonValue::makeArray(g_treasurySvc->getWeeklyPlans(uid));
  o["week_reward"] = g_treasurySvc->getWeekReward(uid);
  o["suggestions"] = JsonValue::makeArray(g_treasurySvc->getSuggestions(uid));

  auto [balance, totalEarned, totalSpent] = g_treasurySvc->getBalance(uid);
  o["balance"] = JsonValue::makeInt(balance);
  o["total_earned"] = JsonValue::makeInt(totalEarned);
  o["total_spent"] = JsonValue::makeInt(totalSpent);
  o["txns"] = JsonValue::makeArray(g_treasurySvc->getTxns(uid, 10));
  o["seals"] = JsonValue::makeArray(g_treasurySvc->getSeals(uid));
  o["current_title"] = JsonValue::makeString(g_treasurySvc->getCurrentTitle(uid));
  o["mastery_count"] = JsonValue::makeInt(g_treasurySvc->getMasteryCount(uid));
  o["solidify_count"] = JsonValue::makeInt(g_treasurySvc->getSolidifyCount(uid));
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
  std::string metric = v.getString("metric_key");
  if (metric.empty()) metric = v.getString("goal_type");   // 旧字段兼容
  int target = (int)v.getInt("target", 0);
  int goalId = (int)v.getInt("goal_id", 0);
  if (metric.empty() || target <= 0) return jsonErr(400, "metric_key and target required");

  int uid = RequestContext::currentUserId();
  if (goalId <= 0)
  {
    auto cur = g_db.query(
      "SELECT id FROM user_goals WHERE user_id=" + std::to_string(uid)
      + " AND is_current=1 AND status='active' LIMIT 1");
    goalId = cur.empty() ? 0 : std::stoi(cur[0]["id"]);
  }
  if (goalId <= 0)
  {
    // 旧接口兼容：无课规时建立默认课规
    if (!g_treasurySvc->setWeeklyGoal(uid, metric, target)) return jsonErr(400, "invalid metric_key");
    JsonObject o;
    o["ok"] = JsonValue::makeBool(true);
    o["weekly_goal"] = g_treasurySvc->getWeeklyGoal(uid);
    return jsonOk(JsonValue::makeObject(o).dump());
  }

  g_treasurySvc->ensureWeeklyTasks(uid, goalId);
  int eff = 0;
  std::string err;
  if (!g_treasurySvc->setWeeklyTaskTarget(uid, goalId, metric, target, eff, err))
    return jsonErr(400, err);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["goal_id"] = JsonValue::makeInt(goalId);
  o["metric_key"] = JsonValue::makeString(metric);
  o["target"] = JsonValue::makeInt(target);
  o["effective_target"] = JsonValue::makeInt(eff);
  o["weekly_goal"] = g_treasurySvc->getWeeklyGoal(uid);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleTreasuryNextWeekTasks(const HttpRequest& req)
{
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  int goalId = (int)v.getInt("goal_id", 0);
  int uid = RequestContext::currentUserId();
  if (goalId <= 0)
  {
    auto cur = g_db.query(
      "SELECT id FROM user_goals WHERE user_id=" + std::to_string(uid)
      + " AND is_current=1 AND status='active' LIMIT 1");
    goalId = cur.empty() ? 0 : std::stoi(cur[0]["id"]);
  }
  if (goalId <= 0) return jsonErr(400, "请先设定课规");
  const JsonValue* tasks = v.find("tasks");
  if (!tasks || !tasks->isArray()) return jsonErr(400, "tasks required");
  std::string err;
  if (!g_treasurySvc->saveNextWeekTargets(uid, goalId, tasks->asArray(), err))
    return jsonErr(400, err);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["goal_id"] = JsonValue::makeInt(goalId);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleTreasuryClaimWeek(const HttpRequest& req)
{
  JsonValue v;
  if (!req.body.empty())
  {
    try { v = parseJson(req.body); } catch (...) {}
  }
  std::string trigger = v.getString("trigger", "panel");
  if (trigger != "panel" && trigger != "complete") trigger = "panel";
  int amount = g_treasurySvc->claimWeeklyRewards(RequestContext::currentUserId(), trigger);
  int uid = RequestContext::currentUserId();
  auto [balance, earned, spent] = g_treasurySvc->getBalance(uid);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["credited"] = JsonValue::makeInt(amount);
  o["balance"] = JsonValue::makeInt(balance);
  o["weekly_plans"] = JsonValue::makeArray(g_treasurySvc->getWeeklyPlans(uid));
  o["week_reward"] = g_treasurySvc->getWeekReward(uid);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleMetrics(const HttpRequest& req)
{
  JsonObject o;
  o["metrics"] = JsonValue::makeArray(g_treasurySvc->getMetrics());
  return jsonOk(JsonValue::makeObject(o).dump());
}

// ============ v1.5.1-T001：课规 ============

static HttpResponse handleGoalParse(const HttpRequest& req)
{
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  std::string text = v.getString("text");
  if (text.empty()) return jsonErr(400, "text required");
  return jsonOk(g_goalSvc->parseText(text).dump());
}

static HttpResponse handleGoalList(const HttpRequest& req)
{
  JsonObject o;
  o["goals"] = JsonValue::makeArray(g_goalSvc->list(RequestContext::currentUserId()));
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleGoalCreate(const HttpRequest& req)
{
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  int uid = RequestContext::currentUserId();
  std::string err;
  int id = g_goalSvc->create(uid, v, err);
  if (id <= 0) return jsonErr(400, err);
  g_treasurySvc->ensureWeeklyTasks(uid, id);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["id"] = JsonValue::makeInt(id);
  o["goals"] = JsonValue::makeArray(g_goalSvc->list(uid));
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleGoalUpdate(const HttpRequest& req)
{
  if (req.body.empty()) return jsonErr(400, "empty body");
  JsonValue v;
  try { v = parseJson(req.body); }
  catch (const std::exception& e) { return jsonErr(400, std::string("invalid JSON: ") + e.what()); }
  int id = 0;
  try { id = std::stoi(req.pathParam("id")); } catch (...) {}
  if (id <= 0) return jsonErr(400, "invalid id");
  int uid = RequestContext::currentUserId();
  // 同一请求可同时修改字段与切换；仅切换时不动其他字段
  bool hasFields = v.find("name") || v.find("deadline") || v.find("daily_minutes")
    || v.find("daily_new_words") || v.find("wordlist_id") || v.find("target_desc")
    || v.find("goal_type");
  if (hasFields)
  {
    std::string err;
    if (!g_goalSvc->update(uid, id, v, err)) return jsonErr(400, err);
  }
  if (v.getBool("is_current", false))
  {
    if (!g_goalSvc->setCurrent(uid, id)) return jsonErr(404, "课规不存在");
    g_treasurySvc->switchCurrentGoal(uid, id);
  }
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["goals"] = JsonValue::makeArray(g_goalSvc->list(uid));
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleGoalDelete(const HttpRequest& req)
{
  int id = 0;
  try { id = std::stoi(req.pathParam("id")); } catch (...) {}
  if (id <= 0) return jsonErr(400, "invalid id");
  int uid = RequestContext::currentUserId();
  if (!g_goalSvc->remove(uid, id)) return jsonErr(404, "课规不存在");
  // 提升后的新课规从此刻起计本周任务
  int cur = g_goalSvc->currentGoalId(uid);
  if (cur > 0) g_treasurySvc->ensureWeeklyTasks(uid, cur);
  JsonObject o;
  o["ok"] = JsonValue::makeBool(true);
  o["goals"] = JsonValue::makeArray(g_goalSvc->list(uid));
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


// 文件是否存在（用于判断候选根目录是否有效）
static bool fileExists(const std::string& p)
{
  std::ifstream f(p);
  return static_cast<bool>(f);
}

// 取父目录（无斜杠时返回空串）
static std::string dirOf(const std::string& p)
{
  size_t pos = p.find_last_of('/');
  return pos == std::string::npos ? std::string() : p.substr(0, pos);
}

// realpath 解析；失败返回空串
static std::string canonicalPath(const std::string& p)
{
  if (p.empty()) return std::string();
  char buf[PATH_MAX];
  return realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}

// 根目录三级回退：APP_ROOT 环境变量 → 可执行文件上级目录 → 当前工作目录
// 以 config/db.json 是否存在判定候选是否有效；--root 参数在 main 中优先覆盖
static std::string resolveRoot(const char* argv0)
{
  const char* envRoot = std::getenv("APP_ROOT");
  if (envRoot && *envRoot) return envRoot;

  std::string exe = canonicalPath(argv0 ? argv0 : "");
  if (exe.empty() && argv0) exe = argv0;
  std::string exeRoot = canonicalPath(dirOf(exe) + "/../..");
  std::string cwdRoot = canonicalPath(".");

  if (!exeRoot.empty() && fileExists(exeRoot + "/config/db.json")) return exeRoot;
  if (!cwdRoot.empty() && fileExists(cwdRoot + "/config/db.json")) return cwdRoot;
  return !exeRoot.empty() ? exeRoot : (cwdRoot.empty() ? std::string(".") : cwdRoot);
}

// 判断路径是否以某后缀结尾（静态资源 MIME 与缓存策略共用）
static bool pathEndsWith(const std::string& p, const std::string& suffix)
{
  return p.size() >= suffix.size() &&
         p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static HttpResponse handleStatic(const HttpRequest& req) {
  std::string p = req.path;
  if (p == "/" || p.empty()) p = "/index.html";
  // 防目录穿越
  if (p.find("..") != std::string::npos) return jsonErr(400, "bad path");
  std::string full = g_root + "/frontend" + p;
  std::ifstream f(full, std::ios::binary);
  if (!f) {
    // 尝试 /pages/...
    full = g_root + "/frontend/pages" + p;
    f.clear(); f.open(full, std::ios::binary);
    if (!f) return jsonErr(404, "not found: " + p);
  }
  f.close();
  // 构建期已生成同名 .gz：客户端支持 gzip 时直接返回压缩体，避免运行时压缩
  bool useGzip = req.header("accept-encoding").find("gzip") != std::string::npos &&
                 fileExists(full + ".gz");
  std::ifstream src(useGzip ? full + ".gz" : full, std::ios::binary);
  std::ostringstream ss; ss << src.rdbuf();
  HttpResponse r;
  r.body = ss.str();
  if (pathEndsWith(full, ".html")) r.contentType = "text/html; charset=utf-8";
  else if (pathEndsWith(full, ".css")) r.contentType = "text/css; charset=utf-8";
  else if (pathEndsWith(full, ".js")) r.contentType = "application/javascript; charset=utf-8";
  else if (pathEndsWith(full, ".woff2")) r.contentType = "font/woff2";
  else if (pathEndsWith(full, ".woff")) r.contentType = "font/woff";
  else r.contentType = "application/octet-stream";
  r.extraHeaders["Vary"] = "Accept-Encoding";
  if (useGzip) r.extraHeaders["Content-Encoding"] = "gzip";
  // HTML 与 CSS/JS 每次校验（避免发版后看到旧页面/旧样式）；字体等不常变的资源仍长缓存
  if (pathEndsWith(full, ".html")) r.extraHeaders["Cache-Control"] = "no-cache";
  else if (pathEndsWith(full, ".css") || pathEndsWith(full, ".js"))
    r.extraHeaders["Cache-Control"] = "no-cache";
  else if (pathEndsWith(full, ".woff2") || pathEndsWith(full, ".woff"))
    r.extraHeaders["Cache-Control"] = "public, max-age=2592000";
  else r.extraHeaders["Cache-Control"] = "public, max-age=3600";
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

// =====================================================================
// v1.5.0-T003 WORDLISTS：词表库接口 + AI 生成词表 runner
// =====================================================================

static std::string wlTrim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace((unsigned char)s[b])) b++;
  while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
  return s.substr(b, e - b);
}

static bool wlParseBody(const HttpRequest& req, JsonValue& v) {
  if (req.body.empty()) { v = JsonValue::makeObject({}); return true; }
  try { v = parseJson(req.body); return true; }
  catch (...) { return false; }
}

static bool wlParseId(const HttpRequest& req, int64_t& id) {
  try { id = std::stoll(req.pathParam("id")); return id > 0; }
  catch (...) { return false; }
}

static HttpResponse handleWlList(const HttpRequest& req) {
  std::string source = req.queryParam("source");
  return jsonOk(g_wlSvc->list(source).dump());
}

static HttpResponse handleWlDetail(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  auto v = g_wlSvc->detail(id);
  if (v.isNull()) return jsonErr(404, "not found");
  return jsonOk(v.dump());
}

static HttpResponse handleWlItems(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  int page = 1, size = 20;
  try { if (!req.queryParam("page").empty()) page = std::stoi(req.queryParam("page")); } catch (...) {}
  try { if (!req.queryParam("size").empty()) size = std::stoi(req.queryParam("size")); } catch (...) {}
  auto v = g_wlSvc->items(id, page, size, req.queryParam("q"));
  if (v.isNull()) return jsonErr(404, "not found");
  return jsonOk(v.dump());
}

static HttpResponse handleWlSearch(const HttpRequest& req) {
  int limit = 20;
  try { if (!req.queryParam("limit").empty()) limit = std::stoi(req.queryParam("limit")); } catch (...) {}
  return jsonOk(g_wlSvc->search(req.queryParam("q"), limit).dump());
}

static HttpResponse handleWlMyWords(const HttpRequest& req) {
  int page = 1, size = 20;
  try { if (!req.queryParam("page").empty()) page = std::stoi(req.queryParam("page")); } catch (...) {}
  try { if (!req.queryParam("size").empty()) size = std::stoi(req.queryParam("size")); } catch (...) {}
  return jsonOk(g_wlSvc->myWords(req.queryParam("sort"), req.queryParam("order"),
                                 req.queryParam("q"), page, size).dump());
}

static HttpResponse handleWlCreate(const HttpRequest& req) {
  JsonValue v;
  if (!wlParseBody(req, v)) return jsonErr(400, "invalid json");
  auto r = g_wlSvc->create(v.getString("title", ""), v.getString("description", ""), v);
  if (!r.ok) return jsonErr(400, r.error);
  JsonObject o;
  o["id"] = JsonValue::makeInt((int)r.id);
  o["item_count"] = JsonValue::makeInt(r.added);
  o["skipped"] = JsonValue::makeInt(r.skipped);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWlUpdate(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  JsonValue v;
  if (!wlParseBody(req, v)) return jsonErr(400, "invalid json");
  std::string err;
  if (!g_wlSvc->update(id, v, err)) return jsonErr(err == "not found" ? 404 : 400, err);
  return jsonOk("{\"ok\":true}");
}

static HttpResponse handleWlDelete(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  std::string err;
  if (!g_wlSvc->remove(id, err)) return jsonErr(err == "not found" ? 404 : 500, err);
  return jsonOk("{\"ok\":true}");
}

static HttpResponse handleWlAddItems(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  JsonValue v;
  if (!wlParseBody(req, v)) return jsonErr(400, "invalid json");
  auto r = g_wlSvc->addItems(id, v);
  if (r.error == "not found") return jsonErr(404, r.error);
  if (r.error == "no valid entries" || r.error == "too many words (max 2000)")
    return jsonErr(400, r.error);
  if (!r.error.empty()) return jsonErr(500, r.error);
  JsonObject o;
  o["added"] = JsonValue::makeInt(r.added);
  o["skipped"] = JsonValue::makeInt(r.skipped);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWlRemoveItem(const HttpRequest& req) {
  int64_t itemId = 0;
  try { itemId = std::stoll(req.pathParam("itemId")); }
  catch (...) { return jsonErr(400, "invalid item id"); }
  std::string err;
  if (!g_wlSvc->removeItem(itemId, err)) return jsonErr(404, err);
  return jsonOk("{\"ok\":true}");
}

static HttpResponse handleWlCopy(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  auto r = g_wlSvc->copy(id);
  if (!r.ok) return jsonErr(r.error == "not found" ? 404 : 400, r.error);
  JsonObject o;
  o["id"] = JsonValue::makeInt((int)r.id);
  o["item_count"] = JsonValue::makeInt(r.added);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWlChildren(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  auto v = g_wlSvc->children(id);
  if (v.isNull()) return jsonErr(404, "not found");
  JsonObject o;
  o["data"] = v;
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWlAddChildren(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  JsonValue v;
  if (!wlParseBody(req, v)) return jsonErr(400, "invalid json");
  auto r = g_wlSvc->addChildren(id, v);
  if (r.error == "not found") return jsonErr(404, r.error);
  if (!r.error.empty()) return jsonErr(400, r.error);
  JsonObject o;
  o["added"] = JsonValue::makeInt(r.added);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWlImportWhole(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  JsonValue v;
  if (!wlParseBody(req, v)) return jsonErr(400, "invalid json");
  int64_t target = v.getInt("target_id", 0);
  if (target <= 0) return jsonErr(400, "target_id required");
  auto r = g_wlSvc->importWhole(id, target);
  if (r.error == "目标单词本不存在") return jsonErr(404, r.error);
  if (!r.error.empty()) return jsonErr(400, r.error);
  JsonObject o;
  o["added"] = JsonValue::makeInt(r.added);
  o["skipped"] = JsonValue::makeInt(r.skipped);
  // 共用词表自动转为私有副本时回传新本信息
  o["copied_id"] = JsonValue::makeInt(r.copied_id);
  o["copied_title"] = JsonValue::makeString(r.copied_title);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWlRemoveChild(const HttpRequest& req) {
  int64_t memberId = 0;
  try { memberId = std::stoll(req.pathParam("childId")); }
  catch (...) { return jsonErr(400, "invalid id"); }
  std::string err;
  if (!g_wlSvc->removeChild(memberId, err)) return jsonErr(err == "not found" ? 404 : 500, err);
  return jsonOk("{\"ok\":true}");
}

static HttpResponse handleWlChildOrder(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  JsonValue v;
  if (!wlParseBody(req, v)) return jsonErr(400, "invalid json");
  std::string err;
  if (!g_wlSvc->reorderChildren(id, v, err)) return jsonErr(err == "not found" ? 404 : 400, err);
  return jsonOk("{\"ok\":true}");
}

static HttpResponse handleWlCompose(const HttpRequest& req) {
  JsonValue v;
  if (!wlParseBody(req, v)) return jsonErr(400, "invalid json");
  auto r = g_wlSvc->compose(v);
  if (!r.ok) return jsonErr(400, r.error);
  JsonObject o;
  o["id"] = JsonValue::makeInt((int)r.id);
  o["item_count"] = JsonValue::makeInt(r.added);
  o["kind"] = JsonValue::makeString("combo");
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWlExpand(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  JsonValue v = g_wlSvc->expandJson(id);
  auto* e = v.find("error");
  if (e && e->isString()) {
    return jsonErr(e->asString() == "not found" ? 404 : 400, e->asString());
  }
  return jsonOk(v.dump());
}

static HttpResponse handleWlStartStudy(const HttpRequest& req) {
  int64_t id = 0;
  if (!wlParseId(req, id)) return jsonErr(400, "invalid id");
  auto ex = g_wlSvc->expand(id);
  if (!ex.ok) return jsonErr(ex.error == "not found" ? 404 : 400, ex.error);
  std::vector<KnowledgeService::BulkItem> items;
  for (auto& w : ex.words) {
    KnowledgeService::BulkItem it;
    it.word = w.word;
    it.meaning = w.meaning;
    it.pos = w.pos;
    it.item_id = w.item_id;
    items.push_back(it);
  }
  auto r = g_kpSvc->bulk(items, true, id, true);
  if (!r.ok) return jsonErr(500, r.error);
  JsonObject o;
  o["words"] = JsonValue::makeInt((int)ex.words.size());
  o["kp_created"] = JsonValue::makeInt(r.created);
  o["kp_skipped"] = JsonValue::makeInt(r.skipped);
  o["learning_created"] = JsonValue::makeInt(r.learning_created);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWlRangePackage(const HttpRequest& req) {
  JsonValue v;
  if (!wlParseBody(req, v)) return jsonErr(400, "invalid json");
  auto r = g_wlSvc->rangePackage(v);
  if (!r.ok) return jsonErr(400, r.error);
  JsonObject o;
  o["id"] = JsonValue::makeInt((int)r.id);
  o["item_count"] = JsonValue::makeInt(r.added);
  o["existing"] = JsonValue::makeBool(r.existing);
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWlImportMyWords(const HttpRequest& req) {
  JsonValue body;
  if (!wlParseBody(req, body)) return jsonErr(400, "invalid json");

  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");

  auto ids = g_wlSvc->myWordIds();
  if (ids.empty()) return jsonErr(400, "暂无可导入的单词");

  int64_t wordbookId = body.getInt("wordbook_id", 0);
  std::string name = wlTrim(body.getString("name", ""));
  bool created = false;

  if (wordbookId > 0) {
    auto rows = g_db.query("SELECT id,name FROM wordbook WHERE id=" + std::to_string(wordbookId)
                           + " AND user_id=" + std::to_string(uid));
    if (rows.empty()) return jsonErr(404, "wordbook not found");
    name = rows[0]["name"];
  } else {
    if (name.empty()) name = "我的单词合集";
    auto cr = g_wbSvc->create(name, "从我的单词一键导入");
    if (!cr.ok) return jsonErr(400, cr.error);
    wordbookId = cr.id;
    created = true;
  }

  JsonArray arr;
  for (auto id : ids) arr.push_back(JsonValue::makeInt((int)id));
  JsonObject payload;
  payload["knowledge_ids"] = JsonValue::makeArray(arr);
  auto ar = g_wbSvc->addEntries(wordbookId, JsonValue::makeObject(payload));
  if (!ar.error.empty() && ar.added == 0) {
    if (created) { std::string ignored; g_wbSvc->remove(wordbookId, ignored); }
    return jsonErr(400, ar.error);
  }

  JsonObject o;
  o["wordbook_id"] = JsonValue::makeInt((int)wordbookId);
  o["name"] = JsonValue::makeString(name);
  o["added"] = JsonValue::makeInt(ar.added);
  o["skipped"] = JsonValue::makeInt(ar.skipped);
  o["total"] = JsonValue::makeInt((int)ids.size());
  return jsonOk(JsonValue::makeObject(o).dump());
}

static HttpResponse handleWlGenerate(const HttpRequest& req) {
  JsonValue body;
  if (!wlParseBody(req, body)) return jsonErr(400, "invalid json");

  int uid = RequestContext::currentUserId();
  if (uid <= 0) return jsonErr(401, "login required");

  std::string topic = wlTrim(body.getString("topic", ""));
  if (topic.empty()) return jsonErr(400, "topic required");
  int count = (int)body.getInt("count", 20);
  if (count < 5) count = 5;
  if (count > 50) count = 50;
  std::string level = body.getString("level", "normal");

  auto usedRows = g_db.query(
    "SELECT COUNT(*) AS c FROM ai_tasks WHERE user_id=" + std::to_string(uid)
    + " AND kind='wordlist_generate' AND create_time>=CURDATE()");
  int used = usedRows.empty() ? 0 : std::stoi(usedRows[0]["c"]);
  if (used >= g_wordlistDailyLimit)
    return jsonErr(429, "今日 AI 生成词表次数已用完（" + std::to_string(g_wordlistDailyLimit) + " 次/日）");

  JsonObject payload;
  payload["topic"] = JsonValue::makeString(topic);
  payload["count"] = JsonValue::makeInt(count);
  payload["level"] = JsonValue::makeString(level);

  std::vector<AiQueueService::NewItem> items;
  items.push_back({0, topic});
  auto cr = g_queueSvc->create(uid, "wordlist_generate", "wordlist", items,
                               JsonValue::makeObject(payload).dump());
  if (!cr.ok) return jsonErr(cr.httpStatus, cr.error);

  JsonObject o;
  o["task_id"] = JsonValue::makeInt((int)cr.taskId);
  o["status"] = JsonValue::makeString("queued");
  o["count"] = JsonValue::makeInt(count);
  return jsonRes(202, JsonValue::makeObject(o).dump());
}

static bool wlParseAiJson(const std::string& body, JsonValue& out) {
  std::string s = body;
  size_t fence = s.find("```");
  if (fence != std::string::npos) {
    size_t end = s.find("```", fence + 3);
    if (end != std::string::npos) s = s.substr(fence + 3, end - fence - 3);
  }
  size_t b = s.find('{'), e = s.rfind('}');
  if (b != std::string::npos && e > b) {
    try { out = parseJson(s.substr(b, e - b + 1)); return true; } catch (...) {}
  }
  b = s.find('['); e = s.rfind(']');
  if (b != std::string::npos && e > b) {
    try { out = parseJson(s.substr(b, e - b + 1)); return true; } catch (...) {}
  }
  return false;
}

static void wlMarkItem(int64_t itemId, bool ok, const std::string& error) {
  std::string sql = "UPDATE ai_task_items SET status='" + std::string(ok ? "done" : "failed")
                  + "', finished_at=NOW(), error='"
                  + Db::escape(ok ? "" : error) + "' WHERE id=" + std::to_string(itemId);
  g_db.exec(sql);
}

static bool runWordlistGenerate(int uid, int64_t taskId) {
  RequestContext::userId = uid;
  auto items = g_db.query(
    "SELECT id,input_text FROM ai_task_items WHERE task_id=" + std::to_string(taskId)
    + " AND status='pending' ORDER BY item_index LIMIT 1");
  if (items.empty()) return false;

  int64_t itemId = std::stoll(items[0]["id"]);
  std::string itemText = items[0]["input_text"];
  g_db.exec("UPDATE ai_task_items SET status='running', started_at=NOW() WHERE id=" + std::to_string(itemId));

  std::string payloadJson;
  auto taskRows = g_db.query("SELECT payload_json FROM ai_tasks WHERE id=" + std::to_string(taskId));
  if (!taskRows.empty()) payloadJson = taskRows[0]["payload_json"];

  JsonValue payload = JsonValue::makeObject({});
  if (!payloadJson.empty()) {
    try { payload = parseJson(payloadJson); } catch (...) {}
  }
  std::string topic = payload.getString("topic", itemText);
  if (topic.empty()) topic = itemText;
  int count = (int)payload.getInt("count", 20);
  std::string level = payload.getString("level", "normal");

  std::string userPrompt = prompts::buildTemplate(prompts::WORDLIST_GENERATE_USER, {
    {"{TOPIC}", topic},
    {"{COUNT}", std::to_string(count)},
    {"{LEVEL}", level},
    {"{LEARNER_IDENTITY}", prompts::getLearnerIdentity(g_db, uid)}
  });

  std::vector<std::map<std::string, std::string>> messages;
  messages.push_back({{"role", "system"}, {"content", prompts::WORDLIST_GENERATE_SYSTEM}});
  messages.push_back({{"role", "user"}, {"content", userPrompt}});
  auto resp = g_ai.postChat(messages, true, 4096, 0.7, "wordlist_generate");
  if (!resp.ok) {
    wlMarkItem(itemId, false, "AI request failed");
    return true;
  }

  std::string raw = resp.extractedJson.empty() ? resp.body : resp.extractedJson;
  JsonValue parsed;
  if (!wlParseAiJson(raw, parsed)) {
    wlMarkItem(itemId, false, "AI returned invalid JSON");
    return true;
  }

  std::string title;
  JsonArray words;
  if (parsed.isObject()) {
    title = wlTrim(parsed.getString("title", ""));
    auto* w = parsed.find("words");
    if (w && w->isArray()) words = w->asArray();
  } else if (parsed.isArray()) {
    words = parsed.asArray();
  }

  JsonArray clean;
  for (auto& el : words) {
    JsonObject item;
    std::string word, meaning, pos;
    if (el.isString()) {
      word = wlTrim(el.asString());
    } else if (el.isObject()) {
      word = wlTrim(el.getString("word", ""));
      meaning = wlTrim(el.getString("meaning", ""));
      pos = wlTrim(el.getString("pos", ""));
    } else {
      continue;
    }
    if (word.empty()) continue;
    item["word"] = JsonValue::makeString(word);
    item["meaning"] = JsonValue::makeString(meaning);
    item["pos"] = JsonValue::makeString(pos);
    clean.push_back(JsonValue::makeObject(item));
    if ((int)clean.size() >= 50) break;
  }

  if (clean.empty()) {
    wlMarkItem(itemId, false, "AI returned no words");
    return true;
  }

  if (title.empty()) title = "AI 词表 · " + topic;
  JsonObject payloadOut;
  payloadOut["words"] = JsonValue::makeArray(clean);
  auto cr = g_wlSvc->create(title, "AI 生成 · " + topic, JsonValue::makeObject(payloadOut));
  if (!cr.ok) {
    wlMarkItem(itemId, false, cr.error);
    return true;
  }

  wlMarkItem(itemId, true, "");
  g_db.exec("UPDATE ai_tasks SET result_ref='" + std::to_string(cr.id) + "' WHERE id=" + std::to_string(taskId));
  return true;
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

  std::string root = resolveRoot(argc > 0 ? argv[0] : "");
  int port = 8088;
  std::string host = "127.0.0.1";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--port" && i+1 < argc) port = std::stoi(argv[++i]);
    else if (a == "--host" && i+1 < argc) host = argv[++i];
    else if (a == "--root" && i+1 < argc) root = argv[++i];
  }
  g_root = root;
  std::cerr << "[server] root=" << g_root << std::endl;

  // 加载配置
  DbConfig dbCfg = Db::loadFromJsonFile(root + "/config/db.json");
  AiConfig aiCfg = AiClient::loadFromJsonFile(root + "/config/ai.json");
  g_wordlistDailyLimit = aiCfg.wordlist_daily_limit;
  if (const char* e = getenv("ENG_WORDLIST_DAILY_LIMIT")) {
    try { g_wordlistDailyLimit = std::max(1, std::stoi(e)); } catch (...) {}
  }
  // [TEST] 测试隔离：仅启动期读取环境变量覆盖，生产不设置即保持配置默认值
  auto envOr = [](const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : def;
  };
  dbCfg.database = envOr("ENG_DB_NAME", dbCfg.database);
  aiCfg.mock_mode = envOr("ENG_AI_MOCK_MODE", aiCfg.mock_mode);
  if (const char* v = std::getenv("ENG_AI_RETRY_TIMES"); v && *v) {
    try { aiCfg.retry_times = std::max(1, std::stoi(v)); } catch (...) {}
  }
  if (const char* v = std::getenv("ENG_AI_RETRY_INTERVAL"); v && *v) {
    try { aiCfg.retry_interval_sec = std::max(0, std::stoi(v)); } catch (...) {}
  }
  if (const char* v = std::getenv("ENG_AI_MAX_CONCURRENCY"); v && *v) {
    try { aiCfg.max_concurrency = std::max(1, std::min(16, std::stoi(v))); } catch (...) {}
  }
  // v1.5.0-T002：队列参数允许测试环境覆盖
  if (const char* v = std::getenv("ENG_AI_QUEUE_MAX"); v && *v) {
    try { aiCfg.queue.max_size = std::max(1, std::stoi(v)); } catch (...) {}
  }
  if (const char* v = std::getenv("ENG_AI_QUEUE_TASK_TIMEOUT"); v && *v) {
    try { aiCfg.queue.task_timeout_sec = std::max(1, std::stoi(v)); } catch (...) {}
  }
  if (const char* v = std::getenv("ENG_AI_QUEUE_RETRY_MAX"); v && *v) {
    try { aiCfg.queue.retry_max = std::max(0, std::stoi(v)); } catch (...) {}
  }
  if (const char* v = std::getenv("ENG_AI_FAIL_THRESHOLD"); v && *v) {
    try { aiCfg.fail_threshold = std::max(1, std::stoi(v)); } catch (...) {}
  }
  if (const char* v = std::getenv("ENG_AI_DEGRADED_COOLDOWN"); v && *v) {
    try { aiCfg.degraded_cooldown_sec = std::max(1, std::stoi(v)); } catch (...) {}
  }
  // v1.5.0-T002：测试环境注入主备模型数组（JSON），生产保持 ai.json 的 providers
  if (const char* v = std::getenv("ENG_AI_PROVIDERS_JSON"); v && *v) {
    try {
      auto arr = parseJson(v);
      if (arr.isArray()) {
        std::vector<AiProviderConfig> list;
        for (const auto& item : arr.asArray()) {
          if (!item.isObject()) continue;
          AiProviderConfig pc;
          pc.name = item.getString("name", "provider" + std::to_string(list.size() + 1));
          pc.base_url = item.getString("base_url", aiCfg.base_url);
          pc.api_key = item.getString("api_key", aiCfg.api_key);
          pc.chat_model = item.getString("chat_model", aiCfg.chat_model);
          pc.provider_type = item.getString("provider_type", aiCfg.provider_type);
          list.push_back(pc);
        }
        if (!list.empty()) aiCfg.providers = list;
      }
    } catch (...) {}
  }
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

  // v1.5.0-T011：AI 调用留痕写入 ai_call_log（表缺失或写库失败不影响 AI 调用结果）
  g_ai.setCallObserver([](const AiCallRecord& r) {
    try {
      std::string err = r.errorMsg.empty() ? "NULL" : "'" + Db::escape(r.errorMsg) + "'";
      g_db.exec(
        "INSERT INTO ai_call_log(user_id,feature,provider,model,prompt_tokens,completion_tokens,total_tokens,duration_ms,ok,error_msg) VALUES(" +
        std::to_string(r.userId) + ",'" + Db::escape(r.feature) + "','" + Db::escape(r.provider) + "','" +
        Db::escape(r.model) + "'," + std::to_string(r.promptTokens) + "," + std::to_string(r.completionTokens) + "," +
        std::to_string(r.totalTokens) + "," + std::to_string(r.durationMs) + "," +
        std::to_string(r.ok ? 1 : 0) + "," + err + ")");
    } catch (const std::exception& e) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true))
        std::cerr << "[ops] ai_call_log write failed: " << e.what() << std::endl;
    }
  });

  std::string wordTmp = envOr("ENG_WORD_TMP", root + "/word_tmp");
  ::mkdir(wordTmp.c_str(), 0755);
  KnowledgeService kpSvc(g_db, g_ai, wordTmp, root);
  g_kpSvc = &kpSvc;
  LearningService learnSvc(g_db);
  g_learnSvc = &learnSvc;
  QuestionService qSvc(g_db, g_ai, kpSvc, learnSvc, wordTmp, root);
  g_qSvc = &qSvc;
  std::string backupDir = envOr("ENG_BACKUP_DIR", root + "/backups");
  MaintenanceService maintSvc(g_db, backupDir, wordTmp);
  g_maintSvc = &maintSvc;
  OpsService opsSvc(g_db);
  g_opsSvc = &opsSvc;
  WordbookService wbSvc(g_db, kpSvc, wordTmp, root);
  g_wbSvc = &wbSvc;
  // v1.5.0-T003：内置词表 seed（幂等，文件缺失不阻断启动）
  WordlistService wlSvc(g_db);
  g_wlSvc = &wlSvc;
  std::string wlTsv = envOr("ENG_WORDLIST_TSV", root + "/backend/data/textbook_wordlists/pep2019.tsv");
  auto wlSeed = wlSvc.seedBuiltin(wlTsv);
  if (!wlSeed.error.empty()) {
    std::cerr << "[wordlist] seed skipped: " << wlSeed.error << std::endl;
  } else {
    std::cout << "[wordlist] builtin seed: " << wlSeed.lists << " lists / "
              << wlSeed.items << " items" << std::endl;
  }
  // v1.5.0-T009：导出中心（只读汇总）
  ExportService exportSvc(g_db, wlSvc, wordTmp, root);
  g_exportSvc = &exportSvc;
  AiTaskService aiTaskSvc(g_db);
  g_aiTaskSvc = &aiTaskSvc;
  AuthService authSvc(g_db);
  g_authSvc = &authSvc;
  authSvc.purgeExpired();
  TreasuryService treasurySvc(g_db, root + "/config/rewards.json", root + "/config/metrics.json");
  g_treasurySvc = &treasurySvc;
  GoalService goalSvc(g_db, root + "/config/goal_templates.json");
  g_goalSvc = &goalSvc;
  // v1.5.1-T005：动作词典（周报锚点与 AI 导航共用）
  ActionService actionSvc(root + "/config/action_rules.json");
  g_actionSvc = &actionSvc;
  AiNavService aiNavSvc(g_db, g_ai, goalSvc, actionSvc, root + "/config/ai_nav.json");
  g_aiNavSvc = &aiNavSvc;
  // v1.5.2-T001 DEV-INTAKE：开发者台账 + 导航沉淀注入
  DevIntakeService devSvc(g_db, root + "/config/dev_intake.json");
  g_devSvc = &devSvc;
  aiNavSvc.setDevIntake(&devSvc);
  ErrorService errorSvc(g_db, g_ai, learnSvc, treasurySvc, root + "/config/error_rules.json");
  g_errorSvc = &errorSvc;
  // v1.4.0-T012 异步批量固化；v1.5.0-T002 迁入通用 AI 队列
  // worker 数 = AI 并发上限；单个任务内部按 3 词分片串行，避免叠加超过并发预算
  auto awardSeals = [](int uid) {
    if (!g_treasurySvc) return;
    g_treasurySvc->checkAndAwardMasterySeals(uid);
    g_treasurySvc->checkAndAwardTitle(uid);
  };
  AiQueueService queueSvc(g_db, aiCfg.max_concurrency, aiCfg.queue);
  g_queueSvc = &queueSvc;
  // v1.5.1-T003：学情周报（AI 建议异步生成，缓存到 weekly_report_ai）
  WeeklyReportService weeklySvc(g_db, g_ai, queueSvc, actionSvc, root + "/config/report_rules.json");
  g_weeklySvc = &weeklySvc;
  queueSvc.registerRunner("weekly_report", [&](int uid, int64_t taskId) {
    return weeklySvc.runTask(uid, taskId);
  });
  SolidifyJobService jobSvc(g_db, kpSvc, queueSvc, awardSeals);
  g_jobSvc = &jobSvc;
  queueSvc.registerRunner("solidify_kp", [&](int uid, int64_t taskId) {
    return jobSvc.runTask(uid, taskId);
  });
  queueSvc.registerRunner("wordlist_generate", [](int uid, int64_t taskId) {
    return runWordlistGenerate(uid, taskId);
  });
  // v1.5.0-T006：AI 讲解 / 定制练习 runner（需在 start 前注册）
  ExplainService explainSvc(g_db, g_ai, queueSvc, root);
  g_explainSvc = &explainSvc;
  CustomExerciseService customExSvc(g_db, g_ai, queueSvc, root, wordTmp);
  g_customExSvc = &customExSvc;
  queueSvc.registerRunner("explain", [&](int uid, int64_t taskId) {
    return explainSvc.runTask(uid, taskId);
  });
  queueSvc.registerRunner("followup", [&](int uid, int64_t taskId) {
    return explainSvc.runFollowupTask(uid, taskId);
  });
  queueSvc.registerRunner("custom_exercise", [&](int uid, int64_t taskId) {
    return customExSvc.runTask(uid, taskId);
  });
  queueSvc.setCompleteHook([&](int uid, int64_t) { awardSeals(uid); });
  jobSvc.recoverOnStartup();
  queueSvc.start();
  std::cout << "[ai-queue] workers=" << aiCfg.max_concurrency
            << " max_size=" << aiCfg.queue.max_size
            << " task_timeout=" << aiCfg.queue.task_timeout_sec << "s" << std::endl;
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
  // v1.5.0-T001：写路由统一包裹登录校验；认证接口走 server.Post 直接放行
  auto postAuthed = [&](const std::string& path, Handler h) { server.Post(path, requireLogin(std::move(h))); };
  auto putAuthed  = [&](const std::string& path, Handler h) { server.Put(path, requireLogin(std::move(h))); };
  auto delAuthed  = [&](const std::string& path, Handler h) { server.Delete(path, requireLogin(std::move(h))); };

  server.Get("/api/health", handleHealth);
  postAuthed("/api/kp", handleKpCreate);
  server.Get("/api/kp", handleKpList);
  postAuthed("/api/kp/preview", handleKpPreview);
  postAuthed("/api/kp/bulk", handleKpBulk);
  postAuthed("/api/kp/check", handleKpCheck);
  postAuthed("/api/kp/:id/visibility", handleKpVisibility);
  server.Get("/api/kp/:id", handleKpDetail);
  postAuthed("/api/kp/solidify", handleKpSolidifyAll);
  postAuthed("/api/kp/solidify/async", handleKpSolidifyAsync);
  server.Get("/api/kp/solidify/jobs", handleKpSolidifyJobList);
  server.Get("/api/kp/solidify/jobs/:job_id", handleKpSolidifyJobGet);
  postAuthed("/api/kp/solidify/jobs/:job_id/resume", handleKpSolidifyJobResume);
  postAuthed("/api/kp/:id/solidify", handleKpSolidifyOne);
  postAuthed("/api/kp/:id/export-word", handleKpExportWord);
  postAuthed("/api/kp/export-batch-word", handleKpExportBatchWord);
  postAuthed("/api/kp/:src/merge/:dst", handleKpMerge);
  postAuthed("/api/kp/:id/split", handleKpSplit);
  putAuthed("/api/kp/:id/standard", handleKpUpdateStd);
  server.Get("/api/word/download", handleWordDownload);
  // 阶段三：学情 & 选题（具体路径必须在 :id 之前注册）
  server.Get("/api/learn/candidates", handleLearnCandidatePool);
  server.Get("/api/learn/stats", handleLearnStats);
  server.Get("/api/learn/review-calendar", handleLearnReviewCalendar);
  server.Get("/api/learn/studying", handleLearnStudying);
  server.Get("/api/learn/summary", handleLearnSummary);
  postAuthed("/api/learn/select", handleLearnSelect);
  server.Get("/api/learn/:id", handleLearnGet);
  putAuthed("/api/learn/:id/proficiency", handleLearnSetProficiency);
  putAuthed("/api/learn/:id/mode", handleLearnSetMode);
  postAuthed("/api/learn/:id/answer", handleLearnAnswer);
  postAuthed("/api/learn/:id/complete-review", handleLearnCompleteReview);
  // 阶段四：AI 出题 & 批改
  postAuthed("/api/exercise/generate", handleExerciseGenerate);
  postAuthed("/api/exercise/export-word", handleExerciseExport);
  postAuthed("/api/exercise/grade", handleGrade);
  postAuthed("/api/exercise/batch-grade", handleBatchGrade);
  // 阶段六：定时任务 & 维护
  postAuthed("/api/maint/backup", handleBackup);
  server.Get("/api/maint/backups", handleListBackups);
  postAuthed("/api/maint/reset-learning/:id", handleResetLearning);
  postAuthed("/api/maint/clear-all-learning", handleClearAllLearning);
  postAuthed("/api/maint/cleanup-temp-word", handleCleanupTempWord);
  postAuthed("/api/maint/cleanup-startup", handleCleanupStartup);
  server.Get("/api/ops/ai-cost", requireLogin(handleOpsAiCost));
  // v1.2.0-T003：单词本系统
  server.Get("/api/wordbook", handleWbList);
  postAuthed("/api/wordbook", handleWbCreate);
  server.Get("/api/wordbook/:id", handleWbDetail);
  putAuthed("/api/wordbook/:id", handleWbUpdate);
  delAuthed("/api/wordbook/:id", handleWbDelete);
  server.Get("/api/wordbook/:id/entries", handleWbEntries);
  postAuthed("/api/wordbook/:id/entries", handleWbAddEntries);
  delAuthed("/api/wordbook/entries/:eid", handleWbRemoveEntry);
  postAuthed("/api/wordbook/:id/export-json", handleWbExportJson);
  postAuthed("/api/wordbook/:id/export-word", handleWbExportWord);
  // v1.5.0-T009 D09/D10：词表库单词卡导出
  postAuthed("/api/wordlists/:id/export-word", handleWlExportWord);
  postAuthed("/api/wordbook/import", handleWbImport);
  // v1.5.0-T009：导出中心汇总 / 讲解导出（游客可调用）
  server.Get("/api/exports/summary", requireLogin(handleExportsSummary));
  server.Post("/api/exports/explanation/:id", handleExportExplanation);
  // v1.5.0-T009 D12：游客当次定制练习导出（仅最新一条 done）
  server.Post("/api/exports/exercise/:id", handleExportGuestExercise);

  // v1.5.0-T003 WORDLISTS：静态段先注册，避免被 /:id 抢占
  server.Get("/api/wordlists/search", handleWlSearch);
  server.Get("/api/wordlists/my-words", handleWlMyWords);
  server.Get("/api/wordlists", handleWlList);
  postAuthed("/api/wordlists", handleWlCreate);
  postAuthed("/api/wordlists/my-words/import", handleWlImportMyWords);
  postAuthed("/api/wordlists/range-package", handleWlRangePackage);
  postAuthed("/api/wordlists/generate", handleWlGenerate);
  postAuthed("/api/wordlists/compose", handleWlCompose);
  // v1.5.0-T005：组合成员与递归展开
  server.Get("/api/wordlists/:id/children", handleWlChildren);
  postAuthed("/api/wordlists/:id/children", handleWlAddChildren);
  postAuthed("/api/wordlists/:id/import-whole", handleWlImportWhole);
  putAuthed("/api/wordlists/:id/children/order", handleWlChildOrder);
  delAuthed("/api/wordlists/:id/children/:childId", handleWlRemoveChild);
  postAuthed("/api/wordlists/:id/expand", handleWlExpand);
  postAuthed("/api/wordlists/:id/start-study", handleWlStartStudy);
  server.Get("/api/wordlists/:id/items", handleWlItems);
  delAuthed("/api/wordlists/:id/items/:itemId", handleWlRemoveItem);
  server.Get("/api/wordlists/:id", handleWlDetail);
  putAuthed("/api/wordlists/:id", handleWlUpdate);
  delAuthed("/api/wordlists/:id", handleWlDelete);
  postAuthed("/api/wordlists/:id/items", handleWlAddItems);
  postAuthed("/api/wordlists/:id/copy", handleWlCopy);
  // v1.2.0-T002：AI 任务管理
  server.Get("/api/ai/tasks", handleAiTaskList);
  postAuthed("/api/ai/tasks/:id/retry", handleAiTaskRetry);
  postAuthed("/api/ai/tasks/retry-all", handleAiTaskRetryAll);
  delAuthed("/api/ai/tasks/:id", handleAiTaskDelete);
  // v1.5.0-T002：通用 AI 队列接口（读接口按当前用户隔离，未登录返回空）
  server.Get("/api/ai-tasks", handleAiTaskListNew);
  server.Get("/api/ai-tasks/:id", handleAiTaskGetNew);
  postAuthed("/api/ai-tasks/:id/retry", handleAiTaskRetryNew);
  // v1.5.0-T006：AI 讲解（游客可用）+ 定制练习 + 空闲队列加急/延时
  server.Post("/api/explain", handleExplainCreate);
  server.Get("/api/explain/history", handleExplainHistory);
  server.Get("/api/explain/:id", handleExplainGet);
  server.Post("/api/explain/:id/regenerate", handleExplainRegenerate);
  postAuthed("/api/explain/:id/save-kp", handleExplainSaveKp);
  postAuthed("/api/explain/:id/to-wordbook", handleExplainToWordbook);
  server.Post("/api/explain/:id/followup", handleExplainFollowupCreate);
  server.Get("/api/explain/:id/followup", handleExplainFollowupList);
  postAuthed("/api/explain/:id/followup/:fid/merge", handleExplainFollowupMerge);
  server.Post("/api/custom-exercise", handleCustomExerciseCreate);
  server.Get("/api/custom-exercise/history", handleCustomExerciseHistory);
  server.Get("/api/custom-exercise/:id", handleCustomExerciseGet);
  server.Post("/api/custom-exercise/:id/grade", handleCustomExerciseGrade);
  // v1.5.0-T009 D08：定制练习导出空白卷（登录用户历史任意可导出）
  postAuthed("/api/custom-exercise/:id/export-word", handleCustomExerciseExport);
  postAuthed("/api/ai-tasks/:id/expedite", handleAiTaskExpedite);
  postAuthed("/api/ai-tasks/:id/defer", handleAiTaskDefer);
  // v1.2.0-T004：多用户认证
  server.Post("/api/auth/register", handleAuthRegister);
  server.Post("/api/auth/login", handleAuthLogin);
  server.Post("/api/auth/logout", handleAuthLogout);
  server.Post("/api/auth/deregister", handleAuthDeregister);
  server.Get("/api/auth/me", handleAuthMe);
  putAuthed("/api/auth/profile", handleAuthProfile);
  server.Get("/api/learner-identities", handleLearnerIdentities);
  // v1.2.0-T006：播放复习
  server.Get("/api/playback/queue", handlePlaybackQueue);
  // v1.2.0-T007：每日小测
  postAuthed("/api/quiz/daily", handleQuizStart);
  postAuthed("/api/quiz/daily/submit", handleQuizSubmit);
  postAuthed("/api/quiz/solidify-item", handleQuizSolidifyItem);
  server.Get("/api/quiz/daily/history", handleQuizHistory);
  // v1.5.1-T002：错题本与重练（静态段先注册）
  server.Get("/api/wrongbook/summary", handleWrongbookSummary);
  server.Get("/api/wrongbook/words", handleWrongbookWords);
  server.Get("/api/wrongbook/words/:kid", handleWrongbookWordDetail);
  server.Get("/api/retry-items", handleRetryItems);
  postAuthed("/api/retry-items/:id/advance", handleRetryAdvance);
  postAuthed("/api/retry-items/:id/skip", handleRetrySkip);
  postAuthed("/api/retry-items/:id/complete", handleRetryComplete);
  postAuthed("/api/retry-items/:id/archive", handleRetryArchive);
  postAuthed("/api/retry-items/:id/restore", handleRetryRestore);
  server.Get("/api/error-attributions/:id", handleErrorAttributionGet);
  putAuthed("/api/error-attributions/:id", handleErrorAttributionUpdate);
  // v1.5.1-T003：学情周报
  server.Get("/api/weekly-reports", handleWeeklyReport);
  postAuthed("/api/weekly-reports/ai", handleWeeklyReportAi);
  // v1.5.1-T005：AI 导航（游客可问答；提案确认需登录）
  server.Post("/api/ai-nav/messages", handleAiNavSend);
  server.Get("/api/ai-nav/messages", handleAiNavHistory);
  delAuthed("/api/ai-nav/messages", handleAiNavClear);
  postAuthed("/api/ai-nav/proposals/:id/confirm", handleAiNavConfirm);
  postAuthed("/api/ai-nav/proposals/:id/discard", handleAiNavDiscard);
  // v1.5.2-T001：开发者台账（仅 admin，权限在各 handler 内 devGuard 判定）
  server.Get("/api/dev/summary", handleDevSummary);
  server.Get("/api/dev/backlog", handleDevBacklogList);
  server.Put("/api/dev/backlog/:id", handleDevBacklogUpdate);
  server.Get("/api/dev/bugs", handleDevBugList);
  server.Put("/api/dev/bugs/:id", handleDevBugUpdate);
  // v1.3.0-T005：点金圣殿
  server.Get("/api/treasury", handleTreasury);
  postAuthed("/api/treasury/spend", handleTreasurySpend);
  server.Get("/api/treasury/stats", handleTreasuryStats);
  postAuthed("/api/treasury/weekly-goal", handleTreasuryWeeklyGoal);
  // v1.5.1-T001：课规 + 本周任务卡
  postAuthed("/api/treasury/next-week-tasks", handleTreasuryNextWeekTasks);
  postAuthed("/api/treasury/claim-week", handleTreasuryClaimWeek);
  server.Get("/api/metrics", handleMetrics);
  postAuthed("/api/goals/parse", handleGoalParse);
  postAuthed("/api/goals", handleGoalCreate);
  server.Get("/api/goals", handleGoalList);
  putAuthed("/api/goals/:id", handleGoalUpdate);
  delAuthed("/api/goals/:id", handleGoalDelete);
  // v1.3.0 文章模块
  server.Get("/api/article/my", handleArticleMy);
  server.Get("/api/article", handleArticleList);
  postAuthed("/api/article", handleArticleCreate);
  server.Get("/api/article/:id", handleArticleDetail);
  putAuthed("/api/article/:id", handleArticleUpdate);
  delAuthed("/api/article/:id", handleArticleDelete);
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
  // 先停队列 worker，保证 runner 引用的 jobSvc 仍存活
  queueSvc.stop();
  return 0;
}
