/****************************************************************************
* VCGLib                                                            o o     *
* Visual and Computer Graphics Library                            o     o   *
*                                                                _   O  _   *
* Copyright(C) 2024                                                \/)\/    *
* Visual Computing Lab                                            /\/|      *
* ISTI - Italian National Research Council                           |      *
*                                                                    \      *
* All rights reserved.                                                      *
*                                                                           *
* This program is free software; you can redistribute it and/or modify      *
* it under the terms of the GNU General Public License as published by      *
* the Free Software Foundation; either version 2 of the License, or         *
* (at your option) any later version.                                       *
*                                                                           *
****************************************************************************/

#ifndef __VCG_TRI_SPLIT_TEXTURE_TILES
#define __VCG_TRI_SPLIT_TEXTURE_TILES

#include <vcg/complex/complex.h>

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace vcg {
namespace tri {

/// A single integer UV cell address: texture index of the source mesh plus
/// the integer grid coordinates (u, v) of the tile.
struct TileAddress
{
  int source_texture = 0;
  int u = 0;
  int v = 0;

  bool operator<(const TileAddress &o) const
  {
    if (source_texture != o.source_texture) return source_texture < o.source_texture;
    if (u != o.u) return u < o.u;
    return v < o.v;
  }
  bool operator==(const TileAddress &o) const
  {
    return source_texture == o.source_texture && u == o.u && v == o.v;
  }
};

/// Output bookkeeping of SplitTextureTiles.
struct TileSplitMap
{
  /// Source face index for every output face, in output face order.
  std::vector<int> face_source;

  /// For every output face, the barycentric coordinates (w.r.t. the source
  /// face, corners 0..2) of its three corners.
  std::vector<std::array<std::array<double, 3>, 3> > corner_bary;

  /// Distinct non empty tile addresses, sorted by (source_texture, u, v).
  /// The short texture index stored in every output wedge is the position of
  /// the address in this vector.
  std::vector<TileAddress> tiles;
};

namespace splittexturetiles_detail {

static const double kParamEps     = 1e-10; ///< shared edge parameter merging tolerance
static const double kEventEps     = 1e-10; ///< barycentric event merging tolerance
static const double kCellEps      = 1e-9;  ///< closed cell membership tolerance (UV units)
static const double kUvSeamEps    = 1e-7;  ///< across-edge UV continuity tolerance
static const double kAreaEps      = 1e-18; ///< minimum accepted cell overlap area (UV^2)
static const int    kMaxTileCount = 32768; ///< nonnegative short texture index limit

struct DVec2 { double x = 0.0, y = 0.0; };

struct SplitEvent
{
  std::array<double, 3> b{{0.0, 0.0, 0.0}}; ///< barycentric coords w.r.t. the source face
  DVec2 uv;                                  ///< global UV recomputed from b
  bool onEdge[3] = {false, false, false};    ///< lies on source edge e (V(e)->V1(e))
  int corner = -1;                           ///< source corner index 0..2 when applicable
};

struct OutFace
{
  int sourceFace = -1;
  int tile = -1;
  int cellU = 0;
  int cellV = 0;
  std::array<int, 3> eventIdx{{-1, -1, -1}}; ///< indices in the source face event list
};

struct EdgeLink { int nfi = -1; int ne = -1; };

struct DSU
{
  std::vector<int> parent;
  explicit DSU(int n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
  int find(int x)
  {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
  }
  void unite(int a, int b)
  {
    a = find(a); b = find(b);
    if (a != b) parent[b] = a;
  }
};

inline bool finiteDouble(double v) { return std::isfinite(v); }

/// Insert a sorted edge parameter, merging events closer than kParamEps.
inline void insertParam(std::vector<double> &v, double t)
{
  for (double q : v)
    if (std::fabs(q - t) <= kParamEps) return;
  v.push_back(t);
  std::sort(v.begin(), v.end());
}

/// Classify a barycentric point against the three source edges/corners.
inline void classifyBary(const std::array<double, 3> &b, bool onEdge[3], int &corner)
{
  for (int k = 0; k < 3; ++k)
    onEdge[(k + 1) % 3] = (b[k] <= kEventEps);
  corner = -1;
  for (int k = 0; k < 3; ++k)
    if (b[k] >= 1.0 - kEventEps) corner = k;
}

/// Insert a barycentric event, merging events whose barycentric distance is
/// below kEventEps; returns the index in the face event list.
inline int insertEvent(std::vector<SplitEvent> &ev, const std::array<double, 3> &b,
                       const DVec2 &uv)
{
  for (size_t i = 0; i < ev.size(); ++i) {
    double d = 0.0;
    for (int k = 0; k < 3; ++k) d += (ev[i].b[k] - b[k]) * (ev[i].b[k] - b[k]);
    if (d <= kEventEps * kEventEps) {
      bool onEdge[3]; int corner;
      classifyBary(b, onEdge, corner);
      for (int k = 0; k < 3; ++k) ev[i].onEdge[k] = ev[i].onEdge[k] || onEdge[k];
      if (ev[i].corner < 0) ev[i].corner = corner;
      return int(i);
    }
  }
  SplitEvent e;
  e.b = b;
  e.uv = uv;
  classifyBary(b, e.onEdge, e.corner);
  ev.push_back(e);
  return int(ev.size()) - 1;
}

/// Strictly interior integer line crossings of the 2D segment q0->q1.
/// Endpoints exactly on an integer line do not generate events.
inline void lineCrossings(const DVec2 &q0, const DVec2 &q1,
                          std::vector<double> &params)
{
  for (int c = 0; c < 2; ++c) {
    double a = (c == 0 ? q0.x : q0.y);
    double d = (c == 0 ? q1.x - q0.x : q1.y - q0.y);
    if (std::fabs(d) <= kParamEps) continue; // parallel/collinear: no interior cut
    int k0 = int(std::floor(std::min(a, a + d)));
    int k1 = int(std::ceil(std::max(a, a + d)));
    for (int k = k0; k <= k1; ++k) {
      double t = (double(k) - a) / d;
      if (t > kParamEps && t < 1.0 - kParamEps) insertParam(params, t);
    }
  }
}

/// Barycentric coordinates of an edge e event at parameter t.
inline std::array<double, 3> edgeBary(int e, double t)
{
  std::array<double, 3> b{{0.0, 0.0, 0.0}};
  if (e == 0)      { b[0] = 1.0 - t; b[1] = t;       b[2] = 0.0; }
  else if (e == 1) { b[0] = 0.0;     b[1] = 1.0 - t; b[2] = t; }
  else             { b[0] = t;       b[1] = 0.0;     b[2] = 1.0 - t; }
  return b;
}

/// Parameter on edge e of a point with the given barycentric coords.
inline double edgeParam(int e, const std::array<double, 3> &b)
{
  if (e == 0) return b[1];
  if (e == 1) return b[2];
  return b[0];
}

/// Floor, snapping values within kCellEps of an integer onto that integer.
inline int snapFloor(double x)
{
  double r = std::round(x);
  if (std::fabs(x - r) <= kCellEps) return int(r);
  return int(std::floor(x));
}

} // namespace splittexturetiles_detail

/**
 * Split a triangle mesh with per-corner (wedge) texture coordinates into
 * independent mesh pieces, one for every non empty integer UV cell of every
 * source texture.
 *
 * The source mesh is never modified. On failure (illegal input, aliasing or
 * more than 32768 distinct non empty tile addresses) the function throws and
 * both \p result and \p map keep their previous contents: no partial mesh is
 * produced.
 *
 * The output mesh uses the same vertex/face components as the input one
 * (positions, normals, quality and color on vertices; wedge texture coords,
 * normals, FF adjacency and flags on faces). Wedge texture indices are the
 * positions of the addresses in TileSplitMap::tiles, and the output texture
 * names are the source names suffixed with ".tile.u.v" (signed decimal u, v).
 */
template <class MeshType>
void SplitTextureTiles(const MeshType &source, MeshType &result, TileSplitMap &map)
{
  namespace detail = splittexturetiles_detail;
  using detail::DVec2;
  using detail::SplitEvent;
  using detail::OutFace;
  using detail::EdgeLink;
  using detail::DSU;

  if (static_cast<const void *>(&source) == static_cast<const void *>(&result))
    throw std::invalid_argument("SplitTextureTiles: source and result must be distinct meshes");
  if (source.fn <= 0 || source.vn <= 0)
    throw std::invalid_argument("SplitTextureTiles: source mesh is empty");

  const int fn = source.fn;
  const int vn = source.vn;

  // ------------------------------------------------------------ validation
  for (int i = 0; i < vn; ++i) {
    const auto &v = source.vert[i];
    if (v.IsD())
      throw std::invalid_argument("SplitTextureTiles: deleted vertices are not allowed");
    for (int k = 0; k < 3; ++k) {
      double c = double(v.P()[k]);
      if (!detail::finiteDouble(c) || std::fabs(c) > 1000.0)
        throw std::invalid_argument("SplitTextureTiles: vertex coordinate out of range");
    }
  }
  if (source.textures.empty())
    throw std::invalid_argument("SplitTextureTiles: mesh has no textures");
  {
    std::set<std::string> names;
    for (const std::string &name : source.textures) {
      if (name.empty())
        throw std::invalid_argument("SplitTextureTiles: empty texture name");
      for (char ch : name)
        if (std::isspace(static_cast<unsigned char>(ch)))
          throw std::invalid_argument("SplitTextureTiles: texture name contains whitespace");
      if (!names.insert(name).second)
        throw std::invalid_argument("SplitTextureTiles: duplicate texture name");
    }
  }

  std::vector<char> referenced(vn, 0);
  std::vector<std::array<DVec2, 3> > faceUV(fn);
  std::vector<int> faceTex(fn, -1);

  for (int fi = 0; fi < fn; ++fi) {
    const auto &f = source.face[fi];
    if (f.IsD())
      throw std::invalid_argument("SplitTextureTiles: deleted faces are not allowed");
    for (int k = 0; k < 3; ++k) {
      if (f.V(k) == nullptr)
        throw std::invalid_argument("SplitTextureTiles: face with null vertex");
      int vi = int(f.V(k) - &source.vert[0]);
      if (vi < 0 || vi >= vn)
        throw std::invalid_argument("SplitTextureTiles: face vertex out of range");
      referenced[vi] = 1;
      faceUV[fi][k] = DVec2{double(f.WT(k).P()[0]), double(f.WT(k).P()[1])};
      if (!detail::finiteDouble(faceUV[fi][k].x) || !detail::finiteDouble(faceUV[fi][k].y) ||
          std::fabs(faceUV[fi][k].x) > 32.0 || std::fabs(faceUV[fi][k].y) > 32.0)
        throw std::invalid_argument("SplitTextureTiles: texture coordinate out of range");
    }
    int t0 = int(f.WT(0).N());
    if (t0 < 0 || t0 >= int(source.textures.size()))
      throw std::invalid_argument("SplitTextureTiles: invalid texture index");
    for (int k = 1; k < 3; ++k)
      if (int(f.WT(k).N()) != t0)
        throw std::invalid_argument("SplitTextureTiles: face corners must share one texture index");
    faceTex[fi] = t0;
  }
  for (int i = 0; i < vn; ++i)
    if (!referenced[i])
      throw std::invalid_argument("SplitTextureTiles: unreferenced (isolated) vertex");

  // Two-manifold check: every undirected edge has at most two incident faces.
  {
    std::map<std::pair<int, int>, int> edgeCount;
    for (int fi = 0; fi < fn; ++fi) {
      const auto &f = source.face[fi];
      for (int e = 0; e < 3; ++e) {
        int a = int(f.V(e) - &source.vert[0]);
        int b = int(f.V1(e) - &source.vert[0]);
        auto key = std::make_pair(std::min(a, b), std::max(a, b));
        if (++edgeCount[key] > 2)
          throw std::invalid_argument("SplitTextureTiles: non manifold edge (more than two faces)");
      }
    }
  }

  // Nonzero 3D and UV areas, plus UV orientation sign per face.
  std::vector<int> uvSign(fn, 1);
  for (int fi = 0; fi < fn; ++fi) {
    const auto &f = source.face[fi];
    double area3 = double(((f.V(1)->P() - f.V(0)->P()) ^
                           (f.V(2)->P() - f.V(0)->P())).Norm());
    if (!(area3 > 0.0))
      throw std::invalid_argument("SplitTextureTiles: zero area 3D face");
    const auto &uv = faceUV[fi];
    DVec2 d1{uv[1].x - uv[0].x, uv[1].y - uv[0].y};
    DVec2 d2{uv[2].x - uv[0].x, uv[2].y - uv[0].y};
    double areaUV = d1.x * d2.y - d1.y * d2.x;
    if (std::fabs(areaUV) <= detail::kAreaEps)
      throw std::invalid_argument("SplitTextureTiles: zero UV area face");
    uvSign[fi] = areaUV > 0.0 ? 1 : -1;
  }

  // FF adjacency reconstructed locally; the source mesh itself is never touched.
  std::vector<std::array<EdgeLink, 3> > ffLink(fn);
  {
    std::map<std::pair<int, int>, std::pair<int, int> > halfEdge;
    for (int fi = 0; fi < fn; ++fi) {
      const auto &f = source.face[fi];
      for (int e = 0; e < 3; ++e) {
        int a = int(f.V(e) - &source.vert[0]);
        int b = int(f.V1(e) - &source.vert[0]);
        auto it = halfEdge.find(std::make_pair(b, a));
        if (it != halfEdge.end()) {
          int nfi = it->second.first, ne = it->second.second;
          ffLink[fi][e] = EdgeLink{nfi, ne};
          ffLink[nfi][ne] = EdgeLink{fi, e};
          halfEdge.erase(it);
        } else {
          halfEdge[std::make_pair(a, b)] = std::make_pair(fi, e);
        }
      }
    }
  }

  // True when the wedge UVs are continuous across a shared source edge:
  // same texture id and matching endpoint UVs (within kUvSeamEps).
  auto edgeContinuous = [&](int fi, int e, int nfi) -> bool {
    if (faceTex[fi] != faceTex[nfi]) return false;
    const auto &f = source.face[fi];
    const auto &g = source.face[nfi];
    const auto *va = f.V(e);
    const auto *vb = f.V1(e);
    int ka = -1, kb = -1, ka2 = -1, kb2 = -1;
    for (int k = 0; k < 3; ++k) {
      if (f.V(k) == va) ka = k;
      if (f.V(k) == vb) kb = k;
      if (g.V(k) == va) ka2 = k;
      if (g.V(k) == vb) kb2 = k;
    }
    if (ka < 0 || kb < 0 || ka2 < 0 || kb2 < 0) return false;
    for (int c = 0; c < 2; ++c) {
      double a0 = (c == 0 ? faceUV[fi][ka].x : faceUV[fi][ka].y);
      double a1 = (c == 0 ? faceUV[nfi][ka2].x : faceUV[nfi][ka2].y);
      double b0 = (c == 0 ? faceUV[fi][kb].x : faceUV[fi][kb].y);
      double b1 = (c == 0 ? faceUV[nfi][kb2].x : faceUV[nfi][kb2].y);
      if (std::fabs(a0 - a1) > detail::kUvSeamEps ||
          std::fabs(b0 - b1) > detail::kUvSeamEps)
        return false;
    }
    return true;
  };

  // ------------------------------------------- per-face edge event parameters
  // Pass 1: every edge gets its own integer-line crossings. Grid corners
  // lying on an edge are collected here as well, so that pass 2 can propagate
  // every boundary event and the 1e-10 welding always finds exact matches.
  std::vector<std::array<std::vector<double>, 3> > edgeParams(fn);
  for (int fi = 0; fi < fn; ++fi) {
    for (int e = 0; e < 3; ++e) {
      auto &params = edgeParams[fi][e];
      detail::insertParam(params, 0.0);
      detail::insertParam(params, 1.0);
      detail::lineCrossings(faceUV[fi][e], faceUV[fi][(e + 1) % 3], params);
    }
  }
  // Integer grid corners inside (or on the boundary of) each UV triangle.
  // Interior ones become face events directly; boundary ones are routed to
  // the owning edge parameter list.
  std::vector<std::vector<std::array<double, 3> > > interiorGridCorners(fn);
  for (int fi = 0; fi < fn; ++fi) {
    const auto &uv = faceUV[fi];
    double minx = std::min(std::min(uv[0].x, uv[1].x), uv[2].x);
    double maxx = std::max(std::max(uv[0].x, uv[1].x), uv[2].x);
    double miny = std::min(std::min(uv[0].y, uv[1].y), uv[2].y);
    double maxy = std::max(std::max(uv[0].y, uv[1].y), uv[2].y);
    DVec2 d1{uv[1].x - uv[0].x, uv[1].y - uv[0].y};
    DVec2 d2{uv[2].x - uv[0].x, uv[2].y - uv[0].y};
    double det = d1.x * d2.y - d1.y * d2.x; // nonzero by validation
    for (int gx = int(std::floor(minx)); gx <= int(std::ceil(maxx)); ++gx) {
      for (int gy = int(std::floor(miny)); gy <= int(std::ceil(maxy)); ++gy) {
        double rx = double(gx) - uv[0].x;
        double ry = double(gy) - uv[0].y;
        double b1 = (rx * d2.y - ry * d2.x) / det;
        double b2 = (d1.x * ry - d1.y * rx) / det;
        double b0 = 1.0 - b1 - b2;
        if (b0 < -detail::kCellEps || b1 < -detail::kCellEps || b2 < -detail::kCellEps)
          continue;
        std::array<double, 3> b{{b0, b1, b2}};
        for (double &bb : b)
          if (std::fabs(bb) <= detail::kCellEps) bb = 0.0;
        int onE = -1;
        for (int e = 0; e < 3; ++e)
          if (b[(e + 2) % 3] <= detail::kEventEps) onE = e;
        if (onE >= 0)
          detail::insertParam(edgeParams[fi][onE], detail::edgeParam(onE, b));
        else
          interiorGridCorners[fi].push_back(b);
      }
    }
  }
  // Pass 2: union the edge parameters across EVERY shared source edge
  // (parameters reversed). The edge parameter is also the 3D/barycentric
  // parameter, so even across a UV seam a point created on one side forces a
  // geometrically coincident event on the other side: the output has no long
  // edge spanning an existing point and no T-junction. Welding of the two
  // coincident events is decided later, only when the UVs are continuous.
  for (int fi = 0; fi < fn; ++fi) {
    for (int e = 0; e < 3; ++e) {
      const EdgeLink &link = ffLink[fi][e];
      if (link.nfi < 0) continue;
      for (double s : edgeParams[link.nfi][link.ne])
        detail::insertParam(edgeParams[fi][e], 1.0 - s);
    }
  }

  // ------------------------------------------------------- per-face event sets
  std::vector<std::vector<SplitEvent> > faceEvents(fn);
  auto interpUV = [&](int fi, const std::array<double, 3> &b) -> DVec2 {
    const auto &uv = faceUV[fi];
    return DVec2{b[0] * uv[0].x + b[1] * uv[1].x + b[2] * uv[2].x,
                 b[0] * uv[0].y + b[1] * uv[1].y + b[2] * uv[2].y};
  };

  for (int fi = 0; fi < fn; ++fi) {
    auto &ev = faceEvents[fi];
    for (int k = 0; k < 3; ++k) {
      std::array<double, 3> b{{0.0, 0.0, 0.0}};
      b[k] = 1.0;
      detail::insertEvent(ev, b, faceUV[fi][k]);
    }
    for (int e = 0; e < 3; ++e) {
      for (double t : edgeParams[fi][e]) {
        auto b = detail::edgeBary(e, t);
        detail::insertEvent(ev, b, interpUV(fi, b));
      }
    }
    for (const auto &b : interiorGridCorners[fi])
      detail::insertEvent(ev, b, interpUV(fi, b));
  }

  // ----------------------------- cell polygons, triangulation and tile set
  std::set<TileAddress> tileSet;
  std::vector<OutFace> outFaces;

  for (int fi = 0; fi < fn; ++fi) {
    const auto &ev = faceEvents[fi];
    const auto &uv = faceUV[fi];
    double minx = std::min(std::min(uv[0].x, uv[1].x), uv[2].x);
    double maxx = std::max(std::max(uv[0].x, uv[1].x), uv[2].x);
    double miny = std::min(std::min(uv[0].y, uv[1].y), uv[2].y);
    double maxy = std::max(std::max(uv[0].y, uv[1].y), uv[2].y);

    for (int cu = detail::snapFloor(minx); cu <= detail::snapFloor(maxx); ++cu) {
      for (int cv = detail::snapFloor(miny); cv <= detail::snapFloor(maxy); ++cv) {
        // closed cell membership
        std::vector<int> members;
        for (size_t i = 0; i < ev.size(); ++i) {
          double lx = ev[i].uv.x - double(cu);
          double ly = ev[i].uv.y - double(cv);
          if (lx >= -detail::kCellEps && lx <= 1.0 + detail::kCellEps &&
              ly >= -detail::kCellEps && ly <= 1.0 + detail::kCellEps)
            members.push_back(int(i));
        }
        if (members.size() < 3) continue;

        // order CCW in the UV plane, then flip when the UV parametrization is
        // mirrored relative to the 3D face so the output keeps the 3D winding.
        DVec2 c{0.0, 0.0};
        for (int i : members) { c.x += ev[i].uv.x; c.y += ev[i].uv.y; }
        c.x /= double(members.size());
        c.y /= double(members.size());
        std::sort(members.begin(), members.end(), [&](int a, int b) {
          double aa = std::atan2(ev[a].uv.y - c.y, ev[a].uv.x - c.x);
          double ab = std::atan2(ev[b].uv.y - c.y, ev[b].uv.x - c.x);
          if (aa == ab) return a < b;
          return aa < ab;
        });
        if (uvSign[fi] < 0)
          std::reverse(members.begin(), members.end());

        // reject boundary-only matches: require real UV area overlap
        double area = 0.0;
        for (size_t i = 0; i < members.size(); ++i) {
          const DVec2 &a = ev[members[i]].uv;
          const DVec2 &b = ev[members[(i + 1) % members.size()]].uv;
          area += a.x * b.y - a.y * b.x;
        }
        if (!(std::fabs(0.5 * area) > detail::kAreaEps)) continue;

        TileAddress addr;
        addr.source_texture = faceTex[fi];
        addr.u = cu;
        addr.v = cv;
        tileSet.insert(addr);

        // convex polygon: fan triangulation from the first vertex; skip the
        // occasional zero-area fan produced by collinear boundary vertices
        for (size_t i = 1; i + 1 < members.size(); ++i) {
          const DVec2 &p0 = ev[members[0]].uv;
          const DVec2 &p1 = ev[members[i]].uv;
          const DVec2 &p2 = ev[members[i + 1]].uv;
          double triArea = (p1.x - p0.x) * (p2.y - p0.y) -
                           (p1.y - p0.y) * (p2.x - p0.x);
          if (std::fabs(triArea) <= detail::kAreaEps) continue;
          OutFace of;
          of.sourceFace = fi;
          of.cellU = cu;
          of.cellV = cv;
          of.eventIdx = {{members[0], members[i], members[i + 1]}};
          outFaces.push_back(of);
        }
      }
    }
  }

  if (int(tileSet.size()) > detail::kMaxTileCount)
    throw std::length_error("SplitTextureTiles: more than 32768 non empty tile addresses");

  // stable tile ids: sorted address position
  std::vector<TileAddress> tiles(tileSet.begin(), tileSet.end());
  std::map<TileAddress, int> tileIdOf;
  for (int i = 0; i < int(tiles.size()); ++i)
    tileIdOf[tiles[i]] = i;
  for (OutFace &of : outFaces) {
    TileAddress a;
    a.source_texture = faceTex[of.sourceFace];
    a.u = of.cellU;
    a.v = of.cellV;
    of.tile = tileIdOf[a];
  }

  // tiles actually used by every (face, event)
  std::vector<std::vector<std::set<int> > > eventTiles(fn);
  for (int fi = 0; fi < fn; ++fi)
    eventTiles[fi].resize(faceEvents[fi].size());
  for (const OutFace &of : outFaces)
    for (int idx : of.eventIdx)
      eventTiles[of.sourceFace][idx].insert(of.tile);

  // ------------ temporary nodes: one per (face, event, tile)
  // A node is bound to one tile, so coincident points belonging to different
  // tiles are never welded: grid boundaries disconnect the pieces. Nodes are
  // then merged by DSU only along locally connected face corners/edges inside
  // the same tile.
  struct NodeRep { int fi = -1; int ei = -1; };
  std::vector<NodeRep> nodeRep;
  std::map<std::tuple<int, int, int>, int> nodeOf;
  auto getNode = [&](int fi, int ei, int tile) -> int {
    auto key = std::make_tuple(fi, ei, tile);
    auto it = nodeOf.find(key);
    if (it != nodeOf.end()) return it->second;
    int id = int(nodeRep.size());
    nodeOf[key] = id;
    nodeRep.push_back(NodeRep{fi, ei});
    return id;
  };
  for (int fi = 0; fi < fn; ++fi)
    for (size_t ei = 0; ei < faceEvents[fi].size(); ++ei)
      for (int tile : eventTiles[fi][ei])
        getNode(fi, int(ei), tile);
  int nodeCount = int(nodeRep.size());
  DSU dsu(nodeCount);

  auto commonTiles = [&](int fi, int ei, int fj, int ej,
                         std::vector<int> &out) {
    out.clear();
    for (int t : eventTiles[fj][ej])
      if (eventTiles[fi][ei].count(t)) out.push_back(t);
  };

  // edge welding across continuous source edges (same event only, <= kParamEps)
  std::vector<int> sharedTiles;
  for (int fi = 0; fi < fn; ++fi) {
    for (int e = 0; e < 3; ++e) {
      const EdgeLink &link = ffLink[fi][e];
      if (link.nfi < 0 || !edgeContinuous(fi, e, link.nfi)) continue;
      const auto &ev = faceEvents[fi];
      const auto &nev = faceEvents[link.nfi];
      for (size_t i = 0; i < ev.size(); ++i) {
        if (!ev[i].onEdge[e]) continue;
        double s = 1.0 - detail::edgeParam(e, ev[i].b);
        for (size_t j = 0; j < nev.size(); ++j) {
          if (!nev[j].onEdge[link.ne]) continue;
          if (std::fabs(detail::edgeParam(link.ne, nev[j].b) - s) <= detail::kParamEps) {
            commonTiles(fi, int(i), link.nfi, int(j), sharedTiles);
            for (int t : sharedTiles)
              dsu.unite(getNode(fi, int(i), t), getNode(link.nfi, int(j), t));
            break;
          }
        }
      }
    }
  }

  // corner welding along each locally connected face fan of every source
  // vertex; seams and borders terminate a fan, so disconnected fans stay split
  {
    std::vector<std::vector<std::pair<int, int> > > incidences(vn);
    for (int fi = 0; fi < fn; ++fi) {
      const auto &f = source.face[fi];
      for (int k = 0; k < 3; ++k)
        incidences[int(f.V(k) - &source.vert[0])].push_back({fi, k});
    }
    auto cornerEvent = [&](int fi, int ck) -> int {
      for (size_t i = 0; i < faceEvents[fi].size(); ++i)
        if (faceEvents[fi][i].corner == ck) return int(i);
      return -1;
    };
    std::set<std::pair<int, int> > visited;
    for (int vi = 0; vi < vn; ++vi) {
      for (const auto &seed : incidences[vi]) {
        if (visited.count(seed)) continue;
        // collect the connected fan component through continuous edges only
        std::vector<std::pair<int, int> > component;
        std::vector<std::pair<int, int> > stack;
        stack.push_back(seed);
        visited.insert(seed);
        while (!stack.empty()) {
          auto cur = stack.back();
          stack.pop_back();
          component.push_back(cur);
          int cfi = cur.first, ck = cur.second;
          for (int step = 0; step < 2; ++step) {
            int outEdge = (step == 0 ? ck : (ck + 2) % 3);
            const EdgeLink &link = ffLink[cfi][outEdge];
            if (link.nfi < 0 || !edgeContinuous(cfi, outEdge, link.nfi)) continue;
            int nk = -1;
            for (int k = 0; k < 3; ++k)
              if (source.face[link.nfi].V(k) == source.face[cfi].V(ck)) { nk = k; break; }
            if (nk < 0) continue;
            auto nxt = std::make_pair(link.nfi, nk);
            if (!visited.insert(nxt).second) continue;
            stack.push_back(nxt);
          }
        }
        // weld corner events that meet inside a common tile
        for (size_t i = 0; i < component.size(); ++i) {
          int ei = cornerEvent(component[i].first, component[i].second);
          for (size_t j = i + 1; j < component.size(); ++j) {
            int ej = cornerEvent(component[j].first, component[j].second);
            if (ei < 0 || ej < 0) continue;
            commonTiles(component[i].first, ei, component[j].first, ej, sharedTiles);
            for (int t : sharedTiles)
              dsu.unite(getNode(component[i].first, ei, t),
                        getNode(component[j].first, ej, t));
          }
        }
      }
    }
  }

  // ------------------------------------------------------------- build output
  // everything below that can fail still does so before result/map are touched
  MeshType local;
  std::map<int, int> rootToVertex;
  std::vector<int> nodeToVertex(nodeCount, -1);
  for (int t = 0; t < nodeCount; ++t) {
    int r = dsu.find(t);
    auto it = rootToVertex.find(r);
    if (it == rootToVertex.end()) {
      nodeToVertex[t] = int(rootToVertex.size());
      rootToVertex[r] = nodeToVertex[t];
    } else {
      nodeToVertex[t] = it->second;
    }
  }
  int outVCount = int(rootToVertex.size());
  int outFCount = int(outFaces.size());
  tri::Allocator<MeshType>::AddVertices(local, outVCount);
  tri::Allocator<MeshType>::AddFaces(local, outFCount);

  using Scalar = typename MeshType::VertexType::CoordType::ScalarType;
  using TexScalar = typename MeshType::FaceType::TexCoordType::ScalarType;

  for (const auto &kv : rootToVertex) {
    int r = kv.first, id = kv.second;
    int rfi = nodeRep[r].fi, rei = nodeRep[r].ei;
    const auto &b = faceEvents[rfi][rei].b;
    const auto &sf = source.face[rfi];
    auto &nv = local.vert[id];

    auto p0 = sf.V(0)->P(), p1 = sf.V(1)->P(), p2 = sf.V(2)->P();
    nv.P() = typename MeshType::VertexType::CoordType(
      Scalar(b[0] * p0[0] + b[1] * p1[0] + b[2] * p2[0]),
      Scalar(b[0] * p0[1] + b[1] * p1[1] + b[2] * p2[1]),
      Scalar(b[0] * p0[2] + b[1] * p1[2] + b[2] * p2[2]));

    if (MeshType::VertexType::HasQuality())
      nv.Q() = typename MeshType::VertexType::QualityType(
        b[0] * sf.V(0)->Q() + b[1] * sf.V(1)->Q() + b[2] * sf.V(2)->Q());

    if (MeshType::VertexType::HasColor()) {
      if (faceEvents[rfi][rei].corner >= 0) {
        nv.C() = sf.V(faceEvents[rfi][rei].corner)->C(); // original values preserved
      } else {
        for (int ch = 0; ch < 4; ++ch) {
          double v = b[0] * sf.V(0)->C()[ch] + b[1] * sf.V(1)->C()[ch] +
                     b[2] * sf.V(2)->C()[ch];
          long rnd = std::lround(v);
          if (rnd < 0) rnd = 0;
          if (rnd > 255) rnd = 255;
          nv.C()[ch] = static_cast<unsigned char>(rnd);
        }
      }
    }
    nv.N() = typename MeshType::VertexType::NormalType(0, 0, 0);
  }

  for (int oi = 0; oi < outFCount; ++oi) {
    const OutFace &of = outFaces[oi];
    auto &dst = local.face[oi];
    for (int k = 0; k < 3; ++k) {
      int node = getNode(of.sourceFace, of.eventIdx[k], of.tile);
      dst.V(k) = &local.vert[nodeToVertex[node]];
      const auto &b = faceEvents[of.sourceFace][of.eventIdx[k]].b;
      DVec2 guv = interpUV(of.sourceFace, b);
      double lx = guv.x - double(tiles[of.tile].u);
      double ly = guv.y - double(tiles[of.tile].v);
      if (lx < 0.0 && lx >= -detail::kCellEps) lx = 0.0;
      if (ly < 0.0 && ly >= -detail::kCellEps) ly = 0.0;
      if (lx > 1.0 && lx <= 1.0 + detail::kCellEps) lx = 1.0;
      if (ly > 1.0 && ly <= 1.0 + detail::kCellEps) ly = 1.0;
      dst.WT(k).P() = typename MeshType::FaceType::TexCoordType::PointType(
        TexScalar(lx), TexScalar(ly));
      dst.WT(k).N() = short(of.tile);
    }
  }

  // texture names, one per tile
  local.textures.clear();
  for (const TileAddress &a : tiles) {
    std::string name = source.textures[a.source_texture];
    name += ".tile.";
    name += std::to_string(a.u);
    name += '.';
    name += std::to_string(a.v);
    local.textures.push_back(name);
  }

  tri::UpdateTopology<MeshType>::FaceFace(local);
  tri::UpdateFlags<MeshType>::FaceBorderFromFF(local);
  tri::UpdateNormal<MeshType>::PerFaceNormalized(local);
  tri::UpdateNormal<MeshType>::PerVertexNormalized(local);

  // ----------------------------------------------- commit (noexcept swaps)
  TileSplitMap outMap;
  outMap.tiles = tiles;
  outMap.face_source.reserve(outFCount);
  outMap.corner_bary.reserve(outFCount);
  for (const OutFace &of : outFaces) {
    outMap.face_source.push_back(of.sourceFace);
    std::array<std::array<double, 3>, 3> cb;
    for (int k = 0; k < 3; ++k)
      cb[k] = faceEvents[of.sourceFace][of.eventIdx[k]].b;
    outMap.corner_bary.push_back(cb);
  }

  result.Clear();
  std::swap(local.vert, result.vert);
  std::swap(local.face, result.face);
  std::swap(local.textures, result.textures);
  result.vn = local.vn;
  result.fn = local.fn;
  local.vn = local.fn = 0;
  map = std::move(outMap);
}

} // namespace tri
} // namespace vcg

#endif
