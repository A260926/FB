package fauxgl

import (
	"fmt"
	"math"
	"sync"
)

// terrainLeafCells is the fixed number of grid cells on a finest-level leaf
// block. Every leaf spans a multiple of this many samples.
const terrainLeafCells = 8

// HeightEdit requests a new height for a single source sample.
type HeightEdit struct {
	X      int
	Z      int
	Height float64
}

// HeightUpdateStats reports the work performed by the last Terrain update.
type HeightUpdateStats struct {
	HeightReads   uint64
	NormalUpdates uint64
	BoundsUpdates uint64
}

// Terrain is a shared, square height field. The source sampling grid has
// 2^k+1 samples per side (k >= 3), unit sample spacing, and local positions
// (x, height, z). A quadtree of axis-aligned nodes subdivides down to leaf
// blocks of 8x8 cells. Many TerrainInstance values may share one Terrain.
type Terrain struct {
	size    int
	k       int
	heights []float64
	normals []Vector

	nodeBase []int // nodeBase[d] is the first node index at depth d
	nodeMinY []float64
	nodeMaxY []float64

	revision uint64
	stats    HeightUpdateStats

	mu sync.RWMutex
}

// NewTerrain validates and takes ownership of a copy of the height array.
// size must be 2^k+1 with k >= 3 and len(heights) must be size*size finite
// samples in row-major order (z*size + x).
func NewTerrain(size int, heights []float64) (*Terrain, error) {
	span := size - 1
	if span < terrainLeafCells || span&(span-1) != 0 {
		return nil, fmt.Errorf("NewTerrain: size must be 2^k+1 with k >= 3, got %d", size)
	}
	if len(heights) != size*size {
		return nil, fmt.Errorf("NewTerrain: got %d heights for size %d, want %d",
			len(heights), size, size*size)
	}
	for i, h := range heights {
		if math.IsNaN(h) || math.IsInf(h, 0) {
			return nil, fmt.Errorf("NewTerrain: height %d is not finite", i)
		}
	}
	k := 0
	for s := span; s > 1; s >>= 1 {
		k++
	}
	t := &Terrain{
		size:    size,
		k:       k,
		heights: append([]float64(nil), heights...),
		normals: make([]Vector, size*size),
	}
	depthCount := k - 3 + 1
	t.nodeBase = make([]int, depthCount+1)
	for d := 0; d <= depthCount; d++ {
		t.nodeBase[d] = (pow4(d) - 1) / 3
	}
	nodeCount := t.nodeBase[depthCount]
	t.nodeMinY = make([]float64, nodeCount)
	t.nodeMaxY = make([]float64, nodeCount)
	t.rebuildNormal(0, 0)
	for z := 0; z < size; z++ {
		for x := 0; x < size; x++ {
			if x == 0 && z == 0 {
				continue
			}
			t.rebuildNormal(x, z)
		}
	}
	t.buildAllBounds()
	return t, nil
}

// Size returns the side length of the source sampling grid.
func (t *Terrain) Size() int { return t.size }

// Revision starts at zero and increments once per effective, non-empty
// UpdateHeights batch.
func (t *Terrain) Revision() uint64 {
	t.mu.RLock()
	defer t.mu.RUnlock()
	return t.revision
}

// LastUpdateStats returns the work performed by the most recent effective
// update. Construction work is not counted; an empty or invalid batch leaves
// the counters at zero (an invalid batch changes nothing at all).
func (t *Terrain) LastUpdateStats() HeightUpdateStats {
	t.mu.RLock()
	defer t.mu.RUnlock()
	return t.stats
}

func (t *Terrain) heightAt(x, z int) float64 {
	return t.heights[z*t.size+x]
}

func pow4(n int) int {
	v := 1
	for i := 0; i < n; i++ {
		v *= 4
	}
	return v
}

func (t *Terrain) nodeIndex(d, qx, qz int) int {
	n := 1 << d
	return t.nodeBase[d] + qz*n + qx
}

