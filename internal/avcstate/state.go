// Package avcstate tracks the currently valid AVC sequence and picture
// parameter sets (SPS/PPS) while scanning MP4 samples in decode order.
//
// Instances are not safe for concurrent use.
package avcstate

import (
	"errors"
	"fmt"
	"sort"
)

// Common errors returned when AVC parameter-set or slice-header parsing fails.
var (
	// ErrTruncated means an EBSP/RBSP structure or Exp-Golomb value ran past
	// the end of the NAL unit.
	ErrTruncated = errors.New("avcstate: truncated NAL unit")
	// ErrIllegalEscape means an emulation-prevention sequence is malformed.
	ErrIllegalEscape = errors.New("avcstate: illegal emulation-prevention byte sequence")
	// ErrUnknownPPS means a slice references a picture parameter set id that
	// is not currently in the table.
	ErrUnknownPPS = errors.New("avcstate: slice references unknown PPS id")
	// ErrUnknownSPS means a picture parameter set references a sequence
	// parameter set id that is not currently in the table.
	ErrUnknownSPS = errors.New("avcstate: PPS references unknown SPS id")
)

// PPS is a stored picture parameter set: its original (escaped) NAL bytes and
// the SPS id it references.
type PPS struct {
	NAL   []byte
	SPSID uint32
}

// Table is the active parameter-set table. SPS and PPS are keyed by their
// in-stream ids. A later parameter set with the same id replaces the previous
// bytes ("same id but different bytes" is an update).
type Table struct {
	sps map[uint32][]byte
	pps map[uint32]PPS
}

// NewTable returns an empty parameter-set table.
func NewTable() *Table {
	return &Table{
		sps: map[uint32][]byte{},
		pps: map[uint32]PPS{},
	}
}

// Clone returns a deep copy of the table. NAL byte slices are copied so later
// updates do not mutate snapshots.
func (t *Table) Clone() *Table {
	c := NewTable()
	for id, nal := range t.sps {
		cp := make([]byte, len(nal))
		copy(cp, nal)
		c.sps[id] = cp
	}
	for id, p := range t.pps {
		cp := make([]byte, len(p.NAL))
		copy(cp, p.NAL)
		c.pps[id] = PPS{NAL: cp, SPSID: p.SPSID}
	}
	return c
}

// SPS returns the SPS NAL for id.
func (t *Table) SPS(id uint32) ([]byte, bool) {
	nal, ok := t.sps[id]
	return nal, ok
}

// PPS returns the PPS entry for id.
func (t *Table) PPS(id uint32) (PPS, bool) {
	p, ok := t.pps[id]
	return p, ok
}

// PutSPS parses the id of an SPS NAL and stores it verbatim, replacing any
// previous SPS with the same id.
func (t *Table) PutSPS(nal []byte) (uint32, error) {
	id, err := SPSID(nal)
	if err != nil {
		return 0, err
	}
	cp := make([]byte, len(nal))
	copy(cp, nal)
	t.sps[id] = cp
	return id, nil
}

// PutPPS parses the PPS id and the SPS id it references and stores the PPS
// verbatim, replacing any previous PPS with the same id.
func (t *Table) PutPPS(nal []byte) (id uint32, spsID uint32, err error) {
	id, spsID, err = PPSIDs(nal)
	if err != nil {
		return 0, 0, err
	}
	cp := make([]byte, len(nal))
	copy(cp, nal)
	t.pps[id] = PPS{NAL: cp, SPSID: spsID}
	return id, spsID, nil
}

// ResetAVCConfig replaces the whole table with the SPS/PPS contained in an
// avcC (AVCDecoderConfigurationRecord) payload. It returns the declared NAL
// length-field size in bytes (1, 2 or 4).
func (t *Table) ResetAVCConfig(config []byte) (int, error) {
	sps, pps, lengthSize, err := ParseAVCConfig(config)
	if err != nil {
		return 0, err
	}
	t.sps = map[uint32][]byte{}
	t.pps = map[uint32]PPS{}
	for _, nal := range sps {
		if _, err := t.PutSPS(nal); err != nil {
			return 0, fmt.Errorf("avcC SPS: %w", err)
		}
	}
	for _, nal := range pps {
		if _, _, err := t.PutPPS(nal); err != nil {
			return 0, fmt.Errorf("avcC PPS: %w", err)
		}
	}
	return lengthSize, nil
}

