// draw_batch record packing (wire format of shared/draw_batch_protocol.py and
// shared/mplot_protocol.py). See docs/COMMAND_DIALECT.md §draw_batch.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/value.h"

namespace pixil {

inline constexpr int kPanelWidth = 64;
inline constexpr int kPanelHeight = 64;

enum DrawOp : uint8_t {
    OP_PLOT = 1,
    OP_LINE = 2,
    OP_RECT = 3,
    OP_CIRCLE = 4,
    OP_POLYGON = 5,
    OP_ELLIPSE = 6,
    OP_ARC = 7,
};

class DrawBatch {
public:
    // Appends one record for `cmd` ("plot", "mplot", "draw_line", ...) using the
    // positional argument layout of legacy pack_draw_op (after parameter type
    // conversion). Shapes fully off-panel append nothing. Returns true if appended.
    bool append(std::string_view cmd, const std::vector<Value>& args);

    bool empty() const { return bytes_.empty(); }
    size_t record_count() const { return records_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    void clear() {
        bytes_.clear();
        records_ = 0;
    }
    // `draw_batch("<base64>")` queue command.
    std::string command() const;

private:
    bool append_record(std::string_view cmd, const std::vector<Value>& args);

    std::vector<uint8_t> bytes_;
    size_t records_ = 0;
};

std::string base64_encode(const uint8_t* data, size_t len);

}  // namespace pixil
