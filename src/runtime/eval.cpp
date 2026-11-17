#include "runtime/eval.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <numbers>

#include "runtime/grid.h"
#include "syntax/parser.h"

namespace pixil {

int SymbolTable::intern(const std::string& name) {
    auto it = slots_.find(name);
    if (it != slots_.end()) return it->second;
    int slot = static_cast<int>(names_.size());
    slots_.emplace(name, slot);
    names_.push_back(name);
    return slot;
}

int SymbolTable::find(const std::string& name) const {
    auto it = slots_.find(name);
    return it == slots_.end() ? -1 : it->second;
}

namespace {

struct BuiltinInfo {
    const char* name;
    Builtin id;
    int min_args;
    int max_args;  // -1 = variadic
};

constexpr BuiltinInfo kBuiltins[] = {
    {"cos", Builtin::Cos, 1, 1},         {"sin", Builtin::Sin, 1, 1},
    {"tan", Builtin::Tan, 1, 1},         {"abs", Builtin::Abs, 1, 1},
    {"pow", Builtin::Pow, 2, 2},         {"sqrt", Builtin::Sqrt, 1, 1},
    {"exp", Builtin::Exp, 1, 1},         {"log", Builtin::Log, 1, 2},
    {"log10", Builtin::Log10, 1, 1},     {"round", Builtin::Round, 1, 2},
    {"floor", Builtin::Floor, 1, 1},     {"ceil", Builtin::Ceil, 1, 1},
    {"trunc", Builtin::Trunc, 1, 1},     {"int", Builtin::Int, 1, 1},
    {"asin", Builtin::Asin, 1, 1},       {"acos", Builtin::Acos, 1, 1},
    {"atan", Builtin::Atan, 1, 1},       {"atan2", Builtin::Atan2, 2, 2},
    {"degrees", Builtin::Degrees, 1, 1}, {"radians", Builtin::Radians, 1, 1},
    {"min", Builtin::Min, 1, -1},        {"max", Builtin::Max, 1, -1},
    {"copysign", Builtin::Copysign, 2, 2}, {"fabs", Builtin::Fabs, 1, 1},
    {"remainder", Builtin::Remainder, 2, 2}, {"fmod", Builtin::Fmod, 2, 2},
    {"random", Builtin::Random, 3, 3},   {"get_datetime", Builtin::GetDatetime, 1, 1},
    {"get_system", Builtin::GetSystem, 1, 1}, {"str", Builtin::Str, 1, 1},
    {"is_array", Builtin::IsArray, 1, 1},
    {"numpy_integers", Builtin::NumpyIntegers, 2, 2},
    {"numpy_reseed", Builtin::NumpyReseed, 0, 0},
};

const BuiltinInfo* find_builtin(const std::string& name) {
    for (const auto& b : kBuiltins) {
        if (name == b.name) return &b;
    }
    return nullptr;
}

[[noreturn]] void raise_at(const Expr& e, ErrorKind kind, const std::string& msg) {
    throw PixilError(kind, msg + (e.line > 0 ? " (line " + std::to_string(e.line) + ")" : ""));
}

[[noreturn]] void domain_error() { throw PixilError(ErrorKind::Value, "math domain error"); }

double checked_range(double r) {
    if (std::isinf(r)) throw PixilError(ErrorKind::Overflow, "math range error");
    return r;
}

Value math1(Builtin id, const Value& v) {
    double x = require_number(v, "math");
    switch (id) {
        case Builtin::Cos:
            if (std::isinf(x)) domain_error();
            return Value::real(std::cos(x));
        case Builtin::Sin:
            if (std::isinf(x)) domain_error();
            return Value::real(std::sin(x));
        case Builtin::Tan:
            if (std::isinf(x)) domain_error();
            return Value::real(std::tan(x));
        case Builtin::Sqrt:
            if (x < 0) domain_error();
            return Value::real(std::sqrt(x));
        case Builtin::Exp:
            if (std::isfinite(x)) return Value::real(checked_range(std::exp(x)));
            return Value::real(std::exp(x));
        case Builtin::Log10:
            if (x <= 0) domain_error();
            return Value::real(std::log10(x));
        case Builtin::Asin:
            if (x < -1 || x > 1) domain_error();
            return Value::real(std::asin(x));
        case Builtin::Acos:
            if (x < -1 || x > 1) domain_error();
            return Value::real(std::acos(x));
        case Builtin::Atan: return Value::real(std::atan(x));
        case Builtin::Degrees: return Value::real(x * (180.0 / std::numbers::pi));
        case Builtin::Radians: return Value::real(x * (std::numbers::pi / 180.0));
        case Builtin::Fabs: return Value::real(std::fabs(x));
        default: break;
    }
    throw PixilError(ErrorKind::Type, "internal: bad math1 builtin");
}

std::string lower_strip(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    std::string out = s.substr(b, e - b);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_leap_year(int year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); }

int days_in_month(int year, int month) {
    static const int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) return 29;
    return kDays[month];
}

// ISO week number, matching datetime.date.isocalendar().week.
int iso_week(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    if (std::mktime(&tm) == static_cast<std::time_t>(-1)) return 1;
    int iso_wday = tm.tm_wday == 0 ? 7 : tm.tm_wday;
    int week = (tm.tm_yday + 1 - iso_wday + 10) / 7;
    if (week < 1) return iso_week(year - 1, 12, 31);
    if (week > 52) {
        std::tm dec{};
        dec.tm_year = year - 1900;
        dec.tm_mon = 11;
        dec.tm_mday = 31;
        dec.tm_hour = 12;
        dec.tm_isdst = -1;
        std::mktime(&dec);
        int end_wday = dec.tm_wday == 0 ? 7 : dec.tm_wday;
        int last = (dec.tm_yday + 1 - end_wday + 10) / 7;
        if (week > last || last == 1) return 1;
    }
    return week;
}

std::string zfill(int value, int width) {
    std::string s = std::to_string(value < 0 ? -value : value);
    if (static_cast<int>(s.size()) < width) s.insert(0, static_cast<size_t>(width) - s.size(), '0');
    if (value < 0) s.insert(s.begin(), '-');
    return s;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    size_t at = 0;
    while ((at = s.find(from, at)) != std::string::npos) {
        s.replace(at, from.size(), to);
        at += to.size();
    }
}

// legacy get_datetime: a single code is a number or a name; anything longer is text.
Value get_datetime(const std::string& format) {
    if (format.empty()) throw PixilError(ErrorKind::Value, "get_datetime format string cannot be empty");
    const auto now = std::chrono::system_clock::now();
    const std::time_t sec = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&sec, &tm);
    const int year = tm.tm_year + 1900;
    const int month = tm.tm_mon + 1;
    const int hour_12_raw = tm.tm_hour % 12;
    const int hour_12 = hour_12_raw == 0 ? 12 : hour_12_raw;
    const int wday = tm.tm_wday;
    const int doy = tm.tm_yday + 1;
    const int days_year = is_leap_year(year) ? 366 : 365;
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    const bool pm = tm.tm_hour >= 12;
    const int weekend = (wday == 0 || wday == 6) ? 1 : 0;
    static const char* kDayFull[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* kDayAbbr[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonthFull[] = {"", "January", "February", "March", "April", "May", "June",
                                       "July", "August", "September", "October", "November", "December"};
    static const char* kMonthAbbr[] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    if (format == "H") return Value::integer(tm.tm_hour);
    if (format == "h") return Value::integer(hour_12);
    if (format == "mi") return Value::integer(tm.tm_min);
    if (format == "s") return Value::integer(tm.tm_sec);
    if (format == "ms") return Value::integer(ms);
    if (format == "d") return Value::integer(tm.tm_mday);
    if (format == "m") return Value::integer(month);
    if (format == "yyyy") return Value::integer(year);
    if (format == "yy") return Value::integer(year % 100);
    if (format == "w") return Value::integer(wday);
    if (format == "ts") return Value::integer(static_cast<int64_t>(sec));
    if (format == "tz") return Value::integer(static_cast<int64_t>(tm.tm_gmtoff / 3600));
    if (format == "doy") return Value::integer(doy);
    if (format == "woy") return Value::integer(iso_week(year, month, tm.tm_mday));
    if (format == "diy") return Value::integer(days_year - doy);
    if (format == "q") return Value::integer((month - 1) / 3 + 1);
    if (format == "ispm") return Value::integer(pm ? 1 : 0);
    if (format == "isweekend") return Value::integer(weekend);
    if (format == "isleap") return Value::integer(is_leap_year(year) ? 1 : 0);
    if (format == "dim") return Value::integer(days_in_month(year, month));
    if (format == "a") return Value::str(pm ? "PM" : "AM");
    if (format == "dddd") return Value::str(kDayFull[wday]);
    if (format == "ddd") return Value::str(kDayAbbr[wday]);
    if (format == "mmmm") return Value::str(kMonthFull[month]);
    if (format == "mmm") return Value::str(kMonthAbbr[month]);

    const std::pair<const char*, std::string> codes[] = {
        {"isweekend", std::to_string(weekend)},
        {"isleap", std::to_string(is_leap_year(year) ? 1 : 0)},
        {"ispm", std::to_string(pm ? 1 : 0)},
        {"yyyy", std::to_string(year)},
        {"dddd", kDayFull[wday]},
        {"mmmm", kMonthFull[month]},
        {"ddd", kDayAbbr[wday]},
        {"mmm", kMonthAbbr[month]},
        {"doy", std::to_string(doy)},
        {"woy", std::to_string(iso_week(year, month, tm.tm_mday))},
        {"diy", std::to_string(days_year - doy)},
        {"dim", std::to_string(days_in_month(year, month))},
        {"mi", zfill(tm.tm_min, 2)},
        {"ms", zfill(ms, 3)},
        {"ts", std::to_string(static_cast<long long>(sec))},
        {"tz", std::to_string(static_cast<long long>(tm.tm_gmtoff / 3600))},
        {"yy", zfill(year % 100, 2)},
        {"H", zfill(tm.tm_hour, 2)},
        {"h", std::to_string(hour_12)},
        {"d", std::to_string(tm.tm_mday)},
        {"m", std::to_string(month)},
        {"q", std::to_string((month - 1) / 3 + 1)},
        {"w", std::to_string(wday)},
        {"s", zfill(tm.tm_sec, 2)},
        {"a", pm ? "PM" : "AM"},
    };
    std::string result = format;
    std::vector<std::pair<std::string, std::string>> reps;
    int n = 0;
    for (const auto& code : codes) {
        if (result.find(code.first) == std::string::npos) continue;
        std::string mark = std::string("\x00") + std::to_string(n) + "\x00";
        replace_all(result, code.first, mark);
        reps.emplace_back(std::move(mark), code.second);
        ++n;
    }
    if (reps.empty()) {
        throw PixilError(ErrorKind::Value, "Invalid datetime format: '" + format + "' - no recognized format codes found");
    }
    for (const auto& rep : reps) replace_all(result, rep.first, rep.second);
    return Value::str(result);
}

Value call_builtin(const Expr& e, std::vector<Value>& a, Env& env) {
    switch (e.builtin) {
        case Builtin::Cos:
        case Builtin::Sin:
        case Builtin::Tan:
        case Builtin::Sqrt:
        case Builtin::Exp:
        case Builtin::Log10:
        case Builtin::Asin:
        case Builtin::Acos:
        case Builtin::Atan:
        case Builtin::Degrees:
        case Builtin::Radians:
        case Builtin::Fabs:
            return math1(e.builtin, a[0]);
        case Builtin::Abs:
            if (a[0].is_integral()) {
                int64_t v = a[0].integral();
                return v < 0 ? negate(Value::integer(v)) : Value::integer(v);
            }
            if (a[0].is_float()) return Value::real(std::fabs(a[0].as_float()));
            throw PixilError(ErrorKind::Type,
                             std::string("bad operand type for abs(): '") + a[0].type_name() + "'");
        case Builtin::Pow: return binary_op(BinOp::Pow, a[0], a[1]);
        case Builtin::Log: {
            double x = require_number(a[0], "log");
            if (x <= 0) domain_error();
            double r = std::log(x);
            if (a.size() == 2) {
                double b = require_number(a[1], "log");
                if (b <= 0) domain_error();
                double lb = std::log(b);
                if (lb == 0.0) throw PixilError(ErrorKind::ZeroDivision, "float division by zero");
                r /= lb;
            }
            return Value::real(r);
        }
        case Builtin::Round:
            return a.size() == 2 ? py_round(a[0], a[1]) : py_round(a[0]);
        case Builtin::Floor: return py_floor(a[0]);
        case Builtin::Ceil: return py_ceil(a[0]);
        case Builtin::Trunc:
            if (a[0].is_str()) {
                throw PixilError(ErrorKind::Type, "type str doesn't define __trunc__ method");
            }
            return py_int(a[0]);
        case Builtin::Int: return py_int(a[0]);
        case Builtin::Str: return Value::str(py_str(a[0]));
        case Builtin::IsArray: return Value::integer(a[0].is_array() ? 1 : 0);
        case Builtin::Atan2:
            return Value::real(std::atan2(require_number(a[0], "atan2"),
                                          require_number(a[1], "atan2")));
        case Builtin::Copysign:
            return Value::real(std::copysign(require_number(a[0], "copysign"),
                                             require_number(a[1], "copysign")));
        case Builtin::Remainder: {
            double x = require_number(a[0], "remainder"), y = require_number(a[1], "remainder");
            if (y == 0.0 || std::isinf(x)) domain_error();
            return Value::real(std::remainder(x, y));
        }
        case Builtin::Fmod: {
            double x = require_number(a[0], "fmod"), y = require_number(a[1], "fmod");
            if (y == 0.0 || std::isinf(x)) domain_error();
            return Value::real(std::fmod(x, y));
        }
        case Builtin::Min:
        case Builtin::Max: {
            if (a.size() < 2) {
                throw PixilError(ErrorKind::Type, std::string("'") + a[0].type_name() +
                                                      "' object is not iterable");
            }
            size_t best = 0;
            CmpOp better = e.builtin == Builtin::Min ? CmpOp::Lt : CmpOp::Gt;
            for (size_t i = 1; i < a.size(); ++i) {
                if (compare(better, a[i], a[best])) best = i;
            }
            return a[best];
        }
        case Builtin::Random: {
            // legacy random_float(min, max, precision)
            Value range = binary_op(BinOp::Sub, a[1], a[0]);
            Value r = binary_op(BinOp::Add,
                                binary_op(BinOp::Mul, Value::real(env.rng.random()), range), a[0]);
            if (compare(CmpOp::Eq, a[2], Value::integer(0))) return py_int(py_round(r));
            return py_round(r, a[2]);
        }
        case Builtin::GetSystem: {
            if (!a[0].is_str()) {
                throw PixilError(ErrorKind::Type, "get_system() metric must be a string");
            }
            std::string m = lower_strip(a[0].as_str());
            if (m == "runtime") {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - env.script_start);
                return Value::integer(static_cast<int64_t>(ms.count()));
            }
            if (m == "test_mode") return Value::integer(env.test_mode ? 1 : 0);
            throw PixilError(ErrorKind::Value, "Unknown system metric: '" + a[0].as_str() +
                                                   "' - valid options: runtime, test_mode");
        }
        case Builtin::GetDatetime: {
            if (!a[0].is_str()) {
                throw PixilError(ErrorKind::Value, std::string("get_datetime format must be a string, got ") + a[0].type_name());
            }
            return get_datetime(a[0].as_str());
        }
        case Builtin::NumpyReseed:
            numpy_reseed();
            return Value::integer(0);
        case Builtin::NumpyIntegers: {
            auto as_int = [](const Value& v, const char* name) {
                if (v.is_integral()) return v.integral();
                if (v.is_float()) return py_int(v).integral();
                throw PixilError(ErrorKind::Type, std::string(name) + "() argument must be a number");
            };
            return Value::integer(numpy_integers(as_int(a[0], "numpy_integers"),
                                                 as_int(a[1], "numpy_integers")));
        }
        case Builtin::None: break;
    }
    raise_at(e, ErrorKind::Type, "internal: unresolved call " + e.name);
}

int64_t array_index(const PixArray& arr, const Value& index) {
    int64_t i = 0;
    if (index.is_integral()) {
        i = index.integral();
    } else if (index.is_float()) {
        Value t = py_int(index);
        if (!t.is_int()) throw PixilError(ErrorKind::Index, "Array index out of bounds");
        i = t.as_int();
    } else {
        throw PixilError(ErrorKind::Value, std::string("Array index must be a number, got ") +
                                               index.type_name());
    }
    const int64_t size = static_cast<int64_t>(arr.data.size());
    if (i < 0 || i >= size) {
        throw PixilError(ErrorKind::Index, "Array index " + std::to_string(i) +
                                               " out of bounds [0, " + std::to_string(size - 1) +
                                               "]");
    }
    return i;
}

}  // namespace

