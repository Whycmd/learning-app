// 最小 HTTP 服务器（纯 C++ socket，无外部依赖）
// 支持：路由注册（预编译）、JSON body 解析、CORS、POST/GET、超时、keep-alive
// v1.4.1-T002 阶段 A：有界线程池、连接超时、请求大小上限、解析加固、部分写重试
// v1.4.1-T002 阶段 B：keep-alive 连接复用、路由正则预编译
#pragma once
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <functional>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <memory>
#include <regex>
#include <iostream>
#include <algorithm>
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
  std::regex re;                     // 注册时预编译
  std::vector<std::string> paramNames;
};

class HttpServer {
  // 连接与请求保护参数（阶段 A/B 固定值，调整需改此处并回归 stage15）
  static constexpr int kDefaultWorkers     = 16;
  static constexpr size_t kMaxQueue        = 1024;
  static constexpr size_t kMaxHeaderBytes  = 16 * 1024;
  static constexpr size_t kMaxHeaderLine   = 8 * 1024;
  static constexpr int kMaxHeaders         = 100;
  static constexpr size_t kMaxBodyBytes    = 8 * 1024 * 1024;
  static constexpr size_t kMaxPathLen      = 2048;
  static constexpr int kReadTimeoutSec     = 10;
  static constexpr int kSendTimeoutSec     = 30;
  static constexpr int kKeepAliveIdleSec   = 5;
  static constexpr int kMaxRequestsPerConn = 100;

  int port_;
  std::string host_;
  int serverFd_=-1;
  std::vector<Route> routes_;
  BeforeDispatch beforeDispatch_;

  // 有界任务队列：fd 入队，固定 worker 消费
  std::mutex mu_;
  std::condition_variable cvWork_;
  std::condition_variable cvSpace_;
  std::deque<int> queue_;
public:
  HttpServer(const std::string& host, int port) : port_(port), host_(host) {}

  // 前置回调：每个请求在路由 handler 执行前调用（用于身份上下文注入）
  void setBeforeDispatch(BeforeDispatch cb) { beforeDispatch_ = std::move(cb); }

  void route(const std::string& method, const std::string& pattern, Handler h) {
    routes_.push_back(compileRoute(method, pattern, std::move(h)));
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

    int workers = kDefaultWorkers;
    const char* envW = std::getenv("HTTP_WORKERS");
    if (envW && *envW) {
      int v = std::atoi(envW);
      if (v >= 1 && v <= 64) workers = v;
    }
    std::cerr << "[server] listening on http://" << host_ << ":" << port_
              << " (workers=" << workers << ", keep-alive " << kKeepAliveIdleSec << "s)" << std::endl;
    for (int i = 0; i < workers; i++) {
      std::thread([this]{ workerLoop(); }).detach();
    }

    while (true) {
      struct sockaddr_in client;
      socklen_t cl = sizeof(client);
      int cfd = ::accept(serverFd_, (struct sockaddr*)&client, &cl);
      if (cfd < 0) continue;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cvSpace_.wait(lk, [this]{ return queue_.size() < kMaxQueue; });
        queue_.push_back(cfd);
      }
      cvWork_.notify_one();
    }
  }

