// AI HTTP 底层工具
// 被 ai_client.h 和 ai_provider.cpp 共用，避免循环依赖
#pragma once
#include <string>
#include <sstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <cstdio>

namespace eng::ai_http {

struct Url { std::string host; int port=80; std::string prefix; };

inline Url parseUrl(const std::string& url) {
  Url u;
  auto p = url.find("://");
  std::string rest = (p == std::string::npos) ? url : url.substr(p+3);
  auto slash = rest.find('/');
  std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  u.prefix = (slash == std::string::npos) ? "" : rest.substr(slash);
  auto colon = hostport.find(':');
  if (colon != std::string::npos) {
    u.host = hostport.substr(0, colon);
    try { u.port = std::stoi(hostport.substr(colon+1)); } catch(...) { u.port = 80; }
  } else { u.host = hostport; u.port = 80; }
  return u;
}

// 拼接 API 路径：base_url 前缀已含 /v1 时不重复添加
// 例：prefix="/v1" 或 "" → "/v1/chat/completions"；prefix="/v2" → "/v2/v1/chat/completions"
inline std::string apiPath(const std::string& prefix, const std::string& suffix) {
  std::string base = prefix;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (base.find("/v1") == std::string::npos) base += "/v1";
  if (suffix.empty() || suffix[0] != '/') base += "/";
  return base + suffix;
}

inline std::string jsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else out += c;
    }
  }
  return out;
}

inline std::string dechunk(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t eol = s.find("\r\n", i);
    if (eol == std::string::npos) break;
    std::string hex = s.substr(i, eol-i);
    try {
      size_t len = std::stoul(hex, nullptr, 16);
      if (len == 0) break;
      i = eol + 2;
      if (i + len > s.size()) break;
      out.append(s, i, len);
      i += len + 2;
    } catch (...) { break; }
  }
  return out;
}

} // namespace eng::ai_http
