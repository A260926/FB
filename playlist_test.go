package wav

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"reflect"
	"testing"

	"github.com/go-audio/audio"
)

// wavChunk is one RIFF sub chunk used by the test WAV builders.
type wavChunk struct {
	id   string
	body []byte
}

// cueSpec describes one cue point in the order it should be written.
type cueSpec struct {
	id           uint32
	position     uint32
	dataChunkID  string
	chunkStart   uint32
	blockStart   uint32
	sampleOffset uint32
}

// buildWAV assembles a little endian PCM RIFF/WAVE container from the
// given chunks in the exact order requested. Odd sized bodies get the
// mandatory padding byte.
func buildWAV(numChans uint16, bits uint16, rate uint32, chunks []wavChunk) []byte {
	var body bytes.Buffer
	put := func(v interface{}) {
		if err := binary.Write(&body, binary.LittleEndian, v); err != nil {
			panic(err)
		}
	}
	writeChunk := func(id string, b []byte) {
		body.WriteString(id)
		put(uint32(len(b)))
		body.Write(b)
		if len(b)%2 != 0 {
			body.WriteByte(0)
		}
	}

	fmtBody := new(bytes.Buffer)
	putF := func(v interface{}) {
		if err := binary.Write(fmtBody, binary.LittleEndian, v); err != nil {
			panic(err)
		}
	}
	putF(uint16(1)) // PCM
	putF(numChans)
	putF(rate)
	putF(rate * uint32(numChans) * uint32(bits) / 8)
	putF(numChans * bits / 8) // block align
	putF(bits)
	writeChunk("fmt ", fmtBody.Bytes())

	for _, c := range chunks {
		writeChunk(c.id, c.body)
	}

	var out bytes.Buffer
	out.WriteString("RIFF")
	if err := binary.Write(&out, binary.LittleEndian, uint32(body.Len()+4)); err != nil {
		panic(err)
	}
	out.WriteString("WAVE")
	out.Write(body.Bytes())
	return out.Bytes()
}

func cueChunkBody(cues []cueSpec) []byte {
	var b bytes.Buffer
	put := func(v interface{}) {
		if err := binary.Write(&b, binary.LittleEndian, v); err != nil {
			panic(err)
		}
	}
	put(uint32(len(cues)))
	for _, c := range cues {
		put(c.id)
		put(c.position)
		b.WriteString(fourCC(c.dataChunkID))
		put(c.chunkStart)
		put(c.blockStart)
		put(c.sampleOffset)
	}
	return b.Bytes()
}

// plstChunkBody builds a playlist payload: segment count followed by
// cue ID / frame length / play count triplets.
func plstChunkBody(segments [][3]uint32) []byte {
	var b bytes.Buffer
	put := func(v interface{}) {
		if err := binary.Write(&b, binary.LittleEndian, v); err != nil {
			panic(err)
		}
	}
	put(uint32(len(segments)))
	for _, s := range segments {
		put(s[0])
		put(s[1])
		put(s[2])
	}
	return b.Bytes()
}

func fourCC(s string) string {
	if len(s) != 4 {
		panic(fmt.Sprintf("fourCC needs 4 bytes, got %q", s))
	}
	return s
}

// pcm16 encodes mono or interleaved stereo 16 bit samples.
func pcm16(samples ...int) []byte {
	b := make([]byte, 2*len(samples))
	for i, s := range samples {
		binary.LittleEndian.PutUint16(b[i*2:], uint16(int16(s)))
	}
	return b
}

// readAllFrames pulls the whole logical stream using a buffer of
// frameSamples samples, which also exercises seeking across segment and
// repetition boundaries.
func readAllFrames(t *testing.T, d *Decoder, frameSamples int) []int {
	t.Helper()
	var out []int
	buf := &audio.IntBuffer{Data: make([]int, frameSamples)}
	for {
		n, err := d.PCMBuffer(buf)
		if err != nil && !errors.Is(err, io.EOF) {
			t.Fatalf("unexpected read error: %v", err)
		}
		if n == 0 {
			break
		}
		out = append(out, buf.Data[:n]...)
	}
	return out
}

