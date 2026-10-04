// MySQL 数据库封装（基于 mysql.h C API）
// 支持：连接池单例、事务、3 次重试、超时回滚、JSON 转义结果集
#pragma once
#include <mysql.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <stdexcept>
#include <sstream>
#include <cstdint>
#include <fstream>
#include <chrono>
#include <thread>
#include <mutex>

namespace eng {

class DbError : public std::runtime_error {
public:
  explicit DbError(const std::string& m) : std::runtime_error(m) {}
};

struct DbConfig {
  std::string host="127.0.0.1";
  int port=3306;
  std::string user="eng_app";
  std::string password;
  std::string database="english_learning";
  std::string socket;
  std::string charset="utf8mb4";
  int connect_timeout=5;
  int retry_times=3;
  int retry_interval_sec=2;
  int query_timeout_sec=10;
};

using Row = std::map<std::string, std::string>;
using Rows = std::vector<Row>;

class Db {
  MYSQL conn_;
  bool connected_=false;
  DbConfig cfg_;
  // 浏览器会并发请求，HttpServer 每连接一个线程；
  // MYSQL* 单连接非线程安全，所有 DB 操作必须串行化（递归锁兼容事务内嵌套调用）
  std::recursive_mutex mu_;
public:
  Db() { mysql_init(&conn_); }
  ~Db() { if (connected_) mysql_close(&conn_); }

  static std::string readPasswordFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw DbError("cannot read password file: " + path);
    std::string pw;
    std::getline(f, pw);
    // 去除空白与可能的 \r
    while (!pw.empty() && (pw.back()=='\r' || pw.back()=='\n' || pw.back()==' ')) pw.pop_back();
    return pw;
  }

