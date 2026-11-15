#pragma once

#include <FastBVH/BBox.h>
#include <FastBVH/BVH.h>
#include <FastBVH/BuildStrategy.h>
#include <FastBVH/Iterable.h>
#include <FastBVH/Ray.h>
#include <FastBVH/Vector3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

//! \brief Hierarchical scene graph with shared sub-scenes built on top of
//! the single-level Fast-BVH.
namespace FastBVH {
namespace SceneGraph {

//! A simple type definition for a 3D vector.
using Vec3 = Vector3<double>;

//! The stable handle of a node owned by a Scene.
using NodeId = uint32_t;

//! A single triangle in mesh-local space.
struct Triangle final {
  //! The first vertex.
  Vec3 a;

  //! The second vertex.
  //! The winding order a -> b -> c defines the front face.
  Vec3 b;

  //! The third vertex.
  Vec3 c;

  //! The material index of the triangle.
  uint32_t material;
};

//! An affine edge from a group to one of its children.
//! The child may be a mesh or another group, and the same child may be
//! referenced by any number of parent groups.
struct Edge final {
  //! The unique identifier of this edge inside its owning group.
  //! It is recorded in Hit::path.
  uint32_t id;

  //! The node referenced by this edge. It must exist when the edge
  //! is added or when the owning group is updated.
  NodeId child;

  //! A row-major 3x4 affine transform (three rows of four doubles).
  //! Points of the child are mapped as p_parent = A * p_child + translation.
  std::array<double, 12> transform;

  //! The visibility mask of the edge. The effective mask of a path is
  //! the query mask AND-ed with every edge mask on the path.
  uint32_t mask;

  //! A material override. -1 means "do not override". A non-negative value,
  //! which must fit in uint32_t, replaces the triangle material. The first
  //! non-negative override on a root-to-leaf path wins.
  int64_t material_override;
};

//! The result of a closest-hit query.
struct Hit final {
  //! The edge ids from the root group down to the mesh that was hit.
  std::vector<uint32_t> path;

  //! The id of the mesh containing the hit triangle.
  NodeId mesh = 0;

  //! The index of the triangle in the original add_mesh() input.
  uint32_t primitive_id = 0;

  //! The effective material after applying path overrides.
  uint32_t material = 0;

  //! The ray parameter of the hit, always in the original world space.
  double t = 0.0;

  //! The hit position in world space.
  Vec3 position{0.0, 0.0, 0.0};

  //! The unit-length geometric normal in world space, following the
  //! actual world-space winding order of the triangle.
  Vec3 normal{0.0, 0.0, 1.0};
};

//! Per-query instrumentation counters.
struct QueryStats final {
  //! Number of BVH nodes (group and mesh) actually read during traversal.
  uint64_t node_visits = 0;

  //! Number of candidate edges examined inside group BVH leaves.
  uint64_t edge_tests = 0;

  //! Number of triangle intersection routines actually executed.
  uint64_t triangle_tests = 0;
};

//! Storage counters that count objects the scene actually owns.
//! Shared sub-scenes are counted once, never per instantiation path.
struct StorageStats final {
  //! Number of meshes owned by the scene.
  uint64_t mesh_count = 0;

  //! Number of groups owned by the scene.
  uint64_t group_count = 0;

  //! Total number of edges stored across all groups.
  uint64_t stored_edges = 0;

  //! Total number of triangles stored across all meshes.
  uint64_t stored_triangles = 0;
};

//! A read-only snapshot of one node of a group edge BVH.
struct BvhDiagnosticNode final {
  //! The bounding box in the group's local space.
  BBox<double> bbox;

  //! The index of the first primitive in the BVH primitive array.
  uint32_t start = 0;

  //! The number of primitives held by this node.
  uint32_t primitive_count = 0;

  //! The flattened-tree right offset (zero for leaves).
  uint32_t right_offset = 0;

  //! Whether this node is a leaf.
  bool leaf = false;
};

//! A read-only snapshot of a group edge BVH used by tests to compare
//! node connections and edge-id ordering across updates.
struct GroupBvhDiagnostic final {
  //! The flattened nodes in storage order.
  std::vector<BvhDiagnosticNode> nodes;

