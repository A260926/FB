#include "projective_baker.h"
#include "our_gl.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace projective_baker {

namespace {

constexpr double kDepthSentinel = -1000.; // matches init_zbuffer()

bool finite(const vec2 &v) { return std::isfinite(v.x) && std::isfinite(v.y); }
bool finite(const vec3 &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(const vec4 &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}
bool finite(const mat<4,4> &m) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!std::isfinite(m[i][j])) return false;
    return true;
}

double cross2(const vec2 &a, const vec2 &b) { // 2D cross product
    return a.x * b.y - a.y * b.x;
}

// Saves and restores the global rasterizer state of our_gl.cpp so capturing
// and baking never leak viewport/depth/matrix state into the caller.
class ScopedRasterState {
public:
    ScopedRasterState()
        : model_view_(ModelView), viewport_(Viewport), perspective_(Perspective), zbuffer_(zbuffer) {}
    ~ScopedRasterState() {
        ModelView = model_view_;
        Viewport = viewport_;
        Perspective = perspective_;
        zbuffer = std::move(zbuffer_);
    }
    ScopedRasterState(const ScopedRasterState &) = delete;
    ScopedRasterState &operator=(const ScopedRasterState &) = delete;
private:
    mat<4,4> model_view_, viewport_, perspective_;
    std::vector<double> zbuffer_;
};

// One source view projected against the captured geometry.
struct ProjectedFace {
    vec4 clip[3] = {};
    vec4 ndc[3] = {};
    vec2 screen[3] = {};
    mat<3,3> screen_inverse_t = {}; // inverse-transpose of the screen-coordinate barycentric matrix
    bool culled = false; // true when the original area rule rejects the face in this view
};

struct SourceView {
    const SourceCapture *capture = nullptr;
    std::vector<ProjectedFace> faces; // one entry per geometry face, model-major
};

} // namespace

ProjectiveGeometry::ProjectiveGeometry(const std::vector<const Model *> &models) {
    meshes_.reserve(models.size());
    for (const Model *model : models) {
        if (model == nullptr) throw bake_error("ProjectiveGeometry: null model pointer");
        Mesh mesh;
        mesh.faces.reserve(model->nfaces());
        for (int f = 0; f < model->nfaces(); ++f) {
            Face face;
            for (int k = 0; k < 3; ++k) {
                vec4 v = model->vert(f, k);
                vec2 t = model->uv(f, k);
                if (!finite(v))
                    throw bake_error("ProjectiveGeometry: non-finite vertex position");
                if (!finite(t))
                    throw bake_error("ProjectiveGeometry: non-finite uv coordinate");
                if (t.x < 0. || t.x > 1. || t.y < 0. || t.y > 1.)
                    throw bake_error("ProjectiveGeometry: uv coordinate outside [0,1]");
                face.world[k] = v.xyz();
                face.uv[k] = t;
            }
            vec3 e1 = face.world[1] - face.world[0];
            vec3 e2 = face.world[2] - face.world[0];
            vec3 gn = cross(e1, e2);
            if (!(norm(gn) > 0.)) // zero length or non-finite: world-degenerate face
                throw bake_error("ProjectiveGeometry: degenerate world triangle");
            face.normal = normalized(gn);
            mesh.faces.push_back(face);
        }
        meshes_.push_back(std::move(mesh));
    }
}

