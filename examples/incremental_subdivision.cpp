// Demonstrates the incremental common subdivision mode of SignpostIntrinsicTriangulation.
//
// Usage:
//   incremental_subdivision <output.obj>
//
// Builds a non-planar triangulated patch with a boundary, enables the incremental common subdivision mode, performs
// a sequence of real edge flips and an edge split, and writes the current common subdivision to the given OBJ file.
// Exits non-zero (and prints this usage) if the file cannot be written.

#include "geometrycentral/surface/meshio.h"
#include "geometrycentral/surface/signpost_intrinsic_triangulation.h"
#include "geometrycentral/surface/vertex_position_geometry.h"

#include <iostream>
#include <string>

using namespace geometrycentral;
using namespace geometrycentral::surface;

namespace {

// A 3x3 gridded, non-planar triangular patch with a boundary. Diagonals alternate between cells so several interior
// edges are flippable.
std::unique_ptr<ManifoldSurfaceMesh> makePatch(std::vector<Vector3>& positions) {
  std::vector<Vector3> verts;
  for (size_t j = 0; j < 3; j++) {
    for (size_t i = 0; i < 3; i++) {
      double x = (double)i;
      double y = (double)j;
      double z = 0.35 * std::sin(1.3 * x + 0.4) * std::cos(0.9 * y) + 0.1 * x * y;
      verts.push_back(Vector3{x, y, z});
    }
  }

  auto vid = [](size_t i, size_t j) { return j * 3 + i; };

  std::vector<std::vector<size_t>> faces;
  for (size_t j = 0; j < 2; j++) {
    for (size_t i = 0; i < 2; i++) {
      size_t v00 = vid(i, j);
      size_t v10 = vid(i + 1, j);
      size_t v01 = vid(i, j + 1);
      size_t v11 = vid(i + 1, j + 1);
      if ((i + j) % 2 == 0) {
        faces.push_back({v00, v10, v11});
        faces.push_back({v00, v11, v01});
      } else {
        faces.push_back({v00, v10, v01});
        faces.push_back({v10, v11, v01});
      }
    }
  }

  positions = verts;
  return std::unique_ptr<ManifoldSurfaceMesh>(new ManifoldSurfaceMesh(faces));
}

} // namespace

int main(int argc, char** argv) {

  if (argc != 2) {
    std::cerr << "Usage: " << (argc > 0 ? argv[0] : "incremental_subdivision") << " <output.obj>\n"
              << "  Performs incremental intrinsic edits and writes the current common subdivision to <output.obj>.\n";
    return 2;
  }
  std::string outputPath = argv[1];

  try {
    std::vector<Vector3> rawPositions;
    std::unique_ptr<ManifoldSurfaceMesh> mesh = makePatch(rawPositions);

    VertexData<Vector3> positions(*mesh);
    for (Vertex v : mesh->vertices()) positions[v] = rawPositions[v.getIndex()];
    VertexPositionGeometry geom(*mesh, positions);
    SignpostIntrinsicTriangulation tri(*mesh, geom);

    tri.setIncrementalCommonSubdivision(true);

    // Flip a first interior edge, flip it back, then flip another one.
    Edge firstEdge;
    size_t edgeCount = 0;
    for (Edge e : tri.intrinsicMesh->edges()) {
      if (!e.isBoundary() && edgeCount++ == 0) firstEdge = e;
    }
    bool flippedFirst = tri.flipEdgeIfPossible(firstEdge);

    Edge secondEdge;
    for (Edge e : tri.intrinsicMesh->edges()) {
      if (!e.isBoundary() && e != firstEdge) {
        secondEdge = e;
        break;
      }
    }
    tri.flipEdgeIfPossible(secondEdge);
    if (flippedFirst) {
      // Reverse the first flip.
      tri.flipEdgeIfPossible(firstEdge);
    }

    // Split a non-boundary edge at a non-midpoint parameter.
    Halfedge splitHe;
    for (Edge e : tri.intrinsicMesh->edges()) {
      if (!e.isBoundary()) {
        splitHe = e.halfedge();
        break;
      }
    }
    tri.splitEdge(splitHe, 0.37);

    CommonSubdivision& cs = tri.getCommonSubdivision();
    cs.constructMesh();

    VertexData<Vector3> csPositions = cs.interpolateAcrossA(geom.inputVertexPositions);
    VertexPositionGeometry csGeom(*cs.mesh, csPositions);
    writeSurfaceMesh(*cs.mesh, csGeom, outputPath, "obj");

    std::cout << "Wrote incremental common subdivision (" << cs.mesh->nVertices() << " vertices, "
              << cs.mesh->nFaces() << " faces) to " << outputPath << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "incremental_subdivision failed: " << e.what() << "\n";
    return 1;
  }
}
