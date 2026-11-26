package mpeg2_test

import (
	"bytes"
	"encoding/binary"
	"os"
	"testing"

	"github.com/yapingcat/gomedia/go-mpeg2"
)

// ---------------------------------------------------------------------------
// Independent TS reassembly helpers (prefix "put" to avoid symbol clashes)
// ---------------------------------------------------------------------------

type putPacket struct {
	raw     []byte
	pid     uint16
	pusi    bool
	cc      byte
	afc     byte
	payload []byte
}

type putRecorder struct {
	pkts [][]byte
}

func putNewMux(t *testing.T) (*mpeg2.TSMuxer, *putRecorder) {
	t.Helper()
	r := &putRecorder{}
	m := mpeg2.NewTSMuxer()
	m.OnPacket = func(p []byte) {
		if len(p) != 188 {
			t.Errorf("on packet: expected 188 bytes, got %d", len(p))
		}
		cp := append([]byte(nil), p...)
		r.pkts = append(r.pkts, cp)
	}
	return m, r
}

func putParsePackets(t *testing.T, pkts [][]byte) []putPacket {
	t.Helper()
	out := make([]putPacket, 0, len(pkts))
	for _, p := range pkts {
		if len(p) != 188 {
			t.Fatalf("bad TS packet length %d", len(p))
		}
		if p[0] != 0x47 {
			t.Fatalf("bad sync byte 0x%02x", p[0])
		}
		pkt := putPacket{
			raw:  p,
			pid:  uint16(p[1]&0x1f)<<8 | uint16(p[2]),
			pusi: p[1]&0x40 != 0,
			cc:   p[3] & 0x0f,
			afc:  (p[3] >> 4) & 0x03,
		}
		off := 4
		if pkt.afc&0x02 != 0 {
			al := int(p[4])
			off = 5 + al
			if off > 188 {
				t.Fatalf("adaptation field length %d overruns packet", al)
			}
		}
		if pkt.afc&0x01 != 0 {
			pkt.payload = p[off:]
		}
		out = append(out, pkt)
	}
	return out
}

// putAssertCCContinuous verifies per-PID continuity counters start at 0 and
// advance one per emitted packet with mod-16 wrap.
func putAssertCCContinuous(t *testing.T, pkts [][]byte) {
	t.Helper()
	parsed := putParsePackets(t, pkts)
	seen := map[uint16]bool{}
	last := map[uint16]byte{}
	for _, p := range parsed {
		if !seen[p.pid] {
			if p.cc != 0 {
				t.Errorf("pid 0x%04x first cc=%d, want 0", p.pid, p.cc)
			}
			seen[p.pid] = true
			last[p.pid] = p.cc
			continue
		}
		want := (last[p.pid] + 1) % 16
		if p.cc != want {
			t.Errorf("pid 0x%04x cc discontinuity: got %d want %d", p.pid, p.cc, want)
		}
		last[p.pid] = p.cc
	}
}

// putCollectSections reassembles every complete section carried on pid,
// joining first/continuation TS packets independently of the muxer code.
func putCollectSections(t *testing.T, pkts [][]byte, pid uint16) [][]byte {
	t.Helper()
	var sections [][]byte
	var buf []byte
	flush := func() {
		for len(buf) >= 3 {
			sl := int(buf[1]&0x0f)<<8 | int(buf[2])
			total := 3 + sl
			if sl < 0 || total > len(buf) {
				return
			}
			sections = append(sections, append([]byte(nil), buf[:total]...))
			buf = append([]byte(nil), buf[total:]...)
		}
	}
	for _, p := range putParsePackets(t, pkts) {
		if p.pid != pid {
			continue
		}
		if p.pusi {
			buf = nil
			if len(p.payload) < 1 {
				t.Fatalf("pid 0x%04x PUSI packet without pointer byte", pid)
			}
			pointer := int(p.payload[0])
			if 1+pointer > len(p.payload) {
				t.Fatalf("pid 0x%04x bad pointer_field %d", pid, pointer)
			}
			buf = append([]byte(nil), p.payload[1+pointer:]...)
		} else {
			buf = append(buf, p.payload...)
		}
		flush()
	}
	return sections
}

