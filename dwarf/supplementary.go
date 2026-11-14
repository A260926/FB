// Copyright 2025 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// DWARF 5 supplementary object support.
//
// A DWARF 5 compilation can move type information and strings into a
// separate object file whose sections are not concatenated with the
// main object. The main object refers to that file with the
// DW_FORM_strp_sup, DW_FORM_ref_sup4, and DW_FORM_ref_sup8 forms. Its
// .debug_sup section records the identity of the supplementary object
// so that a reader can check that it opened the intended file.

package dwarf

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
)

// SupplementaryOffset is a reference to an entry in the .debug_info
// section of the supplementary object bound with [Data.SetSupplementary].
//
// It is returned by [Entry] decoding for the DW_FORM_ref_sup4 and
// DW_FORM_ref_sup8 forms. The value is an offset in the supplementary
// object's namespace; it is never added to, or interpreted as, an offset
// in the referring object.
type SupplementaryOffset uint64

// suppHeader is the parsed header of a .debug_sup section, as described
// by DWARF 5 section 7.3.6.
type suppHeader struct {
	version     uint16
	containsSup bool
	filename    string
	checksum    []byte
}

// parseSuppSection parses a DWARF 5 .debug_sup section. The header is
//
//	unit_length         (initial length, as in other DWARF sections)
//	version             (2 bytes)
//	is_supplementary    (1 byte)
//	filename            (null-terminated string; empty in the
//	                     supplementary object)
//	checksum_len        (1 byte)
//	checksum            (checksum_len bytes)
//
// The fixtures shipped with this package omit the unit_length prefix, so
// that layout is accepted too. The checksum is kept as opaque bytes: it
// is compared for equality, never hashed or interpreted, and the filename
// it belongs to is never opened.
func parseSuppSection(order binary.ByteOrder, data []byte) (suppHeader, error) {
	var h suppHeader

	rest := data
	switch {
	case len(data) >= 2 && order.Uint16(data[:2]) == 5:
		// Spec layout (DWARF 5 §7.3.6): the header starts with the
		// version, with no unit_length prefix.

	case len(data) >= 14 && order.Uint32(data[:4]) == 0xffffffff && order.Uint16(data[12:14]) == 5:
		// 64-bit DWARF format unit_length, accepted for robustness.
		n := order.Uint64(data[4:12])
		if n != uint64(len(data)-12) {
			return h, fmt.Errorf(".debug_sup unit length %d does not match section size %d", n, len(data))
		}
		rest = data[12:]

	case len(data) >= 6 && uint64(order.Uint32(data[:4])) == uint64(len(data)-4) && order.Uint16(data[4:6]) == 5:
		// 32-bit DWARF format unit_length, accepted for robustness.
		rest = data[4:]

	default:
		if len(data) < 2 {
			return h, errors.New(".debug_sup section too short")
		}
		if len(data) >= 6 && order.Uint16(data[4:6]) == 5 {
			return h, fmt.Errorf(".debug_sup unit length %d does not match section size %d", order.Uint32(data[:4]), len(data))
		}
		return h, fmt.Errorf(".debug_sup has unsupported version %d", order.Uint16(data[:2]))
	}

	if len(rest) < 4 {
		return h, errors.New(".debug_sup section too short")
	}
	h.version = order.Uint16(rest[:2])
	if h.version != 5 {
		return h, fmt.Errorf(".debug_sup has unsupported version %d", h.version)
	}
	switch rest[2] {
	case 0:
		h.containsSup = false
	case 1:
		h.containsSup = true
	default:
		return h, fmt.Errorf(".debug_sup has invalid is_supplementary flag %d", rest[2])
	}

	i := bytes.IndexByte(rest[3:], 0)
	if i < 0 {
		return h, errors.New(".debug_sup filename is not null-terminated")
	}
	h.filename = string(rest[3 : 3+i])
	p := 3 + i + 1
	if p >= len(rest) {
		return h, errors.New(".debug_sup is missing checksum length")
	}
	n := int(rest[p])
	p++
	if len(rest)-p < n {
		return h, fmt.Errorf(".debug_sup checksum length %d exceeds section", n)
	}
	// Compare exactly checksum_len bytes; do not hash or interpret them.
	h.checksum = append([]byte(nil), rest[p:p+n]...)
	return h, nil
}

