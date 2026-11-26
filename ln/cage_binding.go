package ln

import (
	"fmt"
	"math"
	"sort"
)

// CageBindingStats describes work performed while building a cage binding.
// It is a snapshot: posing a CageMesh never changes it.
type CageBindingStats struct {
	// TriangleCellClips is the number of original-triangle/cell clipping
	// operations performed while the binding was constructed. Pose never
	// increases it.
	TriangleCellClips int
}

// cageVertex is a point expressed in the barycentric coordinates of one of the
// resting tetrahedra. The coordinates are relative to cells[cell] in order.
type cageVertex struct {
	cell int
	w    [4]float64
}

type cageFacet struct {
	v [3]cageVertex
}

type cagePathVertex struct {
	cell int
	w    [4]float64
}

type cagePath struct {
	pts []cagePathVertex
}

type cageSegment struct {
	t0, t1 float64
	cell   int
}

type cagePlane struct {
	n Vector
	d float64
}

type cageBuilder struct {
	rest   []Vector
	cells  [][4]int
	planes [][4]cagePlane
	scale  float64
	eps    float64

	stats  CageBindingStats
	facets []cageFacet
	edges  []cagePath // exactly three per source triangle
}

func finiteVector(v Vector) bool {
	return !math.IsNaN(v.X) && !math.IsNaN(v.Y) && !math.IsNaN(v.Z) &&
		!math.IsInf(v.X, 0) && !math.IsInf(v.Y, 0) && !math.IsInf(v.Z, 0)
}

func cageVolume6(points []Vector, c [4]int) float64 {
	p0 := points[c[0]]
	e1 := points[c[1]].Sub(p0)
	e2 := points[c[2]].Sub(p0)
	e3 := points[c[3]].Sub(p0)
	return e1.Dot(e2.Cross(e3))
}

func cageCellPlanes(points []Vector, c [4]int) [4]cagePlane {
	var planes [4]cagePlane
	for i := 0; i < 4; i++ {
		var face [3]Vector
		j := 0
		for k := 0; k < 4; k++ {
			if k != i {
				face[j] = points[c[k]]
				j++
			}
		}
		a, b, d := face[0], face[1], face[2]
		n := b.Sub(a).Cross(d.Sub(a))
		if n.Dot(points[c[i]].Sub(a)) < 0 {
			n = n.MulScalar(-1)
		}
		length := n.Length()
		if length > 0 {
			n = n.DivScalar(length)
		}
		planes[i] = cagePlane{n: n, d: n.Dot(a)}
	}
	return planes
}

// cageBarycentric returns the barycentric coordinates of x with respect to the
// tetrahedron points[c]. det is the signed 6-volume of the tetrahedron.
func cageBarycentric(points []Vector, c [4]int, x Vector) ([4]float64, float64) {
	p0 := points[c[0]]
	e1 := points[c[1]].Sub(p0)
	e2 := points[c[2]].Sub(p0)
	e3 := points[c[3]].Sub(p0)
	det := e1.Dot(e2.Cross(e3))
	if math.Abs(det) <= 1e-300 {
		return [4]float64{}, det
	}
	d := x.Sub(p0)
	u := d.Dot(e2.Cross(e3)) / det
	v := e1.Dot(d.Cross(e3)) / det
	w := e1.Dot(e2.Cross(d)) / det
	return [4]float64{1 - u - v - w, u, v, w}, det
}

func cageMinWeight(w [4]float64) float64 {
	m := w[0]
	if w[1] < m {
		m = w[1]
	}
	if w[2] < m {
		m = w[2]
	}
	if w[3] < m {
		m = w[3]
	}
	return m
}

// ownerCell picks the cell whose interior contains x most convincingly. For a
// point on a shared face this deliberately falls back to the lowest cell index
// so that coplanar pieces produced by several cells are emitted exactly once.
func (b *cageBuilder) ownerCell(x Vector) int {
	const tie = 1e-9
	best := -1
	bestMin := math.Inf(-1)
	for ci := range b.cells {
		w, det := cageBarycentric(b.rest, b.cells[ci], x)
		if math.Abs(det) <= 1e-300 {
			continue
		}
		m := cageMinWeight(w)
		if best < 0 || m > bestMin+tie {
			best = ci
			bestMin = m
		}
	}
	return best
}

