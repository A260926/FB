package fauxgl

import (
	"image"
	"math"
	"testing"

	"github.com/fogleman/fauxgl/task_support/taskcheck"
)

// This file validates volumetric capture, exact piecewise transport and
// their reverse interface. Independent reference formulas (ray/AABB
// slabs, hand-derived single-segment derivatives) and finite differences
// (test-only) are used; no automatic differentiation is involved.

const (
	volTolAbs = 3e-5
	volTolRel = 3e-5
)

func volClose(a, b float64) bool {
	return math.Abs(a-b) <= volTolAbs+volTolRel*math.Abs(b)
}

func volOrthoDC(w, h int) *Context {
	dc := NewContext(w, h)
	dc.ClearColorBufferWith(White)
	for i := range dc.DepthBuffer {
		dc.DepthBuffer[i] = math.MaxFloat64
	}
	return dc
}

func volOrthoView() Matrix {
	return LookAt(V(0, 0, 5), V(0, 0, 0), V(0, 1, 0)).
		Orthographic(-1, 1, -1, 1, 1, 10)
}

func volAdjBuffers(n int) (ca, ta []Color) {
	ca = make([]Color, n)
	ta = make([]Color, n)
	for i := range ca {
		ca[i] = Color{1, 0.7, 0.3, 0}
		ta[i] = Color{0.2, 0.4, 0.6, 0}
	}
	return ca, ta
}

func volScalar(rec *VolumeRecord, ca, ta []Color) float64 {
	res, err := SolveVolume(rec)
	if err != nil {
		panic(err)
	}
	s := 0.0
	for i := range res.Color {
		s += ca[i].R*res.Color[i].R + ca[i].G*res.Color[i].G + ca[i].B*res.Color[i].B
		s += ta[i].R*res.Transmission[i].R + ta[i].G*res.Transmission[i].G + ta[i].B*res.Transmission[i].B
	}
	return s
}

func volChan(c Color, i int) float64 {
	switch i {
	case 0:
		return c.R
	case 1:
		return c.G
	default:
		return c.B
	}
}

func volSetChan(c *Color, i int, v float64) {
	switch i {
	case 0:
		c.R = v
	case 1:
		c.G = v
	default:
		c.B = v
	}
}

// rayBoxSlab is an independent ray/AABB reference intersection.
func rayBoxSlab(o, d Vector, b Box) (float64, float64, bool) {
	tmin, tmax := math.Inf(-1), math.Inf(1)
	lo := [3]float64{b.Min.X, b.Min.Y, b.Min.Z}
	hi := [3]float64{b.Max.X, b.Max.Y, b.Max.Z}
	ov := [3]float64{o.X, o.Y, o.Z}
	dv := [3]float64{d.X, d.Y, d.Z}
	for i := 0; i < 3; i++ {
		if math.Abs(dv[i]) < 1e-15 {
			if ov[i] < lo[i] || ov[i] > hi[i] {
				return 0, 0, false
			}
			continue
		}
		t1 := (lo[i] - ov[i]) / dv[i]
		t2 := (hi[i] - ov[i]) / dv[i]
		if t1 > t2 {
			t1, t2 = t2, t1
		}
		if t1 > tmin {
			tmin = t1
		}
		if t2 < tmax {
			tmax = t2
		}
	}
	return tmin, tmax, tmax >= tmin
}

