package wav

import (
	"bytes"
	"encoding/binary"
	"io"
	"math/rand"
	"testing"
)

func wavlChunk(id string, body []byte) []byte {
	var w bytes.Buffer
	w.WriteString(id)
	binary.Write(&w, binary.LittleEndian, uint32(len(body)))
	w.Write(body)
	if len(body)%2 != 0 {
		w.WriteByte(0)
	}
	return w.Bytes()
}

func wavlRiff(format WavFormat, children []byte, extra ...[]byte) []byte {
	var w bytes.Buffer
	var fb bytes.Buffer
	binary.Write(&fb, binary.LittleEndian, format)
	var body bytes.Buffer
	body.Write([]byte("WAVE"))
	body.Write(wavlChunk("fmt ", fb.Bytes()))
	for _, e := range extra {
		body.Write(e)
	}
	body.Write(wavlChunk("LIST", append([]byte("wavl"), children...)))
	w.WriteString("RIFF")
	binary.Write(&w, binary.LittleEndian, uint32(body.Len()))
	w.Write(body.Bytes())
	return w.Bytes()
}

func wavlData(b ...byte) []byte { return wavlChunk("data", b) }

func wavlSlnt(frames uint32) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, frames)
	return wavlChunk("slnt", b)
}

func stereo16Wavl(children []byte, extra ...[]byte) []byte {
	return wavlRiff(WavFormat{AudioFormatPCM, 2, 8000, 32000, 4, 16}, children, extra...)
}

func readAllWavlBytes(t *testing.T, b []byte) []byte {
	t.Helper()
	all, err := io.ReadAll(NewReader(bytes.NewReader(b)))
	if err != nil {
		t.Fatalf("io.ReadAll failed: %v", err)
	}
	return all
}

func exampleWavlChildren() []byte {
	var c bytes.Buffer
	c.Write(wavlData(10, 0, 236, 255, 30, 0, 216, 255))
	c.Write(wavlSlnt(2))
	c.Write(wavlData(50, 0, 196, 255))
	return c.Bytes()
}

var exampleFrames = [][2]int{{10, -20}, {30, -40}, {30, -40}, {30, -40}, {50, -60}}

var exampleBytes = []byte{
	10, 0, 236, 255,
	30, 0, 216, 255,
	30, 0, 216, 255,
	30, 0, 216, 255,
	50, 0, 196, 255,
}

func TestWavlExpandsSegments(t *testing.T) {
	r := NewReader(bytes.NewReader(stereo16Wavl(exampleWavlChildren())))
	d, err := r.Duration()
	if err != nil {
		t.Fatal(err)
	}
	if d.Nanoseconds() != 625000 {
		t.Fatalf("duration=%s want 625us", d)
	}
	if r.WavData.Size != 20 {
		t.Fatalf("expanded size=%d want 20", r.WavData.Size)
	}

	samples, err := r.ReadSamples(5)
	if err != nil {
		t.Fatalf("ReadSamples: %v", err)
	}
	if len(samples) != len(exampleFrames) {
		t.Fatalf("got %d samples want %d", len(samples), len(exampleFrames))
	}
	for i, f := range exampleFrames {
		if samples[i].Values != f {
			t.Fatalf("frame %d=%v want %v", i, samples[i].Values, f)
		}
	}
}

func TestWavlByteStreamMatchesSamples(t *testing.T) {
	got := readAllWavlBytes(t, stereo16Wavl(exampleWavlChildren()))
	if !bytes.Equal(got, exampleBytes) {
		t.Fatalf("bytes=%v want %v", got, exampleBytes)
	}
}

func TestWavlZeroLengthSegments(t *testing.T) {
	var c bytes.Buffer
	c.Write(wavlData(1, 0, 2, 0))
	c.Write(wavlSlnt(0))
	c.Write(wavlData())
	c.Write(wavlSlnt(0))
	c.Write(wavlData(3, 0, 4, 0))

	got := readAllWavlBytes(t, stereo16Wavl(c.Bytes()))
	want := []byte{1, 0, 2, 0, 3, 0, 4, 0}
	if !bytes.Equal(got, want) {
		t.Fatalf("bytes=%v want %v", got, want)
	}
}

