// -*- C++ -*-  Copyright (c) Microsoft Corporation; see LICENSE.
//
// Example: cut a closed genus-1 surface into a single disk atlas and export it as a windowless .m file with UVs.
//
// Usage:
//   disk_atlas [out.m]
//
// The program builds a non-uniform torus, calls hh::make_disk_atlas(), prints the cut-graph summary, and writes the
// resulting disk mesh (including the per-corner uv strings) to out.m (default "disk_atlas.m").  It opens no window.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "libHh/DiskAtlas.h"
#include "libHh/FileIO.h"
#include "libHh/MathOp.h"  // TAU
#include "libHh/MeshOp.h"  // mesh_genus_string()

using namespace hh;

namespace {

GMesh make_torus(int nu, int nv) {
  GMesh mesh;
  std::vector<Vertex> va;
  for (int j = 0; j < nv; ++j) {
    for (int i = 0; i < nu; ++i) {
      const float u = TAU * float(i) / float(nu), v = TAU * float(j) / float(nv);
      const float r = 0.8f + 0.3f * std::sin(2.f * v) + 0.1f * std::cos(3.f * u);
      Vertex p = mesh.create_vertex();
      mesh.set_point(p, Point((2.4f + r * std::cos(v)) * std::cos(u), (2.4f + r * std::cos(v)) * std::sin(u),
                              r * std::sin(v) + 0.15f * std::sin(2.f * u)));
      va.push_back(p);
    }
  }
  const auto at = [&](int i, int j) { return va[(j % nv) * nu + (i % nu)]; };
  for (int j = 0; j < nv; ++j) {
    for (int i = 0; i < nu; ++i) {
      mesh.create_face(at(i, j), at(i + 1, j), at(i + 1, j + 1));
      mesh.create_face(at(i, j), at(i + 1, j + 1), at(i, j + 1));
    }
  }
  return mesh;
}

}  // namespace

int main(int argc, const char** argv) {
  const std::string filename = argc > 1 ? argv[1] : "disk_atlas.m";
  GMesh source = make_torus(9, 6);
  int root = source.vertex_id(*source.vertices().begin());

  GMesh disk;
  const DiskAtlasInfo info = make_disk_atlas(source, root, disk);
  std::cout << "source: " << mesh_genus_string(source) << "\n";
  std::cout << "  primal tree edges:  " << info.primal_tree_edges.size() << "\n";
  std::cout << "  dual tree edges:    " << info.dual_tree_edges.size() << "\n";
  std::cout << "  generator edges:    " << info.generator_edges.size() << "\n";
  std::cout << "  cut edges:          " << info.cut_edges.size() << "\n";
  std::cout << "  max UV residual:    " << info.residual << "\n";
  std::cout << "cut edges (original vertex id pairs):";
  for (const auto& e : info.cut_edges) std::cout << " (" << e.first << " " << e.second << ")";
  std::cout << "\n";
  std::cout << "result: " << mesh_genus_string(disk) << "\n";
  int boundary_edges = 0;
  for (Edge e : disk.edges())
    if (disk.is_boundary(e)) ++boundary_edges;
  std::cout << "  boundary edges:     " << boundary_edges << "\n";

  WFile of(filename);
  disk.write(of());
  of().flush();
  std::cout << "wrote " << filename << " (with per-corner uv strings)\n";
  return 0;
}