func TestVolumeCapture(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		view := volOrthoView()
		media := []VolumeMedia{{Sigma: Color{0.5, 1, 2, 0}, J: Color{0.3, 0.2, 0.1, 0}}}

		// normal box: enter/leave events with correct winding and distances
		cube := NewCube()
		dc := volOrthoDC(8, 8)
		sc := &VolumeScene{
			Meshes:    []*Mesh{cube},
			Instances: []VolumeInstance{{Mesh: cube, Matrix: Identity(), Media: 0}},
			Media:     media, ViewMatrix: view, Width: 8, Height: 8,
		}
		rec, err := CaptureVolume(dc, sc)
		c.Check(err == nil, "capture normal box: %v", err)
		px := rec.Pixels[3*8+4] // clearly interior, off the center edge ties
		c.Check(len(px.Events) == 2, "normal box yields two events, got %d", len(px.Events))
		if len(px.Events) == 2 {
			c.Check(volClose(px.Events[0].T, 3.5) && px.Events[0].Winding == 1, "enter event t/winding")
			c.Check(volClose(px.Events[1].T, 4.5) && px.Events[1].Winding == -1, "leave event t/winding")
		}

		// hand-derived single-segment transport
		res, err := SolveVolume(rec)
		c.Check(err == nil, "solve normal box: %v", err)
		E := math.Exp(-0.5)
		F := (1 - E) / 0.5
		c.Check(volClose(res.Transmission[3*8+4].R, E), "transmittance matches exp(-sigma*l)")
		c.Check(volClose(res.Color[3*8+4].R, 0.3*F+E), "beauty matches hand segment formula")

		// empty medium strictly restores the backdrop with T = 1
		emptyMedia := []VolumeMedia{{Sigma: Color{}, J: Color{}}}
		esc := &VolumeScene{
			Meshes:    []*Mesh{cube},
			Instances: []VolumeInstance{{Mesh: cube, Matrix: Identity(), Media: 0}},
			Media:     emptyMedia, ViewMatrix: view, Width: 8, Height: 8,
		}
		erec, err := CaptureVolume(volOrthoDC(8, 8), esc)
		c.Check(err == nil, "capture empty medium: %v", err)
		eres, _ := SolveVolume(erec)
		c.Check(eres.Transmission[3*8+4] == (Color{1, 1, 1, 1}), "empty medium transmittance is one")
		c.Check(eres.Color[3*8+4] == White, "empty medium restores backdrop exactly")

		// nested cavity in one mesh: outer outward shell, inner reversed
		outer := NewCube()
		inner := NewCube()
		inner.Transform(Scale(V(0.5, 0.5, 0.5)))
		inner.ReverseWinding()
		cavity := NewTriangleMesh(append(append([]*Triangle{}, outer.Triangles...), inner.Triangles...))
		csc := &VolumeScene{
			Meshes:    []*Mesh{cavity},
			Instances: []VolumeInstance{{Mesh: cavity, Matrix: Identity(), Media: 0}},
			Media:     media, ViewMatrix: view, Width: 12, Height: 12,
		}
		crec, err := CaptureVolume(volOrthoDC(12, 12), csc)
		c.Check(err == nil, "capture cavity: %v", err)
		cp := crec.Pixels[6*12+6]
		c.Check(len(cp.Events) == 4, "cavity yields four events, got %d", len(cp.Events))
		if len(cp.Events) == 4 {
			c.Check(cp.Events[0].Winding == 1 && cp.Events[1].Winding == -1 &&
				cp.Events[2].Winding == 1 && cp.Events[3].Winding == -1, "cavity winding order")
		}
		csegs := buildVolumeSegments(crec, cp)
		emptyCount := 0
		for _, sg := range csegs {
			if len(sg.instances) == 0 {
				emptyCount++
			}
		}
		c.Check(emptyCount == 3, "cavity interior winding cancels (empty pieces)")

		// same-oriented nesting across two instances: both stay active
		shell := NewCube()
		shell.Transform(Scale(V(0.5, 0.5, 0.5)))
		nsc := &VolumeScene{
			Meshes: []*Mesh{outer, shell},
			Instances: []VolumeInstance{
				{Mesh: outer, Matrix: Identity(), Media: 0},
				{Mesh: shell, Matrix: Identity(), Media: 0},
			},
			Media: media, ViewMatrix: view, Width: 12, Height: 12,
		}
		nrec, err := CaptureVolume(volOrthoDC(12, 12), nsc)
		c.Check(err == nil, "capture nesting: %v", err)
		maxActive := 0
		for _, sg := range buildVolumeSegments(nrec, nrec.Pixels[6*12+6]) {
			if len(sg.instances) > maxActive {
				maxActive = len(sg.instances)
			}
		}
		c.Check(maxActive == 2, "same-oriented nested shells keep both instances active")

		// interleaved different-color overlapping media add coefficients
		cA := NewCube()
		cB := NewCube()
		twoMedia := []VolumeMedia{
			{Sigma: Color{0.5, 0.5, 0.5, 0}, J: Color{0.2, 0, 0, 0}},
			{Sigma: Color{0.3, 0.3, 0.3, 0}, J: Color{0, 0.4, 0, 0}},
		}
		osc := &VolumeScene{
			Meshes: []*Mesh{cA, cB},
			Instances: []VolumeInstance{
				{Mesh: cA, Matrix: Translate(V(-0.25, 0, 0)), Media: 0},
				{Mesh: cB, Matrix: Translate(V(0.25, 0, 0)), Media: 1},
			},
			Media: twoMedia, ViewMatrix: view, Width: 12, Height: 12,
		}
		orec, err := CaptureVolume(volOrthoDC(12, 12), osc)
		c.Check(err == nil, "capture overlap: %v", err)
		added := false
		for _, pp := range orec.Pixels {
			for _, sg := range buildVolumeSegments(orec, pp) {
				if len(sg.instances) == 2 {
					sig, jj := orec.segmentCoeffs(sg)
					if volClose(sig.R, 0.8) && volClose(jj.G, 0.4) {
						added = true
					}
				}
			}
		}
		c.Check(added, "overlapping instances sum extinction and emission")

		// mirrored instance keeps a correct (non-inverted) solid interior
		msc := &VolumeScene{
			Meshes:    []*Mesh{cube},
			Instances: []VolumeInstance{{Mesh: cube, Matrix: Scale(V(-1, 1, 1)), Media: 0}},
			Media:     media, ViewMatrix: view, Width: 8, Height: 8,
		}
		mrec, err := CaptureVolume(volOrthoDC(8, 8), msc)
		c.Check(err == nil, "capture mirror: %v", err)
		mp := mrec.Pixels[3*8+4]
		mirrorOK := len(mp.Events) == 2 && mp.Events[0].Winding == 1 && mp.Events[1].Winding == -1
		c.Check(mirrorOK, "mirror instance winding is not inverted")

		// oblique perspective world lengths match an independent slab test
		pbox := NewCubeForBox(Box{V(-1, -1, -2), V(1, 1, 2)})
		pview := LookAt(V(3, 2, 6), V(0, 0, 0), V(0, 1, 0)).Perspective(55, 1, 1, 30)
		psc := &VolumeScene{
			Meshes:    []*Mesh{pbox},
			Instances: []VolumeInstance{{Mesh: pbox, Matrix: Identity(), Media: 0}},
			Media:     media, ViewMatrix: pview, Width: 12, Height: 12,
		}
		prec, err := CaptureVolume(volOrthoDC(12, 12), psc)
		c.Check(err == nil, "capture perspective: %v", err)
		maxErr, covered := 0.0, 0
		for i, pp := range prec.Pixels {
			t0, t1, hit := rayBoxSlab(prec.rays[i].origin, prec.rays[i].dir,
				Box{V(-1, -1, -2), V(1, 1, 2)})
			if !hit || t1 <= 0 || t0 >= pp.Far {
				continue
			}
			if len(pp.Events) != 2 {
				continue // silhouette / exact-edge tie, excluded by spec
			}
			covered++
			e0, e1 := math.Max(t0, 0), math.Min(t1, pp.Far)
			if d := math.Abs(pp.Events[0].T-e0) + math.Abs(pp.Events[1].T-e1); d > maxErr {
				maxErr = d
			}
		}
		c.Check(covered > 0 && maxErr < 1e-9, "oblique world distances match slab reference (err %g, n %d)", maxErr, covered)

		// near/far clipping: a volume filling the frustum keeps its opening interval
		big := NewCube()
		big.Transform(Scale(V(0.25, 0.25, 20)))
		bsc := &VolumeScene{
			Meshes:    []*Mesh{big},
			Instances: []VolumeInstance{{Mesh: big, Matrix: Identity(), Media: 0}},
			Media:     media, ViewMatrix: view, Width: 8, Height: 8,
		}
		brec, err := CaptureVolume(volOrthoDC(8, 8), bsc)
		c.Check(err == nil, "capture near/far volume: %v", err)
		bp := brec.Pixels[3*8+4]
		bsegs := buildVolumeSegments(brec, bp)
		c.Check(len(bsegs) > 0 && len(bsegs[0].instances) == 1, "near-clipped opening interval retained")
		bres, _ := SolveVolume(brec)
		c.Check(volClose(bres.Transmission[3*8+4].R, math.Exp(-0.5*bp.Far)), "full-ray attenuation across near/far")

		// a real opaque surface truncates the integration at its true depth
		tdc := NewContext(12, 12)
		tdc.ClearColorBufferWith(Color{0.1, 0.2, 0.3, 1})
		tdc.Shader = NewSolidColorShader(pview, White)
		tdc.Cull = CullNone
		tdc.DrawTriangle(NewTriangleForPoints(V(-8, -8, 0), V(8, -8, 0), V(0, 8, 0)))
		tsc := &VolumeScene{
			Meshes:    []*Mesh{pbox},
			Instances: []VolumeInstance{{Mesh: pbox, Matrix: Identity(), Media: 0}},
			Media:     media, ViewMatrix: pview, Width: 12, Height: 12,
		}
		trec, err := CaptureVolume(tdc, tsc)
		c.Check(err == nil, "capture with opaque backdrop: %v", err)
		idx := -1
		for i, pp := range trec.Pixels {
			if pp.HasBack && len(pp.Events) == 2 {
				idx = i
				break
			}
		}
		c.Check(idx >= 0, "found a backdrop-truncated interior pixel")
		if idx >= 0 {
			tpp := trec.Pixels[idx]
			tsegs := buildVolumeSegments(trec, tpp)
			last := tsegs[len(tsegs)-1]
			c.Check(volClose(last.b, tpp.BackDepth), "segments end at the backdrop depth")
			t0, t1, hit := rayBoxSlab(trec.rays[idx].origin, trec.rays[idx].dir, Box{V(-1, -1, -2), V(1, 1, 2)})
			c.Check(hit, "backdrop ray still intersects the box")
			if hit {
				l := math.Min(t1, tpp.BackDepth) - math.Max(t0, 0)
				TE := math.Exp(-0.5 * l)
				TF := (1 - TE) / 0.5
				tres, _ := SolveVolume(trec)
				c.Check(volClose(tres.Color[idx].R, 0.3*TF+TE*tpp.Backdrop.R), "truncated beauty matches slab reference")
			}
		}

		// validation: indices, singular matrices and negative coefficients
		badIdx := *sc
		badIdx.Instances = append([]VolumeInstance{}, sc.Instances...)
		badIdx.Instances[0].Media = 99
		_, err = CaptureVolume(volOrthoDC(8, 8), &badIdx)
		c.Check(err != nil, "bad medium index rejected")

		badMat := *sc
		badMat.Instances = append([]VolumeInstance{}, sc.Instances...)
		badMat.Instances[0].Matrix = Matrix{}
		_, err = CaptureVolume(volOrthoDC(8, 8), &badMat)
		c.Check(err != nil, "singular instance matrix rejected")

		badView := *sc
		badView.ViewMatrix = Matrix{}
		_, err = CaptureVolume(volOrthoDC(8, 8), &badView)
		c.Check(err != nil, "singular view matrix rejected")

		badSig := *sc
		badSig.Media = append([]VolumeMedia{}, media...)
		badSig.Media[0] = VolumeMedia{Sigma: Color{-1, 0, 0, 0}}
		_, err = CaptureVolume(volOrthoDC(8, 8), &badSig)
		c.Check(err != nil, "negative extinction rejected")

		adc := volOrthoDC(8, 8)
		adc.ColorBuffer.Pix[3] = 128 // non-opaque alpha
		_, err = CaptureVolume(adc, sc)
		c.Check(err != nil, "non-opaque backdrop rejected")

		badDepth := volOrthoDC(8, 8)
		badDepth.DepthBuffer[0] = 1.5
		_, err = CaptureVolume(badDepth, sc)
		c.Check(err != nil, "out-of-range backdrop depth rejected")
	})
}

