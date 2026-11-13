#include <libaaf/AAFIface.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    AAF_Iface* aafi = aafi_alloc(NULL);
    if (!aafi) return 1;
    if (aafi_load_file(aafi,"test/aaf/DR_Audio_Levels.aaf")) return 2;
    unsigned tracks=0, clips=0, gains=0, automation=0;
    for (aafiAudioTrack* t=aafi->Audio->Tracks;t;t=t->next) {
        ++tracks;
        for (aafiTimelineItem* it=t->timelineItems;it;it=it->next) {
            aafiAudioClip* c=aafi_timelineItemToAudioClip(it);
            if (!c) continue;
            ++clips;
            if (c->gain) { ++gains; if (!c->gain->pts_cnt || !c->gain->value) return 3; }
            if (c->automation) { ++automation; if (!c->automation->pts_cnt || !c->automation->time || !c->automation->value) return 4; }
        }
    }
    printf("LIBAAF_BASELINE tracks=%u clips=%u gains=%u automation=%u\n",tracks,clips,gains,automation);
    if (!tracks || !clips || !gains || !automation) return 5;
    aafi_release(&aafi);
    if (aafi) return 6;
    puts("LIBAAF_BASELINE import_and_release=ok");
    return 0;
}
