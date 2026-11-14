// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

// Package jitterbuffer implements a buffer for RTP packets designed to help
// counteract non-deterministic sources of latency
package jitterbuffer

import (
	"errors"
	"sync"
	"time"

	"github.com/pion/rtp"
)

// State tracks a JitterBuffer as either Buffering or Emitting.
type State uint16

// Event represents all events a JitterBuffer can emit.
type Event string

var (
	// ErrBufferUnderrun is returned when the buffer has no items.
	ErrBufferUnderrun = errors.New("invalid Peek: Empty jitter buffer")
	// ErrPopWhileBuffering is returned if a jitter buffer is not in a playback state.
	ErrPopWhileBuffering = errors.New("attempt to pop while buffering")
	// ErrPlayoutNotReady is returned by a timed jitter buffer when the packet at
	// the playout head has been received but its playout time has not arrived.
	ErrPlayoutNotReady = errors.New("playout not ready")
	// ErrInvalidPlayoutConfig is returned by NewTimed when its configuration is
	// invalid (a zero clock rate, a negative delay, or a nil clock).
	ErrInvalidPlayoutConfig = errors.New("invalid timed playout configuration")
)

const (
	// Buffering is the state when the jitter buffer has not started emitting yet,
	// or has hit an underflow and needs to re-buffer packets.
	Buffering State = iota
	//  Emitting is the state when the jitter buffer is operating nominally
	Emitting
)

const (
	// StartBuffering is emitted when the buffer receives its first packet.
	StartBuffering Event = "startBuffering"
	// BeginPlayback is emitted when the buffer has satisfied its buffer length.
	BeginPlayback = "playing"
	// BufferUnderflow is emitted when the buffer does not have enough packets to Pop.
	BufferUnderflow = "underflow"
	// BufferOverflow is emitted when the buffer has exceeded its limit.
	BufferOverflow = "overflow"
)

func (jbs State) String() string {
	switch jbs {
	case Buffering:
		return "Buffering"
	case Emitting:
		return "Emitting"
	}

	return "unknown"
}

type (
	// Option will Override JitterBuffer's defaults.
	Option func(jb *JitterBuffer)
	// EventListener will be called when the corresponding Event occurs.
	EventListener func(event Event, jb *JitterBuffer)
)

// A JitterBuffer will accept Pushed packets, put them in sequence number
// order, and allows removing in either sequence number order or via a
// provided timestamp.
type JitterBuffer struct {
	packets       *PriorityQueue
	minStartCount uint16
	overflowLen   uint16
	lastSequence  uint16
	playoutHead   uint16
	playoutReady  bool
	state         State
	stats         Stats
	listeners     map[Event][]EventListener
	mutex         sync.Mutex

	// Timed playout state. When timed is true the buffer schedules delivery
	// against the RTP media clock instead of waiting for minStartCount packets.
	timed      bool
	clockRate  uint32
	delay      time.Duration
	now        func() time.Time
	anchored   bool
	anchorTime time.Time
	anchorTS   uint32
}

// Stats Track interesting statistics for the life of this JitterBuffer
// outOfOrderCount will provide the number of times a packet was Pushed
//
//	without its predecessor being present
//
// underflowCount will provide the count of attempts to Pop an empty buffer
// overflowCount will track the number of times the jitter buffer exceeds its limit.
type Stats struct {
	outOfOrderCount uint32
	underflowCount  uint32
	overflowCount   uint32
}

// New will initialize a jitter buffer and its associated statistics.
func New(opts ...Option) *JitterBuffer {
	jb := &JitterBuffer{
		state:         Buffering,
		stats:         Stats{0, 0, 0},
		minStartCount: 50,
		overflowLen:   100,
		packets:       NewQueue(),
		listeners:     make(map[Event][]EventListener),
	}

	for _, o := range opts {
		o(jb)
	}

	return jb
}

