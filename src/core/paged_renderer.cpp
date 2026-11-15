#include <scimesh/paged_renderer.h>

#include <scimesh/clipping.h>
#include <scimesh/math_utils.h>
#include <scimesh/normals.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace scimesh {

namespace {

/// One bilinear tap: texel position, hosting page, slot in the bilinear
/// formula and (once delivered) the texel color.  Zero-weight taps are never
/// created, matching the effective tap set of Image::sample_bilinear().
struct TapSpec {
    int slot = 0;         // 0=c00, 1=c10, 2=c01, 3=c11
    int texel_x = 0;
    int texel_y = 0;
    int page_x = 0;
    int page_y = 0;
    uint64_t ticket = 0;  // 0 = resident without an in-flight request
    bool received = false;
    Color texel;
};

/// A screen-space triangle with every attribute the pixel loop needs,
/// precomputed with the exact arithmetic the dense Renderer pipeline uses.
struct PagedWorkItem {
    Vec3 screen_v0, screen_v1, screen_v2;
    Color color0, color1, color2;
    Vec3 normal0, normal1, normal2;
    Vec2 uv0, uv1, uv2;
    bool smooth = true;
    bool back_facing = false;

    // Rasterizer edge-function state (see Rasterizer::rasterize_triangle).
    Vec3 e0_a, e0_b, e1_a, e1_b, e2_a, e2_b;
    bool own0 = true, own1 = true, own2 = true;
    float k0 = 0.0f, k1 = 0.0f, k2 = 0.0f;
    int x_start = 0, x_end = 0, y_start = 0, y_end = 0;

    /// Published texture version to sample, or 0 for an untextured triangle.
    uint64_t texture_version = 0;
};

/// Canonical endpoint ordering, copied verbatim from the rasterizer so the
/// fill rule and edge functions are bit-identical.
void canonical_endpoints(Vec3 &a, Vec3 &b, bool &in_canonical_order) {
    if (b.x < a.x || (b.x == a.x && b.y < a.y)) {
        std::swap(a, b);
        in_canonical_order = !in_canonical_order;
    }
}

} // namespace

/// A fragment parked while one or more of its texture pages are in flight,
/// or (when `ready` is set) held only to preserve submission order behind an
/// earlier, still-parked fragment at the same pixel.  It stores only its own
/// (at most four) texels and the bilinear fractions — never a page copy,
/// never the full texture.
struct PagedRenderJob::PendingFragment {
    int x = 0;
    int y = 0;
    float depth = 0.0f;
    Color base_color;       // interpolated vertex color, pre-texture
    Vec3 normal;
    uint64_t version = 0;
    float sx = 0.0f;        // bilinear fractions, as in Image::sample_bilinear
    float sy = 0.0f;
    std::vector<TapSpec> taps;  // one entry per non-zero-weight slot
    bool ready = false;        // final color already computed, order-gated
    Color ready_color;         // final texture-modulated color when ready
    int64_t sequence = 0;
};

struct PagedRenderJob::WorkItem {
    PagedWorkItem tri;
};

PagedRenderJob::PagedRenderJob(PagedCache &cache,
                               const Scene &scene,
                               const Camera &camera,
                               const RenderOptions &options,
                               const std::vector<PagedTextureBinding> &bindings,
                               size_t max_pending_fragments)
    : cache_(cache),
      scene_(scene),
      camera_(camera),
      options_(options),
      max_pending_(max_pending_fragments),
      output_(options.width, options.height),
      rasterizer_(options.width, options.height),
      z_buffer_(rasterizer_.z_buffer),
      normal_buffer_(rasterizer_.normal_buffer) {
    if (max_pending_fragments < 1) {
        throw std::invalid_argument(
            "max_pending_fragments (K) must be >= 1");
    }
    validate_inputs(bindings);

    for (const auto &binding : bindings) {
        bindings_.emplace(binding.mesh_index, binding.version);
    }

    // Same Rasterizer configuration as Renderer::render_pipeline().
    rasterizer_.clear(1.0f);
    rasterizer_.specular_color = options_.specular_color;
    rasterizer_.shininess = options_.shininess;
    rasterizer_.lights = options_.lights;
    rasterizer_.ambient = options_.ambient;
    rasterizer_.contrast = options_.contrast;
    rasterizer_.fog_enabled = options_.fog_enabled;
    rasterizer_.fog_start = options_.fog_start;
    rasterizer_.fog_end = options_.fog_end;
    rasterizer_.fog_color = options_.fog_color;
    rasterizer_.fog_space = options_.fog_space;
    rasterizer_.z_near = options_.near_plane;
    rasterizer_.z_far = options_.far_plane;
    rasterizer_.orthographic =
        (options_.projection == ProjectionType::ORTHOGRAPHIC);
    rasterizer_.ssao_enabled = false;

    output_.clear_float(options_.background_color.r,
                        options_.background_color.g,
                        options_.background_color.b,
                        options_.background_color.a);

    Mat4 view = camera_.get_view_matrix();
    for (auto &light : rasterizer_.lights) {
        light.position =
            glm::normalize(transform_direction(view, light.position));
    }
    light_direction_ = Vec3(0.0f, 0.0f, 1.0f);

    build_work_items();
}

