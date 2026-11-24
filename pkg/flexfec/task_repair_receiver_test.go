// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package flexfec

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"sync"
	"testing"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/task_support/taskcheck"
	"github.com/pion/rtp"
)

const (
	trrMediaSSRC uint32 = 77001
	trrFECSSRC   uint32 = 88002
	trrMediaPT   uint8  = 96
	trrFECPT     uint8  = 97
)

var errTrrBoom = errors.New("trr upstream boom")

type trrGot struct {
	raw   []byte
	attrs interceptor.Attributes
}

type trrScript struct {
	mu       sync.Mutex
	items    [][]byte
	reads    int
	terminal error
	served   int
	hook     func(a interceptor.Attributes, raw []byte, index int) interceptor.Attributes
}

func (s *trrScript) push(raw []byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.items = append(s.items, raw)
}

func (s *trrScript) readCount() int {
	s.mu.Lock()
	defer s.mu.Unlock()

	return s.reads
}

func (s *trrScript) Read(b []byte, a interceptor.Attributes) (int, interceptor.Attributes, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.reads++

	if len(s.items) == 0 {
		if s.terminal != nil {
			return 0, a, s.terminal
		}

		return 0, a, io.EOF
	}

	item := s.items[0]
	s.items = s.items[1:]
	idx := s.served
	s.served++

	if s.hook != nil {
		a = s.hook(a, item, idx)
	}

	return copy(b, item), a, nil
}

type trrMediaOpts struct {
	pt      uint8
	ssrc    uint32
	ts      uint32
	marker  bool
	csrc    []uint32
	extID   uint8
	extData []byte
	padding byte
}

func trrMedia(seq uint16, payload []byte, o trrMediaOpts) []byte {
	if o.pt == 0 {
		o.pt = trrMediaPT
	}
	if o.ssrc == 0 {
		o.ssrc = trrMediaSSRC
	}
	if o.ts == 0 {
		o.ts = 90000 + uint32(seq)*90
	}
	pkt := rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			Marker:         o.marker,
			PayloadType:    o.pt,
			SequenceNumber: seq,
			Timestamp:      o.ts,
			SSRC:           o.ssrc,
			CSRC:           o.csrc,
		},
		Payload: payload,
	}
	if o.extData != nil {
		if err := pkt.SetExtension(o.extID, o.extData); err != nil {
			panic(err)
		}
	}
	if o.padding > 0 {
		pkt.Header.Padding = true
		pkt.Header.PaddingSize = o.padding
	}
	raw, err := pkt.Marshal()
	if err != nil {
		panic(err)
	}

	return raw
}

// trrRepairID hand-constructs one FlexFEC-03 single-mask repair datagram
// protecting the packets in raws at base+offsets[i]. It is independent of the
// production encoder.
func trrRepairID(
	base uint16, offsets []int, raws [][]byte, fecSeq uint16,
	mediaSSRC, fecSSRC uint32, fecPT uint8,
) []byte {
	if len(offsets) != len(raws) {
		panic("offsets/raws length mismatch")
	}

	head := make([]byte, 8)
	maxBody := 0
	vecs := make([][]byte, len(raws))
	for i, raw := range raws {
		v := recoveryVector(raw)
		vecs[i] = v
		for j := 0; j < 8; j++ {
			head[j] ^= v[j]
		}
		if len(v)-8 > maxBody {
			maxBody = len(v) - 8
		}
	}
	// R and F are zero and the top two bits are cleared by the encoder rules.
	head[0] &^= 0xC0

	body := make([]byte, maxBody)
	for _, v := range vecs {
		for j := 8; j < len(v); j++ {
			body[j-8] ^= v[j]
		}
	}

	var mask uint16 = 0x8000 // k bit: only the first 20 byte header is present
	for _, off := range offsets {
		mask |= 1 << (14 - off)
	}

	fecPayload := make([]byte, BaseFec03HeaderSize+maxBody)
	copy(fecPayload[0:8], head)
	fecPayload[8] = 1 // single SSRC count
	binary.BigEndian.PutUint32(fecPayload[12:16], mediaSSRC)
	binary.BigEndian.PutUint16(fecPayload[16:18], base)
	binary.BigEndian.PutUint16(fecPayload[18:20], mask)
	copy(fecPayload[BaseFec03HeaderSize:], body)

	pkt := rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			PayloadType:    fecPT,
			SequenceNumber: fecSeq,
			SSRC:           fecSSRC,
		},
		Payload: fecPayload,
	}
	raw, err := pkt.Marshal()
	if err != nil {
		panic(err)
	}

	return raw
}

func trrRepair(base uint16, offsets []int, raws [][]byte, fecSeq uint16) []byte {
	return trrRepairID(base, offsets, raws, fecSeq, trrMediaSSRC, trrFECSSRC, trrFECPT)
}

func trrInfo() *interceptor.StreamInfo {
	return &interceptor.StreamInfo{
		SSRC:                              trrMediaSSRC,
		PayloadType:                       trrMediaPT,
		SSRCForwardErrorCorrection:        trrFECSSRC,
		PayloadTypeForwardErrorCorrection: trrFECPT,
	}
}

