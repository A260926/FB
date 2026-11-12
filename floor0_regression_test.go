package vorbis

// Regression tests for floor 0 (LSP) decoding. The whole chain is covered:
// the setup declaration, the codebook vectors inside audio packets, the
// spectral envelope, and the final PCM.
//
// All expectations are computed independently from the Vorbis I
// specification: the curve reference is evaluated in double precision
// directly from the LSP definition, and the PCM reference uses a direct
// cosine transform plus explicit window overlap. The production mapResult,
// Apply, imdct, applyWindow and decodePacket are never used to compute
// expected values.

import (
	"fmt"
	"math"
	"testing"
)

// ---------------------------------------------------------------------------
// LSB-first bitstream construction (the bit order of the Vorbis I spec).
// ---------------------------------------------------------------------------

// lsbWriter packs values LSB-first into bytes, the same order bitReader
// consumes them.
type lsbWriter struct {
	out []byte
	acc uint64
	n   uint
}

func (w *lsbWriter) write(v uint64, bits uint) {
	w.acc |= (v & ((1 << bits) - 1)) << w.n
	w.n += bits
	for w.n >= 8 {
		w.out = append(w.out, byte(w.acc))
		w.acc >>= 8
		w.n -= 8
	}
}

func (w *lsbWriter) bytes() []byte {
	if w.n > 0 {
		return append(w.out, byte(w.acc))
	}
	return w.out
}

// unaryLengths returns the prefix code lengths 1, 2, ..., n-1, n-1. It is a
// complete code, so the codeword assignment is fully determined (see
// writeUnaryCodeword).
func unaryLengths(n int) []int {
	lengths := make([]int, n)
	for i := range lengths {
		lengths[i] = i + 1
	}
	lengths[n-1] = n - 1
	return lengths
}

// unaryTree builds the huffmanCode tree for the unaryLengths code directly,
// without involving the production huffman builder: entry i (0-based) is
// reached by i one-bits followed by a zero-bit, except the last entry, which
// is all one-bits. Leaves are stored as entry*2+1, internal nodes as the
// index of the next node.
func unaryTree(n int) []uint32 {
	tree := make([]uint32, 2*n-2)
	for k := 0; k < n-1; k++ {
		tree[2*k] = uint32(2*k + 1) // leaf for entry k
		if k < n-2 {
			tree[2*k+1] = uint32(2*k + 2) // next internal node
		} else {
			tree[2*k+1] = uint32(2*(n-1) + 1) // leaf for entry n-1
		}
	}
	return tree
}

// writeUnaryCodeword appends the codeword for entry in the unaryLengths code.
func writeUnaryCodeword(w *lsbWriter, entry, entries int) {
	if entry < 0 || entry >= entries {
		panic("codeword entry out of range")
	}
	ones := entry
	if entry == entries-1 {
		ones = entries - 1
	}
	for i := 0; i < ones; i++ {
		w.write(1, 1)
	}
	if entry < entries-1 {
		w.write(0, 1)
	}
}

// vorbisFloatPack converts v to the 32-bit float format of the Vorbis I
// specification (the inverse of float32Unpack). v must be exactly
// representable.
func vorbisFloatPack(v float64) uint32 {
	if v == 0 {
		return 0
	}
	sign := uint32(0)
	if v < 0 {
		sign = 0x80000000
		v = -v
	}
	for e := 788; e > 0; e-- {
		m := math.Ldexp(v, 788-e)
		if m == math.Trunc(m) && m < 1<<21 {
			return sign | uint32(e)<<21 | uint32(m)
		}
	}
	panic("value not representable as vorbis float")
}

// ---------------------------------------------------------------------------
// Independent reference evaluation (double precision, from the spec).
// ---------------------------------------------------------------------------

// barkScale is the Bark scale as defined by the Vorbis I specification.
func barkScale(x float64) float64 {
	return 13.1*math.Atan(.00074*x) + 2.24*math.Atan(.0000000185*x*x) + .0001*x
}

// floor0MapPosition computes the Bark map entry of spectral bin i of a
// spectrum of n bins, directly from the floor 0 definition.
func floor0MapPosition(rate, barkMapSize, i, n int) int {
	b := int(math.Floor(barkScale(float64(rate)*float64(i)/(2*float64(n))) * float64(barkMapSize) / barkScale(0.5*float64(rate))))
	if b > barkMapSize-1 {
		return barkMapSize - 1
	}
	return b
}

