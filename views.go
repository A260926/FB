package simplify

import (
	"container/heap"
	"fmt"
	"math"
	"sort"
)

// View describes one camera. Matrix is row-major 4x4; a 3D point p is
// transformed with homogeneous coordinate W=1 and projected to
// ((X/W+1)*Width/2, (Y/W+1)*Height/2).
type View struct {
	Matrix [16]float64
	Width  float64
	Height float64
}

// ViewResult is the output of SimplifyViews. Original lists the distinct
// input positions in first-occurrence order while walking the input
// triangles; Representative[i] is the final position of the contraction
// cluster that Original[i] belongs to.
//
// The guarantee covers the screen-space displacement of original vertices
// only; it makes no claim about other points on the surface or about
// occlusion changes.
type ViewResult struct {
	Mesh           *Mesh
	Original       []Vector
	Representative []Vector
}

// vCluster is one contraction cluster, initially a distinct input vertex.
// A cluster keeps the original index of its surviving representative, so
// ordering by cluster index is ordering by original vertex number.
type vCluster struct {
	pos     Vector
	quadric Matrix
	members []int
	faces   map[int]bool
	alive   bool
}

// vEdge is one heap candidate between two alive clusters.
type vEdge struct {
	u, v  int
	cost  float64
	t     float64
	pos   Vector
	stale bool
	index int
}

type vHeap []*vEdge

func (h vHeap) Len() int { return len(h) }

func (h vHeap) Less(i, j int) bool {
	a, b := h[i], h[j]
	if a.cost != b.cost {
		return a.cost < b.cost
	}
	ai, aj := a.u, a.v
	if ai > aj {
		ai, aj = aj, ai
	}
	bi, bj := b.u, b.v
	if bi > bj {
		bi, bj = bj, bi
	}
	if ai != bi {
		return ai < bi
	}
	return aj < bj
}

func (h vHeap) Swap(i, j int) {
	h[i], h[j] = h[j], h[i]
	h[i].index = i
	h[j].index = j
}

func (h *vHeap) Push(x interface{}) {
	e := x.(*vEdge)
	e.index = len(*h)
	*h = append(*h, e)
}

func (h *vHeap) Pop() interface{} {
	old := *h
	n := len(old)
	e := old[n-1]
	e.index = -1
	*h = old[:n-1]
	return e
}

type interval struct {
	lo, hi float64
}

// quadIntervals returns the subset of [0,1] where c2*t^2+c1*t+c0 <= 0.
// Roots are solved in closed form; no sampling is performed. The feasible
// set of a quadratic on a segment is either empty, the whole segment, one
// interval or the union of two intervals.
func quadIntervals(c2, c1, c0 float64) []interval {
	scale := math.Max(1, math.Max(math.Abs(c2), math.Max(math.Abs(c1), math.Abs(c0))))
	eps := 1e-12 * scale
	if math.Abs(c2) <= eps {
		// linear or constant case
		if math.Abs(c1) <= eps {
			if c0 <= eps {
				return []interval{{0, 1}}
			}
			return nil
		}
		r := -c0 / c1
		if c1 > 0 { // q <= 0 for t >= r
			return clipIntervals([]interval{{r, 1}})
		}
		return clipIntervals([]interval{{0, r}})
	}
	disc := c1*c1 - 4*c2*c0
	if disc < 0 {
		if -disc <= eps*eps {
			disc = 0
		} else if c2 > 0 {
			return nil // convex and strictly positive
		} else {
			return []interval{{0, 1}} // concave and strictly negative
		}
	}
	s := math.Sqrt(disc)
	r1 := (-c1 - s) / (2 * c2)
	r2 := (-c1 + s) / (2 * c2)
	if r1 > r2 {
		r1, r2 = r2, r1
	}
	var set []interval
	if c2 > 0 {
		set = []interval{{r1, r2}}
	} else {
		set = []interval{{0, r1}, {r2, 1}}
	}
	return clipIntervals(set)
}

