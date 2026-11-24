// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package flexfec

import (
	"encoding/binary"

	"github.com/pion/rtp"
)

// rankRecovery is one media packet uniquely reconstructed by the rank solver.
type rankRecovery struct {
	ext int64
	raw []byte
}

// gfEquation is one incremental GF(2) equation.
//
// cols is the set of (extended media sequence number) coefficients equal to 1.
// rhs is the XOR recovery bit string: the first 8 bytes are the FlexFEC header
// recovery word (V/P/X/CC/M/PT, length recovery, timestamp recovery) and the
// remaining bytes are the XOR of every protected packet's bytes after the fixed
// 12 byte RTP header (CSRC, extension, payload and padding). Equation right hand
// sides of different lengths are zero extended when XORed.
type gfEquation struct {
	cols map[int64]struct{}
	rhs  []byte
}

func newGFEquation(cols []int64, rhs []byte) *gfEquation {
	e := &gfEquation{
		cols: make(map[int64]struct{}, len(cols)),
		rhs:  append([]byte(nil), rhs...),
	}
	for _, c := range cols {
		e.cols[c] = struct{}{}
	}

	return e
}

func (e *gfEquation) clone() *gfEquation {
	c := &gfEquation{rhs: append([]byte(nil), e.rhs...), cols: make(map[int64]struct{}, len(e.cols))}
	for k := range e.cols {
		c.cols[k] = struct{}{}
	}

	return c
}

// xor combines two equations: coefficients are the symmetric difference and the
// right hand sides are XORed with zero extension to the larger length.
func (e *gfEquation) xor(other *gfEquation) {
	for c := range other.cols {
		if _, ok := e.cols[c]; ok {
			delete(e.cols, c)
		} else {
			e.cols[c] = struct{}{}
		}
	}

	xorBytes(&e.rhs, other.rhs)
}

func (e *gfEquation) xorVector(vec []byte) {
	xorBytes(&e.rhs, vec)
}

func xorBytes(dst *[]byte, src []byte) {
	if len(src) > len(*dst) {
		grown := make([]byte, len(src))
		copy(grown, *dst)
		*dst = grown
	}

	for i := range src {
		(*dst)[i] ^= src[i]
	}
}

func minCol(e *gfEquation) int64 {
	var min int64
	first := true
	for c := range e.cols {
		if first || c < min {
			min = c
			first = false
		}
	}

	return min
}

// gfRankTable maintains the accepted independent equations and the media
// packets known inside the window. It is the multi-equation production solving
// path used by RepairReceiverInterceptor and reached through fecDecoder.
type gfRankTable struct {
	mediaSSRC    uint32
	maxEquations int

	// eq is the FIFO of accepted independent equations in their stable form:
	// columns for packets that become known are substituted and removed.
	eq []*gfEquation

	// known maps extended sequence number to the exact wire bytes while the
	// packet remains inside the window.
	known map[int64][]byte
}

func newGFRankTable(mediaSSRC uint32, maxEquations int) *gfRankTable {
	return &gfRankTable{
		mediaSSRC:    mediaSSRC,
		maxEquations: maxEquations,
		known:        make(map[int64][]byte),
	}
}

// enableRank turns on the incremental rank solving path on the existing decoder.
func (d *fecDecoder) enableRank(maxEquations int) {
	d.rank = newGFRankTable(d.protectedStreamSSRC, maxEquations)
}

// recoveryVector builds the GF(2) bit string contributed by one known packet.
//
// It follows the existing FlexFEC-03 rule: bytes 0/1, a network ordered length
// recovery value (wire length minus 12) in the bytes 2/3 slot, bytes 4..7 and
// then every wire byte from offset 12 onwards.
func recoveryVector(raw []byte) []byte {
	bodyLen := len(raw) - BaseRTPHeaderSize
	if bodyLen < 0 {
		bodyLen = 0
	}

	vec := make([]byte, 8+bodyLen)
	if len(raw) >= 2 {
		vec[0] = raw[0]
		vec[1] = raw[1]
	}
	binary.BigEndian.PutUint16(vec[2:4], uint16(bodyLen)) //nolint:gosec // G115
	if len(raw) >= 8 {
		copy(vec[4:8], raw[4:8])
	}
	if bodyLen > 0 && len(raw) >= BaseRTPHeaderSize {
		copy(vec[8:], raw[BaseRTPHeaderSize:])
	}

	return vec
}

