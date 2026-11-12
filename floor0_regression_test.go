package vorbis

import (
	"math"
	"testing"
)

// This file contains regression tests for the Vorbis I floor type 0.
// All reference values are derived independently in float64: the spectral
// envelope uses the direct LSP product definition, and the PCM reference uses
// a plain cosine-sum IMDCT together with an explicit window/overlap-add.
// Neither the production mapResult/Apply nor the production imdct/applyWindow/
// decodePacket functions are used to compute expectations.
//
// The fixes are best understood as three independent layers:
//
//  1. Bitstream coefficient recovery (floor0.Decode). The floor-0 LSPs are one
//     cumulative sequence spanning every codebook vector: each new vector
//     continues the running sum instead of restarting from zero. The final
//     vector contributes only the coefficients still missing, so it never
//     consumes bits that belong to the residue. A wrong sum here shifts every
//     later LSP and therefore the whole spectral shape.
//  2. Spectral curve (floor0.mapResult/Apply). A block with n bins spans
//     0..rate/2, so bin i sits at (rate/2)*i/n; the old formula multiplied by
//     n and sent low-frequency bins to the top Bark cell. The LSP product also
//     needs signed order arithmetic (order 1 must skip an empty loop) and the
//     odd-order q polynomial starts at 1/4 rather than integer 1/4 == 0, which
//     otherwise overflows at zero frequency. Errors here distort the envelope
//     applied to every residue value before the transform.
//  3. Overlapped PCM (decodePacket path, exercised through Decode/DecodeInto).
//     Once the spectrum is correct it still has to survive IMDCT, the long/
//     short window and block overlap-add. The packet test re-derives PCM with a
//     direct cosine transform and an explicit window/overlap, covering short,
//     long, long and short blocks plus a zero-amplitude packet that must carry
//     no floor from the previous packet.

// ---------------------------------------------------------------------------
// LSB-first bitstream writer (mirrors bitReader's bit ordering)
// ---------------------------------------------------------------------------

type bitWriter struct {
	bits []uint64 // packed LSB-first, one byte per slot while building
}

func newBitWriter() *bitWriter { return &bitWriter{} }

func (w *bitWriter) writeBits(value uint32, n uint) {
	for i := uint(0); i < n; i++ {
		w.bits = append(w.bits, uint64((value>>i)&1))
	}
}

func (w *bitWriter) writeBool(b bool) {
	if b {
		w.bits = append(w.bits, 1)
	} else {
		w.bits = append(w.bits, 0)
	}
}

func (w *bitWriter) writeBytes(b []byte) {
	for _, x := range b {
		w.writeBits(uint32(x), 8)
	}
}

func (w *bitWriter) bytes() []byte {
	out := make([]byte, (len(w.bits)+7)/8)
	for i, b := range w.bits {
		if b != 0 {
			out[i/8] |= 1 << uint(i%8)
		}
	}
	return out
}

func (w *bitWriter) bitCount() int { return len(w.bits) }

// float32Pack is the inverse of float32Unpack: it finds the exponent/mantissa
// representation that unpacks back to the requested value.
func float32Pack(f float32) uint32 {
	if f == 0 {
		return 0
	}
	var sign uint32
	m := float64(f)
	if m < 0 {
		sign = 0x80000000
		m = -m
	}
	for e := 0; e <= 1023; e++ {
		mantissa := m * math.Ldexp(1, 788-e)
		if mantissa >= 0 && mantissa <= 0x1fffff && mantissa == math.Floor(mantissa) {
			return sign | uint32(e)<<21 | uint32(mantissa)
		}
	}
	panic("float32Pack: value is not representable")
}

// ---------------------------------------------------------------------------
// Independent float64 floor-0 reference
// ---------------------------------------------------------------------------

func refBark(x float64) float64 {
	return 13.1*math.Atan(.00074*x) + 2.24*math.Atan(.0000000185*x*x) + .0001*x
}

// refFloor0Map is the independent Bark cell mapping for bin i of a block with
// n spectral bins spanning 0..rate/2 Hz.
func refFloor0Map(i, n int, rate float64, barkMapSize int) int {
	freq := .5 * rate * float64(i) / float64(n)
	b := int(math.Floor(refBark(freq) * float64(barkMapSize) / refBark(.5*rate)))
	if b > barkMapSize-1 {
		b = barkMapSize - 1
	}
	return b
}

