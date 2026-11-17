# MemPDB - Benchmark Report

Complete performance, memory, and accuracy stats for the MemPDB parser,
produced by the bundled test/benchmark harness (`tests/test_main.cpp`).

All timings are measured with the CPU timestamp counter (`RDTSC`/`RDTSCP`,
`tests/Timer.hpp`), calibrated once against `QueryPerformanceCounter` and pinned
to one core during calibration. Each configuration is parsed **100 times**; the
tables report the **best** (lowest-noise) and **median** of those runs after a
5-iteration warm-up. The input buffer is copied fresh before every timed parse,
and the copy is *not* timed. Per-phase timings come from the best run.

To regenerate: build the `mempdb_tests` target and run it; it writes
`mempdb_report.txt` next to the executable. This document is a curated write-up
of that report, with the same row labels the harness prints.

---

## 1. Environment

| | |
|---|---|
| CPU | AMD Ryzen 7 7700X (8 cores / 16 threads, max 4.5 GHz) |
| TSC frequency (calibrated) | 4.500 GHz |
| `hardware_concurrency()` (as seen by process) | 16 |
| RAM | 31.1 GB |
| OS | Windows 10 Home, build 19045 |
| Compiler | MSVC (Visual Studio 2022, cl 19.4x), C++20 |
| Build flags | `/O2 /permissive-` + LTCG / whole-program optimization (IPO) |
| CRT | static (`/MT`) |

---

## 2. Inputs

Two PDBs are used so every code path is exercised:

| PDB | Kind | On-disk size | What it carries |
|---|---|---|---|
| `ntdll.pdb` | **Public / stripped** (Microsoft symbol server) | 1,625,088 B (1,587 KB) | public symbols (functions + data globals, with names + RVAs) and the full TPI type stream (structs), but **no** private per-function detail - no argument names, no proc sizes |
| `mempdb_fixture.pdb` | **Private / full debug** (`/Zi /Od /GR`) | ~6.0 MB | everything: proc records (sizes), argument lists (names + types), struct field layouts, and a polymorphic class so vtable/RTTI symbols are emitted |

Most tables below use **ntdll** (the realistic large input). Because a stripped
public PDB has **zero private argument info**, the argument table is always empty
there - so a separate **fixture matrix** (sec7) shows what arguments actually cost.

**Record counts (ntdll, `everything` tier):** 4,484 functions  /  0 arguments  / 
1,822 globals  /  597 structs  /  6,135 fields.

> Earlier reports counted 6,306 "functions" - that lumped data symbols in with
> code. Splitting on the `fFunction` flag gives the accurate 4,484 functions +
> 1,822 globals.

---

## 3. Headline numbers (ntdll.pdb, 1.55 MB)

| Metric | Value |
|---|---|
| Parse - function names + RVAs only | **0.85 ms**, 239.7 KB |
| Parse - globals only | **0.35 ms**, 124.6 KB |
| Parse - everything | **1.91 ms**, 794.1 KB |
| Parse - everything, interning off | 1.74 ms, 3,116.6 KB |
| Symbol lookup (binary search over 4,484 fns) | **~40-49 ns / call** |
| Tiering speedup (everything -> fn-names) | **2.2x** |
| Interning memory saving | **3.9x** (3,116.6 -> 794.1 KB) |
| Accuracy | **71 / 71 checks pass** |

---

## 4. Configuration matrix - what each combination costs (ntdll)

The library lets you turn each body of work on independently. This is the full
cross-product of the useful combinations, **speed and memory from the same
runs**, so the two line up row-for-row.

The flag columns: **Fn** = function table, **Sz** = function sizes, **Ar** =
argument names/types, **Gl** = global (data-symbol) table, **St** = struct
layouts.

| config | Fn | Sz | Ar | Gl | St | best ms | median ms | total KB |
|---|:--:|:--:|:--:|:--:|:--:|---:|---:|---:|
| fn-names      | * | | | | | 0.85 | 1.03 | 239.7 |
| fn+sizes      | * | * | | | | 1.18 | 1.38 | 239.7 |
| fn+args       | * | | * | | | 1.29 | 1.57 | 239.7 |
| fn+sizes+args | * | * | * | | | 0.93 | 1.42 | 239.7 |
| globals       | | | | * | | **0.35** | 0.46 | 124.6 |
| structs       | | | | | * | 0.96 | 1.47 | 429.8 |
| fn+globals    | * | | | * | | 0.65 | 0.75 | 364.2 |
| everything    | * | * | * | * | * | 1.91 | 2.34 | 794.1 |

