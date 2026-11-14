// SPDX-FileCopyrightText: 2024 RizinOrg <info@rizin.re>
// SPDX-License-Identifier: LGPL-3.0-only

#include "demangler.h"
#include "demangler_util.h"
#include "microsoft_demangle.h"
#include <rz_msvc_info.h>

/**
 * \brief Decode an MSVC mangled symbol into a structured result.
 *
 * This is a thin wrapper around the production Microsoft demangler: the
 * display string and the adjustment data are produced by the same parser
 * that libdemangle_handler_msvc() uses, no external tool nor a second
 * regular-expression based pass over the demangled text is involved.
 */
DEM_LIB_EXPORT RzMsvcInfo *libdemangle_msvc_info(const char *symbol, RzDemangleOpts opts) {
	(void)opts; // the structured view is independent from presentation options

	if (!symbol) {
		return NULL;
	}

	RzMsvcInfo *info = RZ_NEW0(RzMsvcInfo);
	if (!info) {
		return NULL;
	}
	info->kind = RZ_MSVC_ADJUST_NONE;
	info->access = RZ_MSVC_ACCESS_NONE;
	info->distance = RZ_MSVC_DISTANCE_NONE;
	info->this_modifier = RZ_MSVC_THIS_NONE;
	info->error = RZ_MSVC_INFO_ERR_NONE;

	// init_demangler() takes its own writable copy of the symbol, so the
	// caller's string is never referenced after this function returns.
	SDemangler *mangler = NULL;
	if (create_demangler(&mangler) != eDemanglerErrOK || !mangler) {
		info->error = RZ_MSVC_INFO_ERR_MEMORY;
		return info;
	}
	if (init_demangler(mangler, (char *)symbol) != eDemanglerErrOK) {
		free_demangler(mangler);
		info->error = RZ_MSVC_INFO_ERR_NOT_MSVC;
		info->error_offset = 0;
		return info;
	}

	char *display = NULL;
	microsoft_demangle_info(mangler, &display, info);

	free_demangler(mangler);

	if (info->error != RZ_MSVC_INFO_ERR_NONE) {
		free(display);
		info->display = NULL;
		return info;
	}
	info->display = display;
	return info;
}

DEM_LIB_EXPORT void libdemangle_msvc_info_free(RzMsvcInfo *info) {
	if (!info) {
		return;
	}
	free(info->display);
	free(info);
}
