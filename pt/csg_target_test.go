package pt

import (
	"fmt"
	"image"
	"math"
	"math/rand"
	"sync"
	"testing"
)

// ---------------------------------------------------------------------------
// Independent reference math (quadratic sphere roots and box slab intervals),
// written separately from the CSG implementation so expected values are not
// just another call into the code under test.
// ---------------------------------------------------------------------------

func refSphereInterval(o, d, c Vector, r float64) (float64, float64, bool) {
	to := o.Sub(c)
	b := to.Dot(d)
	cc := to.Dot(to) - r*r
	disc := b*b - cc
	if disc <= 0 {
		return 0, 0, false
	}
	s := math.Sqrt(disc)
	return -b - s, -b + s, true
}

func refCubeInterval(o, d Vector, min, max Vector) (float64, float64, bool) {
	lo := min.Sub(o).Div(d)
	hi := max.Sub(o).Div(d)
	lo, hi = lo.Min(hi), lo.Max(hi)
	t0 := math.Max(math.Max(lo.X, lo.Y), lo.Z)
	t1 := math.Min(math.Min(hi.X, hi.Y), hi.Z)
	if math.IsNaN(t0) || math.IsNaN(t1) || t0 >= t1 {
		return 0, 0, false
	}
	return t0, t1, true
}

func csgNear(t *testing.T, got, want, tol float64, msg string) {
	t.Helper()
	if math.Abs(got-want) > tol {
		t.Fatalf("%s: got %.12g want %.12g", msg, got, want)
	}
}

func mustCSG(s Shape, err error) Shape {
	if err != nil {
		panic(err)
	}
	return s
}

// ---------------------------------------------------------------------------
// TestCSGIntervals: Boolean occupancy intervals, including rays that start
// inside solids, nested differences, empty results, touching and tangency.
// ---------------------------------------------------------------------------

