// Hand crafted PE test cases for the guard coverage report. Everything is
// built in memory; nothing is loaded or executed. The cases independently
// exercise old short configs, both PE widths, VA to RVA conversion,
// variable width GFIDS entries, suppression/activation interplay, 16 byte
// slot coverage and unknown metadata.

#include <pe-parse/guard_coverage.h>
#include <pe-parse/parse.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char *expr, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::cerr << "FAIL line " << line << ": " << expr << "\n";
  }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

using peparse::guard_coverage_status;
using peparse::guard_target_coverage;
using peparse::RVA;

const guard_coverage_status kOk = guard_coverage_status::ok;
const guard_coverage_status kNotDeclared = guard_coverage_status::not_declared;
const guard_coverage_status kCorrupt = guard_coverage_status::corrupt;
const guard_coverage_status kInvalidRequest =
    guard_coverage_status::invalid_request;

const guard_target_coverage kCovered = guard_target_coverage::covered;
const guard_target_coverage kIndeterminate =
    guard_target_coverage::indeterminate;
const guard_target_coverage kUncovered = guard_target_coverage::uncovered;

struct sec_spec {
  const char *name;
  std::uint32_t va;
  std::uint32_t vsize;
  std::uint32_t raw_ptr;
  std::uint32_t raw_size;
  std::uint32_t chars;
};

struct gfids_spec {
  std::uint32_t rva;
  std::vector<std::uint8_t> additional;
};

struct pe_spec {
  bool x64;
  std::uint16_t dll_chars;
  std::vector<sec_spec> sections;
  bool has_load_config_dir;
  std::uint32_t dir_rva;
  std::uint32_t dir_size;
  std::uint32_t lc_size;
  bool has_table;
  std::uint64_t table_va; // only used when nonzero
  bool auto_count;        // when true, count follows entries.size()
  std::uint64_t count;
  std::uint32_t guard_flags;
  std::uint32_t gfids_file_off;
  std::vector<gfids_spec> entries;
};

// The default spec mirrors a normal CFG enabled binary: one executable
// .text section, a complete load config and a two entry GFIDS table whose
// second entry is FID suppressed.
pe_spec base_spec(bool x64) {
  pe_spec s;
  s.x64 = x64;
  s.dll_chars = 0x4000; // IMAGE_DLLCHARACTERISTICS_GUARD_CF
  s.sections = {{".text", 0x1000, 0x600, 0x200, 0x600, 0x60000020}};
  s.has_load_config_dir = true;
  s.dir_rva = 0x1100;
  s.dir_size = x64 ? 148 : 92;
  s.lc_size = s.dir_size;
  s.has_table = true;
  s.table_va = 0; // builder substitutes image_base + 0x1200
  s.auto_count = true;
  s.count = 0;
  s.guard_flags = 0x10000500; // stride nibble 1, instrumented, table present
  s.gfids_file_off = 0x400;
  s.entries = {{0x1000, {0x00}}, {0x1017, {0x01}}};
  return s;
}

