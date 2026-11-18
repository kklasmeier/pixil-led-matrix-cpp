#include "consumer/shapes.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <utility>

namespace {
constexpr double kPi = std::numbers::pi_v<double>;
}

namespace pixil::consumer {

void line_points(int x0, int y0, int x1, int y1, const PlotFn& plot) {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void rect_points(int x, int y, int w, int h, bool fill, int max_w, int max_h, const PlotFn& plot) {
    if (fill) {
        int x0 = std::max(0, x);
        int x1 = std::min(x + w, max_w);
        int y0 = std::max(0, y);
        int y1 = std::min(y + h, max_h);
        for (int j = y0; j < y1; ++j)
            for (int i = x0; i < x1; ++i) plot(i, j);
        return;
    }
    for (int i = std::max(0, x); i < std::min(x + w, max_w); ++i) {
        if (y >= 0 && y < max_h) plot(i, y);
        if (y + h - 1 >= 0 && y + h - 1 < max_h) plot(i, y + h - 1);
    }
    for (int j = std::max(0, y); j < std::min(y + h, max_h); ++j) {
        if (x >= 0 && x < max_w) plot(x, j);
        if (x + w - 1 >= 0 && x + w - 1 < max_w) plot(x + w - 1, j);
    }
}

void circle_points(int xc, int yc, int r, bool fill, int max_w, int max_h, const PlotFn& plot) {
    if (r < 0) return;
    auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < max_w && y < max_h; };
    // Match rgb_matrix_lib.api.draw_circle midpoint loop (incl. fill spans for ±x and ±y).
    auto fill_span = [&](int x0, int y0, int x1) {
        int start = std::max(0, std::min(x0, x1));
        int end = std::min(max_w - 1, std::max(x0, x1));
        if (y0 < 0 || y0 >= max_h || start > end) return;
        for (int px = start; px <= end; ++px) plot(px, y0);
    };
    auto plot8 = [&](int x, int y) {
        if (in(xc + x, yc + y)) plot(xc + x, yc + y);
        if (in(xc - x, yc + y)) plot(xc - x, yc + y);
        if (in(xc + x, yc - y)) plot(xc + x, yc - y);
        if (in(xc - x, yc - y)) plot(xc - x, yc - y);
        if (in(xc + y, yc + x)) plot(xc + y, yc + x);
        if (in(xc - y, yc + x)) plot(xc - y, yc + x);
        if (in(xc + y, yc - x)) plot(xc + y, yc - x);
        if (in(xc - y, yc - x)) plot(xc - y, yc - x);
    };
    int x = 0, y = r;
    int d = 3 - 2 * r;
    while (y >= x) {
        if (fill) {
            fill_span(xc - x, yc - y, xc + x);
            fill_span(xc - x, yc + y, xc + x);
            fill_span(xc - y, yc - x, xc + y);
            fill_span(xc - y, yc + x, xc + y);
        } else {
            plot8(x, y);
        }
        ++x;
        if (d > 0) {
            --y;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

static std::vector<std::pair<int, int>> polygon_vertices(int xc, int yc, int r, int sides, float rotation_deg) {
    std::vector<std::pair<int, int>> v;
    if (sides < 3 || r <= 0) return v;
    // Match rgb_matrix_lib.utils.polygon_vertices: odd sides point up; even flat-top;
    // clockwise via negated angle; truncate toward zero (Python int()).
    double default_offset = (sides % 2 != 0) ? 90.0 : (90.0 - (180.0 / sides));
    double angle = -(rotation_deg + default_offset) * kPi / 180.0;
    for (int i = 0; i < sides; ++i) {
        double theta = angle + (2.0 * kPi * i) / sides;
        int x = xc + static_cast<int>(r * std::cos(theta));
        int y = yc + static_cast<int>(r * std::sin(theta));
        v.emplace_back(x, y);
    }
    return v;
}

void polygon_points(int xc, int yc, int r, int sides, float rotation_deg, bool fill, int max_w, int max_h,
                    const PlotFn& plot) {
    auto verts = polygon_vertices(xc, yc, r, sides, rotation_deg);
    if (verts.empty()) return;
    auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < max_w && y < max_h; };
    for (size_t i = 0; i < verts.size(); ++i) {
        auto [x0, y0] = verts[i];
        auto [x1, y1] = verts[(i + 1) % verts.size()];
        line_points(x0, y0, x1, y1, [&](int x, int y) {
            if (in(x, y)) plot(x, y);
        });
    }
    if (!fill) return;
    int min_x = max_w, max_x = 0, min_y = max_h, max_y = 0;
    for (auto [x, y] : verts) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    }
    min_x = std::max(0, min_x);
    max_x = std::min(max_w - 1, max_x);
    min_y = std::max(0, min_y);
    max_y = std::min(max_h - 1, max_y);
    for (int y = min_y; y <= max_y; ++y) {
        std::vector<int> xs;
        for (size_t i = 0; i < verts.size(); ++i) {
            auto [x0, y0] = verts[i];
            auto [x1, y1] = verts[(i + 1) % verts.size()];
            if ((y0 < y && y1 >= y) || (y1 < y && y0 >= y)) {
                xs.push_back(static_cast<int>(x0 + (y - y0) * (x1 - x0) / double(y1 - y0)));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            int a = std::max(min_x, std::min(xs[i], xs[i + 1]));
            int b = std::min(max_x, std::max(xs[i], xs[i + 1]));
            for (int x = a; x <= b; ++x) plot(x, y);
        }
    }
}

void ellipse_points(int xc, int yc, int xr, int yr, float rotation_deg, bool fill, int max_w, int max_h,
                    const PlotFn& plot) {
    auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < max_w && y < max_h; };
    // Match rgb_matrix_lib: radians(rotation) without negation; round() on rotate.
    double rad = rotation_deg * kPi / 180.0;
    double cos_r = std::cos(rad), sin_r = std::sin(rad);
    auto rot = [&](int x, int y) {
        return std::pair<int, int>{static_cast<int>(std::lround(x * cos_r - y * sin_r)),
                                   static_cast<int>(std::lround(x * sin_r + y * cos_r))};
    };
    auto plot_rot = [&](int x, int y) {
        auto [rx, ry] = rot(x, y);
        if (in(xc + rx, yc + ry)) plot(xc + rx, yc + ry);
    };
    if (xr == 0 || yr == 0) {
        if (xr == 0 && yr == 0) {
            if (in(xc, yc)) plot(xc, yc);
        } else if (xr == 0) {
            for (int y = -yr; y <= yr; ++y) plot_rot(0, y);
        } else {
            for (int x = -xr; x <= xr; ++x) plot_rot(x, 0);
        }
        return;
    }
    int max_r = std::max(xr, yr);
    int min_x = std::max(0, xc - max_r - 1);
    int max_x = std::min(max_w - 1, xc + max_r + 1);
    int min_y = std::max(0, yc - max_r - 1);
    int max_y = std::min(max_h - 1, yc + max_r + 1);
    double xr2 = double(xr) * xr, yr2 = double(yr) * yr;
    if (fill) {
        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                double dx = x - xc, dy = y - yc;
                double rx = dx * cos_r + dy * sin_r;
                double ry = -dx * sin_r + dy * cos_r;
                if ((rx * rx) / xr2 + (ry * ry) / yr2 <= 1.0) plot(x, y);
            }
        }
        return;
    }
    // Outline: midpoint ellipse (same as rgb_matrix_lib.api.draw_ellipse).
    long long a2 = static_cast<long long>(xr) * xr;
    long long b2 = static_cast<long long>(yr) * yr;
    int x = 0, y = yr;
    long long d1 = b2 - a2 * yr + a2 / 4;
    long long dx = 2 * b2 * x;
    long long dy = 2 * a2 * y;
    plot_rot(x, y);
    plot_rot(-x, y);
    plot_rot(x, -y);
    plot_rot(-x, -y);
    while (dx < dy) {
        ++x;
        dx += 2 * b2;
        if (d1 < 0) {
            d1 += dx + b2;
        } else {
            --y;
            dy -= 2 * a2;
            d1 += dx + b2 - dy;
        }
        plot_rot(x, y);
        plot_rot(-x, y);
        plot_rot(x, -y);
        plot_rot(-x, -y);
    }
    double d2 = (b2 * (x + 0.5) * (x + 0.5) + a2 * (y - 1.0) * (y - 1.0) - static_cast<double>(a2) * b2);
    while (y >= 0) {
        --y;
        dy -= 2 * a2;
        if (d2 > 0) {
            d2 += static_cast<double>(a2) - dy;
        } else {
            ++x;
            dx += 2 * b2;
            d2 += static_cast<double>(a2) - dy + dx;
        }
        plot_rot(x, y);
        plot_rot(-x, y);
        plot_rot(x, -y);
        plot_rot(-x, -y);
    }
}

