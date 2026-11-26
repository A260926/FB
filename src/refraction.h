#pragma once

/*
 * Windowless thick-glass preview for cpp-rasterizer.
 *
 * The module performs real off-screen captures (reusing the rasterizer's
 * triangle traversal and Phong fragment generation) and then composes one
 * transmitted ray through a closed convex glass volume:
 *
 *     camera --(air -> glass)--> entrance
 *            --(internal segment)--> exit
 *            --(glass -> air)--> opaque background
 *
 * Conventions
 * -----------
 *  - Matrices are row-major with column vectors: clip = P * V * p.
 *    Element (row, col) lives at value[row * 4 + col].
 *  - View space is right handed with the camera at the origin looking down
 *    -Z, so visible points have z < 0.  "view depth" means d = -z > 0.
 *  - The projection must satisfy clip.w = -view.z and map the finite view
 *    depth range [near, far] onto NDC z in [0, 1].  PerspectiveProjection()
 *    builds such a matrix and supports asymmetric left/right/bottom/top.
 *  - NDC x/y in [-1, 1] map to screen coordinates
 *
 *        sx = (ndc.x * 0.5 + 0.5) * width
 *        sy = (ndc.y * 0.5 + 0.5) * height
 *
 *    with x right and y up.  Pixel (x, y) is the half-open cell
 *    [x, x+1) x [y, y+1) with centre (x + 0.5, y + 0.5).  Storage is
 *    row-major, index y * width + x.  PPM output is the only place where
 *    rows are flipped (PPM is top-down) and colours quantised.
 *  - Input geometry is world space.  CaptureSettings supplies world-to-view,
 *    projection and normal (world-to-view) matrices.  The caller guarantees
 *    finite vertices inside the frustum with positive clip w; the new path
 *    performs no additional clipping.
 *
 * Geometry
 * --------
 * The background is an opaque triangle mesh; glass is one closed convex,
 * non-self-intersecting mesh.  Glass fragments are split by projected
 * winding: front faces fill the front layer, back faces (which the old
 * rasterizer culled) fill the back layer.  Positions and normals use 1/w
 * perspective-correct interpolation; normals are normalised only after
 * interpolation.  Each layer keeps the nearest fragment per pixel plus a
 * validity flag.
 *
 * Depth fields / ray tracing
 * --------------------------
 * A depth cell is valid when a captured fragment projects into that pixel's
 * half-open cell and its view depth is the value stored there.  A ray is
 * clipped to the capture's finite near/far range and then walked cell by
 * cell through the projected grid (at most width + height + 1 cells per
 * segment, each visited once).  Inside a cell the intersection uses the
 * continuous ray parameter: the ray's linearly varying depth is matched to
 * the cell's recorded depth plane and the projected hit must land in the
 * same cell.  No fixed-step sampling is used.
 *
 * Composition approximation
 * -------------------------
 * One transmission path, exact unpolarised Fresnel at both interfaces, with
 * no further internal reflection:
 *
 *     colour = R_entrance * environment
 *            + T_entrance * T_exit * exp(-sigma_a * internal_length)
 *              * background_colour
 *
 * Interior background misses use the environment colour.  A missing exit or
 * exit total internal reflection drops the transmission term and keeps the
 * entrance reflection.  An entrance hidden behind the opaque background keeps
 * the background pixel.  The internal length is the true entrance-to-exit
 * distance along the refracted ray, never a same-pixel depth difference.
 *
 * Captures stay read-only while composing and the returned colour storage is
 * freshly allocated.  Invalid parameters or captures are rejected before any
 * existing output is modified.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gfx {
namespace refraction {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline auto operator+(const Vec3& a, const Vec3& b) -> Vec3 {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline auto operator-(const Vec3& a, const Vec3& b) -> Vec3 {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline auto operator*(const Vec3& a, double s) -> Vec3 {
    return {a.x * s, a.y * s, a.z * s};
}
inline auto operator*(double s, const Vec3& a) -> Vec3 { return a * s; }
inline auto Dot(const Vec3& a, const Vec3& b) -> double {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline auto Cross(const Vec3& a, const Vec3& b) -> Vec3 {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
inline auto Length(const Vec3& v) -> double { return std::sqrt(Dot(v, v)); }

/* Row-major 4x4 matrix, column-vector convention. */
struct Mat4 {
    std::array<double, 16> value{};