// floor0CurveReference evaluates the floor 0 spectral envelope in double
// precision, directly from the LSP definition of the Vorbis I specification.
func floor0CurveReference(rate, barkMapSize, order int, coefficients []float32, amplitude, amplitudeBits, amplitudeOffset, n int) []float64 {
	curve := make([]float64, n)
	for i := 0; i < n; i++ {
		w := math.Pi * float64(floor0MapPosition(rate, barkMapSize, i, n)) / float64(barkMapSize)
		cosw := math.Cos(w)
		var p, q float64
		if order%2 == 1 {
			p = 1 - cosw*cosw
			for j := 0; j <= (order-3)/2; j++ {
				t := math.Cos(float64(coefficients[2*j+1])) - cosw
				p *= 4 * t * t
			}
			q = 0.25
			for j := 0; j <= (order-1)/2; j++ {
				t := math.Cos(float64(coefficients[2*j])) - cosw
				q *= 4 * t * t
			}
		} else {
			p = (1 - cosw*cosw) / 2
			for j := 0; j <= (order-2)/2; j++ {
				t := math.Cos(float64(coefficients[2*j+1])) - cosw
				p *= 4 * t * t
			}
			q = (1 + cosw*cosw) / 2
			for j := 0; j <= (order-2)/2; j++ {
				t := math.Cos(float64(coefficients[2*j])) - cosw
				q *= 4 * t * t
			}
		}
		curve[i] = math.Exp(0.11512925 * (float64(amplitude)*float64(amplitudeOffset)/((math.Exp2(float64(amplitudeBits))-1)*math.Sqrt(p+q)) - float64(amplitudeOffset)))
	}
	return curve
}

// checkSamples compares got against ref with a per-element tolerance of
// 2e-5*(1+|ref|), reporting the first element and the largest deviation.
func checkSamples(t *testing.T, name string, got []float32, ref []float64) {
	t.Helper()
	if len(got) != len(ref) {
		t.Errorf("%s: got %d elements, expected %d", name, len(got), len(ref))
		return
	}
	if len(ref) == 0 {
		return
	}
	maxDev := 0.0
	maxAt := 0
	firstBad := -1
	for i := range ref {
		dev := math.Abs(float64(got[i]) - ref[i])
		if dev > maxDev {
			maxDev, maxAt = dev, i
		}
		if dev > 2e-5*(1+math.Abs(ref[i])) && firstBad < 0 {
			firstBad = i
		}
	}
	t.Logf("%s: first element got=%.8g ref=%.8g, max deviation %.3g at %d", name, got[0], ref[0], maxDev, maxAt)
	if firstBad >= 0 {
		t.Errorf("%s: element %d: got %.8g, expected %.8g (deviation %.3g)", name, firstBad, got[firstBad], ref[firstBad], math.Abs(float64(got[firstBad])-ref[firstBad]))
	}
}

// ---------------------------------------------------------------------------
// TestFloor0Coefficients: cumulative LSP recovery from the codebook vectors.
// ---------------------------------------------------------------------------

func TestFloor0Coefficients(t *testing.T) {
	tests := []struct {
		name    string
		order   int
		dim     int
		vectors [][]float32 // codebook entries
		entries []int       // entries read by the packet, in order
		want    []float32   // cumulative LSP sequence
		n       uint32      // block spectrum size passed to Decode
	}{
		{
			// Three vector reads, the last group short of the full
			// dimension: 5 = 2+2+1, the second component of the third
			// vector must be left unread.
			name:    "order5 three reads, short final group",
			order:   5,
			dim:     2,
			vectors: [][]float32{{0.5, 0.25}, {0.125, 0.375}, {0.75, 0.875}},
			entries: []int{0, 1, 2},
			want:    []float32{0.5, 0.75, 0.875, 1.25, 2.0},
			n:       32,
		},
		{
			// One-dimensional book read three times: the values must
			// accumulate to 0.2, 0.4, 0.6.
			name:    "order3 one-dimensional, three reads",
			order:   3,
			dim:     1,
			vectors: [][]float32{{0.2}, {0.2}, {0.2}},
			entries: []int{0, 1, 2},
			want:    []float32{0.2, 0.4, 0.6},
			n:       128,
		},
		{
			name:    "order1 single read",
			order:   1,
			dim:     1,
			vectors: [][]float32{{1.5}, {0.5}},
			entries: []int{0},
			want:    []float32{1.5},
			n:       32,
		},
		{
			// Even order, exact fit: no truncation.
			name:    "order4 exact fit",
			order:   4,
			dim:     2,
			vectors: [][]float32{{0.5, 0.5}, {0.25, 0.25}},
			entries: []int{0, 1},
			want:    []float32{0.5, 1.0, 1.25, 1.5},
			n:       128,
		},
		{
			// Dimension 4, order 6: the second vector contributes only
			// its first two components.
			name:    "order6 dimension4, short final group",
			order:   6,
			dim:     4,
			vectors: [][]float32{{0.25, 0.5, 0.75, 1.0}, {0.5, 0.25, 9.5, 9.75}},
			entries: []int{0, 1},
			want:    []float32{0.25, 0.75, 1.5, 2.5, 3.0, 3.25},
			n:       32,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			var values []float32
			for _, v := range tc.vectors {
				values = append(values, v...)
			}
			book := codebook{
				dimensions: uint32(tc.dim),
				values:     values,
				entries:    &huffmanCode{tree: unaryTree(len(tc.vectors))},
			}
			f := &floor0{
				order:           uint8(tc.order),
				rate:            48000,
				barkMapSize:     64,
				amplitudeBits:   6,
				amplitudeOffset: 50,
				bookList:        []uint8{0},
			}
			const amplitude = 41
			const sentinel = 0xABC
			w := &lsbWriter{}
			w.write(amplitude, 6) // amplitude
			w.write(0, 1)         // book number (single book in the list)
			for _, e := range tc.entries {
				writeUnaryCodeword(w, e, len(tc.vectors))
			}
			w.write(sentinel, 12) // must still be readable after Decode
			r := newBitReader(w.bytes())
			data := f.Decode(r, []codebook{book}, tc.n)
			d, ok := data.(floor0Data)
			if !ok {
				t.Fatalf("Decode returned %T, expected floor0Data", data)
			}
			if d.amplitude != amplitude {
				t.Errorf("amplitude = %d, expected %d", d.amplitude, amplitude)
			}
			if len(d.coefficients) != len(tc.want) {
				t.Fatalf("got %d coefficients, expected %d", len(d.coefficients), len(tc.want))
			}
			for i, c := range d.coefficients {
				if c != tc.want[i] {
					t.Errorf("coefficient %d = %v, expected %v (cumulative LSP)", i, c, tc.want[i])
				}
			}
			// The last vector takes only the remaining coefficients; the
			// bits following the floor data must not be consumed.
			if s := r.Read32(12); s != sentinel {
				t.Errorf("sentinel after floor data = %#x, expected %#x: Decode consumed too many bits", s, sentinel)
			}
		})
	}

	t.Run("zero amplitude means no floor", func(t *testing.T) {
		f := &floor0{
			order:           3,
			rate:            48000,
			barkMapSize:     64,
			amplitudeBits:   6,
			amplitudeOffset: 50,
			bookList:        []uint8{0},
		}
		w := &lsbWriter{}
		w.write(0, 6) // amplitude zero
		w.write(0x2A, 8)
		data := f.Decode(newBitReader(w.bytes()), nil, 32)
		if data != nil {
			t.Errorf("zero amplitude returned %v, expected nil (no floor)", data)
		}
	})
}