// NewTimed initializes a jitter buffer that schedules Pop against the RTP
// media clock. The first accepted packet establishes an anchor from its
// arrival time (as reported by now), RTP timestamp and sequence number, and
// every packet becomes due at
//
//	arrival + delay + (packet.Timestamp - anchorTimestamp) * 1s / clockRate
//
// Timed mode never waits for a packet count, starts no goroutines and creates
// no timers: the caller drives playout by calling Pop while now advances
// monotonically. It returns ErrInvalidPlayoutConfig if clockRate is zero,
// delay is negative, or now is nil.
func NewTimed(clockRate uint32, delay time.Duration, now func() time.Time) (*JitterBuffer, error) {
	if clockRate == 0 || delay < 0 || now == nil {
		return nil, ErrInvalidPlayoutConfig
	}

	jb := New()
	jb.timed = true
	jb.clockRate = clockRate
	jb.delay = delay
	jb.now = now

	return jb, nil
}

// playoutDue computes the wall-clock playout time of a packet from its RTP
// timestamp, using integer math only. The caller must hold the mutex and the
// buffer must be anchored.
func (jb *JitterBuffer) playoutDue(timestamp uint32) time.Time {
	//nolint:gosec // G115 the signed cast interprets the 32-bit timestamp wrap.
	delta := int64(int32(timestamp - jb.anchorTS))

	return jb.anchorTime.Add(jb.delay).Add(time.Duration(delta * 1e9 / int64(jb.clockRate)))
}

// seqForwardDelta is the signed modular distance of a sequence number from the
// playout head: negative values are already behind the head. Both counters are
// 16-bit, so the narrowing conversion is intentional and safe.
func seqForwardDelta(seq, playoutHead uint16) int16 {
	//nolint:gosec // G115 modular sequence comparison relies on the wrap.
	return int16(seq - playoutHead)
}

// WithMinimumPacketCount will set the required number of packets to be received before
// any attempt to pop a packet can succeed.
func WithMinimumPacketCount(count uint16) Option {
	return func(jb *JitterBuffer) {
		jb.minStartCount = count
	}
}

// Listen will register an event listener
// The jitter buffer may emit events correspnding, interested listerns should
// look at Event for available events.
func (jb *JitterBuffer) Listen(event Event, cb EventListener) {
	jb.listeners[event] = append(jb.listeners[event], cb)
}

// PlayoutHead returns the SequenceNumber that will be attempted to Pop next.
func (jb *JitterBuffer) PlayoutHead() uint16 {
	jb.mutex.Lock()
	defer jb.mutex.Unlock()

	return jb.playoutHead
}

// SetPlayoutHead allows you to manually specify the packet you wish to pop next
// If you have encountered a packet that hasn't resolved you can skip it.
func (jb *JitterBuffer) SetPlayoutHead(playoutHead uint16) {
	jb.mutex.Lock()
	defer jb.mutex.Unlock()

	jb.playoutHead = playoutHead
}

func (jb *JitterBuffer) updateStats(lastPktSeqNo uint16) {
	// If we have at least one packet, and the next packet being pushed in is not
	// at the expected sequence number increment the out of order count
	if jb.packets.Length() > 0 && lastPktSeqNo != (jb.lastSequence+1) {
		jb.stats.outOfOrderCount++
	}
	jb.lastSequence = lastPktSeqNo
}

// Push an RTP packet into the jitter buffer, this does not clone
// the data so if the memory is expected to be reused, the caller should
// take this in to account and pass a copy of the packet they wish to buffer.
//
// In timed mode the first accepted packet anchors the media clock. Packets
// that duplicate an already accepted sequence number or arrive behind the
// playout head are dropped, and dropped packets never rebuild the anchor.
func (jb *JitterBuffer) Push(packet *rtp.Packet) {
	jb.mutex.Lock()
	defer jb.mutex.Unlock()

	if jb.timed {
		jb.pushTimed(packet)

		return
	}

	if jb.packets.Length() == 0 {
		jb.emit(StartBuffering)
	}

	if jb.packets.Length() > jb.overflowLen {
		jb.stats.overflowCount++
		jb.emit(BufferOverflow)
	}

	if !jb.playoutReady && jb.packets.Length() == 0 {
		jb.playoutHead = packet.SequenceNumber
	}

	jb.updateStats(packet.SequenceNumber)
	jb.packets.Push(packet, packet.SequenceNumber)
	jb.updateState()
}

