/*
  Copyright (C) 2026 David Anderson. All Rights Reserved.

  This program is free software; you can redistribute it
  and/or modify it under the terms of version 2.1 of the
  GNU Lesser General Public License as published by the Free
  Software Foundation.

  This program is distributed in the hope that it would be
  useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.

  Further, this software is distributed without any warranty
  that it is free of the rightful claim of any third person
  regarding infringement or the like.  Any license provided
  herein, whether implied or otherwise, applies only to this
  software file.  Patent licenses, if any, provided herein
  do not apply to combinations of this program with other
  software, or any other product whatsoever.

  You should have received a copy of the GNU Lesser General
  Public License along with this program; if not, write the
  Free Software Foundation, Inc., 51 Franklin Street - Fifth
  Floor, Boston MA 02110-1301, USA.

*/

/*  Support for the AADWARF64 2026Q3 AArch64 return address
    signing state recorded in .debug_frame and .eh_frame.

    This is descriptive metadata only.  libdwarf does not
    authenticate addresses and does not perform stack
    unwinding.  The state is accumulated by the same CFI
    row interpreter used for the CFA and register rules
    (see dwarf_frame_cfa_read.c) and memoized on the
    Dwarf_Fde alongside those rows.
*/

#include <config.h>

#include <stdlib.h> /* calloc() free() */
#include <string.h> /* memset() */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#ifdef HAVE_STDINT_H
#include <stdint.h> /* uintptr_t */
#endif /* HAVE_STDINT_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_frame.h"
#include "dwarf_elf_defines.h"

/*  Mach-O cputype for arm64.  */
#define DW_RA_CPU_TYPE_ARM64 0x0100000c

/*  Is this object an AArch64 object?  Only such objects give
    the 0x2b/0x2c/0x2d instructions their AADWARF64 2026Q3
    return address signing meanings.  For all other objects
    0x2d keeps its GNU window_save meaning and the other
    two opcodes remain unknown.

    The machine value comes from the object header (an Elf
    e_machine or a Mach-O cputype).  The AArch64 Elf value and
    the arm64 Mach-O cputype do not collide with any other
    architecture libdwarf recognizes. */
Dwarf_Bool
_dwarf_object_is_aarch64(Dwarf_Debug dbg)
{
    if (!dbg) {
        return FALSE;
    }
    if (dbg->de_obj_machine == EM_AARCH64) {
        return TRUE;
    }
    if (dbg->de_obj_machine == DW_RA_CPU_TYPE_ARM64) {
        return TRUE;
    }
    return FALSE;
}

/*  Return the AArch64 return address signing state for the
    frame row containing pc_requested.

    The row model, the CIE initial table and the memoization
    on the Dwarf_Fde are exactly those used by
    dwarf_get_fde_info_for_reg3_c() and friends, so the
    signing state is initialized, advanced and cached
    together with the CFA and register rules.

    Non-AArch64 objects return DW_DLV_NO_ENTRY.  */
int
dwarf_get_fde_ra_state(Dwarf_Fde fde,
    Dwarf_Addr       pc_requested,
    Dwarf_Ra_State * ra_state,
    Dwarf_Addr     * row_pc,
    Dwarf_Bool     * has_more_rows,
    Dwarf_Addr     * subsequent_pc,
    Dwarf_Error    * error)
{
    struct Dwarf_Frame_s *fde_frame_table = 0;
    Dwarf_Debug            dbg = 0;
    Dwarf_Unsigned         table_real_data_size = 0;
    int                    res = DW_DLV_ERROR;

    FDE_NULL_CHECKS_AND_SET_DBG(fde, dbg);
    if (!ra_state) {
        _dwarf_error_string(dbg, error, DW_DLE_DEBUGFRAME_ERROR,
            "DW_DLE_DEBUGFRAME_ERROR: "
            "dwarf_get_fde_ra_state() ra_state argument NULL");
        return DW_DLV_ERROR;
    }
    if (!_dwarf_object_is_aarch64(dbg)) {
        return DW_DLV_NO_ENTRY;
    }
    res = _dwarf_validate_register_numbers(dbg,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    fde_frame_table = &fde->fd_fde_frame_table;
    table_real_data_size = dbg->de_frame_reg_rules_entry_count;
    /*  The cached row is reusable only if it is for the same pc
        and it still owns a live register table.  The all-rows
        and iterator calls temporarily substitute a caller-owned
        register table and then release it, so we must not keep
        relying on that one. */
    if (!fde->fd_have_fde_frame_tab  ||
        fde->fd_fde_pc_requested != pc_requested ||
        !fde_frame_table->fr_regtable ||
        !fde_frame_table->fr_owns_regtable) {
        if (fde->fd_have_fde_frame_tab) {
            /*  The pc_requested changed, so restart the
                calculation from the cie initial table and
                the first byte of fde instructions. */
            _dwarf_empty_frame_table(fde_frame_table);
            fde->fd_have_fde_frame_tab = FALSE;
        }
        res = _dwarf_initialize_frame_table(dbg, fde_frame_table,
            table_real_data_size,NULL,error);
        if (res != DW_DLV_OK) {
            return res;
        }
    }
    /*  This runs the same instruction interpreter used by
        the ordinary row queries and stops at the row
        containing pc_requested. */
    res = _dwarf_get_fde_info_for_a_pc_row(fde,
        pc_requested, fde_frame_table,
        dbg->de_frame_cfa_col_number,
        has_more_rows,subsequent_pc,
        NULL /* no iterator data */,
        error);
    if (res != DW_DLV_OK) {
        _dwarf_empty_frame_table(fde_frame_table);
        fde->fd_have_fde_frame_tab = FALSE;
        return res;
    }
    if (row_pc) {
        *row_pc = fde_frame_table->fr_loc;
    }
    /*  Copy the signing state before anything else can
        empty the cached row. */
    *ra_state = fde_frame_table->fr_ra_state;
    fde->fd_have_fde_frame_tab = TRUE;
    /*  Memoized to avoid recalculation! */
    fde->fd_fde_pc_requested = pc_requested;
    return DW_DLV_OK;
}
