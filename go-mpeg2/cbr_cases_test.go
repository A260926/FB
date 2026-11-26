package mpeg2

import (
	"crypto/sha256"
	"encoding/hex"
	"math/big"
)

// This file implements the four fixed entry points invoked by
// task_support/target.sh. All parsing below is independent of the muxer's
// own section/PES types.

const (
	cbrTestNumerator = uint64(188 * 8 * 27000000)
	cbrPCRMod        = (uint64(1) << 33) * 300
)

// cbrSlotPosRef is the independent big-integer reference for
// floor(n*188*8*27e6/Bitrate).
func cbrSlotPosRef(n, bitrate uint64) uint64 {
	num := new(big.Int).Mul(new(big.Int).SetUint64(n), new(big.Int).SetUint64(cbrTestNumerator))
	q := num.Quo(num, new(big.Int).SetUint64(bitrate))
	return q.Uint64()
}

type cbrParsedPkt struct {
	idx          int
	raw          []byte
	pid          uint16
	pusi         bool
	afc          uint8
	cc           uint8
	hasPCR       bool
	pcr          uint64
	pcrBase      uint64
	pcrExt       uint16
	reservedOK   bool
	payloadStart int
	payload      []byte
}

func cbrParsePackets(c *cbrChecks, data []byte) []cbrParsedPkt {
	c.Check(len(data) > 0 && len(data)%188 == 0, "output is a non-empty whole number of 188-byte packets")
	n := len(data) / 188
	pkts := make([]cbrParsedPkt, 0, n)
	for i := 0; i < n; i++ {
		base := i * 188
		b := data[base : base+188]
		c.Check(b[0] == 0x47, "sync byte 0x47")
		p := cbrParsedPkt{idx: i}
		p.raw = append([]byte(nil), b...)
		p.pid = uint16(b[1]&0x1f)<<8 | uint16(b[2])
		p.pusi = b[1]&0x40 != 0
		p.afc = (b[3] >> 4) & 0x03
		p.cc = b[3] & 0x0f
		p.payloadStart = -1
		off := 4
		if p.afc == 0x02 || p.afc == 0x03 {
			al := int(b[4])
			c.Check(al <= 183, "adaptation field length in range")
			if al >= 7 && b[5]&0x10 != 0 {
				p.hasPCR = true
				w := uint64(b[6])<<40 | uint64(b[7])<<32 | uint64(b[8])<<24 |
					uint64(b[9])<<16 | uint64(b[10])<<8 | uint64(b[11])
				p.pcrBase = w >> 15
				p.reservedOK = (w>>9)&0x3f == 0x3f
				p.pcrExt = uint16(w & 0x1ff)
				p.pcr = p.pcrBase*300 + uint64(p.pcrExt)
				c.Check(p.pcrExt < 300, "PCR extension below 300")
				c.Check(p.reservedOK, "PCR reserved bits are all ones")
			}
			off = 5 + al
		}
		if p.afc == 0x01 || p.afc == 0x03 {
			c.Check(off <= 188, "TS payload offset in packet")
			p.payloadStart = off
			p.payload = p.raw[off:]
		}
		pkts = append(pkts, p)
	}
	return pkts
}

type cbrCollector struct{ packets [][]byte }

func (col *cbrCollector) onPacket(p []byte) {
	col.packets = append(col.packets, append([]byte(nil), p...))
}

func (col *cbrCollector) bytes() []byte {
	out := make([]byte, 0, len(col.packets)*188)
	for _, p := range col.packets {
		out = append(out, p...)
	}
	return out
}

func (col *cbrCollector) count() int { return len(col.packets) }

// cbrCheckSlotFormula verifies the emitted packet count and that every
// stamped PCR equals StartPCR + its exact slot start, wrapped.
func cbrCheckSlotFormula(c *cbrChecks, m *CBRMuxer, pkts []cbrParsedPkt, bitrate, start uint64, elapsed uint64) {
	n := len(pkts)
	var expectedCount int
	for cbrSlotPosRef(uint64(expectedCount), bitrate) < elapsed {
		expectedCount++
	}
	c.Check(n == expectedCount, "packet/slot count matches floor(n*NUM/Bitrate) gate")
	c.Check(cbrSlotPosRef(uint64(n), bitrate) >= elapsed, "next slot is not yet due")

	for _, p := range pkts {
		pos := cbrSlotPosRef(uint64(p.idx), bitrate)
		c.Check(pos < elapsed, "emitted slot position is strictly below elapsed")
		if p.hasPCR {
			want := (start%cbrPCRMod + pos%cbrPCRMod) % cbrPCRMod
			c.Check(p.pcr == want, "PCR is StartPCR plus actual slot start modulo wrap")
			c.Check(p.pcrBase == want/300 && uint64(p.pcrExt) == want%300, "PCR base/extension split")
		}
	}
}

