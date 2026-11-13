// Example: build an offline import binding plan for a root module and its
// provider modules.
//
// The program reads PE files named on the command line into memory, parses
// them with ParsePEFromPointer and builds a plan.  It never loads a DLL,
// runs an entry point, consults the system directory/registry or otherwise
// touches the host beyond the files explicitly listed.
//
// Usage:
//   import_binding_plan [--apiset <namespace.bin>] \
//                       <root.dll> <provider.dll> [provider.dll ...]
//
// Module names are only used as basenames, matched ASCII case-insensitively.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <pe-parse/import_binding.h>
#include <pe-parse/parse.h>

namespace {

struct InputFile {
  std::string name;
  std::vector<std::uint8_t> bytes;
};

bool ReadFile(const std::string &path, std::vector<std::uint8_t> &out) {
  std::ifstream stream(path.c_str(), std::ios::binary);
  if (!stream) {
    return false;
  }

  stream.seekg(0, std::ios::end);
  std::streamoff size = stream.tellg();
  if (size < 0) {
    return false;
  }
  stream.seekg(0, std::ios::beg);

  out.assign(static_cast<std::size_t>(size), 0);
  if (size > 0) {
    stream.read(reinterpret_cast<char *>(out.data()),
                static_cast<std::streamsize>(size));
    if (!stream) {
      return false;
    }
  }

  return true;
}

std::string BaseName(const std::string &path) {
  std::size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

void PrintJsonString(const std::string &s) {
  std::printf("\"");
  for (std::size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"':
        std::printf("\\\"");
        break;
      case '\\':
        std::printf("\\\\");
        break;
      case '\n':
        std::printf("\\n");
        break;
      case '\r':
        std::printf("\\r");
        break;
      case '\t':
        std::printf("\\t");
        break;
      default:
        if (c < 0x20) {
          std::printf("\\u%04x", static_cast<unsigned>(c));
        } else {
          std::printf("%c", static_cast<char>(c));
        }
    }
  }
  std::printf("\"");
}

void PrintPlan(const peparse::ImportBindingPlan &plan) {
  std::printf("{\"rows\":[");
  for (std::size_t i = 0; i < plan.Rows().size(); ++i) {
    const peparse::BindingRow &row = plan.Rows()[i];
    if (i != 0) {
      std::printf(",");
    }
    std::printf("\n  {\"status\":");
    PrintJsonString(peparse::BindingStatusName(row.status));
    std::printf(",\"module\":");
    PrintJsonString(row.request.moduleName);
    std::printf(",\"symbol\":");
    if (row.request.kind == peparse::ImportNameKind::ByName) {
      PrintJsonString(row.request.symbolName);
    } else {
      std::printf("\"#%u\"",
                  static_cast<unsigned>(row.request.ordinal));
    }
    std::printf(",\"iat_rva\":%u",
                static_cast<unsigned>(row.request.iatRva));
    std::printf(",\"iat_va\":%llu",
                static_cast<unsigned long long>(row.request.iatVa));
    if (row.status == peparse::BindingStatus::Resolved) {
      std::printf(",\"provider\":");
      PrintJsonString(row.result.providerModule);
      std::printf(",\"export_slot\":%u,\"export_ordinal\":%u,\"rva\":%u",
                  static_cast<unsigned>(row.result.exportSlot),
                  static_cast<unsigned>(row.result.exportOrdinal),
                  static_cast<unsigned>(row.result.rva));
      std::printf(",\"va\":%llu",
                  static_cast<unsigned long long>(row.result.va));
    }
    std::printf(",\"hops\":[");
    for (std::size_t h = 0; h < row.hops.size(); ++h) {
      const peparse::BindingHop &hop = row.hops[h];
      if (h != 0) {
        std::printf(",");
      }
      std::printf("{\"source\":");
      PrintJsonString(hop.sourceModule);
      std::printf(",\"target\":");
      PrintJsonString(hop.targetModule);
      if (hop.kind == peparse::BindingHopKind::ExportForward) {
        std::printf(",\"slot\":%u,\"forward\":",
                    static_cast<unsigned>(hop.exportSlot));
        PrintJsonString(hop.forwardString);
      } else {
        std::printf(",\"contract\":");
        PrintJsonString(hop.contractName);
        std::printf(",\"entry_offset\":%u,\"value_offset\":%u",
                    static_cast<unsigned>(hop.schemaEntryOffset),
                    static_cast<unsigned>(hop.schemaValueOffset));
      }
      std::printf("}");
    }
    std::printf("],\"diagnostic\":");
    PrintJsonString(row.diagnostic);
    std::printf("}");
  }
  std::printf("\n],\"diagnostics\":[");
  for (std::size_t i = 0; i < plan.Diagnostics().size(); ++i) {
    if (i != 0) {
      std::printf(",");
    }
    PrintJsonString(plan.Diagnostics()[i]);
  }
  std::printf("]}\n");
}

void Usage(const char *argv0) {
  std::fprintf(stderr,
               "usage: %s [--apiset <namespace.bin>] "
               "<root.dll> <provider.dll> [provider.dll ...]\n",
               argv0);
}

} // namespace

