//! \file editable_mesh_cases.cpp
//! \brief Regression cases for FastBVH::EditableMesh, registered through
//! task_support/check.h (groups: geometry, updates, acceleration, render).

#include <FastBVH.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <vector>

#include "task_support/check.h"

namespace {

using FastBVH::EditableMesh;
using FastBVH::Ray;
using Vec3 = FastBVH::Vector3<double>;
using FaceIndices = EditableMesh::FaceIndices;
using Contact = EditableMesh::Contact;
using ContactKind = EditableMesh::ContactKind;

constexpr ContactKind P = ContactKind::Point;
constexpr ContactKind S = ContactKind::Segment;
constexpr ContactKind A = ContactKind::Area;

//! Checks one contact record against the mesh.
bool hasContact(const EditableMesh& m, uint32_t a, uint32_t b, ContactKind k) {
  for (const auto& c : m.contacts()) {
    if (c.face_a == a && c.face_b == b && c.kind == k) return true;
  }
  return false;
}

//! The contact set must equal the expected set exactly.
bool contactsExactly(const EditableMesh& m, std::initializer_list<Contact> want) {
  if (m.contacts().size() != want.size()) return false;
  for (const auto& w : want) {
    if (!hasContact(m, w.face_a, w.face_b, w.kind)) return false;
  }
  return true;
}

//! A contact list (added/removed) must equal the expected records in order.
bool contactListEq(const std::vector<Contact>& got, std::initializer_list<Contact> want) {
  if (got.size() != want.size()) return false;
  std::size_t i = 0;
  for (const auto& w : want) {
    if (!(got[i] == w)) return false;
    ++i;
  }
  return true;
}

//! \brief Independent ray reference: brute-force Möller–Trumbore scan over
//! all faces (no BVH). Ties resolve to the smallest face id.
bool bruteClosest(const std::vector<Vec3>& vs, const std::vector<FaceIndices>& fs, const Ray<double>& ray,
                  uint32_t& face, double& t, double& u, double& v) {
  bool hit = false;
  double best = std::numeric_limits<double>::infinity();
  uint32_t bf = 0;
  double bu = 0.0, bv = 0.0;
  for (uint32_t i = 0; i < fs.size(); ++i) {
    const Vec3& a = vs[fs[i][0]];
    const Vec3& b = vs[fs[i][1]];
    const Vec3& c = vs[fs[i][2]];
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 p = FastBVH::cross(ray.d, e2);
    const double det = FastBVH::dot(e1, p);
    if (det == 0.0) continue;
    const double inv = 1.0 / det;
    const Vec3 tv = ray.o - a;
    const double uu = FastBVH::dot(tv, p) * inv;
    if (uu < 0.0 || uu > 1.0) continue;
    const Vec3 q = FastBVH::cross(tv, e1);
    const double vv = FastBVH::dot(ray.d, q) * inv;
    if (vv < 0.0 || uu + vv > 1.0) continue;
    const double tt = FastBVH::dot(e2, q) * inv;
    if (tt < 0.0) continue;
    if (tt < best) {
      best = tt;
      bf = i;
      bu = uu;
      bv = vv;
      hit = true;
    }
  }
  if (hit) {
    face = bf;
    t = best;
    u = bu;
    v = bv;
  }
  return hit;
}

//! Compares one ray between the production closest() and the reference.
bool rayMatches(const EditableMesh& m, const Ray<double>& ray) {
  EditableMesh::RayHit hit;
  const bool got = m.closest(ray, hit);
  uint32_t rface = 0;
  double rt = 0.0, ru = 0.0, rv = 0.0;
  const bool want = bruteClosest(m.vertices(), m.faces(), ray, rface, rt, ru, rv);
  if (got != want) return false;
  if (!got) return true;
  const double tol = 1e-9 * (1.0 + std::fabs(rt));
  return hit.face == rface && std::fabs(hit.t - rt) <= tol && std::fabs(hit.u - ru) <= 1e-9 &&
         std::fabs(hit.v - rv) <= 1e-9;
}

// ---------------------------------------------------------------------------
// geometry group
// ---------------------------------------------------------------------------

//! Transversal crossing of two triangles -> segment contact.
int geo_transversal_crossing() {
  const std::vector<Vec3> v = {{0, 0, 0},       {2, 0, 0},       {0, 2, 0},
                               {0.5, 0.2, -1},   {0.5, 0.8, 1},   {0.5, 1.4, -1},
                               {10, 10, 10},     {12, 10, 10},    {10, 12, 10}};
  const std::vector<FaceIndices> f = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}};
  EditableMesh m(v, f);
  // Face 1 crosses the z=0 plane of face 0 in a segment (y in [0.5, 1.1]
  // at x=0.5, computed independently by hand). Face 2 is far away.
  TASK_CHECK(contactsExactly(m, {Contact{0, 1, S}}));
  TASK_CHECK(m.contacts()[0].face_a == 0 && m.contacts()[0].face_b == 1);
  TASK_CHECK(m.contacts()[0].kind == ContactKind::Segment);
  return 0;
}

