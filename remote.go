// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dwarf

import (
	"bytes"
	"fmt"
	"io"
)

// Section is a DWARF section whose bytes are accessed on demand through
// r instead of being materialized as a single []byte. Size is the
// declared length of the section; reads beyond it are reported as a
// short read.
//
// The package never closes r: its lifetime is the caller's
// responsibility. A Section is safe for concurrent use as long as the
// underlying ReaderAt is.
type Section struct {
	Reader io.ReaderAt
	Size   int64
}

// remoteSection describes a section that is not fully resident in
// memory. Offsets into it keep their full 64-bit values; the package
// never renumbers or compacts the source.
type remoteSection struct {
	r    io.ReaderAt
	size int64
	name string
}

// readAt fetches len(p) bytes at the absolute section offset off.
// A short read, an out-of-range offset or an underlying error is
// wrapped in a DecodeError carrying the section name and the real
// offset, while preserving the original error for errors.Is.
func (s *remoteSection) readAt(p []byte, off int64) error {
	if off < 0 || off >= s.size {
		return DecodeError{
			Name:   s.name,
			Offset: Offset(off),
			Err:    fmt.Sprintf("offset %d out of range (section size %d)", off, s.size),
			cause:  io.ErrUnexpectedEOF,
		}
	}
	if int64(len(p)) > s.size-off {
		p = p[:s.size-off]
		err := DecodeError{
			Name:   s.name,
			Offset: Offset(off),
			Err:    "short read: offset past end of section",
			cause:  io.ErrUnexpectedEOF,
		}
		if _, rerr := s.r.ReadAt(p, off); rerr != nil {
			err.cause = rerr
		}
		return err
	}
	n, err := s.r.ReadAt(p, off)
	if err == io.EOF && n == len(p) {
		// ReaderAt may return EOF when filling exactly to the end.
		return nil
	}
	if err != nil {
		return DecodeError{
			Name:   s.name,
			Offset: Offset(off),
			Err:    fmt.Sprintf("read failed: %v", err),
			cause:  err,
		}
	}
	if n != len(p) {
		return DecodeError{
			Name:   s.name,
			Offset: Offset(off),
			Err:    fmt.Sprintf("short read: got %d bytes, want %d", n, len(p)),
			cause:  io.ErrUnexpectedEOF,
		}
	}
	return nil
}

// readWindow returns a fresh window of at most size bytes starting at
// off. The returned slice never aliases a previously returned window,
// so callers may retain values decoded from it across later fills.
func (s *remoteSection) readWindow(off Offset, size int) ([]byte, error) {
	if size <= 0 {
		return nil, nil
	}
	win := make([]byte, size)
	if err := s.readAt(win, int64(off)); err != nil {
		return nil, err
	}
	return win, nil
}

// defaultWindowSize bounds how much of a remote section is pulled in
// on a cache miss. Individual reads larger than this fetch exactly
// what they need; windows are never shared between bufs.
const defaultWindowSize = 4096

// remoteBuf is the per-buf cursor and window cache for a remote
// section. Every Reader and every nested lookup gets its own
// remoteBuf, so a failure or refill in one Reader cannot corrupt
// another Reader's cursor.
type remoteBuf struct {
	sec *remoteSection
	win []byte
}

func newRemoteBuf(sec *remoteSection) *remoteBuf {
	return &remoteBuf{sec: sec}
}

// fill repositions b onto a window containing at least n bytes from
// b.off. On success b.data is updated in place and true is returned.
func (rb *remoteBuf) fill(b *buf, n int) bool {
	off := b.off
	if int64(off) >= rb.sec.size {
		// Clean end of section: leave b.err unset so sequential
		// iteration can distinguish EOF from a real decode error.
		b.data = b.data[:0]
		return false
	}
	size := defaultWindowSize
	if n > size {
		size = n
	}
	if remaining := rb.sec.size - int64(off); int64(size) > remaining {
		size = int(remaining)
	}
	win, err := rb.sec.readWindow(off, size)
	if err != nil {
		b.err = err
		b.data = nil
		return false
	}
	rb.win = win
	b.data = win
	return len(win) >= n
}

// readString returns the NUL-terminated string starting at b.off,
// reading further windows when the terminator is not in the current
// window. The returned string is freshly allocated and independent
// of the cache.
func (rb *remoteBuf) readString(b *buf) (string, bool) {
	var out []byte
	for {
		if len(b.data) == 0 {
			if !rb.fill(b, 1) {
				return "", false
			}
		}
		for i, c := range b.data {
			if c == 0 {
				out = append(out, b.data[:i]...)
				b.data = b.data[i+1:]
				b.off += Offset(i + 1)
				return string(out), true
			}
		}
		out = append(out, b.data...)
		b.off += Offset(len(b.data))
		b.data = b.data[:0]
	}
}

// makeRemoteBuf builds a buf positioned at off inside a ReaderAt
// backed section.
func (d *Data) makeRemoteBuf(format dataFormat, name string, off Offset, sec *remoteSection) buf {
	return buf{
		dwarf:  d,
		order:  d.order,
		format: format,
		name:   name,
		off:    off,
		remote: newRemoteBuf(sec),
	}
}

// readStringAt returns the NUL-terminated string at absolute offset off
// in the .debug_str section. It works for both the slice-backed and the
// ReaderAt-backed forms and always returns a string that does not alias
// any refillable cache.
func (d *Data) readStringAt(off Offset) (string, error) {
	if d.strSec != nil {
		rb := newRemoteBuf(d.strSec)
		b := buf{
			dwarf:  d,
			order:  d.order,
			format: unknownFormat{},
			name:   "str",
			off:    off,
			remote: rb,
		}
		s, ok := rb.readString(&b)
		if !ok {
			if b.err != nil {
				return "", b.err
			}
			return "", DecodeError{Name: "str", Offset: off, Err: "underflow", cause: io.ErrUnexpectedEOF}
		}
		return s, nil
	}
	if int64(off) > int64(len(d.str)) {
		return "", DecodeError{Name: "str", Offset: off, Err: "offset out of range", cause: io.ErrUnexpectedEOF}
	}
	data := d.str[off:]
	i := bytes.IndexByte(data, 0)
	if i < 0 {
		return "", DecodeError{Name: "str", Offset: off, Err: "underflow", cause: io.ErrUnexpectedEOF}
	}
	return string(data[:i]), nil
}
