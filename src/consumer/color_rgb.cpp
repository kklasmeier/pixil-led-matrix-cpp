#include "consumer/color_rgb.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace pixil::consumer {
namespace {

struct Named {
    const char* name;
    int16_t id;
    Rgb rgb;
};

// RGB values from legacy rgb_matrix_lib/utils.py NAMED_COLORS; ids from mplot_protocol.
constexpr Named kNamed[] = {
    {"black", -1, {0, 0, 0}},
    {"white", -2, {255, 255, 255}},
    {"gray", -3, {128, 128, 128}},
    {"light_gray", -4, {192, 192, 192}},
    {"dark_gray", -5, {64, 64, 64}},
    {"silver", -6, {192, 192, 192}},
    {"red", -7, {255, 0, 0}},
    {"crimson", -8, {220, 20, 60}},
    {"maroon", -9, {128, 0, 0}},
    {"rose", -10, {255, 0, 128}},
    {"pink", -11, {255, 0, 85}},
    {"salmon", -12, {255, 85, 85}},
    {"coral", -13, {255, 64, 64}},
    {"brown", -14, {165, 42, 42}},
    {"standard_brown", -15, {139, 69, 19}},
    {"dark_brown", -16, {101, 67, 33}},
    {"wood_brown", -17, {133, 94, 66}},
    {"tan", -18, {210, 180, 140}},
    {"orange", -19, {255, 85, 0}},
    {"gold", -20, {255, 170, 0}},
    {"peach", -21, {255, 218, 185}},
    {"bronze", -22, {205, 127, 50}},
    {"yellow", -23, {255, 255, 0}},
    {"lime", -24, {85, 255, 0}},
    {"green", -25, {0, 255, 0}},
    {"olive", -26, {128, 128, 0}},
    {"spring_green", -27, {0, 255, 64}},
    {"forest_green", -28, {0, 212, 42}},
    {"mint", -29, {62, 255, 192}},
    {"teal", -30, {0, 255, 128}},
    {"turquoise", -31, {0, 255, 212}},
    {"cyan", -32, {0, 255, 255}},
    {"sky_blue", -33, {0, 128, 255}},
    {"azure", -34, {42, 212, 255}},
    {"blue", -35, {0, 0, 255}},
    {"navy", -36, {0, 0, 128}},
    {"royal_blue", -37, {65, 105, 225}},
    {"ocean_blue", -38, {0, 85, 255}},
    {"indigo", -39, {85, 0, 255}},
    {"purple", -40, {128, 0, 255}},
    {"violet", -41, {212, 0, 255}},
    {"magenta", -42, {255, 0, 255}},
    {"lavender", -43, {212, 0, 212}},
    {"transparent", -44, {kTransparentR, kTransparentG, kTransparentB}},
};

Rgb hsv_to_rgb(double h, double s, double v) {
    double h60 = h / 60.0;
    double h60f = std::floor(h60);
    int hi = static_cast<int>(h60f) % 6;
    double f = h60 - h60f;
    double p = v * (1 - s);
    double q = v * (1 - f * s);
    double t = v * (1 - (1 - f) * s);
    double r = 0, g = 0, b = 0;
    switch (hi) {
        case 0:
            r = v;
            g = t;
            b = p;
            break;
        case 1:
            r = q;
            g = v;
            b = p;
            break;
        case 2:
            r = p;
            g = v;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = v;
            break;
        case 4:
            r = t;
            g = p;
            b = v;
            break;
        default:
            r = v;
            g = p;
            b = q;
            break;
    }
    return {static_cast<uint8_t>(r * 255), static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255)};
}

Rgb spectral(int i) {
    i = std::clamp(i, 0, 99);
    double hue = (i * 359.9) / 99.0;
    return hsv_to_rgb(hue, 1.0, 1.0);
}

Rgb scale(Rgb base, int intensity) {
    intensity = std::clamp(intensity, 0, 100);
    double s = intensity / 100.0;
    return {static_cast<uint8_t>(base.r * s), static_cast<uint8_t>(base.g * s), static_cast<uint8_t>(base.b * s)};
}

const Named* find_named(std::string_view name) {
    std::string lower(name);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& n : kNamed) {
        if (lower == n.name) return &n;
    }
    return nullptr;
}

}  // namespace

Rgb resolve_color(const ColorArg& color, int intensity) {
    if (color.named) {
        if (const Named* n = find_named(color.name)) return scale(n->rgb, intensity);
        return scale({255, 255, 255}, intensity);  // legacy unknown → white
    }
    return scale(spectral(color.spectral), intensity);
}

Rgb resolve_color_id(int16_t color_id, int intensity) { return resolve_color(color_from_id(color_id), intensity); }

ColorArg color_from_id(int16_t color_id) {
    if (color_id >= 0 && color_id <= 99) return ColorArg{false, {}, color_id};
    for (const auto& n : kNamed) {
        if (n.id == color_id) return ColorArg{true, n.name, 0};
    }
    return ColorArg{true, "white", 0};
}

std::optional<ColorArg> parse_color_token(std::string_view tok) {
    if (tok.empty()) return std::nullopt;
    bool digits = true;
    for (char c : tok) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            digits = false;
            break;
        }
    }
    if (digits) {
        int n = std::stoi(std::string(tok));
        if (n < 0 || n > 99) return std::nullopt;
        return ColorArg{false, {}, n};
    }
    if (find_named(tok)) return ColorArg{true, std::string(tok), 0};
    return std::nullopt;
}

}  // namespace pixil::consumer
