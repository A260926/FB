#include <event2/buffer.h>
#include <event2/event.h>
#include <stdio.h>
#include <stdlib.h>
static int fail_at=-1;
static void *m(size_t n){ if(fail_at>0 && --fail_at==0){fail_at=-1;return NULL;} return malloc(n);}
static void *r(void*p,size_t n){ if(fail_at>0 && --fail_at==0){fail_at=-1;return NULL;} return realloc(p,n);}
struct cb{size_t a,d;};
static void ccb(struct evbuffer*b,const struct evbuffer_cb_info*i,void*x){struct cb*c=x;(void)b;c->a+=i->n_added;c->d+=i->n_deleted;}
int main(void){
  event_set_mem_functions(m,r,free);
  struct evbuffer*s=evbuffer_new(),*d=evbuffer_new();
  struct cb cb={0,0}; evbuffer_add_cb(d,ccb,&cb);
  static const char* blk[3]={"AAAAA","BBBBBBB","CCCC"};
  for(int i=0;i<3;i++) evbuffer_add_reference(s,blk[i],i==2?4:(i==0?5:7),NULL,NULL);
  evbuffer_add(d,"PREFIX:",7);
  size_t sl=evbuffer_get_length(s), dl=evbuffer_get_length(d);
  fail_at=2;
  int rc=evbuffer_add_buffer_reference(d,s);
  printf("rc=%d dst=%zu(before %zu) src=%zu(before %zu) added=%zu\n",
    rc,evbuffer_get_length(d),dl,evbuffer_get_length(s),sl,cb.a);
  return 0;
}
