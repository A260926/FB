package beep

import (
	"errors"
	"fmt"
	"math"
	"reflect"
)

var (
	errTimelineStopped      = errors.New("beep: timeline has been stopped")
	errTimelineFading       = errors.New("beep: timeline cannot schedule during a fade")
	errTimelineBeforePos    = errors.New("beep: timeline schedule position is before the current position")
	errTimelineSourceShort  = errors.New("beep: timeline source drained before Out")
	errTimelineNilReader    = errors.New("beep: timeline clip Open returned a nil reader")
	errTimelineClosedReader = errors.New("beep: timeline reader has been closed")
)

// timelineClipSpec is a validated, copied description of a TimelineClip. The
// caller's slice is never retained.
type timelineClipSpec struct {
	open     func() (StreamSeekCloser, error)
	start    int
	in       int
	out      int
	duration int
	gain     float64
}

// timelineCheckedAdd adds two non-negative ints and reports whether the result
// fits in an int.
func timelineCheckedAdd(a, b int) (int, bool) {
	if a < 0 || b < 0 {
		return 0, false
	}
	maxInt := int(^uint(0) >> 1)
	if a > maxInt-b {
		return 0, false
	}
	return a + b, true
}

// joinErrors is like errors.Join but returns the only error unchanged when
// there is exactly one. This keeps caller-supplied errors directly comparable
// in the common single-error case while still preserving errors.Is identity.
func joinErrors(errs []error) error {
	var nonNil []error
	for _, err := range errs {
		if err != nil {
			nonNil = append(nonNil, err)
		}
	}
	switch len(nonNil) {
	case 0:
		return nil
	case 1:
		return nonNil[0]
	default:
		return errors.Join(nonNil...)
	}
}

// sameReader reports whether two readers are the same instance. It compares
// underlying pointers so it never panics on a non-comparable dynamic type.
func sameReader(a, b StreamSeekCloser) bool {
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	va, vb := reflect.ValueOf(a), reflect.ValueOf(b)
	if va.Kind() != reflect.Pointer || vb.Kind() != reflect.Pointer {
		return false
	}
	if va.Type() != vb.Type() {
		return false
	}
	return va.Pointer() == vb.Pointer()
}

// validateTimelineClips validates the whole table and copies it. No reader is
// opened until the complete table has passed validation.
func validateTimelineClips(clips []TimelineClip) ([]timelineClipSpec, int, error) {
	if len(clips) == 0 {
		return nil, 0, errors.New("beep: timeline requires at least one clip")
	}

	specs := make([]timelineClipSpec, len(clips))
	length := 0
	for i, clip := range clips {
		if clip.Open == nil {
			return nil, 0, fmt.Errorf("beep: timeline clip %d: Open is nil", i)
		}
		if clip.Start < 0 {
			return nil, 0, fmt.Errorf("beep: timeline clip %d: Start is negative", i)
		}
		if clip.In < 0 {
			return nil, 0, fmt.Errorf("beep: timeline clip %d: In is negative", i)
		}
		if clip.Out <= clip.In {
			return nil, 0, fmt.Errorf("beep: timeline clip %d: Out must be greater than In", i)
		}
		if clip.Duration <= 0 {
			return nil, 0, fmt.Errorf("beep: timeline clip %d: Duration must be positive", i)
		}
		if math.IsNaN(clip.Gain) || math.IsInf(clip.Gain, 0) || clip.Gain < 0 {
			return nil, 0, fmt.Errorf("beep: timeline clip %d: Gain must be finite and non-negative", i)
		}
		end, ok := timelineCheckedAdd(clip.Start, clip.Duration)
		if !ok {
			return nil, 0, fmt.Errorf("beep: timeline clip %d: Start+Duration overflows", i)
		}
		if end > length {
			length = end
		}
		specs[i] = timelineClipSpec{
			open:     clip.Open,
			start:    clip.Start,
			in:       clip.In,
			out:      clip.Out,
			duration: clip.Duration,
			gain:     clip.Gain,
		}
	}
	return specs, length, nil
}

// timelineSourceAt returns the material frame that corresponds to output frame
// at for spec. Before the clip it is In, after the clip it is Out, and inside
// the clip it is the floor of the absolute retiming formula.
func timelineSourceAt(spec timelineClipSpec, at int) int {
	if at < spec.start {
		return spec.in
	}
	k := at - spec.start
	if k >= spec.duration {
		return spec.out
	}
	pos := int(float64(spec.in) + float64(k)*float64(spec.out-spec.in)/float64(spec.duration))
	if pos < spec.in {
		pos = spec.in
	}
	if pos > spec.out-1 {
		pos = spec.out - 1
	}
	return pos
}

