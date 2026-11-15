// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#include "PDB_PCH.h"
#include "PDB_InlineSource.h"
#include "PDB_RawFile.h"
#include "PDB_CoalescedMSFStream.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>


namespace
{
	const uint32_t InvalidIndex = 0xFFFFFFFFu;
	const uint32_t MaxCoordinate = 0xFFFFFFFFu;
	const uint32_t MaxLineNumber = 0xFFFFFFFFu;
	const uint32_t MaxColumnNumber = 0xFFFFu;

	// CodeView symbol record kinds handled by the inline source reader.
	// https://llvm.org/docs/PDB/CodeViewSymbols.html
	const uint16_t S_END =					0x0006u;
	const uint16_t S_BLOCK32 =				0x1103u;
	const uint16_t S_LPROC32 =				0x110Fu;
	const uint16_t S_GPROC32 =				0x1110u;
	const uint16_t S_SEPCODE =				0x1132u;
	const uint16_t S_LPROC32_ID =			0x1146u;
	const uint16_t S_GPROC32_ID =			0x1147u;
	const uint16_t S_INLINESITE =			0x114Du;
	const uint16_t S_INLINESITE_END =		0x114Eu;
	const uint16_t S_PROC_ID_END =			0x114Fu;
	const uint16_t S_INLINESITE2 =			0x115Du;

	const uint32_t S_FILECHECKSUMS =		0xF4u;
	const uint32_t S_INLINEELINES =			0xF6u;

	const uint32_t NamesMagic =				0xEFFEEFFEu;
	const uint32_t NamesHeaderSize =		12u;

	// Annotation opcodes, numbered as described by the supported format:
	//  1 CodeOffset                     2 ChangeCodeOffset (separated code blocks only)
	//  3 advance code + emit line       4 explicit code length
	//  5 change file                    6 signed line delta
	//  7 line span                      8 statement/expression
	//  9 column start                  10 signed column end delta
	// 11 code offset + line offset     12 explicit code length + code offset
	// 13 column end
	const uint32_t OpCodeOffset = 1u;
	const uint32_t OpChangeCodeOffset = 2u;
	const uint32_t OpAdvanceCodeOffset = 3u;
	const uint32_t OpChangeCodeLength = 4u;
	const uint32_t OpChangeFile = 5u;
	const uint32_t OpChangeLineOffset = 6u;
	const uint32_t OpChangeLineEndDelta = 7u;
	const uint32_t OpChangeRangeKind = 8u;
	const uint32_t OpChangeColumnStart = 9u;
	const uint32_t OpChangeColumnEndDelta = 10u;
	const uint32_t OpChangeCodeOffsetAndLineOffset = 11u;
	const uint32_t OpChangeCodeLengthAndCodeOffset = 12u;
	const uint32_t OpChangeColumnEnd = 13u;
	const uint32_t OpMaximum = OpChangeColumnEnd;


	PDB_NO_DISCARD inline bool IsProcedureKind(uint16_t kind) PDB_NO_EXCEPT
	{
		return kind == S_LPROC32 || kind == S_GPROC32 || kind == S_LPROC32_ID || kind == S_GPROC32_ID;
	}