func putMPEG2CRC(data []byte) uint32 {
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

func putAssertCRC(t *testing.T, sec []byte) {
	t.Helper()
	if len(sec) < 4 {
		t.Fatalf("section too short for CRC: %d", len(sec))
	}
	want := putMPEG2CRC(sec[:len(sec)-4])
	got := binary.BigEndian.Uint32(sec[len(sec)-4:])
	if want != got {
		t.Fatalf("PMT/PAT CRC mismatch: calc %08x wire %08x", want, got)
	}
}

type putES struct {
	streamType  byte
	pid         uint16
	descriptors []byte
}

type putPMT struct {
	version    byte
	pcrPID     uint16
	programNum uint16
	streams    []putES
}

func putParsePMT(t *testing.T, sec []byte) putPMT {
	t.Helper()
	if sec[0] != 0x02 {
		t.Fatalf("not a PMT table id: 0x%02x", sec[0])
	}
	putAssertCRC(t, sec)
	sl := int(sec[1]&0x0f)<<8 | int(sec[2])
	if 3+sl != len(sec) {
		t.Fatalf("section_length %d gives %d bytes, reassembled %d", sl, 3+sl, len(sec))
	}
	info := putPMT{
		programNum: binary.BigEndian.Uint16(sec[3:5]),
		version:    (sec[5] >> 1) & 0x1f,
		pcrPID:     binary.BigEndian.Uint16(sec[8:10]) & 0x1fff,
	}
	pil := int(binary.BigEndian.Uint16(sec[10:12]) & 0x0fff)
	if pil != 0 {
		t.Fatalf("unexpected program_info_length %d", pil)
	}
	off := 12 + pil
	end := 3 + sl - 4 // exclude CRC
	for off < end {
		if off+5 > end {
			t.Fatalf("truncated elementary stream loop at %d/%d", off, end)
		}
		e := putES{
			streamType: sec[off],
			pid:        binary.BigEndian.Uint16(sec[off+1:off+3]) & 0x1fff,
		}
		eil := int(binary.BigEndian.Uint16(sec[off+3:off+5]) & 0x0fff)
		if off+5+eil > end {
			t.Fatalf("truncated ES descriptors at %d", off)
		}
		e.descriptors = append([]byte(nil), sec[off+5:off+5+eil]...)
		info.streams = append(info.streams, e)
		off += 5 + eil
	}
	if off != end {
		t.Fatalf("PMT loop ended at %d, want %d", off, end)
	}
	return info
}

func putParsePATVersion(t *testing.T, sec []byte) (version byte, programPID uint16) {
	t.Helper()
	if sec[0] != 0x00 {
		t.Fatalf("not a PAT table id: 0x%02x", sec[0])
	}
	putAssertCRC(t, sec)
	version = (sec[5] >> 1) & 0x1f
	programPID = binary.BigEndian.Uint16(sec[10:12]) & 0x1fff
	return version, programPID
}

// putCollectES reassembles PES packets for one PID and returns their
// elementary-stream payloads (using PES_packet_length to delimit).
func putCollectES(t *testing.T, pkts [][]byte, pid uint16) [][]byte {
	t.Helper()
	var rawPES [][]byte
	var cur []byte
	have := false
	for _, p := range putParsePackets(t, pkts) {
		if p.pid != pid {
			continue
		}
		if p.pusi {
			if have {
				rawPES = append(rawPES, cur)
			}
			cur = append([]byte(nil), p.payload...)
			have = true
		} else if have {
			cur = append(cur, p.payload...)
		}
	}
	if have {
		rawPES = append(rawPES, cur)
	}
	var es [][]byte
	for _, ps := range rawPES {
		if len(ps) < 9 || ps[0] != 0 || ps[1] != 0 || ps[2] != 1 {
			t.Fatalf("bad PES start code: % x", firstBytes(ps, 4))
		}
		headerLen := 9 + int(ps[8])
		pesLen := int(binary.BigEndian.Uint16(ps[4:6]))
		end := len(ps)
		if pesLen > 0 {
			end = 6 + pesLen
		}
		if headerLen > end || end > len(ps) {
			t.Fatalf("bad PES lengths header=%d pes_end=%d total=%d", headerLen, end, len(ps))
		}
		es = append(es, append([]byte(nil), ps[headerLen:end]...))
	}
	return es
}

func firstBytes(b []byte, n int) []byte {
	if len(b) < n {
		return b
	}
	return b[:n]
}

func putDesc(tag byte, body ...byte) []byte {
	return append([]byte{tag, byte(len(body))}, body...)
}

func putMustConfigure(t *testing.T, m *mpeg2.TSMuxer, streams []mpeg2.ProgramStream, pcr uint16) {
	t.Helper()
	if err := m.ConfigureProgram(streams, pcr); err != nil {
		t.Fatalf("ConfigureProgram(%d streams, pcr 0x%04x): %v", len(streams), pcr, err)
	}
}

func putExpectPMT(t *testing.T, r *putRecorder, want []mpeg2.ProgramStream, pcr uint16, version byte) putPMT {
	t.Helper()
	secs := putCollectSections(t, r.pkts, 0x0200)
	if len(secs) == 0 {
		t.Fatalf("no PMT section emitted")
	}
	info := putParsePMT(t, secs[len(secs)-1])
	if info.version != version {
		t.Fatalf("PMT version=%d want %d", info.version, version)
	}
	if info.pcrPID != pcr {
		t.Fatalf("PMT PCR PID=0x%04x want 0x%04x", info.pcrPID, pcr)
	}
	if info.programNum != 1 {
		t.Fatalf("program_number=%d want 1", info.programNum)
	}
	if len(info.streams) != len(want) {
		t.Fatalf("PMT advertised %d streams, want %d", len(info.streams), len(want))
	}
	for i, w := range want {
		g := info.streams[i]
		if g.streamType != byte(w.Type) || g.pid != w.PID || !bytes.Equal(g.descriptors, w.Descriptors) {
			t.Fatalf("PMT stream %d mismatch: got type=0x%02x pid=0x%04x desc=% x; want type=0x%02x pid=0x%04x desc=% x",
				i, g.streamType, g.pid, g.descriptors, byte(w.Type), w.PID, w.Descriptors)
		}
	}
	return info
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

func TestProgramReplacement(t *testing.T) {
	// ---- phase 1: retain / remove / re-add / PCR switch / invalid Write ----
	m, r := putNewMux(t)

	c0 := []mpeg2.ProgramStream{
		{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC},
		{PID: 0x0101, Type: mpeg2.TS_STREAM_AUDIO_MPEG1, Descriptors: putDesc(0x05, 'a', 'b', 'c')},
		{PID: 0x0102, Type: mpeg2.TS_STREAM_H264},
	}
	origDesc := append([]byte(nil), c0[1].Descriptors...)
	putMustConfigure(t, m, c0, 0x0100)
	if len(r.pkts) != 0 {
		t.Fatalf("ConfigureProgram emitted %d packets", len(r.pkts))
	}

	// Mutating the caller descriptor storage after configuration must not
	// affect the private copy that gets announced.
	c0[1].Descriptors[2] = 0xEE

	es100a := []byte{0xDE, 0xAD, 0xBE, 0xEF}
	if err := m.Write(0x0100, es100a, 0, 0); err != nil {
		t.Fatal(err)
	}
	es101a := []byte("second-pid-payload")
	if err := m.Write(0x0101, es101a, 5, 5); err != nil {
		t.Fatal(err)
	}

	want0 := []mpeg2.ProgramStream{
		c0[0],
		{PID: 0x0101, Type: mpeg2.TS_STREAM_AUDIO_MPEG1, Descriptors: origDesc},
		c0[2],
	}
	putExpectPMT(t, r, want0, 0x0100, 0)
	if es := putCollectES(t, r.pkts, 0x0100); len(es) != 1 || !bytes.Equal(es[0], es100a) {
		t.Fatalf("pid 0x100 ES mismatch: % x", es)
	}
	if es := putCollectES(t, r.pkts, 0x0101); len(es) != 1 || !bytes.Equal(es[0], es101a) {
		t.Fatalf("pid 0x101 ES mismatch: % x", es)
	}

	// c1: 0x100 retained, 0x101 removed, 0x102 retained, 0x103 added; PCR moves.
	c1 := []mpeg2.ProgramStream{
		{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC},
		{PID: 0x0102, Type: mpeg2.TS_STREAM_H264},
		{PID: 0x0103, Type: mpeg2.TS_STREAM_H265},
	}
	putMustConfigure(t, m, c1, 0x0102)

	beforeInvalid := len(r.pkts)
	if err := m.Write(0x0999, []byte("ghost"), 20, 20); err == nil {
		t.Fatal("Write to unknown PID expected error")
	}
	if err := m.Write(0x0101, []byte("removed"), 20, 20); err == nil {
		t.Fatal("Write to removed PID expected error")
	}
	if len(r.pkts) != beforeInvalid {
		t.Fatalf("invalid Write emitted %d packets", len(r.pkts)-beforeInvalid)
	}

	es100b := []byte{0x42, 0x43, 0x44}
	if err := m.Write(0x0100, es100b, 20, 20); err != nil {
		t.Fatal(err)
	}
	// The valid Write must start with PAT + PMT before this PES.
	parsed := putParsePackets(t, r.pkts)
	mark := beforeInvalid
	if parsed[mark].pid != 0x0000 {
		t.Fatalf("after pending update first packet pid=0x%04x, want PAT", parsed[mark].pid)
	}
	if parsed[mark+1].pid != 0x0200 {
		t.Fatalf("after PAT packet pid=0x%04x, want PMT", parsed[mark+1].pid)
	}
	if parsed[mark+2].pid != 0x0100 {
		t.Fatalf("after PMT packet pid=0x%04x, want media PES", parsed[mark+2].pid)
	}
	putExpectPMT(t, r, c1, 0x0102, 1)
	if es := putCollectES(t, r.pkts, 0x0100); len(es) != 2 || !bytes.Equal(es[1], es100b) {
		t.Fatalf("pid 0x100 second ES mismatch: % x", es)
	}

	// c2: re-add 0x101, remove 0x102; PCR switches back to 0x100.
	c2 := []mpeg2.ProgramStream{
		{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC},
		{PID: 0x0101, Type: mpeg2.TS_STREAM_AUDIO_MPEG1, Descriptors: putDesc(0x05, 'a', 'b', 'c')},
		{PID: 0x0103, Type: mpeg2.TS_STREAM_H265},
	}
	putMustConfigure(t, m, c2, 0x0100)
	es101b := []byte("readded-pid-payload")
	if err := m.Write(0x0101, es101b, 30, 30); err != nil {
		t.Fatal(err)
	}
	putExpectPMT(t, r, c2, 0x0100, 2)
	if es := putCollectES(t, r.pkts, 0x0101); len(es) != 2 || !bytes.Equal(es[1], es101b) {
		t.Fatalf("re-added pid 0x101 ES mismatch: % x", es)
	}
	// 0x101 emitted cc=0 before removal; its first packet after re-add must
	// continue at cc=1.
	var ccs101 []byte
	for _, p := range putParsePackets(t, r.pkts) {
		if p.pid == 0x0101 {
			ccs101 = append(ccs101, p.cc)
		}
	}
	if len(ccs101) < 2 || ccs101[0] != 0 || ccs101[1] != 1 {
		t.Fatalf("pid 0x101 cc sequence %v, want [0 1 ...]", ccs101)
	}

	// Identical configuration must not bump the version; after >400ms the
	// periodic announcement repeats the same PMT version.
	putMustConfigure(t, m, c2, 0x0100)
	es100c := []byte("periodic")
	if err := m.Write(0x0100, es100c, 500, 500); err != nil {
		t.Fatal(err)
	}
	putExpectPMT(t, r, c2, 0x0100, 2)
	secs := putCollectSections(t, r.pkts, 0x0200)
	if len(secs) != 4 {
		t.Fatalf("expected 4 PMT sections (v0,v1,v2,v2-repeat), got %d", len(secs))
	}
	for i, wantV := range []byte{0, 1, 2, 2} {
		if v := (secs[i][5] >> 1) & 0x1f; v != wantV {
			t.Fatalf("PMT section %d version=%d want %d", i, v, wantV)
		}
	}
	if es := putCollectES(t, r.pkts, 0x0100); len(es) != 3 || !bytes.Equal(es[2], es100c) {
		t.Fatalf("pid 0x100 periodic ES mismatch: % x", es)
	}
	putAssertCCContinuous(t, r.pkts)

	// ---- phase 2: 33 successive changes, version wrap at 32 --------------
	m2, r2 := putNewMux(t)
	const changes = 33
	for i := 0; i < changes; i++ {
		cfg := []mpeg2.ProgramStream{
			{PID: 0x0300, Type: mpeg2.TS_STREAM_AAC, Descriptors: []byte{0x06, 0x01, byte(i)}},
			{PID: 0x0301, Type: mpeg2.TS_STREAM_AAC},
		}
		putMustConfigure(t, m2, cfg, 0x0300)
		if i%7 == 3 {
			if err := m2.Write(0x0777, nil, uint64(i)*10, uint64(i)*10); err == nil {
				t.Fatal("invalid Write in change loop expected error")
			}
		}
		payload := []byte{byte(i)}
		// Consecutive writes are only 10ms apart (<400ms): the pending PMT
		// must still be announced every time.
		if err := m2.Write(0x0300, payload, uint64(i)*10, uint64(i)*10); err != nil {
			t.Fatal(err)
		}
	}
	pmts := putCollectSections(t, r2.pkts, 0x0200)
	if len(pmts) != changes {
		t.Fatalf("got %d PMT sections for %d changes, want %d", len(pmts), changes, changes)
	}
	pats := putCollectSections(t, r2.pkts, 0x0000)
	if len(pats) != changes {
		t.Fatalf("got %d PAT sections, want %d", len(pats), changes)
	}
	for i, sec := range pmts {
		info := putParsePMT(t, sec)
		if info.version != byte(i%32) {
			t.Fatalf("change %d PMT version=%d want %d", i, info.version, i%32)
		}
		wantDesc := []byte{0x06, 0x01, byte(i)}
		if len(info.streams) != 2 || !bytes.Equal(info.streams[0].descriptors, wantDesc) {
			t.Fatalf("change %d PMT descriptors % x", i, info.streams[0].descriptors)
		}
	}
	for _, sec := range pats {
		v, pid := putParsePATVersion(t, sec)
		if v != 0 {
			t.Fatalf("PAT version changed to %d during track replacement", v)
		}
		if pid != 0x0200 {
			t.Fatalf("PAT program map PID=0x%04x", pid)
		}
	}
	es300 := putCollectES(t, r2.pkts, 0x0300)
	if len(es300) != changes {
		t.Fatalf("got %d ES payloads, want %d", len(es300), changes)
	}
	for i, p := range es300 {
		if !bytes.Equal(p, []byte{byte(i)}) {
			t.Fatalf("ES payload %d = % x", i, p)
		}
	}
	putAssertCCContinuous(t, r2.pkts)

	// ---- phase 3: rapid configurations coalesce to the last PMT -----------
	m3, r3 := putNewMux(t)
	putMustConfigure(t, m3, []mpeg2.ProgramStream{{PID: 0x0400, Type: mpeg2.TS_STREAM_AAC}}, 0x0400)
	putMustConfigure(t, m3, []mpeg2.ProgramStream{
		{PID: 0x0400, Type: mpeg2.TS_STREAM_AAC},
		{PID: 0x0401, Type: mpeg2.TS_STREAM_AAC},
	}, 0x0400)
	cLast := []mpeg2.ProgramStream{{PID: 0x0401, Type: mpeg2.TS_STREAM_AAC}}
	putMustConfigure(t, m3, cLast, 0x0401)
	rapidES := []byte("last-config-wins")
	if err := m3.Write(0x0401, rapidES, 0, 0); err != nil {
		t.Fatal(err)
	}
	putExpectPMT(t, r3, cLast, 0x0401, 2)
	if secs := putCollectSections(t, r3.pkts, 0x0200); len(secs) != 1 {
		t.Fatalf("rapid configs emitted %d PMT sections, want 1", len(secs))
	}
	if es := putCollectES(t, r3.pkts, 0x0401); len(es) != 1 || !bytes.Equal(es[0], rapidES) {
		t.Fatalf("rapid config ES mismatch: % x", es)
	}
	putAssertCCContinuous(t, r3.pkts)

	// ---- phase 4: every explicit stream type runs through the PES path ----
	m4, r4 := putNewMux(t)
	allTypes := []mpeg2.ProgramStream{
		{PID: 0x0500, Type: mpeg2.TS_STREAM_H264},
		{PID: 0x0501, Type: mpeg2.TS_STREAM_H265},
		{PID: 0x0502, Type: mpeg2.TS_STREAM_AUDIO_MPEG1},
		{PID: 0x0503, Type: mpeg2.TS_STREAM_AUDIO_MPEG2},
		{PID: 0x0504, Type: mpeg2.TS_STREAM_AAC},
	}
	putMustConfigure(t, m4, allTypes, 0x0500)
	videoPayloads := map[uint16][]byte{
		0x0500: {0x00, 0x00, 0x00, 0x01, 0x09, 0xf0, 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x0a},
		0x0501: {0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x10},
		0x0502: {0xff, 0xfb, 0x90, 0x44, 0x00},
		0x0503: {0xff, 0xf3, 0x88, 0x44, 0x00},
		0x0504: {0xff, 0xf1, 0x50, 0x80, 0x01},
	}
	for pid, data := range videoPayloads {
		if err := m4.Write(pid, data, 40, 40); err != nil {
			t.Fatalf("Write pid 0x%04x: %v", pid, err)
		}
	}
	for pid, payload := range videoPayloads {
		es := putCollectES(t, r4.pkts, pid)
		if len(es) != 1 {
			t.Fatalf("pid 0x%04x ES count=%d", pid, len(es))
		}
		// Audio payloads are byte-exact; video carries the supplied AUD so no
		// extra AUD NALU is inserted and the payload stays identical too.
		if !bytes.Equal(es[0], payload) {
			t.Fatalf("pid 0x%04x ES mismatch:\n got % x\nwant % x", pid, es[0], payload)
		}
	}

	// A large payload crosses many TS packets and must reassemble byte-exact.
	big := make([]byte, 4096)
	for i := range big {
		big[i] = byte(i*31 + 7)
	}
	if err := m4.Write(0x0504, big, 60, 60); err != nil {
		t.Fatal(err)
	}
	esBig := putCollectES(t, r4.pkts, 0x0504)
	if len(esBig) != 2 || !bytes.Equal(esBig[1], big) {
		got := 0
		if len(esBig) == 2 {
			got = len(esBig[1])
		}
		t.Fatalf("large ES mismatch: count=%d last-len=%d want %d", len(esBig), got, len(big))
	}
	putAssertCCContinuous(t, r4.pkts)

	// ---- phase 5: order / PCR-only / type changes each bump the version ----
	m5, r5 := putNewMux(t)
	a := mpeg2.ProgramStream{PID: 0x0600, Type: mpeg2.TS_STREAM_AAC, Descriptors: []byte{0x05, 0x01, 0xAA}}
	v := mpeg2.ProgramStream{PID: 0x0601, Type: mpeg2.TS_STREAM_H264}
	putMustConfigure(t, m5, []mpeg2.ProgramStream{a, v}, 0x0600)
	if err := m5.Write(0x0600, []byte("v0"), 0, 0); err != nil {
		t.Fatal(err)
	}
	// identical configuration: version stays 0
	putMustConfigure(t, m5, []mpeg2.ProgramStream{a, v}, 0x0600)
	if err := m5.Write(0x0600, []byte("same"), 10, 10); err != nil {
		t.Fatal(err)
	}
	// reorder only: version 1 and advertised order flips
	putMustConfigure(t, m5, []mpeg2.ProgramStream{v, a}, 0x0600)
	if err := m5.Write(0x0600, []byte("reorder"), 20, 20); err != nil {
		t.Fatal(err)
	}
	// PCR only: version 2
	putMustConfigure(t, m5, []mpeg2.ProgramStream{v, a}, 0x0601)
	if err := m5.Write(0x0601, []byte("pcr"), 30, 30); err != nil {
		t.Fatal(err)
	}
	// type change at a retained PID: version 3
	a2 := a
	a2.Type = mpeg2.TS_STREAM_AUDIO_MPEG2
	putMustConfigure(t, m5, []mpeg2.ProgramStream{v, a2}, 0x0601)
	if err := m5.Write(0x0601, []byte("type"), 40, 40); err != nil {
		t.Fatal(err)
	}
	secs5 := putCollectSections(t, r5.pkts, 0x0200)
	wantVersions := []byte{0, 0, 1, 2, 3}
	if len(secs5) != len(wantVersions) {
		t.Fatalf("phase5 PMT count=%d want %d", len(secs5), len(wantVersions))
	}
	for i, sec := range secs5 {
		if got := (sec[5] >> 1) & 0x1f; got != wantVersions[i] {
			t.Fatalf("phase5 PMT %d version=%d want %d", i, got, wantVersions[i])
		}
	}
	reordered := putParsePMT(t, secs5[2])
	if reordered.streams[0].pid != 0x0601 || reordered.streams[1].pid != 0x0600 {
		t.Fatalf("reorder not advertised: %+v", reordered.streams)
	}
	typeChanged := putParsePMT(t, secs5[4])
	if typeChanged.streams[1].streamType != byte(mpeg2.TS_STREAM_AUDIO_MPEG2) {
		t.Fatalf("type change not advertised: %+v", typeChanged.streams[1])
	}
	if typeChanged.pcrPID != 0x0601 {
		t.Fatalf("PCR-only change not advertised: 0x%04x", typeChanged.pcrPID)
	}
	putAssertCCContinuous(t, r5.pkts)
}

func TestPMTSections(t *testing.T) {
	// A descriptor set large enough to force the PMT section across packets.
	streams := make([]mpeg2.ProgramStream, 8)
	for i := range streams {
		body := make([]byte, 12)
		for j := range body {
			body[j] = byte(0xA0 + j)
		}
		desc := append(putDesc(0x05, 0x01, 0x02, 0x03, 0x04), putDesc(0x81, body...)...)
		streams[i] = mpeg2.ProgramStream{
			PID:         uint16(0x0100 + i),
			Type:        []mpeg2.TS_STREAM_TYPE{mpeg2.TS_STREAM_AAC, mpeg2.TS_STREAM_AUDIO_MPEG1, mpeg2.TS_STREAM_AUDIO_MPEG2, mpeg2.TS_STREAM_H264, mpeg2.TS_STREAM_H265}[i%5],
			Descriptors: desc,
		}
	}
	const pcr = uint16(0x0103)

	m, r := putNewMux(t)
	putMustConfigure(t, m, streams, pcr)
	if err := m.Write(streams[0].PID, []byte("cross-packet"), 0, 0); err != nil {
		t.Fatal(err)
	}

	pmtPkts := make([]putPacket, 0)
	for _, p := range putParsePackets(t, r.pkts) {
		if p.pid == 0x0200 {
			pmtPkts = append(pmtPkts, p)
		}
	}
	if len(pmtPkts) != 2 {
		t.Fatalf("cross-packet PMT used %d TS packets, want 2", len(pmtPkts))
	}
	if !pmtPkts[0].pusi || pmtPkts[0].raw[4] != 0x00 {
		t.Fatal("first PMT packet must have PUSI=1 and pointer_field=0")
	}
	if pmtPkts[1].pusi {
		t.Fatal("continuation PMT packet must have PUSI=0")
	}
	if pmtPkts[0].cc != 0 || pmtPkts[1].cc != 1 {
		t.Fatalf("PMT cc = %d,%d want 0,1", pmtPkts[0].cc, pmtPkts[1].cc)
	}
	// Continuation packet must be completely filled by section bytes.
	if pmtPkts[1].afc != 0x01 || len(pmtPkts[1].payload) != 184 {
		t.Fatalf("continuation payload length %d afc %d", len(pmtPkts[1].payload), pmtPkts[1].afc)
	}

	secs := putCollectSections(t, r.pkts, 0x0200)
	if len(secs) != 1 {
		t.Fatalf("got %d PMT sections", len(secs))
	}
	info := putParsePMT(t, secs[0])
	if info.pcrPID != pcr || info.version != 0 || len(info.streams) != len(streams) {
		t.Fatalf("reassembled PMT wrong: %+v", info)
	}
	for i, w := range streams {
		g := info.streams[i]
		if g.pid != w.PID || g.streamType != byte(w.Type) || !bytes.Equal(g.descriptors, w.Descriptors) {
			t.Fatalf("stream %d descriptors not preserved\n got % x\nwant % x", i, g.descriptors, w.Descriptors)
		}
	}
	// Bytes following the section end in the last PMT packet must be 0xFF.
	sl := int(secs[0][1]&0x0f)<<8 | int(secs[0][2])
	sectionTotal := 3 + sl
	firstCap := 183
	lastRaw := pmtPkts[1].raw
	usedInSecond := sectionTotal - firstCap
	for i := 4 + usedInSecond; i < 188; i++ {
		if lastRaw[i] != 0xFF {
			t.Fatalf("post-section byte %d = 0x%02x, want 0xff", i, lastRaw[i])
		}
	}
	putAssertCCContinuous(t, r.pkts)

	// ---- section_length == 1021 accepted (six PMT packets) -----------------
	big := make([]mpeg2.ProgramStream, 4)
	for i := range big {
		body := make([]byte, 245) // tag + length(245) + 245 body = 247 bytes
		for j := range body {
			body[j] = byte(i*17 + j)
		}
		big[i] = mpeg2.ProgramStream{
			PID:         uint16(0x0220 + i),
			Type:        mpeg2.TS_STREAM_AAC,
			Descriptors: putDesc(0x7A, body...),
		}
	}
	mb, rb := putNewMux(t)
	putMustConfigure(t, mb, big, big[0].PID)
	if err := mb.Write(big[0].PID, []byte("big-section"), 0, 0); err != nil {
		t.Fatal(err)
	}
	bigSecs := putCollectSections(t, rb.pkts, 0x0200)
	if len(bigSecs) != 1 {
		t.Fatalf("1021 section: got %d sections", len(bigSecs))
	}
	if sl := int(bigSecs[0][1]&0x0f)<<8 | int(bigSecs[0][2]); sl != 1021 {
		t.Fatalf("boundary section_length=%d want 1021", sl)
	}
	putParsePMT(t, bigSecs[0]) // validates CRC and descriptor loop
	bigPMTPackets := 0
	for _, p := range putParsePackets(t, rb.pkts) {
		if p.pid == 0x0200 {
			bigPMTPackets++
		}
	}
	if bigPMTPackets != 6 {
		t.Fatalf("1021 section carried by %d packets, want 6", bigPMTPackets)
	}

	// ---- validation failures leave the committed configuration intact -----
	good := []mpeg2.ProgramStream{
		{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC},
		{PID: 0x0101, Type: mpeg2.TS_STREAM_H264},
	}
	goodPCR := uint16(0x0100)

	cases := []struct {
		name    string
		streams []mpeg2.ProgramStream
		pcr     uint16
	}{
		{"empty", nil, 0x0100},
		{"unknown type", []mpeg2.ProgramStream{{PID: 0x0100, Type: mpeg2.TS_STREAM_TYPE(0x77)}}, 0x0100},
		{"pid too low", []mpeg2.ProgramStream{{PID: 0x001F, Type: mpeg2.TS_STREAM_AAC}}, 0x001F},
		{"pid null", []mpeg2.ProgramStream{{PID: 0x1FFF, Type: mpeg2.TS_STREAM_AAC}}, 0x1FFF},
		{"pmt pid", []mpeg2.ProgramStream{{PID: 0x0200, Type: mpeg2.TS_STREAM_AAC}}, 0x0200},
		{"duplicate pid", []mpeg2.ProgramStream{
			{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC},
			{PID: 0x0100, Type: mpeg2.TS_STREAM_H264},
		}, 0x0100},
		{"pcr missing", []mpeg2.ProgramStream{
			{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC},
			{PID: 0x0101, Type: mpeg2.TS_STREAM_H264},
		}, 0x0150},
		{"descriptor truncated header", []mpeg2.ProgramStream{
			{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC, Descriptors: []byte{0x05}},
		}, 0x0100},
		{"descriptor truncated body", []mpeg2.ProgramStream{
			{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC, Descriptors: []byte{0x05, 0x05, 0x01, 0x02}},
		}, 0x0100},
		{"descriptor item too long", []mpeg2.ProgramStream{
			{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC, Descriptors: make([]byte, 256)},
		}, 0x0100},
		{"section too long", func() []mpeg2.ProgramStream {
			s := make([]mpeg2.ProgramStream, 4)
			for i := range s {
				s[i] = mpeg2.ProgramStream{PID: uint16(0x0300 + i), Type: mpeg2.TS_STREAM_AAC, Descriptors: make([]byte, 248)}
			}
			return s
		}(), 0x0300},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			mx, rx := putNewMux(t)
			putMustConfigure(t, mx, good, goodPCR)
			before := len(rx.pkts)
			if err := mx.ConfigureProgram(tc.streams, tc.pcr); err == nil {
				t.Fatal("expected ConfigureProgram error, got nil")
			}
			if len(rx.pkts) != before {
				t.Fatal("rejected ConfigureProgram emitted bytes")
			}
			// The previous configuration must still be live and at version 0.
			if err := mx.Write(0x0100, []byte("still-good"), 0, 0); err != nil {
				t.Fatal(err)
			}
			putExpectPMT(t, rx, good, goodPCR, 0)
		})
	}

	// Exactly 255 descriptor bytes on one item is accepted.
	edge := []mpeg2.ProgramStream{{
		PID:         0x0100,
		Type:        mpeg2.TS_STREAM_AAC,
		Descriptors: putDesc(0x09, make([]byte, 253)...), // 2 + 253 = 255
	}}
	me, re := putNewMux(t)
	putMustConfigure(t, me, edge, 0x0100)
	if err := me.Write(0x0100, []byte("edge"), 0, 0); err != nil {
		t.Fatal(err)
	}
	putExpectPMT(t, re, edge, 0x0100, 0)

	// PID range endpoints 0x20 and 0x1ffe are accepted.
	bounds := []mpeg2.ProgramStream{
		{PID: 0x0020, Type: mpeg2.TS_STREAM_AAC},
		{PID: 0x1FFE, Type: mpeg2.TS_STREAM_H265},
	}
	mb2, rb2 := putNewMux(t)
	putMustConfigure(t, mb2, bounds, 0x1FFE)
	if err := mb2.Write(0x1FFE, []byte("bounds"), 0, 0); err != nil {
		t.Fatal(err)
	}
	putExpectPMT(t, rb2, bounds, 0x1FFE, 0)
}