func TestWavlLeadingSilenceIsDigitalSilence(t *testing.T) {
	t.Run("16bit", func(t *testing.T) {
		var c bytes.Buffer
		c.Write(wavlSlnt(2))
		c.Write(wavlData(5, 0, 6, 0))
		got := readAllWavlBytes(t, stereo16Wavl(c.Bytes()))
		want := []byte{0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 6, 0}
		if !bytes.Equal(got, want) {
			t.Fatalf("bytes=%v want %v", got, want)
		}
	})

	t.Run("8bit", func(t *testing.T) {
		format := WavFormat{AudioFormatPCM, 1, 8000, 8000, 1, 8}
		var c bytes.Buffer
		c.Write(wavlSlnt(2))
		c.Write(wavlData(200))
		got := readAllWavlBytes(t, wavlRiff(format, c.Bytes()))
		want := []byte{128, 128, 200}
		if !bytes.Equal(got, want) {
			t.Fatalf("bytes=%v want %v", got, want)
		}
	})
}

func TestWavlUnknownOddChunkIsSkippedWithPadding(t *testing.T) {
	unknown := wavlChunk("junk", []byte{0xaa, 0xbb, 0xcc})
	if len(unknown) != 8+4 {
		t.Fatalf("odd chunk must include one pad byte, len=%d", len(unknown))
	}
	var c bytes.Buffer
	c.Write(wavlData(1, 0, 2, 0))
	c.Write(unknown)
	c.Write(wavlSlnt(1))
	c.Write(wavlData(7, 0, 8, 0))

	got := readAllWavlBytes(t, stereo16Wavl(c.Bytes()))
	want := []byte{1, 0, 2, 0, 1, 0, 2, 0, 7, 0, 8, 0}
	if !bytes.Equal(got, want) {
		t.Fatalf("bytes=%v want %v", got, want)
	}
}

func TestWavlConsecutiveSilentSegmentsHoldValue(t *testing.T) {
	var c bytes.Buffer
	c.Write(wavlData(9, 0, 1, 0))
	c.Write(wavlSlnt(1))
	c.Write(wavlSlnt(2))
	c.Write(wavlData(3, 0, 4, 0))

	got := readAllWavlBytes(t, stereo16Wavl(c.Bytes()))
	want := []byte{
		9, 0, 1, 0,
		9, 0, 1, 0,
		9, 0, 1, 0,
		9, 0, 1, 0,
		3, 0, 4, 0,
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("bytes=%v want %v", got, want)
	}
}

func TestWavlReadOneByteAtATime(t *testing.T) {
	r := NewReader(bytes.NewReader(stereo16Wavl(exampleWavlChildren())))
	var got []byte
	one := make([]byte, 1)
	for {
		n, err := r.Read(one)
		if n == 1 {
			got = append(got, one[0])
		}
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
	}
	if !bytes.Equal(got, exampleBytes) {
		t.Fatalf("bytes=%v want %v", got, exampleBytes)
	}
}

func TestWavlMixedReadAndReadSamplesShareProgress(t *testing.T) {
	r := NewReader(bytes.NewReader(stereo16Wavl(exampleWavlChildren())))
	first := make([]byte, 4)
	if _, err := io.ReadFull(r, first); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(first, []byte{10, 0, 236, 255}) {
		t.Fatalf("first frame=%v", first)
	}

	samples, err := r.ReadSamples(4)
	if err != nil {
		t.Fatalf("ReadSamples after Read: %v", err)
	}
	want := [][2]int{{30, -40}, {30, -40}, {30, -40}, {50, -60}}
	if len(samples) != len(want) {
		t.Fatalf("got %d samples want %d", len(samples), len(want))
	}
	for i, f := range want {
		if samples[i].Values != f {
			t.Fatalf("frame %d=%v want %v", i, samples[i].Values, f)
		}
	}
}

func TestWavlReadSamplesRejectsUnalignedPosition(t *testing.T) {
	var c bytes.Buffer
	c.Write(wavlSlnt(3))

	r := NewReader(bytes.NewReader(stereo16Wavl(c.Bytes())))
	one := make([]byte, 1)
	if _, err := r.Read(one); err != nil {
		t.Fatal(err)
	}
	if _, err := r.ReadSamples(1); err == nil {
		t.Fatal("ReadSamples on an unaligned position must return an error")
	}

	rest := make([]byte, 3)
	if _, err := io.ReadFull(r, rest); err != nil {
		t.Fatalf("completing frame with Read failed: %v", err)
	}
	samples, err := r.ReadSamples(2)
	if err != nil {
		t.Fatalf("ReadSamples after realignment failed: %v", err)
	}
	if len(samples) != 2 || samples[0].Values != [2]int{0, 0} || samples[1].Values != [2]int{0, 0} {
		t.Fatalf("samples=%v", samples)
	}
}

