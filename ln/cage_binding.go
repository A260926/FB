package ln

import (
	"errors"
	"fmt"
	"math"
	"sort"
)

// CageBindingStats reports construction-time details of a CageMesh binding.
type CageBindingStats struct {
	Cells             int // tetrahedral cage cells in the binding
	SourceTriangles   int // triangles of the source mesh
	Slices            int // clipped sub-triangles produced by the binding
	TriangleCellClips int // triangle/cell clip operations executed at construction
}

// Tolerances used by the cage binding. Length-based tolerances are scaled by
// the relevant cell, triangle or edge size, so they work for ordinary
// unit-scale inputs as well as smaller geometry.
const (
	cageVolumeEps   = 1e-12 // relative tetrahedron degeneracy threshold
	cagePlaneSnap   = 1e-11 // fraction of cell scale for plane membership
	cageCoplanarEps = 1e-9  // fraction of cell scale for coplanarity
	cageCoverRel    = 1e-8  // coverage tolerance relative to triangle area
	cageCoverAbs    = 1e-10 // coverage tolerance relative to squared edge length
	cageDropRel     = 1e-14 // piece degeneracy threshold relative to triangle area
	cageEdgeEps     = 1e-9  // fraction of edge length for edge membership
)

// cageCell stores precomputed rest-pose data for one tetrahedral cell.
type cageCell struct {
	indices [4]int
	p0      Vector // rest position of vertex 0
	r0      Vector // rows of the inverse barycentric matrix
	r1      Vector
	r2      Vector
	sign    float64 // orientation sign of the rest determinant
	scale   float64 // max rest edge length
}

// barycentric returns the rest-pose barycentric coordinates of v in the cell.
func (c *cageCell) barycentric(v Vector) [4]float64 {
	d := v.Sub(c.p0)
	l1 := c.r0.Dot(d)
	l2 := c.r1.Dot(d)
	l3 := c.r2.Dot(d)
	return [4]float64{1 - l1 - l2 - l3, l1, l2, l3}
}

// cagePoint is a bound surface point: a cell plus barycentric weights.
type cagePoint struct {
	cell int
	bary [4]float64
}

// cageSlice is one clipped sub-triangle of a source triangle.
type cageSlice struct {
	v [3]cagePoint
}

// cageEdge is the chain of bound points along one original triangle edge.
type cageEdge struct {
	points []cagePoint
}

// cagePiece is the convex intersection of one source triangle with one cell.
type cagePiece struct {
	cell int
	poly []Vector
	area float64
	drop bool
}

func finiteVector(v Vector) bool {
	return !math.IsNaN(v.X) && !math.IsNaN(v.Y) && !math.IsNaN(v.Z) &&
		!math.IsInf(v.X, 0) && !math.IsInf(v.Y, 0) && !math.IsInf(v.Z, 0)
}

// tetDet returns the signed determinant (6x volume) of a tetrahedron.
func tetDet(a, b, c, d Vector) float64 {
	return b.Sub(a).Dot(c.Sub(a).Cross(d.Sub(a)))
}

// tetScale returns the longest edge length of a tetrahedron.
func tetScale(a, b, c, d Vector) float64 {
	s := a.Distance(b)
	s = math.Max(s, a.Distance(c))
	s = math.Max(s, a.Distance(d))
	s = math.Max(s, b.Distance(c))
	s = math.Max(s, b.Distance(d))
	s = math.Max(s, c.Distance(d))
	return s
}

func newCageCell(indices [4]int, points []Vector) (cageCell, error) {
	p0 := points[indices[0]]
	p1 := points[indices[1]]
	p2 := points[indices[2]]
	p3 := points[indices[3]]
	e1 := p1.Sub(p0)
	e2 := p2.Sub(p0)
	e3 := p3.Sub(p0)
	det := e1.Dot(e2.Cross(e3))
	scale := tetScale(p0, p1, p2, p3)
	if math.Abs(det) <= cageVolumeEps*scale*scale*scale {
		return cageCell{}, fmt.Errorf("ln: cage cell %v is degenerate", indices)
	}
	sign := 1.0
	if det < 0 {
		sign = -1
	}
	return cageCell{
		indices: indices,
		p0:      p0,
		r0:      e2.Cross(e3).DivScalar(det),
		r1:      e3.Cross(e1).DivScalar(det),
		r2:      e1.Cross(e2).DivScalar(det),
		sign:    sign,
		scale:   scale,
	}, nil
}

// cageFacePoints returns the rest positions of the face opposite local
// vertex f along with the apex position.
func cageFacePoints(p [4]Vector, f int) (a, b, c, apex Vector) {
	switch f {
	case 0:
		return p[1], p[2], p[3], p[0]
	case 1:
		return p[0], p[3], p[2], p[1]
	case 2:
		return p[0], p[1], p[3], p[2]
	default:
		return p[0], p[2], p[1], p[3]
	}
}

