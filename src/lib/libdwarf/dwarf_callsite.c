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

/*  Owned DWARF5 call site snapshots and tail call chain queries.

    Everything exposed through libdwarf.h is copied into
    malloc-ed storage owned by the snapshot or by a query
    result. Neither the snapshot nor a query result refers to
    the Dwarf_Debug after creation, so both survive
    dwarf_finish().

    The implementation reads debug information only. It never
    executes target instructions and never inspects a process.

    Only the following expression operations are interpreted:
        DW_OP_constu, DW_OP_consts
        DW_OP_entry_value around a single register
        DW_OP_plus, DW_OP_minus
        a trailing DW_OP_stack_value
    A parameter location must be a single DW_OP_regN or
    DW_OP_regx. Anything else is reported as unsupported, with
    the original bytes preserved.
*/

#include <config.h>

#include <limits.h>  /* LLONG_MAX LLONG_MIN */
#include <stdio.h>   /* snprintf() */
#include <stdlib.h>  /* calloc() free() malloc() realloc() */
#include <string.h>  /* memcpy() memset() strlen() */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"

#ifndef LLONG_MAX
#define LLONG_MAX 9223372036854775807LL
#endif
#ifndef LLONG_MIN
#define LLONG_MIN (-LLONG_MAX - 1LL)
#endif

#define CS_MAX_STACK 64
#define CS_MAX_EDGES 4096
#define CS_MAX_CHAINS 16384

/*  Internal expression representation. Value kinds mirror the
    public enum but are kept local so the public enum does not
    leak into internal state.  */
enum cs_val_kind {
    CS_VAL_KNOWN = 0,
    CS_VAL_SYMBOLIC = 1,
    CS_VAL_UNKNOWN = 2
};

struct cs_val {
    enum cs_val_kind cv_kind;
    Dwarf_Signed cv_value;
    Dwarf_Callsite_Die_Identity cv_source;
    Dwarf_Unsigned cv_reg;
    Dwarf_Signed cv_addend;
    enum Dwarf_Callsite_Unknown_Reason cv_reason;
};

/*  One DW_AT_call_value expression as read from the object.  */
struct cs_expr {
    unsigned char *ce_bytes;
    Dwarf_Unsigned ce_length;
    Dwarf_Off ce_offset;
    Dwarf_Off ce_attr_offset;
    Dwarf_Bool ce_present;
};

/*  Location state of a call_site_parameter.  */
enum cs_loc_state {
    CS_LOC_MISSING = 0,
    CS_LOC_SINGLE_REG = 1,
    CS_LOC_UNSUPPORTED = 2
};

struct cs_param {
    enum cs_loc_state cp_loc_state;
    Dwarf_Unsigned cp_reg;
    struct cs_expr cp_value;
};

struct cs_call {
    Dwarf_Off cc_call_site_offset;
    Dwarf_Addr cc_return_pc;
    Dwarf_Bool cc_has_return_pc;
    Dwarf_Bool cc_is_tail;
    Dwarf_Bool cc_origin_known;
    Dwarf_Callsite_Die_Identity cc_origin;
    enum Dwarf_Callsite_Unknown_Reason cc_origin_reason;
    Dwarf_Off cc_origin_attr_offset;
    struct cs_param *cc_params;
    size_t cc_param_count;
    size_t cc_param_cap;
};

struct cs_fn {
    Dwarf_Callsite_Die_Identity cf_id;
    char *cf_name;
    Dwarf_Bool cf_has_all_tail;
    struct cs_call *cf_calls;
    size_t cf_call_count;
    size_t cf_call_cap;
};

struct Dwarf_Callsite_Snapshot_s {
    struct cs_fn *cs_fns;
    size_t cs_fn_count;
    size_t cs_fn_cap;
};

/*  ---- query result internals ---- */

struct q_param {
    Dwarf_Callsite_Param qp_pub;
    unsigned char *qp_bytes;
    Dwarf_Unsigned qp_length;
};

struct q_edge {
    Dwarf_Callsite_Edge qe_pub;
    struct q_param *qe_params;
    size_t qe_param_count;
};

struct q_chain {
    enum Dwarf_Callsite_Chain_Kind qc_kind;
    struct q_edge *qc_edges;
    size_t qc_edge_count;
};

struct q_call {
    Dwarf_Callsite_Call qk_pub;
    struct q_chain *qk_chains;
    size_t qk_chain_count;
    size_t qk_chain_cap;
};

struct Dwarf_Callsite_Query_s {
    struct q_call *cq_calls;
    size_t cq_call_count;
};

/*  Chain builder state.  */
struct q_build {
    struct Dwarf_Callsite_Snapshot_s *qb_snap;
    struct q_call *qb_call;
    struct q_edge *qb_edges;
    size_t qb_edge_count;
    size_t qb_edge_cap;
    Dwarf_Callsite_Die_Identity *qb_path;
    size_t qb_path_count;
    size_t qb_path_cap;
    int qb_failed;
};

/*  ---- small allocation helpers ---- */

static void *
cs_alloc(size_t bytes)
{
    if (!bytes) {
        bytes = 1;
    }
    return calloc(1,bytes);
}

static int
cs_grow(void **array,size_t elem_size,size_t *cap,size_t need)
{
    size_t newcap = 0;
    void *fresh = 0;

    if (need <= *cap) {
        return 1;
    }
    newcap = *cap ? *cap * 2 : 8;
    while (newcap < need) {
        if (newcap > ((size_t)-1)/2) {
            return 0;
        }
        newcap *= 2;
    }
    fresh = realloc(*array,newcap * elem_size);
    if (!fresh) {
        return 0;
    }
    memset((char *)fresh + (*cap * elem_size),0,
        (newcap - *cap) * elem_size);
    *array = fresh;
    *cap = newcap;
    return 1;
}

static char *
cs_strdup(const char *in)
{
    size_t len = 0;
    char *out = 0;

    if (!in) {
        return 0;
    }
    len = strlen(in) + 1;
    out = (char *)malloc(len);
    if (!out) {
        return 0;
    }
    memcpy(out,in,len);
    return out;
}

static void
cs_expr_clear(struct cs_expr *ex)
{
    if (ex->ce_bytes) {
        free(ex->ce_bytes);
    }
    memset(ex,0,sizeof(*ex));
}

static void
cs_call_clear(struct cs_call *call)
{
    size_t i = 0;

    for (i = 0; i < call->cc_param_count; ++i) {
        cs_expr_clear(&call->cc_params[i].cp_value);
    }
    free(call->cc_params);
    memset(call,0,sizeof(*call));
}

static void
cs_fn_clear(struct cs_fn *fn)
{
    size_t i = 0;

    free(fn->cf_name);
    for (i = 0; i < fn->cf_call_count; ++i) {
        cs_call_clear(&fn->cf_calls[i]);
    }
    free(fn->cf_calls);
    memset(fn,0,sizeof(*fn));
}

static void
cs_snapshot_free(struct Dwarf_Callsite_Snapshot_s *snap)
{
    size_t i = 0;

    if (!snap) {
        return;
    }
    for (i = 0; i < snap->cs_fn_count; ++i) {
        cs_fn_clear(&snap->cs_fns[i]);
    }
    free(snap->cs_fns);
    free(snap);
}

static void
q_param_clear(struct q_param *p)
{
    free(p->qp_bytes);
    memset(p,0,sizeof(*p));
}

static void
q_edge_clear(struct q_edge *e)
{
    size_t i = 0;

    for (i = 0; i < e->qe_param_count; ++i) {
        q_param_clear(&e->qe_params[i]);
    }
    free(e->qe_params);
    memset(e,0,sizeof(*e));
}

static void
q_chain_clear(struct q_chain *c)
{
    size_t i = 0;

    for (i = 0; i < c->qc_edge_count; ++i) {
        q_edge_clear(&c->qc_edges[i]);
    }
    free(c->qc_edges);
    memset(c,0,sizeof(*c));
}

static void
q_query_free(struct Dwarf_Callsite_Query_s *q)
{
    size_t i = 0;
    size_t j = 0;

    if (!q) {
        return;
    }
    for (i = 0; i < q->cq_call_count; ++i) {
        struct q_call *call = &q->cq_calls[i];
        for (j = 0; j < call->qk_chain_count; ++j) {
            q_chain_clear(&call->qk_chains[j]);
        }
        free(call->qk_chains);
    }
    free(q->cq_calls);
    free(q);
}

/*  ---- identity helpers ---- */

static int
cs_same_identity(const Dwarf_Callsite_Die_Identity *a,
    const Dwarf_Callsite_Die_Identity *b)
{
    return a->dci_cu_header_offset == b->dci_cu_header_offset &&
        a->dci_die_offset == b->dci_die_offset;
}

/*  The Dwarf_Debug that owns a DIE, so locally discarded errors are
    released against the owning arena instead of the no-dbg list.  */
static Dwarf_Debug
cs_die_dbg(Dwarf_Die die)
{
    if (!die || !die->di_cu_context) {
        return 0;
    }
    return die->di_cu_context->cc_dbg;
}

