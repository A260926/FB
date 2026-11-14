//
// Task tests for analytic rectangular-light visibility, BVH refit and the
// incremental visibility cache.
//
// Registered through task_support/check.h as three groups (projection,
// union, render); this file defines no main().
//
// Geometric error convention: analytic fractions are compared against
// hand-derived values with an absolute tolerance of 1e-9 (the documented
// query contract is 1e-7). Cache-update results are compared against an
// independent uncached query on the same geometry, also at 1e-9.
//
// Standard scene used by most cases:
//   receiver R = (0, 0, 0)
//   light      = rectangle on z = 10, origin (-2,-2,10), edges (4,0,0)
//                and (0,4,0)
//   occluders  = double-sided quads on z = 5
// A point x on z = 5 projects to q = 2x on z = 10, so
//   s = (2*x + 2) / 4 = 0.5*x + 0.5.
// Full-y strips therefore block a directly computable s-width.
//
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "analytic_visibility.h"
#include "nanort.h"
#include "task_support/check.h"

namespace {

const double kTol = 1e-9;

area_visibility::Rectangle StandardLight() {
  return area_visibility::Rectangle(
      area_visibility::Vec3(-2.0, -2.0, 10.0),
      area_visibility::Vec3(4.0, 0.0, 0.0),
      area_visibility::Vec3(0.0, 4.0, 0.0));
}

area_visibility::Vec3 OriginReceiver() {
  return area_visibility::Vec3(0.0, 0.0, 0.0);
}

void BuildBVH(const std::vector<double> &vertices,
              const std::vector<unsigned int> &faces,
              nanort::BVHAccel<double> *bvh) {
  nanort::TriangleMesh<double> mesh(
      vertices.empty() ? 0 : &vertices[0],
      faces.empty() ? 0 : &faces[0], 3 * sizeof(double));
  nanort::TriangleSAHPred<double> pred(
      vertices.empty() ? 0 : &vertices[0],
      faces.empty() ? 0 : &faces[0], 3 * sizeof(double));
  bvh->Build(static_cast<unsigned int>(faces.size() / 3), mesh, pred);
}

// Append a z=5 quad x in [xa,xb], y in [ya,yb]. Reversed winding when
// reverse is true. Returns the two new face ids.
void AddQuad(std::vector<double> *vertices,
             std::vector<unsigned int> *faces, double xa, double xb,
             double ya, double yb, double z, bool reverse = false) {
  const unsigned base =
      static_cast<unsigned int>(vertices->size() / 3);
  const double c[4][3] = {{xa, ya, z}, {xb, ya, z}, {xb, yb, z},
                          {xa, yb, z}};
  for (int i = 0; i < 4; ++i) {
    vertices->push_back(c[i][0]);
    vertices->push_back(c[i][1]);
    vertices->push_back(c[i][2]);
  }
  if (!reverse) {
    faces->push_back(base + 0);
    faces->push_back(base + 1);
    faces->push_back(base + 2);
    faces->push_back(base + 0);
    faces->push_back(base + 2);
    faces->push_back(base + 3);
  } else {
    faces->push_back(base + 0);
    faces->push_back(base + 2);
    faces->push_back(base + 1);
    faces->push_back(base + 0);
    faces->push_back(base + 3);
    faces->push_back(base + 2);
  }
}

// Hand-derived visible fraction for a full-y z=5 strip x in [xa, xb].
double StripVisible(double xa, double xb) {
  auto clamp01 = [](double v) {
    return std::max(0.0, std::min(1.0, v));
  };
  const double s_hi = clamp01(0.5 * xb + 0.5);
  const double s_lo = clamp01(0.5 * xa + 0.5);
  return 1.0 - (s_hi - s_lo);
}

// Primitive accessor that counts every bounding-box read, so refit cost
// can be asserted without scanning the mesh.
struct CountingMesh {
  const nanort::TriangleMesh<double> *inner;
  std::size_t *calls;
  void BoundingBox(nanort::real3<double> *bmin,
                   nanort::real3<double> *bmax,
                   unsigned int prim_index) const {
    ++(*calls);
    inner->BoundingBox(bmin, bmax, prim_index);
  }
};

// Clip one triangle against the half-space z >= zc (keep_above true) or
// z <= zc, appending fan triangles.
void ClipTriangleZ(const double p[3][3], double zc, bool keep_above,
                   std::vector<double> *out_v,
                   std::vector<unsigned int> *out_f) {
  std::vector<double> poly;  // xyz triples
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3;
    const double di = keep_above ? (p[i][2] - zc) : (zc - p[i][2]);
    const double dj = keep_above ? (p[j][2] - zc) : (zc - p[j][2]);
    const bool i_in = di >= 0.0;
    const bool j_in = dj >= 0.0;
    auto add = [&](const double q[3]) {
      poly.push_back(q[0]);
      poly.push_back(q[1]);
      poly.push_back(q[2]);
    };
    if (i_in && j_in) {
      add(p[j]);
    } else if (i_in && !j_in) {
      const double r = di / (di - dj);
      double q[3];
      for (int k = 0; k < 3; ++k) q[k] = p[i][k] + (p[j][k] - p[i][k]) * r;
      add(q);
    } else if (!i_in && j_in) {
      const double r = di / (di - dj);
      double q[3];
      for (int k = 0; k < 3; ++k) q[k] = p[i][k] + (p[j][k] - p[i][k]) * r;
      add(q);
      add(p[j]);
    }
  }
  const unsigned base =
      static_cast<unsigned int>(out_v->size() / 3);
  const unsigned n = static_cast<unsigned>(poly.size() / 3);
  for (unsigned k = 1; k + 1 < n; ++k) {
    out_f->push_back(base);
    out_f->push_back(base + k);
    out_f->push_back(base + k + 1);
  }
  out_v->insert(out_v->end(), poly.begin(), poly.end());
}

