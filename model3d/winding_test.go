package model3d

import (
	"fmt"
	"math"
	"math/rand"
	"sync"
	"testing"
)

// referenceSolidAngle is an independent implementation of
// the directed solid angle of a triangle as seen from p.
//
// Instead of the Van Oosterom-Strackee arctangent formula
// used by the implementation, the unit rays from p toward
// the three vertices are treated as a spherical triangle
// and its area is computed with L'Huilier's theorem:
//
//	tan(Omega/4)^2 = tan(s/2) tan((s-a)/2) tan((s-b)/2)
//	                 tan((s-c)/2)
//
// where a, b, c are the spherical side lengths and
// s = (a+b+c)/2. L'Huilier's form stays accurate when the
// solid angle is tiny (distant geometry), a regime where
// subtracting pi in the spherical-excess form loses all
// precision. The sign comes from the scalar triple product
// of the raw (un-normalized) rays.
func referenceSolidAngle(p Coord3D, t *Triangle) float64 {
	a0 := t[0].Sub(p)
	b0 := t[1].Sub(p)
	c0 := t[2].Sub(p)
	u := a0.Normalize()
	v := b0.Normalize()
	w := c0.Normalize()

	// atan2-based side angles stay accurate even when the
	// rays are nearly parallel (distant geometry), where
	// acos of a near-one dot product loses all precision.
	angle := func(x, y Coord3D) float64 {
		cos := math.Max(-1, math.Min(1, x.Dot(y)))
		sin := x.Cross(y).Norm()
		return math.Atan2(sin, cos)
	}
	a := angle(v, w)
	b := angle(u, w)
	c := angle(u, v)
	s := (a + b + c) / 2
	prod := math.Tan(s/2) * math.Tan((s-a)/2) *
		math.Tan((s-b)/2) * math.Tan((s-c)/2)
	if prod < 0 {
		prod = 0
	}
	omega := 4 * math.Atan(math.Sqrt(prod))

	if a0.Dot(b0.Cross(c0)) < 0 {
		omega = -omega
	}
	return omega
}

func referenceWinding(mesh *Mesh, p Coord3D) float64 {
	sum := 0.0
	mesh.Iterate(func(t *Triangle) {
		sum += referenceSolidAngle(p, t)
	})
	return sum / fourPi
}

// invertedMesh returns a mesh with every triangle's
// winding reversed.
func invertedMesh(mesh *Mesh) *Mesh {
	out := NewMesh()
	mesh.Iterate(func(t *Triangle) {
		flipped := *t
		flipped[0], flipped[1] = flipped[1], flipped[0]
		out.Add(&flipped)
	})
	return out
}

