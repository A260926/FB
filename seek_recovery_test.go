package oggvorbis_test

// Regression tests for seek (SetPosition) recovery.
//
// What is preserved when a seek fails:
// A failed SetPosition is atomic. It returns the underlying Seek/Read error
// (wrapped, but recognizable via errors.Is) and leaves every piece of
// visible playback state untouched: Position() and Length() keep their
// previous values, any decoded-but-unplayed PCM in the buffer is still
// there, and samples still pending from an earlier successful seek remain
// pending. The vorbis decoder state is not touched either, so the PCM read
// after the failure is bit-identical to a reader that never attempted the
// seek, the physical cursor of the underlying source is restored, and the
// same seek can simply be retried.

import (
	"encoding/binary"
	"errors"
	"io"
	"os"
	"testing"

	"github.com/jfreymuth/oggvorbis"
)

var (
	errSeekFault = errors.New("one-shot seek fault")
	errReadFault = errors.New("one-shot read fault")
)

// faultSource wraps a file with deterministic fault injection. The
// failSeekAt-th Seek call (1-based) returns errSeekFault without moving the
// physical cursor, and the failReadAt-th Read call returns (0, errReadFault);
// the source works normally again afterwards. With shortReads enabled every
// Read returns at most 1 to 7 bytes, cycling deterministically.
type faultSource struct {
	f          *os.File
	shortReads bool

	seekCalls  int
	readCalls  int
	failSeekAt int // 1-based call index to fail, 0 disables
	failReadAt int // 1-based call index to fail, 0 disables
}

func (s *faultSource) Seek(off int64, whence int) (int64, error) {
	s.seekCalls++
	if s.failSeekAt != 0 && s.seekCalls == s.failSeekAt {
		return 0, errSeekFault
	}
	return s.f.Seek(off, whence)
}

func (s *faultSource) Read(p []byte) (int, error) {
	s.readCalls++
	if s.failReadAt != 0 && s.readCalls == s.failReadAt {
		return 0, errReadFault
	}
	if s.shortReads {
		if max := 1 + (s.readCalls-1)%7; len(p) > max {
			p = p[:max]
		}
	}
	return s.f.Read(p)
}

func (s *faultSource) resetCounters() {
	s.seekCalls = 0
	s.readCalls = 0
}

func (s *faultSource) disarm() {
	s.failSeekAt = 0
	s.failReadAt = 0
}

func newFaultReader(t *testing.T, path string, shortReads bool) (*oggvorbis.Reader, *faultSource) {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { f.Close() })
	src := &faultSource{f: f, shortReads: shortReads}
	r, err := oggvorbis.NewReader(src)
	if err != nil {
		t.Fatal(err)
	}
	return r, src
}

// readFull fills buf using a bounded number of Read calls.
func readFull(t *testing.T, r *oggvorbis.Reader, buf []float32, context string) {
	t.Helper()
	n := 0
	for calls := 0; n < len(buf); calls++ {
		if calls > len(buf)+1024 {
			t.Fatalf("%s: no read progress after %d Read calls (%d/%d values)",
				context, calls, n, len(buf))
		}
		m, err := r.Read(buf[n:])
		n += m
		if err != nil {
			t.Fatalf("%s: Read error after %d/%d values: %v", context, n, len(buf), err)
		}
	}
}

// compareExact requires bit-identical PCM: two readers that went through the
// same packet stream must decode to the exact same floats.
func compareExact(t *testing.T, got, want []float32, context string) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("%s: decoded %d values, want %d", context, len(got), len(want))
	}
	for i := range got {
		if got[i] != want[i] {
			t.Fatalf("%s: first divergent sample at index %d: got %g, want %g (delta %g)",
				context, i, got[i], want[i], got[i]-want[i])
		}
	}
}