// cbrWantKind is an idle-scheduler prediction.
type cbrWantKind string

const (
	kPAT   cbrWantKind = "PAT"
	kPMT   cbrWantKind = "PMT"
	kPCR   cbrWantKind = "PCR"
	kMedia cbrWantKind = "MEDIA"
	kNull  cbrWantKind = "NULL"
)

// cbrSimIdle independently predicts idle packet kinds using the prose rules.
func cbrSimIdle(bitrate, interval uint64, n int) []cbrWantKind {
	kinds := make([]cbrWantKind, n)
	whole := cbrTestNumerator / bitrate
	rem := cbrTestNumerator % bitrate
	var idx, pos, frac, pcrLast, lastPAT uint64
	pmtPending := false
	for i := 0; i < n; i++ {
		switch idx {
		case 0:
			kinds[i] = kPAT
			lastPAT = pos
			pmtPending = true
		case 1:
			kinds[i] = kPMT
			pmtPending = false
		case 2:
			kinds[i] = kPCR
			pcrLast = pos
		default:
			switch {
			case pos-pcrLast >= interval:
				kinds[i] = kPCR
				pcrLast = pos
			case pmtPending:
				kinds[i] = kPMT
				pmtPending = false
			case pos >= lastPAT+2700000:
				kinds[i] = kPAT
				lastPAT = pos
				pmtPending = true
			default:
				kinds[i] = kNull
			}
		}
		frac += rem
		step := whole
		if frac >= bitrate {
			frac -= bitrate
			step++
		}
		pos += step
		idx++
	}
	return kinds
}

// cbrCheckCounters verifies continuity-counter semantics: ordinary packets
// advance by one mod 16; adaptation-only heartbeats do not consume the
// counter; null PID stays at zero.
func cbrCheckCounters(c *cbrChecks, pkts []cbrParsedPkt) {
	nextCC := map[uint16]uint8{}
	for _, p := range pkts {
		if p.pid == 0x1fff {
			c.Check(p.cc == 0, "null packets use continuity counter zero")
			continue
		}
		c.Check(p.cc == nextCC[p.pid], "continuity counter contiguous at actual output")
		if p.afc != 0x02 { // adaptation-only heartbeat does not advance
			nextCC[p.pid] = (nextCC[p.pid] + 1) % 16
		}
	}
}

func cbrWantKindOfPacket(p cbrParsedPkt, pcrPID uint16) cbrWantKind {
	switch {
	case p.pid == 0x0000:
		return kPAT
	case p.pid == 0x0200:
		return kPMT
	case p.pid == 0x1fff:
		return kNull
	case p.afc == 0x02 && p.pid == pcrPID && p.hasPCR:
		return kPCR
	default:
		return kMedia
	}
}

