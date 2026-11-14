/*
 * Example: feed MSVC thunk / `this` adjustment information into a call-graph
 * analysis layer.
 *
 * Build (from the repository root):
 *
 *   cc -std=c99 -Iinclude src/msvc.c src/microsoft_demangle.c \
 *       src/demangler.c src/demangler_util.c src/msvc_info.c \
 *       examples/msvc_info_example.c -o msvc_info_example
 *
 *   ./msvc_info_example
 *
 * The example only uses the C standard library.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rz_msvc_info.h>

static const char *adjust_kind_name(RzMsvcAdjustKind kind) {
	switch (kind) {
	case RZ_MSVC_ADJUST_NONE:
		return "none";
	case RZ_MSVC_ADJUST_STATIC:
		return "static";
	case RZ_MSVC_ADJUST_VTORDISP:
		return "vtordisp";
	case RZ_MSVC_ADJUST_VTORDISPEX:
		return "vtordispex";
	}
	return "?";
}

static const char *access_name(RzMsvcAccess access) {
	switch (access) {
	case RZ_MSVC_ACCESS_NONE:
		return "none";
	case RZ_MSVC_ACCESS_PRIVATE:
		return "private";
	case RZ_MSVC_ACCESS_PROTECTED:
		return "protected";
	case RZ_MSVC_ACCESS_PUBLIC:
		return "public";
	}
	return "?";
}

static const char *distance_name(RzMsvcDistance distance) {
	switch (distance) {
	case RZ_MSVC_DISTANCE_NONE:
		return "none";
	case RZ_MSVC_DISTANCE_NEAR:
		return "near";
	case RZ_MSVC_DISTANCE_FAR:
		return "far";
	}
	return "?";
}

static const char *this_name(RzMsvcThisModifier mod) {
	switch (mod) {
	case RZ_MSVC_THIS_NONE:
		return "none";
	case RZ_MSVC_THIS_PLAIN:
		return "plain";
	case RZ_MSVC_THIS_CONST:
		return "const";
	case RZ_MSVC_THIS_VOLATILE:
		return "volatile";
	case RZ_MSVC_THIS_CONST_VOLATILE:
		return "const volatile";
	}
	return "?";
}

/* A field that does not exist for this adjustment kind is reported as such,
 * never as a fake zero. */
static void print_number(const char *label, const RzMsvcNumber *num) {
	if (!num->present) {
		printf("  %-13s <absent>\n", label);
		return;
	}
	printf("  %-13s %" PRId32 "  [bytes %zu..%zu)\n",
		label, num->value, num->start, num->end);
}

static void print_symbol(const char *symbol) {
	RzMsvcInfo *info = libdemangle_msvc_info(symbol, RZ_DEMANGLE_OPT_BASE);
	if (!info) {
		printf("%s\n  allocation failure\n\n", symbol);
		return;
	}

	printf("%s\n", symbol);
	if (info->error != RZ_MSVC_INFO_ERR_NONE) {
		printf("  ERROR category=%d offset=%zu\n\n",
			(int)info->error, info->error_offset);
		libdemangle_msvc_info_free(info);
		return;
	}

	printf("  display       %s\n", info->display);
	printf("  thunk         %s (kind=%s)\n",
		info->is_thunk ? "yes" : "no", adjust_kind_name(info->kind));
	printf("  access        %s\n", access_name(info->access));
	printf("  distance      %s\n", distance_name(info->distance));
	printf("  this          %s\n", this_name(info->this_modifier));
	print_number("static offset", &info->static_offset);
	print_number("vtordisp", &info->vtordisp);
	print_number("vbptr", &info->vbptr);
	print_number("vbtable slot", &info->vbtable_slot);

	/* A call-graph layer would, for example, record that the real callee is
	 * `display` with its `this` pointer shifted by the static offset after
	 * applying the virtual base displacements. */
	if (info->is_thunk) {
		printf("  note          thunk: adjust `this` before recording the call\n");
	}
	printf("\n");
	libdemangle_msvc_info_free(info);
}

int main(void) {
	static const char *symbols[] = {
		"??_EDerived@@$4PPPPPPPM@A@EAAPEAXI@Z", /* vtordisp   (-4, 0)   */
		"?f@A@simple@@$R477PPPPPPPM@7AEXXZ", /* vtordispex (8,8,-4,8)*/
		"?f@C@@WBA@EAAHXZ", /* static adjustor{16}    */
		"?public_func@TEST_CLASS@@QEAAHXZ", /* regular signature      */
		"?f@C@@WPPPPPPPPZEAAHXZ", /* malformed number       */
	};
	for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); ++i) {
		print_symbol(symbols[i]);
	}
	return 0;
}
