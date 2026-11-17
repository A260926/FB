#define _CRT_SECURE_NO_WARNINGS
// =============================================================================
//  pdb2json -- drag-and-drop PDB -> JSON exporter, built on MemPDB.
//
//  Drop a .pdb file onto the executable (or run `pdb2json <file.pdb>`). It
//  parses the PDB and writes a sibling folder "<name>_dump/" containing:
//
//      metadata.json            source, image base, counts, parse timing, memory
//      index.json               manifest of the generated files + record counts
//      functions.json           every function: name, RVA, VA, size
//      function_args.json       every function with its argument list
//      structures.json          every struct/class/union with its field layout
//      globals.json             all data globals: name, RVA, VA, kind
//      globals_variables.json   subset: plain data variables
//      globals_vtables.json     subset: vtable symbols         (??_7...)
//      globals_vbtables.json    subset: virtual base tables    (??_8...)
//      globals_rtti.json        subset: RTTI descriptors       (??_R...)
//      globals_strings.json     subset: string literals        (??_C...)
//      globals_constants.json   subset: const-qualified data   (??_...)
//
//  Per-category files are only written when that category is non-empty.
//
//  Virtual addresses are computed as VA = ImageBase + RVA. PDBs do not record
//  the runtime base (ASLR chooses it), so a default x64 base of 0x180000000 is
//  used. Override it:  pdb2json <file.pdb> <imageBaseHex>
//  e.g.  pdb2json ntdll.pdb 0x7FFE0000000
// =============================================================================
#include <MemPDB/MemPDB.hpp>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

// ---- small helpers ----------------------------------------------------------
static std::string HexU(uint64_t v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

// Write a JSON string literal (with proper escaping) to a stream.
static void JsonStr(std::ostream& os, std::string_view s)
{
    os << '"';
    for (char ch : s)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (ch)
        {
        case '"':  os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        case '\n': os << "\\n";  break;
        case '\r': os << "\\r";  break;
        case '\t': os << "\\t";  break;
        case '\b': os << "\\b";  break;
        case '\f': os << "\\f";  break;
        default:
            if (c < 0x20)
            {
                char u[8];
                std::snprintf(u, sizeof(u), "\\u%04x", c);
                os << u;
            }
            else os << ch;
        }
    }
    os << '"';
}

static std::string NowIso8601()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_MSC_VER) || defined(__MINGW32__)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    return buf;
}

static std::vector<std::byte> ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::byte> buf(n);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
    return buf;
}

static void Pause()
{
    if (std::getenv("MEMPDB_NO_PAUSE")) return;
    std::cout << "\nPress Enter to close...";
    std::cin.get();
}

// ---- JSON writers -----------------------------------------------------------
// functions.json : [ { name, rva, va, rva_hex, va_hex, size }, ... ]
static std::size_t WriteFunctions(const fs::path& out, const MemPDB::PDB& pdb, uint64_t base)
{
    std::ofstream o(out, std::ios::binary);
    o << "[\n";
    const std::size_t n = pdb.FunctionCount();
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto f = pdb.FunctionAt(i);
        const uint64_t va = base + f.RVA;
        o << "  { \"name\": ";
        JsonStr(o, f.Name);
        o << ", \"rva\": " << f.RVA
          << ", \"va\": "  << va
          << ", \"rva_hex\": \"" << HexU(f.RVA) << "\""
          << ", \"va_hex\": \""  << HexU(va)    << "\""
          << ", \"size\": " << f.Size << " }";
        o << (i + 1 < n ? ",\n" : "\n");
    }
    o << "]\n";
    return n;
}

// function_args.json : [ { name, rva, va, arg_count, args:[{name,type,size,offset}] } ]
static std::size_t WriteFunctionArgs(const fs::path& out, const MemPDB::PDB& pdb, uint64_t base)
{
    std::ofstream o(out, std::ios::binary);
    o << "[\n";
    const std::size_t n = pdb.FunctionCount();
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto f = pdb.FunctionAt(i);
        const uint64_t va = base + f.RVA;
        o << "  { \"name\": ";
        JsonStr(o, f.Name);
        o << ", \"rva\": " << f.RVA
          << ", \"va\": "  << va
          << ", \"arg_count\": " << f.ArgumentCount
          << ", \"args\": [";
        for (uint32_t a = 0; a < f.ArgumentCount; ++a)
        {
            const auto& arg = f.Arguments[a];
            o << (a ? ", " : " ") << "{ \"name\": ";
            JsonStr(o, arg.Name);
            o << ", \"type\": ";
            JsonStr(o, arg.TypeName);
            o << ", \"size\": " << arg.Size
              << ", \"offset\": " << arg.Offset << " }";
        }
        o << (f.ArgumentCount ? " ]" : "]");
        o << " }";
        o << (i + 1 < n ? ",\n" : "\n");
    }
    o << "]\n";
    return n;
}

