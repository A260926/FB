// SPDX-FileCopyrightText: 2026 RizinOrg <info@rizin.re>
// SPDX-License-Identifier: LGPL-3.0-only
//
// Structured MSVC thunk information tests.
//
// These cover the four groups registered with task_cases[] (adjustor, virtual,
// signature, numbers). Every group exercises TASK_CHECK at least once.

#include "task_support/check.h"

#include <rz_msvc_info.h>

#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Small helpers
///////////////////////////////////////////////////////////////////////////////

/* Assert that 'info' parsed successfully and return its display text. */
static const char *expect_ok(RzMsvcInfo *info) {
	TASK_CHECK(info != NULL);
	TASK_CHECK(info->err == RZ_MSVC_INFO_OK);
	TASK_CHECK(info->display != NULL);
	TASK_CHECK(info->this_modifier != NULL);
	return info->display;
}

static int32_t adj(const RzMsvcInfo *info, RzMsvcAdjustField f) {
	TASK_CHECK(info->adjust[f].present);
	return info->adjust[f].value;
}

static void expect_absent(const RzMsvcInfo *info, RzMsvcAdjustField f) {
	TASK_CHECK(!info->adjust[f].present);
	TASK_CHECK(info->adjust[f].value == 0);
}

///////////////////////////////////////////////////////////////////////////////
// group: adjustor - the classic static adjustor thunks (W/X and friends)
///////////////////////////////////////////////////////////////////////////////
static void test_adjustor_static(void) {
	RzMsvcInfo *info = libdemangle_msvc_info("?f@C@@WBA@EAAHXZ", RZ_DEMANGLE_OPT_BASE);
	const char *disp = expect_ok(info);
	TASK_CHECK(info->is_thunk);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_STATIC_ADJUSTOR);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PUBLIC);
	TASK_CHECK(!info->is_far);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_STATIC_OFFSET) == 16);
	// the adjustor display style is preserved
	TASK_CHECK(strstr(disp, "`adjustor{16}'") != NULL);
	TASK_CHECK(strstr(disp, "[thunk]:public virtual:") != NULL);
	// vtordisp-only slots must be reported as absent, not as zero
	expect_absent(info, RZ_MSVC_ADJ_VBPTR);
	expect_absent(info, RZ_MSVC_ADJ_VBTABLE);
	expect_absent(info, RZ_MSVC_ADJ_VTORDISP);
	libdemangle_msvc_info_free(info);

	// negative static adjustor: "?7" is an explicit minus
	info = libdemangle_msvc_info("?f@C@@W?7EAAHXZ", RZ_DEMANGLE_OPT_BASE);
	expect_ok(info);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_STATIC_OFFSET) == -8);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_STATIC_ADJUSTOR);
	libdemangle_msvc_info_free(info);

	// near/far is encoded by the odd selector letter (W = near, X = far)
	info = libdemangle_msvc_info("?f@C@@X7EAAHXZ", RZ_DEMANGLE_OPT_BASE);
	expect_ok(info);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_STATIC_ADJUSTOR);
	TASK_CHECK(info->is_far);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_STATIC_OFFSET) == 8);
	libdemangle_msvc_info_free(info);
}

static void test_adjustor_access(void) {
	struct {
		const char *sym;
		RzMsvcAccess access;
		int far;
	} cases[] = {
		{ "?f@C@@G7EAAHXZ", RZ_MSVC_ACCESS_PRIVATE, 0 },
		{ "?f@C@@H7EAAHXZ", RZ_MSVC_ACCESS_PRIVATE, 1 },
		{ "?f@C@@O7EAAHXZ", RZ_MSVC_ACCESS_PROTECTED, 0 },
		{ "?f@C@@P7EAAHXZ", RZ_MSVC_ACCESS_PROTECTED, 1 },
		{ "?f@C@@W7EAAHXZ", RZ_MSVC_ACCESS_PUBLIC, 0 },
		{ "?f@C@@X7EAAHXZ", RZ_MSVC_ACCESS_PUBLIC, 1 },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		RzMsvcInfo *info = libdemangle_msvc_info(cases[i].sym, RZ_DEMANGLE_OPT_BASE);
		expect_ok(info);
		TASK_CHECK(info->is_thunk);
		TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_STATIC_ADJUSTOR);
		TASK_CHECK(info->access == cases[i].access);
		TASK_CHECK(info->is_far == cases[i].far);
		TASK_CHECK(adj(info, RZ_MSVC_ADJ_STATIC_OFFSET) == 8);
		libdemangle_msvc_info_free(info);
	}
}

