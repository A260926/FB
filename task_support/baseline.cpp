#include "src.cmd/Simplify.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace Simplify;
static void make_mesh() {
 vertices.clear();triangles.clear();refs.clear();materials.clear();mtllib.clear();
 const int n=8,side_count=(n+1)*(n+1);
 for(int side=0;side<2;++side)for(int j=0;j<=n;++j)for(int i=0;i<=n;++i){
  Vertex v={};v.p=vec3f((side?-1.:1.)*(1.+double(i)/n),double(j)/n,0);vertices.push_back(v);
 }
 for(int side=0;side<2;++side)for(int j=0;j<n;++j)for(int i=0;i<n;++i){
  int a=side*side_count+j*(n+1)+i,b=a+1,c=a+n+1,d=c+1;
  int faces[2][3]={{a,b,d},{a,d,c}};
  for(int k=0;k<2;++k){Triangle t={};t.attr=TEXCOORD;t.material=-1;
   for(int q=0;q<3;++q){t.v[q]=faces[k][side?2-q:q];const vec3f& p=vertices[t.v[q]].p;t.uvs[q]=vec3f(std::fabs(p.x)+side*3,p.y,0);}
   triangles.push_back(t);
  }
 }
}
static void valid(){
 assert(!triangles.empty());
 for(size_t i=0;i<triangles.size();++i){const Triangle& t=triangles[i];
  for(int j=0;j<3;++j){assert(t.v[j]>=0&&size_t(t.v[j])<vertices.size());assert(std::isfinite(t.uvs[j].x)&&std::isfinite(t.uvs[j].y));}
  vec3f cross;cross.cross(vertices[t.v[1]].p-vertices[t.v[0]].p,vertices[t.v[2]].p-vertices[t.v[0]].p);assert(cross.dot(cross)>0);
 }
}
int main(){
 make_mesh();size_t before=triangles.size();simplify_mesh(32);valid();assert(triangles.size()<before);
 size_t after=triangles.size();write_obj(".task-build/roundtrip.obj");load_obj(".task-build/roundtrip.obj",true);valid();assert(triangles.size()==after);
 std::printf("FQ_BASELINE faces_before=%zu faces_after=%zu vertices_after=%zu uv_roundtrip=ok\n",before,after,vertices.size());
 make_mesh();simplify_mesh_lossless();valid();assert(triangles.size()<before);
 std::printf("FQ_LOSSLESS faces_after=%zu\n",triangles.size());return 0;
}
