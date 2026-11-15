package simplify

import (
	"math"
	"sort"
	"strconv"
	"testing"

	"github.com/fogleman/simplify/task_support/taskcheck"
)

// ---- independent helpers (recomputed from the spec, not the implementation) ----

func testIdentityView(w, h float64) View {
	return View{
		Matrix: [16]float64{
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1,
		},
		Width: w, Height: h,
	}
}

func testPerspView(k, w, h float64) View {
	return View{
		Matrix: [16]float64{
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			k, 0, 0, 1,
		},
		Width: w, Height: h,
	}
}

// projectPixel applies the row-major view matrix with W=1 and returns the
// pixel coordinate exactly as the spec defines.
func projectPixel(v View, p Vector) (float64, float64) {
	m := v.Matrix
	x := m[0]*p.X + m[1]*p.Y + m[2]*p.Z + m[3]
	y := m[4]*p.X + m[5]*p.Y + m[6]*p.Z + m[7]
	w := m[12]*p.X + m[13]*p.Y + m[14]*p.Z + m[15]
	return (x/w + 1) * v.Width / 2, (y/w + 1) * v.Height / 2
}

func pixelDistance(v View, a, b Vector) float64 {
	ax, ay := projectPixel(v, a)
	bx, by := projectPixel(v, b)
	return math.Hypot(ax-bx, ay-by)
}

// segmentW returns the homogeneous W along a+t*(b-a).
func segmentW(v View, a, b Vector, t float64) float64 {
	p := a.Add(b.Sub(a).MulScalar(t))
	m := v.Matrix
	return m[12]*p.X + m[13]*p.Y + m[14]*p.Z + m[15]
}

// independentBand scans the exact feasible t range of one edge by sampling
// only for test verification (the implementation itself never samples).
// It applies every constraint of both endpoint clusters in every view.
func independentBand(views []View, membersA, membersB []Vector, r float64) (float64, float64) {
	const steps = 200000
	lo, hi := 2.0, -1.0
	for i := 0; i <= steps; i++ {
		t := float64(i) / steps
		ok := true
		for _, v := range views {
			if segmentW(v, membersA[0], membersB[0], t) < 1e-6 {
				ok = false
				break
			}
			for _, p := range membersA {
				np := membersA[0].Add(membersB[0].Sub(membersA[0]).MulScalar(t))
				if pixelDistance(v, p, np) > r+1e-9 {
					ok = false
				}
			}
			for _, p := range membersB {
				np := membersA[0].Add(membersB[0].Sub(membersA[0]).MulScalar(t))
				if pixelDistance(v, p, np) > r+1e-9 {
					ok = false
				}
			}
		}
		if ok {
			if t < lo {
				lo = t
			}
			if t > hi {
				hi = t
			}
		}
	}
	return lo, hi
}

// independentEdgeBand is the two-endpoint version used for simple meshes.
func independentEdgeBand(views []View, a, b Vector, r float64) (float64, float64) {
	return independentBand(views, []Vector{a}, []Vector{b}, r)
}

func vecNear(a, b Vector, tol float64) bool {
	return math.Abs(a.X-b.X) <= tol && math.Abs(a.Y-b.Y) <= tol && math.Abs(a.Z-b.Z) <= tol
}

func repOf(r *ViewResult, p Vector) Vector {
	for i, o := range r.Original {
		if o == p {
			return r.Representative[i]
		}
	}
	return Vector{math.NaN(), math.NaN(), math.NaN()}
}

// narrowQuadMesh: AB is a boundary edge and, for the small budget, the
// only edge that can be contracted.
func narrowQuadMesh() *Mesh {
	A := Vector{0, 0, 0}
	B := Vector{1, 0, 0}
	C := Vector{0, 3, 0}
	E := Vector{1, 3, 0}
	return NewMesh([]*Triangle{
		NewTriangle(A, B, E),
		NewTriangle(A, E, C),
	})
}

// stripMesh: both endpoints of boundary edge AB carry tilted faces, so the
// merged accumulated quadric has a strict interior minimum along AB.
func stripMesh() *Mesh {
	A := Vector{2, 0, 0}
	B := Vector{3, 0, 0}
	E := Vector{2, 2, 0}
	C := Vector{2, 4, 0.4}
	H := Vector{0.3, 3, 0.3}
	G := Vector{4, 3.5, 1.5}
	return NewMesh([]*Triangle{
		NewTriangle(A, B, E),
		NewTriangle(A, E, C),
		NewTriangle(A, C, H),
		NewTriangle(B, G, E),
	})
}