// clipPolygonHalfPlane clips a convex polygon to the inside of the plane
// through pt with outward unit normal n. Distances within snap of the plane
// are treated as exactly on the plane.
func clipPolygonHalfPlane(poly []Vector, n, pt Vector, snap float64) []Vector {
	m := len(poly)
	ds := make([]float64, m)
	for i, v := range poly {
		d := n.Dot(v.Sub(pt))
		if math.Abs(d) <= snap {
			d = 0
		}
		ds[i] = d
	}
	var out []Vector
	for i := 0; i < m; i++ {
		j := (i + 1) % m
		a, b := poly[i], poly[j]
		da, db := ds[i], ds[j]
		ina, inb := da <= 0, db <= 0
		if ina {
			out = append(out, a)
		}
		if ina != inb {
			t := da / (da - db)
			out = append(out, a.Add(b.Sub(a).MulScalar(t)))
		}
	}
	return out
}

// clipTriangleCell intersects a triangle with a cell, returning a convex
// polygon in the triangle's own winding order (possibly empty).
func clipTriangleCell(v [3]Vector, cell cageCell, rest []Vector) []Vector {
	p := [4]Vector{rest[cell.indices[0]], rest[cell.indices[1]],
		rest[cell.indices[2]], rest[cell.indices[3]]}
	poly := []Vector{v[0], v[1], v[2]}
	snap := cagePlaneSnap * cell.scale
	for f := 0; f < 4 && len(poly) > 0; f++ {
		a, b, c, apex := cageFacePoints(p, f)
		n := b.Sub(a).Cross(c.Sub(a))
		if n.Dot(apex.Sub(a)) > 0 {
			n = n.MulScalar(-1)
		}
		n = n.Normalize()
		poly = clipPolygonHalfPlane(poly, n, a, snap)
	}
	return poly
}

// cleanupPolygon removes consecutive duplicate and collinear vertices.
func cleanupPolygon(poly []Vector) []Vector {
	if len(poly) == 0 {
		return nil
	}
	scale := 0.0
	for i := range poly {
		scale = math.Max(scale, poly[i].Distance(poly[(i+1)%len(poly)]))
	}
	if scale == 0 {
		return nil
	}
	dedup := 1e-12 * scale
	out := make([]Vector, 0, len(poly))
	for _, v := range poly {
		if len(out) == 0 || v.Distance(out[len(out)-1]) > dedup {
			out = append(out, v)
		}
	}
	for len(out) > 1 && out[0].Distance(out[len(out)-1]) <= dedup {
		out = out[:len(out)-1]
	}
	col := 1e-12 * scale * scale
	for changed := true; changed && len(out) >= 3; {
		changed = false
		n := len(out)
		for i := 0; i < n; i++ {
			a := out[(i+n-1)%n]
			b := out[i]
			c := out[(i+1)%n]
			if b.Sub(a).Cross(c.Sub(b)).Length() <= col {
				out = append(out[:i], out[i+1:]...)
				changed = true
				break
			}
		}
	}
	return out
}

// triangleNormalArea returns the unit normal and area of a triangle.
func triangleNormalArea(v [3]Vector) (Vector, float64) {
	n := v[1].Sub(v[0]).Cross(v[2].Sub(v[0]))
	l := n.Length()
	if l == 0 {
		return Vector{}, 0
	}
	return n.DivScalar(l), l / 2
}

// polygonArea returns the area of a planar polygon with unit normal n.
func polygonArea(poly []Vector, n Vector) float64 {
	var c Vector
	for i := range poly {
		c = c.Add(poly[i].Cross(poly[(i+1)%len(poly)]))
	}
	return math.Abs(c.Dot(n)) / 2
}

type cageFaceKey [3]int

type cageFaceRef struct {
	cell int
	face int
}

func sortedTriple(a, b, c int) cageFaceKey {
	t := [3]int{a, b, c}
	if t[0] > t[1] {
		t[0], t[1] = t[1], t[0]
	}
	if t[1] > t[2] {
		t[1], t[2] = t[2], t[1]
	}
	if t[0] > t[1] {
		t[0], t[1] = t[1], t[0]
	}
	return t
}

