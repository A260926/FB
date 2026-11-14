//
// Copyright (c) 2021-present, Trail of Bits, Inc.
// All rights reserved.
//
// This source code is licensed in accordance with the terms specified in
// the LICENSE file found in the root directory of this source tree.
//

#include <btfparse/ibtfinspector.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace btfparse {

namespace {

using Bytes = std::vector<std::uint8_t>;

std::string hexEncode(const std::uint8_t *data, std::size_t size) {
  static const char *digits = "0123456789abcdef";

  std::string output;
  output.reserve(size * 2);

  for (std::size_t i = 0; i < size; ++i) {
    output.push_back(digits[data[i] >> 4]);
    output.push_back(digits[data[i] & 0x0F]);
  }

  return output;
}

std::string hexEncode(const std::vector<std::uint8_t> &bytes) {
  return hexEncode(bytes.data(), bytes.size());
}

// Resolves the size, in bits, of a type as laid out by value. Returns
// nullopt for types that cannot be embedded by value (incomplete tags,
// void, function prototypes, ...). Pointers use the declared BTF pointer
// width, which the header generator also assumes.
std::optional<std::uint32_t> getTypeSizeBits(const IBTF &view,
                                             std::uint32_t id,
                                             std::uint32_t depth = 0) {
  if (id == 0 || depth > 64) {
    return std::nullopt;
  }

  auto opt_type = view.getType(id);
  if (!opt_type.has_value()) {
    return std::nullopt;
  }

  switch (IBTF::getBTFTypeKind(*opt_type)) {
  case BTFKind::Int:
    return std::get<IntBTFType>(*opt_type).size * 8;

  case BTFKind::Float:
    return std::get<FloatBTFType>(*opt_type).size * 8;

  case BTFKind::Enum:
    return std::get<EnumBTFType>(*opt_type).size * 8;

  case BTFKind::Struct:
    return std::get<StructBTFType>(*opt_type).size * 8;

  case BTFKind::Union:
    return std::get<UnionBTFType>(*opt_type).size * 8;

  case BTFKind::Ptr:
    return sizeof(void *) * 8;

  case BTFKind::Array: {
    const auto &array_type = std::get<ArrayBTFType>(*opt_type);
    auto opt_elem = getTypeSizeBits(view, array_type.type, depth + 1);
    if (!opt_elem.has_value()) {
      return std::nullopt;
    }
    return opt_elem.value() * array_type.nelems;
  }

  case BTFKind::Typedef:
    return getTypeSizeBits(view, std::get<TypedefBTFType>(*opt_type).type,
                           depth + 1);

  case BTFKind::Volatile:
    return getTypeSizeBits(view, std::get<VolatileBTFType>(*opt_type).type,
                           depth + 1);

  case BTFKind::Const:
    return getTypeSizeBits(view, std::get<ConstBTFType>(*opt_type).type,
                           depth + 1);

  case BTFKind::Restrict:
    return getTypeSizeBits(view, std::get<RestrictBTFType>(*opt_type).type,
                           depth + 1);

  case BTFKind::Fwd:
  case BTFKind::Void:
  case BTFKind::Func:
  case BTFKind::FuncProto:
  case BTFKind::Var:
  case BTFKind::DataSec:
    return std::nullopt;
  }

  return std::nullopt;
}

std::string getTypeDisplayName(const IBTF &view, std::uint32_t id,
                               std::uint32_t depth = 0) {
  if (depth > 32) {
    return "<?" ">";
  }

  auto opt_type = view.getType(id);
  if (!opt_type.has_value()) {
    return "<missing>";
  }

  switch (IBTF::getBTFTypeKind(*opt_type)) {
  case BTFKind::Struct: {
    const auto &t = std::get<StructBTFType>(*opt_type);
    return t.opt_name.has_value() ? "struct " + t.opt_name.value()
                                  : std::string("<anonymous struct>");
  }

  case BTFKind::Union: {
    const auto &t = std::get<UnionBTFType>(*opt_type);
    return t.opt_name.has_value() ? "union " + t.opt_name.value()
                                  : std::string("<anonymous union>");
  }

  case BTFKind::Enum: {
    const auto &t = std::get<EnumBTFType>(*opt_type);
    return t.opt_name.has_value() ? "enum " + t.opt_name.value()
                                  : std::string("<anonymous enum>");
  }

  case BTFKind::Fwd: {
    const auto &t = std::get<FwdBTFType>(*opt_type);
    return (t.is_union ? "union " : "struct ") + t.name;
  }

  case BTFKind::Typedef:
    return std::get<TypedefBTFType>(*opt_type).name;

  case BTFKind::Int:
    return std::get<IntBTFType>(*opt_type).name;

  case BTFKind::Float:
    return std::get<FloatBTFType>(*opt_type).name;

  case BTFKind::Void:
    return "void";

  case BTFKind::Ptr: {
    const auto &t = std::get<PtrBTFType>(*opt_type);
    return getTypeDisplayName(view, t.type, depth + 1) + " *";
  }

  case BTFKind::Array: {
    const auto &t = std::get<ArrayBTFType>(*opt_type);
    return getTypeDisplayName(view, t.type, depth + 1) + "[" +
           std::to_string(t.nelems) + "]";
  }

  case BTFKind::Const:
    return "const " + getTypeDisplayName(
                         view, std::get<ConstBTFType>(*opt_type).type,
                         depth + 1);

  case BTFKind::Volatile:
    return "volatile " + getTypeDisplayName(
                             view, std::get<VolatileBTFType>(*opt_type).type,
                             depth + 1);

  case BTFKind::Restrict:
    return "restrict " + getTypeDisplayName(
                             view, std::get<RestrictBTFType>(*opt_type).type,
                             depth + 1);

  case BTFKind::Func:
    return "function " + std::get<FuncBTFType>(*opt_type).name;

  case BTFKind::FuncProto:
    return "<function prototype>";

  case BTFKind::Var:
    return std::get<VarBTFType>(*opt_type).name;

  case BTFKind::DataSec:
    return "<data section>";
  }

  return "<unknown>";
}

// Follows typedef/qualifier wrappers to the underlying type. Used only to
// decide whether a member is a pointer or where the by-value layout ends;
// never used to infer identity.
std::uint32_t stripQualifiers(const IBTF &view, std::uint32_t id,
                              std::uint32_t depth = 0) {
  if (depth > 32) {
    return id;
  }

  auto opt_type = view.getType(id);
  if (!opt_type.has_value()) {
    return id;
  }

  switch (IBTF::getBTFTypeKind(*opt_type)) {
  case BTFKind::Typedef:
    return stripQualifiers(view, std::get<TypedefBTFType>(*opt_type).type,
                           depth + 1);
  case BTFKind::Volatile:
    return stripQualifiers(view, std::get<VolatileBTFType>(*opt_type).type,
                           depth + 1);
  case BTFKind::Const:
    return stripQualifiers(view, std::get<ConstBTFType>(*opt_type).type,
                           depth + 1);
  case BTFKind::Restrict:
    return stripQualifiers(view, std::get<RestrictBTFType>(*opt_type).type,
                           depth + 1);
  default:
    return id;
  }
}

std::uint64_t decodePointerValue(const std::uint8_t *data,
                                 std::size_t available) {
  std::uint64_t value{0};
  auto count = std::min<std::size_t>(available, sizeof(std::uint64_t));

  // Synthetic objects are little-endian byte streams; decode explicitly so
  // the rendered value does not depend on the host.
  for (std::size_t i = 0; i < count; ++i) {
    value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
  }

  return value;
}

} // namespace

