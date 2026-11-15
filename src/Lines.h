#pragma once

#include "Canvas.h"
#include "Colour.h"

// Classic line-rasterization primitives operating directly on a Canvas.
//
// - bresenhamLine: integer-only Bresenham line (crisp, aliased).
// - wuLine: Xiaolin Wu anti-aliased line; endpoint and edge pixels are blended
//   against the existing canvas contents by fractional coverage.
// - cohenSutherlandClip: Cohen-Sutherland outcode clip against an axis-aligned
//   rectangle; returns false when the segment lies wholly outside, otherwise
//   clips the endpoints in place and returns true.

void bresenhamLine(Canvas &c, int x0, int y0, int x1, int y1, Colour col);

void wuLine(Canvas &c, float x0, float y0, float x1, float y1, Colour col);

bool cohenSutherlandClip(float &x0, float &y0, float &x1, float &y1, float xmin, float ymin, float xmax, float ymax);
