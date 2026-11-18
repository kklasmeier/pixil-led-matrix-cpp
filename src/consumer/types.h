// Shared consumer types (Phase 2 C1).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <variant>

namespace pixil::consumer {

inline constexpr int kWidth = 64;
inline constexpr int kHeight = 64;
inline constexpr size_t kInboxCapacity = 5000;

// Drawing-buffer transparent sentinel (legacy TRANSPARENT_COLOR).
inline constexpr uint8_t kTransparentR = 0;
inline constexpr uint8_t kTransparentG = 0;
inline constexpr uint8_t kTransparentB = 1;

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

inline bool is_transparent(Rgb c) {
    return c.r == kTransparentR && c.g == kTransparentG && c.b == kTransparentB;
}

// Color as the dialect carries it: named word or spectral 0..99.
struct ColorArg {
    bool named = false;
    std::string name;
    int spectral = 0;
};

using Pixel = Rgb;
using Buffer = std::array<Pixel, kWidth * kHeight>;

inline Pixel& at(Buffer& buf, int x, int y) { return buf[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)]; }
inline const Pixel& at(const Buffer& buf, int x, int y) {
    return buf[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)];
}

}  // namespace pixil::consumer
