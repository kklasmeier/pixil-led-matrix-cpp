#include "consumer/api.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <tuple>

#include "consumer/batch_decode.h"
#include "consumer/color_rgb.h"
#include "consumer/parse.h"
#include "consumer/sha256.h"
#include "consumer/shapes.h"

namespace pixil::consumer {
namespace {

double now_sec() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

}  // namespace

MatrixApi::MatrixApi(std::unique_ptr<MatrixSurface> surface)
    : surface_(std::move(surface)), backgrounds_(sprites_), text_(*this), burnout_(*this) {
    ensure_layer(0);
}

void MatrixApi::fill_buffer_transparent(Buffer& buf) {
    for (auto& p : buf) p = {kTransparentR, kTransparentG, kTransparentB};
}

void MatrixApi::ensure_layer(uint16_t layer) {
    auto [it, inserted] = layers_.try_emplace(layer);
    if (inserted) fill_buffer_transparent(it->second.pixels);
}

Buffer& MatrixApi::current_buffer() { return layer_buffer(current_layer_); }
const Buffer& MatrixApi::current_buffer() const { return layer_buffer(current_layer_); }

Buffer& MatrixApi::layer_buffer(uint16_t layer) {
    ensure_layer(layer);
    return layers_[layer].pixels;
}

const Buffer& MatrixApi::layer_buffer(uint16_t layer) const {
    auto it = layers_.find(layer);
    if (it == layers_.end()) {
        static Buffer empty = [] {
            Buffer b{};
            for (auto& p : b) p = {kTransparentR, kTransparentG, kTransparentB};
            return b;
        }();
        return empty;
    }
    return it->second.pixels;
}

void MatrixApi::set_layer(int layer) {
    if (layer < 0 || layer > 65535) throw std::runtime_error("set_layer layer must be 0..65535");
    current_layer_ = static_cast<uint16_t>(layer);
    ensure_layer(current_layer_);
}

void MatrixApi::clear_layer(int layer) {
    if (layer < 0 || layer > 65535) throw std::runtime_error("clear_layer layer must be 0..65535");
    auto id = static_cast<uint16_t>(layer);
    ensure_layer(id);
    fill_buffer_transparent(layers_[id].pixels);
    burnout_.clear_layer(id);
}

void MatrixApi::delete_layer(int layer) {
    if (layer < 0 || layer > 65535) throw std::runtime_error("delete_layer layer must be 0..65535");
    auto id = static_cast<uint16_t>(layer);
    if (!layers_.count(id)) return;
    sprites_.hide_sprites_on_layer(id);
    burnout_.clear_layer(id);
    layers_.erase(id);
    if (current_layer_ == id) {
        current_layer_ = 0;
        ensure_layer(0);
    } else if (layers_.empty()) {
        ensure_layer(0);
    }
}

void MatrixApi::hide_layer(int layer) {
    if (layer < 0 || layer > 65535) throw std::runtime_error("hide_layer layer must be 0..65535");
    auto id = static_cast<uint16_t>(layer);
    auto it = layers_.find(id);
    if (it == layers_.end()) return;
    it->second.visible = false;
}

void MatrixApi::show_layer(int layer) {
    if (layer < 0 || layer > 65535) throw std::runtime_error("show_layer layer must be 0..65535");
    auto id = static_cast<uint16_t>(layer);
    ensure_layer(id);
    layers_[id].visible = true;
}

void MatrixApi::reorder_layer(int from_layer, int to_layer) {
    if (from_layer < 0 || from_layer > 65535 || to_layer < 0 || to_layer > 65535) {
        throw std::runtime_error("reorder_layer layers must be 0..65535");
    }
    auto from = static_cast<uint16_t>(from_layer);
    auto to = static_cast<uint16_t>(to_layer);
    if (from == to) return;
    auto it = layers_.find(from);
    if (it == layers_.end()) return;  // missing from → no-op

    if (layers_.count(to)) {
        // Overwrite: discard old to content; hide its sprites; end its burnouts.
        sprites_.hide_sprites_on_layer(to);
        burnout_.clear_layer(to);
        layers_.erase(to);
    }

    DrawLayer moved = std::move(it->second);
    layers_.erase(it);
    layers_.emplace(to, std::move(moved));
    sprites_.reassign_layer(from, to);
    burnout_.rekey_layer(from, to);
    if (current_layer_ == from) current_layer_ = to;
}

void MatrixApi::dispose_all_layers() {
    layers_.clear();
    current_layer_ = 0;
    ensure_layer(0);
    burnout_.clear_all();
}

void MatrixApi::set_sprite_layer(const std::string& name, int layer, int instance_id) {
    if (layer < 0 || layer > 65535) throw std::runtime_error("set_sprite_layer layer must be 0..65535");
    auto id = static_cast<uint16_t>(layer);
    ensure_layer(id);  // auto-create empty layer if missing
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (!instance) return;
    instance->layer = id;
    present_sprite_update();
}

void MatrixApi::set_sprite_z(const std::string& name, int z_index, int instance_id) {
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (!instance) return;
    instance->z_index = z_index;
    present_sprite_update();
}

Rgb MatrixApi::peek_drawing(int x, int y) const { return peek_layer(current_layer_, x, y); }

Rgb MatrixApi::peek_layer(uint16_t layer, int x, int y) const {
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return {0, 0, 0};
    return at(layer_buffer(layer), x, y);
}

void MatrixApi::draw_for_burnout(uint16_t layer, int x, int y, Rgb rgb) {
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
    at(layer_buffer(layer), x, y) = rgb;
    if (!frame_mode_ || preserve_) {
        // Immediate/preserve dual-write only for the current layer (legacy path).
        if (layer == current_layer_) {
            surface_->set_pixel(x, y, rgb.r, rgb.g, rgb.b);
            preserve_pixels_.emplace_back(x, y, rgb.r, rgb.g, rgb.b);
        }
    }
}

void MatrixApi::register_burnout(std::vector<std::pair<int, int>> points, Rgb rgb,
                                 std::optional<int> burnout_ms, BurnoutMode mode) {
    if (!burnout_ms.has_value() || *burnout_ms < 0 || points.empty()) return;
    std::vector<Rgb> colors(points.size(), rgb);
    burnout_.add(std::move(points), std::move(colors), *burnout_ms, mode, now_sec(), current_layer_);
}

bool MatrixApi::abort_requested() const {
    if (shutdown_ && shutdown_()) return true;
    if (drain_ && drain_()) return true;
    return false;
}

void MatrixApi::draw_pixel(int x, int y, Rgb rgb) {
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
    at(current_buffer(), x, y) = rgb;
    if (!frame_mode_ || preserve_) {
        surface_->set_pixel(x, y, rgb.r, rgb.g, rgb.b);
        preserve_pixels_.emplace_back(x, y, rgb.r, rgb.g, rgb.b);
    }
}

void MatrixApi::maybe_swap() {
    if (frame_mode_) return;
    surface_->swap();
    for (auto [x, y, r, g, b] : preserve_pixels_) surface_->set_pixel(x, y, r, g, b);
    preserve_pixels_.clear();
    note_present();
    last_present_was_end_frame_ = false;
    pace_after_present();
}

void MatrixApi::note_present() { present_count_.fetch_add(1, std::memory_order_relaxed); }

void MatrixApi::pace_after_present() {
    if (frame_interval_ <= 0) return;
    double now = now_sec();
    if (last_present_ > 0) {
        double sleep_for = frame_interval_ - (now - last_present_);
        if (sleep_for > 0) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(sleep_for);
            while (std::chrono::steady_clock::now() < deadline) {
                if (abort_requested()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    last_present_ = now_sec();
}

void MatrixApi::overlay_layer_onto(Buffer& dst, const Buffer& src) const {
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            Rgb d = at(src, x, y);
            if (!is_transparent(d)) at(dst, x, y) = d;
        }
    }
}

void MatrixApi::compose_layers_onto(Buffer& dst) const {
    // Fast path: single layer 0 — same cost profile as the old single buffer.
    if (layers_.size() == 1) {
        auto it = layers_.begin();
        if (it->first == 0 && it->second.visible) {
            overlay_layer_onto(dst, it->second.pixels);
            return;
        }
    }
    for (const auto& [id, layer] : layers_) {
        if (!layer.visible) continue;
        overlay_layer_onto(dst, layer.pixels);
    }
}

void MatrixApi::blit_for_display() {
    if (layers_.size() == 1 && layers_.begin()->first == 0) {
        surface_->blit(layers_.begin()->second.pixels);
        return;
    }
    Buffer work{};
    for (auto& p : work) p = {0, 0, 0};
    compose_layers_onto(work);
    surface_->blit(work);
}

void MatrixApi::blit_sprites_for_layer(uint16_t layer) {
    for (const auto& [name, id] : sprites_.iter_draw_order_for_layer(layer)) {
        SpriteInstance* inst = sprites_.get_instance(name, id);
        if (!inst || !inst->visible || !inst->template_) continue;
        const SpriteCel& cel = inst->current();
        int x0 = inst->x, y0 = inst->y;
        int sw = inst->width(), sh = inst->height();
        for (int sy = 0; sy < sh; ++sy) {
            for (int sx = 0; sx < sw; ++sx) {
                size_t i = static_cast<size_t>(sy) * static_cast<size_t>(sw) + static_cast<size_t>(sx);
                Rgb c = cel.pixels[i];
                if (is_transparent(c)) continue;
                uint8_t inten = cel.intensity[i];
                if (inten != 100) {
                    c.r = static_cast<uint8_t>(static_cast<uint16_t>(c.r) * inten / 100);
                    c.g = static_cast<uint8_t>(static_cast<uint16_t>(c.g) * inten / 100);
                    c.b = static_cast<uint8_t>(static_cast<uint16_t>(c.b) * inten / 100);
                }
                int dx = x0 + sx, dy = y0 + sy;
                if (dx < 0 || dy < 0 || dx >= kWidth || dy >= kHeight) continue;
                surface_->set_pixel(dx, dy, c.r, c.g, c.b);
            }
        }
    }
}

void MatrixApi::blit_sprites_onto_surface() {
    // Paint sprites interleaved with layers: for each layer, drawing was already
    // composited into the surface buffer; sprites on that layer go next.
    for (const auto& [id, layer] : layers_) {
        if (!layer.visible) continue;
        blit_sprites_for_layer(id);
    }
    // Sprites on layers that have no draw buffer yet (auto via show only) —
    // Phase 1 show assigns current layer which ensure_layer'd; still cover any
    // sprite layers missing from the map by walking all instances via full order.
    // (create_instance always lands on an ensured layer via show_sprite.)
}

void MatrixApi::refresh_display() {
    // Paint composite onto the current (back) canvas; caller may swap.
    // Fast path: one layer (catalog default) matches the pre-layers composite.
    if (layers_.size() == 1 && layers_.begin()->first == 0) {
        const Buffer& drawing = layers_.begin()->second.pixels;
        if (backgrounds_.has_background()) {
            Buffer viewport = backgrounds_.get_viewport(kWidth, kHeight);
            overlay_layer_onto(viewport, drawing);
            surface_->blit(viewport);
        } else {
            surface_->blit(drawing);
        }
        blit_sprites_for_layer(0);
        return;
    }
    // Multi-layer: for each layer ascending — pixels, then that layer's sprites.
    Buffer work{};
    if (backgrounds_.has_background()) {
        work = backgrounds_.get_viewport(kWidth, kHeight);
    } else {
        for (auto& p : work) p = {0, 0, 0};
    }
    surface_->blit(work);
    for (const auto& [id, layer] : layers_) {
        if (!layer.visible) continue;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                Rgb d = at(layer.pixels, x, y);
                if (is_transparent(d)) continue;
                surface_->set_pixel(x, y, d.r, d.g, d.b);
            }
        }
        blit_sprites_for_layer(id);
    }
}