///////////////////////////////////////////////////////////////////////////////
// group: virtual - the vtordisp ($n) and vtordispex ($Rn) thunks
///////////////////////////////////////////////////////////////////////////////
static void test_virtual_vtordisp(void) {
	// The mandatory reference symbol: vtordisp -4, static offset 0
	const char *sym = "??_EDerived@@$4PPPPPPPM@A@EAAPEAXI@Z";
	RzMsvcInfo *info = libdemangle_msvc_info(sym, RZ_DEMANGLE_OPT_BASE);
	const char *disp = expect_ok(info);
	TASK_CHECK(info->is_thunk);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_VTORDISP);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PUBLIC);
	TASK_CHECK(!info->is_far);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_VTORDISP) == -4);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_STATIC_OFFSET) == 0);
	// vtordispex-only fields are absent
	expect_absent(info, RZ_MSVC_ADJ_VBPTR);
	expect_absent(info, RZ_MSVC_ADJ_VBTABLE);
	// half-open byte ranges into the original symbol
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_VTORDISP].begin == 15);
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_VTORDISP].end == 24);
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_STATIC_OFFSET].begin == 24);
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_STATIC_OFFSET].end == 26);
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_VTORDISP].end - info->adjust[RZ_MSVC_ADJ_VTORDISP].begin == 9);
	TASK_CHECK(strncmp(sym + info->adjust[RZ_MSVC_ADJ_VTORDISP].begin, "PPPPPPPM@", 9) == 0);
	TASK_CHECK(strncmp(sym + info->adjust[RZ_MSVC_ADJ_STATIC_OFFSET].begin, "A@", 2) == 0);
	// display keeps the old style and appends the vtordisp numbers
	TASK_CHECK(strstr(disp, "`vtordisp{-4,0}'") != NULL);
	TASK_CHECK(strstr(disp, "[thunk]:public virtual:") != NULL);
	libdemangle_msvc_info_free(info);

	// private vtordisp ($0) with a non-zero static offset
	info = libdemangle_msvc_info("?f@A@simple@@$0A@7EAAXXZ", RZ_DEMANGLE_OPT_BASE);
	expect_ok(info);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_VTORDISP);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PRIVATE);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_VTORDISP) == 0);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_STATIC_OFFSET) == 8);
	libdemangle_msvc_info_free(info);

	// far vtordisp selector ($5, odd) and public access
	info = libdemangle_msvc_info("?f@A@simple@@$5A@7EAAXXZ", RZ_DEMANGLE_OPT_BASE);
	expect_ok(info);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_VTORDISP);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PUBLIC);
	TASK_CHECK(info->is_far);
	libdemangle_msvc_info_free(info);
}

static void test_virtual_vtordispex(void) {
	// The mandatory reference symbol: vbptr 8, vbtable 8, vtordisp -4, static 8
	const char *sym = "?f@A@simple@@$R477PPPPPPPM@7AEXXZ";
	RzMsvcInfo *info = libdemangle_msvc_info(sym, RZ_DEMANGLE_OPT_BASE);
	const char *disp = expect_ok(info);
	TASK_CHECK(info->is_thunk);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_VTORDISPEX);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PUBLIC);
	TASK_CHECK(!info->is_far);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_VBPTR) == 8);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_VBTABLE) == 8);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_VTORDISP) == -4);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_STATIC_OFFSET) == 8);
	// half-open ranges in mangled order
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_VBPTR].begin == 16);
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_VBPTR].end == 17);
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_VBTABLE].begin == 17);
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_VBTABLE].end == 18);
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_VTORDISP].begin == 18);
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_VTORDISP].end == 27);
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_STATIC_OFFSET].begin == 27);
	TASK_CHECK(info->adjust[RZ_MSVC_ADJ_STATIC_OFFSET].end == 28);
	TASK_CHECK(strstr(disp, "`vtordispex{8,8,-4,8}'") != NULL);
	libdemangle_msvc_info_free(info);

	// protected vtordispex ($R2) and far ($R5)
	info = libdemangle_msvc_info("?f@A@simple@@$R2A@A@A@A@EAAXXZ", RZ_DEMANGLE_OPT_BASE);
	expect_ok(info);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_VTORDISPEX);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PROTECTED);
	TASK_CHECK(!info->is_far);
	libdemangle_msvc_info_free(info);

	info = libdemangle_msvc_info("?f@A@simple@@$R5A@A@A@A@EAAXXZ", RZ_DEMANGLE_OPT_BASE);
	expect_ok(info);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_VTORDISPEX);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PUBLIC);
	TASK_CHECK(info->is_far);
	libdemangle_msvc_info_free(info);
}