// readFrames reads exactly n frames using a one-frame buffer.
func readFrames(t *testing.T, d *Decoder, n, numChans int) []int {
	t.Helper()
	var out []int
	buf := &audio.IntBuffer{Data: make([]int, numChans)}
	for i := 0; i < n; i++ {
		got, err := d.PCMBuffer(buf)
		if err != nil {
			t.Fatalf("frame %d: unexpected error: %v", i, err)
		}
		if got != numChans {
			t.Fatalf("frame %d: expected %d samples, got %d", i, numChans, got)
		}
		out = append(out, buf.Data...)
	}
	return out
}

func TestPlaylistPlayback(t *testing.T) {
	t.Run("fixture mono sequence", func(t *testing.T) {
		d := NewDecoder(bytes.NewReader(testPlaylistWAV()))
		d.UsePlaylist = true

		// one frame at a time: forces a fresh call per frame but only the
		// loop boundaries should trigger physical seeks
		got := readAllFrames(t, d, 1)
		want := []int{30, 40, 30, 40, 10}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("got %v, want %v", got, want)
		}
		if !d.EOF() {
			t.Fatal("expected EOF after consuming the playlist")
		}
		if d.PCMLen() != int64(len(want))*2 {
			t.Fatalf("unexpected logical PCM length: %d", d.PCMLen())
		}
		dur, err := d.Duration()
		if err != nil {
			t.Fatal(err)
		}
		if dur.Seconds() <= 0 {
			t.Fatalf("unexpected duration: %s", dur)
		}

		// rewinding replays the same logical stream
		if err := d.Rewind(); err != nil {
			t.Fatal(err)
		}
		if d.EOF() {
			t.Fatal("Rewind should clear EOF")
		}
		full, err := d.FullPCMBuffer()
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(full.Data, want) {
			t.Fatalf("after rewind got %v, want %v", full.Data, want)
		}
	})

	t.Run("stereo order with reversed cues", func(t *testing.T) {
		// frames: (10,11) (20,21) (30,31) (40,42)
		data := pcm16(10, 11, 20, 21, 30, 31, 40, 42)
		// cues are deliberately written in reverse ID order
		cues := []cueSpec{
			{id: 2, dataChunkID: "data", sampleOffset: 0},
			{id: 1, dataChunkID: "data", sampleOffset: 2},
		}
		// playlist references ID 1 first, proving pairing is by raw ID
		plst := plstChunkBody([][3]uint32{
			{1, 2, 1}, // frames 2..3
			{2, 1, 2}, // frame 0 twice
		})
		raw := buildWAV(2, 16, 8000, []wavChunk{
			{id: "cue ", body: cueChunkBody(cues)},
			{id: "plst", body: plst},
			{id: "data", body: data},
		})
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true
		got := readAllFrames(t, d, 2)
		want := []int{30, 31, 40, 42, 10, 11, 10, 11}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("got %v, want %v", got, want)
		}
	})

	t.Run("small buffer crossing segments", func(t *testing.T) {
		raw := testPlaylistWAV()
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true
		// a 3-sample buffer on mono data is frame aligned (1 channel)
		buf := &audio.IntBuffer{Data: make([]int, 3)}
		n, err := d.PCMBuffer(buf)
		if err != nil {
			t.Fatal(err)
		}
		if n != 3 || !reflect.DeepEqual(buf.Data, []int{30, 40, 30}) {
			t.Fatalf("first chunk got %v (n=%d)", buf.Data, n)
		}
		n, err = d.PCMBuffer(buf)
		if err != nil {
			t.Fatal(err)
		}
		if n != 2 || !reflect.DeepEqual(buf.Data[:2], []int{40, 10}) {
			t.Fatalf("tail chunk got %v (n=%d)", buf.Data[:n], n)
		}
	})

	t.Run("misaligned buffer does not move position", func(t *testing.T) {
		data := pcm16(10, 11, 20, 21, 30, 31, 40, 42)
		cues := []cueSpec{{id: 1, dataChunkID: "data", sampleOffset: 0}}
		plst := plstChunkBody([][3]uint32{{1, 4, 1}})
		raw := buildWAV(2, 16, 8000, []wavChunk{
			{id: "cue ", body: cueChunkBody(cues)},
			{id: "plst", body: plst},
			{id: "data", body: data},
		})
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true
		bad := &audio.IntBuffer{Data: make([]int, 3)} // not a multiple of 2
		if _, err := d.PCMBuffer(bad); !errors.Is(err, ErrBufferNotFrameAligned) {
			t.Fatalf("expected ErrBufferNotFrameAligned, got %v", err)
		}
		// position must be unchanged: first frame still available
		good := &audio.IntBuffer{Data: make([]int, 2)}
		n, err := d.PCMBuffer(good)
		if err != nil || n != 2 || !reflect.DeepEqual(good.Data, []int{10, 11}) {
			t.Fatalf("position moved after rejected read: %v n=%d err=%v", good.Data, n, err)
		}
	})

	t.Run("zero play count and empty table are empty streams", func(t *testing.T) {
		data := pcm16(1, 2, 3, 4)
		cues := []cueSpec{{id: 1, dataChunkID: "data", sampleOffset: 0}}

		zeroCount := buildWAV(1, 16, 8000, []wavChunk{
			{id: "cue ", body: cueChunkBody(cues)},
			{id: "plst", body: plstChunkBody([][3]uint32{{1, 4, 0}})},
			{id: "data", body: data},
		})
		d := NewDecoder(bytes.NewReader(zeroCount))
		d.UsePlaylist = true
		if got := readAllFrames(t, d, 1); len(got) != 0 {
			t.Fatalf("zero play count must produce no frames, got %v", got)
		}
		if !d.EOF() || d.PCMLen() != 0 {
			t.Fatalf("zero play count: EOF=%v len=%d", d.EOF(), d.PCMLen())
		}

		emptyTable := buildWAV(1, 16, 8000, []wavChunk{
			{id: "cue ", body: cueChunkBody(cues)},
			{id: "plst", body: plstChunkBody(nil)},
			{id: "data", body: data},
		})
		d2 := NewDecoder(bytes.NewReader(emptyTable))
		d2.UsePlaylist = true
		full, err := d2.FullPCMBuffer()
		if err != nil {
			t.Fatal(err)
		}
		if len(full.Data) != 0 {
			t.Fatalf("empty playlist must be an empty stream, got %v", full.Data)
		}
		if !d2.EOF() {
			t.Fatal("empty playlist should report EOF")
		}
	})

	t.Run("no playlist chunk plays raw data", func(t *testing.T) {
		data := pcm16(5, 6, 7)
		raw := buildWAV(1, 16, 8000, []wavChunk{
			{id: "JUNK", body: []byte{1, 2, 3}}, // odd size: padding skipped
			{id: "data", body: data},
		})
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true
		got := readAllFrames(t, d, 2)
		if !reflect.DeepEqual(got, []int{5, 6, 7}) {
			t.Fatalf("got %v", got)
		}
	})

	t.Run("trailing table and interleaved queries", func(t *testing.T) {
		// fmt, data first; cue and plst after the data
		raw := buildWAV(1, 16, 8000, []wavChunk{
			{id: "data", body: pcm16(10, 20, 30, 40)},
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 7, dataChunkID: "data", sampleOffset: 2},
				{id: 9, dataChunkID: "data", sampleOffset: 0},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{7, 2, 2}, {9, 1, 1}})},
		})
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true

		first := readFrames(t, d, 2, 1)
		if !reflect.DeepEqual(first, []int{30, 40}) {
			t.Fatalf("got %v", first)
		}
		// queries in the middle must not consume a frame or reset loops
		len1 := d.PCMLen()
		dur1, err := d.Duration()
		if err != nil {
			t.Fatal(err)
		}
		d.ReadMetadata()
		if err := d.Err(); err != nil {
			t.Fatalf("ReadMetadata failed: %v", err)
		}
		if d.PCMLen() != len1 {
			t.Fatalf("PCMLen changed across query: %d -> %d", len1, d.PCMLen())
		}
		if dur2, _ := d.Duration(); dur2 != dur1 {
			t.Fatalf("Duration changed across query: %s -> %s", dur1, dur2)
		}
		rest := readAllFrames(t, d, 1)
		if !reflect.DeepEqual(rest, []int{30, 40, 10}) {
			t.Fatalf("stream was disturbed by queries, got %v", rest)
		}
	})

	t.Run("zero length segment produces no sound", func(t *testing.T) {
		raw := buildWAV(1, 16, 8000, []wavChunk{
			// plst deliberately appears before the cue chunk it references
			{id: "plst", body: plstChunkBody([][3]uint32{
				{1, 0, 5}, // zero frames, even repeated: silent
				{2, 2, 1},
			})},
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 1, dataChunkID: "data", sampleOffset: 0},
				{id: 2, dataChunkID: "data", sampleOffset: 1},
			})},
			{id: "data", body: pcm16(10, 20, 30, 40)},
		})
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true
		if got := readAllFrames(t, d, 1); !reflect.DeepEqual(got, []int{20, 30}) {
			t.Fatalf("got %v", got)
		}
	})

	t.Run("same cue referenced multiple times", func(t *testing.T) {
		raw := buildWAV(1, 16, 8000, []wavChunk{
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 1, dataChunkID: "data", sampleOffset: 1},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{1, 2, 1}, {1, 1, 1}})},
			{id: "data", body: pcm16(10, 20, 30, 40)},
		})
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true
		got := readAllFrames(t, d, 1)
		if !reflect.DeepEqual(got, []int{20, 30, 20}) {
			t.Fatalf("got %v", got)
		}
	})

	t.Run("all in-scope bit depths", func(t *testing.T) {
		// one mono frame per physical sample; the playlist plays frames
		// 2..3 then frame 0 once.
		cases := []struct {
			bits     uint16
			physical []int
			want     []int
			encode   func(...int) []byte
		}{
			{8, []int{10, 20, 30, 40}, []int{30, 40, 10}, pcm8},
			{16, []int{10, 20, 30, 40}, []int{30, 40, 10}, pcm16},
			{24, []int{10, 20, 30, 40}, []int{30, 40, 10}, pcm24},
			{32, []int{-10, 20, -30, 40}, []int{-30, 40, -10}, pcm32},
		}
		for _, tc := range cases {
			raw := buildWAV(1, tc.bits, 8000, []wavChunk{
				{id: "cue ", body: cueChunkBody([]cueSpec{
					{id: 1, dataChunkID: "data", sampleOffset: 2},
					{id: 2, dataChunkID: "data", sampleOffset: 0},
				})},
				{id: "plst", body: plstChunkBody([][3]uint32{{1, 2, 1}, {2, 1, 1}})},
				{id: "data", body: tc.encode(tc.physical...)},
			})
			d := NewDecoder(bytes.NewReader(raw))
			d.UsePlaylist = true
			if got := readAllFrames(t, d, 1); !reflect.DeepEqual(got, tc.want) {
				t.Fatalf("%d bits: got %v, want %v", tc.bits, got, tc.want)
			}
		}
	})

	t.Run("partial last buffer then empty read", func(t *testing.T) {
		raw := buildWAV(1, 16, 8000, []wavChunk{
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 1, dataChunkID: "data", sampleOffset: 0},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{1, 4, 1}})},
			{id: "data", body: pcm16(10, 20, 30, 40)},
		})
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true
		buf := &audio.IntBuffer{Data: make([]int, 3)}
		n, err := d.PCMBuffer(buf)
		if err != nil || n != 3 {
			t.Fatalf("first read n=%d err=%v", n, err)
		}
		n, err = d.PCMBuffer(buf)
		if err != nil || n != 1 || buf.Data[0] != 40 {
			t.Fatalf("partial tail n=%d err=%v data=%v", n, err, buf.Data[:n])
		}
		n, err = d.PCMBuffer(buf)
		if err != nil || n != 0 || !d.EOF() {
			t.Fatalf("read after end n=%d err=%v EOF=%v", n, err, d.EOF())
		}
	})

	t.Run("FullPCMBuffer reads from the current position", func(t *testing.T) {
		d := NewDecoder(bytes.NewReader(testPlaylistWAV()))
		d.UsePlaylist = true
		readFrames(t, d, 2, 1) // consume 30, 40
		rest, err := d.FullPCMBuffer()
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(rest.Data, []int{30, 40, 10}) {
			t.Fatalf("got %v", rest.Data)
		}
	})

	t.Run("huge play count stays lazy", func(t *testing.T) {
		const count = uint32(2_000_000_000)
		raw := buildWAV(1, 16, 8000, []wavChunk{
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 1, dataChunkID: "data", sampleOffset: 0},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{1, 2, count}})},
			{id: "data", body: pcm16(1, 2)},
		})
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true

		totalFrames := int64(2) * int64(count)
		if d.PCMLen() != totalFrames*2 {
			t.Fatalf("unexpected expanded size: %d", d.PCMLen())
		}
		prefix := readFrames(t, d, 3, 1)
		if !reflect.DeepEqual(prefix, []int{1, 2, 1}) {
			t.Fatalf("prefix got %v", prefix)
		}
		// jump near the tail without materializing billions of frames
		if err := d.SeekPlaylistFrame(totalFrames - 2); err != nil {
			t.Fatal(err)
		}
		if d.EOF() {
			t.Fatal("two frames before the end must not report EOF yet")
		}
		tail := readFrames(t, d, 2, 1)
		if !reflect.DeepEqual(tail, []int{1, 2}) {
			t.Fatalf("tail got %v", tail)
		}
		if !d.EOF() {
			t.Fatal("consuming the last frames should reach EOF")
		}
		if n, _ := d.PCMBuffer(&audio.IntBuffer{Data: make([]int, 1)}); n != 0 || !d.EOF() {
			t.Fatal("expected an empty final read and EOF")
		}
	})
}