// ---------------------------------------------------------------------------
// TestFloor0Curve: the full spectral envelope.
// ---------------------------------------------------------------------------

func TestFloor0Curve(t *testing.T) {
	tests := []struct {
		name            string
		order           int
		coefficients    []float32
		rate            int
		barkMapSize     int
		amplitude       int
		amplitudeBits   int
		amplitudeOffset int
		n               int // spectrum size (half the block size)
	}{
		{
			// First order: p has no product terms at all.
			name: "order1 block128", order: 1, coefficients: []float32{1.25},
			rate: 48000, barkMapSize: 32, amplitude: 128, amplitudeBits: 8, amplitudeOffset: 20, n: 64,
		},
		{
			name: "order2 block512", order: 2, coefficients: []float32{0.75, 2.5},
			rate: 48000, barkMapSize: 64, amplitude: 140, amplitudeBits: 8, amplitudeOffset: 20, n: 256,
		},
		{
			name: "order3 block512", order: 3, coefficients: []float32{0.5, 1.5, 2.75},
			rate: 44100, barkMapSize: 64, amplitude: 100, amplitudeBits: 8, amplitudeOffset: 30, n: 256,
		},
		{
			name: "order4 block128", order: 4, coefficients: []float32{0.5, 1.0, 2.0, 2.75},
			rate: 48000, barkMapSize: 32, amplitude: 150, amplitudeBits: 8, amplitudeOffset: 25, n: 64,
		},
		{
			// 512 bins mapped onto 16 Bark entries: several bins share
			// one map entry, including the bins closest to zero
			// frequency and the last bin.
			name: "order5 block1024 shared bark entries", order: 5, coefficients: []float32{0.375, 1.0, 1.5, 2.25, 2.875},
			rate: 48000, barkMapSize: 16, amplitude: 180, amplitudeBits: 8, amplitudeOffset: 20, n: 512,
		},
		{
			name: "order6 block1024 shared bark entries", order: 6, coefficients: []float32{0.25, 0.75, 1.25, 1.75, 2.25, 2.75},
			rate: 48000, barkMapSize: 16, amplitude: 90, amplitudeBits: 8, amplitudeOffset: 40, n: 512,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			f := &floor0{
				order:           uint8(tc.order),
				rate:            uint16(tc.rate),
				barkMapSize:     uint16(tc.barkMapSize),
				amplitudeBits:   uint8(tc.amplitudeBits),
				amplitudeOffset: uint8(tc.amplitudeOffset),
			}
			out := make([]float32, tc.n)
			for i := range out {
				out[i] = 1
			}
			f.Apply(out, floor0Data{amplitude: uint32(tc.amplitude), coefficients: tc.coefficients})
			ref := floor0CurveReference(tc.rate, tc.barkMapSize, tc.order, tc.coefficients, tc.amplitude, tc.amplitudeBits, tc.amplitudeOffset, tc.n)
			// The test data uses moderate amplitudes: the reference curve
			// must actually vary and must not degenerate to zero or
			// overflow, otherwise the comparison could pass vacuously.
			minRef, maxRef := math.Inf(1), math.Inf(-1)
			for _, v := range ref {
				if v < minRef {
					minRef = v
				}
				if v > maxRef {
					maxRef = v
				}
			}
			t.Logf("reference curve range [%.4g, %.4g]", minRef, maxRef)
			if minRef < 1e-9 || maxRef > 1e6 || maxRef/minRef < 1.5 {
				t.Errorf("test data not moderate: reference curve range [%.4g, %.4g]", minRef, maxRef)
			}
			checkSamples(t, "floor0 curve", out, ref)
		})
	}
}

