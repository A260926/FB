//! \file editable_mesh.cpp
//! \brief EditableMesh demo: renders a mesh before and after an interactive
//! style vertex update, writing before.ppm / after.ppm, and verifies every
//! pixel against an independent brute-force ray reference.
//!
//! Usage: editable_mesh <output_dir>

#include <FastBVH.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

using FastBVH::EditableMesh;
using FastBVH::Ray;
using Vec3 = FastBVH::Vector3<double>;
using FaceIndices = EditableMesh::FaceIndices;

//! Grid resolution (vertices per side).
constexpr int kGrid = 24;
//! Image resolution.
constexpr int kRes = 200;

//! The height field of the grid: wavy on the west half, flat on the east half.
double wave(int i, int j) { return i < 12 ? 0.3 * std::sin(0.8 * i) * std::cos(0.7 * j) : 0.0; }

uint32_t vid(int i, int j) { return (uint32_t)(i + kGrid * j); }

//! \brief Independent ray reference: brute-force Möller–Trumbore scan over
//! every face (no BVH involved).
bool bruteClosest(const std::vector<Vec3>& vs, const std::vector<FaceIndices>& fs, const Ray<double>& ray,
                  uint32_t& face, double& t) {
  bool hit = false;
  double best = 1e300;
  uint32_t bf = 0;
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
    const double u = FastBVH::dot(tv, p) * inv;
    if (u < 0.0 || u > 1.0) continue;
    const Vec3 q = FastBVH::cross(tv, e1);
    const double v = FastBVH::dot(ray.d, q) * inv;
    if (v < 0.0 || u + v > 1.0) continue;
    const double tt = FastBVH::dot(e2, q) * inv;
    if (tt < 0.0) continue;
    if (tt < best) {
      best = tt;
      bf = i;
      hit = true;
    }
  }
  if (hit) {
    face = bf;
    t = best;
  }
  return hit;
}

//! \brief Reference t for one specific face (used to validate the face the
//! production path reports, even on exact shared-edge ties).
bool referenceTForFace(const std::vector<Vec3>& vs, const FaceIndices& tri, const Ray<double>& ray, double& t) {
  const Vec3& a = vs[tri[0]];
  const Vec3& b = vs[tri[1]];
  const Vec3& c = vs[tri[2]];
  const Vec3 e1 = b - a;
  const Vec3 e2 = c - a;
  const Vec3 p = FastBVH::cross(ray.d, e2);
  const double det = FastBVH::dot(e1, p);
  if (det == 0.0) return false;
  const double inv = 1.0 / det;
  const Vec3 tv = ray.o - a;
  const double u = FastBVH::dot(tv, p) * inv;
  if (u < 0.0 || u > 1.0) return false;
  const Vec3 q = FastBVH::cross(tv, e1);
  const double v = FastBVH::dot(ray.d, q) * inv;
  if (v < 0.0 || u + v > 1.0) return false;
  const double tt = FastBVH::dot(e2, q) * inv;
  if (tt < 0.0) return false;
  t = tt;
  return true;
}

