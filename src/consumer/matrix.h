#pragma once

#include <memory>

#include "consumer/types.h"

namespace pixil::consumer {

// Double-buffered panel surface. Only the executor thread may call these.
class MatrixSurface {
public:
    virtual ~MatrixSurface() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void fill(uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void blit(const Buffer& src) = 0;  // transparent → black
    virtual void swap() = 0;
    virtual void clear() = 0;
};

std::unique_ptr<MatrixSurface> make_headless_matrix(int w = kWidth, int h = kHeight);

// Opens real GPIO hardware. Throws std::runtime_error on failure.
// Only call when not --headless and running as root on the Pi 4.
std::unique_ptr<MatrixSurface> make_hardware_matrix();

}  // namespace pixil::consumer
