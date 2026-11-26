package ln

type Scene struct {
	Shapes []Shape
	Tree   *Tree

	// shapeCount is the number of shapes the cached tree was built from.
	shapeCount int
	// tracked holds the generation-tracked shapes present when the tree was
	// built, with their generations. It is empty for fully static scenes, so
	// the per-ray staleness check costs nothing there.
	tracked []Shape
	stamps  []uint64
}

// stale reports whether the cached tree no longer matches the shapes.
func (s *Scene) stale() bool {
	if s.Tree == nil || len(s.Shapes) != s.shapeCount {
		return true
	}
	for i, shape := range s.tracked {
		if shapeGeneration(shape) != s.stamps[i] {
			return true
		}
	}
	return false
}

// rebuild discards the cached tree and snapshots the current generations.
func (s *Scene) rebuild() {
	s.Tree = NewTree(s.Shapes)
	s.shapeCount = len(s.Shapes)
	s.tracked = s.tracked[:0]
	s.stamps = s.stamps[:0]
	for _, shape := range s.Shapes {
		if _, ok := shape.(generationShape); ok {
			s.tracked = append(s.tracked, shape)
			s.stamps = append(s.stamps, shapeGeneration(shape))
		}
	}
}

func (s *Scene) Compile() {
	for _, shape := range s.Shapes {
		shape.Compile()
	}
	if s.stale() {
		s.rebuild()
	}
}

func (s *Scene) Add(shape Shape) {
	s.Shapes = append(s.Shapes, shape)
	s.shapeCount = -1 // force a rebuild of the acceleration structure
}

func (s *Scene) Intersect(r Ray) Hit {
	if s.stale() {
		s.rebuild()
	}
	return s.Tree.Intersect(r)
}

func (s *Scene) Visible(eye, point Vector) bool {
	v := eye.Sub(point)
	r := Ray{point, v.Normalize()}
	hit := s.Intersect(r)
	return hit.T >= v.Length()
}

func (s *Scene) Paths() Paths {
	var result Paths
	for _, shape := range s.Shapes {
		result = append(result, shape.Paths()...)
	}
	return result
}

func (s *Scene) Render(eye, center, up Vector, width, height, fovy, near, far, step float64) Paths {
	aspect := width / height
	matrix := LookAt(eye, center, up)
	matrix = matrix.Perspective(fovy, aspect, near, far)
	return s.RenderWithMatrix(matrix, eye, width, height, step)
}

func (s *Scene) RenderWithMatrix(matrix Matrix, eye Vector, width, height, step float64) Paths {
	s.Compile()
	paths := s.Paths()
	if step > 0 {
		paths = paths.Chop(step)
	}
	paths = paths.Filter(&ClipFilter{matrix, eye, s})
	if step > 0 {
		paths = paths.Simplify(1e-6)
	}
	matrix = Translate(Vector{1, 1, 0}).Scale(Vector{width / 2, height / 2, 0})
	paths = paths.Transform(matrix)
	return paths
}
