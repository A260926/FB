#pragma once

#include <MemPDB/MemPDB.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace MemPDB::detail
{
    struct FunctionRecord
    {
        std::string_view name;
        uint32_t         rva;
        uint32_t         size;
        uint32_t         argOffset;
        uint32_t         argCount;
    };

    // Name-sorted record for a non-function public symbol (data global).
    // `kind` is packed into the trailing padding, so the record stays 24 bytes.
    struct GlobalRecord
    {
        std::string_view   name;
        uint32_t           rva;
        MemPDB::SymbolKind kind;
    };

    struct StructRecord
    {
        std::string_view name;
        uint32_t         size;        // total size in bytes
        uint32_t         fieldOffset; // index into ParseResult::fields
        uint32_t         fieldCount;
    };

    struct ParseResult
    {
        std::vector<FunctionRecord>            functions;
        std::vector<Argument>                  arguments;
        std::vector<GlobalRecord>              globals;
        std::array<uint32_t, kSymbolKindCount> globalKindCounts{}; // per-category tally
        std::vector<StructRecord>              structs;
        std::vector<Field>                     fields;
        std::vector<std::vector<std::byte>>    streamStorage;
        std::vector<char>                      stringArena;     // populated iff interned
        bool                                   internedStrings = false;
        MemPDB::ParseInfo                      timing;
    };

    ParseResult ParsePDB(std::span<const std::byte> rawPDB,
                         const MemPDB::ParseOptions& options);
}
