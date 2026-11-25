package beep_test

import (
	"errors"
	"fmt"
	"math"
	"testing"

	"github.com/orcaman/writerseeker"

	"github.com/gopxl/beep/v2"
	"github.com/gopxl/beep/v2/task_support/taskcheck"
	"github.com/gopxl/beep/v2/wav"
)

// ---------------------------------------------------------------------------
// Instrumented material factory
// ---------------------------------------------------------------------------

// testReader is one reader handed out by a testFactory. Every Open call makes a
// brand new instance, and the factory records it so tests can assert exactly
// how many readers were opened and closed.
type testReader struct {
	factory    *testFactory
	data       [][2]float64
	length     int
	pos        int
	closed     bool
	closeCalls int
	seekLog    []int
	readLog    []int
}

func (r *testReader) Stream(samples [][2]float64) (n int, ok bool) {
	if r.closed || len(samples) == 0 {
		return 0, false
	}
	if r.factory.readErr != nil && r.pos >= r.factory.readErrAt {
		return 0, false
	}
	if r.factory.shortBefore >= 0 && r.pos >= r.factory.shortBefore {
		return 0, false
	}
	if r.pos >= len(r.data) {
		return 0, false
	}
	n = copy(samples, r.data[r.pos:])
	r.pos += n
	r.readLog = append(r.readLog, r.pos)
	return n, true
}

func (r *testReader) Err() error {
	if r.factory.readErr != nil && r.pos >= r.factory.readErrAt {
		return r.factory.readErr
	}
	return nil
}

func (r *testReader) Len() int { return r.length }

func (r *testReader) Position() int { return r.pos }

func (r *testReader) Seek(p int) error {
	if r.factory.seekErr != nil {
		return r.factory.seekErr
	}
	r.pos = p
	r.seekLog = append(r.seekLog, p)
	return nil
}

func (r *testReader) Close() error {
	r.closeCalls++
	if r.closed {
		return nil
	}
	r.closed = true
	r.factory.closes++
	return r.factory.closeErr
}

// testFactory produces fresh readers over one shared material table. It can
// inject open, seek, read, short-read and close failures.
type testFactory struct {
	data        [][2]float64
	length      int
	openErr     error
	openErrAt   int // 1-based Open call that fails; values <= 0 mean never
	seekErr     error
	readErr     error
	readErrAt   int
	shortBefore int
	closeErr    error
	shared      *testReader

	opens   int
	closes  int
	readers []*testReader
}

func newTestFactory(material [][2]float64) *testFactory {
	return &testFactory{
		data:        material,
		length:      len(material),
		readErrAt:   math.MaxInt,
		shortBefore: -1,
	}
}

func (f *testFactory) Open() (beep.StreamSeekCloser, error) {
	f.opens++
	if f.shared != nil {
		return f.shared, nil
	}
	if f.openErr != nil && f.openErrAt > 0 && f.opens == f.openErrAt {
		return nil, f.openErr
	}
	r := &testReader{factory: f, data: f.data, length: f.length}
	f.readers = append(f.readers, r)
	return r, nil
}

func (f *testFactory) allClosedOnce() bool {
	if f.closes != len(f.readers) {
		return false
	}
	for _, r := range f.readers {
		if r.closeCalls != 1 {
			return false
		}
	}
	return true
}

func material(n int) [][2]float64 {
	data := make([][2]float64, n)
	for i := range data {
		x := math.Sin(float64(i)*0.17)*0.6 + float64(i)*0.003
		y := math.Cos(float64(i)*0.11)*0.4 - float64(i)*0.002
		data[i] = [2]float64{x, y}
	}
	return data
}

// clipDef is an independent description used both to build a TimelineClip and
// to compute the expected output with a separate formula.
type clipDef struct {
	f        *testFactory
	start    int
	in       int
	out      int
	duration int
	gain     float64
}

func (d clipDef) clip() beep.TimelineClip {
	return beep.TimelineClip{
		Open:     d.f.Open,
		Start:    d.start,
		In:       d.in,
		Out:      d.out,
		Duration: d.duration,
		Gain:     d.gain,
	}
}

func clipsOf(defs ...clipDef) []beep.TimelineClip {
	clips := make([]beep.TimelineClip, len(defs))
	for i, d := range defs {
		clips[i] = d.clip()
	}
	return clips
}

// expectedAt is a deliberately independent rendering of the documented
// formula: absolute material position In+k*(Out-In)/Duration, linear
// interpolation with the right neighbour clamped to Out-1, then gain, summed
// across every clip covering the output frame.
func expectedAt(defs []clipDef, frame int) [2]float64 {
	var sum [2]float64
	for _, d := range defs {
		k := frame - d.start
		if k < 0 || k >= d.duration {
			continue
		}
		pos := float64(d.in) + float64(k)*float64(d.out-d.in)/float64(d.duration)
		left := int(math.Floor(pos))
		if left < d.in {
			left = d.in
		}
		if left > d.out-1 {
			left = d.out - 1
		}
		frac := pos - float64(left)
		right := left + 1
		if right > d.out-1 {
			right = d.out - 1
		}
		for ch := 0; ch < 2; ch++ {
			l := d.f.data[left][ch]
			r := d.f.data[right][ch]
			sum[ch] += (l*(1-frac) + r*frac) * d.gain
		}
	}
	return sum
}