void MatrixApi::present_sprite_update() {
    if (frame_mode_) return;
    refresh_display();
    surface_->swap();
    for (auto [x, y, r, g, b] : preserve_pixels_) surface_->set_pixel(x, y, r, g, b);
    preserve_pixels_.clear();
    pace_after_present();
}

void MatrixApi::set_fps(double rate) {
    if (rate <= 0) {
        target_fps_ = 0;
        frame_interval_ = 0;
    } else {
        target_fps_ = rate;
        frame_interval_ = 1.0 / rate;
    }
}

void MatrixApi::reset_fps() { set_fps(0); }

void MatrixApi::begin_frame(bool preserve) {
    frame_mode_ = true;
    preserve_ = preserve;
    current_layer_ = 0;
    if (!preserve) {
        for (auto& [id, layer] : layers_) fill_buffer_transparent(layer.pixels);
    }
}

void MatrixApi::end_frame() {
    if (!frame_mode_) return;

    const bool single0 = layers_.size() == 1 && layers_.begin()->first == 0;

    if (backgrounds_.has_background()) {
        Buffer viewport = backgrounds_.get_viewport(kWidth, kHeight);
        if (single0) {
            overlay_layer_onto(viewport, layers_.begin()->second.pixels);
            surface_->blit(viewport);
            blit_sprites_for_layer(0);
            surface_->swap();
            surface_->blit(viewport);
        } else {
            refresh_display();
            // Rebuild viewport+layers without sprites for the seeded back buffer.
            Buffer seed = backgrounds_.get_viewport(kWidth, kHeight);
            compose_layers_onto(seed);
            surface_->swap();
            surface_->blit(seed);
        }
        if (preserve_) {
            for (auto [x, y, r, g, b] : preserve_pixels_) surface_->set_pixel(x, y, r, g, b);
        }
    } else {
        if (!preserve_) {
            if (single0) {
                surface_->blit(layers_.begin()->second.pixels);
                blit_sprites_for_layer(0);
            } else {
                refresh_display();
            }
        } else if (!single0) {
            refresh_display();
        } else {
            blit_sprites_for_layer(0);
        }
        surface_->swap();
        if (preserve_) {
            blit_for_display();
        } else {
            surface_->fill(0, 0, 0);
        }
    }

    preserve_pixels_.clear();
    frame_mode_ = false;
    preserve_ = false;
    current_layer_ = 0;
    note_present();
    last_present_was_end_frame_ = true;
    pace_after_present();
    // Expire Instant burnouts into layer buffers + the seeded back buffer so preserve
    // scripts (Sine_Wave_Color_Flow) show trails without needing an idle inbox.
    // present=false: next framed swap (or busy/idle pump for fades) displays it.
    pump_fade(0.0, true, false);
}

