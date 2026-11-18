// Copyright (c) The HLSL2GLSLFork Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.txt file.

#include "DiagnosticStore.h"

#include <string.h>

struct DiagnosticStore::Record
{
	Record() { memset(&pub, 0, sizeof(pub)); }

	Hlsl2Glsl_Diagnostic pub;
	std::vector<Hlsl2Glsl_SourceFragment> fragments;
	std::vector<Hlsl2Glsl_MacroExpansion> expansions;
	std::vector<Hlsl2Glsl_IncludeFrame> includes;
};


DiagnosticStore::DiagnosticStore()
{
}

DiagnosticStore::~DiagnosticStore()
{
	clear();
}

void DiagnosticStore::clear()
{
	for (size_t i = 0; i < m_records.size(); ++i)
		delete m_records[i];
	m_records.clear();
	m_strings.clear();
}

const char* DiagnosticStore::intern(const std::string& text)
{
	if (text.empty())
		return NULL;
	m_strings.push_back(text);
	return m_strings.back().c_str();
}

const Hlsl2Glsl_Diagnostic* DiagnosticStore::get(int index) const
{
	if (index < 0 || index >= (int) m_records.size())
		return NULL;
	return &m_records[(size_t) index]->pub;
}

static void fill_position(Hlsl2Glsl_SourcePosition* out,
                          const hlsldiag::Pos& pos,
                          const char* file)
{
	out->file = file;
	out->line = pos.line;
	out->column = pos.column;
}

static void fill_range(Hlsl2Glsl_SourceRange* out,
                       const hlsldiag::Range& range,
                       const char* fileBegin,
                       const char* fileEnd)
{
	fill_position(&out->begin, range.begin, fileBegin);
	fill_position(&out->end, range.end, fileEnd);
}

void DiagnosticStore::report(const std::string& message,
                             const hlsldiag::TokenOrigin* origin,
                             const TSourceLoc& fallback)
{
	Record* record = new Record();
	m_records.push_back(record);

	hlsldiag::TokenOrigin synthetic;
	if (origin == NULL)
	{
		// No provenance (an internal error emitted away from the token
		// stream): fall back to the plain file/line location.
		synthetic.primary.valid = true;
		synthetic.primary.begin = hlsldiag::Pos(fallback.file ? fallback.file : "",
		                                        fallback.line, 0);
		synthetic.primary.end = synthetic.primary.begin;
		origin = &synthetic;
	}

	record->pub.message = intern(message);

	// Primary location.  File names are interned separately for begin and
	// end because #line can make them differ across a token (it cannot in
	// practice, but this keeps the representation honest).
	const char* primaryBegin = intern(origin->primary.begin.file);
	const char* primaryEnd = intern(origin->primary.end.file);
	fill_range(&record->pub.primary, origin->primary, primaryBegin, primaryEnd);

	record->fragments.reserve(origin->fragments.size());
	for (size_t i = 0; i < origin->fragments.size(); ++i)
	{
		const hlsldiag::Fragment& f = origin->fragments[i];
		Hlsl2Glsl_SourceFragment out;
		const char* fileBegin = intern(f.range.begin.file);
		const char* fileEnd = intern(f.range.end.file);
		fill_range(&out.range, f.range, fileBegin, fileEnd);
		switch (f.spelling)
		{
			case hlsldiag::SpellingMacroBody:
				out.spelling = Hlsl2Glsl_SourceSpellingMacroBody;
				break;
			case hlsldiag::SpellingMacroArgument:
				out.spelling = Hlsl2Glsl_SourceSpellingMacroArgument;
				break;
			case hlsldiag::SpellingDirect:
			default:
				out.spelling = Hlsl2Glsl_SourceSpellingDirect;
				break;
		}
		record->fragments.push_back(out);
	}
	record->pub.fragments = record->fragments.empty() ? NULL : &record->fragments[0];
	record->pub.fragmentCount = (int) record->fragments.size();

	record->expansions.reserve(origin->expansions.size());
	for (size_t i = 0; i < origin->expansions.size(); ++i)
	{
		const hlsldiag::Expansion& e = origin->expansions[i];
		Hlsl2Glsl_MacroExpansion out;
		out.name = intern(e.name);
		const char* defBegin = intern(e.definition.begin.file);
		const char* defEnd = intern(e.definition.end.file);
		fill_range(&out.definition, e.definition, defBegin, defEnd);
		const char* invBegin = intern(e.invocation.begin.file);
		const char* invEnd = intern(e.invocation.end.file);
		fill_range(&out.invocation, e.invocation, invBegin, invEnd);
		record->expansions.push_back(out);
	}
	record->pub.expansions = record->expansions.empty() ? NULL : &record->expansions[0];
	record->pub.expansionCount = (int) record->expansions.size();

	record->includes.reserve(origin->includes.size());
	for (size_t i = 0; i < origin->includes.size(); ++i)
	{
		const hlsldiag::IncludeFrame& inc = origin->includes[i];
		Hlsl2Glsl_IncludeFrame out;
		out.file = intern(inc.file);
		const char* dirBegin = intern(inc.directive.begin.file);
		const char* dirEnd = intern(inc.directive.end.file);
		fill_range(&out.directive, inc.directive, dirBegin, dirEnd);
		record->includes.push_back(out);
	}
	record->pub.includes = record->includes.empty() ? NULL : &record->includes[0];
	record->pub.includeCount = (int) record->includes.size();
}