std::vector<std::uint8_t> build_pe(const pe_spec &spec) {
  const std::uint64_t base = spec.x64 ? 0x140000000ULL : 0x400000ULL;
  const std::uint16_t os = spec.x64 ? 240 : 224;
  const std::size_t nt = 0x80;
  const std::size_t opt = nt + 24;
  const std::size_t sec = opt + os;
  const std::uint32_t stride = 4 + (spec.guard_flags >> 28);

  std::uint64_t need = 0x800;
  need = std::max(need,
                  static_cast<std::uint64_t>(sec + 40 * spec.sections.size()));
  for (const sec_spec &s : spec.sections) {
    need = std::max(need,
                    static_cast<std::uint64_t>(s.raw_ptr) + s.raw_size);
  }
  need = std::max(need, static_cast<std::uint64_t>(0x300) + spec.dir_size);
  need = std::max(need,
                  static_cast<std::uint64_t>(spec.gfids_file_off) +
                      stride * spec.entries.size() + 16);

  std::vector<std::uint8_t> b(need, 0);
  auto put = [&b](std::size_t o, std::uint64_t v, std::size_t n) {
    for (std::size_t i = 0; i < n; i++) {
      b.at(o + i) = static_cast<std::uint8_t>(v >> (8 * i));
    }
  };

  put(0, 0x5a4d, 2);
  put(0x3c, nt, 4);
  put(nt, 0x4550, 4);
  put(nt + 4, spec.x64 ? 0x8664 : 0x14c, 2);
  put(nt + 6, spec.sections.size(), 2);
  put(nt + 20, os, 2);
  put(nt + 22, 0x102, 2);
  put(opt, spec.x64 ? 0x20b : 0x10b, 2);
  put(opt + 16, 0x1000, 4);
  put(opt + (spec.x64 ? 24 : 28), base, spec.x64 ? 8 : 4);
  put(opt + 32, 0x1000, 4);
  put(opt + 36, 0x200, 4);
  put(opt + 56, 0x4000, 4);
  put(opt + 60, 0x200, 4);
  put(opt + 68, 3, 2);
  put(opt + 70, spec.dll_chars, 2);
  put(opt + (spec.x64 ? 108 : 92), 16, 4);

  if (spec.has_load_config_dir) {
    std::size_t dir = opt + (spec.x64 ? 112 : 96) + 10 * 8;
    put(dir, spec.dir_rva, 4);
    put(dir + 4, spec.dir_size, 4);
  }

  b[0x200] = 0xc3; // entry point opcode; may be harmlessly overwritten

  for (std::size_t i = 0; i < spec.sections.size(); i++) {
    const sec_spec &s = spec.sections[i];
    std::size_t o = sec + i * 40;
    std::memcpy(b.data() + o, s.name, std::strlen(s.name));
    put(o + 8, s.vsize, 4);
    put(o + 12, s.va, 4);
    put(o + 16, s.raw_size, 4);
    put(o + 20, s.raw_ptr, 4);
    put(o + 36, s.chars, 4);
  }

  // The load config content always sits at file offset 0x300 (RVA 0x1100
  // inside the default .text); the CFG fields are written even when the
  // declared Size excludes them, so tests prove they are not read then.
  const std::size_t cfg = 0x300;
  put(cfg, spec.lc_size, 4);
  std::uint64_t table_va = 0;
  if (spec.has_table) {
    table_va = spec.table_va != 0 ? spec.table_va : base + 0x1200;
  }
  const std::uint64_t count =
      spec.auto_count ? spec.entries.size() : spec.count;
  if (spec.x64) {
    put(cfg + 128, table_va, 8);
    put(cfg + 136, count, 8);
    put(cfg + 144, spec.guard_flags, 4);
  } else {
    put(cfg + 80, table_va & 0xFFFFFFFFULL, 4);
    put(cfg + 84, count & 0xFFFFFFFFULL, 4);
    put(cfg + 88, spec.guard_flags, 4);
  }

  std::size_t o = spec.gfids_file_off;
  for (const gfids_spec &e : spec.entries) {
    put(o, e.rva, 4);
    for (std::uint32_t k = 0; k + 4 < stride; k++) {
      b.at(o + 4 + k) =
          k < e.additional.size() ? e.additional[k] : std::uint8_t(0);
    }
    o += stride;
  }

  return b;
}

struct fixture {
  std::vector<std::uint8_t> bytes;
  peparse::parsed_pe *pe = nullptr;
  peparse::guard_coverage_metadata meta;

  fixture() = default;
  fixture(const fixture &) = delete;
  fixture &operator=(const fixture &) = delete;
  ~fixture() {
    if (pe != nullptr) {
      peparse::DestructParsedPE(pe);
    }
  }
};

bool load(const pe_spec &spec, fixture &f, int line) {
  f.bytes = build_pe(spec);
  f.pe = peparse::ParsePEFromPointer(f.bytes.data(),
                                     static_cast<std::uint32_t>(f.bytes.size()));
  if (f.pe == nullptr) {
    check(false, "ParsePEFromPointer", line);
    return false;
  }
  peparse::GetGuardCoverageMetadata(f.pe, f.meta);
  return true;
}

#define LOAD(spec, f) load((spec), (f), __LINE__)

// ---------------------------------------------------------------- versions

