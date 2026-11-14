// Independent checks for the uv_baker implementation.
//
// The reference math below deliberately does not call uv_baker helpers:
//   * barycentric coordinates are recomputed from the UV triangle by hand,
//   * the top-left UV ownership rule is re-implemented from its definition,
//   * occlusion is answered by brute-force Moller-Trumbore ray/triangle
//     intersection over all world triangles in double precision,
//   * the tangent frame is recomputed from the axis-selection definition.
//
// Covered cases: tangent frame, plane, near occluding plate, concave corner,
// mirrored UV winding, shared UV edges, interior UV overlap rejection,
// zero-UV-area vs zero-world-area handling, narrow charts, two-island dilation
// competition and tie-break, non-cascading dilation, reversed traversal,
// input validation, input immutability, repeat bakes / size changes, and the
// generated ao.ppm / bent.ppm files.
//
// Usage: target <scratch-directory>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "baker.h"
#include "examples/uv_raster/uv_surface.h"

namespace {

int g_failures = 0;
int g_checks = 0;
const char *g_context = "";

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL[%s] %s:%d: %s\n", g_context, __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                              \
  do {                                                                     \
    ++g_checks;                                                            \
    const double va = static_cast<double>(a);                              \
    const double vb = static_cast<double>(b);                              \
    if (!(std::fabs(va - vb) <= (eps))) {                                  \
      std::fprintf(stderr, "FAIL[%s] %s:%d: %s=%.17g vs %s=%.17g (eps %g)\n", g_context, \
                   __FILE__, __LINE__, #a, va, #b, vb,                     \
                   static_cast<double>(eps));                              \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

struct Vec3 {
  double x, y, z;
};
Vec3 sub(const Vec3 &a, const Vec3 &b) {
  return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 cross(const Vec3 &a, const Vec3 &b) {
  return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x};
}
double dot(const Vec3 &a, const Vec3 &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
double len(const Vec3 &a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(const Vec3 &a) {
  const double l = len(a);
  return l > 0.0 ? Vec3{a.x / l, a.y / l, a.z / l} : Vec3{0.0, 0.0, 0.0};
}

// ---------------------------------------------------------------------------
// Independent reference implementations
// ---------------------------------------------------------------------------

bool RefBarycentric(const float uv[6], double pu, double pv, double w[3]) {
  const Vec3 A{uv[0], uv[1], 0.0};
  const Vec3 B{uv[2], uv[3], 0.0};
  const Vec3 C{uv[4], uv[5], 0.0};
  const double den = (B.y - C.y) * (A.x - C.x) + (C.x - B.x) * (A.y - C.y);
  if (den == 0.0) return false;
  const double l0 =
      ((B.y - C.y) * (pu - C.x) + (C.x - B.x) * (pv - C.y)) / den;
  const double l1 =
      ((C.y - A.y) * (pu - C.x) + (A.x - C.x) * (pv - C.y)) / den;
  const double l2 = 1.0 - l0 - l1;
  w[0] = l0;
  w[1] = l1;
  w[2] = l2;
  return true;
}

Vec3 RefPosition(const float *positions, const unsigned int *indices, int face,
                 const double w[3]) {
  const unsigned int ids[3] = {indices[3 * face + 0], indices[3 * face + 1],
                               indices[3 * face + 2]};
  Vec3 p{0.0, 0.0, 0.0};
  for (int c = 0; c < 3; ++c) {
    const float *v = positions + 3 * static_cast<std::size_t>(ids[c]);
    p.x += w[c] * v[0];
    p.y += w[c] * v[1];
    p.z += w[c] * v[2];
  }
  return p;
}

// The independent top-left rule: normalize the triangle to CCW (positive
// signed area), then a texel center on an edge is owned only when that edge
// points along -U (top) or, if not horizontal, along -V (left).
bool RefEdgeOwned(const float uv[6], int edge, double orient) {
  const double u0 = uv[2 * edge + 0];
  const double v0 = uv[2 * edge + 1];
  const double u1 = uv[2 * ((edge + 1) % 3) + 0];
  const double v1 = uv[2 * ((edge + 1) % 3) + 1];
  const double du = orient * (u1 - u0);
  const double dv = orient * (v1 - v0);
  const double elen = std::sqrt(du * du + dv * dv);
  if (elen <= 0.0) return false;
  if (std::fabs(dv) <= 1.0e-7 * elen) return du < 0.0;
  return dv < 0.0;
}

// Returns 1 covered, 0 uncovered, -1 if it would need overlap resolution.
int RefSingleCovered(const float uv[6], double pu, double pv, bool *owned_edge,
                     double *edge_dist) {
  const double area = (uv[2] - uv[0]) * (uv[5] - uv[1]) -
                      (uv[3] - uv[1]) * (uv[4] - uv[0]);
  const double orient = area >= 0.0 ? 1.0 : -1.0;
  bool strict = true;
  bool owned = false;
  bool any_edge = false;
  for (int e = 0; e < 3; ++e) {
    const double u0 = uv[2 * e + 0];
    const double v0 = uv[2 * e + 1];
    const double u1 = uv[2 * ((e + 1) % 3) + 0];
    const double v1 = uv[2 * ((e + 1) % 3) + 1];
    const double du = u1 - u0;
    const double dv = v1 - v0;
    const double elen = std::sqrt(du * du + dv * dv);
    const double d = elen > 0.0
                         ? orient * (du * (pv - v0) - dv * (pu - u0)) / elen
                         : 0.0;
    if (edge_dist) edge_dist[e] = d;
    if (d < -1.0e-7) return 0;
    if (d <= 1.0e-7) {
      strict = false;
      any_edge = true;
      if (RefEdgeOwned(uv, e, orient)) owned = true;
    }
  }
  if (owned_edge) *owned_edge = any_edge;
  return (strict || owned) ? 1 : 0;
}

// Moller-Trumbore in double precision; back faces count as hits.
double RefRayTriangle(const Vec3 &org, const Vec3 &dir, const Vec3 &p0,
                      const Vec3 &p1, const Vec3 &p2) {
  const Vec3 e1 = sub(p1, p0);
  const Vec3 e2 = sub(p2, p0);
  const Vec3 pvec = cross(dir, e2);
  const double det = dot(e1, pvec);
  if (std::fabs(det) < 1.0e-14) return -1.0;
  const double inv_det = 1.0 / det;
  const Vec3 tvec = sub(org, p0);
  const double u = dot(tvec, pvec) * inv_det;
  if (u < 0.0 || u > 1.0) return -1.0;
  const Vec3 qvec = cross(tvec, e1);
  const double v = dot(dir, qvec) * inv_det;
  if (v < 0.0 || u + v > 1.0) return -1.0;
  return dot(e2, qvec) * inv_det;
}

void RefFrame(const Vec3 &n, Vec3 *t, Vec3 *b) {
  int axis = 0;
  double best = std::fabs(n.x);
  if (std::fabs(n.y) < best) {
    axis = 1;
    best = std::fabs(n.y);
  }
  if (std::fabs(n.z) < best) {
    axis = 2;
  }
  Vec3 a{0.0, 0.0, 0.0};
  if (axis == 0) a.x = 1.0;
  if (axis == 1) a.y = 1.0;
  if (axis == 2) a.z = 1.0;
  *t = normalize(cross(a, n));
  *b = cross(n, *t);
}

void RefBake(const std::vector<float> &positions,
             const std::vector<unsigned int> &indices,
             const std::vector<float> &dirs, int face, const double bary[3],
             double radius, double bias, double *visibility, Vec3 *bent,
             Vec3 *surface, Vec3 *normal_out) {
  const Vec3 p = RefPosition(positions.data(), indices.data(), face, bary);
  const unsigned int ids[3] = {indices[3 * face + 0], indices[3 * face + 1],
                               indices[3 * face + 2]};
  const Vec3 p0{positions[3 * ids[0] + 0], positions[3 * ids[0] + 1],
                positions[3 * ids[0] + 2]};
  const Vec3 p1{positions[3 * ids[1] + 0], positions[3 * ids[1] + 1],
                positions[3 * ids[1] + 2]};
  const Vec3 p2{positions[3 * ids[2] + 0], positions[3 * ids[2] + 1],
                positions[3 * ids[2] + 2]};
  const Vec3 n = normalize(cross(sub(p1, p0), sub(p2, p0)));
  Vec3 t, b;
  RefFrame(n, &t, &b);
  const Vec3 org{p.x + bias * n.x, p.y + bias * n.y, p.z + bias * n.z};

  const std::size_t num_faces = indices.size() / 3;
  const std::size_t num_dirs = dirs.size() / 3;
  std::size_t open = 0;
  Vec3 sum{0.0, 0.0, 0.0};
  for (std::size_t d = 0; d < num_dirs; ++d) {
    const Vec3 local{dirs[3 * d + 0], dirs[3 * d + 1], dirs[3 * d + 2]};
    const Vec3 w = normalize(
        Vec3{t.x * local.x + b.x * local.y + n.x * local.z,
             t.y * local.x + b.y * local.y + n.y * local.z,
             t.z * local.x + b.z * local.y + n.z * local.z});
    bool hit = false;
    for (std::size_t f = 0; f < num_faces; ++f) {
      if (f == static_cast<std::size_t>(face)) continue;
      const Vec3 q0{positions[3 * indices[3 * f + 0] + 0],
                    positions[3 * indices[3 * f + 0] + 1],
                    positions[3 * indices[3 * f + 0] + 2]};
      const Vec3 q1{positions[3 * indices[3 * f + 1] + 0],
                    positions[3 * indices[3 * f + 1] + 1],
                    positions[3 * indices[3 * f + 1] + 2]};
      const Vec3 q2{positions[3 * indices[3 * f + 2] + 0],
                    positions[3 * indices[3 * f + 2] + 1],
                    positions[3 * indices[3 * f + 2] + 2]};
      const double tt = RefRayTriangle(org, w, q0, q1, q2);
      if (tt >= 0.0 && tt < radius) {
        hit = true;
        break;
      }
    }
    if (!hit) {
      ++open;
      sum.x += w.x;
      sum.y += w.y;
      sum.z += w.z;
    }
  }
  *visibility = static_cast<double>(open) / static_cast<double>(num_dirs);
  *bent = open > 0 ? normalize(sum) : Vec3{0.0, 0.0, 0.0};
  *surface = p;
  *normal_out = n;
}

// Verify a covered texel against the independent reference.
void CheckTexelAgainstReference(const std::vector<float> &positions,
                                const std::vector<unsigned int> &indices,
                                const std::vector<float> &uvs,
                                const std::vector<float> &dirs,
                                const uv_baker::Result &r, int width,
                                int height, int x, int y, double radius,
                                double bias, bool check_bary = true) {
  const std::size_t i = static_cast<std::size_t>(y) * width + x;
  CHECK(r.real_coverage[i] == 1);
  const int face = r.face_ids[i];
  CHECK(face >= 0 && face < static_cast<int>(indices.size() / 3));

  const double pu = (x + 0.5) / static_cast<double>(width);
  const double pv = (y + 0.5) / static_cast<double>(height);
  double w[3];
  CHECK(RefBarycentric(uvs.data() + 6 * face, pu, pv, w));

  // Barycentric output matches the independent computation.
  if (check_bary) {
    // NanoRT reports (u, v) = (w1, w2).
    CHECK_NEAR(r.barycentric_u[i], w[1], 1e-5);
    CHECK_NEAR(r.barycentric_v[i], w[2], 1e-5);
  }

  double vis = 0.0;
  Vec3 bent, surface, normal;
  (void)uvs;
  RefBake(positions, indices, dirs, face, w, radius, bias, &vis, &bent,
          &surface, &normal);
  CHECK_NEAR(r.visibility[i], vis, 1e-5);
  CHECK_NEAR(r.bent_normals[3 * i + 0], bent.x, 1e-5);
  CHECK_NEAR(r.bent_normals[3 * i + 1], bent.y, 1e-5);
  CHECK_NEAR(r.bent_normals[3 * i + 2], bent.z, 1e-5);

  // The reconstructed surface point (via the shared module) must match the
  // independent barycentric position, so texel -> world surface is consistent.
  uv_surface::SurfaceSample sample;
  CHECK(uv_surface::ReconstructSample(positions.data(), positions.size(),
                                      indices.data(), indices.size(), nullptr,
                                      0, static_cast<unsigned int>(face),
                                      static_cast<float>(w[1]),
                                      static_cast<float>(w[2]), &sample));
  CHECK_NEAR(sample.position[0], surface.x, 2e-5);
  CHECK_NEAR(sample.position[1], surface.y, 2e-5);
  CHECK_NEAR(sample.position[2], surface.z, 2e-5);
  CHECK_NEAR(sample.geometric_normal[0], normal.x, 2e-5);
  CHECK_NEAR(sample.geometric_normal[1], normal.y, 2e-5);
  CHECK_NEAR(sample.geometric_normal[2], normal.z, 2e-5);

  // Sanity on the range and on the bent-normal direction.
  CHECK(r.visibility[i] >= 0.0f && r.visibility[i] <= 1.0f);
  if (vis == 1.0) {
    CHECK_NEAR(len(bent), 1.0, 1e-6);
  } else if (vis == 0.0) {
    CHECK_NEAR(r.bent_normals[3 * i + 0], 0.0f, 1e-6);
    CHECK_NEAR(r.bent_normals[3 * i + 1], 0.0f, 1e-6);
    CHECK_NEAR(r.bent_normals[3 * i + 2], 0.0f, 1e-6);
  } else {
    CHECK_NEAR(std::sqrt(r.bent_normals[3 * i + 0] * r.bent_normals[3 * i + 0] +
                         r.bent_normals[3 * i + 1] * r.bent_normals[3 * i + 1] +
                         r.bent_normals[3 * i + 2] * r.bent_normals[3 * i + 2]),
               1.0, 1e-5);
  }
}

// ---------------------------------------------------------------------------
// Scene construction
// ---------------------------------------------------------------------------

struct Scene {
  std::vector<float> positions;
  std::vector<unsigned int> indices;
  std::vector<float> uvs;
  std::vector<int> charts;
};

void AddTriangle(Scene *s, const float p[3][3], const float uv[3][2], int chart) {
  const unsigned int base = static_cast<unsigned int>(s->positions.size() / 3);
  for (int i = 0; i < 3; ++i) {
    s->positions.push_back(p[i][0]);
    s->positions.push_back(p[i][1]);
    s->positions.push_back(p[i][2]);
    s->uvs.push_back(uv[i][0]);
    s->uvs.push_back(uv[i][1]);
  }
  s->indices.push_back(base + 0);
  s->indices.push_back(base + 1);
  s->indices.push_back(base + 2);
  s->charts.push_back(chart);
}

void AddQuad(Scene *s, const float p[4][3], const float uv[4][2], int chart) {
  float t0[3][3], t1[3][3], u0[3][2], u1[3][2];
  const int tri0[3] = {0, 1, 2};
  const int tri1[3] = {0, 2, 3};
  for (int c = 0; c < 3; ++c) {
    for (int k = 0; k < 3; ++k) {
      t0[c][k] = p[tri0[c]][k];
      t1[c][k] = p[tri1[c]][k];
    }
    for (int k = 0; k < 2; ++k) {
      u0[c][k] = uv[tri0[c]][k];
      u1[c][k] = uv[tri1[c]][k];
    }
  }
  AddTriangle(s, t0, u0, chart);
  AddTriangle(s, t1, u1, chart);
}

uv_baker::Input MakeInput(const Scene &s, const std::vector<float> &dirs) {
  uv_baker::Input in;
  in.positions = s.positions.data();
  in.position_count = s.positions.size();
  in.triangle_indices = s.indices.data();
  in.triangle_index_count = s.indices.size();
  in.face_uvs = s.uvs.data();
  in.face_uv_count = s.uvs.size();
  in.face_chart_ids = s.charts.data();
  in.face_chart_id_count = s.charts.size();
  in.directions = dirs.data();
  in.direction_count = dirs.size();
  return in;
}

std::vector<float> UnitDirections() {
  const float raw[][3] = {
      {0.0f, 0.0f, 1.0f},
      {0.70710678f, 0.0f, 0.70710678f},
      {-0.70710678f, 0.0f, 0.70710678f},
      {0.0f, 0.70710678f, 0.70710678f},
      {0.0f, -0.70710678f, 0.70710678f},
      {0.57735027f, 0.57735027f, 0.57735027f},
      {0.0f, 1.0f, 0.0f},
  };
  std::vector<float> dirs;
  for (const auto &d : raw) {
    dirs.push_back(d[0]);
    dirs.push_back(d[1]);
    dirs.push_back(d[2]);
  }
  return dirs;
}

double RadicalInverse2(unsigned int i) {
  i = (i << 16u) | (i >> 16u);
  i = ((i & 0x55555555u) << 1u) | ((i & 0xAAAAAAAAu) >> 1u);
  i = ((i & 0x33333333u) << 2u) | ((i & 0xCCCCCCCCu) >> 2u);
  i = ((i & 0x0F0F0F0Fu) << 4u) | ((i & 0xF0F0F0F0u) >> 4u);
  i = ((i & 0x00FF00FFu) << 8u) | ((i & 0xFF00FF00u) >> 8u);
  return static_cast<double>(i) * 2.3283064365386963e-10;
}

std::vector<float> MakeCosineDirections(unsigned int count) {
  std::vector<float> dirs;
  dirs.reserve(3 * count);
  for (unsigned int i = 0; i < count; ++i) {
    const double u1 = (i + 0.5) / static_cast<double>(count);
    const double u2 = RadicalInverse2(i);
    const double rr = std::sqrt(u1);
    const double phi = 2.0 * 3.14159265358979323846 * u2;
    const double x = rr * std::cos(phi);
    const double y = rr * std::sin(phi);
    const double z = std::sqrt(std::max(0.0, 1.0 - u1));
    const double l = std::sqrt(x * x + y * y + z * z);
    dirs.push_back(static_cast<float>(x / l));
    dirs.push_back(static_cast<float>(y / l));
    dirs.push_back(static_cast<float>(z / l));
  }
  return dirs;
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

void CheckTangentFrame() {
  // For every test normal verify unit length, orthogonality, T x B = N, and
  // that the chosen axis is the one with the smallest |dot| (ties prefer X,
  // then Y).
  const float normals[][3] = {
      {0.0f, 0.0f, 1.0f},
      {0.0f, 0.0f, -1.0f},
      {0.5f, 0.5f, 0.70710678f},   // |x| = |y| < |z|: X wins the tie
      {0.0f, 0.70710678f, 0.70710678f},
      {0.57735027f, 0.57735027f, 0.57735027f},
      {0.38268343f, 0.92387953f, 0.0f},
  };
  for (const auto &n : normals) {
    const Vec3 N{n[0], n[1], n[2]};
    float t[3], b[3];
    uv_baker::ComputeTangentFrame(n, t, b);
    const Vec3 T{t[0], t[1], t[2]};
    const Vec3 B{b[0], b[1], b[2]};
    CHECK_NEAR(len(T), 1.0, 1e-6);
    CHECK_NEAR(len(B), 1.0, 1e-6);
    CHECK_NEAR(dot(T, N), 0.0, 1e-6);
    CHECK_NEAR(dot(B, N), 0.0, 1e-6);
    CHECK_NEAR(dot(T, B), 0.0, 1e-6);
    const Vec3 tb = cross(T, B);
    CHECK_NEAR(tb.x, N.x, 1e-6);
    CHECK_NEAR(tb.y, N.y, 1e-6);
    CHECK_NEAR(tb.z, N.z, 1e-6);

    // Independent axis choice and T = normalize(cross(A, N)).
    Vec3 rt, rb;
    RefFrame(N, &rt, &rb);
    CHECK_NEAR(T.x, rt.x, 1e-6);
    CHECK_NEAR(T.y, rt.y, 1e-6);
    CHECK_NEAR(T.z, rt.z, 1e-6);
    CHECK_NEAR(B.x, rb.x, 1e-6);
    CHECK_NEAR(B.y, rb.y, 1e-6);
    CHECK_NEAR(B.z, rb.z, 1e-6);
  }

  // The local +Z axis must map to the world normal.
  const float local_up[3] = {0.0f, 0.0f, 1.0f};
  const float local_zero[3] = {0.0f, 0.0f, 0.0f};
  float world[3];
  CHECK(uv_baker::LocalDirectionToWorld(normals[2], local_up, world));
  CHECK_NEAR(world[0], normals[2][0], 1e-6);
  CHECK_NEAR(world[1], normals[2][1], 1e-6);
  CHECK_NEAR(world[2], normals[2][2], 1e-6);
  CHECK(!uv_baker::LocalDirectionToWorld(normals[0], local_zero, world));
}

void CheckPlane() {
  Scene s;
  const float p[3][3] = {{-1.0f, -1.0f, 0.0f}, {2.0f, -1.0f, 0.0f},
                         {-1.0f, 2.0f, 0.0f}};
  const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
  AddTriangle(&s, p, uv, 7);

  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = 8;
  opt.height = 8;
  opt.radius = 2.0f;
  opt.bias = 0.0f;
  uv_baker::Result r;
  std::string err;
  CHECK(uv_baker::Bake(in, opt, &r, &err));

  std::size_t covered = 0;
  std::size_t expected = 0;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * 8 + x;
      const double pu = (x + 0.5) / 8.0;
      const double pv = (y + 0.5) / 8.0;
      double w[3];
      CHECK(RefBarycentric(s.uvs.data(), pu, pv, w));
      const int ref_covered = RefSingleCovered(s.uvs.data(), pu, pv, nullptr,
                                               nullptr);
      CHECK(ref_covered >= 0);
      if (ref_covered) ++expected;

      if (!r.real_coverage[i]) {
        CHECK(ref_covered == 0);
        continue;
      }
      CHECK(ref_covered == 1);
      CHECK(r.face_ids[i] == 0);
      CHECK(r.chart_ids[i] == 7);
      ++covered;
      CheckTexelAgainstReference(s.positions, s.indices, s.uvs, dirs, r, 8, 8,
                                 x, y, opt.radius, opt.bias);
      // Nothing occludes the lone triangle: full visibility.  (The bent
      // normal is the normalized sum of the unoccluded directions, not
      // necessarily the geometric normal, so only its length is asserted.)
      CHECK_NEAR(r.visibility[i], 1.0f, 1e-6);
      CHECK_NEAR(std::sqrt(r.bent_normals[3 * i + 0] * r.bent_normals[3 * i + 0] +
                           r.bent_normals[3 * i + 1] * r.bent_normals[3 * i + 1] +
                           r.bent_normals[3 * i + 2] * r.bent_normals[3 * i + 2]),
                 1.0, 1e-5);
      CHECK(r.bent_normals[3 * i + 2] > 0.0);  // still in the +Z hemisphere
    }
  }
  CHECK(covered == expected);
  CHECK(covered > 0);
  CHECK(expected == 28);  // 36 half-plane centers minus 8 on the diagonal
}

void CheckOccludingPlate() {
  // Floor plus an occluding plate at z = 0.5 facing -Z.  radius = 0.6 makes
  // the straight-up ray occluded while the 45-degree rays escape.
  Scene s;
  {
    const float p[4][3] = {{-2.0f, -2.0f, 0.0f},
                           {2.0f, -2.0f, 0.0f},
                           {2.0f, 2.0f, 0.0f},
                           {-2.0f, 2.0f, 0.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.5f, 0.5f},
                            {0.0f, 0.5f}};
    AddQuad(&s, p, uv, 0);
  }
  {
    // Reversed winding so the geometric normal is -Z.
    const float p[4][3] = {{-0.5f, -0.5f, 0.5f},
                           {-0.5f, 0.5f, 0.5f},
                           {0.5f, 0.5f, 0.5f},
                           {0.5f, -0.5f, 0.5f}};
    const float uv[4][2] = {{0.5f, 0.0f}, {0.5f, 0.5f}, {1.0f, 0.5f},
                            {1.0f, 0.0f}};
    AddQuad(&s, p, uv, 1);
  }

  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = 16;
  opt.height = 16;
  opt.radius = 0.6f;
  opt.bias = 1.0e-3f;
  uv_baker::Result r;
  std::string err;
  CHECK(uv_baker::Bake(in, opt, &r, &err));

  std::size_t floor_covered = 0;
  std::size_t floor_occluded = 0;
  std::size_t floor_open = 0;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * 16 + x;
      if (!r.real_coverage[i]) continue;
      if (r.chart_ids[i] != 0) continue;
      ++floor_covered;
      CheckTexelAgainstReference(s.positions, s.indices, s.uvs, dirs, r, 16, 16,
                                 x, y, opt.radius, opt.bias);
      if (r.visibility[i] < 1.0f) ++floor_occluded;
      if (r.visibility[i] == 1.0f) ++floor_open;
      const double pu = (x + 0.5) / 16.0;
      const double pv = (y + 0.5) / 16.0;
      const double du = pu - 0.25;
      const double dv = pv - 0.25;
      const double d = std::sqrt(du * du + dv * dv);
      // Well under the plate -> the vertical ray is blocked.
      if (d < 0.05) CHECK(r.visibility[i] < 1.0f);
      // Well outside the plate -> every ray escapes within radius 0.6.
      if (d > 0.15) CHECK_NEAR(r.visibility[i], 1.0f, 1e-6);
    }
  }
  CHECK(floor_covered > 0);
  CHECK(floor_occluded > 0);
  CHECK(floor_open > 0);

