#include "syntax/parser.h"

#include "syntax/lexer.h"

namespace pixil {

namespace {

class Parser {
public:
    Parser(std::vector<Token> toks, int line) : toks_(std::move(toks)), line_(line) {}

    ExprPtr parse_all() {
        ExprPtr e = or_expr();
        if (peek().kind != Tok::End) fail(std::string("unexpected ") + describe(peek()));
        return e;
    }

private:
    const Token& peek() const { return toks_[pos_]; }
    Token take() { return toks_[pos_++]; }
    bool accept(Tok k) {
        if (peek().kind != k) return false;
        ++pos_;
        return true;
    }
    void expect(Tok k) {
        if (!accept(k)) {
            fail(std::string("expected ") + tok_name(k) + ", found " + describe(peek()));
        }
    }
    static std::string describe(const Token& t) {
        if (t.kind == Tok::End) return tok_name(t.kind);
        return "'" + t.text + "'";
    }
    [[noreturn]] void fail(const std::string& msg) const {
        throw PixilError(ErrorKind::Syntax, "line " + std::to_string(line_) + ", col " +
                                                std::to_string(peek().col) + ": " + msg);
    }

    ExprPtr node(ExprKind k, int col) const {
        auto e = std::make_unique<Expr>();
        e->kind = k;
        e->line = line_;
        e->col = col;
        return e;
    }
    ExprPtr two(ExprKind k, int col, ExprPtr a, ExprPtr b) const {
        auto e = node(k, col);
        e->args.push_back(std::move(a));
        e->args.push_back(std::move(b));
        return e;
    }

    ExprPtr or_expr() {
        ExprPtr left = and_expr();
        while (peek().kind == Tok::Or) {
            int col = take().col;
            left = two(ExprKind::Or, col, std::move(left), and_expr());
        }
        return left;
    }

    ExprPtr and_expr() {
        ExprPtr left = not_expr();
        while (peek().kind == Tok::And) {
            int col = take().col;
            left = two(ExprKind::And, col, std::move(left), not_expr());
        }
        return left;
    }

    ExprPtr not_expr() {
        if (peek().kind == Tok::Not) {
            int col = take().col;
            auto e = node(ExprKind::Not, col);
            e->args.push_back(not_expr());
            return e;
        }
        return comparison();
    }

    static bool cmp_of(Tok t, CmpOp* op) {
        switch (t) {
            case Tok::EqEq: *op = CmpOp::Eq; return true;
            case Tok::NotEq: *op = CmpOp::Ne; return true;
            case Tok::Lt: *op = CmpOp::Lt; return true;
            case Tok::Le: *op = CmpOp::Le; return true;
            case Tok::Gt: *op = CmpOp::Gt; return true;
            case Tok::Ge: *op = CmpOp::Ge; return true;
            default: return false;
        }
    }

    ExprPtr comparison() {
        ExprPtr first = concat();
        CmpOp op = CmpOp::Eq;
        if (!cmp_of(peek().kind, &op)) return first;
        auto e = node(ExprKind::Compare, peek().col);
        e->args.push_back(std::move(first));
        while (cmp_of(peek().kind, &op)) {
            take();
            e->cmp_ops.push_back(op);
            e->args.push_back(concat());
        }
        return e;
    }

    ExprPtr binary(BinOp op, int col, ExprPtr a, ExprPtr b) const {
        auto e = two(ExprKind::Binary, col, std::move(a), std::move(b));
        e->binop = op;
        return e;
    }

    ExprPtr concat() {
        ExprPtr left = arith();
        while (peek().kind == Tok::Amp) {
            int col = take().col;
            left = binary(BinOp::Concat, col, std::move(left), arith());
        }
        return left;
    }

    ExprPtr arith() {
        ExprPtr left = term();
        while (peek().kind == Tok::Plus || peek().kind == Tok::Minus) {
            Token t = take();
            left = binary(t.kind == Tok::Plus ? BinOp::Add : BinOp::Sub, t.col, std::move(left),
                          term());
        }
        return left;
    }

    ExprPtr term() {
        ExprPtr left = factor();
        for (;;) {
            BinOp op = BinOp::Mul;
            switch (peek().kind) {
                case Tok::Star: op = BinOp::Mul; break;
                case Tok::Slash: op = BinOp::TrueDiv; break;
                case Tok::DSlash: op = BinOp::FloorDiv; break;
                case Tok::Percent: op = BinOp::Mod; break;
                default: return left;
            }
            int col = take().col;
            left = binary(op, col, std::move(left), factor());
        }
    }

    ExprPtr factor() {
        if (peek().kind == Tok::Minus || peek().kind == Tok::Plus) {
            Token t = take();
            auto e = node(t.kind == Tok::Minus ? ExprKind::Neg : ExprKind::Pos, t.col);
            e->args.push_back(factor());
            return e;
        }
        return power();
    }

    ExprPtr power() {
        ExprPtr base = postfix();
        if (peek().kind == Tok::DStar) {
            int col = take().col;
            return binary(BinOp::Pow, col, std::move(base), factor());
        }
        return base;
    }

    ExprPtr postfix() {
        ExprPtr e = atom();
        for (;;) {
            if (peek().kind == Tok::LBracket) {
                int col = take().col;
                ExprPtr idx = or_expr();
                expect(Tok::RBracket);
                e = two(ExprKind::Index, col, std::move(e), std::move(idx));
            } else if (peek().kind == Tok::LParen) {
                if (e->kind != ExprKind::Name) fail("only named functions can be called");
                take();
                auto call = node(ExprKind::Call, e->col);
                call->name = e->name;
                if (peek().kind != Tok::RParen) {
                    call->args.push_back(or_expr());
                    while (accept(Tok::Comma)) {
                        if (peek().kind == Tok::RParen) break;
                        call->args.push_back(or_expr());
                    }
                }
                expect(Tok::RParen);
                e = std::move(call);
            } else {
                return e;
            }
        }
    }

    ExprPtr atom() {
        const Token& t = peek();
        switch (t.kind) {
            case Tok::Number:
            case Tok::String: {
                auto e = node(ExprKind::Literal, t.col);
                e->literal = t.literal;
                ++pos_;
                return e;
            }
            case Tok::Ident: {
                auto e = node(ExprKind::Name, t.col);
                e->name = t.text;
                ++pos_;
                return e;
            }
            case Tok::LParen: {
                ++pos_;
                ExprPtr inner = or_expr();
                expect(Tok::RParen);
                return inner;
            }
            default:
                fail(std::string("unexpected ") + describe(t));
        }
    }

    std::vector<Token> toks_;
    size_t pos_ = 0;
    int line_;
};

}  // namespace

ExprPtr parse_expression(std::string_view src, int line) {
    Parser p(lex_expression(src, line), line);
    return p.parse_all();
}

}  // namespace pixil