bool Close(double a, double b) { return std::fabs(a - b) < kTol; }

// Snapshot of the immutable parts of a built tree.
struct TreeShape {
  std::vector<nanort::BVHNode<double> > nodes;
  std::vector<unsigned int> indices;
};

TreeShape Snapshot(const nanort::BVHAccel<double> &bvh) {
  TreeShape s;
  s.nodes = bvh.GetNodes();
  s.indices = bvh.GetIndices();
  return s;
}

bool ShapeUnchanged(const TreeShape &s,
                    const nanort::BVHAccel<double> &bvh) {
  const std::vector<nanort::BVHNode<double> > &nodes = bvh.GetNodes();
  if (nodes.size() != s.nodes.size() ||
      bvh.GetIndices().size() != s.indices.size()) {
    return false;
  }
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].flag != s.nodes[i].flag ||
        nodes[i].axis != s.nodes[i].axis ||
        nodes[i].data[0] != s.nodes[i].data[0] ||
        nodes[i].data[1] != s.nodes[i].data[1]) {
      return false;
    }
  }
  for (size_t i = 0; i < s.indices.size(); ++i) {
    if (bvh.GetIndices()[i] != s.indices[i]) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// projection group
// ---------------------------------------------------------------------------

int EmptyMeshReturnsOne() {
  std::vector<double> vertices;
  std::vector<unsigned int> faces;
  nanort::BVHAccel<double> bvh;
  BuildBVH(vertices, faces, &bvh);
  TASK_CHECK(!bvh.IsValid());
  const area_visibility::VisibilityResult r = area_visibility::query(
      bvh, vertices, faces, OriginReceiver(), StandardLight());
  TASK_CHECK(r.visible == 1.0);
  TASK_CHECK(r.node_tests == 0 && r.triangle_tests == 0);
  return 0;
}

int StripProjection() {
  std::vector<double> v;
  std::vector<unsigned int> f;
  AddQuad(&v, &f, -0.5, -0.25, -3.0, 3.0, 5.0);
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  const area_visibility::VisibilityResult r = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(r.visible, StripVisible(-0.5, -0.25)));
  TASK_CHECK(Close(r.visible, 0.875));
  TASK_CHECK(r.node_tests >= 1 && r.triangle_tests == 2);
  return 0;
}

int CenterRayMissesButAreaBlocked() {
  // Same off-center strip: the central shadow ray misses the occluder even
  // though an eighth of the light is blocked.
  std::vector<double> v;
  std::vector<unsigned int> f;
  AddQuad(&v, &f, -0.5, -0.25, -3.0, 3.0, 5.0);
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);

  nanort::Ray<double> ray;
  ray.org[0] = ray.org[1] = ray.org[2] = 0.0;
  ray.dir[0] = ray.dir[1] = 0.0;
  ray.dir[2] = 1.0;
  ray.min_t = 0.0;
  ray.max_t = 10.0;
  nanort::TriangleIntersector<double> inter(&v[0], &f[0],
                                            3 * sizeof(double));
  nanort::TriangleIntersection<double> hit;
  TASK_CHECK(!bvh.Traverse(ray, inter, &hit));

  const area_visibility::VisibilityResult r = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight());
  TASK_CHECK(r.visible < 1.0);
  TASK_CHECK(Close(r.visible, 0.875));
  return 0;
}

int SideClipping() {
  // A strip that extends far past the frustum side must clip to the same
  // coverage as a strip whose projection ends exactly on the light edge.
  std::vector<double> v_wide, v_exact;
  std::vector<unsigned int> f_wide, f_exact;
  AddQuad(&v_wide, &f_wide, 0.0, 100.0, -3.0, 3.0, 5.0);
  AddQuad(&v_exact, &f_exact, 0.0, 1.0, -3.0, 3.0, 5.0);
  nanort::BVHAccel<double> bvh_wide, bvh_exact;
  BuildBVH(v_wide, f_wide, &bvh_wide);
  BuildBVH(v_exact, f_exact, &bvh_exact);
  const area_visibility::VisibilityResult rw = area_visibility::query(
      bvh_wide, v_wide, f_wide, OriginReceiver(), StandardLight());
  const area_visibility::VisibilityResult re = area_visibility::query(
      bvh_exact, v_exact, f_exact, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(rw.visible, re.visible));
  TASK_CHECK(Close(rw.visible, StripVisible(0.0, 1.0)));
  return 0;
}

