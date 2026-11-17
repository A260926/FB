# MemPDB

Fast in memory [Microsoft PDB](https://llvm.org/docs/PDB/) parser for **C++20**.

Load a PDB from a buffer (or pull it from a symbol server), choose exactly which
tables to build, then look up functions, globals, and structs by name, without
DIA, without attaching to a process, and without paying for data you do not use.

```cpp
auto pdb = MemPDB::PDB::ParseFromMemory(
    buf, MemPDB::ParseOptions::FunctionsOnly());

if (auto fn = pdb.TryResolveFunction("NtCreateFile"))
    std::cout << (*fn).Name << " @ RVA 0x" << std::hex << (*fn).RVA << "\n";
```

## Why MemPDB

<table>
<tr><td><b>Tiered parsing</b></td><td>Ask for functions only, globals only, structs only, or everything</td></tr>
<tr><td><b>In memory</b></td><td><code>std::vector&lt;std::byte&gt;</code> in, indexes out; no filesystem required after load</td></tr>
<tr><td><b>Symbol server</b></td><td>Optional download from <code>msdl.microsoft.com</code> (or your own server)</td></tr>
<tr><td><b>Small API</b></td><td>One public header: <a href="include/MemPDB/MemPDB.hpp"><code>include/MemPDB/MemPDB.hpp</code></a></td></tr>
<tr><td><b>Tools</b></td><td><code>pdb2json</code> dumps functions / structs / globals to JSON</td></tr>
</table>

## Benchmarks

`ntdll.pdb`: public / stripped Microsoft symbols, **1.55 MB**, about 4.5k functions.

Measured with the bundled harness (`mempdb_tests`), best of 100 RDTSC runs after warmup.

<table>
<tr><th>Config</th><th>Windows (MSVC)</th><th>Linux (WSL / GCC)</th><th>Resident</th></tr>
<tr><td>Functions + RVAs only</td><td><b>0.85 ms</b></td><td><b>0.25 ms</b></td><td>~240 KB</td></tr>
<tr><td>Globals only</td><td>0.35 ms</td><td>0.09 ms</td><td>~125 KB</td></tr>
<tr><td>Structs only</td><td>0.96 ms</td><td>0.60 ms</td><td>~430 KB</td></tr>
<tr><td>Everything</td><td><b>1.91 ms</b></td><td><b>1.20 ms</b></td><td>~794 KB</td></tr>
<tr><td>Everything, no interning</td><td>1.74 ms</td><td>1.12 ms</td><td>~3.1 MB</td></tr>
<tr><td>Name lookup</td><td>~40 to 50 ns</td><td>~39 ns</td><td></td></tr>
</table>

* Asking for **function names only** is about 2 to 5x faster than parsing everything
* **String interning** cuts steady state memory about **3.9x** (3.1 MB to 794 KB)
* Accuracy suite: **71/71** checks on Windows (with fixture PDB); **39/39** public PDB checks on Linux

Full matrix and methodology: [`benchmark.md`](benchmark.md).

> Hardware: AMD Ryzen 7 7700X. Windows = MSVC Release + LTCG. Linux = WSL2 / GCC 15 with O2.

## Optimize: parse only what you need

Default options resolve **everything**. That is the wrong default if you only need RVAs.

<table>
<tr><th>You want...</th><th>Use</th><th>Cost driver</th></tr>
<tr><td>Function name to RVA</td><td><code>ParseOptions::FunctionsOnly()</code></td><td>Public symbol stream</td></tr>
<tr><td>Functions + sizes</td><td><code>FunctionsWithSizes()</code></td><td>+ module proc records</td></tr>
<tr><td>Functions + data symbols</td><td><code>NamesAndRVAs()</code></td><td>One public symbol scan</td></tr>
<tr><td>Globals only</td><td><code>GlobalsOnly()</code></td><td>Often the cheapest path</td></tr>
<tr><td>Vtables / RTTI / strings only</td><td><code>GlobalsOfKind(KindBit(...))</code></td><td>Filtered during scan</td></tr>
<tr><td>Struct / class layouts</td><td><code>StructsOnly()</code></td><td>TPI stream</td></tr>
<tr><td>Full private detail</td><td><code>Everything()</code></td><td>Modules + TPI + all tables</td></tr>
</table>

```cpp
// Cheap: names + RVAs only
auto pdb = MemPDB::PDB::ParseFromMemory(
    std::move(buf), MemPDB::ParseOptions::FunctionsOnly());

// Only vtables; nothing else is stored
auto vt = MemPDB::PDB::ParseFromMemory(
    std::move(buf),
    MemPDB::ParseOptions::GlobalsOfKind(
        MemPDB::KindBit(MemPDB::SymbolKind::Vtable)));

// Custom mix
MemPDB::ParseOptions o;
o.ResolveFunctions = true;
o.ResolveStructs   = true;
o.ResolveSizes = o.ResolveArguments = o.ResolveGlobals = false;
o.InternStrings = true;   // keep this on unless you need the raw PDB bytes
o.Parallel      = true;   // module walk across threads
auto pdb2 = MemPDB::PDB::ParseFromMemory(std::move(buf), o);
```

**Practical tips**

1. Need RVAs only: `FunctionsOnly` / `NamesAndRVAs` / `GlobalsOnly`.
2. Need sizes *or* args: you already pay for the module stream walk; take both.
3. Keep `InternStrings = true` for production (big memory win after parse).
4. Public Microsoft PDBs have **no** private argument names or proc sizes. Those fields stay empty unless you parse a private (`/Zi`) PDB.

## Examples

### Parse a file

```cpp
#include <MemPDB/MemPDB.hpp>
#include <fstream>
#include <vector>

std::vector<std::byte> ReadFile(const char* path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    std::vector<std::byte> buf(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(buf.size()));
    return buf;
}

int main()
{
    auto pdb = MemPDB::PDB::ParseFromMemory(
        ReadFile("ntdll.pdb"), MemPDB::ParseOptions::FunctionsOnly());

    auto fn = pdb.ResolveFunction("NtCreateFile");
    // fn.Name, fn.RVA, fn.Size, fn.Arguments ...
}
```

### Download from a symbol server

```cpp
MemPDB::Config cfg;
cfg.SymbolServer = "https://msdl.microsoft.com/download/symbols";
cfg.Options      = MemPDB::ParseOptions::FunctionsOnly();

auto pdb = MemPDB::PDB::Parse(
    "ntdll.pdb",
    "180BF1B90AA75697D0EFEA5E5630AC7E1", // GUID+Age
    cfg);
```

Windows uses WinHTTP; Linux uses libcurl.

### Enumerate / classify globals

```cpp
auto pdb = MemPDB::PDB::ParseFromMemory(buf, MemPDB::ParseOptions::GlobalsOnly());

for (std::size_t i = 0; i < pdb.GlobalCount(); ++i)
{
    auto g = pdb.GlobalAt(i);
    // g.Kind == SymbolKind::Vtable, Rtti, StringLiteral, ...
}

auto vtables = pdb.GlobalsOfKind(MemPDB::SymbolKind::Vtable);
```

### `pdb2json`

```bash
pdb2json ntdll.pdb
# writes ntdll_dump/functions.json, structures.json, globals_*.json, ...
```

More demos: [`example/main.cpp`](example/main.cpp).

## Build

MemPDB is meant to work **out of the box** with a normal CMake build on:

* **Windows:** MSVC, Clang/LLVM (`clang++` or the clang CL driver), and MinGW
* **Linux:** GCC or Clang/LLVM

Same project, same CMakeLists; no special forks or defines. Parsing a local PDB
needs no network. Symbol server download uses WinHTTP on Windows and **libcurl**
on Linux (install the curl dev package once, then CMake finds it).

### Windows (MSVC)

Open `CMakeLists.txt` in Visual Studio 2022 (x64) and build the Release config,
or configure/build with CMake using the "Visual Studio 17 2022" generator and
x64 architecture (standard CMake CLI options).

### Windows (Clang / MinGW) and Linux (GCC / Clang)

1. Install CMake, a C++20 compiler (`g++` or `clang++`), and your distro's
   **libcurl development** package (on Debian/Ubuntu this is the OpenSSL flavor
   of the libcurl dev package).
2. Create a `build` folder, configure with `CMAKE_BUILD_TYPE=Release`
   (point CMake at the source tree), then build.
3. Optional: set `CMAKE_CXX_COMPILER` to `clang++` or `g++`.

Same flow on Windows with Clang or MinGW: configure with CMake, then build.

<table>
<tr><th>Target</th><th>What you get</th></tr>
<tr><td><code>MemPDB</code></td><td>Static library</td></tr>
<tr><td><code>example</code></td><td>Usage demos</td></tr>
<tr><td><code>pdb2json</code></td><td>PDB to JSON exporter</td></tr>
<tr><td><code>mempdb_tests</code></td><td>Accuracy + speed harness</td></tr>
</table>

```bash
# tests (writes mempdb_report.txt)
./mempdb_tests path/to/ntdll.pdb
```

## Platform support

<table>
<tr><th>Platform</th><th>Compilers</th><th>Status</th></tr>
<tr><td>Windows</td><td>MSVC, Clang/LLVM (<code>clang++</code> or clang CL driver), MinGW</td><td>Supported (out of the box)</td></tr>
<tr><td>Linux</td><td>GCC, Clang/LLVM</td><td>Supported (out of the box; needs libcurl dev package)</td></tr>
<tr><td>macOS</td><td></td><td>Planned</td></tr>
</table>

**Notes**

* Local parse/lookup works the same on every supported toolchain.
* Linux symbol downloads need `libcurl` at build and runtime; Windows needs nothing extra beyond the SDK (WinHTTP).
* Private symbol fixture tests expect an MSVC built fixture PDB; public PDBs (e.g. `ntdll.pdb`) exercise the full Linux/Clang path without that.

## Roadmap

* [x] Windows: MSVC, Clang, MinGW
* [x] Linux: GCC, Clang + libcurl downloads
* [ ] macOS: Apple/LLVM Clang, downloader, test harness
* [ ] CI matrix / release packages
* [ ] Richer symbol cache / multi server helpers

## License

[MIT](LICENSE). PDB/CodeView layouts were cross checked against LLVM DebugInfo
headers (Apache 2.0 with LLVM exception); see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
MemPDB does not link against LLVM.