// ---------------------------------------------------------------------------
// TestFloor0Packets: complete streams through ReadHeader/Decode/DecodeInto.
// ---------------------------------------------------------------------------

// floor0PacketSpec describes one audio packet of a synthetic stream.
type floor0PacketSpec struct {
	longBlock  bool
	windowPrev bool // window flags, long blocks only
	windowNext bool
	amplitude  []uint32    // per channel; zero means no floor
	lsp        [][]float32 // per channel: target LSP coefficients
	residue    [][]float32 // per channel: residue values
}

// floor0Stream assembles a complete, legal Vorbis stream (identification,
// comment and setup headers plus audio packets) around a single floor 0
// configuration.
type floor0Stream struct {
	name            string
	channels        int
	sampleRate      int
	bs0, bs1        int // short and long block sizes
	order           int
	lspDim          int
	barkMapSize     int
	amplitudeBits   int
	amplitudeOffset int
	residueType     int
	resBook         []float32 // residue codebook values (dimension 1)
	packets         []floor0PacketSpec

	lspBook  [][]float32 // LSP delta vectors, dimension lspDim each
	resIndex map[float32]int
}

// finish derives the LSP codebook from the packet specifications and indexes
// the residue book.
func (s *floor0Stream) finish() {
	s.lspBook = nil
	for _, p := range s.packets {
		for ch := 0; ch < s.channels; ch++ {
			if p.amplitude[ch] == 0 {
				continue
			}
			prev := float32(0)
			for i := 0; i < s.order; i += s.lspDim {
				vec := make([]float32, s.lspDim)
				for k := 0; k < s.lspDim; k++ {
					if i+k < s.order {
						vec[k] = p.lsp[ch][i+k] - prev
						prev = p.lsp[ch][i+k]
					}
				}
				if findVector(s.lspBook, vec) < 0 {
					s.lspBook = append(s.lspBook, vec)
				}
			}
		}
	}
	s.resIndex = make(map[float32]int, len(s.resBook))
	for i, v := range s.resBook {
		s.resIndex[v] = i
	}
}

func findVector(pool [][]float32, vec []float32) int {
	for i, v := range pool {
		if len(v) != len(vec) {
			continue
		}
		equal := true
		for k := range v {
			if v[k] != vec[k] {
				equal = false
				break
			}
		}
		if equal {
			return i
		}
	}
	return -1
}

// residuePattern generates a deterministic, non-zero residue signal.
func residuePattern(pool []float32, n, ch, pkt int) []float32 {
	out := make([]float32, n)
	for i := range out {
		out[i] = pool[(i*5+ch*3+pkt*11)%len(pool)]
	}
	return out
}

func le32(v uint32) []byte {
	return []byte{byte(v), byte(v >> 8), byte(v >> 16), byte(v >> 24)}
}

func (s *floor0Stream) identificationHeader() []byte {
	h := []byte{1, 'v', 'o', 'r', 'b', 'i', 's'}
	h = append(h, 0, 0, 0, 0) // version
	h = append(h, byte(s.channels))
	h = append(h, le32(uint32(s.sampleRate))...)
	h = append(h, le32(0)...) // bitrate maximum
	h = append(h, le32(0)...) // bitrate nominal
	h = append(h, le32(0)...) // bitrate minimum
	bs0log, bs1log := uint8(0), uint8(0)
	for 1<<bs0log != s.bs0 {
		bs0log++
	}
	for 1<<bs1log != s.bs1 {
		bs1log++
	}
	h = append(h, bs0log|bs1log<<4)
	h = append(h, 1) // framing
	return h
}

func (s *floor0Stream) commentHeader() []byte {
	vendor := "floor0 regression test"
	h := []byte{3, 'v', 'o', 'r', 'b', 'i', 's'}
	h = append(h, le32(uint32(len(vendor)))...)
	h = append(h, vendor...)
	h = append(h, le32(0)...) // no comments
	return h
}

// writeCodebookSetup appends a codebook with lookup type 2 to the setup
// header. values holds entries*dim values, each of which must be exactly
// representable as minimum+multiplicand*delta with a valueBits-wide
// multiplicand.
func writeCodebookSetup(w *lsbWriter, dim int, values []float32, minimum, delta float64, valueBits uint) {
	entries := len(values) / dim
	w.write(0x564342, 24) // "BCV"
	w.write(uint64(dim), 16)
	w.write(uint64(entries), 24)
	w.write(0, 1) // not ordered
	w.write(0, 1) // not sparse
	for _, l := range unaryLengths(entries) {
		w.write(uint64(l-1), 5)
	}
	w.write(2, 4) // lookup type 2
	w.write(uint64(vorbisFloatPack(minimum)), 32)
	w.write(uint64(vorbisFloatPack(delta)), 32)
	w.write(uint64(valueBits-1), 4)
	w.write(0, 1) // sequenceP false
	maxMult := uint64(1<<valueBits) - 1
	for _, v := range values {
		m := (float64(v) - minimum) / delta
		im := uint64(m + 0.5)
		if math.Abs(float64(im)-m) > 1e-9 || im > maxMult {
			panic("codebook value not representable")
		}
		w.write(im, valueBits)
	}
}

