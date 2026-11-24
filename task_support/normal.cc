#include "tiny_obj_loader.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
static std::string text(const char*p){std::ifstream f(p);std::ostringstream s;s<<f.rdbuf();return s.str();}
static bool valid(const tinyobj::attrib_t&a,const std::vector<tinyobj::shape_t>&ss,const std::vector<tinyobj::material_t>&ms,size_t&faces){faces=0;for(size_t i=0;i<a.vertices.size();i++)if(!std::isfinite(a.vertices[i]))return false;
 for(size_t s=0;s<ss.size();s++){const tinyobj::mesh_t&m=ss[s].mesh;if(m.material_ids.size()!=m.num_face_vertices.size()||m.smoothing_group_ids.size()!=m.num_face_vertices.size())return false;
  size_t off=0;for(size_t f=0;f<m.num_face_vertices.size();f++){if(m.num_face_vertices[f]!=3||m.material_ids[f]<0||size_t(m.material_ids[f])>=ms.size())return false;faces++;
   for(size_t k=0;k<3;k++){if(off>=m.indices.size())return false;const tinyobj::index_t&v=m.indices[off++];if(v.vertex_index<0||size_t(v.vertex_index)>=a.vertices.size()/3)return false;if(v.texcoord_index < -1||(v.texcoord_index>=0&&size_t(v.texcoord_index)>=a.texcoords.size()/2))return false;if(v.normal_index < -1||(v.normal_index>=0&&size_t(v.normal_index)>=a.normals.size()/3))return false;}}
  if(off!=m.indices.size())return false;
 }return faces>0;}
int main(int argc,char**argv){const std::string obj=text(argc>1?argv[1]:"task_support/normal.obj"),mtl=text(argc>2?argv[2]:"task_support/normal.mtl");if(obj.empty()||mtl.empty())return 1;
 tinyobj::ObjReader r;tinyobj::ObjReaderConfig c;c.triangulate=false;if(!r.ParseFromString(obj,mtl,c)){std::cerr<<r.Error();return 2;}size_t f1=0;if(!valid(r.GetAttrib(),r.GetShapes(),r.GetMaterials(),f1))return 3;
 std::istringstream oi(obj),mi(mtl);tinyobj::MaterialStreamReader mr(mi);tinyobj::attrib_t a;std::vector<tinyobj::shape_t>ss;std::vector<tinyobj::material_t>ms;std::string w,e;
 if(!tinyobj::LoadObj(&a,&ss,&ms,&w,&e,&oi,&mr,false))return 4;size_t f2=0;if(!valid(a,ss,ms,f2)||f1!=f2)return 5;
 if(argc==1){if(ss.size()!=2||f1!=2||ss[0].name!="anchor"||ss[1].name!="later"||ss[0].mesh.smoothing_group_ids[0]!=7||ss[1].mesh.smoothing_group_ids[0]!=0)return 6;
  if(ss[0].mesh.indices[0].vertex_index!=1||ss[0].mesh.indices[0].texcoord_index!=0||ss[0].mesh.indices[0].normal_index!=0||ss[1].mesh.indices[0].texcoord_index!=-1||ss[1].mesh.indices[0].normal_index!=-1)return 7;
  if(a.vertices[3]!=0||a.vertices[6]!=1||ms[ss[0].mesh.material_ids[0]].name!="red"||ms[ss[1].mesh.material_ids[0]].name!="blue")return 8;
 }
 std::cout<<"OBJ_NORMAL_OK faces="<<f1<<" stream_faces="<<f2<<" materials="<<ms.size()<<"\n";return 0;}
