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
  // [TEST] 按 base_url 指定前 N 次请求失败（用于验证主备切换）
  int failRemaining_ = 0;
  // [TEST] 每次请求固定延迟，用于队列超时用例
  int delayMs_ = 0;

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

  JsonValue buildWordlist(const std::string& user) {
    std::string topic = "mock topic";
    const std::string topicKey = "主题：";
    size_t tp = user.find(topicKey);
    if (tp != std::string::npos) {
      size_t start = tp + topicKey.size();
      size_t e = user.find('\n', start);
      std::string v = user.substr(start, e == std::string::npos ? std::string::npos : e - start);
      size_t b = v.find_first_not_of(" \t\r\n");
      if (b != std::string::npos) topic = v.substr(b);
    }
    int count = 5;
    const std::string countKey = "目标词数：";
    size_t cp = user.find(countKey);
    if (cp != std::string::npos) {
      try { count = std::stoi(user.substr(cp + countKey.size())); } catch (...) {}
    }
    if (count < 1) count = 5;
    if (count > 50) count = 50;

    static const std::vector<std::string> base = {
      "apple", "book", "change", "develop", "effort", "famous", "grow", "help", "important", "journey"
    };
    JsonArray words;
    for (int i = 0; i < count; ++i) {
      std::string w = i < (int)base.size() ? base[i] : ("term_" + std::to_string(i + 1));
      JsonObject o;
      o["word"] = JsonValue::makeString(w);
      o["meaning"] = JsonValue::makeString(secStr("wordlist", "meaning", "（模拟）释义"));
      o["pos"] = JsonValue::makeString(secStr("wordlist", "pos", "n."));
      words.push_back(JsonValue::makeObject(o));
    }
    JsonObject root;
    root["title"] = JsonValue::makeString("Mock · " + topic);
    root["words"] = JsonValue::makeArray(words);
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

  JsonValue buildExplain(const std::string& user) {
    std::string text = "mock";
    size_t p = user.find("学习内容：");
    if (p != std::string::npos) {
      size_t e = user.find('\n', p);
      text = user.substr(p + 15, e == std::string::npos ? std::string::npos : e - (p + 15));
    }
    std::string summary = fillWord(secStr("explain", "summary", "（模拟）{WORD} 的核心释义"), text);
    std::string md = fillWord(secStr("explain", "markdown", "# 单词精讲：{WORD}\n\n- （模拟）核心含义"), text);
    JsonObject s1;
    s1["title"] = JsonValue::makeString("核心含义");
    s1["content"] = JsonValue::makeString("（模拟）这是核心含义讲解");
    JsonObject s2;
    s2["title"] = JsonValue::makeString("用法要点");
    s2["content"] = JsonValue::makeString("（模拟）这是用法要点");
    JsonObject o;
    o["summary"] = JsonValue::makeString(summary);
    o["markdown_content"] = JsonValue::makeString(md);
    o["sections"] = JsonValue::makeArray({JsonValue::makeObject(s1), JsonValue::makeObject(s2)});
    return JsonValue::makeObject(o);
  }

  // 从 "题型数量：cloze_choice×2 ..." 还原题型与数量
  JsonValue buildCustomExercise(const std::string& user) {
    std::vector<std::pair<std::string, int>> plan;
    size_t p = user.find("题型数量：");
    std::string seg;
    if (p != std::string::npos) {
      size_t e = user.find("；", p);
      seg = user.substr(p + 15, e == std::string::npos ? std::string::npos : e - (p + 15));
    }
    std::istringstream is(seg);
    std::string tok;
    while (is >> tok) {
      size_t x = tok.find("×");
      if (x == std::string::npos) continue;
      std::string type = tok.substr(0, x);
      int n = 0;
      try { n = std::stoi(tok.substr(x + 2)); } catch (...) { continue; }
      if (n > 0) plan.push_back({type, n});
    }
    if (plan.empty()) plan.push_back({"cloze_choice", 1});
    JsonArray items;
    for (auto& kv : plan) {
      for (int i = 0; i < kv.second && items.size() < 100; i++) {
        std::string type = kv.first;
        std::string word = secStr("custom_exercise", "word", "mock");
        JsonObject it;
        it["type"] = JsonValue::makeString(type);
        it["word"] = JsonValue::makeString(word);
        it["stem"] = JsonValue::makeString(
            fillWord(secStr("custom_exercise", "stem", "He ___ to school every day. ({WORD})"), word));
        if (type == "word_match" || type == "cloze_choice") {
          it["options"] = JsonValue::makeArray({
              JsonValue::makeString("A. optionA"), JsonValue::makeString("B. optionB"),
              JsonValue::makeString("C. optionC"), JsonValue::makeString("D. optionD")});
          it["answer"] = JsonValue::makeString("A");
        } else {
          it["answer"] = JsonValue::makeString(secStr("custom_exercise", "answer", "mock"));
        }
        it["explanation"] = JsonValue::makeString("（模拟）解析");
        items.push_back(JsonValue::makeObject(it));
      }
    }
    JsonObject o;
    o["items"] = JsonValue::makeArray(items);
    return JsonValue::makeObject(o);
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
    // 主备切换测试：fail_by_base_url 为 { "<base_url>": N }，N 次请求内返回 5xx
    failRemaining_ = 0;
    if (auto* byUrl = fixture_.find("fail_by_base_url"); byUrl && byUrl->isObject()) {
      if (auto* n = byUrl->find(c.base_url); n) failRemaining_ = (int)n->asInt();
    }
    // [TEST] 环境变量覆写，避免修改共享 fixture 影响其他套件
    if (const char* v = std::getenv("ENG_AI_MOCK_FAIL_BY_URL"); v && *v) {
      try {
        auto arr = parseJson(v);
        if (arr.isObject()) {
          if (auto* n = arr.find(c.base_url); n) failRemaining_ = (int)n->asInt();
        }
      } catch (...) {}
    }
    delayMs_ = topInt("delay_ms", 0);
    if (const char* v = std::getenv("ENG_AI_MOCK_DELAY_MS"); v && *v) {
      try { delayMs_ = std::max(0, std::stoi(v)); } catch (...) {}
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
    if (delayMs_ > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
    if (failRemaining_ > 0) {
      failRemaining_--;
      r.status = topInt("server_error_status", 503);
      r.body = "mock upstream error (provider fail plan)";
      r.ok = false;
      return r;
    }
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
    if (sys.find("continuing an existing explanation") != std::string::npos) {
      // v1.5.0-T008：精讲追问
      JsonObject o;
      o["answer_md"] = JsonValue::makeString(
          secStr("followup", "answer_md", "## （模拟）追问回答\n\n- 这是模拟追问内容"));
      o["section_title"] = JsonValue::makeString(secStr("followup", "section_title", "（模拟）追问"));
      out = JsonValue::makeObject(o);
    } else if (sys.find("grading a subjective exercise") != std::string::npos) {
      JsonObject o;
      o["score"] = JsonValue::makeInt(90);
      o["correct"] = JsonValue::makeBool(true);
      o["feedback"] = JsonValue::makeString("（模拟）批改通过");
      o["reference"] = JsonValue::makeString(secStr("grade", "reference_answer", "（模拟）参考答案"));
      out = JsonValue::makeObject(o);
    } else if (sys.find("English tutor") != std::string::npos) {
      out = buildExplain(user);
    } else if (sys.find("English test item writer") != std::string::npos) {
      out = buildCustomExercise(user);
    } else if (sys.find("grading assistant") != std::string::npos ||
        user.find("请批改以下英语主观题答案") != std::string::npos) {
      out = buildGrade(user);
    } else if (sys.find("vocabulary list generator") != std::string::npos) {
      out = buildWordlist(user);
    } else if (sys.find("英语学习教练") != std::string::npos) {
      // v1.5.1-T003：学情周报下周建议（返回 answer 形状，验证前端/服务端兼容）
      JsonArray arr;
      arr.push_back(JsonValue::makeString("（模拟）每天复习 10 个易错词，先清空重练队列。"));
      arr.push_back(JsonValue::makeString("（模拟）把课规目标拆小，保证每天不断档。"));
      JsonObject o;
      o["answer"] = JsonValue::makeArray(arr);
      out = JsonValue::makeObject(o);
    } else if (sys.find("站内导航助手") != std::string::npos) {
      // v1.5.1-T005：AI 导航；v1.5.2-T001：判定 JSON 夹具（按最后一条用户消息关键词分支，
      // 默认 answerable 保持 stage33 行为；关键词改动需与 tests/stage34_dev_intake.test.js 同步）
      std::string navText;
      for (const auto& m : req.messages)
        if (m.count("role") && m.at("role") != "system" && m.count("content")) navText = m.at("content");
      auto navOut = [](const std::string& reply, const std::string& kind,
                       const std::string& title, const std::string& category, double conf) {
        JsonObject o;
        o["reply"] = JsonValue::makeString(reply);
        o["kind"] = JsonValue::makeString(kind);
        if (!title.empty()) o["title"] = JsonValue::makeString(title);
        if (!category.empty()) o["category"] = JsonValue::makeString(category);
        o["confidence"] = JsonValue::makeDouble(conf);
        return JsonValue::makeObject(o);
      };
      if (navText.find("日语") != std::string::npos)
        out = navOut("这个暂不在本应用范围，本站只做英语学习。", "out_of_scope", "支持日语听力练习", "", 0.9);
      else if (navText.find("点了没反应") != std::string::npos)
        out = navOut("操作没响应确实不正常，我帮你记录下来。", "bug", "导出单词卡按钮无反应", "export", 0.9);
      else if (navText.find("数据不显示") != std::string::npos)
        out = navOut("数据没显示确实不正常，我帮你记录下来。", "bug", "错题本数据不显示", "data", 0.9);
      else if (navText.find("横版") != std::string::npos)
        out = navOut("这个功能现在还没有，你的建议我帮你记录下来。", "feature_request", "想要横版单词卡", "enhancement", 0.9);
      else if (navText.find("听写") != std::string::npos)
        out = navOut("听写练习现在还没有，你的建议我帮你记录下来。", "feature_request", "支持听写练习", "feature", 0.9);
      else if (navText.find("低置信") != std::string::npos)
        out = navOut("这个我不太确定，先帮你记下来。", "feature_request", "低置信测试需求", "enhancement", 0.2);
      else if (navText.find("纯文本") != std::string::npos)
        out = JsonValue::makeString("（模拟）这是一段纯文本回复"); // 非对象 → 判定缺失，走降级
      else if (navText.find("坏JSON") != std::string::npos)
      {
        r.status = 200;
        r.ok = true;
        r.body = "{\"content\":\"（模拟）坏JSON降级回复\"}";
        r.extractedJson = "{这不是合法 JSON";
        return r;
      }
      else
        out = navOut(secStr("ai_nav", "reply", "（模拟）今天有 3 个到期词，先完成每日复习，再做一次小测。"),
                     "answerable", "", "", 0.95);
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
