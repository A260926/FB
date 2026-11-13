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

#include <algorithm>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <pe-parse/import_binding.h>

namespace peparse {
namespace {

constexpr std::uint32_t DIR_ENTRY_KIND_IMPORT = 1;
constexpr std::uint32_t DIR_ENTRY_KIND_EXPORT = 0;
constexpr std::uint32_t MAX_IMPORT_DESCRIPTORS = 4096;
constexpr std::uint32_t MAX_THUNKS_PER_DESCRIPTOR = 65536;
constexpr std::uint32_t MAX_EXPORT_SLOTS = 1u << 20;
constexpr std::uint32_t MAX_EXPORT_NAMES = 1u << 20;
constexpr std::uint64_t ADDRESS_32_MAX = 0xFFFFFFFFull;
constexpr std::uint64_t ADDRESS_64_MAX = 0xFFFFFFFFFFFFFFFFull;

// imageBase + rva without wrapping.  Returns false and saturates on overflow.
bool addImageBase(std::uint64_t imageBase,
                  std::uint32_t rva,
                  std::uint64_t maxAddress,
                  std::uint64_t &out) {
  if (static_cast<std::uint64_t>(rva) > maxAddress - imageBase) {
    out = maxAddress;
    return false;
  }

  out = imageBase + static_cast<std::uint64_t>(rva);
  return true;
}

bool isAsciiOnly(const std::string &s) {
  for (char c : s) {
    if (static_cast<std::uint8_t>(c) > 0x7F) {
      return false;
    }
  }

  return true;
}

std::string asciiLower(const std::string &s) {
  std::string out;
  out.reserve(s.size());

  for (char c : s) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      out.push_back(c);
    }
  }

  return out;
}

std::string moduleBasename(const std::string &name) {
  std::size_t slash = name.find_last_of("/\\");
  if (slash == std::string::npos) {
    return name;
  }

  return name.substr(slash + 1);
}

// Normalized module identity: basename, ASCII lower-cased, one optional .dll
// suffix removed.
std::string normalizeModuleName(const std::string &name) {
  std::string base = asciiLower(moduleBasename(name));
  std::size_t len = base.size();

  if (len >= 4 && base.substr(len - 4) == ".dll") {
    base = base.substr(0, len - 4);
  }

  return base;
}

bool isContractName(const std::string &normalized) {
  return normalized.compare(0, 4, "api-") == 0 ||
         normalized.compare(0, 4, "ext-") == 0;
}

std::uint32_t asciiCodeUnit(char c) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(c));
}

struct SectionView {
  SectionView() : base(0), virtualSize(0), rawSize(0), data(nullptr) {
  }

  std::uint64_t base;
  std::uint32_t virtualSize;
  std::uint32_t rawSize;
  bounded_buffer *data;
};

struct SectionCollector {
  SectionCollector() : out(nullptr) {
  }

  std::vector<SectionView> *out;
};

int collectSectionCallback(void *cbd,
                           const VA &va,
                           const std::string &,
                           const image_section_header &hdr,
                           const bounded_buffer *buf) {
  SectionCollector *collector = static_cast<SectionCollector *>(cbd);
  SectionView view;
  view.base = va;
  view.virtualSize = hdr.Misc.VirtualSize;
  view.rawSize = hdr.SizeOfRawData;
  view.data = const_cast<bounded_buffer *>(buf);

  collector->out->push_back(view);
  return 0;
}

// Read-only accessor over a parsed module: maps RVAs to raw section bytes.
class ModuleReader {
public:
  ModuleReader() : pe(nullptr), imageBase(0), pe64(false) {
  }

  bool init(parsed_pe *parsed, std::string *error) {
    pe = parsed;
    if (pe == nullptr) {
      if (error != nullptr) {
        *error = "null parsed_pe";
      }
      return false;
    }

    if (pe->peHeader.nt.OptionalMagic == NT_OPTIONAL_32_MAGIC) {
      pe64 = false;
      imageBase = pe->peHeader.nt.OptionalHeader.ImageBase;
    } else if (pe->peHeader.nt.OptionalMagic == NT_OPTIONAL_64_MAGIC) {
      pe64 = true;
      imageBase = pe->peHeader.nt.OptionalHeader64.ImageBase;
    } else {
      if (error != nullptr) {
        *error = "unsupported optional header magic";
      }
      return false;
    }

    SectionCollector collector;
    collector.out = &sections;
    IterSec(pe, collectSectionCallback, &collector);

    return true;
  }

  bool isPe64() const {
    return pe64;
  }

  std::uint32_t thunkWidth() const {
    return pe64 ? 8 : 4;
  }

  std::uint64_t imageBaseValue() const {
    return imageBase;
  }

  std::uint64_t maxAddress() const {
    return pe64 ? ADDRESS_64_MAX : ADDRESS_32_MAX;
  }

  // Map an RVA to a section offset.  Only mapped (virtual) bytes qualify.
  bool mapRva(std::uint32_t rva,
              std::uint32_t &offset,
              const SectionView *&section) const {
    if (static_cast<std::uint64_t>(rva) >
        ADDRESS_64_MAX - imageBase) {
      // RVA + ImageBase wraps the address width; never mapped.
      return false;
    }

    std::uint64_t absolute = imageBase + static_cast<std::uint64_t>(rva);
    for (const SectionView &s : sections) {
      std::uint32_t span = s.virtualSize;
      if (span == 0) {
        span = s.rawSize;
      }
      std::uint64_t high = s.base + static_cast<std::uint64_t>(span);
      if (absolute >= s.base && absolute < high) {
        offset = static_cast<std::uint32_t>(absolute - s.base);
        section = &s;
        return true;
      }
    }

    return false;
  }

  bool isRvaMapped(std::uint32_t rva) const {
    std::uint32_t offset = 0;
    const SectionView *section = nullptr;
    return mapRva(rva, offset, section);
  }

  bool readBytes(std::uint32_t rva,
                 std::uint32_t len,
                 std::uint8_t *out) const {
    if (out == nullptr) {
      return false;
    }

    std::uint32_t offset = 0;
    const SectionView *section = nullptr;
    if (!mapRva(rva, offset, section) || section->data == nullptr) {
      return false;
    }

    std::uint32_t rawLen = section->data->bufLen;
    if (offset > rawLen || len > rawLen - offset) {
      return false;
    }

    std::memcpy(out, section->data->buf + offset, len);
    return true;
  }