// refFloor0Curve evaluates the full floor-0 envelope directly from the LSP
// definition (Vorbis I spec, section 7.2.2).
func refFloor0Curve(order int, coefficients []float64, amplitude uint32, amplitudeBits, amplitudeOffset int, rate float64, barkMapSize, n int) []float64 {
	out := make([]float64, n)
	amplitudeScale := float64(int64(1)<<uint(amplitudeBits) - 1)
	for i := 0; i < n; i++ {
		mapi := refFloor0Map(i, n, rate, barkMapSize)
		w := math.Pi * float64(mapi) / float64(barkMapSize)
		cosw := math.Cos(w)
		var p, q float64
		if order%2 == 1 {
			p = 1 - cosw*cosw
			for j := 0; j <= (order-3)/2; j++ {
				tmp := math.Cos(coefficients[2*j+1]) - cosw
				p *= 4 * tmp * tmp
			}
			q = 1. / 4.
			for j := 0; j <= (order-1)/2; j++ {
				tmp := math.Cos(coefficients[2*j]) - cosw
				q *= 4 * tmp * tmp
			}
		} else {
			p = (1 - cosw*cosw) / 2
			for j := 0; j <= (order-2)/2; j++ {
				tmp := math.Cos(coefficients[2*j+1]) - cosw
				p *= 4 * tmp * tmp
			}
			q = (1 + cosw*cosw) / 2
			for j := 0; j <= (order-2)/2; j++ {
				tmp := math.Cos(coefficients[2*j]) - cosw
				q *= 4 * tmp * tmp
			}
		}
		out[i] = math.Exp(.11512925 * (float64(amplitude)*float64(amplitudeOffset)/
			(amplitudeScale*math.Sqrt(p+q)) - float64(amplitudeOffset)))
	}
	return out
}

// cumulativeLSP returns the LSP sequence produced by repeatedly reading a
// codebook vector, i.e. the running sum of all vector components.
func cumulativeLSP(order int, vector []float64) []float64 {
	out := make([]float64, order)
	last := 0.0
	for i := 0; i < order; i++ {
		last += vector[i%len(vector)]
		out[i] = last
	}
	return out
}

// ---------------------------------------------------------------------------
// Test 1: codebook vector accumulation
// ---------------------------------------------------------------------------

func TestFloor0Coefficients(t *testing.T) {
	// marker bits that must remain readable, untouched, after the floor decode
	const markerBits = 6
	const marker = uint32(0x2d) // 101101b, LSB first

	build := func(ampBits uint, amp uint32, codewords int) *bitReader {
		w := newBitWriter()
		w.writeBits(amp, ampBits)
		if amp != 0 {
			// a one-element book list still costs ilog(1)=1 selector bit
			w.writeBits(0, 1)
		}
		for i := 0; i < codewords; i++ {
			w.writeBits(0, 1) // every codeword selects entry 0
		}
		w.writeBits(marker, markerBits)
		return newBitReader(w.bytes())
	}
	expectMarker := func(t *testing.T, r *bitReader) {
		t.Helper()
		if got := r.Read32(markerBits); got != marker {
			t.Fatalf("residue marker bits consumed: got %#x want %#x", got, marker)
		}
	}

	// A one-dimensional codebook whose entry-0 vector is {0.2}: three reads must
	// produce one cumulative sequence 0.2, 0.4, 0.6 (the original bug reset the
	// running sum after the first full vector).
	tree1 := newHuffmanBuilder(2)
	tree1.Put(0, 1)
	tree1.Put(1, 1)
	book1 := codebook{dimensions: 1, entries: tree1.build(), values: []float32{.2, .3}}
	f1 := floor0{order: 3, amplitudeBits: 2, bookList: []uint8{0}}
	r1 := build(2, 1, 3)
	got1 := f1.Decode(r1, []codebook{book1}, 32).(floor0Data)
	assertCoefficients(t, "dim1", got1.coefficients, []float32{.2, .4, .6})
	expectMarker(t, r1)

	// A two-dimensional codebook {0.1, 0.2}: order 5 needs three vector reads,
	// the last one contributes only one of its two values.
	tree2 := newHuffmanBuilder(2)
	tree2.Put(0, 1)
	tree2.Put(1, 1)
	book2 := codebook{dimensions: 2, entries: tree2.build(), values: []float32{.1, .2, .3, .4}}
	f2 := floor0{order: 5, amplitudeBits: 2, bookList: []uint8{0}}
	r2 := build(2, 1, 3)
	got2 := f2.Decode(r2, []codebook{book2}, 32).(floor0Data)
	assertCoefficients(t, "short final vector", got2.coefficients, []float32{.1, .3, .4, .6, .7})
	expectMarker(t, r2)

	// Order 4 with dimension 2: exactly two full reads, no remainder handling.
	f3 := floor0{order: 4, amplitudeBits: 2, bookList: []uint8{0}}
	r3 := build(2, 1, 2)
	got3 := f3.Decode(r3, []codebook{book2}, 32).(floor0Data)
	assertCoefficients(t, "exact-fit", got3.coefficients, []float32{.1, .3, .4, .6})
	expectMarker(t, r3)

	// Order 1: a single coefficient is taken from a three-dimensional vector,
	// exactly one codeword is read and nothing follows it.
	tree4 := newHuffmanBuilder(2)
	tree4.Put(0, 1)
	tree4.Put(1, 1)
	book4 := codebook{dimensions: 3, entries: tree4.build(), values: []float32{.5, .7, .9, .1, .2, .3}}
	f4 := floor0{order: 1, amplitudeBits: 2, bookList: []uint8{0}}
	r4 := build(2, 1, 1)
	got4 := f4.Decode(r4, []codebook{book4}, 32).(floor0Data)
	assertCoefficients(t, "order 1", got4.coefficients, []float32{.5})
	expectMarker(t, r4)

	// A zero amplitude means "no floor": no codewords at all are consumed and
	// the marker must be the very next thing on the stream.
	f0 := floor0{order: 4, amplitudeBits: 2, bookList: []uint8{0}}
	r0 := build(2, 0, 0)
	if data := f0.Decode(r0, []codebook{book2}, 32); data != nil {
		t.Fatalf("zero amplitude returned floor data %v, want nil", data)
	}
	expectMarker(t, r0)
}