func planeGrid17() *Mesh {
	const n = 17
	v := func(x, y int) Vector {
		xx := float64(x)/8 - 1
		yy := float64(y)/8 - 1
		return Vector{xx, yy, 0.15 * math.Sin(xx*2) * math.Cos(yy*3)}
	}
	var ts []*Triangle
	for y := 0; y < n-1; y++ {
		for x := 0; x < n-1; x++ {
			a, b, c, d := v(x, y), v(x+1, y), v(x+1, y+1), v(x, y+1)
			ts = append(ts, NewTriangle(a, b, c), NewTriangle(a, c, d))
		}
	}
	return NewMesh(ts)
}

// independentQuadric rebuilds the per-vertex accumulated face quadric the
// way plain Simplify does, then sums the two clusters' quadrics.
func independentMergedQuadric(m *Mesh, a, b Vector) Matrix {
	var qa, qb Matrix
	for _, t := range m.Triangles {
		q := t.Quadric()
		for _, p := range []Vector{t.V1, t.V2, t.V3} {
			if p == a {
				qa = qa.Add(q)
			}
			if p == b {
				qb = qb.Add(q)
			}
		}
	}
	return qa.Add(qb)
}

func edgeQEMCoeffs(q Matrix, a, b Vector) (aq, bq, cq float64) {
	d := b.Sub(a)
	aq = qForm(q, d.X, d.Y, d.Z, 0)
	cq = qForm(q, a.X, a.Y, a.Z, 1)
	bq = qForm(q, a.X+d.X, a.Y+d.Y, a.Z+d.Z, 1) - aq - cq
	return
}

// TestProjection covers the exact feasible interval, perspective
// projection, multiple cameras tightening the budget and the zero-budget
// guarantee.
func TestProjection(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// (1) perspective, narrow interior band; endpoints and midpoint infeasible.
		persp := testPerspView(1, 100, 100)
		m := narrowQuadMesh()
		A, B := Vector{0, 0, 0}, Vector{1, 0, 0}
		const budget = 15.0
		lo, hi := independentEdgeBand([]View{persp}, A, B, budget)
		c.Check(lo > 0 && hi < 1 && hi-lo < 0.25, "perspective band must be a narrow interior interval, got [%v,%v]", lo, hi)
		c.Check(pixelDistance(persp, A, A.Add(B.Sub(A).MulScalar(0.5))) > budget, "midpoint must violate the budget")
		c.Check(pixelDistance(persp, B, A) > budget, "t=0 endpoint must violate budget for B")
		c.Check(pixelDistance(persp, A, B) > budget, "t=1 endpoint must violate budget for A")

		res, err := SimplifyViews(m, 0.5, []View{persp}, budget)
		c.Check(err == nil, "SimplifyViews narrow quad: %v", err)
		// Only AB may contract: faces drop from 2 to 1 and just A,B move.
		c.Check(len(res.Mesh.Triangles) == 1, "only AB contracts, want 1 face, got %d", len(res.Mesh.Triangles))
		repA, repB := repOf(res, A), repOf(res, B)
		c.Check(repA == repB, "A and B must share a representative")
		gotT := repA.X // segment is along X from 0 to 1
		// Exact analytic roots for the perspective matrix W=1+X:
		// dB=15 -> t=1/4, dA=15 -> t=3/7.
		const wantLo, wantHi = 0.25, 3.0 / 7.0
		c.Check(math.Abs(lo-wantLo) < 1e-4 && math.Abs(hi-wantHi) < 1e-4,
			"independent scan [%.6f,%.6f] must match analytic [%v,%v]", lo, hi, wantLo, wantHi)
		c.Check(gotT >= wantLo-1e-9 && gotT <= wantHi+1e-9, "chosen t=%v must lie in analytic band [%v,%v]", gotT, wantLo, wantHi)
		// QEM is constant along the flat quad's AB: nearest feasible point
		// to 0.5 is the band's upper endpoint.
		c.Check(math.Abs(gotT-wantHi) <= 1e-9, "constant QEM picks point nearest 0.5 (band top %v), got %v", wantHi, gotT)
		for i, o := range res.Original {
			for _, v := range []View{persp} {
				c.Check(pixelDistance(v, o, res.Representative[i]) <= budget+1e-8,
					"pixel budget violated for point %v", o)
			}
		}

		// (2) multiple cameras mutually tighten the feasible interval.
		loose := testIdentityView(40, 40)
		const r2 = 14.0
		lLoose, hLoose := independentEdgeBand([]View{loose}, A, B, r2)
		lPersp, hPersp := independentEdgeBand([]View{persp}, A, B, r2)
		lBoth, hBoth := independentEdgeBand([]View{loose, persp}, A, B, r2)
		c.Check(lBoth >= math.Max(lLoose, lPersp)-1e-9 && hBoth <= math.Min(hLoose, hPersp)+1e-9,
			"combined band must be the intersection")
		c.Check(hBoth < hLoose && lBoth > lPersp, "both cameras must tighten the band")
		resLoose, e1 := SimplifyViews(m, 0.5, []View{loose}, r2)
		resPersp, e2 := SimplifyViews(m, 0.5, []View{persp}, r2)
		resBoth, e3 := SimplifyViews(m, 0.5, []View{loose, persp}, r2)
		c.Check(e1 == nil && e2 == nil && e3 == nil, "two-camera runs must not error")
		tLoose, tPersp, tBoth := repOf(resLoose, A).X, repOf(resPersp, A).X, repOf(resBoth, A).X
		// Analytic combined band: ortho gives [0.3,0.7], perspective gives
		// [11/39, 7/18]; the intersection is [0.3, 7/18].
		const bLo, bHi = 0.3, 7.0 / 18.0
		c.Check(math.Abs(lBoth-bLo) < 1e-4 && math.Abs(hBoth-bHi) < 1e-4,
			"scanned combined band [%.6f,%.6f] must match analytic [%v,%v]", lBoth, hBoth, bLo, bHi)
		c.Check(math.Abs(tBoth-bHi) <= 1e-9, "constant QEM picks combined band top %v, got %v", bHi, tBoth)
		c.Check(tLoose != tBoth || tPersp != tBoth, "combined choice must differ from at least one single camera")
		for i, o := range resBoth.Original {
			for _, v := range []View{loose, persp} {
				c.Check(pixelDistance(v, o, resBoth.Representative[i]) <= r2+1e-8,
					"combined pixel budget violated for %v", o)
			}
		}
		_ = tLoose
		_ = tPersp

		// (3) zero budget: no original point may move at all.
		resZero, err := SimplifyViews(m, 0.5, []View{persp}, 0)
		c.Check(err == nil, "zero budget run: %v", err)
		for i, o := range resZero.Original {
			c.Check(resZero.Representative[i] == o, "zero budget must leave every point fixed")
		}
	})
}

