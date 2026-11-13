package mpeg2

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"testing"
)

// ---------------------------------------------------------------------------
// independent TS reassembly helpers (standard library only)
// ---------------------------------------------------------------------------

// crc32MPEG computes the MPEG-2 section CRC (poly 0x04C11DB7, init 0xFFFFFFFF,
// no reflection, no final xor) bit by bit, independently of go-codec.
func crc32MPEG(data []byte) uint32 {
	crc := uint32(0xFFFFFFFF)
	for _, b := range data {
		crc ^= uint32(b) << 24
		for i := 0; i < 8; i++ {
			if crc&0x80000000 != 0 {
				crc = (crc << 1) ^ 0x04C11DB7
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

type tsPktInfo struct {
	pid          uint16
	pusi         bool
	cc           uint8
	hasPCR       bool
	pcrBase      uint64
	randomAccess bool
	payload      []byte //nil when the packet carries no payload
}

func parseTSPacket(t *testing.T, raw []byte) tsPktInfo {
	t.Helper()
	if len(raw) != TS_PAKCET_SIZE {
		t.Fatalf("ts packet size = %d, want %d", len(raw), TS_PAKCET_SIZE)
	}
	if raw[0] != 0x47 {
		t.Fatalf("missing sync byte: %#02x", raw[0])
	}
	var p tsPktInfo
	p.pusi = raw[1]&0x40 != 0
	p.pid = uint16(raw[1]&0x1F)<<8 | uint16(raw[2])
	afc := raw[3] >> 4 & 0x03
	p.cc = raw[3] & 0x0F
	off := 4
	if afc == 0x02 || afc == 0x03 {
		afl := int(raw[off])
		if afl > 0 {
			flags := raw[off+1]
			p.randomAccess = flags&0x40 != 0
			if flags&0x10 != 0 {
				p.hasPCR = true
				b := raw[off+2 : off+8]
				p.pcrBase = uint64(b[0])<<25 | uint64(b[1])<<17 | uint64(b[2])<<9 |
					uint64(b[3])<<1 | uint64(b[4])>>7
			}
		}
		off += 1 + afl
	}
	if afc == 0x01 || afc == 0x03 {
		p.payload = raw[off:]
	}
	return p
}

// collectSections reassembles every section carried on pid, verifying that
// stuffing behind a section end is 0xFF only.
func collectSections(t *testing.T, pkgs [][]byte, pid uint16) [][]byte {
	t.Helper()
	var out [][]byte
	var cur []byte
	need := -1
	for _, raw := range pkgs {
		p := parseTSPacket(t, raw)
		if p.pid != pid {
			continue
		}
		if p.payload == nil {
			t.Fatalf("pid %#x: section packet without payload", pid)
		}
		payload := p.payload
		if p.pusi {
			if len(cur) != 0 {
				t.Fatalf("pid %#x: new section before previous one completed", pid)
			}
			if payload[0] != 0x00 {
				t.Fatalf("pid %#x: pointer field = %d, want 0", pid, payload[0])
			}
			cur = append([]byte(nil), payload[1:]...)
		} else {
			if cur == nil {
				t.Fatalf("pid %#x: continuation packet without section start", pid)
			}
			cur = append(cur, payload...)
		}
		if need < 0 && len(cur) >= 3 {
			need = 3 + (int(cur[1]&0x0F)<<8 | int(cur[2]))
		}
		if need >= 0 && len(cur) >= need {
			sec := append([]byte(nil), cur[:need]...)
			for _, b := range cur[need:] {
				if b != 0xFF {
					t.Fatalf("pid %#x: non-stuffing byte %#02x behind section end", pid, b)
				}
			}
			out = append(out, sec)
			cur = nil
			need = -1
		}
	}
	if cur != nil {
		t.Fatalf("pid %#x: trailing incomplete section (%d bytes)", pid, len(cur))
	}
	return out
}

func checkSectionCRC(t *testing.T, sec []byte) {
	t.Helper()
	if len(sec) < 4 {
		t.Fatalf("section too short: %d", len(sec))
	}
	want := binary.BigEndian.Uint32(sec[len(sec)-4:])
	if got := crc32MPEG(sec[:len(sec)-4]); got != want {
		t.Fatalf("section crc mismatch: computed %#08x, section carries %#08x", got, want)
	}
}

type pmtStreamInfo struct {
	stype uint8
	pid   uint16
	desc  []byte
}

type pmtInfo struct {
	program uint16
	version uint8
	pcrPID  uint16
	streams []pmtStreamInfo
}

func parsePMTSection(t *testing.T, sec []byte) pmtInfo {
	t.Helper()
	if sec[0] != 0x02 {
		t.Fatalf("pmt table id = %#02x, want 0x02", sec[0])
	}
	secLen := int(sec[1]&0x0F)<<8 | int(sec[2])
	if secLen+3 != len(sec) {
		t.Fatalf("pmt section_length = %d, reassembled %d bytes", secLen, len(sec)-3)
	}
	var info pmtInfo
	info.program = binary.BigEndian.Uint16(sec[3:5])
	info.version = sec[5] >> 1 & 0x1F
	if sec[5]&0x01 == 0 {
		t.Fatalf("pmt current_next_indicator not set")
	}
	info.pcrPID = uint16(sec[8]&0x1F)<<8 | uint16(sec[9])
	progInfo := int(sec[10]&0x0F)<<8 | int(sec[11])
	off := 12 + progInfo
	end := len(sec) - 4
	for off < end {
		var st pmtStreamInfo
		st.stype = sec[off]
		st.pid = uint16(sec[off+1]&0x1F)<<8 | uint16(sec[off+2])
		dl := int(sec[off+3]&0x0F)<<8 | int(sec[off+4])
		st.desc = append([]byte(nil), sec[off+5:off+5+dl]...)
		info.streams = append(info.streams, st)
		off += 5 + dl
	}
	if off != end {
		t.Fatalf("pmt stream loop ends at %d, section ends at %d", off, end)
	}
	checkSectionCRC(t, sec)
	return info
}

func parsePATSection(t *testing.T, sec []byte) (version uint8, pmts map[uint16]uint16) {
	t.Helper()
	if sec[0] != 0x00 {
		t.Fatalf("pat table id = %#02x, want 0x00", sec[0])
	}
	checkSectionCRC(t, sec)
	version = sec[5] >> 1 & 0x1F
	pmts = make(map[uint16]uint16)
	for off := 8; off+4 <= len(sec)-4; off += 4 {
		program := binary.BigEndian.Uint16(sec[off : off+2])
		pid := uint16(sec[off+2]&0x1F)<<8 | uint16(sec[off+3])
		pmts[program] = pid
	}
	return version, pmts
}

type pesInfo struct {
	streamID uint8
	pts      uint64
	dts      uint64
	payload  []byte
}

func decodePTS(b []byte) uint64 {
	return uint64(b[0]>>1&0x07)<<30 | uint64(b[1])<<22 | uint64(b[2]>>1)<<15 |
		uint64(b[3])<<7 | uint64(b[4]>>1)
}

func parsePES(t *testing.T, data []byte) pesInfo {
	t.Helper()
	if len(data) < 9 || !bytes.Equal(data[:3], []byte{0x00, 0x00, 0x01}) {
		t.Fatalf("bad pes start code")
	}
	var pi pesInfo
	pi.streamID = data[3]
	ptsDts := data[7] >> 6
	hdrlen := int(data[8])
	if ptsDts&0x02 != 0 {
		pi.pts = decodePTS(data[9:14])
	}
	if ptsDts == 0x03 {
		pi.dts = decodePTS(data[14:19])
	} else {
		pi.dts = pi.pts
	}
	pi.payload = data[9+hdrlen:]
	return pi
}

// collectPES reassembles the PES packets carried on pid.
func collectPES(t *testing.T, pkgs [][]byte, pid uint16) []pesInfo {
	t.Helper()
	var out []pesInfo
	var cur []byte
	started := false
	flush := func() {
		if started {
			out = append(out, parsePES(t, cur))
		}
	}
	for _, raw := range pkgs {
		p := parseTSPacket(t, raw)
		if p.pid != pid {
			continue
		}
		if p.pusi {
			flush()
			cur = append([]byte(nil), p.payload...)
			started = true
		} else {
			if !started {
				t.Fatalf("pid %#x: pes continuation before start", pid)
			}
			cur = append(cur, p.payload...)
		}
	}
	flush()
	return out
}

// checkContinuity verifies that every PID advances its continuity counter
// by one per emitted packet.
func checkContinuity(t *testing.T, pkgs [][]byte) {
	t.Helper()
	last := make(map[uint16]uint8)
	seen := make(map[uint16]bool)
	for _, raw := range pkgs {
		p := parseTSPacket(t, raw)
		if seen[p.pid] {
			want := (last[p.pid] + 1) & 0x0F
			if p.cc != want {
				t.Fatalf("pid %#x: continuity counter %d, want %d", p.pid, p.cc, want)
			}
		}
		seen[p.pid] = true
		last[p.pid] = p.cc
	}
}

func pidsOf(t *testing.T, pkgs [][]byte) []uint16 {
	t.Helper()
	var out []uint16
	for _, raw := range pkgs {
		out = append(out, parseTSPacket(t, raw).pid)
	}
	return out
}

func hasPID(t *testing.T, pkgs [][]byte, pid uint16) bool {
	t.Helper()
	for _, raw := range pkgs {
		if parseTSPacket(t, raw).pid == pid {
			return true
		}
	}
	return false
}

// ---------------------------------------------------------------------------
// muxer harness
// ---------------------------------------------------------------------------

type muxHarness struct {
	t    *testing.T
	mux  *TSMuxer
	pkgs [][]byte
	mark int
}

func newMuxHarness(t *testing.T) *muxHarness {
	h := &muxHarness{t: t, mux: NewTSMuxer()}
	h.mux.OnPacket = func(pkg []byte) {
		h.pkgs = append(h.pkgs, append([]byte(nil), pkg...))
	}
	return h
}

// since returns the packets emitted since the previous call and consumes them.
func (h *muxHarness) since() [][]byte {
	out := h.pkgs[h.mark:]
	h.mark = len(h.pkgs)
	return out
}

func (h *muxHarness) configure(streams []ProgramStream, pcr uint16) {
	h.t.Helper()
	before := len(h.pkgs)
	if err := h.mux.ConfigureProgram(streams, pcr); err != nil {
		h.t.Fatalf("ConfigureProgram failed: %v", err)
	}
	if len(h.pkgs) != before {
		h.t.Fatalf("ConfigureProgram emitted %d packets", len(h.pkgs)-before)
	}
}

func (h *muxHarness) write(pid uint16, data []byte, pts, dts uint64) {
	h.t.Helper()
	if err := h.mux.Write(pid, data, pts, dts); err != nil {
		h.t.Fatalf("Write(%#x) failed: %v", pid, err)
	}
}

func (h *muxHarness) writeErr(pid uint16, data []byte, pts, dts uint64) {
	h.t.Helper()
	before := len(h.pkgs)
	if err := h.mux.Write(pid, data, pts, dts); err == nil {
		h.t.Fatalf("Write(%#x) succeeded, want error", pid)
	}
	if len(h.pkgs) != before {
		h.t.Fatalf("failed Write(%#x) emitted packets", pid)
	}
}

// wantTables asserts that pkgs start with one PAT and one PMT and returns the
// parsed PMT plus the remaining packets.
func (h *muxHarness) wantTables(pkgs [][]byte, version uint8, pcr uint16) (pmtInfo, [][]byte) {
	h.t.Helper()
	pids := pidsOf(h.t, pkgs)
	if len(pids) < 2 || pids[0] != 0x0000 || pids[1] != 0x0200 {
		h.t.Fatalf("tables not leading the output, pids = %v", pids)
	}
	patSec := collectSections(h.t, pkgs, 0x0000)
	if len(patSec) != 1 {
		h.t.Fatalf("got %d pat sections, want 1", len(patSec))
	}
	patVer, pmts := parsePATSection(h.t, patSec[0])
	if patVer != 0 {
		h.t.Fatalf("pat version = %d, want 0 (track changes must not bump it)", patVer)
	}
	if len(pmts) != 1 || pmts[1] != 0x0200 {
		h.t.Fatalf("pat programs = %v, want {1:0x200}", pmts)
	}
	pmtSec := collectSections(h.t, pkgs, 0x0200)
	if len(pmtSec) != 1 {
		h.t.Fatalf("got %d pmt sections, want 1", len(pmtSec))
	}
	pmt := parsePMTSection(h.t, pmtSec[0])
	if pmt.version != version {
		h.t.Fatalf("pmt version = %d, want %d", pmt.version, version)
	}
	if pmt.pcrPID != pcr {
		h.t.Fatalf("pmt pcr pid = %#x, want %#x", pmt.pcrPID, pcr)
	}
	if pmt.program != 1 {
		h.t.Fatalf("pmt program = %d, want 1", pmt.program)
	}
	rest := pkgs[1+pmtPacketCount(h.t, pkgs):]
	return pmt, rest
}

func pmtPacketCount(t *testing.T, pkgs [][]byte) int {
	t.Helper()
	n := 0
	for _, raw := range pkgs {
		if parseTSPacket(t, raw).pid == 0x0200 {
			n++
		}
	}
	return n
}

func wantNoTables(t *testing.T, pkgs [][]byte) {
	t.Helper()
	if hasPID(t, pkgs, 0x0000) || hasPID(t, pkgs, 0x0200) {
		t.Fatalf("unexpected PAT/PMT in output")
	}
}

func wantPES(t *testing.T, pkgs [][]byte, pid uint16, payload []byte, pts, dts uint64) pesInfo {
	t.Helper()
	pes := collectPES(t, pkgs, pid)
	if len(pes) != 1 {
		t.Fatalf("got %d pes packets on pid %#x, want 1", len(pes), pid)
	}
	if !bytes.Equal(pes[0].payload, payload) {
		t.Fatalf("pid %#x pes payload = %x, want %x", pid, pes[0].payload, payload)
	}
	if pes[0].pts != pts*90 || pes[0].dts != dts*90 {
		t.Fatalf("pid %#x pts/dts = %d/%d, want %d/%d", pid, pes[0].pts, pes[0].dts, pts*90, dts*90)
	}
	return pes[0]
}

var (
	tH264IDR = []byte{0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x99}
	tH264P   = []byte{0x00, 0x00, 0x01, 0x41, 0x11, 0x22}
	tAAC1    = []byte{0xA1, 0xB2, 0xC3}
	tAAC2    = []byte{0xD4, 0xE5}
)

func h264WithAUD(frame []byte) []byte {
	return append(append([]byte(nil), H264_AUD_NALU...), frame...)
}

// ---------------------------------------------------------------------------
// TestProgramReplacement
// ---------------------------------------------------------------------------

func TestProgramReplacement(t *testing.T) {
	h := newMuxHarness(t)

	//initial configuration: nothing is emitted by ConfigureProgram itself
	h.configure([]ProgramStream{
		{PID: 0x100, Type: TS_STREAM_H264},
		{PID: 0x101, Type: TS_STREAM_AAC},
	}, 0x100)
	if len(h.pkgs) != 0 {
		t.Fatalf("ConfigureProgram emitted %d packets", len(h.pkgs))
	}

	//first write publishes PAT + PMT v0 before the PES
	h.write(0x100, tH264IDR, 0, 0)
	out := h.since()
	pmt, rest := h.wantTables(out, 0, 0x100)
	if len(pmt.streams) != 2 || pmt.streams[0].pid != 0x100 || pmt.streams[0].stype != 0x1B ||
		pmt.streams[1].pid != 0x101 || pmt.streams[1].stype != 0x0F {
		t.Fatalf("unexpected pmt streams: %+v", pmt.streams)
	}
	pes := wantPES(t, rest, 0x100, h264WithAUD(tH264IDR), 0, 0)
	if pes.streamID != 0xE0 {
		t.Fatalf("video pes stream id = %#x, want 0xE0", pes.streamID)
	}
	vp := parseTSPacket(t, rest[0])
	if !vp.hasPCR || !vp.randomAccess {
		t.Fatalf("first video packet: pcr=%v randomAccess=%v, want both set", vp.hasPCR, vp.randomAccess)
	}

	//audio only: no tables below 400ms
	h.write(0x101, tAAC1, 10, 10)
	out = h.since()
	wantNoTables(t, out)
	pes = wantPES(t, out, 0x101, tAAC1, 10, 10)
	if pes.streamID != 0xC0 {
		t.Fatalf("audio pes stream id = %#x, want 0xC0", pes.streamID)
	}
	if parseTSPacket(t, out[0]).hasPCR {
		t.Fatalf("audio pid is not the pcr pid, must not carry pcr")
	}

	//replace 0x101 with 0x102
	h.configure([]ProgramStream{
		{PID: 0x100, Type: TS_STREAM_H264},
		{PID: 0x102, Type: TS_STREAM_AAC},
	}, 0x100)

	//the removed pid and unknown pids reject media immediately,
	//without consuming the pending publication
	h.writeErr(0x101, tAAC1, 20, 20)
	h.writeErr(0x1ABC, tAAC1, 20, 20)

	//the next valid write still publishes first, well below 400ms
	h.write(0x102, tAAC2, 20, 20)
	out = h.since()
	pmt, rest = h.wantTables(out, 1, 0x100)
	if len(pmt.streams) != 2 || pmt.streams[1].pid != 0x102 {
		t.Fatalf("unexpected pmt streams after replacement: %+v", pmt.streams)
	}
	wantPES(t, rest, 0x102, tAAC2, 20, 20)

	//retained video pid keeps counting its continuity counter
	h.write(0x100, tH264P, 30, 30)
	out = h.since()
	wantNoTables(t, out)
	wantPES(t, out, 0x100, h264WithAUD(tH264P), 30, 30)
	if cc := parseTSPacket(t, out[0]).cc; cc != 1 {
		t.Fatalf("retained pid 0x100 cc = %d, want 1", cc)
	}

	//re-add 0x101 and move the pcr pid onto it
	h.configure([]ProgramStream{
		{PID: 0x100, Type: TS_STREAM_H264},
		{PID: 0x101, Type: TS_STREAM_AAC},
		{PID: 0x102, Type: TS_STREAM_AAC},
	}, 0x101)
	h.write(0x101, tAAC1, 40, 40)
	out = h.since()
	pmt, rest = h.wantTables(out, 2, 0x101)
	if len(pmt.streams) != 3 {
		t.Fatalf("want 3 streams, got %+v", pmt.streams)
	}
	wantPES(t, rest, 0x101, tAAC1, 40, 40)
	if !parseTSPacket(t, rest[0]).hasPCR {
		t.Fatalf("pid 0x101 is the configured pcr pid, its pes must carry pcr")
	}
	if cc := parseTSPacket(t, rest[0]).cc; cc != 1 {
		t.Fatalf("re-added pid 0x101 cc = %d, want 1 (resumed)", cc)
	}

	//pending publication was consumed: plain write, no tables
	h.write(0x102, tAAC2, 50, 50)
	wantNoTables(t, h.since())

	//identical configuration: no version bump, no forced publication
	h.configure([]ProgramStream{
		{PID: 0x100, Type: TS_STREAM_H264},
		{PID: 0x101, Type: TS_STREAM_AAC},
		{PID: 0x102, Type: TS_STREAM_AAC},
	}, 0x101)
	h.write(0x102, tAAC2, 60, 60)
	wantNoTables(t, h.since())

	//a video write must not steal the pcr pid in explicit mode: the periodic
	//republication still advertises the configured 0x101, and the video pes
	//carries no pcr
	h.write(0x100, tH264P, 500, 500)
	out = h.since()
	pmt, rest = h.wantTables(out, 2, 0x101)
	wantPES(t, rest, 0x100, h264WithAUD(tH264P), 500, 500)
	if parseTSPacket(t, rest[0]).hasPCR {
		t.Fatalf("pid 0x100 is not the configured pcr pid, must not carry pcr")
	}

	//pcr-only change bumps the version
	h.configure([]ProgramStream{
		{PID: 0x100, Type: TS_STREAM_H264},
		{PID: 0x101, Type: TS_STREAM_AAC},
		{PID: 0x102, Type: TS_STREAM_AAC},
	}, 0x100)
	h.write(0x100, tH264IDR, 510, 510)
	out = h.since()
	pmt, rest = h.wantTables(out, 3, 0x100)
	wantPES(t, rest, 0x100, h264WithAUD(tH264IDR), 510, 510)

	//descriptor change bumps the version and keeps the raw bytes
	desc1 := []byte{0x05, 0x02, 0xAA, 0xBB}
	h.configure([]ProgramStream{
		{PID: 0x100, Type: TS_STREAM_H264},
		{PID: 0x101, Type: TS_STREAM_AAC, Descriptors: desc1},
		{PID: 0x102, Type: TS_STREAM_AAC},
	}, 0x100)
	h.write(0x101, tAAC1, 520, 520)
	out = h.since()
	pmt, _ = h.wantTables(out, 4, 0x100)
	if !bytes.Equal(pmt.streams[1].desc, desc1) {
		t.Fatalf("descriptor = %x, want %x", pmt.streams[1].desc, desc1)
	}

	//reordering alone bumps the version
	h.configure([]ProgramStream{
		{PID: 0x102, Type: TS_STREAM_AAC},
		{PID: 0x100, Type: TS_STREAM_H264},
		{PID: 0x101, Type: TS_STREAM_AAC, Descriptors: desc1},
	}, 0x100)
	h.write(0x100, tH264P, 530, 530)
	out = h.since()
	pmt, _ = h.wantTables(out, 5, 0x100)
	if pmt.streams[0].pid != 0x102 || pmt.streams[1].pid != 0x100 || pmt.streams[2].pid != 0x101 {
		t.Fatalf("pmt stream order not preserved: %+v", pmt.streams)
	}

	//periodic republication keeps the version
	h.write(0x102, tAAC2, 1030, 1030)
	out = h.since()
	pmt, _ = h.wantTables(out, 5, 0x100)

	//33 effective changes wrap the 5-bit version counter; an invalid write
	//in between must not eat the pending publication
	for i := 1; i <= 33; i++ {
		d := []byte{0x05, 0x01, byte(i)}
		h.configure([]ProgramStream{
			{PID: 0x100, Type: TS_STREAM_H264},
			{PID: 0x101, Type: TS_STREAM_AAC, Descriptors: d},
			{PID: 0x102, Type: TS_STREAM_AAC},
		}, 0x100)
		if i%5 == 0 {
			h.writeErr(0x103, tAAC1, 1030+uint64(i)*10, 1030+uint64(i)*10)
		}
		h.write(0x101, tAAC1, 1030+uint64(i)*10, 1030+uint64(i)*10)
		out = h.since()
		want := uint8((5 + i) % 32)
		pmt, rest := h.wantTables(out, want, 0x100)
		if !bytes.Equal(pmt.streams[1].desc, d) {
			t.Fatalf("iteration %d: descriptor = %x, want %x", i, pmt.streams[1].desc, d)
		}
		wantPES(t, rest, 0x101, tAAC1, 1030+uint64(i)*10, 1030+uint64(i)*10)
	}

	//continuity counters advance per emitted packet on every pid,
	//across all reconfigurations
	checkContinuity(t, h.pkgs)
}

// ---------------------------------------------------------------------------
// TestPMTSections
// ---------------------------------------------------------------------------

func makeDescriptor(tag, seed byte, bodyLen int) []byte {
	d := make([]byte, 2+bodyLen)
	d[0] = tag
	d[1] = byte(bodyLen)
	for i := 0; i < bodyLen; i++ {
		d[2+i] = seed + byte(i)
	}
	return d
}

func TestPMTSections(t *testing.T) {
	t.Run("MultiPacket", func(t *testing.T) {
		h := newMuxHarness(t)
		//six streams with 150-byte descriptor bodies: section_length = 955,
		//the 958-byte section spans six TS packets
		streams := []ProgramStream{
			{PID: 0x100, Type: TS_STREAM_H264, Descriptors: makeDescriptor(0x05, 0x10, 150)},
			{PID: 0x101, Type: TS_STREAM_AAC, Descriptors: makeDescriptor(0x0A, 0x20, 150)},
			{PID: 0x102, Type: TS_STREAM_AUDIO_MPEG1, Descriptors: makeDescriptor(0x05, 0x30, 150)},
			{PID: 0x103, Type: TS_STREAM_AUDIO_MPEG2, Descriptors: makeDescriptor(0x52, 0x40, 150)},
			{PID: 0x104, Type: TS_STREAM_H265, Descriptors: makeDescriptor(0x7F, 0x50, 150)},
			{PID: 0x105, Type: TS_STREAM_AAC, Descriptors: makeDescriptor(0x05, 0x60, 150)},
		}
		//the muxer must keep its own copy of the descriptors
		orig := make([][]byte, len(streams))
		for i := range streams {
			orig[i] = append([]byte(nil), streams[i].Descriptors...)
		}
		h.configure(streams, 0x104)
		for i := range streams {
			for j := range streams[i].Descriptors {
				streams[i].Descriptors[j] ^= 0xFF
			}
		}

		h265IDR := []byte{0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAA, 0xBB}
		h.write(0x104, h265IDR, 0, 0)
		out := h.since()

		pids := pidsOf(t, out)
		if pids[0] != 0x0000 {
			t.Fatalf("first packet pid = %#x, want PAT", pids[0])
		}
		//collect the PMT packets: they must directly follow the PAT
		nPmt := pmtPacketCount(t, out)
		if nPmt != 6 {
			t.Fatalf("pmt spans %d packets, want 6", nPmt)
		}
		for i := 1; i <= nPmt; i++ {
			if pids[i] != 0x0200 {
				t.Fatalf("pmt packet %d has pid %#x", i, pids[i])
			}
		}
		if !parseTSPacket(t, out[1]).pusi {
			t.Fatalf("first pmt packet must set PUSI")
		}
		for i := 2; i <= nPmt; i++ {
			if parseTSPacket(t, out[i]).pusi {
				t.Fatalf("pmt continuation packet %d must clear PUSI", i)
			}
		}
		//consecutive continuity counters inside the pmt run
		for i := 2; i <= nPmt; i++ {
			prev := parseTSPacket(t, out[i-1]).cc
			cur := parseTSPacket(t, out[i]).cc
			if cur != (prev+1)&0x0F {
				t.Fatalf("pmt cc jumps from %d to %d", prev, cur)
			}
		}

		secs := collectSections(t, out, 0x0200)
		if len(secs) != 1 {
			t.Fatalf("reassembled %d pmt sections, want 1", len(secs))
		}
		pmt := parsePMTSection(t, secs[0])
		if pmt.version != 0 || pmt.pcrPID != 0x104 {
			t.Fatalf("pmt version=%d pcr=%#x", pmt.version, pmt.pcrPID)
		}
		wantTypes := []uint8{0x1B, 0x0F, 0x03, 0x04, 0x24, 0x0F}
		if len(pmt.streams) != 6 {
			t.Fatalf("pmt has %d streams, want 6", len(pmt.streams))
		}
		for i, st := range pmt.streams {
			if st.pid != 0x100+uint16(i) || st.stype != wantTypes[i] {
				t.Fatalf("stream %d = pid %#x type %#x", i, st.pid, st.stype)
			}
			if !bytes.Equal(st.desc, orig[i]) {
				t.Fatalf("stream %d descriptor not preserved byte-exactly", i)
			}
		}

		//the PES follows the whole PMT run, payload untouched apart from the AUD
		rest := out[1+nPmt:]
		wantAUD := append(append([]byte(nil), H265_AUD_NALU...), h265IDR...)
		wantPES(t, rest, 0x104, wantAUD, 0, 0)
		if !parseTSPacket(t, rest[0]).hasPCR {
			t.Fatalf("configured pcr pid 0x104 must carry pcr")
		}
		checkContinuity(t, h.pkgs)
	})

	t.Run("MaxSize", func(t *testing.T) {
		h := newMuxHarness(t)
		//section_length exactly 1021: 6 * (5 + 163) + 9 + 4
		var streams []ProgramStream
		for i := 0; i < 6; i++ {
			streams = append(streams, ProgramStream{
				PID:         0x100 + uint16(i),
				Type:        TS_STREAM_AAC,
				Descriptors: makeDescriptor(0x05, byte(0x10+i), 161),
			})
		}
		h.configure(streams, 0x100)
		h.write(0x100, tAAC1, 0, 0)
		out := h.since()
		secs := collectSections(t, out, 0x0200)
		if len(secs) != 1 {
			t.Fatalf("reassembled %d pmt sections, want 1", len(secs))
		}
		secLen := int(secs[0][1]&0x0F)<<8 | int(secs[0][2])
		if secLen != 1021 {
			t.Fatalf("section_length = %d, want 1021", secLen)
		}
		parsePMTSection(t, secs[0])
		checkContinuity(t, h.pkgs)
	})

	t.Run("Validation", func(t *testing.T) {
		h := newMuxHarness(t)
		good := []ProgramStream{
			{PID: 0x100, Type: TS_STREAM_H264},
			{PID: 0x101, Type: TS_STREAM_AAC},
		}
		h.configure(good, 0x100)
		h.write(0x100, tH264IDR, 0, 0)
		h.since()

		big := make([]ProgramStream, 0, 7)
		for i := 0; i < 7; i++ {
			big = append(big, ProgramStream{
				PID:         0x100 + uint16(i),
				Type:        TS_STREAM_AAC,
				Descriptors: makeDescriptor(0x05, 0x11, 161),
			})
		}
		cases := []struct {
			name    string
			streams []ProgramStream
			pcr     uint16
		}{
			{"empty", nil, 0x100},
			{"unknown-type", []ProgramStream{{PID: 0x100, Type: TS_STREAM_TYPE(0x06)}}, 0x100},
			{"pid-too-small", []ProgramStream{{PID: 0x1F, Type: TS_STREAM_AAC}}, 0x1F},
			{"pid-too-big", []ProgramStream{{PID: 0x1FFF, Type: TS_STREAM_AAC}}, 0x1FFF},
			{"pid-is-pmt", []ProgramStream{{PID: 0x200, Type: TS_STREAM_AAC}}, 0x200},
			{"pid-duplicate", []ProgramStream{
				{PID: 0x100, Type: TS_STREAM_AAC},
				{PID: 0x100, Type: TS_STREAM_H264},
			}, 0x100},
			{"pcr-missing", good, 0x300},
			{"desc-truncated", []ProgramStream{
				{PID: 0x100, Type: TS_STREAM_AAC, Descriptors: []byte{0x05}},
			}, 0x100},
			{"desc-overrun", []ProgramStream{
				{PID: 0x100, Type: TS_STREAM_AAC, Descriptors: []byte{0x05, 0x04, 0x01}},
			}, 0x100},
			{"desc-too-big", []ProgramStream{
				{PID: 0x100, Type: TS_STREAM_AAC, Descriptors: makeDescriptor(0x05, 0x01, 254)},
			}, 0x100},
			{"section-too-big", big, 0x100},
		}
		for _, c := range cases {
			before := len(h.pkgs)
			if err := h.mux.ConfigureProgram(c.streams, c.pcr); err == nil {
				t.Fatalf("%s: ConfigureProgram succeeded, want error", c.name)
			}
			if len(h.pkgs) != before {
				t.Fatalf("%s: failed ConfigureProgram emitted packets", c.name)
			}
		}

		//the previous configuration survived every failed call untouched:
		//no pending publication, and the periodic PMT still shows version 0
		h.write(0x101, tAAC1, 10, 10)
		wantNoTables(t, h.since())
		h.write(0x100, tH264IDR, 5000, 5000)
		out := h.since()
		pmt, _ := h.wantTables(out, 0, 0x100)
		if len(pmt.streams) != 2 || pmt.streams[0].pid != 0x100 || pmt.streams[1].pid != 0x101 {
			t.Fatalf("configuration changed by failed calls: %+v", pmt.streams)
		}
		checkContinuity(t, h.pkgs)
	})

	t.Run("NoMixing", func(t *testing.T) {
		m := NewTSMuxer()
		m.OnPacket = func(pkg []byte) {}
		m.AddStream(TS_STREAM_AAC)
		if err := m.ConfigureProgram([]ProgramStream{{PID: 0x100, Type: TS_STREAM_AAC}}, 0x100); err == nil {
			t.Fatalf("ConfigureProgram after AddStream succeeded, want error")
		}

		m2 := NewTSMuxer()
		m2.OnPacket = func(pkg []byte) {}
		if err := m2.ConfigureProgram([]ProgramStream{{PID: 0x100, Type: TS_STREAM_AAC}}, 0x100); err != nil {
			t.Fatalf("ConfigureProgram failed: %v", err)
		}
		defer func() {
			if recover() == nil {
				t.Fatalf("AddStream after ConfigureProgram did not panic")
			}
		}()
		m2.AddStream(TS_STREAM_AAC)
	})
}

// ---------------------------------------------------------------------------
// TestProgramCompatibility: the legacy AddStream path, byte for byte
// ---------------------------------------------------------------------------

func TestProgramCompatibility(t *testing.T) {
	legacyAAC := []string{
		"474000100000b00d0000c100000001e200c1b3dd64ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
		"474200100002b0120001c10000e100f0000f01000000d8bf3281ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
		"47410030a010000000000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff000001c0001180c00a3100010001110001000101020304",
		"47410130a000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff000001c0001180c00a3100010709110001070905060708",
	}
	legacyH264 := []string{
		"474000100000b00d0000c100000001e200c1b3dd64ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
		"474200100002b0120001c10000e100f0001b010000007b99bf0effffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
		"474100309750000000000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff000001e0001a84c00a310001000111000100010000000109f000000001658899",
		"474100319810000007080000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff000001e0001980c00a3100011c211100011c210000000109f0000001411122",
	}

	run := func(seq []string, writes func(m *TSMuxer)) {
		m := NewTSMuxer()
		var out [][]byte
		m.OnPacket = func(pkg []byte) { out = append(out, append([]byte(nil), pkg...)) }
		writes(m)
		if len(out) != len(seq) {
			t.Fatalf("got %d packets, want %d", len(out), len(seq))
		}
		for i, want := range seq {
			golden, err := hex.DecodeString(want)
			if err != nil {
				t.Fatalf("bad golden hex: %v", err)
			}
			if !bytes.Equal(out[i], golden) {
				t.Fatalf("packet %d differs from legacy bytes\ngot:  %x\nwant: %x", i, out[i], golden)
			}
		}
	}

	//the baseline sequence: a stream added after the first write is not
	//retroactively published, the PMT keeps advertising a single stream
	run(legacyAAC, func(m *TSMuxer) {
		a := m.AddStream(TS_STREAM_AAC)
		if err := m.Write(a, []byte{1, 2, 3, 4}, 0, 0); err != nil {
			t.Fatalf("write failed: %v", err)
		}
		b := m.AddStream(TS_STREAM_AAC)
		if err := m.Write(b, []byte{5, 6, 7, 8}, 10, 10); err != nil {
			t.Fatalf("write failed: %v", err)
		}
	})

	//h264: automatic AUD insertion, pcr auto-selection and keyframe flags
	run(legacyH264, func(m *TSMuxer) {
		v := m.AddStream(TS_STREAM_H264)
		if err := m.Write(v, []byte{0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x99}, 0, 0); err != nil {
			t.Fatalf("write failed: %v", err)
		}
		if err := m.Write(v, []byte{0x00, 0x00, 0x01, 0x41, 0x11, 0x22}, 40, 40); err != nil {
			t.Fatalf("write failed: %v", err)
		}
	})
}
