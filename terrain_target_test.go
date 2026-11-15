package fauxgl

import (
	"fmt"
	"math"
	"reflect"
	"sort"
	"sync"
	"testing"

	"github.com/fogleman/fauxgl/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// independent reference helpers (written separately from the implementation)
// ---------------------------------------------------------------------------

func refHeights(size int, fn func(x, z int) float64) []float64 {
	h := make([]float64, size*size)
	for z := 0; z < size; z++ {
		for x := 0; x < size; x++ {
			h[z*size+x] = fn(x, z)
		}
	}
	return h
}

func refNormalAt(h []float64, size, x, z int) Vector {
	at := func(x, z int) float64 { return h[z*size+x] }
	var dx, dz float64
	switch {
	case x == 0:
		dx = at(1, z) - at(0, z)
	case x == size-1:
		dx = at(size-1, z) - at(size-2, z)
	default:
		dx = (at(x+1, z) - at(x-1, z)) / 2
	}
	switch {
	case z == 0:
		dz = at(x, 1) - at(x, 0)
	case z == size-1:
		dz = at(x, size-1) - at(x, size-2)
	default:
		dz = (at(x, z+1) - at(x, z-1)) / 2
	}
	return Vector{-dx, 1, -dz}.Normalize()
}

type refNode struct{ d, qx, qz int }

func refNodeBox(h []float64, size, k int, n refNode, origin Vector, scale float64) Box {
	span := 1 << (k - n.d)
	x0, z0 := n.qx*span, n.qz*span
	minY := math.Inf(1)
	maxY := math.Inf(-1)
	for z := z0; z <= z0+span; z++ {
		for x := x0; x <= x0+span; x++ {
			y := h[z*size+x]
			if y < minY {
				minY = y
			}
			if y > maxY {
				maxY = y
			}
		}
	}
	toWorld := func(p Vector) Vector { return origin.Add(p.MulScalar(scale)) }
	return Box{
		Min: toWorld(Vector{float64(x0), minY, float64(z0)}),
		Max: toWorld(Vector{float64(x0 + span), maxY, float64(z0 + span)}),
	}
}

func refPointBoxDistance(p Vector, b Box) float64 {
	clamp := func(v, lo, hi float64) float64 {
		if v < lo {
			return lo
		}
		if v > hi {
			return hi
		}
		return v
	}
	q := Vector{clamp(p.X, b.Min.X, b.Max.X), clamp(p.Y, b.Min.Y, b.Max.Y), clamp(p.Z, b.Min.Z, b.Max.Z)}
	return p.Sub(q).Length()
}

// independent replication of the documented selection + 2:1 closure
func refSelect(h []float64, size, k int, origin Vector, scale float64, view TerrainView) map[refNode]struct{} {
	leafDepth := k - 3
	chosen := map[refNode]struct{}{}
	var walk func(n refNode)
	walk = func(n refNode) {
		if n.d == leafDepth {
			chosen[n] = struct{}{}
			return
		}
		box := refNodeBox(h, size, k, n, origin, scale)
		dist := refPointBoxDistance(view.Eye, box)
		if dist < view.SplitDistances[n.d] {
			walk(refNode{n.d + 1, 2 * n.qx, 2 * n.qz})
			walk(refNode{n.d + 1, 2*n.qx + 1, 2 * n.qz})
			walk(refNode{n.d + 1, 2 * n.qx, 2*n.qz + 1})
			walk(refNode{n.d + 1, 2*n.qx + 1, 2*n.qz + 1})
			return
		}
		chosen[n] = struct{}{}
	}
	walk(refNode{0, 0, 0})

	spanOf := func(n refNode) int { return 1 << (k - n.d) }
	find := func(x, z int) refNode {
		for d := 0; ; d++ {
			span := 1 << (k - d)
			qx, qz := x/span, z/span
			if m := (1 << d) - 1; qx > m {
				qx = m
			} else if qz > m {
				qz = m
			}
			n := refNode{d, qx, qz}
			if _, ok := chosen[n]; ok {
				return n
			}
		}
	}
	for {
		var split []refNode
		marked := map[refNode]struct{}{}
		for n := range chosen {
			if n.d == leafDepth {
				continue
			}
			span := spanOf(n)
			x0, z0 := n.qx*span, n.qz*span
			half := span / 2
			probe := func(x, z int) {
				nb := find(x, z)
				if spanOf(nb) > 2*span {
					if _, ok := marked[nb]; !ok {
						marked[nb] = struct{}{}
						split = append(split, nb)
					}
				}
			}
			if x0 > 0 {
				probe(x0-1, z0+half)
			}
			if x0+span < size-1 {
				probe(x0+span, z0+half)
			}
			if z0 > 0 {
				probe(x0+half, z0-1)
			}
			if z0+span < size-1 {
				probe(x0+half, z0+span)
			}
		}
		if len(split) == 0 {
			break
		}
		for _, n := range split {
			delete(chosen, n)
			chosen[refNode{n.d + 1, 2 * n.qx, 2 * n.qz}] = struct{}{}
			chosen[refNode{n.d + 1, 2*n.qx + 1, 2 * n.qz}] = struct{}{}
			chosen[refNode{n.d + 1, 2 * n.qx, 2*n.qz + 1}] = struct{}{}
			chosen[refNode{n.d + 1, 2*n.qx + 1, 2*n.qz + 1}] = struct{}{}
		}
	}
	return chosen
}

func refStitch(h []float64, size, k int, chosen map[refNode]struct{}, n refNode) uint8 {
	span := 1 << (k - n.d)
	x0, z0 := n.qx*span, n.qz*span
	half := span / 2
	find := func(x, z int) refNode {
		for d := 0; ; d++ {
			sp := 1 << (k - d)
			qx, qz := x/sp, z/sp
			if m := (1 << d) - 1; qx > m {
				qx = m
			} else if qz > m {
				qz = m
			}
			cand := refNode{d, qx, qz}
			if _, ok := chosen[cand]; ok {
				return cand
			}
		}
	}
	var stitch uint8
	if x0 > 0 {
		if nb := find(x0-1, z0+half); 1<<(k-nb.d) > span {
			stitch |= stitchWest
		}
	}
	if x0+span < size-1 {
		if nb := find(x0+span, z0+half); 1<<(k-nb.d) > span {
			stitch |= stitchEast
		}
	}
	if z0 > 0 {
		if nb := find(x0+half, z0-1); 1<<(k-nb.d) > span {
			stitch |= stitchSouth
		}
	}
	if z0+span < size-1 {
		if nb := find(x0+half, z0+span); 1<<(k-nb.d) > span {
			stitch |= stitchNorth
		}
	}
	return stitch
}

// independently built stitch index mapping (same geometric convention)
func refLeafIndices(stitch uint8) [][3]int {
	idx := func(i, j int) int { return j*9 + i }
	var out [][3]int
	for j := 0; j < 8; j++ {
		for i := 0; i < 8; i++ {
			out = append(out, [3]int{idx(i, j), idx(i, j+1), idx(i+1, j)},
				[3]int{idx(i+1, j), idx(i, j+1), idx(i+1, j+1)})
		}
	}
	move := make(map[int]int)
	if stitch&stitchWest != 0 {
		for _, j := range []int{1, 3, 5, 7} {
			move[idx(0, j)] = idx(0, j-1)
		}
	}
	if stitch&stitchEast != 0 {
		for _, j := range []int{1, 3, 5, 7} {
			move[idx(8, j)] = idx(8, j-1)
		}
	}
	if stitch&stitchSouth != 0 {
		for _, i := range []int{1, 3, 5, 7} {
			move[idx(i, 0)] = idx(i-1, 0)
		}
	}
	if stitch&stitchNorth != 0 {
		for _, i := range []int{1, 3, 5, 7} {
			move[idx(i, 8)] = idx(i-1, 8)
		}
	}
	mapIdx := func(v int) int {
		if r, ok := move[v]; ok {
			return r
		}
		return v
	}
	var final [][3]int
	for _, t := range out {
		m := [3]int{mapIdx(t[0]), mapIdx(t[1]), mapIdx(t[2])}
		if m[0] == m[1] || m[1] == m[2] || m[0] == m[2] {
			continue
		}
		final = append(final, m)
	}
	return final
}

func refCulled(h []float64, size, k int, n refNode, inst TerrainInstance, m Matrix) bool {
	box := refNodeBox(h, size, k, n, inst.Origin, inst.Scale)
	cs := [8]Vector{
		{box.Min.X, box.Min.Y, box.Min.Z}, {box.Max.X, box.Min.Y, box.Min.Z},
		{box.Min.X, box.Max.Y, box.Min.Z}, {box.Max.X, box.Max.Y, box.Min.Z},
		{box.Min.X, box.Min.Y, box.Max.Z}, {box.Max.X, box.Min.Y, box.Max.Z},
		{box.Min.X, box.Max.Y, box.Max.Z}, {box.Max.X, box.Max.Y, box.Max.Z},
	}
	var out [6]int
	for _, c := range cs {
		cl := m.MulPositionW(c)
		if cl.X < -cl.W {
			out[0]++
		}
		if cl.X > cl.W {
			out[1]++
		}
		if cl.Y < -cl.W {
			out[2]++
		}
		if cl.Y > cl.W {
			out[3]++
		}
		if cl.Z < -cl.W {
			out[4]++
		}
		if cl.Z > cl.W {
			out[5]++
		}
	}
	for _, v := range out {
		if v == 8 {
			return true
		}
	}
	return false
}

// counting shader: Vertex only projects, but records every invocation
type countingShader struct {
	*PhongShader
	mu        sync.Mutex
	calls     int
	positions []Vector
}

func newCountingShader(matrix Matrix, light, eye Vector) *countingShader {
	return &countingShader{PhongShader: NewPhongShader(matrix, light, eye)}
}

func (s *countingShader) Vertex(v Vertex) Vertex {
	s.mu.Lock()
	s.calls++
	s.positions = append(s.positions, v.Position)
	s.mu.Unlock()
	return s.PhongShader.Vertex(v)
}

func slopedHeights(size int) []float64 {
	return refHeights(size, func(x, z int) float64 {
		return 0.05*float64(x) - 0.03*float64(z) +
			2*math.Exp(-(float64((x-3)*(x-3)+(z-5)*(z-5)))/4)
	})
}

// ---------------------------------------------------------------------------
// TestTerrainSelection
// ---------------------------------------------------------------------------

func TestTerrainSelection(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// k=3: empty split table, single root leaf
		h9 := slopedHeights(9)
		terr9, err := NewTerrain(9, h9)
		c.Check(err == nil, "NewTerrain(9): %v", err)
		c.Check(terr9.Size() == 9, "size retained")
		c.Check(terr9.Revision() == 0, "revision starts at zero")

		// validation errors
		_, err = NewTerrain(8, h9)
		c.Check(err != nil, "size 8 rejected")
		_, err = NewTerrain(17, make([]float64, 16))
		c.Check(err != nil, "wrong height count rejected")
		bad := append([]float64(nil), h9...)
		bad[0] = math.NaN()
		_, err = NewTerrain(9, bad)
		c.Check(err != nil, "non-finite height rejected")
		_, err = PrepareTerrain(nil, TerrainView{})
		c.Check(err != nil, "empty instances rejected")

		view := TerrainView{Eye: Vector{4, 10, 4}, Matrix: Identity(), SplitDistances: nil}
		plan, err := PrepareTerrain([]TerrainInstance{{terr9, Vector{}, 1, true}}, view)
		c.Check(err == nil, "prepare k=3: %v", err)
		leaves := plan.Leaves()
		c.Check(len(leaves) == 1 && leaves[0].Span == 8 && leaves[0].Depth == 0,
			"k=3 selects the single root leaf, got %+v", leaves)

		// Leaves() returns independent copies
		leaves[0].X = 999
		again := plan.Leaves()
		c.Check(again[0].X == 0, "Leaves() copies are independent")

		// k=4: strict distance threshold against an independent reference
		size := 17
		k := 4
		h := slopedHeights(size)
		terr, err := NewTerrain(size, h)
		c.Check(err == nil, "NewTerrain(17): %v", err)
		origin := Vector{10, 0, -5}
		scale := 2.0
		rootBox := refNodeBox(h, size, k, refNode{0, 0, 0}, origin, scale)
		rootDist := refPointBoxDistance(Vector{10, 50, -5}, rootBox)
		c.Check(rootDist > 0, "reference root distance positive: %v", rootDist)

		eye := Vector{10, 50, -5}
		// threshold below distance: root retained (strict less-than)
		planFar, err := PrepareTerrain([]TerrainInstance{{terr, origin, scale, true}},
			TerrainView{Eye: eye, Matrix: Identity(), SplitDistances: []float64{rootDist * 0.5}})
		c.Check(err == nil, "far prepare: %v", err)
		c.Check(len(planFar.Leaves()) == 1 && planFar.Leaves()[0].Span == 16,
			"root retained when distance >= threshold")

		// threshold above distance: root subdivides into four 8-cell leaves
		planNear, err := PrepareTerrain([]TerrainInstance{{terr, origin, scale, true}},
			TerrainView{Eye: eye, Matrix: Identity(), SplitDistances: []float64{rootDist * 1.5}})
		c.Check(err == nil, "near prepare: %v", err)
		nearLeaves := planNear.Leaves()
		c.Check(len(nearLeaves) == 4, "root subdivided into 4 leaves, got %d", len(nearLeaves))
		for _, l := range nearLeaves {
			c.Check(l.Span == 8 && l.Depth == 1, "child leaves are depth-1 8-cell blocks")
		}

		// exact equality: strict comparison keeps the root
		planEq, err := PrepareTerrain([]TerrainInstance{{terr, origin, scale, true}},
			TerrainView{Eye: eye, Matrix: Identity(), SplitDistances: []float64{rootDist}})
		c.Check(err == nil, "equal prepare: %v", err)
		c.Check(len(planEq.Leaves()) == 1, "distance == threshold does not subdivide")

		// full reference comparison of the selected node set and stitches
		rv := TerrainView{
			Eye:            Vector{12, 3, -2},
			Matrix:         Identity(),
			SplitDistances: []float64{rootDist * 1.2},
		}
		refSet := refSelect(h, size, k, origin, scale, rv)
		planRef, err := PrepareTerrain([]TerrainInstance{{terr, origin, scale, true}}, rv)
		c.Check(err == nil, "ref prepare: %v", err)
		gotSet := map[refNode]struct{}{}
		for _, l := range planRef.Leaves() {
			gotSet[refNode{l.Depth, l.X / l.Span, l.Z / l.Span}] = struct{}{}
		}
		c.Check(reflect.DeepEqual(gotSet, refSet), "leaf set matches independent reference")
		for _, l := range planRef.Leaves() {
			n := refNode{l.Depth, l.X / l.Span, l.Z / l.Span}
			c.Check(l.Stitch == refStitch(h, size, k, refSet, n),
				"stitch flags match reference for %+v", n)
		}

		// 2:1 closure: no adjacent leaves differ by more than one level
		for n := range refSet {
			sp := 1 << (k - n.d)
			x0, z0 := n.qx*sp, n.qz*sp
			half := sp / 2
			check := func(x, z int) {
				nb := refSelect_find(refSet, k, x, z)
				nsp := 1 << (k - nb.d)
				c.Check(!(nsp > 2*sp || sp > 2*nsp), "2:1 closure violated at %+v", n)
			}
			if x0 > 0 {
				check(x0-1, z0+half)
			}
			if x0+sp < size-1 {
				check(x0+sp, z0+half)
			}
			if z0 > 0 {
				check(x0+half, z0-1)
			}
			if z0+sp < size-1 {
				check(x0+half, z0+sp)
			}
		}

		// two different placements of the same resource select independently
		// and never mutate the shared source data
		snapshot := append([]float64(nil), h...)
		instA := TerrainInstance{terr, Vector{0, 0, 0}, 1, true}
		instB := TerrainInstance{terr, Vector{100, 0, 100}, 1, true}
		planTwo, err := PrepareTerrain([]TerrainInstance{instA, instB},
			TerrainView{Eye: Vector{2, 2, 2}, Matrix: Identity(), SplitDistances: []float64{rootDist * 1.2}})
		c.Check(err == nil, "two-instance prepare: %v", err)
		counts := map[int]int{}
		for _, l := range planTwo.Leaves() {
			counts[l.Instance]++
		}
		c.Check(counts[0] > 0 && counts[1] > 0, "both placements produced leaves")
		c.Check(reflect.DeepEqual(h, snapshot), "source heights unchanged after prepare")
		c.Check(terr.Revision() == 0, "selection did not bump revision")

		// frustum culling via orthographic clip matrix and 8-corner rule
		ortho := Orthographic(-2, 2, -2, 2, 0.1, 50)
		inView := TerrainInstance{terr, Vector{-8, 0, -8}, 1, true}
		outView := TerrainInstance{terr, Vector{-80, 0, -80}, 1, true}
		planCull, err := PrepareTerrain([]TerrainInstance{inView, outView},
			TerrainView{Eye: Vector{0, 20, 0}, Matrix: ortho, SplitDistances: []float64{rootDist}})
		c.Check(err == nil, "cull prepare: %v", err)
		culledRef := map[int]bool{}
		for i, inst := range []TerrainInstance{inView, outView} {
			culledRef[i] = refCulled(h, size, k, refNode{0, 0, 0}, inst, ortho)
		}
		c.Check(culledRef[0] == false && culledRef[1] == true,
			"reference cull flags: %+v", culledRef)
		dcCull := newTestContext(32, 32, ortho)
		infoCull, err := DrawTerrain(planCull, dcCull)
		c.Check(err == nil, "draw cull plan: %v", err)
		// expected submission: every non-culled leaf of instance 0, and
		// nothing at all from the fully excluded instance 1
		var wantTris, wantVerts uint64
		for _, l := range planCull.Leaves() {
			n := refNode{l.Depth, l.X / l.Span, l.Z / l.Span}
			if l.Instance == 1 {
				c.Check(refCulled(h, size, k, n, outView, ortho), "all instance-1 leaves excluded")
				continue
			}
			if !refCulled(h, size, k, n, inView, ortho) {
				wantTris += uint64(len(refLeafIndices(l.Stitch)))
				wantVerts += 81
			}
		}
		c.Check(planCull.DrawStats().Triangles == wantTris && planCull.DrawStats().Vertices == wantVerts,
			"submission matches the 8-corner cull reference, stats=%+v want tris=%d",
			planCull.DrawStats(), wantTris)
		c.Check(infoCull.UpdatedPixels > 0, "visible placement actually drew")

		// invalid view parameters
		_, err = PrepareTerrain([]TerrainInstance{{terr, origin, scale, true}},
			TerrainView{Eye: eye, Matrix: Identity(), SplitDistances: []float64{2, 1}})
		c.Check(err != nil, "wrong split length rejected")
		// non-increasing acceptance needs k=5 (two split distances)
		terr33, err33 := NewTerrain(33, slopedHeights(33))
		c.Check(err33 == nil, "NewTerrain(33): %v", err33)
		_, err = PrepareTerrain([]TerrainInstance{{terr33, origin, scale, true}},
			TerrainView{Eye: eye, Matrix: Identity(), SplitDistances: []float64{2, 1}})
		c.Check(err == nil, "non-increasing splits accepted")
		_, err = PrepareTerrain([]TerrainInstance{{terr33, origin, scale, true}},
			TerrainView{Eye: eye, Matrix: Identity(), SplitDistances: []float64{1, 2}})
		c.Check(err != nil, "increasing splits rejected")
		_, err = PrepareTerrain([]TerrainInstance{{terr, origin, scale, true}},
			TerrainView{Eye: eye, Matrix: Identity(), SplitDistances: []float64{-1}})
		c.Check(err != nil, "non-positive split rejected")
		_, err = PrepareTerrain([]TerrainInstance{{terr, origin, 0, true}},
			TerrainView{Eye: eye, Matrix: Identity(), SplitDistances: []float64{1}})
		c.Check(err != nil, "zero scale rejected")
		_, err = PrepareTerrain([]TerrainInstance{{terr, origin, math.NaN(), true}},
			TerrainView{Eye: eye, Matrix: Identity(), SplitDistances: []float64{1}})
		c.Check(err != nil, "NaN scale rejected")
		terrOther, _ := NewTerrain(9, h9)
		_, err = PrepareTerrain(
			[]TerrainInstance{{terr, origin, scale, true}, {terrOther, Vector{}, 1, true}},
			TerrainView{Eye: eye, Matrix: Identity(), SplitDistances: []float64{1}})
		c.Check(err != nil, "mixed resource sizes rejected")
	})
}

