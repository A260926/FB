// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dwarf

import (
	"errors"
	"io"
)

// A Section is a DWARF section whose contents are read on demand
// through an [io.ReaderAt] rather than materialized as a single byte
// slice. Size is the number of logical bytes in the section; reads
// beyond Size are rejected even if the underlying reader would return
// data there.
//
// The caller owns ReaderAt and is responsible for keeping it alive for
// as long as the [Data] object (and any [Reader] or [Type] derived from
// it) is in use. The package never closes ReaderAt.
type Section struct {
	Reader io.ReaderAt
	Size   int64
}

// errSectionBounds is returned (wrapped in a [DecodeError]) when a read
// falls outside the declared size of a section.
var errSectionBounds = errors.New("read offset outside section bounds")

// backends holds the source-backed sections of a sparse Data. It is
// kept out of the Data struct on purpose: Data must keep the exact same
// memory layout it had before sparse support, because some clients
// reinterpret a standard-library debug/dwarf.Data as this package's Data
// through an unsafe pointer cast. Sparse Data objects are always used
// through the pointer returned by NewReaderAt (never value-copied), so
// keying the registry on that pointer is stable.
type backends struct {
	info *source
	str  *source
}

var sparseBackends = map[*Data]*backends{}

// registerBackends attaches source-backed sections to a Data.
func registerBackends(d *Data, info, str *source) {
	sparseBackends[d] = &backends{info: info, str: str}
}

// sparse returns the source backends of d, or nil for slice-backed Data.
func (d *Data) sparse() *backends {
	return sparseBackends[d]
}

// source is the read backend for a single section. It is shared by all
// readers and therefore holds no per-reader cursor state.
type source struct {
	name string
	ra   io.ReaderAt
	size int64
}

// readAt fetches n bytes at absolute section offset off. The returned
// count may be less than n only when the section ends or the underlying
// reader fails. Section names and real offsets are attached to every
// failure through readErr.
func (s *source) readAt(p []byte, off Offset) (int, error) {
	if off < 0 || int64(off) >= s.size {
		if len(p) == 0 {
			return 0, nil
		}
		return 0, s.readErr(off, errSectionBounds)
	}
	n := int64(len(p))
	if n > s.size-int64(off) {
		n = s.size - int64(off)
	}
	got, err := s.ra.ReadAt(p[:n], int64(off))
	if err != nil && err != io.EOF {
		return got, s.readErr(off, err)
	}
	if int64(got) < n {
		return got, s.readErr(off, io.ErrUnexpectedEOF)
	}
	return got, err
}

// readErr builds a DecodeError carrying the section name and the real
// section offset while preserving the underlying cause for errors.Is.
func (s *source) readErr(off Offset, cause error) error {
	msg := "underflow"
	if cause != io.EOF && cause != io.ErrUnexpectedEOF && cause != errSectionBounds {
		msg = cause.Error()
	}
	if cause == errSectionBounds {
		msg = errSectionBounds.Error()
	}
	return DecodeError{Name: s.name, Offset: off, Err: msg, cause: cause}
}

// newSource builds a source for a Section, validating its declaration.
func newSource(name string, sec Section) (*source, error) {
	if sec.Reader == nil {
		return nil, errors.New("dwarf: Section " + name + " has nil Reader")
	}
	if sec.Size < 0 {
		return nil, errors.New("dwarf: Section " + name + " has negative Size")
	}
	return &source{name: name, ra: sec.Reader, size: sec.Size}, nil
}

// sectionSource returns the source-backed section of name, or nil when
// this Data was constructed from byte slices.
func (d *Data) sectionSource(name string) *source {
	b := d.sparse()
	if b == nil {
		return nil
	}
	switch name {
	case "info":
		return b.info
	case "str":
		return b.str
	}
	return nil
}

// infoSource returns the source-backed .debug_info section, or nil for
// slice-backed Data.
func (d *Data) infoSource() *source {
	if b := d.sparse(); b != nil {
		return b.info
	}
	return nil
}
