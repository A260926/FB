// SPDX-FileCopyrightText: 2024 RizinOrg <info@rizin.re>
// SPDX-License-Identifier: LGPL-3.0-only

#ifndef RZ_MSVC_INFO_H
#define RZ_MSVC_INFO_H

#include <rz_libdemangle.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file rz_msvc_info.h
 *
 * Structured access to the Microsoft C++ (MSVC) mangling scheme.
 *
 * libdemangle_handler_msvc() returns a human readable string only.  That
 * representation is not usable by callers (for example call-graph analysis)
 * that need to know whether a symbol is a thunk and how the `this` pointer
 * has to be adjusted before the real function can be invoked.
 *
 * libdemangle_msvc_info() runs the very same production parser as the old
 * handler (there is no second, regular-expression based decoder) and returns
 * a result which owns its display string and carries the adjustment data as
 * machine readable fields.
 *
 * For a regular (non thunk) function the adjustment kind is
 * RZ_MSVC_ADJUST_NONE and the display string is the same as the one produced
 * by libdemangle_handler_msvc().
 */

/**
 * \brief Kind of `this` pointer adjustment encoded in the mangled symbol.
 *
 * MSVC emits four shapes:
 *
 *   `?f@C@@WBA@EAAHXZ`                  static adjustor thunk
 *       The access code (G/H/O/P/W/X) is followed by one number: the static
 *       offset which has to be added to `this`.
 *
 *   `??_EDerived@@$4PPPPPPPM@A@EAAPEAXI@Z`  vtordisp thunk
 *       `$0` .. `$5` introduce a two number prefix:
 *         [0] vtordisp displacement
 *         [1] static offset
 *
 *   `?f@A@simple@@$R477PPPPPPPM@7AEXXZ`     vtordispex thunk
 *       `$R0` .. `$R5` introduce a four number prefix:
 *         [0] vbptr displacement
 *         [1] vbtable slot displacement
 *         [2] vtordisp displacement
 *         [3] static offset
 *
 * vcall-only symbols are not decoded by this API.
 */
typedef enum {
	RZ_MSVC_ADJUST_NONE = 0, ///< regular function, no adjustment
	RZ_MSVC_ADJUST_STATIC, ///< static adjustor thunk, one static offset
	RZ_MSVC_ADJUST_VTORDISP, ///< vtordisp thunk, vtordisp + static offset
	RZ_MSVC_ADJUST_VTORDISPEX, ///< vtordispex thunk, vbptr + vbtable + vtordisp + static offset
} RzMsvcAdjustKind;

/**
 * \brief Access level of the function.
 *
 * For regular functions this comes from the member function access code
 * (A..H private, I..P protected, Q..X public, Y/Z none).
 * For vtordisp/vtordispex thunks it comes from the virtual adjustment
 * prefix, where `$0/$1` are private, `$2/$3` protected and `$4/$5` public.
 */
typedef enum {
	RZ_MSVC_ACCESS_NONE = 0,
	RZ_MSVC_ACCESS_PRIVATE,
	RZ_MSVC_ACCESS_PROTECTED,
	RZ_MSVC_ACCESS_PUBLIC,
} RzMsvcAccess;

/**
 * \brief Near/far member function indicator.
 *
 * For member function access codes the even letters (A, C, E, ...) are
 * near and the odd letters (B, D, F, ...) are far.  For vtordisp/vtordispex
 * prefixes the even adjustment index (`$0`, `$2`, `$4` / `$R0`, ...) is near
 * and the odd one (`$1`, `$3`, `$5` / `$R1`, ...) is far.
 */
typedef enum {
	RZ_MSVC_DISTANCE_NONE = 0,
	RZ_MSVC_DISTANCE_NEAR,
	RZ_MSVC_DISTANCE_FAR,
} RzMsvcDistance;

/**
 * \brief Qualification of the implicit `this` pointer.
 *
 * The plain/const/volatile storage class letter which follows the member
 * function access code.  RZ_MSVC_THIS_NONE means that the function has no
 * implicit `this` pointer at all (global function or static member).
 */
typedef enum {
	RZ_MSVC_THIS_NONE = 0,
	RZ_MSVC_THIS_PLAIN, ///< `A`  - no const/volatile qualification
	RZ_MSVC_THIS_CONST, ///< `B`/`J`
	RZ_MSVC_THIS_VOLATILE, ///< `C`/`G`/`K`
	RZ_MSVC_THIS_CONST_VOLATILE, ///< `D`/`H`/`L`
} RzMsvcThisModifier;

