#include "consumer/sprite.h"

#include <algorithm>
#include <stdexcept>

namespace pixil::consumer {
namespace {

SpriteCel make_cel(int w, int h) {
    SpriteCel c;
    c.pixels.assign(static_cast<size_t>(w * h), Rgb{kTransparentR, kTransparentG, kTransparentB});
    c.intensity.assign(static_cast<size_t>(w * h), 100);
    return c;
}

}  // namespace

MatrixSprite::MatrixSprite(std::string name, int width, int height)
    : name_(std::move(name)), width_(width), height_(height) {
    cels_.push_back(make_cel(width_, height_));
}

void MatrixSprite::ensure_cel(int index) {
    while (static_cast<int>(cels_.size()) <= index) cels_.push_back(make_cel(width_, height_));
}

void MatrixSprite::set_active_cel(int index) {
    if (index < 0 || index >= cel_count()) throw std::runtime_error("cel index out of range");
    active_ = index;
}

SpriteCel& MatrixSprite::active() { return cels_[static_cast<size_t>(active_)]; }

const SpriteCel& MatrixSprite::cel(int index) const {
    if (index < 0 || index >= cel_count()) throw std::runtime_error("cel index out of range");
    return cels_[static_cast<size_t>(index)];
}

void MatrixSprite::plot(int x, int y, Rgb rgb_full, int intensity) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    intensity = std::clamp(intensity, 0, 100);
    size_t i = static_cast<size_t>(y) * width_ + static_cast<size_t>(x);
    active().pixels[i] = rgb_full;
    active().intensity[i] = static_cast<uint8_t>(intensity);
}

void MatrixSprite::clear_active() {
    active() = make_cel(width_, height_);
}

void MatrixSprite::set_back_cel(int cel_index) {
    ensure_cel(cel_index);
    back_cel_ = cel_index;
}

void SpriteInstance::advance_cel() {
    if (!template_ || template_->cel_count() == 0) return;
    current_cel = (current_cel + 1) % template_->cel_count();
}

void SpriteInstance::set_cel(int index) {
    if (!template_ || index < 0 || index >= template_->cel_count()) {
        throw std::runtime_error("cel index out of range");
    }
    current_cel = index;
}

const SpriteCel& SpriteInstance::current() const {
    return template_->cel(current_cel);
}

void SpriteManager::begin_definition(const std::string& name, int width, int height) {
    if (templates_.count(name)) throw std::runtime_error("sprite template already exists: " + name);
    if (defining_) throw std::runtime_error("already defining a sprite");
    auto [it, ok] = templates_.emplace(name, MatrixSprite(name, width, height));
    (void)ok;
    defining_ = &it->second;
    cels_used_ = {0};
    next_auto_cel_ = 1;
    in_cel_block_ = false;
}

int SpriteManager::start_cel(std::optional<int> cel_index) {
    if (!defining_) throw std::runtime_error("not defining a sprite");
    int idx = 0;
    if (!cel_index.has_value()) {
        if (next_auto_cel_ == 1 && cels_used_.count(0) && !in_cel_block_) idx = 0;
        else {
            idx = next_auto_cel_;
            next_auto_cel_ = idx + 1;
        }
    } else {
        idx = *cel_index;
        next_auto_cel_ = std::max(next_auto_cel_, idx + 1);
    }
    if (cels_used_.count(idx) && idx != 0) throw std::runtime_error("duplicate cel index");
    if (idx == 0 && in_cel_block_) throw std::runtime_error("duplicate cel index");
    defining_->ensure_cel(idx);
    defining_->set_active_cel(idx);
    cels_used_.insert(idx);
    in_cel_block_ = true;
    if (idx == 0) next_auto_cel_ = std::max(next_auto_cel_, 1);
    return idx;
}

void SpriteManager::end_definition() {
    if (!defining_) throw std::runtime_error("not defining a sprite");
    int max_cel = cels_used_.empty() ? 0 : *cels_used_.rbegin();
    for (int i = 0; i <= max_cel; ++i) {
        if (!cels_used_.count(i)) throw std::runtime_error("gap in cel indices");
    }
    defining_ = nullptr;
    cels_used_.clear();
    next_auto_cel_ = 0;
    in_cel_block_ = false;
}

MatrixSprite* SpriteManager::drawing_target() { return defining_; }