func (jb *JitterBuffer) pushTimed(packet *rtp.Packet) bool {
	if !jb.anchored {
		// The first accepted packet establishes the anchor and playout head.
		jb.anchored = true
		jb.anchorTime = jb.now()
		jb.anchorTS = packet.Timestamp
		jb.playoutHead = packet.SequenceNumber
		jb.state = Emitting
		jb.playoutReady = true
		jb.emit(StartBuffering)
		jb.emit(BeginPlayback)
		jb.updateStats(packet.SequenceNumber)
		jb.packets.Push(packet, packet.SequenceNumber)

		return true
	}
	// Duplicates keep the content of the first accepted packet.
	if _, err := jb.packets.Find(packet.SequenceNumber); err == nil {
		return false
	}

	// Late packets whose sequence is already behind the playout head are
	// discarded and must not rebuild the anchor.
	if seqForwardDelta(packet.SequenceNumber, jb.playoutHead) < 0 {
		return false
	}

	if jb.packets.Length() > jb.overflowLen {
		jb.stats.overflowCount++
		jb.emit(BufferOverflow)
	}

	jb.updateStats(packet.SequenceNumber)
	jb.packets.Push(packet, packet.SequenceNumber)

	return true
}

// pushTimedAccepted pushes a packet in timed mode and reports whether it was
// accepted. A duplicated or late packet is rejected and returns false without
// rebuilding the anchor.
func (jb *JitterBuffer) pushTimedAccepted(packet *rtp.Packet) bool {
	jb.mutex.Lock()
	defer jb.mutex.Unlock()

	if !jb.timed {
		return false
	}

	return jb.pushTimed(packet)
}

func (jb *JitterBuffer) emit(event Event) {
	for _, l := range jb.listeners[event] {
		l(event, jb)
	}
}

func (jb *JitterBuffer) updateState() {
	// For now, we only look at the number of packets captured in the play buffer
	if jb.packets.Length() >= jb.minStartCount && jb.state == Buffering {
		jb.state = Emitting
		jb.playoutReady = true
		jb.emit(BeginPlayback)
	}
}

// Peek at the packet which is either:
//
//	At the playout head when we are emitting, and the playoutHead flag is true
//
// or else
//
//	At the last sequence received
func (jb *JitterBuffer) Peek(playoutHead bool) (*rtp.Packet, error) {
	jb.mutex.Lock()
	defer jb.mutex.Unlock()
	if jb.packets.Length() < 1 {
		return nil, ErrBufferUnderrun
	}
	if playoutHead && jb.state == Emitting {
		return jb.packets.Find(jb.playoutHead)
	}

	return jb.packets.Find(jb.lastSequence)
}

// Pop an RTP packet from the jitter buffer at the current playout head.
//
// In timed mode the received packet closest to the playout head in sequence
// order is selected. If its media-clock playout time has not been reached yet
// Pop returns ErrPlayoutNotReady without moving the playout head; otherwise
// the packet is delivered and the head advances past it, skipping any missing
// sequences in between. An empty queue still returns ErrBufferUnderrun and
// never re-anchors the clock.
func (jb *JitterBuffer) Pop() (*rtp.Packet, error) {
	jb.mutex.Lock()
	defer jb.mutex.Unlock()

	if jb.timed {
		return jb.popTimed()
	}

	if jb.state != Emitting {
		return nil, ErrPopWhileBuffering
	}
	packet, err := jb.packets.PopAt(jb.playoutHead)
	if err != nil {
		jb.stats.underflowCount++
		jb.emit(BufferUnderflow)

		return nil, err
	}
	jb.playoutHead = (jb.playoutHead + 1)
	jb.updateState()

	return packet, nil
}