    auto operator()(std::size_t row, std::size_t col) -> double& {
        return value[row * 4 + col];
    }
    auto operator()(std::size_t row, std::size_t col) const -> double {
        return value[row * 4 + col];
    }
    static auto Identity() -> Mat4;
};

struct Vec4 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;
};

auto Transform(const Mat4& m, const Vec4& v) -> Vec4;
auto Normalize(const Vec3& v) -> Vec3;

/*
 * Standard perspective projection with NDC z in [0, 1] and clip.w = -view.z.
 * left/right/bottom/top are the near-plane extents in view space and may be
 * asymmetric.  near_depth and far_depth are positive view depths.
 */
auto PerspectiveProjection(double left, double right, double bottom,
                           double top, double near_depth, double far_depth)
    -> Mat4;

/* Derive the finite near/far view depths from a projection of the form above.
 * Returns false if the matrix does not encode a finite range. */
auto DeriveDepthRange(const Mat4& projection, double& near_depth,
                      double& far_depth) -> bool;

struct Vertex {
    Vec3 position;
    Vec3 normal{0.0, 0.0, 1.0};
    Vec3 color{1.0, 1.0, 1.0}; /* linear RGB; used by background capture */
};

struct Triangle {
    Vertex vertex[3];
};

struct Mesh {
    std::vector<Triangle> triangles;
};

struct CaptureSettings {
    int width = 0;
    int height = 0;
    Mat4 world_to_view = Mat4::Identity();
    Mat4 projection = Mat4::Identity();
    /* World-to-view matrix for normals (inverse transpose of the model/view
     * transform). */
    Mat4 normal_to_view = Mat4::Identity();
    /* Positive view depths.  If either is not positive the finite range is
     * derived from the projection matrix. */
    double near_depth = 0.0;
    double far_depth = 0.0;
};

/* Opaque background: linear RGB and positive view depth.  A cell is meaningful
 * only where valid[i] != 0. */
struct ImageCapture {
    int width = 0;
    int height = 0;
    std::vector<double> color;  /* 3 * width * height, linear RGB */
    std::vector<double> depth;  /* width * height, positive view depth */
    std::vector<std::uint8_t> valid;
    Mat4 projection = Mat4::Identity();
    double near_depth = 0.0;
    double far_depth = 0.0;
};

/* Glass front and back layers.  Positions are view-space positions, normals
 * are unit outward normals, both perspective-correct interpolated.  The
 * derived depth arrays hold positive view depth (-z) for tracing. */
struct GlassCapture {
    int width = 0;
    int height = 0;
    std::vector<Vec3> front_position;
    std::vector<Vec3> front_normal;
    std::vector<Vec3> back_position;
    std::vector<Vec3> back_normal;
    std::vector<std::uint8_t> front_valid;
    std::vector<std::uint8_t> back_valid;
    std::vector<double> front_depth;
    std::vector<double> back_depth;
    Mat4 projection = Mat4::Identity();
    double near_depth = 0.0;
    double far_depth = 0.0;
};

struct SceneCapture {
    ImageCapture background;
    GlassCapture glass;
};

/*
 * Real off-screen captures.  Vertices must be finite, inside the frustum and
 * have positive w.  On failure the output argument is left untouched and, if
 * error is non-null, a diagnostic is stored.
 */
auto CaptureBackground(const Mesh& mesh, const CaptureSettings& settings,
                       ImageCapture& out, std::string* error = nullptr) -> bool;
auto CaptureGlass(const Mesh& mesh, const CaptureSettings& settings,
                  GlassCapture& out, std::string* error = nullptr) -> bool;
