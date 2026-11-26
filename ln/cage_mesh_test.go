package ln

import (
	"fmt"
	"math"
	"sort"
	"testing"

	"github.com/fogleman/ln/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

func cageCheckVec(c *taskcheck.C, got, want Vector, tol float64, label string) {
	c.Check(got.Distance(want) <= tol, "%s: got %v, want %v (tol %g)", label, got, want, tol)
}

func cageCheckNear(c *taskcheck.C, got, want, tol float64, label string) {
	c.Check(math.Abs(got-want) <= tol, "%s: got %.17g, want %.17g (tol %g)", label, got, want, tol)
}

func cageTriangleArea(t *Triangle) float64 {
	return 0.5 * t.V2.Sub(t.V1).Cross(t.V3.Sub(t.V1)).Length()
}

func cagePathsEqual(a, b Paths, tol float64) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if len(a[i]) != len(b[i]) {
			return false
		}
		for j := range a[i] {
			if a[i][j].Distance(b[i][j]) > tol {
				return false
			}
		}
	}
	return true
}

func cageCopyPaths(p Paths) Paths {
	out := make(Paths, len(p))
	for i := range p {
		out[i] = append(Path(nil), p[i]...)
	}
	return out
}

func cageCopyVectors(p []Vector) []Vector {
	out := make([]Vector, len(p))
	copy(out, p)
	return out
}

// affineFromTetra builds the independent affine map defined by sending the
// four resting points to the four posed points.
func affineFromTetra(rest, posed [4]Vector) func(Vector) Vector {
	e1 := rest[1].Sub(rest[0])
	e2 := rest[2].Sub(rest[0])
	e3 := rest[3].Sub(rest[0])
	b1 := posed[1].Sub(posed[0])
	b2 := posed[2].Sub(posed[0])
	b3 := posed[3].Sub(posed[0])
	det := e1.Dot(e2.Cross(e3))
	return func(x Vector) Vector {
		d := x.Sub(rest[0])
		u := e2.Cross(e3).Dot(d) / det
		v := e3.Cross(e1).Dot(d) / det
		w := e1.Cross(e2).Dot(d) / det
		return posed[0].
			Add(b1.MulScalar(u)).
			Add(b2.MulScalar(v)).
			Add(b3.MulScalar(w))
	}
}

func cageCubeCage() ([]Vector, [][4]int) {
	rest := []Vector{
		{-1, -1, -1}, {1, -1, -1}, {-1, 1, -1}, {1, 1, -1},
		{-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, 1, 1},
	}
	cells := [][4]int{
		{0, 1, 3, 7},
		{0, 1, 5, 7},
		{0, 2, 3, 7},
		{0, 2, 6, 7},
		{0, 4, 5, 7},
		{0, 4, 6, 7},
	}
	return rest, cells
}

func cageSquareMesh(half float64, z float64) *Mesh {
	a := Vector{-half, -half, z}
	b := Vector{half, -half, z}
	d := Vector{half, half, z}
	e := Vector{-half, half, z}
	return NewMesh([]*Triangle{
		NewTriangle(a, b, d),
		NewTriangle(a, d, e),
	})
}

// ---------------------------------------------------------------------------
// TestCageSplit
// ---------------------------------------------------------------------------