// ParseAVCConfig parses an AVCDecoderConfigurationRecord payload, returning
// its SPS NALs, PPS NALs and NAL length-field size in bytes.
func ParseAVCConfig(b []byte) (sps [][]byte, pps [][]byte, lengthSize int, err error) {
	// 0 configurationVersion, 1 AVCProfileIndication,
	// 2 profile_compatibility, 3 AVCLevelIndication,
	// 4 reserved(6) | lengthSizeMinusOne(2),
	// 5 reserved(3) | numOfSPS(5)
	if len(b) < 7 {
		return nil, nil, 0, fmt.Errorf("%w: avcC too short", ErrTruncated)
	}
	if b[0] != 1 {
		return nil, nil, 0, fmt.Errorf("avcstate: unsupported avcC configuration version %d", b[0])
	}
	lengthSize = int(b[4]&0x03) + 1
	switch lengthSize {
	case 1, 2, 4:
	default:
		return nil, nil, 0, fmt.Errorf("avcstate: unsupported NAL length size %d", lengthSize)
	}
	pos := 5
	numSPS := int(b[pos] & 0x1f)
	pos++
	sps = make([][]byte, 0, numSPS)
	for i := 0; i < numSPS; i++ {
		nal, n, perr := readConfigNAL(b, pos)
		if perr != nil {
			return nil, nil, 0, perr
		}
		sps = append(sps, nal)
		pos += n
	}
	if pos >= len(b) {
		return nil, nil, 0, fmt.Errorf("%w: avcC missing PPS count", ErrTruncated)
	}
	numPPS := int(b[pos])
	pos++
	pps = make([][]byte, 0, numPPS)
	for i := 0; i < numPPS; i++ {
		nal, n, perr := readConfigNAL(b, pos)
		if perr != nil {
			return nil, nil, 0, perr
		}
		pps = append(pps, nal)
		pos += n
	}
	return sps, pps, lengthSize, nil
}

func readConfigNAL(b []byte, pos int) (nal []byte, consumed int, err error) {
	if pos+2 > len(b) {
		return nil, 0, fmt.Errorf("%w: avcC parameter set length", ErrTruncated)
	}
	n := int(b[pos])<<8 | int(b[pos+1])
	if n <= 0 || pos+2+n > len(b) {
		return nil, 0, fmt.Errorf("%w: avcC parameter set bytes", ErrTruncated)
	}
	return b[pos+2 : pos+2+n], 2 + n, nil
}

// CheckSliceHeader parses the PPS id referenced by a slice NAL and verifies
// that the PPS and its referenced SPS are present in the table.
func (t *Table) CheckSliceHeader(nal []byte) (ppsID uint32, err error) {
	ppsID, err = SlicePPSID(nal)
	if err != nil {
		return 0, err
	}
	p, ok := t.pps[ppsID]
	if !ok {
		return ppsID, fmt.Errorf("%w: %d", ErrUnknownPPS, ppsID)
	}
	if _, ok := t.sps[p.SPSID]; !ok {
		return ppsID, fmt.Errorf("%w: %d (pps %d)", ErrUnknownSPS, p.SPSID, ppsID)
	}
	return ppsID, nil
}

