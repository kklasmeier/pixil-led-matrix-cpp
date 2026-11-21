#include "consumer/api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <tuple>

#include "consumer/batch_decode.h"
#include "consumer/color_rgb.h"
#include "consumer/parse.h"
#include "consumer/sha256.h"
#include "consumer/shapes.h"
#include "consumer/sprite_transform_cache.h"

namespace pixil::consumer {
namespace {

double now_sec() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

Rgb apply_intensity(Rgb c, uint8_t inten) {
    if (inten == 100) return c;
    c.r = static_cast<uint8_t>(static_cast<uint16_t>(c.r) * inten / 100);
    c.g = static_cast<uint8_t>(static_cast<uint16_t>(c.g) * inten / 100);
    c.b = static_cast<uint8_t>(static_cast<uint16_t>(c.b) * inten / 100);
    return c;
}

void put_sprite_pixel(MatrixSurface& surface, int dx, int dy, Rgb c) {
    if (dx < 0 || dy < 0 || dx >= kWidth || dy >= kHeight) return;
    if (is_transparent(c)) return;
    surface.set_pixel(dx, dy, c.r, c.g, c.b);
}

// Sample cel at integer source coords; returns false if out of bounds or transparent.
bool sample_cel(const SpriteCel& cel, int sw, int sh, int sx, int sy, Rgb* out) {
    if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) return false;
    size_t i = static_cast<size_t>(sy) * static_cast<size_t>(sw) + static_cast<size_t>(sx);
    Rgb c = cel.pixels[i];
    if (is_transparent(c)) return false;
    *out = apply_intensity(c, cel.intensity[i]);
    return true;
}

Rgb average_rgb(const Rgb* samples, int n) {
    uint32_t r = 0, g = 0, b = 0;
    for (int i = 0; i < n; ++i) {
        r += samples[i].r;
        g += samples[i].g;
        b += samples[i].b;
    }
    return {static_cast<uint8_t>(r / static_cast<uint32_t>(n)),
            static_cast<uint8_t>(g / static_cast<uint32_t>(n)),
            static_cast<uint8_t>(b / static_cast<uint32_t>(n))};
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

void MatrixApi::set_sprite_scale(const std::string& name, float scale, int instance_id,
                                 std::optional<float> scale_y) {
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (!instance) return;
    float sx = clamp_sprite_scale(std::fabs(scale));
    float sy = scale_y.has_value() ? clamp_sprite_scale(std::fabs(*scale_y)) : sx;
    instance->transform.scale_x = sx;
    instance->transform.scale_y = sy;
    present_sprite_update();
}

void MatrixApi::set_sprite_angle(const std::string& name, float angle_deg, int instance_id) {
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (!instance) return;
    instance->transform.angle_deg = angle_deg;
    present_sprite_update();
}

void MatrixApi::set_sprite_tilt(const std::string& name, float tilt_x_deg, float tilt_y_deg, int instance_id) {
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (!instance) return;
    instance->transform.tilt_x_deg = tilt_x_deg;
    instance->transform.tilt_y_deg = tilt_y_deg;
    present_sprite_update();
}

void MatrixApi::set_sprite_flip(const std::string& name, int flip, int instance_id) {
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (!instance) return;
    flip = std::clamp(flip, 0, 3);
    instance->transform.flip_h = (flip & 1) != 0;
    instance->transform.flip_v = (flip & 2) != 0;
    present_sprite_update();
}

void MatrixApi::set_sprite_anchor(const std::string& name, int mode, int instance_id) {
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (!instance) return;
    instance->transform.centre_anchor = (mode == 1);
    present_sprite_update();
}

void MatrixApi::set_sprite_transform(const std::string& name, float scale, float angle_deg, int instance_id,
                                     std::optional<float> tilt_x, std::optional<float> tilt_y,
                                     std::optional<int> flip) {
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (!instance) return;
    float s = clamp_sprite_scale(std::fabs(scale));
    instance->transform.scale_x = s;
    instance->transform.scale_y = s;
    instance->transform.angle_deg = angle_deg;
    if (tilt_x.has_value()) instance->transform.tilt_x_deg = *tilt_x;
    if (tilt_y.has_value()) instance->transform.tilt_y_deg = *tilt_y;
    if (flip.has_value()) {
        int f = std::clamp(*flip, 0, 3);
        instance->transform.flip_h = (f & 1) != 0;
        instance->transform.flip_v = (f & 2) != 0;
    }
    present_sprite_update();
}

void MatrixApi::reset_sprite_transform(const std::string& name, int instance_id) {
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (!instance) return;
    instance->transform.reset_motion();
    present_sprite_update();
}

void MatrixApi::sprite_back_cel(const std::string& name, int cel_index) {
    MatrixSprite* tmpl = sprites_.get_template(name);
    if (!tmpl) throw std::runtime_error("sprite_back_cel: unknown sprite " + name);
    if (cel_index < 0 || cel_index >= tmpl->cel_count()) {
        throw std::runtime_error("sprite_back_cel: cel out of range");
    }
    tmpl->set_back_cel(cel_index);
    xform_cache_.invalidate_template(name);
}

void MatrixApi::set_sprite_transform_state(const std::string& name, int instance_id, const SpriteTransform& t) {
    SpriteInstance* instance = sprites_.get_instance(name, instance_id);
    if (!instance) return;
    instance->transform = t;
    instance->transform.scale_x = clamp_sprite_scale(std::fabs(instance->transform.scale_x));
    instance->transform.scale_y = clamp_sprite_scale(std::fabs(instance->transform.scale_y));
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

void MatrixApi::blit_one_sprite(const SpriteInstance& inst) {
    const int sw = inst.width();
    const int sh = inst.height();
    if (sw <= 0 || sh <= 0 || !inst.template_) return;
    const SpriteTransform& tr = inst.transform;

    // Destination centre: centre-anchor uses x,y directly; top-left keeps unscaled centre fixed.
    const float dest_cx = tr.centre_anchor ? inst.x : inst.x + 0.5f * static_cast<float>(sw);
    const float dest_cy = tr.centre_anchor ? inst.y : inst.y + 0.5f * static_cast<float>(sh);

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDeg = kPi / 180.f;
    const float cos_tx = std::cos(tr.tilt_x_deg * kDeg);
    const float cos_ty = std::cos(tr.tilt_y_deg * kDeg);
    // Signed foreshortening: negative ⇒ reverse side (mirror).
    float sx_signed = tr.scale_x * cos_ty;
    float sy_signed = tr.scale_y * cos_tx;
    const bool showing_back = (cos_tx * cos_ty) < 0.f;

    // Optional still back cel when reverse faces us; else flipped front animation.
    const bool use_back_art =
        showing_back && inst.template_->back_cel().has_value();
    const SpriteCel& cel =
        use_back_art ? inst.template_->cel(*inst.template_->back_cel()) : inst.current();

    // Fold tilt sign into flip (back art is authored from behind — no extra mirror).
    bool flip_h = tr.flip_h;
    bool flip_v = tr.flip_v;
    float sx = std::fabs(sx_signed);
    float sy = std::fabs(sy_signed);
    if (!use_back_art) {
        if (sx_signed < 0.f) flip_h = !flip_h;
        if (sy_signed < 0.f) flip_v = !flip_v;
    }
    // Avoid divide-by-zero in inverse map; tiny values handled by edge-on / tiny rules.
    if (sx < 1e-4f) sx = 1e-4f;
    if (sy < 1e-4f) sy = 1e-4f;

    auto cel_average = [&](Rgb* out) -> bool {
        uint32_t r = 0, g = 0, b = 0;
        int opaque = 0;
        for (int row = 0; row < sh; ++row) {
            for (int col = 0; col < sw; ++col) {
                Rgb c;
                if (!sample_cel(cel, sw, sh, col, row, &c)) continue;
                r += c.r;
                g += c.g;
                b += c.b;
                ++opaque;
            }
        }
        if (opaque == 0) return false;
        *out = {static_cast<uint8_t>(r / static_cast<uint32_t>(opaque)),
                static_cast<uint8_t>(g / static_cast<uint32_t>(opaque)),
                static_cast<uint8_t>(b / static_cast<uint32_t>(opaque))};
        return true;
    };

    // Wrap angle to [0, 360) for exact-quadrant checks.
    float angle = std::fmod(tr.angle_deg, 360.f);
    if (angle < 0.f) angle += 360.f;
    const bool no_tilt = tr.tilt_x_deg == 0.f && tr.tilt_y_deg == 0.f;
    const bool scale_one = tr.scale_x == 1.f && tr.scale_y == 1.f;

    // --- Identity ---
    if (tr.is_draw_identity()) {
        const int x0 = static_cast<int>(std::lround(dest_cx - 0.5f * static_cast<float>(sw)));
        const int y0 = static_cast<int>(std::lround(dest_cy - 0.5f * static_cast<float>(sh)));
        for (int row = 0; row < sh; ++row) {
            for (int col = 0; col < sw; ++col) {
                size_t i = static_cast<size_t>(row) * static_cast<size_t>(sw) + static_cast<size_t>(col);
                Rgb c = cel.pixels[i];
                if (is_transparent(c)) continue;
                put_sprite_pixel(*surface_, x0 + col, y0 + row, apply_intensity(c, cel.intensity[i]));
            }
        }
        return;
    }

    // --- Flip-only (scale 1, no spin/tilt) ---
    if (scale_one && no_tilt && angle == 0.f) {
        const int x0 = static_cast<int>(std::lround(dest_cx - 0.5f * static_cast<float>(sw)));
        const int y0 = static_cast<int>(std::lround(dest_cy - 0.5f * static_cast<float>(sh)));
        for (int row = 0; row < sh; ++row) {
            for (int col = 0; col < sw; ++col) {
                int src_x = flip_h ? (sw - 1 - col) : col;
                int src_y = flip_v ? (sh - 1 - row) : row;
                Rgb c;
                if (!sample_cel(cel, sw, sh, src_x, src_y, &c)) continue;
                put_sprite_pixel(*surface_, x0 + col, y0 + row, c);
            }
        }
        return;
    }

    // Edge-on / tiny stay uncached (cheap special cases).
    const float rad_exact = tr.angle_deg * kDeg;
    const float ca_e = std::cos(rad_exact);
    const float sa_e = std::sin(rad_exact);
    const float pix_w = std::fabs(sx_signed) * static_cast<float>(sw);
    const float pix_h = std::fabs(sy_signed) * static_cast<float>(sh);
    if ((pix_w < 1.f && pix_h >= 2.f) || (pix_h < 1.f && pix_w >= 2.f)) {
        Rgb avg{};
        if (!cel_average(&avg)) return;
        const float len = std::max(pix_w, pix_h);
        const int steps = std::max(1, static_cast<int>(std::ceil(len)));
        float dir_x = pix_w >= pix_h ? ca_e : -sa_e;
        float dir_y = pix_w >= pix_h ? sa_e : ca_e;
        for (int i = 0; i <= steps; ++i) {
            float t = (static_cast<float>(i) / static_cast<float>(steps)) - 0.5f;
            put_sprite_pixel(*surface_, static_cast<int>(std::lround(dest_cx + dir_x * t * len)),
                             static_cast<int>(std::lround(dest_cy + dir_y * t * len)), avg);
        }
        return;
    }

    // --- Cached path: exact 90° steps and general inverse mapping ---
    const int cel_index = use_back_art ? *inst.template_->back_cel() : inst.current_cel;
    uint8_t flag_bits = 0;
    if (flip_h) flag_bits |= 1;
    if (flip_v) flag_bits |= 2;

    // Quantise scale/angle for the cache key; render with those buckets so hits match.
    const int16_t sx_q = quantise_scale_log(sx);
    const int16_t sy_q = quantise_scale_log(sy);
    const int16_t ang_q = quantise_angle(angle);
    float sx_use = dequantise_scale_log(sx_q);
    float sy_use = dequantise_scale_log(sy_q);
    float angle_use = dequantise_angle(ang_q);
    if (sx_use < 1e-4f) sx_use = 1e-4f;
    if (sy_use < 1e-4f) sy_use = 1e-4f;

    SpriteTransformCacheKey key;
    key.template_name = inst.template_->name();
    key.cel = cel_index;
    key.sx_q = sx_q;
    key.sy_q = sy_q;
    key.angle_q = ang_q;
    key.flags = flag_bits;

    if (const SpriteTransformCacheEntry* hit = xform_cache_.find(key)) {
        blit_cached_transform(*hit, dest_cx, dest_cy);
        return;
    }

    const float rad = angle_use * kDeg;
    const float ca = std::cos(rad);
    const float sa = std::sin(rad);
    const float hw = 0.5f * static_cast<float>(sw);
    const float hh = 0.5f * static_cast<float>(sh);

    // Exact quadrant with quantised angle still on a multiple of 90 when input was exact.
    const bool exact_quad = scale_one && no_tilt &&
                            (angle_use == 90.f || angle_use == 180.f || angle_use == 270.f);

    SpriteTransformCacheEntry entry;
    if (exact_quad) {
        const int box_w = (angle_use == 90.f || angle_use == 270.f) ? sh : sw;
        const int box_h = (angle_use == 90.f || angle_use == 270.f) ? sw : sh;
        entry.width = box_w;
        entry.height = box_h;
        entry.origin_x = -box_w / 2;
        entry.origin_y = -box_h / 2;
        entry.pixels.assign(static_cast<size_t>(box_w * box_h),
                            Rgb{kTransparentR, kTransparentG, kTransparentB});
        for (int src_y = 0; src_y < sh; ++src_y) {
            for (int src_x = 0; src_x < sw; ++src_x) {
                int fx = flip_h ? (sw - 1 - src_x) : src_x;
                int fy = flip_v ? (sh - 1 - src_y) : src_y;
                int out_x = 0, out_y = 0;
                if (angle_use == 90.f) {
                    out_x = sh - 1 - fy;
                    out_y = fx;
                } else if (angle_use == 180.f) {
                    out_x = sw - 1 - fx;
                    out_y = sh - 1 - fy;
                } else {
                    out_x = fy;
                    out_y = sw - 1 - fx;
                }
                Rgb c;
                if (!sample_cel(cel, sw, sh, src_x, src_y, &c)) continue;
                entry.pixels[static_cast<size_t>(out_y) * static_cast<size_t>(box_w) +
                             static_cast<size_t>(out_x)] = c;
            }
        }
    } else {
        // Bounding box relative to centre (0,0).
        auto map_corner = [&](float lx, float ly, float* ox, float* oy) {
            if (flip_h) lx = -lx;
            if (flip_v) ly = -ly;
            lx *= sx_use;
            ly *= sy_use;
            *ox = lx * ca - ly * sa;
            *oy = lx * sa + ly * ca;
        };
        float cxs[4], cys[4];
        map_corner(-hw, -hh, &cxs[0], &cys[0]);
        map_corner(hw, -hh, &cxs[1], &cys[1]);
        map_corner(hw, hh, &cxs[2], &cys[2]);
        map_corner(-hw, hh, &cxs[3], &cys[3]);
        float min_xf = cxs[0], max_xf = cxs[0], min_yf = cys[0], max_yf = cys[0];
        for (int i = 1; i < 4; ++i) {
            min_xf = std::min(min_xf, cxs[i]);
            max_xf = std::max(max_xf, cxs[i]);
            min_yf = std::min(min_yf, cys[i]);
            max_yf = std::max(max_yf, cys[i]);
        }
        int min_x = static_cast<int>(std::floor(min_xf));
        int max_x = static_cast<int>(std::ceil(max_xf));
        int min_y = static_cast<int>(std::floor(min_yf));
        int max_y = static_cast<int>(std::ceil(max_yf));
        // Cap extreme boxes (scale ≤ 8, sprite usually small).
        constexpr int kMaxBox = 128;
        if (max_x - min_x > kMaxBox) {
            int mid = (min_x + max_x) / 2;
            min_x = mid - kMaxBox / 2;
            max_x = min_x + kMaxBox;
        }
        if (max_y - min_y > kMaxBox) {
            int mid = (min_y + max_y) / 2;
            min_y = mid - kMaxBox / 2;
            max_y = min_y + kMaxBox;
        }

        entry.origin_x = min_x;
        entry.origin_y = min_y;
        entry.width = std::max(1, max_x - min_x);
        entry.height = std::max(1, max_y - min_y);
        entry.pixels.assign(static_cast<size_t>(entry.width * entry.height),
                            Rgb{kTransparentR, kTransparentG, kTransparentB});

        if (entry.width < 2 && entry.height < 2) {
            Rgb avg{};
            int opaque = 0;
            const int total = sw * sh;
            uint32_t r = 0, g = 0, b = 0;
            for (int row = 0; row < sh; ++row) {
                for (int col = 0; col < sw; ++col) {
                    Rgb c;
                    if (!sample_cel(cel, sw, sh, col, row, &c)) continue;
                    r += c.r;
                    g += c.g;
                    b += c.b;
                    ++opaque;
                }
            }
            if (opaque > 0 && opaque * 4 >= total) {
                avg = {static_cast<uint8_t>(r / static_cast<uint32_t>(opaque)),
                       static_cast<uint8_t>(g / static_cast<uint32_t>(opaque)),
                       static_cast<uint8_t>(b / static_cast<uint32_t>(opaque))};
                entry.pixels[0] = avg;
                entry.width = entry.height = 1;
                entry.origin_x = entry.origin_y = 0;
            }
        } else {
            const bool downscale = sx_use < 1.f || sy_use < 1.f;
            const int grid_x =
                downscale ? std::min(16, std::max(1, static_cast<int>(std::ceil(1.f / sx_use)))) : 2;
            const int grid_y =
                downscale ? std::min(16, std::max(1, static_cast<int>(std::ceil(1.f / sy_use)))) : 2;
            const int max_samples = grid_x * grid_y;

            for (int iy = 0; iy < entry.height; ++iy) {
                for (int ix = 0; ix < entry.width; ++ix) {
                    int dx = min_x + ix;
                    int dy = min_y + iy;
                    Rgb hits[16];
                    int hit_n = 0;
                    int probed = 0;
                    for (int gy = 0; gy < grid_y; ++gy) {
                        for (int gx = 0; gx < grid_x; ++gx) {
                            float u = downscale ? (static_cast<float>(gx) + 0.5f) / static_cast<float>(grid_x)
                                                : (gx == 0 ? 0.25f : 0.75f);
                            float v = downscale ? (static_cast<float>(gy) + 0.5f) / static_cast<float>(grid_y)
                                                : (gy == 0 ? 0.25f : 0.75f);
                            float dest_x = static_cast<float>(dx) + u;
                            float dest_y = static_cast<float>(dy) + v;
                            float lx = dest_x * ca + dest_y * sa;
                            float ly = -dest_x * sa + dest_y * ca;
                            lx /= sx_use;
                            ly /= sy_use;
                            if (flip_h) lx = -lx;
                            if (flip_v) ly = -ly;
                            int src_x = static_cast<int>(std::floor(lx + hw));
                            int src_y = static_cast<int>(std::floor(ly + hh));
                            ++probed;
                            Rgb c;
                            if (sample_cel(cel, sw, sh, src_x, src_y, &c) && hit_n < 16) hits[hit_n++] = c;
                        }
                    }
                    if (hit_n == 0) continue;
                    Rgb out{};
                    if (downscale) {
                        if (hit_n * 5 < probed * 2) continue;
                        out = average_rgb(hits, hit_n);
                    } else {
                        if (hit_n <= max_samples - 3) continue;
                        out = hits[0];
                        int best_count = 1;
                        for (int i = 0; i < hit_n; ++i) {
                            int count = 0;
                            for (int j = 0; j < hit_n; ++j) {
                                if (hits[j].r == hits[i].r && hits[j].g == hits[i].g &&
                                    hits[j].b == hits[i].b)
                                    ++count;
                            }
                            if (count > best_count) {
                                best_count = count;
                                out = hits[i];
                            }
                        }
                    }
                    entry.pixels[static_cast<size_t>(iy) * static_cast<size_t>(entry.width) +
                                 static_cast<size_t>(ix)] = out;
                }
            }
        }
    }

    blit_cached_transform(entry, dest_cx, dest_cy);
    xform_cache_.insert(std::move(key), std::move(entry));
}

void MatrixApi::blit_cached_transform(const SpriteTransformCacheEntry& entry, float dest_cx, float dest_cy) {
    for (int iy = 0; iy < entry.height; ++iy) {
        for (int ix = 0; ix < entry.width; ++ix) {
            Rgb c = entry.pixels[static_cast<size_t>(iy) * static_cast<size_t>(entry.width) +
                                 static_cast<size_t>(ix)];
            if (is_transparent(c)) continue;
            int dx = static_cast<int>(std::lround(dest_cx + static_cast<float>(entry.origin_x + ix)));
            int dy = static_cast<int>(std::lround(dest_cy + static_cast<float>(entry.origin_y + iy)));
            put_sprite_pixel(*surface_, dx, dy, c);
        }
    }
}

void MatrixApi::blit_sprites_for_layer(uint16_t layer) {
    for (const auto& [name, id] : sprites_.iter_draw_order_for_layer(layer)) {
        SpriteInstance* inst = sprites_.get_instance(name, id);
        if (!inst || !inst->visible || !inst->template_) continue;
        blit_one_sprite(*inst);
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

void MatrixApi::show_sprite(const std::string& name, float x, float y, int instance_id, int z_index,
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

void MatrixApi::move_sprite(const std::string& name, float x, float y, int instance_id,
                            std::optional<int> cel_idx, std::optional<int> z_index) {
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
    xform_cache_.clear();
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

bool MatrixApi::surface_pixel(int x, int y, uint8_t* r, uint8_t* g, uint8_t* b) const {
    return surface_ && surface_->get_pixel(x, y, r, g, b);
}

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
