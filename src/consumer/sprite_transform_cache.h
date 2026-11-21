// LRU cache of rendered sprite transforms (docs/Feature Backlog/Pixil_Sprite_Transforms_Spec.md §7).
#pragma once

#include <cmath>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "consumer/types.h"

namespace pixil::consumer {

inline constexpr size_t kSpriteTransformCacheBytes = 2u * 1024u * 1024u;  // 2 MB
inline constexpr float kSpriteCacheAngleStepDeg = 2.f;

struct SpriteTransformCacheKey {
    std::string template_name;
    int cel = 0;
    int16_t sx_q = 0;     // log2(scale) * 32, rounded
    int16_t sy_q = 0;
    int16_t angle_q = 0;  // degrees / angle step
    uint8_t flags = 0;    // bit0 flipH, bit1 flipV

    bool operator==(const SpriteTransformCacheKey& o) const {
        return cel == o.cel && sx_q == o.sx_q && sy_q == o.sy_q && angle_q == o.angle_q && flags == o.flags &&
               template_name == o.template_name;
    }
};

struct SpriteTransformCacheKeyHash {
    size_t operator()(const SpriteTransformCacheKey& k) const noexcept {
        size_t h = std::hash<std::string>{}(k.template_name);
        h ^= (static_cast<size_t>(k.cel) * 0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2);
        h ^= (static_cast<size_t>(k.sx_q) * 0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2);
        h ^= (static_cast<size_t>(k.sy_q) * 0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2);
        h ^= (static_cast<size_t>(k.angle_q) * 0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2);
        h ^= (static_cast<size_t>(k.flags) * 0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2);
        return h;
    }
};

// Quantise positive scale to 1/32 steps in log2 space.
inline int16_t quantise_scale_log(float s) {
    if (s < 1e-4f) s = 1e-4f;
    return static_cast<int16_t>(std::lround(std::log2(static_cast<double>(s)) * 32.0));
}

inline float dequantise_scale_log(int16_t q) {
    return static_cast<float>(std::pow(2.0, static_cast<double>(q) / 32.0));
}

inline int16_t quantise_angle(float deg_wrapped_0_360) {
    return static_cast<int16_t>(std::lround(deg_wrapped_0_360 / kSpriteCacheAngleStepDeg));
}

inline float dequantise_angle(int16_t q) { return static_cast<float>(q) * kSpriteCacheAngleStepDeg; }

struct SpriteTransformCacheEntry {
    int width = 0;
    int height = 0;
    // Top-left of the buffer relative to the sprite pivot (centre).
    int origin_x = 0;
    int origin_y = 0;
    std::vector<Rgb> pixels;  // transparent sentinel = empty

    size_t bytes() const {
        return pixels.size() * sizeof(Rgb) + sizeof(SpriteTransformCacheEntry);
    }
};

class SpriteTransformCache {
public:
    explicit SpriteTransformCache(size_t byte_cap = kSpriteTransformCacheBytes);

    const SpriteTransformCacheEntry* find(const SpriteTransformCacheKey& key);
    void insert(SpriteTransformCacheKey key, SpriteTransformCacheEntry entry);
    void invalidate_template(const std::string& template_name);
    void clear();

    size_t byte_size() const { return bytes_; }
    size_t entry_count() const { return map_.size(); }
    size_t hits() const { return hits_; }
    size_t misses() const { return misses_; }
    void reset_stats() { hits_ = misses_ = 0; }

private:
    void evict_one();
    void touch(std::list<SpriteTransformCacheKey>::iterator it);

    size_t cap_;
    size_t bytes_ = 0;
    size_t hits_ = 0;
    size_t misses_ = 0;
    std::list<SpriteTransformCacheKey> lru_;  // front = most recent
    struct Node {
        SpriteTransformCacheEntry entry;
        std::list<SpriteTransformCacheKey>::iterator lru_it;
    };
    std::unordered_map<SpriteTransformCacheKey, Node, SpriteTransformCacheKeyHash> map_;
};

}  // namespace pixil::consumer