int DepthClipping() {
  // Triangle entirely behind the light, and one behind the receiver,
  // cannot cast back into the frustum.
  std::vector<double> v_behind_light;
  std::vector<unsigned int> f_behind_light;
  AddQuad(&v_behind_light, &f_behind_light, -1.0, 1.0, -1.0, 1.0, 12.0);
  nanort::BVHAccel<double> bvh_l;
  BuildBVH(v_behind_light, f_behind_light, &bvh_l);
  const area_visibility::VisibilityResult rl = area_visibility::query(
      bvh_l, v_behind_light, f_behind_light, OriginReceiver(),
      StandardLight());
  TASK_CHECK(Close(rl.visible, 1.0));

  std::vector<double> v_behind_recv;
  std::vector<unsigned int> f_behind_recv;
  AddQuad(&v_behind_recv, &f_behind_recv, -1.0, 1.0, -1.0, 1.0, -1.0);
  nanort::BVHAccel<double> bvh_r;
  BuildBVH(v_behind_recv, f_behind_recv, &bvh_r);
  const area_visibility::VisibilityResult rr = area_visibility::query(
      bvh_r, v_behind_recv, f_behind_recv, OriginReceiver(),
      StandardLight());
  TASK_CHECK(Close(rr.visible, 1.0));

  // A triangle straddling the light plane blocks exactly as much as the
  // same triangle clipped by hand to z <= 10.
  const double tri[3][3] = {{-0.5, -0.5, 2.0},
                            {0.5, -0.5, 12.0},
                            {0.5, 0.5, 12.0}};
  std::vector<double> vs;
  std::vector<unsigned int> fs;
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 3; ++k) vs.push_back(tri[i][k]);
  fs.push_back(0);
  fs.push_back(1);
  fs.push_back(2);
  nanort::BVHAccel<double> bvh_s;
  BuildBVH(vs, fs, &bvh_s);
  const area_visibility::VisibilityResult rs = area_visibility::query(
      bvh_s, vs, fs, OriginReceiver(), StandardLight());

  std::vector<double> vc;
  std::vector<unsigned int> fc;
  ClipTriangleZ(tri, 10.0, /*keep_above=*/false, &vc, &fc);
  TASK_CHECK(!fc.empty());
  nanort::BVHAccel<double> bvh_c;
  BuildBVH(vc, fc, &bvh_c);
  const area_visibility::VisibilityResult rc = area_visibility::query(
      bvh_c, vc, fc, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(rs.visible, rc.visible));
  TASK_CHECK(rs.visible < 1.0 && rs.visible > 0.0);

  // A triangle straddling the receiver plane loses its z <= 0 portion.
  const double tri2[3][3] = {{-0.5, -0.5, -2.0},
                             {0.5, -0.5, 3.0},
                             {0.5, 0.5, 3.0}};
  std::vector<double> vs2;
  std::vector<unsigned int> fs2;
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 3; ++k) vs2.push_back(tri2[i][k]);
  fs2.push_back(0);
  fs2.push_back(1);
  fs2.push_back(2);
  nanort::BVHAccel<double> bvh_s2;
  BuildBVH(vs2, fs2, &bvh_s2);
  const area_visibility::VisibilityResult rs2 = area_visibility::query(
      bvh_s2, vs2, fs2, OriginReceiver(), StandardLight());
  std::vector<double> vc2;
  std::vector<unsigned int> fc2;
  ClipTriangleZ(tri2, 0.0, /*keep_above=*/true, &vc2, &fc2);
  nanort::BVHAccel<double> bvh_c2;
  BuildBVH(vc2, fc2, &bvh_c2);
  const area_visibility::VisibilityResult rc2 = area_visibility::query(
      bvh_c2, vc2, fc2, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(rs2.visible, rc2.visible));
  return 0;
}

int FarFacesAreCulled() {
  // One real occluder plus 4096 faces far outside the frustum.
  std::vector<double> v;
  std::vector<unsigned int> f;
  AddQuad(&v, &f, -0.5, -0.25, -3.0, 3.0, 5.0);
  for (int i = 0; i < 4096; ++i) {
    const double xo = 1000.0 + static_cast<double>(i);
    AddQuad(&v, &f, xo, xo + 0.1, -0.1, 0.1, 50.0);
  }
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  const std::size_t total_faces = f.size() / 3;
  TASK_CHECK(total_faces >= 4096u);
  const area_visibility::VisibilityResult r = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(r.visible, 0.875));
  // Fewer than 1/16 of all faces actually enter clip/projection.
  TASK_CHECK(r.triangle_tests < total_faces / 16);
  TASK_CHECK(r.triangle_tests <= 2);
  return 0;
}

