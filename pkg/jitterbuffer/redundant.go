// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package jitterbuffer

import (
	"errors"
	"io"
	"sync"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
)

// Errors specific to redundant dual-path reception.
var (
	// ErrInvalidRedundantConfig is returned when RedundantConfig is invalid.
	ErrInvalidRedundantConfig = errors.New("invalid redundant jitter buffer configuration")
	// ErrRedundantPending is returned when no canonical packet is ready yet.
	ErrRedundantPending = errors.New("redundant packet pending")
	// ErrRedundantMode is returned by APIs that are disabled in redundant mode.
	ErrRedundantMode = errors.New("operation not supported in redundant mode")
)

const redundantSequenceHalfSpan = uint16(1 << 15)

// RedundantPath describes one of the two physical RTP paths carrying the
// redundant media stream.
type RedundantPath struct {
	// SSRC is the path-specific RTP SSRC.
	SSRC uint32
	// SequenceOffset converts a path sequence number to a canonical one:
	// canonical = path sequence - SequenceOffset (modulo 65536).
	SequenceOffset uint16
	// RTT is used to choose the first feedback path.
	RTT time.Duration
}

// RedundantConfig configures canonical merging of two redundant RTP paths.
type RedundantConfig struct {
	// OutputSSRC is the SSRC used by the merged canonical output.
	OutputSSRC uint32
	// PayloadType is the payload type used by the merged canonical output.
	PayloadType uint8
	// Paths are the two redundant input paths.
	Paths [2]RedundantPath
	// Window is the accepted canonical sequence span from the playout head.
	Window uint16
	// Hold limits how long a revealed head gap is waited for.
	Hold time.Duration
	// Retry is the per-gap minimum interval between successful feedback sends.
	Retry time.Duration
	// Now returns the current monotonic-ish time. Tests advance it manually.
	Now func() time.Time
}

func (c RedundantConfig) validate() error {
	if c.Paths[0].SSRC == 0 || c.Paths[1].SSRC == 0 || c.Paths[0].SSRC == c.Paths[1].SSRC {
		return ErrInvalidRedundantConfig
	}
	if c.OutputSSRC == 0 || c.OutputSSRC == c.Paths[0].SSRC || c.OutputSSRC == c.Paths[1].SSRC {
		return ErrInvalidRedundantConfig
	}
	if c.PayloadType > 127 {
		return ErrInvalidRedundantConfig
	}
	if c.Window < 4 || c.Window > 4096 {
		return ErrInvalidRedundantConfig
	}
	if c.Hold <= 0 || c.Retry <= 0 || c.Now == nil {
		return ErrInvalidRedundantConfig
	}
	if c.Paths[0].RTT < 0 || c.Paths[1].RTT < 0 {
		return ErrInvalidRedundantConfig
	}

	return nil
}

// redSlot is the winning copy of one canonical sequence position.
type redSlot struct {
	packet *rtp.Packet
	attrs  interceptor.Attributes
	path   int
}

// redFeedback is committed feedback/rotation state for one canonical gap.
type redFeedback struct {
	epoch      uint64
	gen        uint64
	hasLast    bool
	lastPath   int
	retryAfter time.Time
}

type redundantRuntime struct {
	cfg RedundantConfig

	// owner and queue mirror the canonical head/length onto the existing
	// JitterBuffer fields for code that inspects those package-private values.
	owner *JitterBuffer
	queue *PriorityQueue

	// mu protects every field below. Upstream RTP/RTCP calls are made after
	// unlocking this mutex.
	mu sync.Mutex

	epoch  uint64
	closed bool

	// Binding generations. A Bind increments pathGen; Unbind clears active.
	pathGen [2]uint64
	active  [2]bool
	// unbound is set by Unbind and cleared when that path is bound again. It
	// lets "both paths were unbound" work even if a test unbinds without first
	// retaining the bound reader.
	unbound [2]bool

	haveHead bool
	head     uint16
	slots    map[uint16]*redSlot

	holdArmed    bool
	holdSeq      uint16
	holdDeadline time.Time

	nextGapGen uint64
	feedback   map[uint16]*redFeedback
}