func TestPlaylistSeek(t *testing.T) {
	// logical stream: frames 2,3 then 0,1 then 2,3 -> 30 40 10 20 30 40
	raw := buildWAV(1, 16, 8000, []wavChunk{
		{id: "cue ", body: cueChunkBody([]cueSpec{
			{id: 1, dataChunkID: "data", sampleOffset: 2},
			{id: 2, dataChunkID: "data", sampleOffset: 0},
		})},
		{id: "plst", body: plstChunkBody([][3]uint32{{1, 2, 1}, {2, 2, 1}, {1, 2, 1}})},
		{id: "data", body: pcm16(10, 20, 30, 40)},
	})
	const total = int64(6)

	newDec := func() *Decoder {
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true
		return d
	}

	t.Run("seek across repeated segments", func(t *testing.T) {
		cases := []struct {
			frame int64
			want  []int
		}{
			{0, []int{30, 40, 10, 20, 30, 40}},
			{1, []int{40, 10, 20, 30, 40}},
			{2, []int{10, 20, 30, 40}},
			{3, []int{20, 30, 40}},
			{4, []int{30, 40}},
			{5, []int{40}},
		}
		for _, tc := range cases {
			d := newDec()
			if err := d.SeekPlaylistFrame(tc.frame); err != nil {
				t.Fatalf("frame %d: %v", tc.frame, err)
			}
			if got := readAllFrames(t, d, 1); !reflect.DeepEqual(got, tc.want) {
				t.Fatalf("frame %d: got %v, want %v", tc.frame, got, tc.want)
			}
		}
	})

	t.Run("out of range leaves position untouched", func(t *testing.T) {
		d := newDec()
		readFrames(t, d, 1, 1) // logical pos 1, sample 30 consumed
		if err := d.SeekPlaylistFrame(-1); err == nil {
			t.Fatal("expected error on negative seek")
		}
		if err := d.SeekPlaylistFrame(total + 1); err == nil {
			t.Fatal("expected error past the end")
		}
		got := readAllFrames(t, d, 1)
		if !reflect.DeepEqual(got, []int{40, 10, 20, 30, 40}) {
			t.Fatalf("position moved after rejected seek: %v", got)
		}
	})

	t.Run("seek to end reports EOF", func(t *testing.T) {
		d := newDec()
		if err := d.SeekPlaylistFrame(total); err != nil {
			t.Fatal(err)
		}
		if !d.EOF() {
			t.Fatal("seeking to total frame count should report EOF")
		}
		if n, err := d.PCMBuffer(&audio.IntBuffer{Data: make([]int, 2)}); n != 0 || err != nil {
			t.Fatalf("read at EOF: n=%d err=%v", n, err)
		}
		// move back into the stream and confirm the cursor works again
		if err := d.SeekPlaylistFrame(total - 1); err != nil {
			t.Fatal(err)
		}
		if d.EOF() {
			t.Fatal("seek back should clear EOF")
		}
		if got := readAllFrames(t, d, 1); !reflect.DeepEqual(got, []int{40}) {
			t.Fatalf("got %v", got)
		}
	})

	t.Run("raw seek rejected in playlist mode", func(t *testing.T) {
		d := newDec()
		if _, err := d.Seek(0, 0); !errors.Is(err, ErrRawSeekDisabled) {
			t.Fatalf("expected ErrRawSeekDisabled, got %v", err)
		}
	})

	t.Run("playlist seek rejected in default mode", func(t *testing.T) {
		d := NewDecoder(bytes.NewReader(raw))
		if err := d.SeekPlaylistFrame(0); !errors.Is(err, ErrSeekPlaylistDisabled) {
			t.Fatalf("expected ErrSeekPlaylistDisabled, got %v", err)
		}
		// default mode still reads the physical data unchanged
		full, err := d.FullPCMBuffer()
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(full.Data, []int{10, 20, 30, 40}) {
			t.Fatalf("default mode altered: %v", full.Data)
		}
	})

	t.Run("rewind returns to logical frame zero", func(t *testing.T) {
		d := newDec()
		readFrames(t, d, 4, 1)
		if err := d.Rewind(); err != nil {
			t.Fatal(err)
		}
		if got := readAllFrames(t, d, 1); !reflect.DeepEqual(got, []int{30, 40, 10, 20, 30, 40}) {
			t.Fatalf("got %v", got)
		}
	})

	t.Run("seek works when no playlist chunk is present", func(t *testing.T) {
		raw := buildWAV(1, 16, 8000, []wavChunk{
			{id: "data", body: pcm16(10, 20, 30, 40)},
		})
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true
		if err := d.SeekPlaylistFrame(2); err != nil {
			t.Fatal(err)
		}
		if got := readAllFrames(t, d, 1); !reflect.DeepEqual(got, []int{30, 40}) {
			t.Fatalf("got %v", got)
		}
		// empty stream (zero data) still accepts a zero seek
		empty := buildWAV(1, 16, 8000, []wavChunk{{id: "data", body: nil}})
		d2 := NewDecoder(bytes.NewReader(empty))
		d2.UsePlaylist = true
		if err := d2.SeekPlaylistFrame(0); err != nil {
			t.Fatalf("zero seek on empty stream: %v", err)
		}
		if !d2.EOF() {
			t.Fatal("empty stream should report EOF")
		}
	})
}

