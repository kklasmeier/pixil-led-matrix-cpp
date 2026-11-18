#include "consumer/burnout.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "consumer/api.h"

namespace pixil::consumer {

void BurnoutManager::add(std::vector<std::pair<int, int>> points, std::vector<Rgb> colors, int duration_ms,
                         BurnoutMode mode, double now_sec) {
    if (points.empty() || duration_ms < 0) return;
    if (colors.size() != points.size()) {
        colors.assign(points.size(), Rgb{0, 0, 0});
        for (size_t i = 0; i < points.size(); ++i) {
            auto [x, y] = points[i];
            if (x >= 0 && y >= 0 && x < kWidth && y < kHeight) colors[i] = api_.peek_drawing(x, y);
        }
    }

    // Match Python rgb_matrix_lib: burnout point lists are sets. Outline raster
    // duplicates (shared vertices) leave stale last_pixel_colors at earlier
    // indexes while point_indexes keeps last-wins — fade then fails was_visible
    // on clear and leaves residual opaque pixels.
    {
        std::vector<std::pair<int, int>> uniq_pts;
        std::vector<Rgb> uniq_cols;
        uniq_pts.reserve(points.size());
        uniq_cols.reserve(colors.size());
        std::set<std::pair<int, int>> seen;
        for (size_t i = 0; i < points.size(); ++i) {
            if (seen.insert(points[i]).second) {
                uniq_pts.push_back(points[i]);
                uniq_cols.push_back(colors[i]);
            }
        }
        points = std::move(uniq_pts);
        colors = std::move(uniq_cols);
    }
    if (points.empty()) return;

    auto obj = std::make_shared<Object>();
    obj->start_time = now_sec;
    obj->removal_time = now_sec + duration_ms / 1000.0;
    obj->mode = mode;
    obj->points = std::move(points);
    obj->pixel_colors = std::move(colors);
    obj->last_pixel_colors = obj->pixel_colors;
    for (int i = 0; i < static_cast<int>(obj->points.size()); ++i)
        obj->point_indexes[obj->points[static_cast<size_t>(i)]] = i;

    for (size_t i = 0; i < obj->points.size(); ++i) {
        auto [x, y] = obj->points[i];
        auto key = std::make_pair(x, y);
        auto& entries = pixel_index_[key];
        // Drop underlayers that can never resurface (expire no later than this object).
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const auto& e) { return e.second <= obj->removal_time; }),
                      entries.end());
        entries.emplace_back(obj, obj->removal_time);

        Rgb current = api_.peek_drawing(x, y);
        Rgb want = obj->pixel_colors[i];
        if (current.r != want.r || current.g != want.g || current.b != want.b) {
            api_.draw_for_burnout(x, y, want);
            obj->last_pixel_colors[i] = want;
        }
    }

    if (mode == BurnoutMode::Fade) active_fades_.insert(obj.get());
    objects_.push_back(std::move(obj));
}

bool BurnoutManager::is_pixel_owner(int x, int y, const Object* obj, double now) const {
    auto it = pixel_index_.find({x, y});
    if (it == pixel_index_.end() || it->second.empty()) return false;
    for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
        if (rit->second > now) return rit->first.get() == obj;
    }
    return false;
}

Rgb BurnoutManager::color_at(const Object& obj, int point_index, double now) const {
    Rgb c = obj.pixel_colors[static_cast<size_t>(point_index)];
    if (obj.mode != BurnoutMode::Fade) return c;
    double duration = obj.removal_time - obj.start_time;
    if (duration <= 0) return {0, 0, 0};
    double progress = std::clamp((now - obj.start_time) / duration, 0.0, 1.0);
    double intensity = std::pow(1.0 - progress, kFadeGamma);
    return {static_cast<uint8_t>(c.r * intensity), static_cast<uint8_t>(c.g * intensity),
            static_cast<uint8_t>(c.b * intensity)};
}