int main(int argc, char **argv) {
  std::string apiSetPath;
  std::vector<std::string> modulePaths;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--apiset") {
      if (i + 1 >= argc) {
        Usage(argv[0]);
        return 2;
      }
      apiSetPath = argv[++i];
    } else {
      modulePaths.push_back(arg);
    }
  }

  if (modulePaths.size() < 2) {
    Usage(argv[0]);
    return 2;
  }

  std::vector<InputFile> files(modulePaths.size());
  for (std::size_t i = 0; i < modulePaths.size(); ++i) {
    files[i].name = BaseName(modulePaths[i]);
    if (!ReadFile(modulePaths[i], files[i].bytes)) {
      std::fprintf(stderr, "cannot read %s\n", modulePaths[i].c_str());
      return 1;
    }
  }

  std::vector<std::uint8_t> apiSetBytes;
  if (!apiSetPath.empty() && !ReadFile(apiSetPath, apiSetBytes)) {
    std::fprintf(stderr, "cannot read %s\n", apiSetPath.c_str());
    return 1;
  }

  std::vector<peparse::parsed_pe *> parsed(files.size(), nullptr);
  for (std::size_t i = 0; i < files.size(); ++i) {
    parsed[i] = peparse::ParsePEFromPointer(
        files[i].bytes.data(),
        static_cast<std::uint32_t>(files[i].bytes.size()));
    if (parsed[i] == nullptr) {
      std::fprintf(stderr, "cannot parse %s: %s\n", files[i].name.c_str(),
                   peparse::GetPEErrString().c_str());
      for (std::size_t j = 0; j < i; ++j) {
        peparse::DestructParsedPE(parsed[j]);
      }
      return 1;
    }
  }

  std::vector<peparse::BindingModuleInput> modules;
  for (std::size_t i = 0; i < files.size(); ++i) {
    peparse::BindingModuleInput input;
    input.fileName = files[i].name;
    input.pe = parsed[i];
    modules.push_back(input);
  }

  peparse::ImportBindingPlan plan;
  std::string error;
  bool ok = peparse::BuildImportBindingPlan(
      files[0].name,
      modules,
      apiSetBytes.empty() ? nullptr : apiSetBytes.data(),
      static_cast<std::uint32_t>(apiSetBytes.size()),
      plan,
      &error);

  for (std::size_t i = 0; i < parsed.size(); ++i) {
    peparse::DestructParsedPE(parsed[i]);
  }

  if (!ok) {
    std::fprintf(stderr, "cannot build plan: %s\n", error.c_str());
    return 1;
  }

  PrintPlan(plan);
  return 0;
}
