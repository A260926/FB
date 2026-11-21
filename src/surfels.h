#pragma once

/* Opaque CPU surfel path for the surface-preview renderer.
 *
 * Each surfel is a point p with two linearly independent tangent half-axes a
 * and b.  Together they describe the curved patch
 *
 *     S(u, v) = p + a*u + b*v,   u^2 + v^2 <= 1
 *
 * and a linear-RGB colour.  Unlike transparent particles, surfels are opaque:
 * for every pixel the renderer intersects the pixel's viewing ray with the
 * surfel's plane, solves the exact tangential coordinates (u, v), keeps the
 * hit only when q = u^2 + v^2 < 1 and the view depth is inside [near, far],
 * and weights it by the kernel (1 - q)^2.  The closest such hit d0 is found
 * first, then all hits with d <= d0 + delta are normalised and blended.  This
 * reconstructs a surface; it is *not* alpha compositing: colours are
 * normalised, so a front layer cannot leak through the back layer.
 */

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "pipeline.h"

namespace gfx {

/* ------------------------------------------------------------------------- */
/* Inputs                                                                     */
/* ------------------------------------------------------------------------- */

struct Surfel {
    // Tangent patch: centre p, two linearly independent tangent half-axes a, b.
    std::array<double, 3> p{};
    std::array<double, 3> a{};
    std::array<double, 3> b{};
    Color color;  // linear RGB, each channel in [0, 1]

    Surfel() = default;
    Surfel(const std::array<double, 3>& p, const std::array<double, 3>& a,
           const std::array<double, 3>& b, const Color& color)
        : p(p), a(a), b(b), color(color) {}

    // Compute the unit normal n = normalize(cross(a, b)).  Returns false when
    // a and b are (numerically) collinear.
    auto UnitNormal(std::array<double, 3>& out) const -> bool;

    // Same, from explicit tangent axes.
    static auto NormalFromAxes(const std::array<double, 3>& a,
                               const std::array<double, 3>& b,
                               std::array<double, 3>& out) -> bool;
};

struct SurfelCamera {
    // Conventional finite perspective, symmetric or asymmetric.  clip.w =
    // -view.z, NDC z in [-1, 1] for view depths in [near, far].  The camera
    // looks down -Z in view space; `view` is a rigid transform (rotation +
    // translation, possibly mirrored, but no scale/shear).
    Mat4d view = Mat4d::Identity();
    Mat4d projection;   // callers supply; the renderer derives its inverse
    double near_depth = 0.1;
    double far_depth = 100.0;

    // Finite, non-zero, view-space direction towards the light, handed
    // directly to FragmentShader.
    std::array<double, 3> light_direction{0.0, 0.0, -1.0};
};

// Independent base image that surfels are composited over.  `color` is linear
// RGB (row 0 is y = 0, i.e. y up, index y*width+x).  `depth` is the view depth
// d = -view.z, +infinity where the background is empty.  Inputs are copied,
// never modified.
struct BaseImage {
    int width = 0;
    int height = 0;
    std::vector<Color> color;
    std::vector<double> depth;

    BaseImage() = default;
    BaseImage(int width, int height,
              std::vector<Color> color, std::vector<double> depth)
        : width(width), height(height), color(std::move(color)), depth(std::move(depth)) {}

