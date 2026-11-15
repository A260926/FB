package ln

import "math"

type Box struct {
	Min, Max Vector
}

func BoxForShapes(shapes []Shape) Box {
	if len(shapes) == 0 {
		return Box{}
	}
	box := shapes[0].BoundingBox()
	for _, shape := range shapes {
		box = box.Extend(shape.BoundingBox())
	}
	return box
}

func BoxForTriangles(shapes []*Triangle) Box {
	if len(shapes) == 0 {
		return Box{}
	}
	box := shapes[0].BoundingBox()
	for _, shape := range shapes {
		box = box.Extend(shape.BoundingBox())
	}
	return box
}

func BoxForVectors(vectors []Vector) Box {
	if len(vectors) == 0 {
		return Box{}
	}
	min := vectors[0]
	max := vectors[0]
	for _, v := range vectors {
		min = min.Min(v)
		max = max.Max(v)
	}
	return Box{min, max}
}

func (a Box) Anchor(anchor Vector) Vector {
	return a.Min.Add(a.Size().Mul(anchor))
}

func (a Box) Center() Vector {
	return a.Anchor(Vector{0.5, 0.5, 0.5})
}

func (a Box) Size() Vector {
	return a.Max.Sub(a.Min)
}

func (a Box) Contains(b Vector) bool {
	return a.Min.X <= b.X && a.Max.X >= b.X &&
		a.Min.Y <= b.Y && a.Max.Y >= b.Y &&
		a.Min.Z <= b.Z && a.Max.Z >= b.Z
}

func (a Box) Extend(b Box) Box {
	return Box{a.Min.Min(b.Min), a.Max.Max(b.Max)}
}

func (b *Box) Intersect(r Ray) (float64, float64) {
	t1 := math.Inf(-1)
	t2 := math.Inf(1)
	slab := func(origin, direction, min, max float64) bool {
		if direction == 0 {
			// The ray is parallel to this pair of planes. It either lies
			// between them for every t, or it misses the slab entirely.
			return origin >= min && origin <= max
		}
		a := (min - origin) / direction
		c := (max - origin) / direction
		if a > c {
			a, c = c, a
		}
		if a > t1 {
			t1 = a
		}
		if c < t2 {
			t2 = c
		}
		return true
	}
	if !slab(r.Origin.X, r.Direction.X, b.Min.X, b.Max.X) ||
		!slab(r.Origin.Y, r.Direction.Y, b.Min.Y, b.Max.Y) ||
		!slab(r.Origin.Z, r.Direction.Z, b.Min.Z, b.Max.Z) {
		return math.Inf(1), math.Inf(-1)
	}
	return t1, t2
}

func (b *Box) Partition(axis Axis, point float64) (left, right bool) {
	switch axis {
	case AxisX:
		left = b.Min.X <= point
		right = b.Max.X >= point
	case AxisY:
		left = b.Min.Y <= point
		right = b.Max.Y >= point
	case AxisZ:
		left = b.Min.Z <= point
		right = b.Max.Z >= point
	}
	return
}
