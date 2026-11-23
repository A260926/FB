// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package flexfec

import (
	"encoding/binary"
	"errors"
	"fmt"
	"sort"

	"github.com/pion/interceptor"
	"github.com/pion/rtp"
)

// Static errors for the repair receiver path.
var (
	errUnexpectedProtectedSSRC = errors.New("flexfec repair packet protects an unexpected SSRC")
	errEmptyFecMask            = errors.New("flexfec repair packet has an empty protection mask")
	errRecoveryTruncated       = errors.New("flexfec repair payload is shorter than the recovered length")
)

// seqNumberMod is the size of the RTP sequence number space.
const seqNumberMod = 1 << 16

// eqRow is one GF(2) equation over extended media sequence numbers.
// The right hand side is the FlexFEC bit string of the row: the 8 header
// recovery bytes (RTP bytes 0..7 with the length recovery field at 2..3)
// followed by the repair payload (RTP bytes 12.., zero extended).
type eqRow struct {
	id      uint64
	order   uint64
	attrSeq uint64
	cols    []int64
	head    [8]byte
	body    []byte
	attrs   interceptor.Attributes
}

type knownMedia struct {
	packet  *rtp.Packet
	raw     []byte
	attrs   interceptor.Attributes
	attrSeq uint64
}

type pendingFEC struct {
	packet  rtp.Packet
	attrs   interceptor.Attributes
	attrSeq uint64
}

// outPacket is a media packet waiting for delivery to the downstream reader.
type outPacket struct {
	esn   int64
	data  []byte
	attrs interceptor.Attributes
}

// repairDecoder keeps the incremental GF(2) elimination state for one remote
// stream. It reuses the FlexFEC-03 header parsing and the RTP reconstruction
// rules of the production encoder/decoder instead of maintaining a separate,
// example-only decoder.
type repairDecoder struct {
	mediaSSRC uint32
	window    int
	maxEqs    int

	baseSet bool
	floor   int64
	edge    int64
	refSeq  uint16

	inputSeq uint64
	rowOrder uint64

	known     map[int64]*knownMedia
	delivered map[int64]struct{}
	held      map[int64]*outPacket
	queue     []*outPacket

	// accepted retains the original, unmutated equations in acceptance order.
	accepted []*eqRow
	// pivots is the reduced row echelon form keyed by each row's first column.
	pivots  map[int64]*eqRow
	seenFEC map[uint64]struct{}

	pending []pendingFEC
}

func newRepairDecoder(mediaSSRC uint32, window, maxEquations int) *repairDecoder {
	return &repairDecoder{
		mediaSSRC: mediaSSRC,
		window:    window,
		maxEqs:    maxEquations,
		known:     make(map[int64]*knownMedia),
		delivered: make(map[int64]struct{}),
		held:      make(map[int64]*outPacket),
		pivots:    make(map[int64]*eqRow),
		seenFEC:   make(map[uint64]struct{}),
	}
}

// extendSeq maps a wrapped media sequence number onto the monotonic extended
// sequence number space. All live activity spans less than half the space.
func (d *repairDecoder) extendSeq(seq uint16) int64 {
	if !d.baseSet {
		return int64(seq)
	}

	delta := int32(seq) - int32(d.refSeq)
	if delta > seqNumberMod/2-1 {
		delta -= seqNumberMod
	} else if delta < -seqNumberMod/2 {
		delta += seqNumberMod
	}

	return d.edge + int64(delta)
}

func fecKey(ssrc uint32, seq uint16) uint64 {
	return (uint64(ssrc) << 16) | uint64(seq)
}

