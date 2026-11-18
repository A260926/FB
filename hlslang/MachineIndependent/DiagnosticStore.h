// Copyright (c) The HLSL2GLSLFork Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.txt file.

#ifndef HLSL2GLSL_DIAGNOSTIC_STORE_H
#define HLSL2GLSL_DIAGNOSTIC_STORE_H

// Owns the structured diagnostics produced during one Hlsl2Glsl_Parse call.
// Records are heap allocated and never moved, so the const char* and array
// pointers handed out through Hlsl2Glsl_GetDiagnostic stay valid until the
// store is cleared (the next Parse) or destroyed (the handle).

#include <deque>
#include <string>
#include <vector>

#include "../../include/hlsl2glsl.h"
#include "preprocessor/provenance.h"
#include "../Include/InfoSink.h"

class DiagnosticStore
{
public:
	DiagnosticStore();
	~DiagnosticStore();

	// Drop everything from the previous parse.
	void clear();

	// Append one diagnostic.  'origin' may be NULL for locations that were
	// not produced by a preprocessed token; 'fallback' then supplies the
	// file and line.  'message' is the original error text.
	void report(const std::string& message,
	            const hlsldiag::TokenOrigin* origin,
	            const TSourceLoc& fallback);

	int count() const { return (int) m_records.size(); }
	const Hlsl2Glsl_Diagnostic* get(int index) const;

private:
	struct Record;

	const char* intern(const std::string& text);

	std::vector<Record*> m_records;
	// std::deque never invalidates references on push_back, so c_str()
	// pointers handed out to callers stay valid for the lifetime of the
	// store.
	std::deque<std::string> m_strings;

	DiagnosticStore(const DiagnosticStore&);
	DiagnosticStore& operator=(const DiagnosticStore&);
};

#endif // HLSL2GLSL_DIAGNOSTIC_STORE_H