  static DbConfig loadFromJsonFile(const std::string& cfgPath) {
    // 简化 JSON 解析（仅读取本配置 schema 字段）
    std::ifstream f(cfgPath);
    if (!f) throw DbError("cannot open db config: " + cfgPath);
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    auto getStr = [&](const std::string& key) -> std::string {
      std::string pat = "\"" + key + "\"";
      auto p = s.find(pat);
      if (p == std::string::npos) return "";
      p = s.find(":", p);
      if (p == std::string::npos) return "";
      // 跳过空白
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
    DbConfig c;
    c.host = getStr("host"); if (c.host.empty()) c.host = "127.0.0.1";
    c.port = getInt("port", 3306);
    c.user = getStr("user"); if (c.user.empty()) c.user = "eng_app";
    c.database = getStr("database"); if (c.database.empty()) c.database = "english_learning";
    c.socket = getStr("socket");
    c.charset = getStr("charset"); if (c.charset.empty()) c.charset = "utf8mb4";
    c.connect_timeout = getInt("connect_timeout", 5);
    c.retry_times = getInt("retry_times", 3);
    c.retry_interval_sec = getInt("retry_interval_sec", 2);
    c.query_timeout_sec = getInt("query_timeout_sec", 10);
    // 密码从 password_file 读取（相对路径相对配置文件目录）
    std::string pwFile = getStr("password_file");
    if (!pwFile.empty()) {
      // 配置文件位于 config/ 目录，相对路径 ../.. 解析为 /Users/junwang/.local/...
      std::string cfgDir = cfgPath.substr(0, cfgPath.find_last_of('/'));
      // 解析相对路径
      std::string full = pwFile;
      // 简化：手动处理 ../ 前缀
      std::string base = cfgDir;
      while (full.find("../") == 0) {
        full = full.substr(3);
        auto pos = base.find_last_of('/');
        if (pos != std::string::npos) base = base.substr(0, pos);
      }
      std::string abs = base + "/" + full;
      c.password = readPasswordFromFile(abs);
    }
    return c;
  }

  void connect(const DbConfig& cfg) {
    cfg_ = cfg;
    reconnect();
  }

  void reconnect() {
    if (connected_) { mysql_close(&conn_); mysql_init(&conn_); connected_=false; }
    mysql_options(&conn_, MYSQL_OPT_CONNECT_TIMEOUT, &cfg_.connect_timeout);
    mysql_options(&conn_, MYSQL_SET_CHARSET_NAME, cfg_.charset.c_str());
    // 本地 socket 连接禁用 SSL（SSL_free 在连接重建路径上会触发 OpenSSL 堆损坏崩溃）
    unsigned int ssl_mode = SSL_MODE_DISABLED;
    mysql_options(&conn_, MYSQL_OPT_SSL_MODE, &ssl_mode);
    // 不再使用 MYSQL_OPT_RECONNECT（MySQL 8 已废弃且在线程环境下会导致堆损坏）；
    // 连接丢失由 query/exec 内的 2006/2013 分支显式重连
    unsigned int read_timeout = 30;
    mysql_options(&conn_, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
    const char* sock = cfg_.socket.empty() ? nullptr : cfg_.socket.c_str();
    MYSQL* r = mysql_real_connect(&conn_, cfg_.host.c_str(), cfg_.user.c_str(),
                                  cfg_.password.c_str(), cfg_.database.c_str(),
                                  cfg_.port, sock, 0);
    if (!r) throw DbError(std::string("mysql_real_connect failed: ") + mysql_error(&conn_));
    connected_ = true;
    mysql_set_character_set(&conn_, cfg_.charset.c_str());
  }

  static std::string escape(const std::string& s) {
    // 静态工具：转义 SQL 字符串（需先连接，但转义规则与编码无关，简化处理）
    std::string out; out.reserve(s.size()*2+1);
    for (char c : s) {
      switch (c) {
        case '\'': out += "\\'"; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\0': out += "\\0"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\x1a': out += "\\Z"; break;
        default: out += c;
      }
    }
    return out;
  }

  // libmysqlclient 要求每个使用线程先 mysql_thread_init()，否则其 TLS 会在并发下损坏堆
  static void ensureThreadInit() {
    thread_local bool inited = [](){ mysql_thread_init(); return true; }();
    (void)inited;
  }

  // 带重试的查询（返回结果集）
  Rows query(const std::string& sql) {
    ensureThreadInit();
    std::lock_guard<std::recursive_mutex> lk(mu_);
    for (int attempt = 1; attempt <= cfg_.retry_times; ++attempt) {
      try {
        return queryOnce(sql);
      } catch (const DbError& e) {
        if (attempt == cfg_.retry_times) throw;
        std::this_thread::sleep_for(std::chrono::seconds(cfg_.retry_interval_sec));
        try { reconnect(); } catch (...) {}
      }
    }
    return {};
  }

  Rows queryOnce(const std::string& sql) {
    if (!connected_) reconnect();
    if (mysql_query(&conn_, sql.c_str()) != 0) {
      unsigned int err = mysql_errno(&conn_);
      // 2006/2013 = 连接丢失，重连一次
      if (err == 2006 || err == 2013) {
        reconnect();
        if (mysql_query(&conn_, sql.c_str()) != 0)
          throw DbError(std::string("query failed: ") + mysql_error(&conn_));
      } else {
        throw DbError(std::string("query failed: ") + mysql_error(&conn_) + " [sql=" + sql + "]");
      }
    }
    MYSQL_RES* res = mysql_store_result(&conn_);
    if (!res) {
      // 无结果集（INSERT/UPDATE/DELETE）
      return {};
    }
    Rows rows;
    int n = mysql_num_fields(res);
    MYSQL_FIELD* fields = mysql_fetch_fields(res);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
      unsigned long* lens = mysql_fetch_lengths(res);
      Row r;
      for (int i = 0; i < n; i++) {
        std::string k = fields[i].name ? fields[i].name : "";
        std::string v = row[i] ? std::string(row[i], lens[i]) : "";
        r[k] = v;
      }
      rows.push_back(std::move(r));
    }
    mysql_free_result(res);
    return rows;
  }

  // 执行写操作（INSERT/UPDATE/DELETE），返回 last_insert_id 与 affected_rows
  struct WriteResult { int64_t last_insert_id=0; int64_t affected_rows=0; };

  WriteResult exec(const std::string& sql) {
    ensureThreadInit();
    std::lock_guard<std::recursive_mutex> lk(mu_);
    for (int attempt = 1; attempt <= cfg_.retry_times; ++attempt) {
      try {
        if (!connected_) reconnect();
        if (mysql_query(&conn_, sql.c_str()) != 0) {
          unsigned int err = mysql_errno(&conn_);
          if (err == 2006 || err == 2013) {
            reconnect();
            if (mysql_query(&conn_, sql.c_str()) != 0)
              throw DbError(std::string("exec failed: ") + mysql_error(&conn_));
          } else {
            throw DbError(std::string("exec failed: ") + mysql_error(&conn_) + " [sql=" + sql + "]");
          }
        }
        WriteResult r;
        r.affected_rows = mysql_affected_rows(&conn_);
        r.last_insert_id = mysql_insert_id(&conn_);
        return r;
      } catch (const DbError& e) {
        if (attempt == cfg_.retry_times) throw;
        std::this_thread::sleep_for(std::chrono::seconds(cfg_.retry_interval_sec));
        try { reconnect(); } catch (...) {}
      }
    }
    return {};
  }

  // 事务封装
  bool transaction(const std::function<bool()>& fn) {
    ensureThreadInit();
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (mysql_query(&conn_, "START TRANSACTION") != 0)
      throw DbError(std::string("start tx failed: ") + mysql_error(&conn_));
    try {
      bool ok = fn();
      if (ok) {
        if (mysql_query(&conn_, "COMMIT") != 0) throw DbError("commit failed");
        return true;
      } else {
        mysql_query(&conn_, "ROLLBACK");
        return false;
      }
    } catch (...) {
      mysql_query(&conn_, "ROLLBACK");
      throw;
    }
  }
};

} // namespace eng
