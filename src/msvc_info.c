// SPDX-FileCopyrightText: 2026 RizinOrg <info@rizin.re>
// SPDX-License-Identifier: LGPL-3.0-only

#include <stdlib.h>
#include <string.h>

#include "rz_msvc_info.h"

#include "demangler.h"
#include "microsoft_demangle.h"

///////////////////////////////////////////////////////////////////////////////
/// \brief Translate an internal SMsvcAdjustValue into the public struct.
///////////////////////////////////////////////////////////////////////////////
static void msvc_info_export_adjust(RzMsvcAdjustValue *dst, const SMsvcAdjustValue *src) {
	dst->present = src->present;
	dst->value = src->present ? src->value : 0;
	dst->begin = src->present ? src->begin : 0;
	dst->end = src->present ? src->end : 0;
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Map the internal thunk kind onto the public enum.
///////////////////////////////////////////////////////////////////////////////
static RzMsvcThunkKind msvc_info_export_kind(EMsvcThunkKind kind) {
	switch (kind) {
	case MSVC_THUNK_KIND_STATIC_ADJUSTOR:
		return RZ_MSVC_THUNK_STATIC_ADJUSTOR;
	case MSVC_THUNK_KIND_VTORDISP:
		return RZ_MSVC_THUNK_VTORDISP;
	case MSVC_THUNK_KIND_VTORDISPEX:
		return RZ_MSVC_THUNK_VTORDISPEX;
	default:
		return RZ_MSVC_THUNK_NONE;
	}
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Map the internal access level onto the public enum.
///////////////////////////////////////////////////////////////////////////////
static RzMsvcAccess msvc_info_export_access(EMsvcAccess access) {
	switch (access) {
	case MSVC_ACCESS_PRIVATE:
		return RZ_MSVC_ACCESS_PRIVATE;
	case MSVC_ACCESS_PROTECTED:
		return RZ_MSVC_ACCESS_PROTECTED;
	case MSVC_ACCESS_PUBLIC:
		return RZ_MSVC_ACCESS_PUBLIC;
	default:
		return RZ_MSVC_ACCESS_NONE;
	}
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Set the error category and offset on a freshly built result.
///////////////////////////////////////////////////////////////////////////////
static void msvc_info_set_error(RzMsvcInfo *out, RzMsvcInfoErr err, size_t offset) {
	out->err = err;
	out->err_offset = offset;
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Map the detailed internal adjustment error onto the public one.
///////////////////////////////////////////////////////////////////////////////
static RzMsvcInfoErr msvc_info_map_adjust_err(EMsvcAdjustErr err) {
	switch (err) {
	case MSVC_ADJUST_ERR_NUMBER_WIDTH:
		return RZ_MSVC_INFO_ERR_NUMBER_WIDTH;
	case MSVC_ADJUST_ERR_NUMBER_TERMINATOR:
		return RZ_MSVC_INFO_ERR_NUMBER_TERMINATOR;
	case MSVC_ADJUST_ERR_INCOMPLETE:
		return RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUSTMENT;
	default:
		return RZ_MSVC_INFO_ERR_SYNTAX;
	}
}

///////////////////////////////////////////////////////////////////////////////
static RzMsvcInfo *msvc_info_new(void) {
	RzMsvcInfo *out = (RzMsvcInfo *)calloc(1, sizeof(RzMsvcInfo));
	if (!out) {
		return NULL;
	}
	out->err = RZ_MSVC_INFO_OK;
	return out;
}

///////////////////////////////////////////////////////////////////////////////
DEM_LIB_EXPORT RzMsvcInfo *libdemangle_msvc_info(const char *symbol, RzDemangleOpts opts) {
	(void)opts;

	RzMsvcInfo *out = msvc_info_new();
	if (!out) {
		return NULL;
	}
	if (!symbol) {
		msvc_info_set_error(out, RZ_MSVC_INFO_ERR_INVALID_ARGUMENT, 0);
		return out;
	}

	SDemangler *mangler = NULL;
	SMsvcFunctionInfo parsed;
	char *display = NULL;
	size_t chars_read = 0;
	EDemanglerErr err = eDemanglerErrUncorrectMangledSymbol;

	memset(&parsed, 0, sizeof(parsed));
	parsed.this_modifier = "";

	if (create_demangler(&mangler) != eDemanglerErrOK || !mangler) {
		msvc_info_set_error(out, RZ_MSVC_INFO_ERR_MEMORY, 0);
		return out;
	}
	if (init_demangler(mangler, (char *)symbol) != eDemanglerErrOK) {
		free_demangler(mangler);
		msvc_info_set_error(out, RZ_MSVC_INFO_ERR_NOT_MSVC, 0);
		return out;
	}

	err = microsoft_demangle_info(mangler, &display, &parsed, &chars_read);
	free_demangler(mangler);

	if (err != eDemanglerErrOK) {
		free(display);
		if (parsed.adjust_failed) {
			msvc_info_set_error(out, msvc_info_map_adjust_err(parsed.adjust_err), parsed.adjust_err_offset);
		} else if (parsed.parse_failed) {
			RzMsvcInfoErr mapped = parsed.parse_err == eDemanglerErrMemoryAllocation
				? RZ_MSVC_INFO_ERR_MEMORY
				: RZ_MSVC_INFO_ERR_SYNTAX;
			if (mapped == RZ_MSVC_INFO_ERR_SYNTAX && parsed.parse_err_offset >= strlen(symbol)) {
				// the parser ran off the end of the symbol while still
				// expecting input (typically a truncated adjustment vector)
				mapped = RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUSTMENT;
			}
			msvc_info_set_error(out, mapped, parsed.parse_err_offset);
		} else if (err == eDemanglerErrMemoryAllocation) {
			msvc_info_set_error(out, RZ_MSVC_INFO_ERR_MEMORY, 0);
		} else if (err == eDemanglerErrUnsupportedMangling) {
			msvc_info_set_error(out, RZ_MSVC_INFO_ERR_UNSUPPORTED, chars_read);
		} else {
			msvc_info_set_error(out, RZ_MSVC_INFO_ERR_SYNTAX, chars_read);
		}
		return out;
	}

	// Reject trailing garbage: the structured entry point must consume the
	// whole symbol.
	if (symbol[chars_read] != '\0') {
		free(display);
		msvc_info_set_error(out, RZ_MSVC_INFO_ERR_TRAILING_GARBAGE, chars_read);
		return out;
	}
	if (!display) {
		msvc_info_set_error(out, RZ_MSVC_INFO_ERR_MEMORY, 0);
		return out;
	}

	out->display = display;
	out->is_thunk = parsed.is_thunk;
	out->thunk_kind = msvc_info_export_kind(parsed.thunk_kind);
	out->access = msvc_info_export_access(parsed.access);
	out->is_far = parsed.is_far;

	const char *this_mod = parsed.this_modifier ? parsed.this_modifier : "";
	out->this_modifier = strdup(this_mod);
	if (!out->this_modifier) {
		free(out->display);
		out->display = NULL;
		msvc_info_set_error(out, RZ_MSVC_INFO_ERR_MEMORY, 0);
		return out;
	}

	for (size_t i = 0; i < RZ_MSVC_ADJ_FIELD_COUNT; i++) {
		msvc_info_export_adjust(&out->adjust[i], &parsed.adjust[i]);
	}

	return out;
}

///////////////////////////////////////////////////////////////////////////////
DEM_LIB_EXPORT void libdemangle_msvc_info_free(RzMsvcInfo *info) {
	if (!info) {
		return;
	}
	free(info->display);
	free(info->this_modifier);
	free(info);
}