// structures.json : [ { name, size, field_count, fields:[{name,type,offset,size}] } ]
static std::size_t WriteStructures(const fs::path& out, const MemPDB::PDB& pdb)
{
    std::ofstream o(out, std::ios::binary);
    o << "[\n";
    const std::size_t n = pdb.StructCount();
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto s = pdb.StructAt(i);
        o << "  { \"name\": ";
        JsonStr(o, s.Name);
        o << ", \"size\": " << s.Size
          << ", \"field_count\": " << s.FieldCount
          << ", \"fields\": [";
        for (uint32_t fi = 0; fi < s.FieldCount; ++fi)
        {
            const auto& fld = s.Fields[fi];
            o << (fi ? ",\n      " : "\n      ") << "{ \"name\": ";
            JsonStr(o, fld.Name);
            o << ", \"type\": ";
            JsonStr(o, fld.TypeName);
            o << ", \"offset\": " << fld.Offset
              << ", \"offset_hex\": \"" << HexU(fld.Offset) << "\""
              << ", \"size\": " << fld.Size << " }";
        }
        o << (s.FieldCount ? "\n    ]" : "]");
        o << " }";
        o << (i + 1 < n ? ",\n" : "\n");
    }
    o << "]\n";
    return n;
}

// Writes one global record as a JSON object (no trailing comma or newline).
static void WriteGlobalRecord(std::ostream& o, const MemPDB::Global& g, uint64_t base)
{
    const uint64_t va = base + g.RVA;
    o << "{ \"name\": ";
    JsonStr(o, g.Name);
    o << ", \"rva\": " << g.RVA
      << ", \"va\": "  << va
      << ", \"rva_hex\": \"" << HexU(g.RVA) << "\""
      << ", \"va_hex\": \""  << HexU(va)    << "\""
      << ", \"kind\": \""    << MemPDB::ToString(g.Kind) << "\" }";
}

// globals.json : all data globals [ { name, rva, va, rva_hex, va_hex, kind }, ... ]
static std::size_t WriteGlobals(const fs::path& out, const MemPDB::PDB& pdb, uint64_t base)
{
    std::ofstream o(out, std::ios::binary);
    o << "[\n";
    const std::size_t n = pdb.GlobalCount();
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto g = pdb.GlobalAt(i);
        o << "  ";
        WriteGlobalRecord(o, g, base);
        o << (i + 1 < n ? ",\n" : "\n");
    }
    o << "]\n";
    return n;
}

// globals_<category>.json : filtered subset for one SymbolKind
static std::size_t WriteGlobalsOfKind(const fs::path& out, const MemPDB::PDB& pdb,
                                      MemPDB::SymbolKind kind, uint64_t base)
{
    const auto entries = pdb.GlobalsOfKind(kind);
    std::ofstream o(out, std::ios::binary);
    o << "[\n";
    const std::size_t n = entries.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        o << "  ";
        WriteGlobalRecord(o, entries[i], base);
        o << (i + 1 < n ? ",\n" : "\n");
    }
    o << "]\n";
    return n;
}