func trrBind(window, maxEq uint16) (*RepairReceiverInterceptor, *trrScript, interceptor.RTPReader, error) {
	ic, err := NewRepairReceiver(window, maxEq)
	if err != nil {
		return nil, nil, nil, err
	}
	up := &trrScript{}
	rd := ic.BindRemoteStream(trrInfo(), up)

	return ic, up, rd, nil
}

// trrDrain reads until the upstream terminal error/EOF, collecting media
// outputs. ErrRepairPending is retried.
func trrDrain(rd interceptor.RTPReader) ([]trrGot, error) {
	buf := make([]byte, 16384)
	var out []trrGot
	for i := 0; i < 100000; i++ {
		n, attrs, err := rd.Read(buf, interceptor.Attributes{})
		if errors.Is(err, ErrRepairPending) {
			continue
		}
		if err != nil {
			return out, err
		}
		out = append(out, trrGot{raw: append([]byte(nil), buf[:n]...), attrs: attrs})
	}

	return out, errors.New("trrDrain: iteration cap exceeded")
}

func trrSeq(raw []byte) uint16 {
	return binary.BigEndian.Uint16(raw[2:4])
}

func trrSeqs(got []trrGot) []uint16 {
	seqs := make([]uint16, 0, len(got))
	for _, g := range got {
		seqs = append(seqs, trrSeq(g.raw))
	}

	return seqs
}

func trrFind(got []trrGot, seq uint16) (trrGot, bool) {
	for _, g := range got {
		if trrSeq(g.raw) == seq {
			return g, true
		}
	}

	return trrGot{}, false
}

func trrCountSeq(got []trrGot, seq uint16) int {
	n := 0
	for _, g := range got {
		if trrSeq(g.raw) == seq {
			n++
		}
	}

	return n
}

func trrRecovered(g trrGot) bool {
	return g.attrs != nil && g.attrs.Get(AttributeRecovered) == true
}

// ---------------------------------------------------------------------------
// TestRepairRank
// ---------------------------------------------------------------------------