// NewRedundant creates a JitterBuffer that merges two redundant RTP paths into
// one canonical sequence/SSRC domain.
func NewRedundant(cfg RedundantConfig) (*JitterBuffer, error) {
	if err := cfg.validate(); err != nil {
		return nil, err
	}

	jb := New()
	rt := &redundantRuntime{
		cfg:      cfg,
		owner:    jb,
		queue:    jb.packets,
		slots:    make(map[uint16]*redSlot),
		feedback: make(map[uint16]*redFeedback),
	}
	jb.redundant = rt

	return jb, nil
}

// WithRedundant enables redundant dual-path merging on a ReceiverInterceptor.
// Configuration is validated when InterceptorFactory.NewInterceptor applies
// this option.
func WithRedundant(cfg RedundantConfig) ReceiverInterceptorOption {
	return func(d *ReceiverInterceptor) error {
		jb, err := NewRedundant(cfg)
		if err != nil {
			return err
		}
		d.buffer = jb

		return nil
	}
}

func (rt *redundantRuntime) now() time.Time {
	return rt.cfg.Now()
}

func (rt *redundantRuntime) pathForSSRC(ssrc uint32) int {
	for i := range rt.cfg.Paths {
		if rt.cfg.Paths[i].SSRC == ssrc {
			return i
		}
	}

	return -1
}

func (rt *redundantRuntime) playoutHead() uint16 {
	rt.mu.Lock()
	defer rt.mu.Unlock()

	return rt.head
}

// syncOwnerLocked mirrors state onto the legacy JitterBuffer fields. The
// redundant branches never use those fields for decisions.
func (rt *redundantRuntime) syncOwnerLocked() {
	if rt.owner == nil {
		return
	}
	rt.owner.playoutHead = rt.head
	rt.owner.playoutReady = rt.haveHead
	if rt.haveHead {
		rt.owner.state = Emitting
	} else {
		rt.owner.state = Buffering
	}
}

func shallowCopyAttributes(attrs interceptor.Attributes) interceptor.Attributes {
	if attrs == nil {
		return nil
	}
	copied := make(interceptor.Attributes, len(attrs))
	for key, value := range attrs {
		copied[key] = value
	}

	return copied
}

// push accepts one path packet. It returns whether the packet was a valid,
// previously absent canonical position.
func (rt *redundantRuntime) push(packet *rtp.Packet, attrs interceptor.Attributes) bool {
	rt.mu.Lock()
	defer rt.mu.Unlock()

	return rt.insertLocked(packet, attrs)
}

func (rt *redundantRuntime) pushFromGeneration(
	path int, generation uint64, packet *rtp.Packet, attrs interceptor.Attributes,
) error {
	rt.mu.Lock()
	defer rt.mu.Unlock()

	if !rt.generationCurrentLocked(path, generation) {
		return io.ErrClosedPipe
	}
	rt.insertLocked(packet, attrs)

	return nil
}

func (rt *redundantRuntime) insertLocked(packet *rtp.Packet, attrs interceptor.Attributes) bool {
	if packet == nil {
		return false
	}

	path := rt.pathForSSRC(packet.Header.SSRC)
	if path < 0 || packet.Header.PayloadType != rt.cfg.PayloadType {
		return false
	}

	canonical := packet.Header.SequenceNumber - rt.cfg.Paths[path].SequenceOffset

	if rt.haveHead {
		distance := canonical - rt.head
		// Distances in the upper half of the uint16 space are behind the head;
		// Window is at most 4096, so this also rejects everything too far ahead.
		if distance >= redundantSequenceHalfSpan || distance >= rt.cfg.Window {
			return false
		}
	}

	if _, exists := rt.slots[canonical]; exists {
		// Duplicates consume neither capacity nor timing state.
		return false
	}

	// Clone performs the required deep copy of payload, header extensions,
	// CSRCs and all other slice-backed header fields.
	merged := packet.Clone()
	merged.Header.SequenceNumber = canonical
	merged.Header.SSRC = rt.cfg.OutputSSRC
	merged.Header.PayloadType = rt.cfg.PayloadType

	rt.slots[canonical] = &redSlot{
		packet: merged,
		attrs:  shallowCopyAttributes(attrs),
		path:   path,
	}
	if rt.queue != nil {
		rt.queue.Push(merged, canonical)
	}

	if !rt.haveHead {
		rt.haveHead = true
		rt.head = canonical
	}
	rt.syncOwnerLocked()

	rt.reconcileHoldLocked(rt.now())

	return true
}

