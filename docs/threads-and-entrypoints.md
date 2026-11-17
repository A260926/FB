# Thread records, decoded contexts, and startup entry points

This document describes the read-only API added on top of the existing
`MachO::File`, `MachO::FatFile`, and `MachO::LoadCommands::Thread` parsing
chain. Nothing here loads, maps, relocates, or executes an image.

All ranges below are half-open (`[begin, end)`).

## Thread records

`MachO::LoadCommands::Thread` parses `LC_THREAD` (`0x04`) and
`LC_UNIXTHREAD` (`0x05`) payloads as a sequence of records:

```
u32 flavor
u32 count          // in four-byte units
u8  payload[count * 4]
```

`Thread::records()` returns the records in original on-disk order. Each
`Thread::Record` owns a copy of its payload, so the record remains readable
after the originating stream is destroyed and after the owning `File` is
copied.

| `Thread::Record` field | Meaning |
| --- | --- |
| `offset` | Byte offset from the start of the load command. It includes the 8-byte command header, so the first record normally has offset 8. |
| `flavor` | Raw flavor value. |
| `count` | Raw count value, in four-byte units. |
| `payload` | The record's own `count * 4` bytes. |
| `state` | Optional decoded context (see below). |

The parser consumes the command exactly. A trailing partial header or a
record whose payload crosses the command boundary throws
`std::runtime_error`.

The legacy constructor
`Thread(command, size, kind, stream)` has no CPU argument and therefore
keeps raw records with no decoded state. `File` uses the CPU-aware overload,
so commands created by `File` carry the image CPU automatically.

## Decoded contexts

`Thread::Record::state` is present only when the image CPU and the record
flavor select a known layout. `Thread::State` exposes:

| Member | Meaning |
| --- | --- |
| `architecture()` | `Thread::Architecture` (`I386`, `X86_64`, `ARM`, `ARM64`, or `Unknown`). |
| `architectureName()` | Human-readable architecture name. |
| `registers()` | Registers in on-disk order, using their original names. Values are unsigned 64-bit; 32-bit registers are zero-extended, never sign-extended. |
| `registerValue(name)` | Register lookup by original name; empty optional when absent. |
| `pc()` / `sp()` | Program counter and stack pointer. |

The CPU type - not `File::Kind` - selects the flavor family. In particular
flavor 1 is `i386` for x86 CPUs and `ARM` for ARM CPUs. Unknown CPUs keep
raw records only; known flavors from another family also remain raw.

Supported layouts:

| Family | Flavor | Count | Layout |
| --- | --- | --- | --- |
| x86 | 1 | 16 | 16 u32: `eax ebx ecx edx edi esi ebp esp ss eflags eip cs ds es fs gs` |
| x86 | 4 | 42 | 21 u64: `rax rbx rcx rdx rdi rsi rbp rsp r8..r15 rip rflags cs fs gs` |
| x86 | 7 | 44 | u32 inner flavor, u32 inner count, 168-byte union. Inner flavor 1/count 16 or 4/count 42 decodes with the matching layout above; remaining union bytes are padding retained in the outer payload. Unknown inner flavors leave the whole outer record undecoded. |
| ARM | 1 | 17 | 17 u32: `r0..r12 sp lr pc cpsr` |
| ARM64 | 6 | 68 | 33 u64: `x0..x28 fp lr sp pc`, then u32 `cpsr`, then 4 padding bytes retained as raw payload. |

For flavor 7 the outer record keeps its own identity (`flavor == 7`,
`count == 44`, `offset` of the outer record). Only the decoded state is
taken from the inner header. Known counts must match exactly, otherwise the
parser throws `std::runtime_error`.

## Startup entry points

`File::entryPoints()` returns startup sources in load command order.

`LC_MAIN` contributes one source per command. `LC_UNIXTHREAD` contributes
one source per decoded record. `LC_THREAD` never contributes a source; it
is thread state only.

| `File::EntryPointSource` field | Meaning |
| --- | --- |
| `commandIndex` | Index of the source load command in `File::loadCommands()`. |
| `recordIndex` | For `UnixThread`, the index of the decoded record in `Thread::records()`. Empty for `Main`. |
| `kind` | `EntryPointKind::Main` or `EntryPointKind::UnixThread`. |
| `coordinate` | The raw coordinate: `LC_MAIN`'s file offset, or the record's PC. It is preserved even when no segment maps it. |
| `segments` | All segment mapping candidates, in load command order. Empty when nothing maps the coordinate. |

`File::EntryPointKind` distinguishes the two source kinds.

### Segment mapping

Each candidate is a `File::SegmentMapping`:

| Field | Meaning |
| --- | --- |
| `commandIndex` | Index of the segment load command, so overlapping segments and duplicate addresses remain distinguishable. |
| `name` | Segment name. |
| `vmAddress`, `vmSize`, `fileOffset`, `fileSize` | The raw segment fields. |
| `address` | The virtual address corresponding to the source coordinate. Always present. |
| `mappedFileOffset` | The corresponding file offset inside this Mach-O image, or empty when there are no file bytes at that virtual address. |

`Main` candidates require `fileOffset <= coordinate < fileOffset + fileSize`
and a displacement inside `vmSize`; `address` is
`vmAddress + (coordinate - fileOffset)`.

`UnixThread` candidates require `vmAddress <= PC < vmAddress + vmSize`.
`mappedFileOffset` is present only when the displacement is strictly below
`fileSize`; a PC in the zero-fill tail keeps its VA candidate without a
file offset.

Segments are collected before sources are resolved, so a segment placed
after an entry command gives the same result. Overlapping segments and
multiple sources mapping the same address are all retained. File and fat
slice offsets stay relative to the Mach-O image, never the outer fat file.
Each `FatFile` slice resolves against its own CPU and segment table.

No symbol lookup, dependency traversal, execution or address-space
allocation is performed, and the image bytes are never modified.
