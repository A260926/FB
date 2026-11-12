#define TINYOBJLOADER_IMPLEMENTATION
#define TINYOBJLOADER_USE_DOUBLE
#include "tiny_obj_loader.h"
#include <cstdio>
int main(){
 const char *obj="v 1 0 0 1\nv 1 1 0 0.7071067811865476\nv 0 1 0 1\n"
 "f 1 2 3\ncstype rat bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
 tinyobj::ObjReader r;
 if(!r.ParseFromString(obj,""))return 1;
 const auto&a=r.GetAttrib();const auto&s=r.GetShapes();
 size_t faces=0,lines=0;for(auto&x:s){faces+=x.mesh.num_face_vertices.size();lines+=x.lines.num_line_vertices.size();}
 if(a.vertices.size()!=9||a.vertex_weights.size()!=3||faces!=1||lines!=0)return 2;
 printf("mesh_faces=%zu line_strips=%zu weights=%.6f,%.6f,%.6f\n",faces,lines,a.vertex_weights[0],a.vertex_weights[1],a.vertex_weights[2]);
 puts("TINYOBJ_BASELINE_OK weighted-vertices-and-face");
}
