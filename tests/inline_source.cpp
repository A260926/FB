// Tests for the offline inline source index.
//
// All inputs are synthetic module streams built from bytes described by task_support/FORMAT.md.
// Expected source positions and code ranges are computed from the construction below, never from
// the reader's own output.

#include "task_support/check.h"
#include "task_support/fixture.h"
#include "PDB.h"
#include "PDB_RawFile.h"
#include "PDB_InlineSource.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>


namespace
{
	using fixture::Bytes;
	using fixture::u16;
	using fixture::u32;
	using fixture::put32;
	using fixture::align4;
	using fixture::msf;
	using fixture::record;
	using fixture::subsection;

	// CodeView symbol record kinds used by the fixtures.
	const uint16_t S_END =				0x0006u;
	const uint16_t S_BLOCK32 =			0x1103u;
	const uint16_t S_LPROC32 =			0x110Fu;
	const uint16_t S_GPROC32 =			0x1110u;
	const uint16_t S_SEPCODE =			0x1132u;
	const uint16_t S_GPROC32_ID =		0x1147u;
	const uint16_t S_INLINESITE =		0x114Du;
	const uint16_t S_INLINESITE_END =	0x114Eu;
	const uint16_t S_PROC_ID_END =		0x114Fu;
	const uint16_t S_INLINESITE2 =		0x115Du;

	// Append a compressed unsigned annotation integer.
	void cvU(Bytes& out, uint32_t value)
	{
		if (value < 0x80u)
		{
			out.push_back(static_cast<unsigned char>(value));
		}
		else if (value < 0x4000u)
		{
			out.push_back(static_cast<unsigned char>(0x80u | (value >> 8u)));
			out.push_back(static_cast<unsigned char>(value & 0xFFu));
		}
		else
		{
			out.push_back(static_cast<unsigned char>(0xC0u | (value >> 24u)));
			out.push_back(static_cast<unsigned char>((value >> 16u) & 0xFFu));
			out.push_back(static_cast<unsigned char>((value >> 8u) & 0xFFu));
			out.push_back(static_cast<unsigned char>(value & 0xFFu));
		}
	}

	// Append a compressed signed annotation integer (low bit is the sign).
	void cvS(Bytes& out, int32_t value)
	{
		const uint32_t magnitude = (value < 0) ? static_cast<uint32_t>(-value) : static_cast<uint32_t>(value);
		cvU(out, (magnitude << 1u) | ((value < 0) ? 1u : 0u));
	}

	// Builds a module symbol stream.  Records are appended in stream order and parent/end links
	// are patched once the target offsets are known.
	struct SymStream
	{
		Bytes bytes;

		SymStream(void)
		{
			u32(bytes, 0u);		// module signature
		}

		size_t AddProc(uint16_t kind, uint32_t codeOffset, uint16_t section, uint32_t codeSize, const char* name)
		{
			Bytes data;
			u32(data, 0u);			// parent
			u32(data, 0u);			// end, patched later
			u32(data, 0u);			// next
			u32(data, codeSize);
			u32(data, 0u);			// debugStart
			u32(data, 0u);			// debugEnd
			u32(data, 0u);			// typeIndex
			u32(data, codeOffset);
			u16(data, section);
			data.push_back(0u);		// flags
			data.insert(data.end(), name, name + std::strlen(name) + 1u);
			return record(bytes, kind, data);
		}

		size_t AddBlock(size_t parent, uint32_t codeOffset, uint16_t section, uint32_t codeSize, const char* name)
		{
			Bytes data;
			u32(data, static_cast<uint32_t>(parent));
			u32(data, 0u);			// end, patched later
			u32(data, codeSize);
			u32(data, codeOffset);
			u16(data, section);
			data.insert(data.end(), name, name + std::strlen(name) + 1u);
			return record(bytes, S_BLOCK32, data);
		}

		size_t AddSite(size_t parent, uint32_t inlinee, const Bytes& annotations)
		{
			Bytes data;
			u32(data, static_cast<uint32_t>(parent));
			u32(data, 0u);			// end, patched later
			u32(data, inlinee);
			data.insert(data.end(), annotations.begin(), annotations.end());
			return record(bytes, S_INLINESITE, data);
		}

		size_t AddRecord(uint16_t kind)
		{
			return record(bytes, kind, Bytes());
		}

		void PatchEnd(size_t recordOffset, size_t endOffset)
		{
			put32(bytes, recordOffset + 8u, static_cast<uint32_t>(endOffset));
		}
	};

	struct InlineeDef
	{
		uint32_t inlinee;
		uint32_t fileChecksumOffset;
		uint32_t lineNumber;
		std::vector<uint32_t> extraFiles;
	};

	// Builds the C13 subsections of a module.  File checksum offsets are relative to the checksum
	// subsection data, matching the inlinee source line references.
	struct C13Stream
	{
		Bytes bytes;

		std::vector<uint32_t> AddChecksums(const std::vector<uint32_t>& filenameOffsets)
		{
			Bytes data;
			std::vector<uint32_t> offsets;
			for (size_t i = 0u; i < filenameOffsets.size(); ++i)
			{
				offsets.push_back(static_cast<uint32_t>(data.size()));
				u32(data, filenameOffsets[i]);
				data.push_back(0u);		// checksum size
				data.push_back(0u);		// checksum kind
				align4(data);
			}

			subsection(bytes, 0xF4u, data);
			return offsets;
		}

		void AddInlineeLines(const std::vector<InlineeDef>& definitions, uint32_t signature)
		{
			Bytes data;
			u32(data, signature);
			for (size_t i = 0u; i < definitions.size(); ++i)
			{
				const InlineeDef& definition = definitions[i];
				u32(data, definition.inlinee);
				u32(data, definition.fileChecksumOffset);
				u32(data, definition.lineNumber);
				if (signature == 1u)
				{
					u32(data, static_cast<uint32_t>(definition.extraFiles.size()));
					for (size_t j = 0u; j < definition.extraFiles.size(); ++j)
					{
						u32(data, definition.extraFiles[j]);
					}
				}

				align4(data);
			}

			subsection(bytes, 0xF6u, data);
		}
	};

	struct NamesStream
	{
		Bytes table;

		NamesStream(void)
		{
			table.push_back(0u);		// offset 0 is the empty string
		}

		uint32_t Add(const char* name)
		{
			const uint32_t offset = static_cast<uint32_t>(table.size());
			table.insert(table.end(), name, name + std::strlen(name) + 1u);
			return offset;
		}

		Bytes Build(void) const
		{
			Bytes data;
			u32(data, 0xEFFEEFFEu);
			u32(data, 1u);
			u32(data, static_cast<uint32_t>(table.size()));
			data.insert(data.end(), table.begin(), table.end());
			return data;
		}
	};

	struct ModuleBytes
	{
		Bytes stream;
		uint32_t symbolBytes;
	};

	// Concatenates signature+symbols, optional C11 padding and the C13 data.
	ModuleBytes FinishModule(const SymStream& symbols, const C13Stream& c13, uint32_t c11Bytes = 0u)
	{
		ModuleBytes module;
		module.symbolBytes = static_cast<uint32_t>(symbols.bytes.size());
		module.stream = symbols.bytes;
		module.stream.insert(module.stream.end(), c11Bytes, 0u);
		module.stream.insert(module.stream.end(), c13.bytes.begin(), c13.bytes.end());
		return module;
	}

	PDB::InlineSourceModule Slice(uint16_t moduleIndex, uint16_t streamIndex, const ModuleBytes& module, uint32_t c11Bytes, uint32_t c13Bytes)
	{
		return PDB::InlineSourceModule(moduleIndex, streamIndex, module.symbolBytes, c11Bytes, c13Bytes);
	}

	bool BuildIndex(PDB::InlineSourceIndex& index, const Bytes& image, uint32_t namesStreamIndex, const std::vector<PDB::InlineSourceModule>& modules)
	{
		const PDB::RawFile raw = PDB::CreateRawFile(image.data());
		return index.Build(raw, namesStreamIndex, modules);
	}

}


