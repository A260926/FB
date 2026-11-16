// -*- C++ -*-  Copyright (c) Microsoft Corporation; see LICENSE.
#include "libHh/DiskAtlas.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "libHh/Hh.h"

namespace hh {
namespace {

constexpr double k_tau = 6.283185307179586476925286766559;

using EdgePair = std::pair<int, int>;

[[noreturn]] void fail_solve(const char* s) { throw std::runtime_error(s); }

// Remove one attribute key (and its possible value) from a {key=value}+ mesh info string.
std::string strip_key(const char* ss, const char* key) {
  std::string result;
  if (!ss) return result;
  const std::size_t keylen = std::strlen(key);
  StringKeyIter iter(ss);
  const char* kb;
  const char* vb;
  int kl, vl;
  while (iter.next(kb, kl, vb, vl)) {
    if (std::size_t(kl) == keylen && !std::strncmp(kb, key, keylen)) continue;
    if (!result.empty()) result.push_back(' ');
    result.append(kb, std::size_t(kl));
    if (vl > 0) {
      result.push_back('=');
      result.append(vb, std::size_t(vl));
    }
  }
  return result;
}

std::string append_key(std::string s, const char* key, const std::string& value) {
  return GMesh::string_update(s, key, value.c_str());
}

void normalize_pairs(std::vector<EdgePair>& pairs) {
  for (EdgePair& p : pairs)
    if (p.first > p.second) std::swap(p.first, p.second);
  std::sort(pairs.begin(), pairs.end());
  pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
}

struct UnionFind {
  std::vector<int> parent;
  std::vector<unsigned char> rank_;

  explicit UnionFind(int n) : parent(n), rank_(n, 0) {
    for (int i = 0; i < n; ++i) parent[i] = i;
  }
  int find(int x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  }
  bool unite(int a, int b) {
    a = find(a);
    b = find(b);
    if (a == b) return false;
    if (rank_[a] < rank_[b]) std::swap(a, b);
    parent[b] = a;
    if (rank_[a] == rank_[b]) ++rank_[a];
    return true;
  }
};

int64_t pair_key(int a, int b) {
  if (a > b) std::swap(a, b);
  return (int64_t(uint64_t(uint32_t(a)) << 32) | uint64_t(uint32_t(b)));
}

// Validate the input and gather the vertex/edge/face indexing used below.
struct SourceIndex {
  const GMesh& mesh;
  std::vector<Vertex> vertices;
  std::vector<Face> faces;
  std::vector<Edge> edges;
  std::vector<int> vertex_id;
  std::vector<int> face_id;
  std::vector<int> edge_v1, edge_v2;  // vertex indices, edge_v1 < edge_v2
  std::vector<int> edge_f1, edge_f2;  // face indices
  std::vector<double> edge_length;
  std::unordered_map<Vertex, int> vertex_index;
  std::unordered_map<Face, int> face_index;
  std::unordered_map<Edge, int> edge_index;
  int root{0};
  int genus{0};

