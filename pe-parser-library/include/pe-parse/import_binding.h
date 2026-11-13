/*
The MIT License (MIT)

Copyright (c) 2026 pe-parse contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <pe-parse/parse.h>

namespace peparse {

// How an ordinary import names the symbol it binds to.  This is the real
// identity from the import lookup table; it is never recovered from the
// display string produced by IterImpVAString.
enum class ImportNameKind {
  ByName = 0,
  ByOrdinal = 1,
};

// One ordinary import thunk, with the identity the parser normally discards.
struct StructuredImport {
  // Original module name as written in the import directory.  Unlike
  // IterImpVAString, this is not upper-cased.
  std::string moduleName;
  ImportNameKind kind;
  // Valid when kind == ByName.
  std::string symbolName;
  // Valid when kind == ByOrdinal.  Always the real 16 bit ordinal.
  std::uint16_t ordinal;
  // RVA and width of this IAT slot.
  std::uint32_t iatRva;
  std::uint32_t slotWidth;
  // Zero based index in the descriptor's thunk/lookup table.
  std::uint32_t thunkIndex;
  // RVA of the import lookup table used for this descriptor.
  std::uint32_t lookupTableRva;
};

struct StructuredImportTable {
  StructuredImportTable() : hasImportDirectory(false), importDirectoryRva(0),
                            importDirectorySize(0) {
  }

  bool hasImportDirectory;
  std::uint32_t importDirectoryRva;
  std::uint32_t importDirectorySize;
  std::vector<StructuredImport> entries;
  // Non-fatal problems encountered while scanning the table.
  std::vector<std::string> diagnostics;
};

// Extract the structured, non-display identity of every ordinary import in
// pe.  Delay imports, bound imports and loader relocations are deliberately
// not read.  The display behavior of IterImpVAString is unaffected.
bool BuildStructuredImportTable(parsed_pe *pe,
                                StructuredImportTable &out,
                                std::string *error);

struct StructuredExportName {
  StructuredExportName() : nameRva(0), nameTableIndex(0) {
  }

  std::string name;
  std::uint32_t nameRva;
  std::uint32_t nameTableIndex;
};

// One slot of the export address table, whether or not it has a name.
struct StructuredExportSlot {
  StructuredExportSlot()
      : ordinalBase(0), slotIndex(0), ordinal(0), rva(0), forwarded(false),
        forwardStringRva(0), forwardStringValid(false),
        forwardRangeEnd(0), eatSlotRva(0) {
  }

  std::uint32_t ordinalBase;
  // Zero based index into the export address table.
  std::uint32_t slotIndex;
  // ordinalBase + slotIndex.
  std::uint32_t ordinal;
  // Raw EAT value.  Zero means this is an unexported hole.
  std::uint32_t rva;
  bool forwarded;
  // Present when forwarded.  If the forwarder string was truncated by the
  // export directory range, forwardStringValid is false and this string holds
  // the bytes that were readable.
  std::string forwardString;
  std::uint32_t forwardStringRva;
  bool forwardStringValid;
  // End of the export directory range, exclusive.
  std::uint32_t forwardRangeEnd;
  // RVA of the EAT dword for this slot.
  std::uint32_t eatSlotRva;
  // Every name that indexes this slot, in export name table order.
  std::vector<StructuredExportName> names;
};

struct StructuredExportTable {
  StructuredExportTable()
      : hasExportDirectory(false), exportDirectoryRva(0),
        exportDirectorySize(0), ordinalBase(0), addressTableEntries(0) {
  }

  bool hasExportDirectory;
  std::uint32_t exportDirectoryRva;
  std::uint32_t exportDirectorySize;
  std::uint32_t ordinalBase;
  std::uint32_t addressTableEntries;
  // Module name from the export directory, if it was readable.
  std::string moduleName;
  std::vector<StructuredExportSlot> slots;
  std::vector<std::string> diagnostics;
};

// Enumerate the export address table by slot.  Unnamed ordinal-only exports
// and zero RVA holes are retained; forwarder strings are read strictly inside
// the export directory range.
bool BuildStructuredExportTable(parsed_pe *pe,
                                StructuredExportTable &out,
                                std::string *error);

// Outcome of resolving one import.  Resolved is the only success value; every
// other value identifies a specific failure reason.
enum class BindingStatus {
  Resolved = 0,
  // The export slot exists but its EAT value is zero.
  ZeroSlot,
  // The requested module is not present in the supplied module set.
  MissingModule,
  // The requested export name does not exist in the provider.
  MissingExportName,
  // The requested ordinal is outside the provider's EAT.
  MissingExportOrdinal,
  // The forwarder string is missing, malformed or truncated.
  BadForward,
  // A module/symbol state repeated along the forward chain.
  Cycle,
  // The final export RVA is outside every mapped section.
  FinalRvaNotMapped,
  // ImageBase + RVA does not fit the image's address width.
  AddressOverflow,
  // A contract import was seen but no API-set namespace was supplied.
  ApiSetMissingSchema,
  // The API-set namespace is not version 6.
  ApiSetUnsupportedVersion,
  // The API-set namespace is internally inconsistent or truncated.
  ApiSetMalformed,
  // The API-set namespace contains non-ASCII text.
  ApiSetUnsupportedEncoding,
  // No API-set entry matched the contract.
  ApiSetNotFound,
  // More than one API-set entry matched the contract prefix.
  ApiSetAmbiguous,
  // The importer's alias selects a deliberately empty host.
  ApiSetDisabled,
  // The API-set entry did not yield a usable host.
  ApiSetMissingHost,
  // The provider's export directory could not be read.
  ExportTableInvalid,
  // The import table itself could not be read.
  ImportTableInvalid,
};

// Human readable name of a binding status ("resolved", "zero_slot", ...).
const char *BindingStatusName(BindingStatus status);

struct BindingModuleInput {
  BindingModuleInput() : pe(nullptr) {
  }

  // Module file name.  Only the basename is used; ASCII case is ignored and a
  // single optional ".dll" suffix is stripped during normalization.
  std::string fileName;
  // Parsed module.  The plan copies everything it needs and never modifies it.
  parsed_pe *pe;
};

struct BindingRequest {
  BindingRequest()
      : kind(ImportNameKind::ByName), ordinal(0), iatRva(0), slotWidth(0),
        thunkIndex(0), iatVa(0), iatVaOverflow(false) {
  }

  // Module name exactly as written in the root import directory.
  std::string moduleName;
  // Normalized module name used for lookup.
  std::string normalizedModule;
  ImportNameKind kind;
  std::string symbolName;
  std::uint16_t ordinal;
  std::uint32_t iatRva;
  std::uint32_t slotWidth;
  std::uint32_t thunkIndex;
  // Root ImageBase + iatRva, saturated into 64 bits.
  std::uint64_t iatVa;
  bool iatVaOverflow;
};

enum class BindingHopKind {
  // An export forwarded to another module and symbol.
  ExportForward = 0,
  // A contract name mapped through the API-set namespace.
  ApiSetContract = 1,
};

// One step in a binding chain.  Each hop carries the module that produced it
// and either its export slot or the schema offsets that selected the host.
struct BindingHop {
  BindingHop()
      : kind(BindingHopKind::ExportForward), exportSlot(0), exportOrdinal(0),
        forwardStringRva(0), forwardRangeEnd(0), schemaEntryOffset(0),
        schemaValueOffset(0), schemaHostOffset(0), usedDefaultAlias(false) {
  }

  BindingHopKind kind;
  // Module that issued this hop (normalized).
  std::string sourceModule;
  // Target module (normalized): forward stem or API-set host.
  std::string targetModule;
  // Export slot and ordinal of the forwarder, for ExportForward hops.
  std::uint32_t exportSlot;
  std::uint32_t exportOrdinal;
  std::string forwardString;
  std::uint32_t forwardStringRva;
  std::uint32_t forwardRangeEnd;
  // Namespace-relative offsets for ApiSetContract hops.
  std::uint32_t schemaEntryOffset;
  std::uint32_t schemaValueOffset;
  std::uint32_t schemaHostOffset;
  // Full entry name from the schema (not the HashedLength prefix).
  std::string contractName;
  // Host module name as stored in the schema value.
  std::string schemaHost;
  // Normalized importer whose alias selected the host.
  std::string importerModule;
  bool usedDefaultAlias;
};

struct BindingResult {
  BindingResult() : exportSlot(0), exportOrdinal(0), rva(0), va(0) {
  }

  // Normalized module that actually supplies the final RVA.
  std::string providerModule;
  std::uint32_t exportSlot;
  std::uint32_t exportOrdinal;
  std::uint32_t rva;
  // provider ImageBase + rva.
  std::uint64_t va;
};

struct BindingRow {
  BindingRow() : status(BindingStatus::Resolved) {
  }

  BindingRequest request;
  BindingStatus status;
  // Specific reason, including a rendered state chain for cycles.
  std::string diagnostic;
  std::vector<BindingHop> hops;
  // Module/symbol states attempted, in order.  For a cycle the final entry is
  // the repeated state that closed the chain.
  std::vector<std::string> stateChain;
  // Valid iff status == Resolved.
  BindingResult result;
};

// Read-only result of BuildImportBindingPlan.  It owns all names, ranges and
// diagnostics, so it remains valid after the parsed modules and the API-set
// namespace buffer are destroyed.
class ImportBindingPlan {
public:
  ImportBindingPlan() {
  }

  const std::vector<BindingRow> &Rows() const {
    return rows_;
  }

  const std::vector<std::string> &Diagnostics() const {
    return diagnostics_;
  }

  bool Empty() const {
    return rows_.empty();
  }

private:
  friend bool BuildImportBindingPlan(const std::string &,
                                     const std::vector<BindingModuleInput> &,
                                     const std::uint8_t *,
                                     std::uint32_t,
                                     ImportBindingPlan &,
                                     std::string *);

  std::vector<BindingRow> rows_;
  std::vector<std::string> diagnostics_;
};

// Build a binding plan for every ordinary import of rootModuleName.
//
// modules is the complete universe of modules: nothing is loaded from disk,
// the registry or the process environment.  Root and provider modules are
// found by normalized basename; duplicate normalized names reject the whole
// build.  apiSetNamespace may be null/zero, in which case ordinary DLL imports
// still resolve but contract imports report ApiSetMissingSchema.
//
// Returns false only for invalid caller input (duplicate modules, a missing
// root, a null parsed_pe).  Per-import failures are reported in rows.
bool BuildImportBindingPlan(const std::string &rootModuleName,
                            const std::vector<BindingModuleInput> &modules,
                            const std::uint8_t *apiSetNamespace,
                            std::uint32_t apiSetNamespaceSize,
                            ImportBindingPlan &plan,
                            std::string *error);

} // namespace peparse