int RefitMechanicsAndVersions() {
  // 32 narrow strips => several leaf nodes at the default leaf capacity.
  std::vector<double> v;
  std::vector<unsigned int> f;
  for (int i = 0; i < 32; ++i) {
    const double xa = -1.5 + i * 0.09;
    AddQuad(&v, &f, xa, xa + 0.04, -3.0, 3.0, 5.0);
  }
  nanort::BVHAccel<double> bvh;
  TASK_CHECK(bvh.GetBuildGeneration() == 0);
  TASK_CHECK(bvh.GetRefitRevision() == 0);
  TASK_CHECK(bvh.GetLastRefitPrimitives().empty());
  BuildBVH(v, f, &bvh);
  TASK_CHECK(bvh.IsValid());
  TASK_CHECK(bvh.GetBuildGeneration() == 1);
  TASK_CHECK(bvh.GetRefitRevision() == 0);
  TASK_CHECK(bvh.GetNodes().size() > 1);

  const TreeShape before = Snapshot(bvh);
  double bmin0[3], bmax0[3];
  bvh.BoundingBox(bmin0, bmax0);

  // Move the first quad (vertices 0..3, faces 0 and 1) far sideways, then
  // refit only face 0. Both faces share the moved vertices. The target
  // quad keeps a non-zero x extent so its triangles stay non-degenerate.
  const unsigned int dirty_face = 0;
  // Vertex order in AddQuad: (xa,ya),(xb,ya),(xb,yb),(xa,yb).
  v[3 * 0 + 0] = 79.0;
  v[3 * 1 + 0] = 81.0;
  v[3 * 2 + 0] = 81.0;
  v[3 * 3 + 0] = 79.0;

  nanort::TriangleMesh<double> mesh(&v[0], &f[0], 3 * sizeof(double));
  std::size_t bbox_calls = 0;
  CountingMesh counter;
  counter.inner = &mesh;
  counter.calls = &bbox_calls;

  const std::vector<unsigned int> dirty(1, dirty_face);
  TASK_CHECK(bvh.Refit(counter, dirty));
  // Only primitives in the affected leaf were re-read, not all 64 faces.
  TASK_CHECK(bbox_calls > 0 && bbox_calls < 32);
  TASK_CHECK(bvh.GetRefitRevision() == 1);
  TASK_CHECK(bvh.GetLastRefitPrimitives().size() == 1);
  TASK_CHECK(bvh.GetLastRefitPrimitives()[0] == 0);

  // Tree topology and the original primitive order are untouched.
  TASK_CHECK(ShapeUnchanged(before, bvh));

  // Root bounds grew to include the moved face.
  double bmin1[3], bmax1[3];
  bvh.BoundingBox(bmin1, bmax1);
  TASK_CHECK(bmax1[0] >= 80.0 - kTol);
  TASK_CHECK(bmax0[0] < 80.0);

  // Traverse on the refitted tree: a ray toward the new position hits.
  nanort::Ray<double> ray;
  ray.org[0] = ray.org[1] = ray.org[2] = 0.0;
  ray.dir[0] = 16.0;
  ray.dir[1] = 0.0;
  ray.dir[2] = 1.0;
  ray.min_t = 0.0;
  ray.max_t = std::numeric_limits<double>::max();
  nanort::TriangleIntersector<double> inter(&v[0], &f[0],
                                            3 * sizeof(double));
  nanort::TriangleIntersection<double> hit;
  TASK_CHECK(bvh.Traverse(ray, inter, &hit));
  // The moved quad consists of faces 0 and 1; either may be nearest.
  TASK_CHECK(hit.prim_id == 0 || hit.prim_id == 1);

  // Duplicated dirty ids are processed once and revision advances once.
  const std::vector<unsigned int> dup(3, 0);
  TASK_CHECK(bvh.Refit(counter, dup));
  TASK_CHECK(bvh.GetRefitRevision() == 2);
  TASK_CHECK(bvh.GetLastRefitPrimitives().size() == 1);

  // Out-of-range id: false, and neither the tree nor the versions change.
  const std::vector<unsigned int> bad(1, 9999u);
  TASK_CHECK(!bvh.Refit(counter, bad));
  TASK_CHECK(bvh.GetRefitRevision() == 2);
  TASK_CHECK(bvh.GetLastRefitPrimitives().size() == 1);
  double bmin2[3], bmax2[3];
  bvh.BoundingBox(bmin2, bmax2);
  TASK_CHECK(bmax2[0] == bmax1[0]);

  // Legal empty dirty: true, versions unchanged.
  const std::vector<unsigned int> none;
  TASK_CHECK(bvh.Refit(counter, none));
  TASK_CHECK(bvh.GetRefitRevision() == 2);

  // Refit on an invalid tree fails.
  nanort::BVHAccel<double> invalid;
  TASK_CHECK(!invalid.Refit(counter, none));
  return 0;
}

int RefitSeparateSubtreesMatchesRebuild() {
  // 32 strips spread along x: the SAH split puts the leftmost and rightmost
  // faces in different root subtrees, so one refit must merge both updated
  // halves at every shared ancestor up to the root.
  std::vector<double> v;
  std::vector<unsigned int> f;
  for (int i = 0; i < 32; ++i) {
    const double xa = -1.5 + i * 0.09;
    AddQuad(&v, &f, xa, xa + 0.04, -3.0, 3.0, 5.0);
  }
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);

  // The two extreme faces must not share a leaf.
  const std::vector<nanort::BVHNode<double> > &nodes = bvh.GetNodes();
  unsigned int leaf_first = 0, leaf_last = 0;
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].flag != 1) continue;
    for (unsigned int k = 0; k < nodes[i].data[0]; ++k) {
      const unsigned int id = bvh.GetIndices()[nodes[i].data[1] + k];
      if (id == 0) leaf_first = static_cast<unsigned int>(i);
      if (id == 63) leaf_last = static_cast<unsigned int>(i);
    }
  }
  TASK_CHECK(leaf_first != leaf_last);

  // Find a leaf that contains neither dirty face and snapshot its bounds;
  // refit must not touch any node outside the affected ancestor set.
  unsigned int untouched_leaf = static_cast<unsigned int>(-1);
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].flag != 1) continue;
    bool touches_dirty = false;
    for (unsigned int k = 0; k < nodes[i].data[0]; ++k) {
      const unsigned int id = bvh.GetIndices()[nodes[i].data[1] + k];
      if (id == 0 || id == 63) touches_dirty = true;
    }
    if (!touches_dirty) {
      untouched_leaf = static_cast<unsigned int>(i);
      break;
    }
  }
  TASK_CHECK(untouched_leaf != static_cast<unsigned int>(-1));
  const nanort::BVHNode<double> untouched_before = nodes[untouched_leaf];

  // Move the leftmost quad far left and the rightmost quad far right.
  v[3 * 0 + 0] = -81.0;
  v[3 * 1 + 0] = -79.0;
  v[3 * 2 + 0] = -79.0;
  v[3 * 3 + 0] = -81.0;
  const unsigned last_base = 4 * 31;
  v[3 * (last_base + 0) + 0] = 79.0;
  v[3 * (last_base + 1) + 0] = 81.0;
  v[3 * (last_base + 2) + 0] = 81.0;
  v[3 * (last_base + 3) + 0] = 79.0;

  nanort::TriangleMesh<double> mesh(&v[0], &f[0], 3 * sizeof(double));
  std::size_t bbox_calls = 0;
  CountingMesh counter;
  counter.inner = &mesh;
  counter.calls = &bbox_calls;
  std::vector<unsigned int> dirty;
  dirty.push_back(0);
  dirty.push_back(63);
  TASK_CHECK(bvh.Refit(counter, dirty));
  TASK_CHECK(bbox_calls < 64);

  // A brand-new build over the same moved geometry is the ground truth for
  // every node bound; the refitted root must match it exactly.
  nanort::BVHAccel<double> rebuilt;
  BuildBVH(v, f, &rebuilt);
  double ra_min[3], ra_max[3], rb_min[3], rb_max[3];
  bvh.BoundingBox(ra_min, ra_max);
  rebuilt.BoundingBox(rb_min, rb_max);
  for (int k = 0; k < 3; ++k) {
    TASK_CHECK(Close(ra_min[k], rb_min[k]));
    TASK_CHECK(Close(ra_max[k], rb_max[k]));
  }
  TASK_CHECK(ra_min[0] <= -81.0 + kTol);
  TASK_CHECK(ra_max[0] >= 81.0 - kTol);

  // The untouched leaf still stores its original bounds.
  const nanort::BVHNode<double> &untouched_after = nodes[untouched_leaf];
  for (int k = 0; k < 3; ++k) {
    TASK_CHECK(untouched_after.bmin[k] == untouched_before.bmin[k]);
    TASK_CHECK(untouched_after.bmax[k] == untouched_before.bmax[k]);
  }
  return 0;
}

