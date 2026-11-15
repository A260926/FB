package ln

import (
	"errors"
	"math"
	"reflect"
)

// EditStats reports the work done by the most recent Scene.ReplaceShape.
// NodeVisits counts every tree node visited while detaching the old slot and
// attaching the replacement. ShapeReads counts every stored shape record
// examined while recomputing aggregate bounds (including the replacement's
// bounding box validation).
type EditStats struct {
	NodeVisits int
	ShapeReads int
}

var (
	errReplaceIndex  = errors.New("ln: ReplaceShape index out of range")
	errReplaceShape  = errors.New("ln: ReplaceShape requires a non-nil shape")
	errReplaceBounds = errors.New("ln: ReplaceShape requires a finite bounding box with Min <= Max")
)

// LastEditStats returns the statistics for the most recent successful
// ReplaceShape call.
func (s *Scene) LastEditStats() EditStats {
	return s.lastEdit
}

// ReplaceShape replaces the shape in slot index with replacement. The index
// is the slot number assigned by Add and remains stable for the lifetime of
// the scene. The old shape immediately stops taking part in Intersect and
// Paths; other slots are untouched.
//
// When a tree has already been compiled it is updated in place: the Tree
// pointer, node links, Axis and Point values are preserved. Invalid input
// leaves both the scene and its tree untouched.
func (s *Scene) ReplaceShape(index int, replacement Shape) error {
	if index < 0 || index >= len(s.Shapes) {
		return errReplaceIndex
	}
	if replacement == nil || isNilShape(replacement) {
		return errReplaceShape
	}
	// Compile before reading bounds, matching the order used by Scene.Compile.
	replacement.Compile()
	box := replacement.BoundingBox()
	if !finiteBox(box) {
		return errReplaceBounds
	}
	stats := &EditStats{ShapeReads: 1}
	if s.Tree != nil {
		s.Tree.replaceSlot(index, replacement, box, stats)
	}
	s.Shapes[index] = replacement
	s.lastEdit = *stats
	return nil
}

func (s *Scene) addToTree(shape Shape) {
	if s.Tree == nil {
		return
	}
	slot := len(s.Shapes) - 1
	box := shape.BoundingBox()
	s.Tree.appendSlot(slot, shape, box)
}

func isNilShape(shape Shape) bool {
	v := reflect.ValueOf(shape)
	switch v.Kind() {
	case reflect.Ptr, reflect.Map, reflect.Slice, reflect.Func, reflect.Chan, reflect.Interface:
		return v.IsNil()
	}
	return false
}

func finiteBox(box Box) bool {
	values := [...]float64{
		box.Min.X, box.Min.Y, box.Min.Z,
		box.Max.X, box.Max.Y, box.Max.Z,
	}
	for _, v := range values {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return false
		}
	}
	return box.Min.X <= box.Max.X && box.Min.Y <= box.Max.Y && box.Min.Z <= box.Max.Z
}

// replaceSlot detaches the current entry for slot and inserts replacement,
// following only the old membership, the new coverage and their ancestors.
func (tree *Tree) replaceSlot(slot int, replacement Shape, box Box, stats *EditStats) {
	if tree.Root == nil {
		tree.Root = newNodeFromEntries(nil)
		tree.entries = nil
	}

	// Detach the old entry from every leaf that owns the slot.
	leaves := tree.slotLeaves[slot]
	for leaf := range leaves {
		if stats != nil {
			stats.NodeVisits++
		}
		if leaf.removeSlotEntry(slot, stats) {
			updateAncestors(leaf, stats)
		}
	}
	delete(tree.slotLeaves, slot)

	// Attach the replacement by walking existing split planes.
	added := make([]*Node, 0, 4)
	tree.Root.insertEntry(nodeEntry{slot, replacement, box}, stats, &added)
	for _, leaf := range added {
		tree.registerLeaf(slot, leaf)
		updateAncestors(leaf, stats)
	}

	for slot >= len(tree.entries) {
		// Defensive: keep the slot table addressable even if shapes were
		// appended without going through Add.
		tree.entries = append(tree.entries, nodeEntry{slot: len(tree.entries)})
	}
	tree.entries[slot] = nodeEntry{slot, replacement, box}
	tree.updateBox()
}

// appendSlot adds a new slot to an already compiled tree. This is used when
// Add is called after Compile. It never rebuilds or re-splits the tree.
func (tree *Tree) appendSlot(slot int, shape Shape, box Box) {
	if tree.Root == nil {
		tree.Root = newNodeFromEntries(nil)
	}
	tree.entries = append(tree.entries, nodeEntry{slot, shape, box})
	added := make([]*Node, 0, 4)
	tree.Root.insertEntry(nodeEntry{slot, shape, box}, nil, &added)
	for _, leaf := range added {
		tree.registerLeaf(slot, leaf)
		updateAncestors(leaf, nil)
	}
	tree.updateBox()
}

// reindex appends entries for shape slots that were added without updating
// the tree, so Compile can catch up after Add.
func (tree *Tree) reindex(shapes []Shape) {
	for slot := len(tree.entries); slot < len(shapes); slot++ {
		tree.appendSlot(slot, shapes[slot], shapes[slot].BoundingBox())
	}
}
