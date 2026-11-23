// v1.5.0-T011 OPS-BASELINE：运维统计（AI tokens / 耗时 / 成败 + 金桂流水聚合）
// 只读查询，不做写入；数据来源 ai_call_log 与 user_coin_txn
#pragma once
#include <string>
#include <vector>
#include <cstdlib>
#include "db.h"
#include "json.h"

namespace eng {

class OpsService {
public:
  explicit OpsService(Db& db) : db_(db) {}

  // uid：当前登录用户；all=true 时统计全局（仅系统账号 user_id=1 允许）
  JsonValue aiCost(int uid, int days, bool all) {
    if (days < 1) days = 1;
    if (days > 90) days = 90;
    std::string userCond = all ? "1=1" : ("user_id=" + std::to_string(uid));
    std::string coinCond = all ? "1=1" : ("user_id=" + std::to_string(uid));
    std::string since = "DATE_SUB(CURDATE(), INTERVAL " + std::to_string(days - 1) + " DAY)";

    Rows days_rows = db_.query(
        "SELECT DATE(created_at) AS d, COUNT(*) AS calls,"
        " COALESCE(SUM(ok=1),0) AS ok_calls, COALESCE(SUM(ok=0),0) AS fail_calls,"
        " COALESCE(SUM(prompt_tokens),0) AS pt, COALESCE(SUM(completion_tokens),0) AS ct,"
        " COALESCE(SUM(total_tokens),0) AS tt, COALESCE(ROUND(AVG(duration_ms)),0) AS avg_ms"
        " FROM ai_call_log WHERE " + userCond + " AND created_at >= " + since +
        " GROUP BY DATE(created_at) ORDER BY d");

    Rows feat_rows = db_.query(
        "SELECT feature, COUNT(*) AS calls,"
        " COALESCE(SUM(ok=1),0) AS ok_calls, COALESCE(SUM(ok=0),0) AS fail_calls,"
        " COALESCE(SUM(total_tokens),0) AS tt"
        " FROM ai_call_log WHERE " + userCond + " AND created_at >= " + since +
        " GROUP BY feature ORDER BY calls DESC");

    Rows coin_rows = db_.query(
        "SELECT reason_code, COUNT(*) AS cnt, COALESCE(SUM(delta),0) AS sum_delta"
        " FROM user_coin_txn WHERE " + coinCond + " AND created_at >= " + since +
        " GROUP BY reason_code ORDER BY reason_code");

    int64_t totalCalls = 0, totalTokens = 0;
    JsonArray dayArr, featArr, coinArr;
    for (auto& r : days_rows) {
      int64_t tt = toI64(r.at("tt"));
      totalCalls += toI64(r.at("calls"));
      totalTokens += tt;
      JsonObject o;
      o["date"] = JsonValue::makeString(r.at("d"));
      o["calls"] = JsonValue::makeInt(toI64(r.at("calls")));
      o["ok_calls"] = JsonValue::makeInt(toI64(r.at("ok_calls")));
      o["fail_calls"] = JsonValue::makeInt(toI64(r.at("fail_calls")));
      o["prompt_tokens"] = JsonValue::makeInt(toI64(r.at("pt")));
      o["completion_tokens"] = JsonValue::makeInt(toI64(r.at("ct")));
      o["total_tokens"] = JsonValue::makeInt(tt);
      o["avg_duration_ms"] = JsonValue::makeInt(toI64(r.at("avg_ms")));
      dayArr.push_back(JsonValue::makeObject(o));
    }
    for (auto& r : feat_rows) {
      JsonObject o;
      o["feature"] = JsonValue::makeString(r.at("feature"));
      o["calls"] = JsonValue::makeInt(toI64(r.at("calls")));
      o["ok_calls"] = JsonValue::makeInt(toI64(r.at("ok_calls")));
      o["fail_calls"] = JsonValue::makeInt(toI64(r.at("fail_calls")));
      o["total_tokens"] = JsonValue::makeInt(toI64(r.at("tt")));
      featArr.push_back(JsonValue::makeObject(o));
    }
    int64_t coinNet = 0;
    for (auto& r : coin_rows) {
      int64_t delta = toI64(r.at("sum_delta"));
      coinNet += delta;
      JsonObject o;
      o["reason_code"] = JsonValue::makeString(r.at("reason_code"));
      o["count"] = JsonValue::makeInt(toI64(r.at("cnt")));
      o["delta"] = JsonValue::makeInt(delta);
      coinArr.push_back(JsonValue::makeObject(o));
    }

    JsonObject root;
    root["days"] = JsonValue::makeInt(days);
    root["scope"] = JsonValue::makeString(all ? "all" : "self");
    root["calls"] = JsonValue::makeInt(totalCalls);
    root["total_tokens"] = JsonValue::makeInt(totalTokens);
    // 无 usage 的响应一律记 0：全为 0 时提示调用方 tokens 不可用，避免误读为「零消耗」
    root["tokens_available"] = JsonValue::makeBool(totalTokens > 0);
    root["coin_net"] = JsonValue::makeInt(coinNet);
    root["by_day"] = JsonValue::makeArray(std::move(dayArr));
    root["by_feature"] = JsonValue::makeArray(std::move(featArr));
    root["coin_flows"] = JsonValue::makeArray(std::move(coinArr));
    return JsonValue::makeObject(root);
  }

private:
  static int64_t toI64(const std::string& s) {
    try { return std::stoll(s); } catch (...) { return 0; }
  }
  Db& db_;
};

} // namespace eng
