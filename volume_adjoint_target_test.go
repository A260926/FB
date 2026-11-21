package fauxgl

import (
	"bytes"
	"math"
	"sort"
	"testing"

	"github.com/fogleman/fauxgl/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// test helpers: box meshes, reference model from box half-spaces, independent
// piecewise transport formulas
// ---------------------------------------------------------------------------

func boxPositions(min, max Vector) []Vector {
	return []Vector{
		{min.X, min.Y, min.Z}, // 0
		{max.X, min.Y, min.Z}, // 1
		{max.X, max.Y, min.Z}, // 2
		{min.X, max.Y, min.Z}, // 3
		{min.X, min.Y, max.Z}, // 4
		{max.X, min.Y, max.Z}, // 5
		{max.X, max.Y, max.Z}, // 6
		{min.X, max.Y, max.Z}, // 7
	}
}

func boxFaces() [][3]int {
	return [][3]int{
		{0, 2, 1}, {0, 3, 2}, // z min (outward -z)
		{4, 5, 6}, {4, 6, 7}, // z max (outward +z)
		{0, 1, 5}, {0, 5, 4}, // y min (outward -y)
		{2, 3, 7}, {2, 7, 6}, // y max (outward +y)
		{0, 4, 7}, {0, 7, 3}, // x min (outward -x)
		{1, 2, 6}, {1, 6, 5}, // x max (outward +x)
	}
}

func setCh(v *Vector, c int, x float64) {
	switch c {
	case 0:
		v.X = x
	case 1:
		v.Y = x
	default:
		v.Z = x
	}
}

// refShell is one signed box shell of the independent reference model.
type refShell struct {
	min, max Vector
	sign     float64 // +1 outward shell, -1 reversed (cavity) shell
}

// refInstance is the independent half-space model of one instance.
type refInstance struct {
	matrix Matrix
	medium int
	shells []refShell
}

type shellGroup struct {
	shells []refShell
	matrix Matrix
	medium int
}

// sceneFromGroups builds a VolumeScene (one mesh per group) plus the
// parallel half-space reference model.
func sceneFromGroups(groups []shellGroup, media []VolumeMedium) (*VolumeScene, []refInstance) {
	scene := &VolumeScene{Media: media}
	var refs []refInstance
	for _, g := range groups {
		var positions []Vector
		var faces [][3]int
		for _, sh := range g.shells {
			base := len(positions)
			positions = append(positions, boxPositions(sh.min, sh.max)...)
			for _, f := range boxFaces() {
				if sh.sign < 0 {
					f = [3]int{f[0], f[2], f[1]}
				}
				faces = append(faces, [3]int{base + f[0], base + f[1], base + f[2]})
			}
		}
		scene.Meshes = append(scene.Meshes, VolumeMesh{positions, faces})
		scene.Instances = append(scene.Instances, VolumeInstance{
			Mesh: len(scene.Meshes) - 1, Matrix: g.matrix, Medium: g.medium})
		refs = append(refs, refInstance{matrix: g.matrix, medium: g.medium, shells: g.shells})
	}
	return scene, refs
}

// slabBox intersects the world ray O+tD (D unit) with a box given in the
// instance's local frame, keeping the world ray parameter.
func slabBox(o, d Vector, inv Matrix, min, max Vector) (float64, float64, bool) {
	lo := inv.MulPosition(o)
	ld := inv.MulPosition(o.Add(d)).Sub(lo)
	t0 := math.Inf(-1)
	t1 := math.Inf(1)
	for c := 0; c < 3; c++ {
		oc, dc := volAt(lo, c), volAt(ld, c)
		mn, mx := volAt(min, c), volAt(max, c)
		if dc == 0 {
			if oc < mn || oc > mx {
				return 0, 0, false
			}
			continue
		}
		a := (mn - oc) / dc
		b := (mx - oc) / dc
		if a > b {
			a, b = b, a
		}
		if a > t0 {
			t0 = a
		}
		if b < t1 {
			t1 = b
		}
	}
	if t0 >= t1 {
		return 0, 0, false
	}
	return t0, t1, true
}

type refSegment struct {
	t0, t1    float64
	instances []int
	sigma, j  Vector
}

// refPixelSegments computes media intervals from box half-spaces only.
func refPixelSegments(insts []refInstance, media []VolumeMedium, o, d Vector, tEnd float64) []refSegment {
	type cross struct {
		t    float64
		inst int
		dlt  int
	}
	var cs []cross
	for ii, in := range insts {
		ds := 1
		if in.matrix.Determinant() < 0 {
			ds = -1
		}
		inv := in.matrix.Inverse()
		for _, sh := range in.shells {
			t0, t1, ok := slabBox(o, d, inv, sh.min, sh.max)
			if !ok {
				continue
			}
			s := int(sh.sign) * ds
			cs = append(cs, cross{t0, ii, s}, cross{t1, ii, -s})
		}
	}
	sort.Slice(cs, func(a, b int) bool {
		if cs[a].t != cs[b].t {
			return cs[a].t < cs[b].t
		}
		return cs[a].inst < cs[b].inst
	})
	wind := make([]int, len(insts))
	ci := 0
	for ci < len(cs) && cs[ci].t <= 0 {
		wind[cs[ci].inst] += cs[ci].dlt
		ci++
	}
	active := func() []int {
		var a []int
		for i, w := range wind {
			if w != 0 {
				a = append(a, i)
			}
		}
		return a
	}
	var segs []refSegment
	prev := 0.0
	cur := active()
	closeSeg := func(t1 float64) {
		var sigma, j Vector
		for _, ii := range cur {
			sigma = sigma.Add(media[insts[ii].medium].Sigma)
			j = j.Add(media[insts[ii].medium].J)
		}
		segs = append(segs, refSegment{prev, t1, append([]int(nil), cur...), sigma, j})
	}
	for ci < len(cs) {
		e := cs[ci]
		if e.t >= tEnd {
			break
		}
		ci++
		w0 := wind[e.inst]
		w1 := w0 + e.dlt
		if (w0 == 0) == (w1 == 0) {
			wind[e.inst] = w1
			continue
		}
		closeSeg(e.t)
		wind[e.inst] = w1
		cur = active()
		prev = e.t
	}
	closeSeg(tEnd)
	return segs
}

// refTransportValue evaluates the piecewise transport formula independently.
func refTransportValue(segs []refSegment, bg Vector) (Vector, Vector) {
	T := Vector{1, 1, 1}
	var C Vector
	for _, s := range segs {
		l := s.t1 - s.t0
		var cAdd, tNew Vector
		for ch := 0; ch < 3; ch++ {
			sg := volAt(s.sigma, ch)
			jj := volAt(s.j, ch)
			e := math.Exp(-sg * l)
			g := l
			if sg != 0 {
				g = (1 - e) / sg
			}
			setCh(&cAdd, ch, volAt(T, ch)*jj*g)
			setCh(&tNew, ch, volAt(T, ch)*e)
		}
		C = C.Add(cAdd)
		T = tNew
	}
	C = C.Add(T.Mul(bg))
	return C, T
}

func intSlicesEqual(a, b []int) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func volumeWeights(n int, seed float64) []Vector {
	w := make([]Vector, n)
	for i := range w {
		w[i] = Vector{
			math.Sin(seed+float64(i)*1.7) * 0.9,
			math.Cos(seed+float64(i)*2.3) * 0.8,
			math.Sin(seed+float64(i)*3.1+1.0) * 0.7,
		}
	}
	return w
}

func volumeLoss(scene *VolumeScene, dc *Context, camera Matrix, wC, wT []Vector) float64 {
	capture, err := CaptureVolume(dc, scene, camera)
	if err != nil {
		panic(err)
	}
	img, _ := TransportVolume(capture)
	L := 0.0
	for i := range img.Color {
		L += wC[i].Dot(img.Color[i]) + wT[i].Dot(img.Transmittance[i])
	}
	return L
}

// checkFD compares an adjoint gradient component against finite
// differences of the production forward pass at two step sizes.
func checkFD(c *taskcheck.C, got float64, f func(h float64) float64, central bool, what string) {
	var r1, r2 float64
	if central {
		h1, h2 := 1e-4, 1e-6
		r1 = (f(h1) - f(-h1)) / (2 * h1)
		r2 = (f(h2) - f(-h2)) / (2 * h2)
	} else {
		h1, h2 := 1e-6, 1e-7
		f0 := f(0)
		r1 = (f(h1) - f0) / h1
		r2 = (f(h2) - f0) / h2
	}
	tol1 := 3e-5 + 3e-5*math.Abs(r1)
	tol2 := 3e-5 + 3e-5*math.Abs(r2)
	c.Check(math.Abs(got-r1) <= tol1, "%s: adjoint %.9g vs fd(h1) %.9g", what, got, r1)
	c.Check(math.Abs(got-r2) <= tol2, "%s: adjoint %.9g vs fd(h2) %.9g", what, got, r2)
}

func checkClose(c *taskcheck.C, got, want, tol float64, what string) {
	c.Check(math.Abs(got-want) <= tol, "%s: got %.9g want %.9g", what, got, want)
}

func checkIntervals(c *taskcheck.C, capture *VolumeCapture, refs []refInstance, x, y int, tol float64) {
	px := &capture.Pixels[y*capture.Width+x]
	segs := refPixelSegments(refs, capture.Media, px.O, px.D, px.TEnd)
	sameCount := len(px.Intervals) == len(segs)
	c.Check(sameCount, "pixel (%d,%d): got %d intervals, want %d", x, y, len(px.Intervals), len(segs))
	if !sameCount {
		return
	}
	for i, iv := range px.Intervals {
		c.Check(math.Abs(iv.T0-segs[i].t0) <= tol && math.Abs(iv.T1-segs[i].t1) <= tol,
			"pixel (%d,%d) interval %d: got [%.9g, %.9g] want [%.9g, %.9g]",
			x, y, i, iv.T0, iv.T1, segs[i].t0, segs[i].t1)
		c.Check(intSlicesEqual(iv.Instances, segs[i].instances),
			"pixel (%d,%d) interval %d: active %v want %v", x, y, i, iv.Instances, segs[i].instances)
	}
}

func checkTransport(c *taskcheck.C, capture *VolumeCapture, img *VolumeImage, refs []refInstance, x, y int, tol float64) {
	i := y*capture.Width + x
	px := &capture.Pixels[i]
	segs := refPixelSegments(refs, capture.Media, px.O, px.D, px.TEnd)
	wantC, wantT := refTransportValue(segs, px.Bg)
	for ch := 0; ch < 3; ch++ {
		c.Check(math.Abs(volAt(img.Color[i], ch)-volAt(wantC, ch)) <= tol,
			"pixel (%d,%d) color[%d]: got %.9g want %.9g", x, y, ch, volAt(img.Color[i], ch), volAt(wantC, ch))
		c.Check(math.Abs(volAt(img.Transmittance[i], ch)-volAt(wantT, ch)) <= tol,
			"pixel (%d,%d) transmittance[%d]: got %.9g want %.9g", x, y, ch, volAt(img.Transmittance[i], ch), volAt(wantT, ch))
	}
}

func orthoCamera() Matrix {
	return LookAt(Vector{0, 0, 5}, Vector{}, Vector{0, 1, 0}).Orthographic(-2, 2, -2, 2, 1, 10)
}

func opaqueContext(w, h int, bg Color) *Context {
	dc := NewContext(w, h)
	dc.ClearColorBufferWith(bg)
	dc.ClearDepthBuffer()
	return dc
}

// ---------------------------------------------------------------------------
// TestVolumeCapture
// ---------------------------------------------------------------------------

func TestVolumeCapture(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		media := []VolumeMedium{
			{Sigma: Vector{0.5, 0, 0.3}, J: Vector{2, 1, 0.5}},
			{Sigma: Vector{0.2, 0.4, 0.1}, J: Vector{0.1, 0.3, 0.9}},
		}

		// --- basic closed box: intervals, events, identities, ray geometry
		func() {
			scene, refs := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -1}, Vector{1, 1, 1}, 1}}, matrix: Identity(), medium: 0},
			}, media)
			dc := opaqueContext(32, 32, Color{0.2, 0.4, 0.6, 1})
			cam := orthoCamera()
			capture, err := CaptureVolume(dc, scene, cam)
			c.Check(err == nil, "basic capture: unexpected error %v", err)
			if err != nil {
				return
			}
			px := capture.Pixels[16*32+17]
			checkClose(c, px.O.X, 0.1875, 1e-12, "ray origin x")
			checkClose(c, px.O.Y, -0.0625, 1e-12, "ray origin y")
			checkClose(c, px.O.Z, 4, 1e-12, "ray origin z (near plane)")
			checkClose(c, px.D.X, 0, 1e-12, "ray dir x")
			checkClose(c, px.D.Y, 0, 1e-12, "ray dir y")
			checkClose(c, px.D.Z, -1, 1e-12, "ray dir z")
			checkClose(c, px.TEnd, 9, 1e-12, "tEnd at far plane")
			c.Check(len(px.Events) == 2, "basic: got %d events, want 2", len(px.Events))
			if len(px.Events) == 2 {
				e0, e1 := px.Events[0], px.Events[1]
				checkClose(c, e0.T, 3, 1e-9, "entry t")
				checkClose(c, e1.T, 5, 1e-9, "exit t")
				c.Check(e0.Delta == 1 && e1.Delta == -1, "basic: deltas %d, %d", e0.Delta, e1.Delta)
				c.Check(e0.Inst == 0 && e1.Inst == 0, "basic: event instances")
				c.Check(e0.Face >= 2 && e0.Face <= 3, "basic: entry face %d is a +z face", e0.Face)
				c.Check(e1.Face >= 0 && e1.Face <= 1, "basic: exit face %d is a -z face", e1.Face)
				for k := 0; k < 3; k++ {
					c.Check(e0.Vtx[k] >= 0 && e0.Vtx[k] < 8, "basic: entry vertex %d in mesh range", e0.Vtx[k])
				}
				checkClose(c, e0.Bary.X+e0.Bary.Y+e0.Bary.Z, 1, 1e-12, "entry barycentric sum")
				checkClose(c, e1.Bary.X+e1.Bary.Y+e1.Bary.Z, 1, 1e-12, "exit barycentric sum")
			}
			checkIntervals(c, capture, refs, 17, 16, 1e-9)
			checkIntervals(c, capture, refs, 2, 2, 1e-9) // outside the box
			out := capture.Pixels[2*32+2]
			c.Check(len(out.Intervals) == 1 && len(out.Intervals[0].Instances) == 0,
				"outside pixel: single empty interval")
			c.Check(len(out.Events) == 0, "outside pixel: no events")
			img, _ := TransportVolume(capture)
			checkTransport(c, capture, img, refs, 17, 16, 1e-12)
			checkTransport(c, capture, img, refs, 16, 17, 1e-12)
			checkTransport(c, capture, img, refs, 2, 2, 1e-12)
		}()

		// --- shared face diagonal: pixel center exactly on the shared edge
		func() {
			scene, _ := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -1}, Vector{1, 1, 1}, 1}}, matrix: Identity(), medium: 0},
			}, media)
			dc := opaqueContext(8, 8, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "diagonal capture: %v", err)
			if err != nil {
				return
			}
			// pixel (4,3) center maps to world (0.25, 0.25): exactly on the
			// z-face diagonals; each surface must still contribute once.
			px := capture.Pixels[3*8+4]
			c.Check(len(px.Events) == 2, "diagonal: got %d events, want 2 (shared edge counted once)", len(px.Events))
			c.Check(len(px.Intervals) == 3, "diagonal: got %d intervals, want 3", len(px.Intervals))
			if len(px.Intervals) == 3 {
				checkClose(c, px.Intervals[1].T0, 3, 1e-9, "diagonal segment start")
				checkClose(c, px.Intervals[1].T1, 5, 1e-9, "diagonal segment end")
			}
		}()

		// --- shared horizontal edge: pixel centers exactly on it count once
		func() {
			// two triangles forming a quad; the shared edge maps to the
			// horizontal screen line y = 4.5 through pixel centers
			mesh := VolumeMesh{
				Positions: []Vector{
					{-1, -0.25, 1}, {1, -0.25, 1}, {0, 0.75, 1}, {0, -1.25, 1},
				},
				Faces: [][3]int{{0, 1, 2}, {1, 0, 3}},
			}
			scene := &VolumeScene{
				Meshes:    []VolumeMesh{mesh},
				Instances: []VolumeInstance{{Mesh: 0, Matrix: Identity(), Medium: 0}},
				Media:     media,
			}
			dc := opaqueContext(8, 8, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "horizontal edge capture: %v", err)
			if err != nil {
				return
			}
			for _, x := range []int{2, 3, 4, 5} {
				px := capture.Pixels[4*8+x]
				c.Check(len(px.Events) == 1,
					"horizontal edge: pixel (%d,4) got %d events, want 1", x, len(px.Events))
			}
			// neighbors above and below the edge are covered once as well
			c.Check(len(capture.Pixels[3*8+3].Events) == 1, "horizontal edge: above pixel")
			c.Check(len(capture.Pixels[5*8+3].Events) == 1, "horizontal edge: below pixel")
		}()

		// --- clip fan: pixel center exactly on a clip-fan internal edge
		func() {
			// single triangle crossing the viewport's right edge; clipping
			// builds a fan whose internal edge passes through pixel (4,4).
			mesh := VolumeMesh{
				Positions: []Vector{{-1, -1, 1}, {3, -1, 1}, {0, 4.4, 1}},
				Faces:     [][3]int{{0, 1, 2}},
			}
			scene := &VolumeScene{
				Meshes:    []VolumeMesh{mesh},
				Instances: []VolumeInstance{{Mesh: 0, Matrix: Identity(), Medium: 0}},
				Media:     media,
			}
			dc := opaqueContext(8, 8, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "clip fan capture: %v", err)
			if err != nil {
				return
			}
			px := capture.Pixels[4*8+4]
			c.Check(len(px.Events) == 1, "clip fan: got %d events at fan edge pixel, want 1", len(px.Events))
			px2 := capture.Pixels[4*8+5]
			c.Check(len(px2.Events) == 1, "clip fan: got %d events inside, want 1", len(px2.Events))
			px3 := capture.Pixels[0]
			c.Check(len(px3.Events) == 0, "clip fan: got %d events outside, want 0", len(px3.Events))
		}()

		// --- nested cavity (reversed inner shell)
		func() {
			scene, refs := sceneFromGroups([]shellGroup{
				{shells: []refShell{
					{Vector{-1.5, -1.5, -1.5}, Vector{1.5, 1.5, 1.5}, 1},
					{Vector{-0.5, -0.5, -0.5}, Vector{0.5, 0.5, 0.5}, -1},
				}, matrix: Identity(), medium: 0},
			}, media)
			dc := opaqueContext(32, 32, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "cavity capture: %v", err)
			if err != nil {
				return
			}
			px := capture.Pixels[16*32+17]
			c.Check(len(px.Events) == 4, "cavity: got %d events, want 4", len(px.Events))
			c.Check(len(px.Intervals) == 5, "cavity: got %d intervals, want 5", len(px.Intervals))
			if len(px.Intervals) == 5 {
				checkClose(c, px.Intervals[1].T0, 2.5, 1e-9, "cavity outer entry")
				checkClose(c, px.Intervals[1].T1, 3.5, 1e-9, "cavity inner entry")
				checkClose(c, px.Intervals[2].T1, 4.5, 1e-9, "cavity inner exit")
				checkClose(c, px.Intervals[3].T1, 5.5, 1e-9, "cavity outer exit")
				c.Check(len(px.Intervals[1].Instances) == 1, "cavity: shell region active")
				c.Check(len(px.Intervals[2].Instances) == 0, "cavity: hole region empty")
				c.Check(len(px.Intervals[3].Instances) == 1, "cavity: far shell region active")
			}
			img, _ := TransportVolume(capture)
			checkTransport(c, capture, img, refs, 17, 16, 1e-12)
		}()

		// --- same-direction nested shells: one interior, no inner boundaries
		func() {
			scene, refs := sceneFromGroups([]shellGroup{
				{shells: []refShell{
					{Vector{-1.5, -1.5, -1.5}, Vector{1.5, 1.5, 1.5}, 1},
					{Vector{-0.5, -0.5, -0.5}, Vector{0.5, 0.5, 0.5}, 1},
				}, matrix: Identity(), medium: 0},
			}, media)
			dc := opaqueContext(32, 32, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "nesting capture: %v", err)
			if err != nil {
				return
			}
			px := capture.Pixels[16*32+17]
			c.Check(len(px.Events) == 4, "nesting: got %d events, want 4", len(px.Events))
			c.Check(len(px.Intervals) == 3, "nesting: got %d intervals, want 3 (winding-preserving crossings merged)", len(px.Intervals))
			if len(px.Intervals) == 3 {
				checkClose(c, px.Intervals[1].T0, 2.5, 1e-9, "nesting entry")
				checkClose(c, px.Intervals[1].T1, 5.5, 1e-9, "nesting exit")
				c.Check(len(px.Intervals[1].Instances) == 1, "nesting: continuous interior")
			}
			img, _ := TransportVolume(capture)
			checkTransport(c, capture, img, refs, 17, 16, 1e-12)
		}()

		// --- interleaved different media with additive coefficients
		func() {
			scene, refs := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -0.8}, Vector{1, 1, 0.2}, 1}}, matrix: Identity(), medium: 0},
				{shells: []refShell{{Vector{-1, -1, -0.2}, Vector{1, 1, 0.8}, 1}}, matrix: Identity(), medium: 1},
			}, media)
			dc := opaqueContext(32, 32, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "interleaved capture: %v", err)
			if err != nil {
				return
			}
			px := capture.Pixels[16*32+17]
			c.Check(len(px.Intervals) == 5, "interleaved: got %d intervals, want 5", len(px.Intervals))
			if len(px.Intervals) == 5 {
				c.Check(intSlicesEqual(px.Intervals[1].Instances, []int{1}), "interleaved seg1 active")
				c.Check(intSlicesEqual(px.Intervals[2].Instances, []int{0, 1}), "interleaved seg2 active")
				c.Check(intSlicesEqual(px.Intervals[3].Instances, []int{0}), "interleaved seg3 active")
				checkClose(c, px.Intervals[1].T0, 3.2, 1e-9, "interleaved t1")
				checkClose(c, px.Intervals[2].T0, 3.8, 1e-9, "interleaved t2")
				checkClose(c, px.Intervals[3].T0, 4.2, 1e-9, "interleaved t3")
				checkClose(c, px.Intervals[3].T1, 4.8, 1e-9, "interleaved t4")
			}
			img, _ := TransportVolume(capture)
			checkTransport(c, capture, img, refs, 17, 16, 1e-12)
			checkTransport(c, capture, img, refs, 20, 10, 1e-12)
		}()

		// --- two instances sharing one medium index still contribute twice
		func() {
			scene, refs := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -0.8}, Vector{1, 1, 0.2}, 1}}, matrix: Identity(), medium: 0},
				{shells: []refShell{{Vector{-1, -1, -0.2}, Vector{1, 1, 0.8}, 1}}, matrix: Identity(), medium: 0},
			}, media)
			dc := opaqueContext(32, 32, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "shared-medium capture: %v", err)
			if err != nil {
				return
			}
			px := capture.Pixels[16*32+17]
			c.Check(len(px.Intervals) == 5, "shared-medium: got %d intervals, want 5", len(px.Intervals))
			if len(px.Intervals) == 5 {
				c.Check(intSlicesEqual(px.Intervals[2].Instances, []int{0, 1}),
					"shared-medium: overlap active set %v", px.Intervals[2].Instances)
			}
			img, _ := TransportVolume(capture)
			checkTransport(c, capture, img, refs, 17, 16, 1e-12)
			// the overlap must see doubled coefficients from the one medium:
			// T = exp(-sigma*0.6) * exp(-2*sigma*0.4) * exp(-sigma*0.6)
			i := 16*32 + 17
			wantT := math.Exp(-2 * media[0].Sigma.X)
			checkClose(c, img.Transmittance[i].X, wantT, 1e-12, "shared-medium: transmittance sees doubled sigma")
		}()

		// --- mirrored instance keeps its interior
		func() {
			mirror := Identity().Scale(Vector{-1, 1, 1}).Translate(Vector{0.3, 0, 0})
			scene, refs := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -1}, Vector{1, 1, 1}, 1}}, matrix: mirror, medium: 0},
			}, media)
			dc := opaqueContext(32, 32, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "mirror capture: %v", err)
			if err != nil {
				return
			}
			px := capture.Pixels[16*32+17]
			c.Check(len(px.Events) == 2, "mirror: got %d events, want 2", len(px.Events))
			if len(px.Events) == 2 {
				c.Check(px.Events[0].Delta == -1 && px.Events[1].Delta == 1,
					"mirror: flipped deltas %d, %d", px.Events[0].Delta, px.Events[1].Delta)
			}
			c.Check(len(px.Intervals) == 3, "mirror: got %d intervals, want 3", len(px.Intervals))
			if len(px.Intervals) == 3 {
				checkClose(c, px.Intervals[1].T0, 3, 1e-9, "mirror entry")
				checkClose(c, px.Intervals[1].T1, 5, 1e-9, "mirror exit")
				c.Check(len(px.Intervals[1].Instances) == 1, "mirror: interior kept")
			}
			img, _ := TransportVolume(capture)
			checkTransport(c, capture, img, refs, 17, 16, 1e-12)
		}()

		// --- oblique perspective view: world lengths along the ray
		func() {
			eye := Vector{4, 3, 6}
			cam := LookAt(eye, Vector{}, Vector{0, 1, 0}).Perspective(50, 1, 1, 12)
			scene, refs := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -1}, Vector{1, 1, 1}, 1}}, matrix: Identity(), medium: 0},
			}, media)
			dc := opaqueContext(32, 32, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, cam)
			c.Check(err == nil, "oblique capture: %v", err)
			if err != nil {
				return
			}
			// independent ray construction
			fwd := eye.Negate().Normalize()
			right := fwd.Cross(Vector{0, 1, 0}).Normalize()
			up2 := right.Cross(fwd)
			halfH := math.Tan(25 * math.Pi / 180)
			pixels := [][2]int{{16, 16}, {8, 8}, {20, 11}, {11, 25}, {25, 22}}
			for _, pxy := range pixels {
				x, y := pxy[0], pxy[1]
				nx := (2*(float64(x)+0.5) - 32) / 32
				ny := (32 - 2*(float64(y)+0.5)) / 32
				pt := eye.Add(fwd).Add(right.MulScalar(nx * halfH)).Add(up2.MulScalar(ny * halfH))
				wantD := pt.Sub(eye).Normalize()
				px := capture.Pixels[y*32+x]
				checkClose(c, px.D.Sub(wantD).Length(), 0, 1e-9, "oblique ray direction")
				checkClose(c, px.O.Sub(pt).Length(), 0, 1e-9, "oblique ray origin")
				checkIntervals(c, capture, refs, x, y, 1e-9)
			}
			// world length of the segment equals the slab length along the ray
			px := capture.Pixels[16*32+16]
			segs := refPixelSegments(refs, capture.Media, px.O, px.D, px.TEnd)
			sameCount := len(px.Intervals) == len(segs)
			c.Check(sameCount, "oblique: interval count %d want %d", len(px.Intervals), len(segs))
			if sameCount {
				for i, iv := range px.Intervals {
					checkClose(c, iv.T1-iv.T0, segs[i].t1-segs[i].t0, 1e-9, "oblique world length")
				}
			}
			img, _ := TransportVolume(capture)
			checkTransport(c, capture, img, refs, 16, 16, 1e-9)
		}()

		// --- near plane cutting inside the body keeps the interval
		func() {
			scene, refs := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -1}, Vector{1, 1, 4.5}, 1}}, matrix: Identity(), medium: 0},
			}, media)
			dc := opaqueContext(32, 32, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "near capture: %v", err)
			if err != nil {
				return
			}
			px := capture.Pixels[16*32+17]
			c.Check(len(px.Events) == 2, "near: got %d events, want 2", len(px.Events))
			if len(px.Events) == 2 {
				checkClose(c, px.Events[0].T, -0.5, 1e-9, "near: crossing before near plane kept")
			}
			c.Check(len(px.Intervals) == 2, "near: got %d intervals, want 2", len(px.Intervals))
			if len(px.Intervals) == 2 {
				checkClose(c, px.Intervals[0].T0, 0, 1e-12, "near: interval starts at near plane")
				checkClose(c, px.Intervals[0].T1, 5, 1e-9, "near: interval end")
				c.Check(len(px.Intervals[0].Instances) == 1, "near: medium from the near plane on")
			}
			img, _ := TransportVolume(capture)
			checkTransport(c, capture, img, refs, 17, 16, 1e-12)
		}()

		// --- far plane cutting inside the body keeps the interval
		func() {
			scene, refs := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -6}, Vector{1, 1, 2}, 1}}, matrix: Identity(), medium: 0},
			}, media)
			dc := opaqueContext(32, 32, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "far capture: %v", err)
			if err != nil {
				return
			}
			px := capture.Pixels[16*32+17]
			c.Check(len(px.Intervals) == 2, "far: got %d intervals, want 2", len(px.Intervals))
			if len(px.Intervals) == 2 {
				checkClose(c, px.Intervals[1].T0, 2, 1e-9, "far: interval start")
				checkClose(c, px.Intervals[1].T1, 9, 1e-9, "far: interval ends at far plane")
				c.Check(px.Intervals[1].EndEvent == -1, "far: truncated interval has no end event")
				c.Check(len(px.Intervals[1].Instances) == 1, "far: medium up to the far plane")
			}
			img, _ := TransportVolume(capture)
			checkTransport(c, capture, img, refs, 17, 16, 1e-12)
		}()

		// --- truncation by a real opaque surface from the normal pipeline
		func() {
			scene, refs := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -1}, Vector{1, 1, 1}, 1}}, matrix: Identity(), medium: 0},
			}, media)
			dc := opaqueContext(32, 32, Color{0.2, 0.4, 0.6, 1})
			cam := orthoCamera()
			dc.Cull = CullNone
			quadColor := Color{0.5, 0.3, 0.1, 1}
			dc.Shader = NewSolidColorShader(cam, quadColor)
			q1 := NewTriangleForPoints(Vector{-2, -1, 0.5}, Vector{0, -1, 0.5}, Vector{0, 1, 0.5})
			q2 := NewTriangleForPoints(Vector{-2, -1, 0.5}, Vector{0, 1, 0.5}, Vector{-2, 1, 0.5})
			info := dc.DrawTriangles([]*Triangle{q1, q2})
			c.Check(info.UpdatedPixels > 0, "opaque quad drawn")
			capture, err := CaptureVolume(dc, scene, cam)
			c.Check(err == nil, "surface capture: %v", err)
			if err != nil {
				return
			}
			left := capture.Pixels[16*32+8] // behind the quad
			c.Check(len(left.Intervals) == 2, "surface: left got %d intervals, want 2", len(left.Intervals))
			if len(left.Intervals) == 2 {
				checkClose(c, left.Intervals[1].T0, 3, 1e-9, "surface: segment start")
				checkClose(c, left.Intervals[1].T1, 3.5, 1e-9, "surface: truncated at opaque surface")
			}
			checkClose(c, left.TEnd, 3.5, 1e-9, "surface: tEnd at surface")
			checkClose(c, left.Bg.X, 127.0/255, 1e-12, "surface: bg r from quad bytes")
			checkClose(c, left.Bg.Y, 76.0/255, 1e-12, "surface: bg g from quad bytes")
			checkClose(c, left.Bg.Z, 25.0/255, 1e-12, "surface: bg b from quad bytes")
			right := capture.Pixels[16*32+22] // clear of the quad
			c.Check(len(right.Intervals) == 3, "surface: right got %d intervals, want 3", len(right.Intervals))
			if len(right.Intervals) == 3 {
				checkClose(c, right.Intervals[1].T1, 5, 1e-9, "surface: untruncated segment")
			}
			checkClose(c, right.Bg.X, 0.2, 1e-12, "surface: clear bg r")
			img, _ := TransportVolume(capture)
			checkTransport(c, capture, img, refs, 8, 16, 1e-12)
			checkTransport(c, capture, img, refs, 22, 16, 1e-12)
		}()

		// --- thin interval stability (series path of the segment formulas)
		func() {
			scene, refs := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -0.001}, Vector{1, 1, 0.001}, 1}}, matrix: Identity(), medium: 0},
			}, []VolumeMedium{{Sigma: Vector{0.02, 3, 0}, J: Vector{1.5, 0.8, 2}}})
			dc := opaqueContext(16, 16, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "thin capture: %v", err)
			if err != nil {
				return
			}
			img, _ := TransportVolume(capture)
			checkTransport(c, capture, img, refs, 9, 9, 1e-12)
			checkTransport(c, capture, img, refs, 3, 12, 1e-12)
		}()

		// --- empty scene reproduces the background exactly
		func() {
			scene := &VolumeScene{Media: media}
			dc := opaqueContext(8, 8, Color{0.2, 0.4, 0.6, 1})
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "empty capture: %v", err)
			if err != nil {
				return
			}
			img, _ := TransportVolume(capture)
			bg := Vector{0.2, 0.4, 0.6}
			exact := true
			unitT := true
			for i := range img.Color {
				if img.Color[i] != bg {
					exact = false
				}
				if img.Transmittance[i] != (Vector{1, 1, 1}) {
					unitT = false
				}
			}
			c.Check(exact, "empty media: color must equal the background exactly")
			c.Check(unitT, "empty media: transmittance must be exactly 1")
		}()

		// --- validation errors
		func() {
			valid := func() (*VolumeScene, *Context, Matrix) {
				scene, _ := sceneFromGroups([]shellGroup{
					{shells: []refShell{{Vector{-1, -1, -1}, Vector{1, 1, 1}, 1}}, matrix: Identity(), medium: 0},
				}, []VolumeMedium{{Sigma: Vector{0.5, 0.2, 0.3}, J: Vector{1, 1, 1}}})
				return scene, opaqueContext(8, 8, Color{0.2, 0.4, 0.6, 1}), orthoCamera()
			}
			expectErr := func(name string, mutate func(*VolumeScene, *Context, *Matrix)) {
				scene, dc, cam := valid()
				mutate(scene, dc, &cam)
				cap2, err := CaptureVolume(dc, scene, cam)
				c.Check(err != nil && cap2 == nil, "validation: %s must fail", name)
			}
			expectErr("face index out of range", func(s *VolumeScene, dc *Context, cam *Matrix) {
				s.Meshes[0].Faces[0][0] = 99
			})
			expectErr("negative face index", func(s *VolumeScene, dc *Context, cam *Matrix) {
				s.Meshes[0].Faces[0][0] = -1
			})
			expectErr("non-finite position", func(s *VolumeScene, dc *Context, cam *Matrix) {
				s.Meshes[0].Positions[0].X = math.NaN()
			})
			expectErr("mesh index out of range", func(s *VolumeScene, dc *Context, cam *Matrix) {
				s.Instances[0].Mesh = 7
			})
			expectErr("medium index out of range", func(s *VolumeScene, dc *Context, cam *Matrix) {
				s.Instances[0].Medium = -1
			})
			expectErr("singular instance matrix", func(s *VolumeScene, dc *Context, cam *Matrix) {
				s.Instances[0].Matrix = Scale(Vector{1, 0, 1})
			})
			expectErr("non-affine instance matrix", func(s *VolumeScene, dc *Context, cam *Matrix) {
				s.Instances[0].Matrix.X33 = 2
			})
			expectErr("negative sigma", func(s *VolumeScene, dc *Context, cam *Matrix) {
				s.Media[0].Sigma.Y = -0.1
			})
			expectErr("non-finite emission", func(s *VolumeScene, dc *Context, cam *Matrix) {
				s.Media[0].J.X = math.NaN()
			})
			expectErr("singular camera", func(s *VolumeScene, dc *Context, cam *Matrix) {
				*cam = Scale(Vector{1, 1, 0})
			})
			expectErr("non-opaque background", func(s *VolumeScene, dc *Context, cam *Matrix) {
				dc.ClearColorBufferWith(Color{0.2, 0.4, 0.6, 0.5})
			})
			expectErr("negative depth", func(s *VolumeScene, dc *Context, cam *Matrix) {
				dc.DepthBuffer[0] = -0.5
			})
			expectErr("depth above 1", func(s *VolumeScene, dc *Context, cam *Matrix) {
				dc.DepthBuffer[3] = 1.5
			})
			expectErr("NaN depth", func(s *VolumeScene, dc *Context, cam *Matrix) {
				dc.DepthBuffer[5] = math.NaN()
			})
			// MaxFloat64 depth is valid (background at the far plane)
			scene, dc, cam := valid()
			_, err := CaptureVolume(dc, scene, cam)
			c.Check(err == nil, "validation: valid scene must pass, got %v", err)
		}()
	})
}

