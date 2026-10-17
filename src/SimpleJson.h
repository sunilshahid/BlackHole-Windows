#pragma once
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace BlackHole {

enum class JsonType {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
};

class JsonValue {
public:
    JsonType type = JsonType::Null;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<JsonValue> arrVal;
    std::map<std::string, JsonValue> objVal;

    JsonValue() : type(JsonType::Null) {}
    JsonValue(bool b) : type(JsonType::Boolean), boolVal(b) {}
    JsonValue(int n) : type(JsonType::Number), numVal(n) {}
    JsonValue(int64_t n) : type(JsonType::Number), numVal(static_cast<double>(n)) {}
    JsonValue(double n) : type(JsonType::Number), numVal(n) {}
    JsonValue(const char* s) : type(JsonType::String), strVal(s ? s : "") {}
    JsonValue(const std::string& s) : type(JsonType::String), strVal(s) {}

    static JsonValue Array() {
        JsonValue v;
        v.type = JsonType::Array;
        return v;
    }

    static JsonValue Object() {
        JsonValue v;
        v.type = JsonType::Object;
        return v;
    }

    void set(const std::string& key, const JsonValue& val) {
        type = JsonType::Object;
        objVal[key] = val;
    }

    void push(const JsonValue& val) {
        type = JsonType::Array;
        arrVal.push_back(val);
    }

    bool has(const std::string& key) const {
        if (type != JsonType::Object) return false;
        return objVal.find(key) != objVal.end();
    }

    const JsonValue& get(const std::string& key) const {
        static JsonValue nullVal;
        if (type != JsonType::Object) return nullVal;
        auto it = objVal.find(key);
        if (it != objVal.end()) return it->second;
        return nullVal;
    }

    std::string asString(const std::string& defaultVal = "") const {
        if (type == JsonType::String) return strVal;
        return defaultVal;
    }

    int asInt(int defaultVal = 0) const {
        if (type == JsonType::Number) return static_cast<int>(numVal);
        return defaultVal;
    }

    int64_t asInt64(int64_t defaultVal = 0) const {
        if (type == JsonType::Number) return static_cast<int64_t>(numVal);
        return defaultVal;
    }

    bool asBool(bool defaultVal = false) const {
        if (type == JsonType::Boolean) return boolVal;
        return defaultVal;
    }

    std::string serialize() const {
        std::ostringstream ss;
        switch (type) {
            case JsonType::Null: ss << "null"; break;
            case JsonType::Boolean: ss << (boolVal ? "true" : "false"); break;
            case JsonType::Number: {
                if (numVal == static_cast<int64_t>(numVal)) {
                    ss << static_cast<int64_t>(numVal);
                } else {
                    ss << numVal;
                }
                break;
            }
            case JsonType::String: {
                ss << "\"";
                for (char c : strVal) {
                    if (c == '"') ss << "\\\"";
                    else if (c == '\\') ss << "\\\\";
                    else if (c == '\b') ss << "\\b";
                    else if (c == '\f') ss << "\\f";
                    else if (c == '\n') ss << "\\n";
                    else if (c == '\r') ss << "\\r";
                    else if (c == '\t') ss << "\\t";
                    else ss << c;
                }
                ss << "\"";
                break;
            }
            case JsonType::Array: {
                ss << "[";
                for (size_t i = 0; i < arrVal.size(); ++i) {
                    if (i > 0) ss << ",";
                    ss << arrVal[i].serialize();
                }
                ss << "]";
                break;
            }
            case JsonType::Object: {
                ss << "{";
                size_t i = 0;
                for (auto const& [k, v] : objVal) {
                    if (i > 0) ss << ",";
                    ss << "\"" << k << "\":" << v.serialize();
                    i++;
                }
                ss << "}";
                break;
            }
        }
        return ss.str();
    }
};

class JsonParser {
    std::string src;
    size_t pos = 0;

    void skipWhitespace() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r' || src[pos] == '\n')) {
            pos++;
        }
    }

    char peek() {
        skipWhitespace();
        if (pos >= src.size()) return '\0';
        return src[pos];
    }

    char get() {
        skipWhitespace();
        if (pos >= src.size()) return '\0';
        return src[pos++];
    }

public:
    static JsonValue parse(const std::string& json) {
        JsonParser p;
        p.src = json;
        p.pos = 0;
        return p.parseValue();
    }

private:
    JsonValue parseValue() {
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNumber();
        return JsonValue();
    }

    JsonValue parseObject() {
        JsonValue obj = JsonValue::Object();
        get(); // consume '{'
        while (true) {
            char c = peek();
            if (c == '}' || c == '\0') {
                if (c == '}') get();
                break;
            }
            if (c == ',') {
                get();
                continue;
            }
            JsonValue keyVal = parseString();
            std::string key = keyVal.strVal;
            skipWhitespace();
            if (peek() == ':') get();
            JsonValue val = parseValue();
            obj.set(key, val);
        }
        return obj;
    }

    JsonValue parseArray() {
        JsonValue arr = JsonValue::Array();
        get(); // consume '['
        while (true) {
            char c = peek();
            if (c == ']' || c == '\0') {
                if (c == ']') get();
                break;
            }
            if (c == ',') {
                get();
                continue;
            }
            arr.push(parseValue());
        }
        return arr;
    }

    JsonValue parseString() {
        get(); // consume '"'
        std::string s;
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') break;
            if (c == '\\' && pos < src.size()) {
                char esc = src[pos++];
                if (esc == '"') s += '"';
                else if (esc == '\\') s += '\\';
                else if (esc == 'n') s += '\n';
                else if (esc == 'r') s += '\r';
                else if (esc == 't') s += '\t';
                else s += esc;
            } else {
                s += c;
            }
        }
        return JsonValue(s);
    }

    JsonValue parseNumber() {
        size_t start = pos;
        if (src[pos] == '-') pos++;
        while (pos < src.size() && (std::isdigit(static_cast<unsigned char>(src[pos])) || src[pos] == '.')) {
            pos++;
        }
        std::string numStr = src.substr(start, pos - start);
        double val = 0.0;
        try {
            val = std::stod(numStr);
        } catch (...) {}
        return JsonValue(val);
    }

    JsonValue parseBool() {
        if (src.substr(pos, 4) == "true") {
            pos += 4;
            return JsonValue(true);
        }
        if (src.substr(pos, 5) == "false") {
            pos += 5;
            return JsonValue(false);
        }
        return JsonValue(false);
    }

    JsonValue parseNull() {
        if (src.substr(pos, 4) == "null") {
            pos += 4;
        }
        return JsonValue();
    }
};

} // namespace BlackHole
