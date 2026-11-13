#include <math.h>
#include <stddef.h>
#include <stdio.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
#include <stdint.h>
#include <stdio.h>
int main(void){
 plm_buffer_t*b=plm_buffer_create_with_filename("task_support/source.mpg");plm_demux_t*d=plm_demux_create(b,1);if(!d)return 2;plm_packet_t*p;double pts=-1;while((p=plm_demux_decode(d))){if(p->type==PLM_DEMUX_PACKET_VIDEO_1&&p->pts!=PLM_PACKET_INVALID_TS){pts=p->pts;break;}}plm_demux_destroy(d);if(pts<0)return 3;
 plm_t*m=plm_create_with_filename("task_support/source.mpg");if(!m)return 4;plm_set_audio_enabled(m,0);plm_frame_t*f;unsigned count=0;double first=-1,last=-1;uint32_t hash=2166136261u;while((f=plm_decode_video(m))){if(!count)first=f->time;last=f->time;for(unsigned y=0;y<f->height;y++)for(unsigned x=0;x<f->width;x++)hash=(hash^f->y.data[y*f->y.width+x])*16777619u;count++;if(count>1000)return 5;}plm_destroy(m);
 printf("PES_first_pts90k=%lld frames=%u local_first=%.3f local_last=%.3f luma_fnv=%08x\n",(long long)llround(pts*90000),count,first,last,hash);return !(count>10&&first==0&&pts>0);}
