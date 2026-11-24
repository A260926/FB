package fauxgl

import (
	"fmt"
	"math"
	"sort"
)

// VolumeMedia describes one participating medium: per-channel RGB
// extinction sigma (non-negative, per world unit) and per-channel
// emission j (per world unit).
type VolumeMedia struct {
	Sigma Color
	J     Color
}

// VolumeInstance places a closed, oriented triangle mesh in the world.
// Matrix is a fixed, invertible affine (world) transform. Media is the
// medium index active inside the instance's winding-number volume.
type VolumeInstance struct {
	Mesh   *Mesh
	Matrix Matrix
	Media  int
}

// VolumeScene bundles the inputs of a volumetric capture.
type VolumeScene struct {
	Meshes     []*Mesh
	Instances  []VolumeInstance
	Media      []VolumeMedia
	ViewMatrix Matrix // world-to-clip, perspective or orthographic
	Width      int
	Height     int
}

// volumeHit is one double-sided rasterization event on a pixel-center ray.
type volumeHit struct {
	instance   int
	t          float64 // world-space ray parameter (distance units)
	winding    int     // +1 for an entering crossing, -1 for leaving
	face       int     // face index within the instance mesh
	b0, b1, b2 float64 // ray/triangle intersection barycentric weights
}

// VolumeEvent is a boundary of an active medium interval along a ray.
type VolumeEvent struct {
	T        float64 // world distance from the ray origin
	Winding  int     // signed winding-number change at this boundary
	Instance int
	Media    int
	Face     int
	B0       float64
	B1       float64
	B2       float64
}

// VolumePixel holds the captured ray events and snapshot data for one pixel.
type VolumePixel struct {
	Events    []VolumeEvent // ordered by strictly increasing world distance, t >= 0
	Near      float64       // world distance of the near clipping plane (0)
	Far       float64       // world distance of the far clipping plane
	Backdrop  Color         // fixed opaque backdrop color (linear RGB)
	BackDepth float64       // backdrop world distance, or +inf
	HasBack   bool          // whether a finite opaque backdrop was hit
	// opening records each instance's winding contributions at the near
	// plane, established by crossings with t < 0, so an interval cut by
	// the near plane is retained rather than treated as no crossing.
	opening [][2]int
}

// VolumeRecord is an owning, self-contained capture of a whole frame.
// It holds private copies of all geometry and snapshot data, so mutating
// the source meshes, scene or context after capture cannot affect the
// record or any adjoint run.
type VolumeRecord struct {
	Width, Height int
	Pixels        []VolumePixel
	Media         []VolumeMedia

	// per-mesh shared-position tables: ids are shared across co-located
	// source vertices, and meshFaces[mi][fi] gives the three ids of a face.
	meshVerts [][]Vector
	meshFaces [][][3]int
	// per-instance data; instWorldVerts ids match the instance's mesh
	instMesh       []int
	instMedia      []int
	instMatrix     []Matrix
	instWorldVerts [][]Vector
	// rays holds the private world-space ray of every pixel center.
	rays []rayGeom
}

const volEventEps = 1e-10

// volSidePlanes are the four side frustum planes. Near and far are
// deliberately excluded: a face wholly outside the near/far range still
// establishes a winding crossing on the ray, which is needed to retain
// intervals cut by those planes. The input contract guarantees
// clip.w > 0, so side-only clipping is well defined.
var volSidePlanes = []clipPlane{
	{VectorW{1, 0, 0, 1}, VectorW{-1, 0, 0, 1}},
	{VectorW{-1, 0, 0, 1}, VectorW{1, 0, 0, 1}},
	{VectorW{0, 1, 0, 1}, VectorW{0, -1, 0, 1}},
	{VectorW{0, -1, 0, 1}, VectorW{0, 1, 0, 1}},
}

func isFiniteVector(v Vector) bool {
	return !v.IsDegenerate()
}

