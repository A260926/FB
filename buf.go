// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Buffered reading and decoding of DWARF data streams.

package dwarf

import (
	"bytes"
	"encoding/binary"
	"io"
	"strconv"
)

// sparseReadChunk is the number of bytes fetched in one ReadAt call
// when a buf is backed by an io.ReaderAt. Windows grow on demand, so
// DIE bodies and string sections are only read where the consumer
// actually looks.
const sparseReadChunk = 4096

// Data buffer being decoded.
//
// A buf is either slice-backed (data holds the unread window and src
// is nil) or source-backed (data is a lazily filled window starting at
// off, fetched through src). The two modes share every decoder.
type buf struct {
	dwarf  *Data
	order  binary.ByteOrder
	format dataFormat
	name   string
	off    Offset
	data   []byte
	src    *source
	// limit, when non-zero, is the exclusive section offset this buf
	// is allowed to reach. Source-backed info bufs use it to clip to a
	// unit boundary, mirroring slice-backed unit windows.
	limit Offset
	// exact forces every fill to read only the number of bytes asked
	// for. Unit-header scanning sets it so construction never fetches
	// the DIE bodies that follow a header.
	exact bool
	err   error
}

// Data format, other than byte order. This affects the handling of
// certain field formats.
type dataFormat interface {
	// DWARF version number. Zero means unknown.
	version() int

	// 64-bit DWARF format?
	dwarf64() (dwarf64 bool, isKnown bool)

	// Size of an address, in bytes. Zero means unknown.
	addrsize() int
}

// Some parts of DWARF have no data format, e.g., abbrevs.
type unknownFormat struct{}

func (u unknownFormat) version() int {
	return 0
}

func (u unknownFormat) dwarf64() (bool, bool) {
	return false, false
}

func (u unknownFormat) addrsize() int {
	return 0
}

func makeBuf(d *Data, format dataFormat, name string, off Offset, data []byte) buf {
	return buf{dwarf: d, order: d.order, format: format, name: name, off: off, data: data}
}

// makeSrcBuf returns a source-backed buf positioned at off. No bytes
// are read until one of the decoding methods is called.
func makeSrcBuf(d *Data, format dataFormat, name string, off Offset, src *source) buf {
	return buf{dwarf: d, order: d.order, format: format, name: name, off: off, src: src}
}

// makeSrcBufLimited is like makeSrcBuf but clips reads to the
// half-open section interval [off, limit).
func makeSrcBufLimited(d *Data, format dataFormat, name string, off, limit Offset, src *source) buf {
	return buf{dwarf: d, order: d.order, format: format, name: name, off: off, src: src, limit: limit}
}

// makeSrcBufExact returns a source-backed buf whose fills read only the
// bytes requested, never a larger speculative window. The unit directory
// uses it so construction touches headers alone.
func makeSrcBufExact(d *Data, format dataFormat, name string, off Offset, src *source) buf {
	return buf{dwarf: d, order: d.order, format: format, name: name, off: off, src: src, exact: true}
}

// grow ensures that at least want unread bytes are buffered.
func (b *buf) grow(want int) bool {
	if b.err != nil {
		return false
	}
	if len(b.data) >= want {
		return true
	}
	if b.src == nil {
		b.underflow(nil)
		return false
	}
	n := want
	if !b.exact && n < sparseReadChunk {
		n = sparseReadChunk
	}
	if b.limit != 0 {
		if b.off >= b.limit {
			b.underflow(nil)
			return false
		}
		if room := int(b.limit - b.off); n > room {
			n = room
		}
	}
	old := b.data
	window := make([]byte, len(old), n)
	copy(window, old)
	window = window[:n]
	got, err := b.src.readAt(window[len(old):], b.off+Offset(len(old)))
	window = window[:len(old)+got]
	b.data = window
	if len(window) >= want {
		// A full read may still report io.EOF when the requested
		// window ends exactly at the section boundary.
		return true
	}
	if err == nil {
		err = io.ErrUnexpectedEOF
	}
	b.readFail(err)
	return false
}

func (b *buf) uint8() uint8 {
	if !b.grow(1) {
		return 0
	}
	val := b.data[0]
	b.data = b.data[1:]
	b.off++
	return val
}

func (b *buf) bytes(n int) []byte {
	if n < 0 || !b.grow(n) {
		b.error("underflow")
		return nil
	}
	var data []byte
	if b.src != nil {
		// The window is reused and may be overwritten by the next
		// fill, so hand out an independent copy.
		data = make([]byte, n)
		copy(data, b.data[:n])
	} else {
		data = b.data[0:n]
	}
	b.data = b.data[n:]
	b.off += Offset(n)
	return data
}

func (b *buf) skip(n int) {
	if b.err != nil {
		return
	}
	if n < 0 {
		b.error("negative skip")
		return
	}
	if n <= len(b.data) {
		b.data = b.data[n:]
		b.off += Offset(n)
		return
	}
	if b.src != nil {
		// Discard the window and jump; section bounds are checked
		// by the next read.
		b.off += Offset(n)
		b.data = b.data[:0]
		return
	}
	b.underflow(nil)
}