// cellOwns reports whether a polygon clipped to cell ci is the authoritative
// copy for its centroid. A polygon strictly inside ci is always authoritative;
// only boundary polygons need the (more expensive) tie-break against all other
// cells.
func (b *cageBuilder) cellOwns(ci int, centroid Vector) bool {
	w, det := cageBarycentric(b.rest, b.cells[ci], centroid)
	if math.Abs(det) > 1e-300 && cageMinWeight(w) > 1e-9 {
		return true
	}
	return b.ownerCell(centroid) == ci
}

func cageClipPolygon(poly []Vector, pl cagePlane, eps float64) []Vector {
	if len(poly) == 0 {
		return nil
	}
	out := make([]Vector, 0, len(poly)+1)
	prev := poly[len(poly)-1]
	dprev := pl.n.Dot(prev) - pl.d
	prevIn := dprev >= -eps
	for _, cur := range poly {
		dcur := pl.n.Dot(cur) - pl.d
		curIn := dcur >= -eps
		if curIn != prevIn {
			t := dprev / (dprev - dcur)
			out = append(out, prev.Add(cur.Sub(prev).MulScalar(t)))
		}
		if curIn {
			out = append(out, cur)
		}
		prev, dprev, prevIn = cur, dcur, curIn
	}
	return out
}

func cageCleanPolygon(poly []Vector, tol float64) []Vector {
	out := make([]Vector, 0, len(poly))
	for _, p := range poly {
		if len(out) == 0 || p.Distance(out[len(out)-1]) > tol {
			out = append(out, p)
		}
	}
	for len(out) > 1 && out[0].Distance(out[len(out)-1]) <= tol {
		out = out[:len(out)-1]
	}
	return out
}

func cageDegenerateTriangle(a, b, c Vector) bool {
	ab := b.Sub(a)
	ac := c.Sub(a)
	bc := c.Sub(b)
	max2 := ab.LengthSquared()
	if ac.LengthSquared() > max2 {
		max2 = ac.LengthSquared()
	}
	if bc.LengthSquared() > max2 {
		max2 = bc.LengthSquared()
	}
	if max2 == 0 {
		return true
	}
	return ab.Cross(ac).Length() <= 1e-12*max2
}

func cageEdgeParam(p, a, b Vector, tol, paramTol float64) (float64, bool) {
	v := b.Sub(a)
	l2 := v.LengthSquared()
	if l2 == 0 {
		return 0, p.Distance(a) <= tol
	}
	t := p.Sub(a).Dot(v) / l2
	if t < -paramTol || t > 1+paramTol {
		return 0, false
	}
	q := a.Add(v.MulScalar(t))
	if p.Distance(q) > tol {
		return 0, false
	}
	return t, true
}

func cageClamp01(t float64) float64 {
	if t < 0 {
		return 0
	}
	if t > 1 {
		return 1
	}
	return t
}

