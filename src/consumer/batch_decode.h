#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "consumer/api.h"

namespace pixil::consumer {

// Decodes draw_batch binary payload and invokes the matching MatrixApi draw calls.
void apply_draw_batch(MatrixApi& api, const std::vector<uint8_t>& bytes);

// Decodes plot_batch payload (concatenated 20-byte mplot records). Does not present;
// caller should maybe_swap once (Python plot_batch behavior).
void apply_plot_batch(MatrixApi& api, const std::vector<uint8_t>& bytes);

// Decodes sprite_batch payload (variable-length show/move/hide records).
void apply_sprite_batch(MatrixApi& api, const std::vector<uint8_t>& bytes);

std::vector<uint8_t> base64_decode(std::string_view in);
std::string base64_encode_for_test(const uint8_t* data, size_t len);

}  // namespace pixil::consumer