//! Coplanar containment -> area contact; parallel offset -> nothing.
int geo_coplanar_containment() {
  const std::vector<Vec3> v = {{0, 0, 0}, {4, 0, 0}, {0, 4, 0},   // face 0
                               {1, 1, 0}, {2, 1, 0}, {1, 2, 0},   // face 1, inside face 0
                               {0, 0, 1}, {4, 0, 1}, {0, 4, 1}};  // face 2, parallel copy
  const std::vector<FaceIndices> f = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}};
  EditableMesh m(v, f);
  TASK_CHECK(contactsExactly(m, {Contact{0, 1, A}}));
  TASK_CHECK(hasContact(m, 0, 1, ContactKind::Area));
  TASK_CHECK(!hasContact(m, 0, 2, ContactKind::Area));
  return 0;
}

//! Legal edge sharing (closed tetrahedron + coplanar opposite sides).
int geo_legal_shared_edge() {
  const std::vector<Vec3> v = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}, {0, -2, 0}};
  const std::vector<FaceIndices> f = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}, {0, 1, 4}};
  EditableMesh m(v, f);
  // Every pair either shares an edge of the tetrahedron (intersection is
  // exactly the shared edge) or is coplanar on opposite sides of the
  // shared edge (face 4 against face 0): no contact may be reported.
  TASK_CHECK(m.contacts().empty());
  TASK_CHECK(contactsExactly(m, {}));
  return 0;
}

//! Two faces sharing an edge but folded back coplanar -> area contact.
int geo_shared_edge_foldback() {
  const std::vector<Vec3> v = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {1, 0.5, 0},
                               {10, 10, 10}, {12, 10, 10}, {10, 12, 10}};
  const std::vector<FaceIndices> f = {{0, 1, 2}, {0, 1, 3}, {4, 5, 6}};
  EditableMesh m(v, f);
  // Faces 0 and 1 share edge (0,1) but face 1 lies on the same side and
  // covers part of face 0: the overlap exceeds the shared edge.
  TASK_CHECK(contactsExactly(m, {Contact{0, 1, A}}));
  return 0;
}

//! Point contacts at coincident positions with different vertex indices.
int geo_point_contact_distinct_indices() {
  const std::vector<Vec3> v = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0},      // face 0
                               {2, 0, 0}, {4, 0, 0}, {2, 2, 0},      // face 1, coplanar point touch
                               {2, 0, 0}, {3, 1, 1}, {3, -1, 2}};    // face 2, slanted point touch
  const std::vector<FaceIndices> f = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}};
  EditableMesh m(v, f);
  // All three pairs touch exactly at the point (2,0,0), which has
  // different vertex indices in every face: not a topological adjacency.
  TASK_CHECK(contactsExactly(m, {Contact{0, 1, P}, Contact{0, 2, P}, Contact{1, 2, P}}));
  return 0;
}

//! Shared-vertex exemption, and segment overlap from a shared vertex.
int geo_shared_vertex_exemption() {
  const std::vector<Vec3> v = {{0, 0, 0},                  // 0: shared origin vertex
                               {2, 0, 0}, {0, 2, 0},       // face 0
                               {-2, 0, 0}, {0, -2, 0},     // face 1, coplanar, touches at origin
                               {-2, 0, 1}, {0, -2, 1},     // face 2, slanted, touches at origin
                               {1, 0, 0}, {0, -1, 1}};     // face 3, overlaps face 0 in a segment
  const std::vector<FaceIndices> f = {{0, 1, 2}, {0, 3, 4}, {0, 5, 6}, {0, 7, 8}};
  EditableMesh m(v, f);
  // Faces 1 and 2 only touch face 0 at the shared vertex 0: exempt.
  // Face 3 overlaps face 0 along the segment (0,0,0)-(1,0,0), which is
  // not contained in the shared vertex: reported.
  TASK_CHECK(contactsExactly(m, {Contact{0, 3, S}}));
  return 0;
}

//! The transversal scenario next to the +/-1e6 coordinate bound.
int geo_large_coordinates() {
  const Vec3 s{999998.0, 999997.0, 999980.0};
  const std::vector<Vec3> v = {{s.x + 0.0, s.y + 0.0, s.z + 0.0},
                               {s.x + 2.0, s.y + 0.0, s.z + 0.0},  // reaches exactly 1e6
                               {s.x + 0.0, s.y + 2.0, s.z + 0.0},
                               {s.x + 0.5, s.y + 0.2, s.z - 1.0},
                               {s.x + 0.5, s.y + 0.8, s.z + 1.0},
                               {s.x + 0.5, s.y + 1.4, s.z - 1.0}};
  const std::vector<FaceIndices> f = {{0, 1, 2}, {3, 4, 5}};
  EditableMesh m(v, f);
  TASK_CHECK(contactsExactly(m, {Contact{0, 1, S}}));
  // Ray queries work at this scale as well.
  TASK_CHECK(rayMatches(m, Ray<double>(Vec3{s.x + 0.5, s.y + 0.7, s.z + 10.0}, Vec3{0.0, 0.0, -1.0})));
  TASK_CHECK(rayMatches(m, Ray<double>(Vec3{s.x + 0.9, s.y + 0.3, s.z + 5.0}, Vec3{0.0, 0.0, -7.0})));
  return 0;
}