// ------------------------------------------------------------------------------------------------
// annotations: compressed state machine, combined opcodes, file switch, columns, holes
// ------------------------------------------------------------------------------------------------
static int annotations_combined_ops(void)
{
	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x100u, "f");

	// 0x48 = line delta +2 (signed encoding 4) in the high bits, code delta 8 in the low nibble.
	Bytes annotations;
	cvU(annotations, 11u);
	cvU(annotations, (4u << 4u) | 8u);	// code 8, line 12 -> [8,16)
	cvU(annotations, 7u);
	cvU(annotations, 4u);		// line span 4, affecting only later records
	cvU(annotations, 3u);
	cvU(annotations, 8u);		// code 16, emit record B, sealing A
	cvU(annotations, 5u);		// change file to checksum offset 8
	cvU(annotations, 8u);
	cvU(annotations, 9u);		// column start 5
	cvU(annotations, 5u);
	cvU(annotations, 13u);		// column end 9
	cvU(annotations, 9u);
	cvU(annotations, 8u);		// expression
	cvU(annotations, 0u);
	cvU(annotations, 3u);
	cvU(annotations, 8u);		// code 24, emit record C, sealing B
	cvU(annotations, 12u);		// explicit length 8, code delta 8 -> [32,40)
	cvU(annotations, 8u);
	cvU(annotations, 8u);
	cvU(annotations, 0u);		// terminator

	const size_t site = symbols.AddSite(proc, 0x77u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	NamesStream names;
	const uint32_t nameA = names.Add("a.cpp");
	const uint32_t nameB = names.Add("b.cpp");
	const std::vector<uint32_t> checksums = c13.AddChecksums({nameA, nameB});
	TASK_CHECK(checksums.size() == 2u && checksums[0] == 0u && checksums[1] == 8u);

	InlineeDef definition;
	definition.inlinee = 0x77u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 10u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(3u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));
	TASK_CHECK(index.IsValid());

	// first record: [8,16) procedure-relative, file a.cpp, line 12, span 1, statement, no columns
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x108u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 1u);
		TASK_CHECK(chains[0].procedure.section == 1u);
		TASK_CHECK(chains[0].procedure.codeOffset == 0x100u);
		TASK_CHECK(chains[0].procedure.symbolOffset == proc);
		const PDB::InlineSourceFrame& frame = chains[0].frames[0];
		TASK_CHECK(frame.moduleIndex == 3u);
		TASK_CHECK(frame.streamIndex == 0u);
		TASK_CHECK(frame.symbolOffset == site);
		TASK_CHECK(frame.inlinee == 0x77u);
		TASK_CHECK(frame.codeOffsetBegin == 0x108u);
		TASK_CHECK(frame.codeOffsetEnd == 0x110u);
		TASK_CHECK(frame.file == "a.cpp");
		TASK_CHECK(frame.fileChecksumOffset == checksums[0]);
		TASK_CHECK(frame.lineNumber == 12u);
		TASK_CHECK(frame.lineSpan == 1u);
		TASK_CHECK(frame.GetLastLine() == 12u);
		TASK_CHECK(!frame.hasColumnStart);
		TASK_CHECK(!frame.hasColumnEnd);
		TASK_CHECK(frame.isStatement);
		TASK_CHECK(frame.annotationBegin >= module.symbolBytes ? false : true);
		TASK_CHECK(frame.recordOffset >= frame.annotationBegin && frame.recordOffset < frame.annotationEnd);
	}

	// second record: [16,24), emitted before the file/column changes, so it keeps file a.cpp but
	// picks up the line span set after the first record
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x110u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 1u);
		const PDB::InlineSourceFrame& frame = chains[0].frames[0];
		TASK_CHECK(frame.codeOffsetBegin == 0x110u);
		TASK_CHECK(frame.codeOffsetEnd == 0x118u);
		TASK_CHECK(frame.file == "a.cpp");
		TASK_CHECK(frame.lineNumber == 12u);
		TASK_CHECK(frame.lineSpan == 4u);
		TASK_CHECK(frame.GetLastLine() == 15u);
		TASK_CHECK(!frame.hasColumnStart);
		TASK_CHECK(frame.isStatement);
	}

	// third record: [24,32) is emitted after the state changes and inherits all of them
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x118u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 1u);
		const PDB::InlineSourceFrame& frame = chains[0].frames[0];
		TASK_CHECK(frame.codeOffsetBegin == 0x118u);
		TASK_CHECK(frame.codeOffsetEnd == 0x120u);
		TASK_CHECK(frame.file == "b.cpp");
		TASK_CHECK(frame.fileChecksumOffset == checksums[1]);
		TASK_CHECK(frame.lineNumber == 12u);
		TASK_CHECK(frame.lineSpan == 4u);
		TASK_CHECK(frame.GetLastLine() == 15u);
		TASK_CHECK(frame.hasColumnStart && frame.columnStart == 5u);
		TASK_CHECK(frame.hasColumnEnd && frame.columnEnd == 9u);
		TASK_CHECK(!frame.isStatement);
	}

	// fourth record: explicit [32,40), still using the sticky state
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x120u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 1u);
		const PDB::InlineSourceFrame& frame = chains[0].frames[0];
		TASK_CHECK(frame.codeOffsetBegin == 0x120u);
		TASK_CHECK(frame.codeOffsetEnd == 0x128u);
		TASK_CHECK(frame.file == "b.cpp");
		TASK_CHECK(frame.lineSpan == 4u);
	}

	// the right edge is half-open
	TASK_CHECK(index.Query(1u, 0x128u).empty());
	// nothing before the first record's start
	TASK_CHECK(index.Query(1u, 0x107u).empty());

	return 0;
}

static int annotations_signed_and_compressed(void)
{
	SymStream symbols;
	const size_t proc = symbols.AddProc(S_GPROC32_ID, 0x100u, 1u, 0x100u, "g");

	Bytes annotations;
	cvU(annotations, 3u);
	cvU(annotations, 10u);		// code 10, emit line 100
	cvU(annotations, 6u);
	cvS(annotations, -3);		// line 97
	cvU(annotations, 3u);
	cvU(annotations, 10u);		// code 20, emit line 97
	// 0xD5 = line delta -6 (signed encoding (6 << 1) | 1 = 13) in the high bits, code delta 5 low
	cvU(annotations, 11u);
	cvU(annotations, (13u << 4u) | 5u);
	cvU(annotations, 4u);		// explicit length 5
	cvU(annotations, 5u);
	cvU(annotations, 0u);

	const size_t site = symbols.AddSite(proc, 0x11u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_PROC_ID_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	NamesStream names;
	const uint32_t name = names.Add("signed.cpp");
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});

	InlineeDef definition;
	definition.inlinee = 0x11u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 100u;
	c13.AddInlineeLines({definition}, 1u);		// extended signature

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(1u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x10Fu);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 1u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x10Au);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x114u);
		TASK_CHECK(chains[0].frames[0].lineNumber == 100u);
	}
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x114u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x114u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x119u);
		TASK_CHECK(chains[0].frames[0].lineNumber == 97u);
	}
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x119u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x119u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x11Eu);
		TASK_CHECK(chains[0].frames[0].lineNumber == 91u);
	}
	TASK_CHECK(index.Query(1u, 0x11Eu).empty());

	return 0;
}

static int annotations_holes_and_tail(void)
{
	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x100u, "h");

	Bytes annotations;
	cvU(annotations, 12u);		// length 4 at offset 0
	cvU(annotations, 4u);
	cvU(annotations, 0u);
	cvU(annotations, 3u);		// advance to 10
	cvU(annotations, 6u);
	cvU(annotations, 4u);		// explicit length 2 -> [10,12)
	cvU(annotations, 2u);
	cvU(annotations, 3u);		// advance to 20, emit a tail record
	cvU(annotations, 8u);
	cvU(annotations, 0u);		// terminator; the tail has no verifiable right edge

	const size_t site = symbols.AddSite(proc, 0x22u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	NamesStream names;
	const uint32_t name = names.Add("holes.cpp");
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});

	InlineeDef definition;
	definition.inlinee = 0x22u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 1u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(2u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x101u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x100u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x104u);
	}
	// the hole [4,10) is preserved, not stretched to the next record
	TASK_CHECK(index.Query(1u, 0x105u).empty());
	TASK_CHECK(index.Query(1u, 0x109u).empty());
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x10Bu);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x10Au);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x10Cu);
	}
	// the tail record has no known right edge and never matches
	TASK_CHECK(index.Query(1u, 0x114u).empty());
	TASK_CHECK(index.Query(1u, 0x180u).empty());

	return 0;
}

