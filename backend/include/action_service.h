// 动作词典（v1.5.1-T005 HOME-NAV）
// 职责：把建议文本映射到站内现有页面；周报锚点与 AI 导航共用
// 配置：config/action_rules.json（人工可改，重启后生效）
#pragma once

#include "json.h"

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace eng {

class ActionService
{
public:
  explicit ActionService(const std::string& path = "config/action_rules.json")
  {
    load(path);
  }

  bool loaded() const { return !rules_.empty(); }

  // 按文本命中动作；未命中返回 Null
  // page：当前页面路径（如 /pages/review.html），用于命中 pageRoutes 里的本页路由
  JsonValue match(const std::string& text, const std::string& page = "") const
  {
    int bestScore = 0;
    const Rule* best = nullptr;
    for (const auto& r : rules_)
    {
      if (blocked(r, text)) continue;
      int score = 0;
      for (const auto& kw : r.keywords)
      {
        if (contains(text, kw.word)) score += kw.weight;
      }
      if (score > bestScore)
      {
        bestScore = score;
        best = &r;
      }
    }
    if (!best || bestScore < threshold_) return JsonValue();
    return toJson(*best, text, page);
  }

  // 按 id 取动作，用于兜底动作；未配置返回 Null
  JsonValue byId(const std::string& id, const std::string& page = "") const
  {
    for (const auto& r : rules_)
    {
      if (r.id == id) return toJson(r, "", page);
    }
    return JsonValue();
  }

  bool allowedRoute(const std::string& route) const
  {
    for (const auto& r : rules_)
    {
      if (route == r.route || route.rfind(r.route + "?", 0) == 0 ||
          route.rfind(r.route + "&", 0) == 0)
      {
        return true;
      }
    }
    return false;
  }

private:
  struct Keyword
  {
    std::string word;
    int weight = 0;
  };
  struct Param
  {
    std::string match;
    std::string query;
  };
  struct Rule
  {
    std::string id;
    std::string label;
    std::string route;
    std::vector<Keyword> keywords;
    std::vector<std::string> forbid;
    std::vector<Param> params;
    std::map<std::string, std::string> pageRoutes;   // 页面路径 → 本页专属路由
  };

  std::vector<Rule> rules_;
  int threshold_ = 2;

  static bool contains(const std::string& text, const std::string& needle)
  {
    return !needle.empty() && text.find(needle) != std::string::npos;
  }

  static bool blocked(const Rule& r, const std::string& text)
  {
    for (const auto& f : r.forbid)
    {
      if (contains(text, f)) return true;
    }
    return false;
  }

  static JsonValue toJson(const Rule& r, const std::string& text, const std::string& page = "")
  {
    std::string route = r.route;
    auto hit = r.pageRoutes.find(page);
    if (hit != r.pageRoutes.end()) route = hit->second;
    std::string sep = route.find('?') == std::string::npos ? "?" : "&";
    for (const auto& p : r.params)
    {
      if (!contains(text, p.match)) continue;
      route += sep + p.query;
      sep = "&";
    }
    JsonObject o;
    o["id"] = JsonValue::makeString(r.id);
    o["label"] = JsonValue::makeString(r.label);
    o["route"] = JsonValue::makeString(route);
    return JsonValue::makeObject(o);
  }

  void load(const std::string& path)
  {
    std::ifstream f(path);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    JsonValue root;
    try { root = parseJson(ss.str()); }
    catch (...) { return; }

    threshold_ = (int)root.getInt("threshold", 2);
    if (threshold_ < 1) threshold_ = 1;

    const JsonValue* arr = root.find("actions");
    if (!arr || !arr->isArray()) return;

    for (const auto& item : arr->asArray())
    {
      if (!item.isObject()) continue;
      Rule r;
      r.id = item.getString("id");
      r.label = item.getString("label");
      r.route = item.getString("route");
      if (r.id.empty() || r.label.empty() || r.route.empty()) continue;

      const JsonValue* kws = item.find("keywords");
      if (kws && kws->isObject())
      {
        for (const auto& kv : kws->asObject())
        {
          if (!kv.second.isNull())
          {
            Keyword k;
            k.word = kv.first;
            k.weight = (int)kv.second.asInt();
            if (k.weight > 0) r.keywords.push_back(k);
          }
        }
      }

      const JsonValue* fb = item.find("forbid");
      if (fb && fb->isArray())
      {
        for (const auto& v : fb->asArray())
        {
          if (v.isString() && !v.asString().empty()) r.forbid.push_back(v.asString());
        }
      }

      const JsonValue* ps = item.find("params");
      if (ps && ps->isArray())
      {
        for (const auto& v : ps->asArray())
        {
          Param p;
          p.match = v.getString("match");
          p.query = v.getString("query");
          if (!p.match.empty() && !p.query.empty()) r.params.push_back(p);
        }
      }

      // pageRoutes：当前页命中时改用本页路由（如各页导览 ?tour=1）
      const JsonValue* pr = item.find("pageRoutes");
      if (pr && pr->isObject())
      {
        for (const auto& kv : pr->asObject())
        {
          if (kv.second.isString() && !kv.second.asString().empty())
            r.pageRoutes[kv.first] = kv.second.asString();
        }
      }

      rules_.push_back(r);
    }
  }
};

} // namespace eng
