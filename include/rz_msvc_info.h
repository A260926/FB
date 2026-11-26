// SPDX-FileCopyrightText: 2026 RizinOrg <info@rizin.re>
// SPDX-License-Identifier: LGPL-3.0-only

#ifndef RZ_MSVC_INFO_H
#define RZ_MSVC_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rz_libdemangle.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Structured (machine readable) view over the MSVC / Microsoft C++ mangling
 * scheme.
 *
 * libdemangle_msvc_info() runs the very same parser that backs
 * libdemangle_handler_msvc(), but instead of only handing back the display
 * string it also reports the "this" adjustment applied by a thunk together
 * with the raw adjustment numbers found in the mangled symbol.
 *
 * All strings in the returned structure are owned by the structure.  They stay
 * valid until libdemangle_msvc_info_free() is called, independently of the
 * input buffer (the caller may free the input string right after the call).
 */

/* Error categories reported through RzMsvcInfo::err. */
typedef enum {
	RZ_MSVC_INFO_OK = 0,
	RZ_MSVC_INFO_ERR_INVALID_ARGUMENT, ///< NULL symbol
	RZ_MSVC_INFO_ERR_MEMORY, ///< out of memory
	RZ_MSVC_INFO_ERR_NOT_MSVC, ///< does not look like a Microsoft mangled symbol
	RZ_MSVC_INFO_ERR_SYNTAX, ///< malformed symbol
	RZ_MSVC_INFO_ERR_UNSUPPORTED, ///< valid but unsupported construct
	RZ_MSVC_INFO_ERR_TRAILING_GARBAGE, ///< symbol did not end where the parser stopped
	RZ_MSVC_INFO_ERR_NUMBER_WIDTH, ///< adjustment number does not fit in 32 bits
	RZ_MSVC_INFO_ERR_NUMBER_TERMINATOR, ///< adjustment number is missing its '@' terminator
	RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUSTMENT, ///< a required adjustment field is absent
} RzMsvcInfoErr;

/* Which kind of "this" adjustment the symbol describes. */
typedef enum {
	RZ_MSVC_THUNK_NONE = 0, ///< not a thunk, no adjustment
	RZ_MSVC_THUNK_STATIC_ADJUSTOR, ///< $<n> free function / plain adjustor thunk
	RZ_MSVC_THUNK_VTORDISP, ///< $0 .. $5  (vtordisp)
	RZ_MSVC_THUNK_VTORDISPEX, ///< $R0 .. $R5 (vtordispex)
} RzMsvcThunkKind;

/* Member access level encoded by the function kind / thunk selector. */
typedef enum {
	RZ_MSVC_ACCESS_NONE = 0,
	RZ_MSVC_ACCESS_PRIVATE,
	RZ_MSVC_ACCESS_PROTECTED,
	RZ_MSVC_ACCESS_PUBLIC,
} RzMsvcAccess;

/*
 * Slots inside RzMsvcInfo::adjust.
 *
 * The numbers are always laid out in mangled-symbol order:
 *   static adjustor: [STATIC_OFFSET]
 *   vtordisp:        [VTORDISP, STATIC_OFFSET]
 *   vtordispex:      [VBPTR, VBTABLE, VTORDISP, STATIC_OFFSET]
 * Slots that the symbol does not carry keep present == false.
 */
typedef enum {
	RZ_MSVC_ADJ_VBPTR = 0, ///< vbptr displacement (vtordispex only)
	RZ_MSVC_ADJ_VBTABLE, ///< vbtable slot displacement (vtordispex only)
	RZ_MSVC_ADJ_VTORDISP, ///< vtordisp displacement (vtordisp / vtordispex)
	RZ_MSVC_ADJ_STATIC_OFFSET, ///< static offset (all thunk kinds)
	RZ_MSVC_ADJ_FIELD_COUNT
} RzMsvcAdjustField;

/*
 * One adjustment number.
 *
 * value   signed 32 bit interpretation of the MSVC number:
 *          - an explicit '?' prefix negates the following magnitude;
 *          - without '?' the 32 bit pattern is interpreted as two's
 *            complement, so "PPPPPPPM@" (0xfffffffc) yields -4 and not
 *            4294967292.
 * begin/end half-open byte range of the token inside the original input
 *          string (including the optional '?' and the '@' terminator).
 * present false means the field is absent; value is then irrelevant.
 */
typedef struct {
	bool present;
	int32_t value;
	size_t begin;
	size_t end;
} RzMsvcAdjustValue;

typedef struct RzMsvcInfo {
	RzMsvcInfoErr err; ///< RZ_MSVC_INFO_OK on success
	size_t err_offset; ///< byte offset in the input for the reported error
	char *display; ///< owned demangled text, NULL on error

	bool is_thunk; ///< true for any adjustor / vtordisp / vtordispex thunk
	RzMsvcThunkKind thunk_kind; ///< kind of adjustment
	RzMsvcAccess access; ///< encoded member access level
	bool is_far; ///< far (odd vtordisp/vtordispex selector) thunk

	/*
	 * cv-qualification of the implicit "this" pointer taken from the member
	 * function access code ("", "const", "volatile" or "const volatile").
	 * Always owned, never NULL on success.
	 */
	char *this_modifier;

	RzMsvcAdjustValue adjust[RZ_MSVC_ADJ_FIELD_COUNT];
} RzMsvcInfo;

/*
 * Parse a Microsoft mangled symbol and return its structured description.
 * The whole input must be consumed; trailing bytes are reported as
 * RZ_MSVC_INFO_ERR_TRAILING_GARBAGE.
 *
 * Returns NULL only on allocation failure (or when 'symbol' itself cannot be
 * allocated around); every parse problem is reported through the returned
 * structure with err != RZ_MSVC_INFO_OK.
 */
DEM_LIB_EXPORT RzMsvcInfo *libdemangle_msvc_info(const char *symbol, RzDemangleOpts opts);

/* Release a structure returned by libdemangle_msvc_info(). Passing NULL is ok. */
DEM_LIB_EXPORT void libdemangle_msvc_info_free(RzMsvcInfo *info);

#ifdef __cplusplus
}
#endif

#endif /* RZ_MSVC_INFO_H */
