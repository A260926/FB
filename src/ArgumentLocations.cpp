#include "ArgumentLocations.hpp"
#include "BinaryReader.hpp"

#include <algorithm>
#include <cstring>

namespace MemPDB::detail
{
    namespace
    {
        template<typename Callback>
        void ForEachSymRecord(std::span<const std::byte> data, Callback&& cb)
        {
            BinaryReader r(data);
            while (!r.AtEnd() && r.Remaining() >= 4)
            {
                const uint16_t recLen = r.Read<uint16_t>();
                if (recLen < 2) break;
                const uint16_t kind = r.Read<uint16_t>();
                const uint16_t payloadLen = static_cast<uint16_t>(recLen - 2);
                if (payloadLen > r.Remaining()) break;
                cb(kind, r.ReadBytes(payloadLen));
            }
        }

        struct LocationSpec
        {
            ArgumentLocationKind kind = ArgumentLocationKind::Register;
            uint16_t             reg  = 0;
            int32_t              offset = 0;
        };

        // Parse the location prefix (everything before the shared range
        // header). Returns false for encodings we do not model.
        bool ParseLocationPrefix(uint16_t kind, BinaryReader& r, LocationSpec& spec)
        {
            switch (kind)
            {
            case kS_DEFRANGE_REGISTER:
                if (r.Remaining() < 4) return false;
                spec.kind = ArgumentLocationKind::Register;
                spec.reg  = r.Read<uint16_t>();
                r.Skip(2); // attributes
                spec.offset = 0;
                return true;

            case kS_DEFRANGE_FRAMEPOINTER_REL:
                if (r.Remaining() < 4) return false;
                spec.kind   = ArgumentLocationKind::FramePointerRelative;
                spec.reg    = 0;
                spec.offset = r.Read<int32_t>();
                return true;

            case kS_DEFRANGE_REGISTER_REL:
            {
                if (r.Remaining() < 8) return false;
                const uint16_t reg   = r.Read<uint16_t>();
                const uint16_t flags = r.Read<uint16_t>();
                if (flags & kDefRangeRegRel_SpilledUDTMember) return false;
                if (flags & kDefRangeRegRel_OffsetParent)     return false;
                spec.kind   = ArgumentLocationKind::RegisterRelative;
                spec.reg    = reg;
                spec.offset = r.Read<int32_t>();
                return true;
            }
            default:
                return false;
            }
        }

        void Emit(std::vector<ArgumentLocation>& out,
                  uint64_t lo, uint64_t hi, const LocationSpec& spec)
        {
            if (lo >= hi) return;
            if (lo > 0xFFFFFFFFull) return;
            ArgumentLocation loc{};
            loc.BeginRVA = static_cast<uint32_t>(lo);
            loc.EndRVA   = static_cast<uint32_t>(std::min<uint64_t>(hi, 0xFFFFFFFFull));
            loc.Kind     = spec.kind;
            loc.Register = spec.reg;
            loc.Offset   = spec.offset;
            out.push_back(loc);
        }
    }

