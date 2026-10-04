// AI 客户端：多模型兼容层
// 通过策略模式支持不同 AI 大模型（OpenAI 兼容 / Agnes / 后续可扩展）
#pragma once
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <memory>
#include <fstream>
#include <unistd.h>
#include "json.h"
#include "ai_provider.h"

namespace eng {

class AiClient {
  AiConfig cfg_;
  std::unique_ptr<IAiProvider> provider_;
  bool degraded_ = false;
public:
  void setConfig(const AiConfig& c) {
    cfg_ = c;
    auto newType = parseProviderType(c.provider_type);
    if (!provider_ || provider_->getProviderType() != newType) {
      provider_ = createAiProvider(newType);
    }
    if (provider_) provider_->setConfig(c);
  }
  const AiConfig& config() const { return cfg_; }
  bool isDegraded() const { return degraded_; }
  AiProviderType getProviderType() const {
    return parseProviderType(cfg_.provider_type);
  }

  // 从 ai.json 加载
  static AiConfig loadFromJsonFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open ai config: " + path);
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    auto getStr = [&](const std::string& key) -> std::string {
      std::string pat = "\"" + key + "\"";
      auto p = s.find(pat);
      if (p == std::string::npos) return "";
      p = s.find(":", p);
      if (p == std::string::npos) return "";
      while (p < s.size() && (s[p]==':' || s[p]==' ' || s[p]=='\t')) p++;
      if (p >= s.size() || s[p] != '"') return "";
      p++;
      std::string out;
      while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p+1 < s.size()) { out += s[p+1]; p+=2; }
        else out += s[p++];
      }
      return out;
    };
    auto getInt = [&](const std::string& key, int def) -> int {
      std::string pat = "\"" + key + "\"";
      auto p = s.find(pat);
      if (p == std::string::npos) return def;
      p = s.find(":", p);
      if (p == std::string::npos) return def;
      p++;
      while (p < s.size() && (s[p]==' ' || s[p]=='\t')) p++;
      std::string num;
      while (p < s.size() && (s[p]=='-' || (s[p]>='0' && s[p]<='9'))) num += s[p++];
      try { return num.empty() ? def : std::stoi(num); } catch(...) { return def; }
    };
    auto getDouble = [&](const std::string& key, double def) -> double {
      std::string pat = "\"" + key + "\"";
      auto p = s.find(pat);
      if (p == std::string::npos) return def;
      p = s.find(":", p);
      if (p == std::string::npos) return def;
      p++;
      while (p < s.size() && (s[p]==' ' || s[p]=='\t')) p++;
      std::string num;
      while (p < s.size() && (s[p]=='-' || (s[p]>='0' && s[p]<='9') || s[p]=='.')) num += s[p++];
      try { return num.empty() ? def : std::stod(num); } catch(...) { return def; }
    };
    AiConfig c;
    c.base_url = getStr("base_url");
    c.api_key = getStr("api_key");
    c.chat_model = getStr("chat_model");
    c.provider_type = getStr("provider_type");
    c.request_timeout_sec = getInt("request_timeout_sec", 60);
    c.max_tokens_default = getInt("max_tokens_default", 1024);
    c.temperature_default = getDouble("temperature_default", 0.7);
    c.retry_times = getInt("retry_times", 2);
    c.retry_interval_sec = getInt("retry_interval_sec", 2);
    return c;
  }

  // ── 公开接口 ──

  // 发聊天请求（自动路由到对应 Provider）
  AiResponse postChat(const std::vector<std::map<std::string,std::string>>& messages,
                      bool wantJson=false, int maxTokens=0, double temperature=-1) {
    degraded_ = false;
    AiRequest req;
    req.messages = messages;
    req.wantJson = wantJson;
    req.maxTokens = maxTokens;
    req.temperature = temperature;

    if (!provider_) return AiResponse{.status=0, .ok=false, .body="no provider initialized"};

    // 指数退避重试
    int base = std::max(1, cfg_.retry_interval_sec);
    for (int attempt = 1; attempt <= cfg_.retry_times; ++attempt) {
      try {
        AiResponse r = provider_->postChat(req);
        if (r.ok) return r;
        degraded_ = false;
        if (attempt == cfg_.retry_times) return r;
      } catch (const std::exception&) {
        degraded_ = (attempt == cfg_.retry_times);
        if (attempt == cfg_.retry_times) {
          return AiResponse{.status=0, .ok=false, .body="AI request failed after retries"};
        }
      }
      int wait = base * (1 << (attempt - 1));
      if (wait > 8) wait = 8;
      sleep(wait);
    }
    return AiResponse{};
  }

  // ping 健康检查
  bool ping() {
    if (!provider_) return false;
    return provider_->ping();
  }
};

} // namespace eng
