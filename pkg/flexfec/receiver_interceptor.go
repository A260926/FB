// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package flexfec

import (
	"errors"
	"fmt"
	"io"
	"sync"

	"github.com/pion/interceptor"
	"github.com/pion/logging"
	"github.com/pion/rtp"
)

// AttributeRecovered is the Attributes key set to true on RTP packets that
// were reconstructed from FlexFEC repair packets rather than received as raw
// media.
const AttributeRecovered = "flexfec.recovered"

var (
	_ interceptor.Interceptor = (*RepairReceiverInterceptor)(nil)
	_ interceptor.Factory     = (*RepairReceiverInterceptor)(nil)
)

// Capacity bounds accepted by NewRepairReceiver.
const (
	minRepairCapacity = 2
	maxRepairCapacity = 512
)

// ErrRepairPending is returned by RepairReceiverInterceptor.Read when the
// upstream datagram(s) consumed so far did not yield a media packet to deliver
// (a repair packet was ingested, a duplicate was suppressed or recovery is not
// rank-complete yet). Callers should call Read again.
var ErrRepairPending = errors.New("flexfec: repair pending, no media packet ready")

// ErrSSRCConflict is returned from the reader produced by
// BindRemoteStream when the media/FEC SSRC pair conflicts with an existing
// binding. The reader only returns this error and never touches its upstream.
var ErrSSRCConflict = errors.New("flexfec: media/fec ssrc identity conflicts with an existing binding")

var (
	errRepairCapacity = errors.New("flexfec: window and maxEquations must be between 2 and 512")
	errPTMismatch     = errors.New("flexfec: rtp payload type does not match StreamInfo identity")
	errSSRCIdentity   = errors.New("flexfec: rtp ssrc matches neither media nor repair stream")
	errProtectedSSRC  = errors.New("flexfec: repair packet protects an unexpected media ssrc")
)

// RepairReceiverInterceptor is a public FlexFEC-03 receive entry point.
//
// Bind a remote stream with the media StreamInfo.SSRC/PayloadType and the
// repair StreamInfo.SSRCForwardErrorCorrection/PayloadTypeForwardErrorCorrection.
// The returned RTPReader demultiplexes the interleaved media and repair
// datagrams arriving on the same upstream reader, recovers missing media with
// incremental GF(2) elimination and exposes only original/recovered media RTP
// packets. It is intended to be placed before the jitter buffer in the receive
// chain.
type RepairReceiverInterceptor struct {
	interceptor.NoOp

	window     uint16
	maxEq      uint16
	logger     logging.LoggerFactory
	log        logging.LeveledLogger
	mu         sync.Mutex
	closed     bool
	owners     map[uint32]*repairStream
	mediaIndex map[uint32]*repairStream
}

// NewRepairReceiver constructs a RepairReceiverInterceptor. window is the
// number of newest expanded media sequence numbers retained and maxEquations
// bounds the accepted independent repair equations. Both capacities must be in
// the inclusive range 2..512.
func NewRepairReceiver(window uint16, maxEquations uint16) (*RepairReceiverInterceptor, error) {
	if window < minRepairCapacity || window > maxRepairCapacity ||
		maxEquations < minRepairCapacity || maxEquations > maxRepairCapacity {
		return nil, fmt.Errorf("%w: window=%d maxEquations=%d", errRepairCapacity, window, maxEquations)
	}

	lf := logging.NewDefaultLoggerFactory()

	return &RepairReceiverInterceptor{
		window:     window,
		maxEq:      maxEquations,
		logger:     lf,
		log:        lf.NewLogger("flexfec_repair_receiver"),
		owners:     make(map[uint32]*repairStream),
		mediaIndex: make(map[uint32]*repairStream),
	}, nil
}

// NewInterceptor allows the receiver to be registered as an
// interceptor.Factory. The receiver itself is already a fully constructed
// interceptor.Interceptor and is returned for every id.
func (r *RepairReceiverInterceptor) NewInterceptor(_ string) (interceptor.Interceptor, error) {
	return r, nil
}