func TestCSGIntervals(t *testing.T) {
	red := DiffuseMaterial(Color{1, 0, 0})
	blue := DiffuseMaterial(Color{0, 0, 1})
	px := Ray{Origin: V(0, 0, 0), Direction: V(1, 0, 0)}

	// --- union of the two overlapping unit spheres from the seed scenario
	s1 := NewSphere(V(-0.5, 0, 0), 1, red)
	s2 := NewSphere(V(0.5, 0, 0), 1, blue)
	union := mustCSG(NewCSG(CSGUnion, s1, s2))
	// reference roots, computed independently
	a0, a1, okA := refSphereInterval(px.Origin, px.Direction, V(-0.5, 0, 0), 1)
	b0, b1, okB := refSphereInterval(px.Origin, px.Direction, V(0.5, 0, 0), 1)
	if !okA || !okB {
		t.Fatal("reference sphere intervals missing")
	}
	csgNear(t, a0, -1.5, 1e-12, "ref sphere1 entry")
	csgNear(t, a1, 0.5, 1e-12, "ref sphere1 exit")
	csgNear(t, b0, -0.5, 1e-12, "ref sphere2 entry")
	csgNear(t, b1, 1.5, 1e-12, "ref sphere2 exit")
	h := union.Intersect(px)
	if !h.Ok() {
		t.Fatal("union ray from inside produced no boundary")
	}
	csgNear(t, h.T, 1.5, 1e-9, "union must hide the t=0.5 internal wall")
	if !h.Info(px).Inside {
		t.Fatal("ray starts inside union: Inside must be true")
	}
	// same union, ray from outside: entry is the outer sphere surface
	out := Ray{Origin: V(-3, 0, 0), Direction: V(1, 0, 0)}
	h = union.Intersect(out)
	csgNear(t, h.T, 1.5, 1e-9, "union entry from outside")
	if h.Info(out).Inside {
		t.Fatal("outside hit must report Inside=false")
	}

	// --- intersection: overlap region along X is [-0.5, 0.5]
	inter := mustCSG(NewCSG(CSGIntersection, s1, s2))
	h = inter.Intersect(px)
	if !h.Ok() {
		t.Fatal("intersection missed")
	}
	csgNear(t, h.T, 0.5, 1e-9, "intersection exit at x=0.5")
	if !h.Info(px).Inside {
		t.Fatal("origin is inside both spheres: Inside must be true")
	}
	outsideRay := Ray{Origin: V(-3, 0, 0), Direction: V(1, 0, 0)}
	h = inter.Intersect(outsideRay)
	csgNear(t, h.T, 2.5, 1e-9, "intersection entry at x=-0.5")

	// --- difference: concentric spherical shell, full event sequence
	outer := NewSphere(V(0, 0, 0), 2, red)
	inner := NewSphere(V(0, 0, 0), 1, blue)
	shell := mustCSG(NewCSG(CSGDifference, outer, inner))
	shellRay := Ray{Origin: V(3, 0, 0), Direction: V(-1, 0, 0)}
	events := csgIntervals(shell, Identity(), shellRay)
	if len(events) != 4 {
		t.Fatalf("shell wants 4 boundaries, got %d", len(events))
	}
	wantT := []float64{1, 2, 4, 5}
	// solid shell intervals [1,2] and [4,5] with the cavity between them
	wantEnter := []bool{true, false, true, false}
	for i, e := range events {
		csgNear(t, e.t, wantT[i], 1e-9, fmt.Sprintf("shell boundary %d", i))
		if e.entering != wantEnter[i] {
			t.Fatalf("shell boundary %d entering=%t want %t", i, e.entering, wantEnter[i])
		}
	}
	// first hit is the outer wall with the outer material
	h = shell.Intersect(shellRay)
	csgNear(t, h.T, 1, 1e-9, "shell entry")
	info := h.Info(shellRay)
	if info.Inside || info.Material.Color != red.Color {
		t.Fatal("shell entry must be exterior, outer material")
	}
	csgNear(t, info.Normal.X, 1, 1e-9, "shell entry shading normal +X")
	// cavity wall is owned by the right operand with its material, normal
	// flipped toward the cavity
	cavity := events[1]
	if cavity.leaf != inner {
		t.Fatal("cavity wall must come from the subtracted operand")
	}
	csgNear(t, cavity.outward.X, -1, 1e-9, "cavity outward normal faces the hollow center")
	cavityHit := shell.Intersect(Ray{Origin: V(0, 0, 0), Direction: V(1, 0, 0)})
	csgNear(t, cavityHit.T, 1, 1e-9, "cavity wall from the hollow center")
	cavityInfo := cavityHit.Info(Ray{Origin: V(0, 0, 0), Direction: V(1, 0, 0)})
	if cavityInfo.Material.Color != blue.Color {
		t.Fatal("cavity wall must use the right operand material")
	}
	csgNear(t, cavityInfo.Normal.X, -1, 1e-9, "shading normal faces the incident ray in the cavity")
	if cavityInfo.Inside {
		t.Fatal("ray starts in the cavity: Inside must be false")
	}

	// --- nested difference: (big cube - sphere) - small cube. Along the
	// ray from x=-6 the big cube spans [3,9] and the sphere [4,8], so the
	// first difference leaves two caps; the small cube bites into the far
	// cap where the first difference is solid again.
	cubeA := NewCube(V(-3, -3, -3), V(3, 3, 3), red)
	sphB := NewSphere(V(0, 0, 0), 2, blue)
	cubeC := NewCube(V(2.2, -0.5, -0.5), V(2.7, 0.5, 0.5), DiffuseMaterial(White))
	diff1 := mustCSG(NewCSG(CSGDifference, cubeA, sphB))
	nested := mustCSG(NewCSG(CSGDifference, diff1, cubeC))
	nestedRay := Ray{Origin: V(-6, 0, 0), Direction: V(1, 0, 0)}
	wantNested := []float64{3, 4, 8, 8.2, 8.7, 9}
	events = csgIntervals(nested.(*CSGShape), Identity(), nestedRay)
	if len(events) != len(wantNested) {
		t.Fatalf("nested diff wants %d boundaries, got %d", len(wantNested), len(events))
	}
	for i, e := range events {
		csgNear(t, e.t, wantNested[i], 1e-9, fmt.Sprintf("nested boundary %d", i))
	}
	// cross-check the two primitive reference intervals used above
	if t0, t1, ok := refSphereInterval(nestedRay.Origin, nestedRay.Direction, V(0, 0, 0), 2); !ok ||
		math.Abs(t0-4) > 1e-9 || math.Abs(t1-8) > 1e-9 {
		t.Fatalf("reference sphere interval wrong: %v %v %v", t0, t1, ok)
	}
	if t0, t1, ok := refCubeInterval(nestedRay.Origin, nestedRay.Direction, V(2.2, -0.5, -0.5), V(2.7, 0.5, 0.5)); !ok ||
		math.Abs(t0-8.2) > 1e-9 || math.Abs(t1-8.7) > 1e-9 {
		t.Fatalf("reference cube interval wrong: %v %v %v", t0, t1, ok)
	}

	// --- empty result: identical object subtracted from itself
	empty := mustCSG(NewCSG(CSGDifference, outer, NewSphere(V(0, 0, 0), 2, blue)))
	for _, o := range []Vector{V(0, 0, 0), V(5, 0, 0), V(0, 3, 0), V(-2, -2, -2)} {
		h := empty.Intersect(Ray{o, V(1, 0, 0)})
		if h.Ok() {
			t.Fatalf("empty difference produced a hit from %v", o)
		}
	}
	// disjoint intersection is also empty
	disjoint := mustCSG(NewCSG(CSGIntersection,
		NewSphere(V(-5, 0, 0), 1, red), NewSphere(V(5, 0, 0), 1, blue)))
	h = disjoint.Intersect(Ray{V(0, 0, 0), V(1, 0, 0)})
	if h.Ok() {
		t.Fatal("disjoint intersection must be empty")
	}
	if b := disjoint.BoundingBox(); math.IsInf(b.Min.X, 0) || math.IsInf(b.Max.X, 0) ||
		math.IsNaN(b.Min.X) || math.IsNaN(b.Max.X) {
		t.Fatal("bounding box must stay finite")
	}

	// --- touching boxes: union leaves no seam at the shared face x=1
	boxL := NewCube(V(0, 0, 0), V(1, 1, 1), red)
	boxR := NewCube(V(1, 0, 0), V(2, 1, 1), blue)
	touch := mustCSG(NewCSG(CSGUnion, boxL, boxR))
	touchRay := Ray{Origin: V(-1, 0.5, 0.5), Direction: V(1, 0, 0)}
	events = csgIntervals(touch, Identity(), touchRay)
	if len(events) != 2 {
		t.Fatalf("touching box union wants 2 boundaries, got %d", len(events))
	}
	csgNear(t, events[0].t, 1, 1e-9, "touch union entry")
	csgNear(t, events[1].t, 3, 1e-9, "touch union exit")
	// reference slab intervals confirm both boxes genuinely meet at x=1
	if t0, t1, ok := refCubeInterval(touchRay.Origin, touchRay.Direction, V(0, 0, 0), V(1, 1, 1)); !ok ||
		math.Abs(t0-1) > 1e-9 || math.Abs(t1-2) > 1e-9 {
		t.Fatalf("left box reference interval wrong: %v %v %v", t0, t1, ok)
	}

	// --- tangency creates no boundary
	tanRay := Ray{Origin: V(-3, 2, 0), Direction: V(1, 0, 0)}
	if _, _, ok := refSphereInterval(tanRay.Origin, tanRay.Direction, V(0, 0, 0), 2); ok {
		t.Fatal("reference tangent should be a single repeated root, no interval")
	}
	if h := outer.Intersect(tanRay); h.Ok() {
		t.Fatal("tangent sphere ray must not hit")
	}
	if h := mustCSG(NewCSG(CSGUnion, outer, inner)).Intersect(tanRay); h.Ok() {
		t.Fatal("tangent ray must not produce a CSG boundary")
	}
	edgeRay := Ray{Origin: V(-3, 1, 1), Direction: V(1, 0, 0)}
	if _, _, ok := refCubeInterval(edgeRay.Origin, edgeRay.Direction, V(-1, -1, -1), V(1, 1, 1)); ok {
		t.Fatal("reference edge-grazing ray must not cross the cube interior")
	}

	// --- thin 1e-5 shell survives, distances within 1e-7
	thinOuter := NewSphere(V(0, 0, 0), 1, red)
	thinInner := NewSphere(V(0, 0, 0), 1-1e-5, blue)
	thin := mustCSG(NewCSG(CSGDifference, thinOuter, thinInner))
	thinRay := Ray{Origin: V(0, 0, 0), Direction: V(1, 0, 0)}
	events = csgIntervals(thin.(*CSGShape), Identity(), thinRay)
	forward := 0
	var e0, e1 csgEvent
	for _, e := range events {
		if e.t > EPS {
			if forward == 0 {
				e0 = e
			}
			e1 = e
			forward++
		}
	}
	if forward != 2 {
		t.Fatalf("thin shell wants 2 forward boundaries, got %d", forward)
	}
	csgNear(t, e0.t, 1-1e-5, 1e-7, "thin shell inner wall")
	csgNear(t, e1.t, 1, 1e-7, "thin shell outer wall")
	if e1.t-e0.t < 5e-6 {
		t.Fatal("thin shell thickness was collapsed")
	}

	// --- same primitive may be referenced by both operands
	self := mustCSG(NewCSG(CSGUnion, outer, outer))
	h = self.Intersect(Ray{Origin: V(0, 0, 0), Direction: V(1, 0, 0)})
	csgNear(t, h.T, 2, 1e-9, "self-union behaves like the primitive")

	// --- construction-time validation
	if _, err := NewCSG(CSGUnion, nil, outer); err == nil {
		t.Fatal("nil operand must error")
	}
	if _, err := NewCSG(CSGOp(99), outer, inner); err == nil {
		t.Fatal("unknown op must error")
	}
	if _, err := NewCSG(CSGUnion, NewPlane(V(0, 0, 0), V(0, 0, 1), red), outer); err == nil {
		t.Fatal("unsupported shape must error")
	}
	if _, err := NewCSG(CSGUnion, mustCSG(NewCSG(CSGUnion, outer, inner)), outer); err != nil {
		t.Fatalf("nested CSG operand rejected: %v", err)
	}
}