*(memory totals are identical for the four `fn*` rows because ntdll has no
private argument data - the arg table is empty here; see the fixture matrix in
sec7 for the real argument cost.)*

**What the numbers say:**

- **The cheapest useful result is `globals` (0.35 ms / 124.6 KB)** - it scans the
  public symbol stream, keeps only data symbols, and skips the function sort
  entirely.
- **`fn-names` (0.85 ms)** is the cheapest function result; adding the global
  table on top (`fn+globals`, 0.65 ms) reuses the same symbol scan, so it is no
  more expensive than functions alone.
- **Anything that needs module streams costs ~0.9-1.3 ms regardless of which
  detail you pull.** `fn+sizes`, `fn+args`, and `fn+sizes+args` all land in the
  same band because the dominant cost is *walking* the per-module symbol streams,
  not the specific field you fill from them. If you need either sizes or args,
  you may as well take both. (The exact ordering inside this band is within the
  measurement noise - see the note below.)
- **`structs` (0.96 ms)** is independent of the symbol path - it reads only the
  TPI type stream.
- **`everything` (1.91 ms)** ~ the symbol path + module walk + struct decode,
  i.e. the sum of the parts.

> Measurement noise: the module-stream phase is page-fault / scheduler sensitive,
> so absolute milliseconds wobble run to run and the within-band ordering of the
> `fn+*` rows is not stable (one run even put `fn+sizes+args` *below* `fn+args`).
> Best-of-100 is the headline figure; medians run noticeably higher
> (`everything` median 2.34 ms vs best 1.91). See sec10 for run-to-run spread.

---

## 5. Speed - per-phase breakdown (ntdll)

Time in **milliseconds**, from the best run of each config. Columns are the parse
phases in execution order.

| config | best | MSF | TPI | PubSym | Sort | Mods | Struct | Intern |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fn-names      | 0.853 | 0.133 | 0.000 | 0.271 | 0.352 | 0.000 | 0.000 | 0.069 |
| fn+sizes      | 1.179 | 0.137 | 0.000 | 0.274 | 0.340 | 0.307 | 0.000 | 0.085 |
| fn+args       | 1.289 | 0.122 | 0.155 | 0.240 | 0.339 | 0.325 | 0.000 | 0.070 |
| fn+sizes+args | 0.926 | 0.032 | 0.050 | 0.140 | 0.288 | 0.324 | 0.000 | 0.027 |
| globals       | 0.350 | 0.028 | 0.000 | 0.136 | 0.143 | 0.000 | 0.000 | 0.015 |
| structs       | 0.957 | 0.031 | 0.038 | 0.000 | 0.000 | 0.000 | 0.611 | 0.217 |
| fn+globals    | 0.652 | 0.027 | 0.000 | 0.137 | 0.423 | 0.000 | 0.000 | 0.038 |
| everything    | 1.909 | 0.043 | 0.055 | 0.156 | 0.426 | 0.305 | 0.580 | 0.255 |
| full-serial   | 1.941 | 0.032 | 0.047 | 0.142 | 0.427 | 0.326 | 0.579 | 0.268 |
| full-noIntern | 1.737 | 0.034 | 0.047 | 0.146 | 0.432 | 0.311 | 0.576 | 0.166 |

| Phase | Work | Gated by |
|---|---|---|
| **MSF** | Parse MSF container, stream directory, DBI/section headers. | always |
| **TPI** | Load type-info (+ IPI) stream. | args or structs |
| **PubSym** | Scan public symbols -> split functions/globals on `fFunction`, classify each global by SymbolKind, resolve RVAs. | functions or globals |
| **Sort** | Name-sort function + global tables (8-byte prefix-key sort). | functions or globals |
| **Mods** | Walk per-module symbol streams for proc sizes + argument names. | sizes or args |
| **Struct** | Decode `LF_STRUCTURE/CLASS/UNION` -> field-list member layouts. | structs |
| **Intern** | Copy referenced strings into one arena; free raw buffers. | always (cheap) |