static int annotations_column_state(void)
{
	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x100u, "c");

	Bytes annotations;
	cvU(annotations, 9u);
	cvU(annotations, 3u);		// column start 3
	cvU(annotations, 13u);
	cvU(annotations, 10u);		// column end 10
	cvU(annotations, 10u);
	cvS(annotations, -2);		// column end 8
	cvU(annotations, 3u);
	cvU(annotations, 4u);		// code 4, emit with 3..8
	cvU(annotations, 10u);
	cvS(annotations, -2);		// column end 6
	cvU(annotations, 3u);
	cvU(annotations, 4u);		// code 8, emit with 3..6
	cvU(annotations, 13u);
	cvU(annotations, 0u);		// clear the column end
	cvU(annotations, 10u);
	cvS(annotations, -5);		// ignored: no established column end
	cvU(annotations, 3u);
	cvU(annotations, 4u);		// code 12, emit
	cvU(annotations, 4u);
	cvU(annotations, 4u);		// explicit length 4
	cvU(annotations, 9u);
	cvU(annotations, 0u);		// column start 0 means unspecified
	cvU(annotations, 3u);
	cvU(annotations, 4u);		// code 20, emit
	cvU(annotations, 4u);
	cvU(annotations, 4u);		// explicit length 4
	cvU(annotations, 0u);

	const size_t site = symbols.AddSite(proc, 0x33u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	NamesStream names;
	const uint32_t name = names.Add("cols.cpp");
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});

	InlineeDef definition;
	definition.inlinee = 0x33u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 5u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(4u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x105u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x104u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x108u);
		TASK_CHECK(chains[0].frames[0].hasColumnStart && chains[0].frames[0].columnStart == 3u);
		TASK_CHECK(chains[0].frames[0].hasColumnEnd && chains[0].frames[0].columnEnd == 8u);
	}
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x109u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames[0].hasColumnEnd && chains[0].frames[0].columnEnd == 6u);
	}
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x10Du);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames[0].hasColumnStart && chains[0].frames[0].columnStart == 3u);
		TASK_CHECK(!chains[0].frames[0].hasColumnEnd);
	}
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x115u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(!chains[0].frames[0].hasColumnStart);
		TASK_CHECK(!chains[0].frames[0].hasColumnEnd);
	}

	return 0;
}

static int annotations_large_offsets(void)
{
	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x1000u, 5u, 0x8000u, "big");

	Bytes annotations;
	cvU(annotations, 1u);
	cvU(annotations, 0x5000u);	// needs the four-byte compressed form
	cvU(annotations, 12u);
	cvU(annotations, 0x10u);
	cvU(annotations, 0u);
	cvU(annotations, 0u);

	const size_t site = symbols.AddSite(proc, 0x44u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	NamesStream names;
	const uint32_t name = names.Add("big.cpp");
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});

	InlineeDef definition;
	definition.inlinee = 0x44u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 1u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(5u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	const std::vector<PDB::InlineSourceChain> chains = index.Query(5u, 0x6005u);
	TASK_CHECK(chains.size() == 1u);
	TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x6000u);
	TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x6010u);
	TASK_CHECK(index.Query(5u, 0x6010u).empty());

	return 0;
}

static int annotations_truncated_operand(void)
{
	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x100u, "t");

	Bytes annotations;
	cvU(annotations, 6u);		// signed line delta without an operand
	const size_t site = symbols.AddSite(proc, 0x55u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	NamesStream names;
	const uint32_t name = names.Add("t.cpp");
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});

	InlineeDef definition;
	definition.inlinee = 0x55u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 1u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(!BuildIndex(index, image, 1u, {Slice(6u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));
	TASK_CHECK(!index.IsValid());
	const PDB::InlineSourceError& error = index.GetError();
	TASK_CHECK(error.code == PDB::InlineSourceErrorCode::TruncatedAnnotation);
	TASK_CHECK(error.moduleIndex == 6u);
	TASK_CHECK(error.streamIndex == 0u);
	// the missing operand starts one byte into the annotation blob
	TASK_CHECK(error.offset >= module.symbolBytes ? false : true);

	return 0;
}

static int annotations_backwards_offset(void)
{
	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x100u, "b");

	Bytes annotations;
	cvU(annotations, 1u);
	cvU(annotations, 20u);
	cvU(annotations, 3u);
	cvU(annotations, 0u);		// emit at 20
	cvU(annotations, 1u);
	cvU(annotations, 10u);
	cvU(annotations, 3u);
	cvU(annotations, 0u);		// emit at 10: moves backwards
	cvU(annotations, 0u);

	const size_t site = symbols.AddSite(proc, 0x66u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	NamesStream names;
	const uint32_t name = names.Add("b.cpp");
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});

	InlineeDef definition;
	definition.inlinee = 0x66u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 1u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(!BuildIndex(index, image, 1u, {Slice(7u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));
	TASK_CHECK(index.GetError().code == PDB::InlineSourceErrorCode::InvalidCodeRange);
	TASK_CHECK(index.GetError().moduleIndex == 7u);
	TASK_CHECK(index.GetError().streamIndex == 0u);

	return 0;
}

static int annotations_bad_scope_terminator(void)
{
	// a plain procedure closed by S_PROC_ID_END is a scope-link error
	NamesStream names;
	const uint32_t name = names.Add("badend.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x40u, "badend");
	Bytes annotations;
	cvU(annotations, 12u);
	cvU(annotations, 4u);
	cvU(annotations, 0u);
	cvU(annotations, 0u);
	const size_t site = symbols.AddSite(proc, 0x1234u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_PROC_ID_END);	// wrong for a non-_ID procedure
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});
	InlineeDef definition;
	definition.inlinee = 0x1234u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 1u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(!BuildIndex(index, image, 1u, {Slice(32u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));
	TASK_CHECK(index.GetError().code == PDB::InlineSourceErrorCode::InvalidScopeLink);
	TASK_CHECK(index.GetError().moduleIndex == 32u);
	TASK_CHECK(index.GetError().streamIndex == 0u);
	TASK_CHECK(index.GetError().offset == proc);

	return 0;
}

static int annotations_unsupported_records(void)
{
	// S_SEPCODE is rejected with an explicit diagnostic.
	{
		SymStream symbols;
		const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x100u, "s");
		const size_t sepcode = symbols.AddRecord(S_SEPCODE);
		const size_t site = symbols.AddSite(proc, 0x77u, Bytes());
		const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
		symbols.PatchEnd(site, siteEnd);
		const size_t procEnd = symbols.AddRecord(S_END);
		symbols.PatchEnd(proc, procEnd);

		const ModuleBytes module = FinishModule(symbols, C13Stream());
		NamesStream names;
		const Bytes image = msf({module.stream, names.Build()});
		PDB::InlineSourceIndex index;
		TASK_CHECK(!BuildIndex(index, image, 1u, {Slice(8u, 0u, module, 0u, 0u)}));
		TASK_CHECK(index.GetError().code == PDB::InlineSourceErrorCode::UnsupportedSeparateCodeBlock);
		TASK_CHECK(index.GetError().moduleIndex == 8u);
		TASK_CHECK(index.GetError().streamIndex == 0u);
		TASK_CHECK(index.GetError().offset == sepcode);
	}

	// S_INLINESITE2 is rejected with an explicit diagnostic.
	{
		SymStream symbols;
		const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x100u, "i2");
		const size_t site2 = symbols.AddRecord(S_INLINESITE2);
		const size_t site = symbols.AddSite(proc, 0x77u, Bytes());
		const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
		symbols.PatchEnd(site, siteEnd);
		const size_t procEnd = symbols.AddRecord(S_END);
		symbols.PatchEnd(proc, procEnd);

		const ModuleBytes module = FinishModule(symbols, C13Stream());
		NamesStream names;
		const Bytes image = msf({module.stream, names.Build()});
		PDB::InlineSourceIndex index;
		TASK_CHECK(!BuildIndex(index, image, 1u, {Slice(9u, 0u, module, 0u, 0u)}));
		TASK_CHECK(index.GetError().code == PDB::InlineSourceErrorCode::UnsupportedInlineSite2);
		TASK_CHECK(index.GetError().offset == site2);
	}

	return 0;
}

