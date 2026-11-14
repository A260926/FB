/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if HAVE_STRCASECMP
#include <strings.h>
#endif

#include "wabt/binary-reader-objdump.h"
#include "wabt/binary-reader.h"
#include "wabt/binary-reference-map.h"
#include "wabt/common.h"
#include "wabt/error-formatter.h"
#include "wabt/option-parser.h"
#include "wabt/stream.h"
#include "wabt/string-format.h"

using namespace wabt;

static const char s_description[] =
    R"(  Print information about the contents of wasm binaries.

examples:
  $ wasm-objdump test.wasm
)";

static ObjdumpOptions s_objdump_options;

static std::vector<const char*> s_infiles;

static std::unique_ptr<FileStream> s_log_stream;

static void ParseOptions(int argc, char** argv) {
  OptionParser parser("wasm-objdump", s_description);

  parser.AddOption('h', "headers", "Print headers",
                   []() { s_objdump_options.headers = true; });
  parser.AddOption(
      'j', "section", "SECTION", "Select just one section",
      [](const char* argument) { s_objdump_options.section_name = argument; });
  parser.AddOption('s', "full-contents", "Print raw section contents",
                   []() { s_objdump_options.raw = true; });
  parser.AddOption('d', "disassemble", "Disassemble function bodies",
                   []() { s_objdump_options.disassemble = true; });
  parser.AddOption("debug", "Print extra debug information", []() {
    s_objdump_options.debug = true;
    s_log_stream = FileStream::CreateStderr();
    s_objdump_options.log_stream = s_log_stream.get();
  });
  parser.AddOption('x', "details", "Show section details",
                   []() { s_objdump_options.details = true; });
  parser.AddOption('r', "reloc", "Show relocations inline with disassembly",
                   []() { s_objdump_options.relocs = true; });
  parser.AddOption(0, "section-offsets",
                   "Print section offsets instead of file offsets "
                   "in code disassembly",
                   []() { s_objdump_options.section_offsets = true; });
  parser.AddOption(0, "reference-map",
                   "Print the read-only reference index: every supported "
                   "relocation with its source field bytes and typed target "
                   "identity",
                   []() { s_objdump_options.reference_map = true; });
  parser.AddArgument(
      "filename", OptionParser::ArgumentCount::OneOrMore,
      [](const char* argument) { s_infiles.push_back(argument); });

  parser.Parse(argc, argv);
}

static std::string SectionDisplayName(const ReferenceSection& section) {
  if (section.section_type == BinarySection::Custom) {
    return section.name;
  }
  return GetSectionName(section.section_type);
}

static std::string HexBytes(const std::vector<uint8_t>& encoding) {
  std::string result;
  for (size_t i = 0; i < encoding.size(); ++i) {
    if (i) {
      result += ' ';
    }
    result += StringPrintf("%02x", encoding[i]);
  }
  return result;
}

static std::string FormatSource(const ReferenceSource& source) {
  switch (source.kind) {
    case ReferenceSource::Kind::FunctionInstruction:
      return StringPrintf("func[%" PRIindex "] instruction@0x%" PRIzx,
                          source.function_index, source.instruction_offset);
    case ReferenceSource::Kind::DataSegment:
      return StringPrintf("data[%" PRIindex "]+0x%" PRIzx,
                          source.data_segment, source.data_segment_offset);
    case ReferenceSource::Kind::CustomContent:
      return StringPrintf("custom[%" PRIindex "]+0x%" PRIzx,
                          source.custom_section_index,
                          source.custom_content_offset);
  }
  return "?";
}

static std::string FormatTarget(const ReferenceTarget& target) {
  switch (target.kind) {
    case ReferenceTarget::Kind::Type:
      return StringPrintf("type[%" PRIindex "]", target.index);
    case ReferenceTarget::Kind::Function:
    case ReferenceTarget::Kind::Global:
    case ReferenceTarget::Kind::Table: {
      const char* kind = target.kind == ReferenceTarget::Kind::Function
                             ? "func"
                         : target.kind == ReferenceTarget::Kind::Global
                             ? "global"
                             : "table";
      std::string text = StringPrintf(
          "%s symbol[%" PRIindex "] index[%" PRIindex "]", kind,
          target.symbol_index, target.index);
      if (target.imported) {
        text += StringPrintf(" import<%s.%s>", target.import_module.c_str(),
                             target.import_field.c_str());
      }
      return text;
    }
    case ReferenceTarget::Kind::Data: {
      std::string text =
          StringPrintf("data symbol[%" PRIindex "]", target.symbol_index);
      if (target.defined) {
        text += StringPrintf(" segment[%" PRIindex "]+0x%x size=%u",
                             target.data_segment, target.data_offset,
                             target.data_size);
      } else {
        text += " external undefined";
      }
      return text;
    }
    case ReferenceTarget::Kind::Section:
      return StringPrintf("section symbol[%" PRIindex "] section[%" PRIindex "]",
                          target.symbol_index, target.index);
  }
  return "?";
}

