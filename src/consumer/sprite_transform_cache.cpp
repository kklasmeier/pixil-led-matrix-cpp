#include "consumer/sprite_transform_cache.h"

namespace pixil::consumer {

SpriteTransformCache::SpriteTransformCache(size_t byte_cap) : cap_(byte_cap) {}

void SpriteTransformCache::touch(std::list<SpriteTransformCacheKey>::iterator it) {
    if (it == lru_.begin()) return;
    lru_.splice(lru_.begin(), lru_, it);
}

void SpriteTransformCache::evict_one() {
    if (lru_.empty()) return;
    const SpriteTransformCacheKey& old = lru_.back();
    auto it = map_.find(old);
    if (it != map_.end()) {
        bytes_ -= it->second.entry.bytes();
        map_.erase(it);
    }
    lru_.pop_back();
}

const SpriteTransformCacheEntry* SpriteTransformCache::find(const SpriteTransformCacheKey& key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    touch(it->second.lru_it);
    return &it->second.entry;
}

void SpriteTransformCache::insert(SpriteTransformCacheKey key, SpriteTransformCacheEntry entry) {
    auto existing = map_.find(key);
    if (existing != map_.end()) {
        bytes_ -= existing->second.entry.bytes();
        lru_.erase(existing->second.lru_it);
        map_.erase(existing);
    }
    size_t add = entry.bytes();
    while (!map_.empty() && bytes_ + add > cap_) evict_one();
    // If a single entry is larger than the cap, still keep it (rare huge sprite).
    lru_.push_front(key);
    Node node;
    node.entry = std::move(entry);
    node.lru_it = lru_.begin();
    bytes_ += node.entry.bytes();
    map_.emplace(std::move(key), std::move(node));
}

void SpriteTransformCache::invalidate_template(const std::string& template_name) {
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->first.template_name == template_name) {
            bytes_ -= it->second.entry.bytes();
            lru_.erase(it->second.lru_it);
            it = map_.erase(it);
        } else {
            ++it;
        }
    }
}

void SpriteTransformCache::clear() {
    map_.clear();
    lru_.clear();
    bytes_ = 0;
}

}  // namespace pixil::consumer
