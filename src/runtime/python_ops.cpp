#include "runtime/python_ops.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace pixil {

namespace {

Fault g_fault = Fault::None;

[[noreturn]] void type_error_binop(const char* sym, const Value& a, const Value& b) {
    throw PixilError(ErrorKind::Type, std::string("unsupported operand type(s) for ") + sym +
                                          ": '" + a.type_name() + "' and '" + b.type_name() +
                                          "'");
}

const char* binop_symbol(BinOp op) {
    switch (op) {
        case BinOp::Add: return "+";
        case BinOp::Sub: return "-";
        case BinOp::Mul: return "*";
        case BinOp::TrueDiv: return "/";
        case BinOp::FloorDiv: return "//";
        case BinOp::Mod: return "%";
        case BinOp::Pow: return "**";
        case BinOp::Concat: return "&";
    }
    return "?";
}

const char* cmp_symbol(CmpOp op) {
    switch (op) {
        case CmpOp::Eq: return "==";
        case CmpOp::Ne: return "!=";
        case CmpOp::Lt: return "<";
        case CmpOp::Le: return "<=";
        case CmpOp::Gt: return ">";
        case CmpOp::Ge: return ">=";
    }
    return "?";
}

// Integers that leave int64 range become floats (Python would keep a bigint);
// recorded as a deviation in DEVIATIONS.md.
Value int_or_float(bool overflowed, int64_t r, double fallback) {
    if (overflowed) return Value::real(fallback);
    return Value::integer(r);
}

Value float_from_int_like(double d) {
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
        return Value::integer(static_cast<int64_t>(d));
    }
    return Value::real(d);
}

double check_float_result(double r, double a, double b) {
    if (std::isinf(r) && std::isfinite(a) && std::isfinite(b)) {
        throw PixilError(ErrorKind::Overflow, "(34, 'Numerical result out of range')");
    }
    return r;
}

double float_mod(double vx, double wx) {
    if (wx == 0.0) throw PixilError(ErrorKind::ZeroDivision, "float modulo");
    double mod = std::fmod(vx, wx);
    if (g_fault == Fault::CMod) return mod;
    if (mod != 0.0) {
        if ((wx < 0) != (mod < 0)) mod += wx;
    } else {
        mod = std::copysign(0.0, wx);
    }
    return mod;
}

double float_floordiv(double vx, double wx) {
    if (wx == 0.0) throw PixilError(ErrorKind::ZeroDivision, "float floor division by zero");
    double mod = std::fmod(vx, wx);
    double div = (vx - mod) / wx;
    if (mod != 0.0) {
        if ((wx < 0) != (mod < 0)) div -= 1.0;
    }
    double floordiv;
    if (div != 0.0) {
        floordiv = std::floor(div);
        if (div - floordiv > 0.5) floordiv += 1.0;
    } else {
        floordiv = std::copysign(0.0, vx / wx);
    }
    return floordiv;
}

Value int_pow(int64_t base, int64_t exp) {
    // exp >= 0
    int64_t result = 1;
    int64_t b = base;
    int64_t e = exp;
    bool overflow = false;
    while (e > 0 && !overflow) {
        if (e & 1) overflow |= __builtin_mul_overflow(result, b, &result);
        e >>= 1;
        if (e > 0) overflow |= __builtin_mul_overflow(b, b, &b);
    }
    if (overflow) {
        return Value::real(std::pow(static_cast<double>(base), static_cast<double>(exp)));
    }
    return Value::integer(result);
}

Value float_pow(double x, double y) {
    if (x == 0.0 && y < 0.0) {
        throw PixilError(ErrorKind::ZeroDivision, "0.0 cannot be raised to a negative power");
    }
    if (x < 0.0 && std::isfinite(y) && y != std::floor(y)) {
        throw PixilError(ErrorKind::Value,
                         "negative number cannot be raised to a fractional power "
                         "(Python returns a complex number)");
    }
    return Value::real(check_float_result(std::pow(x, y), x, y));
}