void MatrixApi::clear() {
    for (auto& [id, layer] : layers_) fill_buffer_transparent(layer.pixels);
    current_layer_ = 0;
    surface_->clear();
    frame_mode_ = false;
    preserve_ = false;
    preserve_pixels_.clear();
    backgrounds_.hide_background(std::nullopt);
    burnout_.clear_all();
}

void MatrixApi::blackout() {
    reset_fps();
    if (frame_mode_) {
        frame_mode_ = false;
        preserve_ = false;
    }
    burnout_.clear_all();
    backgrounds_.hide_background(std::nullopt);
    try {
        sprites_.dispose_all();
    } catch (...) {
    }
    dispose_all_layers();
    preserve_pixels_.clear();
    surface_->fill(0, 0, 0);
    surface_->swap();
    surface_->fill(0, 0, 0);
}

void MatrixApi::pump_fade(double min_interval, bool force, bool present) {
    if (frame_mode_) return;
    if (!burnout_.has_work()) return;
    double now = now_sec();
    if (!force && (now - last_fade_pump_) < min_interval) return;
    last_fade_pump_ = now;
    bool changed = burnout_.tick(now);
    if (!present) return;
    if (changed || burnout_.has_active_fades()) {
        refresh_display();
        surface_->swap();
        for (auto [x, y, r, g, b] : preserve_pixels_) surface_->set_pixel(x, y, r, g, b);
        preserve_pixels_.clear();
        pace_after_present();
    }
}

