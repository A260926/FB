// SPDX-License-Identifier: MIT

package jitterbuffer

import (
	"errors"
	"io"
	"sync"
	"testing"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/task_support/taskcheck"
	"github.com/pion/rtp"
)

const (
	attrSeqKey   = "seq"
	attrTSKey    = "ts"
	attrProbeKey = "probe"
)

// manualClock is a fully deterministic clock advanced explicitly by the test.
type manualClock struct {
	t time.Time
}

func (m *manualClock) now() time.Time { return m.t }

func (m *manualClock) advance(d time.Duration) { m.t = m.t.Add(d) }

// genRTPReader is an in-memory RTPReader that synthesizes packets without any
// socket or sleep, and records how many times Read was invoked.
type genRTPReader struct {
	mu          sync.Mutex
	reads       int
	ssrc        uint32
	nextSeq     uint16
	ts          uint32
	ticksPerPkt uint32
	packetsLeft int
	sharedAttrs interceptor.Attributes
}

func (g *genRTPReader) Read(b []byte, _ interceptor.Attributes) (int, interceptor.Attributes, error) {
	g.mu.Lock()
	defer g.mu.Unlock()

	g.reads++
	if g.packetsLeft == 0 {
		return 0, nil, io.EOF
	}
	g.packetsLeft--

	seq := g.nextSeq
	pkt := &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			SSRC:           g.ssrc,
			SequenceNumber: seq,
			Timestamp:      g.ts,
		},
		//nolint:gosec // G115 test payload only needs the low byte of the sequence.
		Payload: []byte{byte(seq)},
	}
	// Reuse and mutate the same map to prove the interceptor copies it.
	if g.sharedAttrs == nil {
		g.sharedAttrs = interceptor.Attributes{}
	}
	g.sharedAttrs[attrSeqKey] = int(seq)
	g.sharedAttrs[attrTSKey] = int(g.ts)

	g.nextSeq++
	g.ts += g.ticksPerPkt

	n, err := pkt.MarshalTo(b)

	return n, g.sharedAttrs, err
}

func (g *genRTPReader) readCount() int {
	g.mu.Lock()
	defer g.mu.Unlock()

	return g.reads
}