// ---- main -------------------------------------------------------------------
int main(int argc, char** argv)
{
    std::cout << "pdb2json -- PDB to JSON exporter (MemPDB)\n";

    if (argc < 2)
    {
        std::cout << "\nUsage: drop a .pdb file onto this program, or run:\n"
                  << "    pdb2json <file.pdb> [imageBaseHex]\n\n"
                  << "Default image base for VA = 0x180000000 (x64 DLL).\n";
        Pause();
        return 1;
    }

    const fs::path pdbPath = argv[1];

    // Optional image base override (hex or decimal).
    uint64_t imageBase = 0x180000000ULL;
    if (argc >= 3)
    {
        try { imageBase = std::stoull(argv[2], nullptr, 0); }
        catch (...)
        {
            std::cout << "  Warning: could not parse image base '" << argv[2]
                      << "', using default " << HexU(imageBase) << "\n";
        }
    }

    try
    {
        if (!fs::exists(pdbPath))
            throw std::runtime_error("file not found: " + pdbPath.string());

        std::cout << "  Input:      " << pdbPath.string() << "\n";
        std::cout << "  Image base: " << HexU(imageBase) << "  (VA = base + RVA)\n";

        auto bytes = ReadFile(pdbPath);
        if (bytes.empty())
            throw std::runtime_error("could not read file (empty or unreadable)");
        const std::size_t fileSize = bytes.size();

        std::cout << "  Parsing " << fileSize << " bytes...\n";
        auto pdb = MemPDB::PDB::ParseFromMemory(std::move(bytes),
                                                MemPDB::ParseOptions::Everything());
        const auto info = pdb.GetParseInfo();
        const auto mem  = pdb.MemoryUsage();

        // Output folder: "<pdb-stem>_dump" next to the input file.
        fs::path outDir = pdbPath.parent_path() / (pdbPath.stem().string() + "_dump");
        fs::create_directories(outDir);
        std::cout << "  Output:     " << outDir.string() << "\n\n";

        // Core tables
        const std::size_t nFns     = WriteFunctions   (outDir / "functions.json",     pdb, imageBase);
        const std::size_t nFnArgs  = WriteFunctionArgs(outDir / "function_args.json", pdb, imageBase);
        const std::size_t nStructs = WriteStructures  (outDir / "structures.json",    pdb);
        const std::size_t nGlobals = WriteGlobals      (outDir / "globals.json",       pdb, imageBase);

        // Per-category global subsets - only write when non-empty.
        struct CategoryFile
        {
            const char*         filename;
            const char*         description;
            MemPDB::SymbolKind  kind;
            std::size_t         count;
        };
        CategoryFile cats[] = {
            { "globals_variables.json",  "plain data variables",                MemPDB::SymbolKind::Variable,      0 },
            { "globals_vtables.json",    "vtable symbols (??_7...)",             MemPDB::SymbolKind::Vtable,        0 },
            { "globals_vbtables.json",   "virtual base table symbols (??_8...)", MemPDB::SymbolKind::VbTable,       0 },
            { "globals_rtti.json",       "RTTI descriptors (??_R...)",           MemPDB::SymbolKind::Rtti,          0 },
            { "globals_strings.json",    "string literals (??_C...)",            MemPDB::SymbolKind::StringLiteral, 0 },
            { "globals_constants.json",  "const-qualified data (??_...)",        MemPDB::SymbolKind::Constant,      0 },
        };
        for (auto& c : cats)
        {
            const std::size_t cnt = pdb.GlobalCountOfKind(c.kind);
            if (cnt > 0)
                c.count = WriteGlobalsOfKind(outDir / c.filename, pdb, c.kind, imageBase);
        }

        // metadata.json
        {
            std::ofstream o(outDir / "metadata.json", std::ios::binary);
            o << "{\n"
              << "  \"tool\": \"pdb2json (MemPDB)\",\n"
              << "  \"generated\": \"" << NowIso8601() << "\",\n"
              << "  \"source_file\": ";
            JsonStr(o, pdbPath.filename().string());
            o << ",\n  \"source_bytes\": " << fileSize << ",\n"
              << "  \"image_base\": " << imageBase << ",\n"
              << "  \"image_base_hex\": \"" << HexU(imageBase) << "\",\n"
              << "  \"va_formula\": \"va = image_base + rva\",\n"
              << "  \"counts\": {\n"
              << "    \"functions\": " << pdb.FunctionCount() << ",\n"
              << "    \"arguments\": " << mem.argumentCount   << ",\n"
              << "    \"structs\": "   << pdb.StructCount()   << ",\n"
              << "    \"fields\": "    << mem.fieldCount      << ",\n"
              << "    \"globals\": "   << pdb.GlobalCount()   << ",\n"
              << "    \"globals_by_kind\": {\n"
              << "      \"Variable\": "      << pdb.GlobalCountOfKind(MemPDB::SymbolKind::Variable)      << ",\n"
              << "      \"Constant\": "      << pdb.GlobalCountOfKind(MemPDB::SymbolKind::Constant)      << ",\n"
              << "      \"Vtable\": "        << pdb.GlobalCountOfKind(MemPDB::SymbolKind::Vtable)        << ",\n"
              << "      \"VbTable\": "       << pdb.GlobalCountOfKind(MemPDB::SymbolKind::VbTable)       << ",\n"
              << "      \"Rtti\": "          << pdb.GlobalCountOfKind(MemPDB::SymbolKind::Rtti)          << ",\n"
              << "      \"StringLiteral\": " << pdb.GlobalCountOfKind(MemPDB::SymbolKind::StringLiteral) << ",\n"
              << "      \"Unknown\": "       << pdb.GlobalCountOfKind(MemPDB::SymbolKind::Unknown)       << "\n"
              << "    }\n"
              << "  },\n"
              << "  \"parse_ms\": {\n"
              << "    \"msf_dbi\": "         << info.msMSFDBI        << ",\n"
              << "    \"tpi\": "             << info.msTPI           << ",\n"
              << "    \"public_symbols\": "  << info.msPublicSymbols << ",\n"
              << "    \"sort\": "            << info.msSort          << ",\n"
              << "    \"module_streams\": "  << info.msModuleStreams << ",\n"
              << "    \"structs\": "         << info.msStructs       << ",\n"
              << "    \"intern\": "          << info.msIntern        << ",\n"
              << "    \"total\": "           << info.msTotal         << "\n"
              << "  },\n"
              << "  \"parse_counts\": {\n"
              << "    \"tpi_types\": "       << info.typeCount    << ",\n"
              << "    \"public_symbols\": "  << info.symbolCount  << ",\n"
              << "    \"modules\": "         << info.moduleCount  << ",\n"
              << "    \"module_bytes\": "    << info.moduleBytes  << ",\n"
              << "    \"procs_parsed\": "    << info.procsParsed  << ",\n"
              << "    \"proc_matches\": "    << info.procMatches  << ",\n"
              << "    \"structs_decoded\": " << info.structCount  << "\n"
              << "  },\n"
              << "  \"memory\": {\n"
              << "    \"raw_pdb\": "        << mem.rawPDB        << ",\n"
              << "    \"stream_storage\": " << mem.streamStorage << ",\n"
              << "    \"function_table\": " << mem.functionTable << ",\n"
              << "    \"argument_table\": " << mem.argumentTable << ",\n"
              << "    \"global_table\": "   << mem.globalTable   << ",\n"
              << "    \"struct_table\": "   << mem.structTable   << ",\n"
              << "    \"field_table\": "    << mem.fieldTable    << ",\n"
              << "    \"string_arena\": "   << mem.stringArena   << ",\n"
              << "    \"total\": "          << mem.Total()       << "\n"
              << "  }\n"
              << "}\n";
        }

        // index.json -- manifest
        {
            std::ofstream o(outDir / "index.json", std::ios::binary);
            o << "{\n  \"files\": [\n"
              << "    { \"name\": \"metadata.json\",      \"description\": \"source, image base, counts, parse timing, parse counts, memory breakdown\" },\n"
              << "    { \"name\": \"functions.json\",     \"description\": \"name -> rva/va/size\", \"records\": " << nFns << " },\n"
              << "    { \"name\": \"function_args.json\", \"description\": \"function name + argument list\", \"records\": " << nFnArgs << " },\n"
              << "    { \"name\": \"structures.json\",    \"description\": \"struct/class/union field layouts\", \"records\": " << nStructs << " },\n"
              << "    { \"name\": \"globals.json\",       \"description\": \"all data globals (name/rva/va/kind)\", \"records\": " << nGlobals << " }";
            for (const auto& c : cats)
            {
                if (c.count > 0)
                    o << ",\n    { \"name\": \"" << c.filename << "\", \"description\": \""
                      << c.description << "\", \"records\": " << c.count << " }";
            }
            o << "\n  ]\n}\n";
        }

        std::cout << "  Wrote:\n"
                  << "    functions.json       (" << nFns     << " functions)\n"
                  << "    function_args.json   (" << nFnArgs  << " functions)\n"
                  << "    structures.json      (" << nStructs << " structs)\n"
                  << "    globals.json         (" << nGlobals << " globals)\n";
        for (const auto& c : cats)
        {
            if (c.count > 0)
            {
                std::cout << "    " << c.filename;
                // pad to align the count column
                const int pad = 23 - static_cast<int>(std::string_view(c.filename).size());
                for (int i = 0; i < pad; ++i) std::cout << ' ';
                std::cout << " (" << c.count << ")\n";
            }
        }
        std::cout << "    metadata.json\n"
                  << "    index.json\n"
                  << "\n  Parsed in " << info.msTotal << " ms, "
                  << (mem.Total() / 1024) << " KB resident.\n"
                  << "  Done.\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "\n  ERROR: " << e.what() << "\n";
        Pause();
        return 2;
    }

    Pause();
    return 0;
}