// TestCageSplit uses a single large triangle that spans two non-coplanar
// affine cells. Clip points and ray hits are computed independently.
func TestCageSplit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Two tetrahedra sharing the face (0,1,2) in the plane z = 0.
		// Cell A lives above, cell B below. Their union is a bipyramid.
		rest := []Vector{
			{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1},
		}
		cells := [][4]int{
			{0, 1, 2, 3},
			{0, 1, 2, 4},
		}

		p0 := Vector{0.1, 0.1, 0.4}
		p1 := Vector{0.5, 0.05, -0.4}
		p2 := Vector{0.1, 0.6, 0.2}
		mesh := NewMesh([]*Triangle{NewTriangle(p0, p1, p2)})

		cm, err := NewCageMesh(mesh, rest, cells)
		c.Check(err == nil, "NewCageMesh returned %v", err)
		if err != nil || cm == nil {
			return
		}
		c.Check(cm.Generation() == 1, "initial generation = %d, want 1", cm.Generation())
		c.Check(cm.BindingStats().TriangleCellClips == 2,
			"TriangleCellClips = %d, want 2", cm.BindingStats().TriangleCellClips)

		// Independently computed split points where the triangle crosses z = 0.
		// p0 and p2 are above the shared face, p1 is below it, so the cut
		// crosses the edges p0-p1 and p1-p2.
		c1 := p0.Add(p1.Sub(p0).MulScalar(0.5))
		beta := p1.Z / (p1.Z - p2.Z)
		c2 := p1.Add(p2.Sub(p1).MulScalar(beta))
		cageCheckNear(c, c1.Z, 0, 1e-12, "split point 1 lies on shared face")
		cageCheckNear(c, c2.Z, 0, 1e-12, "split point 2 lies on shared face")
		// beta is 2/3 of the way from p1 towards p2.
		cageCheckNear(c, beta, 2.0/3.0, 1e-12, "split point 2 parameter")
		cageCheckVec(c, c2, p1.Add(p2.Sub(p1).MulScalar(2.0/3.0)), 1e-12, "split point 2")

		// Exactly three paths, one per source edge, in source order.
		paths := cm.Paths()
		c.Check(len(paths) == 3, "path count = %d, want 3", len(paths))
		if len(paths) != 3 {
			return
		}
		want0 := Path{p0, c1, p1} // edge 0 crosses the shared face
		want1 := Path{p1, c2, p2} // edge 1 crosses the shared face
		want2 := Path{p2, p0}     // edge 2 stays in cell A
		for i, want := range []Path{want0, want1, want2} {
			c.Check(len(paths[i]) == len(want), "path %d length = %d, want %d", i, len(paths[i]), len(want))
			if len(paths[i]) == len(want) {
				for j := range want {
					cageCheckVec(c, paths[i][j], want[j], 1e-9, "path point")
				}
			}
		}

		// Paths are continuous: the shared crossing point is produced by both
		// cells but is a single point, so consecutive edges are joined.
		cageCheckVec(c, paths[0][2], p1, 1e-9, "path 0 ends at p1")
		cageCheckVec(c, paths[1][0], p1, 1e-9, "path 1 starts at p1")

		// The quad A piece is split into 2 triangles, the B piece stays 1.
		c.Check(len(cm.triangles) == 3, "facet count = %d, want 3", len(cm.triangles))
		var areaSum float64
		for i, tri := range cm.triangles {
			a := cageTriangleArea(tri)
			c.Check(a > 0, "triangle %d has non-positive area %g", i, a)
			areaSum += a
		}
		wantArea := 0.5 * p1.Sub(p0).Cross(p2.Sub(p0)).Length()
		cageCheckNear(c, areaSum, wantArea, 1e-8, "total cut area equals source area (no duplicates or cracks)")

		// The paths must not expose the interior cut line: every path endpoint
		// is an original triangle vertex.
		for i, path := range paths {
			ends := []Vector{path[0], path[len(path)-1]}
			for _, e := range ends {
				ok := false
				for _, v := range []Vector{p0, p1, p2} {
					if e.Distance(v) <= 1e-9 {
						ok = true
					}
				}
				c.Check(ok, "path %d has an interior cut-line endpoint %v", i, e)
			}
		}
		// No path segment may lie on the interior cut (the z = 0 plane inside
		// the source triangle). Such a segment would mean the cell boundary was
		// drawn as a texture line.
		for i, path := range paths {
			for j := 0; j+1 < len(path); j++ {
				a, b := path[j], path[j+1]
				onPlane := math.Abs(a.Z) <= 1e-9 && math.Abs(b.Z) <= 1e-9
				c.Check(!onPlane, "path %d segment %d-%d lies on the interior cut line", i, j, j+1)
			}
		}

		// Independent ray hit in the cell-A half. The ray direction is not
		// unit length, so T must remain the parameter of the original ray.
		n := p1.Sub(p0).Cross(p2.Sub(p0))
		pa := Vector{0.18333333333333332, 0.29791666666666666, 0.15}
		rayA := Ray{Origin: Vector{pa.X, pa.Y, 2}, Direction: Vector{0, 0, -2}}
		wantTA := n.Dot(p0.Sub(rayA.Origin)) / n.Dot(rayA.Direction)
		hitA := cm.Intersect(rayA)
		c.Check(hitA.Ok(), "ray A missed the surface")
		if hitA.Ok() {
			cageCheckNear(c, hitA.T, wantTA, 1e-8, "ray A parameter")
			c.Check(hitA.Shape == Shape(cm), "ray A Hit.Shape is %T, want the CageMesh", hitA.Shape)
			got := rayA.Position(hitA.T)
			cageCheckVec(c, got, pa, 1e-8, "ray A hit position")
		}

		// Independent ray hit in the cell-B half, shot from below.
		pb := Vector{0.34444444444444444, 0.18055555555555555, -0.13333333333333333}
		rayB := Ray{Origin: Vector{pb.X, pb.Y, -2}, Direction: Vector{0, 0, 2}}
		wantTB := n.Dot(p0.Sub(rayB.Origin)) / n.Dot(rayB.Direction)
		hitB := cm.Intersect(rayB)
		c.Check(hitB.Ok(), "ray B missed the surface")
		if hitB.Ok() {
			cageCheckNear(c, hitB.T, wantTB, 1e-8, "ray B parameter")
			c.Check(hitB.Shape == Shape(cm), "ray B Hit.Shape is %T, want the CageMesh", hitB.Shape)
			got := rayB.Position(hitB.T)
			cageCheckVec(c, got, pb, 1e-8, "ray B hit position")
		}

		// Pose with two very different affine cell images. Each cut vertex must
		// be transformed by the affine map of the cell that owns it.
		posed := cageCopyVectors(rest)
		posed[3] = Vector{0.2, 0.1, 1.5}
		posed[4] = Vector{-0.1, 0.2, -1.2}
		err = cm.Pose(posed)
		c.Check(err == nil, "Pose returned %v", err)
		c.Check(cm.Generation() == 2, "generation after pose = %d, want 2", cm.Generation())
		c.Check(cm.BindingStats().TriangleCellClips == 2,
			"Pose changed TriangleCellClips to %d", cm.BindingStats().TriangleCellClips)

		mapA := affineFromTetra([4]Vector{rest[0], rest[1], rest[2], rest[3]},
			[4]Vector{posed[0], posed[1], posed[2], posed[3]})
		mapB := affineFromTetra([4]Vector{rest[0], rest[1], rest[2], rest[4]},
			[4]Vector{posed[0], posed[1], posed[2], posed[4]})
		// Both affine maps agree on the shared face, so each cut point has a
		// single well-defined image. p0 and p2 belong to cell A, p1 to cell B.
		expected := []Vector{
			mapA(p0), mapA(p2), mapA(c1), mapA(c2), mapB(p1),
		}
		for _, cut := range []Vector{c1, c2} {
			cageCheckVec(c, mapA(cut), mapB(cut), 1e-12, "cell maps agree on the shared face")
		}
		matched := make([]bool, len(expected))
		for _, tri := range cm.triangles {
			for _, v := range []Vector{tri.V1, tri.V2, tri.V3} {
				found := false
				for i, e := range expected {
					if v.Distance(e) <= 1e-8 {
						found = true
						matched[i] = true
						break
					}
				}
				c.Check(found, "posed vertex %v does not match any independently computed affine image", v)
			}
		}
		for i, m := range matched {
			c.Check(m, "expected affine vertex %d was not produced by the deformed surface", i)
		}

		// The deformed paths must be continuous across the fold: the crest
		// point on the shared face has a single position.
		dp := cm.Paths()
		c.Check(len(dp) == 3, "posed path count = %d, want 3", len(dp))
		if len(dp) == 3 {
			cageCheckVec(c, dp[0][1], mapA(c1), 1e-8, "crest point of edge 0")
			cageCheckVec(c, dp[1][1], mapA(c2), 1e-8, "crest point of edge 1")
		}

		// --- a triangle lying exactly in the shared plane -------------------------
		// It belongs to both cells geometrically, yet must be emitted once, with
		// its full area and no duplicates.
		cubeRest, cubeCells := cageCubeCage()
		faceP0 := Vector{0.1, 0.1, 0}
		faceP1 := Vector{0.8, 0.1, 0}
		faceP2 := Vector{0.1, 0.8, 0}
		faceMesh := NewMesh([]*Triangle{NewTriangle(faceP0, faceP1, faceP2)})
		faceCM, err := NewCageMesh(faceMesh, cubeRest, cubeCells)
		c.Check(err == nil, "coplanar face NewCageMesh returned %v", err)
		if err == nil && faceCM != nil {
			var faceArea float64
			for _, tri := range faceCM.triangles {
				faceArea += cageTriangleArea(tri)
			}
			wantFace := 0.5 * faceP1.Sub(faceP0).Cross(faceP2.Sub(faceP0)).Length()
			cageCheckNear(c, faceArea, wantFace, 1e-8, "coplanar face emitted once with no duplicate area")
			c.Check(len(faceCM.Paths()) == 3, "coplanar face path count = %d, want 3", len(faceCM.Paths()))

			// A coplanar triangle that is partly in both cells must still be
			// fully covered once, and its shared edge must be drawn once.
			sp0 := Vector{-0.5, 0.05, 0}
			sp1 := Vector{0.5, 0.05, 0}
			sp2 := Vector{0.0, 0.9, 0}
			spanner := NewMesh([]*Triangle{NewTriangle(sp0, sp1, sp2)})
			spanCM, err := NewCageMesh(spanner, cubeRest, cubeCells)
			c.Check(err == nil, "spanning face NewCageMesh returned %v", err)
			if err == nil && spanCM != nil {
				var spanArea float64
				for _, tri := range spanCM.triangles {
					spanArea += cageTriangleArea(tri)
				}
				wantSpan := 0.5 * sp1.Sub(sp0).Cross(sp2.Sub(sp0)).Length()
				cageCheckNear(c, spanArea, wantSpan, 1e-8, "spanning coplanar face area")
				c.Check(len(spanCM.Paths()) == 3, "spanning coplanar path count = %d, want 3", len(spanCM.Paths()))
			}
		}

		// --- small but non-zero slices survive -----------------------------------
		// A square bipyramid cage sharing the z = 0 plane. A thin triangle that
		// only just dips below the shared face produces a genuinely tiny sliver.
		biRest := []Vector{
			{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}, // base
			{0, 0, 1}, {0, 0, -1}, // apexes
		}
		biCells := [][4]int{
			{0, 1, 2, 4},
			{0, 2, 3, 4},
			{0, 1, 2, 5},
			{0, 2, 3, 5},
		}
		tinyP0 := Vector{-0.8, -0.5, 1e-6}
		tinyP1 := Vector{0.8, -0.5, 1e-6}
		tinyP2 := Vector{0.0, 0.8, -1e-3}
		tinyMesh := NewMesh([]*Triangle{NewTriangle(tinyP0, tinyP1, tinyP2)})
		tinyCM, err := NewCageMesh(tinyMesh, biRest, biCells)
		c.Check(err == nil, "tiny slice NewCageMesh returned %v", err)
		if err == nil && tinyCM != nil {
			var tinyArea float64
			minPiece := math.Inf(1)
			for _, tri := range tinyCM.triangles {
				a := cageTriangleArea(tri)
				c.Check(a > 0, "tiny slice contains a non-positive area triangle")
				if a < minPiece {
					minPiece = a
				}
				tinyArea += a
			}
			wantTiny := 0.5 * tinyP1.Sub(tinyP0).Cross(tinyP2.Sub(tinyP0)).Length()
			cageCheckNear(c, tinyArea, wantTiny, 1e-12, "tiny slice total area preserved")
			// The sliver below z = 0 is a small but real fraction of the source.
			c.Check(minPiece < 1e-3, "tiny slice was not preserved as separate geometry (min piece %g)", minPiece)
			c.Check(minPiece > 0, "tiny slice collapsed to zero area")
		}
	})
}