//! \brief Renders the mesh through the production closest() entry, checks
//! every pixel against the independent reference, and writes the PPM.
bool renderAndCheck(const EditableMesh& mesh, const char* path) {
  std::vector<unsigned char> pixels(3 * kRes * kRes);
  unsigned char* px = pixels.data();
  for (int y = 0; y < kRes; ++y) {
    for (int x = 0; x < kRes; ++x) {
      const double ox = -1.31 + 25.62 * (x + 0.5) / kRes;
      const double oy = -1.31 + 25.62 * (y + 0.5) / kRes;
      const Ray<double> ray(Vec3{ox, oy, 10.0}, Vec3{0.0, 0.0, -1.0});

      EditableMesh::RayHit hit;
      const bool got = mesh.closest(ray, hit);

      // Independent per-pixel reference.
      uint32_t rface = 0;
      double rt = 0.0;
      const bool want = bruteClosest(mesh.vertices(), mesh.faces(), ray, rface, rt);
      if (got != want) {
        std::fprintf(stderr, "pixel (%d,%d): production hit=%d reference hit=%d\n", x, y, (int)got, (int)want);
        return false;
      }
      if (got) {
        const double tol = 1e-9 * (1.0 + std::fabs(rt));
        if (std::fabs(hit.t - rt) > tol) {
          std::fprintf(stderr, "pixel (%d,%d): t mismatch production=%.17g reference=%.17g\n", x, y, hit.t, rt);
          return false;
        }
        // The reported face must achieve the reference distance itself
        // (exact shared-edge ties may select a different, equally near face).
        double ft = 0.0;
        if (!referenceTForFace(mesh.vertices(), mesh.faces()[hit.face], ray, ft) ||
            std::fabs(ft - rt) > tol) {
          std::fprintf(stderr, "pixel (%d,%d): face %u inconsistent with reference\n", x, y, hit.face);
          return false;
        }
      }

      if (got) {
        const double shade = std::min(1.0, std::max(0.0, (11.0 - hit.t) / 3.0));
        px[0] = (unsigned char)(40 + 200 * shade);
        px[1] = (unsigned char)((hit.face * 37) % 256);
        px[2] = (unsigned char)(220 - 180 * shade);
      } else {
        px[0] = 10;
        px[1] = 10;
        px[2] = 20;
      }
      px += 3;
    }
  }

  std::ofstream out(path, std::ios::binary);
  if (!out.good()) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return false;
  }
  out << "P6\n" << kRes << "\n" << kRes << "\n255\n";
  out.write((const char*)pixels.data(), (std::streamsize)pixels.size());
  return out.good();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: editable_mesh <output_dir>\n");
    return 1;
  }
  const std::string dir = argv[1];

  // A 24x24 vertex grid, wavy on the west half and flat on the east half.
  std::vector<Vec3> verts;
  std::vector<FaceIndices> faces;
  verts.reserve(kGrid * kGrid);
  for (int j = 0; j < kGrid; ++j) {
    for (int i = 0; i < kGrid; ++i) {
      verts.push_back(Vec3{(double)i, (double)j, wave(i, j)});
    }
  }
  for (int j = 0; j < kGrid - 1; ++j) {
    for (int i = 0; i < kGrid - 1; ++i) {
      faces.push_back(FaceIndices{vid(i, j), vid(i + 1, j), vid(i + 1, j + 1)});
      faces.push_back(FaceIndices{vid(i, j), vid(i + 1, j + 1), vid(i, j + 1)});
    }
  }

  EditableMesh mesh(verts, faces);
  std::printf("faces=%zu initial_contacts=%zu\n", faces.size(), mesh.contacts().size());

  const std::string before_path = dir + "/before.ppm";
  if (!renderAndCheck(mesh, before_path.c_str())) return 1;

  // One production update with two edits:
  //  * raise a spike in the wavy region (no self-intersections),
  //  * stretch a vertex across the flat region so its fan crosses the
  //    surface (creates self-intersection contacts).
  const std::vector<EditableMesh::VertexMove> moves = {
      EditableMesh::VertexMove{vid(4, 12), Vec3{4.0, 12.0, wave(4, 12) + 3.0}},
      EditableMesh::VertexMove{vid(6, 6), Vec3{16.0, 6.0, 0.0}},
  };
  const auto result = mesh.update_vertices(moves);
  if (!result.accepted) {
    std::fprintf(stderr, "update rejected: %s (vertex %u)\n", result.error, result.error_vertex);
    return 1;
  }
  std::printf("update: added=%zu removed=%zu contacts=%zu\n", result.added.size(), result.removed.size(),
              mesh.contacts().size());
  std::printf("work: leaves=%llu internal=%llu node_checks=%llu face_box_checks=%llu narrow=%llu\n",
              (unsigned long long)result.stats.leaves_recomputed,
              (unsigned long long)result.stats.internal_nodes_recomputed,
              (unsigned long long)result.stats.node_pair_checks,
              (unsigned long long)result.stats.face_box_checks,
              (unsigned long long)result.stats.triangle_pair_tests);
  for (const auto& c : mesh.contacts()) {
    std::printf("contact faces=(%u,%u) kind=%s\n", c.face_a, c.face_b, FastBVH::contact_kind_name(c.kind));
  }
  if (mesh.contacts().empty()) {
    std::fprintf(stderr, "expected self-intersection contacts after the update\n");
    return 1;
  }

  const std::string after_path = dir + "/after.ppm";
  if (!renderAndCheck(mesh, after_path.c_str())) return 1;

  std::printf("wrote %s and %s\n", before_path.c_str(), after_path.c_str());
  return 0;
}