PagedRenderJob::~PagedRenderJob() {
    cache_.detach_waiter(this);
}

void PagedRenderJob::validate_inputs(
    const std::vector<PagedTextureBinding> &bindings) const {
    if (options_.width <= 0 || options_.height <= 0) {
        throw std::invalid_argument(
            "RenderOptions width and height must be > 0");
    }
    if (options_.aa_samples != 1) {
        throw std::invalid_argument(
            "paged rendering supports only a single sample (aa_samples == 1)");
    }
    if (options_.threads > 1) {
        throw std::invalid_argument(
            "paged rendering is single-threaded (threads must be 0 or 1)");
    }
    if (options_.ssao_enabled) {
        throw std::invalid_argument("SSAO is not supported by paged rendering");
    }
    if (options_.wireframe) {
        throw std::invalid_argument(
            "wireframe rendering is not supported by paged rendering");
    }
    if (!scene_.lines.empty() || !scene_.texts.empty()) {
        throw std::invalid_argument(
            "line and text layers are not supported by paged rendering");
    }

    for (size_t i = 0; i < scene_.meshes.size(); ++i) {
        const Mesh &mesh = scene_.meshes[i];
        if (mesh.empty()) continue;
        if (!mesh.is_valid()) {
            throw std::invalid_argument(
                "Mesh " + std::to_string(i) +
                " failed validation: check indices, vertex data, array sizes");
        }
        if (mesh.is_transparent()) {
            throw std::invalid_argument(
                "paged rendering supports only opaque meshes; mesh " +
                std::to_string(i) + " has translucent colors");
        }
        if (mesh.has_texture()) {
            throw std::invalid_argument(
                "mesh " + std::to_string(i) +
                " carries a dense Image texture; paged jobs only accept "
                "textures through PagedTextureBinding");
        }
    }

    std::map<size_t, uint64_t> seen;
    for (const auto &binding : bindings) {
        if (binding.mesh_index >= scene_.meshes.size()) {
            throw std::invalid_argument(
                "paged texture binding refers to unknown mesh index " +
                std::to_string(binding.mesh_index));
        }
        const Mesh &bound_mesh = scene_.meshes[binding.mesh_index];
        if (bound_mesh.has_texture()) {
            throw std::invalid_argument(
                "mesh " + std::to_string(binding.mesh_index) +
                " carries a dense Image texture; paged jobs require every "
                "textured mesh to name a paged binding");
        }
        if (seen.find(binding.mesh_index) != seen.end()) {
            throw std::invalid_argument(
                "duplicate paged texture binding for mesh " +
                std::to_string(binding.mesh_index));
        }
        seen.emplace(binding.mesh_index, binding.version);
        if (cache_.descriptor(binding.version) == nullptr) {
            throw std::invalid_argument(
                "paged texture binding refers to unknown or released version");
        }
        if (!bound_mesh.has_uvs()) {
            throw std::invalid_argument(
                "mesh " + std::to_string(binding.mesh_index) +
                " is bound to a paged texture but has no UV coordinates");
        }
    }
}

