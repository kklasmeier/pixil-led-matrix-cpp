// pixil-expr: evaluate expression/condition cases with the native evaluator and
// print one JSON result per line (Tier 0 oracle harness, see tools/expr_crosscheck.py).
//
// Input: JSON Lines, one case per line:
//   {"id": "...", "kind": "expr"|"cond", "src": "...",
//    "vars": {"v_x": 10, ...},
//    "arrays": {"v_a": {"type": "numeric"|"string", "size": N, "data": [...]}},
//    "seed": 42}
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "runtime/eval.h"
#include "runtime/python_ops.h"
#include "util/json.h"

using namespace pixil;

namespace {

Value value_from_json(const json::Node& n) {
    switch (n.type) {
        case json::Node::Type::Null: return Value::none();
        case json::Node::Type::Bool: return Value::boolean(n.b);
        case json::Node::Type::Int: return Value::integer(n.i);
        case json::Node::Type::Float: return Value::real(n.d);
        case json::Node::Type::String: return Value::str(n.s);
        default: throw std::runtime_error("unsupported JSON value for a variable");
    }
}

std::string run_case(const json::Node& c) {
    const json::Node* id = c.get("id");
    const json::Node* kind = c.get("kind");
    const json::Node* src = c.get("src");
    if (!id || !src) throw std::runtime_error("case needs id and src");
    std::string out = "{\"id\": " + json::quote(id->s);
    try {
        SymbolTable symbols;
        ExprPtr expr = compile_expression(src->s, symbols, 1);
        // Interning variables after compilation keeps slots for names the
        // expression never mentions; that is harmless.
        std::vector<std::pair<int, Value>> inits;
        if (const json::Node* vars = c.get("vars")) {
            for (const auto& [name, v] : vars->fields) {
                inits.emplace_back(symbols.intern(name), value_from_json(v));
            }
        }
        if (const json::Node* arrays = c.get("arrays")) {
            for (const auto& [name, spec] : arrays->fields) {
                const json::Node* type = spec.get("type");
                const json::Node* size = spec.get("size");
                const json::Node* data = spec.get("data");
                ArrayType t = (type && type->s == "string") ? ArrayType::String : ArrayType::Numeric;
                size_t n = size ? static_cast<size_t>(size->i) : (data ? data->items.size() : 0);
                auto arr = std::make_shared<PixArray>(n, t);
                if (data) {
                    for (size_t i = 0; i < data->items.size(); ++i) {
                        array_store(*arr, Value::integer(static_cast<int64_t>(i)),
                                    value_from_json(data->items[i]));
                    }
                }
                inits.emplace_back(symbols.intern(name), Value::array(arr));
            }
        }
        Env env(symbols);
        env.ensure_slots();
        for (auto& [slot, v] : inits) env.slots[static_cast<size_t>(slot)] = v;
        if (const json::Node* seed = c.get("seed")) env.rng.seed(seed->i);
        Value result = eval(*expr, env);
        if (kind && kind->s == "cond") result = Value::boolean(truthy(result));
        out += ", \"ok\": true, \"type\": " + json::quote(result.type_name()) +
               ", \"repr\": " + json::quote(py_repr(result)) + "}";
    } catch (const PixilError& e) {
        out += ", \"ok\": false, \"error_type\": " + json::quote(error_kind_name(e.kind())) +
               ", \"error\": " + json::quote(e.what()) + "}";
    }
    return out;
}

int usage() {
    std::cerr << "usage: pixil-expr [--inject-fault=c_mod] [CASES.jsonl]\n"
                 "Evaluates Pixil expression cases with the native runtime (reads stdin if no file).\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") return usage(), 0;
        if (a == "--inject-fault=c_mod") {
            set_fault_injection(Fault::CMod);
        } else if (a.rfind("--", 0) == 0) {
            return usage();
        } else {
            path = a;
        }
    }
    std::ifstream file;
    std::istream* in = &std::cin;
    if (!path.empty()) {
        file.open(path);
        if (!file) {
            std::cerr << "pixil-expr: cannot open " << path << "\n";
            return 1;
        }
        in = &file;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(*in, line)) {
        ++lineno;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        try {
            std::cout << run_case(json::parse(line)) << "\n";
        } catch (const std::exception& e) {
            std::cerr << "pixil-expr: bad case on line " << lineno << ": " << e.what() << "\n";
            return 1;
        }
    }
    return 0;
}