  bool readU16(std::uint32_t rva, std::uint16_t &out) const {
    std::uint8_t raw[2];
    if (!readBytes(rva, 2, raw)) {
      return false;
    }

    out = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(raw[0]) |
        (static_cast<std::uint16_t>(raw[1]) << 8));
    return true;
  }

  bool readU32(std::uint32_t rva, std::uint32_t &out) const {
    std::uint8_t raw[4];
    if (!readBytes(rva, 4, raw)) {
      return false;
    }

    out = static_cast<std::uint32_t>(raw[0]) |
          (static_cast<std::uint32_t>(raw[1]) << 8) |
          (static_cast<std::uint32_t>(raw[2]) << 16) |
          (static_cast<std::uint32_t>(raw[3]) << 24);
    return true;
  }

  bool readU64(std::uint32_t rva, std::uint64_t &out) const {
    std::uint8_t raw[8];
    if (!readBytes(rva, 8, raw)) {
      return false;
    }

    std::uint64_t value = 0;
    for (std::uint32_t i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(raw[i]) << (8 * i);
    }
    out = value;
    return true;
  }

  // Read a NUL terminated ASCII string starting at rva.  When limited is true
  // the NUL must occur within maxBytes; otherwise the string is truncated and
  // is not accepted.  On truncation the bytes read so far are still returned
  // in out.
  bool readAsciiString(std::uint32_t rva,
                       std::uint32_t maxBytes,
                       bool limited,
                       std::string &out,
                       bool &truncated) const {
    out.clear();
    truncated = false;

    std::uint32_t offset = 0;
    const SectionView *section = nullptr;
    if (!mapRva(rva, offset, section) || section->data == nullptr) {
      truncated = true;
      return false;
    }

    std::uint32_t rawLen = section->data->bufLen;
    if (offset >= rawLen) {
      truncated = true;
      return false;
    }

    std::uint32_t available = rawLen - offset;
    std::uint32_t limit = available;
    if (limited && maxBytes < limit) {
      limit = maxBytes;
    }

    const std::uint8_t *bytes = section->data->buf + offset;
    for (std::uint32_t i = 0; i < limit; ++i) {
      if (bytes[i] == 0) {
        out.assign(reinterpret_cast<const char *>(bytes), i);
        return true;
      }
      out.push_back(static_cast<char>(bytes[i]));
    }

    truncated = true;
    return false;
  }

private:
  parsed_pe *pe;
  std::uint64_t imageBase;
  bool pe64;
  std::vector<SectionView> sections;
};

// ---------------------------------------------------------------------------
// Ordinary imports
// ---------------------------------------------------------------------------

bool parseImportTable(parsed_pe *pe,
                      StructuredImportTable &out,
                      std::string &error) {
  ModuleReader reader;
  if (!reader.init(pe, &error)) {
    return false;
  }

  out = StructuredImportTable();

  std::uint32_t dirCount = 0;
  data_directory importDir;
  importDir.VirtualAddress = 0;
  importDir.Size = 0;
  if (pe->peHeader.nt.OptionalMagic == NT_OPTIONAL_32_MAGIC) {
    dirCount = pe->peHeader.nt.OptionalHeader.NumberOfRvaAndSizes;
    if (dirCount > DIR_ENTRY_KIND_IMPORT) {
      importDir =
          pe->peHeader.nt.OptionalHeader.DataDirectory[DIR_ENTRY_KIND_IMPORT];
    }
  } else {
    dirCount = pe->peHeader.nt.OptionalHeader64.NumberOfRvaAndSizes;
    if (dirCount > DIR_ENTRY_KIND_IMPORT) {
      importDir = pe->peHeader.nt.OptionalHeader64
                      .DataDirectory[DIR_ENTRY_KIND_IMPORT];
    }
  }

  if (dirCount <= DIR_ENTRY_KIND_IMPORT) {
    return true;
  }

  out.importDirectoryRva = importDir.VirtualAddress;
  out.importDirectorySize = importDir.Size;
  if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
    return true;
  }

  out.hasImportDirectory = true;

  const std::uint32_t descriptorSize = 20;
  std::uint32_t descRva = importDir.VirtualAddress;

  for (std::uint32_t descIndex = 0; descIndex < MAX_IMPORT_DESCRIPTORS;
       ++descIndex, descRva += descriptorSize) {
    if (descRva > ADDRESS_32_MAX - descriptorSize) {
      out.diagnostics.push_back("import descriptor RVA overflow");
      return true;
    }

    std::uint32_t lookupRva = 0;
    std::uint32_t timeStamp = 0;
    std::uint32_t forwarderChain = 0;
    std::uint32_t nameRva = 0;
    std::uint32_t addressRva = 0;

    if (!reader.readU32(descRva, lookupRva) ||
        !reader.readU32(descRva + 4, timeStamp) ||
        !reader.readU32(descRva + 8, forwarderChain) ||
        !reader.readU32(descRva + 12, nameRva) ||
        !reader.readU32(descRva + 16, addressRva)) {
      out.diagnostics.push_back("truncated import descriptor at RVA " +
                                std::to_string(descRva));
      return true;
    }

    static_cast<void>(timeStamp);
    static_cast<void>(forwarderChain);

    if (lookupRva == 0 && timeStamp == 0 && forwarderChain == 0 &&
        nameRva == 0 && addressRva == 0) {
      return true;
    }

    std::string moduleName;
    bool truncated = false;
    if (!reader.readAsciiString(nameRva, 0, false, moduleName, truncated)) {
      out.diagnostics.push_back("unreadable import module name at RVA " +
                                std::to_string(nameRva));
      continue;
    }

    std::uint32_t effectiveLookup = (lookupRva != 0) ? lookupRva : addressRva;
    if (effectiveLookup == 0 || addressRva == 0) {
      out.diagnostics.push_back("import descriptor for " + moduleName +
                                " has no lookup or IAT table");
      continue;
    }

    const std::uint32_t width = reader.thunkWidth();
    std::uint64_t thunkCursor =
        static_cast<std::uint64_t>(effectiveLookup);

    for (std::uint32_t thunkIndex = 0;
         thunkIndex < MAX_THUNKS_PER_DESCRIPTOR; ++thunkIndex) {
      if (thunkCursor > ADDRESS_32_MAX) {
        out.diagnostics.push_back("import thunk table RVA overflow for " +
                                  moduleName);
        break;
      }
      const std::uint32_t thunkRva =
          static_cast<std::uint32_t>(thunkCursor);
      bool isOrdinal = false;
      std::uint64_t rawValue = 0;

      if (width == 4) {
        std::uint32_t v32 = 0;
        if (!reader.readU32(thunkRva, v32)) {
          out.diagnostics.push_back("truncated import thunk table for " +
                                    moduleName);
          break;
        }
        if (v32 == 0) {
          break;
        }
        rawValue = v32;
        isOrdinal = (v32 & 0x80000000u) != 0;
      } else {
        std::uint64_t v64 = 0;
        if (!reader.readU64(thunkRva, v64)) {
          out.diagnostics.push_back("truncated import thunk table for " +
                                    moduleName);
          break;
        }
        if (v64 == 0) {
          break;
        }
        rawValue = v64;
        isOrdinal = (v64 & 0x8000000000000000ull) != 0;
      }

      StructuredImport entry;
      entry.moduleName = moduleName;
      entry.slotWidth = width;
      entry.thunkIndex = thunkIndex;
      entry.lookupTableRva = effectiveLookup;

      std::uint64_t iatRva = static_cast<std::uint64_t>(addressRva) +
                             static_cast<std::uint64_t>(thunkIndex) *
                                 static_cast<std::uint64_t>(width);
      if (iatRva > ADDRESS_32_MAX) {
        out.diagnostics.push_back("IAT slot RVA overflow for " + moduleName);
        break;
      }
      entry.iatRva = static_cast<std::uint32_t>(iatRva);

      if (isOrdinal) {
        entry.kind = ImportNameKind::ByOrdinal;
        entry.ordinal = static_cast<std::uint16_t>(rawValue & 0xFFFFull);
        out.entries.push_back(entry);
      } else {
        std::uint64_t nameRva64 = rawValue & 0x7FFFFFFFFFFFFFFFull;
        if (nameRva64 > ADDRESS_32_MAX) {
          out.diagnostics.push_back("import-by-name RVA overflow for " +
                                    moduleName);
          break;
        }

        std::uint32_t namedRva = static_cast<std::uint32_t>(nameRva64);
        std::uint16_t hint = 0;
        if (namedRva > ADDRESS_32_MAX - 2 ||
            !reader.readU16(namedRva, hint)) {
          out.diagnostics.push_back("unreadable import hint for " +
                                    moduleName);
        } else {
          static_cast<void>(hint);
          std::string symbol;
          bool symbolTruncated = false;
          if (!reader.readAsciiString(
                  namedRva + 2, 0, false, symbol, symbolTruncated)) {
            static_cast<void>(symbolTruncated);
            out.diagnostics.push_back("truncated import name for " +
                                      moduleName);
          } else {
            entry.kind = ImportNameKind::ByName;
            entry.symbolName = symbol;
            out.entries.push_back(entry);
          }
        }
      }

      thunkCursor += static_cast<std::uint64_t>(width);
    }
  }

  out.diagnostics.push_back("too many import descriptors");
  return true;
}