// TestClusters covers accumulation against the original input (not step
// endpoints), cluster bookkeeping and the interior QEM optimum.
func TestClusters(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		sm := stripMesh()
		A, B := Vector{2, 0, 0}, Vector{3, 0, 0}
		view := testIdentityView(200, 200)
		const budget = 90.0

		// AB is the only edge feasible at this budget.
		edgeSet := map[[2]Vector]bool{}
		for _, tr := range sm.Triangles {
			ps := [3]Vector{tr.V1, tr.V2, tr.V3}
			for k := 0; k < 3; k++ {
				a, b := ps[k], ps[(k+1)%3]
				if b.Less(a) {
					a, b = b, a
				}
				edgeSet[[2]Vector{a, b}] = true
			}
		}
		feasibleEdges := 0
		for e := range edgeSet {
			lo, hi := independentEdgeBand([]View{view}, e[0], e[1], budget)
			if lo <= hi {
				feasibleEdges++
				c.Check(e[0] == A && e[1] == B, "only AB may be feasible")
			}
		}
		c.Check(feasibleEdges == 1, "exactly one feasible edge, got %d", feasibleEdges)

		// Independent accumulated quadric and its interior minimum.
		q := independentMergedQuadric(sm, A, B)
		aq, bq, _ := edgeQEMCoeffs(q, A, B)
		tStar := -bq / (2 * aq)
		c.Check(aq > 0 && tStar > 0 && tStar < 1, "merged quadric must have an interior optimum, got t*=%v", tStar)
		lo, hi := independentEdgeBand([]View{view}, A, B, budget)
		c.Check(tStar >= lo && tStar <= hi, "interior optimum %v must be feasible in [%v,%v]", tStar, lo, hi)

		res, err := SimplifyViews(sm, 0.5, []View{view}, budget)
		c.Check(err == nil, "strip run: %v", err)
		c.Check(len(res.Mesh.Triangles) == 3, "one contraction removes one face, got %d", len(res.Mesh.Triangles))
		repA := repOf(res, A)
		gotT := repA.X - A.X
		c.Check(math.Abs(gotT-tStar) <= 1e-9, "chosen t=%v must match independent interior optimum %v", gotT, tStar)
		c.Check(repOf(res, B) == repA, "B must join A's cluster")
		// Every other original point stays fixed.
		for _, o := range res.Original {
			if o == A || o == B {
				continue
			}
			c.Check(repOf(res, o) == o, "uncontracted point %v must stay fixed", o)
		}
		// Representatives of A and B equal the same cluster position, and
		// the displacement respects the budget in the only view.
		for i, o := range res.Original {
			c.Check(pixelDistance(view, o, res.Representative[i]) <= budget+1e-8,
				"cluster budget violated for %v", o)
		}

		// Accumulation check on the plane grid: every representative must
		// satisfy the budget against every original point of its whole
		// cluster, not just the last merge's endpoints. Reconstruct
		// clusters from the returned correspondence.
		grid := planeGrid17()
		gv := testIdentityView(16, 16)
		gres, err := SimplifyViews(grid, 0.5, []View{gv}, 2.0)
		c.Check(err == nil, "grid run: %v", err)
		clusterMembers := map[Vector][]Vector{}
		for i, o := range gres.Original {
			r := gres.Representative[i]
			clusterMembers[r] = append(clusterMembers[r], o)
		}
		for rep, members := range clusterMembers {
			for _, o := range members {
				c.Check(pixelDistance(gv, o, rep) <= 2.0+1e-8,
					"accumulated cluster constraint violated for %v -> %v", o, rep)
			}
		}
		c.Check(len(gres.Original) == len(gres.Representative), "correspondence lengths must match")
	})
}