// clipIntervals restricts intervals to [0,1]. Adjacent intervals produced
// by distinct constraints are merged, but bounds are never expanded
// outward, so the returned set never contains a truly infeasible point.
func clipIntervals(set []interval) []interval {
	var out []interval
	for _, in := range set {
		lo := math.Max(0, in.lo)
		hi := math.Min(1, in.hi)
		if lo > hi {
			continue
		}
		if len(out) > 0 && lo <= out[len(out)-1].hi {
			if hi > out[len(out)-1].hi {
				out[len(out)-1].hi = hi
			}
		} else {
			out = append(out, interval{lo, hi})
		}
	}
	return out
}

func intersectIntervals(a, b []interval) []interval {
	var out []interval
	for _, x := range a {
		for _, y := range b {
			lo := math.Max(x.lo, y.lo)
			hi := math.Min(x.hi, y.hi)
			if lo <= hi {
				if len(out) > 0 && lo <= out[len(out)-1].hi {
					if hi > out[len(out)-1].hi {
						out[len(out)-1].hi = hi
					}
				} else {
					out = append(out, interval{lo, hi})
				}
			}
		}
	}
	return out
}

func containsT(set []interval, t float64) bool {
	if t < 0 || t > 1 {
		return false
	}
	for _, in := range set {
		if t >= in.lo && t <= in.hi {
			return true
		}
	}
	return false
}

// ndcPoint stores the normalized device coordinates (X/W, Y/W) of one
// original point in one view.
type ndcPoint struct {
	nx, ny float64
}

type sFace struct {
	verts   [3]int
	removed bool
}

// SimplifyViews contracts the input mesh under a per-vertex screen-space
// error budget. The input Mesh is never modified: a new mesh is returned
// together with the original-to-representative correspondence.
func SimplifyViews(input *Mesh, factor float64, views []View, maxPixels float64) (*ViewResult, error) {
	if err := validateInputs(input, factor, views, maxPixels); err != nil {
		return nil, err
	}

	// Number the distinct input positions by first occurrence while
	// walking the input triangles in order.
	origIndex := make(map[Vector]int)
	var originals []Vector
	triVerts := make([][3]int, len(input.Triangles))
	for ti, t := range input.Triangles {
		for k, p := range []Vector{t.V1, t.V2, t.V3} {
			id, ok := origIndex[p]
			if !ok {
				id = len(originals)
				origIndex[p] = id
				originals = append(originals, p)
			}
			triVerts[ti][k] = id
		}
	}

	n := len(originals)

	// Precompute the projection of every original point in every view and
	// verify the homogeneous W guarantee.
	ndc := make([][]ndcPoint, len(views))
	for vi := range views {
		ndc[vi] = make([]ndcPoint, n)
		for id, p := range originals {
			x, y, _, w := projectHomogeneous(&views[vi], p)
			if w < 1e-6 {
				return nil, fmt.Errorf("simplifyviews: original point %d has W=%g below 1e-6 in view %d", id, w, vi)
			}
			ndc[vi][id] = ndcPoint{x / w, y / w}
		}
	}

	// Validate a non-degenerate, oriented, manifold triangle soup; open
	// boundaries are allowed.
	edgeFaces := make(map[[2]int][]int)
	edgeDirs := make(map[[2]int][][2]int)
	for ti, tv := range triVerts {
		for k := 0; k < 3; k++ {
			a, b := tv[k], tv[(k+1)%3]
			if a == b {
				return nil, fmt.Errorf("simplifyviews: triangle %d is degenerate", ti)
			}
			key := edgeKey(a, b)
			edgeFaces[key] = append(edgeFaces[key], ti)
			edgeDirs[key] = append(edgeDirs[key], [2]int{a, b})
		}
	}
	for key, faces := range edgeFaces {
		if len(faces) > 2 {
			return nil, fmt.Errorf("simplifyviews: edge %v has %d incident faces, mesh is not manifold", key, len(faces))
		}
		if len(faces) == 2 {
			d0, d1 := edgeDirs[key][0], edgeDirs[key][1]
			if d0[0] == d1[0] && d0[1] == d1[1] {
				return nil, fmt.Errorf("simplifyviews: edge %v is used in the same direction by two faces", key)
			}
			if d0[0] != d1[1] || d0[1] != d1[0] {
				return nil, fmt.Errorf("simplifyviews: edge %v has inconsistent face orientation", key)
			}
		}
	}

	// Clusters, each starting with the accumulated quadric of its
	// incident original faces (the same accumulation plain Simplify uses).
	clusters := make([]*vCluster, n)
	for id, p := range originals {
		clusters[id] = &vCluster{
			pos:     p,
			members: []int{id},
			faces:   make(map[int]bool),
			alive:   true,
		}
	}
	faces := make([]*sFace, len(input.Triangles))
	for ti, tv := range triVerts {
		f := &sFace{verts: tv}
		faces[ti] = f
		q := input.Triangles[ti].Quadric()
		for _, id := range tv {
			clusters[id].faces[ti] = true
			clusters[id].quadric = clusters[id].quadric.Add(q)
		}
	}
	numFaces := len(faces)

	// Manifold adjacency and the current candidate per adjacency pair.
	adj := make(map[int]map[int]bool)
	edgeAt := make(map[int]map[int]*vEdge)
	boundary := make(map[[2]int]bool)
	addAdj := func(a, b int) {
		if adj[a] == nil {
			adj[a] = make(map[int]bool)
		}
		if adj[b] == nil {
			adj[b] = make(map[int]bool)
		}
		adj[a][b] = true
		adj[b][a] = true
	}
	for key, faces := range edgeFaces {
		addAdj(key[0], key[1])
		if len(faces) == 1 {
			boundary[key] = true
		}
	}

	parent := make([]int, n)
	for id := range parent {
		parent[id] = id
	}

	state := &viewState{
		views:     views,
		ndc:       ndc,
		maxPixels: maxPixels,
		clusters:  clusters,
		faces:     faces,
		adj:       adj,
		edgeAt:    edgeAt,
		boundary:  boundary,
		parent:    parent,
	}

	var queue vHeap
	heap.Init(&queue)
	for u := range adj {
		for v := range adj[u] {
			if u < v {
				if e := state.evaluate(u, v, numFaces); e != nil {
					state.storeEdge(e)
					heap.Push(&queue, e)
				}
			}
		}
	}

	target := int(float64(numFaces) * factor)
	for numFaces > target && queue.Len() > 0 {
		e := heap.Pop(&queue).(*vEdge)
		if e.stale || !clusters[e.u].alive || !clusters[e.v].alive {
			continue
		}
		// The cached evaluation is still valid: every contraction that
		// could change either cluster or its neighborhood marks this
		// entry stale, so the heap cost and the committed position agree.
		numFaces = state.contract(e, numFaces, &queue)
	}

	return state.buildResult(originals), nil
}