Observations:
- **Skipped phases are exactly 0.000 ms** - disabling a tier removes its work,
  visible in every row above (e.g. `globals` has TPI/Mods/Struct at 0).
- **Struct decode (~0.58-0.61 ms)** is the single largest phase in a full parse -
  597 field lists, 6,135 members.
- **Sort (~0.34-0.46 ms)** orders the function + global tables; the prefix-key
  makes most comparisons a single integer compare.
- **Symbol categorization is free** - it rides inside the existing PubSym scan as
  a few byte-compares on the name already in hand.
- **MSF varies (0.027-0.150 ms)** from first-touch page faults; negligible in
  absolute terms.

---

## 6. Memory - full per-table breakdown (ntdll)

Live heap owned by the `PDB` instance, in **KB**, from `PDB::MemoryUsage()`.
`rawPDB`/`streams` are the source buffers (freed by interning); the rest are
output tables; `arena` is interned strings.

| config | rawPDB | streams | fnTable | argTable | glbTable | structTbl | fieldTbl | arena | **TOTAL** |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fn-names      | 0.0 | 0.0 | 140.1 | 0.0 | 0.0 | 0.0 | 0.0 | 99.5 | **239.7** |
| fn+sizes      | 0.0 | 0.0 | 140.1 | 0.0 | 0.0 | 0.0 | 0.0 | 99.5 | **239.7** |
| fn+args       | 0.0 | 0.0 | 140.1 | 0.0 | 0.0 | 0.0 | 0.0 | 99.5 | **239.7** |
| fn+sizes+args | 0.0 | 0.0 | 140.1 | 0.0 | 0.0 | 0.0 | 0.0 | 99.5 | **239.7** |
| globals       | 0.0 | 0.0 | 0.0 | 0.0 | 42.7 | 0.0 | 0.0 | 81.9 | **124.6** |
| structs       | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 18.7 | 239.6 | 171.5 | **429.8** |
| fn+globals    | 0.0 | 0.0 | 140.1 | 0.0 | 42.7 | 0.0 | 0.0 | 181.4 | **364.2** |
| everything    | 0.0 | 0.0 | 140.1 | 0.0 | 42.7 | 18.7 | 239.6 | 352.9 | **794.1** |
| everything, no interning | 1587.0 | 1007.2 | 140.1 | 0.0 | 42.7 | 18.7 | 239.6 | 81.3 | **3116.6** |

Key facts:
- **Interning frees the raw input.** With interning on, `rawPDB` + `streams` ->
  0; the parser keeps only compact tables + the name arena. That is the 3.9x
  reduction (3,116.6 -> 794.1 KB, saving 2,322.6 KB).
- **You pay only for what you ask for.** `fn-names` has 0 bytes of
  global/struct/field memory; `globals` has 0 bytes of function table; `structs`
  has 0 bytes of function/global table.
- **`sizes` and `args` add no table on ntdll** - sizes fill existing function
  records in place, and the public PDB has no arg data (see sec7 for the cost when
  args *are* present).
- **Smallest table preset is `globals` at 124.6 KB; `fn-names` is 239.7 KB.**

**Per-record footprint** - internal stored-record size (what each table row
costs in the heap), derived from the table above:

| Table | Bytes / record |
|---|---|
| Function record | 32 B (140.1 KB / 4,484) |
| Global record | 24 B (42.7 KB / 1,822) - SymbolKind tag packed into existing padding |
| Struct record | 32 B (18.7 KB / 597) |
| Field record | 40 B (239.6 KB / 6,135) |

*(The public API value types are slightly larger - `sizeof` reports
Function=48, Argument=40, Global=24, Struct=40, Field=40 bytes - because they
carry convenience members like the `std::span` of arguments/fields. They are
returned by value on lookup and are not what the resident tables store.)*

**Records produced per config** (proof that disabling a tier produces zero of
its records, on ntdll):

