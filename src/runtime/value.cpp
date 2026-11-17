#include "runtime/value.h"

namespace pixil {

const char* error_kind_name(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::Type: return "TypeError";
        case ErrorKind::Value: return "ValueError";
        case ErrorKind::ZeroDivision: return "ZeroDivisionError";
        case ErrorKind::Overflow: return "OverflowError";
        case ErrorKind::Name: return "NameError";
        case ErrorKind::Index: return "IndexError";
        case ErrorKind::Syntax: return "SyntaxError";
        case ErrorKind::NotImplemented: return "NotImplementedError";
    }
    return "Error";
}

double Value::to_double() const {
    if (is_float()) return as_float();
    return static_cast<double>(integral());
}

const char* Value::type_name() const {
    if (is_none()) return "NoneType";
    if (is_bool()) return "bool";
    if (is_int()) return "int";
    if (is_float()) return "float";
    if (is_str()) return "str";
    return "array";
}

PixArray::PixArray(size_t size, ArrayType t) : type(t) {
    // Legacy PixilArray initialises numeric cells to int 0 (not 0.0) and string
    // cells to ''. Assignment later stores float(value).
    data.assign(size, t == ArrayType::Numeric ? Value::integer(0) : Value::str(""));
}

}  // namespace pixil
