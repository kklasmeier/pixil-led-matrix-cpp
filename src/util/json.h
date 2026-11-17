// Small JSON reader/writer for tool inputs (case files, goldens). Not used at runtime.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pixil::json {

struct Node {
    enum class Type { Null, Bool, Int, Float, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    std::string s;
    std::vector<Node> items;
    std::vector<std::pair<std::string, Node>> fields;  // insertion order

    const Node* get(const std::string& key) const;
    bool is(Type t) const { return type == t; }
};

// Throws std::runtime_error on malformed input.
Node parse(std::string_view text);

std::string quote(std::string_view s);

}  // namespace pixil::json
