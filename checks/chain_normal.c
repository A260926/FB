#include "miniaudio.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 float a[]={1,2,3,4,5,6},b[]={11,12,13,14,15,16},out[10];
 const float want[]={2,3,4,5,13,14,15,16};
 ma_audio_buffer_ref x,y;ma_uint64 nr=0;
 assert(ma_audio_buffer_ref_init(ma_format_f32,1,a,6,&x)==MA_SUCCESS);
 assert(ma_audio_buffer_ref_init(ma_format_f32,1,b,6,&y)==MA_SUCCESS);
 assert(ma_data_source_set_range_in_pcm_frames(&x,1,5)==MA_SUCCESS);
 assert(ma_data_source_set_range_in_pcm_frames(&y,2,6)==MA_SUCCESS);
 assert(ma_data_source_seek_to_pcm_frame(&x,0)==MA_SUCCESS);
 assert(ma_data_source_set_next(&x,&y)==MA_SUCCESS);
 out[8]=12345;
 assert(ma_data_source_read_pcm_frames(&x,out,8,&nr)==MA_SUCCESS&&nr==8);
 for(int i=0;i<8;i++)assert(out[i]==want[i]);
 assert(out[8]==12345);
 assert(ma_data_source_read_pcm_frames(&x,out,1,&nr)==MA_AT_END&&nr==0);
 ma_audio_buffer_ref_uninit(&x);ma_audio_buffer_ref_uninit(&y);
 puts("CHAIN_BASELINE_OK frames=8 trimmed=1");return 0;
}
