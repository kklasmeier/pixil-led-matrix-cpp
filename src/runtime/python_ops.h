// Operators and conversions that reproduce CPython 3 behaviour for the value
// kinds Pixil uses. Hazards are listed in the Phase 1 plan §6.6.
#pragma once

#include <string>

#include "runtime/value.h"

namespace pixil {

enum class BinOp { Add, Sub, Mul, TrueDiv, FloorDiv, Mod, Pow, Concat };
enum class CmpOp { Eq, Ne, Lt, Le, Gt, Ge };

// Test-only fault injection used by the oracle negative test (tools/expr_crosscheck.py).
enum class Fault { None, CMod };
void set_fault_injection(Fault fault);

// repr(float) — shortest round-trip digits, Python exponent rules.
std::string py_float_repr(double d);
// str(value) as Python would print it (strings unquoted).
std::string py_str(const Value& v);
// repr(value) (strings single-quoted like Python).
std::string py_repr(const Value& v);

bool truthy(const Value& v);

Value binary_op(BinOp op, const Value& a, const Value& b);
Value negate(const Value& a);
Value unary_plus(const Value& a);
bool compare(CmpOp op, const Value& a, const Value& b);

// Python round(x) and round(x, ndigits).
Value py_round(const Value& x);
Value py_round(const Value& x, const Value& ndigits);
// Python int(x) (truncation; parses decimal strings).
Value py_int(const Value& x);
// math.floor / math.ceil (return int in Python 3).
Value py_floor(const Value& x);
Value py_ceil(const Value& x);

// Numeric argument for math.* functions; TypeError for non-numbers.
double require_number(const Value& v, const char* fn);

}  // namespace pixil
