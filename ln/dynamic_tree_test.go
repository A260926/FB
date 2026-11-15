package ln

import (
	"math"
	"math/rand"
	"reflect"
	"sync"
	"sync/atomic"
	"testing"

	"github.com/fogleman/ln/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// Test helpers

type countingShape struct {
	inner Shape
	calls int64
}

func (s *countingShape) Compile()                          { s.inner.Compile() }
func (s *countingShape) BoundingBox() Box                  { return s.inner.BoundingBox() }
func (s *countingShape) Contains(v Vector, f float64) bool { return s.inner.Contains(v, f) }
func (s *countingShape) Paths() Paths                      { return s.inner.Paths() }

func (s *countingShape) Intersect(r Ray) Hit {
	atomic.AddInt64(&s.calls, 1)
	return s.inner.Intersect(r)
}

func (s *countingShape) Count() int64 {
	return atomic.LoadInt64(&s.calls)
}

type boxShape struct {
	box Box
}

func (s *boxShape) Compile()                          {}
func (s *boxShape) BoundingBox() Box                  { return s.box }
func (s *boxShape) Contains(v Vector, f float64) bool { return false }
func (s *boxShape) Intersect(r Ray) Hit               { return NoHit }
func (s *boxShape) Paths() Paths                      { return nil }

func bruteNearest(scene *Scene, r Ray) Hit {
	best := NoHit
	for _, shape := range scene.Shapes {
		h := shape.Intersect(r)
		if h.T < best.T {
			best = h
		}
	}
	return best
}

func sameHit(a, b Hit) bool {
	if a.Ok() != b.Ok() {
		return false
	}
	if !a.Ok() {
		return true
	}
	if math.Abs(a.T-b.T) > 1e-9 {
		return false
	}
	return sameShape(a.Shape, b.Shape)
}

func sameShape(a, b Shape) bool {
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	ta, tb := reflect.TypeOf(a), reflect.TypeOf(b)
	if ta != tb {
		return false
	}
	if ta.Comparable() {
		return reflect.ValueOf(a).Interface() == reflect.ValueOf(b).Interface()
	}
	return reflect.DeepEqual(a, b)
}

func walkNodes(root *Node, fn func(*Node)) {
	if root == nil {
		return
	}
	fn(root)
	walkNodes(root.Left, fn)
	walkNodes(root.Right, fn)
}

type nodeSig struct {
	axis        Axis
	point       float64
	left, right *Node
}

func snapshotTree(root *Node) map[*Node]nodeSig {
	result := make(map[*Node]nodeSig)
	walkNodes(root, func(n *Node) {
		result[n] = nodeSig{n.Axis, n.Point, n.Left, n.Right}
	})
	return result
}

func sameStructure(before, after map[*Node]nodeSig) bool {
	if len(before) != len(after) {
		return false
	}
	for node, sig := range before {
		other, ok := after[node]
		if !ok || sig != other {
			return false
		}
	}
	return true
}

func checkParents(c *taskcheck.C, root *Node) {
	walkNodes(root, func(n *Node) {
		c.Check(n.parent == nil || n.parent.Left == n || n.parent.Right == n,
			"parent link broken at node %p", n)
		if n.Left != nil {
			c.Check(n.Left.parent == n, "left child parent link broken")
		}
		if n.Right != nil {
			c.Check(n.Right.parent == n, "right child parent link broken")
		}
	})
}

// expectedSlotLeaves walks the existing split planes with the given box. It
// must agree with the incrementally maintained slot membership.
func expectedSlotLeaves(root *Node, box Box) map[*Node]bool {
	result := make(map[*Node]bool)
	var walk func(*Node)
	walk = func(n *Node) {
		if n == nil {
			return
		}
		if n.Axis == AxisNone {
			result[n] = true
			return
		}
		left, right := box.Partition(n.Axis, n.Point)
		if left {
			walk(n.Left)
		}
		if right {
			walk(n.Right)
		}
	}
	walk(root)
	return result
}

func leafEntries(node *Node) []nodeEntry {
	if node.Axis == AxisNone {
		return node.entries
	}
	return nil
}

// checkNodeBox recurses through the tree and validates each aggregate bound.
func checkNodeBox(c *taskcheck.C, node *Node) (Box, bool) {
	if node == nil {
		return Box{}, false
	}
	if node.Axis == AxisNone {
		if len(node.entries) == 0 {
			c.Check(node.box == (Box{}), "empty leaf bound is not Box{}")
			return Box{}, false
		}
		box := node.entries[0].box
		for _, entry := range node.entries[1:] {
			box = box.Extend(entry.box)
		}
		c.Check(box == node.box, "leaf bound mismatch: %v vs %v", box, node.box)
		return box, true
	}
	lb, lok := checkNodeBox(c, node.Left)
	rb, rok := checkNodeBox(c, node.Right)
	var want Box
	has := false
	switch {
	case lok && rok:
		want, has = lb.Extend(rb), true
	case lok:
		want, has = lb, true
	case rok:
		want, has = rb, true
	}
	if !has {
		c.Check(node.box == (Box{}), "empty internal bound is not Box{}")
	} else {
		c.Check(want == node.box, "internal bound mismatch: %v vs %v", want, node.box)
	}
	return want, has
}

func checkIndex(c *taskcheck.C, scene *Scene) {
	tree := scene.Tree
	c.Check(tree != nil, "scene tree is nil")
	if tree == nil {
		return
	}
	c.Check(len(tree.entries) == len(scene.Shapes), "entry count %d != shapes %d", len(tree.entries), len(scene.Shapes))
	for slot, shape := range scene.Shapes {
		if slot < len(tree.entries) {
			c.Check(tree.entries[slot].slot == slot, "entry %d has slot %d", slot, tree.entries[slot].slot)
			c.Check(sameShape(tree.entries[slot].shape, shape), "entry %d shape mismatch", slot)
			c.Check(tree.entries[slot].box == shape.BoundingBox(), "entry %d stale box", slot)
		}
	}
	// slotLeaves must match the leaves reachable with each slot's current box.
	actualSlots := make(map[int]bool)
	for slot, leaves := range tree.slotLeaves {
		actualSlots[slot] = true
		want := expectedSlotLeaves(tree.Root, scene.Shapes[slot].BoundingBox())
		c.Check(len(leaves) == len(want), "slot %d owns %d leaves, want %d", slot, len(leaves), len(want))
		for leaf := range want {
			_, ok := leaves[leaf]
			c.Check(ok, "slot %d missing leaf %p", slot, leaf)
		}
	}
	for slot := range scene.Shapes {
		_, ok := actualSlots[slot]
		c.Check(ok, "slot %d missing from membership index", slot)
	}
	// Leaf entry lists must mirror the membership index.
	seen := make(map[int]int)
	walkNodes(tree.Root, func(n *Node) {
		if n.Axis != AxisNone {
			return
		}
		slots := make(map[int]bool)
		for _, entry := range n.entries {
			c.Check(!slots[entry.slot], "leaf %p lists slot %d twice", n, entry.slot)
			slots[entry.slot] = true
			seen[entry.slot]++
			_, registered := tree.slotLeaves[entry.slot][n]
			c.Check(registered, "leaf %p not registered for slot %d", n, entry.slot)
		}
		for i, shape := range n.Shapes {
			if i < len(n.entries) {
				c.Check(sameShape(shape, n.entries[i].shape), "leaf Shapes mirror mismatch")
			}
		}
	})
	for slot := range scene.Shapes {
		c.Check(seen[slot] > 0, "slot %d has no leaf entry", slot)
	}
	for slot, leaves := range tree.slotLeaves {
		for leaf := range leaves {
			found := false
			for _, entry := range leaf.entries {
				if entry.slot == slot {
					found = true
				}
			}
			c.Check(found, "membership leaf %p lacks slot %d", leaf, slot)
		}
	}
	rootBox, has := checkNodeBox(c, tree.Root)
	if has {
		c.Check(tree.Box == rootBox, "tree root box mismatch: %v vs %v", tree.Box, rootBox)
	} else {
		c.Check(tree.Box == (Box{}), "empty tree box is not Box{}")
	}
	if len(scene.Shapes) > 0 {
		c.Check(tree.Box == BoxForShapes(scene.Shapes), "tree box is not the union of shapes")
	}
	checkParents(c, tree.Root)
}

func randomRay(rng *rand.Rand) Ray {
	origin := Vector{rng.Float64()*20 - 10, rng.Float64()*20 - 10, rng.Float64()*20 - 10}
	direction := Vector{rng.Float64()*2 - 1, rng.Float64()*2 - 1, rng.Float64()*2 - 1}
	if direction.Length() == 0 {
		direction = Vector{X: 1}
	}
	return Ray{origin, direction.Normalize()}
}

func checkQueries(c *taskcheck.C, scene *Scene, rays []Ray) {
	for i, ray := range rays {
		got := scene.Intersect(ray)
		want := bruteNearest(scene, ray)
		c.Check(sameHit(got, want), "ray %d mismatch: got T=%v want T=%v", i, got.T, want.T)
	}
}

// gridScene creates a lattice of small spheres on the X axis and adds a long
// object that spans many split planes.
func gridScene(n int, withLong bool) *Scene {
	scene := &Scene{}
	for i := 0; i < n; i++ {
		scene.Add(NewSphere(Vector{X: float64(i) * 2, Y: 0, Z: 0}, 0.3))
	}
	if withLong {
		scene.Add(NewCube(
			Vector{X: -5, Y: -0.2, Z: -0.2},
			Vector{X: float64(n)*2 + 5, Y: 0.2, Z: 0.2},
		))
	}
	return scene
}

// ---------------------------------------------------------------------------
// TestMembership: slot ownership, repeated edits, shared pointers, bounds.

func TestMembership(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		testLongObjectMembership(c)
		testRepeatedSwaps(c)
		testSharedPointers(c)
		testTightBounds(c)
		testInvalidReplacements(c)
		testEmptyAndPreCompile(c)
	})
}