static int annotations_range_outside_procedure(void)
{
	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x20u, "o");

	Bytes annotations;
	cvU(annotations, 1u);
	cvU(annotations, 0u);
	cvU(annotations, 12u);
	cvU(annotations, 0x40u);	// length 64 in a 32-byte procedure
	cvU(annotations, 0u);
	cvU(annotations, 0u);

	const size_t site = symbols.AddSite(proc, 0x88u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	NamesStream names;
	const uint32_t name = names.Add("o.cpp");
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});

	InlineeDef definition;
	definition.inlinee = 0x88u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 1u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(!BuildIndex(index, image, 1u, {Slice(10u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));
	TASK_CHECK(index.GetError().code == PDB::InlineSourceErrorCode::InvalidCodeRange);
	TASK_CHECK(index.GetError().moduleIndex == 10u);
	TASK_CHECK(index.GetError().streamIndex == 0u);

	return 0;
}


// ------------------------------------------------------------------------------------------------
// module_identity: per-module inlinee/file namespaces and module-local file resolution
// ------------------------------------------------------------------------------------------------
static int module_identity_same_ids(void)
{
	NamesStream names;
	const uint32_t nameA = names.Add("module5.cpp");
	const uint32_t nameB = names.Add("module9.cpp");

	// module 5: S_GPROC32 inside a transparent block, same inlinee id and checksum offset as module 9
	SymStream symbolsA;
	const size_t procA = symbolsA.AddProc(S_GPROC32, 0x10u, 2u, 0x40u, "procA");
	const size_t blockA = symbolsA.AddBlock(procA, 0x0u, 2u, 0x40u, "blockA");
	Bytes annotationsA;
	cvU(annotationsA, 12u);
	cvU(annotationsA, 0x10u);
	cvU(annotationsA, 0u);
	cvU(annotationsA, 0u);
	const size_t siteA = symbolsA.AddSite(blockA, 0x2000u, annotationsA);
	const size_t siteAEnd = symbolsA.AddRecord(S_INLINESITE_END);
	symbolsA.PatchEnd(siteA, siteAEnd);
	const size_t blockAEnd = symbolsA.AddRecord(S_END);
	symbolsA.PatchEnd(blockA, blockAEnd);
	const size_t procAEnd = symbolsA.AddRecord(S_END);
	symbolsA.PatchEnd(procA, procAEnd);

	C13Stream c13A;
	const std::vector<uint32_t> checksumsA = c13A.AddChecksums({nameA});
	InlineeDef defA;
	defA.inlinee = 0x2000u;
	defA.fileChecksumOffset = checksumsA[0];
	defA.lineNumber = 40u;
	c13A.AddInlineeLines({defA}, 0u);
	const ModuleBytes moduleA = FinishModule(symbolsA, c13A);

	// module 9: _ID procedure, same ids/offsets but a different file and line
	SymStream symbolsB;
	const size_t procB = symbolsB.AddProc(S_GPROC32_ID, 0x10u, 2u, 0x40u, "procB");
	Bytes annotationsB;
	cvU(annotationsB, 12u);
	cvU(annotationsB, 0x10u);
	cvU(annotationsB, 0u);
	cvU(annotationsB, 0u);
	const size_t siteB = symbolsB.AddSite(procB, 0x2000u, annotationsB);
	const size_t siteBEnd = symbolsB.AddRecord(S_INLINESITE_END);
	symbolsB.PatchEnd(siteB, siteBEnd);
	const size_t procBEnd = symbolsB.AddRecord(S_PROC_ID_END);
	symbolsB.PatchEnd(procB, procBEnd);

	C13Stream c13B;
	const std::vector<uint32_t> checksumsB = c13B.AddChecksums({nameB});
	InlineeDef defB;
	defB.inlinee = 0x2000u;
	defB.fileChecksumOffset = checksumsB[0];
	defB.lineNumber = 70u;
	c13B.AddInlineeLines({defB}, 0u);
	const ModuleBytes moduleB = FinishModule(symbolsB, c13B);

	const Bytes image = msf({moduleA.stream, moduleB.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 2u, {
		Slice(5u, 0u, moduleA, 0u, static_cast<uint32_t>(c13A.bytes.size())),
		Slice(9u, 1u, moduleB, 0u, static_cast<uint32_t>(c13B.bytes.size()))
	}));

	const std::vector<PDB::InlineSourceChain> chains = index.Query(2u, 0x14u);
	TASK_CHECK(chains.size() == 2u);
	TASK_CHECK(chains[0].procedure.moduleIndex == 5u);
	TASK_CHECK(chains[1].procedure.moduleIndex == 9u);
	TASK_CHECK(chains[0].frames.size() == 1u);
	TASK_CHECK(chains[1].frames.size() == 1u);
	TASK_CHECK(chains[0].frames[0].inlinee == 0x2000u);
	TASK_CHECK(chains[1].frames[0].inlinee == 0x2000u);
	TASK_CHECK(chains[0].frames[0].fileChecksumOffset == 0u);
	TASK_CHECK(chains[1].frames[0].fileChecksumOffset == 0u);
	TASK_CHECK(chains[0].frames[0].file == "module5.cpp");
	TASK_CHECK(chains[1].frames[0].file == "module9.cpp");
	TASK_CHECK(chains[0].frames[0].lineNumber == 40u);
	TASK_CHECK(chains[1].frames[0].lineNumber == 70u);
	TASK_CHECK(chains[0].frames[0].symbolOffset == siteA);
	TASK_CHECK(chains[1].frames[0].symbolOffset == siteB);
	TASK_CHECK(chains[0].procedure.symbolOffset == procA);
	TASK_CHECK(chains[1].procedure.symbolOffset == procB);

	// module-local streams: a query only yields frames of their own module
	for (size_t i = 0u; i < chains.size(); ++i)
	{
		TASK_CHECK(chains[i].frames[0].moduleIndex == chains[i].procedure.moduleIndex);
		TASK_CHECK(chains[i].frames[0].streamIndex == chains[i].procedure.streamIndex);
	}

	// the module 5 site is inside the block, the module 9 one is not; both remain top level
	TASK_CHECK(chains[0].frames.size() == 1u);
	TASK_CHECK(chains[1].frames.size() == 1u);

	return 0;
}

static int module_identity_duplicate_inlinee(void)
{
	NamesStream names;
	const uint32_t name = names.Add("dup.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x100u, "d");
	Bytes annotations;
	cvU(annotations, 12u);
	cvU(annotations, 4u);
	cvU(annotations, 0u);
	cvU(annotations, 0u);
	const size_t site = symbols.AddSite(proc, 0x99u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});
	InlineeDef first;
	first.inlinee = 0x99u;
	first.fileChecksumOffset = checksums[0];
	first.lineNumber = 10u;
	InlineeDef second = first;
	second.lineNumber = 20u;
	c13.AddInlineeLines({first, second}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(!BuildIndex(index, image, 1u, {Slice(11u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));
	TASK_CHECK(index.GetError().code == PDB::InlineSourceErrorCode::DuplicateInlineeDefinition);
	TASK_CHECK(index.GetError().moduleIndex == 11u);
	TASK_CHECK(index.GetError().streamIndex == 0u);
	// the conflicting record lives in the C13 part of the module stream
	TASK_CHECK(index.GetError().offset >= module.symbolBytes);

	// identical duplicates are not contradictory and are accepted
	C13Stream c13b;
	const std::vector<uint32_t> checksumsb = c13b.AddChecksums({name});
	InlineeDef same;
	same.inlinee = 0x99u;
	same.fileChecksumOffset = checksumsb[0];
	same.lineNumber = 10u;
	c13b.AddInlineeLines({same, same}, 0u);
	const ModuleBytes moduleb = FinishModule(symbols, c13b);
	const Bytes imageb = msf({moduleb.stream, names.Build()});
	PDB::InlineSourceIndex indexb;
	TASK_CHECK(BuildIndex(indexb, imageb, 1u, {Slice(11u, 0u, moduleb, 0u, static_cast<uint32_t>(c13b.bytes.size()))}));
	TASK_CHECK(indexb.IsValid());
	TASK_CHECK(indexb.Query(1u, 0x100u).size() == 1u);

	return 0;
}

static int module_identity_invalid_references(void)
{
	// an inline site referencing an inlinee that is not defined fails with a diagnostic
	{
		NamesStream names;
		const uint32_t name = names.Add("u.cpp");
		SymStream symbols;
		const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x100u, "u");
		Bytes annotations;
		cvU(annotations, 12u);
		cvU(annotations, 4u);
		cvU(annotations, 0u);
		cvU(annotations, 0u);
		const size_t site = symbols.AddSite(proc, 0xABCdu, annotations);
		const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
		symbols.PatchEnd(site, siteEnd);
		const size_t procEnd = symbols.AddRecord(S_END);
		symbols.PatchEnd(proc, procEnd);

		C13Stream c13;
		const std::vector<uint32_t> checksums = c13.AddChecksums({name});
		InlineeDef definition;
		definition.inlinee = 0x1u;		// not 0xABCD
		definition.fileChecksumOffset = checksums[0];
		definition.lineNumber = 1u;
		c13.AddInlineeLines({definition}, 0u);

		const ModuleBytes module = FinishModule(symbols, c13);
		const Bytes image = msf({module.stream, names.Build()});
		PDB::InlineSourceIndex index;
		TASK_CHECK(!BuildIndex(index, image, 1u, {Slice(12u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));
		TASK_CHECK(index.GetError().code == PDB::InlineSourceErrorCode::InvalidInlineeReference);
		TASK_CHECK(index.GetError().moduleIndex == 12u);
		TASK_CHECK(index.GetError().streamIndex == 0u);
		TASK_CHECK(index.GetError().offset == site);
	}

	// a checksum record pointing outside the "/names" string table is rejected
	{
		NamesStream names;
		names.Add("v.cpp");
		SymStream symbols;
		const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x100u, "v");
		const size_t procEnd = symbols.AddRecord(S_END);
		symbols.PatchEnd(proc, procEnd);

		C13Stream c13;
		const std::vector<uint32_t> checksums = c13.AddChecksums({0xFFFFu});
		InlineeDef definition;
		definition.inlinee = 0x1u;
		definition.fileChecksumOffset = checksums[0];
		definition.lineNumber = 1u;
		c13.AddInlineeLines({definition}, 0u);

		const ModuleBytes module = FinishModule(symbols, c13);
		const Bytes image = msf({module.stream, names.Build()});
		PDB::InlineSourceIndex index;
		TASK_CHECK(!BuildIndex(index, image, 1u, {Slice(13u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));
		TASK_CHECK(index.GetError().code == PDB::InlineSourceErrorCode::InvalidFileReference);
		TASK_CHECK(index.GetError().moduleIndex == 13u);
		TASK_CHECK(index.GetError().streamIndex == 0u);
		TASK_CHECK(index.GetError().offset >= module.symbolBytes);
	}

	return 0;
}

static int module_identity_checksum_switch_per_module(void)
{
	// two files in one module: the frame records the file active at the producing record, and the
	// path is taken from this module's checksum subsection
	NamesStream names;
	const uint32_t firstName = names.Add("first.cpp");
	const uint32_t secondName = names.Add("second.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x40u, 7u, 0x40u, "s");
	Bytes annotations;
	cvU(annotations, 3u);
	cvU(annotations, 8u);		// code 8, first file
	cvU(annotations, 5u);
	cvU(annotations, 8u);		// switch to the second checksum offset
	cvU(annotations, 3u);
	cvU(annotations, 8u);		// code 16, second file, seals [8,16)
	cvU(annotations, 4u);
	cvU(annotations, 4u);		// explicit length 4
	cvU(annotations, 0u);
	const size_t site = symbols.AddSite(proc, 0x5Au, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({firstName, secondName});
	TASK_CHECK(checksums[1] == 8u);
	InlineeDef definition;
	definition.inlinee = 0x5Au;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 1u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(14u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(7u, 0x48u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames[0].file == "first.cpp");
		TASK_CHECK(chains[0].frames[0].fileChecksumOffset == checksums[0]);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x48u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x50u);
	}
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(7u, 0x51u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames[0].file == "second.cpp");
		TASK_CHECK(chains[0].frames[0].fileChecksumOffset == checksums[1]);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x50u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x54u);
	}

	return 0;
}


// ------------------------------------------------------------------------------------------------
// nested_ranges: nested scopes, ancestor holes, sibling overlap, boundaries, ownership
// ------------------------------------------------------------------------------------------------
static int nested_ranges_nested_holes(void)
{
	NamesStream names;
	const uint32_t outerName = names.Add("outer.cpp");
	const uint32_t innerName = names.Add("inner.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x100u, 1u, 0x100u, "outer");

	// outer site: [0,20) and [40,60) procedure-relative, leaving a hole in [20,40)
	Bytes outerAnnotations;
	cvU(outerAnnotations, 12u);
	cvU(outerAnnotations, 20u);
	cvU(outerAnnotations, 0u);
	cvU(outerAnnotations, 12u);
	cvU(outerAnnotations, 20u);
	cvU(outerAnnotations, 20u);		// advance 20 -> offset 40, length 20
	cvU(outerAnnotations, 0u);
	const size_t outerSite = symbols.AddSite(proc, 0x10u, outerAnnotations);

	// inner site: coordinates are still relative to the physical procedure
	Bytes innerAnnotations;
	cvU(innerAnnotations, 1u);
	cvU(innerAnnotations, 10u);
	cvU(innerAnnotations, 12u);
	cvU(innerAnnotations, 5u);		// [10,15)
	cvU(innerAnnotations, 0u);
	cvU(innerAnnotations, 1u);
	cvU(innerAnnotations, 25u);
	cvU(innerAnnotations, 12u);
	cvU(innerAnnotations, 5u);		// [25,30), inside the outer hole
	cvU(innerAnnotations, 0u);
	cvU(innerAnnotations, 1u);
	cvU(innerAnnotations, 45u);
	cvU(innerAnnotations, 12u);
	cvU(innerAnnotations, 10u);		// [45,55)
	cvU(innerAnnotations, 0u);
	const size_t innerSite = symbols.AddSite(outerSite, 0x20u, innerAnnotations);

	const size_t innerEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(innerSite, innerEnd);
	const size_t outerEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(outerSite, outerEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({outerName, innerName});
	InlineeDef outerDef;
	outerDef.inlinee = 0x10u;
	outerDef.fileChecksumOffset = checksums[0];
	outerDef.lineNumber = 100u;
	InlineeDef innerDef;
	innerDef.inlinee = 0x20u;
	innerDef.fileChecksumOffset = checksums[1];
	innerDef.lineNumber = 200u;
	c13.AddInlineeLines({outerDef, innerDef}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(20u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	// inside outer [0,20) and inner [10,15): a two-frame chain
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x10Cu);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 2u);
		TASK_CHECK(chains[0].frames[0].symbolOffset == outerSite);
		TASK_CHECK(chains[0].frames[1].symbolOffset == innerSite);
		TASK_CHECK(chains[0].frames[0].file == "outer.cpp");
		TASK_CHECK(chains[0].frames[0].lineNumber == 100u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x100u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x114u);
		TASK_CHECK(chains[0].frames[1].file == "inner.cpp");
		TASK_CHECK(chains[0].frames[1].lineNumber == 200u);
		TASK_CHECK(chains[0].frames[1].codeOffsetBegin == 0x10Au);
		TASK_CHECK(chains[0].frames[1].codeOffsetEnd == 0x10Fu);
	}

	// the inner record [25,30) falls into the outer hole and must not appear on its own
	TASK_CHECK(index.Query(1u, 0x11Bu).empty());

	// inside the second outer range and the third inner range
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0x12Fu);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 2u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x128u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x13Cu);
		TASK_CHECK(chains[0].frames[1].codeOffsetBegin == 0x12Du);
		TASK_CHECK(chains[0].frames[1].codeOffsetEnd == 0x137u);
	}

	// outside every declared range
	TASK_CHECK(index.Query(1u, 0x141u).empty());
	TASK_CHECK(index.Query(1u, 0x100u - 1u).empty());

	return 0;
}

static int nested_ranges_sibling_overlap(void)
{
	NamesStream names;
	const uint32_t name = names.Add("sibling.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x200u, 3u, 0x100u, "siblings");

	// sibling 1: [0,32)
	Bytes first;
	cvU(first, 12u);
	cvU(first, 32u);
	cvU(first, 0u);
	cvU(first, 0u);
	const size_t siteA = symbols.AddSite(proc, 0xA1u, first);

	// child of sibling 1: [24,40), clipped by its parent to [24,32)
	Bytes child;
	cvU(child, 1u);
	cvU(child, 24u);
	cvU(child, 12u);
	cvU(child, 16u);
	cvU(child, 0u);
	cvU(child, 0u);
	const size_t siteC = symbols.AddSite(siteA, 0xC3u, child);

	// sibling 2: [16,48), overlapping both sibling 1 and its child
	Bytes second;
	cvU(second, 1u);
	cvU(second, 16u);
	cvU(second, 12u);
	cvU(second, 32u);
	cvU(second, 0u);
	cvU(second, 0u);
	const size_t siteB = symbols.AddSite(proc, 0xB2u, second);

	const size_t childEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(siteC, childEnd);
	const size_t siteAEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(siteA, siteAEnd);
	const size_t siteBEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(siteB, siteBEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});
	InlineeDef a;
	a.inlinee = 0xA1u;
	a.fileChecksumOffset = checksums[0];
	a.lineNumber = 10u;
	InlineeDef b = a;
	b.inlinee = 0xB2u;
	b.lineNumber = 20u;
	InlineeDef c = a;
	c.inlinee = 0xC3u;
	c.lineNumber = 30u;
	c13.AddInlineeLines({a, b, c}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(21u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	// only sibling 1 covers 12
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(3u, 0x20Cu);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 1u);
		TASK_CHECK(chains[0].frames[0].symbolOffset == siteA);
	}

	// in the overlap both sibling chains are returned, and the nested chain is deeper
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(3u, 0x21Cu);
		TASK_CHECK(chains.size() == 2u);
		TASK_CHECK(chains[0].frames.size() == 2u);
		TASK_CHECK(chains[0].frames[0].symbolOffset == siteA);
		TASK_CHECK(chains[0].frames[1].symbolOffset == siteC);
		TASK_CHECK(chains[1].frames.size() == 1u);
		TASK_CHECK(chains[1].frames[0].symbolOffset == siteB);
		// sibling 2's frame starts earlier in the procedure than the nested child's
		TASK_CHECK(chains[0].frames[0].inlinee == 0xA1u);
		TASK_CHECK(chains[0].frames[1].inlinee == 0xC3u);
		TASK_CHECK(chains[1].frames[0].inlinee == 0xB2u);
		// the child's effective range is clipped to its parent
		TASK_CHECK(chains[0].frames[1].codeOffsetBegin == 0x218u);
		TASK_CHECK(chains[0].frames[1].codeOffsetEnd == 0x220u);
	}

	// at 32 sibling 1 and the child end (half-open), only sibling 2 remains
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(3u, 0x220u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 1u);
		TASK_CHECK(chains[0].frames[0].symbolOffset == siteB);
		TASK_CHECK(chains[0].frames[0].inlinee == 0xB2u);
	}

	// sibling 2 ends at 48
	TASK_CHECK(index.Query(3u, 0x230u).empty());

	return 0;
}

static int nested_ranges_same_inlinee_two_call_sites(void)
{
	// the same inlinee id is used by two separate sites; each keeps its own record identity and
	// its own source position
	NamesStream names;
	const uint32_t name = names.Add("twice.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x900u, 10u, 0x80u, "twice");

	Bytes first;
	cvU(first, 1u);
	cvU(first, 4u);
	cvU(first, 6u);
	cvS(first, 5);			// line 6 + 5 = 11
	cvU(first, 12u);
	cvU(first, 8u);			// [4,12)
	cvU(first, 0u);
	cvU(first, 0u);
	const size_t siteA = symbols.AddSite(proc, 0x4D1u, first);

	Bytes second;
	cvU(second, 1u);
	cvU(second, 24u);
	cvU(second, 6u);
	cvS(second, 9);			// line 6 + 9 = 15
	cvU(second, 12u);
	cvU(second, 8u);			// [24,32)
	cvU(second, 0u);
	cvU(second, 0u);
	const size_t siteB = symbols.AddSite(proc, 0x4D1u, second);

	const size_t siteEndA = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(siteA, siteEndA);
	const size_t siteEndB = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(siteB, siteEndB);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});
	InlineeDef definition;
	definition.inlinee = 0x4D1u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 6u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(28u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(10u, 0x907u);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 1u);
		TASK_CHECK(chains[0].frames[0].inlinee == 0x4D1u);
		TASK_CHECK(chains[0].frames[0].symbolOffset == siteA);
		TASK_CHECK(chains[0].frames[0].lineNumber == 11u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x904u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x90Cu);
	}
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(10u, 0x91Bu);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 1u);
		TASK_CHECK(chains[0].frames[0].inlinee == 0x4D1u);
		TASK_CHECK(chains[0].frames[0].symbolOffset == siteB);
		TASK_CHECK(chains[0].frames[0].lineNumber == 15u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x918u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x920u);
	}

	return 0;
}

