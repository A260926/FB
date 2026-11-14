//
// Copyright (c) 2021-present, Trail of Bits, Inc.
// All rights reserved.
//
// This source code is licensed in accordance with the terms specified in
// the LICENSE file found in the root directory of this source tree.
//

#pragma once

#include <btfparse/ibtf.h>

#include <cstdint>
#include <string>
#include <vector>

namespace btfparse {

// One member of a struct/union as it exists in a specific module view.
//
// Offsets are taken from BTF (bit offsets). `raw_bytes` is the slice of the
// caller-supplied synthetic object covered by this member. Pointers are never
// dereferenced: `pointer_value` is the value stored in the bytes, decoded in
// host byte order.
struct BTFInspectedMember final {
  bool has_name{false};
  std::string name;

  // ID of the member's type in the inspected view, plus its provenance.
  std::uint32_t type_id{0};
  BTFSourceID type_source{0};
  std::uint32_t type_depth{0};
  std::string type_name;

  std::uint32_t offset_bits{0};
  std::uint32_t size_bits{0};

  bool is_pointer{false};
  std::uint64_t pointer_value{0};

  bool is_bitfield{false};
  bool incomplete{false};

  // Empty when the member cannot be sliced unambiguously (bitfield or an
  // incomplete/unknown type).
  std::string raw_bytes;
};

// Result of inspecting one module-local type against a synthetic object.
//
// `explainable` is false exactly when at least one member cannot be laid out:
// unsupported bitfields and incomplete (forward or void) by-value types. The
// object is still described (size, provenance, member list) so the caller can
// record why it refused to interpret it.
struct BTFTypeInspection final {
  bool explainable{false};
  std::string unexplainable_reason;

  std::string module_name;
  std::uint32_t requested_id{0};
  std::uint32_t resolved_id{0};

  BTFSourceID source{0};
  std::uint32_t depth{0};

  BTFKind kind{BTFKind::Void};
  std::string type_name;
  std::uint32_t size{0};

  std::vector<BTFInspectedMember> members;

  std::string raw_bytes;

  // Stable, line-oriented rendering used by the example and by forensic
  // records. One header line per field, then one line per member.
  std::string toText() const;
};

// Interprets `object_bytes` as an instance of module-local type
// `local_type_id`, resolved through `view` (a split view for one module).
// `module_name` is recorded verbatim for provenance.
//
// The view identity is authoritative: two modules whose ID 4 is each named
// `node` are never merged by name. Submitting bytes whose length does not
// equal this view's struct size fails with an explicit size-mismatch error
// instead of guessing another layout.
Result<BTFTypeInspection, BTFError>
inspectSplitType(const IBTF &view, const std::string &module_name,
                 std::uint32_t local_type_id,
                 const std::vector<std::uint8_t> &object_bytes) noexcept;

} // namespace btfparse
