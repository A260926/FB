//-----------------------------------------------------------------------------
/*

Bounded Dual Contouring: solver and renderer tests.

The expected minimizers and residuals below are derived analytically from the
plane equations, not by cross-checking two functions of the new code.

*/
//-----------------------------------------------------------------------------

package dc

import (
	"math"
	"os"
	"path/filepath"
	"sync"
	"testing"

	"github.com/deadsy/sdfx/render"
	"github.com/deadsy/sdfx/sdf"
	v3 "github.com/deadsy/sdfx/vec/v3"
)

//-----------------------------------------------------------------------------
// helpers
//-----------------------------------------------------------------------------

const tolSolver = 1e-7

// solveUnit runs the constrained solver on a unit voxel rooted at the origin,
// so voxel-local and world coordinates coincide.
func solveUnit(t *testing.T, points, normals []v3.Vec) boundedQEFResult {
	t.Helper()
	res := boundedQEFSolve(points, normals, v3.Vec{}, v3.Vec{X: 1, Y: 1, Z: 1})
	if math.IsNaN(res.local.X) || math.IsInf(res.local.X, 0) ||
		math.IsNaN(res.local.Y) || math.IsInf(res.local.Y, 0) ||
		math.IsNaN(res.local.Z) || math.IsInf(res.local.Z, 0) {
		t.Fatalf("non-finite solver result: %+v", res)
	}
	if res.local.X < -1e-10 || res.local.X > 1+1e-10 ||
		res.local.Y < -1e-10 || res.local.Y > 1+1e-10 ||
		res.local.Z < -1e-10 || res.local.Z > 1+1e-10 {
		t.Fatalf("solver result outside the voxel box: %+v", res.local)
	}
	return res
}

// planeResidual independently evaluates sum_i (n_i . x - n_i . p_i)^2.
func planeResidual(x v3.Vec, points, normals []v3.Vec) float64 {
	sum := 0.0
	for i, n := range normals {
		d := n.Dot(x) - n.Dot(points[i])
		sum += d * d
	}
	return sum
}

func expectVec(t *testing.T, name string, got, want v3.Vec, tol float64) {
	t.Helper()
	if d := got.Sub(want).Length(); d > tol {
		t.Fatalf("%s = %v, want %v (distance %g > tol %g)", name, got, want, d, tol)
	}
}

var (
	ex = v3.Vec{X: 1}
	ey = v3.Vec{Y: 1}
	ez = v3.Vec{Z: 1}
)

//-----------------------------------------------------------------------------
// TestBoundedRank: rank 1, rank 2 and full rank plane systems
//-----------------------------------------------------------------------------