// assertCoefficients compares accumulated float32 LSPs against their expected
// running sums, allowing for ordinary float32 rounding in repeated additions.
func assertCoefficients(t *testing.T, label string, got, want []float32) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("%s: got %d coefficients %v, want %d %v", label, len(got), got, len(want), want)
	}
	for i := range want {
		// cumulative LSPs are moderate values; 1e-6 is well below any real error.
		if d := got[i] - want[i]; d > 1e-6 || d < -1e-6 {
			t.Fatalf("%s coefficient %d: got %g want %g", label, i, got[i], want[i])
		}
	}
}

// ---------------------------------------------------------------------------
// Test 2: complete spectral envelope
// ---------------------------------------------------------------------------

func TestFloor0Curve(t *testing.T) {
	const (
		rate        = 48000.0
		barkMapSize = 64
		ampBits     = 8
		ampOffset   = 10
		amplitude   = 150
	)
	// order, block bin count, LSPs. Covers first order, odd and even orders and
	// both a short (32 bin) and a long (128 bin) frequency grid.
	cases := []struct {
		order int
		n     int
		lsp   []float64
	}{
		{1, 32, []float64{0.9}},
		{2, 32, []float64{0.7, 1.6}},
		{3, 128, []float64{0.35, 0.9, 1.7}},
		{4, 128, []float64{0.3, 0.8, 1.4, 2.2}},
		{5, 32, []float64{0.25, 0.7, 1.1, 1.7, 2.4}},
	}

	// The exact scenario from the defect report: 48 kHz, 32 bins, 64 Bark cells.
	defect := floor0{order: 4, rate: 48000, barkMapSize: 64, amplitudeBits: ampBits, amplitudeOffset: ampOffset}
	if got := defect.mapResult(1, 32); got != 16 {
		t.Fatalf("first non-zero bin mapped to cell %d, want 16", got)
	}
	if got := defect.mapResult(0, 32); got != 0 {
		t.Fatalf("near-zero bin mapped to cell %d, want 0", got)
	}
	if got := defect.mapResult(31, 32); got != 63 {
		t.Fatalf("last bin mapped to cell %d, want 63", got)
	}

	for _, c := range cases {
		f := floor0{
			order: uint8(c.order), rate: uint16(rate), barkMapSize: barkMapSize,
			amplitudeBits: ampBits, amplitudeOffset: ampOffset,
		}
		coeff32 := make([]float32, c.order)
		for i, v := range c.lsp {
			coeff32[i] = float32(v)
		}
		out := make([]float32, c.n)
		for i := range out {
			out[i] = 1
		}
		f.Apply(out, floor0Data{amplitude, coeff32})

		want := refFloor0Curve(c.order, c.lsp, amplitude, ampBits, ampOffset, rate, barkMapSize, c.n)

		minWant, maxWant := want[0], want[0]
		for _, v := range want {
			if v < minWant {
				minWant = v
			}
			if v > maxWant {
				maxWant = v
			}
		}
		if !(maxWant > minWant*1.05) || maxWant < 1e-6 {
			t.Fatalf("order %d: reference curve is degenerate (min=%g max=%g)", c.order, minWant, maxWant)
		}

		firstBad, maxDev := -1, 0.0
		for i := 0; i < c.n; i++ {
			tol := 2e-5 * (1 + math.Abs(want[i]))
			dev := math.Abs(float64(out[i]) - want[i])
			if dev > maxDev {
				maxDev = dev
			}
			if dev > tol && firstBad == -1 {
				firstBad = i
			}
		}
		if firstBad != -1 {
			t.Fatalf("order %d n %d: first deviation at bin %d (got %.9g want %.9g), max deviation %.3g",
				c.order, c.n, firstBad, out[firstBad], want[firstBad], maxDev)
		}
		t.Logf("order %d n %d: %d bins OK, max deviation %.3g", c.order, c.n, c.n, maxDev)
	}

	// Several frequency bins must be able to share one Bark cell: with a tiny
	// two-cell map the lowest bins (750 Hz, 1500 Hz, ...) all land in cell 0,
	// and they must all receive the same envelope value.
	small := floor0{order: 2, rate: 48000, barkMapSize: 2, amplitudeBits: ampBits, amplitudeOffset: ampOffset}
	if small.mapResult(0, 32) != 0 || small.mapResult(1, 32) != 0 || small.mapResult(2, 32) != 0 {
		t.Fatalf("low-frequency bins do not share a Bark cell: %d %d %d",
			small.mapResult(0, 32), small.mapResult(1, 32), small.mapResult(2, 32))
	}
	out := make([]float32, 32)
	for i := range out {
		out[i] = 1
	}
	small.Apply(out, floor0Data{amplitude, []float32{.7, 1.6}})
	if out[0] != out[1] || out[1] != out[2] {
		t.Fatalf("bins sharing a Bark cell got different values: %g %g %g", out[0], out[1], out[2])
	}
}