type viewState struct {
	views     []View
	ndc       [][]ndcPoint
	maxPixels float64
	clusters  []*vCluster
	faces     []*sFace
	adj       map[int]map[int]bool
	edgeAt    map[int]map[int]*vEdge
	boundary  map[[2]int]bool
	parent    []int
}

func (s *viewState) isBoundaryVertex(u int) bool {
	for x := range s.adj[u] {
		if s.boundary[edgeKey(u, x)] {
			return true
		}
	}
	return false
}

// edgeFaceCount counts the currently alive faces incident to edge (a,b).
func (s *viewState) edgeFaceCount(a, b int) int {
	count := 0
	for fid := range s.clusters[a].faces {
		f := s.faces[fid]
		if f.verts[0] == b || f.verts[1] == b || f.verts[2] == b {
			count++
		}
	}
	return count
}

// positionSatisfies directly re-projects every original point of both
// clusters in every view and checks the pixel budget at pos. The tolerance
// is limited to floating-point round-off scale.
func (s *viewState) positionSatisfies(membersA, membersB []int, pos Vector) bool {
	for vi := range s.views {
		view := &s.views[vi]
		nx, ny, w := projectNDC(view, pos)
		if w < 1e-6 {
			return false
		}
		px := (nx + 1) * view.Width / 2
		py := (ny + 1) * view.Height / 2
		for _, id := range append(append([]int{}, membersA...), membersB...) {
			m := s.ndc[vi][id]
			ox := (m.nx + 1) * view.Width / 2
			oy := (m.ny + 1) * view.Height / 2
			dist := math.Hypot(px-ox, py-oy)
			tol := 1e-12 * math.Max(1, math.Max(math.Abs(px), math.Abs(py)))
			if dist > s.maxPixels+tol {
				return false
			}
		}
	}
	return true
}

