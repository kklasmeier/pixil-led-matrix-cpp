#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "consumer/types.h"

namespace pixil::consumer {

class MatrixApi;

enum class BurnoutMode : uint8_t { Instant = 0, Fade = 1 };

// Executor-only burnout bookkeeping (Phase 2 C4). No background writer thread —
// MatrixApi::pump_fade / rest call tick() on the executor.
// Burnouts are scoped to the drawing layer they were created on; overlay ownership
// among pixels on the same layer matches the single-buffer behavior.
class BurnoutManager {
public:
    explicit BurnoutManager(MatrixApi& api) : api_(api) {}

    void add(std::vector<std::pair<int, int>> points, std::vector<Rgb> colors, int duration_ms,
             BurnoutMode mode, double now_sec, uint16_t layer);

    // Advance fades and expire objects. Returns true if a drawing buffer changed.
    bool tick(double now_sec);

    void clear_all();
    void clear_layer(uint16_t layer);
    // Move burnout objects from from_layer to to_layer (call after ending any on to).
    void rekey_layer(uint16_t from_layer, uint16_t to_layer);
    bool has_active_fades() const { return !active_fades_.empty(); }
    bool has_work() const { return !objects_.empty(); }
    bool check_and_reset_changes();

private:
    struct Object {
        double start_time = 0;
        double removal_time = 0;
        BurnoutMode mode = BurnoutMode::Instant;
        uint16_t layer = 0;
        std::vector<std::pair<int, int>> points;
        std::vector<Rgb> pixel_colors;
        std::vector<Rgb> last_pixel_colors;
        std::map<std::pair<int, int>, int> point_indexes;
    };

    using PixKey = std::tuple<uint16_t, int, int>;

    bool is_pixel_owner(uint16_t layer, int x, int y, const Object* obj, double now) const;
    Rgb color_at(const Object& obj, int point_index, double now) const;
    void clear_object(const std::shared_ptr<Object>& obj, double now);
    void update_active_fades(double now);

    MatrixApi& api_;
    std::vector<std::shared_ptr<Object>> objects_;  // ordered by insertion; expiry scanned each tick
    std::map<PixKey, std::vector<std::pair<std::shared_ptr<Object>, double>>> pixel_index_;
    std::unordered_set<Object*> active_fades_;
    bool changes_made_ = false;

    static constexpr double kFadeGamma = 2.2;
};

}  // namespace pixil::consumer
