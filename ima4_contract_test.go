package aiff

import (
	"bytes"
	"encoding/binary"
	"io"
	"testing"

	"github.com/go-audio/audio"
)

// ---------------------------------------------------------------------------
// Test file construction helpers (independent of the decoder under test).
// ---------------------------------------------------------------------------

type ima4TestPacket struct {
	// predictor is the full predictor used for this packet's header. Only
	// the top 9 bits are stored.
	predictor int
	index     int
	nibbles   [64]int
}

// bytes serializes a packet. nibbles are emitted low nibble first.
func (p ima4TestPacket) bytes() []byte {
	var h uint16
	pred9 := p.predictor >> 7
	if pred9 < 0 {
		pred9 += 512
	}
	h = uint16(pred9&0x1ff)<<7 | uint16(p.index&0x7f)
	out := make([]byte, ima4PacketBytes)
	out[0] = byte(h >> 8)
	out[1] = byte(h)
	for i := 0; i < 32; i++ {
		lo := p.nibbles[2*i] & 0xf
		hi := p.nibbles[2*i+1] & 0xf
		out[2+i] = byte(lo | hi<<4)
	}
	return out
}

func allNibbles(v int) [64]int {
	var n [64]int
	for i := range n {
		n[i] = v
	}
	return n
}

func absInt(v int) int {
	if v < 0 {
		return -v
	}
	return v
}

// ima4HeaderPredictorFromBytes and ima4HeaderIndexFromBytes decode a test
// packet header without going through the production decoder.
func ima4HeaderPredictorFromBytes(b []byte) int {
	h := int(b[0])<<8 | int(b[1])
	p := h >> 7
	if p >= 256 {
		p -= 512
	}
	return p << 7
}

func ima4HeaderIndexFromBytes(b []byte) int {
	return (int(b[0])<<8 | int(b[1])) & 0x7f
}

// ima4TestFile describes a legal FORM/FVER/COMM/SSND file.
type ima4TestFile struct {
	chans      uint16
	groups     uint32
	bitDepth   uint16
	sampleRate int
	offset     uint32
	// headerOffset, when non-nil, is written to the SSND header instead of
	// offset, allowing a declared offset that the chunk can't satisfy.
	headerOffset *uint32
	blockSize    uint32
	data         []byte
	// trailing chunks appended after SSND (already serialized with headers
	// and padding).
	trailing []byte
}

func (f ima4TestFile) build() []byte {
	var comm bytes.Buffer
	binary.Write(&comm, binary.BigEndian, f.chans)
	binary.Write(&comm, binary.BigEndian, f.groups)
	binary.Write(&comm, binary.BigEndian, f.bitDepth)
	rate := audio.IntToIEEEFloat(f.sampleRate)
	comm.Write(rate[:])
	comm.Write([]byte("ima4"))
	// Pascal style encoding name; one character keeps the COMM chunk even.
	comm.Write([]byte{1, 'x'})

	var fver bytes.Buffer
	binary.Write(&fver, binary.BigEndian, uint32(0xA2805140))

	var ssnd bytes.Buffer
	declaredOffset := f.offset
	if f.headerOffset != nil {
		declaredOffset = *f.headerOffset
	}
	binary.Write(&ssnd, binary.BigEndian, declaredOffset)
	binary.Write(&ssnd, binary.BigEndian, f.blockSize)
	if f.offset > 0 {
		ssnd.Write(make([]byte, f.offset))
	}
	ssnd.Write(f.data)

	var body bytes.Buffer
	body.WriteString("AIFC")
	body.WriteString("FVER")
	binary.Write(&body, binary.BigEndian, uint32(fver.Len()))
	body.Write(fver.Bytes())
	body.WriteString("COMM")
	binary.Write(&body, binary.BigEndian, uint32(comm.Len()))
	body.Write(comm.Bytes())
	body.WriteString("SSND")
	binary.Write(&body, binary.BigEndian, uint32(ssnd.Len()))
	body.Write(ssnd.Bytes())
	if ssnd.Len()%2 != 0 {
		body.WriteByte(0)
	}
	body.Write(f.trailing)

	var file bytes.Buffer
	file.WriteString("FORM")
	binary.Write(&file, binary.BigEndian, uint32(body.Len()))
	file.Write(body.Bytes())
	return file.Bytes()
}

// comtChunk builds a COMT chunk with a single comment.
func comtChunk(text string) []byte {
	var c bytes.Buffer
	binary.Write(&c, binary.BigEndian, uint16(1))
	binary.Write(&c, binary.BigEndian, uint32(0)) // marker id
	binary.Write(&c, binary.BigEndian, uint32(0)) // timestamp
	c.WriteByte(byte(len(text)))
	c.WriteString(text)

	var out bytes.Buffer
	out.WriteString("COMT")
	binary.Write(&out, binary.BigEndian, uint32(c.Len()))
	out.Write(c.Bytes())
	if c.Len()%2 != 0 {
		out.WriteByte(0)
	}
	return out.Bytes()
}

