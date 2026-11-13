package mpeg2

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"testing"
)

func sha256Hex(b []byte) string {
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:])
}

// cbrParsedPacket is an independent decode of one 188-byte transport packet,
// written only with the standard library so the tests do not trust the
// muxer's own encoder structures.
type cbrParsedPacket struct {
	offset  int
	pusi    bool
	pid     uint16
	afc     uint8
	cc      uint8
	afLen   int
	afFlags byte
	hasPCR  bool
	pcrBase uint64
	pcrExt  uint16
	payload []byte
}

func cbrParsePacket(t *testing.T, raw []byte, offset int) cbrParsedPacket {
	p := cbrParsedPacket{offset: offset, afLen: -1}
	if len(raw) != TS_PAKCET_SIZE {
		t.Fatalf("packet at %d has %d bytes, want 188", offset, len(raw))
	}
	if raw[0] != 0x47 {
		t.Fatalf("packet at %d missing sync byte", offset)
	}
	p.pusi = raw[1]&0x40 != 0
	p.pid = uint16(raw[1]&0x1f)<<8 | uint16(raw[2])
	p.afc = raw[3] >> 4 & 0x03
	p.cc = raw[3] & 0x0f
	// Null packets carry no structured adaptation field or payload.
	if p.pid == cbrNullPID {
		return p
	}
	pos := 4
	if p.afc == 2 || p.afc == 3 {
		p.afLen = int(raw[4])
		if p.afLen > 0 {
			p.afFlags = raw[5]
			if p.afFlags&0x10 != 0 {
				if p.afLen < 7 {
					t.Fatalf("PCR flag set but adaptation field too short at %d", offset)
				}
				p.hasPCR = true
				b := raw[6:12]
				p.pcrBase = uint64(b[0])<<25 | uint64(b[1])<<17 | uint64(b[2])<<9 |
					uint64(b[3])<<1 | uint64(b[4]>>7)
				p.pcrExt = uint16(b[4]&0x01)<<8 | uint16(b[5])
				// The six reserved bits around base/extension must all be 1.
				if b[4]&0x7e != 0x7e {
					t.Fatalf("PCR reserved bits not all set at %d: %02x", offset, b[4])
				}
			}
		}
		pos = 4 + 1 + p.afLen
	}
	if p.afc == 1 || p.afc == 3 {
		p.payload = raw[pos:]
	}
	return p
}

func cbrPCRValue(p cbrParsedPacket) uint64 {
	return p.pcrBase*300 + uint64(p.pcrExt)
}

// cbrCollector copies every emitted packet because the callback buffer is
// only valid for the duration of the call.
type cbrCollector struct {
	packets [][]byte
}

func (c *cbrCollector) attach(mux *CBRMuxer) {
	mux.OnPacket = func(pkg []byte) {
		cp := make([]byte, len(pkg))
		copy(cp, pkg)
		c.packets = append(c.packets, cp)
	}
}

func (c *cbrCollector) parsed(t *testing.T) []cbrParsedPacket {
	out := make([]cbrParsedPacket, len(c.packets))
	for i, raw := range c.packets {
		out[i] = cbrParsePacket(t, raw, i)
	}
	return out
}

func (c *cbrCollector) allBytes() []byte {
	var buf bytes.Buffer
	for _, p := range c.packets {
		buf.Write(p)
	}
	return buf.Bytes()
}

// cbrCounter is a storage-light sink for long idle runs: it keeps only the
// events the assertions need instead of every 188-byte packet.
type cbrCounter struct {
	total      int
	nulls      int
	heartbeats []int // slot indices carrying adaptation-only PCR packets
	mediaPCR   []int
	patSlots   []int
	pmtSlots   []int
	ccByPID    map[uint16][]uint8
	pidSeq     []uint16
}

func newCBRCounter() *cbrCounter {
	return &cbrCounter{ccByPID: make(map[uint16][]uint8)}
}

func (c *cbrCounter) attach(mux *CBRMuxer) {
	mux.OnPacket = func(pkg []byte) {
		slot := c.total
		c.total++
		pid := uint16(pkg[1]&0x1f)<<8 | uint16(pkg[2])
		afc := pkg[3] >> 4 & 0x03
		cc := pkg[3] & 0x0f
		c.pidSeq = append(c.pidSeq, pid)
		c.ccByPID[pid] = append(c.ccByPID[pid], cc)
		switch {
		case pid == cbrNullPID:
			c.nulls++
		case pid == 0:
			c.patSlots = append(c.patSlots, slot)
		case pid == 0x200:
			c.pmtSlots = append(c.pmtSlots, slot)
		case afc == 2:
			c.heartbeats = append(c.heartbeats, slot)
		case afc == 3 && pkg[4] > 0 && pkg[5]&0x10 != 0:
			c.mediaPCR = append(c.mediaPCR, slot)
		}
	}
}