// rebuildNormal recomputes the unit normal of one source sample from the
// finest-grid finite differences. Boundary samples use one-sided differences.
// Exactly four source heights are read.
func (t *Terrain) rebuildNormal(x, z int) {
	size := t.size
	var dhdx, dhdz float64
	if x == 0 {
		dhdx = t.heightAt(1, z) - t.heightAt(0, z)
	} else if x == size-1 {
		dhdx = t.heightAt(size-1, z) - t.heightAt(size-2, z)
	} else {
		dhdx = (t.heightAt(x+1, z) - t.heightAt(x-1, z)) / 2
	}
	if z == 0 {
		dhdz = t.heightAt(x, 1) - t.heightAt(x, 0)
	} else if z == size-1 {
		dhdz = t.heightAt(x, size-1) - t.heightAt(x, size-2)
	} else {
		dhdz = (t.heightAt(x, z+1) - t.heightAt(x, z-1)) / 2
	}
	t.normals[z*size+x] = Vector{-dhdx, 1, -dhdz}.Normalize()
}

func (t *Terrain) buildAllBounds() {
	leafDepth := t.k - 3
	leafSpan := terrainLeafCells
	leafNodes := 1 << leafDepth
	for qz := 0; qz < leafNodes; qz++ {
		for qx := 0; qx < leafNodes; qx++ {
			x0 := qx * leafSpan
			z0 := qz * leafSpan
			minY := math.Inf(1)
			maxY := math.Inf(-1)
			for lz := 0; lz <= leafSpan; lz++ {
				for lx := 0; lx <= leafSpan; lx++ {
					h := t.heightAt(x0+lx, z0+lz)
					if h < minY {
						minY = h
					}
					if h > maxY {
						maxY = h
					}
				}
			}
			idx := t.nodeIndex(leafDepth, qx, qz)
			t.nodeMinY[idx] = minY
			t.nodeMaxY[idx] = maxY
		}
	}
	for d := leafDepth - 1; d >= 0; d-- {
		nodes := 1 << d
		for qz := 0; qz < nodes; qz++ {
			for qx := 0; qx < nodes; qx++ {
				idx := t.nodeIndex(d, qx, qz)
				child := t.nodeBase[d+1] + (2*qz)*(nodes*2) + 2*qx
				minY := math.Inf(1)
				maxY := math.Inf(-1)
				for cz := 0; cz < 2; cz++ {
					for cx := 0; cx < 2; cx++ {
						c := child + cz*(nodes*2) + cx
						if t.nodeMinY[c] < minY {
							minY = t.nodeMinY[c]
						}
						if t.nodeMaxY[c] > maxY {
							maxY = t.nodeMaxY[c]
						}
					}
				}
				t.nodeMinY[idx] = minY
				t.nodeMaxY[idx] = maxY
			}
		}
	}
}

