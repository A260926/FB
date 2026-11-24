#include "afl-struct.h"
#include <stdio.h>
static int calls;
static int observe(const unsigned char *p,size_t n,void*ctx){(void)p;(void)n;(void)ctx;++calls;return 1;}
int main(void){
 afl_struct_session *s=afl_struct_create(NULL,0,"");if(!s)return 1;
 size_t n=99;afl_struct_data(s,&n);if(n!=0)return 2;
 int result=afl_struct_step(s,0,observe,NULL);
 afl_struct_destroy(s);if(result!=0||calls)return 3;
 printf("structured API empty graph complete callbacks=%d\n",calls);return 0;
}