// ---------------------------------------------------------------------------
// TestCSGSurfaceTransport: materials, local UV sampling, normals, Identity
// wrappers, non-uniform / mirror / shear transforms, deterministic picks.
// ---------------------------------------------------------------------------

type solidTexture struct{ c Color }

func (t *solidTexture) Sample(u, v float64) Color        { return t.c }
func (t *solidTexture) NormalSample(u, v float64) Vector { return V(0, 0, 1) }
func (t *solidTexture) BumpSample(u, v float64) Vector   { return Vector{} }
func (t *solidTexture) Pow(a float64) Texture            { return &solidTexture{t.c.Pow(a)} }
func (t *solidTexture) MulScalar(a float64) Texture      { return &solidTexture{t.c.MulScalar(a)} }

type uvTexture struct{}

func (t *uvTexture) Sample(u, v float64) Color {
	if u < 0.5 {
		return Color{1, 0, 0}
	}
	return Color{0, 0, 1}
}
func (t *uvTexture) NormalSample(u, v float64) Vector { return V(0, 0, 1) }
func (t *uvTexture) BumpSample(u, v float64) Vector   { return Vector{} }
func (t *uvTexture) Pow(a float64) Texture            { return t }
func (t *uvTexture) MulScalar(a float64) Texture      { return t }