func TestProgramCompatibility(t *testing.T) {
	// Byte-for-byte golden output of the historical AddStream short sequence.
	golden, err := os.ReadFile("testdata/legacy_addstream_short.bin")
	if err != nil {
		t.Fatal(err)
	}
	m, r := putNewMux(t)
	a := m.AddStream(mpeg2.TS_STREAM_AAC)
	if a != 0x0100 {
		t.Fatalf("first AddStream pid=0x%04x want 0x0100", a)
	}
	if err := m.Write(a, []byte{1, 2, 3, 4}, 0, 0); err != nil {
		t.Fatal(err)
	}
	if err := m.Write(a, []byte{5, 6, 7, 8}, 10, 10); err != nil {
		t.Fatal(err)
	}
	got := bytes.Join(r.pkts, nil)
	if !bytes.Equal(got, golden) {
		t.Fatalf("legacy AddStream output changed:\n got % x\nwant % x", got, golden)
	}

	// The explicit equivalent of the first write encodes the same PAT and
	// PES packets byte-for-byte; the PMT parses to the same program topology
	// (its reserved bits follow the MPEG-2 convention).
	me, re := putNewMux(t)
	putMustConfigure(t, me, []mpeg2.ProgramStream{{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC}}, 0x0100)
	if err := me.Write(0x0100, []byte{1, 2, 3, 4}, 0, 0); err != nil {
		t.Fatal(err)
	}
	if len(re.pkts) != 3 {
		t.Fatalf("explicit equivalent emitted %d packets, want 3", len(re.pkts))
	}
	if !bytes.Equal(re.pkts[0], golden[0:188]) {
		t.Fatal("explicit PAT differs from legacy PAT")
	}
	if !bytes.Equal(re.pkts[2], golden[2*188:3*188]) {
		t.Fatal("explicit PES differs from legacy PES")
	}
	legacySecs := putCollectSections(t, [][]byte{golden[188 : 2*188]}, 0x0200)
	explicitSecs := putCollectSections(t, [][]byte{re.pkts[1]}, 0x0200)
	if len(legacySecs) != 1 || len(explicitSecs) != 1 {
		t.Fatalf("compatibility PMT reassembly failed: %d/%d", len(legacySecs), len(explicitSecs))
	}
	legacyPMT := putParsePMT(t, legacySecs[0])
	explicitPMT := putParsePMT(t, explicitSecs[0])
	if legacyPMT.version != explicitPMT.version || legacyPMT.pcrPID != explicitPMT.pcrPID ||
		len(legacyPMT.streams) != 1 || len(explicitPMT.streams) != 1 ||
		legacyPMT.streams[0].streamType != explicitPMT.streams[0].streamType ||
		legacyPMT.streams[0].pid != explicitPMT.streams[0].pid ||
		!bytes.Equal(legacyPMT.streams[0].descriptors, explicitPMT.streams[0].descriptors) {
		t.Fatalf("PMT semantics differ: legacy %+v explicit %+v", legacyPMT, explicitPMT)
	}

	// The two configuration modes must not be mixed.
	mix1, _ := putNewMux(t)
	mix1.AddStream(mpeg2.TS_STREAM_AAC)
	if err := mix1.ConfigureProgram([]mpeg2.ProgramStream{{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC}}, 0x0100); err == nil {
		t.Fatal("ConfigureProgram after AddStream must be rejected")
	}

	mix2, _ := putNewMux(t)
	putMustConfigure(t, mix2, []mpeg2.ProgramStream{{PID: 0x0100, Type: mpeg2.TS_STREAM_AAC}}, 0x0100)
	func() {
		defer func() {
			if recover() == nil {
				t.Fatal("AddStream after ConfigureProgram must panic")
			}
		}()
		mix2.AddStream(mpeg2.TS_STREAM_AAC)
	}()
}
