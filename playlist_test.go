package wav

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"reflect"
	"testing"
	"time"

	"github.com/go-audio/audio"
	"github.com/go-audio/riff"
)

// testPlaylistWAV assembles a RIFF/WAVE container in memory. The chunk
// order and contents are chosen by the caller so the tests stay
// independent of any fixture file.
type testPlaylistWAV struct {
	bits  uint16
	chans uint16
	rate  uint32
}

func newTestPlaylistWAV(bits, chans uint16) *testPlaylistWAV {
	return &testPlaylistWAV{bits: bits, chans: chans, rate: 48000}
}

func riffChunk(id [4]byte, payload []byte) []byte {
	size := uint32(len(payload))
	out := make([]byte, 0, 8+len(payload)+1)
	out = append(out, id[:]...)
	var sizeBuf [4]byte
	binary.LittleEndian.PutUint32(sizeBuf[:], size)
	out = append(out, sizeBuf[:]...)
	out = append(out, payload...)
	if size%2 == 1 {
		// RIFF chunks are word aligned with a zero padding byte.
		out = append(out, 0)
	}
	return out
}

// build wraps the given top level chunks (fmt is added automatically)
// into a RIFF/WAVE container.
func (w *testPlaylistWAV) build(chunks ...[]byte) []byte {
	return w.buildRaw(append([][]byte{w.fmtChunk()}, chunks...)...)
}

// dataChunk encodes sample values as the configured integer PCM depth.
// Values are given per sample (interleaved for stereo).
func (w *testPlaylistWAV) dataChunk(samples ...int) []byte {
	bytesPerSample := w.bits / 8
	payload := make([]byte, 0, int(bytesPerSample)*len(samples))
	for _, s := range samples {
		switch w.bits {
		case 8:
			payload = append(payload, byte(s))
		case 16:
			var b [2]byte
			binary.LittleEndian.PutUint16(b[:], uint16(int16(s)))
			payload = append(payload, b[:]...)
		case 24:
			u := uint32(int32(s))
			payload = append(payload, byte(u), byte(u>>8), byte(u>>16))
		case 32:
			var b [4]byte
			binary.LittleEndian.PutUint32(b[:], uint32(int32(s)))
			payload = append(payload, b[:]...)
		}
	}
	return riffChunk(riff.DataFormatID, payload)
}

// rawDataChunk wraps an arbitrary byte payload as a data chunk, allowing
// misaligned sizes.
func rawDataChunk(payload []byte) []byte {
	return riffChunk(riff.DataFormatID, payload)
}

type testCue struct {
	id           uint32
	sampleOffset uint32
}

func cueChunk(cues ...testCue) []byte {
	payload := make([]byte, 4, 4+24*len(cues))
	binary.LittleEndian.PutUint32(payload, uint32(len(cues)))
	for _, c := range cues {
		buf := make([]byte, 24)
		binary.LittleEndian.PutUint32(buf[0:], c.id)
		binary.LittleEndian.PutUint32(buf[4:], 0) // Position is not used for addressing
		copy(buf[8:12], riff.DataFormatID[:])
		binary.LittleEndian.PutUint32(buf[12:], 0) // ChunkStart
		binary.LittleEndian.PutUint32(buf[16:], 0) // BlockStart
		binary.LittleEndian.PutUint32(buf[20:], c.sampleOffset)
		payload = append(payload, buf...)
	}
	return riffChunk(CIDCue, payload)
}

// rawCueChunk hands back the cue chunk payload verbatim, for truncation
// tests.
func rawCueChunk(payload []byte) []byte {
	return riffChunk(CIDCue, payload)
}

type testSegment struct {
	cueID     uint32
	length    uint32
	playCount uint32
}

func playlistChunk(segs ...testSegment) []byte {
	payload := make([]byte, 4, 4+12*len(segs))
	binary.LittleEndian.PutUint32(payload, uint32(len(segs)))
	for _, s := range segs {
		buf := make([]byte, 12)
		binary.LittleEndian.PutUint32(buf[0:], s.cueID)
		binary.LittleEndian.PutUint32(buf[4:], s.length)
		binary.LittleEndian.PutUint32(buf[8:], s.playCount)
		payload = append(payload, buf...)
	}
	return riffChunk(cidPlaylist, payload)
}

func rawPlaylistChunk(payload []byte) []byte {
	return riffChunk(cidPlaylist, payload)
}