  //! The edge ids of every leaf, concatenated in node order.
  std::vector<uint32_t> leaf_edge_ids;
};

namespace Impl {

//! Checks that every component of a vector is finite.
inline bool all_finite(const Vec3& v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

//! Checks that every entry of an affine transform is finite.
inline bool transform_finite(const std::array<double, 12>& m) noexcept {
  for (double x : m) {
    if (!std::isfinite(x)) {
      return false;
    }
  }
  return true;
}

//! Computes the determinant of the 3x3 linear part of a row-major 3x4 matrix.
//! The linear entries live at column indices 0,1,2 of each four-element row.
inline double det3(const std::array<double, 12>& m) noexcept {
  return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
         m[2] * (m[4] * m[9] - m[5] * m[8]);
}

//! Applies an affine transform to a point.
inline Vec3 apply_point(const std::array<double, 12>& m, const Vec3& p) noexcept {
  return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
              m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
              m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

//! Applies the linear part of an affine transform to a direction.
inline Vec3 apply_dir(const std::array<double, 12>& m, const Vec3& d) noexcept {
  return Vec3{m[0] * d.x + m[1] * d.y + m[2] * d.z,
              m[4] * d.x + m[5] * d.y + m[6] * d.z,
              m[8] * d.x + m[9] * d.y + m[10] * d.z};
}

//! Composes two row-major 3x4 affines: result = lhs * rhs.
inline std::array<double, 12> compose(const std::array<double, 12>& lhs,
                                      const std::array<double, 12>& rhs) noexcept {
  std::array<double, 12> out{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      out[r * 4 + c] = lhs[r * 4 + 0] * rhs[0 * 4 + c] + lhs[r * 4 + 1] * rhs[1 * 4 + c] +
                       lhs[r * 4 + 2] * rhs[2 * 4 + c];
    }
    out[r * 4 + 3] = lhs[r * 4 + 0] * rhs[3] + lhs[r * 4 + 1] * rhs[7] +
                     lhs[r * 4 + 2] * rhs[11] + lhs[r * 4 + 3];
  }
  return out;
}

//! Inverts a finite, invertible row-major 3x4 affine transform.
inline std::array<double, 12> invert_affine(const std::array<double, 12>& m) {
  const double a00 = m[0], a01 = m[1], a02 = m[2];
  const double a10 = m[4], a11 = m[5], a12 = m[6];
  const double a20 = m[8], a21 = m[9], a22 = m[10];

  const double i00 = a11 * a22 - a12 * a21;
  const double i01 = a02 * a21 - a01 * a22;
  const double i02 = a01 * a12 - a02 * a11;
  const double i10 = a12 * a20 - a10 * a22;
  const double i11 = a00 * a22 - a02 * a20;
  const double i12 = a02 * a10 - a00 * a12;
  const double i20 = a10 * a21 - a11 * a20;
  const double i21 = a01 * a20 - a00 * a21;
  const double i22 = a00 * a11 - a01 * a10;

  const double inv_det = 1.0 / det3(m);
  std::array<double, 12> inv{};
  inv[0] = i00 * inv_det;
  inv[1] = i01 * inv_det;
  inv[2] = i02 * inv_det;
  inv[4] = i10 * inv_det;
  inv[5] = i11 * inv_det;
  inv[6] = i12 * inv_det;
  inv[8] = i20 * inv_det;
  inv[9] = i21 * inv_det;
  inv[10] = i22 * inv_det;

  // Inverse translation: -invA * b
  inv[3] = -(inv[0] * m[3] + inv[1] * m[7] + inv[2] * m[11]);
  inv[7] = -(inv[4] * m[3] + inv[5] * m[7] + inv[6] * m[11]);
  inv[11] = -(inv[8] * m[3] + inv[9] * m[7] + inv[10] * m[11]);
  return inv;
}

//! Transforms an axis-aligned box by mapping its eight corners.
//! When the child currently owns no geometry, a point box at the
//! translation is returned so every edge still has a finite box.
inline BBox<double> transform_box(const std::array<double, 12>& m, const BBox<double>& b,
                                  bool has_box) noexcept {
  if (!has_box) {
    const Vec3 t{m[3], m[7], m[11]};
    return BBox<double>(t, t);
  }

  BBox<double> out(apply_point(m, b.min));
  const Vec3 corners[7] = {
      Vec3{b.max.x, b.min.y, b.min.z}, Vec3{b.min.x, b.max.y, b.min.z},
      Vec3{b.max.x, b.max.y, b.min.z}, Vec3{b.min.x, b.min.y, b.max.z},
      Vec3{b.max.x, b.min.y, b.max.z}, Vec3{b.min.x, b.max.y, b.max.z},
      Vec3{b.max.x, b.max.y, b.max.z}};
  for (const Vec3& c : corners) {
    out.expandToInclude(apply_point(m, c));
  }
  return out;
}

//! Tests a box against a ray and a closed parameter window.
inline bool box_in_window(const BBox<double>& box, const Ray<double>& ray, double tmin,
                          double tmax) noexcept {
  double n = 0.0;
  double f = 0.0;
  if (!box.intersect(ray, &n, &f)) {
    return false;
  }
  return f >= tmin && n <= tmax;
}

}  // namespace Impl

//! A hierarchical scene in which groups reference meshes or other groups
//! through affine edges. Child nodes are shared, never duplicated: each
//! group owns only its own local edge BVH and each mesh its own triangle
//! BVH. All query methods are read-only and may run concurrently.
class Scene final {
  //! Per-mesh storage.
  struct MeshNode final {
    std::vector<Triangle> triangles;
    std::vector<uint32_t> primitive_order;
    std::unique_ptr<BVH<double, uint32_t>> bvh;
    BBox<double> local_box;
    bool has_box = false;
  };

