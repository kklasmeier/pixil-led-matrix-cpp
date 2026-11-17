#include "util/json.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace pixil::json {

const Node* Node::get(const std::string& key) const {
    for (const auto& [k, v] : fields) {
        if (k == key) return &v;
    }
    return nullptr;
}

namespace {

class Reader {
public:
    explicit Reader(std::string_view t) : t_(t) {}

    Node document() {
        Node n = value();
        ws();
        if (p_ != t_.size()) fail("trailing characters");
        return n;
    }

private:
    [[noreturn]] void fail(const char* msg) const {
        throw std::runtime_error(std::string("json: ") + msg + " at offset " + std::to_string(p_));
    }
    void ws() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\n' || t_[p_] == '\r' || t_[p_] == '\t')) ++p_;
    }
    bool lit(std::string_view w) {
        if (t_.substr(p_, w.size()) == w) {
            p_ += w.size();
            return true;
        }
        return false;
    }
    static void put_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    uint32_t hex4() {
        if (p_ + 4 > t_.size()) fail("bad \\u escape");
        uint32_t v = 0;
        auto r = std::from_chars(t_.data() + p_, t_.data() + p_ + 4, v, 16);
        if (r.ptr != t_.data() + p_ + 4) fail("bad \\u escape");
        p_ += 4;
        return v;
    }
    std::string string() {
        if (t_[p_] != '"') fail("expected string");
        ++p_;
        std::string out;
        while (p_ < t_.size()) {
            char c = t_[p_++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p_ >= t_.size()) fail("bad escape");
            char e = t_[p_++];
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
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && lit("\\u")) {
                        uint32_t lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
        fail("unterminated string");
    }
    Node value() {
        ws();
        if (p_ >= t_.size()) fail("unexpected end");
        Node n;
        char c = t_[p_];
        if (c == '{') {
            ++p_;
            n.type = Node::Type::Object;
            ws();
            if (p_ < t_.size() && t_[p_] == '}') {
                ++p_;
                return n;
            }
            for (;;) {
                ws();
                std::string k = string();
                ws();
                if (p_ >= t_.size() || t_[p_] != ':') fail("expected ':'");
                ++p_;
                n.fields.emplace_back(std::move(k), value());
                ws();
                if (p_ < t_.size() && t_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < t_.size() && t_[p_] == '}') {
                    ++p_;
                    return n;
                }
                fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p_;
            n.type = Node::Type::Array;
            ws();
            if (p_ < t_.size() && t_[p_] == ']') {
                ++p_;
                return n;
            }
            for (;;) {
                n.items.push_back(value());
                ws();
                if (p_ < t_.size() && t_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < t_.size() && t_[p_] == ']') {
                    ++p_;
                    return n;
                }
                fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            n.type = Node::Type::String;
            n.s = string();
            return n;
        }
        if (lit("true")) {
            n.type = Node::Type::Bool;
            n.b = true;
            return n;
        }
        if (lit("false")) {
            n.type = Node::Type::Bool;
            return n;
        }
        if (lit("null")) return n;
        size_t start = p_;
        bool is_float = false;
        if (t_[p_] == '-') ++p_;
        while (p_ < t_.size()) {
            char d = t_[p_];
            if (d >= '0' && d <= '9') {
                ++p_;
            } else if (d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') {
                is_float = true;
                ++p_;
            } else {
                break;
            }
        }
        if (p_ == start) fail("unexpected character");
        std::string num(t_.substr(start, p_ - start));
        if (is_float) {
            n.type = Node::Type::Float;
            n.d = std::strtod(num.c_str(), nullptr);
        } else {
            n.type = Node::Type::Int;
            auto r = std::from_chars(num.data(), num.data() + num.size(), n.i);
            if (r.ec != std::errc()) {
                n.type = Node::Type::Float;
                n.d = std::strtod(num.c_str(), nullptr);
            }
        }
        return n;
    }

    std::string_view t_;
    size_t p_ = 0;
};

}  // namespace

Node parse(std::string_view text) { return Reader(text).document(); }

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace pixil::json
