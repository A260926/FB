package pt

import (
	"errors"
	"math"
	"reflect"
	"sort"
)

// CSGOp identifies a constructive solid geometry operation.
type CSGOp int

const (
	CSGUnion CSGOp = iota
	CSGIntersection
	CSGDifference
)

// CSGEpsilon is the distance below which two boundary parameters along a
// ray are treated as the same geometric boundary. It is small enough to
// keep solid layers as thin as 1e-5 intact.
const CSGEpsilon = 1e-7

// CSGShape evaluates an analytic Boolean combination of two solids. All
// per-ray work is kept in local variables, so a compiled shape is safe for
// concurrent Intersect calls from different rendering goroutines.
type CSGShape struct {
	Op    CSGOp
	Left  Shape
	Right Shape
	Box   Box
	empty bool
}

// NewCSG combines left and right with a Boolean operation. For
// CSGDifference the result is left minus right. The operands must be
// spheres, cubes, transformed solids or nested CSG shapes.
func NewCSG(op CSGOp, left, right Shape) (Shape, error) {
	if err := validateCSGOperand(left); err != nil {
		return nil, err
	}
	if err := validateCSGOperand(right); err != nil {
		return nil, err
	}
	switch op {
	case CSGUnion, CSGIntersection, CSGDifference:
	default:
		return nil, errors.New("pt: unknown CSG operation")
	}
	shape := &CSGShape{Op: op, Left: left, Right: right}
	shape.Box = csgBoundingBox(op, left.BoundingBox(), right.BoundingBox())
	shape.empty = csgBoxEmpty(shape.Box)
	return shape, nil
}

func validateCSGOperand(shape Shape) error {
	if shape == nil {
		return errors.New("pt: CSG operand is nil")
	}
	switch s := shape.(type) {
	case *Sphere, *Cube:
		return nil
	case *TransformedShape:
		return validateCSGOperand(s.Shape)
	case *CSGShape:
		return nil
	default:
		return errors.New("pt: unsupported CSG operand shape")
	}
}

func csgBoxEmpty(b Box) bool {
	return b.Max.X < b.Min.X || b.Max.Y < b.Min.Y || b.Max.Z < b.Min.Z
}

func csgBoundingBox(op CSGOp, left, right Box) Box {
	switch op {
	case CSGUnion:
		return left.Extend(right)
	case CSGIntersection:
		min := left.Min.Max(right.Min)
		max := left.Max.Min(right.Max)
		return Box{min, max}
	default:
		// The exposed surface of a difference never leaves the left
		// operand, so its box is a conservative bound.
		return left
	}
}

func (s *CSGShape) Compile() {
	s.Left.Compile()
	s.Right.Compile()
}

func (s *CSGShape) BoundingBox() Box {
	return s.Box
}

func (s *CSGShape) UV(p Vector) Vector { return p }

func (s *CSGShape) NormalAt(p Vector) Vector { return p }

func (s *CSGShape) MaterialAt(p Vector) Material { return Material{} }

// csgEvent is one occupancy toggle of a solid along a ray, expressed in
// world coordinates. Outward is the geometric outward normal (world frame)
// of the solid that owns the boundary; for a right-hand difference surface
// it has already been flipped toward the cavity.
type csgEvent struct {
	t        float64
	entering bool
	leaf     Shape
	position Vector
	outward  Vector
	material Material
}

func (s *CSGShape) Intersect(r Ray) Hit {
	if s.empty {
		return NoHit
	}
	// A conservative box rejection keeps the k-d tree effective; every
	// real surface of the result lies inside the box.
	if tmin, tmax := s.Box.Intersect(r); tmax < tmin || tmax <= EPS {
		return NoHit
	}
	events := csgIntervals(s, Identity(), r)
	if len(events) == 0 {
		return NoHit
	}
	inside := false
	for _, e := range events {
		if e.t <= EPS {
			inside = e.entering
		} else {
			break
		}
	}
	for _, e := range events {
		if e.t <= EPS {
			continue
		}
		normal := e.outward
		if normal.Dot(r.Direction) > 0 {
			// Hit from inside the solid: shading normal faces the ray.
			normal = normal.Negate()
		}
		info := HitInfo{
			Shape:    e.leaf,
			Position: e.position,
			Normal:   normal,
			Ray:      Ray{e.position, normal},
			Material: e.material,
			Inside:   inside,
		}
		return Hit{s, e.t, &info}
	}
	return NoHit
}

// csgIntervals returns the sorted occupancy toggles of shape along ray r.
// fromWorld is the world-from-local-frame matrix accumulated through any
// enclosing TransformedShape wrappers.
func csgIntervals(shape Shape, fromWorld Matrix, r Ray) []csgEvent {
	switch q := shape.(type) {
	case *CSGShape:
		if q.empty {
			return nil
		}
		left := csgIntervals(q.Left, fromWorld, r)
		right := csgIntervals(q.Right, fromWorld, r)
		return csgCombine(q.Op, left, right)
	case *TransformedShape:
		return csgIntervals(q.Shape, fromWorld.Mul(q.Matrix), r)
	default:
		return csgLeafIntervals(shape, fromWorld, r)
	}
}

