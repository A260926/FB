/* Fixed production call binding; does not implement layered scheduling. */
#include "midi_fixture.hpp"
#include <adlmidi.h>
#define NEED(x) do{if(!(x))return __LINE__;}while(0)
int main(){
    ADL_MIDIPlayer*d=task_fixture::player();NEED(d);NEED(adl_setSMF2Layered(d,1)==0);
    task_fixture::Bytes midi=task_fixture::smf(2);std::vector<short> pcm;
    NEED(adl_openData(d,midi.data(),(unsigned long)midi.size())==0);
    NEED(task_fixture::render(d,4096,pcm) && task_fixture::nonzero(pcm));adl_close(d);
    std::puts("SMF2_API_BOUND frames=4096 nonzero=1");return 0;
}
