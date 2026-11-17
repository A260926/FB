// =============================================================================
//  MemPDB test & benchmark suite
//
//  Three concerns, one binary:
//    * ACCURACY  - differential across every ParseOptions combination, table
//                  invariants, known-symbol probes, and a self-built fixture
//                  PDB that provides real size/argument ground truth.
//    * MEMORY    - MemStats breakdown per configuration showing the interning
//                  and tiering savings.
//    * SPEED     - RDTSC-timed parse (per phase + total, min/median over many
//                  runs) and a lookup microbenchmark.
//
//  Usage: mempdb_tests [public.pdb] [fixture.pdb]
//  Writes a copy of all output to mempdb_report.txt and exits 0 iff every
//  accuracy check passes.
// =============================================================================
#define _CRT_SECURE_NO_WARNINGS // std::fopen for the report file is fine here
#include <MemPDB/MemPDB.hpp>
#include "Timer.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

using MemPDB::ParseOptions;

// ---- output tee: stdout + a report file (robust against shell capture) ------
static FILE* g_rep = nullptr;

static void Out(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    if (g_rep)
    {
        va_start(ap, fmt);
        std::vfprintf(g_rep, fmt, ap);
        va_end(ap);
    }
}

// ---- tiny test harness ------------------------------------------------------
static int    g_pass = 0;
static int    g_fail = 0;
static double g_tps  = 1.0;

static void Check(bool cond, const char* what)
{
    if (cond) { ++g_pass; Out("    [PASS] %s\n", what); }
    else      { ++g_fail; Out("    [FAIL] %s\n", what); }
}

static double Ms(uint64_t cyc) { return static_cast<double>(cyc) / g_tps * 1e3; }
static double Nsf(double cyc)   { return cyc / g_tps * 1e9; }

static std::vector<std::byte> ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::byte> buf(n);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
    return buf;
}

static std::vector<std::byte> Locate(const char* argOverride,
                                     std::initializer_list<const char*> defaults)
{
    if (argOverride && *argOverride)
    {
        auto b = ReadFile(argOverride);
        if (!b.empty()) { Out("  using %s\n", argOverride); return b; }
    }
    for (const char* p : defaults)
    {
        auto b = ReadFile(p);
        if (!b.empty()) { Out("  using %s\n", p); return b; }
    }
    return {};
}

// ---- parse measurement ------------------------------------------------------
struct Measure
{
    uint64_t          best   = ~0ull;
    uint64_t          median = 0;
    MemPDB::ParseInfo info{};
    MemPDB::MemStats  mem{};
};

static Measure MeasureParse(const std::vector<std::byte>& master,
                            const ParseOptions& o, int n)
{
    Measure m;
    std::vector<uint64_t> samples;
    samples.reserve(n);

    // Warm the allocator/caches so the first config measured isn't penalized
    // by first-touch page faults relative to later ones.
    for (int i = 0; i < 5; ++i)
    {
        auto warm = master;
        auto pdb = MemPDB::PDB::ParseFromMemory(std::move(warm), o);
        (void)pdb;
    }

    for (int i = 0; i < n; ++i)
    {
        auto copy = master;                       // copy not timed
        const uint64_t a = bench::RdtscStart();
        auto pdb = MemPDB::PDB::ParseFromMemory(std::move(copy), o);
        const uint64_t b = bench::RdtscEnd();
        const uint64_t c = b - a;
        samples.push_back(c);
        if (c < m.best) { m.best = c; m.info = pdb.GetParseInfo(); m.mem = pdb.MemoryUsage(); }
    }
    std::sort(samples.begin(), samples.end());
    m.median = samples[samples.size() / 2];
    return m;
}

// ---- accuracy snapshot ------------------------------------------------------
struct Snap { std::string name; uint32_t rva, size, argc; };

static std::vector<Snap> Snapshot(const std::vector<std::byte>& master, const ParseOptions& o)
{
    auto copy = master;
    auto pdb = MemPDB::PDB::ParseFromMemory(std::move(copy), o);
    std::vector<Snap> v;
    v.reserve(pdb.FunctionCount());
    for (std::size_t i = 0; i < pdb.FunctionCount(); ++i)
    {
        const auto f = pdb.FunctionAt(i);
        v.push_back({ std::string(f.Name), f.RVA, f.Size, f.ArgumentCount });
    }
    return v;
}

static std::size_t DiffNameRVA(const std::vector<Snap>& a, const std::vector<Snap>& b)
{
    if (a.size() != b.size()) return a.size() > b.size() ? a.size() : b.size();
    std::size_t bad = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].rva != b[i].rva) ++bad;
    return bad;
}

