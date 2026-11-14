//
// Copyright (c) 2021-present, Trail of Bits, Inc.
// All rights reserved.
//
// This source code is licensed in accordance with the terms specified in
// the LICENSE file found in the root directory of this source tree.
//

#pragma once

#include "btf_types.h"

#include <btfparse/ibtf.h>
#include <btfparse/ifilereader.h>

#include <map>
#include <string>
#include <vector>

namespace btfparse {

struct BTFFile final {
  BTFHeader btf_header;
  IFileReader::Ptr file_reader;

  // The bytes [string_start, string_start + string_size) are this file's
  // own string table. cumulative_string_section holds every string byte
  // from global offset 0 through this block, so a file whose offsets
  // continue a previous split base can resolve ancestor strings too.
  std::uint32_t string_start{};
  std::uint32_t string_size{};
  std::string cumulative_string_section;
};

using BTFFileList = std::vector<BTFFile>;

using BTFTypeParser = Result<BTFType, BTFError> (*)(const BTFFileList &,
                                                    const BTFTypeHeader &,
                                                    IFileReader &);

class BTF final : public IBTF {
public:
  virtual ~BTF() override;

  virtual std::optional<BTFType>
  getType(std::uint32_t id) const noexcept override;

  virtual std::optional<BTFKind>
  getKind(std::uint32_t id) const noexcept override;

  virtual std::uint32_t count() const noexcept override;
  virtual BTFTypeMap getAll() const noexcept override;

  virtual std::optional<BTFTypeScope>
  getTypeScope(std::uint32_t id) const noexcept override;

  virtual std::string getStringSection() const override;
  virtual BTFSourceID getOriginToken() const noexcept override;
  virtual std::uint32_t getDepth() const noexcept override;

private:
  struct PrivateData;
  std::unique_ptr<PrivateData> d;

  BTF(const PathList &path_list);
  BTF(const IBTF &base, const std::filesystem::path &path);

public:
  static BTFError convertFileReaderError(const FileReaderError &error) noexcept;

  static std::optional<BTFError>
  detectEndianness(bool &little_endian, IFileReader &file_reader) noexcept;

  static Result<BTFHeader, BTFError>
  readBTFHeader(IFileReader &file_reader) noexcept;

  static Result<BTFTypeMap, BTFError>
  parseTypeSections(const BTFFileList &btf_file_list) noexcept;

  // Parses one or more consecutive type sections starting at `first_type_id`.
  // Split children rely on this to place their local types right after the
  // ancestor numbering while their records still use global type IDs.
  static Result<BTFTypeMap, BTFError>
  parseTypeSection(const BTFFileList &btf_file_list,
                   std::uint32_t first_type_id) noexcept;

  static Result<std::string, BTFError>
  readSection(IFileReader &file_reader, std::uint64_t offset,
              std::uint32_t size) noexcept;

  static Result<BTFTypeHeader, BTFError>
  parseTypeHeader(IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseIntData(const BTFFileList &btf_file_list,
               const BTFTypeHeader &btf_type_header,
               IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parsePtrData(const BTFFileList &, const BTFTypeHeader &btf_type_header,
               IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseConstData(const BTFFileList &, const BTFTypeHeader &btf_type_header,
                 IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseArrayData(const BTFFileList &, const BTFTypeHeader &btf_type_header,
                 IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseTypedefData(const BTFFileList &btf_file_list,
                   const BTFTypeHeader &btf_type_header,
                   IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseEnumData(const BTFFileList &btf_file_list,
                const BTFTypeHeader &btf_type_header,
                IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseFuncProtoData(const BTFFileList &btf_file_list,
                     const BTFTypeHeader &btf_type_header,
                     IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseVolatileData(const BTFFileList &, const BTFTypeHeader &btf_type_header,
                    IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseStructData(const BTFFileList &btf_file_list,
                  const BTFTypeHeader &btf_type_header,
                  IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseUnionData(const BTFFileList &btf_file_list,
                 const BTFTypeHeader &btf_type_header,
                 IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseFwdData(const BTFFileList &btf_file_list,
               const BTFTypeHeader &btf_type_header,
               IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseFuncData(const BTFFileList &btf_file_list,
                const BTFTypeHeader &btf_type_header,
                IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseFloatData(const BTFFileList &btf_file_list,
                 const BTFTypeHeader &btf_type_header,
                 IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseRestrictData(const BTFFileList &, const BTFTypeHeader &btf_type_header,
                    IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseVarData(const BTFFileList &btf_file_list,
               const BTFTypeHeader &btf_type_header,
               IFileReader &file_reader) noexcept;

  static Result<BTFType, BTFError>
  parseDataSecData(const BTFFileList &btf_file_list,
                   const BTFTypeHeader &btf_type_header,
                   IFileReader &file_reader) noexcept;

  static Result<std::string, BTFError>
  parseString(const BTFFileList &btf_file_list, std::uint64_t offset) noexcept;

  static Result<std::string, BTFError>
  parseString(IFileReader &file_reader, std::uint64_t offset) noexcept;

  friend class IBTF;
};

} // namespace btfparse