// addKnown records one original or reconstructed packet, substitutes it into
// every live equation and continues elimination until no more unique solutions
// exist.
func (t *gfRankTable) addKnown(ext int64, raw []byte) []rankRecovery {
	t.known[ext] = append([]byte(nil), raw...)
	vec := recoveryVector(raw)

	kept := make([]*gfEquation, 0, len(t.eq))
	for _, e := range t.eq {
		if _, ok := e.cols[ext]; ok {
			e.xorVector(vec)
			delete(e.cols, ext)
		}
		if len(e.cols) > 0 {
			kept = append(kept, e)
		}
	}
	t.eq = kept

	return t.solve()
}

// addEquation submits one repair equation. It returns false when the equation is
// a duplicate or linearly dependent on the accepted state (and therefore was
// not retained). When independent it is inserted, the equation budget is
// enforced by evicting the earliest accepted independent equation, and
// elimination is run to a fixed point.
func (t *gfRankTable) addEquation(cols []int64, head []byte, body []byte) (bool, []rankRecovery) {
	rhs := append(append([]byte(nil), head...), body...)
	cand := newGFEquation(cols, rhs)

	// Substitute every packet already known.
	for c := range cand.cols {
		if raw, ok := t.known[c]; ok {
			cand.xorVector(recoveryVector(raw))
			delete(cand.cols, c)
		}
	}

	if len(cand.cols) == 0 {
		// Duplicate / fully known / dependent equation.
		return false, nil
	}

	// Independence test against a fresh Jordan basis of accepted equations.
	// Reduce a probe clone: the stable FIFO row must keep its original
	// (known-substituted) coefficients so a later earliest-row eviction and
	// rebuild is exact.
	basis := jordanBasis(t.eq)
	probe := cand.clone()
	reduceWithBasis(probe, basis)
	if len(probe.cols) == 0 {
		return false, nil
	}

	t.eq = append(t.eq, cand)
	if t.maxEquations > 0 && len(t.eq) > t.maxEquations {
		excess := len(t.eq) - t.maxEquations
		t.eq = t.eq[excess:]
	}

	return true, t.solve()
}

// hasKnown reports whether ext is currently a known packet in the window.
func (t *gfRankTable) hasKnown(ext int64) bool {
	_, ok := t.known[ext]

	return ok
}

// slideWindow releases known packets at or before cutoff and discards whole
// equations that still depend on an unknown column which has slid out.
//
// A known column is merely released here because it was already substituted
// into live equations when it became known. An unknown column must never simply
// be deleted from a row, as that would manufacture a false singleton.
func (t *gfRankTable) slideWindow(cutoff int64) {
	bad := make(map[int64]struct{})
	for _, e := range t.eq {
		for c := range e.cols {
			if c <= cutoff {
				if _, known := t.known[c]; !known {
					bad[c] = struct{}{}
				}
			}
		}
	}

	if len(bad) > 0 {
		kept := make([]*gfEquation, 0, len(t.eq))
		for _, e := range t.eq {
			drop := false
			for c := range bad {
				if _, ok := e.cols[c]; ok {
					drop = true

					break
				}
			}
			if !drop {
				kept = append(kept, e)
			}
		}
		t.eq = kept
	}

	for ext := range t.known {
		if ext <= cutoff {
			delete(t.known, ext)
		}
	}
}

