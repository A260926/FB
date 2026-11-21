package render

import (
	"math"
	"math/rand"
	"sync"
	"testing"

	"github.com/markphelps/go-trace/primitive"
	"github.com/markphelps/go-trace/task_support/taskcheck"
)

// ---------- independent reference math (test-only, no production code) ----------

func tAdd(a, b [3]float64) [3]float64 { return [3]float64{a[0] + b[0], a[1] + b[1], a[2] + b[2]} }
func tSub(a, b [3]float64) [3]float64 { return [3]float64{a[0] - b[0], a[1] - b[1], a[2] - b[2]} }
func tScale(a [3]float64, s float64) [3]float64 {
	return [3]float64{a[0] * s, a[1] * s, a[2] * s}
}
func tDot(a, b [3]float64) float64  { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] }
func tLen(a [3]float64) float64     { return math.Sqrt(tDot(a, a)) }
func tNorm(a [3]float64) [3]float64 { return tScale(a, 1/tLen(a)) }
func tCross(a, b [3]float64) [3]float64 {
	return [3]float64{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}
}
func tMul(m [3][3]float64, v [3]float64) [3]float64 {
	return [3]float64{
		m[0][0]*v[0] + m[0][1]*v[1] + m[0][2]*v[2],
		m[1][0]*v[0] + m[1][1]*v[1] + m[1][2]*v[2],
		m[2][0]*v[0] + m[2][1]*v[1] + m[2][2]*v[2],
	}
}
func tMulMM(a, b [3][3]float64) [3][3]float64 {
	var r [3][3]float64
	for i := 0; i < 3; i++ {
		for j := 0; j < 3; j++ {
			r[i][j] = a[i][0]*b[0][j] + a[i][1]*b[1][j] + a[i][2]*b[2][j]
		}
	}
	return r
}
func tT(m [3][3]float64) [3][3]float64 {
	return [3][3]float64{
		{m[0][0], m[1][0], m[2][0]},
		{m[0][1], m[1][1], m[2][1]},
		{m[0][2], m[1][2], m[2][2]},
	}
}
func tInv3(m [3][3]float64) ([3][3]float64, float64) {
	a, b, c := m[0][0], m[0][1], m[0][2]
	d, e, f := m[1][0], m[1][1], m[1][2]
	g, h, i := m[2][0], m[2][1], m[2][2]
	c00 := e*i - f*h
	c01 := -(d*i - f*g)
	c02 := d*h - e*g
	det := a*c00 + b*c01 + c*c02
	s := 1 / det
	return [3][3]float64{
		{c00 * s, -(b*i - c*h) * s, (b*f - c*e) * s},
		{c01 * s, (a*i - c*g) * s, -(a*f - c*d) * s},
		{c02 * s, -(a*h - b*g) * s, (a*e - b*d) * s},
	}, det
}
func tAffine(m [3][3]float64, tx, ty, tz float64) [12]float64 {
	return [12]float64{
		m[0][0], m[0][1], m[0][2], tx,
		m[1][0], m[1][1], m[1][2], ty,
		m[2][0], m[2][1], m[2][2], tz,
	}
}
func tUniform(s, tx, ty, tz float64) [12]float64 {
	return tAffine([3][3]float64{{s, 0, 0}, {0, s, 0}, {0, 0, s}}, tx, ty, tz)
}

// tEllipsoid models one instance as the world-space quadric
// (p-m)^T Q (p-m) = r^2, derived independently of the production code.
type tEllipsoid struct {
	c     [3]float64 // local prototype center
	m     [3]float64 // world center A*c + t
	r     float64
	a, ai [3][3]float64
	ait   [3][3]float64
	q     [3][3]float64 // ai^T * ai
	det   float64
}

func tMakeEllipsoid(c [3]float64, r float64, m [12]float64) tEllipsoid {
	a := [3][3]float64{{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}}
	ai, det := tInv3(a)
	tr := [3]float64{m[3], m[7], m[11]}
	return tEllipsoid{
		c:   c,
		m:   tAdd(tMul(a, c), tr),
		r:   r,
		a:   a,
		ai:  ai,
		ait: tT(ai),
		q:   tMulMM(tT(ai), ai),
		det: det,
	}
}

// intersect solves the world-space ellipsoid quadratic for ray o+t*d.
func (e tEllipsoid) intersect(o, d [3]float64, tMin, tMax float64) (float64, bool) {
	om := tSub(o, e.m)
	a := tDot(d, tMul(e.q, d))
	b := tDot(om, tMul(e.q, d))
	cc := tDot(om, tMul(e.q, om)) - e.r*e.r
	disc := b*b - a*cc
	if disc <= 0 {
		return 0, false
	}
	sq := math.Sqrt(disc)
	for _, tt := range [2]float64{(-b - sq) / a, (-b + sq) / a} {
		if tt > tMin && tt < tMax {
			return tt, true
		}
	}
	return 0, false
}

// normal is the normalized gradient of the ellipsoid implicit function.
func (e tEllipsoid) normal(p [3]float64) [3]float64 {
	return tNorm(tMul(e.q, tSub(p, e.m)))
}

// worldPoint maps the unit-sphere direction n to the world surface point.
func (e tEllipsoid) worldPoint(n [3]float64) [3]float64 {
	return tAdd(tMul(e.a, tScale(n, e.r)), e.m)
}

// localOf maps a world point back to its unit-sphere direction.
func (e tEllipsoid) localOf(p [3]float64) [3]float64 {
	return tNorm(tMul(e.ai, tSub(p, e.m)))
}

// areaFactor is r^2 * |det A| * |A^-T n|, the world area per unit of
// unit-sphere solid angle.
func (e tEllipsoid) areaFactor(n [3]float64) float64 {
	return e.r * e.r * math.Abs(e.det) * tLen(tMul(e.ait, n))
}

// tScene is the test's own scene model used for independent first-hit and
// density computations.
type tScene struct {
	ids []uint64
	ell map[uint64]tEllipsoid
}

func tBuildScene(protos []SphereProto, insts []Instance) tScene {
	s := tScene{ell: map[uint64]tEllipsoid{}}
	for _, inst := range insts {
		p := protos[inst.Proto]
		c := [3]float64{p.Center.X, p.Center.Y, p.Center.Z}
		s.ids = append(s.ids, inst.ID)
		s.ell[inst.ID] = tMakeEllipsoid(c, p.Radius, inst.Matrix)
	}
	return s
}

func (s tScene) firstHit(o, d [3]float64, tMin float64) (uint64, float64, bool) {
	best := math.MaxFloat64
	var id uint64
	ok := false
	for _, i := range s.ids {
		if tt, hit := s.ell[i].intersect(o, d, tMin, best); hit {
			best = tt
			id = i
			ok = true
		}
	}
	return id, best, ok
}

