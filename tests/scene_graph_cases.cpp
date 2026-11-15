#include <FastBVH.h>
#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using FastBVH::Ray;
using FastBVH::Vector3;
using namespace FastBVH::SceneGraph;

namespace {

using Mat = std::array<double, 12>;

Mat identity() { return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}; }

Mat translation(double x, double y, double z) {
  return {{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z}};
}

Mat diag(double sx, double sy, double sz) {
  return {{sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, sz, 0}};
}

Mat rotation_z(double ang) {
  const double c = std::cos(ang);
  const double s = std::sin(ang);
  return {{c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0}};
}

Mat mirror_x() { return {{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}; }

bool nearly(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

bool nearly_vec(const Vec3& a, const Vec3& b, double eps = 1e-9) {
  return nearly(a.x, b.x, eps) && nearly(a.y, b.y, eps) && nearly(a.z, b.z, eps);
}

Vec3 xform_point(const Mat& m, const Vec3& p) {
  return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
              m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
              m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

// ---- An independent reference implementation that fully expands a small
// scene into world-space triangles and brute-forces every ray. It never
// consults any FastBVH acceleration structure. ----

struct RefTriangle {
  Vec3 a, b, c;
  uint32_t material;
  std::vector<uint32_t> path;
  uint32_t primitive_id;
  NodeId mesh;
};

struct RefWorld {
  std::vector<RefTriangle> tris;
};

bool ref_triangle_hit(const RefTriangle& t, const Ray<double>& ray, double tmin, double tmax,
                      double& out_t) {
  const Vec3 e1 = t.b - t.a;
  const Vec3 e2 = t.c - t.a;
  const Vec3 h = FastBVH::cross(ray.d, e2);
  const double det = FastBVH::dot(e1, h);
  if (det == 0.0) return false;
  const double f = 1.0 / det;
  const Vec3 s = ray.o - t.a;
  const double u = f * FastBVH::dot(s, h);
  if (u < 0.0 || u > 1.0) return false;
  const Vec3 q = FastBVH::cross(s, e1);
  const double v = f * FastBVH::dot(ray.d, q);
  if (v < 0.0 || u + v > 1.0) return false;
  out_t = f * FastBVH::dot(e2, q);
  return out_t >= tmin && out_t <= tmax;
}

struct RefHit {
  bool hit = false;
  double t = 0.0;
  const RefTriangle* tri = nullptr;
};

RefHit ref_closest(const RefWorld& w, const Ray<double>& ray, double tmin, double tmax) {
  RefHit best;
  for (const RefTriangle& t : w.tris) {
    double tt = 0.0;
    if (!ref_triangle_hit(t, ray, tmin, tmax, tt)) continue;
    const bool better =
        !best.hit || tt < best.t ||
        (tt == best.t &&
         (t.path < best.tri->path || (t.path == best.tri->path && t.primitive_id < best.tri->primitive_id)));
    if (better) {
      best.hit = true;
      best.t = tt;
      best.tri = &t;
    }
  }
  return best;
}

bool ref_occluded(const RefWorld& w, const Ray<double>& ray, double tmin, double tmax) {
  for (const RefTriangle& t : w.tris) {
    double tt = 0.0;
    if (ref_triangle_hit(t, ray, tmin, tmax, tt)) return true;
  }
  return false;
}

Vec3 ref_normal(const RefTriangle& t) {
  return FastBVH::normalize(FastBVH::cross(t.b - t.a, t.c - t.a));
}

// The reference is kept independent of the scene internals: the test
// rebuilds the same geometry from the very inputs used to construct the
// scene, expands every instantiation path to world-space triangles, and
// brute-forces each ray against them.
struct RefNode {
  bool is_mesh = false;
  std::vector<Triangle> mesh;
  std::vector<Edge> edges;
};

void ref_expand_nodes(const std::vector<RefNode>& nodes, NodeId node, const Mat& world,
                      std::vector<uint32_t> path, int64_t override_material, RefWorld& out) {
  const RefNode& n = nodes[node];
  if (n.is_mesh) {
    for (uint32_t i = 0; i < n.mesh.size(); ++i) {
      const Triangle& tri = n.mesh[i];
      RefTriangle rt;
      rt.a = xform_point(world, tri.a);
      rt.b = xform_point(world, tri.b);
      rt.c = xform_point(world, tri.c);
      rt.material = override_material >= 0 ? static_cast<uint32_t>(override_material) : tri.material;
      rt.path = path;
      rt.primitive_id = i;
      rt.mesh = node;
      out.tris.push_back(rt);
    }
    return;
  }
  for (const Edge& e : n.edges) {
    Mat child_world = Impl::compose(world, e.transform);
    std::vector<uint32_t> child_path = path;
    child_path.push_back(e.id);
    const int64_t child_override =
        override_material >= 0 ? override_material : e.material_override;
    ref_expand_nodes(nodes, e.child, child_world, child_path, child_override, out);
  }
}

RefWorld make_ref_world(const std::vector<RefNode>& nodes, NodeId root) {
  RefWorld w;
  ref_expand_nodes(nodes, root, identity(), std::vector<uint32_t>{}, -1, w);
  return w;
}

// =====================================================================
// dag group: construction, sharing, storage and validation errors
// =====================================================================

int dag_storage_no_expansion() {
  Scene scene;
  std::vector<RefNode> ref;

  // A single one-triangle mesh shared by a long chain of groups.
  const NodeId mesh = scene.add_mesh(
      std::vector<Triangle>{Triangle{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 1}});
  ref.push_back(RefNode{true, {{ {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 1 }}, {}});

  NodeId child = mesh;
  const int levels = 20;
  for (int level = 0; level < levels; ++level) {
    // Both edges keep the shared child at the same place so the 2^20
    // instantiation paths all overlap; storage must still stay linear.
    std::vector<Edge> edges;
    edges.push_back(Edge{1u, child, identity(), 0xFFFFFFFFu, -1});
    edges.push_back(Edge{2u, child, identity(), 0xFFFFFFFFu, -1});
    const NodeId g = scene.add_group(std::move(edges));
    ref.push_back(RefNode{false, {}, {Edge{1u, child, identity(), 0xFFFFFFFFu, -1},
                                      Edge{2u, child, identity(), 0xFFFFFFFFu, -1}}});
    child = g;
  }

  const StorageStats st = scene.storage_stats();
  TASK_CHECK(st.mesh_count == 1);
  TASK_CHECK(st.group_count == static_cast<uint64_t>(levels));
  TASK_CHECK(st.stored_triangles == 1);
  TASK_CHECK(st.stored_edges == static_cast<uint64_t>(levels) * 2u);

  // The exponential instantiation (2^20 paths) must never be materialized:
  // the scene still answers a ray promptly.
  Hit hit;
  QueryStats stats;
  const Ray<double> ray({0.25, 0.25, 2.0}, {0.0, 0.0, -1.0});
  const bool ok = scene.closest(child, ray, 0.0, 100.0, 0xFFFFFFFFu, hit, &stats);
  TASK_CHECK(ok);
  TASK_CHECK(hit.path.size() == static_cast<size_t>(levels));
  TASK_CHECK(hit.mesh == mesh);
  TASK_CHECK(stats.triangle_tests >= 1);
  return 0;
}

int dag_validation_and_empty_nodes() {
  Scene scene;

  // Empty mesh and empty group are legal and stable.
  const NodeId empty_mesh = scene.add_mesh(std::vector<Triangle>{});
  const NodeId empty_group = scene.add_group(std::vector<Edge>{});
  TASK_CHECK(empty_mesh != empty_group);

  StorageStats st = scene.storage_stats();
  TASK_CHECK(st.mesh_count == 1);
  TASK_CHECK(st.group_count == 1);
  TASK_CHECK(st.stored_triangles == 0);
  TASK_CHECK(st.stored_edges == 0);

  // Querying an empty group returns a miss without touching a triangle.
  QueryStats stats;
  Hit hit;
  const bool miss =
      scene.closest(empty_group, Ray<double>({0, 0, 1}, {0, 0, -1}), 0.0, 10.0, 0xFFFFFFFFu, hit, &stats);
  TASK_CHECK(!miss);
  TASK_CHECK(stats.triangle_tests == 0);
  TASK_CHECK(stats.edge_tests == 0);

  // A group may legally reference an empty child.
  const NodeId wrapper =
      scene.add_group(std::vector<Edge>{Edge{9u, empty_mesh, identity(), 0xFFFFFFFFu, -1}});
  TASK_CHECK(!scene.occluded(wrapper, Ray<double>({0, 0, 1}, {0, 0, -1}), 0.0, 10.0, 0xFFFFFFFFu));

  // Invalid mesh input: degenerate and non-finite triangles throw.
  bool threw = false;
  try {
    scene.add_mesh(std::vector<Triangle>{Triangle{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, 0}});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  threw = false;
  try {
    const double inf = std::numeric_limits<double>::infinity();
    scene.add_mesh(std::vector<Triangle>{
        Triangle{{0, 0, 0}, {1, 0, 0}, {0, inf, 0}, 0}});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  const NodeId mesh =
      scene.add_mesh(std::vector<Triangle>{Triangle{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 4}});

  // Unknown child, duplicate ids, singular/non-finite transform, bad override.
  threw = false;
  try {
    scene.add_group(std::vector<Edge>{Edge{1u, 9999u, identity(), 0xFFFFFFFFu, -1}});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  threw = false;
  try {
    std::vector<Edge> dup{Edge{1u, mesh, translation(1, 0, 0), 0xFFFFFFFFu, -1},
                          Edge{1u, mesh, translation(2, 0, 0), 0xFFFFFFFFu, -1}};
    scene.add_group(std::move(dup));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  threw = false;
  try {
    Mat singular{};  // all zeros -> determinant zero
    scene.add_group(std::vector<Edge>{Edge{1u, mesh, singular, 0xFFFFFFFFu, -1}});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  threw = false;
  try {
    Mat bad = identity();
    bad[3] = std::numeric_limits<double>::quiet_NaN();
    scene.add_group(std::vector<Edge>{Edge{1u, mesh, bad, 0xFFFFFFFFu, -1}});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  threw = false;
  try {
    scene.add_group(
        std::vector<Edge>{Edge{1u, mesh, identity(), 0xFFFFFFFFu, -2}});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  // A failed add must not have allocated a node id.
  st = scene.storage_stats();
  TASK_CHECK(st.mesh_count == 2);
  return 0;
}

int dag_diamond_sharing_and_4096_counters() {
  Scene scene;
  const NodeId mesh = scene.add_mesh(
      std::vector<Triangle>{Triangle{{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, 7}});

  const NodeId bottom =
      scene.add_group(std::vector<Edge>{Edge{7u, mesh, identity(), 0xFFFFFFFFu, -1}});
  const NodeId left =
      scene.add_group(std::vector<Edge>{Edge{10u, bottom, identity(), 0xFFFFFFFFu, 20}});
  const NodeId right =
      scene.add_group(std::vector<Edge>{Edge{20u, bottom, identity(), 0xFFFFFFFFu, 30}});
  const NodeId root = scene.add_group(
      std::vector<Edge>{Edge{100u, left, identity(), 0xFFFFFFFFu, -1},
                        Edge{200u, right, identity(), 0xFFFFFFFFu, -1}});

  // The bottom group is shared: stored once, reachable by two paths.
  const StorageStats st = scene.storage_stats();
  TASK_CHECK(st.mesh_count == 1);
  TASK_CHECK(st.group_count == 4);
  TASK_CHECK(st.stored_edges == 5);

  // 4096 x-separated edges referencing the very same single-triangle mesh.
  Scene wide;
  const NodeId wide_mesh = wide.add_mesh(
      std::vector<Triangle>{Triangle{{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, 1}});
  std::vector<Edge> edges;
  const unsigned N = 4096;
  for (unsigned i = 0; i < N; ++i) {
    edges.push_back(Edge{i, wide_mesh, translation(double(i) * 3.0, 0, 0), 0xFFFFFFFFu, -1});
  }
  const NodeId wide_root = wide.add_group(std::move(edges));
  const StorageStats wst = wide.storage_stats();
  TASK_CHECK(wst.stored_edges == N);
  TASK_CHECK(wst.stored_triangles == 1);

  const unsigned slot = 2025;
  Hit hit;
  QueryStats stats;
  const Ray<double> ray({double(slot) * 3.0, 0.0, -5.0}, {0.0, 0.0, 1.0});
  const bool ok = wide.closest(wide_root, ray, 0.0, 1e9, 0xFFFFFFFFu, hit, &stats);
  TASK_CHECK(ok);
  TASK_CHECK(hit.path.size() == 1);
  TASK_CHECK(hit.path[0] == slot);
  TASK_CHECK(stats.edge_tests <= 64);
  TASK_CHECK(stats.triangle_tests <= 64);

  // A ray missing every instance still prunes via the BVH, never line scans.
  QueryStats miss_stats;
  Hit miss_hit;
  const Ray<double> miss_ray({-100000.0, 50.0, -5.0}, {0.0, 0.0, 1.0});
  TASK_CHECK(!wide.closest(wide_root, miss_ray, 0.0, 1e9, 0xFFFFFFFFu, miss_hit, &miss_stats));
  TASK_CHECK(miss_stats.edge_tests <= 64);
  TASK_CHECK(miss_stats.triangle_tests == 0);
  (void)root;
  return 0;
}

// =====================================================================
// paths group: reference-expanded geometry, masks, overrides, transforms
// =====================================================================

int paths_match_reference_basic() {
  Scene scene;
  std::vector<RefNode> ref;

  std::vector<Triangle> mesh_tris;
  mesh_tris.push_back(Triangle{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 11});
  mesh_tris.push_back(Triangle{{2, 0, 0}, {3, 0, 0}, {2, 1, 0}, 12});
  const NodeId mesh = scene.add_mesh(mesh_tris);
  ref.push_back(RefNode{true, mesh_tris, {}});

  std::vector<Edge> gedges;
  gedges.push_back(Edge{5u, mesh, translation(0, 0, 0), 0xFFFFFFFFu, -1});
  const NodeId group = scene.add_group(std::move(gedges));
  ref.push_back(RefNode{false, {}, {Edge{5u, mesh, identity(), 0xFFFFFFFFu, -1}}});

  const RefWorld world = make_ref_world(ref, group);

  const double origins[][2] = {{0.25, 0.25}, {2.25, 0.25}, {0.9, 0.9}, {1.5, 0.5}};
  for (const auto& o : origins) {
    const Ray<double> ray({o[0], o[1], 4.0}, {0.0, 0.0, -1.0});
    Hit hit;
    QueryStats stats;
    const bool ok = scene.closest(group, ray, 0.0, 50.0, 0xFFFFFFFFu, hit, &stats);
    const RefHit rh = ref_closest(world, ray, 0.0, 50.0);
    TASK_CHECK(ok == rh.hit);
    if (rh.hit) {
      TASK_CHECK(nearly(hit.t, rh.t));
      TASK_CHECK(hit.path == rh.tri->path);
      TASK_CHECK(hit.primitive_id == rh.tri->primitive_id);
      TASK_CHECK(hit.material == rh.tri->material);
      const Vec3 expected_pos = ray.o + ray.d * rh.t;
      TASK_CHECK(nearly_vec(hit.position, expected_pos));
      TASK_CHECK(nearly_vec(hit.normal, ref_normal(*rh.tri)));
      TASK_CHECK(nearly(FastBVH::length(hit.normal), 1.0));
    }
  }

  // Finite occlusion over a closed window: the surface is at t = 4.
  const Ray<double> shadow({0.25, 0.25, 4.0}, {0.0, 0.0, -1.0});
  TASK_CHECK(scene.occluded(group, shadow, 0.0, 4.0, 0xFFFFFFFFu));
  TASK_CHECK(!scene.occluded(group, shadow, 0.0, 3.9, 0xFFFFFFFFu));
  TASK_CHECK(scene.occluded(group, shadow, 4.0, 5.0, 0xFFFFFFFFu));
  TASK_CHECK(!scene.occluded(group, shadow, 4.1, 5.0, 0xFFFFFFFFu));
  TASK_CHECK(ref_occluded(world, shadow, 0.0, 4.0));
  TASK_CHECK(!ref_occluded(world, shadow, 0.0, 3.9));
  TASK_CHECK(ref_occluded(world, shadow, 4.0, 5.0));
  TASK_CHECK(!ref_occluded(world, shadow, 4.1, 5.0));

  // On a miss the output Hit is left untouched.
  Hit sentinel;
  sentinel.t = 12345.0;
  sentinel.material = 4242u;
  Hit probe = sentinel;
  const bool missed =
      scene.closest(group, Ray<double>({50.0, 50.0, 4.0}, {0, 0, -1}), 0.0, 1.0, 0xFFFFFFFFu, probe);
  TASK_CHECK(!missed);
  TASK_CHECK(nearly(probe.t, sentinel.t));
  TASK_CHECK(probe.material == sentinel.material);
  return 0;
}

int paths_nested_transforms_vs_reference() {
  Scene scene;
  std::vector<RefNode> ref;

  // Two triangles with distinct winding and materials.
  std::vector<Triangle> mesh_tris;
  mesh_tris.push_back(Triangle{{-0.5, -0.5, 0}, {0.5, -0.5, 0}, {0.0, 0.5, 0}, 3});
  const NodeId mesh = scene.add_mesh(mesh_tris);
  ref.push_back(RefNode{true, mesh_tris, {}});

  // Inner group: non-uniform scale + shear.
  Mat inner = diag(2.0, 0.5, 1.0);
  inner[1] = 0.25;  // shear: y' += 0.25 * x
  std::vector<Edge> inner_edges{Edge{1u, mesh, inner, 0xFFFFFFFFu, -1}};
  const NodeId inner_group = scene.add_group(std::move(inner_edges));
  ref.push_back(RefNode{false, {}, {Edge{1u, mesh, inner, 0xFFFFFFFFu, -1}}});

  // Middle group: rotation.
  const Mat middle_m = rotation_z(0.35);
  std::vector<Edge> middle_edges{Edge{2u, inner_group, middle_m, 0xFFFFFFFFu, -1}};
  const NodeId middle_group = scene.add_group(std::move(middle_edges));
  ref.push_back(RefNode{false, {}, {Edge{2u, inner_group, middle_m, 0xFFFFFFFFu, -1}}});

  // Outer group: mirror in x (negative determinant flips winding).
  const Mat outer_m = mirror_x();
  std::vector<Edge> outer_edges{Edge{3u, middle_group, outer_m, 0xFFFFFFFFu, -1}};
  const NodeId root = scene.add_group(std::move(outer_edges));
  ref.push_back(RefNode{false, {}, {Edge{3u, middle_group, outer_m, 0xFFFFFFFFu, -1}}});

  const RefWorld world = make_ref_world(ref, root);
  TASK_CHECK(world.tris.size() == 1);

  // Shoot a grid of rays; compare every result against the brute reference.
  for (int ix = -4; ix <= 4; ++ix) {
    for (int iy = -4; iy <= 4; ++iy) {
      const Ray<double> ray({double(ix) * 0.3, double(iy) * 0.3, 6.0}, {0.0, 0.0, -1.0});
      Hit hit;
      const bool ok = scene.closest(root, ray, 0.0, 100.0, 0xFFFFFFFFu, hit);
      const RefHit rh = ref_closest(world, ray, 0.0, 100.0);
      TASK_CHECK(ok == rh.hit);
      if (rh.hit) {
        TASK_CHECK(nearly(hit.t, rh.t, 1e-8));
        TASK_CHECK(hit.path == rh.tri->path);
        TASK_CHECK(hit.primitive_id == rh.tri->primitive_id);
        TASK_CHECK(nearly_vec(hit.normal, ref_normal(*rh.tri), 1e-8));
        TASK_CHECK(nearly(FastBVH::length(hit.normal), 1.0, 1e-12));
      }
    }
  }

  // The mirrored world normal must point opposite to the unmirrored one.
  const RefWorld unmirrored = [&] {
    RefWorld w;
    std::vector<uint32_t> p{2u, 1u};
    Mat wm = Impl::compose(middle_m, inner);
    RefTriangle rt;
    rt.a = xform_point(wm, mesh_tris[0].a);
    rt.b = xform_point(wm, mesh_tris[0].b);
    rt.c = xform_point(wm, mesh_tris[0].c);
    rt.material = 3;
    rt.path = p;
    rt.primitive_id = 0;
    w.tris.push_back(rt);
    return w;
  }();
  TASK_CHECK(world.tris[0].a.x == -unmirrored.tris[0].a.x);
  return 0;
}

int paths_masks_and_overrides() {
  Scene scene;
  const NodeId mesh = scene.add_mesh(
      std::vector<Triangle>{Triangle{{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, 100}});

  // Deep group whose edge carries the deeper override (should lose).
  const NodeId deep = scene.add_group(
      std::vector<Edge>{Edge{90u, mesh, identity(), 0xFFFFFFFFu, 555}});
  // Shallow group overrides first (should win) and uses a non-trivial mask.
  const NodeId shallow = scene.add_group(
      std::vector<Edge>{Edge{80u, deep, identity(), 0xFFFFFFFFu, 333}});
  const NodeId root = scene.add_group(
      std::vector<Edge>{Edge{70u, shallow, identity(), 0xFFFFFFFFu, -1}});

  const Ray<double> ray({0.0, 0.0, -5.0}, {0.0, 0.0, 1.0});
  Hit hit;
  TASK_CHECK(scene.closest(root, ray, 0.0, 100.0, 0xFFFFFFFFu, hit));
  TASK_CHECK(hit.material == 333u);
  TASK_CHECK(hit.path.size() == 3);

  // A path whose per-edge masks are each non-zero but AND to zero is culled.
  const NodeId g2 = scene.add_group(
      std::vector<Edge>{Edge{60u, mesh, identity(), 0x2u /* bit 1 */, -1}});
  const NodeId g1 = scene.add_group(
      std::vector<Edge>{Edge{50u, g2, identity(), 0x1u /* bit 0 */, -1}});
  // Query bit 0: edge g1 passes (0x1 & 0x1), then g2 fails (0x1 & 0x2 == 0).
  TASK_CHECK(!scene.occluded(g1, ray, 0.0, 100.0, 0x1u));
  // Querying both bits opens the whole path.
  TASK_CHECK(scene.occluded(g1, ray, 0.0, 100.0, 0x3u));

  // No override anywhere: the face material survives.
  const NodeId plain = scene.add_group(
      std::vector<Edge>{Edge{40u, mesh, identity(), 0xFFFFFFFFu, -1}});
  Hit plain_hit;
  TASK_CHECK(scene.closest(plain, ray, 0.0, 100.0, 0xFFFFFFFFu, plain_hit));
  TASK_CHECK(plain_hit.material == 100u);

  // Query mask zero culls even a fully unmasked edge path.
  TASK_CHECK(!scene.occluded(plain, ray, 0.0, 100.0, 0x0u));
  return 0;
}

int paths_container_ownership_and_bvh_reorder() {
  Scene scene;

  // Build the source containers in an inner scope so they are destroyed.
  NodeId mesh;
  NodeId root;
  {
    std::vector<Triangle> tris;
    for (int i = 0; i < 7; ++i) {
      const double x = double(i) * 4.0;
      tris.push_back(Triangle{{x, 0, 0}, {x + 1, 0, 0}, {x, 1, 0}, uint32_t(1000 + i)});
    }
    std::vector<Triangle> tris_copy = tris;
    mesh = scene.add_mesh(std::move(tris));

    std::vector<Edge> edges{Edge{3u, mesh, identity(), 0xFFFFFFFFu, -1}};
    root = scene.add_group(std::move(edges));
    tris_copy.clear();  // destroy the caller's own copy too
  }

  // The scene must own its data: every original triangle is still hittable
  // and reports its original input index even though the BVH reordered them.
  for (int i = 0; i < 7; ++i) {
    const double x = double(i) * 4.0 + 0.25;
    const Ray<double> ray({x, 0.25, -5.0}, {0.0, 0.0, 1.0});
    Hit hit;
    QueryStats stats;
    TASK_CHECK(scene.closest(root, ray, 0.0, 100.0, 0xFFFFFFFFu, hit, &stats));
    TASK_CHECK(hit.primitive_id == static_cast<uint32_t>(i));
    TASK_CHECK(hit.material == static_cast<uint32_t>(1000 + i));
    TASK_CHECK(stats.triangle_tests >= 1);
  }

  // Destroying a caller edge vector after add_group must also be safe.
  NodeId other;
  {
    std::vector<Edge> tmp{Edge{11u, mesh, translation(0, 10, 0), 0xFFFFFFFFu, -1}};
    other = scene.add_group(std::move(tmp));
  }
  const Ray<double> ray({0.25, 10.25, -5.0}, {0.0, 0.0, 1.0});
  Hit hit;
  TASK_CHECK(scene.closest(other, ray, 0.0, 100.0, 0xFFFFFFFFu, hit));
  TASK_CHECK(hit.path.size() == 1 && hit.path[0] == 11u);

  // Double-sided intersection: a ray from behind still hits.
  const Ray<double> back({0.25, 0.25, 5.0}, {0.0, 0.0, -1.0});
  const Ray<double> front({0.25, 0.25, -5.0}, {0.0, 0.0, 1.0});
  TASK_CHECK(scene.occluded(root, back, 0.0, 100.0, 0xFFFFFFFFu));
  TASK_CHECK(scene.occluded(root, front, 0.0, 100.0, 0xFFFFFFFFu));

  // A non-unit direction keeps t in the original world parameterization.
  const Ray<double> long_dir({0.25, 0.25, -10.0}, {0.0, 0.0, 2.0});
  Hit long_hit;
  TASK_CHECK(scene.closest(root, long_dir, 0.0, 1000.0, 0xFFFFFFFFu, long_hit));
  TASK_CHECK(nearly(long_hit.t, 5.0));
  TASK_CHECK(nearly_vec(long_hit.position, Vec3{0.25, 0.25, 0.0}));
  return 0;
}

// =====================================================================
// updates group: replace_edges, refit propagation, cycles, diagnostics
// =====================================================================

int updates_refit_moves_ancestor_bounds() {
  Scene scene;
  const NodeId mesh = scene.add_mesh(
      std::vector<Triangle>{Triangle{{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, 5}});

  const NodeId bottom =
      scene.add_group(std::vector<Edge>{Edge{7u, mesh, identity(), 0xFFFFFFFFu, -1}});
  const NodeId left =
      scene.add_group(std::vector<Edge>{Edge{10u, bottom, identity(), 0xFFFFFFFFu, 20}});
  const NodeId right =
      scene.add_group(std::vector<Edge>{Edge{20u, bottom, identity(), 0xFFFFFFFFu, 30}});
  const NodeId root = scene.add_group(
      std::vector<Edge>{Edge{100u, left, identity(), 0xFFFFFFFFu, -1},
                        Edge{200u, right, identity(), 0xFFFFFFFFu, -1}});

  const GroupBvhDiagnostic root_before = scene.diagnose_group_bvh(root);
  const GroupBvhDiagnostic left_before = scene.diagnose_group_bvh(left);

  const Ray<double> old_ray({0.0, 0.0, -5.0}, {0.0, 0.0, 1.0});
  Hit before_hit;
  TASK_CHECK(scene.closest(root, old_ray, 0.0, 1e9, 0xFFFFFFFFu, before_hit));

  // Move the bottom mesh far away in +x. Both ancestor bounds (and the
  // diamond's two parent chains) must follow via refit.
  TASK_CHECK(scene.replace_edges(
      bottom, std::vector<Edge>{Edge{7u, mesh, translation(1000, 0, 0), 0xFFFFFFFFu, -1}}));

  TASK_CHECK(!scene.occluded(root, old_ray, 0.0, 1e9, 0xFFFFFFFFu));
  const Ray<double> new_ray({1000.0, 0.0, -5.0}, {0.0, 0.0, 1.0});
  Hit new_hit;
  TASK_CHECK(scene.closest(root, new_ray, 0.0, 1e9, 0xFFFFFFFFu, new_hit));
  TASK_CHECK(new_hit.path == before_hit.path);
  TASK_CHECK(nearly(new_hit.t, before_hit.t));

  // Both shared ancestors were refitted, not rebuilt: identical connections.
  const GroupBvhDiagnostic root_after = scene.diagnose_group_bvh(root);
  const GroupBvhDiagnostic left_after = scene.diagnose_group_bvh(left);
  TASK_CHECK(root_after.nodes.size() == root_before.nodes.size());
  TASK_CHECK(root_after.leaf_edge_ids == root_before.leaf_edge_ids);
  TASK_CHECK(left_after.leaf_edge_ids == left_before.leaf_edge_ids);
  for (size_t i = 0; i < root_before.nodes.size(); ++i) {
    TASK_CHECK(root_after.nodes[i].right_offset == root_before.nodes[i].right_offset);
    TASK_CHECK(root_after.nodes[i].start == root_before.nodes[i].start);
    TASK_CHECK(root_after.nodes[i].primitive_count == root_before.nodes[i].primitive_count);
    TASK_CHECK(root_after.nodes[i].leaf == root_before.nodes[i].leaf);
  }

  // The refitted root box must now span the moved geometry (large +x).
  double root_max_x = -1e30;
  for (const auto& n : root_after.nodes) {
    root_max_x = std::max(root_max_x, n.bbox.max.x);
  }
  TASK_CHECK(root_max_x >= 999.0);
  return 0;
}

int updates_rejections_leave_state_unchanged() {
  Scene scene;
  const NodeId mesh = scene.add_mesh(
      std::vector<Triangle>{Triangle{{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, 5}});
  const NodeId a = scene.add_group(std::vector<Edge>{Edge{1u, mesh, identity(), 0xFFFFFFFFu, -1}});
  const NodeId b = scene.add_group(std::vector<Edge>{Edge{2u, a, identity(), 0xFFFFFFFFu, -1}});
  const NodeId root = scene.add_group(std::vector<Edge>{Edge{3u, b, identity(), 0xFFFFFFFFu, -1}});

  const StorageStats before = scene.storage_stats();
  const GroupBvhDiagnostic diag_before = scene.diagnose_group_bvh(root);
  const Ray<double> ray({0, 0, -5}, {0, 0, 1});
  Hit good_hit;
  TASK_CHECK(scene.closest(root, ray, 0.0, 1e9, 0xFFFFFFFFu, good_hit));

  // Nonexistent node and mesh handles are rejected.
  TASK_CHECK(!scene.replace_edges(9999u, std::vector<Edge>{}));
  TASK_CHECK(!scene.replace_edges(mesh, std::vector<Edge>{}));

  // Indirect cycle: making 'a' point at root creates root -> b -> a -> root.
  TASK_CHECK(!scene.replace_edges(a, std::vector<Edge>{Edge{1u, root, identity(), 0xFFFFFFFFu, -1}}));
  // Direct self cycle.
  TASK_CHECK(!scene.replace_edges(a, std::vector<Edge>{Edge{1u, a, identity(), 0xFFFFFFFFu, -1}}));
  // Unknown child, duplicate ids, singular transform, out-of-range override.
  TASK_CHECK(!scene.replace_edges(a, std::vector<Edge>{Edge{1u, 9999u, identity(), 0xFFFFFFFFu, -1}}));
  TASK_CHECK(!scene.replace_edges(
      a, std::vector<Edge>{Edge{1u, mesh, identity(), 0xFFFFFFFFu, -1},
                           Edge{1u, mesh, translation(1, 0, 0), 0xFFFFFFFFu, -1}}));
  Mat singular{};
  TASK_CHECK(!scene.replace_edges(a, std::vector<Edge>{Edge{1u, mesh, singular, 0xFFFFFFFFu, -1}}));
  TASK_CHECK(!scene.replace_edges(
      a, std::vector<Edge>{Edge{1u, mesh, identity(), 0xFFFFFFFFu, int64_t(-2)}}));

  // Everything is unchanged after every rejection.
  const StorageStats after = scene.storage_stats();
  TASK_CHECK(after.mesh_count == before.mesh_count);
  TASK_CHECK(after.group_count == before.group_count);
  TASK_CHECK(after.stored_edges == before.stored_edges);
  TASK_CHECK(after.stored_triangles == before.stored_triangles);
  const GroupBvhDiagnostic diag_after = scene.diagnose_group_bvh(root);
  TASK_CHECK(diag_after.leaf_edge_ids == diag_before.leaf_edge_ids);
  TASK_CHECK(diag_after.nodes.size() == diag_before.nodes.size());
  Hit still_hit;
  TASK_CHECK(scene.closest(root, ray, 0.0, 1e9, 0xFFFFFFFFu, still_hit));
  TASK_CHECK(still_hit.path == good_hit.path);
  TASK_CHECK(nearly(still_hit.t, good_hit.t));
  return 0;
}

int updates_empty_refill_and_deleted_dependencies() {
  Scene scene;
  const NodeId mesh = scene.add_mesh(
      std::vector<Triangle>{Triangle{{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, 9}});
  const NodeId leaf =
      scene.add_group(std::vector<Edge>{Edge{4u, mesh, identity(), 0xFFFFFFFFu, -1}});
  const NodeId parent =
      scene.add_group(std::vector<Edge>{Edge{5u, leaf, identity(), 0xFFFFFFFFu, -1}});

  const Ray<double> ray({0, 0, -5}, {0, 0, 1});
  TASK_CHECK(scene.occluded(parent, ray, 0.0, 1e9, 0xFFFFFFFFu));

  // Empty the leaf group; its parent must refit to an empty box and miss.
  TASK_CHECK(scene.replace_edges(leaf, std::vector<Edge>{}));
  TASK_CHECK(!scene.occluded(parent, ray, 0.0, 1e9, 0xFFFFFFFFu));
  TASK_CHECK(scene.storage_stats().stored_edges == 1);

  // Refill with a different edge; no stale reverse dependency remains.
  const NodeId mesh2 = scene.add_mesh(
      std::vector<Triangle>{Triangle{{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, 11}});
  TASK_CHECK(scene.replace_edges(
      leaf, std::vector<Edge>{Edge{6u, mesh2, translation(0, 0, 0), 0xFFFFFFFFu, -1}}));
  Hit hit;
  TASK_CHECK(scene.closest(parent, ray, 0.0, 1e9, 0xFFFFFFFFu, hit));
  TASK_CHECK(hit.material == 11u);
  TASK_CHECK(hit.path == std::vector<uint32_t>({5u, 6u}));

  // Roots may themselves be emptied and refilled.
  TASK_CHECK(scene.replace_edges(parent, std::vector<Edge>{}));
  TASK_CHECK(!scene.occluded(parent, ray, 0.0, 1e9, 0xFFFFFFFFu));
  TASK_CHECK(scene.replace_edges(
      parent, std::vector<Edge>{Edge{8u, leaf, identity(), 0xFFFFFFFFu, -1}}));
  TASK_CHECK(scene.occluded(parent, ray, 0.0, 1e9, 0xFFFFFFFFu));

  // Deleting the edge that used to point at mesh means re-pointing at mesh
  // must not resurrect any old parent relationship.
  TASK_CHECK(scene.replace_edges(leaf, std::vector<Edge>{Edge{6u, mesh2, identity(), 0xFFFFFFFFu, -1}}));
  const GroupBvhDiagnostic d = scene.diagnose_group_bvh(leaf);
  TASK_CHECK(d.leaf_edge_ids == std::vector<uint32_t>({6u}));
  return 0;
}

// =====================================================================
// render group: P3 PPM image compared pixel-by-pixel with the reference
// =====================================================================

// Orthographic camera looking straight down -z. Each pixel ray starts at
// (x, y, eye_z) and travels in the (0, 0, -1) direction, so every point of
// the scene's xy extent is guaranteed to be covered by some pixel.
struct Camera {
  double xmin, xmax, ymin, ymax, eye_z;
};

void material_color(uint32_t material, uint8_t rgb[3]) {
  // A small fixed palette indexed by material id.
  static const uint8_t palette[][3] = {
      {220, 60, 60}, {60, 200, 90}, {70, 110, 230}, {230, 200, 70}, {180, 90, 210}};
  const uint8_t* c = palette[material % 5];
  rgb[0] = c[0];
  rgb[1] = c[1];
  rgb[2] = c[2];
}

bool render_ppm(const std::string& path, const Scene& scene, const std::vector<RefNode>& ref,
                NodeId root, const Camera& cam, int width, int height, const Vec3& light,
                int* geometry_pixels, int* shadow_pixels) {
  *geometry_pixels = 0;
  *shadow_pixels = 0;
  const RefWorld world = make_ref_world(ref, root);
  std::ostringstream sg_img;
  std::ostringstream ref_img;
  sg_img << "P3\n" << width << " " << height << "\n255\n";
  ref_img << "P3\n" << width << " " << height << "\n255\n";

  for (int j = height - 1; j >= 0; --j) {
    for (int i = 0; i < width; ++i) {
      const double u = double(i) / double(width - 1);
      const double v = double(j) / double(height - 1);
      const double px = cam.xmin + (cam.xmax - cam.xmin) * u;
      const double py = cam.ymin + (cam.ymax - cam.ymin) * v;
      const Ray<double> ray(Vec3{px, py, cam.eye_z}, Vec3{0.0, 0.0, -1.0});

      uint8_t sg_rgb[3] = {30, 30, 30};
      uint8_t ref_rgb[3] = {30, 30, 30};

      Hit sg_hit;
      if (scene.closest(root, ray, 0.0, 1000.0, 0xFFFFFFFFu, sg_hit)) {
        ++(*geometry_pixels);
        material_color(sg_hit.material, sg_rgb);
        // Finite shadow ray toward the light, capped at a finite distance.
        const Vec3 to_light = light - sg_hit.position;
        const double max_t = FastBVH::length(to_light);
        const Ray<double> shadow(sg_hit.position + sg_hit.normal * 1e-4,
                                 to_light * (1.0 / max_t));
        if (scene.occluded(root, shadow, 0.0, max_t, 0xFFFFFFFFu)) {
          ++(*shadow_pixels);
          sg_rgb[0] = uint8_t(sg_rgb[0] * 0.35);
          sg_rgb[1] = uint8_t(sg_rgb[1] * 0.35);
          sg_rgb[2] = uint8_t(sg_rgb[2] * 0.35);
        }
      }

      const RefHit rh = ref_closest(world, ray, 0.0, 1000.0);
      if (rh.hit) {
        material_color(rh.tri->material, ref_rgb);
        const Vec3 hit_pos = ray.o + ray.d * rh.t;
        const Vec3 to_light = light - hit_pos;
        const double max_t = FastBVH::length(to_light);
        const Ray<double> shadow(hit_pos + ref_normal(*rh.tri) * 1e-4,
                                 to_light * (1.0 / max_t));
        if (ref_occluded(world, shadow, 0.0, max_t)) {
          ref_rgb[0] = uint8_t(ref_rgb[0] * 0.35);
          ref_rgb[1] = uint8_t(ref_rgb[1] * 0.35);
          ref_rgb[2] = uint8_t(ref_rgb[2] * 0.35);
        }
      }

      sg_img << int(sg_rgb[0]) << " " << int(sg_rgb[1]) << " " << int(sg_rgb[2]) << "\n";
      ref_img << int(ref_rgb[0]) << " " << int(ref_rgb[1]) << " " << int(ref_rgb[2]) << "\n";
    }
  }

  const std::string sg_text = sg_img.str();
  const std::string ref_text = ref_img.str();
  if (sg_text != ref_text) {
    return false;
  }
  std::ofstream out(path);
  if (!out) return false;
  out << sg_text;
  return static_cast<bool>(out);
}

int render_matches_reference() {
  Scene scene;
  std::vector<RefNode> ref;

  // A full-frame floor made of two rectangles' worth of triangles.
  std::vector<Triangle> floor_tris{
      Triangle{{-12, -8, 0}, {12, -8, 0}, {12, 8, 0}, 0},
      Triangle{{-12, -8, 0}, {12, 8, 0}, {-12, 8, 0}, 0}};
  const NodeId floor_mesh = scene.add_mesh(floor_tris);
  ref.push_back(RefNode{true, floor_tris, {}});

  // A single occluder triangle reused by two transformed instances.
  std::vector<Triangle> occluder_tris{
      Triangle{{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}, 1}};
  const NodeId occluder_mesh = scene.add_mesh(occluder_tris);
  ref.push_back(RefNode{true, occluder_tris, {}});

  // Shared inner group: non-uniformly scale the occluder.
  const Mat scaled = diag(1.5, 1.5, 1.0);
  const NodeId occl_inner = scene.add_group(
      std::vector<Edge>{Edge{1u, occluder_mesh, scaled, 0xFFFFFFFFu, -1}});
  ref.push_back(RefNode{false, {}, {Edge{1u, occluder_mesh, scaled, 0xFFFFFFFFu, -1}}});

  // First instance, lifted to z = 3, keeps the mesh material.
  const NodeId inst_a = scene.add_group(
      std::vector<Edge>{Edge{2u, occl_inner, translation(-4, 0, 3), 0xFFFFFFFFu, -1}});
  ref.push_back(RefNode{false, {}, {Edge{2u, occl_inner, translation(-4, 0, 3), 0xFFFFFFFFu, -1}}});

  // Second instance: mirrored and translated to the other side, with an
  // override so its color differs.
  const Mat b_xfm = Impl::compose(translation(5, 1, 3), mirror_x());
  const NodeId inst_b = scene.add_group(
      std::vector<Edge>{Edge{3u, occl_inner, b_xfm, 0xFFFFFFFFu, 3}});
  ref.push_back(RefNode{false, {},
                        {Edge{3u, occl_inner, b_xfm, 0xFFFFFFFFu, 3}}});

  const NodeId root = scene.add_group(std::vector<Edge>{
      Edge{10u, floor_mesh, identity(), 0xFFFFFFFFu, -1},
      Edge{11u, inst_a, identity(), 0xFFFFFFFFu, -1},
      Edge{12u, inst_b, identity(), 0xFFFFFFFFu, -1}});
  ref.push_back(RefNode{false,
                        {},
                        {Edge{10u, floor_mesh, identity(), 0xFFFFFFFFu, -1},
                         Edge{11u, inst_a, identity(), 0xFFFFFFFFu, -1},
                         Edge{12u, inst_b, identity(), 0xFFFFFFFFu, -1}}});

  // Orthographic top-down view covering the whole scene.
  Camera cam{-10.0, 10.0, -6.0, 6.0, 20.0};
  const Vec3 light{3.0, -2.0, 12.0};

  const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                           "/fastbvh_scene_graph_render.ppm";
  int geometry_pixels = 0;
  int shadow_pixels = 0;
  TASK_CHECK(render_ppm(path, scene, ref, root, cam, 32, 24, light,
                        &geometry_pixels, &shadow_pixels));
  // The image must actually contain geometry and cast shadows.
  TASK_CHECK(geometry_pixels > 100);
  TASK_CHECK(shadow_pixels > 5);

  // The written file must be a valid P3 PPM with the expected pixel count.
  std::ifstream in(path);
  std::string magic;
  int w = 0, h = 0, maxv = 0;
  in >> magic >> w >> h >> maxv;
  TASK_CHECK(magic == "P3");
  TASK_CHECK(w == 32 && h == 24 && maxv == 255);
  int values = 0;
  int tmp = 0;
  while (in >> tmp) ++values;
  TASK_CHECK(values == 32 * 24 * 3);
  return 0;
}

int render_occlusion_shadow_vs_reference() {
  Scene scene;
  std::vector<RefNode> ref;

  // An occluder above a receiver so shadow rays are visibly blocked.
  std::vector<Triangle> receiver_tris{
      Triangle{{-3, -3, 0}, {3, -3, 0}, {0, 3, 0}, 4}};
  const NodeId receiver = scene.add_mesh(receiver_tris);
  ref.push_back(RefNode{true, receiver_tris, {}});

  std::vector<Triangle> occluder_tris{
      Triangle{{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}, 1}};
  const NodeId occluder = scene.add_mesh(occluder_tris);
  ref.push_back(RefNode{true, occluder_tris, {}});

  const NodeId oc_group = scene.add_group(
      std::vector<Edge>{Edge{1u, occluder, translation(0, 0, 4), 0xFFFFFFFFu, -1}});
  ref.push_back(RefNode{false, {}, {Edge{1u, occluder, translation(0, 0, 4), 0xFFFFFFFFu, -1}}});

  const NodeId root = scene.add_group(std::vector<Edge>{
      Edge{2u, receiver, identity(), 0xFFFFFFFFu, -1},
      Edge{3u, oc_group, identity(), 0xFFFFFFFFu, -1}});
  ref.push_back(RefNode{false,
                        {},
                        {Edge{2u, receiver, identity(), 0xFFFFFFFFu, -1},
                         Edge{3u, oc_group, identity(), 0xFFFFFFFFu, -1}}});

  const RefWorld world = make_ref_world(ref, root);

  // Rays aimed under the occluder produce shadowed receiver hits.
  int shadowed_sg = 0;
  int shadowed_ref = 0;
  int total_hits = 0;
  for (int i = -4; i <= 4; ++i) {
    const Ray<double> ray({double(i) * 0.4, 0.0, -3.0}, {0.0, 0.0, 1.0});
    Hit hit;
    if (scene.closest(root, ray, 0.0, 100.0, 0xFFFFFFFFu, hit)) {
      ++total_hits;
      const Vec3 light{0.0, 0.0, 8.0};
      const Vec3 to_light = light - hit.position;
      const double max_t = FastBVH::length(to_light);
      const Ray<double> shadow(hit.position + hit.normal * 1e-4, to_light * (1.0 / max_t));
      if (scene.occluded(root, shadow, 0.0, max_t, 0xFFFFFFFFu)) ++shadowed_sg;
      if (ref_occluded(world, shadow, 0.0, max_t)) ++shadowed_ref;
    }
  }
  TASK_CHECK(total_hits > 0);
  TASK_CHECK(shadowed_sg == shadowed_ref);
  TASK_CHECK(shadowed_sg > 0);
  return 0;
}

}  // namespace

// Defined at global scope with external linkage so task_support/driver.c
// (compiled as C++ here) can resolve them through check.h.
const struct task_case task_cases[] = {
    {"dag", "storage_no_expansion", dag_storage_no_expansion},
    {"dag", "validation_and_empty_nodes", dag_validation_and_empty_nodes},
    {"dag", "diamond_sharing_and_4096_counters", dag_diamond_sharing_and_4096_counters},

    {"paths", "match_reference_basic", paths_match_reference_basic},
    {"paths", "nested_transforms_vs_reference", paths_nested_transforms_vs_reference},
    {"paths", "masks_and_overrides", paths_masks_and_overrides},
    {"paths", "container_ownership_and_bvh_reorder", paths_container_ownership_and_bvh_reorder},

    {"updates", "refit_moves_ancestor_bounds", updates_refit_moves_ancestor_bounds},
    {"updates", "rejections_leave_state_unchanged", updates_rejections_leave_state_unchanged},
    {"updates", "empty_refill_and_deleted_dependencies", updates_empty_refill_and_deleted_dependencies},

    {"render", "matches_reference", render_matches_reference},
    {"render", "occlusion_shadow_vs_reference", render_occlusion_shadow_vs_reference},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
