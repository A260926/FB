// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

// Task tests for the owning address-provenance index.  Every fixture is synthesized in C++ as
// legal MSF/DBI bytes (including deliberately malformed streams for the diagnostic group), then
// opened through the real PDB::CreateRawFile() entry point and exercised through the new API.

#include "task_support/check.h"

#include "PDB.h"
#include "PDB_Types.h"
#include "PDB_DBITypes.h"
#include "PDB_RawFile.h"
#include "PDB_DirectMSFStream.h"
#include "PDB_AddressProvenance.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>


namespace
{
	using Bytes = std::vector<unsigned char>;

	template <typename T>
	void Append(Bytes& out, const T& value)
	{
		const auto* p = reinterpret_cast<const unsigned char*>(&value);
		out.insert(out.end(), p, p + sizeof(T));
	}

	void PutU16(Bytes& out, unsigned value)
	{
		out.push_back(static_cast<unsigned char>(value & 0xffu));
		out.push_back(static_cast<unsigned char>((value >> 8) & 0xffu));
	}

	void PutU32(Bytes& out, unsigned value)
	{
		for (unsigned i = 0u; i < 4u; ++i)
		{
			out.push_back(static_cast<unsigned char>((value >> (8u * i)) & 0xffu));
		}
	}

	void PutOmapRow(Bytes& out, unsigned sourceRva, unsigned targetRva)
	{
		PutU32(out, sourceRva);
		PutU32(out, targetRva);
	}

	PDB::IMAGE_SECTION_HEADER MakeSection(unsigned virtualAddress, unsigned virtualSize,
		unsigned rawSize, const char* name = ".text")
	{
		PDB::IMAGE_SECTION_HEADER section {};
		std::memcpy(section.Name, name, std::min<std::size_t>(std::strlen(name), 8u));
		section.VirtualAddress = virtualAddress;
		section.Misc.VirtualSize = virtualSize;
		section.SizeOfRawData = rawSize;
		return section;
	}

	struct Msf
	{
		Bytes bytes;
		std::vector<std::vector<unsigned>> blocks;
	};

	// Builds a legal MSF container.  Stream blocks are allocated with a one-page gap on purpose,
	// so every multi-block stream is physically scattered across non-contiguous pages.
	Msf BuildMsf(const std::vector<Bytes>& streams, unsigned page = 512u)
	{
		Bytes directory;
		PutU32(directory, static_cast<unsigned>(streams.size()));
		for (const Bytes& stream : streams)
		{
			PutU32(directory, static_cast<unsigned>(stream.size()));
		}

		unsigned next = 5u;
		std::vector<std::vector<unsigned>> blocks;
		for (const Bytes& stream : streams)
		{
			blocks.emplace_back();
			for (std::size_t offset = 0u; offset < stream.size(); offset += page)
			{
				blocks.back().push_back(next);
				PutU32(directory, next);
				next += 2u;		// leave a gap page: blocks are never contiguous
			}
		}

		if (directory.size() > page)
		{
			// the fixtures below are sized to keep the stream directory on a single page
			std::abort();
		}
		Bytes result(static_cast<std::size_t>(next) * page, 0xcc);
		std::fill(result.begin(), result.begin() + 5u * page, static_cast<unsigned char>(0));

		PDB::SuperBlock super {};
		std::memcpy(super.fileMagic, PDB::SuperBlock::MAGIC, 30u);
		super.blockSize = page;
		super.freeBlockMapIndex = 1u;
		super.blockCount = next;
		super.directorySize = static_cast<unsigned>(directory.size());
		std::memcpy(result.data(), &super, sizeof(super));

		const unsigned blockMapPage = 4u;
		std::memcpy(result.data() + sizeof(super), &blockMapPage, sizeof(blockMapPage));
		const unsigned directoryPage = 3u;
		std::memcpy(result.data() + 4u * page, &directoryPage, sizeof(directoryPage));
		std::memcpy(result.data() + 3u * page, directory.data(), directory.size());

		for (std::size_t i = 0u; i < streams.size(); ++i)
		{
			std::size_t offset = 0u;
			for (unsigned physical : blocks[i])
			{
				const std::size_t size = std::min<std::size_t>(page, streams[i].size() - offset);
				std::memcpy(result.data() + static_cast<std::size_t>(physical) * page,
					streams[i].data() + offset, size);
				offset += size;
			}
		}

		return Msf { std::move(result), std::move(blocks) };
	}

	PDB::DBI::StreamHeader MakeDbiHeader(unsigned contributionSize, unsigned debugHeaderSize)
	{
		PDB::DBI::StreamHeader header {};
		header.signature = PDB::DBI::StreamHeader::Signature;
		header.version = PDB::DBI::StreamHeader::Version::V70;
		header.globalStreamIndex = PDB::NilStreamIndex;
		header.publicStreamIndex = PDB::NilStreamIndex;
		header.symbolRecordStreamIndex = PDB::NilStreamIndex;
		header.moduleInfoSize = 0u;
		header.sectionContributionSize = contributionSize;
		header.sectionMapSize = 4u;
		header.sourceInfoSize = 4u;
		header.typeServerMapSize = 0u;
		header.mfcTypeServerIndex = 0u;
		header.optionalDebugHeaderSize = debugHeaderSize;
		header.ecSize = 0u;
		return header;
	}

	void AppendContribution(Bytes& dbi, unsigned section, unsigned offset, unsigned size, unsigned module)
	{
		PDB::DBI::SectionContribution record {};
		record.section = static_cast<uint16_t>(section);
		record.offset = offset;
		record.size = size;
		record.moduleIndex = static_cast<uint16_t>(module);
		Append(dbi, record);
	}

	// Stream layout used by the rich fixtures: indices 3 (DBI), 5 (current sections),
	// 6 (original sections), 7 (OMAP_FROM_SRC), 8 (OMAP_TO_SRC or payload).
	constexpr unsigned DBI_INDEX = 3u;
	constexpr unsigned SECTION_INDEX = 5u;
	constexpr unsigned ORIG_SECTION_INDEX = 6u;
	constexpr unsigned OMAP_FROM_INDEX = 7u;
	constexpr unsigned OMAP_TO_INDEX = 8u;
	constexpr unsigned SCATTER_INDEX = 9u;

