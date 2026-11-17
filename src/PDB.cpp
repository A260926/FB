#include <MemPDB/MemPDB.hpp>
#include "PDBParser.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>

namespace MemPDB
{
    const char* ToString(SymbolKind kind) noexcept
    {
        switch (kind)
        {
        case SymbolKind::Variable:      return "Variable";
        case SymbolKind::Constant:      return "Constant";
        case SymbolKind::Vtable:        return "Vtable";
        case SymbolKind::VbTable:       return "VbTable";
        case SymbolKind::Rtti:          return "Rtti";
        case SymbolKind::StringLiteral: return "StringLiteral";
        case SymbolKind::Unknown:       return "Unknown";
        }
        return "Unknown";
    }

    PDB::~PDB()                = default;
    PDB::PDB(PDB&&) noexcept   = default;
    PDB& PDB::operator=(PDB&&) noexcept = default;

    struct PDB::Impl
    {
        std::vector<std::byte>                  data;
        std::vector<std::vector<std::byte>>     streamStorage;
        std::vector<char>                       stringArena;
        std::vector<detail::FunctionRecord>     functions;
        std::vector<Argument>                   arguments;
        std::vector<detail::GlobalRecord>       globals;
        std::array<uint32_t, kSymbolKindCount>  globalKindCounts{};
        std::vector<detail::StructRecord>       structs;
        std::vector<Field>                      fields;
        ParseInfo                               parseInfo;

        Function BuildFunction(uint32_t index) const
        {
            const auto& rec = functions[index];
            Function fn{};
            fn.Name          = rec.name;
            fn.RVA           = rec.rva;
            fn.Size          = rec.size;
            fn.ArgumentCount = rec.argCount;
            if (rec.argCount > 0)
            {
                fn.Arguments = std::span<const Argument>(
                    arguments.data() + rec.argOffset,
                    rec.argCount);
            }
            return fn;
        }

        uint32_t FindFunction(std::string_view name) const noexcept
        {
            const auto lo = std::lower_bound(
                functions.begin(), functions.end(), name,
                [](const detail::FunctionRecord& f, std::string_view n)
                { return f.name < n; });
            if (lo == functions.end() || lo->name != name)
                return static_cast<uint32_t>(-1);
            return static_cast<uint32_t>(lo - functions.begin());
        }

        Global BuildGlobal(uint32_t index) const
        {
            const auto& rec = globals[index];
            Global g{};
            g.Name = rec.name;
            g.RVA  = rec.rva;
            g.Kind = rec.kind;
            return g;
        }

        uint32_t FindGlobal(std::string_view name) const noexcept
        {
            const auto lo = std::lower_bound(
                globals.begin(), globals.end(), name,
                [](const detail::GlobalRecord& g, std::string_view n)
                { return g.name < n; });
            if (lo == globals.end() || lo->name != name)
                return static_cast<uint32_t>(-1);
            return static_cast<uint32_t>(lo - globals.begin());
        }

        Struct BuildStruct(uint32_t index) const
        {
            const auto& rec = structs[index];
            Struct s{};
            s.Name       = rec.name;
            s.Size       = rec.size;
            s.FieldCount = rec.fieldCount;
            if (rec.fieldCount > 0)
            {
                s.Fields = std::span<const Field>(
                    fields.data() + rec.fieldOffset,
                    rec.fieldCount);
            }
            return s;
        }

        uint32_t FindStruct(std::string_view name) const noexcept
        {
            const auto lo = std::lower_bound(
                structs.begin(), structs.end(), name,
                [](const detail::StructRecord& s, std::string_view n)
                { return s.name < n; });
            if (lo == structs.end() || lo->name != name)
                return static_cast<uint32_t>(-1);
            return static_cast<uint32_t>(lo - structs.begin());
        }
    };

    PDB PDB::Parse(std::string_view pdbName, std::string_view guidAge)
    {
        return Parse(pdbName, guidAge, Config{});
    }

    PDB PDB::Parse(std::string_view pdbName,
                   std::string_view guidAge,
                   std::string_view symbolServer)
    {
        Config cfg;
        cfg.SymbolServer = std::string(symbolServer);
        return Parse(pdbName, guidAge, cfg);
    }

    PDB PDB::Parse(std::string_view pdbName,
                   std::string_view guidAge,
                   const Config&    config)
    {
        SymbolServer srv(config);
        auto data = srv.Download(pdbName, guidAge);
        return ParseFromMemory(std::move(data), config.Options);
    }

    PDB PDB::ParseFromMemory(std::vector<std::byte> data)
    {
        return ParseFromMemory(std::move(data), ParseOptions{});
    }

    PDB PDB::ParseFromMemory(std::vector<std::byte> data, const ParseOptions& options)
    {
        if (data.empty())
            throw Error("PDB: empty data buffer");

        auto impl = std::make_unique<Impl>();
        impl->data = std::move(data);

        const std::span<const std::byte> view(impl->data.data(), impl->data.size());
        auto parsed = detail::ParsePDB(view, options);

        impl->streamStorage    = std::move(parsed.streamStorage);
        impl->functions        = std::move(parsed.functions);
        impl->arguments        = std::move(parsed.arguments);
        impl->globals          = std::move(parsed.globals);
        impl->globalKindCounts = parsed.globalKindCounts;
        impl->structs          = std::move(parsed.structs);
        impl->fields           = std::move(parsed.fields);
        impl->stringArena      = std::move(parsed.stringArena);
        impl->parseInfo        = parsed.timing;

        if (parsed.internedStrings)
        {
            // Every string_view now points into stringArena; the raw PDB buffer
            // and the fragmented stream copies are no longer referenced.
            impl->data.clear();
            impl->data.shrink_to_fit();
            impl->streamStorage.clear();
            impl->streamStorage.shrink_to_fit();
        }

        PDB pdb;
        pdb.impl_ = std::move(impl);
        return pdb;
    }

