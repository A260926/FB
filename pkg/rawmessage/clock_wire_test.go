package rawmessage_test

// The tests in this file pin the media clock and acknowledgement behavior of
// rawmessage.Reader and rawmessage.Writer to the exact bytes on the wire.
// The helpers below form an independent byte model of the RTMP chunk stream
// encoding: they are written from the wire specification and never call
// pkg/chunk, so nothing round-trips through the code under test. Only the
// legal short chunk stream IDs 3, 4 and 6 are used; chunk stream IDs are
// never extended, and neither Aggregate nor Abort chunks are involved.

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"testing"
	"time"

	"github.com/bluenviron/gortmplib/pkg/bytecounter"
	"github.com/bluenviron/gortmplib/pkg/rawmessage"
	"github.com/bluenviron/gortmplib/task_support/taskcheck"
)

const wireSentinel24 = 0xFFFFFF

// wireChunkSpec describes one chunk on the wire, header fields included.
type wireChunkSpec struct {
	fmt     int
	csid    byte
	field24 uint32 // 3-byte big-endian timestamp/delta field of fmt 0/1/2
	hasExt  bool
	extVal  uint32
	typ     uint8  // fmt 0/1 only
	bodyLen uint32 // fmt 0/1 only: declared message body length
	msid    uint32 // fmt 0 only
	body    []byte
}

func appendUint24(buf []byte, v uint32) []byte {
	return append(buf, byte(v>>16), byte(v>>8), byte(v))
}

func appendUint32(buf []byte, v uint32) []byte {
	return append(buf, byte(v>>24), byte(v>>16), byte(v>>8), byte(v))
}

// encodeWireChunk serializes one chunk. The 4-byte message stream ID of fmt 0
// is written big-endian, matching the convention of the fixed sources.
func encodeWireChunk(s wireChunkSpec) []byte {
	var out []byte
	switch s.fmt {
	case 0:
		out = append(out, s.csid)
		out = appendUint24(out, s.field24)
		out = appendUint24(out, s.bodyLen)
		out = append(out, s.typ)
		out = appendUint32(out, s.msid)
	case 1:
		out = append(out, 1<<6|s.csid)
		out = appendUint24(out, s.field24)
		out = appendUint24(out, s.bodyLen)
		out = append(out, s.typ)
	case 2:
		out = append(out, 2<<6|s.csid)
		out = appendUint24(out, s.field24)
	default: // 3
		out = append(out, 3<<6|s.csid)
	}
	if s.hasExt {
		out = appendUint32(out, s.extVal)
	}
	return append(out, s.body...)
}

func encodeWireChunks(specs []wireChunkSpec) []byte {
	var out []byte
	for _, s := range specs {
		out = append(out, encodeWireChunk(s)...)
	}
	return out
}

// parsedChunk is one chunk decoded from the wire.
type parsedChunk struct {
	off     int
	end     int
	fmt     int
	csid    byte
	field24 uint32
	hasExt  bool
	extVal  uint32
	typ     uint8
	bodyLen uint32 // declared by fmt 0/1, inherited by fmt 2/3
	msid    uint32
	body    []byte
}

func uint24Of(b []byte) uint32 {
	return uint32(b[0])<<16 | uint32(b[1])<<8 | uint32(b[2])
}

func uint32Of(b []byte) uint32 {
	return uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3])
}

type wireParseStream struct {
	hasExt  bool
	bodyLen uint32
	recv    uint32
}

