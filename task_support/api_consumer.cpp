#include "modplug.h"
#include "mod_fixture.h"
#include <cstdio>
#include <vector>
int main() {
    ModPlug_Settings s; ModPlug_GetSettings(&s);
    s.mFrequency=44100; s.mChannels=2; s.mBits=16; s.mFlags=0;
    s.mLoopCount=0; ModPlug_SetSettings(&s);
    auto mod=make_mod(); ModPlugFile* f=ModPlug_Load(mod.data(),(int)mod.size());
    if(!f) return 20;
    if(ModPlug_ChangeRate(f,22050)!=1) return 21;
    ModPlug_GetSettings(&s); if(s.mFrequency!=22050) return 22;
    std::vector<short> pcm(22050*2);
    if(ModPlug_Read(f,pcm.data(),(int)pcm.size()*2)!=(int)pcm.size()*2) return 23;
    long long power=0; for(short v:pcm) power+=v<0?-(int)v:v;
    int row=ModPlug_GetCurrentRow(f); ModPlug_Unload(f);
    if(!power || row<7 || row>9) return 24;
    std::puts("API_RATE_OK production_pcm_at_22050"); return 0;
}
