/****************************************************************************
* VCGLib                                                            o o     *
* Visual and Computer Graphics Library                            o     o   *
*                                                                _   O  _   *
* Copyright(C) 2004-2016                                           \/)\/    *
* Visual Computing Lab                                            /\/|      *
* ISTI - Italian National Research Council                           |      *
*                                                                    \      *
* All rights reserved.                                                      *
*                                                                           *
* This program is free software; you can redistribute it and/or modify      *
* it under the terms of the GNU General Public License as published by      *
* the Free Software Foundation; either version 2 of the License, or         *
* (at your option) any later version.                                       *
*                                                                           *
* This program is distributed in the hope that it will be useful,           *
* but WITHOUT ANY WARRANTY; without even the implied warranty of            *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             *
* GNU General Public License (http://www.gnu.org/licenses/gpl.txt)          *
* for more details.                                                         *
*                                                                           *
****************************************************************************/

/*! \file trimesh_volume_preserve.cpp
\ingroup code_sample

\brief Quadric edge collapse with exact volume preservation.

Builds one or two closed, coherently oriented asymmetric shells, simplifies
them with TriEdgeCollapseQuadricParameter::PreserveVolume enabled, writes the
before/after OBJ files and prints the directed volume of every disconnected
shell before and after the reduction.
*/

#include <vcg/complex/complex.h>
#include <vcg/complex/algorithms/local_optimization.h>
#include <vcg/complex/algorithms/local_optimization/tri_edge_collapse_quadric.h>
#include <vcg/complex/algorithms/create/platonic.h>
#include <vcg/complex/algorithms/update/position.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/append.h>
#include <wrap/io_trimesh/export_obj.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

class VpVertex;
class VpFace;
struct VpTypes : vcg::UsedTypes<vcg::Use<VpVertex>::AsVertexType,
                                vcg::Use<VpFace>::AsFaceType> {};

class VpVertex : public vcg::Vertex< VpTypes,
    vcg::vertex::VFAdj, vcg::vertex::Coord3d, vcg::vertex::Mark,
    vcg::vertex::BitFlags>
{
  vcg::math::Quadric<double> qd;
public:
  vcg::math::Quadric<double> &Qd() { return qd; }
};

class VpFace : public vcg::Face< VpTypes,
    vcg::face::VFAdj, vcg::face::VertexRef, vcg::face::BitFlags> {};

class VpMesh : public vcg::tri::TriMesh<std::vector<VpVertex>,
                                        std::vector<VpFace>> {};

using VpPair = vcg::tri::BasicVertexPair<VpVertex>;

class VpCollapse
    : public vcg::tri::TriEdgeCollapseQuadric<VpMesh, VpPair, VpCollapse,
                                              vcg::tri::QInfoStandard<VpVertex>>
{
public:
  using Base =
      vcg::tri::TriEdgeCollapseQuadric<VpMesh, VpPair, VpCollapse,
                                       vcg::tri::QInfoStandard<VpVertex>>;
  VpCollapse(const VpPair &p, int i, vcg::BaseParameterClass *pp)
      : Base(p, i, pp) {}
};

