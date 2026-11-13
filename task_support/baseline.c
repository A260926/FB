#include "mujs.h"
#include <stdio.h>
#include <string.h>
int main(void){
 js_State *J=js_newstate(NULL,NULL,0);
 if(!J)return 1;
 if(js_ploadstring(J,"seed","var answer=(function(x){return x+1;})(41);"))return 2;
 js_pushundefined(J);
 if(js_pcall(J,0))return 3;
 js_pop(J,1);js_getglobal(J,"answer");
 if(js_tonumber(J,-1)!=42)return 4;
 js_pop(J,1);
 js_gc(J,0);
 if(js_gettop(J)!=0)return 5;
 js_freestate(J);
 puts("MUJS_SEED_CALL_42 stack=0 gc=ok");return 0;
}