// buildRaw wraps arbitrary top level chunks (including fmt) into a
// RIFF/WAVE container, allowing chunks to sit before the fmt chunk.
func (w *testPlaylistWAV) buildRaw(chunks ...[]byte) []byte {
	body := []byte{}
	for _, c := range chunks {
		body = append(body, c...)
	}
	out := []byte{'R', 'I', 'F', 'F'}
	var sizeBuf [4]byte
	binary.LittleEndian.PutUint32(sizeBuf[:], uint32(4+len(body)))
	out = append(out, sizeBuf[:]...)
	out = append(out, 'W', 'A', 'V', 'E')
	out = append(out, body...)
	return out
}

// fmtChunk returns the standalone fmt chunk for this format.
func (w *testPlaylistWAV) fmtChunk() []byte {
	fmtPayload := make([]byte, 16)
	binary.LittleEndian.PutUint16(fmtPayload[0:], 1) // PCM
	binary.LittleEndian.PutUint16(fmtPayload[2:], w.chans)
	binary.LittleEndian.PutUint32(fmtPayload[4:], w.rate)
	blockAlign := w.chans * w.bits / 8
	binary.LittleEndian.PutUint32(fmtPayload[8:], w.rate*uint32(blockAlign))
	binary.LittleEndian.PutUint16(fmtPayload[12:], blockAlign)
	binary.LittleEndian.PutUint16(fmtPayload[14:], w.bits)
	return riffChunk(riff.FmtID, fmtPayload)
}

func junkChunk(payload []byte) []byte {
	return riffChunk([4]byte{'J', 'U', 'N', 'K'}, payload)
}

func playlistDecoder(t *testing.T, raw []byte) *Decoder {
	t.Helper()
	d := NewDecoder(bytes.NewReader(raw))
	d.UsePlaylist = true
	return d
}

// TestBaseline keeps the historical default behavior: even with a cue and
// playlist present, the default decoder reads the four raw frames in
// order.
func TestBaseline(t *testing.T) {
	w := newTestPlaylistWAV(16, 1)
	raw := w.build(
		w.dataChunk(10, 20, 30, 40),
		cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0}),
		playlistChunk(
			testSegment{cueID: 1, length: 2, playCount: 2},
			testSegment{cueID: 2, length: 1, playCount: 1},
		),
	)
	d := NewDecoder(bytes.NewReader(raw))
	got := readAll(t, d, 3)
	want := []int{10, 20, 30, 40}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("baseline default mode expected %v, got %v", want, got)
	}
}

// readAll drains the decoder using the given sample buffer capacity and
// returns every decoded sample.
func readAll(t *testing.T, d *Decoder, bufCap int) []int {
	t.Helper()
	out := []int{}
	buf := &audio.IntBuffer{Data: make([]int, bufCap)}
	for {
		n, err := d.PCMBuffer(buf)
		if err != nil {
			t.Fatalf("PCMBuffer failed: %v", err)
		}
		if n == 0 {
			break
		}
		out = append(out, buf.Data[:n]...)
	}
	return out
}

