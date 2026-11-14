// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package macho

// Regression tests for LC_FILESET_ENTRY support. The images come from the
// read-only synthetic fixture; they are parsed, never executed, and no sample
// offset is rewritten to work around the container/child coordinate split.

import (
	"bytes"
	"encoding/binary"
	"io"
	"os"
	"testing"

	"github.com/Binject/debug/task_support/fixture"
	"github.com/Binject/debug/task_support/taskcheck"
)

// pathKey renders an entry path for map lookups.
func pathKey(path []string) string {
	out := ""
	for i, p := range path {
		if i > 0 {
			out += "/"
		}
		out += p
	}
	return out
}

// ownerByPath finds the ownership record whose entry path matches want.
func ownerByPath(owners []PCOwnership, want ...string) *PCOwnership {
	for i := range owners {
		if pathKey(owners[i].Path) == pathKey(want) {
			return &owners[i]
		}
	}
	return nil
}

// TestFilesetCoordinates covers dual-origin parsing: header and load commands
// read from the entry offset, section and linkedit data read through the
// shared container, and lazy opening of child images.
func TestFilesetCoordinates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		raw := fixture.Fileset()
		before := append([]byte(nil), raw...)

		root, err := NewFile(bytes.NewReader(raw))
		c.Check(err == nil, "fileset NewFile: %v", err)
		c.Check(root.Type == TypeFileset, "root type = %v, want Fileset", root.Type)
		c.Check(len(root.Sections) == 0, "fileset container has %d sections, want 0", len(root.Sections))

		entries := root.FilesetEntries()
		c.Check(len(entries) == 2, "root has %d entries, want 2", len(entries))
		c.Check(root.children == nil, "child opened before OpenEntry was requested")
		opaque, parsed := 0, 0
		for _, l := range root.Loads {
			switch l.(type) {
			case LoadBytes:
				opaque++
			case *FilesetEntry:
				parsed++
			}
		}
		c.Check(opaque == 0 && parsed == 2, "fileset loads: %d opaque, %d parsed entries", opaque, parsed)

		wantID := []string{"left", "right"}
		wantOff := []uint64{0x400, 0x800}
		for i, e := range entries {
			c.Check(e.ID == wantID[i], "entry %d id = %q, want %q", i, e.ID, wantID[i])
			c.Check(e.Vmaddr == 0x100000000, "entry %q vmaddr = %#x", e.ID, e.Vmaddr)
			c.Check(e.Fileoff == wantOff[i], "entry %q fileoff = %#x, want %#x", e.ID, e.Fileoff, wantOff[i])
			c.Check(len(e.Raw()) == 40, "entry %q raw len = %d, want 40", e.ID, len(e.Raw()))
			c.Check(binary.LittleEndian.Uint32(e.Raw()[0:4]) == uint32(LoadCmdFilesetEntry),
				"entry %q raw command = %#x", e.ID, binary.LittleEndian.Uint32(e.Raw()[0:4]))
		}

		left, err := root.OpenEntry("left")
		c.Check(err == nil, "OpenEntry left: %v", err)
		c.Check(len(root.children) == 1, "entry not opened lazily on first request")
		c.Check(left.Type == TypeFileset, "left type = %v, want Fileset", left.Type)
		c.Check(left.Parent() == root, "left parent is not root")
		c.Check(left.Entry().ID == "left" && left.Entry().Fileoff == 0x400, "left entry metadata mismatch")
		c.Check(len(left.FilesetEntries()) == 1 && left.FilesetEntries()[0].ID == "leaf",
			"nested entry id = %v", left.FilesetEntries())

		leaf, err := left.OpenEntry("leaf")
		c.Check(err == nil, "OpenEntry left/leaf: %v", err)
		c.Check(leaf.Type == TypeExec, "leaf type = %v, want Exec", leaf.Type)
		c.Check(len(leaf.Sections) == 4, "leaf sections = %d, want 4", len(leaf.Sections))
		c.Check(len(leaf.Symtab.Syms) == 4, "leaf symbols = %d, want 4", len(leaf.Symtab.Syms))

		// The nested header sits at its absolute container offset, not the sum
		// of offsets along the path, and every level shares one container.
		c.Check(left.headerOff == 0x400, "left headerOff = %#x", left.headerOff)
		c.Check(leaf.headerOff == 0x1000, "leaf headerOff = %#x, want 0x1000 (no offset accumulation)", leaf.headerOff)
		c.Check(leaf.container == root && left.container == root, "nested fileset lost shared container")

		text := leaf.Section("__text")
		c.Check(text != nil, "leaf has no __text section")
		c.Check(text.Addr == 0x100000400, "__text addr = %#x", text.Addr)
		c.Check(text.Offset == 0x1400, "__text container offset = %#x, want 0x1400", text.Offset)
		c.Check(text.Size == 16, "__text size = %d", text.Size)
		c.Check(len(text.Relocs) == 1, "__text relocations = %d, want 1", len(text.Relocs))
		c.Check(text.Relocs[0].Extern && text.Relocs[0].Value == 0 && text.Relocs[0].Len == 3,
			"reloc decoded wrong: %+v", text.Relocs[0])

		data, err := text.Data()
		c.Check(err == nil, "__text Data: %v", err)
		c.Check(bytes.Equal(data, raw[0x1400:0x1410]), "__text bytes do not match container coordinates")
		opened, err := io.ReadAll(text.Open())
		c.Check(err == nil && bytes.Equal(opened, data), "__text Open disagrees with Data")

		// Linkedit data is also read at container-relative offsets.
		c.Check(len(leaf.Symtab.RawSymtab) == 4*16, "raw symtab size = %d", len(leaf.Symtab.RawSymtab))
		c.Check(bytes.Equal(leaf.Symtab.RawSymtab, raw[0x1500:0x1500+len(leaf.Symtab.RawSymtab)]),
			"raw symtab bytes not read at container offset")
		strBase := uint64(0x1580)
		c.Check(bytes.Equal(leaf.Symtab.RawStringtab, raw[strBase:strBase+uint64(len(leaf.Symtab.RawStringtab))]),
			"raw string table not read at container offset")

		dw, err := leaf.DWARF()
		c.Check(err == nil, "leaf DWARF: %v", err)
		die, err := dw.Reader().Next()
		c.Check(err == nil && die != nil && die.Val(3) == "left", "leaf DWARF CU = %v, err %v", die, err)

		// The sibling leaf uses the same container but distinct bytes.
		right, err := root.OpenEntry("right")
		c.Check(err == nil, "OpenEntry right: %v", err)
		rLeaf, err := right.OpenEntry("leaf")
		c.Check(err == nil, "OpenEntry right/leaf: %v", err)
		rText := rLeaf.Section("__text")
		rData, err := rText.Data()
		c.Check(err == nil, "right leaf __text Data: %v", err)
		c.Check(rText.Offset == 0x2400, "right leaf text offset = %#x", rText.Offset)
		c.Check(bytes.Equal(rData, raw[0x2400:0x2410]), "right leaf bytes wrong")
		c.Check(rText != text && rText.Offset != text.Offset, "sibling leaves share one section")

		// Repeated opens are cached, and closing a child does not touch the
		// container's reader.
		leftAgain, err := root.OpenEntry("left")
		c.Check(err == nil && leftAgain == left, "OpenEntry is not stable/cached")
		c.Check(leaf.Close() == nil, "child Close returned error")
		stillGood, err := root.OpenEntry("right")
		c.Check(err == nil && stillGood == right, "container unusable after child Close")

		// Memory mode stays available. The synthetic fileset leaves have no
		// __LINKEDIT segment, so use a real binary that does: NewFileFromMemory
		// must still parse its linkedit-backed data after these changes.
		memBytes, err := os.ReadFile("testdata/gcc-amd64-darwin-exec")
		c.Check(err == nil, "read memory-mode testdata: %v", err)
		memFile, err := NewFileFromMemory(bytes.NewReader(memBytes))
		c.Check(err == nil, "NewFileFromMemory: %v", err)
		c.Check(len(memFile.Symtab.Syms) > 0, "memory mode produced no symbols")
		memData, err := memFile.Section("__text").Data()
		c.Check(err == nil && len(memData) > 0, "memory-mode section data: %d %v", len(memData), err)

		c.Check(fixture.Unchanged(raw, before), "read-only inspection modified the image")
	})
}

