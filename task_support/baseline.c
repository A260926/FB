#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#include <stdio.h>
int main(void){const char *path="vectors/l3-nonstandard-sin1k0db_lame_vbrtag.bit";mp3dec_t d;mp3dec_file_info_t all;mp3dec_ex_t ex;mp3d_sample_t b[257];size_t n,total=0;int nonzero=0;if(mp3dec_load(&d,path,&all,0,0)||!all.samples)return 1;if(mp3dec_ex_open(&ex,path,MP3D_SEEK_TO_SAMPLE))return 2;while((n=mp3dec_ex_read(&ex,b,257))){size_t i;for(i=0;i<n;i++){if(total+i>=all.samples||b[i]!=all.buffer[total+i])return 3;nonzero|=b[i]!=0;}total+=n;}if(total!=all.samples||!nonzero)return 4;printf("MINIMP3_BASELINE samples=%zu hz=%d channels=%d pcm_equal=1\n",total,all.hz,all.channels);mp3dec_ex_close(&ex);free(all.buffer);return 0;}