func caseCBRClock(c *cbrChecks) {
	const bitrate = uint64(1000001) // does not divide the slot numerator
	const interval = uint64(300000)
	const start = uint64(100000007) // non-zero extension

	m, err := NewCBRMuxer(CBRConfig{
		Bitrate:          bitrate,
		StartPCR27MHz:    start,
		PCRInterval27MHz: interval,
		Streams:          []TS_STREAM_TYPE{TS_STREAM_H264},
	})
	c.Check(err == nil, "construct non-divisible-bitrate muxer")
	col := &cbrCollector{}
	m.OnPacket = col.onPacket

	const elapsed = uint64(5000000)
	c.Check(m.AdvanceTo(elapsed) == nil, "advance over non-divisible slots")
	pkts := cbrParsePackets(c, col.bytes())
	cbrCheckSlotFormula(c, m, pkts, bitrate, start, elapsed)

	// Fixed opening sequence.
	c.Check(pkts[0].pid == 0x0000, "first packet is PAT")
	c.Check(pkts[1].pid == 0x0200, "second packet is PMT")
	c.Check(pkts[2].pid == 0x0100 && pkts[2].afc == 0x02 && pkts[2].hasPCR, "third packet is first PCR heartbeat")

	// Idle scheduler prediction matches every packet kind.
	kinds := cbrSimIdle(bitrate, interval, len(pkts))
	for i, k := range kinds {
		c.Check(cbrWantKindOfPacket(pkts[i], 0x0100) == k, "idle packet kind/priority matches schedule")
	}
	cbrCheckCounters(c, pkts)

	// Heartbeat count equals the independent idle prediction.
	heartbeats := 0
	for _, p := range pkts {
		if p.afc == 0x02 && p.pid == 0x0100 {
			heartbeats++
		}
	}
	wantHB := 0
	for _, k := range kinds {
		if k == kPCR {
			wantHB++
		}
	}
	c.Check(heartbeats == wantHB && heartbeats >= 2, "heartbeat count follows PCR interval")

	// Strict < gate and equal-position boundary.
	n := col.count()
	nextPos := cbrSlotPosRef(uint64(n), bitrate)
	c.Check(m.AdvanceTo(elapsed) == nil && col.count() == n, "repeated advance does not resend")
	c.Check(m.AdvanceTo(nextPos) == nil && col.count() == n, "packet at position equal to elapsed waits")
	c.Check(m.AdvanceTo(nextPos+1) == nil && col.count() == n+1, "one more tick releases exactly one packet")

	// Rollback is rejected and changes nothing; continuing afterwards works.
	before := col.count()
	c.Check(m.AdvanceTo(nextPos) != nil, "backwards AdvanceTo returns error")
	c.Check(col.count() == before, "rollback emits nothing")
	c.Check(m.AdvanceTo(nextPos+1) == nil && col.count() == before, "repeating current high-water mark is a no-op")
	c.Check(m.AdvanceTo(nextPos+1000000) == nil, "forward progress after rollback error works")
	c.Check(col.count() > before, "packets flow again after recovered error")

	// Missing callback: error, no slot consumed, clean restart from PAT.
	m2, err := NewCBRMuxer(CBRConfig{
		Bitrate:          8000000,
		PCRInterval27MHz: 540000,
		Streams:          []TS_STREAM_TYPE{TS_STREAM_AAC},
	})
	c.Check(err == nil, "construct nil-callback muxer")
	c.Check(m2.AdvanceTo(2000000) != nil, "AdvanceTo without OnPacket returns error")
	col2 := &cbrCollector{}
	m2.OnPacket = col2.onPacket
	c.Check(m2.AdvanceTo(2000000) == nil, "AdvanceTo after setting callback succeeds")
	p2 := cbrParsePackets(c, col2.bytes())
	c.Check(p2[0].pid == 0x0000, "no slot was consumed by the missing-callback error")
	cbrCheckSlotFormula(c, m2, p2, 8000000, 0, 2000000)

	// PCR wrap-around with an extension close to 299.
	m3, err := NewCBRMuxer(CBRConfig{
		Bitrate:          128000,
		StartPCR27MHz:    cbrPCRMod - 1,
		PCRInterval27MHz: 1000000,
		Streams:          []TS_STREAM_TYPE{TS_STREAM_H264},
	})
	c.Check(err == nil, "construct wrap muxer")
	col3 := &cbrCollector{}
	m3.OnPacket = col3.onPacket
	c.Check(m3.AdvanceTo(4000000) == nil, "advance across PCR wrap")
	p3 := cbrParsePackets(c, col3.bytes())
	cbrCheckSlotFormula(c, m3, p3, 128000, cbrPCRMod-1, 4000000)
	sawWrapped := false
	for _, p := range p3 {
		if p.hasPCR && p.pcr < 1000000 {
			sawWrapped = true
			c.Check(p.pcrExt == uint16(p.pcr%300), "wrapped extension consistent")
		}
	}
	c.Check(sawWrapped, "observed PCR values after 2^33*300 wrap")

	// Safe modulo even for a Start value near uint64 max.
	bigStart := ^uint64(0)
	wrapped := cbrWrapPCR(bigStart, 12345)
	ref := new(big.Int).Add(new(big.Int).SetUint64(bigStart), big.NewInt(12345))
	ref.Mod(ref, new(big.Int).SetUint64(cbrPCRMod))
	c.Check(wrapped == ref.Uint64(), "wrap helper matches big modulo without overflow")

	// Long-run recurrence never drifts from the exact floor formula.
	rm := &CBRMuxer{
		cfg:   CBRConfig{Bitrate: 1000001},
		whole: cbrTestNumerator / 1000001,
		rem:   cbrTestNumerator % 1000001,
	}
	for i := uint64(1); i <= 300000; i++ {
		rm.advanceSlot()
		if i%50000 == 0 || i == 300000 {
			c.Check(rm.pos == cbrSlotPosRef(i, 1000001), "incremental slot clock matches exact floor at distance")
		}
	}
}

