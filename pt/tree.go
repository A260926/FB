package pt

import (
	"fmt"
	"math"
	"sort"
)

// intersectMode selects which shapes a tree query is allowed to return.
type intersectMode int

const (
	// intersectVisible skips LinkedInstances whose Visible flag is false.
	intersectVisible intersectMode = iota
	// intersectSingle returns hits only from the one named shape.
	intersectSingle
	// intersectOccluders ignores the named light shape entirely and returns
	// the nearest hit among shapes allowed to cast a shadow toward it.
	intersectOccluders
)

// intersectQuery carries the parameters of a filtered tree traversal. It is
// passed explicitly through every node so that concurrent reads never share
// state through package-level variables.
type intersectQuery struct {
	mode      intersectMode
	target    Shape  // the single shape (intersectSingle) or target light
	lightMask uint64 // EmitMask of the target light
}

type Tree struct {
	Box  Box
	Root *Node
}

func NewTree(shapes []Shape) *Tree {
	fmt.Printf("Building k-d tree (%d shapes)... ", len(shapes))
	defer fmt.Println("OK")
	box := BoxForShapes(shapes)
	node := NewNode(shapes)
	node.Split(0)
	return &Tree{box, node}
}

func (tree *Tree) Intersect(r Ray) Hit {
	tmin, tmax := tree.Box.Intersect(r)
	if tmax < tmin || tmax <= 0 {
		return NoHit
	}
	return tree.Root.Intersect(r, tmin, tmax)
}

// IntersectQuery runs a filtered ray query against the tree, reusing the
// same splitting and pruning logic as Intersect.
func (tree *Tree) IntersectQuery(r Ray, query *intersectQuery) Hit {
	tmin, tmax := tree.Box.Intersect(r)
	if tmax < tmin || tmax <= 0 {
		return NoHit
	}
	return tree.Root.IntersectQuery(r, tmin, tmax, query)
}

type Node struct {
	Axis   Axis
	Point  float64
	Shapes []Shape
	Left   *Node
	Right  *Node
}

func NewNode(shapes []Shape) *Node {
	return &Node{AxisNone, 0, shapes, nil, nil}
}

func (node *Node) Intersect(r Ray, tmin, tmax float64) Hit {
	return node.IntersectQuery(r, tmin, tmax, nil)
}

func (node *Node) IntersectQuery(r Ray, tmin, tmax float64, query *intersectQuery) Hit {
	var tsplit float64
	var leftFirst bool
	switch node.Axis {
	case AxisNone:
		return node.IntersectShapesQuery(r, query)
	case AxisX:
		tsplit = (node.Point - r.Origin.X) / r.Direction.X
		leftFirst = (r.Origin.X < node.Point) || (r.Origin.X == node.Point && r.Direction.X <= 0)
	case AxisY:
		tsplit = (node.Point - r.Origin.Y) / r.Direction.Y
		leftFirst = (r.Origin.Y < node.Point) || (r.Origin.Y == node.Point && r.Direction.Y <= 0)
	case AxisZ:
		tsplit = (node.Point - r.Origin.Z) / r.Direction.Z
		leftFirst = (r.Origin.Z < node.Point) || (r.Origin.Z == node.Point && r.Direction.Z <= 0)
	}
	var first, second *Node
	if leftFirst {
		first = node.Left
		second = node.Right
	} else {
		first = node.Right
		second = node.Left
	}
	if tsplit > tmax || tsplit <= 0 {
		return first.IntersectQuery(r, tmin, tmax, query)
	} else if tsplit < tmin {
		return second.IntersectQuery(r, tmin, tmax, query)
	} else {
		h1 := first.IntersectQuery(r, tmin, tsplit, query)
		if h1.T <= tsplit {
			return h1
		}
		h2 := second.IntersectQuery(r, tsplit, math.Min(tmax, h1.T), query)
		if h1.T <= h2.T {
			return h1
		} else {
			return h2
		}
	}
}

func (node *Node) IntersectShapes(r Ray) Hit {
	return node.IntersectShapesQuery(r, nil)
}

// shapeAllowed reports whether a shape participates in a filtered query.
func shapeAllowed(shape Shape, query *intersectQuery) bool {
	if query == nil {
		return true
	}
	switch query.mode {
	case intersectSingle:
		return shape == query.target
	case intersectOccluders:
		// the target light never occludes itself; a different instance of
		// the same source remains a regular shape and is judged on its own
		if shape == query.target {
			return false
		}
		if instance, ok := shape.(*LinkedInstance); ok {
			// visibility does not matter for shadow casting, only CastMask
			return instance.links.CastMask&query.lightMask != 0
		}
		// unwrapped shapes carry every bit
		return true
	case intersectVisible:
		if instance, ok := shape.(*LinkedInstance); ok {
			return instance.links.Visible
		}
		return true
	}
	return true
}

func (node *Node) IntersectShapesQuery(r Ray, query *intersectQuery) Hit {
	hit := NoHit
	for _, shape := range node.Shapes {
		if !shapeAllowed(shape, query) {
			// a filtered shape is skipped but never ends the search: the
			// remaining shapes in this leaf can still block the ray
			continue
		}
		h := shape.Intersect(r)
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

func (node *Node) Split(depth int) {
	if len(node.Shapes) < 8 {
		return
	}
	xs := make([]float64, 0, len(node.Shapes)*2)
	ys := make([]float64, 0, len(node.Shapes)*2)
	zs := make([]float64, 0, len(node.Shapes)*2)
	for _, shape := range node.Shapes {
		box := shape.BoundingBox()
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
	best := int(float64(len(node.Shapes)) * 0.85)
	bestAxis := AxisNone
	bestPoint := 0.0
	sx := node.PartitionScore(AxisX, mx)
	if sx < best {
		best = sx
		bestAxis = AxisX
		bestPoint = mx
	}
	sy := node.PartitionScore(AxisY, my)
	if sy < best {
		best = sy
		bestAxis = AxisY
		bestPoint = my
	}
	sz := node.PartitionScore(AxisZ, mz)
	if sz < best {
		best = sz
		bestAxis = AxisZ
		bestPoint = mz
	}
	if bestAxis == AxisNone {
		return
	}
	l, r := node.Partition(best, bestAxis, bestPoint)
	node.Axis = bestAxis
	node.Point = bestPoint
	node.Left = NewNode(l)
	node.Right = NewNode(r)
	node.Left.Split(depth + 1)
	node.Right.Split(depth + 1)
	node.Shapes = nil // only needed at leaf nodes
}
