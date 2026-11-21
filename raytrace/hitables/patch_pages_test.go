package hitables

import (
	"math"
	"math/rand"
	"sync"
	"testing"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/cameras"
	"github.com/markzuber/zgotrace/raytrace/materials"
	"github.com/markzuber/zgotrace/raytrace/textures"
	"github.com/markzuber/zgotrace/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// Independent reference implementation used only by the tests.
// ---------------------------------------------------------------------------

func refBernstein(t float64) [4]float64 {
	mt := 1.0 - t
	return [4]float64{mt * mt * mt, 3 * t * mt * mt, 3 * t * t * mt, t * t * t}
}

func refEvaluate(control [16]mgl64.Vec3, u, v float64) mgl64.Vec3 {
	bu := refBernstein(u)
	bv := refBernstein(v)
	var total [3]float64
	for j := 0; j < 4; j++ {
		for i := 0; i < 4; i++ {
			w := bu[i] * bv[j]
			p := control[j*4+i]
			total[0] += p.X() * w
			total[1] += p.Y() * w
			total[2] += p.Z() * w
		}
	}
	return mgl64.Vec3{total[0], total[1], total[2]}
}

type refTriangle struct {
	a, b, c mgl64.Vec3
	uvA     mgl64.Vec2
	uvB     mgl64.Vec2
	uvC     mgl64.Vec2
	cell    int
	tri     int
}

// refTriangles enumerates every discrete triangle of one patch.
func refTriangles(control [16]mgl64.Vec3, level int) []refTriangle {
	n := 1 << uint(level)
	cell := 1.0 / float64(n)
	out := make([]refTriangle, 0, n*n*2)
	for cv := 0; cv < n; cv++ {
		for cu := 0; cu < n; cu++ {
			u0 := float64(cu) * cell
			u1 := float64(cu+1) * cell
			v0 := float64(cv) * cell
			v1 := float64(cv+1) * cell
			p00 := refEvaluate(control, u0, v0)
			p10 := refEvaluate(control, u1, v0)
			p11 := refEvaluate(control, u1, v1)
			p01 := refEvaluate(control, u0, v1)
			uv00 := mgl64.Vec2{u0, v0}
			uv10 := mgl64.Vec2{u1, v0}
			uv11 := mgl64.Vec2{u1, v1}
			uv01 := mgl64.Vec2{u0, v1}
			id := cv*n + cu
			out = append(out, refTriangle{p00, p10, p11, uv00, uv10, uv11, id, 0})
			out = append(out, refTriangle{p00, p11, p01, uv00, uv11, uv01, id, 1})
		}
	}
	return out
}

func refNormal(t refTriangle) (mgl64.Vec3, bool) {
	cross := t.b.Sub(t.a).Cross(t.c.Sub(t.a))
	l := cross.Len()
	if l == 0 || math.IsNaN(l) || math.IsInf(l, 0) {
		return mgl64.Vec3{}, false
	}
	return cross.Mul(1.0 / l), true
}

func refIntersect(ray *raytrace.Ray, t refTriangle, tMin, tMax float64) (float64, mgl64.Vec2, bool) {
	e1 := t.b.Sub(t.a)
	e2 := t.c.Sub(t.a)
	h := ray.Direction().Cross(e2)
	a := e1.Dot(h)
	if a == 0 {
		return 0, mgl64.Vec2{}, false
	}
	f := 1.0 / a
	s := ray.Origin().Sub(t.a)
	u := f * s.Dot(h)
	if u < 0 || u > 1 {
		return 0, mgl64.Vec2{}, false
	}
	q := s.Cross(e1)
	v := f * ray.Direction().Dot(q)
	if v < 0 || u+v > 1 {
		return 0, mgl64.Vec2{}, false
	}
	tt := f * e2.Dot(q)
	if math.IsNaN(tt) || tt < tMin || tt > tMax {
		return 0, mgl64.Vec2{}, false
	}
	w := 1.0 - u - v
	uv := t.uvA.Mul(w).Add(t.uvB.Mul(u)).Add(t.uvC.Mul(v))
	return tt, uv, true
}

func refHitTriangles(control [16]mgl64.Vec3, tris []refTriangle, ray *raytrace.Ray, tMin, tMax float64) (float64, mgl64.Vec2, mgl64.Vec3, bool) {
	best := math.Inf(1)
	var bestUv mgl64.Vec2
	var bestNormal mgl64.Vec3
	found := false
	for _, t := range tris {
		if _, ok := refNormal(t); !ok {
			continue
		}
		tt, uv, ok := refIntersect(ray, t, tMin, tMax)
		if !ok {
			continue
		}
		if tt < best {
			best = tt
			bestUv = uv
			n, _ := refNormal(t)
			bestNormal = n
			found = true
		}
	}
	return best, bestUv, bestNormal, found
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

var identityMaterial = materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(0.5, 0.5, 0.5)))

func flatPatch(z float64) PatchDesc {
	var control [16]mgl64.Vec3
	for j := 0; j < 4; j++ {
		for i := 0; i < 4; i++ {
			control[j*4+i] = mgl64.Vec3{float64(i) / 3.0, float64(j) / 3.0, z}
		}
	}
	return PatchDesc{Control: control, Material: identityMaterial}
}

func wavyPatch() PatchDesc {
	var control [16]mgl64.Vec3
	for j := 0; j < 4; j++ {
		for i := 0; i < 4; i++ {
			x := float64(i) / 3.0
			y := float64(j) / 3.0
			z := 0.35 * math.Sin(2.5*x+0.4) * math.Cos(1.7*y-0.2)
			control[j*4+i] = mgl64.Vec3{x, y, z}
		}
	}
	return PatchDesc{Control: control, Material: identityMaterial}
}

// foldedPatch has a control net that folds the surface back on itself.
func foldedPatch() PatchDesc {
	var control [16]mgl64.Vec3
	for j := 0; j < 4; j++ {
		for i := 0; i < 4; i++ {
			x := float64(i) / 3.0
			y := float64(j) / 3.0
			z := 0.55 * math.Sin(3.0*x*math.Pi) * math.Cos(2.0*y*math.Pi)
			if i > 1 {
				x = 1.0 - x // fold back along u
			}
			control[j*4+i] = mgl64.Vec3{x, y, z}
		}
	}
	return PatchDesc{Control: control, Material: identityMaterial}
}

func degeneratePatch() PatchDesc {
	var control [16]mgl64.Vec3
	for i := range control {
		control[i] = mgl64.Vec3{0, 0, 0}
	}
	return PatchDesc{Control: control, Material: identityMaterial}
}

func mustPatchSet(t *testing.T, patches []PatchDesc, level, resident int) *PatchSet {
	t.Helper()
	ps, err := NewPatchSet(patches, level, resident)
	if err != nil {
		t.Fatalf("NewPatchSet: %v", err)
	}
	return ps
}