// ingestMedia incorporates an original media packet. Deliverable media
// (including any recovery triggered by it) is appended to the ordered output
// queue; old packets and late duplicates produce nothing.
func (d *repairDecoder) ingestMedia(packet *rtp.Packet, raw []byte, attrs interceptor.Attributes) {
	d.inputSeq++
	seq := packet.SequenceNumber

	if !d.baseSet {
		d.baseSet = true
		d.edge = int64(seq)
		d.refSeq = seq
		d.drainPending()
	}

	esn := d.extendSeq(seq)
	if esn < d.floor {
		// Older than the current window: it must not resurrect evicted state,
		// including late duplicates of already delivered packets.
		return
	}

	deliver := func() {
		// Enqueue through the common path so recovered older packets are
		// delivered before this original in extended sequence order.
		d.enqueue(&outPacket{esn: esn, data: raw, attrs: attrs})
	}

	if _, ok := d.known[esn]; ok {
		// Equations staged before the window may already have recovered this
		// sequence number. The original packet supersedes a recovered copy
		// that has not been delivered yet; otherwise it is a late duplicate.
		if _, held := d.held[esn]; held {
			delete(d.held, esn)
			d.known[esn] = &knownMedia{packet: packet, raw: raw, attrs: attrs, attrSeq: d.inputSeq}
			deliver()

			return
		}

		if d.removeQueued(esn) {
			d.known[esn] = &knownMedia{packet: packet, raw: raw, attrs: attrs, attrSeq: d.inputSeq}
			deliver()
		}

		return
	}

	d.known[esn] = &knownMedia{
		packet:  packet,
		raw:     raw,
		attrs:   attrs,
		attrSeq: d.inputSeq,
	}
	d.delivered[esn] = struct{}{}

	if esn > d.edge {
		d.edge = esn
		d.refSeq = seq
	}

	d.applyKnownToSystem(esn, d.known[esn])
	d.solveLoop()
	d.slideWindow()
	d.releaseHeld()
	deliver()
}

// ingestFEC incorporates a repair packet. Repair packets never reach the
// downstream reader directly.
func (d *repairDecoder) ingestFEC(packet *rtp.Packet, attrs interceptor.Attributes) error {
	d.inputSeq++

	// Structure and protected SSRC are validated even before the window base
	// exists, so malformed datagrams fail the Read that carried them instead
	// of failing later, partially committed.
	fec, err := parseFlexFEC03Header(packet.Payload)
	if err != nil {
		return fmt.Errorf("parse flexfec-03 header: %w", err)
	}

	if fec.protectedSSRC != d.mediaSSRC {
		return fmt.Errorf("%w: flexfec protects SSRC %d, expected %d", errUnexpectedProtectedSSRC, fec.protectedSSRC, d.mediaSSRC)
	}

	if !d.baseSet {
		// Bound the pre-window staging area by maxEquations.
		if len(d.pending) >= d.maxEqs {
			d.pending = d.pending[1:]
		}

		d.pending = append(d.pending, pendingFEC{
			packet:  rtp.Packet{Header: packet.Header, Payload: append([]byte(nil), packet.Payload...)},
			attrs:   attrs,
			attrSeq: d.inputSeq,
		})

		return nil
	}

	key := fecKey(packet.SSRC, packet.SequenceNumber)
	if _, ok := d.seenFEC[key]; ok {
		// A repeated legal equation must not produce repeated output.
		return nil
	}

	row, err := d.buildRow(packet, fec, attrs, d.inputSeq)
	if err != nil {
		return err
	}

	d.seenFEC[key] = struct{}{}
	d.rowOrder++
	row.order = d.rowOrder

	if len(d.accepted) >= d.maxEqs {
		// Evict the oldest accepted independent equation, rebuild the
		// elimination state without it, then trial the new equation. Already
		// delivered frontier packets are unaffected.
		d.accepted = d.accepted[1:]
		d.pivots = make(map[int64]*eqRow)

		for _, orig := range d.accepted {
			d.insertWorkingRow(d.workingCopy(orig))
		}

		working := d.workingCopy(row)
		d.insertWorkingRow(working)

		if len(working.cols) > 0 {
			d.accepted = append(d.accepted, row)
		}
	} else {
		working := d.workingCopy(row)
		d.insertWorkingRow(working)

		if len(working.cols) > 0 {
			d.accepted = append(d.accepted, row)
		}
	}

	d.pruneSeen()
	d.solveLoop()
	d.slideWindow()
	d.releaseHeld()

	return nil
}

func (d *repairDecoder) drainPending() {
	pending := d.pending
	d.pending = nil

	for _, p := range pending {
		// Re-validation is cheap and keeps the staged path identical to the
		// live one; errors here cannot commit partial state.
		if err := d.ingestFEC(&p.packet, p.attrs); err != nil {
			continue
		}
	}
}