func TestPlaylistPlayback(t *testing.T) {
	// The canonical four frame mono source: 10, 20, 30, 40.
	// The playlist plays frames [2,4) twice then frame 0 once, giving
	// 30, 40, 30, 40, 10.
	canonical := func(t *testing.T, bits uint16) {
		w := newTestPlaylistWAV(bits, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0}),
			playlistChunk(
				testSegment{cueID: 1, length: 2, playCount: 2},
				testSegment{cueID: 2, length: 1, playCount: 1},
			),
		)
		got := readAll(t, playlistDecoder(t, raw), 3)
		want := []int{30, 40, 30, 40, 10}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("%d-bit: expected %v, got %v", bits, want, got)
		}
	}
	t.Run("8bit", func(t *testing.T) { canonical(t, 8) })
	t.Run("16bit", func(t *testing.T) { canonical(t, 16) })
	t.Run("24bit", func(t *testing.T) { canonical(t, 24) })
	t.Run("32bit", func(t *testing.T) { canonical(t, 32) })

	t.Run("stereo frame ordering", func(t *testing.T) {
		// Four stereo frames; a frame holds both channels and must not
		// be split into two positions.
		w := newTestPlaylistWAV(16, 2)
		raw := w.build(
			w.dataChunk(1, 2, 3, 4, 5, 6, 7, 8),
			cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0}),
			playlistChunk(
				testSegment{cueID: 1, length: 2, playCount: 2},
				testSegment{cueID: 2, length: 1, playCount: 1},
			),
		)
		got := readAll(t, playlistDecoder(t, raw), 4)
		want := []int{5, 6, 7, 8, 5, 6, 7, 8, 1, 2}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("expected %v, got %v", want, got)
		}
	})

	t.Run("cue IDs matched by raw bytes not table order", func(t *testing.T) {
		// The cue table lists the offset-0 cue first with a high,
		// non-sequential ID; the playlist still references IDs directly.
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 0x00000007, sampleOffset: 0}, testCue{id: 0x00000003, sampleOffset: 2}),
			playlistChunk(
				testSegment{cueID: 0x00000003, length: 2, playCount: 2},
				testSegment{cueID: 0x00000007, length: 1, playCount: 1},
			),
		)
		got := readAll(t, playlistDecoder(t, raw), 2)
		want := []int{30, 40, 30, 40, 10}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("expected %v, got %v", want, got)
		}
	})

	t.Run("same cue referenced repeatedly and small crossing buffers", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 2}),
			playlistChunk(
				testSegment{cueID: 1, length: 2, playCount: 3},
				testSegment{cueID: 1, length: 1, playCount: 2},
			),
		)
		// A one sample buffer forces every segment/repeat boundary to
		// be crossed mid-buffer.
		got := readAll(t, playlistDecoder(t, raw), 1)
		want := []int{30, 40, 30, 40, 30, 40, 30, 30}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("expected %v, got %v", want, got)
		}
	})

	t.Run("zero play count and zero length produce silence", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0}),
			playlistChunk(
				testSegment{cueID: 1, length: 2, playCount: 0},
				testSegment{cueID: 2, length: 0, playCount: 5},
				testSegment{cueID: 2, length: 1, playCount: 1},
			),
		)
		got := readAll(t, playlistDecoder(t, raw), 4)
		want := []int{10}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("expected %v, got %v", want, got)
		}
	})

	t.Run("empty playlist is an empty stream", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 0}),
			playlistChunk(),
		)
		d := playlistDecoder(t, raw)
		got := readAll(t, d, 4)
		if len(got) != 0 {
			t.Fatalf("expected no samples, got %v", got)
		}
		if !d.EOF() {
			t.Fatal("expected EOF on an empty playlist")
		}
		if d.PCMLen() != 0 {
			t.Fatalf("expected zero PCM length, got %d", d.PCMLen())
		}
		dur, err := d.Duration()
		if err != nil || dur != 0 {
			t.Fatalf("expected zero duration, got %s err %v", dur, err)
		}
	})

	t.Run("no playlist plays the raw data in playlist mode", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 0}),
		)
		got := readAll(t, playlistDecoder(t, raw), 3)
		want := []int{10, 20, 30, 40}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("expected %v, got %v", want, got)
		}
	})

	t.Run("default mode still reads raw data", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 2}),
			playlistChunk(testSegment{cueID: 1, length: 2, playCount: 2}),
		)
		d := NewDecoder(bytes.NewReader(raw))
		got := readAll(t, d, 3)
		want := []int{10, 20, 30, 40}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("expected %v, got %v", want, got)
		}
	})

	t.Run("EOF positioning and partial final buffer", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0}),
			playlistChunk(
				testSegment{cueID: 1, length: 2, playCount: 2},
				testSegment{cueID: 2, length: 1, playCount: 1},
			),
		)
		d := playlistDecoder(t, raw)
		if d.EOF() {
			t.Fatal("should not report EOF before any read")
		}
		buf := &audio.IntBuffer{Data: make([]int, 4)}
		n, err := d.PCMBuffer(buf)
		if err != nil || n != 4 {
			t.Fatalf("expected 4 samples, got %d (err %v)", n, err)
		}
		if d.EOF() {
			t.Fatal("should not report EOF with one frame left")
		}
		n, err = d.PCMBuffer(buf)
		if err != nil || n != 1 || buf.Data[0] != 10 {
			t.Fatalf("expected final sample 10, got n=%d err=%v data=%v", n, err, buf.Data[:n])
		}
		if !d.EOF() {
			t.Fatal("expected EOF after the final frame")
		}
		n, err = d.PCMBuffer(buf)
		if err != nil || n != 0 {
			t.Fatalf("expected an empty read at EOF, got n=%d err=%v", n, err)
		}
	})

	t.Run("queries do not consume frames or reset loops", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0}),
			playlistChunk(
				testSegment{cueID: 1, length: 2, playCount: 2},
				testSegment{cueID: 2, length: 1, playCount: 1},
			),
		)
		d := playlistDecoder(t, raw)
		buf := &audio.IntBuffer{Data: make([]int, 2)}
		if n, err := d.PCMBuffer(buf); err != nil || n != 2 {
			t.Fatalf("expected 2 samples, got %d err %v", n, err)
		}
		dur, err := d.Duration()
		if err != nil {
			t.Fatal(err)
		}
		if dur != 5*time.Second/time.Duration(w.rate) {
			t.Fatalf("unexpected duration %s", dur)
		}
		if d.PCMLen() != int64(5)*int64(w.bits/8) {
			t.Fatalf("unexpected PCM length %d", d.PCMLen())
		}
		d.ReadMetadata()
		if err := d.Err(); err != nil {
			t.Fatal(err)
		}
		// Playback resumes exactly where it stopped, loop counters intact.
		got := readAll(t, d, 2)
		want := []int{30, 40, 10}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("expected %v after queries, got %v", want, got)
		}
	})

	t.Run("trailing playlist and odd padded unknown chunk", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			junkChunk([]byte{1, 2, 3}), // odd size, exercises padding skip
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 2}),
			playlistChunk(testSegment{cueID: 1, length: 2, playCount: 1}),
		)
		got := readAll(t, playlistDecoder(t, raw), 2)
		want := []int{30, 40}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("expected %v, got %v", want, got)
		}
	})

	t.Run("chunks before fmt and after data", func(t *testing.T) {
		// Hand-built ordering: cue, fmt, data, plst (plst trailing).
		w := newTestPlaylistWAV(16, 1)
		cue := cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0})
		plst := playlistChunk(
			testSegment{cueID: 1, length: 2, playCount: 2},
			testSegment{cueID: 2, length: 1, playCount: 1},
		)
		raw := w.build(cue, w.dataChunk(10, 20, 30, 40), plst)
		got := readAll(t, playlistDecoder(t, raw), 10)
		want := []int{30, 40, 30, 40, 10}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("expected %v, got %v", want, got)
		}
	})

	t.Run("cue physically before fmt", func(t *testing.T) {
		// readHeaders rewinds when non-fmt chunks lead the container;
		// make sure the playlist scan still starts at the first chunk.
		w := newTestPlaylistWAV(16, 1)
		raw := w.buildRaw(
			junkChunk([]byte{9, 8, 7, 6}),
			cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0}),
			w.fmtChunk(),
			w.dataChunk(10, 20, 30, 40),
			playlistChunk(
				testSegment{cueID: 1, length: 2, playCount: 2},
				testSegment{cueID: 2, length: 1, playCount: 1},
			),
		)
		got := readAll(t, playlistDecoder(t, raw), 3)
		want := []int{30, 40, 30, 40, 10}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("expected %v, got %v", want, got)
		}
	})

	t.Run("playlist and cue before the data chunk", func(t *testing.T) {
		// Every ordering of fmt, cue, plst and data must work.
		w := newTestPlaylistWAV(16, 1)
		raw := w.buildRaw(
			w.fmtChunk(),
			cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0}),
			playlistChunk(
				testSegment{cueID: 1, length: 2, playCount: 2},
				testSegment{cueID: 2, length: 1, playCount: 1},
			),
			w.dataChunk(10, 20, 30, 40),
		)
		got := readAll(t, playlistDecoder(t, raw), 3)
		want := []int{30, 40, 30, 40, 10}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("expected %v, got %v", want, got)
		}
	})

	t.Run("rewind restarts the logical stream", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0}),
			playlistChunk(
				testSegment{cueID: 1, length: 2, playCount: 2},
				testSegment{cueID: 2, length: 1, playCount: 1},
			),
		)
		d := playlistDecoder(t, raw)
		first := readAll(t, d, 2)
		if err := d.Rewind(); err != nil {
			t.Fatal(err)
		}
		second := readAll(t, d, 2)
		if !reflect.DeepEqual(first, second) {
			t.Fatalf("rewind mismatch: %v vs %v", first, second)
		}
	})

	t.Run("FullPCMBuffer reads the remainder", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0}),
			playlistChunk(
				testSegment{cueID: 1, length: 2, playCount: 2},
				testSegment{cueID: 2, length: 1, playCount: 1},
			),
		)
		d := playlistDecoder(t, raw)
		head := &audio.IntBuffer{Data: make([]int, 2)}
		if n, err := d.PCMBuffer(head); err != nil || n != 2 {
			t.Fatalf("expected 2 samples, got %d err %v", n, err)
		}
		rest, err := d.FullPCMBuffer()
		if err != nil {
			t.Fatal(err)
		}
		want := []int{30, 40, 10}
		if !reflect.DeepEqual(rest.Data, want) {
			t.Fatalf("expected %v, got %v", want, rest.Data)
		}
	})

	t.Run("invalid buffer length leaves position untouched", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 2)
		raw := w.build(
			w.dataChunk(1, 2, 3, 4, 5, 6, 7, 8),
			cueChunk(testCue{id: 1, sampleOffset: 0}),
			playlistChunk(testSegment{cueID: 1, length: 4, playCount: 1}),
		)
		d := playlistDecoder(t, raw)
		bad := &audio.IntBuffer{Data: make([]int, 3)} // not a multiple of 2
		if _, err := d.PCMBuffer(bad); err == nil {
			t.Fatal("expected an error for a misaligned stereo buffer")
		}
		good := &audio.IntBuffer{Data: make([]int, 2)}
		n, err := d.PCMBuffer(good)
		if err != nil || n != 2 || good.Data[0] != 1 || good.Data[1] != 2 {
			t.Fatalf("position moved after invalid buffer: n=%d err=%v data=%v", n, err, good.Data[:n])
		}
	})

	t.Run("raw byte seek is rejected in playlist mode", func(t *testing.T) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(w.dataChunk(10, 20, 30, 40))
		d := playlistDecoder(t, raw)
		if _, err := d.Seek(0, io.SeekStart); !errors.Is(err, errPlaylistRawSeek) {
			t.Fatalf("expected errPlaylistRawSeek, got %v", err)
		}
	})
}