namespace {

// Same asymmetric deformation used by the regression fixture.
void makeAsymmetricShell(VpMesh &m)
{
  vcg::tri::Sphere(m, 1);
  for(auto &v : m.vert)
  {
    vcg::Point3d p = v.P();
    v.P() = vcg::Point3d(1.7 * p[0] + 0.13 * p[1] * p[2],
                         0.8 * p[1],
                         1.2 * p[2] + 0.17 * p[0] * p[1]);
  }
}

double directedVolume(const VpMesh &m)
{
  double v = 0.0;
  for(const VpFace &f : m.face)
    if(!f.IsD())
      v += f.cP(0).dot(f.cP(1) ^ f.cP(2)) / 6.0;
  return v;
}

// Flood-fill connected components through VF adjacency and report each
// shell's directed volume, evaluated around its own centroid so that a shell
// placed far from the world origin keeps the correct sign and accuracy.
struct ShellInfo { double volume; size_t faces; };

std::vector<ShellInfo> perShellVolume(VpMesh &m)
{
  std::map<const VpVertex*, int> comp;
  int ncomp = 0;
  for(const VpVertex &seed : m.vert)
  {
    if(seed.IsD() || comp.count(&seed)) continue;
    std::vector<const VpVertex*> stack(1, &seed);
    comp[&seed] = ncomp;
    while(!stack.empty())
    {
      const VpVertex *cur = stack.back();
      stack.pop_back();
      for(vcg::face::VFIterator<VpFace> x(const_cast<VpVertex*>(cur));
          !x.End(); ++x)
        for(int j = 0; j < 3; ++j)
        {
          const VpVertex *w = x.F()->cV(j);
          if(!comp.count(w)) { comp[w] = ncomp; stack.push_back(w); }
        }
    }
    ++ncomp;
  }

  std::vector<vcg::Point3d> centroid(ncomp, vcg::Point3d(0,0,0));
  std::vector<size_t> nv(ncomp, 0);
  for(auto &kv : comp) { centroid[kv.second] += kv.first->cP(); ++nv[kv.second]; }
  for(int i = 0; i < ncomp; ++i) centroid[i] /= double(nv[i]);

  std::vector<ShellInfo> out(ncomp, {0.0, 0});
  for(const VpFace &f : m.face)
  {
    if(f.IsD()) continue;
    int c = comp[f.cV(0)];
    vcg::Point3d a = f.cP(0) - centroid[c],
                 b = f.cP(1) - centroid[c],
                 d = f.cP(2) - centroid[c];
    out[c].volume += a.dot(b ^ d) / 6.0;
    ++out[c].faces;
  }
  return out;
}

void writeObj(const VpMesh &m, const char *path)
{
  int err = vcg::tri::io::ExporterOBJ<VpMesh>::Save(
      const_cast<VpMesh&>(m), path, vcg::tri::io::Mask::IOM_NONE);
  if(err != 0) throw std::runtime_error(std::string("OBJ export failed: ") + path);
}

} // namespace

int main(int argc, char **argv)
{
  const char *beforePath = argc > 1 ? argv[1] : "volume_before.obj";
  const char *afterPath  = argc > 2 ? argv[2] : "volume_after.obj";
  const int targetFaces  = argc > 3 ? std::atoi(argv[3]) : 40;
  const bool twoShells   = argc <= 4 || std::string(argv[4]) != "one";

  VpMesh mesh;
  VpMesh first;
  makeAsymmetricShell(first);
  vcg::tri::Append<VpMesh, VpMesh>::Mesh(mesh, first);

  if(twoShells)
  {
    VpMesh second;
    makeAsymmetricShell(second);
    // A shell may be globally inverted: flip every face of the second copy.
    for(VpFace &f : second.face) std::swap(f.V(1), f.V(2));
    vcg::tri::UpdatePosition<VpMesh>::Translate(second, vcg::Point3d(4.5, 0, 0));
    vcg::tri::Append<VpMesh, VpMesh>::Mesh(mesh, second);
  }

  vcg::tri::UpdateTopology<VpMesh>::VertexFace(mesh);
  vcg::tri::UpdateFlags<VpMesh>::FaceBorderFromVF(mesh);
  vcg::tri::UpdateBounding<VpMesh>::Box(mesh);

  auto before = perShellVolume(mesh);
  writeObj(mesh, beforePath);
  std::cout << "input  : " << mesh.fn << " faces in " << before.size() << " shell(s)\n";
  for(size_t i = 0; i < before.size(); ++i)
    std::cout << "  shell " << i << " volume=" << before[i].volume
              << " faces=" << before[i].faces << "\n";
  std::cout << "total directed volume (world frame): " << directedVolume(mesh) << "\n";

  vcg::tri::TriEdgeCollapseQuadricParameter params;
  params.OptimalPlacement  = true; // required by PreserveVolume
  params.PreserveTopology  = true; // required by PreserveVolume
  params.PreserveVolume    = true;
  params.QualityCheck      = false;

  vcg::LocalOptimization<VpMesh> session(mesh, &params);
  session.Init<VpCollapse>();
  session.SetTargetSimplices(targetFaces);
  session.DoOptimization();
  session.Finalize<VpCollapse>();

  auto after = perShellVolume(mesh);
  writeObj(mesh, afterPath);
  std::cout << "output : " << mesh.fn << " faces in " << after.size() << " shell(s)\n";
  for(size_t i = 0; i < after.size(); ++i)
  {
    double drift = std::abs(after[i].volume - before[i].volume);
    std::cout << "  shell " << i << " volume=" << after[i].volume
              << " faces=" << after[i].faces
              << " drift=" << drift << "\n";
  }
  std::cout << "wrote " << beforePath << " and " << afterPath << "\n";
  return 0;
}
