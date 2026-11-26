# Startup location and thread state API

This document describes the read-only startup/thread-state support added to
lib-macho. Everything operates on already parsed image bytes: the library does
not load, map, execute or allocate an address space for the image, and it never
follows dependent libraries.

The regression contract lives in `task_support/thread-layouts.md`; the
executable coverage is `tests/startup_regression.cpp`.

## Thread records

`MachO::LoadCommands::Thread` represents both `LC_THREAD` (`4`) and
`LC_UNIXTHREAD` (`5`).

```cpp
std::vector<MachO::ThreadRecord> Thread::records() const;
```

Records are returned in on-disk order, **by value**. Every record owns its
bytes, so copying the `Thread`/`File` and destroying the originating input
stream keeps all of this readable.

`MachO::ThreadRecord` (header `MachO/ThreadState.hpp`) offers:

| Member | Meaning |
| --- | --- |
| `uint32_t flavor()` | Raw thread flavor. |
| `uint32_t count()` | Count in **4-byte units**, exactly as on disk. |
| `uint32_t offset()` / `offsetInCommand()` | Offset of the record's `flavor` field from the first byte of the load command. The first record is at `8` (after `cmd`/`cmdsize`). |
| `const std::vector<uint8_t>& bytes()` | Whole record bytes: `flavor`, `count` and payload, in image byte order. |
| `const std::vector<uint8_t>& payload()` | Exactly `count * 4` payload bytes. |
| `const std::optional<ThreadContext>& context()` | Decoded state, when known. `state()` is an alias. |
| `bool decoded()` | Whether `context()` is engaged. |

### Decoded context

`MachO::ThreadContext` / `MachO::ThreadState` exposes:

| Member | Meaning |
| --- | --- |
| `ThreadArchitecture architecture()` | `Unknown`, `I386`, `X86_64`, `ARM`, `ARM64`. |
| `std::string architectureName()` | `"i386"`, `"x86_64"`, `"arm"`, `"arm64"` or `"unknown"`. |
| `uint32_t flavor()` | **Outer** record flavor identity (always `7` for unified records). |
| `std::optional<uint32_t> innerFlavor()` | Inner flavor for x86 unified records (`1` or `4`). |
| `const std::vector<ThreadRegister>& registers()` | Registers in original ABI order and original names. `ThreadRegister` is `{ std::string name; uint64_t value; }`. 32-bit values are zero-extended, never sign-extended. |
| `std::optional<uint64_t> registerValue(name)` | Lookup by original register name. |
| `uint64_t pc()` / `uint64_t sp()` | Program counter and stack pointer. |

Supported CPU/flavor combinations (the CPU is the slice/image `cputype`, not
just `MachO32`/`MachO64`):

| CPU | flavor | count | Layout / registers |
| --- | --- | --- | --- |
| `7` (`CPU_TYPE_X86`) | `1` | `16` | i386: `eax ebx ecx edx edi esi ebp esp ss eflags eip cs ds es fs gs`; PC=`eip`, SP=`esp`. |
| `0x01000007` (`CPU_TYPE_X86_64`) | `4` | `42` | x86_64: 21 u64 `rax … r15 rip rflags cs fs gs`; PC=`rip`, SP=`rsp`. |
| `7` / `0x01000007` | `7` | `44` | x86 unified: u32 inner flavor, u32 inner count, 168-byte union. Inner `1/16` or `4/42` decodes as above; remaining union bytes stay in the outer payload. Unknown inner flavor leaves the **whole outer record** undecoded. |
| `12` (`CPU_TYPE_ARM`) | `1` | `17` | `r0…r12 sp lr pc cpsr`; PC=`pc`, SP=`sp`. |
| `0x0100000C` (`CPU_TYPE_ARM64`) | `6` | `68` | `x0…x28 fp lr sp pc` (33 u64), then u32 `cpsr`, then 4 padding bytes. Padding is raw payload, not a register. |

The same flavor number (notably flavor `1`) means different states on x86 and
ARM; decoding is selected from the current image CPU. Known flavors from
another CPU family and genuinely unknown flavors keep their raw bytes with an
empty context. The legacy constructor

```cpp
Thread(uint32_t command, uint32_t size, File::Kind kind, XS::IO::BinaryStream& stream);
```

is preserved and, having no CPU, only preserves records (no contexts).
`MachO::File` uses the CPU-aware overload and passes the image `cputype`.

