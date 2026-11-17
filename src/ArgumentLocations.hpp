#pragma once

// Modern CodeView parameter locations: S_LOCAL + S_DEFRANGE decoding.
//
// A module symbol stream is walked once, tracking the procedure/block scope
// nesting. Only S_LOCALs that sit directly in a procedure's scope and carry
// CV_LVARFLAG_PARAMETER describe a formal parameter; their S_DEFRANGE records
// are decoded into fully-resolved RVA ranges here, before any interning or
// buffer release, so the result owns plain numbers with no backing PDB bytes.

#include <MemPDB/MemPDB.hpp>
#include "PDBFormat.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace MemPDB::detail
{
    struct SectionTable
    {
        std::vector<uint32_t> rvaBase;

        uint32_t Resolve(uint16_t seg, uint32_t off) const noexcept
        {
            if (seg == 0 || seg > rvaBase.size()) return 0;
            return rvaBase[seg - 1] + off;
        }
    };

    // Legacy S_REGREL32 / S_FRAMEREL parameter: a name and a raw offset.
    struct ModuleArg
    {
        std::string_view name;
        uint32_t         offset;
    };

    // An S_LOCAL declared directly in the procedure's scope.
    struct ModuleLocal
    {
        std::string_view              name;
        uint32_t                      typeIdx;
        bool                          parameter;
        bool                          optimizedOut;
        std::vector<ArgumentLocation> locations; // RVA ranges, record order
    };

    // A procedure record plus the parameter information seen in its scope.
    struct ModuleProc
    {
        std::string_view         name;
        uint32_t                 typeIdx  = 0;
        uint32_t                 procLen  = 0;
        uint32_t                 rva      = 0;     // section-resolved entry RVA
        bool                     isIdType = false; // typeIdx indexes the IPI
        std::vector<ModuleArg>   legacyArgs;       // old positional path
        std::vector<ModuleLocal> locals;           // direct proc scope, in order
        bool                     hasModernParameters = false;
        // True when any S_DEFRANGE record was seen in this procedure -- even if
        // it could not be decoded. Only procs with this set rely on S_LOCALs
        // for parameter naming; the rest keep the legacy positional path.
        bool                     sawDefRange = false;
    };

    struct ModuleSymData
    {
        std::vector<ModuleProc> procs;
    };

    // Walk the S_* records of one module symbol stream. `stream` is the
    // [u32 signature][symbols...] prefix bounded by SymByteSize. Procedure and
    // S_BLOCK32 scopes are paired with their S_END markers so a block's end
    // never terminates the enclosing procedure.
    ModuleSymData ScanModuleSyms(std::span<const std::byte> stream,
                                 const SectionTable&        sections);

    // Decode one S_DEFRANGE* record into zero or more non-empty RVA pieces,
    // appending them to `out`. Handles gap removal, clipping to the procedure
    // range, and the FULL_SCOPE form. Unsupported/foreign encodings are
    // ignored. Exposed for focused unit coverage.
    void DecodeDefRange(uint16_t                        recordKind,
                        std::span<const std::byte>      payload,
                        const SectionTable&             sections,
                        uint32_t                        procRva,
                        uint32_t                        procLen,
                        std::vector<ArgumentLocation>&  out);
}
