#include "fixture.h"
int main(void){
 lsmash_file_parameters_t file={0};uint32_t track=0,discarded=999;
 lsmash_root_t*root=open_movie(".task-build/api.mp4",&file,&track);if(!root)return 1;
 if(lsmash_set_pool_undo(root,track,1)<0)return 10;
 if(append_packet(root,track,0,40,0x11)<0 || append_packet(root,track,40,80,0x22)<0)return 11;
 if(lsmash_discard_pooled_samples(root,track,&discarded)<0 || discarded!=2)return 12;
 isom_trak_t*t=isom_get_trak(root->file,track);
 if(t->cache->chunk.pool && t->cache->chunk.pool->sample_count)return 13;
 if(append_packet(root,track,0,40,0x33)<0 || close_movie(root,track,&file)<0)return 14;
 const uint8_t marks[]={0x33};if(verify_movie(".task-build/api.mp4",1,marks)<0)return 15;
 puts("UNDO_CONSUMER_OK discarded=2 replacement=33");return 0;
}
