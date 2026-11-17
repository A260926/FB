#include "PDBParser.hpp"
#include "BinaryReader.hpp"
#include "MSFReader.hpp"
#include "PDBFormat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace { using Clock = std::chrono::high_resolution_clock; }
static double ElapsedMs(Clock::time_point a, Clock::time_point b) noexcept
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

namespace
{
    // Run f(0..n-1). When parallel is requested and worthwhile, fan out across
    // hardware threads with an atomic work counter; otherwise run inline.
    template<typename F>
    void ParallelFor(std::size_t n, bool parallel, F&& f)
    {
        const unsigned hw = std::thread::hardware_concurrency();
        if (!parallel || hw <= 1 || n <= 1)
        {
            for (std::size_t i = 0; i < n; ++i) f(i);
            return;
        }

        const unsigned workers =
            std::min<unsigned>(hw, static_cast<unsigned>(n));
        std::atomic<std::size_t> next{ 0 };
        auto run = [&]
        {
            for (;;)
            {
                const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) break;
                f(i);
            }
        };

        std::vector<std::thread> pool;
        pool.reserve(workers - 1);
        for (unsigned k = 1; k < workers; ++k) pool.emplace_back(run);
        run();
        for (auto& t : pool) t.join();
    }
}

namespace MemPDB::detail
{
    namespace
    {
        // First 8 bytes of a name packed big-endian into a uint64, so that
        // integer order equals lexicographic order. Names shorter than 8 bytes
        // are zero-padded in the low bytes, which sorts them before longer names
        // sharing the same prefix -- matching string ordering exactly.
        inline uint64_t NamePrefix8(std::string_view s) noexcept
        {
            uint64_t k = 0;
            const std::size_t n = s.size() < 8 ? s.size() : 8;
            for (std::size_t i = 0; i < n; ++i)
                k |= static_cast<uint64_t>(static_cast<unsigned char>(s[i])) << (56 - i * 8);
            return k;
        }

        // Categorize a data symbol from its MSVC decorated name. This is a few
        // byte comparisons -- run once per data symbol inside the symbol scan,
        // so it adds no pass and no allocation.
        //   ??_7 vftable   ??_8 vbtable   ??_C string   ??_R0..R4 RTTI
        //   ?name@@3..      global variable (trailing B/D => const-qualified)
        inline MemPDB::SymbolKind ClassifyGlobal(std::string_view n) noexcept
        {
            const std::size_t sz = n.size();
            if (sz >= 4 && n[0] == '?' && n[1] == '?' && n[2] == '_')
            {
                switch (n[3])
                {
                case '7': return MemPDB::SymbolKind::Vtable;
                case '8': return MemPDB::SymbolKind::VbTable;
                case 'C': return MemPDB::SymbolKind::StringLiteral;
                case 'R': return MemPDB::SymbolKind::Rtti;
                default:  return MemPDB::SymbolKind::Unknown;
                }
            }
            if (sz >= 1 && n[0] == '?')
            {
                // ?name@@3<type><cv>: the final char is the variable's own cv
                // class (A none, B const, C volatile, D const volatile).
                const char last = n[sz - 1];
                if (last == 'B' || last == 'D') return MemPDB::SymbolKind::Constant;
                return MemPDB::SymbolKind::Variable;
            }
            return MemPDB::SymbolKind::Variable; // undecorated / C symbol
        }

        uint64_t ReadLFNumeric(BinaryReader& r)
        {
            const uint16_t tag = r.Peek<uint16_t>();
            if (tag < kLF_NUMERIC_START)
            {
                r.Read<uint16_t>();
                return tag;
            }
            r.Read<uint16_t>();
            switch (tag)
            {
            case 0x8000: return static_cast<uint64_t>(static_cast<int8_t>(r.Read<uint8_t>()));
            case 0x8001: return static_cast<uint64_t>(r.Read<uint16_t>());
            case 0x8002: return static_cast<uint64_t>(r.Read<uint16_t>());
            case 0x8003: return static_cast<uint64_t>(r.Read<uint32_t>());
            case 0x8004: return static_cast<uint64_t>(r.Read<uint32_t>());
            case 0x8005: { r.Skip(4);  return 0; }
            case 0x8006: { r.Skip(8);  return 0; }
            case 0x8007: { r.Skip(10); return 0; }
            case 0x8008: { r.Skip(16); return 0; }
            case 0x8009: return static_cast<uint64_t>(r.Read<int64_t>());
            case 0x800A: return r.Read<uint64_t>();
            default:     return 0;
            }
        }

        struct SectionTable
        {
            std::vector<uint32_t> rvaBase;

            uint32_t Resolve(uint16_t seg, uint32_t off) const noexcept
            {
                if (seg == 0 || seg > rvaBase.size()) return 0;
                return rvaBase[seg - 1] + off;
            }
        };

        SectionTable ParseSectionHeaders(std::span<const std::byte> stream)
        {
            SectionTable tbl;
            if (stream.empty()) return tbl;
            const std::size_t count = stream.size() / sizeof(SectionHeader);
            tbl.rvaBase.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                SectionHeader hdr;
                std::memcpy(&hdr, stream.data() + i * sizeof(SectionHeader), sizeof(hdr));
                tbl.rvaBase.push_back(hdr.VirtualAddress);
            }
            return tbl;
        }

        struct TypeRecord
        {
            uint16_t                   kind;
            std::span<const std::byte> payload;
        };

        struct TPIData
        {
            uint32_t                typeIndexBegin = 0;
            std::vector<TypeRecord> records;