func testLongObjectMembership(c *taskcheck.C) {
	scene := gridScene(64, true)
	scene.Compile()
	longSlot := len(scene.Shapes) - 1
	checkIndex(c, scene)

	if tree := scene.Tree; tree != nil {
		c.Check(len(tree.slotLeaves[longSlot]) > 1, "long object should live in multiple leaves")
	}

	// Move the long object far away: it must leave every old leaf.
	outside := NewCube(Vector{X: 300, Y: -0.1, Z: -0.1}, Vector{X: 320, Y: 0.1, Z: 0.1})
	treeBefore := scene.Tree
	rootBefore := scene.Tree.Root
	beforeSig := snapshotTree(scene.Tree.Root)
	err := scene.ReplaceShape(longSlot, outside)
	c.Check(err == nil, "valid replacement rejected: %v", err)
	c.Check(scene.Tree == treeBefore, "tree pointer changed")
	c.Check(scene.Tree.Root == rootBefore, "root pointer changed")
	c.Check(sameStructure(beforeSig, snapshotTree(scene.Tree.Root)), "replace rebuilt the tree")
	checkIndex(c, scene)
	c.Check(len(scene.Tree.slotLeaves[longSlot]) >= 1, "moved object has no leaf")
	for leaf := range scene.Tree.slotLeaves[longSlot] {
		for _, entry := range leaf.entries {
			if entry.slot == longSlot {
				c.Check(entry.box == outside.BoundingBox(), "moved object kept its old box")
			}
		}
	}

	// And back again: it must be re-registered across the original leaves.
	back := NewCube(Vector{X: -4, Y: -0.1, Z: -0.1}, Vector{X: 150, Y: 0.1, Z: 0.1})
	c.Check(scene.ReplaceShape(longSlot, back) == nil, "move back failed")
	c.Check(len(scene.Tree.slotLeaves[longSlot]) > 1, "long object did not return to multiple leaves")
	checkIndex(c, scene)

	rng := rand.New(rand.NewSource(7))
	var rays []Ray
	for i := 0; i < 200; i++ {
		rays = append(rays, randomRay(rng))
	}
	rays = append(rays,
		Ray{Vector{X: 0, Y: 3, Z: 0}, Vector{Z: -1}},
		Ray{Vector{X: 30, Y: 0.05, Z: 5}, Vector{Z: -1}},
		Ray{Vector{X: 150, Y: 0, Z: 0}, Vector{X: -1}},
	)
	checkQueries(c, scene, rays)
}

