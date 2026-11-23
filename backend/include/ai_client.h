// AI 客户端：多模型兼容层
// 通过策略模式支持不同 AI 大模型（OpenAI 兼容 / Agnes / 后续可扩展）
// v1.5.0-T002：支持 providers 主备数组、单次失败即切换、连续失败降级 10 分钟
#pragma once
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <memory>
#include <atomic>
#include <chrono>
#include <functional>
#include <fstream>
#include <ctime>
#include <unistd.h>
#include "json.h"
#include "ai_provider.h"
#include "auth.h"

namespace eng {

// v1.5.0-T011：AI 调用留痕（只含调用特征与计数，不含 prompt / 响应正文）
struct AiCallRecord {
  int userId = 0;
  std::string feature = "other";
  std::string provider;
  std::string model;
  int promptTokens = 0;
  int completionTokens = 0;
  int totalTokens = 0;
  int durationMs = 0;
  bool ok = false;
  std::string errorMsg;
};

using AiCallObserver = std::function<void(const AiCallRecord&)>;

class AiClient {
  AiConfig cfg_;
  struct Entry {
    AiProviderConfig pc;
    std::unique_ptr<IAiProvider> provider;
    int failures = 0;              // 连续失败次数
    time_t degradedUntil = 0;      // 降级截止时间（秒）
  };
  std::vector<Entry> entries_;
  std::atomic<bool> degraded_{false};
  AiCallObserver callObserver_;    // v1.5.0-T011：调用留痕回调（可空）
public:
  // 注册调用留痕回调；回调异常一律吞掉，不影响 AI 调用结果
  void setCallObserver(AiCallObserver obs) { callObserver_ = std::move(obs); }
  void setConfig(const AiConfig& c) {
    cfg_ = c;
    rebuildProviders();
  }
  const AiConfig& config() const { return cfg_; }
  // 全局降级：主备全部处于冷却期（页面显示 AI 降级徽章）
  bool isDegraded() const { return degraded_; }
  AiProviderType getProviderType() const {
    if (mockEnabled(cfg_)) return AiProviderType::MOCK;
    if (!entries_.empty()) return parseProviderType(entries_[0].pc.provider_type);
    return parseProviderType(cfg_.provider_type);
  }
  static bool mockEnabled(const AiConfig& c) {
    return !c.mock_mode.empty() && c.mock_mode != "off";
  }

  // providers 状态摘要（/api/health 用；不暴露 Key）
  std::string providersJson() const {
    std::ostringstream o;
    o << "[";
    time_t now = time(nullptr);
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (i) o << ",";
      const auto& e = entries_[i];
      o << "{\"name\":\"" << JsonValue::escapeString(e.pc.name.empty() ? e.pc.chat_model : e.pc.name)
        << "\",\"model\":\"" << JsonValue::escapeString(e.pc.chat_model)
        << "\",\"degraded\":" << (e.degradedUntil > now ? "true" : "false")
        << ",\"failures\":" << e.failures << "}";
    }
    o << "]";
    return o.str();
  }

  // 从 ai.json 加载（兼容旧顶层字段；新 providers / queue 段可选）
  static AiConfig loadFromJsonFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open ai config: " + path);
    std::stringstream ss; ss << f.rdbuf();
    AiConfig c;
    JsonValue root;
    try { root = parseJson(ss.str()); } catch (...) { root = JsonValue::makeObject({}); }
    auto str = [&](const std::string& k, const std::string& def) {
      auto v = root.getString(k, def);
      return v.empty() ? def : v;
    };
    auto num = [&](const std::string& k, int def) {
      return root.find(k) ? (int)root.getInt(k, def) : def;
    };
    auto dbl = [&](const std::string& k, double def) {
      auto* v = root.find(k);
      return v ? v->asDouble() : def;
    };