void MatrixApi::pump_fade_while_busy() {
    // Same present policy as rest()'s final pump: avoid an extra composite after
    // end_frame when Instant expiry already stamped the seeded back buffer
    // (Sine_Wave_Color_Flow trails). Live fades and immediate-mode still present.
    const bool present = burnout_.has_active_fades() || !last_present_was_end_frame_;
    pump_fade(0.033, false, present);
}

void MatrixApi::rest(double seconds) {
    if (seconds <= 0) {
        // Still tick burnouts — short/zero rests must not skip expiry (Qix uses 0.05s).
        pump_fade(0.0, true, true);
        return;
    }
    // Match Python ThreadedBurnoutManager ~60 Hz for fade animation. Instant
    // expiry only updates the drawing buffer mid-rest; one present at the end
    // (Aurora_Reactor: ~70 mplots × 40ms burnout was doing 2–3 full composites
    // per rest and capping cfps near 22).
    constexpr auto kBurnoutPumpPeriod = std::chrono::milliseconds(16);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    auto last_refresh = std::chrono::steady_clock::now();
    while (true) {
        if (abort_requested()) return;
        auto current = std::chrono::steady_clock::now();
        if (current >= deadline) break;
        if (current - last_refresh >= kBurnoutPumpPeriod) {
            const bool live_fade = burnout_.has_active_fades();
            pump_fade(0.0, true, live_fade);
            last_refresh = current;
        }
        double remaining =
            std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;
        std::this_thread::sleep_for(std::chrono::duration<double>(std::min(remaining, 0.01)));
    }
    // Tick expiry into the drawing buffer. Present only when needed:
    // - immediate-mode scripts (Qix): must show clears on the panel now
    // - active fades: keep animation live
    // - after end_frame (Aurora_Reactor): skip — next framed present blits buffer
    const bool present =
        burnout_.has_active_fades() || !last_present_was_end_frame_;
    pump_fade(0.0, true, present);
}