func (b *buf) string() string {
	for {
		i := bytes.IndexByte(b.data, 0)
		if i >= 0 {
			s := string(b.data[0:i])
			b.data = b.data[i+1:]
			b.off += Offset(i + 1)
			return s
		}
		if !b.grow(len(b.data) + 1) {
			return ""
		}
	}
}

func (b *buf) uint16() uint16 {
	a := b.bytes(2)
	if a == nil {
		return 0
	}
	return b.order.Uint16(a)
}

func (b *buf) uint24() uint32 {
	a := b.bytes(3)
	if a == nil {
		return 0
	}
	if b.dwarf.bigEndian {
		return uint32(a[2]) | uint32(a[1])<<8 | uint32(a[0])<<16
	} else {
		return uint32(a[0]) | uint32(a[1])<<8 | uint32(a[2])<<16
	}
}

func (b *buf) uint32() uint32 {
	a := b.bytes(4)
	if a == nil {
		return 0
	}
	return b.order.Uint32(a)
}

func (b *buf) uint64() uint64 {
	a := b.bytes(8)
	if a == nil {
		return 0
	}
	return b.order.Uint64(a)
}

// Read a varint, which is 7 bits per byte, little endian.
// the 0x80 bit means read another byte.
func (b *buf) varint() (c uint64, bits uint) {
	for i := 0; ; i++ {
		// Reaching the end of the available data terminates a varint
		// as zero, matching the historical behavior: callers rely on
		// things like an empty abbreviation table ending here without
		// a recorded error.
		if !b.peek(i + 1) {
			return 0, 0
		}
		byte := b.data[i]
		c |= uint64(byte&0x7F) << bits
		bits += 7
		if byte&0x80 == 0 {
			b.off += Offset(i + 1)
			b.data = b.data[i+1:]
			return c, bits
		}
	}
}

// peek reports whether at least n unread bytes are buffered, fetching
// another window for source-backed bufs. Slice-backed exhaustion stays
// silent (callers such as abbreviation-table termination rely on it),
// but a source-backed short or failed read is recorded as an error.
func (b *buf) peek(n int) bool {
	if len(b.data) >= n {
		return true
	}
	if b.err != nil || b.src == nil {
		return false
	}
	want := n
	if !b.exact && want < sparseReadChunk {
		want = sparseReadChunk
	}
	if b.limit != 0 {
		if b.off >= b.limit {
			return false
		}
		if room := int(b.limit - b.off); want > room {
			want = room
		}
	}
	old := b.data
	window := make([]byte, len(old), want)
	copy(window, old)
	window = window[:want]
	got, err := b.src.readAt(window[len(old):], b.off+Offset(len(old)))
	window = window[:len(old)+got]
	b.data = window
	if len(window) >= n {
		return true
	}
	if err == nil {
		err = io.ErrUnexpectedEOF
	}
	b.readFail(err)
	return false
}

// Unsigned int is just a varint.
func (b *buf) uint() uint64 {
	x, _ := b.varint()
	return x
}

// Signed int is a sign-extended varint.
func (b *buf) int() int64 {
	ux, bits := b.varint()
	x := int64(ux)
	if x&(1<<(bits-1)) != 0 {
		x |= -1 << bits
	}
	return x
}

// Address-sized uint.
func (b *buf) addr() uint64 {
	switch b.format.addrsize() {
	case 1:
		return uint64(b.uint8())
	case 2:
		return uint64(b.uint16())
	case 4:
		return uint64(b.uint32())
	case 8:
		return b.uint64()
	}
	b.error("unknown address size")
	return 0
}

func (b *buf) unitLength() (length Offset, dwarf64 bool) {
	length = Offset(b.uint32())
	if length == 0xffffffff {
		dwarf64 = true
		length = Offset(b.uint64())
	} else if length >= 0xfffffff0 {
		b.error("unit length has reserved value")
	}
	return
}

func (b *buf) error(s string) {
	if b.err == nil {
		b.data = nil
		b.err = DecodeError{Name: b.name, Offset: b.off, Err: s}
	}
}

// underflow records a short read, wrapping the underlying error when
// one is available.
func (b *buf) underflow(cause error) {
	if b.err != nil {
		return
	}
	b.data = nil
	b.err = DecodeError{Name: b.name, Offset: b.off, Err: "underflow", cause: cause}
}

// readFail records an error returned by a source-backed section.
func (b *buf) readFail(err error) {
	if b.err != nil {
		return
	}
	b.data = nil
	msg := "underflow"
	if err != io.EOF && err != io.ErrUnexpectedEOF {
		msg = err.Error()
	}
	b.err = DecodeError{Name: b.name, Offset: b.off, Err: msg, cause: err}
}

// DecodeError is returned for malformed or unreadable DWARF data.
type DecodeError struct {
	Name   string
	Offset Offset
	Err    string

	// cause is the underlying error from a section ReaderAt, if any.
	cause error
}

func (e DecodeError) Error() string {
	return "decoding dwarf section " + e.Name + " at offset 0x" + strconv.FormatUint(uint64(e.Offset), 16) + ": " + e.Err
}

// Unwrap exposes the underlying section read error so callers can use
// errors.Is to identify transport-level failures.
func (e DecodeError) Unwrap() error {
	return e.cause
}