// writeClassbookSetup appends a scalar-only codebook (lookup type 0).
func writeClassbookSetup(w *lsbWriter, dim, entries int) {
	w.write(0x564342, 24)
	w.write(uint64(dim), 16)
	w.write(uint64(entries), 24)
	w.write(0, 1) // not ordered
	w.write(0, 1) // not sparse
	for _, l := range unaryLengths(entries) {
		w.write(uint64(l-1), 5)
	}
	w.write(0, 4) // lookup type 0
}

func (s *floor0Stream) setupHeader() []byte {
	w := &lsbWriter{}
	// Codebooks: 0 = LSP book, 1 = residue book, 2 = residue classbook.
	w.write(2, 8)
	var lspValues []float32
	for _, v := range s.lspBook {
		lspValues = append(lspValues, v...)
	}
	writeCodebookSetup(w, s.lspDim, lspValues, 0, 0.25, 3)
	writeCodebookSetup(w, 1, s.resBook, -1, 0.25, 4)
	writeClassbookSetup(w, 1, 2)
	// Time domain transforms.
	w.write(0, 6)
	w.write(0, 16)
	// Floors: a single floor 0.
	w.write(0, 6)
	w.write(0, 16)
	w.write(uint64(s.order), 8)
	w.write(uint64(s.sampleRate), 16)
	w.write(uint64(s.barkMapSize), 16)
	w.write(uint64(s.amplitudeBits), 6)
	w.write(uint64(s.amplitudeOffset), 8)
	w.write(0, 4) // one book in the list
	w.write(0, 8) // book 0
	// Residues.
	w.write(0, 6)
	w.write(uint64(s.residueType), 16)
	w.write(0, 24)    // begin
	w.write(4096, 24) // end (clamped to the actual spectrum size)
	w.write(7, 24)    // partition size 8
	w.write(0, 6)     // one classification
	w.write(2, 8)     // classbook
	w.write(1, 3)     // cascade: pass 0 only
	w.write(0, 1)
	w.write(1, 8) // class 0, pass 0: residue book
	// Mappings: one submap, no coupling.
	w.write(0, 6)
	w.write(0, 16)
	w.write(0, 1)
	w.write(0, 1)
	w.write(0, 2)
	w.write(0, 8) // time
	w.write(0, 8) // floor
	w.write(0, 8) // residue
	// Modes: short and long, both using mapping 0.
	w.write(1, 6)
	w.write(0, 1)
	w.write(0, 16)
	w.write(0, 16)
	w.write(0, 8)
	w.write(1, 1)
	w.write(0, 16)
	w.write(0, 16)
	w.write(0, 8)
	w.write(1, 1) // framing
	return append([]byte{5, 'v', 'o', 'r', 'b', 'i', 's'}, w.bytes()...)
}

// lspIndices converts target LSP coefficients into LSP book entry indices
// (the book stores deltas).
func (s *floor0Stream) lspIndices(lsp []float32) []int {
	idx := make([]int, 0, (s.order+s.lspDim-1)/s.lspDim)
	prev := float32(0)
	for i := 0; i < s.order; i += s.lspDim {
		vec := make([]float32, s.lspDim)
		for k := 0; k < s.lspDim; k++ {
			if i+k < s.order {
				vec[k] = lsp[i+k] - prev
				prev = lsp[i+k]
			}
		}
		e := findVector(s.lspBook, vec)
		if e < 0 {
			panic("LSP vector missing from book")
		}
		idx = append(idx, e)
	}
	return idx
}

// packetBytes assembles one audio packet.
func (s *floor0Stream) packetBytes(i int) []byte {
	p := s.packets[i]
	w := &lsbWriter{}
	w.write(0, 1) // audio packet
	if p.longBlock {
		w.write(1, 1) // mode 1 (long)
		w.write(boolBit(p.windowPrev), 1)
		w.write(boolBit(p.windowNext), 1)
	} else {
		w.write(0, 1) // mode 0 (short)
	}
	for ch := 0; ch < s.channels; ch++ {
		w.write(uint64(p.amplitude[ch]), uint(s.amplitudeBits))
		if p.amplitude[ch] != 0 {
			w.write(0, 1) // book number
			for _, e := range s.lspIndices(p.lsp[ch]) {
				writeUnaryCodeword(w, e, len(s.lspBook))
			}
		}
	}
	dnd := make([]bool, s.channels)
	for ch := range dnd {
		dnd[ch] = p.amplitude[ch] == 0
	}
	s.writeResidue(w, i, dnd)
	return w.bytes()
}

func boolBit(b bool) uint64 {
	if b {
		return 1
	}
	return 0
}