// cbrReassembledPES is one PES rebuilt from a sequence of media TS packets.
type cbrReassembledPES struct {
	pid         uint16
	streamID    byte
	ptsDTSFlags byte
	pts, dts    uint64
	body        []byte
}

func cbrDecodeTime(b []byte) uint64 {
	return uint64(b[0]&0x0e)<<29 | uint64(b[1])<<22 | uint64(b[2]&0xfe)<<14 |
		uint64(b[3])<<7 | uint64(b[4]>>1)
}

// cbrReassemble walks every media PID independently and rebuilds PES packets.
func cbrReassemble(t *testing.T, packets []cbrParsedPacket) []cbrReassembledPES {
	var result []cbrReassembledPES
	current := map[uint16]*cbrReassembledPES{}
	order := []uint16{}
	flush := func(pid uint16) {
		if pes := current[pid]; pes != nil {
			result = append(result, *pes)
			delete(current, pid)
		}
	}
	for _, p := range packets {
		if p.pid == 0 || p.pid == 0x200 || p.pid == cbrNullPID || len(p.payload) == 0 {
			continue
		}
		if p.pusi {
			flush(p.pid)
			payload := p.payload
			if len(payload) < 9 || payload[0] != 0 || payload[1] != 0 || payload[2] != 1 {
				t.Fatalf("PES on pid %d missing start code", p.pid)
			}
			pes := &cbrReassembledPES{pid: p.pid, streamID: payload[3], ptsDTSFlags: payload[7] >> 6}
			hdrLen := int(payload[8])
			body := append([]byte(nil), payload[9+hdrLen:]...)
			pes.body = body
			if pes.ptsDTSFlags&0x02 != 0 {
				pes.pts = cbrDecodeTime(payload[9:])
			}
			if pes.ptsDTSFlags&0x01 != 0 {
				pes.dts = cbrDecodeTime(payload[14:])
			} else {
				pes.dts = pes.pts
			}
			current[p.pid] = pes
			order = append(order, p.pid)
		} else if pes := current[p.pid]; pes != nil {
			pes.body = append(pes.body, p.payload...)
		}
	}
	for _, pid := range order {
		flush(pid)
	}
	return result
}

func cbrSlotPosition(n, bitrate uint64) uint64 {
	return n * cbrSlotNumerator / bitrate
}

func cbrExpectedSlotCount(elapsed, bitrate uint64) uint64 {
	// Smallest N with floor(N*K/bitrate) >= elapsed; emitted count is N.
	lo, hi := uint64(0), uint64(1)
	for cbrSlotPosition(hi, bitrate) < elapsed {
		hi *= 2
	}
	for lo < hi {
		mid := lo + (hi-lo)/2
		if cbrSlotPosition(mid, bitrate) < elapsed {
			lo = mid + 1
		} else {
			hi = mid
		}
	}
	return lo
}