func (jb *JitterBuffer) popTimed() (*rtp.Packet, error) {
	if jb.packets.Length() == 0 {
		jb.stats.underflowCount++
		jb.emit(BufferUnderflow)

		return nil, ErrBufferUnderrun
	}

	// Pick the received packet with the smallest forward distance from the
	// playout head, comparing sequence numbers modulo 2^16.
	head := jb.packets.next
	closest := head
	closestDist := seqForwardDelta(head.priority, jb.playoutHead)
	for pos := head.next; pos != nil; pos = pos.next {
		dist := seqForwardDelta(pos.priority, jb.playoutHead)
		if dist < closestDist {
			closest = pos
			closestDist = dist
		}
	}

	if jb.now().Before(jb.playoutDue(closest.val.Timestamp)) {
		return nil, ErrPlayoutNotReady
	}

	packet, err := jb.packets.PopAt(closest.priority)
	if err != nil {
		jb.stats.underflowCount++
		jb.emit(BufferUnderflow)

		return nil, err
	}
	// Advance past the delivered packet; any missing sequences are skipped.
	jb.playoutHead = closest.priority + 1

	return packet, nil
}

// PopAtSequence will pop an RTP packet from the jitter buffer at the specified Sequence.
func (jb *JitterBuffer) PopAtSequence(sq uint16) (*rtp.Packet, error) {
	jb.mutex.Lock()
	defer jb.mutex.Unlock()
	if jb.state != Emitting {
		return nil, ErrPopWhileBuffering
	}
	packet, err := jb.packets.PopAt(sq)
	if err != nil {
		jb.stats.underflowCount++
		jb.emit(BufferUnderflow)

		return nil, err
	}
	jb.playoutHead = (jb.playoutHead + 1)
	jb.updateState()

	return packet, nil
}

// PeekAtSequence will return an RTP packet from the jitter buffer at the specified Sequence
// without removing it from the buffer.
func (jb *JitterBuffer) PeekAtSequence(sq uint16) (*rtp.Packet, error) {
	jb.mutex.Lock()
	defer jb.mutex.Unlock()
	packet, err := jb.packets.Find(sq)
	if err != nil {
		return nil, err
	}

	return packet, nil
}

// PopAtTimestamp pops an RTP packet from the jitter buffer with the provided timestamp
// Call this method repeatedly to drain the buffer at the timestamp.
func (jb *JitterBuffer) PopAtTimestamp(ts uint32) (*rtp.Packet, error) {
	jb.mutex.Lock()
	defer jb.mutex.Unlock()
	if jb.state != Emitting {
		return nil, ErrPopWhileBuffering
	}
	packet, err := jb.packets.PopAtTimestamp(ts)
	if err != nil {
		jb.stats.underflowCount++
		jb.emit(BufferUnderflow)

		return nil, err
	}
	jb.updateState()

	return packet, nil
}

// Clear will empty the buffer and optionally reset the state.
//
// In timed mode Clear(false) only drops cached packets while keeping the
// playout head and clock anchor, whereas Clear(true) also removes the anchor
// so the next accepted packet re-anchors the schedule. The construction
// parameters (clock rate, delay, clock) are retained either way.
func (jb *JitterBuffer) Clear(resetState bool) {
	jb.mutex.Lock()
	defer jb.mutex.Unlock()
	jb.packets.Clear()

	if jb.timed {
		if resetState {
			jb.anchored = false
			jb.anchorTime = time.Time{}
			jb.anchorTS = 0
			jb.playoutHead = 0
			jb.playoutReady = false
			jb.state = Buffering
			jb.stats = Stats{0, 0, 0}
		}

		return
	}

	if resetState {
		jb.lastSequence = 0
		jb.state = Buffering
		jb.stats = Stats{0, 0, 0}
		jb.minStartCount = 50
	}
}