static Result PrintReferenceMap(const BinaryReferenceMap& map,
                                const char* section_filter) {
  if (section_filter) {
    // Match the historical -j behavior: an unknown section name is an error
    // even when it would simply hide every reference line.
    bool section_found = false;
    for (const ReferenceSection& section : map.sections) {
      if (strcasecmp(section_filter,
                     SectionDisplayName(section).c_str()) == 0) {
        section_found = true;
        break;
      }
    }
    if (!section_found) {
      fprintf(stderr, "Section not found: %s\n", section_filter);
      return Result::Error;
    }
  }

  printf("\nReference Map:\n\n");
  for (const ReferenceRecord& record : map.records) {
    const ReferenceSection* section =
        map.GetSection(record.target_section_index);
    if (section_filter && section) {
      std::string name = SectionDisplayName(*section);
      if (strcasecmp(section_filter, name.c_str()) != 0) {
        continue;
      }
    }

    printf("reloc.section[%" PRIindex "] entry[%" PRIindex "] -> section[%"
           PRIindex "] field=%s section_offset=0x%" PRIzx
           " file=0x%" PRIzx "..0x%" PRIzx " bytes=%s addend=%d source=%s",
           record.reloc_section_index, record.reloc_entry_index,
           record.target_section_index, GetRelocTypeName(record.type),
           record.section_offset, record.file_begin, record.file_end,
           HexBytes(record.encoding).c_str(), record.addend,
           FormatSource(record.source).c_str());
    for (const ReferenceTarget& target : record.targets) {
      printf(" -> %s", FormatTarget(target).c_str());
    }
    printf("\n");
  }
  return Result::Ok;
}

Result dump_file(const char* filename) {
  std::vector<uint8_t> file_data;
  CHECK_RESULT(ReadFile(filename, &file_data));

  // Perform serveral passed over the binary in order to print out different
  // types of information.
  s_objdump_options.filename = filename;
  printf("\n");

  ObjdumpState state;

  Result result = Result::Ok;

  // Reference map pass: build the complete read-only index before anything
  // is printed for it, so a validation failure never emits a partial report.
  BinaryReferenceMap reference_map;
  if (s_objdump_options.reference_map) {
    Errors errors;
    Features features;
    features.EnableAll();
    ReadBinaryOptions read_options(features, s_objdump_options.log_stream,
                                   /*read_debug_names=*/true,
                                   /*stop_on_first_error=*/true,
                                   /*fail_on_custom_section_error=*/true);
    if (Failed(ReadBinaryReferenceMap(file_data, read_options, &errors,
                                      &reference_map))) {
      FormatErrorsToFile(errors, Location::Type::Binary);
      return Result::Error;
    }
  }

  // Pass 0: Prepass
  s_objdump_options.mode = ObjdumpMode::Prepass;
  result |= ReadBinaryObjdump(file_data, &s_objdump_options, &state);
  s_objdump_options.log_stream = nullptr;

  // Pass 1: Print the section headers
  if (s_objdump_options.headers) {
    s_objdump_options.mode = ObjdumpMode::Headers;
    result |= ReadBinaryObjdump(file_data, &s_objdump_options, &state);
  }

  // Pass 2: Print extra information based on section type
  if (s_objdump_options.details) {
    s_objdump_options.mode = ObjdumpMode::Details;
    result |= ReadBinaryObjdump(file_data, &s_objdump_options, &state);
  }

  // Pass 3: Disassemble code section
  if (s_objdump_options.disassemble) {
    s_objdump_options.mode = ObjdumpMode::Disassemble;
    result |= ReadBinaryObjdump(file_data, &s_objdump_options, &state);
  }

  // Pass 4: Dump to raw contents of the sections
  if (s_objdump_options.raw) {
    s_objdump_options.mode = ObjdumpMode::RawData;
    result |= ReadBinaryObjdump(file_data, &s_objdump_options, &state);
  }

  // Pass 5: Print the read-only reference index.
  if (s_objdump_options.reference_map && Succeeded(result)) {
    result |= PrintReferenceMap(reference_map,
                                s_objdump_options.section_name);
  }

  return result;
}

int ProgramMain(int argc, char** argv) {
  InitStdio();

  ParseOptions(argc, argv);
  if (!s_objdump_options.headers && !s_objdump_options.details &&
      !s_objdump_options.disassemble && !s_objdump_options.raw &&
      !s_objdump_options.reference_map) {
    fprintf(stderr, "At least one of the following switches must be given:\n");
    fprintf(stderr, " -d/--disassemble\n");
    fprintf(stderr, " -h/--headers\n");
    fprintf(stderr, " -x/--details\n");
    fprintf(stderr, " -s/--full-contents\n");
    fprintf(stderr, " --reference-map\n");
    return 1;
  }

  for (const char* filename : s_infiles) {
    if (Failed(dump_file(filename))) {
      return 1;
    }
  }

  return 0;
}

int main(int argc, char** argv) {
  WABT_TRY
  return ProgramMain(argc, argv);
  WABT_CATCH_BAD_ALLOC_AND_EXIT
}
