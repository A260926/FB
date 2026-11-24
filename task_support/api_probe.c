#include "mp4d_stream.h"
#include <stdio.h>
static unsigned n;
static int sample(const MP4D_stream_sample_t*s,const unsigned char*p,void*u){
 (void)u;if(!n&&(s->track_id!=1||s->timescale!=90000||s->dts||s->pts||s->duration!=3000||s->size!=6||p[4]!=0x65||s->flags!=0x02000000))return 1;n++;return 0;
}
int main(int argc,char**argv){unsigned char b[7];size_t k;FILE*f;MP4D_stream_t*s;
 if(argc!=2||(f=fopen(argv[1],"rb"))==0)return 2;
 s=MP4D_stream_open(1048576,sample,0);if(!s)return 3;
 while((k=fread(b,1,sizeof b,f)))if(MP4D_stream_feed(s,b,k)<0)return 4;
 if(ferror(f)||fclose(f)||MP4D_stream_finish(s)<0||n!=4)return 5;
 MP4D_stream_close(s);puts("API_NORMAL samples=4 first_video=OK");return 0;
}