            const TypeRecord* Get(uint32_t typeIdx) const noexcept
            {
                if (typeIdx < typeIndexBegin) return nullptr;
                const uint32_t off = typeIdx - typeIndexBegin;
                if (off >= records.size()) return nullptr;
                return &records[off];
            }
        };

        TPIData ParseTPI(std::span<const std::byte> stream)
        {
            TPIData result;
            if (stream.size() < sizeof(TPIStreamHeader)) return result;

            TPIStreamHeader hdr;
            std::memcpy(&hdr, stream.data(), sizeof(hdr));

            if (hdr.TypeRecordBytes == 0) return result;
            if (hdr.HeaderSize > stream.size()) return result;

            result.typeIndexBegin = hdr.TypeIndexBegin;
            const uint32_t typeCount = hdr.TypeIndexEnd > hdr.TypeIndexBegin
                ? hdr.TypeIndexEnd - hdr.TypeIndexBegin : 0;
            result.records.reserve(typeCount);

            const std::size_t bodyLen =
                std::min<std::size_t>(hdr.TypeRecordBytes, stream.size() - hdr.HeaderSize);
            BinaryReader r(stream.subspan(hdr.HeaderSize, bodyLen));

            while (!r.AtEnd() && r.Remaining() >= 4)
            {
                const uint16_t recLen = r.Read<uint16_t>();
                if (recLen < 2 || static_cast<std::size_t>(recLen - 2) > r.Remaining()) break;
                const uint16_t recKind = r.Read<uint16_t>();
                auto payload = r.ReadBytes(recLen - 2);
                result.records.push_back({ recKind, payload });
            }
            return result;
        }

        // Base name of a primitive (ignoring pointer mode in bits 8-10).
        std::string_view GetPrimitiveName(uint32_t typeIdx) noexcept
        {
            switch (typeIdx & 0xFF)
            {
            case 0x00: case 0x03: return "void";
            case 0x08: return "HRESULT";
            case 0x10: return "char";
            case 0x20: return "unsigned char";
            case 0x11: return "short";
            case 0x21: return "unsigned short";
            case 0x12: return "long";
            case 0x22: return "unsigned long";
            case 0x13: return "__int64";          // T_QUAD
            case 0x23: return "unsigned __int64";  // T_UQUAD
            case 0x30: return "bool";
            case 0x40: return "float";
            case 0x41: return "double";
            case 0x70: return "char";
            case 0x71: return "wchar_t";
            case 0x72: return "short";
            case 0x73: return "unsigned short";
            case 0x74: return "int";
            case 0x75: return "unsigned int";
            case 0x76: return "__int64";
            case 0x77: return "unsigned __int64";
            default:   return "int";
            }
        }

        // Size in bytes of a primitive type index (x64 pointer model).
        uint32_t PrimitiveSize(uint32_t t) noexcept
        {
            if (((t >> 8) & 0x7) != 0) return 8; // any pointer-mode primitive
            switch (t & 0xFF)
            {
            case 0x00: case 0x03: return 0;  // void
            case 0x10: case 0x20: case 0x30: case 0x70: return 1;
            case 0x11: case 0x21: case 0x71: case 0x72: case 0x73: return 2;
            case 0x12: case 0x22: case 0x40: case 0x74: case 0x75: return 4;
            case 0x13: case 0x23: case 0x41: case 0x76: case 0x77: return 8;
            default:   return 0;
            }
        }

        // Best-effort byte size of any TPI type (0 if it cannot be determined).
        uint32_t TypeSize(uint32_t typeIdx, const TPIData& tpi, int depth = 0) noexcept
        {
            if (depth > 8) return 0;
            if (typeIdx < 0x1000) return PrimitiveSize(typeIdx);
            const TypeRecord* rec = tpi.Get(typeIdx);
            if (!rec) return 0;
            switch (rec->kind)
            {
            case kLF_POINTER: return 8;
            case kLF_MODIFIER:
            case kLF_BITFIELD:
            {
                if (rec->payload.size() < 4) return 0;
                uint32_t ref; std::memcpy(&ref, rec->payload.data(), 4);
                return TypeSize(ref, tpi, depth + 1);
            }
            case kLF_CLASS:
            case kLF_STRUCTURE:
            {
                if (rec->payload.size() < 17) return 0;
                BinaryReader pr(rec->payload); pr.Skip(16);
                return static_cast<uint32_t>(ReadLFNumeric(pr));
            }
            case kLF_UNION:
            {
                if (rec->payload.size() < 9) return 0;
                BinaryReader pr(rec->payload); pr.Skip(8);
                return static_cast<uint32_t>(ReadLFNumeric(pr));
            }
            case kLF_ENUM: return 4;
            case kLF_ARRAY:
            {
                if (rec->payload.size() < 8) return 0;
                BinaryReader pr(rec->payload); pr.Skip(8);
                return static_cast<uint32_t>(ReadLFNumeric(pr));
            }
            default: return 0;
            }
        }

        // Renders human-readable type names, synthesizing decorations like "*"
        // and "[]" that have no literal string in the PDB. Results are cached
        // and owned here; the node-based map keeps std::string addresses stable,
        // so callers may hold string_views into them until interning copies the
        // bytes into the final arena.
        class TypeNamer
        {
        public:
            explicit TypeNamer(const TPIData& tpi) : tpi_(tpi) {}

            std::string_view Name(uint32_t typeIdx)
            {
                const auto it = cache_.find(typeIdx);
                if (it != cache_.end()) return it->second;
                std::string built = Build(typeIdx, 0);
                return cache_.emplace(typeIdx, std::move(built)).first->second;
            }

