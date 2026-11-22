#include <opusfile.h>
#include <stdio.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"required FEC API probe failed: %s\n",#x);return 1;}}while(0)
static int read37(void*v,unsigned char*p,int n){FILE*f=v;size_t k;if(n>37)n=37;k=fread(p,1,n,f);return k>0||feof(f)?(int)k:-1;}
int main(void){
 FILE*f=fopen(".task-build/source.opus","rb");OpusFileCallbacks cb={read37,NULL,NULL,NULL};int err=0,li=0;float pcm[4];OggOpusFile*of;
 REQUIRE(f!=NULL);of=op_open_callbacks(f,&cb,NULL,0,&err);REQUIRE(of!=NULL&&!op_seekable(of));
 REQUIRE(op_set_inband_fec(of,0)==0);REQUIRE(op_set_inband_fec(of,1)==0);
 REQUIRE(op_set_inband_fec(of,2)==OP_EINVAL);
 REQUIRE(op_read_float(of,pcm,4,&li)>0);REQUIRE(op_set_inband_fec(of,0)==OP_EINVAL);
 op_free(of);REQUIRE(fclose(f)==0);
 of=op_open_file(".task-build/source.opus",&err);REQUIRE(of!=NULL&&op_seekable(of));REQUIRE(op_set_inband_fec(of,1)==OP_EIMPL);op_free(of);
 puts("REQUIRED_INBAND_FEC_API_OK nonseekable=accepted started=invalid seekable=unsupported");return 0;
}