// triangleCoplanarFace reports whether the triangle lies in the plane of the
// cell's local face f.
func triangleCoplanarFace(tv [3]Vector, normalT Vector, cell cageCell, f int, rest []Vector) bool {
	if normalT == (Vector{}) {
		return false
	}
	p := [4]Vector{rest[cell.indices[0]], rest[cell.indices[1]],
		rest[cell.indices[2]], rest[cell.indices[3]]}
	a, b, c, _ := cageFacePoints(p, f)
	n := b.Sub(a).Cross(c.Sub(a)).Normalize()
	if math.Abs(n.Dot(normalT)) < 1-1e-9 {
		return false
	}
	snap := cageCoplanarEps * cell.scale
	for _, v := range tv {
		if math.Abs(n.Dot(v.Sub(a))) > snap {
			return false
		}
	}
	return true
}

type cageEdgeSeg struct {
	tLo, tHi float64
	lo, hi   cagePoint
	cell     int
}

// buildCageEdge chains the per-cell stretches of one original triangle edge
// into a single ordered list of bound points.
func buildCageEdge(a, b Vector, pieces []cagePiece, cells []cageCell) cageEdge {
	dir := b.Sub(a)
	len2 := dir.LengthSquared()
	if len2 == 0 {
		return cageEdge{}
	}
	length := math.Sqrt(len2)
	onEps := cageEdgeEps * length
	tEps := cageEdgeEps
	var segs []cageEdgeSeg
	for _, p := range pieces {
		if p.drop {
			continue
		}
		cc := cells[p.cell]
		n := len(p.poly)
		for i := 0; i < n; i++ {
			q1 := p.poly[i]
			q2 := p.poly[(i+1)%n]
			if q1.SegmentDistance(a, b) > onEps || q2.SegmentDistance(a, b) > onEps {
				continue
			}
			t1 := q1.Sub(a).Dot(dir) / len2
			t2 := q2.Sub(a).Dot(dir) / len2
			if t1 < -tEps || t1 > 1+tEps || t2 < -tEps || t2 > 1+tEps {
				continue
			}
			p1 := cagePoint{p.cell, cc.barycentric(q1)}
			p2 := cagePoint{p.cell, cc.barycentric(q2)}
			if t1 <= t2 {
				segs = append(segs, cageEdgeSeg{t1, t2, p1, p2, p.cell})
			} else {
				segs = append(segs, cageEdgeSeg{t2, t1, p2, p1, p.cell})
			}
		}
	}
	if len(segs) == 0 {
		return cageEdge{}
	}
	sort.Slice(segs, func(i, j int) bool {
		if segs[i].tLo != segs[j].tLo {
			return segs[i].tLo < segs[j].tLo
		}
		if segs[i].cell != segs[j].cell {
			return segs[i].cell < segs[j].cell
		}
		return segs[i].tHi < segs[j].tHi
	})
	// Drop duplicate stretches reported by two cells when the edge runs
	// exactly along their shared face.
	kept := segs[:0]
	for _, s := range segs {
		if len(kept) > 0 {
			last := kept[len(kept)-1]
			if s.tLo >= last.tLo-tEps && s.tHi <= last.tHi+tEps && s.tLo < last.tHi-tEps {
				continue
			}
		}
		kept = append(kept, s)
	}
	edge := cageEdge{points: make([]cagePoint, 0, len(kept)+1)}
	edge.points = append(edge.points, kept[0].lo)
	for _, s := range kept {
		edge.points = append(edge.points, s.hi)
	}
	return edge
}