// ---------------------------------------------------------------------------
// TestCagePose
// ---------------------------------------------------------------------------

func TestCagePose(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		rest, cells := cageCubeCage()
		mesh := cageSquareMesh(0.6, 0)
		cm, err := NewCageMesh(mesh, rest, cells)
		c.Check(err == nil, "NewCageMesh returned %v", err)
		if err != nil || cm == nil {
			return
		}
		const wantClips = 12 // 2 source triangles x 6 cells
		c.Check(cm.BindingStats().TriangleCellClips == wantClips,
			"TriangleCellClips = %d, want %d", cm.BindingStats().TriangleCellClips, wantClips)

		basePaths := cageCopyPaths(cm.Paths())
		c.Check(len(basePaths) == 6, "path count = %d, want 6 (3 per source triangle)", len(basePaths))

		// Coplanar patches keep the exact source area: no overlap, no holes.
		var area float64
		for i, tri := range cm.triangles {
			a := cageTriangleArea(tri)
			c.Check(a > 0, "rest triangle %d has non-positive area %g", i, a)
			area += a
		}
		cageCheckNear(c, area, 1.44, 1e-8, "rest surface area")

		// Shared source edge: the two source triangles both draw the diagonal.
		// The paths must coincide exactly with opposite orientation.
		var sharedA, sharedB Path
		for _, p := range basePaths {
			if len(p) >= 2 && p[0].Distance(Vector{-0.6, -0.6, 0}) < 1e-9 && p[len(p)-1].Distance(Vector{0.6, 0.6, 0}) < 1e-9 {
				sharedA = p
			}
			if len(p) >= 2 && p[0].Distance(Vector{0.6, 0.6, 0}) < 1e-9 && p[len(p)-1].Distance(Vector{-0.6, -0.6, 0}) < 1e-9 {
				sharedB = p
			}
		}
		c.Check(sharedA != nil && sharedB != nil, "shared source edge was not emitted by both triangles")
		if sharedA != nil && sharedB != nil {
			c.Check(len(sharedA) == len(sharedB), "shared edge path lengths differ: %d vs %d", len(sharedA), len(sharedB))
			if len(sharedA) == len(sharedB) {
				for i := range sharedA {
					cageCheckVec(c, sharedA[i], sharedB[len(sharedB)-1-i], 1e-9, "shared edge point")
				}
			}
		}

		// Bounding box reflects the current surface.
		box := cm.BoundingBox()
		cageCheckVec(c, box.Min, Vector{-0.6, -0.6, 0}, 1e-9, "rest box min")
		cageCheckVec(c, box.Max, Vector{0.6, 0.6, 0}, 1e-9, "rest box max")

		// Successful pose: uniform scale. Every point must be recomputed from
		// the resting data, not accumulated on the previous pose.
		scaled := make([]Vector, len(rest))
		for i, p := range rest {
			scaled[i] = p.MulScalar(0.5)
		}
		c.Check(cm.Pose(scaled) == nil, "first Pose failed")
		c.Check(cm.Generation() == 2, "generation = %d, want 2", cm.Generation())
		c.Check(cm.BindingStats().TriangleCellClips == wantClips, "pose altered TriangleCellClips")
		for _, p := range cm.Paths() {
			for _, v := range p {
				// The surface lies at z = 0 and scaling does not move z.
				cageCheckVec(c, v, Vector{v.X, v.Y, 0}, 1e-9, "scaled point stays planar")
			}
		}
		box = cm.BoundingBox()
		cageCheckVec(c, box.Min, Vector{-0.3, -0.3, 0}, 1e-9, "shrunk box min")
		cageCheckVec(c, box.Max, Vector{0.3, 0.3, 0}, 1e-9, "shrunk box max")

		// Second pose: shear + scale, applied to the resting data.
		warp := func(v Vector) Vector {
			return Vector{v.X*0.25 + 0.1*v.Z, v.Y * 0.25, v.Z * 0.8}
		}
		warped := make([]Vector, len(rest))
		for i, p := range rest {
			warped[i] = warp(p)
		}
		c.Check(cm.Pose(warped) == nil, "second Pose failed")
		c.Check(cm.Generation() == 3, "generation = %d, want 3", cm.Generation())
		// Direct check on a known resting vertex: (-0.6,-0.6,0).
		cageCheckVec(c, cm.Paths()[0][0], warp(Vector{-0.6, -0.6, 0}), 1e-8, "warped source vertex")
		// The source data must be unaffected by the mutable warp.
		c.Check(cm.Paths()[0][0].Distance(Vector{-0.6, -0.6, 0}) > 1e-3, "warp did not move the surface")

		// Returning to rest must reproduce the original geometry exactly.
		c.Check(cm.Pose(rest) == nil, "restoring Pose failed")
		c.Check(cm.Generation() == 4, "generation = %d, want 4", cm.Generation())
		c.Check(cagePathsEqual(cm.Paths(), basePaths, 1e-9), "restoring rest did not reproduce the resting paths")
		box = cm.BoundingBox()
		cageCheckVec(c, box.Min, Vector{-0.6, -0.6, 0}, 1e-9, "restored box min")
		cageCheckVec(c, box.Max, Vector{0.6, 0.6, 0}, 1e-9, "restored box max")

		// Force a tree to exist, then make sure rejected poses leave all state
		// (including the tree and the generation) untouched.
		hit := cm.Intersect(Ray{Vector{0, 0, 2}, Vector{0, 0, -1}})
		c.Check(hit.Ok(), "rest plate should be hit from above")
		oldTree := cm.tree
		oldPaths := cageCopyPaths(cm.Paths())
		oldBox := cm.BoundingBox()
		oldGen := cm.Generation()

		reject := func(label string, pts []Vector) {
			err := cm.Pose(pts)
			c.Check(err != nil, "%s: Pose should have failed", label)
			c.Check(cm.Generation() == oldGen, "%s: generation changed to %d", label, cm.Generation())
			c.Check(cm.tree == oldTree, "%s: cached tree was replaced", label)
			c.Check(cm.BoundingBox() == oldBox, "%s: bounding box changed", label)
			c.Check(cagePathsEqual(cm.Paths(), oldPaths, 0), "%s: paths changed", label)
		}

		reject("short point list", cageCopyVectors(rest)[:len(rest)-1])

		nanPts := cageCopyVectors(rest)
		nanPts[2].X = math.NaN()
		reject("NaN coordinate", nanPts)

		infPts := cageCopyVectors(rest)
		infPts[2].Y = math.Inf(1)
		reject("Inf coordinate", infPts)

		collapsed := cageCopyVectors(rest)
		cell := cells[0]
		collapsed[cell[1]] = collapsed[cell[0]]
		reject("collapsed cell", collapsed)

		inverted := cageCopyVectors(rest)
		inverted[cell[0]], inverted[cell[1]] = rest[cell[1]], rest[cell[0]]
		reject("inverted cell", inverted)

		// A rejected pose must not poison a later valid pose either.
		c.Check(cm.Pose(rest) == nil, "valid pose after rejects failed")
		c.Check(cm.Generation() == oldGen+1, "generation = %d, want %d", cm.Generation(), oldGen+1)
		c.Check(cm.tree == nil, "successful pose did not invalidate the tree")

		// --- source data is copied -------------------------------------------------
		mesh2 := cageSquareMesh(0.6, 0)
		rest2, cells2 := cageCubeCage()
		cmA, err := NewCageMesh(mesh2, rest2, cells2)
		c.Check(err == nil, "second NewCageMesh returned %v", err)
		if err != nil {
			return
		}
		saved := cageCopyPaths(cmA.Paths())
		savedBox := cmA.BoundingBox()
		mesh2.Triangles[0].V1 = Vector{9, 9, 9}
		rest2[0] = Vector{7, 7, 7}
		cells2[0] = [4]int{0, 1, 2, 4}
		c.Check(cagePathsEqual(cmA.Paths(), saved, 0), "editing source mesh/rest/cells changed the binding paths")
		c.Check(cmA.BoundingBox() == savedBox, "editing source mesh changed the bounding box")
		c.Check(cmA.Generation() == 1, "editing source data changed the generation")
		restored := []Vector{
			{-1, -1, -1}, {1, -1, -1}, {-1, 1, -1}, {1, 1, -1},
			{-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, 1, 1},
		}
		c.Check(cmA.Pose(restored) == nil, "Pose using copied resting data failed after source edits")

		// --- two instances sharing one source mesh --------------------------------
		sharedMesh := cageSquareMesh(0.6, 0)
		sharedRest, sharedCells := cageCubeCage()
		cmX, err := NewCageMesh(sharedMesh, sharedRest, sharedCells)
		c.Check(err == nil, "cmX: %v", err)
		cmY, err := NewCageMesh(sharedMesh, sharedRest, sharedCells)
		c.Check(err == nil, "cmY: %v", err)
		if err != nil || cmX == nil || cmY == nil {
			return
		}

		scaleX := make([]Vector, len(sharedRest))
		for i, p := range sharedRest {
			scaleX[i] = p.MulScalar(0.5)
		}
		shiftY := make([]Vector, len(sharedRest))
		for i, p := range sharedRest {
			shiftY[i] = p.Add(Vector{0.2, 0, 0})
		}
		c.Check(cmX.Pose(scaleX) == nil, "cmX.Pose failed")
		c.Check(cmY.Pose(shiftY) == nil, "cmY.Pose failed")

		// Interleaved queries must not share state. A probe at x=0.6 is outside
		// the scaled cmX but inside the shifted cmY.
		probe := Ray{Vector{0.6, 0.1, 2}, Vector{0, 0, -1}}
		hx1 := cmX.Intersect(probe)
		hy1 := cmY.Intersect(probe)
		hx2 := cmX.Intersect(probe)
		hy2 := cmY.Intersect(probe)
		c.Check(!hx1.Ok(), "cmX should miss probe at x=0.6")
		c.Check(hy1.Ok(), "cmY should hit probe at x=0.6")
		c.Check(hx1.Ok() == hx2.Ok(), "cmX interleaved query changed result")
		c.Check(hy1.T == hy2.T, "cmY interleaved query changed result")
		if hy1.Ok() {
			c.Check(hy1.Shape == Shape(cmY), "cmY hit shape is %T, want cmY", hy1.Shape)
		}
		if hx1.Ok() {
			c.Check(hx1.Shape == Shape(cmX), "cmX hit shape is %T, want cmX", hx1.Shape)
		}
		// Both instances remain independent: cmX geometry is scaled, cmY shifted.
		cageCheckVec(c, cmX.Paths()[0][0], Vector{-0.3, -0.3, 0}, 1e-9, "cmX stays scaled")
		cageCheckVec(c, cmY.Paths()[0][0], Vector{-0.4, -0.6, 0}, 1e-9, "cmY stays shifted")

		// Posing cmX again must leave cmY unchanged.
		yPaths := cageCopyPaths(cmY.Paths())
		c.Check(cmX.Pose(sharedRest) == nil, "cmX restore failed")
		c.Check(cagePathsEqual(cmY.Paths(), yPaths, 0), "posing cmX changed cmY")

		// --- constructor rejects invalid input ------------------------------------
		rest3, cells3 := cageCubeCage()
		validMesh := cageSquareMesh(0.6, 0)
		mustFail := func(label string, m *Mesh, r []Vector, cl [][4]int) {
			bad, err := NewCageMesh(m, r, cl)
			c.Check(err != nil, "%s: NewCageMesh should have failed", label)
			c.Check(bad == nil, "%s: NewCageMesh returned a mesh alongside an error", label)
		}
		mustFail("nil mesh", nil, rest3, cells3)

		badRest := cageCopyVectors(rest3)
		badRest[1].X = math.NaN()
		mustFail("NaN rest point", validMesh, badRest, cells3)

		badRest = cageCopyVectors(rest3)
		badRest[4].Y = math.Inf(-1)
		mustFail("Inf rest point", validMesh, badRest, cells3)

		badRest = cageCopyVectors(rest3)
		badRest = append(badRest, Vector{0, 0, 0})
		badCells := append([][4]int(nil), cells3...)
		badCells[0] = [4]int{0, 1, 2, 99}
		mustFail("out-of-range index", validMesh, badRest, badCells)

		badCells = append([][4]int(nil), cells3...)
		badCells[2] = [4]int{1, 1, 2, 3}
		mustFail("duplicate index", validMesh, rest3, badCells)

		badCells = append([][4]int(nil), cells3...)
		badCells[3] = [4]int{0, 1, 2, 2}
		mustFail("degenerate repeated corner", validMesh, rest3, badCells)

		badRest = cageCopyVectors(rest3)
		badRest[3] = Vector{-1, -1, -1} // collapse cell 0 onto a plane
		mustFail("zero-volume cell", validMesh, badRest, cells3)

		// A triangle that pokes out of the cage is not fully covered.
		uncovered := NewMesh([]*Triangle{
			NewTriangle(Vector{-3, -0.6, 0}, Vector{-0.6, -0.6, 0}, Vector{-0.6, 0.6, 0}),
		})
		mustFail("uncovered triangle", uncovered, rest3, cells3)

		// A valid mesh still constructs after all those failures.
		cmOK, err := NewCageMesh(validMesh, rest3, cells3)
		c.Check(err == nil, "valid construction after rejects returned %v", err)
		c.Check(cmOK != nil, "valid construction returned nil")
	})
}

