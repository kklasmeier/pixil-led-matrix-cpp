// Expression tokenizer. Runs once per expression at script load; never on a hot path.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "runtime/value.h"

namespace pixil {

enum class Tok {
    End,
    Number,
    String,
    Ident,
    LParen,
    RParen,
    LBracket,
    RBracket,
    Comma,
    Plus,
    Minus,
    Star,
    Slash,
    DSlash,
    Percent,
    DStar,
    Amp,
    EqEq,
    NotEq,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
    Not,
};

struct Token {
    Tok kind = Tok::End;
    std::string text;
    int col = 0;    // 1-based column within the source text
    Value literal;  // Number / String tokens
};

const char* tok_name(Tok t);

// Throws PixilError(Syntax) with "line:col" context.
std::vector<Token> lex_expression(std::string_view src, int line);

}  // namespace pixil