// UpdateHeights applies a batch of edits. The whole batch is validated first;
// duplicate coordinates take the last value. An invalid batch changes
// nothing. An empty batch performs no work and reports zero update stats.
func (t *Terrain) UpdateHeights(edits []HeightEdit) error {
	if len(edits) == 0 {
		t.mu.Lock()
		t.stats = HeightUpdateStats{}
		t.mu.Unlock()
		return nil
	}
	for _, e := range edits {
		if e.X < 0 || e.X >= t.size || e.Z < 0 || e.Z >= t.size {
			return fmt.Errorf("UpdateHeights: sample (%d,%d) is out of range for size %d",
				e.X, e.Z, t.size)
		}
		if math.IsNaN(e.Height) || math.IsInf(e.Height, 0) {
			return fmt.Errorf("UpdateHeights: height for sample (%d,%d) is not finite", e.X, e.Z)
		}
	}

	// last write wins per coordinate
	values := make(map[[2]int]float64, len(edits))
	order := make([][2]int, 0, len(edits))
	for _, e := range edits {
		key := [2]int{e.X, e.Z}
		if _, ok := values[key]; !ok {
			order = append(order, key)
		}
		values[key] = e.Height
	}

	// normal neighborhood: edited points plus their four axial neighbors
	normalSet := make(map[[2]int]struct{})
	addNormalPoint := func(x, z int) {
		if x >= 0 && x < t.size && z >= 0 && z < t.size {
			normalSet[[2]int{x, z}] = struct{}{}
		}
	}
	for _, key := range order {
		x, z := key[0], key[1]
		addNormalPoint(x, z)
		addNormalPoint(x-1, z)
		addNormalPoint(x+1, z)
		addNormalPoint(x, z-1)
		addNormalPoint(x, z+1)
	}

	// nodes whose bounds must be refreshed, deepest first. A sample on a
	// leaf boundary belongs to every leaf whose 9x9 sample set contains it.
	leafDepth := t.k - 3
	affected := make([]map[[2]int]struct{}, leafDepth+1)
	for d := range affected {
		affected[d] = make(map[[2]int]struct{})
	}
	leafQs := func(p int) []int {
		q := p / terrainLeafCells
		leafNodes := 1 << leafDepth
		if q >= leafNodes {
			// the last sample row/column belongs to the final leaf
			q = leafNodes - 1
		}
		qs := []int{q}
		if p%terrainLeafCells == 0 && p > 0 {
			qs = append(qs, q-1)
		}
		return qs
	}
	for _, key := range order {
		for _, qz := range leafQs(key[1]) {
			for _, qx := range leafQs(key[0]) {
				affected[leafDepth][[2]int{qx, qz}] = struct{}{}
			}
		}
	}
	for d := leafDepth - 1; d >= 0; d-- {
		for key := range affected[d+1] {
			affected[d][[2]int{key[0] / 2, key[1] / 2}] = struct{}{}
		}
	}

	t.mu.Lock()
	defer t.mu.Unlock()

	for _, key := range order {
		t.heights[key[1]*t.size+key[0]] = values[key]
	}

	var heightReads, normalUpdates, boundsUpdates uint64

	// refresh leaf bounds straight from the source heights: 9x9 reads each
	for key := range affected[leafDepth] {
		qx, qz := key[0], key[1]
		x0 := qx * terrainLeafCells
		z0 := qz * terrainLeafCells
		minY := math.Inf(1)
		maxY := math.Inf(-1)
		for lz := 0; lz <= terrainLeafCells; lz++ {
			for lx := 0; lx <= terrainLeafCells; lx++ {
				h := t.heightAt(x0+lx, z0+lz)
				heightReads++
				if h < minY {
					minY = h
				}
				if h > maxY {
					maxY = h
				}
			}
		}
		idx := t.nodeIndex(leafDepth, qx, qz)
		t.nodeMinY[idx] = minY
		t.nodeMaxY[idx] = maxY
		boundsUpdates++
	}
	// interior bounds come from the four refreshed children
	for d := leafDepth - 1; d >= 0; d-- {
		nodes := 1 << d
		for key := range affected[d] {
			qx, qz := key[0], key[1]
			idx := t.nodeIndex(d, qx, qz)
			child := t.nodeBase[d+1] + (2*qz)*(nodes*2) + 2*qx
			minY := math.Inf(1)
			maxY := math.Inf(-1)
			for cz := 0; cz < 2; cz++ {
				for cx := 0; cx < 2; cx++ {
					c := child + cz*(nodes*2) + cx
					if t.nodeMinY[c] < minY {
						minY = t.nodeMinY[c]
					}
					if t.nodeMaxY[c] > maxY {
						maxY = t.nodeMaxY[c]
					}
				}
			}
			t.nodeMinY[idx] = minY
			t.nodeMaxY[idx] = maxY
			boundsUpdates++
		}
	}

	// four source reads per refreshed normal
	for key := range normalSet {
		t.rebuildNormal(key[0], key[1])
		heightReads += 4
		normalUpdates++
	}

	t.revision++
	t.stats = HeightUpdateStats{
		HeightReads:   heightReads,
		NormalUpdates: normalUpdates,
		BoundsUpdates: boundsUpdates,
	}
	return nil
}

// TerrainInstance places a shared Terrain in the world. A world position is
// Origin + Scale*localPosition.
type TerrainInstance struct {
	Terrain *Terrain
	Origin  Vector
	Scale   float64
	Visible bool
}

// TerrainView describes one camera. Matrix transforms world positions into
// homogeneous clip space. SplitDistances has one entry per subdividable depth
// (k-3 total), is finite, positive and non-increasing.
type TerrainView struct {
	Eye            Vector
	Matrix         Matrix
	SplitDistances []float64
}

// TerrainLeaf is an independent copy describing one selected leaf block.
// X/Z are the lower-left source sample coordinates, Span is the leaf size in
// sample units and Stitch uses bits 0/1/2/3 for west/east/south/north.
type TerrainLeaf struct {
	Instance int
	X        int
	Z        int
	Span     int
	Depth    int
	Stitch   uint8
}

// DrawStats counts what the most recent DrawTerrain actually submitted.
type DrawStats struct {
	Vertices  uint64
	Triangles uint64
}

type terrainPlanLeaf struct {
	instance int
	x, z     int
	span     int
	depth    int
	stitch   uint8
	culled   bool
}