SourceCapture::SourceCapture(const ProjectiveGeometry &geometry,
                             const mat<4, 4> &world_to_clip, const vec3 &eye,
                             const std::vector<double> &rgb, int width, int height)
    : geometry_(geometry), world_to_clip_(world_to_clip), eye_(eye),
      width_(width), height_(height), rgb_(rgb) {
    if (width <= 0 || height <= 0)
        throw bake_error("SourceCapture: image dimensions must be positive");
    if (rgb_.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3u)
        throw bake_error("SourceCapture: rgb length does not match width*height*3");
    if (!std::all_of(rgb_.begin(), rgb_.end(), [](double v) { return std::isfinite(v); }))
        throw bake_error("SourceCapture: non-finite rgb value");
    if (!finite(world_to_clip_) || !finite(eye_))
        throw bake_error("SourceCapture: non-finite projection matrix or eye position");

    ScopedRasterState state_guard;
    init_viewport(0, 0, width_, height_); // integer pixel centers map 1:1 to the source image
    depth_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), kDepthSentinel);

    for (const ProjectiveGeometry::Mesh &mesh : geometry_.meshes()) {
        for (const ProjectiveGeometry::Face &face : mesh.faces) {
            Triangle clip = {};
            for (int k = 0; k < 3; ++k) {
                vec3 p = face.world[k];
                clip[k] = world_to_clip_ * vec4{p.x, p.y, p.z, 1.};
                if (!finite(clip[k]) || clip[k].w <= 0.)
                    throw bake_error("SourceCapture: geometry leaves the positive-w frustum");
            }
            // Same coverage/perspective/depth kernel as the real rasterize() path.
            rasterize_depth(clip, depth_, width_, height_);
        }
    }

    depth_covered_.resize(depth_.size());
    for (size_t i = 0; i < depth_.size(); ++i)
        depth_covered_[i] = depth_[i] != kDepthSentinel;
}

bool SourceCapture::has_depth(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return false;
    return depth_covered_[static_cast<size_t>(y) * width_ + x] != 0;
}

double SourceCapture::depth(int x, int y) const {
    return depth_[static_cast<size_t>(y) * width_ + x];
}