func TestRepairRank(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- Production encoder single-loss control ----------------------
		{
			media := make([]rtp.Packet, 5)
			raws := make([][]byte, 5)
			for i := range media {
				media[i] = rtp.Packet{
					Header: rtp.Header{
						Version: 2, Marker: i%2 == 0, PayloadType: trrMediaPT,
						SequenceNumber: uint16(200 + i), Timestamp: 7000 + uint32(i*300), SSRC: trrMediaSSRC,
					},
					Payload: []byte{0x10, 0x20, byte(i), 0x03},
				}
				raw, err := media[i].Marshal()
				c.Check(err == nil, "encoder control marshal %d: %v", i, err)
				raws[i] = raw
			}
			enc := NewFlexEncoder03(trrFECPT, trrFECSSRC)
			fecPkts := enc.EncodeFec(media, 2)
			c.Check(len(fecPkts) >= 1, "production encoder produced fec packets: %d", len(fecPkts))

			_, up, rd, err := trrBind(64, 64)
			c.Check(err == nil, "bind: %v", err)
			for i, raw := range raws {
				if uint16(200+i) == 202 {
					continue // single loss
				}
				up.push(raw)
			}
			for i := range fecPkts {
				raw, mErr := fecPkts[i].Marshal()
				c.Check(mErr == nil, "fec marshal: %v", mErr)
				up.push(raw)
			}

			got, drainErr := trrDrain(rd)
			c.Check(errors.Is(drainErr, io.EOF), "encoder control drain eof, got %v", drainErr)
			c.Check(len(got) == 5, "encoder control outputs=5 got %d seqs=%v", len(got), trrSeqs(got))
			rec, ok := trrFind(got, 202)
			c.Check(ok, "encoder control recovered seq 202, seqs=%v", trrSeqs(got))
			if ok {
				c.Check(trrBytesEqual(rec.raw, raws[2]), "encoder control seq202 bytes exact")
				c.Check(trrRecovered(rec), "encoder control seq202 marked recovered")
			}
			c.Check(trrCountSeq(got, 200) == 1 && trrCountSeq(got, 204) == 1,
				"encoder control originals delivered once")
		}

		// --- Three-row joint rank A+B, B+C, A+B+C ------------------------
		{
			aRaw := trrMedia(301, []byte("A-media-bytes"), trrMediaOpts{})
			bRaw := trrMedia(302, []byte("B"), trrMediaOpts{
				padding: 4, extID: 1, extData: []byte{0xAA, 0xBB, 0xCC},
			})
			cRaw := trrMedia(303, make([]byte, 73), trrMediaOpts{
				csrc:  []uint32{0x11111111, 0x22222222},
				extID: 2, extData: []byte{0x01, 0x02, 0x03, 0x04, 0x05, 0x06},
			})
			baseRaw := trrMedia(300, []byte("base"), trrMediaOpts{})
			edgeRaw := trrMedia(304, []byte("edge"), trrMediaOpts{})
			rAB := trrRepair(301, []int{0, 1}, [][]byte{aRaw, bRaw}, 900)
			rBC := trrRepair(302, []int{0, 1}, [][]byte{bRaw, cRaw}, 901)
			rABC := trrRepair(301, []int{0, 1, 2}, [][]byte{aRaw, bRaw, cRaw}, 902)

			_, up, rd, err := trrBind(64, 64)
			c.Check(err == nil, "rank bind: %v", err)
			for _, raw := range [][]byte{baseRaw, edgeRaw, rAB, rBC, rABC} {
				up.push(raw)
			}

			got, drainErr := trrDrain(rd)
			c.Check(errors.Is(drainErr, io.EOF), "rank drain eof: %v", drainErr)
			c.Check(len(got) == 5, "rank outputs=5 got %d seqs=%v", len(got), trrSeqs(got))
			// Recovered packets are ordered by extended sequence: 301,302,303.
			for _, tc := range []struct {
				seq uint16
				raw []byte
			}{{301, aRaw}, {302, bRaw}, {303, cRaw}} {
				g, found := trrFind(got, tc.seq)
				c.Check(found, "rank recovered seq %d, seqs=%v", tc.seq, trrSeqs(got))
				if found {
					c.Check(trrBytesEqual(g.raw, tc.raw), "rank seq %d exact bytes (ext/padding/csrc)", tc.seq)
					c.Check(trrRecovered(g), "rank seq %d recovered attribute", tc.seq)
				}
				c.Check(trrCountSeq(got, tc.seq) == 1, "rank seq %d exactly once", tc.seq)
			}
			// Ordering of the recovered tail.
			seqs := trrSeqs(got)
			if len(seqs) == 5 {
				c.Check(seqs[2] == 301 && seqs[3] == 302 && seqs[4] == 303,
					"recovered tail ordered 301,302,303 got %v", seqs)
			}
		}

		// --- Under-rank must not guess, then completing row solves --------
		{
			aRaw := trrMedia(401, []byte("A401"), trrMediaOpts{})
			bRaw := trrMedia(402, []byte("B402-longer"), trrMediaOpts{})
			cRaw := trrMedia(403, []byte("C403"), trrMediaOpts{})
			baseRaw := trrMedia(400, []byte("base400"), trrMediaOpts{})
			edgeRaw := trrMedia(404, []byte("edge404"), trrMediaOpts{})
			rAB := trrRepair(401, []int{0, 1}, [][]byte{aRaw, bRaw}, 910)
			rBC := trrRepair(402, []int{0, 1}, [][]byte{bRaw, cRaw}, 911)
			rABC := trrRepair(401, []int{0, 1, 2}, [][]byte{aRaw, bRaw, cRaw}, 912)

			_, up, rd, err := trrBind(64, 64)
			c.Check(err == nil, "underrank bind: %v", err)
			up.push(baseRaw)
			up.push(edgeRaw)
			up.push(rAB)
			up.push(rBC)

			got, drainErr := trrDrain(rd)
			c.Check(errors.Is(drainErr, io.EOF), "underrank drain eof: %v", drainErr)
			c.Check(len(got) == 2, "underrank only base/edge outputs got %v", trrSeqs(got))
			for _, seq := range []uint16{401, 402, 403} {
				c.Check(trrCountSeq(got, seq) == 0, "underrank must not fabricate seq %d", seq)
			}

			up.push(rABC)
			got2, drainErr2 := trrDrain(rd)
			c.Check(errors.Is(drainErr2, io.EOF), "complete drain eof: %v", drainErr2)
			c.Check(len(got2) == 3, "completing row yields 3 got %v", trrSeqs(got2))
			for _, tc := range []struct {
				seq uint16
				raw []byte
			}{{401, aRaw}, {402, bRaw}, {403, cRaw}} {
				g, found := trrFind(got2, tc.seq)
				c.Check(found, "complete recovered %d", tc.seq)
				if found {
					c.Check(trrBytesEqual(g.raw, tc.raw), "complete exact bytes %d", tc.seq)
				}
			}
		}

		// --- Duplicate and linearly dependent legal rows ------------------
		{
			aRaw := trrMedia(501, []byte("A501"), trrMediaOpts{})
			bRaw := trrMedia(502, []byte("B502"), trrMediaOpts{})
			cRaw := trrMedia(503, []byte("C503"), trrMediaOpts{})
			baseRaw := trrMedia(500, []byte("base500"), trrMediaOpts{})
			edgeRaw := trrMedia(504, []byte("edge504"), trrMediaOpts{})
			rAB := trrRepair(501, []int{0, 1}, [][]byte{aRaw, bRaw}, 920)
			rBC := trrRepair(502, []int{0, 1}, [][]byte{bRaw, cRaw}, 921)
			rABC := trrRepair(501, []int{0, 1, 2}, [][]byte{aRaw, bRaw, cRaw}, 922)
			rAC := trrRepair(501, []int{0, 2}, [][]byte{aRaw, cRaw}, 923) // AB xor BC

			_, up, rd, err := trrBind(64, 64)
			c.Check(err == nil, "dup bind: %v", err)
			for _, raw := range [][]byte{baseRaw, edgeRaw, rAB, rBC, rABC, rAB, rAC} {
				up.push(raw)
			}
			got, drainErr := trrDrain(rd)
			c.Check(errors.Is(drainErr, io.EOF), "dup drain eof: %v", drainErr)
			c.Check(len(got) == 5, "dup outputs exactly base/edge+3 got %d %v", len(got), trrSeqs(got))
			for _, seq := range []uint16{501, 502, 503} {
				c.Check(trrCountSeq(got, seq) == 1, "dependent/duplicate rows: seq %d once", seq)
			}
		}
	})
}

// ---------------------------------------------------------------------------
// TestRepairWindow
// ---------------------------------------------------------------------------