// ---------------------------------------------------------------------------
// updates group
// ---------------------------------------------------------------------------

//! Two consecutive batches: contact addition, removal and kind change.
int upd_batch_moves_diff() {
  const std::vector<Vec3> v = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0},          // face 0
                               {3, 0, 0}, {5, 0, 0}, {3, 2, 0},          // face 1
                               {0.5, 0.5, -1}, {0.5, 0.5, 1}, {0.5, 1.5, 0}};  // face 2
  const std::vector<FaceIndices> f = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}};
  EditableMesh m(v, f);
  TASK_CHECK(contactsExactly(m, {Contact{0, 2, S}}));

  // Batch 1: slide vertex 3 so face 1 overlaps face 0 coplanar -> new area contact.
  auto r1 = m.update_vertices({EditableMesh::VertexMove{3, Vec3{1.0, 0.5, 0.0}}});
  TASK_CHECK(r1.accepted);
  TASK_CHECK(r1.error == nullptr);
  TASK_CHECK(contactListEq(r1.added, {Contact{0, 1, A}}));
  TASK_CHECK(contactListEq(r1.removed, {}));
  TASK_CHECK(contactsExactly(m, {Contact{0, 1, A}, Contact{0, 2, S}}));

  // Batch 2: restore vertex 3 (removes the area contact) and flatten face 2
  // into the plane of face 0 (segment -> area: kind change is remove + add).
  auto r2 = m.update_vertices({EditableMesh::VertexMove{3, Vec3{3.0, 0.0, 0.0}},
                               EditableMesh::VertexMove{6, Vec3{0.5, 0.5, 0.0}},
                               EditableMesh::VertexMove{7, Vec3{1.5, 0.5, 0.0}},
                               EditableMesh::VertexMove{8, Vec3{0.5, 1.5, 0.0}}});
  TASK_CHECK(r2.accepted);
  TASK_CHECK(contactListEq(r2.removed, {Contact{0, 1, A}, Contact{0, 2, S}}));
  TASK_CHECK(contactListEq(r2.added, {Contact{0, 2, A}}));
  TASK_CHECK(contactsExactly(m, {Contact{0, 2, A}}));
  return 0;
}

//! Rejected batches leave everything untouched; later batches still apply.
int upd_rejection_then_continue() {
  const std::vector<Vec3> v = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {5, 0, 0}, {7, 0, 0}, {5, 2, 0}};
  const std::vector<FaceIndices> f = {{0, 1, 2}, {3, 4, 5}};
  EditableMesh m(v, f);
  TASK_CHECK(m.contacts().empty());

  const Ray<double> probe(Vec3{1.0, 0.5, 10.0}, Vec3{0.0, 0.0, -1.0});
  EditableMesh::RayHit before;
  TASK_CHECK(m.closest(probe, before));
  TASK_CHECK(before.face == 0);

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const EditableMesh::VertexMove bad[] = {
      EditableMesh::VertexMove{9, Vec3{1, 1, 0}},        // vertex does not exist
      EditableMesh::VertexMove{0, Vec3{nan, 0, 0}},      // non-finite
      EditableMesh::VertexMove{0, Vec3{inf, 0, 0}},      // non-finite
      EditableMesh::VertexMove{0, Vec3{2e6, 0, 0}},      // outside the coordinate bound
      EditableMesh::VertexMove{2, Vec3{1, 0, 0}},        // degenerates face 0 (collinear)
      EditableMesh::VertexMove{0, Vec3{2, 0, 0}},        // degenerates face 0 (zero edge)
  };
  for (const auto& mv : bad) {
    auto r = m.update_vertices({mv});
    TASK_CHECK(!r.accepted);
    TASK_CHECK(r.error != nullptr);
    TASK_CHECK(r.error_vertex == mv.vertex);
    TASK_CHECK(r.added.empty() && r.removed.empty());
    // Vertices, contacts and ray results stay untouched.
    TASK_CHECK(m.contacts().empty());
    TASK_CHECK(m.vertices()[0].x == 0.0 && m.vertices()[0].y == 0.0 && m.vertices()[0].z == 0.0);
    TASK_CHECK(m.vertices()[2].x == 0.0 && m.vertices()[2].y == 2.0);
    EditableMesh::RayHit hit;
    TASK_CHECK(m.closest(probe, hit));
    TASK_CHECK(hit.face == before.face && hit.t == before.t);
  }

  // Duplicate vertex in one batch is also rejected.
  auto rd = m.update_vertices({EditableMesh::VertexMove{0, Vec3{1, 1, 0}},
                               EditableMesh::VertexMove{0, Vec3{1, 2, 0}}});
  TASK_CHECK(!rd.accepted);
  TASK_CHECK(rd.error_vertex == 0);
  TASK_CHECK(m.contacts().empty());

  // An empty batch is a valid no-op.
  auto re = m.update_vertices({});
  TASK_CHECK(re.accepted);
  TASK_CHECK(re.added.empty() && re.removed.empty());

  // A valid batch still applies afterwards.
  auto r = m.update_vertices({EditableMesh::VertexMove{3, Vec3{1.0, 0.5, 0.0}}});
  TASK_CHECK(r.accepted);
  TASK_CHECK(contactListEq(r.added, {Contact{0, 1, A}}));
  TASK_CHECK(contactsExactly(m, {Contact{0, 1, A}}));

  // A position exactly at the coordinate bound is accepted...
  auto rb = m.update_vertices({EditableMesh::VertexMove{4, Vec3{1e6, 0.0, 0.0}}});
  TASK_CHECK(rb.accepted);
  TASK_CHECK(contactsExactly(m, {Contact{0, 1, A}}));
  // ...while one past the bound is rejected, leaving the state untouched.
  auto rb2 = m.update_vertices({EditableMesh::VertexMove{4, Vec3{1e6 + 1.0, 0.0, 0.0}}});
  TASK_CHECK(!rb2.accepted);
  TASK_CHECK(m.vertices()[4].x == 1e6);
  TASK_CHECK(contactsExactly(m, {Contact{0, 1, A}}));
  return 0;
}