func caseCBRTables(c *cbrChecks) {
	const bitrate = uint64(8000000)
	const interval = uint64(540000)
	m, err := NewCBRMuxer(CBRConfig{
		Bitrate:          bitrate,
		StartPCR27MHz:    777,
		PCRInterval27MHz: interval,
		Streams:          []TS_STREAM_TYPE{TS_STREAM_H265, TS_STREAM_AAC},
	})
	c.Check(err == nil, "construct table muxer")
	pids := m.StreamPIDs()
	c.Check(len(pids) == 2 && pids[0] == 0x100 && pids[1] == 0x101, "PIDs assigned from 0x100 in order")
	c.Check(m.pcrPID == 0x100, "first video stream is PCR PID")

	col := &cbrCollector{}
	m.OnPacket = col.onPacket
	const elapsed = uint64(9000000)
	c.Check(m.AdvanceTo(elapsed) == nil, "advance idle stream for table repetition")
	pkts := cbrParsePackets(c, col.bytes())

	kinds := cbrSimIdle(bitrate, interval, len(pkts))
	patIdx := make([]int, 0)
	pmtIdx := make([]int, 0)
	for i, k := range kinds {
		got := cbrWantKindOfPacket(pkts[i], 0x100)
		c.Check(got == k, "table/idle schedule matches")
		if got == kPAT {
			patIdx = append(patIdx, i)
		}
		if got == kPMT {
			pmtIdx = append(pmtIdx, i)
		}
	}
	c.Check(len(patIdx) >= 3 && len(pmtIdx) >= 3, "tables repeat while idle")
	cbrCheckCounters(c, pkts)

	// Repeated PAT cadence uses the actual slot start of the prior PAT.
	for j := 1; j < len(patIdx); j++ {
		prev := cbrSlotPosRef(uint64(patIdx[j-1]), bitrate)
		cur := cbrSlotPosRef(uint64(patIdx[j]), bitrate)
		c.Check(cur >= prev+2700000, "new PAT at or after prior PAT slot plus 2700000")
		// No slot strictly between the threshold and this PAT that could
		// have carried a PAT under the simulator.
		c.Check(kinds[patIdx[j]] == kPAT, "PAT index predicted")
	}

	// Every heartbeat on the PCR PID keeps continuity counter zero in idle.
	for _, p := range pkts {
		if p.afc == 0x02 {
			c.Check(p.cc == 0, "idle heartbeats do not advance PCR PID CC")
		}
	}

	// Independent section decoding of the first PAT.
	pat := pkts[0]
	c.Check(pat.pusi && pat.payloadStart == 4, "PAT starts PES/section at payload boundary")
	sec := pat.payload[1:] // skip pointer_field
	c.Check(sec[0] == 0x00, "PAT table id")
	patLen := int(uint16(sec[1]&0x0f)<<8 | uint16(sec[2]))
	c.Check(patLen >= 13, "PAT section length sane")
	c.Check((sec[5]>>1)&0x1f == 0, "PAT version 0")
	prog := uint16(sec[8])<<8 | uint16(sec[9])
	pmtPID := uint16(sec[10]&0x1f)<<8 | uint16(sec[11])
	c.Check(prog == 1 && pmtPID == 0x200, "PAT carries program 1 / PMT 0x200")

	// Independent PMT decoding.
	var pmtPkt *cbrParsedPkt
	for i := range pkts {
		if pkts[i].pid == 0x200 {
			pmtPkt = &pkts[i]
			break
		}
	}
	c.Check(pmtPkt != nil && pmtPkt.pusi, "PMT packet present with PUSI")
	psec := pmtPkt.payload[1:]
	c.Check(psec[0] == 0x02, "PMT table id")
	c.Check((psec[5]>>1)&0x1f == 0, "PMT version 0")
	pcrPID := uint16(psec[8]&0x1f)<<8 | uint16(psec[9])
	c.Check(pcrPID == 0x100, "PMT PCR PID is first video PID")
	pil := uint16(psec[10]&0x0f)<<8 | uint16(psec[11])
	secLen := int(uint16(psec[1]&0x0f)<<8 | uint16(psec[2]))
	crcEnd := 3 + secLen - 4 // relative to psec start
	c.Check(crcEnd > 12 && crcEnd <= len(psec), "PMT section length fits packet")
	k := 12 + int(pil)
	seen := map[uint16]uint8{}
	for k+5 <= crcEnd {
		st := psec[k]
		epid := uint16(psec[k+1]&0x1f)<<8 | uint16(psec[k+2])
		eil := uint16(psec[k+3]&0x0f)<<8 | uint16(psec[k+4])
		seen[epid] = st
		k += 5 + int(eil)
	}
	c.Check(k == crcEnd, "PMT elementary entries walk exactly to CRC")
	c.Check(seen[0x100] == uint8(TS_STREAM_H265), "PMT maps 0x100 to H265")
	c.Check(seen[0x101] == uint8(TS_STREAM_AAC), "PMT maps 0x101 to AAC")

	// StreamPIDs returns an independent slice.
	a := m.StreamPIDs()
	a[0] = 0xeeee
	b := m.StreamPIDs()
	c.Check(b[0] == 0x100, "StreamPIDs returns an independent copy")
}