// ---------------------------------------------------------------------------
// Test 3: end-to-end packets through a real bitstream
// ---------------------------------------------------------------------------

// streamSpec describes a fully self-contained floor-0 vorbis stream.
type streamSpec struct {
	rate      int
	channels  int
	blocks    [2]int // short, long
	barkMap   int
	order     int
	floorVals [2][2]float32 // entry 0 and entry 1 vectors of the floor book
	residue   float32
	packets   []packetSpec
}

type packetSpec struct {
	long     bool
	prevLong bool // window previous flag (long blocks only)
	nextLong bool // window next flag (long blocks only)
	amp      []uint32
	entry    []int // floor codebook entry per channel
	skipRes  []bool
}

// codebookSpec describes a codebook to be serialized.
type codebookSpec struct {
	dim            uint32
	numEntries     uint32
	lookupType     uint8
	minValue       float32
	deltaValue     float32
	valueBits      uint // number of bits per multiplicand (the field stores n-1)
	sequence       bool
	multiplicands  []uint32
	expectedValues []float32 // values the production decoder must reconstruct
}

// writeCodebook serializes one codebook and verifies that the production
// unpacking rules reproduce the intended vector values.
func writeCodebook(w *bitWriter, c codebookSpec) {
	w.writeBits(codebookPattern, 24)
	w.writeBits(c.dim, 16)
	w.writeBits(c.numEntries, 24)
	w.writeBool(false) // not ordered
	w.writeBool(false) // not sparse
	for i := uint32(0); i < c.numEntries; i++ {
		w.writeBits(0, 5) // code length 1: the field stores length-1
	}
	w.writeBits(uint32(c.lookupType), 4)
	w.writeBits(float32Pack(c.minValue), 32)
	w.writeBits(float32Pack(c.deltaValue), 32)
	w.writeBits(uint32(c.valueBits-1), 4)
	w.writeBool(c.sequence)
	for _, m := range c.multiplicands {
		w.writeBits(m, c.valueBits)
	}

	reconstructed := make([]float32, c.numEntries*c.dim)
	if c.lookupType == 1 {
		multiplicands := lookup1Values(int(c.numEntries), c.dim)
		for entry := uint32(0); entry < c.numEntries; entry++ {
			last := float32(0)
			indexDivisor := 1
			for i := uint32(0); i < c.dim; i++ {
				off := (int(entry) / indexDivisor) % multiplicands
				v := float32(c.multiplicands[off])*c.deltaValue + c.minValue + last
				reconstructed[entry*c.dim+i] = v
				if c.sequence {
					last = v
				}
				indexDivisor *= multiplicands
			}
		}
	} else {
		for entry := uint32(0); entry < c.numEntries; entry++ {
			last := float32(0)
			for i := uint32(0); i < c.dim; i++ {
				v := float32(c.multiplicands[entry*c.dim+i])*c.deltaValue + c.minValue + last
				reconstructed[entry*c.dim+i] = v
				if c.sequence {
					last = v
				}
			}
		}
	}
	if len(reconstructed) != len(c.expectedValues) {
		panic("writeCodebook: value length mismatch")
	}
	for i := range reconstructed {
		if reconstructed[i] != c.expectedValues[i] {
			panic("writeCodebook: reconstructed values do not match")
		}
	}
}

