#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "scarletbook_output.h"
#include "dsf.h"
#include "dsdiff.h"
extern const scarletbook_format_handler_t *dsf_format_fn(void);
extern const scarletbook_format_handler_t *dsdiff_format_fn(void);
#define CHECK(x) do {if(!(x)){fprintf(stderr,"baseline failure line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static uint64_t le64(const unsigned char*p){uint64_t n=0;for(int i=7;i>=0;i--)n=(n<<8)|p[i];return n;}
static uint64_t be64(const unsigned char*p){uint64_t n=0;for(int i=0;i<8;i++)n=(n<<8)|p[i];return n;}
static unsigned char reverse(unsigned char n){unsigned char r=0;for(int i=0;i<8;i++){r=(r<<1)|(n&1);n>>=1;}return r;}
static int run(int dsf){
 scarletbook_handle_t *sb=calloc(1,sizeof(*sb));area_toc_t toc={0};master_toc_t master={0};area_tracklist_t times={0};scarletbook_output_format_t f={0};
 unsigned char input[16];for(unsigned i=0;i<16;i++)input[i]=(unsigned char)(i*29+7);
 CHECK(sb);toc.channel_count=2;toc.track_count=1;sb->area[0].area_toc=&toc;sb->id3_tag_mode=2;sb->master_toc=&master;sb->area[0].area_tracklist_time=&times;
 f.sb_handle=sb;f.fd=tmpfile();f.dsd_encoded_export=1;f.filename="baseline";
 f.handler=*(dsf?dsf_format_fn():dsdiff_format_fn());f.priv=calloc(1,f.handler.priv_size);CHECK(f.fd&&f.priv);
 CHECK(f.handler.startwrite(&f)==0);CHECK(f.handler.write(&f,input,sizeof(input))>=0);CHECK(f.handler.stopwrite(&f)==0);
 CHECK(fseek(f.fd,0,SEEK_END)==0);long size=ftell(f.fd);CHECK(size>0);rewind(f.fd);
 unsigned char*b=malloc((size_t)size);CHECK(b&&fread(b,1,(size_t)size,f.fd)==(size_t)size);
 if(dsf){CHECK(!memcmp(b,"DSD ",4));CHECK(le64(b+12)==(uint64_t)size);CHECK(le64(b+64)==64);CHECK(le64(b+84)==8204);CHECK(le64(b+20)==8284);CHECK(!memcmp(b+8284,"ID3",3));
  for(int c=0;c<2;c++)for(int i=0;i<4096;i++)CHECK(b[92+c*4096+i]==(i<8?reverse(input[i*2+c]):0));
  puts("DSF samples=64 data_bytes=8192 metadata_offset=8284 audio_bits=OK");
 }else{CHECK(!memcmp(b,"FRM8",4));CHECK(be64(b+4)==(uint64_t)size-12);int found=0,id3=0;
  for(uint64_t p=16;p+12<=(uint64_t)size;){uint64_t n=be64(b+p+4);CHECK(n<=(uint64_t)size-p-12);if(!memcmp(b+p,"DSD ",4)){CHECK(n==16);CHECK(!memcmp(b+p+12,input,16));found++;}if(!memcmp(b+p,"ID3 ",4)){CHECK(n>=10&&!memcmp(b+p+12,"ID3",3));id3++;}p+=12+n+(n&1);}
  CHECK(found==1&&id3==1);puts("DSDIFF samples=64 data_bytes=16 footer_id3=OK audio_bytes=OK");
 }
 free(b);free(f.priv);fclose(f.fd);free(sb);return 0;
}
int main(void){if(run(1)||run(0))return 1;puts("BASELINE_COMPLETE normal production handlers");return 0;}
