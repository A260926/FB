/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 * Copyright 2004 Komarov Valery
 * Copyright 2006 Christophe Leitienne
 * Copyright 2008-2017 David Hoerl
 * Copyright 2013-2018 Evan Miller
 *
 * This file is part of libxls -- A multiplatform, C/C++ library for parsing
 * Excel(TM) files.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY Evan Miller ''AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef XLS_INCLUDE
#define XLS_INCLUDE
 
/* System headers must be pulled in before the namespace is opened, otherwise
 * the nested includes below would declare the C library inside namespace xls. */
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#ifdef _MSC_VER
#include <BaseTsd.h>
#endif

#ifdef __cplusplus
namespace xls {
extern "C" {
#endif

#include "libxls/xlstypes.h"
#include "libxls/xlsstruct.h"
#include "libxls/xlstool.h"

typedef enum {
    LIBXLS_OK,
    LIBXLS_ERROR_OPEN,
    LIBXLS_ERROR_SEEK,
    LIBXLS_ERROR_READ,
    LIBXLS_ERROR_PARSE,
    LIBXLS_ERROR_MALLOC,
    LIBXLS_ERROR_UNSUPPORTED_ENCRYPTION,
    LIBXLS_ERROR_NULL_ARGUMENT
} xls_error_t;

const char* xls_getVersion(void);
const char* xls_getError(xls_error_t code);

int xls(int debug);	// Set debug. Force library to load?
void xls_set_formula_hander(xls_formula_handler handler);

xls_error_t xls_parseWorkBook(xlsWorkBook* pWB);
xls_error_t xls_parseWorkSheet(xlsWorkSheet* pWS);

// Preferred API
// charset - convert 16bit strings within the spread sheet to this 8-bit encoding (UTF-8 default)
xlsWorkBook *xls_open_file(const char *file, const char *charset, xls_error_t *outError);
xlsWorkBook *xls_open_buffer(const unsigned char *data, size_t data_len,
        const char *charset, xls_error_t *outError);
void xls_close_WB(xlsWorkBook* pWB);

// Historical API
xlsWorkBook* xls_open(const char *file,const char *charset);
#define xls_close xls_close_WB

xlsWorkSheet * xls_getWorkSheet(xlsWorkBook* pWB,int num);
void xls_close_WS(xlsWorkSheet* pWS);

xlsSummaryInfo *xls_summaryInfo(xlsWorkBook* pWB);
void xls_close_summaryInfo(xlsSummaryInfo *pSI);

// utility function
xlsRow *xls_row(xlsWorkSheet* pWS, WORD cellRow);
xlsCell	*xls_cell(xlsWorkSheet* pWS, WORD cellRow, WORD cellCol);

/*
 * Static BIFF8 formula dependency query.
 *
 * xls_get_formula_dependencies() reports the direct, in-worksheet references
 * of the formula in the cell at (row, col). Formula results are never
 * computed: only the stored Ptg tokens are decoded. Shared formulas
 * (SHRFMLA) are resolved per usage cell, so relative PtgRefN/PtgAreaN offsets
 * are interpreted against the queried cell, not the shared anchor.
 *
 * The returned rectangles are zero-based, closed intervals inside the current
 * worksheet. Exact duplicates are collapsed and the list is sorted by
 * (row_first, col_first, row_last, col_last). Overlapping but distinct
 * rectangles are kept separate. Cells referenced by the formula are not
 * themselves followed - only the formula's own direct references are listed.
 *
 * Status values:
 *  XLS_FORMULA_QUERY_OK             the query succeeded; the list may be empty
 *  XLS_FORMULA_QUERY_NOT_A_FORMULA  the cell holds no FORMULA record (a plain
 *                                   number, label or blank cell). The list is
 *                                   empty; this is distinct from a formula
 *                                   that legitimately references nothing.
 *  XLS_FORMULA_QUERY_NO_REFERENCES  a well-formed formula without any static
 *                                   cell/area reference (constants only, etc.)
 *  XLS_FORMULA_QUERY_UNSUPPORTED    the formula contains a construct whose
 *                                   dependencies cannot be reported statically
 *                                   (defined names, external/3D references,
 *                                   INDIRECT/OFFSET, array constants, error or
 *                                   otherwise unsupported tokens). The list is
 *                                   intentionally empty: no partial result is
 *                                   returned. Cached values remain readable
 *                                   through the regular cell API.
 *  XLS_FORMULA_QUERY_OUT_OF_BOUNDS  a signed relative offset resolves outside
 *                                   the 65536 x 256 sheet under the strict
 *                                   non-wrapping policy. No BIFF8 wrap-around
 *                                   is applied.
 *  XLS_FORMULA_QUERY_CORRUPT        the formula metadata is malformed (missing
 *                                   shared anchor, duplicate SHRFMLA, usage
 *                                   cell outside the declared range, truncated
 *                                   token stream or illegal token length).
 *                                   record_id/record_offset/token_offset point
 *                                   at the offending record/token when known.
 *  XLS_FORMULA_QUERY_ERROR          bad arguments or an allocation failure.
 *
 * On every status except XLS_FORMULA_QUERY_ERROR a result object is allocated
 * and written to *out; ownership passes to the caller, which must release it
 * with xls_free_formula_dependencies(). Each query yields an independent copy,
 * so freeing a result, closing the worksheet and issuing further queries never
 * interfere with one another.
 */
typedef struct xls_formula_dependency
{
    WORD row_first;
    WORD row_last;
    WORD col_first;
    WORD col_last;
} xls_formula_dependency;

typedef enum {
    XLS_FORMULA_QUERY_OK = 0,
    XLS_FORMULA_QUERY_NOT_A_FORMULA,
    XLS_FORMULA_QUERY_NO_REFERENCES,
    XLS_FORMULA_QUERY_UNSUPPORTED,
    XLS_FORMULA_QUERY_OUT_OF_BOUNDS,
    XLS_FORMULA_QUERY_CORRUPT,
    XLS_FORMULA_QUERY_ERROR
} xls_formula_query_status;

typedef struct xls_formula_dependencies
{
    xls_formula_query_status status;
    DWORD count;
    xls_formula_dependency *dependencies;

    /* Diagnostics, populated for XLS_FORMULA_QUERY_CORRUPT and
     * XLS_FORMULA_QUERY_UNSUPPORTED; zero otherwise. record_id is the BIFF
     * record id (e.g. 0x0006 FORMULA or 0x04BC SHRFMLA), record_offset is the
     * position of the record header in the worksheet stream, and
     * token_offset is the byte offset of the offending token inside the
     * formula token blob. */
    WORD record_id;
    DWORD record_offset;
    DWORD token_offset;
} xls_formula_dependencies;

xls_formula_query_status xls_get_formula_dependencies(xlsWorkSheet* pWS,
        WORD row, WORD col, xls_formula_dependencies **out);
void xls_free_formula_dependencies(xls_formula_dependencies *result);

#ifdef __cplusplus
} // extern c block
} // namespace
#endif

#endif