func refSelect_find(chosen map[refNode]struct{}, k, x, z int) refNode {
	for d := 0; ; d++ {
		span := 1 << (k - d)
		qx, qz := x/span, z/span
		if m := (1 << d) - 1; qx > m {
			qx = m
		} else if qz > m {
			qz = m
		}
		n := refNode{d, qx, qz}
		if _, ok := chosen[n]; ok {
			return n
		}
	}
}

func newTestContext(w, h int, m Matrix) *Context {
	dc := NewContext(w, h)
	dc.Cull = CullNone
	dc.AlphaBlend = false
	dc.ClearColorBufferWith(Black)
	dc.Shader = NewPhongShader(m, Vector{-0.5, 1, -0.3}.Normalize(), Vector{20, 30, 20})
	return dc
}

// ---------------------------------------------------------------------------
// TestTerrainSeams
// ---------------------------------------------------------------------------

func TestTerrainSeams(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		size := 17
		k := 4
		h := slopedHeights(size)
		terr, err := NewTerrain(size, h)
		c.Check(err == nil, "NewTerrain: %v", err)

		// all 16 stitch templates: corners fixed, odds collapsed, full coverage
		corners := map[int]bool{0: true, 8: true, 9 * 8: true, 9*9 - 1: true}
		for stitch := uint8(0); stitch < 16; stitch++ {
			got := terrainIndexTemplates[stitch]
			ref := refLeafIndices(stitch)
			c.Check(reflect.DeepEqual(got, ref), "template %d matches reference", stitch)
			used := map[int]bool{}
			for _, t3 := range got {
				for _, vi := range t3 {
					c.Check(vi >= 0 && vi < 81, "template %d index in range", stitch)
					used[vi] = true
				}
			}
			for ci := range corners {
				c.Check(used[ci], "template %d keeps corner %d", stitch, ci)
			}
			// the block footprint is covered exactly once. Stitched triangles
			// spill across cell borders, so each generic point is tested
			// against every triangle of the block. Offsets avoid all grid and
			// diagonal edge lines through integer grid points.
			counts := make([]int, 8*8*2)
			sample := func(i, j, s int) (float64, float64) {
				if s == 0 {
					return float64(i) + 0.1731, float64(j) + 0.4193
				}
				return float64(i) + 0.6817, float64(j) + 0.8913
			}
			for _, t3 := range got {
				a := t3[0]
				ax, az := float64(a%9), float64(a/9)
				bx, bz := float64(t3[1]%9), float64(t3[1]/9)
				cx, cz := float64(t3[2]%9), float64(t3[2]/9)
				for j := 0; j < 8; j++ {
					for i := 0; i < 8; i++ {
						for s := 0; s < 2; s++ {
							px, pz := sample(i, j, s)
							if pointInTriangle2D(px, pz, ax, az, bx, bz, cx, cz) {
								counts[(j*8+i)*2+s]++
							}
						}
					}
				}
			}
			for i, n := range counts {
				c.Check(n == 1, "template %d sample %d covered once, got %d", stitch, i, n)
			}
		}
		// templates are shared package-level topology, not per-block copies
		a := terrainIndexTemplates[3]
		b := terrainIndexTemplates[3]
		c.Check(reflect.ValueOf(a).Pointer() == reflect.ValueOf(b).Pointer(),
			"stitch topology is shared")

		// mixed-LOD plan over the sloped + bumpy field (k=5: three depths)
		size = 33
		k = 5
		h = slopedHeights(size)
		terr, err = NewTerrain(size, h)
		c.Check(err == nil, "NewTerrain(33): %v", err)
		var origin Vector
		var scale float64
		scale = 1.0
		var eye Vector
		eye = Vector{1, 8, 1}
		// pick d1 between the nearest and farthest depth-1 children so the
		// root splits while some children are retained
		var childMin, childMax float64
		childMin = math.Inf(1)
		childMax = math.Inf(-1)
		for qz := 0; qz < 2; qz++ {
			for qx := 0; qx < 2; qx++ {
				cb := refNodeBox(h, size, k, refNode{1, qx, qz}, origin, scale)
				d := refPointBoxDistance(eye, cb)
				if d < childMin {
					childMin = d
				}
				if d > childMax {
					childMax = d
				}
			}
		}
		c.Check(childMin < (childMin+childMax)/2 && (childMin+childMax)/2 < childMax,
			"reference child distances straddle the middle threshold")
		var view TerrainView
		// d0 above every child distance forces the root to split while the
		// non-increasing table still holds (d0 > d1); d1 straddles children
		view = TerrainView{
			Eye:    eye,
			Matrix: Identity(),
			SplitDistances: []float64{
				childMax + 1,
				(childMin + childMax) / 2,
			},
		}
		chosen := refSelect(h, size, k, origin, scale, view)
		c.Check(len(chosen) > 1, "reference produced mixed LODs: %d", len(chosen))
		depths := map[int]bool{}
		for n := range chosen {
			depths[n.d] = true
		}
		c.Check(len(depths) > 1, "more than one LOD active: %+v", depths)

		plan, err := PrepareTerrain([]TerrainInstance{{terr, origin, scale, true}}, view)
		c.Check(err == nil, "mixed prepare: %v", err)
		if plan == nil {
			return
		}

		// whole-surface coverage: generic points in every full-resolution cell
		// lie in exactly one positive-area triangle => no holes, no duplicates
		cover := make([]int, (size-1)*(size-1)*2)
		normalAtSample := map[[2]int]Vector{}
		uvAtSample := map[[2]int]Vector{}
		var totalTris uint64
		for _, l := range plan.Leaves() {
			idx := terrainIndexTemplates[l.Stitch]
			totalTris += uint64(len(idx))
			vert := func(vi int) (float64, float64, int, int) {
				i := vi % 9
				j := vi / 9
				sx := l.X + i*l.Span/8
				sz := l.Z + j*l.Span/8
				return float64(sx), float64(sz), sx, sz
			}
			for _, t3 := range idx {
				ax, az, asx, asz := vert(t3[0])
				bx, bz, bsx, bsz := vert(t3[1])
				cx, cz, csx, csz := vert(t3[2])
				for _, key := range [][2]int{{asx, asz}, {bsx, bsz}, {csx, csz}} {
					n := terr.normals[key[1]*size+key[0]]
					if prev, ok := normalAtSample[key]; ok {
						c.Check(prev == n, "shared sample %v normal equal across blocks", key)
					} else {
						normalAtSample[key] = n
					}
					uv := Vector{float64(key[0]) / float64(size-1), float64(key[1]) / float64(size-1), 0}
					if prev, ok := uvAtSample[key]; ok {
						c.Check(prev == uv, "shared sample %v UV equal across blocks", key)
					} else {
						uvAtSample[key] = uv
					}
				}
				for j := 0; j < size-1; j++ {
					for i := 0; i < size-1; i++ {
						for s := 0; s < 2; s++ {
							var px, pz float64
							if s == 0 {
								px, pz = float64(i)+0.1731, float64(j)+0.4193
							} else {
								px, pz = float64(i)+0.6817, float64(j)+0.8913
							}
							if pointInTriangle2D(px, pz, ax, az, bx, bz, cx, cz) {
								cover[(j*(size-1)+i)*2+s]++
							}
						}
					}
				}
			}
		}
		holes, dups := 0, 0
		for i, n := range cover {
			if n == 0 {
				holes++
			} else if n > 1 {
				dups++
			}
			_ = i
		}
		c.Check(holes == 0 && dups == 0, "surface watertight: holes=%d duplicates=%d", holes, dups)
		c.Check(totalTris > 0, "plan has triangles")

		// normals agree with the independent finite-difference reference
		for key, n := range normalAtSample {
			c.Check(n == refNormalAt(h, size, key[0], key[1]),
				"normal at %v matches center-difference reference", key)
		}

		// real draw crossing the near plane, compared against an independently
		// constructed explicit-mesh reference through the legacy DrawMesh path
		cam := Perspective(50, 1, 0.5, 100)
		nearEye := Vector{3, 0.6, 3.2}
		nearView := TerrainView{
			Eye:            nearEye,
			Matrix:         cam.Mul(LookAt(nearEye, Vector{6, 0, 8}, Vector{0, 1, 0})),
			SplitDistances: view.SplitDistances,
		}
		nearPlan, err := PrepareTerrain([]TerrainInstance{{terr, origin, scale, true}}, nearView)
		c.Check(err == nil, "near prepare: %v", err)
		c.Check(len(nearPlan.Leaves()) > 0, "near plan has leaves")

		light := Vector{-0.5, 1, -0.3}.Normalize()
		dcIndexed := NewContext(48, 48)
		dcIndexed.Cull = CullNone
		dcIndexed.AlphaBlend = false
		dcIndexed.ClearColorBufferWith(Black)
		dcIndexed.Shader = NewPhongShader(nearView.Matrix, light, nearEye)
		infoIndexed, err := DrawTerrain(nearPlan, dcIndexed)
		c.Check(err == nil, "indexed draw: %v", err)
		c.Check(infoIndexed.UpdatedPixels > 0, "indexed path produced pixels")

		refMesh := buildReferenceMesh(h, size, k, nearPlan, TerrainInstance{terr, origin, scale, true}, chosen)
		dcMesh := NewContext(48, 48)
		dcMesh.Cull = CullNone
		dcMesh.AlphaBlend = false
		dcMesh.ClearColorBufferWith(Black)
		dcMesh.Shader = NewPhongShader(nearView.Matrix, light, nearEye)
		infoMesh := dcMesh.DrawMesh(refMesh)
		c.Check(infoMesh.UpdatedPixels > 0, "reference mesh produced pixels")

		colorDiff, depthDiff := compareBuffers(dcIndexed, dcMesh)
		c.Check(colorDiff == 0, "indexed vs DrawMesh colors identical, diffs=%d", colorDiff)
		c.Check(depthDiff == 0, "indexed vs DrawMesh depth identical, diffs=%d", depthDiff)

		// counting shader: indexed reuse, one shader call per referenced vertex
		countShader := newCountingShader(nearView.Matrix, light, nearEye)
		dcCount := NewContext(24, 24)
		dcCount.Cull = CullNone
		dcCount.AlphaBlend = false
		dcCount.Shader = countShader
		visibleLeaves := 0
		var expectedCalls int
		visiblePositions := map[Vector]bool{}
		for _, l := range nearPlan.Leaves() {
			if refCulled(h, size, k, refNode{l.Depth, l.X / l.Span, l.Z / l.Span},
				TerrainInstance{terr, origin, scale, true}, nearView.Matrix) {
				continue
			}
			visibleLeaves++
			used := map[int]bool{}
			for _, t3 := range terrainIndexTemplates[l.Stitch] {
				for _, vi := range t3 {
					used[vi] = true
				}
			}
			expectedCalls += len(used)
			for vi := range used {
				i, j := vi%9, vi/9
				sx := l.X + i*l.Span/8
				sz := l.Z + j*l.Span/8
				visiblePositions[Vector{float64(sx), h[sz*size+sx], float64(sz)}] = true
			}
		}
		_, err = DrawTerrain(nearPlan, dcCount)
		c.Check(err == nil, "counting draw: %v", err)
		c.Check(countShader.calls == expectedCalls,
			"shader calls %d == referenced vertices %d across %d leaves",
			countShader.calls, expectedCalls, visibleLeaves)
		for _, p := range countShader.positions {
			c.Check(visiblePositions[p], "submitted vertex belongs to a visible block")
		}
		c.Check(len(countShader.positions) == expectedCalls, "no invisible block submitted")

		// horizontal edges and degenerate faces must terminate without drawing
		dcDeg := NewContext(16, 16)
		dcDeg.Cull = CullNone
		dcDeg.Shader = NewSolidColorShader(Identity(), White)
		flatVerts := []Vertex{
			{Position: Vector{0, 0, 0}, Color: White},
			{Position: Vector{1, 0, 0}, Color: White},
			{Position: Vector{0, 0, 0}, Color: White}, // repeated -> zero area
			{Position: Vector{1, 0, 1}, Color: White},
		}
		infoDeg, err := dcDeg.DrawIndexed(flatVerts, [][3]int{{0, 1, 2}, {0, 1, 3}})
		c.Check(err == nil, "degenerate draw accepted: %v", err)
		c.Check(infoDeg.TotalPixels >= 0, "degenerate path terminates")

		// DrawIndexed validation happens before any shader call or buffer write
		badShader := newCountingShader(Identity(), light, nearEye)
		dcBad := NewContext(8, 8)
		dcBad.Shader = badShader
		goodVerts := []Vertex{
			{Position: Vector{0, 0, 0}, Color: White},
			{Position: Vector{1, 0, 0}, Color: White},
			{Position: Vector{0, 1, 0}, Color: White},
		}
		_, err = dcBad.DrawIndexed(goodVerts, [][3]int{{0, 1, 9}})
		c.Check(err != nil, "out-of-range index rejected")
		nanVerts := append([]Vertex(nil), goodVerts...)
		nanVerts[2].Normal = Vector{math.NaN(), 0, 0}
		_, err = dcBad.DrawIndexed(nanVerts, [][3]int{{0, 1, 2}})
		c.Check(err != nil, "non-finite vertex rejected")
		c.Check(badShader.calls == 0, "validation precedes shader calls")
		infoEmpty, err := dcBad.DrawIndexed(goodVerts, nil)
		c.Check(err == nil && infoEmpty == (RasterizeInfo{}), "empty indices succeed without drawing")

		// unreferenced vertices never reach the shader
		refShader := newCountingShader(Identity(), light, nearEye)
		dcRef := NewContext(8, 8)
		dcRef.Shader = refShader
		extra := append([]Vertex(nil), goodVerts...)
		extra = append(extra, Vertex{Position: Vector{9, 9, 9}, Color: White})
		_, err = dcRef.DrawIndexed(extra, [][3]int{{0, 1, 2}})
		c.Check(err == nil, "draw with unreferenced vertex: %v", err)
		c.Check(refShader.calls == 3, "only three referenced vertices shaded")
	})
}