const Value& array_load(const PixArray& arr, const Value& index) {
    return arr.data[static_cast<size_t>(array_index(arr, index))];
}

void array_store(PixArray& arr, const Value& index, const Value& value) {
    size_t i = static_cast<size_t>(array_index(arr, index));
    if (arr.type == ArrayType::Numeric) {
        if (!value.is_number()) {
            throw PixilError(ErrorKind::Value, std::string("Numeric array requires numeric values, got ") +
                                                   value.type_name());
        }
        arr.data[i] = Value::real(value.to_double());
        return;
    }
    if (!value.is_str()) {
        throw PixilError(ErrorKind::Value, std::string("String array requires string values, got ") +
                                               value.type_name());
    }
    arr.data[i] = value;
}

void resolve(Expr& e, SymbolTable& symbols) {
    for (auto& child : e.args) resolve(*child, symbols);
    if (e.kind == ExprKind::Name) {
        const std::string& n = e.name;
        if (n.rfind("v_", 0) == 0) {
            e.kind = ExprKind::Var;
            e.slot = symbols.intern(n);
        } else if (n == "pi") {
            e.kind = ExprKind::Literal;
            e.literal = Value::real(std::numbers::pi);
        } else if (n == "e") {
            e.kind = ExprKind::Literal;
            e.literal = Value::real(std::numbers::e);
        } else if (n == "tau") {
            e.kind = ExprKind::Literal;
            e.literal = Value::real(2 * std::numbers::pi);
        } else if (n == "True" || n == "true") {
            e.kind = ExprKind::Literal;
            e.literal = Value::boolean(true);
        } else if (n == "False" || n == "false") {
            e.kind = ExprKind::Literal;
            e.literal = Value::boolean(false);
        } else if (n == "None") {
            e.kind = ExprKind::Literal;
            e.literal = Value::none();
        } else {
            e.kind = ExprKind::Unknown;
        }
        return;
    }
    if (e.kind == ExprKind::Call) {
        const BuiltinInfo* b = find_builtin(e.name);
        if (b == nullptr) {
            throw PixilError(ErrorKind::Name, "line " + std::to_string(e.line) +
                                                  ": unknown function '" + e.name + "'");
        }
        int n = static_cast<int>(e.args.size());
        if (n < b->min_args || (b->max_args >= 0 && n > b->max_args)) {
            throw PixilError(ErrorKind::Type, "line " + std::to_string(e.line) + ": " + e.name +
                                                  "() takes " + std::to_string(b->min_args) +
                                                  (b->max_args == b->min_args ? "" : "+") +
                                                  " argument(s), got " + std::to_string(n));
        }
        e.builtin = b->id;
    }
}