// TerrainPlan is a camera-specific, immutable selection of leaf blocks for a
// set of instances.
type TerrainPlan struct {
	instances []TerrainInstance
	leaves    []terrainPlanLeaf
	revisions map[*Terrain]uint64
	view      TerrainView
	lastDraw  DrawStats
}

const (
	stitchWest  = 1 << 0
	stitchEast  = 1 << 1
	stitchSouth = 1 << 2
	stitchNorth = 1 << 3
)

func isFiniteMatrix(m Matrix) bool {
	for _, v := range [16]float64{
		m.X00, m.X01, m.X02, m.X03,
		m.X10, m.X11, m.X12, m.X13,
		m.X20, m.X21, m.X22, m.X23,
		m.X30, m.X31, m.X32, m.X33,
	} {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return false
		}
	}
	return true
}

// PrepareTerrain independently selects leaves for every visible instance.
// Source terrains are never modified.
func PrepareTerrain(instances []TerrainInstance, view TerrainView) (*TerrainPlan, error) {
	if len(instances) == 0 {
		return nil, fmt.Errorf("PrepareTerrain: no instances")
	}
	var size int
	for i, inst := range instances {
		if inst.Terrain == nil {
			return nil, fmt.Errorf("PrepareTerrain: instance %d has nil Terrain", i)
		}
		if math.IsNaN(inst.Scale) || math.IsInf(inst.Scale, 0) || inst.Scale <= 0 {
			return nil, fmt.Errorf("PrepareTerrain: instance %d Scale must be finite and positive", i)
		}
		if inst.Origin.IsDegenerate() {
			return nil, fmt.Errorf("PrepareTerrain: instance %d Origin must be finite", i)
		}
		if i == 0 {
			size = inst.Terrain.size
		} else if inst.Terrain.size != size {
			return nil, fmt.Errorf("PrepareTerrain: all instances must share the same resource size, got %d and %d",
				size, inst.Terrain.size)
		}
	}
	k := instances[0].Terrain.k
	if len(view.SplitDistances) != k-3 {
		return nil, fmt.Errorf("PrepareTerrain: need %d split distances, got %d",
			k-3, len(view.SplitDistances))
	}
	for i, d := range view.SplitDistances {
		if math.IsNaN(d) || math.IsInf(d, 0) || d <= 0 {
			return nil, fmt.Errorf("PrepareTerrain: split distance %d must be finite and positive", i)
		}
		if i > 0 && d > view.SplitDistances[i-1] {
			return nil, fmt.Errorf("PrepareTerrain: split distances must be non-increasing")
		}
	}
	if view.Eye.IsDegenerate() || !isFiniteMatrix(view.Matrix) {
		return nil, fmt.Errorf("PrepareTerrain: eye and view matrix must be finite")
	}

	plan := &TerrainPlan{
		instances: append([]TerrainInstance(nil), instances...),
		revisions: make(map[*Terrain]uint64),
		view:      view,
	}
	for _, inst := range plan.instances {
		plan.revisions[inst.Terrain] = inst.Terrain.Revision()
	}

	for i, inst := range plan.instances {
		if !inst.Visible {
			continue
		}
		leaves, err := selectLeaves(i, inst, view)
		if err != nil {
			return nil, err
		}
		plan.leaves = append(plan.leaves, leaves...)
	}
	return plan, nil
}

// Leaves returns independent copies of the selected leaf descriptions.
func (p *TerrainPlan) Leaves() []TerrainLeaf {
	out := make([]TerrainLeaf, len(p.leaves))
	for i, l := range p.leaves {
		out[i] = TerrainLeaf{
			Instance: l.instance,
			X:        l.x,
			Z:        l.z,
			Span:     l.span,
			Depth:    l.depth,
			Stitch:   l.stitch,
		}
	}
	return out
}

// DrawStats returns what the most recent DrawTerrain submitted.
func (p *TerrainPlan) DrawStats() DrawStats {
	return p.lastDraw
}

type nodeKey struct{ d, qx, qz int }

func distancePointBox(p Vector, b Box) float64 {
	q := p.Max(b.Min).Min(b.Max)
	return p.Sub(q).Length()
}

func (t *Terrain) localNodeBox(x0, z0, span int, idx int) Box {
	return Box{
		Min: Vector{float64(x0), t.nodeMinY[idx], float64(z0)},
		Max: Vector{float64(x0 + span), t.nodeMaxY[idx], float64(z0 + span)},
	}
}

