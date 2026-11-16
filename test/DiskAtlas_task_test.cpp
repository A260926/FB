// -*- C++ -*-  Copyright (c) Microsoft Corporation; see LICENSE.
// Tests for hh::make_disk_atlas().  This file defines the task_cases table; it has no main().

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "libHh/DiskAtlas.h"
#include "libHh/MathOp.h"  // TAU
#include "libHh/MeshOp.h"  // mesh_genus(), gather_boundary()
#include "task_support/check.h"

using namespace hh;

namespace {

// ---------------------------------------------------------------------------
// Mesh construction helpers.

struct TorusGrid {
  GMesh mesh;
  std::vector<Vertex> vertices;
  std::vector<int> ids;
  int nu{0}, nv{0};
  int at(int i, int j) const { return (j % nv) * nu + (i % nu); }
  Vertex vertex(int i, int j) const { return vertices[at(i, j)]; }
};

// A torus with a deliberately non-uniform, wobbly shape.
TorusGrid make_torus(int nu, int nv, const Point& offset, float phase) {
  TorusGrid g;
  g.nu = nu;
  g.nv = nv;
  for (int j = 0; j < nv; ++j) {
    for (int i = 0; i < nu; ++i) {
      const float u = TAU * float(i) / float(nu), w = TAU * float(j) / float(nv);
      const float rr = 0.75f + 0.30f * std::sin(2.f * w + phase) + 0.16f * std::cos(3.f * u - 0.5f * phase);
      const float RR = 2.3f + 0.25f * std::sin(u + phase);
      Vertex v = g.mesh.create_vertex();
      g.vertices.push_back(v);
      g.ids.push_back(g.mesh.vertex_id(v));
      g.mesh.set_point(v, offset + Point((RR + rr * std::cos(w)) * std::cos(u), (RR + rr * std::cos(w)) * std::sin(u),
                                         rr * std::sin(w) + 0.18f * std::sin(2.f * u) * std::cos(w)));
    }
  }
  for (int j = 0; j < nv; ++j) {
    for (int i = 0; i < nu; ++i) {
      g.mesh.create_face(g.vertex(i, j), g.vertex(i + 1, j), g.vertex(i + 1, j + 1));
      g.mesh.create_face(g.vertex(i, j), g.vertex(i + 1, j + 1), g.vertex(i, j + 1));
    }
  }
  return g;
}

// Remove the star of vertex (0,0); return the vertex ids around the resulting hole in cyclic order.
std::vector<int> punch_hole(GMesh& mesh, const TorusGrid& g) {
  std::vector<Face> dead;
  for (Face f : mesh.faces(g.vertex(0, 0))) dead.push_back(f);
  for (Face f : dead) mesh.destroy_face(f);
  mesh.destroy_vertex(g.vertex(0, 0));
  Edge e0 = nullptr;
  for (Edge e : mesh.edges())
    if (mesh.is_boundary(e)) {
      e0 = e;
      break;
    }
  std::vector<int> loop;
  Edge e = e0;
  for (;;) {
    loop.push_back(mesh.vertex_id(mesh.vertex1(e)));
    e = mesh.ccw_boundary(e);
    if (e == e0) break;
  }
  return loop;
}

// Connected sum of two punched tori, matched along their hole boundaries with opposite directions.
GMesh connected_sum(GMesh& a, const std::vector<int>& la, GMesh& b, const std::vector<int>& lb) {
  GMesh out;
  std::map<int, Vertex> map_a, map_b;
  std::set<int> b_on_loop;
  for (size_t k = 0; k < lb.size(); ++k) b_on_loop.insert(lb[k]);
  for (Vertex v : a.vertices()) {
    if (!a.id_retrieve_vertex(a.vertex_id(v))) continue;
    Vertex nv = out.create_vertex();
    out.set_point(nv, a.point(v));
    map_a[a.vertex_id(v)] = nv;
  }
  // Match B's hole loop to A's loop in reverse order.
  for (size_t k = 0; k < lb.size(); ++k) {
    const int ai = la[la.size() - 1 - k];
    map_b[lb[k]] = map_a.at(ai);
  }
  for (Vertex v : b.vertices()) {
    const int id = b.vertex_id(v);
    if (!b.id_retrieve_vertex(id)) continue;
    if (b_on_loop.count(id)) continue;
    Vertex nv = out.create_vertex();
    out.set_point(nv, b.point(v));
    map_b[id] = nv;
  }
  auto copy_faces = [&](GMesh& src, std::map<int, Vertex>& map) {
    for (Face f : src.faces()) {
      std::vector<Vertex> va;
      for (Vertex v : src.vertices(f)) va.push_back(map.at(src.vertex_id(v)));
      out.create_face(CArrayView<Vertex>(va.data(), int(va.size())));
    }
  };
  copy_faces(a, map_a);
  copy_faces(b, map_b);
  return out;
}

// Genus-2 surface: connected sum of two wobbly tori.
GMesh make_genus2() {
  TorusGrid a = make_torus(6, 4, Point(0.f, 0.f, 0.f), 0.f);
  TorusGrid b = make_torus(6, 4, Point(0.f, 0.f, 9.f), 1.2f);
  const std::vector<int> la = punch_hole(a.mesh, a);
  const std::vector<int> lb = punch_hole(b.mesh, b);
  return connected_sum(a.mesh, la, b.mesh, lb);
}

// A nice trihedral torus with discontinuous per-corner normals and other attributes.  Also carries a
// vertex-level "uv" that must not survive, plus normal/color fallbacks at some vertices.
GMesh make_attributed_torus(int nu, int nv) {
  TorusGrid g = make_torus(nu, nv, Point(0.f, 0.f, 0.f), 0.4f);
  GMesh& mesh = g.mesh;
  int fi = 0;
  for (Face f : mesh.ordered_faces()) {
    mesh.update_string(f, "rgb", sform("(%g %g %g)", 0.1f + 0.01f * fi, 0.2f, 0.3f).c_str());
    mesh.update_string(f, "marker", (fi & 1) ? "odd" : "even");
    int ci = 0;
    for (Corner c : mesh.corners(f)) {
      const Point& p = mesh.point(mesh.corner_vertex(c));
      // Leave the corners at the one vertex that has a vertex-level normal without a corner-level normal, so that
      // the vertex-level fallback is exercised without ambiguity.
      if (mesh.corner_vertex(c) != g.vertices[0])
        mesh.update_string(c, "normal", sform("(%g %g %g)", 0.1f * ci + 0.01f * fi, p[0], p[1]).c_str());
      mesh.update_string(c, "rgb", sform("(%g %g %g)", 0.05f * ci, 0.01f * fi, 0.7f).c_str());
      ++ci;
    }
    ++fi;
  }
  mesh.update_string(g.vertices[0], "normal", "(0 0 1)");
  mesh.update_string(g.vertices[0], "color", "red");
  mesh.update_string(g.vertices[1], "uv", "(0.5 0.5)");
  for (Vertex v : mesh.vertices()) mesh.flags(v).flag(GMesh::vflag_cusp) = (mesh.vertex_id(v) & 1) != 0;
  return std::move(mesh);
}

// ---------------------------------------------------------------------------
// Independent graph helpers (all work on original vertex ids / face ids).

struct IdEdge {
  int a, b;
  double length;
};

std::vector<IdEdge> collect_edges(const GMesh& mesh) {
  std::vector<IdEdge> result;
  for (Edge e : mesh.edges()) {
    int a = mesh.vertex_id(mesh.vertex1(e)), b = mesh.vertex_id(mesh.vertex2(e));
    if (a > b) std::swap(a, b);
    result.push_back(IdEdge{a, b, double(mesh.length(e))});
  }
  return result;
}

[[nodiscard]] std::map<int, double> dijkstra(const GMesh& mesh, int root_id) {
  std::map<int, std::vector<std::pair<int, double>>> adj;
  for (const IdEdge& e : collect_edges(mesh)) {
    adj[e.a].push_back({e.b, e.length});
    adj[e.b].push_back({e.a, e.length});
  }
  std::map<int, double> dist;
  using QE = std::pair<double, int>;
  std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
  dist[root_id] = 0.;
  pq.push({0., root_id});
  while (!pq.empty()) {
    const auto [d, v] = pq.top();
    pq.pop();
    if (d > dist[v]) continue;
    for (const auto& [w, l] : adj[v]) {
      const double nd = d + l;
      auto it = dist.find(w);
      if (it == dist.end() || nd < it->second - 1e-12) {
        dist[w] = nd;
        pq.push({nd, w});
      }
    }
  }
  return dist;
}

double edge_length(const GMesh& mesh, const std::pair<int, int>& p) {
  return double(mesh.length(mesh.edge(mesh.id_vertex(p.first), mesh.id_vertex(p.second))));
}

// Sum of lengths of a set of id pairs.
double total_length(const GMesh& mesh, const std::vector<std::pair<int, int>>& edges) {
  double s = 0.;
  for (const auto& e : edges) s += edge_length(mesh, e);
  return s;
}

// Verify that a set of n-1 vertex-id edges is a spanning tree rooted at root; return path distances.
bool tree_distances(const std::vector<std::pair<int, int>>& edges, int root, int num_vertices,
                    std::map<int, double>* distances, const GMesh& mesh) {
  if (int(edges.size()) != num_vertices - 1) return false;
  std::map<int, std::vector<std::pair<int, double>>> adj;
  std::set<int> ids;
  for (const auto& e : edges) {
    if (e.first == e.second) return false;
    const double l = edge_length(mesh, e);
    adj[e.first].push_back({e.second, l});
    adj[e.second].push_back({e.first, l});
    ids.insert(e.first);
    ids.insert(e.second);
  }
  if (int(ids.size()) != num_vertices) return false;
  distances->clear();
  std::vector<int> stack{root};
  (*distances)[root] = 0.;
  while (!stack.empty()) {
    const int v = stack.back();
    stack.pop_back();
    for (const auto& [w, l] : adj[v]) {
      if (distances->count(w)) continue;
      (*distances)[w] = (*distances)[v] + l;
      stack.push_back(w);
    }
  }
  if (int(distances->size()) != num_vertices) return false;  // Acyclic and connected.
  return true;
}

// Maximum spanning tree of the face dual graph on the given allowed primal edges.  Returns primal id pairs.
std::vector<std::pair<int, int>> max_dual_tree(const GMesh& mesh, const std::set<std::pair<int, int>>& forbidden) {
  struct DE {
    double length;
    int a, b;  // face ids
    std::pair<int, int> primal;
  };
  std::vector<DE> dual;
  for (Edge e : mesh.edges()) {
    int a = mesh.vertex_id(mesh.vertex1(e)), b = mesh.vertex_id(mesh.vertex2(e));
    if (a > b) std::swap(a, b);
    if (forbidden.count({a, b})) continue;
    const int f1 = mesh.face_id(mesh.face1(e));
    const int f2 = mesh.face_id(mesh.face2(e));
    dual.push_back(DE{double(mesh.length(e)), f1, f2, {a, b}});
  }
  std::sort(dual.begin(), dual.end(), [](const DE& x, const DE& y) {
    if (x.length != y.length) return x.length > y.length;
    return x.primal < y.primal;
  });
  std::map<int, int> uf;
  auto find = [&](int x) {
    while (uf[x] != x) x = uf[x] = uf[uf[x]];
    return x;
  };
  for (Face f : mesh.faces()) uf[mesh.face_id(f)] = mesh.face_id(f);
  std::vector<std::pair<int, int>> tree;
  for (const DE& de : dual) {
    const int ra = find(de.a), rb = find(de.b);
    if (ra == rb) continue;
    uf[ra] = rb;
    tree.push_back(de.primal);
    if (int(tree.size()) == mesh.num_faces() - 1) break;
  }
  return tree;
}

// The 2-core of a set of vertex-id edges.
std::set<std::pair<int, int>> two_core(const std::vector<std::pair<int, int>>& edges) {
  std::set<std::pair<int, int>> live(edges.begin(), edges.end());
  std::map<int, std::set<std::pair<int, int>>> incident;
  for (const auto& e : live) {
    incident[e.first].insert(e);
    incident[e.second].insert(e);
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto& [v, es] : incident) {
      if (es.size() != 1) continue;
      const auto e = *es.begin();
      live.erase(e);
      incident[e.first].erase(e);
      incident[e.second].erase(e);
      changed = true;
      break;
    }
  }
  return live;
}