// TestFilesetIdentity covers nested entry identity: entry IDs are scoped to
// their immediate parent, same-named children stay distinct, and the parent
// chain and shared container are preserved through nesting.
func TestFilesetIdentity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		raw := fixture.Fileset()
		root, err := NewFile(bytes.NewReader(raw))
		c.Check(err == nil, "fileset NewFile: %v", err)

		c.Check(root.Parent() == nil && root.Entry() == nil, "container claims a parent or entry")
		c.Check(len(root.FilesetEntries()) == 2, "root entry count = %d", len(root.FilesetEntries()))

		left, err := root.OpenEntry("left")
		c.Check(err == nil, "OpenEntry left: %v", err)
		right, err := root.OpenEntry("right")
		c.Check(err == nil, "OpenEntry right: %v", err)

		// Both parents contain a child with the same ID; they must not alias.
		leftLeaf, lErr := left.OpenEntry("leaf")
		rightLeaf, rErr := right.OpenEntry("leaf")
		c.Check(lErr == nil && rErr == nil, "opening nested leaves: %v %v", lErr, rErr)
		c.Check(leftLeaf != rightLeaf, "same-named entries under different parents were conflated")
		c.Check(leftLeaf.Parent() == left && rightLeaf.Parent() == right, "leaf parent links wrong")
		c.Check(leftLeaf.Parent().Parent() == root, "grandparent chain broken")
		c.Check(leftLeaf.Entry().ID == "leaf" && leftLeaf.Entry().Fileoff == 0x1000,
			"left leaf entry = %+v", leftLeaf.Entry())
		c.Check(rightLeaf.Entry().ID == "leaf" && rightLeaf.Entry().Fileoff == 0x2000,
			"right leaf entry = %+v", rightLeaf.Entry())

		// Entry IDs are only unique within their own parent: root has no
		// "leaf", while each group does.
		_, err = root.OpenEntry("leaf")
		c.Check(err != nil, "root unexpectedly resolved a grandchild entry id")
		_, err = left.OpenEntry("missing")
		c.Check(err != nil, "unknown entry id did not error")

		// Same id, different module content: symbols and bytes stay separate.
		c.Check(leftLeaf.Symtab.Syms[0].Name == "left_a", "left leaf first symbol = %q", leftLeaf.Symtab.Syms[0].Name)
		c.Check(rightLeaf.Symtab.Syms[0].Name == "right_a", "right leaf first symbol = %q", rightLeaf.Symtab.Syms[0].Name)
		ld, _ := leftLeaf.Section("__text").Data()
		rd, _ := rightLeaf.Section("__text").Data()
		c.Check(bytes.Equal(ld, raw[0x1400:0x1410]) && bytes.Equal(rd, raw[0x2400:0x2410]),
			"same-named leaves did not keep their own bytes")

		// Every nesting level addresses the same container with absolute,
		// non-accumulated header offsets.
		c.Check(right.headerOff == 0x800 && rightLeaf.headerOff == 0x2000,
			"right-side header offsets = %#x %#x", right.headerOff, rightLeaf.headerOff)
		c.Check(leftLeaf.container == root && rightLeaf.container == root && right.container == root,
			"container identity lost across nesting")

		// An ordinary Mach-O has no fileset identity at all.
		stand, err := NewFile(bytes.NewReader(fixture.Standalone()))
		c.Check(err == nil, "standalone NewFile: %v", err)
		c.Check(stand.FilesetEntries() == nil, "standalone reports fileset entries")
		c.Check(stand.Entry() == nil && stand.Parent() == nil, "standalone claims fileset identity")
	})
}