// writeResidue appends the residue portion of packet i, mirroring the
// partition/classword structure the decoder expects.
func (s *floor0Stream) writeResidue(w *lsbWriter, pkt int, dnd []bool) {
	p := s.packets[pkt]
	n := s.bs0 / 2
	if p.longBlock {
		n = s.bs1 / 2
	}
	writePartition := func(v []float32) {
		for k := 0; k < 8; k++ {
			e, ok := s.resIndex[v[k]]
			if !ok {
				panic("residue value missing from book")
			}
			writeUnaryCodeword(w, e, len(s.resBook))
		}
	}
	switch s.residueType {
	case 2:
		decode := false
		for _, d := range dnd {
			if !d {
				decode = true
			}
		}
		if !decode {
			return
		}
		interleaved := make([]float32, 0, n*len(dnd))
		for i := 0; i < n; i++ {
			for ch := 0; ch < s.channels; ch++ {
				interleaved = append(interleaved, p.residue[ch][i])
			}
		}
		for part := 0; part < len(interleaved)/8; part++ {
			w.write(0, 1) // classword
			writePartition(interleaved[part*8:])
		}
	case 1:
		partitions := n / 8
		for part := 0; part < partitions; part++ {
			for ch := 0; ch < s.channels; ch++ {
				if !dnd[ch] {
					w.write(0, 1) // classword
				}
			}
			for ch := 0; ch < s.channels; ch++ {
				if !dnd[ch] {
					writePartition(p.residue[ch][part*8:])
				}
			}
		}
	}
}

// spectra computes the reference spectrum of packet i from the target LSP
// coefficients and the residue signal.
func (s *floor0Stream) spectra(i int) [][]float64 {
	p := s.packets[i]
	n := s.bs0 / 2
	if p.longBlock {
		n = s.bs1 / 2
	}
	out := make([][]float64, s.channels)
	for ch := 0; ch < s.channels; ch++ {
		spec := make([]float64, n)
		if p.amplitude[ch] != 0 {
			curve := floor0CurveReference(s.sampleRate, s.barkMapSize, s.order, p.lsp[ch], int(p.amplitude[ch]), s.amplitudeBits, s.amplitudeOffset, n)
			for k := range spec {
				spec[k] = float64(p.residue[ch][k]) * curve[k]
			}
		}
		out[ch] = spec
	}
	return out
}

// ---------------------------------------------------------------------------
// Independent PCM reference: direct cosine transform and explicit window
// overlap. Only the window constants (makeWindow) are shared with the
// production code, which is explicitly allowed.
// ---------------------------------------------------------------------------

// slowIMDCT evaluates the inverse MDCT by direct summation.
func slowIMDCT(spectrum []float64) []float64 {
	n := len(spectrum)
	fn := float64(n)
	out := make([]float64, 2*n)
	for i := range out {
		sum := 0.0
		for k := 0; k < n; k++ {
			sum += spectrum[k] * math.Cos(math.Pi/fn*(float64(i)+0.5+fn/2)*(float64(k)+0.5))
		}
		out[i] = sum
	}
	return out
}

// referenceDecoder reproduces the expected PCM of a stream packet by packet.
type referenceDecoder struct {
	channels     int
	bs0, bs1     int
	win0, win1   []float64
	overlap      [][]float64
	hasOverlap   bool
	overlapShort bool
}

func newReferenceDecoder(channels, bs0, bs1 int) *referenceDecoder {
	d := &referenceDecoder{channels: channels, bs0: bs0, bs1: bs1}
	d.win0 = toFloat64(makeWindow(bs0))
	d.win1 = toFloat64(makeWindow(bs1))
	d.overlap = make([][]float64, channels)
	for c := range d.overlap {
		d.overlap[c] = make([]float64, bs1/2)
	}
	return d
}

func toFloat64(v []float32) []float64 {
	out := make([]float64, len(v))
	for i, x := range v {
		out[i] = float64(x)
	}
	return out
}

func (d *referenceDecoder) Clear() {
	d.hasOverlap = false
}

// applyWindowRef applies the Vorbis window to one raw IMDCT block.
func (d *referenceDecoder) applyWindowRef(raw []float64, size, prev, next int) {
	window := func(s int) []float64 {
		if s == d.bs1 {
			return d.win1
		}
		return d.win0
	}
	center := size / 2
	prevOffset := size/4 - prev/4
	nextOffset := size/4 - next/4
	for i := 0; i < prevOffset; i++ {
		raw[i] = 0
	}
	w := window(prev)
	for i := 0; i < prev/2; i++ {
		raw[prevOffset+i] *= w[i]
	}
	w = window(next)
	for i := 0; i < next/2; i++ {
		raw[center+nextOffset+i] *= w[next/2+i]
	}
	for i := size - nextOffset; i < size; i++ {
		raw[i] = 0
	}
}