bool is_sorted_unique(const std::vector<std::pair<int, int>>& v) {
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i].first >= v[i].second) return false;
    if (i && !(v[i - 1] < v[i])) return false;
  }
  return true;
}

std::set<std::pair<int, int>> as_set(const std::vector<std::pair<int, int>>& v) {
  return std::set<std::pair<int, int>>(v.begin(), v.end());
}

// ---------------------------------------------------------------------------
// Output-mesh helpers.

int top_level_key_int(const char* str, const char* key) {
  std::string storage;
  const char* v = GMesh::string_key(storage, str, key);
  if (!v) return -1;
  return std::atoi(v);
}

// The boundary vertex cycle as output vertices, in one consistent direction.
std::vector<Vertex> boundary_cycle(const GMesh& mesh, std::vector<Edge>* edges_out = nullptr) {
  Edge e0 = nullptr;
  for (Edge e : mesh.edges())
    if (mesh.is_boundary(e)) {
      e0 = e;
      break;
    }
  std::vector<Vertex> cycle;
  std::vector<Edge> edges;
  Edge e = e0;
  Vertex start = mesh.vertex1(e0);
  Vertex cur = start;
  for (;;) {
    cycle.push_back(cur);
    const Vertex a = mesh.vertex1(e), b = mesh.vertex2(e);
    const Vertex other = a == cur ? b : a;
    edges.push_back(e);
    Edge next = nullptr;
    for (Edge x : mesh.edges(other))
      if (mesh.is_boundary(x) && x != e) {
        next = x;
        break;
      }
    if (!next) break;
    cur = other;
    e = next;
    if (cur == start) break;
  }
  if (edges_out) *edges_out = edges;
  return cycle;
}