  explicit SourceIndex(const GMesh& m) : mesh(m) {
    for (Vertex v : m.ordered_vertices()) {
      vertex_index.emplace(v, int(vertices.size()));
      vertices.push_back(v);
    }
    for (Face f : m.ordered_faces()) {
      face_index.emplace(f, int(faces.size()));
      faces.push_back(f);
    }
    for (Edge e : m.edges()) {
      edge_index.emplace(e, int(edges.size()));
      edges.push_back(e);
    }
    const int nv = int(vertices.size());
    if (nv == 0 || faces.empty()) throw std::invalid_argument("make_disk_atlas: empty source mesh");
    vertex_id.resize(nv);
    for (int i = 0; i < nv; ++i) vertex_id[i] = m.vertex_id(vertices[i]);
    face_id.resize(faces.size());
    for (int i = 0; i < int(faces.size()); ++i) face_id[i] = m.face_id(faces[i]);
    const int ne = int(edges.size());
    edge_v1.assign(ne, -1);
    edge_v2.assign(ne, -1);
    edge_f1.assign(ne, -1);
    edge_f2.assign(ne, -1);
    edge_length.assign(ne, 0.);
    for (int i = 0; i < ne; ++i) {
      Edge e = edges[i];
      int a = vertex_index.at(m.vertex1(e));
      int b = vertex_index.at(m.vertex2(e));
      if (a > b) std::swap(a, b);
      edge_v1[i] = a;
      edge_v2[i] = b;
      if (!m.face1(e)) throw std::invalid_argument("make_disk_atlas: source has a boundary edge");
      edge_f1[i] = face_index.at(m.face1(e));
      if (!m.face2(e)) throw std::invalid_argument("make_disk_atlas: source has a boundary edge");
      edge_f2[i] = face_index.at(m.face2(e));
      if (edge_f1[i] == edge_f2[i]) throw std::invalid_argument("make_disk_atlas: source is not a 2-manifold");
      const float len = m.length(e);
      if (!(len > 0.f) || !std::isfinite(len)) throw std::invalid_argument("make_disk_atlas: bad edge length");
      edge_length[i] = double(len);
    }
    // Vertices and points.
    for (Vertex v : vertices) {
      if (m.degree(v) <= 0) throw std::invalid_argument("make_disk_atlas: source has an isolated vertex");
      for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(m.point(v)[c])) throw std::invalid_argument("make_disk_atlas: non-finite vertex position");
      }
    }
    for (Face f : faces) {
      if (!m.is_triangle(f)) throw std::invalid_argument("make_disk_atlas: source has a non-triangle face");
      const float area = m.area(f);
      if (!(area > 0.f) || !std::isfinite(area)) throw std::invalid_argument("make_disk_atlas: bad triangle area");
    }
    // Manifold vertex links.
    if (!m.is_nice()) throw std::invalid_argument("make_disk_atlas: source is not a 2-manifold");
    // One face-connected component.
    {
      std::vector<char> visited(faces.size(), 0);
      std::vector<Face> stack;
      stack.push_back(faces[0]);
      visited[0] = 1;
      int reached = 0;
      while (!stack.empty()) {
        Face f = stack.back();
        stack.pop_back();
        ++reached;
        for (Face g : m.faces(f)) {
          int gi = face_index.at(g);
          if (!visited[gi]) {
            visited[gi] = 1;
            stack.push_back(g);
          }
        }
      }
      if (reached != int(faces.size())) throw std::invalid_argument("make_disk_atlas: source is not connected");
    }
    // Euler characteristic of a closed connected orientable surface: nv - ne + nf == 2 - 2 * genus.
    const int chi = nv - ne + int(faces.size());
    if (chi > 2 || ((2 - chi) & 1)) throw std::invalid_argument("make_disk_atlas: source has invalid topology");
    genus = (2 - chi) / 2;
    if (genus < 1) throw std::invalid_argument("make_disk_atlas: source must have genus at least 1");
  }

  int vertex_of_id(int id) const {
    if (id <= 0) throw std::invalid_argument("make_disk_atlas: root_vertex_id is not valid");
    Vertex v = mesh.id_retrieve_vertex(id);
    if (!v) throw std::invalid_argument("make_disk_atlas: root_vertex_id is not a vertex of the source");
    auto it = vertex_index.find(v);
    if (it == vertex_index.end()) throw std::invalid_argument("make_disk_atlas: invalid root_vertex_id");
    return it->second;
  }
};

int64_t edge_lookup_key(int a, int b) { return pair_key(a, b); }

// The maximum-total-length spanning tree of the face-dual graph, using only the dual edges whose primal edge is not
// in T.  Returns primal edge indices.
std::vector<int> build_dual_tree(const SourceIndex& si, const std::vector<char>& in_tree) {
  struct DualEdge {
    double length;
    int edge;
    int a, b;
  };
  std::vector<DualEdge> dual;
  const int ne = int(si.edges.size());
  for (int i = 0; i < ne; ++i) {
    if (in_tree[i]) continue;
    dual.push_back(DualEdge{si.edge_length[i], i, si.edge_f1[i], si.edge_f2[i]});
  }
  std::sort(dual.begin(), dual.end(), [&](const DualEdge& x, const DualEdge& y) {
    if (x.length != y.length) return x.length > y.length;
    int ax = si.vertex_id[si.edge_v1[x.edge]], bx = si.vertex_id[si.edge_v2[x.edge]];
    int ay = si.vertex_id[si.edge_v1[y.edge]], by = si.vertex_id[si.edge_v2[y.edge]];
    if (ax != ay) return ax < ay;
    return bx < by;
  });
  const int nf = int(si.faces.size());
  UnionFind uf(nf);
  std::vector<int> tree;
  for (const DualEdge& de : dual) {
    if (!uf.unite(de.a, de.b)) continue;
    tree.push_back(de.edge);
    if (int(tree.size()) == nf - 1) break;
  }
  if (int(tree.size()) != nf - 1) fail_solve("make_disk_atlas: the dual spanning tree could not be built");
  return tree;
}