        private:
            std::string Build(uint32_t typeIdx, int depth)
            {
                if (depth > 8) return "...";
                if (typeIdx < 0x1000)
                {
                    // Bits 8-10 are the pointer mode; non-zero means the
                    // primitive is itself a pointer (e.g. void* == 0x0603).
                    std::string base(GetPrimitiveName(typeIdx));
                    if (((typeIdx >> 8) & 0x7) != 0) base += "*";
                    return base;
                }

                const TypeRecord* rec = tpi_.Get(typeIdx);
                if (!rec) return "unknown";

                switch (rec->kind)
                {
                case kLF_POINTER:
                {
                    if (rec->payload.size() < 4) return "void*";
                    uint32_t ref; std::memcpy(&ref, rec->payload.data(), 4);
                    return Build(ref, depth + 1) + "*";
                }
                case kLF_MODIFIER:
                {
                    if (rec->payload.size() < 6) return "unknown";
                    uint32_t ref;  std::memcpy(&ref,  rec->payload.data(),     4);
                    uint16_t attr; std::memcpy(&attr, rec->payload.data() + 4, 2);
                    std::string s = Build(ref, depth + 1);
                    if (attr & 0x2) s = "volatile " + s;
                    if (attr & 0x1) s = "const " + s;
                    return s;
                }
                case kLF_ARRAY:
                {
                    if (rec->payload.size() < 8) return "unknown[]";
                    uint32_t elem; std::memcpy(&elem, rec->payload.data(), 4);
                    return Build(elem, depth + 1) + "[]";
                }
                case kLF_BITFIELD:
                {
                    if (rec->payload.size() < 4) return "unknown";
                    uint32_t ref; std::memcpy(&ref, rec->payload.data(), 4);
                    return Build(ref, depth + 1);
                }
                case kLF_CLASS:
                case kLF_STRUCTURE:
                {
                    if (rec->payload.size() < 17) return "struct";
                    BinaryReader pr(rec->payload); pr.Skip(16); ReadLFNumeric(pr);
                    return std::string(pr.ReadCStringSafe());
                }
                case kLF_UNION:
                {
                    if (rec->payload.size() < 9) return "union";
                    BinaryReader pr(rec->payload); pr.Skip(8); ReadLFNumeric(pr);
                    return std::string(pr.ReadCStringSafe());
                }
                case kLF_ENUM:
                {
                    if (rec->payload.size() < 13) return "enum";
                    BinaryReader pr(rec->payload); pr.Skip(12);
                    return std::string(pr.ReadCStringSafe());
                }
                default:
                    return "unknown";
                }
            }

            const TPIData&                            tpi_;
            std::unordered_map<uint32_t, std::string> cache_;
        };

        // Walk an LF_FIELDLIST (chasing LF_INDEX continuations), appending each
        // data member to `out`. Non-member sub-leaves are skipped by their exact
        // size so the cursor stays in sync; an unknown leaf stops the walk.
        void CollectFields(uint32_t fieldListIdx, const TPIData& tpi, TypeNamer& namer,
                           std::vector<Field>& out, int depth)
        {
            if (depth > 8) return;
            const TypeRecord* fl = tpi.Get(fieldListIdx);
            if (!fl || fl->kind != kLF_FIELDLIST) return;

            BinaryReader r(fl->payload);
            while (r.Remaining() >= 2)
            {
                // Inter-record padding: a lead byte >= 0xF0 means "skip (low
                // nibble) bytes total, including this one".
                const uint8_t lead = static_cast<uint8_t>(r.Peek<uint8_t>());
                if (lead >= 0xF0)
                {
                    const std::size_t pad = lead & 0x0F;
                    if (pad > r.Remaining()) break;
                    r.Skip(pad);
                    continue;
                }

                const uint16_t leaf = r.Read<uint16_t>();
                switch (leaf)
                {
                case kLF_MEMBER:
                {
                    r.Skip(2);                          // attr
                    const uint32_t type = r.Read<uint32_t>();
                    const uint64_t off  = ReadLFNumeric(r);
                    const std::string_view name = r.ReadCStringSafe();
                    Field f{};
                    f.Name     = name;
                    f.TypeName = namer.Name(type);
                    f.Offset   = static_cast<uint32_t>(off);
                    f.Size     = TypeSize(type, tpi);
                    out.push_back(f);
                    break;
                }
                case kLF_INDEX:
                {
                    r.Skip(2);                          // reserved
                    const uint32_t cont = r.Read<uint32_t>();
                    CollectFields(cont, tpi, namer, out, depth + 1);
                    break;
                }
                case kLF_BCLASS:
                    r.Skip(2); r.Read<uint32_t>(); ReadLFNumeric(r); break;
                case kLF_VBCLASS:
                case kLF_IVBCLASS:
                    r.Skip(2); r.Read<uint32_t>(); r.Read<uint32_t>();
                    ReadLFNumeric(r); ReadLFNumeric(r); break;
                case kLF_ENUMERATE:
                    r.Skip(2); ReadLFNumeric(r); r.ReadCStringSafe(); break;
                case kLF_STMEMBER:
                case kLF_METHOD:
                case kLF_NESTTYPE:
                    r.Skip(2); r.Read<uint32_t>(); r.ReadCStringSafe(); break;
                case kLF_VFUNCTAB:
                case kLF_FRIENDCLS:
                    r.Skip(2); r.Read<uint32_t>(); break;
                case kLF_ONEMETHOD:
                {
                    const uint16_t attr = r.Read<uint16_t>();
                    r.Read<uint32_t>();                 // type
                    const uint16_t mprop = (attr >> 2) & 0x7;
                    if (mprop == 4 || mprop == 6) r.Read<uint32_t>(); // intro virtual
                    r.ReadCStringSafe();
                    break;
                }
                default:
                    return; // unrecognized sub-leaf -> can't stay in sync
                }
            }
        }

