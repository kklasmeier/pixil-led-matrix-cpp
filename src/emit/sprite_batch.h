// sprite_batch record packing (wire format of shared/sprite_batch_protocol.py).
// See docs/COMMAND_DIALECT.md §sprite_batch.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/value.h"

namespace pixil {

class SpriteBatch {
public:
    // Appends one show_sprite / move_sprite / hide_sprite record. Coordinates are
    // banker's-rounded, matching parse_value's int conversion before pack_sprite_op.
    void append(std::string_view cmd, const std::vector<Value>& args);

    bool empty() const { return bytes_.empty(); }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    void clear() { bytes_.clear(); }
    // `sprite_batch("<base64>")` queue command.
    std::string command() const;

private:
    std::vector<uint8_t> bytes_;
};

}  // namespace pixil
