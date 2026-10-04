// 最小 HTTP 服务器（纯 C++ socket，无外部依赖）
// 支持：路由注册、JSON body 解析、CORS、POST/GET、超时
#pragma once
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <string>
#include <map>
#include <functional>
#include <vector>
#include <thread>
#include <sstream>
#include <memory>
#include <regex>
#include <iostream>
#include "json.h"

namespace eng {

struct HttpRequest {
  std::string method;
  std::string path;          // 不含 query
  std::string query;         // query string (不含 ?)
  std::map<std::string,std::string> headers;  // 小写 key
  std::string body;
  std::map<std::string,std::string> queryParams;
  std::map<std::string,std::string> pathParams; // 路由参数

  std::string header(const std::string& k) const {
    auto it = headers.find(k); return it==headers.end() ? "" : it->second;
  }
  std::string queryParam(const std::string& k) const {
    auto it = queryParams.find(k); return it==queryParams.end() ? "" : it->second;
  }
  std::string pathParam(const std::string& k) const {
    auto it = pathParams.find(k); return it==pathParams.end() ? "" : it->second;
  }
};

struct HttpResponse {
  int status=200;
  std::string contentType="application/json; charset=utf-8";
  std::string body;
  std::map<std::string,std::string> extraHeaders;
};

using Handler = std::function<HttpResponse(const HttpRequest&)>;
using BeforeDispatch = std::function<void(const HttpRequest&)>;

struct Route {
  std::string method;
  std::string pattern;   // 如 "/api/kp/:id"
  Handler handler;
};

class HttpServer {
  int port_;
  std::string host_;
  int serverFd_=-1;
  std::vector<Route> routes_;
  BeforeDispatch beforeDispatch_;
public:
  HttpServer(const std::string& host, int port) : port_(port), host_(host) {}

  // 前置回调：每个请求在路由 handler 执行前调用（用于身份上下文注入）
  void setBeforeDispatch(BeforeDispatch cb) { beforeDispatch_ = std::move(cb); }

  void route(const std::string& method, const std::string& pattern, Handler h) {
    routes_.push_back({method, pattern, std::move(h)});
  }

  // GET / POST / PUT / DELETE 便捷
  void Get(const std::string& p, Handler h)    { route("GET", p, std::move(h)); }
  void Post(const std::string& p, Handler h)   { route("POST", p, std::move(h)); }
  void Put(const std::string& p, Handler h)    { route("PUT", p, std::move(h)); }
  void Delete(const std::string& p, Handler h) { route("DELETE", p, std::move(h)); }