void versions_both_widths() {
  for (bool x64 : {false, true}) {
    fixture f;
    CHECK(LOAD(base_spec(x64), f));
    CHECK(f.meta.status == kOk);
    CHECK(f.meta.pe32_plus == x64);
    CHECK(f.meta.image_base == (x64 ? 0x140000000ULL : 0x400000ULL));
    CHECK(f.meta.load_config_rva == 0x1100);
    CHECK(f.meta.load_config_directory_size == (x64 ? 148u : 92u));
    CHECK(f.meta.load_config_size == (x64 ? 148u : 92u));
    CHECK(f.meta.guard_cf);
    CHECK(f.meta.cf_instrumented);
    CHECK(f.meta.cf_function_table_present);
    CHECK(f.meta.guard_flags == 0x10000500);
    CHECK(f.meta.guard_cf_function_table_va ==
          f.meta.image_base + 0x1200);
    CHECK(f.meta.guard_cf_function_table_rva == 0x1200);
    CHECK(f.meta.guard_cf_function_count == 2);
    CHECK(f.meta.entry_stride == 5);
    CHECK(f.meta.entries.size() == 2);
    if (f.meta.entries.size() == 2) {
      CHECK(f.meta.entries[0].rva == 0x1000);
      CHECK(f.meta.entries[0].file_offset == 0x400);
      CHECK(f.meta.entries[0].additional.size() == 1);
      CHECK(f.meta.entries[0].additional[0] == 0x00);
      CHECK(!f.meta.entries[0].fid_suppressed);
      CHECK(!f.meta.entries[0].export_suppressed);
      CHECK(!f.meta.entries[0].unknown_metadata);
      CHECK(f.meta.entries[1].rva == 0x1017);
      CHECK(f.meta.entries[1].file_offset == 0x405);
      CHECK(f.meta.entries[1].fid_suppressed);
      CHECK(!f.meta.entries[1].export_suppressed);
    }
    CHECK(f.meta.inconsistencies.empty());
    CHECK(f.meta.executable_sections.size() == 1);
    if (f.meta.executable_sections.size() == 1) {
      CHECK(f.meta.executable_sections[0].name == ".text");
      CHECK(f.meta.executable_sections[0].begin == 0x1000);
      CHECK(f.meta.executable_sections[0].end == 0x1600);
    }
  }
}

void versions_short_config_not_declared() {
  // Old configs end before the CFG field group; that is not_declared,
  // never an empty table. All sizes below sit on field boundaries.
  const std::uint32_t sizes32[] = {0, 4, 64, 80, 84, 88};
  const std::uint32_t sizes64[] = {0, 4, 64, 80, 128, 136, 144};
  for (bool x64 : {false, true}) {
    const std::uint32_t *sizes = x64 ? sizes64 : sizes32;
    std::size_t count = x64 ? 7 : 6;
    for (std::size_t i = 0; i < count; i++) {
      pe_spec spec = base_spec(x64);
      spec.lc_size = sizes[i];
      fixture f;
      CHECK(LOAD(spec, f));
      CHECK(f.meta.status == kNotDeclared);
      CHECK(f.meta.guard_cf); // DllCharacteristics is still reported
      CHECK(!f.meta.cf_instrumented);
      CHECK(f.meta.entries.empty());

      std::vector<peparse::guard_target_result> results;
      CHECK(peparse::QueryGuardCoverage(
                f.meta, {0x1000}, {}, results) == kNotDeclared);
      CHECK(results.empty());
    }
  }
}

void versions_no_directory_not_declared() {
  for (bool x64 : {false, true}) {
    pe_spec spec = base_spec(x64);
    spec.has_load_config_dir = false;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kNotDeclared);
    CHECK(f.meta.guard_cf);
  }
}

