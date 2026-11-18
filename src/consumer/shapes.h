#pragma once

#include <functional>
#include <vector>

#include "consumer/types.h"

namespace pixil::consumer {

using PlotFn = std::function<void(int x, int y)>;

void line_points(int x0, int y0, int x1, int y1, const PlotFn& plot);
void rect_points(int x, int y, int w, int h, bool fill, int max_w, int max_h, const PlotFn& plot);
void circle_points(int xc, int yc, int r, bool fill, int max_w, int max_h, const PlotFn& plot);
void polygon_points(int xc, int yc, int r, int sides, float rotation_deg, bool fill, int max_w, int max_h,
                    const PlotFn& plot);
void ellipse_points(int xc, int yc, int xr, int yr, float rotation_deg, bool fill, int max_w, int max_h,
                    const PlotFn& plot);
void arc_points(int x1, int y1, int x2, int y2, float bulge, bool fill, int max_w, int max_h, const PlotFn& plot);

}  // namespace pixil::consumer