std::string BTFTypeInspection::toText() const {
  std::stringstream buffer;

  buffer << "module=" << module_name << " id=" << requested_id
         << " resolved_id=" << resolved_id << " source=" << source
         << " depth=" << depth << " kind=" << static_cast<int>(kind)
         << " name=" << (type_name.empty() ? "<anonymous>" : type_name)
         << " size=" << size << " explainable=" << (explainable ? 1 : 0)
         << "\n";

  if (!explainable) {
    buffer << "unexplainable=" << unexplainable_reason << "\n";
  }

  if (!raw_bytes.empty()) {
    buffer << "bytes=" << raw_bytes << "\n";
  }

  for (const auto &member : members) {
    buffer << "member=" << (member.has_name ? member.name : std::string("<anonymous>"))
           << " type_id=" << member.type_id
           << " type_source=" << member.type_source
           << " type_depth=" << member.type_depth
           << " type=" << member.type_name
           << " offset=" << (member.offset_bits / 8)
           << " offset_bits=" << member.offset_bits
           << " size_bits=" << member.size_bits;

    if (member.is_bitfield) {
      buffer << " bitfield=1";
    }

    if (member.is_pointer) {
      buffer << " pointer=1 ptr_value=0x" << std::hex << member.pointer_value
             << std::dec;
    }

    if (member.incomplete) {
      buffer << " incomplete=1";
    }

    buffer << " raw="
           << (member.raw_bytes.empty() ? std::string("<none>")
                                        : member.raw_bytes)
           << "\n";
  }

  return buffer.str();
}