func testRepeatedSwaps(c *taskcheck.C) {
	scene := gridScene(40, false)
	scene.Compile()
	a := NewSphere(Vector{X: 5, Y: 5, Z: 5}, 1.0)
	b := NewSphere(Vector{X: -5, Y: -5, Z: -5}, 1.0)
	c.Check(scene.ReplaceShape(3, a) == nil, "replace failed")
	c.Check(scene.ReplaceShape(7, b) == nil, "replace failed")
	checkIndex(c, scene)
	rng := rand.New(rand.NewSource(11))
	rays := make([]Ray, 0, 60)
	for i := 0; i < 60; i++ {
		rays = append(rays, randomRay(rng))
	}
	for round := 0; round < 20; round++ {
		slot := round % 9
		var replacement Shape
		if round%2 == 0 {
			replacement = NewCube(
				Vector{X: float64(round) * 0.7, Y: -1, Z: -1},
				Vector{X: float64(round)*0.7 + 2, Y: 1, Z: 1},
			)
		} else {
			replacement = NewSphere(Vector{X: float64(round) * 1.3, Y: 1, Z: 2}, 0.8)
		}
		c.Check(scene.ReplaceShape(slot, replacement) == nil, "round %d replace failed", round)
		checkIndex(c, scene)
		checkQueries(c, scene, rays)
	}
	// Each replacement only touched the scene's slot, never rebuilt the tree.
	c.Check(len(scene.Shapes) == 40, "slot count changed")
	_ = a
	_ = b

	// The old shape no longer contributes to Intersect or Paths.
	scene2 := &Scene{}
	old := NewSphere(Vector{X: 5, Y: 0, Z: 0}, 1)
	scene2.Add(old)
	repl := NewCube(Vector{X: -1, Y: -1, Z: -1}, Vector{X: 1, Y: 1, Z: 1})
	c.Check(scene2.ReplaceShape(0, repl) == nil, "paths replace failed")
	paths := scene2.Paths()
	c.Check(len(paths) == len(repl.Paths()), "old shape still contributes paths: %d vs %d", len(paths), len(repl.Paths()))
	if len(paths) > 0 {
		box := paths.BoundingBox()
		c.Check(box.Max.X <= 1 && box.Min.X >= -1, "paths contain the removed shape: %v", box)
	}
	oldRay := Ray{Origin: Vector{X: 5, Y: 0, Z: 3}, Direction: Vector{Z: -1}}
	c.Check(sameHit(scene2.Intersect(oldRay), bruteNearest(scene2, oldRay)), "old shape still hit")
}

