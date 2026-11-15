// Anchored LOD example: build a curved subdivided patch with measurement markers, then run the
// anchor-aware QEM simplification three times at increasing tolerances. Every level keeps the
// same marker ids and immutable targets; each marker is locally re-projected onto the surviving
// triangles of the collapse patch and may move only within its distance budget.
//
// Usage: anchored_lod <output-directory>
// Writes level{0,1,2}.obj and level{0,1,2}.csv into the directory.

#include "geometrycentral/surface/manifold_surface_mesh.h"
#include "geometrycentral/surface/mutation_manager.h"
#include "geometrycentral/surface/quadric_error_simplification.h"
#include "geometrycentral/surface/surface_anchors.h"
#include "geometrycentral/surface/vertex_position_geometry.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace geometrycentral;
using namespace geometrycentral::surface;

namespace {

struct Grid {
  std::unique_ptr<ManifoldSurfaceMesh> mesh;
  std::unique_ptr<VertexPositionGeometry> geom;
  size_t n = 0;
};

Grid makeWavyGrid(size_t n) {
  Grid g;
  g.n = n;
  std::vector<Vector3> positions;
  positions.reserve(n * n);
  for (size_t y = 0; y < n; y++) {
    for (size_t x = 0; x < n; x++) {
      double u = double(x) / double(n - 1);
      double v = double(y) / double(n - 1);
      double z = 0.18 * std::sin(2.5 * u + 0.4) * std::cos(2.0 * v) + 0.05 * u * v;
      positions.push_back(Vector3{u, v, z});
    }
  }

  std::vector<std::vector<size_t>> faces;
  for (size_t y = 0; y + 1 < n; y++) {
    for (size_t x = 0; x + 1 < n; x++) {
      size_t a = y * n + x;
      size_t b = a + 1;
      size_t c = a + n;
      size_t d = c + 1;
      faces.push_back({a, b, d});
      faces.push_back({a, d, c});
    }
  }

  g.mesh = std::unique_ptr<ManifoldSurfaceMesh>(new ManifoldSurfaceMesh(faces));
  VertexData<Vector3> pos(*g.mesh);
  for (Vertex v : g.mesh->vertices()) pos[v] = positions[v.getIndex()];
  g.geom = std::unique_ptr<VertexPositionGeometry>(new VertexPositionGeometry(*g.mesh, pos));
  return g;
}

void writeObj(const std::filesystem::path& path, ManifoldSurfaceMesh& mesh, VertexPositionGeometry& geom) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("could not open " + path.string());
  out << std::setprecision(std::numeric_limits<double>::max_digits10);

  VertexData<size_t> index = mesh.getVertexIndices();
  for (Vertex v : mesh.vertices()) {
    const Vector3& p = geom.vertexPositions[v];
    out << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
  }
  for (Face f : mesh.faces()) {
    out << "f";
    for (Vertex v : f.adjacentVertices()) out << ' ' << index[v] + 1;
    out << '\n';
  }
  if (!out) throw std::runtime_error("failed writing " + path.string());
}

void writeCsv(const std::filesystem::path& path, const std::vector<SurfaceAnchorSnapshot>& snaps) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("could not open " + path.string());
  out << "id,target_x,target_y,target_z,x,y,z,distance\n";
  out << std::setprecision(std::numeric_limits<double>::max_digits10);
  for (const SurfaceAnchorSnapshot& s : snaps) {
    double distance = (s.position - s.target).norm();
    out << s.id << ',' << s.target.x << ',' << s.target.y << ',' << s.target.z << ',' << s.position.x << ','
        << s.position.y << ',' << s.position.z << ',' << distance << '\n';
  }
  if (!out) throw std::runtime_error("failed writing " + path.string());
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: anchored_lod <output-directory>");
    std::filesystem::path outDir = argv[1];
    std::filesystem::create_directories(outDir);

    Grid grid = makeWavyGrid(22);
    ManifoldSurfaceMesh& mesh = *grid.mesh;
    VertexPositionGeometry& geom = *grid.geom;

    // Measurement markers: face-interior points spread over the patch, well away from the border.
    std::vector<SurfaceAnchorInput> inputs;
    uint64_t id = 1;
    for (size_t iy = 0; iy < 4; iy++) {
      for (size_t ix = 0; ix < 4; ix++) {
        size_t cellX = 3 + ix * 4;
        size_t cellY = 3 + iy * 4;
        size_t faceIndex = 2 * (cellY * 21 + cellX); // first triangle of the grid cell
        Face f = mesh.face(faceIndex);
        SurfaceAnchorInput input;
        input.id = id++;
        input.point = SurfacePoint(f, Vector3{0.25, 0.35, 0.40});
        input.maxDistance = 0.08;
        inputs.push_back(input);
      }
    }

    MutationManager mm(mesh, geom);
    SurfaceAnchorSet anchors(mm, inputs);

    const double tolerances[3] = {0.0, 1e-5, 5e-4};
    size_t previousFaces = mesh.nFaces();
    for (int level = 0; level < 3; level++) {
      if (level > 0) {
        quadricErrorSimplify(mesh, geom, tolerances[level], mm);
        mesh.compress();
      }
      if (mesh.nFaces() > previousFaces) throw std::runtime_error("face count increased between levels");
      previousFaces = mesh.nFaces();

      std::string stem = "level" + std::to_string(level);
      writeObj(outDir / (stem + ".obj"), mesh, geom);
      writeCsv(outDir / (stem + ".csv"), anchors.snapshot());

      std::cout << "LOD " << level << " faces=" << mesh.nFaces() << " markers=" << inputs.size()
                << " anchorVisits=" << anchors.anchorVisits()
                << " projectionTriangleTests=" << anchors.projectionTriangleTests() << '\n';
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "anchored_lod failed: " << e.what() << '\n';
    return 1;
  }
}