  //! Per-group storage.
  struct GroupNode final {
    std::vector<Edge> edges;
    std::vector<uint32_t> edge_order;
    std::unique_ptr<BVH<double, uint32_t>> bvh;
    BBox<double> local_box;
    bool has_box = false;
  };

  //! A node slot. Slots are never erased, so NodeId values stay stable.
  struct NodeEntry final {
    bool is_mesh = false;
    MeshNode mesh;
    GroupNode group;
  };

  std::vector<std::unique_ptr<NodeEntry>> nodes;

  //! Primitive-to-box converter for a mesh triangle BVH.
  struct TriangleBoxConverter final {
    const std::vector<Triangle>* triangles;

    BBox<double> operator()(uint32_t tri_index) const {
      const Triangle& tri = (*triangles)[tri_index];
      BBox<double> box(tri.a);
      box.expandToInclude(tri.b);
      box.expandToInclude(tri.c);
      return box;
    }
  };

  //! Primitive-to-box converter for a group edge BVH.
  struct EdgeBoxConverter final {
    const Scene* scene;
    const std::vector<Edge>* edges;

    BBox<double> operator()(uint32_t edge_index) const {
      return scene->edge_box((*edges)[edge_index]);
    }
  };

  //! Mutable state of one root-to-leaf traversal. It lives on the call
  //! stack only, so shared sub-groups never see path-dependent data.
  struct Frame final {
    Ray<double> ray;
    uint32_t mask = 0;
    int64_t material_override = -1;
    std::vector<uint32_t> path;

    explicit Frame(const Ray<double>& r) : ray(r) {}
  };

  //! Shared closest/any-hit accumulator.
  struct TraceState final {
    double best_t = std::numeric_limits<double>::infinity();
    std::vector<uint32_t> best_path;
    uint32_t best_primitive = 0;
    bool found = false;
    bool occlusion = false;
    QueryStats* stats = nullptr;
  };

  //! Builds an empty BVH.
  static std::unique_ptr<BVH<double, uint32_t>> make_empty_bvh() {
    NodeArray<double> empty_nodes;
    ConstIterable<uint32_t> empty_prims(static_cast<const uint32_t*>(nullptr), 0);
    return std::unique_ptr<BVH<double, uint32_t>>(
        new BVH<double, uint32_t>(std::move(empty_nodes), empty_prims));
  }

  //! Returns the local bounding box of a node.
  void node_local_box(NodeId id, BBox<double>& box, bool& has_box) const noexcept {
    const NodeEntry& entry = *nodes[id];
    if (entry.is_mesh) {
      box = entry.mesh.local_box;
      has_box = entry.mesh.has_box;
    } else {
      box = entry.group.local_box;
      has_box = entry.group.has_box;
    }
  }

  //! Computes the local box of an edge by mapping its child box.
  BBox<double> edge_box(const Edge& edge) const noexcept {
    BBox<double> child_box;
    bool has_box = false;
    node_local_box(edge.child, child_box, has_box);
    return Impl::transform_box(edge.transform, child_box, has_box);
  }