// prepareTimelineRevision validates ownership of every reader for one whole
// generation. Each reader comes from its own Open call, is checked, and is
// positioned at the material frame corresponding to output frame at. If
// anything fails, every reader opened so far is closed exactly once.
func prepareTimelineRevision(specs []timelineClipSpec, length, at int) (*timelineRevision, error) {
	rev := &timelineRevision{
		length: length,
		voices: make([]timelineVoice, 0, len(specs)),
	}
	var last StreamSeekCloser
	for i, spec := range specs {
		r, err := spec.open()
		if err != nil {
			var closeErr error
			if r != nil {
				closeErr = r.Close()
			}
			return nil, joinErrors([]error{err, closeErr, rev.close()})
		}
		if r == nil {
			return nil, joinErrors([]error{errTimelineNilReader, rev.close()})
		}
		if last != nil && sameReader(r, last) {
			return nil, joinErrors([]error{
				fmt.Errorf("beep: timeline clip %d: Open returned a shared reader", i),
				rev.close(),
			})
		}
		last = r
		rev.voices = append(rev.voices, timelineVoice{
			r:        r,
			start:    spec.start,
			in:       spec.in,
			out:      spec.out,
			duration: spec.duration,
			gain:     spec.gain,
			cacheSrc: -1,
		})
		if r.Len() < spec.out {
			return nil, joinErrors([]error{
				fmt.Errorf("beep: timeline clip %d: reader length %d is less than Out %d", i, r.Len(), spec.out),
				rev.close(),
			})
		}
		if err := r.Seek(timelineSourceAt(spec, at)); err != nil {
			return nil, joinErrors([]error{err, rev.close()})
		}
	}
	return rev, nil
}

// timelineRevision is one complete arrangement. Every voice owns an
// independent reader, cursor and interpolation cache.
type timelineRevision struct {
	voices []timelineVoice
	length int
}

// close closes every reader owned by the revision exactly once.
func (rev *timelineRevision) close() error {
	if rev == nil {
		return nil
	}
	var errs []error
	for i := range rev.voices {
		if err := rev.voices[i].closeOnce(); err != nil {
			errs = append(errs, err)
		}
	}
	return joinErrors(errs)
}

// sampleAt renders the revision at absolute output frame f without clipping.
func (rev *timelineRevision) sampleAt(f int) ([2]float64, error) {
	var sum [2]float64
	for i := range rev.voices {
		voice := &rev.voices[i]
		k := f - voice.start
		if k < 0 || k >= voice.duration {
			continue
		}
		sample, err := voice.sampleAtOffset(k)
		if err != nil {
			return [2]float64{}, err
		}
		sum[0] += sample[0]
		sum[1] += sample[1]
	}
	return sum, nil
}

// timelineVoice is one clip inside one generation.
type timelineVoice struct {
	r        StreamSeekCloser
	start    int // output frame where the clip begins
	in       int // first material frame
	out      int // one past the last material frame
	duration int // number of output frames
	gain     float64

	// A single-frame interpolation cache. The cache is per voice, so voices
	// never affect each other's source cursors.
	cacheSrc int
	cacheVal [2]float64
	cached   bool

	closed bool
}

func (v *timelineVoice) closeOnce() error {
	if v.closed {
		return nil
	}
	v.closed = true
	if v.r == nil {
		return nil
	}
	return v.r.Close()
}

// sampleAtOffset renders output offset k inside the clip. The material
// position is always computed from the absolute formula, never accumulated
// from read-block sizes.
func (v *timelineVoice) sampleAtOffset(k int) ([2]float64, error) {
	if k < 0 || k >= v.duration {
		return [2]float64{}, nil
	}

	pos := float64(v.in) + float64(k)*float64(v.out-v.in)/float64(v.duration)
	if pos < float64(v.in) {
		pos = float64(v.in)
	}
	leftSrc := int(pos)
	if leftSrc > v.out-1 {
		leftSrc = v.out - 1
	}
	frac := pos - float64(leftSrc)

	left, err := v.sampleAtSource(leftSrc)
	if err != nil {
		return [2]float64{}, err
	}

	sample := left
	if frac != 0 {
		rightSrc := leftSrc + 1
		if rightSrc > v.out-1 {
			rightSrc = v.out - 1
		}
		if rightSrc != leftSrc {
			right, err := v.sampleAtSource(rightSrc)
			if err != nil {
				return [2]float64{}, err
			}
			sample[0] = left[0]*(1-frac) + right[0]*frac
			sample[1] = left[1]*(1-frac) + right[1]*frac
		}
	}

	sample[0] *= v.gain
	sample[1] *= v.gain
	return sample, nil
}

// sampleAtSource reads a single material frame. Reads never leave [In, Out):
// the right interpolation neighbour is clamped to Out-1 by the caller.
func (v *timelineVoice) sampleAtSource(src int) ([2]float64, error) {
	if v.cached && v.cacheSrc == src {
		return v.cacheVal, nil
	}
	if v.closed {
		return [2]float64{}, errTimelineClosedReader
	}
	if err := v.r.Seek(src); err != nil {
		return [2]float64{}, err
	}
	var buf [1][2]float64
	n, _ := v.r.Stream(buf[:])
	if n < 1 {
		if err := v.r.Err(); err != nil {
			return [2]float64{}, err
		}
		return [2]float64{}, errTimelineSourceShort
	}
	v.cached = true
	v.cacheSrc = src
	v.cacheVal = buf[0]
	return buf[0], nil
}