	void AppendFullDebugHeader(Bytes& dbi, unsigned toSrc, unsigned fromSrc, unsigned section, unsigned orig)
	{
		PDB::DBI::DebugHeader debug {};
		for (unsigned i = 0u; i < 11u; ++i)
		{
			(&debug.fpoDataStreamIndex)[i] = PDB::NilStreamIndex;
		}
		debug.omapToSrcDataStreamIndex = static_cast<uint16_t>(toSrc);
		debug.omapFromSrcDataStreamIndex = static_cast<uint16_t>(fromSrc);
		debug.sectionHeaderStreamIndex = static_cast<uint16_t>(section);
		debug.originalSectionHeaderDataStreamIndex = static_cast<uint16_t>(orig);
		Append(dbi, debug);
	}

	const PDB::Provenance::ForwardSpan* FindSpan(const std::vector<PDB::Provenance::ForwardSpan>& spans,
		unsigned long long start)
	{
		for (const auto& span : spans)
		{
			if (span.original.start == start)
			{
				return &span;
			}
		}
		return nullptr;
	}
}


// ================================================================================================
// group: streams
// ================================================================================================

static int case_scattered_pages_and_full_debug_header(void)
{
	std::vector<Bytes> streams(10u);

	Bytes& dbi = streams[DBI_INDEX];
	Append(dbi, MakeDbiHeader(4u + static_cast<unsigned>(sizeof(PDB::DBI::SectionContribution)),
		static_cast<unsigned>(sizeof(PDB::DBI::DebugHeader))));
	Append(dbi, PDB::DBI::SectionContribution::Version::Ver60);
	AppendContribution(dbi, 1u, 0u, 0x40u, 7u);
	PutU32(dbi, 0u);	// section map placeholder
	PutU32(dbi, 0u);	// source info placeholder
	AppendFullDebugHeader(dbi, OMAP_TO_INDEX, OMAP_FROM_INDEX, SECTION_INDEX, ORIG_SECTION_INDEX);

	Append(streams[SECTION_INDEX], MakeSection(0x5000u, 0x200u, 0x200u));
	Append(streams[ORIG_SECTION_INDEX], MakeSection(0x1000u, 0x400u, 0x400u));
	PutOmapRow(streams[OMAP_FROM_INDEX], 0x1000u, 0x5000u);
	PutOmapRow(streams[OMAP_TO_INDEX], 0x5000u, 0x1000u);

	// a payload stream spanning several scattered pages
	streams[SCATTER_INDEX].resize(1100u);
	for (std::size_t i = 0u; i < streams[SCATTER_INDEX].size(); ++i)
	{
		streams[SCATTER_INDEX][i] = static_cast<unsigned char>((i * 37u + 19u) & 0xffu);
	}

	const Msf msf = BuildMsf(streams);

	// the scattered stream really occupies non-contiguous physical pages
	TASK_CHECK(msf.blocks[SCATTER_INDEX].size() >= 3u);
	for (std::size_t i = 1u; i < msf.blocks[SCATTER_INDEX].size(); ++i)
	{
		TASK_CHECK(msf.blocks[SCATTER_INDEX][i] > msf.blocks[SCATTER_INDEX][i - 1u] + 1u);
	}

	TASK_CHECK(PDB::ValidateFile(msf.bytes.data(), msf.bytes.size()) == PDB::ErrorCode::Success);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	TASK_CHECK(raw.GetStreamCount() == streams.size());

	// scattered reads still return the exact bytes through the real MSF reader
	PDB::DirectMSFStream scattered = raw.CreateMSFStream<PDB::DirectMSFStream>(SCATTER_INDEX);
	Bytes probe(1030u);
	scattered.ReadAtOffset(probe.data(), probe.size(), 29u);
	TASK_CHECK(std::equal(probe.begin(), probe.end(), streams[SCATTER_INDEX].begin() + 29u));

	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);
	TASK_CHECK(built.diagnostics.empty());
	const PDB::Provenance::AddressProvenance& index = built.index;
	TASK_CHECK(!index.IsIdentity());
	TASK_CHECK(index.HasOmapToSrc());
	TASK_CHECK(index.GetOmapFromStreamIndex() == OMAP_FROM_INDEX);
	TASK_CHECK(index.GetOmapRowCount() == 1u);
	TASK_CHECK(index.GetCurrentSectionCount() == 1u);
	TASK_CHECK(index.GetOriginalSectionCount() == 1u);
	TASK_CHECK(index.GetContributionCount() == 1u);
	return 0;
}

static int case_short_optional_debug_header(void)
{
	// only the first six 16-bit slots are present; every later slot must behave like 0xffff
	std::vector<Bytes> streams(9u);
	const unsigned shortHeaderSize = 6u * sizeof(uint16_t);

	Bytes& dbi = streams[DBI_INDEX];
	Append(dbi, MakeDbiHeader(4u, shortHeaderSize));
	Append(dbi, PDB::DBI::SectionContribution::Version::Ver60);
	PutU32(dbi, 0u);
	PutU32(dbi, 0u);
	for (unsigned slot = 0u; slot < 6u; ++slot)
	{
		PutU16(dbi, (slot == 5u) ? SECTION_INDEX : PDB::NilStreamIndex);
	}

	Append(streams[SECTION_INDEX], MakeSection(0x4000u, 0x100u, 0x100u));

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);
	TASK_CHECK(built.index.IsIdentity());
	TASK_CHECK(built.index.GetOmapRowCount() == 0u);
	TASK_CHECK(built.index.GetOriginalSectionCount() == 1u);
	return 0;
}