// ---------------------------------------------------------------------------
// TestCBRClock: slot scheduling, exact PCR values, wrap and error handling.
// ---------------------------------------------------------------------------
func caseCBRClock(c *cbrChecks) {
	const bitrate uint64 = 1000007 // deliberately does not divide the slot numerator
	const startPCR uint64 = 5000000
	mux, err := NewCBRMuxer(CBRConfig{
		Bitrate: bitrate, StartPCR27MHz: startPCR, PCRInterval27MHz: 270000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_H264, TS_STREAM_AAC},
	})
	c.Check(err == nil, "valid config must construct")
	pids := mux.StreamPIDs()
	c.Check(len(pids) == 2 && pids[0] == 0x100 && pids[1] == 0x101, "pids assigned from 0x100 in order")
	pids[0] = 0xffff
	c.Check(mux.StreamPIDs()[0] == 0x100, "StreamPIDs must return an independent slice")

	// Configuration validation.
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 127999, PCRInterval27MHz: 270000, Streams: []TS_STREAM_TYPE{TS_STREAM_AAC}})
	c.Check(err != nil, "bitrate below floor rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 1000000001, PCRInterval27MHz: 270000, Streams: []TS_STREAM_TYPE{TS_STREAM_AAC}})
	c.Check(err != nil, "bitrate above ceiling rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: bitrate, PCRInterval27MHz: 270000})
	c.Check(err != nil, "empty stream list rejected")
	many := make([]TS_STREAM_TYPE, 9)
	for i := range many {
		many[i] = TS_STREAM_AAC
	}
	_, err = NewCBRMuxer(CBRConfig{Bitrate: bitrate, PCRInterval27MHz: 270000, Streams: many})
	c.Check(err != nil, "nine streams rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: bitrate, PCRInterval27MHz: 270000, Streams: []TS_STREAM_TYPE{TS_STREAM_AUDIO_MPEG1}})
	c.Check(err != nil, "unsupported stream type rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: bitrate, PCRInterval27MHz: 1080001, Streams: []TS_STREAM_TYPE{TS_STREAM_AAC}})
	c.Check(err != nil, "PCR interval over 100 ms rejected")
	_, err = NewCBRMuxer(CBRConfig{Bitrate: 1000000000, PCRInterval27MHz: 119, Streams: []TS_STREAM_TYPE{TS_STREAM_AAC}})
	c.Check(err != nil, "PCR interval smaller than three slots rejected")
	audioOnly, err := NewCBRMuxer(CBRConfig{Bitrate: 1000000000, PCRInterval27MHz: 120, Streams: []TS_STREAM_TYPE{TS_STREAM_AAC}})
	c.Check(err == nil && audioOnly.pcrPID == 0x100, "interval of exactly three slots accepted; audio-only PCR pid is first audio")
	edgeLow, err := NewCBRMuxer(CBRConfig{Bitrate: 128000, PCRInterval27MHz: 1080000, Streams: []TS_STREAM_TYPE{TS_STREAM_H264}})
	c.Check(err == nil && edgeLow.pcrPID == 0x100, "bitrate/interval boundary values accepted")

	// No callback: AdvanceTo must fail without advancing.
	c.Check(mux.AdvanceTo(100000) != nil, "advance without OnPacket rejected")

	var col cbrCollector
	col.attach(mux)

	c.Check(mux.AdvanceTo(0) == nil && len(col.packets) == 0, "elapsed 0 emits nothing")

	const elapsed1 uint64 = 300000
	c.Check(mux.AdvanceTo(elapsed1) == nil, "first advance")
	wantCount := cbrExpectedSlotCount(elapsed1, bitrate)
	c.Check(uint64(len(col.packets)) == wantCount, "non-dividing bitrate slot count matches exact formula")
	parsed := col.parsed(c.t)
	c.Check(parsed[0].pid == 0 && parsed[0].pusi, "slot 0 is PAT")
	c.Check(parsed[1].pid == 0x200 && parsed[1].pusi, "slot 1 is PMT")
	hb := parsed[2]
	c.Check(hb.pid == 0x100 && hb.afc == 2 && hb.hasPCR, "slot 2 is adaptation-only PCR heartbeat")
	wantPCR := (startPCR + cbrSlotPosition(2, bitrate)) % cbrPCRMod
	c.Check(cbrPCRValue(hb) == wantPCR, "heartbeat PCR equals start PCR plus slot position")
	c.Check(hb.pcrExt == uint16(wantPCR%300) && hb.pcrBase == wantPCR/300, "PCR base and extension split correctly")

	// Strict inequality: a slot whose position equals elapsed stays unsent.
	// Use a fresh muxer so the boundary slots have not been emitted yet.
	boundMux, err := NewCBRMuxer(CBRConfig{
		Bitrate: bitrate, StartPCR27MHz: startPCR, PCRInterval27MHz: 270000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_H264, TS_STREAM_AAC},
	})
	c.Check(err == nil, "boundary muxer constructed")
	var boundCol cbrCollector
	boundCol.attach(boundMux)
	boundary := cbrSlotPosition(5, bitrate)
	c.Check(boundMux.AdvanceTo(boundary) == nil, "advance to an exact slot position")
	c.Check(len(boundCol.packets) == 5, "packets at positions below elapsed are sent, the equal one is not")
	c.Check(boundMux.AdvanceTo(boundary+1) == nil, "one more tick releases the deferred packet")
	c.Check(len(boundCol.packets) == 6, "exactly one packet released")
	c.Check(boundMux.AdvanceTo(boundary+1) == nil && len(boundCol.packets) == 6, "repeated advance does not resend")

	// PCR wrap-around: start close to the 2^33*300 modulus.
	wrapMux, err := NewCBRMuxer(CBRConfig{
		Bitrate: bitrate, StartPCR27MHz: cbrPCRMod - 30, PCRInterval27MHz: 270000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_H264},
	})
	c.Check(err == nil, "wrap muxer constructed")
	var wrapCol cbrCollector
	wrapCol.attach(wrapMux)
	c.Check(wrapMux.AdvanceTo(300000) == nil, "wrap advance")
	wrapParsed := wrapCol.parsed(c.t)
	wrapHB := wrapParsed[2]
	wantWrap := (cbrPCRMod - 30 + cbrSlotPosition(2, bitrate)) % cbrPCRMod
	c.Check(wantWrap < cbrSlotPosition(2, bitrate), "test really crosses the modulus")
	c.Check(cbrPCRValue(wrapHB) == wantWrap && wrapHB.pcrBase == wantWrap/300, "PCR wraps at 2^33*300 with correct base/ext")

	// Backwards time is rejected and changes nothing, then progress resumes.
	countBefore := len(boundCol.packets)
	c.Check(boundMux.AdvanceTo(boundary) != nil, "backwards advance rejected")
	c.Check(len(boundCol.packets) == countBefore, "failed backwards advance emits nothing")
	c.Check(boundMux.AdvanceTo(boundary+1) == nil && len(boundCol.packets) == countBefore, "muxer keeps working after an error")

	// Long idle run: heartbeat spacing and heartbeat continuity counters.
	idleMux, err := NewCBRMuxer(CBRConfig{
		Bitrate: bitrate, StartPCR27MHz: startPCR, PCRInterval27MHz: 270000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_H264, TS_STREAM_AAC},
	})
	c.Check(err == nil, "idle muxer constructed")
	counter := newCBRCounter()
	counter.attach(idleMux)
	c.Check(idleMux.AdvanceTo(10800000) == nil, "400 ms idle advance")
	c.Check(uint64(counter.total) == cbrExpectedSlotCount(10800000, bitrate), "idle run fills every slot")
	c.Check(len(counter.heartbeats) >= 10, "heartbeats repeat while idle")
	c.Check(counter.heartbeats[0] == 2, "first heartbeat occupies slot 2")
	slotTickFloor := cbrSlotNumerator / bitrate
	for i := 1; i < len(counter.heartbeats); i++ {
		gap := cbrSlotPosition(uint64(counter.heartbeats[i]), bitrate) -
			cbrSlotPosition(uint64(counter.heartbeats[i-1]), bitrate)
		c.Check(gap >= 270000 && gap <= 270000+slotTickFloor+1, "heartbeat gap stays within one interval plus one slot")
	}
	videoCC := counter.ccByPID[0x100]
	c.Check(len(videoCC) == len(counter.heartbeats), "PCR PID only emits heartbeats while no video is queued")
	c.Check(videoCC[0] == 0 && videoCC[0] == videoCC[len(videoCC)-1], "heartbeats do not consume the payload continuity counter")
	c.Check(counter.nulls > 0, "empty media queue is padded with null packets")
}