void PagedRenderJob::build_work_items() {
    Mat4 view = camera_.get_view_matrix();
    const float aspect =
        static_cast<float>(output_.width) / static_cast<float>(output_.height);
    Camera proj_cam = camera_;
    proj_cam.projection = options_.projection;
    Mat4 projection = proj_cam.get_projection_matrix(
        aspect, options_.near_plane, options_.far_plane);
    Mat4 view_projection = projection * view;

    std::vector<ClipPlane> view_clip_planes;
    view_clip_planes.reserve(options_.clip_planes.size());
    for (const auto &cp : options_.clip_planes) {
        view_clip_planes.push_back(
            clip_plane_to_view_space(cp, camera_.eye, view));
    }

    const bool smooth = (options_.shading == ShadingMode::SMOOTH);

    for (size_t node_i = 0; node_i < scene_.meshes.size(); ++node_i) {
        const Mesh &mesh = scene_.meshes[node_i];
        if (mesh.empty()) continue;

        const Mat4 model = scene_.transform(node_i);
        const Mat4 view_model = view * model;
        const Mat4 view_proj_model = view_projection * model;

        auto bound = bindings_.find(node_i);
        const uint64_t texture_version =
            (bound != bindings_.end()) ? bound->second : 0;

        std::vector<Vec3> computed_normals;
        const std::vector<Vec3> *normals_ptr;
        if (mesh.has_normals()) {
            normals_ptr = &mesh.normals;
        } else {
            compute_vertex_normals(mesh, computed_normals);
            normals_ptr = &computed_normals;
        }

        std::vector<Vec3> view_normals(normals_ptr->size());
        for (size_t i = 0; i < normals_ptr->size(); ++i) {
            Vec3 n = (*normals_ptr)[i];
            if (options_.invert_normals) n = -n;
            view_normals[i] = glm::normalize(
                transform_direction(view, transform_direction(model, n)));
        }

        for (int ti = 0; ti < static_cast<int>(mesh.triangles.size()); ++ti) {
            const auto &tri = mesh.triangles[ti];
            Vec3 v0 = mesh.vertices[tri.v0];
            Vec3 v1 = mesh.vertices[tri.v1];
            Vec3 v2 = mesh.vertices[tri.v2];

            Color c0, c1, c2;
            if (mesh.has_face_colors()) {
                c0 = c1 = c2 = mesh.face_colors[ti];
            } else if (mesh.has_colors()) {
                c0 = mesh.colors[tri.v0];
                c1 = mesh.colors[tri.v1];
                c2 = mesh.colors[tri.v2];
            } else {
                c0 = c1 = c2 = options_.default_color;
            }

            Vec3 n0 = view_normals[tri.v0];
            Vec3 n1 = view_normals[tri.v1];
            Vec3 n2 = view_normals[tri.v2];

            Vec2 uv0 = mesh.has_uvs() ? mesh.uvs[tri.v0] : Vec2(0, 0);
            Vec2 uv1 = mesh.has_uvs() ? mesh.uvs[tri.v1] : Vec2(0, 0);
            Vec2 uv2 = mesh.has_uvs() ? mesh.uvs[tri.v2] : Vec2(0, 0);

            ClipVertex cv0, cv1, cv2;
            cv0.position = transform_point_homogeneous(view_proj_model, v0);
            cv0.color = c0; cv0.normal = n0; cv0.uv = uv0;
            cv1.position = transform_point_homogeneous(view_proj_model, v1);
            cv1.color = c1; cv1.normal = n1; cv1.uv = uv1;
            cv2.position = transform_point_homogeneous(view_proj_model, v2);
            cv2.color = c2; cv2.normal = n2; cv2.uv = uv2;

            const bool has_user_clips = !view_clip_planes.empty();
            std::vector<ClipVertex> clipped_vertices;
            std::vector<Triangle> clipped_triangles;

            if (has_user_clips) {
                Vec3 vv0 = transform_point(view_model, v0);
                Vec3 vv1 = transform_point(view_model, v1);
                Vec3 vv2 = transform_point(view_model, v2);

                std::vector<ClipVertex> view_clipped;
                std::vector<Triangle> view_clip_tris;
                int vc_count = clip_triangle_view_plane(
                    vv0, vv1, vv2, n0, n1, n2, c0, c1, c2,
                    uv0, uv1, uv2,
                    view_clip_planes[0], view_clipped, view_clip_tris);

                for (int ci = 1;
                     ci < static_cast<int>(view_clip_planes.size()) && vc_count > 0;
                     ++ci) {
                    std::vector<ClipVertex> next_vertices;
                    std::vector<Triangle> next_triangles;
                    for (const auto &vt : view_clip_tris) {
                        const ClipVertex &a = view_clipped[vt.v0];
                        const ClipVertex &b = view_clipped[vt.v1];
                        const ClipVertex &c = view_clipped[vt.v2];
                        Vec3 pa(a.position.x, a.position.y, a.position.z);
                        Vec3 pb(b.position.x, b.position.y, b.position.z);
                        Vec3 pc(c.position.x, c.position.y, c.position.z);
                        Vec3 na(a.normal), nb(b.normal), nc(c.normal);
                        Color ca(a.color), cb(b.color), cc(c.color);
                        Vec2 ua(a.uv), ub(b.uv), uc(c.uv);
                        clip_triangle_view_plane(
                            pa, pb, pc, na, nb, nc, ca, cb, cc,
                            ua, ub, uc,
                            view_clip_planes[ci], next_vertices, next_triangles);
                    }
                    view_clipped.swap(next_vertices);
                    view_clip_tris.swap(next_triangles);
                    vc_count = static_cast<int>(view_clip_tris.size());
                    if (vc_count == 0) break;
                }

                if (vc_count == 0) continue;

                for (const auto &vt : view_clip_tris) {
                    const ClipVertex &a = view_clipped[vt.v0];
                    const ClipVertex &b = view_clipped[vt.v1];
                    const ClipVertex &c = view_clipped[vt.v2];

                    // The view-space clip carried UVs, but Renderer's
                    // re-projection builds fresh ClipVertices here and never
                    // copies them.  glm::vec2 has no zeroing default, so the
                    // dense renderer reads an *uninitialized* uv at this point
                    // (its output for user-clipped textured triangles varies
                    // across builds).  The paged job stays well-defined and
                    // deterministically uses (0, 0) instead of carrying that
                    // UB; for untextured meshes the uv is unused anyway.
                    ClipVertex cva, cvb, cvc;
                    cva.position = projection *
                        Vec4(a.position.x, a.position.y, a.position.z, 1.0f);
                    cva.color = a.color; cva.normal = a.normal; cva.uv = Vec2(0.0f);
                    cvb.position = projection *
                        Vec4(b.position.x, b.position.y, b.position.z, 1.0f);
                    cvb.color = b.color; cvb.normal = b.normal; cvb.uv = Vec2(0.0f);
                    cvc.position = projection *
                        Vec4(c.position.x, c.position.y, c.position.z, 1.0f);
                    cvc.color = c.color; cvc.normal = c.normal; cvc.uv = Vec2(0.0f);

                    clip_triangle_near_plane(cva, cvb, cvc,
                                             clipped_vertices,
                                             clipped_triangles);
                }
                if (clipped_triangles.empty()) continue;
            } else {
                int num_clipped = clip_triangle_near_plane(
                    cv0, cv1, cv2, clipped_vertices, clipped_triangles);
                if (num_clipped == 0) continue;
            }

            Vec3 flat_normal;
            if (!smooth) {
                flat_normal = compute_face_normal(
                    transform_point(view_model, v0),
                    transform_point(view_model, v1),
                    transform_point(view_model, v2));
            }

            for (const auto &ct : clipped_triangles) {
                const ClipVertex &cv_a = clipped_vertices[ct.v0];
                const ClipVertex &cv_b = clipped_vertices[ct.v1];
                const ClipVertex &cv_c = clipped_vertices[ct.v2];

                Vec3 ndc0 = perspective_divide(cv_a.position);
                Vec3 ndc1 = perspective_divide(cv_b.position);
                Vec3 ndc2 = perspective_divide(cv_c.position);

                float sx0, sy0, sz0, sx1, sy1, sz1, sx2, sy2, sz2;
                ndc_to_screen(ndc0, output_.width, output_.height, sx0, sy0, sz0);
                ndc_to_screen(ndc1, output_.width, output_.height, sx1, sy1, sz1);
                ndc_to_screen(ndc2, output_.width, output_.height, sx2, sy2, sz2);

                PagedWorkItem item;
                item.screen_v0 = Vec3(sx0, sy0, sz0);
                item.screen_v1 = Vec3(sx1, sy1, sz1);
                item.screen_v2 = Vec3(sx2, sy2, sz2);
                item.color0 = cv_a.color;
                item.color1 = cv_b.color;
                item.color2 = cv_c.color;
                item.uv0 = cv_a.uv;
                item.uv1 = cv_b.uv;
                item.uv2 = cv_c.uv;
                item.smooth = smooth;
                if (smooth) {
                    item.normal0 = cv_a.normal;
                    item.normal1 = cv_b.normal;
                    item.normal2 = cv_c.normal;
                } else {
                    item.normal0 = item.normal1 = item.normal2 = flat_normal;
                }
                item.texture_version = texture_version;

                float area =
                    (item.screen_v1.x - item.screen_v0.x) *
                        (item.screen_v2.y - item.screen_v0.y) -
                    (item.screen_v2.x - item.screen_v0.x) *
                        (item.screen_v1.y - item.screen_v0.y);
                if (options_.backface_culling && area > 0.0f) continue;
                if (std::abs(area) < 1e-12f) continue;
                item.back_facing = area > 0.0f;

                float min_x = std::min({item.screen_v0.x, item.screen_v1.x,
                                        item.screen_v2.x});
                float max_x = std::max({item.screen_v0.x, item.screen_v1.x,
                                        item.screen_v2.x});
                float min_y = std::min({item.screen_v0.y, item.screen_v1.y,
                                        item.screen_v2.y});
                float max_y = std::max({item.screen_v0.y, item.screen_v1.y,
                                        item.screen_v2.y});
                item.x_start = std::max(0, static_cast<int>(std::floor(min_x)));
                item.x_end = std::min(output_.width - 1,
                                      static_cast<int>(std::ceil(max_x)));
                item.y_start = std::max(0, static_cast<int>(std::floor(min_y)));
                item.y_end = std::min(output_.height - 1,
                                      static_cast<int>(std::ceil(max_y)));

                float inv_area = 1.0f / area;
                item.e0_a = item.screen_v1; item.e0_b = item.screen_v2;
                item.e1_a = item.screen_v2; item.e1_b = item.screen_v0;
                item.e2_a = item.screen_v0; item.e2_b = item.screen_v1;
                item.own0 = item.own1 = item.own2 = true;
                canonical_endpoints(item.e0_a, item.e0_b, item.own0);
                canonical_endpoints(item.e1_a, item.e1_b, item.own1);
                canonical_endpoints(item.e2_a, item.e2_b, item.own2);
                item.k0 = item.own0 ? inv_area : -inv_area;
                item.k1 = item.own1 ? inv_area : -inv_area;
                item.k2 = item.own2 ? inv_area : -inv_area;

                items_.push_back({std::move(item)});
            }
        }
    }

    if (!items_.empty()) {
        item_index_ = 0;
        cursor_x_ = items_.front().tri.x_start;
        cursor_y_ = items_.front().tri.y_start;
    }
}