// TestTimedSchedule covers playout deadline boundaries, underrun behavior,
// duplicate/late dropping and both Clear modes.
//
//nolint:cyclop,maintidx
func TestTimedSchedule(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := &manualClock{t: time.Unix(1000, 0)}
		const rate = uint32(90000)
		const delay = 100 * time.Millisecond

		_, err := NewTimed(0, delay, clock.now)
		c.Check(errors.Is(err, ErrInvalidPlayoutConfig), "zero clock rate: %v", err)
		_, err = NewTimed(rate, -1, clock.now)
		c.Check(errors.Is(err, ErrInvalidPlayoutConfig), "negative delay: %v", err)
		_, err = NewTimed(rate, delay, nil)
		c.Check(errors.Is(err, ErrInvalidPlayoutConfig), "nil clock: %v", err)

		jb, err := NewTimed(rate, delay, clock.now)
		c.Check(err == nil && jb != nil, "NewTimed: %v", err)

		_, err = jb.Pop()
		c.Check(errors.Is(err, ErrBufferUnderrun), "empty pop before anchor: %v", err)

		// First accepted packet anchors: arrival A=t0, timestamp T=9000, seq S=10.
		jb.Push(&rtp.Packet{
			Header:  rtp.Header{SequenceNumber: 10, Timestamp: 9000},
			Payload: []byte{10},
		})
		c.Check(jb.PlayoutHead() == 10, "head after anchor = %d", jb.PlayoutHead())

		_, err = jb.Pop()
		c.Check(errors.Is(err, ErrPlayoutNotReady), "pop before deadline: %v", err)

		clock.advance(99 * time.Millisecond)
		_, err = jb.Pop()
		c.Check(errors.Is(err, ErrPlayoutNotReady), "pop one ms before deadline: %v", err)

		clock.advance(time.Millisecond) // t0 + 100ms, exactly the deadline.
		pkt, err := jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 10 && pkt.Payload[0] == 10,
			"boundary pop: pkt=%v err=%v", pkt, err)
		c.Check(jb.PlayoutHead() == 11, "head after pop = %d", jb.PlayoutHead())

		_, err = jb.Pop()
		c.Check(errors.Is(err, ErrBufferUnderrun), "underrun keeps clock, got: %v", err)
		c.Check(jb.PlayoutHead() == 11, "underrun must not move head, got %d", jb.PlayoutHead())

		// 90 ticks at 90kHz is exactly 1ms of media time.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 11, Timestamp: 9090}, Payload: []byte{11}})
		_, err = jb.Pop()
		c.Check(errors.Is(err, ErrPlayoutNotReady), "next packet not due at t0+100ms: %v", err)
		clock.advance(time.Millisecond)
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 11, "media-clock pop: %v %v", pkt, err)
		c.Check(jb.PlayoutHead() == 12, "head = %d", jb.PlayoutHead())

		// Duplicate sequence: the first accepted content wins.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 12, Timestamp: 9180}, Payload: []byte{1}})
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 12, Timestamp: 9180}, Payload: []byte{2}})
		clock.advance(time.Millisecond)
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 12 && pkt.Payload[0] == 1,
			"duplicate content: %v err=%v", pkt, err)
		c.Check(jb.PlayoutHead() == 13, "head = %d", jb.PlayoutHead())

		// Late packet behind the playout head is discarded.
		queued := jb.packets.Length()
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 5, Timestamp: 100}, Payload: []byte{5}})
		c.Check(jb.packets.Length() == queued, "late packet must be dropped")

		// Clear(false) drops packets but keeps head and anchor.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 13, Timestamp: 9270}, Payload: []byte{13}})
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 14, Timestamp: 9360}, Payload: []byte{14}})
		jb.Clear(false)
		_, err = jb.Pop()
		c.Check(errors.Is(err, ErrBufferUnderrun), "cleared cache: %v", err)
		c.Check(jb.PlayoutHead() == 13, "Clear(false) keeps head, got %d", jb.PlayoutHead())

		// Seq 15 is due at t0 + 100ms + 450/90000s = t0 + 105ms; old anchor still applies.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 15, Timestamp: 9450}, Payload: []byte{15}})
		clock.advance(3 * time.Millisecond) // t0 + 105ms
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 15, "retained anchor pop: %v %v", pkt, err)
		c.Check(jb.PlayoutHead() == 16, "head after gap skip = %d", jb.PlayoutHead())

		// Clear(true) removes the anchor; the next accepted packet re-anchors.
		jb.Clear(true)
		clock.advance(95 * time.Millisecond) // t0 + 200ms
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 20, Timestamp: 500000}, Payload: []byte{20}})
		_, err = jb.Pop()
		c.Check(errors.Is(err, ErrPlayoutNotReady), "re-anchored packet not due immediately: %v", err)
		clock.advance(100 * time.Millisecond) // arrival + delay
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 20, "re-anchor pop: %v %v", pkt, err)

		// A missing sequence is abandoned once the following packet is due.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 22, Timestamp: 500180}, Payload: []byte{22}})
		clock.advance(2 * time.Millisecond)
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 22, "gap abandonment: %v %v", pkt, err)
		c.Check(jb.PlayoutHead() == 23, "head skips missing seq 21, got %d", jb.PlayoutHead())

		// Two packets can share one RTP timestamp: they have the same deadline
		// and must both be delivered in sequence order at that single instant.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 23, Timestamp: 500360}, Payload: []byte{23}})
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 24, Timestamp: 500360}, Payload: []byte{24}})
		_, err = jb.Pop()
		c.Check(errors.Is(err, ErrPlayoutNotReady), "shared-timestamp packets wait: %v", err)
		clock.advance(2 * time.Millisecond) // 360 ticks after the anchor is 4ms, reached now.
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 23, "shared timestamp first: %v %v", pkt, err)
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 24, "shared timestamp second: %v %v", pkt, err)
		c.Check(jb.PlayoutHead() == 25, "head after shared timestamp = %d", jb.PlayoutHead())
	})
}

