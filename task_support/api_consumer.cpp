#include "SoundTouch.h"
#include <vector>
#include <cmath>
int main(){
 soundtouch::SoundTouch s;s.setSampleRate(48000);s.setChannels(2);s.prepareBounded();
 std::vector<float>x(32000),y(80000);for(unsigned i=0;i<16000;i++){x[2*i]=.1f*std::sin(.03*i);x[2*i+1]=-x[2*i];}
 unsigned used=0,got=0;double energy=0;bool done=false;
 for(unsigned k=0;k<50000&&!done;k++){
 auto r=s.processBounded(used<16000?x.data()+2*used:nullptr,16000-used,y.data(),40000,true);
 if(r.consumed>16000-used||r.produced>40000)return 2;used+=r.consumed;got+=r.produced;done=r.finished;
 for(unsigned j=0;j<r.produced;j++){energy+=y[2*j]*y[2*j];if(!std::isfinite(y[2*j])||std::abs(y[2*j]+y[2*j+1])>3e-5)return 3;}
 }
 if(!done||used!=16000||got<1000||energy<1)return 4;
 auto r=s.processBounded(nullptr,0,nullptr,0,true);if(r.consumed||r.produced||!r.finished)return 5;
 s.clearBounded();return 0;
}