func TestPlaylistSeek(t *testing.T) {
	buildCanonical := func() ([]byte, *testPlaylistWAV) {
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 2}, testCue{id: 2, sampleOffset: 0}),
			playlistChunk(
				testSegment{cueID: 1, length: 2, playCount: 2},
				testSegment{cueID: 2, length: 1, playCount: 1},
			),
		)
		return raw, w
	}
	wantSeq := []int{30, 40, 30, 40, 10}

	t.Run("seek across repeats and segment boundaries", func(t *testing.T) {
		raw, _ := buildCanonical()
		d := playlistDecoder(t, raw)
		positions := []int{0, 1, 2, 3, 4}
		for _, frame := range positions {
			if err := d.SeekPlaylistFrame(int64(frame)); err != nil {
				t.Fatalf("seek to %d failed: %v", frame, err)
			}
			buf := &audio.IntBuffer{Data: make([]int, 1)}
			n, err := d.PCMBuffer(buf)
			if err != nil || n != 1 || buf.Data[0] != wantSeq[frame] {
				t.Fatalf("frame %d: expected %d, got n=%d v=%d err=%v", frame, wantSeq[frame], n, buf.Data[0], err)
			}
		}
	})

	t.Run("seek to total frames lands at EOF", func(t *testing.T) {
		raw, _ := buildCanonical()
		d := playlistDecoder(t, raw)
		if err := d.SeekPlaylistFrame(5); err != nil {
			t.Fatal(err)
		}
		if !d.EOF() {
			t.Fatal("expected EOF after seeking to the total frame count")
		}
		buf := &audio.IntBuffer{Data: make([]int, 2)}
		if n, err := d.PCMBuffer(buf); err != nil || n != 0 {
			t.Fatalf("expected empty read, got n=%d err=%v", n, err)
		}
		// Seeking back inside the stream clears the EOF state.
		if err := d.SeekPlaylistFrame(0); err != nil {
			t.Fatal(err)
		}
		if d.EOF() {
			t.Fatal("expected EOF to clear after seeking back")
		}
		if got := readAll(t, d, 2); !reflect.DeepEqual(got, wantSeq) {
			t.Fatalf("expected %v after seek back, got %v", wantSeq, got)
		}
	})

	t.Run("out of range seek leaves position untouched", func(t *testing.T) {
		raw, _ := buildCanonical()
		d := playlistDecoder(t, raw)
		if err := d.SeekPlaylistFrame(3); err != nil {
			t.Fatal(err)
		}
		for _, frame := range []int64{-1, 6, 1 << 40} {
			if err := d.SeekPlaylistFrame(frame); err == nil {
				t.Fatalf("expected an out of range error for %d", frame)
			}
		}
		buf := &audio.IntBuffer{Data: make([]int, 1)}
		n, err := d.PCMBuffer(buf)
		if err != nil || n != 1 || buf.Data[0] != wantSeq[3] {
			t.Fatalf("position changed after failed seeks: n=%d v=%d err=%v", n, buf.Data[0], err)
		}
	})

	t.Run("unavailable in default mode", func(t *testing.T) {
		raw, _ := buildCanonical()
		d := NewDecoder(bytes.NewReader(raw))
		if err := d.SeekPlaylistFrame(0); !errors.Is(err, errPlaylistSeekDefault) {
			t.Fatalf("expected errPlaylistSeekDefault, got %v", err)
		}
	})

	t.Run("huge play count stays lazy", func(t *testing.T) {
		const huge uint32 = 3_000_000_000
		w := newTestPlaylistWAV(16, 1)
		raw := w.build(
			w.dataChunk(99),
			cueChunk(testCue{id: 1, sampleOffset: 0}),
			playlistChunk(testSegment{cueID: 1, length: 1, playCount: huge}),
		)
		d := playlistDecoder(t, raw)
		if err := d.ensurePlaylistPrepared(); err != nil {
			t.Fatal(err)
		}
		if d.pl.totalFrames != int64(huge) {
			t.Fatalf("expected %d logical frames, got %d", huge, d.pl.totalFrames)
		}
		// Read only a short prefix.
		buf := &audio.IntBuffer{Data: make([]int, 3)}
		n, err := d.PCMBuffer(buf)
		if err != nil || n != 3 {
			t.Fatalf("expected 3 prefix samples, got n=%d err=%v", n, err)
		}
		for _, v := range buf.Data[:3] {
			if v != 99 {
				t.Fatalf("unexpected prefix value %d", v)
			}
		}
		// Jump near the tail without materializing the stream.
		nearTail := int64(huge) - 2
		if err := d.SeekPlaylistFrame(nearTail); err != nil {
			t.Fatal(err)
		}
		tail := readAll(t, d, 2)
		if !reflect.DeepEqual(tail, []int{99, 99}) {
			t.Fatalf("unexpected tail %v", tail)
		}
		if !d.EOF() {
			t.Fatal("expected EOF at the end of the huge playlist")
		}
	})

	t.Run("rewind after seek returns to frame zero", func(t *testing.T) {
		raw, _ := buildCanonical()
		d := playlistDecoder(t, raw)
		if err := d.SeekPlaylistFrame(4); err != nil {
			t.Fatal(err)
		}
		if err := d.Rewind(); err != nil {
			t.Fatal(err)
		}
		if got := readAll(t, d, 3); !reflect.DeepEqual(got, wantSeq) {
			t.Fatalf("expected %v after rewind, got %v", wantSeq, got)
		}
	})
}