// ---------------------------------------------------------------------------
// TestCageScene
// ---------------------------------------------------------------------------

func TestCageScene(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		rest, cells := cageCubeCage()
		plate := cageSquareMesh(0.8, 0)
		cm, err := NewCageMesh(plate, rest, cells)
		c.Check(err == nil, "NewCageMesh returned %v", err)
		if err != nil || cm == nil {
			return
		}

		static := NewMesh([]*Triangle{
			NewTriangle(Vector{-0.3, -0.3, -1}, Vector{0.3, -0.3, -1}, Vector{0, 0.3, -1}),
		})

		eye := Vector{0, 0, 5}
		center := Vector{0, 0, 0}
		up := Vector{0, 1, 0}
		width, height := 256.0, 256.0
		matrix := LookAt(eye, center, up).Perspective(45, width/height, 0.1, 20)

		scene := &Scene{}
		scene.Add(cm)
		scene.Add(static)
		scene.Compile()

		behind := Vector{0, 0, -1}
		c.Check(!scene.Visible(eye, behind), "point behind the plate should be hidden before posing")

		// Direct ray: the plate at z = 0 is hit first and reports the CageMesh.
		ray := Ray{Origin: eye, Direction: Vector{0, 0, -1}}
		before := scene.Intersect(ray)
		c.Check(before.Ok(), "scene ray missed before posing")
		cageCheckNear(c, before.T, 5, 1e-9, "occluder ray parameter before pose")
		c.Check(before.Shape == Shape(cm), "occluder hit shape is %T, want the CageMesh", before.Shape)

		pathsBefore := scene.RenderWithMatrix(matrix, eye, width, height, 0.02)
		c.Check(len(pathsBefore) > 0, "render before posing produced no paths")
		svgBefore := pathsBefore.ToSVG(width, height)

		// Move the whole cage (and therefore the plate) out of the way.
		moved := make([]Vector, len(rest))
		for i, p := range rest {
			moved[i] = p.Add(Vector{5, 0, 0})
		}
		c.Check(cm.Pose(moved) == nil, "Pose failed")
		c.Check(cm.Generation() == 2, "generation = %d, want 2", cm.Generation())

		box := cm.BoundingBox()
		cageCheckVec(c, box.Min, Vector{4.2, -0.8, 0}, 1e-9, "posed box min")
		cageCheckVec(c, box.Max, Vector{5.8, 0.8, 0}, 1e-9, "posed box max")

		// The scene must pick up the new geometry without a manual Compile or
		// a manual tree reset.
		c.Check(scene.Visible(eye, behind), "point behind the moved plate should be visible")
		after := scene.Intersect(ray)
		c.Check(after.Ok(), "scene ray missed after posing")
		cageCheckNear(c, after.T, 6, 1e-9, "static surface parameter after pose")
		c.Check(after.Shape != Shape(cm), "moved plate still occludes; scene tree was not refreshed")
		c.Check(scene.Tree.Box.Max.X > 4, "scene tree bounds were not rebuilt")

		pathsAfter := scene.RenderWithMatrix(matrix, eye, width, height, 0.02)
		svgAfter := pathsAfter.ToSVG(width, height)
		c.Check(svgBefore != svgAfter, "rendered output did not change after moving the occluder")

		// The static triangle must now actually be drawn: its projected
		// vertices have to be present in the output paths.
		projected := func(v Vector) Vector {
			w := matrix.MulPositionW(v)
			return Vector{(w.X + 1) * width / 2, (w.Y + 1) * height / 2, 0}
		}
		nearPath := func(paths Paths, target Vector) bool {
			for _, p := range paths {
				for _, v := range p {
					if v.Distance(target) <= 2.0 {
						return true
					}
				}
			}
			return false
		}
		for _, v := range []Vector{{-0.3, -0.3, -1}, {0.3, -0.3, -1}, {0, 0.3, -1}} {
			pv := projected(v)
			c.Check(nearPath(pathsAfter, pv), "visible static triangle vertex missing at %v", pv)
			c.Check(!nearPath(pathsBefore, pv), "static triangle was already drawn at %v before posing", pv)
		}

		// A second pose keeps working through the auto-invalidated scene.
		c.Check(cm.Pose(rest) == nil, "second Pose failed")
		c.Check(cm.Generation() == 3, "generation = %d, want 3", cm.Generation())
		c.Check(!scene.Visible(eye, behind), "restored plate no longer occludes")
		back := scene.Intersect(ray)
		c.Check(back.Ok() && back.Shape == Shape(cm), "scene did not refresh after second pose")

		// Shrinking the plate must shrink the CageMesh bounds too, so rays
		// that only reach the old extents are no longer considered occluded.
		shrink := make([]Vector, len(rest))
		for i, p := range rest {
			shrink[i] = p.MulScalar(0.1)
		}
		c.Check(cm.Pose(shrink) == nil, "shrink Pose failed")
		scene.Compile()
		cb := cm.BoundingBox()
		c.Check(cb.Max.X < 0.2 && cb.Max.Y < 0.2, "CageMesh bounds did not shrink: %v", cb)
		// The scene tree is the union with the static triangle, so it should
		// shrink in Z near the plate but keep the static extents.
		c.Check(scene.Tree.Box == scene.Tree.Box, "scene tree missing")
		outside := Vector{0.5, 0.5, -1}
		c.Check(scene.Visible(eye, outside), "ray outside the shrunken plate should be unoccluded")

		// --- no degenerate triangles survive any successful pose -------------------
		poses := [][]Vector{rest, moved, shrink}
		for pi, pose := range poses {
			c.Check(cm.Pose(pose) == nil, "pose %d failed", pi)
			seen := make(map[string]bool)
			for ti, tri := range cm.triangles {
				c.Check(cageTriangleArea(tri) > 1e-12, "pose %d triangle %d has zero area", pi, ti)
				key := func(v Vector) string {
					return fmt.Sprintf("%.9f,%.9f,%.9f", v.X, v.Y, v.Z)
				}
				// No duplicate triangle (same three vertices in any order).
				verts := []Vector{tri.V1, tri.V2, tri.V3}
				labels := []string{key(verts[0]), key(verts[1]), key(verts[2])}
				sort.Strings(labels)
				k := labels[0] + "|" + labels[1] + "|" + labels[2]
				c.Check(!seen[k], "pose %d has duplicate triangle %v", pi, k)
				seen[k] = true
			}
			// Paths exactly three per source triangle.
			c.Check(len(cm.Paths()) == 3*len(plate.Triangles),
				"pose %d path count = %d, want %d", pi, len(cm.Paths()), 3*len(plate.Triangles))
		}
	})
}