// refDensity is the test's independent implementation of the transported
// source density: cosine/area in the source scene times the old-to-new area
// ratio, zero when the source's first hit is not the mapped point.
func refDensity(cur, src tScene, srcRef SurfaceRef, yRef SurfaceRef) float64 {
	srcEll, ok := src.ell[yRef.Instance]
	if !ok {
		return 0
	}
	curEll, ok := cur.ell[yRef.Instance]
	if !ok {
		return 0
	}
	srcPointEll := src.ell[srcRef.Instance]
	nS := v3(srcRef.LocalNormal)
	nY := v3(yRef.LocalNormal)
	xS := srcPointEll.worldPoint(nS)
	yS := srcEll.worldPoint(nY)
	d := tSub(yS, xS)
	dist := tLen(d)
	if dist <= tMin {
		return 0
	}
	w := tScale(d, 1/dist)
	cosS := tDot(srcPointEll.normal(xS), w)
	if cosS <= 0 {
		return 0
	}
	id, tt, ok := src.firstHit(xS, w, tMin)
	if !ok || id != yRef.Instance {
		return 0
	}
	hitP := tAdd(xS, tScale(w, tt))
	if tLen(tSub(hitP, yS)) > 1e-9*math.Max(1, dist) {
		return 0
	}
	cosY := math.Abs(tDot(srcEll.normal(yS), w))
	pArea := cosS / math.Pi * cosY / (dist * dist)
	return pArea * srcEll.areaFactor(nY) / curEll.areaFactor(nY)
}

// refKernel is the test's independent target kernel.
func refKernel(x, y SurfacePoint) float64 {
	d := y.Point.Subtract(x.Point)
	dist := d.Length()
	if dist <= tMin {
		return 0
	}
	w := d.DivideScalar(dist)
	cosX := x.Normal.Dot(w)
	if cosX <= 0 {
		return 0
	}
	return cosX / math.Pi * math.Abs(y.Normal.Dot(w)) / (dist * dist)
}

// refCosineTransport is an independent cosine hemisphere sampler built from a
// uniform on-sphere vector: normalize(n + u) is exactly cosine distributed.
func refCosineTransport(n primitive.Vector, rnd *rand.Rand) primitive.Vector {
	for {
		p := primitive.Vector{
			X: 2*rnd.Float64() - 1,
			Y: 2*rnd.Float64() - 1,
			Z: 2*rnd.Float64() - 1,
		}
		if l2 := p.SquaredLength(); l2 > 1e-12 && l2 <= 1 {
			u := p.DivideScalar(math.Sqrt(l2))
			return n.Add(u).Normalize()
		}
	}
}

func v3(v primitive.Vector) [3]float64 { return [3]float64{v.X, v.Y, v.Z} }
func vec(a [3]float64) primitive.Vector {
	return primitive.Vector{X: a[0], Y: a[1], Z: a[2]}
}

func closeTol(a, b, tol float64) bool {
	return math.Abs(a-b) <= tol*math.Max(1, math.Max(math.Abs(a), math.Abs(b)))
}
func vecClose(a, b primitive.Vector, tol float64) bool {
	return closeTol(a.X, b.X, tol) && closeTol(a.Y, b.Y, tol) && closeTol(a.Z, b.Z, tol)
}

func triArea(a, b, c [3]float64) float64 {
	return 0.5 * tLen(tCross(tSub(b, a), tSub(c, a)))
}

// ---------- TestInstanceMeasure ----------

