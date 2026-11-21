#include "geometrycentral/surface/signpost_intrinsic_triangulation.h"
#include "geometrycentral/surface/common_subdivision.h"
#include "geometrycentral/surface/vertex_position_geometry.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace geometrycentral;
using namespace geometrycentral::surface;
struct Observed : SignpostIntrinsicTriangulation {
  using SignpostIntrinsicTriangulation::SignpostIntrinsicTriangulation;
  bool cachePresent() const { return bool(commonSubdivision); }
};
static void check(bool p) { if(!p) throw std::runtime_error("production baseline check failed"); }
int main(int argc,char** argv) {
 try {
  std::vector<std::vector<size_t>> faces={{0,1,2},{0,2,3}};
  ManifoldSurfaceMesh mesh(faces);
  VertexData<Vector3> positions(mesh);
  std::vector<Vector3> raw={{0,0,0},{2,0,0},{2,1,0},{0,1,0}};
  for(Vertex v:mesh.vertices()) positions[v]=raw[v.getIndex()];
  VertexPositionGeometry geom(mesh,positions);
  Observed tri(mesh,geom);
  Edge diagonal;
  for(Edge e:tri.intrinsicMesh->edges()) if(!e.isBoundary()) diagonal=e;
  auto& first=tri.getCommonSubdivision();
  first.constructMesh();
  auto lengths=first.interpolateEdgeLengthsA(geom.edgeLengths);
  check(first.mesh->nFaces()==2 && first.mesh->nVertices()==4);
  check(tri.flipEdgeIfPossible(diagonal));
  bool dropped=!tri.cachePresent();
  auto& now=tri.getCommonSubdivision(); now.constructMesh();
  auto counts=now.elementCounts();
  check(std::get<0>(counts)==5 && std::get<2>(counts)==4);
  VertexData<double> values(mesh,0.);
  for(Vertex v:mesh.vertices()) values[v]=geom.inputVertexPositions[v].x+2*geom.inputVertexPositions[v].y;
  auto mapped=now.interpolateAcrossA(values);
  auto matrix=now.interpolationMatrixA();
  check(mapped.size()==now.mesh->nVertices() && matrix.rows()==5 && matrix.cols()==4);
  double sum=0.;for(Vertex v:now.mesh->vertices()) sum+=mapped[v]; check(std::abs(sum-10.)<1e-8);
  std::cout<<"PRODUCTION_COMMON_SUBDIVISION vertices="<<std::get<0>(counts)<<" faces="<<std::get<2>(counts)<<" color_sum="<<sum<<"\n";
  if(argc>1 && std::string(argv[1])=="baseline") { check(dropped); std::cout<<"CURRENT_CACHE_DROPPED="<<dropped<<"\n"; }
  std::cout<<"BASELINE_COMPLETE\n";return 0;
 } catch(const std::exception& e) { std::cerr<<e.what()<<"\n";return 1; }
}
