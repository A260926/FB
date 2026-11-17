// -*- C++ -*-  Copyright (c) Microsoft Corporation; see LICENSE.
// Regression tests for GMesh::bevel_edges() (edge beveling of a closed strictly convex triangular mesh).
#include "libHh/GMesh.h"

#include <cmath>
#include <sstream>
#include <string>

#include "task_support/check.h"

namespace {
using namespace hh;

// Outward-oriented regular tetrahedron centered at the origin.
GMesh make_tetra() {
  GMesh m;
  std::istringstream in("Vertex 1 1 1 1\nVertex 2 1 -1 -1\nVertex 3 -1 1 -1\nVertex 4 -1 -1 1\n"
                        "Face 1 1 2 3\nFace 2 1 3 4\nFace 3 1 4 2\nFace 4 2 4 3\n");
  m.read(in);
  m.ok();
  return m;
}

// Regular octahedron: 6 vertices, 8 triangular faces; faces 3 and 7 are disjoint from vertices 1 and 3.
GMesh make_octa() {
  GMesh m;
  std::istringstream in(
      "Vertex 1 1 0 0\nVertex 2 -1 0 0\nVertex 3 0 1 0\nVertex 4 0 -1 0\nVertex 5 0 0 1\nVertex 6 0 0 -1\n"
      "Face 1 1 3 5\nFace 2 3 2 5\nFace 3 2 4 5\nFace 4 4 1 5\n"
      "Face 5 3 1 6\nFace 6 2 3 6\nFace 7 4 2 6\nFace 8 1 4 6\n");
  m.read(in);
  m.ok();
  return m;
}

// Distance from p to the line through a and b.
float point_line_distance(const Point& p, const Point& a, const Point& b) {
  const Vector ab = b - a, ap = p - a;
  return mag(ap - normalized(ab) * dot(ap, normalized(ab)));
}

// Barycentric weights of p in triangle (a, b, c).
Vec3<float> barycentric(const Point& p, const Point& a, const Point& b, const Point& c) {
  const Vector e1 = b - a, e2 = c - a, ep = p - a;
  const float d00 = dot(e1, e1), d01 = dot(e1, e2), d11 = dot(e2, e2);
  const float d20 = dot(ep, e1), d21 = dot(ep, e2);
  const float denom = d00 * d11 - d01 * d01;
  const float w1 = (d11 * d20 - d01 * d21) / denom;
  const float w2 = (d00 * d21 - d01 * d20) / denom;
  return Vec3<float>(1.f - w1 - w2, w1, w2);
}

Face find_face_with(GMesh& m, const char* key) {
  for (Face f : m.faces())
    if (const char* s = m.get_string(f); s && strstr(s, key)) return f;
  return nullptr;
}

// Every edge is interior with two oppositely oriented faces; no face is degenerate; all positions are finite.
int check_solid(GMesh& m) {
  m.ok();
  for (Edge e : m.edges()) TASK_CHECK(!m.is_boundary(e));
  for (Face f : m.faces()) {
    TASK_CHECK(m.area(f) > 0.f);
    TASK_CHECK(m.num_vertices(f) >= 3);
    Set<Vertex> seen;
    for (Vertex v : m.vertices(f)) TASK_CHECK(seen.add(v));  // No repeated vertex within a face.
  }
  for (Vertex v : m.vertices())
    for_int(c, 3) TASK_CHECK(std::isfinite(m.point(v)[c]));
  return 0;
}

// Full structural and attribute equality by element id.
bool meshes_equal(const GMesh& a, const GMesh& b) {
  if (a.num_vertices() != b.num_vertices() || a.num_faces() != b.num_faces() || a.num_edges() != b.num_edges())
    return false;
  for (Vertex va : a.ordered_vertices()) {
    Vertex vb = b.id_retrieve_vertex(a.vertex_id(va));
    if (!vb || dist(a.point(va), b.point(vb)) > 1e-5f) return false;
    const char *sa = a.get_string(va), *sb = b.get_string(vb);
    if ((!sa) != (!sb) || (sa && strcmp(sa, sb))) return false;
  }
  for (Face fa : a.ordered_faces()) {
    Face fb = b.id_retrieve_face(a.face_id(fa));
    if (!fb || a.num_vertices(fa) != b.num_vertices(fb)) return false;
    const char *sa = a.get_string(fa), *sb = b.get_string(fb);
    if ((!sa) != (!sb) || (sa && strcmp(sa, sb))) return false;
    Array<Vertex> va, vb;
    a.get_vertices(fa, va);
    b.get_vertices(fb, vb);
    for_int(i, va.num()) if (a.vertex_id(va[i]) != b.vertex_id(vb[i])) return false;
    for (Corner ca : a.corners(fa)) {
      Corner cb = b.corner(b.id_vertex(a.vertex_id(a.corner_vertex(ca))), fb);
      const char *sca = a.get_string(ca), *scb = b.get_string(cb);
      if ((!sca) != (!scb) || (sca && strcmp(sca, scb))) return false;
    }
  }
  for (Edge ea : a.edges()) {
    Edge eb = b.edge(b.id_vertex(a.vertex_id(a.vertex1(ea))), b.id_vertex(a.vertex_id(a.vertex2(ea))));
    const char *sa = a.get_string(ea), *sb = b.get_string(eb);
    if ((!sa) != (!sb) || (sa && strcmp(sa, sb))) return false;
  }
  return true;
}

GMesh replayed(const std::string& seed, const std::string& edits) {
  GMesh r;
  std::istringstream whole(seed + edits);
  r.read(whole);
  r.ok();
  return r;
}

// *** bevel_geometry ***

int tetra_actual_width() {
  GMesh m = make_tetra();
  const float width = 0.2f;
  Edge e = m.edge(m.id_vertex(1), m.id_vertex(2));
  const Point a = m.point(m.vertex1(e)), b = m.point(m.vertex2(e));
  const Face fa = m.face1(e), fb = m.face2(e);
  const Plane pa = plane_of_triangle(m.triangle_points(fa));
  const Plane pb = plane_of_triangle(m.triangle_points(fb));
  m.bevel_edges(CArrayView<Edge>(&e, 1), width);
  check_solid(m);
  Face bev = find_face_with(m, "source_edge");
  TASK_CHECK(bev);
  TASK_CHECK(m.num_vertices(bev) == 4);  // One quadrilateral bevel face.
  for (Vertex v : m.vertices(bev)) {
    // The width is the distance measured perpendicular to the edge inside either original face.
    TASK_CHECK(abs(point_line_distance(m.point(v), a, b) - width) <= 1e-5f);
    // Every bevel vertex lies in one of the two adjacent original face planes.
    const float ra = abs(dot(m.point(v), pa.nor) - pa.d);
    const float rb = abs(dot(m.point(v), pb.nor) - pb.d);
    TASK_CHECK(min(ra, rb) <= 1e-5f);
  }
  return 0;
}

int halfspace_definition_and_polygons() {
  GMesh m = make_tetra();
  const float width = 0.18f;
  Edge e = m.edge(m.id_vertex(1), m.id_vertex(2));
  const Vector n1 = get_normal(m.triangle_points(m.face1(e)));
  const Vector n2 = get_normal(m.triangle_points(m.face2(e)));
  const Point pe = m.point(m.vertex1(e));
  const Vector sum = n1 + n2;
  const float smag = mag(sum);
  const Vector hn = sum / smag;
  // Spec half-space uses the unnormalized normal (n1+n2); with the unit hn its offset divides by |n1+n2|.
  const float root = sqrt(max(0.f, 1.f - dot(n1, n2) * dot(n1, n2)));
  const float limit = dot(hn, pe) - width * root / smag;
  m.bevel_edges(CArrayView<Edge>(&e, 1), width);
  Face bev = find_face_with(m, "source_edge");
  TASK_CHECK(bev);
  for (Vertex v : m.vertices(bev)) {
    TASK_CHECK(abs(dot(m.point(v), hn) - limit) <= 1e-5f);  // Bevel vertices lie exactly on the cutting plane.
  }
  // The output keeps polygonal faces (not forced to triangles): the bevel face is a quad.
  TASK_CHECK(!m.is_triangle(bev));
  // Every original-derived face carries source_face; the bevel face carries source_edge with ascending endpoints.
  int nsource_face = 0;
  for (Face f : m.faces()) {
    const char* s = m.get_string(f);
    TASK_CHECK(s);
    if (strstr(s, "source_face")) ++nsource_face;
  }
  TASK_CHECK(nsource_face == 4);
  const char* bes = m.get_string(bev);
  TASK_CHECK(strstr(bes, "source_edge=(1 2)"));
  return 0;
}

int geometry_order_independence() {
  Array<Edge> es1, es2;
  GMesh a = make_tetra();
  for (Edge e : a.edges(a.id_vertex(1))) es1.push(e);
  GMesh b = make_tetra();
  for (Edge e : b.edges(b.id_vertex(1))) es2.push(e);
  Array<Edge> rev;
  for (int i = es2.num() - 1; i >= 0; --i) rev.push(es2[i]);  // Reverse b's own selection order.
  es2 = rev;
  a.bevel_edges(CArrayView<Edge>(es1.data(), es1.num()), 0.13f);
  b.bevel_edges(CArrayView<Edge>(es2.data(), es2.num()), 0.13f);
  TASK_CHECK(meshes_equal(a, b));
  return 0;
}

// *** junction_topology ***

int three_edges_share_a_vertex() {
  GMesh m = make_tetra();
  Array<Edge> es;
  for (Edge e : m.edges(m.id_vertex(1))) es.push(e);
  TASK_CHECK(es.num() == 3);
  m.bevel_edges(CArrayView<Edge>(es.data(), es.num()), 0.15f);
  check_solid(m);
  // Three bevel faces, one per selected edge, and they meet at a single shared junction vertex.
  Array<Face> bevs;
  for (Face f : m.faces())
    if (const char* s = m.get_string(f); s && strstr(s, "source_edge")) bevs.push(f);
  TASK_CHECK(bevs.num() == 3);
  Set<Vertex> junction;
  for (Face f : bevs)
    for (Vertex v : m.vertices(f)) junction.add(v);
  // A point shared by all three bevel faces is the real junction vertex (not three coincident patches).
  int common = 0;
  for (Vertex v : junction) {
    int count = 0;
    for (Face f : bevs)
      for (Vertex vv : m.vertices(f))
        if (vv == v) ++count;
    if (count == 3) ++common;
  }
  TASK_CHECK(common >= 1);
  // Euler characteristic of the closed solid.
  TASK_CHECK(m.num_vertices() - m.num_edges() + m.num_faces() == 2);
  return 0;
}

int untouched_handles_survive() {
  GMesh m = make_octa();
  Edge cut = m.edge(m.id_vertex(1), m.id_vertex(3));
  const Face far_face = m.id_face(7);       // (4,2,6): neither endpoint is 1 or 3.
  const Vertex far_vertex = m.id_vertex(6);
  const Edge far_edge = m.edge(m.id_vertex(2), m.id_vertex(6));
  m.set_string(far_face, "mat=brick");
  m.set_string(far_vertex, "keep=vertex");
  m.set_string(far_edge, "keep=edge");
  std::ostringstream seed, edits;
  m.write(seed);
  m.record_changes(&edits);
  m.bevel_edges(CArrayView<Edge>(&cut, 1), 0.1f);
  m.record_changes(nullptr);
  check_solid(m);
  TASK_CHECK(m.id_retrieve_face(7) == far_face);          // The face handle is unchanged.
  TASK_CHECK(m.is_triangle(far_face));                    // And it is still the original triangle.
  TASK_CHECK(m.id_retrieve_vertex(6) == far_vertex);      // The vertex handle is unchanged.
  TASK_CHECK(m.edge(m.id_vertex(2), m.id_vertex(6)) == far_edge);  // The edge handle is unchanged.
  const char* fs = m.get_string(far_face);
  TASK_CHECK(fs && strstr(fs, "mat=brick") && strstr(fs, "source_face=7"));
  TASK_CHECK(strstr(m.get_string(far_vertex), "keep=vertex"));
  TASK_CHECK(strstr(m.get_string(far_edge), "keep=edge"));
  // Replay preserves every string, including the one attached to a preserved edge.
  GMesh r = replayed(seed.str(), edits.str());
  TASK_CHECK(meshes_equal(m, r));
  Edge re = r.edge(r.id_vertex(2), r.id_vertex(6));
  TASK_CHECK(strstr(r.get_string(re), "keep=edge"));
  return 0;
}

// *** attributes_replay ***

int uv_seam_and_replay() {
  GMesh m = make_tetra();
  // Face 1 gets a full corner-uv chart; face 2 gets a different complete chart (shared vertices 1 and 3 differ): a seam.
  m.set_string(m.corner(m.id_vertex(1), m.id_face(1)), "uv=(0 0)");
  m.set_string(m.corner(m.id_vertex(2), m.id_face(1)), "uv=(1 0)");
  m.set_string(m.corner(m.id_vertex(3), m.id_face(1)), "uv=(0 1)");
  m.set_string(m.corner(m.id_vertex(1), m.id_face(2)), "uv=(2 2)");
  m.set_string(m.corner(m.id_vertex(3), m.id_face(2)), "uv=(5 5)");
  m.set_string(m.corner(m.id_vertex(4), m.id_face(2)), "uv=(7 7)");
  Edge e = m.edge(m.id_vertex(1), m.id_vertex(2));
  std::ostringstream seed, edits;
  m.write(seed);
  m.record_changes(&edits);
  m.bevel_edges(CArrayView<Edge>(&e, 1), 0.1f);
  m.record_changes(nullptr);
  check_solid(m);
  // The recreated face 1 keeps uvs on every corner, interpolated by barycentric coordinates.
  Face f1 = find_face_with(m, "source_face=1");
  TASK_CHECK(f1);
  const Vec3<Point> tri = V(Point(1, 1, 1), Point(1, -1, -1), Point(-1, 1, -1));
  for (Corner c : m.corners(f1)) {
    Uv uv;
    TASK_CHECK(m.parse_corner_key_vec(c, "uv", uv));
    const Vec3<float> w = barycentric(m.point(m.corner_vertex(c)), tri[0], tri[1], tri[2]);
    const Uv expected(w[1], w[2]);  // Corner uvs were (0,0), (1,0), (0,1).
    TASK_CHECK(abs(uv[0] - expected[0]) <= 1e-5f && abs(uv[1] - expected[1]) <= 1e-5f);
  }
  // The seam is preserved: vertex 3 carries a different uv on face 2 than on face 1.
  Face f2 = find_face_with(m, "source_face=2");
  TASK_CHECK(f2);
  Uv uv2;
  TASK_CHECK(m.parse_corner_key_vec(m.corner(m.id_vertex(3), f2), "uv", uv2));
  TASK_CHECK(abs(uv2[0] - 5.f) <= 1e-5f && abs(uv2[1] - 5.f) <= 1e-5f);
  // Bevel faces never inherit uvs.
  Face bev = find_face_with(m, "source_edge");
  TASK_CHECK(bev);
  for (Corner c : m.corners(bev)) TASK_CHECK(!GMesh::string_has_key(m.get_string(c), "uv"));
  // Replay restores the corner attributes exactly.
  GMesh r = replayed(seed.str(), edits.str());
  TASK_CHECK(meshes_equal(m, r));
  return 0;
}

int bevel_then_attribute_edits_replay() {
  GMesh m = make_tetra();
  Edge e = m.edge(m.id_vertex(1), m.id_vertex(2));
  std::ostringstream seed, edits;
  m.write(seed);
  m.record_changes(&edits);
  m.bevel_edges(CArrayView<Edge>(&e, 1), 0.1f);
  Face bev = find_face_with(m, "source_edge");
  TASK_CHECK(bev);
  Vertex nv = nullptr;
  for (Vertex v : m.vertices())
    if (m.vertex_id(v) >= 5) nv = v;
  TASK_CHECK(nv);
  Face orig = find_face_with(m, "source_face");
  Edge ee = *m.edges().begin();
  Corner cc = *m.corners(bev).begin();
  // Add, overwrite, and delete keys on every element kind, including a brand-new bevel vertex.
  m.set_string(nv, "tag=new");
  m.update_string(orig, "mat", "red");
  m.update_string(orig, "mat", "blue");
  m.update_string(orig, "mat", nullptr);  // Delete the key.
  m.set_string(ee, "sharp");
  m.update_string(ee, "sharp", nullptr);  // Delete again, leaving no edge string.
  m.update_string(cc, "note", "x");
  m.update_string(cc, "note", nullptr);  // Delete the corner key.
  m.record_changes(nullptr);
  // The edit log is incremental, never a full-mesh rewrite.
  std::istringstream rs(edits.str());
  for (std::string line; std::getline(rs, line);) TASK_CHECK(!line.starts_with("Vertex "));
  GMesh r = replayed(seed.str(), edits.str());
  TASK_CHECK(meshes_equal(m, r));
  TASK_CHECK(strstr(m.get_string(nv), "tag=new"));
  TASK_CHECK(!GMesh::string_has_key(m.get_string(orig), "mat"));
  TASK_CHECK(!m.get_string(ee));
  TASK_CHECK(!GMesh::string_has_key(m.get_string(cc), "note"));
  return 0;
}

// *** compatibility ***

int invalid_arguments_rejected() {
  GMesh m = make_tetra();
  Edge e = m.edge(m.id_vertex(1), m.id_vertex(2));
  auto throws_ia = [&](auto&& fn) -> int {
    bool threw = false;
    try {
      fn();
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    TASK_CHECK(threw);
    return 0;
  };
  GMesh other = make_tetra();
  Edge foreign = other.edge(other.id_vertex(1), other.id_vertex(2));
  throws_ia([&] { m.bevel_edges(CArrayView<Edge>(&foreign, 1), 0.1f); });
  Array<Edge> dup{e, e};
  throws_ia([&] { m.bevel_edges(CArrayView<Edge>(dup.data(), dup.num()), 0.1f); });
  for (float w : {0.f, -0.5f, NAN, INFINITY})
    throws_ia([&] { m.bevel_edges(CArrayView<Edge>(&e, 1), w); });
  // Empty selection is a no-op.
  const int nv = m.num_vertices(), nf = m.num_faces();
  m.bevel_edges(CArrayView<Edge>(static_cast<const Edge*>(nullptr), 0), 0.1f);
  TASK_CHECK(m.num_vertices() == nv && m.num_faces() == nf);
  return 0;
}

int line_recognition_and_old_text() {
  static constexpr const char* recognized[] = {
      "Vertex 1 0 0 0",          "Face 1 1 2 3",       "Edge 1 2 {sharp}",
      "MBevV 0.1 0.2 0.3",      "MBevF 5 6 7 {source_edge=(5 6)}", "MBevDF 2",
      "MBevDV 3",               "MStrV 1 {a=b}",      "MStrF 2 {a=b}",
      "MStrE 1 2 {sharp}",      "MStrC 1 2 {uv=(0 0)}"};
  for (const char* line : recognized) TASK_CHECK(GMesh::recognize_line(line));
  TASK_CHECK(!GMesh::recognize_line("# a comment"));
  // Old seed text (the pre-bevel format) still reads, and a recorded bevel replays on top of it.
  GMesh m = make_tetra();
  Edge e = m.edge(m.id_vertex(1), m.id_vertex(2));
  std::ostringstream seed, edits;
  m.write(seed);
  m.record_changes(&edits);
  m.bevel_edges(CArrayView<Edge>(&e, 1), 0.12f);
  m.record_changes(nullptr);
  GMesh r = replayed(seed.str(), edits.str());
  TASK_CHECK(meshes_equal(m, r));
  return 0;
}

}  // namespace

const task_case task_cases[] = {
    {"bevel_geometry", "tetra_actual_width", tetra_actual_width},
    {"bevel_geometry", "halfspace_definition_and_polygons", halfspace_definition_and_polygons},
    {"bevel_geometry", "geometry_order_independence", geometry_order_independence},
    {"junction_topology", "three_edges_share_a_vertex", three_edges_share_a_vertex},
    {"junction_topology", "untouched_handles_survive", untouched_handles_survive},
    {"attributes_replay", "uv_seam_and_replay", uv_seam_and_replay},
    {"attributes_replay", "bevel_then_attribute_edits_replay", bevel_then_attribute_edits_replay},
    {"compatibility", "invalid_arguments_rejected", invalid_arguments_rejected},
    {"compatibility", "line_recognition_and_old_text", line_recognition_and_old_text},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