func TestWindingReference(t *testing.T) {
	// An asymmetric tetrahedron, oriented outward by
	// checking the reference sign at its centroid.
	verts := [4]Coord3D{
		XYZ(0.2, -0.3, 0.1),
		XYZ(1.7, 0.4, -0.2),
		XYZ(-0.6, 1.3, 0.5),
		XYZ(0.1, -0.2, 1.9),
	}
	faces := [][3]int{
		{0, 2, 1},
		{0, 1, 3},
		{1, 2, 3},
		{0, 3, 2},
	}
	tet := NewMesh()
	for _, f := range faces {
		tet.Add(&Triangle{verts[f[0]], verts[f[1]], verts[f[2]]})
	}
	centroid := Coord3D{}
	for _, v := range verts {
		centroid = centroid.Add(v)
	}
	centroid = centroid.Scale(0.25)
	if referenceWinding(tet, centroid) < 0 {
		tet = invertedMesh(tet)
	}

	// Two same-oriented nested boxes, and a version whose
	// inner shell is reversed to carve a cavity.
	outer := NewMeshRect(XYZ(-2, -2, -2), XYZ(2, 2, 2))
	inner := NewMeshRect(XYZ(-1, -1, -1), XYZ(1, 1, 1))
	nested := NewMesh()
	nested.AddMesh(outer)
	nested.AddMesh(inner)
	cavity := NewMesh()
	cavity.AddMesh(outer)
	cavity.AddMesh(invertedMesh(inner))

	// An open surface: a single oriented triangle plus a
	// hemisphere cap whose rim is left open.
	patch := &Triangle{XYZ(-1, -1, 0), XYZ(1, -1, 0), XYZ(0, 1, 0)}
	openMesh := NewMesh()
	openMesh.Add(patch)
	cap := openSphericalCap(40)

	queryPoints := []Coord3D{
		centroid,
		XYZ(0.5, 0.1, 0.2),
		XYZ(0.1, -0.4, 0.3),
		XYZ(5, 0.7, -0.3),
		XYZ(-3.1, 2.2, 1.4),
		// Near a face but clearly not on it.
		XYZ(0, 0, 1.001),
		XYZ(0.999, 0.999, 0.999),
		XYZ(0.3, -0.25, 0.002),
	}

	meshes := map[string]*Mesh{
		"tetrahedron": tet,
		"nested":      nested,
		"cavity":      cavity,
		"open_patch":  openMesh,
		"open_cap":    cap,
	}
	for name, mesh := range meshes {
		tree, err := NewWindingTree(mesh)
		if err != nil {
			t.Fatalf("%s: unexpected constructor error: %v", name, err)
		}
		for _, p := range queryPoints {
			got, err := tree.Evaluate(p, 0)
			if err != nil {
				t.Fatalf("%s at %v: unexpected error: %v", name, p, err)
			}
			want := referenceWinding(mesh, p)
			if math.Abs(got.Value-want) > 1e-10 {
				t.Errorf("%s at %v: winding %g disagrees with reference %g", name, p, got.Value, want)
			}
			if got.ErrorBound != 0 {
				t.Errorf("%s at %v: exact query has non-zero bound %g", name, p, got.ErrorBound)
			}
			if got.ExactTriangles != mesh.NumTriangles() {
				t.Errorf("%s at %v: exact query evaluated %d of %d faces", name, p, got.ExactTriangles, mesh.NumTriangles())
			}
			if got.ApproximatedNodes != 0 {
				t.Errorf("%s at %v: exact query accepted %d aggregate nodes", name, p, got.ApproximatedNodes)
			}
		}
	}

	// Concrete winding-number semantics.
	nestedTree, _ := NewWindingTree(nested)
	cavityTree, _ := NewWindingTree(cavity)
	if v, _ := nestedTree.Evaluate(Coord3D{}, 0); math.Abs(v.Value-2) > 1e-9 {
		t.Errorf("shared interior of two shells should wind +2, got %g", v.Value)
	}
	if v, _ := cavityTree.Evaluate(Coord3D{}, 0); math.Abs(v.Value) > 1e-9 {
		t.Errorf("reversed inner shell should cancel, got %g", v.Value)
	}
	if v, _ := cavityTree.Evaluate(XYZ(1.5, 0, 0), 0); math.Abs(v.Value-1) > 1e-9 {
		t.Errorf("outer shell interior should wind +1, got %g", v.Value)
	}
	if v, _ := nestedTree.Evaluate(XYZ(3, 0, 0), 0); math.Abs(v.Value) > 1e-9 {
		t.Errorf("exterior should wind 0, got %g", v.Value)
	}

	// An open surface gives a genuine fractional value:
	// strictly between the inside and outside limits.
	patchTree, _ := NewWindingTree(openMesh)
	if v, _ := patchTree.Evaluate(XYZ(0, 0, 0.5), 0); math.Abs(v.Value) == 0 || math.Abs(v.Value) >= 1 {
		t.Errorf("open patch should give fractional winding, got %g", v.Value)
	}
	capTree, _ := NewWindingTree(cap)
	if v, _ := capTree.Evaluate(Coord3D{}, 0); math.Abs(math.Abs(v.Value)-0.5) > 1e-9 {
		t.Errorf("hemisphere cap seen from its center should wind +/-0.5, got %g", v.Value)
	}

	// Reversing every triangle reverses the sign, and
	// translation/scale preserve the transformed value.
	tetTree := mustWindingTree(t, tet)
	reversed := invertedMesh(tet)
	revTree, _ := NewWindingTree(reversed)
	for _, p := range []Coord3D{centroid, XYZ(0.5, 0.1, 0.2), XYZ(3, 3, 3)} {
		a, _ := tetTree.Evaluate(p, 0)
		b, _ := revTree.Evaluate(p, 0)
		if math.Abs(a.Value+b.Value) > 1e-10 {
			t.Errorf("mirrored winding at %v: %g and %g are not opposites", p, a.Value, b.Value)
		}
	}
	shift := XYZ(4.7, -2.1, 3.3)
	scaled := tet.Scale(2.5).Translate(shift)
	scaledTree, _ := NewWindingTree(scaled)
	for _, p := range []Coord3D{centroid, XYZ(0.5, 0.1, 0.2), XYZ(-3, 1, 2)} {
		want, _ := tetTree.Evaluate(p, 0)
		moved := p.Scale(2.5).Add(shift)
		got, _ := scaledTree.Evaluate(moved, 0)
		if math.Abs(got.Value-want.Value) > 1e-10 {
			t.Errorf("transform invariance at %v: got %g want %g", moved, got.Value, want.Value)
		}
	}

	// Approximated queries must respect their error
	// interval and the shared budget.
	for _, tol := range []float64{1e-2, 1e-4, 1e-8} {
		res, err := capTree.Evaluate(XYZ(12, 7, -5), tol)
		if err != nil {
			t.Fatalf("tol %g: unexpected error %v", tol, err)
		}
		exact, _ := capTree.Evaluate(XYZ(12, 7, -5), 0)
		if res.ErrorBound > tol*(1+1e-12) {
			t.Errorf("tol %g: bound %g exceeds budget", tol, res.ErrorBound)
		}
		if math.Abs(res.Value-exact.Value) > res.ErrorBound+1e-10 {
			t.Errorf("tol %g: exact value %g outside interval %g +/- %g", tol, exact.Value, res.Value, res.ErrorBound)
		}
	}

	// Input validation.
	if _, err := NewWindingTree(nil); err == nil {
		t.Error("nil mesh should be rejected")
	}
	bad := NewMesh()
	bad.Add(&Triangle{XYZ(0, 0, 0), XYZ(1, 0, 0), XYZ(0, 1, math.NaN())})
	if _, err := NewWindingTree(bad); err == nil {
		t.Error("non-finite triangle should be rejected")
	}
	degenerate := NewMesh()
	degenerate.Add(&Triangle{XYZ(0, 0, 0), XYZ(1, 0, 0), XYZ(2, 0, 0)})
	if _, err := NewWindingTree(degenerate); err == nil {
		t.Error("zero-area triangle should be rejected")
	}
	emptyTree, err := NewWindingTree(NewMesh())
	if err != nil {
		t.Fatalf("empty mesh construction failed: %v", err)
	}
	emptyRes, err := emptyTree.Evaluate(XYZ(1, 2, 3), 1e-4)
	if err != nil {
		t.Fatalf("empty mesh query failed: %v", err)
	}
	if emptyRes != (WindingResult{}) {
		t.Errorf("empty mesh should give zero result, got %+v", emptyRes)
	}
	if _, err := capTree.Evaluate(XYZ(math.NaN(), 0, 0), 1e-4); err == nil {
		t.Error("non-finite point should be rejected")
	}
	if _, err := capTree.Evaluate(Coord3D{}, math.NaN()); err == nil {
		t.Error("non-finite tolerance should be rejected")
	}
	if _, err := capTree.Evaluate(Coord3D{}, -1e-9); err == nil {
		t.Error("negative tolerance should be rejected")
	}

	// A point lying on a triangle (interior, edge, vertex)
	// must be reported explicitly.
	onFace := []Coord3D{
		patch[0].Mid(patch[1]).Mid(patch[2]), // interior-ish
		patch[0].Mid(patch[1]),               // edge
		patch[0],                             // vertex
	}
	for _, p := range onFace {
		if _, err := patchTree.Evaluate(p, 0); err == nil {
			t.Errorf("point %v on triangle should error", p)
		}
		if _, err := patchTree.Evaluate(p, 1e-4); err == nil {
			t.Errorf("point %v on triangle should error even with tolerance", p)
		}
	}
}