func closeVec(a, b mgl64.Vec3, eps float64) bool {
	return math.Abs(a.X()-b.X()) < eps && math.Abs(a.Y()-b.Y()) < eps && math.Abs(a.Z()-b.Z()) < eps
}

func closeVec2(a, b mgl64.Vec2, eps float64) bool {
	return math.Abs(a.X()-b.X()) < eps && math.Abs(a.Y()-b.Y()) < eps
}

// ---------------------------------------------------------------------------
// TestPatchGeometry
// ---------------------------------------------------------------------------

func TestPatchGeometry(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		testPatchGeometryConstruction(t, c)
		testPatchGeometryEvaluation(t, c)
		testPatchGeometryHits(t, c)
		testPatchGeometryTies(t, c)
		testPatchGeometryEdges(t, c)
		testPatchGeometryEmpty(t, c)
		testPatchGeometryUnsupportedLight(t, c)
	})
}

func testPatchGeometryConstruction(t *testing.T, c *taskcheck.C) {
	good := flatPatch(0)
	for _, level := range []int{2, 3, 10, 11} {
		_, err := NewPatchSet([]PatchDesc{good}, level, 1)
		c.Check((err == nil) == (level >= 3 && level <= 10), "level %d validation", level)
	}
	for _, resident := range []int{-1, 0} {
		_, err := NewPatchSet([]PatchDesc{good}, 3, resident)
		c.Check(err != nil, "residentPages %d must be rejected", resident)
	}

	ps := mustPatchSet(t, nil, 3, 1)
	empty := ps.GetBoundingBox(0, 1)
	c.Check(empty != nil, "empty set bounding box must be non-nil")
	if empty != nil {
		c.Check(math.IsInf(empty.Min().X(), 1) && math.IsInf(empty.Max().X(), -1), "empty box min/max infinities")
		c.Check(!empty.Hit(raytrace.NewRay(mgl64.Vec3{}, mgl64.Vec3{0, 0, -1}), -math.MaxFloat64, math.MaxFloat64), "empty box never hits")
	}
	c.Check(ps.Hit(raytrace.NewRay(mgl64.Vec3{}, mgl64.Vec3{0, 0, -1}), 0, 100) == nil, "empty set never hits")

	// Construction copies the description.
	var mutable [16]mgl64.Vec3
	for i := range mutable {
		mutable[i] = mgl64.Vec3{float64(i%4) / 3, float64(i/4) / 3, 0}
	}
	copied := PatchDesc{Control: mutable, Material: identityMaterial}
	copiedSet := mustPatchSet(t, []PatchDesc{copied}, 3, 2)
	for i := range mutable {
		mutable[i] = mgl64.Vec3{100, 100, 100}
	}
	hr := copiedSet.Hit(raytrace.NewRay(mgl64.Vec3{0.5, 0.5, 1}, mgl64.Vec3{0, 0, -1}), 0, 10)
	c.Check(hr != nil, "copied control points remain usable")
	if hr != nil {
		c.Check(math.Abs(hr.T()-1) < 1e-9, "copied control points keep their geometry, got t=%v", hr.T())
	}

	_, err := NewPatchSet([]PatchDesc{{Control: mutable, Material: nil}}, 3, 1)
	c.Check(err != nil, "nil material rejected")
	mutable[5] = mgl64.Vec3{math.NaN(), 0, 0}
	_, err = NewPatchSet([]PatchDesc{{Control: mutable, Material: identityMaterial}}, 3, 1)
	c.Check(err != nil, "non-finite control point rejected")

	// Root bounds cover the control net and are conservative.
	set := mustPatchSet(t, []PatchDesc{wavyPatch()}, 4, 2)
	box := set.GetBoundingBox(0, 1)
	c.Check(box.Min().X() <= 0 && box.Max().X() >= 1, "root box covers u range")
	c.Check(box.Min().Y() <= 0 && box.Max().Y() >= 1, "root box covers v range")

	// Empty boxes do not enlarge another object's bound.
	inf := math.Inf(1)
	emptyBox := raytrace.NewAABB(mgl64.Vec3{inf, inf, inf}, mgl64.Vec3{-inf, -inf, -inf})
	realBox := raytrace.NewAABB(mgl64.Vec3{0, 0, 0}, mgl64.Vec3{1, 1, 1})
	merged := realBox.GetSurroundingBox(emptyBox)
	c.Check(closeVec(merged.Min(), realBox.Min(), 1e-12) && closeVec(merged.Max(), realBox.Max(), 1e-12), "merging empty box keeps bounds")
	mergedReverse := emptyBox.GetSurroundingBox(realBox)
	c.Check(closeVec(mergedReverse.Min(), realBox.Min(), 1e-12) && closeVec(mergedReverse.Max(), realBox.Max(), 1e-12), "merging empty box keeps bounds (reverse)")
}

func testPatchGeometryEvaluation(t *testing.T, c *taskcheck.C) {
	level := 3
	patches := []PatchDesc{flatPatch(0.25), wavyPatch(), foldedPatch()}

	// Verify every page's generated triangles against the independent mesh.
	// A real query forces construction; each patch gets its own set so that a
	// nearer patch cannot shadow the page being inspected.
	for pi, desc := range patches {
		set := mustPatchSet(t, []PatchDesc{desc}, level, 64)
		refMesh := refTriangles(desc.Control, level)
		seenCells := map[[2]int]bool{}
		n := 1 << uint(level)
		pageCount := n / 8
		for pv := 0; pv < pageCount; pv++ {
			for pu := 0; pu < pageCount; pu++ {
				// Collect the sub-mesh of this page from the reference mesh.
				pageRef := make([]refTriangle, 0)
				for _, tri := range refMesh {
					cu := tri.cell % n
					cv := tri.cell / n
					if cu/8 == pu && cv/8 == pv {
						pageRef = append(pageRef, tri)
					}
				}
				// Force this page into the cache through a real query whose
				// ray passes through the parameter-domain center of the page.
				centerU := (float64(pu*8) + 4) / float64(n)
				centerV := (float64(pv*8) + 4) / float64(n)
				center := refEvaluate(desc.Control, centerU, centerV)
				origin := center.Add(mgl64.Vec3{0, 0, 1})
				ray := raytrace.NewRay(origin, mgl64.Vec3{0, 0, -1})
				set.Hit(ray, 0, 100)

				// Compare the page's stored triangles with the reference.
				key := pageKey{patch: 0, pv: pv, pu: pu}
				set.mu.Lock()
				page := set.pages[key]
				if page != nil {
					seenCells[[2]int{pu, pv}] = true
					c.Check(len(page.tris) == len(pageRef), "patch %d page %d,%d triangle count %d vs %d", pi, pu, pv, len(page.tris), len(pageRef))
					for k := range page.tris {
						if k >= len(pageRef) {
							break
						}
						got := page.tris[k]
						want := pageRef[k]
						c.Check(closeVec(got.v[0], want.a, 1e-12) && closeVec(got.v[1], want.b, 1e-12) && closeVec(got.v[2], want.c, 1e-12),
							"patch %d page %d,%d tri %d positions", pi, pu, pv, k)
						c.Check(closeVec2(got.uv[0], want.uvA, 1e-12) && closeVec2(got.uv[1], want.uvB, 1e-12) && closeVec2(got.uv[2], want.uvC, 1e-12),
							"patch %d page %d,%d tri %d uvs", pi, pu, pv, k)
						nrm, ok := refNormal(want)
						c.Check(ok, "reference normal for non-degenerate triangle")
						if ok {
							c.Check(closeVec(got.normal, nrm, 1e-12), "patch %d page %d,%d tri %d normal", pi, pu, pv, k)
						}
						c.Check(got.cell == want.cell && got.tri == want.tri, "patch %d page %d,%d tri %d identity", pi, pu, pv, k)
					}
				}
				set.mu.Unlock()
			}
		}
		c.Check(len(seenCells) == pageCount*pageCount, "patch %d all pages were built", pi)
	}
}