// Repeatedly delete degree-one edges from T union L; the remaining edges form the cut graph.
std::vector<int> prune_cut_graph(const SourceIndex& si, const std::vector<char>& in_set) {
  const int nv = int(si.vertices.size());
  const int ne = int(si.edges.size());
  std::vector<std::vector<int>> incident(nv);
  std::vector<int> degree(nv, 0);
  for (int i = 0; i < ne; ++i) {
    if (!in_set[i]) continue;
    incident[si.edge_v1[i]].push_back(i);
    incident[si.edge_v2[i]].push_back(i);
    ++degree[si.edge_v1[i]];
    ++degree[si.edge_v2[i]];
  }
  std::vector<char> removed(ne, 0);
  std::queue<int> leaves;
  for (int v = 0; v < nv; ++v)
    if (degree[v] == 1) leaves.push(v);
  std::vector<int> cursor(nv, 0);
  auto remove_edge = [&](int e) {
    if (removed[e]) return;
    removed[e] = 1;
    --degree[si.edge_v1[e]];
    --degree[si.edge_v2[e]];
    if (degree[si.edge_v1[e]] == 1) leaves.push(si.edge_v1[e]);
    if (degree[si.edge_v2[e]] == 1) leaves.push(si.edge_v2[e]);
  };
  while (!leaves.empty()) {
    const int v = leaves.front();
    leaves.pop();
    if (degree[v] != 1) continue;
    int& c = cursor[v];
    while (c < int(incident[v].size()) && removed[incident[v][c]]) ++c;
    if (c >= int(incident[v].size())) fail_solve("make_disk_atlas: inconsistent cut-graph pruning");
    remove_edge(incident[v][c]);
  }
  std::vector<int> cut;
  for (int i = 0; i < ne; ++i)
    if (in_set[i] && !removed[i]) cut.push_back(i);
  if (cut.empty()) fail_solve("make_disk_atlas: empty cut graph");
  return cut;
}