// projectNDC returns the normalized device coordinates and homogeneous W
// of a point under the row-major view matrix.
func projectNDC(view *View, p Vector) (nx, ny, w float64) {
	x, y, _, ww := projectHomogeneous(view, p)
	return x / ww, y / ww, ww
}

func (s *viewState) storeEdge(e *vEdge) {
	if s.edgeAt[e.u] == nil {
		s.edgeAt[e.u] = make(map[int]*vEdge)
	}
	if s.edgeAt[e.v] == nil {
		s.edgeAt[e.v] = make(map[int]*vEdge)
	}
	s.edgeAt[e.u][e.v] = e
	s.edgeAt[e.v][e.u] = e
}

// evaluate tests one potential contraction without mutating anything. It
// returns the candidate with its exact feasible position, or nil if no
// legal contraction exists.
func (s *viewState) evaluate(u, v, numFaces int) *vEdge {
	cu, cv := s.clusters[u], s.clusters[v]

	// Boundary rule: an edge touching a boundary vertex may only collapse
	// when it is itself a boundary edge. Together with the link condition
	// this keeps every boundary loop closed and preserves its connectivity
	// (no merging of distinct loops, no swallowed boundary vertices).
	if !s.boundary[edgeKey(u, v)] && (s.isBoundaryVertex(u) || s.isBoundaryVertex(v)) {
		return nil
	}

	// Link condition: the common neighbors must be exactly the opposite
	// vertices of the faces incident to edge (u,v). This preserves
	// manifoldness and prevents duplicate faces after the merge.
	required := make(map[int]bool)
	for fid := range cu.faces {
		f := s.faces[fid]
		if f.verts[0] == v || f.verts[1] == v || f.verts[2] == v {
			for _, w := range f.verts {
				if w != u && w != v {
					required[w] = true
				}
			}
		}
	}
	for w := range s.adj[u] {
		if w != v && s.adj[v][w] && !required[w] {
			return nil
		}
	}
	for w := range required {
		if !s.adj[v][w] {
			return nil
		}
	}

	// Intersect the projection constraints of every original point of
	// both clusters in every view. Constraints are accumulated against
	// the original input points, not against the current endpoints.
	feasible := []interval{{0, 1}}
	a := cu.pos
	d := cv.pos.Sub(a)
	for vi := range s.views {
		view := &s.views[vi]
		r0 := row(view, 0)
		r1 := row(view, 1)
		r3 := row(view, 3)
		aw := r3[0]*d.X + r3[1]*d.Y + r3[2]*d.Z
		bw := r3[0]*a.X + r3[1]*a.Y + r3[2]*a.Z + r3[3]
		ux := view.Width / 2
		uy := view.Height / 2
		for _, id := range cu.members {
			feasible = intersectIntervals(feasible,
				memberInterval(s.ndc[vi][id], r0, r1, r3, aw, bw, ux, uy, d, a, s.maxPixels))
			if len(feasible) == 0 {
				return nil
			}
		}
		for _, id := range cv.members {
			feasible = intersectIntervals(feasible,
				memberInterval(s.ndc[vi][id], r0, r1, r3, aw, bw, ux, uy, d, a, s.maxPixels))
			if len(feasible) == 0 {
				return nil
			}
		}
	}

	// Simulate the face update: reject degenerate remapped faces,
	// duplicate faces, and a contraction that removes the last face.
	affected := make(map[int]bool)
	for fid := range cu.faces {
		affected[fid] = true
	}
	for fid := range cv.faces {
		affected[fid] = true
	}
	surviving := make(map[[3]int]bool)
	collapsed := 0
	for fid := range affected {
		f := s.faces[fid]
		hasU := f.verts[0] == u || f.verts[1] == u || f.verts[2] == u
		hasV := f.verts[0] == v || f.verts[1] == v || f.verts[2] == v
		if hasU && hasV {
			collapsed++
			continue
		}
		var remapped [3]int
		for k, w := range f.verts {
			if w == v {
				remapped[k] = u
			} else {
				remapped[k] = w
			}
		}
		if remapped[0] == remapped[1] || remapped[0] == remapped[2] || remapped[1] == remapped[2] {
			return nil
		}
		key := sortedTriple(remapped)
		if surviving[key] {
			return nil
		}
		surviving[key] = true
	}
	if numFaces-collapsed <= 0 {
		return nil
	}

	// Minimize the accumulated quadric sum over the exact feasible set.
	q := cu.quadric.Add(cv.quadric)
	t, pos := bestPointOnSegment(q, a, d, feasible)

	// Guard against floating-point error at interval boundaries: verify
	// every accumulated constraint directly at the committed position and,
	// if the unconstrained optimum falls a rounding error outside, fall
	// back to a verified feasible interval endpoint.
	if !s.positionSatisfies(cu.members, cv.members, pos) {
		fallback := math.Inf(1)
		found := false
		for _, in := range feasible {
			for _, cand := range [2]float64{in.lo, in.hi} {
				p := a.Add(d.MulScalar(cand))
				if s.positionSatisfies(cu.members, cv.members, p) {
					err := q.QuadricError(p)
					tie := math.Abs(err-fallback) <= 1e-12*math.Max(1, math.Abs(fallback))
					if !found || err < fallback || (tie && closerToHalf(cand, t)) {
						fallback, found, t, pos = err, true, cand, p
					}
				}
			}
		}
		if !found {
			return nil
		}
	}

	// Normal gate at the committed position, using the same 1e-3 dot
	// product threshold as plain Simplify, against each face's current
	// normal.
	for fid := range affected {
		f := s.faces[fid]
		hasU := f.verts[0] == u || f.verts[1] == u || f.verts[2] == u
		hasV := f.verts[0] == v || f.verts[1] == v || f.verts[2] == v
		if hasU && hasV {
			continue
		}
		oldP := [3]Vector{s.clusters[f.verts[0]].pos, s.clusters[f.verts[1]].pos, s.clusters[f.verts[2]].pos}
		oldNormal := oldP[1].Sub(oldP[0]).Cross(oldP[2].Sub(oldP[0]))
		if oldNormal.Length() == 0 {
			return nil
		}
		newP := oldP
		for k, w := range f.verts {
			if w == u || w == v {
				newP[k] = pos
			}
		}
		newNormal := newP[1].Sub(newP[0]).Cross(newP[2].Sub(newP[0]))
		if newNormal.Length() == 0 {
			return nil
		}
		if newNormal.Normalize().Dot(oldNormal.Normalize()) < 1e-3 {
			return nil
		}
	}

	return &vEdge{u: u, v: v, cost: q.QuadricError(pos), t: t, pos: pos}
}