BakeResult bake_atlas(const ProjectiveGeometry &geometry,
                      const std::vector<const SourceCapture *> &sources,
                      int atlas_width, int atlas_height,
                      double depth_eps,
                      std::vector<OverlapConflict> *conflicts) {
    if (atlas_width <= 0 || atlas_height <= 0)
        throw bake_error("bake_atlas: atlas dimensions must be positive");
    if (!std::isfinite(depth_eps) || depth_eps < 0.)
        throw bake_error("bake_atlas: depth epsilon must be a non-negative finite number");
    for (const SourceCapture *source : sources)
        if (source == nullptr)
            throw bake_error("bake_atlas: null source capture");

    // All validation happens before any result storage is touched. The guard
    // guarantees the global raster state is restored even when an exception
    // is thrown; the UV bake itself does not touch the global matrices.
    ScopedRasterState state_guard;

    const size_t texel_count = static_cast<size_t>(atlas_width) * static_cast<size_t>(atlas_height);
    BakeResult result;
    result.width = atlas_width;
    result.height = atlas_height;
    result.covered.assign(texel_count, 0);
    result.owner_model.assign(texel_count, -1);
    result.owner_face.assign(texel_count, -1);
    result.rgb.assign(texel_count * 3u, 0.);
    result.weight.assign(texel_count, 0.);
    std::vector<std::uint8_t> conflicted(texel_count, 0);

    // Project every face once per source; the per-face data is reused per texel.
    std::vector<SourceView> views;
    views.reserve(sources.size());
    for (const SourceCapture *capture : sources) {
        SourceView view;
        view.capture = capture;
        for (const ProjectiveGeometry::Mesh &mesh : geometry.meshes()) {
            for (const ProjectiveGeometry::Face &face : mesh.faces) {
                ProjectedFace pf;
                for (int k = 0; k < 3; ++k) {
                    vec3 p = face.world[k];
                    pf.clip[k] = capture->world_to_clip() * vec4{p.x, p.y, p.z, 1.};
                    pf.ndc[k] = pf.clip[k] / pf.clip[k].w;
                    pf.screen[k] = { (pf.ndc[k].x + 1.) * capture->width()  / 2.,
                                     (pf.ndc[k].y + 1.) * capture->height() / 2. };
                }
                mat<3,3> abc = {{
                    {pf.screen[0].x, pf.screen[0].y, 1.},
                    {pf.screen[1].x, pf.screen[1].y, 1.},
                    {pf.screen[2].x, pf.screen[2].y, 1.} }};
                pf.culled = abc.det() < 1.; // same forward-area rule as rasterize()
                if (!pf.culled) pf.screen_inverse_t = abc.invert_transpose();
                view.faces.push_back(pf);
            }
        }
        views.push_back(std::move(view));
    }

    int global_face = 0;
    for (int model_index = 0; model_index < static_cast<int>(geometry.meshes().size()); ++model_index) {
        const ProjectiveGeometry::Mesh &mesh = geometry.meshes()[model_index];
        for (int face_index = 0; face_index < static_cast<int>(mesh.faces.size()); ++face_index, ++global_face) {
            const ProjectiveGeometry::Face &face = mesh.faces[face_index];

            // UV rasterization setup: counter-clockwise winding in the y-down
            // atlas row coordinate space. Mirrored UVs are kept.
            int order[3] = {0, 1, 2};
            vec2 uv[3] = {face.uv[0], face.uv[1], face.uv[2]};
            double uv_area = cross2(uv[1] - uv[0], uv[2] - uv[0]);
            if (uv_area == 0.) continue; // zero UV area: the face is ignored
            if (uv_area < 0.) {
                std::swap(order[1], order[2]);
                std::swap(uv[1], uv[2]);
                uv_area = -uv_area;
            }
            vec3 world[3] = {face.world[order[0]], face.world[order[1]], face.world[order[2]]};

            double minu = std::min({uv[0].x, uv[1].x, uv[2].x});
            double maxu = std::max({uv[0].x, uv[1].x, uv[2].x});
            double minv = std::min({uv[0].y, uv[1].y, uv[2].y});
            double maxv = std::max({uv[0].y, uv[1].y, uv[2].y});
            int x_min = std::max(0, static_cast<int>(std::floor(minu * atlas_width  - .5)));
            int x_max = std::min(atlas_width  - 1, static_cast<int>(std::floor(maxu * atlas_width  - .5)));
            int y_min = std::max(0, static_cast<int>(std::floor(minv * atlas_height - .5)));
            int y_max = std::min(atlas_height - 1, static_cast<int>(std::floor(maxv * atlas_height - .5)));

            for (int ty = y_min; ty <= y_max; ++ty) {
                for (int tx = x_min; tx <= x_max; ++tx) {
                    vec2 center{(tx + .5) / atlas_width, (ty + .5) / atlas_height};

                    // Top-left half-open rule on the CCW UV triangle.
                    double edge[3];
                    bool inside = true;
                    for (int i = 0; i < 3; ++i) {
                        vec2 a = uv[i], b = uv[(i + 1) % 3];
                        vec2 d = b - a;
                        edge[i] = cross2(d, center - a);
                        if (edge[i] < 0.) { inside = false; break; }
                        if (edge[i] == 0.) {
                            bool top_left = (d.y == 0.) ? (d.x < 0.) : (d.y > 0.);
                            if (!top_left) { inside = false; break; }
                        }
                    }
                    if (!inside) continue;

                    // UV-space barycentrics only; camera perspective weights
                    // must never be used to reconstruct the world point.
                    double lambda[3] = {edge[1] / uv_area, edge[2] / uv_area, edge[0] / uv_area};
                    vec3 P = world[0] * lambda[0] + world[1] * lambda[1] + world[2] * lambda[2];
                    const vec3 &N = face.normal;

                    size_t texel = static_cast<size_t>(ty) * atlas_width + tx;
                    if (!result.covered[texel]) {
                        result.covered[texel] = 1;
                        result.owner_model[texel] = model_index;
                        result.owner_face[texel] = face_index;
                    } else if (result.owner_model[texel] != model_index ||
                               result.owner_face[texel] != face_index) {
                        // True coverage overlap: report both faces and never let
                        // submission order pick a color for this texel.
                        if (!conflicted[texel]) {
                            conflicted[texel] = 1;
                            OverlapConflict conflict;
                            conflict.x = tx;
                            conflict.y = ty;
                            conflict.model_a = result.owner_model[texel];
                            conflict.face_a = result.owner_face[texel];
                            conflict.model_b = model_index;
                            conflict.face_b = face_index;
                            if (conflicts != nullptr) conflicts->push_back(conflict);
                            // Any color the earlier owner may have baked here is
                            // invalid: leave zero color and zero accumulated weight.
                            result.rgb[texel * 3u] = 0.;
                            result.rgb[texel * 3u + 1] = 0.;
                            result.rgb[texel * 3u + 2] = 0.;
                            result.weight[texel] = 0.;
                        }
                        continue;
                    }

                    double accumulated[3] = {0., 0., 0.};
                    double total_weight = 0.;

                    for (const SourceView &view : views) {
                        const SourceCapture &capture = *view.capture;
                        const ProjectedFace &pf = view.faces[global_face];
                        if (pf.culled) continue; // receiver rejected by the source area rule

                        vec4 point_clip = capture.world_to_clip() * vec4{P.x, P.y, P.z, 1.};
                        vec4 point_ndc = point_clip / point_clip.w;
                        double s = (point_ndc.x + 1.) * capture.width()  / 2.;
                        double t = (point_ndc.y + 1.) * capture.height() / 2.;

                        int ix = static_cast<int>(std::floor(s));
                        int iy = static_cast<int>(std::floor(t));
                        double fx = s - ix, fy = t - iy;
                        double bilinear[4] = {
                            (1. - fx) * (1. - fy), fx * (1. - fy),
                            (1. - fx) * fy,        fx * fy };
                        int tap_x[4] = {ix, ix + 1, ix, ix + 1};
                        int tap_y[4] = {iy, iy, iy + 1, iy + 1};

                        // Receiver z/w plane, extrapolated from the three
                        // triangle vertices to each integer tap coordinate.
                        vec3 vert_depth{pf.ndc[0].z, pf.ndc[1].z, pf.ndc[2].z};

                        vec3 to_eye = capture.eye() - P;
                        double eye_dist = norm(to_eye);
                        double facing = 0.;
                        if (eye_dist > 0.)
                            facing = std::pow(std::max(0., N * (to_eye / eye_dist)), 4.);
                        if (!(facing > 0.)) continue; // back-facing wrt this source contributes nothing

                        for (int tap = 0; tap < 4; ++tap) {
                            double tap_weight = bilinear[tap];
                            if (!(tap_weight > 0.)) continue; // zero-weight taps are not accessed
                            int px = tap_x[tap], py = tap_y[tap];
                            if (!capture.has_depth(px, py)) continue; // out of image or no coverage

                            vec3 tap_bary = pf.screen_inverse_t * vec3{static_cast<double>(px),
                                                                       static_cast<double>(py), 1.};
                            double receiver_depth = tap_bary * vert_depth;
                            if (receiver_depth < capture.depth(px, py) - depth_eps)
                                continue; // occluded at this tap; judged per tap, never per point

                            double w = tap_weight * facing;
                            size_t pixel = (static_cast<size_t>(py) * capture.width() + px) * 3u;
                            for (int c = 0; c < 3; ++c)
                                accumulated[c] += w * capture.image()[pixel + c];
                            total_weight += w;
                        }
                    }

                    if (conflicted[texel] || !(total_weight > 0.)) continue; // leave zero color/weight
                    for (int c = 0; c < 3; ++c)
                        result.rgb[texel * 3u + c] = accumulated[c] / total_weight;
                    result.weight[texel] = total_weight;
                }
            }
        }
    }
    return result;
}

} // namespace projective_baker
