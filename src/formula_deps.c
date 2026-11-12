/*
 * BIFF8 formula dependency collection and query.
 *
 * Design notes
 * ------------
 * Parsing is split into two phases, mirroring the BIFF8 record order in which
 * the anchor FORMULA precedes the SHRFMLA definition and the ptgExp usage
 * FORMULA records may arrive much later, non-contiguously:
 *
 *   1. xls_parseWorkSheet() feeds every FORMULA/SHRFMLA record to
 *      xls_fd_record() before any endian conversion takes place, so the bytes
 *      seen here are always the on-disk little-endian encoding. The raw token
 *      blobs are copied into per-worksheet metadata; nothing is resolved yet.
 *      No parse error is ever raised from this path: a malformed formula is
 *      tagged on its own cell so that neighbouring cells keep their cached
 *      values and the historical reader behaviour is unchanged.
 *   2. xls_get_formula_dependencies() resolves the queried cell lazily.
 *      Shared usages are linked to the SHRFMLA definition through the ptgExp
 *      anchor and every relative offset is interpreted against the queried
 *      usage cell, never against the shared anchor.
 *
 * Token walking uses real, variable token sizes (Unicode ptgStr and the
 * ptgAttr Choose jump table included), so a value that merely looks like a
 * reference opcode can never be mistaken for one.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "../include/xls.h"
#include "../include/libxls/xlsstruct.h"
#include "formula_deps.h"

#define XLS_RECORD_SHRFMLA 0x04BC

/* FORMULA record field offsets (BIFF8); byte-addressed to stay endian-safe. */
#define FORMULA_ROW_OFF    0
#define FORMULA_COL_OFF    2
#define FORMULA_LEN_OFF    20
#define FORMULA_FIXED      22	/* offset of the token blob */

/* Ptg opcodes. */
#define PTG_TBL          0x02
#define PTG_EXP          0x01
#define PTG_SHEET        0x1A
#define PTG_ENDSHEET     0x1B
#define PTG_STR          0x17
#define PTG_ATTR         0x19
#define PTG_ARRAY        0x20
#define PTG_FUNC         0x21
#define PTG_FUNCVAR      0x22
#define PTG_NAME         0x23
#define PTG_REF          0x24
#define PTG_AREA         0x25
#define PTG_REFERR       0x2A
#define PTG_AREAERR      0x2B
#define PTG_REFN         0x2C
#define PTG_AREAN        0x2D
#define PTG_NAMEX        0x39
#define PTG_REF3D        0x3A
#define PTG_AREA3D       0x3B
#define PTG_REFERR3D     0x3C
#define PTG_AREAERR3D    0x3D

#define ATTR_CHOOSE      0x08

#define FUNC_INDIRECT    0x94
#define FUNC_OFFSET      0x4E

#define XLS_MAX_ROWS     65536
#define XLS_MAX_COLS     256

typedef struct {
    WORD row;
    WORD col;
    int shared_usage;       /* token blob is a single ptgExp */
    WORD anchor_row;        /* ptgExp target, valid when shared_usage */
    WORD anchor_col;
    int anchor_of;          /* index of the SHRFMLA definition, or -1 */
    WORD token_len;
    BYTE *tokens;           /* independent copy */
    WORD record_id;
    DWORD record_offset;
    int corrupt;            /* parse-time corruption marker */
    DWORD token_offset;     /* location of the parse-time problem */
} fd_cell_t;

typedef struct {
    WORD row_first, row_last;
    WORD col_first, col_last;
    WORD anchor_row;
    WORD anchor_col;
    WORD token_len;
    BYTE *tokens;
    WORD record_id;
    DWORD record_offset;
    int corrupt;
} fd_shared_t;

typedef struct {
    fd_cell_t *cells;
    size_t cell_count;
    size_t cell_cap;
    fd_shared_t *defs;
    size_t def_count;
    size_t def_cap;
    long last_cell;         /* index of the most recent FORMULA, -1 if none */
} fd_ctx_t;

/* ---- little-endian, endian-neutral field readers ---- */