func testPatchGeometryHits(t *testing.T, c *taskcheck.C) {
	rng := rand.New(rand.NewSource(20261121))
	level := 3

	scenarios := []struct {
		name    string
		desc    PatchDesc
		zOffset float64
		rays    int
	}{
		{"flat", flatPatch(0), 0, 200},
		{"wavy", wavyPatch(), 0, 200},
		{"folded", foldedPatch(), 0, 200},
	}
	for _, sc := range scenarios {
		set := mustPatchSet(t, []PatchDesc{sc.desc}, level, 64)
		mesh := refTriangles(sc.desc.Control, level)
		for i := 0; i < sc.rays; i++ {
			// Non-normalized, sometimes reversed or tilted directions.
			ox := -0.4 + rng.Float64()*1.8
			oy := -0.4 + rng.Float64()*1.8
			oz := 1.5 + rng.Float64()
			dx := (rng.Float64() - 0.5) * 0.6
			dy := (rng.Float64() - 0.5) * 0.6
			dz := -(0.4 + rng.Float64()*2.5)
			ray := raytrace.NewRay(mgl64.Vec3{ox, oy, oz}, mgl64.Vec3{dx, dy, dz})
			tMin := rng.Float64() * 0.1
			tMax := 2.0 + rng.Float64()*3.0

			hr := set.Hit(ray, tMin, tMax)
			wantT, wantUv, wantN, wantHit := refHitTriangles(sc.desc.Control, mesh, ray, tMin, tMax)
			c.Check((hr != nil) == wantHit, "%s ray %d hit presence", sc.name, i)
			if hr == nil || !wantHit {
				continue
			}
			c.Check(math.Abs(hr.T()-wantT) < 1e-9, "%s ray %d t %v vs %v", sc.name, i, hr.T(), wantT)
			c.Check(closeVec2(hr.UvCoords(), wantUv, 1e-9), "%s ray %d uv", sc.name, i)
			c.Check(closeVec(hr.Normal(), wantN, 1e-9), "%s ray %d normal", sc.name, i)
			c.Check(closeVec(hr.P(), ray.GetPointAtParameter(hr.T()), 1e-12), "%s ray %d p", sc.name, i)
			c.Check(hr.Material() == identityMaterial, "%s ray %d material", sc.name, i)
		}

		// Closed distance window.
		center := refEvaluate(sc.desc.Control, 0.5, 0.5)
		ray := raytrace.NewRay(center.Add(mgl64.Vec3{0, 0, 2}), mgl64.Vec3{0, 0, -2})
		hr := set.Hit(ray, 0.0, 1.0)
		c.Check(hr != nil, "%s closed window includes endpoint", sc.name)
		hrBelow := set.Hit(ray, 0.0, 0.999)
		c.Check(hrBelow == nil, "%s closed window excludes just below", sc.name)
		hrAbove := set.Hit(ray, 1.0+1e-9, 2.0)
		c.Check(hrAbove == nil, "%s closed window excludes just above tMin", sc.name)
		hrEndpoint := set.Hit(ray, 1.0, 1.0)
		c.Check(hrEndpoint != nil, "%s closed window includes both endpoints", sc.name)
	}
}

func testPatchGeometryTies(t *testing.T, c *taskcheck.C) {
	// Three identical flat patches at z=0. Ties must go patch 0, then cell 0
	// (v-major), then triangle 0.
	desc := flatPatch(0)
	set := mustPatchSet(t, []PatchDesc{desc, desc, desc}, 3, 64)
	// Aim at the shared lower-left corner of the first cell: (u,v) = (0,0).
	ray := raytrace.NewRay(mgl64.Vec3{0, 0, 1}, mgl64.Vec3{0, 0, -1})
	hr := set.Hit(ray, 0, 10)
	c.Check(hr != nil, "tie ray hits")
	if hr != nil {
		c.Check(math.Abs(hr.T()-1.0) < 1e-12, "tie hit parameter")
		c.Check(closeVec2(hr.UvCoords(), mgl64.Vec2{0, 0}, 1e-9), "tie picks patch 0 cell 0")
	}

	// Distinguish patch identity with distinct materials.
	matA := materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(1, 0, 0)))
	matB := materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(0, 1, 0)))
	dA := desc
	dA.Material = matA
	dB := desc
	dB.Material = matB
	tieSet := mustPatchSet(t, []PatchDesc{dA, dB}, 3, 4)
	hr = tieSet.Hit(ray, 0, 10)
	c.Check(hr != nil && hr.Material() == matA, "tie prefers lower patch index")

	// Tie ordering across cells is observable through the v-major cell index
	// when the hit point sits on a shared grid vertex.  Rebuild the page at
	// level 4 so the tie point (1/16, 1/16) lies on the diagonal shared by
	// cells (0,0) and (1,1); cell (0,1) is v-major earlier than (1,1).
	fine := mustPatchSet(t, []PatchDesc{desc}, 4, 8)
	vertex := raytrace.NewRay(mgl64.Vec3{1.0 / 16.0, 1.0 / 16.0, 1}, mgl64.Vec3{0, 0, -1})
	hr = fine.Hit(vertex, 0, 10)
	c.Check(hr != nil, "grid vertex tie hits")
	if hr != nil {
		// The shared vertex is the 10 vertex of cell (0,0) and the 00 vertex
		// of cell (1,1); both belong to v-row 0, so the lower cell wins.
		c.Check(closeVec2(hr.UvCoords(), mgl64.Vec2{1.0 / 16.0, 1.0 / 16.0}, 1e-12), "grid vertex uv")
	}

	// Build a ray exactly on an internal page boundary diagonal, level 3 has
	// page size 8 == whole domain, so instead verify tie ordering across a
	// patch boundary with two patches at different z but identical geometry
	// reached at the same t.
	zA := flatPatch(0)
	zB := flatPatch(0)
	zB.Material = matB
	two := mustPatchSet(t, []PatchDesc{zA, zB}, 3, 2)
	hr = two.Hit(ray, 0, 10)
	c.Check(hr != nil && hr.Material() == identityMaterial, "identical geometry still prefers patch 0")
}

