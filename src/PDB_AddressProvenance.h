// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

// Address provenance index for optimized binaries.
//
// The classic ImageSectionStream::ConvertSectionOffsetToRVA() treats the current
// section table as the whole truth.  Binaries produced by link-time optimizers
// (ICF folding, identical COMDAT folding, block layout, /ORDER, ...) describe the
// relationship between original (pre-optimization) and current image addresses
// through OMAP streams stored in the DBI optional debug header:
//
//   * OMAP_FROM_SRC maps original RVA  -> current image RVA, and is the only
//     authoritative forward map.  It is neither injective (several original
//     ranges can fold onto one image range) nor monotone in its targets.
//   * OMAP_TO_SRC, when present, is at best a best-effort single-predecessor
//     hint emitted for legacy debuggers.  It must never be treated as a complete
//     inverse map, because folded sources have no unique inverse.
//
// The owning index built here copies every byte it needs out of the RawFile,
// so both the RawFile and the backing PDB bytes may be released as soon as
// BuildAddressProvenance() returns.  All addresses are half-open RVA intervals
// expressed as 64-bit values; every intermediate computation is performed in
// 64 bits so 32-bit wraparound is impossible.
//
// API support scope:
//   * DBI version V70 with a Ver60 section contribution sub-stream.
//   * Optional debug header is read by its actual length; trailing slots may be
//     absent.  Stream index 0xffff means "no such stream".
//   * Both OMAP streams absent  -> identity view over the current section table.
//   * OMAP_FROM_SRC present     -> requires the original (SectionHdrOrig) table.
//     OMAP_TO_SRC may be missing and is never used as a complete inverse map.
//   * OMAP_TO_SRC present but OMAP_FROM_SRC absent -> reported as unsupported.
//   * OMAP_FROM_SRC present but empty -> a valid, fully unmapped index.
//   * The MSF container/page directory itself is assumed valid; generic
//     container validation is intentionally out of scope.

#pragma once

#include "Foundation/PDB_Macros.h"

#include <cstdint>
#include <memory>
#include <vector>


namespace PDB
{
	class RawFile;

	namespace Provenance
	{
		// Half-open address interval [start, end), given as RVAs.
		struct PDB_NO_DISCARD AddressRange
		{
			uint64_t start;
			uint64_t end;
		};

		// State of an original-address interval.
		enum class PDB_NO_DISCARD SpanStatus : uint8_t
		{
			// OMAP_FROM_SRC forwards the interval to a valid image interval.
			Mapped,
			// A forward OMAP row with rvaTo == 0 covers the interval.
			Deleted,
			// The interval lies in an original section but precedes every
			// applicable OMAP row, so no forward mapping exists.
			Unmapped,
			// The interval is not covered by any (non-empty) original section.
			OutsideSection,
			// A forward mapping exists, but the resulting image interval does
			// not lie inside a current image section.
			OutsideImage
		};

		// Identity of a Ver60 section contribution.  Both the raw module index
		// and the zero-based ordinal of the record inside the DBI contribution
		// sub-stream are preserved; records are never merged or deduplicated.
		struct PDB_NO_DISCARD ContributionId
		{
			uint32_t ordinal;			// record sequence number, 0-based
			uint16_t moduleIndex;		// raw DBI::SectionContribution::moduleIndex
			uint16_t section;			// raw 1-based (original) section number
		};

		// Pointer at the OMAP evidence backing a mapping.
		struct PDB_NO_DISCARD OmapEvidence
		{
			// MSF stream index of the OMAP_FROM_SRC stream, or NilStreamIndex
			// (0xffff) when the index uses the identity fallback.
			uint16_t streamIndex;
			// Zero-based row index inside the OMAP stream, or UINT32_MAX for
			// identity / absent evidence.
			uint32_t row;
			// Raw contents of the backing row.
			uint32_t sourceRva;
			uint32_t targetRva;
			// True when the relationship is the synthetic identity map built
			// because both OMAP streams are absent.
			bool identity;
		};

		// One element of an original-interval projection.
		struct PDB_NO_DISCARD ForwardSpan
		{
			AddressRange original;					// half-open original RVA interval
			AddressRange image;					// half-open image RVA interval; empty unless status == Mapped
			SpanStatus status;
			uint16_t originalSection;				// 1-based original section; 0 when outside
			OmapEvidence evidence;
			std::vector<ContributionId> contributions;	// every covering contribution, ordinal order
		};

		// One original source explaining part of an image interval.
		struct PDB_NO_DISCARD ImageOrigin
		{
			AddressRange original;					// half-open original RVA interval
			AddressRange image;					// half-open image RVA interval it projects onto
			uint16_t originalSection;				// 1-based original section
			OmapEvidence evidence;
			std::vector<ContributionId> contributions;	// every covering contribution, ordinal order
		};