func TestRepairWindow(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Newest window sequence numbers are retained; duplicates suppressed.
		{
			_, up, rd, err := trrBind(4, 64)
			c.Check(err == nil, "window bind: %v", err)
			buf := make([]byte, 4096)

			for _, seq := range []uint16{10, 11, 12, 13} {
				up.push(trrMedia(seq, []byte{byte(seq)}, trrMediaOpts{}))
				n, _, rErr := rd.Read(buf, interceptor.Attributes{})
				c.Check(rErr == nil && n > 0, "media seq %d delivered: %v", seq, rErr)
				c.Check(trrSeq(buf[:n]) == seq, "delivered seq %d", seq)
			}

			// Late duplicate within window is not delivered again.
			up.push(trrMedia(12, []byte{byte(12)}, trrMediaOpts{}))
			n, _, dErr := rd.Read(buf, interceptor.Attributes{})
			c.Check(n == 0 && errors.Is(dErr, ErrRepairPending), "dup in-window pending got n=%d err=%v", n, dErr)

			// Advance right edge to 14: seq 10 slides out.
			up.push(trrMedia(14, []byte{14}, trrMediaOpts{}))
			_, _, aErr := rd.Read(buf, interceptor.Attributes{})
			c.Check(aErr == nil, "advance 14: %v", aErr)

			// Repeated 14 is a duplicate.
			up.push(trrMedia(14, []byte{14}, trrMediaOpts{}))
			_, _, d14 := rd.Read(buf, interceptor.Attributes{})
			c.Check(errors.Is(d14, ErrRepairPending), "dup 14 pending: %v", d14)

			// Ancient seq 10 input must not resurrect/deliver.
			up.push(trrMedia(10, []byte{10}, trrMediaOpts{}))
			n, _, oldErr := rd.Read(buf, interceptor.Attributes{})
			c.Check(n == 0 && errors.Is(oldErr, ErrRepairPending), "ancient 10 ignored: %v", oldErr)
		}

		// Sliding out an unknown column discards dependent equations wholesale.
		{
			_, up, rd, err := trrBind(4, 64)
			c.Check(err == nil, "evict bind: %v", err)
			xRaw := trrMedia(20, []byte("X-unknown"), trrMediaOpts{})
			yRaw := trrMedia(21, []byte("Y-unknown"), trrMediaOpts{})
			rXY := trrRepair(20, []int{0, 1}, [][]byte{xRaw, yRaw}, 930)

			up.push(trrMedia(19, []byte("base19"), trrMediaOpts{}))
			up.push(rXY)
			for _, seq := range []uint16{22, 23, 24, 25} {
				up.push(trrMedia(seq, []byte{byte(seq)}, trrMediaOpts{}))
			}

			got, drainErr := trrDrain(rd)
			c.Check(errors.Is(drainErr, io.EOF), "evict drain eof: %v", drainErr)
			c.Check(len(got) == 5, "evict only 5 originals got %d %v", len(got), trrSeqs(got))
			c.Check(trrCountSeq(got, 19) == 1, "base 19 present")
			for _, seq := range []uint16{20, 21} {
				c.Check(trrCountSeq(got, seq) == 0,
					"unknown column %d must not be faked after eviction, seqs=%v", seq, trrSeqs(got))
			}
			c.Check(trrSeqs(got)[len(got)-1] == 25, "frontier unaffected, last=25 got %v", trrSeqs(got))
		}

		// Sequence wrap 65535 -> 0 is forward.
		{
			aRaw := trrMedia(65534, []byte("wrap-A"), trrMediaOpts{padding: 2})
			bRaw := trrMedia(65535, []byte("wrap-B-long"), trrMediaOpts{
				extID: 3, extData: []byte{9, 8, 7, 6},
			})
			cRaw := trrMedia(0, []byte("wrap-C"), trrMediaOpts{})
			baseRaw := trrMedia(65533, []byte("wrap-base"), trrMediaOpts{})
			edgeRaw := trrMedia(2, []byte("wrap-edge"), trrMediaOpts{})
			rAB := trrRepair(65534, []int{0, 1}, [][]byte{aRaw, bRaw}, 940)
			rBC := trrRepair(65535, []int{0, 1}, [][]byte{bRaw, cRaw}, 941)
			rABC := trrRepair(65534, []int{0, 1, 2}, [][]byte{aRaw, bRaw, cRaw}, 942)

			_, up, rd, err := trrBind(64, 64)
			c.Check(err == nil, "wrap bind: %v", err)
			for _, raw := range [][]byte{baseRaw, edgeRaw, rAB, rBC, rABC} {
				up.push(raw)
			}
			got, drainErr := trrDrain(rd)
			c.Check(errors.Is(drainErr, io.EOF), "wrap drain eof: %v", drainErr)
			for _, tc := range []struct {
				seq uint16
				raw []byte
			}{{65534, aRaw}, {65535, bRaw}, {0, cRaw}} {
				g, found := trrFind(got, tc.seq)
				c.Check(found, "wrap recovered seq %d seqs=%v", tc.seq, trrSeqs(got))
				if found {
					c.Check(trrBytesEqual(g.raw, tc.raw), "wrap exact bytes %d", tc.seq)
					c.Check(trrRecovered(g), "wrap recovered attr %d", tc.seq)
				}
			}
		}

		// Equation quota evicts earliest independent equation and rebuilds.
		{
			aRaw := trrMedia(601, []byte("Q-A"), trrMediaOpts{})
			bRaw := trrMedia(602, []byte("Q-B"), trrMediaOpts{})
			cRaw := trrMedia(603, []byte("Q-C"), trrMediaOpts{})
			baseRaw := trrMedia(600, []byte("Q-base"), trrMediaOpts{})
			edgeRaw := trrMedia(604, []byte("Q-edge"), trrMediaOpts{})
			rAB := trrRepair(601, []int{0, 1}, [][]byte{aRaw, bRaw}, 950)
			rBC := trrRepair(602, []int{0, 1}, [][]byte{bRaw, cRaw}, 951)
			rABC := trrRepair(601, []int{0, 1, 2}, [][]byte{aRaw, bRaw, cRaw}, 952)

			ic, err := NewRepairReceiver(64, 2)
			c.Check(err == nil, "quota ctor: %v", err)
			up := &trrScript{}
			rd := ic.BindRemoteStream(trrInfo(), up)
			for _, raw := range [][]byte{baseRaw, edgeRaw, rAB, rBC, rABC} {
				up.push(raw)
			}
			got, drainErr := trrDrain(rd)
			c.Check(errors.Is(drainErr, io.EOF), "quota drain eof: %v", drainErr)
			// Earliest AB evicted; BC + ABC rebuild to the unique A only.
			g, found := trrFind(got, 601)
			c.Check(found, "quota rebuild recovers 601, seqs=%v", trrSeqs(got))
			if found {
				c.Check(trrBytesEqual(g.raw, aRaw), "quota 601 exact bytes")
			}
			c.Check(trrCountSeq(got, 602) == 0 && trrCountSeq(got, 603) == 0,
				"quota must not over-solve after earliest eviction, seqs=%v", trrSeqs(got))
		}

		// Repair packets before the first media are bounded-stashed then linked.
		{
			aRaw := trrMedia(701, []byte("pre-A"), trrMediaOpts{})
			single := trrRepair(701, []int{0}, [][]byte{aRaw}, 960)
			_, up, rd, err := trrBind(64, 64)
			c.Check(err == nil, "prebase bind: %v", err)
			up.push(single) // arrives before any media: stashed, pending
			buf := make([]byte, 4096)
			n, _, pErr := rd.Read(buf, interceptor.Attributes{})
			c.Check(n == 0 && errors.Is(pErr, ErrRepairPending), "pre-base repair pending: %v", pErr)

			// Two known packets 700 and 702 advance the edge; 701 missing but
			// the stashed singleton equation resolves once base exists.
			up.push(trrMedia(700, []byte("pre-base700"), trrMediaOpts{}))
			up.push(trrMedia(702, []byte("pre-edge702"), trrMediaOpts{}))
			got, drainErr := trrDrain(rd)
			c.Check(errors.Is(drainErr, io.EOF), "prebase drain eof: %v", drainErr)
			g, found := trrFind(got, 701)
			c.Check(found, "pre-base stashed repair linked and recovered 701, seqs=%v", trrSeqs(got))
			if found {
				c.Check(trrBytesEqual(g.raw, aRaw), "pre-base 701 exact bytes")
			}
		}
	})
}

