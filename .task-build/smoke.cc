#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cstdio>
#include <memory>
using namespace trimesh;
static int reciprocity(TriMesh&m){
 m.need_across_edge(); int bad=0;
 for(size_t f=0;f<m.faces.size();f++)for(int k=0;k<3;k++){
  int g=m.across_edge[f][k]; if(g<0)continue;
  int u=m.faces[f][NEXT_MOD3(k)],w=m.faces[f][PREV_MOD3(k)];
  // find g's corner whose opposite edge is (w,u)
  int kk=-1;
  for(int x=0;x<3;x++) if(m.faces[g][NEXT_MOD3(x)]==w&&m.faces[g][PREV_MOD3(x)]==u) kk=x;
  if(kk<0||m.across_edge[g][kk]!=(int)f)bad++;
 }
 return bad;
}
int main(){
 const char*names[]={"sphere","torus","ccyl","cube","knot"};
 for(int i=0;i<5;i++){
  std::unique_ptr<TriMesh> m;
  if(i==0)m.reset(make_sphere_polar(12,8));
  else if(i==1)m.reset(make_torus(16,12));
  else if(i==2)m.reset(make_ccyl(12,5));
  else if(i==3)m.reset(make_cube(3));
  else m.reset(make_knot(20,10));
  size_t nv0=m->vertices.size(), nf0=m->faces.size();
  SheetSplitResult r; bool ok=split_nonmanifold_sheets(m.get(),r);
  m->need_normals(); m->need_pointareas();
  int bad=ok?reciprocity(*m):-1;
  bool restored=true;
  for(size_t f=0;f<m->faces.size()&&ok;f++)for(int k=0;k<3;k++)
    if(r.vertex_source[m->faces[f][k]]!=(int()>=0?0:0)) {}
  printf("%s ok=%d nv=%zu->%zu nf=%zu->%zu bad=%d\n",names[i],ok,nv0,m->vertices.size(),nf0,m->faces.size(),bad);
 }
}