func buildHeaders(s *streamSpec) [3][]byte {
	// identification header
	ident := []byte{headerTypeIdentification, 'v', 'o', 'r', 'b', 'i', 's'}
	body := make([]byte, 23)
	body[4] = byte(s.channels)
	body[5] = byte(s.rate)
	body[6] = byte(s.rate >> 8)
	body[7] = byte(s.rate >> 16)
	body[8] = byte(s.rate >> 24)
	body[21] = ilog2(s.blocks[0]) | ilog2(s.blocks[1])<<4
	body[22] = 1 // framing bit
	ident = append(ident, body...)

	// comment header
	comment := []byte{headerTypeComment, 'v', 'o', 'r', 'b', 'i', 's'}
	vendor := []byte("floor0-regression")
	l := make([]byte, 4)
	l[0] = byte(len(vendor))
	comment = append(comment, l...)
	comment = append(comment, vendor...)
	comment = append(comment, 0, 0, 0, 0) // zero comments

	// setup header
	w := newBitWriter()
	w.writeBits(2, 8) // three codebooks
	// Floor codebook, lookup type 2: the two entries are fully explicit so the
	// two channels can use genuinely different coefficient vectors.
	const (
		fltMin   = float32(1. / 16.)
		fltDelta = float32(1. / 64.)
	)
	fltMult := make([]uint32, 4)
	fltExpected := make([]float32, 4)
	for e := 0; e < 2; e++ {
		for k := 0; k < 2; k++ {
			v := s.floorVals[e][k]
			m := uint32((v - fltMin) / fltDelta)
			if fltMin+float32(m)*fltDelta != v {
				panic("floor codebook value is not a min+m*delta lattice point")
			}
			fltMult[e*2+k] = m
			fltExpected[e*2+k] = v
		}
	}
	writeCodebook(w, codebookSpec{
		dim: 2, numEntries: 2, lookupType: 2,
		minValue: fltMin, deltaValue: fltDelta, valueBits: 6,
		multiplicands:  fltMult,
		expectedValues: fltExpected,
	})
	// Residue classification book (scalar, always entry 0).
	writeCodebook(w, codebookSpec{
		dim: 1, numEntries: 2, lookupType: 1,
		minValue: 0, deltaValue: 0, valueBits: 1,
		multiplicands:  []uint32{0, 0},
		expectedValues: []float32{0, 0},
	})
	// Residue value book: one constant four-dimensional vector.
	residueValues := make([]float32, 8)
	for i := range residueValues {
		residueValues[i] = s.residue
	}
	writeCodebook(w, codebookSpec{
		dim: 4, numEntries: 2, lookupType: 1,
		minValue: s.residue, deltaValue: 0, valueBits: 1,
		multiplicands:  []uint32{0},
		expectedValues: residueValues,
	})

	w.writeBits(0, 6) // one time domain transform
	w.writeBits(0, 16)

	w.writeBits(0, 6)  // one floor
	w.writeBits(0, 16) // floor type 0
	w.writeBits(uint32(s.order), 8)
	w.writeBits(uint32(s.rate), 16)
	w.writeBits(uint32(s.barkMap), 16)
	w.writeBits(8, 6)  // amplitude bits
	w.writeBits(10, 8) // amplitude offset
	w.writeBits(0, 4)  // one book in the book list
	w.writeBits(0, 8)  // book number 0

	w.writeBits(0, 6)         // one residue
	w.writeBits(0, 16)        // residue type 0
	w.writeBits(0, 24)        // begin
	w.writeBits(0xffffff, 24) // end (clipped to the spectrum size)
	w.writeBits(3, 24)        // partition size 4
	w.writeBits(0, 6)         // one classification
	w.writeBits(1, 8)         // classification book number 1
	w.writeBits(1, 3)         // cascade: low bits = pass 0 present
	w.writeBool(false)        // no high bits
	w.writeBits(2, 8)         // pass 0 uses book number 2

	w.writeBits(0, 6) // one mapping
	w.writeBits(0, 16)
	w.writeBool(false) // single submap
	w.writeBool(false) // no channel coupling
	w.writeBits(0, 2)  // reserved
	w.writeBits(0, 8)  // unused submap byte
	w.writeBits(0, 8)  // floor number
	w.writeBits(0, 8)  // residue number

	w.writeBits(1, 6) // two modes
	w.writeBits(0, 1) // mode 0: short block
	w.writeBits(0, 16)
	w.writeBits(0, 16)
	w.writeBits(0, 8)
	w.writeBits(1, 1) // mode 1: long block
	w.writeBits(0, 16)
	w.writeBits(0, 16)
	w.writeBits(0, 8)
	w.writeBool(true) // framing bit

	setup := []byte{headerTypeSetup, 'v', 'o', 'r', 'b', 'i', 's'}
	setup = append(setup, w.bytes()...)
	return [3][]byte{ident, comment, setup}
}

func ilog2(x int) byte {
	n := byte(0)
	for (1 << n) < x {
		n++
	}
	return n
}

