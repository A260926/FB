#include "opennurbs.h"
#include <cmath>
#include <cstdio>
#include <memory>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"baseline failure line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
  ON::Begin();
  ON_NurbsSurface s(3,true,3,2,3,2);
  const double w[3]={1,std::sqrt(0.5),1};
  const double xy[3][2]={{1,0},{1,1},{0,1}};
  for(int i=0;i<3;++i) for(int j=0;j<2;++j)
    REQUIRE(s.SetCV(i,j,ON_4dPoint(xy[i][0]*w[i],xy[i][1]*w[i],j*2*w[i],w[i])));
  REQUIRE(s.SetKnot(0,0,2)); REQUIRE(s.SetKnot(0,1,2));
  REQUIRE(s.SetKnot(0,2,6)); REQUIRE(s.SetKnot(0,3,6));
  REQUIRE(s.SetKnot(1,0,-3)); REQUIRE(s.SetKnot(1,1,5));
  REQUIRE(s.IsValid());
  ON_Surface *west=nullptr,*east=nullptr;
  REQUIRE(s.Split(0,3.7,west,east));
  std::unique_ptr<ON_Surface> a(west),b(east);
  double error=0;
  for(int i=0;i<=12;++i) for(int j=0;j<=6;++j) {
    double u=2+4.0*i/12,v=-3+8.0*j/6;
    error=std::fmax(error,s.PointAt(u,v).DistanceTo((u<=3.7?a:b)->PointAt(u,v)));
  }
  REQUIRE(error<1e-12);
  std::printf("RATIONAL_SPLIT samples=91 max_error=%.3g domains=[2,3.7],[3.7,6]\n",error);
  ON_3dPoint corners[8]={ON_3dPoint(0,0,0),ON_3dPoint(2,0,0),ON_3dPoint(2,3,0),ON_3dPoint(0,3,0),ON_3dPoint(0,0,4),ON_3dPoint(2,0,4),ON_3dPoint(2,3,4),ON_3dPoint(0,3,4)};
  std::unique_ptr<ON_Brep> box(ON_BrepBox(corners));
  REQUIRE(box && box->IsValid());
  REQUIRE(box->m_F.Count()==6 && box->m_E.Count()==12);
  int mates=0;
  for(int e=0;e<box->m_E.Count();++e) {
    const ON_BrepEdge& edge=box->m_E[e]; REQUIRE(edge.m_ti.Count()==2);
    for(int k=0;k<2;++k) {
      const ON_BrepTrim& t=box->m_T[edge.m_ti[k]];
      const ON_BrepFace* f=t.Face(); REQUIRE(f);
      for(int j=0;j<=4;++j) {
        double n=j/4.0;
        ON_3dPoint uv=t.PointAt(t.Domain().ParameterAt(n));
        ON_3dPoint ep=edge.PointAt(edge.Domain().ParameterAt(t.m_bRev3d?1-n:n));
        REQUIRE(f->PointAt(uv.x,uv.y).DistanceTo(ep)<1e-12);
      }
    }
    ++mates;
  }
  ONX_Model model;
  REQUIRE(!model.AddModelGeometryComponent(box.get(),nullptr).IsEmpty());
  REQUIRE(model.Write(".task-build/baseline.3dm",0,nullptr));
  ONX_Model read;
  REQUIRE(read.Read(".task-build/baseline.3dm",nullptr));
  REQUIRE(read.ActiveComponentCount(ON_ModelComponent::Type::ModelGeometry)==1);
  std::printf("BREP_SHARED edges=%d faces=6 trim_samples=120 archive_objects=1\n",mates);
  std::puts("BASELINE_COMPLETE");
  return 0;
}
