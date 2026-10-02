#include "skywalker/core/Json.h"

#include "skywalker/core/Strings.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace sky {

namespace {
const std::string kEmptyString;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;
constexpr int kMaxDepth = 256;  // agents send untrusted input; bound recursion.
}  // namespace

const Json& Json::null() {
    static const Json kNull;
    return kNull;
}

const char* Json::typeName(Type t) {
    switch (t) {
        case Type::Null: return "null";
        case Type::Bool: return "boolean";
        case Type::Number: return "number";
        case Type::String: return "string";
        case Type::Array: return "array";
        case Type::Object: return "object";
    }
    return "unknown";
}

const std::string& Json::asString() const { return isString() ? string_ : kEmptyString; }

const Json::Array& Json::elements() const { return isArray() ? array_ : kEmptyArray; }

Json::Array& Json::elements() {
    if (!isArray()) {
        *this = Json(Array{});
    }
    return array_;
}

void Json::push(Json value) { elements().push_back(std::move(value)); }

size_t Json::size() const {
    if (isArray()) return array_.size();
    if (isObject()) return object_.size();
    return 0;
}

const Json& Json::operator[](size_t index) const {
    if (!isArray() || index >= array_.size()) return null();
    return array_[index];
}

const Json::Object& Json::members() const { return isObject() ? object_ : kEmptyObject; }

Json::Object& Json::members() {
    if (!isObject()) {
        *this = Json(Object{});
    }
    return object_;
}

const Json* Json::find(std::string_view key) const {
    if (!isObject()) return nullptr;
    for (const auto& [k, v] : object_) {
        if (k == key) return &v;
    }
    return nullptr;
}

Json* Json::find(std::string_view key) {
    if (!isObject()) return nullptr;
    for (auto& [k, v] : object_) {
        if (k == key) return &v;
    }
    return nullptr;
}

const Json& Json::get(std::string_view key) const {
    const Json* v = find(key);
    return v ? *v : null();
}

Json& Json::operator[](std::string_view key) {
    auto& obj = members();
    for (auto& [k, v] : obj) {
        if (k == key) return v;
    }
    obj.emplace_back(std::string(key), Json());
    return obj.back().second;
}

bool Json::erase(std::string_view key) {
    if (!isObject()) return false;
    for (auto it = object_.begin(); it != object_.end(); ++it) {
        if (it->first == key) {
            object_.erase(it);
            return true;
        }
    }
    return false;
}

void Json::mergePatch(const Json& patch) {
    if (!patch.isObject()) {
        *this = patch;
        return;
    }
    if (!isObject()) *this = Json::object();
    for (const auto& [key, value] : patch.object_) {
        if (value.isNull()) {
            erase(key);
        } else {
            (*this)[key].mergePatch(value);
        }
    }
}