func isFiniteMatrix(m Matrix) bool {
	v := []float64{
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

// Validate checks indices, finiteness, matrix invertibility and the
// non-negativity of the medium coefficients. Closed, oriented topology
// is the responsibility of the caller.
func (s *VolumeScene) Validate() error {
	if s.Width <= 0 || s.Height <= 0 {
		return fmt.Errorf("fauxgl: invalid volume frame size %dx%d", s.Width, s.Height)
	}
	if len(s.Meshes) == 0 || len(s.Instances) == 0 {
		return fmt.Errorf("fauxgl: volume scene needs at least one mesh and instance")
	}
	if len(s.Media) == 0 {
		return fmt.Errorf("fauxgl: volume scene needs at least one medium")
	}
	if !isFiniteMatrix(s.ViewMatrix) {
		return fmt.Errorf("fauxgl: view matrix is not finite")
	}
	if s.ViewMatrix.Determinant() == 0 {
		return fmt.Errorf("fauxgl: view matrix is singular")
	}
	for mi, m := range s.Media {
		if !isFiniteVector(Vector{m.Sigma.R, m.Sigma.G, m.Sigma.B}) ||
			!isFiniteVector(Vector{m.J.R, m.J.G, m.J.B}) {
			return fmt.Errorf("fauxgl: medium %d has non-finite coefficients", mi)
		}
		if m.Sigma.R < 0 || m.Sigma.G < 0 || m.Sigma.B < 0 ||
			m.J.R < 0 || m.J.G < 0 || m.J.B < 0 {
			return fmt.Errorf("fauxgl: medium %d has negative coefficients", mi)
		}
	}
	meshIndexOf := make(map[*Mesh]int, len(s.Meshes))
	for mi, m := range s.Meshes {
		meshIndexOf[m] = mi
	}
	for ii, in := range s.Instances {
		if in.Mesh == nil {
			return fmt.Errorf("fauxgl: instance %d has no mesh", ii)
		}
		if _, ok := meshIndexOf[in.Mesh]; !ok {
			return fmt.Errorf("fauxgl: instance %d mesh is not part of the scene", ii)
		}
		if in.Media < 0 || in.Media >= len(s.Media) {
			return fmt.Errorf("fauxgl: instance %d references bad medium index %d", ii, in.Media)
		}
		if !isFiniteMatrix(in.Matrix) {
			return fmt.Errorf("fauxgl: instance %d matrix is not finite", ii)
		}
		if in.Matrix.Determinant() == 0 {
			return fmt.Errorf("fauxgl: instance %d matrix is singular", ii)
		}
		for fi, t := range in.Mesh.Triangles {
			if t == nil {
				return fmt.Errorf("fauxgl: instance %d face %d is nil", ii, fi)
			}
			for k, p := range []Vector{t.V1.Position, t.V2.Position, t.V3.Position} {
				if !isFiniteVector(p) {
					return fmt.Errorf("fauxgl: instance %d face %d vertex %d is not finite", ii, fi, k)
				}
			}
		}
	}
	return nil
}

// rayGeom describes one pixel-center ray in world space.
type rayGeom struct {
	origin Vector
	dir    Vector // unit length
	far    float64
}

// buildRays derives world-space rays from the inverse world-to-clip
// matrix. The near-plane point is the ray origin (distance 0) and the
// far-plane point fixes the unit direction and the far distance.
func buildRays(s *VolumeScene) []rayGeom {
	inv := s.ViewMatrix.Inverse()
	rays := make([]rayGeom, s.Width*s.Height)
	for y := 0; y < s.Height; y++ {
		for x := 0; x < s.Width; x++ {
			ndcX := (float64(x)+0.5)/float64(s.Width)*2 - 1
			ndcY := 1 - (float64(y)+0.5)/float64(s.Height)*2
			nw := inv.MulPositionW(Vector{ndcX, ndcY, -1})
			fw := inv.MulPositionW(Vector{ndcX, ndcY, 1})
			near := nw.DivScalar(nw.W).Vector()
			far := fw.DivScalar(fw.W).Vector()
			delta := far.Sub(near)
			length := delta.Length()
			rays[y*s.Width+x] = rayGeom{
				origin: near,
				dir:    delta.DivScalar(length),
				far:    length,
			}
		}
	}
	return rays
}

// unprojectDepth converts a window-z value in [0,1] to a world distance
// along the ray. The depth point is unprojected exactly rather than
// interpolating window-z differences, which are not world lengths.
func unprojectDepth(inv Matrix, r rayGeom, ndcX, ndcY, d float64) float64 {
	w := inv.MulPositionW(Vector{ndcX, ndcY, 2*d - 1})
	p := w.DivScalar(w.W).Vector()
	return p.Sub(r.origin).Dot(r.dir)
}

type volRasterTriangle struct {
	s0, s1, s2 Vector  // screen positions (y down)
	p0, p1, p2 Vector  // world positions (interpolated for clip pieces)
	c0, c1, c2 VectorW // clip-space positions
	face       int
}

func (s *VolumeScene) screenPosition(c VectorW) Vector {
	ndc := c.DivScalar(c.W).Vector()
	return Vector{
		(ndc.X + 1) * 0.5 * float64(s.Width),
		(1 - ndc.Y) * 0.5 * float64(s.Height),
		0,
	}
}

// clipVolumeTriangle clips against the four side frustum planes,
// reusing the shared Sutherland-Hodgman clipping code. Near/far are not
// applied here so faces cut by them keep producing ray events; the
// segment sweep instead treats near/far by world distance. Every
// emitted piece keeps the original face identity and exactly
// interpolated world positions.
func (s *VolumeScene) clipVolumeTriangle(in volRasterTriangle) []volRasterTriangle {
	out := sutherlandHodgman([]VectorW{in.c0, in.c1, in.c2}, volSidePlanes)
	if len(out) < 3 {
		return nil
	}
	n0 := in.c0.DivScalar(in.c0.W).Vector()
	n1 := in.c1.DivScalar(in.c1.W).Vector()
	n2 := in.c2.DivScalar(in.c2.W).Vector()
	result := make([]volRasterTriangle, 0, len(out)-2)
	for i := 1; i+1 < len(out); i++ {
		b0 := Barycentric(n0, n1, n2, out[0].DivScalar(out[0].W).Vector())
		b1 := Barycentric(n0, n1, n2, out[i].DivScalar(out[i].W).Vector())
		b2 := Barycentric(n0, n1, n2, out[i+1].DivScalar(out[i+1].W).Vector())
		world := func(b VectorW) (Vector, VectorW) {
			w := in.p0.MulScalar(b.X).Add(in.p1.MulScalar(b.Y)).Add(in.p2.MulScalar(b.Z))
			c := in.c0.MulScalar(b.X).Add(in.c1.MulScalar(b.Y)).Add(in.c2.MulScalar(b.Z))
			return w, c
		}
		w0, cw0 := world(b0)
		w1, cw1 := world(b1)
		w2, cw2 := world(b2)
		t := volRasterTriangle{
			p0: w0, p1: w1, p2: w2,
			c0: cw0, c1: cw1, c2: cw2,
			face: in.face,
		}
		t.s0 = s.screenPosition(cw0)
		t.s1 = s.screenPosition(cw1)
		t.s2 = s.screenPosition(cw2)
		result = append(result, t)
	}
	return result
}

// rasterVolumeTriangle walks every covered pixel center using the same
// edge-function coverage rule as the production rasterizer.
func (s *VolumeScene) rasterVolumeTriangle(t volRasterTriangle, emit func(pixel int)) {
	min := t.s0.Min(t.s1.Min(t.s2)).Floor()
	max := t.s0.Max(t.s1.Max(t.s2)).Ceil()
	x0, x1 := int(min.X), int(max.X)
	y0, y1 := int(min.Y), int(max.Y)
	area := edge(t.s0, t.s1, t.s2)
	if area == 0 {
		return
	}
	ra := 1 / area
	p := Vector{float64(x0) + 0.5, float64(y0) + 0.5, 0}
	w00 := edge(t.s1, t.s2, p)
	w01 := edge(t.s2, t.s0, p)
	w02 := edge(t.s0, t.s1, p)
	a01 := t.s1.Y - t.s0.Y
	b01 := t.s0.X - t.s1.X
	a12 := t.s2.Y - t.s1.Y
	b12 := t.s1.X - t.s2.X
	a20 := t.s0.Y - t.s2.Y
	b20 := t.s2.X - t.s0.X
	for y := y0; y <= y1; y++ {
		w0, w1, w2 := w00, w01, w02
		for x := x0; x <= x1; x++ {
			if w0*ra >= 0 && w1*ra >= 0 && w2*ra >= 0 {
				if x >= 0 && x < s.Width && y >= 0 && y < s.Height {
					emit(y*s.Width + x)
				}
			}
			w0 += a12
			w1 += a20
			w2 += a01
		}
		w00 += b12
		w01 += b20
		w02 += b01
	}
}

// faceWinding gives the signed crossing of a forward ray and the face.
// Positive NDC area is an entering crossing; a mirrored instance
// (negative affine determinant) flips it so its winding-number interior
// keeps the correct orientation.
func faceWinding(c0, c1, c2 VectorW, det float64) int {
	n0 := c0.DivScalar(c0.W).Vector()
	n1 := c1.DivScalar(c1.W).Vector()
	n2 := c2.DivScalar(c2.W).Vector()
	a := (n1.X-n0.X)*(n2.Y-n0.Y) - (n2.X-n0.X)*(n1.Y-n0.Y)
	winding := 1
	if a < 0 {
		winding = -1
	}
	if det < 0 {
		winding = -winding
	}
	return winding
}

type rayHit struct {
	t          float64
	b0, b1, b2 float64
}

// rayTriangleIntersect solves origin + t*dir = b0*p0 + b1*p1 + b2*p2
// with a unit direction, so t is a true world distance. Coverage was
// already established by rasterization, so a zero denominator only
// occurs in degenerate roundoff cases and is reported as a miss.
func rayTriangleIntersect(origin, dir, p0, p1, p2 Vector) (rayHit, bool) {
	e1 := p1.Sub(p0)
	e2 := p2.Sub(p0)
	p := dir.Cross(e2)
	det := e1.Dot(p)
	if det == 0 {
		return rayHit{}, false
	}
	inv := 1 / det
	tv := origin.Sub(p0)
	b1 := tv.Dot(p) * inv
	q := tv.Cross(e1)
	b2 := dir.Dot(q) * inv
	b0 := 1 - b1 - b2
	t := e2.Dot(q) * inv
	if math.IsNaN(t) || math.IsInf(t, 0) {
		return rayHit{}, false
	}
	return rayHit{t: t, b0: b0, b1: b1, b2: b2}, true
}

// CaptureVolume rasterizes every instance double-sided, producing per
// pixel ray events from real triangle crossings rather than per-pixel
// mesh scans. The context's current opaque color and depth buffers are
// taken as a fixed backdrop snapshot; the context itself is never
// modified.
func CaptureVolume(dc *Context, s *VolumeScene) (*VolumeRecord, error) {
	if dc == nil {
		return nil, fmt.Errorf("fauxgl: nil context")
	}
	if dc.Width != s.Width || dc.Height != s.Height {
		return nil, fmt.Errorf("fauxgl: context/frame size mismatch")
	}
	if err := s.Validate(); err != nil {
		return nil, err
	}

	rays := buildRays(s)
	hits := make([][]volumeHit, s.Width*s.Height)

	meshIndexOf := make(map[*Mesh]int, len(s.Meshes))
	for i, m := range s.Meshes {
		meshIndexOf[m] = i
	}

	for ii, in := range s.Instances {
		mvp := s.ViewMatrix.Mul(in.Matrix)
		det := in.Matrix.Determinant()
		for fi, tri := range in.Mesh.Triangles {
			p0 := in.Matrix.MulPosition(tri.V1.Position)
			p1 := in.Matrix.MulPosition(tri.V2.Position)
			p2 := in.Matrix.MulPosition(tri.V3.Position)
			c0 := mvp.MulPositionW(tri.V1.Position)
			c1 := mvp.MulPositionW(tri.V2.Position)
			c2 := mvp.MulPositionW(tri.V3.Position)
			if c0.W <= 0 || c1.W <= 0 || c2.W <= 0 {
				continue // input contract guarantees clip.w > 0; guard anyway
			}
			root := volRasterTriangle{face: fi, p0: p0, p1: p1, p2: p2, c0: c0, c1: c1, c2: c2}
			root.s0 = s.screenPosition(c0)
			root.s1 = s.screenPosition(c1)
			root.s2 = s.screenPosition(c2)
			winding := faceWinding(c0, c1, c2, det)
			for _, ct := range s.clipVolumeTriangle(root) {
				face, w := fi, winding
				s.rasterVolumeTriangle(ct, func(pixel int) {
					r := rays[pixel]
					hit, ok := rayTriangleIntersect(r.origin, r.dir, p0, p1, p2)
					if !ok {
						return
					}
					hits[pixel] = append(hits[pixel], volumeHit{
						instance: ii,
						t:        hit.t,
						winding:  w,
						face:     face,
						b0:       hit.b0,
						b1:       hit.b1,
						b2:       hit.b2,
					})
				})
			}
		}
	}

	record := &VolumeRecord{
		Width:          s.Width,
		Height:         s.Height,
		Pixels:         make([]VolumePixel, s.Width*s.Height),
		Media:          append([]VolumeMedia(nil), s.Media...),
		meshVerts:      make([][]Vector, len(s.Meshes)),
		meshFaces:      make([][][3]int, len(s.Meshes)),
		instMesh:       make([]int, len(s.Instances)),
		instMedia:      make([]int, len(s.Instances)),
		instMatrix:     make([]Matrix, len(s.Instances)),
		instWorldVerts: make([][]Vector, len(s.Instances)),
		rays:           rays,
	}

	// shared-position tables per mesh: co-located source vertices share
	// an id, and every face records its three ids.
	for mi, m := range s.Meshes {
		posIndex := make(map[Vector]int)
		verts := make([]Vector, 0, len(m.Triangles)*2)
		faces := make([][3]int, len(m.Triangles))
		for fi, tri := range m.Triangles {
			for k, p := range []Vector{tri.V1.Position, tri.V2.Position, tri.V3.Position} {
				j, ok := posIndex[p]
				if !ok {
					j = len(verts)
					verts = append(verts, p)
					posIndex[p] = j
				}
				faces[fi][k] = j
			}
		}
		record.meshVerts[mi] = verts
		record.meshFaces[mi] = faces
	}
	// private world-space vertex copies per instance; ids match the mesh
	for ii, in := range s.Instances {
		mi := meshIndexOf[in.Mesh]
		record.instMesh[ii] = mi
		record.instMedia[ii] = in.Media
		record.instMatrix[ii] = in.Matrix
		world := make([]Vector, len(record.meshVerts[mi]))
		for vi, p := range record.meshVerts[mi] {
			world[vi] = in.Matrix.MulPosition(p)
		}
		record.instWorldVerts[ii] = world
	}

	inv := s.ViewMatrix.Inverse()
	for pixel := range record.Pixels {
		r := rays[pixel]
		hs := hits[pixel]
		sort.SliceStable(hs, func(i, j int) bool { return hs[i].t < hs[j].t })
		// Shared edges and clipping fans contribute once: group
		// same-instance crossings at coincident distances. Same-winding
		// duplicates collapse to one crossing; opposite-winding
		// tangencies cancel. Coincident crossings of different instances
		// stay separate because overlapping volumes add coefficients.
		events := make([]VolumeEvent, 0, len(hs))
		var opening [][2]int // instance, winding sum at the near plane
		for i := 0; i < len(hs); {
			j := i + 1
			sum := hs[i].winding
			for j < len(hs) && hs[j].instance == hs[i].instance &&
				math.Abs(hs[j].t-hs[i].t) <= volEventEps*(1+math.Abs(hs[i].t)) {
				sum += hs[j].winding
				j++
			}
			if sum != 0 {
				h := hs[i]
				w := 1
				if sum < 0 {
					w = -1
				}
				if h.t < 0 {
					// crossing behind the near plane sets the opening state
					opening = append(opening, [2]int{h.instance, w})
				} else {
					events = append(events, VolumeEvent{
						T: h.t, Winding: w, Instance: h.instance,
						Media: record.instMedia[h.instance], Face: h.face,
						B0: h.b0, B1: h.b1, B2: h.b2,
					})
				}
			}
			i = j
		}
		px := VolumePixel{Events: events, Near: 0, Far: r.far, opening: opening}

		// fixed backdrop snapshot from the context buffers
		x, y := pixel%s.Width, pixel/s.Width
		off := dc.ColorBuffer.PixOffset(x, y)
		a := dc.ColorBuffer.Pix[off+3]
		if a != 255 {
			return nil, fmt.Errorf("fauxgl: backdrop alpha must be 255, got %d at pixel %d", a, pixel)
		}
		px.Backdrop = Color{
			R: float64(dc.ColorBuffer.Pix[off+0]) / 255,
			G: float64(dc.ColorBuffer.Pix[off+1]) / 255,
			B: float64(dc.ColorBuffer.Pix[off+2]) / 255,
			A: 1,
		}
		d := dc.DepthBuffer[pixel]
		switch {
		case d == math.MaxFloat64:
			px.BackDepth = math.Inf(1)
			px.HasBack = false
		case math.IsNaN(d) || math.IsInf(d, 0) || d < 0 || d > 1:
			return nil, fmt.Errorf("fauxgl: backdrop depth out of range at pixel %d: %g", pixel, d)
		default:
			ndcX := (float64(x)+0.5)/float64(s.Width)*2 - 1
			ndcY := 1 - (float64(y)+0.5)/float64(s.Height)*2
			px.BackDepth = unprojectDepth(inv, r, ndcX, ndcY, d)
			px.HasBack = true
		}
		record.Pixels[pixel] = px
	}
	return record, nil
}