| config | functions | args | globals | structs | fields |
|---|---:|---:|---:|---:|---:|
| fn-names      | 4,484 | 0 | 0 | 0 | 0 |
| fn+sizes      | 4,484 | 0 | 0 | 0 | 0 |
| fn+args       | 4,484 | 0 | 0 | 0 | 0 |
| fn+sizes+args | 4,484 | 0 | 0 | 0 | 0 |
| globals       | 0 | 0 | 1,822 | 0 | 0 |
| structs       | 0 | 0 | 0 | 597 | 6,135 |
| fn+globals    | 4,484 | 0 | 1,822 | 0 | 0 |
| everything    | 4,484 | 0 | 1,822 | 597 | 6,135 |

*(args = 0 everywhere because ntdll is stripped; the fixture in sec7 produces real
argument records.)*

---

## 7. Fixture matrix - the real cost of arguments (private PDB)

ntdll has no private argument info, so its argument table is always empty. The
fixture is built with full debug info, so it exercises the argument path. Same
config rows, measured on the fixture:

| config | best ms | fnTbl KB | argTbl KB | structTbl KB | fieldTbl KB | total KB |
|---|---:|---:|---:|---:|---:|---:|
| fn-names      | 0.562 | 81.8 | 0.0 | 0.0 | 0.0 | 317.6 |
| fn+sizes      | 1.639 | 81.8 | 0.0 | 0.0 | 0.0 | 317.6 |
| fn+args       | 2.228 | 81.8 | 48.9 | 0.0 | 0.0 | 388.8 |
| fn+sizes+args | 2.224 | 81.8 | 48.9 | 0.0 | 0.0 | 388.8 |
| structs       | 0.747 | 0.0 | 0.0 | 18.8 | 87.5 | 182.4 |
| everything    | 3.026 | 81.8 | 48.9 | 18.8 | 87.5 | 645.6 |

What this shows that ntdll can't:
- **Arguments cost 48.9 KB of arg table** here (`fn+args` - `fn-names` = +71.2 KB
  total: 48.9 KB is the arg-record table, the rest is the extra interned
  argument-name and type-name strings).
- **`fn+sizes` adds zero memory** - sizes are written into the existing function
  records, never a new table (317.6 KB, identical to `fn-names`). The only cost
  of sizes is the module-stream walk, which shows up as the jump from 0.56 ms to
  1.64 ms.
- **`structs` here has fnTbl=0** (function table correctly skipped), with the
  struct/field tables populated - confirming per-tier isolation on a PDB that has
  full type info.
- **The fixture's module streams are larger and denser than ntdll's**, so its
  module-walk-dependent configs (`fn+sizes`, `fn+args`, `everything`) cost more
  in absolute terms than the same configs on the bigger-but-stripped ntdll -
  detail volume, not file size, drives parse time.

---

## 8. Speed - derived metrics (ntdll)

| Metric | Value |
|---|---|
| Tiering speedup (everything -> fn-names) | **2.24x** |
| Parallel module parse (this input) | 1.02x - *intentionally serial here* |
| Module-stream volume (ntdll) | 0.1 MB |
| Lookup latency | ~40-49 ns / call (500,000 calls over 4,484 fns) |
| Per-function parse cost - fn-names | ~ 0.19 us / function |
| Per-symbol parse cost - everything | ~ 0.30 us / (function + global) |

**On parallelism:** module-stream parsing is multi-threaded, but thread spawn
(~0.5 ms on Windows) only pays off above ~6 MB of symbol data. ntdll's module
streams are 0.1 MB, so the library runs them serially - the measurement confirms
threading would be break-even here (serial 1.94 ms vs parallel-eligible 1.91 ms,
1.02x). The threshold makes the parallel path a win only on large private PDBs.

**On lookup:** ~40-49 ns/call over a 4,484-entry table is ~ log2(4484) ~ 12.1
comparisons (a few ns each) - the prefix-key layout keeps most comparisons to a
single integer compare. (The figure jitters run-to-run; three measured runs gave
39.6 / 41.4 / 49.1 ns.)

---

## 9. Symbols & categories

Every non-function public symbol is classified during the single symbol scan
into a `SymbolKind`, from its MSVC name decoration:

| Kind | Decoration | Meaning |
|---|---|---|
| `Variable` | `?name@@3..A` | ordinary global/static data |
| `Constant` | `?name@@3..B` / `..D` | const-qualified global data |
| `Vtable` | `??_7..@@6B@` | virtual function table |
| `VbTable` | `??_8..` | virtual base table |
| `Rtti` | `??_R0..??_R4` | RTTI descriptor / object locator |
| `StringLiteral` | `??_C..` | compiler-emitted string literal |
| `Unknown` | - | data symbol matching none of the above |