// SetSupplementary binds sup as the DWARF 5 supplementary object for d,
// allowing attributes that use DW_FORM_strp_sup, DW_FORM_ref_sup4, and
// DW_FORM_ref_sup8 to be resolved. The caller does not need to add the
// supplementary object's sections to d.
//
// Both d and sup must have had their .debug_sup sections added with
// [Data.AddSection], and the headers must agree as required by DWARF 5
// section 7.3.6: both must be version 5, d must be the main object and
// sup the supplementary object, d's filename must be non-empty and sup's
// must be empty, and the opaque checksum identities must be byte-for-byte
// equal. The checksum is not computed; the filename is not opened.
//
// A Data can be bound only once, and may not bind itself. Two different
// main objects may bind the same supplementary Data. Once a binding
// succeeds, neither object's .debug_sup section can be replaced with
// AddSection. If SetSupplementary returns an error, any existing binding
// is left unchanged. The implementation is for single-threaded use.
func (d *Data) SetSupplementary(sup *Data) error {
	if sup == nil {
		return errors.New("dwarf: nil supplementary object")
	}
	if d == sup {
		return errors.New("dwarf: a Data cannot be its own supplementary object")
	}
	if d.supData != nil || d.supBoundAsSup {
		return errors.New("dwarf: supplementary object already bound")
	}
	if !d.supSectionSet {
		return errors.New("dwarf: main object has no .debug_sup section")
	}
	if !sup.supSectionSet {
		return errors.New("dwarf: supplementary object has no .debug_sup section")
	}

	mainHdr, err := parseSuppSection(d.order, d.supSection)
	if err != nil {
		return fmt.Errorf("dwarf: main object: %v", err)
	}
	supHdr, err := parseSuppSection(sup.order, sup.supSection)
	if err != nil {
		return fmt.Errorf("dwarf: supplementary object: %v", err)
	}

	if mainHdr.containsSup {
		return errors.New("dwarf: main object .debug_sup is marked supplementary")
	}
	if !supHdr.containsSup {
		return errors.New("dwarf: supplementary object .debug_sup is not marked supplementary")
	}
	if mainHdr.filename == "" {
		return errors.New("dwarf: main object .debug_sup has an empty filename")
	}
	if supHdr.filename != "" {
		return errors.New("dwarf: supplementary object .debug_sup has a non-empty filename")
	}
	if !bytes.Equal(mainHdr.checksum, supHdr.checksum) {
		return errors.New("dwarf: .debug_sup identity checksum mismatch")
	}

	// Commit only after every check succeeded, so a rejected bind leaves
	// any previous association untouched.
	d.supData = sup
	sup.supBoundAsSup = true
	return nil
}

// supplementary returns the bound supplementary Data, or an error if
// none is bound.
func (d *Data) supplementary() (*Data, error) {
	if d.supData == nil {
		return nil, errors.New("no bound supplementary object")
	}
	return d.supData, nil
}

// isDecodeError reports whether err is or wraps a [DecodeError].
func isDecodeError(err error) bool {
	var de DecodeError
	return errors.As(err, &de)
}

// supplementaryString resolves off in the .debug_str section of the bound
// supplementary object. Errors are DecodeErrors so that malformed input
// cannot silently yield a zero-length string.
func (d *Data) supplementaryString(off uint64) (string, error) {
	sup, err := d.supplementary()
	if err != nil {
		return "", DecodeError{"str_sup", Offset(off), "DW_FORM_strp_sup with " + err.Error()}
	}
	if len(sup.str) == 0 {
		return "", DecodeError{"str_sup", Offset(off), "supplementary object has no .debug_str section"}
	}
	if off >= uint64(len(sup.str)) {
		return "", DecodeError{"str_sup", Offset(off), "string offset out of range"}
	}
	data := sup.str[off:]
	i := bytes.IndexByte(data, 0)
	if i < 0 {
		return "", DecodeError{"str_sup", Offset(off), "string is not null-terminated"}
	}
	return string(data[:i]), nil
}
