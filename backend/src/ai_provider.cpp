// AI Provider 实现
#include "ai_provider.h"
#include "ai_http.h"
#include "json.h"
#include <stdexcept>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cctype>
#include <thread>
#include <chrono>

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
// Mock Provider（测试专用，不发网络请求）
// 模式：success / timeout / 5xx；fixture 内容来自 config/ai_mock.json
// ──────────────────────────────────────────────
class MockAiProvider : public IAiProvider {
  AiConfig cfg_;
  JsonValue fixture_ = JsonValue::makeObject({});

  static std::string fillWord(const std::string& s, const std::string& word) {
    std::string out = s;
    size_t p = 0;
    while ((p = out.find("{WORD}", p)) != std::string::npos) {
      out.replace(p, 6, word);
    }
    return out;
  }

  // 从提示词 JSON 中提取真实 knowledge_id
  static std::vector<int> extractIds(const std::string& s) {
    std::vector<int> ids;
    size_t p = 0;
    while ((p = s.find("\"id\"", p)) != std::string::npos) {
      size_t c = s.find(':', p + 4);
      if (c == std::string::npos) break;
      size_t d = c + 1;
      while (d < s.size() && std::isspace(static_cast<unsigned char>(s[d]))) d++;
      std::string num;
      while (d < s.size() && std::isdigit(static_cast<unsigned char>(s[d]))) num += s[d++];
      if (!num.empty()) {
        int v = std::atoi(num.c_str());
        if (std::find(ids.begin(), ids.end(), v) == ids.end()) ids.push_back(v);
      }
      p = d;
    }
    return ids;
  }

  static std::string singleWord(const std::string& user) {
    size_t p = user.find("目标词：");
    if (p == std::string::npos) return "";
    size_t q1 = user.find('"', p);
    if (q1 == std::string::npos) return "";
    size_t q2 = user.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return user.substr(q1 + 1, q2 - q1 - 1);
  }

  static std::vector<std::string> batchWords(const std::string& user) {
    std::vector<std::string> words;
    size_t p = user.find("词表：");
    if (p == std::string::npos) return words;
    std::istringstream is(user.substr(p + 3));
    std::string line;
    while (std::getline(is, line)) {
      size_t q1 = line.find('"');
      if (q1 == std::string::npos) continue;
      size_t q2 = line.find('"', q1 + 1);
      if (q2 == std::string::npos) continue;
      std::string w = line.substr(q1 + 1, q2 - q1 - 1);
      if (!w.empty()) words.push_back(w);
    }
    return words;
  }

  std::string secStr(const std::string& sec, const std::string& key, const std::string& def) const {
    const JsonValue* s = fixture_.find(sec);
    if (s && s->isObject()) return s->getString(key, def);
    return def;
  }
  int secInt(const std::string& sec, const std::string& key, int def) const {
    const JsonValue* s = fixture_.find(sec);
    if (s && s->isObject()) return (int)s->getInt(key, def);
    return def;
  }
  int topInt(const std::string& key, int def) const {
    return (int)fixture_.getInt(key, def);
  }