// packet computes the expected interleaved PCM of one packet.
func (d *referenceDecoder) packet(longBlock, windowPrev, windowNext bool, spectrum [][]float64) []float64 {
	bs := d.bs0
	if longBlock {
		bs = d.bs1
	}
	offset := d.bs1/4 - d.bs0/4
	center := bs / 2
	prev, next := bs, bs
	if longBlock {
		if !windowPrev {
			prev = d.bs0
		}
		if !windowNext {
			next = d.bs0
		}
	}
	raw := make([][]float64, d.channels)
	for c := 0; c < d.channels; c++ {
		raw[c] = slowIMDCT(spectrum[c])
		d.applyWindowRef(raw[c], bs, prev, next)
	}
	var out []float64
	if d.hasOverlap {
		n := center
		if longBlock && !windowPrev {
			n -= offset
		}
		if !longBlock && !d.overlapShort {
			n += offset
		}
		out = make([]float64, n*d.channels)
		if longBlock {
			start := 0
			if !windowPrev {
				start = offset
			}
			for c := 0; c < d.channels; c++ {
				for i := 0; i < center-start; i++ {
					out[i*d.channels+c] = raw[c][start+i] + d.overlap[c][start+i]
				}
			}
		} else {
			if d.overlapShort {
				for c := 0; c < d.channels; c++ {
					for i := 0; i < center; i++ {
						out[i*d.channels+c] = raw[c][i] + d.overlap[c][offset+i]
					}
				}
			} else {
				for c := 0; c < d.channels; c++ {
					for i := 0; i < offset; i++ {
						out[i*d.channels+c] = d.overlap[c][i]
					}
					for i := offset; i < offset+center; i++ {
						out[i*d.channels+c] = raw[c][i-offset] + d.overlap[c][i]
					}
				}
			}
		}
	}
	for c := 0; c < d.channels; c++ {
		for i := range d.overlap[c] {
			d.overlap[c][i] = 0
		}
	}
	oStart := d.bs1/4 - center/2
	oEnd := d.bs1/4 + center/2
	for c := 0; c < d.channels; c++ {
		for i := oStart; i < oEnd; i++ {
			d.overlap[c][i] = raw[c][center+i-oStart]
		}
	}
	d.overlapShort = !longBlock
	d.hasOverlap = true
	return out
}

// ---------------------------------------------------------------------------

// testPackets runs the stream through Decode and DecodeInto and compares
// both against the independent reference and against each other.
func (s *floor0Stream) testPackets(t *testing.T) {
	headers := [][]byte{s.identificationHeader(), s.commentHeader(), s.setupHeader()}

	// The test data uses moderate amplitudes: every reference curve must
	// stay well away from zero and from overflow, otherwise the PCM
	// comparison could pass vacuously.
	for i := range s.packets {
		p := s.packets[i]
		n := s.bs0 / 2
		if p.longBlock {
			n = s.bs1 / 2
		}
		for ch := 0; ch < s.channels; ch++ {
			if p.amplitude[ch] == 0 {
				continue
			}
			curve := floor0CurveReference(s.sampleRate, s.barkMapSize, s.order, p.lsp[ch], int(p.amplitude[ch]), s.amplitudeBits, s.amplitudeOffset, n)
			minC, maxC := math.Inf(1), math.Inf(-1)
			for _, v := range curve {
				if v < minC {
					minC = v
				}
				if v > maxC {
					maxC = v
				}
			}
			t.Logf("packet %d channel %d: reference curve range [%.4g, %.4g]", i, ch, minC, maxC)
			if minC < 1e-3 || maxC > 1e3 {
				t.Errorf("packet %d channel %d: reference curve range [%.4g, %.4g] not moderate", i, ch, minC, maxC)
			}
		}
	}

	runSequence := func(t *testing.T, useDecodeInto bool) [][]float32 {
		var dec Decoder
		for i, h := range headers {
			if err := dec.ReadHeader(h); err != nil {
				t.Fatalf("ReadHeader %d: %v", i, err)
			}
		}
		if !dec.HeadersRead() {
			t.Fatal("headers not completely read")
		}
		if dec.SampleRate() != s.sampleRate || dec.Channels() != s.channels {
			t.Fatalf("stream parameters: got %d Hz/%d channels, expected %d Hz/%d",
				dec.SampleRate(), dec.Channels(), s.sampleRate, s.channels)
		}
		ref := newReferenceDecoder(s.channels, s.bs0, s.bs1)
		outputs := make([][]float32, 0, len(s.packets))
		decodeOne := func(i int) []float32 {
			pkt := s.packetBytes(i)
			if !useDecodeInto {
				got, err := dec.Decode(pkt)
				if err != nil {
					t.Fatalf("packet %d: %v", i, err)
				}
				return got
			}
			const sentinel = float32(7654.25)
			buf := make([]float32, dec.BufferSize()+8)
			for j := range buf {
				buf[j] = sentinel
			}
			got, err := dec.DecodeInto(pkt, buf)
			if err != nil {
				t.Fatalf("packet %d: %v", i, err)
			}
			for j := len(got); j < len(buf); j++ {
				if buf[j] != sentinel {
					t.Errorf("packet %d: DecodeInto overwrote buffer[%d] past the returned slice", i, j)
					break
				}
			}
			return got
		}
		for i := range s.packets {
			got := decodeOne(i)
			p := s.packets[i]
			want := ref.packet(p.longBlock, p.windowPrev, p.windowNext, s.spectra(i))
			if i == 0 && len(got) != 0 {
				t.Errorf("first packet produced %d samples, expected pre-roll with no output", len(got))
			}
			checkSamples(t, fmt.Sprintf("packet %d", i), got, want)
			outputs = append(outputs, append([]float32(nil), got...))
		}
		// Clear re-arms the pre-roll: the next packet must produce no
		// output, and decoding continues from a clean overlap state.
		dec.Clear()
		ref.Clear()
		if got := decodeOne(0); len(got) != 0 {
			t.Errorf("first packet after Clear produced %d samples, expected pre-roll with no output", len(got))
		}
		ref.packet(s.packets[0].longBlock, s.packets[0].windowPrev, s.packets[0].windowNext, s.spectra(0))
		got := decodeOne(1)
		want := ref.packet(s.packets[1].longBlock, s.packets[1].windowPrev, s.packets[1].windowNext, s.spectra(1))
		checkSamples(t, "packet 1 after Clear", got, want)
		return outputs
	}

	gotDecode := runSequence(t, false)
	gotInto := runSequence(t, true)
	if len(gotDecode) != len(gotInto) {
		t.Fatalf("Decode produced %d outputs, DecodeInto %d", len(gotDecode), len(gotInto))
	}
	for i := range gotDecode {
		if len(gotDecode[i]) != len(gotInto[i]) {
			t.Errorf("packet %d: Decode returned %d samples, DecodeInto %d", i, len(gotDecode[i]), len(gotInto[i]))
			continue
		}
		for j := range gotDecode[i] {
			if gotDecode[i][j] != gotInto[i][j] {
				t.Errorf("packet %d sample %d: Decode=%v, DecodeInto=%v", i, j, gotDecode[i][j], gotInto[i][j])
				break
			}
		}
	}
}

