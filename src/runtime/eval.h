// Compile-once expression evaluation: resolve() binds names to slots and
// builtins a single time; eval() walks the annotated tree without string lookups.
#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "runtime/py_random.h"
#include "runtime/value.h"
#include "syntax/ast.h"

namespace pixil {

class SymbolTable {
public:
    int intern(const std::string& name);
    int find(const std::string& name) const;  // -1 if absent
    const std::string& name_of(int slot) const { return names_[static_cast<size_t>(slot)]; }
    size_t size() const { return names_.size(); }

private:
    std::unordered_map<std::string, int> slots_;
    std::vector<std::string> names_;
};

struct Env {
    explicit Env(SymbolTable& table) : symbols(table) {}
    SymbolTable& symbols;
    std::vector<Value> slots;
    PyRandom rng;
    std::chrono::steady_clock::time_point script_start = std::chrono::steady_clock::now();
    bool test_mode = false;

    void ensure_slots() { slots.resize(symbols.size()); }
};

// Annotates `e` in place. v_* names are interned into `symbols`.
void resolve(Expr& e, SymbolTable& symbols);

Value eval(const Expr& e, Env& env);

// Convenience for tools/tests: parse + resolve.
ExprPtr compile_expression(std::string_view src, SymbolTable& symbols, int line = 0);

// Element store with legacy PixilArray coercions (numeric -> float, string -> str).
void array_store(PixArray& arr, const Value& index, const Value& value);
const Value& array_load(const PixArray& arr, const Value& index);

}  // namespace pixil