static int case_identity_fallback(void)
{
	std::vector<Bytes> streams(9u);
	Bytes& dbi = streams[DBI_INDEX];
	Append(dbi, MakeDbiHeader(4u + static_cast<unsigned>(sizeof(PDB::DBI::SectionContribution)),
		static_cast<unsigned>(sizeof(PDB::DBI::DebugHeader))));
	Append(dbi, PDB::DBI::SectionContribution::Version::Ver60);
	AppendContribution(dbi, 1u, 0x10u, 0x20u, 3u);
	PutU32(dbi, 0u);
	PutU32(dbi, 0u);
	AppendFullDebugHeader(dbi, PDB::NilStreamIndex, PDB::NilStreamIndex, SECTION_INDEX, PDB::NilStreamIndex);

	Append(streams[SECTION_INDEX], MakeSection(0x4000u, 0x100u, 0x100u));

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);
	TASK_CHECK(built.index.IsIdentity());

	const auto spans = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x4000ull, 0x4100ull });
	// cut at the contribution boundary [0x4010,0x4030): three identity pieces
	TASK_CHECK(spans.size() == 3u);
	TASK_CHECK(spans[0u].original.start == 0x4000ull && spans[0u].original.end == 0x4010ull);
	TASK_CHECK(spans[1u].original.start == 0x4010ull && spans[1u].original.end == 0x4030ull);
	TASK_CHECK(spans[2u].original.start == 0x4030ull && spans[2u].original.end == 0x4100ull);
	for (const auto& span : spans)
	{
		TASK_CHECK(span.status == PDB::Provenance::SpanStatus::Mapped);
		TASK_CHECK(span.image.start == span.original.start && span.image.end == span.original.end);
		TASK_CHECK(span.evidence.identity);
		TASK_CHECK(span.evidence.streamIndex == PDB::NilStreamIndex);
	}
	TASK_CHECK(spans[0u].contributions.empty());
	TASK_CHECK(spans[1u].contributions.size() == 1u);
	TASK_CHECK(spans[2u].contributions.empty());

	// the contribution is retained even though no OMAP exists
	const auto covered = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x4010ull, 0x4030ull });
	TASK_CHECK(covered.size() == 1u && covered.front().contributions.size() == 1u);
	TASK_CHECK(covered.front().contributions.front().moduleIndex == 3u);
	TASK_CHECK(covered.front().contributions.front().ordinal == 0u);

	const auto outside = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x4100ull, 0x4200ull });
	TASK_CHECK(outside.size() == 1u);
	TASK_CHECK(outside.front().status == PDB::Provenance::SpanStatus::OutsideSection);
	return 0;
}

static int case_empty_omap_from_stream(void)
{
	// OMAP_FROM_SRC exists but is empty: nothing can be mapped, yet the index is valid
	std::vector<Bytes> streams(9u);
	Bytes& dbi = streams[DBI_INDEX];
	Append(dbi, MakeDbiHeader(4u, static_cast<unsigned>(sizeof(PDB::DBI::DebugHeader))));
	Append(dbi, PDB::DBI::SectionContribution::Version::Ver60);
	PutU32(dbi, 0u);
	PutU32(dbi, 0u);
	AppendFullDebugHeader(dbi, PDB::NilStreamIndex, OMAP_FROM_INDEX, SECTION_INDEX, ORIG_SECTION_INDEX);

	Append(streams[SECTION_INDEX], MakeSection(0x5000u, 0x100u, 0x100u));
	Append(streams[ORIG_SECTION_INDEX], MakeSection(0x1000u, 0x100u, 0x100u));
	// streams[OMAP_FROM_INDEX] deliberately stays empty

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);
	TASK_CHECK(!built.index.IsIdentity());
	TASK_CHECK(built.index.GetOmapRowCount() == 0u);

	const auto spans = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x1000ull, 0x1100ull });
	TASK_CHECK(spans.size() == 1u);
	TASK_CHECK(spans.front().status == PDB::Provenance::SpanStatus::Unmapped);

	const auto interpreted = built.index.InterpretImage(PDB::Provenance::AddressRange { 0x5000ull, 0x5100ull });
	TASK_CHECK(interpreted.size() == 1u);
	TASK_CHECK(interpreted.front().origins.empty());
	return 0;
}


// ================================================================================================
// group: forward_spans
// ================================================================================================

static int case_deletion_and_unmapped_hole(void)
{
	std::vector<Bytes> streams(9u);
	Bytes& dbi = streams[DBI_INDEX];
	Append(dbi, MakeDbiHeader(4u, static_cast<unsigned>(sizeof(PDB::DBI::DebugHeader))));
	Append(dbi, PDB::DBI::SectionContribution::Version::Ver60);
	PutU32(dbi, 0u);
	PutU32(dbi, 0u);
	AppendFullDebugHeader(dbi, PDB::NilStreamIndex, OMAP_FROM_INDEX, SECTION_INDEX, ORIG_SECTION_INDEX);

	Append(streams[SECTION_INDEX], MakeSection(0x6000u, 0x300u, 0x300u));
	Append(streams[ORIG_SECTION_INDEX], MakeSection(0x1000u, 0x300u, 0x300u));
	PutOmapRow(streams[OMAP_FROM_INDEX], 0x1100u, 0x6000u);	// mapped middle
	PutOmapRow(streams[OMAP_FROM_INDEX], 0x1200u, 0u);		// deleted tail

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);

	const auto spans = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x1000ull, 0x1300ull });
	TASK_CHECK(spans.size() == 3u);
	TASK_CHECK(spans[0u].status == PDB::Provenance::SpanStatus::Unmapped);
	TASK_CHECK(spans[0u].original.start == 0x1000ull && spans[0u].original.end == 0x1100ull);
	TASK_CHECK(spans[0u].evidence.row == UINT32_MAX);
	TASK_CHECK(spans[0u].image.start == 0u && spans[0u].image.end == 0u);

	TASK_CHECK(spans[1u].status == PDB::Provenance::SpanStatus::Mapped);
	TASK_CHECK(spans[1u].original.start == 0x1100ull && spans[1u].original.end == 0x1200ull);
	TASK_CHECK(spans[1u].image.start == 0x6000ull && spans[1u].image.end == 0x6100ull);
	TASK_CHECK(spans[1u].evidence.row == 0u && spans[1u].evidence.targetRva == 0x6000u);

	// a deletion row must be reported as deleted, never as a mapping to image address 0
	TASK_CHECK(spans[2u].status == PDB::Provenance::SpanStatus::Deleted);
	TASK_CHECK(spans[2u].original.start == 0x1200ull && spans[2u].original.end == 0x1300ull);
	TASK_CHECK(spans[2u].evidence.row == 1u && spans[2u].evidence.targetRva == 0u);
	TASK_CHECK(spans[2u].image.start == 0u && spans[2u].image.end == 0u);
	return 0;
}