// earliestSlotLocked returns the occupied slot with the smallest forward
// modular distance from the current head.
func (rt *redundantRuntime) earliestSlotLocked() (*redSlot, uint16, bool) {
	var earliest uint16
	var bestDistance uint16
	found := false

	for seq := range rt.slots {
		distance := seq - rt.head
		if distance >= redundantSequenceHalfSpan {
			continue
		}
		if !found || distance < bestDistance {
			found = true
			earliest = seq
			bestDistance = distance
		}
	}

	if !found {
		return nil, 0, false
	}

	return rt.slots[earliest], bestDistance, true
}

// reconcileHoldLocked arms the deadline exactly when the head is missing but a
// successor is present. An existing deadline for the same head is preserved.
func (rt *redundantRuntime) reconcileHoldLocked(now time.Time) {
	if !rt.haveHead {
		rt.holdArmed = false
		return
	}

	if _, ok := rt.slots[rt.head]; ok {
		rt.holdArmed = false
		return
	}

	if _, _, found := rt.earliestSlotLocked(); !found {
		rt.holdArmed = false
		return
	}

	if !rt.holdArmed || rt.holdSeq != rt.head {
		rt.holdArmed = true
		rt.holdSeq = rt.head
		rt.holdDeadline = now.Add(rt.cfg.Hold)
	}
}

type redPopPlan struct {
	slot    *redSlot
	oldHead uint16
	newHead uint16
	lastSeq uint16
}

// planPopLocked determines the next output without mutating state. maxSize < 0
// disables the output buffer-size check.
func (rt *redundantRuntime) planPopLocked(now time.Time, maxSize int) (*redPopPlan, error) {
	if !rt.haveHead {
		return nil, ErrRedundantPending
	}

	if slot, ok := rt.slots[rt.head]; ok {
		if maxSize >= 0 && slot.packet.MarshalSize() > maxSize {
			return nil, io.ErrShortBuffer
		}

		return &redPopPlan{
			slot:    slot,
			oldHead: rt.head,
			newHead: rt.head + 1,
			lastSeq: rt.head,
		}, nil
	}

	_, _, hasSuccessor := rt.earliestSlotLocked()
	if !hasSuccessor {
		rt.holdArmed = false
		return nil, ErrRedundantPending
	}

	rt.reconcileHoldLocked(now)
	if now.Before(rt.holdDeadline) {
		return nil, ErrRedundantPending
	}

	slot, distance, _ := rt.earliestSlotLocked()
	earliest := rt.head + distance
	if maxSize >= 0 && slot.packet.MarshalSize() > maxSize {
		return nil, io.ErrShortBuffer
	}

	return &redPopPlan{
		slot:    slot,
		oldHead: rt.head,
		newHead: earliest + 1,
		lastSeq: earliest,
	}, nil
}

func (rt *redundantRuntime) commitPopLocked(plan *redPopPlan, now time.Time) {
	// Every sequence from oldHead through the output packet is either consumed
	// or permanently retired. Drop its feedback state as well.
	for seq := plan.oldHead; ; seq++ {
		delete(rt.feedback, seq)
		if seq == plan.lastSeq {
			break
		}
	}
	delete(rt.slots, plan.lastSeq)
	if rt.queue != nil {
		_, _ = rt.queue.PopAt(plan.lastSeq)
	}

	rt.head = plan.newHead
	rt.haveHead = true
	rt.syncOwnerLocked()
	rt.reconcileHoldLocked(now)
}