static int nested_ranges_extra_files_array(void)
{
	// signature 1 carries a trailing file array; the declared length must be consumed instead of
	// being mistaken for the next inlinee definition
	NamesStream names;
	const uint32_t name = names.Add("extras.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0xA00u, 1u, 0x40u, "extras");
	Bytes annotations;
	cvU(annotations, 12u);
	cvU(annotations, 4u);
	cvU(annotations, 0u);
	cvU(annotations, 0u);
	const size_t site = symbols.AddSite(proc, 0x5E1u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name, names.Add("other.cpp"), names.Add("third.cpp")});

	// this definition declares two extra file checksums; then a second, unrelated definition
	// follows.  Misreading the array would shift the second record.
	InlineeDef first;
	first.inlinee = 0x5E1u;
	first.fileChecksumOffset = checksums[0];
	first.lineNumber = 1u;
	first.extraFiles.push_back(checksums[1]);
	first.extraFiles.push_back(checksums[2]);
	InlineeDef second;
	second.inlinee = 0x5E2u;
	second.fileChecksumOffset = checksums[2];
	second.lineNumber = 50u;
	c13.AddInlineeLines({first, second}, 1u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(29u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0xA02u);
	TASK_CHECK(chains.size() == 1u);
	TASK_CHECK(chains[0].frames[0].inlinee == 0x5E1u);
	TASK_CHECK(chains[0].frames[0].file == "extras.cpp");
	TASK_CHECK(chains[0].frames[0].fileChecksumOffset == checksums[0]);
	TASK_CHECK(chains[0].frames[0].lineNumber == 1u);

	return 0;
}

static int nested_ranges_extra_files_unknown(void)
{
	// a signature-1 extra file that does not resolve through "/names" is a diagnostic
	NamesStream names;
	names.Add("known.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0xA00u, 1u, 0x40u, "extras");
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({0u});
	InlineeDef definition;
	definition.inlinee = 0x5E1u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 1u;
	definition.extraFiles.push_back(0xFFFFu);		// outside the string table
	c13.AddInlineeLines({definition}, 1u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(!BuildIndex(index, image, 1u, {Slice(30u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));
	TASK_CHECK(index.GetError().code == PDB::InlineSourceErrorCode::InvalidFileReference);
	TASK_CHECK(index.GetError().moduleIndex == 30u);
	TASK_CHECK(index.GetError().streamIndex == 0u);
	TASK_CHECK(index.GetError().offset >= module.symbolBytes);

	return 0;
}

static int nested_ranges_noncontiguous_pages_and_release(void)
{
	NamesStream names;
	const uint32_t name = names.Add("release.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x300u, 4u, 0x40u, "release");

	// push the module stream beyond one 512-byte page so the fixture stores it on separate,
	// non-adjacent pages
	{
		fixture::Bytes filler(600u, 0x5Au);
		record(symbols.bytes, 0x0007u, filler);
	}

	Bytes outerAnnotations;
	cvU(outerAnnotations, 12u);
	cvU(outerAnnotations, 8u);
	cvU(outerAnnotations, 0u);
	cvU(outerAnnotations, 0u);
	const size_t outerSite = symbols.AddSite(proc, 0xD1u, outerAnnotations);

	Bytes innerAnnotations;
	cvU(innerAnnotations, 1u);
	cvU(innerAnnotations, 2u);
	cvU(innerAnnotations, 12u);
	cvU(innerAnnotations, 4u);
	cvU(innerAnnotations, 0u);
	cvU(innerAnnotations, 0u);
	const size_t innerSite = symbols.AddSite(outerSite, 0xD2u, innerAnnotations);

	const size_t innerEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(innerSite, innerEnd);
	const size_t outerEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(outerSite, outerEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});
	InlineeDef outer;
	outer.inlinee = 0xD1u;
	outer.fileChecksumOffset = checksums[0];
	outer.lineNumber = 1u;
	InlineeDef inner = outer;
	inner.inlinee = 0xD2u;
	inner.lineNumber = 2u;
	c13.AddInlineeLines({outer, inner}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	TASK_CHECK(module.stream.size() > 512u);		// spans more than one page

	const Bytes image = msf({module.stream, names.Build()});

	// build the index from a separate heap copy, then release both the RawFile and the bytes
	PDB::InlineSourceIndex index;
	{
		const size_t size = image.size();
		std::unique_ptr<unsigned char[]> heap(new unsigned char[size]);
		std::memcpy(heap.get(), image.data(), size);
		{
			const PDB::RawFile raw = PDB::CreateRawFile(heap.get());
			TASK_CHECK(index.Build(raw, 1u, std::vector<PDB::InlineSourceModule>{
				Slice(22u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))
			}));
		}
		heap.reset();
	}

	TASK_CHECK(index.IsValid());
	const std::vector<PDB::InlineSourceChain> chains = index.Query(4u, 0x302u);
	TASK_CHECK(chains.size() == 1u);
	TASK_CHECK(chains[0].frames.size() == 2u);
	TASK_CHECK(chains[0].frames[0].file == "release.cpp");
	TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x300u);
	TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x308u);
	TASK_CHECK(chains[0].frames[1].codeOffsetBegin == 0x302u);
	TASK_CHECK(chains[0].frames[1].codeOffsetEnd == 0x306u);

	return 0;
}

static int nested_ranges_transparent_block(void)
{
	// a lexical block between the procedure and an inline site is transparent to the chain, and
	// nested sites separated by blocks still keep their parent links
	NamesStream names;
	const uint32_t name = names.Add("block.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x400u, 6u, 0x80u, "withblock");
	const size_t block = symbols.AddBlock(proc, 0u, 6u, 0x80u, "lexical");

	Bytes outerAnnotations;
	cvU(outerAnnotations, 12u);
	cvU(outerAnnotations, 16u);
	cvU(outerAnnotations, 0u);
	cvU(outerAnnotations, 0u);
	const size_t outerSite = symbols.AddSite(block, 0xE1u, outerAnnotations);

	const size_t innerBlock = symbols.AddBlock(outerSite, 0u, 6u, 0x80u, "innerlex");

	Bytes innerAnnotations;
	cvU(innerAnnotations, 1u);
	cvU(innerAnnotations, 4u);
	cvU(innerAnnotations, 12u);
	cvU(innerAnnotations, 4u);
	cvU(innerAnnotations, 0u);
	cvU(innerAnnotations, 0u);
	const size_t innerSite = symbols.AddSite(innerBlock, 0xE2u, innerAnnotations);

	const size_t innerEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(innerSite, innerEnd);
	const size_t innerBlockEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(innerBlock, innerBlockEnd);
	const size_t outerEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(outerSite, outerEnd);
	const size_t blockEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(block, blockEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});
	InlineeDef outer;
	outer.inlinee = 0xE1u;
	outer.fileChecksumOffset = checksums[0];
	outer.lineNumber = 1u;
	InlineeDef inner = outer;
	inner.inlinee = 0xE2u;
	inner.lineNumber = 2u;
	c13.AddInlineeLines({outer, inner}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(23u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	const std::vector<PDB::InlineSourceChain> chains = index.Query(6u, 0x405u);
	TASK_CHECK(chains.size() == 1u);
	TASK_CHECK(chains[0].frames.size() == 2u);
	TASK_CHECK(chains[0].frames[0].symbolOffset == outerSite);
	TASK_CHECK(chains[0].frames[1].symbolOffset == innerSite);
	TASK_CHECK(chains[0].frames[0].inlinee == 0xE1u);
	TASK_CHECK(chains[0].frames[1].inlinee == 0xE2u);

	return 0;
}

static int nested_ranges_c11_offset(void)
{
	// a non-zero C11 line info slice between symbols and C13 must be honoured
	NamesStream names;
	const uint32_t name = names.Add("c11.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x500u, 8u, 0x40u, "c11");
	Bytes annotations;
	cvU(annotations, 12u);
	cvU(annotations, 8u);
	cvU(annotations, 0u);
	cvU(annotations, 0u);
	const size_t site = symbols.AddSite(proc, 0xF1u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});
	InlineeDef definition;
	definition.inlinee = 0xF1u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 3u;
	c13.AddInlineeLines({definition}, 0u);

	const uint32_t c11Bytes = 12u;
	const ModuleBytes module = FinishModule(symbols, c13, c11Bytes);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(24u, 0u, module, c11Bytes, static_cast<uint32_t>(c13.bytes.size()))}));

	const std::vector<PDB::InlineSourceChain> chains = index.Query(8u, 0x504u);
	TASK_CHECK(chains.size() == 1u);
	TASK_CHECK(chains[0].frames[0].file == "c11.cpp");
	TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x500u);
	TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x508u);

	return 0;
}


