// Minimal JSON value + serializer/parser (subset, no external deps)
#pragma once
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <memory>
#include <sstream>
#include <cstdint>

namespace eng {

class JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray  = std::vector<JsonValue>;

class JsonValue {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };
private:
    Type type_ = Type::Null;
    bool        b_{false};
    int64_t     i_{0};
    double      d_{0.0};
    std::string s_{};
    JsonArray   a_{};
    JsonObject  o_{};
public:
    JsonValue() = default;
    static JsonValue makeNull() { JsonValue v; v.type_=Type::Null; return v; }
    static JsonValue makeBool(bool b) { JsonValue v; v.type_=Type::Bool; v.b_=b; return v; }
    static JsonValue makeInt(int64_t i) { JsonValue v; v.type_=Type::Int; v.i_=i; return v; }
    static JsonValue makeDouble(double d) { JsonValue v; v.type_=Type::Double; v.d_=d; return v; }
    static JsonValue makeString(std::string s) { JsonValue v; v.type_=Type::String; v.s_=std::move(s); return v; }
    static JsonValue makeArray(JsonArray a) { JsonValue v; v.type_=Type::Array; v.a_=std::move(a); return v; }
    static JsonValue makeObject(JsonObject o) { JsonValue v; v.type_=Type::Object; v.o_=std::move(o); return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_==Type::Null; }
    bool isObject() const { return type_==Type::Object; }
    bool isArray() const { return type_==Type::Array; }
    bool isString() const { return type_==Type::String; }

    const JsonObject& asObject() const { return o_; }
    const JsonArray&  asArray()  const { return a_; }
    const std::string& asString() const { return s_; }
    int64_t asInt() const { return type_==Type::Int ? i_ : (int64_t)d_; }
    double  asDouble() const { return type_==Type::Double ? d_ : (double)i_; }
    bool    asBool() const { return b_; }

    const JsonValue* find(const std::string& key) const {
        if (type_ != Type::Object) return nullptr;
        auto it = o_.find(key);
        return it == o_.end() ? nullptr : &it->second;
    }
    std::string getString(const std::string& key, const std::string& def={}) const {
        auto* v = find(key);
        return (v && v->isString()) ? v->asString() : def;
    }
    int64_t getInt(const std::string& key, int64_t def=0) const {
        auto* v = find(key);
        return v ? v->asInt() : def;
    }
    bool getBool(const std::string& key, bool def=false) const {
        auto* v = find(key);
        return v ? v->asBool() : def;
    }

    static std::string escapeString(const std::string& s) {
        std::string out; out.reserve(s.size()+8);
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char buf[8]; snprintf(buf,sizeof(buf),"\\u%04x",c); out += buf;
                    } else out += c;
            }
        }
        return out;
    }

    std::string dump(int indent=-1, int level=0) const {
        std::ostringstream os;
        switch (type_) {
            case Type::Null:    os << "null"; break;
            case Type::Bool:   os << (b_ ? "true" : "false"); break;
            case Type::Int:    os << i_; break;
            case Type::Double: os << d_; break;
            case Type::String: os << "\"" << escapeString(s_) << "\""; break;
            case Type::Array: {
                os << "[";
                for (size_t k=0;k<a_.size();++k) {
                    if (k) os << ",";
                    if (indent>=0) os << "\n" << std::string((level+1)*indent,' ');
                    os << a_[k].dump(indent, level+1);
                }
                if (indent>=0 && !a_.empty()) os << "\n" << std::string(level*indent,' ');
                os << "]";
                break;
            }
            case Type::Object: {
                os << "{";
                bool first=true;
                for (const auto& kv : o_) {
                    if (!first) os << ",";
                    first=false;
                    if (indent>=0) os << "\n" << std::string((level+1)*indent,' ');
                    os << "\"" << escapeString(kv.first) << "\":";
                    if (indent>=0) os << " ";
                    os << kv.second.dump(indent, level+1);
                }
                if (indent>=0 && !o_.empty()) os << "\n" << std::string(level*indent,' ');
                os << "}";
                break;
            }
        }
        return os.str();
    }
};

