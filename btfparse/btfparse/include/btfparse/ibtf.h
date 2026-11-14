//
// Copyright (c) 2021-present, Trail of Bits, Inc.
// All rights reserved.
//
// This source code is licensed in accordance with the terms specified in
// the LICENSE file found in the root directory of this source tree.
//

#pragma once

#include <btfparse/error.h>
#include <btfparse/result.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace btfparse {

struct BTFErrorInformation final {
  enum class Code {
    Unknown,
    MemoryAllocationFailure,
    FileNotFound,
    IOError,
    InvalidMagicValue,
    InvalidBTFKind,
    UnsupportedBTFKind,
    InvalidIntBTFTypeEncoding,
    InvalidPtrBTFTypeEncoding,
    InvalidArrayBTFTypeEncoding,
    InvalidTypedefBTFTypeEncoding,
    InvalidEnumBTFTypeEncoding,
    InvalidFuncProtoBTFTypeEncoding,
    InvalidVolatileBTFTypeEncoding,
    InvalidFwdBTFTypeEncoding,
    InvalidFuncBTFTypeEncoding,
    InvalidFloatBTFTypeEncoding,
    InvalidRestrictBTFTypeEncoding,
    InvalidVarBTFTypeEncoding,
    InvalidDataSecBTFTypeEncoding,
    InvalidStringOffset,
    InvalidSplitBase,
    InvalidSplitTypeReference,
    InvalidModuleName,
    InvalidModule,
    InvalidTypeLayout,
    ObjectSizeMismatch,
  };

  struct FileRange final {
    std::uint64_t offset{};
    std::size_t size{};
  };

  using OptionalFileRange = std::optional<FileRange>;

  Code code{Code::Unknown};
  OptionalFileRange opt_file_range;
};

struct BTFErrorInformationPrinter final {
  std::string operator()(const BTFErrorInformation &error_information) const {
    std::stringstream buffer;
    buffer << "Error: '";

    switch (error_information.code) {
    case BTFErrorInformation::Code::Unknown:
      buffer << "Unknown error";
      break;

    case BTFErrorInformation::Code::MemoryAllocationFailure:
      buffer << "Memory allocation failure";
      break;

    case BTFErrorInformation::Code::FileNotFound:
      buffer << "File not found";
      break;

    case BTFErrorInformation::Code::IOError:
      buffer << "IO error";
      break;

    case BTFErrorInformation::Code::InvalidMagicValue:
      buffer << "Invalid magic value";
      break;

    case BTFErrorInformation::Code::InvalidBTFKind:
      buffer << "Invalid BTF kind";
      break;

    case BTFErrorInformation::Code::UnsupportedBTFKind:
      buffer << "Unsupported BTF kind";
      break;

    case BTFErrorInformation::Code::InvalidIntBTFTypeEncoding:
      buffer << "Invalid encoding for `Int` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidPtrBTFTypeEncoding:
      buffer << "Invalid encoding for `Ptr` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidArrayBTFTypeEncoding:
      buffer << "Invalid encoding for `Array` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidTypedefBTFTypeEncoding:
      buffer << "Invalid encoding for `Typedef` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidEnumBTFTypeEncoding:
      buffer << "Invalid encoding for `Enum` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidFuncProtoBTFTypeEncoding:
      buffer << "Invalid encoding for `FuncProto` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidVolatileBTFTypeEncoding:
      buffer << "Invalid encoding for `Volatile` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidFwdBTFTypeEncoding:
      buffer << "Invalid encoding for `Fwd` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidFuncBTFTypeEncoding:
      buffer << "Invalid encoding for `Func` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidFloatBTFTypeEncoding:
      buffer << "Invalid encoding for `Float` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidRestrictBTFTypeEncoding:
      buffer << "Invalid encoding for `Restrict` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidVarBTFTypeEncoding:
      buffer << "Invalid encoding for `Var` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidDataSecBTFTypeEncoding:
      buffer << "Invalid encoding for `DataSec` BTFType";
      break;

    case BTFErrorInformation::Code::InvalidStringOffset:
      buffer << "Invalid string offset";
      break;

    case BTFErrorInformation::Code::InvalidSplitBase:
      buffer << "Invalid split BTF base view";
      break;

    case BTFErrorInformation::Code::InvalidSplitTypeReference:
      buffer << "Invalid type reference in split BTF";
      break;

    case BTFErrorInformation::Code::InvalidModuleName:
      buffer << "Invalid module name";
      break;

    case BTFErrorInformation::Code::InvalidModule:
      buffer << "Invalid module view";
      break;

    case BTFErrorInformation::Code::InvalidTypeLayout:
      buffer << "Invalid type layout";
      break;

    case BTFErrorInformation::Code::ObjectSizeMismatch: {
      buffer << "Input length does not match the selected layout";

      if (error_information.opt_file_range.has_value()) {
        const auto &file_range = error_information.opt_file_range.value();
        buffer << " (got " << file_range.offset << " bytes, expected "
               << file_range.size << " bytes)";
      }
      break;
    }
    }

    buffer << "'";

    // ObjectSizeMismatch embeds both lengths in its message above; the
    // generic file-range suffix would misleadingly render them as an
    // interval, so skip it for that code.
    if (error_information.code !=
            BTFErrorInformation::Code::ObjectSizeMismatch &&
        error_information.opt_file_range.has_value()) {
      const auto &file_range = error_information.opt_file_range.value();
      buffer << ", File range: " << file_range.offset << " - "
             << (file_range.offset + file_range.size);
    }

    return buffer.str();
  }
};

