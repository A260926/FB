//
// Copyright (c) 2021-present, Trail of Bits, Inc.
// All rights reserved.
//
// This source code is licensed in accordance with the terms specified in
// the LICENSE file found in the root directory of this source tree.
//

#pragma once

#include <btfparse/ibtf.h>

#include <string>
#include <utility>
#include <vector>

namespace btfparse {

class IBTFHeaderGenerator {
public:
  using Ptr = std::unique_ptr<IBTFHeaderGenerator>;
  static Ptr create();

  IBTFHeaderGenerator() = default;
  virtual ~IBTFHeaderGenerator() = default;

  virtual bool generate(std::string &header, const IBTF::Ptr &btf) const = 0;

  // Generates a single header holding the union of the given module views.
  // Modules are emitted in the supplied order. Types inherited from a
  // shared base are declared once; independently parsed bases are never
  // merged. Every named tag, fwd and typedef is prefixed with
  // "<module>__", enum members are prefixed with "<module>__<enum>__",
  // and fundamental integer/float spelling is left untouched. The header
  // is only assigned on success; on any failure it is cleared and the
  // call returns false.
  virtual bool
  generateModules(std::string &header,
                  const std::vector<std::pair<std::string, const IBTF *>>
                      &modules) const = 0;

  IBTFHeaderGenerator(const IBTFHeaderGenerator &) = delete;
  IBTFHeaderGenerator &operator=(const IBTFHeaderGenerator &) = delete;
};

} // namespace btfparse