// parseWireChunks decodes a chunk stream without using pkg/chunk. Extended
// timestamp inheritance of fmt 3 chunks is resolved from the previous fmt
// 0/1/2 header seen on the same chunk stream.
func parseWireChunks(data []byte, chunkSize uint32) ([]parsedChunk, error) {
	streams := make(map[byte]*wireParseStream)
	var out []parsedChunk
	off := 0
	take := func(n uint32) ([]byte, error) {
		if uint32(len(data)-off) < n {
			return nil, fmt.Errorf("truncated chunk at offset %d", off)
		}
		b := data[off : off+int(n)]
		off += int(n)
		return b, nil
	}

	for off < len(data) {
		start := off
		head, err := take(1)
		if err != nil {
			return nil, err
		}
		f := int(head[0] >> 6)
		csid := head[0] & 0x3F
		st := streams[csid]
		if st == nil {
			st = &wireParseStream{}
			streams[csid] = st
		}
		pc := parsedChunk{off: start, fmt: f, csid: csid}

		switch f {
		case 0:
			hdr, err := take(11)
			if err != nil {
				return nil, err
			}
			pc.field24 = uint24Of(hdr[0:3])
			pc.bodyLen = uint24Of(hdr[3:6])
			pc.typ = hdr[6]
			pc.msid = uint32Of(hdr[7:11])
			if pc.field24 == wireSentinel24 {
				ext, err := take(4)
				if err != nil {
					return nil, err
				}
				pc.hasExt = true
				pc.extVal = uint32Of(ext)
			}
			st.hasExt = pc.hasExt
			st.bodyLen = pc.bodyLen
			n := min(pc.bodyLen, chunkSize)
			body, err := take(n)
			if err != nil {
				return nil, err
			}
			pc.body = body
			st.recv = n
			if n == pc.bodyLen {
				st.recv = 0
			}

		case 1:
			hdr, err := take(7)
			if err != nil {
				return nil, err
			}
			pc.field24 = uint24Of(hdr[0:3])
			pc.bodyLen = uint24Of(hdr[3:6])
			pc.typ = hdr[6]
			if pc.field24 == wireSentinel24 {
				ext, err := take(4)
				if err != nil {
					return nil, err
				}
				pc.hasExt = true
				pc.extVal = uint32Of(ext)
			}
			st.hasExt = pc.hasExt
			st.bodyLen = pc.bodyLen
			n := min(pc.bodyLen, chunkSize)
			body, err := take(n)
			if err != nil {
				return nil, err
			}
			pc.body = body
			st.recv = n
			if n == pc.bodyLen {
				st.recv = 0
			}

		case 2:
			hdr, err := take(3)
			if err != nil {
				return nil, err
			}
			pc.field24 = uint24Of(hdr)
			if pc.field24 == wireSentinel24 {
				ext, err := take(4)
				if err != nil {
					return nil, err
				}
				pc.hasExt = true
				pc.extVal = uint32Of(ext)
			}
			st.hasExt = pc.hasExt
			pc.bodyLen = st.bodyLen
			n := min(st.bodyLen, chunkSize)
			body, err := take(n)
			if err != nil {
				return nil, err
			}
			pc.body = body
			st.recv = n
			if n == st.bodyLen {
				st.recv = 0
			}

		default: // 3
			pc.hasExt = st.hasExt
			if pc.hasExt {
				ext, err := take(4)
				if err != nil {
					return nil, err
				}
				pc.extVal = uint32Of(ext)
			}
			pc.bodyLen = st.bodyLen
			var n uint32
			if st.recv != 0 {
				n = min(st.bodyLen-st.recv, chunkSize)
			} else {
				n = min(st.bodyLen, chunkSize)
			}
			body, err := take(n)
			if err != nil {
				return nil, err
			}
			pc.body = body
			st.recv += n
			if st.recv == st.bodyLen {
				st.recv = 0
			}
		}

		pc.end = off
		out = append(out, pc)
	}
	return out, nil
}

// reconMsg is a media message reassembled from parsed chunks.
type reconMsg struct {
	csid byte
	tsMs uint32
	typ  uint8
	msid uint32
	body []byte
}

// reassembleMessages rebuilds messages from parsed chunks, applying the
// timestamp inheritance rules: fmt 3 continuation fragments keep the clock,
// while a fmt 3 chunk that opens a new message advances it by the delta
// established by the previous fmt 1/2 header on the same chunk stream.
func reassembleMessages(chunks []parsedChunk) []reconMsg {
	type streamState struct {
		ts      uint32
		delta   uint32
		typ     uint8
		msid    uint32
		bodyLen uint32
		recv    uint32
		body    []byte
	}
	streams := make(map[byte]*streamState)
	var out []reconMsg

	for _, ch := range chunks {
		st := streams[ch.csid]
		if st == nil {
			st = &streamState{}
			streams[ch.csid] = st
		}

		switch ch.fmt {
		case 0:
			st.ts = ch.field24
			if ch.hasExt {
				st.ts = ch.extVal
			}
			st.delta = 0
			st.typ = ch.typ
			st.msid = ch.msid
			st.bodyLen = ch.bodyLen
			st.recv = 0
			st.body = st.body[:0]
		case 1:
			d := ch.field24
			if ch.hasExt {
				d = ch.extVal
			}
			st.ts += d
			st.delta = d
			st.typ = ch.typ
			st.bodyLen = ch.bodyLen
			st.recv = 0
			st.body = st.body[:0]
		case 2:
			d := ch.field24
			if ch.hasExt {
				d = ch.extVal
			}
			st.ts += d
			st.delta = d
			st.recv = 0
			st.body = st.body[:0]
		default: // 3
			if st.recv == 0 {
				st.ts += st.delta
			}
		}

		st.body = append(st.body, ch.body...)
		st.recv += uint32(len(ch.body))
		if st.recv == st.bodyLen {
			out = append(out, reconMsg{csid: ch.csid, tsMs: st.ts, typ: st.typ, msid: st.msid, body: st.body})
			st.recv = 0
			st.body = nil
		}
	}
	return out
}

// planMsg is one entry of a media message plan: an independent media
// sequence number plus an integer-millisecond clock (with optional
// sub-millisecond input to verify quantization).
type planMsg struct {
	seq   int
	csid  byte
	msid  uint32
	typ   uint8
	clock time.Duration
	body  []byte
}

func (m planMsg) clockMs() int64 {
	return int64(m.clock / time.Millisecond)
}

