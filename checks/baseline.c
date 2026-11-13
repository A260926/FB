#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define DMALLOC_DISABLE
#include "dmalloc.h"
#include "arg_check.h"
#include "debug_tok.h"
int main(void){dmalloc_debug(DMALLOC_DEBUG_CHECK_FUNCS|DMALLOC_DEBUG_CHECK_FENCE);char*d=dmalloc_malloc(__FILE__,__LINE__,8,DMALLOC_FUNC_MALLOC,0,0);char*s=dmalloc_malloc(__FILE__,__LINE__,9,DMALLOC_FUNC_MALLOC,0,0);if(!d||!s)return 1;memset(d,'D',8);memset(s,'Q',9);dmalloc_errno=0;void*r=_dmalloc_memcpy(__FILE__,__LINE__,d,s,9);int err=dmalloc_errno;if(r!=d||d[0]!='Q'||!err)return 2;printf("DMALLOC check=reported destination=changed error=%d\n",err);return 0;}
