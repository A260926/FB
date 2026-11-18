/*
Copyright (c) 2026 David Anderson.  All rights reserved.
This example code is hereby placed in the Public Domain
for anyone to use for any purpose.
*/

/*  frame_ra_state.c

    A small example showing how to read the AADWARF64 2026Q3
    AArch64 return address signing metadata from
    .debug_frame or .eh_frame.

    It walks the rows of every FDE, printing, for each row,
    the CFA rule and the return address signing state.

    This is descriptive only: libdwarf does not authenticate
    return addresses and does not unwind stacks.

    Build (after libdwarf is built):
        cc -std=c99 -I../../lib/libdwarf \
            frame_ra_state.c -ldwarf -o frame_ra_state

    Then run it on an AArch64 object:
        ./frame_ra_state aarch64-object

    On a non-AArch64 object dwarf_get_fde_ra_state() returns
    DW_DLV_NO_ENTRY and this example says so and stops.
*/

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_FCNTL_H
#include <fcntl.h> /* open() O_RDONLY */
#endif

#ifdef _WIN32
#include <io.h> /* close() open() */
#elif defined HAVE_UNISTD_H
#include <unistd.h> /* close() */
#endif /* _WIN32 */

#include "dwarf.h"
#include "libdwarf.h"

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#if defined(_WIN32) && !defined(O_BINARY)
#define O_BINARY _O_BINARY
#elif !defined(O_BINARY)
#define O_BINARY 0
#endif

static const char *
state_name(Dwarf_Unsigned state)
{
    switch (state) {
    case 0: return "unsigned";
    case 1: return "signed(SP)";
    case 2: return "signed(SP,PC)";
    default: return "reserved";
    }
}

/*  Print each row of one FDE.  Starting from the FDE low pc,
    dwarf_get_fde_ra_state() reports the row containing the pc
    plus has_more_rows/subsequent_pc, the same row model used
    by the CFA and register queries. */
static void
print_fde_rows(Dwarf_Fde fde,Dwarf_Error *error)
{
    Dwarf_Addr      lowpc = 0;
    Dwarf_Unsigned  funclen = 0;
    Dwarf_Small    *fde_bytes = 0;
    Dwarf_Unsigned  fde_byte_len = 0;
    Dwarf_Off       cie_offset = 0;
    Dwarf_Signed    cie_index = 0;
    Dwarf_Off       fde_offset = 0;
    Dwarf_Addr      pc = 0;
    int             res = 0;

    res = dwarf_get_fde_range(fde,&lowpc,&funclen,
        &fde_bytes,&fde_byte_len,&cie_offset,
        &cie_index,&fde_offset,error);
    if (res != DW_DLV_OK) {
        return;
    }
    printf("FDE low pc 0x%llx length %llu, CIE %lld\n",
        (unsigned long long)lowpc,
        (unsigned long long)funclen,
        (long long)cie_index);
    pc = lowpc;
    for (;;) {
        Dwarf_Ra_State ra;
        Dwarf_Addr     row_pc = 0;
        Dwarf_Bool     more = FALSE;
        Dwarf_Addr     next_pc = 0;
        Dwarf_Small    value_type = 0;
        Dwarf_Unsigned offset_relevant = 0;
        Dwarf_Unsigned regnum = 0;
        Dwarf_Signed   offset = 0;
        Dwarf_Block    block;

        memset(&block,0,sizeof(block));
        res = dwarf_get_fde_ra_state(fde,pc,&ra,&row_pc,
            &more,&next_pc,error);
        if (res == DW_DLV_NO_ENTRY) {
            printf("  not an AArch64 object: no RA state\n");
            return;
        }
        if (res != DW_DLV_OK) {
            printf("  dwarf_get_fde_ra_state failed: %s\n",
                dwarf_errmsg(*error));
            return;
        }
        /*  Read the ordinary CFA row alongside the RA state. */
        (void)dwarf_get_fde_info_for_cfa_reg3_c(fde,pc,
            &value_type,&offset_relevant,&regnum,&offset,
            &block,&row_pc,&more,&next_pc,error);
        printf("  row 0x%llx: cfa reg %llu offset %lld"
            "  ra-state %llu (%s)",
            (unsigned long long)row_pc,
            (unsigned long long)regnum,
            (long long)offset,
            (unsigned long long)ra.state,
            state_name(ra.state));
        if (ra.state == 2) {
            printf(" signing-pc 0x%llx",
                (unsigned long long)ra.signing_pc);
        }
        printf(" key %s\n",ra.b_key?"B":"A");
        if (!more || next_pc == pc) {
            break;
        }
        pc = next_pc;
    }
}

int
main(int argc,char **argv)
{
    Dwarf_Debug     dbg = 0;
    Dwarf_Error     error = 0;
    Dwarf_Cie      *cies = 0;
    Dwarf_Fde      *fdes = 0;
    Dwarf_Signed    cie_count = 0;
    Dwarf_Signed    fde_count = 0;
    Dwarf_Signed    i = 0;
    int             fd = -1;
    const char     *path = 0;
    int             res = DW_DLV_ERROR;

    if (argc != 2) {
        printf("usage: %s <object-with-debug_frame-or-eh_frame>\n",
            argv[0]);
        return 1;
    }
    path = argv[1];
    fd = open(path,O_RDONLY|O_BINARY);
    if (fd < 0) {
        printf("cannot open %s\n",path);
        return 1;
    }
    res = dwarf_init_b(fd,DW_GROUPNUMBER_ANY,0,0,&dbg,&error);
    if (res != DW_DLV_OK) {
        printf("dwarf_init_b failed: %s\n",dwarf_errmsg(error));
        return 1;
    }
    /*  Try .debug_frame first, then GNU .eh_frame. */
    res = dwarf_get_fde_list(dbg,&cies,&cie_count,
        &fdes,&fde_count,&error);
    if (res != DW_DLV_OK) {
        if (error) {
            dwarf_dealloc_error(dbg,error);
            error = 0;
        }
        res = dwarf_get_fde_list_eh(dbg,&cies,&cie_count,
            &fdes,&fde_count,&error);
    }
    if (res != DW_DLV_OK) {
        printf("no frame section found\n");
        dwarf_finish(dbg);
        return 1;
    }
    printf("%lld CIE(s), %lld FDE(s)\n",
        (long long)cie_count,(long long)fde_count);
    for (i = 0; i < fde_count; ++i) {
        print_fde_rows(fdes[i],&error);
    }
    dwarf_dealloc_fde_cie_list(dbg,cies,cie_count,
        fdes,fde_count);
    dwarf_finish(dbg);
    close(fd);
    return 0;
}
