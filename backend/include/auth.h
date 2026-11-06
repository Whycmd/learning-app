// 认证基础工具（v1.2.0-T004）
// 职责：密码哈希（内置 SHA-256）、随机字节（/dev/urandom）、Cookie 解析
// 零三方依赖：SHA-256 为内置纯 C++ 实现，/dev/urandom 为系统提供
// 说明：v1.4.1-T001 起不再依赖 macOS 的 CommonCrypto，改用内置实现，
//       以便同一份源码在 macOS 和 Linux 容器中都能编译，
//       且两平台产出的哈希值完全一致（历史用户密码不受影响）。
#pragma once

#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cstdint>

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
  // 内置实现，遵循 FIPS 180-4 标准。
  // 输出与 macOS CommonCrypto 的 CC_SHA256 完全一致，
  // 因此迁移到 Linux 后，数据库里已有的密码哈希依然能校验通过。
  static std::string sha256Hex(const std::string& input)
  {
    // 标准初始向量
    uint32_t h[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    // 消息填充：先补 1 个 0x80，再补 0 直到长度 ≡ 56 (mod 64)，
    // 最后追加 8 字节原始长度（比特数，大端）
    std::string msg = input;
    const uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; i--)
      msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xff));

    // 逐 64 字节块压缩
    for (size_t off = 0; off < msg.size(); off += 64)
      compressBlock(h, reinterpret_cast<const unsigned char*>(msg.data() + off));

    static const char* HEX = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (int i = 0; i < 8; i++)
    {
      for (int j = 0; j < 4; j++)
      {
        unsigned char byte = static_cast<unsigned char>((h[i] >> (24 - j * 8)) & 0xff);
        out[(i * 4 + j) * 2]     = HEX[byte >> 4];
        out[(i * 4 + j) * 2 + 1] = HEX[byte & 0x0f];
      }
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
  // 32 位循环右移
  static uint32_t rotr(uint32_t x, uint32_t n)
  {
    return (x >> n) | (x << (32 - n));
  }

  // SHA-256 单个 64 字节块的压缩函数
  static void compressBlock(uint32_t state[8], const unsigned char block[64])
  {
    // 64 个轮常量（FIPS 180-4 定义）
    static const uint32_t K[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
      0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
      0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
      0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
      0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
      0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
      0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
      0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
      0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
      0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };

    uint32_t w[64];

    // 前 16 个字：直接把 64 字节按大端拆成 32 位整数
    for (int i = 0; i < 16; i++)
    {
      w[i] = (static_cast<uint32_t>(block[i * 4])     << 24)
           | (static_cast<uint32_t>(block[i * 4 + 1]) << 16)
           | (static_cast<uint32_t>(block[i * 4 + 2]) << 8)
           |  static_cast<uint32_t>(block[i * 4 + 3]);
    }
    // 后 48 个字：由前面推导
    for (int i = 16; i < 64; i++)
    {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++)
    {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + S1 + ch + K[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;

      h = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
  }

  static std::string trim(const std::string& s)
  {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    return s.substr(b, e - b);
  }
};

} // namespace eng