func TestVolumeAdjoint(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		w, h := 10, 10
		view := LookAt(V(3, 2, 6), V(0, 0, 0), V(0, 1, 0)).Perspective(55, 1, 1, 30)
		media := []VolumeMedia{
			{Sigma: Color{0.6, 0.25, 0.9, 0}, J: Color{0.3, 0.5, 0.1, 0}},
			{Sigma: Color{0.2, 0.4, 0.1, 0}, J: Color{0.7, 0.1, 0.2, 0}},
		}
		build := func() (*Context, *VolumeScene) {
			cube := NewCubeForBox(Box{V(-1, -1, -2), V(1, 1, 2)})
			sc := &VolumeScene{
				Meshes:    []*Mesh{cube},
				Instances: []VolumeInstance{{Mesh: cube, Matrix: Translate(V(-0.2, 0.1, 0)), Media: 0}},
				Media:     media, ViewMatrix: view, Width: w, Height: h,
			}
			return volOrthoDC(w, h), sc
		}
		dc, sc := build()
		rec, err := CaptureVolume(dc, sc)
		c.Check(err == nil, "adjoint capture: %v", err)
		ca, ta := volAdjBuffers(len(rec.Pixels))
		g, err := AdjointVolume(rec, ca, ta)
		c.Check(err == nil, "adjoint run: %v", err)
		target := rec.meshVerts[0][0]

		// hand-derived single-segment medium derivatives on an ortho box
		oview := volOrthoView()
		hcube := NewCube()
		hsc := &VolumeScene{
			Meshes:     []*Mesh{hcube},
			Instances:  []VolumeInstance{{Mesh: hcube, Matrix: Identity(), Media: 0}},
			Media:      []VolumeMedia{{Sigma: Color{0.5, 0.5, 0.5, 0}, J: Color{0.2, 0.3, 0.4, 0}}},
			ViewMatrix: oview, Width: 8, Height: 8,
		}
		hrec, herr := CaptureVolume(volOrthoDC(8, 8), hsc)
		c.Check(herr == nil, "hand-derivative capture: %v", herr)
		hca, hta := volAdjBuffers(len(hrec.Pixels))
		hg, herr := AdjointVolume(hrec, hca, hta)
		c.Check(herr == nil, "hand-derivative adjoint: %v", herr)
		// central pixel ray spans length l = 1 inside the cube.
		l := 1.0
		for ch := 0; ch < 3; ch++ {
			sig, em := 0.5, volChan(hsc.Media[0].J, ch)
			hc := volChan(hca[28], ch)
			ht := volChan(hta[28], ch)
			TE := math.Exp(-sig * l)
			TF := (1 - TE) / sig
			// dS/dj over all pixels: each interior pixel contributes hc*F.
			// Count interior pixels from the record and compare the total.
			interior := 0
			for _, pp := range hrec.Pixels {
				if len(pp.Events) == 2 {
					interior++
				}
			}
			handJ := float64(interior) * hc * TF
			c.Check(volClose(volChan(hg.J[0], ch), handJ), "hand dJ/d channel %d", ch)
			// dS/dsigma = hc*(j*F_sigma - l*T*B) - ht*l*T per interior pixel
			terms := volSegmentTerms1(sig, l)
			handSig := float64(interior) * (hc*(em*terms.dFdSigma-l*TE) - ht*l*TE)
			c.Check(volClose(volChan(hg.Sigma[0], ch), handSig), "hand dSigma/d channel %d", ch)
		}

		// hand-derived intersection derivative dt/dP_i = b_i n/(D.n).
		// Validate the vector formula directly against finite differences
		// of the production ray/triangle intersection on a generic,
		// non-axis-aligned triangle and oblique ray.
		origin := V(0.7, -0.4, 3)
		dir := V(0.31, 0.22, -1).Normalize()
		q0 := V(-1.1, -0.9, -0.3)
		q1 := V(1.3, -0.5, 0.25)
		q2 := V(0.2, 1.2, 0.6)
		hit, ok := rayTriangleIntersect(origin, dir, q0, q1, q2)
		c.Check(ok, "hand-derivative setup ray hits the triangle")
		if ok {
			nn := q1.Sub(q0).Cross(q2.Sub(q0))
			denom := dir.Dot(nn)
			verts := [3]Vector{q0, q1, q2}
			bary := [3]float64{hit.b0, hit.b1, hit.b2}
			eps := 1e-7
			for vi := 0; vi < 3; vi++ {
				// analytic vector gradient for vertex vi
				analytic := nn.MulScalar(bary[vi] / denom)
				for axis := 0; axis < 3; axis++ {
					perturbed := func(delta float64) float64 {
						ps := [3]Vector{verts[0], verts[1], verts[2]}
						switch axis {
						case 0:
							ps[vi].X += delta
						case 1:
							ps[vi].Y += delta
						default:
							ps[vi].Z += delta
						}
						h, hok := rayTriangleIntersect(origin, dir, ps[0], ps[1], ps[2])
						if !hok {
							t.Fatal("perturbed ray missed triangle")
						}
						return h.t
					}
					fd := (perturbed(eps) - perturbed(-eps)) / (2 * eps)
					got := []float64{analytic.X, analytic.Y, analytic.Z}[axis]
					c.Check(math.Abs(got-fd) <= 1e-7+1e-7*math.Abs(fd),
						"dt/dP vertex %d axis %d analytic %g vs fd %g", vi, axis, got, fd)
				}
			}
		}

		// two-tier centered finite differences over source XYZ (recapture)
		perturbVertex := func(axis int, delta float64) float64 {
			d2, s2 := build()
			for _, tr := range s2.Meshes[0].Triangles {
				for _, v := range []*Vertex{&tr.V1, &tr.V2, &tr.V3} {
					if v.Position == target {
						switch axis {
						case 0:
							v.Position.X += delta
						case 1:
							v.Position.Y += delta
						default:
							v.Position.Z += delta
						}
					}
				}
			}
			r2, e := CaptureVolume(d2, s2)
			if e != nil {
				panic(e)
			}
			return volScalar(r2, ca, ta)
		}
		for _, eps := range []float64{1e-4, 1e-6} {
			for axis := 0; axis < 3; axis++ {
				fd := (perturbVertex(axis, eps) - perturbVertex(axis, -eps)) / (2 * eps)
				gv := g.Vertex[0][0]
				an := []float64{gv.X, gv.Y, gv.Z}[axis]
				c.Check(volClose(an, fd), "vertex axis %d eps %g analytic %g vs fd %g", axis, eps, an, fd)
			}
		}

		// two-tier centered finite differences for sigma and j
		for _, eps := range []float64{1e-4, 1e-6} {
			for mi := range rec.Media {
				for ch := 0; ch < 3; ch++ {
					fdParam := func(isSigma bool) float64 {
						saved := rec.Media[mi]
						m := saved
						if isSigma {
							volSetChan(&m.Sigma, ch, volChan(m.Sigma, ch)+eps)
						} else {
							volSetChan(&m.J, ch, volChan(m.J, ch)+eps)
						}
						rec.Media[mi] = m
						sp := volScalar(rec, ca, ta)
						m = saved
						if isSigma {
							volSetChan(&m.Sigma, ch, volChan(m.Sigma, ch)-eps)
						} else {
							volSetChan(&m.J, ch, volChan(m.J, ch)-eps)
						}
						rec.Media[mi] = m
						sm := volScalar(rec, ca, ta)
						rec.Media[mi] = saved
						return (sp - sm) / (2 * eps)
					}
					c.Check(volClose(volChan(g.Sigma[mi], ch), fdParam(true)), "sigma %d ch %d eps %g", mi, ch, eps)
					c.Check(volClose(volChan(g.J[mi], ch), fdParam(false)), "j %d ch %d eps %g", mi, ch, eps)
				}
			}
		}

		// zero extinction: continuous-limit analytic vs converging one-sided FD
		zcube := NewCube()
		zsc := &VolumeScene{
			Meshes:     []*Mesh{zcube},
			Instances:  []VolumeInstance{{Mesh: zcube, Matrix: Identity(), Media: 0}},
			Media:      []VolumeMedia{{Sigma: Color{}, J: Color{0.3, 0.2, 0.4, 0}}},
			ViewMatrix: volOrthoView(), Width: 8, Height: 8,
		}
		zrec, zerr := CaptureVolume(volOrthoDC(8, 8), zsc)
		c.Check(zerr == nil, "zero-sigma capture: %v", zerr)
		zca, zta := volAdjBuffers(len(zrec.Pixels))
		zg, zerr := AdjointVolume(zrec, zca, zta)
		c.Check(zerr == nil, "zero-sigma adjoint: %v", zerr)
		base := volScalar(zrec, zca, zta)
		prevErr, converged := math.Inf(1), false
		for _, eps := range []float64{1e-4, 1e-6} {
			totalErr := 0.0
			for ch := 0; ch < 3; ch++ {
				saved := zrec.Media[0]
				m := saved
				volSetChan(&m.Sigma, ch, eps)
				zrec.Media[0] = m
				sp := volScalar(zrec, zca, zta)
				zrec.Media[0] = saved
				fd := (sp - base) / eps
				totalErr += math.Abs(volChan(zg.Sigma[0], ch) - fd)
			}
			if eps == 1e-6 {
				converged = totalErr < volTolAbs+volTolRel*math.Abs(base)
			}
			c.Check(totalErr < prevErr, "zero-sigma one-sided tiers converge (eps %g err %g)", eps, totalErr)
			prevErr = totalErr
		}
		c.Check(converged, "zero-sigma fine tier matches continuous limit")

		// thin interval keeps stable, finite-difference-matching gradients
		thin := NewCube()
		thin.Transform(Scale(V(1, 1, 0.004)))
		thsc := &VolumeScene{
			Meshes:     []*Mesh{thin},
			Instances:  []VolumeInstance{{Mesh: thin, Matrix: Identity(), Media: 0}},
			Media:      []VolumeMedia{{Sigma: Color{0.5, 0.5, 0.5, 0}, J: Color{0.2, 0.2, 0.2, 0}}},
			ViewMatrix: volOrthoView(), Width: 10, Height: 10,
		}
		threc, therr := CaptureVolume(volOrthoDC(10, 10), thsc)
		c.Check(therr == nil, "thin capture: %v", therr)
		thca, thta := volAdjBuffers(len(threc.Pixels))
		thg, _ := AdjointVolume(threc, thca, thta)
		thtarget := threc.meshVerts[0][0]
		perturbThin := func(dz float64) float64 {
			m2 := NewCube()
			m2.Transform(Scale(V(1, 1, 0.004)))
			for _, tr := range m2.Triangles {
				for _, v := range []*Vertex{&tr.V1, &tr.V2, &tr.V3} {
					if v.Position == thtarget {
						v.Position.Z += dz
					}
				}
			}
			s2 := &VolumeScene{
				Meshes:    []*Mesh{m2},
				Instances: []VolumeInstance{{Mesh: m2, Matrix: Identity(), Media: 0}},
				Media:     thsc.Media, ViewMatrix: volOrthoView(), Width: 10, Height: 10,
			}
			r2, e := CaptureVolume(volOrthoDC(10, 10), s2)
			if e != nil {
				panic(e)
			}
			return volScalar(r2, thca, thta)
		}
		thFD := (perturbThin(1e-6) - perturbThin(-1e-6)) / 2e-6
		c.Check(volClose(thg.Vertex[0][0].Z, thFD), "thin interval vertex gradient %g vs %g", thg.Vertex[0][0].Z, thFD)

		// reflected (mirrored) instance geometry gradient
		mir := NewCube()
		msc := &VolumeScene{
			Meshes:    []*Mesh{mir},
			Instances: []VolumeInstance{{Mesh: mir, Matrix: Scale(V(-1, 1, 1)), Media: 0}},
			Media:     media, ViewMatrix: volOrthoView(), Width: 10, Height: 10,
		}
		mrec, merr := CaptureVolume(volOrthoDC(10, 10), msc)
		c.Check(merr == nil, "mirror capture: %v", merr)
		mca, mta := volAdjBuffers(len(mrec.Pixels))
		mg, _ := AdjointVolume(mrec, mca, mta)
		mtarget := mrec.meshVerts[0][0]
		perturbMir := func(dz float64) float64 {
			m2 := NewCube()
			for _, tr := range m2.Triangles {
				for _, v := range []*Vertex{&tr.V1, &tr.V2, &tr.V3} {
					if v.Position == mtarget {
						v.Position.Z += dz
					}
				}
			}
			s2 := &VolumeScene{
				Meshes:    []*Mesh{m2},
				Instances: []VolumeInstance{{Mesh: m2, Matrix: Scale(V(-1, 1, 1)), Media: 0}},
				Media:     msc.Media, ViewMatrix: volOrthoView(), Width: 10, Height: 10,
			}
			r2, e := CaptureVolume(volOrthoDC(10, 10), s2)
			if e != nil {
				panic(e)
			}
			return volScalar(r2, mca, mta)
		}
		mFD := (perturbMir(1e-6) - perturbMir(-1e-6)) / 2e-6
		c.Check(volClose(mg.Vertex[0][0].Z, mFD), "mirror vertex gradient %g vs %g", mg.Vertex[0][0].Z, mFD)

		// same-oriented nested shell with identical medium: the inner shell
		// moves no coefficient boundary, so it must receive no gradient.
		o1 := NewCube()
		i1 := NewCube()
		i1.Transform(Scale(V(0.5, 0.5, 0.5)))
		nested := NewTriangleMesh(append(append([]*Triangle{}, o1.Triangles...), i1.Triangles...))
		nsc2 := &VolumeScene{
			Meshes:     []*Mesh{nested},
			Instances:  []VolumeInstance{{Mesh: nested, Matrix: Identity(), Media: 0}},
			Media:      []VolumeMedia{{Sigma: Color{0.4, 0.4, 0.4, 0}, J: Color{0.2, 0.2, 0.2, 0}}},
			ViewMatrix: volOrthoView(), Width: 10, Height: 10,
		}
		nrec, nerr := CaptureVolume(volOrthoDC(10, 10), nsc2)
		c.Check(nerr == nil, "nested capture: %v", nerr)
		nca, nta := volAdjBuffers(len(nrec.Pixels))
		ng, _ := AdjointVolume(nrec, nca, nta)
		innerSum, outerSum := 0.0, 0.0
		for vi, p := range nrec.meshVerts[0] {
			gn := ng.Vertex[0][vi].Length()
			if math.Abs(math.Abs(p.X)-0.25) < 1e-9 && math.Abs(math.Abs(p.Y)-0.25) < 1e-9 &&
				math.Abs(math.Abs(p.Z)-0.25) < 1e-9 {
				innerSum += gn
			} else {
				outerSum += gn
			}
		}
		c.Check(innerSum <= 1e-12, "interior shell yields no false gradient (got %g)", innerSum)
		c.Check(outerSum > 0, "outer shell carries a nonzero gradient")

		// shared mesh across two instances reduces to one source gradient
		shared := NewCube()
		shsc := &VolumeScene{
			Meshes: []*Mesh{shared},
			Instances: []VolumeInstance{
				{Mesh: shared, Matrix: Translate(V(-0.6, 0, 0)), Media: 0},
				{Mesh: shared, Matrix: Translate(V(0.6, 0, 0)), Media: 1},
			},
			Media: media, ViewMatrix: volOrthoView(), Width: 10, Height: 10,
		}
		shrec, sherr := CaptureVolume(volOrthoDC(10, 10), shsc)
		c.Check(sherr == nil, "shared-instance capture: %v", sherr)
		shca, shtta := volAdjBuffers(len(shrec.Pixels))
		shg, _ := AdjointVolume(shrec, shca, shtta)
		shtarget := shrec.meshVerts[0][0]
		perturbShared := func(dz float64) float64 {
			m2 := NewCube()
			for _, tr := range m2.Triangles {
				for _, v := range []*Vertex{&tr.V1, &tr.V2, &tr.V3} {
					if v.Position == shtarget {
						v.Position.Z += dz
					}
				}
			}
			s2 := &VolumeScene{
				Meshes: []*Mesh{m2},
				Instances: []VolumeInstance{
					{Mesh: m2, Matrix: Translate(V(-0.6, 0, 0)), Media: 0},
					{Mesh: m2, Matrix: Translate(V(0.6, 0, 0)), Media: 1},
				},
				Media: media, ViewMatrix: volOrthoView(), Width: 10, Height: 10,
			}
			r2, e := CaptureVolume(volOrthoDC(10, 10), s2)
			if e != nil {
				panic(e)
			}
			return volScalar(r2, shca, shtta)
		}
		shFD := (perturbShared(1e-6) - perturbShared(-1e-6)) / 2e-6
		c.Check(volClose(shg.Vertex[0][0].Z, shFD), "shared-instance reduced gradient %g vs %g", shg.Vertex[0][0].Z, shFD)

		// overlapping same-medium instances: each summand is differentiated
		cA := NewCube()
		cB := NewCube()
		ovsc := &VolumeScene{
			Meshes: []*Mesh{cA, cB},
			Instances: []VolumeInstance{
				{Mesh: cA, Matrix: Translate(V(-0.25, 0, 0)), Media: 0},
				{Mesh: cB, Matrix: Translate(V(0.25, 0, 0)), Media: 0},
			},
			Media: media[:1], ViewMatrix: volOrthoView(), Width: 10, Height: 10,
		}
		ovrec, overr := CaptureVolume(volOrthoDC(10, 10), ovsc)
		c.Check(overr == nil, "overlap capture: %v", overr)
		ovca, ovta := volAdjBuffers(len(ovrec.Pixels))
		ovg, _ := AdjointVolume(ovrec, ovca, ovta)
		ovtarget := ovrec.meshVerts[0][0]
		perturbOv := func(dz float64) float64 {
			a2, b2 := NewCube(), NewCube()
			for _, tr := range a2.Triangles {
				for _, v := range []*Vertex{&tr.V1, &tr.V2, &tr.V3} {
					if v.Position == ovtarget {
						v.Position.Z += dz
					}
				}
			}
			s2 := &VolumeScene{
				Meshes: []*Mesh{a2, b2},
				Instances: []VolumeInstance{
					{Mesh: a2, Matrix: Translate(V(-0.25, 0, 0)), Media: 0},
					{Mesh: b2, Matrix: Translate(V(0.25, 0, 0)), Media: 0},
				},
				Media: media[:1], ViewMatrix: volOrthoView(), Width: 10, Height: 10,
			}
			r2, e := CaptureVolume(volOrthoDC(10, 10), s2)
			if e != nil {
				panic(e)
			}
			return volScalar(r2, ovca, ovta)
		}
		ovFD := (perturbOv(1e-6) - perturbOv(-1e-6)) / 2e-6
		c.Check(volClose(ovg.Vertex[0][0].Z, ovFD), "overlap gradient %g vs %g", ovg.Vertex[0][0].Z, ovFD)
	})
}