  JsonValue buildSolidify(const std::string& word) {
    std::string example = fillWord(
        secStr("solidify", "example", "This is a mock example sentence for {WORD}."), word);
    JsonObject d1;
    d1["pos"] = JsonValue::makeString(secStr("solidify", "pos", "n."));
    d1["meaning"] = JsonValue::makeString(secStr("solidify", "meaning", "（模拟释义）核心含义"));
    d1["example"] = JsonValue::makeString(example);
    JsonObject d2;
    d2["pos"] = JsonValue::makeString(secStr("solidify", "pos2", "v."));
    d2["meaning"] = JsonValue::makeString(secStr("solidify", "meaning2", "（模拟释义）引申含义"));
    d2["example"] = JsonValue::makeString(example);

    JsonObject coll;
    coll["collocation"] = JsonValue::makeString(secStr("solidify", "collocation", "mock collocation"));
    coll["example"] = JsonValue::makeString(example);

    JsonObject o;
    o["standard_content"] = JsonValue::makeString(word);
    o["difficulty"] = JsonValue::makeInt(secInt("solidify", "difficulty", 3));
    o["definitions"] = JsonValue::makeArray({JsonValue::makeObject(d1), JsonValue::makeObject(d2)});
    o["collocations"] = JsonValue::makeArray({JsonValue::makeObject(coll)});
    o["synonyms"] = JsonValue::makeArray({JsonValue::makeString(secStr("solidify", "synonym", "mock synonym"))});
    o["antonyms"] = JsonValue::makeArray({JsonValue::makeString(secStr("solidify", "antonym", "mock antonym"))});
    o["markdown_content"] = JsonValue::makeArray({
        JsonValue::makeString(std::string("# 单词精讲：") + word),
        JsonValue::makeString("**音标**：/mock/"),
        JsonValue::makeString("## 核心含义与例句"),
        JsonValue::makeString(std::string("> ") + example),
        JsonValue::makeString("## 固定搭配"),
        JsonValue::makeString(std::string("- ") + secStr("solidify", "collocation", "mock collocation")),
    });
    return JsonValue::makeObject(o);
  }

  JsonValue buildExercise(const std::string& user) {
    std::vector<int> ids = extractIds(user);
    if (ids.empty()) ids.push_back(1);
    auto kid = [&](size_t i) { return JsonValue::makeInt(ids[i % ids.size()]); };

    JsonObject e1;
    e1["type"] = JsonValue::makeString("word_match");
    e1["passage"] = JsonValue::makeString("Mock passage for word matching.");
    e1["question"] = JsonValue::makeString("Choose the best match for the mock word.");
    e1["options"] = JsonValue::makeArray({JsonValue::makeString("alpha"), JsonValue::makeString("beta"),
                                          JsonValue::makeString("gamma"), JsonValue::makeString("delta")});
    e1["answer"] = JsonValue::makeString("A");
    e1["knowledge_id"] = kid(0);

    JsonObject e2;
    e2["type"] = JsonValue::makeString("word_form_fill");
    e2["question"] = JsonValue::makeString("The mock team will ______ the plan tomorrow.");
    e2["answer"] = JsonValue::makeString("apply");
    e2["knowledge_id"] = kid(1);

    JsonObject e3;
    e3["type"] = JsonValue::makeString("context_blank_fill");
    e3["question"] = JsonValue::makeString("Please ______ the correct option from the list.");
    e3["options"] = JsonValue::makeArray({JsonValue::makeString("alpha"), JsonValue::makeString("beta"),
                                          JsonValue::makeString("gamma"), JsonValue::makeString("delta")});
    e3["answer"] = JsonValue::makeString("B");
    e3["knowledge_id"] = kid(2);

    JsonObject e4;
    e4["type"] = JsonValue::makeString("cn_to_en_translation");
    e4["question"] = JsonValue::makeString("请翻译：这是一个模拟句子。");
    e4["reference_answer"] = JsonValue::makeString("This is a mock sentence.");
    e4["knowledge_id"] = kid(3);

    JsonObject e5;
    e5["type"] = JsonValue::makeString("cloze_choice");
    e5["passage"] = JsonValue::makeString("The mock team ______ the plan carefully.");
    e5["options"] = JsonValue::makeArray({JsonValue::makeString("reviewed"), JsonValue::makeString("review"),
                                          JsonValue::makeString("reviewing"), JsonValue::makeString("reviews")});
    e5["answer"] = JsonValue::makeString("A");
    e5["knowledge_id"] = kid(4);

    JsonObject e6;
    e6["type"] = JsonValue::makeString("sentence_imitation");
    e6["prompt"] = JsonValue::makeString("Imitate the mock sentence pattern.");
    e6["sample"] = JsonValue::makeString("The mock team reviewed the plan carefully.");
    e6["knowledge_id"] = kid(5);

    JsonObject memoItem;
    memoItem["knowledge"] = JsonValue::makeString("mock");
    memoItem["brief"] = JsonValue::makeString("（模拟）核心释义");
    JsonObject quick;
    quick["items"] = JsonValue::makeArray({JsonValue::makeObject(memoItem)});

    JsonObject root;
    root["exercises"] = JsonValue::makeArray({JsonValue::makeObject(e1), JsonValue::makeObject(e2),
                                              JsonValue::makeObject(e3), JsonValue::makeObject(e4),
                                              JsonValue::makeObject(e5), JsonValue::makeObject(e6)});
    root["quick_memo"] = JsonValue::makeObject(quick);
    return JsonValue::makeObject(root);
  }

