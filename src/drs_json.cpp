#include "drs_json.h"

#include <cstdio>

namespace acdb {
namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {
        if (s_.size() >= 3 && s_.compare(0, 3, "\xEF\xBB\xBF") == 0) pos_ = 3;
    }

    bool Run(JsonValue* out, std::string* error) {
        SkipSpace();
        if (!Value(out, 0)) {
            *error = error_;
            return false;
        }
        SkipSpace();
        if (pos_ != s_.size()) {
            *error = At("text after the value");
            return false;
        }
        return true;
    }

private:
    std::string At(const char* why) const {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s at byte %zu", why, pos_);
        return buf;
    }

    bool Fail(const char* why) {
        if (error_.empty()) error_ = At(why);
        return false;
    }

    void SkipSpace() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n'))
            ++pos_;
    }

    bool Literal(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (s_.compare(pos_, n, word) != 0) return Fail("unknown word");
        pos_ += n;
        return true;
    }

    bool Value(JsonValue* v, int depth) {
        if (depth > kMaxDepth) return Fail("nested too deeply");
        if (pos_ >= s_.size()) return Fail("missing value");
        const char c = s_[pos_];
        if (c == '{') return Object(v, depth);
        if (c == '[') return Array(v, depth);
        if (c == '"') {
            v->type = JsonValue::Type::String;
            return String(&v->text);
        }
        if (c == 't' || c == 'f') {
            v->type = JsonValue::Type::Bool;
            v->boolean = c == 't';
            return Literal(c == 't' ? "true" : "false");
        }
        if (c == 'n') {
            v->type = JsonValue::Type::Null;
            return Literal("null");
        }
        if (c == '-' || (c >= '0' && c <= '9')) return Number(v);
        return Fail("unexpected character");
    }

    bool Object(JsonValue* v, int depth) {
        v->type = JsonValue::Type::Object;
        ++pos_;  // {
        SkipSpace();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("expected a member name");
            std::string key;
            if (!String(&key)) return false;
            SkipSpace();
            if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("expected ':'");
            ++pos_;
            SkipSpace();
            JsonValue member;
            if (!Value(&member, depth + 1)) return false;
            v->members.emplace_back(std::move(key), std::move(member));
            SkipSpace();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool Array(JsonValue* v, int depth) {
        v->type = JsonValue::Type::Array;
        ++pos_;  // [
        SkipSpace();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipSpace();
            JsonValue item;
            if (!Value(&item, depth + 1)) return false;
            v->items.push_back(std::move(item));
            SkipSpace();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    // JSON's number grammar; the value keeps its text.
    bool Number(JsonValue* v) {
        const size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        auto digits = [this] {
            const size_t from = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            return pos_ - from;
        };
        if (pos_ < s_.size() && s_[pos_] == '0') {
            ++pos_;
            if (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') return Fail("leading zero");
        } else if (digits() == 0) {
            return Fail("expected a digit");
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (digits() == 0) return Fail("expected a digit after '.'");
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (digits() == 0) return Fail("expected a digit in the exponent");
        }
        v->type = JsonValue::Type::Number;
        v->text = s_.substr(start, pos_ - start);
        return true;
    }

    bool Hex4(uint32_t* out) {
        if (pos_ + 4 > s_.size()) return Fail("short \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return Fail("bad \\u escape");
        }
        *out = v;
        return true;
    }

    static void AppendUtf8(uint32_t cp, std::string* out) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool String(std::string* out) {
        ++pos_;  // "
        for (;;) {
            if (pos_ >= s_.size()) return Fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return Fail("control character in a string");
            if (c != '\\') {
                out->push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= s_.size()) return Fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!Hex4(&cp)) return false;
                    if (cp >= 0xDC00 && cp <= 0xDFFF) return Fail("lone low surrogate");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t low = 0;
                        if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u')
                            return Fail("lone high surrogate");
                        pos_ += 2;
                        if (!Hex4(&low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return Fail("lone high surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    AppendUtf8(cp, out);
                    break;
                }
                default: return Fail("unknown escape");
            }
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
    std::string error_;
};

}  // namespace

const JsonValue* JsonValue::Find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& [name, value] : members) {
        if (name == key) return &value;
    }
    return nullptr;
}

bool ParseJson(const std::string& utf8, JsonValue* out, std::string* error) {
    *out = JsonValue();
    std::string err;
    Parser parser(utf8);
    if (parser.Run(out, &err)) return true;
    *out = JsonValue();
    if (error) *error = err;
    return false;
}

bool JsonUint32(const JsonValue& v, uint32_t* out) {
    if (v.type != JsonValue::Type::Number || v.text.empty() || v.text.size() > 10) return false;
    uint64_t n = 0;
    for (const char c : v.text) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<uint64_t>(c - '0');
    }
    if (n > 0xFFFFFFFFull) return false;
    *out = static_cast<uint32_t>(n);
    return true;
}

std::string JsonQuote(const std::string& utf8) {
    std::string q = "\"";
    for (const char ch : utf8) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': q += "\\\""; break;
            case '\\': q += "\\\\"; break;
            case '\n': q += "\\n"; break;
            case '\r': q += "\\r"; break;
            case '\t': q += "\\t"; break;
            case '\b': q += "\\b"; break;
            case '\f': q += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(c));
                    q += buf;
                } else {
                    q.push_back(ch);
                }
        }
    }
    return q + "\"";
}

}  // namespace acdb