// csgCombine merges the interval lists of two operand solids according to
// the Boolean operation. Coincident boundaries are evaluated as a single
// group, so the result does not depend on traversal order. Events at
// t <= EPS are kept (they encode rays starting inside a solid); callers
// skip them when locating the first visible boundary.
func csgCombine(op CSGOp, left, right []csgEvent) []csgEvent {
	tagged := make([]struct {
		side int
		e    csgEvent
	}, 0, len(left)+len(right))
	for _, e := range left {
		tagged = append(tagged, struct {
			side int
			e    csgEvent
		}{0, e})
	}
	for _, e := range right {
		tagged = append(tagged, struct {
			side int
			e    csgEvent
		}{1, e})
	}
	sort.SliceStable(tagged, func(i, j int) bool {
		a, b := tagged[i].e, tagged[j].e
		if a.t != b.t {
			return a.t < b.t
		}
		if tagged[i].side != tagged[j].side {
			return tagged[i].side < tagged[j].side
		}
		if a.entering != b.entering {
			return a.entering
		}
		return csgLeafKey(a.leaf) < csgLeafKey(b.leaf)
	})

	result := make([]csgEvent, 0, len(tagged))
	inside := [2]bool{}
	i := 0
	for i < len(tagged) {
		e0 := tagged[i].e
		j := i + 1
		for j < len(tagged) && tagged[j].e.t-e0.t <= CSGEpsilon {
			j++
		}
		before := inside
		after := inside
		for k := i; k < j; k++ {
			after[tagged[k].side] = tagged[k].e.entering
		}
		outBefore := csgOccupied(op, before[0], before[1])
		outAfter := csgOccupied(op, after[0], after[1])
		if outBefore != outAfter {
			pick := csgPickBoundary(op, tagged[i:j], before, after)
			outward := pick.e.outward
			if op == CSGDifference && pick.side == 1 {
				// Cavity wall: the right operand's outward normal points
				// into the solid, so reverse it for the resulting body.
				outward = outward.Negate()
			}
			result = append(result, csgEvent{
				t:        pick.e.t,
				entering: outAfter,
				leaf:     pick.e.leaf,
				position: pick.e.position,
				outward:  outward,
				material: pick.e.material,
			})
		}
		inside = after
		i = j
	}
	return result
}

// csgPickBoundary selects, deterministically, the operand surface that
// owns a Boolean boundary inside one coincident-event group. An operand
// event participates when flipping that operand's membership alone (with
// the other operand held at its post-group state) flips the output.
func csgPickBoundary(op CSGOp, group []struct {
	side int
	e    csgEvent
}, before, after [2]bool) struct {
	side int
	e    csgEvent
} {
	for _, g := range group {
		k := g.side
		other := 1 - k
		var outBefore, outAfter bool
		if k == 0 {
			outBefore = csgOccupied(op, before[0], after[other])
			outAfter = csgOccupied(op, after[0], after[other])
		} else {
			outBefore = csgOccupied(op, after[other], before[1])
			outAfter = csgOccupied(op, after[other], after[1])
		}
		if outBefore != outAfter {
			return g
		}
	}
	// Coincident external surfaces (both operands toggle together): any
	// source is geometrically equivalent and the group order is stable.
	return group[0]
}

func csgOccupied(op CSGOp, left, right bool) bool {
	switch op {
	case CSGUnion:
		return left || right
	case CSGIntersection:
		return left && right
	default:
		return left && !right
	}
}

// csgLeafKey gives leaf primitives a stable identity order that does not
// depend on tree traversal or goroutine scheduling.
func csgLeafKey(leaf Shape) uintptr {
	return reflect.ValueOf(leaf).Pointer()
}

// csgLeafIntervals solves the full slab/quadratic interval of one
// primitive in its local frame and returns both entry and exit events in
// world coordinates, so rays that start inside the solid are handled
// correctly. Tangent rays that never cross the interior produce nothing.
func csgLeafIntervals(leaf Shape, fromWorld Matrix, r Ray) []csgEvent {
	localRay := fromWorld.Inverse().MulRay(r)
	t0, t1, ok := csgPrimitiveInterval(leaf, localRay)
	if !ok || t1 <= EPS {
		return nil
	}
	normalMatrix := fromWorld.Inverse().Transpose()
	events := make([]csgEvent, 0, 2)
	for _, q := range []struct {
		t        float64
		entering bool
	}{{t0, true}, {t1, false}} {
		localPos := localRay.Position(q.t)
		worldPos := fromWorld.MulPosition(localPos)
		wt := worldPos.Sub(r.Origin).Dot(r.Direction)
		outward := normalMatrix.MulDirection(leaf.NormalAt(localPos)).Normalize()
		material := MaterialAt(leaf, localPos)
		events = append(events, csgEvent{
			t:        wt,
			entering: q.entering,
			leaf:     leaf,
			position: worldPos,
			outward:  outward,
			material: material,
		})
	}
	return events
}

// csgPrimitiveInterval returns the entry and exit ray parameters of a
// primitive in its local frame, independently of the shading Intersect
// path. Tangent rays that never cross the interior yield ok == false.
func csgPrimitiveInterval(leaf Shape, r Ray) (t0, t1 float64, ok bool) {
	switch q := leaf.(type) {
	case *Sphere:
		to := r.Origin.Sub(q.Center)
		b := to.Dot(r.Direction)
		c := to.Dot(to) - q.Radius*q.Radius
		d := b*b - c
		if d <= 0 {
			return 0, 0, false
		}
		d = math.Sqrt(d)
		return -b - d, -b + d, true
	case *Cube:
		n := q.Min.Sub(r.Origin).Div(r.Direction)
		f := q.Max.Sub(r.Origin).Div(r.Direction)
		n, f = n.Min(f), n.Max(f)
		t0 = math.Max(math.Max(n.X, n.Y), n.Z)
		t1 = math.Min(math.Min(f.X, f.Y), f.Z)
		if math.IsNaN(t0) || math.IsNaN(t1) || t0 >= t1 {
			return 0, 0, false
		}
		return t0, t1, true
	default:
		return 0, 0, false
	}
}