func TestInstanceMeasure(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		protoA := SphereProto{
			Center: primitive.Vector{X: 0.3, Y: -0.2, Z: 0.1},
			Radius: 1.7,
			Albedo: primitive.Color{R: 0.6, G: 0.5, B: 0.4},
		}
		protoB := SphereProto{
			Center: primitive.Vector{X: -0.1, Y: 0.4, Z: 0},
			Radius: 0.9,
			Albedo: primitive.Color{R: 0.2, G: 0.7, B: 0.5},
		}
		protos := []SphereProto{protoA, protoB}
		matShear := tAffine([3][3]float64{{1.5, 0.4, 0.2}, {0, 0.8, 0.3}, {0.1, 0.2, 2.0}}, 3, -1, 0.5)
		matMirror := tAffine([3][3]float64{{-1.1, 0.5, 0}, {0.3, 1.0, 0.2}, {0, 0.1, 0.9}}, -2, 0.4, 1.2)
		matFar := tUniform(2, 0, 5, -3)
		insts := []Instance{
			{ID: 11, Proto: 0, Matrix: matShear},
			{ID: 22, Proto: 0, Matrix: matMirror}, // prototype reuse: ids 11 and 22 share proto 0
			{ID: 33, Proto: 1, Matrix: matFar},
		}
		scene, err := NewInstanceScene(protos, insts)
		c.Check(err == nil, "valid scene rejected: %v", err)
		if err != nil {
			return
		}
		mini := tBuildScene(protos, insts)
		c.Check(mini.ell[22].det < 0, "mirror matrix should have negative determinant, got %v", mini.ell[22].det)

		// Per-instance scenes isolate the intersection under test.
		solo := map[uint64]*InstanceScene{}
		for _, inst := range insts {
			s, err := NewInstanceScene(protos, []Instance{inst})
			if err != nil {
				c.Check(false, "solo scene failed: %v", err)
				return
			}
			solo[inst.ID] = s
		}

		// Non-unit ray directions; T must keep the O+tD parameterization.
		rays := []primitive.Ray{
			{Origin: primitive.Vector{X: 6, Y: 2.5, Z: -4}, Direction: primitive.Vector{X: -1.7, Y: -0.6, Z: 1.1}},
			{Origin: primitive.Vector{X: -3, Y: 4, Z: 2}, Direction: primitive.Vector{X: 0.8, Y: -1.4, Z: 0.3}},
			{Origin: primitive.Vector{X: 0.5, Y: -3, Z: 6}, Direction: primitive.Vector{X: 0.2, Y: 0.9, Z: -2.2}},
			{Origin: primitive.Vector{X: -6, Y: -2, Z: 5}, Direction: primitive.Vector{X: 1.3, Y: 0.4, Z: -1.8}},
		}
		hitCount := 0
		for _, id := range []uint64{11, 22, 33} {
			e := mini.ell[id]
			for ri, ray := range rays {
				tt, wantHit := e.intersect(v3(ray.Origin), v3(ray.Direction), tMin, math.MaxFloat64)
				ok, h := solo[id].HitSurface(ray, tMin, math.MaxFloat64)
				c.Check(ok == wantHit, "instance %d ray %d: hit mismatch got %v want %v", id, ri, ok, wantHit)
				if !ok || !wantHit {
					continue
				}
				hitCount++
				c.Check(closeTol(h.Hit.T, tt, 1e-9), "instance %d ray %d: T %v want %v", id, ri, h.Hit.T, tt)
				// Point follows O+tD with the non-unit direction.
				p := tAdd(v3(ray.Origin), tScale(v3(ray.Direction), tt))
				c.Check(vecClose(h.Hit.Point, vec(p), 1e-9), "instance %d ray %d: point %v want %v", id, ri, h.Hit.Point, vec(p))
				// Outward normal from the independent ellipsoid gradient.
				gn := e.normal(p)
				c.Check(vecClose(h.Hit.Normal, vec(gn), 1e-9), "instance %d ray %d: normal %v want %v", id, ri, h.Hit.Normal, vec(gn))
				c.Check(closeTol(h.Hit.Normal.Length(), 1, 1e-12), "normal not unit: %v", h.Hit.Normal.Length())
				// Outward means away from the ellipsoid center, also under mirroring.
				c.Check(h.Hit.Normal.Dot(vec(tSub(p, e.m))) > 0, "instance %d ray %d: normal points inward", id, ri)
				// Rays here start outside: the outward normal faces the incoming ray.
				c.Check(h.Hit.Normal.Dot(ray.Direction) < 0, "instance %d ray %d: inside/outside flipped", id, ri)
				// SurfaceRef: right instance, unit local normal matching the map.
				c.Check(h.Ref.Instance == id, "ref instance %d want %d", h.Ref.Instance, id)
				c.Check(closeTol(h.Ref.LocalNormal.Length(), 1, 1e-12), "ref local normal not unit")
				c.Check(vecClose(h.Ref.LocalNormal, vec(e.localOf(p)), 1e-9), "ref local normal %v want %v", h.Ref.LocalNormal, vec(e.localOf(p)))
				// Material comes from the prototype albedo.
				wantAlbedo := protoA.Albedo
				if id == 33 {
					wantAlbedo = protoB.Albedo
				}
				c.Check(h.Hit.Material.Color() == wantAlbedo, "material color %v want %v", h.Hit.Material.Color(), wantAlbedo)
			}
		}
		c.Check(hitCount > 0, "no ray hit anything in intersection checks")

		// Jacobian: a small unit-sphere patch maps to world area through the
		// area factor. Compare triangulated patch areas against AreaFactor.
		e := mini.ell[11]
		n0 := tNorm([3]float64{0.35, 0.8, -0.5})
		t1 := tNorm(tCross(n0, [3]float64{0, 0, 1}))
		t2 := tCross(n0, t1)
		const delta = 1e-4
		n00 := n0
		n10 := tNorm(tAdd(n0, tScale(t1, delta)))
		n01 := tNorm(tAdd(n0, tScale(t2, delta)))
		n11 := tNorm(tAdd(n0, tScale(tAdd(t1, t2), delta)))
		unitArea := triArea(n00, n10, n11) + triArea(n00, n11, n01)
		w00, w10 := e.worldPoint(n00), e.worldPoint(n10)
		w01, w11 := e.worldPoint(n01), e.worldPoint(n11)
		worldArea := triArea(w00, w10, w11) + triArea(w00, w11, w01)
		measured := worldArea / unitArea
		expected := e.areaFactor(n0)
		c.Check(closeTol(measured, expected, 5e-3), "jacobian: measured %v want %v", measured, expected)
		for _, id := range []uint64{11, 22, 33} {
			got, err := solo[id].AreaFactor(SurfaceRef{Instance: id, LocalNormal: vec(n0)})
			c.Check(err == nil, "AreaFactor error: %v", err)
			c.Check(closeTol(got, mini.ell[id].areaFactor(n0), 1e-9),
				"instance %d area factor %v want %v", id, got, mini.ell[id].areaFactor(n0))
		}

		// Resolve agrees with the independent world map.
		rp, err := scene.Resolve(SurfaceRef{Instance: 22, LocalNormal: vec(n0)})
		c.Check(err == nil, "resolve error: %v", err)
		c.Check(vecClose(rp.Point, vec(mini.ell[22].worldPoint(n0)), 1e-12), "resolve point %v want %v", rp.Point, vec(mini.ell[22].worldPoint(n0)))
		c.Check(rp.Albedo == protoA.Albedo, "resolve albedo %v want %v", rp.Albedo, protoA.Albedo)

		// Mutating the input descriptions after construction must not change
		// the scene.
		mutProtos := []SphereProto{protoA, protoB}
		mutInsts := []Instance{
			{ID: 5, Proto: 0, Matrix: matShear},
			{ID: 6, Proto: 1, Matrix: matMirror},
		}
		mutScene, err := NewInstanceScene(mutProtos, mutInsts)
		c.Check(err == nil, "mutation scene failed: %v", err)
		mutRay := primitive.Ray{Origin: primitive.Vector{X: 5, Y: 1, Z: -3}, Direction: primitive.Vector{X: -1.2, Y: 0.3, Z: 0.7}}
		ok1, h1 := mutScene.HitSurface(mutRay, tMin, math.MaxFloat64)
		mutProtos[0].Center = primitive.Vector{X: 99, Y: 99, Z: 99}
		mutProtos[0].Radius = 0.01
		mutProtos[0].Albedo = primitive.Color{R: 0.01, G: 0.01, B: 0.01}
		mutInsts[0].ID = 777
		mutInsts[0].Matrix[0] = 42
		mutInsts[1].Matrix[11] = -37
		ok2, h2 := mutScene.HitSurface(mutRay, tMin, math.MaxFloat64)
		c.Check(ok1 == ok2 && h1.Hit.T == h2.Hit.T && h1.Ref == h2.Ref &&
			h1.Hit.Point == h2.Hit.Point && h1.Hit.Normal == h2.Hit.Normal,
			"scene changed after input mutation")

		// An old pose stays queryable while a new pose is queried concurrently.
		oldPose, err := NewInstanceScene(protos, insts)
		c.Check(err == nil, "old pose failed: %v", err)
		newInsts := []Instance{
			{ID: 11, Proto: 0, Matrix: tUniform(1.2, 3.5, -0.5, 0.2)},
			{ID: 22, Proto: 0, Matrix: matMirror},
			{ID: 33, Proto: 1, Matrix: matFar},
		}
		newPose, err := NewInstanceScene(protos, newInsts)
		c.Check(err == nil, "new pose failed: %v", err)
		probeRays := make([]primitive.Ray, 24)
		for i := range probeRays {
			rnd := rand.New(rand.NewSource(int64(4000 + i)))
			probeRays[i] = primitive.Ray{
				Origin:    primitive.Vector{X: 8*rnd.Float64() - 4, Y: 8*rnd.Float64() - 4, Z: 8*rnd.Float64() - 4},
				Direction: primitive.Vector{X: 2*rnd.Float64() - 1, Y: 2*rnd.Float64() - 1, Z: 2*rnd.Float64() - 1},
			}
		}
		type expect struct {
			ok bool
			h  SurfaceHit
		}
		expOld := make([]expect, len(probeRays))
		expNew := make([]expect, len(probeRays))
		for i, r := range probeRays {
			expOld[i].ok, expOld[i].h = oldPose.HitSurface(r, tMin, math.MaxFloat64)
			expNew[i].ok, expNew[i].h = newPose.HitSurface(r, tMin, math.MaxFloat64)
		}
		var wg sync.WaitGroup
		mismatch := make([]bool, 6)
		for g := 0; g < 6; g++ {
			wg.Add(1)
			go func(g int) {
				defer wg.Done()
				for i, r := range probeRays {
					ok, h := oldPose.HitSurface(r, tMin, math.MaxFloat64)
					if ok != expOld[i].ok || h.Hit.T != expOld[i].h.Hit.T || h.Ref != expOld[i].h.Ref {
						mismatch[g] = true
					}
					ok, h = newPose.HitSurface(r, tMin, math.MaxFloat64)
					if ok != expNew[i].ok || h.Hit.T != expNew[i].h.Hit.T || h.Ref != expNew[i].h.Ref {
						mismatch[g] = true
					}
				}
			}(g)
		}
		wg.Wait()
		for g := range mismatch {
			c.Check(!mismatch[g], "concurrent query mismatch in goroutine %d", g)
		}

		// Invalid construction must fail as a whole.
		good := tUniform(1, 0, 0, 0)
		badProtoCases := []struct {
			name  string
			proto SphereProto
		}{
			{"nan center", SphereProto{Center: primitive.Vector{X: math.NaN()}, Radius: 1, Albedo: primitive.Color{R: 0.5, G: 0.5, B: 0.5}}},
			{"inf radius", SphereProto{Center: primitive.Vector{}, Radius: math.Inf(1), Albedo: primitive.Color{R: 0.5, G: 0.5, B: 0.5}}},
			{"zero radius", SphereProto{Center: primitive.Vector{}, Radius: 0, Albedo: primitive.Color{R: 0.5, G: 0.5, B: 0.5}}},
			{"negative radius", SphereProto{Center: primitive.Vector{}, Radius: -2, Albedo: primitive.Color{R: 0.5, G: 0.5, B: 0.5}}},
			{"albedo below zero", SphereProto{Center: primitive.Vector{}, Radius: 1, Albedo: primitive.Color{R: -0.1, G: 0.5, B: 0.5}}},
			{"albedo above one", SphereProto{Center: primitive.Vector{}, Radius: 1, Albedo: primitive.Color{R: 0.5, G: 1.2, B: 0.5}}},
			{"nan albedo", SphereProto{Center: primitive.Vector{}, Radius: 1, Albedo: primitive.Color{R: 0.5, G: math.NaN(), B: 0.5}}},
		}
		for _, bp := range badProtoCases {
			_, err := NewInstanceScene([]SphereProto{bp.proto}, nil)
			c.Check(err != nil, "prototype case %q was accepted", bp.name)
		}
		nanMat := tUniform(1, 0, 0, 0)
		nanMat[5] = math.NaN()
		badInstCases := []struct {
			name  string
			insts []Instance
		}{
			{"bad proto index", []Instance{{ID: 1, Proto: 5, Matrix: good}}},
			{"negative proto index", []Instance{{ID: 1, Proto: -1, Matrix: good}}},
			{"nan matrix", []Instance{{ID: 1, Proto: 0, Matrix: nanMat}}},
			{"singular matrix", []Instance{{ID: 1, Proto: 0, Matrix: tAffine([3][3]float64{{1, 2, 3}, {2, 4, 6}, {0, 1, 0}}, 0, 0, 0)}}},
			{"zero matrix", []Instance{{ID: 1, Proto: 0, Matrix: [12]float64{}}}},
			{"duplicate ids", []Instance{{ID: 1, Proto: 0, Matrix: good}, {ID: 1, Proto: 0, Matrix: good}}},
		}
		for _, bc := range badInstCases {
			_, err := NewInstanceScene([]SphereProto{protoA}, bc.insts)
			c.Check(err != nil, "instance case %q was accepted", bc.name)
		}

		// Invalid references are rejected.
		_, err = scene.Resolve(SurfaceRef{Instance: 999, LocalNormal: primitive.Vector{X: 1}})
		c.Check(err != nil, "unknown instance id accepted")
		_, err = scene.Resolve(SurfaceRef{Instance: 11, LocalNormal: primitive.Vector{X: 2}})
		c.Check(err != nil, "non-unit local normal accepted")
		_, err = scene.Resolve(SurfaceRef{Instance: 11, LocalNormal: primitive.Vector{X: math.NaN(), Y: 1}})
		c.Check(err != nil, "non-finite local normal accepted")
		_, err = scene.Resolve(SurfaceRef{Instance: 11, LocalNormal: primitive.Vector{X: 1 + 5e-7}})
		c.Check(err == nil, "approximately unit local normal rejected: %v", err)

		// Empty scene is valid and misses everywhere.
		empty, err := NewInstanceScene(nil, nil)
		c.Check(err == nil, "empty scene failed: %v", err)
		ok, _ := empty.Hit(probeRays[0], tMin, math.MaxFloat64)
		c.Check(!ok, "empty scene reported a hit")
	})
}