    void DecodeDefRange(uint16_t                       recordKind,
                        std::span<const std::byte>     payload,
                        const SectionTable&            sections,
                        uint32_t                       procRva,
                        uint32_t                       procLen,
                        std::vector<ArgumentLocation>& out)
    {
        BinaryReader r(payload);
        LocationSpec spec{};

        if (recordKind == kS_DEFRANGE_FRAMEPOINTER_REL_FULL_SCOPE)
        {
            // i32 offset only; the location covers the whole procedure.
            if (r.Remaining() < 4 || procLen == 0 || procRva == 0) return;
            spec.kind   = ArgumentLocationKind::FramePointerRelative;
            spec.reg    = 0;
            spec.offset = r.Read<int32_t>();
            Emit(out, procRva, static_cast<uint64_t>(procRva) + procLen, spec);
            return;
        }

        if (!ParseLocationPrefix(recordKind, r, spec)) return;
        if (r.Remaining() < 8) return;

        const uint32_t sectionOffset = r.Read<uint32_t>();
        const uint16_t section       = r.Read<uint16_t>();
        const uint16_t length        = r.Read<uint16_t>();
        if (section == 0 || section > sections.rvaBase.size()) return;

        const uint32_t beginRva = sections.Resolve(section, sectionOffset);
        uint64_t begin = beginRva;
        uint64_t end   = begin + length;
        if (length == 0 || begin >= end) return;

        // Clip to the procedure's extent before cutting the gap.
        if (procLen != 0)
        {
            const uint64_t procEnd = static_cast<uint64_t>(procRva) + procLen;
            begin = std::max<uint64_t>(begin, procRva);
            end   = std::min<uint64_t>(end, procEnd);
            if (begin >= end) return;
        }

        uint32_t gapOffset = 0;
        uint32_t gapLength = 0;
        if (r.Remaining() >= 4)
        {
            gapOffset = r.Read<uint16_t>();
            gapLength = r.Read<uint16_t>();
        }

        if (gapLength == 0)
        {
            Emit(out, begin, end, spec);
            return;
        }

        // gapOffset is relative to the original range start, so anchor it
        // before clipping shifted `begin`.
        const uint64_t gapBegin = static_cast<uint64_t>(beginRva) + gapOffset;
        const uint64_t gapEnd   = gapBegin + gapLength;

        if (gapBegin >= end || gapEnd <= begin)
        {
            Emit(out, begin, end, spec);
            return;
        }
        Emit(out, begin, std::min<uint64_t>(gapBegin, end), spec);
        Emit(out, std::max<uint64_t>(gapEnd, begin), end, spec);
    }

