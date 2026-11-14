// SPDX-FileCopyrightText: 2024 RizinOrg <info@rizin.re>
// SPDX-License-Identifier: LGPL-3.0-only

// Structured MSVC thunk / this-adjustment tests.
//
// These tests exercise the public structured API (rz_msvc_info.h) and also
// assert that the historical string handler keeps producing the same output
// for every symbol it already supported.

#include "task_support/check.h"
#include <rz_libdemangle.h>
#include <rz_msvc_info.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Small helper: decode and free in one go only after assertions.
static RzMsvcInfo *decode(const char *sym) {
	RzMsvcInfo *info = libdemangle_msvc_info(sym, RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(info != NULL);
	return info;
}

static void check_number(const RzMsvcNumber *num, int32_t value, size_t start, size_t end) {
	TASK_CHECK(num->present);
	TASK_CHECK(num->value == value);
	TASK_CHECK(num->start == start);
	TASK_CHECK(num->end == end);
	(void)start;
	(void)end;
}

static void check_absent(const RzMsvcNumber *num) {
	TASK_CHECK(!num->present);
	TASK_CHECK(num->value == 0);
	TASK_CHECK(num->start == 0);
	TASK_CHECK(num->end == 0);
}

// The two symbols from the task statement.  Both are decoded by the same
// production parser that the old handler uses.
static void test_adjustor_virtual(void) {
	const char *vtordisp_sym = "??_EDerived@@$4PPPPPPPM@A@EAAPEAXI@Z";
	RzMsvcInfo *info = decode(vtordisp_sym);
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(info->display != NULL);
	TASK_CHECK(info->is_thunk);
	TASK_CHECK(info->kind == RZ_MSVC_ADJUST_VTORDISP);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PUBLIC);
	TASK_CHECK(info->distance == RZ_MSVC_DISTANCE_NEAR);
	TASK_CHECK(info->this_modifier == RZ_MSVC_THIS_PLAIN);
	// vtordisp{-4,0}
	check_number(&info->vtordisp, -4, 15, 24);
	check_number(&info->static_offset, 0, 24, 26);
	check_absent(&info->vbptr);
	check_absent(&info->vbtable_slot);
	// Display keeps the old style and appends vtordisp{a,b}.
	TASK_CHECK(strstr(info->display, "[thunk]:public virtual:") != NULL);
	TASK_CHECK(strstr(info->display, "`vtordisp{-4,0}'") != NULL);
	TASK_CHECK(strstr(info->display, "Derived::vector_dtor") != NULL);
	// The old handler must see the same new symbol.
	char *legacy = libdemangle_handler_msvc(vtordisp_sym, RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(legacy != NULL);
	TASK_CHECK(strcmp(legacy, info->display) == 0);
	free(legacy);
	libdemangle_msvc_info_free(info);

	const char *ex_sym = "?f@A@simple@@$R477PPPPPPPM@7AEXXZ";
	info = decode(ex_sym);
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(info->display != NULL);
	TASK_CHECK(info->is_thunk);
	TASK_CHECK(info->kind == RZ_MSVC_ADJUST_VTORDISPEX);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PUBLIC);
	TASK_CHECK(info->distance == RZ_MSVC_DISTANCE_NEAR);
	TASK_CHECK(info->this_modifier == RZ_MSVC_THIS_PLAIN);
	// vtordispex{8,8,-4,8}
	check_number(&info->vbptr, 8, 16, 17);
	check_number(&info->vbtable_slot, 8, 17, 18);
	check_number(&info->vtordisp, -4, 18, 27);
	check_number(&info->static_offset, 8, 27, 28);
	TASK_CHECK(strstr(info->display, "`vtordispex{8,8,-4,8}'") != NULL);
	TASK_CHECK(strstr(info->display, "simple::A::f") != NULL);
	legacy = libdemangle_handler_msvc(ex_sym, RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(legacy != NULL);
	TASK_CHECK(strcmp(legacy, info->display) == 0);
	free(legacy);
	libdemangle_msvc_info_free(info);
}

// Access level and near/far selection for every vtordisp index, including
// that the legacy string content matches the old style.
static void test_virtual_access_and_distance(void) {
	// $0/$1 private, $2/$3 protected, $4/$5 public; odd is far.
	static const char *symbols[] = {
		"??_EC@@$0PPPPPPPM@A@EAAXXZ",
		"??_EC@@$1PPPPPPPM@A@EAAXXZ",
		"??_EC@@$2PPPPPPPM@A@EAAXXZ",
		"??_EC@@$3PPPPPPPM@A@EAAXXZ",
		"??_EC@@$4PPPPPPPM@A@EAAXXZ",
		"??_EC@@$5PPPPPPPM@A@EAAXXZ",
	};
	static const RzMsvcAccess expected_access[] = {
		RZ_MSVC_ACCESS_PRIVATE, RZ_MSVC_ACCESS_PRIVATE,
		RZ_MSVC_ACCESS_PROTECTED, RZ_MSVC_ACCESS_PROTECTED,
		RZ_MSVC_ACCESS_PUBLIC, RZ_MSVC_ACCESS_PUBLIC
	};
	static const RzMsvcDistance expected_distance[] = {
		RZ_MSVC_DISTANCE_NEAR, RZ_MSVC_DISTANCE_FAR,
		RZ_MSVC_DISTANCE_NEAR, RZ_MSVC_DISTANCE_FAR,
		RZ_MSVC_DISTANCE_NEAR, RZ_MSVC_DISTANCE_FAR
	};
	for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); ++i) {
		RzMsvcInfo *info = decode(symbols[i]);
		TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
		TASK_CHECK(info->kind == RZ_MSVC_ADJUST_VTORDISP);
		TASK_CHECK(info->access == expected_access[i]);
		TASK_CHECK(info->distance == expected_distance[i]);
		TASK_CHECK(info->vtordisp.present);
		check_number(&info->vtordisp, -4, info->vtordisp.start,
			info->vtordisp.end); // self consistent range
		TASK_CHECK(info->vtordisp.end > info->vtordisp.start);
		TASK_CHECK(info->static_offset.present);
		TASK_CHECK(info->static_offset.start >= info->vtordisp.end);
		TASK_CHECK(info->static_offset.end > info->static_offset.start);
		libdemangle_msvc_info_free(info);
	}

	// Same rule for vtordispex ($R0..$R5).  Numbers are encoded as
	// B@ = 1, C@ = 2, D@ = 3 and 7 = 8, so the fields stay easy to read.
	static const char *ex_symbols[] = {
		"??_EC@@$R0B@C@D@7AEXXZ",
		"??_EC@@$R1B@C@D@7AEXXZ",
		"??_EC@@$R2B@C@D@7AEXXZ",
		"??_EC@@$R3B@C@D@7AEXXZ",
		"??_EC@@$R4B@C@D@7AEXXZ",
		"??_EC@@$R5B@C@D@7AEXXZ",
	};
	for (size_t i = 0; i < sizeof(ex_symbols) / sizeof(ex_symbols[0]); ++i) {
		RzMsvcInfo *info = decode(ex_symbols[i]);
		TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
		TASK_CHECK(info->kind == RZ_MSVC_ADJUST_VTORDISPEX);
		TASK_CHECK(info->access == expected_access[i]);
		TASK_CHECK(info->distance == expected_distance[i]);
		check_number(&info->vbptr, 1, 10, 12);
		check_number(&info->vbtable_slot, 2, 12, 14);
		check_number(&info->vtordisp, 3, 14, 16);
		check_number(&info->static_offset, 8, 16, 17);
		libdemangle_msvc_info_free(info);
	}

	// Static adjustor thunks inherit access and near/far from the access
	// letter: G/H private, O/P protected, W/X public; odd letters are far.
	static const char *static_syms[] = {
		"??_EC@@G7EAAXXZ", "??_EC@@H7EAAXXZ",
		"??_EC@@O7EAAXXZ", "??_EC@@P7EAAXXZ",
		"??_EC@@W7EAAXXZ", "??_EC@@X7EAAXXZ",
		"?f@C@@QEAAHXZ", // non thunk, public
		"?f@C@@YAAHXZ", // non thunk, no access level, near
		"?f@C@@ZAAHXZ", // non thunk, no access level, far
	};
	static const RzMsvcAdjustKind kinds[] = {
		RZ_MSVC_ADJUST_STATIC, RZ_MSVC_ADJUST_STATIC,
		RZ_MSVC_ADJUST_STATIC, RZ_MSVC_ADJUST_STATIC,
		RZ_MSVC_ADJUST_STATIC, RZ_MSVC_ADJUST_STATIC,
		RZ_MSVC_ADJUST_NONE, RZ_MSVC_ADJUST_NONE, RZ_MSVC_ADJUST_NONE
	};
	static const RzMsvcAccess accs[] = {
		RZ_MSVC_ACCESS_PRIVATE, RZ_MSVC_ACCESS_PRIVATE,
		RZ_MSVC_ACCESS_PROTECTED, RZ_MSVC_ACCESS_PROTECTED,
		RZ_MSVC_ACCESS_PUBLIC, RZ_MSVC_ACCESS_PUBLIC,
		RZ_MSVC_ACCESS_PUBLIC,
		RZ_MSVC_ACCESS_NONE, RZ_MSVC_ACCESS_NONE
	};
	static const RzMsvcDistance dists[] = {
		RZ_MSVC_DISTANCE_NEAR, RZ_MSVC_DISTANCE_FAR,
		RZ_MSVC_DISTANCE_NEAR, RZ_MSVC_DISTANCE_FAR,
		RZ_MSVC_DISTANCE_NEAR, RZ_MSVC_DISTANCE_FAR,
		RZ_MSVC_DISTANCE_NEAR,
		RZ_MSVC_DISTANCE_NEAR, RZ_MSVC_DISTANCE_FAR
	};
	for (size_t i = 0; i < sizeof(static_syms) / sizeof(static_syms[0]); ++i) {
		RzMsvcInfo *info = decode(static_syms[i]);
		TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
		TASK_CHECK(info->kind == kinds[i]);
		TASK_CHECK(info->access == accs[i]);
		TASK_CHECK(info->distance == dists[i]);
		TASK_CHECK(info->is_thunk == (kinds[i] == RZ_MSVC_ADJUST_STATIC));
		if (kinds[i] == RZ_MSVC_ADJUST_STATIC) {
			check_number(&info->static_offset, 8, 8, 9);
			check_absent(&info->vtordisp);
		} else {
			check_absent(&info->static_offset);
		}
		libdemangle_msvc_info_free(info);
	}
}

