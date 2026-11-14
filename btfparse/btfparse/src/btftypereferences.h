//
// Copyright (c) 2021-present, Trail of Bits, Inc.
// All rights reserved.
//
// This source code is licensed in accordance with the terms specified in
// the LICENSE file found in the root directory of this source tree.
//

#pragma once

#include <btfparse/ibtf.h>

#include <utility>

namespace btfparse {
namespace detail {

// Enumerates every type-ID-carrying field of a parsed BTF type. This is the
// single place that knows the full reference surface of BTFKind, so split
// views and the multi-module header generator cannot disagree about which
// fields need rewriting.
//
// `visitor` is called as visitor(std::uint32_t &reference) and returns bool.
// Enumeration stops at the first false and the function returns false.
template <typename Visitor>
bool forEachTypeReference(BTFType &btf_type, Visitor &&visitor) {

  switch (static_cast<BTFKind>(btf_type.index())) {
  case BTFKind::Ptr: {
    return visitor(std::get<PtrBTFType>(btf_type).type);
  }

  case BTFKind::Array: {
    auto &array = std::get<ArrayBTFType>(btf_type);
    if (!visitor(array.type)) {
      return false;
    }
    return visitor(array.index_type);
  }

  case BTFKind::Struct: {
    auto &struct_type = std::get<StructBTFType>(btf_type);
    for (auto &member : struct_type.member_list) {
      if (!visitor(member.type)) {
        return false;
      }
    }
    return true;
  }

  case BTFKind::Union: {
    auto &union_type = std::get<UnionBTFType>(btf_type);
    for (auto &member : union_type.member_list) {
      if (!visitor(member.type)) {
        return false;
      }
    }
    return true;
  }

  case BTFKind::Typedef: {
    return visitor(std::get<TypedefBTFType>(btf_type).type);
  }

  case BTFKind::Volatile: {
    return visitor(std::get<VolatileBTFType>(btf_type).type);
  }

  case BTFKind::Const: {
    return visitor(std::get<ConstBTFType>(btf_type).type);
  }

  case BTFKind::Restrict: {
    return visitor(std::get<RestrictBTFType>(btf_type).type);
  }

  case BTFKind::Func: {
    return visitor(std::get<FuncBTFType>(btf_type).type);
  }

  case BTFKind::FuncProto: {
    auto &func_proto = std::get<FuncProtoBTFType>(btf_type);
    if (!visitor(func_proto.return_type)) {
      return false;
    }
    for (auto &param : func_proto.param_list) {
      if (!visitor(param.type)) {
        return false;
      }
    }
    return true;
  }

  case BTFKind::Var: {
    return visitor(std::get<VarBTFType>(btf_type).type);
  }

  case BTFKind::DataSec: {
    auto &data_sec = std::get<DataSecBTFType>(btf_type);
    for (auto &variable : data_sec.variable_list) {
      if (!visitor(variable.type)) {
        return false;
      }
    }
    return true;
  }

  case BTFKind::Void:
  case BTFKind::Int:
  case BTFKind::Enum:
  case BTFKind::Fwd:
  case BTFKind::Float:
    return true;
  }

  return true;
}

} // namespace detail
} // namespace btfparse