func pointInTriangle2D(px, pz, ax, az, bx, bz, cx, cz float64) bool {
	sign := func(px, pz, ax, az, bx, bz float64) float64 {
		return (px-bx)*(az-bz) - (ax-bx)*(pz-bz)
	}
	d1 := sign(px, pz, ax, az, bx, bz)
	d2 := sign(px, pz, bx, bz, cx, cz)
	d3 := sign(px, pz, cx, cz, ax, az)
	hasNeg := d1 < 0 || d2 < 0 || d3 < 0
	hasPos := d1 > 0 || d2 > 0 || d3 > 0
	return !(hasNeg && hasPos)
}

func buildReferenceMesh(h []float64, size, k int, plan *TerrainPlan, inst TerrainInstance, chosen map[refNode]struct{}) *Mesh {
	var tris []*Triangle
	for _, l := range plan.Leaves() {
		if refCulled(h, size, k, refNode{l.Depth, l.X / l.Span, l.Z / l.Span}, inst, plan.view.Matrix) {
			continue
		}
		verts := make([]Vertex, 81)
		for j := 0; j <= 8; j++ {
			for i := 0; i <= 8; i++ {
				sx := l.X + i*l.Span/8
				sz := l.Z + j*l.Span/8
				local := Vector{float64(sx), h[sz*size+sx], float64(sz)}
				verts[j*9+i] = Vertex{
					Position: inst.Origin.Add(local.MulScalar(inst.Scale)),
					Normal:   refNormalAt(h, size, sx, sz),
					Texture:  Vector{float64(sx) / float64(size-1), float64(sz) / float64(size-1), 0},
					Color:    White,
				}
			}
		}
		for _, t3 := range refLeafIndices(l.Stitch) {
			tris = append(tris, NewTriangle(verts[t3[0]], verts[t3[1]], verts[t3[2]]))
		}
	}
	return NewTriangleMesh(tris)
}