// Sparse Jacobi-preconditioned conjugate gradients for the uniform-weight Laplacian with Dirichlet boundary values.
// The equations for the interior vertices are: deg(u) * x_u - sum_{interior v ~ u} x_v = sum_{boundary w ~ u} x_w.
std::vector<double> solve_laplacian(const std::vector<std::vector<int>>& neighbors, const std::vector<int>& interior,
                                   const std::vector<int>& interior_index, const std::vector<char>& is_boundary,
                                   const std::vector<double>& boundary_value) {
  const int n = int(interior.size());
  std::vector<double> diagonal(n, 0.), rhs(n, 0.);
  for (int i = 0; i < n; ++i) {
    const int u = interior[i];
    double sum = 0.;
    for (int w : neighbors[u]) {
      if (is_boundary[w]) sum += boundary_value[w];
    }
    diagonal[i] = double(neighbors[u].size());
    rhs[i] = sum;
  }
  std::vector<double> x(n, 0.), r = rhs, z(n, 0.), p(n, 0.), ap(n, 0.);
  auto apply = [&](const std::vector<double>& v, std::vector<double>& out) {
    for (int i = 0; i < n; ++i) {
      double val = diagonal[i] * v[i];
      for (int w : neighbors[interior[i]]) {
        if (is_boundary[w]) continue;
        const int j = interior_index[w];
        if (j >= 0) val -= v[j];
      }
      out[i] = val;
    }
  };
  auto dot = [&](const std::vector<double>& a, const std::vector<double>& b) {
    double s = 0.;
    for (int i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
  };
  auto max_abs = [&](const std::vector<double>& a) {
    double m = 0.;
    for (double v : a) m = std::max(m, std::abs(v));
    return m;
  };
  double norm_b = max_abs(rhs);
  if (norm_b == 0.) norm_b = 1.;
  const double tolerance = 1e-13 * norm_b;
  for (int i = 0; i < n; ++i) z[i] = r[i] / diagonal[i];
  p = z;
  double rz = dot(r, z);
  const int max_iterations = std::max(1000, 8 * n + 1000);
  int iteration = 0;
  while (max_abs(r) > tolerance && iteration < max_iterations) {
    apply(p, ap);
    const double pap = dot(p, ap);
    if (!(pap > 0.)) fail_solve("make_disk_atlas: texture solve is singular");
    const double alpha = rz / pap;
    for (int i = 0; i < n; ++i) {
      x[i] += alpha * p[i];
      r[i] -= alpha * ap[i];
    }
    for (int i = 0; i < n; ++i) z[i] = r[i] / diagonal[i];
    const double rz_new = dot(r, z);
    const double beta = rz_new / rz;
    for (int i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
    rz = rz_new;
    ++iteration;
  }
  if (!(max_abs(r) <= 1e-9 * norm_b)) fail_solve("make_disk_atlas: texture solve did not converge");
  return x;
}

}  // namespace

DiskAtlasInfo make_disk_atlas(const GMesh& source, int root_vertex_id, GMesh& result) {
  if (std::addressof(source) == std::addressof(result))
    throw std::invalid_argument("make_disk_atlas: source and result must not alias");
  const SourceIndex si(source);
  const int nv = int(si.vertices.size());
  const int ne = int(si.edges.size());
  const int nf = int(si.faces.size());
  const int root = si.vertex_of_id(root_vertex_id);

  // Primal shortest-path tree T from the root, using 3D edge lengths.
  std::vector<std::vector<std::pair<int, int>>> adjacency(nv);  // (neighbor, edge)
  for (int i = 0; i < ne; ++i) {
    adjacency[si.edge_v1[i]].push_back({si.edge_v2[i], i});
    adjacency[si.edge_v2[i]].push_back({si.edge_v1[i], i});
  }
  const double infinity = std::numeric_limits<double>::infinity();
  std::vector<double> distance(nv, infinity);
  std::vector<int> parent_edge(nv, -1);
  using Entry = std::pair<double, int>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
  distance[root] = 0.;
  queue.push({0., root});
  while (!queue.empty()) {
    const auto [d, v] = queue.top();
    queue.pop();
    if (d > distance[v]) continue;
    for (const auto& [w, e] : adjacency[v]) {
      const double nd = d + si.edge_length[e];
      if (nd < distance[w]) {
        distance[w] = nd;
        parent_edge[w] = e;
        queue.push({nd, w});
      }
    }
  }
  std::vector<char> in_tree(ne, 0);
  for (int v = 0; v < nv; ++v) {
    if (v == root) continue;
    if (parent_edge[v] < 0) throw std::invalid_argument("make_disk_atlas: source is not connected");
    in_tree[parent_edge[v]] = 1;
  }
  std::vector<EdgePair> primal_tree_edges;
  for (int i = 0; i < ne; ++i)
    if (in_tree[i]) primal_tree_edges.push_back({si.vertex_id[si.edge_v1[i]], si.vertex_id[si.edge_v2[i]]});

  // Dual maximum spanning tree D over the dual edges that are not in T.
  const std::vector<int> dual_tree = build_dual_tree(si, in_tree);
  std::vector<char> in_dual(ne, 0);
  for (int e : dual_tree) in_dual[e] = 1;

  // Generator edges L = E \ (T union D).
  std::vector<char> in_generator(ne, 0);
  std::vector<int> generator;
  for (int i = 0; i < ne; ++i)
    if (!in_tree[i] && !in_dual[i]) {
      in_generator[i] = 1;
      generator.push_back(i);
    }
  if (int(generator.size()) != 2 * si.genus) fail_solve("make_disk_atlas: unexpected number of generator edges");

  // Cut graph: 2-core of T union L.
  std::vector<char> in_prunable(ne, 0);
  for (int i = 0; i < ne; ++i) in_prunable[i] = char(in_tree[i] || in_generator[i]);
  const std::vector<int> cut = prune_cut_graph(si, in_prunable);
  std::vector<char> in_cut(ne, 0);
  for (int e : cut) in_cut[e] = 1;

  std::vector<EdgePair> dual_tree_edges;
  for (int e : dual_tree) dual_tree_edges.push_back({si.vertex_id[si.edge_v1[e]], si.vertex_id[si.edge_v2[e]]});
  std::vector<EdgePair> generator_edges;
  for (int e : generator) generator_edges.push_back({si.vertex_id[si.edge_v1[e]], si.vertex_id[si.edge_v2[e]]});
  std::vector<EdgePair> cut_edges;
  for (int e : cut) cut_edges.push_back({si.vertex_id[si.edge_v1[e]], si.vertex_id[si.edge_v2[e]]});

  // Split each vertex into one copy per face sector between consecutive incident cut edges.
  GMesh out;
  std::unordered_map<Corner, int> corner_out_vertex;
  std::vector<Vertex> out_vertex;
  std::vector<int> out_source_vertex;
  std::unordered_map<Vertex, int> out_vertex_index;
  std::vector<std::vector<int>> copies_of_vertex(nv);
  for (int vi = 0; vi < nv; ++vi) {
    std::vector<Corner> fan;
    for (Corner c : source.ccw_corners(si.vertices[vi])) fan.push_back(c);
    const int k = int(fan.size());
    if (k == 0) throw std::invalid_argument("make_disk_atlas: source has an isolated vertex");
    std::vector<char> cut_after(k, 0);
    int num_cuts = 0;
    for (int j = 0; j < k; ++j) {
      Edge e = source.clw_face_edge(fan[j]);
      auto it = si.edge_index.find(e);
      if (it == si.edge_index.end()) fail_solve("make_disk_atlas: inconsistent corner-edge association");
      if (in_cut[it->second]) {
        cut_after[j] = 1;
        ++num_cuts;
      }
    }
    const int num_copies = std::max(1, num_cuts);
    std::vector<int> sector_of_corner(k, 0);
    if (num_cuts) {
      int first = 0;
      while (!cut_after[first]) ++first;
      const int start = (first + 1) % k;
      int sector = 0;
      for (int t = 0; t < k; ++t) {
        const int j = (start + t) % k;
        sector_of_corner[j] = sector;
        if (cut_after[j]) ++sector;
      }
    }
    for (int s = 0; s < num_copies; ++s) {
      Vertex ov = out.create_vertex();
      out.set_point(ov, source.point(si.vertices[vi]));
      out.flags(ov) = source.flags(si.vertices[vi]);
      std::string str = strip_key(source.get_string(si.vertices[vi]), "uv");
      str = strip_key(str.c_str(), "source_vertex");
      str = append_key(std::move(str), "source_vertex", std::to_string(si.vertex_id[vi]));
      out.set_string(ov, str.c_str());
      const int index = int(out_vertex.size());
      out_vertex.push_back(ov);
      out_source_vertex.push_back(vi);
      out_vertex_index.emplace(ov, index);
      copies_of_vertex[vi].push_back(index);
    }
    for (int j = 0; j < k; ++j) corner_out_vertex.emplace(fan[j], copies_of_vertex[vi][sector_of_corner[j]]);
  }

  // Create one output face per source face, preserving its rotation and corner strings.
  for (int fi = 0; fi < nf; ++fi) {
    const Face f = si.faces[fi];
    std::vector<Vertex> va;
    std::vector<Corner> source_corners;
    for (Corner c : source.corners(f)) {
      source_corners.push_back(c);
      auto it = corner_out_vertex.find(c);
      if (it == corner_out_vertex.end()) fail_solve("make_disk_atlas: missing output vertex for corner");
      va.push_back(out_vertex[it->second]);
    }
    const CArrayView<Vertex> face_vertices(va.data(), int(va.size()));
    if (!out.legal_create_face(face_vertices)) fail_solve("make_disk_atlas: the cut mesh would not be a legal surface");
    Face of = out.create_face(face_vertices);
    out.flags(of) = source.flags(f);
    std::string str = strip_key(source.get_string(f), "uv");
    str = strip_key(str.c_str(), "source_face");
    str = append_key(std::move(str), "source_face", std::to_string(si.face_id[fi]));
    out.set_string(of, str.c_str());
    std::size_t ci = 0;
    for (Corner oc : out.corners(of)) {
      std::string cstr = strip_key(source.get_string(source_corners[ci]), "uv");
      cstr = strip_key(cstr.c_str(), "source_vertex");
      if (!cstr.empty()) out.set_string(oc, cstr.c_str());
      ++ci;
    }
  }

  // Edge provenance; a cut edge gives two output boundary edges, both copying the original edge attributes.
  std::unordered_map<int64_t, int> pair_to_edge;
  for (int i = 0; i < ne; ++i) pair_to_edge.emplace(edge_lookup_key(si.edge_v1[i], si.edge_v2[i]), i);
  for (Edge oe : out.edges()) {
    auto itu = out_vertex_index.find(out.vertex1(oe));
    auto itv = out_vertex_index.find(out.vertex2(oe));
    if (itu == out_vertex_index.end() || itv == out_vertex_index.end())
      fail_solve("make_disk_atlas: unknown output edge endpoint");
    const int a = out_source_vertex[itu->second];
    const int b = out_source_vertex[itv->second];
    auto ite = pair_to_edge.find(edge_lookup_key(a, b));
    if (ite == pair_to_edge.end()) fail_solve("make_disk_atlas: output edge has no source edge");
    const Edge se = si.edges[ite->second];
    out.flags(oe) = source.flags(se);
    std::string str = strip_key(source.get_string(se), "uv");
    str = strip_key(str.c_str(), "source_edge");
    int ida = si.vertex_id[a], idb = si.vertex_id[b];
    if (ida > idb) std::swap(ida, idb);
    str = append_key(std::move(str), "source_edge", sform("(%d %d)", ida, idb));
    out.set_string(oe, str.c_str());
  }

  // Structural checks: one face component, one simple boundary loop, and the topology of a disk.
  if (out.num_faces() != nf) fail_solve("make_disk_atlas: unexpected number of output faces");
  if (out.num_edges() != ne + int(cut.size())) fail_solve("make_disk_atlas: unexpected number of output edges");
  if (!out.is_nice()) fail_solve("make_disk_atlas: output mesh is not a 2-manifold");
  {
    std::vector<char> visited(out.num_faces(), 0);
    visited[out.face_id(out.id_face(1)) - 1] = 1;
    std::vector<Face> stack{out.id_face(1)};
    int reached = 0;
    while (!stack.empty()) {
      Face f = stack.back();
      stack.pop_back();
      ++reached;
      for (Face g : out.faces(f)) {
        const int gi = out.face_id(g) - 1;
        if (!visited[gi]) {
          visited[gi] = 1;
          stack.push_back(g);
        }
      }
    }
    if (reached != out.num_faces()) fail_solve("make_disk_atlas: output has multiple face components");
  }
  const int nout = int(out_vertex.size());
  std::vector<int> out_index(out.num_vertices(), -1);
  for (int i = 0; i < nout; ++i) out_index[out.vertex_id(out_vertex[i]) - 1] = i;
  std::vector<std::vector<Edge>> boundary_incident(nout);
  std::vector<char> is_boundary(nout, 0);
  int num_boundary_edges = 0;
  for (Edge e : out.edges()) {
    if (!out.is_boundary(e)) continue;
    ++num_boundary_edges;
    const int a = out_index[out.vertex_id(out.vertex1(e)) - 1];
    const int b = out_index[out.vertex_id(out.vertex2(e)) - 1];
    boundary_incident[a].push_back(e);
    boundary_incident[b].push_back(e);
    is_boundary[a] = 1;
    is_boundary[b] = 1;
  }
  if (num_boundary_edges != 2 * int(cut.size())) fail_solve("make_disk_atlas: unexpected boundary length");
  {
    int num_boundary_vertices = 0;
    for (int i = 0; i < nout; ++i) {
      if (!is_boundary[i]) continue;
      ++num_boundary_vertices;
      if (boundary_incident[i].size() != 2) fail_solve("make_disk_atlas: boundary is not a simple loop");
    }
    if (num_boundary_edges != num_boundary_vertices) fail_solve("make_disk_atlas: boundary is not a simple loop");
  }

  // Order the unique boundary loop, starting at the copy with the smallest source vertex id.
  int start_vertex = -1;
  for (int i = 0; i < nout; ++i) {
    if (!is_boundary[i]) continue;
    if (start_vertex < 0 ||
        si.vertex_id[out_source_vertex[i]] < si.vertex_id[out_source_vertex[start_vertex]])
      start_vertex = i;
  }
  if (start_vertex < 0) fail_solve("make_disk_atlas: output has no boundary");
  std::vector<int> boundary_vertex;
  std::vector<double> boundary_length;
  {
    std::vector<char> visited(nout, 0);
    int current = start_vertex;
    Edge previous = nullptr;
    for (;;) {
      if (visited[current]) fail_solve("make_disk_atlas: boundary visits a vertex twice");
      visited[current] = 1;
      boundary_vertex.push_back(current);
      const auto& incident = boundary_incident[current];
      Edge next_edge = incident[0] == previous ? incident[1] : incident[0];
      int a = out_index[out.vertex_id(out.vertex1(next_edge)) - 1];
      int b = out_index[out.vertex_id(out.vertex2(next_edge)) - 1];
      const int other = a == current ? b : a;
      boundary_length.push_back(double(dist(out.point(out_vertex[current]), out.point(out_vertex[other]))));
      previous = next_edge;
      current = other;
      if (current == start_vertex) break;
      if (int(boundary_vertex.size()) > nout) fail_solve("make_disk_atlas: boundary walk did not close");
    }
  }
  if (int(boundary_vertex.size()) != num_boundary_edges) fail_solve("make_disk_atlas: multiple boundary loops");
  double perimeter = 0.;
  for (double l : boundary_length) perimeter += l;
  if (!(perimeter > 0.)) fail_solve("make_disk_atlas: degenerate boundary");
  {
    // Rotate the loop so that the chosen start copy is first.
    auto pos = std::find(boundary_vertex.begin(), boundary_vertex.end(), start_vertex);
    const std::size_t offset = std::size_t(pos - boundary_vertex.begin());
    std::rotate(boundary_vertex.begin(), boundary_vertex.begin() + offset, boundary_vertex.end());
    std::rotate(boundary_length.begin(), boundary_length.begin() + offset, boundary_length.end());
  }

  // Distinct-neighbor adjacency of the output mesh, in output vertex indices.
  std::vector<std::vector<int>> neighbors(nout);
  for (int i = 0; i < nout; ++i) {
    std::vector<int>& row = neighbors[i];
    for (Vertex w : out.vertices(out_vertex[i])) row.push_back(out_vertex_index.at(w));
    std::sort(row.begin(), row.end());
    row.erase(std::unique(row.begin(), row.end()), row.end());
    if (row.empty()) fail_solve("make_disk_atlas: isolated output vertex");
  }

  // Boundary UVs at cumulative 3D-length proportions on the unit circle.
  std::vector<double> uv_x(nout, 0.), uv_y(nout, 0.);
  {
    double cumulative = 0.;
    for (int p = 0; p < int(boundary_vertex.size()); ++p) {
      const int vi = boundary_vertex[p];
      const double angle = k_tau * cumulative / perimeter;
      uv_x[vi] = std::cos(angle);
      uv_y[vi] = std::sin(angle);
      cumulative += boundary_length[p];
    }
  }

  // Solve the uniform-weight harmonic interior (sparse, no dense matrix).
  std::vector<int> interior;
  std::vector<int> interior_index(nout, -1);
  for (int i = 0; i < nout; ++i) {
    if (is_boundary[i]) continue;
    interior_index[i] = int(interior.size());
    interior.push_back(i);
  }
  if (!interior.empty()) {
    const std::vector<double> solved_x = solve_laplacian(neighbors, interior, interior_index, is_boundary, uv_x);
    const std::vector<double> solved_y = solve_laplacian(neighbors, interior, interior_index, is_boundary, uv_y);
    for (int i = 0; i < int(interior.size()); ++i) {
      uv_x[interior[i]] = solved_x[i];
      uv_y[interior[i]] = solved_y[i];
    }
  }

  // Match the face orientation: choose the boundary direction sign that makes every UV triangle positive.
  std::vector<float> fx(nout), fy(nout);
  for (int i = 0; i < nout; ++i) {
    fx[i] = float(uv_x[i]);
    fy[i] = float(uv_y[i]);
  }
  auto face_areas_positive = [&](bool report) {
    int negative = 0, zero = 0;
    for (Face f : out.faces()) {
      Vec2<float> p[3];
      int c = 0;
      for (Corner corner : out.corners(f)) {
        const int vi = out_vertex_index.at(out.corner_vertex(corner));
        p[c++] = Vec2<float>(fx[vi], fy[vi]);
      }
      const double area =
          .5 * (double(p[0][0]) * double(p[1][1]) - double(p[1][0]) * double(p[0][1]) +
                double(p[1][0]) * double(p[2][1]) - double(p[2][0]) * double(p[1][1]) +
                double(p[2][0]) * double(p[0][1]) - double(p[0][0]) * double(p[2][1]));
      if (area < 0.) {
        ++negative;
      } else if (area == 0.) {
        ++zero;
      }
    }
    if (report && (negative || zero)) fail_solve("make_disk_atlas: degenerate or folded UV layout");
    return negative == 0 && zero == 0;
  };
  if (!face_areas_positive(false)) {
    for (int i = 0; i < nout; ++i) fy[i] = -fy[i];
    if (!face_areas_positive(true)) fail_solve("make_disk_atlas: degenerate or folded UV layout");
  }

  auto average_residual = [&]() {
    double residual = 0.;
    for (int u : interior) {
      double sx = 0., sy = 0.;
      for (int w : neighbors[u]) {
        sx += double(fx[w]);
        sy += double(fy[w]);
      }
      const double n = double(neighbors[u].size());
      residual = std::max(residual, std::abs(double(fx[u]) - sx / n));
      residual = std::max(residual, std::abs(double(fy[u]) - sy / n));
    }
    return residual;
  };
  double residual = average_residual();
  if (residual > 1e-8) {
    // Re-polish directly in the stored (float) representation.  The goal is a fixed point at which every interior
    // float coordinate is the rounded mean of its stored neighbor coordinates, so the equation residual that a
    // consumer observes after parsing the uv strings is bounded by rounding alone.  A first over-relaxed phase moves
    // quickly toward that fixed point, and a final Gauss-Seidel phase rounds each mean exactly.
    int pass = 0;
    for (; pass < 20000 && residual > 5e-8; ++pass) {
      constexpr double omega = 1.7;
      double max_change = 0.;
      for (int u : interior) {
        const int deg = int(neighbors[u].size());
        double sx = 0., sy = 0.;
        for (int w : neighbors[u]) {
          sx += double(fx[w]);
          sy += double(fy[w]);
        }
        const float nx = float((1. - omega) * double(fx[u]) + omega * sx / double(deg));
        const float ny = float((1. - omega) * double(fy[u]) + omega * sy / double(deg));
        max_change = std::max(max_change, std::abs(double(nx) - double(fx[u])));
        max_change = std::max(max_change, std::abs(double(ny) - double(fy[u])));
        fx[u] = nx;
        fy[u] = ny;
      }
      residual = average_residual();
      if (max_change == 0.) break;
    }
    for (; pass < 200000 && residual > 1e-8; ++pass) {
      double max_change = 0.;
      for (int u : interior) {
        const int deg = int(neighbors[u].size());
        double sx = 0., sy = 0.;
        for (int w : neighbors[u]) {
          sx += double(fx[w]);
          sy += double(fy[w]);
        }
        const float nx = float(sx / double(deg));
        const float ny = float(sy / double(deg));
        max_change = std::max(max_change, std::abs(double(nx) - double(fx[u])));
        max_change = std::max(max_change, std::abs(double(ny) - double(fy[u])));
        fx[u] = nx;
        fy[u] = ny;
      }
      residual = average_residual();
      if (max_change == 0.) break;
    }
  }
  if (residual > 1e-7) fail_solve("make_disk_atlas: interior UV residual too large");
  if (!face_areas_positive(true)) fail_solve("make_disk_atlas: degenerate or folded UV layout");

  // Store the UVs on every face corner.  The values are written with enough digits to round-trip through float.
  // (The boundary is in increasing output-vertex-id order, which is also a consistent boundary direction.)
  for (Face f : out.faces()) {
    for (Corner c : out.corners(f)) {
      const int vi = out_vertex_index.at(out.corner_vertex(c));
      const std::string uv = sform("(%.9g %.9g)", double(fx[vi]), double(fy[vi]));
      out.update_string(c, "uv", uv.c_str());
    }
  }

  const int chi_out = out.num_vertices() - out.num_edges() + out.num_faces();
  if (chi_out != 1) fail_solve("make_disk_atlas: output is not a disk (Euler characteristic)");

  DiskAtlasInfo info;
  normalize_pairs(primal_tree_edges);
  normalize_pairs(dual_tree_edges);
  normalize_pairs(generator_edges);
  normalize_pairs(cut_edges);
  info.primal_tree_edges = std::move(primal_tree_edges);
  info.dual_tree_edges = std::move(dual_tree_edges);
  info.generator_edges = std::move(generator_edges);
  info.cut_edges = std::move(cut_edges);
  info.residual = residual;
  result = std::move(out);
  return info;
}

}  // namespace hh
