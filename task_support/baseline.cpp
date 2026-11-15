#include "sisl.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstdlib>
int main(){
 double k[]={0,0,1,1};double a[]={0,0,0,1,0,0,0,1,0,1,1,0};double b[]={0,.5,-.5,1,.5,-.5,0,.5,.5,1,.5,.5};
 SISLSurf* s1=newSurf(2,2,2,2,k,k,a,1,3,1);SISLSurf*s2=newSurf(2,2,2,2,k,k,b,1,3,1);
 if(!s1||!s2)return 2;
 int np=0,nc=0,status=0;double*uv1=nullptr,*uv2=nullptr;SISLIntcurve**curves=nullptr;
 s1859(s1,s2,1e-14,1e-9,&np,&uv1,&uv2,&nc,&curves,&status);
 std::cout<<"s1859_status="<<status<<" isolated="<<np<<" branches="<<nc<<"\n";
 bool ok=status==0&&nc==1&&np==0;double maxgap=0;int observations=0;
 for(int i=0;i<nc&&ok;++i){
  s1310(s1,s2,curves[i],1e-9,0,2,0,&status);
  auto c=curves[i];std::cout<<"s1310_status="<<status<<" paired="<<(c->pgeom&&c->ppar1&&c->ppar2)<<"\n";
  if(status!=0||!c->pgeom||!c->ppar1||!c->ppar2){ok=false;break;}
  for(int j=0;j<=16;++j){double t=c->pgeom->et[c->pgeom->ik-1]+(c->pgeom->et[c->pgeom->in]-c->pgeom->et[c->pgeom->ik-1])*j/16.;double p[3],u[2],v[2],x[3],y[3],n[3];int left=0,lu=0,lv=0;
   s1221(c->pgeom,0,t,&left,p,&status);if(status<0){ok=false;break;}
   left=0;s1221(c->ppar1,0,t,&left,u,&status);if(status<0){ok=false;break;}
   left=0;s1221(c->ppar2,0,t,&left,v,&status);if(status<0){ok=false;break;}
   s1421(s1,0,u,&lu,&lv,x,n,&status);if(status<0){ok=false;break;}
   lu=lv=0;s1421(s2,0,v,&lu,&lv,y,n,&status);if(status<0){ok=false;break;}
   for(int d=0;d<3;++d){maxgap=std::max(maxgap,std::fabs(x[d]-p[d]));maxgap=std::max(maxgap,std::fabs(y[d]-p[d]));}
   ++observations;
  }
 }
 if(curves)freeIntcrvlist(curves,nc);std::free(uv1);std::free(uv2);freeSurf(s1);freeSurf(s2);
 if(!ok||observations!=17||!std::isfinite(maxgap)||maxgap>1e-8)return 3;
 std::cout<<"paired_samples="<<observations<<" max_surface_gap="<<maxgap<<"\nSISL_PAIR_BASELINE_OK\n";
}
