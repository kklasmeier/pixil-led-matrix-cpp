// Expression AST. Produced once by the parser, annotated once by the resolver
// (names -> slots, calls -> builtin ids), then evaluated many times.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "runtime/python_ops.h"
#include "runtime/value.h"

namespace pixil {

enum class ExprKind {
    Literal,
    Name,     // unresolved identifier (parser output)
    Var,      // resolved v_* variable slot
    Unknown,  // resolved but undefined name -> NameError when evaluated
    Call,
    Index,
    Neg,
    Pos,
    Binary,
    Compare,
    And,
    Or,
    Not,
};

enum class Builtin {
    None,
    Cos, Sin, Tan, Abs, Pow, Sqrt, Exp, Log, Log10,
    Round, Floor, Ceil, Trunc, Int,
    Asin, Acos, Atan, Atan2, Degrees, Radians,
    Min, Max,
    Copysign, Fabs, Remainder, Fmod,
    Random, GetDatetime, GetSystem, Str, IsArray,
    NumpyIntegers, NumpyReseed,
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    ExprKind kind = ExprKind::Literal;
    int line = 0;
    int col = 0;
    Value literal;                 // Literal
    std::string name;              // Name / Unknown / Call
    std::vector<ExprPtr> args;     // children (see ExprKind)
    BinOp binop = BinOp::Add;      // Binary
    std::vector<CmpOp> cmp_ops;    // Compare: args.size() == cmp_ops.size() + 1
    int slot = -1;                 // Var
    Builtin builtin = Builtin::None;  // Call
};

}  // namespace pixil
