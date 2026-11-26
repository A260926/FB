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

## Split-segment v2 page groups

`File.ForEachV2SplitSegReference` enumerates the raw `LC_SEGMENT_SPLIT_INFO`
(v2) references. `File.AnalyzeSplitPageGroups` goes one step further: it pairs
each `ARM64_OFF12` reference with the exact `ADRP` that supplies the consumer's
base register, then reports whether that pairing still works if sections move.

```go
analysis, err := m.AnalyzeSplitPageGroups([]macho.SplitPageCodeRange{
    {SectionIndex: textSectionIndex, StartOffset: 0x0, EndOffset: 0x120},
}, map[uint64]uint64{
    constSectionIndex: 0x200000, // one-based section index -> new VM base
})
if err != nil {
    log.Fatal(err)
}
for _, group := range analysis.Groups {
    fmt.Printf("ADRP %#x -> %s (%d consumers)\n",
        group.ADRPVmAddr, group.Status, len(group.Consumers))
}
```

Sections without a new base keep their old address. Index `0` is the image
header (`GetBaseAddress`); it may be a target but never a code source.
Code ranges use one-based section indices and half-open byte offsets; lengths
must be multiples of four, ranges must not overlap, and their instructions must
be file-backed. Targets may live in zero-fill sections but must stay inside the
declared section size.

Pairing does not guess "the nearest preceding ADRP". Each range entry point is
seeded, then direct `B`, `B.cond`, `CBZ`/`CBNZ`, and fall-through edges are
followed, including back edges. The data flow understands `ADRP`, 64-bit `MOV`
register copies, unshifted `ADD` immediates, unsigned-offset `LDR`/`STR` of
W/X registers, `NOP`, and `RET`. `MOV` propagates a source, an ordinary write
clears the destination register, `STR` does not clobber its base, and `X31`
never carries a source.

Unknown instructions and branches that leave the range are **analysis
barriers**: the analyzer will not guess through them. A consumer that is
unreachable, or whose base register cannot be proven because of a barrier,
is reported as `unknown` with a reason. A merge with different ADRPs, or one
path carrying an ADRP and another not, is `ambiguous`.

Consumers with a unique ADRP source are grouped by that ADRP. Each is verified
against the metadata (the original ADRP page plus the instruction's immediate
and access scaling must reproduce the declared old target); a mismatch is
`inconsistent`. The group is recomputed from each consumer's own target
section. If all consumers still land on one page and the signed 21-bit page
distance from the new ADRP address is encodable, the group is `compatible`.
A split into different pages, or an out-of-range page distance, is `conflict`,
and every affected consumer is reported rather than picking one. Identical
references are merged and contradictory metadata is an error. Output is sorted
and repeated calls do not accumulate state.

## License

MIT Copyright (c) 2020-2026 **blacktop**