func textured(c Color) Material {
	m := DiffuseMaterial(c)
	m.Texture = &solidTexture{c}
	return m
}

func TestCSGSurfaceTransport(t *testing.T) {
	// difference of two textured cubes: cavity wall keeps the right
	// operand's locally sampled material
	matA := textured(Color{0.9, 0.1, 0.1})
	matB := textured(Color{0.1, 0.1, 0.9})
	cubeA := NewCube(V(-3, -3, -3), V(3, 3, 3), matA)
	cubeB := NewCube(V(-1, -1, -1), V(1, 1, 1), matB)
	diff := mustCSG(NewCSG(CSGDifference, cubeA, cubeB))
	r := Ray{Origin: V(0, 0, 0), Direction: V(1, 0, 0)}
	h := diff.Intersect(r)
	csgNear(t, h.T, 1, 1e-9, "cavity wall at x=1")
	info := h.Info(r)
	if info.Material.Color != matB.Color {
		t.Fatal("inner wall must sample the right operand material")
	}
	if info.Shape != cubeB {
		t.Fatal("HitInfo.Shape must identify the producing primitive")
	}
	csgNear(t, info.Normal.X, -1, 1e-9, "shading normal faces the cavity and the incident ray")
	if info.Inside {
		t.Fatal("ray starts in the cavity: Inside=false")
	}
	// outer wall further along keeps the left material
	r2 := Ray{Origin: V(1.5, 0, 0), Direction: V(1, 0, 0)}
	h2 := diff.Intersect(r2)
	csgNear(t, h2.T, 1.5, 1e-9, "outer wall at x=3")
	info2 := h2.Info(r2)
	if info2.Material.Color != matA.Color || !info2.Inside {
		t.Fatal("outer wall must use left material and report Inside=true")
	}

	// local UV sampling through a transformed operand: unit cube with a
	// UV-split texture, scaled and translated into the world
	uvMat := DiffuseMaterial(White)
	uvMat.Texture = &uvTexture{}
	localCube := NewCube(V(0, 0, 0), V(1, 1, 1), uvMat)
	moved := NewTransformedShape(localCube, Translate(V(0, 0, 4)))
	flatUnion := mustCSG(NewCSG(CSGUnion, moved, NewSphere(V(0, 5, 0), 0.25, textured(White))))
	// hit the local x=0 face (u=0 -> red)
	redRay := Ray{Origin: V(-2, 0.5, 4.5), Direction: V(1, 0, 0)}
	h = flatUnion.Intersect(redRay)
	if !h.Ok() || h.Info(redRay).Material.Color.R < 0.9 {
		t.Fatalf("local u=0 face must sample red, got %+v", h)
	}
	// hit the local x=1 face from the other side (u=1 -> blue)
	blueRay := Ray{Origin: V(2, 0.5, 4.5), Direction: V(-1, 0, 0)}
	h = flatUnion.Intersect(blueRay)
	if !h.Ok() || h.Info(blueRay).Material.Color.B < 0.9 {
		t.Fatalf("local u=1 face must sample blue, got %+v", h)
	}

	// non-uniform scale + shear + mirror around the operands
	shear := Matrix{
		1, 0.3, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1}
	mirrored := NewTransformedShape(NewSphere(V(0, 0, 0), 1, matA),
		Scale(V(-2, 1, 0.5)).Mul(shear))
	csgShear := mustCSG(NewCSG(CSGDifference, mirrored, NewSphere(V(3, 0, 0), 0.25, matB)))
	csgShear.Compile()
	sr := Ray{Origin: V(-4, 0.7, 0), Direction: V(1, 0, 0)}
	h = csgShear.Intersect(sr)
	if !h.Ok() {
		t.Fatal("sheared/mirrored CSG missed")
	}
	// independent expectation: transform the ray into the primitive frame
	// and solve the sphere quadratic there, then project back to world t
	local := mirrored.(*TransformedShape).Inverse.MulRay(sr)
	t0, _, ok := refSphereInterval(local.Origin, local.Direction, V(0, 0, 0), 1)
	if !ok {
		t.Fatal("reference sheared sphere interval missing")
	}
	worldHit := mirrored.(*TransformedShape).Matrix.MulPosition(local.Position(t0))
	wantWorldT := worldHit.Sub(sr.Origin).Dot(sr.Direction)
	csgNear(t, h.T, wantWorldT, 1e-7, "world distance under shear/mirror")
	hi := h.Info(sr)
	csgNear(t, hi.Position.X, worldHit.X, 1e-7, "world position under shear/mirror")
	if math.Abs(hi.Normal.Length()-1) > 1e-9 {
		t.Fatal("normal must be unit length")
	}
	if hi.Normal.Dot(sr.Direction) > 0 {
		t.Fatal("shading normal must oppose the incident ray")
	}

	// an Identity wrapper around a CSG operand preserves the child info
	wrapped := NewTransformedShape(diff, Identity())
	hw := wrapped.Intersect(r)
	hbare := diff.Intersect(r)
	iw, ib := hw.Info(r), hbare.Info(r)
	csgNear(t, hw.T, hbare.T, 1e-12, "identity wrapper changes t")
	if iw.Material.Color != ib.Material.Color || iw.Inside != ib.Inside {
		t.Fatal("identity wrapper lost hit information")
	}
	csgNear(t, iw.Normal.X, ib.Normal.X, 1e-12, "identity wrapper changed normal")

	// a transform wrapped around the whole CSG keeps world-space answers
	outerWrap := NewTransformedShape(diff, Rotate(V(0, 1, 0), math.Pi/2))
	wr := Ray{Origin: V(0, 0, 0), Direction: V(-1, 0, 0)}
	hw = outerWrap.Intersect(wr)
	// rotating the x-oriented cavity by 90 degrees about Y maps +X to -X
	csgNear(t, hw.T, 1, 1e-9, "wrapped CSG world distance")
	if hw.Info(wr).Material.Color != matB.Color {
		t.Fatal("wrapped CSG lost the cavity material")
	}

	// coincident-boundary choice is stable under repeated, concurrent use
	touchLocal, err := NewCSG(CSGUnion,
		NewCube(V(0, 0, 0), V(1, 1, 1), matA),
		NewCube(V(1, 0, 0), V(2, 1, 1), matB))
	if err != nil {
		t.Fatal(err)
	}
	scene := &Scene{}
	scene.Add(touchLocal)
	scene.Compile()
	probe := Ray{Origin: V(-1, 0.5, 0.5), Direction: V(1, 0, 0)}
	ref := scene.Intersect(probe)
	var wg sync.WaitGroup
	for g := 0; g < 32; g++ {
		wg.Add(1)
		go func(seed int64) {
			defer wg.Done()
			rnd := rand.New(rand.NewSource(seed))
			for k := 0; k < 200; k++ {
				rr := probe
				rr.Origin = probe.Origin.Add(V(0, (rnd.Float64()-0.5)*0.4, (rnd.Float64()-0.5)*0.4))
				got := scene.Intersect(rr)
				want := touchLocal.Intersect(rr)
				if got.Ok() != want.Ok() || (got.Ok() && math.Abs(got.T-want.T) > 1e-12) {
					t.Errorf("nondeterministic concurrent hit")
					return
				}
			}
		}(int64(g + 1))
	}
	wg.Wait()
	if !ref.Ok() {
		t.Fatal("deterministic probe missed")
	}
}