static WORD rd16(const BYTE *p)
{
    return (WORD)(p[0] | (p[1] << 8));
}

static int s16(WORD v)
{
    return (v & 0x8000) ? (int)v - 65536 : (int)v;
}

/* Relative column offsets share their word with the two flag bits. */
static int s14rel(WORD v)
{
    v &= 0x3FFF;
    return (v & 0x2000) ? (int)v - 16384 : (int)v;
}

static BYTE fd_normalize(BYTE op)
{
    /* Reference class 0x20-0x3F, value class +0x20, array class +0x40. */
    if (op >= 0x60)
        return (BYTE)(op - 0x40);
    if (op >= 0x40)
        return (BYTE)(op - 0x20);
    return op;
}

static void *fd_memdup(const void *src, size_t n)
{
    void *dst = malloc(n ? n : 1);
    if (dst)
        memcpy(dst, src, n);
    return dst;
}

void xls_fd_init(xlsWorkSheet *pWS)
{
    fd_ctx_t *ctx;

    if (pWS->formula_deps)
        xls_fd_free(pWS);	/* tolerate a repeated worksheet parse */

    ctx = calloc(1, sizeof(fd_ctx_t));
    if (ctx)
        ctx->last_cell = -1;
    pWS->formula_deps = ctx;
}

void xls_fd_free(xlsWorkSheet *pWS)
{
    fd_ctx_t *ctx;
    size_t i;

    if (!pWS || !(ctx = pWS->formula_deps))
        return;

    for (i = 0; i < ctx->cell_count; ++i)
        free(ctx->cells[i].tokens);
    free(ctx->cells);

    for (i = 0; i < ctx->def_count; ++i)
        free(ctx->defs[i].tokens);
    free(ctx->defs);

    free(ctx);
    pWS->formula_deps = NULL;
}

static fd_cell_t *fd_last_formula(fd_ctx_t *ctx)
{
    if (ctx->last_cell < 0)
        return NULL;
    return &ctx->cells[ctx->last_cell];
}

static void fd_record_formula(fd_ctx_t *ctx, WORD id, WORD size,
        const BYTE *buf, DWORD record_offset)
{
    fd_cell_t cell;
    size_t available;

    memset(&cell, 0, sizeof(cell));
    cell.anchor_of = -1;
    cell.record_id = id;
    cell.record_offset = record_offset;

    if (size < FORMULA_FIXED) {
        cell.corrupt = 1;
        cell.token_len = 0;
        cell.tokens = NULL;
    } else {
        cell.row = rd16(buf + FORMULA_ROW_OFF);
        cell.col = rd16(buf + FORMULA_COL_OFF);
        available = (size_t)size - FORMULA_FIXED;
        cell.token_len = rd16(buf + FORMULA_LEN_OFF);
        if (cell.token_len > available) {
            /* Claimed token run overruns the record. */
            cell.corrupt = 1;
            cell.token_len = (WORD)available;
        }
        cell.tokens = fd_memdup(buf + FORMULA_FIXED, cell.token_len);
        if (!cell.tokens) {
            cell.corrupt = 1;
            cell.token_len = 0;
        }
    }

    if (cell.token_len >= 1 && cell.tokens && cell.tokens[0] == PTG_EXP) {
        /* Shared-formula usage: the only legal token is a five-byte ptgExp. */
        cell.shared_usage = 1;
        if (cell.token_len != 5) {
            cell.corrupt = 1;
        } else {
            cell.anchor_row = rd16(cell.tokens + 1);
            cell.anchor_col = (WORD)(rd16(cell.tokens + 3) & 0x01FF);
        }
    }

    if (ctx->cell_count == ctx->cell_cap) {
        size_t new_cap = ctx->cell_cap ? ctx->cell_cap * 2 : 16;
        fd_cell_t *grown = realloc(ctx->cells, new_cap * sizeof(fd_cell_t));
        if (!grown) {
            free(cell.tokens);
            return;
        }
        ctx->cells = grown;
        ctx->cell_cap = new_cap;
    }
    ctx->cells[ctx->cell_count] = cell;
    ctx->last_cell = (long)ctx->cell_count;
    ctx->cell_count++;
}

