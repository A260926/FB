#include <stdio.h>
#include <stdlib.h>
#include "dwarf.h"
#include "libdwarf.h"
static void line(void *u,const char *s){(void)u;printf("%s\n",s);}
int main(int argc,char **argv){
    Dwarf_Debug dbg=0;Dwarf_Error e=0;Dwarf_Callsite_Snapshot snap=0;
    Dwarf_Unsigned n=0,i=0,j=0;
    if (argc!=2) return 2;
    if (dwarf_init_path(argv[1],0,0,DW_GROUPNUMBER_ANY,0,0,&dbg,&e)!=DW_DLV_OK) return 1;
    if (dwarf_callsite_snapshot_create(dbg,&snap,&e)!=DW_DLV_OK) return 1;
    dwarf_finish(dbg);
    dwarf_callsite_snapshot_function_count(snap,&n,&e);
    for(i=0;i<n;i++){
        Dwarf_Callsite_Die_Identity id;const char *nm=0;Dwarf_Unsigned c=0,k=0;
        dwarf_callsite_snapshot_function_identity(snap,i,&id,&e);
        dwarf_callsite_function_name(snap,&id,&nm,&e);
        dwarf_callsite_snapshot_function_call_count(snap,&id,&c,&e);
        for(k=0;k<c;k++){
            Dwarf_Off off;Dwarf_Addr pc;Dwarf_Bool hp,t;
            dwarf_callsite_snapshot_function_call(snap,&id,k,&off,&pc,&hp,&t,&e);
            if(!hp||t) continue;
            Dwarf_Callsite_Query q=0;
            if(dwarf_callsite_query(snap,&id,pc,&q,&e)==DW_DLV_OK){
                printf("FUNCTION %s\n",nm?nm:"<unnamed>");
                dwarf_callsite_query_print(q,line,0,&e);
                dwarf_callsite_query_destroy(q);
            }
        }
    }
    dwarf_callsite_snapshot_destroy(snap);
    return 0;
}
