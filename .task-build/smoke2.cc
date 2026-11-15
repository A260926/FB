#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cstdio>
#include <memory>
using namespace trimesh;
int main(){
 std::unique_ptr<TriMesh> m(make_cube(3));
 m->need_across_edge();
 printf("before: ae0=%d,%d,%d f0=%d %d %d\n",m->across_edge[0][0],m->across_edge[0][1],m->across_edge[0][2],m->faces[0][0],m->faces[0][1],m->faces[0][2]);
 // what would ae of f1,k be for f0,k?
 for(int f=0;f<3;f++)for(int k=0;k<3;k++){
  int g=m->across_edge[f][k]; if(g<0)continue;
  int v=m->faces[f][k];
  int kk=-1; for(int x=0;x<3;x++) if(m->faces[g][x]==v) kk=x;
  printf("f%d k%d partner g=%d shares vertex %d at g's corner %d\n",f,k,g,v,kk);
 }
}