static int case_cross_section_and_image_boundaries(void)
{
	std::vector<Bytes> streams(9u);
	Bytes& dbi = streams[DBI_INDEX];
	Append(dbi, MakeDbiHeader(4u, static_cast<unsigned>(sizeof(PDB::DBI::DebugHeader))));
	Append(dbi, PDB::DBI::SectionContribution::Version::Ver60);
	PutU32(dbi, 0u);
	PutU32(dbi, 0u);
	AppendFullDebugHeader(dbi, PDB::NilStreamIndex, OMAP_FROM_INDEX, SECTION_INDEX, ORIG_SECTION_INDEX);

	// current image section is shorter than the projected original range
	Append(streams[SECTION_INDEX], MakeSection(0x7000u, 0x100u, 0x100u));
	Bytes& orig = streams[ORIG_SECTION_INDEX];
	Append(orig, MakeSection(0x1000u, 0x200u, 0x200u, ".text"));
	Append(orig, MakeSection(0x3000u, 0x100u, 0x100u, ".rdata"));
	PutOmapRow(streams[OMAP_FROM_INDEX], 0x1000u, 0x7000u);

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);

	const auto spans = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x1000ull, 0x1200ull });
	TASK_CHECK(spans.size() == 2u);
	TASK_CHECK(spans[0u].status == PDB::Provenance::SpanStatus::Mapped);
	TASK_CHECK(spans[0u].original.start == 0x1000ull && spans[0u].original.end == 0x1100ull);
	TASK_CHECK(spans[0u].image.start == 0x7000ull && spans[0u].image.end == 0x7100ull);
	TASK_CHECK(spans[1u].status == PDB::Provenance::SpanStatus::OutsideImage);
	TASK_CHECK(spans[1u].original.start == 0x1100ull && spans[1u].original.end == 0x1200ull);

	// query straddling the original section boundary: outside_image -> outside_section -> unmapped
	const auto cross = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x11f0ull, 0x3010ull });
	TASK_CHECK(cross.size() == 3u);
	TASK_CHECK(cross[0u].status == PDB::Provenance::SpanStatus::OutsideImage);
	TASK_CHECK(cross[1u].status == PDB::Provenance::SpanStatus::OutsideSection);
	TASK_CHECK(cross[1u].original.start == 0x1200ull && cross[1u].original.end == 0x3000ull);
	TASK_CHECK(cross[2u].status == PDB::Provenance::SpanStatus::Unmapped);
	TASK_CHECK(cross[2u].original.start == 0x3000ull && cross[2u].original.end == 0x3010ull);
	return 0;
}

static int case_overlapping_contributions_are_kept(void)
{
	std::vector<Bytes> streams(9u);
	Bytes& dbi = streams[DBI_INDEX];
	Append(dbi, MakeDbiHeader(4u + 2u * static_cast<unsigned>(sizeof(PDB::DBI::SectionContribution)),
		static_cast<unsigned>(sizeof(PDB::DBI::DebugHeader))));
	Append(dbi, PDB::DBI::SectionContribution::Version::Ver60);
	AppendContribution(dbi, 1u, 0x00u, 0x40u, 7u);		// [0x1000,0x1040)
	AppendContribution(dbi, 1u, 0x20u, 0x40u, 8u);		// [0x1020,0x1060), overlaps the first
	PutU32(dbi, 0u);
	PutU32(dbi, 0u);
	AppendFullDebugHeader(dbi, PDB::NilStreamIndex, PDB::NilStreamIndex, SECTION_INDEX, PDB::NilStreamIndex);

	Append(streams[SECTION_INDEX], MakeSection(0x1000u, 0x100u, 0x100u));

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);
	TASK_CHECK(built.index.GetContributionCount() == 2u);

	const auto overlap = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x1020ull, 0x1040ull });
	TASK_CHECK(overlap.size() == 1u);
	TASK_CHECK(overlap.front().contributions.size() == 2u);
	// identities survive individually, ordered by contribution record ordinal
	TASK_CHECK(overlap.front().contributions[0u].ordinal == 0u);
	TASK_CHECK(overlap.front().contributions[0u].moduleIndex == 7u);
	TASK_CHECK(overlap.front().contributions[1u].ordinal == 1u);
	TASK_CHECK(overlap.front().contributions[1u].moduleIndex == 8u);

	const auto onlyFirst = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x1000ull, 0x1020ull });
	TASK_CHECK(onlyFirst.size() == 1u && onlyFirst.front().contributions.size() == 1u);
	TASK_CHECK(onlyFirst.front().contributions.front().moduleIndex == 7u);
	return 0;
}

static int case_virtual_size_zero_uses_raw_size(void)
{
	std::vector<Bytes> streams(9u);
	Bytes& dbi = streams[DBI_INDEX];
	Append(dbi, MakeDbiHeader(4u, static_cast<unsigned>(sizeof(PDB::DBI::DebugHeader))));
	Append(dbi, PDB::DBI::SectionContribution::Version::Ver60);
	PutU32(dbi, 0u);
	PutU32(dbi, 0u);
	AppendFullDebugHeader(dbi, PDB::NilStreamIndex, PDB::NilStreamIndex, SECTION_INDEX, PDB::NilStreamIndex);

	Append(streams[SECTION_INDEX], MakeSection(0x8000u, 0u, 0x80u));	// VirtualSize == 0

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);

	const auto inside = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x8000ull, 0x8080ull });
	TASK_CHECK(inside.size() == 1u && inside.front().status == PDB::Provenance::SpanStatus::Mapped);
	const auto beyond = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x8080ull, 0x8100ull });
	TASK_CHECK(beyond.size() == 1u && beyond.front().status == PDB::Provenance::SpanStatus::OutsideSection);
	return 0;
}


// ================================================================================================
// group: folded_origins
// ================================================================================================

namespace
{
	// Shared fold fixture:
	//   original section 1 [0x1000,0x1400), section 2 [0x2000,0x2400)
	//   current section   [0x5000,0x5200)
	//   OMAP: 0x1000->0x5000 (row 0), 0x1100->0 deleted (row 1), 0x2000->0x5000 fold (row 2)
	//   contributions: rec0 sec1 [0x1000,0x1040) mod7, rec1 sec1 [0x1020,0x1060) mod8,
	//                  rec2 sec2 [0x2000,0x2100) mod9
	Msf BuildFoldMsf()
	{
		std::vector<Bytes> streams(9u);
		Bytes& dbi = streams[DBI_INDEX];
		Append(dbi, MakeDbiHeader(4u + 3u * static_cast<unsigned>(sizeof(PDB::DBI::SectionContribution)),
			static_cast<unsigned>(sizeof(PDB::DBI::DebugHeader))));
		Append(dbi, PDB::DBI::SectionContribution::Version::Ver60);
		AppendContribution(dbi, 1u, 0x000u, 0x40u, 7u);
		AppendContribution(dbi, 1u, 0x020u, 0x40u, 8u);
		AppendContribution(dbi, 2u, 0x000u, 0x100u, 9u);
		PutU32(dbi, 0u);
		PutU32(dbi, 0u);
		AppendFullDebugHeader(dbi, PDB::NilStreamIndex, OMAP_FROM_INDEX, SECTION_INDEX, ORIG_SECTION_INDEX);

		Append(streams[SECTION_INDEX], MakeSection(0x5000u, 0x200u, 0x200u));
		Bytes& orig = streams[ORIG_SECTION_INDEX];
		Append(orig, MakeSection(0x1000u, 0x400u, 0x400u, ".text"));
		Append(orig, MakeSection(0x2000u, 0x400u, 0x400u, ".text2"));
		PutOmapRow(streams[OMAP_FROM_INDEX], 0x1000u, 0x5000u);
		PutOmapRow(streams[OMAP_FROM_INDEX], 0x1100u, 0u);
		PutOmapRow(streams[OMAP_FROM_INDEX], 0x2000u, 0x5000u);

		return BuildMsf(streams);
	}
}