void MatrixApi::plot(int x, int y, const ColorArg& color, int intensity, std::optional<int> burnout_ms,
                     BurnoutMode burnout_mode) {
    Rgb rgb = resolve_color(color, intensity);
    draw_pixel(x, y, rgb);
    maybe_swap();
    if (x >= 0 && y >= 0 && x < kWidth && y < kHeight)
        register_burnout({{x, y}}, rgb, burnout_ms, burnout_mode);
}

void MatrixApi::stamp_plot(int x, int y, const ColorArg& color, int intensity, std::optional<int> burnout_ms,
                           BurnoutMode burnout_mode) {
    Rgb rgb = resolve_color(color, intensity);
    draw_pixel(x, y, rgb);
    if (x >= 0 && y >= 0 && x < kWidth && y < kHeight)
        register_burnout({{x, y}}, rgb, burnout_ms, burnout_mode);
}

void MatrixApi::stamp_rgb(int x, int y, Rgb rgb) { draw_pixel(x, y, rgb); }

void MatrixApi::present_if_immediate() { maybe_swap(); }

void MatrixApi::draw_line(int x0, int y0, int x1, int y1, const ColorArg& color, int intensity,
                          std::optional<int> burnout_ms, BurnoutMode burnout_mode) {
    Rgb rgb = resolve_color(color, intensity);
    std::vector<std::pair<int, int>> pts;
    bool track = burnout_ms.has_value() && *burnout_ms >= 0;
    line_points(x0, y0, x1, y1, [&](int x, int y) {
        if (x >= 0 && y >= 0 && x < kWidth && y < kHeight) {
            draw_pixel(x, y, rgb);
            if (track) pts.emplace_back(x, y);
        }
    });
    maybe_swap();
    register_burnout(std::move(pts), rgb, burnout_ms, burnout_mode);
}

void MatrixApi::draw_rectangle(int x, int y, int w, int h, const ColorArg& color, int intensity, bool fill,
                               std::optional<int> burnout_ms, BurnoutMode burnout_mode) {
    Rgb rgb = resolve_color(color, intensity);
    std::vector<std::pair<int, int>> pts;
    bool track = burnout_ms.has_value() && *burnout_ms >= 0;
    if (fill && frame_mode_ && !preserve_) {
        Buffer& buf = current_buffer();
        int x0 = std::max(0, x), x1 = std::min(x + w, kWidth);
        int y0 = std::max(0, y), y1 = std::min(y + h, kHeight);
        for (int j = y0; j < y1; ++j)
            for (int i = x0; i < x1; ++i) {
                at(buf, i, j) = rgb;
                if (track) pts.emplace_back(i, j);
            }
    } else {
        rect_points(x, y, w, h, fill, kWidth, kHeight, [&](int px, int py) {
            draw_pixel(px, py, rgb);
            if (track) pts.emplace_back(px, py);
        });
    }
    maybe_swap();
    register_burnout(std::move(pts), rgb, burnout_ms, burnout_mode);
}