// mediaBody builds a fixed, recognizable media payload.
func mediaBody(seq int, n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(0x40 + seq*3 + i)
	}
	return b
}

type writeStreamState struct {
	initialized bool
	msid        uint32
	typ         uint8
	bodyLen     uint32
	ts          int64
	delta       int64
	hasDelta    bool
	hasExt      bool
	extVal      uint32
}

// modelWriterChunks computes the chunk sequence that a plan must produce:
// fmt 0 when the stream starts, the clock goes backward or the message
// stream changes; fmt 1 when type or length change; fmt 2 when the delta
// changes; fmt 3 when the delta repeats. The extended field inherited by
// fmt 3 chunks keeps the value of the previous fmt 0/1/2 header on the same
// chunk stream, i.e. an absolute timestamp after fmt 0 and a delta after
// fmt 1/2. The clock is quantized to milliseconds before deltas are computed
// and before messages are fragmented.
func modelWriterChunks(plan []planMsg, chunkSize uint32) []wireChunkSpec {
	streams := make(map[byte]*writeStreamState)
	var out []wireChunkSpec

	for _, msg := range plan {
		st := streams[msg.csid]
		if st == nil {
			st = &writeStreamState{}
			streams[msg.csid] = st
		}

		ts := msg.clockMs()
		bodyLen := uint32(len(msg.body))

		var delta int64
		hasDelta := false
		if st.initialized {
			if d := ts - st.ts; d >= 0 {
				delta = d
				hasDelta = true
			}
		}

		pos := uint32(0)
		first := true
		for {
			n := min(bodyLen-pos, chunkSize)
			body := msg.body[pos : pos+n]

			if first {
				first = false
				switch {
				case !st.initialized || !hasDelta || st.msid != msg.msid:
					t := uint32(ts)
					spec := wireChunkSpec{fmt: 0, csid: msg.csid, typ: msg.typ, bodyLen: bodyLen, msid: msg.msid, body: body}
					if t >= wireSentinel24 {
						spec.field24 = wireSentinel24
						spec.hasExt = true
						spec.extVal = t
					} else {
						spec.field24 = t
					}
					st.hasExt = spec.hasExt
					st.extVal = t
					out = append(out, spec)

				case st.typ != msg.typ || st.bodyLen != bodyLen:
					d := uint32(delta)
					spec := wireChunkSpec{fmt: 1, csid: msg.csid, typ: msg.typ, bodyLen: bodyLen, body: body}
					if d >= wireSentinel24 {
						spec.field24 = wireSentinel24
						spec.hasExt = true
						spec.extVal = d
					} else {
						spec.field24 = d
					}
					st.hasExt = spec.hasExt
					st.extVal = d
					out = append(out, spec)

				case !st.hasDelta || st.delta != delta:
					d := uint32(delta)
					spec := wireChunkSpec{fmt: 2, csid: msg.csid, body: body}
					if d >= wireSentinel24 {
						spec.field24 = wireSentinel24
						spec.hasExt = true
						spec.extVal = d
					} else {
						spec.field24 = d
					}
					st.hasExt = spec.hasExt
					st.extVal = d
					out = append(out, spec)

				default:
					out = append(out, wireChunkSpec{fmt: 3, csid: msg.csid, hasExt: st.hasExt, extVal: st.extVal, body: body})
				}
			} else {
				out = append(out, wireChunkSpec{fmt: 3, csid: msg.csid, hasExt: st.hasExt, extVal: st.extVal, body: body})
			}

			pos += n
			if (bodyLen - pos) == 0 {
				break
			}
		}

		st.initialized = true
		st.msid = msg.msid
		st.typ = msg.typ
		st.bodyLen = bodyLen
		st.ts = ts
		st.delta = delta
		st.hasDelta = hasDelta
	}
	return out
}

// pieceReader feeds data in controlled short blocks and records how many
// bytes actually left the underlying reader.
type pieceReader struct {
	data  []byte
	sizes []int
	off   int
	n     int
}

func (p *pieceReader) Read(buf []byte) (int, error) {
	if p.off >= len(p.data) {
		return 0, io.EOF
	}
	sz := p.sizes[p.n%len(p.sizes)]
	p.n++
	n := min(sz, len(p.data)-p.off, len(buf))
	copy(buf, p.data[p.off:p.off+n])
	p.off += n
	return n, nil
}

