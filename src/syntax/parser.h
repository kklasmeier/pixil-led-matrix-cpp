#pragma once

#include <string_view>

#include "syntax/ast.h"

namespace pixil {

// Parse a complete expression (grammar in docs/LANGUAGE.md §Expression grammar).
// Throws PixilError(Syntax) on malformed input or trailing tokens.
ExprPtr parse_expression(std::string_view src, int line = 0);

}  // namespace pixil