func testPatchGeometryEdges(t *testing.T, c *taskcheck.C) {
	desc := wavyPatch()
	level := 3
	set := mustPatchSet(t, []PatchDesc{desc}, level, 64)
	mesh := refTriangles(desc.Control, level)

	// Rays that graze the parameter-domain border.
	type edgeRay struct {
		u, v float64
	}
	edges := []edgeRay{{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0.5, 0}, {1, 0.5}, {0.5, 1}, {0, 0.5}}
	for _, e := range edges {
		p := refEvaluate(desc.Control, e.u, e.v)
		origin := p.Add(mgl64.Vec3{0, 0, 1.2})
		ray := raytrace.NewRay(origin, mgl64.Vec3{0, 0, -1.2})
		hr := set.Hit(ray, 0, 10)
		_, _, _, wantHit := refHitTriangles(desc.Control, mesh, ray, 0, 10)
		c.Check((hr != nil) == wantHit, "edge ray %v,%v presence", e.u, e.v)
		if hr != nil {
			c.Check(closeVec2(hr.UvCoords(), mgl64.Vec2{e.u, e.v}, 1e-9), "edge ray %v,%v uv", e.u, e.v)
		}
	}

	// Fully degenerate patch contributes no triangles and never hits.
	degSet := mustPatchSet(t, []PatchDesc{degeneratePatch()}, 3, 4)
	hr := degSet.Hit(raytrace.NewRay(mgl64.Vec3{0, 0, 1}, mgl64.Vec3{0, 0, -1}), 0, 10)
	c.Check(hr == nil, "degenerate patch never hits")

	// A partially degenerate patch (one row collapsed) still renders the rest.
	var partial [16]mgl64.Vec3
	base := wavyPatch()
	for i := range partial {
		partial[i] = base.Control[i]
	}
	for i := 0; i < 4; i++ {
		partial[i] = partial[4+i]
	}
	partialSet := mustPatchSet(t, []PatchDesc{{Control: partial, Material: identityMaterial}}, 3, 16)
	mesh = refTriangles(partial, 3)
	hitSome := false
	for i := 0; i < 50; i++ {
		u := rngEdge(i)
		p := refEvaluate(partial, u, 0.5)
		ray := raytrace.NewRay(p.Add(mgl64.Vec3{0, 0, 1}), mgl64.Vec3{0, 0, -1})
		hr := partialSet.Hit(ray, 0, 10)
		_, _, _, want := refHitTriangles(partial, mesh, ray, 0, 10)
		c.Check((hr != nil) == want, "partial degenerate ray %d presence", i)
		if hr != nil {
			hitSome = true
		}
	}
	c.Check(hitSome, "partially degenerate patch still has visible triangles")
}

func rngEdge(i int) float64 {
	return float64(i) / 49.0
}

func testPatchGeometryEmpty(t *testing.T, c *taskcheck.C) {
	// Degenerate patch (all control points identical) yields an empty usable
	// mesh but a valid root box.
	set := mustPatchSet(t, []PatchDesc{degeneratePatch()}, 3, 1)
	box := set.GetBoundingBox(0, 1)
	c.Check(box != nil, "degenerate patch box non-nil")
	c.Check(!box.IsEmpty(), "degenerate patch box is a point, not empty")
	ray := raytrace.NewRay(mgl64.Vec3{1, 1, 1}, mgl64.Vec3{0, 0, -1})
	c.Check(set.Hit(ray, 0, 100) == nil, "degenerate patch does not hit off-axis")
}

func testPatchGeometryUnsupportedLight(t *testing.T, c *taskcheck.C) {
	set := mustPatchSet(t, []PatchDesc{flatPatch(0)}, 3, 1)
	c.Check(set.GetPdfValue(mgl64.Vec3{}, mgl64.Vec3{0, 0, -1}) == 0, "GetPdfValue returns zero density")
	c.Check(closeVec(set.Random(mgl64.Vec3{}), mgl64.Vec3{1, 0, 0}, 1e-12), "Random returns fixed direction")
}

// ---------------------------------------------------------------------------
// TestPatchResidency
// ---------------------------------------------------------------------------

func TestPatchResidency(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		testLazyGeneration(t, c)
		testDistantRayColdCache(t, c)
		testPruningCounts(t, c)
		testCapacityOneChurn(t, c)
		testExpandedCapacity(t, c)
		testConcurrentQueries(t, c)
		testConcurrentStats(t, c)
	})
}

func testLazyGeneration(t *testing.T, c *taskcheck.C) {
	level := 6 // 64x64 cells, 8x8 pages => 64 pages
	desc := wavyPatch()
	set := mustPatchSet(t, []PatchDesc{desc}, level, 16)

	stats := set.Stats()
	c.Check(stats.Resident == 0 && stats.PageBuilds == 0, "cold cache has no pages, got %+v", stats)

	// A ray far away must not build anything.
	far := raytrace.NewRay(mgl64.Vec3{50, 50, 50}, mgl64.Vec3{1, 1, 1})
	c.Check(set.Hit(far, 0, 10) == nil, "far ray misses")
	stats = set.Stats()
	c.Check(stats.Resident == 0 && stats.PageBuilds == 0, "far ray builds no pages, got %+v", stats)

	// A local ray through the interior of one page builds only a few pages,
	// not the whole 64-page mesh.
	p := refEvaluate(desc.Control, 0.53, 0.47)
	ray := raytrace.NewRay(p.Add(mgl64.Vec3{0, 0, 2}), mgl64.Vec3{0, 0, -0.5})
	hr := set.Hit(ray, 0, 100)
	c.Check(hr != nil, "local ray hits")
	stats = set.Stats()
	c.Check(stats.PageBuilds >= 1 && stats.PageBuilds <= 4, "local ray builds a few pages, got %+v", stats)
	c.Check(stats.Resident >= 1 && stats.Resident <= 16, "resident bounded, got %+v", stats)
	c.Check(stats.PageBuilds < 64, "local ray does not build every page")
}