int EmptyBuildPublishesGeneration() {
  nanort::BVHAccel<double> bvh;
  nanort::TriangleMesh<double> mesh(0, 0, 3 * sizeof(double));
  nanort::TriangleSAHPred<double> pred(0, 0, 3 * sizeof(double));
  TASK_CHECK(!bvh.Build(0, mesh, pred));
  TASK_CHECK(!bvh.IsValid());
  TASK_CHECK(bvh.GetBuildGeneration() == 1);
  TASK_CHECK(bvh.GetRefitRevision() == 0);
  TASK_CHECK(bvh.GetLastRefitPrimitives().empty());
  return 0;
}

// ---------------------------------------------------------------------------
// union group
// ---------------------------------------------------------------------------

int OverlappingStripsUnion() {
  std::vector<double> v;
  std::vector<unsigned int> f;
  AddQuad(&v, &f, -1.0, 0.0, -3.0, 3.0, 5.0);
  AddQuad(&v, &f, -0.5, 0.5, -3.0, 3.0, 5.0);
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  const area_visibility::VisibilityResult r = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight());
  // Union x in [-1, 0.5]: blocked s-width 0.75.
  TASK_CHECK(Close(r.visible, 0.25));
  return 0;
}

int DuplicateAndReversedFaces() {
  std::vector<double> v_single;
  std::vector<unsigned int> f_single;
  AddQuad(&v_single, &f_single, -0.5, -0.25, -3.0, 3.0, 5.0);
  nanort::BVHAccel<double> bvh_single;
  BuildBVH(v_single, f_single, &bvh_single);
  const area_visibility::VisibilityResult base = area_visibility::query(
      bvh_single, v_single, f_single, OriginReceiver(), StandardLight());

  std::vector<double> v_dup;
  std::vector<unsigned int> f_dup;
  AddQuad(&v_dup, &f_dup, -0.5, -0.25, -3.0, 3.0, 5.0);
  AddQuad(&v_dup, &f_dup, -0.5, -0.25, -3.0, 3.0, 5.0, /*reverse=*/true);
  nanort::BVHAccel<double> bvh_dup;
  BuildBVH(v_dup, f_dup, &bvh_dup);
  const area_visibility::VisibilityResult dup = area_visibility::query(
      bvh_dup, v_dup, f_dup, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(dup.visible, base.visible));
  TASK_CHECK(Close(dup.visible, 0.875));

  // A reversed winding alone blocks the same area.
  std::vector<double> v_rev;
  std::vector<unsigned int> f_rev;
  AddQuad(&v_rev, &f_rev, -0.5, -0.25, -3.0, 3.0, 5.0,
          /*reverse=*/true);
  nanort::BVHAccel<double> bvh_rev;
  BuildBVH(v_rev, f_rev, &bvh_rev);
  const area_visibility::VisibilityResult rev = area_visibility::query(
      bvh_rev, v_rev, f_rev, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(rev.visible, base.visible));
  return 0;
}

int RetriangulationAndSharedEdge() {
  // Same quad triangulated along the other diagonal.
  std::vector<double> va;
  std::vector<unsigned int> fa;
  AddQuad(&va, &fa, -0.6, 0.4, -3.0, 3.0, 5.0);
  std::vector<double> vb = va;
  std::vector<unsigned int> fb;
  fb.push_back(0);
  fb.push_back(1);
  fb.push_back(3);
  fb.push_back(1);
  fb.push_back(2);
  fb.push_back(3);
  nanort::BVHAccel<double> bvh_a, bvh_b;
  BuildBVH(va, fa, &bvh_a);
  BuildBVH(vb, fb, &bvh_b);
  const area_visibility::VisibilityResult ra = area_visibility::query(
      bvh_a, va, fa, OriginReceiver(), StandardLight());
  const area_visibility::VisibilityResult rb = area_visibility::query(
      bvh_b, vb, fb, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(ra.visible, rb.visible));
  TASK_CHECK(Close(ra.visible, StripVisible(-0.6, 0.4)));

  // Two strips sharing an edge union without double counting the edge.
  std::vector<double> v;
  std::vector<unsigned int> f;
  AddQuad(&v, &f, -1.0, -0.5, -3.0, 3.0, 5.0);
  AddQuad(&v, &f, -0.5, 0.0, -3.0, 3.0, 5.0);
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  const area_visibility::VisibilityResult r = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(r.visible, StripVisible(-1.0, 0.0)));
  TASK_CHECK(Close(r.visible, 0.5));
  return 0;
}