//! Move construction and move assignment keep the tree attached.
int upd_move_semantics() {
  const std::vector<Vec3> v = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0.5, 0.2, -1}, {0.5, 0.8, 1}, {0.5, 1.4, -1}};
  const std::vector<FaceIndices> f = {{0, 1, 2}, {3, 4, 5}};
  EditableMesh first(v, f);
  TASK_CHECK(contactsExactly(first, {Contact{0, 1, S}}));

  EditableMesh second(std::move(first));
  TASK_CHECK(contactsExactly(second, {Contact{0, 1, S}}));
  TASK_CHECK(rayMatches(second, Ray<double>(Vec3{0.5, 0.7, 3.0}, Vec3{0.2, 0.0, -1.0})));

  // Updates keep working after the move.
  auto r = second.update_vertices({EditableMesh::VertexMove{4, Vec3{0.5, 0.8, 2.0}}});
  TASK_CHECK(r.accepted);
  TASK_CHECK(contactsExactly(second, {Contact{0, 1, S}}));

  EditableMesh third(std::vector<Vec3>{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, std::vector<FaceIndices>{{0, 1, 2}});
  third = std::move(second);
  TASK_CHECK(contactsExactly(third, {Contact{0, 1, S}}));
  TASK_CHECK(rayMatches(third, Ray<double>(Vec3{0.5, 0.7, 3.0}, Vec3{0.2, 0.0, -1.0})));
  return 0;
}

