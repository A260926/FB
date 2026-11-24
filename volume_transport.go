package fauxgl

import (
	"fmt"
	"math"
)

// VolumeResult holds an owning linear-RGB beauty image and the per
// channel transmittance of every pixel. No gamma, byte quantization or
// negative clamping is applied.
type VolumeResult struct {
	Width, Height int
	Color         []Color
	Transmission  []Color
}

// volumeSegment is one constant-coefficient piece of a ray: the world
// interval [a, b], the instances whose winding-number interior is
// active across it, and the event creating b (-1 for a fixed near, far
// or backdrop boundary).
type volumeSegment struct {
	a, b       float64
	instances  []int
	rightEvent int
}

// effectiveEnd returns the world distance up to which transport is
// integrated: the far plane, or a nearer opaque backdrop.
func (px VolumePixel) effectiveEnd() float64 {
	end := px.Far
	if px.HasBack && px.BackDepth < end {
		end = px.BackDepth
	}
	if end < 0 {
		end = 0
	}
	return end
}

// buildVolumeSegments sweeps the captured winding events. The opening
// winding (crossings behind the near plane) is taken from the record so
// a medium cut by the near plane keeps its opening interval; events past
// the effective end do not create intervals. An instance is active while
// its winding is a nonzero integer, so same-oriented nested shells form
// one interior and extra interior shells only split segments at
// unchanged coefficients.
func buildVolumeSegments(rec *VolumeRecord, px VolumePixel) []volumeSegment {
	end := px.effectiveEnd()
	winding := make([]int, len(rec.instMedia))
	events := px.Events

	// crossings before the near plane establish the opening state
	for _, ow := range px.opening {
		winding[ow[0]] += ow[1]
	}

	snapshot := func() []int {
		var active []int
		for inst, w := range winding {
			if w != 0 {
				active = append(active, inst)
			}
		}
		return active
	}

	var segments []volumeSegment
	cur := 0.0
	for i := 0; i < len(events) && events[i].T <= end; {
		e := events[i]
		if e.T >= cur {
			segments = append(segments, volumeSegment{cur, e.T, snapshot(), i})
			cur = e.T
		}
		winding[e.Instance] += e.Winding
		i++
	}
	segments = append(segments, volumeSegment{cur, end, snapshot(), -1})
	return segments
}

// segmentCoeffs sums extinction and emission over the active instances.
// Overlapping instances add, even when they reference the same medium.
func (rec *VolumeRecord) segmentCoeffs(seg volumeSegment) (sigma, j Color) {
	for _, inst := range seg.instances {
		m := rec.instMedia[inst]
		ms := rec.Media[m]
		sigma = sigma.Add(ms.Sigma)
		j = j.Add(ms.J)
	}
	return sigma, j
}

// volHF evaluates h(x) = (1-e^-x)/x and its derivative, with Taylor
// branches for small x so zero and tiny extinctions stay stable.
func volHF(x float64) (h, hp float64) {
	if math.Abs(x) < 1e-4 {
		// h(x)  = 1 - x/2 + x²/6 - x³/24 + x⁴/120
		// h'(x) = -1/2 + x/6 - x²/24 + x³/120 - x⁴/720
		x2 := x * x
		h = 1 - x/2 + x2/6 - x2*x/24 + x2*x2/120
		hp = -0.5 + x/6 - x2/24 + x2*x/120 - x2*x2/720
		return h, hp
	}
	e := math.Exp(-x)
	h = (1 - e) / x
	hp = (e*(x+1) - 1) / (x * x)
	return h, hp
}

// volSegmentTerms holds the stable per-channel transport terms of one
// segment: E = exp(-sigma*l), F = (1-E)/sigma (F = l at sigma = 0),
// dF/dsigma and dF/dl, each including its continuous zero-extinction
// limit.
type volSegmentTerms struct {
	e, f, dFdSigma, dFdL float64
}