func testSharedPointers(c *taskcheck.C) {
	scene := &Scene{}
	shared := &countingShape{inner: NewSphere(Vector{X: 2, Y: 0, Z: 0}, 0.5)}
	scene.Add(shared)
	for i := 0; i < 12; i++ {
		scene.Add(NewSphere(Vector{X: float64(i) * 3, Y: 5, Z: 0}, 0.2))
	}
	scene.Add(shared) // same pointer, different slot
	sharedSlot := len(scene.Shapes) - 1
	scene.Compile()
	checkIndex(c, scene)

	shared.calls = 0
	ray := Ray{Vector{X: 2, Y: 0, Z: 4}, Vector{Z: -1}}
	hit := scene.Intersect(ray)
	c.Check(hit.Ok(), "shared shape not hit")
	// Two slots, one call each, even though both are in the same leaf.
	c.Check(shared.Count() == 2, "shared pointer slots should each be queried once, got %d", shared.Count())

	// Replacing one slot must not disturb the other slot's membership.
	shared.calls = 0
	c.Check(scene.ReplaceShape(0, NewSphere(Vector{X: -30, Y: 0, Z: 0}, 0.5)) == nil, "replace failed")
	checkIndex(c, scene)
	c.Check(len(scene.Tree.slotLeaves[sharedSlot]) >= 1, "shared slot lost membership")
	c.Check(sameShape(scene.Shapes[sharedSlot], shared), "shared slot was overwritten")
	hit = scene.Intersect(ray)
	c.Check(hit.Ok(), "shared slot stopped being hit")
	c.Check(sameShape(hit.Shape, shared.inner), "slot identity not preserved")
	c.Check(shared.Count() == 1, "only the surviving shared slot should be queried, got %d", shared.Count())

	// A slot copied into many leaves still calls Intersect at most once.
	long := &countingShape{inner: NewCube(
		Vector{X: -10, Y: -0.1, Z: -0.1}, Vector{X: 40, Y: 0.1, Z: 0.1},
	)}
	longSlot := len(scene.Shapes)
	scene.Add(long)
	scene.Compile()
	checkIndex(c, scene)
	c.Check(len(scene.Tree.slotLeaves[longSlot]) > 1, "long counting object should span leaves")
	long.calls = 0
	hit = scene.Intersect(Ray{Vector{X: 20, Y: 3, Z: 0}, Vector{Y: -1}})
	c.Check(hit.Ok(), "long object not hit")
	c.Check(long.Count() == 1, "multi-leaf slot queried %d times in one Intersect", long.Count())
}