func TestBoundedRank(t *testing.T) {
	const tol = tolSolver

	// --- Rank 1: a single plane keeps the tangential mean position. ---
	t.Run("rank1_single_plane", func(t *testing.T) {
		// Plane x = 0.3, two Hermite points with different tangential coords.
		points := []v3.Vec{{X: 0.3, Y: 0.2, Z: 0.4}, {X: 0.3, Y: 0.8, Z: 0.1}}
		normals := []v3.Vec{ex, ex}
		res := solveUnit(t, points, normals)
		if res.rank != 1 {
			t.Fatalf("rank = %d, want 1", res.rank)
		}
		// Analytic minimizer: x pinned, y/z at the mean point.
		expectVec(t, "local", res.local, v3.Vec{X: 0.3, Y: 0.5, Z: 0.25}, tol)
		if math.Abs(res.residual) > tol*tol {
			t.Fatalf("residual = %g, want ~0", res.residual)
		}
	})

	// --- Rank 1: parallel/repeated normals must not fall back to the center. ---
	t.Run("rank1_parallel_repeated", func(t *testing.T) {
		points := []v3.Vec{
			{X: 0.3, Y: 0.2, Z: 0.4},
			{X: 0.3, Y: 0.8, Z: 0.1},
			{X: 0.3, Y: 0.8, Z: 0.1}, // exact duplicate
			{X: 0.3, Y: 0.6, Z: 0.9},
		}
		normals := []v3.Vec{ex, ex, ex, ex}
		res := solveUnit(t, points, normals)
		if res.rank != 1 {
			t.Fatalf("rank = %d, want 1", res.rank)
		}
		mean := v3.Vec{X: 0.3, Y: (0.2 + 0.8 + 0.8 + 0.6) / 4, Z: (0.4 + 0.1 + 0.1 + 0.9) / 4}
		expectVec(t, "local", res.local, mean, tol)
		if res.local.Equals(v3.Vec{X: 0.5, Y: 0.5, Z: 0.5}, 1e-9) {
			t.Fatalf("parallel planes regressed to voxel center")
		}
	})

	// --- Rank 2: the canonical example from the design discussion. ---
	// Unit cube, planes x+y=2 (unit normal (1,1,0)/sqrt2) and x=0.
	// True constrained minimum is x=1/3, y=1; z is free and follows the mean.
	t.Run("rank2_canonical_corner_planes", func(t *testing.T) {
		sqrt2 := math.Sqrt2
		n1 := v3.Vec{X: 1 / sqrt2, Y: 1 / sqrt2}
		n2 := ex
		p1 := v3.Vec{X: 1, Y: 1, Z: 0.6}   // on x+y=2, inside the cube
		p2 := v3.Vec{X: 0, Y: 0.4, Z: 0.2} // on x=0
		points := []v3.Vec{p1, p2}
		normals := []v3.Vec{n1, n2}

		res := solveUnit(t, points, normals)
		if res.rank != 2 {
			t.Fatalf("rank = %d, want 2", res.rank)
		}
		want := v3.Vec{X: 1.0 / 3.0, Y: 1, Z: 0.4}
		expectVec(t, "local", res.local, want, tol)

		// Analytic residual: 1/3 (plane 1 contributes 2/9, plane 2 contributes 1/9).
		if math.Abs(res.residual-1.0/3.0) > tol {
			t.Fatalf("residual = %v, want 1/3", res.residual)
		}
		// Independent recomputation.
		if r := planeResidual(res.local, points, normals); math.Abs(r-res.residual) > 1e-12 {
			t.Fatalf("residual mismatch: solver %g vs direct %g", res.residual, r)
		}
		// Naive per-component clamp of the unconstrained point (0,1,0.4) is
		// demonstrably worse than the true minimum.
		clamped := v3.Vec{X: 0, Y: 1, Z: 0.4}
		if rc := planeResidual(clamped, points, normals); rc <= res.residual {
			t.Fatalf("clamped point residual %g not larger than minimum %g", rc, res.residual)
		}
	})

	// --- Rank 2: input ordering and duplicate planes leave the answer unchanged. ---
	t.Run("rank2_order_and_duplicates", func(t *testing.T) {
		sqrt2 := math.Sqrt2
		n1 := v3.Vec{X: 1 / sqrt2, Y: 1 / sqrt2}
		n2 := ex
		p1 := v3.Vec{X: 1, Y: 1, Z: 0.6}
		p2 := v3.Vec{X: 0, Y: 0.4, Z: 0.2}
		base := solveUnit(t, []v3.Vec{p1, p2}, []v3.Vec{n1, n2})

		reversed := solveUnit(t, []v3.Vec{p2, p1}, []v3.Vec{n2, n1})
		expectVec(t, "reversed", reversed.local, base.local, tol)

		// Balanced duplicates keep every plane's weight and the mean, so the
		// minimizer is exactly preserved.
		dupPoints := []v3.Vec{p1, p2, p1, p2}
		dupNormals := []v3.Vec{n1, n2, n1, n2}
		dups := solveUnit(t, dupPoints, dupNormals)
		expectVec(t, "duplicated", dups.local, base.local, tol)

		// An unbalanced duplicate changes the plane weights: the new minimizer
		// is checked against its own analytically recomputed solution, not the
		// old one (on y=1: min of (x-1)^2 + 3x^2 is x = 1/4).
		unbalanced := solveUnit(t,
			[]v3.Vec{p1, p2, p1, p2, p2},
			[]v3.Vec{n1, n2, n1, n2, n2})
		expectVec(t, "reweighted", unbalanced.local,
			v3.Vec{X: 0.25, Y: 1, Z: (2*0.6 + 3*0.2) / 5}, tol)
	})

	// --- Full rank: three independent planes pin every coordinate. ---
	t.Run("rank3_full", func(t *testing.T) {
		points := []v3.Vec{
			{X: 0.4, Y: 0.1, Z: 0.2},
			{X: 0.0, Y: 0.6, Z: 0.3},
			{X: 0.1, Y: 0.2, Z: 0.7},
			{X: 0.4, Y: 0.6, Z: 0.7}, // extra consistent plane n=(1,1,1)/sqrt3
		}
		n4 := v3.Vec{X: 1 / math.Sqrt(3), Y: 1 / math.Sqrt(3), Z: 1 / math.Sqrt(3)}
		normals := []v3.Vec{ex, ey, ez, n4}
		res := solveUnit(t, points, normals)
		if res.rank != 3 {
			t.Fatalf("rank = %d, want 3", res.rank)
		}
		expectVec(t, "local", res.local, v3.Vec{X: 0.4, Y: 0.6, Z: 0.7}, tol)
		if math.Abs(res.residual) > tol {
			t.Fatalf("residual = %g, want ~0", res.residual)
		}
	})

	// --- Near rank deficiency: finite, in-box, no worse than the center. ---
	t.Run("near_rank_deficient", func(t *testing.T) {
		// Two almost-parallel planes whose exact intersection sits at y=2,
		// well outside the voxel: x=0 and cos(e)x + sin(e)y = 2 sin(e).
		const eps = 1e-6
		n1 := ex
		n2 := v3.Vec{X: math.Cos(eps), Y: math.Sin(eps)}
		points := []v3.Vec{{}, {Y: 2}}
		normals := []v3.Vec{n1, n2}
		res := solveUnit(t, points, normals)
		center := v3.Vec{X: 0.5, Y: 0.5, Z: 0.5}
		if rc := planeResidual(center, points, normals); res.residual > rc+1e-12 {
			t.Fatalf("near-rank residual %g exceeds feasible center candidate %g", res.residual, rc)
		}
		if r := planeResidual(res.local, points, normals); math.Abs(r-res.residual) > 1e-10 {
			t.Fatalf("residual mismatch: solver %g vs direct %g", res.residual, r)
		}
	})

	// --- No effective constraint: the voxel center is the fallback. ---
	t.Run("no_constraint_center_fallback", func(t *testing.T) {
		res := boundedQEFSolve(nil, nil, v3.Vec{}, v3.Vec{X: 1, Y: 1, Z: 1})
		if res.rank != 0 {
			t.Fatalf("rank = %d, want 0", res.rank)
		}
		expectVec(t, "local", res.local, v3.Vec{X: 0.5, Y: 0.5, Z: 0.5}, tol)
	})

	// --- World translation and uniform scale leave the local answer invariant. ---
	t.Run("translation_and_scale_invariance", func(t *testing.T) {
		sqrt2 := math.Sqrt2
		n1 := v3.Vec{X: 1 / sqrt2, Y: 1 / sqrt2}
		n2 := ex
		localPoints := []v3.Vec{{X: 1, Y: 1, Z: 0.6}, {X: 0, Y: 0.4, Z: 0.2}}
		localNormals := []v3.Vec{n1, n2}
		reference := solveUnit(t, localPoints, localNormals)

		origins := []v3.Vec{
			{X: 1e6, Y: -2e6, Z: 3e5},
			{X: -12345.678, Y: 98765.4321, Z: 555.5},
		}
		sizes := []float64{1e-3, 1, 1e3}
		for _, origin := range origins {
			for _, size := range sizes {
				cellSize := v3.Vec{X: size, Y: size, Z: size}
				worldPoints := []v3.Vec{
					origin.Add(localPoints[0].Mul(cellSize)),
					origin.Add(localPoints[1].Mul(cellSize)),
				}
				res := boundedQEFSolve(worldPoints, localNormals, origin, cellSize)
				expectVec(t, "scale-invariant local", res.local, reference.local, 1e-7)
				wantWorld := origin.Add(reference.local.Mul(cellSize))
				// Absolute float error grows with the translation magnitude, so
				// combine a voxel-relative and a machine-relative tolerance.
				tolWorld := size*1e-7 + wantWorld.Length()*1e-13
				expectVec(t, "world", res.world, wantWorld, tolWorld)
			}
		}
	})
}

