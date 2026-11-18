// Copyright (c) The HLSL2GLSLFork Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.txt file.

#ifndef HLSL2GLSL_PROVENANCE_H
#define HLSL2GLSL_PROVENANCE_H

// Internal, C++-only model used by the preprocessor to remember exactly where
// every preprocessed token was spelled.  The public, flattened representation
// lives in include/hlsl2glsl.h; DiagnosticStore converts between the two.
//
// Nothing here may be constructed by re-searching the source text for the
// final token string: every byte that ends up in the preprocessed stream is
// carried in a BufferSpan that remembers the bytes it was copied from, so
// comments, strings, "##" pasting and repeated macro arguments all keep their
// own spelling.

#include <string>
#include <vector>

namespace hlsldiag
{

// A source position.  'file' is the logical file name (#line aware); an empty
// string means "no known file".  Columns are 1 based and counted in raw UTF-8
// bytes of the *physical* line.
struct Pos
{
	Pos() : line(0), column(0) {}
	Pos(const std::string& f, int l, int c) : file(f), line(l), column(c) {}
	std::string file;
	int line;
	int column;
};

struct Range
{
	Range() : valid(false) {}
	bool valid;
	Pos begin;
	Pos end; // exclusive
};

enum Spelling
{
	SpellingDirect = 0,     // written directly in source
	SpellingMacroBody,      // written in a macro replacement list
	SpellingMacroArgument   // written at a macro call site as an argument
};

struct Expansion;

struct Fragment
{
	Fragment() : spelling(SpellingDirect), beginByte(0), endByte(0) {}
	Range range;
	Spelling spelling;
	// Byte range, relative to the surrounding BufferSpan, that this fragment
	// contributed.  Only the preprocessor uses these; the public API exposes
	// 'range' and 'spelling'.
	unsigned beginByte;
	unsigned endByte;
	// Macros whose expansion produced these bytes, outer-most first.
	std::vector<Expansion> chain;
};

struct Expansion
{
	Expansion() {}
	std::string name;
	Range definition;   // spelling of the macro name in its #define
	Range invocation;   // spelling of the name at the call site
};

struct IncludeFrame
{
	IncludeFrame() {}
	std::string file;    // the included file
	Range directive;     // the #include directive in the including file
};

// Provenance of one preprocessed token.
struct TokenOrigin
{
	TokenOrigin() {}
	Range primary;                       // outermost invocation, or the token
	std::vector<Fragment> fragments;     // ordered, left to right
	std::vector<Expansion> expansions;   // outer-most first
	std::vector<IncludeFrame> includes;  // outer-most include first
};

// A contiguous byte range in a constructed (macro replacement / argument)
// buffer together with the source it came from.  'text' must be exactly the
// bytes that were appended; offsets are absolute in the constructed buffer.
struct BufferSpan
{
	BufferSpan() : begin(0), end(0) {}
	unsigned begin;
	unsigned end;
	std::string text;
	Pos beginPos;                        // source position of text[0]
	std::vector<Fragment> fragments;     // ordered pieces of spelling
	std::vector<Expansion> expansions;   // expansion chain, outer-most first
	std::vector<IncludeFrame> includes;  // include chain of beginPos.file
};

// Provenance of the source buffer currently at the top of the include stack.
struct BufferProvenance
{
	BufferProvenance() : constructed(false) {}

	bool constructed;                 // true for macro / argument buffers
	std::vector<BufferSpan> spans;    // for constructed buffers
	std::vector<IncludeFrame> includes; // include chain of a real file
};

// Captured #define metadata: where the macro name was spelled, and the
// spelling of each token in its replacement list.
struct MacroDefineInfo
{
	MacroDefineInfo() {}
	Range nameRange;
	std::string file;                 // file the #define appears in
	std::vector<BufferSpan> bodySpans;   // offsets relative to def->definition
	std::vector<IncludeFrame> includes;  // include chain of the #define
};

// Captured argument metadata: spelling of the macro-expanded argument text
// and of the unexpanded (original) argument text.  Offsets are relative to
// the corresponding flattened string.
struct MacroArgInfo
{
	MacroArgInfo() {}
	std::vector<BufferSpan> definitionSpans;
	std::vector<BufferSpan> originalSpans;
};

// Map byte 'offset' inside a span to a source position, honouring embedded
// newlines (line splicing is already gone, but arguments may legitimately
// span physical lines).
inline Pos span_position(const BufferSpan& span, unsigned offset)
{
	int line = span.beginPos.line;
	int column = span.beginPos.column;
	const unsigned limit = (offset < span.text.size()) ? offset : (unsigned) span.text.size();
	for (unsigned i = 0; i < limit; ++i)
	{
		if (span.text[i] == '\n')
		{
			++line;
			column = 1;
		}
		else
		{
			++column;
		}
	}
	return Pos(span.beginPos.file, line, column);
}

inline Pos span_position_after(const BufferSpan& span, unsigned offset, unsigned length)
{
	return span_position(span, offset + length);
}

// Find the span covering 'offset'.  Returns NULL for gaps (inserted spaces).
inline const BufferSpan* find_span(const std::vector<BufferSpan>& spans, unsigned offset,
                                   unsigned* localOffset)
{
	for (size_t i = 0; i < spans.size(); ++i)
	{
		const BufferSpan& span = spans[i];
		if (offset >= span.begin && offset < span.end)
		{
			if (localOffset != NULL)
				*localOffset = offset - span.begin;
			return &span;
		}
	}
	return NULL;
}

} // namespace hlsldiag

#endif // HLSL2GLSL_PROVENANCE_H
