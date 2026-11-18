#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pixil::consumer {

// Returns lowercase hex digest (64 chars) of data.
std::string sha256_hex(const uint8_t* data, size_t len);
inline std::string sha256_hex(std::string_view s) {
    return sha256_hex(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

}  // namespace pixil::consumer