bool parseExportTable(parsed_pe *pe,
                      StructuredExportTable &out,
                      std::string &error) {
  ModuleReader reader;
  if (!reader.init(pe, &error)) {
    return false;
  }

  out = StructuredExportTable();

  std::uint32_t dirCount = 0;
  data_directory exportDir;
  exportDir.VirtualAddress = 0;
  exportDir.Size = 0;
  if (pe->peHeader.nt.OptionalMagic == NT_OPTIONAL_32_MAGIC) {
    dirCount = pe->peHeader.nt.OptionalHeader.NumberOfRvaAndSizes;
    if (dirCount > DIR_ENTRY_KIND_EXPORT) {
      exportDir =
          pe->peHeader.nt.OptionalHeader.DataDirectory[DIR_ENTRY_KIND_EXPORT];
    }
  } else {
    dirCount = pe->peHeader.nt.OptionalHeader64.NumberOfRvaAndSizes;
    if (dirCount > DIR_ENTRY_KIND_EXPORT) {
      exportDir = pe->peHeader.nt.OptionalHeader64
                      .DataDirectory[DIR_ENTRY_KIND_EXPORT];
    }
  }

  if (dirCount <= DIR_ENTRY_KIND_EXPORT) {
    return true;
  }

  out.exportDirectoryRva = exportDir.VirtualAddress;
  out.exportDirectorySize = exportDir.Size;
  if (exportDir.VirtualAddress == 0 || exportDir.Size == 0) {
    return true;
  }

  out.hasExportDirectory = true;

  const std::uint32_t exportRva = exportDir.VirtualAddress;
  std::uint64_t exportEnd =
      static_cast<std::uint64_t>(exportRva) + exportDir.Size;
  if (exportEnd > ADDRESS_32_MAX) {
    exportEnd = ADDRESS_32_MAX;
  }

  std::uint32_t flags = 0;
  std::uint32_t timeDateStamp = 0;
  std::uint32_t nameRva = 0;
  std::uint32_t ordinalBase = 0;
  std::uint32_t addressTableEntries = 0;
  std::uint32_t numberOfNamePointers = 0;
  std::uint32_t eatRva = 0;
  std::uint32_t namePointerRva = 0;
  std::uint32_t ordinalTableRva = 0;

  if (!reader.readU32(exportRva, flags) ||
      !reader.readU32(exportRva + 4, timeDateStamp) ||
      !reader.readU32(exportRva + 12, nameRva) ||
      !reader.readU32(exportRva + 16, ordinalBase) ||
      !reader.readU32(exportRva + 20, addressTableEntries) ||
      !reader.readU32(exportRva + 24, numberOfNamePointers) ||
      !reader.readU32(exportRva + 28, eatRva) ||
      !reader.readU32(exportRva + 32, namePointerRva) ||
      !reader.readU32(exportRva + 36, ordinalTableRva)) {
    error = "truncated export directory";
    return false;
  }

  static_cast<void>(flags);
  static_cast<void>(timeDateStamp);

  out.ordinalBase = ordinalBase;
  out.addressTableEntries = addressTableEntries;

  if (nameRva != 0) {
    bool truncated = false;
    if (!reader.readAsciiString(nameRva, 0, false, out.moduleName,
                                truncated)) {
      out.diagnostics.push_back("unreadable export module name at RVA " +
                                std::to_string(nameRva));
    }
  }

  if (addressTableEntries > MAX_EXPORT_SLOTS) {
    error = "too many export address table entries";
    return false;
  }

  for (std::uint32_t i = 0; i < addressTableEntries; ++i) {
    std::uint64_t slotRva64 =
        static_cast<std::uint64_t>(eatRva) +
        static_cast<std::uint64_t>(i) * 4ull;
    if (slotRva64 > ADDRESS_32_MAX) {
      out.diagnostics.push_back("export address table RVA overflow");
      break;
    }

    std::uint32_t slotRva = static_cast<std::uint32_t>(slotRva64);
    std::uint32_t value = 0;
    if (!reader.readU32(slotRva, value)) {
      out.diagnostics.push_back("truncated export address table at RVA " +
                                std::to_string(slotRva));
      break;
    }

    StructuredExportSlot slot;
    slot.ordinalBase = ordinalBase;
    slot.slotIndex = i;
    std::uint64_t ordinal64 = static_cast<std::uint64_t>(ordinalBase) + i;
    if (ordinal64 > ADDRESS_32_MAX) {
      out.diagnostics.push_back("export ordinal overflow at slot " +
                                std::to_string(i));
      slot.ordinal = static_cast<std::uint32_t>(ADDRESS_32_MAX);
    } else {
      slot.ordinal = static_cast<std::uint32_t>(ordinal64);
    }
    slot.rva = value;
    slot.eatSlotRva = slotRva;

    if (value != 0 && value >= exportRva && value < exportEnd) {
      slot.forwarded = true;
      slot.forwardStringRva = value;
      slot.forwardRangeEnd = static_cast<std::uint32_t>(exportEnd);
      std::uint32_t maxBytes =
          static_cast<std::uint32_t>(exportEnd - value);
      std::string text;
      bool truncated = false;
      if (!reader.readAsciiString(value, maxBytes, true, text, truncated)) {
        slot.forwardStringValid = false;
        slot.forwardString = text;
        out.diagnostics.push_back(
            "truncated forwarder string at RVA " + std::to_string(value) +
            " (export directory ends at RVA " +
            std::to_string(exportEnd) + ")");
      } else {
        slot.forwardStringValid = true;
        slot.forwardString = text;
      }
    }

    out.slots.push_back(slot);
  }

  if (numberOfNamePointers > MAX_EXPORT_NAMES) {
    error = "too many export name pointers";
    return false;
  }

  for (std::uint32_t i = 0; i < numberOfNamePointers; ++i) {
    std::uint64_t pointerRva64 =
        static_cast<std::uint64_t>(namePointerRva) +
        static_cast<std::uint64_t>(i) * 4ull;
    std::uint64_t ordinalRva64 =
        static_cast<std::uint64_t>(ordinalTableRva) +
        static_cast<std::uint64_t>(i) * 2ull;

    if (pointerRva64 > ADDRESS_32_MAX || ordinalRva64 > ADDRESS_32_MAX) {
      out.diagnostics.push_back("export name table RVA overflow");
      break;
    }

    std::uint32_t curNameRva = 0;
    std::uint16_t ordinal = 0;
    if (!reader.readU32(static_cast<std::uint32_t>(pointerRva64), curNameRva) ||
        !reader.readU16(static_cast<std::uint32_t>(ordinalRva64), ordinal)) {
      out.diagnostics.push_back("truncated export name tables");
      break;
    }

    std::string name;
    bool truncated = false;
    if (!reader.readAsciiString(curNameRva, 0, false, name, truncated)) {
      out.diagnostics.push_back("unreadable export name at RVA " +
                                std::to_string(curNameRva));
      continue;
    }

    if (ordinal < ordinalBase) {
      out.diagnostics.push_back("export name " + name +
                                " has an ordinal below the ordinal base");
      continue;
    }

    std::uint32_t slotIndex =
        static_cast<std::uint32_t>(ordinal) - ordinalBase;
    if (slotIndex >= static_cast<std::uint32_t>(out.slots.size())) {
      out.diagnostics.push_back("export name " + name +
                                " references a nonexistent slot");
      continue;
    }

    StructuredExportName n;
    n.name = name;
    n.nameRva = curNameRva;
    n.nameTableIndex = i;
    out.slots[slotIndex].names.push_back(n);
  }

  return true;
}