func checkWireChunk(c *taskcheck.C, idx int, want wireChunkSpec, got parsedChunk, raw []byte) {
	c.Check(got.fmt == want.fmt, "chunk %d: fmt: got %d want %d", idx, got.fmt, want.fmt)
	c.Check(got.csid == want.csid, "chunk %d: chunk stream ID: got %d want %d", idx, got.csid, want.csid)
	if want.fmt <= 2 {
		c.Check(got.field24 == want.field24,
			"chunk %d: 3-byte big-endian time field: got 0x%06X want 0x%06X", idx, got.field24, want.field24)
	}
	c.Check(got.hasExt == want.hasExt, "chunk %d: extended timestamp presence: got %v want %v", idx, got.hasExt, want.hasExt)
	if want.hasExt {
		c.Check(got.extVal == want.extVal,
			"chunk %d: extended timestamp field: got 0x%08X want 0x%08X", idx, got.extVal, want.extVal)
	}
	if want.fmt <= 1 {
		c.Check(got.typ == want.typ, "chunk %d: type: got %d want %d", idx, got.typ, want.typ)
		c.Check(got.bodyLen == want.bodyLen, "chunk %d: 3-byte big-endian length field: got %d want %d",
			idx, got.bodyLen, want.bodyLen)
	}
	if want.fmt == 0 {
		c.Check(got.msid == want.msid, "chunk %d: message stream ID: got %d want %d", idx, got.msid, want.msid)
	}
	c.Check(bytes.Equal(got.body, want.body), "chunk %d: body bytes differ", idx)
	if got.end <= len(raw) {
		c.Check(bytes.Equal(raw[got.off:got.end], encodeWireChunk(want)),
			"chunk %d: raw bytes: got % X want % X", idx, raw[got.off:got.end], encodeWireChunk(want))
	}
}

// modelAckEvents reproduces the acknowledgement schedule: after every
// complete chunk, if uint32(count)-last exceeds the window, one callback
// fires and last advances by exactly one window.
func modelAckEvents(consumed []int, countOf func(consumed int) uint64, window uint32, initLast uint32) ([]uint32, uint32) {
	var counts []uint32
	last := initLast
	for _, cns := range consumed {
		count := uint32(countOf(cns))
		if count-last > window {
			counts = append(counts, count)
			last += window
		}
	}
	return counts, last
}