func buildAudioPacket(s *streamSpec, p *packetSpec) []byte {
	w := newBitWriter()
	w.writeBool(false) // packet type
	if p.long {
		w.writeBits(1, 1)
		w.writeBool(p.prevLong)
		w.writeBool(p.nextLong)
	} else {
		w.writeBits(0, 1)
	}
	n := s.blocks[0] / 2
	if p.long {
		n = s.blocks[1] / 2
	}
	for ch := 0; ch < s.channels; ch++ {
		w.writeBits(p.amp[ch], 8)
		if p.amp[ch] == 0 {
			continue
		}
		w.writeBits(0, 1) // floor book selector; the setup lists one book
		vectors := (s.order + 1) / 2
		for i := 0; i < vectors; i++ {
			w.writeBits(uint32(p.entry[ch]), 1)
		}
	}
	for ch := 0; ch < s.channels; ch++ {
		if p.skipRes[ch] {
			continue
		}
		for partition := 0; partition < n/4; partition++ {
			w.writeBits(0, 1) // class word: entry 0
			w.writeBits(0, 1) // residue vector: entry 0
		}
	}
	return w.bytes()
}

// --- independent float64 synthesis reference ---

func refIMDCT(spectrum []float64) []float64 {
	n2 := len(spectrum)
	n := 2 * n2
	out := make([]float64, n)
	for i := 0; i < n; i++ {
		sum := 0.0
		for k := 0; k < n2; k++ {
			sum += spectrum[k] * math.Cos((math.Pi/float64(n2))*
				(float64(i)+.5+float64(n2)/2)*(float64(k)+.5))
		}
		out[i] = sum
	}
	return out
}

func refWindow(raw []float64, size, prev, next int, longWindow, shortWindow []float64) []float64 {
	center := size / 2
	prevOffset := size/4 - prev/4
	nextOffset := size/4 - next/4
	// a section is windowed with the long window exactly when its length
	// equals the long block size, otherwise with the short window
	winPrev, winNext := shortWindow, shortWindow
	if prev == len(longWindow) {
		winPrev = longWindow
	}
	if next == len(longWindow) {
		winNext = longWindow
	}
	out := make([]float64, size)
	for i := 0; i < size; i++ {
		switch {
		case i < prevOffset:
			out[i] = 0
		case i < prevOffset+prev/2:
			out[i] = raw[i] * winPrev[i-prevOffset]
		case i < center+nextOffset:
			out[i] = raw[i]
		case i < center+nextOffset+next/2:
			out[i] = raw[i] * winNext[next/2+i-(center+nextOffset)]
		default:
			out[i] = 0
		}
	}
	return out
}

type refDecoder struct {
	blocks   [2]int
	channels int
	overlap  []float64
	has      bool
	short    bool
	winLong  []float64
	winShort []float64
}

func newRefDecoder(s *streamSpec) *refDecoder {
	wl32 := makeWindow(s.blocks[1])
	ws32 := makeWindow(s.blocks[0])
	wl := make([]float64, len(wl32))
	ws := make([]float64, len(ws32))
	for i := range wl {
		wl[i] = float64(wl32[i])
	}
	for i := range ws {
		ws[i] = float64(ws32[i])
	}
	return &refDecoder{
		blocks:   s.blocks,
		channels: s.channels,
		overlap:  make([]float64, s.blocks[1]*s.channels),
		winLong:  wl,
		winShort: ws,
	}
}

