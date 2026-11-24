#include "jerryscript.h"
#include "jerryscript-ext/handle-scope.h"
#include <assert.h>
#include <stdio.h>
static int finalized[2];
static void finish(void *p,jerry_object_native_info_t *info){(void)info;int id=*(int*)p;assert(!finalized[id]);finalized[id]++;}
static const jerry_object_native_info_t native_info={.free_cb=finish,.number_of_references=0,.offset_of_references=0};
int main(void){
 int ids[2]={0,1};
 for(int cycle=0;cycle<2;cycle++){
  jerry_init(JERRY_INIT_EMPTY);jerryx_handle_scope outer,inner;
  assert(jerryx_open_handle_scope(&outer)==jerryx_handle_scope_ok);
  assert(jerryx_open_escapable_handle_scope(&inner)==jerryx_handle_scope_ok);
  jerry_value_t obj=jerry_object();jerry_object_set_native_ptr(obj,&native_info,&ids[cycle]);
  jerry_value_t exc=jerryx_create_handle(jerry_throw_value(obj,true));assert(jerry_value_is_exception(exc));
  jerry_value_t payload=jerryx_create_handle(jerry_exception_value(exc,false));assert(jerry_value_is_object(payload));
  jerry_value_t escaped;assert(jerryx_escape_handle(inner,payload,&escaped)==jerryx_handle_scope_ok);
  assert(jerryx_close_escapable_handle_scope(inner)==jerryx_handle_scope_ok);
  jerry_heap_gc(JERRY_GC_PRESSURE_HIGH);assert(finalized[cycle]==0);
  assert(jerryx_close_handle_scope(outer)==jerryx_handle_scope_ok);
  jerry_heap_gc(JERRY_GC_PRESSURE_HIGH);assert(finalized[cycle]==1);
  assert(jerryx_handle_scope_get_current()==jerryx_handle_scope_get_root());jerry_cleanup();
 }
 assert(finalized[0]==1&&finalized[1]==1);puts("JERRY_BASELINE_OK cycles=2 escaped=2 finalized=2");return 0;
}