// ---------- TestProposalMixture ----------

// mixtureScenes builds the current and old poses used by the mixture tests.
func mixtureScenes() (protos []SphereProto, curInsts, oldInsts []Instance) {
	protos = []SphereProto{
		{Center: primitive.Vector{}, Radius: 1.0, Albedo: primitive.Color{R: 0.6, G: 0.5, B: 0.4}},
		{Center: primitive.Vector{X: 0.1, Y: 0, Z: -0.2}, Radius: 0.8, Albedo: primitive.Color{R: 0.2, G: 0.7, B: 0.5}},
	}
	curInsts = []Instance{
		{ID: 1, Proto: 0, Matrix: tUniform(1, 0, 0, 0)},
		{ID: 2, Proto: 1, Matrix: tUniform(1.5, 1.3, 2.4, 1.6)},
		{ID: 5, Proto: 0, Matrix: tAffine([3][3]float64{{-1.1, 0.4, 0.1}, {0.2, 0.9, 0.3}, {0.05, -0.2, 1.3}}, -4, 0, 1)},
	}
	oldInsts = []Instance{
		{ID: 1, Proto: 0, Matrix: tUniform(1.1, 0.6, -0.2, 0.3)},
		{ID: 2, Proto: 1, Matrix: tUniform(1.2, 1.7, 2.1, 1.2)},
		{ID: 5, Proto: 0, Matrix: tAffine([3][3]float64{{0.8, 0.2, 0}, {0, 1.1, 0.1}, {0.2, 0, 0.9}}, -3.5, 0.4, 0.8)},
		{ID: 9, Proto: 1, Matrix: tUniform(1, 0, 5, 0)}, // vanished in the current frame
	}
	return protos, curInsts, oldInsts
}

