// Synthetic, metadata-only coverage for modern CodeView parameter locations:
// S_LOCAL parameter records and their S_DEFRANGE ranges, surfaced through the
// existing Function/Argument views. No symbol server, process, or target code
// is touched: every image is built byte-by-byte in task_support/image.h.
#include <MemPDB/MemPDB.hpp>
// task_support headers are found via a relative path from tests/.
#include "../task_support/image.h"
#include "../task_support/check.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace
{
    using namespace fixture;
    using namespace MemPDB::detail;
    using MemPDB::Argument;
    using MemPDB::ArgumentLocation;
    using MemPDB::ArgumentLocationKind;
    using MemPDB::Function;
    using MemPDB::ParseOptions;
    using MemPDB::PDB;

    constexpr uint16_t kParam    = 0x0001;
    constexpr uint16_t kOptimized = 0x0100;

    // ---- small fixture helpers ---------------------------------------------

    Bytes argList(int count) { return argListPayload(count); }

    struct ImageConfig
    {
        std::vector<LocalSpec> locals;
        std::vector<uint32_t>  argTypes;
        int                    argCount = 0;
        uint32_t               procLen  = 0x40;
        uint32_t               procOff  = 0x20;
        bool                   fragmented = false;
        uint32_t               blockSize  = 4096;
        bool                   rich       = false; // add global + struct
    };

    // One .text section at RVA 0x1000; one module with one procedure "f".
    Bytes singleProcImage(const ImageConfig& cfg)
    {
        Builder b;
        b.sections.push_back({ ".text", 0x1000, 0x400, 0x60000020 });
        if (cfg.argTypes.empty())
            b.argLists.push_back(argList(cfg.argCount));
        else
            b.argLists.push_back(argListPayload(
                std::span<const uint32_t>(cfg.argTypes)));

        ModuleSpec m;
        m.name = "one.obj"; m.section = 1; m.offset = cfg.procOff;
        m.size = cfg.procLen;

        ProcSpec p;
        p.name = "f"; p.typeIdx = 0; p.procLen = cfg.procLen;
        p.segment = 1; p.offset = cfg.procOff;
        p.locals = cfg.locals;
        m.procs.push_back(p);
        b.modules.push_back(m);

        b.publics.push_back({ "f", 1, cfg.procOff, 2 });

        if (cfg.rich)
        {
            b.publics.push_back({ "?soloGlobal@@3HA", 1, 0x200, 0 });

            // LF_FIELDLIST { int x; } at 0x3000, then LF_CLASS "Solo" at 0x3001.
            Bytes fields;
            put(fields, uint16_t(0x150D)); put(fields, uint16_t(0)); // LF_MEMBER, attr
            put(fields, uint32_t(0x74));                              // int
            put(fields, uint16_t(0));                                 // offset 0
            text(fields, "x");
            const uint32_t extraBase = 0x1000 + 2u * uint32_t(b.argLists.size());
            b.extraTypes.push_back({ 0x1203, fields });               // LF_FIELDLIST

            Bytes st;
            put(st, uint16_t(1));                 // member count
            put(st, uint16_t(0));                 // property
            put(st, uint32_t(extraBase));         // field list index
            put(st, uint32_t(0));                 // derived
            put(st, uint32_t(0));                 // vshape
            put(st, uint16_t(0x8003)); put(st, uint32_t(4)); // size = 4
            text(st, "Solo");
            b.extraTypes.push_back({ 0x1504, st });                   // LF_CLASS
        }
        return b.build(cfg.fragmented, cfg.blockSize);
    }

    bool LocIs(const ArgumentLocation& l, uint32_t begin, uint32_t end,
               ArgumentLocationKind kind, uint16_t reg, int32_t off)
    {
        return l.BeginRVA == begin && l.EndRVA == end && l.Kind == kind &&
               l.Register == reg && l.Offset == off;
    }

    bool SameSpans(std::span<const ArgumentLocation> a,
                   std::span<const ArgumentLocation> b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!LocIs(a[i], b[i].BeginRVA, b[i].EndRVA, b[i].Kind,
                       b[i].Register, b[i].Offset))
                return false;
        return true;
    }

    Function FindByRVA(const PDB& pdb, uint32_t rva)
    {
        for (std::size_t i = 0; i < pdb.FunctionCount(); ++i)
        {
            const Function f = pdb.FunctionAt(i);
            if (f.RVA == rva) return f;
        }
        return Function{};
    }

    // ======================================================================
    // scope: which S_LOCALs become formal parameters, in which slot
    // ======================================================================

    int case_modern_order()
    {
        ImageConfig cfg;
        cfg.argCount = 2;
        cfg.argTypes = { 0x74, 0x75 };
        LocalSpec p0;
        p0.typeIdx = 0x74; p0.flags = kParam; p0.name = "alpha";
        p0.ranges.push_back({ 0x1141, defRangeRegister(17, { 1, 0x20, 0x10 }) });
        LocalSpec p1;
        p1.typeIdx = 0x75; p1.flags = kParam; p1.name = "beta";
        p1.ranges.push_back({ 0x1142, defRangeFramePointerRel(-16, { 1, 0x30, 0x10 }) });
        cfg.locals = { p0, p1 };

        auto pdb = PDB::ParseFromMemory(singleProcImage(cfg));
        const Function f = pdb.ResolveFunction("f");
        TASK_CHECK(f.ArgumentCount == 2);
        TASK_CHECK(f.Arguments.size() == 2);
        TASK_CHECK(f.Arguments[0].Name == "alpha");
        TASK_CHECK(f.Arguments[0].TypeName == "int");
        TASK_CHECK(f.Arguments[0].Locations.size() == 1);
        TASK_CHECK(LocIs(f.Arguments[0].Locations[0], 0x1020, 0x1030,
                         ArgumentLocationKind::Register, 17, 0));
        TASK_CHECK(f.Arguments[1].Name == "beta");
        TASK_CHECK(f.Arguments[1].TypeName == "unsigned int");
        TASK_CHECK(f.Arguments[1].Locations.size() == 1);
        TASK_CHECK(LocIs(f.Arguments[1].Locations[0], 0x1030, 0x1040,
                         ArgumentLocationKind::FramePointerRelative, 0, -16));
        return 0;
    }

    int case_locals_interspersed()
    {
        ImageConfig cfg;
        cfg.argCount = 2;
        LocalSpec p0;
        p0.typeIdx = 0x74; p0.flags = kParam; p0.name = "p0";
        p0.ranges.push_back({ 0x1141, defRangeRegister(17, { 1, 0x20, 0x8 }) });
        LocalSpec local; // ordinary local: not a parameter, owns no slot
        local.typeIdx = 0x74; local.flags = 0; local.name = "temp";
        local.ranges.push_back({ 0x1141, defRangeRegister(19, { 1, 0x28, 0x8 }) });
        LocalSpec p1;
        p1.typeIdx = 0x75; p1.flags = kParam; p1.name = "p1";
        p1.ranges.push_back({ 0x1141, defRangeRegister(18, { 1, 0x30, 0x8 }) });
        cfg.locals = { p0, local, p1 };

        auto pdb = PDB::ParseFromMemory(singleProcImage(cfg));
        const Function f = pdb.ResolveFunction("f");
        TASK_CHECK(f.Arguments.size() == 2);
        TASK_CHECK(f.Arguments[0].Name == "p0");
        TASK_CHECK(f.Arguments[1].Name == "p1");
        // The local's register 19 must not leak into either slot.
        TASK_CHECK(f.Arguments[0].Locations.size() == 1);
        TASK_CHECK(f.Arguments[0].Locations[0].Register == 17);
        TASK_CHECK(f.Arguments[1].Locations.size() == 1);
        TASK_CHECK(f.Arguments[1].Locations[0].Register == 18);
        return 0;
    }

    int case_block_scope()
    {
        // p0 and p1 are direct-scope parameters; an intervening block declares
        // its own shadowing "p1" (not a parameter) plus a range. The block's
        // S_END must pop only the block, and its local must never take a slot
        // or inherit p1's locations.
        Builder b;
        b.sections.push_back({ ".text", 0x1000, 0x400, 0x600020 });
        b.argLists.push_back(argList(3));

        ModuleSpec m; m.name = "blocks.obj"; m.section = 1; m.offset = 0x20; m.size = 0x40;
        ProcSpec p; p.name = "f"; p.typeIdx = 0; p.procLen = 0x40;
        p.segment = 1; p.offset = 0x20;
        p.prefix.push_back({ 0x113E, localPayload(0x74, kParam, "p0") });
        p.prefix.push_back({ 0x1141, defRangeRegister(17, { 1, 0x20, 0x8 }) });
        // Block, hand-built so the block's S_END sits between p0 and p1.
        Bytes block = blockPayload(0x28, 1, 0x8, "scope");
        {
            // Append block records inside a tiny sub-stream, then splice them.
            Bytes inner;
            const size_t blockAt = record(inner, 0x1103, block);
            record(inner, 0x113E, localPayload(0x74, 0, "p1"));
            record(inner, 0x1141, defRangeRegister(19, { 1, 0x30, 0x8 }));
            const size_t blockEnd = record(inner, 0x0006, {});
            uint32_t e = uint32_t(blockEnd);
            std::memcpy(inner.data() + blockAt + 4 + 8, &e, 4); // BlockSym.End
            // Stash the raw bytes as prefix records with a placeholder kind;
            // buildModuleStream copies payloads verbatim, so emit them by hand
            // via rawSymbols instead (below).
            (void)inner;
        }

        // Build the module stream by hand: proc, p0, block(with p1 shadow),
        // then direct-scope p1 and p2.
        Bytes mod; put(mod, uint32_t(4));
        ProcSym32Payload ps{};
        ps.ProcLen = p.procLen; ps.TypeIndex = 0x1000 + uint32_t(b.argLists.size());
        ps.Offset = p.offset; ps.Segment = 1; ps.DbgEnd = p.procLen;
        Bytes pp; put(pp, ps); text(pp, "f");
        const size_t procAt = record(mod, kS_GPROC32, pp);
        record(mod, kS_LOCAL, localPayload(0x74, kParam, "p0"));
        record(mod, kS_DEFRANGE_REGISTER, defRangeRegister(17, { 1, 0x20, 0x8 }));
        const size_t blockAt = record(mod, kS_BLOCK32, blockPayload(0x28, 1, 0x8, "scope"));
        record(mod, kS_LOCAL, localPayload(0x74, 0, "p1"));
        record(mod, kS_DEFRANGE_REGISTER, defRangeRegister(19, { 1, 0x30, 0x8 }));
        const size_t blockEnd = record(mod, kS_END, {});
        uint32_t be = uint32_t(blockEnd);
        std::memcpy(mod.data() + blockAt + 4 + 8, &be, 4);
        record(mod, kS_LOCAL, localPayload(0x74, kParam, "p1"));
        record(mod, kS_DEFRANGE_REGISTER, defRangeRegister(18, { 1, 0x28, 0x8 }));
        record(mod, kS_LOCAL, localPayload(0x74, kParam, "p2"));
        record(mod, kS_DEFRANGE_REGISTER, defRangeRegister(20, { 1, 0x38, 0x8 }));
        const size_t procEnd = record(mod, kS_END, {});
        uint32_t pe = uint32_t(procEnd);
        std::memcpy(mod.data() + procAt + 4 + 4, &pe, 4);
        m.rawSymbols = std::move(mod);
        m.procs.clear();
        b.modules.push_back(m);
        b.publics.push_back({ "f", 1, 0x20, 2 });

        auto pdb = PDB::ParseFromMemory(b.build());
        const Function f = pdb.ResolveFunction("f");
        TASK_CHECK(f.Arguments.size() == 3);
        TASK_CHECK(f.Arguments[0].Name == "p0");
        TASK_CHECK(f.Arguments[1].Name == "p1");
        TASK_CHECK(f.Arguments[2].Name == "p2");
        TASK_CHECK(f.Arguments[0].Locations.size() == 1);
        TASK_CHECK(f.Arguments[0].Locations[0].Register == 17);
        // The block-shadow local (reg 19) never claims p1's slot.
        TASK_CHECK(f.Arguments[1].Locations.size() == 1);
        TASK_CHECK(f.Arguments[1].Locations[0].Register == 18);
        TASK_CHECK(f.Arguments[2].Locations.size() == 1);
        TASK_CHECK(f.Arguments[2].Locations[0].Register == 20);
        return 0;
    }

    int case_optimized_out()
    {
        ImageConfig cfg;
        cfg.argCount = 2;
        LocalSpec p0;
        p0.typeIdx = 0x74; p0.flags = kParam | kOptimized; p0.name = "gone";
        LocalSpec p1;
        p1.typeIdx = 0x75; p1.flags = kParam; p1.name = "kept";
        p1.ranges.push_back({ 0x1141, defRangeRegister(17, { 1, 0x20, 0x10 }) });
        cfg.locals = { p0, p1 };

        auto pdb = PDB::ParseFromMemory(singleProcImage(cfg));
        const Function f = pdb.ResolveFunction("f");
        TASK_CHECK(f.Arguments.size() == 2);
        TASK_CHECK(f.Arguments[0].Name == "gone");
        TASK_CHECK(f.Arguments[0].TypeName == "int");
        TASK_CHECK(f.Arguments[0].Locations.empty());
        TASK_CHECK(f.Arguments[1].Name == "kept");
        TASK_CHECK(f.Arguments[1].Locations.size() == 1);
        return 0;
    }

    int case_legacy_unchanged()
    {
        Builder b;
        b.sections.push_back({ ".text", 0x1000, 0x400, 0x600020 });
        b.argLists.push_back(argList(2));

        Bytes mod; put(mod, uint32_t(4));
        ProcSym32Payload ps{};
        ps.ProcLen = 0x40; ps.TypeIndex = 0x1000 + uint32_t(b.argLists.size());
        ps.Offset = 0x20; ps.Segment = 1; ps.DbgEnd = 0x40;
        Bytes pp; put(pp, ps); text(pp, "old");
        const size_t procAt = record(mod, kS_GPROC32, pp);
        RegRelSym32Payload rr{}; rr.Offset = 8; rr.TypeIndex = 0x74; rr.Register = 22;
        Bytes rp; put(rp, rr); text(rp, "first");
        record(mod, kS_REGREL32, rp);
        FrameRelSymPayload fr{}; fr.Offset = 0xFFFFFFF0u; fr.TypeIndex = 0x74;
        Bytes fp; put(fp, fr); text(fp, "second");
        record(mod, kS_FRAMEREL, fp);
        const size_t endAt = record(mod, kS_END, {});
        uint32_t end32 = uint32_t(endAt);
        std::memcpy(mod.data() + procAt + 4 + 4, &end32, 4);

        ModuleSpec m; m.name = "legacy.obj"; m.section = 1; m.offset = 0x20; m.size = 0x40;
        m.rawSymbols = std::move(mod);
        b.modules.push_back(m);
        b.publics.push_back({ "old", 1, 0x20, 2 });

        auto pdb = PDB::ParseFromMemory(b.build());
        const Function f = pdb.ResolveFunction("old");
        TASK_CHECK(f.Arguments.size() == 2);
        TASK_CHECK(f.Arguments[0].Name == "first");
        TASK_CHECK(f.Arguments[0].Offset == 8);
        TASK_CHECK(f.Arguments[0].Locations.empty());
        TASK_CHECK(f.Arguments[1].Name == "second");
        TASK_CHECK(f.Arguments[1].Offset == 0xFFFFFFF0u);
        TASK_CHECK(f.Arguments[1].Locations.empty());
        return 0;
    }

    int case_same_name_two_addresses()
    {
        Builder b;
        b.sections.push_back({ ".text", 0x1000, 0x400, 0x600020 });
        b.argLists.push_back(argList(1)); // dup@A: 1 arg
        b.argLists.push_back(argList(2)); // dup@B: 2 args

        ModuleSpec ma; ma.name = "a.obj"; ma.section = 1; ma.offset = 0x20; ma.size = 0x10;
        ProcSpec pa; pa.name = "dup"; pa.typeIdx = 0; pa.procLen = 0x10;
        pa.segment = 1; pa.offset = 0x20;
        LocalSpec la; la.typeIdx = 0x74; la.flags = kParam; la.name = "aOnly";
        la.ranges.push_back({ 0x1141, defRangeRegister(17, { 1, 0x20, 0x10 }) });
        pa.locals.push_back(la);
        ma.procs.push_back(pa);

        ModuleSpec mb; mb.name = "b.obj"; mb.section = 1; mb.offset = 0x60; mb.size = 0x30;
        ProcSpec pb; pb.name = "dup"; pb.typeIdx = 1; pb.procLen = 0x30;
        pb.segment = 1; pb.offset = 0x60;
        LocalSpec lb0; lb0.typeIdx = 0x74; lb0.flags = kParam; lb0.name = "bFirst";
        lb0.ranges.push_back({ 0x1141, defRangeRegister(18, { 1, 0x60, 0x10 }) });
        LocalSpec lb1; lb1.typeIdx = 0x74; lb1.flags = kParam; lb1.name = "bSecond";
        lb1.ranges.push_back({ 0x1142, defRangeFramePointerRel(-8, { 1, 0x70, 0x10 }) });
        pb.locals = { lb0, lb1 };
        mb.procs.push_back(pb);

        b.modules = { ma, mb };
        b.publics.push_back({ "dup", 1, 0x20, 2 });
        b.publics.push_back({ "dup", 1, 0x60, 2 });

        auto pdb = PDB::ParseFromMemory(b.build());
        TASK_CHECK(pdb.FunctionCount() == 2);

        const Function atA = FindByRVA(pdb, 0x1020);
        const Function atB = FindByRVA(pdb, 0x1060);
        TASK_CHECK(atA.Name == "dup" && atB.Name == "dup");
        TASK_CHECK(atA.Size == 0x10);
        TASK_CHECK(atB.Size == 0x30);
        TASK_CHECK(atA.Arguments.size() == 1);
        TASK_CHECK(atA.Arguments[0].Name == "aOnly");
        TASK_CHECK(atB.Arguments.size() == 2);
        TASK_CHECK(atB.Arguments[0].Name == "bFirst");
        TASK_CHECK(atB.Arguments[1].Name == "bSecond");
        TASK_CHECK(atB.Arguments[1].Locations.size() == 1);
        TASK_CHECK(atB.Arguments[1].Locations[0].Offset == -8);

        // The legacy name lookup still returns one of the name-sorted entries.
        const Function byName = pdb.ResolveFunction("dup");
        TASK_CHECK(byName.RVA == 0x1020 || byName.RVA == 0x1060);
        return 0;
    }

    int case_missing_slots_preserved()
    {
        ImageConfig cfg;
        cfg.argCount = 3;
        LocalSpec p0; p0.typeIdx = 0x74; p0.flags = kParam; p0.name = "p0";
        p0.ranges.push_back({ 0x1141, defRangeRegister(17, { 1, 0x20, 0x8 }) });
        LocalSpec p1; p1.typeIdx = 0x75; p1.flags = kParam; p1.name = "p1";
        p1.ranges.push_back({ 0x1141, defRangeRegister(18, { 1, 0x28, 0x8 }) });
        cfg.locals = { p0, p1 };

        auto pdb = PDB::ParseFromMemory(singleProcImage(cfg));
        const Function f = pdb.ResolveFunction("f");
        TASK_CHECK(f.ArgumentCount == 3);
        TASK_CHECK(f.Arguments.size() == 3);
        TASK_CHECK(f.Arguments[0].Name == "p0");
        TASK_CHECK(f.Arguments[1].Name == "p1");
        TASK_CHECK(f.Arguments[2].Name.empty());
        TASK_CHECK(f.Arguments[2].TypeName == "int");
        TASK_CHECK(f.Arguments[2].Locations.empty());
        return 0;
    }

    // ======================================================================
    // ranges: DEFRANGE decoding, gap removal, clipping, ordering
    // ======================================================================

    int case_register_rel_spilled_rejected()
    {
        // A spilled-UDT / offset-parent register-relative location is out of
        // scope; it must be ignored rather than misread as a plain loc.
        ImageConfig cfg;
        cfg.argCount = 1;
        LocalSpec p; p.typeIdx = 0x74; p.flags = kParam; p.name = "spill";
        // flag bit0 (spilled UDT member)
        p.ranges.push_back({ 0x1145, defRangeRegisterRel(22, 0x1, -4, { 1, 0x20, 0x8 }) });
        cfg.locals = { p };

        auto pdb = PDB::ParseFromMemory(singleProcImage(cfg));
        const auto& l = pdb.ResolveFunction("f").Arguments[0].Locations;
        TASK_CHECK(l.empty());
        return 0;
    }

    int case_register()
    {
        ImageConfig cfg;
        cfg.argCount = 1;
        LocalSpec p; p.typeIdx = 0x74; p.flags = kParam; p.name = "r";
        p.ranges.push_back({ 0x1141, defRangeRegister(9, { 1, 0x24, 0x10 }) });
        cfg.locals = { p };

        auto pdb = PDB::ParseFromMemory(singleProcImage(cfg));
        const auto& l = pdb.ResolveFunction("f").Arguments[0].Locations;
        TASK_CHECK(l.size() == 1);
        TASK_CHECK(LocIs(l[0], 0x1024, 0x1034, ArgumentLocationKind::Register, 9, 0));
        return 0;
    }

    int case_register_relative_negative()
    {
        ImageConfig cfg;
        cfg.argCount = 1;
        LocalSpec p; p.typeIdx = 0x74; p.flags = kParam; p.name = "rr";
        p.ranges.push_back({ 0x1145, defRangeRegisterRel(22, 0, -8, { 1, 0x24, 0x8 }) });
        cfg.locals = { p };

        auto pdb = PDB::ParseFromMemory(singleProcImage(cfg));
        const auto& l = pdb.ResolveFunction("f").Arguments[0].Locations;
        TASK_CHECK(l.size() == 1);
        TASK_CHECK(LocIs(l[0], 0x1024, 0x102c,
                         ArgumentLocationKind::RegisterRelative, 22, -8));
        return 0;
    }

    int case_frame_pointer_negative()
    {
        ImageConfig cfg;
        cfg.argCount = 1;
        LocalSpec p; p.typeIdx = 0x74; p.flags = kParam; p.name = "fr";
        p.ranges.push_back({ 0x1142, defRangeFramePointerRel(-24, { 1, 0x28, 0x8 }) });
        cfg.locals = { p };

        auto pdb = PDB::ParseFromMemory(singleProcImage(cfg));
        const auto& l = pdb.ResolveFunction("f").Arguments[0].Locations;
        TASK_CHECK(l.size() == 1);
        TASK_CHECK(LocIs(l[0], 0x1028, 0x1030,
                         ArgumentLocationKind::FramePointerRelative, 0, -24));
        return 0;
    }

    int case_full_scope()
    {
        ImageConfig cfg;
        cfg.argCount = 1;
        cfg.procLen = 0x40;
        LocalSpec p; p.typeIdx = 0x74; p.flags = kParam; p.name = "all";
        p.ranges.push_back({ 0x1144, defRangeFramePointerRelFullScope(-32) });
        cfg.locals = { p };

        auto pdb = PDB::ParseFromMemory(singleProcImage(cfg));
        const auto& l = pdb.ResolveFunction("f").Arguments[0].Locations;
        TASK_CHECK(l.size() == 1);
        TASK_CHECK(LocIs(l[0], 0x1020, 0x1060,
                         ArgumentLocationKind::FramePointerRelative, 0, -32));
        return 0;
    }

    int case_gap_split()
    {
        ImageConfig cfg;
        cfg.argCount = 1;
        LocalSpec p; p.typeIdx = 0x74; p.flags = kParam; p.name = "g";
        RangeSpec r{ 1, 0x20, 0x10 };
        r.gapOffset = 4; r.gapLength = 4;
        p.ranges.push_back({ 0x1141, defRangeRegister(17, r) });
        cfg.locals = { p };

        auto pdb = PDB::ParseFromMemory(singleProcImage(cfg));
        const auto& l = pdb.ResolveFunction("f").Arguments[0].Locations;
        TASK_CHECK(l.size() == 2);
        TASK_CHECK(LocIs(l[0], 0x1020, 0x1024, ArgumentLocationKind::Register, 17, 0));
        TASK_CHECK(LocIs(l[1], 0x1028, 0x1030, ArgumentLocationKind::Register, 17, 0));
        // No piece may bridge the hole.
        TASK_CHECK(!(l[0].EndRVA > 0x1024));
        TASK_CHECK(!(l[1].BeginRVA < 0x1028));
        return 0;
    }

    int case_clip_and_multiple()
    {
        ImageConfig cfg;
        cfg.argCount = 1;
        LocalSpec p; p.typeIdx = 0x74; p.flags = kParam; p.name = "m";
        // Range extends before the proc start; clipped to [0x1020, 0x1040).
        RangeSpec r1{ 1, 0x10, 0x30 };
        p.ranges.push_back({ 0x1141, defRangeRegister(17, r1) });
        // A second, lower-address record must keep its original record order.
        RangeSpec r2{ 1, 0x20, 0x8 };
        p.ranges.push_back({ 0x1145, defRangeRegisterRel(22, 0, 16, r2) });
        // A third, higher-address record.
        RangeSpec r3{ 1, 0x38, 0x8 };
        p.ranges.push_back({ 0x1141, defRangeRegister(18, r3) });
        cfg.locals = { p };

        auto pdb = PDB::ParseFromMemory(singleProcImage(cfg));
        const auto& l = pdb.ResolveFunction("f").Arguments[0].Locations;
        TASK_CHECK(l.size() == 3);
        TASK_CHECK(LocIs(l[0], 0x1020, 0x1040,
                         ArgumentLocationKind::Register, 17, 0));
        TASK_CHECK(LocIs(l[1], 0x1020, 0x1028,
                         ArgumentLocationKind::RegisterRelative, 22, 16));
        TASK_CHECK(LocIs(l[2], 0x1038, 0x1040,
                         ArgumentLocationKind::Register, 18, 0));
        return 0;
    }

    // ======================================================================
    // ownership: Locations outlive raw bytes, streams, intern mode, and moves
    // ======================================================================

    ImageConfig richConfig()
    {
        ImageConfig cfg;
        cfg.argCount = 2;
        cfg.rich = true;
        LocalSpec p0; p0.typeIdx = 0x74; p0.flags = kParam; p0.name = "first";
        p0.ranges.push_back({ 0x1141, defRangeRegister(17, { 1, 0x20, 0x10 }) });
        RangeSpec r{ 1, 0x30, 0x10 }; r.gapOffset = 4; r.gapLength = 4;
        p0.ranges.push_back({ 0x1145, defRangeRegisterRel(22, 0, -8, r) });
        LocalSpec p1; p1.typeIdx = 0x75; p1.flags = kParam; p1.name = "second";
        p1.ranges.push_back({ 0x1142, defRangeFramePointerRel(-16, { 1, 0x40, 0x10 }) });
        p1.ranges.push_back({ 0x1144, defRangeFramePointerRelFullScope(-32) });
        cfg.locals = { p0, p1 };
        return cfg;
    }

    int case_intern_true_after_release()
    {
        ParseOptions opt;
        opt.InternStrings = true;
        auto pdb = PDB::ParseFromMemory(singleProcImage(richConfig()), opt);
        const Function f = pdb.ResolveFunction("f");
        TASK_CHECK(f.Arguments.size() == 2);
        TASK_CHECK(f.Arguments[0].Name == "first");       // interned arena
        TASK_CHECK(f.Arguments[1].Name == "second");
        TASK_CHECK(f.Arguments[0].Locations.size() == 3); // reg + gap piece
        TASK_CHECK(f.Arguments[1].Locations.size() == 2); // frame + full scope
        TASK_CHECK(LocIs(f.Arguments[0].Locations[0], 0x1020, 0x1030,
                         ArgumentLocationKind::Register, 17, 0));
        TASK_CHECK(LocIs(f.Arguments[0].Locations[1], 0x1030, 0x1034,
                         ArgumentLocationKind::RegisterRelative, 22, -8));
        TASK_CHECK(LocIs(f.Arguments[0].Locations[2], 0x1038, 0x1040,
                         ArgumentLocationKind::RegisterRelative, 22, -8));
        TASK_CHECK(LocIs(f.Arguments[1].Locations[1], 0x1020, 0x1060,
                         ArgumentLocationKind::FramePointerRelative, 0, -32));
        // Raw PDB bytes and copied streams are gone, yet the views read fine.
        TASK_CHECK(pdb.MemoryUsage().rawPDB == 0);
        return 0;
    }

    int case_intern_false_same_intervals()
    {
        ParseOptions on;  on.InternStrings  = true;
        ParseOptions off; off.InternStrings = false;
        auto a = PDB::ParseFromMemory(singleProcImage(richConfig()), on);
        auto b = PDB::ParseFromMemory(singleProcImage(richConfig()), off);
        TASK_CHECK(b.MemoryUsage().rawPDB > 0);
        const Function fa = a.ResolveFunction("f");
        const Function fb = b.ResolveFunction("f");
        TASK_CHECK(fa.Arguments.size() == fb.Arguments.size());
        for (size_t i = 0; i < fa.Arguments.size(); ++i)
        {
            TASK_CHECK(SameSpans(fa.Arguments[i].Locations,
                                 fb.Arguments[i].Locations));
            TASK_CHECK(fa.Arguments[i].Name == fb.Arguments[i].Name);
        }
        return 0;
    }

    int case_fragmented_pages()
    {
        // A block size small enough that the module and TPI streams span several
        // pages; fragmented=true stores those pages out of order, forcing the
        // MSF reader down its copy path.
        ImageConfig cfg = richConfig();
        cfg.fragmented = true;
        cfg.blockSize  = 256;

        auto frag = PDB::ParseFromMemory(singleProcImage(cfg));
        auto cont = PDB::ParseFromMemory(singleProcImage(richConfig()));
        const Function ff = frag.ResolveFunction("f");
        const Function fc = cont.ResolveFunction("f");
        TASK_CHECK(ff.Arguments.size() == 2);
        TASK_CHECK(SameSpans(ff.Arguments[0].Locations, fc.Arguments[0].Locations));
        TASK_CHECK(SameSpans(ff.Arguments[1].Locations, fc.Arguments[1].Locations));
        TASK_CHECK(ff.Arguments[1].Locations.size() == 2);

        // The same scattered pages with InternStrings off still yield identical
        // intervals (raw PDB bytes retained instead of interned).
        ParseOptions keep;
        keep.InternStrings = false;
        auto fragRaw = PDB::ParseFromMemory(singleProcImage(cfg), keep);
        TASK_CHECK(fragRaw.MemoryUsage().rawPDB > 0);
        const Function fr = fragRaw.ResolveFunction("f");
        TASK_CHECK(SameSpans(fr.Arguments[0].Locations, fc.Arguments[0].Locations));
        TASK_CHECK(SameSpans(fr.Arguments[1].Locations, fc.Arguments[1].Locations));
        TASK_CHECK(fr.Arguments[0].Name == "first");
        return 0;
    }

    int case_moved_views()
    {
        auto pdb = PDB::ParseFromMemory(singleProcImage(richConfig()));

        // Capture views, then move the PDB and destroy the source object. The
        // Argument/Function spans must keep pointing at the moved-into storage.
        const Function before = pdb.ResolveFunction("f");
        const Argument argCopy = before.Arguments[0];
        const std::vector<Argument> argsCopy(before.Arguments.begin(),
                                             before.Arguments.end());

        PDB moved = std::move(pdb);
        { PDB destroyed = std::move(pdb); (void)destroyed; } // source is empty already
        TASK_CHECK(!pdb.IsLoaded());

        // Views obtained before the move remain readable while `moved` lives.
        TASK_CHECK(!argCopy.Name.empty() && argCopy.Name == "first");
        TASK_CHECK(argsCopy.size() == 2);
        TASK_CHECK(argsCopy[0].Locations.size() == 3);
        TASK_CHECK(LocIs(argsCopy[0].Locations[1], 0x1030, 0x1034,
                         ArgumentLocationKind::RegisterRelative, 22, -8));
        TASK_CHECK(argsCopy[1].Locations.size() == 2);

        const Function after = moved.ResolveFunction("f");
        TASK_CHECK(after.Arguments.size() == 2);
        TASK_CHECK(SameSpans(after.Arguments[0].Locations,
                             argsCopy[0].Locations));
        return 0;
    }

    int case_resolve_arguments_off()
    {
        ParseOptions opt;
        opt.ResolveArguments = false;
        opt.ResolveSizes     = true;
        auto pdb = PDB::ParseFromMemory(singleProcImage(richConfig()), opt);
        const Function f = pdb.ResolveFunction("f");
        TASK_CHECK(f.Size == 0x40);
        TASK_CHECK(f.ArgumentCount == 0);
        TASK_CHECK(f.Arguments.empty());
        TASK_CHECK(pdb.FunctionCount() == 1);

        // Other interfaces are untouched: globals and structs still resolve.
        const auto g = pdb.TryResolveGlobal("?soloGlobal@@3HA");
        TASK_CHECK(g.has_value());
        TASK_CHECK(g->RVA == 0x1200);
        TASK_CHECK(g->Kind == MemPDB::SymbolKind::Variable);
        const auto s = pdb.TryResolveStruct("Solo");
        TASK_CHECK(s.has_value());
        TASK_CHECK(s->Size == 4);
        TASK_CHECK(s->FieldCount == 1);
        TASK_CHECK(s->Fields[0].Name == "x");
        return 0;
    }
}

