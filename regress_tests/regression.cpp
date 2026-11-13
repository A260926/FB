// Regression tests for pe-parse's import binding plan.
//
// Everything here is self-contained: the PE images and API-set namespaces are
// generated in memory and parsed with ParsePEFromPointer.  No system DLL,
// registry entry or host file is consulted.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <pe-parse/import_binding.h>
#include <pe-parse/parse.h>

#include "support/check.h"

namespace {

const std::uint32_t kSectionRva = 0x1000;
const std::uint32_t kSectionRaw = 0x400;
const std::uint32_t kSectionRawSize = 0x1000;
const std::uint32_t kSectionVirtualSize = 0x1000;
const std::uint32_t kFileSize = kSectionRaw + kSectionRawSize;

const std::uint32_t kImageBase32 = 0x400000;
const std::uint64_t kImageBase64 = 0x140000000ull;

const std::uint32_t kDescBase = 0x1000;
const std::uint32_t kIltBase = 0x1200;
const std::uint32_t kIatBase = 0x1400;
const std::uint32_t kImportNameBase = 0x1600;
const std::uint32_t kHintNameBase = 0x1700;
const std::uint32_t kExportDirRva = 0x1800;
const std::uint32_t kExportDirSize = 0x500;
const std::uint32_t kExportDirEnd = kExportDirRva + kExportDirSize;
const std::uint32_t kEatBase = 0x1840;
const std::uint32_t kNamePtrBase = 0x1940;
const std::uint32_t kOrdinalBaseRva = 0x1A40;
const std::uint32_t kExportModuleName = 0x1B40;
const std::uint32_t kExportNameBase = 0x1B80;
const std::uint32_t kForwardBase = 0x1C80;
const std::uint32_t kTruncatedForwardRva = 0x1CE0;

std::size_t FileOff(std::uint32_t rva) {
  return static_cast<std::size_t>(kSectionRaw + (rva - kSectionRva));
}

void Put16(std::vector<std::uint8_t> &b, std::size_t off, std::uint16_t v) {
  b[off] = static_cast<std::uint8_t>(v & 0xFF);
  b[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void Put32(std::vector<std::uint8_t> &b, std::size_t off, std::uint32_t v) {
  b[off] = static_cast<std::uint8_t>(v & 0xFF);
  b[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
  b[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
  b[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

void Put64(std::vector<std::uint8_t> &b, std::size_t off, std::uint64_t v) {
  for (std::size_t i = 0; i < 8; ++i) {
    b[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  }
}

void WriteStringAtRva(std::vector<std::uint8_t> &b,
                      std::uint32_t rva,
                      const std::string &s) {
  std::size_t off = FileOff(rva);
  UNIT_CHECK(off + s.size() + 1 <= b.size());
  std::memcpy(b.data() + off, s.data(), s.size());
  b[off + s.size()] = 0;
}

struct ThunkSpec {
  bool byOrdinal;
  std::string name;
  std::uint16_t ordinal;
};

ThunkSpec ByName(const std::string &name) {
  ThunkSpec t;
  t.byOrdinal = false;
  t.name = name;
  t.ordinal = 0;
  return t;
}

ThunkSpec ByOrdinal(std::uint16_t ordinal) {
  ThunkSpec t;
  t.byOrdinal = true;
  t.ordinal = ordinal;
  return t;
}

struct ImportModuleSpec {
  std::string name;
  std::vector<ThunkSpec> thunks;
};

ImportModuleSpec MakeImport(const std::string &name,
                            const std::vector<ThunkSpec> &thunks) {
  ImportModuleSpec m;
  m.name = name;
  m.thunks = thunks;
  return m;
}

struct ExportSlotSpec {
  std::uint32_t rva;
  std::vector<std::string> names;
  std::string forward;
  bool truncatedForward;
};

ExportSlotSpec Slot(std::uint32_t rva, const std::vector<std::string> &names) {
  ExportSlotSpec s;
  s.rva = rva;
  s.names = names;
  s.forward.clear();
  s.truncatedForward = false;
  return s;
}

ExportSlotSpec ForwardSlot(const std::string &forward,
                           const std::vector<std::string> &names) {
  ExportSlotSpec s;
  s.rva = 0;
  s.names = names;
  s.forward = forward;
  s.truncatedForward = false;
  return s;
}

ExportSlotSpec TruncatedForwardSlot(const std::string &forward,
                                    const std::vector<std::string> &names) {
  ExportSlotSpec s;
  s.rva = 0;
  s.names = names;
  s.forward = forward;
  s.truncatedForward = true;
  return s;
}

struct ExportSpec {
  std::string moduleName;
  std::uint32_t ordinalBase;
  std::vector<ExportSlotSpec> slots;
};

ExportSpec MakeExports(const std::string &moduleName,
                       std::uint32_t ordinalBase,
                       const std::vector<ExportSlotSpec> &slots) {
  ExportSpec e;
  e.moduleName = moduleName;
  e.ordinalBase = ordinalBase;
  e.slots = slots;
  return e;
}

class PeImage {
public:
  explicit PeImage(bool pe64) : pe64_(pe64), hasExports_(false) {
  }

  void setImports(const std::vector<ImportModuleSpec> &imports) {
    imports_ = imports;
  }

  void setExports(const ExportSpec &exports) {
    exports_ = exports;
    hasExports_ = true;
  }

  std::uint64_t imageBase() const {
    return pe64_ ? kImageBase64 : kImageBase32;
  }

  std::vector<std::uint8_t> build() const {
    std::vector<std::uint8_t> b(kFileSize, 0);

    Put16(b, 0, 0x5A4D);
    Put32(b, 0x3C, 0x80);

    const std::size_t nt = 0x80;
    Put32(b, nt, 0x4550);
    const std::size_t fh = nt + 4;
    Put16(b, fh + 0, pe64_ ? 0x8664 : 0x14C);
    Put16(b, fh + 2, 1);
    Put32(b, fh + 4, 0);
    Put32(b, fh + 8, 0);
    Put32(b, fh + 12, 0);
    Put16(b, fh + 16, pe64_ ? 240 : 224);
    Put16(b, fh + 18, pe64_ ? 0x2022 : 0x2102);

    const std::size_t opt = nt + 24;
    if (pe64_) {
      Put16(b, opt, 0x20B);
      Put32(b, opt + 16, kSectionRva);
      Put64(b, opt + 24, kImageBase64);
      Put32(b, opt + 32, 0x1000);
      Put32(b, opt + 36, 0x200);
      Put32(b, opt + 56, 0x2000);
      Put32(b, opt + 60, 0x400);
      Put16(b, opt + 68, 3);
      Put32(b, opt + 108, 16);
    } else {
      Put16(b, opt, 0x10B);
      Put32(b, opt + 16, kSectionRva);
      Put32(b, opt + 28, kImageBase32);
      Put32(b, opt + 32, 0x1000);
      Put32(b, opt + 36, 0x200);
      Put32(b, opt + 56, 0x2000);
      Put32(b, opt + 60, 0x400);
      Put16(b, opt + 68, 3);
      Put32(b, opt + 92, 16);
    }

    const std::size_t sec = opt + (pe64_ ? 240 : 224);
    std::memcpy(b.data() + sec, ".rdata", 6);
    Put32(b, sec + 8, kSectionVirtualSize);
    Put32(b, sec + 12, kSectionRva);
    Put32(b, sec + 16, kSectionRawSize);
    Put32(b, sec + 20, kSectionRaw);
    Put32(b, sec + 36, 0x40000040);

    const std::size_t dirOff = opt + (pe64_ ? 112 : 96);
    const std::uint32_t width = pe64_ ? 8 : 4;

    if (!imports_.empty()) {
      UNIT_CHECK(imports_.size() <= 4);
      std::uint32_t hintCursor = kHintNameBase;

      for (std::size_t i = 0; i < imports_.size(); ++i) {
        const std::uint32_t desc = kDescBase +
                                   static_cast<std::uint32_t>(i) * 20;
        const std::uint32_t ilt = kIltBase + static_cast<std::uint32_t>(i) *
                                               0x40;
        const std::uint32_t iat = kIatBase + static_cast<std::uint32_t>(i) *
                                               0x40;
        const std::uint32_t nameRva =
            kImportNameBase + static_cast<std::uint32_t>(i) * 0x40;

        WriteStringAtRva(b, nameRva, imports_[i].name);
        UNIT_CHECK(imports_[i].thunks.size() <= 8);

        for (std::size_t j = 0; j < imports_[i].thunks.size(); ++j) {
          const ThunkSpec &thunk = imports_[i].thunks[j];
          const std::uint32_t slot =
              ilt + static_cast<std::uint32_t>(j) * width;
          if (thunk.byOrdinal) {
            std::uint64_t tagged =
                (pe64_ ? (1ull << 63) : (1ull << 31)) |
                static_cast<std::uint64_t>(thunk.ordinal);
            if (pe64_) {
              Put64(b, FileOff(slot), tagged);
            } else {
              Put32(b, FileOff(slot),
                    static_cast<std::uint32_t>(tagged));
            }
          } else {
            UNIT_CHECK(hintCursor < kExportDirRva);
            const std::uint32_t entryRva = hintCursor;
            Put16(b, FileOff(entryRva), 0);
            WriteStringAtRva(b, entryRva + 2, thunk.name);
            hintCursor += static_cast<std::uint32_t>(thunk.name.size()) + 3;
            if (pe64_) {
              Put64(b, FileOff(slot), entryRva);
            } else {
              Put32(b, FileOff(slot), entryRva);
            }
          }
        }

        Put32(b, FileOff(desc + 0), ilt);
        Put32(b, FileOff(desc + 12), nameRva);
        Put32(b, FileOff(desc + 16), iat);
      }

      Put32(b, dirOff + 8, kDescBase);
      Put32(b, dirOff + 12,
            static_cast<std::uint32_t>(imports_.size() + 1) * 20);
    }

    if (hasExports_) {
      UNIT_CHECK(exports_.slots.size() <= 16);
      Put32(b, FileOff(kExportDirRva + 12), kExportModuleName);
      Put32(b, FileOff(kExportDirRva + 16), exports_.ordinalBase);
      Put32(b, FileOff(kExportDirRva + 20),
            static_cast<std::uint32_t>(exports_.slots.size()));
      Put32(b, FileOff(kExportDirRva + 28), kEatBase);
      Put32(b, FileOff(kExportDirRva + 32), kNamePtrBase);
      Put32(b, FileOff(kExportDirRva + 36), kOrdinalBaseRva);
      WriteStringAtRva(b, kExportModuleName, exports_.moduleName);

      std::uint32_t namePtrCursor = kNamePtrBase;
      std::uint32_t ordinalCursor = kOrdinalBaseRva;
      std::uint32_t nameCursor = kExportNameBase;
      std::uint32_t forwardCursor = kForwardBase;
      std::uint32_t nameCount = 0;

      for (std::size_t i = 0; i < exports_.slots.size(); ++i) {
        const ExportSlotSpec &slot = exports_.slots[i];
        std::uint32_t value = slot.rva;

        if (slot.truncatedForward) {
          value = kTruncatedForwardRva;
          std::size_t off = FileOff(kTruncatedForwardRva);
          UNIT_CHECK(slot.forward.size() <= 0x20);
          std::memcpy(b.data() + off, slot.forward.data(),
                      slot.forward.size());
          for (std::size_t p = off + slot.forward.size();
               p < FileOff(kExportDirEnd); ++p) {
            b[p] = 'X';
          }
          b[FileOff(kExportDirEnd)] = 0;
        } else if (!slot.forward.empty()) {
          value = forwardCursor;
          WriteStringAtRva(b, forwardCursor, slot.forward);
          forwardCursor +=
              static_cast<std::uint32_t>(slot.forward.size()) + 1;
          UNIT_CHECK(forwardCursor <= kTruncatedForwardRva);
        }

        Put32(b, FileOff(kEatBase + static_cast<std::uint32_t>(i) * 4),
              value);

        for (const std::string &name : slot.names) {
          UNIT_CHECK(nameCount < 16);
          Put32(b, FileOff(namePtrCursor), nameCursor);
          UNIT_CHECK(exports_.ordinalBase + i <= 0xFFFF);
          Put16(b, FileOff(ordinalCursor),
                static_cast<std::uint16_t>(exports_.ordinalBase +
                                           static_cast<std::uint32_t>(i)));
          WriteStringAtRva(b, nameCursor, name);
          nameCursor += static_cast<std::uint32_t>(name.size()) + 1;
          UNIT_CHECK(nameCursor <= kForwardBase);
          namePtrCursor += 4;
          ordinalCursor += 2;
          ++nameCount;
        }
      }

      Put32(b, FileOff(kExportDirRva + 24), nameCount);
      Put32(b, dirOff + 0, kExportDirRva);
      Put32(b, dirOff + 4, kExportDirSize);
    }

    return b;
  }

private:
  bool pe64_;
  std::vector<ImportModuleSpec> imports_;
  ExportSpec exports_;
  bool hasExports_;
};

std::uint32_t HashPrefix(const std::string &prefix, std::uint32_t factor) {
  std::uint32_t hash = 0;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    char c = prefix[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    hash = hash * factor + static_cast<std::uint32_t>(
                               static_cast<std::uint8_t>(c));
  }
  return hash;
}

std::string SchemaPrefix(const std::string &name) {
  std::size_t dash = name.find_last_of('-');
  if (dash == std::string::npos) {
    return name;
  }
  return name.substr(0, dash);
}

struct SchemaValueSpec {
  std::string alias;
  std::string host;
};

SchemaValueSpec Value(const std::string &alias, const std::string &host) {
  SchemaValueSpec v;
  v.alias = alias;
  v.host = host;
  return v;
}

struct SchemaEntrySpec {
  std::string name;
  std::vector<SchemaValueSpec> values;
};

SchemaEntrySpec Entry(const std::string &name,
                      const std::vector<SchemaValueSpec> &values) {
  SchemaEntrySpec e;
  e.name = name;
  e.values = values;
  return e;
}

std::vector<std::uint8_t> BuildNamespace(
    std::uint32_t factor, const std::vector<SchemaEntrySpec> &specs,
    std::uint32_t version = 6) {
  const std::uint32_t count = static_cast<std::uint32_t>(specs.size());
  const std::uint32_t entryOff = 28;
  const std::uint32_t hashOff = entryOff + 24 * count;

  std::uint32_t valueCount = 0;
  for (std::size_t i = 0; i < specs.size(); ++i) {
    valueCount += static_cast<std::uint32_t>(specs[i].values.size());
  }

  const std::uint32_t valueBase = hashOff + 8 * count;
  const std::uint32_t stringsBase = valueBase + 20 * valueCount;

  std::vector<std::uint8_t> b(stringsBase, 0);

  std::vector<std::uint32_t> nameOffsets(specs.size(), 0);
  std::vector<std::uint32_t> nameLengths(specs.size(), 0);

  struct ValueRecord {
    std::uint32_t aliasOff;
    std::uint32_t aliasLen;
    std::uint32_t hostOff;
    std::uint32_t hostLen;
  };
  std::vector<std::vector<ValueRecord> > records(specs.size());

  for (std::size_t i = 0; i < specs.size(); ++i) {
    nameOffsets[i] = static_cast<std::uint32_t>(b.size());
    const std::string &name = specs[i].name;
    for (std::size_t k = 0; k < name.size(); ++k) {
      b.push_back(static_cast<std::uint8_t>(
          static_cast<std::uint8_t>(name[k])));
      b.push_back(0);
    }
    nameLengths[i] = static_cast<std::uint32_t>(name.size() * 2);

    for (std::size_t v = 0; v < specs[i].values.size(); ++v) {
      ValueRecord record;
      record.aliasOff = 0;
      record.aliasLen = 0;
      record.hostOff = 0;
      record.hostLen = 0;

      const SchemaValueSpec &value = specs[i].values[v];
      if (!value.alias.empty()) {
        record.aliasOff = static_cast<std::uint32_t>(b.size());
        for (std::size_t k = 0; k < value.alias.size(); ++k) {
          b.push_back(static_cast<std::uint8_t>(
              static_cast<std::uint8_t>(value.alias[k])));
          b.push_back(0);
        }
        record.aliasLen = static_cast<std::uint32_t>(value.alias.size() * 2);
      }
      if (!value.host.empty()) {
        record.hostOff = static_cast<std::uint32_t>(b.size());
        for (std::size_t k = 0; k < value.host.size(); ++k) {
          b.push_back(static_cast<std::uint8_t>(
              static_cast<std::uint8_t>(value.host[k])));
          b.push_back(0);
        }
        record.hostLen = static_cast<std::uint32_t>(value.host.size() * 2);
      }
      records[i].push_back(record);
    }
  }

  std::uint32_t valueCursor = valueBase;
  for (std::size_t i = 0; i < specs.size(); ++i) {
    for (std::size_t v = 0; v < records[i].size(); ++v) {
      Put32(b, valueCursor + 0, 0);
      Put32(b, valueCursor + 4, records[i][v].aliasOff);
      Put32(b, valueCursor + 8, records[i][v].aliasLen);
      Put32(b, valueCursor + 12, records[i][v].hostOff);
      Put32(b, valueCursor + 16, records[i][v].hostLen);
      valueCursor += 20;
    }
  }

  valueCursor = valueBase;
  for (std::size_t i = 0; i < specs.size(); ++i) {
    const std::uint32_t record = entryOff +
                                 static_cast<std::uint32_t>(i) * 24;
    const std::string prefix = SchemaPrefix(specs[i].name);
    Put32(b, record + 0, 0);
    Put32(b, record + 4, nameOffsets[i]);
    Put32(b, record + 8, nameLengths[i]);
    Put32(b, record + 12, static_cast<std::uint32_t>(prefix.size() * 2));
    Put32(b, record + 16, valueCursor);
    Put32(b, record + 20,
          static_cast<std::uint32_t>(specs[i].values.size()));
    valueCursor += static_cast<std::uint32_t>(specs[i].values.size()) * 20;
  }

  std::vector<std::pair<std::uint32_t, std::uint32_t> > hashes;
  for (std::size_t i = 0; i < specs.size(); ++i) {
    hashes.push_back(std::make_pair(
        HashPrefix(SchemaPrefix(specs[i].name), factor),
        static_cast<std::uint32_t>(i)));
  }
  std::stable_sort(hashes.begin(), hashes.end());

  for (std::size_t i = 0; i < hashes.size(); ++i) {
    const std::uint32_t record = hashOff + static_cast<std::uint32_t>(i) * 8;
    Put32(b, record + 0, hashes[i].first);
    Put32(b, record + 4, hashes[i].second);
  }

  const std::uint32_t totalSize = static_cast<std::uint32_t>(b.size());
  Put32(b, 0, version);
  Put32(b, 4, totalSize);
  Put32(b, 8, 0);
  Put32(b, 12, count);
  Put32(b, 16, entryOff);
  Put32(b, 20, hashOff);
  Put32(b, 24, factor);

  return b;
}

// Namespace whose value table offset/count multiply-adds past 32 bits.
std::vector<std::uint8_t> BuildOverflowNamespace() {
  std::vector<std::uint8_t> b(80, 0);
  Put32(b, 0, 6);
  Put32(b, 4, 80);
  Put32(b, 8, 0);
  Put32(b, 12, 1);
  Put32(b, 16, 28);
  Put32(b, 20, 52);
  Put32(b, 24, 1);

  Put32(b, 28, 0);
  Put32(b, 32, 60);
  Put32(b, 36, 16);
  Put32(b, 40, 10);
  Put32(b, 44, 0xFFFFFFF0u);
  Put32(b, 48, 0x10000000u);

  Put32(b, 52, HashPrefix("api-q", 1));
  Put32(b, 56, 0);

  const std::string name = "api-q-l1";
  for (std::size_t i = 0; i < name.size(); ++i) {
    b[60 + i * 2] = static_cast<std::uint8_t>(name[i]);
    b[60 + i * 2 + 1] = 0;
  }

  return b;
}

struct ModuleSet {
  std::vector<std::vector<std::uint8_t> > images;
  std::vector<peparse::parsed_pe *> pes;

  ~ModuleSet() {
    release();
  }

  void release() {
    for (std::size_t i = 0; i < pes.size(); ++i) {
      peparse::DestructParsedPE(pes[i]);
    }
    pes.clear();
  }

  std::size_t add(const PeImage &image) {
    images.push_back(image.build());
    std::vector<std::uint8_t> &data = images.back();
    peparse::parsed_pe *pe = peparse::ParsePEFromPointer(
        data.data(), static_cast<std::uint32_t>(data.size()));
    if (pe == nullptr) {
      std::fprintf(stderr, "ParsePEFromPointer failed: %s\n",
                   peparse::GetPEErrString().c_str());
    }
    pes.push_back(pe);
    return pes.size() - 1;
  }

  peparse::parsed_pe *pe(std::size_t i) const {
    return pes[i];
  }
};

peparse::BindingModuleInput Input(const std::string &name,
                                  peparse::parsed_pe *pe) {
  peparse::BindingModuleInput input;
  input.fileName = name;
  input.pe = pe;
  return input;
}

std::string RenderPlan(const peparse::ImportBindingPlan &plan) {
  std::string out;
  for (std::size_t i = 0; i < plan.Rows().size(); ++i) {
    const peparse::BindingRow &row = plan.Rows()[i];
    out += peparse::BindingStatusName(row.status);
    out += "|";
    out += row.request.moduleName;
    out += "|";
    out += row.request.normalizedModule;
    out += "|";
    if (row.request.kind == peparse::ImportNameKind::ByName) {
      out += row.request.symbolName;
    } else {
      out += "#" + std::to_string(row.request.ordinal);
    }
    out += "|";
    out += std::to_string(row.request.iatRva);
    out += "|";
    out += std::to_string(row.request.iatVa);
    out += "|";
    out += row.result.providerModule;
    out += "|";
    out += std::to_string(row.result.rva);
    out += "|";
    out += std::to_string(row.result.va);
    for (std::size_t h = 0; h < row.hops.size(); ++h) {
      const peparse::BindingHop &hop = row.hops[h];
      out += "|";
      out += std::to_string(static_cast<int>(hop.kind));
      out += ",";
      out += hop.sourceModule;
      out += ",";
      out += hop.targetModule;
      out += ",";
      out += hop.forwardString;
      out += ",";
      out += hop.contractName;
      out += ",";
      out += hop.importerModule;
    }
    out += "|";
    out += row.diagnostic;
    out += "\n";
  }
  return out;
}

struct DisplayEntry {
  std::string module;
  std::string symbol;
  peparse::VA va;
};

int CollectDisplay(void *cbd,
                   const peparse::VA &va,
                   const std::string &module,
                   const std::string &symbol) {
  std::vector<DisplayEntry> *out =
      static_cast<std::vector<DisplayEntry> *>(cbd);
  DisplayEntry entry;
  entry.module = module;
  entry.symbol = symbol;
  entry.va = va;
  out->push_back(entry);
  return 0;
}

bool Contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// normal cases
// ---------------------------------------------------------------------------

void CaseStructuredIdentity() {
  PeImage root(false);
  root.setImports({MakeImport(
      "B", {ByName("ORDINAL_B_7"), ByOrdinal(7), ByName("Alias")})});

  PeImage provider(false);
  provider.setExports(MakeExports(
      "B.dll", 5,
      {Slot(0x1E00, {"Entry"}),
       Slot(0x1E10, {"ORDINAL_B_7", "Twin"}),
       Slot(0x1E20, {}),
       Slot(0, {"Alias"})}));

  ModuleSet set;
  set.add(root);
  set.add(provider);
  UNIT_CHECK(set.pe(0) != nullptr);
  UNIT_CHECK(set.pe(1) != nullptr);
  if (set.pe(0) == nullptr || set.pe(1) == nullptr) {
    return;
  }

  peparse::StructuredImportTable imports;
  std::string error;
  UNIT_CHECK(peparse::BuildStructuredImportTable(set.pe(0), imports, &error));
  UNIT_CHECK(imports.entries.size() == 3);
  UNIT_CHECK(imports.entries[0].moduleName == "B");
  UNIT_CHECK(imports.entries[0].kind == peparse::ImportNameKind::ByName);
  UNIT_CHECK(imports.entries[0].symbolName == "ORDINAL_B_7");
  UNIT_CHECK(imports.entries[0].iatRva == 0x1400);
  UNIT_CHECK(imports.entries[1].kind == peparse::ImportNameKind::ByOrdinal);
  UNIT_CHECK(imports.entries[1].ordinal == 7);
  UNIT_CHECK(imports.entries[1].iatRva == 0x1404);
  UNIT_CHECK(imports.entries[2].iatRva == 0x1408);

  peparse::StructuredExportTable exports;
  UNIT_CHECK(peparse::BuildStructuredExportTable(set.pe(1), exports, &error));
  UNIT_CHECK(exports.ordinalBase == 5);
  UNIT_CHECK(exports.slots.size() == 4);
  UNIT_CHECK(exports.slots[1].names.size() == 2);
  UNIT_CHECK(exports.slots[1].ordinal == 6);
  UNIT_CHECK(exports.slots[2].ordinal == 7);
  UNIT_CHECK(exports.slots[2].names.empty());
  UNIT_CHECK(exports.slots[3].rva == 0);

  // The literal import name and the ordinal-7 import produce the same
  // IterImpVAString display string; the structured table keeps them apart.
  std::vector<DisplayEntry> display;
  peparse::IterImpVAString(set.pe(0), CollectDisplay, &display);
  UNIT_CHECK(display.size() == 3);
  UNIT_CHECK(display[0].symbol == "ORDINAL_B_7");
  UNIT_CHECK(display[1].symbol == "ORDINAL_B_7");
  UNIT_CHECK(display[2].symbol == "Alias");

  std::vector<peparse::BindingModuleInput> modules;
  modules.push_back(Input("Root.dll", set.pe(0)));
  modules.push_back(Input("B.dll", set.pe(1)));

  peparse::ImportBindingPlan plan;
  UNIT_CHECK(peparse::BuildImportBindingPlan("Root.dll", modules, nullptr, 0,
                                             plan, &error));
  UNIT_CHECK(plan.Rows().size() == 3);

  const peparse::BindingRow &named = plan.Rows()[0];
  UNIT_CHECK(named.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(named.request.kind == peparse::ImportNameKind::ByName);
  UNIT_CHECK(named.request.iatRva == 0x1400);
  UNIT_CHECK(named.result.providerModule == "b");
  UNIT_CHECK(named.result.exportSlot == 1);
  UNIT_CHECK(named.result.exportOrdinal == 6);
  UNIT_CHECK(named.result.rva == 0x1E10);
  UNIT_CHECK(named.result.va == 0x401E10);
  UNIT_CHECK(named.hops.empty());

  const peparse::BindingRow &ordinal = plan.Rows()[1];
  UNIT_CHECK(ordinal.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(ordinal.request.kind == peparse::ImportNameKind::ByOrdinal);
  UNIT_CHECK(ordinal.request.ordinal == 7);
  UNIT_CHECK(ordinal.request.iatRva == 0x1404);
  UNIT_CHECK(ordinal.result.exportSlot == 2);
  UNIT_CHECK(ordinal.result.exportOrdinal == 7);
  UNIT_CHECK(ordinal.result.rva == 0x1E20);
  UNIT_CHECK(ordinal.result.va == 0x401E20);

  const peparse::BindingRow &zero = plan.Rows()[2];
  UNIT_CHECK(zero.status == peparse::BindingStatus::ZeroSlot);
  UNIT_CHECK(Contains(zero.diagnostic, "zero RVA"));
}

void CaseTwoHopForward() {
  PeImage root(false);
  root.setImports({MakeImport("Shim.dll", {ByName("Fn")})});

  PeImage shim(false);
  shim.setExports(MakeExports("Shim.dll", 3,
                              {ForwardSlot("Mid.Target", {"Fn"})}));

  PeImage mid(false);
  mid.setExports(MakeExports("Mid.dll", 10,
                             {ForwardSlot("Final.#12", {"Target"})}));

  PeImage finalImage(false);
  finalImage.setExports(MakeExports(
      "Final.dll", 10,
      {Slot(0, {}), Slot(0x1E30, {"Other"}), Slot(0x1E40, {})}));

  ModuleSet set;
  set.add(root);
  set.add(shim);
  set.add(mid);
  set.add(finalImage);
  for (std::size_t i = 0; i < 4; ++i) {
    UNIT_CHECK(set.pe(i) != nullptr);
  }
  if (set.pe(0) == nullptr || set.pe(1) == nullptr ||
      set.pe(2) == nullptr || set.pe(3) == nullptr) {
    return;
  }

  std::vector<peparse::BindingModuleInput> modules;
  modules.push_back(Input("Root.dll", set.pe(0)));
  modules.push_back(Input("Shim.dll", set.pe(1)));
  modules.push_back(Input("Mid.dll", set.pe(2)));
  modules.push_back(Input("Final.dll", set.pe(3)));

  peparse::ImportBindingPlan plan;
  std::string error;
  UNIT_CHECK(peparse::BuildImportBindingPlan("Root.dll", modules, nullptr, 0,
                                             plan, &error));
  UNIT_CHECK(plan.Rows().size() == 1);
  const peparse::BindingRow &row = plan.Rows()[0];
  UNIT_CHECK(row.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(row.request.iatRva == 0x1400);
  UNIT_CHECK(row.result.providerModule == "final");
  UNIT_CHECK(row.result.exportSlot == 2);
  UNIT_CHECK(row.result.exportOrdinal == 12);
  UNIT_CHECK(row.result.rva == 0x1E40);
  UNIT_CHECK(row.result.va == 0x401E40);

  UNIT_CHECK(row.hops.size() == 2);
  UNIT_CHECK(row.hops[0].kind == peparse::BindingHopKind::ExportForward);
  UNIT_CHECK(row.hops[0].sourceModule == "shim");
  UNIT_CHECK(row.hops[0].targetModule == "mid");
  UNIT_CHECK(row.hops[0].exportSlot == 0);
  UNIT_CHECK(row.hops[0].exportOrdinal == 3);
  UNIT_CHECK(row.hops[0].forwardString == "Mid.Target");
  UNIT_CHECK(row.hops[1].sourceModule == "mid");
  UNIT_CHECK(row.hops[1].targetModule == "final");
  UNIT_CHECK(row.hops[1].forwardString == "Final.#12");
}

void CaseReportSurvivesInputs() {
  PeImage root(false);
  root.setImports(
      {MakeImport("B.dll", {ByName("Entry")}),
       MakeImport("api-ms-win-core-z-l1-1-0.dll", {ByName("Fn")})});

  PeImage provider(false);
  provider.setExports(MakeExports("B.dll", 1, {Slot(0x1E00, {"Entry"})}));

  PeImage host(false);
  host.setExports(MakeExports("Host.dll", 1, {Slot(0x1E10, {"Fn"})}));

  ModuleSet set;
  set.add(root);
  set.add(provider);
  set.add(host);
  for (std::size_t i = 0; i < 3; ++i) {
    UNIT_CHECK(set.pe(i) != nullptr);
  }
  if (set.pe(0) == nullptr || set.pe(1) == nullptr ||
      set.pe(2) == nullptr) {
    return;
  }

  const std::vector<std::vector<std::uint8_t> > snapshot = set.images;

  std::vector<peparse::BindingModuleInput> modules;
  modules.push_back(Input("Root.dll", set.pe(0)));
  modules.push_back(Input("B.dll", set.pe(1)));
  modules.push_back(Input("Host.dll", set.pe(2)));

  peparse::ImportBindingPlan plan;
  std::string error;
  {
    std::vector<std::uint8_t> schema = BuildNamespace(
        7, {Entry("api-ms-win-core-z-l1-1-0",
                  {Value("", "Host.dll")})});
    UNIT_CHECK(peparse::BuildImportBindingPlan(
        "Root.dll", modules, schema.data(),
        static_cast<std::uint32_t>(schema.size()), plan, &error));
  }

  set.release();
  UNIT_CHECK(set.images == snapshot);
  UNIT_CHECK(plan.Rows().size() == 2);

  const peparse::BindingRow &plain = plan.Rows()[0];
  UNIT_CHECK(plain.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(plain.request.moduleName == "B.dll");
  UNIT_CHECK(plain.request.symbolName == "Entry");
  UNIT_CHECK(plain.result.providerModule == "b");
  UNIT_CHECK(plain.result.rva == 0x1E00);

  const peparse::BindingRow &contract = plan.Rows()[1];
  UNIT_CHECK(contract.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(contract.request.normalizedModule ==
             "api-ms-win-core-z-l1-1-0");
  UNIT_CHECK(contract.hops.size() == 1);
  UNIT_CHECK(contract.hops[0].kind == peparse::BindingHopKind::ApiSetContract);
  UNIT_CHECK(contract.hops[0].contractName == "api-ms-win-core-z-l1-1-0");
  UNIT_CHECK(contract.hops[0].schemaHost == "Host.dll");
  UNIT_CHECK(contract.hops[0].schemaEntryOffset != 0);
  UNIT_CHECK(contract.result.providerModule == "host");
  UNIT_CHECK(contract.result.rva == 0x1E10);
}

// ---------------------------------------------------------------------------
// boundary cases
// ---------------------------------------------------------------------------

void CaseTruncatedForwarder() {
  PeImage root(false);
  root.setImports({MakeImport(
      "Trunc.dll",
      {ByName("Fn"), ByName("Safe"), ByName("Missing"), ByOrdinal(99)})});

  PeImage trunc(false);
  trunc.setExports(MakeExports(
      "Trunc.dll", 1,
      {TruncatedForwardSlot("K.Sym", {"Fn"}), Slot(0x1E00, {"Safe"})}));

  ModuleSet set;
  set.add(root);
  set.add(trunc);
  UNIT_CHECK(set.pe(0) != nullptr);
  UNIT_CHECK(set.pe(1) != nullptr);
  if (set.pe(0) == nullptr || set.pe(1) == nullptr) {
    return;
  }

  std::vector<peparse::BindingModuleInput> modules;
  modules.push_back(Input("Root.dll", set.pe(0)));
  modules.push_back(Input("Trunc.dll", set.pe(1)));

  peparse::ImportBindingPlan plan;
  std::string error;
  UNIT_CHECK(peparse::BuildImportBindingPlan("Root.dll", modules, nullptr, 0,
                                             plan, &error));
  UNIT_CHECK(plan.Rows().size() == 4);
  UNIT_CHECK(plan.Rows()[0].request.iatRva == 0x1400);
  UNIT_CHECK(plan.Rows()[0].status == peparse::BindingStatus::BadForward);
  UNIT_CHECK(Contains(plan.Rows()[0].diagnostic, "truncated"));
  UNIT_CHECK(plan.Rows()[0].hops.empty());

  UNIT_CHECK(plan.Rows()[1].status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(plan.Rows()[1].result.rva == 0x1E00);

  UNIT_CHECK(plan.Rows()[2].status ==
             peparse::BindingStatus::MissingExportName);
  UNIT_CHECK(Contains(plan.Rows()[2].diagnostic, "Missing"));

  UNIT_CHECK(plan.Rows()[3].status ==
             peparse::BindingStatus::MissingExportOrdinal);
  UNIT_CHECK(Contains(plan.Rows()[3].diagnostic, "99"));
}

void CaseNamespaceOffsetOverflow() {
  PeImage root(true);
  root.setImports(
      {MakeImport("api-ms-win-core-x-l1-1-0.dll", {ByName("Func")}),
       MakeImport("B.dll", {ByName("Entry")}),
       MakeImport("Nope.dll", {ByName("Ghost")})});

  PeImage provider(true);
  provider.setExports(MakeExports("B.dll", 1, {Slot(0x1E00, {"Entry"})}));

  ModuleSet set;
  set.add(root);
  set.add(provider);
  UNIT_CHECK(set.pe(0) != nullptr);
  UNIT_CHECK(set.pe(1) != nullptr);
  if (set.pe(0) == nullptr || set.pe(1) == nullptr) {
    return;
  }

  std::vector<peparse::BindingModuleInput> modules;
  modules.push_back(Input("Root.dll", set.pe(0)));
  modules.push_back(Input("B.dll", set.pe(1)));

  std::vector<std::uint8_t> schema = BuildOverflowNamespace();

  peparse::ImportBindingPlan plan;
  std::string error;
  UNIT_CHECK(peparse::BuildImportBindingPlan(
      "Root.dll", modules, schema.data(),
      static_cast<std::uint32_t>(schema.size()), plan, &error));
  UNIT_CHECK(plan.Rows().size() == 3);
  UNIT_CHECK(plan.Rows()[0].status == peparse::BindingStatus::ApiSetMalformed);
  UNIT_CHECK(!plan.Diagnostics().empty());

  UNIT_CHECK(plan.Rows()[1].status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(plan.Rows()[1].request.iatRva == 0x1440);
  UNIT_CHECK(plan.Rows()[1].result.rva == 0x1E00);
  UNIT_CHECK(plan.Rows()[1].result.va == kImageBase64 + 0x1E00);

  UNIT_CHECK(plan.Rows()[2].status == peparse::BindingStatus::MissingModule);
  UNIT_CHECK(Contains(plan.Rows()[2].diagnostic, "nope"));
}

void CaseDuplicateModuleRejected() {
  PeImage root(false);
  root.setImports({MakeImport("B.dll", {ByName("Entry")})});

  PeImage provider(false);
  provider.setExports(MakeExports("B.dll", 1, {Slot(0x1E00, {"Entry"})}));

  ModuleSet set;
  set.add(root);
  set.add(provider);
  UNIT_CHECK(set.pe(0) != nullptr);
  UNIT_CHECK(set.pe(1) != nullptr);
  if (set.pe(0) == nullptr || set.pe(1) == nullptr) {
    return;
  }

  std::vector<peparse::BindingModuleInput> modules;
  modules.push_back(Input("Root.dll", set.pe(0)));
  modules.push_back(Input("B.dll", set.pe(1)));
  modules.push_back(Input("b.DLL", set.pe(0)));

  peparse::ImportBindingPlan plan;
  std::string error;
  UNIT_CHECK(!peparse::BuildImportBindingPlan("Root.dll", modules, nullptr, 0,
                                              plan, &error));
  UNIT_CHECK(!error.empty());
  UNIT_CHECK(plan.Rows().empty());
}

void CaseContractHashCollision() {
  PeImage root(true);
  root.setImports({MakeImport("api-xy-l1.dll", {ByName("Fn")}),
                   MakeImport("api-yx-l1.dll", {ByName("Fn")}),
                   MakeImport("api-dup-l1.dll", {ByName("Fn")})});

  PeImage hxy(true);
  hxy.setExports(MakeExports("Hxy.dll", 1, {Slot(0x1E00, {"Fn"})}));

  PeImage hyx(true);
  hyx.setExports(MakeExports("Hyx.dll", 1, {Slot(0x1E10, {"Fn"})}));

  PeImage hd1(true);
  hd1.setExports(MakeExports("Hd1.dll", 1, {Slot(0x1E20, {"Fn"})}));

  PeImage hd2(true);
  hd2.setExports(MakeExports("Hd2.dll", 1, {Slot(0x1E30, {"Fn"})}));

  ModuleSet set;
  set.add(root);
  set.add(hxy);
  set.add(hyx);
  set.add(hd1);
  set.add(hd2);
  for (std::size_t i = 0; i < 5; ++i) {
    UNIT_CHECK(set.pe(i) != nullptr);
  }
  if (set.pe(0) == nullptr || set.pe(1) == nullptr ||
      set.pe(2) == nullptr || set.pe(3) == nullptr ||
      set.pe(4) == nullptr) {
    return;
  }

  std::vector<peparse::BindingModuleInput> modules;
  modules.push_back(Input("Root.dll", set.pe(0)));
  modules.push_back(Input("Hxy.dll", set.pe(1)));
  modules.push_back(Input("Hyx.dll", set.pe(2)));
  modules.push_back(Input("Hd1.dll", set.pe(3)));
  modules.push_back(Input("Hd2.dll", set.pe(4)));

  // The two prefixes hash identically under factor 1 but must not be treated
  // as the same contract.
  UNIT_CHECK(HashPrefix("api-xy", 1) == HashPrefix("api-yx", 1));

  std::vector<std::uint8_t> schema = BuildNamespace(
      1, {Entry("api-xy-l1", {Value("", "Hxy.dll")}),
          Entry("api-yx-l1", {Value("", "Hyx.dll")}),
          Entry("api-dup-l1", {Value("", "Hd1.dll")}),
          Entry("api-dup-l2", {Value("", "Hd2.dll")})});

  peparse::ImportBindingPlan plan;
  std::string error;
  UNIT_CHECK(peparse::BuildImportBindingPlan(
      "Root.dll", modules, schema.data(),
      static_cast<std::uint32_t>(schema.size()), plan, &error));
  UNIT_CHECK(plan.Rows().size() == 3);

  UNIT_CHECK(plan.Rows()[0].status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(plan.Rows()[0].result.providerModule == "hxy");
  UNIT_CHECK(plan.Rows()[0].result.rva == 0x1E00);

  UNIT_CHECK(plan.Rows()[1].status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(plan.Rows()[1].result.providerModule == "hyx");
  UNIT_CHECK(plan.Rows()[1].result.rva == 0x1E10);

  UNIT_CHECK(plan.Rows()[2].status == peparse::BindingStatus::ApiSetAmbiguous);
}

// ---------------------------------------------------------------------------
// integration cases
// ---------------------------------------------------------------------------

void CaseImporterSpecificHosts() {
  PeImage root(true);
  root.setImports({MakeImport("Sh1.dll", {ByName("Func1")}),
                   MakeImport("Sh2.dll", {ByName("Func1")}),
                   MakeImport("api-ms-win-core-y-l1-1-0.dll",
                              {ByName("Fn")})});

  PeImage sh1(true);
  sh1.setExports(MakeExports(
      "Sh1.dll", 1,
      {ForwardSlot("api-ms-win-core-x-l1-1-0.Fn", {"Func1"})}));

  PeImage sh2(true);
  sh2.setExports(MakeExports(
      "Sh2.dll", 1,
      {ForwardSlot("api-ms-win-core-x-l1-1-0.Fn", {"Func1"})}));

  PeImage hostA(true);
  hostA.setExports(MakeExports("HostA.dll", 1, {Slot(0x1E00, {"Fn"})}));

  PeImage hostB(true);
  hostB.setExports(MakeExports("HostB.dll", 1, {Slot(0x1E10, {"Fn"})}));

  PeImage hostDefault(true);
  hostDefault.setExports(
      MakeExports("Default.dll", 1, {Slot(0x1E20, {"Fn"})}));

  ModuleSet set;
  set.add(root);
  set.add(sh1);
  set.add(sh2);
  set.add(hostA);
  set.add(hostB);
  set.add(hostDefault);
  for (std::size_t i = 0; i < 6; ++i) {
    UNIT_CHECK(set.pe(i) != nullptr);
  }
  if (set.pe(0) == nullptr || set.pe(1) == nullptr ||
      set.pe(2) == nullptr || set.pe(3) == nullptr ||
      set.pe(4) == nullptr || set.pe(5) == nullptr) {
    return;
  }

  std::vector<peparse::BindingModuleInput> modules;
  modules.push_back(Input("Root.dll", set.pe(0)));
  modules.push_back(Input("Sh1.dll", set.pe(1)));
  modules.push_back(Input("Sh2.dll", set.pe(2)));
  modules.push_back(Input("HostA.dll", set.pe(3)));
  modules.push_back(Input("HostB.dll", set.pe(4)));
  modules.push_back(Input("Default.dll", set.pe(5)));

  std::vector<std::uint8_t> schema = BuildNamespace(
      3, {Entry("api-ms-win-core-x-l1-1-0",
                {Value("", "Default.dll"), Value("Sh1.dll", "HostA.dll"),
                 Value("SH2.DLL", "HostB.dll")}),
          Entry("api-ms-win-core-y-l1-1-0", {Value("", "Default.dll")})});

  peparse::ImportBindingPlan plan;
  std::string error;
  UNIT_CHECK(peparse::BuildImportBindingPlan(
      "Root.dll", modules, schema.data(),
      static_cast<std::uint32_t>(schema.size()), plan, &error));
  UNIT_CHECK(plan.Rows().size() == 3);

  const peparse::BindingRow &viaSh1 = plan.Rows()[0];
  UNIT_CHECK(viaSh1.request.iatRva == 0x1400);
  UNIT_CHECK(viaSh1.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(viaSh1.hops.size() == 2);
  UNIT_CHECK(viaSh1.hops[0].kind == peparse::BindingHopKind::ExportForward);
  UNIT_CHECK(viaSh1.hops[0].sourceModule == "sh1");
  UNIT_CHECK(viaSh1.hops[1].kind == peparse::BindingHopKind::ApiSetContract);
  UNIT_CHECK(viaSh1.hops[1].importerModule == "sh1");
  UNIT_CHECK(!viaSh1.hops[1].usedDefaultAlias);
  UNIT_CHECK(viaSh1.hops[1].schemaEntryOffset != 0);
  UNIT_CHECK(viaSh1.hops[1].schemaValueOffset != 0);
  UNIT_CHECK(viaSh1.hops[1].schemaHostOffset != 0);
  UNIT_CHECK(viaSh1.result.providerModule == "hosta");
  UNIT_CHECK(viaSh1.result.rva == 0x1E00);
  UNIT_CHECK(viaSh1.result.va == kImageBase64 + 0x1E00);

  const peparse::BindingRow &viaSh2 = plan.Rows()[1];
  UNIT_CHECK(viaSh2.request.iatRva == 0x1440);
  UNIT_CHECK(viaSh2.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(viaSh2.hops.size() == 2);
  UNIT_CHECK(viaSh2.hops[1].importerModule == "sh2");
  UNIT_CHECK(viaSh2.result.providerModule == "hostb");
  UNIT_CHECK(viaSh2.result.rva == 0x1E10);

  const peparse::BindingRow &direct = plan.Rows()[2];
  UNIT_CHECK(direct.request.iatRva == 0x1480);
  UNIT_CHECK(direct.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(direct.hops.size() == 1);
  UNIT_CHECK(direct.hops[0].importerModule == "root");
  UNIT_CHECK(direct.hops[0].usedDefaultAlias);
  UNIT_CHECK(direct.result.providerModule == "default");
  UNIT_CHECK(direct.result.rva == 0x1E20);
}

void CaseCycleAndHealthyImport() {
  PeImage root(false);
  root.setImports({MakeImport("A.dll", {ByName("X"), ByName("P")}),
                   MakeImport("C.dll", {ByName("Good")})});

  PeImage a(false);
  a.setExports(MakeExports("A.dll", 1,
                           {ForwardSlot("B.Y", {"X"}),
                            Slot(0x1E20, {"W"}),
                            ForwardSlot("B.Q", {"P"})}));

  PeImage b(false);
  b.setExports(MakeExports("B.dll", 1,
                           {ForwardSlot("A.W", {"Y"}),
                            ForwardSlot("A.P", {"Q"})}));

  PeImage c(false);
  c.setExports(MakeExports("C.dll", 1, {Slot(0x1E00, {"Good"})}));

  ModuleSet set;
  set.add(root);
  set.add(a);
  set.add(b);
  set.add(c);
  for (std::size_t i = 0; i < 4; ++i) {
    UNIT_CHECK(set.pe(i) != nullptr);
  }
  if (set.pe(0) == nullptr || set.pe(1) == nullptr ||
      set.pe(2) == nullptr || set.pe(3) == nullptr) {
    return;
  }

  std::vector<peparse::BindingModuleInput> modules;
  modules.push_back(Input("Root.dll", set.pe(0)));
  modules.push_back(Input("A.dll", set.pe(1)));
  modules.push_back(Input("B.dll", set.pe(2)));
  modules.push_back(Input("C.dll", set.pe(3)));

  peparse::ImportBindingPlan plan;
  std::string error;
  UNIT_CHECK(peparse::BuildImportBindingPlan("Root.dll", modules, nullptr, 0,
                                             plan, &error));
  UNIT_CHECK(plan.Rows().size() == 3);

  // A.W is reached by revisiting module A with a different symbol: legal.
  const peparse::BindingRow &revisit = plan.Rows()[0];
  UNIT_CHECK(revisit.request.iatRva == 0x1400);
  UNIT_CHECK(revisit.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(revisit.hops.size() == 2);
  UNIT_CHECK(revisit.result.providerModule == "a");
  UNIT_CHECK(revisit.result.exportSlot == 1);
  UNIT_CHECK(revisit.result.rva == 0x1E20);

  const peparse::BindingRow &cycle = plan.Rows()[1];
  UNIT_CHECK(cycle.request.iatRva == 0x1404);
  UNIT_CHECK(cycle.status == peparse::BindingStatus::Cycle);
  UNIT_CHECK(cycle.stateChain.size() == 3);
  UNIT_CHECK(cycle.stateChain[0] == "a!P");
  UNIT_CHECK(cycle.stateChain[1] == "b!Q");
  UNIT_CHECK(cycle.stateChain[2] == "a!P");
  UNIT_CHECK(Contains(cycle.diagnostic, "a!P -> b!Q -> a!P"));

  const peparse::BindingRow &healthy = plan.Rows()[2];
  UNIT_CHECK(healthy.request.iatRva == 0x1440);
  UNIT_CHECK(healthy.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(healthy.result.providerModule == "c");
  UNIT_CHECK(healthy.result.rva == 0x1E00);
  UNIT_CHECK(healthy.result.va == 0x401E00);
}

void CasePe64OrdinalsAndOrderIndependence() {
  PeImage root(true);
  root.setImports(
      {MakeImport("B.dll", {ByName("Entry"), ByOrdinal(9)})});

  PeImage provider(true);
  provider.setExports(MakeExports(
      "B.dll", 1,
      {Slot(0x1E00, {"Entry"}), Slot(0, {}), Slot(0, {}), Slot(0, {}),
       Slot(0, {}), Slot(0, {}), Slot(0, {}), Slot(0, {}),
       Slot(0x1E30, {})}));

  PeImage dummy(true);

  ModuleSet set;
  set.add(root);
  set.add(provider);
  set.add(dummy);
  for (std::size_t i = 0; i < 3; ++i) {
    UNIT_CHECK(set.pe(i) != nullptr);
  }
  if (set.pe(0) == nullptr || set.pe(1) == nullptr ||
      set.pe(2) == nullptr) {
    return;
  }

  peparse::StructuredImportTable imports;
  std::string error;
  UNIT_CHECK(peparse::BuildStructuredImportTable(set.pe(0), imports, &error));
  UNIT_CHECK(imports.entries.size() == 2);
  UNIT_CHECK(imports.entries[0].kind == peparse::ImportNameKind::ByName);
  UNIT_CHECK(imports.entries[0].slotWidth == 8);
  UNIT_CHECK(imports.entries[0].iatRva == 0x1400);
  UNIT_CHECK(imports.entries[1].kind == peparse::ImportNameKind::ByOrdinal);
  UNIT_CHECK(imports.entries[1].ordinal == 9);
  UNIT_CHECK(imports.entries[1].iatRva == 0x1408);

  std::vector<peparse::BindingModuleInput> forwardOrder;
  forwardOrder.push_back(Input("Root.dll", set.pe(0)));
  forwardOrder.push_back(Input("B.dll", set.pe(1)));
  forwardOrder.push_back(Input("Dummy.dll", set.pe(2)));

  std::vector<peparse::BindingModuleInput> reverseOrder;
  reverseOrder.push_back(Input("Dummy.dll", set.pe(2)));
  reverseOrder.push_back(Input("B.dll", set.pe(1)));
  reverseOrder.push_back(Input("Root.dll", set.pe(0)));

  peparse::ImportBindingPlan first;
  peparse::ImportBindingPlan second;
  UNIT_CHECK(peparse::BuildImportBindingPlan("Root.dll", forwardOrder, nullptr,
                                             0, first, &error));
  UNIT_CHECK(peparse::BuildImportBindingPlan("Root.dll", reverseOrder, nullptr,
                                             0, second, &error));
  UNIT_CHECK(!first.Rows().empty());
  UNIT_CHECK(RenderPlan(first) == RenderPlan(second));

  const peparse::BindingRow &named = first.Rows()[0];
  UNIT_CHECK(named.request.iatRva == 0x1400);
  UNIT_CHECK(named.request.iatVa == kImageBase64 + 0x1400);
  UNIT_CHECK(named.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(named.result.providerModule == "b");
  UNIT_CHECK(named.result.rva == 0x1E00);
  UNIT_CHECK(named.result.va == kImageBase64 + 0x1E00);

  const peparse::BindingRow &ordinal = first.Rows()[1];
  UNIT_CHECK(ordinal.request.iatRva == 0x1408);
  UNIT_CHECK(ordinal.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(ordinal.result.exportSlot == 8);
  UNIT_CHECK(ordinal.result.exportOrdinal == 9);
  UNIT_CHECK(ordinal.result.rva == 0x1E30);
  UNIT_CHECK(ordinal.result.va == kImageBase64 + 0x1E30);
}

void CaseSchemaStatesAndNormalization() {
  PeImage root(true);
  root.setImports(
      {MakeImport("B.DLL", {ByName("Entry")}),
       MakeImport("api-ms-win-core-d-l1-1-0.dll", {ByName("Fn")})});

  PeImage provider(true);
  provider.setExports(MakeExports("B.dll", 1, {Slot(0x1E00, {"Entry"})}));

  ModuleSet set;
  set.add(root);
  set.add(provider);
  UNIT_CHECK(set.pe(0) != nullptr);
  UNIT_CHECK(set.pe(1) != nullptr);
  if (set.pe(0) == nullptr || set.pe(1) == nullptr) {
    return;
  }

  // File names deliberately carry a path, mixed case and a suffix; the plan
  // must reduce them to the same normalized basenames.
  std::vector<peparse::BindingModuleInput> modules;
  modules.push_back(Input("some/dir/Root.dll", set.pe(0)));
  modules.push_back(Input("PROVIDER\\B.dll", set.pe(1)));

  // 1) No schema: ordinary DLL imports still resolve, contract imports report
  //    a missing schema rather than guessing a host.
  {
    peparse::ImportBindingPlan plan;
    std::string error;
    UNIT_CHECK(peparse::BuildImportBindingPlan(
        "any/path/root.DLL", modules, nullptr, 0, plan, &error));
    UNIT_CHECK(plan.Rows().size() == 2);
    UNIT_CHECK(plan.Rows()[0].status == peparse::BindingStatus::Resolved);
    UNIT_CHECK(plan.Rows()[0].request.normalizedModule == "b");
    UNIT_CHECK(plan.Rows()[0].result.providerModule == "b");
    UNIT_CHECK(plan.Rows()[1].status ==
               peparse::BindingStatus::ApiSetMissingSchema);
  }

  // 2) Unknown namespace version is explicitly unsupported.
  {
    std::vector<std::uint8_t> bad = BuildNamespace(
        3, {Entry("api-ms-win-core-d-l1-1-0",
                  {Value("", "Prov.dll")})},
        7);
    peparse::ImportBindingPlan plan;
    std::string error;
    UNIT_CHECK(peparse::BuildImportBindingPlan(
        "Root.dll", modules, bad.data(),
        static_cast<std::uint32_t>(bad.size()), plan, &error));
    UNIT_CHECK(plan.Rows()[1].status ==
               peparse::BindingStatus::ApiSetUnsupportedVersion);
  }

  // 3) The root's alias selects an empty host: the contract is disabled and
  //    must not fall back to the non-empty default host.
  {
    std::vector<std::uint8_t> schema = BuildNamespace(
        3, {Entry("api-ms-win-core-d-l1-1-0",
                  {Value("", "Prov.dll"), Value("Root.dll", "")})});
    peparse::ImportBindingPlan plan;
    std::string error;
    UNIT_CHECK(peparse::BuildImportBindingPlan(
        "Root.dll", modules, schema.data(),
        static_cast<std::uint32_t>(schema.size()), plan, &error));
    UNIT_CHECK(plan.Rows().size() == 2);
    UNIT_CHECK(plan.Rows()[0].status == peparse::BindingStatus::Resolved);
    const peparse::BindingRow &disabled = plan.Rows()[1];
    UNIT_CHECK(disabled.status == peparse::BindingStatus::ApiSetDisabled);
    UNIT_CHECK(disabled.hops.empty());
    UNIT_CHECK(Contains(disabled.diagnostic, "disables importer root"));
  }
}

void CaseContractHostForwardsAgain() {
  // The API-set host is itself a forwarder: contract -> hostfwd -> real.
  PeImage root(true);
  root.setImports(
      {MakeImport("api-ms-win-core-f-l1-1-0.dll", {ByName("Fn")})});

  PeImage hostFwd(true);
  hostFwd.setExports(MakeExports(
      "HostFwd.dll", 1, {ForwardSlot("Real.Final", {"Fn"})}));

  PeImage real(true);
  real.setExports(MakeExports("Real.dll", 1, {Slot(0x1E50, {"Final"})}));

  ModuleSet set;
  set.add(root);
  set.add(hostFwd);
  set.add(real);
  for (std::size_t i = 0; i < 3; ++i) {
    UNIT_CHECK(set.pe(i) != nullptr);
  }
  if (set.pe(0) == nullptr || set.pe(1) == nullptr ||
      set.pe(2) == nullptr) {
    return;
  }

  std::vector<peparse::BindingModuleInput> modules;
  modules.push_back(Input("Root.dll", set.pe(0)));
  modules.push_back(Input("HostFwd.dll", set.pe(1)));
  modules.push_back(Input("Real.dll", set.pe(2)));

  std::vector<std::uint8_t> schema = BuildNamespace(
      5, {Entry("api-ms-win-core-f-l1-1-0",
                {Value("", "HostFwd.dll")})});

  peparse::ImportBindingPlan plan;
  std::string error;
  UNIT_CHECK(peparse::BuildImportBindingPlan(
      "Root.dll", modules, schema.data(),
      static_cast<std::uint32_t>(schema.size()), plan, &error));
  UNIT_CHECK(plan.Rows().size() == 1);

  const peparse::BindingRow &row = plan.Rows()[0];
  UNIT_CHECK(row.status == peparse::BindingStatus::Resolved);
  UNIT_CHECK(row.hops.size() == 2);
  UNIT_CHECK(row.hops[0].kind == peparse::BindingHopKind::ApiSetContract);
  UNIT_CHECK(row.hops[0].targetModule == "hostfwd");
  UNIT_CHECK(row.hops[1].kind == peparse::BindingHopKind::ExportForward);
  UNIT_CHECK(row.hops[1].sourceModule == "hostfwd");
  UNIT_CHECK(row.hops[1].targetModule == "real");
  UNIT_CHECK(row.hops[1].forwardString == "Real.Final");
  UNIT_CHECK(row.result.providerModule == "real");
  UNIT_CHECK(row.result.exportSlot == 0);
  UNIT_CHECK(row.result.rva == 0x1E50);
  UNIT_CHECK(row.result.va == kImageBase64 + 0x1E50);
}

} // namespace

extern "C" {

extern const unit_case unit_cases[] = {
    {"structured_identity", "normal", CaseStructuredIdentity},
    {"two_hop_forward", "normal", CaseTwoHopForward},
    {"report_outlives_inputs", "normal", CaseReportSurvivesInputs},
    {"truncated_forwarder", "boundary", CaseTruncatedForwarder},
    {"namespace_offset_overflow", "boundary", CaseNamespaceOffsetOverflow},
    {"duplicate_module_rejected", "boundary", CaseDuplicateModuleRejected},
    {"contract_hash_collision", "boundary", CaseContractHashCollision},
    {"schema_states_and_normalization", "boundary",
     CaseSchemaStatesAndNormalization},
    {"importer_specific_hosts", "integration", CaseImporterSpecificHosts},
    {"contract_host_forwards_again", "integration",
     CaseContractHostForwardsAgain},
    {"cycle_and_healthy_import", "integration", CaseCycleAndHealthyImport},
    {"pe64_ordinals_order_independence", "integration",
     CasePe64OrdinalsAndOrderIndependence},
};

extern const size_t unit_case_count =
    sizeof(unit_cases) / sizeof(unit_cases[0]);

} // extern "C"
