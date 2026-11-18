#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "consumer/types.h"

namespace pixil::consumer {

Rgb resolve_color(const ColorArg& color, int intensity);
Rgb resolve_color_id(int16_t color_id, int intensity);
ColorArg color_from_id(int16_t color_id);
std::optional<ColorArg> parse_color_token(std::string_view tok);

}  // namespace pixil::consumer