func testTightBounds(c *taskcheck.C) {
	scene := &Scene{}
	scene.Add(NewSphere(Vector{X: 0, Y: 0, Z: 0}, 1.0))
	for i := 0; i < 30; i++ {
		scene.Add(NewSphere(Vector{X: 10 + float64(i)*2, Y: 0, Z: 0}, 0.2))
	}
	scene.Compile()
	checkIndex(c, scene)
	c.Check(math.Abs(scene.Tree.Box.Min.X-(-1)) < 1e-12, "unexpected initial min")

	// Move the extreme object inward; the root box must shrink with it.
	c.Check(scene.ReplaceShape(0, NewSphere(Vector{X: 10, Y: 0, Z: 0}, 0.2)) == nil, "replace failed")
	checkIndex(c, scene)
	c.Check(math.Abs(scene.Tree.Box.Min.X-9.8) < 1e-12, "root box did not shrink: %v", scene.Tree.Box.Min.X)

	// Add a far object to the extreme, then remove it by replacing with a
	// small central object. Both directions are exercised.
	c.Check(scene.ReplaceShape(1, NewSphere(Vector{X: 1000, Y: 0, Z: 0}, 1.0)) == nil, "replace failed")
	checkIndex(c, scene)
	c.Check(math.Abs(scene.Tree.Box.Max.X-1001) < 1e-12, "root box did not grow")
	c.Check(scene.ReplaceShape(1, NewSphere(Vector{X: 10, Y: 0, Z: 0}, 0.2)) == nil, "replace failed")
	checkIndex(c, scene)
	c.Check(math.Abs(scene.Tree.Box.Max.X-68.2) < 1e-12, "root box did not shrink after extreme move")

	// An object moved well outside the original root box must still be found.
	c.Check(scene.ReplaceShape(2, NewSphere(Vector{X: 500, Y: -300, Z: 200}, 1.0)) == nil, "replace failed")
	checkIndex(c, scene)
	outsideRay := Ray{
		Origin:    Vector{X: 500, Y: -300, Z: 210},
		Direction: Vector{Z: -1},
	}
	hit := scene.Intersect(outsideRay)
	c.Check(hit.Ok(), "object outside the original root box was not found")
	c.Check(math.Abs(hit.T-9) < 1e-9, "unexpected hit distance %v", hit.T)
	c.Check(sameHit(hit, bruteNearest(scene, outsideRay)), "outside-box hit differs from reference")
}

func testInvalidReplacements(c *taskcheck.C) {
	scene := gridScene(20, true)
	scene.Compile()
	checkIndex(c, scene)
	treeBefore := scene.Tree
	sigBefore := snapshotTree(scene.Tree.Root)
	shapesBefore := append([]Shape(nil), scene.Shapes...)
	statsBefore := scene.LastEditStats()

	var typedNil *Sphere
	cases := []struct {
		name  string
		index int
		shape Shape
	}{
		{"negative index", -1, NewSphere(Vector{}, 1)},
		{"index past end", len(scene.Shapes), NewSphere(Vector{}, 1)},
		{"nil", 0, nil},
		{"typed nil", 0, typedNil},
		{"nan box", 0, &boxShape{Box{Vector{X: math.NaN()}, Vector{X: 1, Y: 1, Z: 1}}}},
		{"inf box", 0, &boxShape{Box{Vector{X: math.Inf(-1)}, Vector{X: 1, Y: 1, Z: 1}}}},
		{"min above max", 0, &boxShape{Box{Vector{X: 2, Y: 0, Z: 0}, Vector{X: 1, Y: 1, Z: 1}}}},
		{"min above max on z", 0, &boxShape{Box{Vector{X: 0, Y: 0, Z: 5}, Vector{X: 1, Y: 1, Z: 4}}}},
	}
	for _, tc := range cases {
		err := scene.ReplaceShape(tc.index, tc.shape)
		c.Check(err != nil, "%s should be rejected", tc.name)
	}
	c.Check(scene.Tree == treeBefore, "invalid replacement changed the tree")
	c.Check(sameStructure(sigBefore, snapshotTree(scene.Tree.Root)), "invalid replacement changed tree nodes")
	c.Check(len(scene.Shapes) == len(shapesBefore), "invalid replacement changed slot count")
	for i := range shapesBefore {
		c.Check(sameShape(scene.Shapes[i], shapesBefore[i]), "invalid replacement changed slot %d", i)
	}
	c.Check(scene.LastEditStats() == statsBefore, "invalid replacement changed edit stats")
	checkIndex(c, scene)

	// The scene still works after all the rejected edits.
	rng := rand.New(rand.NewSource(21))
	rays := make([]Ray, 0, 40)
	for i := 0; i < 40; i++ {
		rays = append(rays, randomRay(rng))
	}
	checkQueries(c, scene, rays)

	// A valid, degenerate-but-finite box is accepted.
	degenerate := &boxShape{Box{Vector{}, Vector{}}}
	err := scene.ReplaceShape(0, degenerate)
	c.Check(err == nil, "degenerate finite box should be accepted: %v", err)
	checkIndex(c, scene)
	c.Check(sameShape(scene.Shapes[0], degenerate), "replacement not stored")
}

