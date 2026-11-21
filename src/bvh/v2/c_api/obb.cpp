// C API glue for the oriented BVH. The actual implementation lives in `bvh/v2/obb.h`; this file
// only translates between the C interface and the C++ one.

#include <bvh/v2/c_api/bvh.h>
#include <bvh/v2/obb.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/vec.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

namespace bvh::v2::c_api {
namespace {

using Scalar = float;
using Vec3 = bvh::v2::Vec<Scalar, 3>;
using Tri3 = bvh::v2::Tri<Scalar, 3>;
using Node3 = bvh::v2::Node<Scalar, 3>;
using ObbBvh = bvh::v2::OrientedBvh<Scalar>;

struct ObbHandle {
    ObbBvh bvh;
};

inline Vec3 to_vec(const struct bvh_vec3f& v) {
    return Vec3(v.x, v.y, v.z);
}

inline struct bvh_vec3f to_c_vec(const Vec3& v) {
    struct bvh_vec3f out;
    out.x = v[0];
    out.y = v[1];
    out.z = v[2];
    return out;
}

inline std::vector<Tri3> to_triangles(
    const struct bvh_vec3f* vertices,
    size_t triangle_count)
{
    std::vector<Tri3> triangles;
    if (!vertices)
        return triangles;
    triangles.reserve(triangle_count);
    for (size_t i = 0; i < triangle_count; ++i) {
        triangles.emplace_back(
            to_vec(vertices[i * 3 + 0]),
            to_vec(vertices[i * 3 + 1]),
            to_vec(vertices[i * 3 + 2]));
    }
    return triangles;
}

inline const ObbBvh* as_bvh(const struct bvh_obb3f* handle) {
    return handle ? &reinterpret_cast<const ObbHandle*>(handle)->bvh : nullptr;
}

inline ObbBvh* as_bvh(struct bvh_obb3f* handle) {
    return handle ? &reinterpret_cast<ObbHandle*>(handle)->bvh : nullptr;
}

template <bool AnyHit>
void intersect_impl(
    const struct bvh_obb3f* obb,
    const struct bvh_ray3f* ray,
    const struct bvh_intersect_callbackf* callback,
    struct bvh_obb_traversal_stats* out_stats)
{
    auto* bvh = as_bvh(obb);
    if (!bvh || !ray || !callback || !callback->user_fn)
        return;

    bvh::v2::Ray<Scalar, 3> local_ray(
        to_vec(ray->org),
        to_vec(ray->dir),
        ray->tmin,
        ray->tmax);
    ObbBvh::TraversalStats stats;
    ObbBvh::DefaultStack stack;
    bvh->template intersect<AnyHit>(
        local_ray,
        stack,
        [&] (size_t begin, size_t end) {
            return callback->user_fn(callback->user_data, &local_ray.tmax, begin, end);
        },
        &stats);

    if (out_stats) {
        out_stats->node_tests = stats.node_tests;
        out_stats->leaf_tests = stats.leaf_tests;
        out_stats->primitive_tests = stats.primitive_tests;
    }
}

} // namespace
} // namespace bvh::v2::c_api