// mustWindingTree builds a tree or fails the test.
func mustWindingTree(t *testing.T, m *Mesh) *WindingTree {
	t.Helper()
	tree, err := NewWindingTree(m)
	if err != nil {
		t.Fatal(err)
	}
	return tree
}

// ExampleNewWindingSDF shows the sign difference from the
// parity-based SDF: nested same-oriented shells keep their
// common interior solid under the winding rule.
func ExampleNewWindingSDF() {
	inner := NewMeshRect(XYZ(-1, -1, -1), XYZ(1, 1, 1))
	outer := NewMeshRect(XYZ(-2, -2, -2), XYZ(2, 2, 2))
	outer.AddMesh(inner)

	sdf, err := NewWindingSDF(outer, 0.5)
	if err != nil {
		panic(err)
	}
	// Inside both shells: winding 2 > 0.5, distance to the
	// inner shell is 1.
	fmt.Printf("center: %+.1f\n", sdf.SDF(Coord3D{}))
	// Outside everything: negative distance to the outer
	// shell.
	fmt.Printf("exterior: %+.1f\n", sdf.SDF(XYZ(3, 0, 0)))
	// Output:
	// center: +1.0
	// exterior: -1.0
}

// openSphericalCap builds an open hemisphere of radius 1
// around the negative Y pole, with the equatorial rim left
// open. With stops=40 it already has several thousand
// triangles; callers needing more pass a larger value.
func openSphericalCap(stops int) *Mesh {
	mesh := NewMesh()
	latStep := math.Pi / 2 / float64(stops)
	lonStep := math.Pi * 2 / float64(stops)
	point := func(lat, lon float64) Coord3D {
		return XYZ(math.Sin(lon)*math.Cos(lat), math.Sin(lat), math.Cos(lon)*math.Cos(lat))
	}
	for lonIdx := 0; lonIdx < stops; lonIdx++ {
		for latIdx := 0; latIdx < stops; latIdx++ {
			lat0 := -math.Pi/2 + float64(latIdx)*latStep
			lat1 := lat0 + latStep
			lon0 := -math.Pi + float64(lonIdx)*lonStep
			lon1 := lon0 + lonStep
			p0 := point(lat0, lon0)
			p1 := point(lat0, lon1)
			p2 := point(lat1, lon1)
			p3 := point(lat1, lon0)
			if latIdx == 0 {
				p0 = XYZ(0, -1, 0)
				p1 = XYZ(0, -1, 0)
			}
			if latIdx != 0 {
				mesh.Add(&Triangle{p0, p1, p2})
			}
			mesh.Add(&Triangle{p0, p2, p3})
		}
	}
	return mesh
}

