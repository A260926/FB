#include "quickjs.h"
#include <assert.h>
#include <stdio.h>
static JSClassID cid;
static int finalized[2];
static void finish(JSRuntime *rt,JSValue v){
  int *id=JS_GetOpaque(v,cid);(void)rt;assert(id);assert(!finalized[*id]);finalized[*id]++;
}
int main(void){
 JSRuntime *rt=JS_NewRuntime();assert(rt);JSContext *ctx=JS_NewContext(rt);assert(ctx);
 JSClassDef def={.class_name="BaselineOwned",.finalizer=finish};JS_NewClassID(rt,&cid);assert(JS_NewClass(rt,cid,&def)==0);
 int ids[2]={0,1};JSValue a=JS_NewObjectClass(ctx,cid),b=JS_NewObjectClass(ctx,cid);assert(JS_IsObject(a)&&JS_IsObject(b));JS_SetOpaque(a,&ids[0]);JS_SetOpaque(b,&ids[1]);
 JSValue alias=JS_DupValue(ctx,a);assert(JS_IsException(JS_Throw(ctx,a)));assert(JS_HasException(ctx));
 assert(JS_IsException(JS_Throw(ctx,b)));assert(!finalized[0]&&!finalized[1]);
 JS_FreeValue(ctx,alias);JS_RunGC(rt);assert(finalized[0]==1&&finalized[1]==0);
 JSValue caught=JS_GetException(ctx);assert(!JS_HasException(ctx));assert(JS_GetOpaque(caught,cid)==&ids[1]);
 JS_FreeValue(ctx,caught);JS_RunGC(rt);assert(finalized[0]==1&&finalized[1]==1);
 JS_FreeContext(ctx);JS_FreeRuntime(rt);puts("QJS_BASELINE_OK replaced=1 caught=1 finalized=2");return 0;
}
