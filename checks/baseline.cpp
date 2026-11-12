#include "opennurbs.h"
#include <cmath>
#include <iostream>
int main(){
 ON::Begin();
 ON_NurbsSurface surface(3,false,3,3,3,3);
 for(int d=0;d<2;++d){surface.SetKnot(d,0,0);surface.SetKnot(d,1,0);surface.SetKnot(d,2,1);surface.SetKnot(d,3,1);}
 for(int i=0;i<3;++i)for(int j=0;j<3;++j)surface.SetCV(i,j,ON_3dPoint(i*.5,j*.5,.1*i*j));
 ON_3dPoint before=surface.PointAt(.25,.75);
 if(!surface.SetDomain(0,2,6))return 1;
 ON_3dPoint after=surface.PointAt(3,.75);
 double error=before.DistanceTo(after);
 if(!surface.IsValid()||!std::isfinite(error)||error>1e-12)return 2;
 ON_Brep* brep=new ON_Brep;
 ON_Surface* copy=surface.DuplicateSurface();
 if(!brep->Create(copy)) return 4;
 if(!brep||!brep->IsValid()||brep->m_F.Count()!=1)return 3;
 std::cout<<"OPENNURBS_BASELINE_OK faces="<<brep->m_F.Count()<<" affine_error="<<error<<"\n";
 delete brep; ON::End();
}