  //! Builds the triangle BVH of a mesh.
  void build_mesh_bvh(MeshNode& mesh) {
    mesh.primitive_order.clear();
    mesh.primitive_order.reserve(mesh.triangles.size());
    for (uint32_t i = 0; i < mesh.triangles.size(); ++i) {
      mesh.primitive_order.push_back(i);
    }

    if (mesh.primitive_order.empty()) {
      mesh.bvh = make_empty_bvh();
      mesh.has_box = false;
      return;
    }

    DefaultBuilder<double> builder;
    mesh.bvh.reset(new BVH<double, uint32_t>(
        builder(mesh.primitive_order, TriangleBoxConverter{&mesh.triangles})));

    mesh.local_box = mesh.bvh->getNodes()[0].bbox;
    mesh.has_box = true;
  }

  //! Builds the local edge BVH of a group from scratch.
  void build_group_bvh(GroupNode& group) {
    group.edge_order.clear();
    group.edge_order.reserve(group.edges.size());
    for (uint32_t i = 0; i < group.edges.size(); ++i) {
      group.edge_order.push_back(i);
    }

    if (group.edge_order.empty()) {
      group.bvh = make_empty_bvh();
      group.has_box = false;
      return;
    }

    DefaultBuilder<double> builder;
    group.bvh.reset(new BVH<double, uint32_t>(
        builder(group.edge_order, EdgeBoxConverter{this, &group.edges})));

    group.local_box = group.bvh->getNodes()[0].bbox;
    group.has_box = true;
  }

  //! Refits an existing group edge BVH in place, preserving topology and
  //! edge ordering. Empty trees are no-ops.
  void refit_group_bvh(GroupNode& group) {
    if (group.edge_order.empty()) {
      group.has_box = false;
      return;
    }

    group.bvh->refit(EdgeBoxConverter{this, &group.edges});
    group.local_box = group.bvh->getNodes()[0].bbox;
    group.has_box = true;
  }

  //! Validates a batch of edges about to be stored in a group.
  //! \param replaced The group whose outgoing edges are being replaced,
  //! or an invalid id when validating a brand new group.
  void validate_edges(const std::vector<Edge>& edges, NodeId replaced) const {
    for (const Edge& edge : edges) {
      if (edge.child >= nodes.size()) {
        throw std::invalid_argument("SceneGraph edge references an unknown child node");
      }
      if (!Impl::transform_finite(edge.transform) || Impl::det3(edge.transform) == 0.0) {
        throw std::invalid_argument("SceneGraph edge transform must be finite and invertible");
      }
      if (edge.material_override < -1 ||
          edge.material_override > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::invalid_argument("SceneGraph edge material override is out of range");
      }
    }

    for (std::size_t i = 0; i < edges.size(); ++i) {
      for (std::size_t j = i + 1; j < edges.size(); ++j) {
        if (edges[i].id == edges[j].id) {
          throw std::invalid_argument("SceneGraph edge ids must be unique within a group");
        }
      }
    }

    // Cycle check: a cycle is formed exactly when the replaced group can be
    // reached again from one of its new children. A brand new group cannot
    // be referenced yet, so adding one can never introduce a cycle.
    if (replaced < nodes.size()) {
      for (const Edge& edge : edges) {
        if (reaches(edge.child, replaced, edges)) {
          throw std::invalid_argument("SceneGraph edges must not introduce a cycle");
        }
      }
    }
  }

  //! DFS over the graph where the outgoing edges of \p replaced are taken
  //! from \p new_edges. Returns true if \p target is reachable from \p start.
  bool reaches(NodeId start, NodeId target, const std::vector<Edge>& new_edges) const {
    std::vector<uint8_t> visited(nodes.size(), 0);
    std::vector<NodeId> stack;
    stack.push_back(start);

    while (!stack.empty()) {
      const NodeId current = stack.back();
      stack.pop_back();
      if (current == target) {
        return true;
      }
      if (visited[current]) {
        continue;
      }
      visited[current] = 1;

      const NodeEntry& entry = *nodes[current];
      if (entry.is_mesh) {
        continue;
      }
      const std::vector<Edge>& outgoing =
          (current == target) ? new_edges : entry.group.edges;
      for (const Edge& edge : outgoing) {
        if (!visited[edge.child]) {
          stack.push_back(edge.child);
        }
      }
    }
    return false;
  }