static int case_non_monotone_partial_fold(void)
{
	const Msf msf = BuildFoldMsf();
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);

	const auto forward = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x1000ull, 0x1400ull });
	const PDB::Provenance::ForwardSpan* mapped = FindSpan(forward, 0x1000ull);
	TASK_CHECK(mapped != nullptr && mapped->status == PDB::Provenance::SpanStatus::Mapped);
	TASK_CHECK(mapped->image.start == 0x5000ull);
	// the row-0 region [0x1000,0x1100) projects onto the full [0x5000,0x5100); contribution
	// boundaries split the pieces but the image coverage is contiguous and complete
	bool sawMappedEnd = false;
	for (const auto& span : forward)
	{
		if (span.status == PDB::Provenance::SpanStatus::Mapped && span.original.end == 0x1100ull)
		{
			TASK_CHECK(span.image.end == 0x5100ull);
			sawMappedEnd = true;
		}
	}
	TASK_CHECK(sawMappedEnd);
	const PDB::Provenance::ForwardSpan* deleted = FindSpan(forward, 0x1100ull);
	TASK_CHECK(deleted != nullptr && deleted->status == PDB::Provenance::SpanStatus::Deleted);

	// the folded image interval [0x5000,0x5100) has TWO origins that must not be deduplicated
	const auto interpreted = built.index.InterpretImage(PDB::Provenance::AddressRange { 0x5000ull, 0x5200ull });
	TASK_CHECK(interpreted.size() >= 2u);
	TASK_CHECK(interpreted.front().image.start == 0x5000ull);
	TASK_CHECK(interpreted.front().origins.size() == 2u);

	const auto& firstOrigin = interpreted.front().origins[0u];
	const auto& secondOrigin = interpreted.front().origins[1u];
	TASK_CHECK(firstOrigin.original.start == 0x1000ull);
	TASK_CHECK(secondOrigin.original.start == 0x2000ull);
	TASK_CHECK(firstOrigin.originalSection == 1u && secondOrigin.originalSection == 2u);
	TASK_CHECK(firstOrigin.evidence.row == 0u && secondOrigin.evidence.row == 2u);
	// stable ordering: image position first, then original position within the same image span
	TASK_CHECK(firstOrigin.original.start < secondOrigin.original.start);
	return 0;
}

static int case_reverse_lookup_finds_all_predecessors(void)
{
	const Msf msf = BuildFoldMsf();
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);

	// the query starts in the middle of the folded region; a predecessor-only lookup at the
	// start address would miss the first source that still intersects [start,0x5100)
	const auto interpreted = built.index.InterpretImage(PDB::Provenance::AddressRange { 0x5050ull, 0x5180ull });

	// the returned spans partition the whole query without gaps or overlaps
	TASK_CHECK(!interpreted.empty());
	TASK_CHECK(interpreted.front().image.start == 0x5050ull);
	TASK_CHECK(interpreted.back().image.end == 0x5180ull);
	for (std::size_t i = 1u; i < interpreted.size(); ++i)
	{
		TASK_CHECK(interpreted[i - 1u].image.end == interpreted[i].image.start);
	}

	// every span still inside the folded region [0x5050,0x5100) carries both origins
	for (const auto& span : interpreted)
	{
		if (span.image.start < 0x5100ull)
		{
			TASK_CHECK(span.origins.size() == 2u);
		}
	}

	// the first span begins exactly at the query start and lists both predecessors
	TASK_CHECK(interpreted.front().image.start == 0x5050ull);
	TASK_CHECK(interpreted.front().origins.size() == 2u);

	// the span starting at 0x5100 has a single origin: the folded second source continuing on
	const PDB::Provenance::ImageSpan* tail = nullptr;
	for (const auto& span : interpreted)
	{
		if (span.image.start == 0x5100ull)
		{
			tail = &span;
		}
	}
	TASK_CHECK(tail != nullptr);
	TASK_CHECK(tail->origins.size() == 1u);
	TASK_CHECK(tail->origins[0u].original.start == 0x2100ull);

	// the second source extends past the current image section and is marked outside_image
	const auto forward = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x2200ull, 0x2400ull });
	bool sawOutsideImage = false;
	for (const auto& span : forward)
	{
		sawOutsideImage = sawOutsideImage || (span.status == PDB::Provenance::SpanStatus::OutsideImage);
	}
	TASK_CHECK(sawOutsideImage);
	return 0;
}

static int case_contribution_identities_split_and_merge_spans(void)
{
	const Msf msf = BuildFoldMsf();
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);

	// inside the folded region, the first origin carries two overlapping contributions on
	// [0x1020,0x1040); that identity change must split the image interpretation
	const auto interpreted = built.index.InterpretImage(PDB::Provenance::AddressRange { 0x5020ull, 0x5060ull });
	std::size_t totalOrigins = 0u;
	for (const auto& span : interpreted)
	{
		TASK_CHECK(span.image.start < span.image.end);
		totalOrigins += span.origins.size();
		for (const auto& origin : span.origins)
		{
			TASK_CHECK(origin.original.start < origin.original.end);
			TASK_CHECK(origin.image.start == span.image.start && origin.image.end == span.image.end);
		}
	}
	// every span carries the folded second source plus whichever first-source pieces intersect
	TASK_CHECK(totalOrigins >= interpreted.size() + 1u);

	// overlapping contributions on one origin are both reported, in record order
	const auto forward = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x1020ull, 0x1040ull });
	TASK_CHECK(forward.size() == 1u);
	TASK_CHECK(forward.front().contributions.size() == 2u);
	TASK_CHECK(forward.front().contributions[0u].moduleIndex == 7u);
	TASK_CHECK(forward.front().contributions[1u].moduleIndex == 8u);
	return 0;
}