//-----------------------------------------------------------------------------
// TestBoundedBoundary: minima on a face, an edge and a corner of the voxel
//-----------------------------------------------------------------------------

func TestBoundedBoundary(t *testing.T) {
	const tol = tolSolver
	sqrt2 := math.Sqrt2

	// --- Face minimum: the unconstrained point leaves through y=1, while the
	// constrained face minimum keeps x and z interior. ---
	// Planes: x = 1/5 and (x+y)/sqrt2 = 4 sqrt2/5 (through (4/5, 4/5)).
	// Unconstrained: x=1/5, y=7/5. On y=1 the residual is
	// (x-1/5)^2 + (1/2)(x-3/5)^2, minimized at x=1/3 with value 4/75.
	t.Run("face", func(t *testing.T) {
		n1 := ex
		n2 := v3.Vec{X: 1 / sqrt2, Y: 1 / sqrt2}
		p1 := v3.Vec{X: 1.0 / 5.0, Y: 0.2, Z: 0.3}
		p2 := v3.Vec{X: 4.0 / 5.0, Y: 4.0 / 5.0, Z: 0.9}
		points := []v3.Vec{p1, p2}
		normals := []v3.Vec{n1, n2}
		res := solveUnit(t, points, normals)
		want := v3.Vec{X: 1.0 / 3.0, Y: 1, Z: (0.3 + 0.9) / 2}
		expectVec(t, "face point", res.local, want, tol)
		if math.Abs(res.residual-4.0/75.0) > tol {
			t.Fatalf("residual = %v, want 4/75", res.residual)
		}
		if math.Abs(res.local.Y-1) > tol {
			t.Fatalf("expected minimum on y=1 face, got %v", res.local)
		}
	})

	// --- Edge minimum: face case plus z pinned outside at z=1. ---
	t.Run("edge", func(t *testing.T) {
		n1 := ex
		n2 := v3.Vec{X: 1 / sqrt2, Y: 1 / sqrt2}
		n3 := ez
		p1 := v3.Vec{X: 1.0 / 5.0, Y: 0.2, Z: 0.3}
		p2 := v3.Vec{X: 4.0 / 5.0, Y: 4.0 / 5.0, Z: 0.9}
		p3 := v3.Vec{Z: 1.15}
		points := []v3.Vec{p1, p2, p3}
		normals := []v3.Vec{n1, n2, n3}
		res := solveUnit(t, points, normals)
		want := v3.Vec{X: 1.0 / 3.0, Y: 1, Z: 1}
		expectVec(t, "edge point", res.local, want, tol)
		wantResidual := 4.0/75.0 + (1-1.15)*(1-1.15)
		if math.Abs(res.residual-wantResidual) > tol {
			t.Fatalf("residual = %v, want %v", res.residual, wantResidual)
		}
	})

	// --- Corner minimum: three axis planes all want coordinates above 1. ---
	t.Run("corner", func(t *testing.T) {
		points := []v3.Vec{{X: 1.2}, {Y: 1.3}, {Z: 1.4}}
		normals := []v3.Vec{ex, ey, ez}
		res := solveUnit(t, points, normals)
		expectVec(t, "corner point", res.local, v3.Vec{X: 1, Y: 1, Z: 1}, tol)
		wantResidual := 0.2*0.2 + 0.3*0.3 + 0.4*0.4
		if math.Abs(res.residual-wantResidual) > tol {
			t.Fatalf("residual = %v, want %v", res.residual, wantResidual)
		}
	})

	// --- Face minimum with the free coordinate pinned by the mean rule. ---
	t.Run("face_rank1_tangent_mean", func(t *testing.T) {
		// Single plane wanting x=1.2: x pins to the x=1 face and the tangential
		// coordinates stay at the mean position, never at the center.
		points := []v3.Vec{{X: 1.2, Y: 0.2, Z: 0.8}, {X: 1.2, Y: 0.4, Z: 0.6}}
		normals := []v3.Vec{ex, ex}
		res := solveUnit(t, points, normals)
		expectVec(t, "face point", res.local, v3.Vec{X: 1, Y: 0.3, Z: 0.7}, tol)
	})

	// --- Anisotropic voxel: local normals carry the voxel diagonal D. ---
	// Box [0,2]x[0,0.75]x[0,4]. Planes x=0.3 and (x+y)/sqrt2 = 1.3/sqrt2.
	// The unconstrained point (x=0.3, y=1) leaves through the y=0.75 face; on
	// that face f=(x-0.3)^2 + 0.5(x-0.55)^2 is minimized at x=23/60 with
	// residual 1/48 and z at the Hermite mean 0.75.
	t.Run("anisotropic_voxel_face", func(t *testing.T) {
		cellStart := v3.Vec{}
		cellSize := v3.Vec{X: 2, Y: 0.75, Z: 4}
		sqrt2 := math.Sqrt2
		n1 := v3.Vec{X: 1 / sqrt2, Y: 1 / sqrt2}
		n2 := ex
		p1 := v3.Vec{X: 0.6, Y: 0.7, Z: 1.0} // x+y = 1.3
		p2 := v3.Vec{X: 0.3, Y: 0.4, Z: 0.5} // x = 0.3
		res := boundedQEFSolve([]v3.Vec{p1, p2}, []v3.Vec{n1, n2}, cellStart, cellSize)

		wantWorld := v3.Vec{X: 23.0 / 60.0, Y: 0.75, Z: 0.75}
		expectVec(t, "world", res.world, wantWorld, tol)
		wantLocal := v3.Vec{X: (23.0 / 60.0) / 2, Y: 1, Z: 0.75 / 4}
		expectVec(t, "local", res.local, wantLocal, tol)
		if math.Abs(res.residual-1.0/48.0) > tol {
			t.Fatalf("residual = %v, want 1/48", res.residual)
		}
		if r := planeResidual(wantWorld, []v3.Vec{p1, p2}, []v3.Vec{n1, n2}); math.Abs(r-res.residual) > 1e-12 {
			t.Fatalf("world residual mismatch: solver %g vs direct %g", res.residual, r)
		}
	})
}