void arc_points(int x1, int y1, int x2, int y2, float bulge, bool fill, int max_w, int max_h, const PlotFn& plot) {
    auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < max_w && y < max_h; };
    auto emit = [&](int x, int y) {
        if (in(x, y)) plot(x, y);
    };

    if (std::fabs(bulge) < 0.001f) {
        line_points(x1, y1, x2, y2, emit);
        return;
    }

    double chord_dx = x2 - x1;
    double chord_dy = y2 - y1;
    double chord_length = std::sqrt(chord_dx * chord_dx + chord_dy * chord_dy);
    if (chord_length < 0.001) {
        emit(static_cast<int>(std::lround(x1)), static_cast<int>(std::lround(y1)));
        return;
    }

    double mx = (x1 + x2) / 2.0;
    double my = (y1 + y2) / 2.0;
    double ux = chord_dx / chord_length;
    double uy = chord_dy / chord_length;
    double px = -uy;
    double py = ux;
    double peak_x = mx + px * bulge;
    double peak_y = my + py * bulge;

    double half_chord = chord_length / 2.0;
    double abs_bulge = std::fabs(bulge);
    double radius = (half_chord * half_chord + abs_bulge * abs_bulge) / (2.0 * abs_bulge);
    double center_dist = radius - abs_bulge;
    double cx = mx - px * (bulge / abs_bulge) * center_dist;
    double cy = my - py * (bulge / abs_bulge) * center_dist;

    auto normalize = [](double a) {
        while (a < 0) a += 2 * kPi;
        while (a >= 2 * kPi) a -= 2 * kPi;
        return a;
    };

    double start_angle = std::atan2(y1 - cy, x1 - cx);
    double end_angle = std::atan2(y2 - cy, x2 - cx);
    double peak_angle = std::atan2(peak_y - cy, peak_x - cx);
    double start_n = normalize(start_angle);
    double end_n = normalize(end_angle);
    double peak_n = normalize(peak_angle);

    auto angle_between_ccw = [](double start, double end, double test) {
        if (start <= end) return start <= test && test <= end;
        return test >= start || test <= end;
    };
    bool ccw_passes_peak = angle_between_ccw(start_n, end_n, peak_n);

    double sweep = 0;
    int direction = 1;
    if (ccw_passes_peak) {
        if (end_n < start_n) end_n += 2 * kPi;
        sweep = end_n - start_n;
        direction = 1;
    } else {
        if (start_n < end_n) start_n += 2 * kPi;
        sweep = start_n - end_n;
        direction = -1;
    }

    double arc_length = std::fabs(sweep) * radius;
    int num_steps = std::max(static_cast<int>(arc_length * 2), 20);
    std::vector<std::pair<int, int>> outline;
    outline.reserve(static_cast<size_t>(num_steps) + 1);
    std::set<std::pair<int, int>> points;
    for (int i = 0; i <= num_steps; ++i) {
        double t = static_cast<double>(i) / num_steps;
        double angle = direction == 1 ? start_angle + t * sweep : start_angle - t * sweep;
        int ax = static_cast<int>(std::lround(cx + radius * std::cos(angle)));
        int ay = static_cast<int>(std::lround(cy + radius * std::sin(angle)));
        outline.emplace_back(ax, ay);
        points.emplace(ax, ay);
    }

    if (fill && !outline.empty()) {
        int min_x = outline[0].first, max_x = outline[0].first;
        int min_y = outline[0].second, max_y = outline[0].second;
        for (auto [x, y] : outline) {
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
        for (int y = min_y; y <= max_y; ++y) {
            std::vector<double> xs;
            for (size_t i = 0; i + 1 < outline.size(); ++i) {
                auto [ax1, ay1] = outline[i];
                auto [ax2, ay2] = outline[i + 1];
                if ((ay1 <= y && y < ay2) || (ay2 <= y && y < ay1)) {
                    if (ay2 != ay1) {
                        double t = (y - ay1) / double(ay2 - ay1);
                        xs.push_back(ax1 + t * (ax2 - ax1));
                    }
                }
            }
            if ((y1 <= y && y < y2) || (y2 <= y && y < y1)) {
                if (y2 != y1) {
                    double t = (y - y1) / double(y2 - y1);
                    xs.push_back(x1 + t * (x2 - x1));
                }
            }
            if (xs.size() >= 2) {
                std::sort(xs.begin(), xs.end());
                for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                    int x_start = static_cast<int>(std::lround(xs[i]));
                    int x_end = static_cast<int>(std::lround(xs[i + 1]));
                    for (int x = x_start; x <= x_end; ++x) points.emplace(x, y);
                }
            }
        }
    }

    for (auto [x, y] : points) emit(x, y);
}

}  // namespace pixil::consumer