func TestWavlReadSamplesKeepsTailBatchAtEOF(t *testing.T) {
	var c bytes.Buffer
	c.Write(wavlData(10, 0, 236, 255, 30, 0, 216, 255))
	c.Write(wavlSlnt(1))

	r := NewReader(bytes.NewReader(stereo16Wavl(c.Bytes())))
	samples, err := r.ReadSamples(10)
	if err != nil {
		t.Fatalf("final batch must keep valid samples, got err=%v", err)
	}
	if len(samples) != 3 {
		t.Fatalf("got %d samples want 3", len(samples))
	}
	if samples[2].Values != [2]int{30, -40} {
		t.Fatalf("last frame=%v", samples[2].Values)
	}
	if _, err = r.ReadSamples(1); err != io.EOF {
		t.Fatalf("read after EOF must return io.EOF, got %v", err)
	}
}

func TestWavlDurationIsStableAndDoesNotConsume(t *testing.T) {
	var c bytes.Buffer
	c.Write(wavlSlnt(2))
	c.Write(wavlData(50, 0, 196, 255))
	b := stereo16Wavl(c.Bytes())

	r := NewReader(bytes.NewReader(b))
	d1, err := r.Duration()
	if err != nil {
		t.Fatal(err)
	}
	d2, err := r.Duration()
	if err != nil {
		t.Fatal(err)
	}
	if d1 != d2 || d1.Nanoseconds() != 375000 {
		t.Fatalf("duration d1=%s d2=%s", d1, d2)
	}

	if _, err = r.Read(make([]byte, 4)); err != nil {
		t.Fatal(err)
	}
	d3, err := r.Duration()
	if err != nil {
		t.Fatal(err)
	}
	if d3 != d1 {
		t.Fatalf("duration changed after Read: %s vs %s", d3, d1)
	}

	all, err := io.ReadAll(r)
	if err != nil {
		t.Fatal(err)
	}
	if len(all) != 8 {
		t.Fatalf("Duration consumed audio, remaining=%d", len(all))
	}
	d4, err := r.Duration()
	if err != nil {
		t.Fatal(err)
	}
	if d4 != d1 {
		t.Fatalf("duration after EOF changed: %s vs %s", d4, d1)
	}
}

func TestWavlSupportsSupportedBitDepthsAndChannels(t *testing.T) {
	cases := []struct {
		name   string
		format WavFormat
		frame  []byte
		want   int
	}{
		{"mono8", WavFormat{AudioFormatPCM, 1, 8000, 8000, 1, 8}, []byte{200}, 200},
		{"mono16", WavFormat{AudioFormatPCM, 1, 8000, 16000, 2, 16}, []byte{236, 255}, -20},
		{"mono24", WavFormat{AudioFormatPCM, 1, 8000, 24000, 3, 24}, []byte{250, 255, 255}, -6},
		{"mono32", WavFormat{AudioFormatPCM, 1, 8000, 32000, 4, 32}, []byte{236, 255, 255, 255}, -20},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var c bytes.Buffer
			c.Write(wavlChunk("data", tc.frame))
			c.Write(wavlSlnt(1))
			r := NewReader(bytes.NewReader(wavlRiff(tc.format, c.Bytes())))
			samples, err := r.ReadSamples(2)
			if err != nil {
				t.Fatal(err)
			}
			if len(samples) != 2 {
				t.Fatalf("got %d samples", len(samples))
			}
			if samples[0].Values[0] != tc.want || samples[1].Values[0] != tc.want {
				t.Fatalf("samples=%v want %d held", samples, tc.want)
			}
		})
	}
}

func TestWavlReadAcrossSegmentWithSmallBuffer(t *testing.T) {
	var c bytes.Buffer
	c.Write(wavlData(1, 2, 3, 4))
	c.Write(wavlSlnt(1))
	c.Write(wavlData(5, 6, 7, 8))

	r := NewReader(bytes.NewReader(stereo16Wavl(c.Bytes())))
	var got []byte
	buf := make([]byte, 3)
	for {
		n, err := r.Read(buf)
		got = append(got, buf[:n]...)
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
	}
	want := []byte{1, 2, 3, 4, 1, 2, 3, 4, 5, 6, 7, 8}
	if !bytes.Equal(got, want) {
		t.Fatalf("bytes=%v want %v", got, want)
	}
}

