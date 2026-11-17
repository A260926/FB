#pragma once

// Headless thick-glass refraction preview.
//
// All routines in this header are windowless: they never touch SDL, a GPU or
// any dependency beyond the C++20 standard library and src/math.h.  The old
// interactive program (src/rasterizer.cpp) keeps its own lighting, depth
// comparison, pixel packing and model loading; this module only shares its
// fill-triangle rasterization conventions.
//
// Coordinate conventions
// ----------------------
// * World space is right handed.  The caller supplies a world->view matrix;
//   in view space the camera sits at the origin and looks down -Z.
// * The caller supplies a perspective projection matrix.  It must map the
//   finite view-space frustum to clip space with clip.w == -view.z, and the
//   perspective divide must map positive view depth to NDC z in [0, 1] and
//   NDC x/y in [-1, 1] (asymmetric frusta are supported, so the x/y centres
//   need not be zero).
// * The caller supplies a normal matrix that maps a world-space direction to
//   the corresponding view-space direction; the mapped normals are normalized
//   after interpolation.
// * Input vertices are assumed finite, already inside the frustum and to have
//   positive w after projection.  No clipping is performed.
//
// Offscreen target conventions
// ----------------------------
// * A target has width x height pixels, index y * width + x with y growing
//   upwards and x growing rightwards.  Pixel (x, y) is centred at the
//   continuous target coordinate (x + 0.5, y + 0.5).  A projected point with
//   target coordinate (px, py) belongs to the half-open cell
//   [floor(px), floor(px)+1) x [floor(py), floor(py)+1).
// * PPM output flips the row order so the first file row is the top row.
//
// Transport approximation
// -----------------------
// One transmission only: the camera ray is refracted into the glass at the
// front entry, traced through the back depth field, refracted out at the exit
// and intersected once with the opaque background.  Internal reflections are
// not followed.  The per-pixel colour is
//
//   F_entry * ambient
//     + T_entry * T_exit * exp(-sigmaA * internal_path_length) * background
//
// with exact unpolarized Fresnel terms at both interfaces.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "math.h"

namespace gfx::refr {

using std::size_t;

// Three-component linear RGB / Euclidean vector, stored separately from the
// SDL-oriented four-component gfx::Vec.
struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit Vec3(const Vec& v) : x(v[0]), y(v[1]), z(v[2]) {}

    auto operator+(const Vec3& o) const -> Vec3 { return {x + o.x, y + o.y, z + o.z}; }
    auto operator-(const Vec3& o) const -> Vec3 { return {x - o.x, y - o.y, z - o.z}; }
    auto operator*(float s) const -> Vec3 { return {x * s, y * s, z * s}; }
    auto operator*(const Vec3& o) const -> Vec3 { return {x * o.x, y * o.y, z * o.z}; }
    auto operator+=(const Vec3& o) -> Vec3& { x += o.x; y += o.y; z += o.z; return *this; }
};

auto Dot(const Vec3& a, const Vec3& b) -> float;
auto Cross(const Vec3& a, const Vec3& b) -> Vec3;
auto Length(const Vec3& a) -> float;
auto Normalize(const Vec3& a) -> Vec3;
auto ToVec4(const Vec3& v, float w) -> Vec;
auto IdentityMat4() -> Mat4;

// Perspective projection for a finite frustum.  Near/far are positive eye
// depths; l/r/b/t are the NDC [-1,1] bounds on the near plane.  The resulting
// matrix has clip.w == -view.z and maps positive eye depth to NDC z in
// [0, 1]; asymmetric frusta (l != -r or b != -t) are supported.
auto MakePerspective(float l, float r, float b, float t, float n, float f) -> Mat4;

// A single filled triangle with world-space positions and outward normals.
struct Triangle {
    Vec position[3];
    Vec normal[3];
};

// What a depth-field cell records for its nearest covering fragment.
struct CellRecord {
    bool valid = false;
    float view_depth = 0.f;   // positive eye-space depth, -view_position.z
    Vec3 position;            // perspective-correct interpolated view position
    Vec3 normal;              // ditto, normalized after interpolation
};

// Which winding a fill pass keeps.  The original rasterizer discards
// screen-space clockwise fragments (back-face culling); the glass back layer
// must instead keep exactly those back faces, so culling is inverted rather
// than simply switched off.
enum class FaceMode {
    FrontFaces, // keep screen-space CCW fragments (original behaviour)
    BackFaces,  // keep screen-space CW fragments (glass exit shell capture)
    AllFaces    // no winding test
};

// A reusable, size-configurable offscreen capture target.  Each layer is an
// independent y*width+x array of CellRecord cells.
class DepthTarget {
public:
    DepthTarget(int width, int height);

    auto width() const -> int { return width_; }
    auto height() const -> int { return height_; }
    auto size() const -> size_t { return static_cast<size_t>(width_) * height_; }
    auto cell(int x, int y) const -> const CellRecord& { return cells_[y * width_ + x]; }
    auto cell(int x, int y) -> CellRecord& { return cells_[y * width_ + x]; }

    // Rasterize one filled triangle.  Positions use 1/w perspective
    // interpolation; only normals are re-normalized afterwards.  The nearest
    // fragment per cell wins with the original rasterizer's "write when the
    // new depth is not greater than the stored one" rule.
    void RasterizeTriangle(const Mat4& view, const Mat4& projection, const Mat4& normal_matrix,
                           const Triangle& triangle, FaceMode mode);

    // True when every cell that could be read is valid and finite.
    auto IsComplete() const -> bool;
    void Clear();

private:
    int width_;
    int height_;
    std::vector<CellRecord> cells_;
};