using BTFError = Error<BTFErrorInformation, BTFErrorInformationPrinter>;

struct IntBTFType final {
  enum class Encoding {
    None,
    Signed,
    Char,
    Bool,
  };

  std::string name;
  std::uint32_t size{};
  Encoding encoding{Encoding::None};

  std::uint8_t offset{};
  std::uint8_t bits{};
};

struct PtrBTFType final {
  std::uint32_t type{};
};

struct ConstBTFType final {
  std::uint32_t type{};
};

struct ArrayBTFType final {
  std::uint32_t type{};
  std::uint32_t index_type{};
  std::uint32_t nelems{};
};

struct TypedefBTFType final {
  std::string name;
  std::uint32_t type{};
};

struct EnumBTFType final {
  struct Value final {
    std::string name;
    std::int32_t val{};
  };

  using ValueList = std::vector<Value>;

  std::optional<std::string> opt_name;
  std::uint32_t size{};
  ValueList value_list;
};

struct FuncProtoBTFType final {
  struct Param final {
    std::optional<std::string> opt_name;
    std::uint32_t type{};
  };

  using ParamList = std::vector<Param>;

  std::uint32_t return_type{};
  ParamList param_list;
  bool is_variadic{false};
};

struct VolatileBTFType final {
  std::uint32_t type{};
};

struct StructBTFType final {
  struct Member final {
    std::optional<std::string> opt_name;
    std::uint32_t type{};
    std::uint32_t offset{};
    std::optional<std::uint8_t> opt_bitfield_size;
  };

  using MemberList = std::vector<Member>;

  std::optional<std::string> opt_name;
  std::uint32_t size{};
  MemberList member_list;
};

struct UnionBTFType final {
  struct Member final {
    std::optional<std::string> opt_name;
    std::uint32_t type{};
    std::uint32_t offset{};
    std::optional<std::uint8_t> opt_bitfield_size;
  };

  using MemberList = std::vector<Member>;

  std::optional<std::string> opt_name;
  std::uint32_t size{};
  MemberList member_list;
};

struct FwdBTFType final {
  std::string name;
  bool is_union{false};
};

struct FuncBTFType final {
  enum class Linkage {
    Static = 0,
    Global = 1,
    Extern = 2,
  };

  std::string name;
  std::uint32_t type{};
  Linkage linkage{Linkage::Static};
};

struct FloatBTFType final {
  std::string name;
  std::uint32_t size{};
};

struct RestrictBTFType final {
  std::uint32_t type{};
};

struct VarBTFType final {
  std::string name;
  std::uint32_t type{};
  std::uint32_t linkage{};
};

struct DataSecBTFType final {
  struct Variable final {
    std::uint32_t type{};
    std::uint32_t offset{};
    std::uint32_t size{};
  };