void BurnoutManager::update_active_fades(double now) {
    if (active_fades_.empty()) return;

    struct Vis {
        int x, y;
        Object* obj;
        int point_index;
    };
    std::vector<Vis> visible;
    for (const auto& [key, entries] : pixel_index_) {
        for (auto rit = entries.rbegin(); rit != entries.rend(); ++rit) {
            if (rit->second <= now) continue;
            Object* obj = rit->first.get();
            if (obj->mode != BurnoutMode::Fade) break;
            auto pit = obj->point_indexes.find(key);
            if (pit != obj->point_indexes.end())
                visible.push_back({key.first, key.second, obj, pit->second});
            break;
        }
    }

    std::map<Object*, double> intensity_by_object;
    bool pixels_updated = false;
    for (const auto& v : visible) {
        double intensity;
        auto it = intensity_by_object.find(v.obj);
        if (it == intensity_by_object.end()) {
            double duration = v.obj->removal_time - v.obj->start_time;
            if (duration <= 0) continue;
            double progress = std::clamp((now - v.obj->start_time) / duration, 0.0, 1.0);
            intensity = std::pow(1.0 - progress, kFadeGamma);
            intensity_by_object[v.obj] = intensity;
        } else {
            intensity = it->second;
        }

        if (!is_pixel_owner(v.x, v.y, v.obj, now)) continue;
        Rgb expected = v.obj->last_pixel_colors[static_cast<size_t>(v.point_index)];
        Rgb current = api_.peek_drawing(v.x, v.y);
        if (current.r != expected.r || current.g != expected.g || current.b != expected.b) continue;

        Rgb src = v.obj->pixel_colors[static_cast<size_t>(v.point_index)];
        Rgb color{static_cast<uint8_t>(src.r * intensity), static_cast<uint8_t>(src.g * intensity),
                  static_cast<uint8_t>(src.b * intensity)};
        api_.draw_for_burnout(v.x, v.y, color);
        v.obj->last_pixel_colors[static_cast<size_t>(v.point_index)] = color;
        pixels_updated = true;
    }
    if (pixels_updated) changes_made_ = true;
}

void BurnoutManager::clear_object(const std::shared_ptr<Object>& obj, double now) {
    bool pixels_changed = false;
    for (int point_index = 0; point_index < static_cast<int>(obj->points.size()); ++point_index) {
        auto [x, y] = obj->points[static_cast<size_t>(point_index)];
        auto key = std::make_pair(x, y);
        auto it = pixel_index_.find(key);
        if (it == pixel_index_.end()) continue;
        auto& entries = it->second;

        int obj_position = -1;
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            if (entries[static_cast<size_t>(i)].first.get() == obj.get()) obj_position = i;
        }
        if (obj_position < 0) continue;

        bool was_top = true;
        for (int i = obj_position + 1; i < static_cast<int>(entries.size()); ++i) {
            if (entries[static_cast<size_t>(i)].second > now) {
                was_top = false;
                break;
            }
        }

        Rgb expected = obj->last_pixel_colors[static_cast<size_t>(point_index)];
        Rgb current = api_.peek_drawing(x, y);
        bool was_visible =
            was_top && current.r == expected.r && current.g == expected.g && current.b == expected.b;

        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const auto& e) { return e.first.get() == obj.get(); }),
                      entries.end());
        if (entries.empty()) pixel_index_.erase(it);

        if (!was_visible) continue;

        std::shared_ptr<Object> under;
        if (auto jt = pixel_index_.find(key); jt != pixel_index_.end()) {
            for (auto rit = jt->second.rbegin(); rit != jt->second.rend(); ++rit) {
                if (rit->second > now) {
                    under = rit->first;
                    break;
                }
            }
        }
        if (!under) {
            api_.draw_for_burnout(x, y, {kTransparentR, kTransparentG, kTransparentB});
        } else {
            auto pit = under->point_indexes.find(key);
            if (pit == under->point_indexes.end()) continue;
            Rgb color = color_at(*under, pit->second, now);
            api_.draw_for_burnout(x, y, color);
            under->last_pixel_colors[static_cast<size_t>(pit->second)] = color;
        }
        pixels_changed = true;
    }
    if (pixels_changed) changes_made_ = true;
}

bool BurnoutManager::tick(double now_sec) {
    update_active_fades(now_sec);

    std::vector<std::shared_ptr<Object>> expired;
    for (auto& obj : objects_) {
        if (now_sec >= obj->removal_time) expired.push_back(obj);
    }
    for (auto& obj : expired) {
        clear_object(obj, now_sec);
        active_fades_.erase(obj.get());
    }
    if (!expired.empty()) {
        objects_.erase(std::remove_if(objects_.begin(), objects_.end(),
                                      [&](const std::shared_ptr<Object>& o) {
                                          return now_sec >= o->removal_time;
                                      }),
                       objects_.end());
    }
    return check_and_reset_changes();
}

void BurnoutManager::clear_all() {
    objects_.clear();
    pixel_index_.clear();
    active_fades_.clear();
    changes_made_ = false;
}

bool BurnoutManager::check_and_reset_changes() {
    bool c = changes_made_;
    changes_made_ = false;
    return c;
}

}  // namespace pixil::consumer