void MatrixApi::draw_circle(int xc, int yc, int r, const ColorArg& color, int intensity, bool fill,
                            std::optional<int> burnout_ms, BurnoutMode burnout_mode) {
    Rgb rgb = resolve_color(color, intensity);
    std::vector<std::pair<int, int>> pts;
    bool track = burnout_ms.has_value() && *burnout_ms >= 0;
    circle_points(xc, yc, r, fill, kWidth, kHeight, [&](int x, int y) {
        draw_pixel(x, y, rgb);
        if (track) pts.emplace_back(x, y);
    });
    maybe_swap();
    register_burnout(std::move(pts), rgb, burnout_ms, burnout_mode);
}

void MatrixApi::draw_polygon(int xc, int yc, int r, int sides, const ColorArg& color, int intensity, float rotation,
                             bool fill, std::optional<int> burnout_ms, BurnoutMode burnout_mode) {
    Rgb rgb = resolve_color(color, intensity);
    std::vector<std::pair<int, int>> pts;
    bool track = burnout_ms.has_value() && *burnout_ms >= 0;
    polygon_points(xc, yc, r, sides, rotation, fill, kWidth, kHeight, [&](int x, int y) {
        draw_pixel(x, y, rgb);
        if (track) pts.emplace_back(x, y);
    });
    maybe_swap();
    register_burnout(std::move(pts), rgb, burnout_ms, burnout_mode);
}

void MatrixApi::draw_ellipse(int xc, int yc, int xr, int yr, const ColorArg& color, int intensity, bool fill,
                             float rotation, std::optional<int> burnout_ms, BurnoutMode burnout_mode) {
    Rgb rgb = resolve_color(color, intensity);
    std::vector<std::pair<int, int>> pts;
    bool track = burnout_ms.has_value() && *burnout_ms >= 0;
    ellipse_points(xc, yc, xr, yr, rotation, fill, kWidth, kHeight, [&](int x, int y) {
        draw_pixel(x, y, rgb);
        if (track) pts.emplace_back(x, y);
    });
    maybe_swap();
    register_burnout(std::move(pts), rgb, burnout_ms, burnout_mode);
}

void MatrixApi::draw_arc(int x1, int y1, int x2, int y2, float bulge, const ColorArg& color, int intensity, bool fill,
                         std::optional<int> burnout_ms, BurnoutMode burnout_mode) {
    Rgb rgb = resolve_color(color, intensity);
    std::vector<std::pair<int, int>> pts;
    bool track = burnout_ms.has_value() && *burnout_ms >= 0;
    arc_points(x1, y1, x2, y2, bulge, fill, kWidth, kHeight, [&](int x, int y) {
        draw_pixel(x, y, rgb);
        if (track) pts.emplace_back(x, y);
    });
    maybe_swap();
    register_burnout(std::move(pts), rgb, burnout_ms, burnout_mode);
}

void MatrixApi::draw_batch_bytes(const std::vector<uint8_t>& bytes) { apply_draw_batch(*this, bytes); }

void MatrixApi::plot_batch_bytes(const std::vector<uint8_t>& bytes) {
    apply_plot_batch(*this, bytes);
    maybe_swap();
}

void MatrixApi::sprite_batch_bytes(const std::vector<uint8_t>& bytes) { apply_sprite_batch(*this, bytes); }

void MatrixApi::define_sprite(const std::string& name, int width, int height) {
    sprites_.begin_definition(name, width, height);
}

void MatrixApi::endsprite() { sprites_.end_definition(); }

void MatrixApi::sprite_cel(std::optional<int> cel_index) { sprites_.start_cel(cel_index); }

