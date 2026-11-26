#include "refraction.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <queue>

namespace gfx {
namespace refraction {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

auto Finite(double v) -> bool { return std::isfinite(v); }
auto Finite(const Vec3& v) -> bool {
    return Finite(v.x) && Finite(v.y) && Finite(v.z);
}
auto Finite(const Vec4& v) -> bool {
    return Finite(v.x) && Finite(v.y) && Finite(v.z) && Finite(v.w);
}
auto Finite(const Mat4& m) -> bool {
    for (double v : m.value) {
        if (!Finite(v)) return false;
    }
    return true;
}
auto NonNegative(const Vec3& v) -> bool {
    return Finite(v) && v.x >= 0.0 && v.y >= 0.0 && v.z >= 0.0;
}

auto SetError(std::string* error, const char* message) -> bool {
    if (error) *error = message;
    return false;
}

/* Validates that a background capture has consistent, finite contents. */
auto ValidateImageCapture(const ImageCapture& capture, std::string* error)
    -> bool {
    if (capture.width <= 0 || capture.height <= 0)
        return SetError(error, "background capture has non-positive size");
    const std::size_t cells =
        static_cast<std::size_t>(capture.width) * capture.height;
    if (capture.color.size() != 3 * cells || capture.depth.size() != cells ||
        capture.valid.size() != cells)
        return SetError(error, "background capture array size mismatch");
    if (!Finite(capture.projection))
        return SetError(error, "background projection is not finite");
    if (!(capture.near_depth > 0.0) ||
        !(capture.far_depth > capture.near_depth) ||
        !Finite(capture.near_depth) || !Finite(capture.far_depth))
        return SetError(error, "background depth range is invalid");
    for (std::size_t i = 0; i < cells; ++i) {
        if (!capture.valid[i]) continue;
        if (!Finite(capture.depth[i]) || !(capture.depth[i] > 0.0))
            return SetError(error, "background depth is invalid");
        for (int c = 0; c < 3; ++c) {
            const double v = capture.color[3 * i + c];
            if (!Finite(v) || v < 0.0)
                return SetError(error, "background colour is invalid");
        }
    }
    return true;
}

auto ValidateGlassCapture(const GlassCapture& capture, std::string* error)
    -> bool {
    if (capture.width <= 0 || capture.height <= 0)
        return SetError(error, "glass capture has non-positive size");
    const std::size_t cells =
        static_cast<std::size_t>(capture.width) * capture.height;
    if (capture.front_position.size() != cells ||
        capture.front_normal.size() != cells ||
        capture.back_position.size() != cells ||
        capture.back_normal.size() != cells ||
        capture.front_valid.size() != cells ||
        capture.back_valid.size() != cells ||
        capture.front_depth.size() != cells ||
        capture.back_depth.size() != cells)
        return SetError(error, "glass capture array size mismatch");
    if (!Finite(capture.projection))
        return SetError(error, "glass projection is not finite");
    if (!(capture.near_depth > 0.0) ||
        !(capture.far_depth > capture.near_depth) ||
        !Finite(capture.near_depth) || !Finite(capture.far_depth))
        return SetError(error, "glass depth range is invalid");
    for (std::size_t i = 0; i < cells; ++i) {
        if (capture.front_valid[i]) {
            if (!Finite(capture.front_position[i]) ||
                !Finite(capture.front_normal[i]) ||
                !Finite(capture.front_depth[i]) ||
                !(capture.front_depth[i] > 0.0))
                return SetError(error, "glass front sample is invalid");
            if (Length(capture.front_normal[i]) < 1e-9)
                return SetError(error, "glass front normal is degenerate");
        }
        if (capture.back_valid[i]) {
            if (!Finite(capture.back_position[i]) ||
                !Finite(capture.back_normal[i]) ||
                !Finite(capture.back_depth[i]) ||
                !(capture.back_depth[i] > 0.0))
                return SetError(error, "glass back sample is invalid");
            if (Length(capture.back_normal[i]) < 1e-9)
                return SetError(error, "glass back normal is degenerate");
        }
    }
    return true;
}

auto ValidateSettings(const CaptureSettings& settings, std::string* error)
    -> bool {
    if (settings.width <= 0 || settings.height <= 0)
        return SetError(error, "capture size must be positive");
    if (!Finite(settings.world_to_view) || !Finite(settings.projection) ||
        !Finite(settings.normal_to_view))
        return SetError(error, "capture matrix is not finite");
    return true;
}

/* Projected vertex used by the reusable triangle traversal. */
struct ScreenVertex {
    Vec3 view;
    Vec3 normal_view;
    Vec3 color;
    double sx = 0.0;
    double sy = 0.0;
    double invw = 0.0;
    double ndc_z = 0.0;
};

auto MakeScreenVertex(const CaptureSettings& settings, const Vertex& vertex,
                      ScreenVertex& out) -> bool {
    if (!Finite(vertex.position) || !Finite(vertex.normal) ||
        !Finite(vertex.color))
        return false;

    const Vec4 world{vertex.position.x, vertex.position.y, vertex.position.z,
                     1.0};
    const Vec4 view = Transform(settings.world_to_view, world);
    const Vec4 clip = Transform(settings.projection, view);
    if (!Finite(view) || !Finite(clip)) return false;
    if (!(clip.w > 0.0)) return false;

    const double invw = 1.0 / clip.w;
    out.view = {view.x, view.y, view.z};
    out.color = vertex.color;

    const Vec4 n = Transform(
        settings.normal_to_view,
        Vec4{vertex.normal.x, vertex.normal.y, vertex.normal.z, 0.0});
    if (!Finite(n)) return false;
    /* Keep the transformed normal unnormalised: the fragment interpolates
     * 1/w-weighted normals and normalises only after interpolation. */
    out.normal_view = {n.x, n.y, n.z};
    if (!(Length(out.normal_view) > 1e-12)) return false;

    out.sx = (clip.x * invw * 0.5 + 0.5) * settings.width;
    out.sy = (clip.y * invw * 0.5 + 0.5) * settings.height;
    out.invw = invw;
    out.ndc_z = clip.z * invw;
    return Finite(out.sx) && Finite(out.sy) && Finite(out.ndc_z) &&
           Finite(out.invw);
}

auto Orient2D(double ax, double ay, double bx, double by, double px,
              double py) -> double {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/*
 * Shared triangle traversal.  Calls `emit(x, y, l0, l1, l2, a, b, c)` for
 * every pixel whose centre falls in the projected triangle.  l0..l2 are the
 * screen-space barycentric weights.  The caller applies perspective-correct
 * interpolation and its own depth/ownership rules.
 */
template <typename Emit>
void TraverseTriangle(int width, int height, const ScreenVertex& a,
                      const ScreenVertex& b, const ScreenVertex& c,
                      Emit&& emit) {
    const double area = Orient2D(a.sx, a.sy, b.sx, b.sy, c.sx, c.sy);
    if (!Finite(area) || std::abs(area) < 1e-14) return;

    const double min_x = std::min({a.sx, b.sx, c.sx});
    const double max_x = std::max({a.sx, b.sx, c.sx});
    const double min_y = std::min({a.sy, b.sy, c.sy});
    const double max_y = std::max({a.sy, b.sy, c.sy});

    const int x_begin = std::max(0, static_cast<int>(std::floor(min_x)));
    const int x_end = std::min(width - 1, static_cast<int>(std::floor(max_x)));
    const int y_begin = std::max(0, static_cast<int>(std::floor(min_y)));
    const int y_end = std::min(height - 1, static_cast<int>(std::floor(max_y)));
    if (x_begin > x_end || y_begin > y_end) return;

    const double edge_eps = 1e-12 * (std::abs(area) + 1.0);
    const bool positive = area > 0.0;

    for (int y = y_begin; y <= y_end; ++y) {
        const double py = y + 0.5;
        for (int x = x_begin; x <= x_end; ++x) {
            const double px = x + 0.5;
            /* Weights follow the vertices: e0 belongs to a, e1 to b, e2 to
             * c.  orient(b, c, p) is proportional to a's weight. */
            const double e0 = Orient2D(b.sx, b.sy, c.sx, c.sy, px, py);
            const double e1 = Orient2D(c.sx, c.sy, a.sx, a.sy, px, py);
            const double e2 = Orient2D(a.sx, a.sy, b.sx, b.sy, px, py);
            if (positive) {
                if (e0 < -edge_eps || e1 < -edge_eps || e2 < -edge_eps)
                    continue;
            } else {
                if (e0 > edge_eps || e1 > edge_eps || e2 > edge_eps) continue;
            }
            const double l0 = e0 / area;
            const double l1 = e1 / area;
            const double l2 = e2 / area;
            emit(x, y, l0, l1, l2, a, b, c);
        }
    }
}

auto ResolveDepthRange(const CaptureSettings& settings, double& near_depth,
                       double& far_depth) -> bool {
    if (settings.near_depth > 0.0 &&
        settings.far_depth > settings.near_depth &&
        Finite(settings.near_depth) && Finite(settings.far_depth)) {
        near_depth = settings.near_depth;
        far_depth = settings.far_depth;
        return true;
    }
    return DeriveDepthRange(settings.projection, near_depth, far_depth);
}

struct TraceEntry {
    double t;
    int x;
    int y;
    bool operator<(const TraceEntry& other) const {
        return t > other.t; /* std::priority_queue keeps the smallest t. */
    }
};

auto RowDot(const Mat4& m, int row, const Vec3& p, double w) -> double {
    return m(row, 0) * p.x + m(row, 1) * p.y + m(row, 2) * p.z +
           m(row, 3) * w;
}
auto RowDotDir(const Mat4& m, int row, const Vec3& d) -> double {
    return m(row, 0) * d.x + m(row, 1) * d.y + m(row, 2) * d.z;
}

/* Solve C(t) = target * w(t) for the linear clip forms C and w. */
auto BoundaryTime(double c0, double c1, double w0, double w1, double target)
    -> std::optional<double> {
    const double denom = c1 - target * w1;
    if (std::abs(denom) < 1e-15) return std::nullopt;
    const double t = (target * w0 - c0) / denom;
    if (!Finite(t)) return std::nullopt;
    return t;
}

}  // namespace

auto Mat4::Identity() -> Mat4 {
    Mat4 m;
    m(0, 0) = 1.0;
    m(1, 1) = 1.0;
    m(2, 2) = 1.0;
    m(3, 3) = 1.0;
    return m;
}

auto Transform(const Mat4& m, const Vec4& v) -> Vec4 {
    return {m(0, 0) * v.x + m(0, 1) * v.y + m(0, 2) * v.z + m(0, 3) * v.w,
            m(1, 0) * v.x + m(1, 1) * v.y + m(1, 2) * v.z + m(1, 3) * v.w,
            m(2, 0) * v.x + m(2, 1) * v.y + m(2, 2) * v.z + m(2, 3) * v.w,
            m(3, 0) * v.x + m(3, 1) * v.y + m(3, 2) * v.z + m(3, 3) * v.w};
}

auto Normalize(const Vec3& v) -> Vec3 {
    const double len = Length(v);
    if (!(len > 0.0)) return {0.0, 0.0, 0.0};
    return v * (1.0 / len);
}

auto PerspectiveProjection(double left, double right, double bottom,
                           double top, double near_depth, double far_depth)
    -> Mat4 {
    Mat4 m;
    m(0, 0) = 2.0 * near_depth / (right - left);
    m(0, 2) = (right + left) / (right - left);
    m(1, 1) = 2.0 * near_depth / (top - bottom);
    m(1, 2) = (top + bottom) / (top - bottom);
    /* View depth d = -z maps to ndc z: 0 at near, 1 at far. */
    m(2, 2) = -far_depth / (far_depth - near_depth);
    m(2, 3) = -far_depth * near_depth / (far_depth - near_depth);
    m(3, 2) = -1.0;
    return m;
}

auto DeriveDepthRange(const Mat4& projection, double& near_depth,
                      double& far_depth) -> bool {
    if (!Finite(projection)) return false;
    /* Only the conventional perspective form (row 3 = [0, 0, -1, 0]) is
     * supported. */
    if (std::abs(projection(3, 0)) > 1e-9 ||
        std::abs(projection(3, 1)) > 1e-9 ||
        std::abs(projection(3, 2) + 1.0) > 1e-9 ||
        std::abs(projection(3, 3)) > 1e-9)
        return false;
    if (std::abs(projection(2, 0)) > 1e-9 ||
        std::abs(projection(2, 1)) > 1e-9)
        return false;
    const double e = projection(2, 2);
    const double f = projection(2, 3);
    if (std::abs(e) < 1e-12 || std::abs(1.0 + e) < 1e-12) return false;
    const double n = f / e;
    const double far = f / (1.0 + e);
    if (!Finite(n) || !Finite(far) || !(n > 0.0) || !(far > n)) return false;
    near_depth = n;
    far_depth = far;
    return true;
}

auto CaptureBackground(const Mesh& mesh, const CaptureSettings& settings,
                       ImageCapture& out, std::string* error) -> bool {
    if (!ValidateSettings(settings, error)) return false;
    double near_depth = 0.0, far_depth = 0.0;
    if (!ResolveDepthRange(settings, near_depth, far_depth))
        return SetError(error, "cannot determine background depth range");

    const int width = settings.width;
    const int height = settings.height;
    const std::size_t cells = static_cast<std::size_t>(width) * height;

    ImageCapture result;
    result.width = width;
    result.height = height;
    result.color.assign(3 * cells, 0.0);
    result.depth.assign(cells, kInf);
    result.valid.assign(cells, 0);
    result.projection = settings.projection;
    result.near_depth = near_depth;
    result.far_depth = far_depth;

    for (const Triangle& tri : mesh.triangles) {
        ScreenVertex sv[3];
        for (int i = 0; i < 3; ++i) {
            if (!MakeScreenVertex(settings, tri.vertex[i], sv[i]))
                return SetError(error,
                                "background vertex is invalid or behind camera");
        }
        TraverseTriangle(width, height, sv[0], sv[1], sv[2],
                         [&](int x, int y, double l0, double l1, double l2,
                             const ScreenVertex& a, const ScreenVertex& b,
                             const ScreenVertex& c) {
                             const double invw = l0 * a.invw + l1 * b.invw +
                                                 l2 * c.invw;
                             if (!Finite(invw) || !(invw > 0.0)) return;
                             const double depth = 1.0 / invw;
                             const std::size_t idx =
                                 static_cast<std::size_t>(y) * width + x;
                             if (!(depth < result.depth[idx])) return;
                             const double w0 = l0 * a.invw / invw;
                             const double w1 = l1 * b.invw / invw;
                             const double w2 = l2 * c.invw / invw;
                             result.depth[idx] = depth;
                             const double channels[3][3] = {
                                 {a.color.x, a.color.y, a.color.z},
                                 {b.color.x, b.color.y, b.color.z},
                                 {c.color.x, c.color.y, c.color.z}};
                             for (int ch = 0; ch < 3; ++ch) {
                                 result.color[3 * idx + ch] =
                                     w0 * channels[0][ch] +
                                     w1 * channels[1][ch] +
                                     w2 * channels[2][ch];
                             }
                             result.valid[idx] = 1;
                         });
    }
    out = std::move(result);
    return true;
}

auto CaptureGlass(const Mesh& mesh, const CaptureSettings& settings,
                  GlassCapture& out, std::string* error) -> bool {
    if (!ValidateSettings(settings, error)) return false;
    double near_depth = 0.0, far_depth = 0.0;
    if (!ResolveDepthRange(settings, near_depth, far_depth))
        return SetError(error, "cannot determine glass depth range");

    const int width = settings.width;
    const int height = settings.height;
    const std::size_t cells = static_cast<std::size_t>(width) * height;

    GlassCapture result;
    result.width = width;
    result.height = height;
    result.front_position.assign(cells, Vec3{});
    result.front_normal.assign(cells, Vec3{});
    result.back_position.assign(cells, Vec3{});
    result.back_normal.assign(cells, Vec3{});
    result.front_valid.assign(cells, 0);
    result.back_valid.assign(cells, 0);
    result.front_depth.assign(cells, kInf);
    result.back_depth.assign(cells, kInf);
    result.projection = settings.projection;
    result.near_depth = near_depth;
    result.far_depth = far_depth;

    for (const Triangle& tri : mesh.triangles) {
        ScreenVertex sv[3];
        for (int i = 0; i < 3; ++i) {
            if (!MakeScreenVertex(settings, tri.vertex[i], sv[i]))
                return SetError(error,
                                "glass vertex is invalid or behind camera");
        }
        TraverseTriangle(
            width, height, sv[0], sv[1], sv[2],
            [&](int x, int y, double l0, double l1, double l2,
                const ScreenVertex& a, const ScreenVertex& b,
                const ScreenVertex& c) {
                const double invw = l0 * a.invw + l1 * b.invw + l2 * c.invw;
                if (!Finite(invw) || !(invw > 0.0)) return;
                const double depth = 1.0 / invw;
                const std::size_t idx = static_cast<std::size_t>(y) * width + x;

                /* Winding sign in the y-up NDC frame: outward CCW faces are
                 * front-facing, CW faces are the far side of the glass. */
                const double area =
                    Orient2D(a.sx, a.sy, b.sx, b.sy, c.sx, c.sy);
                const bool front = area > 0.0;

                const double w0 = l0 * a.invw / invw;
                const double w1 = l1 * b.invw / invw;
                const double w2 = l2 * c.invw / invw;
                const Vec3 pos = a.view * w0 + b.view * w1 + c.view * w2;
                const Vec3 nrm = Normalize(a.normal_view * w0 +
                                           b.normal_view * w1 +
                                           c.normal_view * w2);
                if (!Finite(pos) || !Finite(nrm) || Length(nrm) < 1e-9)
                    return;

                Vec3& stored_pos = front ? result.front_position[idx]
                                         : result.back_position[idx];
                Vec3& stored_nrm = front ? result.front_normal[idx]
                                         : result.back_normal[idx];
                double& stored_depth = front ? result.front_depth[idx]
                                             : result.back_depth[idx];
                std::uint8_t& stored_valid = front ? result.front_valid[idx]
                                                   : result.back_valid[idx];
                if (!(depth < stored_depth)) return;
                stored_depth = depth;
                stored_pos = pos;
                stored_nrm = nrm;
                stored_valid = 1;
            });
    }
    out = std::move(result);
    return true;
}

auto CaptureScene(const Mesh& background, const Mesh& glass,
                  const CaptureSettings& settings, SceneCapture& out,
                  std::string* error) -> bool {
    ImageCapture bg;
    GlassCapture gl;
    if (!CaptureBackground(background, settings, bg, error)) return false;
    if (!CaptureGlass(glass, settings, gl, error)) return false;
    SceneCapture result;
    result.background = std::move(bg);
    result.glass = std::move(gl);
    out = std::move(result);
    return true;
}

auto BackgroundField(const ImageCapture& capture) -> DepthFieldView {
    DepthFieldView view;
    view.width = capture.width;
    view.height = capture.height;
    view.depth = capture.depth.data();
    view.valid = capture.valid.data();
    view.projection = capture.projection;
    view.near_depth = capture.near_depth;
    view.far_depth = capture.far_depth;
    return view;
}

auto GlassFrontField(const GlassCapture& capture) -> DepthFieldView {
    DepthFieldView view;
    view.width = capture.width;
    view.height = capture.height;
    view.depth = capture.front_depth.data();
    view.valid = capture.front_valid.data();
    view.projection = capture.projection;
    view.near_depth = capture.near_depth;
    view.far_depth = capture.far_depth;
    return view;
}

auto GlassBackField(const GlassCapture& capture) -> DepthFieldView {
    DepthFieldView view;
    view.width = capture.width;
    view.height = capture.height;
    view.depth = capture.back_depth.data();
    view.valid = capture.back_valid.data();
    view.projection = capture.projection;
    view.near_depth = capture.near_depth;
    view.far_depth = capture.far_depth;
    return view;
}

auto TraceDepthField(const DepthFieldView& field, const Vec3& origin,
                     const Vec3& direction, RayHit& hit, std::string* error)
    -> bool {
    if (field.width <= 0 || field.height <= 0 || !field.depth || !field.valid)
        return SetError(error, "depth field is invalid");
    if (!Finite(field.projection))
        return SetError(error, "depth field projection is not finite");
    if (!(field.near_depth > 0.0) ||
        !(field.far_depth > field.near_depth) ||
        !Finite(field.near_depth) || !Finite(field.far_depth))
        return SetError(error, "depth field range is invalid");
    if (!Finite(origin) || !Finite(direction))
        return SetError(error, "ray is not finite");
    const double dir_len = Length(direction);
    if (!(dir_len > 1e-15) || !Finite(dir_len))
        return SetError(error, "ray direction is degenerate");

    RayHit result;
    const Vec3 dir = direction * (1.0 / dir_len);
    const int width = field.width;
    const int height = field.height;
    const Mat4& p = field.projection;

    /* Linear forms used for exact cell-boundary crossings. */
    const double cx0 = RowDot(p, 0, origin, 1.0);
    const double cx1 = RowDotDir(p, 0, dir);
    const double cy0 = RowDot(p, 1, origin, 1.0);
    const double cy1 = RowDotDir(p, 1, dir);
    const double w0 = RowDot(p, 3, origin, 1.0);
    const double w1 = RowDotDir(p, 3, dir);

    /* Screen x and y are monotone in t (their numerator is constant), so the
     * sign decides which cell boundary the ray crosses next. */
    const double du_num = cx1 * w0 - cx0 * w1;
    const double dv_num = cy1 * w0 - cy0 * w1;
    const int sign_x = (du_num > 0.0) - (du_num < 0.0);
    const int sign_y = (dv_num > 0.0) - (dv_num < 0.0);

    auto project = [&](double t, double& u, double& v, double& depth)
        -> bool {
        const Vec3 point = origin + dir * t;
        const Vec4 clip = Transform(p, Vec4{point.x, point.y, point.z, 1.0});
        if (!Finite(clip) || !(clip.w > 0.0)) return false;
        const double invw = 1.0 / clip.w;
        u = (clip.x * invw * 0.5 + 0.5) * width;
        v = (clip.y * invw * 0.5 + 0.5) * height;
        depth = -point.z;
        return Finite(u) && Finite(v) && Finite(depth);
    };

    /* Clip to t > 0, the captured finite view-depth range (view depth along
     * the ray is d(t) = d0 + a * t) and the image rectangle (ndc -1..1 on
     * both axes).  A ray may start outside the field and enter it, but it
     * stops once it leaves. */
    double t_low = 0.0;
    double t_high = kInf;
    const double d0 = -origin.z;
    const double a = -dir.z;
    if (std::abs(a) < 1e-15) {
        if (!(d0 >= field.near_depth - 1e-12 &&
              d0 <= field.far_depth + 1e-12)) {
            hit = result;
            return true;
        }
    } else {
        const double t1 = (field.near_depth - d0) / a;
        const double t2 = (field.far_depth - d0) / a;
        t_low = std::max(t_low, std::min(t1, t2));
        t_high = std::min(t_high, std::max(t1, t2));
    }

    /* Restrict to the region in front of the camera, w(t) = w0 + w1 * t > 0.
     * Screen coordinates are monotone in t there. */
    if (w0 <= 0.0) {
        if (!(w1 > 0.0)) {
            hit = result;
            return true;
        }
        t_low = std::max(t_low, -w0 / w1);
    }
    if (w1 < 0.0) t_high = std::min(t_high, -w0 / w1);
    if (!Finite(t_low) || t_low < 0.0) t_low = 0.0;
    if (!(t_high > t_low) || !Finite(t_high)) {
        hit = result;
        return true;
    }

    /* Clip to the image rectangle.  In front of the camera,
     * ndc = C / w lies in [-1, 1] exactly when |C| <= w.  These are linear
     * half planes in t, so no division by w is required and crossings behind
     * the camera cannot create spurious roots. */
    auto clip_half_plane = [&](double k0, double k1, double& low,
                               double& high) -> bool {
        /* k(t) = k0 + k1 * t >= 0 */
        if (std::abs(k1) < 1e-15) return k0 >= -1e-12;
        const double t = -k0 / k1;
        if (k1 > 0.0)
            low = std::max(low, t);
        else
            high = std::min(high, t);
        return true;
    };
    const double eps = 1e-12;
    if (!clip_half_plane(cx0 + w0, cx1 + w1, t_low, t_high) ||
        !clip_half_plane(-(cx0 - w0), -(cx1 - w1), t_low, t_high) ||
        !clip_half_plane(cy0 + w0, cy1 + w1, t_low, t_high) ||
        !clip_half_plane(-(cy0 - w0), -(cy1 - w1), t_low, t_high)) {
        hit = result;
        return true;
    }
    if (!(t_high > t_low - eps)) {
        hit = result;
        return true;
    }

    double u0 = 0.0, v0 = 0.0, depth0 = 0.0;
    if (!project(t_low, u0, v0, depth0)) {
        hit = result;
        return true;
    }
    /* The entry may lie exactly on a border; keep the start cell inside. */
    u0 = std::clamp(u0, 0.0,
                    std::nextafter(static_cast<double>(width), 0.0));
    v0 = std::clamp(v0, 0.0,
                    std::nextafter(static_cast<double>(height), 0.0));

    const std::size_t cells = static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> visited(cells, 0);
    std::priority_queue<TraceEntry> frontier;
    frontier.push({t_low, static_cast<int>(std::floor(u0)),
                   static_cast<int>(std::floor(v0))});

    const std::size_t max_cells = static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(height) + 1;
    std::size_t visited_count = 0;
    bool found = false;

    while (!frontier.empty()) {
        const TraceEntry entry = frontier.top();
        frontier.pop();
        if (entry.x < 0 || entry.x >= width || entry.y < 0 ||
            entry.y >= height)
            continue;
        const std::size_t idx =
            static_cast<std::size_t>(entry.y) * width + entry.x;
        if (visited[idx]) continue;
        /* Each cell is visited once and the walk never exceeds
         * width + height + 1 cells per segment. */
        if (visited_count >= max_cells) break;
        visited[idx] = 1;
        ++visited_count;

        const double t_in = entry.t;
        if (t_in > t_high + 1e-9) continue;

        double t_x = kInf, t_y = kInf;
        int next_x = entry.x, next_y = entry.y;
        if (sign_x > 0) {
            next_x = entry.x + 1;
            const double target =
                2.0 * static_cast<double>(entry.x + 1) / width - 1.0;
            if (auto t = BoundaryTime(cx0, cx1, w0, w1, target)) t_x = *t;
        } else if (sign_x < 0) {
            next_x = entry.x - 1;
            const double target =
                2.0 * static_cast<double>(entry.x) / width - 1.0;
            if (auto t = BoundaryTime(cx0, cx1, w0, w1, target)) t_x = *t;
        }
        if (sign_y > 0) {
            next_y = entry.y + 1;
            const double target =
                2.0 * static_cast<double>(entry.y + 1) / height - 1.0;
            if (auto t = BoundaryTime(cy0, cy1, w0, w1, target)) t_y = *t;
        } else if (sign_y < 0) {
            next_y = entry.y - 1;
            const double target =
                2.0 * static_cast<double>(entry.y) / height - 1.0;
            if (auto t = BoundaryTime(cy0, cy1, w0, w1, target)) t_y = *t;
        }

        /* Crossings behind the entry are numerical echoes of the boundary we
         * just came through. */
        const double t_slack = 1e-9 * (1.0 + std::abs(t_in));
        auto adjust = [&](double t) -> double {
            if (t < t_in) {
                if (t_in - t <= t_slack) return t_in;
                return kInf;
            }
            return t;
        };
        t_x = adjust(t_x);
        t_y = adjust(t_y);

        const double t_out = std::min({t_x, t_y, t_high});

        if (field.valid[idx]) {
            const double cell_depth = field.depth[idx];
            if (Finite(cell_depth) && cell_depth > 0.0 &&
                std::abs(a) > 1e-15) {
                /* Continuous intersection: match the ray's linearly varying
                 * view depth to the depth stored in this cell. */
                const double t_hit = (cell_depth - d0) / a;
                const double tolerance = 1e-9 * (1.0 + std::abs(t_hit));
                /* t <= 0 self-intersections are ignored. */
                if (t_hit > 0.0 && t_hit >= t_low - tolerance &&
                    t_hit >= t_in - tolerance &&
                    t_hit <= t_out + tolerance &&
                    t_hit <= t_high + tolerance) {
                    double u = 0.0, v = 0.0, depth = 0.0;
                    if (project(t_hit, u, v, depth)) {
                        const double cell_eps = 1e-7;
                        if (u >= entry.x - cell_eps &&
                            u <= entry.x + 1.0 + cell_eps &&
                            v >= entry.y - cell_eps &&
                            v <= entry.y + 1.0 + cell_eps) {
                            result.hit = true;
                            /* Report the cell whose recorded depth was
                             * matched (boundary hits can floor elsewhere). */
                            result.pixel_x = entry.x;
                            result.pixel_y = entry.y;
                            result.u = u;
                            result.v = v;
                            result.distance = t_hit;
                            result.visited_cells = visited_count;
                            found = true;
                            break;
                        }
                    }
                }
            }
        }

        if (t_out >= t_high - 1e-12) continue; /* clipped ray finished */

        const double neighbour_slack = 1e-12 * (1.0 + std::abs(t_out));
        const bool cross_x = Finite(t_x) && t_x <= t_out + neighbour_slack &&
                             t_x <= t_high + 1e-12 &&
                             next_x >= 0 && next_x < width;
        const bool cross_y = Finite(t_y) && t_y <= t_out + neighbour_slack &&
                             t_y <= t_high + 1e-12 &&
                             next_y >= 0 && next_y < height;
        /* When both boundaries are reached at once the ray passes exactly
         * through a grid corner; it continues into the diagonal cell, and the
         * two side cells are only touched with zero length.  This keeps the
         * visit count bounded by width + height + 1 for diagonal rays. */
        const bool corner = cross_x && cross_y &&
                            std::abs(t_x - t_y) <=
                                1e-12 * (1.0 + std::abs(t_x));
        if (corner) {
            if (next_x >= 0 && next_x < width && next_y >= 0 &&
                next_y < height)
                frontier.push({t_x, next_x, next_y});
        } else {
            if (cross_x) frontier.push({t_x, next_x, entry.y});
            if (cross_y) frontier.push({t_y, entry.x, next_y});
        }
    }

    if (!found) result.visited_cells = visited_count;
    hit = result;
    return true;
}

auto FresnelRefract(const Vec3& incident, const Vec3& normal, double eta)
    -> FresnelSample {
    FresnelSample sample;
    const double inc_len = Length(incident);
    const double nrm_len = Length(normal);
    if (!(inc_len > 0.0) || !(nrm_len > 0.0) || !Finite(eta) ||
        !(eta > 0.0)) {
        sample.reflectance = 1.0;
        sample.transmittance = 0.0;
        sample.total_internal_reflection = true;
        return sample;
    }
    const Vec3 i = incident * (1.0 / inc_len);
    Vec3 n = normal * (1.0 / nrm_len);
    double cos_i = -Dot(i, n);
    if (cos_i < 0.0) {
        n = n * -1.0;
        cos_i = -Dot(i, n);
    }
    if (cos_i > 1.0) cos_i = 1.0;

    const double sin2_t = eta * eta * (1.0 - cos_i * cos_i);
    if (sin2_t > 1.0) {
        sample.reflectance = 1.0;
        sample.transmittance = 0.0;
        sample.total_internal_reflection = true;
        return sample;
    }
    const double cos_t = std::sqrt(std::max(0.0, 1.0 - sin2_t));

    /* Exact unpolarised Fresnel: the average of s and p reflectance. */
    const double rs = (eta * cos_i - cos_t) / (eta * cos_i + cos_t);
    const double rp = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
    double reflectance = 0.5 * (rs * rs + rp * rp);
    reflectance = std::clamp(reflectance, 0.0, 1.0);

    sample.reflectance = reflectance;
    sample.transmittance = 1.0 - reflectance;
    sample.total_internal_reflection = false;
    sample.transmission_direction =
        Normalize(i * eta + n * (eta * cos_i - cos_t));
    return sample;
}

auto Compose(const SceneCapture& scene, const Material& material,
             CompositeResult& out, std::string* error) -> bool {
    if (!ValidateImageCapture(scene.background, error)) return false;
    if (!ValidateGlassCapture(scene.glass, error)) return false;
    if (scene.background.width != scene.glass.width ||
        scene.background.height != scene.glass.height)
        return SetError(error, "background and glass captures differ in size");
    if (!Finite(material.ior) || !(material.ior >= 1.01) ||
        !(material.ior <= 2.5))
        return SetError(error, "ior is outside [1.01, 2.5]");
    if (!NonNegative(material.sigma_a))
        return SetError(error, "sigma_a must be finite and non-negative");
    if (!NonNegative(material.environment))
        return SetError(error, "environment must be finite and non-negative");

    const int width = scene.background.width;
    const int height = scene.background.height;
    const std::size_t cells = static_cast<std::size_t>(width) * height;

    CompositeResult result;
    result.width = width;
    result.height = height;
    result.color.assign(3 * cells, 0.0);
    result.pixels.assign(cells, PixelTrace{});

    const ImageCapture& bg = scene.background;
    const GlassCapture& glass = scene.glass;
    const DepthFieldView bg_field = BackgroundField(bg);
    const DepthFieldView back_field = GlassBackField(glass);

    for (std::size_t idx = 0; idx < cells; ++idx) {
        /* Start from the opaque background, or the environment where the
         * background has no data. */
        Vec3 background_color = material.environment;
        if (bg.valid[idx]) {
            background_color = {bg.color[3 * idx], bg.color[3 * idx + 1],
                                bg.color[3 * idx + 2]};
        }
        result.color[3 * idx] = background_color.x;
        result.color[3 * idx + 1] = background_color.y;
        result.color[3 * idx + 2] = background_color.z;

        PixelTrace& trace = result.pixels[idx];
        if (!glass.front_valid[idx]) {
            trace.background_kept = bg.valid[idx] != 0;
            continue;
        }

        const Vec3 entrance = glass.front_position[idx];
        const double entrance_distance = Length(entrance);
        /* The entrance is visible unless the camera ray hits the opaque
         * background before it.  The hit need not be in the same pixel as
         * the entrance (the capture is a depth field, not a per-pixel
         * pairing). */
        RayHit camera_background_hit;
        bool background_in_front = false;
        if (TraceDepthField(bg_field, Vec3{}, entrance, camera_background_hit,
                            error) &&
            camera_background_hit.hit) {
            background_in_front =
                camera_background_hit.distance < entrance_distance - 1e-9;
        }
        if (background_in_front) {
            trace.background_kept = true;
            continue;
        }

        trace.entrance_visible = true;
        trace.entrance_distance = entrance_distance;

        const Vec3 front_normal = Normalize(glass.front_normal[idx]);
        const Vec3 camera_dir = Normalize(entrance);
        const FresnelSample entrance_fresnel =
            FresnelRefract(camera_dir, front_normal, 1.0 / material.ior);

        Vec3 colour = material.environment * entrance_fresnel.reflectance;

        if (!entrance_fresnel.total_internal_reflection &&
            entrance_fresnel.transmittance > 0.0) {
            RayHit exit_hit;
            if (TraceDepthField(back_field, entrance,
                                entrance_fresnel.transmission_direction,
                                exit_hit, error) &&
                exit_hit.hit) {
                trace.exit_found = true;
                trace.exit_hit = exit_hit;

                const std::size_t exit_idx =
                    static_cast<std::size_t>(exit_hit.pixel_y) * width +
                    exit_hit.pixel_x;
                const Vec3 exit_normal =
                    Normalize(glass.back_normal[exit_idx]);
                const Vec3 internal_dir =
                    entrance_fresnel.transmission_direction;
                const FresnelSample exit_fresnel = FresnelRefract(
                    internal_dir, exit_normal, material.ior);
                /* True internal path length from entrance to exit. */
                const double internal_length = exit_hit.distance;

                if (!exit_fresnel.total_internal_reflection) {
                    const Vec3 exit_point =
                        entrance + internal_dir * internal_length;
                    RayHit background_hit;
                    if (TraceDepthField(bg_field, exit_point,
                                        exit_fresnel.transmission_direction,
                                        background_hit, error)) {
                        Vec3 background = material.environment;
                        if (background_hit.hit) {
                            const std::size_t bidx =
                                static_cast<std::size_t>(
                                    background_hit.pixel_y) *
                                    width +
                                background_hit.pixel_x;
                            if (bg.valid[bidx]) {
                                trace.background_found = true;
                                background = {bg.color[3 * bidx],
                                              bg.color[3 * bidx + 1],
                                              bg.color[3 * bidx + 2]};
                            }
                        }
                        trace.background_hit = background_hit;
                        const double transmittance =
                            entrance_fresnel.transmittance *
                            exit_fresnel.transmittance;
                        const Vec3 absorb{
                            std::exp(-material.sigma_a.x * internal_length),
                            std::exp(-material.sigma_a.y * internal_length),
                            std::exp(-material.sigma_a.z * internal_length)};
                        /* One transmission path; no internal reflection is
                         * followed.  Absorption is per channel and uses the
                         * true internal length. */
                        colour = colour +
                                 Vec3{background.x * transmittance * absorb.x,
                                      background.y * transmittance * absorb.y,
                                      background.z * transmittance * absorb.z};
                    }
                }
            }
        }
        result.color[3 * idx] = colour.x;
        result.color[3 * idx + 1] = colour.y;
        result.color[3 * idx + 2] = colour.z;
    }

    out = std::move(result);
    return true;
}

auto WritePPM(const std::string& filename, int width, int height,
              const std::vector<double>& linear_rgb, std::string* error)
    -> bool {
    if (width <= 0 || height <= 0)
        return SetError(error, "PPM size must be positive");
    const std::size_t cells = static_cast<std::size_t>(width) * height;
    if (linear_rgb.size() != 3 * cells)
        return SetError(error, "PPM colour size mismatch");
    for (double v : linear_rgb) {
        if (!Finite(v)) return SetError(error, "PPM colour is not finite");
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file) return SetError(error, "cannot open PPM output");
    file << "P6\n" << width << ' ' << height << "\n255\n";
    /* PPM stores rows top-down; the framebuffer is y-up, so flip rows and
     * quantise only here. */
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * width + x;
            unsigned char rgb[3];
            for (int c = 0; c < 3; ++c) {
                const double clamped =
                    std::clamp(linear_rgb[3 * idx + c], 0.0, 1.0);
                rgb[c] = static_cast<unsigned char>(
                    std::lround(clamped * 255.0));
            }
            file.write(reinterpret_cast<const char*>(rgb), 3);
        }
    }
    if (!file) return SetError(error, "failed to write PPM output");
    return true;
}

auto WritePPM(const std::string& filename, const CompositeResult& result,
              std::string* error) -> bool {
    if (result.width <= 0 || result.height <= 0)
        return SetError(error, "composite size must be positive");
    const std::size_t cells =
        static_cast<std::size_t>(result.width) * result.height;
    if (result.color.size() != 3 * cells)
        return SetError(error, "composite colour size mismatch");
    return WritePPM(filename, result.width, result.height, result.color,
                    error);
}

}  // namespace refraction
}  // namespace gfx