// memberInterval builds the quadratic q(t) <= 0 expressing the pixel
// distance budget of one original point in one view along a+t*d and
// returns its feasible intervals.
func memberInterval(m ndcPoint, r0, r1, r3 [4]float64, aw, bw, ux, uy float64, d, a Vector, maxPixels float64) []interval {
	// gx(t) = (r0 - nx*r3) . (a+t*d, 1); the pixel offset is ux*gx/w
	// (and analogously uy*gy/w), so the budget becomes
	// (ux*gx)^2+(uy*gy)^2-r^2*w^2 <= 0.
	gx := [4]float64{
		r0[0] - m.nx*r3[0],
		r0[1] - m.nx*r3[1],
		r0[2] - m.nx*r3[2],
		r0[3] - m.nx*r3[3],
	}
	gy := [4]float64{
		r1[0] - m.ny*r3[0],
		r1[1] - m.ny*r3[1],
		r1[2] - m.ny*r3[2],
		r1[3] - m.ny*r3[3],
	}
	alphaX := gx[0]*d.X + gx[1]*d.Y + gx[2]*d.Z
	betaX := gx[0]*a.X + gx[1]*a.Y + gx[2]*a.Z + gx[3]
	alphaY := gy[0]*d.X + gy[1]*d.Y + gy[2]*d.Z
	betaY := gy[0]*a.X + gy[1]*a.Y + gy[2]*a.Z + gy[3]
	r := maxPixels
	c2 := ux*ux*alphaX*alphaX + uy*uy*alphaY*alphaY - r*r*aw*aw
	c1 := 2*(ux*ux*alphaX*betaX+uy*uy*alphaY*betaY) - 2*r*r*aw*bw
	c0 := ux*ux*betaX*betaX + uy*uy*betaY*betaY - r*r*bw*bw
	return quadIntervals(c2, c1, c0)
}