**Category breakdown - ntdll.pdb (1,822 globals):** 751 Variable  /  1,071
StringLiteral  /  0 of each other kind. (A stripped public PDB exposes no
vtables/RTTI/consts as distinct publics - the fixture, built with `/GR`, does;
see sec11.)

**Per-category access (no re-scan):**
- `GlobalCountOfKind(kind)` - O(1), counts tallied during the scan.
- `GlobalsOfKind(kind)` - name-sorted `vector<Global>`, allocates exactly
  `count` entries on demand; categories you never ask for cost nothing.
- `GlobalKindMask` on `ParseOptions` filters *before* storing, so e.g.
  vtables-only never allocates for the categories you skip.

---

## 10. Run-to-run stability

Three consecutive full suite runs (best-time, ms):

| config | run 1 | run 2 | run 3 |
|---|---:|---:|---:|
| fn-names   | 0.841 | 0.853 | 0.839 |
| globals    | 0.336 | 0.350 | 0.330 |
| everything | 1.946 | 1.909 | 1.933 |
| lookup (ns)| 39.6 | 49.1 | 41.4 |

Best-times are stable to within ~2%; medians are noisier (the `everything`
median ranged 2.27-3.04 ms, dominated by the module-stream phase), which is why
best-of-100 is the headline figure.

---

## 11. Accuracy - 71 / 71 checks pass

**Differential consistency (public PDB)** - names-only, sizes, no-intern, and
serial all produce identical name->RVA mappings; sizes match across tiers
(thread-safe); names-only yields zero sizes/args (work was actually skipped).

**Globals** - non-empty, all RVAs non-zero, name-sorted; function and global
tables never overlap (the `fFunction` split is correct); per-category counts sum
to the total; `GlobalCountOfKind` matches a manual tally; `GlobalsOfKind`
returns exactly the matching tagged entries.

**Preset selectivity** - presets allocate exactly what they claim
(`FunctionsOnly` no global/struct/field memory; `GlobalsOnly` no function/struct
memory; `StructsOnly` no function/global memory; `GlobalKindMask` stores only
the requested kind).

**Table invariants** - function and struct tables name-sorted, no RVA is 0,
every `Arguments`/`Fields` span length equals its declared count.

**Ground truth (private fixture):**

| Function | Size | Args (decoded types) |
|---|---:|---|
| `fixture_add` | 21 | `int a, int b` |
| `fixture_five` | 43 | `int a, int b, int c, int d, int e` |
| `fixture_mixed` | 41 | `int a, double b, char c` |
| `fixture_ptr` | 25 | `const char* s, unsigned int n` |

| Struct | Size | Fields (offset / type / name) |
|---|---:|---|
| `FixturePoint` | 16 | `+0 int x`, `+4 int y`, `+8 double z` |
| `FixtureMix` | 40 | `+0 char tag`, `+8 void* ptr`, `+16 unsigned __int64 id`, `+24 int[] arr` |

| SymbolKind | Example classified from the fixture |
|---|---|
| Vtable | `??_7DNameNode@@6B@` |
| Rtti | `??_R0?AUFixtureShape@@@8` |
| StringLiteral | `??_C@_00CNPNBAHC@@` |
| Constant | a `?...@@3?B`-decorated const global |

These confirm correct pointer rendering (`void*`, `const char*`), 64-bit
primitives (`unsigned __int64`), arrays (`int[]`), exact x64 offsets/sizes, and
that no `??_`-prefixed special symbol is ever misfiled as Variable/Constant.

---

## 12. How to reproduce

```sh
# Configure + build (from the repo root)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Run the suite (writes mempdb_report.txt next to the exe)
build/Release/mempdb_tests.exe [public.pdb] [fixture.pdb]
```

With no arguments it auto-locates `ntdll.pdb` and `mempdb_fixture.pdb` in the
build output directory. Exit code is 0 iff all accuracy checks pass.

*Numbers in this document were measured on the environment in sec1. Absolute
timings vary with hardware; the relative results (tiering speedup, interning
savings, per-phase distribution, per-config memory) are the portable takeaways.*