// ---------------------------------------------------------------------------
// API-set namespace (version 6)
// ---------------------------------------------------------------------------

enum class SchemaState {
  Absent = 0,
  Ok,
  UnsupportedVersion,
  Malformed,
  UnsupportedEncoding,
};

enum class DecodeResult {
  Ok = 0,
  OutOfBounds,
  Malformed,
  Unsupported,
};

struct SchemaValue {
  SchemaValue()
      : flags(0), nameOffset(0), nameLength(0), hostOffset(0),
        hostLength(0), recordOffset(0) {
  }

  std::uint32_t flags;
  std::uint32_t nameOffset;
  std::uint32_t nameLength;
  std::uint32_t hostOffset;
  std::uint32_t hostLength;
  std::uint32_t recordOffset;
  std::string alias;
  std::string normalizedAlias;
  std::string host;
  std::string normalizedHost;
};

struct SchemaEntry {
  SchemaEntry()
      : flags(0), nameOffset(0), nameLength(0), hashedLength(0),
        valueOffset(0), valueCount(0), recordOffset(0) {
  }

  std::uint32_t flags;
  std::uint32_t nameOffset;
  std::uint32_t nameLength;
  std::uint32_t hashedLength;
  std::uint32_t valueOffset;
  std::uint32_t valueCount;
  std::uint32_t recordOffset;
  std::string fullName;
  std::string lowerName;
  std::vector<SchemaValue> values;
};

bool readTableU32(const std::uint8_t *data,
                  std::uint32_t limit,
                  std::uint32_t offset,
                  std::uint32_t &out) {
  if (offset > limit || limit - offset < 4) {
    return false;
  }

  out = static_cast<std::uint32_t>(data[offset]) |
        (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(data[offset + 3]) << 24);
  return true;
}

DecodeResult decodeUtf16Ascii(const std::uint8_t *data,
                              std::uint32_t limit,
                              std::uint32_t offset,
                              std::uint32_t byteLength,
                              std::string &out) {
  out.clear();
  if (byteLength == 0) {
    return DecodeResult::Ok;
  }
  if (offset > limit || limit - offset < byteLength) {
    return DecodeResult::OutOfBounds;
  }
  if ((byteLength & 1u) != 0) {
    return DecodeResult::Malformed;
  }

  for (std::uint32_t i = 0; i < byteLength; i += 2) {
    std::uint16_t unit = static_cast<std::uint16_t>(data[offset + i]) |
                         (static_cast<std::uint16_t>(data[offset + i + 1])
                          << 8);
    if (unit > 0x7F) {
      out.clear();
      return DecodeResult::Unsupported;
    }
    out.push_back(static_cast<char>(unit));
  }

  return DecodeResult::Ok;
}

class ApiSetSchema {
public:
  ApiSetSchema()
      : state(SchemaState::Absent), version(0), hashFactor(0),
        namespaceSize(0) {
  }