// TestFilesetPCOwnership covers read-only PC attribution: half-open section
// intervals, overlapping leaf modules, zerofill sections and per-module N_SECT
// symbol selection with aliases and sibling isolation.
func TestFilesetPCOwnership(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		raw := fixture.Fileset()
		root, err := NewFile(bytes.NewReader(raw))
		c.Check(err == nil, "fileset NewFile: %v", err)

		// PC inside both leaves' __text sections: two overlapping owners.
		owners := root.PCOwnership(0x100000404)
		c.Check(len(owners) == 2, "owners at shared PC = %d, want 2", len(owners))
		c.Check(pathKey(owners[0].Path) == "left/leaf" && pathKey(owners[1].Path) == "right/leaf",
			"owner paths = %v %v", owners[0].Path, owners[1].Path)

		leftOwner := ownerByPath(owners, "left", "leaf")
		rightOwner := ownerByPath(owners, "right", "leaf")
		c.Check(leftOwner != nil && rightOwner != nil, "missing an overlapping owner")
		for _, o := range []*PCOwnership{leftOwner, rightOwner} {
			c.Check(len(o.Module.FilesetEntries()) == 0, "reported module is not a leaf")
			c.Check(o.SectionIndex == 0 && o.Section.Name == "__text", "section = %d %q", o.SectionIndex, o.Section.Name)
			c.Check(o.SectionOffset == 4, "section offset = %#x, want 4", o.SectionOffset)
			c.Check(!o.Zerofill, "__text reported as zerofill")
		}
		c.Check(leftOwner.FileOffset == 0x1404, "left file offset = %#x", leftOwner.FileOffset)
		c.Check(rightOwner.FileOffset == 0x2404, "right file offset = %#x", rightOwner.FileOffset)

		// Nearest N_SECT symbol not above the PC; aliases at that address are
		// both kept, the later symbol and the absolute symbol are excluded.
		c.Check(len(leftOwner.Symbols) == 2, "left candidate count = %d", len(leftOwner.Symbols))
		c.Check(leftOwner.Symbols[0].Name == "left_a" && leftOwner.Symbols[1].Name == "left_alias",
			"left candidates = %+v", leftOwner.Symbols)
		c.Check(rightOwner.Symbols[0].Name == "right_a" && rightOwner.Symbols[1].Name == "right_alias",
			"right candidates = %+v", rightOwner.Symbols)

		// Exactly on an alias address: both aliases still win.
		atAlias := root.PCOwnership(0x100000402)
		c.Check(len(atAlias) == 2 && atAlias[0].SectionOffset == 2, "alias-address attribution wrong")
		la := ownerByPath(atAlias, "left", "leaf")
		c.Check(la != nil && len(la.Symbols) == 2 && la.Symbols[0].Value == 0x100000402,
			"alias symbols = %+v", la)

		// Past the alias but before the section end, the later symbol wins.
		atLater := root.PCOwnership(0x100000406)
		ll := ownerByPath(atLater, "left", "leaf")
		c.Check(ll != nil && len(ll.Symbols) == 1 && ll.Symbols[0].Name == "left_later",
			"later-symbol selection = %+v", ll)

		// At the section start no symbol is yet at or below the PC.
		atStart := root.PCOwnership(0x100000400)
		ls := ownerByPath(atStart, "left", "leaf")
		c.Check(ls != nil && ls.SectionOffset == 0 && len(ls.Symbols) == 0,
			"section start symbols = %+v", ls)

		// Half-open interval: the end address is excluded, a gap matches nothing.
		c.Check(len(root.PCOwnership(0x100000410)) == 0, "section end address was included")
		c.Check(len(root.PCOwnership(0x100000010)) == 0, "unmapped gap produced an owner")

		// Debug sections are attributable and carry container file offsets.
		debugOwners := root.PCOwnership(0x100000420)
		c.Check(len(debugOwners) == 2, "debug section owners = %d", len(debugOwners))
		dl := ownerByPath(debugOwners, "left", "leaf")
		c.Check(dl != nil && dl.SectionIndex == 1 && dl.Section.Name == "__debug_abbrev",
			"debug section = %+v", dl)
		c.Check(dl.FileOffset == 0x1420 && len(dl.Symbols) == 0, "debug offset/symbols = %#x %d",
			dl.FileOffset, len(dl.Symbols))

		// Zerofill sections own the PC but have no file bytes or symbols.
		bssOwners := root.PCOwnership(0x100001004)
		c.Check(len(bssOwners) == 2, "zerofill owners = %d", len(bssOwners))
		for _, o := range bssOwners {
			c.Check(o.Zerofill && o.Section.Name == "__bss" && o.SectionIndex == 3,
				"zerofill section = %q idx %d", o.Section.Name, o.SectionIndex)
			c.Check(o.FileOffset == 0 && o.SectionOffset == 4, "zerofill offset = %#x secoff %#x",
				o.FileOffset, o.SectionOffset)
			c.Check(len(o.Symbols) == 0, "zerofill section produced symbols")
		}

		// Sibling isolation: each owner only sees its own module's symbols.
		for _, o := range owners {
			for _, s := range o.Symbols {
				wantPrefix := "left_"
				if pathKey(o.Path) == "right/leaf" {
					wantPrefix = "right_"
				}
				c.Check(bytes.HasPrefix([]byte(s.Name), []byte(wantPrefix)),
					"symbol %q leaked across sibling modules on path %v", s.Name, o.Path)
			}
		}

		// A standalone, non-fileset File is itself the leaf, with an empty path.
		stand, err := NewFile(bytes.NewReader(fixture.Standalone()))
		c.Check(err == nil, "standalone NewFile: %v", err)
		solo := stand.PCOwnership(0x100000404)
		c.Check(len(solo) == 1, "standalone owners = %d", len(solo))
		c.Check(len(solo[0].Path) == 0 && solo[0].Section.Name == "__text" && solo[0].FileOffset == 0x404,
			"standalone ownership = %+v", solo[0])
		c.Check(len(solo[0].Symbols) == 2, "standalone symbols = %+v", solo[0].Symbols)

		c.Check(fixture.Unchanged(raw, append([]byte(nil), raw...)), "ownership query modified the image")
	})
}