func TestClockWireWriter(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const chunkSize = 16
		ms := time.Millisecond
		us := time.Microsecond

		plan := []planMsg{
			// CSID 4: fmt 0 opens the stream, fmt 2 establishes the delta,
			// fmt 3 reuses it for the next message.
			{seq: 0, csid: 4, msid: 1, typ: 9, clock: 1000 * ms, body: mediaBody(0, 10)},
			{seq: 1, csid: 4, msid: 1, typ: 9, clock: 1040 * ms, body: mediaBody(1, 10)},
			{seq: 2, csid: 4, msid: 1, typ: 9, clock: 1080 * ms, body: mediaBody(2, 10)},
			// CSID 6 interleave: per-stream state, delta 20, then a delta-0 pair.
			{seq: 3, csid: 6, msid: 1, typ: 8, clock: 2000 * ms, body: mediaBody(3, 8)},
			{seq: 4, csid: 6, msid: 1, typ: 8, clock: 2020 * ms, body: mediaBody(4, 8)},
			{seq: 5, csid: 6, msid: 1, typ: 8, clock: 2040 * ms, body: mediaBody(5, 8)},
			{seq: 6, csid: 6, msid: 1, typ: 8, clock: 2040 * ms, body: mediaBody(6, 8)},
			{seq: 7, csid: 6, msid: 1, typ: 8, clock: 2040 * ms, body: mediaBody(7, 8)},
			// CSID 3: one message on the third legal short chunk stream ID.
			{seq: 8, csid: 3, msid: 2, typ: 9, clock: 5000 * ms, body: mediaBody(8, 6)},
			// CSID 4 resumes, unaffected by the interleave.
			{seq: 9, csid: 4, msid: 1, typ: 8, clock: 1120 * ms, body: mediaBody(9, 12)},  // fmt 1: type+length change
			{seq: 10, csid: 4, msid: 1, typ: 8, clock: 900 * ms, body: mediaBody(10, 12)}, // fmt 0: clock goes backward
			{seq: 11, csid: 4, msid: 2, typ: 8, clock: 940 * ms, body: mediaBody(11, 12)}, // fmt 0: message stream change
			// sub-millisecond input: quantization happens before the delta and
			// before fragmentation, so seq 13 still matches delta 40 and uses fmt 3.
			{seq: 12, csid: 4, msid: 2, typ: 8, clock: 980*ms + 500*us, body: mediaBody(12, 40)},  // fmt 1 + fragments
			{seq: 13, csid: 4, msid: 2, typ: 8, clock: 1020*ms + 400*us, body: mediaBody(13, 40)}, // fmt 3 + fragments
			// extended timestamps on both sides of 0x1000000.
			{seq: 14, csid: 4, msid: 3, typ: 8, clock: 0x1000000 * ms, body: mediaBody(14, 40)}, // fmt 0, extended absolute;
			// its fmt 3 fragments inherit the absolute value
			{seq: 15, csid: 4, msid: 3, typ: 8, clock: (0x1000000 + 0xFFFFFE) * ms, body: mediaBody(15, 40)},                   // fmt 2, 0xFFFFFE stays in 3 bytes
			{seq: 16, csid: 4, msid: 3, typ: 8, clock: (0x1000000 + 0xFFFFFE + 0xFFFFFF) * ms, body: mediaBody(16, 40)},        // fmt 2, sentinel + extended delta
			{seq: 17, csid: 4, msid: 3, typ: 8, clock: (0x1000000 + 0xFFFFFE + 2*0xFFFFFF) * ms, body: mediaBody(17, 40)},      // fmt 3 inherits the extended delta
			{seq: 18, csid: 4, msid: 3, typ: 8, clock: (0x1000000 + 0xFFFFFE + 2*0xFFFFFF + 40) * ms, body: mediaBody(18, 40)}, // fmt 2
			{seq: 19, csid: 4, msid: 3, typ: 8, clock: (0x1000000 + 0xFFFFFE + 2*0xFFFFFF + 80) * ms, body: mediaBody(19, 40)}, // fmt 3
		}

		var buf bytes.Buffer
		w := &rawmessage.Writer{BW: &buf}
		w.Initialize()
		w.SetChunkSize(chunkSize)

		for i := range plan {
			err := w.Write(&rawmessage.Message{
				ChunkStreamID:   plan[i].csid,
				Timestamp:       plan[i].clock,
				Type:            plan[i].typ,
				MessageStreamID: plan[i].msid,
				Body:            plan[i].body,
			})
			c.Check(err == nil, "seq %d: write failed: %v", plan[i].seq, err)
		}

		want := modelWriterChunks(plan, chunkSize)

		raw := buf.Bytes()
		wantLen := 0
		for _, spec := range want {
			wantLen += len(encodeWireChunk(spec))
		}
		c.Check(len(raw) == wantLen, "wire size: got %d want %d", len(raw), wantLen)

		parsed, err := parseWireChunks(raw, chunkSize)
		c.Check(err == nil, "parse wire bytes: %v", err)
		if err != nil {
			return
		}
		c.Check(len(parsed) == len(want), "chunk count: got %d want %d", len(parsed), len(want))

		n := min(len(parsed), len(want))
		for i := 0; i < n; i++ {
			checkWireChunk(c, i, want[i], parsed[i], raw)
		}

		// spot-check the fmt selection at the key transitions.
		spotFmt := map[int]int{
			0: 0, 1: 2, 2: 3, // CSID 4: fmt 0 -> fmt 2 -> fmt 3
			9:  1,        // type+length change -> fmt 1
			10: 0,        // backward clock -> fmt 0
			11: 0,        // message stream change -> fmt 0
			12: 1, 15: 3, // fragmented fmt 1, then fmt 3 on quantized delta
			18: 0, // extended absolute timestamp -> fmt 0
			21: 2, // delta 0xFFFFFE in the 3-byte field
			24: 2, // delta 0xFFFFFF -> sentinel + extended field
			27: 3, // fmt 3 inheriting the extended delta
			30: 2, // fmt 2 after the delta shrank back to 40
			33: 3, // fmt 3 on the repeated delta
		}
		for idx, wantFmt := range spotFmt {
			if idx < len(parsed) {
				c.Check(parsed[idx].fmt == wantFmt, "spot chunk %d: fmt: got %d want %d", idx, parsed[idx].fmt, wantFmt)
			}
		}

		// the fmt 0 chunk that switches to a non-zero message stream ID must
		// carry it in the 4 message-stream-ID bytes, in the established order.
		for i := 0; i < n; i++ {
			if want[i].fmt == 0 && want[i].msid == 2 && parsed[i].fmt == 0 {
				got := raw[parsed[i].off+8 : parsed[i].off+12]
				c.Check(bytes.Equal(got, []byte{0, 0, 0, 2}),
					"chunk %d: message stream ID bytes: got % X want 00 00 00 02", i, got)
			}
		}

		// reassemble messages from the parsed chunks and compare the media
		// clock, fields and payloads against the plan.
		gotMsgs := reassembleMessages(parsed)
		c.Check(len(gotMsgs) == len(plan), "reassembled message count: got %d want %d", len(gotMsgs), len(plan))
		m := min(len(gotMsgs), len(plan))
		for i := 0; i < m; i++ {
			wantTs := uint32(plan[i].clockMs())
			c.Check(gotMsgs[i].csid == plan[i].csid, "seq %d: reassembled chunk stream ID: got %d want %d",
				plan[i].seq, gotMsgs[i].csid, plan[i].csid)
			c.Check(gotMsgs[i].tsMs == wantTs, "seq %d: reassembled timestamp: got %d ms want %d ms",
				plan[i].seq, gotMsgs[i].tsMs, wantTs)
			c.Check(gotMsgs[i].typ == plan[i].typ, "seq %d: reassembled type: got %d want %d",
				plan[i].seq, gotMsgs[i].typ, plan[i].typ)
			c.Check(gotMsgs[i].msid == plan[i].msid, "seq %d: reassembled message stream ID: got %d want %d",
				plan[i].seq, gotMsgs[i].msid, plan[i].msid)
			c.Check(bytes.Equal(gotMsgs[i].body, plan[i].body), "seq %d: reassembled body differs", plan[i].seq)
		}
	})
}