// ---------------------------------------------------------------------------
// TestCBRTables: PAT/PMT content, repetition timing and continuity counters.
// ---------------------------------------------------------------------------
func caseCBRTables(c *cbrChecks) {
	const bitrate uint64 = 3000001
	mux, err := NewCBRMuxer(CBRConfig{
		Bitrate: bitrate, StartPCR27MHz: 0, PCRInterval27MHz: 270000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_H264, TS_STREAM_H265, TS_STREAM_AAC},
	})
	c.Check(err == nil, "table muxer constructed")
	counter := newCBRCounter()
	counter.attach(mux)
	c.Check(mux.AdvanceTo(8100000) == nil, "300 ms advance")
	c.Check(counter.patSlots[0] == 0 && counter.pmtSlots[0] == 1, "stream opens PAT then PMT")
	c.Check(len(counter.patSlots) >= 3 && len(counter.patSlots) == len(counter.pmtSlots), "tables repeat while idle, PAT and PMT counts match")

	// Every PAT is completed by a PMT before the next PAT starts.
	for i, pat := range counter.patSlots {
		pmt := counter.pmtSlots[i]
		c.Check(pmt > pat, "PMT follows its PAT")
		if i+1 < len(counter.patSlots) {
			c.Check(pmt < counter.patSlots[i+1], "PMT completes before the next PAT group")
		}
	}
	// Groups are scheduled 2,700,000 ticks after the previous PAT slot.
	slotFloor := cbrSlotNumerator / bitrate
	for i := 1; i < len(counter.patSlots); i++ {
		gap := cbrSlotPosition(uint64(counter.patSlots[i]), bitrate) -
			cbrSlotPosition(uint64(counter.patSlots[i-1]), bitrate)
		c.Check(gap >= 2700000 && gap < 2700000+uint64(slotFloor)+1, "PAT repetition measured from previous PAT slot")
	}

	// Re-render and decode table bytes independently.
	var col cbrCollector
	mux2, err := NewCBRMuxer(CBRConfig{
		Bitrate: bitrate, PCRInterval27MHz: 270000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_H264, TS_STREAM_H265, TS_STREAM_AAC},
	})
	c.Check(err == nil, "second table muxer constructed")
	col.attach(mux2)
	c.Check(mux2.AdvanceTo(cbrSlotPosition(3, bitrate)+1) == nil, "advance through opening tables")
	raw := col.packets
	pat := raw[0]
	c.Check(pat[4] == 0x00 && pat[5] == 0x00, "PAT pointer field and table id")
	patVersion := pat[10] >> 1 & 0x1f
	c.Check(patVersion == 0, "PAT version stays 0")
	prog := uint16(pat[13])<<8 | uint16(pat[14])
	patPid := uint16(pat[15]&0x1f)<<8 | uint16(pat[16])
	c.Check(prog == 1 && patPid == 0x200, "PAT maps program 1 to PMT PID 0x200")

	pmt := raw[1]
	c.Check(pmt[4] == 0x00 && pmt[5] == 0x02, "PMT pointer field and table id")
	pmtVersion := pmt[10] >> 1 & 0x1f
	c.Check(pmtVersion == 0, "PMT version stays 0")
	pmtPCRPid := uint16(pmt[13]&0x1f)<<8 | uint16(pmt[14])
	c.Check(pmtPCRPid == 0x100, "first video stream is PCR PID")
	infoLen := uint16(pmt[15]&0x0f)<<8 | uint16(pmt[16])
	c.Check(infoLen == 0, "no program info descriptors")
	wantTypes := []byte{byte(TS_STREAM_H264), byte(TS_STREAM_H265), byte(TS_STREAM_AAC)}
	for i, st := range wantTypes {
		at := 17 + i*5
		elemPid := uint16(pmt[at+1]&0x1f)<<8 | uint16(pmt[at+2])
		esInfo := uint16(pmt[at+3]&0x0f)<<8 | uint16(pmt[at+4])
		c.Check(pmt[at] == st && elemPid == 0x100+uint16(i) && esInfo == 0, "PMT stream entry matches config")
	}

	// Table continuity counters advance once per actual emission.
	patCC := counter.ccByPID[0]
	pmtCC := counter.ccByPID[0x200]
	c.Check(len(patCC) == len(counter.patSlots) && len(pmtCC) == len(counter.pmtSlots), "one CC value per table packet")
	for i := range patCC {
		c.Check(patCC[i] == uint8(i%16), "PAT continuity counter increments per emission")
		c.Check(pmtCC[i] == uint8(i%16), "PMT continuity counter increments per emission")
	}

	// Audio-only program puts the PCR on the first audio PID.
	audioMux, err := NewCBRMuxer(CBRConfig{
		Bitrate: bitrate, PCRInterval27MHz: 270000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_AAC, TS_STREAM_AAC},
	})
	c.Check(err == nil, "audio-only muxer constructed")
	var audioCol cbrCollector
	audioCol.attach(audioMux)
	c.Check(audioMux.AdvanceTo(cbrSlotPosition(3, bitrate)+1) == nil, "audio-only opening advance")
	audioPMT := audioCol.packets[1]
	audioPCRPid := uint16(audioPMT[13]&0x1f)<<8 | uint16(audioPMT[14])
	c.Check(audioPCRPid == 0x100, "audio-only PCR PID is the first audio stream")
	c.Check(audioCol.parsed(c.t)[2].pid == 0x100, "opening heartbeat uses the audio PCR PID")

	// At a very high bitrate the minimum PCR interval (three slots) makes
	// heartbeats frequent enough to collide with due PAT slots; the muxer
	// must still complete every PAT/PMT group in order.
	fastMux, err := NewCBRMuxer(CBRConfig{
		Bitrate: 1000000000, PCRInterval27MHz: 120,
		Streams: []TS_STREAM_TYPE{TS_STREAM_H264, TS_STREAM_AAC},
	})
	c.Check(err == nil, "high-bitrate table muxer constructed")
	fastCounter := newCBRCounter()
	fastCounter.attach(fastMux)
	c.Check(fastMux.AdvanceTo(8100000) == nil, "300 ms high-bitrate advance")
	c.Check(len(fastCounter.patSlots) == len(fastCounter.pmtSlots), "every high-rate PAT has a PMT")
	for i, pat := range fastCounter.patSlots {
		c.Check(fastCounter.pmtSlots[i] > pat, "high-rate PMT follows its PAT")
		if i+1 < len(fastCounter.patSlots) {
			c.Check(fastCounter.pmtSlots[i] < fastCounter.patSlots[i+1], "high-rate group completes before the next PAT")
		}
	}
	c.Check(len(fastCounter.heartbeats) > len(fastCounter.patSlots), "tight interval produces many heartbeats")
}