func (rt *redundantRuntime) pop() (*rtp.Packet, error) {
	rt.mu.Lock()
	defer rt.mu.Unlock()

	now := rt.now()
	plan, err := rt.planPopLocked(now, -1)
	if err != nil {
		return nil, err
	}
	packet := plan.slot.packet
	rt.commitPopLocked(plan, now)

	return packet, nil
}

func (rt *redundantRuntime) clear(resetHead bool) {
	rt.mu.Lock()
	defer rt.mu.Unlock()
	rt.clearLocked(resetHead)
}

func (rt *redundantRuntime) clearLocked(resetHead bool) {
	rt.epoch++
	rt.slots = make(map[uint16]*redSlot)
	rt.feedback = make(map[uint16]*redFeedback)
	if rt.queue != nil {
		rt.queue.Clear()
	}
	rt.holdArmed = false
	rt.holdSeq = 0
	rt.holdDeadline = time.Time{}
	if resetHead {
		rt.haveHead = false
		rt.head = 0
	}
	rt.syncOwnerLocked()
}

// isOutstandingLocked reports whether canonical seq is revealed by a later
// received copy, is not filled, and has not been consumed/retired.
func (rt *redundantRuntime) isOutstandingLocked(seq uint16) bool {
	if !rt.haveHead {
		return false
	}

	distance := seq - rt.head
	if distance >= redundantSequenceHalfSpan || distance >= rt.cfg.Window {
		return false
	}
	if _, filled := rt.slots[seq]; filled {
		return false
	}

	for received := range rt.slots {
		ahead := received - seq
		if ahead > 0 && ahead < redundantSequenceHalfSpan {
			return true
		}
	}

	return false
}

type redNackCommit struct {
	canonical uint16
	path      int
	epoch     uint64
	gapGen    uint64
	pathGen   uint64
}

func (rt *redundantRuntime) chooseFeedbackPathLocked(seq uint16, now time.Time) (int, *redFeedback, bool, uint64) {
	fs := rt.feedback[seq]
	if fs == nil || fs.epoch != rt.epoch {
		rt.nextGapGen++
		fs = &redFeedback{
			epoch: rt.epoch,
			gen:   rt.nextGapGen,
		}
		rt.feedback[seq] = fs
	}

	if fs.hasLast && now.Before(fs.retryAfter) {
		return 0, fs, true, rt.pathGen[0]
	}

	var path int
	if fs.hasLast {
		path = 1 - fs.lastPath
	} else if rt.cfg.Paths[0].RTT < rt.cfg.Paths[1].RTT {
		path = 0
	} else if rt.cfg.Paths[1].RTT < rt.cfg.Paths[0].RTT {
		path = 1
	} else {
		path = 0
	}

	return path, fs, false, rt.pathGen[path]
}

// transformNACKLocked expands one canonical OutputSSRC NACK into zero, one or
// two path-SSRC NACKs.
func (rt *redundantRuntime) transformNACKLocked(
	nack *rtcp.TransportLayerNack,
) ([]rtcp.Packet, []redNackCommit) {
	now := rt.now()
	var pathSequences [2][]uint16
	var commits []redNackCommit
	seen := make(map[uint16]struct{})

	for _, pair := range nack.Nacks {
		pair.Range(func(canonical uint16) bool {
			if _, duplicate := seen[canonical]; duplicate {
				return true
			}
			seen[canonical] = struct{}{}

			if !rt.isOutstandingLocked(canonical) {
				return true
			}

			path, fs, suppressed, pathGen := rt.chooseFeedbackPathLocked(canonical, now)
			if suppressed {
				return true
			}

			pathSequence := canonical + rt.cfg.Paths[path].SequenceOffset
			pathSequences[path] = append(pathSequences[path], pathSequence)
			commits = append(commits, redNackCommit{
				canonical: canonical,
				path:      path,
				epoch:     rt.epoch,
				gapGen:    fs.gen,
				pathGen:   pathGen,
			})

			return true
		})
	}

	var packets []rtcp.Packet
	for path := range rt.cfg.Paths {
		if len(pathSequences[path]) == 0 {
			continue
		}

		packets = append(packets, &rtcp.TransportLayerNack{
			SenderSSRC: nack.SenderSSRC,
			MediaSSRC:  rt.cfg.Paths[path].SSRC,
			Nacks:      rtcp.NackPairsFromSequenceNumbers(pathSequences[path]),
		})
	}

	return packets, commits
}