func testDistantRayColdCache(t *testing.T, c *taskcheck.C) {
	set := mustPatchSet(t, []PatchDesc{wavyPatch()}, 5, 2)
	// Fires away from the patch's conservative bounds.
	ray := raytrace.NewRay(mgl64.Vec3{-20, -20, -20}, mgl64.Vec3{-1, -0.3, -0.2})
	c.Check(set.Hit(ray, 0, 1000) == nil, "distant ray misses")
	stats := set.Stats()
	c.Check(stats.Resident == 0 && stats.PageBuilds == 0 && stats.Evictions == 0, "distant ray leaves cache cold, got %+v", stats)
	c.Check(stats.BoundsTests > 0, "pruning still performed conservative tests")
}

func testPruningCounts(t *testing.T, c *taskcheck.C) {
	level := 9 // 512x512 cells, 4096 pages, 1,048,576 triangles total
	set := mustPatchSet(t, []PatchDesc{wavyPatch()}, level, 8)

	p := refEvaluate(wavyPatch().Control, 0.5, 0.5)
	ray := raytrace.NewRay(p.Add(mgl64.Vec3{0, 0, 1}), mgl64.Vec3{0, 0, -1})
	hr := set.Hit(ray, 0, 10)
	c.Check(hr != nil, "high level local ray hits")

	stats := set.Stats()
	totalTriangles := uint64((1 << uint(level)) * (1 << uint(level)) * 2)
	c.Check(stats.TriangleTests < totalTriangles/100, "triangle tests are pruned: %d of %d", stats.TriangleTests, totalTriangles)
	c.Check(stats.PageBuilds <= 8, "only a few pages built, got %d", stats.PageBuilds)
	c.Check(stats.Resident <= 8, "residency bounded, got %d", stats.Resident)
	c.Check(stats.BoundsTests < uint64(1<<uint(level))*8, "hierarchy prunes page groups")

	// Repeated identical query has identical hit and does not rebuild pages.
	before := set.Stats()
	hr2 := set.Hit(ray, 0, 10)
	after := set.Stats()
	c.Check(hr2 != nil && hr != nil && hr2.T() == hr.T(), "repeat query same t")
	c.Check(after.PageBuilds == before.PageBuilds && after.Resident == before.Resident, "repeat query reuses resident pages")
	c.Check(after.TriangleTests > before.TriangleTests, "repeat query still tests triangles on resident pages")
}

func testCapacityOneChurn(t *testing.T, c *taskcheck.C) {
	level := 4 // 16x16 cells, 4 pages
	set := mustPatchSet(t, []PatchDesc{wavyPatch()}, level, 1)
	mesh := refTriangles(wavyPatch().Control, level)

	type probe struct {
		ray  *raytrace.Ray
		want float64
	}
	probes := make([]probe, 0, 64)
	rng := rand.New(rand.NewSource(7))
	for i := 0; i < 24; i++ {
		u := 0.5 + (rng.Float64()-0.5)*0.4
		v := 0.5 + (rng.Float64()-0.5)*0.4
		p := refEvaluate(wavyPatch().Control, u, v)
		ray := raytrace.NewRay(p.Add(mgl64.Vec3{0, 0, 1.5}), mgl64.Vec3{0, 0, -1.5})
		tt, _, _, ok := refHitTriangles(wavyPatch().Control, mesh, ray, 0, 10)
		if ok {
			probes = append(probes, probe{ray, tt})
		}
	}
	c.Check(len(probes) > 0, "capacity churn probes exist")

	results := make([]float64, len(probes))
	for i, pb := range probes {
		hr := set.Hit(pb.ray, 0, 10)
		c.Check(hr != nil && math.Abs(hr.T()-pb.want) < 1e-9, "capacity-1 probe %d hit", i)
		if hr != nil {
			results[i] = hr.T()
		}
	}
	stats := set.Stats()
	c.Check(stats.Resident <= 1, "capacity 1 never exceeds residency, got %d", stats.Resident)
	c.Check(stats.Evictions > 0, "capacity 1 churn evicts, got %d", stats.Evictions)
	c.Check(stats.PeakResident <= 1, "peak residency respects capacity, got %d", stats.PeakResident)

	// Re-run in a different order and confirm identical hits.
	perm := []int{}
	for i := range probes {
		perm = append(perm, len(probes)-1-i)
	}
	for _, i := range perm {
		hr := set.Hit(probes[i].ray, 0, 10)
		c.Check(hr != nil && math.Abs(hr.T()-results[i]) < 1e-12, "capacity-1 repeat probe %d identical", i)
	}
}

func testExpandedCapacity(t *testing.T, c *taskcheck.C) {
	level := 4
	desc := wavyPatch()
	mesh := refTriangles(desc.Control, level)

	smallSet := mustPatchSet(t, []PatchDesc{desc}, level, 1)
	bigSet := mustPatchSet(t, []PatchDesc{desc}, level, 16)

	rays := []*raytrace.Ray{
		raytrace.NewRay(mgl64.Vec3{0.05, 0.05, 1}, mgl64.Vec3{0, 0, -1}),
		raytrace.NewRay(mgl64.Vec3{0.95, 0.05, 1}, mgl64.Vec3{0, 0, -1}),
		raytrace.NewRay(mgl64.Vec3{0.05, 0.95, 1}, mgl64.Vec3{0, 0, -1}),
		raytrace.NewRay(mgl64.Vec3{0.95, 0.95, 1}, mgl64.Vec3{0, 0, -1}),
		raytrace.NewRay(mgl64.Vec3{0.5, 0.5, 1}, mgl64.Vec3{0, 0, -1}),
	}
	for i, ray := range rays {
		hrSmall := smallSet.Hit(ray, 0, 10)
		hrBig := bigSet.Hit(ray, 0, 10)
		wantT, wantUv, wantN, want := refHitTriangles(desc.Control, mesh, ray, 0, 10)
		c.Check((hrSmall != nil) == want && (hrBig != nil) == want, "expanded capacity ray %d hit presence", i)
		if want && hrSmall != nil && hrBig != nil {
			c.Check(math.Abs(hrSmall.T()-hrBig.T()) < 1e-12, "expanded capacity ray %d same t", i)
			c.Check(closeVec2(hrSmall.UvCoords(), hrBig.UvCoords(), 1e-12), "expanded capacity ray %d same uv", i)
			c.Check(closeVec(hrSmall.Normal(), hrBig.Normal(), 1e-12), "expanded capacity ray %d same normal", i)
			c.Check(math.Abs(hrSmall.T()-wantT) < 1e-9 && closeVec2(hrSmall.UvCoords(), wantUv, 1e-9) && closeVec(hrSmall.Normal(), wantN, 1e-9), "expanded capacity ray %d matches reference", i)
		}
	}
	c.Check(bigSet.Stats().PeakResident <= 16, "expanded capacity peak bound")
	c.Check(smallSet.Stats().PeakResident <= 1, "small capacity peak bound")
}

