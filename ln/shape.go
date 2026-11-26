package ln

type Shape interface {
	Compile()
	BoundingBox() Box
	Contains(Vector, float64) bool
	Intersect(Ray) Hit
	Paths() Paths
}

// generationShape is optionally implemented by shapes whose geometry can
// change without the caller rebuilding a Scene. Scene compares the reported
// generation and rebuilds its acceleration structure when it changes.
type generationShape interface {
	Generation() uint64
}

// shapeGeneration returns a stamp for a shape. Shapes without a generation
// are reported as zero.
func shapeGeneration(s Shape) uint64 {
	if g, ok := s.(generationShape); ok {
		return g.Generation()
	}
	return 0
}

type EmptyShape struct {
}

func (s *EmptyShape) Compile() {
}

func (s *EmptyShape) BoundingBox() Box {
	return Box{Vector{}, Vector{}}
}

func (s *EmptyShape) Contains(v Vector, f float64) bool {
	return false
}

func (s *EmptyShape) Intersect(r Ray) Hit {
	return NoHit
}

func (s *EmptyShape) Paths() Paths {
	return nil
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

func (s *TransformedShape) Contains(v Vector, f float64) bool {
	return s.Shape.Contains(s.Inverse.MulPosition(v), f)
}

func (s *TransformedShape) Intersect(r Ray) Hit {
	return s.Shape.Intersect(s.Inverse.MulRay(r))
}

func (s *TransformedShape) Paths() Paths {
	return s.Shape.Paths().Transform(s.Matrix)
}
