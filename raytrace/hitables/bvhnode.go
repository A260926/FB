package hitables

import (
	"math"
	"math/rand"
	"sort"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type BvhNode struct {
	box   *raytrace.AABB
	left  raytrace.Hitable
	right raytrace.Hitable
}

func NewBvhNode(hitables []raytrace.Hitable, time0 float64, time1 float64) *BvhNode {
	axis := int32(3.0 * rand.Float64())
	if axis == 0 {
		sort.Sort(ByX(hitables))
	} else if axis == 1 {
		sort.Sort(ByY(hitables))
	} else {
		sort.Sort(ByZ(hitables))
	}

	var left raytrace.Hitable
	var right raytrace.Hitable

	if len(hitables) == 1 {
		left = hitables[0]
		right = hitables[0]
	} else if len(hitables) == 2 {
		left = hitables[0]
		right = hitables[1]
	} else {
		hitablesLen := len(hitables)
		left = NewBvhNode(hitables[0:hitablesLen/2], time0, time1)
		right = NewBvhNode(hitables[hitablesLen/2:hitablesLen], time0, time1)
	}

	boxLeft := left.GetBoundingBox(time0, time1)
	boxRight := right.GetBoundingBox(time0, time1)

	box := combineBoxes(boxLeft, boxRight)

	return &BvhNode{box, left, right}
}

// combineBoxes merges two child bounds.  A nil or empty bound never enlarges
// the other child's box.
func combineBoxes(left *raytrace.AABB, right *raytrace.AABB) *raytrace.AABB {
	leftEmpty := left == nil || left.IsEmpty()
	rightEmpty := right == nil || right.IsEmpty()

	switch {
	case leftEmpty && rightEmpty:
		inf := math.Inf(1)
		return raytrace.NewAABB(mgl64.Vec3{inf, inf, inf}, mgl64.Vec3{-inf, -inf, -inf})
	case leftEmpty:
		return right
	case rightEmpty:
		return left
	default:
		return left.GetSurroundingBox(right)
	}
}

// Refit recomputes this node's bounding box bottom-up from its existing
// children.  It does not re-sort, build a replacement tree, or swap out leaf
// objects, so a shared PatchSet keeps its identity.
func (s *BvhNode) Refit(time0 float64, time1 float64) {
	if s == nil {
		return
	}
	leftNode, leftIsNode := s.left.(*BvhNode)
	rightNode, rightIsNode := s.right.(*BvhNode)
	if leftIsNode {
		leftNode.Refit(time0, time1)
	}
	if rightIsNode && rightNode != leftNode {
		rightNode.Refit(time0, time1)
	}

	var leftBox *raytrace.AABB
	if s.left != nil {
		leftBox = s.left.GetBoundingBox(time0, time1)
	}
	var rightBox *raytrace.AABB
	if s.right != nil {
		rightBox = s.right.GetBoundingBox(time0, time1)
	}
	s.box = combineBoxes(leftBox, rightBox)
}

func (s *BvhNode) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {

	// fmt.Printf("BvhNode Hit(): %#v\n", ray)

	if !s.box.Hit(ray, tMin, tMax) {
		return nil
	}

	hrLeft := s.left.Hit(ray, tMin, tMax)
	hrRight := s.right.Hit(ray, tMin, tMax)
	if hrLeft != nil && hrRight != nil {
		if hrLeft.T() < hrRight.T() {
			return hrLeft
		}
		return hrRight
	} else if hrLeft != nil {
		return hrLeft
	} else if hrRight != nil {
		return hrRight
	}
	return nil
}

func (s *BvhNode) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	return s.box
}

func (s *BvhNode) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	return 1.0
}

func (s *BvhNode) Random(origin mgl64.Vec3) mgl64.Vec3 {
	return vectorextensions.UnitX()
}

type ByX []raytrace.Hitable

func (a ByX) Len() int      { return len(a) }
func (a ByX) Swap(i, j int) { a[i], a[j] = a[j], a[i] }
func (a ByX) Less(i, j int) bool {
	return a[i].GetBoundingBox(0, 0).Min().X() < a[j].GetBoundingBox(0, 0).Min().X()
}

type ByY []raytrace.Hitable

func (a ByY) Len() int      { return len(a) }
func (a ByY) Swap(i, j int) { a[i], a[j] = a[j], a[i] }
func (a ByY) Less(i, j int) bool {
	return a[i].GetBoundingBox(0, 0).Min().Y() < a[j].GetBoundingBox(0, 0).Min().Y()
}

type ByZ []raytrace.Hitable

func (a ByZ) Len() int      { return len(a) }
func (a ByZ) Swap(i, j int) { a[i], a[j] = a[j], a[i] }
func (a ByZ) Less(i, j int) bool {
	return a[i].GetBoundingBox(0, 0).Min().Z() < a[j].GetBoundingBox(0, 0).Min().Z()
}