  void parse(const std::uint8_t *data, std::uint32_t size) {
    state = SchemaState::Absent;
    version = 0;
    hashFactor = 0;
    namespaceSize = 0;
    entries.clear();
    hashes.clear();
    diagnostics.clear();

    if (data == nullptr || size == 0) {
      return;
    }

    if (size < 28) {
      markMalformed("namespace smaller than the 28 byte header");
      return;
    }

    std::uint32_t headerVersion = 0;
    std::uint32_t headerSize = 0;
    std::uint32_t flags = 0;
    std::uint32_t count = 0;
    std::uint32_t entryOffset = 0;
    std::uint32_t hashOffset = 0;
    std::uint32_t factor = 0;

    if (!readTableU32(data, size, 0, headerVersion) ||
        !readTableU32(data, size, 4, headerSize) ||
        !readTableU32(data, size, 8, flags) ||
        !readTableU32(data, size, 12, count) ||
        !readTableU32(data, size, 16, entryOffset) ||
        !readTableU32(data, size, 20, hashOffset) ||
        !readTableU32(data, size, 24, factor)) {
      markMalformed("truncated namespace header");
      return;
    }

    version = headerVersion;
    if (headerVersion != 6) {
      state = SchemaState::UnsupportedVersion;
      diagnostics.push_back("unsupported API-set namespace version " +
                            std::to_string(headerVersion) +
                            " (only version 6 is supported)");
      return;
    }

    if (headerSize < 28 || headerSize > size) {
      markMalformed("namespace size field is outside the supplied buffer");
      return;
    }

    namespaceSize = headerSize;
    hashFactor = factor;

    static_cast<void>(flags);

    std::uint64_t entryEnd =
        static_cast<std::uint64_t>(entryOffset) +
        static_cast<std::uint64_t>(count) * 24ull;
    std::uint64_t hashEnd =
        static_cast<std::uint64_t>(hashOffset) +
        static_cast<std::uint64_t>(count) * 8ull;

    if (entryEnd > namespaceSize || hashEnd > namespaceSize) {
      markMalformed("entry or hash table extends past the namespace");
      return;
    }

    entries.resize(count);
    hashes.resize(count);

    for (std::uint32_t i = 0; i < count; ++i) {
      std::uint64_t record = static_cast<std::uint64_t>(entryOffset) +
                             static_cast<std::uint64_t>(i) * 24ull;
      std::uint32_t entryFlags = 0;
      std::uint32_t nameOffset = 0;
      std::uint32_t nameLength = 0;
      std::uint32_t hashedLength = 0;
      std::uint32_t valueOffset = 0;
      std::uint32_t valueCount = 0;

      if (!readTableU32(data, namespaceSize,
                        static_cast<std::uint32_t>(record), entryFlags) ||
          !readTableU32(data, namespaceSize,
                        static_cast<std::uint32_t>(record) + 4, nameOffset) ||
          !readTableU32(data, namespaceSize,
                        static_cast<std::uint32_t>(record) + 8, nameLength) ||
          !readTableU32(data, namespaceSize,
                        static_cast<std::uint32_t>(record) + 12,
                        hashedLength) ||
          !readTableU32(data, namespaceSize,
                        static_cast<std::uint32_t>(record) + 16, valueOffset) ||
          !readTableU32(data, namespaceSize,
                        static_cast<std::uint32_t>(record) + 20,
                        valueCount)) {
        markMalformed("truncated API-set entry record");
        return;
      }

      if ((hashedLength & 1u) != 0) {
        markMalformed("HashedLength is not even");
        return;
      }
      if (hashedLength > nameLength) {
        markMalformed("HashedLength exceeds NameLength");
        return;
      }
      if ((nameLength & 1u) != 0) {
        markMalformed("entry NameLength is not even");
        return;
      }

      std::string fullName;
      DecodeResult decoded = decodeUtf16Ascii(
          data, namespaceSize, nameOffset, nameLength, fullName);
      if (decoded == DecodeResult::OutOfBounds) {
        markMalformed("entry name range is outside the namespace");
        return;
      }
      if (decoded == DecodeResult::Unsupported) {
        state = SchemaState::UnsupportedEncoding;
        diagnostics.push_back(
            "entry name contains non-ASCII UTF-16 code units");
        return;
      }
      if (decoded == DecodeResult::Malformed) {
        markMalformed("entry name has an odd byte length");
        return;
      }

      SchemaEntry entry;
      entry.flags = entryFlags;
      entry.nameOffset = nameOffset;
      entry.nameLength = nameLength;
      entry.hashedLength = hashedLength;
      entry.valueOffset = valueOffset;
      entry.valueCount = valueCount;
      entry.recordOffset = static_cast<std::uint32_t>(record);
      entry.fullName = fullName;
      entry.lowerName = asciiLower(fullName);

      std::uint64_t valueEnd =
          static_cast<std::uint64_t>(valueOffset) +
          static_cast<std::uint64_t>(valueCount) * 20ull;
      if (valueEnd > namespaceSize) {
        markMalformed("API-set value table extends past the namespace");
        return;
      }

      for (std::uint32_t v = 0; v < valueCount; ++v) {
        std::uint64_t recordOff = static_cast<std::uint64_t>(valueOffset) +
                                  static_cast<std::uint64_t>(v) * 20ull;
        std::uint32_t valueFlags = 0;
        std::uint32_t valueNameOffset = 0;
        std::uint32_t valueNameLength = 0;
        std::uint32_t hostOffset = 0;
        std::uint32_t hostLength = 0;

        if (!readTableU32(data, namespaceSize,
                          static_cast<std::uint32_t>(recordOff), valueFlags) ||
            !readTableU32(data, namespaceSize,
                          static_cast<std::uint32_t>(recordOff) + 4,
                          valueNameOffset) ||
            !readTableU32(data, namespaceSize,
                          static_cast<std::uint32_t>(recordOff) + 8,
                          valueNameLength) ||
            !readTableU32(data, namespaceSize,
                          static_cast<std::uint32_t>(recordOff) + 12,
                          hostOffset) ||
            !readTableU32(data, namespaceSize,
                          static_cast<std::uint32_t>(recordOff) + 16,
                          hostLength)) {
          markMalformed("truncated API-set value record");
          return;
        }

        SchemaValue value;
        value.flags = valueFlags;
        value.nameOffset = valueNameOffset;
        value.nameLength = valueNameLength;
        value.hostOffset = hostOffset;
        value.hostLength = hostLength;
        value.recordOffset = static_cast<std::uint32_t>(recordOff);

        std::string alias;
        if (valueNameLength != 0) {
          DecodeResult aliasDecoded =
              decodeUtf16Ascii(data, namespaceSize, valueNameOffset,
                               valueNameLength, alias);
          if (aliasDecoded == DecodeResult::OutOfBounds) {
            markMalformed("alias name range is outside the namespace");
            return;
          }
          if (aliasDecoded == DecodeResult::Unsupported) {
            state = SchemaState::UnsupportedEncoding;
            diagnostics.push_back(
                "alias name contains non-ASCII UTF-16 code units");
            return;
          }
          if (aliasDecoded == DecodeResult::Malformed) {
            markMalformed("alias name has an odd byte length");
            return;
          }
        }
        value.alias = alias;
        value.normalizedAlias =
            (valueNameLength == 0) ? std::string() : normalizeModuleName(alias);

        std::string host;
        if (hostLength != 0) {
          DecodeResult hostDecoded = decodeUtf16Ascii(
              data, namespaceSize, hostOffset, hostLength, host);
          if (hostDecoded == DecodeResult::OutOfBounds) {
            markMalformed("host name range is outside the namespace");
            return;
          }
          if (hostDecoded == DecodeResult::Unsupported) {
            state = SchemaState::UnsupportedEncoding;
            diagnostics.push_back(
                "host name contains non-ASCII UTF-16 code units");
            return;
          }
          if (hostDecoded == DecodeResult::Malformed) {
            markMalformed("host name has an odd byte length");
            return;
          }
        }
        value.host = host;
        value.normalizedHost = normalizeModuleName(host);

        if (v == 0 && valueNameLength != 0) {
          markMalformed("default API-set value has a non-empty name");
          return;
        }

        if (v != 0) {
          if (value.normalizedAlias.empty()) {
            markMalformed("alias name normalizes to empty");
            return;
          }
          for (std::size_t p = 1; p < entry.values.size(); ++p) {
            if (!entry.values[p].normalizedAlias.empty() &&
                entry.values[p].normalizedAlias == value.normalizedAlias) {
              markMalformed("duplicate normalized API-set alias " +
                            value.normalizedAlias);
              return;
            }
          }
        }

        entry.values.push_back(value);
      }

      entries[i] = entry;
    }

    for (std::uint32_t i = 0; i < count; ++i) {
      std::uint64_t record = static_cast<std::uint64_t>(hashOffset) +
                             static_cast<std::uint64_t>(i) * 8ull;
      std::uint32_t hash = 0;
      std::uint32_t index = 0;

      if (!readTableU32(data, namespaceSize,
                        static_cast<std::uint32_t>(record), hash) ||
          !readTableU32(data, namespaceSize,
                        static_cast<std::uint32_t>(record) + 4, index)) {
        markMalformed("truncated API-set hash record");
        return;
      }

      if (index >= count) {
        markMalformed("API-set hash index is out of range");
        return;
      }
      if (i > 0 && hash < hashes[i - 1].first) {
        markMalformed("API-set hash table is not sorted");
        return;
      }

      hashes[i] = std::make_pair(hash, index);
    }

    state = SchemaState::Ok;
  }

  SchemaState state;
  std::uint32_t version;
  std::uint32_t hashFactor;
  std::uint32_t namespaceSize;
  std::vector<SchemaEntry> entries;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> hashes;
  std::vector<std::string> diagnostics;

private:
  void markMalformed(const std::string &why) {
    state = SchemaState::Malformed;
    diagnostics.push_back(why);
  }
};

// Compute the hashed prefix of a contract name: everything before the last
// '-'.  If the name has no '-', the whole name is used.
std::string contractHashPrefix(const std::string &normalized) {
  std::size_t dash = normalized.find_last_of('-');
  if (dash == std::string::npos) {
    return normalized;
  }

  return normalized.substr(0, dash);
}

struct HostSelection {
  HostSelection()
      : ok(false), errorCode(BindingStatus::ApiSetMalformed),
        entryOffset(0), valueOffset(0), hostOffset(0), usedDefault(false) {
  }

  bool ok;
  BindingStatus errorCode;
  std::string error;
  std::string host;
  std::string originalHost;
  std::string contractName;
  std::uint32_t entryOffset;
  std::uint32_t valueOffset;
  std::uint32_t hostOffset;
  bool usedDefault;
};

class BindingContext {
public:
  struct ModuleEntry {
    ModuleEntry()
        : input(nullptr), exportOk(false), exportReadable(false) {
    }

    const BindingModuleInput *input;
    ModuleReader reader;
    StructuredExportTable exports;
    bool exportOk;
    bool exportReadable;
    std::string exportError;
  };

  BindingContext() {
  }

  ~BindingContext() {
    for (ModuleEntry *entry : ownedModules) {
      delete entry;
    }
  }

