package ln

import (
	"math"
	"testing"

	"github.com/fogleman/ln/task_support/taskcheck"
)

// twoCellCage returns two non-coplanar tetrahedral cells sharing the face
// (1,0,0),(0,1,0),(0,0,1) with apices (0,0,0) and (1,1,1).
func twoCellCage() ([]Vector, [][4]int) {
	rest := []Vector{
		{0, 0, 0},
		{1, 0, 0},
		{0, 1, 0},
		{0, 0, 1},
		{1, 1, 1},
	}
	cells := [][4]int{{0, 1, 2, 3}, {1, 2, 3, 4}}
	return rest, cells
}

func cageSliceVertices(m *CageMesh) []Vector {
	var out []Vector
	for _, t := range m.tris {
		out = append(out, t.V1, t.V2, t.V3)
	}
	return out
}

func hasVector(list []Vector, want Vector, eps float64) bool {
	for _, v := range list {
		if v.Distance(want) <= eps {
			return true
		}
	}
	return false
}

func triangleArea3(a, b, c Vector) float64 {
	return b.Sub(a).Cross(c.Sub(a)).Length() / 2
}

func cageSliceArea(m *CageMesh) float64 {
	sum := 0.0
	for _, t := range m.tris {
		sum += triangleArea3(t.V1, t.V2, t.V3)
	}
	return sum
}

func pathsDeepEqual(a, b Paths) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if len(a[i]) != len(b[i]) {
			return false
		}
		for j := range a[i] {
			if a[i][j] != b[i][j] {
				return false
			}
		}
	}
	return true
}

func checkVector(c *taskcheck.C, got, want Vector, eps float64, msg string) {
	c.Check(got.Distance(want) <= eps, "%s: got %v want %v", msg, got, want)
}

