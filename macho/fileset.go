// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package macho

// This file implements LC_FILESET_ENTRY support.
//
// A fileset is a Mach-O container whose LC_FILESET_ENTRY commands point at
// complete child images embedded in the same bytes. The child's Mach-O header
// starts at the entry's file offset, but the offsets inside its load commands
// (section offsets, relocation offsets, the symbol and string tables) are
// relative to the start of the whole container, not to the child header.
//
// Every File opened from a fileset therefore shares the container File's
// underlying reader and coordinate system. The only offset interpreted
// relative to the child itself is the location of its header and load
// commands; nested filesets reuse the same container so offsets are never
// accumulated across levels.

import (
	"bytes"
	"encoding/binary"
	"fmt"
)

// filesetEntryCmd is the fixed prefix of an LC_FILESET_ENTRY command:
// cmd, cmdsize, vmaddr, fileoff, entry_id (offset to the ID string), reserved.
type filesetEntryCmd struct {
	Cmd     LoadCmd
	Len     uint32
	Vmaddr  uint64
	Fileoff uint64
	EntryID uint32
	Resvd   uint32
}

// A FilesetEntry is a single LC_FILESET_ENTRY load command of a fileset
// container. It records where a child image is mapped and where its header
// lives in the container, plus the raw command bytes.
type FilesetEntry struct {
	LoadBytes
	// ID is the entry's identifier string. IDs are only unique among the
	// entries of the File that owns this entry.
	ID string
	// Vmaddr is the virtual address at which the child image is mapped.
	Vmaddr uint64
	// Fileoff is the container-relative offset of the child's Mach-O header.
	Fileoff uint64
}

// parseFilesetEntry decodes one LC_FILESET_ENTRY command. cmddat is the
// command's bytes as read from this image's load-command area; off is only
// used to anchor format errors.
func parseFilesetEntry(bo binary.ByteOrder, cmddat []byte, off int64) (*FilesetEntry, error) {
	var hdr filesetEntryCmd
	if err := binary.Read(bytes.NewReader(cmddat), bo, &hdr); err != nil {
		return nil, err
	}
	if hdr.EntryID >= uint32(len(cmddat)) {
		return nil, &FormatError{off, "invalid entry id in fileset entry command", hdr.EntryID}
	}
	return &FilesetEntry{
		LoadBytes: LoadBytes(cmddat),
		ID:        cstring(cmddat[hdr.EntryID:]),
		Vmaddr:    hdr.Vmaddr,
		Fileoff:   hdr.Fileoff,
	}, nil
}

// FilesetEntries returns the LC_FILESET_ENTRY commands of this File in file
// order. It returns nil for an ordinary Mach-O. The returned slice and entries
// are owned by the File and must not be modified.
func (f *File) FilesetEntries() []*FilesetEntry {
	return f.entries
}

// Entry returns the LC_FILESET_ENTRY that was used to open this File, or nil
// for the container itself and for standalone images opened with NewFile.
func (f *File) Entry() *FilesetEntry {
	return f.entry
}

// Parent returns the File whose fileset entry opened this File, or nil for
// the container and for standalone images.
func (f *File) Parent() *File {
	return f.parent
}

// OpenEntry opens the child image identified by id, where id must match one of
// this File's own LC_FILESET_ENTRY identifiers. Entry IDs are only unique
// within their immediate parent, so a child of a different parent is never
// returned even if it carries the same ID.
//
// The child's header and load commands are read from the entry's file offset,
// while its sections and linkedit data are read through the shared container
// using the container-relative offsets recorded in the child. The same
// container is reused at every nesting level, so offsets are not accumulated.
//
// Opening a child does not take ownership of the container: closing the child
// never closes the underlying reader, which stays owned by the File returned
// by NewFile or Open.
func (f *File) OpenEntry(id string) (*File, error) {
	var entry *FilesetEntry
	for _, e := range f.entries {
		if e.ID == id {
			entry = e
			break
		}
	}
	if entry == nil {
		return nil, &FormatError{f.headerOff,
			fmt.Sprintf("fileset has no entry with id %q", id), nil}
	}
	if f.children == nil {
		f.children = make(map[string]*File)
	}
	if child, ok := f.children[id]; ok {
		return child, nil
	}
	// The container always owns the reader; for a standalone File that is f
	// itself. Reuse it directly so every child, including children of nested
	// filesets, reads container-relative coordinates without added offsets.
	container := f.container
	if container == nil {
		container = f
	}
	child, err := newFileInternal(container.r, f.memoryMode, container, int64(entry.Fileoff))
	if err != nil {
		return nil, err
	}
	child.entry = entry
	child.parent = f
	f.children[id] = child
	return child, nil
}
