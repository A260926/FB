#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace MemPDB
{
    class Error : public std::runtime_error
    {
    public:
        explicit Error(const std::string& msg) : std::runtime_error(msg) {}
        explicit Error(const char* msg)        : std::runtime_error(msg) {}
    };

    // Category of a non-function public (data) symbol, inferred from its MSVC
    // name decoration. Classification is a cheap prefix test done during the
    // single symbol scan -- it adds no extra pass and no per-record storage.
    enum class SymbolKind : uint8_t
    {
        Variable,       // ordinary global/static data           (?name@@3..A)
        Constant,       // const-qualified global data           (?name@@3..B/..D)
        Vtable,         // virtual function table                (??_7..@@6B@)
        VbTable,        // virtual base table                    (??_8..)
        Rtti,           // RTTI descriptor / object locator      (??_R0..??_R4)
        StringLiteral,  // compiler-emitted string literal       (??_C..)
        Unknown,        // data symbol that matched none of above
    };
    inline constexpr unsigned kSymbolKindCount = 7;

    // Human-readable name for a SymbolKind ("Vtable", "StringLiteral", ...).
    const char* ToString(SymbolKind kind) noexcept;

    // Bit for a kind, for ParseOptions::GlobalKindMask. e.g.
    //   opt.GlobalKindMask = KindBit(SymbolKind::Vtable) | KindBit(SymbolKind::Rtti);
    inline constexpr uint32_t KindBit(SymbolKind k) noexcept
    {
        return 1u << static_cast<unsigned>(k);
    }
    inline constexpr uint32_t kAllKinds = (1u << kSymbolKindCount) - 1u;

    // Controls how much of the PDB is decoded. Each flag turns an independent
    // body of work on or off, so you only pay the time and memory for what you
    // ask for. The data sources are:
    //   ResolveFunctions : public symbol stream  -> function name + RVA
    //   ResolveSizes     : module proc records   -> function Size  (needs functions)
    //   ResolveArguments : TPI + module streams  -> argument names/types
    //   ResolveGlobals   : public symbol stream  -> data-symbol name + RVA + kind
    //   ResolveStructs   : TPI                   -> struct/class/union layouts
    //
    // Use the named presets below for the common cases, or set flags directly.
    // Example; functions only, smallest footprint:
    //     auto pdb = MemPDB::PDB::ParseFromMemory(std::move(buf),
    //                                             MemPDB::ParseOptions::FunctionsOnly());
    struct ParseOptions
    {
        bool ResolveFunctions = true;  // build the function name->RVA table
        bool ResolveSizes     = true;  // decode function sizes from proc records
        bool ResolveArguments = true;  // decode argument names/types (implies TPI)
        bool ResolveGlobals   = true;  // build the global (data-symbol) name->RVA table
                                       // (public symbols NOT marked fFunction)
        // When ResolveGlobals is set, only keep globals whose SymbolKind bit is
        // present here. Excluded kinds are never stored -- this is the knob for
        // "vtables only", "drop string literals to save memory", etc. Default:
        // keep every kind. Build with KindBit(SymbolKind::...).
        uint32_t GlobalKindMask = kAllKinds;
        bool ResolveStructs   = true;  // decode struct/class/union layouts (implies TPI)
        bool InternStrings    = true;  // copy referenced strings into a compact
                                       // arena and free the raw PDB + stream
                                       // buffers (large steady-state memory win)
        bool Parallel         = true;  // parse module symbol streams across threads

        // ---- presets ----------------------------------------------------------

        // Everything (the default): functions + sizes + arguments + structs.
        static ParseOptions Everything() noexcept { return {}; }

        // Just function names and RVAs -- smallest result for function lookup.
        static ParseOptions FunctionsOnly() noexcept
        {
            ParseOptions o;
            o.ResolveSizes = o.ResolveArguments = o.ResolveGlobals = o.ResolveStructs = false;
            return o;
        }

        // Function names, RVAs, and sizes (no args, no globals, no structs).
        static ParseOptions FunctionsWithSizes() noexcept
        {
            ParseOptions o;
            o.ResolveArguments = o.ResolveGlobals = o.ResolveStructs = false;
            return o;
        }

        // Functions and globals (all named public symbols with their RVAs).
        static ParseOptions NamesAndRVAs() noexcept
        {
            ParseOptions o;
            o.ResolveSizes = o.ResolveArguments = o.ResolveStructs = false;
            return o;
        }

        // Just struct/class/union layouts; function and global tables are skipped.
        static ParseOptions StructsOnly() noexcept
        {
            ParseOptions o;
            o.ResolveFunctions = o.ResolveSizes = o.ResolveArguments = o.ResolveGlobals = false;
            return o;
        }

        // Just the global (data-symbol) table; no functions, no structs.
        static ParseOptions GlobalsOnly() noexcept
        {
            ParseOptions o;
            o.ResolveFunctions = o.ResolveSizes = o.ResolveArguments = o.ResolveStructs = false;
            return o;
        }

        // Only globals of the given kind(s); nothing else. Cheapest way to pull
        // a single category, e.g. GlobalsOfKind(KindBit(SymbolKind::Vtable)).
        static ParseOptions GlobalsOfKind(uint32_t kindMask) noexcept
        {
            ParseOptions o = GlobalsOnly();
            o.GlobalKindMask = kindMask;
            return o;
        }
    };

    struct Config
    {
        std::string  SymbolServer = "https://msdl.microsoft.com/download/symbols";
        ParseOptions Options      = {};
    };

    // Per-phase timing returned by PDB::GetParseInfo()
    struct ParseInfo
    {
        double msMSFDBI        = 0.0; // MSF container + DBI/section-header setup
        double msTPI           = 0.0; // type-info stream
        double msPublicSymbols = 0.0; // public symbol stream
        double msModuleStreams = 0.0; // per-module debug streams
        double msSort          = 0.0; // sort + index build
        double msStructs       = 0.0; // struct/class/union layout decode
        double msIntern        = 0.0; // string interning + buffer release
        double msTotal         = 0.0; // wall time for full parse
        std::size_t typeCount   = 0;  // types parsed from TPI
        std::size_t symbolCount = 0;  // raw public symbols seen
        std::size_t moduleCount = 0;  // module streams processed
        std::size_t moduleBytes = 0;  // total bytes across module symbol streams
        std::size_t procsParsed = 0;  // proc records found across module streams
        std::size_t procMatches = 0;  // procs matched to a public function
        std::size_t structCount = 0;  // struct/class/union layouts decoded
    };

    // Memory breakdown returned by PDB::MemoryUsage()
    struct MemStats
    {
        std::size_t rawPDB        = 0; // raw PDB buffer (bytes)
        std::size_t streamStorage = 0; // copied fragmented streams (bytes)
        std::size_t functionTable = 0; // function index array (bytes)
        std::size_t argumentTable = 0; // argument record array (bytes)
        std::size_t globalTable   = 0; // global-variable index array (bytes)
        std::size_t structTable   = 0; // struct index array (bytes)
        std::size_t fieldTable    = 0; // struct-member record array (bytes)
        std::size_t stringArena   = 0; // interned name/type strings (bytes)
        std::size_t functionCount = 0; // number of indexed functions
        std::size_t argumentCount = 0; // number of indexed arguments
        std::size_t globalCount   = 0; // number of indexed global variables
        std::size_t structCount   = 0; // number of indexed structs
        std::size_t fieldCount    = 0; // number of indexed struct members

        std::size_t Total() const noexcept
        {
            return rawPDB + streamStorage + functionTable + argumentTable
                 + globalTable + structTable + fieldTable + stringArena;
        }
    };

    struct Argument
    {
        std::string_view Name;
        std::string_view TypeName;
        uint32_t         Size;
        uint32_t         Offset;
    };

    struct Function
    {
        std::string_view          Name;
        uint32_t                  RVA;
        uint32_t                  Size;
        uint32_t                  ArgumentCount;
        std::span<const Argument> Arguments;
    };

    // A named global variable from the public symbol stream.
    // These are data symbols whose PublicSymFlags fFunction bit is NOT set --
    // e.g. exported global pointers, named data tables, vtable pointers,
    // TLS variables, jump tables, and any other non-callable named address.
    struct Global
    {
        std::string_view Name; // symbol name (mangled if C++)
        uint32_t         RVA;  // relative virtual address
        SymbolKind       Kind; // category (Vtable, Rtti, StringLiteral, ...)
    };

    // A single data member of a struct/class/union.
    struct Field
    {
        std::string_view Name;     // member name
        std::string_view TypeName; // rendered type, e.g. "int", "Foo*", "char[16]"
        uint32_t         Offset;   // byte offset within the aggregate
        uint32_t         Size;     // size of the member's type in bytes (0 if unknown)
    };

    // A struct/class/union with its full member layout.
    struct Struct
    {
        std::string_view       Name;
        uint32_t               Size;       // total size in bytes
        uint32_t               FieldCount;
        std::span<const Field> Fields;
    };

    class SymbolServer
    {
    public:
        explicit SymbolServer(Config config = {});

        std::vector<std::byte> Download(std::string_view pdbName,
                                        std::string_view guidAge) const;

    private:
        Config config_;
    };

    class PDB
    {
    public:
        PDB(const PDB&)            = delete;
        PDB& operator=(const PDB&) = delete;
        PDB(PDB&&)                 noexcept;
        PDB& operator=(PDB&&)      noexcept;
        ~PDB();

        static PDB Parse(std::string_view pdbName,
                         std::string_view guidAge);

        static PDB Parse(std::string_view pdbName,
                         std::string_view guidAge,
                         const Config&    config);

        static PDB Parse(std::string_view pdbName,
                         std::string_view guidAge,
                         std::string_view symbolServer);

        static PDB ParseFromMemory(std::vector<std::byte> data);
        static PDB ParseFromMemory(std::vector<std::byte> data,
                                   const ParseOptions&    options);

        Function                  ResolveFunction(std::string_view name) const;
        std::optional<Function>   TryResolveFunction(std::string_view name) const;

        // Enumerate the full (name-sorted) function table.
        std::size_t               FunctionCount() const noexcept;
        Function                  FunctionAt(std::size_t index) const;

        // Global-variable lookup (requires ParseOptions::ResolveGlobals).
        // Returns data symbols (fFunction bit NOT set): pointers, tables, vtables, etc.
        Global                    ResolveGlobal(std::string_view name) const;
        std::optional<Global>     TryResolveGlobal(std::string_view name) const;

        // Enumerate the full (name-sorted) global table. Each Global carries its
        // .Kind, so you can filter in place without re-scanning.
        std::size_t               GlobalCount() const noexcept;
        Global                    GlobalAt(std::size_t index) const;

        // How many globals fall in a given category (O(1); precomputed).
        std::size_t               GlobalCountOfKind(SymbolKind kind) const noexcept;

        // Materialize just one category (e.g. all vtables), name-sorted.
        // Allocates only when called -- nothing is pre-split at parse time, so
        // categories you never ask for cost no memory.
        std::vector<Global>       GlobalsOfKind(SymbolKind kind) const;

        // Struct/class/union layout lookup (requires ParseOptions::ResolveStructs).
        Struct                    ResolveStruct(std::string_view name) const;
        std::optional<Struct>     TryResolveStruct(std::string_view name) const;

        // Enumerate the full (name-sorted) struct table.
        std::size_t               StructCount() const noexcept;
        Struct                    StructAt(std::size_t index) const;

        // Returns true if the PDB is still loaded (not yet Shutdown).
        bool      IsLoaded()     const noexcept;

        // Per-phase timing recorded during the last parse.
        ParseInfo GetParseInfo() const noexcept;

        // Live breakdown of heap bytes allocated by this instance.
        MemStats  MemoryUsage()  const noexcept;

        // Frees all internal buffers immediately.
        // After this call IsLoaded() == false and TryResolveFunction returns nullopt.
        void      Shutdown()     noexcept;

    private:
        PDB() = default;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