///////////////////////////////////////////////////////////////////////////////
// group: signature - ordinary symbols, this modifiers, templates, repeats
///////////////////////////////////////////////////////////////////////////////
static void test_signature_plain(void) {
	RzMsvcInfo *info = libdemangle_msvc_info("?public_func@TEST_CLASS@@QEAAHXZ", RZ_DEMANGLE_OPT_BASE);
	const char *disp = expect_ok(info);
	TASK_CHECK(!info->is_thunk);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_NONE);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PUBLIC);
	TASK_CHECK(strcmp(info->this_modifier, "") == 0);
	TASK_CHECK(strstr(disp, "TEST_CLASS::public_func(void)") != NULL);
	for (int i = 0; i < RZ_MSVC_ADJ_FIELD_COUNT; i++) {
		expect_absent(info, (RzMsvcAdjustField)i);
	}
	libdemangle_msvc_info_free(info);

	// private, const-qualified this pointer
	info = libdemangle_msvc_info("?f@TEST_CLASS@@AEBAXXZ", RZ_DEMANGLE_OPT_BASE);
	expect_ok(info);
	TASK_CHECK(!info->is_thunk);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PRIVATE);
	TASK_CHECK(strcmp(info->this_modifier, "const") == 0);
	libdemangle_msvc_info_free(info);

	// protected, volatile-const this pointer
	info = libdemangle_msvc_info("?f@TEST_CLASS@@IEDAAXXZ", RZ_DEMANGLE_OPT_BASE);
	expect_ok(info);
	TASK_CHECK(info->access == RZ_MSVC_ACCESS_PROTECTED);
	TASK_CHECK(strcmp(info->this_modifier, "const volatile") == 0);
	libdemangle_msvc_info_free(info);
}

static void test_signature_template_and_repeats(void) {
	// a member thunk on a template class whose argument repeats the class type
	const char *sym = "?f@?$Tmpl@H@ns@@$4PPPPPPPM@A@EAAXPEAPEAV?$Tmpl@H@ns@@@Z";
	RzMsvcInfo *info = libdemangle_msvc_info(sym, RZ_DEMANGLE_OPT_BASE);
	const char *disp = expect_ok(info);
	TASK_CHECK(info->is_thunk);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_VTORDISP);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_VTORDISP) == -4);
	// the template scope is part of the demangled name, and the new fields
	// did not consume any of the template text
	TASK_CHECK(strstr(disp, "ns::Tmpl<int>::f`vtordisp{-4,0}'") != NULL);
	TASK_CHECK(strstr(disp, "class ns::Tmpl<int> * __ptr64 * __ptr64") != NULL);
	libdemangle_msvc_info_free(info);

	// repeated arguments (abbreviations) inside a thunk signature
	info = libdemangle_msvc_info("?f@A@simple@@$R477PPPPPPPM@7EAAXPEAVsimple@A@0@Z", RZ_DEMANGLE_OPT_BASE);
	disp = expect_ok(info);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_VTORDISPEX);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_VBPTR) == 8);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_VBTABLE) == 8);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_VTORDISP) == -4);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_STATIC_OFFSET) == 8);
	TASK_CHECK(strstr(disp, "`vtordispex{8,8,-4,8}'") != NULL);
	libdemangle_msvc_info_free(info);

	// a thunk that also takes a function pointer: the nested function type
	// must not clobber the outer thunk information
	info = libdemangle_msvc_info("?f@C@@$4PPPPPPPM@A@EAAXP6AXXZ@Z", RZ_DEMANGLE_OPT_BASE);
	disp = expect_ok(info);
	TASK_CHECK(info->is_thunk);
	TASK_CHECK(info->thunk_kind == RZ_MSVC_THUNK_VTORDISP);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_VTORDISP) == -4);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_STATIC_OFFSET) == 0);
	TASK_CHECK(strstr(disp, "__cdecl *") != NULL);
	libdemangle_msvc_info_free(info);
}

