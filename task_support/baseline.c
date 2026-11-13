#include "wren.h"
#include <stdio.h>
static int errors=0;
static void error(WrenVM* v,WrenErrorType t,const char*m,int l,const char*s){(void)v;(void)t;fprintf(stderr,"%s:%d:%s\n",m?m:"?",l,s);errors++;}
int main(void){
 WrenConfiguration c;wrenInitConfiguration(&c);c.errorFn=error;WrenVM*v=wrenNewVM(&c);
 const char*src="var Suspended = Fiber.new {\n var x = 10\n var get = Fn.new { x }\n Fiber.yield(get)\n x = x + 7\n Fiber.yield(get)\n return get\n}\n";
 if(wrenInterpret(v,"baseline",src)!=WREN_RESULT_SUCCESS)return 2;
 wrenEnsureSlots(v,1);wrenGetVariable(v,"baseline","Suspended",0);
 WrenHandle*f=wrenGetSlotHandle(v,0),*call=wrenMakeCallHandle(v,"call()");
 wrenSetSlotHandle(v,0,f);if(wrenCall(v,call)!=WREN_RESULT_SUCCESS)return 3;
 WrenHandle*g=wrenGetSlotHandle(v,0);int vals[3];
 for(int i=0;i<3;i++){
  wrenCollectGarbage(v);wrenEnsureSlots(v,1);wrenSetSlotHandle(v,0,g);
  if(wrenCall(v,call)!=WREN_RESULT_SUCCESS)return 4;vals[i]=(int)wrenGetSlotDouble(v,0);
  if(i<2){wrenSetSlotHandle(v,0,f);if(wrenCall(v,call)!=WREN_RESULT_SUCCESS)return 5;}
 }
 wrenReleaseHandle(v,g);wrenReleaseHandle(v,f);wrenReleaseHandle(v,call);wrenFreeVM(v);
 if(errors||vals[0]!=10||vals[1]!=17||vals[2]!=17)return 6;
 printf("FIBER_UPVALUE_BASELINE_OK values=%d/%d/%d\n",vals[0],vals[1],vals[2]);return 0;
}