static int case_index_survives_released_input(void)
{
	Msf msf = BuildFoldMsf();
	PDB::Provenance::AddressProvenance index;
	{
		PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
		PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
		TASK_CHECK(built.ok);
		index = std::move(built.index);
	}

	// destroy and reuse the backing storage; the owning index must keep working unchanged
	std::fill(msf.bytes.begin(), msf.bytes.end(), static_cast<unsigned char>(0));
	msf.bytes.clear();
	msf.bytes.shrink_to_fit();

	const auto interpreted = index.InterpretImage(PDB::Provenance::AddressRange { 0x5000ull, 0x5100ull });

	// the spans partition the whole folded interval, and every span keeps BOTH folded origins
	TASK_CHECK(!interpreted.empty());
	TASK_CHECK(interpreted.front().image.start == 0x5000ull);
	TASK_CHECK(interpreted.back().image.end == 0x5100ull);
	unsigned long long expectedImageOffset = 0ull;
	unsigned long long expectedOriginalOffset = 0ull;
	for (const auto& span : interpreted)
	{
		TASK_CHECK(span.image.start == 0x5000ull + expectedImageOffset);
		TASK_CHECK(span.origins.size() == 2u);
		TASK_CHECK(span.origins[0u].originalSection == 1u);
		TASK_CHECK(span.origins[1u].originalSection == 2u);
		TASK_CHECK(span.origins[0u].original.start == 0x1000ull + expectedOriginalOffset);
		TASK_CHECK(span.origins[1u].original.start == 0x2000ull + expectedOriginalOffset);
		expectedImageOffset += span.image.end - span.image.start;
		expectedOriginalOffset += span.origins[0u].original.end - span.origins[0u].original.start;
	}
	TASK_CHECK(expectedImageOffset == 0x100ull);
	TASK_CHECK(expectedOriginalOffset == 0x100ull);

	const auto forward = index.ProjectOriginal(PDB::Provenance::AddressRange { 0x1000ull, 0x1100ull });
	TASK_CHECK(!forward.empty());
	TASK_CHECK(forward.front().status == PDB::Provenance::SpanStatus::Mapped);
	TASK_CHECK(forward.front().image.start == 0x5000ull);
	bool fullyMapped = true;
	unsigned long long covered = 0ull;
	for (const auto& span : forward)
	{
		fullyMapped = fullyMapped && (span.status == PDB::Provenance::SpanStatus::Mapped);
		covered += span.original.end - span.original.start;
	}
	TASK_CHECK(fullyMapped && covered == 0x100ull);
	return 0;
}

static int case_reverse_across_image_section_boundary(void)
{
	// One original section maps linearly onto an image range spanning two current sections.
	// The mapped projection is cut into fragments at the current-section boundary 0x7100;
	// reverse interpretation of the second fragment must use the segment's own linear base.
	std::vector<Bytes> streams(9u);
	Bytes& dbi = streams[DBI_INDEX];
	Append(dbi, MakeDbiHeader(4u, static_cast<unsigned>(sizeof(PDB::DBI::DebugHeader))));
	Append(dbi, PDB::DBI::SectionContribution::Version::Ver60);
	PutU32(dbi, 0u);
	PutU32(dbi, 0u);
	AppendFullDebugHeader(dbi, PDB::NilStreamIndex, OMAP_FROM_INDEX, SECTION_INDEX, ORIG_SECTION_INDEX);

	Bytes& current = streams[SECTION_INDEX];
	Append(current, MakeSection(0x7000u, 0x100u, 0x100u, ".text"));
	Append(current, MakeSection(0x7100u, 0x100u, 0x100u, ".text2"));
	Append(streams[ORIG_SECTION_INDEX], MakeSection(0x1000u, 0x200u, 0x200u));
	PutOmapRow(streams[OMAP_FROM_INDEX], 0x1000u, 0x7000u);

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(built.ok);

	// forward projection spans both current sections as two mapped pieces
	const auto forward = built.index.ProjectOriginal(PDB::Provenance::AddressRange { 0x1000ull, 0x1200ull });
	for (const auto& span : forward)
	{
		TASK_CHECK(span.status == PDB::Provenance::SpanStatus::Mapped);
	}
	unsigned long long forwardImageCovered = 0ull;
	for (const auto& span : forward)
	{
		forwardImageCovered += span.image.end - span.image.start;
	}
	TASK_CHECK(forwardImageCovered == 0x200ull);

	// query entirely inside the second image fragment: original must be [0x1100,0x1180)
	const auto interpreted = built.index.InterpretImage(PDB::Provenance::AddressRange { 0x7100ull, 0x7180ull });
	TASK_CHECK(interpreted.size() == 1u);
	TASK_CHECK(interpreted.front().image.start == 0x7100ull && interpreted.front().image.end == 0x7180ull);
	TASK_CHECK(interpreted.front().origins.size() == 1u);
	const auto& origin = interpreted.front().origins[0u];
	TASK_CHECK(origin.original.start == 0x1100ull && origin.original.end == 0x1180ull);
	TASK_CHECK(origin.originalSection == 1u);
	TASK_CHECK(origin.evidence.row == 0u);

	// a query straddling the fragment boundary merges back into one span because both fragments
	// share the same source segment, OMAP row and linear relationship; the reverse arithmetic
	// across the boundary must still recover the correct original interval [0x10c0,0x1140)
	const auto straddle = built.index.InterpretImage(PDB::Provenance::AddressRange { 0x70c0ull, 0x7140ull });
	TASK_CHECK(straddle.size() == 1u);
	TASK_CHECK(straddle[0u].image.start == 0x70c0ull && straddle[0u].image.end == 0x7140ull);
	TASK_CHECK(straddle[0u].origins.size() == 1u);
	TASK_CHECK(straddle[0u].origins[0u].original.start == 0x10c0ull);
	TASK_CHECK(straddle[0u].origins[0u].original.end == 0x1140ull);
	return 0;
}


// ================================================================================================
// group: diagnostics
// ================================================================================================

namespace
{
	bool HasDiagnostic(const std::vector<PDB::Provenance::Diagnostic>& diagnostics,
		PDB::Provenance::DiagnosticKind kind, unsigned streamIndex, unsigned long long offset)
	{
		for (const auto& diagnostic : diagnostics)
		{
			if (diagnostic.kind == kind && diagnostic.streamIndex == streamIndex
				&& diagnostic.logicalOffset == offset)
			{
				return true;
			}
		}
		return false;
	}

	// Minimal valid section/OMAP scaffolding around a deliberately broken datum.
	std::vector<Bytes> BaseDiagnosticStreams()
	{
		std::vector<Bytes> streams(9u);
		Append(streams[SECTION_INDEX], MakeSection(0x5000u, 0x200u, 0x200u));
		Append(streams[ORIG_SECTION_INDEX], MakeSection(0x1000u, 0x400u, 0x400u));
		return streams;
	}

