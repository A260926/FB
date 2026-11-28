#include "wavefront.h"
#include <vector>
#include <cmath>
#include <cstdio>
using namespace tinybvh;
int main(){
 bvhvec4 v[]={bvhvec4(-1,-1,2,0),bvhvec4(1,-1,2,0),bvhvec4(0,1,2,0)};BVH b;b.settings.useSIMDifavailable=false;b.Build(v,1);
 std::vector<Ray> rays={Ray(bvhvec3(0,0,0),bvhvec3(0,0,2),3),Ray(bvhvec3(0,0,0),bvhvec3(0,0,2),3),Ray(bvhvec3(3,0,0),bvhvec3(0,0,2),3)};
 WaveBatch batch(b,{rays[0],rays[1]},{WaveQuery::Nearest,WaveQuery::Any});
 auto initial=batch.Stats();if(initial.nodeLoads||initial.boxTests||initial.triangleTests)return 10;
 size_t work=batch.Step(1);if(work!=1)return 15;auto first=batch.Append({rays[2]},{WaveQuery::Nearest});if(first!=2)return 16;for(size_t i=0;!batch.Done()&&i<1000;i++){size_t n=batch.Step(1);if(n>1)return 11;work+=n;}
 if(!batch.Done())return 12;auto st=batch.Stats();if(work!=st.nodeLoads+st.boxTests+st.triangleTests||work==0)return 13;
 const auto&r=batch.Results();if(r.size()!=3||r[0].state!=WaveState::Hit||std::abs(r[0].ray.hit.t-1)>1e-5||!r[1].occluded||r[2].state!=WaveState::Miss)return 14;
 auto before=batch.Stats();auto next=batch.Append({rays[0]},{WaveQuery::Nearest});if(next!=3||batch.Done()||batch.Stats().nodeLoads!=before.nodeLoads)return 17;for(size_t i=0;!batch.Done()&&i<1000;i++)batch.Step(1);if(!batch.Done()||batch.Results().size()!=4||batch.Results()[3].state!=WaveState::Hit||std::abs(batch.Results()[3].ray.hit.t-1)>1e-5||batch.Stats().nodeLoads!=before.nodeLoads)return 18;std::puts("API_PROBE wave_online_normal");return 0;
}