// After a failed SetPosition the reader must produce exactly the PCM a
// reader that never attempted the seek produces, and the seek must succeed
// when retried.
func TestSeekRecoveryPCM(t *testing.T) {
	const target = 200000
	r, src := newFaultReader(t, "testdata/long.ogg", false)
	ref, _ := newFaultReader(t, "testdata/long.ogg", false)

	// Prime both readers with 17 samples, like the audio preview component.
	readFull(t, r, make([]float32, 17), "priming read")
	readFull(t, ref, make([]float32, 17), "reference priming read")
	posBefore := r.Position()
	lenBefore := r.Length()

	src.resetCounters()
	src.failSeekAt = 1 // fail the first Seek of SetPosition
	err := r.SetPosition(target)
	src.disarm()
	if !errors.Is(err, errSeekFault) {
		t.Fatalf("SetPosition(%d) error = %v, want wrapped one-shot seek fault", target, err)
	}
	if got := r.Position(); got != posBefore {
		t.Fatalf("Position after failed SetPosition(%d) = %d, want %d", target, got, posBefore)
	}
	if got := r.Length(); got != lenBefore {
		t.Fatalf("Length after failed SetPosition(%d) = %d, want %d", target, got, lenBefore)
	}

	// 40000 samples cross the internal buffer boundary several times, so a
	// merely cosmetically restored Position with lost/skipped audio would
	// be caught here.
	got := make([]float32, 40000)
	want := make([]float32, 40000)
	readFull(t, r, got, "post-failure read")
	readFull(t, ref, want, "reference read")
	compareExact(t, got, want, "PCM after failed seek")
	if got, want := r.Position(), ref.Position(); got != want {
		t.Fatalf("Position after continued playback = %d, want %d", got, want)
	}

	// Retrying the seek must succeed and produce the right audio.
	if err := r.SetPosition(target); err != nil {
		t.Fatalf("retry SetPosition(%d): %v", target, err)
	}
	if got := r.Position(); got != target {
		t.Fatalf("Position after retried SetPosition(%d) = %d", target, got)
	}
	if err := ref.SetPosition(target); err != nil {
		t.Fatal(err)
	}
	got = make([]float32, 5000)
	want = make([]float32, 5000)
	readFull(t, r, got, "read after retried seek")
	readFull(t, ref, want, "reference read after seek")
	compareExact(t, got, want, "PCM after retried seek")
}

// SetPosition must roll back cleanly no matter where the underlying I/O
// fails: while scanning for the target page, while reading/restoring that
// page, or while preparing the decode state.
func TestSeekRecoveryFaultPoints(t *testing.T) {
	const target = 200000

	// Dry run: count the Seek and Read calls a successful SetPosition
	// performs after 17 samples have been decoded.
	probe, src := newFaultReader(t, "testdata/long.ogg", false)
	readFull(t, probe, make([]float32, 17), "probe priming read")
	src.resetCounters()
	if err := probe.SetPosition(target); err != nil {
		t.Fatal(err)
	}
	seeks, reads := src.seekCalls, src.readCalls
	if seeks == 0 || reads == 0 {
		t.Fatalf("probe counted %d seeks and %d reads, expected both > 0", seeks, reads)
	}

	faults := []struct {
		name       string
		failSeekAt int
		failReadAt int
	}{
		{"seek/initial", 1, 0},
		{"seek/scan", 1 + seeks/2, 0},
		{"seek/final", seeks, 0},
		{"read/initial", 0, 1},
		{"read/scan", 0, 1 + reads/2},
		{"read/final", 0, reads},
	}
	for _, f := range faults {
		t.Run(f.name, func(t *testing.T) {
			r, src := newFaultReader(t, "testdata/long.ogg", false)
			ref, _ := newFaultReader(t, "testdata/long.ogg", false)
			readFull(t, r, make([]float32, 17), "priming read")
			readFull(t, ref, make([]float32, 17), "reference priming read")
			posBefore := r.Position()
			lenBefore := r.Length()

			src.resetCounters()
			src.failSeekAt = f.failSeekAt
			src.failReadAt = f.failReadAt
			err := r.SetPosition(target)
			src.disarm()
			wantErr := errSeekFault
			if f.failReadAt != 0 {
				wantErr = errReadFault
			}
			if !errors.Is(err, wantErr) {
				t.Fatalf("SetPosition(%d) error = %v, want wrapped %v", target, err, wantErr)
			}
			if got := r.Position(); got != posBefore {
				t.Fatalf("Position after failed SetPosition(%d) = %d, want %d", target, got, posBefore)
			}
			if got := r.Length(); got != lenBefore {
				t.Fatalf("Length after failed SetPosition(%d) = %d, want %d", target, got, lenBefore)
			}

			got := make([]float32, 40000)
			want := make([]float32, 40000)
			readFull(t, r, got, "post-failure read")
			readFull(t, ref, want, "reference read")
			compareExact(t, got, want, "PCM after failed seek")

			if err := r.SetPosition(target); err != nil {
				t.Fatalf("retry SetPosition(%d): %v", target, err)
			}
			if got := r.Position(); got != target {
				t.Fatalf("Position after retried SetPosition(%d) = %d", target, got)
			}
		})
	}
}