func TestCageSplit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		rest, cells := twoCellCage()
		v1 := Vector{0.5, 0.1, 0.1}
		v2 := Vector{0.1, 0.5, 0.1}
		v3 := Vector{0.8, 0.8, 0.8}
		mesh := NewMesh([]*Triangle{NewTriangle(v1, v2, v3)})
		cage, err := NewCageMesh(mesh, rest, cells)
		c.Check(err == nil, "NewCageMesh failed: %v", err)
		if err != nil {
			return
		}
		stats := cage.BindingStats()
		c.Check(stats.Cells == 2, "Cells=%d", stats.Cells)
		c.Check(stats.SourceTriangles == 1, "SourceTriangles=%d", stats.SourceTriangles)
		c.Check(stats.TriangleCellClips == 2, "TriangleCellClips=%d", stats.TriangleCellClips)
		c.Check(stats.Slices == 3, "Slices=%d", stats.Slices)
		c.Check(cage.Generation() == 1, "Generation=%d", cage.Generation())

		// Independently computed cut points on the shared face x+y+z=1.
		s := 3.0 / 17.0
		p := v1.Add(v3.Sub(v1).MulScalar(s))
		q := v2.Add(v3.Sub(v2).MulScalar(s))
		verts := cageSliceVertices(cage)
		c.Check(len(verts) == 9, "slice vertex count=%d", len(verts))
		for i, want := range []Vector{v1, v2, v3, p, q} {
			c.Check(hasVector(verts, want, 1e-9), "missing slice vertex %d (%v)", i, want)
		}

		// Total slice area equals the source triangle area and the winding
		// (hence the normal) is preserved on every slice.
		areaT := triangleArea3(v1, v2, v3)
		c.Check(math.Abs(cageSliceArea(cage)-areaT) <= 1e-9, "slice area sum")
		normalT := v2.Sub(v1).Cross(v3.Sub(v1)).Normalize()
		for i, tr := range cage.tris {
			n := tr.V2.Sub(tr.V1).Cross(tr.V3.Sub(tr.V1))
			c.Check(n.Dot(normalT) > 0, "slice %d winding flipped", i)
		}

		// Ray hit against the piece inside the second cell, with an
		// independently computed hit point and ray parameter.
		cent := p.Add(q).Add(v3).DivScalar(3)
		ray := Ray{cent.Add(normalT.MulScalar(2)), normalT.MulScalar(-1)}
		h := cage.Intersect(ray)
		c.Check(h.Ok(), "expected hit")
		c.Check(h.Shape == cage, "Hit.Shape should be the CageMesh")
		c.Check(math.Abs(h.T-2) <= 1e-9, "hit T=%v", h.T)
		checkVector(c, ray.Position(h.T), cent, 1e-9, "hit position")
		// Non-unit ray: T stays the original ray parameter.
		ray2 := Ray{cent.Add(normalT.MulScalar(2)), normalT.MulScalar(-4)}
		h2 := cage.Intersect(ray2)
		c.Check(h2.Ok() && math.Abs(h2.T-0.5) <= 1e-9, "non-unit ray T=%v", h2.T)

		// Bounding box matches the source triangle.
		box := cage.BoundingBox()
		checkVector(c, box.Min, Vector{0.1, 0.1, 0.1}, 1e-9, "box min")
		checkVector(c, box.Max, Vector{0.8, 0.8, 0.8}, 1e-9, "box max")

		// Paths: one continuous path per original edge, in order, with bend
		// points where the edges cross the shared face. No cut lines.
		paths := cage.Paths()
		c.Check(len(paths) == 3, "path count=%d", len(paths))
		if len(paths) == 3 {
			c.Check(len(paths[0]) == 2, "edge0 points=%d", len(paths[0]))
			c.Check(len(paths[1]) == 3, "edge1 points=%d", len(paths[1]))
			c.Check(len(paths[2]) == 3, "edge2 points=%d", len(paths[2]))
			checkVector(c, paths[0][0], v1, 1e-9, "edge0 start")
			checkVector(c, paths[0][1], v2, 1e-9, "edge0 end")
			checkVector(c, paths[1][0], v2, 1e-9, "edge1 start")
			checkVector(c, paths[1][1], q, 1e-9, "edge1 bend")
			checkVector(c, paths[1][2], v3, 1e-9, "edge1 end")
			checkVector(c, paths[2][0], v3, 1e-9, "edge2 start")
			checkVector(c, paths[2][1], p, 1e-9, "edge2 bend")
			checkVector(c, paths[2][2], v1, 1e-9, "edge2 end")
		}

		// A tiny but non-zero sliver of a triangle poking into the second
		// cell must be kept as its own slice.
		v3b := Vector{0.2, 0.2, 0.601}
		mesh2 := NewMesh([]*Triangle{NewTriangle(v1, v2, v3b)})
		cage2, err := NewCageMesh(mesh2, rest, cells)
		c.Check(err == nil, "sliver NewCageMesh: %v", err)
		if err == nil {
			c.Check(cage2.BindingStats().Slices == 3, "sliver slices=%d", cage2.BindingStats().Slices)
			sb := 0.3 / 0.301
			p2 := v1.Add(v3b.Sub(v1).MulScalar(sb))
			q2 := v2.Add(v3b.Sub(v2).MulScalar(sb))
			wantSliver := triangleArea3(p2, q2, v3b)
			gotSliver := math.Inf(1)
			for _, tr := range cage2.tris {
				gotSliver = math.Min(gotSliver, triangleArea3(tr.V1, tr.V2, tr.V3))
			}
			c.Check(math.Abs(gotSliver-wantSliver) <= 1e-10, "sliver area=%v want %v", gotSliver, wantSliver)
			areaT2 := triangleArea3(v1, v2, v3b)
			c.Check(math.Abs(cageSliceArea(cage2)-areaT2) <= 1e-9, "sliver total area")
		}

		// Constructor validation.
		_, err = NewCageMesh(nil, rest, cells)
		c.Check(err != nil, "nil mesh should fail")
		badRest := append([]Vector{}, rest...)
		badRest[0] = Vector{math.NaN(), 0, 0}
		_, err = NewCageMesh(mesh, badRest, cells)
		c.Check(err != nil, "non-finite rest point should fail")
		_, err = NewCageMesh(mesh, rest, [][4]int{{0, 1, 2, 9}})
		c.Check(err != nil, "out of range index should fail")
		_, err = NewCageMesh(mesh, rest, [][4]int{{0, 1, 2, -1}})
		c.Check(err != nil, "negative index should fail")
		_, err = NewCageMesh(mesh, rest, [][4]int{{0, 1, 2, 2}})
		c.Check(err != nil, "duplicate index should fail")
		flatRest := append(append([]Vector{}, rest...), Vector{0.3, 0.4, 0})
		_, err = NewCageMesh(mesh, flatRest, [][4]int{{0, 1, 2, 5}})
		c.Check(err != nil, "degenerate cell should fail")
		outside := NewMesh([]*Triangle{NewTriangle(Vector{5, 5, 5}, v1, v2)})
		_, err = NewCageMesh(outside, rest, cells)
		c.Check(err != nil, "uncovered triangle should fail")
		infMesh := NewMesh([]*Triangle{NewTriangle(Vector{math.Inf(1), 0, 0}, v1, v2)})
		_, err = NewCageMesh(infMesh, rest, cells)
		c.Check(err != nil, "non-finite mesh triangle should fail")
	})
}