int ExactCacheRepeatIsFree() {
  std::vector<double> v;
  std::vector<unsigned int> f;
  AddQuad(&v, &f, -0.5, -0.25, -3.0, 3.0, 5.0);
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  area_visibility::VisibilityCache cache;
  const area_visibility::VisibilityResult first = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), cache);
  TASK_CHECK(first.node_tests >= 1 && first.triangle_tests == 2);
  const area_visibility::VisibilityResult second = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), cache);
  TASK_CHECK(Close(second.visible, first.visible));
  TASK_CHECK(second.node_tests == 0 && second.triangle_tests == 0);
  return 0;
}

int IncrementalRefitReprojectsOnlyChangedFace() {
  // Six strips, all inside the frustum.
  std::vector<double> v;
  std::vector<unsigned int> f;
  for (int i = 0; i < 6; ++i) {
    const double xa = -1.2 + i * 0.4;
    AddQuad(&v, &f, xa, xa + 0.2, -3.0, 3.0, 5.0);
  }
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  area_visibility::VisibilityCache cache;
  const area_visibility::VisibilityResult first = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), cache);
  TASK_CHECK(first.triangle_tests == 12);

  // Move strip 0 (faces 0 and 1) slightly; it keeps covering the light.
  v[3 * f[0] + 0] += 0.05;
  v[3 * f[1] + 0] += 0.05;
  v[3 * f[2] + 0] += 0.05;
  v[3 * f[5] + 0] += 0.05;
  nanort::TriangleMesh<double> mesh(&v[0], &f[0], 3 * sizeof(double));
  std::vector<unsigned int> dirty;
  dirty.push_back(0);
  dirty.push_back(1);
  TASK_CHECK(bvh.Refit(mesh, dirty));

  const area_visibility::VisibilityResult inc = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), cache);
  const area_visibility::VisibilityResult fresh = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(inc.visible, fresh.visible));
  // Only the two changed triangles entered clip/projection.
  TASK_CHECK(inc.triangle_tests == 2);
  TASK_CHECK(inc.node_tests >= 1);
  return 0;
}

int FaceMovesOutAndIn() {
  std::vector<double> v;
  std::vector<unsigned int> f;
  AddQuad(&v, &f, -0.4, -0.2, -3.0, 3.0, 5.0);   // faces 0,1
  AddQuad(&v, &f, 0.2, 0.4, -3.0, 3.0, 5.0);     // faces 2,3
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  area_visibility::VisibilityCache cache;
  const area_visibility::VisibilityResult first = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), cache);
  TASK_CHECK(first.visible < 1.0);
  nanort::TriangleMesh<double> mesh(&v[0], &f[0], 3 * sizeof(double));

  // Move the first quad out of the frustum: its stale coverage is removed.
  for (int idx = 0; idx < 4; ++idx) v[3 * idx + 0] = 500.0;
  std::vector<unsigned int> dirty(2, 0);
  dirty[1] = 1;
  TASK_CHECK(bvh.Refit(mesh, dirty));
  area_visibility::VisibilityResult cached = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), cache);
  area_visibility::VisibilityResult fresh = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(cached.visible, fresh.visible));
  TASK_CHECK(cached.visible > first.visible);

  // Move it back in: it must be picked up even though no old candidate
  // polygon existed for it.
  const double xs[4] = {-0.4, -0.2, -0.2, -0.4};
  for (int idx = 0; idx < 4; ++idx) v[3 * idx + 0] = xs[idx];
  TASK_CHECK(bvh.Refit(mesh, dirty));
  cached = area_visibility::query(bvh, v, f, OriginReceiver(),
                                  StandardLight(), cache);
  fresh = area_visibility::query(bvh, v, f, OriginReceiver(),
                                 StandardLight());
  TASK_CHECK(Close(cached.visible, fresh.visible));
  TASK_CHECK(Close(cached.visible, first.visible));
  return 0;
}

int RemovingOneDuplicateKeepsCoverage() {
  std::vector<double> v;
  std::vector<unsigned int> f;
  AddQuad(&v, &f, -0.4, 0.4, -3.0, 3.0, 5.0);  // faces 0,1
  AddQuad(&v, &f, -0.4, 0.4, -3.0, 3.0, 5.0);  // faces 2,3
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  area_visibility::VisibilityCache cache;
  const area_visibility::VisibilityResult before = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), cache);
  TASK_CHECK(before.visible < 1.0);

  // Move one coincident quad away; the other still covers the region.
  const unsigned base = 4;  // second quad's vertices
  for (int idx = 0; idx < 4; ++idx) v[3 * (base + idx) + 0] = 500.0;
  nanort::TriangleMesh<double> mesh(&v[0], &f[0], 3 * sizeof(double));
  std::vector<unsigned int> dirty;
  dirty.push_back(2);
  dirty.push_back(3);
  TASK_CHECK(bvh.Refit(mesh, dirty));
  const area_visibility::VisibilityResult cached = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), cache);
  const area_visibility::VisibilityResult fresh = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(cached.visible, fresh.visible));
  TASK_CHECK(Close(cached.visible, before.visible));
  return 0;
}

