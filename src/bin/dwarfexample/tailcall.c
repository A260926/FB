/*
  Copyright (c) 2026 David Anderson.  All rights reserved.

  This example program is hereby
  placed into the public domain to be copied or
  used by anyone for any purpose without restriction.

*/
/*  tailcall.c
    Reads an object file and prints, for every ordinary call in
    .debug_info, the DWARF5 call site chain candidates and the
    parameter values and sources declared for them.

    Only debug information is read. No instruction is executed
    and no process is inspected. The candidates say what the
    compiler declared, not which machine path ran.

    Usage: tailcall <object-file>
*/

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dwarf.h"
#include "libdwarf.h"

struct user_state {
    Dwarf_Callsite_Snapshot snap;
    unsigned function_index;
    unsigned call_count;
};

static void
print_line(void *user,const char *line)
{
    (void)user;
    printf("%s\n",line);
}

/*  Print one function: its name, then every call and, for the
    ordinary calls, the chain candidates with parameters.  */
static int
print_function(struct user_state *state,
    const Dwarf_Callsite_Die_Identity *id,unsigned index)
{
    Dwarf_Error error = 0;
    const char *name = 0;
    Dwarf_Unsigned call_count = 0;
    Dwarf_Unsigned ci = 0;
    int res = 0;

    res = dwarf_callsite_function_name(state->snap,id,&name,
        &error);
    if (res != DW_DLV_OK) {
        return 0;
    }
    res = dwarf_callsite_snapshot_function_call_count(state->snap,
        id,&call_count,&error);
    if (res != DW_DLV_OK) {
        return 0;
    }
    printf("FUNCTION index=%u cu=0x%llx die=0x%llx name=%s "
        "calls=%llu\n",index,
        (unsigned long long)id->dci_cu_header_offset,
        (unsigned long long)id->dci_die_offset,
        name?name:"<unnamed>",
        (unsigned long long)call_count);
    for (ci = 0; ci < call_count; ++ci) {
        Dwarf_Off call_site = 0;
        Dwarf_Addr return_pc = 0;
        Dwarf_Bool has_return_pc = 0;
        Dwarf_Bool is_tail = 0;
        Dwarf_Callsite_Query query = 0;

        res = dwarf_callsite_snapshot_function_call(state->snap,
            id,ci,&call_site,&return_pc,&has_return_pc,&is_tail,
            &error);
        if (res != DW_DLV_OK) {
            continue;
        }
        if (is_tail) {
            /*  Tail calls are printed as edges of the ordinary
                call chains that reach them.  */
            continue;
        }
        if (!has_return_pc) {
            printf("CALL call_site=0x%llx return_pc=<missing> "
                "tail=0\n",(unsigned long long)call_site);
            continue;
        }
        res = dwarf_callsite_query(state->snap,id,return_pc,
            &query,&error);
        if (res == DW_DLV_NO_ENTRY) {
            continue;
        }
        if (res != DW_DLV_OK) {
            continue;
        }
        ++state->call_count;
        dwarf_callsite_query_print(query,print_line,0,&error);
        dwarf_callsite_query_destroy(query);
    }
    return 1;
}

int
main(int argc,char **argv)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Error error = 0;
    Dwarf_Callsite_Snapshot snap = 0;
    Dwarf_Unsigned count = 0;
    Dwarf_Unsigned i = 0;
    struct user_state state;
    int res = 0;
    int rc = 0;

    if (argc != 2) {
        printf("usage: %s <object-file>\n",argv[0]);
        return 2;
    }
    res = dwarf_init_path(argv[1],0,0,DW_GROUPNUMBER_ANY,0,0,
        &dbg,&error);
    if (res != DW_DLV_OK) {
        printf("tailcall: unable to open %s: %s\n",argv[1],
            error?dwarf_errmsg(error):"no DWARF data");
        if (error) {
            dwarf_dealloc_error(0,error);
        }
        return 1;
    }
    res = dwarf_callsite_snapshot_create(dbg,&snap,&error);
    if (res != DW_DLV_OK) {
        printf("tailcall: snapshot creation failed: %s\n",
            error?dwarf_errmsg(error):"unknown");
        if (error) {
            dwarf_dealloc_error(dbg,error);
        }
        dwarf_finish(dbg);
        return 1;
    }
    /*  The snapshot owns everything. Finish the Dwarf_Debug
        first: printing below must not need it.  */
    dwarf_finish(dbg);
    dbg = 0;

    memset(&state,0,sizeof(state));
    state.snap = snap;
    res = dwarf_callsite_snapshot_function_count(snap,&count,
        &error);
    if (res != DW_DLV_OK) {
        rc = 1;
        goto done;
    }
    for (i = 0; i < count; ++i) {
        Dwarf_Callsite_Die_Identity id;
        Dwarf_Unsigned has_calls = 0;

        memset(&id,0,sizeof(id));
        res = dwarf_callsite_snapshot_function_identity(snap,i,
            &id,&error);
        if (res != DW_DLV_OK) {
            continue;
        }
        res = dwarf_callsite_snapshot_function_call_count(snap,&id,
            &has_calls,&error);
        if (res != DW_DLV_OK) {
            continue;
        }
        if (!has_calls) {
            continue;
        }
        if (!print_function(&state,&id,(unsigned)i)) {
            rc = 1;
            break;
        }
    }
done:
    printf("TAILCALL_SUMMARY functions=%llu calls=%u\n",
        (unsigned long long)count,state.call_count);
    dwarf_callsite_snapshot_destroy(snap);
    if (error) {
        dwarf_dealloc_error(0,error);
    }
    return rc;
}