    c.base_url = str("base_url", c.base_url);
    c.api_key = str("api_key", c.api_key);
    c.chat_model = str("chat_model", c.chat_model);
    c.provider_type = str("provider_type", c.provider_type);
    c.request_timeout_sec = num("request_timeout_sec", 60);
    c.max_tokens_default = num("max_tokens_default", 1024);
    c.temperature_default = dbl("temperature_default", 0.7);
    c.retry_times = num("retry_times", 2);
    c.retry_interval_sec = num("retry_interval_sec", 2);
    c.max_concurrency = num("max_concurrency", 2);
    if (c.max_concurrency < 1) c.max_concurrency = 2;
    if (c.max_concurrency > 16) c.max_concurrency = 16;
    c.fail_threshold = num("fail_threshold", 3);
    c.degraded_cooldown_sec = num("degraded_cooldown_sec", 600);
    c.wordlist_daily_limit = num("wordlist_daily_limit", 5);
    if (c.wordlist_daily_limit < 1) c.wordlist_daily_limit = 5;
    if (c.fail_threshold < 1) c.fail_threshold = 3;
    if (c.degraded_cooldown_sec < 1) c.degraded_cooldown_sec = 600;

    // providers 数组（可选）
    auto* pv = root.find("providers");
    if (pv && pv->isArray()) {
      for (const auto& item : pv->asArray()) {
        if (!item.isObject()) continue;
        AiProviderConfig pc;
        pc.name = item.getString("name", "");
        pc.base_url = item.getString("base_url", c.base_url);
        pc.api_key = item.getString("api_key", c.api_key);
        pc.chat_model = item.getString("chat_model", c.chat_model);
        pc.provider_type = item.getString("provider_type", c.provider_type);
        c.providers.push_back(pc);
      }
    }

    // queue 段（可选）
    if (auto* q = root.find("queue"); q && q->isObject()) {
      c.queue.max_size = (int)q->getInt("max_size", c.queue.max_size);
      c.queue.task_timeout_sec = (int)q->getInt("task_timeout_sec", c.queue.task_timeout_sec);
      c.queue.retry_max = (int)q->getInt("retry_max", c.queue.retry_max);
      c.queue.backoff_base_sec = (int)q->getInt("backoff_base_sec", c.queue.backoff_base_sec);
      c.queue.avg_cost_cold_start_sec = (int)q->getInt("avg_cost_cold_start_sec", c.queue.avg_cost_cold_start_sec);
      c.queue.idle_max_workers = (int)q->getInt("idle_max_workers", c.queue.idle_max_workers);
      c.queue.idle_max_level = (int)q->getInt("idle_max_level", c.queue.idle_max_level);
      c.queue.idle_hint_min_items = (int)q->getInt("idle_hint_min_items", c.queue.idle_hint_min_items);
      c.queue.idle_hint_min_eta_sec = (int)q->getInt("idle_hint_min_eta_sec", c.queue.idle_hint_min_eta_sec);
    }
    if (c.queue.max_size < 1) c.queue.max_size = 1024;
    if (c.queue.task_timeout_sec < 1) c.queue.task_timeout_sec = 180;
    if (c.queue.retry_max < 0) c.queue.retry_max = 2;
    if (c.queue.backoff_base_sec < 1) c.queue.backoff_base_sec = 2;
    if (c.queue.avg_cost_cold_start_sec < 1) c.queue.avg_cost_cold_start_sec = 30;
    if (c.queue.idle_max_workers < 0) c.queue.idle_max_workers = 1;
    if (c.queue.idle_max_level < 0) c.queue.idle_max_level = 0;

