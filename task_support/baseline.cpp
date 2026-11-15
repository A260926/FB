#include "geometrycentral/surface/quadric_error_simplification.h"
#include "geometrycentral/surface/surface_point.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
using namespace geometrycentral;
using namespace geometrycentral::surface;
struct Grid {
  std::unique_ptr<ManifoldSurfaceMesh> mesh;
  std::unique_ptr<VertexPositionGeometry> geom;
  Grid(size_t n) {
    std::vector<std::vector<size_t>> fs;
    std::vector<Vector3> ps;
    for(size_t y=0;y<n;++y) for(size_t x=0;x<n;++x) {
      double xx=double(x)/(n-1), yy=double(y)/(n-1);
      ps.push_back({xx,yy,0.12*std::sin(3*xx)*std::cos(2*yy)+0.03*xx*yy});
    }
    for(size_t y=0;y+1<n;++y) for(size_t x=0;x+1<n;++x) {
      size_t a=y*n+x,b=a+1,c=a+n,d=c+1;
      fs.push_back({a,b,d});fs.push_back({a,d,c});
    }
    mesh=std::make_unique<ManifoldSurfaceMesh>(fs);
    VertexData<Vector3> positions(*mesh);
    for(Vertex v:mesh->vertices()) positions[v]=ps.at(v.getIndex());
    geom=std::make_unique<VertexPositionGeometry>(*mesh,positions);
  }
};
int main(int argc,char** argv) {
  try {
    Grid g(9);auto& mesh=*g.mesh;auto& geom=*g.geom;
    size_t before=mesh.nFaces();
    MutationManager mm(mesh,geom);
    VertexData<bool> movable(mesh,true);
    for(Vertex v:mesh.vertices()) if(v.isBoundary()) movable[v]=false;
    mm.setRepositionableVertices(movable);
    quadricErrorSimplify(mesh,geom,0.001,mm);
    mesh.validateConnectivity();
    if(mesh.nFaces()==0 || mesh.nFaces()>=before) throw std::runtime_error("normal QEM failed to produce nonempty simplification");
    for(Vertex v:mesh.vertices()) {
      Vector3 p=geom.vertexPositions[v];
      if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))throw std::runtime_error("nonfinite QEM position");
    }
    std::cout<<"QEM_NORMAL before="<<before<<" after="<<mesh.nFaces()<<" connectivity=valid finite=true\n";
    if(argc>1 && std::string(argv[1])=="--witness") {
      Grid w(5); MutationManager wm(*w.mesh,*w.geom);
      Edge chosen;
      for(Edge e:w.mesh->edges()) if(!e.isBoundary() && !e.halfedge().vertex().isBoundary() && !e.halfedge().tipVertex().isBoundary()){chosen=e;break;}
      if(chosen==Edge())throw std::runtime_error("missing witness edge");
      Face raw=chosen.halfedge().face();SurfacePoint marker(raw,{0.2,0.3,0.5});
      Vector3 initial=marker.interpolate(w.geom->vertexPositions);
      Vector3 a=w.geom->vertexPositions[chosen.halfedge().vertex()],b=w.geom->vertexPositions[chosen.halfedge().tipVertex()];
      Vector3 next=0.5*(a+b);
      double pre=-99,post=-99;int preCount=0,postCount=0;
      wm.registerEdgeCollapseHandlers([&](Edge,double t){pre=t;++preCount;},[&](Vertex,double t){post=t;++postCount;});
      Vertex result=wm.collapseEdge(chosen,next);
      bool dead=raw.isDead();
      std::cout<<"RAW_SURFACE_POINT collapse="<<(result!=Vertex())<<" old_face_dead="<<dead<<" callbacks="<<preCount<<","<<postCount<<" pre_t="<<pre<<" post_t="<<post<<" expected_projection_t=0.5\n";
      std::cout<<"RAW_TARGET "<<initial.x<<","<<initial.y<<","<<initial.z<<"\n";
      w.mesh->compress();w.mesh->validateConnectivity();
      std::cout<<"EXPLICIT_COMPRESS faces="<<w.mesh->nFaces()<<" connectivity=valid\n";
    }
    return 0;
  } catch(const std::exception& e) {std::cerr<<"BASELINE_FAIL "<<e.what()<<"\n";return 2;}
}
