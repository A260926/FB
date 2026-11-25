package beep

import (
	"fmt"
)

// TimelineClip describes one clip of a Timeline arrangement.
//
// Start and Duration are measured in output frames, while In and Out are
// measured in source frames. Every interval is half-open: a clip occupies
// output frames [Start, Start+Duration) and reads material frames [In, Out).
//
// Open must return a fresh StreamSeekCloser each time it is called. The
// returned reader belongs to the Timeline generation that called Open, must
// support seeking, and must report a length of at least Out. Returned readers
// must never be shared between calls.
type TimelineClip struct {
	// Open creates the source reader for this clip. It must not be nil.
	Open func() (StreamSeekCloser, error)
	// Start is the output frame where the clip begins.
	Start int
	// In is the first material frame read by the clip.
	In int
	// Out is one past the last material frame read by the clip.
	Out int
	// Duration is the number of output frames the clip occupies.
	Duration int
	// Gain is the non-negative, finite multiplier applied to the clip.
	Gain float64
}

// Timeline is a Streamer that plays a multi-clip arrangement and can prepare a
// replacement arrangement without interrupting playback.
//
// A replacement is prepared with Schedule and takes effect at a chosen output
// frame, optionally through a short cross-fade. Preparation happens before
// anything becomes audible: either the whole new generation is ready, or the
// previously audible generation and any pending generation are left untouched.
//
// "Pending" refers to a prepared arrangement that has not reached its switch
// frame yet. "Audible" refers to the generation currently being streamed.
//
// An error while preparing a replacement is returned by Schedule and leaves the
// Timeline unchanged. An error while closing the generation that Schedule
// replaced is also returned by Schedule, but the new generation has already
// been installed in that case. The two cases are distinct: a preparation error
// means nothing changed, while a cleanup error means the replacement happened.
type Timeline struct {
	rate SampleRate
	pos  int
	err  error

	// pendingErr holds an error that occurred after this Stream call had
	// already produced PCM. It is made public only on the next non-empty
	// Stream call.
	pendingErr error
	done       bool
	closed     bool

	cur    *timelineRevision
	pend   *timelineRevision
	pendAt int
	pendF  int

	// Cross-fade bookkeeping. A fade is active from the first frame at the
	// switch point until its final frame has been delivered.
	fadeActive bool
	fadeAt     int
	fadeLen    int
	fadeLeft   int
	old        *timelineRevision
}

// NewTimeline validates the whole clip table, then opens and seeks one reader
// per clip. If any open or seek fails, all readers opened during that attempt
// are closed and the error is returned.
//
// rate must be positive and clips must be non-empty. The caller's slice is
// copied; later mutations of it do not affect the Timeline.
func NewTimeline(rate SampleRate, clips []TimelineClip) (*Timeline, error) {
	if rate <= 0 {
		return nil, fmt.Errorf("beep: timeline sample rate must be positive, got %d", rate)
	}
	specs, length, err := validateTimelineClips(clips)
	if err != nil {
		return nil, err
	}
	cur, err := prepareTimelineRevision(specs, length, 0)
	if err != nil {
		return nil, err
	}
	return &Timeline{
		rate: rate,
		cur:  cur,
	}, nil
}

// Position returns the number of output frames streamed so far.
func (t *Timeline) Position() int {
	return t.pos
}

// Len returns the output frame at which the audible generation ends. While a
// scheduled replacement is waiting, Len already reports the replacement's end.
func (t *Timeline) Len() int {
	if t.pend != nil {
		return t.pend.length
	}
	return t.cur.length
}

// Err returns the error that terminated the Timeline, or nil.
func (t *Timeline) Err() error {
	return t.err
}

// stopped reports whether the Timeline no longer accepts new schedules.
func (t *Timeline) stopped() bool {
	return t.err != nil || t.pendingErr != nil || t.done || t.closed
}