PagedJobStatus PagedRenderJob::step(size_t budget) {
    if (status_ == PagedJobStatus::CANCELLED ||
        status_ == PagedJobStatus::FAILED ||
        status_ == PagedJobStatus::COMPLETE) {
        return status_;
    }
    if (budget == 0) {
        return status_;  // zero budget never advances the job
    }

    size_t remaining = budget;
    while (remaining > 0) {
        // Parked fragments are resumed first.  Parking order follows the
        // original traversal, and has_earlier_pending() additionally protects
        // equal-depth submission order when deliveries arrive out of order.
        if (resume_one_pending()) {
            --remaining;
            ++steps_;
            if (status_ != PagedJobStatus::READY) break;
            continue;
        }
        if (status_ == PagedJobStatus::FAILED) break;

        if (item_index_ < items_.size()) {
            const bool blocked = scan_one_pixel();
            --remaining;
            ++steps_;
            if (status_ == PagedJobStatus::FAILED) break;
            if (blocked) {
                status_ = PagedJobStatus::WAITING;
                break;
            }
            continue;
        }

        if (!pending_.empty()) {
            status_ = PagedJobStatus::WAITING;
            break;
        }
        status_ = PagedJobStatus::COMPLETE;
        break;
    }

    if (status_ == PagedJobStatus::READY) {
        status_ = finish_status();
    }
    return status_;
}