    c.mock_mode = str("mock_mode", "off");
    if (c.mock_mode.empty()) c.mock_mode = "off";
    // fixture 与 ai.json 同目录，避免额外配置项
    size_t slash = path.find_last_of('/');
    c.mock_fixture_path = (slash == std::string::npos ? "" : path.substr(0, slash + 1))
                          + "ai_mock.json";
    return c;
  }

  // ── 公开接口 ──

  // 发聊天请求：按主备顺序尝试；单次失败立即切下一个；
  // 单 provider 时保留旧的指数退避重试（retry_times）
  // v1.5.0-T011：feature 用于调用留痕区分场景；usage 在返回前统一解析
  AiResponse postChat(const std::vector<std::map<std::string,std::string>>& messages,
                      bool wantJson=false, int maxTokens=0, double temperature=-1,
                      const std::string& feature="other") {
    auto started = std::chrono::steady_clock::now();
    std::string usedProvider, usedModel;

    // 统一收尾：解析 usage、回调留痕（回调异常不影响返回值）
    auto finish = [&](AiResponse r, bool ok) {
      parseUsage(r);
      if (callObserver_) {
        AiCallRecord rec;
        rec.userId = RequestContext::currentUserId();
        rec.feature = feature;
        rec.provider = usedProvider;
        rec.model = usedModel;
        rec.promptTokens = r.promptTokens;
        rec.completionTokens = r.completionTokens;
        rec.totalTokens = r.totalTokens;
        rec.durationMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started).count();
        rec.ok = ok;
        if (!ok) {
          std::string msg = r.body.empty() ? "AI request failed" : r.body;
          if (msg.size() > 255) msg.resize(255);
          rec.errorMsg = msg;
        }
        try { callObserver_(rec); } catch (...) {}
      }
      return r;
    };

    AiRequest req;
    req.messages = messages;
    req.wantJson = wantJson;
    req.maxTokens = maxTokens;
    req.temperature = temperature;
    if (entries_.empty()) {
      return finish(AiResponse{.status=0, .body="no provider initialized", .ok=false}, false);
    }

    time_t now = time(nullptr);
    AiResponse last{0, "all providers degraded", "", false};
    int attemptsPerProvider = entries_.size() == 1 ? std::max(1, cfg_.retry_times) : 1;

    for (auto& e : entries_) {
      if (e.degradedUntil > now) continue;
      usedProvider = e.pc.provider_type;
      usedModel = e.pc.chat_model;
      for (int a = 0; a < attemptsPerProvider; ++a) {
        try {
          last = e.provider ? e.provider->postChat(req)
                            : AiResponse{0, "provider missing", "", false};
          if (last.ok) {
            e.failures = 0;
            degraded_ = false;
            return finish(last, true);
          }
        } catch (const std::exception&) {
          last = AiResponse{0, "AI request failed", "", false};
        }
        if (a + 1 < attemptsPerProvider) {
          int wait = std::max(1, cfg_.retry_interval_sec) * (1 << a);
          if (wait > 8) wait = 8;
          sleep(wait);
        }
      }
      e.failures++;
      if (e.failures >= cfg_.fail_threshold)
        e.degradedUntil = now + cfg_.degraded_cooldown_sec;
    }

    // 走到这里说明没有任何 provider 成功（全部失败或全部处于冷却期）
    degraded_ = true;
    return finish(last, false);
  }

  // 从响应体解析 OpenAI 兼容 usage；无该字段时保持 0
  static void parseUsage(AiResponse& r) {
    if (r.body.empty()) return;
    try {
      JsonValue v = parseJson(r.body);
      if (!v.isObject()) return;
      auto* u = v.find("usage");
      if (!u || !u->isObject()) return;
      r.promptTokens = (int)u->getInt("prompt_tokens", 0);
      r.completionTokens = (int)u->getInt("completion_tokens", 0);
      r.totalTokens = (int)u->getInt("total_tokens", 0);
      if (r.totalTokens == 0) r.totalTokens = r.promptTokens + r.completionTokens;
    } catch (...) {
      // 非 JSON 响应（如网关错误页）不解析，保持 0
    }
  }

  // ping 健康检查（全部 provider 均不可用时返回 false）
  bool ping() {
    bool anyOk = false;
    for (auto& e : entries_) {
      if (e.provider && e.provider->ping()) { anyOk = true; break; }
    }
    return anyOk;
  }

private:
  void rebuildProviders() {
    entries_.clear();
    std::vector<AiProviderConfig> list = cfg_.providers;
    if (list.empty()) {
      AiProviderConfig def;
      def.name = "primary";
      def.base_url = cfg_.base_url;
      def.api_key = cfg_.api_key;
      def.chat_model = cfg_.chat_model;
      def.provider_type = cfg_.provider_type;
      list.push_back(def);
    }
    entries_.reserve(list.size());
    for (auto& pc : list) {
      AiConfig per = cfg_;
      per.base_url = pc.base_url;
      per.api_key = pc.api_key;
      per.chat_model = pc.chat_model;
      per.provider_type = pc.provider_type;
      AiProviderType type = mockEnabled(cfg_) ? AiProviderType::MOCK
                                              : parseProviderType(pc.provider_type);
      Entry e;
      e.pc = pc;
      e.provider = createAiProvider(type);
      if (e.provider) e.provider->setConfig(per);
      entries_.push_back(std::move(e));
    }
    degraded_ = false;
  }
};

} // namespace eng
