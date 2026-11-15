#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cstdio>
using namespace trimesh;
int main(){
 TriMesh m;
 m.vertices={point(0,0,0),point(1,0,0),point(0.13f,1.07f,0.21f),
   point(-0.31f,0.22f,1.13f),point(0.47f,-0.63f,0.58f)};
 m.faces={TriMesh::Face(0,1,2),TriMesh::Face(0,1,3),TriMesh::Face(0,4,1),TriMesh::Face(0,3,4),TriMesh::Face(1,4,3)};
 SheetSplitResult r;
 bool ok=split_nonmanifold_sheets(&m,r);
 printf("ok=%d\n",ok);
 if(ok){
  for(size_t i=0;i<r.vertex_source.size();++i)printf("vs[%zu]=%d\n",i,r.vertex_source[i]);
  for(size_t i=0;i<m.faces.size();++i)printf("f%zu=%d %d %d\n",i,m.faces[i][0],m.faces[i][1],m.faces[i][2]);
  for(size_t i=0;i<r.edge_partner.size();++i)printf("ep[%zu]=%d %d %d\n",i,r.edge_partner[i][0],r.edge_partner[i][1],r.edge_partner[i][2]);
 }
}