  JsonValue buildGrade(const std::string& user) {
    std::string ref = secStr("grade", "reference_answer", "（模拟）参考译文");
    size_t p = user.find("参考答案：");
    if (p != std::string::npos) {
      size_t e = user.find('\n', p);
      std::string v = user.substr(p + 5, e == std::string::npos ? std::string::npos : e - p - 5);
      size_t b = v.find_first_not_of(" \t\r\n");
      if (b != std::string::npos) ref = v.substr(b);
    }
    JsonObject o;
    o["score"] = JsonValue::makeInt(secInt("grade", "score", 100));
    o["correct"] = JsonValue::makeBool(true);
    o["error_analysis"] = JsonValue::makeString(secStr("grade", "error_analysis", "（模拟批改）表达正确"));
    o["reference_answer"] = JsonValue::makeString(ref);
    return JsonValue::makeObject(o);
  }

  JsonValue buildQuiz(const std::string& user) {
    std::vector<int> ids = extractIds(user);
    if (ids.empty()) ids.push_back(1);
    JsonArray qs;
    for (int i = 1; i <= 3; i++) {
      JsonObject q;
      q["no"] = JsonValue::makeInt(i);
      q["category"] = JsonValue::makeString("solidified");
      q["type"] = JsonValue::makeString("cloze_choice");
      q["question"] = JsonValue::makeString("Mock sentence " + std::to_string(i) + " has ______ here.");
      q["options"] = JsonValue::makeArray({JsonValue::makeString("alpha"), JsonValue::makeString("beta"),
                                            JsonValue::makeString("gamma"), JsonValue::makeString("delta")});
      q["answer"] = JsonValue::makeString("A");
      q["knowledge_id"] = JsonValue::makeInt(ids[(i - 1) % ids.size()]);
      qs.push_back(JsonValue::makeObject(q));
    }
    JsonObject q4;
    q4["no"] = JsonValue::makeInt(4);
    q4["category"] = JsonValue::makeString("familiar_meaning");
    q4["type"] = JsonValue::makeString("cloze_choice");
    q4["word"] = JsonValue::makeString(secStr("quiz", "familiar_word", "run"));
    q4["meaning"] = JsonValue::makeString(secStr("quiz", "familiar_meaning", "经营；管理"));
    q4["example"] = JsonValue::makeString(secStr("quiz", "familiar_example", "He runs a company."));
    q4["question"] = JsonValue::makeString("The manager ______ the company.");
    q4["options"] = JsonValue::makeArray({JsonValue::makeString("controls"), JsonValue::makeString("runs"),
                                          JsonValue::makeString("owns"), JsonValue::makeString("operates")});
    q4["answer"] = JsonValue::makeString("B");
    qs.push_back(JsonValue::makeObject(q4));

    JsonObject q5;
    q5["no"] = JsonValue::makeInt(5);
    q5["category"] = JsonValue::makeString("new_kp");
    q5["type"] = JsonValue::makeString("word_form_fill");
    q5["word"] = JsonValue::makeString(secStr("quiz", "new_word", "ample"));
    q5["meaning"] = JsonValue::makeString(secStr("quiz", "new_meaning", "充足的"));
    q5["example"] = JsonValue::makeString(secStr("quiz", "new_example", "There was ample food."));
    q5["question"] = JsonValue::makeString("Food was ______ during the festival.");
    q5["answer"] = JsonValue::makeString(secStr("quiz", "new_word", "ample"));
    qs.push_back(JsonValue::makeObject(q5));

    JsonObject root;
    root["questions"] = JsonValue::makeArray(qs);
    return JsonValue::makeObject(root);
  }

public:
  void setConfig(const AiConfig& c) override {
    cfg_ = c;
    fixture_ = JsonValue::makeObject({});
    if (c.mock_fixture_path.empty()) return;
    std::ifstream f(c.mock_fixture_path);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    try {
      JsonValue v = parseJson(ss.str());
      if (v.isObject()) fixture_ = v;
    } catch (...) {
      // fixture 缺失/损坏时使用内置默认值，不影响 Mock 启动
    }
  }
  AiProviderType getProviderType() const override { return AiProviderType::MOCK; }
  bool isDegraded() const override {
    return cfg_.mock_mode == "timeout" || cfg_.mock_mode == "5xx";
  }
  bool ping() override {
    return cfg_.mock_mode != "timeout" && cfg_.mock_mode != "5xx";
  }