// Schedule prepares a replacement arrangement and makes it pending. It takes
// effect at output frame at. If fade is greater than zero, the first fade frames
// starting at at cross-fade from the current generation; the frame after the
// fade uses the new generation alone.
//
// at must not be before Position and must be inside both the old and the new
// generation. fade must not be negative, and at+fade must not exceed either
// generation's end. Only one generation may be pending at a time.
//
// The new generation is prepared completely before anything changes. If
// preparation fails, the audible generation, any existing pending generation,
// and all of their cursors, read-ahead samples, plans and lengths are left
// untouched. Only after a successful replacement is the previously pending
// generation closed; a cleanup error is returned separately from a preparation
// error, without resurrecting the replaced generation.
//
// Schedule returns an error for a stopped or closed Timeline, and while a
// cross-fade is in progress.
func (t *Timeline) Schedule(clips []TimelineClip, at, fade int) error {
	if t.stopped() {
		return errTimelineStopped
	}
	if t.fadeActive {
		return errTimelineFading
	}
	if fade < 0 {
		return fmt.Errorf("beep: timeline fade must be non-negative, got %d", fade)
	}
	if at < 0 || at < t.pos {
		return errTimelineBeforePos
	}

	specs, newLen, err := validateTimelineClips(clips)
	if err != nil {
		return err
	}
	if at >= newLen {
		return fmt.Errorf("%w: at %d is not inside new length %d", errTimelineBeforePos, at, newLen)
	}
	if at >= t.cur.length {
		return fmt.Errorf("%w: at %d is not inside current length %d", errTimelineBeforePos, at, t.cur.length)
	}
	fadeEnd, ok := timelineCheckedAdd(at, fade)
	if !ok || fadeEnd > newLen {
		return fmt.Errorf("beep: timeline at+fade exceeds the new generation")
	}
	if fadeEnd > t.cur.length {
		return fmt.Errorf("beep: timeline at+fade exceeds the current generation")
	}

	prepared, err := prepareTimelineRevision(specs, newLen, at)
	if err != nil {
		return err
	}

	// Preparation is complete; from here on the new generation is installed.
	// Only now may the replaced pending generation be closed.
	replaced := t.pend
	t.pend = prepared
	t.pendAt = at
	t.pendF = fade
	if replaced != nil {
		if err := replaced.close(); err != nil {
			return err
		}
	}
	return nil
}

// Cancel removes a pending replacement before it becomes audible.
//
// It returns true and any cleanup errors if there was a pending replacement to
// remove. It returns false and nil when no replacement is pending. Cancel also
// returns false and nil once a cross-fade has begun, so the switch cannot be
// undone from the first faded frame onward. Cancelling before the switch frame
// has been output is allowed, including when Position has already reached at.
func (t *Timeline) Cancel() (bool, error) {
	if t.err != nil || t.pend == nil || t.fadeActive {
		return false, nil
	}
	pend := t.pend
	t.pend = nil
	t.pendAt = 0
	t.pendF = 0
	return true, pend.close()
}

