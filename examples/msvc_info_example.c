// SPDX-FileCopyrightText: 2026 RizinOrg <info@rizin.re>
// SPDX-License-Identifier: LGPL-3.0-only
//
// Minimal C usage example for the structured MSVC demangler.
//
// Build (from the repository root):
//
//   cc -std=gnu99 -Iinclude -Isrc \
//      examples/msvc_info_example.c \
//      src/msvc.c src/msvc_info.c src/microsoft_demangle.c \
//      src/demangler.c src/demangler_util.c -o msvc_info_example
//
// The example only uses the C standard library plus the public header.

#include <rz_msvc_info.h>

#include <stdio.h>
#include <stdlib.h>

static const char *thunk_name(RzMsvcThunkKind kind) {
	switch (kind) {
	case RZ_MSVC_THUNK_STATIC_ADJUSTOR: return "static adjustor";
	case RZ_MSVC_THUNK_VTORDISP: return "vtordisp";
	case RZ_MSVC_THUNK_VTORDISPEX: return "vtordispex";
	case RZ_MSVC_THUNK_NONE: return "none";
	default: return "?";
	}
}

static const char *access_name(RzMsvcAccess access) {
	switch (access) {
	case RZ_MSVC_ACCESS_PRIVATE: return "private";
	case RZ_MSVC_ACCESS_PROTECTED: return "protected";
	case RZ_MSVC_ACCESS_PUBLIC: return "public";
	case RZ_MSVC_ACCESS_NONE: return "none";
	default: return "?";
	}
}

static void describe(const char *symbol) {
	RzMsvcInfo *info = libdemangle_msvc_info(symbol, RZ_DEMANGLE_OPT_BASE);
	if (!info) {
		fprintf(stderr, "out of memory\n");
		return;
	}

	printf("symbol : %s\n", symbol);
	if (info->err != RZ_MSVC_INFO_OK) {
		printf("  error : category=%d offset=%zu\n", (int)info->err, info->err_offset);
		libdemangle_msvc_info_free(info);
		return;
	}

	printf("  display: %s\n", info->display);
	printf("  thunk  : %s (%s, %s)\n",
		info->is_thunk ? "yes" : "no",
		thunk_name(info->thunk_kind),
		info->is_far ? "far" : "near");
	printf("  access : %s\n", access_name(info->access));
	printf("  this   : '%s'\n", info->this_modifier);

	// Print only the adjustment fields the symbol actually carries.
	static const char *field_name[] = { "vbptr", "vbtable", "vtordisp", "static offset" };
	for (int i = 0; i < RZ_MSVC_ADJ_FIELD_COUNT; i++) {
		const RzMsvcAdjustValue *v = &info->adjust[i];
		if (!v->present) {
			continue;
		}
		printf("  adjust : %-13s = %d   [%zu,%zu)\n",
			field_name[i], v->value, v->begin, v->end);
	}
	putchar('\n');

	// The result owns all of its strings, so it is safe to free the input
	// before releasing the structure.
	libdemangle_msvc_info_free(info);
}

int main(void) {
	describe("?public_func@TEST_CLASS@@QEAAHXZ"); // plain function
	describe("?f@C@@WBA@EAAHXZ"); // static adjustor thunk
	describe("??_EDerived@@$4PPPPPPPM@A@EAAPEAXI@Z"); // vtordisp thunk
	describe("?f@A@simple@@$R477PPPPPPPM@7AEXXZ"); // vtordispex thunk
	describe("?f@C@@WPPPPPPPPP@EAAHXZ"); // rejected: number too wide
	return 0;
}