func TestWavlErrors(t *testing.T) {
	t.Run("truncated sub-chunk header", func(t *testing.T) {
		var c bytes.Buffer
		c.Write([]byte("wavl"))
		c.Write([]byte("dat"))
		b := wavlRiff(WavFormat{AudioFormatPCM, 2, 8000, 32000, 4, 16}, c.Bytes())
		if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
			t.Fatal("expected error for truncated sub-chunk")
		}
	})

	t.Run("truncated sub-chunk body", func(t *testing.T) {
		var c bytes.Buffer
		c.Write([]byte("wavl"))
		var h bytes.Buffer
		h.WriteString("data")
		binary.Write(&h, binary.LittleEndian, uint32(8))
		h.Write([]byte{1, 2, 3})
		c.Write(h.Bytes())
		b := wavlRiff(WavFormat{AudioFormatPCM, 2, 8000, 32000, 4, 16}, c.Bytes())
		if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
			t.Fatal("expected error for truncated sub-chunk body")
		}
	})

	t.Run("slnt length not 4", func(t *testing.T) {
		var c bytes.Buffer
		c.Write(wavlChunk("slnt", []byte{1, 0}))
		b := stereo16Wavl(c.Bytes())
		if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
			t.Fatal("expected error for slnt length 2")
		}
	})

	t.Run("data not multiple of block align", func(t *testing.T) {
		var c bytes.Buffer
		c.Write(wavlData(1, 2, 3))
		b := stereo16Wavl(c.Bytes())
		if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
			t.Fatal("expected error for misaligned data chunk")
		}
	})

	t.Run("compressed format rejected", func(t *testing.T) {
		format := WavFormat{AudioFormatMULaw, 1, 8000, 8000, 1, 8}
		var c bytes.Buffer
		c.Write(wavlData(1))
		b := wavlRiff(format, c.Bytes())
		if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
			t.Fatal("expected error for compressed wavl")
		}
	})

	t.Run("float format rejected", func(t *testing.T) {
		format := WavFormat{AudioFormatIEEEFloat, 1, 8000, 16000, 2, 16}
		var c bytes.Buffer
		c.Write(wavlData(0, 0))
		b := wavlRiff(format, c.Bytes())
		if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
			t.Fatal("expected error for float wavl")
		}
	})

	t.Run("unsupported channel count", func(t *testing.T) {
		format := WavFormat{AudioFormatPCM, 3, 8000, 48000, 6, 16}
		var c bytes.Buffer
		c.Write(wavlData(0, 0, 0, 0, 0, 0))
		b := wavlRiff(format, c.Bytes())
		if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
			t.Fatal("expected error for 3-channel wavl")
		}
	})

	t.Run("expanded size overflows uint32", func(t *testing.T) {
		var c bytes.Buffer
		c.Write(wavlSlnt(uint32(0xFFFFFFFF/4 + 1)))
		b := stereo16Wavl(c.Bytes())
		if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
			t.Fatal("expected overflow error")
		}
	})
}

func TestWavlRootChunkRules(t *testing.T) {
	format := WavFormat{AudioFormatPCM, 2, 8000, 32000, 4, 16}
	var fb bytes.Buffer
	binary.Write(&fb, binary.LittleEndian, format)
	fmtChunk := wavlChunk("fmt ", fb.Bytes())
	dataChunk := wavlChunk("data", []byte{1, 0, 2, 0})
	var listBody bytes.Buffer
	listBody.Write([]byte("wavl"))
	listBody.Write(wavlSlnt(1))
	listChunk := wavlChunk("LIST", listBody.Bytes())

	wrap := func(chunks ...[]byte) []byte {
		var body bytes.Buffer
		body.Write([]byte("WAVE"))
		body.Write(fmtChunk)
		for _, ch := range chunks {
			body.Write(ch)
		}
		var w bytes.Buffer
		w.WriteString("RIFF")
		binary.Write(&w, binary.LittleEndian, uint32(body.Len()))
		w.Write(body.Bytes())
		return w.Bytes()
	}

	t.Run("data and wavl mixed", func(t *testing.T) {
		b := wrap(dataChunk, listChunk)
		if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
			t.Fatal("expected error when data and wavl LIST are mixed")
		}
	})

	t.Run("two data chunks", func(t *testing.T) {
		b := wrap(dataChunk, dataChunk)
		if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
			t.Fatal("expected error for duplicate data root")
		}
	})

	t.Run("two wavl lists", func(t *testing.T) {
		b := wrap(listChunk, listChunk)
		if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
			t.Fatal("expected error for duplicate wavl root")
		}
	})

	t.Run("non-wavl list is ignored", func(t *testing.T) {
		other := wavlChunk("LIST", append([]byte("INFO"), []byte("note")...))
		b := wrap(other, dataChunk)
		got := readAllWavlBytes(t, b)
		if !bytes.Equal(got, []byte{1, 0, 2, 0}) {
			t.Fatalf("bytes=%v", got)
		}
	})
}

