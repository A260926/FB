#include <yajl/yajl_parse.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct Ctx{int calls;size_t bytes;};
static int string(void*p,const unsigned char*s,size_t n){struct Ctx*c=p;c->calls++;c->bytes+=n;return 1;}
int main(void){struct Ctx c={0,0};yajl_callbacks cb={0};cb.yajl_string=string;yajl_handle h=yajl_alloc(&cb,NULL,&c);assert(h);
assert(yajl_parse(h,(const unsigned char*)"[\"",2)==yajl_status_ok);
unsigned char block[65536];memset(block,'x',sizeof(block));
for(int i=0;i<32;i++){assert(yajl_parse(h,block,sizeof(block))==yajl_status_ok);assert(c.calls==0);}
assert(yajl_parse(h,(const unsigned char*)"\"]",2)==yajl_status_ok);assert(yajl_complete_parse(h)==yajl_status_ok);
assert(c.calls==1&&c.bytes==2097152);yajl_free(h);
printf("YAJL_BASELINE_STRING_CALLBACK_OK bytes=%zu calls=%d\n",c.bytes,c.calls);}