func TestProposalMixture(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		protos, curInsts, oldInsts := mixtureScenes()
		cur, err := NewInstanceScene(protos, curInsts)
		c.Check(err == nil, "current scene failed: %v", err)
		old, err := NewInstanceScene(protos, oldInsts)
		c.Check(err == nil, "old scene failed: %v", err)
		if err != nil {
			return
		}
		miniCur := tBuildScene(protos, curInsts)
		miniOld := tBuildScene(protos, oldInsts)

		targetRef := SurfaceRef{Instance: 1, LocalNormal: vec(tNorm([3]float64{0.4, 0.75, 0.5}))}
		tgtPt, err := cur.Resolve(targetRef)
		c.Check(err == nil, "target resolve failed: %v", err)
		if err != nil {
			return
		}
		// y on instance 2, on the side facing the target, slightly perturbed
		// so the direction is not perfectly radial. Instance 2 is uniformly
		// scaled, so the local normal is the world offset direction.
		toTarget := tSub(v3(tgtPt.Point), miniCur.ell[2].m)
		yRef := SurfaceRef{Instance: 2, LocalNormal: vec(tNorm(tAdd(toTarget, [3]float64{0.3, 0.2, -0.2})))}

		yPt, err := cur.Resolve(yRef)
		c.Check(err == nil, "y resolve failed: %v", err)
		oldSrcPt, err := old.Resolve(targetRef)
		c.Check(err == nil, "old source resolve failed: %v", err)
		if err != nil {
			return
		}

		// Two sources can reach the same point: both the current target and
		// the old-pose source have positive transported density at y.
		expCur := refDensity(miniCur, miniCur, targetRef, yRef)
		expOld := refDensity(miniCur, miniOld, targetRef, yRef)
		c.Check(expCur > 0 && expOld > 0, "expected both source densities positive, got cur=%v old=%v", expCur, expOld)
		gotCur := transportedDensity(cur, cur, tgtPt, yPt)
		gotOld := transportedDensity(cur, old, oldSrcPt, yPt)
		c.Check(closeTol(gotCur, expCur, 1e-9), "current density %v want %v", gotCur, expCur)
		c.Check(closeTol(gotOld, expOld, 1e-9), "old density %v want %v", gotOld, expOld)
		// qmix is the equal-weight average over all sources, including sources
		// that did not generate a given candidate.
		c.Check(closeTol((gotCur+gotOld)/2, (expCur+expOld)/2, 1e-12), "qmix average mismatch")

		// A source whose view of y is blocked has zero density. The blocker is
		// placed on the segment between the old-pose endpoints.
		xS := miniOld.ell[1].worldPoint(v3(targetRef.LocalNormal))
		yS := miniOld.ell[2].worldPoint(v3(yRef.LocalNormal))
		blockerPos := tAdd(xS, tScale(tSub(yS, xS), 0.4))
		blockerInsts := append(append([]Instance{}, oldInsts...),
			Instance{ID: 3, Proto: 0, Matrix: tUniform(0.2, blockerPos[0], blockerPos[1], blockerPos[2])})
		blocked, err := NewInstanceScene(protos, blockerInsts)
		c.Check(err == nil, "blocked scene failed: %v", err)
		miniBlocked := tBuildScene(protos, blockerInsts)
		blockedSrcPt, err := blocked.Resolve(targetRef)
		c.Check(err == nil, "blocked source resolve failed: %v", err)
		// Sanity: the test's own model agrees the blocker is in the way.
		wBlock := tNorm(tSub(yS, xS))
		bid, _, _ := miniBlocked.firstHit(xS, wBlock, tMin)
		c.Check(bid == 3, "test setup: blocker should be first hit, got instance %d", bid)
		gotBlocked := transportedDensity(cur, blocked, blockedSrcPt, yPt)
		c.Check(gotBlocked == 0, "occluded source density %v want 0", gotBlocked)

		// Mirror + shear instance as the source: density still exact. The
		// target sits on instance 5 facing instance 1; y is the point of
		// instance 1 that looks back at it.
		e5 := miniCur.ell[5]
		y1bRef := SurfaceRef{Instance: 1, LocalNormal: vec(tNorm([3]float64{-0.9, 0.3, 0.25}))}
		offset := tSub(v3(y1bRef.LocalNormal), e5.m)
		nX5 := tNorm(tMul(e5.ai, tNorm(offset)))
		mirrorRef := SurfaceRef{Instance: 5, LocalNormal: vec(nX5)}
		expMirror := refDensity(miniCur, miniCur, mirrorRef, y1bRef)
		mirrorPt, err := cur.Resolve(mirrorRef)
		c.Check(err == nil, "mirror resolve failed: %v", err)
		y1bPt, err := cur.Resolve(y1bRef)
		c.Check(err == nil, "y1b resolve failed: %v", err)
		gotMirror := transportedDensity(cur, cur, mirrorPt, y1bPt)
		c.Check(expMirror > 0, "mirror density should be positive, got %v", expMirror)
		c.Check(closeTol(gotMirror, expMirror, 1e-9), "mirror+shear density %v want %v", gotMirror, expMirror)

		// A source scene that lacks the instance contributes zero density.
		onlyOne, err := NewInstanceScene(protos, []Instance{curInsts[0]})
		c.Check(err == nil, "single scene failed: %v", err)
		onePt, err := onlyOne.Resolve(targetRef)
		c.Check(err == nil, "single resolve failed: %v", err)
		c.Check(transportedDensity(cur, onlyOne, onePt, yPt) == 0, "missing instance density should be zero")

		// Estimator pooling: replay the exact candidate stream with an
		// independent per-sample summation and compare M, S and the selection.
		const nCand = 7
		const seed = 12345
		sources := []ProposalSource{{Scene: old, Ref: targetRef}}
		type srcInfo struct {
			scene *InstanceScene
			mini  tScene
			pt    SurfacePoint
		}
		refSrcs := []srcInfo{
			{old, miniOld, oldSrcPt},
			{cur, miniCur, tgtPt},
		}
		refRnd := rand.New(rand.NewSource(seed))
		var refTotal float64
		var refPositive int
		var refSel SurfaceRef
		var refSelOK bool
		var refSelQ float64
		for _, s := range refSrcs {
			for i := 0; i < nCand; i++ {
				dir := cosineDirection(s.pt.Normal, refRnd)
				ok, hit := s.scene.HitSurface(primitive.Ray{Origin: s.pt.Point, Direction: dir}, tMin, math.MaxFloat64)
				if !ok {
					continue
				}
				y, err := cur.Resolve(hit.Ref)
				if err != nil {
					continue
				}
				k := refKernel(tgtPt, y)
				if k <= 0 {
					continue
				}
				q := 0.0
				for _, s2 := range refSrcs {
					q += refDensity(miniCur, s2.mini, targetRef, y.Ref)
				}
				q /= float64(len(refSrcs))
				if q <= 0 {
					continue
				}
				w := k / q
				refTotal += w
				refPositive++
				if refRnd.Float64() < w/refTotal {
					refSel = y.Ref
					refSelQ = q
					refSelOK = true
				}
			}
		}
		c.Check(refPositive > 0, "pooling scenario is degenerate: no positive weights")
		_, stats, err := EstimateReconnect(cur, targetRef, sources, nCand, rand.New(rand.NewSource(seed)))
		c.Check(err == nil, "estimate failed: %v", err)
		c.Check(stats.M() == nCand*2, "M %d want %d", stats.M(), nCand*2)
		c.Check(stats.PositiveWeights() == refPositive, "positive weights %d want %d", stats.PositiveWeights(), refPositive)
		c.Check(closeTol(stats.TotalWeight(), refTotal, 1e-9), "S %v want %v", stats.TotalWeight(), refTotal)
		sel, ok := stats.SelectedRef()
		c.Check(ok == refSelOK, "selection presence %v want %v", ok, refSelOK)
		if ok && refSelOK {
			c.Check(sel == refSel, "selected ref %v want %v", sel, refSel)
			c.Check(closeTol(stats.SelectedQmix(), refSelQ, 1e-9), "selected qmix %v want %v", stats.SelectedQmix(), refSelQ)
		}

		// All candidates miss: single convex instance in both frames, so every
		// cosine ray leaves forever. M still counts them, no resampling.
		soloCur, err := NewInstanceScene(protos, []Instance{curInsts[0]})
		c.Check(err == nil, "solo current failed: %v", err)
		soloOld, err := NewInstanceScene(protos, []Instance{oldInsts[0]})
		c.Check(err == nil, "solo old failed: %v", err)
		soloRef := SurfaceRef{Instance: 1, LocalNormal: targetRef.LocalNormal}
		soloColor, soloStats, err := EstimateReconnect(soloCur, soloRef,
			[]ProposalSource{{Scene: soloOld, Ref: soloRef}}, 4, rand.New(rand.NewSource(777)))
		c.Check(err == nil, "solo estimate failed: %v", err)
		c.Check(soloStats.M() == 8, "solo M %d want 8", soloStats.M())
		c.Check(soloStats.PositiveWeights() == 0, "solo positive %d want 0", soloStats.PositiveWeights())
		c.Check(soloStats.TotalWeight() == 0, "solo S %v want 0", soloStats.TotalWeight())
		_, ok = soloStats.SelectedRef()
		c.Check(!ok, "solo selection should be empty")
		c.Check(!soloStats.Visible(), "solo visibility should be false")
		c.Check(soloStats.ContinuationCalls() == 0, "solo continuation %d want 0", soloStats.ContinuationCalls())
		// With no surface contribution the result is albedo * sky gradient.
		albedo := protos[0].Albedo
		gR := soloColor.R / albedo.R // gradient red channel = 1 - 0.5 f
		f := 2 * (1 - gR)
		c.Check(f >= -1e-9 && f <= 1+1e-9, "sky blend factor %v outside [0,1]", f)
		c.Check(closeTol(soloColor.G, albedo.G*(1-0.3*f), 1e-9),
			"sky green %v want %v", soloColor.G, albedo.G*(1-0.3*f))
		c.Check(closeTol(soloColor.B, albedo.B, 1e-9),
			"sky blue %v want %v", soloColor.B, albedo.B)

		// Vanished ID: candidates landing on an instance that no longer exists
		// in the current frame are dropped, still counted in M.
		curNo2, err := NewInstanceScene(protos, []Instance{curInsts[0]})
		c.Check(err == nil, "curNo2 failed: %v", err)
		oldWith2, err := NewInstanceScene(protos, []Instance{oldInsts[0], oldInsts[1]})
		c.Check(err == nil, "oldWith2 failed: %v", err)
		_, goneStats, err := EstimateReconnect(curNo2, soloRef,
			[]ProposalSource{{Scene: oldWith2, Ref: soloRef}}, 5, rand.New(rand.NewSource(4242)))
		c.Check(err == nil, "vanished estimate failed: %v", err)
		c.Check(goneStats.M() == 10, "vanished M %d want 10", goneStats.M())
		c.Check(goneStats.PositiveWeights() == 0, "vanished positive %d want 0", goneStats.PositiveWeights())
		c.Check(goneStats.ContinuationCalls() == 0, "vanished continuation %d want 0", goneStats.ContinuationCalls())

		// Validation errors.
		_, _, err = EstimateReconnect(nil, soloRef, nil, 1, rand.New(rand.NewSource(1)))
		c.Check(err != nil, "nil current scene accepted")
		_, _, err = EstimateReconnect(soloCur, soloRef, nil, 1, nil)
		c.Check(err != nil, "nil rand accepted")
		_, _, err = EstimateReconnect(soloCur, soloRef, nil, 0, rand.New(rand.NewSource(1)))
		c.Check(err != nil, "zero candidates accepted")
		_, _, err = EstimateReconnect(soloCur, soloRef, nil, -3, rand.New(rand.NewSource(1)))
		c.Check(err != nil, "negative candidates accepted")
		_, _, err = EstimateReconnect(soloCur, SurfaceRef{Instance: 77, LocalNormal: primitive.Vector{X: 1}}, nil, 1, rand.New(rand.NewSource(1)))
		c.Check(err != nil, "invalid target ref accepted")
		_, _, err = EstimateReconnect(soloCur, soloRef, []ProposalSource{{Scene: nil, Ref: soloRef}}, 1, rand.New(rand.NewSource(1)))
		c.Check(err != nil, "nil source scene accepted")
		_, _, err = EstimateReconnect(soloCur, soloRef, []ProposalSource{{Scene: soloOld, Ref: SurfaceRef{Instance: 55, LocalNormal: primitive.Vector{X: 1}}}}, 1, rand.New(rand.NewSource(1)))
		c.Check(err != nil, "invalid source ref accepted")

		// RenderReconnect validation errors.
		cam := primitive.NewCamera(primitive.Vector{Y: 3, Z: 9}, primitive.Vector{Y: 0.5}, 50, 4.0/3.0, 0)
		_, err = RenderReconnect(nil, cam, nil, 4, 4, 1, 1, 1, 0)
		c.Check(err != nil, "nil scene accepted by renderer")
		_, err = RenderReconnect(soloCur, nil, nil, 4, 4, 1, 1, 1, 0)
		c.Check(err != nil, "nil camera accepted by renderer")
		_, err = RenderReconnect(soloCur, cam, nil, 0, 4, 1, 1, 1, 0)
		c.Check(err != nil, "zero width accepted")
		_, err = RenderReconnect(soloCur, cam, nil, 4, 0, 1, 1, 1, 0)
		c.Check(err != nil, "zero height accepted")
		_, err = RenderReconnect(soloCur, cam, nil, 4, 4, 0, 1, 1, 0)
		c.Check(err != nil, "zero samples accepted")
		_, err = RenderReconnect(soloCur, cam, nil, 4, 4, 1, 0, 1, 0)
		c.Check(err != nil, "zero candidates accepted by renderer")
		_, err = RenderReconnect(soloCur, cam, nil, 4, 4, 1, 1, 0, 0)
		c.Check(err != nil, "zero workers accepted")
		_, err = RenderReconnect(soloCur, cam, []*InstanceScene{nil}, 4, 4, 1, 1, 1, 0)
		c.Check(err != nil, "nil old scene accepted")

		// Empty scene renders as sky: the gradient keeps B == 1 and
		// 0.5 <= R <= G <= B.
		empty, err := NewInstanceScene(nil, nil)
		c.Check(err == nil, "empty scene failed: %v", err)
		skyFrame, err := RenderReconnect(empty, cam, nil, 8, 6, 2, 3, 2, 55)
		c.Check(err == nil, "empty render failed: %v", err)
		skyOK := len(skyFrame) == 48
		for _, px := range skyFrame {
			skyOK = skyOK && closeTol(px.B, 1, 1e-9) && px.R <= px.G+1e-12 && px.G <= px.B+1e-12 && px.R >= 0.5-1e-9
		}
		c.Check(skyOK, "empty scene frame is not the sky gradient")

		// Cross-worker repeatability: identical frames for any worker count.
		f1, err := RenderReconnect(cur, cam, []*InstanceScene{old}, 16, 12, 2, 3, 1, 777)
		c.Check(err == nil, "workers=1 render failed: %v", err)
		f3, err := RenderReconnect(cur, cam, []*InstanceScene{old}, 16, 12, 2, 3, 3, 777)
		c.Check(err == nil, "workers=3 render failed: %v", err)
		f8, err := RenderReconnect(cur, cam, []*InstanceScene{old}, 16, 12, 2, 3, 8, 777)
		c.Check(err == nil, "workers=8 render failed: %v", err)
		same := len(f1) == len(f3) && len(f1) == len(f8)
		for i := range f1 {
			same = same && f1[i] == f3[i] && f1[i] == f8[i]
		}
		c.Check(same, "frames differ across worker counts")
		f1b, err := RenderReconnect(cur, cam, []*InstanceScene{old}, 16, 12, 2, 3, 1, 777)
		c.Check(err == nil, "repeat render failed: %v", err)
		same = len(f1) == len(f1b)
		for i := range f1 {
			same = same && f1[i] == f1b[i]
		}
		c.Check(same, "same seed produced different frames")
	})
}