// ---------------------------------------------------------------------------
// TestCBRMedia: PES reassembly, timestamps, queue ordering, buffer reuse.
// ---------------------------------------------------------------------------
func caseCBRMedia(c *cbrChecks) {
	const bitrate uint64 = 2000000
	mux, err := NewCBRMuxer(CBRConfig{
		Bitrate: bitrate, PCRInterval27MHz: 270000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_H264, TS_STREAM_H265, TS_STREAM_AAC},
	})
	c.Check(err == nil, "media muxer constructed")
	videoPID, hevcPID, audioPID := uint16(0x100), uint16(0x101), uint16(0x102)

	// IDR H264 frame that already carries an AUD, large enough for several TS packets.
	h264Frame := make([]byte, 700)
	copy(h264Frame, []byte{0, 0, 0, 1, 0x09, 0xf0, 0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21})
	for i := 14; i < len(h264Frame); i++ {
		h264Frame[i] = 0x55
	}
	// H265 IDR slice without an AUD: the muxer must prepend one, as legacy does.
	h265Frame := make([]byte, 400)
	copy(h265Frame, []byte{0, 0, 0, 1, 0x26, 0x01, 0xaf})
	for i := 7; i < len(h265Frame); i++ {
		h265Frame[i] = 0x33
	}
	aacFrame := []byte{0xff, 0xf1, 0x4c, 0x80, 0x01, 0x7f, 0xfc, 0x11, 0x22, 0x33}

	// Audio is written before video: queue order must be preserved.
	c.Check(mux.Write(audioPID, aacFrame, 1000, 1000) == nil, "audio write")
	c.Check(mux.Write(videoPID, h264Frame, 1000, 960) == nil, "h264 write")
	c.Check(mux.Write(hevcPID, h265Frame, 1040, 1040) == nil, "h265 write")
	c.Check(mux.PendingPackets() > 3, "multi-packet frames produce several pending TS packets")

	// Input buffer reuse: overwrite a shared backing array after Write returns.
	reuse := make([]byte, 128)
	copy(reuse, []byte{0xff, 0xf1, 0x4c, 0x80, 0xde, 0xad, 0xbe, 0xef})
	c.Check(mux.Write(audioPID, reuse, 2000, 2000) == nil, "first reuse write")
	firstSnapshot := append([]byte(nil), reuse...)
	for i := range reuse {
		reuse[i] = 0x77
	}
	c.Check(mux.Write(audioPID, reuse, 3000, 3000) == nil, "second reuse write")
	c.Check(string(firstSnapshot) != string(reuse), "test setup: backing array really changed")

	// Unknown PID is an error and does not corrupt the queue.
	pendingBefore := mux.PendingPackets()
	c.Check(mux.Write(0x199, h264Frame, 1, 1) != nil, "unknown pid rejected")
	c.Check(mux.PendingPackets() == pendingBefore, "failed write enqueues nothing")

	var col cbrCollector
	col.attach(mux)
	c.Check(mux.AdvanceTo(27000000) == nil, "drain all queued media")
	c.Check(mux.PendingPackets() == 0, "every media packet transmitted")
	parsed := col.parsed(c.t)

	// Every PID belongs to the fixed topology.
	validPIDs := map[uint16]bool{0: true, 0x200: true, cbrNullPID: true, 0x100: true, 0x101: true, 0x102: true}
	for _, p := range parsed {
		c.Check(validPIDs[p.pid], "emitted PID belongs to the fixed topology")
	}

	// Media continuity counters increment per emitted packet, mod 16.
	ccSeen := map[uint16][]uint8{}
	for _, p := range parsed {
		if p.pid >= 0x100 {
			ccSeen[p.pid] = append(ccSeen[p.pid], p.cc)
		}
	}
	for _, ccs := range ccSeen {
		// Heartbeats repeat the current counter without consuming it; collapse
		// consecutive duplicates before checking the +1 sequence.
		dedup := ccs[:0]
		for _, v := range ccs {
			if len(dedup) == 0 || dedup[len(dedup)-1] != v {
				dedup = append(dedup, v)
			}
		}
		for i := 1; i < len(dedup); i++ {
			c.Check(dedup[i] == (dedup[i-1]+1)%16, "media continuity counter advances by one on each emitted packet")
		}
	}

	pesList := cbrReassemble(c.t, parsed)
	c.Check(len(pesList) == 5, "three AAC, one H264 and one H265 PES rebuilt")
	var aacPES, h264PES, h265PES []cbrReassembledPES
	for _, pes := range pesList {
		switch pes.pid {
		case audioPID:
			aacPES = append(aacPES, pes)
		case videoPID:
			h264PES = append(h264PES, pes)
		case hevcPID:
			h265PES = append(h265PES, pes)
		}
	}
	c.Check(len(aacPES) == 3 && len(h264PES) == 1 && len(h265PES) == 1, "per-PID PES counts")
	c.Check(bytes.Equal(aacPES[0].body, aacFrame), "first AAC payload survives reassembly")
	c.Check(aacPES[0].pts == 90000 && aacPES[0].dts == 90000 && aacPES[0].streamID == 0xc0, "AAC PTS/DTS in 90 kHz units and audio stream id")
	c.Check(bytes.Equal(aacPES[1].body, firstSnapshot), "queued copy is immune to input buffer reuse")
	c.Check(bytes.Equal(h264PES[0].body, h264Frame), "multi-packet H264 PES body reassembled byte for byte")
	c.Check(h264PES[0].pts == 90000 && h264PES[0].dts == 86400 && h264PES[0].streamID == 0xe0, "H264 PTS/DTS preserved")
	wantH265Body := append(append([]byte(nil), H265_AUD_NALU...), h265Frame...)
	c.Check(bytes.Equal(h265PES[0].body, wantH265Body), "missing H265 AUD is inserted like the legacy muxer")

	// The IDR H264 PES head carries the random access indicator and a PCR.
	var headWithRAI, mediaPCRCount int
	for _, p := range parsed {
		if p.pid == videoPID && p.pusi {
			headWithRAI++
		}
		if p.hasPCR && p.afc == 3 {
			mediaPCRCount++
			slot := uint64(p.offset)
			c.Check(cbrPCRValue(p) == (mux.startPCR+cbrSlotPosition(slot, bitrate))%cbrPCRMod, "media packet PCR stamped at its real output slot")
		}
	}
	c.Check(headWithRAI == 1, "one H264 PES head")
	c.Check(mediaPCRCount >= 1, "PCR-PID PES heads carry PCR fields")
	rai := false
	for _, p := range parsed {
		if p.pid == videoPID && p.pusi && p.afFlags&0x40 != 0 {
			rai = true
		}
	}
	c.Check(rai, "IDR frame sets the random access indicator")

	// The same queue drained in one advance and in many small advances gives
	// the identical byte stream.
	build := func() (*CBRMuxer, *cbrCollector) {
		m, e := NewCBRMuxer(CBRConfig{Bitrate: bitrate, PCRInterval27MHz: 270000, Streams: []TS_STREAM_TYPE{TS_STREAM_H264, TS_STREAM_AAC}})
		if e != nil {
			c.t.Fatal(e)
		}
		var cc cbrCollector
		cc.attach(m)
		if e := m.Write(0x100, h264Frame, 1000, 960); e != nil {
			c.t.Fatal(e)
		}
		if e := m.Write(0x101, aacFrame, 1000, 1000); e != nil {
			c.t.Fatal(e)
		}
		return m, &cc
	}
	oneMux, oneCol := build()
	c.Check(oneMux.AdvanceTo(5400000) == nil, "single big advance")
	splitMux, splitCol := build()
	// Steps deliberately include tiny and uneven boundaries and sum exactly
	// to the same elapsed value as the single advance above.
	for _, step := range []uint64{1, 7, 4096, 300000, 1, 2700000, 2395895} {
		c.Check(splitMux.AdvanceTo(splitMux.elapsed+step) == nil, "split advance step")
	}
	c.Check(splitMux.elapsed == 5400000, "split advances reach the same elapsed as the single advance")
	c.Check(bytes.Equal(oneCol.allBytes(), splitCol.allBytes()), "one advance and split advances emit identical bytes")
	c.Check(len(oneCol.packets) == len(splitCol.packets) && len(oneCol.packets) > 4, "split run actually exercised multiple advances")

	// An error mid-stream leaves the muxer fully usable afterwards.
	c.Check(splitMux.AdvanceTo(1) != nil, "backwards error after media drain")
	c.Check(splitMux.Write(0x101, aacFrame, 4000, 4000) == nil, "writing continues after an advance error")
	c.Check(splitMux.AdvanceTo(27000000) == nil, "advancing continues after an error")
}