// BindRemoteStream binds one media/repair identity pair. Streams without a
// complete FEC identity are passed through unchanged. Conflicting SSRC
// identities return a reader that only errors without reading upstream or
// altering the existing binding.
func (r *RepairReceiverInterceptor) BindRemoteStream(
	info *interceptor.StreamInfo, upstream interceptor.RTPReader,
) interceptor.RTPReader {
	if !fecIdentityComplete(info) {
		return upstream
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	if r.closed {
		return staticErrorReader{err: io.ErrClosedPipe}
	}

	mediaSSRC := info.SSRC
	repairSSRC := info.SSRCForwardErrorCorrection

	om := r.owners[mediaSSRC]
	of := r.owners[repairSSRC]

	// An exact re-bind of the same identity pair starts a fresh generation.
	exactRebind := mediaSSRC != repairSSRC &&
		om != nil && om == of &&
		om.mediaSSRC == mediaSSRC && om.repairSSRC == repairSSRC

	if !exactRebind {
		// Any overlap of either SSRC with a different active binding, or using
		// the same value for both identities, is a conflict.
		if mediaSSRC == repairSSRC || om != nil || of != nil {
			return staticErrorReader{err: fmt.Errorf("%w: media=%d repair=%d", ErrSSRCConflict, mediaSSRC, repairSSRC)}
		}
	} else {
		r.removeLocked(om)
		om.kill()
	}

	st := newRepairStream(r, info, upstream)
	r.owners[mediaSSRC] = st
	r.owners[repairSSRC] = st
	r.mediaIndex[mediaSSRC] = st

	return repairReader{stream: st}
}

// UnbindRemoteStream drops the stream and isolates its binding generation.
// Readers created for it afterwards return io.ErrClosedPipe without reading
// upstream.
func (r *RepairReceiverInterceptor) UnbindRemoteStream(info *interceptor.StreamInfo) {
	if info == nil {
		return
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	st := r.mediaIndex[info.SSRC]
	if st == nil {
		st = r.owners[info.SSRCForwardErrorCorrection]
	}
	if st == nil {
		return
	}

	r.removeLocked(st)
	st.kill()
}

// Close clears every binding generation.
func (r *RepairReceiverInterceptor) Close() error {
	r.mu.Lock()
	defer r.mu.Unlock()

	r.closed = true
	for _, st := range r.mediaIndex {
		st.kill()
	}
	r.owners = make(map[uint32]*repairStream)
	r.mediaIndex = make(map[uint32]*repairStream)

	return nil
}

func (r *RepairReceiverInterceptor) removeLocked(st *repairStream) {
	if cur := r.owners[st.mediaSSRC]; cur == st {
		delete(r.owners, st.mediaSSRC)
	}
	if cur := r.owners[st.repairSSRC]; cur == st {
		delete(r.owners, st.repairSSRC)
	}
	if cur := r.mediaIndex[st.mediaSSRC]; cur == st {
		delete(r.mediaIndex, st.mediaSSRC)
	}
}

func fecIdentityComplete(info *interceptor.StreamInfo) bool {
	return info != nil &&
		info.SSRC != 0 &&
		info.SSRCForwardErrorCorrection != 0 &&
		info.PayloadTypeForwardErrorCorrection != 0
}

// staticErrorReader is the conflict/closed reader: it never holds or calls an
// upstream reader.
type staticErrorReader struct {
	err error
}

func (e staticErrorReader) Read([]byte, interceptor.Attributes) (int, interceptor.Attributes, error) {
	return 0, nil, e.err
}

type repairReader struct {
	stream *repairStream
}

func (rd repairReader) Read(b []byte, a interceptor.Attributes) (int, interceptor.Attributes, error) {
	return rd.stream.read(b, a)
}

type pendingPacket struct {
	ext   int64
	raw   []byte
	attrs interceptor.Attributes
}

type stashedRepair struct {
	raw   []byte
	attrs interceptor.Attributes
}

// repairStream is one binding generation.
type repairStream struct {
	ic       *RepairReceiverInterceptor
	upstream interceptor.RTPReader

	mediaSSRC  uint32
	repairSSRC uint32
	mediaPT    uint8
	repairPT   uint8
	window     int

	mu   sync.Mutex
	dead bool

	// dec is the existing FlexFEC decoder type; rank is its GF(2) path.
	dec  *fecDecoder
	rank *gfRankTable

	hasBase  bool
	baseExt  int64
	rightExt int64

	delivered map[int64]struct{}
	queued    map[int64]struct{}
	ready     []pendingPacket
	future    map[int64]pendingPacket
	preBase   []stashedRepair
}

func newRepairStream(
	ic *RepairReceiverInterceptor, info *interceptor.StreamInfo, upstream interceptor.RTPReader,
) *repairStream {
	dec := newFECDecoder(
		info.SSRCForwardErrorCorrection,
		info.SSRC,
		ic.logger,
	)
	dec.enableRank(int(ic.maxEq))

	return &repairStream{
		ic:         ic,
		upstream:   upstream,
		mediaSSRC:  info.SSRC,
		repairSSRC: info.SSRCForwardErrorCorrection,
		mediaPT:    info.PayloadType,
		repairPT:   info.PayloadTypeForwardErrorCorrection,
		window:     int(ic.window),
		dec:        dec,
		rank:       dec.rank,
		delivered:  make(map[int64]struct{}),
		queued:     make(map[int64]struct{}),
		future:     make(map[int64]pendingPacket),
	}
}

func (s *repairStream) kill() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.dead = true
	s.ready = nil
	s.future = make(map[int64]pendingPacket)
	s.queued = make(map[int64]struct{})
	s.preBase = nil
}

func (s *repairStream) read(b []byte, attrs interceptor.Attributes) (int, interceptor.Attributes, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.dead {
		return 0, nil, io.ErrClosedPipe
	}

	// 1. Already recovered packets are delivered first, without reading upstream.
	if len(s.ready) > 0 {
		pkt := s.ready[0]
		if len(b) < len(pkt.raw) {
			// Keep the packet queued rather than losing ready data.
			return 0, nil, io.ErrShortBuffer
		}
		s.ready = s.ready[1:]
		delete(s.queued, pkt.ext)
		n := copy(b, pkt.raw)

		return n, pkt.attrs, nil
	}

	// 2. No pending media: read upstream at most once.
	n, upAttrs, err := s.upstream.Read(b, attrs)
	if err != nil {
		// Upstream errors are returned verbatim; queued state is untouched.
		return n, upAttrs, err
	}
	if n <= 0 {
		return 0, nil, ErrRepairPending
	}

	// Independent snapshots: the caller reuses b and the Attributes map.
	raw := append([]byte(nil), b[:n]...)
	snap := snapshotAttributes(firstAttributes(upAttrs, attrs))

	mediaNow, err := s.ingest(raw, snap)
	if err != nil {
		// Invalid identity/PT/structure: nothing was committed.
		return 0, nil, err
	}
	if mediaNow {
		// b still holds the original media datagram that upstream wrote.
		return n, snap, nil
	}

	return 0, nil, ErrRepairPending
}

func firstAttributes(a, fallback interceptor.Attributes) interceptor.Attributes {
	if a != nil {
		return a
	}

	return fallback
}

// snapshotAttributes returns an independent map copy.
func snapshotAttributes(src interceptor.Attributes) interceptor.Attributes {
	if src == nil {
		return nil
	}
	dst := make(interceptor.Attributes, len(src))
	for k, v := range src {
		dst[k] = v
	}

	return dst
}

// recoveredAttributes copies the solving input's snapshot, drops any stale
// cached *rtp.Header (it must not be reused against the new recovered bytes)
// and marks the packet as recovered.
func recoveredAttributes(src interceptor.Attributes) interceptor.Attributes {
	dst := make(interceptor.Attributes)
	for k, v := range src {
		if _, ok := v.(*rtp.Header); ok {
			continue
		}
		dst[k] = v
	}
	dst[AttributeRecovered] = true

	return dst
}

// ingest parses and commits one snapshotted datagram. It returns mediaNow=true
// when the call must deliver the original media bytes immediately.
func (s *repairStream) ingest(raw []byte, snap interceptor.Attributes) (bool, error) {
	var pkt rtp.Packet
	if err := pkt.Unmarshal(raw); err != nil {
		return false, err
	}

	switch pkt.Header.SSRC {
	case s.mediaSSRC:
		if pkt.Header.PayloadType != s.mediaPT {
			return false, fmt.Errorf("%w: media expected PT %d got %d", errPTMismatch, s.mediaPT, pkt.Header.PayloadType)
		}

		return s.handleMedia(raw, &pkt, snap), nil
	case s.repairSSRC:
		if pkt.Header.PayloadType != s.repairPT {
			return false, fmt.Errorf("%w: repair expected PT %d got %d", errPTMismatch, s.repairPT, pkt.Header.PayloadType)
		}
		if err := s.handleRepair(raw, &pkt, snap); err != nil {
			return false, err
		}

		return false, nil
	default:
		return false, fmt.Errorf("%w: got %d", errSSRCIdentity, pkt.Header.SSRC)
	}
}

func (s *repairStream) handleMedia(raw []byte, pkt *rtp.Packet, snap interceptor.Attributes) bool {
	seq := pkt.Header.SequenceNumber

	if !s.hasBase {
		s.hasBase = true
		s.baseExt = int64(seq)
		s.rightExt = int64(seq)
		ext := s.baseExt

		s.delivered[ext] = struct{}{}
		delete(s.future, ext)
		delete(s.queued, ext)
		s.rank.addKnown(ext, raw)

		stashed := s.preBase
		s.preBase = nil
		for _, st := range stashed {
			s.ingestStashedRepair(st)
		}
		s.promoteFuture()

		return true
	}

	ext := extendSequence(s.rightExt, seq)
	cutoff := s.rightExt - int64(s.window)

	// Inputs earlier than the first media packet or already slid out must not
	// resurrect state and are not delivered twice.
	if ext < s.baseExt || ext <= cutoff {
		return false
	}

	if _, dup := s.delivered[ext]; dup {
		return false
	}

	if ext > s.rightExt {
		s.rightExt = ext
		s.slideWindow()
	}

	// An original supersedes any held recovered copy of the same number.
	delete(s.future, ext)
	delete(s.queued, ext)

	s.delivered[ext] = struct{}{}
	recovered := s.rank.addKnown(ext, raw)
	s.enqueueRecovered(recovered, snap)
	s.promoteFuture()

	return true
}

func (s *repairStream) slideWindow() {
	cutoff := s.rightExt - int64(s.window)
	s.rank.slideWindow(cutoff)
	for ext := range s.delivered {
		if ext <= cutoff {
			delete(s.delivered, ext)
		}
	}
}

func (s *repairStream) handleRepair(raw []byte, pkt *rtp.Packet, snap interceptor.Attributes) error {
	fec, err := parseFlexFEC03Header(pkt.Payload)
	if err != nil {
		// Unsupported R/F/SSRC count or truncation: reject before commit.
		return err
	}
	if fec.protectedSSRC != s.mediaSSRC {
		return fmt.Errorf("%w: expected %d got %d", errProtectedSSRC, s.mediaSSRC, fec.protectedSSRC)
	}

	if !s.hasBase {
		if len(s.preBase) >= int(s.ic.maxEq) {
			s.preBase = s.preBase[1:]
		}
		s.preBase = append(s.preBase, stashedRepair{raw: raw, attrs: snap})

		return nil
	}

	return s.commitRepair(pkt, &fec, snap)
}

// ingestStashedRepair replays one pre-base repair snapshot using its own
// attribute snapshot for attribution.
func (s *repairStream) ingestStashedRepair(st stashedRepair) {
	var pkt rtp.Packet
	if err := pkt.Unmarshal(st.raw); err != nil {
		return
	}
	fec, err := parseFlexFEC03Header(pkt.Payload)
	if err != nil || fec.protectedSSRC != s.mediaSSRC {
		return
	}
	if err := s.commitRepair(&pkt, &fec, st.attrs); err != nil {
		s.ic.log.Debugf("stashed flexfec repair discarded: %v", err)
	}
}

func (s *repairStream) commitRepair(pkt *rtp.Packet, fec *flexFec, snap interceptor.Attributes) error {
	cols := s.protectedExtSeqs(fec)
	if len(cols) == 0 {
		return errPacketTruncated
	}

	cutoff := s.rightExt - int64(s.window)
	for _, ext := range cols {
		if ext < s.baseExt || ext <= cutoff {
			if !s.rank.hasKnown(ext) {
				// Whole equation references an unavailable out-of-window
				// unknown column; discard it rather than partially commit.
				return nil
			}
		}
	}

	head := pkt.Payload[:8]
	_, recovered := s.rank.addEquation(cols, head, fec.payload)
	s.enqueueRecovered(recovered, snap)
	s.promoteFuture()

	return nil
}

func (s *repairStream) protectedExtSeqs(fec *flexFec) []int64 {
	protectedSeqs := decodeMask(uint64(fec.mask0), 15, fec.seqNumBase)
	if fec.mask1 != 0 {
		protectedSeqs = append(protectedSeqs, decodeMask(uint64(fec.mask1), 31, fec.seqNumBase+15)...)
	}
	if fec.mask2 != 0 {
		protectedSeqs = append(protectedSeqs, decodeMask(fec.mask2, 63, fec.seqNumBase+46)...)
	}

	baseExt := extendSequence(s.rightExt, fec.seqNumBase)
	cols := make([]int64, 0, len(protectedSeqs))
	for _, seq := range protectedSeqs {
		offset := int64(uint16(seq - fec.seqNumBase))
		cols = append(cols, baseExt+offset)
	}

	return cols
}

// enqueueRecovered files solutions either behind the right edge (future) or into
// the ascending ready queue. snap is the solving input's attribute snapshot.
func (s *repairStream) enqueueRecovered(recovered []rankRecovery, snap interceptor.Attributes) {
	for _, rec := range recovered {
		if _, done := s.delivered[rec.ext]; done {
			continue
		}
		if _, queued := s.queued[rec.ext]; queued {
			continue
		}

		item := pendingPacket{
			ext:   rec.ext,
			raw:   rec.raw,
			attrs: recoveredAttributes(snap),
		}
		s.queued[rec.ext] = struct{}{}

		if s.hasBase && rec.ext > s.rightExt {
			s.future[rec.ext] = item

			continue
		}

		s.insertReady(item)
	}
}

func (s *repairStream) insertReady(item pendingPacket) {
	idx := len(s.ready)
	for i, p := range s.ready {
		if item.ext < p.ext {
			idx = i

			break
		}
	}
	s.ready = append(s.ready, pendingPacket{})
	copy(s.ready[idx+1:], s.ready[idx:])
	s.ready[idx] = item
}

// promoteFuture moves held solutions whose numbers are now covered by the
// original-media right edge into the ready queue.
func (s *repairStream) promoteFuture() {
	if len(s.future) == 0 {
		return
	}
	for ext, item := range s.future {
		if ext <= s.rightExt {
			delete(s.future, ext)
			s.insertReady(item)
		}
	}
}
