#include "emit/colors.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace pixil {

namespace {

struct NamedColor {
    const char* name;
    int16_t id;
};

// Order and ids must match NAMED_COLOR_TO_ID in shared/mplot_protocol.py.
constexpr NamedColor kNamedColors[] = {
    {"black", -1},          {"white", -2},          {"gray", -3},
    {"light_gray", -4},     {"dark_gray", -5},      {"silver", -6},
    {"red", -7},            {"crimson", -8},        {"maroon", -9},
    {"rose", -10},          {"pink", -11},          {"salmon", -12},
    {"coral", -13},         {"brown", -14},         {"standard_brown", -15},
    {"dark_brown", -16},    {"wood_brown", -17},    {"tan", -18},
    {"orange", -19},        {"gold", -20},          {"peach", -21},
    {"bronze", -22},        {"yellow", -23},        {"lime", -24},
    {"green", -25},         {"olive", -26},         {"spring_green", -27},
    {"forest_green", -28},  {"mint", -29},          {"teal", -30},
    {"turquoise", -31},     {"cyan", -32},          {"sky_blue", -33},
    {"azure", -34},         {"blue", -35},          {"navy", -36},
    {"royal_blue", -37},    {"ocean_blue", -38},    {"indigo", -39},
    {"purple", -40},        {"violet", -41},        {"magenta", -42},
    {"lavender", -43},      {"transparent", -44},
};

bool all_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

}  // namespace

std::optional<int16_t> named_color_id(std::string_view name) {
    std::string lower(name);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& nc : kNamedColors) {
        if (lower == nc.name) return nc.id;
    }
    return std::nullopt;
}

int16_t color_id_from_value(const Value& v) {
    if (v.is_float()) {
        double r = std::nearbyint(v.as_float());
        return static_cast<int16_t>(std::clamp(r, 0.0, 99.0));
    }
    if (v.is_integral()) {
        int64_t n = v.integral();
        if (n < 0 || n > 99) {
            throw PixilError(ErrorKind::Value, "Spectral color must be 0-99, got " + std::to_string(n));
        }
        return static_cast<int16_t>(n);
    }
    if (v.is_str()) {
        const std::string& s = v.as_str();
        if (all_digits(s)) {
            size_t first = s.find_first_not_of('0');
            int n = first == std::string::npos ? 0 : (s.size() - first > 3 ? 1000 : std::stoi(s.substr(first)));
            if (n > 99) throw PixilError(ErrorKind::Value, "Spectral color must be 0-99, got " + s);
            return static_cast<int16_t>(n);
        }
        if (auto id = named_color_id(s)) return *id;
        throw PixilError(ErrorKind::Value, "Unknown named color: " + s);
    }
    throw PixilError(ErrorKind::Value, std::string("Color must be string or int, got ") + v.type_name());
}

}  // namespace pixil
