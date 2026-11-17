#include <stdio.h>
#include <string.h>
#include "libdwarf.h"
#include "dwarf.h"
static int one(const char *path,int plain) {
    Dwarf_Debug dbg=0;Dwarf_Error err=0;Dwarf_Cie *cs=0;
    Dwarf_Fde *fs=0;Dwarf_Signed nc=0,nf=0;Dwarf_Small *bytes=0;
    Dwarf_Unsigned len=0,count=0;Dwarf_Frame_Instr_Head head=0;
    int r=dwarf_init_path(path,0,0,DW_GROUPNUMBER_ANY,0,0,&dbg,&err);
    if(r!=DW_DLV_OK){fprintf(stderr,"init=%d %s\n",r,err?dwarf_errmsg(err):"");return 1;}
    r=dwarf_get_fde_list(dbg,&cs,&nc,&fs,&nf,&err);
    if(r!=DW_DLV_OK||nf!=1||nc!=1){fprintf(stderr,"list=%d nf=%lld err=%s\n",r,(long long)nf,err?dwarf_errmsg(err):"");return 2;}
    if(plain) {
        Dwarf_Small value_type=0;Dwarf_Signed offset=0;
        Dwarf_Unsigned reg=0;Dwarf_Unsigned relevant=0;Dwarf_Block block={0};
        Dwarf_Addr row=0,next=0;Dwarf_Bool more=0;
        r=dwarf_get_fde_info_for_cfa_reg3_c(fs[0],0x1010,&value_type,
            &relevant,&reg,&offset,&block,&row,&more,&next,&err);
        if(r!=DW_DLV_OK||reg!=31||offset!=32||row!=0x1010){
            fprintf(stderr,"CFA r=%d reg=%lld off=%lld row=%llx\n",r,
                (long long)reg,(long long)offset,(unsigned long long)row);return 3;}
        puts("CFA_BASELINE SP+32 row=0x1010");
    }
    r=dwarf_get_fde_instr_bytes(fs[0],&bytes,&len,&err);
    if(r!=DW_DLV_OK)return 4;
    r=dwarf_expand_frame_instructions(cs[0],bytes,len,&head,&count,&err);
    printf("CFI_%s expand=%d count=%llu",plain?"PLAIN":"SET_RA",r,(unsigned long long)count);
    if(r==DW_DLV_ERROR) {
        printf(" error=%s",dwarf_errmsg(err));
        dwarf_dealloc_error(dbg,err);err=0;
    }
    puts("");
    if(head)dwarf_dealloc_frame_instr_head(head);
    dwarf_dealloc_fde_cie_list(dbg,cs,nc,fs,nf);
    dwarf_finish(dbg);
    return plain&&r!=DW_DLV_OK?5:0;
}
int main(void) {
    int r=one(".task-build/plain.elf",1);if(r)return r;
    r=one(".task-build/set.elf",0);if(r)return r;
    puts("BASELINE_OK");return 0;
}