func testConcurrentQueries(t *testing.T, c *taskcheck.C) {
	level := 5
	desc := wavyPatch()
	mesh := refTriangles(desc.Control, level)
	set := mustPatchSet(t, []PatchDesc{desc}, level, 3)

	type job struct {
		u, v float64
		ray  *raytrace.Ray
		want float64
		ok   bool
	}
	rng := rand.New(rand.NewSource(99))
	jobs := make([]job, 0, 64)
	for i := 0; i < 64; i++ {
		u := rng.Float64()
		v := rng.Float64()
		p := refEvaluate(desc.Control, u, v)
		ray := raytrace.NewRay(p.Add(mgl64.Vec3{0, 0, 1.2}), mgl64.Vec3{0, 0, -1.2})
		tt, _, _, ok := refHitTriangles(desc.Control, mesh, ray, 0, 10)
		jobs = append(jobs, job{u, v, ray, tt, ok})
	}

	var wg sync.WaitGroup
	errs := make([]string, 0)
	var errMu sync.Mutex
	for w := 0; w < 8; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			for i := 0; i < 4; i++ {
				for j := range jobs {
					jb := jobs[j]
					hr := set.Hit(jb.ray, 0, 10)
					if (hr != nil) != jb.ok {
						errMu.Lock()
						errs = append(errs, "hit presence mismatch")
						errMu.Unlock()
						continue
					}
					if hr != nil && math.Abs(hr.T()-jb.want) > 1e-9 {
						errMu.Lock()
						errs = append(errs, "hit parameter mismatch")
						errMu.Unlock()
					}
				}
			}
		}(w)
	}

	// Stats concurrently while hits happen.
	stop := make(chan struct{})
	go func() {
		for {
			select {
			case <-stop:
				return
			default:
				st := set.Stats()
				if st.Resident < 0 || st.Resident > 3 {
					errMu.Lock()
					errs = append(errs, "stats residency out of range")
					errMu.Unlock()
				}
			}
		}
	}()

	wg.Wait()
	close(stop)
	for _, e := range errs {
		c.Check(false, "concurrent query: %s", e)
	}
	c.Check(len(errs) == 0, "concurrent queries agree with reference")
	c.Check(set.Stats().Resident <= 3, "concurrent residency bounded")
}

func testConcurrentStats(t *testing.T, c *taskcheck.C) {
	set := mustPatchSet(t, []PatchDesc{wavyPatch(), flatPatch(0.1)}, 5, 4)
	ray := raytrace.NewRay(mgl64.Vec3{0.5, 0.5, 1}, mgl64.Vec3{0, 0, -1})
	set.Hit(ray, 0, 10)
	base := set.Stats()

	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 100; j++ {
				st := set.Stats()
				if st.Resident > 4 || st.PeakResident > 4 {
					c.Check(false, "stats snapshot exceeds capacity")
					return
				}
			}
		}()
	}
	wg.Wait()
	after := set.Stats()
	c.Check(after.PageBuilds == base.PageBuilds, "read-only Stats does not build pages")
	c.Check(after.TriangleTests == base.TriangleTests, "read-only Stats does not change counters")
}

// ---------------------------------------------------------------------------
// TestPatchUpdates
// ---------------------------------------------------------------------------

func TestPatchUpdates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		testReplacePatchValidation(t, c)
		testReplacePatchInvalidates(t, c)
		testReplaceAll(t, c)
		testRefitMovesOutOfOldRoot(t, c)
		testSharedRootUpdate(t, c)
		testMultiLevelRefit(t, c)
		testEmptyToNonEmpty(t, c)
		testRenderIntegration(t, c)
	})
}

func testReplacePatchValidation(t *testing.T, c *taskcheck.C) {
	desc := flatPatch(0)
	set := mustPatchSet(t, []PatchDesc{desc}, 3, 4)
	before := set.Stats()

	c.Check(set.ReplacePatch(-1, desc) != nil, "negative index rejected")
	c.Check(set.ReplacePatch(1, desc) != nil, "out of range index rejected")

	badMat := desc
	badMat.Material = nil
	c.Check(set.ReplacePatch(0, badMat) != nil, "nil material rejected")
	badCtl := desc
	badCtl.Control[3] = mgl64.Vec3{math.Inf(1), 0, 0}
	c.Check(set.ReplacePatch(0, badCtl) != nil, "non-finite control rejected")

	after := set.Stats()
	c.Check(before == after, "failed edits leave stats untouched")

	// Geometry untouched after failed edits.
	ray := raytrace.NewRay(mgl64.Vec3{0.5, 0.5, 1}, mgl64.Vec3{0, 0, -1})
	hr := set.Hit(ray, 0, 10)
	c.Check(hr != nil && math.Abs(hr.T()-1.0) < 1e-12, "failed edits leave geometry untouched")
}

func testReplacePatchInvalidates(t *testing.T, c *taskcheck.C) {
	level := 4
	matA := materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(1, 0, 0)))
	matB := materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(0, 1, 0)))
	descA := flatPatch(0)
	descA.Material = matA
	set := mustPatchSet(t, []PatchDesc{descA}, level, 8)

	ray := raytrace.NewRay(mgl64.Vec3{0.5, 0.5, 1}, mgl64.Vec3{0, 0, -1})
	hr := set.Hit(ray, 0, 10)
	c.Check(hr != nil && hr.Material() == matA, "initial hit uses material A")
	c.Check(set.Stats().Resident > 0, "initial query built pages")

	descB := flatPatch(0.5)
	descB.Material = matB
	c.Check(set.ReplacePatch(0, descB) == nil, "valid replace succeeds")

	stats := set.Stats()
	c.Check(stats.Resident == 0, "replace invalidates all old pages, got %d", stats.Resident)
	hr = set.Hit(ray, 0, 10)
	c.Check(hr != nil, "replacement still hits")
	if hr != nil {
		c.Check(hr.Material() == matB, "replacement material visible")
		c.Check(math.Abs(hr.T()-0.5) < 1e-12, "replacement geometry visible, got t=%v", hr.T())
	}

	// Patch index is preserved with multiple patches.  Patch 1 is moved
	// spatially so each patch occupies its own region.
	shifted := func(desc PatchDesc, dx float64) PatchDesc {
		for i := range desc.Control {
			desc.Control[i] = desc.Control[i].Add(mgl64.Vec3{dx, 0, 0})
		}
		return desc
	}
	set2 := mustPatchSet(t, []PatchDesc{descA, shifted(flatPatch(-0.3), 5)}, level, 8)
	c.Check(set2.ReplacePatch(1, shifted(descB, 5)) == nil, "second patch replace succeeds")
	hrOther := set2.Hit(ray, 0, 10)
	c.Check(hrOther != nil && hrOther.Material() == matA, "patch index 0 untouched")
	hrMoved := set2.Hit(raytrace.NewRay(mgl64.Vec3{5.5, 0.5, 1}, mgl64.Vec3{0, 0, -1}), 0, 10)
	c.Check(hrMoved != nil && hrMoved.Material() == matB && math.Abs(hrMoved.T()-0.5) < 1e-12, "replaced patch 1 keeps its index")

	// Query the replaced patch alone to confirm index 1 geometry.
	rayOnly := raytrace.NewRay(mgl64.Vec3{0.5, 0.5, 1}, mgl64.Vec3{0, 0, -1})
	set3 := mustPatchSet(t, []PatchDesc{flatPatch(2), descA}, level, 8)
	hitFirst := set3.Hit(rayOnly, 0, 10)
	c.Check(hitFirst != nil && hitFirst.Material() == matA, "patch index ordering before replace")
	c.Check(set3.ReplacePatch(0, descB) == nil, "replace index 0")
	hitAfter := set3.Hit(rayOnly, 0, 10)
	c.Check(hitAfter != nil && hitAfter.Material() == matB, "replaced index 0 now nearest")
}