// TestTimedGapWrap drives both counters through their 16/32-bit wrap points
// and checks gap fill-in, gap abandonment and out-of-order delivery.
//
//nolint:cyclop,maintidx
func TestTimedGapWrap(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := &manualClock{t: time.Unix(2000, 0)}
		const rate = uint32(90000)
		const delay = 10 * time.Millisecond

		jb, err := NewTimed(rate, delay, clock.now)
		c.Check(err == nil && jb != nil, "NewTimed: %v", err)

		// Anchor right before both counters wrap: 90 ticks == 1ms per packet.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 65534, Timestamp: 4294967200}, Payload: []byte{34}})
		// Deliberately out of order: seq 0 (after the timestamp wrap) before 65535.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 0, Timestamp: 84}, Payload: []byte{0}})
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 65535, Timestamp: 4294967290}, Payload: []byte{35}})
		// Gap: sequence 1 is missing.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 2, Timestamp: 264}, Payload: []byte{2}})

		c.Check(jb.PlayoutHead() == 65534, "anchor head = %d", jb.PlayoutHead())

		clock.advance(delay) // t0 + 10ms
		pkt, err := jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 65534, "anchor delivery: %v %v", pkt, err)

		_, err = jb.Pop() // 65535 is only due at t0 + 11ms
		c.Check(errors.Is(err, ErrPlayoutNotReady), "wrapped seq not ready: %v", err)

		clock.advance(time.Millisecond)
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 65535, "seq wrap pop: %v %v", pkt, err)
		c.Check(jb.PlayoutHead() == 0, "head wrapped to 0, got %d", jb.PlayoutHead())

		clock.advance(time.Millisecond) // t0 + 12ms
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 0 && pkt.Payload[0] == 0,
			"timestamp wrap pop: %v %v", pkt, err)
		c.Check(jb.PlayoutHead() == 1, "head = %d", jb.PlayoutHead())

		_, err = jb.Pop() // closest buffered packet is seq 2, due only at t0 + 14ms
		c.Check(errors.Is(err, ErrPlayoutNotReady), "gapped packet not ready: %v", err)

		clock.advance(2 * time.Millisecond) // t0 + 14ms
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 2, "deliver after gap: %v %v", pkt, err)
		c.Check(jb.PlayoutHead() == 3, "missing seq 1 abandoned, head = %d", jb.PlayoutHead())

		// Gap fill-in: seq 4 arrives before the missing seq 3, then seq 3 catches up.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 4, Timestamp: 444}, Payload: []byte{4}})
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 3, Timestamp: 354}, Payload: []byte{3}})
		_, err = jb.Pop()
		c.Check(errors.Is(err, ErrPlayoutNotReady), "filled-in seq 3 not ready yet: %v", err)
		clock.advance(time.Millisecond) // t0 + 15ms
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 3, "gap filled in: %v %v", pkt, err)
		clock.advance(time.Millisecond) // t0 + 16ms
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 4, "in-order resume: %v %v", pkt, err)
		c.Check(jb.PlayoutHead() == 5, "head = %d", jb.PlayoutHead())

		// Out-of-order arrival still plays out in sequence order.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 6, Timestamp: 624}, Payload: []byte{6}})
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 5, Timestamp: 534}, Payload: []byte{5}})
		clock.advance(time.Millisecond) // t0 + 17ms
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 5, "out-of-order first: %v %v", pkt, err)
		clock.advance(time.Millisecond) // t0 + 18ms
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 6, "out-of-order second: %v %v", pkt, err)
		c.Check(jb.PlayoutHead() == 7, "head = %d", jb.PlayoutHead())

		// Duplicate after a wrap still keeps the first content.
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 7, Timestamp: 714}, Payload: []byte{7}})
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 7, Timestamp: 714}, Payload: []byte{77}})
		clock.advance(time.Millisecond) // t0 + 19ms
		pkt, err = jb.Pop()
		c.Check(err == nil && pkt != nil && pkt.SequenceNumber == 7 && pkt.Payload[0] == 7,
			"wrapped duplicate: %v %v", pkt, err)

		// A late re-send of an already consumed sequence is dropped.
		queued := jb.packets.Length()
		jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 0, Timestamp: 84}, Payload: []byte{99}})
		c.Check(jb.packets.Length() == queued, "late wrapped duplicate must be dropped")
		_, err = jb.Pop()
		c.Check(errors.Is(err, ErrBufferUnderrun), "queue stays empty after late drop: %v", err)
	})
}