// ---------- TestReconnectTransport ----------

type runningStat struct {
	n    int
	sum  float64
	sum2 float64
}

func (r *runningStat) add(v float64) { r.n++; r.sum += v; r.sum2 += v * v }
func (r *runningStat) mean() float64 { return r.sum / float64(r.n) }
func (r *runningStat) stderr() float64 {
	if r.n < 2 {
		return 0
	}
	v := r.sum2/float64(r.n) - r.mean()*r.mean()
	if v < 0 {
		v = 0
	}
	return math.Sqrt(v / float64(r.n))
}

func TestReconnectTransport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		protos := []SphereProto{
			{Center: primitive.Vector{}, Radius: 1, Albedo: primitive.Color{R: 0.75, G: 0.60, B: 0.45}},
			{Center: primitive.Vector{}, Radius: 1, Albedo: primitive.Color{R: 0.25, G: 0.55, B: 0.80}},
			{Center: primitive.Vector{}, Radius: 1, Albedo: primitive.Color{R: 0.50, G: 0.35, B: 0.30}},
		}
		rotZ := func(a float64) [3][3]float64 {
			ca, sa := math.Cos(a), math.Sin(a)
			return [3][3]float64{{1.1 * ca, -1.1 * sa, 0}, {1.1 * sa, 1.1 * ca, 0}, {0, 0, 1.1}}
		}
		curInsts := []Instance{
			{ID: 1, Proto: 0, Matrix: tUniform(1, 0, 0, 0)},
			{ID: 2, Proto: 1, Matrix: tUniform(0.9, 3.2, 1.2, 0.5)},
		}
		oldInsts := []Instance{
			{ID: 1, Proto: 0, Matrix: tAffine(rotZ(0.4), 0.4, -0.3, 0.2)},
			{ID: 2, Proto: 1, Matrix: tUniform(1.15, 2.6, 1.5, -0.3)},
		}
		// Variant B adds an occluder between the target and instance 2.
		occCur := Instance{ID: 4, Proto: 2, Matrix: tUniform(0.35, 2.0, 0.8, 0.32)}
		occOld := Instance{ID: 4, Proto: 2, Matrix: tUniform(0.4, 1.1, 1.0, 0.6)}

		cur, err := NewInstanceScene(protos, curInsts)
		c.Check(err == nil, "current failed: %v", err)
		old, err := NewInstanceScene(protos, oldInsts)
		c.Check(err == nil, "old failed: %v", err)
		curB, err := NewInstanceScene(protos, append(append([]Instance{}, curInsts...), occCur))
		c.Check(err == nil, "current B failed: %v", err)
		oldB, err := NewInstanceScene(protos, append(append([]Instance{}, oldInsts...), occOld))
		c.Check(err == nil, "old B failed: %v", err)
		if err != nil {
			return
		}

		targetRef := SurfaceRef{Instance: 1, LocalNormal: vec(tNorm([3]float64{1, 0.5, 0.2}))}
		tgtPt, err := cur.Resolve(targetRef)
		c.Check(err == nil, "target resolve failed: %v", err)
		if err != nil {
			return
		}

		// Cross-pose scaling: the area factor ratio matches the independent
		// computation.
		miniCur := tBuildScene(protos, curInsts)
		miniOld := tBuildScene(protos, oldInsts)
		yRef := SurfaceRef{Instance: 2, LocalNormal: vec(tNorm([3]float64{-0.7, 0.4, 0.3}))}
		fCur, err := cur.AreaFactor(yRef)
		c.Check(err == nil, "cur area factor failed: %v", err)
		fOld, err := old.AreaFactor(yRef)
		c.Check(err == nil, "old area factor failed: %v", err)
		wantRatio := miniOld.ell[2].areaFactor(v3(yRef.LocalNormal)) / miniCur.ell[2].areaFactor(v3(yRef.LocalNormal))
		c.Check(closeTol(fOld/fCur, wantRatio, 1e-12), "cross-pose area ratio %v want %v", fOld/fCur, wantRatio)

		// The old scene is queryable before and after everything below.
		probeRay := primitive.Ray{Origin: primitive.Vector{X: 1, Y: 4, Z: 2}, Direction: primitive.Vector{X: 0.3, Y: -1.1, Z: 0.2}}
		oldBefore, oldHitBefore := old.HitSurface(probeRay, tMin, math.MaxFloat64)

		// Mean transport check: fixed-seed batches of the estimator against an
		// independent integration over current-frame directions.
		runMean := func(scene, srcScene *InstanceScene, batches, refSamples int) (prod, ref [3]runningStat) {
			for b := 0; b < batches; b++ {
				rnd := rand.New(rand.NewSource(9000 + int64(b)))
				col, _, err := EstimateReconnect(scene, targetRef,
					[]ProposalSource{{Scene: srcScene, Ref: targetRef}}, 3, rnd)
				if err != nil {
					c.Check(false, "estimate failed: %v", err)
					return
				}
				prod[0].add(col.R)
				prod[1].add(col.G)
				prod[2].add(col.B)
			}
			// Independent reference: integrate over cosine directions in the
			// current frame; continuation uses the original bounce + color.
			rrnd := rand.New(rand.NewSource(31337))
			for j := 0; j < refSamples; j++ {
				w := refCosineTransport(tgtPt.Normal, rrnd)
				ok, hit := scene.HitSurface(primitive.Ray{Origin: tgtPt.Point, Direction: w}, tMin, math.MaxFloat64)
				var g primitive.Color
				if !ok {
					g = primitive.Gradient(primitive.White, primitive.Blue, w.Y)
				} else {
					yp, err := scene.Resolve(hit.Ref)
					if err != nil {
						c.Check(false, "resolve failed: %v", err)
						return
					}
					mat := primitive.Lambertian{Attenuation: yp.Albedo}
					rec := primitive.Hit{Point: yp.Point, Normal: yp.Normal, Material: mat}
					_, bouncedRay := mat.Bounce(primitive.Ray{Origin: tgtPt.Point, Direction: w}, rec, rrnd)
					g = mat.Color().Multiply(color(bouncedRay, scene, rrnd, 2))
				}
				g = tgtPt.Albedo.Multiply(g)
				ref[0].add(g.R)
				ref[1].add(g.G)
				ref[2].add(g.B)
			}
			return prod, ref
		}

		const batches = 60000
		const refSamples = 60000
		names := [3]string{"R", "G", "B"}
		variants := []struct {
			name string
			cur  *InstanceScene
			old  *InstanceScene
		}{
			{"plain", cur, old},
			{"occluded", curB, oldB},
		}
		for _, sc := range variants {
			prod, ref := runMean(sc.cur, sc.old, batches, refSamples)
			for ch := 0; ch < 3; ch++ {
				diff := math.Abs(prod[ch].mean() - ref[ch].mean())
				se := math.Hypot(prod[ch].stderr(), ref[ch].stderr())
				tol := 6*se + 2e-3
				lo := prod[ch].mean() - 1.96*prod[ch].stderr()
				hi := prod[ch].mean() + 1.96*prod[ch].stderr()
				t.Logf("transport %s/%s: batches=%d refSamples=%d prod=%.5f ci95=[%.5f,%.5f] ref=%.5f err=%.5f tol=%.5f",
					sc.name, names[ch], batches, refSamples, prod[ch].mean(), lo, hi, ref[ch].mean(), diff, tol)
				c.Check(diff < tol, "transport %s/%s: estimator mean %v vs reference %v (err %v, tol %v)",
					sc.name, names[ch], prod[ch].mean(), ref[ch].mean(), diff, tol)
			}
		}

		// Determinism: identical seeds give identical results.
		ca, sa, err := EstimateReconnect(cur, targetRef, []ProposalSource{{Scene: old, Ref: targetRef}}, 3, rand.New(rand.NewSource(5150)))
		c.Check(err == nil, "determinism run A failed: %v", err)
		cb, sb, err := EstimateReconnect(cur, targetRef, []ProposalSource{{Scene: old, Ref: targetRef}}, 3, rand.New(rand.NewSource(5150)))
		c.Check(err == nil, "determinism run B failed: %v", err)
		c.Check(ca == cb && sa == sb, "same seed gave different results")

		// Target occlusion: for candidates on instance 2 the visibility flag
		// must match an independent first-hit computation, and a continuation
		// runs exactly when the selected point is visible.
		miniCurB := tBuildScene(protos, append(append([]Instance{}, curInsts...), occCur))
		foundOccluded := false
		foundVisible := false
		for seed := int64(0); seed < 400 && !(foundOccluded && foundVisible); seed++ {
			_, st, err := EstimateReconnect(curB, targetRef,
				[]ProposalSource{{Scene: oldB, Ref: targetRef}}, 2, rand.New(rand.NewSource(60000+seed)))
			c.Check(err == nil, "occlusion probe failed: %v", err)
			sel, ok := st.SelectedRef()
			if !ok {
				continue
			}
			if st.Visible() {
				foundVisible = true
				c.Check(st.ContinuationCalls() == 1, "visible candidate continuation %d want 1", st.ContinuationCalls())
			} else {
				c.Check(st.ContinuationCalls() == 0, "invisible candidate continuation %d want 0", st.ContinuationCalls())
			}
			if sel.Instance != 2 {
				continue
			}
			// Independent visibility of the selected y from the target.
			ySel, err := curB.Resolve(sel)
			if err != nil {
				c.Check(false, "occlusion resolve failed: %v", err)
				return
			}
			d := tSub(v3(ySel.Point), v3(tgtPt.Point))
			dist := tLen(d)
			w := tScale(d, 1/dist)
			id, tt, ok := miniCurB.firstHit(v3(tgtPt.Point), w, tMin)
			expVis := ok && id == 2 &&
				tLen(tSub(tAdd(v3(tgtPt.Point), tScale(w, tt)), v3(ySel.Point))) <= 1e-9*math.Max(1, dist)
			c.Check(st.Visible() == expVis, "visibility %v want %v", st.Visible(), expVis)
			if !expVis {
				foundOccluded = true
			}
		}
		c.Check(foundOccluded, "no occluded candidate selected in probe")
		c.Check(foundVisible, "no visible candidate selected in probe")

		// The old scene is untouched by building the new pose and by all the
		// estimator runs above.
		oldAfter, oldHitAfter := old.HitSurface(probeRay, tMin, math.MaxFloat64)
		c.Check(oldBefore == oldAfter && oldHitBefore.Ref == oldHitAfter.Ref && oldHitBefore.Hit.T == oldHitAfter.Hit.T,
			"old scene changed across poses and estimator runs")
	})
}
