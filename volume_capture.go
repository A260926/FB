package fauxgl

import (
	"fmt"
	"image"
	"math"
	"sort"
)

// VolumeMesh is a closed, oriented triangle mesh with shared vertex
// positions. Outer shells wind counter-clockwise when seen from outside,
// cavity shells wind the other way, and same-direction nested shells
// describe one interior through the non-zero integer winding rule.
// Closedness and consistent orientation are guaranteed by the caller and
// are not validated.
type VolumeMesh struct {
	Positions []Vector // shared source vertex positions
	Faces     [][3]int // triangle corners as indices into Positions
}

// VolumeInstance places one mesh in the world with a fixed invertible
// affine matrix and assigns it a medium. Non-uniform scale, shear and
// mirroring are all allowed.
type VolumeInstance struct {
	Mesh   int    // index into VolumeScene.Meshes
	Matrix Matrix // invertible affine object-to-world matrix
	Medium int    // index into VolumeScene.Media
}

// VolumeMedium describes a homogeneous participating medium: non-negative
// RGB extinction Sigma and RGB emission J, both per unit world length.
type VolumeMedium struct {
	Sigma Vector // RGB extinction coefficient, every component >= 0
	J     Vector // RGB emission per unit world length
}

// VolumeScene is the complete input to CaptureVolume.
type VolumeScene struct {
	Meshes    []VolumeMesh
	Instances []VolumeInstance
	Media     []VolumeMedium
}

// VolumeEvent is one ray/surface crossing produced by rasterization. It
// keeps the identity of the original face, its source vertices and its
// instance, plus everything the adjoint stage needs to differentiate the
// crossing distance.
type VolumeEvent struct {
	T     float64 // world-space distance along the pixel ray (from the near-plane point)
	Delta int     // winding delta: +1 when the ray crosses against the face orientation, -1 otherwise
	Inst  int     // instance index
	Face  int     // face index inside the instance's mesh
	Vtx   [3]int  // global source vertex indices (mesh vertices concatenated in scene order)
	Bary  Vector  // barycentric coordinates of the hit point, X/Y/Z for Vtx[0..2]
	N     Vector  // unnormalized world geometric normal from the face winding
	Dn    float64 // D . N
}

// VolumeInterval is one maximal ray segment with a constant active set.
type VolumeInterval struct {
	T0, T1     float64 // world distances along the pixel ray
	Instances  []int   // active instance indices (non-zero winding), sorted
	StartEvent int     // index into VolumePixel.Events, -1 when starting at the near plane
	EndEvent   int     // index into VolumePixel.Events, -1 when ending at the depth limit
}

// VolumePixel holds the captured data for one pixel-center ray.
type VolumePixel struct {
	O, D      Vector           // ray origin (near plane) and unit direction, world space
	TEnd      float64          // integration limit: min(far plane, background depth) as a world distance
	Bg        Vector           // background linear RGB from the color buffer (byte/255)
	Intervals []VolumeInterval // ordered media intervals covering [0, TEnd]
	Events    []VolumeEvent    // every crossing of the ray, sorted by (T, Inst, Face)
}

// VolumeCapture is the result of the capture stage. It owns all of its
// data; later changes to the scene or to the context buffers do not
// affect it. Pixels are stored row-major: pixel (x, y) is Pixels[y*Width+x].
type VolumeCapture struct {
	Width, Height    int
	Camera           Matrix         // world-to-clip matrix used for the capture
	MeshVertexCounts []int          // vertices per mesh, defines the global vertex indexing
	InstanceMedium   []int          // medium index per instance
	InstanceMatrix   []Matrix       // affine matrix per instance
	Media            []VolumeMedium // copy of the media table
	Pixels           []VolumePixel  // row-major pixels
}

func volumeFinite(v Vector) bool {
	return !math.IsNaN(v.X) && !math.IsNaN(v.Y) && !math.IsNaN(v.Z) &&
		!math.IsInf(v.X, 0) && !math.IsInf(v.Y, 0) && !math.IsInf(v.Z, 0)
}

func volumeFiniteMatrix(m Matrix) bool {
	v := [...]float64{
		m.X00, m.X01, m.X02, m.X03,
		m.X10, m.X11, m.X12, m.X13,
		m.X20, m.X21, m.X22, m.X23,
		m.X30, m.X31, m.X32, m.X33,
	}
	for _, x := range v {
		if math.IsNaN(x) || math.IsInf(x, 0) {
			return false
		}
	}
	return true
}