	void AppendDiagnosticDbi(Bytes& dbi, unsigned contributionSize, unsigned fromSrc,
		unsigned toSrc = PDB::NilStreamIndex, unsigned orig = ORIG_SECTION_INDEX,
		unsigned debugHeaderSize = static_cast<unsigned>(sizeof(PDB::DBI::DebugHeader)))
	{
		Append(dbi, MakeDbiHeader(contributionSize, debugHeaderSize));
		if (contributionSize != 0u)
		{
			Append(dbi, PDB::DBI::SectionContribution::Version::Ver60);
		}
		PutU32(dbi, 0u);
		PutU32(dbi, 0u);
		AppendFullDebugHeader(dbi, toSrc, fromSrc, SECTION_INDEX, orig);
	}
}

static int case_half_omap_row_rejected(void)
{
	std::vector<Bytes> streams = BaseDiagnosticStreams();
	AppendDiagnosticDbi(streams[DBI_INDEX], 4u, OMAP_FROM_INDEX);
	PutOmapRow(streams[OMAP_FROM_INDEX], 0x1000u, 0x5000u);
	PutU32(streams[OMAP_FROM_INDEX], 0x1100u);		// trailing half row: 12 bytes total

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(!built.ok);
	TASK_CHECK(HasDiagnostic(built.diagnostics, PDB::Provenance::DiagnosticKind::OmapHalfRow,
		OMAP_FROM_INDEX, 8ull));
	return 0;
}

static int case_unsorted_omap_rejected(void)
{
	std::vector<Bytes> streams = BaseDiagnosticStreams();
	AppendDiagnosticDbi(streams[DBI_INDEX], 4u, OMAP_FROM_INDEX);
	PutOmapRow(streams[OMAP_FROM_INDEX], 0x2000u, 0x5000u);
	PutOmapRow(streams[OMAP_FROM_INDEX], 0x1000u, 0x6000u);		// source RVA goes backwards

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(!built.ok);
	TASK_CHECK(HasDiagnostic(built.diagnostics, PDB::Provenance::DiagnosticKind::OmapNotStrictlyAscending,
		OMAP_FROM_INDEX, 8ull));
	return 0;
}

static int case_corrupt_contribution_table_rejected(void)
{
	std::vector<Bytes> streams = BaseDiagnosticStreams();
	const unsigned corruptSize = 4u
		+ static_cast<unsigned>(sizeof(PDB::DBI::SectionContribution)) + 12u;
	AppendDiagnosticDbi(streams[DBI_INDEX], corruptSize, PDB::NilStreamIndex,
		PDB::NilStreamIndex, PDB::NilStreamIndex);
	AppendContribution(streams[DBI_INDEX], 1u, 0u, 0x10u, 1u);
	for (unsigned i = 0u; i < 12u; ++i)
	{
		streams[DBI_INDEX].push_back(0u);		// trailing partial record
	}

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(!built.ok);
	TASK_CHECK(HasDiagnostic(built.diagnostics, PDB::Provenance::DiagnosticKind::ContributionTableCorrupt,
		DBI_INDEX, static_cast<unsigned long long>(sizeof(PDB::DBI::StreamHeader)) + 4ull));
	return 0;
}

static int case_section_stream_index_out_of_range(void)
{
	std::vector<Bytes> streams(9u);
	Append(streams[ORIG_SECTION_INDEX], MakeSection(0x1000u, 0x100u, 0x100u));
	AppendDiagnosticDbi(streams[DBI_INDEX], 4u, OMAP_FROM_INDEX, PDB::NilStreamIndex,
		ORIG_SECTION_INDEX);
	// slot 5 (SectionHdr) points at stream 0xfffe through the full debug header
	PDB::DBI::DebugHeader debug {};
	for (unsigned i = 0u; i < 11u; ++i)
	{
		(&debug.fpoDataStreamIndex)[i] = PDB::NilStreamIndex;
	}
	debug.sectionHeaderStreamIndex = 0xfffe;
	debug.omapFromSrcDataStreamIndex = static_cast<uint16_t>(OMAP_FROM_INDEX);
	debug.originalSectionHeaderDataStreamIndex = static_cast<uint16_t>(ORIG_SECTION_INDEX);
	// replace the debug header appended by the helper with this customized one
	const std::size_t headerPos = streams[DBI_INDEX].size() - sizeof(PDB::DBI::DebugHeader);
	std::memcpy(streams[DBI_INDEX].data() + headerPos, &debug, sizeof(debug));

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(!built.ok);
	TASK_CHECK(HasDiagnostic(built.diagnostics, PDB::Provenance::DiagnosticKind::StreamIndexOutOfRange,
		0xfffe, 0ull));
	return 0;
}

static int case_omap_to_src_only_unsupported(void)
{
	std::vector<Bytes> streams = BaseDiagnosticStreams();
	AppendDiagnosticDbi(streams[DBI_INDEX], 4u, PDB::NilStreamIndex, OMAP_TO_INDEX);
	PutOmapRow(streams[OMAP_TO_INDEX], 0x5000u, 0x1000u);

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(!built.ok);
	TASK_CHECK(HasDiagnostic(built.diagnostics, PDB::Provenance::DiagnosticKind::OmapToSrcOnlyUnsupported,
		DBI_INDEX, static_cast<unsigned long long>(sizeof(PDB::DBI::StreamHeader))
		+ 4ull + 4ull + 4ull + 3ull * sizeof(uint16_t)));
	return 0;
}

static int case_overlapping_sections_rejected(void)
{
	std::vector<Bytes> streams(9u);
	Bytes& sections = streams[SECTION_INDEX];
	Append(sections, MakeSection(0x1000u, 0x100u, 0x100u));
	Append(sections, MakeSection(0x1080u, 0x100u, 0x100u));		// overlaps the first section
	AppendDiagnosticDbi(streams[DBI_INDEX], 4u, PDB::NilStreamIndex, PDB::NilStreamIndex,
		PDB::NilStreamIndex);

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(!built.ok);
	TASK_CHECK(HasDiagnostic(built.diagnostics, PDB::Provenance::DiagnosticKind::SectionsOverlap,
		SECTION_INDEX, sizeof(PDB::IMAGE_SECTION_HEADER)));
	return 0;
}

