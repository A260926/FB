package ln

import (
	"fmt"
)

// CageMesh is a deformable triangle surface driven by a tetrahedral control
// cage. It implements Shape and can be added to a Scene directly.
//
// The binding is constructed once from a source Mesh and a resting cage. It
// stores a copy of everything it needs, so later edits to the source mesh, the
// resting points or the cell indices do not change the binding or its pose.
//
// Surface triangles are split along resting cell boundaries at construction
// time. Pose only evaluates the cached barycentric coordinates, it never
// re-clips the source triangles.
type CageMesh struct {
	rest   []Vector
	cells  [][4]int
	facets []cageFacet
	edges  []cagePath

	generation uint64
	stats      CageBindingStats

	points    []Vector
	triangles []*Triangle
	paths     Paths
	box       Box
	tree      *Tree
}

// Generation returns the pose generation. It starts at 1 and is incremented by
// every successful Pose.
func (c *CageMesh) Generation() uint64 {
	return c.generation
}

// BindingStats returns construction statistics. It never changes after
// construction.
func (c *CageMesh) BindingStats() CageBindingStats {
	return c.stats
}

// Pose re-evaluates the surface from its resting source with the given cage
// point positions. If the update is rejected the previous triangles, paths,
// bounding box, tree and generation are left untouched.
func (c *CageMesh) Pose(points []Vector) error {
	if len(points) != len(c.rest) {
		return fmt.Errorf("ln: CageMesh.Pose: got %d points, want %d", len(points), len(c.rest))
	}
	for i, p := range points {
		if !finiteVector(p) {
			return fmt.Errorf("ln: CageMesh.Pose: point %d is not finite", i)
		}
	}
	for ci, cell := range c.cells {
		if !(cageVolume6(points, cell) > 0) {
			return fmt.Errorf("ln: CageMesh.Pose: cell %d no longer has positive volume", ci)
		}
	}
	c.apply(points, true)
	return nil
}

// cagePosition evaluates a stored barycentric vertex against the current cage
// point positions.
func cagePosition(pts []Vector, cell [4]int, w [4]float64) Vector {
	return pts[cell[0]].MulScalar(w[0]).
		Add(pts[cell[1]].MulScalar(w[1])).
		Add(pts[cell[2]].MulScalar(w[2])).
		Add(pts[cell[3]].MulScalar(w[3]))
}

// apply rebuilds the posed triangles, paths and bounds from the cached resting
// data. The previous state is only replaced once the new one has been fully
// computed.
func (c *CageMesh) apply(points []Vector, bump bool) {
	pts := make([]Vector, len(points))
	copy(pts, points)

	triangles := make([]*Triangle, 0, len(c.facets))
	for _, facet := range c.facets {
		t := NewTriangle(
			cagePosition(pts, c.cells[facet.v[0].cell], facet.v[0].w),
			cagePosition(pts, c.cells[facet.v[1].cell], facet.v[1].w),
			cagePosition(pts, c.cells[facet.v[2].cell], facet.v[2].w),
		)
		triangles = append(triangles, t)
	}

	paths := make(Paths, 0, len(c.edges))
	for _, edge := range c.edges {
		path := make(Path, 0, len(edge.pts))
		for _, pv := range edge.pts {
			path = append(path, cagePosition(pts, c.cells[pv.cell], pv.w))
		}
		paths = append(paths, path)
	}

	box := BoxForTriangles(triangles)

	c.points = pts
	c.triangles = triangles
	c.paths = paths
	c.box = box
	c.tree = nil
	if bump {
		c.generation++
	}
}

// Compile builds the acceleration structure for the current pose.
func (c *CageMesh) Compile() {
	if c.tree == nil {
		shapes := make([]Shape, len(c.triangles))
		for i, t := range c.triangles {
			shapes[i] = t
		}
		c.tree = NewTree(shapes)
	}
}

// BoundingBox returns the bounding box of the current posed surface.
func (c *CageMesh) BoundingBox() Box {
	return c.box
}

// Contains reports false; CageMesh is an open triangle surface.
func (c *CageMesh) Contains(v Vector, f float64) bool {
	return false
}

// Intersect tests the current posed surface. Hit.Shape is the CageMesh itself
// and Hit.T is the ray parameter for the original (possibly non-unit) ray.
func (c *CageMesh) Intersect(r Ray) Hit {
	if c.tree == nil {
		c.Compile()
	}
	hit := c.tree.Intersect(r)
	if hit.Ok() {
		hit.Shape = c
	}
	return hit
}

// Paths returns the current posed paths. There are exactly three paths per
// source triangle, in the source edge order, and a source edge stays one
// continuous path even when it crosses cell boundaries.
func (c *CageMesh) Paths() Paths {
	return c.paths
}

var _ Shape = (*CageMesh)(nil)