func TestWavlInternallyTruncatedChild(t *testing.T) {
	var c bytes.Buffer
	c.Write([]byte("wavl"))
	var h bytes.Buffer
	h.WriteString("data")
	binary.Write(&h, binary.LittleEndian, uint32(8))
	h.Write([]byte{1, 2, 3, 4})
	c.Write(h.Bytes())
	b := wavlRiff(WavFormat{AudioFormatPCM, 2, 8000, 32000, 4, 16}, c.Bytes())
	if _, err := io.ReadAll(NewReader(bytes.NewReader(b))); err == nil {
		t.Fatal("expected error for a child truncated inside the wavl LIST")
	}
}

func TestWavlTopLevelChunkTruncated(t *testing.T) {
	var fb bytes.Buffer
	binary.Write(&fb, binary.LittleEndian, WavFormat{AudioFormatPCM, 2, 8000, 32000, 4, 16})
	var body bytes.Buffer
	body.Write([]byte("WAVE"))
	body.Write(wavlChunk("fmt ", fb.Bytes()))
	var h bytes.Buffer
	h.WriteString("data")
	binary.Write(&h, binary.LittleEndian, uint32(16))
	h.Write([]byte{1, 2, 3, 4})
	body.Write(h.Bytes())
	var w bytes.Buffer
	w.WriteString("RIFF")
	binary.Write(&w, binary.LittleEndian, uint32(body.Len()))
	w.Write(body.Bytes())
	if _, err := io.ReadAll(NewReader(bytes.NewReader(w.Bytes()))); err == nil {
		t.Fatal("expected error for a truncated top-level data chunk")
	}
}

func TestWavlStressBuffers(t *testing.T) {
	rng := rand.New(rand.NewSource(42))
	block := 4
	for iter := 0; iter < 200; iter++ {
		var c bytes.Buffer
		var expect bytes.Buffer
		var last []byte
		leading := true
		writeFrame := func(v uint16) {
			f := make([]byte, 4)
			binary.LittleEndian.PutUint16(f, v)
			binary.LittleEndian.PutUint16(f[2:], v+1)
			c.Write(wavlData(f...))
			expect.Write(f)
			last = append(last[:0], f...)
			leading = false
		}
		for k := 0; k < 6; k++ {
			switch rng.Intn(3) {
			case 0:
				writeFrame(uint16(iter*10 + k))
			case 1:
				n := uint32(rng.Intn(4))
				c.Write(wavlSlnt(n))
				for z := uint32(0); z < n; z++ {
					if leading {
						expect.Write(make([]byte, block))
					} else {
						expect.Write(last)
					}
				}
			case 2:
				c.Write(wavlData())
			}
		}
		b := stereo16Wavl(c.Bytes())
		r := NewReader(bytes.NewReader(b))
		size := 1 + rng.Intn(7)
		buf := make([]byte, size)
		var got []byte
		for {
			n, err := r.Read(buf)
			got = append(got, buf[:n]...)
			if err == io.EOF {
				break
			}
			if err != nil {
				t.Fatal(err)
			}
		}
		if !bytes.Equal(got, expect.Bytes()) {
			t.Fatalf("iter %d mismatch got=%v want=%v", iter, got, expect.Bytes())
		}
	}
}

func TestWavlErrorBeforeAnyAudio(t *testing.T) {
	var c bytes.Buffer
	c.Write(wavlData(1, 2, 3))
	b := stereo16Wavl(c.Bytes())
	r := NewReader(bytes.NewReader(b))
	buf := make([]byte, 64)
	n, err := r.Read(buf)
	if err == nil {
		t.Fatal("expected error for misaligned data")
	}
	if n != 0 {
		t.Fatalf("no audio may be delivered before validation, got %d bytes", n)
	}
}

func TestWavlDataSizeExcludesPadding(t *testing.T) {
	format := WavFormat{AudioFormatPCM, 1, 8000, 8000, 1, 8}
	var c bytes.Buffer
	c.Write(wavlData(11, 22, 33))
	c.Write(wavlSlnt(1))
	r := NewReader(bytes.NewReader(wavlRiff(format, c.Bytes())))
	if err := r.loadWavData(); err != nil {
		t.Fatal(err)
	}
	if r.WavData.Size != 4 {
		t.Fatalf("size=%d want 4 (no RIFF padding)", r.WavData.Size)
	}
	all, err := io.ReadAll(r)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(all, []byte{11, 22, 33, 33}) {
		t.Fatalf("bytes=%v", all)
	}
}