func worldBox(inst TerrainInstance, local Box) Box {
	return Box{
		Min: inst.Origin.Add(local.Min.MulScalar(inst.Scale)),
		Max: inst.Origin.Add(local.Max.MulScalar(inst.Scale)),
	}
}

func selectLeaves(instanceIndex int, inst TerrainInstance, view TerrainView) ([]terrainPlanLeaf, error) {
	t := inst.Terrain
	k := t.k
	leafDepth := k - 3

	t.mu.RLock()
	defer t.mu.RUnlock()

	selected := make(map[nodeKey]struct{})

	var visit func(d, qx, qz int)
	visit = func(d, qx, qz int) {
		if d == leafDepth {
			selected[nodeKey{d, qx, qz}] = struct{}{}
			return
		}
		span := 1 << (k - d)
		idx := t.nodeIndex(d, qx, qz)
		box := worldBox(inst, t.localNodeBox(qx*span, qz*span, span, idx))
		dist := distancePointBox(view.Eye, box)
		if dist < view.SplitDistances[d] {
			visit(d+1, 2*qx, 2*qz)
			visit(d+1, 2*qx+1, 2*qz)
			visit(d+1, 2*qx, 2*qz+1)
			visit(d+1, 2*qx+1, 2*qz+1)
			return
		}
		selected[nodeKey{d, qx, qz}] = struct{}{}
	}
	visit(0, 0, 0)

	// find the currently selected leaf containing source sample (x, z)
	findLeaf := func(x, z int) nodeKey {
		d, qx, qz := 0, 0, 0
		for {
			if _, ok := selected[nodeKey{d, qx, qz}]; ok {
				return nodeKey{d, qx, qz}
			}
			d++
			span := 1 << (k - d)
			maxQ := (1 << d) - 1
			qx = x / span
			qz = z / span
			if qx > maxQ {
				qx = maxQ
			}
			if qz > maxQ {
				qz = maxQ
			}
		}
	}

	// 2:1 closure: only subdivide, until every shared edge honors it
	for {
		var toSplit []nodeKey
		marked := make(map[nodeKey]struct{})
		for key := range selected {
			d := key.d
			if d == leafDepth {
				continue
			}
			span := 1 << (k - d)
			x0 := key.qx * span
			z0 := key.qz * span
			half := span / 2
			type probe struct{ x, z int }
			probes := make([]probe, 0, 4)
			if x0 > 0 {
				probes = append(probes, probe{x0 - 1, z0 + half})
			}
			if x0+span < t.size-1 {
				probes = append(probes, probe{x0 + span, z0 + half})
			}
			if z0 > 0 {
				probes = append(probes, probe{x0 + half, z0 - 1})
			}
			if z0+span < t.size-1 {
				probes = append(probes, probe{x0 + half, z0 + span})
			}
			for _, pr := range probes {
				n := findLeaf(pr.x, pr.z)
				nSpan := 1 << (k - n.d)
				if nSpan > 2*span {
					if _, ok := marked[n]; !ok {
						marked[n] = struct{}{}
						toSplit = append(toSplit, n)
					}
				}
			}
		}
		if len(toSplit) == 0 {
			break
		}
		for _, key := range toSplit {
			delete(selected, key)
			d, qx, qz := key.d, key.qx, key.qz
			selected[nodeKey{d + 1, 2 * qx, 2 * qz}] = struct{}{}
			selected[nodeKey{d + 1, 2*qx + 1, 2 * qz}] = struct{}{}
			selected[nodeKey{d + 1, 2 * qx, 2*qz + 1}] = struct{}{}
			selected[nodeKey{d + 1, 2*qx + 1, 2*qz + 1}] = struct{}{}
		}
	}

	leaves := make([]terrainPlanLeaf, 0, len(selected))
	for key := range selected {
		span := 1 << (k - key.d)
		leaves = append(leaves, terrainPlanLeaf{
			instance: instanceIndex,
			x:        key.qx * span,
			z:        key.qz * span,
			span:     span,
			depth:    key.d,
		})
	}

	// stitch flags: a fine block collapses its boundary odd vertices where
	// the block sharing the edge is coarser
	for i := range leaves {
		l := &leaves[i]
		half := l.span / 2
		if l.x > 0 {
			if n := findLeaf(l.x-1, l.z+half); 1<<(k-n.d) > l.span {
				l.stitch |= stitchWest
			}
		}
		if l.x+l.span < t.size-1 {
			if n := findLeaf(l.x+l.span, l.z+half); 1<<(k-n.d) > l.span {
				l.stitch |= stitchEast
			}
		}
		if l.z > 0 {
			if n := findLeaf(l.x+half, l.z-1); 1<<(k-n.d) > l.span {
				l.stitch |= stitchSouth
			}
		}
		if l.z+l.span < t.size-1 {
			if n := findLeaf(l.x+half, l.z+l.span); 1<<(k-n.d) > l.span {
				l.stitch |= stitchNorth
			}
		}
	}

	// frustum culling: only reject when one clip half-space excludes all
	// eight world AABB corners
	for i := range leaves {
		l := &leaves[i]
		idx := t.nodeIndex(l.depth, l.x/l.span, l.z/l.span)
		box := worldBox(inst, t.localNodeBox(l.x, l.z, l.span, idx))
		corners := [8]Vector{
			{box.Min.X, box.Min.Y, box.Min.Z},
			{box.Max.X, box.Min.Y, box.Min.Z},
			{box.Min.X, box.Max.Y, box.Min.Z},
			{box.Max.X, box.Max.Y, box.Min.Z},
			{box.Min.X, box.Min.Y, box.Max.Z},
			{box.Max.X, box.Min.Y, box.Max.Z},
			{box.Min.X, box.Max.Y, box.Max.Z},
			{box.Max.X, box.Max.Y, box.Max.Z},
		}
		var outside [6]int
		for _, c := range corners {
			clipped := view.Matrix.MulPositionW(c)
			if clipped.X < -clipped.W {
				outside[0]++
			}
			if clipped.X > clipped.W {
				outside[1]++
			}
			if clipped.Y < -clipped.W {
				outside[2]++
			}
			if clipped.Y > clipped.W {
				outside[3]++
			}
			if clipped.Z < -clipped.W {
				outside[4]++
			}
			if clipped.Z > clipped.W {
				outside[5]++
			}
		}
		for _, n := range outside {
			if n == 8 {
				l.culled = true
				break
			}
		}
	}

	return leaves, nil
}