// jordanBasis builds a reduced (Gauss-Jordan) basis. Each pivot column is owned
// by exactly one row and pivot rows contain no other pivot columns. The input
// equations are not modified.
func jordanBasis(eqs []*gfEquation) map[int64]*gfEquation {
	pivots := make(map[int64]*gfEquation)
	for _, src := range eqs {
		row := src.clone()
		reduceWithBasis(row, pivots)
		if len(row.cols) == 0 {
			continue
		}

		p := minCol(row)

		// Remove the new pivot from all existing pivot rows.
		for _, r := range pivots {
			if _, ok := r.cols[p]; ok {
				r.xor(row)
			}
		}
		pivots[p] = row
	}

	return pivots
}

// reduceWithBasis XORs pivot rows into e whenever e contains a pivot column
// until no pivot column remains.
func reduceWithBasis(e *gfEquation, pivots map[int64]*gfEquation) {
	for {
		var pivotCol int64
		found := false
		for c := range e.cols {
			if pr, ok := pivots[c]; ok && pr != e { //nolint:revive
				pivotCol = c
				found = true

				break
			}
		}
		if !found {
			return
		}
		e.xor(pivots[pivotCol])
	}
}

// solve repeatedly extracts unit rows from the Jordan basis, reconstructs the
// unique packet, commits it as known and substitutes it back, until rank can no
// longer produce a unique solution.
func (t *gfRankTable) solve() []rankRecovery {
	recovered := make([]rankRecovery, 0)

	for {
		pivots := jordanBasis(t.eq)

		target := int64(0)
		found := false
		for c, row := range pivots {
			if len(row.cols) == 1 {
				if !found || c < target {
					target = c
					found = true
				}
			}
		}
		if !found {
			return recovered
		}

		row := pivots[target]
		raw, ok := reconstructPacket(row.rhs, target, t.mediaSSRC)
		if !ok {
			// Never spin on a row that cannot produce a structurally valid
			// packet. Under legal FlexFEC data this is unreachable.
			return recovered
		}

		recovered = append(recovered, rankRecovery{ext: target, raw: raw})

		// Commit the solution and substitute it into the stable equations.
		t.known[target] = append([]byte(nil), raw...)
		vec := recoveryVector(raw)
		kept := make([]*gfEquation, 0, len(t.eq))
		for _, e := range t.eq {
			if _, ok := e.cols[target]; ok {
				e.xorVector(vec)
				delete(e.cols, target)
			}
			if len(e.cols) > 0 {
				kept = append(kept, e)
			}
		}
		t.eq = kept
	}
}

// reconstructPacket turns a unit-row right hand side into exact RTP wire bytes
// following the existing FlexFEC-03 recovery rules.
func reconstructPacket(rhs []byte, ext int64, mediaSSRC uint32) ([]byte, bool) {
	if len(rhs) < 8 {
		return nil, false
	}

	bodyLen := int(binary.BigEndian.Uint16(rhs[2:4]))
	raw := make([]byte, BaseRTPHeaderSize+bodyLen)

	copy(raw[0:8], rhs[0:8])

	// Version must be 2: set top bit and clear the second top bit.
	raw[0] |= 0x80
	raw[0] &^= 0x40

	// Recovered sequence number and protected media SSRC are known directly.
	binary.BigEndian.PutUint16(raw[2:4], uint16(ext)) //nolint:gosec // G115
	binary.BigEndian.PutUint32(raw[8:12], mediaSSRC)

	if bodyLen > 0 && len(rhs) > 8 {
		copy(raw[BaseRTPHeaderSize:], rhs[8:min(len(rhs), 8+bodyLen)])
	}

	// Validate the complete structure (CSRC, extension and padding) before
	// exposing or committing the reconstruction.
	var pkt rtp.Packet
	if err := pkt.Unmarshal(raw); err != nil {
		return nil, false
	}

	return raw, true
}

// extendSequence maps seq16 to an extended sequence number close to anchorExt
// using the RFC 1982 half range. Forward crossing of 65535 to 0 is positive and
// the active span is assumed to be below 32768.
func extendSequence(anchorExt int64, seq16 uint16) int64 {
	anchor16 := uint16(anchorExt) //nolint:gosec // G115
	delta := int32(int16(seq16 - anchor16))

	return anchorExt + int64(delta)
}