int SkippedRefitsForceFullRecompute() {
  std::vector<double> v;
  std::vector<unsigned int> f;
  for (int i = 0; i < 6; ++i) {
    const double xa = -1.2 + i * 0.4;
    AddQuad(&v, &f, xa, xa + 0.2, -3.0, 3.0, 5.0);
  }
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  area_visibility::VisibilityCache cache;
  area_visibility::query(bvh, v, f, OriginReceiver(), StandardLight(),
                         cache);

  nanort::TriangleMesh<double> mesh(&v[0], &f[0], 3 * sizeof(double));
  std::vector<unsigned int> d0(1, 0);
  std::vector<unsigned int> d1(1, 2);
  v[3 * f[0] + 0] += 0.05;
  v[3 * f[1] + 0] += 0.05;
  v[3 * f[2] + 0] += 0.05;
  v[3 * f[5] + 0] += 0.05;
  TASK_CHECK(bvh.Refit(mesh, d0));
  v[3 * f[6] + 0] += 0.05;
  v[3 * f[7] + 0] += 0.05;
  v[3 * f[8] + 0] += 0.05;
  v[3 * f[11] + 0] += 0.05;
  TASK_CHECK(bvh.Refit(mesh, d1));
  TASK_CHECK(bvh.GetRefitRevision() == 2);

  // The cache missed revision 1: this must be a full recompute.
  const area_visibility::VisibilityResult cached = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), cache);
  const area_visibility::VisibilityResult fresh = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(cached.visible, fresh.visible));
  TASK_CHECK(cached.triangle_tests == fresh.triangle_tests);
  TASK_CHECK(cached.triangle_tests > 2);
  return 0;
}

int RebuildAndViewChangesInvalidate() {
  std::vector<double> v;
  std::vector<unsigned int> f;
  AddQuad(&v, &f, -0.4, 0.4, -3.0, 3.0, 5.0);
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  area_visibility::VisibilityCache cache;
  area_visibility::query(bvh, v, f, OriginReceiver(), StandardLight(),
                         cache);

  // A receiver change invalidates the stored view.
  const area_visibility::Vec3 shifted(0.3, 0.0, 0.0);
  area_visibility::VisibilityResult cached = area_visibility::query(
      bvh, v, f, shifted, StandardLight(), cache);
  area_visibility::VisibilityResult fresh = area_visibility::query(
      bvh, v, f, shifted, StandardLight());
  TASK_CHECK(Close(cached.visible, fresh.visible));
  TASK_CHECK(cached.triangle_tests > 0);

  // A light component change invalidates as well.
  const area_visibility::Rectangle smaller(
      area_visibility::Vec3(-1.0, -1.0, 10.0),
      area_visibility::Vec3(2.0, 0.0, 0.0),
      area_visibility::Vec3(0.0, 2.0, 0.0));
  cached = area_visibility::query(bvh, v, f, OriginReceiver(), smaller,
                                  cache);
  fresh = area_visibility::query(bvh, v, f, OriginReceiver(), smaller);
  TASK_CHECK(Close(cached.visible, fresh.visible));
  TASK_CHECK(cached.triangle_tests > 0);

  // Rebuild on the same object raises the generation: full recompute.
  const unsigned long long old_gen = bvh.GetBuildGeneration();
  BuildBVH(v, f, &bvh);
  TASK_CHECK(bvh.GetBuildGeneration() == old_gen + 1);
  TASK_CHECK(bvh.GetRefitRevision() == 0);
  cached = area_visibility::query(bvh, v, f, OriginReceiver(),
                                  StandardLight(), cache);
  fresh = area_visibility::query(bvh, v, f, OriginReceiver(),
                                 StandardLight());
  TASK_CHECK(Close(cached.visible, fresh.visible));
  TASK_CHECK(cached.triangle_tests == fresh.triangle_tests);
  return 0;
}

int TwoCachesInterleavedAcrossMissedUpdates() {
  std::vector<double> v;
  std::vector<unsigned int> f;
  for (int i = 0; i < 6; ++i) {
    const double xa = -1.2 + i * 0.4;
    AddQuad(&v, &f, xa, xa + 0.2, -3.0, 3.0, 5.0);
  }
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  area_visibility::VisibilityCache a, b;
  area_visibility::query(bvh, v, f, OriginReceiver(), StandardLight(), a);
  area_visibility::query(bvh, v, f, OriginReceiver(), StandardLight(), b);

  nanort::TriangleMesh<double> mesh(&v[0], &f[0], 3 * sizeof(double));
  std::vector<unsigned int> dirty(2, 0);
  dirty[1] = 1;
  v[3 * f[0] + 0] += 0.05;
  v[3 * f[1] + 0] += 0.05;
  v[3 * f[2] + 0] += 0.05;
  v[3 * f[5] + 0] += 0.05;
  TASK_CHECK(bvh.Refit(mesh, dirty));

  // B sees exactly one refit: incremental.
  const area_visibility::VisibilityResult rb = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), b);
  TASK_CHECK(rb.triangle_tests == 2);

  v[3 * f[0] + 0] += 0.05;
  v[3 * f[1] + 0] += 0.05;
  v[3 * f[2] + 0] += 0.05;
  v[3 * f[5] + 0] += 0.05;
  TASK_CHECK(bvh.Refit(mesh, dirty));

  // A missed revision 1 and now sees revision 2: full recompute, but
  // still correct. B also continues correctly.
  const area_visibility::VisibilityResult ra = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), a);
  const area_visibility::VisibilityResult ground = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight());
  TASK_CHECK(Close(ra.visible, ground.visible));
  TASK_CHECK(ra.triangle_tests == ground.triangle_tests);
  const area_visibility::VisibilityResult rb2 = area_visibility::query(
      bvh, v, f, OriginReceiver(), StandardLight(), b);
  TASK_CHECK(Close(rb2.visible, ground.visible));
  return 0;
}