// TestTimedReceiver exercises the ReceiverInterceptor timed path with two
// different clock rates, per-packet attribute copies and upstream read limits.
//
//nolint:cyclop,maintidx
func TestTimedReceiver(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := &manualClock{t: time.Unix(3000, 0)}
		const delay = 10 * time.Millisecond

		badFactory, err := NewInterceptor(WithTimedPlayout(-1, clock.now))
		c.Check(err == nil, "option construction: %v", err)
		_, err = badFactory.NewInterceptor("bad")
		c.Check(errors.Is(err, ErrInvalidPlayoutConfig), "negative delay option: %v", err)
		badFactory, err = NewInterceptor(WithTimedPlayout(delay, nil))
		c.Check(err == nil, "option construction: %v", err)
		_, err = badFactory.NewInterceptor("bad")
		c.Check(errors.Is(err, ErrInvalidPlayoutConfig), "nil clock option: %v", err)

		factory, err := NewInterceptor(WithTimedPlayout(delay, clock.now))
		c.Check(err == nil, "NewInterceptor: %v", err)
		inst, err := factory.NewInterceptor("timed")
		c.Check(err == nil, "NewInterceptor instance: %v", err)
		defer func() { c.Check(inst.Close() == nil, "close") }()

		// Stream A: 90kHz, 1ms of media timestamp per packet, three packets.
		upstreamA := &genRTPReader{ssrc: 1001, ticksPerPkt: 90, packetsLeft: 3}
		infoA := &interceptor.StreamInfo{SSRC: 1001, ClockRate: 90000}
		readerA := inst.BindRemoteStream(infoA, upstreamA)
		raw := make([]byte, 1500)

		n, _, err := readerA.Read(raw, nil)
		c.Check(n == 0 && errors.Is(err, ErrPlayoutNotReady), "first read not ready: n=%d err=%v", n, err)
		c.Check(upstreamA.readCount() == 1, "first read touches upstream once: %d", upstreamA.readCount())
		_, _, err = readerA.Read(raw, nil)
		c.Check(errors.Is(err, ErrPlayoutNotReady), "second read not ready: %v", err)
		c.Check(upstreamA.readCount() == 2, "upstream reads = %d", upstreamA.readCount())

		clock.advance(delay) // t0 + 10ms: seq 0 is due.
		n, attr0, err := readerA.Read(raw, nil)
		c.Check(err == nil && n > 0, "due delivery without upstream: n=%d err=%v", n, err)
		c.Check(upstreamA.readCount() == 2, "due delivery must not read upstream: %d", upstreamA.readCount())
		out := &rtp.Packet{}
		c.Check(out.Unmarshal(raw[:n]) == nil && out.SequenceNumber == 0 && out.SSRC == 1001,
			"stream A first packet: %+v", out)
		c.Check(attr0 != nil && attr0[attrSeqKey] == 0, "attr0 at acceptance: %v", attr0)

		// seq 1 is due at t0 + 11ms, not yet; this call may consume one upstream packet.
		_, _, err = readerA.Read(raw, nil)
		c.Check(errors.Is(err, ErrPlayoutNotReady), "seq 1 waits: %v", err)
		c.Check(upstreamA.readCount() == 3, "one upstream read while waiting: %d", upstreamA.readCount())

		clock.advance(time.Millisecond)
		n, attr1, err := readerA.Read(raw, nil)
		c.Check(err == nil && n > 0, "seq 1 delivery: n=%d err=%v", n, err)
		c.Check(upstreamA.readCount() == 3, "no extra upstream read: %d", upstreamA.readCount())
		c.Check(attr1 != nil && attr1[attrSeqKey] == 1, "attr1: %v", attr1)

		clock.advance(time.Millisecond)
		n, attr2, err := readerA.Read(raw, nil)
		c.Check(err == nil && n > 0, "seq 2 delivery: n=%d err=%v", n, err)
		c.Check(upstreamA.readCount() == 3, "seq 2 served from buffer: %d", upstreamA.readCount())
		c.Check(attr2 != nil && attr2[attrSeqKey] == 2, "attr2: %v", attr2)

		// The shared upstream map kept being mutated; the delivered copies stay isolated.
		attr0[attrProbeKey] = "a"
		_, collided := attr1[attrProbeKey].(string)
		c.Check(!collided, "attribute maps must not be shared between packets")
		c.Check(attr0[attrSeqKey] == 0 && attr1[attrSeqKey] == 1 && attr2[attrSeqKey] == 2,
			"accepted attributes preserved: %v %v %v", attr0, attr1, attr2)

		// Empty buffer falls through to upstream once and surfaces its error.
		_, _, err = readerA.Read(raw, nil)
		c.Check(errors.Is(err, io.EOF), "upstream error surfaced: %v", err)
		c.Check(upstreamA.readCount() == 4, "underrun reads upstream once: %d", upstreamA.readCount())

		// Stream B: 8kHz with 20ms packets (160 ticks), a different clock rate.
		// It anchors at the current clock time (t0 + 12ms).
		upstreamB := &genRTPReader{ssrc: 2002, ticksPerPkt: 160, packetsLeft: 3}
		infoB := &interceptor.StreamInfo{SSRC: 2002, ClockRate: 8000}
		readerB := inst.BindRemoteStream(infoB, upstreamB)
		_, _, err = readerB.Read(raw, nil)
		c.Check(errors.Is(err, ErrPlayoutNotReady), "stream B first read: %v", err)
		_, _, err = readerB.Read(raw, nil)
		c.Check(errors.Is(err, ErrPlayoutNotReady), "stream B second read: %v", err)
		c.Check(upstreamB.readCount() == 2, "stream B upstream reads: %d", upstreamB.readCount())

		// seq 0 is due 10ms after the stream-B anchor; advancing gets it
		// delivered straight from the buffer without another upstream read.
		clock.advance(delay) // anchor + 10ms
		n, _, err = readerB.Read(raw, nil)
		c.Check(err == nil && n > 0, "8kHz seq 0: n=%d err=%v", n, err)
		c.Check(upstreamB.readCount() == 2, "8kHz due delivery without upstream: %d", upstreamB.readCount())

		// Another 10ms is only halfway to seq 1's deadline (20ms media packet):
		// it must still wait, and the waiting call pulls the last upstream packet.
		clock.advance(10 * time.Millisecond)
		_, _, err = readerB.Read(raw, nil)
		c.Check(errors.Is(err, ErrPlayoutNotReady), "8kHz 20ms packet waits: %v", err)
		c.Check(upstreamB.readCount() == 3, "waiting consumed seq 2 upstream: %d", upstreamB.readCount())

		clock.advance(10 * time.Millisecond) // seq 1 deadline
		n, bAttr, err := readerB.Read(raw, nil)
		c.Check(err == nil && n > 0, "8kHz seq 1: n=%d err=%v", n, err)
		c.Check(upstreamB.readCount() == 3, "due 8kHz packet skips upstream: %d", upstreamB.readCount())
		outB := &rtp.Packet{}
		c.Check(outB.Unmarshal(raw[:n]) == nil && outB.SequenceNumber == 1 && outB.Timestamp == 160,
			"stream B packet: %+v", outB)
		c.Check(bAttr != nil && bAttr[attrTSKey] == 160, "stream B attributes: %v", bAttr)

		// A zero clock rate keeps the reader shape but always fails without upstream.
		upstreamC := &genRTPReader{ssrc: 3003, ticksPerPkt: 1, packetsLeft: 2}
		infoC := &interceptor.StreamInfo{SSRC: 3003, ClockRate: 0}
		readerC := inst.BindRemoteStream(infoC, upstreamC)
		_, _, err = readerC.Read(raw, nil)
		c.Check(errors.Is(err, ErrInvalidPlayoutConfig), "zero clock rate read: %v", err)
		_, _, err = readerC.Read(raw, nil)
		c.Check(errors.Is(err, ErrInvalidPlayoutConfig), "zero clock rate repeat: %v", err)
		c.Check(upstreamC.readCount() == 0, "invalid stream never reads upstream: %d", upstreamC.readCount())

		// Unbind only clears its own SSRC; B still has seq 2 buffered.
		inst.UnbindRemoteStream(infoA)
		_, _, err = readerA.Read(raw, nil)
		c.Check(errors.Is(err, io.ErrClosedPipe), "unbound reader closed: %v", err)
		c.Check(upstreamA.readCount() == 4, "closed reader does not read upstream: %d", upstreamA.readCount())

		clock.advance(20 * time.Millisecond) // well past seq 2's anchor+30ms deadline
		n, _, err = readerB.Read(raw, nil)
		c.Check(err == nil && n > 0, "sibling stream survives unbind: n=%d err=%v", n, err)
		outB2 := &rtp.Packet{}
		c.Check(outB2.Unmarshal(raw[:n]) == nil && outB2.SequenceNumber == 2, "stream B seq 2: %+v", outB2)

		// Rebinding the same SSRC re-anchors against the current clock.
		upstreamA2 := &genRTPReader{ssrc: 1001, ticksPerPkt: 90, packetsLeft: 1}
		readerA2 := inst.BindRemoteStream(infoA, upstreamA2)
		_, _, err = readerA2.Read(raw, nil)
		c.Check(errors.Is(err, ErrPlayoutNotReady), "rebound stream re-buffers: %v", err)
		c.Check(upstreamA2.readCount() == 1, "rebound upstream read: %d", upstreamA2.readCount())
		clock.advance(delay)
		n, _, err = readerA2.Read(raw, nil)
		c.Check(err == nil && n > 0, "rebound delivery: n=%d err=%v", n, err)
		c.Check(upstreamA2.readCount() == 1, "rebound due delivery skips upstream: %d", upstreamA2.readCount())

		// Close retires every outstanding reader without touching upstream again.
		c.Check(inst.Close() == nil, "interceptor close")
		_, _, err = readerB.Read(raw, nil)
		c.Check(errors.Is(err, io.ErrClosedPipe), "closed stream B: %v", err)
		_, _, err = readerA2.Read(raw, nil)
		c.Check(errors.Is(err, io.ErrClosedPipe), "closed rebound stream: %v", err)
		c.Check(upstreamB.readCount() == 3, "close adds no upstream reads: %d", upstreamB.readCount())
		c.Check(upstreamA2.readCount() == 1, "close adds no rebound reads: %d", upstreamA2.readCount())
	})
}