bool PagedRenderJob::has_earlier_pending(int x, int y,
                                         int64_t sequence) const {
    for (const PendingFragment &other : pending_) {
        if (other.x == x && other.y == y && other.sequence < sequence) {
            return true;
        }
    }
    return false;
}

bool PagedRenderJob::resume_one_pending() {
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        PendingFragment &frag = *it;

        bool complete = frag.ready;
        if (!complete) {
            complete = true;
            for (const TapSpec &tap : frag.taps) {
                if (!tap.received) {
                    complete = false;
                    break;
                }
            }
        }
        if (!complete) continue;

        // An earlier fragment for the same pixel is still parked: submitting
        // now could invert the dense order on an equal-depth tie.  Leave the
        // whole queue for a later step (the step budget is not consumed by a
        // mere re-check: callers only pay when a fragment actually resumes).
        if (has_earlier_pending(frag.x, frag.y, frag.sequence)) {
            return false;
        }

        Color shaded_base;
        if (frag.ready) {
            shaded_base = frag.ready_color;
        } else {
            // Replicate Image::sample_bilinear()'s exact arithmetic, including
            // the order of the multiplications, so the bytes match the dense
            // renderer.
            Color c[4];
            for (const TapSpec &tap : frag.taps) {
                c[tap.slot] = tap.texel;
                if (tap.texel.a < 1.0f - 1e-6f) {
                    detach_and_fail();
                    return false;
                }
            }
            const Color &c00 = c[0];
            const Color &c10 = c[1];
            const Color &c01 = c[2];
            const Color &c11 = c[3];
            const float sx = frag.sx;
            const float sy = frag.sy;
            Color tex(
                c00.r * (1 - sx) * (1 - sy) + c10.r * sx * (1 - sy) +
                    c01.r * (1 - sx) * sy + c11.r * sx * sy,
                c00.g * (1 - sx) * (1 - sy) + c10.g * sx * (1 - sy) +
                    c01.g * (1 - sx) * sy + c11.g * sx * sy,
                c00.b * (1 - sx) * (1 - sy) + c10.b * sx * (1 - sy) +
                    c01.b * (1 - sx) * sy + c11.b * sx * sy,
                c00.a * (1 - sx) * (1 - sy) + c10.a * sx * (1 - sy) +
                    c01.a * (1 - sx) * sy + c11.a * sx * sy);

            shaded_base = Color(
                frag.base_color.r * tex.r,
                frag.base_color.g * tex.g,
                frag.base_color.b * tex.b,
                frag.base_color.a * tex.a);
        }

        const int x = frag.x, y = frag.y;
        const float depth = frag.depth;
        const Vec3 normal = frag.normal;
        pending_.erase(it);
        submit_fragment(x, y, depth, shaded_base, normal);
        return true;
    }
    return false;
}