func TestPlaylistValidation(t *testing.T) {
	data := pcm16(10, 20, 30, 40)
	cues := func(extra ...cueSpec) []wavChunk {
		cs := append([]cueSpec{
			{id: 1, dataChunkID: "data", sampleOffset: 0},
		}, extra...)
		return []wavChunk{{id: "cue ", body: cueChunkBody(cs)}}
	}

	prepare := func(t *testing.T, chunks []wavChunk) error {
		t.Helper()
		raw := buildWAV(1, 16, 8000, chunks)
		d := NewDecoder(bytes.NewReader(raw))
		d.UsePlaylist = true
		_, err := d.PCMBuffer(&audio.IntBuffer{Data: make([]int, 1)})
		return err
	}

	t.Run("truncated table", func(t *testing.T) {
		body := plstChunkBody([][3]uint32{{1, 1, 1}, {1, 1, 1}})
		body = body[:len(body)-4] // drop the last triplet's final word
		chunks := append(cues(),
			wavChunk{id: "plst", body: body},
			wavChunk{id: "data", body: data},
		)
		if err := prepare(t, chunks); !errors.Is(err, ErrTruncatedPlaylist) {
			t.Fatalf("got %v", err)
		}

		// a payload shorter than the segment count itself
		chunks = append(cues(),
			wavChunk{id: "plst", body: []byte{1, 2}},
			wavChunk{id: "data", body: data},
		)
		if err := prepare(t, chunks); !errors.Is(err, ErrTruncatedPlaylist) {
			t.Fatalf("short payload got %v", err)
		}
	})

	t.Run("duplicate cue id", func(t *testing.T) {
		chunks := []wavChunk{
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 5, dataChunkID: "data", sampleOffset: 0},
				{id: 5, dataChunkID: "data", sampleOffset: 1},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{5, 1, 1}})},
			{id: "data", body: data},
		}
		if err := prepare(t, chunks); !errors.Is(err, ErrDuplicateCueID) {
			t.Fatalf("got %v", err)
		}
	})

	t.Run("missing reference", func(t *testing.T) {
		chunks := append(cues(),
			wavChunk{id: "plst", body: plstChunkBody([][3]uint32{{99, 1, 1}})},
			wavChunk{id: "data", body: data},
		)
		if err := prepare(t, chunks); !errors.Is(err, ErrMissingCueReference) {
			t.Fatalf("got %v", err)
		}
	})

	t.Run("cue outside single data chunk", func(t *testing.T) {
		// slnt data chunk id and non-zero starts are not acceptable locators
		chunks := []wavChunk{
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 1, dataChunkID: "slnt", chunkStart: 0, blockStart: 0, sampleOffset: 0},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{1, 1, 1}})},
			{id: "data", body: data},
		}
		if err := prepare(t, chunks); !errors.Is(err, ErrMissingCueReference) {
			t.Fatalf("slnt cue got %v", err)
		}

		chunks = []wavChunk{
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 1, dataChunkID: "data", chunkStart: 1, blockStart: 0, sampleOffset: 0},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{1, 1, 1}})},
			{id: "data", body: data},
		}
		if err := prepare(t, chunks); !errors.Is(err, ErrMissingCueReference) {
			t.Fatalf("non-zero ChunkStart got %v", err)
		}
	})

	t.Run("segment out of data", func(t *testing.T) {
		chunks := []wavChunk{
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 1, dataChunkID: "data", sampleOffset: 3},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{1, 2, 1}})},
			{id: "data", body: data},
		}
		if err := prepare(t, chunks); !errors.Is(err, ErrSegmentOutOfData) {
			t.Fatalf("got %v", err)
		}
	})

	t.Run("non whole frame data", func(t *testing.T) {
		chunks := append(cues(),
			wavChunk{id: "plst", body: plstChunkBody([][3]uint32{{1, 1, 1}})},
			wavChunk{id: "data", body: []byte{1, 2, 3}}, // 3 bytes, 2 bytes per frame
		)
		if err := prepare(t, chunks); !errors.Is(err, ErrMisalignedData) {
			t.Fatalf("got %v", err)
		}
	})

	t.Run("duplicate data and playlist chunks", func(t *testing.T) {
		dupData := append(cues(),
			wavChunk{id: "plst", body: plstChunkBody([][3]uint32{{1, 1, 1}})},
			wavChunk{id: "data", body: data},
			wavChunk{id: "data", body: data},
		)
		if err := prepare(t, dupData); !errors.Is(err, ErrDuplicateChunk) {
			t.Fatalf("duplicate data got %v", err)
		}

		dupPlst := append(cues(),
			wavChunk{id: "plst", body: plstChunkBody([][3]uint32{{1, 1, 1}})},
			wavChunk{id: "plst", body: plstChunkBody([][3]uint32{{1, 1, 1}})},
			wavChunk{id: "data", body: data},
		)
		if err := prepare(t, dupPlst); !errors.Is(err, ErrDuplicateChunk) {
			t.Fatalf("duplicate plst got %v", err)
		}
	})

	t.Run("errors surface before any sample", func(t *testing.T) {
		// valid first segment, invalid second one: the failure must be
		// reported on the very first read, not after segment one plays
		chunks := []wavChunk{
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 1, dataChunkID: "data", sampleOffset: 0},
				{id: 2, dataChunkID: "data", sampleOffset: 0},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{1, 1, 1}, {2, 99, 1}})},
			{id: "data", body: data},
		}
		d := NewDecoder(bytes.NewReader(buildWAV(1, 16, 8000, chunks)))
		d.UsePlaylist = true
		buf := &audio.IntBuffer{Data: make([]int, 1)}
		if n, err := d.PCMBuffer(buf); err == nil || n != 0 {
			t.Fatalf("first read must fail with no samples, got n=%d err=%v", n, err)
		}
	})

	t.Run("unsupported format is rejected", func(t *testing.T) {
		// three channels
		threeCh := buildWAV(3, 16, 8000, []wavChunk{
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 1, dataChunkID: "data", sampleOffset: 0},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{1, 1, 1}})},
			{id: "data", body: pcm16(1, 2, 3)},
		})
		d := NewDecoder(bytes.NewReader(threeCh))
		d.UsePlaylist = true
		if _, err := d.PCMBuffer(&audio.IntBuffer{Data: make([]int, 3)}); !errors.Is(err, ErrPlaylistFormatUnsupported) {
			t.Fatalf("three channels got %v", err)
		}

		// 12 bit is not in the supported 8/16/24/32 set
		twelveBit := buildWAV(1, 12, 8000, []wavChunk{
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 1, dataChunkID: "data", sampleOffset: 0},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{1, 1, 1}})},
			{id: "data", body: []byte{1}},
		})
		d2 := NewDecoder(bytes.NewReader(twelveBit))
		d2.UsePlaylist = true
		if _, err := d2.PCMBuffer(&audio.IntBuffer{Data: make([]int, 1)}); !errors.Is(err, ErrPlaylistFormatUnsupported) {
			t.Fatalf("12 bit got %v", err)
		}

		// non-PCM format tag: patch the two format-tag bytes of the
		// otherwise valid 16 bit container to 3 (IEEE float)
		nonPCM := buildWAV(1, 16, 8000, []wavChunk{
			{id: "cue ", body: cueChunkBody([]cueSpec{
				{id: 1, dataChunkID: "data", sampleOffset: 0},
			})},
			{id: "plst", body: plstChunkBody([][3]uint32{{1, 1, 1}})},
			{id: "data", body: data},
		})
		binary.LittleEndian.PutUint16(nonPCM[20:22], 3)
		d3 := NewDecoder(bytes.NewReader(nonPCM))
		d3.UsePlaylist = true
		if _, err := d3.PCMBuffer(&audio.IntBuffer{Data: make([]int, 1)}); !errors.Is(err, ErrPlaylistFormatUnsupported) {
			t.Fatalf("non-PCM got %v", err)
		}
	})
}