// Opaque background: linear RGB plus an eye-space positive depth per cell.
class BackgroundTarget {
public:
    BackgroundTarget(int width, int height);

    auto width() const -> int { return width_; }
    auto height() const -> int { return height_; }
    auto size() const -> size_t { return static_cast<size_t>(width_) * height_; }
    auto color(int x, int y) const -> const Vec3& { return colors_[y * width_ + x]; }
    // Positive eye-space depth (-view z) of the nearest covering fragment.
    auto depth(int x, int y) const -> float { return depths_[y * width_ + x]; }
    auto valid(int x, int y) const -> bool { return valid_[y * width_ + x]; }

    void RasterizeTriangle(const Mat4& view, const Mat4& projection,
                           const Triangle& triangle, const Vec3& linear_color);
    auto IsComplete() const -> bool;
    void Clear();

private:
    int width_;
    int height_;
    std::vector<Vec3> colors_;
    std::vector<float> depths_;
    std::vector<uint8_t> valid_;
};

// Why a depth-field ray walk ended.  Total internal reflection is decided at
// refraction time in Synthesize, so the field walk only reports hit/miss.
enum class TraceStatus {
    ExitFound,   // a valid exit cell was intersected at t > 0
    ExitMissing  // the ray left the frustum without any hit
};

// Result of walking the back depth field from an entry point.
struct ExitTrace {
    TraceStatus status = TraceStatus::ExitMissing;
    bool hit = false;
    int cell_x = -1;        // exit cell, -1 when no hit
    int cell_y = -1;
    double ndc_x = 0.;      // continuous NDC coordinate of the hit
    double ndc_y = 0.;
    Vec3 exit_position;     // view-space hit point
    Vec3 exit_normal;       // stored unit outward normal at the exit cell
    double entry_distance = 0.;    // |entry - camera| along the camera ray
    double internal_distance = 0.; // view-space length entry -> exit
    double exit_t = 0.;            // ray parameter at the exit, 0 when absent
    size_t cells_visited = 0;
};

// Result of walking the opaque background after leaving the glass.
struct BackgroundTrace {
    bool hit = false;
    int cell_x = -1;
    int cell_y = -1;
    double ndc_x = 0.;
    double ndc_y = 0.;
    double distance = 0.;   // view-space length from the ray origin
    size_t cells_visited = 0;
    Vec3 color;             // captured linear RGB, valid only when hit
};

// Material description of the glass.
struct GlassMaterial {
    float ior = 1.5f;       // finite, in [1.01, 2.5]
    Vec3 sigma_a{0.f, 0.f, 0.f}; // per-channel absorption, non-negative
};

// Aggregate statistics for a full synthesis pass.
struct SynthesisStats {
    size_t entry_pixels = 0;
    size_t exit_hits = 0;
    size_t exit_missing = 0;
    size_t total_internal_reflection = 0;
    size_t background_hits = 0;
    size_t background_misses = 0;
    size_t occluded_entries = 0; // entry behind the opaque background
    size_t max_cells_per_segment = 0;
};

struct SynthesisResult {
    std::vector<Vec3> color; // independent storage, width*height, y-up indexing
    SynthesisStats stats;
};

// Snell refraction of an incident unit vector against an interface with the
// given unit outward normal.  eta is eta_incident / eta_transmitted.  Returns
// false (with cos_i set to the incident angle cosine) on total internal
// reflection.  Exact vector form, no small-angle approximation.
auto RefractSnell(const Vec3& incident, const Vec3& outward_normal, float eta,
                  Vec3* refracted, float* cos_incident) -> bool;

// Unpolarized Fresnel reflectance at a dielectric interface.  eta is
// eta_incident / eta_transmitted; cos_incident is the angle on the incident
// side against the outward normal.  Handles total internal reflection by
// returning 1.
auto FresnelUnpolarized(float cos_incident, float eta) -> float;

// Walk an already-refracted view-space ray through a captured depth field.
// origin is on the entry interface; the ray's self-intersection at t == 0 is
// ignored.  The ray is first clipped to the finite near/far frustum recorded
// at capture time; the walk stops at the image border, and each cell is
// visited at most once.  Hits come from the continuous ray parameter inside
// each cell, never from fixed stepping.  cells_visited never exceeds
// width + height + 1.
auto TraceDepthField(const DepthTarget& field, const Mat4& projection,
                     const Vec3& origin, const Vec3& direction) -> ExitTrace;

// Walk a view-space ray through the opaque background layer.
auto TraceBackground(const BackgroundTarget& background, const Mat4& projection,
                     const Vec3& origin, const Vec3& direction) -> BackgroundTrace;

// Full headless composite.  front/back are the nearest front-face and
// back-face captures of the same closed convex glass mesh; the camera is
// assumed outside the glass.  Entries hidden behind the opaque background keep
// the background colour.  The three capture layers are only read; the returned
// colour buffer is freshly allocated, so repeated calls, changing sigma_a or
// interleaving captures never feed back into the inputs.  Throws
// std::invalid_argument (before any result is produced) when an argument is
// invalid.
auto Synthesize(const DepthTarget& front, const DepthTarget& back,
                const BackgroundTarget& background, const Mat4& projection,
                const GlassMaterial& material, const Vec3& ambient) -> SynthesisResult;

// Quantize linear RGB to [0,255] only at file time and write binary P6 PPM,
// flipping the y-up storage into top-down file rows.
void WritePPM(const std::string& path, const std::vector<Vec3>& linear,
              int width, int height);

} // namespace gfx::refr
