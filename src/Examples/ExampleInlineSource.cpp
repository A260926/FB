// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#include "Examples_PCH.h"
#include "ExampleTimedScope.h"
#include "PDB_RawFile.h"
#include "PDB_DBIStream.h"
#include "PDB_InfoStream.h"
#include "PDB_InlineSource.h"


namespace
{
	// Builds a query address from the first physical procedure found in a module's symbol stream.
	// The inline index only needs a section and a section-relative offset; it does not need the
	// executable image, so this is enough to demonstrate an offline lookup.
	bool FindFirstProcedure(const PDB::RawFile& rawPdbFile, const PDB::ModuleInfoStream::Module& module,
		uint16_t& section, uint32_t& codeOffset)
	{
		const PDB::ModuleSymbolStream symbols = module.CreateSymbolStream(rawPdbFile);

		bool found = false;
		symbols.ForEachSymbol([&](const PDB::CodeView::DBI::Record* record)
		{
			if (found)
			{
				return;
			}

			if (record->header.kind == PDB::CodeView::DBI::SymbolRecordKind::S_LPROC32 ||
				record->header.kind == PDB::CodeView::DBI::SymbolRecordKind::S_LPROC32_ID)
			{
				section = record->data.S_LPROC32.section;
				codeOffset = record->data.S_LPROC32.offset + 1u;
				found = true;
			}
			else if (record->header.kind == PDB::CodeView::DBI::SymbolRecordKind::S_GPROC32 ||
				record->header.kind == PDB::CodeView::DBI::SymbolRecordKind::S_GPROC32_ID)
			{
				section = record->data.S_GPROC32.section;
				codeOffset = record->data.S_GPROC32.offset + 1u;
				found = true;
			}
		});

		return found;
	}


}


void ExampleInlineSource(const PDB::RawFile& rawPdbFile, const PDB::DBIStream& dbiStream, const PDB::InfoStream& infoStream);
void ExampleInlineSource(const PDB::RawFile& rawPdbFile, const PDB::DBIStream& dbiStream, const PDB::InfoStream& infoStream)
{
	if (!infoStream.HasNamesStream())
	{
		printf("\nPDB has no '/names' stream for resolving inline source files, skipping \"Inline source\" example.\n");
		return;
	}

	TimedScope total("\nRunning example \"Inline source\"");

	// collect the module slices the index needs.  symbolSize already includes the 4-byte signature.
	TimedScope moduleScope("Reading module info stream");
	const PDB::ModuleInfoStream moduleInfoStream = dbiStream.CreateModuleInfoStream(rawPdbFile);
	const PDB::ArrayView<PDB::ModuleInfoStream::Module> modules = moduleInfoStream.GetModules();
	moduleScope.Done();

	std::vector<PDB::InlineSourceModule> slices;
	std::vector<std::pair<uint16_t, uint32_t> > addresses;

	{
		TimedScope scope("Collecting module slices");

		for (size_t i = 0u; i < modules.GetLength(); ++i)
		{
			const PDB::ModuleInfoStream::Module& module = modules[i];
			if (!module.HasSymbolStream())
			{
				continue;
			}

			const PDB::DBI::ModuleInfo* info = module.GetInfo();
			if (info->c13Size == 0u)
			{
				// without C13 line information there are no inline sites to index
				continue;
			}

			slices.push_back(PDB::InlineSourceModule(
				static_cast<uint16_t>(i),
				info->moduleSymbolStreamIndex,
				info->symbolSize,
				info->c11Size,
				info->c13Size));

			uint16_t section = 0u;
			uint32_t codeOffset = 0u;
			if (FindFirstProcedure(rawPdbFile, module, section, codeOffset))
			{
				addresses.push_back(std::make_pair(section, codeOffset));
			}
		}

		scope.Done(slices.size());
	}

	TimedScope buildScope("Building the inline source index");
	PDB::InlineSourceIndex index;
	const bool built = index.Build(rawPdbFile, infoStream.GetNamesStreamIndex(), slices);
	buildScope.Done();

	if (!built)
	{
		const PDB::InlineSourceError& error = index.GetError();
		printf("Failed to build the inline source index: %s (module %u, stream %u, offset 0x%X)\n",
			error.description.c_str(), error.moduleIndex, error.streamIndex, error.offset);
		return;
	}

	printf("Indexed %zu module(s)\n", slices.size());

	// The address is only a vehicle for the lookup; the caller passes whatever section-relative
	// address it is interested in.
	for (size_t i = 0u; i < addresses.size(); ++i)
	{
		const std::vector<PDB::InlineSourceChain> chains = index.Query(addresses[i].first, addresses[i].second);
		printf("Address [0x%04X:0x%08X]: %zu inline chain(s)\n", addresses[i].first, addresses[i].second, chains.size());

		for (size_t c = 0u; c < chains.size(); ++c)
		{
			const PDB::InlineSourceChain& chain = chains[c];
			printf("  physical procedure 0x%08X\n", chain.procedure.symbolOffset);

			for (size_t f = 0u; f < chain.frames.size(); ++f)
			{
				const PDB::InlineSourceFrame& frame = chain.frames[f];
				printf("    %s:%u-%u inlinee 0x%X in [0x%08X, 0x%08X)\n",
					frame.file.c_str(), frame.lineNumber, frame.GetLastLine(), frame.inlinee,
					frame.codeOffsetBegin, frame.codeOffsetEnd);
			}
		}
	}
}