func compareBuffers(a, b *Context) (colorDiff, depthDiff int) {
	for i := range a.ColorBuffer.Pix {
		if a.ColorBuffer.Pix[i] != b.ColorBuffer.Pix[i] {
			colorDiff++
		}
	}
	for i := range a.DepthBuffer {
		if a.DepthBuffer[i] != b.DepthBuffer[i] {
			depthDiff++
		}
	}
	return
}

// ---------------------------------------------------------------------------
// TestTerrainUpdates
// ---------------------------------------------------------------------------

func TestTerrainUpdates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// 257x257 field, single point edit, exact independently counted work
		const size = 257
		const k = 8
		h := refHeights(size, func(x, z int) float64 {
			return math.Sin(float64(x)*0.1) + math.Cos(float64(z)*0.07)
		})
		terr, err := NewTerrain(size, h)
		c.Check(err == nil, "NewTerrain(257): %v", err)
		c.Check(terr.LastUpdateStats() == (HeightUpdateStats{}), "construction does not count")

		ex, ez := 100, 100
		// independent expectation
		wantNormals := [][2]int{{ex, ez}, {ex - 1, ez}, {ex + 1, ez}, {ex, ez - 1}, {ex, ez + 1}}
		leafDepth := k - 3
		wantLeaves := map[[2]int]struct{}{}
		for _, qz := range []int{ez / 8} {
			for _, qx := range []int{ex / 8} {
				wantLeaves[[2]int{qx, qz}] = struct{}{}
			}
		}
		wantNodes := map[refNode]struct{}{}
		for q := range wantLeaves {
			qx, qz := q[0], q[1]
			for d := leafDepth; d >= 0; d-- {
				wantNodes[refNode{d, qx, qz}] = struct{}{}
				qx, qz = qx/2, qz/2
			}
		}
		wantBounds := len(wantNodes)
		wantReads := 81*len(wantLeaves) + 4*len(wantNormals)

		err = terr.UpdateHeights([]HeightEdit{{ex, ez, h[ez*size+ex] + 3}})
		c.Check(err == nil, "single edit: %v", err)
		c.Check(terr.Revision() == 1, "revision incremented")
		st := terr.LastUpdateStats()
		c.Check(st.NormalUpdates == uint64(len(wantNormals)), "normal updates %d want %d", st.NormalUpdates, len(wantNormals))
		c.Check(st.BoundsUpdates == uint64(wantBounds), "bounds updates %d want %d", st.BoundsUpdates, wantBounds)
		c.Check(st.HeightReads == uint64(wantReads), "height reads %d want %d", st.HeightReads, wantReads)
		c.Check(terr.heights[ez*size+ex] == h[ez*size+ex]+3, "height applied")

		// node bounds really were refreshed: walk the affected ancestor chain
		for n := range wantNodes {
			span := 1 << (k - n.d)
			x0, z0 := n.qx*span, n.qz*span
			minY, maxY := math.Inf(1), math.Inf(-1)
			for z := z0; z <= z0+span; z++ {
				for x := x0; x <= x0+span; x++ {
					v := terr.heights[z*size+x]
					if v < minY {
						minY = v
					}
					if v > maxY {
						maxY = v
					}
				}
			}
			idx := terr.nodeIndex(n.d, n.qx, n.qz)
			c.Check(terr.nodeMinY[idx] == minY && terr.nodeMaxY[idx] == maxY,
				"node %+v bounds refreshed", n)
		}
		// refreshed normals match the independent difference formula
		for _, key := range wantNormals {
			c.Check(terr.normals[key[1]*size+key[0]] == refNormalAt(terr.heights, size, key[0], key[1]),
				"normal refreshed at %v", key)
		}

		// invalid batch changes nothing at all
		beforeRev := terr.Revision()
		beforeStats := terr.LastUpdateStats()
		beforeH := append([]float64(nil), terr.heights...)
		beforeN := append([]Vector(nil), terr.normals...)
		err = terr.UpdateHeights([]HeightEdit{{0, 0, 1}, {size, 0, 2}})
		c.Check(err != nil, "out-of-range edit rejected")
		err = terr.UpdateHeights([]HeightEdit{{1, 1, math.Inf(1)}})
		c.Check(err != nil, "infinite height rejected")
		c.Check(terr.Revision() == beforeRev, "failed batch keeps revision")
		c.Check(terr.LastUpdateStats() == beforeStats, "failed batch keeps stats")
		c.Check(reflect.DeepEqual(terr.heights, beforeH), "failed batch keeps heights")
		c.Check(reflect.DeepEqual(terr.normals, beforeN), "failed batch keeps normals")

		// duplicate coordinates: last value wins
		err = terr.UpdateHeights([]HeightEdit{{5, 6, 1}, {5, 6, 7.5}})
		c.Check(err == nil, "duplicate edit: %v", err)
		c.Check(terr.heights[6*size+5] == 7.5, "last edit wins")
		c.Check(terr.Revision() == beforeRev+1, "one revision for the batch")

		// empty edit: no work, no invalidation, zero reported stats
		err = terr.UpdateHeights(nil)
		c.Check(err == nil, "empty edit accepted")
		c.Check(terr.Revision() == beforeRev+1, "empty edit does not bump revision")
		c.Check(terr.LastUpdateStats() == (HeightUpdateStats{}), "empty edit reports zero work")

		// corner edit (0,0): three normal points, one leaf, full ancestor chain
		err = terr.UpdateHeights([]HeightEdit{{0, 0, 4}})
		c.Check(err == nil, "corner edit: %v", err)
		st = terr.LastUpdateStats()
		cornerNodes := leafDepth + 1
		c.Check(st.BoundsUpdates == uint64(cornerNodes), "corner bounds %d want %d", st.BoundsUpdates, cornerNodes)
		c.Check(st.NormalUpdates == 3, "corner normal updates 3, got %d", st.NormalUpdates)
		c.Check(st.HeightReads == uint64(81+12), "corner reads exact, got %d", st.HeightReads)
		for _, key := range [][2]int{{0, 0}, {1, 0}, {0, 1}} {
			c.Check(terr.normals[key[1]*size+key[0]] == refNormalAt(terr.heights, size, key[0], key[1]),
				"one-sided boundary normal at %v", key)
		}

		// far-edge corner edit (size-1,size-1): clamped onto the final leaf
		// and shared with the three neighboring leaves
		err = terr.UpdateHeights([]HeightEdit{{size - 1, size - 1, -4}})
		c.Check(err == nil, "far-corner edit: %v", err)
		st = terr.LastUpdateStats()
		farLeaves := 4
		// depth leafDepth: 4 leaves; their parents already coincide, so the
		// remaining leafDepth levels form a single ancestor chain
		farNodes := farLeaves + leafDepth
		c.Check(st.BoundsUpdates == uint64(farNodes), "far-corner bounds %d want %d", st.BoundsUpdates, farNodes)
		c.Check(st.NormalUpdates == 3, "far-corner normal updates 3, got %d", st.NormalUpdates)
		c.Check(st.HeightReads == uint64(81*farLeaves+12), "far-corner reads exact, got %d", st.HeightReads)
		lastLeaf := (1 << leafDepth) - 1
		idx := terr.nodeIndex(leafDepth, lastLeaf, lastLeaf)
		c.Check(terr.nodeMinY[idx] <= -4 && terr.nodeMaxY[idx] >= -4,
			"final leaf bounds include the edited height")

		// edit on a block seam touches four leaves; check the counted set
		sx, sz := 96, 96
		seamLeaves := map[[2]int]struct{}{}
		for _, dz := range []int{0, -1} {
			for _, dx := range []int{0, -1} {
				seamLeaves[[2]int{sx/8 + dx, sz/8 + dz}] = struct{}{}
			}
		}
		seamNodes := map[refNode]struct{}{}
		for q := range seamLeaves {
			qx, qz := q[0], q[1]
			for d := leafDepth; d >= 0; d-- {
				seamNodes[refNode{d, qx, qz}] = struct{}{}
				qx, qz = qx/2, qz/2
			}
		}
		seamNormalPoints := map[[2]int]struct{}{}
		for _, p := range [][2]int{{sx, sz}, {sx - 1, sz}, {sx + 1, sz}, {sx, sz - 1}, {sx, sz + 1}} {
			seamNormalPoints[p] = struct{}{}
		}
		err = terr.UpdateHeights([]HeightEdit{{sx, sz, terr.heights[sz*size+sx] - 2.5}})
		c.Check(err == nil, "seam edit: %v", err)
		st = terr.LastUpdateStats()
		c.Check(st.BoundsUpdates == uint64(len(seamNodes)),
			"seam bounds updates %d want %d", st.BoundsUpdates, len(seamNodes))
		c.Check(st.NormalUpdates == uint64(len(seamNormalPoints)),
			"seam normal updates %d want %d", st.NormalUpdates, len(seamNormalPoints))
		c.Check(st.HeightReads == uint64(81*len(seamLeaves)+4*len(seamNormalPoints)),
			"seam height reads exact")

		// shared resource: two placements both see new heights after re-prepare
		splits := make([]float64, k-3)
		for i := range splits {
			splits[i] = 4
		}
		view := TerrainView{
			Eye: Vector{128, 60, 200},
			Matrix: Perspective(50, 1, 1, 500).
				Mul(LookAt(Vector{128, 60, 200}, Vector{128, 0, 100}, Vector{0, 1, 0})),
			SplitDistances: splits,
		}
		instA := TerrainInstance{terr, Vector{0, 0, 0}, 1, true}
		instB := TerrainInstance{terr, Vector{400, 0, 0}, 1, true}
		plan, err := PrepareTerrain([]TerrainInstance{instA, instB}, view)
		c.Check(err == nil, "shared prepare: %v", err)

		// empty edit and an invalid batch neither invalidate the plan nor
		// block subsequent drawing of the still-valid resource
		okDc := NewContext(24, 24)
		okDc.Cull = CullNone
		okDc.Shader = NewPhongShader(view.Matrix, Vector{0, 1, 0}, view.Eye)
		c.Check(terr.UpdateHeights(nil) == nil, "empty edit accepted")
		_, err = DrawTerrain(plan, okDc)
		c.Check(err == nil, "empty edit does not invalidate the plan")
		c.Check(terr.UpdateHeights([]HeightEdit{{-1, 0, 1}}) != nil, "invalid batch rejected")
		_, err = DrawTerrain(plan, okDc)
		c.Check(err == nil, "failed batch leaves the plan drawable")

		// stale plan fails before any shader/buffer write
		staleDc := NewContext(24, 24)
		staleDc.Cull = CullNone
		staleDc.Shader = NewPhongShader(view.Matrix, Vector{0, 1, 0}, view.Eye)
		colorBefore := append([]byte(nil), staleDc.ColorBuffer.Pix...)
		depthBefore := append([]float64(nil), staleDc.DepthBuffer...)
		statsBeforeFail := plan.DrawStats()
		c.Check(statsBeforeFail.Triangles > 0, "a successful draw recorded stats")
		err = terr.UpdateHeights([]HeightEdit{{200, 200, 5}})
		c.Check(err == nil, "invalidating edit: %v", err)
		_, err = DrawTerrain(plan, staleDc)
		c.Check(err != nil, "stale plan rejected")
		c.Check(reflect.DeepEqual(staleDc.ColorBuffer.Pix, colorBefore), "stale draw wrote no color")
		c.Check(reflect.DeepEqual(staleDc.DepthBuffer, depthBefore), "stale draw wrote no depth")
		c.Check(plan.DrawStats() == statsBeforeFail, "failed draw keeps the previous stats")

		// re-prepare: both placements render with the edited surface, and the
		// image matches an independently built DrawMesh reference
		plan2, err := PrepareTerrain([]TerrainInstance{instA, instB}, view)
		c.Check(err == nil, "re-prepare: %v", err)
		light := Vector{-0.4, 1, -0.2}.Normalize()
		dc1 := NewContext(40, 40)
		dc1.Cull = CullNone
		dc1.AlphaBlend = false
		dc1.ClearColorBufferWith(Black)
		dc1.Shader = NewPhongShader(view.Matrix, light, view.Eye)
		info1, err := DrawTerrain(plan2, dc1)
		c.Check(err == nil, "fresh draw: %v", err)
		c.Check(info1.UpdatedPixels > 0, "fresh draw painted")
		c.Check(plan2.DrawStats().Triangles > 0 && plan2.DrawStats().Vertices > 0,
			"draw stats recorded: %+v", plan2.DrawStats())

		refMesh := NewEmptyMesh()
		for ii, inst := range []TerrainInstance{instA, instB} {
			for _, l := range plan2.Leaves() {
				if l.Instance != ii {
					continue
				}
				if refCulled(terr.heights, size, k, refNode{l.Depth, l.X / l.Span, l.Z / l.Span}, inst, view.Matrix) {
					continue
				}
				verts := make([]Vertex, 81)
				for j := 0; j <= 8; j++ {
					for i := 0; i <= 8; i++ {
						sx2 := l.X + i*l.Span/8
						sz2 := l.Z + j*l.Span/8
						local := Vector{float64(sx2), terr.heights[sz2*size+sx2], float64(sz2)}
						verts[j*9+i] = Vertex{
							Position: inst.Origin.Add(local.MulScalar(inst.Scale)),
							Normal:   refNormalAt(terr.heights, size, sx2, sz2),
							Texture:  Vector{float64(sx2) / (size - 1), float64(sz2) / (size - 1), 0},
							Color:    White,
						}
					}
				}
				for _, t3 := range refLeafIndices(l.Stitch) {
					refMesh.Triangles = append(refMesh.Triangles,
						NewTriangle(verts[t3[0]], verts[t3[1]], verts[t3[2]]))
				}
			}
		}
		dc2 := NewContext(40, 40)
		dc2.Cull = CullNone
		dc2.AlphaBlend = false
		dc2.ClearColorBufferWith(Black)
		dc2.Shader = NewPhongShader(view.Matrix, light, view.Eye)
		dc2.DrawMesh(refMesh)
		colorDiff, depthDiff := compareBuffers(dc1, dc2)
		c.Check(colorDiff == 0 && depthDiff == 0,
			"edited shared terrain matches DrawMesh reference (diffs c=%d d=%d)", colorDiff, depthDiff)

		// edited bump really changed the Phong image versus the pre-edit field
		preHeights := refHeights(size, func(x, z int) float64 {
			return math.Sin(float64(x)*0.1) + math.Cos(float64(z)*0.07)
		})
		terrPre, err := NewTerrain(size, preHeights)
		c.Check(err == nil, "pristine terrain: %v", err)
		planPre, err := PrepareTerrain([]TerrainInstance{instA, instB}, view)
		_ = planPre
		// instA/instB point at the edited terrain; build matching instances
		preA := TerrainInstance{terrPre, Vector{0, 0, 0}, 1, true}
		preB := TerrainInstance{terrPre, Vector{400, 0, 0}, 1, true}
		planPre, err = PrepareTerrain([]TerrainInstance{preA, preB}, view)
		c.Check(err == nil, "pristine prepare: %v", err)
		dcPre := NewContext(40, 40)
		dcPre.Cull = CullNone
		dcPre.AlphaBlend = false
		dcPre.ClearColorBufferWith(Black)
		dcPre.Shader = NewPhongShader(view.Matrix, light, view.Eye)
		DrawTerrain(planPre, dcPre)
		cd, _ := compareBuffers(dc1, dcPre)
		c.Check(cd > 0, "editing changed the rendered Phong image, diffs=%d", cd)

		// sorted leaves are deterministic-looking for diagnostics
		var ls []int
		for _, l := range plan2.Leaves() {
			ls = append(ls, l.Depth)
		}
		sort.Ints(ls)
		c.Check(len(ls) > 0, "plan2 has leaves: %s", fmt.Sprint(ls[:min(3, len(ls))]))
	})
}