  AiResponse postChat(const AiRequest& req) override {
    AiResponse r;
    if (cfg_.mock_mode == "timeout") {
      int ms = topInt("timeout_delay_ms", 300);
      if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      r.status = 0;
      r.body = "mock timeout";
      r.ok = false;
      return r;
    }
    if (cfg_.mock_mode == "5xx") {
      r.status = topInt("server_error_status", 503);
      r.body = "mock upstream error";
      r.ok = false;
      return r;
    }

    std::string sys, user;
    for (const auto& m : req.messages) {
      std::string role = m.count("role") ? m.at("role") : "";
      std::string content = m.count("content") ? m.at("content") : "";
      if (role == "system") sys += content;
      else user += content;
    }

    JsonValue out;
    if (sys.find("grading assistant") != std::string::npos ||
        user.find("请批改以下英语主观题答案") != std::string::npos) {
      out = buildGrade(user);
    } else if (sys.find("exercise generator") != std::string::npos) {
      out = buildExercise(user);
    } else if (sys.find("daily-quiz") != std::string::npos) {
      out = buildQuiz(user);
    } else {
      // 固化场景：单条 / 批量
      std::vector<std::string> words = batchWords(user);
      std::string one = singleWord(user);
      if (!one.empty()) words = {one};
      // 批量固化模拟真实耗时，便于测试观察“生成中渐进可见”
      if (words.size() > 1) {
        int ms = topInt("batch_delay_ms", 1200);
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      }
      JsonArray arr;
      for (const auto& w : words) arr.push_back(buildSolidify(w));
      out = words.size() == 1 ? arr[0] : JsonValue::makeArray(arr);
      if (words.empty()) out = buildSolidify("mock");
    }

    r.status = 200;
    r.ok = true;
    r.body = "{\"mock\":true}";
    r.extractedJson = out.dump();
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
  if (name == "mock")
    return AiProviderType::MOCK;
  return AiProviderType::OPENAI_COMPATIBLE; // 默认兼容
}

std::string providerTypeName(AiProviderType type) {
  switch (type) {
    case AiProviderType::OPENAI_COMPATIBLE: return "openai_compatible";
    case AiProviderType::AGNES:             return "agnes";
    case AiProviderType::MOCK:              return "mock";
    default:                                return "unknown";
  }
}

std::unique_ptr<IAiProvider> createAiProvider(AiProviderType type) {
  switch (type) {
    case AiProviderType::OPENAI_COMPATIBLE:
      return std::make_unique<OpenAiCompatibleProvider>();
    case AiProviderType::AGNES:
      return std::make_unique<AgnesProvider>();
    case AiProviderType::MOCK:
      return std::make_unique<MockAiProvider>();
    default:
      return std::make_unique<OpenAiCompatibleProvider>();
  }
}

} // namespace eng