bool PagedRenderJob::scan_one_pixel() {
    const PagedWorkItem &item = items_[item_index_].tri;

    const int x = cursor_x_;
    const int y = cursor_y_;
    const size_t current_item = item_index_;

    // Advance the traversal cursor; rewound below if this pixel must park
    // against the K limit.
    if (cursor_x_ < item.x_end) {
        ++cursor_x_;
    } else if (cursor_y_ < item.y_end) {
        cursor_x_ = item.x_start;
        ++cursor_y_;
    } else {
        ++item_index_;
        if (item_index_ < items_.size()) {
            cursor_x_ = items_[item_index_].tri.x_start;
            cursor_y_ = items_[item_index_].tri.y_start;
        }
    }

    float px = static_cast<float>(x) + 0.5f;
    float py = static_cast<float>(y) + 0.5f;

    float w0 = item.k0 * ((item.e0_a.x - px) * (item.e0_b.y - py) -
                          (item.e0_b.x - px) * (item.e0_a.y - py));
    float w1 = item.k1 * ((item.e1_a.x - px) * (item.e1_b.y - py) -
                          (item.e1_b.x - px) * (item.e1_a.y - py));
    float w2 = item.k2 * ((item.e2_a.x - px) * (item.e2_b.y - py) -
                          (item.e2_b.x - px) * (item.e2_a.y - py));

    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) return false;
    if (w0 == 0.0f && !item.own0) return false;
    if (w1 == 0.0f && !item.own1) return false;
    if (w2 == 0.0f && !item.own2) return false;

    float depth = w0 * item.screen_v0.z + w1 * item.screen_v1.z +
                  w2 * item.screen_v2.z;

    Color base_color;
    Vec3 interp_normal;
    if (item.smooth) {
        base_color = Color(
            w0 * item.color0.r + w1 * item.color1.r + w2 * item.color2.r,
            w0 * item.color0.g + w1 * item.color1.g + w2 * item.color2.g,
            w0 * item.color0.b + w1 * item.color1.b + w2 * item.color2.b,
            w0 * item.color0.a + w1 * item.color1.a + w2 * item.color2.a);
        interp_normal = w0 * item.normal0 + w1 * item.normal1 +
                        w2 * item.normal2;
    } else {
        base_color = item.color0;
        interp_normal = item.normal0;
    }

    if (item.back_facing && interp_normal.z < 0.0f) {
        interp_normal = -interp_normal;
    }

    // Every covered fragment gets a traversal-order sequence number, used to
    // preserve the dense submission order on equal-depth ties when earlier
    // fragments for the same pixel are parked waiting on pages.
    const int64_t sequence = next_sequence_++;

    // Submit now only if no earlier fragment for this pixel is still queued.
    // Otherwise hold the fully computed fragment as a ready, order-gated
    // queue entry so the earlier one wins the tie as in the dense renderer.
    auto emit = [&](const Color &final_color) -> bool {
        if (!has_earlier_pending(x, y, sequence)) {
            submit_fragment(x, y, depth, final_color, interp_normal);
            return false;  // not blocked
        }
        if (pending_.size() >= max_pending_) {
            // Queue full: rewind so this pixel is retried after deliveries.
            item_index_ = current_item;
            cursor_x_ = x;
            cursor_y_ = y;
            --next_sequence_;  // this sequence is reused on the re-examination
            return true;       // blocked
        }
        PendingFragment held;
        held.x = x;
        held.y = y;
        held.depth = depth;
        held.normal = interp_normal;
        held.ready = true;
        held.ready_color = final_color;
        held.sequence = sequence;
        pending_.push_back(std::move(held));
        return false;
    };

    if (item.texture_version == 0) {
        return emit(base_color);
    }

    // Textured fragment: derive the exact bilinear taps that
    // Image::sample_bilinear() would use for this UV.
    const TextureDescriptor *desc =
        cache_.descriptor(item.texture_version);
    // The version is pinned by binding validation; it can only disappear if
    // the caller releases it while the job is alive, which is a caller error.
    if (desc == nullptr) {
        detach_and_fail();
        return false;
    }

    const float interp_u = item.smooth
        ? (w0 * item.uv0.x + w1 * item.uv1.x + w2 * item.uv2.x)
        : item.uv0.x;
    const float interp_v = item.smooth
        ? (w0 * item.uv0.y + w1 * item.uv1.y + w2 * item.uv2.y)
        : item.uv0.y;
    const float u = std::max(0.0f, std::min(1.0f, interp_u));
    const float v = std::max(0.0f, std::min(1.0f, interp_v));
    const float fx = u * (desc->width - 1);
    const float fy = v * (desc->height - 1);
    const int tx0 = static_cast<int>(fx);
    const int ty0 = static_cast<int>(fy);
    const int tx1 = std::min(tx0 + 1, desc->width - 1);
    const int ty1 = std::min(ty0 + 1, desc->height - 1);
    const float sx = fx - tx0;
    const float sy = fy - ty0;

    struct RawTap { int slot; int tx; int ty; float weight; };
    const std::array<RawTap, 4> raw = {{
        {0, tx0, ty0, (1.0f - sx) * (1.0f - sy)},
        {1, tx1, ty0, sx * (1.0f - sy)},
        {2, tx0, ty1, (1.0f - sx) * sy},
        {3, tx1, ty1, sx * sy},
    }};

    auto read_texel = [&](TapSpec &tap, int texel_x, int texel_y) {
        const ResidentPage *page = cache_.lookup_resident(
            item.texture_version, tap.page_x, tap.page_y);
        if (page == nullptr) return;
        const int local_x = texel_x - page->origin_x;
        const int local_y = texel_y - page->origin_y;
        const size_t idx =
            (static_cast<size_t>(local_y) * page->width + local_x) * 4u;
        tap.texel = Color(page->rgba[idx] / 255.0f,
                          page->rgba[idx + 1] / 255.0f,
                          page->rgba[idx + 2] / 255.0f,
                          page->rgba[idx + 3] / 255.0f);
        tap.received = true;
        if (page->rgba[idx + 3] != 255) {
            detach_and_fail();
        }
    };

    std::vector<TapSpec> taps;
    taps.reserve(4);
    bool all_resident = true;
    for (const RawTap &raw_tap : raw) {
        if (raw_tap.weight == 0.0f) continue;  // zero-weight taps need no page
        TapSpec tap;
        tap.slot = raw_tap.slot;
        tap.texel_x = raw_tap.tx;
        tap.texel_y = raw_tap.ty;
        tap.page_x = raw_tap.tx / desc->page_size;
        tap.page_y = raw_tap.ty / desc->page_size;
        read_texel(tap, raw_tap.tx, raw_tap.ty);
        if (!tap.received) {
            all_resident = false;
            tap.ticket = cache_.request_page(
                item.texture_version, tap.page_x, tap.page_y, this);
            if (tap.ticket == 0) {
                // request_page() found the page resident: read it now.
                read_texel(tap, raw_tap.tx, raw_tap.ty);
            }
        }
        taps.push_back(std::move(tap));
    }

    if (status_ == PagedJobStatus::FAILED) {
        return false;
    }

    if (all_resident) {
        Color c[4];
        for (const TapSpec &tap : taps) {
            c[tap.slot] = tap.texel;
        }
        const Color &c00 = c[0];
        const Color &c10 = c[1];
        const Color &c01 = c[2];
        const Color &c11 = c[3];
        Color tex(
            c00.r * (1 - sx) * (1 - sy) + c10.r * sx * (1 - sy) +
                c01.r * (1 - sx) * sy + c11.r * sx * sy,
            c00.g * (1 - sx) * (1 - sy) + c10.g * sx * (1 - sy) +
                c01.g * (1 - sx) * sy + c11.g * sx * sy,
            c00.b * (1 - sx) * (1 - sy) + c10.b * sx * (1 - sy) +
                c01.b * (1 - sx) * sy + c11.b * sx * sy,
            c00.a * (1 - sx) * (1 - sy) + c10.a * sx * (1 - sy) +
                c01.a * (1 - sx) * sy + c11.a * sx * sy);
        Color shaded_base(
            base_color.r * tex.r,
            base_color.g * tex.g,
            base_color.b * tex.b,
            base_color.a * tex.a);
        return emit(shaded_base);
    }

    // K limit: the fragment cannot be parked yet.  Rewind the cursor so this
    // exact pixel is examined again once deliveries free queue slots.
    if (pending_.size() >= max_pending_) {
        item_index_ = current_item;
        cursor_x_ = x;
        cursor_y_ = y;
        --next_sequence_;  // reuse this sequence number on re-examination
        return true;
    }

    PendingFragment frag;
    frag.x = x;
    frag.y = y;
    frag.depth = depth;
    frag.base_color = base_color;
    frag.normal = interp_normal;
    frag.version = item.texture_version;
    frag.sx = sx;
    frag.sy = sy;
    frag.taps = std::move(taps);
    frag.sequence = sequence;
    pending_.push_back(std::move(frag));
    return false;
}

