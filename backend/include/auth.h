// 认证基础工具（v1.2.0-T004）
// 职责：密码哈希（CommonCrypto SHA-256）、随机字节（/dev/urandom）、Cookie 解析
// 零三方依赖：CommonCrypto 与 /dev/urandom 均为 macOS 系统提供
#pragma once

#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <CommonCrypto/CommonDigest.h>

namespace eng
{

// 请求级身份上下文：beforeDispatch 中按 Cookie 设置；未登录=1（游客）
// C++17 inline thread_local，头文件被多个翻译单元包含也安全
class RequestContext
{
public:
  static inline thread_local int userId = 1;

  static int currentUserId()
  {
    return userId;
  }
};

class Auth
{
public:
  // ---- 随机数 ----

  // 读取 n 字节 /dev/urandom，返回 2n 位小写 hex
  static std::string randomHex(size_t bytes)
  {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom) return "";
    unsigned char buf[64];
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    urandom.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(bytes));
    if (!urandom) return "";
    static const char* HEX = "0123456789abcdef";
    std::string out;
    out.resize(bytes * 2);
    for (size_t i = 0; i < bytes; i++)
    {
      out[i * 2]     = HEX[buf[i] >> 4];
      out[i * 2 + 1] = HEX[buf[i] & 0x0f];
    }
    return out;
  }

  // ---- SHA-256 ----

  static std::string sha256Hex(const std::string& input)
  {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(input.data(), static_cast<CC_LONG>(input.size()), digest);
    static const char* HEX = "0123456789abcdef";
    std::string out;
    out.resize(CC_SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
    {
      out[i * 2]     = HEX[digest[i] >> 4];
      out[i * 2 + 1] = HEX[digest[i] & 0x0f];
    }
    return out;
  }

  // ---- 密码 ----

  // 生成新盐（16 字节 → 32 hex）
  static std::string newSalt()
  {
    return randomHex(16);
  }

  // hash = SHA256(saltHex + password)
  static std::string hashPassword(const std::string& password, const std::string& saltHex)
  {
    return sha256Hex(saltHex + password);
  }

  static bool verifyPassword(const std::string& password, const std::string& saltHex,
                             const std::string& expectedHash)
  {
    return hashPassword(password, saltHex) == expectedHash;
  }

  // ---- 会话令牌（32 字节 → 64 hex）----

  static std::string newSessionToken()
  {
    return randomHex(32);
  }

  // ---- Cookie ----

  // 解析原始 Cookie 头："a=1; b=2" → map
  static std::map<std::string, std::string> parseCookies(const std::string& header)
  {
    std::map<std::string, std::string> cookies;
    std::istringstream ss(header);
    std::string pair;
    while (std::getline(ss, pair, ';'))
    {
      size_t eq = pair.find('=');
      if (eq == std::string::npos) continue;
      std::string key = trim(pair.substr(0, eq));
      std::string val = trim(pair.substr(eq + 1));
      if (!key.empty()) cookies[key] = val;
    }
    return cookies;
  }

  static std::string cookieValue(const std::string& header, const std::string& name)
  {
    auto cookies = parseCookies(header);
    auto it = cookies.find(name);
    return it == cookies.end() ? "" : it->second;
  }

private:
  static std::string trim(const std::string& s)
  {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    return s.substr(b, e - b);
  }
};

} // namespace eng