func (d *repairDecoder) buildRow(packet *rtp.Packet, fec flexFec, attrs interceptor.Attributes, attrSeq uint64) (*eqRow, error) {
	seqs := decodeMask(uint64(fec.mask0), 15, fec.seqNumBase)
	if fec.mask1 != 0 {
		seqs = append(seqs, decodeMask(uint64(fec.mask1), 31, fec.seqNumBase+15)...)
	}

	if fec.mask2 != 0 {
		seqs = append(seqs, decodeMask(fec.mask2, 63, fec.seqNumBase+46)...)
	}

	if len(seqs) == 0 {
		return nil, errEmptyFecMask
	}

	cols := make([]int64, 0, len(seqs))
	for _, seq := range seqs {
		cols = append(cols, d.extendSeq(seq))
	}

	row := &eqRow{
		id:      fecKey(packet.SSRC, packet.SequenceNumber),
		attrSeq: attrSeq,
		cols:    uniqueSorted(cols),
		body:    append([]byte(nil), fec.payload...),
		attrs:   attrs,
	}
	copy(row.head[:], packet.Payload[:8])

	return row, nil
}

func (d *repairDecoder) workingCopy(orig *eqRow) *eqRow {
	row := &eqRow{
		id:      orig.id,
		order:   orig.order,
		attrSeq: orig.attrSeq,
		cols:    append([]int64(nil), orig.cols...),
		body:    append([]byte(nil), orig.body...),
		attrs:   orig.attrs,
	}
	copy(row.head[:], orig.head[:])

	return row
}

// insertWorkingRow reduces a row against the current pivot set, discards it
// when dependent, or installs it as a new pivot while keeping reduced row
// echelon form.
func (d *repairDecoder) insertWorkingRow(row *eqRow) {
	d.substituteKnowns(row)

	for len(row.cols) > 0 {
		pivot, ok := d.pivots[row.cols[0]]
		if !ok {
			break
		}

		d.xorRow(row, pivot)
	}

	if len(row.cols) == 0 {
		// Duplicate or linearly dependent legal equation.
		return
	}

	pivotCol := row.cols[0]
	d.pivots[pivotCol] = row

	// Clear the new pivot column from every other pivot row.
	for col, other := range d.pivots {
		if col == pivotCol {
			continue
		}

		if sortedContains(other.cols, pivotCol) {
			d.xorRow(other, row)
		}
	}
}

func (d *repairDecoder) substituteKnowns(row *eqRow) {
	next := row.cols[:0]

	for _, col := range row.cols {
		if km, ok := d.known[col]; ok {
			d.applyKnown(row, km)
		} else {
			next = append(next, col)
		}
	}

	row.cols = uniqueSorted(next)
}

// applyKnownToSystem feeds one newly known media packet into every equation
// that references it, then re-establishes the pivot structure.
func (d *repairDecoder) applyKnownToSystem(esn int64, km *knownMedia) {
	affected := make([]*eqRow, 0)

	for col, row := range d.pivots {
		if sortedContains(row.cols, esn) {
			affected = append(affected, row)
			delete(d.pivots, col)
		}
	}

	sort.Slice(affected, func(i, j int) bool {
		return affected[i].order < affected[j].order
	})

	for _, row := range affected {
		d.applyKnown(row, km)
		row.cols = removeSorted(row.cols, esn)
		d.insertWorkingRow(row)
	}
}

// rebuild restores the RREF state from the retained original equations and
// the currently known media packets.
func (d *repairDecoder) rebuild() {
	d.pivots = make(map[int64]*eqRow)

	for _, orig := range d.accepted {
		d.insertWorkingRow(d.workingCopy(orig))
	}

	d.solveLoop()
}

// purgeStaleEquations drops every equation that still references an unknown
// column below the window floor. Removing such rows entirely is mandatory:
// deleting only the stale column would let the remaining terms be solved into
// fabricated packets.
func (d *repairDecoder) purgeStaleEquations() {
	staleIDs := map[uint64]struct{}{}

	for col, row := range d.pivots {
		for _, unknown := range row.cols {
			if _, known := d.known[unknown]; !known && unknown < d.floor {
				staleIDs[row.id] = struct{}{}
				delete(d.pivots, col)

				break
			}
		}
	}

	if len(staleIDs) == 0 {
		return
	}

	kept := d.accepted[:0]

	for _, orig := range d.accepted {
		if _, stale := staleIDs[orig.id]; !stale {
			kept = append(kept, orig)
		}
	}

	d.accepted = kept
}