MatrixSprite* SpriteManager::get_template(const std::string& name) {
    auto it = templates_.find(name);
    return it == templates_.end() ? nullptr : &it->second;
}

SpriteInstance* SpriteManager::get_instance(const std::string& name, int instance_id) {
    auto it = instances_.find(name);
    if (it == instances_.end()) return nullptr;
    auto jt = it->second.find(instance_id);
    return jt == it->second.end() ? nullptr : &jt->second;
}

SpriteInstance* SpriteManager::create_instance(const std::string& name, int instance_id, float x, float y,
                                               int z_index, int cel_index, uint16_t layer) {
    MatrixSprite* tmpl = get_template(name);
    if (!tmpl) return nullptr;
    if (cel_index < 0 || cel_index >= tmpl->cel_count()) return nullptr;
    auto& bucket = instances_[name];
    if (bucket.count(instance_id)) return &bucket[instance_id];
    SpriteInstance inst;
    inst.template_ = tmpl;
    inst.x = x;
    inst.y = y;
    inst.z_index = z_index;
    inst.layer = layer;
    inst.current_cel = cel_index;
    bucket.emplace(instance_id, inst);
    z_order_.emplace_back(name, instance_id);
    return &bucket[instance_id];
}

std::vector<std::pair<std::string, int>> SpriteManager::iter_draw_order() const {
    struct Row {
        int z;
        int creation;
        std::string name;
        int id;
    };
    std::vector<Row> rows;
    for (int i = 0; i < static_cast<int>(z_order_.size()); ++i) {
        const auto& [name, id] = z_order_[static_cast<size_t>(i)];
        int z = 0;
        auto it = instances_.find(name);
        if (it != instances_.end()) {
            auto jt = it->second.find(id);
            if (jt != it->second.end()) z = jt->second.z_index;
        }
        rows.push_back({z, i, name, id});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.z != b.z) return a.z < b.z;
        return a.creation < b.creation;
    });
    std::vector<std::pair<std::string, int>> out;
    out.reserve(rows.size());
    for (auto& r : rows) out.emplace_back(std::move(r.name), r.id);
    return out;
}

std::vector<std::pair<std::string, int>> SpriteManager::iter_draw_order_for_layer(uint16_t layer) const {
    struct Row {
        int z;
        int creation;
        std::string name;
        int id;
    };
    std::vector<Row> rows;
    for (int i = 0; i < static_cast<int>(z_order_.size()); ++i) {
        const auto& [name, id] = z_order_[static_cast<size_t>(i)];
        auto it = instances_.find(name);
        if (it == instances_.end()) continue;
        auto jt = it->second.find(id);
        if (jt == it->second.end() || jt->second.layer != layer) continue;
        rows.push_back({jt->second.z_index, i, name, id});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.z != b.z) return a.z < b.z;
        return a.creation < b.creation;
    });
    std::vector<std::pair<std::string, int>> out;
    out.reserve(rows.size());
    for (auto& r : rows) out.emplace_back(std::move(r.name), r.id);
    return out;
}

void SpriteManager::hide_sprites_on_layer(uint16_t layer) {
    for (auto& [name, bucket] : instances_) {
        for (auto& [id, inst] : bucket) {
            if (inst.layer == layer) inst.visible = false;
        }
    }
}

void SpriteManager::reassign_layer(uint16_t from_layer, uint16_t to_layer) {
    for (auto& [name, bucket] : instances_) {
        for (auto& [id, inst] : bucket) {
            if (inst.layer == from_layer) inst.layer = to_layer;
        }
    }
}

void SpriteManager::dispose_instance(const std::string& name, int instance_id) {
    auto it = instances_.find(name);
    if (it == instances_.end() || !it->second.count(instance_id)) return;
    it->second.erase(instance_id);
    if (it->second.empty()) instances_.erase(it);
    z_order_.erase(std::remove(z_order_.begin(), z_order_.end(), std::make_pair(name, instance_id)),
                   z_order_.end());
}

void SpriteManager::dispose_all() {
    instances_.clear();
    templates_.clear();
    z_order_.clear();
    defining_ = nullptr;
    cels_used_.clear();
    next_auto_cel_ = 0;
    in_cel_block_ = false;
}

}  // namespace pixil::consumer
