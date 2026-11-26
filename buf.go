// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Buffered reading and decoding of DWARF data streams.

package dwarf

import (
	"bytes"
	"encoding/binary"
	"strconv"
)

// Data buffer being decoded.
//
// A buf either wraps a fully resident []byte, or a window (cache) into a
// section accessed through a ReaderAt via remote. In the latter case the
// absolute position is always kept in off, and reads past the current
// window pull exactly the bytes needed from the underlying section instead
// of treating them as an underflow.
type buf struct {
	dwarf  *Data
	order  binary.ByteOrder
	format dataFormat
	name   string
	off    Offset
	data   []byte
	err    error
	// remote, when non-nil, means data is a cache window into a section
	// that is not fully resident in memory. It owns the window and the
	// cursor; each buf gets its own remoteBuf so that independent
	// Readers never share a refillable cache.
	remote *remoteBuf
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

// refill ensures that at least n bytes are available starting at the
// current position. Slice-backed bufs report an underflow; ReaderAt
// backed bufs pull another window from the section. It returns false
// when the buf cannot continue (b.err is set for real failures; a
// clean end-of-window has b.err == nil and the caller decides whether
// that is an underflow).
func (b *buf) refill(n int) bool {
	if n <= len(b.data) {
		return true
	}
	if b.err != nil {
		return false
	}
	if b.remote == nil {
		return false
	}
	return b.remote.fill(b, n)
}

func (b *buf) uint8() uint8 {
	if len(b.data) < 1 {
		if !b.refill(1) {
			if b.err == nil {
				b.error("underflow")
			}
			return 0
		}
	}
	val := b.data[0]
	b.data = b.data[1:]
	b.off++
	return val
}

func (b *buf) bytes(n int) []byte {
	if n < 0 {
		b.error("underflow")
		return nil
	}
	if n == 0 {
		return []byte{}
	}
	if len(b.data) < n {
		if !b.refill(n) {
			if b.err == nil {
				b.error("underflow")
			}
			return nil
		}
	}
	var data []byte
	if b.remote != nil {
		// The window may be replaced by the next refill, so hand out
		// an independent copy.
		data = append([]byte(nil), b.data[:n]...)
	} else {
		data = b.data[0:n]
	}
	b.data = b.data[n:]
	b.off += Offset(n)
	return data
}

func (b *buf) skip(n int) { b.bytes(n) }

func (b *buf) string() string {
	if i := bytes.IndexByte(b.data, 0); i >= 0 {
		s := string(b.data[0:i])
		b.data = b.data[i+1:]
		b.off += Offset(i + 1)
		return s
	}
	if b.remote == nil {
		b.error("underflow")
		return ""
	}
	// The terminating NUL may lie beyond the current window.
	s, ok := b.remote.readString(b)
	if !ok {
		if b.err == nil {
			b.error("underflow")
		}
		return ""
	}
	return s
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
//
// As with the original slice-backed decoder, running out of bytes is
// not itself an error: the value decoded so far is returned, which
// presents an endless stream of zeros to callers such as the
// abbreviation table parser. Real ReaderAt failures are recorded in
// b.err and surfaced by the enclosing entry parse.
func (b *buf) varint() (c uint64, bits uint) {
	for i := 0; i < 10; i++ {
		if len(b.data) < 1 {
			if b.remote == nil || b.err != nil || !b.refill(1) {
				return c, bits
			}
		}
		byt := b.data[0]
		b.data = b.data[1:]
		b.off++
		c |= uint64(byt&0x7F) << bits
		bits += 7
		if byt&0x80 == 0 {
			return c, bits
		}
	}
	return c, bits
}

// Unsigned int is just a varint.
func (b *buf) uint() uint64 {
	x, _ := b.varint()
	return x
}

// Signed int is a sign-extended varint.
func (b *buf) int() int64 {
	ux, bits := b.varint()
	if b.err != nil || bits == 0 {
		return 0
	}
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

// DecodeError is an error that is returned when the parsing of a
// DWARF entry fails.
type DecodeError struct {
	Name   string
	Offset Offset
	Err    string
	// cause wraps the underlying failure, if any (for example an
	// io.ErrUnexpectedEOF from a ReaderAt-backed section).
	cause error
}

func (e DecodeError) Error() string {
	return "decoding dwarf section " + e.Name + " at offset 0x" + strconv.FormatUint(uint64(e.Offset), 16) + ": " + e.Err
}

// Unwrap exposes the underlying read failure so that callers can use
// errors.Is to recognize short reads and I/O errors.
func (e DecodeError) Unwrap() error {
	return e.cause
}
