// Test suite for vcg::tri::SplitTextureTiles.
// No main(): task_support/driver.c provides the runner and the TASK_CHECK
// protocol declared in task_support/check.h.

#include "task_support/check.h"
#include "task_support/mesh.hpp"

#include <vcg/complex/algorithms/udim_tiles.h>
#include <vcg/complex/algorithms/refine.h>
#include <wrap/io_trimesh/export_ply.h>
#include <wrap/io_trimesh/import_ply.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using V3 = std::array<double, 3>;
using UV2 = std::array<double, 2>;

// A second instantiation: same components, double 3D coordinates and double
// wedge texture coordinates, proving the template is not bound to UMesh.
struct DTypes : vcg::UsedTypes<vcg::Use<class DVertex>::AsVertexType,
                               vcg::Use<class DFace>::AsFaceType> {};
class DVertex : public vcg::Vertex<DTypes, vcg::vertex::Coord3d, vcg::vertex::Normal3d,
                                   vcg::vertex::Qualityd, vcg::vertex::Color4b,
                                   vcg::vertex::BitFlags> {};
class DFace : public vcg::Face<DTypes, vcg::face::VertexRef, vcg::face::FFAdj,
                                vcg::face::Normal3d, vcg::face::WedgeTexCoord2d,
                                vcg::face::BitFlags> {};
class DMesh : public vcg::tri::TriMesh<std::vector<DVertex>, std::vector<DFace>> {};

constexpr double GEPS = 1e-9;
constexpr double FUV_EPS = 1e-5;

struct TriGeom { std::array<int, 3> v; int tex; std::array<UV2, 3> uv; };

template <class Mesh>
void buildMesh(Mesh &m, const std::vector<V3> &P, const std::vector<TriGeom> &tris,
               const std::vector<std::string> &texNames)
{
  vcg::tri::Allocator<Mesh>::AddVertices(m, P.size());
  vcg::tri::Allocator<Mesh>::AddFaces(m, tris.size());
  m.textures = texNames;
  for (size_t i = 0; i < P.size(); ++i) {
    m.vert[i].P() = typename Mesh::VertexType::CoordType(P[i][0], P[i][1], P[i][2]);
    m.vert[i].Q() = P[i][0] + 2.0 * P[i][1] + 0.5 * P[i][2];
    int r = int(std::lround(200.0 * (P[i][0] + 1.0))) & 255;
    int g = int(std::lround(120.0 * (P[i][1] + 1.0))) & 255;
    int b = int(std::lround(60.0 * (P[i][2] + 1.0))) & 255;
    m.vert[i].C() = vcg::Color4b(static_cast<unsigned char>(r),
                                 static_cast<unsigned char>(g),
                                 static_cast<unsigned char>(b), 255);
    m.vert[i].N() = typename Mesh::VertexType::NormalType(0, 0, 1);
  }
  for (size_t f = 0; f < tris.size(); ++f) {
    for (int k = 0; k < 3; ++k) {
      m.face[f].V(k) = &m.vert[tris[f].v[k]];
      m.face[f].WT(k).P() = typename Mesh::FaceType::TexCoordType::PointType(
        typename Mesh::FaceType::TexCoordType::ScalarType(tris[f].uv[k][0]),
        typename Mesh::FaceType::TexCoordType::ScalarType(tris[f].uv[k][1]));
      m.face[f].WT(k).N() = short(tris[f].tex);
    }
  }
  vcg::tri::UpdateTopology<Mesh>::FaceFace(m);
  vcg::tri::UpdateFlags<Mesh>::FaceBorderFromFF(m);
}

// ---- independent 2D/3D geometry -------------------------------------------

double polyArea(const std::vector<UV2> &p)
{
  double a = 0.0;
  for (size_t i = 0; i < p.size(); ++i) {
    const UV2 &q = p[(i + 1) % p.size()];
    a += p[i][0] * q[1] - p[i][1] * q[0];
  }
  return std::fabs(0.5 * a);
}

// Sutherland-Hodgman clipping of a polygon to the integer cell [cu,cu+1]x[cv,cv+1].
std::vector<UV2> clipToCell(std::vector<UV2> poly, int cu, int cv)
{
  const double L = double(cu), R = double(cu + 1), B = double(cv), T = double(cv + 1);
  auto inside = [&](const UV2 &p, int side) {
    switch (side) {
      case 0: return p[0] >= L; case 1: return p[0] <= R;
      case 2: return p[1] >= B; default: return p[1] <= T;
    }
  };
  auto intersect = [&](const UV2 &a, const UV2 &b, int side) {
    double t = 0.0;
    switch (side) {
      case 0: t = (L - a[0]) / (b[0] - a[0]); break;
      case 1: t = (R - a[0]) / (b[0] - a[0]); break;
      case 2: t = (B - a[1]) / (b[1] - a[1]); break;
      default: t = (T - a[1]) / (b[1] - a[1]); break;
    }
    return UV2{a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])};
  };
  for (int side = 0; side < 4; ++side) {
    std::vector<UV2> out;
    if (poly.empty()) break;
    for (size_t i = 0; i < poly.size(); ++i) {
      const UV2 &cur = poly[i];
      const UV2 &prev = poly[(i + poly.size() - 1) % poly.size()];
      bool cin = inside(cur, side), pin = inside(prev, side);
      if (cin) {
        if (!pin) out.push_back(intersect(prev, cur, side));
        out.push_back(cur);
      } else if (pin) {
        out.push_back(intersect(prev, cur, side));
      }
    }
    poly.swap(out);
  }
  return poly;
}