// ---------------------------------------------------------------------------
// render group: mirrors examples/area_visibility.cpp pixel math.
// ---------------------------------------------------------------------------

int RoundGray(double visible) {
  int c = static_cast<int>(std::floor(25.0 + 230.0 * visible + 0.5));
  if (c < 0) c = 0;
  if (c > 255) c = 255;
  return c;
}

int RenderPixelsMatchQuery() {
  // Same single quad occluder as the example.
  std::vector<double> v;
  std::vector<unsigned int> f;
  AddQuad(&v, &f, -1.5, 1.5, -1.5, 1.5, 5.0);
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  const area_visibility::Rectangle light(
      area_visibility::Vec3(-2.0, -2.0, 10.0),
      area_visibility::Vec3(4.0, 0.0, 0.0),
      area_visibility::Vec3(0.0, 4.0, 0.0));

  const int width = 96;
  const int height = 64;
  bool saw_partial = false;
  int center_gray = -1;
  int corner_gray = -1;

  for (int py = 0; py < height; ++py) {
    for (int px = 0; px < width; ++px) {
      const double rx = -4.0 + 8.0 * px / (width - 1);
      const double ry =
          -3.0 + 6.0 * (height - 1 - py) / (height - 1);
      const area_visibility::Vec3 receiver(rx, ry, 0.0);
      const area_visibility::VisibilityResult r =
          area_visibility::query(bvh, v, f, receiver, light);
      TASK_CHECK(r.visible >= 0.0 && r.visible <= 1.0);
      TASK_CHECK(RoundGray(r.visible) ==
                 static_cast<int>(
                     std::floor(25.0 + 230.0 * r.visible + 0.5)));
      if (r.visible > 0.0 && r.visible < 1.0) saw_partial = true;
      if (px == width / 2 && py == height / 2) {
        center_gray = RoundGray(r.visible);
      }
      if (px == 0 && py == 0) corner_gray = RoundGray(r.visible);
    }
  }

  // Central receiver is in the umbra: fully blocked -> gray 25.
  TASK_CHECK(center_gray == 25);
  // Corner is lit much more strongly than the umbra.
  TASK_CHECK(corner_gray > center_gray);
  TASK_CHECK(saw_partial);
  return 0;
}

int RenderUmbraAndPenumbraValues() {
  std::vector<double> v;
  std::vector<unsigned int> f;
  AddQuad(&v, &f, -1.5, 1.5, -1.5, 1.5, 5.0);
  nanort::BVHAccel<double> bvh;
  BuildBVH(v, f, &bvh);
  const area_visibility::Rectangle light = StandardLight();

  // Receiver at the origin is fully inside the umbra.
  const area_visibility::VisibilityResult umbra = area_visibility::query(
      bvh, v, f, area_visibility::Vec3(0.0, 0.0, 0.0), light);
  TASK_CHECK(Close(umbra.visible, 0.0));

  // A receiver near the silhouette sees part of the light: penumbra.
  const area_visibility::VisibilityResult pen = area_visibility::query(
      bvh, v, f, area_visibility::Vec3(2.0, 0.0, 0.0), light);
  TASK_CHECK(pen.visible > 0.0 && pen.visible < 1.0);

  // Far enough sideways the light is mostly visible.
  const area_visibility::VisibilityResult bright = area_visibility::query(
      bvh, v, f, area_visibility::Vec3(4.0, 4.0, 0.0), light);
  TASK_CHECK(bright.visible > pen.visible);
  return 0;
}

}  // namespace

// driver.c is compiled as C++ (-x c++) by the task runner, so these
// definitions must use C++ linkage to match its check.h declarations.
const struct task_case task_cases[] = {
    {"projection", "empty_mesh", EmptyMeshReturnsOne},
    {"projection", "strip", StripProjection},
    {"projection", "centerline_miss", CenterRayMissesButAreaBlocked},
    {"projection", "side_clipping", SideClipping},
    {"projection", "depth_clipping", DepthClipping},
    {"projection", "far_faces_culling", FarFacesAreCulled},
    {"projection", "refit_mechanics", RefitMechanicsAndVersions},
    {"projection", "refit_separate_subtrees",
     RefitSeparateSubtreesMatchesRebuild},
    {"projection", "empty_build_generation", EmptyBuildPublishesGeneration},
    {"union", "overlapping_strips", OverlappingStripsUnion},
    {"union", "duplicate_reversed", DuplicateAndReversedFaces},
    {"union", "retriangulation_shared_edge", RetriangulationAndSharedEdge},
    {"union", "cache_repeat_free", ExactCacheRepeatIsFree},
    {"union", "cache_incremental", IncrementalRefitReprojectsOnlyChangedFace},
    {"union", "cache_move_in_out", FaceMovesOutAndIn},
    {"union", "cache_duplicate_removal", RemovingOneDuplicateKeepsCoverage},
    {"union", "cache_skipped_updates", SkippedRefitsForceFullRecompute},
    {"union", "cache_rebuild_view_change", RebuildAndViewChangesInvalidate},
    {"union", "cache_two_interleaved", TwoCachesInterleavedAcrossMissedUpdates},
    {"render", "pixel_colors", RenderPixelsMatchQuery},
    {"render", "umbra_penumbra", RenderUmbraAndPenumbraValues},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