// 极简 JSON 解析器（支持 Object/Array/String/Number/Bool/Null，无注释/无尾随逗号严格）
class JsonParser {
    const std::string& src;
    size_t pos=0;
    void skipWs() { while (pos<src.size() && (src[pos]==' '||src[pos]=='\t'||src[pos]=='\n'||src[pos]=='\r')) ++pos; }
    char peek() { skipWs(); return pos<src.size() ? src[pos] : '\0'; }
    char get() { skipWs(); return pos<src.size() ? src[pos++] : '\0'; }
    JsonValue parseValue() {
        char c = peek();
        if (c=='"') return parseString();
        if (c=='{') return parseObject();
        if (c=='[') return parseArray();
        if (c=='t'||c=='f') return parseBool();
        if (c=='n') return parseNull();
        return parseNumber();
    }
    JsonValue parseString() {
        if (get() != '"') throw std::runtime_error("expected '\"'");
        std::string s;
        while (pos<src.size()) {
            char c = src[pos++];
            if (c=='"') return JsonValue::makeString(s);
            if (c=='\\') {
                if (pos>=src.size()) break;
                char e = src[pos++];
                switch (e) {
                    case '"': s+='"'; break;
                    case '\\': s+='\\'; break;
                    case '/': s+='/'; break;
                    case 'b': s+='\b'; break;
                    case 'f': s+='\f'; break;
                    case 'n': s+='\n'; break;
                    case 'r': s+='\r'; break;
                    case 't': s+='\t'; break;
                    case 'u': {
                        if (pos+4 > src.size()) throw std::runtime_error("bad unicode");
                        s += '?'; pos += 4; // 简化处理：跳过 4 位 hex（足够覆盖本项目 ASCII 场景）
                        break;
                    }
                    default: s += e;
                }
            } else s += c;
        }
        throw std::runtime_error("unterminated string");
    }
    JsonValue parseNumber() {
        size_t start=pos;
        if (src[pos]=='-'||src[pos]=='+') ++pos;
        while (pos<src.size() && ((src[pos]>='0'&&src[pos]<='9')||src[pos]=='.'||src[pos]=='e'||src[pos]=='E'||src[pos]=='+'||src[pos]=='-')) ++pos;
        std::string num = src.substr(start, pos-start);
        if (num.find('.')!=std::string::npos || num.find('e')!=std::string::npos || num.find('E')!=std::string::npos) {
            return JsonValue::makeDouble(std::stod(num));
        }
        try { return JsonValue::makeInt(std::stoll(num)); }
        catch(...) { return JsonValue::makeDouble(std::stod(num)); }
    }
    JsonValue parseBool() {
        if (src.compare(pos,4,"true")==0) { pos+=4; return JsonValue::makeBool(true); }
        if (src.compare(pos,5,"false")==0) { pos+=5; return JsonValue::makeBool(false); }
        throw std::runtime_error("bad bool");
    }
    JsonValue parseNull() {
        if (src.compare(pos,4,"null")==0) { pos+=4; return JsonValue::makeNull(); }
        throw std::runtime_error("bad null");
    }
    JsonValue parseArray() {
        if (get() != '[') throw std::runtime_error("expected '['");
        JsonArray a;
        if (peek()==']') { get(); return JsonValue::makeArray(std::move(a)); }
        while (true) {
            a.push_back(parseValue());
            char c = get();
            if (c==']') break;
            if (c!=',') throw std::runtime_error("expected ',' or ']'");
        }
        return JsonValue::makeArray(std::move(a));
    }
    JsonValue parseObject() {
        if (get() != '{') throw std::runtime_error("expected '{'");
        JsonObject o;
        if (peek()=='}') { get(); return JsonValue::makeObject(std::move(o)); }
        while (true) {
            // key
            std::string key;
            char c = get();
            if (c != '"') throw std::runtime_error("expected string key");
            while (pos<src.size()) {
                char ch = src[pos++];
                if (ch=='"') break;
                if (ch=='\\') { if (pos<src.size()) key += src[pos++]; }
                else key += ch;
            }
            if (get() != ':') throw std::runtime_error("expected ':'");
            o[std::move(key)] = parseValue();
            c = get();
            if (c=='}') break;
            if (c!=',') throw std::runtime_error("expected ',' or '}'");
        }
        return JsonValue::makeObject(std::move(o));
    }
public:
    JsonParser(const std::string& s) : src(s) {}
    JsonValue parse() { skipWs(); JsonValue v = parseValue(); return v; }
};

inline JsonValue parseJson(const std::string& s) {
    JsonParser p(s);
    return p.parse();
}

} // namespace eng