  //! Walks a BVH in local ray space, clipping every node against the closed
  //! parameter window. The leaf callback receives a primitive index and
  //! returns true to abort the walk early. Only nodes actually read are
  //! reported to the stats counters.
  template <typename LeafFn>
  void walk_bvh(const BVH<double, uint32_t>& bvh, const Ray<double>& ray, double tmin,
                double tmax, QueryStats* stats, LeafFn&& leaf) const {
    const auto node_view = bvh.getNodes();
    const auto prim_view = bvh.getPrimitives();
    if (node_view.size() == 0) {
      return;
    }

    std::vector<uint32_t> stack;
    stack.push_back(0);

    while (!stack.empty()) {
      const uint32_t ni = stack.back();
      stack.pop_back();
      if (stats) {
        ++stats->node_visits;
      }

      const Node<double>& node = node_view[ni];
      if (node.isLeaf()) {
        for (uint32_t o = 0; o < node.primitive_count; ++o) {
          if (leaf(prim_view[node.start + o])) {
            return;
          }
        }
        continue;
      }

      double n0 = 0.0, f0 = 0.0, n1 = 0.0, f1 = 0.0;
      const bool hit_left = node_view[ni + 1].bbox.intersect(ray, &n0, &f0) &&
                            f0 >= tmin && n0 <= tmax;
      const bool hit_right =
          node_view[ni + node.right_offset].bbox.intersect(ray, &n1, &f1) &&
          f1 >= tmin && n1 <= tmax;

      if (hit_left && hit_right) {
        // Farther child first, closer child on top of the stack.
        if (n0 < n1) {
          stack.push_back(ni + node.right_offset);
          stack.push_back(ni + 1);
        } else {
          stack.push_back(ni + 1);
          stack.push_back(ni + node.right_offset);
        }
      } else if (hit_left) {
        stack.push_back(ni + 1);
      } else if (hit_right) {
        stack.push_back(ni + node.right_offset);
      }
    }
  }

  //! Double-sided Moller-Trumbore intersection. The affine parameter t is
  //! shared with the world ray because inverse affine maps preserve it;
  //! the ray direction is never normalized.
  static bool intersect_triangle(const Triangle& tri, const Ray<double>& ray,
                                 double& t) noexcept {
    const Vec3 e1 = tri.b - tri.a;
    const Vec3 e2 = tri.c - tri.a;
    const Vec3 h = cross(ray.d, e2);
    const double det = dot(e1, h);
    if (det == 0.0) {
      return false;
    }

    const double f = 1.0 / det;
    const Vec3 s = ray.o - tri.a;
    const double u = f * dot(s, h);
    if (u < 0.0 || u > 1.0) {
      return false;
    }

    const Vec3 q = cross(s, e1);
    const double v = f * dot(ray.d, q);
    if (v < 0.0 || u + v > 1.0) {
      return false;
    }

    t = f * dot(e2, q);
    return true;
  }

  //! Traverses a mesh node.
  bool trace_mesh(const Frame& frame, const MeshNode& mesh, double tmin, double tmax,
                  TraceState& state) const {
    const Ray<double>& ray = frame.ray;
    // Narrow the window to the current best hit so later nodes and
    // triangles beyond it are pruned without changing correctness.
    const double eff_max = state.occlusion ? tmax : std::min(tmax, state.best_t);
    bool stop = false;

    walk_bvh(
        *mesh.bvh, ray, tmin, eff_max, state.stats, [&](uint32_t tri_index) -> bool {
          if (stop) {
            return true;
          }
          if (state.stats) {
            ++state.stats->triangle_tests;
          }

          double t = 0.0;
          const Triangle& tri = mesh.triangles[tri_index];
          if (!intersect_triangle(tri, ray, t) || t < tmin || t > eff_max) {
            return false;
          }

          if (state.occlusion) {
            state.found = true;
            stop = true;
            return true;
          }

          const bool take =
              !state.found || t < state.best_t ||
              (t == state.best_t &&
               (frame.path < state.best_path ||
                (frame.path == state.best_path && tri_index < state.best_primitive)));
          if (take) {
            state.best_t = t;
            state.best_path = frame.path;
            state.best_primitive = tri_index;
            state.found = true;
          }
          return false;
        });

    return stop;
  }