  // The plate's local +Z hemisphere points at the floor, so its bent normal
  // must point downward.  Its sample values are covered by the reference
  // comparison above as well.
  std::size_t plate_covered = 0;
  for (std::size_t i = 0; i < r.real_coverage.size(); ++i) {
    if (!r.real_coverage[i] || r.chart_ids[i] != 1) continue;
    ++plate_covered;
    CHECK(r.visibility[i] < 1.0f);
    CHECK(r.bent_normals[3 * i + 2] < 0.0f);
    const double bl =
        std::sqrt(r.bent_normals[3 * i + 0] * r.bent_normals[3 * i + 0] +
                  r.bent_normals[3 * i + 1] * r.bent_normals[3 * i + 1] +
                  r.bent_normals[3 * i + 2] * r.bent_normals[3 * i + 2]);
    CHECK_NEAR(bl, 1.0, 1e-5);
  }
  CHECK(plate_covered > 0);

  // The plate shares no vertex with the floor, so also verify a shared-vertex
  // occluder: an extra triangle sharing one floor vertex still blocks rays
  // from texels under it (only the source face is skipped).
  Scene shared;
  {
    const float p[3][3] = {{-2.0f, -2.0f, 0.0f},
                           {2.0f, -2.0f, 0.0f},
                           {0.0f, 2.0f, 0.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    AddTriangle(&shared, p, uv, 0);
  }
  {
    // Shares the world vertex (2, -2, 0) with the floor triangle.  Its UVs
    // live in the unused u+v > 1 half of the texture, so this is not a UV
    // overlap.
    const float p[3][3] = {{2.0f, -2.0f, 0.0f},
                           {1.0f, -1.5f, 0.8f},
                           {2.0f, -1.0f, 0.8f}};
    const float uv[3][2] = {{0.6f, 0.6f}, {0.95f, 0.6f}, {0.7f, 0.95f}};
    AddTriangle(&shared, p, uv, 1);
  }
  uv_baker::Input in2 = MakeInput(shared, dirs);
  uv_baker::Options opt2;
  opt2.width = 12;
  opt2.height = 12;
  opt2.radius = 2.0f;
  opt2.bias = 0.0f;
  uv_baker::Result r2;
  CHECK(uv_baker::Bake(in2, opt2, &r2, &err));
  std::size_t shared_vertex_occluded = 0;
  for (std::size_t i = 0; i < r2.real_coverage.size(); ++i) {
    if (!r2.real_coverage[i] || r2.chart_ids[i] != 0) continue;
    CheckTexelAgainstReference(shared.positions, shared.indices, shared.uvs,
                               dirs, r2, 12, 12,
                               static_cast<int>(i % 12),
                               static_cast<int>(i / 12), opt2.radius, opt2.bias);
    if (r2.visibility[i] < 1.0f) ++shared_vertex_occluded;
  }
  CHECK(shared_vertex_occluded > 0);
}

void CheckConcaveCornerAndMirroredUV() {
  Scene s;
  {
    // Floor at z = 0, facing +Z, chart 0.
    const float p[4][3] = {{-1.0f, -2.0f, 0.0f},
                           {1.0f, -2.0f, 0.0f},
                           {1.0f, 0.0f, 0.0f},
                           {-1.0f, 0.0f, 0.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.5f, 0.5f},
                            {0.0f, 0.5f}};
    AddQuad(&s, p, uv, 0);
  }
  {
    // Wall at y = 0, facing -Y, chart 1.  Deliberately reversed UV winding.
    const float p[4][3] = {{-1.0f, 0.0f, 0.0f},
                           {1.0f, 0.0f, 0.0f},
                           {1.0f, 0.0f, 2.0f},
                           {-1.0f, 0.0f, 2.0f}};
    const float uv[4][2] = {{0.5f, 0.0f}, {0.5f, 0.5f}, {1.0f, 0.5f},
                            {1.0f, 0.0f}};
    AddQuad(&s, p, uv, 1);
  }

  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = 16;
  opt.height = 16;
  opt.radius = 2.0f;
  opt.bias = 1.0e-3f;
  uv_baker::Result r;
  std::string err;
  CHECK(uv_baker::Bake(in, opt, &r, &err));

  // Floor and wall must both be present.
  bool saw_floor = false, saw_wall = false;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * 16 + x;
      if (!r.real_coverage[i]) continue;
      if (r.chart_ids[i] == 0) saw_floor = true;
      if (r.chart_ids[i] == 1) saw_wall = true;
      CheckTexelAgainstReference(s.positions, s.indices, s.uvs, dirs, r, 16, 16,
                                 x, y, opt.radius, opt.bias);
    }
  }
  CHECK(saw_floor);
  CHECK(saw_wall);

