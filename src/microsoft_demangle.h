// SPDX-FileCopyrightText: 2015 inisider <inisider@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-only

#ifndef MICROSOFT_DEMANGLE_H
#define MICROSOFT_DEMANGLE_H

#include "demangler_util.h"
#include "demangler_types.h"

///////////////////////////////////////////////////////////////////////////////
/// \brief Kind of "this" adjustment described by a thunk symbol.
///////////////////////////////////////////////////////////////////////////////
typedef enum {
	MSVC_THUNK_KIND_NONE = 0, ///< not a thunk
	MSVC_THUNK_KIND_STATIC_ADJUSTOR, ///< the classic W/X adjustor form
	MSVC_THUNK_KIND_VTORDISP, ///< $0 .. $5
	MSVC_THUNK_KIND_VTORDISPEX, ///< $R0 .. $R5
} EMsvcThunkKind;

///////////////////////////////////////////////////////////////////////////////
/// \brief Member access encoded by the function kind / thunk selector.
///////////////////////////////////////////////////////////////////////////////
typedef enum {
	MSVC_ACCESS_NONE = 0,
	MSVC_ACCESS_PRIVATE,
	MSVC_ACCESS_PROTECTED,
	MSVC_ACCESS_PUBLIC,
} EMsvcAccess;

///////////////////////////////////////////////////////////////////////////////
/// \brief Slots of the adjustment vector, in mangled symbol order.
///////////////////////////////////////////////////////////////////////////////
typedef enum {
	MSVC_ADJUST_VBPTR = 0, ///< vtordispex only
	MSVC_ADJUST_VBTABLE, ///< vtordispex only
	MSVC_ADJUST_VTORDISP, ///< vtordisp / vtordispex
	MSVC_ADJUST_STATIC_OFFSET, ///< all thunk kinds
	MSVC_ADJUST_COUNT
} EMsvcAdjustField;

///////////////////////////////////////////////////////////////////////////////
/// \brief A single decoded adjustment number.
///////////////////////////////////////////////////////////////////////////////
typedef struct SMsvcAdjustValue {
	bool present; ///< false when the symbol does not carry this field
	bool negative; ///< an explicit '?' sign preceded the number
	int32_t value; ///< signed 32 bit two's complement value
	size_t begin; ///< half-open byte range inside the original symbol
	size_t end;
} SMsvcAdjustValue;

///////////////////////////////////////////////////////////////////////////////
/// \brief Detailed error category for adjustment number decoding.
///////////////////////////////////////////////////////////////////////////////
typedef enum {
	MSVC_ADJUST_ERR_NONE = 0,
	MSVC_ADJUST_ERR_NUMBER_WIDTH, ///< does not fit in 32 bits
	MSVC_ADJUST_ERR_NUMBER_TERMINATOR, ///< hex number without closing '@'
	MSVC_ADJUST_ERR_INCOMPLETE, ///< required adjustment field missing
	MSVC_ADJUST_ERR_SYNTAX, ///< unsupported selector / malformed field
} EMsvcAdjustErr;

///////////////////////////////////////////////////////////////////////////////
/// \brief Structured function-kind information recorded by the production
///        parser (parse_function_type). It is filled in whether the result is
///        a thunk or a plain function.
///////////////////////////////////////////////////////////////////////////////
typedef struct SMsvcFunctionInfo {
	bool is_thunk;
	EMsvcThunkKind thunk_kind;
	EMsvcAccess access;
	bool is_far;
	const char *this_modifier; ///< static storage, never NULL after parsing
	SMsvcAdjustValue adjust[MSVC_ADJUST_COUNT];

	bool adjust_failed;
	EMsvcAdjustErr adjust_err;
	size_t adjust_err_offset;

	/*
	 * Offset in the original symbol where a non-adjustment parse failure was
	 * detected, and the corresponding library error code. Zero/OK when the
	 * parse succeeded.
	 */
	bool parse_failed;
	size_t parse_err_offset;
	EDemanglerErr parse_err;
} SMsvcFunctionInfo;

///////////////////////////////////////////////////////////////////////////////
/// \brief Do demangle for microsoft mangling scheme. Demangled name need to be
///			free by user
/// \param demangler 'this' object of demangler
/// \param demangled_name Demangled name of symbol of demangler object
/// \return Returns OK if initialization has been finish with success, else one
///			of next errors: eDemanglerErrUnsupportedMangling, ...
///////////////////////////////////////////////////////////////////////////////
EDemanglerErr microsoft_demangle(SDemangler *demangler, char **demangled_name);

///////////////////////////////////////////////////////////////////////////////
/// \brief Same as microsoft_demangle() but also records the structured
///        function-kind / thunk information. The consumed character count is
///        reported through chars_read (relative to demangler->symbol) so the
///        caller can reject trailing garbage.
///////////////////////////////////////////////////////////////////////////////
EDemanglerErr microsoft_demangle_info(SDemangler *demangler, char **demangled_name, SMsvcFunctionInfo *info, size_t *chars_read);

#endif // MICROSOFT_DEMANGLE_H
