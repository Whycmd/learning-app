// AI Provider 实现
#include "ai_provider.h"
#include "ai_http.h"
#include <stdexcept>
#include <algorithm>

namespace eng {

// ──────────────────────────────────────────────
// OpenAI 兼容 Provider
// ──────────────────────────────────────────────
class OpenAiCompatibleProvider : public IAiProvider {
  AiConfig cfg_;
  bool degraded_ = false;
public:
  void setConfig(const AiConfig& c) override { cfg_ = c; }
  bool isDegraded() const override { return degraded_; }
  AiProviderType getProviderType() const override { return AiProviderType::OPENAI_COMPATIBLE; }

  bool ping() override {
    auto u = ai_http::parseUrl(cfg_.base_url);
    // 健康检查固定在网关根路径 /readyz
    std::string path = "/readyz";
    struct hostent* he = gethostbyname(u.host.c_str());
    if (!he) return false;
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(u.port);
    std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { ::close(sock); return false; }
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\nHost: " << u.host << "\r\nConnection: close\r\n\r\n";
    std::string out = req.str();
    if (::send(sock, out.data(), out.size(), 0) < 0) { ::close(sock); return false; }
    char buf[1024]; std::string resp;
    while (true) {
      ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
      if (n <= 0) break;
      resp.append(buf, n);
    }
    ::close(sock);
    return resp.find("200 OK") != std::string::npos || resp.find("\"status\":\"ok\"") != std::string::npos;
  }

  AiResponse postChat(const AiRequest& req) override {
    auto u = ai_http::parseUrl(cfg_.base_url);
    std::string path = ai_http::apiPath(u.prefix, "/chat/completions");

    // 构造请求体
    std::ostringstream body;
    body << "{";
    body << "\"model\":\"" << cfg_.chat_model << "\",";
    body << "\"messages\":[";
    for (size_t i = 0; i < req.messages.size(); i++) {
      if (i) body << ",";
      body << "{\"role\":\"" << req.messages[i].at("role") << "\",";
      body << "\"content\":" << "\"" << ai_http::jsonEscape(req.messages[i].at("content")) << "\"}";
    }
    body << "],";
    body << "\"temperature\":" << (req.temperature < 0 ? cfg_.temperature_default : req.temperature) << ",";
    body << "\"max_tokens\":" << (req.maxTokens > 0 ? req.maxTokens : cfg_.max_tokens_default);
    if (req.wantJson) body << ",\"response_format\":{\"type\":\"json_object\"}";
    body << "}";

    return doRequest(cfg_, path, body.str());
  }

private:
  AiResponse doRequest(const AiConfig& cfg, const std::string& path, const std::string& body) {
    auto u = ai_http::parseUrl(cfg.base_url);
    struct hostent* he = gethostbyname(u.host.c_str());
    if (!he) throw std::runtime_error("DNS failed: " + u.host);
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) throw std::runtime_error("socket failed");
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(u.port);
    std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    struct timeval tv; tv.tv_sec = cfg.request_timeout_sec; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      ::close(sock);
      throw std::runtime_error("connect failed");
    }
    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n";
    req << "Host: " << u.host << "\r\n";
    req << "Content-Type: application/json\r\n";
    req << "Content-Length: " << body.size() << "\r\n";
    if (!cfg.api_key.empty()) req << "Authorization: Bearer " << cfg.api_key << "\r\n";
    req << "Connection: close\r\n\r\n" << body;
    std::string out = req.str();
    if (::send(sock, out.data(), out.size(), 0) < 0) {
      ::close(sock);
      throw std::runtime_error("send failed");
    }
    std::string resp;
    char buf[4096];
    while (true) {
      ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
      if (n <= 0) break;
      resp.append(buf, n);
    }
    ::close(sock);