static std::size_t DiffSize(const std::vector<Snap>& a, const std::vector<Snap>& b)
{
    if (a.size() != b.size()) return ~std::size_t(0);
    std::size_t bad = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].size != b[i].size) ++bad;
    return bad;
}

// =============================================================================
int main(int argc, char** argv)
{
    g_rep = std::fopen("mempdb_report.txt", "w");
    g_tps = bench::TscPerSec();

    Out("============================================================\n");
    Out(" MemPDB test & benchmark suite\n");
    Out("============================================================\n");
    Out(" TSC: %.3f GHz  hw_threads: %u\n\n",
        g_tps / 1e9, std::thread::hardware_concurrency());

    Out("[locating public PDB]\n");
    const auto pub = Locate(argc > 1 ? argv[1] : nullptr,
        { "ntdll.pdb", "../ntdll.pdb", "../../ntdll.pdb" });
    Out("[locating fixture PDB]\n");
    const auto fixture = Locate(argc > 2 ? argv[2] : nullptr,
        { "mempdb_fixture.pdb", "../mempdb_fixture.pdb" });

    if (pub.empty()) { Out("FATAL: no public PDB found\n"); return 2; }
    Out("  public PDB: %zu bytes\n\n", pub.size());

    // Option presets ---------------------------------------------------------
    const ParseOptions cfgNames   = ParseOptions::FunctionsOnly();      // functions only
    const ParseOptions cfgSizes   = ParseOptions::FunctionsWithSizes(); // + function sizes
    const ParseOptions cfgFull    = ParseOptions::Everything();         // funcs+globals+args+structs
    const ParseOptions cfgStructs = ParseOptions::StructsOnly();        // structs, no funcs/globals
    const ParseOptions cfgGlobals = ParseOptions::GlobalsOnly();        // globals, no funcs/structs
    ParseOptions cfgNoIntern = cfgFull; cfgNoIntern.InternStrings = false; // keep raw buffers
    ParseOptions cfgSerial   = cfgFull; cfgSerial.Parallel        = false; // no threads

    // =========================================================================
    Out("------------------------------------------------------------\n");
    Out(" ACCURACY\n");
    Out("------------------------------------------------------------\n");

    Out("  [differential across option combinations]\n");
    const auto sNames    = Snapshot(pub, cfgNames);
    const auto sSizes    = Snapshot(pub, cfgSizes);
    const auto sFull     = Snapshot(pub, cfgFull);
    const auto sNoIntern = Snapshot(pub, cfgNoIntern);
    const auto sSerial   = Snapshot(pub, cfgSerial);

    Out("    function count: %zu\n", sFull.size());
    Check(!sFull.empty(), "function table is non-empty");
    Check(DiffNameRVA(sFull, sNames)    == 0, "names-only agrees on name->RVA");
    Check(DiffNameRVA(sFull, sSizes)    == 0, "sizes tier agrees on name->RVA");
    Check(DiffNameRVA(sFull, sNoIntern) == 0, "no-intern agrees on name->RVA");
    Check(DiffNameRVA(sFull, sSerial)   == 0, "serial agrees on name->RVA");
    Check(DiffSize(sFull, sSizes)       == 0, "sizes match: full vs sizes-tier");
    Check(DiffSize(sFull, sNoIntern)    == 0, "sizes match: full vs no-intern");
    Check(DiffSize(sFull, sSerial)      == 0, "sizes match: full vs serial (thread-safe)");
    {
        bool allZero = true;
        for (const auto& s : sNames) if (s.size != 0 || s.argc != 0) { allZero = false; break; }
        Check(allZero, "names-only tier yields no sizes/args (work skipped)");
    }

    // Presets must turn off exactly the work they claim to, so callers only pay
    // for (and store) what they request.
    Out("  [globals]\n");
    {
        auto c1 = pub; auto pFull = MemPDB::PDB::ParseFromMemory(std::move(c1), cfgFull);
        Out("    global count: %zu\n", pFull.GlobalCount());
        Check(pFull.GlobalCount() > 0, "global table is non-empty");

        // Every global must have a non-zero RVA and be name-sorted.
        std::size_t zeroRva = 0, unsorted = 0;
        for (std::size_t i = 0; i < pFull.GlobalCount(); ++i)
        {
            const auto g = pFull.GlobalAt(i);
            if (g.RVA == 0) ++zeroRva;
            if (i > 0 && pFull.GlobalAt(i).Name < pFull.GlobalAt(i - 1).Name) ++unsorted;
        }
        Check(zeroRva == 0,   "no global has RVA == 0");
        Check(unsorted == 0,  "global table is name-sorted (binary-search safe)");

        // No symbol should appear in BOTH the function and global tables.
        // The fFunction flag must have been applied correctly.
        {
            std::size_t overlap = 0;
            for (std::size_t i = 0; i < pFull.GlobalCount(); ++i)
            {
                if (pFull.TryResolveFunction(pFull.GlobalAt(i).Name)) ++overlap;
            }
            Check(overlap == 0, "function and global tables do not overlap");
        }

        // Per-category breakdown. The precomputed counts must agree with both a
        // manual tally over GlobalAt() and the size of GlobalsOfKind().
        Out("    category breakdown:\n");
        const MemPDB::SymbolKind kinds[] = {
            MemPDB::SymbolKind::Variable, MemPDB::SymbolKind::Constant,
            MemPDB::SymbolKind::Vtable,   MemPDB::SymbolKind::VbTable,
            MemPDB::SymbolKind::Rtti,     MemPDB::SymbolKind::StringLiteral,
            MemPDB::SymbolKind::Unknown,
        };
        std::size_t sumOfKinds = 0;
        bool countsConsistent = true, viewsConsistent = true;
        for (auto k : kinds)
        {
            const std::size_t c = pFull.GlobalCountOfKind(k);
            sumOfKinds += c;
            std::size_t manual = 0;
            for (std::size_t i = 0; i < pFull.GlobalCount(); ++i)
                if (pFull.GlobalAt(i).Kind == k) ++manual;
            if (manual != c) countsConsistent = false;
            const auto view = pFull.GlobalsOfKind(k);
            if (view.size() != c) viewsConsistent = false;
            for (const auto& g : view) if (g.Kind != k) viewsConsistent = false;
            Out("      %-14s %6zu\n", MemPDB::ToString(k), c);
        }
        Check(sumOfKinds == pFull.GlobalCount(), "kind counts sum to total global count");
        Check(countsConsistent, "GlobalCountOfKind matches manual tally for every kind");
        Check(viewsConsistent,  "GlobalsOfKind returns exactly the matching, tagged entries");

        // Spot-check a couple of categories that ntdll always has.
        Check(pFull.GlobalCountOfKind(MemPDB::SymbolKind::StringLiteral) > 0,
              "ntdll has string-literal globals");

        // Show up to 6 example globals (name + category) for readability.
        Out("    sample globals (first 6):\n");
        for (std::size_t i = 0; i < pFull.GlobalCount() && i < 6; ++i)
        {
            const auto g = pFull.GlobalAt(i);
            Out("      RVA=0x%08X  %-14s  %.*s\n",
                g.RVA, MemPDB::ToString(g.Kind), (int)g.Name.size(), g.Name.data());
        }
    }

    Out("  [preset selectivity]\n");
    {
        auto c1 = pub; auto pFns = MemPDB::PDB::ParseFromMemory(std::move(c1), cfgNames);
        Check(pFns.FunctionCount() > 0,  "FunctionsOnly: has functions");
        Check(pFns.GlobalCount()  == 0,  "FunctionsOnly: no globals decoded");
        Check(pFns.StructCount()  == 0,  "FunctionsOnly: no structs decoded");
        Check(pFns.MemoryUsage().globalTable == 0 &&
              pFns.MemoryUsage().structTable == 0 &&
              pFns.MemoryUsage().fieldTable  == 0,
              "FunctionsOnly: no global/struct/field memory allocated");

        auto c2 = pub; auto pGlb = MemPDB::PDB::ParseFromMemory(std::move(c2), cfgGlobals);
        Check(pGlb.GlobalCount()   > 0,  "GlobalsOnly: has globals");
        Check(pGlb.FunctionCount() == 0, "GlobalsOnly: no functions decoded");
        Check(pGlb.StructCount()   == 0, "GlobalsOnly: no structs decoded");
        Check(pGlb.MemoryUsage().functionTable == 0 &&
              pGlb.MemoryUsage().structTable   == 0,
              "GlobalsOnly: no function/struct memory allocated");

        auto c3 = pub; auto pStr = MemPDB::PDB::ParseFromMemory(std::move(c3), cfgStructs);
        Check(pStr.StructCount()   > 0,  "StructsOnly: has structs");
        Check(pStr.FunctionCount() == 0, "StructsOnly: no functions decoded");
        Check(pStr.GlobalCount()   == 0, "StructsOnly: no globals decoded");
        Check(pStr.MemoryUsage().functionTable == 0 &&
              pStr.MemoryUsage().globalTable   == 0,
              "StructsOnly: no function/global memory allocated");

        // A struct resolved in structs-only mode must match full-parse output.
        auto c4 = pub; auto pAll = MemPDB::PDB::ParseFromMemory(std::move(c4), cfgFull);
        if (pAll.StructCount() > 0)
        {
            const auto any = pAll.StructAt(0);
            auto a = pStr.TryResolveStruct(any.Name);
            auto b = pAll.TryResolveStruct(any.Name);
            const bool same = a && b && a->Size == b->Size && a->FieldCount == b->FieldCount;
            Check(same, "StructsOnly layout matches full-parse layout");
        }

        // A global resolved in globals-only mode must match full-parse RVA.
        if (pAll.GlobalCount() > 0)
        {
            const auto any = pAll.GlobalAt(0);
            auto a = pGlb.TryResolveGlobal(any.Name);
            auto b = pAll.TryResolveGlobal(any.Name);
            Check(a && b && a->RVA == b->RVA, "GlobalsOnly RVA matches full-parse RVA");
        }

        // GlobalKindMask: parsing only one category stores ONLY that category,
        // and its contents/RVAs match the full parse exactly.
        {
            const auto mask = MemPDB::KindBit(MemPDB::SymbolKind::StringLiteral);
            auto c5 = pub;
            auto pStrs = MemPDB::PDB::ParseFromMemory(
                std::move(c5), MemPDB::ParseOptions::GlobalsOfKind(mask));

            bool onlyKind = true;
            for (std::size_t i = 0; i < pStrs.GlobalCount(); ++i)
                if (pStrs.GlobalAt(i).Kind != MemPDB::SymbolKind::StringLiteral)
                    onlyKind = false;
            Check(onlyKind, "GlobalKindMask: only the requested kind is stored");
            Check(pStrs.GlobalCount() ==
                      pAll.GlobalCountOfKind(MemPDB::SymbolKind::StringLiteral),
                  "GlobalKindMask: count matches that category in the full parse");
            Check(pStrs.MemoryUsage().globalTable <= pAll.MemoryUsage().globalTable,
                  "GlobalKindMask: stores no more global memory than the full parse");
        }
    }

    Out("  [table invariants]\n");
    {
        std::size_t unsorted = 0, zeroRva = 0, dups = 0;
        for (std::size_t i = 1; i < sFull.size(); ++i)
        {
            if (sFull[i].name < sFull[i - 1].name) ++unsorted;
            if (sFull[i].name == sFull[i - 1].name) ++dups;
        }
        for (const auto& s : sFull) if (s.rva == 0) ++zeroRva;
        Check(unsorted == 0, "function table is name-sorted (binary-search safe)");
        Check(zeroRva == 0,  "no function has RVA == 0");
        Out("    (%zu duplicate-named entries)\n", dups);
    }
    {
        auto copy = pub;
        auto pdb = MemPDB::PDB::ParseFromMemory(std::move(copy), cfgFull);
        std::size_t bad = 0;
        for (std::size_t i = 0; i < pdb.FunctionCount(); ++i)
        {
            const auto f = pdb.FunctionAt(i);
            if (f.Arguments.size() != f.ArgumentCount) ++bad;
        }
        Check(bad == 0, "Arguments span length == ArgumentCount for all functions");
    }

    Out("  [known-symbol probes]\n");
    {
        auto copy = pub;
        auto pdb = MemPDB::PDB::ParseFromMemory(std::move(copy), cfgFull);
        const char* probes[] = { "NtCreateFile", "NtClose", "LdrLoadDll",
                                 "RtlAllocateHeap", "NtQuerySystemInformation" };
        bool allFound = true;
        for (const char* p : probes)
        {
            auto f = pdb.TryResolveFunction(p);
            if (!f || f->RVA == 0) { allFound = false; Out("      MISSING %s\n", p); }
        }
        Check(allFound, "well-known ntdll symbols resolve with non-zero RVA");

        auto f = pdb.TryResolveFunction("NtCreateFile");
        bool match = false;
        for (std::size_t i = 0; i < pdb.FunctionCount() && f; ++i)
        {
            const auto e = pdb.FunctionAt(i);
            if (e.Name == "NtCreateFile") { match = (e.RVA == f->RVA); break; }
        }
        Check(match, "ResolveFunction RVA matches enumerated entry");
    }

    // Fixture: real size/argument ground truth -------------------------------
    if (!fixture.empty())
    {
        Out("  [fixture PDB: size + argument ground truth]\n");
        auto copy = fixture;
        auto pdb = MemPDB::PDB::ParseFromMemory(std::move(copy), cfgFull);
        const auto pi = pdb.GetParseInfo();
        Out("    (%zu modules, %zu procs parsed, %zu matched to public symbols)\n",
            pi.moduleCount, pi.procsParsed, pi.procMatches);

        // Same differential as the public-PDB block: name->RVA must agree across
        // tiers even when the PDB carries private symbols.
        const auto fNames = Snapshot(fixture, cfgNames);
        const auto fFull  = Snapshot(fixture, cfgFull);
        Check(DiffNameRVA(fFull, fNames) == 0, "fixture: full agrees with names-only on name->RVA");

        struct Ex { const char* name; uint32_t argc; };
        const Ex ex[] = {
            { "fixture_add",   2 }, { "fixture_three", 3 }, { "fixture_five", 5 },
            { "fixture_none",  0 }, { "fixture_mixed", 3 }, { "fixture_ptr",  2 },
        };
        for (const auto& e : ex)
        {
            auto f = pdb.TryResolveFunction(e.name);
            if (!f) { Check(false, e.name); continue; }
            Out("      %-16s RVA=0x%06X size=%-4u args=%u :",
                e.name, f->RVA, f->Size, f->ArgumentCount);
            for (uint32_t i = 0; i < f->ArgumentCount; ++i)
            {
                Out(" %s", std::string(f->Arguments[i].TypeName).c_str());
                if (!f->Arguments[i].Name.empty())
                    Out(" %s", std::string(f->Arguments[i].Name).c_str());
            }
            Out("\n");
            char buf[80];
            std::snprintf(buf, sizeof(buf), "%s has non-zero size", e.name);
            Check(f->Size > 0, buf);
            std::snprintf(buf, sizeof(buf), "%s argc == %u (got %u)", e.name, e.argc, f->ArgumentCount);
            Check(f->ArgumentCount == e.argc, buf);
        }

        // -- struct layout ground truth --------------------------------------
        Out("  [fixture PDB: struct layout ground truth]\n");
        Out("    (%zu structs decoded)\n", pdb.StructCount());
        Check(pdb.StructCount() > 0, "struct table is non-empty");

        // Expected member layouts (name, type, byte offset). These mirror the
        // x64 layouts the compiler must produce for the fixture types.
        struct ExField { const char* name; const char* type; uint32_t off; };
        struct ExStruct { const char* name; uint32_t size; std::vector<ExField> fields; };
        const ExStruct xs[] = {
            { "FixturePoint", 16, {
                { "x", "int",    0 },
                { "y", "int",    4 },
                { "z", "double", 8 },
            }},
            { "FixtureMix", 40, {
                { "tag", "char",     0 },
                { "ptr", "void*",    8 },
                { "id",  "unsigned __int64", 16 },
                { "arr", "int[]",   24 },
            }},
        };
        for (const auto& x : xs)
        {
            auto s = pdb.TryResolveStruct(x.name);
            if (!s) { Check(false, x.name); continue; }

            Out("      struct %s (size=%u, %u fields):\n", x.name, s->Size, s->FieldCount);
            for (uint32_t i = 0; i < s->FieldCount; ++i)
                Out("        +%-3u %-18.*s %.*s\n",
                    s->Fields[i].Offset,
                    (int)s->Fields[i].TypeName.size(), s->Fields[i].TypeName.data(),
                    (int)s->Fields[i].Name.size(), s->Fields[i].Name.data());

            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s total size == %u (got %u)", x.name, x.size, s->Size);
            Check(s->Size == x.size, buf);
            std::snprintf(buf, sizeof(buf), "%s field count == %zu (got %u)",
                          x.name, x.fields.size(), s->FieldCount);
            Check(s->FieldCount == x.fields.size(), buf);

            for (std::size_t i = 0; i < x.fields.size() && i < s->FieldCount; ++i)
            {
                const auto& want = x.fields[i];
                const auto& got  = s->Fields[i];
                const bool ok = got.Name == want.name
                             && got.TypeName == want.type
                             && got.Offset == want.off;
                std::snprintf(buf, sizeof(buf), "%s.%s : %s @%u",
                              x.name, want.name, want.type, want.off);
                Check(ok, buf);
            }
        }

        // Struct table must be name-sorted for binary search, like functions.
        {
            std::size_t unsorted = 0;
            for (std::size_t i = 1; i < pdb.StructCount(); ++i)
                if (pdb.StructAt(i).Name < pdb.StructAt(i - 1).Name) ++unsorted;
            Check(unsorted == 0, "struct table is name-sorted (binary-search safe)");
        }

        // Every field span length must equal FieldCount.
        {
            std::size_t bad = 0;
            for (std::size_t i = 0; i < pdb.StructCount(); ++i)
            {
                const auto s = pdb.StructAt(i);
                if (s.Fields.size() != s.FieldCount) ++bad;
            }
            Check(bad == 0, "Fields span length == FieldCount for all structs");
        }

        // -- symbol-category ground truth ------------------------------------
        // The fixture deliberately emits a vtable, RTTI, a string literal, and a
        // C++ const global. Verify each lands in the right SymbolKind. Names are
        // matched by their decoration prefix to avoid pinning exact mangled text.
        Out("  [fixture PDB: symbol-category ground truth]\n");
        {
            auto cg = fixture; auto pg = MemPDB::PDB::ParseFromMemory(std::move(cg), cfgFull);
            auto firstOfKind = [&](MemPDB::SymbolKind k) -> std::optional<MemPDB::Global>
            {
                for (std::size_t i = 0; i < pg.GlobalCount(); ++i)
                {
                    const auto g = pg.GlobalAt(i);
                    if (g.Kind == k) return g;
                }
                return std::nullopt;
            };
            auto kindHasPrefix = [&](MemPDB::SymbolKind k, const char* pfx) -> bool
            {
                const std::string_view p(pfx);
                for (std::size_t i = 0; i < pg.GlobalCount(); ++i)
                {
                    const auto g = pg.GlobalAt(i);
                    if (g.Kind == k && g.Name.size() >= p.size() &&
                        g.Name.substr(0, p.size()) == p) return true;
                }
                return false;
            };

            for (auto k : { MemPDB::SymbolKind::Vtable, MemPDB::SymbolKind::Rtti,
                            MemPDB::SymbolKind::StringLiteral, MemPDB::SymbolKind::Constant })
            {
                if (auto g = firstOfKind(k))
                    Out("      %-14s e.g. %.*s\n", MemPDB::ToString(k),
                        (int)g->Name.size(), g->Name.data());
            }

            // Every classified vtable name must start with ??_7, every string
            // with ??_C, every RTTI with ??_R -- the classifier's contract.
            Check(kindHasPrefix(MemPDB::SymbolKind::Vtable, "??_7"),
                  "fixture vtable is classified Vtable (??_7)");
            Check(kindHasPrefix(MemPDB::SymbolKind::StringLiteral, "??_C"),
                  "fixture string literal is classified StringLiteral (??_C)");
            Check(pg.GlobalCountOfKind(MemPDB::SymbolKind::Rtti) == 0 ||
                  kindHasPrefix(MemPDB::SymbolKind::Rtti, "??_R"),
                  "fixture RTTI (if any) is classified Rtti (??_R)");
            Check(pg.GlobalCountOfKind(MemPDB::SymbolKind::Constant) > 0,
                  "fixture has a const-qualified global (Constant)");

            // No vtable/string/rtti should ever be misfiled as a plain Variable.
            std::size_t misfiled = 0;
            for (std::size_t i = 0; i < pg.GlobalCount(); ++i)
            {
                const auto g = pg.GlobalAt(i);
                const bool special =
                    g.Name.size() >= 4 && g.Name[0]=='?' && g.Name[1]=='?' && g.Name[2]=='_';
                if (special && (g.Kind == MemPDB::SymbolKind::Variable ||
                                g.Kind == MemPDB::SymbolKind::Constant)) ++misfiled;
            }
            Check(misfiled == 0, "no ??_ special symbol is misfiled as Variable/Constant");
        }
    }
    else
    {
        Out("  [fixture PDB not found -- skipping size/arg/struct ground truth]\n");
    }

    // =========================================================================
    //  CONFIGURATION MATRIX
    //  Every meaningful extraction combination, each measured once and reported
    //  in both the SPEED table and the MEMORY table below (same row order, same
    //  underlying runs), so speed and memory line up per config.
    // =========================================================================
    const int N = 100;

    auto mk = [](bool f, bool sz, bool args, bool g, bool s) -> ParseOptions
    {
        ParseOptions o;            // default = everything on
        o.ResolveFunctions = f;
        o.ResolveSizes     = sz;
        o.ResolveArguments = args;
        o.ResolveGlobals   = g;
        o.ResolveStructs   = s;
        return o;
    };
    ParseOptions cfgFullSerial = cfgFull;   cfgFullSerial.Parallel     = false;
    ParseOptions cfgFullNoInt  = cfgFull;   cfgFullNoInt.InternStrings = false;

    struct MRow { const char* label; ParseOptions o; Measure m; };
    MRow matrix[] = {
        //  label             F  Sz Ar G  S
        { "fn-names",      mk(1, 0, 0, 0, 0), {} }, // function names + RVA only
        { "fn+sizes",      mk(1, 1, 0, 0, 0), {} }, // + function sizes
        { "fn+args",       mk(1, 0, 1, 0, 0), {} }, // + argument names/types (no sizes)
        { "fn+sizes+args", mk(1, 1, 1, 0, 0), {} }, // full function detail
        { "globals",       mk(0, 0, 0, 1, 0), {} }, // data symbols only
        { "structs",       mk(0, 0, 0, 0, 1), {} }, // struct/union layouts only
        { "fn+globals",    mk(1, 0, 0, 1, 0), {} }, // all named symbols, names+RVA
        { "everything",    mk(1, 1, 1, 1, 1), {} }, // default Everything()
        { "full-serial",   cfgFullSerial,     {} }, // everything, no threads
        { "full-noIntern", cfgFullNoInt,      {} }, // everything, keep raw buffers
    };
    for (auto& r : matrix) r.m = MeasureParse(pub, r.o, N);

    auto find = [&](const char* lbl) -> const Measure&
    {
        for (const auto& r : matrix) if (std::string(r.label) == lbl) return r.m;
        return matrix[0].m;
    };

    // -- SPEED ---------------------------------------------------------------
    Out("\n------------------------------------------------------------\n");
    Out(" SPEED  (RDTSC, ms; best & median of %d, per-phase = best run)\n", N);
    Out("------------------------------------------------------------\n");
    Out("  %-14s %8s %8s | %6s %6s %6s %6s %6s %6s %6s\n",
        "config", "best", "median", "MSF", "TPI", "PubSym", "Sort", "Mods", "Struct", "Intern");
    for (const auto& r : matrix)
        Out("  %-14s %8.3f %8.3f | %6.3f %6.3f %6.3f %6.3f %6.3f %6.3f %6.3f\n",
            r.label, Ms(r.m.best), Ms(r.m.median),
            r.m.info.msMSFDBI, r.m.info.msTPI, r.m.info.msPublicSymbols,
            r.m.info.msSort, r.m.info.msModuleStreams, r.m.info.msStructs, r.m.info.msIntern);

    const Measure& mNames  = find("fn-names");
    const Measure& mFull   = find("everything");
    const Measure& mSerial = find("full-serial");
    Out("  tiering speedup (everything -> fn-names): %.2fx\n",
        mNames.best ? (double)mFull.best / mNames.best : 0.0);
    Out("  module parse: serial %.3f ms vs parallel-eligible %.3f ms (%.2fx); "
        "module-stream volume %.1f MB\n",
        Ms(mSerial.best), Ms(mFull.best),
        mFull.best ? (double)mSerial.best / mFull.best : 0.0,
        mFull.info.moduleBytes / (1024.0 * 1024.0));

    // -- MEMORY --------------------------------------------------------------
    Out("\n------------------------------------------------------------\n");
    Out(" MEMORY  (MemStats, KB; from the best run of each config)\n");
    Out("------------------------------------------------------------\n");
    Out("  %-14s %8s %8s %8s %8s %8s %8s %8s %8s | %9s\n",
        "config", "rawPDB", "streams", "fnTable", "argTable",
        "glbTable", "structTbl", "fieldTbl", "arena", "TOTAL");
    for (const auto& r : matrix)
    {
        const auto& m = r.m.mem;
        Out("  %-14s %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f | %9.1f\n",
            r.label, m.rawPDB / 1024.0, m.streamStorage / 1024.0,
            m.functionTable / 1024.0, m.argumentTable / 1024.0,
            m.globalTable / 1024.0, m.structTable / 1024.0, m.fieldTable / 1024.0,
            m.stringArena / 1024.0, m.Total() / 1024.0);
    }
    const auto& memNames        = mNames.mem;
    const auto& memFull         = mFull.mem;
    const auto& memFullNoIntern = find("full-noIntern").mem;
    Out("  interning saves %.1f KB (%.1fx smaller); fn-names is %.1f KB "
        "vs %.1f KB everything (%.1fx less)\n",
        (memFullNoIntern.Total() - memFull.Total()) / 1024.0,
        memFull.Total() ? (double)memFullNoIntern.Total() / memFull.Total() : 0.0,
        memNames.Total() / 1024.0, memFull.Total() / 1024.0,
        memNames.Total() ? (double)memFull.Total() / memNames.Total() : 0.0);
    Out("  everything tier: %zu functions, %zu args, %zu globals, %zu structs, %zu fields\n",
        memFull.functionCount, memFull.argumentCount,
        memFull.globalCount, memFull.structCount, memFull.fieldCount);

    // -- COUNTS (records actually produced per config) -----------------------
    Out("\n------------------------------------------------------------\n");
    Out(" COUNTS  (records produced per config)\n");
    Out("------------------------------------------------------------\n");
    Out("  %-14s %10s %10s %10s %10s %10s\n",
        "config", "functions", "args", "globals", "structs", "fields");
    for (const auto& r : matrix)
    {
        const auto& m = r.m.mem;
        Out("  %-14s %10zu %10zu %10zu %10zu %10zu\n",
            r.label, m.functionCount, m.argumentCount,
            m.globalCount, m.structCount, m.fieldCount);
    }

    // -- FIXTURE matrix: arguments cost only shows on a PDB that HAS args -----
    //  ntdll is a public PDB with zero private argument info, so its argTable is
    //  always 0. The private fixture exercises the argument path, so we report a
    //  small matrix for it too -- this is where "just arguments" has a real cost.
    if (!fixture.empty())
    {
        Out("\n------------------------------------------------------------\n");
        Out(" FIXTURE matrix (private PDB with argument info; KB / ms)\n");
        Out("------------------------------------------------------------\n");
        Out("  %-14s %8s %8s %8s | %8s %8s %8s\n",
            "config", "best", "fnTbl", "argTbl", "structTbl", "fieldTbl", "TOTAL");
        struct FRow { const char* label; ParseOptions o; };
        const FRow frows[] = {
            { "fn-names",      mk(1, 0, 0, 0, 0) },
            { "fn+sizes",      mk(1, 1, 0, 0, 0) },
            { "fn+args",       mk(1, 0, 1, 0, 0) },
            { "fn+sizes+args", mk(1, 1, 1, 0, 0) },
            { "structs",       mk(0, 0, 0, 0, 1) },
            { "everything",    mk(1, 1, 1, 1, 1) },
        };
        for (const auto& r : frows)
        {
            const auto fm = MeasureParse(fixture, r.o, N);
            const auto& m = fm.mem;
            Out("  %-14s %8.3f %8.1f %8.1f | %8.1f %8.1f %8.1f\n",
                r.label, Ms(fm.best),
                m.functionTable / 1024.0, m.argumentTable / 1024.0,
                m.structTable / 1024.0, m.fieldTable / 1024.0, m.Total() / 1024.0);
        }
    }

    // Lookup microbenchmark --------------------------------------------------
    {
        auto copy = pub;
        auto pdb = MemPDB::PDB::ParseFromMemory(std::move(copy), cfgFull);
        const int M = 500000;
        uint64_t sink = 0;
        for (int i = 0; i < 1000; ++i) { auto f = pdb.TryResolveFunction("NtCreateFile"); if (f) sink += f->RVA; }
        const uint64_t a = bench::RdtscStart();
        for (int i = 0; i < M; ++i) { auto f = pdb.TryResolveFunction("NtCreateFile"); if (f) sink += f->RVA; }
        const uint64_t b = bench::RdtscEnd();
        Out("\n  lookup: %.1f ns/call (%d calls over %zu fns)%s\n",
            Nsf(double(b - a) / M), M, pdb.FunctionCount(),
            sink == 0 ? " [!]" : "");
        // Per-record footprint (compile-time facts; portable across runs).
        Out("  per-record bytes: Function=%zu Argument=%zu Global=%zu Struct=%zu Field=%zu\n",
            sizeof(MemPDB::Function), sizeof(MemPDB::Argument), sizeof(MemPDB::Global),
            sizeof(MemPDB::Struct), sizeof(MemPDB::Field));
    }

    // =========================================================================
    Out("\n============================================================\n");
    Out(" RESULT: %d passed, %d failed\n", g_pass, g_fail);
    Out("============================================================\n");
    if (g_rep) std::fclose(g_rep);
    return g_fail == 0 ? 0 : 1;
}
