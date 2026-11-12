/*
 * Internal interface for BIFF8 formula dependency collection.
 *
 * The public query API lives in include/xls.h. This header is only used by
 * src/xls.c while a worksheet is being parsed; none of these symbols are
 * exported as part of the documented API.
 *
 * The metadata accumulated here is owned by the worksheet: every
 * xlsWorkSheet gets its own context, and xls_close_WS() releases it. The
 * global formula handler is deliberately bypassed - dependency collection
 * must not depend on a user-installed callback.
 */
#ifndef XLS_FORMULA_DEPS_INTERNAL_H
#define XLS_FORMULA_DEPS_INTERNAL_H

#include "../include/libxls/xlsstruct.h"

/* Allocate the per-worksheet collection context. */
void xls_fd_init(xlsWorkSheet *pWS);

/*
 * Feed one worksheet record. Only FORMULA and SHRFMLA records are retained;
 * everything else is ignored. Failures are recorded on the affected formula
 * cell and reported lazily by xls_get_formula_dependencies(), never raised
 * through the historical worksheet parse path, so a single bad formula cannot
 * discard cached values held by neighbouring cells.
 *
 * record_offset is the stream position of the record header, kept for
 * corruption diagnostics.
 */
void xls_fd_record(xlsWorkSheet *pWS, WORD id, WORD size, const BYTE *buf,
        DWORD record_offset);

/* Release the collection context and everything it owns. */
void xls_fd_free(xlsWorkSheet *pWS);

#endif