// Validate checks indices, non-finite values, singular matrices and
// negative extinction coefficients. Topology (closed and oriented) is the
// caller's responsibility and is not checked.
func (s *VolumeScene) Validate() error {
	for mi := range s.Meshes {
		mesh := &s.Meshes[mi]
		for vi, p := range mesh.Positions {
			if !volumeFinite(p) {
				return fmt.Errorf("volume: mesh %d vertex %d is not finite", mi, vi)
			}
		}
		for fi, f := range mesh.Faces {
			for k := 0; k < 3; k++ {
				if f[k] < 0 || f[k] >= len(mesh.Positions) {
					return fmt.Errorf("volume: mesh %d face %d index %d is out of range [0, %d)",
						mi, fi, f[k], len(mesh.Positions))
				}
			}
		}
	}
	for ii, inst := range s.Instances {
		if inst.Mesh < 0 || inst.Mesh >= len(s.Meshes) {
			return fmt.Errorf("volume: instance %d mesh index %d is out of range [0, %d)",
				ii, inst.Mesh, len(s.Meshes))
		}
		if inst.Medium < 0 || inst.Medium >= len(s.Media) {
			return fmt.Errorf("volume: instance %d medium index %d is out of range [0, %d)",
				ii, inst.Medium, len(s.Media))
		}
		m := inst.Matrix
		if !volumeFiniteMatrix(m) {
			return fmt.Errorf("volume: instance %d matrix is not finite", ii)
		}
		if m.X30 != 0 || m.X31 != 0 || m.X32 != 0 || m.X33 != 1 {
			return fmt.Errorf("volume: instance %d matrix is not affine", ii)
		}
		if d := m.Determinant(); d == 0 || math.IsNaN(d) || math.IsInf(d, 0) {
			return fmt.Errorf("volume: instance %d matrix is singular", ii)
		}
	}
	for mi, med := range s.Media {
		if !volumeFinite(med.Sigma) || !volumeFinite(med.J) {
			return fmt.Errorf("volume: medium %d has a non-finite coefficient", mi)
		}
		if med.Sigma.X < 0 || med.Sigma.Y < 0 || med.Sigma.Z < 0 {
			return fmt.Errorf("volume: medium %d has a negative extinction coefficient", mi)
		}
	}
	return nil
}

// volumeTopLeft reports whether the directed edge a->b is a top or left
// edge for a triangle whose interior is on the right of the edge (y-down
// screen coordinates).
func volumeTopLeft(a, b Vector) bool {
	e := b.Sub(a)
	return e.Y < 0 || (e.Y == 0 && e.X > 0)
}

// volumeCoverage calls emit for every pixel center covered by the
// screen-space triangle, using a top-left fill rule so that shared edges
// and clip-fan internal edges are covered exactly once. Degenerate
// (zero-area) triangles cover nothing.
func volumeCoverage(width, height int, s0, s1, s2 Vector, emit func(x, y int)) {
	a := edge(s0, s1, s2)
	if a == 0 {
		return
	}
	if a < 0 {
		s1, s2 = s2, s1
	}
	tl0 := volumeTopLeft(s1, s2)
	tl1 := volumeTopLeft(s2, s0)
	tl2 := volumeTopLeft(s0, s1)
	minX := math.Min(s0.X, math.Min(s1.X, s2.X))
	maxX := math.Max(s0.X, math.Max(s1.X, s2.X))
	minY := math.Min(s0.Y, math.Min(s1.Y, s2.Y))
	maxY := math.Max(s0.Y, math.Max(s1.Y, s2.Y))
	x0 := int(math.Ceil(minX - 0.5))
	x1 := int(math.Floor(maxX - 0.5))
	y0 := int(math.Ceil(minY - 0.5))
	y1 := int(math.Floor(maxY - 0.5))
	if x0 < 0 {
		x0 = 0
	}
	if y0 < 0 {
		y0 = 0
	}
	if x1 > width-1 {
		x1 = width - 1
	}
	if y1 > height-1 {
		y1 = height - 1
	}
	for y := y0; y <= y1; y++ {
		for x := x0; x <= x1; x++ {
			p := Vector{float64(x) + 0.5, float64(y) + 0.5, 0}
			e0 := edge(s1, s2, p)
			if e0 < 0 || (e0 == 0 && !tl0) {
				continue
			}
			e1 := edge(s2, s0, p)
			if e1 < 0 || (e1 == 0 && !tl1) {
				continue
			}
			e2 := edge(s0, s1, p)
			if e2 < 0 || (e2 == 0 && !tl2) {
				continue
			}
			emit(x, y)
		}
	}
}

type volumeEventRef struct {
	inst int
	face int
}

func volumeUnproject(inv Matrix, ndc Vector) Vector {
	p := inv.MulPositionW(ndc)
	return p.Vector().DivScalar(p.W)
}