//! Face identity and ray distances survive the build-time reordering.
int upd_identity_and_rays_after_reorder() {
  constexpr uint32_t N = 100;
  std::vector<Vec3> v;
  std::vector<FaceIndices> f;
  v.reserve(3 * N);
  f.reserve(N);
  for (uint32_t i = 0; i < N; ++i) {
    // Pseudo-random x slot (a permutation), so the builder reorders slots.
    const double x = 3.0 * (double)((i * 37) % N);
    const double z = 0.1 * (double)(i % 7);
    v.push_back(Vec3{x, 0, 0});
    v.push_back(Vec3{x + 2, 0, 0});
    v.push_back(Vec3{x, 2, z});
    f.push_back(FaceIndices{3 * i, 3 * i + 1, 3 * i + 2});
  }
  EditableMesh m(v, f);
  TASK_CHECK(m.bvh().getNodes().size() > 1);

  // The builder must have reordered at least one slot.
  bool reordered = false;
  const auto prims = m.bvh().getPrimitives();
  for (uint32_t i = 0; i < prims.size(); ++i) {
    if (prims[i].face != i) reordered = true;
  }
  TASK_CHECK(reordered);

  // Every face is hit by its own ray with the input face id and distance.
  for (uint32_t i = 0; i < N; ++i) {
    const double x = 3.0 * (double)((i * 37) % N);
    const Ray<double> ray(Vec3{x + 0.66, 0.66, 10.0}, Vec3{0.0, 0.0, -1.0});
    TASK_CHECK(rayMatches(m, ray));
    EditableMesh::RayHit hit;
    TASK_CHECK(m.closest(ray, hit));
    TASK_CHECK(hit.face == i);
    const double want_t = 10.0 - 0.33 * 0.1 * (double)(i % 7);
    TASK_CHECK(std::fabs(hit.t - want_t) < 1e-9);
  }

  // Diagnostics: parent links, leaf slots and box containment.
  const auto diag = m.node_diagnostics();
  const auto nodes = m.bvh().getNodes();
  TASK_CHECK(diag.size() == nodes.size());
  TASK_CHECK(diag[0].parent == -1);
  std::vector<uint32_t> slot_faces;
  for (uint32_t i = 0; i < diag.size(); ++i) {
    if (!diag[i].leaf) {
      const uint32_t l = i + 1;
      const uint32_t r = i + nodes[i].right_offset;
      TASK_CHECK(diag[l].parent == (int32_t)i && diag[r].parent == (int32_t)i);
      TASK_CHECK(diag[l].depth == diag[i].depth + 1 && diag[r].depth == diag[i].depth + 1);
      // Box containment of the children.
      TASK_CHECK(diag[i].bbox.min.x <= diag[l].bbox.min.x + 1e-12 && diag[i].bbox.max.x >= diag[l].bbox.max.x - 1e-12);
      TASK_CHECK(diag[i].bbox.min.x <= diag[r].bbox.min.x + 1e-12 && diag[i].bbox.max.x >= diag[r].bbox.max.x - 1e-12);
    } else {
      const auto slots = m.leaf_slots(i);
      TASK_CHECK(slots.size() == diag[i].count);
      for (const uint32_t face : slots) {
        TASK_CHECK(m.face_leaf(face) == i);
        slot_faces.push_back(face);
      }
    }
  }
  // The leaf slots partition all faces exactly once.
  std::sort(slot_faces.begin(), slot_faces.end());
  TASK_CHECK(slot_faces.size() == N);
  for (uint32_t i = 0; i < N; ++i) TASK_CHECK(slot_faces[i] == i);
  return 0;
}

// ---------------------------------------------------------------------------
// acceleration group
// ---------------------------------------------------------------------------

//! The 4096 spaced triangles workload: bounded incremental work.
int acc_spaced_workload_stats() {
  constexpr uint32_t N = 4096;
  std::vector<Vec3> v;
  std::vector<FaceIndices> f;
  v.reserve(3 * N);
  f.reserve(N);
  for (uint32_t i = 0; i < N; ++i) {
    const double x = 8.0 * (double)i;
    v.push_back(Vec3{x, 0, 0});
    v.push_back(Vec3{x + 2, 0, 0});
    v.push_back(Vec3{x, 2, 0});
    f.push_back(FaceIndices{3 * i, 3 * i + 1, 3 * i + 2});
  }
  EditableMesh m(v, f);
  TASK_CHECK(m.contacts().empty());

  // Move one vertex of the middle face, staying inside its own interval.
  const uint32_t face = 2048;
  const uint32_t vtx = 3 * face + 1;
  auto r = m.update_vertices({EditableMesh::VertexMove{vtx, Vec3{8.0 * face + 1.0, 1.0, 0.0}}});
  TASK_CHECK(r.accepted);
  TASK_CHECK(r.added.empty() && r.removed.empty());
  TASK_CHECK(m.contacts().empty());

  // Bounded incremental work, as documented.
  TASK_CHECK(r.stats.triangle_pair_tests <= 64);
  TASK_CHECK(r.stats.leaves_recomputed + r.stats.internal_nodes_recomputed <= 64);
  TASK_CHECK(r.stats.leaves_recomputed == 1);
  TASK_CHECK(r.stats.internal_nodes_recomputed >= 1);
  // Real node pruning: far fewer box checks than tree nodes.
  TASK_CHECK(r.stats.node_pair_checks > 0);
  TASK_CHECK(r.stats.node_pair_checks < 512);
  TASK_CHECK(m.bvh().getNodes().size() > 1000);

  // The refit tree answers rays with the new geometry.
  const Ray<double> ray_hit(Vec3{8.0 * face + 0.4, 0.5, 10.0}, Vec3{0.0, 0.0, -1.0});
  EditableMesh::RayHit hit;
  TASK_CHECK(m.closest(ray_hit, hit));
  TASK_CHECK(hit.face == face);
  TASK_CHECK(std::fabs(hit.t - 10.0) < 1e-12);
  // This ray hits the old triangle but misses the moved one.
  const Ray<double> ray_gone(Vec3{8.0 * face + 1.5, 0.1, 10.0}, Vec3{0.0, 0.0, -1.0});
  EditableMesh::RayHit gone;
  TASK_CHECK(!m.closest(ray_gone, gone));
  return 0;
}

