#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
using namespace trimesh;
static bool ck(bool b,const char*s){if(!b)std::fprintf(stderr,"BASE_FAIL %s\n",s);return b;}
int main(){
 TriMesh m; m.vertices={point(0,0,0),point(1,0,0),point(1,1,0),point(0,1,0)};m.faces={TriMesh::Face(0,1,2),TriMesh::Face(0,2,3)};
 for(int i=0;i<4;++i){m.colors.push_back(Color(.125f*i,.25f,.5f));m.confidences.push_back(.1f*(i+1));}
 m.need_across_edge();if(!ck(m.across_edge[0][1]==1&&m.across_edge[1][2]==0,"manifold neighbor"))return 1;
 m.need_normals();m.need_pointareas();float area=0;for(float a:m.pointareas)area+=a;
 if(!ck(std::abs(area-1)<1e-6&&m.normals[0][2]>.999f,"area and normals"))return 1;
 for(int mode=0;mode<2;++mode){std::string name=mode?".task-build/base-binary.ply":".task-build/base-ascii.ply";
 std::string spec=std::string("norm:cflt:")+(mode?"ply_binary:":"ply_ascii:")+name;
 if(!ck(m.write(spec.c_str()),"write PLY"))return 1;
 std::unique_ptr<TriMesh> n(TriMesh::read(name.c_str()));if(!ck(n&&n->vertices.size()==4&&n->faces.size()==2&&n->normals.size()==4,"read PLY"))return 1;
 for(int i=0;i<4;++i)if(!ck(len(n->colors[i]-m.colors[i])<1e-6&&std::abs(n->confidences[i]-m.confidences[i])<1e-6,"PLY colors and confidence"))return 1;
 }
 TriMesh cross;cross.vertices={point(0,0,0),point(1,0,0),point(0,1,0),point(0,-1,0),point(0,0,1),point(0,0,-1)};
 cross.faces={TriMesh::Face(0,1,2),TriMesh::Face(1,0,3),TriMesh::Face(0,1,4),TriMesh::Face(1,0,5)};
 cross.need_across_edge();int nonreciprocal=0;for(int i=0;i<4;++i){int j=cross.across_edge[i][2];if(j>=0&&cross.across_edge[j][2]!=i)++nonreciprocal;}
 if(!ck(nonreciprocal==2,"existing nonmanifold first-match witness"))return 1;
 if(!ck(cross.write("ply_ascii:.task-build/nonmanifold.ply"),"write crossing fixture"))return 1;
 int status=std::system("sh -c '.task-build/mesh_check .task-build/nonmanifold.ply .task-build/default-check.ply > .task-build/default-check.log 2>&1; result=$?; test \"$result\" -eq 2'");
 if(!ck(status==0,"legacy mesh_check reports deletion with exit 2"))return 1;
 std::puts("TRIMESH_BASELINE_OK manifold_faces=2 ply_modes=2 nonreciprocal=2 legacy_check_exit=2");return 0;
}
