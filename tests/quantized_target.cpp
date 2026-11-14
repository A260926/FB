// Tests for the quantized offline ray preview (QuantizedBVH + QuantizedTraverser).
//
// The cases are registered through task_cases/task_case_count and are grouped
// into "encoding", "traversal" and "integration". See task_support/check.h.

#include <FastBVH.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

#include "task_support/check.h"

using namespace FastBVH;

namespace {

// ---------------------------------------------------------------------------
// Test primitives, converters and intersectors
// ---------------------------------------------------------------------------

template <typename Float>
struct QSphere {
  Vector3<Float> c;
  Float r;
  unsigned id;
  Float color[3];
};

template <typename Float>
struct QBox {
  Vector3<Float> lo;
  Vector3<Float> hi;
  unsigned id;
};

template <typename Float>
struct QSphereBox {
  BBox<Float> operator()(const QSphere<Float>& s) const {
    const Vector3<Float> d{s.r, s.r, s.r};
    return BBox<Float>(s.c - d, s.c + d);
  }
};

template <typename Float>
struct QBoxBox {
  BBox<Float> operator()(const QBox<Float>& b) const { return BBox<Float>(b.lo, b.hi); }
};

// Analytic ray/sphere intersector. Only returns non-negative, finite
// candidates or empty hits, and counts the invocations through `calls`.
template <typename Float>
struct QSphereHit {
  unsigned* calls;
  Intersection<Float, QSphere<Float>> operator()(const QSphere<Float>& s, const Ray<Float>& ray) const {
    if (calls) ++*calls;
    const auto v = ray.o - s.c;
    const Float a = dot(ray.d, ray.d);
    const Float b = dot(v, ray.d);
    const Float cc = dot(v, v) - s.r * s.r;
    const Float disc = b * b - a * cc;
    if (disc < 0) return {};
    const Float sq = std::sqrt(disc);
    Float t = (-b - sq) / a;
    if (t < 0) t = (-b + sq) / a;
    if (!(t >= 0)) return {};
    Intersection<Float, QSphere<Float>> hit;
    hit.t = t;
    hit.object = &s;
    const auto n = normalize(ray.o + ray.d * t - s.c);
    hit.normal = n;
    hit.uv[0] = std::atan2(n.z, n.x);
    hit.uv[1] = n.y;
    return hit;
  }
};

// Slab ray/box intersector, same candidate contract as QSphereHit.
template <typename Float>
struct QBoxHit {
  unsigned* calls;
  Intersection<Float, QBox<Float>> operator()(const QBox<Float>& b, const Ray<Float>& ray) const {
    if (calls) ++*calls;
    const BBox<Float> box(b.lo, b.hi);
    Float tn, tf;
    if (!box.intersect(ray, &tn, &tf)) return {};
    const Float t = tn >= 0 ? tn : tf;
    if (!(t >= 0) || !std::isfinite(t)) return {};
    const auto p = ray.o + ray.d * t;
    Vector3<Float> n{0, 0, 0};
    Float best = std::numeric_limits<Float>::infinity();
    int axis = 0;
    for (int a = 0; a < 3; ++a) {
      const Float dlo = std::fabs(p[a] - b.lo[a]);
      const Float dhi = std::fabs(p[a] - b.hi[a]);
      if (dlo < best) {
        best = dlo;
        axis = a;
        n = Vector3<Float>{0, 0, 0};
        n[a] = -1;
      }
      if (dhi < best) {
        best = dhi;
        axis = a;
        n = Vector3<Float>{0, 0, 0};
        n[a] = 1;
      }
    }
    Intersection<Float, QBox<Float>> hit;
    hit.t = t;
    hit.object = &b;
    hit.normal = n;
    hit.uv[0] = p[(axis + 1) % 3];
    hit.uv[1] = p[(axis + 2) % 3];
    return hit;
  }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Deterministic little PRNG (Numerical Recipes LCG).
struct Lcg {
  uint32_t s;
  explicit Lcg(uint32_t seed) : s(seed) {}
  uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s;
  }
  double f64() { return double(next() >> 8) * (1.0 / 16777216.0); }
};

template <typename Float>
void colorOf(unsigned id, Float* out) {
  out[0] = Float((id * 37u) % 100u) / Float(100);
  out[1] = Float((id * 61u) % 100u) / Float(100);
  out[2] = Float((id * 13u) % 97u) / Float(97);
}

std::vector<QSphere<float>> makeGridSpheres(float r) {
  std::vector<QSphere<float>> prims;
  for (unsigned y = 0; y < 8; ++y) {
    for (unsigned x = 0; x < 8; ++x) {
      QSphere<float> s;
      s.c = {3.0f * x, 3.0f * y, 0.0f};
      s.r = r;
      s.id = y * 8 + x;
      colorOf(s.id, s.color);
      prims.push_back(s);
    }
  }
  return prims;
}

// Independent brute-force closest hit: the reference for all traversal checks.
template <typename Float, typename Primitive, typename Intersector>
Intersection<Float, Primitive> bruteClosest(const std::vector<Primitive>& prims, const Ray<Float>& ray,
                                            const Intersector& isect) {
  Intersection<Float, Primitive> best;
  for (const auto& p : prims) {
    const auto h = isect(p, ray);
    if (h) best = closest(best, h);
  }
  return best;
}

// True if `obj` is one of the primitives that hit at exactly `t`.
template <typename Float, typename Primitive, typename Intersector>
bool inTieSet(const std::vector<Primitive>& prims, const Ray<Float>& ray, const Intersector& isect, Float t,
              const Primitive* obj) {
  for (const auto& p : prims) {
    const auto h = isect(p, ray);
    if (h && h.t == t && h.object == obj) return true;
  }
  return false;
}

// Number of primitives hitting at exactly `t`.
template <typename Float, typename Primitive, typename Intersector>
unsigned countTies(const std::vector<Primitive>& prims, const Ray<Float>& ray, const Intersector& isect, Float t) {
  unsigned n = 0;
  for (const auto& p : prims) {
    const auto h = isect(p, ray);
    if (h && h.t == t) ++n;
  }
  return n;
}

// Walks the exported tree level by level, decoding every node against the
// actually-decoded parent box, and verifies conservativeness and tightness
// against the source node boxes. Also checks that the layout fields survive.
template <typename Float, typename Primitive>
int checkLevels(const BVH<Float, Primitive>& src, const QuantizedBVH<Float, Primitive>& qb) {
  const auto sn = src.getNodes();
  const auto qn = qb.getNodes();
  TASK_CHECK(sn.size() == qn.size());
  TASK_CHECK(qb.getPrimitives().size() == src.getPrimitives().size());
  if (sn.size() == 0) return 0;

  const Float eps = std::numeric_limits<Float>::epsilon();
  const auto& root = qb.getRootBounds();

  // The root decodes exactly onto the stored root bounds, and the stored
  // root bounds cover the source root box.
  const BBox<Float> root_dec = decodeQuantizedBounds(root, qn[0]);
  for (int a = 0; a < 3; ++a) {
    TASK_CHECK(root_dec.min[a] == root.min[a]);
    TASK_CHECK(root_dec.max[a] == root.max[a]);
    TASK_CHECK(root.min[a] <= sn[0].bbox.min[a]);
    TASK_CHECK(root.max[a] >= sn[0].bbox.max[a]);
    TASK_CHECK(qn[0].min[a] == 0);
    const Float span = root.max[a] - root.min[a];
    const Float pabs = std::max(std::max(std::fabs(root.min[a]), std::fabs(root.max[a])), Float(1));
    // When the grid step clearly dominates rounding, the root spans the whole grid.
    if (span > 0 && span / 65535 > 8 * eps * pabs) TASK_CHECK(qn[0].max[a] == 65535);
  }

  struct Entry {
    uint32_t i;
    BBox<Float> parent;
  };
  std::vector<Entry> stack;
  stack.push_back(Entry{0, root});
  std::vector<char> visited(sn.size(), 0);
  size_t visits = 0;

  while (!stack.empty()) {
    const Entry e = stack.back();
    stack.pop_back();
    const auto& s = sn[e.i];
    const auto& q = qn[e.i];
    if (!visited[e.i]) {
      visited[e.i] = 1;
      ++visits;
    }

    // Layout fields survive the export verbatim (leaf slots, child offsets).
    TASK_CHECK(q.start == s.start);
    TASK_CHECK(q.primitive_count == s.primitive_count);
    TASK_CHECK(q.right_offset == s.right_offset);
    TASK_CHECK(q.isLeaf() == s.isLeaf());

    // Decode this node against the actually-decoded parent box.
    const BBox<Float> dec = decodeQuantizedBounds(e.parent, q);
    for (int a = 0; a < 3; ++a) {
      const Float pspan = e.parent.max[a] - e.parent.min[a];
      const Float pabs = std::max(std::max(std::fabs(e.parent.min[a]), std::fabs(e.parent.max[a])), Float(1));
      const Float tol = pspan * (Float(2) / Float(65535)) + Float(32) * eps * pabs;
      // Conservative: the decoded box covers the closed source box.
      TASK_CHECK(dec.min[a] <= s.bbox.min[a]);
      TASK_CHECK(dec.max[a] >= s.bbox.max[a]);
      // Tight: each end overshoots the source box by less than the tolerance.
      TASK_CHECK(dec.min[a] >= s.bbox.min[a] - tol);
      TASK_CHECK(dec.max[a] <= s.bbox.max[a] + tol);
      TASK_CHECK(dec.min[a] <= dec.max[a]);
    }

    if (!s.isLeaf()) {
      stack.push_back(Entry{e.i + 1, dec});
      stack.push_back(Entry{e.i + s.right_offset, dec});
    }
  }
  TASK_CHECK(visits == sn.size());
  return 0;
}

// ---------------------------------------------------------------------------
// encoding group
// ---------------------------------------------------------------------------

int encoding_level_decode_float_mixed() {
  std::vector<QBox<float>> boxes;
  unsigned id = 0;
  // Regular boxes.
  for (int i = 0; i < 60; ++i) {
    const float x = float(i % 10) * 2.0f, y = float(i / 10) * 2.0f;
    boxes.push_back({{x, y, -1.0f}, {x + 1.0f, y + 1.0f, 1.0f}, id++});
  }
  // Thin boxes (thin in z).
  for (int i = 0; i < 20; ++i) {
    const float x = i * 3.0f;
    boxes.push_back({{x, 20.0f, 5.0f}, {x + 1.5f, 21.0f, 5.0f + 1e-3f}, id++});
  }
  // Planar boxes (zero-width y axis).
  for (int i = 0; i < 20; ++i) {
    const float x = i * 2.0f;
    boxes.push_back({{x, 30.0f, -3.0f}, {x + 1.0f, 30.0f, -2.0f}, id++});
  }
  // Large coordinates.
  for (int i = 0; i < 20; ++i) {
    const float b = 1.0e4f + i * 3.0f;
    boxes.push_back({{b, -1.0e4f, 2.0e4f}, {b + 1.0f, -1.0e4f + 2.0f, 2.0e4f + 1.0f}, id++});
  }
  // Negative-zero corners, including a zero-volume box at (-0, -0, -0).
  boxes.push_back({{-0.0f, -0.0f, -0.0f}, {1.0f, 1.0f, 1.0f}, id++});
  boxes.push_back({{-0.0f, -0.0f, -0.0f}, {-0.0f, -0.0f, -0.0f}, id++});

  DefaultBuilder<float> builder;
  auto bvh = builder(boxes, QBoxBox<float>{});
  TASK_CHECK(bvh.getNodes().size() > 1);

  QuantizedBVH<float, QBox<float>> qb(bvh);
  TASK_CHECK(sizeof(QuantizedNode) <= 24);
  TASK_CHECK(qb.getNodes().size() * sizeof(QuantizedNode) < bvh.getNodes().size() * sizeof(Node<float>));

  return checkLevels(bvh, qb);
}

int encoding_level_decode_double_deep() {
  // Hand-built chain: deep tree with nested boxes, thin and planar boxes,
  // and large coordinates over many consecutive levels.
  constexpr int kDepth = 40;  // internal nodes; tree depth is kDepth + 1
  constexpr int kPrims = kDepth + 1;

  std::vector<QBox<double>> prims;
  prims.reserve(kPrims);
  for (int i = 0; i < kPrims; ++i) {
    const double x = 2.0 * i;
    QBox<double> b;
    b.lo = {x, -0.005, 1.0e6};
    b.hi = {x + 0.5, 0.005, 1.0e6 + 1.0};
    if (i % 3 == 0) {
      b.lo.y = 0.0;
      b.hi.y = 0.0;  // planar box: zero-width y axis
    }
    b.id = unsigned(i);
    prims.push_back(b);
  }

  NodeArray<double> nodes(2 * kDepth + 1);
  for (int i = 0; i < kDepth; ++i) {
    BBox<double> bb(prims[i].lo, prims[i].hi);
    for (int j = i + 1; j < kPrims; ++j) bb.expandToInclude(BBox<double>(prims[j].lo, prims[j].hi));
    nodes[2 * i] = Node<double>{bb, uint32_t(i), uint32_t(kPrims - i), 2u};
    nodes[2 * i + 1] = Node<double>{BBox<double>(prims[i].lo, prims[i].hi), uint32_t(i), 1u, 0u};
  }
  nodes[2 * kDepth] = Node<double>{BBox<double>(prims[kDepth].lo, prims[kDepth].hi), uint32_t(kDepth), 1u, 0u};

  BVH<double, QBox<double>> bvh(std::move(nodes), ConstIterable<QBox<double>>(prims.data(), prims.size()));
  QuantizedBVH<double, QBox<double>> qb(bvh);

  TASK_CHECK(qb.getNodes().size() == size_t(2 * kDepth + 1));
  TASK_CHECK(sizeof(QuantizedNode) <= 24);
  if (checkLevels(bvh, qb) != 0) return 1;

  // The deep exported tree traverses correctly (no recursion, small stack).
  unsigned calls = 0;
  QuantizedTraverser<double, QBox<double>, QBoxHit<double>> trav(qb, QBoxHit<double>{&calls});
  const QBoxHit<double> bhit{nullptr};
  const Ray<double> rays[] = {
      {{-1.0, 0.0, 1.0e6 + 0.5}, {1.0, 0.0, 0.0}},        // along the whole chain
      {{-1.0, -0.005, 1.0e6 + 0.5}, {1.0, -0.0, 0.0}},    // origin on the min-y face, -0 direction
      {{2.0 * 25 + 0.25, 0.0, 1.0e6 - 3.0}, {0.0, 0.0, 1.0}},  // straight at box 25
      {{-1.0, 5.0, 1.0e6 + 0.5}, {1.0, 0.0, 0.0}},        // misses everything
  };
  for (const auto& ray : rays) {
    const auto qh = trav.traverse(ray);
    const auto bh = bruteClosest(prims, ray, bhit);
    TASK_CHECK(bool(qh) == bool(bh));
    TASK_CHECK(qh.t == bh.t);
    if (qh) TASK_CHECK(inTieSet(prims, ray, bhit, bh.t, qh.object));
  }
  // The missing ray is rejected at the root: not a single primitive callback.
  {
    const unsigned before = calls;
    const auto h = trav.traverse(Ray<double>({-1.0, 5.0, 1.0e6 + 0.5}, {1.0, 0.0, 0.0}));
    TASK_CHECK(!h);
    TASK_CHECK(calls == before);
  }
  return 0;
}

int encoding_empty_and_degenerate() {
  // Empty tree from an empty node array and empty primitives (float).
  {
    BVH<float, QSphere<float>> empty_bvh(NodeArray<float>{}, ConstIterable<QSphere<float>>(nullptr, 0));
    QuantizedBVH<float, QSphere<float>> qb(empty_bvh);
    TASK_CHECK(qb.getNodes().size() == 0);
    TASK_CHECK(qb.getPrimitives().size() == 0);
    QuantizedTraverser<float, QSphere<float>, QSphereHit<float>> trav(qb, QSphereHit<float>{nullptr});
    TASK_CHECK(!trav.traverse(Ray<float>({0, 0, 0}, {0, 0, 1})));
  }
  // Empty tree (double), occlusion query.
  {
    BVH<double, QBox<double>> empty_bvh(NodeArray<double>{}, ConstIterable<QBox<double>>(nullptr, 0));
    QuantizedBVH<double, QBox<double>> qb(empty_bvh);
    TASK_CHECK(qb.getNodes().size() == 0);
    QuantizedTraverser<double, QBox<double>, QBoxHit<double>, OnlyTestOcclusion> trav(qb, QBoxHit<double>{nullptr});
    TASK_CHECK(!trav.traverse(Ray<double>({1, 2, 3}, {-1, 0.5, 0.25})));
  }
  // Single primitive: the root is a leaf.
  {
    std::vector<QSphere<float>> prims(1);
    prims[0].c = {1.0f, 2.0f, 3.0f};
    prims[0].r = 0.5f;
    prims[0].id = 7;
    colorOf(7u, prims[0].color);
    DefaultBuilder<float> builder;
    auto bvh = builder(prims, QSphereBox<float>{});
    TASK_CHECK(bvh.getNodes().size() == 1);
    QuantizedBVH<float, QSphere<float>> qb(bvh);
    if (checkLevels(bvh, qb) != 0) return 1;
    QuantizedTraverser<float, QSphere<float>, QSphereHit<float>> trav(qb, QSphereHit<float>{nullptr});
    const auto h = trav.traverse(Ray<float>({1.0f, 2.0f, 0.0f}, {0.0f, 0.0f, 1.0f}));
    TASK_CHECK(h);
    TASK_CHECK(h.object == &prims[0]);
    TASK_CHECK(!trav.traverse(Ray<float>({1.0f, 2.0f, 0.0f}, {0.0f, 0.0f, -1.0f})));
  }
  // Coplanar primitives: the root box has a zero-width axis.
  {
    std::vector<QBox<double>> boxes;
    for (int i = 0; i < 12; ++i) {
      boxes.push_back({{i * 2.0, 7.0, -1.0}, {i * 2.0 + 1.0, 7.0, 1.0}, unsigned(i)});
    }
    DefaultBuilder<double> builder;
    auto bvh = builder(boxes, QBoxBox<double>{});
    TASK_CHECK(bvh.getNodes()[0].bbox.min[1] == 7.0);
    TASK_CHECK(bvh.getNodes()[0].bbox.max[1] == 7.0);
    QuantizedBVH<double, QBox<double>> qb(bvh);
    if (checkLevels(bvh, qb) != 0) return 1;
    // The zero-width axis keeps its coordinate exactly, no division by zero.
    const BBox<double> root_dec = decodeQuantizedBounds(qb.getRootBounds(), qb.getNodes()[0]);
    TASK_CHECK(root_dec.min[1] == 7.0);
    TASK_CHECK(root_dec.max[1] == 7.0);
    // A ray inside the plane finds the same box as brute force.
    QuantizedTraverser<double, QBox<double>, QBoxHit<double>> trav(qb, QBoxHit<double>{nullptr});
    const QBoxHit<double> bhit{nullptr};
    const Ray<double> ray({0.5, 7.0, -5.0}, {0.0, -0.0, 1.0});
    const auto qh = trav.traverse(ray);
    const auto bh = bruteClosest(boxes, ray, bhit);
    TASK_CHECK(qh);
    TASK_CHECK(qh.t == bh.t);
    TASK_CHECK(qh.object == bh.object);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// traversal group
// ---------------------------------------------------------------------------

int traversal_closest_matches_bruteforce_float() {
  auto prims = makeGridSpheres(0.5f);
  DefaultBuilder<float> builder;
  auto bvh = builder(prims, QSphereBox<float>{});
  QuantizedBVH<float, QSphere<float>> qb(bvh);

  unsigned qcalls = 0, bcalls = 0;
  QuantizedTraverser<float, QSphere<float>, QSphereHit<float>> qtrav(qb, QSphereHit<float>{&qcalls});
  Traverser<float, QSphere<float>, QSphereHit<float>> otrav(bvh, QSphereHit<float>{nullptr});
  const QSphereHit<float> bhit{&bcalls};
  const QSphereHit<float> ncount{nullptr};

  Lcg rng(7);
  unsigned compared = 0;
  for (int r = 0; r < 256; ++r) {
    Vector3<float> o, d;
    if (r < 64) {  // aimed hits, like the baseline
      const unsigned x = unsigned(r) % 8, y = unsigned(r) / 8;
      o = {3.0f * x - 0.03f, 3.0f * y - 0.02f, -3.0f};
      d = {0.01f, 0.006666667f, 1.0f};
    } else if (r < 128) {  // aimed misses
      const unsigned x = unsigned(r) % 8, y = (unsigned(r) / 8) % 8;
      o = {3.0f * x + 1.0f, 3.0f * y + 1.0f, -3.0f};
      d = {0.001f, 0.002f, 1.0f};
    } else {  // oblique rays
      o = {float(rng.f64() * 24 - 1.5), float(rng.f64() * 24 - 1.5), float(rng.f64() * -8 - 0.5)};
      d = {float(rng.f64() * 0.6 - 0.3), float(rng.f64() * 0.6 - 0.3), 1.0f};
    }
    const Ray<float> ray(o, d);
    const auto qh = qtrav.traverse(ray);
    const auto bh = bruteClosest(prims, ray, bhit);
    const auto oh = otrav.traverse(ray);
    TASK_CHECK(bool(qh) == bool(bh));
    TASK_CHECK(qh.t == bh.t);   // same closest distance as independent brute force
    TASK_CHECK(oh.t == bh.t);   // ... and as the original traverser
    if (qh) TASK_CHECK(inTieSet(prims, ray, ncount, bh.t, qh.object));
    ++compared;
  }
  TASK_CHECK(compared == 256);
  // Real pruning: far fewer primitive tests than the exhaustive reference.
  TASK_CHECK(qcalls * 2 < bcalls);
  std::printf("PRUNE closest rays=256 quantized_primitive_calls=%u brute_primitive_calls=%u\n", qcalls, bcalls);
  return 0;
}

int traversal_occlusion_flags_double() {
  Lcg rng(123);
  std::vector<QSphere<double>> prims;
  for (unsigned i = 0; i < 300; ++i) {
    QSphere<double> s;
    s.c = {rng.f64() * 100 - 50, rng.f64() * 100 - 50, rng.f64() * 100 - 50};
    s.r = 0.5 + rng.f64() * 1.5;
    s.id = i;
    colorOf(i, s.color);
    prims.push_back(s);
  }
  DefaultBuilder<double> builder;
  auto bvh = builder(prims, QSphereBox<double>{});
  QuantizedBVH<double, QSphere<double>> qb(bvh);

  unsigned qcalls = 0;
  QuantizedTraverser<double, QSphere<double>, QSphereHit<double>> qclose(qb, QSphereHit<double>{&qcalls});
  QuantizedTraverser<double, QSphere<double>, QSphereHit<double>, OnlyTestOcclusion> qocc(
      qb, QSphereHit<double>{&qcalls});
  const QSphereHit<double> bhit{nullptr};

  Lcg rays(999);
  unsigned occluded = 0;
  for (int r = 0; r < 128; ++r) {
    Vector3<double> o{rays.f64() * 120 - 60, rays.f64() * 120 - 60, rays.f64() * 120 - 60};
    Vector3<double> d{rays.f64() * 2 - 1, rays.f64() * 2 - 1, rays.f64() * 2 - 1};
    if (d.x == 0 && d.y == 0 && d.z == 0) d.z = 1.0;
    const Ray<double> ray(o, d);

    // Occlusion: any hit at all, compared against independent brute force.
    const auto occ = qocc.traverse(ray);
    bool bany = false;
    for (const auto& p : prims) {
      if (bhit(p, ray)) {
        bany = true;
        break;
      }
    }
    TASK_CHECK(bool(occ) == bany);
    if (occ) {
      TASK_CHECK(occ.object >= &prims.front() && occ.object <= &prims.back());
      TASK_CHECK(std::isfinite(occ.t) && occ.t >= 0);
      ++occluded;
    }

    // Closest hit on the same tree.
    const auto qh = qclose.traverse(ray);
    const auto bh = bruteClosest(prims, ray, bhit);
    TASK_CHECK(qh.t == bh.t);
    if (qh) TASK_CHECK(inTieSet(prims, ray, bhit, bh.t, qh.object));
  }
  TASK_CHECK(occluded > 0);
  std::printf("PRUNE occlusion rays=128 quantized_primitive_calls=%u occluded=%u\n", qcalls, occluded);
  return 0;
}

int traversal_edge_rays_signed_zero() {
  auto prims = makeGridSpheres(0.5f);
  DefaultBuilder<float> builder;
  auto bvh = builder(prims, QSphereBox<float>{});
  QuantizedBVH<float, QSphere<float>> qb(bvh);
  QuantizedTraverser<float, QSphere<float>, QSphereHit<float>> trav(qb, QSphereHit<float>{nullptr});
  const QSphereHit<float> bhit{nullptr};

  const float cx = 3.0f * 3, cy = 3.0f * 5;  // sphere (3,5): center (9, 15, 0)
  const Ray<float> rays[] = {
      {{cx, cy, -5.0f}, {0.0f, 0.0f, 1.0f}},                              // axis-parallel through the center
      {{-5.0f, cy, 0.0f}, {1.0f, -0.0f, -0.0f}},                          // negative-zero direction components
      {{cx, cy, -5.0f}, {-0.0f, -0.0f, 1.0f}},                            // more negative zero
      {{cx - 0.5f, cy - 0.5f, -1.5f}, {1.0f, 1.0f, 1.0f}},                // straight through the box corner
      {{cx - 0.5f, cy, -3.0f}, {0.0f, 0.0f, 1.0f}},                       // origin on the box face
      {{cx + 0.5f, cy + 0.5f, -5.0f}, {0.0f, 0.0f, 1.0f}},                // travels along the box edge
      {{cx + 0.5f, cy, -5.0f}, {0.0f, 0.0f, 1.0f}},                       // tangent to the sphere
      {{cx, cy, -5.0f}, {0.0f, 1.0f, 0.0f}},                              // parallel to the sphere plane, miss
  };
  for (const auto& ray : rays) {
    const auto qh = trav.traverse(ray);
    const auto bh = bruteClosest(prims, ray, bhit);
    TASK_CHECK(bool(qh) == bool(bh));
    TASK_CHECK(qh.t == bh.t);
    if (qh) TASK_CHECK(inTieSet(prims, ray, bhit, bh.t, qh.object));
  }

  // Double precision: origin exactly on the surface, signed-zero components.
  std::vector<QSphere<double>> one(1);
  one[0].c = {0.0, 0.0, 0.0};
  one[0].r = 1.0;
  one[0].id = 0;
  colorOf(0u, one[0].color);
  DefaultBuilder<double> dbuilder;
  auto dbvh = dbuilder(one, QSphereBox<double>{});
  QuantizedBVH<double, QSphere<double>> dqb(dbvh);
  QuantizedTraverser<double, QSphere<double>, QSphereHit<double>> dtrav(dqb, QSphereHit<double>{nullptr});
  const QSphereHit<double> dbhit{nullptr};
  const Ray<double> drays[] = {
      {{-1.0, -0.0, -0.0}, {1.0, 0.0, 0.0}},   // origin on the surface/box face, t == 0 hit
      {{0.0, -5.0, 0.0}, {-0.0, 1.0, -0.0}},   // vertical ray, negative-zero components
      {{2.0, 0.0, 0.0}, {1.0, 0.0, 0.0}},      // points away, miss
  };
  for (const auto& ray : drays) {
    const auto qh = dtrav.traverse(ray);
    const auto bh = bruteClosest(one, ray, dbhit);
    TASK_CHECK(bool(qh) == bool(bh));
    TASK_CHECK(qh.t == bh.t);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// integration group
// ---------------------------------------------------------------------------

int integration_source_destroyed() {
  auto prims = makeGridSpheres(0.5f);
  std::unique_ptr<QuantizedBVH<float, QSphere<float>>> qb;
  size_t src_nodes = 0, src_bytes = 0;
  {
    DefaultBuilder<float> builder;
    auto bvh = builder(prims, QSphereBox<float>{});
    src_nodes = bvh.getNodes().size();
    src_bytes = src_nodes * sizeof(Node<float>);
    qb.reset(new QuantizedBVH<float, QSphere<float>>(bvh));
    TASK_CHECK(qb->getNodes().size() == src_nodes);
    // The compressed nodes are owned; they do not alias the source nodes.
    TASK_CHECK((const void*)qb->getNodes().begin() != (const void*)bvh.getNodes().begin());
    TASK_CHECK(qb->getPrimitives().begin() == bvh.getPrimitives().begin());
  }  // The source BVH is destroyed here.

  TASK_CHECK(qb->getNodes().size() == src_nodes);
  TASK_CHECK(qb->getNodes().size() * sizeof(QuantizedNode) < src_bytes);

  // Queries keep working from the compressed nodes alone.
  unsigned calls = 0;
  QuantizedTraverser<float, QSphere<float>, QSphereHit<float>> trav(*qb, QSphereHit<float>{&calls});
  const QSphereHit<float> bhit{nullptr};
  for (unsigned r = 0; r < 64; ++r) {
    const unsigned x = r % 8, y = r / 8;
    const Ray<float> ray({3.0f * x - 0.03f, 3.0f * y - 0.02f, -3.0f}, {0.01f, 0.006666667f, 1.0f});
    const auto qh = trav.traverse(ray);
    const auto bh = bruteClosest(prims, ray, bhit);
    TASK_CHECK(qh);
    TASK_CHECK(qh.t == bh.t);
    TASK_CHECK(qh.object == bh.object);
  }
  for (unsigned r = 0; r < 16; ++r) {
    const unsigned x = r % 8, y = r / 8;
    const Ray<float> ray({3.0f * x + 1.0f, 3.0f * y + 1.0f, -3.0f}, {0.001f, 0.002f, 1.0f});
    TASK_CHECK(!trav.traverse(ray));
  }
  return 0;
}

int integration_borrowed_primitive_identity() {
  Lcg rng(2024);
  std::vector<QSphere<double>> prims;
  for (unsigned i = 0; i < 100; ++i) {
    QSphere<double> s;
    s.c = {rng.f64() * 40 - 20, rng.f64() * 40 - 20, rng.f64() * 40 - 20};
    s.r = 0.25 + rng.f64();
    s.id = i;
    colorOf(i, s.color);
    prims.push_back(s);
  }
  DefaultBuilder<double> builder;
  auto bvh = builder(prims, QSphereBox<double>{});
  QuantizedBVH<double, QSphere<double>> qb(bvh);

  // The primitives are borrowed from the caller's array, not copied.
  TASK_CHECK(qb.getPrimitives().begin() == prims.data());
  TASK_CHECK(qb.getPrimitives().size() == prims.size());

  // Leaf slots keep their identity through the export.
  const auto sn = bvh.getNodes();
  const auto qn = qb.getNodes();
  unsigned leafs = 0;
  for (size_t i = 0; i < sn.size(); ++i) {
    if (!sn[i].isLeaf()) continue;
    ++leafs;
    TASK_CHECK(qn[i].start == sn[i].start);
    TASK_CHECK(qn[i].primitive_count == sn[i].primitive_count);
    for (uint32_t o = 0; o < sn[i].primitive_count; ++o) {
      TASK_CHECK(&qb.getPrimitives()[qn[i].start + o] == &prims[sn[i].start + o]);
    }
  }
  TASK_CHECK(leafs > 0);

  // object/t/normal/uv come from the real intersector, bit for bit.
  QuantizedTraverser<double, QSphere<double>, QSphereHit<double>> trav(qb, QSphereHit<double>{nullptr});
  const QSphereHit<double> direct{nullptr};
  Lcg rays(31);
  for (int r = 0; r < 32; ++r) {
    const Ray<double> ray({rays.f64() * 50 - 25, rays.f64() * 50 - 25, rays.f64() * 50 - 25},
                          {rays.f64() * 2 - 1, rays.f64() * 2 - 1, rays.f64() * 2 - 1});
    const auto h = trav.traverse(ray);
    const auto bh = bruteClosest(prims, ray, direct);
    TASK_CHECK(h.t == bh.t);
    if (!h) continue;
    if (countTies(prims, ray, direct, bh.t) == 1) {
      TASK_CHECK(h.object == bh.object);
    } else {
      TASK_CHECK(inTieSet(prims, ray, direct, bh.t, h.object));
    }
    const auto d = direct(*h.object, ray);
    TASK_CHECK(bool(d));
    TASK_CHECK(d.t == h.t);
    TASK_CHECK(d.normal.x == h.normal.x && d.normal.y == h.normal.y && d.normal.z == h.normal.z);
    TASK_CHECK(d.uv[0] == h.uv[0] && d.uv[1] == h.uv[1]);
  }
  return 0;
}

int integration_ppm_preview() {
  auto prims = makeGridSpheres(0.6f);
  DefaultBuilder<float> builder;
  auto bvh = builder(prims, QSphereBox<float>{});
  QuantizedBVH<float, QSphere<float>> qb(bvh);

  const int W = 64, H = 48;
  std::vector<unsigned char> img(size_t(W) * size_t(H) * 3);
  unsigned qcalls = 0, bcalls = 0;
  QuantizedTraverser<float, QSphere<float>, QSphereHit<float>> trav(qb, QSphereHit<float>{&qcalls});
  const QSphereHit<float> bhit{&bcalls};
  const Vector3<float> light = normalize(Vector3<float>{0.5f, 0.8f, -0.6f});

  unsigned mismatches = 0, ties = 0, hit_pixels = 0;
  for (int py = 0; py < H; ++py) {
    for (int px = 0; px < W; ++px) {
      const float u = (px + 0.5f) / W, v = (py + 0.5f) / H;
      const Ray<float> ray({u * 26.0f - 2.5f, v * 26.0f - 2.5f, -8.0f},
                           {(u - 0.5f) * 0.3f, (v - 0.5f) * 0.3f, 1.0f});
      const auto qh = trav.traverse(ray);
      const auto bh = bruteClosest(prims, ray, bhit);
      if (qh.t != bh.t) ++mismatches;

      // Shade from the primitive color and the hit normal.
      float rgb[3] = {0.08f, 0.12f, 0.2f};
      if (qh) {
        ++hit_pixels;
        if (qh.object != bh.object) ++ties;  // only possible on an exact tie
        const auto& s = *qh.object;
        const float lam = 0.35f + 0.65f * std::fmax(0.0f, dot(qh.normal, light));
        rgb[0] = s.color[0] * lam;
        rgb[1] = s.color[1] * lam;
        rgb[2] = s.color[2] * lam;
      }
      for (int k = 0; k < 3; ++k) {
        const float c = rgb[k] < 0 ? 0.0f : (rgb[k] > 1 ? 1.0f : rgb[k]);
        img[(size_t(py) * W + px) * 3 + k] = (unsigned char)(c * 255.0f + 0.5f);
      }
    }
  }
  TASK_CHECK(mismatches == 0);
  TASK_CHECK(ties == 0);
  TASK_CHECK(hit_pixels > 0);

  FILE* f = std::fopen("quantized_preview.ppm", "wb");
  TASK_CHECK(f != nullptr);
  if (f) {
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    const size_t wrote = std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    TASK_CHECK(wrote == img.size());
  }

  const size_t qbytes = qb.getNodes().size() * sizeof(QuantizedNode);
  const size_t sbytes = bvh.getNodes().size() * sizeof(Node<float>);
  TASK_CHECK(qbytes < sbytes);
  TASK_CHECK(qcalls < bcalls);
  std::printf(
      "PPM %dx%d nodes=%zu quantized_node_bytes=%zu float_node_bytes=%zu "
      "quantized_primitive_calls=%u brute_primitive_calls=%u hit_pixels=%u\n",
      W, H, qb.getNodes().size(), qbytes, sbytes, qcalls, bcalls, hit_pixels);
  return 0;
}

}  // namespace

const task_case task_cases[] = {
    {"encoding", "level_decode_float_mixed", encoding_level_decode_float_mixed},
    {"encoding", "level_decode_double_deep", encoding_level_decode_double_deep},
    {"encoding", "empty_and_degenerate", encoding_empty_and_degenerate},
    {"traversal", "closest_matches_bruteforce_float", traversal_closest_matches_bruteforce_float},
    {"traversal", "occlusion_flags_double", traversal_occlusion_flags_double},
    {"traversal", "edge_rays_signed_zero", traversal_edge_rays_signed_zero},
    {"integration", "source_destroyed", integration_source_destroyed},
    {"integration", "borrowed_primitive_identity", integration_borrowed_primitive_identity},
    {"integration", "ppm_preview", integration_ppm_preview},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