//! Incremental contact sets must match a full rebuild after every batch.
int acc_incremental_vs_rebuild() {
  std::vector<Vec3> v;
  std::vector<FaceIndices> f;
  for (uint32_t j = 0; j < 10; ++j) {
    for (uint32_t i = 0; i < 10; ++i) {
      v.push_back(Vec3{(double)i, (double)j, 0.0});
    }
  }
  auto vid = [](uint32_t i, uint32_t j) { return i + 10 * j; };
  for (uint32_t j = 0; j < 9; ++j) {
    for (uint32_t i = 0; i < 9; ++i) {
      f.push_back(FaceIndices{vid(i, j), vid(i + 1, j), vid(i + 1, j + 1)});
      f.push_back(FaceIndices{vid(i, j), vid(i + 1, j + 1), vid(i, j + 1)});
    }
  }
  // An overlapping coplanar pair far away from the grid.
  v.push_back(Vec3{20, 0, 0});
  v.push_back(Vec3{22, 0, 0});
  v.push_back(Vec3{20, 2, 0});
  v.push_back(Vec3{21, 0.5, 0});
  v.push_back(Vec3{23, 0.5, 0});
  v.push_back(Vec3{21, 2.5, 0});
  f.push_back(FaceIndices{100, 101, 102});
  f.push_back(FaceIndices{103, 104, 105});

  EditableMesh m(v, f);
  TASK_CHECK(contactsExactly(m, {Contact{162, 163, A}}));

  auto checkAgainstRebuild = [&](const EditableMesh& mesh) {
    EditableMesh ref(mesh.vertices(), mesh.faces());
    if (ref.contacts().size() != mesh.contacts().size()) return false;
    for (std::size_t i = 0; i < ref.contacts().size(); ++i) {
      if (!(ref.contacts()[i] == mesh.contacts()[i])) return false;
    }
    return true;
  };

  // Batch 1: in-plane move inside the ring; the distant contact must be
  // preserved without re-testing the whole mesh.
  auto r1 = m.update_vertices({EditableMesh::VertexMove{55, Vec3{5.3, 5.2, 0.0}}});
  TASK_CHECK(r1.accepted);
  TASK_CHECK(r1.added.empty() && r1.removed.empty());
  TASK_CHECK(contactsExactly(m, {Contact{162, 163, A}}));
  TASK_CHECK(r1.stats.triangle_pair_tests < 2000);
  TASK_CHECK(checkAgainstRebuild(m));

  // Batch 2: lift a vertex; still no contacts, still incremental.
  auto r2 = m.update_vertices({EditableMesh::VertexMove{22, Vec3{2.0, 2.0, 1.0}}});
  TASK_CHECK(r2.accepted);
  TASK_CHECK(r2.added.empty() && r2.removed.empty());
  TASK_CHECK(contactsExactly(m, {Contact{162, 163, A}}));
  TASK_CHECK(checkAgainstRebuild(m));

  // Batch 3: separate the overlapping pair; the contact disappears.
  auto r3 = m.update_vertices({EditableMesh::VertexMove{103, Vec3{30.0, 0.5, 0.0}}});
  TASK_CHECK(r3.accepted);
  TASK_CHECK(contactListEq(r3.removed, {Contact{162, 163, A}}));
  TASK_CHECK(r3.added.empty());
  TASK_CHECK(m.contacts().empty());
  TASK_CHECK(checkAgainstRebuild(m));
  return 0;
}

