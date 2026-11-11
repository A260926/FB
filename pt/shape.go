package pt

type Shape interface {
	Compile()
	BoundingBox() Box
	Intersect(Ray) Hit
	UV(Vector) Vector
	NormalAt(Vector) Vector
	MaterialAt(Vector) Material
}

type TransformedShape struct {
	Shape
	Matrix  Matrix
	Inverse Matrix
}

func NewTransformedShape(s Shape, m Matrix) Shape {
	return &TransformedShape{s, m, m.Inverse()}
}

func (s *TransformedShape) BoundingBox() Box {
	return s.Matrix.MulBox(s.Shape.BoundingBox())
}

func (s *TransformedShape) Intersect(r Ray) Hit {
	shapeRay := s.Inverse.MulRay(r)
	hit := s.Shape.Intersect(shapeRay)
	if !hit.Ok() {
		return hit
	}
	if hit.HitInfo != nil {
		// The child already resolved shading data against the leaf
		// primitive; transform that information instead of reinterpreting
		// the leaf in this wrapper's coordinate frame.
		inner := *hit.HitInfo
		position := s.Matrix.MulPosition(inner.Position)
		normal := s.Inverse.Transpose().MulDirection(inner.Normal)
		if normal.Dot(r.Direction) > 0 {
			normal = normal.Negate()
		}
		normal = normal.Normalize()
		ray := Ray{position, normal}
		info := HitInfo{inner.Shape, position, normal, ray, inner.Material, inner.Inside}
		hit.T = position.Sub(r.Origin).Length()
		hit.HitInfo = &info
		return hit
	}
	shape := hit.Shape
	shapePosition := shapeRay.Position(hit.T)
	shapeNormal := shape.NormalAt(shapePosition)
	position := s.Matrix.MulPosition(shapePosition)
	normal := s.Inverse.Transpose().MulDirection(shapeNormal)
	material := MaterialAt(shape, shapePosition)
	inside := false
	if shapeNormal.Dot(shapeRay.Direction) > 0 {
		normal = normal.Negate()
		inside = true
	}
	ray := Ray{position, normal}
	info := HitInfo{shape, position, normal, ray, material, inside}
	hit.T = position.Sub(r.Origin).Length()
	hit.HitInfo = &info
	return hit
}
