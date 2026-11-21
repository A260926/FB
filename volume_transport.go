package fauxgl

import "math"

// VolumeImage is the transport stage output: owned linear RGB and RGB
// transmittance, row-major with Width*Height pixels. Values are raw
// linear quantities: no gamma, no byte quantization, no clamping.
type VolumeImage struct {
	Width, Height int
	Color         []Vector // linear RGB, row-major
	Transmittance []Vector // RGB transmittance, row-major
}

// VolumeSegment is one constant-coefficient piece of a pixel ray with the
// forward intermediates the adjoint stage needs.
type VolumeSegment struct {
	L          float64 // world length of the segment
	Sigma, J   Vector  // summed coefficients of the active instances
	E          Vector  // exp(-Sigma * L) per channel
	F          Vector  // J * (1 - E) / Sigma per channel (continuous limit at Sigma == 0)
	TPrev      Vector  // transmittance at the start of the segment
	Media      []int   // active medium indices, one per active instance
	StartEvent int     // boundary event index into VolumePixelRecord.Events, -1 at the near plane
	EndEvent   int     // boundary event index into VolumePixelRecord.Events, -1 at the depth limit
}

// VolumePixelRecord is the self-contained record of one pixel.
type VolumePixelRecord struct {
	O, D     Vector          // ray origin (near plane) and unit direction
	Bg       Vector          // background linear RGB
	Segments []VolumeSegment // constant-coefficient segments covering [0, TEnd]
	Events   []VolumeEvent   // crossing events referenced by the segments
}

// VolumeRecord owns every value the adjoint stage needs. It is fully
// independent of the scene, the capture and the context buffers: they may
// be modified or released after TransportVolume returns. Pixels are
// row-major: pixel (x, y) is Pixels[y*Width+x].
type VolumeRecord struct {
	Width, Height    int
	MediaCount       int
	MeshVertexCounts []int // vertices per mesh, defines the global vertex indexing
	InstanceMatrix   []Matrix
	Pixels           []VolumePixelRecord
}

// volAt returns channel c (0=R, 1=G, 2=B) of an RGB vector.
func volAt(v Vector, c int) float64 {
	switch c {
	case 0:
		return v.X
	case 1:
		return v.Y
	default:
		return v.Z
	}
}

// volAdd adds x to channel c of an RGB vector.
func volAdd(v *Vector, c int, x float64) {
	switch c {
	case 0:
		v.X += x
	case 1:
		v.Y += x
	default:
		v.Z += x
	}
}

// volOneMinusExp returns 1 - exp(-x), accurate for small x.
func volOneMinusExp(x float64) float64 {
	if math.Abs(x) < 1e-4 {
		return x * (1 - x*(0.5-x*(1.0/6-x*(1.0/24))))
	}
	return 1 - math.Exp(-x)
}

// volG returns (1 - exp(-sigma*l))/sigma with the continuous limit l at
// sigma == 0, stable for tiny positive sigma as well.
func volG(sigma, l float64) float64 {
	x := sigma * l
	if math.Abs(x) < 1e-4 {
		return l * (1 - x*(0.5-x*(1.0/6-x*(1.0/24))))
	}
	return volOneMinusExp(x) / sigma
}

// volGSigma returns d/dsigma of volG with the continuous one-sided limit
// -l*l/2 at sigma == 0, stable for tiny positive sigma as well.
func volGSigma(sigma, l float64) float64 {
	x := sigma * l
	if math.Abs(x) < 1e-4 {
		return l * l * (-0.5 + x*(1.0/3-x*(1.0/8-x*(1.0/30))))
	}
	return (l*math.Exp(-x)*sigma - volOneMinusExp(x)) / (sigma * sigma)
}

// TransportVolume integrates the captured intervals along the world ray
// of every pixel. Each active constant-coefficient segment of length l
// accumulates, per channel, C += T * j * (1 - exp(-sigma*l)) / sigma and
// T *= exp(-sigma*l), where sigma and j are the sums over the active
// instances and the sigma == 0 case uses the continuous limit. Finally
// C += T * background. It returns the owned linear image and a
// self-contained record for AdjointVolume. The context buffers are not
// modified.
func TransportVolume(c *VolumeCapture) (*VolumeImage, *VolumeRecord) {
	w, h := c.Width, c.Height
	npix := w * h
	img := &VolumeImage{
		Width:         w,
		Height:        h,
		Color:         make([]Vector, npix),
		Transmittance: make([]Vector, npix),
	}
	rec := &VolumeRecord{
		Width:            w,
		Height:           h,
		MediaCount:       len(c.Media),
		MeshVertexCounts: append([]int(nil), c.MeshVertexCounts...),
		InstanceMatrix:   append([]Matrix(nil), c.InstanceMatrix...),
		Pixels:           make([]VolumePixelRecord, npix),
	}
	for i := 0; i < npix; i++ {
		px := &c.Pixels[i]
		rp := &rec.Pixels[i]
		rp.O = px.O
		rp.D = px.D
		rp.Bg = px.Bg
		if len(px.Events) > 0 {
			rp.Events = append([]VolumeEvent(nil), px.Events...)
		}
		T := Vector{1, 1, 1}
		var C Vector
		for _, iv := range px.Intervals {
			var sigma, j Vector
			var media []int
			for _, inst := range iv.Instances {
				m := c.InstanceMedium[inst]
				media = append(media, m)
				sigma = sigma.Add(c.Media[m].Sigma)
				j = j.Add(c.Media[m].J)
			}
			l := iv.T1 - iv.T0
			var e, f Vector
			for ch := 0; ch < 3; ch++ {
				s := volAt(sigma, ch)
				volAdd(&e, ch, math.Exp(-s*l))
				volAdd(&f, ch, volAt(j, ch)*volG(s, l))
			}
			C = C.Add(T.Mul(f))
			rp.Segments = append(rp.Segments, VolumeSegment{
				L:          l,
				Sigma:      sigma,
				J:          j,
				E:          e,
				F:          f,
				TPrev:      T,
				Media:      media,
				StartEvent: iv.StartEvent,
				EndEvent:   iv.EndEvent,
			})
			T = T.Mul(e)
		}
		C = C.Add(T.Mul(px.Bg))
		img.Color[i] = C
		img.Transmittance[i] = T
	}
	return img, rec
}