func TestClockWireReader(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const chunkSize = 16

		bodyA := mediaBody(31, 16)
		bodyB := mediaBody(32, 8)
		bodyC := mediaBody(33, 40)
		bodyD := mediaBody(34, 8)
		bodyE := mediaBody(35, 8)
		bodyF := mediaBody(36, 40)
		bodyG := mediaBody(37, 40)

		// hand-written chunk stream: two chunk streams interleave in the
		// middle of a media message, carrying different deltas and extended
		// timestamp states.
		chunks := []wireChunkSpec{
			// CSID 4 message C opens with an extended absolute timestamp and
			// is delivered in three fragments, interleaved with CSID 6.
			{fmt: 0, csid: 4, field24: wireSentinel24, hasExt: true, extVal: 0xFFFFFFF0, typ: 9, bodyLen: 40, msid: 7, body: bodyC[:16]},
			{fmt: 0, csid: 6, field24: 500, typ: 8, bodyLen: 16, msid: 3, body: bodyA},
			{fmt: 3, csid: 4, hasExt: true, extVal: 0xFFFFFFF0, body: bodyC[16:32]},
			{fmt: 1, csid: 6, field24: 20, typ: 9, bodyLen: 8, body: bodyB}, // fmt 1 establishes delta 20
			{fmt: 3, csid: 4, hasExt: true, extVal: 0xFFFFFFF0, body: bodyC[32:]},
			// fmt 2 establishes delta 0x20 on CSID 4 and pushes the clock past 2^32.
			{fmt: 2, csid: 4, field24: 0x20, body: bodyF[:16]},
			{fmt: 3, csid: 4, body: bodyF[16:32]},
			{fmt: 2, csid: 6, field24: 20, body: bodyD},
			{fmt: 3, csid: 6, body: bodyE}, // fmt 3 opens the next CSID 6 message
			{fmt: 3, csid: 4, body: bodyF[32:]},
			{fmt: 3, csid: 4, body: bodyG[:16]}, // fmt 3 opens the next CSID 4 message
			{fmt: 3, csid: 4, body: bodyG[16:32]},
			{fmt: 3, csid: 4, body: bodyG[32:]},
		}
		wire := encodeWireChunks(chunks)

		manifest := []reconMsg{
			{csid: 6, tsMs: 500, typ: 8, msid: 3, body: bodyA},
			{csid: 6, tsMs: 520, typ: 9, msid: 3, body: bodyB},
			{csid: 4, tsMs: 0xFFFFFFF0, typ: 9, msid: 7, body: bodyC},
			{csid: 6, tsMs: 540, typ: 9, msid: 3, body: bodyD},
			{csid: 6, tsMs: 560, typ: 9, msid: 3, body: bodyE},
			{csid: 4, tsMs: 16, typ: 9, msid: 7, body: bodyF}, // 0xFFFFFFF0 + 0x20 wrapped to uint32
			{csid: 4, tsMs: 48, typ: 9, msid: 7, body: bodyG},
		}

		feedModes := []struct {
			name  string
			sizes []int
		}{
			{"whole supply", []int{1 << 20}},
			{"one byte at a time", []int{1}},
			{"odd short blocks", []int{3, 1, 4, 1, 5, 9, 2, 6}},
		}

		for _, fm := range feedModes {
			pr := &pieceReader{data: wire, sizes: fm.sizes}
			bcr := bytecounter.NewReader(pr)
			r := &rawmessage.Reader{BR: bcr, BCR: bcr}
			r.Initialize()
			err := r.SetChunkSize(chunkSize)
			c.Check(err == nil, "%s: set chunk size: %v", fm.name, err)

			var delivered []*rawmessage.Message
			for {
				msg, err := r.Read()
				if err != nil {
					c.Check(errors.Is(err, io.EOF), "%s: final read: got error %v, want EOF", fm.name, err)
					break
				}
				delivered = append(delivered, msg)
			}

			c.Check(len(delivered) == len(manifest), "%s: delivered %d messages, want %d", fm.name, len(delivered), len(manifest))
			n := min(len(delivered), len(manifest))
			for i := 0; i < n; i++ {
				got := delivered[i]
				want := manifest[i]
				c.Check(got.ChunkStreamID == want.csid, "%s: message %d: chunk stream ID: got %d want %d",
					fm.name, i, got.ChunkStreamID, want.csid)
				c.Check(got.Timestamp == time.Duration(want.tsMs)*time.Millisecond,
					"%s: message %d: timestamp: got %v want %d ms", fm.name, i, got.Timestamp, want.tsMs)
				c.Check(got.Type == want.typ, "%s: message %d: type: got %d want %d", fm.name, i, got.Type, want.typ)
				c.Check(got.MessageStreamID == want.msid, "%s: message %d: message stream ID: got %d want %d",
					fm.name, i, got.MessageStreamID, want.msid)
				c.Check(bytes.Equal(got.Body, want.body), "%s: message %d: body differs", fm.name, i)
			}
			// later reads must not have mutated the bodies delivered earlier.
			for i := 0; i < n; i++ {
				c.Check(bytes.Equal(delivered[i].Body, manifest[i].body),
					"%s: message %d: body mutated by later reads", fm.name, i)
			}
			c.Check(bcr.Count() == uint64(len(wire)), "%s: underlying bytes read: got %d want %d",
				fm.name, bcr.Count(), len(wire))
		}

		// one complete message followed by a truncated second one: the
		// truncation must surface as an error and no half message may be
		// delivered.
		head := encodeWireChunk(wireChunkSpec{fmt: 0, csid: 6, field24: 500, typ: 8, bodyLen: 4, msid: 3,
			body: []byte{9, 9, 9, 9}})
		second := encodeWireChunk(wireChunkSpec{fmt: 0, csid: 4, field24: wireSentinel24, hasExt: true,
			extVal: 0xFFFFFF, typ: 9, bodyLen: 8, msid: 7, body: []byte{1, 2, 3, 4, 5, 6, 7, 8}})

		truncations := []struct {
			name string
			cut  int
		}{
			{"plain header", 6},    // inside the 12-byte fmt 0 header
			{"extended field", 14}, // inside the 4-byte extended timestamp
			{"body", 21},           // inside the 8-byte body
		}
		for _, tr := range truncations {
			truncWire := append(append([]byte{}, head...), second[:tr.cut]...)
			bcr := bytecounter.NewReader(&pieceReader{data: truncWire, sizes: []int{1 << 20}})
			r := &rawmessage.Reader{BR: bcr, BCR: bcr}
			r.Initialize()
			err := r.SetChunkSize(chunkSize)
			c.Check(err == nil, "truncated %s: set chunk size: %v", tr.name, err)

			msg, err := r.Read()
			c.Check(err == nil, "truncated %s: first message: got error %v", tr.name, err)
			if msg != nil {
				c.Check(bytes.Equal(msg.Body, []byte{9, 9, 9, 9}), "truncated %s: first message body differs", tr.name)
			} else {
				c.Check(false, "truncated %s: first message not delivered", tr.name)
			}

			msg, err = r.Read()
			c.Check(err != nil, "truncated %s: expected an error, got none", tr.name)
			c.Check(msg == nil, "truncated %s: half media message must not be delivered", tr.name)
		}
	})
}