void MatrixApi::sprite_draw(const std::string& name, const std::string& cmd, const std::vector<std::string>& args) {
    MatrixSprite* sprite = sprites_.drawing_target();
    if (!sprite || sprite->name() != name) sprite = sprites_.get_template(name);
    if (!sprite) return;

    auto require_c = [](const std::string& tok) -> ColorArg {
        if (auto c = parse_color_token(tok)) return *c;
        throw std::runtime_error("bad color in sprite_draw: " + tok);
    };

    if (cmd == "plot") {
        if (args.size() < 3) throw std::runtime_error("sprite plot needs x,y,color");
        int inten = args.size() > 3 ? std::stoi(args[3]) : 100;
        sprite->plot(std::stoi(args[0]), std::stoi(args[1]), resolve_color(require_c(args[2]), 100), inten);
    } else if (cmd == "draw_line") {
        if (args.size() < 5) throw std::runtime_error("sprite draw_line needs 5+ args");
        int inten = args.size() > 5 ? std::stoi(args[5]) : 100;
        ColorArg color = require_c(args[4]);
        Rgb full = resolve_color(color, 100);
        line_points(std::stoi(args[0]), std::stoi(args[1]), std::stoi(args[2]), std::stoi(args[3]),
                    [&](int x, int y) { sprite->plot(x, y, full, inten); });
    } else if (cmd == "draw_rectangle") {
        if (args.size() < 5) throw std::runtime_error("sprite draw_rectangle needs 5+ args");
        int inten = args.size() > 5 ? std::stoi(args[5]) : 100;
        bool fill = args.size() > 6 && (args[6] == "true" || args[6] == "1" || args[6] == "True");
        ColorArg color = require_c(args[4]);
        Rgb full = resolve_color(color, 100);
        rect_points(std::stoi(args[0]), std::stoi(args[1]), std::stoi(args[2]), std::stoi(args[3]), fill,
                    sprite->width(), sprite->height(), [&](int x, int y) { sprite->plot(x, y, full, inten); });
    } else if (cmd == "draw_circle") {
        if (args.size() < 4) throw std::runtime_error("sprite draw_circle needs 4+ args");
        int inten = args.size() > 4 ? std::stoi(args[4]) : 100;
        bool fill = args.size() > 5 && (args[5] == "true" || args[5] == "1" || args[5] == "True");
        ColorArg color = require_c(args[3]);
        Rgb full = resolve_color(color, 100);
        circle_points(std::stoi(args[0]), std::stoi(args[1]), std::stoi(args[2]), fill, sprite->width(),
                      sprite->height(), [&](int x, int y) { sprite->plot(x, y, full, inten); });
    } else if (cmd == "draw_polygon") {
        if (args.size() < 5) throw std::runtime_error("sprite draw_polygon needs 5+ args");
        int inten = args.size() > 5 ? std::stoi(args[5]) : 100;
        float rot = args.size() > 6 ? std::stof(args[6]) : 0.f;
        bool fill = args.size() > 7 && (args[7] == "true" || args[7] == "1" || args[7] == "True");
        ColorArg color = require_c(args[4]);
        Rgb full = resolve_color(color, 100);
        polygon_points(std::stoi(args[0]), std::stoi(args[1]), std::stoi(args[2]), std::stoi(args[3]), rot, fill,
                       sprite->width(), sprite->height(), [&](int x, int y) { sprite->plot(x, y, full, inten); });
    } else if (cmd == "draw_ellipse") {
        if (args.size() < 5) throw std::runtime_error("sprite draw_ellipse needs 5+ args");
        int inten = args.size() > 5 ? std::stoi(args[5]) : 100;
        bool fill = args.size() > 6 && (args[6] == "true" || args[6] == "1" || args[6] == "True");
        float rot = args.size() > 7 ? std::stof(args[7]) : 0.f;
        ColorArg color = require_c(args[4]);
        Rgb full = resolve_color(color, 100);
        ellipse_points(std::stoi(args[0]), std::stoi(args[1]), std::stoi(args[2]), std::stoi(args[3]), rot, fill,
                       sprite->width(), sprite->height(), [&](int x, int y) { sprite->plot(x, y, full, inten); });
    } else if (cmd == "draw_arc") {
        if (args.size() < 6) throw std::runtime_error("sprite draw_arc needs 6+ args");
        int inten = args.size() > 6 ? std::stoi(args[6]) : 100;
        bool fill = args.size() > 7 && (args[7] == "true" || args[7] == "1" || args[7] == "True");
        ColorArg color = require_c(args[5]);
        Rgb full = resolve_color(color, 100);
        arc_points(std::stoi(args[0]), std::stoi(args[1]), std::stoi(args[2]), std::stoi(args[3]),
                   std::stof(args[4]), fill, sprite->width(), sprite->height(),
                   [&](int x, int y) { sprite->plot(x, y, full, inten); });
    } else if (cmd == "clear") {
        sprite->clear_active();
    } else {
        throw std::runtime_error("unsupported sprite command: " + cmd);
    }
    backgrounds_.invalidate_cache();
}