Vec2<float> corner_uv(const GMesh& mesh, Vertex v) {
  for (Face f : mesh.faces(v))
    for (Corner c : mesh.corners(f))
      if (mesh.corner_vertex(c) == v) {
        float uv[2] = {0.f, 0.f};
        if (mesh.parse_corner_key_vec(c, "uv", uv)) return Vec2<float>(uv[0], uv[1]);
      }
  return Vec2<float>(0.f, 0.f);
}

std::set<Vertex> distinct_neighbors(const GMesh& mesh, Vertex v) {
  std::set<Vertex> s;
  for (Vertex w : mesh.vertices(v)) s.insert(w);
  return s;
}

// The largest |uv - mean(neighbor uv)| over interior vertices, using the mesh adjacency.
double measured_average_residual(const GMesh& mesh) {
  double residual = 0.;
  for (Vertex v : mesh.vertices()) {
    bool on_boundary = false;
    for (Edge e : mesh.edges(v))
      if (mesh.is_boundary(e)) on_boundary = true;
    if (on_boundary) continue;
    const std::set<Vertex> nbrs = distinct_neighbors(mesh, v);
    if (nbrs.empty()) continue;
    double sx = 0., sy = 0.;
    for (Vertex w : nbrs) {
      const Vec2<float> uv = corner_uv(mesh, w);
      sx += uv[0];
      sy += uv[1];
    }
    const Vec2<float> uv = corner_uv(mesh, v);
    residual = std::max(residual, std::abs(double(uv[0]) - sx / nbrs.size()));
    residual = std::max(residual, std::abs(double(uv[1]) - sy / nbrs.size()));
  }
  return residual;
}