	PDB_NO_DISCARD inline uint32_t ReadU16(const uint8_t* data) PDB_NO_EXCEPT
	{
		return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u);
	}

	PDB_NO_DISCARD inline uint32_t ReadU32(const uint8_t* data) PDB_NO_EXCEPT
	{
		return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) |
			(static_cast<uint32_t>(data[2]) << 16u) | (static_cast<uint32_t>(data[3]) << 24u);
	}

	PDB_NO_DISCARD inline uint32_t AlignUp4(uint64_t value) PDB_NO_EXCEPT
	{
		return static_cast<uint32_t>((value + 3u) & ~static_cast<uint64_t>(3u));
	}

	PDB_NO_DISCARD inline int32_t DecodeSigned(uint32_t encoded) PDB_NO_EXCEPT
	{
		// the low bit carries the sign, the remaining bits are the magnitude
		const int32_t magnitude = static_cast<int32_t>(encoded >> 1u);
		return ((encoded & 1u) != 0u) ? -magnitude : magnitude;
	}


	// Reads the compressed integers used by binary annotations.  The encoding is the one from
	// cvinfo.h's CVUncompressData: values below 0x80 use one byte, values below 0x4000 use a
	// two-byte form with the 0b10 prefix, and all remaining values use a four-byte form with the
	// 0b110 prefix.  The maximum representable value is 0x1FFFFFFF.  This is not ULEB128.
	class AnnotationReader
	{
	public:
		AnnotationReader(const uint8_t* data, uint32_t size, uint32_t logicalBegin, uint32_t moduleIndex, uint32_t streamIndex, PDB::InlineSourceError& error) PDB_NO_EXCEPT
			: m_data(data)
			, m_size(size)
			, m_offset(0u)
			, m_logicalBegin(logicalBegin)
			, m_moduleIndex(moduleIndex)
			, m_streamIndex(streamIndex)
			, m_error(error)
		{
		}

		PDB_NO_DISCARD inline bool IsAtEnd(void) const PDB_NO_EXCEPT
		{
			return m_offset >= m_size;
		}

		// Consumes the remainder of the blob, requiring every raw byte to be zero.
		PDB_NO_DISCARD bool SkipRemainingZeroPadding(void) PDB_NO_EXCEPT
		{
			for (; m_offset < m_size; ++m_offset)
			{
				if (m_data[m_offset] != 0u)
				{
					return false;
				}
			}

			return true;
		}

		PDB_NO_DISCARD inline uint32_t GetLogicalOffset(void) const PDB_NO_EXCEPT
		{
			return m_logicalBegin + m_offset;
		}

		PDB_NO_DISCARD bool Read(uint32_t& value) PDB_NO_EXCEPT
		{
			const uint32_t start = m_offset;
			if (m_offset >= m_size)
			{
				Fail(PDB::InlineSourceErrorCode::TruncatedAnnotation, start, "annotation operand truncated");
				return false;
			}

			const uint8_t first = m_data[m_offset];
			if ((first & 0x80u) == 0u)
			{
				value = first;
				m_offset += 1u;
				return true;
			}

			if ((first & 0xC0u) == 0x80u)
			{
				if (static_cast<uint64_t>(m_offset) + 2u > m_size)
				{
					Fail(PDB::InlineSourceErrorCode::TruncatedAnnotation, start, "annotation two-byte operand truncated");
					return false;
				}

				value = (static_cast<uint32_t>(first & 0x3Fu) << 8u) | m_data[m_offset + 1u];
				m_offset += 2u;
				return true;
			}

			if ((first & 0xE0u) == 0xC0u)
			{
				if (static_cast<uint64_t>(m_offset) + 4u > m_size)
				{
					Fail(PDB::InlineSourceErrorCode::TruncatedAnnotation, start, "annotation four-byte operand truncated");
					return false;
				}

				value = (static_cast<uint32_t>(first & 0x1Fu) << 24u) |
					(static_cast<uint32_t>(m_data[m_offset + 1u]) << 16u) |
					(static_cast<uint32_t>(m_data[m_offset + 2u]) << 8u) |
					static_cast<uint32_t>(m_data[m_offset + 3u]);
				m_offset += 4u;
				return true;
			}

			Fail(PDB::InlineSourceErrorCode::InvalidAnnotation, start, "invalid annotation operand prefix");
			return false;
		}

		PDB_NO_DISCARD bool ReadSigned(int32_t& value) PDB_NO_EXCEPT
		{
			uint32_t encoded = 0u;
			if (!Read(encoded))
			{
				return false;
			}

			value = DecodeSigned(encoded);
			return true;
		}

		void Fail(PDB::InlineSourceErrorCode code, uint32_t relativeOffset, const char* description) PDB_NO_EXCEPT
		{
			if (m_error.IsError())
			{
				return;
			}

			m_error.code = code;
			m_error.moduleIndex = m_moduleIndex;
			m_error.streamIndex = m_streamIndex;
			m_error.offset = m_logicalBegin + relativeOffset;
			m_error.description = description;
		}

	private:
		const uint8_t* m_data;
		uint32_t m_size;
		uint32_t m_offset;
		uint32_t m_logicalBegin;
		uint32_t m_moduleIndex;
		uint32_t m_streamIndex;
		PDB::InlineSourceError& m_error;
	};


	struct Interval
	{
		Interval(void) PDB_NO_EXCEPT : begin(0u), end(0u) {}
		Interval(uint32_t b, uint32_t e) PDB_NO_EXCEPT : begin(b), end(e) {}

		uint32_t begin;
		uint32_t end;
	};

	typedef std::vector<Interval> IntervalSet;

	// Sorts and merges intervals.  Touching intervals are merged, holes between disjoint
	// intervals are preserved.  Empty intervals are dropped.
	void NormalizeIntervals(IntervalSet& intervals) PDB_NO_EXCEPT
	{
		if (intervals.empty())
		{
			return;
		}

		std::sort(intervals.begin(), intervals.end(), [](const Interval& lhs, const Interval& rhs)
		{
			return lhs.begin < rhs.begin || (lhs.begin == rhs.begin && lhs.end < rhs.end);
		});

		size_t count = 0u;
		for (size_t i = 0u; i < intervals.size(); ++i)
		{
			if (intervals[i].begin >= intervals[i].end)
			{
				continue;
			}

			if (count == 0u || static_cast<uint64_t>(intervals[i].begin) > intervals[count - 1u].end)
			{
				intervals[count] = intervals[i];
				++count;
			}
			else if (intervals[i].end > intervals[count - 1u].end)
			{
				intervals[count - 1u].end = intervals[i].end;
			}
		}

		intervals.resize(count);
	}

	// Intersects a single interval with a normalized interval set.
	PDB_NO_DISCARD IntervalSet IntersectInterval(const Interval& interval, const IntervalSet& other) PDB_NO_EXCEPT
	{
		IntervalSet result;
		for (size_t i = 0u; i < other.size(); ++i)
		{
			if (other[i].begin >= interval.end)
			{
				break;
			}

			const uint32_t begin = std::max(interval.begin, other[i].begin);
			const uint32_t end = std::min(interval.end, other[i].end);
			if (begin < end)
			{
				result.push_back(Interval(begin, end));
			}
		}

		return result;
	}

	PDB_NO_DISCARD bool Covers(const IntervalSet& intervals, uint32_t offset) PDB_NO_EXCEPT
	{
		size_t lo = 0u;
		size_t hi = intervals.size();
		while (lo < hi)
		{
			const size_t mid = lo + (hi - lo) / 2u;
			if (intervals[mid].end <= offset)
			{
				lo = mid + 1u;
			}
			else
			{
				hi = mid;
			}
		}

		return lo < intervals.size() && intervals[lo].begin <= offset;
	}


	// One source range produced by an inline site's annotation stream.  Declared offsets are
	// procedure-relative.  `pieces` holds the section-relative intervals of this range after
	// intersecting it with the coverage of all inline ancestors; an ancestor hole can therefore
	// split one declared range into several pieces.
	struct SourceRange
	{
		SourceRange(void) PDB_NO_EXCEPT
			: begin(0u)
			, end(0u)
			, bounded(false)
			, recordOffset(0u)
			, fileChecksumOffset(0u)
			, lineNumber(0u)
			, lineSpan(1u)
			, columnStart(0u)
			, columnEnd(0u)
			, hasColumnStart(false)
			, hasColumnEnd(false)
			, isStatement(true)
			, file()
			, pieces()
		{
		}

		uint32_t begin;				// procedure-relative
		uint32_t end;
		bool bounded;				// a right edge was established
		uint32_t recordOffset;		// logical module stream offset of the producing annotation opcode
		uint32_t fileChecksumOffset;
		uint32_t lineNumber;
		uint32_t lineSpan;
		uint32_t columnStart;
		uint32_t columnEnd;
		bool hasColumnStart;
		bool hasColumnEnd;
		bool isStatement;
		std::string file;
		IntervalSet pieces;			// section-relative effective intervals
	};


	struct ProcedureData
	{
		ProcedureData(void) PDB_NO_EXCEPT
			: moduleIndex(0u)
			, streamIndex(0u)
			, section(0u)
			, symbolOffset(0u)
			, codeOffset(0u)
			, codeSize(0u)
			, isId(false)
		{
		}

		uint16_t moduleIndex;
		uint16_t streamIndex;
		uint16_t section;
		uint32_t symbolOffset;
		uint32_t codeOffset;			// section-relative
		uint32_t codeSize;
		bool isId;						// S_LPROC32_ID/S_GPROC32_ID
	};


	struct SiteData
	{
		SiteData(void) PDB_NO_EXCEPT
			: moduleIndex(0u)
			, streamIndex(0u)
			, section(0u)
			, symbolOffset(0u)
			, inlinee(0u)
			, procedureIndex(InvalidIndex)
			, parentSite(InvalidIndex)
			, annotationBegin(0u)
			, annotationEnd(0u)
			, ranges()
			, children()
			, effective()
		{
		}

		uint16_t moduleIndex;
		uint16_t streamIndex;
		uint16_t section;
		uint32_t symbolOffset;
		uint32_t inlinee;
		uint32_t procedureIndex;
		uint32_t parentSite;			// nearest inline ancestor, or InvalidIndex
		uint32_t annotationBegin;		// logical module stream offsets of the annotation blob
		uint32_t annotationEnd;
		std::vector<SourceRange> ranges;
		std::vector<uint32_t> children;	// inline sites whose nearest inline ancestor is this site
		IntervalSet effective;			// section-relative union, normalized
	};


	struct CoverageEntry
	{
		CoverageEntry(uint32_t b, uint32_t e, uint32_t s) PDB_NO_EXCEPT : begin(b), end(e), siteIndex(s) {}

		uint32_t begin;			// section-relative, half-open
		uint32_t end;
		uint32_t siteIndex;
	};

	// Interval tree over coverage entries sorted by begin.  Each node stores the largest end in
	// its subtree so that queries can prune entire subtrees.
	struct SectionIndex
	{
		SectionIndex(void) PDB_NO_EXCEPT : entries(), left(), right(), maxEnd(), root(-1) {}

		std::vector<CoverageEntry> entries;
		std::vector<int32_t> left;
		std::vector<int32_t> right;
		std::vector<uint32_t> maxEnd;
		int32_t root;

		void Build(void) PDB_NO_EXCEPT
		{
			std::sort(entries.begin(), entries.end(), [](const CoverageEntry& lhs, const CoverageEntry& rhs)
			{
				return lhs.begin < rhs.begin || (lhs.begin == rhs.begin && lhs.end < rhs.end);
			});

			left.assign(entries.size(), -1);
			right.assign(entries.size(), -1);
			maxEnd.assign(entries.size(), 0u);
			root = entries.empty() ? -1 : BuildNode(0, static_cast<int32_t>(entries.size()) - 1);
		}

		int32_t BuildNode(int32_t lo, int32_t hi) PDB_NO_EXCEPT
		{
			if (lo > hi)
			{
				return -1;
			}

			const int32_t mid = lo + (hi - lo) / 2;
			const int32_t l = BuildNode(lo, mid - 1);
			const int32_t r = BuildNode(mid + 1, hi);
			const size_t index = static_cast<size_t>(mid);
			left[index] = l;
			right[index] = r;

			uint32_t maximum = entries[index].end;
			if (l >= 0)
			{
				maximum = std::max(maximum, maxEnd[static_cast<size_t>(l)]);
			}
			if (r >= 0)
			{
				maximum = std::max(maximum, maxEnd[static_cast<size_t>(r)]);
			}
			maxEnd[index] = maximum;

			return mid;
		}

		void Query(uint32_t offset, std::vector<uint32_t>& hits) const PDB_NO_EXCEPT
		{
			QueryNode(root, offset, hits);
		}

		void QueryNode(int32_t node, uint32_t offset, std::vector<uint32_t>& hits) const PDB_NO_EXCEPT
		{
			if (node < 0)
			{
				return;
			}

			const size_t index = static_cast<size_t>(node);
			if (maxEnd[index] <= offset)
			{
				return;
			}

			const CoverageEntry& entry = entries[index];

			QueryNode(left[index], offset, hits);

			if (entry.begin <= offset && offset < entry.end)
			{
				hits.push_back(static_cast<uint32_t>(node));
			}

			if (entry.begin <= offset)
			{
				QueryNode(right[index], offset, hits);
			}
		}
	};


	struct InlineeInfo
	{
		InlineeInfo(void) PDB_NO_EXCEPT : fileChecksumOffset(0u), lineNumber(0u), extraFiles() {}

		uint32_t fileChecksumOffset;
		uint32_t lineNumber;
		std::vector<uint32_t> extraFiles;
	};

	inline bool operator==(const InlineeInfo& lhs, const InlineeInfo& rhs) PDB_NO_EXCEPT
	{
		return lhs.fileChecksumOffset == rhs.fileChecksumOffset && lhs.lineNumber == rhs.lineNumber &&
			lhs.extraFiles == rhs.extraFiles;
	}


	// An owned, parsed copy of one module's symbols and C13 data.  The copy only lives while the
	// module is being parsed; the index retains decoded ranges and file paths.
	struct ModuleParse
	{
		ModuleParse(void) PDB_NO_EXCEPT
			: moduleIndex(0u)
			, streamIndex(0u)
			, symbolBytes(0u)
			, c11Bytes(0u)
			, c13Base(0u)
			, symbols()
			, c13()
			, files()
			, inlinees()
		{
		}

		uint16_t moduleIndex;
		uint16_t streamIndex;
		uint32_t symbolBytes;
		uint32_t c11Bytes;
		uint32_t c13Base;
		std::vector<uint8_t> symbols;				// starts with the 4-byte signature
		std::vector<uint8_t> c13;
		std::map<uint32_t, std::string> files;		// checksum subsection offset -> "/names" path
		std::map<uint32_t, InlineeInfo> inlinees;	// inlinee id -> definition
	};


	struct SymbolRecordInfo
	{
		SymbolRecordInfo(void) PDB_NO_EXCEPT
			: offset(0u)
			, kind(0u)
			, dataStart(0u)
			, dataEnd(0u)
		{
		}

		uint32_t offset;
		uint16_t kind;
		uint32_t dataStart;
		uint32_t dataEnd;			// one past the record's data
	};


	struct ScopeInfo
	{
		ScopeInfo(void) PDB_NO_EXCEPT
			: recordOffset(0u)
			, recordEnd(0u)
			, ownerProcedure(InvalidIndex)
			, nearestInline(InvalidIndex)
			, globalSite(InvalidIndex)
			, isProcedure(false)
			, isBlock(false)
			, isSite(false)
		{
		}

		uint32_t recordOffset;
		uint32_t recordEnd;
		uint32_t ownerProcedure;	// global procedure index
		uint32_t nearestInline;		// global site index of the closest enclosing site
		uint32_t globalSite;		// global site index when this scope is a site
		bool isProcedure;
		bool isBlock;
		bool isSite;
	};


	struct NamesView
	{
		NamesView(const uint8_t* d, uint32_t n) PDB_NO_EXCEPT : data(d), size(n) {}

		PDB_NO_DISCARD bool Resolve(uint32_t filenameOffset, std::string& result) const PDB_NO_EXCEPT
		{
			if (filenameOffset >= size)
			{
				return false;
			}

			const uint8_t* start = data + filenameOffset;
			const uint8_t* end = data + size;
			const uint8_t* terminator = std::find(start, end, static_cast<uint8_t>(0));
			if (terminator == end)
			{
				return false;
			}

			result.assign(reinterpret_cast<const char*>(start), static_cast<size_t>(terminator - start));
			return true;
		}

		const uint8_t* data;
		uint32_t size;
	};


	struct PendingChecksum
	{
		PendingChecksum(void) PDB_NO_EXCEPT : offset(0u), filenameOffset(0u), logicalOffset(0u) {}

		uint32_t offset;			// relative to the checksum subsection data
		uint32_t filenameOffset;
		uint32_t logicalOffset;		// logical module stream offset of the record
	};


	struct PendingInlinee
	{
		PendingInlinee(void) PDB_NO_EXCEPT
			: inlinee(0u)
			, fileChecksumOffset(0u)
			, lineNumber(0u)
			, extraFiles()
			, logicalOffset(0u)
		{
		}

		uint32_t inlinee;
		uint32_t fileChecksumOffset;
		uint32_t lineNumber;
		std::vector<uint32_t> extraFiles;
		uint32_t logicalOffset;
	};
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
struct PDB::InlineSourceIndex::Impl
{
	Impl(void) PDB_NO_EXCEPT
		: error()
		, procedures()
		, sites()
		, sections()
	{
	}