int64_t int_floor_mod(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (g_fault == Fault::CMod) return r;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

Value arith_numbers(BinOp op, const Value& a, const Value& b) {
    const bool ints = a.is_integral() && b.is_integral();
    switch (op) {
        case BinOp::Add:
        case BinOp::Sub:
        case BinOp::Mul: {
            if (ints) {
                int64_t x = a.integral(), y = b.integral(), r = 0;
                bool of = op == BinOp::Add   ? __builtin_add_overflow(x, y, &r)
                          : op == BinOp::Sub ? __builtin_sub_overflow(x, y, &r)
                                             : __builtin_mul_overflow(x, y, &r);
                double dx = static_cast<double>(x), dy = static_cast<double>(y);
                double fb = op == BinOp::Add ? dx + dy : op == BinOp::Sub ? dx - dy : dx * dy;
                return int_or_float(of, r, fb);
            }
            double x = a.to_double(), y = b.to_double();
            double r = op == BinOp::Add ? x + y : op == BinOp::Sub ? x - y : x * y;
            return Value::real(r);
        }
        case BinOp::TrueDiv: {
            double y = b.to_double();
            if (y == 0.0) {
                throw PixilError(ErrorKind::ZeroDivision,
                                 ints ? "division by zero" : "float division by zero");
            }
            return Value::real(a.to_double() / y);
        }
        case BinOp::FloorDiv: {
            if (ints) {
                int64_t x = a.integral(), y = b.integral();
                if (y == 0) {
                    throw PixilError(ErrorKind::ZeroDivision,
                                     "integer division or modulo by zero");
                }
                if (x == std::numeric_limits<int64_t>::min() && y == -1) {
                    return Value::real(-static_cast<double>(x));
                }
                int64_t q = x / y;
                if ((x % y != 0) && ((x < 0) != (y < 0))) q -= 1;
                return Value::integer(q);
            }
            return Value::real(float_floordiv(a.to_double(), b.to_double()));
        }
        case BinOp::Mod: {
            if (ints) {
                int64_t x = a.integral(), y = b.integral();
                if (y == 0) throw PixilError(ErrorKind::ZeroDivision, "integer modulo by zero");
                if (y == -1) return Value::integer(0);
                return Value::integer(int_floor_mod(x, y));
            }
            return Value::real(float_mod(a.to_double(), b.to_double()));
        }
        case BinOp::Pow: {
            if (ints) {
                int64_t x = a.integral(), y = b.integral();
                if (y >= 0) return int_pow(x, y);
                if (x == 0) {
                    throw PixilError(ErrorKind::ZeroDivision,
                                     "0.0 cannot be raised to a negative power");
                }
                return Value::real(std::pow(static_cast<double>(x), static_cast<double>(y)));
            }
            return float_pow(a.to_double(), b.to_double());
        }
        case BinOp::Concat: break;
    }
    type_error_binop(binop_symbol(op), a, b);
}

bool parse_decimal_int(const std::string& text, int64_t* out) {
    size_t i = 0, n = text.size();
    while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    while (n > i && std::isspace(static_cast<unsigned char>(text[n - 1]))) --n;
    if (i >= n) return false;
    std::string digits;
    bool neg = false;
    if (text[i] == '+' || text[i] == '-') {
        neg = text[i] == '-';
        ++i;
    }
    bool prev_digit = false;
    for (; i < n; ++i) {
        char c = text[i];
        if (c >= '0' && c <= '9') {
            digits.push_back(c);
            prev_digit = true;
        } else if (c == '_' && prev_digit && i + 1 < n && text[i + 1] >= '0' &&
                   text[i + 1] <= '9') {
            prev_digit = false;
        } else {
            return false;
        }
    }
    if (digits.empty()) return false;
    int64_t value = 0;
    auto res = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (res.ec != std::errc()) return false;
    *out = neg ? -value : value;
    return true;
}

}  // namespace

void set_fault_injection(Fault fault) { g_fault = fault; }

std::string py_float_repr(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);
    std::string sci(buf, res.ptr);
    std::string out;
    size_t pos = 0;
    if (sci[0] == '-') {
        out.push_back('-');
        pos = 1;
    }
    size_t e = sci.find('e');
    std::string digits;
    for (size_t i = pos; i < e; ++i) {
        if (sci[i] != '.') digits.push_back(sci[i]);
    }
    int exp10 = std::atoi(sci.c_str() + e + 1);
    int decpt = exp10 + 1;
    int nd = static_cast<int>(digits.size());
    if (decpt > -4 && decpt <= 16) {
        if (decpt <= 0) {
            out += "0.";
            out.append(static_cast<size_t>(-decpt), '0');
            out += digits;
        } else if (decpt >= nd) {
            out += digits;
            out.append(static_cast<size_t>(decpt - nd), '0');
            out += ".0";
        } else {
            out += digits.substr(0, static_cast<size_t>(decpt));
            out += '.';
            out += digits.substr(static_cast<size_t>(decpt));
        }
        return out;
    }
    out.push_back(digits[0]);
    if (nd > 1) {
        out.push_back('.');
        out += digits.substr(1);
    }
    int x = decpt - 1;
    out.push_back('e');
    out.push_back(x < 0 ? '-' : '+');
    int ax = x < 0 ? -x : x;
    if (ax < 10) out.push_back('0');
    out += std::to_string(ax);
    return out;
}