// TestTopology covers manifold/link/boundary preservation, canonical
// ordering, determinism, input immutability and validation errors.
func TestTopology(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		grid := planeGrid17()
		view := testIdentityView(16, 16)
		inputSnapshot := snapshotMesh(grid)

		res, err := SimplifyViews(grid, 0.5, []View{view}, 2.0)
		c.Check(err == nil, "topology run: %v", err)

		// Input mesh is untouched.
		c.Check(snapshotMesh(grid) == inputSnapshot, "input mesh must not be modified")

		// At least 30% face reduction; the budget must be met by real
		// contractions, not by refusing everything.
		inFaces, outFaces := len(grid.Triangles), len(res.Mesh.Triangles)
		c.Check(float64(inFaces-outFaces)/float64(inFaces) >= 0.30,
			"must reduce at least 30%%, got %d -> %d", inFaces, outFaces)
		c.Check(outFaces > 0, "result must stay non-empty")

		// Output faces are non-degenerate, unique and manifold.
		faceKeys := map[[3]Vector]bool{}
		edgeFaceCount := map[[2]Vector]int{}
		for _, tr := range res.Mesh.Triangles {
			c.Check(!vecNear(tr.V1, tr.V2, 0) && !vecNear(tr.V1, tr.V3, 0) && !vecNear(tr.V2, tr.V3, 0),
				"output contains a degenerate face")
			key := [3]Vector{tr.V1, tr.V2, tr.V3}
			c.Check(!faceKeys[key], "duplicate output face")
			faceKeys[key] = true
			ps := [3]Vector{tr.V1, tr.V2, tr.V3}
			for k := 0; k < 3; k++ {
				a, b := ps[k], ps[(k+1)%3]
				if b.Less(a) {
					a, b = b, a
				}
				edgeFaceCount[[2]Vector{a, b}]++
			}
		}
		for e, n := range edgeFaceCount {
			c.Check(n <= 2, "edge %v has %d faces, output is not manifold", e, n)
		}

		// Every output vertex is a representative position.
		repSet := map[Vector]bool{}
		for _, r := range res.Representative {
			repSet[r] = true
		}
		for _, tr := range res.Mesh.Triangles {
			for _, p := range []Vector{tr.V1, tr.V2, tr.V3} {
				c.Check(repSet[p], "output vertex %v is not a cluster representative", p)
			}
		}

		// Canonical ordering by each cluster's minimum original index.
		repMinIndex := map[Vector]int{}
		for i, rep := range res.Representative {
			if cur, ok := repMinIndex[rep]; !ok || i < cur {
				repMinIndex[rep] = i
			}
		}
		var prev [3]int
		first := true
		for _, tr := range res.Mesh.Triangles {
			idx := sortedTriple([3]int{
				repMinIndex[tr.V1],
				repMinIndex[tr.V2],
				repMinIndex[tr.V3],
			})
			if !first {
				greater := false
				for k := 0; k < 3; k++ {
					if idx[k] != prev[k] {
						greater = idx[k] > prev[k]
						break
					}
				}
				c.Check(greater, "output faces must be canonically sorted")
			}
			prev, first = idx, false
		}

		// Determinism: repeated runs give identical triangles.
		res2, err := SimplifyViews(planeGrid17(), 0.5, []View{view}, 2.0)
		c.Check(err == nil, "repeat run: %v", err)
		c.Check(len(res2.Mesh.Triangles) == len(res.Mesh.Triangles), "repeat run face count differs")
		same := true
		for i := range res.Mesh.Triangles {
			a, b := res.Mesh.Triangles[i], res2.Mesh.Triangles[i]
			if a.V1 != b.V1 || a.V2 != b.V2 || a.V3 != b.V3 {
				same = false
			}
		}
		c.Check(same, "repeated runs must be identical")

		// Rejected contractions leave other regions simplifiable: a
		// component whose every edge violates the budget is preserved
		// while a fine component elsewhere still contracts.
		twoComp := twoComponentMesh()
		tv := testIdentityView(200, 200)
		tres, err := SimplifyViews(twoComp, 0.5, []View{tv}, 8.0)
		c.Check(err == nil, "two-component run: %v", err)
		frozenMoved := 0
		for i, o := range tres.Original {
			if o.X < 5 && tres.Representative[i] != o {
				frozenMoved++
			}
		}
		c.Check(frozenMoved == 0, "over-budget component must remain fixed")
		c.Check(len(tres.Mesh.Triangles) < len(twoComp.Triangles), "legal region must still simplify")
		for i, o := range tres.Original {
			c.Check(pixelDistance(tv, o, tres.Representative[i]) <= 8.0+1e-8,
				"two-component budget violated for %v", o)
		}

		// Validation errors must be returned without touching the input.
		bad := []struct {
			name   string
			factor float64
			views  []View
			budget float64
		}{
			{"factor zero", 0, []View{view}, 1},
			{"factor negative", -0.5, []View{view}, 1},
			{"factor above one", 1.5, []View{view}, 1},
			{"no views", 0.5, nil, 1},
			{"too many views", 0.5, make([]View, 9), 1},
			{"negative budget", 0.5, []View{view}, -1},
			{"NaN budget", 0.5, []View{view}, math.NaN()},
			{"zero viewport", 0.5, []View{testIdentityView(0, 16)}, 1},
		}
		for _, tc := range bad {
			_, err := SimplifyViews(grid, tc.factor, tc.views, tc.budget)
			c.Check(err != nil, "%s must return an error", tc.name)
		}
		c.Check(snapshotMesh(grid) == inputSnapshot, "input unchanged after errors")
	})
}