type cbrPESMsg struct {
	pid      uint16
	streamID uint8
	pts, dts uint64
	hasPTS   bool
	hasDTS   bool
	payload  []byte
}

func cbrDecodeTime(p []byte) uint64 {
	return (uint64(p[0]&0x0e) >> 1 << 30) |
		uint64(p[1])<<22 |
		(uint64(p[2]&0xfe) >> 1 << 15) |
		uint64(p[3])<<7 |
		(uint64(p[4]) >> 1)
}

func cbrAssemblePES(c *cbrChecks, pkts []cbrParsedPkt, pid uint16) []cbrPESMsg {
	var msgs []cbrPESMsg
	var cur []byte
	flush := func() {
		if len(cur) < 9 {
			return
		}
		c.Check(cur[0] == 0x00 && cur[1] == 0x00 && cur[2] == 0x01, "PES start code prefix")
		hdrLen := int(cur[8])
		c.Check(9+hdrLen <= len(cur), "PES header length fits")
		msg := cbrPESMsg{pid: pid, streamID: cur[3]}
		flags := cur[7] >> 6
		if flags&0x02 != 0 {
			msg.hasPTS = true
			msg.pts = cbrDecodeTime(cur[9:14])
		}
		if flags == 0x03 {
			msg.hasDTS = true
			msg.dts = cbrDecodeTime(cur[14:19])
		} else {
			msg.dts = msg.pts
		}
		msg.payload = append([]byte(nil), cur[9+hdrLen:]...)
		msgs = append(msgs, msg)
	}
	for _, p := range pkts {
		if p.pid != pid {
			continue
		}
		if p.pusi {
			if cur != nil {
				flush()
			}
			cur = nil
		}
		if p.payloadStart >= 0 && (p.pusi || cur != nil) {
			cur = append(cur, p.payload...)
		}
	}
	if cur != nil {
		flush()
	}
	return msgs
}

// cbrFeedMedia writes the same access units into m.
func cbrFeedMedia(c *cbrChecks, m *CBRMuxer, video, audio []byte) {
	pids := m.StreamPIDs()
	c.Check(m.Write(pids[0], audio, 1000, 1000) == nil, "write audio first")
	c.Check(m.Write(pids[1], video, 2080, 2040) == nil, "write video second")
}