func TestCagePose(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		rest, cells := twoCellCage()

		// (a) Triangle coplanar with the shared face: it must be emitted
		// exactly once, not duplicated by both adjacent cells.
		a := Vector{0.5, 0.3, 0.2}
		b := Vector{0.3, 0.5, 0.2}
		cc := Vector{0.2, 0.3, 0.5}
		coplanar := NewMesh([]*Triangle{NewTriangle(a, b, cc)})
		cageA, err := NewCageMesh(coplanar, rest, cells)
		c.Check(err == nil, "coplanar NewCageMesh: %v", err)
		if err == nil {
			areaABC := triangleArea3(a, b, cc)
			c.Check(cageA.BindingStats().Slices == 1, "coplanar slices=%d", cageA.BindingStats().Slices)
			c.Check(math.Abs(cageSliceArea(cageA)-areaABC) <= 1e-9, "coplanar area")
			posed := append([]Vector{}, rest...)
			posed[4] = Vector{1.4, 1.2, 1.6}
			c.Check(cageA.Pose(posed) == nil, "coplanar pose")
			c.Check(cageA.BindingStats().Slices == 1, "coplanar slices after pose")
			c.Check(math.Abs(cageSliceArea(cageA)-areaABC) <= 1e-9, "coplanar area after pose")
			checkVector(c, cageA.tris[0].V1, a, 1e-9, "coplanar vertex after pose")
		}

		// (b) Triangle with an edge exactly on a shared cage edge: no
		// duplicate or zero-area slices, no duplicated edge stretches.
		e1 := Vector{1, 0, 0}
		e2 := Vector{0, 1, 0}
		apex := Vector{0.2, 0.2, 0.2}
		edgeMesh := NewMesh([]*Triangle{NewTriangle(e1, e2, apex)})
		cageB, err := NewCageMesh(edgeMesh, rest, cells)
		c.Check(err == nil, "shared edge NewCageMesh: %v", err)
		if err == nil {
			c.Check(cageB.BindingStats().Slices == 1, "shared edge slices=%d", cageB.BindingStats().Slices)
			c.Check(math.Abs(cageSliceArea(cageB)-triangleArea3(e1, e2, apex)) <= 1e-9, "shared edge area")
			paths := cageB.Paths()
			c.Check(len(paths) == 3, "shared edge path count=%d", len(paths))
			if len(paths) == 3 {
				c.Check(len(paths[0]) == 2, "shared edge path0 points=%d", len(paths[0]))
				if len(paths[0]) == 2 {
					checkVector(c, paths[0][0], e1, 1e-9, "shared edge start")
					checkVector(c, paths[0][1], e2, 1e-9, "shared edge end")
				}
			}
		}

		// (c) Editing the source mesh, rest points or cells after
		// construction must not affect the binding.
		v1 := Vector{0.5, 0.1, 0.1}
		v2 := Vector{0.1, 0.5, 0.1}
		v3 := Vector{0.8, 0.8, 0.8}
		restC, cellsC := twoCellCage()
		tr := NewTriangle(v1, v2, v3)
		meshC := NewMesh([]*Triangle{tr})
		cageC, err := NewCageMesh(meshC, restC, cellsC)
		c.Check(err == nil, "source edit NewCageMesh: %v", err)
		if err == nil {
			pathsBefore := cageC.Paths()
			boxBefore := cageC.BoundingBox()
			ray := Ray{Vector{0.4, 0.4, 2}, Vector{0, 0, -1}}
			hitBefore := cageC.Intersect(ray)
			tr.V1 = Vector{99, 99, 99}
			meshC.Triangles = append(meshC.Triangles, NewTriangle(Vector{0, 0, 0}, Vector{1, 0, 0}, Vector{0, 1, 0}))
			restC[0] = Vector{-5, -5, -5}
			cellsC[0][0] = 3
			c.Check(pathsDeepEqual(cageC.Paths(), pathsBefore), "paths changed after source edit")
			c.Check(cageC.BoundingBox() == boxBefore, "box changed after source edit")
			hitAfter := cageC.Intersect(ray)
			c.Check(hitAfter.Ok() == hitBefore.Ok() && hitAfter.T == hitBefore.T, "hit changed after source edit")
		}

		// (d) Consecutive poses recompute from the rest pose; they never
		// accumulate on top of the previous pose.
		restD, cellsD := twoCellCage()
		meshD := NewMesh([]*Triangle{NewTriangle(v1, v2, v3)})
		cageD1, _ := NewCageMesh(meshD, restD, cellsD)
		cageD2, _ := NewCageMesh(meshD, restD, cellsD)
		initial := cageD1.Paths()
		pose1 := append([]Vector{}, restD...)
		pose1[0] = Vector{-0.2, 0.1, 0}
		pose1[4] = Vector{1.5, 1.5, 1.5}
		pose2 := append([]Vector{}, restD...)
		pose2[4] = Vector{0.6, 1.2, 0.9}
		c.Check(cageD1.Pose(pose1) == nil, "pose1")
		c.Check(cageD1.Pose(pose2) == nil, "pose2")
		c.Check(cageD2.Pose(pose2) == nil, "pose2 fresh")
		c.Check(pathsDeepEqual(cageD1.Paths(), cageD2.Paths()), "pose2 differs after pose1")
		c.Check(cageD1.BoundingBox() == cageD2.BoundingBox(), "box differs after pose1")
		c.Check(cageD1.Generation() == 3, "generation=%d", cageD1.Generation())
		c.Check(cageD2.Generation() == 2, "generation=%d", cageD2.Generation())
		c.Check(cageD1.Pose(append([]Vector{}, restD...)) == nil, "pose rest")
		c.Check(pathsDeepEqual(cageD1.Paths(), initial), "rest pose not restored")
		// Bounding box follows the actual surface when it shrinks.
		shrink := append([]Vector{}, restD...)
		shrink[4] = Vector{0.9, 0.9, 0.9}
		c.Check(cageD1.Pose(shrink) == nil, "shrink pose")
		box := cageD1.BoundingBox()
		c.Check(box.Max.X < 0.8, "box did not shrink: %v", box.Max)
		checkVector(c, box.Max, Vector{0.73, 0.73, 0.73}, 1e-9, "shrunk box max")

		// (e) Rejected poses keep the previous geometry, tree and generation.
		restE, cellsE := twoCellCage()
		meshE := NewMesh([]*Triangle{NewTriangle(v1, v2, v3)})
		cageE, _ := NewCageMesh(meshE, restE, cellsE)
		cageE.Compile()
		treeBefore := cageE.tree
		boxBefore := cageE.BoundingBox()
		pathsBefore := cageE.Paths()
		genBefore := cageE.Generation()
		clipsBefore := cageE.BindingStats().TriangleCellClips
		rayE := Ray{Vector{0.4, 0.4, 2}, Vector{0, 0, -1}}
		hitBefore := cageE.Intersect(rayE)
		c.Check(cageE.Pose(restE[:4]) != nil, "wrong point count should fail")
		badFinite := append([]Vector{}, restE...)
		badFinite[2] = Vector{math.Inf(1), 0, 0}
		c.Check(cageE.Pose(badFinite) != nil, "non-finite pose should fail")
		flat := append([]Vector{}, restE...)
		flat[0] = Vector{0.5, 0.5, 0}
		c.Check(cageE.Pose(flat) != nil, "collapsed cell should fail")
		inverted := append([]Vector{}, restE...)
		inverted[0] = Vector{2, 2, 2}
		c.Check(cageE.Pose(inverted) != nil, "inverted cell should fail")
		c.Check(cageE.Generation() == genBefore, "generation changed after failed pose")
		c.Check(cageE.tree == treeBefore, "tree changed after failed pose")
		c.Check(cageE.BoundingBox() == boxBefore, "box changed after failed pose")
		c.Check(pathsDeepEqual(cageE.Paths(), pathsBefore), "paths changed after failed pose")
		hitAfter := cageE.Intersect(rayE)
		c.Check(hitAfter.Ok() == hitBefore.Ok() && hitAfter.T == hitBefore.T, "hit changed after failed pose")
		c.Check(cageE.BindingStats().TriangleCellClips == clipsBefore, "clips changed after pose")
		good := append([]Vector{}, restE...)
		good[4] = Vector{1.5, 1.5, 1.5}
		c.Check(cageE.Pose(good) == nil, "valid pose after failures")
		c.Check(cageE.Generation() == genBefore+1, "generation after recovery=%d", cageE.Generation())
		c.Check(cageE.BindingStats().TriangleCellClips == clipsBefore, "clips changed after valid pose")

		// (f) Two CageMeshes bound to the same source deform independently;
		// interleaved queries do not cross-contaminate.
		restF, cellsF := twoCellCage()
		shared := NewMesh([]*Triangle{NewTriangle(v1, v2, v3)})
		cageF1, _ := NewCageMesh(shared, restF, cellsF)
		cageF2, _ := NewCageMesh(shared, restF, cellsF)
		bent := append([]Vector{}, restF...)
		bent[4] = Vector{1.5, 1.5, 1.5}
		c.Check(cageF1.Pose(bent) == nil, "bend cageF1")
		c.Check(!pathsDeepEqual(cageF1.Paths(), cageF2.Paths()), "poses should differ")
		// v3 is bound to cell 1 with barycentric weights (0.1,0.1,0.1,0.7),
		// so it lands at 0.1*(1,0,0)+0.1*(0,1,0)+0.1*(0,0,1)+0.7*(1.5,1.5,1.5).
		c.Check(hasVector(cageSliceVertices(cageF1), Vector{1.15, 1.15, 1.15}, 1e-9), "bent vertex missing")
		c.Check(hasVector(cageSliceVertices(cageF2), v3, 1e-9), "rest vertex missing")
		normalF := v2.Sub(v1).Cross(v3.Sub(v1)).Normalize()
		targetF := v1.MulScalar(0.05).Add(v2.MulScalar(0.05)).Add(v3.MulScalar(0.9))
		rayF := Ray{targetF.Add(normalF), normalF.MulScalar(-1)}
		h1a := cageF1.Intersect(rayF)
		h2a := cageF2.Intersect(rayF)
		h1b := cageF1.Intersect(rayF)
		h2b := cageF2.Intersect(rayF)
		c.Check(h1a.Ok() == h1b.Ok() && h1a.T == h1b.T, "cageF1 not stable")
		c.Check(h2a.Ok() == h2b.Ok() && h2a.T == h2b.T, "cageF2 not stable")
		c.Check(h2a.Ok() && math.Abs(h2a.T-1) <= 1e-9, "rest hit T=%v", h2a.T)
		c.Check(!h1a.Ok() || math.Abs(h1a.T-h2a.T) > 0.01, "bent hit should differ: %v", h1a.T)
		c.Check(cageF1.Generation() == 2 && cageF2.Generation() == 1, "generations %d/%d",
			cageF1.Generation(), cageF2.Generation())
	})
}

