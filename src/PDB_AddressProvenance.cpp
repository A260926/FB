// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#include "PDB_PCH.h"
#include "PDB_AddressProvenance.h"
#include "PDB_RawFile.h"
#include "PDB_DirectMSFStream.h"
#include "PDB_DBITypes.h"
#include "PDB_Types.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>


namespace
{
	using PDB::Provenance::Diagnostic;
	using PDB::Provenance::DiagnosticKind;

	static constexpr uint32_t DBI_STREAM_INDEX = 3u;
	static constexpr uint32_t NO_ROW = std::numeric_limits<uint32_t>::max();
	static constexpr uint64_t RVA_SPACE_END = 0x100000000ull;


	static inline bool PushDiagnostic(std::vector<Diagnostic>& diagnostics,
		DiagnosticKind kind, uint16_t streamIndex, uint64_t logicalOffset) PDB_NO_EXCEPT
	{
		diagnostics.push_back(Diagnostic { kind, streamIndex, logicalOffset });
		return false;
	}


	PDB_NO_DISCARD static inline uint64_t SectionEffectiveSize(const PDB::IMAGE_SECTION_HEADER& header) PDB_NO_EXCEPT
	{
		// the virtual size is authoritative when present; fall back to the raw size
		return (header.Misc.VirtualSize != 0u) ? header.Misc.VirtualSize : header.SizeOfRawData;
	}
}


// All owned, post-construction state of the provenance index.  Everything here is copied out
// of the RawFile, so the RawFile and its backing bytes can be released after construction.
struct PDB::Provenance::AddressProvenance::Impl
{
	struct SectionInfo
	{
		uint32_t virtualAddress;
		uint32_t effectiveSize;
		uint16_t oneBasedIndex;
	};

	struct OmapRow
	{
		uint32_t sourceRva;
		uint32_t targetRva;
	};

	struct ContributionRecord
	{
		uint64_t rvaStart;
		uint64_t rvaEnd;
		uint32_t ordinal;
		uint16_t section;
		uint16_t moduleIndex;
	};

	struct SourceSegment
	{
		uint64_t start;
		uint64_t end;
		uint64_t imageStart;
		uint64_t imageEnd;
		uint16_t section;					// 1-based original section
		uint32_t row;						// governing OMAP row, NO_ROW when unmapped/identity
		SpanStatus status;
		std::vector<uint32_t> contributionOrdinals;
		std::vector<uint32_t> fragmentIndices;	// image fragments produced from this segment
	};

	struct ImageFragment
	{
		uint64_t start;
		uint64_t end;
		uint32_t segmentIndex;
	};

	struct ImageAtom
	{
		uint64_t start;
		uint64_t end;
		std::vector<uint32_t> fragmentIndices;
	};

	bool identity = false;
	uint16_t omapFromStream = PDB::NilStreamIndex;
	bool omapToPresent = false;
	uint64_t lastRowDomainEnd = 0u;				// domain end of the final OMAP row (its section end)

	std::vector<SectionInfo> currentSections;		// sorted by RVA
	std::vector<SectionInfo> originalSections;		// sorted by RVA
	std::vector<OmapRow> rows;						// OMAP_FROM_SRC, strictly ascending
	std::vector<ContributionRecord> contributions;
	std::vector<SourceSegment> segments;			// atomic original-coordinate segments
	std::vector<ImageFragment> fragments;			// mapped fragments inside current sections
	std::vector<ImageAtom> imageAtoms;				// atomic image-coordinate segments
};


namespace
{
	using Impl = PDB::Provenance::AddressProvenance::Impl;


	// ------------------------------------------------------------------------------------------------
	// Section table parsing.  The returned vector is sorted by virtual address; the original
	// one-based section number is retained for contribution validation.
	// ------------------------------------------------------------------------------------------------
	PDB_NO_DISCARD static bool ParseSections(const PDB::RawFile& file, uint16_t streamIndex,
		std::vector<Impl::SectionInfo>& out, std::vector<Diagnostic>& diagnostics) PDB_NO_EXCEPT
	{
		if (streamIndex == PDB::NilStreamIndex)
		{
			return PushDiagnostic(diagnostics, DiagnosticKind::RequiredStreamMissing, streamIndex, 0u);
		}
		if (streamIndex >= file.GetStreamCount())
		{
			return PushDiagnostic(diagnostics, DiagnosticKind::StreamIndexOutOfRange, streamIndex, 0u);
		}

		const uint32_t streamSize = file.GetStreamSize(streamIndex);
		if ((streamSize % sizeof(PDB::IMAGE_SECTION_HEADER)) != 0u)
		{
			return PushDiagnostic(diagnostics, DiagnosticKind::SectionStreamLengthInvalid, streamIndex, streamSize);
		}

		PDB::DirectMSFStream stream = file.CreateMSFStream<PDB::DirectMSFStream>(streamIndex);
		const uint32_t count = streamSize / sizeof(PDB::IMAGE_SECTION_HEADER);

		struct RawSection
		{
			uint32_t virtualAddress;
			uint32_t effectiveSize;
			uint16_t oneBasedIndex;
		};
		std::vector<RawSection> raw;
		raw.reserve(count);

		for (uint32_t i = 0u; i < count; ++i)
		{
			const uint64_t logicalOffset = static_cast<uint64_t>(i) * sizeof(PDB::IMAGE_SECTION_HEADER);
			const PDB::IMAGE_SECTION_HEADER header = stream.ReadAtOffset<PDB::IMAGE_SECTION_HEADER>(static_cast<size_t>(logicalOffset));
			const uint64_t effectiveSize = SectionEffectiveSize(header);
			const uint64_t rangeEnd = static_cast<uint64_t>(header.VirtualAddress) + effectiveSize;
			if (rangeEnd > RVA_SPACE_END)
			{
				return PushDiagnostic(diagnostics, DiagnosticKind::SectionRangeOverflow, streamIndex, logicalOffset);
			}

			raw.push_back(RawSection { header.VirtualAddress, static_cast<uint32_t>(effectiveSize), static_cast<uint16_t>(i + 1u) });
		}

		std::sort(raw.begin(), raw.end(), [](const RawSection& lhs, const RawSection& rhs) PDB_NO_EXCEPT
		{
			return lhs.virtualAddress < rhs.virtualAddress;
		});

		// non-empty sections in the same coordinate system must not overlap
		for (size_t i = 1u; i < raw.size(); ++i)
		{
			if (raw[i - 1u].effectiveSize == 0u || raw[i].effectiveSize == 0u)
			{
				continue;
			}
			const uint64_t previousEnd = static_cast<uint64_t>(raw[i - 1u].virtualAddress) + raw[i - 1u].effectiveSize;
			if (previousEnd > raw[i].virtualAddress)
			{
				const uint64_t logicalOffset = static_cast<uint64_t>(raw[i].oneBasedIndex - 1u) * sizeof(PDB::IMAGE_SECTION_HEADER);
				return PushDiagnostic(diagnostics, DiagnosticKind::SectionsOverlap, streamIndex, logicalOffset);
			}
		}

		out.reserve(raw.size());
		for (const RawSection& section : raw)
		{
			out.push_back(Impl::SectionInfo { section.virtualAddress, section.effectiveSize, section.oneBasedIndex });
		}

		return true;
	}


