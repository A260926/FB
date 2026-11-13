#include "mojoshader.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static int run(const uint32_t*w,size_t n,const char*name){const MOJOSHADER_parseData*p=MOJOSHADER_parse("glsl120","main",(const unsigned char*)w,(unsigned)(n*4),NULL,0,NULL,0,NULL,NULL,NULL);printf("%s errors=%d\n",name,p->error_count);for(int i=0;i<p->error_count;i++)printf("error@%d: %s\n",p->errors[i].error_position,p->errors[i].error);if(p->output)puts(p->output);int e=p->error_count;MOJOSHADER_freeParseData(p);return e;}
int main(int argc,char**argv){
 const uint32_t plain[]={0xfffe0300,0x0200001f,0x80000000,0x900f0000,0x0200001f,0x80000000,0xe00f0000,0x02000001,0xe00f0000,0x90e40000,0x0000ffff};
 const uint32_t pred[]={0xfffe0300,0x0200001f,0x80000000,0x900f0000,0x0200001f,0x80000000,0xe00f0000,0x02000001,0x800f0000,0x90e40000,0x0303005e,0xb00f1000,0x90e40000,0x90e40000,0x13000001,0x800f0000,0x90e40000,0xb0e41000,0x02000001,0xe00f0000,0x80e40000,0x0000ffff};
 if(argc>1&&strcmp(argv[1],"--predicated")==0)return run(pred,sizeof(pred)/4,"PREDICATED")?4:0;
 if(run(plain,sizeof(plain)/4,"PLAIN"))return 1;puts("BASELINE_GLSL_VERTEX_OK");return 0;
}