func (rt *redundantRuntime) commitNACKsLocked(commits []redNackCommit, now time.Time) {
	if rt.closed {
		return
	}

	for _, commit := range commits {
		if commit.epoch != rt.epoch || rt.pathGen[commit.path] != commit.pathGen {
			continue
		}

		fs := rt.feedback[commit.canonical]
		if fs == nil || fs.epoch != commit.epoch || fs.gen != commit.gapGen {
			continue
		}
		if !rt.isOutstandingLocked(commit.canonical) {
			continue
		}

		fs.hasLast = true
		fs.lastPath = commit.path
		fs.retryAfter = now.Add(rt.cfg.Retry)
	}
}

// BindRTCPWriter installs the redundant NACK translator. In default mode it is
// an exact passthrough.
func (i *ReceiverInterceptor) BindRTCPWriter(writer interceptor.RTCPWriter) interceptor.RTCPWriter {
	if i.buffer == nil || i.buffer.redundant == nil {
		return writer
	}

	return interceptor.RTCPWriterFunc(func(
		packets []rtcp.Packet, attributes interceptor.Attributes,
	) (int, error) {
		return i.writeRedundantRTCP(writer, packets, attributes)
	})
}

func (i *ReceiverInterceptor) writeRedundantRTCP(
	upstream interceptor.RTCPWriter,
	packets []rtcp.Packet,
	attributes interceptor.Attributes,
) (int, error) {
	rt := i.buffer.redundant

	rt.mu.Lock()
	if rt.closed {
		rt.mu.Unlock()
		return 0, io.ErrClosedPipe
	}

	out := make([]rtcp.Packet, 0, len(packets))
	var commits []redNackCommit
	for _, packet := range packets {
		nack, ok := packet.(*rtcp.TransportLayerNack)
		if !ok || nack.MediaSSRC != rt.cfg.OutputSSRC {
			out = append(out, packet)
			continue
		}

		transformed, nackCommits := rt.transformNACKLocked(nack)
		out = append(out, transformed...)
		commits = append(commits, nackCommits...)
	}
	rt.mu.Unlock()

	if len(out) == 0 {
		return 0, nil
	}

	n, err := upstream.Write(out, attributes)
	if err == nil {
		commitTime := rt.now()
		rt.mu.Lock()
		rt.commitNACKsLocked(commits, commitTime)
		rt.mu.Unlock()
	}

	return n, err
}

func closedPipeReader() interceptor.RTPReader {
	return interceptor.RTPReaderFunc(func(
		[]byte, interceptor.Attributes,
	) (int, interceptor.Attributes, error) {
		return 0, nil, io.ErrClosedPipe
	})
}

func (i *ReceiverInterceptor) bindRedundantRemoteStream(
	info *interceptor.StreamInfo, reader interceptor.RTPReader,
) interceptor.RTPReader {
	rt := i.buffer.redundant
	path := -1
	if info != nil {
		path = rt.pathForSSRC(info.SSRC)
	}
	if path < 0 {
		// Unconfigured SSRCs bypass redundant processing entirely.
		return reader
	}

	rt.mu.Lock()
	if rt.closed {
		rt.mu.Unlock()
		return closedPipeReader()
	}
	rt.pathGen[path]++
	generation := rt.pathGen[path]
	rt.active[path] = true
	rt.unbound[path] = false
	rt.mu.Unlock()

	return interceptor.RTPReaderFunc(func(b []byte, attrs interceptor.Attributes) (int, interceptor.Attributes, error) {
		return rt.readBound(path, generation, reader, b, attrs)
	})
}

func (rt *redundantRuntime) generationCurrentLocked(path int, generation uint64) bool {
	return !rt.closed && rt.active[path] && rt.pathGen[path] == generation
}