// ---------------------------------------------------------------------------
// Independent reference decoder. This is a direct transcription of the IMA
// step/index integer recursion; it shares no code with the production decoder.
// ---------------------------------------------------------------------------

var refStep = [89]int{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
	34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
	157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
	3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
}

var refIndex = [16]int{-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8}

type refChannel struct {
	predictor int
	index     int
	set       bool
}

func refHeader(b []byte) (predictor, index int) {
	h := int(b[0])<<8 | int(b[1])
	p := h >> 7
	if p >= 256 {
		p -= 512
	}
	return p << 7, h & 0x7f
}

func refNibbleStep(predictor, index, n int) (int, int) {
	step := refStep[index]
	diff := step>>3 + step*int((n>>2)&1) + (step>>1)*int((n>>1)&1) + (step>>2)*int(n&1)
	if n&8 != 0 {
		predictor -= diff
	} else {
		predictor += diff
	}
	if predictor > 32767 {
		predictor = 32767
	} else if predictor < -32768 {
		predictor = -32768
	}
	index += refIndex[n&7]
	if index < 0 {
		index = 0
	} else if index > 88 {
		index = 88
	}
	return predictor, index
}

// refDecode decodes a whole group stream into interleaved PCM scalars.
func refDecode(data []byte, chans int, groups int) []int {
	out := make([]int, 0, groups*64*chans)
	var state [2]refChannel
	pos := 0
	for g := 0; g < groups; g++ {
		chansamples := make([][]int, chans)
		next := state
		for c := 0; c < chans; c++ {
			if pos+ima4PacketBytes > len(data) {
				return out
			}
			pkt := data[pos : pos+ima4PacketBytes]
			pos += ima4PacketBytes
			hp, hi := refHeader(pkt)
			p, idx := hp, hi
			if state[c].set {
				diff := hp - state[c].predictor
				if diff < 0 {
					diff = -diff
				}
				if state[c].index == hi && diff <= 127 {
					p, idx = state[c].predictor, state[c].index
				}
			}
			samples := make([]int, 64)
			for s := 0; s < 64; s++ {
				b := pkt[2+s/2]
				n := int(b & 0xf)
				if s%2 == 1 {
					n = int(b >> 4)
				}
				p, idx = refNibbleStep(p, idx, n)
				samples[s] = p
			}
			chansamples[c] = samples
			next[c] = refChannel{predictor: p, index: idx, set: true}
		}
		for s := 0; s < 64; s++ {
			for c := 0; c < chans; c++ {
				out = append(out, chansamples[c][s])
			}
		}
		state = next
	}
	return out
}