        struct CollectedStruct
        {
            std::string_view   name;
            uint32_t           size;
            std::vector<Field> fields;
        };

        // Scan every TPI record for struct/class/union definitions (skipping
        // forward references) and decode each one's member layout.
        std::vector<CollectedStruct> CollectStructs(const TPIData& tpi, TypeNamer& namer)
        {
            std::vector<CollectedStruct> out;
            for (const TypeRecord& rec : tpi.records)
            {
                const bool isCU    = (rec.kind == kLF_CLASS || rec.kind == kLF_STRUCTURE);
                const bool isUnion = (rec.kind == kLF_UNION);
                if (!isCU && !isUnion) continue;

                try
                {
                    BinaryReader pr(rec.payload);
                    if (pr.Remaining() < 8) continue;
                    pr.Skip(2);                                   // count
                    const uint16_t property  = pr.Read<uint16_t>();
                    if (property & kCV_PROP_FWDREF) continue;     // declaration only
                    const uint32_t fieldList = pr.Read<uint32_t>();
                    if (isCU) { pr.Read<uint32_t>(); pr.Read<uint32_t>(); } // derived, vshape
                    const uint64_t size      = ReadLFNumeric(pr);
                    const std::string_view name = pr.ReadCStringSafe();
                    if (fieldList == 0 || name.empty()) continue;

                    CollectedStruct cs;
                    cs.name = name;
                    cs.size = static_cast<uint32_t>(size);
                    CollectFields(fieldList, tpi, namer, cs.fields, 0);
                    out.push_back(std::move(cs));
                }
                catch (const Error&) { /* skip a malformed type record */ }
            }
            return out;
        }

        struct ProcArgInfo
        {
            uint32_t paramCount;
            uint32_t argListTypeIdx;
        };

        std::optional<ProcArgInfo> GetProcArgInfo(uint32_t typeIdx,
                                                   const TPIData& tpi) noexcept
        {
            const TypeRecord* rec = tpi.Get(typeIdx);
            if (!rec) return std::nullopt;

            BinaryReader pr(rec->payload);

            if (rec->kind == kLF_PROCEDURE)
            {
                if (rec->payload.size() < 12) return std::nullopt;
                pr.Skip(4);
                pr.Skip(1);
                pr.Skip(1);
                const uint16_t paramCount = pr.Read<uint16_t>();
                const uint32_t argList    = pr.Read<uint32_t>();
                return ProcArgInfo{ paramCount, argList };
            }
            if (rec->kind == kLF_MFUNCTION)
            {
                if (rec->payload.size() < 24) return std::nullopt;
                pr.Skip(4);
                pr.Skip(4);
                pr.Skip(4);
                pr.Skip(1);
                pr.Skip(1);
                const uint16_t paramCount = pr.Read<uint16_t>();
                const uint32_t argList    = pr.Read<uint32_t>();
                return ProcArgInfo{ paramCount, argList };
            }
            return std::nullopt;
        }