func TestPlaylistValidationErrors(t *testing.T) {
	w := newTestPlaylistWAV(16, 1)

	t.Run("truncated playlist count", func(t *testing.T) {
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 0}),
			rawPlaylistChunk([]byte{1, 2}),
		)
		if err := playlistDecoder(t, raw).ensurePlaylistPrepared(); !errors.Is(err, errPlaylistTruncated) {
			t.Fatalf("expected truncation error, got %v", err)
		}
	})

	t.Run("truncated playlist segment", func(t *testing.T) {
		payload := make([]byte, 8) // count + half a segment
		binary.LittleEndian.PutUint32(payload, 1)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 0}),
			rawPlaylistChunk(payload),
		)
		if err := playlistDecoder(t, raw).ensurePlaylistPrepared(); !errors.Is(err, errPlaylistTruncated) {
			t.Fatalf("expected truncation error, got %v", err)
		}
	})

	t.Run("truncated cue table", func(t *testing.T) {
		payload := make([]byte, 8)
		binary.LittleEndian.PutUint32(payload, 2) // claims two cues, none follow
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			rawCueChunk(payload),
			playlistChunk(testSegment{cueID: 1, length: 1, playCount: 1}),
		)
		if err := playlistDecoder(t, raw).ensurePlaylistPrepared(); !errors.Is(err, errPlaylistTruncated) {
			t.Fatalf("expected truncation error, got %v", err)
		}
	})

	t.Run("duplicate cue ID", func(t *testing.T) {
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 0}, testCue{id: 1, sampleOffset: 2}),
			playlistChunk(testSegment{cueID: 1, length: 1, playCount: 1}),
		)
		if err := playlistDecoder(t, raw).ensurePlaylistPrepared(); !errors.Is(err, errPlaylistDupCueID) {
			t.Fatalf("expected duplicate cue error, got %v", err)
		}
	})

	t.Run("missing cue reference", func(t *testing.T) {
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 0}),
			playlistChunk(testSegment{cueID: 99, length: 1, playCount: 1}),
		)
		if err := playlistDecoder(t, raw).ensurePlaylistPrepared(); !errors.Is(err, errPlaylistMissingCue) {
			t.Fatalf("expected missing cue error, got %v", err)
		}
	})

	t.Run("segment runs past data", func(t *testing.T) {
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 2}),
			playlistChunk(testSegment{cueID: 1, length: 3, playCount: 1}),
		)
		if err := playlistDecoder(t, raw).ensurePlaylistPrepared(); !errors.Is(err, errPlaylistOutOfData) {
			t.Fatalf("expected out of data error, got %v", err)
		}
	})

	t.Run("data is not whole frames", func(t *testing.T) {
		raw := w.build(
			rawDataChunk([]byte{1, 2, 3}), // 3 bytes, 2 bytes per frame
			cueChunk(testCue{id: 1, sampleOffset: 0}),
			playlistChunk(testSegment{cueID: 1, length: 1, playCount: 1}),
		)
		if err := playlistDecoder(t, raw).ensurePlaylistPrepared(); !errors.Is(err, errPlaylistUnaligned) {
			t.Fatalf("expected alignment error, got %v", err)
		}
	})

	t.Run("duplicate data chunks", func(t *testing.T) {
		raw := w.build(
			w.dataChunk(10, 20),
			w.dataChunk(30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 0}),
			playlistChunk(testSegment{cueID: 1, length: 1, playCount: 1}),
		)
		if err := playlistDecoder(t, raw).ensurePlaylistPrepared(); !errors.Is(err, errPlaylistDupData) {
			t.Fatalf("expected duplicate data error, got %v", err)
		}
	})

	t.Run("duplicate playlist chunks", func(t *testing.T) {
		plst := playlistChunk(testSegment{cueID: 1, length: 1, playCount: 1})
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 0}),
			plst,
			plst,
		)
		if err := playlistDecoder(t, raw).ensurePlaylistPrepared(); !errors.Is(err, errPlaylistDupPlst) {
			t.Fatalf("expected duplicate playlist error, got %v", err)
		}
	})

	t.Run("cue outside the single data chunk", func(t *testing.T) {
		payload := make([]byte, 28) // count (4) + one full cue (24)
		binary.LittleEndian.PutUint32(payload[0:], 1)
		copy(payload[8:12], riff.DataFormatID[:])
		binary.LittleEndian.PutUint32(payload[12:], 4) // nonzero ChunkStart
		binary.LittleEndian.PutUint32(payload[20:], 0)
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			rawCueChunk(payload),
			playlistChunk(testSegment{cueID: 1, length: 1, playCount: 1}),
		)
		if err := playlistDecoder(t, raw).ensurePlaylistPrepared(); !errors.Is(err, errPlaylistBadCue) {
			t.Fatalf("expected bad cue error, got %v", err)
		}
	})

	t.Run("validation happens before any sample is returned", func(t *testing.T) {
		// A first segment that plays fine, followed by a broken one.
		raw := w.build(
			w.dataChunk(10, 20, 30, 40),
			cueChunk(testCue{id: 1, sampleOffset: 0}, testCue{id: 2, sampleOffset: 2}),
			playlistChunk(
				testSegment{cueID: 1, length: 1, playCount: 1},
				testSegment{cueID: 2, length: 99, playCount: 1},
			),
		)
		d := playlistDecoder(t, raw)
		buf := &audio.IntBuffer{Data: make([]int, 1)}
		if _, err := d.PCMBuffer(buf); err == nil {
			t.Fatal("expected the whole playlist, including the later bad segment, to be rejected up front")
		}
		if n, _ := d.PCMBuffer(buf); n != 0 {
			t.Fatal("no sample should ever be handed out after failed validation")
		}
	})
}