  using VariableList = std::vector<Variable>;

  std::string name;
  std::uint32_t size{};
  VariableList variable_list;
};

enum class BTFKind {
  Void = 0,
  Int = 1,
  Ptr = 2,
  Array = 3,
  Struct = 4,
  Union = 5,
  Enum = 6,
  Fwd = 7,
  Typedef = 8,
  Volatile = 9,
  Const = 10,
  Restrict = 11,
  Func = 12,
  FuncProto = 13,
  Var = 14,
  DataSec = 15,
  Float = 16,
};

using BTFType =
    std::variant<std::monostate, IntBTFType, PtrBTFType, ArrayBTFType,
                 StructBTFType, UnionBTFType, EnumBTFType, FwdBTFType,
                 TypedefBTFType, VolatileBTFType, ConstBTFType, RestrictBTFType,
                 FuncBTFType, FuncProtoBTFType, VarBTFType, DataSecBTFType,
                 FloatBTFType>;

using BTFTypeMap = std::unordered_map<std::uint32_t, BTFType>;
using PathList = std::vector<std::filesystem::path>;

// Opaque, process-local identity for one loaded BTF view. Tokens are never
// reused, even after the owning view is destroyed, so independent loads of
// identical bytes can never be confused for one another.
using BTFSourceID = std::uint64_t;

// Identifies the origin of a type inside a split view.
//
// - `source` is the identity of the load that introduced the type.
// - `depth` is 0 for the root load and increases by one per split layer.
// - `id` is the ID under which the queried view exposes the type. Ancestor
//   types keep their original ID; a split layer numbers its own types right
//   after the highest ancestor ID, so the same numeric ID in two sibling
//   layers refers to two different types. Together, (`source`, `id`) is the
//   stable identity of a type across views, independent of its name.
struct BTFTypeScope final {
  BTFSourceID source{0};
  std::uint32_t depth{0};
  std::uint32_t id{0};
};

class IBTF {
public:
  using Ptr = std::unique_ptr<IBTF>;

  static Result<Ptr, BTFError>
  createFromPath(const std::filesystem::path &path) noexcept;

  static Result<Ptr, BTFError>
  createFromPathList(const PathList &path_list) noexcept;

  // Creates a split child of an already parsed view. The child keeps all
  // ancestor types visible under their original IDs, maps its own local IDs
  // (starting right after the ancestor maximum) into the shared numbering
  // space, and owns its string table so it stays usable after the base
  // handle (and the source file) are gone.
  static Result<Ptr, BTFError>
  createSplitFromPath(const IBTF &base,
                      const std::filesystem::path &path) noexcept;

  virtual std::optional<BTFType> getType(std::uint32_t id) const noexcept = 0;
  virtual std::optional<BTFKind> getKind(std::uint32_t id) const noexcept = 0;

  virtual std::uint32_t count() const noexcept = 0;
  virtual BTFTypeMap getAll() const noexcept = 0;

  // Scope metadata for a view. The root returns depth 0 for every ID; split
  // views report which load introduced the requested type. This is the
  // authoritative provenance used when rewriting type references; names are
  // never consulted to infer identity.
  virtual std::optional<BTFTypeScope>
  getTypeScope(std::uint32_t) const noexcept {
    return std::nullopt;
  }

  // Raw string section owned by this view, using the same global offsets as
  // its type records. Retaining these bytes (not just decoded names) is what
  // lets a split child resolve ancestor strings that no ancestor type used,
  // and what lets grandchildren keep loading after the parent handle and the
  // source file are gone.
  virtual std::string getStringSection() const { return {}; }

  // Identity of the load behind this view, and its split depth.
  virtual BTFSourceID getOriginToken() const noexcept { return 0; }
  virtual std::uint32_t getDepth() const noexcept { return 0; }

  static BTFKind getBTFTypeKind(const BTFType &btf_type) noexcept;

  IBTF() = default;
  virtual ~IBTF() = default;

  IBTF(const IBTF &) = delete;
  IBTF &operator=(const IBTF &) = delete;
};

} // namespace btfparse