	// ------------------------------------------------------------------------------------------------
	// OMAP_FROM_SRC parsing.
	// ------------------------------------------------------------------------------------------------
	PDB_NO_DISCARD static bool ParseOmapFrom(const PDB::RawFile& file, uint16_t streamIndex,
		std::vector<Impl::OmapRow>& out, std::vector<Diagnostic>& diagnostics) PDB_NO_EXCEPT
	{
		if (streamIndex == PDB::NilStreamIndex)
		{
			return PushDiagnostic(diagnostics, DiagnosticKind::RequiredStreamMissing, streamIndex, 0u);
		}
		if (streamIndex >= file.GetStreamCount())
		{
			return PushDiagnostic(diagnostics, DiagnosticKind::StreamIndexOutOfRange, streamIndex, 0u);
		}

		const uint32_t streamSize = file.GetStreamSize(streamIndex);
		if ((streamSize % 8u) != 0u)
		{
			// a trailing half row makes the stream unusable
			return PushDiagnostic(diagnostics, DiagnosticKind::OmapHalfRow, streamIndex, streamSize & ~7u);
		}

		PDB::DirectMSFStream stream = file.CreateMSFStream<PDB::DirectMSFStream>(streamIndex);
		const uint32_t rowCount = streamSize / 8u;
		out.reserve(rowCount);

		for (uint32_t i = 0u; i < rowCount; ++i)
		{
			const uint64_t logicalOffset = static_cast<uint64_t>(i) * 8u;
			const uint32_t sourceRva = stream.ReadAtOffset<uint32_t>(static_cast<size_t>(logicalOffset));
			const uint32_t targetRva = stream.ReadAtOffset<uint32_t>(static_cast<size_t>(logicalOffset + 4u));

			// the input RVAs must be strictly ascending; a deletion row (target == 0) is still
			// an ordinary row and must not be confused with a mapping to address 0.
			if (i != 0u && sourceRva <= out.back().sourceRva)
			{
				return PushDiagnostic(diagnostics, DiagnosticKind::OmapNotStrictlyAscending, streamIndex, logicalOffset);
			}

			out.push_back(Impl::OmapRow { sourceRva, targetRva });
		}

		return true;
	}