func testEmptyAndPreCompile(c *taskcheck.C) {
	var empty Scene
	empty.Compile()
	c.Check(empty.Tree != nil, "empty compile did not create a tree")
	c.Check(empty.Tree.Box == (Box{}), "empty tree must use Box{}")
	c.Check(!empty.Intersect(Ray{Vector{}, Vector{Z: 1}}).Ok(), "empty tree must return NoHit")
	c.Check(empty.ReplaceShape(0, NewSphere(Vector{}, 1)) != nil, "replace on empty scene must fail")
	c.Check(empty.ReplaceShape(-1, NewSphere(Vector{}, 1)) != nil, "replace on empty scene must fail")

	// Compile first, then Add, then intersect without another Compile.
	empty.Add(NewSphere(Vector{X: 0, Y: 0, Z: 0}, 1))
	c.Check(empty.Intersect(Ray{Vector{X: 0, Y: 0, Z: 5}, Vector{Z: -1}}).Ok(),
		"Add after Compile was not queried")
	empty.Add(NewSphere(Vector{X: 4, Y: 0, Z: 0}, 1))
	checkIndex(c, &empty)

	// Replace before the first Compile.
	pre := &Scene{}
	pre.Add(NewSphere(Vector{X: 0, Y: 0, Z: 0}, 1))
	pre.Add(NewSphere(Vector{X: 3, Y: 0, Z: 0}, 1))
	c.Check(pre.Tree == nil, "tree should not exist before Compile")
	c.Check(pre.ReplaceShape(0, NewSphere(Vector{X: -2, Y: 0, Z: 0}, 0.5)) == nil, "replace before compile failed")
	pre.Compile()
	checkIndex(c, pre)
	rng := rand.New(rand.NewSource(31))
	rays := make([]Ray, 0, 40)
	for i := 0; i < 40; i++ {
		rays = append(rays, randomRay(rng))
	}
	checkQueries(c, pre, rays)

	// Compile twice is harmless and keeps the same tree.
	tree := pre.Tree
	pre.Compile()
	c.Check(pre.Tree == tree, "repeat Compile replaced the tree")
	checkIndex(c, pre)
}

// ---------------------------------------------------------------------------
// TestTraversal: ray correctness under awkward directions and origins.

func TestTraversal(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		scene := gridScene(80, true)
		scene.Compile()

		rng := rand.New(rand.NewSource(99))
		var rays []Ray
		for i := 0; i < 300; i++ {
			rays = append(rays, randomRay(rng))
		}

		// Rays parallel to each axis (zero direction components).
		for i := 0; i < 100; i++ {
			origin := Vector{rng.Float64()*160 - 20, rng.Float64()*6 - 3, rng.Float64()*6 - 3}
			rays = append(rays,
				Ray{origin, Vector{X: 1}},
				Ray{origin, Vector{X: -1}},
				Ray{Vector{origin.X, origin.Y, 50}, Vector{Z: -1}},
				Ray{Vector{origin.X, 0, origin.Z}, Vector{Y: 1}},
			)
		}

		// Origins exactly on split planes, both along and across them.
		var splits []struct {
			axis  Axis
			point float64
		}
		walkNodes(scene.Tree.Root, func(n *Node) {
			if n.Axis != AxisNone && len(splits) < 12 {
				splits = append(splits, struct {
					axis  Axis
					point float64
				}{n.Axis, n.Point})
			}
		})
		c.Check(len(splits) > 0, "expected at least one split plane")
		for _, sp := range splits {
			origin := Vector{X: 20, Y: 0.05, Z: 0.05}
			switch sp.axis {
			case AxisX:
				origin.X = sp.point
			case AxisY:
				origin.Y = sp.point
			case AxisZ:
				origin.Z = sp.point
			}
			for _, dir := range []Vector{
				{X: 1}, {X: -1}, {Y: 1}, {Y: -1}, {Z: 1}, {Z: -1},
			} {
				rays = append(rays, Ray{origin, dir})
			}
		}

		checkQueries(c, scene, rays)

		// Exact ties are resolved by the smaller slot number.
		tie := &Scene{}
		tie.Add(NewSphere(Vector{X: 0, Y: 0, Z: 0}, 1))
		tie.Add(NewSphere(Vector{X: 0, Y: 0, Z: 0}, 1))
		tie.Add(NewSphere(Vector{X: 0, Y: 0, Z: 0}, 1))
		tie.Compile()
		ray := Ray{Vector{X: 0, Y: 0, Z: 5}, Vector{Z: -1}}
		hit := tie.Intersect(ray)
		c.Check(hit.Ok(), "tie scene not hit")
		c.Check(sameShape(hit.Shape, tie.Shapes[0]), "tie should pick the smaller slot")
		// Swap slots 0 and 2 so a different shape owns the smaller slot.
		third := tie.Shapes[2]
		c.Check(tie.ReplaceShape(2, NewSphere(Vector{X: 0, Y: 0, Z: 0}, 1)) == nil, "replace failed")
		hit = tie.Intersect(ray)
		c.Check(hit.Ok(), "tie scene not hit after replace")
		c.Check(sameShape(hit.Shape, tie.Shapes[0]), "tie should still pick slot 0")
		_ = third

		// A hit outside the leaf range must not be cached as NoHit. The long
		// object is hit far from the leaf that owns the entry near the origin.
		long := &countingShape{inner: NewCube(
			Vector{X: -50, Y: -0.1, Z: -0.1}, Vector{X: 200, Y: 0.1, Z: 0.1},
		)}
		scene.Add(long)
		scene.Compile()
		checkIndex(c, scene)
		long.calls = 0
		farRay := Ray{Origin: Vector{X: -1000, Y: 0, Z: 0}, Direction: Vector{X: 1}}
		got := scene.Intersect(farRay)
		calls := long.Count()
		want := bruteNearest(scene, farRay)
		c.Check(sameHit(got, want), "far hit mismatch: %v vs %v", got.T, want.T)
		c.Check(calls <= 1, "multi-leaf slot queried %d times", calls)

		// Move an object out of the original root box and query there.
		c.Check(scene.ReplaceShape(0, NewSphere(Vector{X: -500, Y: 0, Z: 0}, 0.5)) == nil, "replace failed")
		checkIndex(c, scene)
		outside := Ray{Origin: Vector{X: -500, Y: 0, Z: 10}, Direction: Vector{Z: -1}}
		got = scene.Intersect(outside)
		want = bruteNearest(scene, outside)
		c.Check(sameHit(got, want), "outside hit mismatch: %v vs %v", got.T, want.T)
		c.Check(got.Ok(), "outside object not hit")
	})
}