// ---------------------------------------------------------------------------
// TestCBRCompatibility: legacy byte output, demuxer roundtrip, idle filling.
// ---------------------------------------------------------------------------
func caseCBRCompatibility(c *cbrChecks) {
	// The legacy TSMuxer must keep producing its exact historical byte stream.
	legacy := NewTSMuxer()
	v := legacy.AddStream(TS_STREAM_H264)
	a := legacy.AddStream(TS_STREAM_AAC)
	var legacyPackets [][]byte
	legacy.OnPacket = func(p []byte) {
		c.Check(len(p) == 188 && p[0] == 0x47, "legacy packet shape unchanged")
		legacyPackets = append(legacyPackets, append([]byte(nil), p...))
	}
	video := make([]byte, 700)
	copy(video, []byte{0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21})
	for i := 8; i < len(video); i++ {
		video[i] = 0x55
	}
	aac := []byte{0xff, 0xf1, 0x4c, 0x80, 1, 0x7f, 0xfc, 0x11}
	c.Check(legacy.Write(v, video, 1000, 960) == nil, "legacy video write")
	c.Check(legacy.Write(a, aac, 1000, 1000) == nil, "legacy audio write")
	c.Check(legacy.Write(v, video, 2080, 2040) == nil, "legacy second video write")
	var legacyAll bytes.Buffer
	legacyPCR := 0
	for _, p := range legacyPackets {
		legacyAll.Write(p)
		if p[3]&0x20 != 0 && p[4] >= 7 && p[5]&0x10 != 0 {
			legacyPCR++
		}
	}
	c.Check(len(legacyPackets) == 13, "legacy packet count unchanged")
	c.Check(legacyPCR == 2, "legacy PCR count unchanged")
	c.Check(sha256Hex(legacyAll.Bytes()) == "9758f331ae6622970f077301639af0f0a34f229e43232edddee3ed695101f5a6", "legacy byte-for-byte output unchanged")

	// A full CBR stream is readable by the in-repo TSDemuxer.
	mux, err := NewCBRMuxer(CBRConfig{
		Bitrate: 4000000, StartPCR27MHz: 123456, PCRInterval27MHz: 270000,
		Streams: []TS_STREAM_TYPE{TS_STREAM_H264, TS_STREAM_AAC},
	})
	c.Check(err == nil, "compatibility muxer constructed")
	frame := make([]byte, 500)
	copy(frame, []byte{0, 0, 0, 1, 0x09, 0xf0, 0, 0, 0, 1, 0x65, 0x01, 0x02, 0x03})
	for i := 14; i < len(frame); i++ {
		frame[i] = 0x44
	}
	c.Check(mux.Write(0x100, frame, 1000, 960) == nil, "cbr video write")
	c.Check(mux.Write(0x101, aac, 1000, 1000) == nil, "cbr audio write")
	c.Check(mux.Write(0x100, frame, 2000, 1960) == nil, "cbr second video write")
	var col cbrCollector
	col.attach(mux)
	c.Check(mux.AdvanceTo(27000000) == nil, "cbr drain")
	c.Check(len(col.packets) == int(cbrExpectedSlotCount(27000000, 4000000)), "cbr slot count matches formula")

	type gotFrame struct {
		cid      TS_STREAM_TYPE
		data     []byte
		pts, dts uint64
	}
	var got []gotFrame
	demux := NewTSDemuxer()
	demux.OnFrame = func(cid TS_STREAM_TYPE, f []byte, pts, dts uint64) {
		got = append(got, gotFrame{cid, append([]byte(nil), f...), pts, dts})
	}
	c.Check(demux.Input(bytes.NewReader(col.allBytes())) == nil, "in-repo demuxer parses CBR output")
	c.Check(len(got) == 3, "demuxer rebuilds all three access units")
	var vFrames, aFrames int
	for _, f := range got {
		switch f.cid {
		case TS_STREAM_H264:
			vFrames++
			c.Check(bytes.Equal(f.data, frame[6:]), "demuxed video body matches input with AUD stripped")
			c.Check(f.dts%40 == 0, "video timestamp roundtrips through PES")
		case TS_STREAM_AAC:
			aFrames++
			c.Check(bytes.Equal(f.data, aac) && f.pts == 1000 && f.dts == 1000, "demuxed AAC body and timestamp")
		}
	}
	c.Check(vFrames == 2 && aFrames == 1, "demuxed frame type counts")

	// Idle output keeps every slot filled and stays decodable.
	idle, err := NewCBRMuxer(CBRConfig{Bitrate: 1500000, PCRInterval27MHz: 270000, Streams: []TS_STREAM_TYPE{TS_STREAM_H264}})
	c.Check(err == nil, "idle compatibility muxer constructed")
	var idleCol cbrCollector
	idleCol.attach(idle)
	c.Check(idle.AdvanceTo(5400000) == nil, "idle 200 ms advance")
	idleParsed := idleCol.parsed(c.t)
	c.Check(uint64(len(idleParsed)) == cbrExpectedSlotCount(5400000, 1500000), "idle slot count")
	nullCount := 0
	for _, p := range idleParsed {
		if p.pid == cbrNullPID {
			nullCount++
		}
	}
	c.Check(nullCount > 0, "null packets fill idle slots")
	demux2 := NewTSDemuxer()
	frameCount := 0
	demux2.OnFrame = func(cid TS_STREAM_TYPE, f []byte, pts, dts uint64) { frameCount++ }
	c.Check(demux2.Input(bytes.NewReader(idleCol.allBytes())) == nil, "table/PCR/null-only stream is decodable")
	c.Check(frameCount == 0, "no media frames invented while idle")
}