// SortedSPS returns SPS ids in ascending order.
func (t *Table) SortedSPS() []uint32 {
	ids := make([]uint32, 0, len(t.sps))
	for id := range t.sps {
		ids = append(ids, id)
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	return ids
}

// SortedPPS returns PPS ids in ascending order.
func (t *Table) SortedPPS() []uint32 {
	ids := make([]uint32, 0, len(t.pps))
	for id := range t.pps {
		ids = append(ids, id)
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	return ids
}

// ValidateReferences ensures every stored PPS references a stored SPS.
func (t *Table) ValidateReferences() error {
	for id, p := range t.pps {
		if _, ok := t.sps[p.SPSID]; !ok {
			return fmt.Errorf("%w: %d (pps %d)", ErrUnknownSPS, p.SPSID, id)
		}
	}
	return nil
}

// PrefixBytes renders the Annex B parameter-set prefix for an edit start:
// all SPS in id order followed by all PPS in id order, each with a four byte
// start code. The original (escaped) NAL bytes are preserved.
func (t *Table) PrefixBytes() ([]byte, error) {
	if err := t.ValidateReferences(); err != nil {
		return nil, err
	}
	var out []byte
	for _, id := range t.SortedSPS() {
		out = append(out, 0, 0, 0, 1)
		out = append(out, t.sps[id]...)
	}
	for _, id := range t.SortedPPS() {
		out = append(out, 0, 0, 0, 1)
		out = append(out, t.pps[id].NAL...)
	}
	return out, nil
}

// RemoveEmulationPrevention converts EBSP to RBSP by dropping 0x03 bytes
// inserted after 00 00. It rejects malformed emulation-prevention sequences.
func RemoveEmulationPrevention(ebsp []byte) ([]byte, error) {
	out := make([]byte, 0, len(ebsp))
	zeros := 0
	for i := 0; i < len(ebsp); i++ {
		c := ebsp[i]
		if zeros >= 2 && c == 0x03 {
			if i+1 >= len(ebsp) || ebsp[i+1] > 0x03 {
				return nil, ErrIllegalEscape
			}
			zeros = 0
			continue // drop the emulation-prevention byte
		}
		out = append(out, c)
		if c == 0x00 {
			zeros++
		} else {
			zeros = 0
		}
	}
	return out, nil
}

// bitReader reads big-endian bits and unsigned Exp-Golomb values.
type bitReader struct {
	b   []byte
	pos int  // next byte index
	bit uint // 0..7, next bit position from MSB
}

func (r *bitReader) readBit() (uint, error) {
	if r.pos >= len(r.b) {
		return 0, ErrTruncated
	}
	v := (r.b[r.pos] >> (7 - r.bit)) & 1
	r.bit++
	if r.bit == 8 {
		r.bit = 0
		r.pos++
	}
	return uint(v), nil
}

// readUE reads one unsigned Exp-Golomb coded value.
func (r *bitReader) readUE() (uint64, error) {
	zeros := 0
	for {
		bit, err := r.readBit()
		if err != nil {
			return 0, fmt.Errorf("%w: ue leading bits", ErrTruncated)
		}
		if bit == 1 {
			break
		}
		zeros++
		if zeros > 63 {
			return 0, errors.New("avcstate: exp-golomb code too long")
		}
	}
	var v uint64 = 1
	for i := 0; i < zeros; i++ {
		bit, err := r.readBit()
		if err != nil {
			return 0, fmt.Errorf("%w: ue suffix", ErrTruncated)
		}
		v = v<<1 | uint64(bit)
	}
	return v - 1, nil
}

// ReadUE reads one unsigned Exp-Golomb value from the start of rbsp.
func ReadUE(rbsp []byte) (uint64, error) {
	r := &bitReader{b: rbsp}
	return r.readUE()
}

// rbspForID returns the RBSP of a NAL unit for header parsing.
func rbspForID(nal []byte) ([]byte, error) {
	if len(nal) < 1 {
		return nil, fmt.Errorf("%w: empty NAL", ErrTruncated)
	}
	return RemoveEmulationPrevention(nal)
}

// SPSID returns seq_parameter_set_id from an SPS NAL (type 7).
func SPSID(nal []byte) (uint32, error) {
	rbsp, err := rbspForID(nal)
	if err != nil {
		return 0, err
	}
	// [0] NAL header, [1] profile_idc, [2] constraints, [3] level_idc,
	// then seq_parameter_set_id as ue(v).
	if len(rbsp) < 5 {
		return 0, fmt.Errorf("%w: SPS fixed header", ErrTruncated)
	}
	id, err := ReadUE(rbsp[4:])
	if err != nil {
		return 0, err
	}
	if id > 0xffffffff {
		return 0, errors.New("avcstate: SPS id out of range")
	}
	return uint32(id), nil
}

// PPSIDs returns pic_parameter_set_id and seq_parameter_set_id from a PPS
// NAL (type 8).
func PPSIDs(nal []byte) (ppsID uint32, spsID uint32, err error) {
	rbsp, err := rbspForID(nal)
	if err != nil {
		return 0, 0, err
	}
	if len(rbsp) < 2 {
		return 0, 0, fmt.Errorf("%w: PPS header", ErrTruncated)
	}
	r := &bitReader{b: rbsp[1:]}
	id0, err := r.readUE()
	if err != nil {
		return 0, 0, err
	}
	id1, err := r.readUE()
	if err != nil {
		return 0, 0, err
	}
	return uint32(id0), uint32(id1), nil
}

// SlicePPSID returns pic_parameter_set_id from a slice NAL (type 1 or 5).
func SlicePPSID(nal []byte) (uint32, error) {
	rbsp, err := rbspForID(nal)
	if err != nil {
		return 0, err
	}
	if len(rbsp) < 2 {
		return 0, fmt.Errorf("%w: slice header", ErrTruncated)
	}
	r := &bitReader{b: rbsp[1:]}
	if _, err := r.readUE(); err != nil { // first_mb_in_slice
		return 0, err
	}
	if _, err := r.readUE(); err != nil { // slice_type
		return 0, err
	}
	ppsID, err := r.readUE()
	if err != nil {
		return 0, err
	}
	return uint32(ppsID), nil
}