  // Reversed (mirrored) UV winding of the wall must rasterize the same UV
  // region.  The reflected layout keeps the u range inside [0.5, 1], so the
  // union coverage and chart attribution at each texel must be identical; the
  // per-face barycentric/world mapping legitimately differs because the UV
  // corners are re-paired, so values are not compared here.
  Scene mirrored = s;
  for (std::size_t f = 2; f < mirrored.charts.size(); ++f) {
    for (int c = 0; c < 3; ++c) {
      mirrored.uvs[6 * f + 2 * c + 0] =
          1.5f - mirrored.uvs[6 * f + 2 * c + 0];
    }
  }
  uv_baker::Input in2 = MakeInput(mirrored, dirs);
  uv_baker::Result r2;
  CHECK(uv_baker::Bake(in2, opt, &r2, &err));
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * 16 + x;
      CHECK(r.real_coverage[i] == r2.real_coverage[i]);
      if (!r.real_coverage[i]) continue;
      CHECK(r.chart_ids[i] == r2.chart_ids[i]);
    }
  }

  // Two single-triangle scenes with opposite UV winding over the same UV
  // region: coverage must be identical (the top-left rule is normalized by
  // orientation) even though the world-to-UV pairing differs.
  {
    Scene a, b;
    const float p[3][3] = {{0.0f, 0.0f, 0.0f},
                           {2.0f, 0.0f, 0.0f},
                           {0.0f, 2.0f, 0.0f}};
    const float uv_ccw[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    const float uv_cw[3][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f}};
    AddTriangle(&a, p, uv_ccw, 4);
    AddTriangle(&b, p, uv_cw, 4);
    uv_baker::Options o = opt;
    o.width = 10;
    o.height = 10;
    o.radius = 1.0f;
    o.bias = 0.0f;
    uv_baker::Result ra, rb;
    uv_baker::Input ia = MakeInput(a, dirs);
    uv_baker::Input ib = MakeInput(b, dirs);
    CHECK(uv_baker::Bake(ia, o, &ra, &err));
    CHECK(uv_baker::Bake(ib, o, &rb, &err));
    for (std::size_t i = 0; i < ra.real_coverage.size(); ++i) {
      CHECK(ra.real_coverage[i] == rb.real_coverage[i]);
      if (!ra.real_coverage[i]) continue;
      CHECK(ra.chart_ids[i] == 4);
      CHECK(rb.chart_ids[i] == 4);
    }
  }
}