// qForm evaluates the full 4x4 quadric form at a homogeneous point.
func qForm(q Matrix, x, y, z, w float64) float64 {
	return (x*q.x00*x + y*q.x10*x + z*q.x20*x + w*q.x30*x +
		x*q.x01*y + y*q.x11*y + z*q.x21*y + w*q.x31*y +
		x*q.x02*z + y*q.x12*z + z*q.x22*z + w*q.x32*z +
		x*q.x03*w + y*q.x13*w + z*q.x23*w + w*q.x33*w)
}

// bestPointOnSegment minimizes q on the feasible interval set. When the
// quadric is constant along the segment, the feasible point nearest to
// t=0.5 is chosen.
func bestPointOnSegment(q Matrix, a, d Vector, set []interval) (float64, Vector) {
	A := qForm(q, d.X, d.Y, d.Z, 0)
	C := qForm(q, a.X, a.Y, a.Z, 1)
	B := qForm(q, a.X+d.X, a.Y+d.Y, a.Z+d.Z, 1) - A - C
	scale := math.Max(1, math.Max(math.Abs(A), math.Max(math.Abs(B), math.Abs(C))))

	candidates := make(map[float64]bool)
	for _, in := range set {
		candidates[in.lo] = true
		candidates[in.hi] = true
	}
	if math.Abs(A) > 1e-12*scale || math.Abs(B) > 1e-12*scale {
		t0 := -B / (2 * A)
		if containsT(set, t0) {
			candidates[t0] = true
		}
	}
	if containsT(set, 0.5) {
		candidates[0.5] = true
	}

	constant := math.Abs(A) <= 1e-12*scale && math.Abs(B) <= 1e-12*scale
	if constant {
		t := nearestToHalf(set)
		return t, a.Add(d.MulScalar(t))
	}

	bestT := 0.0
	bestErr := math.Inf(1)
	for t := range candidates {
		t = math.Max(0, math.Min(1, t))
		if !containsT(set, t) {
			continue
		}
		err := q.QuadricError(a.Add(d.MulScalar(t)))
		tie := math.Abs(err-bestErr) <= 1e-12*math.Max(1, math.Abs(bestErr))
		if err < bestErr || (tie && closerToHalf(t, bestT)) {
			bestErr = err
			bestT = t
		}
	}
	return bestT, a.Add(d.MulScalar(bestT))
}

func closerToHalf(t, best float64) bool {
	dt := math.Abs(t - 0.5)
	db := math.Abs(best - 0.5)
	if dt != db {
		return dt < db
	}
	return t < best
}

func nearestToHalf(set []interval) float64 {
	best := 0.0
	bestDist := math.Inf(1)
	for _, in := range set {
		var t float64
		switch {
		case 0.5 < in.lo:
			t = in.lo
		case 0.5 > in.hi:
			t = in.hi
		default:
			t = 0.5
		}
		if dist := math.Abs(t - 0.5); dist < bestDist || (dist == bestDist && t < best) {
			bestDist = dist
			best = t
		}
	}
	return best
}