// Reads trickling in 1 to 7 bytes at a time must not change decoding or
// seeking behavior, and a one-shot fault on such a source must still roll
// back cleanly.
func TestSeekRecoveryShortReads(t *testing.T) {
	// Independent PCM reference for sequential decoding.
	raw, err := os.Open("testdata/test.raw")
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	rawSize, _ := raw.Seek(0, io.SeekEnd)
	raw.Seek(0, io.SeekStart)
	rawData := make([]float32, rawSize/4)
	binary.Read(raw, binary.LittleEndian, rawData)

	r, _ := newFaultReader(t, "testdata/test.ogg", true)
	got := make([]float32, 0, len(rawData))
	buf := make([]float32, 4096)
	for calls := 0; ; calls++ {
		if calls > len(rawData) {
			t.Fatalf("no read progress after %d Read calls (%d/%d values decoded)",
				calls, len(got), len(rawData))
		}
		n, err := r.Read(buf)
		got = append(got, buf[:n]...)
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("Read error after %d/%d values: %v", len(got), len(rawData), err)
		}
		if n == 0 {
			t.Fatalf("no read progress at %d/%d values", len(got), len(rawData))
		}
	}
	if len(got) != len(rawData) {
		t.Fatalf("short-read decode produced %d values, want %d", len(got), len(rawData))
	}
	for i := range got {
		if !equal(got[i], rawData[i], 0.00002) {
			t.Fatalf("short-read decode: first divergent sample at index %d: got %g, want %g (delta %g)",
				i, got[i], rawData[i], got[i]-rawData[i])
		}
	}

	// Seeking across page boundaries with short reads, including a clamp to
	// EOF and a seek back from EOF.
	r2, _ := newFaultReader(t, "testdata/long.ogg", true)
	ref, _ := newFaultReader(t, "testdata/long.ogg", false)
	for _, target := range []int64{182399, 182400, 182401, 600000} {
		if err := r2.SetPosition(target); err != nil {
			t.Fatalf("short-read SetPosition(%d): %v", target, err)
		}
		if got := r2.Position(); got != target {
			t.Fatalf("Position after short-read SetPosition(%d) = %d", target, got)
		}
		if err := ref.SetPosition(target); err != nil {
			t.Fatal(err)
		}
		got := make([]float32, 5000)
		want := make([]float32, 5000)
		readFull(t, r2, got, "short-read post-seek read")
		readFull(t, ref, want, "reference post-seek read")
		compareExact(t, got, want, "PCM after short-read seek")
	}
	if err := r2.SetPosition(r2.Length()); err != nil {
		t.Fatal(err)
	}
	if got := r2.Position(); got != r2.Length() {
		t.Fatalf("Position after seek to EOF = %d, want %d", got, r2.Length())
	}
	if _, err := r2.Read(make([]float32, 16)); err != io.EOF {
		t.Fatalf("Read at EOF = %v, want EOF", err)
	}
	if err := r2.SetPosition(200000); err != nil {
		t.Fatalf("seek back from EOF: %v", err)
	}

	// A one-shot read fault during a seek on the short-read source.
	r3, src3 := newFaultReader(t, "testdata/long.ogg", true)
	ref3, _ := newFaultReader(t, "testdata/long.ogg", false)
	readFull(t, r3, make([]float32, 17), "priming read")
	readFull(t, ref3, make([]float32, 17), "reference priming read")
	posBefore := r3.Position()
	src3.resetCounters()
	src3.failReadAt = 2
	err = r3.SetPosition(200000)
	src3.disarm()
	if !errors.Is(err, errReadFault) {
		t.Fatalf("SetPosition(200000) error = %v, want wrapped one-shot read fault", err)
	}
	if got := r3.Position(); got != posBefore {
		t.Fatalf("Position after failed SetPosition(200000) = %d, want %d", got, posBefore)
	}
	got3 := make([]float32, 40000)
	want3 := make([]float32, 40000)
	readFull(t, r3, got3, "post-failure read")
	readFull(t, ref3, want3, "reference read")
	compareExact(t, got3, want3, "PCM after failed short-read seek")
	if err := r3.SetPosition(200000); err != nil {
		t.Fatalf("retry SetPosition(200000): %v", err)
	}
}

