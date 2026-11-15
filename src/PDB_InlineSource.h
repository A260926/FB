// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#pragma once

#include "Foundation/PDB_Macros.h"

#include <memory>
#include <string>
#include <vector>


// Offline reader for CodeView inline source information.
//
// The index is built from a raw PDB file, the index of the "/names" stream, and a set of
// module slices.  A slice describes one module's contribution to its module stream:
//
//		struct ModiStream {
//			uint32_t Signature;
//			uint8_t Symbols[SymbolSize - 4];
//			uint8_t C11LineInfo[C11Size];
//			uint8_t C13LineInfo[C13Size];
//			uint32_t GlobalRefsSize;
//			uint8_t GlobalRefs[GlobalRefsSize];
//		};
//
// `symbolBytes` includes the 4-byte module signature, `c11Bytes` is the amount of C11 line
// information following the symbols, and `c13Bytes` is the amount of C13 line information
// following C11.  Everything the index needs is copied while building, so neither the RawFile
// nor the raw file bytes have to stay alive afterwards.
namespace PDB
{
	class RawFile;


	// One module's slice of its module stream.  `streamIndex` is the module symbol stream.
	struct InlineSourceModule
	{
		InlineSourceModule(void) PDB_NO_EXCEPT
			: moduleIndex(0u)
			, streamIndex(0u)
			, symbolBytes(0u)
			, c11Bytes(0u)
			, c13Bytes(0u)
		{
		}

		InlineSourceModule(uint16_t moduleIndex_, uint16_t streamIndex_, uint32_t symbolBytes_, uint32_t c11Bytes_, uint32_t c13Bytes_) PDB_NO_EXCEPT
			: moduleIndex(moduleIndex_)
			, streamIndex(streamIndex_)
			, symbolBytes(symbolBytes_)
			, c11Bytes(c11Bytes_)
			, c13Bytes(c13Bytes_)
		{
		}

		uint16_t moduleIndex;
		uint16_t streamIndex;
		uint32_t symbolBytes;		// includes the 4-byte signature
		uint32_t c11Bytes;
		uint32_t c13Bytes;
	};


	enum class PDB_NO_DISCARD InlineSourceErrorCode : uint32_t
	{
		None = 0,

		InvalidModuleSlice,
		InvalidStream,
		InvalidNamesStream,
		InvalidSymbolRecord,
		InvalidScopeLink,
		InvalidC13Section,
		UnsupportedInlineeLinesSignature,
		UnsupportedSeparateCodeBlock,
		UnsupportedInlineSite2,
		InvalidAnnotation,
		TruncatedAnnotation,
		InvalidInlineeReference,
		DuplicateInlineeDefinition,
		InvalidFileReference,
		InvalidCodeRange,
		InvalidLineOrColumn
	};


	// Diagnostic for the first error encountered while building an index.
	// `offset` is a logical byte offset into the module's module stream (the bytes described by
	// the slice, starting at its stream offset 0).  C13 offsets therefore include symbolBytes and
	// c11Bytes.  For failures that cannot be attributed to a module, moduleIndex is 0xFFFFFFFF.
	struct InlineSourceError
	{
		InlineSourceError(void) PDB_NO_EXCEPT;

		InlineSourceErrorCode code;
		uint32_t moduleIndex;
		uint32_t streamIndex;
		uint32_t offset;
		std::string description;

		PDB_NO_DISCARD inline bool IsError(void) const PDB_NO_EXCEPT
		{
			return code != InlineSourceErrorCode::None;
		}
	};


	// One source range of an inline site.
	//
	// `codeOffsetBegin`/`codeOffsetEnd` are section-relative addresses in the same coordinate
	// system as Query, and describe the effective range: the range declared by the annotation
	// record intersected with the covering ranges of every inline ancestor.  Ranges are half-open.
	//
	// `annotationBegin`/`annotationEnd` are the original annotation bytes of the whole site,
	// whereas `recordOffset` identifies the annotation record that produced this range.  All of
	// them are logical module stream offsets.
	struct InlineSourceFrame
	{
		InlineSourceFrame(void) PDB_NO_EXCEPT;