func (d *refDecoder) decode(s *streamSpec, p *packetSpec) []float64 {
	bs := d.blocks[0]
	if p.long {
		bs = d.blocks[1]
	}
	center := bs / 2
	prev, next := bs, bs
	if p.long {
		if !p.prevLong {
			prev = d.blocks[0]
		}
		if !p.nextLong {
			next = d.blocks[0]
		}
	}
	n := bs / 2
	spectra := make([][]float64, d.channels)
	nonZero := false
	for ch := 0; ch < d.channels; ch++ {
		spectrum := make([]float64, n)
		if p.amp[ch] != 0 {
			vector := make([]float64, 2)
			vector[0] = float64(s.floorVals[p.entry[ch]][0])
			vector[1] = float64(s.floorVals[p.entry[ch]][1])
			lsp := cumulativeLSP(s.order, vector)
			curve := refFloor0Curve(s.order, lsp, p.amp[ch], 8, 10,
				float64(s.rate), s.barkMap, n)
			for i := range spectrum {
				spectrum[i] = float64(s.residue) * curve[i]
				if spectrum[i] != 0 {
					nonZero = true
				}
			}
		}
		spectra[ch] = spectrum
	}
	if !nonZero {
		// a zero-amplitude packet must still participate in overlap, but with a
		// genuinely empty spectrum - nothing is carried over from earlier floors.
		for ch := range spectra {
			for i := range spectra[ch] {
				if spectra[ch][i] != 0 {
					panic("reference: zero-amplitude channel contains data")
				}
			}
		}
	}

	raw := make([][]float64, d.channels)
	windowed := make([][]float64, d.channels)
	for ch := 0; ch < d.channels; ch++ {
		raw[ch] = refIMDCT(spectra[ch])
		windowed[ch] = refWindow(raw[ch], bs, prev, next, d.winLong, d.winShort)
	}

	offset := d.blocks[1]/4 - d.blocks[0]/4
	samples := 0
	if d.has {
		samples = bs / 2
		if p.long && !p.prevLong {
			samples -= offset
		}
		if !p.long && !d.short {
			samples += offset
		}
	}
	out := make([]float64, samples*d.channels)
	if d.has {
		if p.long {
			start := 0
			if !p.prevLong {
				start = offset
			}
			for ch := 0; ch < d.channels; ch++ {
				for i := 0; i < center-start; i++ {
					out[i*d.channels+ch] = windowed[ch][start+i] + d.overlap[(start+i)*d.channels+ch]
				}
			}
		} else {
			if d.short {
				for ch := 0; ch < d.channels; ch++ {
					for i := 0; i < center; i++ {
						out[i*d.channels+ch] = windowed[ch][i] + d.overlap[(offset+i)*d.channels+ch]
					}
				}
			} else {
				for i := 0; i < offset*d.channels; i++ {
					out[i] = d.overlap[i]
				}
				for ch := 0; ch < d.channels; ch++ {
					for i := offset; i < offset+center; i++ {
						out[i*d.channels+ch] = windowed[ch][i-offset] + d.overlap[i*d.channels+ch]
					}
				}
			}
		}
	}

	overlapCenter := d.blocks[1] / 4
	oStart := overlapCenter - center/2
	oEnd := overlapCenter + center/2
	for i := range d.overlap {
		d.overlap[i] = 0
	}
	for ch := 0; ch < d.channels; ch++ {
		for i := oStart; i < oEnd; i++ {
			d.overlap[i*d.channels+ch] = windowed[ch][center+i-oStart]
		}
	}
	d.has = true
	d.short = !p.long
	return out
}

func makeStream(channels int) *streamSpec {
	s := &streamSpec{
		rate:     48000,
		channels: channels,
		barkMap:  16,
		order:    4,
		// dyadic fractions so the codebook float packing is bit-exact. The
		// accumulated LSPs stay well separated and away from 0 and pi, which
		// keeps the envelope moderate instead of overflowing at the band edges.
		floorVals: [2][2]float32{{1. / 2., 11. / 16.}, {3. / 8., 3. / 4.}},
		residue:   3. / 8.,
	}
	if channels == 1 {
		s.rate = 8000
		s.blocks = [2]int{32, 64}
		s.order = 3
	} else {
		s.blocks = [2]int{64, 256}
	}
	// short, long, long(zero amplitude), short
	s.packets = []packetSpec{
		{long: false, amp: amps(channels, 120), entry: entries(channels, 0), skipRes: falses(channels)},
		{long: true, prevLong: false, nextLong: true, amp: amps(channels, 140), entry: stereoEntries(channels), skipRes: falses(channels)},
		{long: true, prevLong: true, nextLong: false, amp: zeroAmps(channels), entry: stereoEntries(channels), skipRes: zeroSkips(channels)},
		{long: false, amp: amps(channels, 160), entry: stereoEntries(channels), skipRes: falses(channels)},
	}
	return s
}

func amps(channels int, a uint32) []uint32 {
	out := make([]uint32, channels)
	for i := range out {
		out[i] = a
	}
	return out
}

func entries(channels, e int) []int {
	out := make([]int, channels)
	for i := range out {
		out[i] = e
	}
	return out
}

func stereoEntries(channels int) []int {
	out := make([]int, channels)
	for i := range out {
		out[i] = i // mono stays on entry 0, stereo uses asymmetric entries 0/1
	}
	return out
}

func falses(channels int) []bool { return make([]bool, channels) }

func zeroAmps(channels int) []uint32 {
	out := make([]uint32, channels)
	if channels == 2 {
		out[1] = 150 // only channel 0 has no floor in stereo
	}
	return out
}

func zeroSkips(channels int) []bool {
	out := make([]bool, channels)
	if channels == 1 {
		out[0] = true
	} else {
		out[0] = true
	}
	return out
}