private:
  // 固定 worker：取出连接 → 处理（可能连续多个请求）→ 关闭；异常在 handleConn 内兜底
  void workerLoop() {
    while (true) {
      int cfd = -1;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cvWork_.wait(lk, [this]{ return !queue_.empty(); });
        cfd = queue_.front();
        queue_.pop_front();
      }
      cvSpace_.notify_one();
      handleConn(cfd);
    }
  }

  void handleConn(int cfd) {
    struct timeval stv;
    stv.tv_sec = kSendTimeoutSec; stv.tv_usec = 0;
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));

    std::string pending;   // 尚未消费的连接字节（含流水线请求）
    int served = 0;
    try {
      while (served < kMaxRequestsPerConn) {
        // ---- 读请求头（空闲时用 keep-alive 超时，收首字节后恢复读超时）----
        size_t hEnd = pending.find("\r\n\r\n");
        bool firstRecv = true;
        setRecvTimeout(cfd, (pending.empty() && served > 0) ? kKeepAliveIdleSec : kReadTimeoutSec);
        char tmp[4096];
        while (hEnd == std::string::npos) {
          ssize_t n = ::recv(cfd, tmp, sizeof(tmp), 0);
          if (n < 0) {
            if (errno == EINTR) continue;      // 信号打断重试，其余（含超时）直接关闭
            closeQuietly(cfd); return;
          }
          if (n == 0) { closeQuietly(cfd); return; } // 客户端断开
          pending.append(tmp, static_cast<size_t>(n));
          if (firstRecv) { setRecvTimeout(cfd, kReadTimeoutSec); firstRecv = false; }
          hEnd = pending.find("\r\n\r\n");
          if (hEnd == std::string::npos && pending.size() > kMaxHeaderBytes) {
            sendError(cfd, 431, "request header too large"); return;
          }
        }
        if (hEnd > kMaxHeaderBytes) { sendError(cfd, 431, "request header too large"); return; }

        std::string head = pending.substr(0, hEnd);

        // ---- 解析请求行与头部 ----
        HttpRequest req;
        size_t contentLength = 0;
        bool hasContentLength = false, hasTransferEncoding = false;
        bool http10 = false, connClose = false, connKeepAlive = false;
        int errStatus = 0;
        std::string errMsg;
        parseHead(head, req, hasContentLength, contentLength, hasTransferEncoding,
                  http10, connClose, connKeepAlive, errStatus, errMsg);
        if (errStatus) { sendError(cfd, errStatus, errMsg); return; }
        if (hasTransferEncoding) { sendError(cfd, 501, "transfer-encoding not supported"); return; }

        // ---- body：按 Content-Length 补齐；剩余字节留给下一请求 ----
        size_t bodyStart = hEnd + 4;
        std::string remainder;
        if (hasContentLength) {
          while (pending.size() - bodyStart < contentLength) {
            ssize_t n = ::recv(cfd, tmp, sizeof(tmp), 0);
            if (n < 0) {
              if (errno == EINTR) continue;
              sendError(cfd, 400, "incomplete body"); return;
            }
            if (n == 0) { sendError(cfd, 400, "incomplete body"); return; }
            pending.append(tmp, static_cast<size_t>(n));
          }
          req.body = pending.substr(bodyStart, contentLength);
          remainder = pending.substr(bodyStart + contentLength);
        } else {
          req.body.clear();
          remainder = pending.substr(bodyStart);
        }

        // ---- 路由 ----
        HttpResponse res;
        dispatch(req, res);
        served++;

        bool keep = !connClose && (!http10 || connKeepAlive) && served < kMaxRequestsPerConn;
        std::string out = buildResponse(res, keep);
        if (!sendAll(cfd, out)) { closeQuietly(cfd); return; }
        if (!keep) { closeQuietly(cfd); return; }
        pending = remainder;   // 复用连接，继续下一个请求
      }
      closeQuietly(cfd);       // 达到单连接请求上限
    } catch (const std::exception& e) {
      // 任何异常都不允许逃出 worker 线程
      std::cerr << "[http] connection error: " << e.what() << std::endl;
      sendError(cfd, 500, "internal server error");
    } catch (...) {
      std::cerr << "[http] connection error: unknown" << std::endl;
      closeQuietly(cfd);
    }
  }

  // 解析请求行与头部；成功返回 errStatus=0
  void parseHead(const std::string& head, HttpRequest& req,
                 bool& hasContentLength, size_t& contentLength, bool& hasTransferEncoding,
                 bool& http10, bool& connClose, bool& connKeepAlive,
                 int& errStatus, std::string& errMsg) {
    errStatus = 0;
    std::istringstream is(head);
    std::string line;
    if (!std::getline(is, line)) { errStatus = 400; errMsg = "bad request line"; return; }
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

    std::istringstream rl(line);
    std::string target, version;
    rl >> req.method >> target >> version;
    if (req.method.empty() || target.empty() || version.empty()) {
      errStatus = 400; errMsg = "bad request line"; return;
    }
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
      errStatus = 400; errMsg = "bad http version"; return;
    }
    http10 = (version == "HTTP/1.0");
    if (target[0] != '/') { errStatus = 400; errMsg = "bad request target"; return; }
    if (target.size() > kMaxPathLen) { errStatus = 414; errMsg = "uri too long"; return; }
    if (req.method != "GET" && req.method != "POST" && req.method != "PUT" &&
        req.method != "DELETE" && req.method != "OPTIONS") {
      errStatus = 405; errMsg = "method not allowed"; return;
    }

    // target 拆 path / query
    auto qpos = target.find('?');
    if (qpos != std::string::npos) {
      req.path = target.substr(0, qpos);
      req.query = target.substr(qpos + 1);
      std::string q = req.query;
      size_t pos = 0;
      while (pos < q.size()) {
        size_t amp = q.find('&', pos);
        std::string pair = (amp == std::string::npos) ? q.substr(pos) : q.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
          req.queryParams[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
      }
    } else {
      req.path = target;
    }

    int headerCount = 0;
    while (std::getline(is, line)) {
      line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
      if (line.empty()) continue;
      if (line.size() > kMaxHeaderLine) { errStatus = 431; errMsg = "header line too large"; return; }
      if (++headerCount > kMaxHeaders) { errStatus = 431; errMsg = "too many headers"; return; }
      auto c = line.find(':');
      if (c == std::string::npos || c == 0) { errStatus = 400; errMsg = "bad header"; return; }
      std::string k = line.substr(0, c);
      std::string v = line.substr(c + 1);
      size_t s = v.find_first_not_of(" \t");
      if (s != std::string::npos) v = v.substr(s);
      std::transform(k.begin(), k.end(), k.begin(), ::tolower);
      if (k == "content-length") {
        size_t parsed = 0;
        if (!parseSize(v, parsed)) { errStatus = 400; errMsg = "bad content-length"; return; }
        if (parsed > kMaxBodyBytes) { errStatus = 413; errMsg = "payload too large"; return; }
        contentLength = parsed;
        hasContentLength = true;
      } else if (k == "transfer-encoding") {
        hasTransferEncoding = true;
      } else if (k == "connection") {
        std::string lv = v;
        std::transform(lv.begin(), lv.end(), lv.begin(), ::tolower);
        if (lv.find("close") != std::string::npos) connClose = true;
        if (lv.find("keep-alive") != std::string::npos) connKeepAlive = true;
      }
      req.headers[k] = v;
    }
  }

  // 路由分发（预编译正则）
  void dispatch(HttpRequest& req, HttpResponse& res) {
    bool matched = false;
    for (const auto& r : routes_) {
      if (r.method != req.method) continue;
      std::smatch m;
      if (std::regex_match(req.path, m, r.re)) {
        size_t vi = 1;
        for (size_t pi = 0; pi < r.paramNames.size(); pi++) {
          if (vi < m.size()) req.pathParams[r.paramNames[pi]] = m[vi++];
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
  }

  static Route compileRoute(const std::string& method, const std::string& pattern, Handler h) {
    std::string re = "^";
    std::vector<std::string> names;
    size_t i = 0;
    while (i < pattern.size()) {
      if (pattern[i] == ':') {
        re += "([^/]+)";
        i++;
        std::string name;
        while (i < pattern.size() && pattern[i] != '/') name += pattern[i++];
        names.push_back(name);
      } else if (pattern[i] == '*') {
        re += ".*";
        i++;
      } else if (pattern[i] == '.') {
        re += "\\.";
        i++;
      } else {
        re += pattern[i];
        i++;
      }
    }
    re += "$";
    return Route{method, pattern, std::move(h), std::regex(re), names};
  }

  static std::string buildResponse(const HttpResponse& res, bool keepAlive) {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << res.status << " " << statusText(res.status) << "\r\n";
    resp << "Content-Type: " << res.contentType << "\r\n";
    resp << "Content-Length: " << res.body.size() << "\r\n";
    resp << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n";
    if (keepAlive) resp << "Keep-Alive: timeout=" << kKeepAliveIdleSec << "\r\n";
    resp << "Access-Control-Allow-Origin: *\r\n";
    resp << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
    resp << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    for (auto& kv : res.extraHeaders) resp << kv.first << ": " << kv.second << "\r\n";
    resp << "\r\n" << res.body;
    return resp.str();
  }

  static void setRecvTimeout(int cfd, int sec) {
    struct timeval tv;
    tv.tv_sec = sec; tv.tv_usec = 0;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  static void closeQuietly(int cfd) {
    ::close(cfd);
  }

  // 循环写直到发完；失败/断开返回 false
  static bool sendAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (n == 0) return false;
      off += static_cast<size_t>(n);
    }
    return true;
  }

  // 解析阶段错误：直接回状态码并关闭连接（不复用）
  void sendError(int cfd, int status, const std::string& msg) {
    HttpResponse res;
    res.status = status;
    res.body = std::string("{\"error\":\"") + msg + "\"}";
    sendAll(cfd, buildResponse(res, false));
    ::close(cfd);
  }

  // 仅接受纯数字；溢出或负数视为非法
  static bool parseSize(const std::string& v, size_t& out) {
    if (v.empty()) return false;
    size_t n = 0;
    for (char c : v) {
      if (c < '0' || c > '9') return false;
      size_t d = static_cast<size_t>(c - '0');
      if (n > (SIZE_MAX - d) / 10) return false;
      n = n * 10 + d;
    }
    out = n;
    return true;
  }

  static std::string statusText(int s) {
    switch (s) {
      case 200: return "OK";
      case 201: return "Created";
      case 204: return "No Content";
      case 400: return "Bad Request";
      case 401: return "Unauthorized";
      case 403: return "Forbidden";
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
      case 409: return "Conflict";
      case 411: return "Length Required";
      case 413: return "Payload Too Large";
      case 414: return "URI Too Long";
      case 429: return "Too Many Requests";
      case 431: return "Request Header Fields Too Large";
      case 500: return "Internal Server Error";
      case 501: return "Not Implemented";
      case 502: return "Bad Gateway";
      case 503: return "Service Unavailable";
      case 504: return "Gateway Timeout";
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
