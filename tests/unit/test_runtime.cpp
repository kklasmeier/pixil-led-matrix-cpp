// Python-semantics unit tests. Expected values were produced by CPython 3.11.
#include "check.h"
#include "runtime/eval.h"
#include "runtime/py_random.h"
#include "runtime/python_ops.h"

using namespace pixil;

namespace {

std::string ev(const std::string& src) {
    SymbolTable symbols;
    ExprPtr e = compile_expression(src, symbols);
    Env env(symbols);
    env.ensure_slots();
    return py_repr(eval(*e, env));
}

}  // namespace

TEST(float_repr_matches_cpython) {
    CHECK_EQ(py_float_repr(0.1 + 0.2), "0.30000000000000004");
    CHECK_EQ(py_float_repr(1e16), "1e+16");
    CHECK_EQ(py_float_repr(1e15), "1000000000000000.0");
    CHECK_EQ(py_float_repr(1e-5), "1e-05");
    CHECK_EQ(py_float_repr(0.0001), "0.0001");
    CHECK_EQ(py_float_repr(123456789.0 * 1e9), "1.23456789e+17");
    CHECK_EQ(py_float_repr(3.0), "3.0");
    CHECK_EQ(py_float_repr(-0.0), "-0.0");
    CHECK_EQ(py_float_repr(5e-324), "5e-324");
    CHECK_EQ(py_float_repr(1.7976931348623157e308), "1.7976931348623157e+308");
    CHECK_EQ(py_float_repr(2.5e-7 * 2), "5e-07");
    CHECK_EQ(py_float_repr(1.0 / 3), "0.3333333333333333");
    CHECK_EQ(py_float_repr(100.0), "100.0");
    CHECK_EQ(py_float_repr(1e22), "1e+22");
    CHECK_EQ(py_float_repr(12345678901234567.0), "1.2345678901234568e+16");
}

TEST(mt19937_matches_cpython_seeding) {
    PyRandom r;
    r.seed(42);
    CHECK_EQ(py_float_repr(r.random()), "0.6394267984578837");
    CHECK_EQ(py_float_repr(r.random()), "0.025010755222666936");
    CHECK_EQ(py_float_repr(r.random()), "0.27502931836911926");
    r.seed(0);
    CHECK_EQ(py_float_repr(r.random()), "0.8444218515250481");
    r.seed(-7);  // CPython seeds with abs(n)
    CHECK_EQ(py_float_repr(r.random()), "0.32383276483316237");
    r.seed((int64_t{1} << 40) + 5);  // two 32-bit key words
    CHECK_EQ(py_float_repr(r.random()), "0.5043802970418443");
    CHECK_EQ(py_float_repr(r.random()), "0.2686044399723282");
    r.seed(123456789012);
    CHECK_EQ(py_float_repr(r.random()), "0.37701448538609916");
    r.seed(42);
    CHECK_EQ(r.genrand_uint32(), 2746317213u);
    CHECK_EQ(r.genrand_uint32(), 478163327u);
}

TEST(floor_mod_and_division) {
    CHECK_EQ(ev("-1 % 64"), "63");
    CHECK_EQ(ev("7 % -3"), "-2");
    CHECK_EQ(ev("-7.5 % 2"), "0.5");
    CHECK_EQ(ev("5.5 % -2"), "-0.5");
    CHECK_EQ(ev("10 / 5"), "2.0");
    CHECK_EQ(ev("-7 // 2"), "-4");
    CHECK_EQ(ev("-7.0 // 2"), "-4.0");
    CHECK_THROWS(ev("1 / 0"));
    CHECK_THROWS(ev("1 % 0"));
}

TEST(rounding_is_bankers) {
    CHECK_EQ(ev("round(2.5)"), "2");
    CHECK_EQ(ev("round(-2.5)"), "-2");
    CHECK_EQ(ev("round(3.5)"), "4");
    CHECK_EQ(ev("round(2.675, 2)"), "2.67");
    CHECK_EQ(ev("round(1.5, 0)"), "2.0");
    CHECK_EQ(ev("round(0.125, 2)"), "0.12");
    CHECK_EQ(ev("round(0.375, 2)"), "0.38");
    CHECK_EQ(ev("round(25, -1)"), "20");
    CHECK_EQ(ev("round(35, -1)"), "40");
    CHECK_EQ(ev("round(-25, -1)"), "-20");
}

TEST(power_and_precedence) {
    CHECK_EQ(ev("-2 ** 2"), "-4");
    CHECK_EQ(ev("(-2) ** 2"), "4");
    CHECK_EQ(ev("2 ** -1"), "0.5");
    CHECK_EQ(ev("2 ** 3 ** 2"), "512");
    CHECK_EQ(ev("1 + 2 * 3 - 4 / 2"), "5.0");
    CHECK_THROWS(ev("0 ** -1"));
}

TEST(logic_returns_operands) {
    CHECK_EQ(ev("3 and 5"), "5");
    CHECK_EQ(ev("0 and 5"), "0");
    CHECK_EQ(ev("0 or 7"), "7");
    CHECK_EQ(ev("not 0"), "True");
    CHECK_EQ(ev("1 < 2 < 3"), "True");
    CHECK_EQ(ev("3 > 2 > 2"), "False");
    CHECK_EQ(ev("1 == 1.0"), "True");
    CHECK_EQ(ev("'a' == 1"), "False");
    CHECK_THROWS(ev("'a' < 1"));
}

TEST(math_catalog_types) {
    CHECK_EQ(ev("abs(-3)"), "3");
    CHECK_EQ(ev("abs(-3.0)"), "3.0");
    CHECK_EQ(ev("floor(-3.5)"), "-4");
    CHECK_EQ(ev("int(-3.7)"), "-3");
    CHECK_EQ(ev("min(1, 1.0)"), "1");
    CHECK_EQ(ev("max(2, 2.0, 1)"), "2");
    CHECK_EQ(ev("pow(2, 3)"), "8");
    CHECK_EQ(ev("sqrt(9)"), "3.0");
    CHECK_EQ(ev("\"a\" & 1 & 2.5"), "'a12.5'");
    CHECK_THROWS(ev("sqrt(-1)"));
    CHECK_THROWS(ev("nosuchfn(1)"));
    CHECK_THROWS(ev("1 +"));
    CHECK_THROWS(ev("v_unset + 1"));
}

TEST(arrays_follow_pixil_array_rules) {
    SymbolTable symbols;
    ExprPtr e = compile_expression("v_a[v_i] + v_a[1.9]", symbols);
    Env env(symbols);
    env.ensure_slots();
    auto arr = std::make_shared<PixArray>(3, ArrayType::Numeric);
    CHECK_EQ(py_repr(arr->data[0]), "0");  // unassigned numeric cell is int 0
    array_store(*arr, Value::integer(1), Value::integer(20));
    CHECK_EQ(py_repr(arr->data[1]), "20.0");  // stores float(value)
    env.slots[static_cast<size_t>(symbols.find("v_a"))] = Value::array(arr);
    env.slots[static_cast<size_t>(symbols.find("v_i"))] = Value::integer(1);
    CHECK_EQ(py_repr(eval(*e, env)), "40.0");
    CHECK_THROWS(array_load(*arr, Value::integer(3)));
    CHECK_THROWS(array_load(*arr, Value::integer(-1)));
    CHECK_THROWS(array_store(*arr, Value::integer(0), Value::str("x")));
}