// solveLoop repeatedly solves rows reduced to a single unknown and feeds the
// recovered packets back into the remaining equations.
func (d *repairDecoder) solveLoop() {
	for {
		// Unknown columns below the floor invalidate whole equations before
		// any elimination: dropping only the column would let the remaining
		// terms produce fake packets.
		d.purgeStaleEquations()

		var pivotCol int64

		var pivot *eqRow

		for col, row := range d.pivots {
			if len(row.cols) == 1 {
				pivotCol = col
				pivot = row

				break
			}
		}

		if pivot == nil {
			return
		}

		delete(d.pivots, pivotCol)
		d.removeAccepted(pivot.id)

		raw, packet, err := d.rebuildPacket(pivot, pivotCol)
		if err != nil {
			// A malformed equation can never yield a unique packet: drop it
			// instead of guessing.
			continue
		}

		attrs := recoveredAttributes(pivot.attrs)
		km := &knownMedia{
			packet:  packet,
			raw:     raw,
			attrs:   attrs,
			attrSeq: pivot.attrSeq,
		}
		d.known[pivotCol] = km
		d.applyKnownToSystem(pivotCol, km)

		if _, ok := d.delivered[pivotCol]; ok {
			continue
		}

		d.delivered[pivotCol] = struct{}{}
		out := &outPacket{esn: pivotCol, data: raw, attrs: attrs}

		if pivotCol <= d.edge {
			d.enqueue(out)
		} else {
			// Equations may protect sequence numbers ahead of the media
			// frontier: hold the solution until original media covers them.
			d.held[pivotCol] = out
		}
	}
}

// rebuildPacket reconstructs the full RTP packet (header, CSRC, extension,
// payload and padding) from a singleton equation row.
func (d *repairDecoder) rebuildPacket(row *eqRow, esn int64) ([]byte, *rtp.Packet, error) {
	payloadLength := int(binary.BigEndian.Uint16(row.head[2:4]))
	if payloadLength > len(row.body) {
		return nil, nil, fmt.Errorf("%w: declared %d bytes, repair body %d", errRecoveryTruncated, payloadLength, len(row.body))
	}

	headerRecovery := make([]byte, 12)
	copy(headerRecovery[:8], row.head[:])
	// Force RTP version 2 and clear the R/F bits of the recovery header.
	headerRecovery[0] |= 0x80
	headerRecovery[0] &= 0xbf
	binary.BigEndian.PutUint16(headerRecovery[2:4], uint16(esn)) //nolint:gosec
	binary.BigEndian.PutUint32(headerRecovery[8:12], d.mediaSSRC)

	full := append(headerRecovery, row.body[:payloadLength]...)

	var packet rtp.Packet
	if err := packet.Unmarshal(full); err != nil {
		return nil, nil, fmt.Errorf("unmarshal recovered packet: %w", err)
	}

	raw, err := packet.Marshal()
	if err != nil {
		return nil, nil, fmt.Errorf("marshal recovered packet: %w", err)
	}

	return raw, &packet, nil
}

func (d *repairDecoder) slideWindow() {
	// The window covers the latest window extended sequence numbers up to the
	// media frontier. The frontier itself never moves backwards and is only
	// advanced by original media packets; equations staged before the first
	// media may legitimately protect columns just before the window anchor.
	lower := d.edge - int64(d.window) + 1

	d.floor = lower

	// Known packets sliding out are substituted into the retained equations
	// before their storage is released.
	changed := false

	for esn, km := range d.known {
		if esn >= lower {
			continue
		}

		for _, orig := range d.accepted {
			if sortedContains(orig.cols, esn) {
				d.applyKnown(orig, km)
				orig.cols = removeSorted(orig.cols, esn)
				changed = true
			}
		}
	}

	if changed {
		d.rebuild()
	}

	if len(d.queue) > 0 {
		keptQueue := d.queue[:0]

		for _, out := range d.queue {
			if out.esn >= lower {
				keptQueue = append(keptQueue, out)
			}
		}

		d.queue = keptQueue
	}

	for esn := range d.known {
		if esn < lower {
			delete(d.known, esn)
		}
	}

	for esn := range d.delivered {
		if esn < lower {
			delete(d.delivered, esn)
		}
	}
}

func (d *repairDecoder) releaseHeld() {
	for esn, out := range d.held {
		if esn < d.floor {
			// The frontier jumped over this held solution: it is stale now.
			delete(d.held, esn)

			continue
		}

		if esn <= d.edge {
			d.enqueue(out)
			delete(d.held, esn)
		}
	}
}

func (d *repairDecoder) enqueue(out *outPacket) {
	d.queue = append(d.queue, out)
	sort.SliceStable(d.queue, func(i, j int) bool {
		return d.queue[i].esn < d.queue[j].esn
	})
}

// removeQueued drops the queued packet for esn and reports whether one existed.
func (d *repairDecoder) removeQueued(esn int64) bool {
	for i, out := range d.queue {
		if out.esn == esn {
			d.queue = append(d.queue[:i], d.queue[i+1:]...)

			return true
		}
	}

	return false
}

