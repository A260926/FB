#include "pmp/algorithms/shapes.h"
#include "pmp/algorithms/subdivision.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace pmp;
static void require(bool x){if(!x)throw std::runtime_error("production subdivision regression");}
int main(){
 auto closed=hexahedron(); auto p=closed.vertex_property<Point>("v:point");
 for(auto v:closed.vertices()){auto q=p[v];p[v]=Point(1.2*q[0]+0.3*q[1],0.7*q[1],q[2]+0.2*q[0]);}
 catmull_clark_subdivision(closed); require(closed.n_vertices()==26 && closed.n_faces()==24);
 catmull_clark_subdivision(closed); require(closed.n_vertices()==98 && closed.n_faces()==96);
 for(auto v:closed.vertices())for(int j=0;j<3;j++)require(std::isfinite(closed.position(v)[j]));
 auto open=plane(1);auto tex=open.halfedge_property<TexCoord>("h:tex");
 for(auto h:open.halfedges())tex[h]=TexCoord(3+0.2*h.idx(),-2-0.3*h.idx());
 catmull_clark_subdivision(open,BoundaryHandling::Preserve);
 require(open.n_vertices()==9 && open.n_faces()==4);
 size_t zeros=0;for(auto f:open.faces())for(auto h:open.halfedges(f))if(tex[h]==TexCoord(0,0))++zeros;
 std::cout<<"PMP_PRODUCTION_OK closed_faces="<<closed.n_faces()<<" open_faces="<<open.n_faces()<<" legacy_default_uv_corners="<<zeros<<"\n";
}