		uint16_t moduleIndex;
		uint16_t streamIndex;
		uint16_t section;
		uint32_t symbolOffset;			// S_INLINESITE record offset in the module symbol stream
		uint32_t inlinee;				// inlinee id, local to the module
		uint32_t codeOffsetBegin;
		uint32_t codeOffsetEnd;
		uint32_t annotationBegin;		// site annotation blob
		uint32_t annotationEnd;
		uint32_t recordOffset;			// annotation record that produced this range
		uint32_t fileChecksumOffset;	// file identity, local to the module
		uint32_t lineNumber;			// first line of the range
		uint32_t lineSpan;				// number of source lines covered, at least 1
		uint16_t columnStart;
		uint16_t columnEnd;
		bool hasColumnStart;
		bool hasColumnEnd;
		bool isStatement;
		std::string file;				// exactly as stored in the "/names" stream

		// The last line covered by this range, inclusive.  Equals lineNumber for a single line.
		PDB_NO_DISCARD inline uint32_t GetLastLine(void) const PDB_NO_EXCEPT
		{
			return lineNumber + lineSpan - 1u;
		}
	};


	// Identity of the physical procedure an inline chain belongs to.
	struct InlineSourceProcedure
	{
		InlineSourceProcedure(void) PDB_NO_EXCEPT;

		uint16_t moduleIndex;
		uint16_t streamIndex;
		uint16_t section;
		uint32_t symbolOffset;			// S_GPROC32/S_LPROC32 record offset
		uint32_t codeOffset;			// section-relative start
		uint32_t codeSize;
	};


	// A maximal physical-procedure-to-innermost-inline chain covering a queried address.
	struct InlineSourceChain
	{
		InlineSourceChain(void) PDB_NO_EXCEPT;

		InlineSourceProcedure procedure;
		std::vector<InlineSourceFrame> frames;	// outer to inner
	};


	class PDB_NO_DISCARD InlineSourceIndex
	{
	public:
		InlineSourceIndex(void) PDB_NO_EXCEPT;
		InlineSourceIndex(InlineSourceIndex&& other) PDB_NO_EXCEPT;
		InlineSourceIndex& operator=(InlineSourceIndex&& other) PDB_NO_EXCEPT;
		~InlineSourceIndex(void) PDB_NO_EXCEPT;

		// Builds an owned index.  Returns true on success, false on error.  On failure, GetError()
		// describes the first problem.  A successful call with zero modules produces a valid,
		// empty index.
		PDB_NO_DISCARD bool Build(const RawFile& file, uint32_t namesStreamIndex, const InlineSourceModule* modules, size_t moduleCount) PDB_NO_EXCEPT;

		PDB_NO_DISCARD bool Build(const RawFile& file, uint32_t namesStreamIndex, const std::vector<InlineSourceModule>& modules) PDB_NO_EXCEPT;

		// Returns all maximal inline chains covering the given section-relative address.
		// The result is sorted by module index, physical procedure record offset, and the
		// lexicographic sequence of site record offsets.  Returns an empty vector when nothing
		// covers the address.
		PDB_NO_DISCARD std::vector<InlineSourceChain> Query(uint16_t section, uint32_t codeOffset) const PDB_NO_EXCEPT;

		PDB_NO_DISCARD const InlineSourceError& GetError(void) const PDB_NO_EXCEPT;

		PDB_NO_DISCARD inline bool IsValid(void) const PDB_NO_EXCEPT
		{
			return m_valid;
		}

	private:
		struct Impl;

		std::unique_ptr<Impl> m_impl;
		InlineSourceError m_error;
		bool m_valid;

		PDB_DISABLE_COPY(InlineSourceIndex);
	};
}
