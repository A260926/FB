#include "midi_fixture.hpp"
#define NEED(x) do{if(!(x)){std::fprintf(stderr,"baseline failed line %d\n",__LINE__);return 1;}}while(0)
int main(){
    ADL_MIDIPlayer *d=task_fixture::player();NEED(d);
    task_fixture::Bytes smf=task_fixture::smf(2);NEED(adl_openData(d,smf.data(),(unsigned long)smf.size())==0);
    double length=adl_totalTimeLength(d);std::vector<short> pcm;
    NEED(length>0 && task_fixture::render(d,8192,pcm) && task_fixture::nonzero(pcm));
    long long sum=0;for(size_t i=0;i<pcm.size();i++)sum+=pcm[i]<0?-pcm[i]:pcm[i];
    std::printf("ADL_BASELINE format=2 frames=8192 nonzero=1 absolute_sum=%lld duration=%.9f\n",sum,length);
    adl_positionSeek(d,0.3);NEED(task_fixture::render(d,2048,pcm));
    adl_positionRewind(d);NEED(task_fixture::render(d,4096,pcm) && task_fixture::nonzero(pcm));
    std::puts("ADL_TRANSPORT seek_frames=2048 rewind_frames=4096 nonzero=1");adl_close(d);return 0;
}