func pcm8(samples ...int) []byte {
	b := make([]byte, len(samples))
	for i, s := range samples {
		b[i] = byte(s)
	}
	return b
}

func pcm24(samples ...int) []byte {
	b := make([]byte, 3*len(samples))
	for i, s := range samples {
		u := uint32(int32(s))
		b[i*3+0] = byte(u)
		b[i*3+1] = byte(u >> 8)
		b[i*3+2] = byte(u >> 16)
	}
	return b
}

func pcm32(samples ...int) []byte {
	b := make([]byte, 4*len(samples))
	for i, s := range samples {
		binary.LittleEndian.PutUint32(b[i*4:], uint32(int32(s)))
	}
	return b
}

// ExampleDecoder_SeekPlaylistFrame shows following the "plst" playlist
// instead of reading the data chunk linearly, and positioning inside the
// logical stream with SeekPlaylistFrame.
func ExampleDecoder_SeekPlaylistFrame() {
	d := NewDecoder(bytes.NewReader(testPlaylistWAV()))
	// opt in before the first read; the zero value keeps the old behavior
	d.UsePlaylist = true
	d.ReadInfo()

	buf := &audio.IntBuffer{Data: make([]int, int(d.NumChans))}
	var played []int
	for {
		n, err := d.PCMBuffer(buf)
		if err == io.EOF || n == 0 {
			break
		}
		if err != nil {
			panic(err)
		}
		played = append(played, buf.Data[:n]...)
	}
	fmt.Println(played)

	// jump back to logical frame 3 and read the tail
	if err := d.SeekPlaylistFrame(3); err != nil {
		panic(err)
	}
	var tail []int
	for {
		n, err := d.PCMBuffer(buf)
		if err == io.EOF || n == 0 {
			break
		}
		if err != nil {
			panic(err)
		}
		tail = append(tail, buf.Data[:n]...)
	}
	fmt.Println(tail)
	// Output:
	// [30 40 30 40 10]
	// [40 10]
}
