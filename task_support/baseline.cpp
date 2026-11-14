#include "tiny_obj_loader.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"baseline failed %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
 tinyobj::ObjReader r; CHECK(r.ParseFromFile("task_support/seams.obj"));
 const auto&a=r.GetAttrib();const auto&s=r.GetShapes();
 CHECK(a.vertices.size()==15&&s.size()==2);
 CHECK(s[0].mesh.indices.size()==3&&s[1].mesh.indices.size()==6);
 auto x=s[0].mesh.indices[1],y=s[1].mesh.indices[0];
 CHECK(x.vertex_index==1&&y.vertex_index==1&&x.texcoord_index==1&&y.texcoord_index==3);
 CHECK(x.normal_index==0&&y.normal_index==0);
 CHECK(a.skin_weights.size()==2&&a.skin_weights[0].vertex_id==4&&a.skin_weights[1].vertex_id==0);
 CHECK(a.skin_weights[0].weightValues.size()==2&&a.skin_weights[0].weightValues[0].joint_id==2);
 CHECK(std::abs(a.skin_weights[0].weightValues[0].weight-.8)<1e-12);
 std::ifstream f("task_support/seams.obj");std::ostringstream text;text<<f.rdbuf();tinyobj::ObjReader v;
 CHECK(v.ParseFromString(text.str(),"")); CHECK(v.GetAttrib().vertices==a.vertices);
 std::istringstream input(text.str());tinyobj::attrib_t b;std::vector<tinyobj::shape_t> t;std::vector<tinyobj::material_t> m;std::string warn,err;
 CHECK(tinyobj::LoadObj(&b,&t,&m,&warn,&err,&input));CHECK(t.size()==2&&b.skin_weights.size()==2);
 CHECK(t[1].mesh.indices[0].vertex_index==1&&t[1].mesh.indices[0].texcoord_index==3);
 std::puts("TINYOBJ_WEIGHT_SOURCE_OK positions=5 shapes=2 corners=9 seam_vertex=1 vw_order=4,0");
}