func twoComponentMesh() *Mesh {
	A := Vector{0, 0, 0}
	B := Vector{1, 0, 0}
	C := Vector{0, 3, 0}
	E := Vector{1, 3, 0}
	tris := []*Triangle{
		NewTriangle(A, B, E),
		NewTriangle(A, E, C),
	}
	g := func(i, j int) Vector {
		return Vector{10 + 0.05*float64(i), 10 + 0.05*float64(j), 0}
	}
	for j := 0; j < 2; j++ {
		for i := 0; i < 2; i++ {
			a, b, c, d := g(i, j), g(i+1, j), g(i+1, j+1), g(i, j+1)
			tris = append(tris, NewTriangle(a, b, c), NewTriangle(a, c, d))
		}
	}
	return NewMesh(tris)
}

func snapshotMesh(m *Mesh) string {
	type tri struct{ a, b, c Vector }
	tris := make([]tri, len(m.Triangles))
	for i, t := range m.Triangles {
		tris[i] = tri{t.V1, t.V2, t.V3}
	}
	sort.Slice(tris, func(i, j int) bool {
		for k := 0; k < 3; k++ {
			x := [3]Vector{tris[i].a, tris[i].b, tris[i].c}[k]
			y := [3]Vector{tris[j].a, tris[j].b, tris[j].c}[k]
			if x != y {
				return x.Less(y)
			}
		}
		return false
	})
	out := ""
	for _, t := range tris {
		out += fmtVec(t.a) + "|" + fmtVec(t.b) + "|" + fmtVec(t.c) + ";"
	}
	return out
}

func fmtVec(v Vector) string {
	return floatToStr(v.X) + "," + floatToStr(v.Y) + "," + floatToStr(v.Z)
}

func floatToStr(f float64) string {
	return strconv.FormatUint(math.Float64bits(f), 16)
}
