#include "libdwarf.h"
#include "dwarf.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned functions,calls,parameters;
static int walk(Dwarf_Debug dbg,Dwarf_Die die) {
    Dwarf_Error e=0;Dwarf_Half tag=0;Dwarf_Die child=0,next=0;
    int r;Dwarf_Attribute a=0;
    if(dwarf_tag(die,&tag,&e)!=DW_DLV_OK)return 1;
    if(tag==DW_TAG_subprogram)++functions;
    if(tag==DW_TAG_call_site) {
        Dwarf_Addr pc=0;Dwarf_Off origin=0;Dwarf_Bool info=0;
        ++calls;
        if(dwarf_attr(die,DW_AT_call_return_pc,&a,&e)!=DW_DLV_OK)return 2;
        if(dwarf_formaddr(a,&pc,&e)!=DW_DLV_OK)return 3;dwarf_dealloc_attribute(a);
        if(dwarf_attr(die,DW_AT_call_origin,&a,&e)!=DW_DLV_OK)return 4;
        if(dwarf_global_formref_b(a,&origin,&info,&e)!=DW_DLV_OK)return 5;dwarf_dealloc_attribute(a);
        printf("CALL return_pc=0x%llx origin=0x%llx info=%u\n",(unsigned long long)pc,(unsigned long long)origin,info);
    }
    if(tag==DW_TAG_call_site_parameter) {
        Dwarf_Unsigned n=0,i;Dwarf_Ptr bytes=0;++parameters;
        if(dwarf_attr(die,DW_AT_call_value,&a,&e)!=DW_DLV_OK)return 6;
        if(dwarf_formexprloc(a,&n,&bytes,&e)!=DW_DLV_OK)return 7;
        printf("CALL_VALUE bytes=");for(i=0;i<n;i++)printf("%02x",((unsigned char*)bytes)[i]);puts("");
        dwarf_dealloc_attribute(a);
    }
    r=dwarf_child(die,&child,&e);
    if(r==DW_DLV_ERROR)return 8;
    while(r==DW_DLV_OK) {
        int rc=walk(dbg,child);if(rc)return rc;
        r=dwarf_siblingof_b(dbg,child,1,&next,&e);dwarf_dealloc_die(child);child=next;next=0;
        if(r==DW_DLV_ERROR)return 9;
    }
    return 0;
}
int main(int argc,char **argv) {
    Dwarf_Debug dbg=0;Dwarf_Error e=0;Dwarf_Die cu=0;int rc,r;
    Dwarf_Unsigned len=0,next=0,to=0;Dwarf_Half ver=0,addr=0,ls=0,ext=0,ut=0;
    Dwarf_Off ab=0;Dwarf_Sig8 sig;
    if(argc!=2)return 2;
    r=dwarf_init_path(argv[1],0,0,DW_GROUPNUMBER_ANY,0,0,&dbg,&e);
    if(r!=DW_DLV_OK){fprintf(stderr,"init: %s\n",e?dwarf_errmsg(e):"no debug");return 3;}
    r=dwarf_next_cu_header_d(dbg,1,&len,&ver,&ab,&addr,&ls,&ext,&sig,&to,&next,&ut,&e);
    if(r!=DW_DLV_OK)return 4;
    if(dwarf_siblingof_b(dbg,0,1,&cu,&e)!=DW_DLV_OK)return 5;
    rc=walk(dbg,cu);dwarf_dealloc_die(cu);dwarf_finish(dbg);
    if(rc){fprintf(stderr,"walk failed %d\n",rc);return rc;}
    if(functions!=2||calls!=1||parameters!=1)return 10;
    printf("BASELINE_OK functions=%u callsites=%u parameters=%u dwarf=%u\n",functions,calls,parameters,ver);
    return 0;
}