std::string py_str(const Value& v) {
    if (v.is_none()) return "None";
    if (v.is_bool()) return v.as_bool() ? "True" : "False";
    if (v.is_int()) return std::to_string(v.as_int());
    if (v.is_float()) return py_float_repr(v.as_float());
    if (v.is_str()) return v.as_str();
    return "<PixilArray>";
}

std::string py_repr(const Value& v) {
    if (!v.is_str()) return py_str(v);
    const std::string& s = v.as_str();
    bool has_single = s.find('\'') != std::string::npos;
    bool has_double = s.find('"') != std::string::npos;
    char q = (has_single && !has_double) ? '"' : '\'';
    std::string out(1, q);
    for (char c : s) {
        if (c == q || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out.push_back(c);
        }
    }
    out.push_back(q);
    return out;
}

bool truthy(const Value& v) {
    if (v.is_none()) return false;
    if (v.is_bool()) return v.as_bool();
    if (v.is_int()) return v.as_int() != 0;
    if (v.is_float()) return v.as_float() != 0.0;
    if (v.is_str()) return !v.as_str().empty();
    return true;
}

Value binary_op(BinOp op, const Value& a, const Value& b) {
    if (op == BinOp::Concat) return Value::str(py_str(a) + py_str(b));
    if (a.is_number() && b.is_number()) return arith_numbers(op, a, b);
    if (op == BinOp::Add && a.is_str() && b.is_str()) return Value::str(a.as_str() + b.as_str());
    if (op == BinOp::Mul && (a.is_str() || b.is_str()) && (a.is_integral() || b.is_integral())) {
        const std::string& s = a.is_str() ? a.as_str() : b.as_str();
        int64_t n = a.is_str() ? b.integral() : a.integral();
        std::string out;
        for (int64_t i = 0; i < n; ++i) out += s;
        return Value::str(out);
    }
    if (op == BinOp::Mod && a.is_str()) {
        throw PixilError(ErrorKind::NotImplemented, "printf-style string formatting");
    }
    type_error_binop(binop_symbol(op), a, b);
}

Value negate(const Value& a) {
    if (a.is_integral()) {
        int64_t x = a.integral();
        if (x == std::numeric_limits<int64_t>::min()) return Value::real(-static_cast<double>(x));
        return Value::integer(-x);
    }
    if (a.is_float()) return Value::real(-a.as_float());
    throw PixilError(ErrorKind::Type,
                     std::string("bad operand type for unary -: '") + a.type_name() + "'");
}

Value unary_plus(const Value& a) {
    if (a.is_integral()) return Value::integer(a.integral());
    if (a.is_float()) return a;
    throw PixilError(ErrorKind::Type,
                     std::string("bad operand type for unary +: '") + a.type_name() + "'");
}

bool compare(CmpOp op, const Value& a, const Value& b) {
    if (a.is_number() && b.is_number()) {
        if (a.is_integral() && b.is_integral()) {
            int64_t x = a.integral(), y = b.integral();
            switch (op) {
                case CmpOp::Eq: return x == y;
                case CmpOp::Ne: return x != y;
                case CmpOp::Lt: return x < y;
                case CmpOp::Le: return x <= y;
                case CmpOp::Gt: return x > y;
                case CmpOp::Ge: return x >= y;
            }
        }
        double x = a.to_double(), y = b.to_double();
        switch (op) {
            case CmpOp::Eq: return x == y;
            case CmpOp::Ne: return x != y;
            case CmpOp::Lt: return x < y;
            case CmpOp::Le: return x <= y;
            case CmpOp::Gt: return x > y;
            case CmpOp::Ge: return x >= y;
        }
    }
    if (a.is_str() && b.is_str()) {
        int c = a.as_str().compare(b.as_str());
        switch (op) {
            case CmpOp::Eq: return c == 0;
            case CmpOp::Ne: return c != 0;
            case CmpOp::Lt: return c < 0;
            case CmpOp::Le: return c <= 0;
            case CmpOp::Gt: return c > 0;
            case CmpOp::Ge: return c >= 0;
        }
    }
    if (op == CmpOp::Eq || op == CmpOp::Ne) {
        bool same = a.is_none() && b.is_none();
        if (a.is_array() && b.is_array()) same = a.as_array() == b.as_array();
        return op == CmpOp::Eq ? same : !same;
    }
    throw PixilError(ErrorKind::Type, std::string("'") + cmp_symbol(op) +
                                          "' not supported between instances of '" +
                                          a.type_name() + "' and '" + b.type_name() + "'");
}