  void start() {
    serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd_ < 0) throw std::runtime_error("socket failed");
    int yes = 1;
    setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    ::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);
    if (::bind(serverFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      throw std::runtime_error(std::string("bind failed on ") + host_ + ":" + std::to_string(port_));
    }
    if (::listen(serverFd_, 16) < 0) throw std::runtime_error("listen failed");
    std::cerr << "[server] listening on http://" << host_ << ":" << port_ << std::endl;
    while (true) {
      struct sockaddr_in client;
      socklen_t cl = sizeof(client);
      int cfd = ::accept(serverFd_, (struct sockaddr*)&client, &cl);
      if (cfd < 0) continue;
      std::thread([this, cfd]{ handleConn(cfd); }).detach();
    }
  }

private:
  void handleConn(int cfd) {
    std::string buf;
    buf.reserve(8192);
    char tmp[4096];
    while (true) {
      ssize_t n = ::recv(cfd, tmp, sizeof(tmp), 0);
      if (n <= 0) { ::close(cfd); return; }
      buf.append(tmp, n);
      if (buf.find("\r\n\r\n") != std::string::npos) {
        // 头部结束。判断 Content-Length
        auto hEnd = buf.find("\r\n\r\n");
        std::string head = buf.substr(0, hEnd);
        // 提取 Content-Length
        std::regex cl("Content-Length:\\s*(\\d+)", std::regex::icase);
        std::smatch m;
        size_t need = 0;
        if (std::regex_search(head, m, cl)) need = std::stoul(m[1]);
        std::string body = buf.substr(hEnd + 4);
        if (body.size() >= need) break;
      }
    }
    // 解析
    HttpRequest req;
    auto hEnd = buf.find("\r\n\r\n");
    std::string head = buf.substr(0, hEnd);
    std::string body = buf.substr(hEnd + 4);
    // 请求行
    std::istringstream is(head);
    std::string line;
    if (std::getline(is, line)) {
      line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
      std::istringstream rl(line);
      rl >> req.method;
      std::string fullPath; rl >> fullPath;
      auto qpos = fullPath.find('?');
      if (qpos != std::string::npos) {
        req.path = fullPath.substr(0, qpos);
        req.query = fullPath.substr(qpos+1);
        // 解析 query
        std::string q = req.query;
        size_t pos = 0;
        while (pos < q.size()) {
          size_t amp = q.find('&', pos);
          std::string pair = (amp==std::string::npos) ? q.substr(pos) : q.substr(pos, amp-pos);
          size_t eq = pair.find('=');
          if (eq != std::string::npos) {
            req.queryParams[urlDecode(pair.substr(0,eq))] = urlDecode(pair.substr(eq+1));
          }
          if (amp == std::string::npos) break;
          pos = amp + 1;
        }
      } else req.path = fullPath;
    }
    while (std::getline(is, line)) {
      line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
      auto c = line.find(':');
      if (c != std::string::npos) {
        std::string k = line.substr(0, c);
        std::string v = line.substr(c+1);
        // trim
        size_t s = v.find_first_not_of(" \t");
        if (s != std::string::npos) v = v.substr(s);
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        req.headers[k] = v;
      }
    }
    req.body = body;

    // 路由匹配
    HttpResponse res;
    bool matched = false;
    for (const auto& r : routes_) {
      if (r.method != req.method) continue;
      std::smatch m;
      // 把 pattern /:var/ 转成正则
      std::string re = "^";
      std::string pat = r.pattern;
      // 简化：逐字符扫描
      size_t i = 0;
      while (i < pat.size()) {
        if (pat[i] == ':') {
          re += "([^/]+)";
          i++;
          while (i < pat.size() && pat[i] != '/') i++;
        } else if (pat[i] == '*') {
          re += ".*";
          i++;
        } else if (pat[i] == '.') {
          re += "\\.";
          i++;
        } else {
          re += pat[i];
          i++;
        }
      }
      re += "$";
      std::regex reObj(re);
      if (std::regex_match(req.path, m, reObj)) {
        // 提取路径参数（按 pattern 顺序）
        size_t vi = 1;
        size_t pos = 0;
        while (pos < pat.size()) {
          if (pat[pos] == ':') {
            pos++;
            std::string name;
            while (pos < pat.size() && pat[pos] != '/') name += pat[pos++];
            if (vi < m.size()) req.pathParams[name] = m[vi++];
          } else pos++;
        }
        try {
          if (beforeDispatch_) beforeDispatch_(req);
          res = r.handler(req);
        } catch (const std::exception& e) {
          res.status = 500;
          res.body = std::string("{\"error\":\"") + e.what() + "\"}";
        }
        matched = true;
        break;
      }
    }
    if (!matched) {
      res.status = 404;
      res.body = "{\"error\":\"not found\"}";
    }

    // 发送响应
    std::ostringstream resp;
    resp << "HTTP/1.1 " << res.status << " " << statusText(res.status) << "\r\n";
    resp << "Content-Type: " << res.contentType << "\r\n";
    resp << "Content-Length: " << res.body.size() << "\r\n";
    resp << "Connection: close\r\n";
    resp << "Access-Control-Allow-Origin: *\r\n";
    resp << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
    resp << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    for (auto& kv : res.extraHeaders) resp << kv.first << ": " << kv.second << "\r\n";
    resp << "\r\n" << res.body;
    std::string out = resp.str();
    ::send(cfd, out.data(), out.size(), 0);
    ::close(cfd);
  }

  static std::string statusText(int s) {
    switch (s) {
      case 200: return "OK";
      case 201: return "Created";
      case 204: return "No Content";
      case 400: return "Bad Request";
      case 404: return "Not Found";
      case 409: return "Conflict";
      case 500: return "Internal Server Error";
      case 503: return "Service Unavailable";
      default: return "OK";
    }
  }

  static std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] == '+') out += ' ';
      else if (s[i] == '%' && i+2 < s.size()) {
        int hi = hexVal(s[i+1]);
        int lo = hexVal(s[i+2]);
        if (hi>=0 && lo>=0) { out += (char)((hi<<4)|lo); i+=2; }
        else out += s[i];
      } else out += s[i];
    }
    return out;
  }
  static int hexVal(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
  }
};

} // namespace eng