  //! Traverses a group node, descending through shared children without
  //! writing any path state into them.
  bool trace_group(const Frame& frame, const GroupNode& group, double tmin, double tmax,
                   TraceState& state) const {
    const Ray<double>& ray = frame.ray;
    const double eff_max = state.occlusion ? tmax : std::min(tmax, state.best_t);
    bool stop = false;

    walk_bvh(
        *group.bvh, ray, tmin, eff_max, state.stats, [&](uint32_t edge_index) -> bool {
          if (stop) {
            return true;
          }
          if (state.stats) {
            ++state.stats->edge_tests;
          }

          const Edge& edge = group.edges[edge_index];
          const uint32_t effective_mask = frame.mask & edge.mask;
          if (effective_mask == 0) {
            return false;
          }

          if (!Impl::box_in_window(edge_box(edge), ray, tmin, eff_max)) {
            return false;
          }

          const std::array<double, 12> inv = Impl::invert_affine(edge.transform);
          Frame child_frame = frame;
          child_frame.ray =
              Ray<double>(Impl::apply_point(inv, ray.o), Impl::apply_dir(inv, ray.d));
          if (child_frame.material_override < 0 && edge.material_override >= 0) {
            child_frame.material_override = edge.material_override;
          }
          child_frame.path.push_back(edge.id);

          const NodeEntry& child = *nodes[edge.child];
          if (child.is_mesh) {
            stop = trace_mesh(child_frame, child.mesh, tmin, tmax, state);
          } else {
            stop = trace_group(child_frame, child.group, tmin, tmax, state);
          }
          return stop;
        });

    return stop;
  }

  //! Validates the shared query preconditions.
  static void validate_query(const Ray<double>& ray, double tmin, double tmax) {
    if (!std::isfinite(tmin) || !std::isfinite(tmax) || tmin < 0.0 || tmin > tmax) {
      throw std::invalid_argument("SceneGraph query requires a finite closed window 0 <= tmin <= tmax");
    }
    if (!Impl::all_finite(ray.o) || !Impl::all_finite(ray.d) ||
        (ray.d.x == 0.0 && ray.d.y == 0.0 && ray.d.z == 0.0)) {
      throw std::invalid_argument("SceneGraph ray direction must be finite and non-zero");
    }
  }

 public:
  Scene() = default;
  ~Scene() = default;
  Scene(const Scene&) = delete;
  Scene& operator=(const Scene&) = delete;

  //! Adds an immutable mesh and takes ownership of its triangles.
  //! Empty meshes are legal.
  //! \param triangles The mesh triangles. Finite, non-degenerate triangles
  //! are required; otherwise std::invalid_argument is thrown.
  //! \return The stable id of the new mesh.
  NodeId add_mesh(std::vector<Triangle> triangles) {
    for (const Triangle& tri : triangles) {
      if (!Impl::all_finite(tri.a) || !Impl::all_finite(tri.b) || !Impl::all_finite(tri.c)) {
        throw std::invalid_argument("SceneGraph triangle vertices must be finite");
      }
      const Vec3 normal = cross(tri.b - tri.a, tri.c - tri.a);
      if (dot(normal, normal) == 0.0) {
        throw std::invalid_argument("SceneGraph triangles must not be degenerate");
      }
    }

    auto entry = std::unique_ptr<NodeEntry>(new NodeEntry());
    entry->is_mesh = true;
    entry->mesh.triangles = std::move(triangles);
    build_mesh_bvh(entry->mesh);

    const NodeId id = static_cast<NodeId>(nodes.size());
    nodes.push_back(std::move(entry));
    return id;
  }

  //! Adds a group and takes ownership of its edges. Empty groups are legal.
  //! Every child must already exist, edge ids must be unique within the
  //! batch, and every transform must be finite and invertible.
  //! Invalid input throws std::invalid_argument.
  //! \return The stable id of the new group.
  NodeId add_group(std::vector<Edge> edges) {
    validate_edges(edges, std::numeric_limits<NodeId>::max());

    auto entry = std::unique_ptr<NodeEntry>(new NodeEntry());
    entry->is_mesh = false;
    entry->group.edges = std::move(edges);
    build_group_bvh(entry->group);

    const NodeId id = static_cast<NodeId>(nodes.size());
    nodes.push_back(std::move(entry));
    return id;
  }