void PagedRenderJob::paged_page_ready(uint64_t ticket, uint64_t version,
                                      const ResidentPage &page) {
    if (status_ == PagedJobStatus::CANCELLED ||
        status_ == PagedJobStatus::FAILED) {
        return;
    }
    for (PendingFragment &frag : pending_) {
        if (frag.version != version) continue;
        for (TapSpec &tap : frag.taps) {
            if (tap.received || tap.ticket != ticket) continue;
            if (tap.page_x != page.page_x || tap.page_y != page.page_y) {
                continue;
            }
            const int local_x = tap.texel_x - page.origin_x;
            const int local_y = tap.texel_y - page.origin_y;
            const size_t idx =
                (static_cast<size_t>(local_y) * page.width + local_x) * 4u;
            tap.texel = Color(page.rgba[idx] / 255.0f,
                              page.rgba[idx + 1] / 255.0f,
                              page.rgba[idx + 2] / 255.0f,
                              page.rgba[idx + 3] / 255.0f);
            tap.received = true;
            if (page.rgba[idx + 3] != 255) {
                detach_and_fail();
                return;
            }
        }
    }
}

void PagedRenderJob::submit_fragment(int x, int y, float depth,
                                     const Color &color, const Vec3 &normal) {
    if (x < 0 || x >= rasterizer_.width || y < 0 || y >= rasterizer_.height) {
        return;
    }
    const int idx = y * rasterizer_.width + x;
    if (!(depth < z_buffer_[idx])) {
        return;  // strict <, exactly like Rasterizer::shade_and_write
    }

    Color shaded;
    if (rasterizer_.lights.empty()) {
        shaded = shade_pixel(color, normal, light_direction_,
                             rasterizer_.specular_color,
                             rasterizer_.shininess);
    } else {
        shaded = shade_pixel_multi(color, normal, rasterizer_.lights,
                                   rasterizer_.ambient,
                                   rasterizer_.specular_color,
                                   rasterizer_.shininess);
    }

    if (rasterizer_.contrast != 1.0f) {
        auto apply_contrast = [&](float val) {
            return std::clamp((val - 0.5f) * rasterizer_.contrast + 0.5f,
                              0.0f, 1.0f);
        };
        shaded.r = apply_contrast(shaded.r);
        shaded.g = apply_contrast(shaded.g);
        shaded.b = apply_contrast(shaded.b);
    }

    uint8_t r = static_cast<uint8_t>(
        std::clamp(shaded.r, 0.0f, 1.0f) * 255.0f);
    uint8_t g = static_cast<uint8_t>(
        std::clamp(shaded.g, 0.0f, 1.0f) * 255.0f);
    uint8_t b = static_cast<uint8_t>(
        std::clamp(shaded.b, 0.0f, 1.0f) * 255.0f);
    uint8_t a = static_cast<uint8_t>(
        std::clamp(shaded.a, 0.0f, 1.0f) * 255.0f);

    if (rasterizer_.fog_enabled) {
        const float fog_depth =
            (rasterizer_.fog_space == FogSpace::WORLD)
                ? rasterizer_.fog_depth_from_ndc(depth)
                : depth;
        const float span = rasterizer_.fog_end - rasterizer_.fog_start;
        float fog_fac;
        if (std::abs(span) < 1e-12f) {
            fog_fac = (fog_depth >= rasterizer_.fog_start) ? 1.0f : 0.0f;
        } else {
            fog_fac = (fog_depth - rasterizer_.fog_start) / span;
        }
        fog_fac = std::max(0.0f, std::min(1.0f, fog_fac));
        r = static_cast<uint8_t>(
            r * (1.0f - fog_fac) + rasterizer_.fog_color.r * 255.0f * fog_fac);
        g = static_cast<uint8_t>(
            g * (1.0f - fog_fac) + rasterizer_.fog_color.g * 255.0f * fog_fac);
        b = static_cast<uint8_t>(
            b * (1.0f - fog_fac) + rasterizer_.fog_color.b * 255.0f * fog_fac);
        a = static_cast<uint8_t>(
            a * (1.0f - fog_fac) + rasterizer_.fog_color.a * 255.0f * fog_fac);
    }

    // Opaque path only: write depth and normals, never blend.
    z_buffer_[idx] = depth;
    normal_buffer_[idx] = normal;
    output_.set_pixel(x, y, r, g, b, a);
    ++submissions_;
}