func TestWindingHierarchy(t *testing.T) {
	const stops = 110
	mesh := openSphericalCap(stops)
	if n := mesh.NumTriangles(); n < 10000 {
		t.Fatalf("test setup needs at least 10000 faces, got %d", n)
	}
	tree, err := NewWindingTree(mesh)
	if err != nil {
		t.Fatal(err)
	}

	min, max := mesh.Min(), mesh.Max()
	diag := min.Dist(max)

	// Fixed directions, including diagonal ones.
	dirs := []Coord3D{
		XYZ(1, 0, 0),
		XYZ(0, 1, 0),
		XYZ(1, 1, 1).Normalize(),
		XYZ(-1, 0.6, -0.4).Normalize(),
	}
	var points []Coord3D
	for k := 4; k <= 10; k++ {
		for _, dir := range dirs {
			// Start from the extreme bounding-box corner in
			// the query direction, so the distance from the
			// resulting point to the box is exactly k*D.
			corner := Coord3D{}
			if dir.X >= 0 {
				corner.X = max.X
			} else {
				corner.X = min.X
			}
			if dir.Y >= 0 {
				corner.Y = max.Y
			} else {
				corner.Y = min.Y
			}
			if dir.Z >= 0 {
				corner.Z = max.Z
			} else {
				corner.Z = min.Z
			}
			p := corner.Add(dir.Scale(float64(k) * diag))
			d := math.Sqrt(pointToBoundsDistSquared(p, min, max))
			if d < 4*diag*(1-1e-12) || d > 10*diag*(1+1e-12) {
				t.Fatalf("test setup: point %v box distance %g outside [4D, 10D] (D=%g)", p, d, diag)
			}
			points = append(points, p)
		}
	}

	const tol = 1e-4
	for _, p := range points {
		res, err := tree.Evaluate(p, tol)
		if err != nil {
			t.Fatalf("query at %v failed: %v", p, err)
		}
		if res.ErrorBound > tol {
			t.Errorf("point %v: bound %g exceeds tolerance %g", p, res.ErrorBound, tol)
		}
		if res.ApproximatedNodes == 0 {
			t.Errorf("point %v: no aggregate node was accepted", p)
		}
		if res.ExactTriangles >= mesh.NumTriangles()/5 {
			t.Errorf("point %v: evaluated %d of %d faces, want under 20%%", p, res.ExactTriangles, mesh.NumTriangles())
		}
		exact, err := tree.Evaluate(p, 0)
		if err != nil {
			t.Fatalf("exact query at %v failed: %v", p, err)
		}
		if math.Abs(res.Value-exact.Value) > res.ErrorBound+1e-10 {
			t.Errorf("point %v: exact winding %g outside %g +/- %g", p, exact.Value, res.Value, res.ErrorBound)
		}
		if math.Abs(exact.Value-referenceWinding(mesh, p)) > 1e-10 {
			t.Errorf("point %v: implementation disagrees with independent reference", p)
		}
	}

	// Mutually cancelling opposite-facing shells must keep
	// the guarantee: the unsigned-area bound may be loose,
	// but may never underestimate the error.
	outer := NewMeshRect(XYZ(-1, -1, -1), XYZ(1, 1, 1))
	cancelling := NewMesh()
	cancelling.AddMesh(outer)
	cancelling.AddMesh(invertedMesh(outer))
	cancelTree, _ := NewWindingTree(cancelling)
	for _, p := range points[:4] {
		res, err := cancelTree.Evaluate(p, tol)
		if err != nil {
			t.Fatalf("cancelling shells at %v: %v", p, err)
		}
		exact, _ := cancelTree.Evaluate(p, 0)
		if math.Abs(res.Value-exact.Value) > res.ErrorBound+1e-10 {
			t.Errorf("cancelling shells at %v: interval %g +/- %g misses exact %g", p, res.Value, res.ErrorBound, exact.Value)
		}
		if math.Abs(exact.Value) > 1e-10 {
			t.Errorf("cancelling shells should sum to zero, got %g", exact.Value)
		}
	}

	// Slim triangles: a fan of very thin triangles still
	// carries a valid bound.
	slim := NewMesh()
	for i := 0; i < 200; i++ {
		theta := float64(i) * math.Pi / 100
		slim.Add(&Triangle{
			XYZ(0, 0, 0),
			XYZ(math.Cos(theta), math.Sin(theta), 0),
			XYZ(math.Cos(theta+0.002), math.Sin(theta+0.002), 0),
		})
	}
	slimTree, _ := NewWindingTree(slim)
	for _, p := range []Coord3D{XYZ(8, 3, 5), XYZ(-6, -7, 4), XYZ(10, 0, 0)} {
		res, err := slimTree.Evaluate(p, tol)
		if err != nil {
			t.Fatalf("slim fan at %v: %v", p, err)
		}
		exact, _ := slimTree.Evaluate(p, 0)
		if math.Abs(res.Value-exact.Value) > res.ErrorBound+1e-10 {
			t.Errorf("slim fan at %v: interval invalid", p)
		}
	}

	// Snapshot ownership: later mesh edits and triangle
	// mutations must not change tree answers.
	snapshotMesh := NewMeshRect(XYZ(-1, -1, -1), XYZ(1, 1, 1))
	snapTree, _ := NewWindingTree(snapshotMesh)
	before, _ := snapTree.Evaluate(XYZ(2, 0.3, -0.2), 1e-4)
	var stolen *Triangle
	snapshotMesh.Iterate(func(t *Triangle) {
		if stolen == nil {
			stolen = t
		}
	})
	snapshotMesh.Remove(stolen)
	stolen[0] = XYZ(99, 99, 99)
	snapshotMesh.Add(&Triangle{XYZ(5, 5, 5), XYZ(6, 5, 5), XYZ(5, 6, 5)})
	after, _ := snapTree.Evaluate(XYZ(2, 0.3, -0.2), 1e-4)
	if after != before {
		t.Errorf("tree changed after mesh mutation: %+v vs %+v", before, after)
	}

	// Concurrent read-only queries must not share per-query
	// state (run under -race by the check script).
	const goroutines = 48
	const queriesPer = 25
	var wg sync.WaitGroup
	errCh := make(chan error, goroutines)
	for g := 0; g < goroutines; g++ {
		wg.Add(1)
		go func(seed int64) {
			defer wg.Done()
			rng := rand.New(rand.NewSource(seed))
			for i := 0; i < queriesPer; i++ {
				p := XYZ(rng.NormFloat64()*12, rng.NormFloat64()*12, rng.NormFloat64()*12)
				useTol := tol
				if i%3 == 0 {
					useTol = 0
				}
				res, err := tree.Evaluate(p, useTol)
				if err != nil {
					errCh <- err
					return
				}
				if useTol > 0 && res.ErrorBound > useTol {
					errCh <- fmt.Errorf("aggregate bound %g exceeded tolerance %g", res.ErrorBound, useTol)
					return
				}
				exact, err := tree.Evaluate(p, 0)
				if err != nil {
					errCh <- err
					return
				}
				if math.Abs(res.Value-exact.Value) > res.ErrorBound+1e-10 {
					errCh <- fmt.Errorf("exact value %g outside reported interval %g +/- %g",
						exact.Value, res.Value, res.ErrorBound)
					return
				}
			}
		}(int64(g) + 1)
	}
	wg.Wait()
	close(errCh)
	for err := range errCh {
		t.Error(err)
	}
}

