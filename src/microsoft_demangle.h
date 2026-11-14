// SPDX-FileCopyrightText: 2015 inisider <inisider@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-only

#ifndef MICROSOFT_DEMANGLE_H
#define MICROSOFT_DEMANGLE_H

#include "demangler_util.h"
#include "demangler_types.h"
#include <rz_msvc_info.h>

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
/// \brief Same as microsoft_demangle(), but also records the structured
///			thunk / `this` adjustment information into `info`.
///
/// The demangling itself is performed by the very same parser; `info` is
/// filled while the production function-category parsing is running.  All
/// `start`/`end` offsets stored in `info` are relative to
/// `demangler->symbol`.  On a parsing error, `info->error` and
/// `info->error_offset` describe the failure and `*demangled_name` is left
/// untouched (NULL).
///
/// `info` may be NULL, in which case the function behaves exactly like
/// microsoft_demangle().
///////////////////////////////////////////////////////////////////////////////
EDemanglerErr microsoft_demangle_info(SDemangler *demangler, char **demangled_name, RzMsvcInfo *info);

#endif // MICROSOFT_DEMANGLE_H
