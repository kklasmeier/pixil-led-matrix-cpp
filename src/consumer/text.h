#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "consumer/types.h"

namespace pixil::consumer {

class MatrixApi;

// tiny64 bitmap font + NORMAL text path (C3). Fancy effects render as NORMAL.
class TextRenderer {
public:
    explicit TextRenderer(MatrixApi& api) : api_(api) {}

    // font_path: tiny64_font.txt; empty → search next to cwd / repo fonts/.
    bool load_font(const std::string& font_path = {});
    void draw_text(int x, int y, const std::string& text, const std::string& font_name, int font_size,
                   Rgb color, const std::string& effect = "NORMAL", const std::string& modifier = {});
    void clear_text(int x, int y);

private:
    struct Bounds {
        int x, y, w, h;
    };

    bool ensure_font();
    std::pair<int, int> text_size(const std::string& text) const;
    void render_bitmap(int x, int y, const std::string& text, Rgb color);

    MatrixApi& api_;
    std::map<std::string, std::vector<std::string>> glyphs_;
    bool loaded_ = false;
    std::string font_path_;
    std::map<std::pair<int, int>, Bounds> bounds_;
};

}  // namespace pixil::consumer