// ---------------------------------------------------------------------------
// TestScene: concurrency, realistic edit cost, and the example's two frames.

func TestScene(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		testConcurrentQueries(c)
		testRealWorkload(c)
		testExampleFrames(c)
	})
}

func testConcurrentQueries(c *taskcheck.C) {
	scene := gridScene(120, true)
	scene.Compile()
	c.Check(scene.ReplaceShape(10, NewSphere(Vector{X: 33.3, Y: 1.2, Z: -0.4}, 0.9)) == nil, "replace failed")
	checkIndex(c, scene)

	rng := rand.New(rand.NewSource(123))
	var rays []Ray
	for i := 0; i < 128; i++ {
		rays = append(rays, randomRay(rng))
	}
	want := make([]Hit, len(rays))
	for i, r := range rays {
		want[i] = bruteNearest(scene, r)
	}

	var wg sync.WaitGroup
	var failures int64
	for g := 0; g < 8; g++ {
		wg.Add(1)
		go func(offset int) {
			defer wg.Done()
			for round := 0; round < 20; round++ {
				for i := range rays {
					idx := (i + offset) % len(rays)
					got := scene.Intersect(rays[idx])
					if !sameHit(got, want[idx]) {
						atomic.AddInt64(&failures, 1)
					}
				}
			}
		}(g)
	}
	wg.Wait()
	c.Check(failures == 0, "%d concurrent query mismatches", failures)

	// Interleave edits with queries on other goroutines; replacement must be
	// outside rendering, but readers still observe whole, valid states.
	c.Check(scene.ReplaceShape(20, NewSphere(Vector{X: 40, Y: 0, Z: 0}, 1.1)) == nil, "replace failed")
	checkIndex(c, scene)
}

func testRealWorkload(c *taskcheck.C) {
	const count = 4096
	scene := &Scene{}
	for i := 0; i < count; i++ {
		scene.Add(NewSphere(Vector{X: float64(i) * 0.5, Y: 0, Z: 0}, 0.05))
	}
	scene.Compile()
	checkIndex(c, scene)

	middle := count / 2
	replacement := NewSphere(Vector{X: float64(middle)*0.5 + 0.01, Y: 0, Z: 0}, 0.05)
	c.Check(scene.ReplaceShape(middle, replacement) == nil, "real workload replace failed")
	stats := scene.LastEditStats()
	c.Check(stats.NodeVisits > 0, "no nodes visited")
	c.Check(stats.ShapeReads > 0, "no shapes read")
	c.Check(stats.NodeVisits < 256, "NodeVisits too high: %d", stats.NodeVisits)
	c.Check(stats.ShapeReads < 256, "ShapeReads too high: %d", stats.ShapeReads)
	checkIndex(c, scene)

	// The replacement must be the object hit near its new position.
	ray := Ray{
		Origin:    Vector{X: float64(middle)*0.5 + 0.01, Y: 0, Z: 5},
		Direction: Vector{Z: -1},
	}
	hit := scene.Intersect(ray)
	c.Check(hit.Ok(), "replacement not hit")
	c.Check(sameShape(hit.Shape, replacement), "wrong shape hit after replacement")
	c.Check(math.Abs(hit.T-4.95) < 1e-9, "unexpected hit distance %v", hit.T)

	// Repeated edits in the same neighbourhood stay cheap and correct.
	worstVisits, worstReads := 0, 0
	for i := 0; i < 50; i++ {
		x := float64(middle)*0.5 + float64(i%7-3)*0.001
		c.Check(scene.ReplaceShape(middle, NewSphere(Vector{X: x, Y: 0, Z: 0}, 0.05)) == nil, "repeat replace failed")
		s := scene.LastEditStats()
		if s.NodeVisits > worstVisits {
			worstVisits = s.NodeVisits
		}
		if s.ShapeReads > worstReads {
			worstReads = s.ShapeReads
		}
	}
	c.Check(worstVisits < 256, "repeated NodeVisits too high: %d", worstVisits)
	c.Check(worstReads < 256, "repeated ShapeReads too high: %d", worstReads)
	checkIndex(c, scene)
}