static int case_contribution_out_of_range_rejected(void)
{
	{
		std::vector<Bytes> streams = BaseDiagnosticStreams();
		AppendDiagnosticDbi(streams[DBI_INDEX],
			4u + static_cast<unsigned>(sizeof(PDB::DBI::SectionContribution)), PDB::NilStreamIndex,
			PDB::NilStreamIndex, PDB::NilStreamIndex);
		AppendContribution(streams[DBI_INDEX], 9u, 0u, 0x10u, 1u);		// section 9 does not exist

		const Msf msf = BuildMsf(streams);
		PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
		PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
		TASK_CHECK(!built.ok);
		TASK_CHECK(HasDiagnostic(built.diagnostics, PDB::Provenance::DiagnosticKind::ContributionOutOfRange,
			DBI_INDEX, static_cast<unsigned long long>(sizeof(PDB::DBI::StreamHeader)) + 4ull));
	}

	{
		std::vector<Bytes> streams = BaseDiagnosticStreams();
		AppendDiagnosticDbi(streams[DBI_INDEX],
			4u + static_cast<unsigned>(sizeof(PDB::DBI::SectionContribution)), PDB::NilStreamIndex,
			PDB::NilStreamIndex, PDB::NilStreamIndex);
		AppendContribution(streams[DBI_INDEX], 1u, 0x300u, 0x200u, 1u);	// leaves the 0x400 section

		const Msf msf = BuildMsf(streams);
		PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
		PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
		TASK_CHECK(!built.ok);
		TASK_CHECK(!built.diagnostics.empty());
		TASK_CHECK(built.diagnostics.front().kind == PDB::Provenance::DiagnosticKind::ContributionOutOfRange);
	}
	return 0;
}

static int case_section_and_omap_integer_overflow_rejected(void)
{
	{
		// VA + effective size leaves the 32-bit RVA space
		std::vector<Bytes> streams(9u);
		Append(streams[SECTION_INDEX], MakeSection(0xfffff000u, 0x2000u, 0x2000u));
		AppendDiagnosticDbi(streams[DBI_INDEX], 4u, PDB::NilStreamIndex, PDB::NilStreamIndex,
			PDB::NilStreamIndex);

		const Msf msf = BuildMsf(streams);
		PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
		PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
		TASK_CHECK(!built.ok);
		TASK_CHECK(HasDiagnostic(built.diagnostics, PDB::Provenance::DiagnosticKind::SectionRangeOverflow,
			SECTION_INDEX, 0ull));
	}

	{
		// forward target arithmetic leaves the 32-bit RVA space; all intermediates are 64-bit
		std::vector<Bytes> streams = BaseDiagnosticStreams();
		// the original section must be large enough that the forward target runs past 4GB
		streams[ORIG_SECTION_INDEX].clear();
		Append(streams[ORIG_SECTION_INDEX], MakeSection(0x1000u, 0x2000u, 0x2000u));
		AppendDiagnosticDbi(streams[DBI_INDEX], 4u, OMAP_FROM_INDEX);
		PutOmapRow(streams[OMAP_FROM_INDEX], 0x1000u, 0xfffff000u);

		const Msf msf = BuildMsf(streams);
		PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
		PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
		TASK_CHECK(!built.ok);
		TASK_CHECK(HasDiagnostic(built.diagnostics, PDB::Provenance::DiagnosticKind::OmapTargetOverflow,
			OMAP_FROM_INDEX, 0ull));
	}
	return 0;
}

static int case_section_stream_bad_length_rejected(void)
{
	std::vector<Bytes> streams(9u);
	streams[SECTION_INDEX].resize(sizeof(PDB::IMAGE_SECTION_HEADER) + 7u);		// not a whole section
	AppendDiagnosticDbi(streams[DBI_INDEX], 4u, PDB::NilStreamIndex, PDB::NilStreamIndex,
		PDB::NilStreamIndex);

	const Msf msf = BuildMsf(streams);
	PDB::RawFile raw = PDB::CreateRawFile(msf.bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(raw);
	TASK_CHECK(!built.ok);
	TASK_CHECK(HasDiagnostic(built.diagnostics, PDB::Provenance::DiagnosticKind::SectionStreamLengthInvalid,
		SECTION_INDEX, sizeof(PDB::IMAGE_SECTION_HEADER) + 7ull));
	return 0;
}


// ================================================================================================
// registration
// ================================================================================================

extern const struct task_case task_cases[] = {
	{ "streams", "scattered_pages_and_full_debug_header", case_scattered_pages_and_full_debug_header },
	{ "streams", "short_optional_debug_header", case_short_optional_debug_header },
	{ "streams", "identity_fallback", case_identity_fallback },
	{ "streams", "empty_omap_from_stream", case_empty_omap_from_stream },

	{ "forward_spans", "deletion_and_unmapped_hole", case_deletion_and_unmapped_hole },
	{ "forward_spans", "cross_section_and_image_boundaries", case_cross_section_and_image_boundaries },
	{ "forward_spans", "overlapping_contributions_are_kept", case_overlapping_contributions_are_kept },
	{ "forward_spans", "virtual_size_zero_uses_raw_size", case_virtual_size_zero_uses_raw_size },

	{ "folded_origins", "non_monotone_partial_fold", case_non_monotone_partial_fold },
	{ "folded_origins", "reverse_lookup_finds_all_predecessors", case_reverse_lookup_finds_all_predecessors },
	{ "folded_origins", "contribution_identities_split_and_merge_spans", case_contribution_identities_split_and_merge_spans },
	{ "folded_origins", "index_survives_released_input", case_index_survives_released_input },
	{ "folded_origins", "reverse_across_image_section_boundary", case_reverse_across_image_section_boundary },

	{ "diagnostics", "half_omap_row_rejected", case_half_omap_row_rejected },
	{ "diagnostics", "unsorted_omap_rejected", case_unsorted_omap_rejected },
	{ "diagnostics", "corrupt_contribution_table_rejected", case_corrupt_contribution_table_rejected },
	{ "diagnostics", "section_stream_index_out_of_range", case_section_stream_index_out_of_range },
	{ "diagnostics", "omap_to_src_only_unsupported", case_omap_to_src_only_unsupported },
	{ "diagnostics", "overlapping_sections_rejected", case_overlapping_sections_rejected },
	{ "diagnostics", "contribution_out_of_range_rejected", case_contribution_out_of_range_rejected },
	{ "diagnostics", "section_and_omap_integer_overflow_rejected", case_section_and_omap_integer_overflow_rejected },
	{ "diagnostics", "section_stream_bad_length_rejected", case_section_stream_bad_length_rejected },
};

extern const std::size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
