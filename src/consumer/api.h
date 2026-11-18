#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "consumer/background.h"
#include "consumer/burnout.h"
#include "consumer/matrix.h"
#include "consumer/sprite.h"
#include "consumer/text.h"
#include "consumer/types.h"

namespace pixil::consumer {

class MatrixApi {
public:
    explicit MatrixApi(std::unique_ptr<MatrixSurface> surface);

    int width() const { return surface_->width(); }
    int height() const { return surface_->height(); }

    void set_drain_checker(std::function<bool()> fn) { drain_ = std::move(fn); }
    void set_shutdown_checker(std::function<bool()> fn) { shutdown_ = std::move(fn); }
    bool abort_requested() const;

    void execute_command(const std::string& command);

    void clear();
    void blackout();
    void reset_fps();
    void set_fps(double rate);
    void begin_frame(bool preserve);
    void end_frame();
    void rest(double seconds);
    // Executor-pumped burnout: loop + rest call this. No GPIO/thread writers.
    // present=false ticks bookkeeping only (no refresh/swap) — used mid-rest so
    // instant-heavy scripts (Aurora_Reactor) aren't capped by extra composites.
    void pump_fade(double min_interval = 0.033, bool force = false, bool present = true);
    // Tick burnouts while commands are flowing (Python ThreadedBurnoutManager).
    // Presents only for live fades or immediate-mode; Instant clears after
    // end_frame update the back buffer via set_pixel and show on the next swap.
    void pump_fade_while_busy();

    // BurnoutManager pixel access (executor only).
    Rgb peek_drawing(int x, int y) const;
    void draw_for_burnout(int x, int y, Rgb rgb);

    void plot(int x, int y, const ColorArg& color, int intensity,
              std::optional<int> burnout_ms = std::nullopt, BurnoutMode burnout_mode = BurnoutMode::Instant);
    // Write a pixel without presenting (for plot_batch atomic present).
    void stamp_plot(int x, int y, const ColorArg& color, int intensity,
                    std::optional<int> burnout_ms = std::nullopt,
                    BurnoutMode burnout_mode = BurnoutMode::Instant);
    // Text path: stamp RGB already resolved; no present.
    void stamp_rgb(int x, int y, Rgb rgb);
    // Present once if not in frame mode (after multi-pixel stamps).
    void present_if_immediate();

    void draw_line(int x0, int y0, int x1, int y1, const ColorArg& color, int intensity,
                   std::optional<int> burnout_ms = std::nullopt,
                   BurnoutMode burnout_mode = BurnoutMode::Instant);
    void draw_rectangle(int x, int y, int w, int h, const ColorArg& color, int intensity, bool fill,
                        std::optional<int> burnout_ms = std::nullopt,
                        BurnoutMode burnout_mode = BurnoutMode::Instant);
    void draw_circle(int xc, int yc, int r, const ColorArg& color, int intensity, bool fill,
                     std::optional<int> burnout_ms = std::nullopt,
                     BurnoutMode burnout_mode = BurnoutMode::Instant);
    void draw_polygon(int xc, int yc, int r, int sides, const ColorArg& color, int intensity, float rotation,
                      bool fill, std::optional<int> burnout_ms = std::nullopt,
                      BurnoutMode burnout_mode = BurnoutMode::Instant);
    void draw_ellipse(int xc, int yc, int xr, int yr, const ColorArg& color, int intensity, bool fill,
                      float rotation, std::optional<int> burnout_ms = std::nullopt,
                      BurnoutMode burnout_mode = BurnoutMode::Instant);
    void draw_arc(int x1, int y1, int x2, int y2, float bulge, const ColorArg& color, int intensity, bool fill,
                  std::optional<int> burnout_ms = std::nullopt,
                  BurnoutMode burnout_mode = BurnoutMode::Instant);

    void draw_batch_bytes(const std::vector<uint8_t>& bytes);
    void plot_batch_bytes(const std::vector<uint8_t>& bytes);
    void sprite_batch_bytes(const std::vector<uint8_t>& bytes);

    // Sprites
    void define_sprite(const std::string& name, int width, int height);
    void endsprite();
    void sprite_cel(std::optional<int> cel_index);
    void sprite_draw(const std::string& name, const std::string& cmd, const std::vector<std::string>& args);
    void show_sprite(const std::string& name, int x, int y, int instance_id = 0, int z_index = 0,
                     std::optional<int> cel_idx = std::nullopt);
    void hide_sprite(const std::string& name, int instance_id = 0);
    void move_sprite(const std::string& name, int x, int y, int instance_id = 0,
                     std::optional<int> cel_idx = std::nullopt, std::optional<int> z_index = std::nullopt);
    void dispose_sprite(const std::string& name, int instance_id = 0);
    void dispose_all_sprites();

    // Backgrounds
    void set_background(const std::string& sprite_name, int layer = 0, int cel_index = 0);
    void hide_background(std::optional<int> layer = std::nullopt);
    void nudge_background(int dx, int dy, int layer = 0, std::optional<int> cel_index = std::nullopt);
    void set_background_offset(int x, int y, int layer = 0, std::optional<int> cel_index = std::nullopt);

    // Text
    void draw_text(int x, int y, const std::string& text, const std::string& font_name, int font_size,
                   const ColorArg& color, int intensity = 100, const std::string& effect = "NORMAL",
                   const std::string& modifier = {});
    void clear_text(int x, int y);

    std::string fingerprint() const;
    bool frame_mode() const { return frame_mode_; }
    // Monotonic count of end_frame + immediate maybe_swap presents (not fade pumps).
    // Atomic: control-plane Q sampler may read from another thread.
    uint32_t present_count() const { return present_count_.load(std::memory_order_relaxed); }

    void cleanup();

private:
    void draw_pixel(int x, int y, Rgb rgb);
    void maybe_swap();
    void pace_after_present();
    void note_present();  // end_frame / maybe_swap only
    void blit_for_display();
    void blit_sprites_onto_surface();
    void refresh_display();  // immediate-mode full composite
    void present_sprite_update();
    void register_burnout(std::vector<std::pair<int, int>> points, Rgb rgb, std::optional<int> burnout_ms,
                          BurnoutMode mode);

    std::unique_ptr<MatrixSurface> surface_;
    Buffer drawing_{};
    bool frame_mode_ = false;
    bool preserve_ = false;
    double target_fps_ = 0.0;
    double frame_interval_ = 0.0;
    double last_present_ = 0.0;
    double last_fade_pump_ = 0.0;
    std::atomic<uint32_t> present_count_{0};
    // True after end_frame; false after immediate maybe_swap. rest() uses this to
    // skip an extra composite when the next framed present will blit the buffer.
    bool last_present_was_end_frame_ = false;
    std::vector<std::tuple<int, int, uint8_t, uint8_t, uint8_t>> preserve_pixels_;
    std::function<bool()> drain_;
    std::function<bool()> shutdown_;

    SpriteManager sprites_;
    BackgroundManager backgrounds_;
    TextRenderer text_;
    BurnoutManager burnout_;
};

}  // namespace pixil::consumer