// The dynamic example frame parameters, mirrored here so the test can pin
// both frames without importing package main.
const (
	exWidth  = 256.0
	exHeight = 256.0
)

var (
	exEye   = Vector{X: 0, Y: 0, Z: 12}
	exProbe = Vector{X: 0, Y: 0, Z: -2.8}
	exMoved = Vector{X: 5, Y: 0, Z: 0}
)

// exampleScene mirrors examples/dynamic.
func exampleScene() *Scene {
	scene := &Scene{}
	scene.Add(NewSphere(Vector{X: 0, Y: 0, Z: 0}, 1.2))
	scene.Add(NewSphere(Vector{X: 0, Y: 0, Z: -4}, 1.2))
	for i := 0; i < 8; i++ {
		x := -14 + float64(i)*4
		scene.Add(NewCube(Vector{X: x, Y: 7.7, Z: -2.3}, Vector{X: x + 0.6, Y: 8.3, Z: -1.7}))
	}
	return scene
}

func exampleRender(scene *Scene) Paths {
	return scene.Render(exEye, Vector{}, Vector{Y: 1}, exWidth, exHeight, 45, 0.1, 100, 0.05)
}

func testExampleFrames(c *taskcheck.C) {
	scene := exampleScene()
	scene.Compile()
	eye := exEye
	camera := Ray{Origin: eye, Direction: Vector{Z: -1}}

	// Frame one: the front sphere is still in place.
	before := exampleRender(scene)
	c.Check(len(before) == 45, "unexpected before-frame path count: %d", len(before))
	hitBefore := scene.Intersect(camera)
	c.Check(hitBefore.Ok(), "before frame misses the front sphere")
	c.Check(math.Abs(hitBefore.T-10.8) < 1e-9, "before hit T=%v", hitBefore.T)
	c.Check(sameShape(hitBefore.Shape, scene.Shapes[0]), "before hit the wrong shape")
	c.Check(!scene.Visible(eye, exProbe), "probe should be hidden before the move")
	c.Check(sameHit(hitBefore, bruteNearest(scene, camera)), "before hit differs from reference")
	if len(before) > 0 && len(before[0]) > 0 {
		p := before[0][0]
		c.Check(math.Abs(p.X-158.97025665372846) < 1e-6, "before path X=%v", p.X)
		c.Check(math.Abs(p.Y-128) < 1e-6, "before path Y=%v", p.Y)
		p = before[0][len(before[0])/2]
		c.Check(math.Abs(p.X-97.02974334627154) < 1e-6, "before mid path X=%v", p.X)
	} else {
		c.Check(false, "before frame has no paths")
	}

	// Move slot 0 out of the line of sight.
	c.Check(scene.ReplaceShape(0, NewSphere(exMoved, 1.2)) == nil, "example replace failed")
	checkIndex(c, scene)

	after := exampleRender(scene)
	c.Check(len(after) == 70, "unexpected after-frame path count: %d", len(after))
	hitAfter := scene.Intersect(camera)
	c.Check(hitAfter.Ok(), "after frame misses the rear sphere")
	c.Check(math.Abs(hitAfter.T-14.8) < 1e-9, "after hit T=%v", hitAfter.T)
	c.Check(sameShape(hitAfter.Shape, scene.Shapes[1]), "after hit the wrong shape")
	c.Check(scene.Visible(eye, exProbe), "probe should be visible after the move")
	c.Check(sameHit(hitAfter, bruteNearest(scene, camera)), "after hit differs from reference")
	c.Check(!sameHit(hitBefore, hitAfter), "the two frames must differ in their ray hit")
	if len(after) > 0 && len(after[0]) > 0 {
		p := after[0][0]
		c.Check(math.Abs(p.X-234.93564600982677) < 1e-6, "after path X=%v", p.X)
		c.Check(math.Abs(p.Y-150.57564992573563) < 1e-6, "after path Y=%v", p.Y)
	} else {
		c.Check(false, "after frame has no paths")
	}

	// SVG export still works and both frames are present.
	svg := after.ToSVG(exWidth, exHeight)
	c.Check(len(svg) > 100, "SVG output too small")
}