    Function PDB::ResolveFunction(std::string_view name) const
    {
        if (!impl_)
            throw Error("PDB: not loaded (Shutdown was called)");
        const uint32_t idx = impl_->FindFunction(name);
        if (idx == static_cast<uint32_t>(-1))
            throw Error("PDB: symbol not found: " + std::string(name));
        return impl_->BuildFunction(idx);
    }

    std::optional<Function> PDB::TryResolveFunction(std::string_view name) const
    {
        if (!impl_) return std::nullopt;
        const uint32_t idx = impl_->FindFunction(name);
        if (idx == static_cast<uint32_t>(-1))
            return std::nullopt;
        return impl_->BuildFunction(idx);
    }

    std::size_t PDB::FunctionCount() const noexcept
    {
        return impl_ ? impl_->functions.size() : 0;
    }

    Function PDB::FunctionAt(std::size_t index) const
    {
        if (!impl_)
            throw Error("PDB: not loaded (Shutdown was called)");
        if (index >= impl_->functions.size())
            throw Error("PDB: function index out of range");
        return impl_->BuildFunction(static_cast<uint32_t>(index));
    }

    Global PDB::ResolveGlobal(std::string_view name) const
    {
        if (!impl_)
            throw Error("PDB: not loaded (Shutdown was called)");
        const uint32_t idx = impl_->FindGlobal(name);
        if (idx == static_cast<uint32_t>(-1))
            throw Error("PDB: global not found: " + std::string(name));
        return impl_->BuildGlobal(idx);
    }

    std::optional<Global> PDB::TryResolveGlobal(std::string_view name) const
    {
        if (!impl_) return std::nullopt;
        const uint32_t idx = impl_->FindGlobal(name);
        if (idx == static_cast<uint32_t>(-1))
            return std::nullopt;
        return impl_->BuildGlobal(idx);
    }

    std::size_t PDB::GlobalCount() const noexcept
    {
        return impl_ ? impl_->globals.size() : 0;
    }

    Global PDB::GlobalAt(std::size_t index) const
    {
        if (!impl_)
            throw Error("PDB: not loaded (Shutdown was called)");
        if (index >= impl_->globals.size())
            throw Error("PDB: global index out of range");
        return impl_->BuildGlobal(static_cast<uint32_t>(index));
    }

    std::size_t PDB::GlobalCountOfKind(SymbolKind kind) const noexcept
    {
        if (!impl_) return 0;
        const auto i = static_cast<unsigned>(kind);
        return i < kSymbolKindCount ? impl_->globalKindCounts[i] : 0;
    }

    std::vector<Global> PDB::GlobalsOfKind(SymbolKind kind) const
    {
        std::vector<Global> out;
        if (!impl_) return out;
        out.reserve(GlobalCountOfKind(kind)); // exact size, single allocation
        for (const auto& rec : impl_->globals)
            if (rec.kind == kind)
                out.push_back(Global{ rec.name, rec.rva, rec.kind });
        return out; // already name-sorted: globals[] is, and we keep its order
    }

    Struct PDB::ResolveStruct(std::string_view name) const
    {
        if (!impl_)
            throw Error("PDB: not loaded (Shutdown was called)");
        const uint32_t idx = impl_->FindStruct(name);
        if (idx == static_cast<uint32_t>(-1))
            throw Error("PDB: struct not found: " + std::string(name));
        return impl_->BuildStruct(idx);
    }

    std::optional<Struct> PDB::TryResolveStruct(std::string_view name) const
    {
        if (!impl_) return std::nullopt;
        const uint32_t idx = impl_->FindStruct(name);
        if (idx == static_cast<uint32_t>(-1))
            return std::nullopt;
        return impl_->BuildStruct(idx);
    }

    std::size_t PDB::StructCount() const noexcept
    {
        return impl_ ? impl_->structs.size() : 0;
    }

    Struct PDB::StructAt(std::size_t index) const
    {
        if (!impl_)
            throw Error("PDB: not loaded (Shutdown was called)");
        if (index >= impl_->structs.size())
            throw Error("PDB: struct index out of range");
        return impl_->BuildStruct(static_cast<uint32_t>(index));
    }

    bool PDB::IsLoaded() const noexcept
    {
        return impl_ != nullptr;
    }

    ParseInfo PDB::GetParseInfo() const noexcept
    {
        if (!impl_) return {};
        return impl_->parseInfo;
    }

    MemStats PDB::MemoryUsage() const noexcept
    {
        if (!impl_) return {};
        MemStats s{};
        s.rawPDB        = impl_->data.capacity();
        s.functionCount = impl_->functions.size();
        s.argumentCount = impl_->arguments.size();
        s.globalCount   = impl_->globals.size();
        s.structCount   = impl_->structs.size();
        s.fieldCount    = impl_->fields.size();
        s.functionTable = impl_->functions.capacity() * sizeof(detail::FunctionRecord);
        s.argumentTable = impl_->arguments.capacity() * sizeof(Argument);
        s.globalTable   = impl_->globals.capacity() * sizeof(detail::GlobalRecord);
        s.structTable   = impl_->structs.capacity() * sizeof(detail::StructRecord);
        s.fieldTable    = impl_->fields.capacity() * sizeof(Field);
        s.stringArena   = impl_->stringArena.capacity();
        for (const auto& ss : impl_->streamStorage)
            s.streamStorage += ss.capacity();
        return s;
    }

    void PDB::Shutdown() noexcept
    {
        impl_.reset();
    }
}