static void fd_record_shared(fd_ctx_t *ctx, WORD size, const BYTE *buf,
        DWORD record_offset)
{
    fd_cell_t *anchor;
    fd_shared_t def;
    WORD cce;
    size_t available;

    memset(&def, 0, sizeof(def));
    def.record_id = XLS_RECORD_SHRFMLA;
    def.record_offset = record_offset;

    anchor = fd_last_formula(ctx);
    if (!anchor || anchor->shared_usage) {
        /* Orphan SHRFMLA: no anchor FORMULA precedes it. Nothing can be
         * attached, so any ptgExp naming this cell is later reported as a
         * missing definition. */
        return;
    }

    if (size < 8) {
        anchor->corrupt = 1;
        anchor->token_offset = 0;
        return;
    }

    def.row_first = rd16(buf);
    def.row_last = rd16(buf + 2);
    def.col_first = buf[4];
    def.col_last = buf[5];
    cce = rd16(buf + 6);
    def.anchor_row = anchor->row;
    def.anchor_col = anchor->col;

    available = (size_t)size - 8;	/* cce itself plus the token blob */
    if (cce > available) {
        anchor->corrupt = 1;
        anchor->token_offset = 0;
        return;
    }
    def.token_len = cce;
    def.tokens = fd_memdup(buf + 8, cce);
    if (!def.tokens) {
        anchor->corrupt = 1;
        return;
    }

    if (!(def.row_first <= def.row_last && def.col_first <= def.col_last &&
            def.row_first <= anchor->row && anchor->row <= def.row_last &&
            def.col_first <= anchor->col && anchor->col <= def.col_last)) {
        def.corrupt = 1;
    }

    /* A second SHRFMLA for the same anchor is duplicate corruption. */
    {
        size_t i;
        for (i = 0; i < ctx->def_count; ++i) {
            fd_shared_t *other = &ctx->defs[i];
            if (other->anchor_row == def.anchor_row &&
                    other->anchor_col == def.anchor_col) {
                def.corrupt = 1;
                break;
            }
        }
    }

    if (ctx->def_count == ctx->def_cap) {
        size_t new_cap = ctx->def_cap ? ctx->def_cap * 2 : 8;
        fd_shared_t *grown = realloc(ctx->defs,
                new_cap * sizeof(fd_shared_t));
        if (!grown) {
            free(def.tokens);
            anchor->corrupt = 1;
            return;
        }
        ctx->defs = grown;
        ctx->def_cap = new_cap;
    }

    ctx->defs[ctx->def_count] = def;
    anchor->anchor_of = (int)ctx->def_count;
    ctx->def_count++;
}

void xls_fd_record(xlsWorkSheet *pWS, WORD id, WORD size, const BYTE *buf,
        DWORD record_offset)
{
    fd_ctx_t *ctx;

    if (!pWS || !buf)
        return;
    if (!(ctx = pWS->formula_deps)) {
        xls_fd_init(pWS);
        ctx = pWS->formula_deps;
        if (!ctx)
            return;
    }

    switch (id) {
    case XLS_RECORD_FORMULA:
    case XLS_RECORD_FORMULA_ALT:
        fd_record_formula(ctx, id, size, buf, record_offset);
        break;
    case XLS_RECORD_SHRFMLA:
        fd_record_shared(ctx, size, buf, record_offset);
        break;
    default:
        break;
    }
}

/* ---- token stream decoding ---- */

typedef struct {
    xls_formula_dependency *rects;
    size_t count;
    size_t cap;
    xls_formula_query_status status;
    DWORD token_offset;
} fd_decode_t;

static int fd_grow_rects(fd_decode_t *dec)
{
    size_t new_cap = dec->cap ? dec->cap * 2 : 8;
    xls_formula_dependency *grown = realloc(dec->rects,
            new_cap * sizeof(*grown));
    if (!grown)
        return 0;
    dec->rects = grown;
    dec->cap = new_cap;
    return 1;
}