func TestFloor0Packets(t *testing.T) {
	resBook := []float32{-1, -0.75, -0.5, -0.25, 0.25, 0.5, 0.75, 1}

	// Mono, odd order, residue type 2.
	mono := &floor0Stream{
		name: "Mono", channels: 1, sampleRate: 48000,
		bs0: 64, bs1: 256,
		order: 3, lspDim: 2, barkMapSize: 64,
		amplitudeBits: 8, amplitudeOffset: 20,
		residueType: 2,
		resBook:     resBook,
	}
	short, long := mono.bs0/2, mono.bs1/2
	mono.packets = []floor0PacketSpec{
		{longBlock: false, amplitude: []uint32{100},
			lsp:     [][]float32{{0.5, 1.25, 2.25}},
			residue: [][]float32{residuePattern(resBook, short, 0, 0)}},
		{longBlock: true, windowPrev: false, windowNext: true, amplitude: []uint32{140},
			lsp:     [][]float32{{0.5, 1.0, 2.0}},
			residue: [][]float32{residuePattern(resBook, long, 0, 1)}},
		{longBlock: true, windowPrev: true, windowNext: false, amplitude: []uint32{180},
			lsp:     [][]float32{{0.75, 1.5, 2.5}},
			residue: [][]float32{residuePattern(resBook, long, 0, 2)}},
		{longBlock: false, amplitude: []uint32{120},
			lsp:     [][]float32{{0.5, 1.0, 1.5}},
			residue: [][]float32{residuePattern(resBook, short, 0, 3)}},
		// Zero amplitude: no floor for this packet, output is the
		// remaining overlap only.
		{longBlock: false, amplitude: []uint32{0},
			lsp:     [][]float32{nil},
			residue: [][]float32{residuePattern(resBook, short, 0, 4)}},
	}
	mono.finish()

	// Stereo with asymmetric channels, even order, residue type 1.
	stereo := &floor0Stream{
		name: "Stereo", channels: 2, sampleRate: 44100,
		bs0: 64, bs1: 256,
		order: 4, lspDim: 2, barkMapSize: 32,
		amplitudeBits: 8, amplitudeOffset: 25,
		residueType: 1,
		resBook:     resBook,
	}
	stereo.packets = []floor0PacketSpec{
		{longBlock: false, amplitude: []uint32{110, 150},
			lsp:     [][]float32{{0.5, 1.0, 2.0, 2.5}, {0.5, 1.25, 1.75, 2.25}},
			residue: [][]float32{residuePattern(resBook, short, 0, 0), residuePattern(resBook, short, 1, 0)}},
		{longBlock: true, windowPrev: false, windowNext: true, amplitude: []uint32{120, 120},
			lsp:     [][]float32{{0.5, 0.75, 1.5, 2.25}, {0.5, 1.5, 2.0, 2.5}},
			residue: [][]float32{residuePattern(resBook, long, 0, 1), residuePattern(resBook, long, 1, 1)}},
		{longBlock: true, windowPrev: true, windowNext: false, amplitude: []uint32{190, 90},
			lsp:     [][]float32{{0.5, 1.25, 2.0, 2.5}, {0.75, 1.0, 1.75, 2.5}},
			residue: [][]float32{residuePattern(resBook, long, 0, 2), residuePattern(resBook, long, 1, 2)}},
		{longBlock: false, amplitude: []uint32{100, 90},
			lsp:     [][]float32{{0.5, 1.0, 1.5, 2.0}, {0.5, 0.75, 1.25, 2.0}},
			residue: [][]float32{residuePattern(resBook, short, 0, 3), residuePattern(resBook, short, 1, 3)}},
		// Channel 0 has no floor (amplitude zero), channel 1 does.
		{longBlock: false, amplitude: []uint32{0, 120},
			lsp:     [][]float32{nil, {0.5, 1.5, 2.25, 2.5}},
			residue: [][]float32{residuePattern(resBook, short, 0, 4), residuePattern(resBook, short, 1, 4)}},
	}
	stereo.finish()

	t.Run(mono.name, mono.testPackets)
	t.Run(stereo.name, stereo.testPackets)
}