        std::vector<uint32_t> GetArgTypeList(uint32_t typeIdx,
                                              const TPIData& tpi)
        {
            const TypeRecord* rec = tpi.Get(typeIdx);
            if (!rec || rec->kind != kLF_ARGLIST) return {};
            if (rec->payload.size() < 4) return {};

            BinaryReader pr(rec->payload);
            const uint32_t count = pr.Read<uint32_t>();

            std::vector<uint32_t> types;
            types.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                if (pr.Remaining() < 4) break;
                types.push_back(pr.Read<uint32_t>());
            }
            return types;
        }

        template<typename Callback>
        void IterateSymbolRecords(std::span<const std::byte> data, Callback&& callback)
        {
            BinaryReader r(data);
            while (!r.AtEnd() && r.Remaining() >= 4)
            {
                const uint16_t recLen  = r.Read<uint16_t>();
                if (recLen < 2) break;
                const uint16_t recType = r.Read<uint16_t>();
                const uint16_t payloadLen = static_cast<uint16_t>(recLen - 2);
                if (payloadLen > r.Remaining()) break;
                auto payload = r.ReadBytes(payloadLen);
                callback(recType, payload);
            }
        }

        struct ModuleArg
        {
            std::string_view argName;
            uint32_t         offset;
        };

        // A proc plus the params/locals seen while it was the active scope.
        // Args are attached during the single pass, so no name re-matching is
        // needed afterwards (the old code paired them through a hash map).
        struct ModuleProc
        {
            std::string_view        name;
            uint32_t                typeIdx;
            uint32_t                procLen;
            bool                    isIdType;   // typeIdx indexes IPI, not TPI
            std::vector<ModuleArg>  args;
        };

        struct ModuleSymData
        {
            std::vector<ModuleProc> procs;
        };

        ModuleSymData ParseModuleSymStream(std::span<const std::byte> stream)
        {
            ModuleSymData out;
            if (stream.size() < 4) return out;

            const auto body = stream.subspan(4);
            int32_t     nestDepth = 0;
            std::size_t curProc   = static_cast<std::size_t>(-1);

            IterateSymbolRecords(body,
                [&](uint16_t type, std::span<const std::byte> payload)
                {
                    if (type == kS_GPROC32 || type == kS_LPROC32 ||
                        type == kS_GPROC32_ID || type == kS_LPROC32_ID)
                    {
                        if (payload.size() < sizeof(ProcSym32Payload) + 1) return;
                        ProcSym32Payload p;
                        std::memcpy(&p, payload.data(), sizeof(p));

                        const char* namePtr =
                            reinterpret_cast<const char*>(payload.data() + sizeof(p));
                        const std::size_t nameLen = strnlen(namePtr, payload.size() - sizeof(p));

                        const bool isId = (type == kS_GPROC32_ID || type == kS_LPROC32_ID);
                        curProc   = out.procs.size();
                        nestDepth = 1;
                        out.procs.push_back(ModuleProc{
                            std::string_view(namePtr, nameLen), p.TypeIndex, p.ProcLen, isId, {} });
                        return;
                    }

                    if (type == kS_END || type == kS_ENDPROC)
                    {
                        if (nestDepth > 0 && --nestDepth == 0)
                            curProc = static_cast<std::size_t>(-1);
                        return;
                    }

                    if (nestDepth <= 0 || curProc == static_cast<std::size_t>(-1)) return;

                    if (type == kS_REGREL32)
                    {
                        if (payload.size() < sizeof(RegRelSym32Payload) + 1) return;
                        RegRelSym32Payload p;
                        std::memcpy(&p, payload.data(), sizeof(p));
                        const char* namePtr =
                            reinterpret_cast<const char*>(payload.data() + sizeof(p));
                        const std::size_t nameLen = strnlen(namePtr, payload.size() - sizeof(p));
                        out.procs[curProc].args.push_back(
                            { std::string_view(namePtr, nameLen), p.Offset });
                        return;
                    }

                    if (type == kS_FRAMEREL)
                    {
                        if (payload.size() < sizeof(FrameRelSymPayload) + 1) return;
                        FrameRelSymPayload p;
                        std::memcpy(&p, payload.data(), sizeof(p));
                        const char* namePtr =
                            reinterpret_cast<const char*>(payload.data() + sizeof(p));
                        const std::size_t nameLen = strnlen(namePtr, payload.size() - sizeof(p));
                        out.procs[curProc].args.push_back(
                            { std::string_view(namePtr, nameLen), p.Offset });
                    }
                });

            return out;
        }

        struct ModuleStreamRef
        {
            uint16_t streamIndex;
            uint32_t symByteSize; // bytes of S_* records at the front of the stream
        };

        std::vector<ModuleStreamRef> ParseModuleStreamIndices(std::span<const std::byte> modInfoData)
        {
            std::vector<ModuleStreamRef> refs;
            BinaryReader r(modInfoData);

            // Each entry is a fixed struct followed by two NUL-terminated names,
            // padded to 4 bytes. Skipping the names with memchr beats the
            // byte-at-a-time Peek loop (one bounds check + memcpy per byte).
            const auto* base = reinterpret_cast<const char*>(modInfoData.data());

            while (!r.AtEnd())
            {
                if (r.Remaining() < sizeof(ModuleInfoEntry)) break;

                const std::size_t entryStart = r.Offset();
                ModuleInfoEntry entry;
                std::memcpy(&entry, modInfoData.data() + entryStart, sizeof(entry));
                r.Skip(sizeof(entry));

                if (entry.ModuleSymStream != 0xFFFF)
                    refs.push_back({ entry.ModuleSymStream, entry.SymByteSize });

                for (int names = 0; names < 2 && !r.AtEnd(); ++names)
                {
                    const std::size_t pos = r.Offset();
                    const std::size_t rem = r.View().size() - pos;
                    const void* nul = std::memchr(base + pos, '\0', rem);
                    const std::size_t len = nul
                        ? static_cast<std::size_t>(static_cast<const char*>(nul) - (base + pos))
                        : rem;
                    r.Skip(len);
                    if (!r.AtEnd()) r.Skip(1); // consume the terminator
                }

                const std::size_t pos     = r.Offset();
                const std::size_t aligned = (pos + 3) & ~std::size_t(3);
                if (aligned > r.View().size()) break;
                r.Seek(aligned);
            }
            return refs;
        }
    }

    ParseResult ParsePDB(std::span<const std::byte> rawPDB, const MemPDB::ParseOptions& opts)
    {
        // Sizes/arguments are properties of functions, so they require the
        // function table; skip the module phase entirely when functions are off.
        const bool wantModules = opts.ResolveFunctions &&
                                 (opts.ResolveSizes || opts.ResolveArguments);
        const bool wantTPI     = opts.ResolveArguments || opts.ResolveStructs;
        const bool wantIPI     = wantModules && opts.ResolveArguments; // id->type indirection
        const bool wantPubSym  = opts.ResolveFunctions || opts.ResolveGlobals;

        const auto tTotal0 = Clock::now();

        // ---- Phase 1: MSF + DBI + section headers ----
        const auto tMSF0 = Clock::now();

        MSFReader msf(rawPDB);
        if (msf.StreamCount() < 4)
            throw Error("PDB: too few streams");

        ParseResult result;

        auto getStream = [&](uint32_t idx) -> std::span<const std::byte>
        {
            if (idx >= msf.StreamCount()) return {};
            if (auto view = msf.TryGetStreamView(idx)) return *view;
            auto data = msf.GetStream(idx);
            if (data.empty()) return {};
            result.streamStorage.push_back(std::move(data));
            return result.streamStorage.back();
        };

        const auto dbiData = getStream(3);
        if (dbiData.size() < sizeof(DBIStreamHeader))
            throw Error("PDB: DBI stream too small");

        DBIStreamHeader dbi;
        std::memcpy(&dbi, dbiData.data(), sizeof(dbi));
        if (dbi.VersionSignature != -1)
            throw Error("PDB: invalid DBI signature");

        uint16_t sectionHdrStream = 0xFFFF;
        {
            const std::size_t optHdrOff =
                sizeof(DBIStreamHeader) +
                static_cast<std::size_t>(dbi.ModInfoSize) +
                static_cast<std::size_t>(dbi.SectionContributionSize) +
                static_cast<std::size_t>(dbi.SectionMapSize) +
                static_cast<std::size_t>(dbi.SourceInfoSize) +
                static_cast<std::size_t>(dbi.TypeServerMapSize) +
                static_cast<std::size_t>(dbi.ECSubstreamSize);

            if (dbi.OptionalDbgHeaderSize >= static_cast<int32_t>(sizeof(uint16_t) * 6) &&
                optHdrOff + sizeof(uint16_t) * 6 <= dbiData.size())
            {
                std::memcpy(&sectionHdrStream,
                            dbiData.data() + optHdrOff + sizeof(uint16_t) * 5,
                            sizeof(sectionHdrStream));
            }
        }

        SectionTable sections;
        if (sectionHdrStream != 0xFFFF && sectionHdrStream < msf.StreamCount())
            sections = ParseSectionHeaders(getStream(sectionHdrStream));

        const auto tMSF1 = Clock::now();

        // ---- Phase 2: TPI + IPI (only needed for argument types) ----
        // IPI (stream 4) shares the TPI framing; modern MSVC proc records are
        // S_GPROC32_ID whose "type index" points at an LF_FUNC_ID in the IPI
        // that in turn names the real LF_PROCEDURE in the TPI.
        const auto tTPI0 = Clock::now();
        const TPIData tpi = wantTPI ? ParseTPI(getStream(2)) : TPIData{};
        const TPIData ipi = wantIPI ? ParseTPI(getStream(4)) : TPIData{};
        const auto tTPI1 = Clock::now();
        result.timing.typeCount = tpi.records.size();

        // Shared type-name renderer (pointers, arrays, const, tag names). Lives
        // across the argument and struct phases; its cached strings stay valid
        // until interning copies them into the arena.
        TypeNamer namer(tpi);

        // ---- Phase 3: Public symbols -> function + global tables (fused) ----
        // Each S_PUB32 record carries a PublicSymFlags field:
        //   fFunction (0x2) set   -> callable function -> function table
        //   fFunction (0x2) clear -> data symbol       -> global table
        // Everything goes through the same scan; the flag decides the bucket.
        const auto tSym0 = Clock::now();
        std::vector<FunctionRecord> functions;
        std::vector<GlobalRecord>   globals;
        if (wantPubSym)
        {
            const auto symData = getStream(dbi.SymRecordStream);
            if (opts.ResolveFunctions) functions.reserve(symData.size() / 40);
            if (opts.ResolveGlobals)   globals.reserve(symData.size() / 40);
            std::size_t pub32Seen = 0;
            IterateSymbolRecords(symData,
                [&](uint16_t type, std::span<const std::byte> payload)
                {
                    if (type != kS_PUB32) return;
                    if (payload.size() < sizeof(PublicSym32Payload) + 1) return;
                    ++pub32Seen;

                    PublicSym32Payload sym;
                    std::memcpy(&sym, payload.data(), sizeof(sym));

                    const uint32_t rva = sections.Resolve(sym.Segment, sym.Offset);
                    if (rva == 0) return;

                    const char* namePtr =
                        reinterpret_cast<const char*>(payload.data() + sizeof(sym));
                    const std::size_t nameLen = strnlen(namePtr, payload.size() - sizeof(sym));
                    const std::string_view name(namePtr, nameLen);

                    if (sym.PublicSymFlags & kPubSymFlag_Function)
                    {
                        if (opts.ResolveFunctions)
                            functions.push_back({ name, rva, 0, 0, 0 });
                    }
                    else if (opts.ResolveGlobals)
                    {
                        const MemPDB::SymbolKind kind = ClassifyGlobal(name);
                        if (opts.GlobalKindMask & KindBit(kind))
                        {
                            globals.push_back({ name, rva, kind });
                            ++result.globalKindCounts[static_cast<unsigned>(kind)];
                        }
                    }
                });
            result.timing.symbolCount = pub32Seen;
        }
        const auto tSym1 = Clock::now();

        // ---- Phase 4: Sort (enables binary-search lookup) ----
        // Naively sorting by string_view does ~n*log2(n) memcmp calls, each
        // chasing a pointer into scattered raw-PDB memory. Instead sort an
        // index array keyed on an 8-byte big-endian prefix of each name: that
        // makes integer order match lexicographic order, so the vast majority
        // of comparisons are a single register compare. A full string compare
        // is only needed to break prefix ties. Then apply the permutation once.
        const auto tSort0 = Clock::now();
        // Sort helper: in-place prefix-key sort for any Record vector with a .name field.
        auto prefixSort = [&](auto& vec)
        {
            if (vec.empty()) return;
            using Rec = typename std::remove_reference_t<decltype(vec)>::value_type;
            const std::size_t n = vec.size();
            std::vector<std::pair<uint64_t, uint32_t>> keys(n);
            for (std::size_t i = 0; i < n; ++i)
                keys[i] = { NamePrefix8(vec[i].name), static_cast<uint32_t>(i) };
            std::sort(keys.begin(), keys.end(),
                [&](const std::pair<uint64_t, uint32_t>& a,
                    const std::pair<uint64_t, uint32_t>& b)
                {
                    if (a.first != b.first) return a.first < b.first;
                    return vec[a.second].name < vec[b.second].name;
                });
            std::vector<Rec> sorted;
            sorted.reserve(n);
            for (const auto& k : keys) sorted.push_back(vec[k.second]);
            vec = std::move(sorted);
        };

        prefixSort(functions);
        prefixSort(globals);
        const auto tSort1 = Clock::now();

        std::vector<Argument> allArgs;

        // ---- Phase 5: Module streams (sizes + arguments) ----
        const auto tMod0 = Clock::now();
        if (wantModules)
        {
            const std::span<const std::byte> modInfoData =
                dbiData.size() > sizeof(DBIStreamHeader)
                    ? dbiData.subspan(sizeof(DBIStreamHeader),
                                      static_cast<std::size_t>(std::max(0, dbi.ModInfoSize)))
                    : std::span<const std::byte>{};

            const auto modRefs = ParseModuleStreamIndices(modInfoData);
            result.timing.moduleCount = modRefs.size();

            // Gather stream spans single-threaded (getStream mutates state),
            // then decode them in parallel (each stream is independent).
            // A module stream is [u32 sig][symbols][C11 lines][C13 lines][refs];
            // only the SymByteSize prefix holds S_* records, so bound the span
            // to it. This avoids grinding megabytes of line-info as if it were
            // symbol records -- a large, correctness-preserving speedup.
            std::vector<std::span<const std::byte>> modSpans;
            modSpans.reserve(modRefs.size());
            for (const auto& ref : modRefs)
            {
                if (ref.streamIndex >= msf.StreamCount()) continue;
                const auto modData = getStream(ref.streamIndex);
                if (modData.empty()) continue;
                const std::size_t symBytes =
                    (ref.symByteSize >= 4 && ref.symByteSize <= modData.size())
                        ? ref.symByteSize : modData.size();
                modSpans.push_back(modData.first(symBytes));
            }
            // Spawning a thread per core costs ~0.5 ms on Windows, which only
            // amortizes once there is enough symbol data to decode. Small PDBs
            // (a 1-2 MB ntdll decodes its module streams in well under a ms) are
            // measurably faster serial, so gate parallelism on total volume.
            std::size_t modBytes = 0;
            for (const auto& s : modSpans) modBytes += s.size();
            result.timing.moduleBytes = modBytes;
            constexpr std::size_t kParallelByteThreshold = 6u * 1024 * 1024;
            const bool goParallel = opts.Parallel && modBytes >= kParallelByteThreshold;

            std::vector<ModuleSymData> parsedMods(modSpans.size());
            ParallelFor(modSpans.size(), goParallel,
                        [&](std::size_t i) { parsedMods[i] = ParseModuleSymStream(modSpans[i]); });

            // Merge single-threaded: bind procs to function indices, fill sizes
            // and (optionally) arguments. No per-function hashing: we index the
            // sorted function table directly by binary-search position.
            std::vector<std::vector<Argument>> argsByIdx;
            if (opts.ResolveArguments) argsByIdx.resize(functions.size());

            for (const auto& mod : parsedMods)
            {
                for (const auto& proc : mod.procs)
                {
                    ++result.timing.procsParsed;
                    const auto lo = std::lower_bound(
                        functions.begin(), functions.end(), proc.name,
                        [](const FunctionRecord& f, std::string_view n) { return f.name < n; });
                    if (lo == functions.end() || lo->name != proc.name) continue;
                    const std::size_t idx = static_cast<std::size_t>(lo - functions.begin());
                    ++result.timing.procMatches;

                    if (opts.ResolveSizes && proc.procLen > 0)
                        functions[idx].size = proc.procLen;

                    if (!opts.ResolveArguments || proc.typeIdx == 0) continue;

                    auto& dst = argsByIdx[idx];
                    if (!dst.empty()) continue; // first definition wins

                    // For *_ID procs the type index points into the IPI at an
                    // LF_FUNC_ID/LF_MFUNC_ID whose 2nd dword is the real TPI type.
                    uint32_t procType = proc.typeIdx;
                    if (proc.isIdType)
                    {
                        const TypeRecord* idr = ipi.Get(proc.typeIdx);
                        if (idr && (idr->kind == kLF_FUNC_ID || idr->kind == kLF_MFUNC_ID) &&
                            idr->payload.size() >= 8)
                            std::memcpy(&procType, idr->payload.data() + 4, 4);
                        else
                            continue;
                    }

                    const auto argInfo = GetProcArgInfo(procType, tpi);
                    if (!argInfo) continue;

                    const auto typeIndices = GetArgTypeList(argInfo->argListTypeIdx, tpi);
                    dst.reserve(typeIndices.size());
                    for (const uint32_t ti : typeIndices)
                    {
                        Argument arg{};
                        arg.TypeName = namer.Name(ti);
                        dst.push_back(arg);
                    }

                    // Param names/offsets are positional with the arg-list types.
                    const std::size_t n = std::min(proc.args.size(), dst.size());
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        dst[i].Name   = proc.args[i].argName;
                        dst[i].Offset = proc.args[i].offset;
                    }
                }
            }

            if (opts.ResolveArguments)
            {
                std::size_t total = 0;
                for (const auto& a : argsByIdx) total += a.size();
                allArgs.reserve(total);
                for (std::size_t i = 0; i < functions.size(); ++i)
                {
                    auto& a = argsByIdx[i];
                    if (a.empty()) continue;
                    functions[i].argOffset = static_cast<uint32_t>(allArgs.size());
                    functions[i].argCount  = static_cast<uint32_t>(a.size());
                    for (auto& x : a) allArgs.push_back(std::move(x));
                }
            }
        }
        const auto tMod1 = Clock::now();

        // ---- Phase 5b: Struct/class/union layouts (from TPI) ----
        const auto tStruct0 = Clock::now();
        std::vector<StructRecord> structRecs;
        std::vector<Field>        allFields;
        if (opts.ResolveStructs && !tpi.records.empty())
        {
            auto collected = CollectStructs(tpi, namer);

            // Name-sort for binary-search lookup (same prefix-key trick as the
            // function table).
            std::sort(collected.begin(), collected.end(),
                [](const CollectedStruct& a, const CollectedStruct& b)
                {
                    const uint64_t ka = NamePrefix8(a.name), kb = NamePrefix8(b.name);
                    if (ka != kb) return ka < kb;
                    return a.name < b.name;
                });

            std::size_t totalFields = 0;
            for (const auto& c : collected) totalFields += c.fields.size();
            structRecs.reserve(collected.size());
            allFields.reserve(totalFields);

            for (auto& c : collected)
            {
                StructRecord sr{};
                sr.name        = c.name;
                sr.size        = c.size;
                sr.fieldOffset = static_cast<uint32_t>(allFields.size());
                sr.fieldCount  = static_cast<uint32_t>(c.fields.size());
                for (auto& f : c.fields) allFields.push_back(f);
                structRecs.push_back(sr);
            }
            result.timing.structCount = structRecs.size();
        }
        const auto tStruct1 = Clock::now();

        // ---- Phase 6: Strings ----
        // Synthesized type names ("Foo*", "char[]") have no backing bytes in the
        // PDB, so they MUST be copied into our arena regardless of InternStrings
        // -- otherwise they would dangle once `namer` is destroyed. When
        // InternStrings is also set we copy the PDB-resident names too (function/
        // argument/field/struct) and release the raw buffers.
        const auto tIntern0 = Clock::now();
        {
            std::vector<char>& arena = result.stringArena;

            std::size_t bound = 0;
            for (const auto& a : allArgs)    bound += a.TypeName.size();
            for (const auto& f : allFields)  bound += f.TypeName.size();
            if (opts.InternStrings)
            {
                for (const auto& f : functions)  bound += f.name.size();
                for (const auto& g : globals)    bound += g.name.size();
                for (const auto& a : allArgs)    bound += a.Name.size();
                for (const auto& s : structRecs) bound += s.name.size();
                for (const auto& f : allFields)  bound += f.Name.size();
            }
            arena.reserve(bound); // upper bound (dedup only shrinks) -> data() stable

            std::unordered_map<std::string_view, uint32_t> typeInterned;
            auto internType = [&](std::string_view& sv)   // dedups repeats
            {
                if (sv.empty()) return;
                const auto it = typeInterned.find(sv);
                uint32_t off;
                if (it != typeInterned.end())
                {
                    off = it->second;
                }
                else
                {
                    off = static_cast<uint32_t>(arena.size());
                    arena.insert(arena.end(), sv.begin(), sv.end());
                    typeInterned.emplace(sv, off); // key points into source (stable)
                }
                sv = std::string_view(arena.data() + off, sv.size());
            };
            auto append = [&](std::string_view& sv)        // unique strings
            {
                if (sv.empty()) return;
                const uint32_t off = static_cast<uint32_t>(arena.size());
                arena.insert(arena.end(), sv.begin(), sv.end());
                sv = std::string_view(arena.data() + off, sv.size());
            };

            // Always: the synthesized, dedup-friendly type names.
            for (auto& a : allArgs)   internType(a.TypeName);
            for (auto& f : allFields) internType(f.TypeName);

            if (opts.InternStrings)
            {
                for (auto& f : functions)  append(f.name);
                for (auto& g : globals)    append(g.name);
                for (auto& a : allArgs)    append(a.Name);
                for (auto& s : structRecs) append(s.name);
                for (auto& f : allFields)  append(f.Name);

                result.internedStrings = true;
                result.streamStorage.clear();
                result.streamStorage.shrink_to_fit();
            }
        }
        const auto tIntern1 = Clock::now();

        // ---- Collect timing ----
        const auto tTotal1 = Clock::now();
        result.timing.msMSFDBI        = ElapsedMs(tMSF0,    tMSF1);
        result.timing.msTPI           = ElapsedMs(tTPI0,    tTPI1);
        result.timing.msPublicSymbols = ElapsedMs(tSym0,    tSym1);
        result.timing.msSort          = ElapsedMs(tSort0,   tSort1);
        result.timing.msModuleStreams = ElapsedMs(tMod0,    tMod1);
        result.timing.msStructs       = ElapsedMs(tStruct0, tStruct1);
        result.timing.msIntern        = ElapsedMs(tIntern0, tIntern1);
        result.timing.msTotal         = ElapsedMs(tTotal0,  tTotal1);

        result.functions = std::move(functions);
        result.arguments = std::move(allArgs);
        result.globals   = std::move(globals);
        result.structs   = std::move(structRecs);
        result.fields    = std::move(allFields);
        return result;
    }
}