double require_number(const Value& v, const char* fn) {
    if (v.is_number()) return v.to_double();
    throw PixilError(ErrorKind::Type,
                     std::string(fn) + "(): must be real number, not " + v.type_name());
}

static void check_finite_for_int(double d) {
    if (std::isnan(d)) throw PixilError(ErrorKind::Value, "cannot convert float NaN to integer");
    if (std::isinf(d)) {
        throw PixilError(ErrorKind::Overflow, "cannot convert float infinity to integer");
    }
}

Value py_round(const Value& x) {
    if (x.is_integral()) return Value::integer(x.integral());
    if (x.is_float()) {
        double d = x.as_float();
        check_finite_for_int(d);
        return float_from_int_like(std::nearbyint(d));
    }
    throw PixilError(ErrorKind::Type, std::string("type ") + x.type_name() +
                                          " doesn't define __round__ method");
}

Value py_round(const Value& x, const Value& ndigits) {
    if (ndigits.is_none()) return py_round(x);
    if (!ndigits.is_integral()) {
        throw PixilError(ErrorKind::Type, std::string("'") + ndigits.type_name() +
                                              "' object cannot be interpreted as an integer");
    }
    int64_t n = ndigits.integral();
    if (x.is_integral()) {
        int64_t v = x.integral();
        if (n >= 0) return Value::integer(v);
        if (n < -18) return Value::integer(0);
        int64_t p = 1;
        for (int64_t i = 0; i < -n; ++i) p *= 10;
        int64_t q = v / p, r = v % p;
        if (r < 0) {
            r += p;
            q -= 1;
        }
        if (2 * r > p || (2 * r == p && (q % 2 != 0))) q += 1;
        return Value::integer(q * p);
    }
    if (!x.is_float()) {
        throw PixilError(ErrorKind::Type, std::string("type ") + x.type_name() +
                                              " doesn't define __round__ method");
    }
    double d = x.as_float();
    if (!std::isfinite(d) || n > 323) return Value::real(d);
    if (n >= 0) {
        // printf rounds the exact binary value half-to-even, as CPython's round does.
        char buf[512];
        std::snprintf(buf, sizeof buf, "%.*f", static_cast<int>(n), d);
        return Value::real(std::strtod(buf, nullptr));
    }
    if (n < -308) return Value::real(std::copysign(0.0, d));
    double p = std::pow(10.0, static_cast<double>(-n));
    return Value::real(std::nearbyint(d / p) * p);
}

Value py_int(const Value& x) {
    if (x.is_integral()) return Value::integer(x.integral());
    if (x.is_float()) {
        double d = x.as_float();
        check_finite_for_int(d);
        return float_from_int_like(std::trunc(d));
    }
    if (x.is_str()) {
        int64_t out = 0;
        if (parse_decimal_int(x.as_str(), &out)) return Value::integer(out);
        throw PixilError(ErrorKind::Value,
                         "invalid literal for int() with base 10: " + py_repr(x));
    }
    throw PixilError(ErrorKind::Type,
                     std::string("int() argument must be a string, a bytes-like object or a "
                                 "real number, not '") +
                         x.type_name() + "'");
}

Value py_floor(const Value& x) {
    if (x.is_integral()) return Value::integer(x.integral());
    double d = require_number(x, "floor");
    check_finite_for_int(d);
    return float_from_int_like(std::floor(d));
}

Value py_ceil(const Value& x) {
    if (x.is_integral()) return Value::integer(x.integral());
    double d = require_number(x, "ceil");
    check_finite_for_int(d);
    return float_from_int_like(std::ceil(d));
}

}  // namespace pixil