void CheckSharedEdgeNoOverlap() {
  // One quad split into two triangles sharing a diagonal UV edge.  The shared
  // edge must resolve without throwing an overlap error, and both faces must
  // win somewhere.
  Scene s;
  const float p[4][3] = {{-1.0f, -1.0f, 0.0f},
                         {1.0f, -1.0f, 0.0f},
                         {1.0f, 1.0f, 0.0f},
                         {-1.0f, 1.0f, 0.0f}};
  const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f},
                          {0.0f, 1.0f}};
  AddQuad(&s, p, uv, 0);

  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = 23;  // odd so centers land exactly on the diagonal
  opt.height = 23;
  opt.radius = 1.0f;
  // The two triangles are coplanar and the diagonal texels put the ray origin
  // exactly on the shared edge.  A small positive bias avoids the inherently
  // ill-conditioned float/double grazing-hit tie at t == 0; the reference
  // still uses double precision below.
  opt.bias = 1.0e-3f;
  uv_baker::Result r;
  std::string err;
  CHECK(uv_baker::Bake(in, opt, &r, &err));
  CHECK(err.empty());

  std::size_t covered = 0;
  bool has0 = false, has1 = false;
  for (int y = 0; y < 23; ++y) {
    for (int x = 0; x < 23; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * 23 + x;
      // Every center is strictly inside the quad; the diagonal belongs to
      // face 0.
      CHECK(r.real_coverage[i]);
      ++covered;
      CHECK(r.face_ids[i] == 0 || r.face_ids[i] == 1);
      CHECK(r.chart_ids[i] == 0);
      if (r.face_ids[i] == 0) has0 = true;
      if (r.face_ids[i] == 1) has1 = true;
      // Every covered texel, including those on the shared diagonal, must
      // match the independent reference (which skips only the source face).
      CheckTexelAgainstReference(s.positions, s.indices, s.uvs, dirs, r, 23,
                                 23, x, y, opt.radius, opt.bias);
    }
  }
  CHECK(has0);
  CHECK(has1);
  CHECK(covered == 23 * 23);
}