extern "C" {

BVH_EXPORT struct bvh_obb3f* bvh_obb3f_from_bvh(
    const struct bvh3f* source,
    const struct bvh_vec3f* triangle_vertices,
    size_t triangle_count)
{
    using namespace bvh::v2::c_api;
    if (!source)
        return nullptr;
    const auto& source_bvh = *reinterpret_cast<const bvh::v2::Bvh<Node3>*>(source);
    auto triangles = to_triangles(triangle_vertices, triangle_count);
    auto bvh = ObbBvh::from_bvh(source_bvh, triangles);
    return reinterpret_cast<struct bvh_obb3f*>(new ObbHandle { std::move(bvh) });
}

BVH_EXPORT void bvh_obb3f_destroy(struct bvh_obb3f* obb) {
    delete reinterpret_cast<bvh::v2::c_api::ObbHandle*>(obb);
}

BVH_EXPORT bool bvh_obb3f_get_node_obb(
    const struct bvh_obb3f* obb,
    size_t node_id,
    struct bvh_vec3f out_axes[3],
    struct bvh_vec3f* out_center,
    struct bvh_vec3f* out_half_size)
{
    using namespace bvh::v2::c_api;
    auto* bvh = as_bvh(obb);
    if (!bvh || node_id >= bvh->node_count())
        return false;
    const auto& node = bvh->nodes()[node_id];
    if (out_axes) {
        for (size_t i = 0; i < 3; ++i)
            out_axes[i] = to_c_vec(node.axis[i]);
    }
    if (out_center)
        *out_center = to_c_vec(node.center);
    if (out_half_size)
        *out_half_size = to_c_vec(node.half_size);
    return true;
}

BVH_EXPORT bool bvh_obb3f_node_is_leaf(const struct bvh_obb3f* obb, size_t node_id) {
    using namespace bvh::v2::c_api;
    auto* bvh = as_bvh(obb);
    return bvh && node_id < bvh->node_count() && bvh->nodes()[node_id].is_leaf();
}

BVH_EXPORT size_t bvh_obb3f_node_get_prim_count(const struct bvh_obb3f* obb, size_t node_id) {
    using namespace bvh::v2::c_api;
    auto* bvh = as_bvh(obb);
    if (!bvh || node_id >= bvh->node_count())
        return 0;
    return bvh->nodes()[node_id].index.prim_count();
}

BVH_EXPORT size_t bvh_obb3f_node_get_first_id(const struct bvh_obb3f* obb, size_t node_id) {
    using namespace bvh::v2::c_api;
    auto* bvh = as_bvh(obb);
    if (!bvh || node_id >= bvh->node_count())
        return 0;
    return bvh->nodes()[node_id].index.first_id();
}

BVH_EXPORT size_t bvh_obb3f_get_node_count(const struct bvh_obb3f* obb) {
    auto* bvh = bvh::v2::c_api::as_bvh(obb);
    return bvh ? bvh->node_count() : 0;
}

BVH_EXPORT size_t bvh_obb3f_get_prim_count(const struct bvh_obb3f* obb) {
    auto* bvh = bvh::v2::c_api::as_bvh(obb);
    return bvh ? bvh->prim_count() : 0;
}

BVH_EXPORT size_t bvh_obb3f_get_prim_id(const struct bvh_obb3f* obb, size_t i) {
    auto* bvh = bvh::v2::c_api::as_bvh(obb);
    if (!bvh || i >= bvh->prim_count())
        return SIZE_MAX;
    return bvh->prim_ids()[i];
}

BVH_EXPORT void bvh_obb3f_refit(
    struct bvh_obb3f* obb,
    const struct bvh_vec3f* triangle_vertices,
    size_t triangle_count)
{
    using namespace bvh::v2::c_api;
    auto* bvh = as_bvh(obb);
    if (!bvh)
        return;
    auto triangles = to_triangles(triangle_vertices, triangle_count);
    bvh->refit(triangles);
}

BVH_EXPORT void bvh_obb3f_save(const struct bvh_obb3f* obb, FILE* file) {
    using namespace bvh::v2::c_api;
    auto* bvh = as_bvh(obb);
    if (!bvh || !file)
        return;
    struct FileOutputStream : bvh::v2::OutputStream {
        FILE* file;
        explicit FileOutputStream(FILE* f) : file(f) {}
        bool write_raw(const void* data, size_t size) override {
            return fwrite(data, 1, size, file) == size;
        }
    };
    FileOutputStream stream { file };
    bvh->serialize(stream);
}

BVH_EXPORT struct bvh_obb3f* bvh_obb3f_load(FILE* file) {
    using namespace bvh::v2::c_api;
    if (!file)
        return nullptr;
    struct FileInputStream : bvh::v2::InputStream {
        FILE* file;
        explicit FileInputStream(FILE* f) : file(f) {}
        size_t read_raw(void* data, size_t size) override {
            return fread(data, 1, size, file);
        }
    };
    FileInputStream stream { file };
    auto bvh = ObbBvh::deserialize(stream);
    if (bvh.empty())
        return nullptr;
    return reinterpret_cast<struct bvh_obb3f*>(new ObbHandle { std::move(bvh) });
}

BVH_EXPORT void bvh_obb3f_intersect_ray(
    const struct bvh_obb3f* obb,
    const struct bvh_ray3f* ray,
    const struct bvh_intersect_callbackf* callback)
{
    bvh::v2::c_api::intersect_impl<false>(obb, ray, callback, nullptr);
}

BVH_EXPORT void bvh_obb3f_intersect_ray_any(
    const struct bvh_obb3f* obb,
    const struct bvh_ray3f* ray,
    const struct bvh_intersect_callbackf* callback)
{
    bvh::v2::c_api::intersect_impl<true>(obb, ray, callback, nullptr);
}

BVH_EXPORT void bvh_obb3f_intersect_ray_stats(
    const struct bvh_obb3f* obb,
    const struct bvh_ray3f* ray,
    const struct bvh_intersect_callbackf* callback,
    struct bvh_obb_traversal_stats* stats)
{
    bvh::v2::c_api::intersect_impl<false>(obb, ray, callback, stats);
}

BVH_EXPORT void bvh_obb3f_intersect_ray_any_stats(
    const struct bvh_obb3f* obb,
    const struct bvh_ray3f* ray,
    const struct bvh_intersect_callbackf* callback,
    struct bvh_obb_traversal_stats* stats)
{
    bvh::v2::c_api::intersect_impl<true>(obb, ray, callback, stats);
}

} // extern "C"
