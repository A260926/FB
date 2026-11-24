package fauxgl

import (
	"fmt"
	"math"
)

// VolumeGradients holds the adjoint of a weighted sum of the beauty RGB
// and transmittance images with respect to the shared source vertex
// positions (XYZ, per mesh) and the per-medium sigma and j coefficients.
// Camera, instance matrices and the backdrop are treated as fixed.
type VolumeGradients struct {
	Vertex [][]Vector // one XYZ gradient per shared source vertex, per mesh
	Sigma  []Color    // per medium, per channel
	J      []Color    // per medium, per channel
}

// AdjointVolume back-propagates per-pixel color and transmittance
// adjoints through the exact piecewise transport and the captured ray
// intersections. Coverage, event ordering and integer winding branches
// are held fixed; ray/triangle intersection distances, world segment
// lengths and per-segment attenuation are all differentiated. The work
// grows linearly with the record size: no redraws, parameter
// perturbations or automatic-differentiation dependencies.
func AdjointVolume(rec *VolumeRecord, colorAdj, transAdj []Color) (*VolumeGradients, error) {
	if rec == nil {
		return nil, fmt.Errorf("fauxgl: nil volume record")
	}
	n := rec.Width * rec.Height
	if len(rec.Pixels) != n || len(colorAdj) != n || len(transAdj) != n {
		return nil, fmt.Errorf("fauxgl: adjoint buffer size mismatch")
	}
	grad := &VolumeGradients{
		Vertex: make([][]Vector, len(rec.meshVerts)),
		Sigma:  make([]Color, len(rec.Media)),
		J:      make([]Color, len(rec.Media)),
	}
	for mi := range rec.meshVerts {
		grad.Vertex[mi] = make([]Vector, len(rec.meshVerts[mi]))
	}

	for pixel := 0; pixel < n; pixel++ {
		px := rec.Pixels[pixel]
		gC := colorAdj[pixel]
		gT := transAdj[pixel]
		if !isFiniteVector(Vector{gC.R, gC.G, gC.B}) ||
			!isFiniteVector(Vector{gT.R, gT.G, gT.B}) {
			return nil, fmt.Errorf("fauxgl: non-finite adjoint at pixel %d", pixel)
		}
		segs := evaluateVolumePixel(rec, px)
		if len(segs) == 0 {
			continue
		}

		// per-segment adjoint of sigma, j and the segment length.
		gSigma := make([][3]float64, len(segs))
		gJ := make([][3]float64, len(segs))
		gL := make([][3]float64, len(segs))

		for c := 0; c < 3; c++ {
			gc := colorChannel(gC, c)
			gt := colorChannel(gT, c)
			back := colorChannel(px.Backdrop, c)
			// C += T_final * backdrop: the color adjoint enters the
			// final transmittance here.
			adjT := gt + gc*back
			for k := len(segs) - 1; k >= 0; k-- {
				seg := &segs[k]
				sigma := colorChannel(seg.sigma, c)
				j := colorChannel(seg.j, c)
				tIn := seg.tIn[c]
				term := seg.terms[c]

				// C += T_in * j * F
				gF := gc * tIn * j
				gJ[k][c] += gc * tIn * term.f
				// T_out = T_in * E
				gE := adjT * tIn
				adjT = adjT*term.e + gc*j*term.f

				gSigma[k][c] += gE*(-seg.length()*term.e) + gF*term.dFdSigma
				gL[k][c] += gE*(-sigma*term.e) + gF*term.dFdL
			}
		}

		// medium parameter adjoints: segment coefficients are sums over
		// the active instances, so every summand receives the full
		// segment adjoint (overlapping instances, same medium included).
		for k, seg := range segs {
			if seg.length() <= 0 {
				continue
			}
			for _, inst := range seg.instances {
				m := rec.instMedia[inst]
				grad.Sigma[m] = grad.Sigma[m].Add(colorFromChannels(gSigma[k]))
				grad.J[m] = grad.J[m].Add(colorFromChannels(gJ[k]))
			}
		}

		// length adjoints move boundary distances. Every internal piece
		// boundary maps to exactly one driving event; its adjoint is the
		// difference of the length adjoints of the pieces immediately to
		// its left and right (moving a boundary changes both adjacent
		// medium segments). Zero-length pieces between coincident events
		// of different instances are kept, so overlapping volumes keep
		// distinct per-event adjoints. A shell whose two adjacent pieces
		// have identical coefficients yields equal length adjoints and
		// hence zero, so interior shells produce no false gradient.
		end := px.effectiveEnd()
		for k := 0; k+1 < len(segs); k++ {
			ei := segs[k].rightEvent
			if ei < 0 || ei >= len(px.Events) {
				continue
			}
			ev := px.Events[ei]
			// the near plane and the fixed far/backdrop end carry no
			// gradient; boundaries fully outside the active range pin.
			if ev.T <= 0 || ev.T >= end-volEventEps*(1+math.Abs(end)) {
				continue
			}
			var gDistance float64
			for c := 0; c < 3; c++ {
				gDistance += gL[k][c] - gL[k+1][c]
			}
			if gDistance == 0 {
				continue
			}
			accumulateEventGradient(rec, grad, pixel, ev, gDistance)
		}
	}
	return grad, nil
}

func (s *cachedSegment) length() float64 { return s.b - s.a }

func colorChannel(c Color, i int) float64 {
	switch i {
	case 0:
		return c.R
	case 1:
		return c.G
	default:
		return c.B
	}
}

func colorFromChannels(v [3]float64) Color {
	return Color{R: v[0], G: v[1], B: v[2]}
}

// accumulateEventGradient distributes the adjoint of a ray/face
// intersection distance over the face's three world vertices and then
// through the fixed instance affine matrix onto the shared source
// vertices.
//
// For X = O + tD = b0 P0 + b1 P1 + b2 P2 with b0+b1+b2 = 1, implicit
// differentiation of the intersection equations gives
//
//	d t / d P_i = b_i * n / (D . n),  n = (P1-P0) x (P2-P0)
//
// which stays valid for back-facing and mirrored geometry.
func accumulateEventGradient(rec *VolumeRecord, grad *VolumeGradients, pixel int, e VolumeEvent, gDistance float64) {
	inst := e.Instance
	mi := rec.instMesh[inst]
	face := rec.meshFaces[mi][e.Face]
	world := rec.instWorldVerts[inst]
	p0 := world[face[0]]
	p1 := world[face[1]]
	p2 := world[face[2]]
	n := p1.Sub(p0).Cross(p2.Sub(p0))
	denom := rec.rays[pixel].dir.Dot(n)
	if denom == 0 {
		return // tangent event; geometric gradients are not required there
	}
	bary := [3]float64{e.B0, e.B1, e.B2}
	for i := 0; i < 3; i++ {
		gWorld := n.MulScalar(gDistance * bary[i] / denom)
		gSource := transposeLinear(rec.instMatrix[inst], gWorld)
		grad.Vertex[mi][face[i]] = grad.Vertex[mi][face[i]].Add(gSource)
	}
}

// transposeLinear applies the transpose of the linear part of an affine
// matrix to a world-space gradient vector.
func transposeLinear(m Matrix, g Vector) Vector {
	return Vector{
		X: m.X00*g.X + m.X10*g.Y + m.X20*g.Z,
		Y: m.X01*g.X + m.X11*g.Y + m.X21*g.Z,
		Z: m.X02*g.X + m.X12*g.Y + m.X22*g.Z,
	}
}