void PagedRenderJob::cancel() {
    if (status_ == PagedJobStatus::CANCELLED ||
        status_ == PagedJobStatus::COMPLETE ||
        status_ == PagedJobStatus::FAILED) {
        return;
    }
    cache_.detach_waiter(this);
    pending_.clear();
    status_ = PagedJobStatus::CANCELLED;
}

void PagedRenderJob::detach_and_fail() {
    cache_.detach_waiter(this);
    pending_.clear();
    status_ = PagedJobStatus::FAILED;
}

PagedJobStatus PagedRenderJob::finish_status() const {
    if (item_index_ < items_.size()) return PagedJobStatus::READY;
    if (!pending_.empty()) return PagedJobStatus::WAITING;
    return PagedJobStatus::COMPLETE;
}

PagedJobStatus PagedRenderJob::status() const {
    return status_;
}

const Image &PagedRenderJob::image() const {
    if (status_ != PagedJobStatus::COMPLETE) {
        throw std::logic_error(
            "PagedRenderJob::image() is only valid after COMPLETE");
    }
    return output_;
}

Image PagedRenderJob::take_image() {
    if (status_ != PagedJobStatus::COMPLETE) {
        throw std::logic_error(
            "PagedRenderJob::take_image() is only valid after COMPLETE");
    }
    return std::move(output_);
}

std::vector<PageRequest> PagedRenderJob::outstanding_requests() const {
    std::map<uint64_t, PageRequest> by_ticket;
    for (const PendingFragment &frag : pending_) {
        for (const TapSpec &tap : frag.taps) {
            if (tap.received || tap.ticket == 0) continue;
            if (by_ticket.find(tap.ticket) != by_ticket.end()) continue;
            PageRequest req;
            req.version = frag.version;
            req.page_x = tap.page_x;
            req.page_y = tap.page_y;
            req.ticket = tap.ticket;
            by_ticket.emplace(tap.ticket, req);
        }
    }
    std::vector<PageRequest> out;
    out.reserve(by_ticket.size());
    for (auto &entry : by_ticket) {
        out.push_back(entry.second);
    }
    return out;
}

size_t PagedRenderJob::pending_fragment_count() const {
    return pending_.size();
}

uint64_t PagedRenderJob::submission_count() const {
    return submissions_;
}

uint64_t PagedRenderJob::total_steps() const {
    return steps_;
}

size_t PagedRenderJob::max_pending_fragments() const {
    return max_pending_;
}

} // namespace scimesh