// contract commits one accepted edge: clusters, quadrics, faces,
// adjacency and candidates are updated together; rejected evaluations
// never mutate anything.
func (s *viewState) contract(e *vEdge, numFaces int, queue *vHeap) int {
	u, v := e.u, e.v
	if u > v {
		u, v = v, u
	}
	cu, cv := s.clusters[u], s.clusters[v]

	// Merge faces: faces containing both endpoints collapse, faces
	// containing v are rewired to u.
	affected := make(map[int]bool)
	for fid := range cu.faces {
		affected[fid] = true
	}
	for fid := range cv.faces {
		affected[fid] = true
	}
	newFaces := make(map[int]bool)
	for fid := range affected {
		f := s.faces[fid]
		hasU := f.verts[0] == u || f.verts[1] == u || f.verts[2] == u
		hasV := f.verts[0] == v || f.verts[1] == v || f.verts[2] == v
		if hasU && hasV {
			f.removed = true
			numFaces--
			continue
		}
		if hasV {
			for k, w := range f.verts {
				if w == v {
					f.verts[k] = u
				}
			}
		}
		newFaces[fid] = true
	}

	// Collect the merged neighborhood before tearing adjacency down.
	neighbors := make(map[int]bool)
	for x := range s.adj[u] {
		if x != v {
			neighbors[x] = true
		}
	}
	for x := range s.adj[v] {
		if x != u {
			neighbors[x] = true
		}
	}

	// Invalidate every candidate touching the merged 1-ring. A rewired
	// face (v,x,z)->(u,x,z) changes the face set seen by a candidate
	// between two neighbors, so the whole ring must be re-evaluated.
	touched := make(map[int]bool)
	touched[u] = true
	for x := range neighbors {
		touched[x] = true
	}
	var staleEdges []*vEdge
	for x := range touched {
		for _, old := range s.edgeAt[x] {
			staleEdges = append(staleEdges, old)
		}
	}
	seenStale := make(map[*vEdge]bool)
	for _, old := range staleEdges {
		if seenStale[old] {
			continue
		}
		seenStale[old] = true
		old.stale = true
	}
	for x := range touched {
		s.edgeAt[x] = make(map[int]*vEdge)
	}
	delete(s.adj, v)
	for x := range s.adj {
		delete(s.adj[x], v)
	}
	s.adj[u] = make(map[int]bool)

	// Merge cluster state.
	cu.members = append(cu.members, cv.members...)
	for _, id := range cv.members {
		s.parent[id] = u
	}
	cu.quadric = cu.quadric.Add(cv.quadric)
	cu.pos = e.pos
	cu.faces = newFaces
	cv.alive = false
	cv.faces = nil
	cv.members = nil

	// Rebuild adjacency and re-evaluate every candidate incident to the
	// touched 1-ring: spokes, ring edges and edges leaving the ring.
	for x := range neighbors {
		s.adj[u][x] = true
		s.adj[x][u] = true
	}
	// Recompute boundary status for the new spokes and drop every entry
	// that mentioned the removed cluster. Other edges keep their status.
	for x := range neighbors {
		if s.edgeFaceCount(u, x) == 1 {
			s.boundary[edgeKey(u, x)] = true
		} else {
			delete(s.boundary, edgeKey(u, x))
		}
	}
	for key := range s.boundary {
		if key[0] == v || key[1] == v {
			delete(s.boundary, key)
		}
	}
	ids := make([]int, 0, len(neighbors))
	for x := range neighbors {
		ids = append(ids, x)
	}
	sort.Ints(ids)
	ensure := func(a, b int) {
		if a > b {
			a, b = b, a
		}
		if s.edgeAt[a][b] != nil {
			return
		}
		if ne := s.evaluate(a, b, numFaces); ne != nil {
			s.storeEdge(ne)
			heap.Push(queue, ne)
		}
	}
	for _, x := range ids {
		ensure(u, x)
	}
	for _, x := range ids {
		for y := range s.adj[x] {
			if y == u {
				continue
			}
			ensure(x, y)
		}
	}
	return numFaces
}