static int
cs_identity_of_die(Dwarf_Die die,
    Dwarf_Callsite_Die_Identity *id,
    Dwarf_Error *error)
{
    int res = 0;
    Dwarf_Off cu_header = 0;
    Dwarf_Off cu_length = 0;
    Dwarf_Off die_offset = 0;

    if (!die || !id) {
        _dwarf_error(0,error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    res = dwarf_dieoffset(die,&die_offset,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    res = dwarf_die_CU_offset_range(die,&cu_header,&cu_length,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    id->dci_cu_header_offset = cu_header;
    id->dci_die_offset = die_offset;
    return DW_DLV_OK;
}

/*  Snapshot function list is built in .debug_info order so the
    global DIE offsets are strictly increasing. Binary search on
    the DIE offset, then verify the CU.  */
static struct cs_fn *
cs_find_fn(struct Dwarf_Callsite_Snapshot_s *snap,
    const Dwarf_Callsite_Die_Identity *id)
{
    size_t lo = 0;
    size_t hi = 0;

    if (!snap || !id || !snap->cs_fn_count) {
        return 0;
    }
    lo = 0;
    hi = snap->cs_fn_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo)/2;
        struct cs_fn *fn = &snap->cs_fns[mid];

        if (fn->cf_id.dci_die_offset < id->dci_die_offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= snap->cs_fn_count) {
        return 0;
    }
    if (!cs_same_identity(&snap->cs_fns[lo].cf_id,id)) {
        return 0;
    }
    return &snap->cs_fns[lo];
}

/*  ---- little endian DWARF expression readers ---- */

static int
cs_uleb(const unsigned char *bytes,size_t length,size_t *pos,
    Dwarf_Unsigned *out)
{
    Dwarf_Unsigned value = 0;
    unsigned shift = 0;
    size_t i = *pos;

    for (;;) {
        unsigned char byte = 0;

        if (i >= length || shift >= 64) {
            return 0;
        }
        byte = bytes[i++];
        value |= ((Dwarf_Unsigned)(byte & 0x7f)) << shift;
        if (!(byte & 0x80)) {
            break;
        }
        shift += 7;
    }
    *pos = i;
    *out = value;
    return 1;
}

static int
cs_sleb(const unsigned char *bytes,size_t length,size_t *pos,
    Dwarf_Signed *out)
{
    Dwarf_Unsigned value = 0;
    unsigned shift = 0;
    size_t i = *pos;
    unsigned char byte = 0;

    for (;;) {
        if (i >= length || shift >= 64) {
            return 0;
        }
        byte = bytes[i++];
        value |= ((Dwarf_Unsigned)(byte & 0x7f)) << shift;
        shift += 7;
        if (!(byte & 0x80)) {
            break;
        }
    }
    if (shift < 64 && (byte & 0x40)) {
        value |= ((Dwarf_Unsigned)-1) << shift;
    }
    *pos = i;
    *out = (Dwarf_Signed)value;
    return 1;
}

/*  ---- location parsing ---- */

static int
cs_single_register(const unsigned char *bytes,size_t length,
    Dwarf_Unsigned *reg)
{
    if (length == 1 && bytes[0] >= DW_OP_reg0 &&
        bytes[0] <= DW_OP_reg31) {
        *reg = bytes[0] - DW_OP_reg0;
        return 1;
    }
    if (length >= 2 && bytes[0] == DW_OP_regx) {
        size_t pos = 1;
        Dwarf_Unsigned value = 0;

        if (cs_uleb(bytes,length,&pos,&value) && pos == length) {
            *reg = value;
            return 1;
        }
    }
    return 0;
}

/*  ---- signed 64 bit checked arithmetic ---- */

static int
cs_add(Dwarf_Signed a,Dwarf_Signed b,Dwarf_Signed *out)
{
    if (b > 0 && a > LLONG_MAX - b) {
        return 0;
    }
    if (b < 0 && a < LLONG_MIN - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static int
cs_sub(Dwarf_Signed a,Dwarf_Signed b,Dwarf_Signed *out)
{
    if (b > 0 && a < LLONG_MIN + b) {
        return 0;
    }
    if (b < 0 && a > LLONG_MAX + b) {
        return 0;
    }
    *out = a - b;
    return 1;
}

/*  ---- expression evaluation ---- */

static void
cs_val_unknown(struct cs_val *v,
    enum Dwarf_Callsite_Unknown_Reason reason)
{
    memset(v,0,sizeof(*v));
    v->cv_kind = CS_VAL_UNKNOWN;
    v->cv_reason = reason;
}

static void
cs_val_known(struct cs_val *v,Dwarf_Signed value)
{
    memset(v,0,sizeof(*v));
    v->cv_kind = CS_VAL_KNOWN;
    v->cv_value = value;
}

static void
cs_val_symbolic(struct cs_val *v,
    const Dwarf_Callsite_Die_Identity *source,
    Dwarf_Unsigned reg,Dwarf_Signed addend)
{
    memset(v,0,sizeof(*v));
    v->cv_kind = CS_VAL_SYMBOLIC;
    v->cv_source = *source;
    v->cv_reg = reg;
    v->cv_addend = addend;
}

/*  Resolve DW_OP_entry_value at the entry of the forwarding
    function. The previous edge describes what was actually
    passed in. When that is not known the source symbol is kept
    instead of pretending to know a number.  */
static void
cs_entry_value(const Dwarf_Callsite_Die_Identity *owner,
    Dwarf_Unsigned reg,
    const struct q_edge *previous,
    struct cs_val *out)
{
    size_t i = 0;

    if (previous) {
        for (i = 0; i < previous->qe_param_count; ++i) {
            const Dwarf_Callsite_Param *p =
                &previous->qe_params[i].qp_pub;

            if (!p->csp_register_known ||
                p->csp_register != reg) {
                continue;
            }
            if (p->csp_value_kind == DW_CS_VALUE_KNOWN) {
                cs_val_known(out,p->csp_value);
                return;
            }
            if (p->csp_value_kind == DW_CS_VALUE_SYMBOLIC) {
                cs_val_symbolic(out,&p->csp_source_function,
                    p->csp_source_register,p->csp_source_addend);
                return;
            }
            /*  The incoming argument is declared but its value
                is not computable. Retain the source symbol. */
            cs_val_symbolic(out,owner,reg,0);
            return;
        }
    }
    /*  No incoming declaration for this register. Retain the
        source symbol.  */
    cs_val_symbolic(out,owner,reg,0);
}

static int
cs_combine(struct cs_val *a,struct cs_val *b,int is_add,
    struct cs_val *out)
{
    if (a->cv_kind == CS_VAL_UNKNOWN) {
        *out = *a;
        return 1;
    }
    if (b->cv_kind == CS_VAL_UNKNOWN) {
        *out = *b;
        return 1;
    }
    if (a->cv_kind == CS_VAL_KNOWN && b->cv_kind == CS_VAL_KNOWN) {
        Dwarf_Signed r = 0;
        int ok = is_add? cs_add(a->cv_value,b->cv_value,&r):
            cs_sub(a->cv_value,b->cv_value,&r);
        if (!ok) {
            cs_val_unknown(out,DW_CS_UNKNOWN_OVERFLOW);
            return 1;
        }
        cs_val_known(out,r);
        return 1;
    }
    if (a->cv_kind == CS_VAL_SYMBOLIC &&
        b->cv_kind == CS_VAL_KNOWN) {
        Dwarf_Signed r = 0;
        int ok = is_add? cs_add(a->cv_addend,b->cv_value,&r):
            cs_sub(a->cv_addend,b->cv_value,&r);

        if (!ok) {
            cs_val_unknown(out,DW_CS_UNKNOWN_OVERFLOW);
            return 1;
        }
        cs_val_symbolic(out,&a->cv_source,a->cv_reg,r);
        return 1;
    }
    if (a->cv_kind == CS_VAL_KNOWN &&
        b->cv_kind == CS_VAL_SYMBOLIC && is_add) {
        Dwarf_Signed r = 0;

        if (!cs_add(a->cv_value,b->cv_addend,&r)) {
            cs_val_unknown(out,DW_CS_UNKNOWN_OVERFLOW);
            return 1;
        }
        cs_val_symbolic(out,&b->cv_source,b->cv_reg,r);
        return 1;
    }
    if (a->cv_kind == CS_VAL_SYMBOLIC &&
        b->cv_kind == CS_VAL_SYMBOLIC &&
        cs_same_identity(&a->cv_source,&b->cv_source) &&
        a->cv_reg == b->cv_reg) {
        Dwarf_Signed r = 0;

        if (is_add) {
            if (!cs_add(a->cv_addend,b->cv_addend,&r)) {
                cs_val_unknown(out,DW_CS_UNKNOWN_OVERFLOW);
                return 1;
            }
            cs_val_symbolic(out,&a->cv_source,a->cv_reg,r);
        } else {
            if (!cs_sub(a->cv_addend,b->cv_addend,&r)) {
                cs_val_unknown(out,DW_CS_UNKNOWN_OVERFLOW);
                return 1;
            }
            cs_val_known(out,r);
        }
        return 1;
    }
    /*  Known minus a symbolic source, or two different
        symbols, cannot be represented.  */
    (void)out;
    return 0;
}

static int
cs_entry_value_parse(const unsigned char *bytes,size_t length,
    size_t *pos,const Dwarf_Callsite_Die_Identity *owner,
    const struct q_edge *previous,struct cs_val *out)
{
    Dwarf_Unsigned blocklen = 0;
    size_t inner_end = 0;
    size_t inner = 0;
    Dwarf_Unsigned reg = 0;

    if (!cs_uleb(bytes,length,pos,&blocklen)) {
        return 0;
    }
    if (blocklen > length - *pos) {
        return 0;
    }
    inner = *pos;
    inner_end = inner + (size_t)blocklen;
    if (!cs_single_register(bytes + inner,inner_end - inner,&reg)) {
        return 0;
    }
    *pos = inner_end;
    cs_entry_value(owner,reg,previous,out);
    return 1;
}

/*  Evaluate one DW_AT_call_value expression.  Unsupported or
    overflowed expressions set the value to UNKNOWN with a
    reason, but the bytes are retained by the caller.  */
static void
cs_eval_expr(const Dwarf_Callsite_Die_Identity *owner,
    const struct q_edge *previous,
    const unsigned char *bytes,size_t length,struct cs_val *out)
{
    struct cs_val stack[CS_MAX_STACK];
    size_t depth = 0;
    size_t pos = 0;

    memset(stack,0,sizeof(stack));
    memset(out,0,sizeof(*out));
    if (!bytes || !length) {
        cs_val_unknown(out,DW_CS_UNKNOWN_MISSING_VALUE);
        return;
    }
    while (pos < length) {
        unsigned char op = bytes[pos++];

        switch (op) {
        case DW_OP_constu: {
            Dwarf_Unsigned uvalue = 0;
            struct cs_val v;

            if (!cs_uleb(bytes,length,&pos,&uvalue) ||
                depth >= CS_MAX_STACK) {
                cs_val_unknown(out,
                    DW_CS_UNKNOWN_UNSUPPORTED_EXPRESSION);
                return;
            }
            if (uvalue > (Dwarf_Unsigned)LLONG_MAX) {
                cs_val_unknown(out,DW_CS_UNKNOWN_OVERFLOW);
                return;
            }
            cs_val_known(&v,(Dwarf_Signed)uvalue);
            stack[depth++] = v;
            break;
            }
        case DW_OP_consts: {
            Dwarf_Signed svalue = 0;
            struct cs_val v;

            if (!cs_sleb(bytes,length,&pos,&svalue) ||
                depth >= CS_MAX_STACK) {
                cs_val_unknown(out,
                    DW_CS_UNKNOWN_UNSUPPORTED_EXPRESSION);
                return;
            }
            cs_val_known(&v,svalue);
            stack[depth++] = v;
            break;
            }
        case DW_OP_entry_value: {
            struct cs_val v;

            if (depth >= CS_MAX_STACK ||
                !cs_entry_value_parse(bytes,length,&pos,owner,
                    previous,&v)) {
                cs_val_unknown(out,
                    DW_CS_UNKNOWN_UNSUPPORTED_EXPRESSION);
                return;
            }
            stack[depth++] = v;
            break;
            }
        case DW_OP_plus:
        case DW_OP_minus: {
            struct cs_val a;
            struct cs_val b;
            struct cs_val v;

            if (depth < 2) {
                cs_val_unknown(out,
                    DW_CS_UNKNOWN_UNSUPPORTED_EXPRESSION);
                return;
            }
            b = stack[--depth];
            a = stack[--depth];
            if (!cs_combine(&a,&b,op == DW_OP_plus,&v)) {
                cs_val_unknown(out,
                    DW_CS_UNKNOWN_UNSUPPORTED_EXPRESSION);
                return;
            }
            stack[depth++] = v;
            break;
            }
        case DW_OP_stack_value:
            /*  Only meaningful as the final operation.  */
            if (pos != length) {
                cs_val_unknown(out,
                    DW_CS_UNKNOWN_UNSUPPORTED_EXPRESSION);
                return;
            }
            break;
        default:
            cs_val_unknown(out,
                DW_CS_UNKNOWN_UNSUPPORTED_EXPRESSION);
            return;
        }
    }
    if (depth != 1) {
        cs_val_unknown(out,
            DW_CS_UNKNOWN_UNSUPPORTED_EXPRESSION);
        return;
    }
    *out = stack[0];
}

/*  ---- snapshot building ---- */

static int
cs_add_function(struct Dwarf_Callsite_Snapshot_s *snap,
    Dwarf_Die die,size_t *index,Dwarf_Error *error)
{
    struct cs_fn *fn = 0;
    Dwarf_Attribute attr = 0;
    Dwarf_Error local_error = 0;
    Dwarf_Debug dbg = cs_die_dbg(die);
    char *name = 0;
    int res = 0;
    Dwarf_Bool flag = FALSE;

    if (!cs_grow((void **)&snap->cs_fns,sizeof(struct cs_fn),
        &snap->cs_fn_cap,snap->cs_fn_count + 1)) {
        _dwarf_error(0,error,DW_DLE_MAF);
        return DW_DLV_ERROR;
    }
    fn = &snap->cs_fns[snap->cs_fn_count];
    memset(fn,0,sizeof(*fn));
    res = cs_identity_of_die(die,&fn->cf_id,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    res = dwarf_attr(die,DW_AT_name,&attr,&local_error);
    if (res == DW_DLV_OK) {
        if (dwarf_formstring(attr,&name,&local_error) == DW_DLV_OK) {
            fn->cf_name = cs_strdup(name);
            if (!fn->cf_name) {
                dwarf_dealloc_attribute(attr);
                _dwarf_error(0,error,DW_DLE_MAF);
                return DW_DLV_ERROR;
            }
        }
        dwarf_dealloc_attribute(attr);
        attr = 0;
    } else if (local_error) {
        dwarf_dealloc_error(dbg,local_error);
        local_error = 0;
    }
    res = dwarf_attr(die,DW_AT_call_all_tail_calls,&attr,&local_error);
    if (res == DW_DLV_OK) {
        if (dwarf_formflag(attr,&flag,&local_error) == DW_DLV_OK &&
            flag) {
            fn->cf_has_all_tail = TRUE;
        }
        dwarf_dealloc_attribute(attr);
        attr = 0;
    } else if (local_error) {
        dwarf_dealloc_error(dbg,local_error);
        local_error = 0;
    }
    *index = snap->cs_fn_count;
    ++snap->cs_fn_count;
    return DW_DLV_OK;
}

/*  Read one block/exprloc attribute into owned bytes.  */
static int
cs_copy_expr(Dwarf_Die die,Dwarf_Half attrnum,
    struct cs_expr *expr,Dwarf_Error *error)
{
    Dwarf_Attribute attr = 0;
    Dwarf_Error local_error = 0;
    Dwarf_Debug dbg = cs_die_dbg(die);
    Dwarf_Block *block = 0;
    Dwarf_Off attr_offset = 0;
    int res = 0;

    memset(expr,0,sizeof(*expr));
    res = dwarf_attr(die,attrnum,&attr,&local_error);
    if (res == DW_DLV_NO_ENTRY) {
        if (local_error) {
            dwarf_dealloc_error(dbg,local_error);
        }
        return DW_DLV_NO_ENTRY;
    }
    if (res != DW_DLV_OK) {
        if (local_error) {
            dwarf_dealloc_error(dbg,local_error);
        }
        return res;
    }
    res = dwarf_formblock(attr,&block,&local_error);
    if (res == DW_DLV_OK) {
        if (dwarf_attr_offset(die,attr,&attr_offset,&local_error) ==
            DW_DLV_OK) {
            expr->ce_attr_offset = attr_offset;
        }
        if (block->bl_len) {
            expr->ce_bytes =
                (unsigned char *)malloc((size_t)block->bl_len);
            if (!expr->ce_bytes) {
                dwarf_dealloc(attr->ar_dbg,block,DW_DLA_BLOCK);
                dwarf_dealloc_attribute(attr);
                _dwarf_error(0,error,DW_DLE_MAF);
                return DW_DLV_ERROR;
            }
            memcpy(expr->ce_bytes,block->bl_data,
                (size_t)block->bl_len);
        }
        expr->ce_length = block->bl_len;
        expr->ce_offset = block->bl_section_offset;
        expr->ce_present = TRUE;
        dwarf_dealloc(attr->ar_dbg,block,DW_DLA_BLOCK);
    } else {
        if (local_error) {
            dwarf_dealloc_error(dbg,local_error);
            local_error = 0;
        }
        if (dwarf_attr_offset(die,attr,&attr_offset,&local_error) ==
            DW_DLV_OK) {
            expr->ce_attr_offset = attr_offset;
        }
        expr->ce_present = FALSE;
    }
    if (local_error) {
        dwarf_dealloc_error(dbg,local_error);
    }
    dwarf_dealloc_attribute(attr);
    return DW_DLV_OK;
}

static int
cs_parse_param(Dwarf_Die die,struct cs_param *param,
    Dwarf_Error *error)
{
    Dwarf_Attribute attr = 0;
    Dwarf_Error local_error = 0;
    Dwarf_Debug owner_dbg = cs_die_dbg(die);
    Dwarf_Block *block = 0;
    int res = 0;

    memset(param,0,sizeof(*param));
    param->cp_loc_state = CS_LOC_MISSING;
    res = dwarf_attr(die,DW_AT_location,&attr,&local_error);
    if (res == DW_DLV_OK) {
        int blockres = dwarf_formblock(attr,&block,&local_error);
        Dwarf_Unsigned reg = 0;

        if (blockres == DW_DLV_OK) {
            if (cs_single_register(block->bl_data,
                (size_t)block->bl_len,&reg)) {
                param->cp_loc_state = CS_LOC_SINGLE_REG;
                param->cp_reg = reg;
            } else {
                param->cp_loc_state = CS_LOC_UNSUPPORTED;
            }
            dwarf_dealloc(attr->ar_dbg,block,DW_DLA_BLOCK);
        } else {
            param->cp_loc_state = CS_LOC_UNSUPPORTED;
            if (local_error) {
                dwarf_dealloc_error(owner_dbg,local_error);
                local_error = 0;
            }
        }
        dwarf_dealloc_attribute(attr);
        attr = 0;
    } else if (res == DW_DLV_ERROR) {
        if (local_error) {
            dwarf_dealloc_error(owner_dbg,local_error);
            local_error = 0;
        }
        param->cp_loc_state = CS_LOC_UNSUPPORTED;
    } else if (local_error) {
        dwarf_dealloc_error(owner_dbg,local_error);
        local_error = 0;
    }
    res = cs_copy_expr(die,DW_AT_call_value,&param->cp_value,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    return DW_DLV_OK;
}

static int
cs_add_param(struct cs_call *call,struct cs_param *param,
    Dwarf_Error *error)
{
    if (!cs_grow((void **)&call->cc_params,sizeof(struct cs_param),
        &call->cc_param_cap,call->cc_param_count + 1)) {
        _dwarf_error(0,error,DW_DLE_MAF);
        return DW_DLV_ERROR;
    }
    call->cc_params[call->cc_param_count++] = *param;
    memset(param,0,sizeof(*param));
    return DW_DLV_OK;
}

/*  Resolve DW_AT_call_origin to the identity of a subprogram.
    Returns TRUE only when the target is a subprogram in
    .debug_info.  */
static int
cs_resolve_origin(Dwarf_Debug dbg,Dwarf_Off offset,
    Dwarf_Callsite_Die_Identity *id)
{
    Dwarf_Die target = 0;
    Dwarf_Error local_error = 0;
    Dwarf_Half tag = 0;
    int res = 0;
    int ok = 0;

    res = dwarf_offdie_b(dbg,offset,TRUE,&target,&local_error);
    if (res != DW_DLV_OK) {
        if (local_error) {
            dwarf_dealloc_error(dbg,local_error);
        }
        return 0;
    }
    res = dwarf_tag(target,&tag,&local_error);
    if (res == DW_DLV_OK && tag == DW_TAG_subprogram) {
        if (cs_identity_of_die(target,id,&local_error) == DW_DLV_OK) {
            ok = 1;
        }
    }
    if (local_error) {
        dwarf_dealloc_error(dbg,local_error);
    }
    dwarf_dealloc_die(target);
    return ok;
}

static int
cs_parse_call(Dwarf_Debug dbg,Dwarf_Die die,struct cs_call *call,
    Dwarf_Error *error)
{
    Dwarf_Attribute attr = 0;
    Dwarf_Error local_error = 0;
    Dwarf_Off attr_offset = 0;
    Dwarf_Addr pc = 0;
    Dwarf_Bool flag = FALSE;
    int res = 0;

    memset(call,0,sizeof(*call));
    res = dwarf_dieoffset(die,&call->cc_call_site_offset,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    res = dwarf_attr(die,DW_AT_call_return_pc,&attr,&local_error);
    if (res == DW_DLV_OK) {
        if (dwarf_formaddr(attr,&pc,&local_error) == DW_DLV_OK) {
            call->cc_has_return_pc = TRUE;
            call->cc_return_pc = pc;
        }
        dwarf_dealloc_attribute(attr);
        attr = 0;
    } else if (local_error) {
        dwarf_dealloc_error(dbg,local_error);
        local_error = 0;
    }
    res = dwarf_attr(die,DW_AT_call_tail_call,&attr,&local_error);
    if (res == DW_DLV_OK) {
        if (dwarf_formflag(attr,&flag,&local_error) == DW_DLV_OK &&
            flag) {
            call->cc_is_tail = TRUE;
        }
        dwarf_dealloc_attribute(attr);
        attr = 0;
    } else if (local_error) {
        dwarf_dealloc_error(dbg,local_error);
        local_error = 0;
    }

    /*  DW_AT_call_origin first. DW_AT_call_target means the
        callee is only known indirectly.  */
    call->cc_origin_reason = DW_CS_UNKNOWN_UNRESOLVED_ORIGIN;
    res = dwarf_attr(die,DW_AT_call_origin,&attr,&local_error);
    if (res == DW_DLV_OK) {
        Dwarf_Off target_offset = 0;
        Dwarf_Bool is_info = TRUE;
        int refres = 0;

        if (dwarf_attr_offset(die,attr,&attr_offset,&local_error) ==
            DW_DLV_OK) {
            call->cc_origin_attr_offset = attr_offset;
        }
        refres = dwarf_global_formref_b(attr,&target_offset,
            &is_info,&local_error);
        if (refres == DW_DLV_OK && is_info &&
            cs_resolve_origin(dbg,target_offset,
                &call->cc_origin)) {
            call->cc_origin_known = TRUE;
            call->cc_origin_reason = DW_CS_UNKNOWN_NONE;
        } else {
            if (local_error) {
                dwarf_dealloc_error(dbg,local_error);
                local_error = 0;
            }
            call->cc_origin_known = FALSE;
            call->cc_origin_reason =
                DW_CS_UNKNOWN_UNRESOLVED_ORIGIN;
        }
        dwarf_dealloc_attribute(attr);
        attr = 0;
    } else {
        if (local_error) {
            dwarf_dealloc_error(dbg,local_error);
            local_error = 0;
        }
        res = dwarf_attr(die,DW_AT_call_target,&attr,&local_error);
        if (res == DW_DLV_OK) {
            if (dwarf_attr_offset(die,attr,&attr_offset,
                &local_error) == DW_DLV_OK) {
                call->cc_origin_attr_offset = attr_offset;
            }
            call->cc_origin_known = FALSE;
            call->cc_origin_reason =
                DW_CS_UNKNOWN_INDIRECT_TARGET;
            dwarf_dealloc_attribute(attr);
            attr = 0;
        } else if (local_error) {
            dwarf_dealloc_error(dbg,local_error);
            local_error = 0;
        }
    }
    if (local_error) {
        dwarf_dealloc_error(dbg,local_error);
        local_error = 0;
    }

    /*  Children are call_site_parameter DIEs.  */
    {
        Dwarf_Die child = 0;
        int childres = dwarf_child(die,&child,&local_error);

        if (childres == DW_DLV_ERROR) {
            if (local_error) {
                dwarf_dealloc_error(dbg,local_error);
            }
            _dwarf_error(0,error,DW_DLE_IA);
            return DW_DLV_ERROR;
        }
        while (childres == DW_DLV_OK) {
            Dwarf_Half tag = 0;
            int tres = dwarf_tag(child,&tag,&local_error);

            if (tres == DW_DLV_OK &&
                tag == DW_TAG_call_site_parameter) {
                struct cs_param param;
                int pres = cs_parse_param(child,&param,error);

                if (pres != DW_DLV_OK) {
                    dwarf_dealloc_die(child);
                    return pres;
                }
                if (cs_add_param(call,&param,error) !=
                    DW_DLV_OK) {
                    dwarf_dealloc_die(child);
                    return DW_DLV_ERROR;
                }
            }
            {
                Dwarf_Die sibling = 0;
                int sres = dwarf_siblingof_c(child,&sibling,
                    &local_error);

                dwarf_dealloc_die(child);
                child = sibling;
                if (sres == DW_DLV_ERROR) {
                    if (local_error) {
                        dwarf_dealloc_error(dbg,local_error);
                    }
                    _dwarf_error(0,error,DW_DLE_IA);
                    return DW_DLV_ERROR;
                }
                childres = sres;
            }
        }
    }
    return DW_DLV_OK;
}

static int
cs_add_call(struct cs_fn *fn,struct cs_call *call,
    Dwarf_Error *error)
{
    if (!cs_grow((void **)&fn->cf_calls,sizeof(struct cs_call),
        &fn->cf_call_cap,fn->cf_call_count + 1)) {
        _dwarf_error(0,error,DW_DLE_MAF);
        return DW_DLV_ERROR;
    }
    fn->cf_calls[fn->cf_call_count++] = *call;
    memset(call,0,sizeof(*call));
    return DW_DLV_OK;
}

static int cs_walk_die(Dwarf_Debug dbg,Dwarf_Die die,
    long current_fn,struct Dwarf_Callsite_Snapshot_s *snap,
    Dwarf_Error *error);

static int
cs_walk_children(Dwarf_Debug dbg,Dwarf_Die die,long current_fn,
    struct Dwarf_Callsite_Snapshot_s *snap,Dwarf_Error *error)
{
    Dwarf_Die child = 0;
    Dwarf_Error local_error = 0;
    int res = 0;

    res = dwarf_child(die,&child,&local_error);
    if (res == DW_DLV_ERROR) {
        if (local_error) {
            dwarf_dealloc_error(dbg,local_error);
        }
        _dwarf_error(0,error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    while (res == DW_DLV_OK) {
        int walkres = cs_walk_die(dbg,child,current_fn,snap,error);
        Dwarf_Die sibling = 0;
        int sres = 0;

        if (walkres != DW_DLV_OK) {
            dwarf_dealloc_die(child);
            return walkres;
        }
        sres = dwarf_siblingof_c(child,&sibling,&local_error);
        dwarf_dealloc_die(child);
        child = sibling;
        if (sres == DW_DLV_ERROR) {
            if (local_error) {
                dwarf_dealloc_error(dbg,local_error);
            }
            _dwarf_error(0,error,DW_DLE_IA);
            return DW_DLV_ERROR;
        }
        res = sres;
    }
    return DW_DLV_OK;
}

static int
cs_walk_die(Dwarf_Debug dbg,Dwarf_Die die,long current_fn,
    struct Dwarf_Callsite_Snapshot_s *snap,Dwarf_Error *error)
{
    Dwarf_Half tag = 0;
    Dwarf_Error local_error = 0;
    int res = 0;
    long fn_index = current_fn;

    res = dwarf_tag(die,&tag,&local_error);
    if (res != DW_DLV_OK) {
        if (local_error) {
            dwarf_dealloc_error(dbg,local_error);
        }
        _dwarf_error(0,error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    if (tag == DW_TAG_subprogram) {
        size_t index = 0;

        res = cs_add_function(snap,die,&index,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        fn_index = (long)index;
    } else if (tag == DW_TAG_call_site && fn_index >= 0) {
        struct cs_call call;

        res = cs_parse_call(dbg,die,&call,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        res = cs_add_call(&snap->cs_fns[fn_index],&call,error);
        if (res != DW_DLV_OK) {
            cs_call_clear(&call);
            return res;
        }
    }
    return cs_walk_children(dbg,die,fn_index,snap,error);
}

/*  ---- public snapshot API ---- */

int
dwarf_callsite_die_identity(Dwarf_Die dw_die,
    Dwarf_Callsite_Die_Identity *dw_identity,
    Dwarf_Error *dw_error)
{
    if (!dw_identity) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    memset(dw_identity,0,sizeof(*dw_identity));
    return cs_identity_of_die(dw_die,dw_identity,dw_error);
}

int
dwarf_callsite_snapshot_create(Dwarf_Debug dw_dbg,
    Dwarf_Callsite_Snapshot *dw_snapshot,
    Dwarf_Error *dw_error)
{
    struct Dwarf_Callsite_Snapshot_s *snap = 0;
    Dwarf_Error local_error = 0;
    int res = 0;

    if (!dw_snapshot) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    *dw_snapshot = 0;
    if (!dw_dbg) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    snap = (struct Dwarf_Callsite_Snapshot_s *)
        cs_alloc(sizeof(*snap));
    if (!snap) {
        _dwarf_error(0,dw_error,DW_DLE_MAF);
        return DW_DLV_ERROR;
    }
    for (;;) {
        Dwarf_Die cu_die = 0;
        Dwarf_Unsigned header_length = 0;
        Dwarf_Half version = 0;
        Dwarf_Off abbrev_offset = 0;
        Dwarf_Half address_size = 0;
        Dwarf_Half offset_size = 0;
        Dwarf_Half extension_size = 0;
        Dwarf_Sig8 signature;
        Dwarf_Unsigned typeoffset = 0;
        Dwarf_Unsigned next_offset = 0;
        Dwarf_Half unit_type = 0;

        memset(&signature,0,sizeof(signature));
        res = dwarf_next_cu_header_e(dw_dbg,TRUE,&cu_die,
            &header_length,&version,&abbrev_offset,&address_size,
            &offset_size,&extension_size,&signature,&typeoffset,
            &next_offset,&unit_type,&local_error);
        if (res == DW_DLV_NO_ENTRY) {
            if (local_error) {
                dwarf_dealloc_error(dw_dbg,local_error);
            }
            break;
        }
        if (res != DW_DLV_OK) {
            if (local_error) {
                dwarf_dealloc_error(dw_dbg,local_error);
            } else {
                _dwarf_error(0,dw_error,DW_DLE_IA);
            }
            cs_snapshot_free(snap);
            return DW_DLV_ERROR;
        }
        res = cs_walk_die(dw_dbg,cu_die,-1,snap,&local_error);
        dwarf_dealloc_die(cu_die);
        if (res != DW_DLV_OK) {
            if (local_error) {
                dwarf_dealloc_error(dw_dbg,local_error);
            }
            if (dw_error && !*dw_error) {
                _dwarf_error(0,dw_error,DW_DLE_IA);
            }
            cs_snapshot_free(snap);
            return DW_DLV_ERROR;
        }
    }
    *dw_snapshot = snap;
    return DW_DLV_OK;
}

void
dwarf_callsite_snapshot_destroy(Dwarf_Callsite_Snapshot dw_snapshot)
{
    cs_snapshot_free(dw_snapshot);
}

int
dwarf_callsite_snapshot_function_count(
    Dwarf_Callsite_Snapshot dw_snapshot,
    Dwarf_Unsigned *dw_count,Dwarf_Error *dw_error)
{
    if (!dw_count) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    *dw_count = 0;
    if (!dw_snapshot) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    *dw_count = (Dwarf_Unsigned)dw_snapshot->cs_fn_count;
    return DW_DLV_OK;
}

int
dwarf_callsite_snapshot_function_identity(
    Dwarf_Callsite_Snapshot dw_snapshot,
    Dwarf_Unsigned dw_index,
    Dwarf_Callsite_Die_Identity *dw_identity,Dwarf_Error *dw_error)
{
    if (!dw_snapshot || !dw_identity) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    memset(dw_identity,0,sizeof(*dw_identity));
    if (dw_index >= (Dwarf_Unsigned)dw_snapshot->cs_fn_count) {
        return DW_DLV_NO_ENTRY;
    }
    *dw_identity =
        dw_snapshot->cs_fns[(size_t)dw_index].cf_id;
    return DW_DLV_OK;
}

int
dwarf_callsite_function_name(
    Dwarf_Callsite_Snapshot dw_snapshot,
    const Dwarf_Callsite_Die_Identity *dw_function,
    const char **dw_name,Dwarf_Error *dw_error)
{
    struct cs_fn *fn = 0;

    if (!dw_snapshot || !dw_function || !dw_name) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    *dw_name = 0;
    fn = cs_find_fn(dw_snapshot,dw_function);
    if (!fn) {
        return DW_DLV_NO_ENTRY;
    }
    *dw_name = fn->cf_name;
    return DW_DLV_OK;
}

int
dwarf_callsite_snapshot_function_call_count(
    Dwarf_Callsite_Snapshot dw_snapshot,
    const Dwarf_Callsite_Die_Identity *dw_function,
    Dwarf_Unsigned *dw_count,Dwarf_Error *dw_error)
{
    struct cs_fn *fn = 0;

    if (!dw_snapshot || !dw_function || !dw_count) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    *dw_count = 0;
    fn = cs_find_fn(dw_snapshot,dw_function);
    if (!fn) {
        return DW_DLV_NO_ENTRY;
    }
    *dw_count = (Dwarf_Unsigned)fn->cf_call_count;
    return DW_DLV_OK;
}

int
dwarf_callsite_snapshot_function_call(
    Dwarf_Callsite_Snapshot dw_snapshot,
    const Dwarf_Callsite_Die_Identity *dw_function,
    Dwarf_Unsigned dw_index,
    Dwarf_Off *dw_call_site_offset,
    Dwarf_Addr *dw_call_return_pc,
    Dwarf_Bool *dw_has_return_pc,
    Dwarf_Bool *dw_is_tail_call,
    Dwarf_Error *dw_error)
{
    struct cs_fn *fn = 0;
    struct cs_call *call = 0;

    if (!dw_snapshot || !dw_function) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    if (dw_call_site_offset) {
        *dw_call_site_offset = 0;
    }
    if (dw_call_return_pc) {
        *dw_call_return_pc = 0;
    }
    if (dw_has_return_pc) {
        *dw_has_return_pc = FALSE;
    }
    if (dw_is_tail_call) {
        *dw_is_tail_call = FALSE;
    }
    fn = cs_find_fn(dw_snapshot,dw_function);
    if (!fn) {
        return DW_DLV_NO_ENTRY;
    }
    if (dw_index >= (Dwarf_Unsigned)fn->cf_call_count) {
        return DW_DLV_NO_ENTRY;
    }
    call = &fn->cf_calls[(size_t)dw_index];
    if (dw_call_site_offset) {
        *dw_call_site_offset = call->cc_call_site_offset;
    }
    if (dw_call_return_pc) {
        *dw_call_return_pc = call->cc_return_pc;
    }
    if (dw_has_return_pc) {
        *dw_has_return_pc = call->cc_has_return_pc;
    }
    if (dw_is_tail_call) {
        *dw_is_tail_call = call->cc_is_tail;
    }
    return DW_DLV_OK;
}

/*  ---- query construction ---- */

static int
qb_add_chain(struct q_build *qb,
    enum Dwarf_Callsite_Chain_Kind kind)
{
    struct q_call *call = qb->qb_call;
    struct q_chain *chain = 0;
    size_t i = 0;

    if (call->qk_chain_count >= CS_MAX_CHAINS) {
        qb->qb_failed = 1;
        return 0;
    }
    if (!cs_grow((void **)&call->qk_chains,sizeof(struct q_chain),
        &call->qk_chain_cap,call->qk_chain_count + 1)) {
        qb->qb_failed = 1;
        return 0;
    }
    chain = &call->qk_chains[call->qk_chain_count];
    memset(chain,0,sizeof(*chain));
    chain->qc_kind = kind;
    /*  Count the chain immediately so a later allocation failure
        still frees whatever was partly filled in.  */
    ++call->qk_chain_count;
    if (qb->qb_edge_count) {
        chain->qc_edges = (struct q_edge *)cs_alloc(
            qb->qb_edge_count * sizeof(struct q_edge));
        if (!chain->qc_edges) {
            qb->qb_failed = 1;
            return 0;
        }
        chain->qc_edge_count = qb->qb_edge_count;
        for (i = 0; i < qb->qb_edge_count; ++i) {
            struct q_edge *src = &qb->qb_edges[i];
            struct q_edge *dst = &chain->qc_edges[i];
            size_t j = 0;

            *dst = *src;
            dst->qe_pub.cse_parameter_count =
                (Dwarf_Unsigned)src->qe_param_count;
            /*  The shorthand copy aliases the builder's arrays.
                Clear them so cleanup never frees twice.  */
            dst->qe_params = 0;
            dst->qe_param_count = 0;
            if (!src->qe_param_count) {
                continue;
            }
            dst->qe_params = (struct q_param *)cs_alloc(
                src->qe_param_count * sizeof(struct q_param));
            if (!dst->qe_params) {
                qb->qb_failed = 1;
                return 0;
            }
            dst->qe_param_count = src->qe_param_count;
            for (j = 0; j < src->qe_param_count; ++j) {
                struct q_param *sp = &src->qe_params[j];
                struct q_param *dp = &dst->qe_params[j];

                *dp = *sp;
                dp->qp_bytes = 0;
                if (sp->qp_length) {
                    dp->qp_bytes =
                        (unsigned char *)malloc(dp->qp_length);
                    if (!dp->qp_bytes) {
                        qb->qb_failed = 1;
                        return 0;
                    }
                    memcpy(dp->qp_bytes,sp->qp_bytes,
                        (size_t)sp->qp_length);
                }
            }
        }
    }
    return 1;
}

static void
qb_pop_edge(struct q_build *qb)
{
    if (!qb->qb_edge_count) {
        return;
    }
    --qb->qb_edge_count;
    q_edge_clear(&qb->qb_edges[qb->qb_edge_count]);
    memset(&qb->qb_edges[qb->qb_edge_count],0,
        sizeof(struct q_edge));
}

/*  Append one edge to the current chain, computing parameter
    values using the previous edge's computed parameters.  */
static int
qb_push_edge(struct q_build *qb,struct cs_fn *owner,
    struct cs_call *call)
{
    struct q_edge *edge = 0;
    struct q_edge *previous = 0;
    size_t i = 0;

    if (qb->qb_edge_count >= CS_MAX_EDGES) {
        qb->qb_failed = 1;
        return 0;
    }
    if (!cs_grow((void **)&qb->qb_edges,sizeof(struct q_edge),
        &qb->qb_edge_cap,qb->qb_edge_count + 1)) {
        qb->qb_failed = 1;
        return 0;
    }
    if (qb->qb_edge_count) {
        previous = &qb->qb_edges[qb->qb_edge_count - 1];
    }
    edge = &qb->qb_edges[qb->qb_edge_count];
    memset(edge,0,sizeof(*edge));
    /*  Count the edge before allocating its parameters so a later
        failure still frees whatever was already allocated.  */
    ++qb->qb_edge_count;
    edge->qe_pub.cse_call_site_offset =
        call->cc_call_site_offset;
    edge->qe_pub.cse_call_return_pc = call->cc_return_pc;
    edge->qe_pub.cse_is_tail_call = call->cc_is_tail;
    edge->qe_pub.cse_origin_known = call->cc_origin_known;
    edge->qe_pub.cse_origin = call->cc_origin;
    edge->qe_pub.cse_origin_unknown_reason =
        call->cc_origin_reason;
    edge->qe_pub.cse_origin_attr_offset =
        call->cc_origin_attr_offset;
    if (call->cc_param_count) {
        edge->qe_params = (struct q_param *)cs_alloc(
            call->cc_param_count * sizeof(struct q_param));
        if (!edge->qe_params) {
            qb->qb_failed = 1;
            return 0;
        }
    }
    edge->qe_param_count = 0;
    edge->qe_pub.cse_parameter_count =
        (Dwarf_Unsigned)call->cc_param_count;
    for (i = 0; i < call->cc_param_count; ++i) {
        struct cs_param *src = &call->cc_params[i];
        struct q_param *dst = 0;

        /*  Set the count as each parameter is filled so cleanup
            always knows how much of the array to release.  */
        dst = &edge->qe_params[i];
        ++edge->qe_param_count;
        memset(dst,0,sizeof(*dst));
        dst->qp_length = src->cp_value.ce_length;
        if (src->cp_value.ce_length) {
            dst->qp_bytes = (unsigned char *)malloc(
                (size_t)src->cp_value.ce_length);
            if (!dst->qp_bytes) {
                qb->qb_failed = 1;
                return 0;
            }
            memcpy(dst->qp_bytes,src->cp_value.ce_bytes,
                (size_t)src->cp_value.ce_length);
        }
        dst->qp_pub.csp_expression_offset =
            src->cp_value.ce_offset;
        dst->qp_pub.csp_expression_length =
            src->cp_value.ce_length;
        dst->qp_pub.csp_call_value_attr_offset =
            src->cp_value.ce_attr_offset;
        dst->qp_pub.csp_register_known =
            src->cp_loc_state == CS_LOC_SINGLE_REG;
        dst->qp_pub.csp_register = src->cp_reg;
        dst->qp_pub.csp_unknown_reason = DW_CS_UNKNOWN_NONE;
        if (src->cp_loc_state == CS_LOC_MISSING &&
            !src->cp_value.ce_present) {
            dst->qp_pub.csp_value_kind = DW_CS_VALUE_UNKNOWN;
            dst->qp_pub.csp_unknown_reason =
                DW_CS_UNKNOWN_MISSING_ARGUMENT;
            continue;
        }
        if (src->cp_loc_state == CS_LOC_UNSUPPORTED) {
            dst->qp_pub.csp_value_kind = DW_CS_VALUE_UNKNOWN;
            dst->qp_pub.csp_unknown_reason =
                DW_CS_UNKNOWN_UNSUPPORTED_LOCATION;
            continue;
        }
        if (src->cp_loc_state == CS_LOC_MISSING) {
            dst->qp_pub.csp_value_kind = DW_CS_VALUE_UNKNOWN;
            dst->qp_pub.csp_unknown_reason =
                DW_CS_UNKNOWN_MISSING_LOCATION;
            continue;
        }
        if (!src->cp_value.ce_present) {
            dst->qp_pub.csp_value_kind = DW_CS_VALUE_UNKNOWN;
            dst->qp_pub.csp_unknown_reason =
                DW_CS_UNKNOWN_MISSING_VALUE;
            continue;
        }
        {
            struct cs_val val;

            cs_eval_expr(&owner->cf_id,previous,
                src->cp_value.ce_bytes,
                (size_t)src->cp_value.ce_length,&val);
            if (val.cv_kind == CS_VAL_KNOWN) {
                dst->qp_pub.csp_value_kind = DW_CS_VALUE_KNOWN;
                dst->qp_pub.csp_value = val.cv_value;
            } else if (val.cv_kind == CS_VAL_SYMBOLIC) {
                dst->qp_pub.csp_value_kind =
                    DW_CS_VALUE_SYMBOLIC;
                dst->qp_pub.csp_source_function = val.cv_source;
                dst->qp_pub.csp_source_register = val.cv_reg;
                dst->qp_pub.csp_source_addend = val.cv_addend;
            } else {
                dst->qp_pub.csp_value_kind = DW_CS_VALUE_UNKNOWN;
                dst->qp_pub.csp_unknown_reason = val.cv_reason;
            }
        }
    }
    return 1;
}

static int
qb_path_contains(struct q_build *qb,
    const Dwarf_Callsite_Die_Identity *id)
{
    size_t i = 0;

    for (i = 0; i < qb->qb_path_count; ++i) {
        if (cs_same_identity(&qb->qb_path[i],id)) {
            return 1;
        }
    }
    return 0;
}

static int
qb_path_push(struct q_build *qb,
    const Dwarf_Callsite_Die_Identity *id)
{
    if (!cs_grow((void **)&qb->qb_path,
        sizeof(Dwarf_Callsite_Die_Identity),&qb->qb_path_cap,
        qb->qb_path_count + 1)) {
        qb->qb_failed = 1;
        return 0;
    }
    qb->qb_path[qb->qb_path_count++] = *id;
    return 1;
}

static void
qb_path_pop(struct q_build *qb)
{
    if (qb->qb_path_count) {
        --qb->qb_path_count;
    }
}

static void qb_expand(struct q_build *qb,struct cs_fn *fn);

/*  Follow one edge out of the current function.  */
static void
qb_follow(struct q_build *qb,struct cs_fn *owner,
    struct cs_call *call)
{
    struct cs_fn *target = 0;

    if (!qb_push_edge(qb,owner,call)) {
        return;
    }
    if (!call->cc_origin_known) {
        qb_add_chain(qb,DW_CS_CHAIN_INCOMPLETE);
        qb_pop_edge(qb);
        return;
    }
    if (qb_path_contains(qb,&call->cc_origin)) {
        qb_add_chain(qb,DW_CS_CHAIN_CYCLE);
        qb_pop_edge(qb);
        return;
    }
    target = cs_find_fn(qb->qb_snap,&call->cc_origin);
    if (!target) {
        qb_add_chain(qb,DW_CS_CHAIN_INCOMPLETE);
        qb_pop_edge(qb);
        return;
    }
    if (!qb_path_push(qb,&target->cf_id)) {
        qb_pop_edge(qb);
        return;
    }
    qb_expand(qb,target);
    qb_path_pop(qb);
    qb_pop_edge(qb);
}

static void
qb_expand(struct q_build *qb,struct cs_fn *fn)
{
    size_t i = 0;
    size_t tail_count = 0;

    if (qb->qb_failed) {
        return;
    }
    for (i = 0; i < fn->cf_call_count; ++i) {
        if (fn->cf_calls[i].cc_is_tail) {
            ++tail_count;
        }
    }
    if (fn->cf_has_all_tail && !tail_count) {
        qb_add_chain(qb,DW_CS_CHAIN_COMPLETE);
    }
    for (i = 0; i < fn->cf_call_count; ++i) {
        if (!fn->cf_calls[i].cc_is_tail) {
            continue;
        }
        qb_follow(qb,fn,&fn->cf_calls[i]);
        if (qb->qb_failed) {
            return;
        }
    }
    if (!fn->cf_has_all_tail) {
        /*  The debug information does not declare the set of
            tail exits to be complete, so an incomplete
            candidate is kept even when known tail edges
            exist.  */
        qb_add_chain(qb,DW_CS_CHAIN_INCOMPLETE);
    }
}

/*  Build every chain candidate for one matching call.  */
static int
qb_build_call(struct q_build *qb,
    struct Dwarf_Callsite_Snapshot_s *snap,struct cs_fn *root_fn,
    struct cs_call *root_call,struct q_call *result)
{
    memset(qb,0,sizeof(*qb));
    qb->qb_snap = snap;
    qb->qb_call = result;
    if (!qb_path_push(qb,&root_fn->cf_id)) {
        return 0;
    }
    if (!qb_push_edge(qb,root_fn,root_call)) {
        return 0;
    }
    if (!root_call->cc_origin_known) {
        qb_add_chain(qb,DW_CS_CHAIN_INCOMPLETE);
    } else if (qb_path_contains(qb,&root_call->cc_origin)) {
        /*  The ordinary call is already recursive: the chain
            re-entered the function it started in.  */
        qb_add_chain(qb,DW_CS_CHAIN_CYCLE);
    } else {
        struct cs_fn *target =
            cs_find_fn(qb->qb_snap,&root_call->cc_origin);

        if (!target) {
            qb_add_chain(qb,DW_CS_CHAIN_INCOMPLETE);
        } else if (qb_path_push(qb,&target->cf_id)) {
            qb_expand(qb,target);
        } else {
            qb->qb_failed = 1;
        }
    }
    qb_pop_edge(qb);
    qb_path_pop(qb);
    return !qb->qb_failed;
}

/*  Free the builder's working edge stack (chains own copies). */
static void
qb_release(struct q_build *qb)
{
    size_t i = 0;

    for (i = 0; i < qb->qb_edge_count; ++i) {
        q_edge_clear(&qb->qb_edges[i]);
    }
    free(qb->qb_edges);
    free(qb->qb_path);
    memset(qb,0,sizeof(*qb));
}

int
dwarf_callsite_query(Dwarf_Callsite_Snapshot dw_snapshot,
    const Dwarf_Callsite_Die_Identity *dw_function,
    Dwarf_Addr dw_call_return_pc,
    Dwarf_Callsite_Query *dw_query,Dwarf_Error *dw_error)
{
    struct cs_fn *fn = 0;
    struct Dwarf_Callsite_Query_s *q = 0;
    size_t matches = 0;
    size_t i = 0;

    if (!dw_query) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    *dw_query = 0;
    if (!dw_snapshot || !dw_function) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    fn = cs_find_fn(dw_snapshot,dw_function);
    if (!fn) {
        return DW_DLV_NO_ENTRY;
    }
    for (i = 0; i < fn->cf_call_count; ++i) {
        if (fn->cf_calls[i].cc_has_return_pc &&
            fn->cf_calls[i].cc_return_pc == dw_call_return_pc) {
            ++matches;
        }
    }
    if (!matches) {
        return DW_DLV_NO_ENTRY;
    }
    q = (struct Dwarf_Callsite_Query_s *)cs_alloc(sizeof(*q));
    if (!q) {
        _dwarf_error(0,dw_error,DW_DLE_MAF);
        return DW_DLV_ERROR;
    }
    q->cq_calls = (struct q_call *)cs_alloc(
        matches * sizeof(struct q_call));
    if (!q->cq_calls) {
        free(q);
        _dwarf_error(0,dw_error,DW_DLE_MAF);
        return DW_DLV_ERROR;
    }
    for (i = 0; i < fn->cf_call_count; ++i) {
        struct cs_call *call = &fn->cf_calls[i];
        struct q_call *qc = 0;
        struct q_build build;

        if (!call->cc_has_return_pc ||
            call->cc_return_pc != dw_call_return_pc) {
            continue;
        }
        qc = &q->cq_calls[q->cq_call_count];
        memset(qc,0,sizeof(*qc));
        qc->qk_pub.cscall_call_site_offset =
            call->cc_call_site_offset;
        qc->qk_pub.cscall_call_return_pc = call->cc_return_pc;
        qc->qk_pub.cscall_origin_known = call->cc_origin_known;
        qc->qk_pub.cscall_origin = call->cc_origin;
        qc->qk_pub.cscall_origin_unknown_reason =
            call->cc_origin_reason;
        /*  Count the call before building so a partial build is
            still released by q_query_free().  */
        ++q->cq_call_count;
        memset(&build,0,sizeof(build));
        if (!qb_build_call(&build,dw_snapshot,fn,call,qc)) {
            qb_release(&build);
            q_query_free(q);
            _dwarf_error(0,dw_error,DW_DLE_MAF);
            return DW_DLV_ERROR;
        }
        qb_release(&build);
        qc->qk_pub.cscall_chain_count =
            (Dwarf_Unsigned)qc->qk_chain_count;
    }
    *dw_query = q;
    return DW_DLV_OK;
}

void
dwarf_callsite_query_destroy(Dwarf_Callsite_Query dw_query)
{
    q_query_free(dw_query);
}

/*  ---- query result accessors ---- */

static struct q_call *
cs_query_call(Dwarf_Callsite_Query q,Dwarf_Unsigned index)
{
    if (!q || index >= (Dwarf_Unsigned)q->cq_call_count) {
        return 0;
    }
    return &q->cq_calls[(size_t)index];
}

static struct q_chain *
cs_query_chain(Dwarf_Callsite_Query q,Dwarf_Unsigned call_index,
    Dwarf_Unsigned chain_index)
{
    struct q_call *call = cs_query_call(q,call_index);

    if (!call || chain_index >= (Dwarf_Unsigned)call->qk_chain_count) {
        return 0;
    }
    return &call->qk_chains[(size_t)chain_index];
}

static struct q_edge *
cs_query_edge(Dwarf_Callsite_Query q,Dwarf_Unsigned call_index,
    Dwarf_Unsigned chain_index,Dwarf_Unsigned edge_index)
{
    struct q_chain *chain =
        cs_query_chain(q,call_index,chain_index);

    if (!chain ||
        edge_index >= (Dwarf_Unsigned)chain->qc_edge_count) {
        return 0;
    }
    return &chain->qc_edges[(size_t)edge_index];
}

static struct q_param *
cs_query_param(Dwarf_Callsite_Query q,Dwarf_Unsigned call_index,
    Dwarf_Unsigned chain_index,Dwarf_Unsigned edge_index,
    Dwarf_Unsigned param_index)
{
    struct q_edge *edge =
        cs_query_edge(q,call_index,chain_index,edge_index);

    if (!edge || param_index >= (Dwarf_Unsigned)edge->qe_param_count) {
        return 0;
    }
    return &edge->qe_params[(size_t)param_index];
}

int
dwarf_callsite_query_call_count(Dwarf_Callsite_Query dw_query,
    Dwarf_Unsigned *dw_count,Dwarf_Error *dw_error)
{
    if (!dw_count) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    *dw_count = 0;
    if (!dw_query) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    *dw_count = (Dwarf_Unsigned)dw_query->cq_call_count;
    return DW_DLV_OK;
}

int
dwarf_callsite_query_call(Dwarf_Callsite_Query dw_query,
    Dwarf_Unsigned dw_call_index,Dwarf_Callsite_Call *dw_call,
    Dwarf_Error *dw_error)
{
    struct q_call *call = 0;

    if (!dw_query || !dw_call) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    memset(dw_call,0,sizeof(*dw_call));
    call = cs_query_call(dw_query,dw_call_index);
    if (!call) {
        return DW_DLV_NO_ENTRY;
    }
    *dw_call = call->qk_pub;
    return DW_DLV_OK;
}

int
dwarf_callsite_query_chain(Dwarf_Callsite_Query dw_query,
    Dwarf_Unsigned dw_call_index,Dwarf_Unsigned dw_chain_index,
    Dwarf_Callsite_Chain *dw_chain,Dwarf_Error *dw_error)
{
    struct q_chain *chain = 0;

    if (!dw_query || !dw_chain) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    memset(dw_chain,0,sizeof(*dw_chain));
    chain = cs_query_chain(dw_query,dw_call_index,dw_chain_index);
    if (!chain) {
        return DW_DLV_NO_ENTRY;
    }
    dw_chain->csc_kind = chain->qc_kind;
    dw_chain->csc_edge_count =
        (Dwarf_Unsigned)chain->qc_edge_count;
    return DW_DLV_OK;
}

int
dwarf_callsite_query_edge(Dwarf_Callsite_Query dw_query,
    Dwarf_Unsigned dw_call_index,Dwarf_Unsigned dw_chain_index,
    Dwarf_Unsigned dw_edge_index,Dwarf_Callsite_Edge *dw_edge,
    Dwarf_Error *dw_error)
{
    struct q_edge *edge = 0;

    if (!dw_query || !dw_edge) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    memset(dw_edge,0,sizeof(*dw_edge));
    edge = cs_query_edge(dw_query,dw_call_index,dw_chain_index,
        dw_edge_index);
    if (!edge) {
        return DW_DLV_NO_ENTRY;
    }
    *dw_edge = edge->qe_pub;
    return DW_DLV_OK;
}

int
dwarf_callsite_query_param(Dwarf_Callsite_Query dw_query,
    Dwarf_Unsigned dw_call_index,Dwarf_Unsigned dw_chain_index,
    Dwarf_Unsigned dw_edge_index,Dwarf_Unsigned dw_param_index,
    Dwarf_Callsite_Param *dw_param,Dwarf_Error *dw_error)
{
    struct q_param *param = 0;

    if (!dw_query || !dw_param) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    memset(dw_param,0,sizeof(*dw_param));
    param = cs_query_param(dw_query,dw_call_index,dw_chain_index,
        dw_edge_index,dw_param_index);
    if (!param) {
        return DW_DLV_NO_ENTRY;
    }
    *dw_param = param->qp_pub;
    return DW_DLV_OK;
}

int
dwarf_callsite_query_param_expression(Dwarf_Callsite_Query dw_query,
    Dwarf_Unsigned dw_call_index,Dwarf_Unsigned dw_chain_index,
    Dwarf_Unsigned dw_edge_index,Dwarf_Unsigned dw_param_index,
    const unsigned char **dw_bytes,Dwarf_Unsigned *dw_length,
    Dwarf_Error *dw_error)
{
    struct q_param *param = 0;

    if (!dw_query || !dw_bytes || !dw_length) {
        _dwarf_error(0,dw_error,DW_DLE_IA);
        return DW_DLV_ERROR;
    }
    *dw_bytes = 0;
    *dw_length = 0;
    param = cs_query_param(dw_query,dw_call_index,dw_chain_index,
        dw_edge_index,dw_param_index);
    if (!param) {
        return DW_DLV_NO_ENTRY;
    }
    *dw_bytes = param->qp_bytes;
    *dw_length = param->qp_length;
    return DW_DLV_OK;
}

/*  ---- printing ---- */

static void
cs_print_line(dwarf_callsite_print_callback cb,void *user,
    const char *line)
{
    if (cb) {
        cb(user,line);
    }
}

static const char *
cs_chain_kind_name(enum Dwarf_Callsite_Chain_Kind kind)
{
    switch (kind) {
    case DW_CS_CHAIN_COMPLETE: return "complete";
    case DW_CS_CHAIN_INCOMPLETE: return "incomplete";
    case DW_CS_CHAIN_CYCLE: return "cycle";
    default: break;
    }
    return "unknown";
}

static const char *
cs_reason_name(enum Dwarf_Callsite_Unknown_Reason reason)
{
    switch (reason) {
    case DW_CS_UNKNOWN_NONE: return "none";
    case DW_CS_UNKNOWN_MISSING_ARGUMENT: return "missing-argument";
    case DW_CS_UNKNOWN_MISSING_VALUE: return "missing-value";
    case DW_CS_UNKNOWN_MISSING_LOCATION: return "missing-location";
    case DW_CS_UNKNOWN_INDIRECT_TARGET: return "indirect-target";
    case DW_CS_UNKNOWN_UNRESOLVED_ORIGIN:
        return "unresolved-origin";
    case DW_CS_UNKNOWN_UNSUPPORTED_EXPRESSION:
        return "unsupported-expression";
    case DW_CS_UNKNOWN_UNSUPPORTED_LOCATION:
        return "unsupported-location";
    case DW_CS_UNKNOWN_OVERFLOW: return "overflow";
    default: break;
    }
    return "unknown-reason";
}

int
dwarf_callsite_query_print(Dwarf_Callsite_Query dw_query,
    dwarf_callsite_print_callback dw_callback,void *dw_user_pointer,
    Dwarf_Error *dw_error)
{
    size_t ci = 0;

    if (!dw_query) {
        return DW_DLV_NO_ENTRY;
    }
    (void)dw_error;
    for (ci = 0; ci < dw_query->cq_call_count; ++ci) {
        struct q_call *call = &dw_query->cq_calls[ci];
        size_t chi = 0;

        {
            char line[160];

            snprintf(line,sizeof(line),
                "CALL call_site=0x%llx return_pc=0x%llx "
                "origin_cu=0x%llx origin_die=0x%llx "
                "origin=%s chains=%llu",
                (unsigned long long)
                    call->qk_pub.cscall_call_site_offset,
                (unsigned long long)
                    call->qk_pub.cscall_call_return_pc,
                (unsigned long long)
                    call->qk_pub.cscall_origin
                        .dci_cu_header_offset,
                (unsigned long long)
                    call->qk_pub.cscall_origin.dci_die_offset,
                call->qk_pub.cscall_origin_known ? "known" :
                    "unknown",
                (unsigned long long)
                    call->qk_pub.cscall_chain_count);
            cs_print_line(dw_callback,dw_user_pointer,line);
        }
        for (chi = 0; chi < call->qk_chain_count; ++chi) {
            struct q_chain *chain = &call->qk_chains[chi];
            size_t ei = 0;

            {
                char line[160];

                snprintf(line,sizeof(line),
                    "CHAIN call=%llu index=%llu kind=%s edges=%llu",
                    (unsigned long long)ci,
                    (unsigned long long)chi,
                    cs_chain_kind_name(chain->qc_kind),
                    (unsigned long long)chain->qc_edge_count);
                cs_print_line(dw_callback,dw_user_pointer,line);
            }
            for (ei = 0; ei < chain->qc_edge_count; ++ei) {
                struct q_edge *edge = &chain->qc_edges[ei];
                size_t pi = 0;

                {
                    char line[200];

                    snprintf(line,sizeof(line),
                        "EDGE call=%llu chain=%llu edge=%llu "
                        "call_site=0x%llx return_pc=0x%llx "
                        "tail=%u origin_cu=0x%llx origin_die=0x%llx "
                        "origin=%s origin_attr=0x%llx params=%llu",
                        (unsigned long long)ci,
                        (unsigned long long)chi,
                        (unsigned long long)ei,
                        (unsigned long long)
                            edge->qe_pub.cse_call_site_offset,
                        (unsigned long long)
                            edge->qe_pub.cse_call_return_pc,
                        (unsigned)edge->qe_pub.cse_is_tail_call,
                        (unsigned long long)
                            edge->qe_pub.cse_origin
                                .dci_cu_header_offset,
                        (unsigned long long)
                            edge->qe_pub.cse_origin.dci_die_offset,
                        edge->qe_pub.cse_origin_known ?
                            "known" : "unknown",
                        (unsigned long long)
                            edge->qe_pub.cse_origin_attr_offset,
                        (unsigned long long)
                            edge->qe_pub.cse_parameter_count);
                    cs_print_line(dw_callback,dw_user_pointer,line);
                }
                for (pi = 0; pi < edge->qe_param_count; ++pi) {
                    struct q_param *p = &edge->qe_params[pi];
                    char line[320];
                    char bytes[136];
                    size_t bi = 0;
                    size_t maxbytes = (size_t)p->qp_length;
                    Dwarf_Callsite_Param *pub = &p->qp_pub;

                    if (maxbytes > 64) {
                        maxbytes = 64;
                    }
                    bytes[0] = 0;
                    for (bi = 0; bi < maxbytes; ++bi) {
                        char chunk[4];

                        snprintf(chunk,sizeof(chunk),"%02x",
                            p->qp_bytes[bi]);
                        if (strlen(bytes) + 3 <
                            sizeof(bytes)) {
                            strcat(bytes,chunk);
                        }
                    }
                    if (pub->csp_value_kind == DW_CS_VALUE_KNOWN) {
                        snprintf(line,sizeof(line),
                            "PARAM call=%llu chain=%llu edge=%llu "
                            "param=%llu reg=%s%llu value=known "
                            "%lld expr_off=0x%llx bytes=%s",
                            (unsigned long long)ci,
                            (unsigned long long)chi,
                            (unsigned long long)ei,
                            (unsigned long long)pi,
                            pub->csp_register_known ? "" : "?",
                            (unsigned long long)
                                pub->csp_register,
                            (long long)pub->csp_value,
                            (unsigned long long)
                                pub->csp_expression_offset,
                            bytes);
                    } else if (pub->csp_value_kind ==
                        DW_CS_VALUE_SYMBOLIC) {
                        snprintf(line,sizeof(line),
                            "PARAM call=%llu chain=%llu edge=%llu "
                            "param=%llu reg=%s%llu value=symbolic "
                            "source_cu=0x%llx source_die=0x%llx "
                            "source_reg=%llu addend=%lld "
                            "expr_off=0x%llx bytes=%s",
                            (unsigned long long)ci,
                            (unsigned long long)chi,
                            (unsigned long long)ei,
                            (unsigned long long)pi,
                            pub->csp_register_known ? "" : "?",
                            (unsigned long long)
                                pub->csp_register,
                            (unsigned long long)
                                pub->csp_source_function
                                    .dci_cu_header_offset,
                            (unsigned long long)
                                pub->csp_source_function
                                    .dci_die_offset,
                            (unsigned long long)
                                pub->csp_source_register,
                            (long long)pub->csp_source_addend,
                            (unsigned long long)
                                pub->csp_expression_offset,
                            bytes);
                    } else {
                        snprintf(line,sizeof(line),
                            "PARAM call=%llu chain=%llu edge=%llu "
                            "param=%llu reg=%s%llu value=unknown "
                            "reason=%s expr_off=0x%llx bytes=%s",
                            (unsigned long long)ci,
                            (unsigned long long)chi,
                            (unsigned long long)ei,
                            (unsigned long long)pi,
                            pub->csp_register_known ? "" : "?",
                            (unsigned long long)
                                pub->csp_register,
                            cs_reason_name(
                                pub->csp_unknown_reason),
                            (unsigned long long)
                                pub->csp_expression_offset,
                            bytes);
                    }
                    cs_print_line(dw_callback,dw_user_pointer,line);
                }
            }
        }
    }
    return DW_DLV_OK;
}