	PDB::InlineSourceError error;
	std::vector<ProcedureData> procedures;
	std::vector<SiteData> sites;
	std::map<uint16_t, SectionIndex> sections;

	void Fail(PDB::InlineSourceErrorCode code, uint32_t moduleIndex, uint32_t streamIndex, uint32_t offset, const char* description) PDB_NO_EXCEPT
	{
		if (error.IsError())
		{
			return;
		}

		error.code = code;
		error.moduleIndex = moduleIndex;
		error.streamIndex = streamIndex;
		error.offset = offset;
		error.description = description;
	}

	void ParseC13(ModuleParse& module, const NamesView& names) PDB_NO_EXCEPT;
	void ParseSymbols(ModuleParse& module) PDB_NO_EXCEPT;
	void DecodeAnnotations(const ModuleParse& module, SiteData& site, const ProcedureData& procedure) PDB_NO_EXCEPT;
	void BuildCoverage(void) PDB_NO_EXCEPT;

	PDB_NO_DISCARD bool Build(const RawFile& file, uint32_t namesStreamIndex, const InlineSourceModule* modules, size_t moduleCount) PDB_NO_EXCEPT;
};


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void PDB::InlineSourceIndex::Impl::ParseC13(ModuleParse& module, const NamesView& names) PDB_NO_EXCEPT
{
	std::vector<PendingChecksum> checksums;
	std::vector<PendingInlinee> inlinees;

	const uint8_t* c13 = module.c13.empty() ? nullptr : &module.c13[0];
	const uint32_t c13Size = static_cast<uint32_t>(module.c13.size());

	uint32_t rel = 0u;
	while (static_cast<uint64_t>(rel) + 8u <= c13Size)
	{
		const uint32_t logicalSubsection = module.c13Base + rel;
		const uint32_t kind = ReadU32(c13 + rel);
		const uint32_t size = ReadU32(c13 + rel + 4u);
		const uint32_t dataRel = rel + 8u;
		const uint64_t dataEnd64 = static_cast<uint64_t>(dataRel) + size;

		if (dataEnd64 > c13Size)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidC13Section, module.moduleIndex, module.streamIndex, logicalSubsection, "C13 subsection extends past the module slice");
			return;
		}

		const uint32_t dataEnd = static_cast<uint32_t>(dataEnd64);

		if (kind == S_FILECHECKSUMS)
		{
			uint32_t p = dataRel;
			while (p < dataEnd)
			{
				const uint32_t recordLogical = module.c13Base + p;
				if (static_cast<uint64_t>(p) + 6u > dataEnd)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidC13Section, module.moduleIndex, module.streamIndex, recordLogical, "truncated file checksum header");
					return;
				}

				const uint32_t filenameOffset = ReadU32(c13 + p);
				const uint32_t checksumSize = c13[p + 4u];
				const uint64_t recordEnd64 = static_cast<uint64_t>(p) + 6u + checksumSize;
				if (recordEnd64 > dataEnd)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidC13Section, module.moduleIndex, module.streamIndex, recordLogical, "truncated file checksum");
					return;
				}

				PendingChecksum record;
				record.offset = p - dataRel;
				record.filenameOffset = filenameOffset;
				record.logicalOffset = recordLogical;
				checksums.push_back(record);

				const uint32_t recordEnd = static_cast<uint32_t>(recordEnd64);
				if (recordEnd == dataEnd)
				{
					break;
				}

				const uint32_t next = AlignUp4(module.c13Base + recordEnd) - module.c13Base;
				if (next <= p || next > dataEnd)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidC13Section, module.moduleIndex, module.streamIndex, recordLogical, "file checksum records do not fill the subsection");
					return;
				}

				p = next;
			}
		}
		else if (kind == S_INLINEELINES)
		{
			if (size < 4u)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidC13Section, module.moduleIndex, module.streamIndex, logicalSubsection, "inlinee lines subsection has no signature");
				return;
			}

			const uint32_t signature = ReadU32(c13 + dataRel);
			if (signature > 1u)
			{
				Fail(PDB::InlineSourceErrorCode::UnsupportedInlineeLinesSignature, module.moduleIndex, module.streamIndex, module.c13Base + dataRel, "unsupported INLINEELINES signature");
				return;
			}

			uint32_t p = AlignUp4(module.c13Base + dataRel + 4u) - module.c13Base;
			while (p < dataEnd)
			{
				const uint32_t recordLogical = module.c13Base + p;
				if (static_cast<uint64_t>(p) + 12u > dataEnd)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidC13Section, module.moduleIndex, module.streamIndex, recordLogical, "truncated inlinee source line");
					return;
				}

				PendingInlinee record;
				record.inlinee = ReadU32(c13 + p);
				record.fileChecksumOffset = ReadU32(c13 + p + 4u);
				record.lineNumber = ReadU32(c13 + p + 8u);
				record.logicalOffset = recordLogical;

				uint64_t recordSize = 12u;
				if (signature == 1u)
				{
					if (static_cast<uint64_t>(p) + 16u > dataEnd)
					{
						Fail(PDB::InlineSourceErrorCode::InvalidC13Section, module.moduleIndex, module.streamIndex, recordLogical, "truncated extended inlinee source line");
						return;
					}

					const uint32_t extraLines = ReadU32(c13 + p + 12u);
					recordSize = 16u + 4u * static_cast<uint64_t>(extraLines);
					if (static_cast<uint64_t>(p) + recordSize > dataEnd)
					{
						Fail(PDB::InlineSourceErrorCode::InvalidC13Section, module.moduleIndex, module.streamIndex, recordLogical, "extended inlinee file array overruns the subsection");
						return;
					}

					record.extraFiles.reserve(extraLines);
					for (uint32_t i = 0u; i < extraLines; ++i)
					{
						record.extraFiles.push_back(ReadU32(c13 + p + 16u + 4u * i));
					}
				}

				inlinees.push_back(record);

				const uint32_t recordEnd = static_cast<uint32_t>(static_cast<uint64_t>(p) + recordSize);
				if (recordEnd == dataEnd)
				{
					break;
				}

				const uint32_t next = AlignUp4(module.c13Base + recordEnd) - module.c13Base;
				if (next <= p || next > dataEnd)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidC13Section, module.moduleIndex, module.streamIndex, recordLogical, "inlinee source lines do not fill the subsection");
					return;
				}

				p = next;
			}
		}

		const uint32_t next = AlignUp4(module.c13Base + dataEnd) - module.c13Base;
		if (next <= rel)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidC13Section, module.moduleIndex, module.streamIndex, logicalSubsection, "C13 subsection makes no progress");
			return;
		}

		rel = next;
	}

	// resolve file checksum records through the "/names" stream
	for (size_t i = 0u; i < checksums.size(); ++i)
	{
		std::string path;
		if (!names.Resolve(checksums[i].filenameOffset, path))
		{
			Fail(PDB::InlineSourceErrorCode::InvalidFileReference, module.moduleIndex, module.streamIndex, checksums[i].logicalOffset, "file checksum references an invalid /names offset");
			return;
		}

		if (module.files.find(checksums[i].offset) == module.files.end())
		{
			module.files[checksums[i].offset] = path;
		}
	}

	// validate and publish the inlinee definitions
	for (size_t i = 0u; i < inlinees.size(); ++i)
	{
		const PendingInlinee& record = inlinees[i];

		if (module.files.find(record.fileChecksumOffset) == module.files.end())
		{
			Fail(PDB::InlineSourceErrorCode::InvalidFileReference, module.moduleIndex, module.streamIndex, record.logicalOffset, "inlinee references an unknown file checksum");
			return;
		}

		for (size_t j = 0u; j < record.extraFiles.size(); ++j)
		{
			if (module.files.find(record.extraFiles[j]) == module.files.end())
			{
				Fail(PDB::InlineSourceErrorCode::InvalidFileReference, module.moduleIndex, module.streamIndex, record.logicalOffset, "inlinee references an unknown extra file checksum");
				return;
			}
		}

		InlineeInfo info;
		info.fileChecksumOffset = record.fileChecksumOffset;
		info.lineNumber = record.lineNumber;
		info.extraFiles = record.extraFiles;

		const auto existing = module.inlinees.find(record.inlinee);
		if (existing == module.inlinees.end())
		{
			module.inlinees[record.inlinee] = info;
		}
		else if (!(existing->second == info))
		{
			Fail(PDB::InlineSourceErrorCode::DuplicateInlineeDefinition, module.moduleIndex, module.streamIndex, record.logicalOffset, "conflicting duplicate inlinee definition");
			return;
		}
	}
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void PDB::InlineSourceIndex::Impl::DecodeAnnotations(const ModuleParse& module, SiteData& site, const ProcedureData& procedure) PDB_NO_EXCEPT
{
	const auto inlineeIt = module.inlinees.find(site.inlinee);
	if (inlineeIt == module.inlinees.end())
	{
		Fail(PDB::InlineSourceErrorCode::InvalidInlineeReference, module.moduleIndex, module.streamIndex, site.symbolOffset, "inline site references an undefined inlinee");
		return;
	}

	const uint32_t size = site.annotationEnd - site.annotationBegin;
	const uint8_t* data = module.symbols.empty() ? nullptr : (&module.symbols[0] + site.annotationBegin);

	AnnotationReader reader(data, size, site.annotationBegin, module.moduleIndex, module.streamIndex, error);

	// the state starts at procedure-relative offset 0, with the inlinee's base file and line,
	// a line span of one, statement=true and no columns
	uint32_t codeOffset = 0u;
	uint32_t fileChecksumOffset = inlineeIt->second.fileChecksumOffset;
	uint32_t lineNumber = inlineeIt->second.lineNumber;
	uint32_t lineSpan = 1u;
	uint32_t columnStart = 0u;
	uint32_t columnEnd = 0u;
	bool hasColumnStart = false;
	bool hasColumnEnd = false;
	bool isStatement = true;

	int32_t pending = -1;
	uint32_t monotonicFloor = 0u;

	// Emits a record at the current code offset.  Without an explicit length the record stays
	// open and is sealed by the next record's start.
	auto emit = [&](uint32_t begin, bool explicitLength, uint32_t length, uint32_t recordOffset) -> bool
	{
		if (module.files.find(fileChecksumOffset) == module.files.end())
		{
			Fail(PDB::InlineSourceErrorCode::InvalidFileReference, module.moduleIndex, module.streamIndex, recordOffset, "annotation references an unknown file checksum");
			return false;
		}

		if (static_cast<uint64_t>(lineNumber) + lineSpan - 1u > MaxLineNumber)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidLineOrColumn, module.moduleIndex, module.streamIndex, recordOffset, "annotation line range out of range");
			return false;
		}

		if (columnStart > MaxColumnNumber || columnEnd > MaxColumnNumber)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidLineOrColumn, module.moduleIndex, module.streamIndex, recordOffset, "annotation column out of range");
			return false;
		}

		if (explicitLength)
		{
			const uint64_t end = static_cast<uint64_t>(begin) + length;
			if (end > MaxCoordinate || (procedure.codeSize != 0u && end > procedure.codeSize))
			{
				Fail(PDB::InlineSourceErrorCode::InvalidCodeRange, module.moduleIndex, module.streamIndex, recordOffset, "explicit source length lies outside the physical procedure");
				return false;
			}
		}

		// records are emitted in non-decreasing address order within a site; a record starting
		// before the end of a previously bounded range is a backwards move
		if (begin < monotonicFloor)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidCodeRange, module.moduleIndex, module.streamIndex, recordOffset, "annotation code offset moves backwards");
			return false;
		}

		if (pending >= 0)
		{
			SourceRange& previous = site.ranges[static_cast<size_t>(pending)];
			previous.end = begin;
			previous.bounded = true;
			pending = -1;
		}

		SourceRange range;
		range.begin = begin;
		range.end = explicitLength ? begin + length : begin;
		range.bounded = explicitLength;
		range.recordOffset = recordOffset;
		range.fileChecksumOffset = fileChecksumOffset;
		range.lineNumber = lineNumber;
		range.lineSpan = lineSpan;
		range.columnStart = columnStart;
		range.columnEnd = columnEnd;
		range.hasColumnStart = hasColumnStart;
		range.hasColumnEnd = hasColumnEnd;
		range.isStatement = isStatement;
		range.file = module.files.find(fileChecksumOffset)->second;

		site.ranges.push_back(range);

		if (!explicitLength)
		{
			pending = static_cast<int32_t>(site.ranges.size()) - 1;
		}

		// the next record may not start before the end of an explicitly sized range
		if (explicitLength)
		{
			monotonicFloor = begin + length;
		}
		else
		{
			monotonicFloor = std::max(monotonicFloor, begin);
		}

		return true;
	};

	auto advance = [&](uint32_t delta, uint32_t recordOffset) -> bool
	{
		const uint64_t next = static_cast<uint64_t>(codeOffset) + delta;
		if (next > MaxCoordinate)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidCodeRange, module.moduleIndex, module.streamIndex, recordOffset, "annotation code offset exceeds 32 bits");
			return false;
		}

		if (procedure.codeSize != 0u && next > procedure.codeSize)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidCodeRange, module.moduleIndex, module.streamIndex, recordOffset, "annotation code offset lies outside the physical procedure");
			return false;
		}

		codeOffset = static_cast<uint32_t>(next);
		return true;
	};

	auto applyLineDelta = [&](int32_t delta, uint32_t recordOffset) -> bool
	{
		const int64_t next = static_cast<int64_t>(lineNumber) + delta;
		if (next < 0 || next > static_cast<int64_t>(MaxLineNumber))
		{
			Fail(PDB::InlineSourceErrorCode::InvalidLineOrColumn, module.moduleIndex, module.streamIndex, recordOffset, "line delta leaves the valid line range");
			return false;
		}

		lineNumber = static_cast<uint32_t>(next);
		return true;
	};

	while (!reader.IsAtEnd() && !error.IsError())
	{
		const uint32_t opcodeOffset = reader.GetLogicalOffset();
		uint32_t opcode = 0u;
		if (!reader.Read(opcode))
		{
			return;
		}

		if (opcode == 0u)
		{
			// zero terminates the stream; the remaining raw bytes have to be padding zeroes
			const uint32_t paddingOffset = reader.GetLogicalOffset();
			if (!reader.SkipRemainingZeroPadding())
			{
				Fail(PDB::InlineSourceErrorCode::InvalidAnnotation, module.moduleIndex, module.streamIndex, paddingOffset, "non-zero annotation padding after the terminator");
			}

			break;
		}

		if (opcode > OpMaximum)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidAnnotation, module.moduleIndex, module.streamIndex, opcodeOffset, "unknown annotation opcode");
			return;
		}

		uint32_t value = 0u;
		int32_t signedValue = 0;
		switch (opcode)
		{
			case OpCodeOffset:
				if (!reader.Read(value))
				{
					return;
				}

				if (value < monotonicFloor)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidCodeRange, module.moduleIndex, module.streamIndex, opcodeOffset, "annotation code offset moves backwards");
					return;
				}

				if (procedure.codeSize != 0u && value > procedure.codeSize)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidCodeRange, module.moduleIndex, module.streamIndex, opcodeOffset, "annotation code offset lies outside the physical procedure");
					return;
				}

				codeOffset = value;
				break;

			case OpChangeCodeOffset:
				if (!reader.Read(value))
				{
					return;
				}

				if (value != 0u)
				{
					Fail(PDB::InlineSourceErrorCode::UnsupportedSeparateCodeBlock, module.moduleIndex, module.streamIndex, opcodeOffset, "non-zero separated code offset is not supported");
					return;
				}

				break;

			case OpAdvanceCodeOffset:
				if (!reader.Read(value))
				{
					return;
				}

				if (!advance(value, opcodeOffset))
				{
					return;
				}

				if (!emit(codeOffset, false, 0u, opcodeOffset))
				{
					return;
				}

				break;

			case OpChangeCodeLength:
				if (!reader.Read(value))
				{
					return;
				}

				if (pending < 0)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidAnnotation, module.moduleIndex, module.streamIndex, opcodeOffset, "explicit code length without a pending record");
					return;
				}

				{
					SourceRange& previous = site.ranges[static_cast<size_t>(pending)];
					const uint64_t end = static_cast<uint64_t>(previous.begin) + value;
					if (end > MaxCoordinate || (procedure.codeSize != 0u && end > procedure.codeSize))
					{
						Fail(PDB::InlineSourceErrorCode::InvalidCodeRange, module.moduleIndex, module.streamIndex, opcodeOffset, "explicit source length lies outside the physical procedure");
						return;
					}

					previous.end = static_cast<uint32_t>(end);
					previous.bounded = true;
					pending = -1;
					codeOffset = static_cast<uint32_t>(end);
					monotonicFloor = std::max(monotonicFloor, static_cast<uint32_t>(end));
				}

				break;

			case OpChangeFile:
				if (!reader.Read(value))
				{
					return;
				}

				if (module.files.find(value) == module.files.end())
				{
					Fail(PDB::InlineSourceErrorCode::InvalidFileReference, module.moduleIndex, module.streamIndex, opcodeOffset, "ChangeFile references an unknown checksum offset");
					return;
				}

				fileChecksumOffset = value;
				break;

			case OpChangeLineOffset:
				if (!reader.ReadSigned(signedValue))
				{
					return;
				}

				if (!applyLineDelta(signedValue, opcodeOffset))
				{
					return;
				}

				break;

			case OpChangeLineEndDelta:
				if (!reader.Read(value))
				{
					return;
				}

				// the operand is the source line span; a range always covers at least one line
				if (value == 0u)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidLineOrColumn, module.moduleIndex, module.streamIndex, opcodeOffset, "line span is zero");
					return;
				}

				if (static_cast<uint64_t>(lineNumber) + value - 1u > MaxLineNumber)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidLineOrColumn, module.moduleIndex, module.streamIndex, opcodeOffset, "line span leaves the valid line range");
					return;
				}

				lineSpan = value;
				break;

			case OpChangeRangeKind:
				if (!reader.Read(value))
				{
					return;
				}

				if (value > 1u)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidAnnotation, module.moduleIndex, module.streamIndex, opcodeOffset, "invalid range kind");
					return;
				}

				isStatement = (value == 1u);
				break;

			case OpChangeColumnStart:
				if (!reader.Read(value))
				{
					return;
				}

				if (value > MaxColumnNumber)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidLineOrColumn, module.moduleIndex, module.streamIndex, opcodeOffset, "column start out of range");
					return;
				}

				columnStart = value;
				hasColumnStart = (value != 0u);
				break;

			case OpChangeColumnEndDelta:
				if (!reader.ReadSigned(signedValue))
				{
					return;
				}

				// without an established column end, a delta never creates one
				if (!hasColumnEnd)
				{
					break;
				}

				{
					const int64_t next = static_cast<int64_t>(columnEnd) + signedValue;
					if (next < 0 || next > static_cast<int64_t>(MaxColumnNumber))
					{
						Fail(PDB::InlineSourceErrorCode::InvalidLineOrColumn, module.moduleIndex, module.streamIndex, opcodeOffset, "column end delta leaves the valid column range");
						return;
					}

					columnEnd = static_cast<uint32_t>(next);
					hasColumnEnd = (columnEnd != 0u);
				}

				break;

			case OpChangeCodeOffsetAndLineOffset:
				if (!reader.Read(value))
				{
					return;
				}

				{
					const uint32_t codeDelta = value & 0x0Fu;
					const int32_t lineDelta = DecodeSigned(value >> 4u);

					if (!advance(codeDelta, opcodeOffset))
					{
						return;
					}

					if (!applyLineDelta(lineDelta, opcodeOffset))
					{
						return;
					}

					if (!emit(codeOffset, false, 0u, opcodeOffset))
					{
						return;
					}
				}

				break;

			case OpChangeCodeLengthAndCodeOffset:
			{
				// the explicit length comes first, then the code delta
				uint32_t length = 0u;
				if (!reader.Read(length))
				{
					return;
				}

				uint32_t codeDelta = 0u;
				if (!reader.Read(codeDelta))
				{
					return;
				}

				if (!advance(codeDelta, opcodeOffset))
				{
					return;
				}

				if (!emit(codeOffset, true, length, opcodeOffset))
				{
					return;
				}

				const uint64_t next = static_cast<uint64_t>(codeOffset) + length;
				if (next > MaxCoordinate)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidCodeRange, module.moduleIndex, module.streamIndex, opcodeOffset, "annotation code offset exceeds 32 bits");
					return;
				}

				codeOffset = static_cast<uint32_t>(next);
			}
			break;

			case OpChangeColumnEnd:
				if (!reader.Read(value))
				{
					return;
				}

				if (value > MaxColumnNumber)
				{
					Fail(PDB::InlineSourceErrorCode::InvalidLineOrColumn, module.moduleIndex, module.streamIndex, opcodeOffset, "column end out of range");
					return;
				}

				columnEnd = value;
				hasColumnEnd = (value != 0u);
				break;

			default:
				Fail(PDB::InlineSourceErrorCode::InvalidAnnotation, module.moduleIndex, module.streamIndex, opcodeOffset, "unhandled annotation opcode");
				return;
		}
	}

	// the final open record has no verifiable right edge and is left unbounded; it never matches.
	(void)pending;
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void PDB::InlineSourceIndex::Impl::ParseSymbols(ModuleParse& module) PDB_NO_EXCEPT
{
	if (module.symbols.size() < 4u)
	{
		Fail(PDB::InlineSourceErrorCode::InvalidModuleSlice, module.moduleIndex, module.streamIndex, 0u, "module symbol slice is too small for its signature");
		return;
	}

	std::vector<SymbolRecordInfo> records;
	std::map<uint32_t, uint32_t> recordByOffset;

	uint32_t offset = 4u;
	while (offset < module.symbolBytes)
	{
		if (static_cast<uint64_t>(offset) + 4u > module.symbolBytes)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidSymbolRecord, module.moduleIndex, module.streamIndex, offset, "truncated symbol record header");
			return;
		}

		const uint32_t size = ReadU16(&module.symbols[offset]);
		const uint16_t kind = static_cast<uint16_t>(ReadU16(&module.symbols[offset + 2u]));
		const uint32_t dataStart = offset + 4u;
		const uint64_t dataEnd64 = static_cast<uint64_t>(offset) + 2u + size;

		if (size < 2u || dataEnd64 > module.symbolBytes)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidSymbolRecord, module.moduleIndex, module.streamIndex, offset, "symbol record extends past the module slice");
			return;
		}

		if (kind == S_SEPCODE)
		{
			Fail(PDB::InlineSourceErrorCode::UnsupportedSeparateCodeBlock, module.moduleIndex, module.streamIndex, offset, "separated code blocks are not supported");
			return;
		}

		if (kind == S_INLINESITE2)
		{
			Fail(PDB::InlineSourceErrorCode::UnsupportedInlineSite2, module.moduleIndex, module.streamIndex, offset, "S_INLINESITE2 is not supported");
			return;
		}

		SymbolRecordInfo record;
		record.offset = offset;
		record.kind = kind;
		record.dataStart = dataStart;
		record.dataEnd = static_cast<uint32_t>(dataEnd64);
		recordByOffset[offset] = static_cast<uint32_t>(records.size());
		records.push_back(record);

		const uint32_t next = AlignUp4(dataEnd64);
		if (next <= offset)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidSymbolRecord, module.moduleIndex, module.streamIndex, offset, "symbol record makes no progress");
			return;
		}

		offset = next;
	}

	// Walk the scopes in stream order.  Parents always precede their children, so each nested
	// scope can resolve its procedure and its closest enclosing inline site on the way.
	std::vector<ScopeInfo> scopes;
	std::map<uint32_t, uint32_t> scopeByOffset;

	const uint8_t* data = &module.symbols[0];

	for (size_t i = 0u; i < records.size(); ++i)
	{
		const SymbolRecordInfo& record = records[i];

		if (IsProcedureKind(record.kind))
		{
			if (static_cast<uint64_t>(record.dataStart) + 34u > record.dataEnd)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidSymbolRecord, module.moduleIndex, module.streamIndex, record.offset, "procedure record is too small");
				return;
			}

			ProcedureData procedure;
			procedure.moduleIndex = module.moduleIndex;
			procedure.streamIndex = module.streamIndex;
			procedure.symbolOffset = record.offset;
			procedure.codeSize = ReadU32(data + record.dataStart + 12u);
			procedure.codeOffset = ReadU32(data + record.dataStart + 28u);
			procedure.section = static_cast<uint16_t>(ReadU16(data + record.dataStart + 32u));
			procedure.isId = (record.kind == S_LPROC32_ID || record.kind == S_GPROC32_ID);

			const uint32_t procedureIndex = static_cast<uint32_t>(procedures.size());
			procedures.push_back(procedure);

			ScopeInfo scope;
			scope.recordOffset = record.offset;
			scope.recordEnd = ReadU32(data + record.dataStart + 4u);
			scope.ownerProcedure = procedureIndex;
			scope.isProcedure = true;
			scopeByOffset[record.offset] = static_cast<uint32_t>(scopes.size());
			scopes.push_back(scope);
		}
		else if (record.kind == S_BLOCK32)
		{
			if (static_cast<uint64_t>(record.dataStart) + 18u > record.dataEnd)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidSymbolRecord, module.moduleIndex, module.streamIndex, record.offset, "block record is too small");
				return;
			}

			const uint32_t parentOffset = ReadU32(data + record.dataStart);
			const auto parentIt = scopeByOffset.find(parentOffset);
			if (parentIt == scopeByOffset.end())
			{
				Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, record.offset, "block parent does not reference an enclosing scope");
				return;
			}

			const ScopeInfo& parent = scopes[parentIt->second];
			if (parent.recordEnd <= record.offset)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, record.offset, "block starts at or after its parent's terminator");
				return;
			}

			ScopeInfo scope;
			scope.recordOffset = record.offset;
			scope.recordEnd = ReadU32(data + record.dataStart + 4u);
			scope.ownerProcedure = parent.ownerProcedure;
			scope.nearestInline = parent.isSite ? parent.globalSite : parent.nearestInline;
			scope.isBlock = true;
			scopeByOffset[record.offset] = static_cast<uint32_t>(scopes.size());
			scopes.push_back(scope);
		}
		else if (record.kind == S_INLINESITE)
		{
			if (static_cast<uint64_t>(record.dataStart) + 12u > record.dataEnd)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidSymbolRecord, module.moduleIndex, module.streamIndex, record.offset, "inline site record is too small");
				return;
			}

			const uint32_t parentOffset = ReadU32(data + record.dataStart);
			const auto parentIt = scopeByOffset.find(parentOffset);
			if (parentIt == scopeByOffset.end())
			{
				Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, record.offset, "inline site parent does not reference an enclosing scope");
				return;
			}

			const ScopeInfo& parent = scopes[parentIt->second];
			if (parent.recordEnd <= record.offset)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, record.offset, "inline site starts at or after its parent's terminator");
				return;
			}

			SiteData site;
			site.moduleIndex = module.moduleIndex;
			site.streamIndex = module.streamIndex;
			site.symbolOffset = record.offset;
			site.inlinee = ReadU32(data + record.dataStart + 8u);
			site.procedureIndex = parent.ownerProcedure;
			site.section = procedures[parent.ownerProcedure].section;
			site.parentSite = parent.isSite ? parent.globalSite : parent.nearestInline;
			site.annotationBegin = record.dataStart + 12u;
			site.annotationEnd = record.dataEnd;

			const uint32_t siteIndex = static_cast<uint32_t>(sites.size());
			sites.push_back(site);

			if (site.parentSite != InvalidIndex)
			{
				sites[site.parentSite].children.push_back(siteIndex);
			}

			ScopeInfo scope;
			scope.recordOffset = record.offset;
			scope.recordEnd = ReadU32(data + record.dataStart + 4u);
			scope.ownerProcedure = parent.ownerProcedure;
			scope.nearestInline = site.parentSite;
			scope.globalSite = siteIndex;
			scope.isSite = true;
			scopeByOffset[record.offset] = static_cast<uint32_t>(scopes.size());
			scopes.push_back(scope);

			DecodeAnnotations(module, sites[siteIndex], procedures[site.procedureIndex]);
			if (error.IsError())
			{
				return;
			}
		}
	}

	// Validate that every scope is closed by the record it points at, that the terminator kind
	// matches, and that no scope crosses a physical procedure.
	for (size_t i = 0u; i < scopes.size(); ++i)
	{
		const ScopeInfo& scope = scopes[i];

		if (scope.recordEnd <= scope.recordOffset)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, scope.recordOffset, "scope terminator does not follow its scope");
			return;
		}

		const ProcedureData& procedure = procedures[scope.ownerProcedure];

		if (scope.recordEnd > module.symbolBytes)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, scope.recordOffset, "scope terminator lies outside the symbol slice");
			return;
		}

		const auto endIt = recordByOffset.find(scope.recordEnd);
		if (endIt == recordByOffset.end())
		{
			Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, scope.recordOffset, "scope terminator does not reference a record boundary");
			return;
		}

		const uint16_t endKind = records[endIt->second].kind;

		if (scope.isProcedure)
		{
			// an _ID procedure ends with S_PROC_ID_END, any other procedure with S_END
			const uint16_t expectedEnd = procedure.isId ? S_PROC_ID_END : S_END;
			if (endKind != expectedEnd)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, scope.recordOffset, "procedure terminator does not match its kind");
				return;
			}
		}
		else if (scope.isSite)
		{
			if (endKind != S_INLINESITE_END)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, scope.recordOffset, "inline site terminator is not S_INLINESITE_END");
				return;
			}

			if (scope.recordEnd > scope.recordOffset && scope.recordEnd < procedure.symbolOffset)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, scope.recordOffset, "inline site terminator precedes its procedure");
				return;
			}
		}
		else if (scope.isBlock)
		{
			if (endKind != S_END)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, scope.recordOffset, "block terminator is not S_END");
				return;
			}
		}
	}

	// no scope may extend beyond its procedure's own terminator
	for (size_t i = 0u; i < scopes.size(); ++i)
	{
		const ScopeInfo& scope = scopes[i];
		if (scope.isProcedure)
		{
			continue;
		}

		const ProcedureData& procedure = procedures[scope.ownerProcedure];
		const auto recordIt = recordByOffset.find(scope.recordOffset);
		if (recordIt == recordByOffset.end())
		{
			Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, scope.recordOffset, "scope record boundary is unknown");
			return;
		}

		// the outermost procedure terminator is the procedure scope's recordEnd
		const auto procedureScopeIt = scopeByOffset.find(procedure.symbolOffset);
		if (procedureScopeIt == scopeByOffset.end())
		{
			Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, procedure.symbolOffset, "procedure scope is missing");
			return;
		}

		const uint32_t procedureEnd = scopes[procedureScopeIt->second].recordEnd;
		if (scope.recordEnd > procedureEnd)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidScopeLink, module.moduleIndex, module.streamIndex, scope.recordOffset, "scope crosses its physical procedure");
			return;
		}
	}
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void PDB::InlineSourceIndex::Impl::BuildCoverage(void) PDB_NO_EXCEPT
{
	for (size_t siteIndex = 0u; siteIndex < sites.size(); ++siteIndex)
	{
		SiteData& site = sites[siteIndex];
		const ProcedureData& procedure = procedures[site.procedureIndex];

		const uint64_t procedureBegin = procedure.codeOffset;
		const uint64_t procedureEnd = procedureBegin + procedure.codeSize;
		const bool hasProcedureBound = (procedure.codeSize != 0u);

		if (procedureEnd > MaxCoordinate)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidCodeRange, procedure.moduleIndex, procedure.streamIndex, procedure.symbolOffset, "physical procedure extends past 32-bit code coordinates");
			return;
		}

		// the coverage inherited from the enclosing scope.  A top-level site of a procedure with
		// unknown code size has no ancestor bound at all; children of a site whose parent covers
		// nothing inherit an empty set and therefore contribute nothing.
		IntervalSet ancestor;
		const bool hasAncestor = (site.parentSite != InvalidIndex) || hasProcedureBound;
		if (site.parentSite != InvalidIndex)
		{
			ancestor = sites[site.parentSite].effective;
		}
		else if (hasProcedureBound)
		{
			ancestor.push_back(Interval(static_cast<uint32_t>(procedureBegin), static_cast<uint32_t>(procedureEnd)));
		}

		IntervalSet unionOfPieces;
		for (size_t rangeIndex = 0u; rangeIndex < site.ranges.size(); ++rangeIndex)
		{
			SourceRange& range = site.ranges[rangeIndex];
			range.pieces.clear();

			if (!range.bounded || range.begin >= range.end)
			{
				continue;
			}

			const uint64_t begin = procedureBegin + range.begin;
			const uint64_t end = procedureBegin + range.end;
			if (begin > MaxCoordinate || end > MaxCoordinate || end < begin)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidCodeRange, procedure.moduleIndex, procedure.streamIndex, range.recordOffset, "source range exceeds 32-bit code coordinates");
				return;
			}

			if (hasProcedureBound && end > procedureEnd)
			{
				Fail(PDB::InlineSourceErrorCode::InvalidCodeRange, procedure.moduleIndex, procedure.streamIndex, range.recordOffset, "source range lies outside the physical procedure");
				return;
			}

			IntervalSet pieces;
			pieces.push_back(Interval(static_cast<uint32_t>(begin), static_cast<uint32_t>(end)));
			if (hasAncestor)
			{
				pieces = IntersectInterval(pieces.front(), ancestor);
			}

			NormalizeIntervals(pieces);
			range.pieces.swap(pieces);

			for (size_t i = 0u; i < range.pieces.size(); ++i)
			{
				unionOfPieces.push_back(range.pieces[i]);
			}
		}

		NormalizeIntervals(unionOfPieces);
		site.effective.swap(unionOfPieces);
	}

	for (size_t siteIndex = 0u; siteIndex < sites.size(); ++siteIndex)
	{
		const SiteData& site = sites[siteIndex];
		for (size_t rangeIndex = 0u; rangeIndex < site.ranges.size(); ++rangeIndex)
		{
			const SourceRange& range = site.ranges[rangeIndex];
			for (size_t i = 0u; i < range.pieces.size(); ++i)
			{
				if (range.pieces[i].begin < range.pieces[i].end)
				{
					sections[site.section].entries.push_back(
						CoverageEntry(range.pieces[i].begin, range.pieces[i].end, static_cast<uint32_t>(siteIndex)));
				}
			}
		}
	}

	for (auto it = sections.begin(); it != sections.end(); ++it)
	{
		it->second.Build();
	}
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
bool PDB::InlineSourceIndex::Impl::Build(const RawFile& file, uint32_t namesStreamIndex, const InlineSourceModule* modules, size_t moduleCount) PDB_NO_EXCEPT
{
	error = PDB::InlineSourceError();
	procedures.clear();
	sites.clear();
	sections.clear();

	if (moduleCount != 0u && modules == nullptr)
	{
		Fail(PDB::InlineSourceErrorCode::InvalidModuleSlice, InvalidIndex, InvalidIndex, 0u, "module array is null");
		return false;
	}

	if (namesStreamIndex >= file.GetStreamCount())
	{
		Fail(PDB::InlineSourceErrorCode::InvalidNamesStream, InvalidIndex, namesStreamIndex, 0u, "names stream index is outside the file");
		return false;
	}

	const CoalescedMSFStream namesStream = file.CreateMSFStream<CoalescedMSFStream>(namesStreamIndex);
	if (namesStream.GetSize() < NamesHeaderSize)
	{
		Fail(PDB::InlineSourceErrorCode::InvalidNamesStream, InvalidIndex, namesStreamIndex, 0u, "names stream is too small");
		return false;
	}

	const uint8_t* namesData = namesStream.GetDataAtOffset<uint8_t>(0u);
	if (ReadU32(namesData) != NamesMagic)
	{
		Fail(PDB::InlineSourceErrorCode::InvalidNamesStream, InvalidIndex, namesStreamIndex, 0u, "names stream signature mismatch");
		return false;
	}

	const uint32_t stringTableSize = ReadU32(namesData + 8u);
	if (static_cast<uint64_t>(NamesHeaderSize) + stringTableSize > namesStream.GetSize())
	{
		Fail(PDB::InlineSourceErrorCode::InvalidNamesStream, InvalidIndex, namesStreamIndex, 8u, "names string table overruns the stream");
		return false;
	}

	const NamesView names(namesData + NamesHeaderSize, stringTableSize);

	for (size_t i = 0u; i < moduleCount; ++i)
	{
		const InlineSourceModule& slice = modules[i];

		if (slice.symbolBytes < 4u)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidModuleSlice, slice.moduleIndex, slice.streamIndex, 0u, "module symbol slice is too small");
			return false;
		}

		if (slice.streamIndex >= file.GetStreamCount())
		{
			Fail(PDB::InlineSourceErrorCode::InvalidStream, slice.moduleIndex, slice.streamIndex, 0u, "module stream index is outside the file");
			return false;
		}

		const uint64_t c13Base64 = static_cast<uint64_t>(slice.symbolBytes) + slice.c11Bytes;
		const uint64_t c13End64 = c13Base64 + slice.c13Bytes;
		if (c13End64 > file.GetStreamSize(slice.streamIndex) || c13End64 > MaxCoordinate)
		{
			Fail(PDB::InlineSourceErrorCode::InvalidStream, slice.moduleIndex, slice.streamIndex, static_cast<uint32_t>(std::min<uint64_t>(c13Base64, MaxCoordinate)), "module slice extends past the stream");
			return false;
		}

		const CoalescedMSFStream stream = file.CreateMSFStream<CoalescedMSFStream>(slice.streamIndex, static_cast<uint32_t>(c13End64));
		const uint8_t* streamData = stream.GetDataAtOffset<uint8_t>(0u);

		ModuleParse module;
		module.moduleIndex = slice.moduleIndex;
		module.streamIndex = slice.streamIndex;
		module.symbolBytes = slice.symbolBytes;
		module.c11Bytes = slice.c11Bytes;
		module.c13Base = static_cast<uint32_t>(c13Base64);
		module.symbols.assign(streamData, streamData + slice.symbolBytes);
		if (slice.c13Bytes != 0u)
		{
			module.c13.assign(streamData + module.c13Base, streamData + module.c13Base + slice.c13Bytes);
		}

		ParseC13(module, names);
		if (error.IsError())
		{
			return false;
		}

		ParseSymbols(module);
		if (error.IsError())
		{
			return false;
		}
	}

	BuildCoverage();
	if (error.IsError())
	{
		return false;
	}

	return true;
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::InlineSourceError::InlineSourceError(void) PDB_NO_EXCEPT
	: code(InlineSourceErrorCode::None)
	, moduleIndex(InvalidIndex)
	, streamIndex(InvalidIndex)
	, offset(InvalidIndex)
	, description()
{
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::InlineSourceFrame::InlineSourceFrame(void) PDB_NO_EXCEPT
	: moduleIndex(0u)
	, streamIndex(0u)
	, section(0u)
	, symbolOffset(0u)
	, inlinee(0u)
	, codeOffsetBegin(0u)
	, codeOffsetEnd(0u)
	, annotationBegin(0u)
	, annotationEnd(0u)
	, recordOffset(0u)
	, fileChecksumOffset(0u)
	, lineNumber(0u)
	, lineSpan(1u)
	, columnStart(0u)
	, columnEnd(0u)
	, hasColumnStart(false)
	, hasColumnEnd(false)
	, isStatement(true)
	, file()
{
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::InlineSourceProcedure::InlineSourceProcedure(void) PDB_NO_EXCEPT
	: moduleIndex(0u)
	, streamIndex(0u)
	, section(0u)
	, symbolOffset(0u)
	, codeOffset(0u)
	, codeSize(0u)
{
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::InlineSourceChain::InlineSourceChain(void) PDB_NO_EXCEPT
	: procedure()
	, frames()
{
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::InlineSourceIndex::InlineSourceIndex(void) PDB_NO_EXCEPT
	: m_impl(new Impl())
	, m_error()
	, m_valid(false)
{
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::InlineSourceIndex::InlineSourceIndex(InlineSourceIndex&& other) PDB_NO_EXCEPT
	: m_impl(std::move(other.m_impl))
	, m_error(other.m_error)
	, m_valid(other.m_valid)
{
	other.m_valid = false;
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::InlineSourceIndex& PDB::InlineSourceIndex::operator=(InlineSourceIndex&& other) PDB_NO_EXCEPT
{
	if (this != &other)
	{
		m_impl = std::move(other.m_impl);
		m_error = other.m_error;
		m_valid = other.m_valid;
		other.m_valid = false;
	}

	return *this;
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::InlineSourceIndex::~InlineSourceIndex(void) PDB_NO_EXCEPT
{
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
bool PDB::InlineSourceIndex::Build(const RawFile& file, uint32_t namesStreamIndex, const InlineSourceModule* modules, size_t moduleCount) PDB_NO_EXCEPT
{
	m_valid = false;

	if (!m_impl)
	{
		m_impl.reset(new Impl());
	}

	m_valid = m_impl->Build(file, namesStreamIndex, modules, moduleCount);
	m_error = m_impl->error;
	return m_valid;
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
bool PDB::InlineSourceIndex::Build(const RawFile& file, uint32_t namesStreamIndex, const std::vector<InlineSourceModule>& modules) PDB_NO_EXCEPT
{
	return Build(file, namesStreamIndex, modules.empty() ? nullptr : &modules[0], modules.size());
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
std::vector<PDB::InlineSourceChain> PDB::InlineSourceIndex::Query(uint16_t section, uint32_t codeOffset) const PDB_NO_EXCEPT
{
	std::vector<InlineSourceChain> result;
	if (!m_valid || !m_impl)
	{
		return result;
	}

	const auto sectionIt = m_impl->sections.find(section);
	if (sectionIt == m_impl->sections.end())
	{
		return result;
	}

	const SectionIndex& index = sectionIt->second;
	std::vector<uint32_t> hits;
	index.Query(codeOffset, hits);
	if (hits.empty())
	{
		return result;
	}

	// group hits by site; a site can contribute several pieces if an ancestor hole split it
	std::vector<uint32_t> coveringSites;
	for (size_t i = 0u; i < hits.size(); ++i)
	{
		const uint32_t siteIndex = index.entries[hits[i]].siteIndex;
		if (std::find(coveringSites.begin(), coveringSites.end(), siteIndex) == coveringSites.end())
		{
			coveringSites.push_back(siteIndex);
		}
	}

	// only innermost covering sites start a chain.  Effective ranges are already intersected with
	// every ancestor, so a covering descendant implies covering ancestors.
	std::vector<uint32_t> leaves;
	for (size_t i = 0u; i < coveringSites.size(); ++i)
	{
		const SiteData& site = m_impl->sites[coveringSites[i]];

		bool hasCoveringChild = false;
		for (size_t c = 0u; c < site.children.size(); ++c)
		{
			if (Covers(m_impl->sites[site.children[c]].effective, codeOffset))
			{
				hasCoveringChild = true;
				break;
			}
		}

		if (!hasCoveringChild)
		{
			leaves.push_back(coveringSites[i]);
		}
	}

	for (size_t i = 0u; i < leaves.size(); ++i)
	{
		std::vector<uint32_t> chainSites;
		for (uint32_t siteIndex = leaves[i]; siteIndex != InvalidIndex; siteIndex = m_impl->sites[siteIndex].parentSite)
		{
			chainSites.push_back(siteIndex);
		}

		std::reverse(chainSites.begin(), chainSites.end());

		InlineSourceChain chain;
		const SiteData& leafSite = m_impl->sites[leaves[i]];
		const ProcedureData& procedure = m_impl->procedures[leafSite.procedureIndex];

		chain.procedure.moduleIndex = procedure.moduleIndex;
		chain.procedure.streamIndex = procedure.streamIndex;
		chain.procedure.section = procedure.section;
		chain.procedure.symbolOffset = procedure.symbolOffset;
		chain.procedure.codeOffset = procedure.codeOffset;
		chain.procedure.codeSize = procedure.codeSize;

		bool valid = true;
		for (size_t c = 0u; c < chainSites.size(); ++c)
		{
			const SiteData& site = m_impl->sites[chainSites[c]];

			const SourceRange* chosen = nullptr;
			const Interval* chosenPiece = nullptr;
			for (size_t r = 0u; r < site.ranges.size() && chosen == nullptr; ++r)
			{
				const SourceRange& range = site.ranges[r];
				for (size_t p = 0u; p < range.pieces.size(); ++p)
				{
					if (range.pieces[p].begin <= codeOffset && codeOffset < range.pieces[p].end)
					{
						chosen = &range;
						chosenPiece = &range.pieces[p];
						break;
					}
				}
			}

			if (chosen == nullptr)
			{
				valid = false;
				break;
			}

			InlineSourceFrame frame;
			frame.moduleIndex = site.moduleIndex;
			frame.streamIndex = site.streamIndex;
			frame.section = site.section;
			frame.symbolOffset = site.symbolOffset;
			frame.inlinee = site.inlinee;
			frame.codeOffsetBegin = chosenPiece->begin;
			frame.codeOffsetEnd = chosenPiece->end;
			frame.annotationBegin = site.annotationBegin;
			frame.annotationEnd = site.annotationEnd;
			frame.recordOffset = chosen->recordOffset;
			frame.fileChecksumOffset = chosen->fileChecksumOffset;
			frame.lineNumber = chosen->lineNumber;
			frame.lineSpan = chosen->lineSpan;
			frame.columnStart = static_cast<uint16_t>(chosen->columnStart);
			frame.columnEnd = static_cast<uint16_t>(chosen->columnEnd);
			frame.hasColumnStart = chosen->hasColumnStart;
			frame.hasColumnEnd = chosen->hasColumnEnd;
			frame.isStatement = chosen->isStatement;
			frame.file = chosen->file;

			chain.frames.push_back(frame);
		}

		if (valid)
		{
			result.push_back(chain);
		}
	}

	std::stable_sort(result.begin(), result.end(), [](const InlineSourceChain& lhs, const InlineSourceChain& rhs)
	{
		if (lhs.procedure.moduleIndex != rhs.procedure.moduleIndex)
		{
			return lhs.procedure.moduleIndex < rhs.procedure.moduleIndex;
		}

		if (lhs.procedure.symbolOffset != rhs.procedure.symbolOffset)
		{
			return lhs.procedure.symbolOffset < rhs.procedure.symbolOffset;
		}

		// lexicographic order over the site record offsets of the chain
		const size_t count = std::min(lhs.frames.size(), rhs.frames.size());
		for (size_t i = 0u; i < count; ++i)
		{
			if (lhs.frames[i].symbolOffset != rhs.frames[i].symbolOffset)
			{
				return lhs.frames[i].symbolOffset < rhs.frames[i].symbolOffset;
			}
		}

		return lhs.frames.size() < rhs.frames.size();
	});

	return result;
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
const PDB::InlineSourceError& PDB::InlineSourceIndex::GetError(void) const PDB_NO_EXCEPT
{
	return m_error;
}