// A second SetPosition issued while a previous seek still has pending (not
// yet decoded) skip samples must not leak the leftover state, whether it
// succeeds, clamps to EOF, or fails.
func TestSeekRecoveryConsecutiveSeeks(t *testing.T) {
	t.Run("success", func(t *testing.T) {
		r, _ := newFaultReader(t, "testdata/long.ogg", false)
		ref, _ := newFaultReader(t, "testdata/long.ogg", false)
		if err := r.SetPosition(600000); err != nil {
			t.Fatal(err)
		}
		// No Read in between: the skip samples of the first seek are still
		// pending and must be discarded by the second seek.
		if err := r.SetPosition(182401); err != nil {
			t.Fatal(err)
		}
		if got := r.Position(); got != 182401 {
			t.Fatalf("Position after second SetPosition(182401) = %d", got)
		}
		if err := ref.SetPosition(182401); err != nil {
			t.Fatal(err)
		}
		got := make([]float32, 5000)
		want := make([]float32, 5000)
		readFull(t, r, got, "post-seek read")
		readFull(t, ref, want, "reference post-seek read")
		compareExact(t, got, want, "PCM after consecutive seeks")
	})

	t.Run("eof", func(t *testing.T) {
		r, _ := newFaultReader(t, "testdata/long.ogg", false)
		ref, _ := newFaultReader(t, "testdata/long.ogg", false)
		if err := r.SetPosition(600000); err != nil {
			t.Fatal(err)
		}
		// Clamping to EOF must discard the pending skip samples of the
		// previous seek: Position must be exactly Length.
		if err := r.SetPosition(r.Length()); err != nil {
			t.Fatal(err)
		}
		if got := r.Position(); got != r.Length() {
			t.Fatalf("Position after seek to EOF = %d, want %d", got, r.Length())
		}
		if _, err := r.Read(make([]float32, 16)); err != io.EOF {
			t.Fatalf("Read at EOF = %v, want EOF", err)
		}
		// Seeking back from EOF to a valid position must still work.
		if err := r.SetPosition(200000); err != nil {
			t.Fatalf("seek back from EOF: %v", err)
		}
		if err := ref.SetPosition(200000); err != nil {
			t.Fatal(err)
		}
		got := make([]float32, 5000)
		want := make([]float32, 5000)
		readFull(t, r, got, "read after seek back from EOF")
		readFull(t, ref, want, "reference read")
		compareExact(t, got, want, "PCM after seek back from EOF")
	})

	t.Run("failure", func(t *testing.T) {
		r, src := newFaultReader(t, "testdata/long.ogg", false)
		ref, _ := newFaultReader(t, "testdata/long.ogg", false)
		if err := r.SetPosition(600000); err != nil {
			t.Fatal(err)
		}
		if err := ref.SetPosition(600000); err != nil {
			t.Fatal(err)
		}
		// Fail the second seek: the pending state of the first seek
		// (position 600000 with skip samples) must survive untouched.
		src.resetCounters()
		src.failSeekAt = 1
		err := r.SetPosition(300000)
		src.disarm()
		if !errors.Is(err, errSeekFault) {
			t.Fatalf("SetPosition(300000) error = %v, want wrapped one-shot seek fault", err)
		}
		if got := r.Position(); got != 600000 {
			t.Fatalf("Position after failed second SetPosition(300000) = %d, want 600000", got)
		}
		got := make([]float32, 40000)
		want := make([]float32, 40000)
		readFull(t, r, got, "post-failure read")
		readFull(t, ref, want, "reference read")
		compareExact(t, got, want, "PCM after failed second seek")
	})
}

// A negative target must be rejected without touching the underlying source
// or the playback state.
func TestSeekRecoveryNegativeTarget(t *testing.T) {
	r, src := newFaultReader(t, "testdata/long.ogg", false)
	ref, _ := newFaultReader(t, "testdata/long.ogg", false)
	readFull(t, r, make([]float32, 17), "priming read")
	readFull(t, ref, make([]float32, 17), "reference priming read")
	posBefore := r.Position()
	offBefore, err := src.f.Seek(0, io.SeekCurrent)
	if err != nil {
		t.Fatal(err)
	}

	if err := r.SetPosition(-1); err == nil {
		t.Fatal("SetPosition(-1) = nil, want error")
	}
	if got := r.Position(); got != posBefore {
		t.Fatalf("Position after SetPosition(-1) = %d, want %d", got, posBefore)
	}
	offAfter, err := src.f.Seek(0, io.SeekCurrent)
	if err != nil {
		t.Fatal(err)
	}
	if offAfter != offBefore {
		t.Fatalf("negative seek moved the source cursor from %d to %d", offBefore, offAfter)
	}

	got := make([]float32, 40000)
	want := make([]float32, 40000)
	readFull(t, r, got, "post-rejection read")
	readFull(t, ref, want, "reference read")
	compareExact(t, got, want, "PCM after rejected negative seek")
}

// A plain io.Reader must keep reporting "not seekable" and continue
// decoding normally afterwards.
func TestSeekRecoveryNonSeekable(t *testing.T) {
	f, err := os.Open("testdata/test.ogg")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	r, err := oggvorbis.NewReader(reader{f})
	if err != nil {
		t.Fatal(err)
	}
	err = r.SetPosition(100)
	if err == nil || err.Error() != "oggvorbis: reader is not seekable" {
		t.Fatalf("SetPosition on non-seekable reader = %v, want \"oggvorbis: reader is not seekable\"", err)
	}
	got := make([]float32, 1000)
	readFull(t, r, got, "read after rejected seek")
	if got[0] == 0 && got[len(got)-1] == 0 {
		t.Fatal("reader produced only silence after rejected seek")
	}
}