  //! Replaces the outgoing edges of a group between frames.
  //! On failure nothing changes: the old edges, bounds, query results and
  //! storage counters stay intact. On success only the replaced group's
  //! edge tree is rebuilt; every shared ancestor keeps its node connections
  //! and edge ordering and is merely refitted in dependency order.
  //! \return True if the replacement was applied, false otherwise.
  bool replace_edges(NodeId group, std::vector<Edge> new_edges) {
    if (group >= nodes.size() || nodes[group]->is_mesh) {
      return false;
    }

    // Validate everything before touching live state. The validation
    // helpers throw for malformed batches; translate that into false.
    try {
      validate_edges(new_edges, group);
    } catch (const std::invalid_argument&) {
      return false;
    }

    GroupNode& target = nodes[group]->group;
    target.edges = std::move(new_edges);
    build_group_bvh(target);

    // Mark every group that can still reach the replaced group through the
    // reverse dependency relation. Deleted edges simply never appear here,
    // so no stale reverse dependency can survive.
    std::vector<uint8_t> is_ancestor(nodes.size(), 0);
    std::vector<NodeId> pending;
    pending.push_back(group);
    is_ancestor[group] = 1;

    while (!pending.empty()) {
      const NodeId current = pending.back();
      pending.pop_back();
      for (NodeId p = 0; p < nodes.size(); ++p) {
        const NodeEntry& entry = *nodes[p];
        if (entry.is_mesh || is_ancestor[p]) {
          continue;
        }
        for (const Edge& edge : entry.group.edges) {
          if (edge.child == current) {
            is_ancestor[p] = 1;
            pending.push_back(p);
            break;
          }
        }
      }
    }

    // Postorder over all groups (three-color iterative DFS) guarantees a
    // child is refreshed before every parent. Only ancestors are refitted;
    // unrelated sub-groups and mesh BLAS are never rebuilt.
    enum DfsState : uint8_t { kUnvisited = 0, kActive = 1, kDone = 2 };
    std::vector<uint8_t> state_mark(nodes.size(), kUnvisited);
    std::vector<NodeId> order;
    std::vector<NodeId> dfs;

    for (NodeId root = 0; root < nodes.size(); ++root) {
      if (state_mark[root] != kUnvisited || nodes[root]->is_mesh) {
        continue;
      }
      dfs.push_back(root);
      while (!dfs.empty()) {
        const NodeId current = dfs.back();
        if (state_mark[current] == kDone) {
          dfs.pop_back();
          continue;
        }
        if (state_mark[current] == kActive) {
          state_mark[current] = kDone;
          order.push_back(current);
          dfs.pop_back();
          continue;
        }
        state_mark[current] = kActive;

        const NodeEntry& entry = *nodes[current];
        for (const Edge& edge : entry.group.edges) {
          const NodeId child = edge.child;
          if (!nodes[child]->is_mesh && state_mark[child] == kUnvisited) {
            dfs.push_back(child);
          }
        }
      }
    }

    for (NodeId id : order) {
      if (id != group && is_ancestor[id]) {
        refit_group_bvh(nodes[id]->group);
      }
    }
    return true;
  }