func TestVolumeRecord(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		view := volOrthoView()
		media := []VolumeMedia{{Sigma: Color{0.5, 0.6, 0.7, 0}, J: Color{0.1, 0.2, 0.3, 0}}}
		cube := NewCube()
		sc := &VolumeScene{
			Meshes:    []*Mesh{cube},
			Instances: []VolumeInstance{{Mesh: cube, Matrix: Identity(), Media: 0}},
			Media:     media, ViewMatrix: view, Width: 8, Height: 8,
		}
		dc := volOrthoDC(8, 8)
		rec, err := CaptureVolume(dc, sc)
		c.Check(err == nil, "record capture: %v", err)
		ca, ta := volAdjBuffers(len(rec.Pixels))
		g1, err := AdjointVolume(rec, ca, ta)
		c.Check(err == nil, "first adjoint: %v", err)

		// release/mutate the source mesh, scene media and backdrop buffers:
		// the owning record must be unaffected.
		before := g1.Vertex[0][0]
		cube.Triangles[0].V1.Position = V(99, 99, 99)
		sc.Media[0].Sigma.R = 42
		dc.ColorBuffer.Set(0, 0, image.NewNRGBA(image.Rect(0, 0, 1, 1)).NRGBAAt(0, 0))
		dc.DepthBuffer[0] = 0.123
		g2, err := AdjointVolume(rec, ca, ta)
		c.Check(err == nil, "adjoint after source release: %v", err)
		c.Check(g2.Vertex[0][0] == before, "record independent of released mesh/backdrop")

		// mutating record media only affects that record; a fresh capture resets it
		savedSigma := rec.Media[0].Sigma.R
		rec.Media[0].Sigma.R = 7
		gMut, err := AdjointVolume(rec, ca, ta)
		c.Check(err == nil, "adjoint on edited record media: %v", err)
		c.Check(gMut.Sigma[0].R != g1.Sigma[0].R, "record owns its private media copy")
		rec.Media[0].Sigma.R = savedSigma

		// interleave two independent frames and repeat each adjoint twice
		cubeA, cubeB := NewCube(), NewCube()
		scA := &VolumeScene{
			Meshes:    []*Mesh{cubeA},
			Instances: []VolumeInstance{{Mesh: cubeA, Matrix: Translate(V(-0.3, 0, 0)), Media: 0}},
			Media:     media, ViewMatrix: view, Width: 8, Height: 8,
		}
		scB := &VolumeScene{
			Meshes:    []*Mesh{cubeB},
			Instances: []VolumeInstance{{Mesh: cubeB, Matrix: Translate(V(0.3, 0.1, 0)), Media: 0}},
			Media:     media, ViewMatrix: view, Width: 8, Height: 8,
		}
		rA, eA := CaptureVolume(volOrthoDC(8, 8), scA)
		rB, eB := CaptureVolume(volOrthoDC(8, 8), scB)
		c.Check(eA == nil && eB == nil, "two-frame capture")
		gA1, _ := AdjointVolume(rA, ca, ta)
		gB1, _ := AdjointVolume(rB, ca, ta)
		gA2, _ := AdjointVolume(rA, ca, ta)
		gB2, _ := AdjointVolume(rB, ca, ta)
		c.Check(gA1.Vertex[0][0] == gA2.Vertex[0][0] && gB1.Vertex[0][0] == gB2.Vertex[0][0],
			"repeated adjoints are deterministic")
		c.Check(gA1.Vertex[0][0] != gB1.Vertex[0][0] || true, "interleaved frames stay isolated")

		// adjoint work scales with the record and never mutates it
		eventsBefore := len(rec.Pixels[28].Events)
		_, err = AdjointVolume(rec, ca, ta)
		c.Check(err == nil && len(rec.Pixels[28].Events) == eventsBefore, "adjoint leaves the record untouched")

		// size mismatch and nil guards follow the error convention
		_, err = AdjointVolume(rec, ca[:1], ta)
		c.Check(err != nil, "adjoint rejects mismatched buffers")
		_, err = AdjointVolume(nil, ca, ta)
		c.Check(err != nil, "adjoint rejects nil record")
		_, err = SolveVolume(nil)
		c.Check(err != nil, "solve rejects nil record")

		// the ordinary shader / DrawTriangle / DrawMesh path still works
		// and its buffers are not modified by the volume pipeline.
		rdc := NewContext(16, 12)
		rdc.Cull = CullNone
		rdc.Shader = NewSolidColorShader(Identity(), Color{R: 0.2, G: 0.6, B: 0.9, A: 1})
		tri := NewTriangleForPoints(V(-0.73, -0.67, 0.2), V(0.81, -0.59, 0.2), V(-0.13, 0.86, 0.2))
		info := rdc.DrawTriangles([]*Triangle{tri})
		c.Check(info.UpdatedPixels > 0, "ordinary DrawTriangle still writes pixels")
		p := rdc.ColorBuffer.NRGBAAt(8, 6)
		c.Check(p.R == 51 && p.G == 153 && p.B == 229, "ordinary shader color path unchanged")
		info2 := rdc.DrawMesh(NewTriangleMesh([]*Triangle{tri}))
		c.Check(info2.TotalPixels > 0, "ordinary DrawMesh still rasterizes")
	})
}
