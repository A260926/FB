#include "fixture.h"
int main(void){
 lsmash_file_parameters_t file={0};uint32_t track=0;
 lsmash_root_t*root=open_movie(".task-build/baseline.mp4",&file,&track);
 if(!root){puts("BASELINE_SETUP_FAILED");return 1;}
 if(append_packet(root,track,0,40,0x11)<0 || append_packet(root,track,40,80,0x22)<0)return 2;
 isom_trak_t*t=isom_get_trak(root->file,track);
 if(!t || !t->cache->chunk.pool || t->cache->chunk.pool->sample_count!=2 || t->cache->chunk.pool->size!=16)return 3;
 puts("POOLED samples=2 bytes=16; no discard API in pinned source");
 if(close_movie(root,track,&file)<0)return 4;
 const uint8_t marks[]={0x11,0x22};if(verify_movie(".task-build/baseline.mp4",2,marks)<0)return 5;
 puts("PRODUCTION_REMUX_READBACK samples=2 dts=0,40 payload=11,22");return 0;
}