// ---------------------------------------------------------------------------
// TestRepairIngress
// ---------------------------------------------------------------------------

func TestRepairIngress(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Capacity validation 2..512.
		for _, tc := range []struct {
			w, m uint16
			ok   bool
		}{
			{0, 64, false}, {1, 64, false}, {64, 1, false}, {64, 513, false},
			{513, 64, false}, {1024, 1, false},
			{2, 2, true}, {512, 512, true}, {64, 64, true}, {8, 16, true},
		} {
			ic, err := NewRepairReceiver(tc.w, tc.m)
			if tc.ok {
				c.Check(err == nil && ic != nil, "ctor (%d,%d) expected ok got %v", tc.w, tc.m, err)
			} else {
				c.Check(err != nil, "ctor (%d,%d) expected error", tc.w, tc.m)
			}
		}

		// Incomplete FEC identity passes the upstream reader through.
		{
			ic, err := NewRepairReceiver(64, 64)
			c.Check(err == nil, "passthrough ctor: %v", err)
			up := &trrScript{}
			partial := &interceptor.StreamInfo{SSRC: 42, PayloadType: 96}
			got := ic.BindRemoteStream(partial, up)
			c.Check(got == up, "missing FEC identity must return upstream directly")
			gotNil := ic.BindRemoteStream(nil, up)
			c.Check(gotNil == up, "nil StreamInfo must passthrough")
		}

		// Normal media same-call; repair returns the exact pending contract.
		{
			_, up, rd, err := trrBind(64, 64)
			c.Check(err == nil, "ingress bind: %v", err)
			buf := make([]byte, 4096)
			attrs := interceptor.Attributes{}
			attrs.Set("route", "media-10")
			up.push(trrMedia(10, []byte("hello"), trrMediaOpts{}))
			n, outAttrs, rErr := rd.Read(buf, attrs)
			c.Check(rErr == nil && n > 0, "media same call: %v", rErr)
			c.Check(trrSeq(buf[:n]) == 10, "media seq 10")
			c.Check(outAttrs != nil && outAttrs.Get("route") == "media-10", "media own attrs")
			c.Check(outAttrs.Get(AttributeRecovered) != true, "original not marked recovered")

			// Repair with two unknown columns -> no output, exact triple.
			x := trrMedia(90, []byte("x"), trrMediaOpts{})
			y := trrMedia(91, []byte("y"), trrMediaOpts{})
			up.push(trrRepair(90, []int{0, 1}, [][]byte{x, y}, 990))
			n2, a2, e2 := rd.Read(buf, interceptor.Attributes{})
			c.Check(n2 == 0 && a2 == nil && errors.Is(e2, ErrRepairPending),
				"repair contract 0,nil,ErrRepairPending got n=%d a=%v err=%v", n2, a2, e2)
		}

		// Upstream error verbatim, ready data delivered first and not lost.
		{
			aRaw := trrMedia(811, []byte("ready-A"), trrMediaOpts{})
			single := trrRepair(811, []int{0}, [][]byte{aRaw}, 991)
			_, up, rd, err := trrBind(64, 64)
			c.Check(err == nil, "upstream-err bind: %v", err)
			up.terminal = errTrrBoom
			up.push(trrMedia(810, []byte("base810"), trrMediaOpts{}))
			up.push(trrMedia(812, []byte("edge812"), trrMediaOpts{}))
			up.push(single)

			got, drainErr := trrDrain(rd)
			// 810, 812, recovered 811 are produced before boom on empty upstream.
			var found811 bool
			for _, g := range got {
				if trrSeq(g.raw) == 811 {
					found811 = true
				}
			}
			c.Check(found811, "ready recovery delivered before upstream error")
			c.Check(errors.Is(drainErr, errTrrBoom), "upstream error verbatim got %v", drainErr)

			// A later read still reports the same upstream error.
			buf := make([]byte, 4096)
			_, _, e2 := rd.Read(buf, interceptor.Attributes{})
			c.Check(errors.Is(e2, errTrrBoom), "upstream error still verbatim: %v", e2)
		}

		// SSRC conflict: error-only reader, no upstream reads, binding unchanged.
		{
			ic, err := NewRepairReceiver(64, 64)
			c.Check(err == nil, "conflict ctor: %v", err)
			up1 := &trrScript{}
			info1 := trrInfo()
			rd1 := ic.BindRemoteStream(info1, up1)
			c.Check(rd1 != nil, "bind1")

			up2 := &trrScript{}
			conflict := &interceptor.StreamInfo{
				SSRC: 555001, PayloadType: trrMediaPT,
				SSRCForwardErrorCorrection:        trrFECSSRC, // collides with info1 FEC SSRC
				PayloadTypeForwardErrorCorrection: trrFECPT,
			}
			rd2 := ic.BindRemoteStream(conflict, up2)
			buf := make([]byte, 4096)
			n, _, cErr := rd2.Read(buf, interceptor.Attributes{})
			c.Check(n == 0 && cErr != nil, "conflict reader errors")
			c.Check(errors.Is(cErr, ErrSSRCConflict), "conflict wraps ErrSSRCConflict: %v", cErr)
			c.Check(up2.readCount() == 0, "conflict reader must not read upstream, reads=%d", up2.readCount())

			// Cross-type collision: new media SSRC equals existing FEC SSRC.
			up3 := &trrScript{}
			cross := &interceptor.StreamInfo{
				SSRC: trrFECSSRC, PayloadType: trrMediaPT,
				SSRCForwardErrorCorrection: 666777, PayloadTypeForwardErrorCorrection: trrFECPT,
			}
			rd3 := ic.BindRemoteStream(cross, up3)
			_, _, e3 := rd3.Read(buf, interceptor.Attributes{})
			c.Check(e3 != nil && up3.readCount() == 0, "cross ssrc conflict errors/no upstream: %v", e3)

			// Existing binding still works.
			up1.push(trrMedia(1, []byte("still-alive"), trrMediaOpts{}))
			n1, _, e1 := rd1.Read(buf, interceptor.Attributes{})
			c.Check(e1 == nil && n1 > 0 && trrSeq(buf[:n1]) == 1, "existing binding unaffected: %v", e1)
		}

		// Unbind isolates the generation with io.ErrClosedPipe and no upstream.
		{
			ic, _, rd, err := trrBind(64, 64)
			c.Check(err == nil, "unbind bind: %v", err)
			up := &trrScript{}
			// Re-bind with an observable upstream (trrBind's script is internal
			// but we created a fresh pair here for clarity).
			info := trrInfo()
			rd = ic.BindRemoteStream(info, up)
			before := up.readCount()
			ic.UnbindRemoteStream(info)
			buf := make([]byte, 4096)
			_, _, e := rd.Read(buf, interceptor.Attributes{})
			c.Check(errors.Is(e, io.ErrClosedPipe), "old reader after unbind pipe: %v", e)
			c.Check(up.readCount() == before, "unbound reader does not read upstream")
			// Idempotent unbind is safe.
			ic.UnbindRemoteStream(info)
		}

		// Re-bind same SSRC starts a clean generation.
		{
			ic, up1, rd1, err := trrBind(64, 64)
			c.Check(err == nil, "rebind bind1: %v", err)
			aRaw := trrMedia(101, []byte("old-recovery"), trrMediaOpts{})
			up1.push(trrMedia(100, []byte("old-base"), trrMediaOpts{}))
			up1.push(trrMedia(102, []byte("old-edge"), trrMediaOpts{}))
			up1.push(trrRepair(101, []int{0}, [][]byte{aRaw}, 995))
			_, drainErr := trrDrain(rd1)
			c.Check(errors.Is(drainErr, io.EOF), "rebind pre-drain: %v", drainErr)
			// Recovery 101 is now ready but unread; unbind must discard it.
			info := trrInfo()
			ic.UnbindRemoteStream(info)
			buf := make([]byte, 4096)
			_, _, oldErr := rd1.Read(buf, interceptor.Attributes{})
			c.Check(errors.Is(oldErr, io.ErrClosedPipe), "old gen pipe after rebind unbind: %v", oldErr)

			up2 := &trrScript{}
			rd2 := ic.BindRemoteStream(info, up2)
			up2.push(trrMedia(500, []byte("new-generation"), trrMediaOpts{}))
			n, _, e := rd2.Read(buf, interceptor.Attributes{})
			c.Check(e == nil && trrSeq(buf[:n]) == 500, "new gen first output 500 not stale 101: %v", e)
		}

		// Close clears everything.
		{
			ic, _, rd, err := trrBind(64, 64)
			c.Check(err == nil, "close bind: %v", err)
			c.Check(ic.Close() == nil, "close nil")
			buf := make([]byte, 4096)
			_, _, e := rd.Read(buf, interceptor.Attributes{})
			c.Check(errors.Is(e, io.ErrClosedPipe), "reader after close pipe: %v", e)
			c.Check(ic.Close() == nil, "double close nil")
		}

		// Two streams, interleaved, states isolated.
		{
			ic, err := NewRepairReceiver(64, 64)
			c.Check(err == nil, "dual ctor: %v", err)
			infoA := trrInfo()
			upA := &trrScript{}
			rdA := ic.BindRemoteStream(infoA, upA)

			infoB := &interceptor.StreamInfo{
				SSRC: 77002, PayloadType: trrMediaPT,
				SSRCForwardErrorCorrection: 88003, PayloadTypeForwardErrorCorrection: trrFECPT,
			}
			upB := &trrScript{}
			rdB := ic.BindRemoteStream(infoB, upB)

			aLost := trrMedia(11, []byte("A-lost"), trrMediaOpts{ssrc: 77001})
			bLost := trrMedia(21, []byte("B-lost-long"), trrMediaOpts{ssrc: 77002, padding: 2})
			upA.push(trrMedia(10, []byte("A10"), trrMediaOpts{ssrc: 77001}))
			upB.push(trrMedia(20, []byte("B20"), trrMediaOpts{ssrc: 77002}))
			upA.push(trrMedia(12, []byte("A12"), trrMediaOpts{ssrc: 77001}))
			upB.push(trrMedia(22, []byte("B22"), trrMediaOpts{ssrc: 77002}))
			upA.push(trrRepairID(11, []int{0}, [][]byte{aLost}, 970, 77001, 88002, trrFECPT))
			upB.push(trrRepairID(21, []int{0}, [][]byte{bLost}, 971, 77002, 88003, trrFECPT))

			buf := make([]byte, 16384)
			readSeq := func(rd interceptor.RTPReader) uint16 {
				for i := 0; i < 50; i++ {
					n, _, e := rd.Read(buf, interceptor.Attributes{})
					if errors.Is(e, ErrRepairPending) {
						continue
					}
					if e != nil {
						return 0
					}

					return trrSeq(buf[:n])
				}

				return 0
			}

			// Interleave direct media reads from both streams.
			c.Check(readSeq(rdA) == 10, "A first 10")
			c.Check(readSeq(rdB) == 20, "B first 20")
			c.Check(readSeq(rdA) == 12, "A second 12")
			c.Check(readSeq(rdB) == 22, "B second 22")

			gA, eA := trrDrain(rdA)
			gB, eB := trrDrain(rdB)
			c.Check(errors.Is(eA, io.EOF), "drain A eof: %v", eA)
			c.Check(errors.Is(eB, io.EOF), "drain B eof: %v", eB)
			rA, okA := trrFind(gA, 11)
			rB, okB := trrFind(gB, 21)
			c.Check(okA && trrBytesEqual(rA.raw, aLost), "stream A recovers own 11 exact, seqs=%v", trrSeqs(gA))
			c.Check(okB && trrBytesEqual(rB.raw, bLost), "stream B recovers own 21 exact, seqs=%v", trrSeqs(gB))
			if okA {
				var p rtp.Packet
				_ = p.Unmarshal(rA.raw)
				c.Check(p.SSRC == 77001, "A recovery ssrc isolated")
			}
			if okB {
				var p rtp.Packet
				_ = p.Unmarshal(rB.raw)
				c.Check(p.SSRC == 77002, "B recovery ssrc isolated")
			}
		}

		// Input buffer + Attributes reuse; cached rtp.Header must not leak.
		{
			aRaw := trrMedia(1001, []byte("reuse-A"), trrMediaOpts{})
			bRaw := trrMedia(1002, []byte("reuse-B-longer"), trrMediaOpts{padding: 3})
			cRaw := trrMedia(1003, []byte("reuse-C"), trrMediaOpts{
				extID: 1, extData: []byte{1, 2, 3, 4},
			})
			baseRaw := trrMedia(1000, []byte("reuse-base"), trrMediaOpts{})
			edgeRaw := trrMedia(1004, []byte("reuse-edge"), trrMediaOpts{})
			rAB := trrRepair(1001, []int{0, 1}, [][]byte{aRaw, bRaw}, 980)
			rBC := trrRepair(1002, []int{0, 1}, [][]byte{bRaw, cRaw}, 981)
			rABC := trrRepair(1001, []int{0, 1, 2}, [][]byte{aRaw, bRaw, cRaw}, 982)

			_, up, rd, err := trrBind(64, 64)
			c.Check(err == nil, "reuse bind: %v", err)
			items := [][]byte{baseRaw, edgeRaw, rAB, rBC, rABC}
			for _, it := range items {
				up.push(it)
			}
			up.hook = func(a interceptor.Attributes, raw []byte, index int) interceptor.Attributes {
				if a == nil {
					a = interceptor.Attributes{}
				}
				a.Set("trr-marker", fmt.Sprintf("m%d", index))
				// Populate the framework's cached *rtp.Header deliberately.
				_, _ = a.GetRTPHeader(raw)

				return a
			}

			oneBuf := make([]byte, 16384)
			sharedAttrs := interceptor.Attributes{}
			var recovered []trrGot
			for i := 0; i < 200 && len(recovered) < 3; i++ {
				n, attrs, rErr := rd.Read(oneBuf, sharedAttrs)
				if errors.Is(rErr, ErrRepairPending) {
					continue
				}
				if rErr != nil {
					c.Check(false, "reuse unexpected err: %v", rErr)

					break
				}
				seq := trrSeq(oneBuf[:n])
				if seq >= 1001 && seq <= 1003 {
					recovered = append(recovered, trrGot{
						raw:   append([]byte(nil), oneBuf[:n]...),
						attrs: attrs,
					})
				}
			}
			c.Check(len(recovered) == 3, "reuse recovered 3 got %d", len(recovered))
			for _, tc := range []struct {
				seq uint16
				raw []byte
			}{{1001, aRaw}, {1002, bRaw}, {1003, cRaw}} {
				var g trrGot
				ok := false
				for _, r := range recovered {
					if trrSeq(r.raw) == tc.seq {
						g, ok = r, true
					}
				}
				c.Check(ok, "reuse found %d", tc.seq)
				if ok {
					c.Check(trrBytesEqual(g.raw, tc.raw), "reuse exact bytes despite buffer reuse %d", tc.seq)
					c.Check(trrRecovered(g), "reuse recovered flag %d", tc.seq)
					c.Check(g.attrs.Get("trr-marker") == "m4",
						"seq %d attrs snapshot from solving input m4 got %v", tc.seq, g.attrs.Get("trr-marker"))
					h, hErr := g.attrs.GetRTPHeader(g.raw)
					c.Check(hErr == nil && h != nil && h.SequenceNumber == tc.seq,
						"seq %d cached header not misused, hdrSeq err=%v", tc.seq, hErr)
				}
			}
		}

		// Identity / PT / structure rejection without partial commit.
		{
			_, up, rd, err := trrBind(64, 64)
			c.Check(err == nil, "reject bind: %v", err)
			buf := make([]byte, 4096)

			// Wrong media payload type.
			badPT := append([]byte(nil), trrMedia(2, []byte("badpt"), trrMediaOpts{})...)
			badPT[1] = (badPT[1] & 0x80) | 70
			up.push(badPT)
			n, _, e := rd.Read(buf, interceptor.Attributes{})
			c.Check(n == 0 && e != nil && !errors.Is(e, ErrRepairPending), "PT mismatch error: %v", e)

			// Unknown SSRC.
			badSSRC := trrMedia(3, []byte("ssrc"), trrMediaOpts{ssrc: 12345})
			up.push(badSSRC)
			_, _, e2 := rd.Read(buf, interceptor.Attributes{})
			c.Check(e2 != nil && !errors.Is(e2, ErrRepairPending), "unknown ssrc error: %v", e2)

			// Malformed RTP.
			up.push([]byte{0x80, 0x62, 0x00})
			_, _, e3 := rd.Read(buf, interceptor.Attributes{})
			c.Check(e3 != nil, "malformed rtp error: %v", e3)

			// Unsupported FlexFEC R bit (retransmission) is explicitly rejected.
			valid := trrMedia(4, []byte("p"), trrMediaOpts{})
			badFEC := trrRepair(4, []int{0}, [][]byte{valid}, 999)
			badFEC[12] |= 0x80 // R bit in FlexFEC header
			up.push(badFEC)
			_, _, e4 := rd.Read(buf, interceptor.Attributes{})
			c.Check(e4 != nil && !errors.Is(e4, ErrRepairPending), "R-bit unsupported rejected: %v", e4)

			// Binding still commits valid media after rejections.
			up.push(trrMedia(5, []byte("valid-after"), trrMediaOpts{}))
			n5, _, e5 := rd.Read(buf, interceptor.Attributes{})
			c.Check(e5 == nil && n5 > 0 && trrSeq(buf[:n5]) == 5,
				"valid media after rejections still delivered: %v", e5)
		}
	})
}

func trrBytesEqual(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}

	return true
}
