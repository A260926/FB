// Example: build intrinsic edge lengths from the initial geometry, run the
// intrinsic Delaunay flip algorithm, and assemble the discrete cotangent
// Laplacian L from the resulting weights.  Applied to a constant vector the
// result is zero (each row of the stiffness matrix sums to zero).

#include <OpenMesh/Core/Geometry/VectorT.hh>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Utils/IntrinsicDelaunayT.hh>

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

typedef OpenMesh::TriMesh_ArrayKernelT<> Mesh;

int main()
{
  // Folded surface: two triangles sharing the diagonal ac.
  Mesh mesh;
  Mesh::VertexHandle a = mesh.add_vertex(Mesh::Point(-1, 0, 0));
  Mesh::VertexHandle b = mesh.add_vertex(Mesh::Point( 0, 0.2f, 0));
  Mesh::VertexHandle c = mesh.add_vertex(Mesh::Point( 1, 0, 0));
  Mesh::VertexHandle d = mesh.add_vertex(Mesh::Point( 0, 0, 2));
  mesh.add_face(a, b, c);
  mesh.add_face(a, c, d);

  OpenMesh::EPropHandleT<double> lengths, weights;
  OpenMesh::EPropHandleT<bool>   locked;
  mesh.add_property(lengths, "intrinsic_edge_length");
  mesh.add_property(weights, "cotangent_weight");
  mesh.add_property(locked,  "locked_edge");

  // Step 1: initialise the intrinsic metric from the current geometry.
  for (Mesh::EdgeIter e = mesh.edges_begin(); e != mesh.edges_end(); ++e)
  {
    Mesh::HalfedgeHandle h = mesh.halfedge_handle(*e, 0);
    mesh.property(lengths, *e) =
      (mesh.point(mesh.to_vertex_handle(h)) -
       mesh.point(mesh.from_vertex_handle(h))).norm();
    mesh.property(locked, *e) = false;
  }

  // Step 2: intrinsic Delaunay flips; weights now live on the new edges.
  OpenMesh::Utils::IntrinsicDelaunayReport report =
    OpenMesh::Utils::make_intrinsic_delaunay(mesh, lengths, locked, weights);
  std::cout << "performed " << report.flips << " intrinsic flip(s)\n";

  // Step 3: assemble the sparse cotangent Laplacian L.
  //         For an edge (u,v): L_uu += w, L_vv += w, L_uv = L_vu = -w.
  size_t n = mesh.n_vertices();
  std::vector<double> diagonal(n, 0.0);
  std::vector<std::vector<std::pair<int, double> > > off_diagonal(n);

  for (Mesh::EdgeIter e = mesh.edges_begin(); e != mesh.edges_end(); ++e)
  {
    Mesh::HalfedgeHandle h = mesh.halfedge_handle(*e, 0);
    int u = mesh.from_vertex_handle(h).idx();
    int v = mesh.to_vertex_handle(h).idx();
    double w = mesh.property(weights, *e);

    diagonal[u] += w;
    diagonal[v] += w;
    off_diagonal[u].push_back(std::make_pair(v, -w));
    off_diagonal[v].push_back(std::make_pair(u, -w));
  }

  // Step 4: multiply L by the constant vector x = (1, 1, ..., 1).
  std::vector<double> x(n, 1.0), y(n, 0.0);
  for (size_t i = 0; i < n; ++i)
  {
    y[i] = diagonal[i] * x[i];
    for (size_t k = 0; k < off_diagonal[i].size(); ++k)
      y[i] += off_diagonal[i][k].second * x[off_diagonal[i][k].first];
    assert(std::fabs(y[i]) < 1e-12);
  }

  std::cout << "L * 1 = 0 at every vertex: laplace-example PASS\n";
  return 0;
}