void MatrixApi::show_sprite(const std::string& name, int x, int y, int instance_id, int z_index,
                            std::optional<int> cel_idx) {
    ensure_layer(current_layer_);
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    bool is_new = false;
    if (!instance) {
        int initial_cel = cel_idx.value_or(0);
        instance =
            sprites_.create_instance(name, instance_id, x, y, z_index, initial_cel, current_layer_);
        if (!instance) return;
        is_new = true;
    }
    instance->x = x;
    instance->y = y;
    instance->visible = true;
    instance->layer = current_layer_;
    if (!is_new) instance->z_index = z_index;
    if (cel_idx.has_value()) instance->set_cel(*cel_idx);
    present_sprite_update();
}

void MatrixApi::hide_sprite(const std::string& name, int instance_id) {
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (instance && instance->visible) {
        instance->visible = false;
        present_sprite_update();
    }
}

void MatrixApi::move_sprite(const std::string& name, int x, int y, int instance_id, std::optional<int> cel_idx,
                            std::optional<int> z_index) {
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (!instance || !instance->visible) return;
    instance->x = x;
    instance->y = y;
    if (z_index.has_value()) instance->z_index = *z_index;
    if (cel_idx.has_value()) instance->set_cel(*cel_idx);
    else instance->advance_cel();
    present_sprite_update();
}

void MatrixApi::dispose_sprite(const std::string& name, int instance_id) {
    sprites_.dispose_instance(name, instance_id);
    present_sprite_update();
}

void MatrixApi::dispose_all_sprites() {
    backgrounds_.destroy_all();
    sprites_.dispose_all();
    if (!frame_mode_) present_sprite_update();
}

void MatrixApi::set_background(const std::string& sprite_name, int layer, int cel_index) {
    backgrounds_.set_background(sprite_name, layer, cel_index);
    if (!frame_mode_) present_sprite_update();
}

void MatrixApi::hide_background(std::optional<int> layer) {
    backgrounds_.hide_background(layer);
    if (!frame_mode_) present_sprite_update();
}

void MatrixApi::nudge_background(int dx, int dy, int layer, std::optional<int> cel_index) {
    backgrounds_.nudge(dx, dy, layer, cel_index);
    if (!frame_mode_) present_sprite_update();
}

void MatrixApi::set_background_offset(int x, int y, int layer, std::optional<int> cel_index) {
    backgrounds_.set_offset(x, y, layer, cel_index);
    if (!frame_mode_) present_sprite_update();
}

void MatrixApi::draw_text(int x, int y, const std::string& text, const std::string& font_name, int font_size,
                          const ColorArg& color, int intensity, const std::string& effect,
                          const std::string& modifier) {
    text_.draw_text(x, y, text, font_name, font_size, resolve_color(color, intensity), effect, modifier);
}

void MatrixApi::clear_text(int x, int y) { text_.clear_text(x, y); }

std::string MatrixApi::fingerprint() const {
    // Composite all visible layer pixels (no sprites/backgrounds) so layer-0-only
    // scripts match the historical single-buffer fingerprint.
    Buffer composed{};
    for (auto& p : composed) p = {kTransparentR, kTransparentG, kTransparentB};
    compose_layers_onto(composed);
    std::vector<uint8_t> payload;
    payload.reserve(kWidth * kHeight * 3);
    bool any = false;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            Rgb p = at(composed, x, y);
            if (is_transparent(p)) continue;
            any = true;
            payload.push_back(p.r);
            payload.push_back(p.g);
            payload.push_back(p.b);
        }
    }
    if (!any) return "empty";
    return sha256_hex(payload.data(), payload.size()).substr(0, 16);
}

void MatrixApi::cleanup() {
    try {
        blackout();
    } catch (...) {
    }
}

void MatrixApi::execute_command(const std::string& command) { parse_and_execute(*this, command); }

}  // namespace pixil::consumer
