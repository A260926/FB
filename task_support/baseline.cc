#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cstdio>
#include <cmath>
using namespace trimesh;
int main(){
 TriMesh::set_verbose(0); TriMesh m;
 m.vertices={point(0,0,0),point(1,0,0),point(0,1,0),point(0,0,1)};
 m.faces={TriMesh::Face(0,1,2),TriMesh::Face(1,0,3)};
 m.need_normals(); m.need_pointareas(); m.need_across_edge();
 std::vector<vec> f(m.vertices.size());
 vec axis(0.3f,0.7f,0.4f);
 for(size_t i=0;i<f.size();++i) f[i]=axis-m.normals[i]*(axis DOT m.normals[i]);
 diffuse_vector(&m,f,2.0f);
 double leakage=0; for(size_t i=0;i<f.size();++i) leakage=std::max(leakage,std::fabs(double(f[i] DOT m.normals[i])));
 printf("FOLDED_MESH faces=%zu shared=%d areas=%.6f\n",m.faces.size(),m.across_edge[0][2],m.pointareas[0]);
 printf("WORLD_VECTOR_DIFFUSION normal_component=%.6f\n",leakage);
 if(!std::isfinite(leakage)||leakage<0.01||m.across_edge[0][2]!=1) return 1;
 puts("BASELINE_OK existing_world_vector_semantics"); return 0;
}