static int fd_add_rect(fd_decode_t *dec, WORD r0, WORD r1, WORD c0, WORD c1)
{
    size_t i;
    for (i = 0; i < dec->count; ++i) {
        xls_formula_dependency *d = &dec->rects[i];
        if (d->row_first == r0 && d->row_last == r1 &&
                d->col_first == c0 && d->col_last == c1)
            return 1;	/* exact duplicate kept once */
    }
    if (dec->count == dec->cap && !fd_grow_rects(dec))
        return 0;
    dec->rects[dec->count].row_first = r0;
    dec->rects[dec->count].row_last = r1;
    dec->rects[dec->count].col_first = c0;
    dec->rects[dec->count].col_last = c1;
    dec->count++;
    return 1;
}

static void fd_fail(fd_decode_t *dec, xls_formula_query_status status,
        DWORD token_offset)
{
    dec->status = status;
    dec->token_offset = token_offset;
}

/* Resolve one row/column value under the strict non-wrapping policy. */
static int fd_resolve(int base, int offset, int absolute, int is_row, int *out)
{
    int v = absolute ? offset : base + offset;
    int limit = is_row ? XLS_MAX_ROWS : XLS_MAX_COLS;
    if (v < 0 || v >= limit)
        return 0;
    *out = v;
    return 1;
}

/* PtgRef (5 bytes) and PtgRefN (9 bytes). is_n selects the shared-formula
 * layout carrying two dedicated signed offsets. */
static int fd_decode_ref(const BYTE *t, WORD base_row, WORD base_col,
        int is_n, WORD *r, WORD *c, int *oob)
{
    WORD rw = rd16(t + 1);
    WORD cw = rd16(t + 3);
    int rel_row = (cw & 0x4000) != 0;
    int rel_col = (cw & 0x8000) != 0;
    int row_off, col_off, rv, cv;

    if (is_n) {
        row_off = s16(rd16(t + 5));
        col_off = s16(rd16(t + 7));
    } else {
        row_off = s16(rw);
        col_off = s14rel(cw);
    }

    if (!fd_resolve(base_row, row_off, !rel_row, 1, &rv) ||
            !fd_resolve(base_col, col_off, !rel_col, 0, &cv)) {
        *oob = 1;
        return 0;
    }
    *r = (WORD)rv;
    *c = (WORD)cv;
    return 1;
}

/* PtgArea (9 bytes) and PtgAreaN (13 bytes). */
static int fd_decode_area(const BYTE *t, WORD base_row, WORD base_col,
        int is_n, WORD *r0, WORD *r1, WORD *c0, WORD *c1, int *oob)
{
    WORD rw_first = rd16(t + 1);
    WORD rw_last = rd16(t + 3);
    WORD cw_first = rd16(t + 5);
    WORD cw_last = rd16(t + 7);
    int rel_row = (cw_first & 0x4000) != 0;
    int rel_col = (cw_first & 0x8000) != 0;
    int rf, rl, cf, cl;

    if (is_n) {
        int row_off = s16(rd16(t + 9));
        int col_off = s16(rd16(t + 11));
        int row_delta = s16(rw_last) - s16(rw_first);
        int col_delta = s14rel(cw_last) - s14rel(cw_first);

        rf = rel_row ? (int)base_row + row_off : (int)rw_first;
        rl = rel_row ? rf + row_delta : (int)rw_last;
        cf = rel_col ? (int)base_col + col_off : (int)(cw_first & 0x01FF);
        cl = rel_col ? cf + col_delta : (int)(cw_last & 0x01FF);
    } else {
        rf = rel_row ? (int)base_row + s16(rw_first) : (int)rw_first;
        rl = rel_row ? (int)base_row + s16(rw_last) : (int)rw_last;
        cf = rel_col ? (int)base_col + s14rel(cw_first)
                     : (int)(cw_first & 0x01FF);
        cl = rel_col ? (int)base_col + s14rel(cw_last)
                     : (int)(cw_last & 0x01FF);
    }

    if (rf < 0 || rf >= XLS_MAX_ROWS || rl < 0 || rl >= XLS_MAX_ROWS ||
            cf < 0 || cf >= XLS_MAX_COLS || cl < 0 || cl >= XLS_MAX_COLS) {
        *oob = 1;
        return 0;
    }
    *r0 = (WORD)rf;
    *r1 = (WORD)rl;
    *c0 = (WORD)cf;
    *c1 = (WORD)cl;
    return 1;
}