Value eval(const Expr& e, Env& env) {
    switch (e.kind) {
        case ExprKind::Literal: return e.literal;
        case ExprKind::Var: {
            const Value& v = env.slots[static_cast<size_t>(e.slot)];
            if (v.is_none()) {
                raise_at(e, ErrorKind::Name,
                         "Variable '" + env.symbols.name_of(e.slot) + "' not found");
            }
            return v;
        }
        case ExprKind::Name:
        case ExprKind::Unknown:
            raise_at(e, ErrorKind::Name, "name '" + e.name + "' is not defined");
        case ExprKind::Index: {
            Value target = eval(*e.args[0], env);
            if (!target.is_array()) {
                raise_at(e, ErrorKind::Type,
                         std::string("'") + target.type_name() + "' object is not subscriptable");
            }
            return array_load(*target.as_array(), eval(*e.args[1], env));
        }
        case ExprKind::Neg: return negate(eval(*e.args[0], env));
        case ExprKind::Pos: return unary_plus(eval(*e.args[0], env));
        case ExprKind::Binary: {
            Value l = eval(*e.args[0], env);
            Value r = eval(*e.args[1], env);
            return binary_op(e.binop, l, r);
        }
        case ExprKind::Compare: {
            Value left = eval(*e.args[0], env);
            for (size_t i = 0; i < e.cmp_ops.size(); ++i) {
                Value right = eval(*e.args[i + 1], env);
                if (!compare(e.cmp_ops[i], left, right)) return Value::boolean(false);
                left = std::move(right);
            }
            return Value::boolean(true);
        }
        case ExprKind::And: {
            Value l = eval(*e.args[0], env);
            if (!truthy(l)) return l;
            return eval(*e.args[1], env);
        }
        case ExprKind::Or: {
            Value l = eval(*e.args[0], env);
            if (truthy(l)) return l;
            return eval(*e.args[1], env);
        }
        case ExprKind::Not: return Value::boolean(!truthy(eval(*e.args[0], env)));
        case ExprKind::Call: {
            std::vector<Value> args;
            args.reserve(e.args.size());
            for (const auto& a : e.args) args.push_back(eval(*a, env));
            return call_builtin(e, args, env);
        }
    }
    raise_at(e, ErrorKind::Type, "internal: bad expression node");
}

ExprPtr compile_expression(std::string_view src, SymbolTable& symbols, int line) {
    ExprPtr e = parse_expression(src, line);
    resolve(*e, symbols);
    return e;
}

}  // namespace pixil