Parsing is strict: incomplete record headers/payloads, a record spanning past
`cmdsize`, overflowing spans, and a recognized state whose `count` does not
match its exact layout all throw a descriptive `std::runtime_error`. The
command must be consumed exactly. `Thread::getInfo()` includes every record
and, when decoded, every named register plus PC/SP.

## Entry points

```cpp
std::vector<MachO::EntryPointSource> MachO::File::entryPoints() const;
std::vector<MachO::EntryPointSource> MachO::File::entryPointSources() const; // alias
```

Sources are returned in load-command order. `LC_MAIN` yields one source;
`LC_UNIXTHREAD` (`5`) yields one source per **decoded** record (using its
PC); `LC_THREAD` (`4`) is display-only and yields nothing.

`MachO::EntryPointSource` (header `MachO/EntryPoints.hpp`):

| Field | Meaning |
| --- | --- |
| `size_t commandIndex` | Zero-based ordinal of the load command (`ncmd` order). |
| `std::optional<size_t> recordIndex` | Zero-based `Thread::records()` index for `LC_UNIXTHREAD` sources. |
| `EntryPointKind kind` | `Main` (`LC_MAIN`) or `UnixThread` (`LC_UNIXTHREAD`); enum aliases `LC_MAIN` / `LC_UNIXTHREAD`. |
| `uint32_t command` | Raw load command id. |
| `uint64_t coordinate` | Raw coordinate: `LC_MAIN` `entryoff` (a **file** offset) or the decoded thread PC (a **virtual** address). |
| `std::vector<EntryPointCandidate> candidates` | **Every** matching segment, in load-command order. Empty when no segment matches; the source and its coordinate are still preserved. |

`MachO::EntryPointCandidate` (alias `EntryPoint`) fields:

| Field | Meaning |
| --- | --- |
| `size_t segmentIndex` | Command ordinal of the `LC_SEGMENT`/`LC_SEGMENT_64`. |
| `std::string segmentName` | Segment name. |
| `uint64_t vmAddress`, `vmSize` | Segment `vmaddr`/`vmsize`. |
| `uint64_t fileOffset`, `fileSize` | Segment `fileoff`/`filesize`. |
| `uint64_t address` | Resolved entry virtual address for this mapping. |
| `std::optional<uint64_t> mappedFileOffset` | Resolved file coordinate; empty for a PC in the zero-filled tail. |

### Coordinate rules (all intervals half-open)

* **LC_MAIN (file coordinate).** A segment matches when
  `fileOffset <= coordinate < fileOffset + fileSize` and the displacement
  `coordinate - fileOffset` is also `< vmSize`. Then
  `address = vmAddress + displacement` (checked arithmetic) and
  `mappedFileOffset = coordinate`.
* **LC_UNIXTHREAD (virtual coordinate).** A segment matches when
  `vmAddress <= PC < vmAddress + vmSize`. Then `address = PC` and
  `displacement = PC - vmAddress`. When `displacement < fileSize`,
  `mappedFileOffset = fileOffset + displacement`; otherwise the PC is in the
  zero-filled (`vmsize > filesize`) tail and `mappedFileOffset` is empty while
  the VA candidate remains.
* Overlapping segments and several segments at the same address all produce
  candidates and are preserved. Sources from different commands/records at the
  same address are all preserved. Segment commands may appear after the entry
  command; results are identical because mapping is computed after parsing.
* Overflowing segment spans or coordinate arithmetic throw a descriptive
  `std::runtime_error`.

## Fat files

Each `MachO::FatFile` slice is parsed from its own byte range as an
independent `MachO::File`, so it uses only its own CPU and its own segment
table. All file/fat-slice offsets described here are relative to that Mach-O
image, not to the outer fat file. `FatFile::architectures()` returns owned
`{FatArch, File}` pairs.

## Example

`examples/startup_dump.cpp` is a C++17 read-only dumper:

```cpp
MachO::File file(stream);

for (auto cmd : file.loadCommands<MachO::LoadCommands::Thread>())
    for (auto rec : cmd.records())
        if (rec.context())
            std::cout << rec.context()->architectureName()
                      << " pc=" << rec.context()->pc()
                      << " sp=" << rec.context()->sp() << '\n';

for (auto e : file.entryPoints())
    for (auto c : e.candidates)
        std::cout << e.commandIndex << ' ' << c.address << '\n';
```
