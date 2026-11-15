// Example for vcg::tri::SplitTextureTiles.
//
// Builds a small two-patch triangle surface whose wedge texture coordinates
// span several integer UV cells (including negative tile addresses and a
// texture seam), splits it into per-tile mesh pieces and writes the result
// through the standard VCGLib PLY exporter.

#include "task_support/mesh.hpp"

#include <vcg/complex/algorithms/udim_tiles.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <wrap/io_trimesh/export_ply.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

void setCorner(UMesh &m, int face, int corner, int vertex, double u, double v, int tex)
{
  m.face[face].V(corner) = &m.vert[vertex];
  m.face[face].WT(corner).P() = vcg::Point2f(float(u), float(v));
  m.face[face].WT(corner).N() = short(tex);
}

} // namespace

int main()
{
  UMesh mesh;
  vcg::tri::Allocator<UMesh>::AddVertices(mesh, 4);
  vcg::tri::Allocator<UMesh>::AddFaces(mesh, 2);
  mesh.textures = {"paint.png", "decals.png"};

  // 3D positions: a folded quad in the XY plane.
  mesh.vert[0].P() = vcg::Point3d(0.0, 0.0, 0.0);
  mesh.vert[1].P() = vcg::Point3d(2.0, 0.0, 0.0);
  mesh.vert[2].P() = vcg::Point3d(2.0, 2.0, 0.0);
  mesh.vert[3].P() = vcg::Point3d(0.0, 2.0, 0.0);
  for (auto &v : mesh.vert) {
    v.Q() = v.P()[0] + 2.0 * v.P()[1];
    v.C() = vcg::Color4b(120, 90, 40, 255);
  }

  // First patch: UV rectangle (-1,-1)..(2,2) crosses nine integer cells.
  setCorner(mesh, 0, 0, 0, -1.0, -1.0, 0);
  setCorner(mesh, 0, 1, 1,  2.0, -1.0, 0);
  setCorner(mesh, 0, 2, 2,  2.0,  2.0, 0);

  // Second patch: a different texture and an offset UV island (a real seam).
  setCorner(mesh, 1, 0, 0, 0.2, 0.1, 1);
  setCorner(mesh, 1, 1, 2, 1.7, 1.4, 1);
  setCorner(mesh, 1, 2, 3, 0.3, 1.6, 1);

  vcg::tri::UpdateTopology<UMesh>::FaceFace(mesh);
  vcg::tri::UpdateFlags<UMesh>::FaceBorderFromFF(mesh);

  UMesh tiles;
  vcg::tri::TileSplitMap map;
  vcg::tri::SplitTextureTiles(mesh, tiles, map);

  std::printf("source faces=%d -> tile pieces: faces=%d vertices=%d tiles=%zu\n",
              mesh.fn, tiles.fn, tiles.vn, map.tiles.size());
  for (size_t i = 0; i < map.tiles.size(); ++i) {
    const auto &addr = map.tiles[i];
    std::printf("  source texture %d tile (%d,%d) -> output texture %zu: %s\n",
                addr.source_texture, addr.u, addr.v, i,
                tiles.textures[i].c_str());
  }

  std::filesystem::create_directories(".task-build");
  const std::string path = ".task-build/udim_tiles_example.ply";
  const int mask = vcg::tri::io::Mask::IOM_WEDGTEXCOORD |
                   vcg::tri::io::Mask::IOM_VERTNORMAL |
                   vcg::tri::io::Mask::IOM_VERTQUALITY |
                   vcg::tri::io::Mask::IOM_VERTCOLOR;
  int err = vcg::tri::io::ExporterPLY<UMesh>::Save(tiles, path.c_str(), mask, false);
  if (err != 0) {
    std::fprintf(stderr, "PLY export failed: %d\n", err);
    return 1;
  }
  std::printf("wrote %s\n", path.c_str());
  return 0;
}