auto CaptureScene(const Mesh& background, const Mesh& glass,
                  const CaptureSettings& settings, SceneCapture& out,
                  std::string* error = nullptr) -> bool;

/* A read-only view of one captured depth field. */
struct DepthFieldView {
    int width = 0;
    int height = 0;
    const double* depth = nullptr; /* positive view depth */
    const std::uint8_t* valid = nullptr;
    Mat4 projection = Mat4::Identity();
    double near_depth = 0.0;
    double far_depth = 0.0;
};

auto BackgroundField(const ImageCapture& capture) -> DepthFieldView;
auto GlassFrontField(const GlassCapture& capture) -> DepthFieldView;
auto GlassBackField(const GlassCapture& capture) -> DepthFieldView;

struct RayHit {
    bool hit = false;
    int pixel_x = -1;
    int pixel_y = -1;
    /* Continuous pixel coordinates of the hit (pixel units, centre at
     * x + 0.5, y + 0.5). */
    double u = 0.0;
    double v = 0.0;
    /* View-space distance from the ray origin to the hit, in the same units
     * as the view-space positions. */
    double distance = 0.0;
    std::size_t visited_cells = 0;
};

/*
 * Trace one ray (origin and direction in view space) through a captured depth
 * field.  direction need not be normalised.  The returned bool reports
 * whether the inputs were valid; a valid trace that misses returns true with
 * hit.hit == false.  Identical invalid arguments leave hit untouched.
 */
auto TraceDepthField(const DepthFieldView& field, const Vec3& origin,
                     const Vec3& direction, RayHit& hit,
                     std::string* error = nullptr) -> bool;

/* Exact unpolarised Fresnel + Snell for one interface.
 *
 * incident is the unit direction towards the surface.  normal is the unit
 * surface normal; it is oriented internally so that it opposes the incident
 * ray.  eta is n_incident / n_transmitted.  On total internal reflection
 * reflectance is 1, transmittance 0 and transmission_direction is zero.
 */
struct FresnelSample {
    double reflectance = 0.0;
    double transmittance = 0.0;
    bool total_internal_reflection = false;
    Vec3 transmission_direction{};
};

auto FresnelRefract(const Vec3& incident, const Vec3& normal, double eta)
    -> FresnelSample;

struct Material {
    /* Index of refraction of the glass; air is assumed to be 1.  Must be
     * finite and in [1.01, 2.5]. */
    double ior = 1.5;
    /* Per-channel absorption coefficient, >= 0. */
    Vec3 sigma_a{};
    /* Environment colour used for reflections and for background misses.
     * Finite and >= 0 per channel. */
    Vec3 environment{};
};

struct PixelTrace {
    bool entrance_visible = false;
    bool background_kept = false;
    bool exit_found = false;
    bool background_found = false;
    /* Camera origin -> glass entrance distance. */
    double entrance_distance = 0.0;
    /* Glass back layer trace.  distance is the true internal segment length. */
    RayHit exit_hit;
    /* Background trace after the exit interface.  distance is the external
     * segment length from the exit point. */
    RayHit background_hit;
};

struct CompositeResult {
    int width = 0;
    int height = 0;
    std::vector<double> color; /* 3 * width * height, linear RGB */
    std::vector<PixelTrace> pixels;
};

/*
 * Compose the scene.  Captures and material are read-only; out is replaced
 * only after every parameter and capture has been validated.
 */
auto Compose(const SceneCapture& scene, const Material& material,
             CompositeResult& out, std::string* error = nullptr) -> bool;

/*
 * Quantise linear RGB to 8 bits, clamp, and flip rows for PPM.  Quantisation
 * happens only here.  Validates sizes and finiteness before touching the
 * file.
 */
auto WritePPM(const std::string& filename, int width, int height,
              const std::vector<double>& linear_rgb,
              std::string* error = nullptr) -> bool;
auto WritePPM(const std::string& filename, const CompositeResult& result,
              std::string* error = nullptr) -> bool;

}  // namespace refraction
}  // namespace gfx
