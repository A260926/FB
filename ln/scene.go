package ln

type Scene struct {
	Shapes []Shape
	Tree   *Tree
	gens   []uint64
	genOK  []bool
}

// shapeGeneration reports the geometry generation of shapes that can change
// over time (such as CageMesh). Static shapes report ok == false and are
// treated as constant.
func shapeGeneration(s Shape) (uint64, bool) {
	if v, ok := s.(interface{ Generation() uint64 }); ok {
		return v.Generation(), true
	}
	return 0, false
}

// shapesChanged reports whether any shape's geometry generation differs from
// the snapshot taken when the current tree was built.
func (s *Scene) shapesChanged() bool {
	if len(s.gens) != len(s.Shapes) {
		return true
	}
	for i, shape := range s.Shapes {
		g, ok := shapeGeneration(shape)
		if ok != s.genOK[i] || (ok && g != s.gens[i]) {
			return true
		}
	}
	return false
}

func (s *Scene) snapshotGenerations() {
	s.gens = make([]uint64, len(s.Shapes))
	s.genOK = make([]bool, len(s.Shapes))
	for i, shape := range s.Shapes {
		s.gens[i], s.genOK[i] = shapeGeneration(shape)
	}
}

func (s *Scene) Compile() {
	for _, shape := range s.Shapes {
		shape.Compile()
	}
	if s.Tree == nil || s.shapesChanged() {
		s.Tree = NewTree(s.Shapes)
		s.snapshotGenerations()
	}
}

func (s *Scene) Add(shape Shape) {
	s.Shapes = append(s.Shapes, shape)
}

func (s *Scene) Intersect(r Ray) Hit {
	if s.Tree == nil || s.shapesChanged() {
		s.Compile()
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