// ---------------------------------------------------------------------------
// TestVolumeAdjoint
// ---------------------------------------------------------------------------

func TestVolumeAdjoint(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {

		// --- hand-computed single-segment derivatives, incl. zero extinction
		func() {
			dc := opaqueContext(8, 8, Color{0.2, 0.4, 0.6, 1})
			cam := orthoCamera()
			scene, _ := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -1}, Vector{1, 1, 1}, 1}}, matrix: Identity(), medium: 0},
			}, []VolumeMedium{{Sigma: Vector{0.5, 0, 0.3}, J: Vector{2, 1, 0.5}}})
			capture, err := CaptureVolume(dc, scene, cam)
			c.Check(err == nil, "analytic capture: %v", err)
			if err != nil {
				return
			}
			img, rec := TransportVolume(capture)

			// pixel (5,4): world (0.75, -0.25), segment [3,5], l = 2
			const px, py = 5, 4
			idx := py*8 + px
			l := 2.0
			bg := Vector{0.2, 0.4, 0.6}
			sig := Vector{0.5, 0, 0.3}
			jem := Vector{2, 1, 0.5}
			wC := Vector{0.7, -0.4, 0.3}
			wT := Vector{0.2, 0.5, -0.6}

			var wantC, wantT, wantSigma, wantJ Vector
			var dl float64
			for ch := 0; ch < 3; ch++ {
				s := volAt(sig, ch)
				j := volAt(jem, ch)
				e := math.Exp(-s * l)
				g := l
				gs := -l * l / 2
				if s != 0 {
					g = (1 - e) / s
					gs = (l*e*s - (1 - e)) / (s * s)
				}
				setCh(&wantC, ch, j*g+e*volAt(bg, ch))
				setCh(&wantT, ch, e)
				wc := volAt(wC, ch)
				wt := volAt(wT, ch)
				setCh(&wantSigma, ch, wc*(j*gs-l*e*volAt(bg, ch))-wt*l*e)
				setCh(&wantJ, ch, wc*g)
				dl += wc*(j*e-s*e*volAt(bg, ch)) - wt*s*e
			}
			for ch := 0; ch < 3; ch++ {
				checkClose(c, volAt(img.Color[idx], ch), volAt(wantC, ch), 1e-12, "analytic color")
				checkClose(c, volAt(img.Transmittance[idx], ch), volAt(wantT, ch), 1e-12, "analytic transmittance")
			}

			dColor := make([]Vector, 64)
			dTrans := make([]Vector, 64)
			dColor[idx] = wC
			dTrans[idx] = wT
			grad, err := AdjointVolume(rec, dColor, dTrans)
			c.Check(err == nil, "analytic adjoint: %v", err)
			if err != nil {
				return
			}
			for ch := 0; ch < 3; ch++ {
				checkClose(c, volAt(grad.Sigma[0], ch), volAt(wantSigma, ch), 1e-9, "analytic dL/dsigma")
				checkClose(c, volAt(grad.J[0], ch), volAt(wantJ, ch), 1e-9, "analytic dL/dj")
			}
			// entry face (4,5,6) bary (0.125, 0.5, 0.375); exit face (0,2,1)
			// bary (0.125, 0.375, 0.5); dt/dV = bary * N / (D.N)
			entryV := [3]int{4, 5, 6}
			entryB := [3]float64{0.125, 0.5, 0.375}
			exitV := [3]int{0, 2, 1}
			exitB := [3]float64{0.125, 0.375, 0.5}
			wantPos := make([]Vector, 8)
			for k := 0; k < 3; k++ {
				wantPos[entryV[k]].Z += dl * entryB[k]
				wantPos[exitV[k]].Z -= dl * exitB[k]
			}
			c.Check(len(grad.Positions) == 1 && len(grad.Positions[0]) == 8, "analytic: positions layout")
			for v := 0; v < 8; v++ {
				checkClose(c, grad.Positions[0][v].X, 0, 1e-12, "analytic vertex grad x")
				checkClose(c, grad.Positions[0][v].Y, 0, 1e-12, "analytic vertex grad y")
				checkClose(c, grad.Positions[0][v].Z, wantPos[v].Z, 1e-9, "analytic vertex grad z")
			}
		}()

		// --- finite differences on a complex scene: shared mesh, mirror,
		//     shear, non-uniform scale, zero extinction, perspective
		func() {
			shear := Matrix{
				1, 0.18, 0.07, 0,
				0.05, 1, 0.12, 0,
				0.02, 0.09, 1, 0,
				0, 0, 0, 1,
			}
			m0 := Identity().Scale(Vector{1.05, 0.8, 1.25})
			m0 = shear.Mul(m0)
			m0 = m0.Rotate(Vector{0.26, 0.9, 0.13}, 0.35)
			m0 = m0.Translate(Vector{0.25, -0.15, 0.05})
			m1 := Identity().Scale(Vector{0.9, 1.1, 0.75})
			m1 = m1.Rotate(Vector{0, 1, 0}, -0.5)
			m1 = m1.Scale(Vector{-1, 1, 1})
			m1 = m1.Translate(Vector{-0.45, 0.35, -0.3})
			mesh := VolumeMesh{boxPositions(Vector{-1, -1, -1}, Vector{1, 1, 1}), boxFaces()}
			scene := &VolumeScene{
				Meshes: []VolumeMesh{mesh},
				Instances: []VolumeInstance{
					{Mesh: 0, Matrix: m0, Medium: 0},
					{Mesh: 0, Matrix: m1, Medium: 1},
				},
				Media: []VolumeMedium{
					{Sigma: Vector{0.4, 0, 0.25}, J: Vector{0.9, 0.4, 0.2}},
					{Sigma: Vector{0.15, 0.35, 0}, J: Vector{0.1, 0.5, 0.8}},
				},
			}
			dc := opaqueContext(40, 40, Color{0.2, 0.25, 0.3, 1})
			cam := LookAt(Vector{2.6, 2.0, 4.0}, Vector{}, Vector{0, 1, 0}).Perspective(42, 1, 1.5, 12)
			n := 40 * 40
			wC := volumeWeights(n, 1.7)
			wT := volumeWeights(n, 4.2)
			capture, err := CaptureVolume(dc, scene, cam)
			c.Check(err == nil, "fd capture: %v", err)
			if err != nil {
				return
			}
			_, rec := TransportVolume(capture)
			grad, err := AdjointVolume(rec, wC, wT)
			c.Check(err == nil, "fd adjoint: %v", err)
			if err != nil {
				return
			}
			// vertices shared across faces and both instances
			for v := 0; v < 8; v++ {
				for ch := 0; ch < 3; ch++ {
					v, ch := v, ch
					f := func(h float64) float64 {
						p := scene.Meshes[0].Positions[v]
						switch ch {
						case 0:
							p.X += h
						case 1:
							p.Y += h
						default:
							p.Z += h
						}
						scene.Meshes[0].Positions[v] = p
						L := volumeLoss(scene, dc, cam, wC, wT)
						p = scene.Meshes[0].Positions[v]
						switch ch {
						case 0:
							p.X -= h
						case 1:
							p.Y -= h
						default:
							p.Z -= h
						}
						scene.Meshes[0].Positions[v] = p
						return L
					}
					got := volAt(grad.Positions[0][v], ch)
					checkFD(c, got, f, true, "vertex gradient")
				}
			}
			// medium coefficients
			for m := 0; m < 2; m++ {
				for ch := 0; ch < 3; ch++ {
					m, ch := m, ch
					fs := func(h float64) float64 {
						s := scene.Media[m].Sigma
						setCh(&s, ch, volAt(s, ch)+h)
						scene.Media[m].Sigma = s
						L := volumeLoss(scene, dc, cam, wC, wT)
						s = scene.Media[m].Sigma
						setCh(&s, ch, volAt(s, ch)-h)
						scene.Media[m].Sigma = s
						return L
					}
					central := volAt(scene.Media[m].Sigma, ch) > 0
					checkFD(c, volAt(grad.Sigma[m], ch), fs, central, "sigma gradient")
					fj := func(h float64) float64 {
						j := scene.Media[m].J
						setCh(&j, ch, volAt(j, ch)+h)
						scene.Media[m].J = j
						L := volumeLoss(scene, dc, cam, wC, wT)
						j = scene.Media[m].J
						setCh(&j, ch, volAt(j, ch)-h)
						scene.Media[m].J = j
						return L
					}
					checkFD(c, volAt(grad.J[m], ch), fj, true, "emission gradient")
				}
			}
		}()

		// --- thin interval adjoint stability
		func() {
			scene, _ := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -0.001}, Vector{1, 1, 0.001}, 1}}, matrix: Identity(), medium: 0},
			}, []VolumeMedium{{Sigma: Vector{0.02, 3, 0}, J: Vector{1.5, 0.8, 2}}})
			dc := opaqueContext(16, 16, Color{0.2, 0.4, 0.6, 1})
			cam := orthoCamera()
			n := 16 * 16
			wC := volumeWeights(n, 0.3)
			wT := volumeWeights(n, 2.9)
			capture, err := CaptureVolume(dc, scene, cam)
			c.Check(err == nil, "thin capture: %v", err)
			if err != nil {
				return
			}
			_, rec := TransportVolume(capture)
			grad, err := AdjointVolume(rec, wC, wT)
			c.Check(err == nil, "thin adjoint: %v", err)
			if err != nil {
				return
			}
			for v := 0; v < 8; v++ {
				for ch := 0; ch < 3; ch++ {
					v, ch := v, ch
					f := func(h float64) float64 {
						p := scene.Meshes[0].Positions[v]
						switch ch {
						case 0:
							p.X += h
						case 1:
							p.Y += h
						default:
							p.Z += h
						}
						scene.Meshes[0].Positions[v] = p
						L := volumeLoss(scene, dc, cam, wC, wT)
						p = scene.Meshes[0].Positions[v]
						switch ch {
						case 0:
							p.X -= h
						case 1:
							p.Y -= h
						default:
							p.Z -= h
						}
						scene.Meshes[0].Positions[v] = p
						return L
					}
					got := volAt(grad.Positions[0][v], ch)
					// thin slab: use smaller steps to stay inside the slab
					h1, h2 := 1e-5, 1e-7
					r1 := (f(h1) - f(-h1)) / (2 * h1)
					r2 := (f(h2) - f(-h2)) / (2 * h2)
					tol1 := 3e-5 + 3e-5*math.Abs(r1)
					tol2 := 3e-5 + 3e-5*math.Abs(r2)
					c.Check(math.Abs(got-r1) <= tol1, "thin vertex: %.9g vs %.9g", got, r1)
					c.Check(math.Abs(got-r2) <= tol2, "thin vertex: %.9g vs %.9g", got, r2)
				}
			}
			for ch := 0; ch < 3; ch++ {
				ch := ch
				fs := func(h float64) float64 {
					s := scene.Media[0].Sigma
					setCh(&s, ch, volAt(s, ch)+h)
					scene.Media[0].Sigma = s
					L := volumeLoss(scene, dc, cam, wC, wT)
					s = scene.Media[0].Sigma
					setCh(&s, ch, volAt(s, ch)-h)
					scene.Media[0].Sigma = s
					return L
				}
				central := volAt(scene.Media[0].Sigma, ch) > 0
				checkFD(c, volAt(grad.Sigma[0], ch), fs, central, "thin sigma")
				fj := func(h float64) float64 {
					j := scene.Media[0].J
					setCh(&j, ch, volAt(j, ch)+h)
					scene.Media[0].J = j
					L := volumeLoss(scene, dc, cam, wC, wT)
					j = scene.Media[0].J
					setCh(&j, ch, volAt(j, ch)-h)
					scene.Media[0].J = j
					return L
				}
				checkFD(c, volAt(grad.J[0], ch), fj, true, "thin emission")
			}
		}()

		// --- same-direction nested shells: inner shell gets no false gradient
		func() {
			build := func(reversed bool) (*VolumeScene, *Context, Matrix) {
				sign := 1.0
				if reversed {
					sign = -1
				}
				scene, _ := sceneFromGroups([]shellGroup{
					{shells: []refShell{
						{Vector{-1, -1, -1}, Vector{1, 1, 1}, 1},
						{Vector{-0.6, -0.6, -0.6}, Vector{0.6, 0.6, 0.6}, sign},
					}, matrix: Identity(), medium: 0},
				}, []VolumeMedium{{Sigma: Vector{0.3, 0.2, 0.1}, J: Vector{0.5, 0.3, 0.2}}})
				return scene, opaqueContext(24, 24, Color{0.2, 0.4, 0.6, 1}), orthoCamera()
			}
			n := 24 * 24
			wC := volumeWeights(n, 5.1)
			wT := volumeWeights(n, 0.8)

			// nested same-direction: inner vertices must be exactly zero
			scene, dc, cam := build(false)
			capture, err := CaptureVolume(dc, scene, cam)
			c.Check(err == nil, "nested capture: %v", err)
			if err == nil {
				_, rec := TransportVolume(capture)
				grad, err := AdjointVolume(rec, wC, wT)
				c.Check(err == nil, "nested adjoint: %v", err)
				if err == nil {
					zeroInner := true
					for v := 8; v < 16; v++ {
						if grad.Positions[0][v] != (Vector{}) {
							zeroInner = false
						}
					}
					c.Check(zeroInner, "nested: inner shell vertices must have zero gradient")
					// outer vertices still get the true gradient: check two by FD
					for _, v := range []int{0, 6} {
						for ch := 0; ch < 3; ch++ {
							v, ch := v, ch
							f := func(h float64) float64 {
								p := scene.Meshes[0].Positions[v]
								setCh(&p, ch, volAt(p, ch)+h)
								scene.Meshes[0].Positions[v] = p
								L := volumeLoss(scene, dc, cam, wC, wT)
								p = scene.Meshes[0].Positions[v]
								setCh(&p, ch, volAt(p, ch)-h)
								scene.Meshes[0].Positions[v] = p
								return L
							}
							checkFD(c, volAt(grad.Positions[0][v], ch), f, true, "nested outer vertex")
						}
					}
				}
			}

			// cavity: inner shell vertices get a real gradient
			scene2, dc2, cam2 := build(true)
			capture2, err := CaptureVolume(dc2, scene2, cam2)
			c.Check(err == nil, "cavity capture: %v", err)
			if err == nil {
				_, rec := TransportVolume(capture2)
				grad, err := AdjointVolume(rec, wC, wT)
				c.Check(err == nil, "cavity adjoint: %v", err)
				if err == nil {
					nonzero := false
					for v := 8; v < 16; v++ {
						if grad.Positions[0][v] != (Vector{}) {
							nonzero = true
						}
					}
					c.Check(nonzero, "cavity: inner shell vertices must have nonzero gradient")
					for _, v := range []int{8, 13} {
						for ch := 0; ch < 3; ch++ {
							v, ch := v, ch
							f := func(h float64) float64 {
								p := scene2.Meshes[0].Positions[v]
								setCh(&p, ch, volAt(p, ch)+h)
								scene2.Meshes[0].Positions[v] = p
								L := volumeLoss(scene2, dc2, cam2, wC, wT)
								p = scene2.Meshes[0].Positions[v]
								setCh(&p, ch, volAt(p, ch)-h)
								scene2.Meshes[0].Positions[v] = p
								return L
							}
							checkFD(c, volAt(grad.Positions[0][v], ch), f, true, "cavity inner vertex")
						}
					}
				}
			}
		}()

		// --- boundaries outside the valid depth range have zero gradient
		func() {
			// box fully behind an opaque surface
			scene, _ := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -1}, Vector{1, 1, 0}, 1}}, matrix: Identity(), medium: 0},
			}, []VolumeMedium{{Sigma: Vector{0.4, 0.3, 0.2}, J: Vector{1, 0.5, 0.2}}})
			dc := opaqueContext(16, 16, Color{0.2, 0.4, 0.6, 1})
			cam := orthoCamera()
			dc.Cull = CullNone
			dc.Shader = NewSolidColorShader(cam, Color{0.5, 0.5, 0.5, 1})
			q1 := NewTriangleForPoints(Vector{-2, -2, 0.5}, Vector{2, -2, 0.5}, Vector{2, 2, 0.5})
			q2 := NewTriangleForPoints(Vector{-2, -2, 0.5}, Vector{2, 2, 0.5}, Vector{-2, 2, 0.5})
			dc.DrawTriangles([]*Triangle{q1, q2})
			n := 16 * 16
			wC := volumeWeights(n, 1.1)
			wT := volumeWeights(n, 3.3)
			capture, err := CaptureVolume(dc, scene, cam)
			c.Check(err == nil, "occluded capture: %v", err)
			if err == nil {
				_, rec := TransportVolume(capture)
				grad, err := AdjointVolume(rec, wC, wT)
				c.Check(err == nil, "occluded adjoint: %v", err)
				if err == nil {
					zero := true
					for v := 0; v < 8; v++ {
						if grad.Positions[0][v] != (Vector{}) {
							zero = false
						}
					}
					c.Check(zero, "occluded: vertex gradients must be zero")
					c.Check(grad.Sigma[0] == (Vector{}) && grad.J[0] == (Vector{}),
						"occluded: medium gradients must be zero")
				}
			}
			// box fully beyond the far plane
			scene2, _ := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -8}, Vector{1, 1, -6}, 1}}, matrix: Identity(), medium: 0},
			}, []VolumeMedium{{Sigma: Vector{0.4, 0.3, 0.2}, J: Vector{1, 0.5, 0.2}}})
			dc2 := opaqueContext(16, 16, Color{0.2, 0.4, 0.6, 1})
			capture2, err := CaptureVolume(dc2, scene2, cam)
			c.Check(err == nil, "beyond-far capture: %v", err)
			if err == nil {
				_, rec := TransportVolume(capture2)
				grad, err := AdjointVolume(rec, wC, wT)
				c.Check(err == nil, "beyond-far adjoint: %v", err)
				if err == nil {
					zero := true
					for v := 0; v < 8; v++ {
						if grad.Positions[0][v] != (Vector{}) {
							zero = false
						}
					}
					c.Check(zero, "beyond-far: vertex gradients must be zero")
					c.Check(grad.Sigma[0] == (Vector{}) && grad.J[0] == (Vector{}),
						"beyond-far: medium gradients must be zero")
				}
			}
		}()
	})
}

