#include "consumer/matrix.h"

#include <stdexcept>
#include <vector>

#if defined(PIXIL_HAS_RGBMATRIX)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "led-matrix.h"
#pragma GCC diagnostic pop
#endif

namespace pixil::consumer {
namespace {

class HeadlessMatrix final : public MatrixSurface {
public:
    HeadlessMatrix(int w, int h) : w_(w), h_(h), front_(static_cast<size_t>(w * h)), back_(front_) {}

    int width() const override { return w_; }
    int height() const override { return h_; }

    void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) override {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
        back_[static_cast<size_t>(y) * w_ + static_cast<size_t>(x)] = {r, g, b};
    }

    void fill(uint8_t r, uint8_t g, uint8_t b) override {
        Rgb c{r, g, b};
        for (auto& p : back_) p = c;
    }

    void blit(const Buffer& src) override {
        for (int y = 0; y < h_; ++y) {
            for (int x = 0; x < w_; ++x) {
                Rgb p = at(src, x, y);
                if (is_transparent(p)) p = {0, 0, 0};
                set_pixel(x, y, p.r, p.g, p.b);
            }
        }
    }

    void swap() override { front_.swap(back_); }

    void clear() override {
        fill(0, 0, 0);
        swap();
        fill(0, 0, 0);
    }

private:
    int w_;
    int h_;
    std::vector<Rgb> front_;
    std::vector<Rgb> back_;
};

#if defined(PIXIL_HAS_RGBMATRIX)
class HardwareMatrix final : public MatrixSurface {
public:
    HardwareMatrix() {
        rgb_matrix::RGBMatrix::Options opts;
        opts.rows = kHeight;
        opts.cols = kWidth;
        opts.hardware_mapping = "adafruit-hat";
        opts.scan_mode = 1;
        opts.pwm_bits = 11;
        opts.brightness = 100;
        opts.limit_refresh_rate_hz = 0;
        rgb_matrix::RuntimeOptions rt;
        rt.gpio_slowdown = 3;
        matrix_.reset(rgb_matrix::RGBMatrix::CreateFromOptions(opts, rt));
        if (!matrix_) throw std::runtime_error("RGBMatrix::CreateFromOptions failed");
        canvas_ = matrix_->CreateFrameCanvas();
        if (!canvas_) throw std::runtime_error("CreateFrameCanvas failed");
    }

    ~HardwareMatrix() override {
        if (matrix_) matrix_->Clear();
    }

    int width() const override { return matrix_->width(); }
    int height() const override { return matrix_->height(); }

    void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) override {
        canvas_->SetPixel(x, y, r, g, b);
    }

    void fill(uint8_t r, uint8_t g, uint8_t b) override { canvas_->Fill(r, g, b); }

    void blit(const Buffer& src) override {
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                Rgb p = at(src, x, y);
                if (is_transparent(p)) p = {0, 0, 0};
                canvas_->SetPixel(x, y, p.r, p.g, p.b);
            }
        }
    }

    void swap() override { canvas_ = matrix_->SwapOnVSync(canvas_); }

    void clear() override {
        matrix_->Clear();
        canvas_->Fill(0, 0, 0);
        canvas_ = matrix_->SwapOnVSync(canvas_);
        canvas_->Fill(0, 0, 0);
    }

private:
    std::unique_ptr<rgb_matrix::RGBMatrix> matrix_;
    rgb_matrix::FrameCanvas* canvas_ = nullptr;
};
#endif

}  // namespace

std::unique_ptr<MatrixSurface> make_headless_matrix(int w, int h) {
    return std::make_unique<HeadlessMatrix>(w, h);
}

std::unique_ptr<MatrixSurface> make_hardware_matrix() {
#if defined(PIXIL_HAS_RGBMATRIX)
    return std::make_unique<HardwareMatrix>();
#else
    throw std::runtime_error("pixil-consumer built without librgbmatrix; use --headless");
#endif
}

}  // namespace pixil::consumer