func TestWindingSDF(t *testing.T) {
	// Construction validation.
	if _, err := NewWindingSDF(NewMesh(), 0.5); err == nil {
		t.Error("empty mesh must be rejected")
	}
	box := NewMeshRect(XYZ(-1, -1, -1), XYZ(1, 1, 1))
	for _, bad := range []float64{0, -0.5, math.NaN(), math.Inf(1)} {
		if _, err := NewWindingSDF(box, bad); err == nil {
			t.Errorf("threshold %g should be rejected", bad)
		}
	}

	// Same-oriented nested boxes: parity flips the center to
	// outside, but winding 2 with threshold 0.5 is inside
	// with a positive true distance (the inner shell at 1).
	outer := NewMeshRect(XYZ(-2, -2, -2), XYZ(2, 2, 2))
	inner := NewMeshRect(XYZ(-1, -1, -1), XYZ(1, 1, 1))
	nested := NewMesh()
	nested.AddMesh(outer)
	nested.AddMesh(inner)
	if ColliderContains(MeshToCollider(nested), Coord3D{}, 0) {
		t.Fatal("baseline assumption broken: nested center is expected parity-outside")
	}
	sdf, err := NewWindingSDF(nested, 0.5)
	if err != nil {
		t.Fatal(err)
	}
	if v := sdf.SDF(Coord3D{}); math.Abs(v-1) > 1e-10 || v < 0 {
		t.Errorf("nested center should be positive distance 1, got %g", v)
	}
	if v := sdf.SDF(XYZ(3, 0, 0)); v >= 0 || math.Abs(math.Abs(v)-1) > 1e-10 {
		t.Errorf("exterior point should be negative distance -1, got %g", v)
	}

	// Both sides of a threshold: with threshold 1.5 the
	// center (winding 2) stays inside while the gap between
	// shells (winding 1) becomes outside.
	strict, err := NewWindingSDF(nested, 1.5)
	if err != nil {
		t.Fatal(err)
	}
	if v := strict.SDF(Coord3D{}); v <= 0 {
		t.Errorf("winding 2 should pass threshold 1.5, got %g", v)
	}
	if v := strict.SDF(XYZ(1.5, 0, 0)); v >= 0 {
		t.Errorf("winding 1 should fail threshold 1.5, got %g", v)
	}

	// Nearest face and point come from the real geometric
	// distance path and the SDF's own snapshot.
	snapshot := map[*Triangle]bool{}
	for _, f := range sdf.(*windingSDF).tree.snapshotFaces() {
		snapshot[f] = true
	}
	rng := rand.New(rand.NewSource(1234))
	for i := 0; i < 200; i++ {
		p := XYZ(rng.NormFloat64()*3.5, rng.NormFloat64()*3.5, rng.NormFloat64()*3.5)
		face, point, dist := sdf.FaceSDF(p)
		if face == nil {
			t.Fatalf("no face returned at %v", p)
		}
		if !snapshot[face] {
			t.Fatalf("returned face does not belong to the construction snapshot")
		}
		if math.Abs(point.Dist(p)-math.Abs(dist)) > 1e-10 {
			t.Errorf("at %v: point distance %g disagrees with |sdf| %g", p, point.Dist(p), dist)
		}
		if d := face.Dist(p); math.Abs(d-math.Abs(dist)) > 1e-10 {
			t.Errorf("at %v: face distance %g disagrees with |sdf| %g", p, d, dist)
		}
		// Sign agreement across the SDF surface methods.
		if d := sdf.SDF(p); math.Abs(d-dist) > 1e-10 {
			t.Errorf("at %v: SDF %g disagrees with FaceSDF %g", p, d, dist)
		}
		pp, pd := sdf.PointSDF(p)
		if math.Abs(pd-dist) > 1e-10 || pp != point {
			t.Errorf("at %v: PointSDF (%v, %g) disagrees with FaceSDF (%v, %g)", p, pp, pd, point, dist)
		}
		n, nd := sdf.NormalSDF(p)
		if math.Abs(nd-dist) > 1e-10 || n != face.Normal() {
			t.Errorf("at %v: NormalSDF inconsistent", p)
		}
	}

	// Distance zero on the surface returns zero regardless
	// of the (undefined) winding there.
	surface := XYZ(0, 0, 1)
	if v := sdf.SDF(surface); v != 0 {
		t.Errorf("surface point should give zero, got %g", v)
	}
	if _, _, d := sdf.FaceSDF(surface); d != 0 {
		t.Errorf("surface FaceSDF should give zero, got %g", d)
	}

	// Open surface: fractional winding still yields a
	// well-defined sign, and distances remain exact. The
	// two points lie on opposite sides of the patch, so
	// their signs must differ and agree with the strict
	// winding rule regardless of patch orientation.
	cap := openSphericalCap(30)
	openSDF, err := NewWindingSDF(cap, 0.5)
	if err != nil {
		t.Fatal(err)
	}
	capTree, _ := NewWindingTree(cap)
	deep := XYZ(0, -0.5, 0)
	far := XYZ(0, 0.5, 0)
	deepWinding, _ := capTree.Evaluate(deep, 0)
	farWinding, _ := capTree.Evaluate(far, 0)
	deepSDF := openSDF.SDF(deep)
	farSDF := openSDF.SDF(far)
	if math.Abs(deepWinding.Value) > 0.5 {
		if deepSDF <= 0 {
			t.Errorf("deep-side point should be inside, got %g (winding %g)", deepSDF, deepWinding.Value)
		}
	} else {
		if deepSDF >= 0 {
			t.Errorf("deep-side point should be outside, got %g (winding %g)", deepSDF, deepWinding.Value)
		}
	}
	if math.Abs(farWinding.Value) > 0.5 {
		if farSDF <= 0 {
			t.Errorf("far-side point should be inside, got %g (winding %g)", farSDF, farWinding.Value)
		}
	} else {
		if farSDF >= 0 {
			t.Errorf("far-side point should be outside, got %g (winding %g)", farSDF, farWinding.Value)
		}
	}
	if deepSDF*farSDF >= 0 {
		t.Errorf("opposite sides of the open patch should have opposite signs, got %g and %g", deepSDF, farSDF)
	}
	face, point, dist := openSDF.FaceSDF(deep)
	if face == nil || math.Abs(point.Dist(deep)-math.Abs(dist)) > 1e-10 {
		t.Errorf("open surface nearest-face result inconsistent: %v %g", point, dist)
	}

	// Snapshot isolation for the SDF as well.
	snapMesh := NewMeshRect(XYZ(-1, -1, -1), XYZ(1, 1, 1))
	snapSDF, _ := NewWindingSDF(snapMesh, 0.5)
	before := snapSDF.SDF(XYZ(2, 0, 0))
	var face0 *Triangle
	snapMesh.Iterate(func(t *Triangle) { face0 = t })
	snapMesh.Remove(face0)
	face0[0] = XYZ(42, 42, 42)
	if after := snapSDF.SDF(XYZ(2, 0, 0)); after != before {
		t.Errorf("snapshot SDF changed after mesh mutation: %g vs %g", after, before)
	}
}
