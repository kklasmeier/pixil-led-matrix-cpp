// Runtime values with Python-compatible semantics (see docs/LANGUAGE.md §Values).
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace pixil {

enum class ErrorKind {
    Type,
    Value,
    ZeroDivision,
    Overflow,
    Name,
    Index,
    Syntax,
    NotImplemented,
};

const char* error_kind_name(ErrorKind kind);

// Every evaluation failure carries the Python exception class it mirrors so the
// oracle harness can compare failure classes, not just "it failed".
class PixilError : public std::runtime_error {
public:
    PixilError(ErrorKind kind, const std::string& message)
        : std::runtime_error(message), kind_(kind) {}
    ErrorKind kind() const { return kind_; }

private:
    ErrorKind kind_;
};

struct PixArray;

struct NoneValue {
    bool operator==(const NoneValue&) const = default;
};

class Value {
public:
    using Storage =
        std::variant<NoneValue, bool, int64_t, double, std::string, std::shared_ptr<PixArray>>;

    Value() = default;
    static Value none() { return Value(); }
    static Value boolean(bool b) { return Value(Storage(b)); }
    static Value integer(int64_t i) { return Value(Storage(i)); }
    static Value real(double d) { return Value(Storage(d)); }
    static Value str(std::string s) { return Value(Storage(std::move(s))); }
    static Value array(std::shared_ptr<PixArray> a) { return Value(Storage(std::move(a))); }

    bool is_none() const { return std::holds_alternative<NoneValue>(v_); }
    bool is_bool() const { return std::holds_alternative<bool>(v_); }
    bool is_int() const { return std::holds_alternative<int64_t>(v_); }
    bool is_float() const { return std::holds_alternative<double>(v_); }
    bool is_str() const { return std::holds_alternative<std::string>(v_); }
    bool is_array() const { return std::holds_alternative<std::shared_ptr<PixArray>>(v_); }
    // Python: bool is a subclass of int.
    bool is_integral() const { return is_int() || is_bool(); }
    bool is_number() const { return is_integral() || is_float(); }

    bool as_bool() const { return std::get<bool>(v_); }
    int64_t as_int() const { return std::get<int64_t>(v_); }
    double as_float() const { return std::get<double>(v_); }
    const std::string& as_str() const { return std::get<std::string>(v_); }
    const std::shared_ptr<PixArray>& as_array() const {
        return std::get<std::shared_ptr<PixArray>>(v_);
    }

    // Integral value of an int or bool. Precondition: is_integral().
    int64_t integral() const { return is_bool() ? (as_bool() ? 1 : 0) : as_int(); }
    // Numeric value widened to double. Precondition: is_number().
    double to_double() const;

    // Python type name: "NoneType", "bool", "int", "float", "str", "array".
    const char* type_name() const;

private:
    explicit Value(Storage v) : v_(std::move(v)) {}
    Storage v_;
};

enum class ArrayType { Numeric, String };

struct PixArray {
    PixArray(size_t size, ArrayType type);
    ArrayType type;
    std::vector<Value> data;
};

}  // namespace pixil
