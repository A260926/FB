package hitables

// PatchSet is an on-demand, page-resident bicubic Bezier micro-mesh hitable.
//
// A PatchDesc describes a tensor-product cubic Bernstein surface over [0,1]^2.
// Control points are laid out by v rows and u columns: Control[v*4+u].  The
// rendered geometry is a discrete triangle mesh: the parameter square is split
// into N = 2^level cells per axis and the surface position is evaluated at the
// grid vertices.  Every cell is triangulated as (00,10,11) and (00,11,01).
//
// The mesh is partitioned into 8x8-cell pages.  Page bounds are derived from a
// conservative control net of the corresponding parameter sub-domain (the
// convex-hull property of Bezier patches) and are organized in a lazy
// quadtree.  Triangle data for a page is only generated when a query actually
// reaches that page.

import (
	"container/list"
	"fmt"
	"math"
	"sort"
	"sync"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

const (
	patchPageCells = 8
	patchMinLevel  = 3
	patchMaxLevel  = 10
)

// PatchDesc describes one bicubic Bernstein patch.  Control points are stored
// in v-row major, u-column order: Control[v*4+u].
type PatchDesc struct {
	Control  [16]mgl64.Vec3
	Material raytrace.Material
}

// PatchStats is a snapshot of a PatchSet's work counters.  Resident and
// PeakResident are measured in pages.  Every resident page includes both its
// triangulated micro-mesh and its local triangle acceleration structure.
type PatchStats struct {
	Resident      int
	PeakResident  int
	PageBuilds    uint64
	Evictions     uint64
	TriangleTests uint64
	BoundsTests   uint64
}

type patchState struct {
	control  [16]mgl64.Vec3
	material raytrace.Material
	box      *raytrace.AABB
}

type pageKey struct {
	patch int
	pv    int
	pu    int
}

type pageTriangle struct {
	v      [3]mgl64.Vec3
	uv     [3]mgl64.Vec2
	normal mgl64.Vec3
	cell   int
	tri    int
}

type triangleBVHNode struct {
	mn    mgl64.Vec3
	mx    mgl64.Vec3
	left  int32
	right int32
	start int32
	count int32
}

type residentPage struct {
	key   pageKey
	tris  []pageTriangle
	order []int32
	nodes []triangleBVHNode
	elem  *list.Element
}

type patchHitCandidate struct {
	t        float64
	patch    int
	cell     int
	tri      int
	p        mgl64.Vec3
	normal   mgl64.Vec3
	uv       mgl64.Vec2
	material raytrace.Material
}

// PatchSet implements raytrace.Hitable on top of a lazily generated,
// page-resident Bezier micro-mesh.
type PatchSet struct {
	mu       sync.Mutex
	level    int
	n        int
	pageCols int
	capacity int
	patches  []patchState
	box      *raytrace.AABB
	pages    map[pageKey]*residentPage
	lru      *list.List
	stats    PatchStats
}

// NewPatchSet validates and copies patches and creates an empty page cache.
// level must be in [3,10] and residentPages must be at least 1.
func NewPatchSet(patches []PatchDesc, level, residentPages int) (*PatchSet, error) {
	if level < patchMinLevel || level > patchMaxLevel {
		return nil, fmt.Errorf("patch level %d out of range [%d,%d]", level, patchMinLevel, patchMaxLevel)
	}
	if residentPages < 1 {
		return nil, fmt.Errorf("residentPages must be >= 1, got %d", residentPages)
	}
	for i := range patches {
		if err := validatePatchDesc(patches[i]); err != nil {
			return nil, fmt.Errorf("patch %d: %w", i, err)
		}
	}

	s := &PatchSet{
		level:    level,
		n:        1 << uint(level),
		capacity: residentPages,
		pages:    make(map[pageKey]*residentPage),
		lru:      list.New(),
	}
	s.pageCols = s.n / patchPageCells
	s.patches = make([]patchState, len(patches))
	for i := range patches {
		s.patches[i] = makePatchState(patches[i])
	}
	s.recomputeRootBoxLocked()
	return s, nil
}

// ReplacePatch replaces one patch description.  It must be called between
// frames.  Invalid indices or descriptions leave all state untouched.
func (s *PatchSet) ReplacePatch(index int, desc PatchDesc) error {
	if err := validatePatchDesc(desc); err != nil {
		return fmt.Errorf("patch %d: %w", index, err)
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	if index < 0 || index >= len(s.patches) {
		return fmt.Errorf("patch index %d out of range [0,%d)", index, len(s.patches))
	}

	s.patches[index] = makePatchState(desc)
	s.invalidatePatchLocked(index)
	s.recomputeRootBoxLocked()
	return nil
}

// ReplaceAll atomically replaces every patch in the set.  The batch is fully
// validated before anything is committed.  level and the residency budget do
// not change.
func (s *PatchSet) ReplaceAll(patches []PatchDesc) error {
	for i := range patches {
		if err := validatePatchDesc(patches[i]); err != nil {
			return fmt.Errorf("patch %d: %w", i, err)
		}
	}

	states := make([]patchState, len(patches))
	for i := range patches {
		states[i] = makePatchState(patches[i])
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	s.invalidateAllLocked()
	s.patches = states
	s.recomputeRootBoxLocked()
	return nil
}

// Stats returns a consistent snapshot of residency and work counters.
func (s *PatchSet) Stats() PatchStats {
	s.mu.Lock()
	defer s.mu.Unlock()

	st := s.stats
	st.Resident = len(s.pages)
	return st
}

// Hit intersects the ray with the discrete triangle mesh.  t is the raw ray
// parameter (the direction need not be normalized) and the [tMin,tMax] window
// is closed.  Exact ties are resolved by patch index, then by cell in v-row
// major order, then by triangle order within the cell.
func (s *PatchSet) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {
	if ray == nil {
		return nil
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	if len(s.patches) == 0 || s.n <= 0 {
		return nil
	}
	// Also rejects NaN bounds.
	if !(tMin <= tMax) {
		return nil
	}

	var best *patchHitCandidate
	for pi := range s.patches {
		s.hitPageGroup(ray, pi, 0, s.pageCols, 0, s.pageCols, tMin, tMax, &best)
	}

	if best == nil {
		return nil
	}
	return raytrace.NewHitRecord(best.t, best.p, best.normal, best.uv, best.material)
}

// GetBoundingBox returns the conservative bounds of the whole patch set.  An
// empty set returns a non-nil empty box (Min = +Inf, Max = -Inf).
func (s *PatchSet) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.box
}

// GetPdfValue explicitly reports that PatchSet does not support surface-light
// sampling.
func (s *PatchSet) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	return 0.0
}

// Random returns a fixed direction.  PatchSet does not support surface-light
// sampling.
func (s *PatchSet) Random(origin mgl64.Vec3) mgl64.Vec3 {
	return vectorextensions.UnitX()
}

func validatePatchDesc(desc PatchDesc) error {
	if desc.Material == nil {
		return fmt.Errorf("material must not be nil")
	}
	for i, p := range desc.Control {
		if math.IsNaN(p.X()) || math.IsInf(p.X(), 0) ||
			math.IsNaN(p.Y()) || math.IsInf(p.Y(), 0) ||
			math.IsNaN(p.Z()) || math.IsInf(p.Z(), 0) {
			return fmt.Errorf("control point %d is not finite", i)
		}
	}
	return nil
}

func makePatchState(desc PatchDesc) patchState {
	return patchState{
		control:  desc.Control,
		material: desc.Material,
		box:      controlNetBox(desc.Control),
	}
}

func emptyAABB() *raytrace.AABB {
	inf := math.Inf(1)
	return raytrace.NewAABB(
		mgl64.Vec3{inf, inf, inf},
		mgl64.Vec3{-inf, -inf, -inf},
	)
}

func (s *PatchSet) recomputeRootBoxLocked() {
	if len(s.patches) == 0 {
		s.box = emptyAABB()
		return
	}
	box := s.patches[0].box
	for i := 1; i < len(s.patches); i++ {
		box = box.GetSurroundingBox(s.patches[i].box)
	}
	s.box = box
}

func (s *PatchSet) invalidatePatchLocked(patch int) {
	for key, page := range s.pages {
		if key.patch != patch {
			continue
		}
		s.lru.Remove(page.elem)
		delete(s.pages, key)
		s.stats.Evictions++
	}
}

func (s *PatchSet) invalidateAllLocked() {
	for key, page := range s.pages {
		s.lru.Remove(page.elem)
		delete(s.pages, key)
		s.stats.Evictions++
	}
	s.lru.Init()
}

// hitPageGroup recursively prunes a quadtree of page groups.  Each node's
// bound comes from restricting the control net to that parameter sub-domain.
func (s *PatchSet) hitPageGroup(ray *raytrace.Ray, patch, pu0, pu1, pv0, pv1 int, tMin, tMax float64, best **patchHitCandidate) {
	mn, mx := s.pageGroupBounds(patch, pu0, pu1, pv0, pv1)
	s.stats.BoundsTests++

	limit := tMax
	if *best != nil && (*best).t < limit {
		limit = (*best).t
	}
	if !rayHitsBox(ray, mn, mx, tMin, limit) {
		return
	}

	if pu1-pu0 == 1 && pv1-pv0 == 1 {
		page := s.ensurePage(patch, pu0, pv0)
		s.testPageTriangles(page, ray, tMin, tMax, best)
		return
	}

	midU := (pu0 + pu1) / 2
	midV := (pv0 + pv1) / 2
	splitU := pu1-pu0 > 1
	splitV := pv1-pv0 > 1

	if splitU && splitV {
		s.hitPageGroup(ray, patch, pu0, midU, pv0, midV, tMin, tMax, best)
		s.hitPageGroup(ray, patch, midU, pu1, pv0, midV, tMin, tMax, best)
		s.hitPageGroup(ray, patch, pu0, midU, midV, pv1, tMin, tMax, best)
		s.hitPageGroup(ray, patch, midU, pu1, midV, pv1, tMin, tMax, best)
	} else if splitU {
		s.hitPageGroup(ray, patch, pu0, midU, pv0, pv1, tMin, tMax, best)
		s.hitPageGroup(ray, patch, midU, pu1, pv0, pv1, tMin, tMax, best)
	} else {
		s.hitPageGroup(ray, patch, pu0, pu1, pv0, midV, tMin, tMax, best)
		s.hitPageGroup(ray, patch, pu0, pu1, midV, pv1, tMin, tMax, best)
	}
}

func (s *PatchSet) pageGroupBounds(patch, pu0, pu1, pv0, pv1 int) (mgl64.Vec3, mgl64.Vec3) {
	control := s.patches[patch].control
	u0 := float64(pu0*patchPageCells) / float64(s.n)
	u1 := float64(pu1*patchPageCells) / float64(s.n)
	v0 := float64(pv0*patchPageCells) / float64(s.n)
	v1 := float64(pv1*patchPageCells) / float64(s.n)
	sub := restrictPatchControl(control, u0, u1, v0, v1)
	box := controlNetBox(sub)
	return box.Min(), box.Max()
}

func (s *PatchSet) ensurePage(patch, pu, pv int) *residentPage {
	key := pageKey{patch: patch, pv: pv, pu: pu}
	if page, ok := s.pages[key]; ok {
		s.lru.MoveToFront(page.elem)
		return page
	}

	// Evict before building so the cache never transiently exceeds capacity.
	for len(s.pages) >= s.capacity {
		elem := s.lru.Back()
		if elem == nil {
			break
		}
		page := elem.Value.(*residentPage)
		s.lru.Remove(elem)
		delete(s.pages, page.key)
		s.stats.Evictions++
	}

	page := s.buildPage(patch, pu, pv)
	page.elem = s.lru.PushFront(page)
	s.pages[key] = page
	s.stats.PageBuilds++
	if resident := len(s.pages); resident > s.stats.PeakResident {
		s.stats.PeakResident = resident
	}
	return page
}

func (s *PatchSet) buildPage(patch, pu, pv int) *residentPage {
	control := s.patches[patch].control
	n := s.n

	var verts [9 * 9]mgl64.Vec3
	for gj := 0; gj <= patchPageCells; gj++ {
		v := float64(pv*patchPageCells+gj) / float64(n)
		for gi := 0; gi <= patchPageCells; gi++ {
			u := float64(pu*patchPageCells+gi) / float64(n)
			verts[gj*(patchPageCells+1)+gi] = evalPatchSurface(control, u, v)
		}
	}

	tris := make([]pageTriangle, 0, patchPageCells*patchPageCells*2)
	row := patchPageCells + 1
	for gj := 0; gj < patchPageCells; gj++ {
		for gi := 0; gi < patchPageCells; gi++ {
			i00 := gj*row + gi
			i10 := i00 + 1
			i01 := i00 + row
			i11 := i01 + 1

			cellU := pu*patchPageCells + gi
			cellV := pv*patchPageCells + gj
			cell := cellV*n + cellU

			uv00 := mgl64.Vec2{float64(cellU) / float64(n), float64(cellV) / float64(n)}
			uv10 := mgl64.Vec2{float64(cellU+1) / float64(n), float64(cellV) / float64(n)}
			uv11 := mgl64.Vec2{float64(cellU+1) / float64(n), float64(cellV+1) / float64(n)}
			uv01 := mgl64.Vec2{float64(cellU) / float64(n), float64(cellV+1) / float64(n)}

			if tri, ok := newPageTriangle(verts[i00], verts[i10], verts[i11], uv00, uv10, uv11, cell, 0); ok {
				tris = append(tris, tri)
			}
			if tri, ok := newPageTriangle(verts[i00], verts[i11], verts[i01], uv00, uv11, uv01, cell, 1); ok {
				tris = append(tris, tri)
			}
		}
	}

	nodes, order := buildTriangleBVH(tris)
	return &residentPage{key: pageKey{patch: patch, pv: pv, pu: pu}, tris: tris, order: order, nodes: nodes}
}

func newPageTriangle(a, b, c mgl64.Vec3, uvA, uvB, uvC mgl64.Vec2, cell, tri int) (pageTriangle, bool) {
	e1 := b.Sub(a)
	e2 := c.Sub(a)
	cross := e1.Cross(e2)
	length := cross.Len()
	if length == 0.0 || math.IsNaN(length) || math.IsInf(length, 0) {
		return pageTriangle{}, false
	}
	return pageTriangle{
		v:      [3]mgl64.Vec3{a, b, c},
		uv:     [3]mgl64.Vec2{uvA, uvB, uvC},
		normal: cross.Mul(1.0 / length),
		cell:   cell,
		tri:    tri,
	}, true
}

func buildTriangleBVH(tris []pageTriangle) ([]triangleBVHNode, []int32) {
	if len(tris) == 0 {
		return nil, nil
	}

	order := make([]int32, len(tris))
	for i := range order {
		order[i] = int32(i)
	}

	var nodes []triangleBVHNode

	var build func(start, end int) int32
	build = func(start, end int) int32 {
		mn := mgl64.Vec3{math.Inf(1), math.Inf(1), math.Inf(1)}
		mx := mgl64.Vec3{math.Inf(-1), math.Inf(-1), math.Inf(-1)}
		cmin := mgl64.Vec3{math.Inf(1), math.Inf(1), math.Inf(1)}
		cmax := mgl64.Vec3{math.Inf(-1), math.Inf(-1), math.Inf(-1)}

		for k := start; k < end; k++ {
			tri := &tris[order[k]]
			var c mgl64.Vec3
			for _, v := range tri.v {
				mn = mgl64.Vec3{math.Min(mn.X(), v.X()), math.Min(mn.Y(), v.Y()), math.Min(mn.Z(), v.Z())}
				mx = mgl64.Vec3{math.Max(mx.X(), v.X()), math.Max(mx.Y(), v.Y()), math.Max(mx.Z(), v.Z())}
				c = c.Add(v)
			}
			c = c.Mul(1.0 / 3.0)
			cmin = mgl64.Vec3{math.Min(cmin.X(), c.X()), math.Min(cmin.Y(), c.Y()), math.Min(cmin.Z(), c.Z())}
			cmax = mgl64.Vec3{math.Max(cmax.X(), c.X()), math.Max(cmax.Y(), c.Y()), math.Max(cmax.Z(), c.Z())}
		}

		index := int32(len(nodes))
		nodes = append(nodes, triangleBVHNode{
			mn:    mn,
			mx:    mx,
			left:  -1,
			right: -1,
			start: int32(start),
			count: int32(end - start),
		})

		if end-start <= 4 {
			return index
		}

		ext := cmax.Sub(cmin)
		axis := 0
		if ext.Y() > ext.X() {
			axis = 1
		}
		if ext.Z() > axisExtent(ext, axis) {
			axis = 2
		}
		sort.Slice(order[start:end], func(i, j int) bool {
			ta := &tris[order[start+i]]
			tb := &tris[order[start+j]]
			return triangleCentroidAxis(ta, axis) < triangleCentroidAxis(tb, axis)
		})

		mid := (start + end) / 2
		left := build(start, mid)
		right := build(mid, end)
		nodes[index].left = left
		nodes[index].right = right
		nodes[index].start = -1
		nodes[index].count = 0
		return index
	}

	build(0, len(tris))
	return nodes, order
}

func axisExtent(v mgl64.Vec3, axis int) float64 {
	switch axis {
	case 0:
		return v.X()
	case 1:
		return v.Y()
	default:
		return v.Z()
	}
}

func triangleCentroidAxis(tri *pageTriangle, axis int) float64 {
	return (axisExtent(tri.v[0], axis) + axisExtent(tri.v[1], axis) + axisExtent(tri.v[2], axis)) / 3.0
}

func (s *PatchSet) testPageTriangles(page *residentPage, ray *raytrace.Ray, tMin, tMax float64, best **patchHitCandidate) {
	if len(page.nodes) == 0 {
		return
	}

	rootLimit := tMax
	if *best != nil && (*best).t < rootLimit {
		rootLimit = (*best).t
	}

	stack := []int32{0}
	for len(stack) > 0 {
		index := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		node := &page.nodes[index]

		limit := rootLimit
		if *best != nil && (*best).t < limit {
			limit = (*best).t
		}
		s.stats.BoundsTests++
		if !rayHitsBox(ray, node.mn, node.mx, tMin, limit) {
			continue
		}

		if node.left < 0 {
			end := node.start + node.count
			for k := node.start; k < end; k++ {
				tri := &page.tris[page.order[k]]
				limit = tMax
				if *best != nil && (*best).t < limit {
					limit = (*best).t
				}
				s.stats.TriangleTests++
				t, u, v, ok := intersectTriangle(ray, tri, tMin, limit)
				if !ok {
					continue
				}

				candidate := &patchHitCandidate{
					t:        t,
					patch:    page.key.patch,
					cell:     tri.cell,
					tri:      tri.tri,
					p:        ray.GetPointAtParameter(t),
					normal:   tri.normal,
					uv:       interpolateUV(tri, u, v),
					material: s.patches[page.key.patch].material,
				}
				if betterHitCandidate(candidate, *best) {
					*best = candidate
				}
			}
		} else {
			stack = append(stack, node.right, node.left)
		}
	}
}

func interpolateUV(tri *pageTriangle, u, v float64) mgl64.Vec2 {
	w := 1.0 - u - v
	return tri.uv[0].Mul(w).Add(tri.uv[1].Mul(u)).Add(tri.uv[2].Mul(v))
}

func betterHitCandidate(a, b *patchHitCandidate) bool {
	if b == nil {
		return true
	}
	if a.t < b.t {
		return true
	}
	if a.t > b.t {
		return false
	}
	if a.patch != b.patch {
		return a.patch < b.patch
	}
	if a.cell != b.cell {
		return a.cell < b.cell
	}
	return a.tri < b.tri
}

// intersectTriangle runs the Moller-Trumbore test.  It returns the raw ray
// parameter t and the barycentric weights of the second and third vertices.
func intersectTriangle(ray *raytrace.Ray, tri *pageTriangle, tMin, tMax float64) (float64, float64, float64, bool) {
	origin := ray.Origin()
	direction := ray.Direction()

	edge1 := tri.v[1].Sub(tri.v[0])
	edge2 := tri.v[2].Sub(tri.v[0])
	h := direction.Cross(edge2)
	a := edge1.Dot(h)
	if a == 0.0 {
		return 0, 0, 0, false
	}

	f := 1.0 / a
	s := origin.Sub(tri.v[0])
	u := f * s.Dot(h)
	if u < 0.0 || u > 1.0 {
		return 0, 0, 0, false
	}

	q := s.Cross(edge1)
	v := f * direction.Dot(q)
	if v < 0.0 || u+v > 1.0 {
		return 0, 0, 0, false
	}

	t := f * edge2.Dot(q)
	if math.IsNaN(t) || t < tMin || t > tMax {
		return 0, 0, 0, false
	}
	return t, u, v, true
}

// rayHitsBox is an inclusive slab test.  Empty boxes never hit.
func rayHitsBox(ray *raytrace.Ray, mn, mx mgl64.Vec3, tMin, tMax float64) bool {
	if mn.X() > mx.X() || mn.Y() > mx.Y() || mn.Z() > mx.Z() {
		return false
	}

	o := ray.Origin()
	d := ray.Direction()
	for axis := 0; axis < 3; axis++ {
		lo := axisExtent(mn, axis)
		hi := axisExtent(mx, axis)
		origin := axisExtent(o, axis)
		dir := axisExtent(d, axis)

		if dir == 0.0 {
			if origin < lo || origin > hi {
				return false
			}
			continue
		}

		inv := 1.0 / dir
		t0 := (lo - origin) * inv
		t1 := (hi - origin) * inv
		if t0 > t1 {
			t0, t1 = t1, t0
		}
		if t0 > tMin {
			tMin = t0
		}
		if t1 < tMax {
			tMax = t1
		}
		if tMax < tMin {
			return false
		}
	}
	return true
}

func bernsteinCubic(t float64) [4]float64 {
	mt := 1.0 - t
	return [4]float64{
		mt * mt * mt,
		3.0 * t * mt * mt,
		3.0 * t * t * mt,
		t * t * t,
	}
}

func evalPatchSurface(control [16]mgl64.Vec3, u, v float64) mgl64.Vec3 {
	bu := bernsteinCubic(u)
	bv := bernsteinCubic(v)

	var p mgl64.Vec3
	for j := 0; j < 4; j++ {
		for i := 0; i < 4; i++ {
			p = p.Add(control[j*4+i].Mul(bu[i] * bv[j]))
		}
	}
	return p
}

func splitCurve(p [4]mgl64.Vec3, t float64) ([4]mgl64.Vec3, [4]mgl64.Vec3) {
	a := p[0].Mul(1.0 - t).Add(p[1].Mul(t))
	b := p[1].Mul(1.0 - t).Add(p[2].Mul(t))
	c := p[2].Mul(1.0 - t).Add(p[3].Mul(t))
	d := a.Mul(1.0 - t).Add(b.Mul(t))
	e := b.Mul(1.0 - t).Add(c.Mul(t))
	f := d.Mul(1.0 - t).Add(e.Mul(t))
	return [4]mgl64.Vec3{p[0], a, d, f}, [4]mgl64.Vec3{f, e, c, p[3]}
}

func restrictCurve(p [4]mgl64.Vec3, from, to float64) [4]mgl64.Vec3 {
	if from == 0.0 && to == 1.0 {
		return p
	}
	if from == 0.0 {
		left, _ := splitCurve(p, to)
		return left
	}

	_, right := splitCurve(p, from)
	if to == 1.0 {
		return right
	}
	fraction := (to - from) / (1.0 - from)
	left, _ := splitCurve(right, fraction)
	return left
}

// restrictPatchControl returns the control net of the patch restricted to the
// given parameter sub-domain.  The surface over that sub-domain lies inside the
// convex hull of the returned points.
func restrictPatchControl(control [16]mgl64.Vec3, u0, u1, v0, v1 float64) [16]mgl64.Vec3 {
	var tmp [16]mgl64.Vec3
	for j := 0; j < 4; j++ {
		row := [4]mgl64.Vec3{
			control[j*4+0],
			control[j*4+1],
			control[j*4+2],
			control[j*4+3],
		}
		restricted := restrictCurve(row, u0, u1)
		for i := 0; i < 4; i++ {
			tmp[j*4+i] = restricted[i]
		}
	}

	var out [16]mgl64.Vec3
	for i := 0; i < 4; i++ {
		column := [4]mgl64.Vec3{
			tmp[0*4+i],
			tmp[1*4+i],
			tmp[2*4+i],
			tmp[3*4+i],
		}
		restricted := restrictCurve(column, v0, v1)
		for j := 0; j < 4; j++ {
			out[j*4+i] = restricted[j]
		}
	}
	return out
}

func controlNetBox(control [16]mgl64.Vec3) *raytrace.AABB {
	mn := control[0]
	mx := control[0]
	for _, p := range control[1:] {
		mn = mgl64.Vec3{math.Min(mn.X(), p.X()), math.Min(mn.Y(), p.Y()), math.Min(mn.Z(), p.Z())}
		mx = mgl64.Vec3{math.Max(mx.X(), p.X()), math.Max(mx.Y(), p.Y()), math.Max(mx.Z(), p.Z())}
	}

	loX, hiX := padRange(mn.X(), mx.X())
	loY, hiY := padRange(mn.Y(), mx.Y())
	loZ, hiZ := padRange(mn.Z(), mx.Z())
	return raytrace.NewAABB(mgl64.Vec3{loX, loY, loZ}, mgl64.Vec3{hiX, hiY, hiZ})
}

// padRange adds a small relative margin so that floating point evaluation of
// the surface cannot fall outside a conservative control-net bound.
func padRange(lo, hi float64) (float64, float64) {
	magnitude := math.Max(math.Abs(lo), math.Abs(hi))
	margin := 1e-9 * (1.0 + magnitude)
	if math.IsInf(margin, 0) || math.IsNaN(margin) {
		return lo, hi
	}
	return lo - margin, hi + margin
}
