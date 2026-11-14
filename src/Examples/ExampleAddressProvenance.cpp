// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

// Offline address-provenance example.
//
// This example is deliberately self-contained and performs no process inspection,
// dynamic-library loading or symbol-server access: it only reads the bytes of a
// PDB file from disk and builds the owning provenance index.  Once the index has
// been built, both the RawFile and the raw PDB bytes are released, while every
// query result stays valid because the index owns all of its data.

#include "PDB.h"
#include "PDB_RawFile.h"
#include "PDB_AddressProvenance.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>


namespace
{
	const char* StatusName(PDB::Provenance::SpanStatus status)
	{
		switch (status)
		{
			case PDB::Provenance::SpanStatus::Mapped:			return "mapped";
			case PDB::Provenance::SpanStatus::Deleted:			return "deleted";
			case PDB::Provenance::SpanStatus::Unmapped:		return "unmapped";
			case PDB::Provenance::SpanStatus::OutsideSection:	return "outside_section";
			case PDB::Provenance::SpanStatus::OutsideImage:	return "outside_image";
		}
		return "unknown";
	}
}


int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::printf("usage: ExampleAddressProvenance <file.pdb>\n");
		return 1;
	}

	// read the PDB fully into memory; no mapping of the target image is involved
	std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
	if (!file)
	{
		std::printf("cannot open %s\n", argv[1]);
		return 1;
	}
	const std::streamoff fileSize = file.tellg();
	file.seekg(0, std::ios::beg);

	std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
	if (!file.read(reinterpret_cast<char*>(bytes.data()), fileSize))
	{
		std::printf("cannot read %s\n", argv[1]);
		return 1;
	}

	if (PDB::ValidateFile(bytes.data(), bytes.size()) != PDB::ErrorCode::Success)
	{
		std::printf("not a valid PDB file\n");
		return 2;
	}

	PDB::RawFile rawFile = PDB::CreateRawFile(bytes.data());
	PDB::Provenance::BuildResult built = PDB::Provenance::BuildAddressProvenance(rawFile);
	if (!built.ok)
	{
		std::printf("provenance index could not be built, %zu diagnostic(s):\n", built.diagnostics.size());
		for (const PDB::Provenance::Diagnostic& diagnostic : built.diagnostics)
		{
			std::printf("  kind=%u stream=%u offset=%llu\n", static_cast<unsigned>(diagnostic.kind),
				static_cast<unsigned>(diagnostic.streamIndex),
				static_cast<unsigned long long>(diagnostic.logicalOffset));
		}
		return 3;
	}

	PDB::Provenance::AddressProvenance& index = built.index;
	std::printf("provenance index: identity=%d omapRows=%zu currentSections=%zu originalSections=%zu contributions=%zu\n",
		index.IsIdentity() ? 1 : 0, index.GetOmapRowCount(), index.GetCurrentSectionCount(),
		index.GetOriginalSectionCount(), index.GetContributionCount());

	// the owning index no longer needs the RawFile or the backing bytes
	bytes.clear();
	bytes.shrink_to_fit();
	(void)rawFile;

	// 1) project an original (pre-optimization) interval forward.  The exact interval
	//    would normally come from a symbol or a contribution; a whole first-section
	//    query is used here to keep the example self-contained.
	const PDB::Provenance::AddressRange originalQuery { 0x1000ull, 0x1200ull };
	const std::vector<PDB::Provenance::ForwardSpan> forward = index.ProjectOriginal(originalQuery);
	std::printf("\nforward projection of [%llx,%llx): %zu span(s)\n",
		static_cast<unsigned long long>(originalQuery.start),
		static_cast<unsigned long long>(originalQuery.end), forward.size());
	for (const PDB::Provenance::ForwardSpan& span : forward)
	{
		std::printf("  orig [%llx,%llx) status=%s section=%u image [%llx,%llx) omapRow=%u contributions=%zu\n",
			static_cast<unsigned long long>(span.original.start),
			static_cast<unsigned long long>(span.original.end), StatusName(span.status),
			static_cast<unsigned>(span.originalSection),
			static_cast<unsigned long long>(span.image.start),
			static_cast<unsigned long long>(span.image.end),
			span.evidence.row, span.contributions.size());
	}

	// 2) interpret an image interval: enumerate every original source that folds into it.
	const PDB::Provenance::AddressRange imageQuery { 0x5000ull, 0x5200ull };
	const std::vector<PDB::Provenance::ImageSpan> interpreted = index.InterpretImage(imageQuery);
	std::printf("\nimage interpretation of [%llx,%llx): %zu span(s)\n",
		static_cast<unsigned long long>(imageQuery.start),
		static_cast<unsigned long long>(imageQuery.end), interpreted.size());
	for (const PDB::Provenance::ImageSpan& span : interpreted)
	{
		if (span.origins.empty())
		{
			std::printf("  image [%llx,%llx) hole: no original source\n",
				static_cast<unsigned long long>(span.image.start),
				static_cast<unsigned long long>(span.image.end));
			continue;
		}
		for (const PDB::Provenance::ImageOrigin& origin : span.origins)
		{
			std::printf("  image [%llx,%llx) <- orig [%llx,%llx) section=%u omapRow=%u contributions=%zu\n",
				static_cast<unsigned long long>(span.image.start),
				static_cast<unsigned long long>(span.image.end),
				static_cast<unsigned long long>(origin.original.start),
				static_cast<unsigned long long>(origin.original.end),
				static_cast<unsigned>(origin.originalSection), origin.evidence.row,
				origin.contributions.size());
		}
	}

	return 0;
}