// cbrFlushEnd advances in odd increments until the media queue drains and
// returns the final elapsed high-water mark.
func cbrFlushEnd(c *cbrChecks, m *CBRMuxer, col *cbrCollector) uint64 {
	m.OnPacket = col.onPacket
	var elapsed uint64
	for m.PendingPackets() > 0 || elapsed == 0 {
		elapsed += 250000
		c.Check(m.AdvanceTo(elapsed) == nil, "incremental flush advances")
		if elapsed > 200000000 {
			c.Check(false, "flush bounded")
			return elapsed
		}
	}
	return elapsed
}

func caseCBRMedia(c *cbrChecks) {
	const bitrate = uint64(128000)
	const interval = uint64(1000000)

	audio := []byte{0xff, 0xf1, 0x4c, 0x80, 0x01, 0x7f, 0xfc, 0x11}
	video := make([]byte, 3000)
	copy(video, []byte{0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21})
	for i := 8; i < len(video); i++ {
		video[i] = 0x55
	}

	newMediaMuxer := func() *CBRMuxer {
		m, err := NewCBRMuxer(CBRConfig{
			Bitrate:          bitrate,
			StartPCR27MHz:    42,
			PCRInterval27MHz: interval,
			Streams:          []TS_STREAM_TYPE{TS_STREAM_AAC, TS_STREAM_H264},
		})
		c.Check(err == nil, "construct media muxer")
		return m
	}

	// Write only enqueues: it must not touch the user callback, and a tiny
	// AAC frame packetizes into exactly one pending TS packet.
	m0 := newMediaMuxer()
	userCalls := 0
	m0.OnPacket = func([]byte) { userCalls++ }
	c.Check(m0.Write(m0.StreamPIDs()[0], audio, 1000, 1000) == nil, "write tiny AAC")
	c.Check(userCalls == 0, "Write does not invoke the user callback")
	c.Check(m0.PendingPackets() == 1, "tiny AAC frame is one pending TS packet")

	// Reference run with fine-grained advancing.
	m1 := newMediaMuxer()
	cbrFeedMedia(c, m1, video, audio)
	pids := m1.StreamPIDs()
	c.Check(pids[0] == 0x100 && pids[1] == 0x101, "audio then video PID order")
	c.Check(m1.pcrPID == 0x101, "video PID chosen as PCR even though audio written first")
	pendingAfterWrite := m1.PendingPackets()
	c.Check(pendingAfterWrite >= 10, "PendingPackets counts packetized media TS packets")
	col1 := &cbrCollector{}
	end := cbrFlushEnd(c, m1, col1)
	c.Check(m1.PendingPackets() == 0, "all pending media emitted")

	pkts1 := cbrParsePackets(c, col1.bytes())
	cbrCheckCounters(c, pkts1)

	// The number of emitted media PID packets equals the original pending
	// count (pending counted both PIDs, tables/heartbeats excluded).
	mediaPktCount := 0
	for _, p := range pkts1 {
		if p.pid == 0x100 || p.pid == 0x101 {
			// Heartbeats are adaptation-only and were never queued.
			if p.afc != 0x02 {
				mediaPktCount++
			}
		}
	}
	c.Check(mediaPktCount == pendingAfterWrite, "every pending media packet was sent exactly once")

	// One-shot advance to the same endpoint.
	m2 := newMediaMuxer()
	cbrFeedMedia(c, m2, video, audio)
	col2 := &cbrCollector{}
	m2.OnPacket = col2.onPacket
	c.Check(m2.AdvanceTo(end) == nil, "one-shot advance")
	c.Check(m2.PendingPackets() == 0, "one-shot drains media")
	c.Check(hex.EncodeToString(col2.bytes()) == hex.EncodeToString(col1.bytes()),
		"fine-grained and one-shot advances produce identical bytes")

	// Weird split boundaries, including duplicates and zero.
	m3 := newMediaMuxer()
	cbrFeedMedia(c, m3, video, audio)
	col3 := &cbrCollector{}
	m3.OnPacket = col3.onPacket
	cuts := []uint64{0, 1, 2, 317249, 317250, 317251, end / 3, end / 2, end - 317250, end - 1, end, end}
	var last uint64
	for _, e := range cuts {
		if e < last {
			e = last
		}
		c.Check(m3.AdvanceTo(e) == nil, "split advance segment")
		last = e
	}
	c.Check(m3.PendingPackets() == 0, "split run drains media")
	c.Check(hex.EncodeToString(col3.bytes()) == hex.EncodeToString(col1.bytes()),
		"arbitrary AdvanceTo splits yield identical complete byte stream")

	// Reassemble and verify PES bodies and timestamps.
	pkts := cbrParsePackets(c, col2.bytes())
	audios := cbrAssemblePES(c, pkts, 0x100)
	videos := cbrAssemblePES(c, pkts, 0x101)
	c.Check(len(audios) == 1 && len(videos) == 1, "one PES per written access unit")
	a := audios[0]
	c.Check(a.streamID == uint8(PES_STREAM_AUDIO), "audio PES stream id")
	c.Check(a.hasPTS && a.pts == 1000*90, "audio PTS in 90kHz")
	c.Check(a.hasDTS && a.dts == 1000*90, "audio DTS in 90kHz")
	c.Check(len(a.payload) == len(audio) && string(a.payload) == string(audio), "AAC body preserved")

	v := videos[0]
	c.Check(v.streamID == uint8(PES_STREAM_VIDEO), "video PES stream id")
	c.Check(v.hasPTS && v.pts == 2080*90, "video PTS in 90kHz")
	c.Check(v.hasDTS && v.dts == 2040*90, "video DTS in 90kHz")
	wantVideo := append(append([]byte(nil), H264_AUD_NALU...), video...)
	c.Check(len(v.payload) == len(wantVideo), "video payload length includes injected AUD")
	c.Check(string(v.payload) == string(wantVideo), "H264 AUD behaviour and full multi-packet body preserved")
	c.Check(len(packetsForPID(pkts, 0x101)) >= 5, "video PES spans multiple TS packets")

	// Input buffer reuse after Write returns.
	m4 := newMediaMuxer()
	pidV := m4.StreamPIDs()[1]
	buf := make([]byte, 900)
	copy(buf, []byte{0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21})
	for i := 8; i < len(buf); i++ {
		buf[i] = 0x6a
	}
	saved := append([]byte(nil), buf...)
	c.Check(m4.Write(pidV, buf, 3000, 3000) == nil, "write reusable buffer")
	for i := range buf {
		buf[i] = 0x00 // overwrite the caller buffer completely
	}
	col4 := &cbrCollector{}
	end4 := cbrFlushEnd(c, m4, col4)
	_ = end4
	p4 := cbrParsePackets(c, col4.bytes())
	v4 := cbrAssemblePES(c, p4, pidV)
	c.Check(len(v4) == 1, "reuse test PES assembled")
	want4 := append(append([]byte(nil), H264_AUD_NALU...), saved...)
	c.Check(string(v4[0].payload) == string(want4), "Write copies input so buffer reuse cannot mutate queued media")
}

