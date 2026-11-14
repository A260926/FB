/*
The MIT License (MIT)

Copyright (c) 2013 Andrew Ruef

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

#include "parse.h"

// Static interpretation of Control Flow Guard coverage.
//
// The functions in this header only decode structures that are already
// present in a parsed PE image: the load config directory, its GuardFlags
// and the GFIDS (guard CF function) table. They never load a DLL, never
// execute code and never ask the host operating system whether CFG is
// actually enabled for some process. All conclusions are drawn from file
// contents alone.

namespace peparse {

#ifndef _PEPARSE_WINDOWS_CONFLICTS
// GuardFlags bits and GFIDS entry metadata bits. Only the subset needed to
// explain CFG coverage is listed here.
constexpr std::uint32_t IMAGE_GUARD_CF_INSTRUMENTED = 0x00000100;
constexpr std::uint32_t IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT = 0x00000400;
constexpr std::uint32_t IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_MASK = 0xF0000000;
constexpr std::uint32_t IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_SHIFT = 28;

constexpr std::uint8_t IMAGE_GUARD_FID_SUPPRESSED = 0x1;
constexpr std::uint8_t IMAGE_GUARD_FID_EXPORT_SUPPRESSED = 0x2;
#endif

// Outcome of reading the guard metadata or of evaluating a query.
enum class guard_coverage_status {
  ok = 0,             // structures declared, well formed and fully evaluated
  not_declared = 1,   // the load config does not declare the whole CFG group
  corrupt = 2,        // malformed, truncated or contradictory structures
  invalid_request = 3 // the caller supplied an invalid query
};

// Coverage verdict for a single queried target RVA.
enum class guard_target_coverage {
  covered = 0,       // definitely covered by at least one usable entry
  indeterminate = 1, // only potential coverage via unknown metadata entries
  uncovered = 2      // no entry covers this target
};

// One entry of the guard CF function table (GFIDS). Entries are kept in
// file order; each of them is 4 bytes of target RVA plus a number of
// additional bytes dictated by the high nibble of GuardFlags.
struct guard_fids_entry {
  guard_fids_entry()
      : rva(0),
        file_offset(0),
        fid_suppressed(false),
        export_suppressed(false),
        unknown_metadata(false) {
  }

  RVA rva;                            // target RVA declared by this entry
  std::uint32_t file_offset;          // offset of this entry inside the file
  std::vector<std::uint8_t> additional; // raw additional bytes, file order
  bool fid_suppressed;                // first additional byte, bit 0x1
  bool export_suppressed;             // first additional byte, bit 0x2
  bool unknown_metadata;              // unknown bits or nonzero trailing bytes
};

// Mapped extent of one section, expressed in RVAs. Used to tell whether a
// target lies inside a unique executable section.
struct guard_section_range {
  guard_section_range() : begin(0), end(0) {
  }

  std::string name;
  std::uint64_t begin; // inclusive
  std::uint64_t end;   // exclusive
};

// A self-contained report about the control flow guard metadata of one PE.
// The report owns all of its data; it stays valid after the parsed_pe it
// was built from has been destructed.
struct guard_coverage_metadata {
  guard_coverage_metadata()
      : status(guard_coverage_status::corrupt),
        pe32_plus(false),
        image_base(0),
        load_config_rva(0),
        load_config_directory_size(0),
        load_config_size(0),
        guard_cf(false),
        cf_instrumented(false),
        cf_function_table_present(false),
        guard_flags(0),
        guard_cf_function_table_va(0),
        guard_cf_function_table_rva(0),
        guard_cf_function_count(0),
        entry_stride(0) {
  }

  guard_coverage_status status;
  bool pe32_plus;   // true for PE32+, false for PE32
  std::uint64_t image_base;

  RVA load_config_rva;                  // data directory RVA (an RVA)
  std::uint32_t load_config_directory_size; // data directory size
  std::uint32_t load_config_size;       // self-declared Size of the config

  bool guard_cf;                    // DllCharacteristics GUARD_CF declared
  bool cf_instrumented;             // GuardFlags CF_INSTRUMENTED declared
  bool cf_function_table_present;   // GuardFlags CF_FUNCTION_TABLE_PRESENT
  std::uint32_t guard_flags;        // raw GuardFlags value

  VA guard_cf_function_table_va;    // GuardCFFunctionTable as declared (a VA)
  RVA guard_cf_function_table_rva;  // the same table, converted to an RVA
  std::uint64_t guard_cf_function_count;
  std::uint32_t entry_stride;       // 4 plus the per-entry additional bytes

  std::vector<guard_fids_entry> entries;

  // Contradictions between GUARD_CF, CF_INSTRUMENTED,
  // CF_FUNCTION_TABLE_PRESENT and the table that is actually present.
  // The presence of a table alone never implies that every indirect call
  // site is protected.
  std::vector<std::string> inconsistencies;

  // Executable sections, used by the target query for its annotation.
  std::vector<guard_section_range> executable_sections;
};

// Identifies one original GFIDS entry that takes part in the coverage of a
// queried target.
struct guard_covering_entry {
  guard_covering_entry() : index(0), rva(0), file_offset(0) {
  }

  std::uint64_t index;        // position of the entry in the GFIDS table
  RVA rva;                    // target RVA declared by the entry
  std::uint32_t file_offset;  // offset of the entry inside the file
};

// The outcome for one queried target RVA. Results own their data.
struct guard_target_result {
  guard_target_result()
      : rva(0),
        coverage(guard_target_coverage::uncovered),
        in_unique_executable_section(false) {
  }

  RVA rva;                        // the queried target, input order kept
  guard_target_coverage coverage;
  std::vector<guard_covering_entry> covering_entries;  // definite coverage
  std::vector<guard_covering_entry> potential_entries; // potential coverage
  bool in_unique_executable_section;
};

// Read the guard coverage metadata of a parsed PE. The load config fields
// are only read within the range declared by its Size field; a Size that
// exceeds the data directory or that ends in the middle of a scalar field
// is reported as corrupt. An old config that ends before the CFG field
// group (GuardCFFunctionTable, GuardCFFunctionCount, GuardFlags) yields
// not_declared rather than an empty table.
guard_coverage_status GetGuardCoverageMetadata(
    parsed_pe *pe, guard_coverage_metadata &metadata);

// Explain the coverage of a set of target RVAs under a static model:
// plain entries are usable, FID_SUPPRESSED entries never are, and entries
// that only carry EXPORT_SUPPRESSED are usable exactly when their RVA is
// part of activated_export_suppressed. An aligned usable entry covers only
// itself; an unaligned usable entry covers its whole 16 byte slot. Entries
// with unknown metadata only ever contribute potential coverage.
//
// Every RVA in activated_export_suppressed must name an existing entry
// that is export suppressed, otherwise the whole query is rejected with
// invalid_request. One result is produced per input target, in input
// order and with duplicates preserved. Neither input is modified.
guard_coverage_status QueryGuardCoverage(
    const guard_coverage_metadata &metadata,
    const std::vector<RVA> &targets,
    const std::vector<RVA> &activated_export_suppressed,
    std::vector<guard_target_result> &results);
} // namespace peparse