	// ------------------------------------------------------------------------------------------------
	// Ver60 section contribution parsing.
	// ------------------------------------------------------------------------------------------------
	PDB_NO_DISCARD static bool ParseContributions(const PDB::DirectMSFStream& dbiStream, uint32_t regionOffset,
		uint32_t regionSize, const std::vector<Impl::SectionInfo>& originalSections,
		std::vector<Impl::ContributionRecord>& out, std::vector<Diagnostic>& diagnostics) PDB_NO_EXCEPT
	{
		if (regionSize < sizeof(uint32_t))
		{
			return PushDiagnostic(diagnostics, DiagnosticKind::ContributionTableCorrupt, DBI_STREAM_INDEX, regionOffset);
		}

		const PDB::DBI::SectionContribution::Version version =
			dbiStream.ReadAtOffset<PDB::DBI::SectionContribution::Version>(regionOffset);
		if (version != PDB::DBI::SectionContribution::Version::Ver60)
		{
			return PushDiagnostic(diagnostics, DiagnosticKind::ContributionTableCorrupt, DBI_STREAM_INDEX, regionOffset);
		}

		const uint32_t payloadSize = regionSize - static_cast<uint32_t>(sizeof(uint32_t));
		if ((payloadSize % sizeof(PDB::DBI::SectionContribution)) != 0u)
		{
			// a trailing partial contribution record makes the table corrupt
			return PushDiagnostic(diagnostics, DiagnosticKind::ContributionTableCorrupt, DBI_STREAM_INDEX,
				regionOffset + sizeof(uint32_t));
		}

		const uint32_t recordCount = payloadSize / sizeof(PDB::DBI::SectionContribution);
		out.reserve(recordCount);

		for (uint32_t i = 0u; i < recordCount; ++i)
		{
			const uint64_t logicalOffset = static_cast<uint64_t>(regionOffset) + sizeof(uint32_t)
				+ static_cast<uint64_t>(i) * sizeof(PDB::DBI::SectionContribution);
			const PDB::DBI::SectionContribution record =
				dbiStream.ReadAtOffset<PDB::DBI::SectionContribution>(static_cast<size_t>(logicalOffset));

			// section numbers are 1-based indices into the pre-optimization (original) section table
			if (record.section == 0u || record.section > originalSections.size())
			{
				return PushDiagnostic(diagnostics, DiagnosticKind::ContributionOutOfRange, DBI_STREAM_INDEX, logicalOffset);
			}

			const Impl::SectionInfo& section = originalSections[record.section - 1u];
			const uint64_t rvaStart = static_cast<uint64_t>(section.virtualAddress) + record.offset;
			const uint64_t rvaEnd = rvaStart + record.size;
			const uint64_t sectionEnd = static_cast<uint64_t>(section.virtualAddress) + section.effectiveSize;
			if (rvaStart < section.virtualAddress || rvaEnd > sectionEnd)
			{
				return PushDiagnostic(diagnostics, DiagnosticKind::ContributionOutOfRange, DBI_STREAM_INDEX, logicalOffset);
			}

			out.push_back(Impl::ContributionRecord {
				rvaStart, rvaEnd, i, record.section, record.moduleIndex
			});
		}

		return true;
	}


	// Finds the last OMAP row whose source RVA is <= position; returns NO_ROW when position
	// precedes every row (unmapped).  O(log n), never scans the whole table.
	PDB_NO_DISCARD static uint32_t FindGoverningRow(const std::vector<Impl::OmapRow>& rows, uint64_t position) PDB_NO_EXCEPT
	{
		auto it = std::upper_bound(rows.begin(), rows.end(), position,
			[](uint64_t value, const Impl::OmapRow& row) PDB_NO_EXCEPT
			{
				return value < row.sourceRva;
			});
		if (it == rows.begin())
		{
			return NO_ROW;
		}
		return static_cast<uint32_t>(std::distance(rows.begin(), it - 1));
	}


	// ------------------------------------------------------------------------------------------------
	// Builds the OMAP evidence describing a segment's linear relationship.
	// ------------------------------------------------------------------------------------------------
	PDB_NO_DISCARD static PDB::Provenance::OmapEvidence MakeEvidence(const Impl& impl,
		const Impl::SourceSegment& segment) PDB_NO_EXCEPT
	{
		PDB::Provenance::OmapEvidence evidence;
		evidence.identity = impl.identity;
		evidence.streamIndex = impl.omapFromStream;
		evidence.row = NO_ROW;
		evidence.sourceRva = 0u;
		evidence.targetRva = 0u;

		if (impl.identity)
		{
			evidence.streamIndex = PDB::NilStreamIndex;
			evidence.sourceRva = static_cast<uint32_t>(segment.start);
			evidence.targetRva = static_cast<uint32_t>(segment.start);
		}
		else if (segment.row != NO_ROW)
		{
			const Impl::OmapRow& row = impl.rows[segment.row];
			evidence.row = segment.row;
			evidence.sourceRva = row.sourceRva;
			evidence.targetRva = row.targetRva;
		}
		return evidence;
	}


	PDB_NO_DISCARD static std::vector<PDB::Provenance::ContributionId> MakeContributionIds(
		const Impl& impl, const Impl::SourceSegment& segment) PDB_NO_EXCEPT
	{
		std::vector<PDB::Provenance::ContributionId> ids;
		ids.reserve(segment.contributionOrdinals.size());
		for (uint32_t ordinal : segment.contributionOrdinals)
		{
			const Impl::ContributionRecord& contribution = impl.contributions[ordinal];
			ids.push_back(PDB::Provenance::ContributionId {
				contribution.ordinal, contribution.moduleIndex, contribution.section
			});
		}
		return ids;
	}
}


// ------------------------------------------------------------------------------------------------
// Lifetime
// ------------------------------------------------------------------------------------------------
PDB::Provenance::AddressProvenance::AddressProvenance(void) PDB_NO_EXCEPT
	: m_impl(std::make_unique<Impl>())
{
}


PDB::Provenance::AddressProvenance::~AddressProvenance(void) PDB_NO_EXCEPT = default;


PDB::Provenance::AddressProvenance::AddressProvenance(AddressProvenance&& other) PDB_NO_EXCEPT = default;


PDB::Provenance::AddressProvenance& PDB::Provenance::AddressProvenance::operator=(AddressProvenance&& other) PDB_NO_EXCEPT = default;


// ------------------------------------------------------------------------------------------------
// Accessors
// ------------------------------------------------------------------------------------------------
PDB_NO_DISCARD bool PDB::Provenance::AddressProvenance::IsIdentity(void) const PDB_NO_EXCEPT
{
	return m_impl->identity;
}

PDB_NO_DISCARD uint16_t PDB::Provenance::AddressProvenance::GetOmapFromStreamIndex(void) const PDB_NO_EXCEPT
{
	return m_impl->omapFromStream;
}

PDB_NO_DISCARD bool PDB::Provenance::AddressProvenance::HasOmapToSrc(void) const PDB_NO_EXCEPT
{
	return m_impl->omapToPresent;
}

