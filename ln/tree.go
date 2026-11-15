package ln

import (
	"sort"
)

type Tree struct {
	Box  Box
	Root *Node

	// entries is the slot-indexed view of the shapes in the tree. A shape
	// may be referenced by more than one slot; slots are the identity used
	// for ownership and query caching.
	entries []nodeEntry
	// slotLeaves records every leaf that currently owns a slot.
	slotLeaves map[int]map[*Node]struct{}
}

type nodeEntry struct {
	slot  int
	shape Shape
	box   Box
}

func NewTree(shapes []Shape) *Tree {
	entries := make([]nodeEntry, len(shapes))
	for i, shape := range shapes {
		entries[i] = nodeEntry{i, shape, shape.BoundingBox()}
	}
	node := newNodeFromEntries(entries)
	node.Split(0)
	tree := &Tree{Root: node, entries: entries}
	tree.rebuildMembership()
	if node.hasObjects {
		tree.Box = node.box
	}
	return tree
}

func (tree *Tree) Intersect(r Ray) Hit {
	if tree == nil || tree.Root == nil || !tree.Root.hasObjects {
		return NoHit
	}
	tmin, tmax := tree.Box.Intersect(r)
	if tmax < tmin || tmax <= 0 {
		return NoHit
	}
	query := &queryState{}
	if len(tree.entries) > 0 {
		query.cache = make([]slotHit, len(tree.entries))
	}
	hit := tree.Root.intersect(r, tmin, tmax, query)
	if !hit.valid {
		return NoHit
	}
	return hit.hit
}

// queryState holds all per-query mutable state. It is allocated for each
// Intersect call, so concurrent queries never share a cache and nothing is
// written back to nodes or shapes.
type queryState struct {
	cache []slotHit
}

type slotHit struct {
	computed bool
	hit      Hit
}

func (q *queryState) intersect(slot int, shape Shape, r Ray) Hit {
	if q == nil || q.cache == nil || slot < 0 || slot >= len(q.cache) {
		return shape.Intersect(r)
	}
	entry := &q.cache[slot]
	if !entry.computed {
		entry.hit = shape.Intersect(r)
		entry.computed = true
	}
	return entry.hit
}

// hitRef carries the owning slot alongside a hit so ties can be resolved by
// slot number.
type hitRef struct {
	hit   Hit
	slot  int
	valid bool
}

func minHitRef(a, b hitRef) hitRef {
	if !a.valid {
		return b
	}
	if !b.valid {
		return a
	}
	if a.hit.T < b.hit.T {
		return a
	}
	if b.hit.T < a.hit.T {
		return b
	}
	if a.slot <= b.slot {
		return a
	}
	return b
}

type Node struct {
	Axis   Axis
	Point  float64
	Shapes []Shape
	Left   *Node
	Right  *Node

	entries    []nodeEntry
	box        Box
	hasObjects bool
	parent     *Node
}

func NewNode(shapes []Shape) *Node {
	entries := make([]nodeEntry, len(shapes))
	for i, shape := range shapes {
		entries[i] = nodeEntry{i, shape, shape.BoundingBox()}
	}
	return newNodeFromEntries(entries)
}

func newNodeFromEntries(entries []nodeEntry) *Node {
	node := &Node{Axis: AxisNone}
	node.entries = entries
	node.recomputeLeaf(nil)
	return node
}

func (node *Node) Intersect(r Ray, tmin, tmax float64) Hit {
	if node == nil {
		return NoHit
	}
	hit := node.intersect(r, tmin, tmax, nil)
	if !hit.valid {
		return NoHit
	}
	return hit.hit
}

