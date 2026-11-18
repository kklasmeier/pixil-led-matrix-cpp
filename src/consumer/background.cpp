#include "consumer/background.h"

#include <algorithm>

namespace pixil::consumer {

bool BackgroundManager::set_background(const std::string& sprite_name, int layer, int cel_index) {
    if (layer < 0) return false;
    MatrixSprite* tmpl = sprites_.get_template(sprite_name);
    if (!tmpl || cel_index < 0 || cel_index >= tmpl->cel_count()) return false;
    layers_[layer] = Layer{sprite_name, cel_index, 0, 0, true};
    invalidate_cache();
    return true;
}

void BackgroundManager::hide_background(std::optional<int> layer) {
    if (!layer.has_value()) {
        for (auto& [_, s] : layers_) s.visible = false;
    } else if (layers_.count(*layer)) {
        layers_[*layer].visible = false;
    }
    invalidate_cache();
}

void BackgroundManager::destroy_all() {
    layers_.clear();
    invalidate_cache();
}

void BackgroundManager::update_cel(Layer& state, std::optional<int> cel_index) {
    MatrixSprite* tmpl = sprites_.get_template(state.sprite_name);
    if (!tmpl || tmpl->cel_count() == 0) return;
    if (cel_index.has_value()) state.cel_index = *cel_index % tmpl->cel_count();
    else state.cel_index = (state.cel_index + 1) % tmpl->cel_count();
}

void BackgroundManager::nudge(int dx, int dy, int layer, std::optional<int> cel_index) {
    auto it = layers_.find(layer);
    if (it == layers_.end()) return;
    it->second.offset_x += dx;
    it->second.offset_y += dy;
    update_cel(it->second, cel_index);
    invalidate_cache();
}

void BackgroundManager::set_offset(int x, int y, int layer, std::optional<int> cel_index) {
    auto it = layers_.find(layer);
    if (it == layers_.end()) return;
    it->second.offset_x = x;
    it->second.offset_y = y;
    update_cel(it->second, cel_index);
    invalidate_cache();
}

bool BackgroundManager::has_background() const {
    for (const auto& [_, s] : layers_)
        if (s.visible) return true;
    return false;
}

void BackgroundManager::render_layer(const MatrixSprite& tmpl, const Layer& state, int width, int height,
                                     Buffer& out) {
    const SpriteCel& cel = tmpl.cel(state.cel_index);
    int sp_w = tmpl.width(), sp_h = tmpl.height();
    if (sp_w <= 0 || sp_h <= 0) return;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sx = ((x + state.offset_x) % sp_w + sp_w) % sp_w;
            int sy = ((y + state.offset_y) % sp_h + sp_h) % sp_h;
            size_t si = static_cast<size_t>(sy) * sp_w + static_cast<size_t>(sx);
            Rgb c = cel.pixels[si];
            uint8_t inten = cel.intensity[si];
            if (inten != 100) {
                c.r = static_cast<uint8_t>(c.r * inten / 100);
                c.g = static_cast<uint8_t>(c.g * inten / 100);
                c.b = static_cast<uint8_t>(c.b * inten / 100);
            }
            if (is_transparent(c) && inten != 100) c = {0, 0, 0};
            if (!is_transparent(c)) at(out, x, y) = c;
        }
    }
}

Buffer BackgroundManager::get_viewport(int width, int height) {
    if (cache_valid_ && width == kWidth && height == kHeight) return cache_;
    Buffer viewport{};
    for (auto& p : viewport) p = {kTransparentR, kTransparentG, kTransparentB};
    for (auto& [_, state] : layers_) {
        if (!state.visible) continue;
        MatrixSprite* tmpl = sprites_.get_template(state.sprite_name);
        if (!tmpl) continue;
        Buffer layer{};
        for (auto& p : layer) p = {kTransparentR, kTransparentG, kTransparentB};
        render_layer(*tmpl, state, width, height, layer);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (!is_transparent(at(layer, x, y))) at(viewport, x, y) = at(layer, x, y);
    }
    if (width == kWidth && height == kHeight) {
        cache_ = viewport;
        cache_valid_ = true;
    }
    return viewport;
}

}  // namespace pixil::consumer