func TestCageScene(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Occluder: a quad bound to a single tetrahedral cell, in front of a
		// static background triangle.
		rest := []Vector{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}}
		cells := [][4]int{{0, 1, 2, 3}}
		q1 := Vector{0.2, 0.2, 0.4}
		q2 := Vector{0.6, 0.2, 0.4}
		q3 := Vector{0.6, 0.6, 0.4}
		q4 := Vector{0.2, 0.6, 0.4}
		quad := NewMesh([]*Triangle{NewTriangle(q1, q2, q3), NewTriangle(q1, q3, q4)})
		cage, err := NewCageMesh(quad, rest, cells)
		c.Check(err == nil, "occluder NewCageMesh: %v", err)
		if err != nil {
			return
		}
		c.Check(cage.BindingStats().Slices == 2, "occluder slices=%d", cage.BindingStats().Slices)
		bg := NewMesh([]*Triangle{NewTriangle(Vector{-5, -5, -1}, Vector{5, -5, -1}, Vector{0, 5, -1})})
		scene := &Scene{}
		scene.Add(cage)
		scene.Add(bg)
		scene.Compile()
		treeBefore := scene.Tree
		eye := Vector{0.42, 0.38, 5}
		target := Vector{0.42, 0.38, -1}
		dir := target.Sub(eye).Normalize()
		c.Check(!scene.Visible(eye, target), "target should be occluded")
		h := scene.Intersect(Ray{eye, dir})
		c.Check(h.Ok() && h.Shape == cage, "first hit should be the cage")
		center := Vector{0.4, 0.4, 0}
		up := Vector{0, 1, 0}
		paths1 := scene.Render(eye, center, up, 200, 200, 45, 0.1, 20, 0.02)
		c.Check(len(paths1) > 0, "empty render")

		// Move the occluder aside. The next Compile/Render must pick up the
		// new geometry without manually clearing any tree.
		posed := append([]Vector{}, rest...)
		posed[0] = Vector{-3, 0, 0}
		c.Check(cage.Pose(posed) == nil, "occluder pose")
		scene.Compile()
		c.Check(scene.Tree != treeBefore, "scene tree should be rebuilt")
		c.Check(scene.Visible(eye, target), "target should be visible after pose")
		h2 := scene.Intersect(Ray{eye, dir})
		c.Check(h2.Ok() && h2.Shape != cage, "cage should no longer be the first hit")
		paths2 := scene.Render(eye, center, up, 200, 200, 45, 0.1, 20, 0.02)
		c.Check(!pathsDeepEqual(paths1, paths2), "rendered paths should change")

		// A triangle crossing two cells: only the three original edges are
		// drawn (no internal cut lines), and bends articulate with the pose.
		rest2, cells2 := twoCellCage()
		v1 := Vector{0.5, 0.1, 0.1}
		v2 := Vector{0.1, 0.5, 0.1}
		v3 := Vector{0.8, 0.8, 0.8}
		cage2, err := NewCageMesh(NewMesh([]*Triangle{NewTriangle(v1, v2, v3)}), rest2, cells2)
		c.Check(err == nil, "scene cage2: %v", err)
		if err == nil {
			posed2 := append([]Vector{}, rest2...)
			posed2[4] = Vector{1.6, 1.4, 1.8}
			c.Check(cage2.Pose(posed2) == nil, "cage2 pose")
			paths := cage2.Paths()
			c.Check(len(paths) == 3, "cut lines drawn: paths=%d", len(paths))
			if len(paths) == 3 && len(paths[1]) == 3 {
				mid := paths[1][1]
				dev := mid.SegmentDistance(paths[1][0], paths[1][2])
				c.Check(dev > 1e-3, "edge should articulate at the cell boundary")
			}
			scene2 := &Scene{}
			scene2.Add(cage2)
			scene2.Compile()
			c.Check(len(scene2.Paths()) == 3, "scene path count=%d", len(scene2.Paths()))
		}

		// Static scenes keep reusing their tree; TransformedShape and plain
		// Mesh usage is unchanged.
		staticScene := &Scene{}
		staticScene.Add(bg)
		staticScene.Compile()
		st := staticScene.Tree
		staticScene.Compile()
		c.Check(staticScene.Tree == st, "static scene tree should be reused")
		ts := NewTransformedShape(bg, Translate(Vector{0, 0, 0.001}))
		scene3 := &Scene{}
		scene3.Add(ts)
		scene3.Compile()
		hit3 := scene3.Intersect(Ray{Vector{0.4, 0.4, 5}, Vector{0, 0, -1}})
		c.Check(hit3.Ok(), "transformed shape hit")
		st3 := scene3.Tree
		scene3.Compile()
		c.Check(scene3.Tree == st3, "transformed static scene tree should be reused")
	})
}