void versions_size_mid_scalar_corrupt() {
  // A declared Size that ends inside a scalar field is corrupt.
  const std::uint32_t bad32[] = {1, 3, 5, 81, 90, 91, 93};
  const std::uint32_t bad64[] = {1, 3, 5, 129, 145, 146, 147, 149};
  for (bool x64 : {false, true}) {
    const std::uint32_t *bad = x64 ? bad64 : bad32;
    std::size_t count = x64 ? 8 : 7;
    for (std::size_t i = 0; i < count; i++) {
      pe_spec spec = base_spec(x64);
      spec.lc_size = bad[i];
      fixture f;
      CHECK(LOAD(spec, f));
      CHECK(f.meta.status == kCorrupt);
    }
  }

  // Same check past the default directory size: 163/263 are not field
  // boundaries either.
  {
    pe_spec spec = base_spec(false);
    spec.dir_size = 220;
    spec.lc_size = 163;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
  {
    pe_spec spec = base_spec(true);
    spec.dir_size = 280;
    spec.lc_size = 263;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
}

void versions_size_beyond_directory_corrupt() {
  // The declared Size may not exceed the data directory it lives in.
  for (bool x64 : {false, true}) {
    pe_spec spec = base_spec(x64);
    spec.lc_size = spec.dir_size + 4;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
}

void versions_size_extension_accepted() {
  // Sizes beyond the last field we model belong to newer struct versions;
  // the CFG group is still read from its known offsets.
  for (bool x64 : {false, true}) {
    pe_spec spec = base_spec(x64);
    spec.dir_size = x64 ? 320 : 220;
    spec.lc_size = x64 ? 300 : 200;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kOk);
    CHECK(f.meta.load_config_size == (x64 ? 300u : 200u));
    CHECK(f.meta.entries.size() == 2);
  }
}

void versions_table_in_data_section() {
  // The GFIDS table may live in a non-executable section; its RVAs are
  // still translated through that section's raw data.
  for (bool x64 : {false, true}) {
    pe_spec spec = base_spec(x64);
    spec.sections.push_back({".rdata", 0x2000, 0x200, 0x800, 0x200,
                             0x40000040});
    spec.table_va = (x64 ? 0x140000000ULL : 0x400000ULL) + 0x2000;
    spec.gfids_file_off = 0x800;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kOk);
    CHECK(f.meta.guard_cf_function_table_rva == 0x2000);
    CHECK(f.meta.entries.size() == 2);
    if (f.meta.entries.size() == 2) {
      CHECK(f.meta.entries[0].file_offset == 0x800);
      CHECK(f.meta.entries[1].file_offset == 0x805);
    }
  }
}

// ---------------------------------------------------------------- policies

void policies_flags_and_inconsistencies() {
  // Baseline: everything declared, no contradictions.
  {
    fixture f;
    CHECK(LOAD(base_spec(true), f));
    CHECK(f.meta.inconsistencies.empty());
  }

  // GUARD_CF without CF_INSTRUMENTED.
  {
    pe_spec spec = base_spec(true);
    spec.guard_flags = 0x10000400;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kOk);
    CHECK(f.meta.guard_cf);
    CHECK(!f.meta.cf_instrumented);
    CHECK(f.meta.cf_function_table_present);
    CHECK(f.meta.inconsistencies.size() == 1);
  }

  // CF_INSTRUMENTED without GUARD_CF.
  {
    pe_spec spec = base_spec(true);
    spec.dll_chars = 0;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kOk);
    CHECK(!f.meta.guard_cf);
    CHECK(f.meta.cf_instrumented);
    CHECK(f.meta.inconsistencies.size() == 1);
  }

  // A table without CF_FUNCTION_TABLE_PRESENT.
  {
    pe_spec spec = base_spec(true);
    spec.guard_flags = 0x10000100;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kOk);
    CHECK(!f.meta.cf_function_table_present);
    CHECK(f.meta.entries.size() == 2);
    CHECK(f.meta.inconsistencies.size() == 1);
  }

  // Both mismatches at once are both reported.
  {
    pe_spec spec = base_spec(true);
    spec.guard_flags = 0x10000000;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kOk);
    CHECK(f.meta.inconsistencies.size() == 2);
  }
}

void policies_empty_table_declared() {
  // A null table with a zero count is a declared empty table, which is
  // not the same as not_declared.
  pe_spec spec = base_spec(true);
  spec.has_table = false;
  spec.auto_count = false;
  spec.count = 0;
  fixture f;
  CHECK(LOAD(spec, f));
  CHECK(f.meta.status == kOk);
  CHECK(f.meta.entries.empty());
  CHECK(f.meta.guard_cf_function_count == 0);
  // CF_FUNCTION_TABLE_PRESENT without a table is an inconsistency.
  CHECK(f.meta.inconsistencies.size() == 1);

  std::vector<peparse::guard_target_result> results;
  CHECK(peparse::QueryGuardCoverage(f.meta, {0x1000}, {}, results) == kOk);
  CHECK(results.size() == 1);
  if (results.size() == 1) {
    CHECK(results[0].coverage == kUncovered);
    CHECK(results[0].in_unique_executable_section);
  }
}