bool Json::operator==(const Json& other) const {
    if (type_ != other.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == other.bool_;
        case Type::Number: return number_ == other.number_;
        case Type::String: return string_ == other.string_;
        case Type::Array: return array_ == other.array_;
        case Type::Object: {
            // Key order does not affect equality.
            if (object_.size() != other.object_.size()) return false;
            for (const auto& [k, v] : object_) {
                const Json* o = other.find(k);
                if (!o || *o != v) return false;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

namespace {

void appendEscaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void appendNumber(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";  // JSON has no NaN/Inf
        return;
    }
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        auto res = std::to_chars(buf, buf + sizeof(buf), static_cast<int64_t>(v));
        out.append(buf, res.ptr);
        return;
    }
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, res.ptr);
}

void newline(std::string& out, int indent, int depth) {
    if (indent < 0) return;
    out.push_back('\n');
    out.append(static_cast<size_t>(indent * depth), ' ');
}

}  // namespace

void Json::dumpTo(std::string& out, int indent, int depth) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: appendNumber(out, number_); break;
        case Type::String: appendEscaped(out, string_); break;
        case Type::Array: {
            out.push_back('[');
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i) out.push_back(',');
                newline(out, indent, depth + 1);
                array_[i].dumpTo(out, indent, depth + 1);
            }
            if (!array_.empty()) newline(out, indent, depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            for (size_t i = 0; i < object_.size(); ++i) {
                if (i) out.push_back(',');
                newline(out, indent, depth + 1);
                appendEscaped(out, object_[i].first);
                out += indent >= 0 ? ": " : ":";
                object_[i].second.dumpTo(out, indent, depth + 1);
            }
            if (!object_.empty()) newline(out, indent, depth);
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Result<Json> run() {
        skipWs();
        Json value;
        if (!parseValue(value, 0)) return fail();
        skipWs();
        if (pos_ != text_.size()) {
            error_ = "unexpected trailing characters";
            return fail();
        }
        return value;
    }

private:
    Error fail() const {
        // Report line:column so agents can locate the problem in what they sent.
        size_t line = 1, col = 1;
        for (size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        return Error::make("parse_error",
                           "JSON " + error_ + " at line " + std::to_string(line) + ", column " + std::to_string(col));
    }

    void skipWs() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume(std::string_view lit) {
        if (text_.substr(pos_, lit.size()) == lit) {
            pos_ += lit.size();
            return true;
        }
        return false;
    }

    bool parseValue(Json& out, int depth) {
        if (depth > kMaxDepth) {
            error_ = "nesting too deep";
            return false;
        }
        if (pos_ >= text_.size()) {
            error_ = "unexpected end of input";
            return false;
        }
        char c = text_[pos_];
        switch (c) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string s;
                if (!parseString(s)) return false;
                out = Json(std::move(s));
                return true;
            }
            case 't':
                if (consume("true")) { out = Json(true); return true; }
                break;
            case 'f':
                if (consume("false")) { out = Json(false); return true; }
                break;
            case 'n':
                if (consume("null")) { out = Json(); return true; }
                break;
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
        }
        error_ = std::string("unexpected character '") + c + "'";
        return false;
    }

    bool parseNumber(Json& out) {
        size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        auto digits = [&] {
            size_t s = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            return pos_ > s;
        };
        if (!digits()) { error_ = "invalid number"; return false; }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (!digits()) { error_ = "invalid number fraction"; return false; }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (!digits()) { error_ = "invalid number exponent"; return false; }
        }
        double v = 0;
        if (!str::parseDouble(text_.substr(start, pos_ - start), v)) {
            error_ = "invalid number";
            return false;
        }
        out = Json(v);
        return true;
    }

    static void appendUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseHex4(uint32_t& out) {
        if (pos_ + 4 > text_.size()) { error_ = "truncated \\u escape"; return false; }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else { error_ = "invalid \\u escape"; return false; }
        }
        return true;
    }

    bool parseString(std::string& out) {
        ++pos_;  // opening quote
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) { error_ = "control character in string"; return false; }
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= text_.size()) break;
            char e = text_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
                        uint32_t lo = 0;
                        if (!consume("\\u") || !parseHex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
                            error_ = "invalid surrogate pair";
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: error_ = "invalid escape"; return false;
            }
        }
        error_ = "unterminated string";
        return false;
    }

    bool parseArray(Json& out, int depth) {
        ++pos_;
        Json::Array arr;
        skipWs();
        if (consume("]")) { out = Json(std::move(arr)); return true; }
        while (true) {
            skipWs();
            Json v;
            if (!parseValue(v, depth + 1)) return false;
            arr.push_back(std::move(v));
            skipWs();
            if (consume(",")) continue;
            if (consume("]")) break;
            error_ = "expected ',' or ']'";
            return false;
        }
        out = Json(std::move(arr));
        return true;
    }

    bool parseObject(Json& out, int depth) {
        ++pos_;
        Json obj = Json::object();
        skipWs();
        if (consume("}")) { out = std::move(obj); return true; }
        while (true) {
            skipWs();
            if (pos_ >= text_.size() || text_[pos_] != '"') { error_ = "expected string key"; return false; }
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (!consume(":")) { error_ = "expected ':'"; return false; }
            skipWs();
            Json v;
            if (!parseValue(v, depth + 1)) return false;
            obj[key] = std::move(v);  // duplicate keys: last wins
            skipWs();
            if (consume(",")) continue;
            if (consume("}")) break;
            error_ = "expected ',' or '}'";
            return false;
        }
        out = std::move(obj);
        return true;
    }

    std::string_view text_;
    size_t pos_ = 0;
    std::string error_;
};

}  // namespace

Result<Json> Json::parse(std::string_view text) { return Parser(text).run(); }

}  // namespace sky