// NewCageMesh binds a triangle mesh to a tetrahedral control cage. rest holds
// the rest-pose cage vertices and cells the tetrahedral cells indexing into
// rest. The rest cage must be a conforming, non-overlapping, positive-volume
// tetrahedral complex covering the whole mesh; these topological properties
// are guaranteed by the caller. The returned CageMesh keeps its own copy of
// all source data, so later edits to mesh, rest or cells do not affect it.
func NewCageMesh(mesh *Mesh, rest []Vector, cells [][4]int) (*CageMesh, error) {
	if mesh == nil {
		return nil, errors.New("ln: NewCageMesh requires a non-nil mesh")
	}
	for i, t := range mesh.Triangles {
		if t == nil {
			return nil, fmt.Errorf("ln: mesh triangle %d is nil", i)
		}
		if !finiteVector(t.V1) || !finiteVector(t.V2) || !finiteVector(t.V3) {
			return nil, fmt.Errorf("ln: mesh triangle %d has non-finite coordinates", i)
		}
	}
	restCopy := make([]Vector, len(rest))
	for i, p := range rest {
		if !finiteVector(p) {
			return nil, fmt.Errorf("ln: cage rest point %d is not finite", i)
		}
		restCopy[i] = p
	}
	cellsCopy := make([][4]int, len(cells))
	for i, c := range cells {
		for j := 0; j < 4; j++ {
			if c[j] < 0 || c[j] >= len(rest) {
				return nil, fmt.Errorf("ln: cage cell %d index %d out of range", i, c[j])
			}
			for k := j + 1; k < 4; k++ {
				if c[j] == c[k] {
					return nil, fmt.Errorf("ln: cage cell %d repeats index %d", i, c[j])
				}
			}
		}
		cellsCopy[i] = c
	}
	cellData := make([]cageCell, len(cellsCopy))
	for i, c := range cellsCopy {
		cc, err := newCageCell(c, restCopy)
		if err != nil {
			return nil, err
		}
		cellData[i] = cc
	}
	// Map each cell face to the cells sharing it; faces with two cells are
	// interior and need duplicate-piece handling.
	faces := make(map[cageFaceKey][]cageFaceRef)
	for ci, c := range cellsCopy {
		for f := 0; f < 4; f++ {
			var tri [3]int
			n := 0
			for j := 0; j < 4; j++ {
				if j != f {
					tri[n] = c[j]
					n++
				}
			}
			key := sortedTriple(tri[0], tri[1], tri[2])
			faces[key] = append(faces[key], cageFaceRef{ci, f})
		}
	}
	neighbor := make(map[[2]int]int)
	for _, refs := range faces {
		if len(refs) != 2 {
			continue
		}
		neighbor[[2]int{refs[0].cell, refs[0].face}] = refs[1].cell
		neighbor[[2]int{refs[1].cell, refs[1].face}] = refs[0].cell
	}
	m := &CageMesh{
		rest:   restCopy,
		cells:  cellsCopy,
		ccells: cellData,
		cur:    make([]Vector, len(restCopy)),
		gen:    1,
	}
	copy(m.cur, restCopy)
	m.stats.Cells = len(cellsCopy)
	m.stats.SourceTriangles = len(mesh.Triangles)
	for ti, t := range mesh.Triangles {
		tv := [3]Vector{t.V1, t.V2, t.V3}
		normalT, areaT := triangleNormalArea(tv)
		edgeLen := math.Max(tv[0].Distance(tv[1]),
			math.Max(tv[1].Distance(tv[2]), tv[2].Distance(tv[0])))
		var pieces []cagePiece
		for ci := range cellData {
			m.stats.TriangleCellClips++
			poly := cleanupPolygon(clipTriangleCell(tv, cellData[ci], restCopy))
			if len(poly) < 3 {
				continue
			}
			a := polygonArea(poly, normalT)
			if a <= cageDropRel*areaT {
				continue
			}
			pieces = append(pieces, cagePiece{cell: ci, poly: poly, area: a})
		}
		// A triangle lying exactly in a shared cell face is produced by both
		// adjacent cells; keep only the copy from the lower-indexed cell so
		// no duplicate area is generated.
		hasPiece := make(map[int]bool)
		for _, p := range pieces {
			hasPiece[p.cell] = true
		}
		for i := range pieces {
			ci := pieces[i].cell
			for f := 0; f < 4; f++ {
				nb, ok := neighbor[[2]int{ci, f}]
				if !ok || nb >= ci || !hasPiece[nb] {
					continue
				}
				if triangleCoplanarFace(tv, normalT, cellData[ci], f, restCopy) {
					pieces[i].drop = true
					break
				}
			}
		}
		sum := 0.0
		for _, p := range pieces {
			if !p.drop {
				sum += p.area
			}
		}
		tol := cageCoverRel*areaT + cageCoverAbs*edgeLen*edgeLen
		if math.Abs(sum-areaT) > tol {
			return nil, fmt.Errorf("ln: mesh triangle %d is not fully covered by the cage", ti)
		}
		// Fan-triangulate each piece, preserving the source winding, and bind
		// the slice vertices to the owning cell.
		for _, p := range pieces {
			if p.drop {
				continue
			}
			cc := cellData[p.cell]
			n := len(p.poly)
			for i := 1; i+1 < n; i++ {
				tri := [3]Vector{p.poly[0], p.poly[i], p.poly[i+1]}
				if _, a := triangleNormalArea(tri); a <= cageDropRel*p.area {
					continue
				}
				var s cageSlice
				for k := 0; k < 3; k++ {
					s.v[k] = cagePoint{p.cell, cc.barycentric(tri[k])}
				}
				m.slices = append(m.slices, s)
			}
		}
		// Chain the original edges; cell boundary crossings become bend
		// points while slice diagonals and cage cut lines are not recorded.
		for e := 0; e < 3; e++ {
			m.edges = append(m.edges, buildCageEdge(tv[e], tv[(e+1)%3], pieces, cellData))
		}
	}
	m.stats.Slices = len(m.slices)
	m.tris = make([]*Triangle, len(m.slices))
	for i := range m.tris {
		m.tris[i] = &Triangle{}
	}
	m.applyPose()
	return m, nil
}
