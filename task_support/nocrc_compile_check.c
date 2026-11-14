#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_CRC
#include "dr_flac.h"
#include "flac_gen.h"
#include <stdio.h>

/* In a no-CRC build the preserve mode must be unavailable and the object must stay usable. */
int main(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[4];
    unsigned long long coords[4];
    int blockSizes[2] = {16, 16};
    drflac_int32 out[32];
    unsigned int size = flac_gen_build(&bw, 0, 2, blockSizes, coords, 2, 32, spans);
    drflac* d = drflac_open_memory(bw.data, size, NULL);
    if (d == NULL) { printf("open failed\n"); return 1; }
    if (drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) != DRFLAC_FALSE) { printf("no-CRC enable should fail\n"); return 2; }
    if (d->timelinePreserveEnabled != 0) { printf("object mutated on rejection\n"); return 3; }
    if (drflac_read_pcm_frames_s32(d, 32, out) != 32) { printf("normal read broken\n"); return 4; }
    drflac_close(d);
    printf("DRFLAC_NO_CRC ok\n");
    return 0;
}