static int module_identity_rawfile_release(void)
{
	// the same fixture is parsed once with the RawFile alive and once after the bytes have been
	// released; the query results must be identical
	NamesStream names;
	const uint32_t name = names.Add("detached.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0xB00u, 1u, 0x40u, "detached");
	Bytes annotations;
	cvU(annotations, 12u);
	cvU(annotations, 8u);
	cvU(annotations, 0u);
	cvU(annotations, 0u);
	const size_t site = symbols.AddSite(proc, 0x6F1u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});
	InlineeDef definition;
	definition.inlinee = 0x6F1u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 7u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});

	// parse from a temporary heap copy, then destroy both the copy and the RawFile
	PDB::InlineSourceIndex index;
	{
		std::unique_ptr<unsigned char[]> heap(new unsigned char[image.size()]);
		std::memcpy(heap.get(), image.data(), image.size());
		{
			const PDB::RawFile raw = PDB::CreateRawFile(heap.get());
			TASK_CHECK(index.Build(raw, 1u, std::vector<PDB::InlineSourceModule>{
				Slice(31u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))
			}));
		}
		heap.reset();
	}

	TASK_CHECK(index.IsValid());
	const std::vector<PDB::InlineSourceChain> chains = index.Query(1u, 0xB04u);
	TASK_CHECK(chains.size() == 1u);
	TASK_CHECK(chains[0].frames[0].file == "detached.cpp");
	TASK_CHECK(chains[0].frames[0].lineNumber == 7u);
	TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0xB00u);
	TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0xB08u);

	return 0;
}