/* Variable/special token lengths beyond the fixed-size lookup table.
 * Returns 1 with *size_out filled, 0 when the header is truncated, -1 when
 * the opcode has no variable form. */
static int fd_token_size(const BYTE *b, size_t remaining, WORD *size_out)
{
    if (b[0] == PTG_STR) {
        BYTE cch;
        WORD len;
        if (remaining < 3)
            return 0;
        cch = b[1];
        len = (WORD)(3 + cch * ((b[2] & 0x01) ? 2 : 1));
        if (len > remaining)
            return 0;
        *size_out = len;
        return 1;
    }
    if (b[0] == PTG_ATTR) {
        if (remaining < 4)
            return 0;
        if (b[1] == ATTR_CHOOSE) {
            WORD choices = rd16(b + 2);
            WORD len = (WORD)(4 + 2 * (choices + 1));
            if (len > remaining)
                return 0;
            *size_out = len;
            return 1;
        }
        *size_out = 4;
        return 1;
    }
    return -1;
}

/* Fixed token sizes; zero means the opcode is unknown/illegal here. The
 * PtgRefN/PtgAreaN entries (9/13) deliberately differ from the old dump table
 * in formulas/xlsformula.c, which under-counts the two shared-formula offset
 * words. */
static const WORD fd_fixed_size[128] = {
    /* 0x00 */ 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 0x10 */ 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 2, 2, 2, 2, 3, 9,
    /* 0x20 */ 8, 3, 4, 5, 5, 9, 7, 7, 7, 3, 5, 9, 9, 13, 3, 3,
    /* 0x30 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 7, 11, 7, 11, 0, 0,
    /* 0x40 */ 8, 3, 4, 5, 5, 9, 7, 7, 7, 3, 5, 9, 9, 13, 3, 3,
    /* 0x50 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 7, 11, 7, 11, 0, 0,
    /* 0x60 */ 8, 3, 4, 5, 5, 9, 7, 7, 7, 3, 5, 9, 9, 13, 3, 3,
    /* 0x70 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 7, 11, 7, 11, 0, 0
};

static int fd_is_unsupported(BYTE normalized)
{
    switch (normalized) {
    case PTG_TBL:		/* data table */
    case PTG_SHEET:
    case PTG_ENDSHEET:
    case PTG_ARRAY:		/* array constant */
    case PTG_NAME:
    case PTG_NAMEX:
    case PTG_REF3D:
    case PTG_AREA3D:
    case PTG_REFERR3D:
    case PTG_AREAERR3D:
    case PTG_REFERR:	/* explicit #REF! error reference */
    case PTG_AREAERR:
        return 1;
    default:
        return 0;
    }
}

static int fd_dynamic_function(const BYTE *b, WORD len)
{
    BYTE norm = fd_normalize(b[0]);
    WORD func;

    if (norm == PTG_FUNC) {
        if (len < 3)
            return 0;
        func = rd16(b + 1);
    } else if (norm == PTG_FUNCVAR) {
        if (len < 4)
            return 0;
        func = rd16(b + 2);
    } else {
        return 0;
    }
    return func == FUNC_INDIRECT || func == FUNC_OFFSET;
}

