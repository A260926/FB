# go-macho

[![Go](https://github.com/blacktop/go-macho/actions/workflows/go.yml/badge.svg)](https://github.com/blacktop/go-macho/actions/workflows/go.yml) [![Go Reference](https://pkg.go.dev/badge/github.com/blacktop/go-macho.svg)](https://pkg.go.dev/github.com/blacktop/go-macho) [![License](http://img.shields.io/:license-mit-blue.svg)](http://doge.mit-license.org)

> Package macho implements access to and creation of Mach-O object files.

---

## Why 🤔

This package goes beyond the Go's `debug/macho` to:

- Cover ALL load commands and architectures
- Provide nice summary string output
- Allow for creating custom MachO files
- Parse Objective-C runtime information (non-fragile runtime ABI; legacy `__OBJC` metadata is detected and reported unsupported)
- Parse Swift runtime information
- Read/Write code signature information
- Parse fixup chain information

## Install

```bash
$ go get github.com/blacktop/go-macho
```

## Getting Started

```go
package main

import "github.com/blacktop/go-macho"

func main() {
    m, err := macho.Open("/path/to/macho")
    if err != nil {
        panic(err)
    }
    defer m.Close()

    fmt.Println(m.FileTOC.String())
}
```

## Logging

This library uses Go's structured logging (`log/slog`) for diagnostic messages. By default, no logs are emitted. To see library diagnostics (warnings about malformed Mach-O data, etc.), set up an slog handler in your application:

```go
import (
	"log/slog"
	"os"
)

// Enable debug logging
slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{
	Level: slog.LevelWarn, // Or slog.LevelDebug for more verbose output
})))
```

This allows you to control go-macho's logging output in your application without it polluting stdout by default.

## Split-segment page group analysis

`File.AnalyzeSplitPageGroups` answers a question `ForEachV2SplitSegReference`
cannot: which `ARM64_OFF12` references really share one `ADRP`, and whether
that sharing still encodes once sections move to hypothetical new virtual
bases. It is a read-only static analysis — instructions are decoded but never
modified or executed, `Export` is not called, and the `File` keeps no state
between calls.

Pass half-open code ranges as one-based section indices (section `0` is the
image header, resolved through `GetBaseAddress`, and can never be a code
source) plus an optional map from section index to a new virtual base;
sections absent from the map keep their current base. References come from the
real `LC_SEGMENT_SPLIT_INFO` v2 payload, and only `ARM64_ADRP`/`ARM64_OFF12`
references participate — every other kind is left to the existing interfaces.

The pairing is proved, not guessed from the nearest preceding `ADRP`. Starting
at the first instruction of each range, the analysis follows direct `B`,
`B.cond`, `CBZ`/`CBNZ` and fall-through edges (including loop back edges) and
tracks per-register provenance through `ADRP`, 64-bit `MOV`, no-shift `ADD`
immediate, unsigned-offset W/X `LDR`/`STR`, `NOP` and `RET`. Each `OFF12`
reference keeps its source and target section/offset, original and
hypothetical VM addresses, and a final status:

- **compatible** — a unique `ADRP` source was proved, the original
  `ADRP`+`OFF12` encoding reconstructs the metadata-declared target, and in
  the new layout every consumer still lands on one encodable page.
- **conflict** — the group's consumers split across pages, or the signed
  21-bit `ADRP` page distance overflows. Every consumer in the group is
  reported; the analysis never picks one target and keeps going.
- **ambiguous** — a join merges different `ADRP` sources, or one path carries
  a value that is not an `ADRP` page at all.
- **unknown** — no single `ADRP` can be proved. The reference carries the
  reason, which is always one of the two barrier situations below.
- **inconsistent** — the `ADRP` page displacement plus the consumer's
  immediate and memory access scale do not reconstruct the target the
  metadata declares.

### Analysis barriers and the unknown status

The analyzer only reasons about the supported instruction set and the given
code ranges, and it never speculates through something it cannot model. Two
situations are barriers:

1. An unsupported or unrecognized instruction on a reachable path. It may
   clobber any register, so provenance does not cross it.
2. A branch (or the end of a range) that leaves the analyzed region. The
   value a register has on return is unknowable from the supplied ranges.

Barriers are modeled explicitly rather than by dropping the path: at a join,
a path that crossed a barrier still participates in the register merge. If
every path to an `OFF12` consumer crosses a barrier — or the base register is
never established on any reachable path — the consumer is **unknown** and its
`Reason` names the barrier. If a blocked path joins a clean path that proves
an `ADRP`, the result is **ambiguous** instead, because the unprovable path
might carry a different value. Split a function into its real entry ranges so
each range's first instruction is a genuine entry; otherwise instructions only
reachable from outside a range are reported through these statuses rather
than guessed at.


## License

MIT Copyright (c) 2020-2026 **blacktop**