    auto hEnd = resp.find("\r\n\r\n");
    if (hEnd == std::string::npos) throw std::runtime_error("bad HTTP response");
    std::string head = resp.substr(0, hEnd);
    std::string respBody = resp.substr(hEnd + 4);
    if (head.find("Transfer-Encoding: chunked") != std::string::npos ||
        head.find("transfer-encoding: chunked") != std::string::npos) {
      respBody = ai_http::dechunk(respBody);
    }
    std::istringstream is(head);
    std::string proto; int status = 0;
    is >> proto >> status;
    AiResponse r;
    r.status = status;
    r.body = respBody;
    r.ok = (status == 200);
    // 提取 _extracted_json 字段
    auto ejPos = respBody.find("\"_extracted_json\":");
    if (ejPos != std::string::npos) {
      size_t p = ejPos + std::string("\"_extracted_json\":").size();
      while (p < respBody.size() && (respBody[p]==' ' || respBody[p]=='\t')) p++;
      if (p < respBody.size() && respBody[p] == '"') {
        p++;
        std::string val;
        while (p < respBody.size()) {
          if (respBody[p] == '\\' && p+1 < respBody.size()) { val += respBody[p+1]; p+=2; }
          else if (respBody[p] == '"') break;
          else val += respBody[p++];
        }
        r.extractedJson = val;
      } else if (p < respBody.size() && respBody[p] == 'n' && respBody.compare(p,4,"null")==0) {
        r.extractedJson = "";
      }
    }
    if (r.extractedJson.empty()) {
      auto cPos = respBody.find("\"content\":\"");
      if (cPos != std::string::npos) {
        size_t p = cPos + std::string("\"content\":\"").size();
        std::string content;
        while (p < respBody.size()) {
          if (respBody[p] == '\\' && p+1 < respBody.size()) {
            char e = respBody[p+1];
            if (e == 'n') content += '\n';
            else if (e == 't') content += '\t';
            else if (e == 'r') content += '\r';
            else if (e == '"') content += '"';
            else if (e == '\\') content += '\\';
            else content += e;
            p += 2;
          } else if (respBody[p] == '"') break;
          else { content += respBody[p++]; }
        }
        auto start = content.find_first_of("[{");
        if (start != std::string::npos) {
          char open = content[start];
          char close = open == '{' ? '}' : ']';
          int depth = 0;
          for (size_t i = start; i < content.size(); i++) {
            if (content[i] == open) depth++;
            else if (content[i] == close) {
              depth--;
              if (depth == 0) {
                r.extractedJson = content.substr(start, i-start+1);
                break;
              }
            }
          }
        }
      }
    }
    return r;
  }
};

// ──────────────────────────────────────────────
// Agnes Provider（预留接口，待 API 规格确定后填充）
// ──────────────────────────────────────────────
class AgnesProvider : public IAiProvider {
  AiConfig cfg_;
  bool degraded_ = false;
public:
  void setConfig(const AiConfig& c) override { cfg_ = c; }
  bool isDegraded() const override { return degraded_; }
  AiProviderType getProviderType() const override { return AiProviderType::AGNES; }

  bool ping() override {
    // TODO: 实现 Agnes 特有的健康检查端点
    // 暂时沿用 OpenAI 兼容方式作为兜底
    OpenAiCompatibleProvider fallback;
    fallback.setConfig(cfg_);
    return fallback.ping();
  }

  AiResponse postChat(const AiRequest& req) override {
    // TODO: 实现 Agnes 特有的 API 格式
    // 当前降级到 OpenAI 兼容格式，待 API 文档确认后替换
    // 示例结构：
    //   POST {base_url}/agbes/v1/messages
    //   body: {"model": cfg_.chat_model, "input": [...], ...}
    OpenAiCompatibleProvider fallback;
    fallback.setConfig(cfg_);
    return fallback.postChat(req);
  }
};

// ──────────────────────────────────────────────
// 工厂实现
// ──────────────────────────────────────────────

AiProviderType parseProviderType(const std::string& name) {
  if (name == "openai_compatible" || name.empty())
    return AiProviderType::OPENAI_COMPATIBLE;
  if (name == "agnes")
    return AiProviderType::AGNES;
  return AiProviderType::OPENAI_COMPATIBLE; // 默认兼容
}

std::string providerTypeName(AiProviderType type) {
  switch (type) {
    case AiProviderType::OPENAI_COMPATIBLE: return "openai_compatible";
    case AiProviderType::AGNES:             return "agnes";
    default:                                return "unknown";
  }
}

std::unique_ptr<IAiProvider> createAiProvider(AiProviderType type) {
  switch (type) {
    case AiProviderType::OPENAI_COMPATIBLE:
      return std::make_unique<OpenAiCompatibleProvider>();
    case AiProviderType::AGNES:
      return std::make_unique<AgnesProvider>();
    default:
      return std::make_unique<OpenAiCompatibleProvider>();
  }
}

} // namespace eng