func volSegmentTerms1(sigma, l float64) volSegmentTerms {
	if sigma == 0 {
		return volSegmentTerms{1, l, -0.5 * l * l, 1}
	}
	x := sigma * l
	h, hp := volHF(x)
	f := l * h
	e := math.Exp(-x)
	return volSegmentTerms{
		e:        e,
		f:        f,
		dFdSigma: l * l * hp,
		dFdL:     e, // dF/dl = exp(-sigma*l), including the sigma = 0 limit
	}
}

// cachedSegment is a segment together with every term the forward
// evaluation and its adjoint need, so both paths share one derivation.
type cachedSegment struct {
	a, b       float64
	instances  []int
	rightEvent int
	sigma, j   Color
	terms      [3]volSegmentTerms
	tIn        [3]float64 // transmittance entering the segment
	tOut       [3]float64 // transmittance leaving the segment
}

// evaluateVolumePixel runs the piecewise forward integration for one
// pixel and caches the per-segment terms used by the adjoint.
func evaluateVolumePixel(rec *VolumeRecord, px VolumePixel) []cachedSegment {
	raw := buildVolumeSegments(rec, px)
	segs := make([]cachedSegment, len(raw))
	tIn := [3]float64{1, 1, 1}
	for k, seg := range raw {
		sigma, j := rec.segmentCoeffs(seg)
		l := seg.b - seg.a
		var terms [3]volSegmentTerms
		tOut := tIn
		if l > 0 {
			s := [3]float64{sigma.R, sigma.G, sigma.B}
			for c := 0; c < 3; c++ {
				terms[c] = volSegmentTerms1(s[c], l)
				tOut[c] = tIn[c] * terms[c].e
			}
		} else {
			for c := 0; c < 3; c++ {
				terms[c] = volSegmentTerms{1, 0, 0, 1}
			}
		}
		segs[k] = cachedSegment{
			a: seg.a, b: seg.b, instances: seg.instances,
			rightEvent: seg.rightEvent,
			sigma:      sigma, j: j, terms: terms, tIn: tIn, tOut: tOut,
		}
		tIn = tOut
	}
	return segs
}

// SolveVolume integrates exact piecewise homogeneous transport along
// every captured ray, from the near plane to the backdrop or far plane,
// using true world distances. For each active segment of length l:
//
//	C += T * j * (1-exp(-sigma*l))/sigma
//	T  *= exp(-sigma*l)
//
// followed by C += T * backdrop. With no active medium the beauty image
// is exactly the backdrop and the transmittance is exactly one.
func SolveVolume(rec *VolumeRecord) (*VolumeResult, error) {
	if rec == nil {
		return nil, fmt.Errorf("fauxgl: nil volume record")
	}
	if rec.Width <= 0 || rec.Height <= 0 || len(rec.Pixels) != rec.Width*rec.Height {
		return nil, fmt.Errorf("fauxgl: malformed volume record")
	}
	n := rec.Width * rec.Height
	result := &VolumeResult{
		Width:        rec.Width,
		Height:       rec.Height,
		Color:        make([]Color, n),
		Transmission: make([]Color, n),
	}
	for pixel := 0; pixel < n; pixel++ {
		px := rec.Pixels[pixel]
		segs := evaluateVolumePixel(rec, px)
		color := Color{A: 1}
		trans := Color{R: 1, G: 1, B: 1, A: 1}
		for _, seg := range segs {
			l := seg.b - seg.a
			if l <= 0 {
				continue
			}
			j := [3]float64{seg.j.R, seg.j.G, seg.j.B}
			c := [3]float64{color.R, color.G, color.B}
			t := [3]float64{trans.R, trans.G, trans.B}
			for ch := 0; ch < 3; ch++ {
				c[ch] += seg.tIn[ch] * j[ch] * seg.terms[ch].f
				t[ch] = seg.tOut[ch]
			}
			color = Color{R: c[0], G: c[1], B: c[2], A: 1}
			trans = Color{R: t[0], G: t[1], B: t[2], A: 1}
		}
		color = color.Add(trans.Mul(px.Backdrop))
		color.A = 1
		result.Color[pixel] = color
		result.Transmission[pixel] = trans
	}
	return result, nil
}
