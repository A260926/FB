#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <utility>
using namespace trimesh;
static void check_at(bool x,int line){if(!x){std::fprintf(stderr,"baseline assertion failed at line %d\n",line);std::exit(1);}}
#define check(x) check_at((x),__LINE__)
int main(){
 TriMesh m;
 m.vertices={point(0,0,0),point(1,0,0),point(1,1,0),point(0,1,0)};
 m.faces={TriMesh::Face(0,1,2),TriMesh::Face(0,2,3)};
 m.colors={Color(0.0f,0.0f,0.0f),Color(1.0f,0.0f,0.0f),Color(1.0f,1.0f,0.0f),Color(0.0f,1.0f,0.0f)};
 m.confidences={0,1,2,1};
 m.need_adjacentfaces(); m.need_across_edge();
 check(m.across_edge[0][1]==1 && m.across_edge[1][2]==0);
 subdiv(&m,SUBDIV_PLANAR);
 check(m.vertices.size()==9 && m.faces.size()==8);
 check(m.colors.size()==9 && m.confidences.size()==9);
 double area=0; std::map<std::pair<int,int>,int> edges;
 for(auto f:m.faces){auto a=m.vertices[f[1]]-m.vertices[f[0]],b=m.vertices[f[2]]-m.vertices[f[0]];auto n=a CROSS b;check(n[2]>0);area+=n[2]*.5;
  for(int j=0;j<3;++j){int x=f[j],y=f[(j+1)%3];if(x>y)std::swap(x,y);++edges[{x,y}];}}
 check(std::abs(area-1)<1e-7); int boundary=0;for(auto e:edges){check(e.second==1||e.second==2);boundary+=e.second==1;}
 check(boundary==8);for(size_t i=0;i<m.vertices.size();++i){check(std::abs(m.colors[i][0]-m.vertices[i][0])<1e-7);check(std::abs(m.confidences[i]-m.vertices[i][0]-m.vertices[i][1])<1e-7);}
 m.need_across_edge();check(m.across_edge.size()==8);
 std::puts("TRIMESH_BASELINE vertices=9 faces=8 boundary=8 area=1 attributes=OK");
 std::puts("OBSERVED: original subdiv refines both root faces; no persistent refinement forest API");
}