func testReplaceAll(t *testing.T, c *taskcheck.C) {
	level := 4
	descA := flatPatch(0)
	set := mustPatchSet(t, []PatchDesc{descA}, level, 8)
	ray := raytrace.NewRay(mgl64.Vec3{0.5, 0.5, 1}, mgl64.Vec3{0, 0, -1})
	c.Check(set.Hit(ray, 0, 10) != nil, "initial hit")

	// Failed batch leaves everything untouched.
	bad := []PatchDesc{descA, {Control: descA.Control, Material: nil}}
	c.Check(set.ReplaceAll(bad) != nil, "invalid batch rejected")
	hr := set.Hit(ray, 0, 10)
	c.Check(hr != nil && math.Abs(hr.T()-1.0) < 1e-12, "failed batch leaves old geometry")
	c.Check(set.Stats().Resident > 0, "failed batch leaves pages resident")

	// Successful batch commits everything at once.
	matB := materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(0, 1, 0)))
	descB := flatPatch(0.25)
	descB.Material = matB
	descC := flatPatch(-0.25)
	ok := set.ReplaceAll([]PatchDesc{descB, descC})
	c.Check(ok == nil, "valid batch accepted")
	c.Check(set.Stats().Resident == 0, "successful batch invalidates pages")
	hr = set.Hit(ray, 0, 10)
	c.Check(hr != nil && hr.Material() == matB && math.Abs(hr.T()-0.75) < 1e-12, "new patch 0 visible after ReplaceAll")
	// Second patch is farther (z=-0.25 -> t=1.25), so patch 0 wins for a
	// ray from z=1 towards -z only if patch 0 is nearer; patch 0 at z=0.25.
	c.Check(hr != nil && math.Abs(hr.T()-0.75) < 1e-12, "nearest patch wins after ReplaceAll")

	// Whole-set replacement back to non-empty from empty.
	empty := mustPatchSet(t, nil, level, 8)
	c.Check(empty.ReplaceAll([]PatchDesc{descA}) == nil, "empty to non-empty batch")
	hr = empty.Hit(ray, 0, 10)
	c.Check(hr != nil, "newly populated set hits")

	// Whole-set replacement to empty.
	c.Check(set.ReplaceAll(nil) == nil, "batch to empty")
	c.Check(set.Hit(ray, 0, 10) == nil, "empty set after batch never hits")
	box := set.GetBoundingBox(0, 1)
	c.Check(box != nil && box.IsEmpty(), "empty batch yields empty bounds")
}

func testRefitMovesOutOfOldRoot(t *testing.T, c *taskcheck.C) {
	level := 3
	desc := flatPatch(0)
	set := mustPatchSet(t, []PatchDesc{desc}, level, 4)
	sphere := NewSphere(mgl64.Vec3{3, 0, 0}, 0.4, identityMaterial)

	root := NewBvhNode([]raytrace.Hitable{set, sphere}, 0, 1)

	// Ray through the original patch position.
	originalRay := raytrace.NewRay(mgl64.Vec3{0.5, 0.5, 2}, mgl64.Vec3{0, 0, -1})
	c.Check(root.Hit(originalRay, 0, 10) != nil, "original root hits patch")

	// Move the patch far outside the original root box along +x.
	moved := desc
	for j := 0; j < 4; j++ {
		for i := 0; i < 4; i++ {
			moved.Control[j*4+i] = desc.Control[j*4+i].Add(mgl64.Vec3{20, 0, 0})
		}
	}
	c.Check(set.ReplacePatch(0, moved) == nil, "move replacement accepted")

	// Without Refit, the stale root box still contains the old location, so
	// the new location is missed as the root box no longer covers it.
	missBeforeRefit := root.Hit(raytrace.NewRay(mgl64.Vec3{20.5, 0.5, 2}, mgl64.Vec3{0, 0, -1}), 0, 10)
	c.Check(missBeforeRefit == nil, "stale root misses moved patch")

	root.Refit(0, 1)

	movedRay := raytrace.NewRay(mgl64.Vec3{20.5, 0.5, 2}, mgl64.Vec3{0, 0, -1})
	hr := root.Hit(movedRay, 0, 10)
	c.Check(hr != nil && math.Abs(hr.T()-2.0) < 1e-12, "refit root hits moved patch, got %v", hr)
	c.Check(root.Hit(originalRay, 0, 10) == nil, "refit root no longer hits old location")

	// Refit must not change leaf identity.
	c.Check(set.Stats().PageBuilds >= 0, "patch set still usable")
	hr = set.Hit(movedRay, 0, 10)
	c.Check(hr != nil, "shared PatchSet identity preserved after Refit")

	// Sphere leaf still hit.
	sphereRay := raytrace.NewRay(mgl64.Vec3{3, 0, 2}, mgl64.Vec3{0, 0, -1})
	c.Check(root.Hit(sphereRay, 0, 10) != nil, "refit preserves other leaf")
}

