package astits

import (
	"bytes"
	"sort"
)

// packetAccumulator keeps track of packets for a single PID and decides when to flush them
type packetAccumulator struct {
	pid        uint16
	programMap *programMap
	q          []*Packet

	// Continuity state of the last payload packet seen on this PID. It is kept
	// across queue flushes so that a retransmitted packet is still detected once
	// the payload it belongs to has been delivered.
	hasLast     bool
	lastCounter uint8
	lastStart   bool
	lastPayload []byte

	// waitForStart is true while recovering from a continuity conflict: packets
	// are dropped until the next payload start.
	waitForStart bool
}

// newPacketAccumulator creates a new packet queue for a single PID
func newPacketAccumulator(pid uint16, programMap *programMap) *packetAccumulator {
	return &packetAccumulator{
		pid:        pid,
		programMap: programMap,
	}
}

// add adds a new packet for this PID to the queue
func (b *packetAccumulator) add(p *Packet) (ps []*Packet) {
	if b.hasLast {
		switch c := p.Header.ContinuityCounter; {
		case c == b.lastCounter &&
			p.Header.PayloadUnitStartIndicator == b.lastStart &&
			bytes.Equal(p.Payload, b.lastPayload):
			// Exact retransmission of the previous packet on this PID: its payload
			// has already contributed once. The adaptation field is left out of the
			// comparison on purpose, an updated PCR doesn't make it a new packet.
			return
		case c == b.lastCounter:
			// Same counter but different content: continuity is broken and the
			// payload being assembled can't be trusted anymore. Void it and wait
			// for the next payload start to recover.
			b.void()
			if !p.Header.PayloadUnitStartIndicator {
				b.waitForStart = true
				return
			}
		case c != (b.lastCounter+1)&0x0f:
			// Counter gap: the payload being assembled is incomplete, void it.
			b.void()
		}
	}

	// Recovering from a continuity conflict: only a payload start resumes the assembly
	if b.waitForStart {
		if !p.Header.PayloadUnitStartIndicator {
			return
		}
		b.waitForStart = false
	}

	// Track continuity state
	b.hasLast = true
	b.lastCounter = p.Header.ContinuityCounter
	b.lastStart = p.Header.PayloadUnitStartIndicator
	b.lastPayload = p.Payload

	mps := b.q

	// Flush buffer if new payload starts here
	if p.Header.PayloadUnitStartIndicator {
		ps = mps
		mps = make([]*Packet, 0, cap(mps))
	}

	mps = append(mps, p)

	// A single call can only return one payload: if this packet already flushed the
	// previous one, keep the new payload queued instead of overwriting the result.
	if len(ps) > 0 {
		b.q = mps
		return
	}

	// Check if PSI payload is complete
	if b.programMap != nil &&
		(b.pid == PIDPAT || b.programMap.existsUnlocked(b.pid)) &&
		isPSIComplete(mps) {
		ps = mps
		mps = nil
	} else if isPESPayload(mps[0].Payload) && isPESComplete(mps) { // Check if PES payload is complete
		ps = mps
		mps = nil
	}

	b.q = mps
	return
}

// void drops the payload being assembled
func (b *packetAccumulator) void() {
	if cap(b.q) > 0 {
		b.q = b.q[:0]
	} else {
		b.q = nil
	}
}

// reset opens a new continuity interval: the payload being assembled, if any,
// is voided and continuity tracking starts over
func (b *packetAccumulator) reset() {
	b.void()
	b.hasLast = false
	b.waitForStart = false
}

// packetPool represents a queue of packets for each PID in the stream
type packetPool struct {
	// We use map[uint32] instead map[uint16] as go runtime provide optimized hash functions for (u)int32/64 keys
	b map[uint32]*packetAccumulator // Indexed by PID

	programMap *programMap
}

// newPacketPool creates a new packet pool with an optional parser and programMap
func newPacketPool(programMap *programMap) *packetPool {
	return &packetPool{
		b: make(map[uint32]*packetAccumulator),

		programMap: programMap,
	}
}

// addUnlocked adds a new packet to the pool
func (b *packetPool) addUnlocked(p *Packet) (ps []*Packet) {
	// Throw away packet if error indicator
	if p.Header.TransportErrorIndicator {
		return
	}

	// An explicit discontinuity indicator opens a new continuity interval for
	// this PID: the payload being assembled, if any, is voided and a non-start
	// payload can't be appended to it anymore
	if p.Header.HasAdaptationField && p.AdaptationField.DiscontinuityIndicator {
		if acc, ok := b.b[uint32(p.Header.PID)]; ok {
			acc.reset()
		}
	}

	// Throw away packets that don't have a payload: they don't advance the
	// payload continuity counter
	// TODO figure out what we're going to do with them :D
	if !p.Header.HasPayload {
		return
	}

	// Make sure accumulator exists
	acc, ok := b.b[uint32(p.Header.PID)]
	if !ok {
		acc = newPacketAccumulator(p.Header.PID, b.programMap)
		b.b[uint32(p.Header.PID)] = acc
	}

	// Add to the accumulator
	return acc.add(p)
}

// dumpUnlocked dumps the packet pool by looking for the first item with packets inside
func (b *packetPool) dumpUnlocked() (ps []*Packet) {
	var keys []int
	for k := range b.b {
		keys = append(keys, int(k))
	}
	sort.Ints(keys)
	for _, k := range keys {
		ps = b.b[uint32(k)].q
		delete(b.b, uint32(k))
		if len(ps) > 0 {
			return
		}
	}
	return
}