func packetsForPID(pkts []cbrParsedPkt, pid uint16) []cbrParsedPkt {
	var out []cbrParsedPkt
	for _, p := range pkts {
		if p.pid == pid {
			out = append(out, p)
		}
	}
	return out
}

func caseCBRCompatibility(c *cbrChecks) {
	// --- Legacy TSMuxer output must be byte-for-byte unchanged. ---
	x := NewTSMuxer()
	v := x.AddStream(TS_STREAM_H264)
	a := x.AddStream(TS_STREAM_AAC)
	var legacy [][]byte
	x.OnPacket = func(p []byte) {
		legacy = append(legacy, append([]byte(nil), p...))
	}
	video := make([]byte, 700)
	copy(video, []byte{0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21})
	for i := 8; i < len(video); i++ {
		video[i] = 0x55
	}
	c.Check(x.Write(v, video, 1000, 960) == nil, "legacy write video 1")
	c.Check(x.Write(a, []byte{0xff, 0xf1, 0x4c, 0x80, 1, 0x7f, 0xfc, 0x11}, 1000, 1000) == nil, "legacy write audio")
	c.Check(x.Write(v, video, 2080, 2040) == nil, "legacy write video 2")
	var all []byte
	pcrCount := 0
	for _, p := range legacy {
		c.Check(len(p) == 188 && p[0] == 0x47, "legacy packet shape")
		all = append(all, p...)
		if p[3]&0x20 != 0 && p[4] >= 7 && p[5]&0x10 != 0 {
			pcrCount++
		}
	}
	sum := sha256.Sum256(all)
	c.Check(hex.EncodeToString(sum[:]) == "9758f331ae6622970f077301639af0f0a34f229e43232edddee3ed695101f5a6",
		"legacy TSMuxer byte stream golden hash preserved")
	c.Check(len(legacy) == 13 && pcrCount == 2, "legacy packet/PCR counts preserved")

	// --- Constructor validation. ---
	valid := CBRConfig{Bitrate: 128000, PCRInterval27MHz: 951750, Streams: []TS_STREAM_TYPE{TS_STREAM_AAC}}
	_, err := NewCBRMuxer(valid)
	c.Check(err == nil, "minimum bitrate and exact three-slot interval accepted")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 128000, PCRInterval27MHz: 951749, Streams: valid.Streams})
	c.Check(err != nil, "interval shorter than three slots rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 128000, PCRInterval27MHz: 1080000, Streams: valid.Streams})
	c.Check(err == nil, "maximum PCR interval accepted")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 127999, PCRInterval27MHz: 1080000, Streams: valid.Streams})
	c.Check(err != nil, "bitrate below minimum rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 1000000001, PCRInterval27MHz: 1080000, Streams: valid.Streams})
	c.Check(err != nil, "bitrate above maximum rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 500000, PCRInterval27MHz: 1080001, Streams: valid.Streams})
	c.Check(err != nil, "PCR interval above maximum rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 500000, PCRInterval27MHz: 10, Streams: valid.Streams})
	c.Check(err != nil, "PCR interval too small rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 500000, PCRInterval27MHz: 100000})
	c.Check(err != nil, "empty stream list rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 500000, PCRInterval27MHz: 100000,
		Streams: []TS_STREAM_TYPE{}})
	c.Check(err != nil, "zero streams rejected")
	nine := make([]TS_STREAM_TYPE, 9)
	for i := range nine {
		nine[i] = TS_STREAM_AAC
	}
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 500000, PCRInterval27MHz: 100000, Streams: nine})
	c.Check(err != nil, "nine streams rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 500000, PCRInterval27MHz: 100000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_TYPE(0x77)}})
	c.Check(err != nil, "unsupported stream type rejected")

	eight := make([]TS_STREAM_TYPE, 8)
	for i := range eight {
		eight[i] = TS_STREAM_H264
	}
	m8, err := NewCBRMuxer(CBRConfig{Bitrate: 500000, PCRInterval27MHz: 300000, Streams: eight})
	c.Check(err == nil, "eight streams accepted")
	got := m8.StreamPIDs()
	c.Check(len(got) == 8, "eight PIDs returned")
	for i, pid := range got {
		c.Check(pid == 0x100+uint16(i), "PID list is 0x100 upward in order")
	}

	// No-video topology falls back to first audio for PCR PID.
	ma, err := NewCBRMuxer(CBRConfig{Bitrate: 500000, PCRInterval27MHz: 300000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_AAC, TS_STREAM_AAC}})
	c.Check(err == nil && ma.pcrPID == 0x100, "no video means first audio is PCR PID")

	// Unknown PID write is an error.
	c.Check(ma.Write(0x0123, []byte{1, 2, 3}, 0, 0) != nil, "write to unknown PID rejected")

	// Backwards elapsed errors without advancing; recovery continues.
	mr, err := NewCBRMuxer(CBRConfig{Bitrate: 1000000, PCRInterval27MHz: 200000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_H264}})
	c.Check(err == nil, "construct recovery muxer")
	rc := &cbrCollector{}
	mr.OnPacket = rc.onPacket
	c.Check(mr.AdvanceTo(900000) == nil, "first advance ok")
	c.Check(rc.count() > 0, "packets emitted")
	first := rc.bytes()
	c.Check(mr.AdvanceTo(100) != nil, "rollback rejected")
	c.Check(rc.count() == len(first)/188, "rollback did not emit")
	c.Check(mr.AdvanceTo(900000) == nil, "same high-water mark after error is ok")
	c.Check(rc.count() == len(first)/188, "no duplicate after recovered no-op")
	c.Check(mr.AdvanceTo(2000000) == nil, "further advance after error ok")
	c.Check(rc.count() > len(first)/188, "progress continues after error")
}