/**
 * \brief One MSVC encoded number used by an adjustment prefix.
 *
 * `present` is false when the field does not exist for the given adjustment
 * kind (for example vtordispex has a vbptr field while vtordisp does not).
 * An absent field is never represented by a zero value silently stored as a
 * real adjustment.
 *
 * `start` and `end` form a half open byte range [start, end) inside the
 * original, unmodified mangled symbol passed to libdemangle_msvc_info().
 * `value` is always the signed 32 bit interpretation of the encoded number:
 * a number without an explicit `?` sign is interpreted as a two's complement
 * 32 bit pattern (`PPPPPPPM@` therefore yields -4 and not 4294967292).
 */
typedef struct {
	bool present;
	int32_t value;
	size_t start;
	size_t end;
} RzMsvcNumber;

/**
 * \brief Error categories returned by libdemangle_msvc_info().
 *
 * `error_offset` in RzMsvcInfo is a zero based offset into the original
 * input string.  For number related errors it points at the first byte of
 * the malformed number, for RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUST at the byte
 * where the missing field was expected, and for
 * RZ_MSVC_INFO_ERR_TRAILING at the first unconsumed byte.
 */
typedef enum {
	RZ_MSVC_INFO_ERR_NONE = 0, ///< parsing succeeded
	RZ_MSVC_INFO_ERR_NOT_MSVC, ///< input is not an MSVC mangled symbol
	RZ_MSVC_INFO_ERR_NUMBER, ///< malformed number (missing `@` terminator, bad digits, ...)
	RZ_MSVC_INFO_ERR_NUMBER_RANGE, ///< number does not fit into a signed 32 bit value
	RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUST, ///< adjustment prefix ended before all fields were present
	RZ_MSVC_INFO_ERR_TRAILING, ///< trailing bytes after a complete symbol
	RZ_MSVC_INFO_ERR_SYNTAX, ///< any other malformed symbol
	RZ_MSVC_INFO_ERR_MEMORY, ///< allocation failure
} RzMsvcInfoError;

/**
 * \brief Machine readable result of an MSVC symbol decode.
 *
 * The structure and everything it points to are owned by the caller after a
 * successful libdemangle_msvc_info() call and must be released with
 * libdemangle_msvc_info_free().  libdemangle_msvc_info() never returns NULL
 * for a non-NULL input: parsing errors are reported through `error` and
 * `error_offset` so callers can distinguish malformed input from OOM.
 *
 * `display` is NULL when parsing failed.
 */
typedef struct {
	char *display; ///< owned human readable signature (same style as the old handler)
	bool is_thunk; ///< true for static adjustor, vtordisp and vtordispex thunks
	RzMsvcAdjustKind kind; ///< adjustment kind
	RzMsvcAccess access; ///< access level
	RzMsvcDistance distance; ///< near/far indicator
	RzMsvcThisModifier this_modifier; ///< const/volatile qualification of `this`
	RzMsvcNumber static_offset; ///< static adjustor offset (STATIC/VTORDISP/VTORDISPEX)
	RzMsvcNumber vtordisp; ///< vtordisp displacement (VTORDISP/VTORDISPEX)
	RzMsvcNumber vbptr; ///< vbptr displacement (VTORDISPEX only)
	RzMsvcNumber vbtable_slot; ///< vbtable slot displacement (VTORDISPEX only)
	RzMsvcInfoError error; ///< parsing result category
	size_t error_offset; ///< offset of the offending byte in the original input
} RzMsvcInfo;

/**
 * \brief Decode an MSVC mangled symbol into a structured result.
 *
 * The whole input must be consumed: trailing bytes after a complete symbol
 * are rejected with RZ_MSVC_INFO_ERR_TRAILING.  This is intentionally
 * stricter than libdemangle_handler_msvc(), whose historical lenient
 * behaviour is kept unchanged.
 *
 * \param symbol Mangled symbol (must not be NULL)
 * \param opts   Same options accepted by libdemangle_handler_msvc()
 * \return owned result, or NULL on allocation failure / NULL input
 */
DEM_LIB_EXPORT RzMsvcInfo *libdemangle_msvc_info(const char *symbol, RzDemangleOpts opts);

/**
 * \brief Release a result returned by libdemangle_msvc_info().
 *
 * Frees the owned display string and the structure itself.  Passing NULL is
 * safe.  The original input string is never referenced after the call, so
 * it may already have been freed by the caller.
 */
DEM_LIB_EXPORT void libdemangle_msvc_info_free(RzMsvcInfo *info);

#ifdef __cplusplus
}
#endif

#endif /* RZ_MSVC_INFO_H */
