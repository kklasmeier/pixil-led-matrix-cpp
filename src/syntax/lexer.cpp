#include "syntax/lexer.h"

#include <cctype>
#include <charconv>
#include <cstdlib>

namespace pixil {

const char* tok_name(Tok t) {
    switch (t) {
        case Tok::End: return "end of expression";
        case Tok::Number: return "number";
        case Tok::String: return "string";
        case Tok::Ident: return "name";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBracket: return "'['";
        case Tok::RBracket: return "']'";
        case Tok::Comma: return "','";
        case Tok::Plus: return "'+'";
        case Tok::Minus: return "'-'";
        case Tok::Star: return "'*'";
        case Tok::Slash: return "'/'";
        case Tok::DSlash: return "'//'";
        case Tok::Percent: return "'%'";
        case Tok::DStar: return "'**'";
        case Tok::Amp: return "'&'";
        case Tok::EqEq: return "'=='";
        case Tok::NotEq: return "'!='";
        case Tok::Lt: return "'<'";
        case Tok::Le: return "'<='";
        case Tok::Gt: return "'>'";
        case Tok::Ge: return "'>='";
        case Tok::And: return "'and'";
        case Tok::Or: return "'or'";
        case Tok::Not: return "'not'";
    }
    return "?";
}

namespace {

[[noreturn]] void syntax_error(int line, int col, const std::string& msg) {
    throw PixilError(ErrorKind::Syntax,
                     "line " + std::to_string(line) + ", col " + std::to_string(col) + ": " + msg);
}

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

std::vector<Token> lex_expression(std::string_view src, int line) {
    std::vector<Token> out;
    size_t i = 0;
    const size_t n = src.size();
    auto push = [&](Tok k, size_t start, size_t len) {
        Token t;
        t.kind = k;
        t.text = std::string(src.substr(start, len));
        t.col = static_cast<int>(start) + 1;
        out.push_back(std::move(t));
    };
    while (i < n) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        const size_t start = i;
        const int col = static_cast<int>(start) + 1;
        if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(src[i + 1]))) {
            bool is_float = false;
            while (i < n && is_digit(src[i])) ++i;
            if (i < n && src[i] == '.') {
                is_float = true;
                ++i;
                while (i < n && is_digit(src[i])) ++i;
            }
            if (i < n && (src[i] == 'e' || src[i] == 'E')) {
                size_t j = i + 1;
                if (j < n && (src[j] == '+' || src[j] == '-')) ++j;
                if (j < n && is_digit(src[j])) {
                    is_float = true;
                    i = j;
                    while (i < n && is_digit(src[i])) ++i;
                }
            }
            if (i < n && is_ident_start(src[i])) syntax_error(line, col, "invalid number literal");
            std::string text(src.substr(start, i - start));
            Token t;
            t.kind = Tok::Number;
            t.text = text;
            t.col = col;
            if (is_float) {
                t.literal = Value::real(std::strtod(text.c_str(), nullptr));
            } else {
                int64_t v = 0;
                auto r = std::from_chars(text.data(), text.data() + text.size(), v);
                if (r.ec == std::errc()) {
                    t.literal = Value::integer(v);
                } else {
                    t.literal = Value::real(std::strtod(text.c_str(), nullptr));
                }
            }
            out.push_back(std::move(t));
            continue;
        }
        if (c == '"' || c == '\'') {
            const char q = c;
            ++i;
            std::string value;
            bool closed = false;
            while (i < n) {
                char d = src[i];
                if (d == q) {
                    closed = true;
                    ++i;
                    break;
                }
                if (d == '\\' && i + 1 < n) {
                    char e = src[i + 1];
                    switch (e) {
                        case 'n': value.push_back('\n'); break;
                        case 't': value.push_back('\t'); break;
                        case 'r': value.push_back('\r'); break;
                        case '\\': value.push_back('\\'); break;
                        case '\'': value.push_back('\''); break;
                        case '"': value.push_back('"'); break;
                        default:
                            value.push_back('\\');
                            value.push_back(e);
                            break;
                    }
                    i += 2;
                    continue;
                }
                value.push_back(d);
                ++i;
            }
            if (!closed) syntax_error(line, col, "unterminated string literal");
            Token t;
            t.kind = Tok::String;
            t.text = std::string(src.substr(start, i - start));
            t.col = col;
            t.literal = Value::str(std::move(value));
            out.push_back(std::move(t));
            continue;
        }
        if (is_ident_start(c)) {
            while (i < n && is_ident_char(src[i])) ++i;
            std::string_view word = src.substr(start, i - start);
            Tok k = Tok::Ident;
            if (word == "and") k = Tok::And;
            else if (word == "or") k = Tok::Or;
            else if (word == "not") k = Tok::Not;
            push(k, start, i - start);
            continue;
        }
        auto two = [&](char a, char b) { return c == a && i + 1 < n && src[i + 1] == b; };
        if (two('*', '*')) { push(Tok::DStar, start, 2); i += 2; continue; }
        if (two('/', '/')) { push(Tok::DSlash, start, 2); i += 2; continue; }
        if (two('=', '=')) { push(Tok::EqEq, start, 2); i += 2; continue; }
        if (two('!', '=')) { push(Tok::NotEq, start, 2); i += 2; continue; }
        if (two('<', '=')) { push(Tok::Le, start, 2); i += 2; continue; }
        if (two('>', '=')) { push(Tok::Ge, start, 2); i += 2; continue; }
        Tok single = Tok::End;
        switch (c) {
            case '(': single = Tok::LParen; break;
            case ')': single = Tok::RParen; break;
            case '[': single = Tok::LBracket; break;
            case ']': single = Tok::RBracket; break;
            case ',': single = Tok::Comma; break;
            case '+': single = Tok::Plus; break;
            case '-': single = Tok::Minus; break;
            case '*': single = Tok::Star; break;
            case '/': single = Tok::Slash; break;
            case '%': single = Tok::Percent; break;
            case '&': single = Tok::Amp; break;
            case '<': single = Tok::Lt; break;
            case '>': single = Tok::Gt; break;
            case '=':
                syntax_error(line, col, "'=' is assignment; use '==' to compare");
            default:
                syntax_error(line, col, std::string("unexpected character '") + c + "'");
        }
        push(single, start, 1);
        ++i;
    }
    Token end;
    end.kind = Tok::End;
    end.col = static_cast<int>(n) + 1;
    out.push_back(std::move(end));
    return out;
}

}  // namespace pixil
