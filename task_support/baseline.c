#include "libinjection_xss.h"
#include <stdio.h>
#include <string.h>
int main(void){
 const char *a="<a href='javascript:x'>",*b="<p>ordinary text</p>";
 size_t n=strlen(a),cut=(size_t)(strstr(a,"ascript")-a);
 int whole=libinjection_xss(a,n),clean=libinjection_xss(b,strlen(b));
 int left=libinjection_xss(a,cut),right=libinjection_xss(a+cut,n-cut);
 if(whole!=1||clean!=0)return 2;
 printf("XSS_ONESHOT_OK unsafe=%d clean=%d split_calls=%d/%d cut=%zu\n",whole,clean,left,right,cut);
 h5_state_t h;libinjection_h5_init(&h,a,n,DATA_STATE);int tokens=0;
 while(libinjection_h5_next(&h)==LIBINJECTION_RESULT_TRUE)tokens++;
 if(tokens<3)return 3;printf("HTML5_TOKENS_OK count=%d\n",tokens);return 0;
}
