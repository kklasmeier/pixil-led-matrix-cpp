// Minimal self-registering test harness (no external dependency on the Pi).
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace pixil_test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void report(const char* file, int line, const std::string& msg) {
    ++failures();
    std::cerr << file << ":" << line << ": FAILED: " << msg << "\n";
}

}  // namespace pixil_test

#define PIXIL_CONCAT2(a, b) a##b
#define PIXIL_CONCAT(a, b) PIXIL_CONCAT2(a, b)
#define TEST(name)                                                                   \
    static void name();                                                              \
    static ::pixil_test::Registrar PIXIL_CONCAT(registrar_, name)(#name, name);      \
    static void name()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) ::pixil_test::report(__FILE__, __LINE__, "CHECK(" #cond ")");    \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        auto&& va_ = (a);                                                            \
        auto&& vb_ = (b);                                                            \
        if (!(va_ == vb_)) {                                                         \
            std::ostringstream os_;                                                  \
            os_ << #a " == " #b " (" << va_ << " vs " << vb_ << ")";                 \
            ::pixil_test::report(__FILE__, __LINE__, os_.str());                     \
        }                                                                            \
    } while (0)

#define CHECK_THROWS(expr)                                                           \
    do {                                                                             \
        bool threw_ = false;                                                         \
        try {                                                                        \
            (void)(expr);                                                            \
        } catch (...) {                                                              \
            threw_ = true;                                                           \
        }                                                                            \
        if (!threw_) ::pixil_test::report(__FILE__, __LINE__, "expected throw: " #expr); \
    } while (0)