		// One element of an image-interval interpretation.  An empty origin
		// list is an explicit hole: no original source projects there.
		struct PDB_NO_DISCARD ImageSpan
		{
			AddressRange image;					// half-open image RVA interval inside the query
			std::vector<ImageOrigin> origins;
		};

		// Kinds of construction problems.  Every diagnostic carries the MSF
		// stream index and the logical byte offset (within that stream) of the
		// offending datum.
		enum class PDB_NO_DISCARD DiagnosticKind : uint8_t
		{
			InvalidDBIHeader,						// stream 3 is missing or too small
			InvalidDBIVersion,						// unsupported DBI signature/version
			DebugHeaderOutOfBounds,					// optional header claims more bytes than the DBI stream holds
			RequiredStreamMissing,					// a needed optional slot is 0xffff
			StreamIndexOutOfRange,					// a slot names a stream that does not exist
			StreamLengthInvalid,					// generic length/remainder check failed
			OmapHalfRow,							// OMAP stream size is not a multiple of 8 bytes
			OmapNotStrictlyAscending,				// OMAP source RVAs are not strictly increasing
			OmapTargetOverflow,						// forward target arithmetic leaves the 32-bit RVA space
			SectionStreamLengthInvalid,				// section stream size is not a multiple of IMAGE_SECTION_HEADER
			SectionRangeOverflow,					// VA + effective size leaves the 32-bit RVA space
			SectionsOverlap,						// non-empty sections of one coordinate system overlap
			ContributionTableCorrupt,				// Ver60 contribution size is not a whole number of records
			ContributionOutOfRange,					// contribution references an unknown section or leaves its section
			OmapToSrcOnlyUnsupported				// OMAP_TO_SRC present without OMAP_FROM_SRC
		};

		struct PDB_NO_DISCARD Diagnostic
		{
			DiagnosticKind kind;
			uint16_t streamIndex;					// MSF stream index (DBI is stream 3)
			uint64_t logicalOffset;					// byte offset within that stream
		};

		struct BuildResult;

		class PDB_NO_DISCARD AddressProvenance
		{
		public:
			// Owning implementation state; defined entirely inside the .cpp so that no
			// internal data layout leaks into the public header.
			struct Impl;

			AddressProvenance(void) PDB_NO_EXCEPT;
			~AddressProvenance(void) PDB_NO_EXCEPT;

			AddressProvenance(AddressProvenance&& other) PDB_NO_EXCEPT;
			AddressProvenance& operator=(AddressProvenance&& other) PDB_NO_EXCEPT;

			// True when the index was built from the current section table as
			// an identity map because both OMAP streams were absent.
			PDB_NO_DISCARD bool IsIdentity(void) const PDB_NO_EXCEPT;

			PDB_NO_DISCARD uint16_t GetOmapFromStreamIndex(void) const PDB_NO_EXCEPT;
			PDB_NO_DISCARD bool HasOmapToSrc(void) const PDB_NO_EXCEPT;
			PDB_NO_DISCARD size_t GetOmapRowCount(void) const PDB_NO_EXCEPT;
			PDB_NO_DISCARD size_t GetCurrentSectionCount(void) const PDB_NO_EXCEPT;
			PDB_NO_DISCARD size_t GetOriginalSectionCount(void) const PDB_NO_EXCEPT;
			PDB_NO_DISCARD size_t GetContributionCount(void) const PDB_NO_EXCEPT;

			// Projects a half-open original RVA interval onto the image.  The
			// result is ordered by original position and is cut at original
			// section, OMAP row and contribution boundaries; mapped pieces are
			// additionally cut at current image section boundaries.  Runs in
			// O(log n + k) where k is the number of returned spans.
			PDB_NO_DISCARD std::vector<ForwardSpan> ProjectOriginal(AddressRange range) const PDB_NO_EXCEPT;

			// Interprets a half-open image RVA interval by enumerating every
			// original source whose forward projection intersects it.  Folded
			// origins and overlapping contributions are never deduplicated;
			// positions with no source come back as explicit hole spans.
			// Runs in O(log n + k) where k grows with the actual output only.
			PDB_NO_DISCARD std::vector<ImageSpan> InterpretImage(AddressRange range) const PDB_NO_EXCEPT;

		private:
			std::unique_ptr<Impl> m_impl;

			PDB_DISABLE_COPY(AddressProvenance);

			friend BuildResult BuildAddressProvenance(const RawFile& file) PDB_NO_EXCEPT;
		};

		struct PDB_NO_DISCARD BuildResult
		{
			bool ok;
			AddressProvenance index;						// valid only when ok == true
			std::vector<Diagnostic> diagnostics;			// all problems found, in parse order
		};

		// Builds the owning provenance index from an already-constructed RawFile.
		// Never throws; all failure modes are reported through BuildResult.
		PDB_NO_DISCARD BuildResult BuildAddressProvenance(const RawFile& file) PDB_NO_EXCEPT;
	}
}