// NewCageMesh binds mesh to the resting tetrahedral complex described by rest
// and cells. All source data is copied; the caller may modify mesh, rest and
// cells afterwards without affecting the binding.
func NewCageMesh(mesh *Mesh, rest []Vector, cells [][4]int) (*CageMesh, error) {
	if mesh == nil {
		return nil, fmt.Errorf("ln: NewCageMesh: nil mesh")
	}
	if len(rest) == 0 {
		return nil, fmt.Errorf("ln: NewCageMesh: empty rest cage")
	}
	if len(cells) == 0 {
		return nil, fmt.Errorf("ln: NewCageMesh: empty cell list")
	}

	restCopy := make([]Vector, len(rest))
	copy(restCopy, rest)
	for i, p := range restCopy {
		if !finiteVector(p) {
			return nil, fmt.Errorf("ln: NewCageMesh: rest point %d is not finite", i)
		}
	}

	src := make([][3]Vector, 0, len(mesh.Triangles))
	for i, t := range mesh.Triangles {
		if t == nil {
			return nil, fmt.Errorf("ln: NewCageMesh: triangle %d is nil", i)
		}
		if !finiteVector(t.V1) || !finiteVector(t.V2) || !finiteVector(t.V3) {
			return nil, fmt.Errorf("ln: NewCageMesh: triangle %d has non-finite coordinates", i)
		}
		src = append(src, [3]Vector{t.V1, t.V2, t.V3})
	}

	cellsCopy := make([][4]int, len(cells))
	copy(cellsCopy, cells)
	for ci, c := range cellsCopy {
		var seen [4]int
		for i := 0; i < 4; i++ {
			idx := c[i]
			if idx < 0 || idx >= len(restCopy) {
				return nil, fmt.Errorf("ln: NewCageMesh: cell %d index %d out of range", ci, idx)
			}
			for j := 0; j < i; j++ {
				if seen[j] == idx {
					return nil, fmt.Errorf("ln: NewCageMesh: cell %d has duplicate index %d", ci, idx)
				}
			}
			seen[i] = idx
		}
		// Normalize the winding so every stored cell has positive signed
		// volume. The barycentric coordinates are adjusted accordingly.
		vol := cageVolume6(restCopy, c)
		var maxEdge float64
		for i := 0; i < 4; i++ {
			for j := i + 1; j < 4; j++ {
				d := restCopy[c[i]].Distance(restCopy[c[j]])
				if d > maxEdge {
					maxEdge = d
				}
			}
		}
		if maxEdge == 0 {
			return nil, fmt.Errorf("ln: NewCageMesh: cell %d is degenerate", ci)
		}
		if math.Abs(vol) <= 1e-12*maxEdge*maxEdge*maxEdge {
			return nil, fmt.Errorf("ln: NewCageMesh: cell %d is degenerate", ci)
		}
		if vol < 0 {
			cellsCopy[ci][2], cellsCopy[ci][3] = cellsCopy[ci][3], cellsCopy[ci][2]
		}
	}

	// A global length scale used for tolerances. It is deliberately based on
	// the source geometry, not on a fixed step.
	scale := 1.0
	var boxMin, boxMax Vector
	haveBox := false
	consider := func(p Vector) {
		if !haveBox {
			boxMin, boxMax = p, p
			haveBox = true
			return
		}
		boxMin = boxMin.Min(p)
		boxMax = boxMax.Max(p)
	}
	for _, p := range restCopy {
		consider(p)
	}
	for _, t := range src {
		consider(t[0])
		consider(t[1])
		consider(t[2])
	}
	if haveBox {
		if d := boxMax.Sub(boxMin).Length(); d > scale {
			scale = d
		}
	}

	b := &cageBuilder{
		rest:   restCopy,
		cells:  cellsCopy,
		planes: make([][4]cagePlane, len(cellsCopy)),
		scale:  scale,
		eps:    1e-12 * scale,
	}
	for ci, c := range cellsCopy {
		b.planes[ci] = cageCellPlanes(restCopy, c)
	}

	for i, tri := range src {
		if err := b.addTriangle(tri); err != nil {
			return nil, fmt.Errorf("ln: NewCageMesh: triangle %d: %w", i, err)
		}
	}

	cm := &CageMesh{
		rest:       restCopy,
		cells:      cellsCopy,
		facets:     b.facets,
		edges:      b.edges,
		generation: 1,
		stats:      b.stats,
	}
	cm.apply(restCopy, false)
	return cm, nil
}

type cagePolygon struct {
	cell int
	poly []Vector
}

