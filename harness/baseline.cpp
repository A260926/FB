#include "QuickHull.hpp"
#include <cstdio>
#include <cmath>
#include <cstring>
using V=quickhull::Vector3<double>;
static std::vector<V> points(){return {
{-.63644445627244906,.68846768444896989,.77772965710215303},
{.61364265394764972,-.17955349395657641,-.79401228965430826},
{-.56099800177033965,.25951522662099591,.71984816390977391},
{-.91745487211373944,.48513244229633506,.14083471850494145},
{-.93137361301852306,-.56897355808071304,-.024861964838954753},
{-.45458828580918786,-.76187313462060358,-.40073183849569732},
{-.24429187295794086,-.99775220161164746,-.044736384275497094},
{.049342598096238983,-.90708559127708555,-.34982095854883044},
{-.19079979349929632,-.40648696518321337,.33026712813922515},
{.035240375928227596,.51679849006144196,-.64696790683440997},
{.024984826892043403,.58737329944566097,.22723194657265736},
{-.87322206139455572,-.28755736232502449,.083104115053526564}};}
int main(int argc,char**argv){
 quickhull::QuickHull<double> q;auto p=points();auto h=q.getConvexHull(p,true,true,1e-9);
 size_t normal=h.getIndexBuffer().size()/3;if(normal!=18)return 1;
 if(argc>1&&!strcmp(argv[1],"--control-only")){puts("QUICKHULL_CONTROL_OK faces=18");return 0;}
 for(auto&v:p)v=v*1e-5;auto small=q.getConvexHull(p,true,true,1e-9);
 printf("scale witness unit_faces=%zu small_faces=%zu\n",normal,small.getIndexBuffer().size()/3);
 p.clear();for(int i=0;i<8;i++)p.push_back({1e6+.001*(i&1),1e6+.001*((i>>1)&1),1e6+.001*((i>>2)&1)});
 auto cube=q.getConvexHull(p,true,true,1e-9);auto idx=cube.getIndexBuffer();double volume=0;
 for(size_t i=0;i<idx.size();i+=3){V a=p[idx[i]]-p[0],b=p[idx[i+1]]-p[0],c=p[idx[i+2]]-p[0];volume+=a.dotProduct(b.crossProduct(c))/6;}
 printf("translation witness faces=%zu volume_ratio=%.6f\n",idx.size()/3,std::abs(volume)/1e-9);
 // After the scale/translation fix both witnesses must describe the same genuine
 // 3D convex hull as the untransformed input: the small cloud keeps 18 faces and
 // the far, tiny cube is a closed 12-triangle solid with its full volume.
 if(small.getIndexBuffer().size()/3!=18||idx.size()/3!=12||std::abs(std::abs(volume)/1e-9-1.0)>1e-4)return 2;
 puts("QUICKHULL_BASELINE_OK scale-and-translation-invariant");
}