Result<BTFTypeInspection, BTFError>
inspectSplitType(const IBTF &view, const std::string &module_name,
                 std::uint32_t local_type_id,
                 const std::vector<std::uint8_t> &object_bytes) noexcept {
  try {
    if (module_name.empty()) {
      return BTFError(BTFErrorInformation{
          BTFErrorInformation::Code::InvalidModuleName, std::nullopt});
    }

    if (view.getOriginToken() == 0) {
      return BTFError(BTFErrorInformation{
          BTFErrorInformation::Code::InvalidModule, std::nullopt});
    }

    auto opt_type = view.getType(local_type_id);
    if (!opt_type.has_value()) {
      return BTFError(BTFErrorInformation{
          BTFErrorInformation::Code::InvalidSplitTypeReference,
          BTFErrorInformation::FileRange{local_type_id, 0},
      });
    }

    auto kind = IBTF::getBTFTypeKind(*opt_type);
    if (kind != BTFKind::Struct && kind != BTFKind::Union) {
      return BTFError(BTFErrorInformation{
          BTFErrorInformation::Code::InvalidTypeLayout,
          BTFErrorInformation::FileRange{local_type_id, 0},
      });
    }

    auto opt_scope = view.getTypeScope(local_type_id);
    if (!opt_scope.has_value()) {
      return BTFError(BTFErrorInformation{
          BTFErrorInformation::Code::InvalidModule,
          BTFErrorInformation::FileRange{local_type_id, 0},
      });
    }

    BTFTypeInspection inspection;
    inspection.module_name = module_name;
    inspection.requested_id = local_type_id;
    inspection.resolved_id = local_type_id;
    inspection.source = opt_scope->source;
    inspection.depth = opt_scope->depth;
    inspection.kind = kind;
    inspection.type_name = getTypeDisplayName(view, local_type_id);

    std::uint32_t declared_size{0};
    std::vector<std::string> unexplainable_reasons;

    if (kind == BTFKind::Struct) {
      const auto &struct_type = std::get<StructBTFType>(*opt_type);
      declared_size = struct_type.size;

      if (object_bytes.size() != declared_size) {
        return BTFError(BTFErrorInformation{
            BTFErrorInformation::Code::ObjectSizeMismatch,
            BTFErrorInformation::FileRange{object_bytes.size(), declared_size},
        });
      }

      inspection.size = declared_size;
      inspection.raw_bytes = hexEncode(object_bytes);

      for (const auto &member : struct_type.member_list) {
        BTFInspectedMember out;
        out.has_name = member.opt_name.has_value();
        if (out.has_name) {
          out.name = member.opt_name.value();
        }

        out.type_id = member.type;
        auto opt_member_scope = view.getTypeScope(member.type);
        if (opt_member_scope.has_value()) {
          out.type_source = opt_member_scope->source;
          out.type_depth = opt_member_scope->depth;
        }

        out.type_name = getTypeDisplayName(view, member.type);
        out.offset_bits = member.offset;

        // Recurse through wrappers to find the underlying kind.
        auto underlying_id = stripQualifiers(view, member.type);
        auto opt_underlying = view.getType(underlying_id);
        if (opt_underlying.has_value() &&
            IBTF::getBTFTypeKind(*opt_underlying) == BTFKind::Ptr) {
          out.is_pointer = true;
          out.size_bits = sizeof(void *) * 8;
        }

        if (member.opt_bitfield_size.has_value() &&
            member.opt_bitfield_size.value() != 0) {
          out.is_bitfield = true;
          out.size_bits = member.opt_bitfield_size.value();

          if (inspection.unexplainable_reason.empty()) {
            inspection.unexplainable_reason = "unsupported bitfield member";
          }

          unexplainable_reasons.push_back("bitfield");
        } else {
          auto opt_size_bits = getTypeSizeBits(view, member.type);
          if (!opt_size_bits.has_value()) {
            out.incomplete = true;

            if (inspection.unexplainable_reason.empty()) {
              inspection.unexplainable_reason =
                  "incomplete or unsupported member type";
            }

            unexplainable_reasons.push_back("incomplete");
          } else {
            out.size_bits = opt_size_bits.value();
          }
        }

        if (!out.is_bitfield && !out.incomplete) {
          auto start_bit = out.offset_bits;
          auto end_bit = start_bit + out.size_bits;

          if (end_bit % 8 == 0 && end_bit <= declared_size * 8) {
            auto start_byte = start_bit / 8;
            auto size_byte = out.size_bits / 8;
            out.raw_bytes = hexEncode(object_bytes.data() + start_byte,
                                      size_byte);

            if (out.is_pointer) {
              out.pointer_value =
                  decodePointerValue(object_bytes.data() + start_byte,
                                     size_byte);
            }
          }
        }

        inspection.members.push_back(std::move(out));
      }

    } else {
      // Unions share storage; there is no single by-value offset walk. The
      // bytes still describe every member, so report them with offset 0.
      const auto &union_type = std::get<UnionBTFType>(*opt_type);
      declared_size = union_type.size;

      if (object_bytes.size() != declared_size) {
        return BTFError(BTFErrorInformation{
            BTFErrorInformation::Code::ObjectSizeMismatch,
            BTFErrorInformation::FileRange{object_bytes.size(), declared_size},
        });
      }

      inspection.size = declared_size;
      inspection.raw_bytes = hexEncode(object_bytes);

      for (const auto &member : union_type.member_list) {
        BTFInspectedMember out;
        out.has_name = member.opt_name.has_value();
        if (out.has_name) {
          out.name = member.opt_name.value();
        }

        out.type_id = member.type;
        auto opt_member_scope = view.getTypeScope(member.type);
        if (opt_member_scope.has_value()) {
          out.type_source = opt_member_scope->source;
          out.type_depth = opt_member_scope->depth;
        }

        out.type_name = getTypeDisplayName(view, member.type);
        out.offset_bits = 0;

        auto underlying_id = stripQualifiers(view, member.type);
        auto opt_underlying = view.getType(underlying_id);
        if (opt_underlying.has_value() &&
            IBTF::getBTFTypeKind(*opt_underlying) == BTFKind::Ptr) {
          out.is_pointer = true;
          out.size_bits = sizeof(void *) * 8;
        }

        if (member.opt_bitfield_size.has_value() &&
            member.opt_bitfield_size.value() != 0) {
          out.is_bitfield = true;
          out.size_bits = member.opt_bitfield_size.value();

          if (inspection.unexplainable_reason.empty()) {
            inspection.unexplainable_reason = "unsupported bitfield member";
          }
        } else {
          auto opt_size_bits = getTypeSizeBits(view, member.type);
          if (!opt_size_bits.has_value()) {
            out.incomplete = true;
            if (inspection.unexplainable_reason.empty()) {
              inspection.unexplainable_reason =
                  "incomplete or unsupported member type";
            }
          } else {
            out.size_bits = opt_size_bits.value();
          }
        }

        // A union member covers the whole object when it fits; slice from 0.
        if (!out.is_bitfield && !out.incomplete &&
            out.size_bits <= object_bytes.size() * 8) {
          auto size_byte = out.size_bits / 8;
          out.raw_bytes =
              hexEncode(object_bytes.data(), size_byte);

          if (out.is_pointer) {
            out.pointer_value =
                decodePointerValue(object_bytes.data(), size_byte);
          }
        }

        inspection.members.push_back(std::move(out));
      }
    }

    inspection.explainable = unexplainable_reasons.empty();
    if (!inspection.explainable && inspection.unexplainable_reason.empty()) {
      inspection.unexplainable_reason = "unsupported layout";
    }

    return inspection;

  } catch (const std::bad_alloc &) {
    return BTFError(BTFErrorInformation{
        BTFErrorInformation::Code::MemoryAllocationFailure, std::nullopt});

  } catch (const BTFError &e) {
    return e;
  }
}

} // namespace btfparse
