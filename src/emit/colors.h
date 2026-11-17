// Color ids shared with the consumer wire protocol (shared/mplot_protocol.py).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "runtime/value.h"

namespace pixil {

// Named color -> negative id (e.g. "red" -> -7); nullopt if unknown. Case-insensitive.
std::optional<int16_t> named_color_id(std::string_view name);

// Legacy get_color_id(normalize_mplot_color(v)): spectral 0..99 or a named color.
// Floats are rounded half-to-even and clamped to 0..99. Throws PixilError(Value).
int16_t color_id_from_value(const Value& v);

}  // namespace pixil
