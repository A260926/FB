# VHDX Parser

Based on documentation from https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/83e061f8-f6e2-4de1-91bd-5d518a43d477

This parser is used by Velociraptor's vhdx accessor. Example of use in
VQL:

```sql
SELECT OSPath.Path AS OSPath, Size, Mode.String
FROM glob(
  globs="*", accessor="raw_ntfs", root=pathspec(
     Path="/",
     DelegateAccessor="offset",
     DelegatePath=pathspec(
         Path="/65536",
         DelegateAccessor="vhdx",
         DelegatePath="/tmp/test.vhdx")))
```

# Testing locally

There is a small tool that allows inspection of the VHDX volume:

The following will print some information about internal data structures and metadata.
```
govhdx parse test.vhdx
```

The following will dump the image into stdout and redirect to a flat dd file.
```
govhdx cat test.vhdx > test.dd
```

# Reading differencing disks with an explicit parent

A differencing VHDX (a disk whose file parameters set `HasParent`) cannot be
opened on its own: `NewVHDXFile` rejects it instead of silently zero-filling
the blocks it does not own. Combine it with the parent through
`NewVHDXFileWithParent`. The parent is supplied by the caller as an already
opened `*VHDXFile`; path strings stored in the parent locator are treated as
metadata only and never used to open a host file. The parent may itself be a
differencing disk, so chains of any depth are built one link at a time.

```go
package main

import (
	"io"
	"os"

	"github.com/Velocidex/go-vhdx/parser"
)

func openChain() (*parser.VHDXFile, error) {
	base, err := os.Open("/data/base.vhdx")
	if err != nil {
		return nil, err
	}
	parent, err := parser.NewVHDXFile(base) // standalone base disk
	if err != nil {
		return nil, err
	}

	diff, err := os.Open("/data/snapshot.vhdx")
	if err != nil {
		return nil, err
	}
	// Routing is decided per logical sector: FULLY_PRESENT blocks come
	// from the child, NOT_PRESENT blocks recurse into the parent,
	// PARTIALLY_PRESENT blocks follow the sector bitmap, and
	// ZERO/UNDEFINED/UNMAPPED blocks always read back as zeros.
	return parser.NewVHDXFileWithParent(diff, parent)
}

func readSector(disk *parser.VHDXFile, offset int64) ([]byte, error) {
	buf := make([]byte, 512)
	n, err := disk.ReadAt(buf, offset)
	if err == io.EOF {
		return buf[:n], io.EOF // prefix read past the virtual end
	}
	return buf[:n], err
}
```

Before the chain is accepted, the child's parent locator `parent_linkage`
must match the parent's currently effective header `DataWriteGuid`, and the
virtual disk size and logical sector size must match (block sizes may
differ). The current header is chosen by CRC32C validity and sequence
number.

## Unsupported: log replay

Only images whose selected current header carries a zero `LogGuid` are
supported. A non-zero `LogGuid` means the image has a replayable log with
pending changes; opening such an image (at any level of the chain) fails
with an explicit error rather than reading stale, pre-replay state. Bring
the disk to a clean state in Hyper-V first.

