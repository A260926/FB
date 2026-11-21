package ln

import "fmt"

// CageMesh is a triangle mesh bound to a tetrahedral control cage. The source
// triangles are clipped along the rest cage's cell boundaries once at
// construction; Pose then evaluates the bound points affinely per cell from
// the posed cage vertices. CageMesh implements Shape, so it can be added to a
// Scene like any other shape; the scene picks up new poses on the next
// Compile or render without any manual tree invalidation.
type CageMesh struct {
	rest   []Vector   // own copy of the rest cage vertices
	cells  [][4]int   // own copy of the tetrahedral cells
	ccells []cageCell // precomputed rest-pose cell data

	slices []cageSlice // clipped sub-triangles in rest-pose barycentric form
	edges  []cageEdge  // original triangle edges as chains of bound points

	tris []*Triangle // posed slice triangles used for intersection
	cur  []Vector    // current posed cage vertices
	box  Box         // bounding box of the posed surface
	tree *Tree       // spatial index over tris, rebuilt when dirty

	gen   uint64
	stats CageBindingStats
}

// mapPoint evaluates a bound point with the current posed cage vertices.
func (m *CageMesh) mapPoint(p cagePoint) Vector {
	idx := m.cells[p.cell]
	return m.cur[idx[0]].MulScalar(p.bary[0]).
		Add(m.cur[idx[1]].MulScalar(p.bary[1])).
		Add(m.cur[idx[2]].MulScalar(p.bary[2])).
		Add(m.cur[idx[3]].MulScalar(p.bary[3]))
}

// applyPose recomputes the posed triangles and bounding box from m.cur.
func (m *CageMesh) applyPose() {
	for i, s := range m.slices {
		t := m.tris[i]
		t.V1 = m.mapPoint(s.v[0])
		t.V2 = m.mapPoint(s.v[1])
		t.V3 = m.mapPoint(s.v[2])
		t.UpdateBoundingBox()
	}
	m.box = BoxForTriangles(m.tris)
}

// Pose replaces the cage vertex positions and re-evaluates the bound surface
// from the rest-pose binding (it never accumulates on top of the previous
// pose). points must match the rest vertex count, be finite, and keep every
// cell's volume positive; otherwise an error is returned and the previous
// triangles, paths, bounding box, spatial tree and generation are kept.
func (m *CageMesh) Pose(points []Vector) error {
	if len(points) != len(m.rest) {
		return fmt.Errorf("ln: cage pose needs %d points, got %d", len(m.rest), len(points))
	}
	for i, p := range points {
		if !finiteVector(p) {
			return fmt.Errorf("ln: cage pose point %d is not finite", i)
		}
	}
	for i := range m.ccells {
		idx := m.ccells[i].indices
		p0, p1, p2, p3 := points[idx[0]], points[idx[1]], points[idx[2]], points[idx[3]]
		det := tetDet(p0, p1, p2, p3)
		scale := tetScale(p0, p1, p2, p3)
		if det*m.ccells[i].sign <= cageVolumeEps*scale*scale*scale {
			return fmt.Errorf("ln: cage pose leaves cell %d with non-positive volume", i)
		}
	}
	copy(m.cur, points)
	m.applyPose()
	m.tree = nil
	m.gen++
	return nil
}

// Generation returns the geometry generation: 1 after construction, plus one
// per successful Pose.
func (m *CageMesh) Generation() uint64 {
	return m.gen
}

// BindingStats returns construction-time binding statistics. Pose does not
// change them.
func (m *CageMesh) BindingStats() CageBindingStats {
	return m.stats
}

// Compile builds the spatial index over the posed triangles if it is dirty.
func (m *CageMesh) Compile() {
	if m.tree == nil {
		shapes := make([]Shape, len(m.tris))
		for i, t := range m.tris {
			shapes[i] = t
		}
		m.tree = NewTree(shapes)
	}
}

// BoundingBox returns the box of the currently posed surface.
func (m *CageMesh) BoundingBox() Box {
	return m.box
}

// Contains always returns false, matching Mesh.
func (m *CageMesh) Contains(v Vector, f float64) bool {
	return false
}

// Intersect finds the nearest intersection with the posed surface. The
// returned Hit.Shape is the CageMesh itself and T is the original ray
// parameter, whether or not the ray direction is unit length.
func (m *CageMesh) Intersect(r Ray) Hit {
	if m.tree == nil {
		m.Compile()
	}
	h := m.tree.Intersect(r)
	if !h.Ok() {
		return NoHit
	}
	return Hit{m, h.T}
}

// Paths returns the outline of the source triangles: for each source
// triangle, in order, one continuous path per original edge. Edges that cross
// cell boundaries contain bend points at the crossings. Slice diagonals and
// cage cut lines are never emitted.
func (m *CageMesh) Paths() Paths {
	var result Paths
	for _, e := range m.edges {
		if len(e.points) == 0 {
			continue
		}
		path := make(Path, len(e.points))
		for i, p := range e.points {
			path[i] = m.mapPoint(p)
		}
		result = append(result, path)
	}
	return result
}

// ensure CageMesh satisfies the Shape interface
var _ Shape = (*CageMesh)(nil)