// ---- task registration -----------------------------------------------------
const struct task_case task_cases[] = {
    { "scope", "modern_order",               &case_modern_order },
    { "scope", "locals_interspersed",        &case_locals_interspersed },
    { "scope", "block_scope",                &case_block_scope },
    { "scope", "optimized_out",              &case_optimized_out },
    { "scope", "legacy_unchanged",           &case_legacy_unchanged },
    { "scope", "same_name_two_addresses",    &case_same_name_two_addresses },
    { "scope", "missing_slots_preserved",    &case_missing_slots_preserved },

    { "ranges", "register_rel_spilled_rejected", &case_register_rel_spilled_rejected },
    { "ranges", "register",                  &case_register },
    { "ranges", "register_relative_negative", &case_register_relative_negative },
    { "ranges", "frame_pointer_negative",    &case_frame_pointer_negative },
    { "ranges", "full_scope",                &case_full_scope },
    { "ranges", "gap_split",                 &case_gap_split },
    { "ranges", "clip_and_multiple",         &case_clip_and_multiple },

    { "ownership", "intern_true_after_release", &case_intern_true_after_release },
    { "ownership", "intern_false_same_intervals", &case_intern_false_same_intervals },
    { "ownership", "fragmented_pages",       &case_fragmented_pages },
    { "ownership", "moved_views",            &case_moved_views },
    { "ownership", "resolve_arguments_off",  &case_resolve_arguments_off },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
