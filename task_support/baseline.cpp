#include "mesh.hpp"
#include <vcg/complex/algorithms/refine.h>
#include <wrap/io_trimesh/export_ply.h>
#include <wrap/io_trimesh/import_ply.h>
#include <cmath>
#include <cstdio>
static int check(bool b,const char*s){if(!b)std::fprintf(stderr,"BASE_FAIL %s\n",s);return b?0:1;}
int main(){
 UMesh m; vcg::tri::Allocator<UMesh>::AddVertices(m,4); vcg::tri::Allocator<UMesh>::AddFaces(m,2);
 double p[4][2]={{0,0},{2,0},{2,1},{0,1}}; int f[2][3]={{0,1,2},{0,2,3}};
 for(int i=0;i<4;++i){m.vert[i].P()=vcg::Point3d(p[i][0],p[i][1],0);m.vert[i].Q()=p[i][0]+2*p[i][1];m.vert[i].C()=vcg::Color4b(17,61,123,255);m.vert[i].N()=vcg::Point3d(0,0,1);}
 m.textures={"surface.png","unused.png"};
 for(int i=0;i<2;++i)for(int k=0;k<3;++k){int v=f[i][k];m.face[i].V(k)=&m.vert[v];m.face[i].WT(k).P()=vcg::Point2f(float(p[v][0]+.2),float(p[v][1]+.2));m.face[i].WT(k).N()=0;}
 vcg::tri::UpdateTopology<UMesh>::FaceFace(m);vcg::tri::UpdateFlags<UMesh>::FaceBorderFromFF(m);
 vcg::tri::Refine<UMesh,vcg::tri::MidPoint<UMesh>>(m,vcg::tri::MidPoint<UMesh>(&m),0.0);
 if(check(m.vn==9&&m.fn==8,"shared midpoint refinement"))return 1;
 for(const auto&v:m.vert)if(check(std::abs(v.Q()-v.P()[0]-2*v.P()[1])<1e-9,"quality transfer"))return 1;
 vcg::tri::UpdateNormal<UMesh>::PerFaceNormalized(m);vcg::tri::UpdateNormal<UMesh>::PerVertexNormalized(m);
 int mask=vcg::tri::io::Mask::IOM_WEDGTEXCOORD|vcg::tri::io::Mask::IOM_VERTNORMAL|vcg::tri::io::Mask::IOM_VERTQUALITY|vcg::tri::io::Mask::IOM_VERTCOLOR;
 for(int binary=0;binary<2;++binary){const char*name=binary?".task-build/base-binary.ply":".task-build/base-ascii.ply";
 if(check(vcg::tri::io::ExporterPLY<UMesh>::Save(m,name,mask,bool(binary))==0,"write PLY"))return 1;
 UMesh n;int loaded=0;if(check(vcg::tri::io::ImporterPLY<UMesh>::Open(n,name,loaded)==0,"read PLY"))return 1;
 if(check(n.vn==m.vn&&n.fn==m.fn&&n.textures.size()==2,"PLY topology and texture names"))return 1;
 for(int i=0;i<n.fn;++i)for(int k=0;k<3;++k){if(check((n.face[i].WT(k).P()-m.face[i].WT(k).P()).Norm()<1e-6&&n.face[i].WT(k).N()==0,"wedge roundtrip"))return 1;}
 }
 std::puts("VCG_BASELINE_OK vertices=9 faces=8 shared_midpoints=5 ply_modes=2 uv_outside_unit=present");return 0;
}
