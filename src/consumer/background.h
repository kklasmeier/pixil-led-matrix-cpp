#pragma once

#include <map>
#include <optional>
#include <vector>

#include "consumer/sprite.h"
#include "consumer/types.h"

namespace pixil::consumer {

class BackgroundManager {
public:
    explicit BackgroundManager(SpriteManager& sprites) : sprites_(sprites) {}

    bool set_background(const std::string& sprite_name, int layer = 0, int cel_index = 0);
    void hide_background(std::optional<int> layer);
    void destroy_all();
    void nudge(int dx, int dy, int layer = 0, std::optional<int> cel_index = std::nullopt);
    void set_offset(int x, int y, int layer = 0, std::optional<int> cel_index = std::nullopt);
    bool has_background() const;
    // Returns panel-sized buffer; transparent sentinel where nothing drew.
    Buffer get_viewport(int width, int height);
    void invalidate_cache() { cache_valid_ = false; }

private:
    struct Layer {
        std::string sprite_name;
        int cel_index = 0;
        int offset_x = 0;
        int offset_y = 0;
        bool visible = true;
    };

    void update_cel(Layer& state, std::optional<int> cel_index);
    void render_layer(const MatrixSprite& tmpl, const Layer& state, int width, int height, Buffer& out);

    SpriteManager& sprites_;
    std::map<int, Layer> layers_;
    Buffer cache_{};
    bool cache_valid_ = false;
};

}  // namespace pixil::consumer