func (b *cageBuilder) addTriangle(v [3]Vector) error {
	srcNormal := v[1].Sub(v[0]).Cross(v[2].Sub(v[0]))
	srcArea := 0.5 * srcNormal.Length()

	var accepted []cagePolygon
	for ci := range b.cells {
		b.stats.TriangleCellClips++
		poly := []Vector{v[0], v[1], v[2]}
		for _, pl := range b.planes[ci] {
			poly = cageClipPolygon(poly, pl, b.eps)
			if len(poly) < 3 {
				break
			}
		}
		if len(poly) < 3 {
			continue
		}
		poly = cageCleanPolygon(poly, b.eps)
		if len(poly) < 3 {
			continue
		}
		var centroid Vector
		for _, p := range poly {
			centroid = centroid.Add(p)
		}
		centroid = centroid.DivScalar(float64(len(poly)))
		if !b.cellOwns(ci, centroid) {
			continue
		}
		accepted = append(accepted, cagePolygon{ci, poly})
	}

	var coveredArea float64
	var facets []cageFacet
	for _, ap := range accepted {
		base := ap.poly[0]
		for k := 1; k+1 < len(ap.poly); k++ {
			a, c, d := base, ap.poly[k], ap.poly[k+1]
			normal := c.Sub(a).Cross(d.Sub(a))
			if cageDegenerateTriangle(a, c, d) {
				continue
			}
			if normal.Dot(srcNormal) < 0 {
				c, d = d, c
				normal = normal.MulScalar(-1)
			}
			coveredArea += 0.5 * normal.Length()
			wa, _ := cageBarycentric(b.rest, b.cells[ap.cell], a)
			wc, _ := cageBarycentric(b.rest, b.cells[ap.cell], c)
			wd, _ := cageBarycentric(b.rest, b.cells[ap.cell], d)
			facets = append(facets, cageFacet{
				v: [3]cageVertex{
					{cell: ap.cell, w: wa},
					{cell: ap.cell, w: wc},
					{cell: ap.cell, w: wd},
				},
			})
		}
	}

	if !cageDegenerateTriangle(v[0], v[1], v[2]) {
		tol := 1e-8 * srcArea
		floor := 1e-14 * b.scale * b.scale
		if tol < floor {
			tol = floor
		}
		if math.Abs(coveredArea-srcArea) > tol {
			return fmt.Errorf("triangle is not fully covered by the cage (covered %.12g of %.12g)", coveredArea, srcArea)
		}
	}

	b.facets = append(b.facets, facets...)

	var segs [3][]cageSegment
	edgeTol := 1e-10 * b.scale
	if edgeTol < 1e-14 {
		edgeTol = 1e-14
	}
	for _, ap := range accepted {
		n := len(ap.poly)
		for k := 0; k < n; k++ {
			p := ap.poly[k]
			q := ap.poly[(k+1)%n]
			if p.Distance(q) <= b.eps {
				continue
			}
			for e := 0; e < 3; e++ {
				a := v[e]
				bb := v[(e+1)%3]
				paramTol := edgeTol
				if l := a.Distance(bb); l > 0 {
					paramTol = edgeTol / l
				}
				tp, okp := cageEdgeParam(p, a, bb, edgeTol, paramTol)
				tq, okq := cageEdgeParam(q, a, bb, edgeTol, paramTol)
				if !okp || !okq {
					continue
				}
				tp = cageClamp01(tp)
				tq = cageClamp01(tq)
				if math.Abs(tp-tq) <= 1e-12 {
					continue
				}
				if tp > tq {
					tp, tq = tq, tp
				}
				segs[e] = append(segs[e], cageSegment{t0: tp, t1: tq, cell: ap.cell})
			}
		}
	}

	for e := 0; e < 3; e++ {
		path, err := b.buildPath(v[e], v[(e+1)%3], segs[e], edgeTol)
		if err != nil {
			return err
		}
		b.edges = append(b.edges, path)
	}
	return nil
}

func (b *cageBuilder) buildPath(a, c Vector, segs []cageSegment, tol float64) (cagePath, error) {
	if len(segs) == 0 {
		return cagePath{}, fmt.Errorf("triangle edge is not covered by the cage")
	}
	sort.SliceStable(segs, func(i, j int) bool {
		if segs[i].t0 == segs[j].t0 {
			return segs[i].t1 < segs[j].t1
		}
		return segs[i].t0 < segs[j].t0
	})

	ts := make([]float64, 0, len(segs)*2+2)
	ts = append(ts, 0, 1)
	for _, s := range segs {
		ts = append(ts, cageClamp01(s.t0), cageClamp01(s.t1))
	}
	sort.Float64s(ts)
	unique := ts[:0]
	for _, t := range ts {
		t = cageClamp01(t)
		if len(unique) == 0 || math.Abs(t-unique[len(unique)-1]) > 1e-12 {
			unique = append(unique, t)
		}
	}
	// Make sure the full original edge is present.
	if unique[0] > 0 {
		unique = append([]float64{0}, unique...)
	}
	if unique[len(unique)-1] < 1 {
		unique = append(unique, 1)
	}

	path := cagePath{pts: make([]cagePathVertex, 0, len(unique))}
	dir := c.Sub(a)
	paramTol := 1e-9
	if l := dir.Length(); l > 0 {
		paramTol = tol / l
	}
	for _, t := range unique {
		x := a.Add(dir.MulScalar(t))
		cell := -1
		bestCell := -1
		bestDist := math.Inf(1)
		for _, s := range segs {
			if t >= s.t0-paramTol && t <= s.t1+paramTol && (cell < 0 || s.cell < cell) {
				cell = s.cell
			}
			d := 0.0
			if t < s.t0 {
				d = s.t0 - t
			} else if t > s.t1 {
				d = t - s.t1
			}
			if d < bestDist {
				bestDist = d
				bestCell = s.cell
			}
		}
		if cell < 0 {
			cell = bestCell
		}
		if cell < 0 {
			return cagePath{}, fmt.Errorf("triangle edge is not covered by the cage")
		}
		w, _ := cageBarycentric(b.rest, b.cells[cell], x)
		path.pts = append(path.pts, cagePathVertex{cell: cell, w: w})
	}
	return path, nil
}
