#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#define REQUIRE(x) do { if (!(x)) {fprintf(stderr,"baseline failure line %d\n",__LINE__);return 1;} } while(0)
int main(void) {
 const char *files[]={"vectors/l3-si_block.bit","vectors/l3-compl.bit","vectors/M2L3_noise.bit","vectors/l3-sin1k0db.bit"};
 for(size_t i=0;i<sizeof(files)/sizeof(files[0]);i++) {
  mp3dec_t d; mp3dec_file_info_t all; mp3dec_ex_t ex; mp3d_sample_t block[514];
  REQUIRE(mp3dec_load(&d,files[i],&all,NULL,NULL)==0); REQUIRE(all.samples>2048);
  size_t nonzero=0;for(size_t j=0;j<all.samples;j++) nonzero+=all.buffer[j]!=0; REQUIRE(nonzero>100);
  REQUIRE(mp3dec_ex_open(&ex,files[i],MP3D_SEEK_TO_SAMPLE)==0);
  size_t n=mp3dec_ex_read(&ex,block,514);REQUIRE(n==514); REQUIRE(!memcmp(block,all.buffer,n*sizeof(*block)));
  REQUIRE(mp3dec_ex_seek(&ex,0)==0);n=mp3dec_ex_read(&ex,block,514);REQUIRE(n==514);REQUIRE(!memcmp(block,all.buffer,n*sizeof(*block)));
  printf("NATIVE_OK file=%s hz=%d channels=%d samples=%zu delay=%d nonzero=%zu seek_zero=equal\n",files[i],all.hz,all.channels,all.samples,ex.start_delay,nonzero);
  mp3dec_ex_close(&ex);free(all.buffer);
 }
 puts("NATIVE_BASELINE_OK");return 0;
}