static void fd_decode_tokens(const BYTE *blob, WORD blob_len,
        WORD base_row, WORD base_col, fd_decode_t *dec)
{
    size_t pos = 0;

    while (pos < blob_len) {
        BYTE op = blob[pos];
        BYTE norm;
        WORD tlen;
        int variable;
        const BYTE *t = blob + pos;

        if (op >= 0x80) {
            fd_fail(dec, XLS_FORMULA_QUERY_CORRUPT, (DWORD)pos);
            return;
        }
        if (op == PTG_EXP) {
            /* A ptgExp outside the shared-usage fast path is always wrong. */
            fd_fail(dec, XLS_FORMULA_QUERY_CORRUPT, (DWORD)pos);
            return;
        }

        norm = fd_normalize(op);
        if (fd_is_unsupported(norm)) {
            fd_fail(dec, XLS_FORMULA_QUERY_UNSUPPORTED, (DWORD)pos);
            return;
        }

        variable = fd_token_size(blob + pos, blob_len - pos, &tlen);
        if (variable == 0) {
            fd_fail(dec, XLS_FORMULA_QUERY_CORRUPT, (DWORD)pos);
            return;
        }
        if (variable < 0) {
            tlen = fd_fixed_size[op];
            if (tlen == 0) {
                fd_fail(dec, XLS_FORMULA_QUERY_UNSUPPORTED, (DWORD)pos);
                return;
            }
            if (tlen > blob_len - pos) {
                fd_fail(dec, XLS_FORMULA_QUERY_CORRUPT, (DWORD)pos);
                return;
            }
        }

        if ((norm == PTG_FUNC || norm == PTG_FUNCVAR) &&
                fd_dynamic_function(t, tlen)) {
            fd_fail(dec, XLS_FORMULA_QUERY_UNSUPPORTED, (DWORD)pos);
            return;
        }

        switch (norm) {
        case PTG_REF:
        case PTG_REFN:
        {
            int oob = 0;
            WORD r, c;
            if (!fd_decode_ref(t, base_row, base_col, norm == PTG_REFN,
                    &r, &c, &oob)) {
                fd_fail(dec, XLS_FORMULA_QUERY_OUT_OF_BOUNDS, (DWORD)pos);
                return;
            }
            if (!fd_add_rect(dec, r, r, c, c)) {
                fd_fail(dec, XLS_FORMULA_QUERY_ERROR, (DWORD)pos);
                return;
            }
            break;
        }
        case PTG_AREA:
        case PTG_AREAN:
        {
            int oob = 0;
            WORD r0, r1, c0, c1;
            if (!fd_decode_area(t, base_row, base_col, norm == PTG_AREAN,
                    &r0, &r1, &c0, &c1, &oob)) {
                fd_fail(dec, XLS_FORMULA_QUERY_OUT_OF_BOUNDS, (DWORD)pos);
                return;
            }
            if (!fd_add_rect(dec, r0, r1, c0, c1)) {
                fd_fail(dec, XLS_FORMULA_QUERY_ERROR, (DWORD)pos);
                return;
            }
            break;
        }
        default:
            /* Operators, constants, functions, Attr, Mem*: syntax only. */
            break;
        }

        pos += tlen;
    }
}

static int fd_rect_cmp(const void *a, const void *b)
{
    const xls_formula_dependency *x = a;
    const xls_formula_dependency *y = b;
    if (x->row_first != y->row_first)
        return x->row_first < y->row_first ? -1 : 1;
    if (x->col_first != y->col_first)
        return x->col_first < y->col_first ? -1 : 1;
    if (x->row_last != y->row_last)
        return x->row_last < y->row_last ? -1 : 1;
    if (x->col_last != y->col_last)
        return x->col_last < y->col_last ? -1 : 1;
    return 0;
}

static const fd_cell_t *fd_find_cell(fd_ctx_t *ctx, WORD row, WORD col)
{
    const fd_cell_t *found = NULL;
    size_t i;
    for (i = 0; i < ctx->cell_count; ++i) {
        if (ctx->cells[i].row == row && ctx->cells[i].col == col)
            found = &ctx->cells[i];	/* last record wins, like xls_addCell */
    }
    return found;
}

static fd_shared_t *fd_find_def(fd_ctx_t *ctx, WORD anchor_row,
        WORD anchor_col)
{
    size_t i;
    for (i = 0; i < ctx->def_count; ++i) {
        if (ctx->defs[i].anchor_row == anchor_row &&
                ctx->defs[i].anchor_col == anchor_col)
            return &ctx->defs[i];
    }
    return NULL;
}

