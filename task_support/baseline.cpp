#include "opennurbs_public.h"
#include <cmath>
#include <cstdio>
#include <memory>
int main(){
 ON::Begin();
 ON_Cylinder cylinder(ON_Circle(ON_Plane::World_xy,2.0),3.0);
 std::unique_ptr<ON_Brep> brep(ON_BrepCylinder(cylinder,true,true));
 if(!brep || !brep->IsValid()) return 1;
 int face=-1;
 ON_NurbsSurface surface;
 for(int i=0;i<brep->m_F.Count();++i){ON_NurbsSurface s;if(brep->m_F[i].GetNurbForm(s)>0&&s.IsClosed(0)){face=i;surface=s;break;}}
 if(face<0)return 2;
 auto domain=surface.Domain(0);auto vdom=surface.Domain(1);
 const double seam=domain.ParameterAt(0.37),v=vdom.ParameterAt(0.41),period=domain.Length();
 ON_NurbsSurface moved(surface);
 if(!moved.ChangeSurfaceSeam(0,seam))return 3;
 double maxError=0;
 for(int i=0;i<19;++i){double u=seam+period*(i+0.25)/19;double old=u;if(old>domain[1])old-=period;maxError=std::fmax(maxError,moved.PointAt(u,v).DistanceTo(surface.PointAt(old,v)));}
 if(maxError>1e-10 || std::fabs(moved.Domain(0)[0]-seam)>1e-12)return 4;
 int pairs=0;for(int i=0;i<brep->m_E.Count();++i){int seams=0;const auto&e=brep->m_E[i];for(int j=0;j<e.m_ti.Count();++j)if(brep->m_T[e.m_ti[j]].m_type==ON_BrepTrim::seam)++seams;if(seams==2)++pairs;}
 if(pairs!=1)return 5;
 // Surface-only edits leave the source BRep topology and domains as they were.
 if(brep->m_F[face].Domain(0)!=domain || !brep->IsValid())return 6;
 std::printf("BASELINE surface_seam_exact max_error=%.3g seam_pairs=%d faces=%d\n",maxError,pairs,brep->m_F.Count());
 std::puts("BASELINE original_brep_unchanged valid=1");
 return 0;
}
