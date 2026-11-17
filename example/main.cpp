#include <MemPDB/MemPDB.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

using Clock = std::chrono::high_resolution_clock;

static double Ms(Clock::time_point a, Clock::time_point b) noexcept
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static std::string FmtKB(std::size_t bytes) noexcept
{
    if (bytes == 0) return "0 KB";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    return buf;
}

static void PrintParseInfo(const MemPDB::ParseInfo& p)
{
    std::cout << "  ---- parse breakdown ----\n"
              << "    MSF/DBI setup:    " << std::fixed << std::setprecision(3)
              << p.msMSFDBI        << " ms\n"
              << "    TPI (types):      " << p.msTPI           << " ms"
              << "  (" << p.typeCount << " types)\n"
              << "    Public symbols:   " << p.msPublicSymbols << " ms"
              << "  (" << p.symbolCount << " symbols)\n"
              << "    Sort + index:     " << p.msSort          << " ms\n"
              << "    Module streams:   " << p.msModuleStreams  << " ms"
              << "  (" << p.moduleCount << " modules)\n"
              << "    Total parse:      " << p.msTotal         << " ms\n";
}

static void PrintMemStats(const MemPDB::MemStats& m)
{
    std::cout << "  ---- memory usage ----\n"
              << "    Raw PDB buffer:   " << FmtKB(m.rawPDB)        << "\n"
              << "    Stream fragments: " << FmtKB(m.streamStorage) << "\n"
              << "    Function table:   " << FmtKB(m.functionTable)
              << "  (" << m.functionCount << " functions)\n"
              << "    Argument table:   " << FmtKB(m.argumentTable)
              << "  (" << m.argumentCount << " arguments)\n"
              << "    TOTAL:            " << FmtKB(m.Total())       << "\n";
}