void CheckOppositeWindingSharedEdge() {
  // Two triangles that share a UV edge but use opposite winding conventions
  // (the second one lists its corners in reverse).  The bake must not report
  // an overlap, and the shared edge must have exactly one owner.
  Scene s;
  {
    const float p[3][3] = {{-1.0f, -1.0f, 0.0f},
                           {1.0f, -1.0f, 0.0f},
                           {-1.0f, 1.0f, 0.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    AddTriangle(&s, p, uv, 0);
  }
  {
    // Other half of the same UV square, sharing the (1,0)-(0,1) diagonal.
    // Its corners are listed with reversed UV winding on a non-coplanar
    // world triangle.
    const float p[3][3] = {{1.0f, 0.0f, 0.5f},
                           {0.0f, 1.0f, 0.5f},
                           {1.0f, 1.0f, 0.5f}};
    const float uv[3][2] = {{1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    AddTriangle(&s, p, uv, 1);
  }
  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = 11;
  opt.height = 11;
  opt.radius = 1.0f;
  opt.bias = 1.0e-3f;
  uv_baker::Result r;
  std::string err;
  CHECK(uv_baker::Bake(in, opt, &r, &err));
  CHECK(err.empty());
  std::size_t covered = 0;
  for (int y = 0; y < 11; ++y) {
    for (int x = 0; x < 11; ++x) {
      const std::size_t idx = static_cast<std::size_t>(y) * 11 + x;
      if (!r.real_coverage[idx]) continue;
      ++covered;
      CHECK(r.chart_ids[idx] == 0 || r.chart_ids[idx] == 1);
    }
  }
  CHECK(covered > 0);
}

void CheckDuplicateUVRejected() {
  // Two faces with the exact same UV triangle (a duplicated chart).  Every
  // interior texel is claimed twice; the bake must report an overlap rather
  // than pick a face.
  Scene s;
  const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
  const float world_a[3][3] = {{-2.0f, -2.0f, 0.0f},
                               {2.0f, -2.0f, 0.0f},
                               {0.0f, 2.0f, 0.0f}};
  const float world_b[3][3] = {{-2.0f, -2.0f, 1.0f},
                               {2.0f, -2.0f, 1.0f},
                               {0.0f, 2.0f, 1.0f}};
  AddTriangle(&s, world_a, uv, 0);
  AddTriangle(&s, world_b, uv, 1);
  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = 16;
  opt.height = 16;
  opt.radius = 1.0f;
  uv_baker::Result r;
  std::string err;
  CHECK(!uv_baker::Bake(in, opt, &r, &err));
  CHECK(!err.empty());
}

void CheckOverlapRejected() {
  Scene s;
  {
    const float p[3][3] = {{-1.0f, -1.0f, 0.0f},
                           {1.0f, -1.0f, 0.0f},
                           {-1.0f, 1.0f, 0.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    AddTriangle(&s, p, uv, 0);
  }
  {
    const float p[3][3] = {{-1.0f, -1.0f, 1.0f},
                           {1.0f, -1.0f, 1.0f},
                           {-1.0f, 1.0f, 1.0f}};
    // Interior overlap, no shared boundary segment.
    const float uv[3][2] = {{0.2f, 0.2f}, {1.0f, 0.2f}, {0.2f, 1.0f}};
    AddTriangle(&s, p, uv, 1);
  }

  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = 16;
  opt.height = 16;
  opt.radius = 2.0f;
  uv_baker::Result r;
  std::string err;

  // Clear the result first so "stays empty after failure" is meaningful.
  uv_baker::Options warm = opt;
  warm.width = 4;
  warm.height = 4;
  uv_baker::Result scratch;
  Scene plane;
  {
    const float p[3][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    AddTriangle(&plane, p, uv, 0);
  }
  uv_baker::Input warm_in = MakeInput(plane, dirs);
  CHECK(uv_baker::Bake(warm_in, warm, &scratch, &err));
  CHECK(scratch.real_coverage.size() == 16);

  CHECK(!uv_baker::Bake(in, opt, &scratch, &err));
  CHECK(!err.empty());
  CHECK(scratch.real_coverage.empty());
  CHECK(scratch.visibility.empty());
  CHECK(scratch.width == 0 && scratch.height == 0);
}

void CheckZeroUVAreaAndZeroWorldArea() {
  const std::vector<float> dirs = UnitDirections();
  uv_baker::Options opt;
  opt.width = 8;
  opt.height = 8;
  opt.radius = 1.0f;
  uv_baker::Result r;
  std::string err;

  // Zero UV area is ignored; zero world area is rejected.
  Scene s;
  {
    const float p[3][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f}};
    const float uv[3][2] = {{0.2f, 0.2f}, {0.4f, 0.2f}, {0.6f, 0.2f}};
    AddTriangle(&s, p, uv, 0);
  }
  {
    const float p[3][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                           {2.0f, 0.0f, 0.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {0.2f, 0.0f}, {0.0f, 0.2f}};
    AddTriangle(&s, p, uv, 1);
  }
  uv_baker::Input in = MakeInput(s, dirs);
  CHECK(!uv_baker::Bake(in, opt, &r, &err));
  CHECK(!err.empty());

  // With only the zero-UV-area face, the bake succeeds with empty coverage.
  Scene deg;
  {
    const float p[3][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f}};
    const float uv[3][2] = {{0.2f, 0.2f}, {0.4f, 0.2f}, {0.6f, 0.2f}};
    AddTriangle(&deg, p, uv, 0);
  }
  uv_baker::Input in2 = MakeInput(deg, dirs);
  CHECK(uv_baker::Bake(in2, opt, &r, &err));
  CHECK(r.width == 8 && r.height == 8);
  for (std::size_t i = 0; i < r.real_coverage.size(); ++i) {
    CHECK(!r.real_coverage[i]);
    CHECK(!r.has_value[i]);
    CHECK(r.face_ids[i] == -1);
    CHECK(r.chart_ids[i] == -1);
  }
}

void CheckEmptyCoverage() {
  // Zero faces, zero UVs and zero charts is valid empty coverage, not an
  // error.  Unused world vertices are also fine.
  const std::vector<float> dirs = UnitDirections();
  uv_baker::Options opt;
  opt.width = 7;
  opt.height = 5;
  opt.radius = 1.0f;
  opt.bias = 0.0f;
  uv_baker::Result r;
  std::string err;

  uv_baker::Input empty;
  empty.directions = dirs.data();
  empty.direction_count = dirs.size();
  CHECK(uv_baker::Bake(empty, opt, &r, &err));
  CHECK(err.empty());
  CHECK(r.width == 7 && r.height == 5);
  CHECK(r.real_coverage.size() == 35);
  for (std::size_t i = 0; i < r.real_coverage.size(); ++i) {
    CHECK(!r.real_coverage[i]);
    CHECK(!r.has_value[i]);
    CHECK(r.face_ids[i] == -1);
    CHECK(r.bent_normals[3 * i + 2] == 0.0f);
  }

  // Unused trailing vertices are allowed.
  Scene s;
  {
    const float p[3][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    AddTriangle(&s, p, uv, 0);
  }
  s.positions.push_back(9.0f);
  s.positions.push_back(9.0f);
  s.positions.push_back(9.0f);
  uv_baker::Input in = MakeInput(s, dirs);
  CHECK(uv_baker::Bake(in, opt, &r, &err));
}

void CheckValidation() {
  Scene s;
  {
    const float p[3][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    AddTriangle(&s, p, uv, 0);
  }
  const std::vector<float> dirs = UnitDirections();
  uv_baker::Options opt;
  opt.width = 8;
  opt.height = 8;
  opt.radius = 1.0f;
  uv_baker::Result r;
  std::string err;

  {
    Scene bad = s;
    bad.indices[1] = 99;
    uv_baker::Input in = MakeInput(bad, dirs);
    CHECK(!uv_baker::Bake(in, opt, &r, &err));
  }
  {
    Scene bad = s;
    bad.positions[0] = std::numeric_limits<float>::quiet_NaN();
    uv_baker::Input in = MakeInput(bad, dirs);
    CHECK(!uv_baker::Bake(in, opt, &r, &err));
  }
  {
    Scene bad = s;
    bad.positions[2] = std::numeric_limits<float>::infinity();
    uv_baker::Input in = MakeInput(bad, dirs);
    CHECK(!uv_baker::Bake(in, opt, &r, &err));
  }
  {
    Scene bad = s;
    bad.uvs[2] = std::numeric_limits<float>::infinity();
    uv_baker::Input in = MakeInput(bad, dirs);
    CHECK(!uv_baker::Bake(in, opt, &r, &err));
  }
  {
    std::vector<float> bad = dirs;
    bad[1] = std::numeric_limits<float>::quiet_NaN();
    uv_baker::Input in = MakeInput(s, bad);
    CHECK(!uv_baker::Bake(in, opt, &r, &err));
  }
  {
    std::vector<float> bad = dirs;
    bad[0] = 0.5f;
    bad[1] = 0.5f;
    bad[2] = 0.5f;
    uv_baker::Input in = MakeInput(s, bad);
    CHECK(!uv_baker::Bake(in, opt, &r, &err));
  }
  {
    std::vector<float> bad = dirs;
    bad[2] = -1.0f;
    uv_baker::Input in = MakeInput(s, bad);
    CHECK(!uv_baker::Bake(in, opt, &r, &err));
  }
  {
    std::vector<float> empty;
    uv_baker::Input in = MakeInput(s, empty);
    CHECK(!uv_baker::Bake(in, opt, &r, &err));
  }
  {
    uv_baker::Input in = MakeInput(s, dirs);
    uv_baker::Options o = opt;
    o.width = 0;
    CHECK(!uv_baker::Bake(in, o, &r, &err));
    o = opt;
    o.height = -3;
    CHECK(!uv_baker::Bake(in, o, &r, &err));
  }
  {
    uv_baker::Input in = MakeInput(s, dirs);
    uv_baker::Options o = opt;
    o.radius = 0.0f;
    CHECK(!uv_baker::Bake(in, o, &r, &err));
    o.radius = -1.0f;
    CHECK(!uv_baker::Bake(in, o, &r, &err));
    o.radius = std::numeric_limits<float>::infinity();
    CHECK(!uv_baker::Bake(in, o, &r, &err));
    o.radius = std::numeric_limits<float>::quiet_NaN();
    CHECK(!uv_baker::Bake(in, o, &r, &err));
  }
  {
    uv_baker::Input in = MakeInput(s, dirs);
    uv_baker::Options o = opt;
    o.bias = -0.1f;
    CHECK(!uv_baker::Bake(in, o, &r, &err));
    o.bias = std::numeric_limits<float>::quiet_NaN();
    CHECK(!uv_baker::Bake(in, o, &r, &err));
  }
  {
    uv_baker::Input in = MakeInput(s, dirs);
    uv_baker::Options o = opt;
    o.dilation_radius = -1;
    CHECK(!uv_baker::Bake(in, o, &r, &err));
  }
  {
    uv_baker::Input in = MakeInput(s, dirs);
    in.face_uv_count -= 1;
    CHECK(!uv_baker::Bake(in, opt, &r, &err));
  }
  {
    uv_baker::Input in = MakeInput(s, dirs);
    in.face_chart_id_count = 0;
    CHECK(!uv_baker::Bake(in, opt, &r, &err));
  }
  {
    uv_baker::Input in = MakeInput(s, dirs);
    in.direction_count = 4;  // not a multiple of 3
    CHECK(!uv_baker::Bake(in, opt, &r, &err));
  }
  // A valid bake afterwards must work and carry no stale state.
  {
    uv_baker::Input in = MakeInput(s, dirs);
    CHECK(uv_baker::Bake(in, opt, &r, &err));
    std::size_t covered = 0;
    for (std::size_t i = 0; i < r.real_coverage.size(); ++i) {
      if (r.real_coverage[i]) ++covered;
    }
    CHECK(covered > 0);
  }
}

void CheckNarrowChartsAndDilation() {
  // Two one-quad wide vertical islands separated by a 5-texel gap.  Chart 2 is
  // on the left, chart 5 on the right.
  Scene s;
  {
    const float p[4][3] = {{-3.0f, -3.0f, 0.0f},
                           {-1.0f, -3.0f, 0.0f},
                           {-1.0f, 3.0f, 0.0f},
                           {-3.0f, 3.0f, 0.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {2.0f / 9.0f, 0.0f},
                            {2.0f / 9.0f, 1.0f}, {0.0f, 1.0f}};
    AddQuad(&s, p, uv, 2);
  }
  {
    const float p[4][3] = {{1.0f, -3.0f, 0.0f},
                           {3.0f, -3.0f, 0.0f},
                           {3.0f, 3.0f, 0.0f},
                           {1.0f, 3.0f, 0.0f}};
    const float uv[4][2] = {{7.0f / 9.0f, 0.0f}, {1.0f, 0.0f},
                            {1.0f, 1.0f}, {7.0f / 9.0f, 1.0f}};
    AddQuad(&s, p, uv, 5);
  }

  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = 9;
  opt.height = 5;
  opt.radius = 1.0f;
  opt.bias = 0.0f;

  // radius 2: texels x in {0,1} and {7,8} are real.  Column 4 is 3 away from
  // every real source, so it must stay empty (filled texels are never
  // sources); columns 2,3,5,6 are filled.
  {
    uv_baker::Options o = opt;
    o.dilation_radius = 2;
    uv_baker::Result r;
    std::string err;
    CHECK(uv_baker::Bake(in, o, &r, &err));
    for (int y = 0; y < 5; ++y) {
      CHECK(r.real_coverage[static_cast<std::size_t>(y) * 9 + 0]);
      CHECK(r.real_coverage[static_cast<std::size_t>(y) * 9 + 1]);
      CHECK(r.real_coverage[static_cast<std::size_t>(y) * 9 + 7]);
      CHECK(r.real_coverage[static_cast<std::size_t>(y) * 9 + 8]);
      CHECK(!r.real_coverage[static_cast<std::size_t>(y) * 9 + 4]);
      CHECK(!r.has_value[static_cast<std::size_t>(y) * 9 + 4]);
      CHECK(!r.dilated[static_cast<std::size_t>(y) * 9 + 4]);

      const std::size_t x2 = static_cast<std::size_t>(y) * 9 + 2;
      CHECK(r.has_value[x2] && r.dilated[x2] && !r.real_coverage[x2]);
      CHECK(r.chart_ids[x2] == 2);
      const std::size_t x3 = static_cast<std::size_t>(y) * 9 + 3;
      CHECK(r.has_value[x3] && r.dilated[x3]);
      CHECK(r.chart_ids[x3] == 2);
      const std::size_t x5 = static_cast<std::size_t>(y) * 9 + 5;
      CHECK(r.has_value[x5] && r.dilated[x5]);
      CHECK(r.chart_ids[x5] == 5);
      const std::size_t x6 = static_cast<std::size_t>(y) * 9 + 6;
      CHECK(r.has_value[x6] && r.dilated[x6]);
      CHECK(r.chart_ids[x6] == 5);
    }

    // The fill must copy every number and attribution of its source exactly.
    // (x=3, y=2) has source (x=1, y=2).
    const std::size_t dst = 2u * 9u + 3u;
    const std::size_t src = 2u * 9u + 1u;
    CHECK(r.real_coverage[src]);
    CHECK(r.face_ids[dst] == r.face_ids[src]);
    CHECK(r.chart_ids[dst] == r.chart_ids[src]);
    CHECK_NEAR(r.barycentric_u[dst], r.barycentric_u[src], 0.0);
    CHECK_NEAR(r.barycentric_v[dst], r.barycentric_v[src], 0.0);
    CHECK_NEAR(r.visibility[dst], r.visibility[src], 0.0);
    CHECK_NEAR(r.bent_normals[3 * dst + 0], r.bent_normals[3 * src + 0], 0.0);
    CHECK_NEAR(r.bent_normals[3 * dst + 1], r.bent_normals[3 * src + 1], 0.0);
    CHECK_NEAR(r.bent_normals[3 * dst + 2], r.bent_normals[3 * src + 2], 0.0);
    CHECK(r.applied_dilation_radius == 2);
  }

  // radius 3: column 4 is equidistant (3) from x=1 (chart 2) and x=7
  // (chart 5); the documented tie-break by chart id picks chart 2, and by
  // source y/x among same-chart ties for other rows.
  {
    uv_baker::Options o = opt;
    o.dilation_radius = 3;
    uv_baker::Result r;
    std::string err;
    CHECK(uv_baker::Bake(in, o, &r, &err));
    for (int y = 0; y < 5; ++y) {
      const std::size_t x4 = static_cast<std::size_t>(y) * 9 + 4;
      CHECK(r.has_value[x4]);
      CHECK(!r.real_coverage[x4]);
      CHECK(r.chart_ids[x4] == 2);
      const std::size_t src = static_cast<std::size_t>(y) * 9 + 1;
      CHECK(r.face_ids[x4] == r.face_ids[src]);
      CHECK_NEAR(r.visibility[x4], r.visibility[src], 0.0);
    }
  }

  // Re-bake at a different size with dilation disabled: no stale values.
  {
    uv_baker::Options o = opt;
    o.dilation_radius = 0;
    o.width = 11;
    o.height = 7;
    uv_baker::Result r;
    std::string err;
    CHECK(uv_baker::Bake(in, o, &r, &err));
    CHECK(r.width == 11 && r.height == 7);
    CHECK(r.visibility.size() == 77);
    CHECK(r.bent_normals.size() == 3 * 77);
    for (std::size_t i = 0; i < r.real_coverage.size(); ++i) {
      CHECK(r.dilated[i] == 0);
      if (!r.real_coverage[i]) {
        CHECK(r.face_ids[i] == -1);
        CHECK(r.chart_ids[i] == -1);
        CHECK_NEAR(r.visibility[i], 0.0f, 0.0);
        CHECK_NEAR(r.bent_normals[3 * i + 0], 0.0f, 0.0);
        CHECK_NEAR(r.bent_normals[3 * i + 1], 0.0f, 0.0);
        CHECK_NEAR(r.bent_normals[3 * i + 2], 0.0f, 0.0);
      }
    }
    CHECK(r.applied_dilation_radius == 0);
  }
}

void CheckSubTexelNarrowChart() {
  // A chart narrower than one texel in u and tall in v.  It must still cover
  // exactly the texel column containing its center line and nothing else.
  Scene s;
  const float p[4][3] = {{-1.0f, -1.0f, 0.0f},
                         {1.0f, -1.0f, 0.0f},
                         {1.0f, 1.0f, 0.0f},
                         {-1.0f, 1.0f, 0.0f}};
  // Width 0.02 (well under one texel at width 32), straddling the center of
  // texel column 15 (center u = 15.5 / 32 = 0.484375).
  const float u0 = 0.47f;
  const float u1 = 0.49f;
  const float uv[4][2] = {{u0, 0.0f}, {u1, 0.0f}, {u1, 1.0f}, {u0, 1.0f}};
  AddQuad(&s, p, uv, 0);
  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = 32;
  opt.height = 32;
  opt.radius = 1.0f;
  opt.bias = 0.0f;
  uv_baker::Result r;
  std::string err;
  CHECK(uv_baker::Bake(in, opt, &r, &err));

  std::size_t covered = 0;
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * 32 + x;
      if (!r.real_coverage[i]) continue;
      ++covered;
      // Only texel column 15 contains a center inside the narrow strip.
      CHECK(x == 15);
      CHECK(r.chart_ids[i] == 0);
    }
  }
  CHECK(covered == 32);
}

void CheckDilationSourceOrdering() {
  // Two isolated one-texel islands on the same chart, at (2,2) and (2,6) in a
  // 10 x 10 texture.  The texel at (2,4) is equidistant from both, so the
  // tie-break must use the smaller source y, independent of scan order.
  Scene s;
  {
    const float p[3][3] = {{0.0f, 0.0f, 0.0f}, {0.1f, 0.0f, 0.0f},
                           {0.0f, 0.1f, 0.0f}};
    const float uv[3][2] = {{0.24f, 0.24f}, {0.27f, 0.24f},
                            {0.24f, 0.27f}};
    AddTriangle(&s, p, uv, 3);
  }
  {
    const float p[3][3] = {{0.0f, 0.0f, 1.0f}, {0.1f, 0.0f, 1.0f},
                           {0.0f, 0.1f, 1.0f}};
    const float uv[3][2] = {{0.24f, 0.64f}, {0.27f, 0.64f},
                            {0.24f, 0.67f}};
    AddTriangle(&s, p, uv, 3);
  }
  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = 10;
  opt.height = 10;
  opt.radius = 0.5f;
  opt.bias = 0.0f;
  opt.dilation_radius = 3;
  uv_baker::Result r;
  std::string err;
  CHECK(uv_baker::Bake(in, opt, &r, &err));

  const std::size_t lower = static_cast<std::size_t>(2) * 10 + 2;
  const std::size_t upper = static_cast<std::size_t>(6) * 10 + 2;
  CHECK(r.real_coverage[lower]);
  CHECK(r.real_coverage[upper]);
  const std::size_t mid = static_cast<std::size_t>(4) * 10 + 2;
  CHECK(r.has_value[mid]);
  CHECK(!r.real_coverage[mid]);
  // Equal distance; the smaller source y must win, i.e. copy `lower`.
  CHECK(r.chart_ids[mid] == 3);
  CHECK(r.face_ids[mid] == r.face_ids[lower]);
  CHECK_NEAR(r.barycentric_u[mid], r.barycentric_u[lower], 0.0);
  CHECK_NEAR(r.barycentric_v[mid], r.barycentric_v[lower], 0.0);
  CHECK_NEAR(r.visibility[mid], r.visibility[lower], 0.0);
  CHECK_NEAR(r.bent_normals[3 * mid + 0], r.bent_normals[3 * lower + 0], 0.0);
  CHECK_NEAR(r.bent_normals[3 * mid + 1], r.bent_normals[3 * lower + 1], 0.0);
  CHECK_NEAR(r.bent_normals[3 * mid + 2], r.bent_normals[3 * lower + 2], 0.0);

  // A filled texel may never act as a source.  (2,8) is distance 2 from the
  // real source at (2,6), and would be distance 2 from a hypothetical fill at
  // (2,7) only if fills cascaded; verify the value still equals (2,6).
  const std::size_t far = static_cast<std::size_t>(8) * 10 + 2;
  CHECK(r.has_value[far]);
  CHECK_NEAR(r.visibility[far], r.visibility[upper], 0.0);
  // (2,3) is distance 1 from (2,2) and 3 from (2,6): copies the lower.
  const std::size_t near_low = static_cast<std::size_t>(3) * 10 + 2;
  CHECK_NEAR(r.visibility[near_low], r.visibility[lower], 0.0);
}

void CheckFullyOccluded() {
  // A small floor patch completely enclosed by a five-sided box.  Every ray
  // in the +Z hemisphere hits a wall or the top within radius 3, so the patch
  // must report visibility 0 and an exactly zero bent normal.
  Scene s;
  {
    const float p[3][3] = {{-0.05f, -0.05f, 0.0f}, {0.05f, -0.05f, 0.0f},
                           {0.0f, 0.05f, 0.0f}};
    const float uv[3][2] = {{0.02f, 0.02f}, {0.12f, 0.02f}, {0.02f, 0.12f}};
    AddTriangle(&s, p, uv, 0);
  }
  // Box walls: x = -1 / +1, y = -1 / +1, top z = 2.  Each chart gets its own
  // disjoint UV rectangle so the box faces are rasterized without overlap.
  auto add_wall = [&s](const float p[4][3], int chart) {
    float uv[4][2];
    const float u0 = 0.2f + 0.12f * static_cast<float>(chart - 1);
    const float v0 = 0.75f;
    uv[0][0] = u0;        uv[0][1] = v0;
    uv[1][0] = u0 + 0.08f; uv[1][1] = v0;
    uv[2][0] = u0 + 0.08f; uv[2][1] = v0 + 0.08f;
    uv[3][0] = u0;        uv[3][1] = v0 + 0.08f;
    AddQuad(&s, p, uv, chart);
  };
  {
    const float p[4][3] = {{-1.0f, -1.0f, 0.0f},
                           {-1.0f, 1.0f, 0.0f},
                           {-1.0f, 1.0f, 2.0f},
                           {-1.0f, -1.0f, 2.0f}};
    add_wall(p, 1);
  }
  {
    const float p[4][3] = {{1.0f, -1.0f, 0.0f},
                           {1.0f, -1.0f, 2.0f},
                           {1.0f, 1.0f, 2.0f},
                           {1.0f, 1.0f, 0.0f}};
    add_wall(p, 2);
  }
  {
    const float p[4][3] = {{-1.0f, -1.0f, 0.0f},
                           {-1.0f, -1.0f, 2.0f},
                           {1.0f, -1.0f, 2.0f},
                           {1.0f, -1.0f, 0.0f}};
    add_wall(p, 3);
  }
  {
    const float p[4][3] = {{-1.0f, 1.0f, 0.0f},
                           {1.0f, 1.0f, 0.0f},
                           {1.0f, 1.0f, 2.0f},
                           {-1.0f, 1.0f, 2.0f}};
    add_wall(p, 4);
  }
  {
    const float p[4][3] = {{-1.0f, -1.0f, 2.0f},
                           {-1.0f, 1.0f, 2.0f},
                           {1.0f, 1.0f, 2.0f},
                           {1.0f, -1.0f, 2.0f}};
    add_wall(p, 5);
  }

  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = 16;
  opt.height = 16;
  opt.radius = 3.0f;
  opt.bias = 0.0f;
  uv_baker::Result r;
  std::string err;
  CHECK(uv_baker::Bake(in, opt, &r, &err));
  std::size_t covered = 0;
  std::size_t fully = 0;
  for (std::size_t i = 0; i < r.real_coverage.size(); ++i) {
    if (!r.real_coverage[i] || r.chart_ids[i] != 0) continue;
    ++covered;
    CHECK_NEAR(r.visibility[i], 0.0f, 0.0);
    ++fully;
    CHECK_NEAR(r.bent_normals[3 * i + 0], 0.0f, 0.0);
    CHECK_NEAR(r.bent_normals[3 * i + 1], 0.0f, 0.0);
    CHECK_NEAR(r.bent_normals[3 * i + 2], 0.0f, 0.0);
  }
  CHECK(covered > 0);
  CHECK(fully == covered);
}

void CheckRayOriginUsesBias() {
  // Two otherwise identical bakes with different biases must differ when a
  // thin occluder sits between the two origin positions, proving the origin
  // is P + bias*N and not a shared/previous texel position.
  Scene s;
  {
    const float p[4][3] = {{-1.0f, -1.0f, 0.0f},
                           {1.0f, -1.0f, 0.0f},
                           {1.0f, 1.0f, 0.0f},
                           {-1.0f, 1.0f, 0.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.5f, 0.5f},
                            {0.0f, 0.5f}};
    AddQuad(&s, p, uv, 0);
  }
  {
    // Thin plate at z = 0.25 over the floor center (occludes upward rays for
    // radius 0.5 when bias is below it).
    const float p[4][3] = {{-0.4f, -0.4f, 0.25f},
                           {-0.4f, 0.4f, 0.25f},
                           {0.4f, 0.4f, 0.25f},
                           {0.4f, -0.4f, 0.25f}};
    const float uv[4][2] = {{0.5f, 0.0f}, {0.5f, 0.5f}, {1.0f, 0.5f},
                            {1.0f, 0.0f}};
    AddQuad(&s, p, uv, 1);
  }
  const std::vector<float> dirs = UnitDirections();
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options low;
  low.width = 16;
  low.height = 16;
  low.radius = 0.5f;
  low.bias = 0.0f;
  uv_baker::Options high = low;
  high.bias = 0.26f;  // origin above the thin plate

  uv_baker::Result rl, rh;
  std::string err;
  CHECK(uv_baker::Bake(in, low, &rl, &err));
  CHECK(uv_baker::Bake(in, high, &rh, &err));

  std::size_t changed = 0;
  for (std::size_t i = 0; i < rl.real_coverage.size(); ++i) {
    if (!rl.real_coverage[i] || rl.chart_ids[i] != 0) continue;
    const double pu = (i % 16 + 0.5) / 16.0;
    const double pv = (i / 16 + 0.5) / 16.0;
    const double du = pu - 0.25;
    const double dv = pv - 0.25;
    if (std::sqrt(du * du + dv * dv) > 0.12) continue;
    CHECK(rl.visibility[i] < 1.0f);
    // Above the plate the same rays escape (the plate is below the origin).
    CHECK_NEAR(rh.visibility[i], 1.0f, 1e-6);
    ++changed;
  }
  CHECK(changed > 0);
}

void CheckReversedTraversal() {
  // A floor and a plate, exercised with reversed face order and reversed
  // direction order; results must match face-for-face and texel-for-texel.
  Scene s;
  {
    const float p[4][3] = {{-2.0f, -2.0f, 0.0f},
                           {2.0f, -2.0f, 0.0f},
                           {2.0f, 2.0f, 0.0f},
                           {-2.0f, 2.0f, 0.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.5f, 0.5f},
                            {0.0f, 0.5f}};
    AddQuad(&s, p, uv, 0);
  }
  {
    const float p[4][3] = {{-0.5f, -0.5f, 0.5f},
                           {-0.5f, 0.5f, 0.5f},
                           {0.5f, 0.5f, 0.5f},
                           {0.5f, -0.5f, 0.5f}};
    const float uv[4][2] = {{0.5f, 0.0f}, {0.5f, 0.5f}, {1.0f, 0.5f},
                            {1.0f, 0.0f}};
    AddQuad(&s, p, uv, 1);
  }

  const std::vector<float> dirs = UnitDirections();
  uv_baker::Options opt;
  opt.width = 13;
  opt.height = 11;
  opt.radius = 2.0f;
  opt.bias = 1.0e-3f;

  const Scene snapshot = s;
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Result a;
  uv_baker::Result b;
  std::string err;
  CHECK(uv_baker::Bake(in, opt, &a, &err));
  CHECK(uv_baker::Bake(in, opt, &b, &err));

  // Inputs are untouched.
  CHECK(s.positions == snapshot.positions);
  CHECK(s.indices == snapshot.indices);
  CHECK(s.uvs == snapshot.uvs);
  CHECK(s.charts == snapshot.charts);

  // Repeat bake is bit-identical.
  CHECK(a.real_coverage == b.real_coverage);
  CHECK(a.dilated == b.dilated);
  CHECK(a.face_ids == b.face_ids);
  CHECK(a.chart_ids == b.chart_ids);
  CHECK(a.barycentric_u == b.barycentric_u);
  CHECK(a.barycentric_v == b.barycentric_v);
  CHECK(a.visibility == b.visibility);
  CHECK(a.bent_normals == b.bent_normals);

  // Reverse the direction order: sums differ only by addition order.
  std::vector<float> rev_dirs;
  const std::size_t nd = dirs.size() / 3;
  for (std::size_t i = 0; i < nd; ++i) {
    const std::size_t src = nd - 1 - i;
    rev_dirs.push_back(dirs[3 * src + 0]);
    rev_dirs.push_back(dirs[3 * src + 1]);
    rev_dirs.push_back(dirs[3 * src + 2]);
  }
  uv_baker::Input inrev = MakeInput(s, rev_dirs);
  uv_baker::Result rr;
  CHECK(uv_baker::Bake(inrev, opt, &rr, &err));
  for (std::size_t i = 0; i < a.real_coverage.size(); ++i) {
    CHECK(a.real_coverage[i] == rr.real_coverage[i]);
    if (!a.real_coverage[i]) continue;
    CHECK_NEAR(a.visibility[i], rr.visibility[i], 1e-6);
    CHECK_NEAR(a.bent_normals[3 * i + 0], rr.bent_normals[3 * i + 0], 1e-6);
    CHECK_NEAR(a.bent_normals[3 * i + 1], rr.bent_normals[3 * i + 1], 1e-6);
    CHECK_NEAR(a.bent_normals[3 * i + 2], rr.bent_normals[3 * i + 2], 1e-6);
  }

  // Reverse world triangle order.  Face ids change, but each texel's chart and
  // surface values must stay the same (the UV layout is unchanged).
  Scene rev;
  std::vector<std::int32_t> old_to_new(s.charts.size(), -1);
  for (std::size_t f = 0; f < s.charts.size(); ++f) {
    const std::size_t src = s.charts.size() - 1 - f;
    old_to_new[src] = static_cast<std::int32_t>(f);
    float p[3][3];
    float uv[3][2];
    for (int c = 0; c < 3; ++c) {
      const unsigned int vi = s.indices[3 * src + c];
      p[c][0] = s.positions[3 * vi + 0];
      p[c][1] = s.positions[3 * vi + 1];
      p[c][2] = s.positions[3 * vi + 2];
      uv[c][0] = s.uvs[6 * src + 2 * c + 0];
      uv[c][1] = s.uvs[6 * src + 2 * c + 1];
    }
    AddTriangle(&rev, p, uv, s.charts[src]);
  }
  uv_baker::Input in2 = MakeInput(rev, dirs);
  uv_baker::Result rb;
  CHECK(uv_baker::Bake(in2, opt, &rb, &err));
  for (std::size_t i = 0; i < a.real_coverage.size(); ++i) {
    CHECK(a.real_coverage[i] == rb.real_coverage[i]);
    if (!a.real_coverage[i]) continue;
    CHECK(a.chart_ids[i] == rb.chart_ids[i]);
    CHECK_NEAR(a.visibility[i], rb.visibility[i], 1e-6);
    CHECK_NEAR(a.bent_normals[3 * i + 0], rb.bent_normals[3 * i + 0], 1e-6);
    CHECK_NEAR(a.bent_normals[3 * i + 1], rb.bent_normals[3 * i + 1], 1e-6);
    CHECK_NEAR(a.bent_normals[3 * i + 2], rb.bent_normals[3 * i + 2], 1e-6);
  }
}

void CheckSharedSurfaceReconstruction() {
  const float positions[] = {0, 0, 0, 2, 0, 0, 0, 2, 1};
  const unsigned int indices[] = {0, 1, 2};
  // Independent geometric normal for the tilted triangle
  // (0,0,0), (2,0,0), (0,2,1):
  //   cross((2,0,0), (0,2,1)) = (0, -2, 4), normalized (0, -1/sqrt5, 2/sqrt5).
  uv_surface::SurfaceSample s;
  CHECK(uv_surface::ReconstructSample(positions, 9, indices, 3, nullptr, 0, 0,
                                      0.25f, 0.5f, &s));
  CHECK_NEAR(s.position[0], 0.5, 1e-6);
  CHECK_NEAR(s.position[1], 1.0, 1e-6);
  CHECK_NEAR(s.position[2], 0.5, 1e-6);
  CHECK_NEAR(s.geometric_normal[0], 0.0, 1e-6);
  CHECK_NEAR(s.geometric_normal[1], -1.0 / std::sqrt(5.0), 1e-6);
  CHECK_NEAR(s.geometric_normal[2], 2.0 / std::sqrt(5.0), 1e-6);

  // Out-of-range face, out-of-range vertex index, zero world area.
  CHECK(!uv_surface::ReconstructSample(positions, 9, indices, 3, nullptr, 0, 1,
                                       0.0f, 0.0f, &s));
  const unsigned int bad_indices[] = {0, 1, 9};
  CHECK(!uv_surface::ReconstructSample(positions, 9, bad_indices, 3, nullptr, 0,
                                       0, 0.0f, 0.0f, &s));
  const float flat[] = {0, 0, 0, 1, 0, 0, 2, 0, 0};
  CHECK(!uv_surface::ReconstructSample(flat, 9, indices, 3, nullptr, 0, 0,
                                       0.0f, 0.0f, &s));

  // The legacy UV mesh builder matches the original uv_raster preparation.
  const float uvs[] = {0, 0, 1, 0, 0, 1};
  std::vector<float> uvp;
  std::vector<unsigned int> uvi;
  std::size_t corners = 0;
  CHECK(uv_surface::BuildUVTriangleMesh(uvs, 6, &uvp, &uvi, &corners));
  CHECK(uvp.size() == 9);
  CHECK(uvi.size() == 3);
  CHECK(corners == 3);
  CHECK_NEAR(uvp[3], 1.0, 0.0);
}

// ---------------------------------------------------------------------------
// PPM generation and verification
// ---------------------------------------------------------------------------

bool WritePPM(const std::string &filename, int width, int height,
              const std::vector<unsigned char> &rgb) {
  FILE *fp = std::fopen(filename.c_str(), "wb");
  if (!fp) return false;
  std::fprintf(fp, "P6\n%d %d\n255\n", width, height);
  const std::size_t written = std::fwrite(rgb.data(), 1, rgb.size(), fp);
  std::fclose(fp);
  return written == rgb.size();
}

// Generate ao.ppm / bent.ppm in `dir` and assert they actually contain both
// occluded and unoccluded surface.
void GenerateAndCheckPPMs(const std::string &dir) {
  const int width = 64;
  const int height = 64;

  Scene s;
  {
    // Floor chart 0: 8 x 8 units, so its outer texels are far from the
    // occluder and stay open with radius 2.5.
    const float p[4][3] = {{-4.0f, -4.0f, 0.0f},
                           {4.0f, -4.0f, 0.0f},
                           {4.0f, 4.0f, 0.0f},
                           {-4.0f, 4.0f, 0.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.5f, 0.5f},
                            {0.0f, 0.5f}};
    AddQuad(&s, p, uv, 0);
  }
  {
    // Small near plate chart 1 (upper-left UV quadrant) at z = 0.5.
    const float p[4][3] = {{-0.5f, -0.5f, 0.5f},
                           {-0.5f, 0.5f, 0.5f},
                           {0.5f, 0.5f, 0.5f},
                           {0.5f, -0.5f, 0.5f}};
    const float uv[4][2] = {{0.0f, 0.5f}, {0.0f, 1.0f}, {0.5f, 1.0f},
                            {0.5f, 0.5f}};
    AddQuad(&s, p, uv, 1);
  }
  {
    // Back wall chart 2.
    const float p[4][3] = {{-4.0f, 4.0f, 0.0f},
                           {4.0f, 4.0f, 0.0f},
                           {4.0f, 4.0f, 3.0f},
                           {-4.0f, 4.0f, 3.0f}};
    const float uv[4][2] = {{0.5f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.5f},
                            {0.5f, 0.5f}};
    AddQuad(&s, p, uv, 2);
  }
  {
    // Left wall chart 3.
    const float p[4][3] = {{-4.0f, -4.0f, 0.0f},
                           {-4.0f, 4.0f, 0.0f},
                           {-4.0f, 4.0f, 3.0f},
                           {-4.0f, -4.0f, 3.0f}};
    const float uv[4][2] = {{0.5f, 0.5f}, {1.0f, 0.5f}, {1.0f, 1.0f},
                            {0.5f, 1.0f}};
    AddQuad(&s, p, uv, 3);
  }

  const std::vector<float> dirs = MakeCosineDirections(64);
  uv_baker::Input in = MakeInput(s, dirs);
  uv_baker::Options opt;
  opt.width = width;
  opt.height = height;
  opt.radius = 2.5f;
  opt.bias = 1.0e-3f;
  opt.dilation_radius = 1;

  uv_baker::Result r;
  std::string err;
  CHECK(uv_baker::Bake(in, opt, &r, &err));

  std::vector<unsigned char> ao(3u * width * height);
  std::vector<unsigned char> bent(3u * width * height);
  std::size_t occluded = 0;
  std::size_t unoccluded = 0;
  std::size_t covered = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t dst = static_cast<std::size_t>(y) * width + x;
      const std::size_t src =
          static_cast<std::size_t>(height - 1 - y) * width + x;
      const float vis = r.has_value[src] ? r.visibility[src] : 0.0f;
      const unsigned char g =
          static_cast<unsigned char>(255.0f * vis + 0.5f);
      ao[3 * dst + 0] = g;
      ao[3 * dst + 1] = g;
      ao[3 * dst + 2] = g;
      for (int c = 0; c < 3; ++c) {
        const float n =
            r.has_value[src] ? r.bent_normals[3 * src + c] : 0.0f;
        const float mapped = 0.5f * n + 0.5f;
        bent[3 * dst + c] = static_cast<unsigned char>(
            255.0f * std::min(1.0f, std::max(0.0f, mapped)) + 0.5f);
      }
      if (r.real_coverage[src]) {
        ++covered;
        if (vis < 1.0f) {
          ++occluded;
        } else {
          ++unoccluded;
        }
      }
    }
  }
  CHECK(covered > 0);
  CHECK(occluded > 0);
  CHECK(unoccluded > 0);

  const std::string ao_path = dir + "/ao.ppm";
  const std::string bent_path = dir + "/bent.ppm";
  CHECK(WritePPM(ao_path, width, height, ao));
  CHECK(WritePPM(bent_path, width, height, bent));

  // Read back the AO file and confirm it contains both dark (occluded) and
  // bright (unoccluded) pixels, not merely a narrow gray band.
  std::FILE *fp = std::fopen(ao_path.c_str(), "rb");
  CHECK(fp != nullptr);
  if (fp) {
    int w = 0, h = 0, maxv = 0;
    CHECK(std::fscanf(fp, "P6 %d %d %d", &w, &h, &maxv) == 3);
    CHECK(w == width && h == height && maxv == 255);
    std::vector<unsigned char> readback(3u * width * height);
    const std::size_t got =
        std::fread(readback.data(), 1, readback.size(), fp);
    std::fclose(fp);
    CHECK(got == readback.size());
    std::size_t dark = 0, bright = 0, mid = 0;
    for (std::size_t i = 0; i < readback.size(); i += 3) {
      if (readback[i] < 128) ++dark;
      else if (readback[i] == 255) ++bright;
      else ++mid;
    }
    // Genuinely dark (occluded), genuinely open (fully visible), and
    // intermediate texels must all be present.
    CHECK(dark > 0);
    CHECK(bright > 0);
    CHECK(mid > 0);
  }
}

}  // namespace

int main(int argc, char **argv) {
  const std::string scratch = (argc > 1) ? argv[1] : ".";

  g_context = "tangent"; CheckTangentFrame();
  g_context = "plane"; CheckPlane();
  g_context = "plate"; CheckOccludingPlate();
  g_context = "corner"; CheckConcaveCornerAndMirroredUV();
  g_context = "edge"; CheckSharedEdgeNoOverlap();
  g_context = "opposite-winding"; CheckOppositeWindingSharedEdge();
  g_context = "duplicate"; CheckDuplicateUVRejected();
  g_context = "dilate"; CheckNarrowChartsAndDilation();
  g_context = "narrow-subtexel"; CheckSubTexelNarrowChart();
  g_context = "dilate-order"; CheckDilationSourceOrdering();
  g_context = "fully-occluded"; CheckFullyOccluded();
  g_context = "bias"; CheckRayOriginUsesBias();
  g_context = "overlap"; CheckOverlapRejected();
  g_context = "zeroarea"; CheckZeroUVAreaAndZeroWorldArea();
  g_context = "empty"; CheckEmptyCoverage();
  g_context = "validation"; CheckValidation();
  g_context = "reversed"; CheckReversedTraversal();
  g_context = "shared"; CheckSharedSurfaceReconstruction();
  g_context = "ppm"; GenerateAndCheckPPMs(scratch);

  std::printf("checks=%d failures=%d\n", g_checks, g_failures);
  if (g_failures != 0) {
    std::fprintf(stderr, "TARGET CHECKS FAILED\n");
    return EXIT_FAILURE;
  }
  std::printf("TARGET CHECKS OK\n");
  return EXIT_SUCCESS;
}