static void test_signature_legacy_agrees(void) {
	// the legacy handler must expose the newly supported symbols too
	char *legacy = libdemangle_handler_msvc("??_EDerived@@$4PPPPPPPM@A@EAAPEAXI@Z", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(legacy != NULL);
	TASK_CHECK(strstr(legacy, "`vtordisp{-4,0}'") != NULL);
	free(legacy);

	legacy = libdemangle_handler_msvc("?f@A@simple@@$R477PPPPPPPM@7AEXXZ", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(legacy != NULL);
	TASK_CHECK(strstr(legacy, "`vtordispex{8,8,-4,8}'") != NULL);
	free(legacy);

	// and a known-good old symbol keeps its exact display
	legacy = libdemangle_handler_msvc("?f@C@@WBA@EAAHXZ", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(legacy != NULL);
	TASK_CHECK(strcmp(legacy, "[thunk]:public virtual: int __cdecl C::f`adjustor{16}'(void) __ptr64") == 0);
	free(legacy);
}

///////////////////////////////////////////////////////////////////////////////
// group: numbers - signed decoding, offsets, and malformed adjustments
///////////////////////////////////////////////////////////////////////////////
static void test_numbers_signed(void) {
	static const struct {
		const char *sym;
		int32_t value;
	} cases[] = {
		// "0".."9" -> 1..10
		{ "?f@C@@W0EAAHXZ", 1 },
		{ "?f@C@@W9EAAHXZ", 10 },
		// explicit '?' negates
		{ "?f@C@@W?0EAAHXZ", -1 },
		{ "?f@C@@W?9EAAHXZ", -10 },
		// 'A'..'P' hex nibbles, '@' terminated; A@ is zero
		{ "?f@C@@WA@EAAHXZ", 0 },
		{ "?f@C@@WB@EAAHXZ", 1 },
		{ "?f@C@@WBA@EAAHXZ", 16 },
		// no sign -> two's complement of the 32 bit pattern, not 4294967292
		{ "?f@C@@WPPPPPPPM@EAAHXZ", -4 },
		{ "?f@C@@WPPPPPPPL@EAAHXZ", -5 },
		{ "?f@C@@WPPPPPPPP@EAAHXZ", -1 },
		// signed boundaries
		{ "?f@C@@WHPPPPPPP@EAAHXZ", 2147483647 },
		{ "?f@C@@WIAAAAAAA@EAAHXZ", (-2147483647 - 1) },
		{ "?f@C@@W?IAAAAAAA@EAAHXZ", (-2147483647 - 1) },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		RzMsvcInfo *info = libdemangle_msvc_info(cases[i].sym, RZ_DEMANGLE_OPT_BASE);
		expect_ok(info);
		TASK_CHECK(adj(info, RZ_MSVC_ADJ_STATIC_OFFSET) == cases[i].value);
		libdemangle_msvc_info_free(info);
	}
}

static void test_numbers_errors(void) {
	// explicit '?' on a vtordisp value keeps its sign too
	RzMsvcInfo *info = libdemangle_msvc_info("?f@A@simple@@$4?7A@EAAXXZ", RZ_DEMANGLE_OPT_BASE);
	expect_ok(info);
	TASK_CHECK(adj(info, RZ_MSVC_ADJ_VTORDISP) == -8);
	libdemangle_msvc_info_free(info);

	// an over-wide hex number is rejected, not truncated
	info = libdemangle_msvc_info("?f@C@@WPPPPPPPPP@EAAHXZ", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(info && info->err == RZ_MSVC_INFO_ERR_NUMBER_WIDTH);
	TASK_CHECK(info->display == NULL);
	TASK_CHECK(info->err_offset == 7);
	libdemangle_msvc_info_free(info);

	// a negative value below INT32_MIN is rejected
	info = libdemangle_msvc_info("?f@C@@W?JAAAAAAA@EAAHXZ", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(info && info->err == RZ_MSVC_INFO_ERR_NUMBER_WIDTH);
	libdemangle_msvc_info_free(info);

	// a hex number without its terminating '@'
	info = libdemangle_msvc_info("?f@C@@WPPPPPPPM3EAAHXZ", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(info && info->err == RZ_MSVC_INFO_ERR_NUMBER_TERMINATOR);
	TASK_CHECK(info->err_offset == 7);
	libdemangle_msvc_info_free(info);

	// a truncated adjustment vector reports the missing field, with an offset
	info = libdemangle_msvc_info("?f@A@simple@@$4PPPPPPPM@", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(info && info->err == RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUSTMENT);
	TASK_CHECK(info->err_offset == 24);
	libdemangle_msvc_info_free(info);

	info = libdemangle_msvc_info("?f@A@simple@@$R47PPPPPPPM@", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(info && info->err == RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUSTMENT);
	TASK_CHECK(info->err_offset == 26);
	libdemangle_msvc_info_free(info);

	// unsupported vtordisp selector digit
	info = libdemangle_msvc_info("?f@A@simple@@$7PPPPPPPM@A@EAAHXZ", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(info && info->err != RZ_MSVC_INFO_OK);
	TASK_CHECK(info->display == NULL);
	libdemangle_msvc_info_free(info);

	// the strict entry point rejects trailing garbage: the valid symbol is
	// 16 bytes long, so the first byte past it is the reported offset
	info = libdemangle_msvc_info("?f@C@@WBA@EAAHXZ trailing", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(info && info->err == RZ_MSVC_INFO_ERR_TRAILING_GARBAGE);
	TASK_CHECK(info->err_offset == 16);
	libdemangle_msvc_info_free(info);

	// a non-MSVC symbol
	info = libdemangle_msvc_info("_Z3foov", RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(info && info->err == RZ_MSVC_INFO_ERR_NOT_MSVC);
	libdemangle_msvc_info_free(info);

	// NULL input
	info = libdemangle_msvc_info(NULL, RZ_DEMANGLE_OPT_BASE);
	TASK_CHECK(info && info->err == RZ_MSVC_INFO_ERR_INVALID_ARGUMENT);
	libdemangle_msvc_info_free(info);
}

static void test_numbers_recovery_after_errors(void) {
	// a run of failures must not poison the next successful call
	static const char *bad[] = {
		"?f@A@simple@@$4PPPPPPPM@",
		"?f@C@@WPPPPPPPPP@EAAHXZ",
		"?f@C@@WPPPPPPPM3EAAHXZ",
		"?f@A@simple@@$7PPPPPPPM@A@EAAHXZ",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		RzMsvcInfo *info = libdemangle_msvc_info(bad[i], RZ_DEMANGLE_OPT_BASE);
		TASK_CHECK(info && info->err != RZ_MSVC_INFO_OK);
		libdemangle_msvc_info_free(info);

		info = libdemangle_msvc_info("?f@C@@WBA@EAAHXZ", RZ_DEMANGLE_OPT_BASE);
		expect_ok(info);
		TASK_CHECK(adj(info, RZ_MSVC_ADJ_STATIC_OFFSET) == 16);
		libdemangle_msvc_info_free(info);
	}
}

///////////////////////////////////////////////////////////////////////////////
// Case registration
//
// main.c drives four named groups. Each group's 'run' entry chains the more
// focused sub-tests above so every assertion runs.
///////////////////////////////////////////////////////////////////////////////
static void run_adjustor_group(void) {
	test_adjustor_static();
	test_adjustor_access();
}

static void run_virtual_group(void) {
	test_virtual_vtordisp();
	test_virtual_vtordispex();
}

static void run_signature_group(void) {
	test_signature_plain();
	test_signature_template_and_repeats();
	test_signature_legacy_agrees();
}

static void run_numbers_group(void) {
	test_numbers_signed();
	test_numbers_errors();
	test_numbers_recovery_after_errors();
}

const struct task_case task_cases[] = {
	{ "adjustor", run_adjustor_group },
	{ "virtual", run_virtual_group },
	{ "signature", run_signature_group },
	{ "numbers", run_numbers_group },
};

const unsigned task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