//! After an update, every node box must equal the reference refit.
int acc_refit_correctness() {
  std::vector<Vec3> v;
  std::vector<FaceIndices> f;
  for (uint32_t j = 0; j < 6; ++j) {
    for (uint32_t i = 0; i < 6; ++i) {
      v.push_back(Vec3{(double)i, (double)j, 0.0});
    }
  }
  auto vid = [](uint32_t i, uint32_t j) { return i + 6 * j; };
  for (uint32_t j = 0; j < 5; ++j) {
    for (uint32_t i = 0; i < 5; ++i) {
      f.push_back(FaceIndices{vid(i, j), vid(i + 1, j), vid(i + 1, j + 1)});
      f.push_back(FaceIndices{vid(i, j), vid(i + 1, j + 1), vid(i, j + 1)});
    }
  }
  EditableMesh m(v, f);
  const auto before = m.node_diagnostics();

  // Move the central vertex (3,3) = index 21.
  auto r = m.update_vertices({EditableMesh::VertexMove{21, Vec3{3.2, 3.1, 0.4}}});
  TASK_CHECK(r.accepted);
  TASK_CHECK(r.added.empty() && r.removed.empty());

  // Expected number of refit leaves: distinct leaves of the incident faces.
  std::set<uint32_t> dirty_faces;
  for (uint32_t fi = 0; fi < f.size(); ++fi) {
    if (f[fi][0] == 21 || f[fi][1] == 21 || f[fi][2] == 21) dirty_faces.insert(fi);
  }
  std::set<uint32_t> want_leaves;
  for (const uint32_t fi : dirty_faces) want_leaves.insert(m.face_leaf(fi));
  TASK_CHECK(r.stats.leaves_recomputed == want_leaves.size());

  // Expected number of refit internal nodes: distinct ancestors of those leaves.
  const auto diag = m.node_diagnostics();
  std::set<uint32_t> want_ancestors;
  for (const uint32_t leaf : want_leaves) {
    for (int32_t p = diag[leaf].parent; p >= 0; p = diag[p].parent) want_ancestors.insert((uint32_t)p);
  }
  TASK_CHECK(r.stats.internal_nodes_recomputed == want_ancestors.size());

  // Every node box must equal the reference: union of slot faces for
  // leaves, union of children for internal nodes.
  const auto nodes = m.bvh().getNodes();
  for (uint32_t i = 0; i < diag.size(); ++i) {
    FastBVH::BBox<double> want;
    if (diag[i].leaf) {
      const auto slots = m.leaf_slots(i);
      TASK_CHECK(!slots.empty());
      const auto& t0 = m.faces()[slots[0]];
      want = FastBVH::BBox<double>(FastBVH::min(m.vertices()[t0[0]], FastBVH::min(m.vertices()[t0[1]], m.vertices()[t0[2]])),
                                   FastBVH::max(m.vertices()[t0[0]], FastBVH::max(m.vertices()[t0[1]], m.vertices()[t0[2]])));
      for (std::size_t s = 1; s < slots.size(); ++s) {
        const auto& t = m.faces()[slots[s]];
        want.expandToInclude(FastBVH::min(m.vertices()[t[0]], FastBVH::min(m.vertices()[t[1]], m.vertices()[t[2]])));
        want.expandToInclude(FastBVH::max(m.vertices()[t[0]], FastBVH::max(m.vertices()[t[1]], m.vertices()[t[2]])));
      }
    } else {
      want = diag[i + 1].bbox;
      want.expandToInclude(diag[i + nodes[i].right_offset].bbox);
    }
    TASK_CHECK(diag[i].bbox.min.x == want.min.x && diag[i].bbox.min.y == want.min.y &&
               diag[i].bbox.min.z == want.min.z);
    TASK_CHECK(diag[i].bbox.max.x == want.max.x && diag[i].bbox.max.y == want.max.y &&
               diag[i].bbox.max.z == want.max.z);
  }

  // A leaf far away from the edit keeps its exact old box.
  const uint32_t far_leaf = m.face_leaf(0);
  TASK_CHECK(diag[far_leaf].bbox.min.x == before[far_leaf].bbox.min.x &&
             diag[far_leaf].bbox.max.x == before[far_leaf].bbox.max.x &&
             diag[far_leaf].bbox.min.z == before[far_leaf].bbox.min.z);
  return 0;
}

// ---------------------------------------------------------------------------
// render group
// ---------------------------------------------------------------------------

//! closest() must match the independent reference on a ray battery.
int rnd_closest_matches_reference() {
  const std::vector<Vec3> v = {{0, 0, 0}, {4, 0, 0}, {0, 4, 0},          // face 0
                               {1, 1, 1}, {3, 1, 1}, {1, 3, 1},          // face 1, above face 0
                               {10, 0, 0}, {14, 0, 0}, {10, 4, 0},       // face 2
                               {20, 0, -2}, {20, 0, 2}, {20, 4, 0},      // face 3, vertical
                               {0, 10, 0}, {4, 10, 0}, {0, 14, 0.5}};    // face 4, tilted
  const std::vector<FaceIndices> f = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}, {9, 10, 11}, {12, 13, 14}};
  EditableMesh m(v, f);

  const Ray<double> rays[] = {
      Ray<double>(Vec3{1.5, 0.5, 10.0}, Vec3{0.0, 0.0, -1.0}),     // hits face 0
      Ray<double>(Vec3{1.5, 1.5, 10.0}, Vec3{0.0, 0.0, -1.0}),     // hits face 1 (above face 0)
      Ray<double>(Vec3{1.5, 1.5, 10.0}, Vec3{0.0, 0.0, -2.5}),     // non-unit direction
      Ray<double>(Vec3{2.5, 1.2, 10.0}, Vec3{0.0, 0.0, -1.0}),     // hits face 1
      Ray<double>(Vec3{12.0, 1.0, 10.0}, Vec3{0.0, 0.0, -1.0}),    // hits face 2
      Ray<double>(Vec3{10.0, 1.0, 0.0}, Vec3{1.0, 0.05, 0.02}),    // hits vertical face 3
      Ray<double>(Vec3{1.0, 12.0, 10.0}, Vec3{0.0, 0.0, -1.0}),    // hits tilted face 4
      Ray<double>(Vec3{0.5, 0.5, 10.0}, Vec3{0.1, 0.05, -1.0}),    // tilted non-unit
      Ray<double>(Vec3{100.0, 100.0, 100.0}, Vec3{0.3, 0.4, 0.5}), // miss, pointing away
      Ray<double>(Vec3{1.5, 1.5, 10.0}, Vec3{0.0, 0.0, 1.0}),      // miss, pointing up
      Ray<double>(Vec3{20.0, 1.0, 0.0}, Vec3{-1.0, 0.01, 0.03}),   // miss, away from face 3
  };
  for (const auto& ray : rays) {
    TASK_CHECK(rayMatches(m, ray));
  }

  // Spot-check the exact hit data of one ray, including barycentrics.
  EditableMesh::RayHit hit;
  TASK_CHECK(m.closest(Ray<double>(Vec3{1.5, 1.5, 10.0}, Vec3{0.0, 0.0, -2.5}), hit));
  TASK_CHECK(hit.face == 1);
  TASK_CHECK(std::fabs(hit.t - 3.6) < 1e-9);
  TASK_CHECK(std::fabs(hit.u - 0.25) < 1e-9 && std::fabs(hit.v - 0.25) < 1e-9);
  return 0;
}

