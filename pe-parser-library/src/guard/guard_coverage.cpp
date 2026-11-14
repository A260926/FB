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

#include <pe-parse/guard_coverage.h>

#include <algorithm>
#include <set>

namespace peparse {

namespace {

// Layout of the load config fields this report depends on, per PE flavor.
// offsets are relative to the start of the load config directory data.
struct load_config_layout {
  std::uint32_t guard_cf_function_table; // GuardCFFunctionTable (a VA)
  std::uint32_t guard_cf_function_count; // GuardCFFunctionCount
  std::uint32_t guard_flags;             // GuardFlags
  std::uint32_t cfg_group_end;           // end of the whole CFG field group
  std::uint32_t max_known_end;           // end of the last field we model
  const std::uint32_t *boundaries;       // sorted scalar field boundaries
  std::uint32_t boundary_count;
  bool wide_fields;                      // true when table/count are 64 bit
};

// Scalar field boundaries of image_load_config_32, including the nested
// code integrity scalars. A declared Size that ends anywhere else cuts a
// scalar field in half.
// clang-format off
const std::uint32_t kBoundaries32[] = {
    0,   4,   8,   10,  12,  16,  20,  24,  28,  32,
    36,  40,  44,  48,  52,  54,  56,  60,  64,  68,
    72,  76,  80,  84,  88,  92,  94,  96,  100, 104,
    108, 112, 116, 120, 124, 128, 132, 136, 140, 142,
    144, 148, 152, 156, 160, 164,
};

// Scalar field boundaries of image_load_config_64.
const std::uint32_t kBoundaries64[] = {
    0,   4,   8,   10,  12,  16,  20,  24,  32,  40,
    48,  56,  64,  72,  76,  78,  80,  88,  96,  104,
    112, 120, 128, 136, 144, 148, 150, 152, 156, 160,
    168, 176, 184, 192, 200, 208, 216, 224, 228, 230,
    232, 240, 244, 248, 256, 264,
};
// clang-format on

const load_config_layout kLayout32 = {
    80, 84, 88, 92, 164, kBoundaries32, 46, false,
};

const load_config_layout kLayout64 = {
    128, 136, 144, 148, 264, kBoundaries64, 46, true,
};

// A snapshot of one section, taken while the parsed_pe is alive. The raw
// data pointer is borrowed and only used during the metadata read.
struct section_snapshot {
  std::string name;
  std::uint32_t virtual_address;
  std::uint32_t virtual_size;
  std::uint32_t raw_size;
  std::uint32_t raw_pointer;
  std::uint32_t characteristics;
  const std::uint8_t *raw_data;
  std::uint64_t raw_length;
};

int collectSection(void *cbd,
                   const VA &,
                   const std::string &name,
                   const image_section_header &header,
                   const bounded_buffer *data) {
  auto *sections = static_cast<std::vector<section_snapshot> *>(cbd);

  section_snapshot snapshot;
  snapshot.name = name;
  snapshot.virtual_address = header.VirtualAddress;
  snapshot.virtual_size = header.Misc.VirtualSize;
  snapshot.raw_size = header.SizeOfRawData;
  snapshot.raw_pointer = header.PointerToRawData;
  snapshot.characteristics = header.Characteristics;
  snapshot.raw_data = data != nullptr ? data->buf : nullptr;
  snapshot.raw_length = data != nullptr ? data->bufLen : 0;
  sections->push_back(snapshot);

  return 0;
}

std::uint32_t readU32(const std::uint8_t *p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t readU64(const std::uint8_t *p) {
  std::uint64_t value = 0;
  for (std::uint32_t i = 0; i < 8; i++) {
    value |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  }
  return value;
}

// A declared Size may not end in the middle of a scalar field. Sizes past
// the last field we model are accepted: newer struct versions append
// fields we know nothing about.
bool sizeOnFieldBoundary(const load_config_layout &layout,
                         std::uint32_t size) {
  if (size > layout.max_known_end) {
    return true;
  }

  for (std::uint32_t i = 0; i < layout.boundary_count; i++) {
    if (layout.boundaries[i] == size) {
      return true;
    }
  }

  return false;
}

// Locate the byte range [rva, rva + size) inside the raw data of exactly
// one section. Zero filled parts of a section and ranges covered by more
// than one mapping are both refused: nothing may stand in for real file
// bytes and overlapping mappings are ambiguous.
bool locateRawBytes(const std::vector<section_snapshot> &sections,
                    std::uint64_t rva,
                    std::uint64_t size,
                    const section_snapshot *&section,
                    std::uint64_t &offset) {
  const section_snapshot *found = nullptr;

  for (const section_snapshot &candidate : sections) {
    if (candidate.raw_length == 0 || candidate.raw_data == nullptr) {
      continue;
    }

    std::uint64_t low = candidate.virtual_address;
    std::uint64_t high = low + candidate.raw_length;

    if (rva >= low && rva + size >= rva && rva + size <= high) {
      if (found != nullptr) {
        return false; // covered by two raw mappings
      }
      found = &candidate;
    }
  }

  if (found == nullptr) {
    return false;
  }

  section = found;
  offset = rva - found->virtual_address;
  return true;
}

// The 16 byte slot a table entry influences. Aligned entries only speak
// for themselves; unaligned ones cover their whole slot.
std::pair<std::uint64_t, std::uint64_t> entryCoverage(RVA rva) {
  std::uint64_t address = rva;
  if (address % 16 == 0) {
    return std::make_pair(address, address + 1);
  }

  std::uint64_t slot = address & ~static_cast<std::uint64_t>(15);
  return std::make_pair(slot, slot + 16);
}

} // namespace

guard_coverage_status GetGuardCoverageMetadata(
    parsed_pe *pe, guard_coverage_metadata &metadata) {
  metadata = guard_coverage_metadata();

  if (pe == nullptr) {
    metadata.status = guard_coverage_status::invalid_request;
    return metadata.status;
  }

  const nt_header_32 &nt = pe->peHeader.nt;

  const load_config_layout *layout;
  std::uint64_t imageBase;
  std::uint16_t dllCharacteristics;
  data_directory directory;

  if (nt.OptionalMagic == NT_OPTIONAL_32_MAGIC) {
    layout = &kLayout32;
    imageBase = nt.OptionalHeader.ImageBase;
    dllCharacteristics = nt.OptionalHeader.DllCharacteristics;
    directory = nt.OptionalHeader.DataDirectory[DIR_LOAD_CONFIG];
    metadata.pe32_plus = false;
  } else if (nt.OptionalMagic == NT_OPTIONAL_64_MAGIC) {
    layout = &kLayout64;
    imageBase = nt.OptionalHeader64.ImageBase;
    dllCharacteristics = nt.OptionalHeader64.DllCharacteristics;
    directory = nt.OptionalHeader64.DataDirectory[DIR_LOAD_CONFIG];
    metadata.pe32_plus = true;
  } else {
    metadata.status = guard_coverage_status::corrupt;
    return metadata.status;
  }

  metadata.image_base = imageBase;
  metadata.guard_cf =
      (dllCharacteristics & IMAGE_DLLCHARACTERISTICS_GUARD_CF) != 0;

  // Snapshot the sections once; every lookup below works on the copy.
  std::vector<section_snapshot> sections;
  IterSec(pe, collectSection, &sections);

  for (const section_snapshot &section : sections) {
    if ((section.characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
      continue;
    }

    guard_section_range range;
    range.name = section.name;
    range.begin = section.virtual_address;
    range.end = range.begin + std::max<std::uint64_t>(section.virtual_size,
                                                      section.raw_size);
    metadata.executable_sections.push_back(range);
  }

  // The data directory holds an RVA; a missing directory means the CFG
  // field group was never declared.
  if (directory.VirtualAddress == 0 || directory.Size == 0) {
    metadata.status = guard_coverage_status::not_declared;
    return metadata.status;
  }

  metadata.load_config_rva = directory.VirtualAddress;
  metadata.load_config_directory_size = directory.Size;

  // The Size field itself has to be readable and inside the directory.
  if (directory.Size < sizeof(std::uint32_t)) {
    metadata.status = guard_coverage_status::corrupt;
    return metadata.status;
  }

  const section_snapshot *configSection = nullptr;
  std::uint64_t configOffset = 0;
  if (!locateRawBytes(sections,
                      directory.VirtualAddress,
                      sizeof(std::uint32_t),
                      configSection,
                      configOffset)) {
    metadata.status = guard_coverage_status::corrupt;
    return metadata.status;
  }

  std::uint32_t declaredSize =
      readU32(configSection->raw_data + configOffset);
  metadata.load_config_size = declaredSize;

  // The declared Size may not exceed the directory it lives in, and it
  // may not cut a scalar field in half.
  if (declaredSize > directory.Size ||
      !sizeOnFieldBoundary(*layout, declaredSize)) {
    metadata.status = guard_coverage_status::corrupt;
    return metadata.status;
  }

  // Fields are only read within the declared Size; an old config that
  // ends before the CFG group does not declare an empty table.
  if (declaredSize < layout->cfg_group_end) {
    metadata.status = guard_coverage_status::not_declared;
    return metadata.status;
  }

  if (!locateRawBytes(sections,
                      directory.VirtualAddress,
                      declaredSize,
                      configSection,
                      configOffset)) {
    metadata.status = guard_coverage_status::corrupt;
    return metadata.status;
  }

  const std::uint8_t *config = configSection->raw_data + configOffset;

  std::uint64_t tableVA;
  std::uint64_t functionCount;
  if (layout->wide_fields) {
    tableVA = readU64(config + layout->guard_cf_function_table);
    functionCount = readU64(config + layout->guard_cf_function_count);
  } else {
    tableVA = readU32(config + layout->guard_cf_function_table);
    functionCount = readU32(config + layout->guard_cf_function_count);
  }
  std::uint32_t guardFlags = readU32(config + layout->guard_flags);

  metadata.guard_flags = guardFlags;
  metadata.cf_instrumented =
      (guardFlags & IMAGE_GUARD_CF_INSTRUMENTED) != 0;
  metadata.cf_function_table_present =
      (guardFlags & IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT) != 0;
  metadata.guard_cf_function_table_va = tableVA;
  metadata.guard_cf_function_count = functionCount;
  metadata.entry_stride =
      sizeof(std::uint32_t) +
      ((guardFlags & IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_MASK) >>
       IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_SHIFT);

  // A null table with a nonzero count, or the other way around, is a
  // contradiction rather than an empty table.
  if ((tableVA == 0) != (functionCount == 0)) {
    metadata.status = guard_coverage_status::corrupt;
    return metadata.status;
  }

  if (tableVA != 0) {
    // GuardCFFunctionTable is a VA; convert it with the file's own image
    // base and refuse to truncate a 64 bit result.
    if (tableVA < imageBase) {
      metadata.status = guard_coverage_status::corrupt;
      return metadata.status;
    }

    std::uint64_t tableRVA = tableVA - imageBase;
    if (tableRVA > 0xFFFFFFFFULL) {
      metadata.status = guard_coverage_status::corrupt;
      return metadata.status;
    }

    metadata.guard_cf_function_table_rva =
        static_cast<RVA>(tableRVA);

    std::uint64_t stride = metadata.entry_stride;
    if (functionCount >
        0xFFFFFFFFFFFFFFFFULL / stride) {
      metadata.status = guard_coverage_status::corrupt;
      return metadata.status;
    }
    std::uint64_t tableSize = functionCount * stride;

    const section_snapshot *tableSection = nullptr;
    std::uint64_t tableOffset = 0;
    if (!locateRawBytes(
            sections, tableRVA, tableSize, tableSection, tableOffset)) {
      metadata.status = guard_coverage_status::corrupt;
      return metadata.status;
    }

    const std::uint8_t *table = tableSection->raw_data + tableOffset;
    std::uint64_t fileOffset = tableSection->raw_pointer + tableOffset;

    RVA previous = 0;
    for (std::uint64_t i = 0; i < functionCount; i++) {
      const std::uint8_t *raw = table + i * stride;

      guard_fids_entry entry;
      entry.rva = readU32(raw);
      entry.file_offset =
          static_cast<std::uint32_t>(fileOffset + i * stride);
      entry.additional.assign(raw + sizeof(std::uint32_t),
                              raw + stride);

      // Targets are strictly increasing; duplicates are rejected, never
      // silently merged.
      if (i != 0 && entry.rva <= previous) {
        metadata.status = guard_coverage_status::corrupt;
        return metadata.status;
      }
      previous = entry.rva;

      if (!entry.additional.empty()) {
        std::uint8_t first = entry.additional[0];
        entry.fid_suppressed =
            (first & IMAGE_GUARD_FID_SUPPRESSED) != 0;
        entry.export_suppressed =
            (first & IMAGE_GUARD_FID_EXPORT_SUPPRESSED) != 0;

        bool unknown =
            (first & ~(IMAGE_GUARD_FID_SUPPRESSED |
                       IMAGE_GUARD_FID_EXPORT_SUPPRESSED)) != 0;
        for (std::size_t k = 1; k < entry.additional.size(); k++) {
          if (entry.additional[k] != 0) {
            unknown = true;
          }
        }
        entry.unknown_metadata = unknown;
      }

      metadata.entries.push_back(entry);
    }
  }

  // Report where the declared properties contradict each other. Having a
  // table never implies that every call site is protected.
  if (metadata.guard_cf && !metadata.cf_instrumented) {
    metadata.inconsistencies.push_back(
        "DllCharacteristics declares GUARD_CF but GuardFlags does not "
        "declare CF_INSTRUMENTED");
  }
  if (!metadata.guard_cf && metadata.cf_instrumented) {
    metadata.inconsistencies.push_back(
        "GuardFlags declares CF_INSTRUMENTED but DllCharacteristics does "
        "not declare GUARD_CF");
  }
  if (metadata.cf_function_table_present && metadata.entries.empty()) {
    metadata.inconsistencies.push_back(
        "GuardFlags declares CF_FUNCTION_TABLE_PRESENT but the guard "
        "function table is absent or empty");
  }
  if (!metadata.cf_function_table_present && !metadata.entries.empty()) {
    metadata.inconsistencies.push_back(
        "guard function table is present but GuardFlags does not declare "
        "CF_FUNCTION_TABLE_PRESENT");
  }

  metadata.status = guard_coverage_status::ok;
  return metadata.status;
}

guard_coverage_status QueryGuardCoverage(
    const guard_coverage_metadata &metadata,
    const std::vector<RVA> &targets,
    const std::vector<RVA> &activated_export_suppressed,
    std::vector<guard_target_result> &results) {
  results.clear();

  if (metadata.status != guard_coverage_status::ok) {
    return metadata.status;
  }

  // The activation set may only name existing export suppressed entries;
  // anything else rejects the query instead of opening the table up.
  std::set<RVA> activated;
  for (RVA rva : activated_export_suppressed) {
    const guard_fids_entry *match = nullptr;
    for (const guard_fids_entry &entry : metadata.entries) {
      if (entry.rva == rva) {
        match = &entry;
        break;
      }
    }

    if (match == nullptr || !match->export_suppressed) {
      return guard_coverage_status::invalid_request;
    }

    activated.insert(rva);
  }

  // Classify every entry once. Unknown metadata never justifies a
  // definite verdict; suppressed entries contribute nothing at all.
  struct coverage_source {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t index;
    RVA rva;
    std::uint32_t file_offset;
  };

  std::vector<coverage_source> definite;
  std::vector<coverage_source> potential;

  for (std::uint64_t i = 0; i < metadata.entries.size(); i++) {
    const guard_fids_entry &entry = metadata.entries[i];
    if (entry.fid_suppressed) {
      continue;
    }

    bool usable = !entry.unknown_metadata &&
                  (!entry.export_suppressed || activated.count(entry.rva) != 0);
    bool maybe = entry.unknown_metadata;
    if (!usable && !maybe) {
      continue;
    }

    std::pair<std::uint64_t, std::uint64_t> range = entryCoverage(entry.rva);

    coverage_source source;
    source.begin = range.first;
    source.end = range.second;
    source.index = i;
    source.rva = entry.rva;
    source.file_offset = entry.file_offset;

    if (usable) {
      definite.push_back(source);
    } else {
      potential.push_back(source);
    }
  }

  for (RVA target : targets) {
    guard_target_result result;
    result.rva = target;

    for (const coverage_source &source : definite) {
      if (target >= source.begin && target < source.end) {
        guard_covering_entry producer;
        producer.index = source.index;
        producer.rva = source.rva;
        producer.file_offset = source.file_offset;
        result.covering_entries.push_back(producer);
      }
    }

    for (const coverage_source &source : potential) {
      if (target >= source.begin && target < source.end) {
        guard_covering_entry producer;
        producer.index = source.index;
        producer.rva = source.rva;
        producer.file_offset = source.file_offset;
        result.potential_entries.push_back(producer);
      }
    }

    // Definite coverage wins; only without it does potential coverage
    // make the target indeterminate rather than uncovered.
    if (!result.covering_entries.empty()) {
      result.coverage = guard_target_coverage::covered;
    } else if (!result.potential_entries.empty()) {
      result.coverage = guard_target_coverage::indeterminate;
    } else {
      result.coverage = guard_target_coverage::uncovered;
    }

    // Metadata coverage and executability are reported separately.
    std::uint32_t executableMatches = 0;
    for (const guard_section_range &range : metadata.executable_sections) {
      if (target >= range.begin && target < range.end) {
        executableMatches++;
      }
    }
    result.in_unique_executable_section = executableMatches == 1;

    results.push_back(result);
  }

  return guard_coverage_status::ok;
}

} // namespace peparse
