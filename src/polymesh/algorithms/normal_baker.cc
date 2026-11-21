#include "normal_baker.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace polymesh
{
namespace
{
// ---------------------------------------------------------------------------
// double-precision 3-vector helpers (std::array based)
// ---------------------------------------------------------------------------

using vec3 = std::array<double, 3>;

inline vec3 make_vec(double x, double y, double z) { return vec3{{x, y, z}}; }
inline vec3 operator+(vec3 const& a, vec3 const& b) { return vec3{{a[0] + b[0], a[1] + b[1], a[2] + b[2]}}; }
inline vec3 operator-(vec3 const& a, vec3 const& b) { return vec3{{a[0] - b[0], a[1] - b[1], a[2] - b[2]}}; }
inline vec3 operator*(vec3 const& a, double s) { return vec3{{a[0] * s, a[1] * s, a[2] * s}}; }
inline double dot(vec3 const& a, vec3 const& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline vec3 cross(vec3 const& a, vec3 const& b)
{
    return vec3{{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}};
}
inline double length_sq(vec3 const& a) { return dot(a, a); }
inline double length(vec3 const& a) { return std::sqrt(length_sq(a)); }

inline bool is_finite(double v) { return std::isfinite(v); }
inline bool is_finite(vec3 const& v) { return is_finite(v[0]) && is_finite(v[1]) && is_finite(v[2]); }

/// normalizes v into out; returns false if v is zero or non-finite
inline bool normalize(vec3 const& v, vec3& out)
{
    if (!is_finite(v))
        return false;
    auto const l = length(v);
    if (!(l > 0.0) || !is_finite(l))
        return false;
    out = v * (1.0 / l);
    return true;
}

[[noreturn]] void fail(std::string const& msg) { throw std::invalid_argument("polymesh::normal_bake_session: " + msg); }

// ---------------------------------------------------------------------------
// 2d / coverage helpers
// ---------------------------------------------------------------------------

/// signed 2d cross product of edge (b-a) with point p
inline double edge_cross_2d(vec3 const& a, vec3 const& b, vec3 const& p)
{
    return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
}

/// Directed edge function with canonical endpoint ordering.
///
/// Two triangles sharing an edge traverse it in opposite directions. Computing
/// the cross product with a canonical endpoint order (and then applying the
/// direction sign) makes both triangles evaluate exactly opposite values, so
/// samples lying exactly on a shared edge produce an exact 0 on both sides and
/// the top-left rule can break the tie deterministically. Computing
/// cross(a, b, p) and cross(b, a, p) independently does not guarantee this.
inline double canonical_edge_cross_2d(vec3 const& a, vec3 const& b, vec3 const& p)
{
    auto const reverse = (b[0] < a[0]) || (b[0] == a[0] && b[1] < a[1]);
    auto const& a2 = reverse ? b : a;
    auto const& b2 = reverse ? a : b;
    auto const c = edge_cross_2d(a2, b2, p);
    return reverse ? -c : c;
}

/// Top-left fill rule for an edge a->b of a CCW triangle.
/// Horizontal edges going left and non-horizontal edges going down are inclusive.
inline bool edge_accepts(vec3 const& a, vec3 const& b, vec3 const& p)
{
    auto const e = canonical_edge_cross_2d(a, b, p);
    auto const dx = b[0] - a[0];
    auto const dy = b[1] - a[1];
    auto const is_top_left = (dy == 0.0) ? (dx < 0.0) : (dy < 0.0);
    if (is_top_left)
        return e >= 0.0;
    return e > 0.0;
}

/// 2d barycentric weights of p in triangle (a, b, c) (z ignored)
inline bool barycentric_2d(vec3 const& a, vec3 const& b, vec3 const& c, vec3 const& p, double& wa, double& wb, double& wc)
{
    auto const denom = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    if (!(denom != 0.0) || !is_finite(denom))
        return false;
    auto const inv = 1.0 / denom;
    wb = ((p[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (p[1] - a[1])) * inv;
    wc = ((b[0] - a[0]) * (p[1] - a[1]) - (p[0] - a[0]) * (b[1] - a[1])) * inv;
    wa = 1.0 - wb - wc;
    return is_finite(wa) && is_finite(wb) && is_finite(wc);
}

/// Moller-Trumbore, double-sided. Returns the smallest t >= 0.
inline bool ray_triangle(vec3 const& orig, vec3 const& dir, vec3 const& a, vec3 const& b, vec3 const& c, double& out_t)
{
    auto const e1 = b - a;
    auto const e2 = c - a;
    auto const p = cross(dir, e2);
    auto const det = dot(e1, p);
    if (det == 0.0 || !is_finite(det))
        return false;
    auto const inv_det = 1.0 / det;
    auto const tv = orig - a;
    auto const u = dot(tv, p) * inv_det;
    if (!is_finite(u) || u < 0.0 || u > 1.0)
        return false;
    auto const q = cross(tv, e1);
    auto const v = dot(dir, q) * inv_det;
    if (!is_finite(v) || v < 0.0 || u + v > 1.0)
        return false;
    auto const t = dot(e2, q) * inv_det;
    if (!is_finite(t) || t < 0.0)
        return false;
    out_t = t;
    return true;
}

/// segment/AABB overlap using the slab method for t in [0, max_t]
inline bool segment_hits_aabb(vec3 const& orig, vec3 const& dir, double max_t, vec3 const& mn, vec3 const& mx)
{
    double t0 = 0.0;
    double t1 = max_t;
    for (auto i = 0; i < 3; ++i)
    {
        if (dir[i] == 0.0)
        {
            if (orig[i] < mn[i] || orig[i] > mx[i])
                return false;
        }
        else
        {
            auto const inv = 1.0 / dir[i];
            auto a = (mn[i] - orig[i]) * inv;
            auto b = (mx[i] - orig[i]) * inv;
            if (a > b)
                std::swap(a, b);
            if (a > t0)
                t0 = a;
            if (b < t1)
                t1 = b;
            if (t0 > t1)
                return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

normal_bake_session::normal_bake_session(vertex_attribute<std::array<double, 4>> const& high_positions,
                                         vertex_attribute<std::array<double, 4>> const& low_positions,
                                         halfedge_attribute<std::array<double, 3>> const& low_uvs,
                                         int width,
                                         int height,
                                         double r)
{
    if (width <= 0)
        fail("width must be positive");
    if (height <= 0)
        fail("height must be positive");
    if (!(r > 0.0) || !is_finite(r))
        fail("projection half-length r must be positive and finite");

    mWidth = width;
    mHeight = height;
    mR = r;

    // ---- high-poly snapshot -------------------------------------------------
    auto const& high_mesh = high_positions.mesh();
    auto const high_vertex_count = high_positions.size();
    mHighPositions.resize(size_t(high_vertex_count));
    for (auto i = 0; i < high_vertex_count; ++i)
    {
        auto const& p = high_positions.data()[i];
        for (auto k = 0; k < 3; ++k)
            if (!is_finite(p[k]))
                fail("high-poly position is not finite");
        if (!is_finite(p[3]) || p[3] != 1.0)
            fail("high-poly position w must be 1");
        mHighPositions[size_t(i)] = make_vec(p[0], p[1], p[2]);
    }

    auto const high_face_count = high_mesh.all_faces().size();
    mHighFaces.assign(size_t(high_face_count), std::array<int, 3>{{-1, -1, -1}});
    mHighFaceValid.assign(size_t(high_face_count), 0);
    mFaceBoundsMin.assign(size_t(high_face_count), vec3{});
    mFaceBoundsMax.assign(size_t(high_face_count), vec3{});

    for (auto f : high_mesh.faces())
    {
        std::vector<int> vs;
        for (auto v : f.vertices())
            vs.push_back(v.idx.value);
        if (vs.size() != 3)
            fail("high-poly faces must be triangles");
        for (auto const vi : vs)
            if (vi < 0 || vi >= high_vertex_count)
                fail("invalid high-poly position slot");
        auto const fi = f.idx.value;
        mHighFaceValid[size_t(fi)] = 1;
        mHighFaces[size_t(fi)] = std::array<int, 3>{{vs[0], vs[1], vs[2]}};
    }

    // ---- world-degenerate check and per-face bounds -------------------------
    for (auto fi = 0; fi < high_face_count; ++fi)
    {
        if (!mHighFaceValid[size_t(fi)])
            continue;
        auto const& f = mHighFaces[size_t(fi)];
        auto const& p0 = mHighPositions[size_t(f[0])];
        auto const& p1 = mHighPositions[size_t(f[1])];
        auto const& p2 = mHighPositions[size_t(f[2])];
        auto const n = cross(p1 - p0, p2 - p0);
        if (!is_finite(n) || !(length_sq(n) > 0.0))
            fail("high-poly face is world-degenerate");

        for (auto k = 0; k < 3; ++k)
        {
            mFaceBoundsMin[size_t(fi)][k] = std::min(p0[k], std::min(p1[k], p2[k]));
            mFaceBoundsMax[size_t(fi)][k] = std::max(p0[k], std::max(p1[k], p2[k]));
        }
    }

    // ---- vertex -> high face reverse map (CSR) ------------------------------
    mVertexFaceOffsets.assign(size_t(high_vertex_count) + 1, 0);
    for (auto fi = 0; fi < high_face_count; ++fi)
        if (mHighFaceValid[size_t(fi)])
            for (auto const vi : mHighFaces[size_t(fi)])
                ++mVertexFaceOffsets[size_t(vi) + 1];
    for (auto i = 0; i < high_vertex_count; ++i)
        mVertexFaceOffsets[size_t(i) + 1] += mVertexFaceOffsets[size_t(i)];
    mVertexFaceList.resize(size_t(mVertexFaceOffsets.back()));
    {
        std::vector<int> cursor(mVertexFaceOffsets.begin(), mVertexFaceOffsets.end() - 1);
        for (auto fi = 0; fi < high_face_count; ++fi)
            if (mHighFaceValid[size_t(fi)])
                for (auto const vi : mHighFaces[size_t(fi)])
                    mVertexFaceList[size_t(cursor[size_t(vi)]++)] = fi;
    }

    // ---- low-poly snapshot --------------------------------------------------
    auto const& low_mesh = low_positions.mesh();
    auto const low_vertex_count = low_positions.size();
    std::vector<vec3> low_pos(static_cast<size_t>(low_vertex_count));
    for (auto i = 0; i < low_vertex_count; ++i)
    {
        auto const& p = low_positions.data()[i];
        for (auto k = 0; k < 3; ++k)
            if (!is_finite(p[k]))
                fail("low-poly position is not finite");
        if (!is_finite(p[3]) || p[3] != 1.0)
            fail("low-poly position w must be 1");
        low_pos[size_t(i)] = make_vec(p[0], p[1], p[2]);
    }

    auto const low_face_count = low_mesh.all_faces().size();
    mLowFaceValid.assign(size_t(low_face_count), 0);
    mLowFaces.assign(size_t(low_face_count), low_face{});

    for (auto f : low_mesh.faces())
    {
        std::vector<int> vs;
        std::vector<vec3> uvs;
        for (auto h : f.halfedges())
        {
            vs.push_back(h.vertex_to().idx.value);
            auto const& uv = low_uvs[h];
            for (auto k = 0; k < 3; ++k)
                if (!is_finite(uv[k]))
                    fail("low-poly UV is not finite");
            uvs.push_back(make_vec(uv[0], uv[1], 0.0));
        }
        if (vs.size() != 3)
            fail("low-poly faces must be triangles");
        for (auto const vi : vs)
            if (vi < 0 || vi >= low_vertex_count)
                fail("invalid low-poly position slot");

        auto const fi = f.idx.value;
        auto& lf = mLowFaces[size_t(fi)];

        for (auto k = 0; k < 3; ++k)
            lf.p_orig[k] = low_pos[size_t(vs[size_t(k)])];

        auto const n_orig = cross(lf.p_orig[1] - lf.p_orig[0], lf.p_orig[2] - lf.p_orig[0]);
        if (!is_finite(n_orig) || !(length_sq(n_orig) > 0.0))
            fail("low-poly face is world-degenerate");

        auto const uv_area = (uvs[1][0] - uvs[0][0]) * (uvs[2][1] - uvs[0][1]) - (uvs[2][0] - uvs[0][0]) * (uvs[1][1] - uvs[0][1]);
        if (uv_area == 0.0)
            continue; // zero UV area: ignore this face

        // unify to CCW in UV space; the world positions follow the permutation
        int order[3] = {0, 1, 2};
        if (uv_area < 0.0)
            std::swap(order[1], order[2]);

        for (auto k = 0; k < 3; ++k)
        {
            lf.p[k] = lf.p_orig[size_t(order[k])];
            lf.uv[k] = uvs[size_t(order[k])];
        }

        if (!normalize(n_orig, lf.n))
            fail("low-poly face normal is not finite");

        auto const& p0 = lf.p[0];
        auto const& p1 = lf.p[1];
        auto const& p2 = lf.p[2];
        auto const e1 = p1 - p0;
        auto const e2 = p2 - p0;

        auto const du1 = lf.uv[1][0] - lf.uv[0][0];
        auto const dv1 = lf.uv[1][1] - lf.uv[0][1];
        auto const du2 = lf.uv[2][0] - lf.uv[0][0];
        auto const dv2 = lf.uv[2][1] - lf.uv[0][1];
        auto const det = du1 * dv2 - du2 * dv1;
        if (det == 0.0 || !is_finite(det))
            continue; // defensive; zero UV area was already handled

        auto const inv_det = 1.0 / det;
        auto const dpdu = (e1 * dv2 - e2 * dv1) * inv_det;
        auto const dpdv = (e2 * du1 - e1 * du2) * inv_det;

        // tangent: dP/du orthogonalized against N
        vec3 t;
        {
            auto t_raw = dpdu - lf.n * dot(dpdu, lf.n);
            if (!normalize(t_raw, t))
            {
                t_raw = dpdv - lf.n * dot(dpdv, lf.n);
                if (!normalize(t_raw, t))
                    fail("low-poly tangent is not finite");
            }
        }
        vec3 b;
        if (!normalize(cross(lf.n, t), b))
            fail("low-poly bitangent is not finite");

        // handedness from dP/dv
        if (dot(dpdv, b) < 0.0)
            b = b * -1.0;

        lf.t = t;
        lf.b = b;

        mLowFaceValid[size_t(fi)] = 1;
    }

    // ---- coverage rasterization (texel centers, top-left rule) -------------
    auto const texel_count = size_t(mWidth) * size_t(mHeight);
    mResult.assign(texel_count, normal_bake_texel{});
    mTexelFace.assign(texel_count, -1);
    mTexelToRay.assign(texel_count, -1);

    for (auto fi = 0; fi < low_face_count; ++fi)
    {
        if (!mLowFaceValid[size_t(fi)])
            continue;
        auto const& lf = mLowFaces[size_t(fi)];

        double umin = lf.uv[0][0], umax = lf.uv[0][0];
        double vmin = lf.uv[0][1], vmax = lf.uv[0][1];
        for (auto k = 1; k < 3; ++k)
        {
            umin = std::min(umin, lf.uv[k][0]);
            umax = std::max(umax, lf.uv[k][0]);
            vmin = std::min(vmin, lf.uv[k][1]);
            vmax = std::max(vmax, lf.uv[k][1]);
        }

        auto x0 = int(std::floor(umin * double(mWidth) - 0.5));
        auto x1 = int(std::ceil(umax * double(mWidth) - 0.5));
        auto y0 = int(std::floor(vmin * double(mHeight) - 0.5));
        auto y1 = int(std::ceil(vmax * double(mHeight) - 0.5));
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, mWidth - 1);
        y1 = std::min(y1, mHeight - 1);

        for (auto y = y0; y <= y1; ++y)
            for (auto x = x0; x <= x1; ++x)
            {
                auto const p = make_vec((double(x) + 0.5) / double(mWidth), (double(y) + 0.5) / double(mHeight), 0.0);
                if (!edge_accepts(lf.uv[0], lf.uv[1], p))
                    continue;
                if (!edge_accepts(lf.uv[1], lf.uv[2], p))
                    continue;
                if (!edge_accepts(lf.uv[2], lf.uv[0], p))
                    continue;

                auto const t = size_t(y) * size_t(mWidth) + size_t(x);
                if (mTexelFace[t] != -1)
                    fail("overlapping UV coverage");
                mTexelFace[t] = fi;
            }
    }

    // ---- build per-texel rays and the projection segment index -------------
    std::vector<detail::finite_segment> segments;
    segments.reserve(texel_count);

    for (auto t = 0; t < int(texel_count); ++t)
    {
        auto const fi = mTexelFace[size_t(t)];
        if (fi < 0)
            continue;
        auto const& lf = mLowFaces[size_t(fi)];

        auto const x = t % mWidth;
        auto const y = t / mWidth;
        auto const p = make_vec((double(x) + 0.5) / double(mWidth), (double(y) + 0.5) / double(mHeight), 0.0);

        double wa, wb, wc;
        if (!barycentric_2d(lf.uv[0], lf.uv[1], lf.uv[2], p, wa, wb, wc))
        {
            // covered but no usable barycentric recovery: report a covered miss
            set_result(mResult[size_t(t)], true, fi, -1, 0.0, make_vec(0.0, 0.0, 0.0));
            continue;
        }

        // recover the world point from UV barycentrics
        auto const world = lf.p[0] * wa + lf.p[1] * wb + lf.p[2] * wc;

        texel_ray ray;
        ray.texel = t;
        ray.low_face = fi;
        ray.n = lf.n;
        ray.t = lf.t;
        ray.b = lf.b;
        ray.dir = lf.n * -1.0;
        ray.origin = world + lf.n * mR;
        ray.max_t = 2.0 * mR;

        auto const ray_index = int(mRays.size());
        mTexelToRay[size_t(t)] = ray_index;
        mRays.push_back(ray);

        detail::finite_segment seg;
        seg.origin[0] = ray.origin[0];
        seg.origin[1] = ray.origin[1];
        seg.origin[2] = ray.origin[2];
        seg.direction[0] = ray.dir[0];
        seg.direction[1] = ray.dir[1];
        seg.direction[2] = ray.dir[2];
        seg.max_t = ray.max_t;
        seg.id = t;
        segments.push_back(seg);
    }
    mSegments.build(segments);

    // ---- high-poly BVH ------------------------------------------------------
    build_high_bvh();

    // ---- initial intersection ----------------------------------------------
    for (auto const& ray : mRays)
    {
        double t = 0.0;
        int face = -1;
        if (cast_ray(ray, t, face))
            project_texel(ray, t, face);
        else
            set_result(mResult[size_t(ray.texel)], true, ray.low_face, -1, 0.0, make_vec(0.0, 0.0, 0.0));
    }

    mLastUpdate = normal_bake_update{};
}

// ---------------------------------------------------------------------------
// result helpers
// ---------------------------------------------------------------------------

void normal_bake_session::set_result(normal_bake_texel& r, bool coverage, int low_face_idx, int high_face_idx, double t, std::array<double, 3> const& n)
{
    r.coverage = coverage;
    r.hit = high_face_idx >= 0;
    r.low_face = low_face_idx;
    r.high_face = high_face_idx;
    r.t = t;
    r.normal = n;

    // keep the convenience aliases in sync
    r.covered = coverage;
    r.low_face_index = low_face_idx;
    r.high_face_index = high_face_idx;
    r.low_poly_face = low_face_idx;
    r.high_poly_face = high_face_idx;
    r.ray_parameter = t;
    r.tangent_normal = n;
}

void normal_bake_session::clear_result(normal_bake_texel& r) { set_result(r, false, -1, -1, 0.0, make_vec(0.0, 0.0, 0.0)); }

void normal_bake_session::project_texel(texel_ray const& ray, double t, int high_face)
{
    auto const& f = mHighFaces[size_t(high_face)];
    auto const n = cross(mHighPositions[size_t(f[1])] - mHighPositions[size_t(f[0])],
                         mHighPositions[size_t(f[2])] - mHighPositions[size_t(f[0])]);
    vec3 high_n;
    if (!normalize(n, high_n))
        high_n = make_vec(0.0, 0.0, 0.0);

    // never flip the high-poly normal to face the ray
    auto const local = make_vec(dot(high_n, ray.t), dot(high_n, ray.b), dot(high_n, ray.n));
    vec3 encoded;
    if (!normalize(local, encoded))
        encoded = make_vec(0.0, 0.0, 0.0);

    set_result(mResult[size_t(ray.texel)], true, ray.low_face, high_face, t, encoded);
}

// ---------------------------------------------------------------------------
// BVH construction
// ---------------------------------------------------------------------------

void normal_bake_session::build_high_bvh()
{
    mBvhNodes.clear();
    mBvhFaceOrder.clear();
    mFaceLeaf.assign(mHighFaces.size(), -1);

    for (auto fi = 0; fi < int(mHighFaces.size()); ++fi)
        if (mHighFaceValid[size_t(fi)])
            mBvhFaceOrder.push_back(fi);

    if (mBvhFaceOrder.empty())
        return;

    mBvhNodes.reserve(mBvhFaceOrder.size() * 2);
    build_high_bvh_recursive(0, int(mBvhFaceOrder.size()), -1);
}

int normal_bake_session::build_high_bvh_recursive(int begin, int end, int parent)
{
    auto const idx = int(mBvhNodes.size());
    mBvhNodes.push_back(normal_bake_bvh_node{});
    auto& node = mBvhNodes[size_t(idx)];
    node.parent = parent;

    // bounds of the current range
    vec3 mn = mFaceBoundsMin[size_t(mBvhFaceOrder[size_t(begin)])];
    vec3 mx = mFaceBoundsMax[size_t(mBvhFaceOrder[size_t(begin)])];
    for (auto i = begin + 1; i < end; ++i)
    {
        auto const fi = size_t(mBvhFaceOrder[size_t(i)]);
        for (auto k = 0; k < 3; ++k)
        {
            mn[k] = std::min(mn[k], mFaceBoundsMin[fi][k]);
            mx[k] = std::max(mx[k], mFaceBoundsMax[fi][k]);
        }
    }
    node.min = mn;
    node.max = mx;

    auto const count = end - begin;
    if (count <= 4)
    {
        node.first = begin;
        node.count = count;
        for (auto i = begin; i < end; ++i)
            mFaceLeaf[size_t(mBvhFaceOrder[size_t(i)])] = idx;
        return idx;
    }

    // split along the longest axis at the median of the face centroids
    auto axis = 0;
    auto extent = mx[0] - mn[0];
    for (auto k = 1; k < 3; ++k)
        if (mx[k] - mn[k] > extent)
        {
            axis = k;
            extent = mx[k] - mn[k];
        }

    auto const mid = begin + count / 2;
    std::nth_element(mBvhFaceOrder.begin() + begin, mBvhFaceOrder.begin() + mid, mBvhFaceOrder.begin() + end, [&](int a, int b) {
        auto const ca = mFaceBoundsMin[size_t(a)][axis] + mFaceBoundsMax[size_t(a)][axis];
        auto const cb = mFaceBoundsMin[size_t(b)][axis] + mFaceBoundsMax[size_t(b)][axis];
        return ca < cb;
    });

    auto const left = build_high_bvh_recursive(begin, mid, idx);
    auto const right = build_high_bvh_recursive(mid, end, idx);
    mBvhNodes[size_t(idx)].left = left;
    mBvhNodes[size_t(idx)].right = right;
    return idx;
}

void normal_bake_session::recompute_node_bounds(int node_idx)
{
    auto& node = mBvhNodes[size_t(node_idx)];
    if (node.is_leaf())
    {
        auto const fi = size_t(mBvhFaceOrder[size_t(node.first)]);
        vec3 mn = mFaceBoundsMin[fi];
        vec3 mx = mFaceBoundsMax[fi];
        for (auto i = node.first + 1; i < node.first + node.count; ++i)
        {
            auto const f = size_t(mBvhFaceOrder[size_t(i)]);
            for (auto k = 0; k < 3; ++k)
            {
                mn[k] = std::min(mn[k], mFaceBoundsMin[f][k]);
                mx[k] = std::max(mx[k], mFaceBoundsMax[f][k]);
            }
        }
        node.min = mn;
        node.max = mx;
    }
    else
    {
        auto const& l = mBvhNodes[size_t(node.left)];
        auto const& r = mBvhNodes[size_t(node.right)];
        for (auto k = 0; k < 3; ++k)
        {
            node.min[k] = std::min(l.min[k], r.min[k]);
            node.max[k] = std::max(l.max[k], r.max[k]);
        }
    }
}

// ---------------------------------------------------------------------------
// ray casting
// ---------------------------------------------------------------------------

bool normal_bake_session::cast_ray(texel_ray const& ray, double& out_t, int& out_face) const
{
    if (mBvhNodes.empty())
        return false;

    auto best_t = std::numeric_limits<double>::infinity();
    auto best_face = -1;
    auto const max_t_eps = 1e-12 * std::max(1.0, ray.max_t);

    int stack[64];
    auto stack_size = 0;
    stack[stack_size++] = 0;

    // Fallback stack used only if the (balanced, log-depth) tree ever exceeds
    // the fixed stack, so traversal never silently skips a subtree.
    std::vector<int> overflow_stack;

    while (stack_size > 0 || !overflow_stack.empty())
    {
        auto ni = 0;
        if (stack_size > 0)
            ni = stack[--stack_size];
        else
        {
            ni = overflow_stack.back();
            overflow_stack.pop_back();
        }
        auto const& node = mBvhNodes[size_t(ni)];

        if (!segment_hits_aabb(ray.origin, ray.dir, ray.max_t, node.min, node.max))
            continue;

        if (node.is_leaf())
        {
            for (auto i = node.first; i < node.first + node.count; ++i)
            {
                auto const fi = mBvhFaceOrder[size_t(i)];
                auto const& f = mHighFaces[size_t(fi)];
                auto const& a = mHighPositions[size_t(f[0])];
                auto const& b = mHighPositions[size_t(f[1])];
                auto const& c = mHighPositions[size_t(f[2])];

                double t;
                if (!ray_triangle(ray.origin, ray.dir, a, b, c, t))
                    continue;
                if (t > ray.max_t + max_t_eps)
                    continue;

                // equal distances: prefer the smaller original high-poly face index
                if (best_face < 0 || t < best_t || (t == best_t && fi < best_face))
                {
                    best_t = t;
                    best_face = fi;
                }
            }
        }
        else
        {
            if (stack_size + 2 <= 64)
            {
                stack[stack_size++] = node.left;
                stack[stack_size++] = node.right;
            }
            else
            {
                overflow_stack.push_back(node.left);
                overflow_stack.push_back(node.right);
            }
        }
    }

    if (best_face < 0)
        return false;

    out_t = best_t;
    out_face = best_face;
    return true;
}

// ---------------------------------------------------------------------------
// updates
// ---------------------------------------------------------------------------

void normal_bake_session::update_positions(std::vector<std::pair<int, std::array<double, 3>>> const& updates)
{
    if (updates.empty())
    {
        mLastUpdate = normal_bake_update{};
        return;
    }

    auto const high_vertex_count = int(mHighPositions.size());

    // duplicate slots: the last value wins
    std::unordered_map<int, vec3> patch;
    patch.reserve(updates.size());
    for (auto const& u : updates)
    {
        auto const slot = u.first;
        if (slot < 0 || slot >= high_vertex_count)
            fail("invalid high-poly position slot in update");
        auto const v = make_vec(u.second[0], u.second[1], u.second[2]);
        if (!is_finite(v))
            fail("non-finite high-poly position in update");
        patch[slot] = v;
    }

    // only genuinely moved vertices cause work
    std::vector<int> changed;
    changed.reserve(patch.size());
    for (auto const& kv : patch)
        if (kv.second != mHighPositions[size_t(kv.first)])
            changed.push_back(kv.first);
    std::sort(changed.begin(), changed.end());

    if (changed.empty())
    {
        mLastUpdate = normal_bake_update{};
        return;
    }

    auto candidate_position = [&](int slot) -> vec3 const& {
        auto it = patch.find(slot);
        return it != patch.end() ? it->second : mHighPositions[size_t(slot)];
    };

    // affected faces: all faces adjacent to a changed vertex
    std::vector<int> affected;
    for (auto const slot : changed)
        for (auto i = mVertexFaceOffsets[size_t(slot)]; i < mVertexFaceOffsets[size_t(slot) + 1]; ++i)
            affected.push_back(mVertexFaceList[size_t(i)]);
    std::sort(affected.begin(), affected.end());
    affected.erase(std::unique(affected.begin(), affected.end()), affected.end());

    // validate that no affected face degenerates (before mutating anything)
    for (auto const fi : affected)
    {
        auto const& f = mHighFaces[size_t(fi)];
        auto const& p0 = candidate_position(f[0]);
        auto const& p1 = candidate_position(f[1]);
        auto const& p2 = candidate_position(f[2]);
        auto const n = cross(p1 - p0, p2 - p0);
        if (!is_finite(n) || !(length_sq(n) > 0.0))
            fail("high-poly face becomes world-degenerate after update");
    }

    // remember old bounds for the segment index query
    std::vector<vec3> old_min(affected.size());
    std::vector<vec3> old_max(affected.size());
    for (auto i = 0u; i < affected.size(); ++i)
    {
        old_min[i] = mFaceBoundsMin[size_t(affected[i])];
        old_max[i] = mFaceBoundsMax[size_t(affected[i])];
    }

    // commit positions
    for (auto const& kv : patch)
        mHighPositions[size_t(kv.first)] = kv.second;

    // recompute affected face bounds
    std::vector<vec3> new_min(affected.size());
    std::vector<vec3> new_max(affected.size());
    for (auto i = 0u; i < affected.size(); ++i)
    {
        auto const fi = affected[i];
        auto const& f = mHighFaces[size_t(fi)];
        auto const& p0 = mHighPositions[size_t(f[0])];
        auto const& p1 = mHighPositions[size_t(f[1])];
        auto const& p2 = mHighPositions[size_t(f[2])];
        for (auto k = 0; k < 3; ++k)
        {
            new_min[i][k] = std::min(p0[k], std::min(p1[k], p2[k]));
            new_max[i][k] = std::max(p0[k], std::max(p1[k], p2[k]));
        }
        mFaceBoundsMin[size_t(fi)] = new_min[i];
        mFaceBoundsMax[size_t(fi)] = new_max[i];
    }

    // recompute leaf bounds and all ancestors; record touched nodes
    std::unordered_set<int> touched;
    for (auto const fi : affected)
    {
        auto node = mFaceLeaf[size_t(fi)];
        while (node >= 0 && touched.insert(node).second)
            node = mBvhNodes[size_t(node)].parent;
    }

    std::vector<int> compute_order(touched.begin(), touched.end());
    // children have larger indices than their parents, so descending recomputes
    // leaves before the ancestors that depend on them
    std::sort(compute_order.begin(), compute_order.end(), std::greater<int>());
    for (auto const n : compute_order)
        recompute_node_bounds(n);

    std::vector<int> nodes(compute_order.begin(), compute_order.end());
    std::sort(nodes.begin(), nodes.end());

    // candidate texels: old and new bounds of the affected faces
    std::vector<int> candidates;
    for (auto i = 0u; i < affected.size(); ++i)
    {
        detail::segment_aabb old_box;
        detail::segment_aabb new_box;
        for (auto k = 0; k < 3; ++k)
        {
            old_box.min[k] = old_min[i][k];
            old_box.max[k] = old_max[i][k];
            new_box.min[k] = new_min[i][k];
            new_box.max[k] = new_max[i][k];
        }
        mSegments.query(old_box, candidates);
        mSegments.query(new_box, candidates);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    // reproject each candidate texel; only texels with an actual ray count as
    // "re-projected" in the update record
    std::vector<int> reprojected;
    reprojected.reserve(candidates.size());
    for (auto const texel : candidates)
    {
        auto const ri = mTexelToRay[size_t(texel)];
        if (ri < 0)
            continue;
        auto const& ray = mRays[size_t(ri)];
        reprojected.push_back(texel);

        double t = 0.0;
        int face = -1;
        if (cast_ray(ray, t, face))
            project_texel(ray, t, face);
        else
            set_result(mResult[size_t(texel)], true, ray.low_face, -1, 0.0, make_vec(0.0, 0.0, 0.0));
    }

    mLastUpdate = normal_bake_update{};
    mLastUpdate.faces = affected;
    mLastUpdate.texels = reprojected;
    mLastUpdate.nodes = nodes;
}
} // namespace polymesh