void policies_pointer_count_mismatch() {
  // Table and count must be zero together or nonzero together.
  {
    pe_spec spec = base_spec(true);
    spec.has_table = false;
    spec.auto_count = false;
    spec.count = 2;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
  {
    pe_spec spec = base_spec(true);
    spec.auto_count = false;
    spec.count = 0;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
}

void policies_va_conversion() {
  const std::uint64_t base64 = 0x140000000ULL;
  // The VA to RVA subtraction must not truncate 64 bit values.
  {
    pe_spec spec = base_spec(true);
    spec.table_va = base64 + 0x100000000ULL;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
  // A table VA below the image base is corrupt on both widths.
  for (bool x64 : {false, true}) {
    pe_spec spec = base_spec(x64);
    spec.table_va = (x64 ? base64 : 0x400000ULL) - 1;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
}

void policies_count_overflow() {
  // count * stride must not overflow.
  {
    pe_spec spec = base_spec(true);
    spec.auto_count = false;
    spec.count = 0xFFFFFFFFFFFFFFFFULL;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
  {
    pe_spec spec = base_spec(true);
    spec.auto_count = false;
    spec.count = 0x4000000000000000ULL;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
  // A merely huge count runs off the end of every section.
  {
    pe_spec spec = base_spec(false);
    spec.auto_count = false;
    spec.count = 0xFFFFFFFFULL;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
}

void policies_table_location() {
  // Beyond every section.
  {
    pe_spec spec = base_spec(true);
    spec.table_va = 0x140000000ULL + 0x1800;
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }

  // Inside the zero filled part of a section: virtual bytes never stand
  // in for raw data.
  {
    pe_spec spec = base_spec(true);
    spec.sections = {{".text", 0x1000, 0x600, 0x200, 0x200, 0x60000020}};
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }

  // Covered by two overlapping raw mappings: ambiguous, refused.
  {
    pe_spec spec = base_spec(true);
    spec.sections.push_back({".dup", 0x1200, 0x300, 0x500, 0x300,
                             0x40000040});
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
}

void policies_duplicate_targets() {
  // Targets are strictly increasing; duplicates are rejected, not merged.
  {
    pe_spec spec = base_spec(true);
    spec.entries = {{0x1000, {0x00}}, {0x1000, {0x00}}};
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
  {
    pe_spec spec = base_spec(true);
    spec.entries = {{0x1017, {0x00}}, {0x1000, {0x00}}};
    fixture f;
    CHECK(LOAD(spec, f));
    CHECK(f.meta.status == kCorrupt);
  }
}

void policies_activation_validation() {
  fixture f;
  CHECK(LOAD(base_spec(true), f));

  std::vector<peparse::guard_target_result> results;

  // The activation set may not name entries that do not exist.
  results.clear();
  CHECK(peparse::QueryGuardCoverage(
            f.meta, {0x1000}, {0x9999}, results) == kInvalidRequest);
  CHECK(results.empty());

  // Nor entries that are not export suppressed.
  CHECK(peparse::QueryGuardCoverage(
            f.meta, {0x1000}, {0x1000}, results) == kInvalidRequest);
  CHECK(results.empty());

  // A FID suppressed entry is not an export suppressed entry.
  CHECK(peparse::QueryGuardCoverage(
            f.meta, {0x1000}, {0x1017}, results) == kInvalidRequest);
  CHECK(results.empty());
}

void policies_export_suppression_model() {
  pe_spec spec = base_spec(true);
  // Aligned entries keep the arithmetic out of this policy check.
  spec.entries = {{0x1000, {0x00}},   // plain
                  {0x1010, {0x02}},   // export suppressed
                  {0x1020, {0x03}}};  // export suppressed + fid suppressed
  fixture f;
  CHECK(LOAD(spec, f));
  CHECK(f.meta.status == kOk);
  CHECK(f.meta.entries.size() == 3);
  if (f.meta.entries.size() == 3) {
    CHECK(!f.meta.entries[0].export_suppressed);
    CHECK(f.meta.entries[1].export_suppressed);
    CHECK(!f.meta.entries[1].fid_suppressed);
    CHECK(f.meta.entries[2].export_suppressed);
    CHECK(f.meta.entries[2].fid_suppressed);
  }

  std::vector<peparse::guard_target_result> results;

  // Without activation only the plain entry is usable.
  CHECK(peparse::QueryGuardCoverage(
            f.meta, {0x1000, 0x1010, 0x1020}, {}, results) == kOk);
  CHECK(results.size() == 3);
  if (results.size() == 3) {
    CHECK(results[0].coverage == kCovered);
    CHECK(results[1].coverage == kUncovered);
    CHECK(results[2].coverage == kUncovered);
  }

  // Activating the export suppressed entry makes it usable; the entry
  // that also carries FID_SUPPRESSED stays unusable even when activated.
  CHECK(peparse::QueryGuardCoverage(
            f.meta, {0x1010, 0x1020}, {0x1010, 0x1020}, results) == kOk);
  CHECK(results.size() == 2);
  if (results.size() == 2) {
    CHECK(results[0].coverage == kCovered);
    CHECK(results[0].covering_entries.size() == 1);
    if (results[0].covering_entries.size() == 1) {
      CHECK(results[0].covering_entries[0].index == 1);
      CHECK(results[0].covering_entries[0].rva == 0x1010);
      CHECK(results[0].covering_entries[0].file_offset == 0x405);
    }
    CHECK(results[1].coverage == kUncovered);
  }
}

// ------------------------------------------------------------------- slots

void slots_aligned_covers_self() {
  pe_spec spec = base_spec(true);
  spec.entries = {{0x1000, {0x00}}};
  fixture f;
  CHECK(LOAD(spec, f));

  std::vector<peparse::guard_target_result> results;
  CHECK(peparse::QueryGuardCoverage(
            f.meta, {0x0FFF, 0x1000, 0x1001, 0x100F, 0x1010}, {}, results) ==
        kOk);
  CHECK(results.size() == 5);
  if (results.size() == 5) {
    CHECK(results[0].coverage == kUncovered);
    CHECK(results[1].coverage == kCovered);
    CHECK(results[2].coverage == kUncovered);
    CHECK(results[3].coverage == kUncovered);
    CHECK(results[4].coverage == kUncovered);
    CHECK(results[1].covering_entries.size() == 1);
    if (results[1].covering_entries.size() == 1) {
      CHECK(results[1].covering_entries[0].index == 0);
      CHECK(results[1].covering_entries[0].rva == 0x1000);
      CHECK(results[1].covering_entries[0].file_offset == 0x400);
    }
  }
}

void slots_unaligned_covers_slot() {
  pe_spec spec = base_spec(true);
  spec.entries = {{0x1017, {0x00}}};
  fixture f;
  CHECK(LOAD(spec, f));

  // An unaligned usable entry covers its whole 16 byte slot.
  std::vector<peparse::guard_target_result> results;
  CHECK(peparse::QueryGuardCoverage(
            f.meta, {0x100F, 0x1010, 0x1017, 0x101F, 0x1020}, {}, results) ==
        kOk);
  CHECK(results.size() == 5);
  if (results.size() == 5) {
    CHECK(results[0].coverage == kUncovered);
    CHECK(results[1].coverage == kCovered);
    CHECK(results[2].coverage == kCovered);
    CHECK(results[3].coverage == kCovered);
    CHECK(results[4].coverage == kUncovered);
  }
}

void slots_same_slot_suppressed_covered() {
  // A suppressed address is still covered when a usable entry shares its
  // slot; an exact match on the suppressed entry must not end the search.
  pe_spec spec = base_spec(true);
  spec.entries = {{0x1011, {0x00}}, {0x1017, {0x01}}};
  fixture f;
  CHECK(LOAD(spec, f));

  std::vector<peparse::guard_target_result> results;
  CHECK(peparse::QueryGuardCoverage(f.meta, {0x1017}, {}, results) == kOk);
  CHECK(results.size() == 1);
  if (results.size() == 1) {
    CHECK(results[0].coverage == kCovered);
    CHECK(results[0].covering_entries.size() == 1);
    if (results[0].covering_entries.size() == 1) {
      CHECK(results[0].covering_entries[0].rva == 0x1011);
    }
  }

  // Without the slot mate the suppressed entry itself covers nothing.
  pe_spec lone = base_spec(true);
  lone.entries = {{0x1017, {0x01}}};
  fixture f2;
  CHECK(LOAD(lone, f2));
  CHECK(peparse::QueryGuardCoverage(f2.meta, {0x1017}, {}, results) == kOk);
  CHECK(results.size() == 1);
  if (results.size() == 1) {
    CHECK(results[0].coverage == kUncovered);
  }
}

void slots_unknown_metadata() {
  // Unknown bits make an entry a source of potential coverage only.
  pe_spec spec = base_spec(true);
  spec.entries = {{0x1000, {0x00}}, {0x1040, {0x04}}};
  fixture f;
  CHECK(LOAD(spec, f));
  CHECK(f.meta.status == kOk);
  CHECK(f.meta.entries.size() == 2);
  if (f.meta.entries.size() == 2) {
    CHECK(!f.meta.entries[0].unknown_metadata);
    CHECK(f.meta.entries[1].unknown_metadata);
    CHECK(!f.meta.entries[1].fid_suppressed);
    CHECK(!f.meta.entries[1].export_suppressed);
  }

  std::vector<peparse::guard_target_result> results;
  CHECK(peparse::QueryGuardCoverage(
            f.meta, {0x1000, 0x1040, 0x2000}, {}, results) == kOk);
  CHECK(results.size() == 3);
  if (results.size() == 3) {
    CHECK(results[0].coverage == kCovered);
    CHECK(results[1].coverage == kIndeterminate);
    CHECK(results[1].covering_entries.empty());
    CHECK(results[1].potential_entries.size() == 1);
    if (results[1].potential_entries.size() == 1) {
      CHECK(results[1].potential_entries[0].index == 1);
      CHECK(results[1].potential_entries[0].rva == 0x1040);
    }
    CHECK(results[2].coverage == kUncovered);
    CHECK(results[2].potential_entries.empty());
  }

  // Definite coverage wins over potential coverage in the same slot, and
  // both producing entries are still listed.
  pe_spec mixed = base_spec(true);
  mixed.entries = {{0x1041, {0x00}}, {0x1043, {0x04}}};
  fixture f2;
  CHECK(LOAD(mixed, f2));
  CHECK(peparse::QueryGuardCoverage(f2.meta, {0x1042}, {}, results) == kOk);
  CHECK(results.size() == 1);
  if (results.size() == 1) {
    CHECK(results[0].coverage == kCovered);
    CHECK(results[0].covering_entries.size() == 1);
    CHECK(results[0].potential_entries.size() == 1);
    if (results[0].covering_entries.size() == 1 &&
        results[0].potential_entries.size() == 1) {
      CHECK(results[0].covering_entries[0].rva == 0x1041);
      CHECK(results[0].potential_entries[0].rva == 0x1043);
    }
  }
}

void slots_variable_width_entries() {
  // GuardFlags high nibble 2: two additional bytes per entry. A nonzero
  // trailing byte is unknown metadata; the raw bytes are kept in order.
  pe_spec spec = base_spec(true);
  spec.guard_flags = 0x20000500;
  spec.entries = {{0x1000, {0x00, 0x00}},
                  {0x1017, {0x01, 0x00}},
                  {0x1020, {0x02, 0x09}}};
  fixture f;
  CHECK(LOAD(spec, f));
  CHECK(f.meta.status == kOk);
  CHECK(f.meta.entry_stride == 6);
  CHECK(f.meta.entries.size() == 3);
  if (f.meta.entries.size() == 3) {
    CHECK(f.meta.entries[0].file_offset == 0x400);
    CHECK(f.meta.entries[1].file_offset == 0x406);
    CHECK(f.meta.entries[2].file_offset == 0x40C);
    CHECK(f.meta.entries[0].additional.size() == 2);
    CHECK(!f.meta.entries[0].unknown_metadata);
    CHECK(f.meta.entries[1].fid_suppressed);
    CHECK(!f.meta.entries[1].unknown_metadata);
    CHECK(f.meta.entries[2].export_suppressed);
    CHECK(f.meta.entries[2].unknown_metadata);
    if (f.meta.entries[2].additional.size() == 2) {
      CHECK(f.meta.entries[2].additional[0] == 0x02);
      CHECK(f.meta.entries[2].additional[1] == 0x09);
    }
  }

  // Even an activated export suppressed entry stays potential only when
  // it carries unknown metadata.
  std::vector<peparse::guard_target_result> results;
  CHECK(peparse::QueryGuardCoverage(
            f.meta, {0x1020}, {0x1020}, results) == kOk);
  CHECK(results.size() == 1);
  if (results.size() == 1) {
    CHECK(results[0].coverage == kIndeterminate);
    CHECK(results[0].potential_entries.size() == 1);
  }
}

void slots_result_order_and_duplicates() {
  fixture f;
  CHECK(LOAD(base_spec(true), f));

  std::vector<peparse::guard_target_result> results;
  CHECK(peparse::QueryGuardCoverage(
            f.meta, {0x1017, 0x1000, 0x1017, 0x2000, 0x1000}, {}, results) ==
        kOk);
  CHECK(results.size() == 5);
  if (results.size() == 5) {
    CHECK(results[0].rva == 0x1017);
    CHECK(results[1].rva == 0x1000);
    CHECK(results[2].rva == 0x1017);
    CHECK(results[3].rva == 0x2000);
    CHECK(results[4].rva == 0x1000);
    CHECK(results[0].coverage == kUncovered);
    CHECK(results[1].coverage == kCovered);
    CHECK(results[2].coverage == results[0].coverage);
    CHECK(results[3].coverage == kUncovered);
    CHECK(results[4].coverage == results[1].coverage);
  }
}

void slots_executable_annotation() {
  // Coverage and executability are separate facts: .code and .code2 are
  // both executable and overlap, .data is not executable.
  pe_spec spec = base_spec(true);
  spec.sections.push_back({".code", 0x2000, 0x100, 0x800, 0x100,
                           0x60000020});
  spec.sections.push_back({".code2", 0x2080, 0x100, 0x900, 0x100,
                           0x60000020});
  spec.sections.push_back({".data", 0x3000, 0x100, 0xA00, 0x100,
                           0xC0000040});
  fixture f;
  CHECK(LOAD(spec, f));
  CHECK(f.meta.status == kOk);
  CHECK(f.meta.executable_sections.size() == 3);

  std::vector<peparse::guard_target_result> results;
  CHECK(peparse::QueryGuardCoverage(
            f.meta, {0x1000, 0x2050, 0x20C0, 0x3010, 0x9000}, {},
            results) == kOk);
  CHECK(results.size() == 5);
  if (results.size() == 5) {
    CHECK(results[0].coverage == kCovered);
    CHECK(results[0].in_unique_executable_section);
    CHECK(results[1].coverage == kUncovered);
    CHECK(results[1].in_unique_executable_section);
    CHECK(results[2].coverage == kUncovered);
    CHECK(!results[2].in_unique_executable_section); // two matches
    CHECK(results[3].coverage == kUncovered);
    CHECK(!results[3].in_unique_executable_section); // not executable
    CHECK(results[4].coverage == kUncovered);
    CHECK(!results[4].in_unique_executable_section); // no section
  }
}

void slots_results_outlive_pe() {
  std::vector<std::uint8_t> bytes = build_pe(base_spec(true));
  peparse::parsed_pe *pe = peparse::ParsePEFromPointer(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  CHECK(pe != nullptr);

  peparse::guard_coverage_metadata meta;
  CHECK(peparse::GetGuardCoverageMetadata(pe, meta) == kOk);
  std::vector<peparse::guard_target_result> results;
  CHECK(peparse::QueryGuardCoverage(
            meta, {0x1000, 0x1017}, {}, results) == kOk);

  peparse::DestructParsedPE(pe);

  // The report and the results own their bytes and strings.
  CHECK(meta.entries.size() == 2);
  if (meta.entries.size() == 2) {
    CHECK(meta.entries[1].additional.size() == 1);
    if (meta.entries[1].additional.size() == 1) {
      CHECK(meta.entries[1].additional[0] == 0x01);
    }
  }
  CHECK(meta.executable_sections.size() == 1);
  if (meta.executable_sections.size() == 1) {
    CHECK(meta.executable_sections[0].name == ".text");
  }
  CHECK(results.size() == 2);
  if (results.size() == 2) {
    CHECK(results[0].coverage == kCovered);
    CHECK(results[1].coverage == kUncovered);
  }
}

void slots_query_status_passthrough() {
  std::vector<peparse::guard_target_result> results;

  // A config without the CFG group answers queries with not_declared.
  pe_spec short_config = base_spec(true);
  short_config.lc_size = 64;
  fixture f1;
  CHECK(LOAD(short_config, f1));
  CHECK(f1.meta.status == kNotDeclared);
  CHECK(peparse::QueryGuardCoverage(f1.meta, {0x1000}, {}, results) ==
        kNotDeclared);
  CHECK(results.empty());

  // A corrupt table cannot answer queries at all.
  pe_spec dup = base_spec(true);
  dup.entries = {{0x1000, {0x00}}, {0x1000, {0x00}}};
  fixture f2;
  CHECK(LOAD(dup, f2));
  CHECK(f2.meta.status == kCorrupt);
  CHECK(peparse::QueryGuardCoverage(f2.meta, {0x1000}, {}, results) ==
        kCorrupt);
  CHECK(results.empty());
}

// -------------------------------------------------------------------- main

void versions() {
  versions_both_widths();
  versions_short_config_not_declared();
  versions_no_directory_not_declared();
  versions_size_mid_scalar_corrupt();
  versions_size_beyond_directory_corrupt();
  versions_size_extension_accepted();
  versions_table_in_data_section();
}

void policies() {
  policies_flags_and_inconsistencies();
  policies_empty_table_declared();
  policies_pointer_count_mismatch();
  policies_va_conversion();
  policies_count_overflow();
  policies_table_location();
  policies_duplicate_targets();
  policies_activation_validation();
  policies_export_suppression_model();
}

void slots() {
  slots_aligned_covers_self();
  slots_unaligned_covers_slot();
  slots_same_slot_suppressed_covered();
  slots_unknown_metadata();
  slots_variable_width_entries();
  slots_result_order_and_duplicates();
  slots_executable_annotation();
  slots_results_outlive_pe();
  slots_query_status_passthrough();
}

int group(const char *name, void (*fn)()) {
  int before = g_failures;
  fn();
  if (g_failures == before) {
    std::cout << "PASS " << name << "\n";
    return 0;
  }
  std::cout << "FAIL " << name << "\n";
  return 1;
}

} // namespace

int main() {
  int rc = 0;
  rc |= group("versions", versions);
  rc |= group("policies", policies);
  rc |= group("slots", slots);
  if (rc == 0) {
    std::cout << "GUARD_COVERAGE_OK\n";
  } else {
    std::cerr << g_failures << " of " << g_checks << " checks failed\n";
  }
  return rc;
}