  ModuleEntry *find(const std::string &normalized) const {
    std::map<std::string, ModuleEntry *>::const_iterator it =
        modules.find(normalized);
    if (it == modules.end()) {
      return nullptr;
    }

    return it->second;
  }

  // The cache key includes the importer so two different shims forwarding the
  // same contract can resolve to different hosts.
  HostSelection selectHost(const std::string &contract,
                           const std::string &importer) {
    std::string key = contract + "\x1f" + importer;
    std::map<std::string, HostSelection>::const_iterator cached =
        hostCache.find(key);
    if (cached != hostCache.end()) {
      return cached->second;
    }

    HostSelection selection = computeHost(contract, importer);
    hostCache[key] = selection;
    return selection;
  }

  std::map<std::string, ModuleEntry *> modules;
  ApiSetSchema schema;
  std::vector<ModuleEntry *> ownedModules;

private:
  std::map<std::string, HostSelection> hostCache;

  HostSelection computeHost(const std::string &contract,
                            const std::string &importer) const {
    HostSelection result;

    switch (schema.state) {
      case SchemaState::Absent:
        result.errorCode = BindingStatus::ApiSetMissingSchema;
        result.error = "no API-set namespace supplied for contract " +
                       contract;
        return result;
      case SchemaState::UnsupportedVersion:
        result.errorCode = BindingStatus::ApiSetUnsupportedVersion;
        result.error = "unsupported API-set namespace version " +
                       std::to_string(schema.version);
        return result;
      case SchemaState::Malformed:
        result.errorCode = BindingStatus::ApiSetMalformed;
        result.error = "malformed API-set namespace for contract " + contract;
        return result;
      case SchemaState::UnsupportedEncoding:
        result.errorCode = BindingStatus::ApiSetUnsupportedEncoding;
        result.error =
            "unsupported API-set namespace encoding for contract " + contract;
        return result;
      case SchemaState::Ok:
        break;
      default:
        result.errorCode = BindingStatus::ApiSetMalformed;
        result.error = "unrecognized API-set namespace state";
        return result;
    }

    if (!isAsciiOnly(contract)) {
      result.errorCode = BindingStatus::ApiSetUnsupportedEncoding;
      result.error = "contract name contains non-ASCII bytes";
      return result;
    }

    std::string prefix = contractHashPrefix(contract);
    std::uint32_t hash = 0;
    for (char c : prefix) {
      hash = hash * schema.hashFactor + asciiCodeUnit(c);
    }

    const std::uint32_t prefixUnits =
        static_cast<std::uint32_t>(prefix.size());
    std::set<std::uint32_t> candidates;

    for (std::size_t i = 0; i < schema.hashes.size(); ++i) {
      if (schema.hashes[i].first != hash) {
        continue;
      }

      std::uint32_t index = schema.hashes[i].second;
      if (index >= schema.entries.size()) {
        continue;
      }

      const SchemaEntry &entry = schema.entries[index];
      const std::uint32_t hashedUnits = entry.hashedLength / 2u;
      if (hashedUnits != prefixUnits) {
        continue;
      }

      bool matches = true;
      for (std::uint32_t u = 0; u < hashedUnits; ++u) {
        char expected = static_cast<std::size_t>(u) < entry.lowerName.size()
                            ? entry.lowerName[u]
                            : '\0';
        if (expected != prefix[u]) {
          matches = false;
          break;
        }
      }
      if (!matches) {
        continue;
      }

      candidates.insert(index);
    }

    if (candidates.empty()) {
      result.errorCode = BindingStatus::ApiSetNotFound;
      result.error = "no API-set entry matches contract " + contract;
      return result;
    }

    if (candidates.size() > 1) {
      result.errorCode = BindingStatus::ApiSetAmbiguous;
      result.error = "multiple API-set entries match contract " + contract;
      return result;
    }

    const std::uint32_t index = *candidates.begin();
    const SchemaEntry &entry = schema.entries[index];
    result.contractName = entry.fullName;
    result.entryOffset = entry.recordOffset;

    if (entry.values.empty()) {
      result.errorCode = BindingStatus::ApiSetMissingHost;
      result.error = "API-set entry " + entry.fullName + " has no values";
      return result;
    }

    const SchemaValue *chosen = nullptr;
    for (std::size_t v = 1; v < entry.values.size(); ++v) {
      if (!entry.values[v].normalizedAlias.empty() &&
          entry.values[v].normalizedAlias == importer) {
        chosen = &entry.values[v];
        result.usedDefault = false;
        break;
      }
    }

    if (chosen == nullptr) {
      chosen = &entry.values[0];
      result.usedDefault = true;
    }

    result.valueOffset = chosen->recordOffset;
    result.hostOffset = chosen->hostOffset;

    if (chosen->host.empty()) {
      if (!result.usedDefault) {
        result.errorCode = BindingStatus::ApiSetDisabled;
        result.error = "API-set entry " + entry.fullName +
                       " disables importer " + importer;
      } else {
        result.errorCode = BindingStatus::ApiSetMissingHost;
        result.error = "API-set entry " + entry.fullName +
                       " has no host for importer " + importer;
      }
      return result;
    }

    result.host = chosen->normalizedHost;
    result.originalHost = chosen->host;
    if (result.host.empty()) {
      result.errorCode = BindingStatus::ApiSetMissingHost;
      result.error =
          "API-set entry " + entry.fullName + " yielded an empty host";
      return result;
    }

    result.ok = true;
    return result;
  }
};

struct SymbolState {
  SymbolState() : kind(ImportNameKind::ByName), ordinal(0) {
  }

  std::string module;
  ImportNameKind kind;
  std::string name;
  std::uint16_t ordinal;
};

std::string stateKey(const SymbolState &state) {
  std::string key = state.module;
  key += "!";
  if (state.kind == ImportNameKind::ByName) {
    key += state.name;
  } else {
    key += "#";
    key += std::to_string(static_cast<std::uint32_t>(state.ordinal));
  }
  return key;
}

struct ForwardTarget {
  ForwardTarget() : kind(ImportNameKind::ByName), ordinal(0) {
  }

  std::string module;
  ImportNameKind kind;
  std::string name;
  std::uint16_t ordinal;
};

bool parseForwarderString(const std::string &text,
                          ForwardTarget &target,
                          std::string &why) {
  std::size_t dot = text.find('.');
  if (dot == std::string::npos) {
    why = "forwarder has no '.' separator";
    return false;
  }
  if (dot == 0) {
    why = "forwarder has an empty module stem";
    return false;
  }
  if (dot + 1 >= text.size()) {
    why = "forwarder has an empty symbol";
    return false;
  }

  std::string stem = text.substr(0, dot);
  std::string symbol = text.substr(dot + 1);

  target.module = normalizeModuleName(stem);
  if (target.module.empty()) {
    why = "forwarder module stem normalizes to empty";
    return false;
  }

  if (symbol[0] == '#') {
    if (symbol.size() < 2) {
      why = "forwarder ordinal is empty";
      return false;
    }

    std::uint32_t value = 0;
    for (std::size_t i = 1; i < symbol.size(); ++i) {
      char c = symbol[i];
      if (c < '0' || c > '9') {
        why = "forwarder ordinal is not decimal";
        return false;
      }
      value = value * 10u + static_cast<std::uint32_t>(c - '0');
      if (value > 65535u) {
        why = "forwarder ordinal exceeds 65535";
        return false;
      }
    }

    target.kind = ImportNameKind::ByOrdinal;
    target.ordinal = static_cast<std::uint16_t>(value);
    return true;
  }

  target.kind = ImportNameKind::ByName;
  target.name = symbol;
  return true;
}