PDB_NO_DISCARD size_t PDB::Provenance::AddressProvenance::GetOmapRowCount(void) const PDB_NO_EXCEPT
{
	return m_impl->rows.size();
}

PDB_NO_DISCARD size_t PDB::Provenance::AddressProvenance::GetCurrentSectionCount(void) const PDB_NO_EXCEPT
{
	return m_impl->currentSections.size();
}

PDB_NO_DISCARD size_t PDB::Provenance::AddressProvenance::GetOriginalSectionCount(void) const PDB_NO_EXCEPT
{
	return m_impl->originalSections.size();
}

PDB_NO_DISCARD size_t PDB::Provenance::AddressProvenance::GetContributionCount(void) const PDB_NO_EXCEPT
{
	return m_impl->contributions.size();
}


// ------------------------------------------------------------------------------------------------
// Construction
// ------------------------------------------------------------------------------------------------
PDB::Provenance::BuildResult PDB::Provenance::BuildAddressProvenance(const RawFile& file) PDB_NO_EXCEPT
{
	BuildResult result;
	result.ok = false;

	auto impl = std::make_unique<Impl>();

	// --- DBI stream (always stream 3) ---------------------------------------------------------
	if (DBI_STREAM_INDEX >= file.GetStreamCount())
	{
		PushDiagnostic(result.diagnostics, DiagnosticKind::RequiredStreamMissing,
			static_cast<uint16_t>(DBI_STREAM_INDEX), 0u);
		return result;
	}

	DirectMSFStream dbiStream = file.CreateMSFStream<DirectMSFStream>(DBI_STREAM_INDEX);
	const uint64_t dbiSize = dbiStream.GetSize();
	if (dbiSize < sizeof(DBI::StreamHeader))
	{
		PushDiagnostic(result.diagnostics, DiagnosticKind::InvalidDBIHeader, DBI_STREAM_INDEX, dbiSize);
		return result;
	}

	const DBI::StreamHeader dbiHeader = dbiStream.ReadAtOffset<DBI::StreamHeader>(0u);
	if (dbiHeader.signature != DBI::StreamHeader::Signature)
	{
		PushDiagnostic(result.diagnostics, DiagnosticKind::InvalidDBIVersion, DBI_STREAM_INDEX, 0u);
		return result;
	}
	if (dbiHeader.version != DBI::StreamHeader::Version::V70)
	{
		PushDiagnostic(result.diagnostics, DiagnosticKind::InvalidDBIVersion, DBI_STREAM_INDEX,
			offsetof(DBI::StreamHeader, version));
		return result;
	}

	// --- sub-stream layout; every sum is computed in 64 bits ----------------------------------
	const uint64_t moduleInfoOffset = sizeof(DBI::StreamHeader);
	const uint64_t contributionOffset = moduleInfoOffset + dbiHeader.moduleInfoSize;
	const uint64_t sectionMapOffset = contributionOffset + dbiHeader.sectionContributionSize;
	const uint64_t sourceInfoOffset = sectionMapOffset + dbiHeader.sectionMapSize;
	const uint64_t typeServerMapOffset = sourceInfoOffset + dbiHeader.sourceInfoSize;
	const uint64_t ecOffset = typeServerMapOffset + dbiHeader.typeServerMapSize;
	const uint64_t debugHeaderOffset = ecOffset + dbiHeader.ecSize;

	if (contributionOffset > dbiSize || debugHeaderOffset > dbiSize)
	{
		PushDiagnostic(result.diagnostics, DiagnosticKind::InvalidDBIHeader, DBI_STREAM_INDEX, debugHeaderOffset);
		return result;
	}

	// --- optional debug header: read only the slots that actually exist.  A short header is
	// legal; missing slots are treated as 0xffff (absent stream) rather than as an error. -------
	uint16_t debugSlots[11u];
	for (uint16_t& slot : debugSlots)
	{
		slot = DBI::DebugHeader::InvalidStreamIndex;
	}
	if (dbiHeader.optionalDebugHeaderSize != 0u)
	{
		const uint64_t available = (debugHeaderOffset < dbiSize) ? (dbiSize - debugHeaderOffset) : 0u;
		const uint64_t availableSlots = std::min<uint64_t>(available / sizeof(uint16_t), 11u);
		for (uint64_t slot = 0u; slot < availableSlots; ++slot)
		{
			debugSlots[slot] = dbiStream.ReadAtOffset<uint16_t>(
				static_cast<size_t>(debugHeaderOffset + slot * sizeof(uint16_t)));
		}
	}

	const uint16_t omapToSrcStream = debugSlots[3u];
	const uint16_t omapFromSrcStream = debugSlots[4u];
	const uint16_t sectionHeaderStream = debugSlots[5u];
	const uint16_t originalSectionHeaderStream = debugSlots[10u];

	const bool fromPresent = (omapFromSrcStream != DBI::DebugHeader::InvalidStreamIndex);
	impl->omapToPresent = (omapToSrcStream != DBI::DebugHeader::InvalidStreamIndex);

	// OMAP_TO_SRC without OMAP_FROM_SRC cannot be turned into a complete inverse map.
	if (!fromPresent && impl->omapToPresent)
	{
		PushDiagnostic(result.diagnostics, DiagnosticKind::OmapToSrcOnlyUnsupported, DBI_STREAM_INDEX,
			debugHeaderOffset + 3u * sizeof(uint16_t));
		return result;
	}

	// --- current section table is always required ----------------------------------------------
	if (!ParseSections(file, sectionHeaderStream, impl->currentSections, result.diagnostics))
	{
		return result;
	}

	if (!fromPresent)
	{
		// identity fallback: no OMAP at all, the current table is also the original table
		impl->identity = true;
		impl->omapFromStream = NilStreamIndex;
		impl->originalSections = impl->currentSections;
	}
	else
	{
		impl->identity = false;
		impl->omapFromStream = omapFromSrcStream;

		// a forward map without the pre-optimization section table is meaningless
		if (!ParseSections(file, originalSectionHeaderStream, impl->originalSections, result.diagnostics))
		{
			return result;
		}
		if (!ParseOmapFrom(file, omapFromSrcStream, impl->rows, result.diagnostics))
		{
			return result;
		}

		// validate that every forward target stays inside the 32-bit RVA space.
		// the domain of the last row ends at the end of the original section that contains it.
		uint64_t originalSpaceEnd = 0u;
		for (const Impl::SectionInfo& section : impl->originalSections)
		{
			originalSpaceEnd = std::max<uint64_t>(originalSpaceEnd,
				static_cast<uint64_t>(section.virtualAddress) + section.effectiveSize);
		}
		if (!impl->rows.empty())
		{
			impl->lastRowDomainEnd = originalSpaceEnd;
			const uint32_t lastSourceRva = impl->rows.back().sourceRva;
			for (const Impl::SectionInfo& section : impl->originalSections)
			{
				const uint64_t sectionStart = section.virtualAddress;
				const uint64_t sectionEnd = sectionStart + section.effectiveSize;
				if (lastSourceRva >= sectionStart && lastSourceRva < sectionEnd)
				{
					impl->lastRowDomainEnd = sectionEnd;
					break;
				}
			}
		}
		for (size_t i = 0u; i < impl->rows.size(); ++i)
		{
			const Impl::OmapRow& row = impl->rows[i];
			if (row.targetRva == 0u)
			{
				continue;
			}
			const uint64_t domainEnd = (i + 1u < impl->rows.size())
				? impl->rows[i + 1u].sourceRva
				: impl->lastRowDomainEnd;
			if (domainEnd <= row.sourceRva)
			{
				continue;
			}
			const uint64_t targetEnd = static_cast<uint64_t>(row.targetRva) + (domainEnd - row.sourceRva);
			if (targetEnd > RVA_SPACE_END)
			{
				PushDiagnostic(result.diagnostics, DiagnosticKind::OmapTargetOverflow, omapFromSrcStream,
					static_cast<uint64_t>(i) * 8u);
				return result;
			}
		}
	}

	// --- Ver60 section contributions -----------------------------------------------------------
	if (dbiHeader.sectionContributionSize != 0u)
	{
		if (contributionOffset + dbiHeader.sectionContributionSize > dbiSize)
		{
			PushDiagnostic(result.diagnostics, DiagnosticKind::ContributionTableCorrupt, DBI_STREAM_INDEX, contributionOffset);
			return result;
		}
		if (!ParseContributions(dbiStream, static_cast<uint32_t>(contributionOffset),
			dbiHeader.sectionContributionSize, impl->originalSections, impl->contributions, result.diagnostics))
		{
			return result;
		}
	}

	// --- build atomic original-coordinate segments ---------------------------------------------
	// Contributions grouped by their 1-based original section, in ordinal (record) order.
	std::vector<std::vector<uint32_t>> contributionsBySection(impl->originalSections.size() + 1u);
	for (uint32_t i = 0u; i < impl->contributions.size(); ++i)
	{
		contributionsBySection[impl->contributions[i].section].push_back(i);
	}

	for (size_t s = 0u; s < impl->originalSections.size(); ++s)
	{
		const Impl::SectionInfo& section = impl->originalSections[s];
		if (section.effectiveSize == 0u)
		{
			// empty sections have no addressable content
			continue;
		}
		const uint64_t sectionStart = section.virtualAddress;
		const uint64_t sectionEnd = sectionStart + section.effectiveSize;

		std::vector<uint64_t> bounds;
		bounds.push_back(sectionStart);
		bounds.push_back(sectionEnd);

		// OMAP row boundaries landing inside this section
		for (const Impl::OmapRow& row : impl->rows)
		{
			if (row.sourceRva > sectionStart && row.sourceRva < sectionEnd)
			{
				bounds.push_back(row.sourceRva);
			}
		}
		// contribution boundaries, clamped to the section
		for (uint32_t ordinal : contributionsBySection[section.oneBasedIndex])
		{
			const Impl::ContributionRecord& contribution = impl->contributions[ordinal];
			bounds.push_back(std::max(sectionStart, std::min(sectionEnd, contribution.rvaStart)));
			bounds.push_back(std::max(sectionStart, std::min(sectionEnd, contribution.rvaEnd)));
		}

		std::sort(bounds.begin(), bounds.end());
		bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

		for (size_t b = 0u; b + 1u < bounds.size(); ++b)
		{
			const uint64_t segmentStart = bounds[b];
			const uint64_t segmentEnd = bounds[b + 1u];
			if (segmentStart == segmentEnd)
			{
				continue;
			}

			Impl::SourceSegment segment;
			segment.start = segmentStart;
			segment.end = segmentEnd;
			segment.section = section.oneBasedIndex;
			segment.imageStart = 0u;
			segment.imageEnd = 0u;
			segment.row = NO_ROW;
			segment.status = SpanStatus::Unmapped;

			if (impl->identity)
			{
				segment.status = SpanStatus::Mapped;
				segment.imageStart = segmentStart;
				segment.imageEnd = segmentEnd;
			}
			else
			{
				uint32_t rowIndex = FindGoverningRow(impl->rows, segmentStart);
				// the final row only extends to the end of its own original section; beyond
				// that boundary there is no applicable mapping, so later sections are unmapped.
				if (rowIndex != NO_ROW && rowIndex + 1u == impl->rows.size()
					&& segmentStart >= impl->lastRowDomainEnd)
				{
					rowIndex = NO_ROW;
				}
				segment.row = rowIndex;
				if (rowIndex == NO_ROW)
				{
					// before the first applicable row: unmapped, never a mapping to zero
					segment.status = SpanStatus::Unmapped;
				}
				else if (impl->rows[rowIndex].targetRva == 0u)
				{
					segment.status = SpanStatus::Deleted;
				}
				else
				{
					segment.status = SpanStatus::Mapped;
					const Impl::OmapRow& row = impl->rows[rowIndex];
					segment.imageStart = static_cast<uint64_t>(row.targetRva) + (segmentStart - row.sourceRva);
					segment.imageEnd = static_cast<uint64_t>(row.targetRva) + (segmentEnd - row.sourceRva);
				}
			}

			impl->segments.push_back(std::move(segment));
		}
	}

	// Attach contributions to the segments they cover.  Iterating records in ordinal order
	// keeps every segment's identity list in contribution record order, with no deduplication.
	std::vector<size_t> segmentSectionStart(impl->originalSections.size() + 1u, 0u);
	{
		size_t cursor = 0u;
		uint16_t currentSection = 0u;
		for (const Impl::SourceSegment& segment : impl->segments)
		{
			while (currentSection < segment.section)
			{
				segmentSectionStart[currentSection] = cursor;
				++currentSection;
			}
			++cursor;
		}
		while (currentSection <= impl->originalSections.size())
		{
			segmentSectionStart[currentSection] = cursor;
			++currentSection;
		}
	}

	for (uint32_t ordinal = 0u; ordinal < impl->contributions.size(); ++ordinal)
	{
		const Impl::ContributionRecord& contribution = impl->contributions[ordinal];
		const size_t begin = segmentSectionStart[contribution.section - 1u];
		const size_t end = segmentSectionStart[contribution.section];

		auto first = std::lower_bound(impl->segments.begin() + static_cast<std::ptrdiff_t>(begin),
			impl->segments.begin() + static_cast<std::ptrdiff_t>(end), contribution.rvaStart,
			[](const Impl::SourceSegment& segment, uint64_t value) PDB_NO_EXCEPT
			{
				return segment.end <= value;
			});
		for (auto it = first; it != impl->segments.begin() + static_cast<std::ptrdiff_t>(end); ++it)
		{
			if (it->start >= contribution.rvaEnd)
			{
				break;
			}
			it->contributionOrdinals.push_back(ordinal);
		}
	}

	// --- cut mapped projections at current image section boundaries ----------------------------
	for (uint32_t segmentIndex = 0u; segmentIndex < impl->segments.size(); ++segmentIndex)
	{
		Impl::SourceSegment& segment = impl->segments[segmentIndex];
		if (segment.status != SpanStatus::Mapped)
		{
			continue;
		}

		for (const Impl::SectionInfo& imageSection : impl->currentSections)
		{
			if (imageSection.effectiveSize == 0u)
			{
				continue;
			}
			const uint64_t sectionStart = imageSection.virtualAddress;
			const uint64_t sectionEnd = sectionStart + imageSection.effectiveSize;
			const uint64_t overlapStart = std::max(sectionStart, segment.imageStart);
			const uint64_t overlapEnd = std::min(sectionEnd, segment.imageEnd);
			if (overlapStart < overlapEnd)
			{
				const uint32_t fragmentIndex = static_cast<uint32_t>(impl->fragments.size());
				impl->fragments.push_back(Impl::ImageFragment { overlapStart, overlapEnd, segmentIndex });
				segment.fragmentIndices.push_back(fragmentIndex);
			}
		}
	}

	// --- build the reverse image-coordinate index ----------------------------------------------
	std::vector<uint64_t> imageBounds;
	imageBounds.reserve(impl->fragments.size() * 2u);
	for (const Impl::ImageFragment& fragment : impl->fragments)
	{
		imageBounds.push_back(fragment.start);
		imageBounds.push_back(fragment.end);
	}
	std::sort(imageBounds.begin(), imageBounds.end());
	imageBounds.erase(std::unique(imageBounds.begin(), imageBounds.end()), imageBounds.end());

	impl->imageAtoms.reserve(imageBounds.size() >= 1u ? imageBounds.size() - 1u : 0u);
	for (size_t i = 0u; i + 1u < imageBounds.size(); ++i)
	{
		impl->imageAtoms.push_back(Impl::ImageAtom { imageBounds[i], imageBounds[i + 1u], {} });
	}

	for (uint32_t fragmentIndex = 0u; fragmentIndex < impl->fragments.size(); ++fragmentIndex)
	{
		const Impl::ImageFragment& fragment = impl->fragments[fragmentIndex];
		auto first = std::lower_bound(impl->imageAtoms.begin(), impl->imageAtoms.end(), fragment.start,
			[](const Impl::ImageAtom& atom, uint64_t value) PDB_NO_EXCEPT
			{
				return atom.end <= value;
			});
		for (auto it = first; it != impl->imageAtoms.end() && it->start < fragment.end; ++it)
		{
			it->fragmentIndices.push_back(fragmentIndex);
		}
	}

	// deterministic origin order inside every atom: by original position, fragment index as tie-break
	for (Impl::ImageAtom& atom : impl->imageAtoms)
	{
		std::sort(atom.fragmentIndices.begin(), atom.fragmentIndices.end(),
			[&](uint32_t lhs, uint32_t rhs) PDB_NO_EXCEPT
			{
				const Impl::ImageFragment& leftFragment = impl->fragments[lhs];
				const Impl::ImageFragment& rightFragment = impl->fragments[rhs];
				const Impl::SourceSegment& leftSegment = impl->segments[leftFragment.segmentIndex];
				const Impl::SourceSegment& rightSegment = impl->segments[rightFragment.segmentIndex];
				if (leftSegment.start != rightSegment.start)
				{
					return leftSegment.start < rightSegment.start;
				}
				return lhs < rhs;
			});
	}

	result.ok = true;
	result.index.m_impl = std::move(impl);
	return result;
}