xls_formula_query_status xls_get_formula_dependencies(xlsWorkSheet* pWS,
        WORD row, WORD col, xls_formula_dependencies **out)
{
    xls_formula_dependencies *result;
    fd_ctx_t *ctx;
    const fd_cell_t *fcell;
    const BYTE *blob;
    WORD blob_len;
    WORD base_row, base_col;
    WORD record_id;
    DWORD record_offset;
    fd_decode_t dec;

    if (!out)
        return XLS_FORMULA_QUERY_ERROR;
    *out = NULL;
    if (!pWS)
        return XLS_FORMULA_QUERY_ERROR;

    result = calloc(1, sizeof(*result));
    if (!result)
        return XLS_FORMULA_QUERY_ERROR;
    result->status = XLS_FORMULA_QUERY_NOT_A_FORMULA;

    ctx = pWS->formula_deps;
    if (!ctx || !(fcell = fd_find_cell(ctx, row, col))) {
        *out = result;
        return result->status;
    }

    record_id = fcell->record_id;
    record_offset = fcell->record_offset;

    if (fcell->corrupt) {
        result->status = XLS_FORMULA_QUERY_CORRUPT;
        result->record_id = record_id;
        result->record_offset = record_offset;
        result->token_offset = fcell->token_offset;
        *out = result;
        return result->status;
    }

    blob = fcell->tokens;
    blob_len = fcell->token_len;
    base_row = row;
    base_col = col;

    if (fcell->shared_usage) {
        fd_shared_t *def = fd_find_def(ctx, fcell->anchor_row,
                fcell->anchor_col);
        if (!def) {
            result->status = XLS_FORMULA_QUERY_CORRUPT;
            result->record_id = record_id;
            result->record_offset = record_offset;
            *out = result;
            return result->status;
        }
        if (def->corrupt) {
            result->status = XLS_FORMULA_QUERY_CORRUPT;
            result->record_id = def->record_id;
            result->record_offset = def->record_offset;
            *out = result;
            return result->status;
        }
        if (row < def->row_first || row > def->row_last ||
                col < def->col_first || col > def->col_last) {
            result->status = XLS_FORMULA_QUERY_CORRUPT;
            result->record_id = record_id;
            result->record_offset = record_offset;
            *out = result;
            return result->status;
        }
        blob = def->tokens;
        blob_len = def->token_len;
    } else if (fcell->anchor_of >= 0) {
        fd_shared_t *def = &ctx->defs[fcell->anchor_of];
        if (def->corrupt) {
            result->status = XLS_FORMULA_QUERY_CORRUPT;
            result->record_id = def->record_id;
            result->record_offset = def->record_offset;
            *out = result;
            return result->status;
        }
        blob = def->tokens;
        blob_len = def->token_len;
        base_row = def->anchor_row;
        base_col = def->anchor_col;
    }

    memset(&dec, 0, sizeof(dec));
    fd_decode_tokens(blob, blob_len, base_row, base_col, &dec);

    if (dec.status == XLS_FORMULA_QUERY_ERROR) {
        free(dec.rects);
        free(result);
        return XLS_FORMULA_QUERY_ERROR;
    }

    if (dec.status == 0) {
        result->status = dec.count == 0
                ? XLS_FORMULA_QUERY_NO_REFERENCES
                : XLS_FORMULA_QUERY_OK;
    } else {
        /* A failure must never surface a partial dependency list, even when
         * valid references preceded the offending token. */
        result->status = dec.status;
        result->record_id = record_id;
        result->record_offset = record_offset;
        result->token_offset = dec.token_offset;
        free(dec.rects);
        dec.rects = NULL;
        dec.count = 0;
    }

    if (dec.count) {
        qsort(dec.rects, dec.count, sizeof(*dec.rects), fd_rect_cmp);
        result->dependencies = dec.rects;
        result->count = (DWORD)dec.count;
    } else {
        free(dec.rects);
        result->dependencies = NULL;
        result->count = 0;
    }
    *out = result;
    return result->status;
}

void xls_free_formula_dependencies(xls_formula_dependencies *result)
{
    if (!result)
        return;
    free(result->dependencies);
    free(result);
}