struct ResolveOutcome {
  ResolveOutcome() : status(BindingStatus::Resolved), ok(false) {
  }

  BindingStatus status;
  bool ok;
  std::string diagnostic;
};

std::string joinStates(const std::vector<std::string> &states,
                       std::size_t from) {
  std::string out;
  for (std::size_t i = from; i < states.size(); ++i) {
    if (!out.empty()) {
      out += " -> ";
    }
    out += states[i];
  }
  return out;
}

ResolveOutcome resolveSymbol(BindingContext &context,
                             const std::string &rootModule,
                             const SymbolState &start,
                             BindingRow &row) {
  ResolveOutcome outcome;

  SymbolState cur = start;
  // The module whose import currently names the state's module.  For the root
  // import that is the root itself; each export forward replaces it with the
  // forwarder.  API-set hops keep it, so a shim's alias governs its contracts.
  std::string importer = rootModule;
  std::map<std::string, std::size_t> seen;
  row.stateChain.clear();
  row.hops.clear();

  while (true) {
    // A contract's identity includes the importer: the same contract reached
    // from a different shim may legitimately map to a different host, so it
    // is not the same state and must not be reported as a cycle.
    std::string key = stateKey(cur);
    if (isContractName(cur.module)) {
      key += "@";
      key += importer;
    }

    std::map<std::string, std::size_t>::const_iterator prior = seen.find(key);
    if (prior != seen.end()) {
      row.stateChain.push_back(key);
      outcome.status = BindingStatus::Cycle;
      outcome.diagnostic =
          "cycle closed by repeated state: " +
          joinStates(row.stateChain, prior->second);
      return outcome;
    }

    seen[key] = row.stateChain.size();
    row.stateChain.push_back(key);

    if (isContractName(cur.module)) {
      HostSelection selection = context.selectHost(cur.module, importer);

      if (!selection.ok) {
        outcome.status = selection.errorCode;
        outcome.diagnostic = selection.error;
        return outcome;
      }

      BindingHop hop;
      hop.kind = BindingHopKind::ApiSetContract;
      hop.sourceModule = importer;
      hop.targetModule = selection.host;
      hop.contractName = selection.contractName;
      hop.schemaEntryOffset = selection.entryOffset;
      hop.schemaValueOffset = selection.valueOffset;
      hop.schemaHostOffset = selection.hostOffset;
      hop.schemaHost = selection.originalHost;
      hop.importerModule = importer;
      hop.usedDefaultAlias = selection.usedDefault;
      row.hops.push_back(hop);

      cur.module = selection.host;
      continue;
    }

    const BindingContext::ModuleEntry *module = context.find(cur.module);
    if (module == nullptr) {
      outcome.status = BindingStatus::MissingModule;
      outcome.diagnostic =
          "module " + cur.module + " is not in the supplied module set";
      return outcome;
    }

    if (!module->exportOk || !module->exportReadable) {
      outcome.status = BindingStatus::ExportTableInvalid;
      outcome.diagnostic = "export table of " + cur.module +
                           " could not be read: " + module->exportError;
      return outcome;
    }

    const StructuredExportTable &exports = module->exports;
    if (!exports.hasExportDirectory || exports.slots.empty()) {
      if (cur.kind == ImportNameKind::ByName) {
        outcome.status = BindingStatus::MissingExportName;
        outcome.diagnostic =
            "module " + cur.module + " has no export named " + cur.name;
      } else {
        outcome.status = BindingStatus::MissingExportOrdinal;
        outcome.diagnostic = "module " + cur.module +
                             " has no export ordinal " +
                             std::to_string(cur.ordinal);
      }
      return outcome;
    }

    const StructuredExportSlot *slot = nullptr;
    if (cur.kind == ImportNameKind::ByName) {
      for (const StructuredExportSlot &candidate : exports.slots) {
        for (const StructuredExportName &name : candidate.names) {
          if (name.name == cur.name) {
            slot = &candidate;
            break;
          }
        }
        if (slot != nullptr) {
          break;
        }
      }

      if (slot == nullptr) {
        outcome.status = BindingStatus::MissingExportName;
        outcome.diagnostic =
            "module " + cur.module + " has no export named " + cur.name;
        return outcome;
      }
    } else {
      if (static_cast<std::uint64_t>(cur.ordinal) < exports.ordinalBase) {
        outcome.status = BindingStatus::MissingExportOrdinal;
        outcome.diagnostic = "ordinal " + std::to_string(cur.ordinal) +
                             " is below the ordinal base of " + cur.module;
        return outcome;
      }

      std::uint64_t index64 = static_cast<std::uint64_t>(cur.ordinal) -
                              exports.ordinalBase;
      if (index64 >= exports.slots.size()) {
        outcome.status = BindingStatus::MissingExportOrdinal;
        outcome.diagnostic = "module " + cur.module +
                             " has no export ordinal " +
                             std::to_string(cur.ordinal);
        return outcome;
      }

      slot = &exports.slots[static_cast<std::size_t>(index64)];
    }

    if (slot->rva == 0) {
      outcome.status = BindingStatus::ZeroSlot;
      outcome.diagnostic =
          "module " + cur.module + " export " +
          (cur.kind == ImportNameKind::ByName
               ? cur.name
               : "#" + std::to_string(cur.ordinal)) +
          " (slot " + std::to_string(slot->slotIndex) + ") has a zero RVA";
      return outcome;
    }

    if (slot->forwarded) {
      if (!slot->forwardStringValid) {
        outcome.status = BindingStatus::BadForward;
        outcome.diagnostic =
            "forwarder string at RVA " + std::to_string(slot->forwardStringRva) +
            " is truncated before the export directory end RVA " +
            std::to_string(slot->forwardRangeEnd);
        return outcome;
      }

      ForwardTarget target;
      std::string why;
      if (!parseForwarderString(slot->forwardString, target, why)) {
        outcome.status = BindingStatus::BadForward;
        outcome.diagnostic = "invalid forwarder \"" + slot->forwardString +
                             "\" in " + cur.module + ": " + why;
        return outcome;
      }

      BindingHop hop;
      hop.kind = BindingHopKind::ExportForward;
      hop.sourceModule = cur.module;
      hop.targetModule = target.module;
      hop.exportSlot = slot->slotIndex;
      hop.exportOrdinal = slot->ordinal;
      hop.forwardString = slot->forwardString;
      hop.forwardStringRva = slot->forwardStringRva;
      hop.forwardRangeEnd = slot->forwardRangeEnd;
      row.hops.push_back(hop);

      importer = cur.module;
      cur.module = target.module;
      cur.kind = target.kind;
      cur.name = target.name;
      cur.ordinal = target.ordinal;
      continue;
    }

    if (!module->reader.isRvaMapped(slot->rva)) {
      outcome.status = BindingStatus::FinalRvaNotMapped;
      outcome.diagnostic = "final RVA " + std::to_string(slot->rva) + " of " +
                           cur.module + " is not mapped by any section";
      return outcome;
    }

    std::uint64_t va = 0;
    bool vaOk = addImageBase(module->reader.imageBaseValue(), slot->rva,
                             module->reader.maxAddress(), va);
    if (!vaOk || (!module->reader.isPe64() && va > ADDRESS_32_MAX)) {
      outcome.status = BindingStatus::AddressOverflow;
      outcome.diagnostic =
          "ImageBase + RVA overflows the address width for " + cur.module;
      return outcome;
    }

    row.result.providerModule = cur.module;
    row.result.exportSlot = slot->slotIndex;
    row.result.exportOrdinal = slot->ordinal;
    row.result.rva = slot->rva;
    row.result.va = va;

    outcome.status = BindingStatus::Resolved;
    outcome.diagnostic = "resolved through " + std::to_string(row.hops.size()) +
                         " hop(s)";
    outcome.ok = true;
    return outcome;
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Public structured tables
// ---------------------------------------------------------------------------

bool BuildStructuredImportTable(parsed_pe *pe,
                                StructuredImportTable &out,
                                std::string *error) {
  std::string localError;
  if (error != nullptr) {
    *error = std::string();
  }

  if (!parseImportTable(pe, out, localError)) {
    if (error != nullptr) {
      *error = localError;
    }
    return false;
  }

  return true;
}

bool BuildStructuredExportTable(parsed_pe *pe,
                                StructuredExportTable &out,
                                std::string *error) {
  std::string localError;
  if (error != nullptr) {
    *error = std::string();
  }

  if (!parseExportTable(pe, out, localError)) {
    if (error != nullptr) {
      *error = localError;
    }
    return false;
  }

  return true;
}

const char *BindingStatusName(BindingStatus status) {
  switch (status) {
    case BindingStatus::Resolved:
      return "resolved";
    case BindingStatus::ZeroSlot:
      return "zero_slot";
    case BindingStatus::MissingModule:
      return "missing_module";
    case BindingStatus::MissingExportName:
      return "missing_export_name";
    case BindingStatus::MissingExportOrdinal:
      return "missing_export_ordinal";
    case BindingStatus::BadForward:
      return "bad_forward";
    case BindingStatus::Cycle:
      return "cycle";
    case BindingStatus::FinalRvaNotMapped:
      return "final_rva_not_mapped";
    case BindingStatus::AddressOverflow:
      return "address_overflow";
    case BindingStatus::ApiSetMissingSchema:
      return "apiset_missing_schema";
    case BindingStatus::ApiSetUnsupportedVersion:
      return "apiset_unsupported_version";
    case BindingStatus::ApiSetMalformed:
      return "apiset_malformed";
    case BindingStatus::ApiSetUnsupportedEncoding:
      return "apiset_unsupported_encoding";
    case BindingStatus::ApiSetNotFound:
      return "apiset_not_found";
    case BindingStatus::ApiSetAmbiguous:
      return "apiset_ambiguous";
    case BindingStatus::ApiSetDisabled:
      return "apiset_disabled";
    case BindingStatus::ApiSetMissingHost:
      return "apiset_missing_host";
    case BindingStatus::ExportTableInvalid:
      return "export_table_invalid";
    case BindingStatus::ImportTableInvalid:
      return "import_table_invalid";
    default:
      return "unknown";
  }
}

bool BuildImportBindingPlan(const std::string &rootModuleName,
                            const std::vector<BindingModuleInput> &modules,
                            const std::uint8_t *apiSetNamespace,
                            std::uint32_t apiSetNamespaceSize,
                            ImportBindingPlan &plan,
                            std::string *error) {
  if (error != nullptr) {
    *error = std::string();
  }

  plan.rows_.clear();
  plan.diagnostics_.clear();

  BindingContext context;
  context.schema.parse(apiSetNamespace, apiSetNamespaceSize);

  for (const std::string &diag : context.schema.diagnostics) {
    plan.diagnostics_.push_back("api-set: " + diag);
  }

  std::string rootNormalized = normalizeModuleName(rootModuleName);
  if (rootNormalized.empty()) {
    if (error != nullptr) {
      *error = "root module name is empty";
    }
    return false;
  }

  for (std::size_t i = 0; i < modules.size(); ++i) {
    const BindingModuleInput &input = modules[i];
    if (input.pe == nullptr) {
      if (error != nullptr) {
        *error = "module entry has a null parsed_pe";
      }
      return false;
    }

    std::string normalized = normalizeModuleName(input.fileName);
    if (normalized.empty()) {
      if (error != nullptr) {
        *error = "module entry has an empty file name";
      }
      return false;
    }

    if (context.modules.find(normalized) != context.modules.end()) {
      if (error != nullptr) {
        *error = "duplicate normalized module name " + normalized;
      }
      return false;
    }

    BindingContext::ModuleEntry *entry =
        new (std::nothrow) BindingContext::ModuleEntry();
    if (entry == nullptr) {
      if (error != nullptr) {
        *error = "out of memory";
      }
      return false;
    }

    context.ownedModules.push_back(entry);
    entry->input = &input;

    std::string readerError;
    if (!entry->reader.init(input.pe, &readerError)) {
      entry->exportOk = false;
      entry->exportReadable = false;
      entry->exportError = readerError;
    } else {
      std::string exportError;
      if (!parseExportTable(input.pe, entry->exports, exportError)) {
        entry->exportOk = false;
        entry->exportReadable = false;
        entry->exportError = exportError;
      } else {
        entry->exportOk = true;
        entry->exportReadable = true;
      }
    }

    context.modules[normalized] = entry;
  }

  BindingContext::ModuleEntry *root = context.find(rootNormalized);
  if (root == nullptr) {
    if (error != nullptr) {
      *error = "root module " + rootNormalized +
               " is not in the supplied module set";
    }
    return false;
  }

  StructuredImportTable imports;
  std::string importError;
  if (!parseImportTable(root->input->pe, imports, importError)) {
    plan.diagnostics_.push_back("root import table unreadable: " + importError);
    return true;
  }

  for (const std::string &diag : imports.diagnostics) {
    plan.diagnostics_.push_back("imports: " + diag);
  }

  for (const StructuredImport &structured : imports.entries) {
    BindingRow row;
    row.request.moduleName = structured.moduleName;
    row.request.normalizedModule =
        normalizeModuleName(structured.moduleName);
    row.request.kind = structured.kind;
    row.request.symbolName = structured.symbolName;
    row.request.ordinal = structured.ordinal;
    row.request.iatRva = structured.iatRva;
    row.request.slotWidth = structured.slotWidth;
    row.request.thunkIndex = structured.thunkIndex;

    std::uint64_t iatVa = 0;
    bool iatOk = addImageBase(root->reader.imageBaseValue(),
                              structured.iatRva,
                              root->reader.maxAddress(), iatVa);
    row.request.iatVa = iatVa;
    row.request.iatVaOverflow =
        !iatOk || (!root->reader.isPe64() && iatVa > ADDRESS_32_MAX);

    SymbolState start;
    start.module = row.request.normalizedModule;
    start.kind = structured.kind;
    start.name = structured.kind == ImportNameKind::ByName
                     ? structured.symbolName
                     : std::string();
    start.ordinal = structured.ordinal;

    if (start.module.empty()) {
      row.status = BindingStatus::MissingModule;
      row.diagnostic = "import module name normalizes to empty";
      plan.rows_.push_back(row);
      continue;
    }

    ResolveOutcome outcome = resolveSymbol(context, rootNormalized, start, row);
    row.status = outcome.status;
    row.diagnostic = outcome.diagnostic;
    plan.rows_.push_back(row);
  }

  // Stable ordering by the root module's original IAT RVA.
  std::stable_sort(plan.rows_.begin(),
                   plan.rows_.end(),
                   [](const BindingRow &a, const BindingRow &b) {
                     return a.request.iatRva < b.request.iatRva;
                   });

  return true;
}

} // namespace peparse
