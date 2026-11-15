#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cstdio>
#include <memory>
using namespace trimesh;
int main(){
 // butterfly: two triangles sharing vertex 0, edges (0,1),(0,2) and (0,3),(0,4)
 TriMesh m;
 m.vertices={point(0,0,0),point(1,0,0),point(0,1,0),point(-1,0,0),point(0,-1,0)};
 m.faces={TriMesh::Face(0,1,2),TriMesh::Face(0,3,4)};
 SheetSplitResult r;
 bool ok=split_nonmanifold_sheets(&m,r);
 printf("ok=%d nv=%zu nf=%zu\n",ok,m.vertices.size(),m.faces.size());
 for(size_t i=0;i<r.vertex_source.size();++i)printf("vs[%zu]=%d\n",i,r.vertex_source[i]);
 for(size_t i=0;i<r.edge_partner.size();++i)printf("ep[%zu]=%d %d %d\n",i,r.edge_partner[i][0],r.edge_partner[i][1],r.edge_partner[i][2]);
 for(size_t i=0;i<m.faces.size();++i)printf("f%zu=%d %d %d\n",i,m.faces[i][0],m.faces[i][1],m.faces[i][2]);
 m.need_normals();m.need_across_edge();
 for(size_t i=0;i<m.across_edge.size();++i)printf("ae[%zu]=%d %d %d\n",i,m.across_edge[i][0],m.across_edge[i][1],m.across_edge[i][2]);
 printf("nv0 normal %f %f %f\n",m.normals[0][0],m.normals[0][1],m.normals[0][2]);
 for(size_t i=0;i<m.vertices.size();++i)printf("n%zu %f %f %f\n",i,m.normals[i][0],m.normals[i][1],m.normals[i][2]);
}