// Barycentric coordinates of a 3D point in a non degenerate source triangle.
std::array<double, 3> bary3d(const V3 &p0, const V3 &p1, const V3 &p2, const V3 &x)
{
  auto sub = [](const V3 &a, const V3 &b) { return V3{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
  auto cross = [](const V3 &a, const V3 &b) {
    return V3{a[1] * b[2] - a[2] * b[1],
              a[2] * b[0] - a[0] * b[2],
              a[0] * b[1] - a[1] * b[0]};
  };
  auto dot = [](const V3 &a, const V3 &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  V3 e1 = sub(p1, p0), e2 = sub(p2, p0), d = sub(x, p0);
  V3 n = cross(e1, e2);
  double den = dot(n, n);
  double b1 = dot(cross(d, e2), n) / den;
  double b2 = dot(cross(e1, d), n) / den;
  return {1.0 - b1 - b2, b1, b2};
}

// Strictly interior integer crossings of a 2D segment (independent reference).
std::vector<double> segmentGridParams(const UV2 &a, const UV2 &b)
{
  std::vector<double> out;
  for (int c = 0; c < 2; ++c) {
    double d = b[c] - a[c];
    if (std::fabs(d) <= 1e-12) continue;
    int k0 = int(std::floor(std::min(a[c], b[c])));
    int k1 = int(std::ceil(std::max(a[c], b[c])));
    for (int k = k0; k <= k1; ++k) {
      double t = (double(k) - a[c]) / d;
      if (t > 1e-9 && t < 1.0 - 1e-9) {
        bool dup = false;
        for (double q : out) if (std::fabs(q - t) <= 1e-9) dup = true;
        if (!dup) out.push_back(t);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

template <class Mesh>
std::vector<V3> sourcePoints(const Mesh &m)
{
  std::vector<V3> p(m.vn);
  for (int i = 0; i < m.vn; ++i)
    p[i] = {double(m.vert[i].P()[0]), double(m.vert[i].P()[1]), double(m.vert[i].P()[2])};
  return p;
}

template <class Mesh>
std::vector<std::array<UV2, 3> > sourceUVs(const Mesh &m)
{
  std::vector<std::array<UV2, 3> > u(m.fn);
  for (int f = 0; f < m.fn; ++f)
    for (int k = 0; k < 3; ++k)
      u[f][k] = {double(m.face[f].WT(k).P()[0]), double(m.face[f].WT(k).P()[1])};
  return u;
}

// Whole-mesh independent audit shared by the geometry cases.
template <class Mesh>
int auditSplit(const Mesh &src, const Mesh &out, const vcg::tri::TileSplitMap &mp)
{
  auto P = sourcePoints(src);
  auto UV = sourceUVs(src);
  TASK_CHECK(int(mp.face_source.size()) == out.fn);
  TASK_CHECK(int(mp.corner_bary.size()) == out.fn);

  // tiles sorted and unique
  for (size_t i = 1; i < mp.tiles.size(); ++i)
    TASK_CHECK(mp.tiles[i - 1] < mp.tiles[i]);

  // per-source-face cell coverage by independent clipping
  std::map<std::pair<int, std::pair<int, int> >, double> expectedArea;
  std::map<std::pair<int, std::pair<int, int> >, double> gotArea;
  for (int fi = 0; fi < src.fn; ++fi) {
    std::vector<UV2> tri = {UV[fi][0], UV[fi][1], UV[fi][2]};
    double mnx = std::min({UV[fi][0][0], UV[fi][1][0], UV[fi][2][0]});
    double mxx = std::max({UV[fi][0][0], UV[fi][1][0], UV[fi][2][0]});
    double mny = std::min({UV[fi][0][1], UV[fi][1][1], UV[fi][2][1]});
    double mxy = std::max({UV[fi][0][1], UV[fi][1][1], UV[fi][2][1]});
    for (int cu = int(std::floor(mnx)); cu <= int(std::ceil(mxx)); ++cu)
      for (int cv = int(std::floor(mny)); cv <= int(std::ceil(mxy)); ++cv) {
        auto clip = clipToCell(tri, cu, cv);
        double a = polyArea(clip);
        if (a > 1e-12) expectedArea[{fi, {cu, cv}}] = a;
      }
  }

  for (int ofi = 0; ofi < out.fn; ++ofi) {
    const auto &f = out.face[ofi];
    int sfi = mp.face_source[ofi];
    TASK_CHECK(sfi >= 0 && sfi < src.fn);
    int tile = int(f.WT(0).N());
    TASK_CHECK(tile >= 0 && tile < int(mp.tiles.size()));
    for (int k = 1; k < 3; ++k) TASK_CHECK(int(f.WT(k).N()) == tile);
    const auto &addr = mp.tiles[tile];
    TASK_CHECK(addr.source_texture == int(src.face[sfi].WT(0).N()));

    // local UV inside the closed unit cell, global recovery matches source
    for (int k = 0; k < 3; ++k) {
      double lu = double(f.WT(k).P()[0]), lv = double(f.WT(k).P()[1]);
      TASK_CHECK(lu >= -FUV_EPS && lu <= 1.0 + FUV_EPS);
      TASK_CHECK(lv >= -FUV_EPS && lv <= 1.0 + FUV_EPS);
      const auto &b = mp.corner_bary[ofi][k];
      double gu = 0, gv = 0;
      for (int j = 0; j < 3; ++j) { gu += b[j] * UV[sfi][j][0]; gv += b[j] * UV[sfi][j][1]; }
      TASK_CHECK(std::fabs((lu + addr.u) - gu) <= FUV_EPS);
      TASK_CHECK(std::fabs((lv + addr.v) - gv) <= FUV_EPS);

      // map barycentric coords agree with the actual 3D output position
      V3 x{double(f.V(k)->P()[0]), double(f.V(k)->P()[1]), double(f.V(k)->P()[2])};
      std::array<int, 3> idx = {int(src.face[sfi].V(0) - &src.vert[0]),
                               int(src.face[sfi].V(1) - &src.vert[0]),
                               int(src.face[sfi].V(2) - &src.vert[0])};
      auto bb = bary3d(P[idx[0]], P[idx[1]], P[idx[2]], x);
      for (int j = 0; j < 3; ++j) TASK_CHECK(std::fabs(bb[j] - b[j]) <= GEPS);
    }

    // global-UV polygon area of this output face
    std::vector<UV2> gp(3);
    for (int k = 0; k < 3; ++k)
      gp[k] = {double(f.WT(k).P()[0]) + addr.u, double(f.WT(k).P()[1]) + addr.v};
    gotArea[{sfi, {addr.u, addr.v}}] += polyArea(gp);
  }

  TASK_CHECK(gotArea.size() == expectedArea.size());
  for (auto &kv : expectedArea) {
    auto it = gotArea.find(kv.first);
    TASK_CHECK(it != gotArea.end());
    if (it != gotArea.end())
      TASK_CHECK(std::fabs(it->second - kv.second) <= 1e-5 * std::max(1.0, kv.second));
  }

  // 3D area of every source face is covered exactly once
  for (int fi = 0; fi < src.fn; ++fi) {
    std::array<int, 3> idx = {int(src.face[fi].V(0) - &src.vert[0]),
                             int(src.face[fi].V(1) - &src.vert[0]),
                             int(src.face[fi].V(2) - &src.vert[0])};
    double e1[3] = {P[idx[1]][0] - P[idx[0]][0], P[idx[1]][1] - P[idx[0]][1], P[idx[1]][2] - P[idx[0]][2]};
    double e2[3] = {P[idx[2]][0] - P[idx[0]][0], P[idx[2]][1] - P[idx[0]][1], P[idx[2]][2] - P[idx[0]][2]};
    double orig = 0.5 * std::sqrt(std::pow(e1[1] * e2[2] - e1[2] * e2[1], 2) +
                                  std::pow(e1[2] * e2[0] - e1[0] * e2[2], 2) +
                                  std::pow(e1[0] * e2[1] - e1[1] * e2[0], 2));
    double sum = 0.0;
    for (int ofi = 0; ofi < out.fn; ++ofi) {
      if (mp.face_source[ofi] != fi) continue;
      const auto &f = out.face[ofi];
      V3 a{double(f.V(0)->P()[0]), double(f.V(0)->P()[1]), double(f.V(0)->P()[2])};
      V3 b{double(f.V(1)->P()[0]), double(f.V(1)->P()[1]), double(f.V(1)->P()[2])};
      V3 c{double(f.V(2)->P()[0]), double(f.V(2)->P()[1]), double(f.V(2)->P()[2])};
      double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
      double v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      sum += 0.5 * std::sqrt(std::pow(u[1] * v[2] - u[2] * v[1], 2) +
                             std::pow(u[2] * v[0] - u[0] * v[2], 2) +
                             std::pow(u[0] * v[1] - u[1] * v[0], 2));
    }
    TASK_CHECK(std::fabs(sum - orig) <= 1e-9 * std::max(1.0, orig));
  }
  return 0;
}

// Manifold + single open/closed fan at every output vertex.
template <class Mesh>
int auditTopology(const Mesh &out)
{
  std::map<std::pair<int, int>, int> edgeCount;
  for (int i = 0; i < out.fn; ++i)
    for (int e = 0; e < 3; ++e) {
      int a = int(out.face[i].V(e) - &out.vert[0]);
      int b = int(out.face[i].V1(e) - &out.vert[0]);
      ++edgeCount[{std::min(a, b), std::max(a, b)}];
    }
  for (auto &kv : edgeCount) TASK_CHECK(kv.second <= 2);

  std::vector<std::set<int>> vf(out.vn);
  for (int i = 0; i < out.fn; ++i)
    for (int k = 0; k < 3; ++k)
      vf[int(out.face[i].V(k) - &out.vert[0])].insert(i);
  for (int v = 0; v < out.vn; ++v) {
    TASK_CHECK(!vf[v].empty());
    std::set<int> seen;
    std::vector<int> st{*vf[v].begin()};
    seen.insert(*vf[v].begin());
    while (!st.empty()) {
      int f = st.back();
      st.pop_back();
      for (int e = 0; e < 3; ++e) {
        const auto *nf = out.face[f].FFp(e);
        if (nf == nullptr) continue;
        int ni = int(nf - &out.face[0]);
        if (ni < 0 || ni >= out.fn) continue;
        if (vf[v].count(ni) && seen.insert(ni).second) st.push_back(ni);
      }
    }
    TASK_CHECK(seen.size() == vf[v].size());
  }
  return 0;
}

template <class Mesh>
int auditQualityColor(const Mesh &src, const Mesh &out, const vcg::tri::TileSplitMap &mp)
{
  auto P = sourcePoints(src);
  for (int ofi = 0; ofi < out.fn; ++ofi) {
    int sfi = mp.face_source[ofi];
    std::array<int, 3> idx = {int(src.face[sfi].V(0) - &src.vert[0]),
                             int(src.face[sfi].V(1) - &src.vert[0]),
                             int(src.face[sfi].V(2) - &src.vert[0])};
    for (int k = 0; k < 3; ++k) {
      const auto &b = mp.corner_bary[ofi][k];
      double q = b[0] * src.vert[idx[0]].Q() + b[1] * src.vert[idx[1]].Q() +
                 b[2] * src.vert[idx[2]].Q();
      TASK_CHECK(std::fabs(double(out.face[ofi].V(k)->Q()) - q) <= 1e-9);
      bool isCorner = false;
      for (int j = 0; j < 3; ++j) if (b[j] >= 1.0 - 1e-10) {
        isCorner = true;
        for (int ch = 0; ch < 4; ++ch)
          TASK_CHECK(int(out.face[ofi].V(k)->C()[ch]) == int(src.vert[idx[j]].C()[ch]));
      }
      if (!isCorner) {
        for (int ch = 0; ch < 4; ++ch) {
          double v = b[0] * src.vert[idx[0]].C()[ch] + b[1] * src.vert[idx[1]].C()[ch] +
                     b[2] * src.vert[idx[2]].C()[ch];
          long rr = std::lround(v);
          rr = std::max(0L, std::min(255L, rr));
          TASK_CHECK(int(out.face[ofi].V(k)->C()[ch]) == int(rr));
        }
      }
    }
  }
  return 0;
}

// Independent check that every source edge carries all (and only consecutive)
// split parameters, so no output edge spans an existing point (no T-junction).
template <class Mesh>
int auditEdgeSplits(const Mesh &src, const Mesh &out, const vcg::tri::TileSplitMap &mp)
{
  auto P = sourcePoints(src);
  auto UV = sourceUVs(src);
  // neighbour table
  struct Link { int nf = -1, ne = -1; };
  std::vector<std::array<Link, 3> > ff(src.fn);
  std::map<std::pair<int, int>, std::pair<int, int> > half;
  for (int f = 0; f < src.fn; ++f)
    for (int e = 0; e < 3; ++e) {
      int a = int(src.face[f].V(e) - &src.vert[0]);
      int b = int(src.face[f].V1(e) - &src.vert[0]);
      auto it = half.find({b, a});
      if (it != half.end()) {
        ff[f][e] = {it->second.first, it->second.second};
        ff[it->second.first][it->second.second] = {f, e};
        half.erase(it);
      } else half[{a, b}] = {f, e};
    }

  for (int f = 0; f < src.fn; ++f) {
    std::array<int, 3> idx = {int(src.face[f].V(0) - &src.vert[0]),
                             int(src.face[f].V(1) - &src.vert[0]),
                             int(src.face[f].V(2) - &src.vert[0])};
    for (int e = 0; e < 3; ++e) {
      std::vector<double> expected = segmentGridParams(UV[f][e], UV[f][(e + 1) % 3]);
      const Link &lk = ff[f][e];
      if (lk.nf >= 0) {
        auto neigh = segmentGridParams(UV[lk.nf][lk.ne], UV[lk.nf][(lk.ne + 1) % 3]);
        for (double s : neigh) {
          double t = 1.0 - s;
          bool dup = false;
          for (double q : expected) if (std::fabs(q - t) <= 1e-9) dup = true;
          if (!dup) expected.push_back(t);
        }
      }
      expected.push_back(0.0);
      expected.push_back(1.0);
      std::sort(expected.begin(), expected.end());

      // parameters actually realized by output vertices on this source edge
      std::vector<double> got;
      for (int ofi = 0; ofi < out.fn; ++ofi) {
        if (mp.face_source[ofi] != f) continue;
        for (int k = 0; k < 3; ++k) {
          V3 x{double(out.face[ofi].V(k)->P()[0]),
               double(out.face[ofi].V(k)->P()[1]),
               double(out.face[ofi].V(k)->P()[2])};
          auto bb = bary3d(P[idx[0]], P[idx[1]], P[idx[2]], x);
          double zero = bb[(e + 2) % 3];
          if (std::fabs(zero) > 1e-8) continue; // not on this edge
          double t = (e == 0 ? bb[1] : e == 1 ? bb[2] : bb[0]);
          bool dup = false;
          for (double q : got) if (std::fabs(q - t) <= 1e-9) dup = true;
          if (!dup) got.push_back(t);
        }
      }
      std::sort(got.begin(), got.end());
      TASK_CHECK(got.size() == expected.size());
      for (size_t i = 0; i < std::min(got.size(), expected.size()); ++i)
        TASK_CHECK(std::fabs(got[i] - expected[i]) <= 1e-9);
    }
  }
  return 0;
}

struct AlwaysRefine {
  bool operator()(vcg::face::Pos<UMesh::FaceType>) { return true; }
};

const std::vector<std::string> TEX1 = {"paint.png"};
const std::vector<std::string> TEX2 = {"paint.png", "decals.png"};

} // namespace

// ============================================================ tiles group
static int tiles_multi_grid_corner()
{
  UMesh m;
  std::vector<V3> P = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  std::vector<TriGeom> tris = {{{{0, 1, 2}}, 0, {{{0.0, 0.0}, {2.0, 0.0}, {0.0, 2.0}}}}};
  buildMesh(m, P, tris, TEX1); // exact grid corner (1,1) inside
  UMesh out;
  vcg::tri::TileSplitMap mp;
  vcg::tri::SplitTextureTiles(m, out, mp);

  std::set<std::pair<int, int>> cells;
  for (auto &t : mp.tiles) cells.insert({t.u, t.v});
  TASK_CHECK(cells.count({0, 0}) && cells.count({1, 0}) && cells.count({0, 1}));
  TASK_CHECK(!cells.count({1, 1})); // the (1,1) grid corner only touches, no area
  TASK_CHECK(mp.tiles.size() == 3);
  if (auditSplit(m, out, mp)) return 1;
  if (auditEdgeSplits(m, out, mp)) return 1;
  if (auditQualityColor(m, out, mp)) return 1;
  return 0;
}

static int tiles_negative_double_mesh()
{
  DMesh m; // double wedge UV instantiation, negative tile addresses
  std::vector<V3> P = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  std::vector<TriGeom> tris = {{{{0, 1, 2}}, 0,
                                {{{-1.5, -1.5}, {0.5, -1.5}, {-1.5, 0.5}}}}};
  buildMesh(m, P, tris, TEX1);
  DMesh out;
  vcg::tri::TileSplitMap mp;
  vcg::tri::SplitTextureTiles(m, out, mp);
  bool hasNeg = false;
  for (auto &t : mp.tiles) {
    TASK_CHECK(t.u >= -2 && t.v >= -2);
    if (t.u < 0 || t.v < 0) hasNeg = true;
  }
  TASK_CHECK(hasNeg);
  if (auditSplit(m, out, mp)) return 1;
  return 0;
}

static int tiles_mirror_uv()
{
  UMesh m;
  std::vector<V3> P = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  // reversed UV ordering -> mirrored parametrization, spans 4 cells
  std::vector<TriGeom> tris = {{{{0, 1, 2}}, 0,
                                {{{0.0, 0.0}, {0.0, 2.0}, {2.0, 2.0}}}}};
  buildMesh(m, P, tris, TEX1);
  UMesh out;
  vcg::tri::TileSplitMap mp;
  vcg::tri::SplitTextureTiles(m, out, mp);
  TASK_CHECK(mp.tiles.size() == 3);
  // every output face keeps the +z 3D winding of the source
  for (int i = 0; i < out.fn; ++i) {
    const auto &f = out.face[i];
    auto n = (f.V(1)->P() - f.V(0)->P()) ^ (f.V(2)->P() - f.V(0)->P());
    TASK_CHECK(n[2] > 0);
  }
  if (auditSplit(m, out, mp)) return 1;
  if (auditEdgeSplits(m, out, mp)) return 1;
  return 0;
}

static int tiles_limit_exact()
{
  const int N = 32768;
  UMesh m;
  std::vector<V3> P;
  std::vector<TriGeom> tris;
  std::vector<std::string> names;
  for (int t = 0; t < 8; ++t) names.push_back("tex" + std::to_string(t) + ".png");
  P.reserve(3 * N);
  tris.reserve(N);
  for (int i = 0; i < N; ++i) {
    int base = int(P.size());
    P.push_back({0.0, 0.0, 0});
    P.push_back({0.7, 0.0, 0});
    P.push_back({0.0, 0.7, 0});
    int tex = i / 4096;
    int cu = (i % 64) - 32;
    int cv = ((i / 64) % 64) - 32;
    tris.push_back({{{base, base + 1, base + 2}}, tex,
                    {{{double(cu) + 0.1, double(cv) + 0.1},
                     {double(cu) + 0.8, double(cv) + 0.2},
                     {double(cu) + 0.2, double(cv) + 0.8}}}});
  }
  buildMesh(m, P, tris, names);
  UMesh out;
  vcg::tri::TileSplitMap mp;
  vcg::tri::SplitTextureTiles(m, out, mp);
  TASK_CHECK(int(mp.tiles.size()) == 32768);
  TASK_CHECK(out.fn == N);
  if (auditSplit(m, out, mp)) return 1;
  return 0;
}

static int tiles_limit_overflow_reject()
{
  const int N = 32769;
  UMesh m;
  std::vector<V3> P;
  std::vector<TriGeom> tris;
  std::vector<std::string> names;
  for (int t = 0; t < 9; ++t) names.push_back("ovf" + std::to_string(t) + ".png");
  P.reserve(3 * N);
  tris.reserve(N);
  for (int i = 0; i < N; ++i) {
    int base = int(P.size());
    P.push_back({0.0, 0.0, 0});
    P.push_back({0.7, 0.0, 0});
    P.push_back({0.0, 0.7, 0});
    // 9 textures, each contributing cells inside the allowed |UV| <= 32 box
    int tex = i / 3641;
    int j = i % 3641;
    int cu = (j % 64) - 32;
    int cv = (j / 64) - 32;
    tris.push_back({{{base, base + 1, base + 2}}, tex,
                    {{{double(cu) + 0.1, double(cv) + 0.1},
                     {double(cu) + 0.8, double(cv) + 0.2},
                     {double(cu) + 0.2, double(cv) + 0.8}}}});
  }
  buildMesh(m, P, tris, names);

  UMesh out;
  vcg::tri::Allocator<UMesh>::AddVertices(out, 1);
  out.vert[0].P() = vcg::Point3d(9, 9, 9);
  out.vn = 1;
  vcg::tri::TileSplitMap mp;
  mp.face_source.push_back(7);
  bool threw = false;
  try {
    vcg::tri::SplitTextureTiles(m, out, mp);
  } catch (std::length_error &) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(out.vn == 1 && out.vert[0].P()[0] == 9.0);
  TASK_CHECK(mp.face_source.size() == 1 && mp.face_source[0] == 7);

  // a legal call afterwards must still succeed
  UMesh good;
  std::vector<V3> gp = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  std::vector<TriGeom> gt = {{{{0, 1, 2}}, 0, {{{0.1, 0.1}, {0.8, 0.1}, {0.1, 0.8}}}}};
  buildMesh(good, gp, gt, TEX1);
  UMesh gout;
  vcg::tri::TileSplitMap gmp;
  vcg::tri::SplitTextureTiles(good, gout, gmp);
  TASK_CHECK(gout.fn == 1 && gmp.tiles.size() == 1);
  return 0;
}

// ========================================================== topology group
static int topo_continuous_weld()
{
  UMesh m;
  std::vector<V3> P = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
  std::vector<TriGeom> tris = {
    {{{0, 1, 2}}, 0, {{{0, 0}, {2, 0}, {2, 2}}}},
    {{{0, 2, 3}}, 0, {{{0, 0}, {2, 2}, {0, 2}}}}};
  buildMesh(m, P, tris, TEX1);
  UMesh out;
  vcg::tri::TileSplitMap mp;
  vcg::tri::SplitTextureTiles(m, out, mp);
  if (auditTopology(out)) return 1;
  if (auditEdgeSplits(m, out, mp)) return 1;
  if (auditSplit(m, out, mp)) return 1;

  // the interior source edge must produce welded (two-face) output edges
  std::map<std::pair<int, int>, int> ec;
  for (int i = 0; i < out.fn; ++i)
    for (int e = 0; e < 3; ++e) {
      int a = int(out.face[i].V(e) - &out.vert[0]);
      int b = int(out.face[i].V1(e) - &out.vert[0]);
      ++ec[{std::min(a, b), std::max(a, b)}];
    }
  int interior = 0;
  for (auto &kv : ec) if (kv.second == 2) ++interior;
  TASK_CHECK(interior > 0);
  return 0;
}

static int topo_seam_one_sided_split()
{
  UMesh m;
  // shared edge v1-v2: left face crosses a u grid line, right face stays in
  // one cell and uses a different texture (real seam)
  std::vector<V3> P = {{0, 0, 0}, {2, 0, 0}, {1, 1, 0}, {2, 2, 0}};
  std::vector<TriGeom> tris = {
    {{{0, 1, 2}}, 0, {{{0, 0}, {2, 0}, {1, 1}}}},
    {{{1, 3, 2}}, 1, {{{0.3, 0.2}, {0.8, 0.2}, {0.5, 0.9}}}}};
  buildMesh(m, P, tris, TEX2);
  UMesh out;
  vcg::tri::TileSplitMap mp;
  vcg::tri::SplitTextureTiles(m, out, mp);
  if (auditTopology(out)) return 1;
  if (auditEdgeSplits(m, out, mp)) return 1; // geometric split propagated across the seam
  if (auditSplit(m, out, mp)) return 1;

  // all output edges lying on the seam 3D segment must be borders
  vcg::tri::UpdateTopology<UMesh>::FaceFace(out);
  vcg::tri::UpdateFlags<UMesh>::FaceBorderFromFF(out);
  int seamEdges = 0, seamBorder = 0;
  for (int i = 0; i < out.fn; ++i)
    for (int e = 0; e < 3; ++e) {
      V3 a{double(out.face[i].V(e)->P()[0]), double(out.face[i].V(e)->P()[1]), double(out.face[i].V(e)->P()[2])};
      V3 b{double(out.face[i].V1(e)->P()[0]), double(out.face[i].V1(e)->P()[1]), double(out.face[i].V1(e)->P()[2])};
      auto onSeam = [](const V3 &p) { return std::fabs((p[0] + p[1]) - 2.0) < 1e-9 && p[2] == 0.0; };
      if (onSeam(a) && onSeam(b)) {
        ++seamEdges;
        if (out.face[i].IsB(e)) ++seamBorder;
      }
    }
  TASK_CHECK(seamEdges > 0);
  TASK_CHECK(seamEdges == seamBorder);
  return 0;
}

static int topo_disconnected_fan_same_cell()
{
  UMesh m;
  // two faces share source vertex v0, but the wedge UVs of v0 are offset by
  // 0.5 (still inside cell (0,0)): the fan around v0 is disconnected there
  std::vector<V3> P = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 1, 0}};
  std::vector<TriGeom> tris = {
    {{{0, 1, 2}}, 0, {{{0.1, 0.1}, {0.9, 0.1}, {0.1, 0.9}}}},
    {{{0, 2, 3}}, 0, {{{0.6, 0.6}, {0.6, 0.9}, {0.2, 0.9}}}}};
  buildMesh(m, P, tris, TEX1);
  UMesh out;
  vcg::tri::TileSplitMap mp;
  vcg::tri::SplitTextureTiles(m, out, mp);
  if (auditTopology(out)) return 1;
  int atV0 = 0;
  for (int i = 0; i < out.vn; ++i)
    if ((out.vert[i].P() - m.vert[0].P()).Norm() < 1e-12) ++atV0;
  TASK_CHECK(atV0 == 2); // two distinct output vertices for one source vertex
  return 0;
}

static int topo_overlapping_disconnected()
{
  UMesh m;
  // two unconnected components occupying identical 3D positions and UVs
  std::vector<V3> P = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0},
                       {0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  std::vector<TriGeom> tris = {
    {{{0, 1, 2}}, 0, {{{0.1, 0.1}, {0.7, 0.1}, {0.1, 0.7}}}},
    {{{3, 4, 5}}, 0, {{{0.1, 0.1}, {0.7, 0.1}, {0.1, 0.7}}}}};
  buildMesh(m, P, tris, TEX1);
  UMesh out;
  vcg::tri::TileSplitMap mp;
  vcg::tri::SplitTextureTiles(m, out, mp);
  if (auditTopology(out)) return 1;
  TASK_CHECK(out.fn == 2);
  TASK_CHECK(out.vn == 6); // coincident but disconnected: nothing welded
  if (auditSplit(m, out, mp)) return 1;
  return 0;
}

// ======================================================== persistence group
static int persist_ply_roundtrip_and_refine()
{
  UMesh m;
  std::vector<V3> P = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
  std::vector<TriGeom> tris = {
    {{{0, 1, 2}}, 0, {{{0, 0}, {2, 0}, {2, 2}}}},
    {{{0, 2, 3}}, 1, {{{-1, -1}, {1, 1}, {-1, 1}}}}}; // two textures -> texnumber
  buildMesh(m, P, tris, TEX2);
  UMesh out;
  vcg::tri::TileSplitMap mp;
  vcg::tri::SplitTextureTiles(m, out, mp);
  TASK_CHECK(out.textures.size() == mp.tiles.size());
  for (size_t i = 0; i < mp.tiles.size(); ++i) {
    const auto &a = mp.tiles[i];
    std::string expect = TEX2[a.source_texture] + ".tile." +
                         std::to_string(a.u) + "." + std::to_string(a.v);
    TASK_CHECK(out.textures[i] == expect);
  }

  int mask = vcg::tri::io::Mask::IOM_WEDGTEXCOORD | vcg::tri::io::Mask::IOM_VERTNORMAL |
             vcg::tri::io::Mask::IOM_VERTQUALITY | vcg::tri::io::Mask::IOM_VERTCOLOR;
  for (int binary = 0; binary < 2; ++binary) {
    std::string path = std::string(".task-build/udim_rt_") + (binary ? "bin.ply" : "asc.ply");
    TASK_CHECK(vcg::tri::io::ExporterPLY<UMesh>::Save(out, path.c_str(), mask, bool(binary)) == 0);
    UMesh in;
    int loadMask = 0;
    TASK_CHECK(vcg::tri::io::ImporterPLY<UMesh>::Open(in, path.c_str(), loadMask) == 0);
    TASK_CHECK(in.fn == out.fn && in.vn == out.vn);
    TASK_CHECK(in.textures.size() == out.textures.size());
    for (size_t i = 0; i < in.textures.size(); ++i)
      TASK_CHECK(in.textures[i] == out.textures[i]);
    for (int f = 0; f < out.fn; ++f)
      for (int k = 0; k < 3; ++k) {
        TASK_CHECK((in.face[f].WT(k).P() - out.face[f].WT(k).P()).Norm() <= FUV_EPS);
        TASK_CHECK(in.face[f].WT(k).N() == out.face[f].WT(k).N());
      }
    for (int v = 0; v < out.vn; ++v) {
      TASK_CHECK(std::fabs(double(in.vert[v].Q()) - double(out.vert[v].Q())) <= 1e-9);
      for (int ch = 0; ch < 4; ++ch)
        TASK_CHECK(int(in.vert[v].C()[ch]) == int(out.vert[v].C()[ch]));
      TASK_CHECK(std::fabs(double(in.vert[v].N()[2]) - double(out.vert[v].N()[2])) <= 1e-6);
    }

    // rebuild normals from the actual surface, then keep refining with RefineE
    vcg::tri::UpdateTopology<UMesh>::FaceFace(in);
    vcg::tri::UpdateNormal<UMesh>::PerFaceNormalized(in);
    vcg::tri::UpdateNormal<UMesh>::PerVertexNormalized(in);
    for (int f = 0; f < in.fn; ++f)
      TASK_CHECK(std::fabs(double(in.face[f].N()[2]) - 1.0) <= 1e-9);
    int oldFn = in.fn;
    vcg::tri::MidPoint<UMesh> mid(&in);
    AlwaysRefine ep;
    bool refined = vcg::tri::RefineE<UMesh, vcg::tri::MidPoint<UMesh>, AlwaysRefine>(in, mid, ep);
    TASK_CHECK(refined);
    TASK_CHECK(in.fn > oldFn);
  }
  return 0;
}

static int persist_failure_keeps_state()
{
  UMesh m;
  std::vector<V3> P = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  std::vector<TriGeom> tris = {{{{0, 1, 2}}, 0, {{{0.1, 0.1}, {0.8, 0.1}, {0.1, 0.8}}}}};
  buildMesh(m, P, tris, TEX1);

  UMesh out;
  vcg::tri::TileSplitMap mp;
  // aliasing
  bool threw = false;
  try { vcg::tri::SplitTextureTiles(m, m, mp); }
  catch (std::invalid_argument &) { threw = true; }
  TASK_CHECK(threw);

  // invalid texture index leaves a pre-filled result/map untouched
  m.face[0].WT(0).N() = m.face[0].WT(1).N() = m.face[0].WT(2).N() = 99;
  vcg::tri::Allocator<UMesh>::AddVertices(out, 2);
  out.vert[0].P() = vcg::Point3d(3, 3, 3);
  out.vn = 2;
  mp.tiles.push_back(vcg::tri::TileAddress{4, 5, 6});
  threw = false;
  try { vcg::tri::SplitTextureTiles(m, out, mp); }
  catch (std::invalid_argument &) { threw = true; }
  TASK_CHECK(threw);
  TASK_CHECK(out.vn == 2 && out.vert[0].P()[0] == 3.0);
  TASK_CHECK(mp.tiles.size() == 1 && mp.tiles[0].u == 5);

  // restore validity and confirm the mesh can still be processed normally
  m.face[0].WT(0).N() = m.face[0].WT(1).N() = m.face[0].WT(2).N() = 0;
  UMesh good;
  vcg::tri::TileSplitMap gmp;
  vcg::tri::SplitTextureTiles(m, good, gmp);
  TASK_CHECK(good.fn == 1 && gmp.tiles.size() == 1);
  return 0;
}

extern const struct task_case task_cases[] = {
  {"tiles", "multi_grid_corner", tiles_multi_grid_corner},
  {"tiles", "negative_double_mesh", tiles_negative_double_mesh},
  {"tiles", "mirror_uv", tiles_mirror_uv},
  {"tiles", "limit_exact_32768", tiles_limit_exact},
  {"tiles", "limit_overflow_atomic", tiles_limit_overflow_reject},
  {"topology", "continuous_edge_welding", topo_continuous_weld},
  {"topology", "seam_one_sided_split", topo_seam_one_sided_split},
  {"topology", "disconnected_fan_same_cell", topo_disconnected_fan_same_cell},
  {"topology", "overlapping_disconnected", topo_overlapping_disconnected},
  {"persistence", "ply_roundtrip_refine", persist_ply_roundtrip_and_refine},
  {"persistence", "failure_keeps_state", persist_failure_keeps_state},
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