// ---------------------------------------------------------------------------
// TestCSGRender: the DefaultSampler sees one solid for direct hits, specular
// bounces and light visibility, with concurrent interleaved queries.
// ---------------------------------------------------------------------------

func TestCSGRender(t *testing.T) {
	red := DiffuseMaterial(Color{0.8, 0.1, 0.1})
	blue := DiffuseMaterial(Color{0.1, 0.2, 0.8})
	metal := MetallicMaterial(Color{0.7, 0.7, 0.7}, 0, 0)

	// perforated sphere: a second sphere subtracts a cavity open on +X
	big := NewSphere(V(0, 0, 0), 1.5, red)
	hole := NewSphere(V(1.2, 0, 0), 0.9, blue)
	body, err := NewCSG(CSGDifference, big, hole)
	if err != nil {
		t.Fatal(err)
	}
	metallicBody, err := NewCSG(CSGDifference,
		NewSphere(V(0, 0, 0), 1.5, metal), hole)
	if err != nil {
		t.Fatal(err)
	}

	scene := &Scene{Color: Color{0.05, 0.05, 0.08}}
	scene.Add(NewPlane(V(0, -2, 0), V(0, 1, 0), DiffuseMaterial(White)))
	scene.Add(body)
	// the opening-facing light sits above the X axis so probe rays do not
	// run straight into the light sphere; a second light is buried in the
	// solid shell and must stay occluded
	frontPos, backPos := V(3.5, 2.5, 0), V(-3, 0, 0)
	lightFront := NewSphere(frontPos, 0.2, LightMaterial(White, 5))
	lightBack := NewSphere(backPos, 0.1, LightMaterial(White, 5))
	scene.Add(lightFront)
	scene.Add(lightBack)
	empty, err := NewCSG(CSGDifference, big, NewSphere(V(0, 0, 0), 1.5, red))
	if err != nil {
		t.Fatal(err)
	}
	scene.Add(empty)
	scene.Compile()

	// direct hit through the opening lands on the cavity wall, whose
	// material belongs to the subtracted sphere
	through := Ray{Origin: V(4, 0, 0), Direction: V(-1, 0, 0)}
	h := body.Intersect(through)
	if !h.Ok() {
		t.Fatal("CSG body missed through its opening")
	}
	csgNear(t, h.T, 3.7, 1e-7, "cavity wall through the opening")
	info := h.Info(through)
	if info.Shape != hole {
		t.Fatal("cavity hit must resolve to the subtracted primitive")
	}
	if info.Material.Color != blue.Color {
		t.Fatal("cavity wall material lost through the scene path")
	}
	if info.Inside {
		t.Fatal("approach through the opening starts outside the solid")
	}
	// the same solid must be reached through the scene k-d tree: offset
	// the ray slightly so no light sphere lies on it
	throughScene := Ray{Origin: V(4, 0.25, 0), Direction: V(-1, 0, 0)}
	sh := scene.Intersect(throughScene)
	if !sh.Ok() {
		t.Fatal("scene k-d tree missed the CSG body")
	}
	if sh.Info(throughScene).Shape == nil {
		t.Fatal("scene hit carried no hit info")
	}

	// light visibility through the Boolean opening: the shadow ray reaches
	// the front light instead of colliding with the deleted outer wall.
	shadowFront := Ray{Origin: info.Position.Add(V(1e-4, 0, 0)),
		Direction: frontPos.Sub(info.Position).Normalize()}
	sh = scene.Intersect(shadowFront)
	if !sh.Ok() || sh.Shape != lightFront {
		t.Fatal("light seen through the Boolean opening was occluded by a fake wall")
	}
	// the real shell still occludes the light buried inside it
	shadowBack := Ray{Origin: info.Position,
		Direction: backPos.Sub(info.Position).Normalize()}
	sh = scene.Intersect(shadowBack)
	if sh.Ok() && sh.Shape == lightBack {
		t.Fatal("light buried in the solid shell must be occluded")
	}

	// control scene: uncut sphere keeps its full surface at x=1.5
	control := &Scene{Color: scene.Color}
	control.Add(big)
	control.Compile()
	ch := control.Intersect(through)
	csgNear(t, ch.T, 2.5, 1e-9, "uncut sphere surface at x=1.5")
	if ch.Info(through).Material.Color != red.Color {
		t.Fatal("control sphere material changed")
	}

	// ray missing every solid still reaches the environment
	miss := scene.Intersect(Ray{Origin: V(4, 1.7, 0), Direction: V(-1, 0, 0)})
	if miss.Ok() {
		t.Fatal("ray above the perforated sphere must not hit it")
	}

	// full DefaultSampler paths: diffuse, specular and environment all run
	// against the same Boolean solid
	samplerFull := NewSampler(1, 3)
	metalScene := &Scene{Color: scene.Color}
	metalScene.Add(metallicBody)
	metalScene.Add(lightFront)
	metalScene.Compile()
	var wg sync.WaitGroup
	errCh := make(chan error, 16)
	for g := 0; g < 16; g++ {
		wg.Add(1)
		go func(seed int64) {
			defer wg.Done()
			r := rand.New(rand.NewSource(seed))
			for k := 0; k < 64; k++ {
				ray := Ray{
					Origin:    V(4, (r.Float64()-0.5)*2.5, (r.Float64()-0.5)*2.5),
					Direction: V(-1, 0, 0),
				}
				c1 := samplerFull.Sample(scene, ray, r)
				c2 := samplerFull.Sample(metalScene, ray, r)
				for _, c := range [2]Color{c1, c2} {
					if math.IsNaN(c.R) || math.IsNaN(c.G) || math.IsNaN(c.B) {
						select {
						case errCh <- fmt.Errorf("NaN sample"):
						default:
						}
						return
					}
				}
			}
		}(int64(g + 100))
	}
	wg.Wait()
	select {
	case e := <-errCh:
		t.Fatal(e)
	default:
	}

	// a deterministic ray through the hole samples differently from the
	// uncut control: no internal red wall, cavity shading instead
	rnd1, rnd2 := rand.New(rand.NewSource(7)), rand.New(rand.NewSource(7))
	cs := samplerFull.Sample(scene, through, rnd1)
	cc := samplerFull.Sample(control, through, rnd2)
	if cs == cc {
		t.Fatal("perforated and uncut solids rendered identically")
	}

	// ordinary, non-CSG transformed geometry keeps its original behavior
	plain := NewTransformedShape(NewSphere(V(0, 0, 0), 1, red), Scale(V(2, 1, 0.5)).Translate(V(0, 0, 5)))
	plainRay := Ray{Origin: V(0, 0, 0), Direction: V(0, 0, 1)}
	ph := plain.Intersect(plainRay)
	csgNear(t, ph.T, 4.5, 1e-9, "plain transformed ellipsoid unchanged")
	phi := ph.Info(plainRay)
	csgNear(t, phi.Normal.Z, -1, 1e-9, "plain ellipsoid normal unchanged")

	// image.Image is still produced by the buffer used by the renderer
	if img := NewBuffer(2, 2).Image(ColorChannel); img.Bounds() != image.Rect(0, 0, 2, 2) {
		t.Fatal("buffer image bounds changed")
	}
}