func (d *repairDecoder) removeAccepted(id uint64) {
	kept := d.accepted[:0]

	for _, orig := range d.accepted {
		if orig.id != id {
			kept = append(kept, orig)
		}
	}

	d.accepted = kept
}

// pruneSeen bounds the duplicate-detection set using the equations that are
// still retained.
func (d *repairDecoder) pruneSeen() {
	limit := d.maxEqs*2 + d.window + 64
	if len(d.seenFEC) <= limit {
		return
	}

	live := make(map[uint64]struct{}, len(d.accepted)+len(d.pending))
	for _, orig := range d.accepted {
		live[orig.id] = struct{}{}
	}

	for _, p := range d.pending {
		live[fecKey(p.packet.SSRC, p.packet.SequenceNumber)] = struct{}{}
	}

	d.seenFEC = live
}

// applyKnown XORs the FlexFEC bit string of one known media packet into a row.
func (d *repairDecoder) applyKnown(row *eqRow, km *knownMedia) {
	raw := km.raw
	lengthRecovery := uint16(len(raw) - BaseRTPHeaderSize) //nolint:gosec

	var head [8]byte
	copy(head[:], raw[:8])
	binary.BigEndian.PutUint16(head[2:4], lengthRecovery)

	for i := range 8 {
		row.head[i] ^= head[i]
	}

	body := raw[BaseRTPHeaderSize:]
	if len(row.body) < len(body) {
		grown := make([]byte, len(body))
		copy(grown, row.body)
		row.body = grown
	}

	for i, b := range body {
		row.body[i] ^= b
	}

	// A recovered packet inherits the attributes of the last input that
	// contributed to solving its equation.
	if km.attrSeq > row.attrSeq {
		row.attrSeq = km.attrSeq
		row.attrs = km.attrs
	}
}

func (d *repairDecoder) xorRow(dst, src *eqRow) {
	for i := range 8 {
		dst.head[i] ^= src.head[i]
	}

	if len(dst.body) < len(src.body) {
		grown := make([]byte, len(src.body))
		copy(grown, dst.body)
		dst.body = grown
	}

	for i, b := range src.body {
		dst.body[i] ^= b
	}

	dst.cols = symmetricDifference(dst.cols, src.cols)

	if src.attrSeq > dst.attrSeq {
		dst.attrSeq = src.attrSeq
		dst.attrs = src.attrs
	}
}

// AttributeKeyRecovered is set to true on the attributes of packets that were
// reconstructed from FlexFEC repair packets.
const AttributeKeyRecovered = "flexfec.recovered"

// recoveredAttributes snapshots the contributing input's attributes without
// carrying over cached RTP headers, which describe input bytes rather than the
// freshly recovered ones.
func recoveredAttributes(in interceptor.Attributes) interceptor.Attributes {
	out := interceptor.Attributes{}

	for key, value := range in {
		if _, isHeader := value.(*rtp.Header); isHeader {
			continue
		}

		out[key] = value
	}

	out[AttributeKeyRecovered] = true

	return out
}

func sortedContains(cols []int64, col int64) bool {
	i := sort.Search(len(cols), func(i int) bool { return cols[i] >= col })

	return i < len(cols) && cols[i] == col
}

func removeSorted(cols []int64, col int64) []int64 {
	i := sort.Search(len(cols), func(i int) bool { return cols[i] >= col })
	if i == len(cols) || cols[i] != col {
		return cols
	}

	return append(cols[:i], cols[i+1:]...)
}

func uniqueSorted(cols []int64) []int64 {
	sort.Slice(cols, func(i, j int) bool { return cols[i] < cols[j] })

	if len(cols) <= 1 {
		return cols
	}

	out := cols[:1]

	for _, col := range cols[1:] {
		if col != out[len(out)-1] {
			out = append(out, col)
		}
	}

	return out
}

// symmetricDifference returns the XOR of two sorted column sets, which is the
// GF(2) coefficient vector after adding two rows.
func symmetricDifference(a, b []int64) []int64 {
	out := make([]int64, 0, len(a)+len(b))
	i, j := 0, 0

	for i < len(a) && j < len(b) {
		switch {
		case a[i] < b[j]:
			out = append(out, a[i])
			i++
		case a[i] > b[j]:
			out = append(out, b[j])
			j++
		default:
			i++
			j++
		}
	}

	out = append(out, a[i:]...)
	out = append(out, b[j:]...)

	return out
}
