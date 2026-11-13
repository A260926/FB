#include <event2/buffer.h>
#include <event2/event.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
static int fail_at=-1, cleanups;
static void *allocfn(size_t n){if(fail_at>0 && --fail_at==0){fail_at=-1;return NULL;}return malloc(n);}
static void *reallocfn(void *p,size_t n){if(fail_at>0 && --fail_at==0){fail_at=-1;return NULL;}return realloc(p,n);}
static void cleanup(const void *p,size_t n,void *arg){(void)p;(void)n;(void)arg;++cleanups;}
static size_t added;
static void change(struct evbuffer *b,const struct evbuffer_cb_info *i,void *x){(void)b;(void)x;added+=i->n_added;}
int main(int argc,char **argv){struct evbuffer *s,*d;char out[32]={0};int r;const char *blocks[]={"ABCD","EFGH","IJKL"};
 event_set_mem_functions(allocfn,reallocfn,free);s=evbuffer_new();d=evbuffer_new();assert(s&&d);
 for(int i=0;i<3;i++)assert(evbuffer_add_reference(s,blocks[i],4,cleanup,NULL)==0);
 assert(evbuffer_add(d,"D",1)==0);assert(evbuffer_add_cb(d,change,NULL));added=0;
 if(argc>1 && strcmp(argv[1],"--oom-witness")==0)fail_at=2;
 r=evbuffer_add_buffer_reference(d,s);fail_at=-1;
 if(argc>1){printf("ORIGINAL_MULTICAST_OOM rc=%d length=%zu added=%zu\n",r,evbuffer_get_length(d),added);assert(r==0 && evbuffer_get_length(d)==5 && added==12);}
 else {assert(r==0 && evbuffer_get_length(d)==13 && added==12);evbuffer_free(s);s=NULL;assert(cleanups==0);assert(evbuffer_copyout(d,out,13)==13);assert(memcmp(out,"DABCDEFGHIJKL",13)==0);puts("EVBUFFER_SHARED_BASELINE_OK");}
 evbuffer_free(d);if(s)evbuffer_free(s);assert(cleanups==3);return 0;}