// DrawTerrain rasterizes a prepared plan through Context.DrawIndexed. Every
// resource revision is verified before any shader call or buffer write; a
// stale plan fails without touching the previous draw statistics.
func DrawTerrain(plan *TerrainPlan, context *Context) (RasterizeInfo, error) {
	if plan == nil {
		return RasterizeInfo{}, fmt.Errorf("DrawTerrain: nil plan")
	}
	if context == nil {
		return RasterizeInfo{}, fmt.Errorf("DrawTerrain: nil context")
	}
	for terrain, revision := range plan.revisions {
		if terrain.Revision() != revision {
			return RasterizeInfo{}, fmt.Errorf("DrawTerrain: plan is stale; terrain revision changed from %d to %d",
				revision, terrain.Revision())
		}
	}

	var result RasterizeInfo
	var stats DrawStats

	for _, l := range plan.leaves {
		if l.culled {
			continue
		}
		inst := plan.instances[l.instance]
		t := inst.Terrain
		vertices := make([]Vertex, (terrainLeafCells+1)*(terrainLeafCells+1))

		t.mu.RLock()
		for j := 0; j <= terrainLeafCells; j++ {
			for i := 0; i <= terrainLeafCells; i++ {
				sx := l.x + i*l.span/terrainLeafCells
				sz := l.z + j*l.span/terrainLeafCells
				h := t.heights[sz*t.size+sx]
				local := Vector{float64(sx), h, float64(sz)}
				vertices[j*(terrainLeafCells+1)+i] = Vertex{
					Position: inst.Origin.Add(local.MulScalar(inst.Scale)),
					Normal:   t.normals[sz*t.size+sx],
					Texture:  Vector{float64(sx) / float64(t.size-1), float64(sz) / float64(t.size-1), 0},
					Color:    White,
				}
			}
		}
		t.mu.RUnlock()

		indices := terrainIndexTemplates[l.stitch]
		info, err := context.DrawIndexed(vertices, indices)
		if err != nil {
			return RasterizeInfo{}, err
		}
		result = result.Add(info)
		stats.Vertices += uint64(len(vertices))
		stats.Triangles += uint64(len(indices))
	}

	plan.lastDraw = stats
	return result, nil
}
