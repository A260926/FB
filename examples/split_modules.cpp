//
// Copyright (c) 2021-present, Trail of Bits, Inc.
// All rights reserved.
//
// This source code is licensed in accordance with the terms specified in
// the LICENSE file found in the root directory of this source tree.
//

// Generates one C header that declares a shared BTF base plus both sibling
// split layers, then optionally interprets a synthetic object using the
// production split views.
//
// Usage:
//   split_modules <base.btf> <left.btf> <right.btf>
//
// The three-path form prints the union declaration to stdout. The forensic
// form additionally accepts a module name, a local type ID and a hex-encoded
// synthetic object:
//
//   split_modules <base.btf> <left.btf> <right.btf> \
//       <module> <local-id> <hex-bytes>
//
// Only files on disk are read. No kernel module is loaded and no image is
// executed; pointer members are reported as their stored value and are never
// followed.

#include <btfparse/ibtf.h>
#include <btfparse/ibtfheadergenerator.h>
#include <btfparse/ibtfinspector.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> parseHex(const std::string &text,
                                   bool &ok) {
  ok = true;

  std::vector<std::uint8_t> bytes;
  std::string normalized;

  for (auto ch : text) {
    if (ch == ':' || ch == '-' || ch == '_') {
      continue;
    }
    normalized.push_back(ch);
  }

  if (normalized.size() % 2 != 0) {
    ok = false;
    return bytes;
  }

  for (std::size_t i = 0; i < normalized.size(); i += 2) {
    auto is_hex = [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
             (c >= 'A' && c <= 'F');
    };

    if (!is_hex(normalized[i]) || !is_hex(normalized[i + 1])) {
      ok = false;
      return bytes;
    }

    auto hex_value = [](char c) -> unsigned {
      if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
      }
      if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a' + 10);
      }
      return static_cast<unsigned>(c - 'A' + 10);
    };

    bytes.push_back(static_cast<std::uint8_t>(hex_value(normalized[i]) * 16 +
                                              hex_value(normalized[i + 1])));
  }

  return bytes;
}

std::uint32_t parseTypeID(const std::string &text, bool &ok) {
  ok = true;

  try {
    std::size_t consumed{0};
    auto value = std::stoul(text, &consumed);
    if (consumed != text.size()) {
      ok = false;
      return 0;
    }
    return static_cast<std::uint32_t>(value);

  } catch (...) {
    ok = false;
    return 0;
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 4 && argc != 7) {
    std::cerr << "usage: " << argv[0]
              << " <base.btf> <left.btf> <right.btf>"
                 " [<module> <local-type-id> <hex-bytes>]\n";
    return 2;
  }

  auto base_result = btfparse::IBTF::createFromPath(argv[1]);
  if (base_result.failed()) {
    std::cerr << "base: " << base_result.takeError().toString() << "\n";
    return 1;
  }

  auto base = base_result.takeValue();

  auto left_result = btfparse::IBTF::createSplitFromPath(*base, argv[2]);
  if (left_result.failed()) {
    std::cerr << "left: " << left_result.takeError().toString() << "\n";
    return 1;
  }

  auto left = left_result.takeValue();

  auto right_result = btfparse::IBTF::createSplitFromPath(*base, argv[3]);
  if (right_result.failed()) {
    std::cerr << "right: " << right_result.takeError().toString() << "\n";
    return 1;
  }

  auto right = right_result.takeValue();

  auto generator = btfparse::IBTFHeaderGenerator::create();
  if (generator == nullptr) {
    std::cerr << "failed to create the header generator\n";
    return 1;
  }

  std::string header;
  std::vector<std::pair<std::string, const btfparse::IBTF *>> modules{
      {"base", base.get()},
      {"left", left.get()},
      {"right", right.get()},
  };

  if (!generator->generateModules(header, modules) || header.empty()) {
    std::cerr << "failed to generate the union declaration\n";
    return 1;
  }

  if (argc == 4) {
    std::cout << header;
    return 0;
  }

  // Forensic mode: resolve the requested module by name, inspect the type
  // through that module's own split view, and print the record.
  const std::string module_name = argv[4];

  const btfparse::IBTF *selected_view{nullptr};
  for (const auto &module : modules) {
    if (module.first == module_name) {
      selected_view = module.second;
      break;
    }
  }

  if (selected_view == nullptr) {
    std::cerr << "unknown module '" << module_name
              << "' (expected base, left or right)\n";
    return 1;
  }

  bool id_ok{false};
  auto local_type_id = parseTypeID(argv[5], id_ok);
  if (!id_ok) {
    std::cerr << "invalid local type id '" << argv[5] << "'\n";
    return 1;
  }

  bool bytes_ok{false};
  auto object_bytes = parseHex(argv[6], bytes_ok);
  if (!bytes_ok) {
    std::cerr << "invalid hex object bytes\n";
    return 1;
  }

  auto inspection_result = btfparse::inspectSplitType(
      *selected_view, module_name, local_type_id, object_bytes);

  if (inspection_result.failed()) {
    std::cerr << "inspection: "
              << inspection_result.takeError().toString() << "\n";
    return 1;
  }

  auto inspection = inspection_result.takeValue();
  std::cout << inspection.toText();
  return 0;
}