const closeEnough = 1e-9

func sameSample(a, b [2]float64) bool {
	return math.Abs(a[0]-b[0]) <= closeEnough && math.Abs(a[1]-b[1]) <= closeEnough
}

// checkFrames compares each delivered sample against the independent formula.
func checkFrames(c *taskcheck.C, got [][2]float64, from int, defs []clipDef, label string) {
	for i := range got {
		want := expectedAt(defs, from+i)
		if !sameSample(got[i], want) {
			c.Check(false, "%s: frame %d got %v want %v", label, from+i, got[i], want)
			return
		}
	}
	c.Check(true, "%s: all %d frames match", label, len(got))
}

// checkClosedOnce asserts every reader of every factory was closed exactly once.
func checkClosedOnce(c *taskcheck.C, label string, factories ...*testFactory) {
	for i, f := range factories {
		c.Check(f.allClosedOnce(), "%s: factory %d opens=%d closes=%d", label, i, f.opens, f.closes)
	}
}

// ---------------------------------------------------------------------------
// TestTimelineRevision
// ---------------------------------------------------------------------------

func TestTimelineRevision(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("validation", func(t *testing.T) {
			f := newTestFactory(material(32))
			valid := clipDef{f: f, start: 0, in: 0, out: 16, duration: 16, gain: 1}.clip()

			bad := func(clips []beep.TimelineClip, rate beep.SampleRate) {
				_, err := beep.NewTimeline(rate, clips)
				c.Check(err != nil, "invalid input must be rejected")
			}
			bad(nil, 8000)
			bad([]beep.TimelineClip{}, 8000)
			bad([]beep.TimelineClip{valid}, 0)
			bad([]beep.TimelineClip{valid}, -1)

			mutate := func(fn func(*beep.TimelineClip)) []beep.TimelineClip {
				cl := valid
				fn(&cl)
				return []beep.TimelineClip{cl}
			}
			bad(mutate(func(cl *beep.TimelineClip) { cl.Open = nil }), 8000)
			bad(mutate(func(cl *beep.TimelineClip) { cl.Start = -1 }), 8000)
			bad(mutate(func(cl *beep.TimelineClip) { cl.In = -1 }), 8000)
			bad(mutate(func(cl *beep.TimelineClip) { cl.Out = cl.In }), 8000)
			bad(mutate(func(cl *beep.TimelineClip) { cl.Out = cl.In - 1 }), 8000)
			bad(mutate(func(cl *beep.TimelineClip) { cl.Duration = 0 }), 8000)
			bad(mutate(func(cl *beep.TimelineClip) { cl.Duration = -3 }), 8000)
			bad(mutate(func(cl *beep.TimelineClip) { cl.Gain = -0.1 }), 8000)
			bad(mutate(func(cl *beep.TimelineClip) { cl.Gain = math.NaN() }), 8000)
			bad(mutate(func(cl *beep.TimelineClip) { cl.Gain = math.Inf(1) }), 8000)
			bad(mutate(func(cl *beep.TimelineClip) { cl.Start = math.MaxInt - 1; cl.Duration = 10 }), 8000)

			// Whole-table validation happens before any reader is opened.
			c.Check(f.opens == 0, "validation must not open any reader, got %d", f.opens)
		})

		t.Run("length_holes_and_overlap", func(t *testing.T) {
			f1 := newTestFactory(material(64))
			f2 := newTestFactory(material(64))
			defs := []clipDef{
				{f: f1, start: 0, in: 0, out: 10, duration: 10, gain: 1},
				{f: f2, start: 4, in: 20, out: 32, duration: 12, gain: 0.5},
			}
			tl, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			c.Check(tl.Len() == 16, "Len should be max(Start+Duration)=16, got %d", tl.Len())
			c.Check(tl.Position() == 0, "fresh Position should be 0")
			c.Check(tl.Err() == nil, "fresh Err should be nil")

			var buf [][2]float64 = make([][2]float64, 16)
			n, ok := tl.Stream(buf)
			c.Check(n == 16 && ok, "expected 16 frames, got n=%d ok=%v", n, ok)
			checkFrames(c, buf[:n], 0, defs, "overlap render")

			n, ok = tl.Stream(buf[:4])
			c.Check(n == 0 && !ok, "after EOF expected 0,false, got %d,%v", n, ok)
			c.Check(tl.Err() == nil, "normal EOF must not set Err")
			checkClosedOnce(c, "normal EOF releases both generations", f1, f2)
		})

		t.Run("non_integer_retiming", func(t *testing.T) {
			f := newTestFactory(material(32))
			defs := []clipDef{{f: f, start: 0, in: 0, out: 7, duration: 3, gain: 1}}
			tl, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			buf := make([][2]float64, 3)
			n, ok := tl.Stream(buf)
			c.Check(n == 3 && ok, "expected 3 frames, got %d,%v", n, ok)
			checkFrames(c, buf[:n], 0, defs, "non-integer retiming")
			// The middle frame must be interpolated, not snapped to a material frame.
			c.Check(!sameSample(buf[1], [2]float64{f.data[2][0], f.data[2][1]}), "middle frame should interpolate")
		})

		t.Run("caller_slice_is_copied", func(t *testing.T) {
			f := newTestFactory(material(32))
			defs := []clipDef{{f: f, start: 0, in: 0, out: 8, duration: 8, gain: 1}}
			clips := clipsOf(defs...)
			tl, err := beep.NewTimeline(8000, clips)
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			// Mutating the caller's table must not change the prepared plan.
			clips[0].Start = 100
			clips[0].Gain = 100
			clips[0].In = 20
			_ = clips
			buf := make([][2]float64, 8)
			n, ok := tl.Stream(buf)
			c.Check(n == 8 && ok, "expected 8 frames, got %d,%v", n, ok)
			checkFrames(c, buf[:n], 0, defs, "copied plan")
		})

		t.Run("schedule_and_len_transitions", func(t *testing.T) {
			fOld := newTestFactory(material(48))
			fNew := newTestFactory(material(48))
			old := []clipDef{{f: fOld, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			newer := []clipDef{{f: fNew, start: 0, in: 10, out: 40, duration: 24, gain: 1}}

			tl, err := beep.NewTimeline(8000, clipsOf(old...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			c.Check(tl.Len() == 16, "old Len should be 16")

			err = tl.Schedule(clipsOf(newer...), 4, 0)
			c.Check(err == nil, "schedule failed: %v", err)
			c.Check(tl.Len() == 24, "after schedule Len must report the new end (24), got %d", tl.Len())
			c.Check(tl.Position() == 0, "schedule must not advance Position")

			buf := make([][2]float64, 4)
			n, ok := tl.Stream(buf)
			c.Check(n == 4 && ok, "expected 4 old frames, got %d,%v", n, ok)
			c.Check(tl.Position() == 4, "Position should be 4")
			checkFrames(c, buf[:n], 0, old, "before switch")

			// After the switch frame, Len still reports the new end.
			buf = make([][2]float64, 4)
			n, ok = tl.Stream(buf)
			c.Check(n == 4 && ok, "expected 4 new frames, got %d,%v", n, ok)
			checkFrames(c, buf[:n], 4, newer, "after hard switch")

			// Cancelling after the switch must fail.
			yes, err := tl.Cancel()
			c.Check(!yes && err == nil, "cancel after switch must return false,nil, got %v,%v", yes, err)
		})

		t.Run("cancel_before_output", func(t *testing.T) {
			fOld := newTestFactory(material(48))
			fNew := newTestFactory(material(48))
			old := []clipDef{{f: fOld, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			newer := []clipDef{{f: fNew, start: 0, in: 0, out: 30, duration: 30, gain: 1}}

			tl, err := beep.NewTimeline(8000, clipsOf(old...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			if err := tl.Schedule(clipsOf(newer...), 4, 0); err != nil {
				c.Check(false, "schedule failed: %v", err)
				return
			}
			c.Check(tl.Len() == 30, "pending Len should be 30")

			// Advance exactly to the switch frame without outputting it.
			buf := make([][2]float64, 4)
			n, ok := tl.Stream(buf)
			c.Check(n == 4 && ok, "expected 4 frames, got %d,%v", n, ok)
			c.Check(tl.Position() == 4, "Position should be 4")

			yes, err := tl.Cancel()
			c.Check(yes && err == nil, "cancel at the switch frame must succeed, got %v,%v", yes, err)
			c.Check(tl.Len() == 16, "cancel must restore the old end (16), got %d", tl.Len())
			c.Check(fNew.allClosedOnce(), "cancelled pending readers must be closed once")

			yes, err = tl.Cancel()
			c.Check(!yes && err == nil, "cancel with no pending must return false,nil, got %v,%v", yes, err)

			buf = make([][2]float64, 4)
			n, ok = tl.Stream(buf)
			c.Check(n == 4 && ok, "expected 4 old frames, got %d,%v", n, ok)
			checkFrames(c, buf[:n], 4, old, "old audio after cancel")
		})

		t.Run("schedule_constraints", func(t *testing.T) {
			fOld := newTestFactory(material(48))
			fNew := newTestFactory(material(48))
			old := []clipDef{{f: fOld, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			newer := []clipDef{{f: fNew, start: 0, in: 0, out: 12, duration: 12, gain: 1}}

			tl, err := beep.NewTimeline(8000, clipsOf(old...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			buf := make([][2]float64, 5)
			tl.Stream(buf)
			c.Check(tl.Position() == 5, "Position should be 5")

			checkErr := func(label string, wantErr bool) {
				err := tl.Schedule(clipsOf(newer...), 0, 0)
				_ = label
				c.Check((err != nil) == wantErr, "%s: err=%v", label, err)
			}
			checkErr("at before Position", true)
			c.Check(tl.Schedule(clipsOf(newer...), 5, -1) != nil, "negative fade must be rejected")
			c.Check(tl.Schedule(clipsOf(newer...), 12, 0) != nil, "at >= new length must be rejected")
			c.Check(tl.Schedule(clipsOf(newer...), 16, 0) != nil, "at >= old length must be rejected")
			c.Check(tl.Schedule(clipsOf(newer...), 6, 11) != nil, "at+fade past new length must be rejected")
			c.Check(tl.Schedule(clipsOf(old...), 6, 11) != nil, "at+fade past old length must be rejected")
			c.Check(tl.Schedule(nil, 6, 0) != nil, "empty clip table must be rejected")

			// No failed schedule may have installed anything.
			c.Check(tl.Len() == 16, "failed schedules must not change Len, got %d", tl.Len())
			c.Check(fNew.opens == 0, "failed validation must not open readers")
			c.Check(fNew.closes == 0, "failed validation must not close anything")
		})

		t.Run("fade_formula_and_boundaries", func(t *testing.T) {
			fOld := newTestFactory(material(64))
			fNew := newTestFactory(material(64))
			old := []clipDef{{f: fOld, start: 0, in: 0, out: 12, duration: 12, gain: 1}}
			newer := []clipDef{{f: fNew, start: 0, in: 6, out: 34, duration: 16, gain: 0.75}}

			tl, err := beep.NewTimeline(8000, clipsOf(old...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			const at, fade = 4, 4
			if err := tl.Schedule(clipsOf(newer...), at, fade); err != nil {
				c.Check(false, "schedule failed: %v", err)
				return
			}

			// Cross the switch and the fade end inside one odd-sized chunk.
			var got [][2]float64
			buf := make([][2]float64, 7)
			for len(got) < at+fade+4 {
				n, ok := tl.Stream(buf[:7])
				c.Check(ok, "stream should stay ok during fade")
				got = append(got, buf[:n]...)
			}

			for k := 0; k < at+fade+4; k++ {
				var want [2]float64
				if k < at {
					want = expectedAt(old, k)
				} else if k < at+fade {
					w := float64((k-at)+1) / float64(fade+1)
					o := expectedAt(old, k)
					nw := expectedAt(newer, k)
					want = [2]float64{o[0]*(1-w) + nw[0]*w, o[1]*(1-w) + nw[1]*w}
				} else {
					want = expectedAt(newer, k)
				}
				if !sameSample(got[k], want) {
					c.Check(false, "fade frame %d got %v want %v", k, got[k], want)
					break
				}
			}
			c.Check(true, "fade formula matched across %d frames", len(got))

			// The old generation must be released right after its last frame.
			c.Check(fOld.closes == 1, "old generation should be closed after the fade, closes=%d", fOld.closes)
			c.Check(fNew.closes == 0, "new generation must stay open during the fade")
			checkClosedOnce(c, "old closed exactly once", fOld)
		})

		t.Run("schedule_rejected_during_fade", func(t *testing.T) {
			fOld := newTestFactory(material(64))
			fNew := newTestFactory(material(64))
			fThird := newTestFactory(material(64))
			old := []clipDef{{f: fOld, start: 0, in: 0, out: 12, duration: 12, gain: 1}}
			newer := []clipDef{{f: fNew, start: 0, in: 0, out: 12, duration: 12, gain: 1}}
			third := []clipDef{{f: fThird, start: 0, in: 0, out: 12, duration: 12, gain: 1}}

			tl, err := beep.NewTimeline(8000, clipsOf(old...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			if err := tl.Schedule(clipsOf(newer...), 2, 3); err != nil {
				c.Check(false, "schedule failed: %v", err)
				return
			}
			buf := make([][2]float64, 3)
			n, _ := tl.Stream(buf)
			c.Check(n == 3, "expected 3 frames, got %d", n)
			// Position 3 is inside the fade (frames 2,3,4).
			c.Check(tl.Schedule(clipsOf(third...), 3, 0) != nil, "schedule during fade must be rejected")
			yes, err := tl.Cancel()
			c.Check(!yes && err == nil, "cancel during fade must return false,nil, got %v,%v", yes, err)
		})

		t.Run("second_schedule_replaces_pending", func(t *testing.T) {
			fOld := newTestFactory(material(64))
			fNew1 := newTestFactory(material(64))
			fNew2 := newTestFactory(material(64))
			old := []clipDef{{f: fOld, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			n1 := []clipDef{{f: fNew1, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			n2 := []clipDef{{f: fNew2, start: 0, in: 4, out: 20, duration: 16, gain: 1}}

			tl, err := beep.NewTimeline(8000, clipsOf(old...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			if err := tl.Schedule(clipsOf(n1...), 4, 0); err != nil {
				c.Check(false, "first schedule failed: %v", err)
				return
			}
			if err := tl.Schedule(clipsOf(n2...), 10, 0); err != nil {
				c.Check(false, "second schedule failed: %v", err)
				return
			}
			c.Check(fNew1.allClosedOnce(), "replaced pending must be closed exactly once")
			c.Check(fNew2.closes == 0, "new pending must stay open")

			buf := make([][2]float64, 10)
			n, ok := tl.Stream(buf)
			c.Check(n == 10 && ok, "expected 10 frames, got %d,%v", n, ok)
			checkFrames(c, buf[:n], 0, old, "old audio before second schedule switch")
			buf = make([][2]float64, 6)
			n, ok = tl.Stream(buf)
			c.Check(n == 6 && ok, "expected 6 frames, got %d,%v", n, ok)
			checkFrames(c, buf[:n], 10, n2, "second pending audio")
		})

		t.Run("zero_length_stream", func(t *testing.T) {
			f := newTestFactory(material(16))
			defs := []clipDef{{f: f, start: 0, in: 0, out: 8, duration: 8, gain: 1}}
			tl, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			n, ok := tl.Stream(nil)
			c.Check(n == 0 && ok, "zero-length stream before termination should be 0,true, got %d,%v", n, ok)
			c.Check(tl.Position() == 0, "zero-length stream must not advance")

			buf := make([][2]float64, 8)
			tl.Stream(buf)
			n, ok = tl.Stream([][2]float64{})
			c.Check(n == 0 && !ok, "zero-length stream after termination should be 0,false, got %d,%v", n, ok)
			c.Check(f.allClosedOnce(), "readers closed after normal EOF")
		})
		t.Run("switch_at_final_frame", func(t *testing.T) {
			fOld := newTestFactory(material(40))
			fNew := newTestFactory(material(40))
			old := []clipDef{{f: fOld, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			newer := []clipDef{{f: fNew, start: 0, in: 8, out: 40, duration: 16, gain: 1}}

			tl, err := beep.NewTimeline(8000, clipsOf(old...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			// at+fade == newLen == old length is the exclusive end and must be
			// accepted; only the final frame is faded.
			err = tl.Schedule(clipsOf(newer...), 15, 1)
			c.Check(err == nil, "at+fade == newLen must be accepted, got %v", err)

			buf := make([][2]float64, 16)
			n, ok := tl.Stream(buf)
			c.Check(n == 16 && ok, "expected 16 frames, got %d,%v", n, ok)
			checkFrames(c, buf[:15], 0, old, "old audio up to the final frame")
			// Frame 15 is the first faded frame: w = (0+1)/(1+1) = 0.5.
			o := expectedAt(old, 15)
			nw := expectedAt(newer, 15)
			want := [2]float64{(o[0] + nw[0]) / 2, (o[1] + nw[1]) / 2}
			c.Check(sameSample(buf[15], want), "final faded frame got %v want %v", buf[15], want)
			n, ok = tl.Stream(buf[:1])
			c.Check(n == 0 && !ok, "expected EOF after the old end, got %d,%v", n, ok)
			checkClosedOnce(c, "final-frame switch releases both generations", fOld, fNew)
		})
	})
}

// ---------------------------------------------------------------------------
// TestTimelineOwnership
// ---------------------------------------------------------------------------

func TestTimelineOwnership(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("construction_cleanup", func(t *testing.T) {
			sentinel := errors.New("open exploded")
			f1 := newTestFactory(material(32))
			f2 := newTestFactory(material(32))
			f2.openErr = sentinel
			f2.openErrAt = 1
			defs := []clipDef{
				{f: f1, start: 0, in: 0, out: 8, duration: 8, gain: 1},
				{f: f2, start: 8, in: 0, out: 8, duration: 8, gain: 1},
			}
			_, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(errors.Is(err, sentinel), "open error identity lost: %v", err)
			checkClosedOnce(c, "construction open failure", f1, f2)
		})

		t.Run("construction_seek_cleanup", func(t *testing.T) {
			sentinel := errors.New("seek exploded")
			f1 := newTestFactory(material(32))
			f2 := newTestFactory(material(32))
			f2.seekErr = sentinel
			defs := []clipDef{
				{f: f1, start: 0, in: 0, out: 8, duration: 8, gain: 1},
				{f: f2, start: 8, in: 0, out: 8, duration: 8, gain: 1},
			}
			_, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(errors.Is(err, sentinel), "seek error identity lost: %v", err)
			checkClosedOnce(c, "construction seek failure", f1, f2)
		})

		t.Run("short_reader_cleanup", func(t *testing.T) {
			f := newTestFactory(material(4)) // Len 4 < Out 8
			defs := []clipDef{{f: f, start: 0, in: 0, out: 8, duration: 8, gain: 1}}
			_, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err != nil, "reader shorter than Out must be rejected")
			checkClosedOnce(c, "short reader cleanup", f)
		})

		t.Run("shared_reader_rejected", func(t *testing.T) {
			f := newTestFactory(material(32))
			shared := &testReader{factory: f, data: f.data, length: f.length}
			f.shared = shared
			defs := []clipDef{
				{f: f, start: 0, in: 0, out: 8, duration: 8, gain: 1},
				{f: f, start: 8, in: 0, out: 8, duration: 8, gain: 1},
			}
			_, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err != nil, "a factory returning a shared reader must be rejected")
		})

		t.Run("failed_prepare_preserves_audible_and_pending", func(t *testing.T) {
			fOld := newTestFactory(material(48))
			fPend := newTestFactory(material(48))
			fFail := newTestFactory(material(48))
			sentinel := errors.New("late open failure")
			fFail.openErr = sentinel
			fFail.openErrAt = 1

			old := []clipDef{{f: fOld, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			pend := []clipDef{{f: fPend, start: 0, in: 4, out: 20, duration: 16, gain: 1}}
			failing := []clipDef{{f: fFail, start: 0, in: 0, out: 16, duration: 16, gain: 1}}

			tl, err := beep.NewTimeline(8000, clipsOf(old...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			if err := tl.Schedule(clipsOf(pend...), 4, 0); err != nil {
				c.Check(false, "initial schedule failed: %v", err)
				return
			}

			// Advance a little so the audible cursor is non-zero.
			buf := make([][2]float64, 2)
			tl.Stream(buf)

			curPos := fOld.readers[0].pos
			pendPos := fPend.readers[0].pos
			oldLen := tl.Len()

			err = tl.Schedule(clipsOf(failing...), 6, 0)
			c.Check(errors.Is(err, sentinel), "preparation error identity lost: %v", err)
			c.Check(tl.Len() == oldLen, "Len changed on failed prepare: %d -> %d", oldLen, tl.Len())
			c.Check(fOld.readers[0].pos == curPos, "audible cursor changed on failed prepare")
			c.Check(fPend.readers[0].pos == pendPos, "pending cursor changed on failed prepare")
			c.Check(fOld.closes == 0 && fPend.closes == 0, "failed prepare must not close existing readers")
			c.Check(fFail.allClosedOnce(), "failed prepare must close its partial readers")

			// The original pending arrangement still takes effect at frame 4.
			buf = make([][2]float64, 14)
			n, ok := tl.Stream(buf)
			c.Check(n == 14 && ok, "expected 14 frames, got %d,%v", n, ok)
			checkFrames(c, buf[:2], 2, old, "audible audio after failed prepare")
			checkFrames(c, buf[2:], 4, pend, "pending audio after failed prepare")
			n, ok = tl.Stream(buf[:4])
			c.Check(n == 0 && !ok, "expected EOF at the pending end, got %d,%v", n, ok)
		})

		t.Run("replacement_cleanup_error_is_reported", func(t *testing.T) {
			fOld := newTestFactory(material(48))
			fPend := newTestFactory(material(48))
			fNew := newTestFactory(material(48))
			closeSentinel := errors.New("close exploded")
			fPend.closeErr = closeSentinel

			old := []clipDef{{f: fOld, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			pend := []clipDef{{f: fPend, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			newer := []clipDef{{f: fNew, start: 0, in: 0, out: 16, duration: 16, gain: 1}}

			tl, err := beep.NewTimeline(8000, clipsOf(old...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			if err := tl.Schedule(clipsOf(pend...), 4, 0); err != nil {
				c.Check(false, "first schedule failed: %v", err)
				return
			}
			err = tl.Schedule(clipsOf(newer...), 8, 0)
			c.Check(errors.Is(err, closeSentinel), "cleanup error identity lost: %v", err)
			// The cleanup error must not be treated as a preparation failure:
			// the new generation is installed and Len reports its end.
			c.Check(tl.Len() == 16, "replacement must remain installed after cleanup error, Len=%d", tl.Len())
			c.Check(fPend.closes == 1, "replaced pending must be closed")

			// The new generation is what becomes audible.
			buf := make([][2]float64, 12)
			tl.Stream(buf)
			buf = make([][2]float64, 4)
			n, ok := tl.Stream(buf)
			c.Check(n == 4 && ok, "expected 4 new frames, got %d,%v", n, ok)
			checkFrames(c, buf[:n], 12, newer, "new generation after cleanup error")
		})

		t.Run("independent_instances_per_generation", func(t *testing.T) {
			f := newTestFactory(material(48))
			old := []clipDef{{f: f, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			newer := []clipDef{{f: f, start: 0, in: 8, out: 32, duration: 16, gain: 1}}

			tl, err := beep.NewTimeline(8000, clipsOf(old...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			if err := tl.Schedule(clipsOf(newer...), 4, 2); err != nil {
				c.Check(false, "schedule failed: %v", err)
				return
			}
			c.Check(f.opens == 2, "each generation must open its own reader, got %d", f.opens)
			c.Check(f.readers[0] != f.readers[1], "generations must not share a reader instance")

			buf := make([][2]float64, 10)
			n, ok := tl.Stream(buf)
			c.Check(n == 10 && ok, "expected 10 frames, got %d,%v", n, ok)
			// Frames before the switch use the old generation; frames inside
			// the fade are blended; after the fade only the new generation.
			checkFrames(c, buf[:4], 0, old, "old generation absolute render")
			if len(buf) > 6 {
				checkFrames(c, buf[6:], 6, newer, "new generation starts at In, not zero")
			}
			// Old and new cursors are independent readers.
			c.Check(f.readers[0] != f.readers[1], "readers must be distinct")
		})

		t.Run("read_error_prefix_then_exposure", func(t *testing.T) {
			sentinel := errors.New("read exploded")
			f := newTestFactory(material(32))
			f.readErr = sentinel
			f.readErrAt = 5
			defs := []clipDef{{f: f, start: 0, in: 0, out: 20, duration: 20, gain: 1}}
			tl, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			buf := make([][2]float64, 10)
			for i := range buf {
				buf[i] = [2]float64{123, 456}
			}
			n, ok := tl.Stream(buf)
			c.Check(n == 5 && ok, "expected a 5-frame prefix with ok=true, got %d,%v", n, ok)
			c.Check(tl.Err() == nil, "Err must stay nil when a prefix was delivered")
			checkFrames(c, buf[:n], 0, defs, "prefix before read error")
			for i := n; i < len(buf); i++ {
				c.Check(buf[i] == [2]float64{123, 456}, "samples[%d:] must be untouched after the prefix", i)
			}

			next := make([][2]float64, 4)
			for i := range next {
				next[i] = [2]float64{7, 8}
			}
			n, ok = tl.Stream(next)
			c.Check(n == 0 && !ok, "next non-empty Stream must return 0,false, got %d,%v", n, ok)
			c.Check(errors.Is(tl.Err(), sentinel), "read error identity lost: %v", tl.Err())
			c.Check(next[0] == [2]float64{7, 8}, "next buffer must be untouched")
			checkClosedOnce(c, "read error releases readers", f)
		})

		t.Run("read_error_without_prefix", func(t *testing.T) {
			sentinel := errors.New("immediate read failure")
			f := newTestFactory(material(32))
			f.readErr = sentinel
			f.readErrAt = 0
			defs := []clipDef{{f: f, start: 0, in: 0, out: 20, duration: 20, gain: 1}}
			tl, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			buf := make([][2]float64, 4)
			for i := range buf {
				buf[i] = [2]float64{9, 10}
			}
			n, ok := tl.Stream(buf)
			c.Check(n == 0 && !ok, "immediate error must return 0,false, got %d,%v", n, ok)
			c.Check(errors.Is(tl.Err(), sentinel), "read error identity lost: %v", tl.Err())
			c.Check(buf[0] == [2]float64{9, 10}, "buffer must be untouched on immediate error")
			checkClosedOnce(c, "immediate read error releases readers", f)
		})

		t.Run("short_source_is_an_error", func(t *testing.T) {
			f := newTestFactory(material(20))
			f.shortBefore = 10 // Len still reports 20, but the material ends at 10
			defs := []clipDef{{f: f, start: 0, in: 0, out: 20, duration: 20, gain: 1}}
			tl, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			buf := make([][2]float64, 20)
			n, ok := tl.Stream(buf)
			c.Check(n == 10 && ok, "expected a 10-frame prefix, got %d,%v", n, ok)
			n, ok = tl.Stream(buf[:4])
			c.Check(n == 0 && !ok, "short source must terminate, got %d,%v", n, ok)
			c.Check(tl.Err() != nil, "short source before Out must set Err")
		})

		t.Run("close_is_idempotent", func(t *testing.T) {
			f := newTestFactory(material(16))
			defs := []clipDef{{f: f, start: 0, in: 0, out: 8, duration: 8, gain: 1}}
			tl, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			buf := make([][2]float64, 8)
			tl.Stream(buf)
			c.Check(tl.Close() == nil, "first Close should report no error")
			c.Check(tl.Close() == nil, "second Close should report no error")
			checkClosedOnce(c, "repeated Close closes each reader once", f)
			c.Check(tl.Schedule(clipsOf(defs...), 0, 0) != nil, "closed timeline must reject Schedule")
		})

		t.Run("schedule_rejected_after_termination", func(t *testing.T) {
			sentinel := errors.New("boom")
			f := newTestFactory(material(16))
			f.readErr = sentinel
			f.readErrAt = 2
			defs := []clipDef{{f: f, start: 0, in: 0, out: 8, duration: 8, gain: 1}}
			tl, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			buf := make([][2]float64, 8)
			tl.Stream(buf)
			tl.Stream(buf[:2])
			c.Check(tl.Err() != nil, "timeline should be terminated")
			c.Check(tl.Schedule(clipsOf(defs...), 0, 0) != nil, "terminated timeline must reject Schedule")
		})

		t.Run("cancel_cleanup_error", func(t *testing.T) {
			fOld := newTestFactory(material(32))
			fPend := newTestFactory(material(32))
			closeSentinel := errors.New("cancel close exploded")
			fPend.closeErr = closeSentinel
			old := []clipDef{{f: fOld, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			pend := []clipDef{{f: fPend, start: 0, in: 0, out: 16, duration: 16, gain: 1}}
			tl, err := beep.NewTimeline(8000, clipsOf(old...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			if err := tl.Schedule(clipsOf(pend...), 4, 0); err != nil {
				c.Check(false, "schedule failed: %v", err)
				return
			}
			yes, err := tl.Cancel()
			c.Check(yes, "Cancel should report a removal")
			c.Check(errors.Is(err, closeSentinel), "Cancel must report cleanup errors: %v", err)
			c.Check(tl.Len() == 16, "Cancel should restore the old end")
		})
	})
}

// ---------------------------------------------------------------------------
// TestTimelineExport
// ---------------------------------------------------------------------------

func TestTimelineExport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("encodes_and_round_trips", func(t *testing.T) {
			f1 := newTestFactory(material(48))
			f2 := newTestFactory(material(48))
			defs := []clipDef{
				{f: f1, start: 0, in: 0, out: 12, duration: 12, gain: 0.5},
				{f: f2, start: 12, in: 4, out: 24, duration: 8, gain: 0.5},
			}
			tl, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			var sink writerseeker.WriterSeeker
			format := beep.Format{SampleRate: 8000, NumChannels: 2, Precision: 2}
			err = wav.Encode(&sink, tl, format)
			c.Check(err == nil, "wav.Encode failed: %v", err)

			reader, gotFormat, err := wav.Decode(sink.BytesReader())
			c.Check(err == nil, "wav.Decode failed: %v", err)
			if err != nil {
				return
			}
			defer reader.Close()
			c.Check(gotFormat.NumChannels == 2 && gotFormat.SampleRate == 8000, "unexpected format: %+v", gotFormat)

			var decoded [][2]float64
			buf := make([][2]float64, 7)
			for {
				n, ok := reader.Stream(buf)
				decoded = append(decoded, buf[:n]...)
				if !ok {
					break
				}
			}
			c.Check(len(decoded) == 20, "expected 20 decoded frames, got %d", len(decoded))
			if len(decoded) != 20 {
				return
			}
			const quant = 2.0 / 32768.0
			bad := ""
			for i := range decoded {
				want := expectedAt(defs, i)
				if math.Abs(decoded[i][0]-want[0]) > quant || math.Abs(decoded[i][1]-want[1]) > quant {
					bad = fmt.Sprintf("frame %d got %v want %v", i, decoded[i], want)
					break
				}
			}
			c.Check(bad == "", "decoded audio differs from the timeline formula: %s", bad)
			checkClosedOnce(c, "export closes readers", f1, f2)
		})

		t.Run("erroring_stream_is_not_a_success", func(t *testing.T) {
			sentinel := errors.New("boom during export")
			f := newTestFactory(material(32))
			f.readErr = sentinel
			f.readErrAt = 6
			defs := []clipDef{{f: f, start: 0, in: 0, out: 20, duration: 20, gain: 1}}
			tl, err := beep.NewTimeline(8000, clipsOf(defs...))
			c.Check(err == nil, "construction failed: %v", err)
			if err != nil {
				return
			}
			var sink writerseeker.WriterSeeker
			err = wav.Encode(&sink, tl, beep.Format{SampleRate: 8000, NumChannels: 2, Precision: 2})
			c.Check(err != nil, "wav.Encode must fail for a terminated stream")
			c.Check(errors.Is(err, sentinel), "wav.Encode must preserve the stream error: %v", err)
		})

		t.Run("plain_streamer_still_encodes", func(t *testing.T) {
			var sink writerseeker.WriterSeeker
			format := beep.Format{SampleRate: 8000, NumChannels: 2, Precision: 2}
			source := beep.StreamerFunc(func(samples [][2]float64) (int, bool) {
				for i := range samples {
					samples[i] = [2]float64{0.25, -0.25}
				}
				return len(samples), true
			})
			err := wav.Encode(&sink, beep.Take(300, source), format)
			c.Check(err == nil, "normal encoding must keep working: %v", err)
			r, gotFormat, err := wav.Decode(sink.BytesReader())
			c.Check(err == nil, "normal decode failed: %v", err)
			if err != nil {
				return
			}
			defer r.Close()
			var total int
			buf := make([][2]float64, 64)
			for {
				n, ok := r.Stream(buf)
				total += n
				if !ok {
					break
				}
			}
			c.Check(total == 300, "normal encode length changed: %d", total)
			c.Check(gotFormat.NumChannels == 2, "normal encode channels changed")
		})
	})
}