func TestFloor0Packets(t *testing.T) {
	const sentinel = float32(12345.75)
	for _, channels := range []int{1, 2} {
		s := makeStream(channels)
		headers := buildHeaders(s)
		packets := make([][]byte, len(s.packets))
		for i := range packets {
			packets[i] = buildAudioPacket(s, &s.packets[i])
		}

		// reference PCM, synthesized straight from the LSPs and residue values
		ref := newRefDecoder(s)
		var refOut [][]float64
		for i := range s.packets {
			refOut = append(refOut, ref.decode(s, &s.packets[i]))
		}
		refSamples := 0
		maxRef := 0.0
		for _, o := range refOut {
			refSamples += len(o) / channels
			for _, v := range o {
				if math.Abs(v) > maxRef {
					maxRef = math.Abs(v)
				}
			}
		}
		if refSamples == 0 || maxRef < 1e-3 {
			t.Fatalf("channels %d: reference PCM is empty/silent (samples=%d max=%g)", channels, refSamples, maxRef)
		}

		runDecoder := func(t *testing.T, into bool) [][]float32 {
			var dec Decoder
			for _, h := range headers {
				if err := dec.ReadHeader(h); err != nil {
					t.Fatalf("ReadHeader: %v", err)
				}
			}
			if dec.SampleRate() != s.rate || dec.Channels() != channels {
				t.Fatalf("header round trip: rate=%d ch=%d", dec.SampleRate(), dec.Channels())
			}
			buf := make([]float32, dec.BufferSize()+8)
			var got [][]float32
			for _, pkt := range packets {
				if into {
					for i := range buf {
						buf[i] = sentinel
					}
					out, err := dec.DecodeInto(pkt, buf)
					if err != nil {
						t.Fatalf("DecodeInto: %v", err)
					}
					copied := append([]float32(nil), out...)
					got = append(got, copied)
					for i := len(out); i < len(buf); i++ {
						if buf[i] != sentinel {
							t.Fatalf("sentinel at buffer position %d was overwritten", i)
						}
					}
				} else {
					out, err := dec.Decode(pkt)
					if err != nil {
						t.Fatalf("Decode: %v", err)
					}
					got = append(got, out)
				}
			}
			return got
		}

		gotDecode := runDecoder(t, false)
		gotInto := runDecoder(t, true)

		if len(gotDecode) != len(refOut) || len(gotInto) != len(refOut) {
			t.Fatalf("channels %d: packet count %d/%d vs reference %d",
				channels, len(gotDecode), len(gotInto), len(refOut))
		}
		for i := range refOut {
			if len(gotDecode[i]) != len(refOut[i]) || len(gotInto[i]) != len(refOut[i]) {
				t.Fatalf("channels %d packet %d: lengths decode=%d into=%d reference=%d",
					channels, i, len(gotDecode[i]), len(gotInto[i]), len(refOut[i]))
			}
			// the two public entry points must agree sample for sample
			for j := range gotDecode[i] {
				if gotDecode[i][j] != gotInto[i][j] {
					t.Fatalf("channels %d packet %d sample %d: Decode=%g DecodeInto=%g",
						channels, i, j, gotDecode[i][j], gotInto[i][j])
				}
			}
			firstBad, maxDev := -1, 0.0
			for j, want := range refOut[i] {
				dev := math.Abs(float64(gotDecode[i][j]) - want)
				if dev > maxDev {
					maxDev = dev
				}
				tol := 1e-4 * (1 + math.Abs(want))
				if dev > tol && firstBad == -1 {
					firstBad = j
				}
			}
			if firstBad != -1 {
				t.Fatalf("channels %d packet %d: first PCM deviation at sample %d (got %.9g want %.9g), max deviation %.3g",
					channels, i, firstBad, gotDecode[i][firstBad], refOut[i][firstBad], maxDev)
			}
			t.Logf("channels %d packet %d: %d samples OK, max deviation %.3g",
				channels, i, len(refOut[i])/channels, maxDev)
		}
		if len(gotDecode[0]) != 0 {
			t.Fatalf("channels %d: first (pre-roll) packet produced %d samples", channels, len(gotDecode[0]))
		}

		// Clear() must reset the overlap state: re-running the pre-roll packet
		// yields no output again, and the following packet matches the first run.
		var dec Decoder
		for _, h := range headers {
			if err := dec.ReadHeader(h); err != nil {
				t.Fatalf("ReadHeader after Clear: %v", err)
			}
		}
		for _, pkt := range packets {
			if _, err := dec.Decode(pkt); err != nil {
				t.Fatalf("Decode before Clear: %v", err)
			}
		}
		dec.Clear()
		out, err := dec.Decode(packets[0])
		if err != nil {
			t.Fatalf("Decode after Clear: %v", err)
		}
		if len(out) != 0 {
			t.Fatalf("after Clear, pre-roll packet produced %d samples, want 0", len(out))
		}
		out, err = dec.Decode(packets[1])
		if err != nil {
			t.Fatalf("Decode post-Clear packet: %v", err)
		}
		if len(out) != len(gotDecode[1]) {
			t.Fatalf("post-Clear packet length %d, want %d", len(out), len(gotDecode[1]))
		}
		for i := range out {
			if out[i] != gotDecode[1][i] {
				t.Fatalf("post-Clear packet sample %d differs: %g vs %g", i, out[i], gotDecode[1][i])
			}
		}
	}
}