    static auto Empty(int width, int height) -> BaseImage {
        const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
        return BaseImage(width, height,
                         std::vector<Color>(n, Color{}),
                         std::vector<double>(n, std::numeric_limits<double>::infinity()));
    }
};

struct SurfelOptions {
    // delta >= 0 in view-space length.  All hits with depth <= d0 + delta are
    // blended.  delta = 0 keeps exactly the nearest depth layer.
    double delta = 0.0;
    int tile_size = 16;  // candidate tile size (pixels)
};

/* ------------------------------------------------------------------------- */
/* Results and statistics                                                     */
/* ------------------------------------------------------------------------- */

// Conservative integer pixel rectangle [x0, x1] x [y0, y1] (inclusive).
struct PixelRect {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // empty when x1 < x0 or y1 < y0
    auto empty() const -> bool { return x1 < x0 || y1 < y0; }
    auto width() const -> int { return empty() ? 0 : x1 - x0 + 1; }
    auto height() const -> int { return empty() ? 0 : y1 - y0 + 1; }
    auto Contains(int x, int y) const -> bool {
        return !empty() && x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
};

struct SurfelPerPoint {
    // Derivation: -1 not derived (validation failed or rejected point),
    //             0 fully inside the near/far depth range (exact analytic
    //               screen support), 1 crossing a near/far plane (conservative
    //               clipped-tangent-square support), 2 rejected (collinear
    //               axes / non-finite input), 3 entirely outside the depth range.
    int support_kind = -1;
    // Continuous support rectangle in screen coordinates (pixel centres are at
    // x+0.5, y+0.5, y up).  Kind 0 is the exact extremum of the projected
    // disk; kinds 1/3 are conservative bounds of the clipped tangent square.
    double support_x_min = 0, support_x_max = 0;
    double support_y_min = 0, support_y_max = 0;
    // Cached image-clipped integer screen rectangle (inclusive).  Empty when
    // nothing visible.
    PixelRect screen_bounds;
    // Bounding box used for tiling (already clamped to the image); this is
    // screen_bounds widened by one pixel so boundary pixels are never lost.
    PixelRect tile_bounds;
    // Number of (tile, surfel) references this surfel received.
    size_t tile_refs = 0;
    // Number of ray/disk intersection tests actually performed for this surfel.
    size_t intersection_tests = 0;
    // Number of pixels where this surfel produced a valid hit.
    size_t accepted_hits = 0;
};

struct SurfelStats {
    int width = 0, height = 0;
    int tile_cols = 0, tile_rows = 0;
    int tile_size = 0;

    // Every stored surfel index that is referenced by at least one tile,
    // sorted ascending.
    std::vector<int> candidates;
    // Flat [tile_cols * tile_rows][candidate] table: row-major by tile index
    // (ty * tile_cols + tx), each row a sorted list of surfel indices.
    std::vector<std::vector<int>> tile_refs;
    std::vector<SurfelPerPoint> per_point;

    size_t intersection_tests = 0;
    size_t accepted_hits = 0;
    size_t pixels_with_hits = 0;
    size_t pixels_modified = 0;
};

struct SurfelResult {
    int width = 0;
    int height = 0;
    std::vector<Color> color;   // linear RGB, y-up, index y*width+x
    std::vector<double> depth;  // view depth; +infinity when nothing hit
    SurfelStats stats;
};

/* ------------------------------------------------------------------------- */
/* API                                                                        */
/* ------------------------------------------------------------------------- */

// Validate a camera without touching any published result.  Useful on its own
// and used internally before rendering.
auto ValidateCamera(const SurfelCamera& camera, int width, int height) -> bool;

// Rasterize surfels over the base image.
//
// Returns std::nullopt and leaves *out untouched when the dimensions, camera,
// surfels, options or base image are invalid (non-finite numbers, collinear
// half-axes, colours outside [0,1], delta < 0, non-invertible projection, ...).
// On success the result is owned by the caller; inputs are not modified.
auto RenderSurfels(const std::vector<Surfel>& surfels, const SurfelCamera& camera,
                   const BaseImage& base, const SurfelOptions& options,
                   SurfelResult* out) -> bool;

// Convenience overload returning an empty optional on failure.
auto RenderSurfels(const std::vector<Surfel>& surfels, const SurfelCamera& camera,
                   const BaseImage& base, const SurfelOptions& options = {})
    -> std::optional<SurfelResult>;

// Copy a Target into a linear BaseImage.  Target depths are view depths
// (d = -view.z), so the copy is direct.
auto BaseFromTarget(const Target& target) -> BaseImage;

// Decode/encode 24-bit PPM (P6).  Decode flips the stored top-down row order
// into the y-up array layout; encode flips it back.
auto DecodePPM(const uint8_t* bytes, size_t size, BaseImage* out) -> bool;
auto EncodePPM(const Color* color, int width, int height,
               std::vector<uint8_t>* out) -> bool;

} // namespace gfx
