#include "fixture.h"
int main(void){lsmash_root_t*r=lsmash_create_root();lsmash_file_parameters_t p={0};
 if(!r || lsmash_open_file(".task-build/pool-undo-example.mp4",1,&p)<0)return 1;
 lsmash_file_t*f=lsmash_set_file(r,&p);if(!f || lsmash_read_file(f,&p)<0)return 2;
 uint32_t t=lsmash_get_track_ID(r,1);if(!t || lsmash_construct_timeline(r,t)<0)return 3;
 uint32_t n=lsmash_get_sample_count_in_media_timeline(r,t);if(!n)return 4;
 for(uint32_t i=1;i<=n;++i){lsmash_sample_t*s=lsmash_get_sample_from_media_timeline(r,t,i);if(!s || !s->length)return 5;lsmash_delete_sample(s);}
 lsmash_close_file(&p);lsmash_destroy_root(r);printf("EXAMPLE_READABLE samples=%u\n",n);return 0;}