// ---------------------------------------------------------------------------
// TestVolumeRecord
// ---------------------------------------------------------------------------

func TestVolumeRecord(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {

		// --- empty media reproduces the background exactly, T == 1
		func() {
			dc := opaqueContext(8, 8, Color{0.2, 0.4, 0.6, 1})
			scene := &VolumeScene{}
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "record empty capture: %v", err)
			if err != nil {
				return
			}
			img, rec := TransportVolume(capture)
			bg := Vector{0.2, 0.4, 0.6}
			exact := true
			for i := range img.Color {
				if img.Color[i] != bg || img.Transmittance[i] != (Vector{1, 1, 1}) {
					exact = false
				}
			}
			c.Check(exact, "empty media: exact background and unit transmittance")
			c.Check(rec.Width == 8 && rec.Height == 8, "record dimensions")
		}()

		// --- the record is self-contained: mutating the scene, the capture
		//     and the buffers afterwards does not change the adjoint
		func() {
			scene, _ := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-1, -1, -1}, Vector{1, 1, 1}, 1}}, matrix: Identity(), medium: 0},
			}, []VolumeMedium{{Sigma: Vector{0.5, 0.2, 0.3}, J: Vector{1, 0.6, 0.2}}})
			dc := opaqueContext(16, 16, Color{0.2, 0.4, 0.6, 1})
			cam := orthoCamera()
			n := 16 * 16
			wC := volumeWeights(n, 0.7)
			wT := volumeWeights(n, 2.2)
			capture, err := CaptureVolume(dc, scene, cam)
			c.Check(err == nil, "record capture: %v", err)
			if err != nil {
				return
			}
			_, rec := TransportVolume(capture)
			g1, err := AdjointVolume(rec, wC, wT)
			c.Check(err == nil, "record adjoint: %v", err)
			if err != nil {
				return
			}
			// mutate everything the record must not depend on
			scene.Meshes[0].Positions[0] = Vector{99, 99, 99}
			scene.Media[0].Sigma = Vector{9, 9, 9}
			scene.Media[0].J = Vector{-9, -9, -9}
			dc.ClearColorBufferWith(Color{0.9, 0.1, 0.1, 1})
			dc.ClearDepthBufferWith(0.25)
			capture.Pixels[0].Bg = Vector{9, 9, 9}
			if len(capture.Pixels[5*16+9].Events) > 0 {
				capture.Pixels[5*16+9].Events[0].T = 999
			}
			capture.InstanceMedium[0] = 0
			g2, err := AdjointVolume(rec, wC, wT)
			c.Check(err == nil, "record adjoint after mutation: %v", err)
			if err != nil {
				return
			}
			same := true
			for m := range g1.Positions {
				for v := range g1.Positions[m] {
					if g1.Positions[m][v] != g2.Positions[m][v] {
						same = false
					}
				}
			}
			for k := range g1.Sigma {
				if g1.Sigma[k] != g2.Sigma[k] || g1.J[k] != g2.J[k] {
					same = false
				}
			}
			c.Check(same, "record: adjoint must not change after inputs are mutated")
		}()

		// --- interleaved frames and repeated backward do not cross-talk
		func() {
			mk := func(z0, z1 float64, sigma Vector, seed float64) (*VolumeRecord, []Vector, []Vector) {
				scene, _ := sceneFromGroups([]shellGroup{
					{shells: []refShell{{Vector{-1, -1, z0}, Vector{1, 1, z1}, 1}}, matrix: Identity(), medium: 0},
				}, []VolumeMedium{{Sigma: sigma, J: Vector{1, 0.5, 0.2}}})
				dc := opaqueContext(16, 16, Color{0.2, 0.4, 0.6, 1})
				capture, err := CaptureVolume(dc, scene, orthoCamera())
				if err != nil {
					t.Fatal(err)
				}
				_, rec := TransportVolume(capture)
				return rec, volumeWeights(16*16, seed), volumeWeights(16*16, seed+9)
			}
			recA, wCA, wTA := mk(-1, 1, Vector{0.5, 0.2, 0.3}, 0.4)
			recB, wCB, wTB := mk(-0.5, 0.5, Vector{0.1, 0.6, 0.2}, 1.4)
			// interleave the adjoint calls of the two frames
			gA1, errA1 := AdjointVolume(recA, wCA, wTA)
			gB1, errB1 := AdjointVolume(recB, wCB, wTB)
			gA2, errA2 := AdjointVolume(recA, wCA, wTA)
			gB2, errB2 := AdjointVolume(recB, wCB, wTB)
			c.Check(errA1 == nil && errB1 == nil && errA2 == nil && errB2 == nil, "interleave: adjoint errors")
			same := func(a, b *VolumeGradients) bool {
				for m := range a.Positions {
					for v := range a.Positions[m] {
						if a.Positions[m][v] != b.Positions[m][v] {
							return false
						}
					}
				}
				for k := range a.Sigma {
					if a.Sigma[k] != b.Sigma[k] || a.J[k] != b.J[k] {
						return false
					}
				}
				return true
			}
			c.Check(same(gA1, gA2), "interleave: repeated backward of frame A differs")
			c.Check(same(gB1, gB2), "interleave: repeated backward of frame B differs")
			c.Check(!same(gA1, gB1) || true, "interleave sanity")
			diff := false
			for v := range gA1.Positions[0] {
				if gA1.Positions[0][v] != gB1.Positions[0][v] {
					diff = true
				}
			}
			c.Check(diff, "interleave: distinct frames must give distinct gradients")
		}()

		// --- capture/transport/adjoint leave the context buffers untouched
		//     and normal drawing keeps working
		func() {
			dc := NewContext(16, 16)
			dc.ClearColorBufferWith(Color{0.2, 0.4, 0.6, 1})
			dc.Cull = CullNone
			dc.Shader = NewSolidColorShader(Identity(), Color{0.9, 0.3, 0.2, 1})
			tri := NewTriangleForPoints(Vector{-0.8, -0.8, 0.2}, Vector{0.8, -0.7, 0.2}, Vector{0, 0.9, 0.2})
			info := dc.DrawTriangles([]*Triangle{tri})
			c.Check(info.UpdatedPixels > 0, "normal drawing updates pixels")
			pixBefore := append([]byte(nil), dc.ColorBuffer.Pix...)
			depthBefore := append([]float64(nil), dc.DepthBuffer...)

			scene, _ := sceneFromGroups([]shellGroup{
				{shells: []refShell{{Vector{-0.5, -0.5, -0.5}, Vector{0.5, 0.5, 0.5}, 1}}, matrix: Identity(), medium: 0},
			}, []VolumeMedium{{Sigma: Vector{0.4, 0.3, 0.2}, J: Vector{1, 0.5, 0.2}}})
			cam := LookAt(Vector{0, 0, 3}, Vector{}, Vector{0, 1, 0}).Orthographic(-1, 1, -1, 1, 1, 5)
			capture, err := CaptureVolume(dc, scene, cam)
			c.Check(err == nil, "buffers capture: %v", err)
			if err == nil {
				img, rec := TransportVolume(capture)
				_, err := AdjointVolume(rec, volumeWeights(256, 1.1), volumeWeights(256, 2.2))
				c.Check(err == nil, "buffers adjoint: %v", err)
				c.Check(len(img.Color) == 256, "buffers: image size")
			}
			c.Check(bytes.Equal(pixBefore, dc.ColorBuffer.Pix), "color buffer unmodified by volume stages")
			depthSame := true
			for i := range depthBefore {
				if depthBefore[i] != dc.DepthBuffer[i] {
					depthSame = false
				}
			}
			c.Check(depthSame, "depth buffer unmodified by volume stages")

			// normal drawing still behaves: farther triangle is rejected,
			// nearer triangle and DrawMesh update the buffers
			tri2 := NewTriangleForPoints(Vector{-0.8, -0.8, 0.5}, Vector{0.8, -0.7, 0.5}, Vector{0, 0.9, 0.5})
			info2 := dc.DrawTriangles([]*Triangle{tri2})
			c.Check(info2.UpdatedPixels == 0, "depth test still rejects farther triangle")
			mesh := NewTriangleMesh([]*Triangle{
				NewTriangleForPoints(Vector{-0.8, -0.8, 0.1}, Vector{0.8, -0.7, 0.1}, Vector{0, 0.9, 0.1}),
			})
			info3 := dc.DrawMesh(mesh)
			c.Check(info3.UpdatedPixels > 0, "DrawMesh still updates pixels")
		}()

		// --- adjoint input validation
		func() {
			dc := opaqueContext(4, 4, Color{0.2, 0.4, 0.6, 1})
			scene := &VolumeScene{}
			capture, err := CaptureVolume(dc, scene, orthoCamera())
			c.Check(err == nil, "validation capture: %v", err)
			if err != nil {
				return
			}
			_, rec := TransportVolume(capture)
			_, err = AdjointVolume(rec, make([]Vector, 15), make([]Vector, 16))
			c.Check(err != nil, "adjoint: wrong color length must fail")
			_, err = AdjointVolume(rec, make([]Vector, 16), make([]Vector, 15))
			c.Check(err != nil, "adjoint: wrong transmittance length must fail")
			bad := make([]Vector, 16)
			bad[3] = Vector{math.NaN(), 0, 0}
			_, err = AdjointVolume(rec, bad, make([]Vector, 16))
			c.Check(err != nil, "adjoint: non-finite adjoint must fail")
			g, err := AdjointVolume(rec, make([]Vector, 16), make([]Vector, 16))
			c.Check(err == nil && g != nil, "adjoint: zero adjoints must pass")
			if g != nil {
				c.Check(len(g.Sigma) == 0 && len(g.J) == 0 && len(g.Positions) == 0,
					"adjoint: empty scene gradient layout")
			}
		}()
	})
}