// ------------------------------------------------------------------------------------------------
// Original interval projection
// ------------------------------------------------------------------------------------------------
std::vector<PDB::Provenance::ForwardSpan> PDB::Provenance::AddressProvenance::ProjectOriginal(AddressRange range) const PDB_NO_EXCEPT
{
	const Impl& impl = *m_impl;
	std::vector<ForwardSpan> result;
	if (range.start >= range.end)
	{
		return result;
	}

	const OmapEvidence noEvidence { NilStreamIndex, NO_ROW, 0u, 0u, false };
	uint64_t cursor = range.start;

	// jump straight to the first non-empty section that can contain range.start (or follows it),
	// so sections entirely below the query are never visited.
	auto sectionIt = std::lower_bound(impl.originalSections.begin(), impl.originalSections.end(), range.start,
		[](const Impl::SectionInfo& section, uint64_t value) PDB_NO_EXCEPT
		{
			return static_cast<uint64_t>(section.virtualAddress) + section.effectiveSize <= value;
		});

	// walk the original sections in RVA order; gaps between/around sections are outside_section
	for (; sectionIt != impl.originalSections.end(); ++sectionIt)
	{
		const Impl::SectionInfo& section = *sectionIt;
		if (section.effectiveSize == 0u)
		{
			continue;
		}
		const uint64_t sectionStart = section.virtualAddress;
		const uint64_t sectionEnd = sectionStart + section.effectiveSize;
		if (sectionEnd <= cursor)
		{
			continue;
		}
		if (sectionStart >= range.end)
		{
			break;
		}

		if (cursor < sectionStart)
		{
			const uint64_t holeEnd = std::min(sectionStart, range.end);
			result.push_back(ForwardSpan { { cursor, holeEnd }, { 0u, 0u }, SpanStatus::OutsideSection, 0u, noEvidence, {} });
			cursor = holeEnd;
			if (cursor == range.end)
			{
				return result;
			}
		}

		const uint64_t queryEnd = std::min(range.end, sectionEnd);

		// locate the first segment at or after cursor via binary search; segments are in
		// original-RVA order and partition every non-empty section
		auto segmentIt = std::lower_bound(impl.segments.begin(), impl.segments.end(), cursor,
			[](const Impl::SourceSegment& segment, uint64_t value) PDB_NO_EXCEPT
			{
				return segment.end <= value;
			});

		for (; segmentIt != impl.segments.end() && segmentIt->start < queryEnd; ++segmentIt)
		{
			const Impl::SourceSegment& segment = *segmentIt;
			if (segment.section != section.oneBasedIndex)
			{
				continue;
			}
			const uint64_t spanStart = std::max(cursor, segment.start);
			const uint64_t spanEnd = std::min(queryEnd, segment.end);
			if (spanStart >= spanEnd)
			{
				continue;
			}
			cursor = spanEnd;

			const OmapEvidence evidence = MakeEvidence(impl, segment);
			std::vector<ContributionId> ids = MakeContributionIds(impl, segment);

			if (segment.status != SpanStatus::Mapped)
			{
				result.push_back(ForwardSpan { { spanStart, spanEnd }, { 0u, 0u }, segment.status,
					segment.section, evidence, std::move(ids) });
				continue;
			}

			// map the queried sub-range through the segment's linear relationship, then cut
			// at current image section boundaries using the segment's precomputed fragments
			const uint64_t imageSpanStart = segment.imageStart + (spanStart - segment.start);
			const uint64_t imageSpanEnd = segment.imageStart + (spanEnd - segment.start);

			uint64_t imageCursor = imageSpanStart;
			for (uint32_t fragmentIndex : segment.fragmentIndices)
			{
				const Impl::ImageFragment& fragment = impl.fragments[fragmentIndex];
				const uint64_t hitStart = std::max(imageCursor, fragment.start);
				const uint64_t hitEnd = std::min(imageSpanEnd, fragment.end);
				if (hitStart >= hitEnd)
				{
					continue;
				}

				if (imageCursor < hitStart)
				{
					// image sub-interval outside every current section
					const uint64_t originalGapStart = spanStart + (imageCursor - imageSpanStart);
					const uint64_t originalGapEnd = spanStart + (hitStart - imageSpanStart);
					result.push_back(ForwardSpan { { originalGapStart, originalGapEnd }, { 0u, 0u },
						SpanStatus::OutsideImage, segment.section, evidence, ids });
				}

				const uint64_t originalHitStart = spanStart + (hitStart - imageSpanStart);
				const uint64_t originalHitEnd = spanStart + (hitEnd - imageSpanStart);
				result.push_back(ForwardSpan { { originalHitStart, originalHitEnd }, { hitStart, hitEnd },
					SpanStatus::Mapped, segment.section, evidence, ids });
				imageCursor = hitEnd;
			}
			if (imageCursor < imageSpanEnd)
			{
				const uint64_t originalGapStart = spanStart + (imageCursor - imageSpanStart);
				result.push_back(ForwardSpan { { originalGapStart, spanEnd }, { 0u, 0u },
					SpanStatus::OutsideImage, segment.section, evidence, std::move(ids) });
			}
		}
	}

	if (cursor < range.end)
	{
		result.push_back(ForwardSpan { { cursor, range.end }, { 0u, 0u }, SpanStatus::OutsideSection, 0u, noEvidence, {} });
	}

	return result;
}