// -------------------------------------------------------------------------
static void Demo1()
{
    std::cout << "\n=== [Demo1] Download + parse ntdll.pdb from Microsoft symbols ===\n";
    try
    {
        MemPDB::Config cfg;
        cfg.SymbolServer = "https://msdl.microsoft.com/download/symbols";
        MemPDB::SymbolServer srv(cfg);

        // Step 1 - Download
        std::cout << "  [1/5] Downloading...\n";
        const auto td0 = Clock::now();
        auto rawData = srv.Download("ntdll.pdb", "180BF1B90AA75697D0EFEA5E5630AC7E1");
        const auto td1 = Clock::now();
        std::cout << "        downloaded " << FmtKB(rawData.size())
                  << " in " << std::fixed << std::setprecision(2)
                  << Ms(td0, td1) << " ms\n";

        // Save locally for Demo4
        {
            std::ofstream f("ntdll.pdb", std::ios::binary);
            f.write(reinterpret_cast<const char*>(rawData.data()),
                    static_cast<std::streamsize>(rawData.size()));
            std::cout << "        saved to ntdll.pdb\n";
        }

        // Step 2 - Parse
        std::cout << "  [2/5] Parsing...\n";
        const auto tp0 = Clock::now();
        auto pdb = MemPDB::PDB::ParseFromMemory(std::move(rawData));
        const auto tp1 = Clock::now();
        std::cout << "        parsed in " << Ms(tp0, tp1) << " ms\n";
        PrintParseInfo(pdb.GetParseInfo());

        // Step 3 - Memory snapshot
        std::cout << "  [3/5] Memory after parse:\n";
        PrintMemStats(pdb.MemoryUsage());

        // Step 4 - Resolve symbols
        std::cout << "  [4/5] Resolving symbols...\n";
        {
            const auto ta = Clock::now();
            const auto fn = pdb.TryResolveFunction("NtQuerySystemInformation");
            const auto tb = Clock::now();
            std::cout << "        first lookup:   " << std::setprecision(4)
                      << Ms(ta, tb) << " ms\n";
            if (fn)
            {
                std::cout << "        NtQuerySystemInformation  RVA=0x"
                          << std::hex << fn->RVA << std::dec
                          << "  size=" << fn->Size
                          << "  args=" << fn->ArgumentCount << "\n";
                for (uint32_t i = 0; i < fn->ArgumentCount; ++i)
                    std::cout << "          [" << i << "] "
                              << fn->Arguments[i].TypeName << " "
                              << fn->Arguments[i].Name << "\n";
            }
            else
            {
                std::cout << "        NtQuerySystemInformation: not found\n";
            }
        }
        {
            const auto ta = Clock::now();
            for (int i = 0; i < 1000; ++i)
                pdb.TryResolveFunction("NtCreateFile");
            const auto tb = Clock::now();
            const double total = Ms(ta, tb);
            std::cout << "        1000x batch:    " << std::setprecision(4)
                      << total << " ms  ("
                      << std::setprecision(6) << total / 1000.0 << " ms each)\n";
        }

        // Step 5 - Shutdown
        std::cout << "  [5/5] Shutdown...\n";
        const MemPDB::MemStats beforeShutdown = pdb.MemoryUsage();
        pdb.Shutdown();
        const MemPDB::MemStats afterShutdown  = pdb.MemoryUsage();

        std::cout << "        IsLoaded() = " << (pdb.IsLoaded() ? "true" : "false") << "\n"
                  << "        Memory before: " << FmtKB(beforeShutdown.Total()) << "\n"
                  << "        Memory after:  " << FmtKB(afterShutdown.Total())  << "\n"
                  << "        Freed:         " << FmtKB(beforeShutdown.Total()) << "\n";

        const auto fn = pdb.TryResolveFunction("NtCreateFile");
        std::cout << "        TryResolve after shutdown: "
                  << (fn.has_value() ? "returned data (BUG)" : "nullopt (correct)") << "\n";
    }
    catch (const MemPDB::Error& e)
    {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// -------------------------------------------------------------------------
static void Demo2()
{
    std::cout << "\n=== [Demo2] Custom symbol server (expected 404) ===\n";
    try
    {
        auto pdb = MemPDB::PDB::Parse(
            "GameAssembly.pdb",
            "AABBCCDDEEFF00112233445566778899A",
            "https://example.com/unity/symbols");
        auto fn = pdb.TryResolveFunction("il2cpp_init");
        if (fn)
            std::cout << "  il2cpp_init RVA=0x" << std::hex << fn->RVA << "\n";
        else
            std::cout << "  symbol not found\n";
    }
    catch (const MemPDB::Error& e)
    {
        std::cout << "  Error (expected): " << e.what() << "\n";
    }
}

// -------------------------------------------------------------------------
static void Demo3()
{
    std::cout << "\n=== [Demo3] Config-based parse ===\n";
    try
    {
        MemPDB::Config config;
        config.SymbolServer = "https://msdl.microsoft.com/download/symbols";

        const auto t0 = Clock::now();
        auto pdb = MemPDB::PDB::Parse("ntdll.pdb", "180BF1B90AA75697D0EFEA5E5630AC7E1", config);
        const auto t1 = Clock::now();

        std::cout << "  total (download+parse): "
                  << std::fixed << std::setprecision(2) << Ms(t0, t1) << " ms\n";
        PrintMemStats(pdb.MemoryUsage());

        auto fn = pdb.TryResolveFunction("LdrLoadDll");
        if (fn)
            std::cout << "  LdrLoadDll  RVA=0x" << std::hex << fn->RVA << std::dec
                      << "  size=" << fn->Size << "\n";
        else
            std::cout << "  LdrLoadDll: not found\n";
    }
    catch (const MemPDB::Error& e)
    {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// -------------------------------------------------------------------------
static void Demo4()
{
    std::cout << "\n=== [Demo4] ParseFromMemory (local file) ===\n";
    try
    {
        std::ifstream file("ntdll.pdb", std::ios::binary | std::ios::ate);
        if (!file)
        {
            std::cout << "  ntdll.pdb not found locally -- run Demo1 first\n";
            return;
        }

        const auto fileSize = static_cast<std::size_t>(file.tellg());
        file.seekg(0);
        std::vector<std::byte> bytes(fileSize);
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(fileSize));
        file.close();
        std::cout << "  File read:  " << FmtKB(fileSize) << "\n";

        const auto t0 = Clock::now();
        auto pdb = MemPDB::PDB::ParseFromMemory(std::move(bytes));
        const auto t1 = Clock::now();
        std::cout << "  Parse time: " << std::fixed << std::setprecision(3)
                  << Ms(t0, t1) << " ms\n";
        PrintParseInfo(pdb.GetParseInfo());
        PrintMemStats(pdb.MemoryUsage());

        {
            const auto ta = Clock::now();
            auto fn = pdb.TryResolveFunction("NtCreateFile");
            const auto tb = Clock::now();
            if (fn)
                std::cout << "  NtCreateFile  RVA=0x" << std::hex << fn->RVA << std::dec
                          << "  size=" << fn->Size
                          << "  args=" << fn->ArgumentCount << "\n";
            else
                std::cout << "  NtCreateFile: not found\n";
            std::cout << "  Resolve: " << std::setprecision(4) << Ms(ta, tb) << " ms\n";
        }

        std::cout << "  Calling Shutdown()...\n";
        const std::size_t before = pdb.MemoryUsage().Total();
        pdb.Shutdown();
        std::cout << "  IsLoaded() = " << (pdb.IsLoaded() ? "true" : "false") << "\n"
                  << "  Freed " << FmtKB(before) << " -- all clear\n";
    }
    catch (const MemPDB::Error& e)
    {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// -------------------------------------------------------------------------
// Tiered parsing: ask only for what you need. "names-only" skips the TPI and
// module-stream phases entirely and frees the raw buffers via string interning,
// so it is both faster and far smaller when all you want is name -> RVA.
static void Demo5()
{
    std::cout << "\n=== [Demo5] Tiered parsing (ParseOptions) ===\n";
    try
    {
        std::ifstream file("ntdll.pdb", std::ios::binary | std::ios::ate);
        if (!file)
        {
            std::cout << "  ntdll.pdb not found locally -- run Demo1 first\n";
            return;
        }
        const auto fileSize = static_cast<std::size_t>(file.tellg());
        file.seekg(0);
        std::vector<std::byte> base(fileSize);
        file.read(reinterpret_cast<char*>(base.data()),
                  static_cast<std::streamsize>(fileSize));

        // Cheapest tier: function names + RVAs only (no sizes/args/structs).
        {
            auto bytes = base;
            const auto t0 = Clock::now();
            auto pdb = MemPDB::PDB::ParseFromMemory(
                std::move(bytes), MemPDB::ParseOptions::FunctionsOnly());
            const auto t1 = Clock::now();
            auto fn = pdb.TryResolveFunction("NtCreateFile");
            std::cout << "  functions:   parse " << std::fixed << std::setprecision(3)
                      << Ms(t0, t1) << " ms, resident " << FmtKB(pdb.MemoryUsage().Total())
                      << ", NtCreateFile RVA=0x" << std::hex << (fn ? fn->RVA : 0) << std::dec
                      << "\n";
        }

        // Structs only: skips the function table entirely.
        {
            auto bytes = base;
            const auto t0 = Clock::now();
            auto pdb = MemPDB::PDB::ParseFromMemory(
                std::move(bytes), MemPDB::ParseOptions::StructsOnly());
            const auto t1 = Clock::now();
            std::cout << "  structs:     parse " << Ms(t0, t1) << " ms, resident "
                      << FmtKB(pdb.MemoryUsage().Total())
                      << ", " << pdb.StructCount() << " structs, "
                      << pdb.FunctionCount() << " functions\n";
        }

        // Full tier (default): sizes + argument names/types + struct layouts.
        {
            auto bytes = base;
            const auto t0 = Clock::now();
            auto pdb = MemPDB::PDB::ParseFromMemory(std::move(bytes));
            const auto t1 = Clock::now();
            std::cout << "  full:       parse " << Ms(t0, t1) << " ms, resident "
                      << FmtKB(pdb.MemoryUsage().Total())
                      << ", " << pdb.StructCount() << " structs\n";

            // Show one struct's full member layout (name, type, offset).
            if (auto s = pdb.TryResolveStruct("_PEB"))
            {
                std::cout << "  struct _PEB (size=" << s->Size << ", "
                          << s->FieldCount << " fields), first few:\n";
                for (uint32_t i = 0; i < s->FieldCount && i < 6; ++i)
                    std::cout << "        +0x" << std::hex << s->Fields[i].Offset << std::dec
                              << "  " << s->Fields[i].TypeName
                              << " " << s->Fields[i].Name << "\n";
            }
        }

        // Globals by category -- full per-kind control. Categories are tagged at
        // parse time; pull a whole category, or parse just one to save memory.
        {
            auto bytes = base;
            auto pdb = MemPDB::PDB::ParseFromMemory(
                std::move(bytes), MemPDB::ParseOptions::GlobalsOnly());
            std::cout << "  globals:     " << pdb.GlobalCount() << " total  ["
                      << "vtables="  << pdb.GlobalCountOfKind(MemPDB::SymbolKind::Vtable)
                      << " rtti="    << pdb.GlobalCountOfKind(MemPDB::SymbolKind::Rtti)
                      << " strings=" << pdb.GlobalCountOfKind(MemPDB::SymbolKind::StringLiteral)
                      << " consts="  << pdb.GlobalCountOfKind(MemPDB::SymbolKind::Constant)
                      << " vars="    << pdb.GlobalCountOfKind(MemPDB::SymbolKind::Variable)
                      << "]\n";
            const auto vtables = pdb.GlobalsOfKind(MemPDB::SymbolKind::Vtable);
            std::cout << "  first vtables:\n";
            for (std::size_t i = 0; i < vtables.size() && i < 3; ++i)
                std::cout << "        RVA=0x" << std::hex << vtables[i].RVA << std::dec
                          << "  " << vtables[i].Name << "\n";
        }

        // Vtables ONLY -- cheapest way to pull a single category; nothing else
        // is stored, so memory stays minimal.
        {
            auto bytes = base;
            const auto t0 = Clock::now();
            auto pdb = MemPDB::PDB::ParseFromMemory(
                std::move(bytes),
                MemPDB::ParseOptions::GlobalsOfKind(MemPDB::KindBit(MemPDB::SymbolKind::Vtable)));
            const auto t1 = Clock::now();
            std::cout << "  vtables-only: parse " << Ms(t0, t1) << " ms, resident "
                      << FmtKB(pdb.MemoryUsage().Total())
                      << ", " << pdb.GlobalCount() << " vtables\n";
        }
    }
    catch (const MemPDB::Error& e)
    {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// -------------------------------------------------------------------------
int main()
{
    Demo1();
    Demo2();
    Demo3();
    Demo4();
    Demo5();
    std::cout << "\nDone.\n";
#if defined(_WIN32)
    if (!std::getenv("MEMPDB_NO_PAUSE"))
        (void)std::system("pause");
#endif
    return 0;
}