    ModuleSymData ScanModuleSyms(std::span<const std::byte> stream,
                                 const SectionTable&        sections)
    {
        ModuleSymData out;
        if (stream.size() < 4) return out;

        struct Scope
        {
            bool   isProc = false;
            size_t proc   = static_cast<size_t>(-1);
            // Index into procs[proc].locals of the most recent directly-scoped
            // S_LOCAL, or -1. Only the immediately following DEFRANGE records
            // may attach to it.
            int64_t lastLocal = -1;
        };

        std::vector<Scope> scopes;

        ForEachSymRecord(stream.subspan(4),
            [&](uint16_t kind, std::span<const std::byte> payload)
            {
                switch (kind)
                {
                case kS_GPROC32:
                case kS_LPROC32:
                case kS_GPROC32_ID:
                case kS_LPROC32_ID:
                {
                    if (payload.size() < sizeof(ProcSym32Payload) + 1) return;
                    ProcSym32Payload p;
                    std::memcpy(&p, payload.data(), sizeof(p));
                    const char* namePtr =
                        reinterpret_cast<const char*>(payload.data() + sizeof(p));
                    const size_t nameLen = strnlen(namePtr, payload.size() - sizeof(p));

                    const bool isId = (kind == kS_GPROC32_ID || kind == kS_LPROC32_ID);
                    ModuleProc proc;
                    proc.name     = std::string_view(namePtr, nameLen);
                    proc.typeIdx  = p.TypeIndex;
                    proc.procLen  = p.ProcLen;
                    proc.rva      = sections.Resolve(p.Segment, p.Offset);
                    proc.isIdType = isId;

                    Scope s;
                    s.isProc = true;
                    s.proc   = out.procs.size();
                    scopes.push_back(s);
                    out.procs.push_back(std::move(proc));
                    return;
                }
                case kS_BLOCK32:
                case kS_INLINESITE:
                case kS_INLINESITE2:
                {
                    // Nested scopes get their own (parameter-less) frame so an
                    // S_END pairs with them and never terminates the enclosing
                    // procedure. Entering one also closes the parent's pending
                    // DEFRANGE owner: ranges that "immediately follow" a local
                    // cannot reappear after an intervening scope.
                    if (!scopes.empty()) scopes.back().lastLocal = -1;
                    Scope s;
                    scopes.push_back(s);
                    return;
                }
                case kS_END:
                case kS_ENDPROC:
                    // Popping discards that scope's own pending owner, so a
                    // local can never keep receiving ranges after its scope
                    // closes.
                    if (!scopes.empty()) scopes.pop_back();
                    return;
                case kS_LOCAL:
                {
                    if (scopes.empty()) return;
                    Scope& scope = scopes.back();

                    // A new S_LOCAL always truncates the previous attribution,
                    // whether or not it is a parameter.
                    scope.lastLocal = -1;

                    if (payload.size() < 6) return;
                    uint32_t typeIdx = 0;
                    uint16_t flags   = 0;
                    std::memcpy(&typeIdx, payload.data(), 4);
                    std::memcpy(&flags, payload.data() + 4, 2);
                    const char* namePtr =
                        reinterpret_cast<const char*>(payload.data() + 6);
                    const size_t nameLen = strnlen(namePtr, payload.size() - 6);

                    // Block-scope locals never describe a formal parameter.
                    if (!scope.isProc) return;

                    ModuleProc& proc = out.procs[scope.proc];
                    if (!(flags & kLocalFlag_Parameter)) return;

                    proc.hasModernParameters = true;
                    ModuleLocal local;
                    local.name         = std::string_view(namePtr, nameLen);
                    local.typeIdx      = typeIdx;
                    local.parameter    = true;
                    local.optimizedOut = (flags & kLocalFlag_OptimizedOut) != 0;
                    proc.locals.push_back(std::move(local));
                    scope.lastLocal = static_cast<int64_t>(proc.locals.size() - 1);
                    return;
                }
                default:
                {
                    if (kind == kS_REGREL32 || kind == kS_FRAMEREL)
                    {
                        // Legacy positional parameter path: collect while any
                        // enclosing procedure is active, exactly as before.
                        if (scopes.empty()) return;
                        size_t procIdx = static_cast<size_t>(-1);
                        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
                            if (it->isProc) { procIdx = it->proc; break; }
                        if (procIdx == static_cast<size_t>(-1)) return;

                        std::string_view name;
                        uint32_t offset = 0;
                        if (kind == kS_REGREL32)
                        {
                            if (payload.size() < sizeof(RegRelSym32Payload) + 1) return;
                            RegRelSym32Payload p;
                            std::memcpy(&p, payload.data(), sizeof(p));
                            offset = p.Offset;
                            const char* np =
                                reinterpret_cast<const char*>(payload.data() + sizeof(p));
                            name = std::string_view(np, strnlen(np, payload.size() - sizeof(p)));
                        }
                        else
                        {
                            if (payload.size() < sizeof(FrameRelSymPayload) + 1) return;
                            FrameRelSymPayload p;
                            std::memcpy(&p, payload.data(), sizeof(p));
                            offset = p.Offset;
                            const char* np =
                                reinterpret_cast<const char*>(payload.data() + sizeof(p));
                            name = std::string_view(np, strnlen(np, payload.size() - sizeof(p)));
                        }
                        out.procs[procIdx].legacyArgs.push_back({ name, offset });
                        return;
                    }

                    if (kind < kS_DEFRANGE_REGISTER || kind > kS_DEFRANGE_REGISTER_REL)
                        return;
                    // S_DEFRANGE*: belongs to the local that immediately
                    // precedes it in the current scope, if any.
                    if (scopes.empty()) return;
                    Scope& scope = scopes.back();
                    if (!scope.isProc || scope.lastLocal < 0) return;

                    ModuleProc& proc = out.procs[scope.proc];
                    proc.sawDefRange = true;
                    ModuleLocal& local =
                        proc.locals[static_cast<size_t>(scope.lastLocal)];
                    // An optimized-away parameter keeps its name and type but
                    // never acquires a location, whatever follows it.
                    if (local.optimizedOut) return;
                    DecodeDefRange(kind, payload, sections,
                                   proc.rva, proc.procLen, local.locations);
                    return;
                }
                }
            });

        return out;
    }
}
