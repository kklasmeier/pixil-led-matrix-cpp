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

private:
    std::string name_;
    int width_;
    int height_;
    std::vector<SpriteCel> cels_;
    int active_ = 0;
};

struct SpriteInstance {
    MatrixSprite* template_ = nullptr;
    int x = 0;
    int y = 0;
    int z_index = 0;
    uint16_t layer = 0;
    int current_cel = 0;
    bool visible = false;

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
    SpriteInstance* create_instance(const std::string& name, int instance_id, int x, int y, int z_index,
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