static int nested_ranges_unknown_procedure_size(void)
{
	// a procedure with codeSize 0 gives the top-level site no procedure bound; its explicit
	// ranges still resolve, while a child clipped by an ancestor hole is dropped
	NamesStream names;
	const uint32_t name = names.Add("unknown.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x600u, 2u, 0u, "unknown");

	Bytes outerAnnotations;
	cvU(outerAnnotations, 1u);
	cvU(outerAnnotations, 10u);
	cvU(outerAnnotations, 12u);
	cvU(outerAnnotations, 10u);	// [10,20)
	cvU(outerAnnotations, 0u);
	cvU(outerAnnotations, 1u);
	cvU(outerAnnotations, 40u);
	cvU(outerAnnotations, 12u);
	cvU(outerAnnotations, 10u);	// [40,50)
	cvU(outerAnnotations, 0u);
	const size_t outerSite = symbols.AddSite(proc, 0x1A1u, outerAnnotations);

	// child covers [5,55) and is clipped to the union of its parent's two ranges
	Bytes innerAnnotations;
	cvU(innerAnnotations, 1u);
	cvU(innerAnnotations, 5u);
	cvU(innerAnnotations, 12u);
	cvU(innerAnnotations, 50u);
	cvU(innerAnnotations, 0u);
	const size_t innerSite = symbols.AddSite(outerSite, 0x1A2u, innerAnnotations);

	const size_t innerEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(innerSite, innerEnd);
	const size_t outerEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(outerSite, outerEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});
	InlineeDef outer;
	outer.inlinee = 0x1A1u;
	outer.fileChecksumOffset = checksums[0];
	outer.lineNumber = 1u;
	InlineeDef inner = outer;
	inner.inlinee = 0x1A2u;
	inner.lineNumber = 2u;
	c13.AddInlineeLines({outer, inner}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(25u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	// parent ranges are [10,20) and [40,50); the child's [5,55) is clipped to both
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(2u, 0x60Fu);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 2u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x60Au);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x614u);
		TASK_CHECK(chains[0].frames[1].codeOffsetBegin == 0x60Au);
		TASK_CHECK(chains[0].frames[1].codeOffsetEnd == 0x614u);
	}
	// in the parent hole the child has been clipped away, so there is no chain at all
	TASK_CHECK(index.Query(2u, 0x620u).empty());
	// the clipped child's second piece covers the parent's second range
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(2u, 0x62Au);
		TASK_CHECK(chains.size() == 1u);
		TASK_CHECK(chains[0].frames.size() == 2u);
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x628u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x632u);
		TASK_CHECK(chains[0].frames[1].codeOffsetBegin == 0x628u);
		TASK_CHECK(chains[0].frames[1].codeOffsetEnd == 0x632u);
	}
	// outside both explicit ranges nothing matches
	TASK_CHECK(index.Query(2u, 0x640u).empty());

	return 0;
}

