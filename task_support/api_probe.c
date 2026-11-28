/* Fixed normal-path consumer. No mixed production implementation is supplied. */
#include "task_support/fixture.h"
int main(void) {
    task_fixture f;
    mrw_mem_req sr,orr;
    mrw_dispatch d;
    mrw_mixed_item item;
    mrw_mixed_stats stats;
    if(task_fixture_init(&f)) return 10;
    memset(&item,0,sizeof item);
    item.skel=&f.skel;item.clip=&f.a;item.time=.5f;
    if(mrw_batch_mixed_to_palette_requirements(&item,1,MRW_PALETTE_F32,&sr,&orr)!=MRW_OK) return 11;
    if(orr.size!=24*sizeof(float)) return 12;
    float *out=mrw_authoring_alloc(orr.size);void *scratch=mrw_authoring_alloc(sr.size);
    if(!out || !scratch) return 13;
    if(mrw_dispatch_scalar(&d)!=MRW_OK) return 14;
    if(mrw_batch_mixed_to_palette(&d,&item,1,MRW_PALETTE_F32,out,orr.size,scratch,sr.size,&stats)!=MRW_OK) return 15;
    if(fabsf(out[3]-1)>.0002f || fabsf(out[15]-1)>.0002f) return 16;
    if(stats.active_lane_joints!=2 || stats.tiles!=1) return 17;
    mrw_authoring_free(out);mrw_authoring_free(scratch);mrw_authoring_free(f.bytes);
    return 0;
}
