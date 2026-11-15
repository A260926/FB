#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cstdio>
using namespace trimesh;
int main(){
 // Baseline fixture: 4 faces share edge 0-1, 2 forward 2 backward
 TriMesh m;
 m.vertices={point(0,0,0),point(1,0,0),point(0,1,0),point(0,-1,0),point(0,0,1),point(0,0,-1),point(0,2,0),point(0,-2,0)};
 m.faces={TriMesh::Face(0,1,2),TriMesh::Face(1,0,3),TriMesh::Face(0,1,4),TriMesh::Face(1,0,5)};
 SheetSplitResult r;
 bool ok=split_nonmanifold_sheets(&m,r);
 printf("ok=%d nv=%zu\n",ok,m.vertices.size());
 if(ok){
  for(size_t f=0;f<m.faces.size();f++)printf("f%zu %d %d %d  ep %d %d %d\n",f,m.faces[f][0],m.faces[f][1],m.faces[f][2],r.edge_partner[f][0],r.edge_partner[f][1],r.edge_partner[f][2]);
  for(size_t i=0;i<r.vertex_source.size();i++)printf("vs%d=%d\n",(int)i,r.vertex_source[i]);
 }
}
