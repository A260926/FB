#pragma once
// Shared rasterization internals used by both the classic rasterize() path and
// the differentiable renderer. Not part of the public teaching API.
#include "our_gl.h"

namespace raster_core {

// A triangle after the vertex stage, in the exact form rasterize() consumes.
// The screen-space area rejection is applied when the structure is built.
struct Prepared {
    bool valid = false;     // false => culled (area < 1 pixel)
    vec4 ndc[3];            // clip/w
    double clipw[3];        // original w of the clip-space corners (for perspective correction)
    vec2 screen[3];         // Viewport * ndc
    mat<3,3> abc;           // rows {sx, sy, 1} of the three corners
    mat<3,3> inv_abc_t;     // inverse transpose, as used by rasterize()
};

// Project clip-space corners through the given viewport and reject triangles
// whose signed screen area is smaller than one pixel. This is the culling
// branch of rasterize(); it is fixed at forward time.
Prepared prepare(const Triangle &clip, const mat<4,4> &viewport);

// Screen-space barycentric coordinates of pixel (x,y) w.r.t. the triangle.
vec3 bary_screen(const Prepared &p, const int x, const int y);

// Coverage test: all screen-space barycentric coordinates are non-negative.
// The boundary counts as covered.
inline bool inside(const vec3 &bc_screen) {
    return bc_screen.x>=0 && bc_screen.y>=0 && bc_screen.z>=0;
}

// Integer bounding box of the prepared triangle, clipped to [0,w)x[0,h).
// x0>x1 or y0>y1 means the triangle covers no pixel centre.
struct Bounds { int x0, x1, y0, y1; };
Bounds pixel_bounds(const Prepared &p, const int width, const int height);

// Perspective-corrected (normalized) barycentric coordinates.
vec3 bary_clip(const Prepared &p, const vec3 &bc_screen);

// Linear interpolation of depth in screen space: bc * (ndc0.z, ndc1.z, ndc2.z).
double depth(const Prepared &p, const vec3 &bc_screen);

// Depth test shared with rasterize(): the strict '>' comparison. Ties keep the
// face drawn first (the depth buffer only ever grows).
inline bool depth_passes(const double z, const double zbuffer_value) {
    return z > zbuffer_value;
}

} // namespace raster_core