  //! Runs a closest-hit query.
  //! The distance window is the finite closed interval [tmin, tmax] with
  //! 0 <= tmin <= tmax. The ray direction must be finite and non-zero but
  //! does not need to be normalized. Exact-distance ties are resolved by
  //! the lexicographically smallest (path, primitive_id). Intersections
  //! are double-sided. On a miss, \p out is left untouched.
  //! \return True if anything was hit, false otherwise.
  bool closest(NodeId root, const Ray<double>& ray, double tmin, double tmax, uint32_t mask,
               Hit& out, QueryStats* stats = nullptr) const {
    if (root >= nodes.size()) {
      throw std::invalid_argument("SceneGraph query references an unknown root node");
    }
    validate_query(ray, tmin, tmax);

    QueryStats local_stats;
    TraceState state;
    state.stats = stats ? stats : &local_stats;
    state.stats->node_visits = 0;
    state.stats->edge_tests = 0;
    state.stats->triangle_tests = 0;

    Frame frame(ray);
    frame.mask = mask;
    const NodeEntry& entry = *nodes[root];
    if (entry.is_mesh) {
      trace_mesh(frame, entry.mesh, tmin, tmax, state);
    } else {
      trace_group(frame, entry.group, tmin, tmax, state);
    }

    if (!state.found) {
      return false;
    }

    // Replay the winning path to recover its world transform and the
    // first non-negative material override.
    std::array<double, 12> world{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
    int64_t override_material = -1;
    NodeId current = root;
    for (uint32_t edge_id : state.best_path) {
      const GroupNode& group = nodes[current]->group;
      const Edge* edge = nullptr;
      for (const Edge& candidate : group.edges) {
        if (candidate.id == edge_id) {
          edge = &candidate;
          break;
        }
      }
      world = Impl::compose(world, edge->transform);
      if (override_material < 0 && edge->material_override >= 0) {
        override_material = edge->material_override;
      }
      current = edge->child;
    }

    const NodeId mesh_id = current;
    const MeshNode& mesh = nodes[mesh_id]->mesh;
    const Triangle& tri = mesh.triangles[state.best_primitive];

    out.path = state.best_path;
    out.mesh = mesh_id;
    out.primitive_id = state.best_primitive;
    out.material = override_material >= 0 ? static_cast<uint32_t>(override_material) : tri.material;
    out.t = state.best_t;
    out.position = ray.o + ray.d * state.best_t;

    // The normal follows the actual world-space winding: transform all
    // three vertices and take the cross product. This automatically
    // includes the orientation flip of a negative-determinant (mirroring)
    // transform, which a bare inverse-transpose normal matrix would miss.
    const Vec3 wa = Impl::apply_point(world, tri.a);
    const Vec3 wb = Impl::apply_point(world, tri.b);
    const Vec3 wc = Impl::apply_point(world, tri.c);
    out.normal = normalize(cross(wb - wa, wc - wa));
    return true;
  }

  //! Runs an any-hit occlusion query over the same closed window.
  //! \return True as soon as any hit is found, false otherwise.
  bool occluded(NodeId root, const Ray<double>& ray, double tmin, double tmax, uint32_t mask,
                QueryStats* stats = nullptr) const {
    if (root >= nodes.size()) {
      throw std::invalid_argument("SceneGraph query references an unknown root node");
    }
    validate_query(ray, tmin, tmax);

    QueryStats local_stats;
    TraceState state;
    state.occlusion = true;
    state.stats = stats ? stats : &local_stats;
    state.stats->node_visits = 0;
    state.stats->edge_tests = 0;
    state.stats->triangle_tests = 0;

    Frame frame(ray);
    frame.mask = mask;
    const NodeEntry& entry = *nodes[root];
    if (entry.is_mesh) {
      trace_mesh(frame, entry.mesh, tmin, tmax, state);
    } else {
      trace_group(frame, entry.group, tmin, tmax, state);
    }
    return state.found;
  }

  //! Captures a read-only snapshot of a group's edge BVH for diagnostics.
  //! Node connections and leaf edge-id ordering can be compared across
  //! replace_edges() calls; bounding boxes themselves change during refits.
  GroupBvhDiagnostic diagnose_group_bvh(NodeId group) const {
    if (group >= nodes.size() || nodes[group]->is_mesh) {
      throw std::invalid_argument("SceneGraph diagnostic requires an existing group node");
    }

    const GroupNode& g = nodes[group]->group;
    GroupBvhDiagnostic diag;
    const auto node_view = g.bvh->getNodes();
    const auto prim_view = g.bvh->getPrimitives();
    diag.nodes.reserve(node_view.size());

    for (std::size_t i = 0; i < node_view.size(); ++i) {
      const Node<double>& node = node_view[i];
      diag.nodes.push_back(BvhDiagnosticNode{node.bbox, node.start, node.primitive_count,
                                             node.right_offset, node.isLeaf()});
      if (node.isLeaf()) {
        for (uint32_t o = 0; o < node.primitive_count; ++o) {
          diag.leaf_edge_ids.push_back(g.edges[prim_view[node.start + o]].id);
        }
      }
    }
    return diag;
  }

  //! Returns storage counters for the objects actually owned by the scene.
  //! Shared sub-scenes are counted once, never once per instantiation.
  StorageStats storage_stats() const {
    StorageStats stats;
    for (const auto& entry : nodes) {
      if (entry->is_mesh) {
        ++stats.mesh_count;
        stats.stored_triangles += entry->mesh.triangles.size();
      } else {
        ++stats.group_count;
        stats.stored_edges += entry->group.edges.size();
      }
    }
    return stats;
  }
};

}  // namespace SceneGraph
}  // namespace FastBVH