func (node *Node) intersect(r Ray, tmin, tmax float64, q *queryState) hitRef {
	if node == nil || !node.hasObjects {
		return hitRef{}
	}
	if node.Axis == AxisNone {
		return node.intersectEntries(r, q)
	}
	var split, direction, origin float64
	switch node.Axis {
	case AxisX:
		split, direction, origin = node.Point, r.Direction.X, r.Origin.X
	case AxisY:
		split, direction, origin = node.Point, r.Direction.Y, r.Origin.Y
	case AxisZ:
		split, direction, origin = node.Point, r.Direction.Z, r.Origin.Z
	}
	if direction == 0 {
		// The ray is parallel to the split plane and never crosses it.
		// A ray exactly on the plane has to consider both subtrees.
		if origin < split {
			return node.Left.intersect(r, tmin, tmax, q)
		}
		if origin > split {
			return node.Right.intersect(r, tmin, tmax, q)
		}
		h1 := node.Left.intersect(r, tmin, tmax, q)
		upper := tmax
		if h1.valid && h1.hit.T < upper {
			upper = h1.hit.T
		}
		h2 := node.Right.intersect(r, tmin, upper, q)
		return minHitRef(h1, h2)
	}
	tsplit := (split - origin) / direction
	leftFirst := (origin < split) || (origin == split && direction <= 0)
	var first, second *Node
	if leftFirst {
		first = node.Left
		second = node.Right
	} else {
		first = node.Right
		second = node.Left
	}
	if tsplit > tmax || tsplit <= 0 {
		return first.intersect(r, tmin, tmax, q)
	} else if tsplit < tmin {
		return second.intersect(r, tmin, tmax, q)
	}
	h1 := first.intersect(r, tmin, tsplit, q)
	// A hit strictly before the split cannot be beaten by anything in the
	// second subtree. On an exact tie both sides have to be considered so
	// that the smaller slot number can win.
	if h1.valid && h1.hit.T < tsplit {
		return h1
	}
	upper := tmax
	if h1.valid && h1.hit.T < upper {
		upper = h1.hit.T
	}
	h2 := second.intersect(r, tsplit, upper, q)
	return minHitRef(h1, h2)
}

func (node *Node) intersectEntries(r Ray, q *queryState) hitRef {
	var best hitRef
	for _, entry := range node.entries {
		h := q.intersect(entry.slot, entry.shape, r)
		if h.T < INF {
			best = minHitRef(best, hitRef{h, entry.slot, true})
		}
	}
	return best
}

func (node *Node) IntersectShapes(r Ray) Hit {
	hit := NoHit
	if node.entries == nil && len(node.Shapes) > 0 {
		for _, shape := range node.Shapes {
			h := shape.Intersect(r)
			if h.T < hit.T {
				hit = h
			}
		}
		return hit
	}
	for _, entry := range node.entries {
		h := entry.shape.Intersect(r)
		if h.T < hit.T {
			hit = h
		}
	}
	return hit
}

func (node *Node) PartitionScore(axis Axis, point float64) int {
	left, right := 0, 0
	for _, shape := range node.Shapes {
		box := shape.BoundingBox()
		l, r := box.Partition(axis, point)
		if l {
			left++
		}
		if r {
			right++
		}
	}
	if left >= right {
		return left
	} else {
		return right
	}
}

func (node *Node) Partition(size int, axis Axis, point float64) (left, right []Shape) {
	left = make([]Shape, 0, size)
	right = make([]Shape, 0, size)
	for _, shape := range node.Shapes {
		box := shape.BoundingBox()
		l, r := box.Partition(axis, point)
		if l {
			left = append(left, shape)
		}
		if r {
			right = append(right, shape)
		}
	}
	return
}

func (node *Node) partitionScoreEntries(axis Axis, point float64) int {
	left, right := 0, 0
	for _, entry := range node.entries {
		l, r := entry.box.Partition(axis, point)
		if l {
			left++
		}
		if r {
			right++
		}
	}
	if left >= right {
		return left
	}
	return right
}

func (node *Node) Split(depth int) {
	if node.entries == nil && len(node.Shapes) > 0 {
		node.entries = make([]nodeEntry, len(node.Shapes))
		for i, shape := range node.Shapes {
			node.entries[i] = nodeEntry{i, shape, shape.BoundingBox()}
		}
	}
	if len(node.entries) < 8 {
		return
	}
	xs := make([]float64, 0, len(node.entries)*2)
	ys := make([]float64, 0, len(node.entries)*2)
	zs := make([]float64, 0, len(node.entries)*2)
	for _, entry := range node.entries {
		box := entry.box
		xs = append(xs, box.Min.X)
		xs = append(xs, box.Max.X)
		ys = append(ys, box.Min.Y)
		ys = append(ys, box.Max.Y)
		zs = append(zs, box.Min.Z)
		zs = append(zs, box.Max.Z)
	}
	sort.Float64s(xs)
	sort.Float64s(ys)
	sort.Float64s(zs)
	mx, my, mz := Median(xs), Median(ys), Median(zs)
	best := int(float64(len(node.entries)) * 0.85)
	bestAxis := AxisNone
	bestPoint := 0.0
	sx := node.partitionScoreEntries(AxisX, mx)
	if sx < best {
		best = sx
		bestAxis = AxisX
		bestPoint = mx
	}
	sy := node.partitionScoreEntries(AxisY, my)
	if sy < best {
		best = sy
		bestAxis = AxisY
		bestPoint = my
	}
	sz := node.partitionScoreEntries(AxisZ, mz)
	if sz < best {
		best = sz
		bestAxis = AxisZ
		bestPoint = mz
	}
	if bestAxis == AxisNone {
		return
	}
	left := make([]nodeEntry, 0, len(node.entries))
	right := make([]nodeEntry, 0, len(node.entries))
	for _, entry := range node.entries {
		l, r := entry.box.Partition(bestAxis, bestPoint)
		if l {
			left = append(left, entry)
		}
		if r {
			right = append(right, entry)
		}
	}
	node.Axis = bestAxis
	node.Point = bestPoint
	node.Left = newNodeFromEntries(left)
	node.Left.parent = node
	node.Right = newNodeFromEntries(right)
	node.Right.parent = node
	node.Shapes = nil // only needed at leaf nodes
	node.entries = nil
	node.recomputeInternal(nil)
	node.Left.Split(depth + 1)
	node.Right.Split(depth + 1)
}

