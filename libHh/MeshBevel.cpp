// -*- C++ -*-  Copyright (c) Microsoft Corporation; see LICENSE.
#include "libHh/GMesh.h"

#include <algorithm>   // std::ranges::reverse()
#include <cmath>       // isfinite(), sqrt(), llround()
#include <cstdint>     // int64_t, uint64_t
#include <stdexcept>   // std::invalid_argument

#include "libHh/Map.h"
#include "libHh/Polygon.h"
#include "libHh/RangeOp.h"  // sort()
#include "libHh/Set.h"

namespace hh {

namespace {

// Relative tolerances; absolute values are derived from the mesh's characteristic size at run time.
constexpr float k_rel_snap = 1e-6f;  // Position quantization merging points that coincide by construction.
constexpr float k_rel_eps = 1e-7f;
constexpr double k_rel_min_volume = 1e-10;

struct VKey {
  int64_t x, y, z;
  [[nodiscard]] bool operator==(const VKey& o) const { return x == o.x && y == o.y && z == o.z; }
  [[nodiscard]] bool operator<(const VKey& o) const {
    return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z;
  }
};

struct VKeyHash {
  [[nodiscard]] size_t operator()(const VKey& k) const noexcept {
    uint64_t h = uint64_t(k.x) * 0x9e3779b97f4a7c15ULL;
    h ^= uint64_t(k.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= uint64_t(k.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return size_t(h);
  }
};

[[nodiscard]] VKey key_of(const Point& p, float snap) {
  return {std::llround(p[0] / snap), std::llround(p[1] / snap), std::llround(p[2] / snap)};
}

[[nodiscard]] bool finite_point(const Point& p) {
  return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

// Clip a polygon against the half-space hn . x <= d, keeping the boundary side.  Consecutive points that coincide on
//  the clipping plane (within the snap grid) are collapsed so the result has no repeated vertices.
void clip_points(InlinedArray<Point, 10>& pts, const Vector& hn, float d, float snap) {
  const int n = pts.num();
  if (!n) return;
  InlinedArray<Point, 10> out;
  auto sval = [&](const Point& p) { return dot(p, hn) - d; };
  for_int(i, n) {
    const int ip = i ? i - 1 : n - 1;
    const float sc = sval(pts[i]);
    const float sp = sval(pts[ip]);
    const bool inc = sc <= 0.f;
    const bool inp = sp <= 0.f;
    if (inp != inc) {
      float t = sc / (sc - sp);  // Intersection weight on pts[ip] for interp(ip, i, t).
      t = clamp(t, 0.f, 1.f);
      const Point q = interp(pts[ip], pts[i], t);
      if (!out.num() || key_of(out.last(), snap) != key_of(q, snap)) out.push(q);
    }
    if (inc && (!out.num() || key_of(out.last(), snap) != key_of(pts[i], snap))) out.push(pts[i]);
  }
  pts = std::move(out);
}

// Clip a 2D polygon against the half-space na*x + nb*y <= c, collapsing snap-coincident consecutive points.
void clip_points_2d(InlinedArray<Vec2<float>, 10>& pts, float na, float nb, float c, float snap) {
  const int n = pts.num();
  if (!n) return;
  InlinedArray<Vec2<float>, 10> out;
  auto sval = [&](const Vec2<float>& p) { return na * p[0] + nb * p[1] - c; };
  auto same = [&](const Vec2<float>& a, const Vec2<float>& b) {
    return std::llround(a[0] / snap) == std::llround(b[0] / snap) &&
           std::llround(a[1] / snap) == std::llround(b[1] / snap);
  };
  for_int(i, n) {
    const int ip = i ? i - 1 : n - 1;
    const float sc = sval(pts[i]);
    const float sp = sval(pts[ip]);
    const bool inc = sc <= 0.f;
    const bool inp = sp <= 0.f;
    if (inp != inc) {
      const float t = clamp(sc / (sc - sp), 0.f, 1.f);  // Intersection weight on pts[ip].
      const Vec2<float> q = interp(pts[ip], pts[i], t);
      if (!out.num() || !same(out.last(), q)) out.push(q);
    }
    if (inc && (!out.num() || !same(out.last(), pts[i]))) out.push(pts[i]);
  }
  pts = std::move(out);
}

// Barycentric coordinates of p in the plane triangle tri (weights for tri[0], tri[1], tri[2]).
[[nodiscard]] Bary bary_of_point(const Vec3<Point>& tri, const Point& p) {
  const Vector e1 = tri[1] - tri[0];
  const Vector e2 = tri[2] - tri[0];
  const Vector ep = p - tri[0];
  const float d00 = dot(e1, e1), d01 = dot(e1, e2), d11 = dot(e2, e2);
  const float d20 = dot(ep, e1), d21 = dot(ep, e2);
  const float denom = assertx(d00 * d11 - d01 * d01);
  const float b1 = (d11 * d20 - d01 * d21) / denom;
  const float b2 = (d00 * d21 - d01 * d20) / denom;
  return Bary(1.f - b1 - b2, b1, b2);
}

// Newell-style normal of a planar polygon; its magnitude is twice the polygon area.
[[nodiscard]] Vector polygon_normal(CArrayView<Point> pts) {
  Vector ndir{};
  for_intL(i, 1, pts.num() - 1) ndir += cross(pts[i] - pts[0], pts[i + 1] - pts[0]);
  return ndir;
}

// Signed volume of an outward-oriented closed polyhedron (1/6 sum over fan triangles of each polygon).
[[nodiscard]] double enclosed_volume(CArrayView<const InlinedArray<Point, 10>*> faces) {
  double vol = 0.;
  for (const auto* fp : faces) {
    const auto& poly = *fp;
    for_intL(i, 1, poly.num() - 1) vol += dot(poly[0], cross(poly[i], poly[i + 1]));
  }
  return vol / 6.;
}

}  // namespace

void GMesh::bevel_edges(CArrayView<Edge> iedges, float width) {
  if (!std::isfinite(width) || width <= 0.f)
    throw std::invalid_argument("bevel_edges: width must be finite and positive");
  if (!iedges.num()) return;  // Empty selection leaves the mesh untouched.

  // --- Validate the selection: null, foreign, or duplicate edges are rejected (handles are compared, not dereferenced).
  Set<Edge> selected;
  for (Edge e : iedges) {
    if (!e) throw std::invalid_argument("bevel_edges: null edge in selection");
    bool belongs = false;
    for (Edge ee : edges())
      if (ee == e) {
        belongs = true;
        break;
      }
    if (!belongs) throw std::invalid_argument("bevel_edges: edge does not belong to this mesh");
    if (!selected.add(e)) throw std::invalid_argument("bevel_edges: duplicate edge in selection");
  }

  // --- Validate the mesh: closed nice triangular surface, strictly convex and outward-oriented.
  if (!num_vertices() || !num_faces()) throw std::invalid_argument("bevel_edges: empty mesh");
  for (Vertex v : vertices()) {
    if (!finite_point(point(v))) throw std::invalid_argument("bevel_edges: non-finite vertex position");
    if (!is_nice(v) || is_boundary(v)) throw std::invalid_argument("bevel_edges: mesh is not a closed nice surface");
  }
  for (Face f : faces()) {
    if (!is_triangle(f) || is_boundary(f))
      throw std::invalid_argument("bevel_edges: mesh must be a closed triangular surface");
  }
  for (Edge e : edges())
    if (is_boundary(e)) throw std::invalid_argument("bevel_edges: mesh is not closed");

  // Characteristic size used to scale all geometric tolerances.
  float scale = 0.f;
  for (Vertex v : vertices()) scale = max(scale, float(mag(point(v))));
  scale = max(scale, 1.f);
  const float snap = k_rel_snap * scale;
  const float eps = k_rel_eps * scale;
  const float area_eps = square(square(scale) * k_rel_eps);
  const double min_volume = k_rel_min_volume * double(scale) * scale * scale;
  auto kof = [&](const Point& p) { return key_of(p, snap); };

  struct FacePlane {
    Face f;
    Vector nor;
    Point p0;
  };
  Array<FacePlane> face_planes;
  Map<Face, int> face_plane_index;
  for (Face f : ordered_faces()) {
    const Vec3<Point> tp = triangle_points(f);
    const Vector ndir = get_normal_dir(tp);
    if (mag2(ndir) <= area_eps) throw std::invalid_argument("bevel_edges: degenerate triangle face");
    face_plane_index.enter(f, face_planes.num());
    face_planes.push(FacePlane{f, normalized(ndir), tp[0]});
  }
  // Strict convexity: every vertex not on a face must lie strictly behind that face's outward plane.
  for (const FacePlane& fp : face_planes) {
    Set<Vertex> incident;
    for (Vertex v : vertices(fp.f)) incident.enter(v);
    for (Vertex v : vertices()) {
      if (incident.contains(v)) continue;
      if (dot(fp.nor, point(v) - fp.p0) >= -eps)
        throw std::invalid_argument("bevel_edges: surface is not strictly convex");
    }
  }

  const Array<Face> old_faces(ordered_faces());
  const Array<Vertex> old_vertices(ordered_vertices());
  Map<VKey, Vertex, VKeyHash> old_key_vertex;
  for (Vertex v : old_vertices) old_key_vertex.enter(kof(point(v)), v);

  // --- Deterministic, order-independent per-edge half-space list.
  //  Half-space of edge e with outward unit normals n1, n2 and point p on it:
  //    (n1+n2)/|n1+n2| . (x - p) <= -width * sqrt(1 - (n1.n2)^2)
  struct BevPlane {
    Edge e;
    int id_lo, id_hi;
    Vector hn;
    float d;
  };
  Array<Edge> sorted_edges;
  for (Edge e : selected) sorted_edges.push(e);
  sort(sorted_edges, [&](Edge ea, Edge eb) {
    const int a1 = min(vertex_id(vertex1(ea)), vertex_id(vertex2(ea)));
    const int a2 = max(vertex_id(vertex1(ea)), vertex_id(vertex2(ea)));
    const int b1 = min(vertex_id(vertex1(eb)), vertex_id(vertex2(eb)));
    const int b2 = max(vertex_id(vertex1(eb)), vertex_id(vertex2(eb)));
    return a1 != b1 ? a1 < b1 : a2 < b2;
  });
  Array<BevPlane> planes;
  for (Edge e : sorted_edges) {
    const Vector n1 = face_planes[face_plane_index.retrieve(face1(e))].nor;
    const Vector n2 = face_planes[face_plane_index.retrieve(face2(e))].nor;
    const Vector sum = n1 + n2;
    const float smag = mag(sum);
    if (!(smag > k_rel_eps)) throw std::invalid_argument("bevel_edges: adjacent faces are coplanar");
    const Vector hn = sum / smag;
    const float nd = dot(n1, n2);
    const float root = sqrt(max(0.f, 1.f - nd * nd));
    const Point pe = point(vertex1(e));
    // Spec half-space uses the unnormalized normal (n1+n2): with unit hn the offset divides by |n1+n2|, so that
    //  the distance measured perpendicular to the edge inside either original face is exactly `width`.
    planes.push(BevPlane{e, min(vertex_id(vertex1(e)), vertex_id(vertex2(e))),
                         max(vertex_id(vertex1(e)), vertex_id(vertex2(e))), hn,
                         float(dot(hn, pe) - width * root / smag)});
  }

  // --- Clip every original triangle against all half-spaces; strings and UVs are computed while the original
  //     half-edge structure is still intact (the faces are destroyed later when the topology is rebuilt).
  struct OrigFaceResult {
    Face f;
    bool changed;
    Vec3<Point> tri;
    Vec3<Vertex> tverts;
    Vector nor;
    InlinedArray<Point, 10> poly;
    string sinfo;
    InlinedArray<string, 10> corner_uvs;  // Parallel to poly; empty entries carry no uv.
  };
  Array<OrigFaceResult> orig_results;

  for (Face f : old_faces) {
    const Vec3<Point> tp = triangle_points(f);
    const Vec3<Vertex> tv = triangle_vertices(f);
    InlinedArray<Point, 10> poly;
    for_int(i, 3) poly.push(tp[i]);
    for (const BevPlane& bp : planes) {
      clip_points(poly, bp.hn, bp.d, snap);
      if (poly.num() < 3) throw std::invalid_argument("bevel_edges: bevel width removes an original face");
    }
    bool changed = false;
    for (const Point& p : poly) {
      if (!finite_point(p)) throw std::invalid_argument("bevel_edges: non-finite clipped position");
      if (!old_key_vertex.contains(kof(p))) changed = true;
    }
    if (mag2(polygon_normal(poly)) <= area_eps)
      throw std::invalid_argument("bevel_edges: bevel creates a zero-area original face");
    string s0 = get_string(f) ? get_string(f) : "";
    OrigFaceResult r;
    r.f = f;
    r.changed = changed;
    r.tri = tp;
    r.tverts = tv;
    r.nor = get_normal(tp);
    r.poly = poly;
    r.sinfo = string_update(s0, "source_face", sform("%d", face_id(f)).c_str());
    // Face-corner UVs: corner string first, vertex string fallback; only when all three corners provide one.
    Vec3<Uv> uvs{};
    bool have_uv = true;
    for_int(i, 3) {
      Corner c = corner(tv[i], f);
      if (!parse_corner_key_vec(c, "uv", uvs[i])) have_uv = false;
    }
    for (const Point& p : poly) {
      if (have_uv) {
        const Bary bary = bary_of_point(tp, p);
        const Uv uvi = interp(uvs[0], uvs[1], uvs[2], bary);
        r.corner_uvs.push(sform("uv=(%g %g)", uvi[0], uvi[1]));
      } else {
        r.corner_uvs.push(string{});
      }
    }
    orig_results.push(std::move(r));
  }

  // --- Build the new facet of each plane directly as a 2D convex polygon inside the plane: it is the intersection
  //     of that plane with the original solid and with the other bevel half-spaces, so junctions share real vertices.
  struct NewFacet {
    int id_lo, id_hi;
    InlinedArray<Point, 10> poly;
    string sinfo;
  };
  Array<NewFacet> new_facets;
  const float big = 4.f * scale + 2.f * width + 1.f;  // Bounding square guaranteed to contain every facet.

  for_int(pi, planes.num()) {
    const BevPlane& bp = planes[pi];
    const Vector u = normalized(point(vertex2(bp.e)) - point(vertex1(bp.e)));  // Along the edge.
    const Vector w = normalized(cross(bp.hn, u));  // In the bevel plane, perpendicular to the edge.
    if (!(mag2(w) > square(k_rel_eps))) throw std::invalid_argument("bevel_edges: degenerate bevel plane");
    // Frame origin: project the edge endpoint onto the (offset) bevel plane, so to3() stays inside that plane.
    const Point edge_p = point(vertex1(bp.e));
    const Point o = edge_p - bp.hn * (dot(bp.hn, edge_p) - bp.d);
    auto to3 = [&](const Vec2<float>& q) -> Point { return Point(Vec3<float>(o) + u * q[0] + w * q[1]); };
    struct Half2 {
      float a, b, c;  // a*x + b*y <= c inside the bevel plane.
    };
    Array<Half2> halfs;
    auto add_half = [&](float a, float b, float c) {
      const float hn2 = sqrt(a * a + b * b);
      if (hn2 <= k_rel_eps) {
        if (c < -eps) throw std::invalid_argument("bevel_edges: bevel facet is infeasible");
        return;  // The half-space contains the whole bevel plane.
      }
      halfs.push(Half2{a / hn2, b / hn2, c / hn2});
    };
    // Traces of all original face planes (only the two adjacent faces can bind in a feasible bevel).
    for (const FacePlane& fp : face_planes)
      add_half(dot(fp.nor, u), dot(fp.nor, w), float(dot(fp.nor, fp.p0 - o)));
    // Traces of every other bevel half-space.
    for_int(pj, planes.num()) {
      if (pj == pi) continue;
      const BevPlane& q = planes[pj];
      add_half(dot(q.hn, u), dot(q.hn, w), float(q.d - dot(q.hn, o)));
    }
    InlinedArray<Vec2<float>, 10> q2{Vec2<float>(-big, -big), Vec2<float>(big, -big), Vec2<float>(big, big),
                                     Vec2<float>(-big, big)};
    for (const Half2& h : halfs) {
      clip_points_2d(q2, h.a, h.b, h.c, snap);
      if (q2.num() < 3) throw std::invalid_argument("bevel_edges: bevel width removes a bevel facet");
    }
    InlinedArray<Point, 10> seed;
    for (const Vec2<float>& q : q2) seed.push(to3(q));
    if (dot(polygon_normal(seed), bp.hn) < 0.f) std::ranges::reverse(seed);
    for (const Point& p : seed) {
      if (!finite_point(p)) throw std::invalid_argument("bevel_edges: non-finite bevel position");
    }
    if (mag2(polygon_normal(seed)) <= area_eps)
      throw std::invalid_argument("bevel_edges: bevel creates a zero-area bevel face");
    NewFacet nf;
    nf.id_lo = bp.id_lo;
    nf.id_hi = bp.id_hi;
    nf.poly = std::move(seed);
    nf.sinfo = sform("source_edge=(%d %d)", bp.id_lo, bp.id_hi);
    new_facets.push(std::move(nf));
  }

  // --- Deterministically merge points that coincide by construction but were produced by different floating-point
  //     code paths (3D face clipping vs. 2D facet intersection), so junctions share one real vertex.  Union-find
  //     over every output point; an old vertex, when part of a component, always supplies the canonical position.
  Array<Point> uniq;
  Map<VKey, int, VKeyHash> point_index;
  Array<char> is_old_point;
  auto add_unique = [&](const Point& p, bool is_old) -> int {
    const VKey k = kof(p);
    if (const int* pi = point_index.find_ptr(k)) return *pi;
    const int idx = uniq.num();
    point_index.enter(k, idx);
    uniq.push(p);
    is_old_point.push(char(is_old));
    return idx;
  };
  for (Vertex v : old_vertices) add_unique(point(v), true);
  for (const OrigFaceResult& r : orig_results)
    for (const Point& p : r.poly) add_unique(p, false);
  for (const NewFacet& nf : new_facets)
    for (const Point& p : nf.poly) add_unique(p, false);
  Array<int> parent;
  for_int(i, uniq.num()) parent.push(i);
  auto find_root = [&](int x) {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
  };
  auto union_pts = [&](int a, int b) {
    a = find_root(a), b = find_root(b);
    if (a != b) parent[b] = a;
  };
  const float merge_tol2 = square(1e-5f * scale);
  for_int(i, uniq.num())
    for_intL(j, i + 1, uniq.num())
      if (dist2(uniq[i], uniq[j]) <= merge_tol2) union_pts(i, j);
  // Canonical point per component: the old vertex if present, else the smallest-id member (deterministic).
  Array<int> root_rep(uniq.num(), -1);
  for_int(i, uniq.num()) {
    const int root = find_root(i);
    int& rep = root_rep[root];
    if (rep < 0 || (is_old_point[i] && !is_old_point[rep]) ||
        (!is_old_point[i] && !is_old_point[rep] && i < rep))
      rep = i;
  }
  Map<VKey, Point, VKeyHash> canonical;
  for_int(i, uniq.num()) {
    const int rep = root_rep[find_root(i)];
    canonical.enter(kof(uniq[i]), uniq[rep]);  // Every merged key resolves to the same canonical point.
  }
  auto canon = [&](const Point& p) -> Point { return canonical.retrieve(kof(p)); };
  // Apply the merge to every output polygon and drop repeated consecutive corners.  Original-face polygons carry a
  //  parallel corner-uv array, so it is compacted in lockstep; bevel facets have no corner data.
  auto weld_points = [&](CArrayView<Point> pts, InlinedArray<Point, 10>& outp) {
    for (const Point& p : pts) {
      const Point q = canon(p);
      if (outp.num() && kof(outp.last()) == kof(q)) continue;
      outp.push(q);
    }
    if (outp.num() >= 2 && kof(outp[0]) == kof(outp.last())) outp.pop();
  };
  for (OrigFaceResult& r : orig_results) {
    InlinedArray<Point, 10> outp;
    InlinedArray<string, 10> outu;
    for_int(i, r.poly.num()) {
      const Point q = canon(r.poly[i]);
      if (outp.num() && kof(outp.last()) == kof(q)) continue;
      outp.push(q);
      outu.push(r.corner_uvs[i]);
    }
    if (outp.num() >= 2 && kof(outp[0]) == kof(outp.last())) {
      outp.pop();
      outu.pop();
    }
    r.poly = std::move(outp);
    r.corner_uvs = std::move(outu);
  }
  for (NewFacet& nf : new_facets) {
    InlinedArray<Point, 10> outp;
    weld_points(nf.poly, outp);
    nf.poly = std::move(outp);
  }

  // --- Classify old vertices; check orientation and enclosed volume before touching the topology.
  Set<VKey, VKeyHash> used_keys;
  for (const OrigFaceResult& r : orig_results)
    for (const Point& p : r.poly) used_keys.add(kof(p));
  for (const NewFacet& nf : new_facets)
    for (const Point& p : nf.poly) used_keys.add(kof(p));
  Array<Vertex> surviving_old;
  Array<Vertex> doomed_old;
  for (Vertex v : old_vertices) {
    if (used_keys.contains(kof(point(v))))
      surviving_old.push(v);
    else
      doomed_old.push(v);
  }
  for (const OrigFaceResult& r : orig_results) {
    if (r.poly.num() < 3 || mag2(polygon_normal(r.poly)) <= area_eps)
      throw std::invalid_argument("bevel_edges: bevel creates a zero-area original face");
    if (dot(polygon_normal(r.poly), r.nor) <= 0.f)
      throw std::invalid_argument("bevel_edges: clipped face lost its outward orientation");
  }
  for (const NewFacet& nf : new_facets) {
    if (nf.poly.num() < 3 || mag2(polygon_normal(nf.poly)) <= area_eps)
      throw std::invalid_argument("bevel_edges: bevel creates a zero-area bevel face");
  }
  Array<const InlinedArray<Point, 10>*> volume_faces;
  for (const OrigFaceResult& r : orig_results) volume_faces.push(&r.poly);
  for (const NewFacet& nf : new_facets) volume_faces.push(&nf.poly);
  if (!(enclosed_volume(volume_faces) > min_volume))
    throw std::invalid_argument("bevel_edges: bevel leaves no enclosed volume");

  Array<VKey> new_keys;
  for (const VKey& k : used_keys)
    if (!old_key_vertex.contains(k)) new_keys.push(k);
  sort(new_keys);

  // --- Rebuild the topology with the change stream silenced; the edit script is emitted afterwards as one unit.
  Array<int> destroy_face_ids;
  for (const OrigFaceResult& r : orig_results)
    if (r.changed) destroy_face_ids.push(face_id(r.f));
  sort(destroy_face_ids);
  Array<int> destroy_vertex_ids;
  for (Vertex v : doomed_old) destroy_vertex_ids.push(vertex_id(v));
  sort(destroy_vertex_ids);

  std::ostream* tos = change_stream();
  set_change_stream(nullptr);

  // Create the new vertices first so every output point can be resolved to a concrete vertex handle.
  Map<VKey, Vertex, VKeyHash> key_to_vertex;
  for (Vertex v : surviving_old) key_to_vertex.enter(kof(point(v)), v);
  Array<Point> new_vertex_points;
  for (const VKey& k : new_keys) {
    const Point p = canonical.retrieve(k);
    Vertex vn = create_bevel_vertex(p);
    key_to_vertex.enter(k, vn);
    new_vertex_points.push(p);
  }

  // An old edge survives iff both endpoints survive and the pair appears as consecutive corners of an output face.
  //  Such MEdge objects are detached and reattached (rather than deleted) so their handle, flags, and strings survive.
  Set<int64_t> output_edge_keys;
  auto note_poly_edges = [&](CArrayView<Point> pts) {
    const int n = pts.num();
    for_int(i, n) {
      const int in = i + 1 == n ? 0 : i + 1;
      const Vertex va = key_to_vertex.retrieve(kof(pts[i]));
      const Vertex vb = key_to_vertex.retrieve(kof(pts[in]));
      output_edge_keys.add(bevel_edge_key(vertex_id(va), vertex_id(vb)));  // Shared edges are seen twice.
    }
  };
  for (const OrigFaceResult& r : orig_results) note_poly_edges(r.poly);
  for (const NewFacet& nf : new_facets) note_poly_edges(nf.poly);
  Set<int64_t> keep_edge_keys;
  for (Edge e : edges()) {
    const int64_t key = bevel_edge_key(vertex_id(vertex1(e)), vertex_id(vertex2(e)));
    if (output_edge_keys.contains(key)) keep_edge_keys.enter(key);
  }

  // Preserved edges keep their MEdge (and its string) in memory; record those strings now, before the topology is
  //  detached, so the text replay restores them.
  struct EdgeStringRecord {
    int id1, id2;
    string sinfo;
  };
  Array<EdgeStringRecord> edge_string_records;
  for (int64_t key : keep_edge_keys) {
    Edge e = query_edge(id_vertex(int(key >> 32)), id_vertex(int(key & 0xffffffffLL)));
    if (e && get_string(e))
      edge_string_records.push(EdgeStringRecord{vertex_id(vertex1(e)), vertex_id(vertex2(e)), get_string(e)});
  }

  Map<int64_t, Edge> kept_edges;
  for (const OrigFaceResult& r : orig_results)
    if (r.changed) bevel_detach_face(r.f, keep_edge_keys, kept_edges);
  for (Vertex v : doomed_old) destroy_bevel_vertex(v);

  struct FaceRecord {
    int id;
    Array<int> vids;
    string sinfo;
    InlinedArray<string, 10> corner_uvs;
  };
  Array<FaceRecord> face_records;
  struct FaceStringRecord {
    int id;
    string sinfo;
  };
  Array<FaceStringRecord> face_string_records;  // MStrF lines for uncut original faces that gain source_face.
  auto create_output_face = [&](CArrayView<Point> pts, const string& sinfo,
                                const InlinedArray<string, 10>& corner_uvs) {
    InlinedArray<Vertex, 10> va;
    for (const Point& p : pts) va.push(key_to_vertex.retrieve(kof(p)));
    Face fn = bevel_create_face(CArrayView<Vertex>(va.data(), va.num()), kept_edges, sinfo.c_str());
    FaceRecord rec;
    rec.id = face_id(fn);
    for (Vertex v : va) rec.vids.push(vertex_id(v));
    rec.sinfo = sinfo;
    rec.corner_uvs = corner_uvs;
    for_int(i, rec.corner_uvs.num()) {
      if (rec.corner_uvs[i].empty()) continue;
      set_bevel_corner(corner(va[i], fn), rec.corner_uvs[i].c_str());
    }
    face_records.push(std::move(rec));
  };
  for (const OrigFaceResult& r : orig_results) {
    if (!r.changed) continue;  // Uncut faces keep their original handle, edges, and corner attributes.
    create_output_face(r.poly, r.sinfo, r.corner_uvs);
  }
  for (const NewFacet& nf : new_facets) {
    const InlinedArray<string, 10> no_uvs;  // Bevel faces never inherit UVs.
    create_output_face(nf.poly, nf.sinfo, no_uvs);
  }
  // Kept edges shared with an uncut neighbor were reattached through their opposite hedge; the rest were consumed
  //  by bevel_create_face().  Every remaining one must already be attached to a live hedge.
  for (const auto& [key, e] : kept_edges) assertx(e->_herep != nullptr);
  kept_edges.clear();
  // Uncut original faces keep their handle but still gain source_face; recorded as an MStrF string replacement.
  for (const OrigFaceResult& r : orig_results) {
    if (r.changed) continue;
    string s0 = get_string(r.f) ? get_string(r.f) : "";
    string s1 = string_update(s0, "source_face", sform("%d", face_id(r.f)).c_str());
    set_string(r.f, s1.c_str());  // The change stream is silenced; the edit is emitted explicitly below.
    face_string_records.push(FaceStringRecord{face_id(r.f), std::move(s1)});
  }

  set_change_stream(tos);
  ok();  // Verify the half-edge structure: manifold, matched sym edges, no duplicate vertices per face.
  for (Edge e : edges())
    if (is_boundary(e)) throw std::invalid_argument("bevel_edges: internal error: open boundary in result");

  // --- Emit the ordered edit script so that read() on the seed reproduces the same result.
  if (tos) {
    for (int id : destroy_face_ids) *tos << "MBevDF " << id << '\n';
    for (int id : destroy_vertex_ids) *tos << "MBevDV " << id << '\n';
    for (const Point& p : new_vertex_points)
      *tos << sform("MBevV %g %g %g\n", p[0], p[1], p[2]);
    for (const FaceRecord& rec : face_records) {
      *tos << "MBevF";
      for (int vid : rec.vids) *tos << ' ' << vid;
      if (!rec.sinfo.empty()) *tos << " {" << rec.sinfo << '}';
      *tos << '\n';
      for_int(i, rec.corner_uvs.num()) {
        if (rec.corner_uvs[i].empty()) continue;
        *tos << "Corner " << rec.vids[i] << ' ' << rec.id << " {" << rec.corner_uvs[i] << "}\n";
      }
    }
    for (const FaceStringRecord& rec : face_string_records) {
      *tos << "MStrF " << rec.id;
      if (!rec.sinfo.empty()) *tos << " {" << rec.sinfo << '}';
      *tos << '\n';
    }
    sort(edge_string_records, [](const EdgeStringRecord& a, const EdgeStringRecord& b) {
      return a.id1 != b.id1 ? a.id1 < b.id1 : a.id2 < b.id2;
    });
    for (const EdgeStringRecord& rec : edge_string_records)
      *tos << "Edge " << rec.id1 << ' ' << rec.id2 << " {" << rec.sinfo << "}\n";
    tos->flush();
  }
}

}  // namespace hh