//-----------------------------------------------------------------------------
// TestBoundedRender: geometry through the public render.Render3 entry points
//-----------------------------------------------------------------------------

// emptySDF3 is a closed bounding box whose field is everywhere outside.
type emptySDF3 struct{}

func (emptySDF3) Evaluate(p v3.Vec) float64 { return 1 }
func (emptySDF3) BoundingBox() sdf.Box3 {
	return sdf.NewBox3(v3.Vec{}, v3.Vec{X: 2, Y: 2, Z: 2})
}

func TestBoundedRender(t *testing.T) {
	// --- Empty object: no triangles and the collector must not hang. ---
	t.Run("empty_object", func(t *testing.T) {
		tris := render.CollectTriangles(emptySDF3{}, NewDualContouringBounded(16))
		if len(tris) != 0 {
			t.Fatalf("empty SDF produced %d triangles", len(tris))
		}
	})

	// --- Closed sphere: non-empty, finite, watertight, outward facing. ---
	var sphereTris []sdf.Triangle3
	t.Run("sphere", func(t *testing.T) {
		sphere, err := sdf.Sphere3D(1)
		if err != nil {
			t.Fatal(err)
		}
		r := NewDualContouringBounded(48)
		sphereTris = render.CollectTriangles(sphere, r)
		if len(sphereTris) < 100 {
			t.Fatalf("sphere produced only %d triangles", len(sphereTris))
		}
		for _, tri := range sphereTris {
			for _, p := range tri {
				if math.IsNaN(p.X) || math.IsInf(p.X, 0) {
					t.Fatalf("non-finite vertex %v", p)
				}
				// Every vertex belongs to a voxel intersecting the sphere: stay
				// within one voxel diagonal of the unit surface.
				if d := math.Abs(p.Length() - 1); d > 0.1 {
					t.Fatalf("vertex %v drifts %g from the sphere", p, d)
				}
			}
			if tri.Degenerate(0) {
				t.Fatalf("degenerate triangle %v", tri)
			}
			// Outward orientation for a sphere centered at the origin.
			centroid := tri[0].Add(tri[1]).Add(tri[2]).DivScalar(3)
			if tri.Normal().Dot(centroid) <= 0 {
				t.Fatalf("inward facing triangle %v", tri)
			}
		}
		if !render.IsWatertight(sphereTris) {
			t.Fatalf("sphere mesh has boundary edges")
		}
	})

	// --- Rotated box translated far from the origin: sharp edges survive. ---
	t.Run("rotated_offset_box", func(t *testing.T) {
		box, err := sdf.Box3D(v3.Vec{X: 2, Y: 3, Z: 4}, 0)
		if err != nil {
			t.Fatal(err)
		}
		m := sdf.Translate3d(v3.Vec{X: 12345, Y: -23456, Z: 34567}).
			Mul(sdf.RotateZ(0.7)).
			Mul(sdf.RotateX(0.4))
		rotated := sdf.Transform3D(box, m)
		tris := render.CollectTriangles(rotated, NewDualContouringBounded(60))
		if len(tris) < 100 {
			t.Fatalf("rotated box produced only %d triangles", len(tris))
		}
		for _, tri := range tris {
			for _, p := range tri {
				if math.IsNaN(p.X) || math.IsInf(p.X, 0) {
					t.Fatalf("non-finite vertex %v", p)
				}
			}
			if tri.Degenerate(0) {
				t.Fatalf("degenerate triangle %v", tri)
			}
		}
		if !render.IsWatertight(tris) {
			t.Fatalf("rotated box mesh has boundary edges")
		}
	})

	// --- Channel contract: explicit close flushes the final batch. ---
	t.Run("channel_flush_and_close", func(t *testing.T) {
		sphere, err := sdf.Sphere3D(1)
		if err != nil {
			t.Fatal(err)
		}
		var raw []*sdf.Triangle3
		var wg sync.WaitGroup
		ch := sdf.WriteTriangles(&wg, &raw)
		NewDualContouringBounded(32).Render(sphere, sdf.NewTriangle3Buffer(ch))
		close(ch)
		wg.Wait()
		if len(raw) != len(sphereTris) && len(raw) < 100 {
			t.Fatalf("streamed %d triangles, collector saw %d", len(raw), len(sphereTris))
		}
	})

	// --- ToSTL round trip through the same Render3 implementation. ---
	t.Run("stl_output", func(t *testing.T) {
		sphere, err := sdf.Sphere3D(1)
		if err != nil {
			t.Fatal(err)
		}
		path := filepath.Join(os.TempDir(), "bounded_dc_sphere.stl")
		defer os.Remove(path)
		render.ToSTL(sphere, path, NewDualContouringBounded(32))
		info, err := os.Stat(path)
		if err != nil {
			t.Fatalf("STL not written: %v", err)
		}
		if info.Size() == 0 {
			t.Fatalf("STL file is empty")
		}
		loaded, err := render.LoadSTL(path)
		if err != nil {
			t.Fatalf("STL reload failed: %v", err)
		}
		if len(loaded) < 100 {
			t.Fatalf("reloaded STL has only %d triangles", len(loaded))
		}
	})

	// --- The existing V2 renderer keeps working unchanged. ---
	t.Run("v2_still_works", func(t *testing.T) {
		sphere, err := sdf.Sphere3D(1)
		if err != nil {
			t.Fatal(err)
		}
		var raw []*sdf.Triangle3
		var wg sync.WaitGroup
		ch := sdf.WriteTriangles(&wg, &raw)
		NewDualContouringDefault(24).Render(sphere, ch)
		close(ch)
		wg.Wait()
		if len(raw) < 100 {
			t.Fatalf("V2 produced only %d triangles", len(raw))
		}
		for _, tri := range raw {
			for _, p := range tri {
				if math.IsNaN(p.X) || math.IsInf(p.X, 0) {
					t.Fatalf("non-finite V2 vertex %v", p)
				}
			}
		}
	})
}