// Stream renders output frames from the audible generation and, once the
// switch frame is reached, from the replacement generation.
//
// A zero-length buffer never advances the Timeline, never opens a source, and
// never switches generations; it returns 0, true while the Timeline has not
// stopped and 0, false after it has. Reaching the normal end of the arrangement
// releases both generations and later calls return 0, false.
func (t *Timeline) Stream(samples [][2]float64) (n int, ok bool) {
	if len(samples) == 0 {
		if t.err != nil || t.pendingErr != nil || t.done || t.closed {
			return 0, false
		}
		return 0, true
	}
	if t.err != nil || t.done || t.closed {
		return 0, false
	}
	if t.pendingErr != nil {
		// A previous call already returned its PCM prefix; now publish the
		// error and become drained.
		err := t.pendingErr
		t.pendingErr = nil
		t.fail(err)
		return 0, false
	}

	for n < len(samples) {
		if t.pend != nil && !t.fadeActive && t.pos == t.pendAt {
			if err := t.beginFade(); err != nil {
				if n > 0 {
					t.pendingErr = err
					return n, true
				}
				t.fail(err)
				return 0, false
			}
		}

		cur := t.cur
		f := t.pos

		// Determine how many whole frames this inner pass may cover. A waiting
		// replacement stops the pass exactly at its switch frame so a single
		// Stream call can cross the switch and a cross-fade. During a fade the
		// pass stops at the fade boundary so the weight and the old/new mixing
		// stay consistent.
		maxEnd := f + (len(samples) - n)
		if t.pend != nil && !t.fadeActive && t.pendAt < maxEnd {
			maxEnd = t.pendAt
		}
		if t.fadeActive && maxEnd > t.fadeAt+t.fadeLen {
			maxEnd = t.fadeAt + t.fadeLen
		}
		if maxEnd <= f {
			// The fade boundary is at the cursor; the old generation was
			// already released after its last frame, so continue with the new
			// generation alone.
			t.fadeActive = false
			maxEnd = f + 1
		}

		for frame := f; frame < maxEnd; frame++ {
			if frame >= cur.length {
				break
			}
			curSample, err := cur.sampleAt(frame)
			if err != nil {
				if n > 0 {
					t.pendingErr = err
					return n, true
				}
				t.fail(err)
				return 0, false
			}

			var out [2]float64
			if t.fadeActive && frame < t.fadeAt+t.fadeLen {
				pos := frame - t.fadeAt
				if pos < 0 {
					pos = 0
				}
				weight := float64(pos+1) / float64(t.fadeLen+1)
				oldSample, err := t.old.sampleAt(frame)
				if err != nil {
					if n > 0 {
						t.pendingErr = err
						return n, true
					}
					t.fail(err)
					return 0, false
				}
				out[0] = oldSample[0]*(1-weight) + curSample[0]*weight
				out[1] = oldSample[1]*(1-weight) + curSample[1]*weight
				t.fadeLeft--
				if t.fadeLeft == 0 {
					// The old generation is no longer needed after this frame.
					t.fadeActive = false
					old := t.old
					t.old = nil
					if err := old.close(); err != nil {
						if n > 0 {
							t.pendingErr = err
							return n, true
						}
						t.fail(err)
						return 0, false
					}
				}
			} else {
				out = curSample
			}

			samples[n] = out
			n++
			t.pos = frame + 1
		}

		if t.pos >= cur.length {
			// Normal EOF: release both generations.
			if err := t.closeRevisions(); err != nil {
				if n > 0 {
					t.pendingErr = err
					return n, true
				}
				t.fail(err)
				return 0, false
			}
			t.done = true
			return n, true
		}
		if t.pos <= f {
			// Defensive: guarantee forward progress.
			break
		}
	}
	return n, true
}

// beginFade switches t.cur to the pending generation. With no fade the old
// generation is released immediately; with a fade it is kept for the
// cross-fade and released after its last needed frame.
func (t *Timeline) beginFade() error {
	old := t.cur
	t.cur = t.pend
	t.pend = nil
	t.fadeAt = t.pendAt
	t.fadeLen = t.pendF
	if t.pendF <= 0 {
		t.fadeActive = false
		t.fadeLeft = 0
		return old.close()
	}
	t.fadeActive = true
	t.fadeLeft = t.pendF
	t.old = old
	return nil
}

// fail records err and releases every generation exactly once. Errors raised
// while releasing readers are kept alongside the original error so Err still
// reports them (and preserves errors.Is identity for the original cause).
func (t *Timeline) fail(err error) {
	var errs []error
	if err != nil {
		errs = append(errs, err)
	}
	if closeErr := t.closeRevisions(); closeErr != nil {
		errs = append(errs, closeErr)
	}
	joined := joinErrors(errs)
	if t.err == nil {
		t.err = joined
	}
}

// closeRevisions closes the audible, pending and fading-out generations, each
// reader exactly once. The reported error is the first cleanup error.
func (t *Timeline) closeRevisions() error {
	var errs []error
	if t.cur != nil {
		errs = append(errs, t.cur.close())
	}
	if t.pend != nil {
		errs = append(errs, t.pend.close())
	}
	if t.old != nil {
		errs = append(errs, t.old.close())
	}
	return joinErrors(errs)
}

// Close releases all readers. It may be called repeatedly; readers are closed
// at most once. Close does not resurrect a generation that has already been
// switched out.
func (t *Timeline) Close() error {
	err := t.closeRevisions()
	t.cur = nil
	t.pend = nil
	t.old = nil
	t.pendAt = 0
	t.pendF = 0
	t.fadeActive = false
	t.closed = true
	return err
}