func (rt *redundantRuntime) readBound(
	path int,
	generation uint64,
	upstream interceptor.RTPReader,
	b []byte,
	attrs interceptor.Attributes,
) (int, interceptor.Attributes, error) {
	rt.mu.Lock()
	if !rt.generationCurrentLocked(path, generation) {
		rt.mu.Unlock()
		return 0, nil, io.ErrClosedPipe
	}

	now := rt.now()
	plan, err := rt.planPopLocked(now, len(b))
	if err == nil {
		n, marshalErr := plan.slot.packet.MarshalTo(b)
		if marshalErr != nil {
			rt.mu.Unlock()
			if errors.Is(marshalErr, io.ErrShortBuffer) {
				return 0, nil, io.ErrShortBuffer
			}
			return 0, nil, marshalErr
		}

		winnerAttrs := plan.slot.attrs
		rt.commitPopLocked(plan, now)
		rt.mu.Unlock()

		return n, winnerAttrs, nil
	}
	if !errors.Is(err, ErrRedundantPending) {
		// io.ErrShortBuffer: leave the planned/current packet unconsumed.
		rt.mu.Unlock()
		return 0, nil, err
	}
	rt.mu.Unlock()

	// At most one upstream read per call. No runtime lock is held across it.
	n, upstreamAttrs, err := upstream.Read(b, attrs)
	if err != nil {
		return n, upstreamAttrs, err
	}

	rt.mu.Lock()
	current := rt.generationCurrentLocked(path, generation)
	rt.mu.Unlock()
	if !current {
		return 0, nil, io.ErrClosedPipe
	}

	if n > 0 {
		packet := &rtp.Packet{}
		if unmarshalErr := packet.Unmarshal(b[:n]); unmarshalErr != nil {
			return 0, nil, unmarshalErr
		}

		acceptedAttrs := upstreamAttrs
		if acceptedAttrs == nil {
			acceptedAttrs = attrs
		}
		if pushErr := rt.pushFromGeneration(path, generation, packet, acceptedAttrs); pushErr != nil {
			return 0, nil, pushErr
		}
	}

	rt.mu.Lock()
	if !rt.generationCurrentLocked(path, generation) {
		rt.mu.Unlock()
		return 0, nil, io.ErrClosedPipe
	}

	now = rt.now()
	plan, err = rt.planPopLocked(now, len(b))
	if err != nil {
		rt.mu.Unlock()
		if errors.Is(err, ErrRedundantPending) {
			return 0, nil, ErrRedundantPending
		}

		return 0, nil, err
	}

	n, err = plan.slot.packet.MarshalTo(b)
	if err != nil {
		rt.mu.Unlock()
		if errors.Is(err, io.ErrShortBuffer) {
			return 0, nil, io.ErrShortBuffer
		}

		return 0, nil, err
	}

	winnerAttrs := plan.slot.attrs
	rt.commitPopLocked(plan, now)
	rt.mu.Unlock()

	return n, winnerAttrs, nil
}

func (i *ReceiverInterceptor) unbindRedundantRemoteStream(info *interceptor.StreamInfo) {
	rt := i.buffer.redundant
	if info == nil {
		return
	}
	path := rt.pathForSSRC(info.SSRC)
	if path < 0 {
		return
	}

	rt.mu.Lock()
	defer rt.mu.Unlock()

	rt.active[path] = false
	rt.unbound[path] = true
	// Invalidate any generation that is currently in an upstream call.
	rt.pathGen[path]++

	// Unbinding both configured paths empties the shared canonical window.
	// clearLocked(false) deliberately retains the playout head.
	if rt.unbound[0] && rt.unbound[1] {
		rt.clearLocked(false)
	}
}

func (i *ReceiverInterceptor) closeRedundant() error {
	rt := i.buffer.redundant

	rt.mu.Lock()
	defer rt.mu.Unlock()

	rt.closed = true
	rt.active[0] = false
	rt.active[1] = false
	rt.unbound[0] = true
	rt.unbound[1] = true
	rt.clearLocked(true)

	return nil
}
