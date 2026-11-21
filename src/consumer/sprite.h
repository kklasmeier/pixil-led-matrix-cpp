#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "consumer/types.h"

namespace pixil::consumer {

struct SpriteCel {
    std::vector<Rgb> pixels;       // w*h, transparent sentinel
    std::vector<uint8_t> intensity;  // w*h, 0..100
};

class MatrixSprite {
public:
    MatrixSprite(std::string name, int width, int height);

    const std::string& name() const { return name_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int cel_count() const { return static_cast<int>(cels_.size()); }
    int active_cel() const { return active_; }

    void ensure_cel(int index);
    void set_active_cel(int index);
    SpriteCel& active();
    const SpriteCel& cel(int index) const;

    void plot(int x, int y, Rgb rgb_full, int intensity);
    void clear_active();

    void set_back_cel(int cel_index);
    std::optional<int> back_cel() const { return back_cel_; }

private:
    std::string name_;
    int width_;
    int height_;
    std::vector<SpriteCel> cels_;
    int active_ = 0;
    std::optional<int> back_cel_;
};

// Per-instance scale / flip / anchor (angle + tilt fields exist for the wire; S2 draws them).
struct SpriteTransform {
    float scale_x = 1.f;
    float scale_y = 1.f;
    float angle_deg = 0.f;   // carried; rendered in S2
    float tilt_x_deg = 0.f;  // carried; rendered in S2
    float tilt_y_deg = 0.f;  // carried; rendered in S2
    bool flip_h = false;
    bool flip_v = false;
    bool centre_anchor = false;

    // True when the 1:1 blit path applies (anchor may still shift placement).
    bool is_draw_identity() const {
        return scale_x == 1.f && scale_y == 1.f && angle_deg == 0.f && tilt_x_deg == 0.f &&
               tilt_y_deg == 0.f && !flip_h && !flip_v;
    }
    // True when fractional positions / OP_MOVE_SUB are used (scale or flip in S1).
    bool needs_subpixel() const { return !is_draw_identity(); }

    void reset_motion() {
        scale_x = scale_y = 1.f;
        angle_deg = tilt_x_deg = tilt_y_deg = 0.f;
        flip_h = flip_v = false;
        // centre_anchor intentionally unchanged
    }
};

inline constexpr float kSpriteScaleMin = 0.1f;
inline constexpr float kSpriteScaleMax = 8.f;

inline float clamp_sprite_scale(float s) {
    if (s < kSpriteScaleMin) return kSpriteScaleMin;
    if (s > kSpriteScaleMax) return kSpriteScaleMax;
    return s;
}

struct SpriteInstance {
    MatrixSprite* template_ = nullptr;
    float x = 0.f;
    float y = 0.f;
    int z_index = 0;
    uint16_t layer = 0;
    int current_cel = 0;
    bool visible = false;
    SpriteTransform transform{};

    int width() const { return template_ ? template_->width() : 0; }
    int height() const { return template_ ? template_->height() : 0; }
    void advance_cel();
    void set_cel(int index);
    const SpriteCel& current() const;
};

class SpriteManager {
public:
    void begin_definition(const std::string& name, int width, int height);
    int start_cel(std::optional<int> cel_index);
    void end_definition();
    MatrixSprite* drawing_target();
    MatrixSprite* get_template(const std::string& name);
    SpriteInstance* get_instance(const std::string& name, int instance_id);
    SpriteInstance* create_instance(const std::string& name, int instance_id, float x, float y, int z_index,
                                    int cel_index, uint16_t layer = 0);
    std::vector<std::pair<std::string, int>> iter_draw_order() const;
    std::vector<std::pair<std::string, int>> iter_draw_order_for_layer(uint16_t layer) const;
    void hide_sprites_on_layer(uint16_t layer);
    void reassign_layer(uint16_t from_layer, uint16_t to_layer);
    void dispose_instance(const std::string& name, int instance_id);
    void dispose_all();

private:
    std::map<std::string, MatrixSprite> templates_;
    std::map<std::string, std::map<int, SpriteInstance>> instances_;
    std::vector<std::pair<std::string, int>> z_order_;
    MatrixSprite* defining_ = nullptr;
    std::set<int> cels_used_;
    int next_auto_cel_ = 0;
    bool in_cel_block_ = false;
};

}  // namespace pixil::consumer