func TestClockWireAck(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		bodyJ1 := mediaBody(51, 600)
		bodyJ2 := mediaBody(52, 30)
		bodyJ4 := mediaBody(54, 30)
		bodyJ6 := mediaBody(56, 30)
		bodyJ7 := mediaBody(57, 30)
		bodyJ8 := mediaBody(58, 30)

		// two chunk streams interleave; the first chunk alone spans several
		// acknowledgement windows and carries an extended timestamp, so the
		// window accounting covers compressed headers, extended fields and
		// continuation headers, not body estimates.
		specs := []wireChunkSpec{
			{fmt: 0, csid: 4, field24: wireSentinel24, hasExt: true, extVal: 0x1000000, typ: 9, bodyLen: 600, msid: 1, body: bodyJ1[:256]},
			{fmt: 0, csid: 6, field24: 2000, typ: 8, bodyLen: 30, msid: 1, body: bodyJ2},
			{fmt: 3, csid: 4, hasExt: true, extVal: 0x1000000, body: bodyJ1[256:512]},
			{fmt: 2, csid: 6, field24: 7, body: bodyJ4},
			{fmt: 3, csid: 4, hasExt: true, extVal: 0x1000000, body: bodyJ1[512:]},
			{fmt: 3, csid: 6, body: bodyJ6},
			{fmt: 3, csid: 6, body: bodyJ7},
			{fmt: 3, csid: 6, body: bodyJ8},
		}
		wire := encodeWireChunks(specs)

		var consumed []int
		sum := 0
		for _, s := range specs {
			sum += len(encodeWireChunk(s))
			consumed = append(consumed, sum)
		}
		total := len(wire)

		runAck := func(name string, sizes []int, base uint64, countOf func(consumed int) uint64) {
			pr := &pieceReader{data: wire, sizes: sizes}
			bcr := bytecounter.NewReader(pr)
			var gotCounts []uint32
			var gotUnderlying []uint64
			r := &rawmessage.Reader{
				BR:  bcr,
				BCR: bcr,
				OnAckNeeded: func(count uint32) error {
					gotCounts = append(gotCounts, count)
					gotUnderlying = append(gotUnderlying, bcr.Count())
					return nil
				},
			}
			r.Initialize()
			err := r.SetChunkSize(256)
			c.Check(err == nil, "%s: set chunk size: %v", name, err)
			r.SetWindowAckSize(100)
			if base != 0 {
				bcr.SetCount(base)
				r.LastAckCount = uint32(base)
			}

			nMsg := 0
			for {
				_, err := r.Read()
				if err != nil {
					c.Check(errors.Is(err, io.EOF), "%s: final read: got error %v, want EOF", name, err)
					break
				}
				nMsg++
			}
			c.Check(nMsg == 6, "%s: delivered %d messages, want 6", name, nMsg)

			wantCounts, wantLast := modelAckEvents(consumed, countOf, 100, uint32(base))
			c.Check(len(gotCounts) == len(wantCounts),
				"%s: ack callback count: got %d want %d", name, len(gotCounts), len(wantCounts))
			m := min(len(gotCounts), len(wantCounts))
			for i := 0; i < m; i++ {
				c.Check(gotCounts[i] == wantCounts[i],
					"%s: ack #%d: count: got %d want %d", name, i, gotCounts[i], wantCounts[i])
				c.Check(uint32(gotUnderlying[i]) == gotCounts[i],
					"%s: ack #%d: callback count %d does not match underlying reads %d",
					name, i, gotCounts[i], gotUnderlying[i])
			}
			c.Check(r.LastAckCount == wantLast, "%s: LastAckCount: got %d want %d", name, r.LastAckCount, wantLast)
			c.Check(bcr.Count() == base+uint64(total),
				"%s: underlying bytes read: got %d want %d", name, bcr.Count(), base+uint64(total))
		}

		// bufio may prefetch, so callback positions follow the bytes that
		// actually entered the underlying reader; each supply mode is checked
		// against its own schedule, not against one shared position list.
		runAck("one byte at a time", []int{1}, 0, func(cns int) uint64 { return uint64(cns) })
		runAck("seven-byte pieces", []int{7}, 0, func(cns int) uint64 {
			n := (cns + 6) / 7 * 7
			if n > total {
				n = total
			}
			return uint64(n)
		})
		runAck("whole supply", []int{1 << 20}, 0, func(int) uint64 { return uint64(total) })
		runAck("wrapped 32-bit counter", []int{1}, 0xFFFFFF00, func(cns int) uint64 { return 0xFFFFFF00 + uint64(cns) })

		// a failing callback must keep its error identity and must not
		// commit LastAckCount.
		errSentinel := errors.New("sentinel ack failure")
		pr := &pieceReader{data: wire, sizes: []int{1}}
		bcr := bytecounter.NewReader(pr)
		calls := 0
		r := &rawmessage.Reader{
			BR:  bcr,
			BCR: bcr,
			OnAckNeeded: func(uint32) error {
				calls++
				return errSentinel
			},
		}
		r.Initialize()
		err := r.SetChunkSize(256)
		c.Check(err == nil, "sentinel: set chunk size: %v", err)
		r.SetWindowAckSize(100)

		msg, err := r.Read()
		c.Check(msg == nil, "sentinel: half media message must not be delivered")
		c.Check(errors.Is(err, errSentinel), "sentinel: error identity lost: got %v", err)
		c.Check(calls == 1, "sentinel: callback count: got %d want 1", calls)
		c.Check(r.LastAckCount == 0, "sentinel: LastAckCount committed on failure: got %d", r.LastAckCount)

		// writer-side acknowledgement debt threshold, driven by a controlled
		// byte counter; no network and no waiting involved.
		var out bytes.Buffer
		bcw := bytecounter.NewWriter(&out)
		w := &rawmessage.Writer{BW: bcw, BCW: bcw, CheckAcknowledge: true}
		w.Initialize()
		w.SetWindowAckSize(1000) // debt threshold = 1000 * 3 / 2 = 1500
		wmsg := &rawmessage.Message{ChunkStreamID: 4, Type: 9, MessageStreamID: 1, Body: mediaBody(60, 4)}

		bcw.SetCount(1500)
		w.SetAcknowledgeValue(0)
		err = w.Write(wmsg)
		c.Check(err == nil, "writer ack: debt equal to the threshold must pass: %v", err)
		sent := out.Len()
		c.Check(sent > 0, "writer ack: accepted write emitted no bytes")

		bcw.SetCount(1501)
		err = w.Write(wmsg)
		c.Check(err != nil && err.Error() == "no acknowledge received within window",
			"writer ack: debt above the threshold: got %v", err)
		c.Check(out.Len() == sent, "writer ack: rejected write emitted bytes")

		// same threshold across a 2^32 wrap of the byte counter.
		bcw.SetCount(1<<32 + 50)
		w.SetAcknowledgeValue(uint32(1<<32 - 1450)) // debt = 1500
		err = w.Write(wmsg)
		c.Check(err == nil, "writer ack: wrapped debt equal to the threshold must pass: %v", err)
		sent = out.Len()

		bcw.SetCount(1<<32 + 51) // debt = 1501
		err = w.Write(wmsg)
		c.Check(err != nil && err.Error() == "no acknowledge received within window",
			"writer ack: wrapped debt above the threshold: got %v", err)
		c.Check(out.Len() == sent, "writer ack: rejected wrapped write emitted bytes")
	})
}
