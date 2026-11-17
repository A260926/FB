#include "task_support/runtime.h"
static unsigned depth(DW_EvalState *s) {
 unsigned n=0; for(DW_EvalValNode *p=s->top_val;p;p=p->next) ++n; return n;
}
int main(void) {
 Arena *a=arena_alloc();
 U8 code[]={DW_ExprOp_ConstU,0x80,0x20,DW_ExprOp_DerefSize,8};
 U8 full[]={1,2,3,4,5,6,7,8}; U8 newer[]={9,10};
 String8 expr=str8(code,sizeof(code));
 MemoryMap complete={0}; memory_map_push(a,&complete,r1u64(4096,4104),full);
 DW_EvalState normal={0};
 DW_Eval n=dw_eval(a,DW_Format_32Bit,Arch_x64,&complete,0,0,0,0,&normal,expr,100);
 printf("normal status=%d value=%016llx ops=%llu\n",n.status,(unsigned long long)n.val.u512.u64[0],(unsigned long long)normal.op_idx);
 if(n.status!=DW_EvalStatus_Good || n.val.u512.u64[0]!=0x0807060504030201ULL) return 1;
 MemoryMap missing={0}; DW_EvalState ms={0};
 DW_Eval m=dw_eval(a,DW_Format_32Bit,Arch_x64,&missing,0,0,0,0,&ms,expr,100);
 printf("missing status=%d off=%llu ops=%llu depth=%u missed=[%llu,%llu)\n",m.status,(unsigned long long)ms.off,(unsigned long long)ms.op_idx,depth(&ms),(unsigned long long)m.missed_read_vaddr_range.min,(unsigned long long)m.missed_read_vaddr_range.max);
 MemoryMap partial={0}; memory_map_push(a,&partial,r1u64(4096,4098),full); DW_EvalState ps={0};
 DW_Eval p=dw_eval(a,DW_Format_32Bit,Arch_x64,&partial,0,0,0,0,&ps,expr,100);
 printf("partial status=%d value=%016llx ops=%llu\n",p.status,(unsigned long long)p.val.u512.u64[0],(unsigned long long)ps.op_idx);
 memory_map_push(a,&complete,r1u64(4098,4100),newer);
 U8 out[8]={0}; U64 got=memory_map_read(&complete,r1u64(4096,4104),out);
 printf("overlap bytes=%llu data=",(unsigned long long)got);for(unsigned i=0;i<8;i++)printf("%02x",out[i]);puts("");
 arena_release(a);puts("BASELINE_OK: production DWARF evaluator and MemoryMap executed");return 0;
}
