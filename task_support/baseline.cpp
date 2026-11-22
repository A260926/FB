#include "modplug.h"
#include "mod_fixture.h"
#include <cstdio>
#include <cstdint>
#include <vector>
int main() {
    ModPlug_Settings s; ModPlug_GetSettings(&s);
    s.mFrequency=44100; s.mChannels=2; s.mBits=16;
    s.mFlags=MODPLUG_ENABLE_OVERSAMPLING; s.mLoopCount=0;
    ModPlug_SetSettings(&s);
    std::vector<unsigned char> mod=make_mod();
    ModPlugFile* f=ModPlug_Load(mod.data(),static_cast<int>(mod.size()));
    if(!f) return 10;
    std::vector<short> pcm(22050*2);
    int n=ModPlug_Read(f,pcm.data(),static_cast<int>(pcm.size()*sizeof(short)));
    uint64_t sum=0;
    for(short v:pcm) sum+=v<0?-static_cast<int>(v):v;
    int row=ModPlug_GetCurrentRow(f);
    std::printf("BASELINE decoded_bytes=%d row=%d abs_sum=%llu\n",n,row,(unsigned long long)sum);
    ModPlug_Unload(f);
    if(n!=88200 || !sum || row<3 || row>5) return 11;
    std::puts("BASELINE_OK real_mod_pcm"); return 0;
}