func (node *Node) recomputeLeaf(stats *EditStats) {
	node.hasObjects = len(node.entries) > 0
	if !node.hasObjects {
		node.box = Box{}
		node.Shapes = nil
		return
	}
	box := node.entries[0].box
	if stats != nil {
		stats.ShapeReads++
	}
	for _, entry := range node.entries[1:] {
		box = box.Extend(entry.box)
		if stats != nil {
			stats.ShapeReads++
		}
	}
	node.box = box
	if cap(node.Shapes) < len(node.entries) {
		node.Shapes = make([]Shape, len(node.entries))
	} else {
		node.Shapes = node.Shapes[:len(node.entries)]
	}
	for i, entry := range node.entries {
		node.Shapes[i] = entry.shape
	}
}

func (node *Node) recomputeInternal(stats *EditStats) {
	if node.Left == nil && node.Right == nil {
		node.recomputeLeaf(stats)
		return
	}
	hasLeft := node.Left != nil && node.Left.hasObjects
	hasRight := node.Right != nil && node.Right.hasObjects
	switch {
	case hasLeft && hasRight:
		node.box = node.Left.box.Extend(node.Right.box)
		node.hasObjects = true
	case hasLeft:
		node.box = node.Left.box
		node.hasObjects = true
	case hasRight:
		node.box = node.Right.box
		node.hasObjects = true
	default:
		node.box = Box{}
		node.hasObjects = false
	}
}

func (node *Node) insertEntry(entry nodeEntry, stats *EditStats, added *[]*Node) {
	if stats != nil {
		stats.NodeVisits++
	}
	if node.Axis == AxisNone || node.Left == nil || node.Right == nil {
		node.entries = append(node.entries, entry)
		node.recomputeLeaf(stats)
		*added = append(*added, node)
		return
	}
	left, right := entry.box.Partition(node.Axis, node.Point)
	if left {
		node.Left.insertEntry(entry, stats, added)
	}
	if right {
		node.Right.insertEntry(entry, stats, added)
	}
	node.recomputeInternal(stats)
}

func (node *Node) removeSlotEntry(slot int, stats *EditStats) bool {
	index := -1
	for i, entry := range node.entries {
		if stats != nil {
			stats.ShapeReads++
		}
		if entry.slot == slot {
			index = i
			break
		}
	}
	if index < 0 {
		return false
	}
	copy(node.entries[index:], node.entries[index+1:])
	node.entries = node.entries[:len(node.entries)-1]
	node.recomputeLeaf(stats)
	return true
}

func updateAncestors(node *Node, stats *EditStats) {
	for parent := node.parent; parent != nil; parent = parent.parent {
		if stats != nil {
			stats.NodeVisits++
		}
		parent.recomputeInternal(stats)
	}
}

func (tree *Tree) rebuildMembership() {
	tree.slotLeaves = make(map[int]map[*Node]struct{})
	var walk func(*Node)
	walk = func(node *Node) {
		if node == nil {
			return
		}
		if node.Axis == AxisNone {
			for _, entry := range node.entries {
				leaves := tree.slotLeaves[entry.slot]
				if leaves == nil {
					leaves = make(map[*Node]struct{})
					tree.slotLeaves[entry.slot] = leaves
				}
				leaves[node] = struct{}{}
			}
			return
		}
		walk(node.Left)
		walk(node.Right)
	}
	walk(tree.Root)
}

func (tree *Tree) registerLeaf(slot int, leaf *Node) {
	if tree.slotLeaves == nil {
		tree.slotLeaves = make(map[int]map[*Node]struct{})
	}
	leaves := tree.slotLeaves[slot]
	if leaves == nil {
		leaves = make(map[*Node]struct{})
		tree.slotLeaves[slot] = leaves
	}
	leaves[leaf] = struct{}{}
}

func (tree *Tree) updateBox() {
	if tree.Root != nil && tree.Root.hasObjects {
		tree.Box = tree.Root.box
	} else {
		tree.Box = Box{}
	}
}