// Check all structural invariants of the cut disk; returns false on failure.
bool verify_disk(const GMesh& source, const GMesh& result, const DiskAtlasInfo& info) {
  if (result.num_faces() != source.num_faces()) return false;
  if (result.num_edges() != source.num_edges() + int(info.cut_edges.size())) return false;
  if (result.num_vertices() - result.num_edges() + result.num_faces() != 1) return false;
  // One face-connected component.
  {
    std::set<int> seen;
    std::vector<Face> stack;
    Face first = *result.faces().begin();
    seen.insert(result.face_id(first));
    stack.push_back(first);
    while (!stack.empty()) {
      Face f = stack.back();
      stack.pop_back();
      for (Face g : result.faces(f))
        if (seen.insert(result.face_id(g)).second) stack.push_back(g);
    }
    if (int(seen.size()) != result.num_faces()) return false;
  }
  // Exactly one simple boundary loop, of length twice the number of cut edges.
  int boundary_edges = 0;
  for (Edge e : result.edges())
    if (result.is_boundary(e)) ++boundary_edges;
  if (boundary_edges != 2 * int(info.cut_edges.size())) return false;
  const std::vector<Vertex> cycle = boundary_cycle(result);
  if (int(cycle.size()) != boundary_edges) return false;
  {
    std::set<Vertex> seen;
    for (Vertex v : cycle)
      if (!seen.insert(v).second) return false;
    for (Vertex v : cycle) {
      int degree = 0;
      for (Edge e : result.edges(v))
        if (result.is_boundary(e)) ++degree;
      if (degree != 2) return false;
    }
  }
  // Provenance keys: every face once, and the original face's ordered vertex ids and positions preserved.
  std::map<int, Face> source_faces;
  for (Face f : source.faces()) source_faces[source.face_id(f)] = f;
  std::set<int> seen_faces;
  std::map<std::pair<int, int>, int> edge_multiplicity;
  for (Face f : result.faces()) {
    const int sid = top_level_key_int(result.get_string(f), "source_face");
    if (sid < 0 || !source_faces.count(sid)) return false;
    if (!seen_faces.insert(sid).second) return false;
    const Face sf = source_faces.at(sid);
    std::vector<Vertex> of, sv;
    for (Vertex v : result.vertices(f)) of.push_back(v);
    for (Vertex v : source.vertices(sf)) sv.push_back(v);
    if (of.size() != sv.size()) return false;
    for (size_t i = 0; i < of.size(); ++i) {
      const int oid = top_level_key_int(result.get_string(of[i]), "source_vertex");
      if (oid != source.vertex_id(sv[i])) return false;
      const Point& p = result.point(of[i]);
      const Point& q = source.point(sv[i]);
      if (p[0] != q[0] || p[1] != q[1] || p[2] != q[2]) return false;
    }
  }
  if (int(seen_faces.size()) != source.num_faces()) return false;
  // Edge provenance: one or two copies; cut edges are exactly the boundary edges and appear twice.
  std::set<std::pair<int, int>> cut_set = as_set(info.cut_edges);
  for (Edge e : result.edges()) {
    std::string storage;
    const char* v = GMesh::string_key(storage, result.get_string(e), "source_edge");
    if (!v) return false;
    int a = 0, b = 0;
    if (std::sscanf(v, "(%d %d)", &a, &b) != 2) return false;
    if (a > b) std::swap(a, b);
    ++edge_multiplicity[{a, b}];
    const bool is_cut = cut_set.count({a, b}) != 0;
    if (is_cut != result.is_boundary(e)) return false;
  }
  for (const auto& [pair, count] : edge_multiplicity) {
    if (cut_set.count(pair)) {
      if (count != 2) return false;
    } else if (count != 1) {
      return false;  // Non-cut edges are shared by their two original faces.
    }
  }
  // Non-cut edges keep the same two incident source faces on both sides.
  for (Edge e : result.edges()) {
    if (result.is_boundary(e)) continue;
    std::string storage;
    const char* v = GMesh::string_key(storage, result.get_string(e), "source_edge");
    int a = 0, b = 0;
    std::sscanf(v, "(%d %d)", &a, &b);
    if (a > b) std::swap(a, b);
    const Edge se = source.edge(source.id_vertex(a), source.id_vertex(b));
    std::set<int> expected{source.face_id(source.face1(se)), source.face_id(source.face2(se))};
    std::set<int> got{top_level_key_int(result.get_string(result.face1(e)), "source_face"),
                      top_level_key_int(result.get_string(result.face2(e)), "source_face")};
    if (expected != got) return false;
  }
  // Every vertex string carries its source id, and split copies are grouped correctly.
  {
    std::map<int, int> copies;
    std::map<int, int> cut_degree;
    for (const auto& p : info.cut_edges) {
      ++cut_degree[p.first];
      ++cut_degree[p.second];
    }
    for (Vertex v : result.vertices()) {
      const int sid = top_level_key_int(result.get_string(v), "source_vertex");
      if (sid < 0 || !source.id_retrieve_vertex(sid)) return false;
      ++copies[sid];
    }
    // A vertex is duplicated once per incident cut edge sector: max(1, number of incident cut edges).
    for (Vertex v : source.vertices()) {
      const int sid = source.vertex_id(v);
      const int expected = std::max(1, cut_degree[sid]);
      if (copies[sid] != expected) return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Cut-graph verification common to all surfaces.

bool verify_cut_graph(const GMesh& source, int root_id, const DiskAtlasInfo& info, int expected_genus) {
  const std::vector<IdEdge> edges = collect_edges(source);
  const int nv = source.num_vertices();
  const int nf = source.num_faces();
  if (!is_sorted_unique(info.primal_tree_edges)) return false;
  if (!is_sorted_unique(info.dual_tree_edges)) return false;
  if (!is_sorted_unique(info.generator_edges)) return false;
  if (!is_sorted_unique(info.cut_edges)) return false;
  // All ids are valid.
  std::set<int> valid_ids;
  for (Vertex v : source.vertices()) valid_ids.insert(source.vertex_id(v));
  for (const auto* list : {&info.primal_tree_edges, &info.dual_tree_edges, &info.generator_edges, &info.cut_edges}) {
    for (const auto& p : *list)
      if (!valid_ids.count(p.first) || !valid_ids.count(p.second)) return false;
  }
  // T is a shortest-path spanning tree from the root.
  std::map<int, double> tree_dist;
  if (!tree_distances(info.primal_tree_edges, root_id, nv, &tree_dist, source)) return false;
  const std::map<int, double> shortest = dijkstra(source, root_id);
  if (shortest.size() != size_t(nv)) return false;
  for (const auto& [id, d] : shortest) {
    auto it = tree_dist.find(id);
    if (it == tree_dist.end()) return false;
    if (std::abs(it->second - d) > 1e-4 * std::max(1., d)) return false;
  }
  // Every vertex except the root has exactly one parent edge in T; T has nv - 1 edges.
  if (int(info.primal_tree_edges.size()) != nv - 1) return false;
  // D is a spanning tree of the dual graph using only duals not in T, and it is maximal.
  const std::set<std::pair<int, int>> tree_set = as_set(info.primal_tree_edges);
  for (const auto& p : info.dual_tree_edges)
    if (tree_set.count(p)) return false;
  if (int(info.dual_tree_edges.size()) != nf - 1) return false;
  std::vector<std::pair<int, int>> independent_dual = max_dual_tree(source, tree_set);
  if (independent_dual.size() != info.dual_tree_edges.size()) return false;
  // Independently computed maximum spanning tree must reach the same optimum total length.
  if (std::abs(total_length(source, independent_dual) - total_length(source, info.dual_tree_edges)) > 1e-4) return false;
  // L is exactly E \ (T union D) and has 2g edges.
  std::set<std::pair<int, int>> all_edges;
  for (const IdEdge& e : edges) all_edges.insert({e.a, e.b});
  const std::set<std::pair<int, int>> dual_set = as_set(info.dual_tree_edges);
  std::set<std::pair<int, int>> rest;
  for (const auto& p : all_edges)
    if (!tree_set.count(p) && !dual_set.count(p)) rest.insert(p);
  if (rest != as_set(info.generator_edges)) return false;
  if (int(info.generator_edges.size()) != 2 * expected_genus) return false;
  // The cut is the 2-core of T union L.
  std::vector<std::pair<int, int>> union_tl(info.primal_tree_edges);
  union_tl.insert(union_tl.end(), info.generator_edges.begin(), info.generator_edges.end());
  if (two_core(union_tl) != as_set(info.cut_edges)) return false;
  // The cut graph has no leaves, and it is contained in T union L.
  std::map<int, int> degree;
  for (const auto& p : info.cut_edges) {
    ++degree[p.first];
    ++degree[p.second];
    if (!tree_set.count(p) && !as_set(info.generator_edges).count(p)) return false;
  }
  for (const auto& [v, d] : degree)
    if (d == 1) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Test cases.

int test_cut_graph_torus() {
  TorusGrid g = make_torus(7, 5, Point(0.f, 0.f, 0.f), 0.3f);
  GMesh result;
  const DiskAtlasInfo info = make_disk_atlas(g.mesh, g.ids[0], result);
  TASK_CHECK(mesh_genus(g.mesh) == 1.f);
  TASK_CHECK(verify_cut_graph(g.mesh, g.ids[0], info, 1));
  TASK_CHECK(info.generator_edges.size() == 2);
  TASK_CHECK(info.primal_tree_edges.size() == size_t(g.mesh.num_vertices() - 1));
  TASK_CHECK(info.dual_tree_edges.size() == size_t(g.mesh.num_faces() - 1));
  TASK_CHECK(info.cut_edges.size() >= 2);
  // A branch vertex exists in this cut graph.
  {
    std::map<int, int> degree;
    for (const auto& p : info.cut_edges) {
      ++degree[p.first];
      ++degree[p.second];
    }
    int branches = 0;
    for (const auto& [v, d] : degree) branches += d >= 3;
    TASK_CHECK(branches >= 1);
  }
  return 0;
}

int test_cut_graph_genus2() {
  GMesh source = make_genus2();
  TASK_CHECK(mesh_genus(source) == 2.f);
  TASK_CHECK(source.is_nice());
  int root = source.vertex_id(*source.vertices().begin());
  GMesh result;
  const DiskAtlasInfo info = make_disk_atlas(source, root, result);
  TASK_CHECK(verify_cut_graph(source, root, info, 2));
  TASK_CHECK(info.generator_edges.size() == 4);
  std::map<int, int> degree;
  for (const auto& p : info.cut_edges) {
    ++degree[p.first];
    ++degree[p.second];
  }
  int branches = 0;
  for (const auto& [v, d] : degree) branches += d >= 3;
  TASK_CHECK(branches >= 2);
  return 0;
}

int test_cut_graph_choices() {
  // Different roots give different (but equally valid) shortest-path trees.
  TorusGrid g = make_torus(6, 4, Point(0.f, 0.f, 0.f), 0.9f);
  GMesh r1, r2;
  const DiskAtlasInfo a = make_disk_atlas(g.mesh, g.ids[0], r1);
  const DiskAtlasInfo b = make_disk_atlas(g.mesh, g.ids[g.at(2, 1)], r2);
  TASK_CHECK(verify_cut_graph(g.mesh, g.ids[0], a, 1));
  TASK_CHECK(verify_cut_graph(g.mesh, g.ids[g.at(2, 1)], b, 1));
  TASK_CHECK(a.primal_tree_edges.size() == b.primal_tree_edges.size());
  TASK_CHECK(a.generator_edges.size() == b.generator_edges.size());
  return 0;
}

int test_disk_topology_torus() {
  TorusGrid g = make_torus(7, 5, Point(1.f, 2.f, 3.f), 0.6f);
  GMesh result;
  const DiskAtlasInfo info = make_disk_atlas(g.mesh, g.ids[1], result);
  TASK_CHECK(verify_disk(g.mesh, result, info));
  TASK_CHECK(result.is_nice());
  result.ok();
  return 0;
}

int test_disk_topology_genus2() {
  GMesh source = make_genus2();
  GMesh result;
  const DiskAtlasInfo info = make_disk_atlas(source, source.vertex_id(*source.vertices().begin()), result);
  TASK_CHECK(verify_disk(source, result, info));
  TASK_CHECK(result.is_nice());
  result.ok();
  // The cut graph branch vertices are split into multiple copies.
  std::map<int, int> cut_degree;
  for (const auto& p : info.cut_edges) {
    ++cut_degree[p.first];
    ++cut_degree[p.second];
  }
  int multi_copy = 0;
  for (const auto& [sid, d] : cut_degree) {
    if (d < 3) continue;
    int copies = 0;
    for (Vertex v : result.vertices())
      if (top_level_key_int(result.get_string(v), "source_vertex") == sid) ++copies;
    TASK_CHECK(copies == d);
    if (copies >= 3) ++multi_copy;
  }
  TASK_CHECK(multi_copy >= 2);
  return 0;
}

int test_disk_attributes() {
  GMesh source = make_attributed_torus(6, 4);
  GMesh result;
  const DiskAtlasInfo info = make_disk_atlas(source, source.vertex_id(*source.vertices().begin()), result);
  TASK_CHECK(verify_disk(source, result, info));
  // Retained attributes: face strings and flags, vertex strings (minus uv), edge strings/flags.
  for (Face f : result.faces()) {
    const int sid = top_level_key_int(result.get_string(f), "source_face");
    const Face sf = source.id_face(sid);
    std::string a, b;
    TASK_CHECK(GMesh::string_key(a, result.get_string(f), "marker") != nullptr);
    TASK_CHECK(GMesh::string_key(b, source.get_string(sf), "marker") != nullptr);
    TASK_CHECK(a == b);
  }
  // The source vertex-level uv must not survive anywhere.
  for (Vertex v : result.vertices()) {
    TASK_CHECK(!GMesh::string_has_key(result.get_string(v), "uv"));
    for (Face f : result.faces(v)) TASK_CHECK(result.get_string(result.corner(v, f)) != nullptr);
  }
  return 0;
}

int test_uv_boundary_and_interior() {
  TorusGrid g = make_torus(7, 5, Point(0.f, 0.f, 0.f), 0.2f);
  std::ostringstream source_before;
  g.mesh.write(source_before);
  GMesh result;
  const DiskAtlasInfo info = make_disk_atlas(g.mesh, g.ids[0], result);
  TASK_CHECK(verify_disk(g.mesh, result, info));
  // The source mesh is only read.
  std::ostringstream source_after;
  g.mesh.write(source_after);
  TASK_CHECK(source_before.str() == source_after.str());
  // All per-corner UVs exist and agree per vertex.
  for (Vertex v : result.vertices()) {
    Vec2<float> first;
    bool have = false;
    for (Face f : result.faces(v)) {
      const Corner c = result.corner(v, f);
      float uv[2] = {0.f, 0.f};
      TASK_CHECK(result.parse_corner_key_vec(c, "uv", uv));
      if (!have) {
        first = Vec2<float>(uv[0], uv[1]);
        have = true;
      } else {
        TASK_CHECK(first[0] == uv[0] && first[1] == uv[1]);
      }
    }
  }
  // The boundary lies on the unit circle.
  std::vector<Edge> bedges;
  const std::vector<Vertex> cycle = boundary_cycle(result, &bedges);
  for (Vertex v : cycle) {
    const Vec2<float> uv = corner_uv(result, v);
    TASK_CHECK(std::abs(std::hypot(double(uv[0]), double(uv[1])) - 1.) < 1e-5);
  }
  // The point of the unit circle at angle zero is a boundary copy with the smallest source_vertex id.
  {
    int min_source = -1;
    std::vector<Vertex> min_copies;
    for (Vertex v : cycle) {
      const int sid = top_level_key_int(result.get_string(v), "source_vertex");
      if (min_source < 0 || sid < min_source) {
        min_source = sid;
        min_copies.clear();
      }
      if (sid == min_source) min_copies.push_back(v);
    }
    bool found_start = false;
    for (Vertex v : min_copies) {
      const Vec2<float> uv = corner_uv(result, v);
      if (std::abs(double(uv[0]) - 1.) < 1e-5 && std::abs(double(uv[1])) < 1e-5) found_start = true;
    }
    TASK_CHECK(found_start);
  }
  // Boundary angles follow cumulative 3D boundary length (in one direction or the other).
  double perimeter = 0.;
  std::vector<double> lens;
  for (size_t i = 0; i < cycle.size(); ++i) {
    const Vertex a = cycle[i], b = cycle[(i + 1) % cycle.size()];
    const double l = double(dist(result.point(a), result.point(b)));
    lens.push_back(l);
    perimeter += l;
  }
  TASK_CHECK(perimeter > 0.);
  std::vector<double> angles;
  for (Vertex v : cycle) {
    const Vec2<float> uv = corner_uv(result, v);
    angles.push_back(std::atan2(double(uv[1]), double(uv[0])));
  }
  double arc_error = 0.;
  for (size_t i = 0; i < cycle.size(); ++i) {
    const double delta = angles[(i + 1) % cycle.size()] - angles[i];
    const double expected = TAU * lens[i] / perimeter;
    double best = 1e9;
    for (int k = -1; k <= 1; ++k) best = std::min(best, std::abs(delta + TAU * k - expected));
    arc_error = std::max(arc_error, best);
  }
  TASK_CHECK(arc_error < 2e-5);
  // Interior UVs are the mean of the distinct neighbors.
  int interior = 0;
  for (Vertex v : result.vertices()) {
    bool bnd = false;
    for (Edge e : result.edges(v))
      if (result.is_boundary(e)) bnd = true;
    if (!bnd) ++interior;
  }
  TASK_CHECK(interior > 0);
  const double measured = measured_average_residual(result);
  TASK_CHECK(measured <= 1e-7);
  TASK_CHECK(std::abs(measured - info.residual) < 2e-8);
  // Every face has positive signed UV area.
  for (Face f : result.faces()) {
    Vec2<float> p[3];
    int i = 0;
    for (Corner c : result.corners(f)) {
      float uv[2] = {0.f, 0.f};
      TASK_CHECK(result.parse_corner_key_vec(c, "uv", uv));
      p[i++] = Vec2<float>(uv[0], uv[1]);
    }
    const double area = .5 * ((double(p[1][0]) - p[0][0]) * (p[2][1] - p[0][1]) -
                              (double(p[2][0]) - p[0][0]) * (p[1][1] - p[0][1]));
    TASK_CHECK(area > 0.);
  }
  return 0;
}

int test_uv_residual_consistency() {
  // The reported residual must be the actual largest averaging-equation residual of the stored layout, and must be 0
  // exactly when the disk has no interior vertex.
  for (const auto& dims : {std::pair<int, int>{3, 3}, std::pair<int, int>{4, 3}, std::pair<int, int>{6, 4}}) {
    TorusGrid g = make_torus(dims.first, dims.second, Point(0.f, 0.f, 0.f), 0.f);
    GMesh result;
    const DiskAtlasInfo info = make_disk_atlas(g.mesh, g.ids[0], result);
    TASK_CHECK(verify_disk(g.mesh, result, info));
    int interior = 0;
    for (Vertex v : result.vertices()) {
      bool bnd = false;
      for (Edge e : result.edges(v))
        if (result.is_boundary(e)) bnd = true;
      if (!bnd) ++interior;
    }
    const double measured = measured_average_residual(result);
    TASK_CHECK(measured <= 1e-7);
    TASK_CHECK(std::abs(measured - info.residual) < 3e-8);
    if (interior == 0) TASK_CHECK(info.residual == 0. && measured == 0.);
  }
  return 0;
}

int test_uv_persistence_roundtrip() {
  GMesh source = make_attributed_torus(6, 5);
  GMesh result;
  const DiskAtlasInfo info = make_disk_atlas(source, source.vertex_id(*source.vertices().begin()), result);
  TASK_CHECK(verify_disk(source, result, info));
  const double before_residual = measured_average_residual(result);
  TASK_CHECK(before_residual <= 1e-7);
  // Save corner normals and UVs by face id / corner index before the round trip.
  std::ostringstream oss;
  result.write(oss);
  TASK_CHECK(oss.str().find("source_vertex=") != std::string::npos);
  TASK_CHECK(oss.str().find("source_face=") != std::string::npos);
  TASK_CHECK(oss.str().find("source_edge=(") != std::string::npos);
  TASK_CHECK(oss.str().find("uv=(") != std::string::npos);
  std::istringstream iss(oss.str());
  GMesh copy;
  copy.read(iss);
  copy.ok();
  TASK_CHECK(copy.num_faces() == result.num_faces());
  TASK_CHECK(copy.num_vertices() == result.num_vertices());
  TASK_CHECK(copy.num_edges() == result.num_edges());
  for (Face f : result.ordered_faces()) {
    const Face cf = copy.id_face(copy.face_id(copy.id_face(result.face_id(f))));
    (void)cf;
  }
  // Match faces by (unique) source_face id, then match the corner sequence.
  std::map<int, Face> copied_by_source;
  for (Face f : copy.faces()) copied_by_source[top_level_key_int(copy.get_string(f), "source_face")] = f;
  for (Face f : result.faces()) {
    const int sid = top_level_key_int(result.get_string(f), "source_face");
    TASK_CHECK(copied_by_source.count(sid) == 1);
    const Face cf = copied_by_source.at(sid);
    // Same source vertex ids in the same order.
    std::vector<int> a, b;
    for (Vertex v : result.vertices(f)) a.push_back(top_level_key_int(result.get_string(v), "source_vertex"));
    for (Vertex v : copy.vertices(cf)) b.push_back(top_level_key_int(copy.get_string(v), "source_vertex"));
    TASK_CHECK(a == b);
    TASK_CHECK(result.get_string(f) && copy.get_string(cf));
    // UVs and explicit normals survive per corner.
    std::vector<Corner> ca, cb;
    for (Corner c : result.corners(f)) ca.push_back(c);
    for (Corner c : copy.corners(cf)) cb.push_back(c);
    TASK_CHECK(ca.size() == cb.size());
    for (size_t i = 0; i < ca.size(); ++i) {
      float ua[2], ub[2];
      TASK_CHECK(result.parse_corner_key_vec(ca[i], "uv", ua));
      TASK_CHECK(copy.parse_corner_key_vec(cb[i], "uv", ub));
      TASK_CHECK(ua[0] == ub[0] && ua[1] == ub[1]);
      float na[3], nb[3];
      const bool ha = result.parse_corner_key_vec(ca[i], "normal", na);
      const bool hb = copy.parse_corner_key_vec(cb[i], "normal", nb);
      TASK_CHECK(ha == hb);
      if (ha)
        for (int k = 0; k < 3; ++k) TASK_CHECK(na[k] == nb[k]);
    }
  }
  // The UV layout still satisfies the mean equations after parsing.
  TASK_CHECK(measured_average_residual(copy) <= 1e-7);
  // Editing still works on the reloaded mesh.
  Edge boundary = nullptr;
  for (Edge e : copy.edges())
    if (copy.is_boundary(e)) {
      boundary = e;
      break;
    }
  TASK_CHECK(boundary != nullptr);
  const int nv = copy.num_vertices(), ne = copy.num_edges(), nf = copy.num_faces();
  copy.split_edge(boundary);
  copy.ok();
  // A boundary edge has a single incident face, so splitting it adds one vertex, two edges, and one face.
  TASK_CHECK(copy.num_vertices() == nv + 1);
  TASK_CHECK(copy.num_edges() == ne + 2);
  TASK_CHECK(copy.num_faces() == nf + 1);
  TASK_CHECK(copy.num_vertices() - copy.num_edges() + copy.num_faces() == 1);
  return 0;
}

int test_rejection_then_success() {
  TorusGrid g = make_torus(5, 4, Point(0.f, 0.f, 0.f), 0.f);
  GMesh result;
  const DiskAtlasInfo good = make_disk_atlas(g.mesh, g.ids[0], result);
  TASK_CHECK(good.generator_edges.size() == 2);
  std::ostringstream before;
  result.write(before);
  // Invalid root.  The rejection must leave the caller's result untouched.
  bool threw = false;
  try {
    GMesh scratch;
    (void)make_disk_atlas(g.mesh, 999999, scratch);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  // Not closed (a grid patch) and not genus >= 1.
  GMesh patch;
  {
    std::vector<Vertex> vs;
    for (int y = 0; y <= 3; ++y)
      for (int x = 0; x <= 3; ++x) {
        Vertex v = patch.create_vertex();
        patch.set_point(v, Point(float(x), float(y), 0.2f * float(x * y)));
        vs.push_back(v);
      }
    auto idx = [](int y, int x) { return y * 4 + x; };
    for (int y = 0; y < 3; ++y)
      for (int x = 0; x < 3; ++x) {
        patch.create_face(vs[idx(y, x)], vs[idx(y, x + 1)], vs[idx(y + 1, x + 1)]);
        patch.create_face(vs[idx(y, x)], vs[idx(y + 1, x + 1)], vs[idx(y + 1, x)]);
      }
  }
  threw = false;
  try {
    GMesh scratch;
    (void)make_disk_atlas(patch, patch.vertex_id(*patch.vertices().begin()), scratch);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  // Genus 0 closed surface (tetrahedron) is rejected.
  GMesh tet;
  {
    std::vector<Vertex> vs;
    for (int i = 0; i < 4; ++i) {
      Vertex v = tet.create_vertex();
      tet.set_point(v, Point(i & 1 ? 1.f : -1.f, i & 2 ? 1.f : -1.f, i & 4 ? 1.f : -1.f));
      vs.push_back(v);
    }
    tet.create_face(vs[0], vs[1], vs[2]);
    tet.create_face(vs[0], vs[2], vs[3]);
    tet.create_face(vs[0], vs[3], vs[1]);
    tet.create_face(vs[1], vs[3], vs[2]);
  }
  TASK_CHECK(mesh_genus(tet) == 0.f);
  threw = false;
  try {
    GMesh scratch;
    (void)make_disk_atlas(tet, tet.vertex_id(*tet.vertices().begin()), scratch);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  // On rejection the caller's result is left unchanged: still the previous atlas.
  {
    std::ostringstream snapshot;
    result.write(snapshot);
    bool threw_here = false;
    try {
      (void)make_disk_atlas(g.mesh, 999999, result);  // Fails after validating the root.
    } catch (const std::invalid_argument&) {
      threw_here = true;
    }
    TASK_CHECK(threw_here);
    std::ostringstream after_reject;
    result.write(after_reject);
    TASK_CHECK(snapshot.str() == after_reject.str());
  }
  std::ostringstream after;
  result.write(after);
  TASK_CHECK(before.str() == after.str());
  // A later successful call still works.
  GMesh other;
  const DiskAtlasInfo again = make_disk_atlas(g.mesh, g.ids[1], other);
  TASK_CHECK(verify_cut_graph(g.mesh, g.ids[1], again, 1));
  TASK_CHECK(verify_disk(g.mesh, other, again));
  return 0;
}

int test_persistence_genus2() {
  GMesh source = make_genus2();
  GMesh result;
  const DiskAtlasInfo info = make_disk_atlas(source, source.vertex_id(*source.vertices().begin()), result);
  TASK_CHECK(verify_disk(source, result, info));
  TASK_CHECK(measured_average_residual(result) <= 1e-7);
  std::ostringstream oss;
  result.write(oss);
  std::istringstream iss(oss.str());
  GMesh copy;
  copy.read(iss);
  copy.ok();
  TASK_CHECK(copy.num_vertices() == result.num_vertices());
  TASK_CHECK(copy.num_faces() == result.num_faces());
  TASK_CHECK(measured_average_residual(copy) <= 1e-7);
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Registration for task_support/driver.c.

const struct task_case task_cases[] = {
    {"cut_graph", "torus_nonuniform", test_cut_graph_torus},
    {"cut_graph", "genus2", test_cut_graph_genus2},
    {"cut_graph", "root_choices", test_cut_graph_choices},
    {"disk_topology", "torus", test_disk_topology_torus},
    {"disk_topology", "genus2", test_disk_topology_genus2},
    {"disk_topology", "attributes", test_disk_attributes},
    {"uv_persistence", "uv_equations", test_uv_boundary_and_interior},
    {"uv_persistence", "residual_consistency", test_uv_residual_consistency},
    {"uv_persistence", "roundtrip_and_split", test_uv_persistence_roundtrip},
    {"uv_persistence", "reject_then_succeed", test_rejection_then_success},
    {"uv_persistence", "genus2_roundtrip", test_persistence_genus2},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