// sameInts reports whether two int slices hold the same values; nil and
// empty slices compare equal.
func sameInts(a, b []int) bool {
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

// ---------------------------------------------------------------------------
// Decoder helpers used by the tests.
// ---------------------------------------------------------------------------

func newTestDecoder(t *testing.T, file []byte) *Decoder {
	t.Helper()
	return NewDecoder(bytes.NewReader(file))
}

// readAllScalars drains PCMBuffer in chunks of the given size, exercising
// partial groups and small reads.
func readAllScalars(t *testing.T, d *Decoder, chunk int) []int {
	t.Helper()
	var got []int
	buf := &audio.IntBuffer{Data: make([]int, chunk)}
	for {
		n, err := d.PCMBuffer(buf)
		if n > 0 {
			got = append(got, buf.Data[:n]...)
		}
		if err != nil {
			t.Fatalf("PCMBuffer: %v", err)
		}
		if n == 0 {
			return got
		}
	}
}

// ---------------------------------------------------------------------------
// TestIMA4Codec
// ---------------------------------------------------------------------------

func TestIMA4Codec(t *testing.T) {
	t.Run("mono", func(t *testing.T) {
		// Two packet groups, both headers zero, all data bytes 0x11.
		// First nibble is 1 with index 0: step 7, diff 7>>3=0, +7>>2=1.
		// Expected from an independent recursion: 1..128 (the header's
		// predictor is not an emitted sample).
		pkt1 := ima4TestPacket{predictor: 0, index: 0, nibbles: allNibbles(1)}
		pkt2 := ima4TestPacket{predictor: 0, index: 0, nibbles: allNibbles(1)}
		data := append(pkt1.bytes(), pkt2.bytes()...)
		file := ima4TestFile{chans: 1, groups: 2, bitDepth: 16, sampleRate: 44100, data: data}.build()

		want := refDecode(data, 1, 2)
		if len(want) != 128 {
			t.Fatalf("reference emitted %d samples, want 128", len(want))
		}
		if want[0] != 1 || want[64] != 65 {
			t.Fatalf("reference sample[0]=%d sample[64]=%d, want 1 and 65", want[0], want[64])
		}
		// The documented example is the exact sequence 1..128.
		for i, v := range want {
			if v != i+1 {
				t.Fatalf("reference sample[%d]=%d, want %d", i, v, i+1)
			}
		}

		d := newTestDecoder(t, file)
		d.ReadInfo()
		if d.NumSampleFrames != 2 {
			t.Fatalf("raw NumSampleFrames=%d, want 2 packet groups", d.NumSampleFrames)
		}
		if got := d.PCMLen(); got != 256 {
			t.Fatalf("PCMLen=%d, want 256 uncompressed bytes", got)
		}
		dur, err := d.Duration()
		if err != nil {
			t.Fatalf("Duration: %v", err)
		}
		wantDur := float64(128) / 44100.0
		if diff := dur.Seconds() - wantDur; diff > 1e-6 || diff < -1e-6 {
			t.Fatalf("Duration=%f, want %f", dur.Seconds(), wantDur)
		}

		got, err := d.FullPCMBuffer()
		if err != nil {
			t.Fatalf("FullPCMBuffer: %v", err)
		}
		if len(got.Data) != 128 || got.NumFrames() != 128 {
			t.Fatalf("got %d samples / %d frames, want 128", len(got.Data), got.NumFrames())
		}
		if !sameInts(got.Data, want) {
			t.Fatalf("decoded data mismatch\n got %v\nwant %v", got.Data, want)
		}

		// Sequential scalar reads in small partial increments must match too.
		d.Rewind()
		if got2 := readAllScalars(t, d, 7); !sameInts(got2, want) {
			t.Fatalf("chunked PCMBuffer mismatch")
		}

		// Cross-packet state rules, verified against the reference:
		//  - a near header (index equal, predictor within 127) continues;
		//  - a far header restarts from the header;
		//  - an index change restarts even with a near predictor.
		// The reference already encodes these rules, so decode a stream that
		// exercises all three and compare sample by sample.
		base := ima4TestPacket{predictor: 0, index: 20, nibbles: allNibbles(8)}
		baseData := base.bytes()
		var end, endIndex int
		{
			p, idx := ima4HeaderPredictorFromBytes(baseData), ima4HeaderIndexFromBytes(baseData)
			for s := 0; s < 64; s++ {
				p, idx = refNibbleStep(p, idx, 8)
			}
			end = p
			endIndex = idx
		}
		nearPkt := ima4TestPacket{predictor: end, index: endIndex, nibbles: allNibbles(2)}
		if d := absInt(ima4HeaderPredictorFromBytes(nearPkt.bytes()) - end); d > 127 {
			t.Fatalf("test setup: near header differs by %d", d)
		}
		// Far header: high bits far from the true endpoint, so the state must
		// restart from the header instead of continuing.
		farPkt := ima4TestPacket{predictor: end + 8192, index: endIndex, nibbles: allNibbles(2)}
		// Same predictor magnitude but a changed index also forces a restart.
		indexChangePkt := ima4TestPacket{predictor: end, index: (endIndex + 3) % 89, nibbles: allNibbles(2)}
		ruleData := append([]byte{}, baseData...)
		ruleData = append(ruleData, nearPkt.bytes()...)
		ruleData = append(ruleData, farPkt.bytes()...)
		ruleData = append(ruleData, indexChangePkt.bytes()...)
		ruleFile := ima4TestFile{chans: 1, groups: 4, bitDepth: 16, sampleRate: 44100, data: ruleData}.build()
		dRule := newTestDecoder(t, ruleFile)
		gotRule, err := dRule.FullPCMBuffer()
		if err != nil {
			t.Fatalf("rule FullPCMBuffer: %v", err)
		}
		wantRule := refDecode(ruleData, 1, 4)
		if !sameInts(gotRule.Data, wantRule) {
			t.Fatalf("cross-packet state rule mismatch")
		}
		// Make sure the test is meaningful: continuing from the exact end
		// state must differ from restarting at the near packet's header.
		hp := ima4HeaderPredictorFromBytes(nearPkt.bytes())
		if hp == end {
			t.Fatalf("test setup: near header carried the full predictor")
		}
		restartSample, _ := refNibbleStep(hp, endIndex, 2)
		if restartSample == wantRule[64] {
			t.Fatalf("test setup: near-header restart is indistinguishable")
		}

		// Saturation: a huge positive predictor pushed forward by a maximum
		// positive nibble must clamp at 32767; negative at -32768. Index must
		// clamp to 0..88.
		satPkt := ima4TestPacket{predictor: 32767, index: 88, nibbles: allNibbles(7)}
		negPkt := ima4TestPacket{predictor: -32768, index: 0, nibbles: allNibbles(15)}
		// Also push an extreme nibble from a mid-range predictor with index
		// clamped high/low, checking both index ends.
		hiPkt := ima4TestPacket{predictor: 32767, index: 88, nibbles: allNibbles(6)}
		loPkt := ima4TestPacket{predictor: -32768, index: 0, nibbles: allNibbles(9)}
		satData := append(append(append(satPkt.bytes(), negPkt.bytes()...), hiPkt.bytes()...), loPkt.bytes()...)
		satFile := ima4TestFile{chans: 1, groups: 4, bitDepth: 16, sampleRate: 44100, data: satData}.build()
		dSat := newTestDecoder(t, satFile)
		gotSat, err := dSat.FullPCMBuffer()
		if err != nil {
			t.Fatalf("saturation FullPCMBuffer: %v", err)
		}
		if wantSat := refDecode(satData, 1, 4); !sameInts(gotSat.Data, wantSat) {
			t.Fatalf("saturation/index clamp mismatch")
		}
		for _, v := range gotSat.Data {
			if v < -32768 || v > 32767 {
				t.Fatalf("sample %d outside int16 range", v)
			}
		}
		if gotSat.Data[0] != 32767 {
			t.Fatalf("positive saturation sample=%d, want 32767", gotSat.Data[0])
		}
		if gotSat.Data[64] != -32768 {
			t.Fatalf("negative saturation sample=%d, want -32768", gotSat.Data[64])
		}
		taskCheck(t, true, "mono ima4 decodes two packet groups with continued state")
	})

	t.Run("stereo", func(t *testing.T) {
		// Left packet: index 0, nibbles 1 (positive) so samples count up.
		// Right packet: index 30, nibbles 8 (negative) so samples count
		// down and the two channels have deliberately different state.
		left := ima4TestPacket{predictor: 0, index: 0, nibbles: allNibbles(1)}
		right := ima4TestPacket{predictor: 0, index: 30, nibbles: allNibbles(8)}
		var data []byte
		for g := 0; g < 2; g++ {
			data = append(data, left.bytes()...)
			data = append(data, right.bytes()...)
		}
		file := ima4TestFile{chans: 2, groups: 2, bitDepth: 16, sampleRate: 48000, data: data}.build()

		want := refDecode(data, 2, 2)
		if len(want) != 256 {
			t.Fatalf("reference emitted %d scalars, want 256", len(want))
		}
		// Interleaving sanity: left and right must differ.
		if want[0] == want[1] {
			t.Fatalf("stereo channels did not decode independently")
		}

		d := newTestDecoder(t, file)
		if got := d.PCMLen(); got != 512 {
			t.Fatalf("PCMLen=%d, want 512", got)
		}
		got, err := d.FullPCMBuffer()
		if err != nil {
			t.Fatalf("FullPCMBuffer: %v", err)
		}
		if !sameInts(got.Data, want) {
			t.Fatalf("stereo mismatch at first divergence")
		}
		if got.NumFrames() != 128 {
			t.Fatalf("got %d frames, want 128", got.NumFrames())
		}

		// Interleave explicitly: frame s is [left[s], right[s]].
		for s := 0; s < 64; s++ {
			if got.Data[2*s] != want[2*s] || got.Data[2*s+1] != want[2*s+1] {
				t.Fatalf("frame %d interleave mismatch: %v", s, got.Data[2*s:2*s+2])
			}
		}

		// Reading one scalar then Rewind must re-establish both channels.
		d.Rewind()
		buf := &audio.IntBuffer{Data: make([]int, 1)}
		n, err := d.PCMBuffer(buf)
		if err != nil || n != 1 {
			t.Fatalf("first scalar read: n=%d err=%v", n, err)
		}
		if buf.Data[0] != want[0] {
			t.Fatalf("first scalar=%d, want %d", buf.Data[0], want[0])
		}
		if err := d.Rewind(); err != nil {
			t.Fatalf("Rewind: %v", err)
		}
		got2 := readAllScalars(t, d, 3)
		if !sameInts(got2, want) {
			t.Fatalf("rewound chunked read mismatch")
		}
		taskCheck(t, true, "stereo channels keep independent state and interleave by frame")
	})
}

// ---------------------------------------------------------------------------
// TestIMA4Stream
// ---------------------------------------------------------------------------

func TestIMA4Stream(t *testing.T) {
	t.Run("extent", func(t *testing.T) {
		pkt := ima4TestPacket{predictor: 100 << 7, index: 5, nibbles: allNibbles(3)}
		// Three groups plus trailing bytes after the declared sound data:
		// the last group has a complete extent but we deliberately declare
		// fewer groups than the bytes present, then add offset padding and
		// a COMT chunk after SSND.
		first := pkt.bytes()
		second := ima4TestPacket{predictor: 100 << 7, index: 5, nibbles: allNibbles(9)}
		data := append(append([]byte{}, first...), second.bytes()...)
		data = append(data, 0xde, 0xad, 0xbe, 0xef) // undeclared tail inside SSND

		tail := comtChunk("after sound")
		file := ima4TestFile{
			chans: 1, groups: 2, bitDepth: 16, sampleRate: 44100,
			offset: 5, blockSize: 34, data: data, trailing: tail,
		}.build()

		// Reference uses only the declared 2 groups (128 PCM frames).
		declared := data[:2*ima4PacketBytes]
		want := refDecode(declared, 1, 2)

		d := newTestDecoder(t, file)
		d.ReadInfo()
		if got := d.PCMLen(); got != 256 {
			t.Fatalf("PCMLen=%d, want 256", got)
		}
		if d.NumSampleFrames != 2 {
			t.Fatalf("raw NumSampleFrames=%d, want 2", d.NumSampleFrames)
		}
		got, err := d.FullPCMBuffer()
		if err != nil {
			t.Fatalf("FullPCMBuffer: %v", err)
		}
		if len(got.Data) != 128 {
			t.Fatalf("got %d frames, want exactly 128 (no inferred tail)", len(got.Data))
		}
		if !sameInts(got.Data, want) {
			t.Fatalf("extent PCM mismatch")
		}
		// The undeclared bytes inside SSND and the offset padding must not
		// leak into the audio.

		// Drain must skip the rest of SSND and still parse the trailing COMT.
		if err := d.Drain(); err != nil {
			t.Fatalf("Drain: %v", err)
		}
		if len(d.Comments) != 1 || d.Comments[0] != "after sound" {
			t.Fatalf("trailing comment not parsed: %#v", d.Comments)
		}

		// Drain after a partial read must do the same: skip the remaining
		// compressed groups from the SSND end, not from inside the packets.
		dPartial := newTestDecoder(t, file)
		if _, err := dPartial.PCMBuffer(&audio.IntBuffer{Data: make([]int, 10)}); err != nil {
			t.Fatalf("partial read: %v", err)
		}
		if err := dPartial.Drain(); err != nil {
			t.Fatalf("Drain after partial read: %v", err)
		}
		if len(dPartial.Comments) != 1 || dPartial.Comments[0] != "after sound" {
			t.Fatalf("Drain after partial read lost the trailing comment: %#v", dPartial.Comments)
		}
		if dPartial.EOF() != true {
			t.Fatalf("decoder not at EOF after Drain")
		}
		if n, err := dPartial.PCMBuffer(&audio.IntBuffer{Data: make([]int, 4)}); err != nil || n != 0 {
			t.Fatalf("PCM after Drain: n=%d err=%v", n, err)
		}

		// Drain without ever touching PCM data must behave the same way.
		dMeta := newTestDecoder(t, file)
		if err := dMeta.Drain(); err != nil {
			t.Fatalf("metadata-only Drain: %v", err)
		}
		if len(dMeta.Comments) != 1 || dMeta.Comments[0] != "after sound" {
			t.Fatalf("metadata-only Drain lost the trailing comment: %#v", dMeta.Comments)
		}

		// Same file without trailing metadata: zero groups is empty audio;
		// declared groups beyond the available bytes is an explicit error.
		emptyFile := ima4TestFile{chans: 1, groups: 0, bitDepth: 16, sampleRate: 44100}.build()
		dEmpty := newTestDecoder(t, emptyFile)
		gotEmpty, err := dEmpty.FullPCMBuffer()
		if err != nil {
			t.Fatalf("empty FullPCMBuffer: %v", err)
		}
		if len(gotEmpty.Data) != 0 {
			t.Fatalf("zero groups yielded %d scalars, want 0", len(gotEmpty.Data))
		}
		if dEmpty.PCMLen() != 0 {
			t.Fatalf("zero groups PCMLen=%d, want 0", dEmpty.PCMLen())
		}

		shortFile := ima4TestFile{chans: 1, groups: 3, bitDepth: 16, sampleRate: 44100,
			data: first}.build()
		dShort := newTestDecoder(t, shortFile)
		if _, err := dShort.FullPCMBuffer(); err == nil {
			t.Fatalf("declared 3 groups with 1 group of bytes did not error")
		}
		taskCheck(t, true, "extent: declared groups, offset, tail bytes and trailing COMT handled")
	})

	t.Run("truncated", func(t *testing.T) {
		pkt := ima4TestPacket{predictor: 0, index: 0, nibbles: allNibbles(1)}
		stereoLeft := ima4TestPacket{predictor: 0, index: 0, nibbles: allNibbles(5)}
		stereoRight := ima4TestPacket{predictor: 0, index: 10, nibbles: allNibbles(6)}

		// Mono: one complete group then a group truncated mid-packet. The
		// declared count says 2, so completion must fail rather than borrow
		// the following COMT bytes.
		trunc := append(pkt.bytes(), pkt.bytes()[:20]...)
		tail := comtChunk("not audio")
		d := newTestDecoder(t, ima4TestFile{chans: 1, groups: 2, bitDepth: 16,
			sampleRate: 44100, data: trunc, trailing: tail}.build())
		want := refDecode(pkt.bytes(), 1, 1) // the complete first group survives
		got, err := d.FullPCMBuffer()
		if err == nil {
			t.Fatalf("truncated mono stream did not error")
		}
		if len(got.Data) != 64 || !sameInts(got.Data, want) {
			t.Fatalf("error before the good group was preserved: %d samples", len(got.Data))
		}

		// Stereo: complete left packet but truncated right packet must not
		// publish the group (and must report the right packet).
		rightTrunc := stereoLeft.bytes()
		rightTrunc = append(rightTrunc, stereoRight.bytes()[:25]...)
		d2 := newTestDecoder(t, ima4TestFile{chans: 2, groups: 1, bitDepth: 16,
			sampleRate: 44100, data: rightTrunc}.build())
		got2, err := d2.FullPCMBuffer()
		if err == nil {
			t.Fatalf("truncated right packet did not error")
		}
		if len(got2.Data) != 0 {
			t.Fatalf("truncated stereo group published %d scalars", len(got2.Data))
		}
		if err := d2.Err(); err == nil {
			t.Fatalf("decoder Err() is nil after truncation")
		}

		// Illegal bit depth / channels / header index are explicit errors.
		badDepth := newTestDecoder(t, ima4TestFile{chans: 1, groups: 1, bitDepth: 8,
			sampleRate: 44100, data: pkt.bytes()}.build())
		if _, err := badDepth.FullPCMBuffer(); err == nil {
			t.Fatalf("8-bit ima4 did not error")
		}
		badChans := newTestDecoder(t, ima4TestFile{chans: 3, groups: 1, bitDepth: 16,
			sampleRate: 44100, data: pkt.bytes()}.build())
		if _, err := badChans.FullPCMBuffer(); err == nil {
			t.Fatalf("3-channel ima4 did not error")
		}
		badIndexPkt := ima4TestPacket{predictor: 0, index: 89, nibbles: allNibbles(1)}
		badIndex := newTestDecoder(t, ima4TestFile{chans: 1, groups: 1, bitDepth: 16,
			sampleRate: 44100, data: badIndexPkt.bytes()}.build())
		if _, err := badIndex.FullPCMBuffer(); err == nil {
			t.Fatalf("step index 89 did not error")
		}
		badOffsetVal := uint32(200)
		badOffset := newTestDecoder(t, ima4TestFile{chans: 1, groups: 1, bitDepth: 16,
			sampleRate: 44100, headerOffset: &badOffsetVal, data: pkt.bytes()}.build())
		if _, err := badOffset.FullPCMBuffer(); err == nil {
			t.Fatalf("SSND offset overrunning the chunk did not error")
		}

		// A reader that returns 1-3 bytes per Read, and a final Read that
		// returns data alongside io.EOF, must still decode all bytes.
		full := ima4TestFile{chans: 1, groups: 2, bitDepth: 16, sampleRate: 44100,
			data: append(append([]byte{}, pkt.bytes()...), stereoLeft.bytes()...)}.build()
		d3 := NewDecoder(&trickledReader{r: bytes.NewReader(full), max: 3, eofWithData: true})
		got3, err := d3.FullPCMBuffer()
		if err != nil {
			t.Fatalf("trickled FullPCMBuffer: %v", err)
		}
		if want3 := refDecode(append(append([]byte{}, pkt.bytes()...), stereoLeft.bytes()...), 1, 2); !sameInts(got3.Data, want3) {
			t.Fatalf("trickled read mismatch")
		}

		taskCheck(t, true, "truncation, illegal parameters and fragmented reads error correctly")
	})
}

// trickledReader returns at most max bytes per Read and can return a final
// Read carrying data together with io.EOF.
type trickledReader struct {
	r           *bytes.Reader
	max         int
	eofWithData bool
}

func (t *trickledReader) Read(p []byte) (int, error) {
	if t.r.Len() == 0 {
		return 0, io.EOF
	}
	if len(p) > t.max {
		p = p[:t.max]
	}
	n, _ := t.r.Read(p)
	if t.eofWithData && t.r.Len() == 0 {
		return n, io.EOF
	}
	return n, nil
}

func (t *trickledReader) Seek(offset int64, whence int) (int64, error) {
	return t.r.Seek(offset, whence)
}

// ---------------------------------------------------------------------------
// TestIMA4Seek
// ---------------------------------------------------------------------------

func TestIMA4Seek(t *testing.T) {
	// Build a stereo stream whose packets use distinct, non-trivial state.
	// Header predictors are chosen so that a header restart differs from the
	// true continued state (the low 7 bits are absent from the header).
	buildStream := func() ([]byte, []int) {
		var data []byte
		// Group 0: starts from a clean state, deliberately off-zero.
		appendGroup := func(left, right ima4TestPacket) {
			data = append(data, left.bytes()...)
			data = append(data, right.bytes()...)
		}
		appendGroup(
			ima4TestPacket{predictor: -3 << 7, index: 17, nibbles: nibblePattern(1, 4)},
			ima4TestPacket{predictor: 2 << 7, index: 9, nibbles: nibblePattern(0, 10)},
		)
		// Groups 1 and 2: headers approximate the true continuation within
		// their high 9 bits. Restarting from the header alone (dropping the
		// unrecorded low 7 bits) produces different samples, so exact seeks
		// must replay rather than use an approximate header restart.
		for g := 1; g < 3; g++ {
			prev := refDecode(data, 2, g)
			lastLeft := prev[len(prev)-2]
			lastRight := prev[len(prev)-1]
			appendGroup(
				ima4TestPacket{predictor: (lastLeft >> 7) << 7, index: 17, nibbles: nibblePattern(3, 5)},
				ima4TestPacket{predictor: (lastRight >> 7) << 7, index: 9, nibbles: nibblePattern(2, 7)},
			)
		}
		file := ima4TestFile{chans: 2, groups: 3, bitDepth: 16, sampleRate: 44100, data: data}.build()
		return file, refDecode(data, 2, 3)
	}

	stream, want := buildStream()
	if len(want) != 384 {
		t.Fatalf("reference emitted %d scalars, want 384", len(want))
	}

	t.Run("exact", func(t *testing.T) {
		for _, frame := range []int{0, 1, 63, 64, 65, 100, 130, 191} {
			d := newTestDecoder(t, stream)
			if err := d.SeekPCM(int64(frame)); err != nil {
				t.Fatalf("SeekPCM(%d): %v", frame, err)
			}
			got := readAllScalars(t, d, 11)
			wantTail := want[frame*2:]
			if !sameInts(got, wantTail) {
				t.Fatalf("seek to frame %d: tail mismatch\n got %v\nwant %v", frame, got, wantTail)
			}
		}

		// Seeking to the total frame count is EOF, and seeking back to a
		// valid frame afterwards works.
		d := newTestDecoder(t, stream)
		if err := d.SeekPCM(192); err != nil {
			t.Fatalf("SeekPCM(192): %v", err)
		}
		if err := d.SeekPCM(192); err != nil { // total frames is 64*3 = 192
			t.Fatalf("SeekPCM(EOF): %v", err)
		}
		got := readAllScalars(t, d, 5)
		if len(got) != 0 {
			t.Fatalf("read after EOF seek returned %d scalars", len(got))
		}
		if err := d.SeekPCM(3); err != nil {
			t.Fatalf("SeekPCM(3) after EOF: %v", err)
		}
		if got := readAllScalars(t, d, 9); !sameInts(got, want[3*2:]) {
			t.Fatalf("seek back after EOF mismatch")
		}

		// Invalid seeks must report an error and leave the pending playback
		// position untouched.
		d2 := newTestDecoder(t, stream)
		if err := d2.SeekPCM(10); err != nil {
			t.Fatalf("SeekPCM(10): %v", err)
		}
		consumed := &audio.IntBuffer{Data: make([]int, 7)}
		if n, err := d2.PCMBuffer(consumed); err != nil || n != 7 {
			t.Fatalf("partial read before invalid seeks: n=%d err=%v", n, err)
		}
		if !sameInts(consumed.Data, want[10*2:10*2+7]) {
			t.Fatalf("unexpected partial read data")
		}
		if err := d2.SeekPCM(-1); err == nil {
			t.Fatalf("negative frame seek did not error")
		}
		if err := d2.SeekPCM(1000); err == nil {
			t.Fatalf("out of range seek did not error")
		}
		// Continue without re-seeking: the next scalar must be exactly the
		// one that would have followed the partial read.
		resumed, err := d2.FullPCMBuffer()
		if err != nil {
			t.Fatalf("FullPCMBuffer after failed seeks: %v", err)
		}
		if !sameInts(resumed.Data, want[10*2+7:]) {
			t.Fatalf("failed seeks disturbed the playback position")
		}

		// SeekPCM on a non-ima4 file must fail without changing position.
		pcm := func() []byte {
			var comm, ssnd, form bytes.Buffer
			binary.Write(&comm, binary.BigEndian, uint16(1))
			binary.Write(&comm, binary.BigEndian, uint32(4))
			binary.Write(&comm, binary.BigEndian, uint16(16))
			comm.Write([]byte{0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0})
			binary.Write(&ssnd, binary.BigEndian, uint32(0))
			binary.Write(&ssnd, binary.BigEndian, uint32(0))
			binary.Write(&ssnd, binary.BigEndian, []int16{1, 2, 3, 4})
			form.WriteString("AIFF")
			form.WriteString("COMM")
			binary.Write(&form, binary.BigEndian, uint32(comm.Len()))
			form.Write(comm.Bytes())
			form.WriteString("SSND")
			binary.Write(&form, binary.BigEndian, uint32(ssnd.Len()))
			form.Write(ssnd.Bytes())
			var f bytes.Buffer
			f.WriteString("FORM")
			binary.Write(&f, binary.BigEndian, uint32(form.Len()))
			f.Write(form.Bytes())
			return f.Bytes()
		}()
		dp := newTestDecoder(t, pcm)
		buf := &audio.IntBuffer{Data: make([]int, 2)}
		if n, err := dp.PCMBuffer(buf); err != nil || n != 2 {
			t.Fatalf("plain PCM read: n=%d err=%v", n, err)
		}
		if err := dp.SeekPCM(0); err == nil {
			t.Fatalf("SeekPCM on plain AIFF did not error")
		}
		if n, err := dp.PCMBuffer(buf); err != nil || n != 2 {
			t.Fatalf("plain PCM position changed after failed SeekPCM: n=%d err=%v", n, err)
		}
		if buf.Data[0] != 3 || buf.Data[1] != 4 {
			t.Fatalf("plain PCM continued at %v, want [3 4]", buf.Data)
		}

		taskCheck(t, true, "exact seeks replay state; EOF/bounds/rejections behave")
	})

	t.Run("pending", func(t *testing.T) {
		// Release exactly one scalar (left channel of frame 0) and then
		// seek: the pending right scalar, the in-group remainder and the old
		// predictors must all be dropped.
		d := newTestDecoder(t, stream)
		buf := &audio.IntBuffer{Data: make([]int, 1)}
		if n, err := d.PCMBuffer(buf); err != nil || n != 1 {
			t.Fatalf("single scalar: n=%d err=%v", n, err)
		}
		if buf.Data[0] != want[0] {
			t.Fatalf("single scalar=%d, want %d", buf.Data[0], want[0])
		}
		// Now seek to a frame inside the second group; a naive decoder
		// would still have the old in-packet position/right channel.
		if err := d.SeekPCM(70); err != nil {
			t.Fatalf("SeekPCM(70): %v", err)
		}
		got := readAllScalars(t, d, 1)
		if !sameInts(got, want[140:]) {
			t.Fatalf("pending seek to 70 mismatch")
		}

		// Seek to the very next frame (1) after a half-frame read.
		d2 := newTestDecoder(t, stream)
		if _, err := d2.PCMBuffer(&audio.IntBuffer{Data: make([]int, 1)}); err != nil {
			t.Fatalf("half frame: %v", err)
		}
		if err := d2.SeekPCM(1); err != nil {
			t.Fatalf("SeekPCM(1): %v", err)
		}
		if got := readAllScalars(t, d2, 2); !sameInts(got, want[2:]) {
			t.Fatalf("pending seek to 1 mismatch")
		}

		// Rewind resets from frame 0, including after a mid-group read.
		d3 := newTestDecoder(t, stream)
		if err := d3.SeekPCM(97); err != nil {
			t.Fatalf("SeekPCM(97): %v", err)
		}
		if _, err := d3.PCMBuffer(&audio.IntBuffer{Data: make([]int, 3)}); err != nil {
			t.Fatalf("mid-group read: %v", err)
		}
		if err := d3.Rewind(); err != nil {
			t.Fatalf("Rewind: %v", err)
		}
		if got := readAllScalars(t, d3, 13); !sameInts(got, want) {
			t.Fatalf("rewind after pending read mismatch")
		}

		// FullPCMBuffer continues from the current position and must equal
		// the remaining samples read in batches.
		d4 := newTestDecoder(t, stream)
		if err := d4.SeekPCM(33); err != nil {
			t.Fatalf("SeekPCM(33): %v", err)
		}
		partialBuf := &audio.IntBuffer{Data: make([]int, 5)}
		if _, err := d4.PCMBuffer(partialBuf); err != nil {
			t.Fatalf("partial read: %v", err)
		}
		full, err := d4.FullPCMBuffer()
		if err != nil {
			t.Fatalf("FullPCMBuffer after partial: %v", err)
		}
		start := 33*2 + 5
		if !sameInts(full.Data, want[start:]) {
			t.Fatalf("FullPCMBuffer continuation mismatch")
		}

		// PCMLen / Duration / ReadInfo don't consume pending PCM.
		d5 := newTestDecoder(t, stream)
		if _, err := d5.PCMBuffer(&audio.IntBuffer{Data: make([]int, 1)}); err != nil {
			t.Fatalf("read: %v", err)
		}
		for i := 0; i < 3; i++ {
			d5.ReadInfo()
			if d5.PCMLen() != 768 {
				t.Fatalf("PCMLen after read = %d, want 768", d5.PCMLen())
			}
			if _, err := d5.Duration(); err != nil {
				t.Fatalf("Duration after read: %v", err)
			}
		}
		if got := readAllScalars(t, d5, 2); !sameInts(got, want[1:]) {
			t.Fatalf("metadata queries consumed pending PCM")
		}

		// A tiny trickled-and-EOF reader must produce the same samples when
		// read through the decoder, including SeekPCM's rewind-and-replay.
		dTrickle := NewDecoder(&trickledReader{r: bytes.NewReader(stream), max: 3, eofWithData: true})
		if err := dTrickle.SeekPCM(5); err != nil {
			t.Fatalf("trickled SeekPCM: %v", err)
		}
		if got := readAllScalars(t, dTrickle, 13); !sameInts(got, want[5*2:]) {
			t.Fatalf("trickled PCMBuffer mismatch")
		}

		taskCheck(t, true, "pending channel/group state is dropped on seek and preserved on metadata queries")
	})
}

// nibblePattern builds a deterministic 64-nibble pattern.
func nibblePattern(base, step int) [64]int {
	var n [64]int
	for i := range n {
		n[i] = (base + i*step) & 0xf
	}
	return n
}
