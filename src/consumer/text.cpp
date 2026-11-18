#include "consumer/text.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include "consumer/api.h"

namespace pixil::consumer {

bool TextRenderer::load_font(const std::string& font_path) {
    font_path_ = font_path;
    loaded_ = false;
    glyphs_.clear();
    return ensure_font();
}

bool TextRenderer::ensure_font() {
    if (loaded_) return true;
    std::vector<std::string> candidates;
    if (!font_path_.empty()) candidates.push_back(font_path_);
    if (const char* env = std::getenv("PIXIL_FONT_PATH")) candidates.insert(candidates.begin(), env);
    candidates.push_back("fonts/tiny64_font.txt");
    candidates.push_back("../fonts/tiny64_font.txt");
    candidates.push_back("../../fonts/tiny64_font.txt");

    for (const auto& path : candidates) {
        std::ifstream in(path);
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            auto comma = line.find(',');
            if (comma == std::string::npos) continue;
            std::string ch = line.substr(0, comma);
            std::vector<std::string> rows;
            size_t start = comma + 1;
            while (start <= line.size()) {
                auto next = line.find(',', start);
                if (next == std::string::npos) {
                    rows.push_back(line.substr(start));
                    break;
                }
                rows.push_back(line.substr(start, next - start));
                start = next + 1;
            }
            if (!ch.empty() && !rows.empty()) glyphs_[ch] = std::move(rows);
        }
        if (!glyphs_.empty()) {
            loaded_ = true;
            font_path_ = path;
            return true;
        }
    }
    std::cerr << "[pixil-consumer] tiny64_font.txt not found\n";
    return false;
}

std::pair<int, int> TextRenderer::text_size(const std::string& text) const {
    int width = 0, height = 0;
    for (char c : text) {
        std::string key(1, c);
        auto it = glyphs_.find(key);
        if (it == glyphs_.end()) {
            width += 3;
            continue;
        }
        const auto& rows = it->second;
        int orig_w = rows.empty() ? 0 : static_cast<int>(rows[0].size());
        int leading = 0, trailing = 0;
        for (int x = 0; x < orig_w; ++x) {
            bool col = false;
            for (const auto& row : rows)
                if (x < static_cast<int>(row.size()) && row[static_cast<size_t>(x)] == '1') col = true;
            if (col) break;
            ++leading;
        }
        for (int x = orig_w - 1; x >= 0; --x) {
            bool col = false;
            for (const auto& row : rows)
                if (x < static_cast<int>(row.size()) && row[static_cast<size_t>(x)] == '1') col = true;
            if (col) break;
            ++trailing;
        }
        int tw = std::max(1, orig_w - leading - trailing);
        width += tw + 1;
        height = std::max(height, static_cast<int>(rows.size()));
    }
    if (width > 0) --width;
    return {width, height};
}

void TextRenderer::render_bitmap(int x, int y, const std::string& text, Rgb color) {
    int x_pos = x;
    for (char c : text) {
        std::string key(1, c);
        auto it = glyphs_.find(key);
        if (it == glyphs_.end()) {
            x_pos += 3;
            continue;
        }
        const auto& rows = it->second;
        int orig_w = rows.empty() ? 0 : static_cast<int>(rows[0].size());
        int leading = 0, trailing = 0;
        for (int col = 0; col < orig_w; ++col) {
            bool any = false;
            for (const auto& row : rows)
                if (col < static_cast<int>(row.size()) && row[static_cast<size_t>(col)] == '1') any = true;
            if (any) break;
            ++leading;
        }
        for (int col = orig_w - 1; col >= 0; --col) {
            bool any = false;
            for (const auto& row : rows)
                if (col < static_cast<int>(row.size()) && row[static_cast<size_t>(col)] == '1') any = true;
            if (any) break;
            ++trailing;
        }
        int tw = std::max(1, orig_w - leading - trailing);
        for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
            const std::string& bits = rows[static_cast<size_t>(row)];
            for (int col = leading; col < orig_w - trailing; ++col) {
                if (col < static_cast<int>(bits.size()) && bits[static_cast<size_t>(col)] == '1') {
                    api_.stamp_rgb(x_pos + (col - leading), y + row, color);
                }
            }
        }
        x_pos += tw + 1;
    }
}

void TextRenderer::draw_text(int x, int y, const std::string& text, const std::string& /*font_name*/,
                             int /*font_size*/, Rgb color, const std::string& effect,
                             const std::string& modifier) {
    if (!ensure_font()) return;
    auto [tw, th] = text_size(text);
    int render_x = x;
    std::string mod = modifier;
    for (auto& c : mod) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string eff = effect;
    for (auto& c : eff) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (eff == "NORMAL" || eff.empty()) {
        if (mod == "center") render_x = x - tw / 2;
        else if (mod == "right") render_x = x - tw;
    }
    bounds_[{x, y}] = Bounds{render_x, y, tw, th};
    render_bitmap(render_x, y, text, color);
    api_.present_if_immediate();
}

void TextRenderer::clear_text(int x, int y) {
    auto it = bounds_.find({x, y});
    if (it == bounds_.end()) return;
    const Bounds& b = it->second;
    for (int py = b.y; py < b.y + b.h; ++py)
        for (int px = b.x; px < b.x + b.w; ++px)
            api_.stamp_rgb(px, py, {kTransparentR, kTransparentG, kTransparentB});
    bounds_.erase(it);
    api_.present_if_immediate();
}

}  // namespace pixil::consumer