func (s *viewState) buildResult(originals []Vector) *ViewResult {
	type outFace struct {
		key [3]int
		tri *Triangle
	}
	var out []outFace
	for _, f := range s.faces {
		if f.removed {
			continue
		}
		ps := [3]Vector{}
		for k, id := range f.verts {
			ps[k] = s.clusters[id].pos
		}
		out = append(out, outFace{key: f.verts, tri: NewTriangle(ps[0], ps[1], ps[2])})
	}
	// Canonical output ordering by the smallest original indices of the
	// surviving face clusters.
	sort.Slice(out, func(i, j int) bool {
		ai := sortedTriple(out[i].key)
		bi := sortedTriple(out[j].key)
		for k := 0; k < 3; k++ {
			if ai[k] != bi[k] {
				return ai[k] < bi[k]
			}
		}
		return false
	})
	triangles := make([]*Triangle, len(out))
	for i, of := range out {
		triangles[i] = of.tri
	}

	representative := make([]Vector, len(originals))
	for id := range originals {
		root := id
		for s.parent[root] != root {
			root = s.parent[root]
		}
		representative[id] = s.clusters[root].pos
	}
	return &ViewResult{
		Mesh:           NewMesh(triangles),
		Original:       originals,
		Representative: representative,
	}
}

func sortedTriple(v [3]int) [3]int {
	out := v
	if out[0] > out[1] {
		out[0], out[1] = out[1], out[0]
	}
	if out[1] > out[2] {
		out[1], out[2] = out[2], out[1]
	}
	if out[0] > out[1] {
		out[0], out[1] = out[1], out[0]
	}
	return out
}

func validateInputs(input *Mesh, factor float64, views []View, maxPixels float64) error {
	if input == nil {
		return fmt.Errorf("simplifyviews: nil input mesh")
	}
	if len(input.Triangles) == 0 {
		return fmt.Errorf("simplifyviews: empty input mesh")
	}
	if math.IsNaN(factor) || math.IsInf(factor, 0) || factor <= 0 || factor > 1 {
		return fmt.Errorf("simplifyviews: factor must be in (0,1], got %g", factor)
	}
	if len(views) < 1 || len(views) > 8 {
		return fmt.Errorf("simplifyviews: need 1 to 8 views, got %d", len(views))
	}
	if math.IsNaN(maxPixels) || math.IsInf(maxPixels, 0) || maxPixels < 0 {
		return fmt.Errorf("simplifyviews: maxPixels must be finite and non-negative, got %g", maxPixels)
	}
	for vi, v := range views {
		if math.IsNaN(v.Width) || math.IsInf(v.Width, 0) || v.Width <= 0 ||
			math.IsNaN(v.Height) || math.IsInf(v.Height, 0) || v.Height <= 0 {
			return fmt.Errorf("simplifyviews: view %d has invalid viewport %gx%g", vi, v.Width, v.Height)
		}
		for k, m := range v.Matrix {
			if math.IsNaN(m) || math.IsInf(m, 0) {
				return fmt.Errorf("simplifyviews: view %d matrix entry %d is not finite", vi, k)
			}
		}
	}
	for ti, t := range input.Triangles {
		for k, p := range []Vector{t.V1, t.V2, t.V3} {
			if math.IsNaN(p.X) || math.IsInf(p.X, 0) ||
				math.IsNaN(p.Y) || math.IsInf(p.Y, 0) ||
				math.IsNaN(p.Z) || math.IsInf(p.Z, 0) {
				return fmt.Errorf("simplifyviews: triangle %d vertex %d is not finite", ti, k)
			}
		}
		if t.Normal().Length() == 0 {
			return fmt.Errorf("simplifyviews: triangle %d is degenerate", ti)
		}
	}
	return nil
}

func edgeKey(a, b int) [2]int {
	if a > b {
		a, b = b, a
	}
	return [2]int{a, b}
}

func row(v *View, r int) [4]float64 {
	i := r * 4
	return [4]float64{v.Matrix[i], v.Matrix[i+1], v.Matrix[i+2], v.Matrix[i+3]}
}

// projectHomogeneous applies the row-major view matrix with W=1.
func projectHomogeneous(v *View, p Vector) (x, y, z, w float64) {
	for r := 0; r < 4; r++ {
		val := v.Matrix[r*4]*p.X + v.Matrix[r*4+1]*p.Y + v.Matrix[r*4+2]*p.Z + v.Matrix[r*4+3]
		switch r {
		case 0:
			x = val
		case 1:
			y = val
		case 2:
			z = val
		case 3:
			w = val
		}
	}
	return
}
