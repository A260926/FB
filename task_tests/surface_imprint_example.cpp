// ===========================================================================
// Surface imprint example.
//
// Builds a small two-triangle mesh, draws a few segments on the triangles
// using barycentric coordinates, and turns them into real mesh edges with
// imprint_surface().  The printed map shows how each input segment keeps its
// identity and how an output edge can be attributed back to a segment.
//
// Build (from the repository root, matching task_support/run.sh):
//   c++ -std=c++17 -Isrc surface_imprint_example.cpp \
//       src/OpenMesh/Core/Mesh/*.cc src/OpenMesh/Core/System/omstream.cc \
//       -o surface_imprint_example
// ===========================================================================

#include <OpenMesh/Tools/Utils/SurfaceImprintT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>

#include <array>
#include <iostream>
#include <vector>

// A mesh kernel whose point coordinates are doubles, as requested.
struct DoublePointTraits : public OpenMesh::DefaultTraits
{
  typedef OpenMesh::Vec3d Point;
  typedef OpenMesh::Vec3d Normal;
};

typedef OpenMesh::TriMesh_ArrayKernelT<DoublePointTraits> Mesh;

static std::array<double, 3> bary(double w0, double w1, double w2)
{
  return std::array<double, 3>{{w0, w1, w2}};
}

int main()
{
  Mesh mesh;

  // Two triangles sharing edge (a,b): face 0 = (a,b,c), face 1 = (b,a,d).
  Mesh::VertexHandle a = mesh.add_vertex(Mesh::Point(0.0, 0.0, 0.0));
  Mesh::VertexHandle b = mesh.add_vertex(Mesh::Point(1.0, 0.0, 0.0));
  Mesh::VertexHandle c = mesh.add_vertex(Mesh::Point(0.0, 1.0, 0.0));
  Mesh::VertexHandle d = mesh.add_vertex(Mesh::Point(0.0, 0.0, 1.0));
  mesh.add_face(a, b, c);
  mesh.add_face(b, a, d);

  // Barycentric triples refer to the cfv_iter corner order of each face.
  // Every triple sums to 1; components in [0,1].
  std::vector<OpenMesh::Utils::SurfaceSegment> segments;

  // A crossing segment inside face 0.
  segments.push_back(OpenMesh::Utils::SurfaceSegment{
      0, bary(0.50, 0.50, 0.00), bary(0.00, 0.25, 0.75)});
  // A second segment that crosses the first one; the intersection becomes a
  // real vertex shared by both edge chains.
  segments.push_back(OpenMesh::Utils::SurfaceSegment{
      0, bary(0.25, 0.00, 0.75), bary(0.00, 0.75, 0.25)});
  // A segment ending on the shared edge (w2 == 0): its endpoint creates one
  // split vertex used by both adjacent faces, and face 1 is split even
  // though it carries no segment of its own.
  segments.push_back(OpenMesh::Utils::SurfaceSegment{
      0, bary(0.10, 0.10, 0.80), bary(0.60, 0.40, 0.00)});
  // A segment drawn on the other face along the same shared edge.  It is
  // attributed alongside the face-0 line without duplicating the split point.
  segments.push_back(OpenMesh::Utils::SurfaceSegment{
      1, bary(0.60, 0.40, 0.00), bary(0.90, 0.10, 0.00)});

  // The source mesh is read-only; a fresh mesh receives the result.
  Mesh result;
  OpenMesh::Utils::SurfaceImprintMap map;
  OpenMesh::Utils::imprint_surface(mesh, segments, result, map);

  if (!OpenMesh::Utils::MeshCheckerT<Mesh>(result).check())
  {
    std::cerr << "result mesh failed the manifold check\n";
    return 1;
  }

  std::cout << "source faces=" << mesh.n_faces()
            << "  result faces=" << result.n_faces()
            << "  result vertices=" << result.n_vertices() << "\n";

  // Every output face reports the source face it covers.
  for (size_t f = 0; f < result.n_faces(); ++f)
    std::cout << "output face " << f << " covers source face "
              << map.face_source[f] << "\n";

  // Each input segment maps to an ordered chain of output vertices; every
  // consecutive pair is a real edge that lists the segment id.
  for (size_t s = 0; s < segments.size(); ++s)
  {
    std::cout << "segment " << s << " vertex chain:";
    for (int v : map.segment_chains[s]) std::cout << ' ' << v;
    std::cout << '\n';
  }

  // Walk the edges and print the segments that cover each one.
  for (size_t e = 0; e < result.n_edges(); ++e)
  {
    const std::vector<size_t>& ids = map.edge_segments[e];
    if (ids.empty()) continue;
    std::cout << "edge " << e << " is covered by segment";
    if (ids.size() != 1) std::cout << 's';
    for (size_t id : ids) std::cout << ' ' << id;
    std::cout << '\n';
  }

  // corner_bary gives, for every halfedge, the barycentric coordinates of its
  // target vertex inside the corresponding source face (zero on boundary
  // halfedges), which is enough to re-run any per-face interpolation.
  std::cout << "corner_bary entries: " << map.corner_bary.size()
            << " (one per output halfedge)\n";
  return 0;
}
