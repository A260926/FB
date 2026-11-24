#define DMALLOC_DISABLE
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "dmalloc.h"
#include "debug_tok.h"
#define VERIFY(x) do {if (!(x)) {fprintf(stderr,"baseline:%d\n",__LINE__); return 1;}} while(0)
int main(void) {
  unsigned char *p, *q;
  unsigned long i, sum=0;
  DMALLOC_SIZE size=0;
  dmalloc_debug(DMALLOC_DEBUG_CHECK_FENCE | DMALLOC_DEBUG_ALLOC_BLANK | DMALLOC_DEBUG_FREE_BLANK);
  p=dmalloc_malloc(__FILE__,__LINE__,73,DMALLOC_FUNC_MALLOC,0,0);
  VERIFY(p!=NULL);
  for(i=0;i<73;i++) p[i]=(unsigned char)(i+1);
  VERIFY(dmalloc_examine(p,&size,NULL,NULL,NULL,NULL,NULL,NULL)); VERIFY(size==73);
  q=dmalloc_realloc(__FILE__,__LINE__,p,117,DMALLOC_FUNC_REALLOC,0); VERIFY(q!=NULL);
  for(i=0;i<73;i++) VERIFY(q[i]==(unsigned char)(i+1));
  for(i=73;i<117;i++) q[i]=(unsigned char)(i+1);
  for(i=0;i<117;i++)sum+=q[i];
  VERIFY(dmalloc_verify(q)==DMALLOC_VERIFY_NOERROR);
  VERIFY(dmalloc_free(__FILE__,__LINE__,q,DMALLOC_FUNC_FREE)==FREE_NOERROR);
  p=dmalloc_malloc(__FILE__,__LINE__,55,DMALLOC_FUNC_NEW_ARRAY,0,0); VERIFY(p!=NULL);
  memset(p,7,55); VERIFY(dmalloc_verify(p)==DMALLOC_VERIFY_NOERROR);
  VERIFY(dmalloc_free(__FILE__,__LINE__,p,DMALLOC_FUNC_DELETE_ARRAY)==FREE_NOERROR);
  VERIFY(dmalloc_verify(NULL)==DMALLOC_VERIFY_NOERROR);
  printf("DMALLOC_HEALTHY checksum=%lu families=C,array fences=ok realloc=ok\n",sum);
  return 0;
}