// ------------------------------------------------------------------------------------------------
// Image interval interpretation
// ------------------------------------------------------------------------------------------------
std::vector<PDB::Provenance::ImageSpan> PDB::Provenance::AddressProvenance::InterpretImage(AddressRange range) const PDB_NO_EXCEPT
{
	const Impl& impl = *m_impl;
	if (range.start >= range.end)
	{
		return std::vector<ImageSpan> {};
	}

	std::vector<ImageSpan> result;

	// locate the first atom overlapping the query start; atoms partition the mapped image space
	auto atomIt = std::lower_bound(impl.imageAtoms.begin(), impl.imageAtoms.end(), range.start,
		[](const Impl::ImageAtom& atom, uint64_t value) PDB_NO_EXCEPT
		{
			return atom.end <= value;
		});

	// two adjacent spans may merge only when every source identity and its linear relationship
	// (identity map, or the same OMAP row) and the contribution identities are identical.
	auto sameOrigins = [&impl](const ImageSpan& span, const Impl::ImageAtom& atom) -> bool
	{
		if (span.origins.size() != atom.fragmentIndices.size())
		{
			return false;
		}
		for (size_t i = 0u; i < atom.fragmentIndices.size(); ++i)
		{
			const Impl::ImageFragment& fragment = impl.fragments[atom.fragmentIndices[i]];
			const Impl::SourceSegment& segment = impl.segments[fragment.segmentIndex];
			const ImageOrigin& origin = span.origins[i];
			if (origin.originalSection != segment.section || origin.evidence.row != segment.row
				|| origin.evidence.identity != impl.identity)
			{
				return false;
			}
			if (origin.contributions.size() != segment.contributionOrdinals.size())
			{
				return false;
			}
			for (size_t c = 0u; c < segment.contributionOrdinals.size(); ++c)
			{
				if (origin.contributions[c].ordinal != impl.contributions[segment.contributionOrdinals[c]].ordinal)
				{
					return false;
				}
			}
		}
		return true;
	};

	for (; atomIt != impl.imageAtoms.end() && atomIt->start < range.end; ++atomIt)
	{
		const Impl::ImageAtom& atom = *atomIt;
		const uint64_t spanStart = std::max(range.start, atom.start);
		const uint64_t spanEnd = std::min(range.end, atom.end);
		if (spanStart >= spanEnd)
		{
			continue;
		}

		ImageSpan span;
		span.image = { spanStart, spanEnd };

		for (uint32_t fragmentIndex : atom.fragmentIndices)
		{
			const Impl::ImageFragment& fragment = impl.fragments[fragmentIndex];
			if (fragment.end <= spanStart || fragment.start >= spanEnd)
			{
				continue;
			}
			const Impl::SourceSegment& segment = impl.segments[fragment.segmentIndex];

			// project the image sub-interval back onto the segment's original line.  The linear
			// relationship is anchored at the whole segment's image start, not at the fragment
			// start: a segment crossing a current-section boundary produces several fragments,
			// and only the first one begins at segment.imageStart.
			const uint64_t originalStart = segment.start + (spanStart - segment.imageStart);
			const uint64_t originalEnd = segment.start + (spanEnd - segment.imageStart);

			const OmapEvidence evidence = MakeEvidence(impl, segment);
			std::vector<ContributionId> ids = MakeContributionIds(impl, segment);

			span.origins.push_back(ImageOrigin { { originalStart, originalEnd }, { spanStart, spanEnd },
				segment.section, evidence, std::move(ids) });
		}

		// merge with the previous span only when image-adjacent and all identities match
		if (!result.empty() && result.back().image.end == spanStart && sameOrigins(result.back(), atom))
		{
			const uint64_t extension = spanEnd - spanStart;
			result.back().image.end = spanEnd;
			for (ImageOrigin& origin : result.back().origins)
			{
				origin.original.end += extension;
				origin.image.end = spanEnd;
			}
		}
		else
		{
			result.push_back(std::move(span));
		}
	}

	// insert explicit holes wherever no source projects into the query
	std::vector<ImageSpan> withHoles;
	withHoles.reserve(result.size() * 2u + 1u);
	uint64_t holeCursor = range.start;
	for (ImageSpan& span : result)
	{
		if (span.image.start > holeCursor)
		{
			withHoles.push_back(ImageSpan { { holeCursor, span.image.start }, {} });
		}
		withHoles.push_back(std::move(span));
		holeCursor = withHoles.back().image.end;
	}
	if (holeCursor < range.end)
	{
		withHoles.push_back(ImageSpan { { holeCursor, range.end }, {} });
	}

	return withHoles;
}