func testSharedRootUpdate(t *testing.T, c *taskcheck.C) {
	level := 3
	desc := flatPatch(0)
	set := mustPatchSet(t, []PatchDesc{desc}, level, 4)
	sphere1 := NewSphere(mgl64.Vec3{-3, 0, 0}, 0.3, identityMaterial)
	sphere2 := NewSphere(mgl64.Vec3{3, 0, 0}, 0.3, identityMaterial)

	rootA := NewBvhNode([]raytrace.Hitable{set, sphere1}, 0, 1)
	rootB := NewBvhNode([]raytrace.Hitable{set, sphere2}, 0, 1)

	matNew := materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(1, 0, 0)))
	replacement := flatPatch(0.5)
	replacement.Material = matNew
	c.Check(set.ReplacePatch(0, replacement) == nil, "shared replace accepted")

	rootA.Refit(0, 1)
	rootB.Refit(0, 1)

	probe := raytrace.NewRay(mgl64.Vec3{0.5, 0.5, 1}, mgl64.Vec3{0, 0, -1})
	hrA := rootA.Hit(probe, 0, 10)
	hrB := rootB.Hit(probe, 0, 10)
	c.Check(hrA != nil && hrA.Material() == matNew, "shared root A sees replacement")
	c.Check(hrB != nil && hrB.Material() == matNew, "shared root B sees replacement")
	c.Check(math.Abs(hrA.T()-0.5) < 1e-12 && math.Abs(hrB.T()-0.5) < 1e-12, "shared roots agree on geometry")
	c.Check(rootA.Hit(raytrace.NewRay(mgl64.Vec3{-3, 0, 2}, mgl64.Vec3{0, 0, -1}), 0, 10) != nil, "root A other leaf")
	c.Check(rootB.Hit(raytrace.NewRay(mgl64.Vec3{3, 0, 2}, mgl64.Vec3{0, 0, -1}), 0, 10) != nil, "root B other leaf")
}

func testMultiLevelRefit(t *testing.T, c *taskcheck.C) {
	level := 3
	desc := flatPatch(0)
	set := mustPatchSet(t, []PatchDesc{desc}, level, 4)
	s1 := NewSphere(mgl64.Vec3{-3, 0, 0}, 0.3, identityMaterial)
	s2 := NewSphere(mgl64.Vec3{3, 0, 0}, 0.3, identityMaterial)
	s3 := NewSphere(mgl64.Vec3{0, 3, 0}, 0.3, identityMaterial)

	innerA := NewBvhNode([]raytrace.Hitable{set, s1}, 0, 1)
	innerB := NewBvhNode([]raytrace.Hitable{s2, s3}, 0, 1)
	root := NewBvhNode([]raytrace.Hitable{innerA, innerB}, 0, 1)

	moved := desc
	for i := range moved.Control {
		moved.Control[i] = moved.Control[i].Add(mgl64.Vec3{-15, 0, 0})
	}
	c.Check(set.ReplacePatch(0, moved) == nil, "multi-level replace accepted")
	root.Refit(0, 1)

	movedRay := raytrace.NewRay(mgl64.Vec3{-14.5, 0.5, 1}, mgl64.Vec3{0, 0, -1})
	hr := root.Hit(movedRay, 0, 10)
	c.Check(hr != nil && math.Abs(hr.T()-1.0) < 1e-12, "multi-level refit hits moved patch, got %v", hr)
	c.Check(root.Hit(raytrace.NewRay(mgl64.Vec3{0.5, 0.5, 1}, mgl64.Vec3{0, 0, -1}), 0, 10) == nil, "multi-level refit drops old location")
	c.Check(root.Hit(raytrace.NewRay(mgl64.Vec3{3, 0, 1}, mgl64.Vec3{0, 0, -1}), 0, 10) != nil, "multi-level refit keeps sibling subtree")
	c.Check(root.Hit(raytrace.NewRay(mgl64.Vec3{0, 3, 1}, mgl64.Vec3{0, 0, -1}), 0, 10) != nil, "multi-level refit keeps deep leaf")
}

func testEmptyToNonEmpty(t *testing.T, c *taskcheck.C) {
	level := 3
	set := mustPatchSet(t, nil, level, 2)
	box := set.GetBoundingBox(0, 1)
	c.Check(box != nil && box.IsEmpty(), "empty set empty box")
	ray := raytrace.NewRay(mgl64.Vec3{0.5, 0.5, 1}, mgl64.Vec3{0, 0, -1})
	c.Check(set.Hit(ray, 0, 10) == nil, "empty set no hit")

	sphere := NewSphere(mgl64.Vec3{-3, 0, 0}, 0.3, identityMaterial)
	root := NewBvhNode([]raytrace.Hitable{set, sphere}, 0, 1)
	// The empty PatchSet must not enlarge the BVH through its empty bounds.
	rb := root.GetBoundingBox(0, 1)
	c.Check(closeVec(rb.Min(), sphere.GetBoundingBox(0, 1).Min(), 1e-12), "empty patch does not enlarge root min")

	c.Check(set.ReplaceAll([]PatchDesc{flatPatch(0)}) == nil, "empty set populated")
	root.Refit(0, 1)
	hr := root.Hit(ray, 0, 10)
	c.Check(hr != nil && math.Abs(hr.T()-1.0) < 1e-12, "populated set reachable through refit tree")
	c.Check(root.Hit(raytrace.NewRay(mgl64.Vec3{-3, 0, 1}, mgl64.Vec3{0, 0, -1}), 0, 10) != nil, "sibling still reachable")
}

func testRenderIntegration(t *testing.T, c *taskcheck.C) {
	// The original MonteCarloTracer must consume the PatchSet unchanged,
	// alongside an existing light.
	lightMat := materials.NewDiffuseLight(textures.NewColorTexture(raytrace.NewColorVector(4, 4, 4)))
	light := NewXzRect(-2, 2, -2, 2, 2.0, lightMat)
	set := mustPatchSet(t, []PatchDesc{wavyPatch()}, 4, 6)

	world := NewBvhNode([]raytrace.Hitable{set, light}, 0, 1)
	cam := newTestCamera()
	cfg := raytrace.NewRenderConfig(4, 1, false)
	scene := raytrace.NewScene(cam, world, NewHitableList([]raytrace.Hitable{light}), func(ray *raytrace.Ray) raytrace.ColorVector {
		return raytrace.NewColorVector(0.05, 0.05, 0.08)
	})
	buf := raytrace.NewPixelBuffer(12, 12)
	raytrace.NewRenderer(cfg).Render(raytrace.NewMonteCarloTracer(), buf, scene, cfg)

	nonBackground := 0
	for y := 0; y < 12; y++ {
		for x := 0; x < 12; x++ {
			clr := buf.GetPixelColor(x, y)
			if clr.R() > 0.1 || clr.G() > 0.1 || clr.B() > 0.1 {
				nonBackground++
			}
		}
	}
	c.Check(nonBackground > 0, "renderer produces patch pixels")
	c.Check(set.Stats().Resident > 0, "render populated page cache")
}

func newTestCamera() raytrace.Camera {
	return cameras.NewBasicCamera(
		mgl64.Vec3{0.5, 0.5, 2.2},
		mgl64.Vec3{-0.9, -0.9, 0},
		mgl64.Vec3{1.8, 0, 0},
		mgl64.Vec3{0, 1.8, 0})
}