func volumeActive(wind []int) []int {
	var active []int
	for i, w := range wind {
		if w != 0 {
			active = append(active, i)
		}
	}
	return active
}

// CaptureVolume rasterizes every instance's faces double-sided and turns
// the per-pixel crossing events into media intervals. The context's color
// and depth buffers are only read: the color buffer must be fully opaque
// (alpha 255) and is interpreted as byte/255 linear RGB; the depth buffer
// must hold finite values in [0, 1] or MaxFloat64 (background at the far
// plane). The camera is a conventional finite-near/far perspective or
// orthographic world-to-clip matrix used with Screen's y-down and
// window-z conventions. Invalid inputs produce a non-nil error and a nil
// capture.
func CaptureVolume(ctx *Context, scene *VolumeScene, camera Matrix) (*VolumeCapture, error) {
	if scene == nil {
		return nil, fmt.Errorf("volume: nil scene")
	}
	if err := scene.Validate(); err != nil {
		return nil, err
	}
	if !volumeFiniteMatrix(camera) {
		return nil, fmt.Errorf("volume: camera matrix is not finite")
	}
	if det := camera.Determinant(); det == 0 || math.IsNaN(det) || math.IsInf(det, 0) {
		return nil, fmt.Errorf("volume: camera matrix is singular")
	}
	w, h := ctx.Width, ctx.Height
	if w <= 0 || h <= 0 {
		return nil, fmt.Errorf("volume: bad context size %d x %d", w, h)
	}
	if ctx.ColorBuffer == nil || ctx.ColorBuffer.Rect != image.Rect(0, 0, w, h) {
		return nil, fmt.Errorf("volume: color buffer does not match context size %d x %d", w, h)
	}
	npix := w * h
	if len(ctx.DepthBuffer) != npix {
		return nil, fmt.Errorf("volume: depth buffer length %d does not match %d pixels", len(ctx.DepthBuffer), npix)
	}

	// background snapshot
	bg := make([]Vector, npix)
	depthQ := make([]float64, npix) // ndc z of the background, NaN when at the far plane
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			i := y*w + x
			c := ctx.ColorBuffer.NRGBAAt(x, y)
			if c.A != 255 {
				return nil, fmt.Errorf("volume: color buffer alpha at pixel (%d, %d) is %d, want 255", x, y, c.A)
			}
			bg[i] = Vector{float64(c.R) / 255, float64(c.G) / 255, float64(c.B) / 255}
			d := ctx.DepthBuffer[i]
			switch {
			case d == math.MaxFloat64:
				depthQ[i] = math.NaN()
			case d >= 0 && d <= 1:
				depthQ[i] = 2*d - 1
			default:
				return nil, fmt.Errorf("volume: depth buffer at pixel (%d, %d) is %v, want a finite value in [0, 1] or MaxFloat64", x, y, d)
			}
		}
	}

	screen := Screen(w, h)
	sidePlanes := clipPlanes[:4] // x and y planes only; near/far never clip crossings

	// per-instance world positions and per-pixel face coverage
	worlds := make([][]Vector, len(scene.Instances))
	refs := make([][]volumeEventRef, npix)
	for ii := range scene.Instances {
		inst := &scene.Instances[ii]
		mesh := &scene.Meshes[inst.Mesh]
		world := make([]Vector, len(mesh.Positions))
		for i, p := range mesh.Positions {
			world[i] = inst.Matrix.MulPosition(p)
		}
		worlds[ii] = world
		for fi, f := range mesh.Faces {
			c0 := camera.MulPositionW(world[f[0]])
			c1 := camera.MulPositionW(world[f[1]])
			c2 := camera.MulPositionW(world[f[2]])
			if c0.W <= 0 || c1.W <= 0 || c2.W <= 0 {
				return nil, fmt.Errorf("volume: instance %d face %d has a vertex behind the eye plane (clip.w <= 0)", ii, fi)
			}
			poly := sutherlandHodgman([]VectorW{c0, c1, c2}, sidePlanes)
			if len(poly) < 3 {
				continue
			}
			sp := make([]Vector, len(poly))
			for i, c := range poly {
				sp[i] = screen.MulPosition(c.DivScalar(c.W).Vector())
			}
			ii2, fi2 := ii, fi
			for i := 2; i < len(sp); i++ {
				volumeCoverage(w, h, sp[0], sp[i-1], sp[i], func(x, y int) {
					p := y*w + x
					refs[p] = append(refs[p], volumeEventRef{ii2, fi2})
				})
			}
		}
	}

	// global vertex offsets
	offsets := make([]int, len(scene.Meshes))
	total := 0
	for i, m := range scene.Meshes {
		offsets[i] = total
		total += len(m.Positions)
	}

	capture := &VolumeCapture{
		Width:            w,
		Height:           h,
		Camera:           camera,
		MeshVertexCounts: make([]int, len(scene.Meshes)),
		InstanceMedium:   make([]int, len(scene.Instances)),
		InstanceMatrix:   make([]Matrix, len(scene.Instances)),
		Media:            append([]VolumeMedium(nil), scene.Media...),
		Pixels:           make([]VolumePixel, npix),
	}
	for i, m := range scene.Meshes {
		capture.MeshVertexCounts[i] = len(m.Positions)
	}
	for i, inst := range scene.Instances {
		capture.InstanceMedium[i] = inst.Medium
		capture.InstanceMatrix[i] = inst.Matrix
	}

	camInv := camera.Inverse()
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			i := y*w + x
			px := &capture.Pixels[i]

			// world ray through the pixel center; O sits on the near plane
			nx := (2*(float64(x)+0.5) - float64(w)) / float64(w)
			ny := (float64(h) - 2*(float64(y)+0.5)) / float64(h)
			o := volumeUnproject(camInv, Vector{nx, ny, -1})
			p2 := volumeUnproject(camInv, Vector{nx, ny, 1})
			d := p2.Sub(o).Normalize()

			// integration limit from the background depth or the far plane
			c0 := camera.MulPositionW(o)
			c1 := camera.MulPositionW(o.Add(d))
			z0, w0 := c0.Z, c0.W
			z1, w1 := c1.Z-z0, c1.W-w0
			solve := func(q float64) float64 {
				return (z0 - q*w0) / (q*w1 - z1)
			}
			tEnd := solve(1) // far plane
			if !math.IsNaN(depthQ[i]) {
				if tb := solve(depthQ[i]); tb < tEnd {
					tEnd = tb
				}
			}
			if tEnd < 0 {
				tEnd = 0
			}

			// crossings from the real ray and the original faces
			var events []VolumeEvent
			for _, r := range refs[i] {
				inst := &scene.Instances[r.inst]
				mesh := &scene.Meshes[inst.Mesh]
				f := mesh.Faces[r.face]
				world := worlds[r.inst]
				v0 := world[f[0]]
				v1 := world[f[1]]
				v2 := world[f[2]]
				n := v1.Sub(v0).Cross(v2.Sub(v0))
				dn := d.Dot(n)
				if dn == 0 {
					continue
				}
				t := v0.Sub(o).Dot(n) / dn
				p := o.Add(d.MulScalar(t))
				b := Barycentric(v0, v1, v2, p)
				delta := 1
				if dn > 0 {
					delta = -1
				}
				off := offsets[inst.Mesh]
				events = append(events, VolumeEvent{
					T:     t,
					Delta: delta,
					Inst:  r.inst,
					Face:  r.face,
					Vtx:   [3]int{off + f[0], off + f[1], off + f[2]},
					Bary:  Vector{b.X, b.Y, b.Z},
					N:     n,
					Dn:    dn,
				})
			}
			sort.Slice(events, func(a, b int) bool {
				ea, eb := &events[a], &events[b]
				if ea.T != eb.T {
					return ea.T < eb.T
				}
				if ea.Inst != eb.Inst {
					return ea.Inst < eb.Inst
				}
				return ea.Face < eb.Face
			})

			// winding sweep: close an interval only when the active set changes
			wind := make([]int, len(scene.Instances))
			ei := 0
			for ei < len(events) && events[ei].T <= 0 {
				wind[events[ei].Inst] += events[ei].Delta
				ei++
			}
			active := volumeActive(wind)
			var intervals []VolumeInterval
			prev := 0.0
			startEvent := -1
			for ei < len(events) {
				e := events[ei]
				if e.T >= tEnd {
					break
				}
				ei++
				w0 := wind[e.Inst]
				w1 := w0 + e.Delta
				if (w0 == 0) == (w1 == 0) {
					wind[e.Inst] = w1
					continue
				}
				intervals = append(intervals, VolumeInterval{
					T0: prev, T1: e.T, Instances: active,
					StartEvent: startEvent, EndEvent: ei - 1,
				})
				wind[e.Inst] = w1
				active = volumeActive(wind)
				prev = e.T
				startEvent = ei - 1
			}
			intervals = append(intervals, VolumeInterval{
				T0: prev, T1: tEnd, Instances: active,
				StartEvent: startEvent, EndEvent: -1,
			})

			px.O = o
			px.D = d
			px.TEnd = tEnd
			px.Bg = bg[i]
			px.Intervals = intervals
			px.Events = events
		}
	}
	return capture, nil
}