//! Per-pixel render comparison before and after a production update.
int rnd_pixels_after_update() {
  std::vector<Vec3> v;
  std::vector<FaceIndices> f;
  for (uint32_t j = 0; j < 3; ++j) {
    for (uint32_t i = 0; i < 3; ++i) {
      v.push_back(Vec3{2.0 * (double)i, 2.0 * (double)j, 0.0});
    }
  }
  auto vid = [](uint32_t i, uint32_t j) { return i + 3 * j; };
  for (uint32_t j = 0; j < 2; ++j) {
    for (uint32_t i = 0; i < 2; ++i) {
      f.push_back(FaceIndices{vid(i, j), vid(i + 1, j), vid(i + 1, j + 1)});
      f.push_back(FaceIndices{vid(i, j), vid(i + 1, j + 1), vid(i, j + 1)});
    }
  }
  EditableMesh m(v, f);

  // 16x16 orthographic pixels with a non-unit direction; the offsets keep
  // every pixel away from triangle edges (no exact ties).
  auto pixelRay = [](uint32_t px, uint32_t py) {
    const double x = -1.75 + 0.5 * (double)px;
    const double y = -1.7 + 0.5 * (double)py;
    return Ray<double>(Vec3{x, y, 10.0}, Vec3{0.0, 0.0, -3.0});
  };

  std::vector<double> t_before(256, -1.0);
  for (uint32_t py = 0; py < 16; ++py) {
    for (uint32_t px = 0; px < 16; ++px) {
      TASK_CHECK(rayMatches(m, pixelRay(px, py)));
      EditableMesh::RayHit hit;
      if (m.closest(pixelRay(px, py), hit)) t_before[px + 16 * py] = hit.t;
    }
  }

  // Production update: raise the center vertex into a pyramid.
  auto r = m.update_vertices({EditableMesh::VertexMove{4, Vec3{2.0, 2.0, 2.0}}});
  TASK_CHECK(r.accepted);
  TASK_CHECK(m.contacts().empty());

  uint32_t changed = 0;
  uint32_t unchanged = 0;
  for (uint32_t py = 0; py < 16; ++py) {
    for (uint32_t px = 0; px < 16; ++px) {
      TASK_CHECK(rayMatches(m, pixelRay(px, py)));
      EditableMesh::RayHit hit;
      const double t = m.closest(pixelRay(px, py), hit) ? hit.t : -1.0;
      const double was = t_before[px + 16 * py];
      if ((t < 0.0) != (was < 0.0) || (t >= 0.0 && was >= 0.0 && std::fabs(t - was) > 1e-9)) {
        ++changed;
      } else {
        ++unchanged;
      }
    }
  }
  // The pyramid changed the pixels near the center; the rays missing the
  // mesh are unaffected.
  TASK_CHECK(changed > 0);
  TASK_CHECK(unchanged > 0);
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"geometry", "transversal_crossing", geo_transversal_crossing},
    {"geometry", "coplanar_containment", geo_coplanar_containment},
    {"geometry", "legal_shared_edge", geo_legal_shared_edge},
    {"geometry", "shared_edge_foldback", geo_shared_edge_foldback},
    {"geometry", "point_contact_distinct_indices", geo_point_contact_distinct_indices},
    {"geometry", "shared_vertex_exemption", geo_shared_vertex_exemption},
    {"geometry", "large_coordinates", geo_large_coordinates},
    {"updates", "batch_moves_diff", upd_batch_moves_diff},
    {"updates", "rejection_then_continue", upd_rejection_then_continue},
    {"updates", "identity_and_rays_after_reorder", upd_identity_and_rays_after_reorder},
    {"updates", "move_semantics", upd_move_semantics},
    {"acceleration", "spaced_workload_stats", acc_spaced_workload_stats},
    {"acceleration", "incremental_vs_rebuild", acc_incremental_vs_rebuild},
    {"acceleration", "refit_correctness", acc_refit_correctness},
    {"render", "closest_matches_reference", rnd_closest_matches_reference},
    {"render", "pixels_after_update", rnd_pixels_after_update},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