// Signed boundaries and the various number encodings.
//
// Reminder: A=0, B=1, ... P=15.  A hexadecimal token is terminated by '@',
// while the digits 0..9 are self terminating and encode 1..10.
static void test_numbers(void) {
	// Largest positive two's complement pattern: 0x7FFFFFFF.
	// Horner nibbles 7,F,F,F,F,F,F,F -> "HPPPPPPP@" (H=7, P=15).
	RzMsvcInfo *info = decode("?f@C@@WHPPPPPPP@EAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&info->static_offset, 0x7fffffff, 7, 16);
	libdemangle_msvc_info_free(info);

	// PPPPPPPP@ -> 0xFFFFFFFF -> -1.
	info = decode("?f@C@@WPPPPPPPP@EAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&info->static_offset, -1, 7, 16);
	libdemangle_msvc_info_free(info);

	// IAAAAAAA@ -> 0x80000000 -> INT32_MIN.
	info = decode("?f@C@@WIAAAAAAA@EAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&info->static_offset, INT32_MIN, 7, 16);
	libdemangle_msvc_info_free(info);

	// @ alone encodes 0.
	info = decode("?f@C@@W@EAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&info->static_offset, 0, 7, 8);
	libdemangle_msvc_info_free(info);

	// Single digit encodings: 0..8 -> 1..9, 9 -> 10.
	static const struct {
		const char *sym;
		int32_t value;
	} digits[] = {
		{ "?f@C@@W0EAAHXZ", 1 },
		{ "?f@C@@W8EAAHXZ", 9 },
		{ "?f@C@@W9EAAHXZ", 10 },
	};
	for (size_t i = 0; i < sizeof(digits) / sizeof(digits[0]); ++i) {
		info = decode(digits[i].sym);
		TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
		check_number(&info->static_offset, digits[i].value, 7, 8);
		libdemangle_msvc_info_free(info);
	}

	// A@ encodes 0 as a hexadecimal token (A=0), B@ encodes 1.
	info = decode("?f@C@@WA@EAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&info->static_offset, 0, 7, 9);
	libdemangle_msvc_info_free(info);

	info = decode("?f@C@@WB@EAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&info->static_offset, 1, 7, 9);
	libdemangle_msvc_info_free(info);

	// Explicit negative sign.
	info = decode("?f@C@@W?0EAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&info->static_offset, -1, 7, 9);
	libdemangle_msvc_info_free(info);

	// Negative magnitude that fits: BA@ is 0x10 = 16.
	info = decode("?f@C@@W?BA@EAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&info->static_offset, -16, 7, 11);
	libdemangle_msvc_info_free(info);

	// -2147483648 is representable (magnitude 0x80000000), -2147483649 is not.
	info = decode("?f@C@@W?IAAAAAAA@EAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&info->static_offset, INT32_MIN, 7, 17);
	libdemangle_msvc_info_free(info);

	info = decode("?f@C@@W?IAAAAAAB@EAAHXZ");
	TASK_CHECK(info->display == NULL);
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NUMBER_RANGE);
	TASK_CHECK(info->error_offset == 7);
	libdemangle_msvc_info_free(info);

	// Nine hexadecimal digits can never fit into 32 bits.
	info = decode("?f@C@@WPPPPPPPPPM@EAAHXZ");
	TASK_CHECK(info->display == NULL);
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NUMBER_RANGE);
	TASK_CHECK(info->error_offset == 7);
	libdemangle_msvc_info_free(info);

	// Hexadecimal token without the terminating '@' ('Z' is neither a hex
	// digit nor the terminator).
	info = decode("?f@C@@WPPPPPPPPZEAAHXZ");
	TASK_CHECK(info->display == NULL);
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NUMBER);
	TASK_CHECK(info->error_offset == 7);
	libdemangle_msvc_info_free(info);

	// A malformed adjustment must not disturb a following clean decode.
	RzMsvcInfo *good = decode("?f@C@@W7EAAHXZ");
	TASK_CHECK(good->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&good->static_offset, 8, 7, 8);
	TASK_CHECK(good->display != NULL);
	libdemangle_msvc_info_free(good);
}

// Truncated adjustment prefixes and strict full-symbol consumption.
static void test_adjust_errors(void) {
	// vtordisp requiring two numbers, only one present.
	RzMsvcInfo *info = decode("??_EC@@$4PPPPPPPM@");
	TASK_CHECK(info->display == NULL);
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUST);
	TASK_CHECK(info->error_offset == 18);
	libdemangle_msvc_info_free(info);

	// vtordisp, second number malformed (no '@').
	info = decode("??_EC@@$4PPPPPPPM@EAAHXZ");
	TASK_CHECK(info->display == NULL);
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NUMBER);
	TASK_CHECK(info->error_offset == 18);
	libdemangle_msvc_info_free(info);

	// vtordispex requiring four numbers, only two present.
	info = decode("??_EC@@$R478AEXXZ");
	TASK_CHECK(info->display == NULL);
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NUMBER ||
		info->error == RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUST);
	libdemangle_msvc_info_free(info);

	// Static thunk truncated right after the access letter.
	info = decode("?f@C@@W");
	TASK_CHECK(info->display == NULL);
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUST);
	TASK_CHECK(info->error_offset == 7);
	libdemangle_msvc_info_free(info);

	// Strict entry point: trailing garbage after a complete symbol.
	info = decode("??_EDerived@@$4PPPPPPPM@A@EAAPEAXI@Ztrailing");
	TASK_CHECK(info->display == NULL);
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_TRAILING);
	TASK_CHECK(info->error_offset == 36);
	libdemangle_msvc_info_free(info);

	// The old handler keeps its historical lenient behaviour: it does not
	// reject the trailing bytes, but is not required to produce a value here.
	char *legacy = libdemangle_handler_msvc("?public_func@TEST_CLASS@@QEAAHXZ", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(legacy != NULL);
	free(legacy);
}

// Regular signatures still flow through the production function parser,
// including templates, repeated argument abbreviations, and the member
// function access/this modifiers.
static void test_signatures(void) {
	const char *sym = "?public_func@TEST_CLASS@@QEAAHXZ";
	RzMsvcInfo *info = decode(sym);
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(!info->is_thunk);
	TASK_CHECK(info->kind == RZ_MSVC_ADJUST_NONE);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PUBLIC);
	TASK_CHECK(info->distance == RZ_MSVC_DISTANCE_NEAR);
	TASK_CHECK(info->this_modifier == RZ_MSVC_THIS_PLAIN);
	check_absent(&info->static_offset);
	check_absent(&info->vtordisp);
	check_absent(&info->vbptr);
	check_absent(&info->vbtable_slot);
	char *legacy = libdemangle_handler_msvc(sym, RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(legacy != NULL);
	TASK_CHECK(strcmp(legacy, info->display) == 0);
	free(legacy);
	libdemangle_msvc_info_free(info);

	// const member function -> const `this`.
	info = decode("?f@C@@QEBAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(info->this_modifier == RZ_MSVC_THIS_CONST);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PUBLIC);
	libdemangle_msvc_info_free(info);

	// Static member function -> no implicit this.
	info = decode("?static_func@TEST_CLASS@@SAXH@Z");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(info->this_modifier == RZ_MSVC_THIS_NONE);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PUBLIC);
	TASK_CHECK(strstr(info->display, "static") != NULL);
	libdemangle_msvc_info_free(info);

	// Global function -> no access level, no this.
	info = decode("?SomeFunction@@YAPAVClass1@@PAV1@PAVClass2@@1AAV2@@Z");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_NONE);
	TASK_CHECK(info->this_modifier == RZ_MSVC_THIS_NONE);
	// Repeated argument abbreviations must still be expanded.
	TASK_CHECK(strstr(info->display, "class Class1 *") != NULL);
	TASK_CHECK(strstr(info->display, "class Class2 *") != NULL);
	TASK_CHECK(strstr(info->display, "class Class2 &") != NULL);
	libdemangle_msvc_info_free(info);

	// Template member signature; the new adjustment fields must not eat the
	// template scope nor the back references.
	info = decode("??$f@H@C@@WBA@EAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(info->is_thunk);
	TASK_CHECK(info->kind == RZ_MSVC_ADJUST_STATIC);
	check_number(&info->static_offset, 16, 11, 14);
	TASK_CHECK(strstr(info->display, "C::f<int>") != NULL);
	libdemangle_msvc_info_free(info);

	// Template + vtordispex.
	info = decode("??$g@H@ns@@$R477PPPPPPPM@7AEXXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(info->kind == RZ_MSVC_ADJUST_VTORDISPEX);
	check_number(&info->vbptr, 8, 14, 15);
	check_number(&info->vbtable_slot, 8, 15, 16);
	check_number(&info->vtordisp, -4, 16, 25);
	check_number(&info->static_offset, 8, 25, 26);
	TASK_CHECK(strstr(info->display, "ns::g<int>") != NULL);
	libdemangle_msvc_info_free(info);

	// Member function with repeated parameters.
	info = decode("?f@C@@WBA@EAAHH@Z");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(strstr(info->display, "(int)") != NULL);
	libdemangle_msvc_info_free(info);

	// const/volatile qualification of `this` on a thunk.
	info = decode("?f@C@@WBA@EBAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(info->this_modifier == RZ_MSVC_THIS_CONST);
	TASK_CHECK(info->is_thunk);
	check_number(&info->static_offset, 16, 7, 10);
	libdemangle_msvc_info_free(info);

	info = decode("?f@C@@WBA@ECAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(info->this_modifier == RZ_MSVC_THIS_VOLATILE);
	libdemangle_msvc_info_free(info);

	// A function type used as a template argument may contain its own
	// adjustment.  The outer symbol's thunk information must win.
	info = decode("??$f@P6AX$4PPPPPPPM@A@EAAXXZ@C@@WBA@EAAHXZ");
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(info->kind == RZ_MSVC_ADJUST_STATIC);
	TASK_CHECK(!info->vtordisp.present);
	check_number(&info->static_offset, 16, 33, 36);
	libdemangle_msvc_info_free(info);
}

// The result must own its display text: freeing the input first must not
// invalidate it, and interleaved success/failure decodes must be independent.
static void test_ownership_and_isolation(void) {
	char *input = strdup("??_EDerived@@$4PPPPPPPM@A@EAAPEAXI@Z");
	TASK_CHECK(input != NULL);
	RzMsvcInfo *info = decode(input);
	free(input); // input no longer exists
	TASK_CHECK(info->error == RZ_MSVC_INFO_ERR_NONE);
	TASK_CHECK(info->display != NULL);
	TASK_CHECK(strstr(info->display, "vtordisp{-4,0}") != NULL);
	libdemangle_msvc_info_free(info);

	// Interleave successes and failures; later results must not inherit any
	// state from earlier ones.
	RzMsvcInfo *bad1 = decode("?f@C@@WPPPPPPPMEAAHXZ");
	TASK_CHECK(bad1->error == RZ_MSVC_INFO_ERR_NUMBER);
	RzMsvcInfo *good1 = decode("?f@C@@W7EAAHXZ");
	TASK_CHECK(good1->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&good1->static_offset, 8, 7, 8);
	RzMsvcInfo *bad2 = decode("?f@C@@W");
	TASK_CHECK(bad2->error == RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUST);
	RzMsvcInfo *good2 = decode("?f@C@@W@EAAHXZ");
	TASK_CHECK(good2->error == RZ_MSVC_INFO_ERR_NONE);
	check_number(&good2->static_offset, 0, 7, 8);
	TASK_CHECK(good2->display != NULL);
	check_absent(&good1->vtordisp);
	check_absent(&good2->vtordisp);
	libdemangle_msvc_info_free(bad1);
	libdemangle_msvc_info_free(good1);
	libdemangle_msvc_info_free(bad2);
	libdemangle_msvc_info_free(good2);

	// NULL input is tolerated.
	RzMsvcInfo *null_info = libdemangle_msvc_info(NULL, RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(null_info == NULL);

	// Freeing NULL is safe.
	libdemangle_msvc_info_free(NULL);

	// Not an MSVC symbol.
	RzMsvcInfo *not_msvc = decode("hello");
	TASK_CHECK(not_msvc->display == NULL);
	TASK_CHECK(not_msvc->error == RZ_MSVC_INFO_ERR_NOT_MSVC);
	TASK_CHECK(not_msvc->kind == RZ_MSVC_ADJUST_NONE);
	libdemangle_msvc_info_free(not_msvc);
}

const struct task_case task_cases[] = {
	{ "adjustor", test_adjustor_virtual },
	{ "virtual", test_virtual_access_and_distance },
	{ "signature", test_signatures },
	{ "numbers", test_numbers },
	{ "adjustor_errors", test_adjust_errors },
	{ "ownership", test_ownership_and_isolation },
};

const unsigned task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