static int nested_ranges_tail_not_guessed(void)
{
	// no explicit right edge means no coverage, even though the procedure continues
	NamesStream names;
	const uint32_t name = names.Add("tail.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x700u, 1u, 0x100u, "tail");

	Bytes annotations;
	cvU(annotations, 3u);
	cvU(annotations, 10u);		// emit at 10 with no following record
	cvU(annotations, 0u);		// terminator
	const size_t site = symbols.AddSite(proc, 0x2B1u, annotations);
	const size_t siteEnd = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(site, siteEnd);
	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});
	InlineeDef definition;
	definition.inlinee = 0x2B1u;
	definition.fileChecksumOffset = checksums[0];
	definition.lineNumber = 1u;
	c13.AddInlineeLines({definition}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(26u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	// an address inside the procedure but after the unbounded record's start: no coverage
	TASK_CHECK(index.Query(1u, 0x70Fu).empty());
	TASK_CHECK(index.Query(1u, 0x780u).empty());

	return 0;
}

static int nested_ranges_chained_siblings_order(void)
{
	// three siblings with different record offsets all cover one address; results are ordered by
	// their site record offsets, and each keeps its own inlinee identity
	NamesStream names;
	const uint32_t name = names.Add("order.cpp");

	SymStream symbols;
	const size_t proc = symbols.AddProc(S_LPROC32, 0x800u, 9u, 0x80u, "order");

	// each site declares an explicit one-byte range at the procedure's start
	Bytes oneByte;
	cvU(oneByte, 12u);
	cvU(oneByte, 1u);
	cvU(oneByte, 0u);
	cvU(oneByte, 0u);

	const size_t siteA = symbols.AddSite(proc, 0x3C1u, oneByte);
	const size_t siteEndA = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(siteA, siteEndA);

	const size_t siteB = symbols.AddSite(proc, 0x3C2u, oneByte);
	const size_t siteEndB = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(siteB, siteEndB);

	const size_t siteC = symbols.AddSite(proc, 0x3C3u, oneByte);
	const size_t siteEndC = symbols.AddRecord(S_INLINESITE_END);
	symbols.PatchEnd(siteC, siteEndC);

	const size_t procEnd = symbols.AddRecord(S_END);
	symbols.PatchEnd(proc, procEnd);

	C13Stream c13;
	const std::vector<uint32_t> checksums = c13.AddChecksums({name});
	InlineeDef a;
	a.inlinee = 0x3C1u;
	a.fileChecksumOffset = checksums[0];
	a.lineNumber = 10u;
	InlineeDef b = a;
	b.inlinee = 0x3C2u;
	b.lineNumber = 20u;
	InlineeDef c = a;
	c.inlinee = 0x3C3u;
	c.lineNumber = 30u;
	c13.AddInlineeLines({a, b, c}, 0u);

	const ModuleBytes module = FinishModule(symbols, c13);
	const Bytes image = msf({module.stream, names.Build()});
	PDB::InlineSourceIndex index;
	TASK_CHECK(BuildIndex(index, image, 1u, {Slice(27u, 0u, module, 0u, static_cast<uint32_t>(c13.bytes.size()))}));

	// all three sites have the same inlinee id and the same checksum offset; identity is carried
	// by the site record offset, so all three chains are reported, ordered by that offset
	TASK_CHECK(siteA < siteB && siteB < siteC);
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(9u, 0x800u);
		TASK_CHECK(chains.size() == 3u);
		TASK_CHECK(chains[0].frames.size() == 1u);
		TASK_CHECK(chains[1].frames.size() == 1u);
		TASK_CHECK(chains[2].frames.size() == 1u);
		TASK_CHECK(chains[0].frames[0].symbolOffset == siteA);
		TASK_CHECK(chains[1].frames[0].symbolOffset == siteB);
		TASK_CHECK(chains[2].frames[0].symbolOffset == siteC);
		TASK_CHECK(chains[0].frames[0].lineNumber == 10u);
		TASK_CHECK(chains[1].frames[0].lineNumber == 20u);
		TASK_CHECK(chains[2].frames[0].lineNumber == 30u);
		// every site's record starts at the procedure's first code byte, so each covers [0,1)
		TASK_CHECK(chains[0].frames[0].codeOffsetBegin == 0x800u);
		TASK_CHECK(chains[0].frames[0].codeOffsetEnd == 0x801u);
	}

	return 0;
}


// ------------------------------------------------------------------------------------------------
// registration
// ------------------------------------------------------------------------------------------------
const struct task_case task_cases[] = {
	{"annotations", "combined_ops", annotations_combined_ops},
	{"annotations", "signed_and_compressed", annotations_signed_and_compressed},
	{"annotations", "holes_and_tail", annotations_holes_and_tail},
	{"annotations", "column_state", annotations_column_state},
	{"annotations", "large_offsets", annotations_large_offsets},
	{"annotations", "truncated_operand", annotations_truncated_operand},
	{"annotations", "backwards_offset", annotations_backwards_offset},
	{"annotations", "unsupported_records", annotations_unsupported_records},
	{"annotations", "bad_scope_terminator", annotations_bad_scope_terminator},
	{"annotations", "range_outside_procedure", annotations_range_outside_procedure},

	{"module_identity", "same_ids", module_identity_same_ids},
	{"module_identity", "duplicate_inlinee", module_identity_duplicate_inlinee},
	{"module_identity", "invalid_references", module_identity_invalid_references},
	{"module_identity", "checksum_switch_per_module", module_identity_checksum_switch_per_module},
	{"module_identity", "rawfile_release", module_identity_rawfile_release},

	{"nested_ranges", "nested_holes", nested_ranges_nested_holes},
	{"nested_ranges", "sibling_overlap", nested_ranges_sibling_overlap},
	{"nested_ranges", "noncontiguous_pages_and_release", nested_ranges_noncontiguous_pages_and_release},
	{"nested_ranges", "transparent_block", nested_ranges_transparent_block},
	{"nested_ranges", "c11_offset", nested_ranges_c11_offset},
	{"nested_ranges", "unknown_procedure_size", nested_ranges_unknown_procedure_size},
	{"nested_ranges", "tail_not_guessed", nested_ranges_tail_not_guessed},
	{"nested_ranges", "sibling_order", nested_ranges_chained_siblings_order},
	{"nested_ranges", "same_inlinee_two_sites", nested_ranges_same_inlinee_two_call_sites},
	{"nested_ranges", "extra_files_array", nested_ranges_extra_files_array},
	{"nested_ranges", "extra_files_unknown", nested_ranges_extra_files_unknown},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
