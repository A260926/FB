package render

import (
	"math"
	"math/rand"
	"testing"

	"github.com/markphelps/go-trace/primitive"
	"github.com/markphelps/go-trace/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

func identity(scale float64, tx, ty, tz float64) [3][4]float64 {
	return [3][4]float64{
		{scale, 0, 0, tx},
		{0, scale, 0, ty},
		{0, 0, scale, tz},
	}
}

func mustScene(t *testing.T, protos []SpherePrototype, insts []InstanceDesc) *InstanceScene {
	t.Helper()
	sc, err := NewInstanceScene(protos, insts)
	if err != nil {
		t.Fatalf("NewInstanceScene: %v", err)
	}
	return sc
}

func closeTo(a, b, eps float64) bool { return math.Abs(a-b) <= eps }

// ---------------------------------------------------------------------------
// TestInstanceMeasure
// ---------------------------------------------------------------------------

func TestInstanceMeasure(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- independent ellipsoid quadratic check -------------------------
		// A sphere of prototype radius 1 scaled by diag(2,0.5,1.5) is an
		// ellipsoid x^2/4 + y^2/0.25 + z^2/2.25 = 1. Verify the non-unit
		// ray intersection against that quadratic directly.
		proto := []SpherePrototype{{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: .4, G: .5, B: .6}}}
		inst := []InstanceDesc{{ID: 1, Prototype: 0, Matrix: identity(1, 0, 0, 0)}}
		inst[0].Matrix = [3][4]float64{{2, 0, 0, 0}, {0, 0.5, 0, 0}, {0, 0, 1.5, 0}}
		sc := mustScene(t, proto, inst)

		origin := primitive.Vector{X: 0.3, Y: 0.2, Z: 0}
		target := primitive.Vector{X: 4.2, Y: 1.1, Z: 2.7}
		ray := primitive.Ray{Origin: origin, Direction: target.Subtract(origin)} // deliberately not unit

		ok, hit, _ := sc.HitRef(ray, 0.001, math.MaxFloat64)
		c.Check(ok, "ellipsoid ray must hit")
		c.Check(hit.T > 0, "hit T must be positive")

		// Independent quadratic in world coordinates. The ellipsoid is
		// x^2/4 + y^2/0.25 + z^2/2.25 = 1 (prototype radius 1).
		d := ray.Direction
		p := ray.Origin
		qa := d.X*d.X/(2*2) + d.Y*d.Y/(0.5*0.5) + d.Z*d.Z/(1.5*1.5)
		qb := 2 * (p.X*d.X/(2*2) + p.Y*d.Y/(0.5*0.5) + p.Z*d.Z/(1.5*1.5))
		qc := p.X*p.X/(2*2) + p.Y*p.Y/(0.5*0.5) + p.Z*p.Z/(1.5*1.5) - 1
		disc := qb*qb - 4*qa*qc
		c.Check(disc > 0, "independent quadratic discriminant positive")
		wantT := (-qb + math.Sqrt(disc)) / (2 * qa)
		c.Check(closeTo(hit.T, wantT, 1e-9), "affine hit t matches ellipsoid quadratic: got %v want %v", hit.T, wantT)
		c.Check(closeTo(hit.Point.X, p.X+d.X*hit.T, 1e-12) &&
			closeTo(hit.Point.Y, p.Y+d.Y*hit.T, 1e-12) &&
			closeTo(hit.Point.Z, p.Z+d.Z*hit.T, 1e-12), "O+tD preserved")

		// Independent outward normal: gradient of x^2/4 + y^2/0.25 + z^2/2.25.
		hp := hit.Point
		grad := primitive.Vector{X: 2 * hp.X / 4, Y: 2 * hp.Y / 0.25, Z: 2 * hp.Z / 2.25}.Normalize()
		c.Check(closeTo(hit.Normal.X, grad.X, 1e-7) &&
			closeTo(hit.Normal.Y, grad.Y, 1e-7) &&
			closeTo(hit.Normal.Z, grad.Z, 1e-7), "normal is normalized inverse-transpose gradient")
		c.Check(closeTo(hit.Normal.Length(), 1, 1e-12), "normal unit length")

		// Non-unit direction and unit direction with the same world line must
		// agree on T only when t is measured in the same parameter; verify by
		// scaling the direction and checking O+tD still lands on the surface.
		scaled := primitive.Ray{Origin: origin, Direction: ray.Direction.MultiplyScalar(2.5)}
		ok2, hit2, _ := sc.HitRef(scaled, 0.001, math.MaxFloat64)
		c.Check(ok2, "scaled ray hits")
		c.Check(closeTo(hit2.Point.X, hit.Point.X, 1e-9) &&
			closeTo(hit2.Point.Y, hit.Point.Y, 1e-9) &&
			closeTo(hit2.Point.Z, hit.Point.Z, 1e-9), "t scales inversely with direction length")

		// --- Jacobian via small surface patch ---------------------------------
		// For an instance with matrix A and prototype radius r, a small solid
		// angle dω on the unit sphere maps to area r^2 |det A| |A^-T n| dω.
		// Compare against the numerically measured area of a patch.
		lambert := primitive.Color{R: .2, G: .8, B: .35}
		protoJ := []SpherePrototype{{Center: primitive.Vector{X: 0.1, Y: -0.2, Z: 0.3}, Radius: 1.7, Color: lambert}}
		matJ := [3][4]float64{{1.6, 0.35, 0.1, 2.0}, {0.0, 0.7, 0.25, -0.4}, {0.2, 0.0, 1.25, 0.9}}
		scJ := mustScene(t, protoJ, []InstanceDesc{{ID: 77, Prototype: 0, Matrix: matJ}})

		normal := primitive.Vector{X: 0.3, Y: -0.5, Z: 0.8}.Normalize()
		refJ := SurfaceRef{Instance: 77, LocalNormal: normal}
		surfJ, err := scJ.Surface(refJ)
		c.Check(err == nil, "jacobian surface resolves: %v", err)

		// Analytic area factor from the documented formula.
		var A mat3
		for r := 0; r < 3; r++ {
			for col := 0; col < 3; col++ {
				A[r][col] = matJ[r][col]
			}
		}
		inv, det, invOK := invert3(A)
		c.Check(invOK, "matrix invertible")
		// A^-T n
		var invT mat3
		for r := 0; r < 3; r++ {
			for col := 0; col < 3; col++ {
				invT[r][col] = inv[col][r]
			}
		}
		an := invT.mulVec(normal).Length()
		analytic := protoJ[0].Radius * protoJ[0].Radius * math.Abs(det) * an
		internal := scJ.areaFactor(refJ)
		c.Check(closeTo(analytic, internal, 1e-10), "area factor formula: analytic %v internal %v", analytic, internal)

		// Numeric patch area: small disc of solid angle around n on the unit
		// sphere, mapped to the world surface.
		// Build an orthonormal tangent basis at normal.
		helper := primitive.Vector{X: 1}
		if math.Abs(normal.X) > 0.9 {
			helper = primitive.Vector{Y: 1}
		}
		tb := normal.Cross(helper).Normalize()
		bb := normal.Cross(tb)
		const kSteps = 40
		const ring = 2e-3 // patch radius in radians
		numeric := 0.0
		radial := 0.0
		for i := 0; i < kSteps; i++ {
			a0 := 2 * math.Pi * float64(i) / kSteps
			a1 := 2 * math.Pi * float64(i+1) / kSteps
			p0 := surfacePointOnProto(protoJ[0], normal, tb, bb, radial, a0)
			p1 := surfacePointOnProto(protoJ[0], normal, tb, bb, ring, a0)
			p2 := surfacePointOnProto(protoJ[0], normal, tb, bb, ring, a1)
			w0 := applyAffine(matJ, p0)
			w1 := applyAffine(matJ, p1)
			w2 := applyAffine(matJ, p2)
			// Triangle area via cross product.
			numeric += w1.Subtract(w0).Cross(w2.Subtract(w0)).Length() / 2
		}
		// The numeric patch subtends solid angle Ω = π*ring^2 (small angle).
		omega := math.Pi * ring * ring
		numericPerSr := numeric / omega
		c.Check(closeTo(numericPerSr/analytic, 1, 5e-3),
			"jacobian ratio numeric/analytic = %v", numericPerSr/analytic)
		c.Check(surfJ.Normal.Length() > 0, "jacobian surface normal valid")

		// --- mirror does not flip inside/outside -----------------------------
		mirror := [3][4]float64{{-1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}
		scM := mustScene(t, proto, []InstanceDesc{{ID: 5, Prototype: 0, Matrix: mirror}})
		// A ray from +Z toward the origin hits the outward-facing +Z side.
		okM, hitM, refM := scM.HitRef(primitive.Ray{Origin: primitive.Vector{Z: 3}, Direction: primitive.Vector{Z: -1}}, 0.001, 100)
		c.Check(okM, "mirrored sphere hit")
		c.Check(hitM.Point.Z > 0, "mirrored hit on near side")
		c.Check(hitM.Normal.Z > 0, "outward normal faces the camera after mirror")
		c.Check(refM.Instance == 5, "mirrored ref instance")
		c.Check(closeTo(refM.LocalNormal.Z, 1, 1e-9), "mirrored local normal is +Z")

		// --- validation ------------------------------------------------------
		_, err = NewInstanceScene(
			[]SpherePrototype{{Center: primitive.Vector{}, Radius: -1, Color: primitive.Color{}}},
			nil)
		c.Check(err != nil, "negative radius rejected")
		_, err = NewInstanceScene(
			[]SpherePrototype{{Center: primitive.Vector{}, Radius: math.Inf(1), Color: primitive.Color{}}},
			nil)
		c.Check(err != nil, "infinite radius rejected")
		_, err = NewInstanceScene(
			[]SpherePrototype{{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 1.2}}},
			nil)
		c.Check(err != nil, "color above 1 rejected")
		_, err = NewInstanceScene(
			[]SpherePrototype{{Center: primitive.Vector{X: math.NaN()}, Radius: 1, Color: primitive.Color{}}},
			nil)
		c.Check(err != nil, "non-finite center rejected")
		_, err = NewInstanceScene(proto,
			[]InstanceDesc{{ID: 1, Prototype: 9, Matrix: identity(1, 0, 0, 0)}})
		c.Check(err != nil, "bad prototype index rejected")
		_, err = NewInstanceScene(proto,
			[]InstanceDesc{{ID: 1, Prototype: 0, Matrix: [3][4]float64{{0, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}})
		c.Check(err != nil, "singular matrix rejected")
		_, err = NewInstanceScene(proto,
			[]InstanceDesc{{ID: 1, Prototype: 0, Matrix: [3][4]float64{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, math.Inf(1), 0}}}})
		c.Check(err != nil, "non-finite matrix rejected")
		_, err = NewInstanceScene(proto,
			[]InstanceDesc{{ID: 1, Prototype: 0, Matrix: identity(1, 0, 0, 0)}, {ID: 1, Prototype: 0, Matrix: identity(1, 1, 0, 0)}})
		c.Check(err != nil, "duplicate instance id rejected")

		// Invalid refs and non-positive candidates are errors.
		_, err = sc.Surface(SurfaceRef{Instance: 999, LocalNormal: primitive.Vector{Z: 1}})
		c.Check(err != nil, "unknown instance ref rejected")
		_, err = sc.Surface(SurfaceRef{Instance: 1, LocalNormal: primitive.Vector{X: 2, Z: 1}})
		c.Check(err != nil, "non-unit local normal rejected")
		_, _, err = EstimateReconnect(sc, SurfaceRef{Instance: 12345, LocalNormal: primitive.Vector{Z: 1}}, nil, 4, rand.New(rand.NewSource(1)))
		c.Check(err != nil, "invalid target reference errors")
		_, _, err = EstimateReconnect(sc, SurfaceRef{Instance: 1, LocalNormal: primitive.Vector{Z: 1}}, nil, 0, rand.New(rand.NewSource(1)))
		c.Check(err != nil, "zero candidates errors")
		_, _, err = EstimateReconnect(sc, SurfaceRef{Instance: 1, LocalNormal: primitive.Vector{Z: 1}}, nil, 4, nil)
		c.Check(err != nil, "nil random source errors")

		// --- input mutation --------------------------------------------------
		protosIn := []SpherePrototype{{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: .5, G: .5, B: .5}}}
		instsIn := []InstanceDesc{{ID: 1, Prototype: 0, Matrix: identity(1, 0, 0, 0)}}
		frozen := mustScene(t, protosIn, instsIn)
		protosIn[0].Radius = 99
		protosIn[0].Color = primitive.Color{R: 1}
		instsIn[0].Matrix[0][0] = 50
		instsIn[0].ID = 4242
		okF, hitF, _ := frozen.HitRef(primitive.Ray{Origin: primitive.Vector{Z: 3}, Direction: primitive.Vector{Z: -1}}, 0.001, 100)
		c.Check(okF, "frozen scene still hits after caller mutation")
		c.Check(closeTo(hitF.T, 2, 1e-9), "frozen scene kept original transform: t=%v", hitF.T)
		c.Check(frozen.HasInstance(1), "frozen scene kept original id")

		// --- empty scene renders as sky --------------------------------------
		empty := mustScene(t, nil, nil)
		c.Check(empty.Count() == 0, "empty scene count zero")
		okE, _ := empty.Hit(primitive.Ray{Origin: primitive.Vector{}, Direction: primitive.Vector{Z: -1}}, 0.001, 100)
		c.Check(!okE, "empty scene misses")
		img, err := RenderReconnect(empty, primitive.NewCamera(primitive.Vector{Z: 1}, primitive.Vector{}, 60, 1, 0), nil, 4, 3, 1, 1, 1, 5)
		c.Check(err == nil, "empty scene render errors: %v", err)
		allSky := true
		for y := range img {
			for x := range img[y] {
				for k := 0; k < 3; k++ {
					if img[y][x][k] <= 0 {
						allSky = false
					}
				}
			}
		}
		c.Check(allSky, "empty scene renders non-zero sky")

		// --- render dimension validation -------------------------------------
		cam := primitive.NewCamera(primitive.Vector{Z: 1}, primitive.Vector{}, 60, 1, 0)
		_, err = RenderReconnect(sc, cam, nil, 0, 4, 1, 1, 1, 1)
		c.Check(err != nil, "zero width rejected")
		_, err = RenderReconnect(sc, cam, nil, 4, 0, 1, 1, 1, 1)
		c.Check(err != nil, "zero height rejected")
		_, err = RenderReconnect(sc, cam, nil, 4, 4, 0, 1, 1, 1)
		c.Check(err != nil, "zero samples rejected")
		_, err = RenderReconnect(sc, cam, nil, 4, 4, 1, 0, 1, 1)
		c.Check(err != nil, "zero candidates rejected")
		_, err = RenderReconnect(sc, cam, nil, 4, 4, 1, 1, 0, 1)
		c.Check(err != nil, "zero workers rejected")
	})
}

// surfacePointOnProto returns the local prototype surface point at polar angle
// t and azimuth a around the given unit direction.
func surfacePointOnProto(p SpherePrototype, n, tb, bb primitive.Vector, t, a float64) primitive.Vector {
	dir := n.MultiplyScalar(math.Cos(t)).
		Add(tb.MultiplyScalar(math.Sin(t) * math.Cos(a))).
		Add(bb.MultiplyScalar(math.Sin(t) * math.Sin(a)))
	return p.Center.Add(dir.MultiplyScalar(p.Radius))
}

func applyAffine(m [3][4]float64, v primitive.Vector) primitive.Vector {
	return primitive.Vector{
		X: m[0][0]*v.X + m[0][1]*v.Y + m[0][2]*v.Z + m[0][3],
		Y: m[1][0]*v.X + m[1][1]*v.Y + m[1][2]*v.Z + m[1][3],
		Z: m[2][0]*v.X + m[2][1]*v.Y + m[2][2]*v.Z + m[2][3],
	}
}

// ---------------------------------------------------------------------------
// TestProposalMixture
// ---------------------------------------------------------------------------

func TestProposalMixture(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		lambert := primitive.Color{R: .45, G: .55, B: .65}
		protos := []SpherePrototype{
			{Center: primitive.Vector{}, Radius: 1, Color: lambert},
			{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: .5, G: .5, B: .5}},
		}
		// The target frustum faces +Z; sphere 1 sits in front of it at world
		// +Z and emits the shared proposals. Sphere 2 moves between poses.
		current := mustScene(t, protos, []InstanceDesc{
			{ID: 0, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
			{ID: 1, Prototype: 1, Matrix: identity(1, 0.0, 0.0, 2.2)},
			{ID: 2, Prototype: 0, Matrix: identity(0.45, 1.5, 0.3, 2.4)},
		})
		target := SurfaceRef{Instance: 0, LocalNormal: primitive.Vector{Z: 1}}
		targetSurf, err := current.Surface(target)
		c.Check(err == nil, "target resolves")

		// Old pose 1: same IDs, sphere 2 shifted behind and to the side.
		old1 := mustScene(t, protos, []InstanceDesc{
			{ID: 0, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
			{ID: 1, Prototype: 1, Matrix: identity(1, 0.0, 0.0, 2.2)},
			{ID: 2, Prototype: 0, Matrix: identity(0.45, 1.9, 0.45, 2.2)},
		})
		// Old pose 2: shear + mirror, to exercise the area factor.
		old2 := mustScene(t, protos, []InstanceDesc{
			{ID: 0, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
			{ID: 1, Prototype: 1, Matrix: identity(1, 0.0, 0.0, 2.2)},
			{ID: 2, Prototype: 0, Matrix: [3][4]float64{{-0.45, 0.2, 0.1, 1.1}, {0.05, 0.45, 0.1, 0.2}, {0.1, 0, 0.5, 2.6}}},
		})
		n1 := primitive.Vector{X: 0.15, Y: -0.1, Z: -1}.Normalize()
		n2 := primitive.Vector{X: -0.5, Y: 0.2, Z: -0.8}.Normalize()
		src1 := Source{Scene: old1, Surface: SurfaceRef{Instance: 1, LocalNormal: n1}}
		src2 := Source{Scene: old2, Surface: SurfaceRef{Instance: 1, LocalNormal: n2}}
		proposals := []ProposalSource{src1, src2}

		// Independently sample candidate pools trial by trial and sum K/q over
		// every proposal, including proposals with zero weight. Compare the
		// mean budget, positive count and total weight with the estimator.
		sources := []*InstanceScene{old1, old2, current}
		srcRefs := []SurfaceRef{src1.Surface, src2.Surface, target}
		const nCand = 5
		M := nCand * len(sources)

		refTrial := func(rndRef *rand.Rand) (sum, pos int, weight float64, hitEmitter float64) {
			weight = 0
			for i, sc := range sources {
				surf, _ := sc.Surface(srcRefs[i])
				for k := 0; k < nCand; k++ {
					dir := cosineHemisphere(surf.Normal, rndRef)
					hit, _, ref := sc.HitRef(primitive.Ray{Origin: surf.Point, Direction: dir}, tMin, math.MaxFloat64)
					if !hit {
						continue
					}
					ys, e := current.Surface(SurfaceRef{Instance: ref.Instance, LocalNormal: ref.LocalNormal})
					if e != nil {
						continue
					}
					q := 0.0
					for j, sj := range sources {
						q += densityForSource(current, sj, srcRefs[j], ys.Ref, tMin)
					}
					q /= float64(len(sources))
					K := kernel(targetSurf, ys)
					if K > 0 && q > 0 {
						weight += K / q
						pos++
						if ys.Ref.Instance == 1 {
							hitEmitter += K / q
						}
					}
				}
			}
			return
		}

		const trials = 400
		refRnd := rand.New(rand.NewSource(2024))
		var refWeightSum, refPosum, refInst2Sum float64
		for i := 0; i < trials; i++ {
			_, pos, w, i2 := refTrial(refRnd)
			refWeightSum += w
			refPosum += float64(pos)
			refInst2Sum += i2
		}

		estRnd := rand.New(rand.NewSource(777))
		var estWeightSum, estPosum, estInst2Selected float64
		for i := 0; i < trials; i++ {
			_, stats, err := EstimateReconnect(current, target, proposals, nCand, estRnd)
			if err != nil {
				c.Check(false, "estimate succeeds: %v", err)
				return
			}
			if stats.M != M {
				c.Check(false, "M is N times source count including target: got %d want %d", stats.M, M)
				return
			}
			estWeightSum += stats.SumWeight
			estPosum += float64(stats.Positive)
			if stats.Positive > 0 && stats.Selected.Instance == 1 {
				estInst2Selected++
			}
		}
		meanRef := refWeightSum / trials
		meanEst := estWeightSum / trials
		c.Check(closeTo(meanEst, meanRef, 0.10*math.Max(1, meanRef)),
			"mean S matches independent per-sample sum: est %v ref %v", meanEst, meanRef)
		c.Check(math.Abs(estPosum/trials-refPosum/trials) <= 0.5,
			"mean positive weight count close: est %v ref %v", estPosum/trials, refPosum/trials)
		c.Check(estPosum > 0, "some proposals land on current surfaces")

		// Every source must be able to reach the same y: confirm at least one
		// candidate has positive density from both old scenes.
		both := 0
		bothRnd := rand.New(rand.NewSource(31337))
		for i := 0; i < 3000 && both == 0; i++ {
			sc := old1
			surf, _ := sc.Surface(src1.Surface)
			dir := cosineHemisphere(surf.Normal, bothRnd)
			hit, _, ref := sc.HitRef(primitive.Ray{Origin: surf.Point, Direction: dir}, tMin, math.MaxFloat64)
			if !hit {
				continue
			}
			_, e := current.Surface(SurfaceRef{Instance: ref.Instance, LocalNormal: ref.LocalNormal})
			if e != nil {
				continue
			}
			yRef := SurfaceRef{Instance: ref.Instance, LocalNormal: ref.LocalNormal}
			if densityForSource(current, old1, src1.Surface, yRef, tMin) > 0 &&
				densityForSource(current, old2, src2.Surface, yRef, tMin) > 0 {
				both++
			}
		}
		c.Check(both > 0, "two sources can reach a common surface point")

		// qmix of the selected point must include sources that did not emit
		// that candidate. Check for every run with a selection.
		selRnd := rand.New(rand.NewSource(555))
		selChecked := 0
		for i := 0; i < 200 && selChecked < 20; i++ {
			_, stats, err := EstimateReconnect(current, target, proposals, 4, selRnd)
			if err != nil {
				c.Check(false, "estimate succeeds: %v", err)
				return
			}
			if stats.Positive == 0 {
				continue
			}
			sel, e := current.Surface(stats.Selected)
			c.Check(e == nil, "selected ref resolves")
			q := 0.0
			for j, sc := range sources {
				q += densityForSource(current, sc, srcRefs[j], sel.Ref, tMin)
			}
			q /= float64(len(sources))
			c.Check(closeTo(stats.QMix, q, 1e-9),
				"reported qmix is equal average over all sources: got %v want %v", stats.QMix, q)
			c.Check(stats.ContinuationCalls <= 1, "surface continuation at most once")
			selChecked++
		}
		c.Check(selChecked > 0, "at least one run made a weighted selection")

		// --- blocked source contributes zero density ------------------------
		// Source point at world (0,0,2), target point y at world (0,0,1).
		// Both scenes share IDs 2 and 3, so the identity maps cleanly.
		baseProto := []SpherePrototype{{Center: primitive.Vector{}, Radius: 1, Color: lambert}}
		openScene := mustScene(t, baseProto, []InstanceDesc{
			{ID: 2, Prototype: 0, Matrix: identity(1, 0, 0, 3)},
			{ID: 3, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
		})
		curBlock := mustScene(t, baseProto, []InstanceDesc{
			{ID: 3, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
		})
		srcBlockRef := SurfaceRef{Instance: 2, LocalNormal: primitive.Vector{Z: -1}}
		yBlock := SurfaceRef{Instance: 3, LocalNormal: primitive.Vector{Z: 1}}
		unblocked := densityForSource(curBlock, openScene, srcBlockRef, yBlock, tMin)
		c.Check(unblocked > 0, "unblocked source density positive: %v", unblocked)

		blockerProto := []SpherePrototype{
			{Center: primitive.Vector{}, Radius: 1, Color: lambert},
			{Center: primitive.Vector{}, Radius: 0.2, Color: lambert},
		}
		blockedScene := mustScene(t, blockerProto, []InstanceDesc{
			{ID: 2, Prototype: 0, Matrix: identity(1, 0, 0, 3)},
			{ID: 3, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
			{ID: 9, Prototype: 1, Matrix: identity(1, 0, 0, 1.5)},
		})
		blocked := densityForSource(curBlock, blockedScene, srcBlockRef, yBlock, tMin)
		c.Check(blocked == 0, "blocked source density is zero: %v", blocked)
		// The same blocked source must also produce no qmix contribution in a
		// normal estimate.
		_, bstats, err := EstimateReconnect(curBlock, SurfaceRef{Instance: 3, LocalNormal: primitive.Vector{Z: -1}},
			[]ProposalSource{Source{Scene: blockedScene, Surface: srcBlockRef}}, 4, rand.New(rand.NewSource(21)))
		c.Check(err == nil, "estimate with blocked source runs: %v", err)
		c.Check(bstats.M == 8, "M includes the blocked source: %d", bstats.M)

		// --- ID disappears from the current frame ---------------------------
		goneScene := mustScene(t, protos, []InstanceDesc{
			{ID: 0, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
		})
		rndGone := rand.New(rand.NewSource(99))
		_, goneStats, err := EstimateReconnect(goneScene, target, []ProposalSource{src1}, 6, rndGone)
		c.Check(err == nil, "missing-ID estimate still runs: %v", err)
		c.Check(goneStats.M == 12, "M counts candidates from both sources even when IDs vanish: %d", goneStats.M)
		if goneStats.Positive > 0 {
			c.Check(goneScene.HasInstance(goneStats.Selected.Instance), "selected instance exists")
		}

		// --- all candidates miss --------------------------------------------
		farProto := []SpherePrototype{{Center: primitive.Vector{}, Radius: 1, Color: lambert}}
		far := mustScene(t, farProto, []InstanceDesc{{ID: 1, Prototype: 0, Matrix: identity(0.05, 0, 0, 0)}})
		farTarget := SurfaceRef{Instance: 1, LocalNormal: primitive.Vector{Z: 1}}
		emptyOld := mustScene(t, farProto, []InstanceDesc{{ID: 1, Prototype: 0, Matrix: identity(0.05, 0, 0, 0)}})
		_, missStats, err := EstimateReconnect(far, farTarget,
			[]ProposalSource{Source{Scene: emptyOld, Surface: farTarget}}, 5,
			rand.New(rand.NewSource(7)))
		c.Check(err == nil, "all-miss estimate runs")
		c.Check(missStats.M == 10, "M fixed at N times sources even when all miss: %d", missStats.M)
		c.Check(missStats.Positive == 0 && missStats.SumWeight == 0, "no positive weights when all candidates miss")
		c.Check(missStats.ContinuationCalls == 0, "no continuation when pool empty")

		// --- proto reuse across instances -----------------------------------
		reuse := mustScene(t, farProto, []InstanceDesc{
			{ID: 21, Prototype: 0, Matrix: identity(0.5, 0, 0, 0)},
			{ID: 22, Prototype: 0, Matrix: identity(0.5, 0, 0, 2.0)},
		})
		c.Check(reuse.Count() == 2, "two instances share one prototype")
		refA := SurfaceRef{Instance: 21, LocalNormal: primitive.Vector{Z: 1}}
		refB := SurfaceRef{Instance: 22, LocalNormal: primitive.Vector{Z: 1}}
		sa, _ := reuse.Surface(refA)
		sb, _ := reuse.Surface(refB)
		c.Check(closeTo(sa.Point.Z, 0.5, 1e-12) && closeTo(sb.Point.Z, 2.5, 1e-12),
			"shared prototype maps to distinct world points")
		c.Check(sa.Color == sb.Color, "shared prototype shares color")
	})
}

// ---------------------------------------------------------------------------
// TestReconnectTransport
// ---------------------------------------------------------------------------

// refColor is an independent, direct implementation of the existing path
// tracer used only by the tests as a reference for the current-frame
// continuation term.
func refColor(ray primitive.Ray, sc *InstanceScene, rnd *rand.Rand, depth int) primitive.Color {
	hit, record := sc.Hit(ray, tMin, math.MaxFloat64)
	if !hit {
		return primitive.Gradient(primitive.White, primitive.Blue, ray.Direction.Normalize().Y)
	}
	if depth >= 50 {
		return primitive.Black
	}
	bounced, bouncedRay := record.Bounce(ray, record, rnd)
	if !bounced {
		return primitive.Black
	}
	return record.Material.Color().Multiply(refColor(bouncedRay, sc, rnd, depth+1))
}

func TestReconnectTransport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		protos := []SpherePrototype{
			{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.55, G: 0.35, B: 0.2}},
			{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.2, G: 0.5, B: 0.4}},
			{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.4, G: 0.3, B: 0.7}},
		}
		// Target at origin. Candidate sphere 1 sits in +Z; sphere 2 sits to the
		// side so it changes the continuation after the first bounce.
		scene := mustScene(t, protos, []InstanceDesc{
			{ID: 0, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
			{ID: 1, Prototype: 1, Matrix: identity(0.9, 0.0, 0.0, 2.2)},
			{ID: 2, Prototype: 2, Matrix: identity(0.7, 1.4, 0.5, 1.4)},
		})
		target := SurfaceRef{Instance: 0, LocalNormal: primitive.Vector{Z: 1}}
		targetSurf, _ := scene.Surface(target)

		// The proposal source is a previous pose with the same IDs, moving
		// both the candidate sphere and the side sphere.
		oldPose := mustScene(t, protos, []InstanceDesc{
			{ID: 0, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
			{ID: 1, Prototype: 1, Matrix: identity(0.9, 0.0, 0.0, 2.2)},
			{ID: 2, Prototype: 2, Matrix: identity(0.7, 1.9, 0.8, 1.1)},
		})
		oldRef := SurfaceRef{Instance: 1, LocalNormal: primitive.Vector{X: 0.1, Y: -0.2, Z: -1}.Normalize()}
		proposals := []ProposalSource{Source{Scene: oldPose, Surface: oldRef}}

		// Independent current-direction integral: cosine-weighted rays from
		// the target, each traced with the reference renderer, times the
		// target albedo and the estimator's fixed S/M normalization contract.
		// We compare the true continuation mean directly against a large
		// reference integration of the same quantity.
		refRnd := rand.New(rand.NewSource(1234))
		const refSamples = 300000
		var refSum, refSumSq [3]float64
		for i := 0; i < refSamples; i++ {
			dir := cosineHemisphere(targetSurf.Normal, refRnd)
			val := targetSurf.Color.Multiply(refColor(primitive.Ray{Origin: targetSurf.Point, Direction: dir}, scene, refRnd, 1))
			for k, v := range []float64{val.R, val.G, val.B} {
				refSum[k] += v
				refSumSq[k] += v * v
			}
		}

		estRnd := rand.New(rand.NewSource(5678))
		const batches = 300
		const perBatch = 6
		var batchMeans [3][2]float64 // mean, sq
		for b := 0; b < batches; b++ {
			var accum [3]float64
			for s := 0; s < perBatch; s++ {
				rgb, _, err := EstimateReconnect(scene, target, proposals, 4, estRnd)
				if err != nil {
					c.Check(false, "estimate: %v", err)
					return
				}
				accum[0] += rgb.R
				accum[1] += rgb.G
				accum[2] += rgb.B
			}
			for k := 0; k < 3; k++ {
				m := accum[k] / perBatch
				batchMeans[k][0] += m
				batchMeans[k][1] += m * m
			}
		}

		estMean := [3]float64{}
		estSE := [3]float64{}
		for k := 0; k < 3; k++ {
			estMean[k] = batchMeans[k][0] / batches
			v := batchMeans[k][1]/batches - estMean[k]*estMean[k]
			if v < 0 {
				v = 0
			}
			estSE[k] = math.Sqrt(v / batches)
		}
		refMean := [3]float64{}
		refSE := [3]float64{}
		for k := 0; k < 3; k++ {
			refMean[k] = refSum[k] / refSamples
			v := refSumSq[k]/refSamples - refMean[k]*refMean[k]
			if v < 0 {
				v = 0
			}
			refSE[k] = math.Sqrt(v / refSamples)
		}
		if testing.Verbose() {
			t.Logf("transport R: est %.5f +- %.5f, ref %.5f +- %.5f (batches=%d, batchSamples=%d, refSamples=%d)",
				estMean[0], estSE[0], refMean[0], refSE[0], batches, perBatch, refSamples)
			t.Logf("transport G: est %.5f +- %.5f, ref %.5f +- %.5f", estMean[1], estSE[1], refMean[1], refSE[1])
			t.Logf("transport B: est %.5f +- %.5f, ref %.5f +- %.5f", estMean[2], estSE[2], refMean[2], refSE[2])
		}
		for k, name := range []string{"R", "G", "B"} {
			diff := math.Abs(estMean[k] - refMean[k])
			allowed := 4*math.Sqrt(estSE[k]*estSE[k]+refSE[k]*refSE[k]) + 0.005
			c.Check(diff <= allowed,
				"transport %s within confidence interval: diff %v allowed %v", name, diff, allowed)
		}

		// --- target occlusion ------------------------------------------------
		// A dedicated pose pair: the target sphere sits at the origin facing
		// +Z, the candidate sphere is farther along +Z, and an occluder sits
		// between them. Candidates sampled from the old target source land on
		// the candidate sphere, are invisible from the target, and must not
		// trigger a surface continuation.
		occProtos := []SpherePrototype{
			{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.5, G: 0.5, B: 0.5}},
			{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.3, G: 0.6, B: 0.2}},
			{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.6, G: 0.3, B: 0.3}},
		}
		targetOrigin := SurfaceRef{Instance: 0, LocalNormal: primitive.Vector{Z: 1}}
		occCurrent := mustScene(t, occProtos, []InstanceDesc{
			{ID: 0, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
			{ID: 1, Prototype: 1, Matrix: identity(0.9, 0.0, 0.0, 3.0)},
			{ID: 5, Prototype: 2, Matrix: identity(0.3, 0.0, 0.0, 1.55)},
		})
		occOld := mustScene(t, occProtos, []InstanceDesc{
			{ID: 0, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
			{ID: 1, Prototype: 1, Matrix: identity(0.9, 0.0, 0.0, 3.0)},
		})
		occProps := []ProposalSource{Source{Scene: occOld, Surface: targetOrigin}}
		occRnd := rand.New(rand.NewSource(4321))
		sawOccluded := false
		positives := 0
		for i := 0; i < 400; i++ {
			_, stats, err := EstimateReconnect(occCurrent, targetOrigin, occProps, 6, occRnd)
			if err != nil {
				c.Check(false, "occluded estimate: %v", err)
				return
			}
			if stats.Positive > 0 {
				positives++
			}
			if stats.Positive > 0 && !stats.Visible {
				sawOccluded = true
				c.Check(stats.ContinuationCalls == 0, "no continuation when target-side visibility fails")
			}
		}
		c.Check(positives > 0, "occlusion scene produced positive-weight proposals")
		c.Check(sawOccluded, "target occlusion produced a positive-weight but invisible selection")

		// A colonnade-free variant sees the same candidate sphere, so at least
		// one visible selection must occur without the occluder.
		openCurrent := mustScene(t, occProtos, []InstanceDesc{
			{ID: 0, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
			{ID: 1, Prototype: 1, Matrix: identity(0.9, 0.0, 0.0, 3.0)},
		})
		openRnd := rand.New(rand.NewSource(24680))
		sawVisible := false
		for i := 0; i < 200 && !sawVisible; i++ {
			_, stats, err := EstimateReconnect(openCurrent, targetOrigin, occProps, 6, openRnd)
			if err != nil {
				c.Check(false, "open estimate: %v", err)
				return
			}
			if stats.Positive > 0 && stats.Visible {
				sawVisible = true
				c.Check(stats.ContinuationCalls <= 1, "visible selection continues at most once")
			}
		}
		c.Check(sawVisible, "unoccluded scene produces a visible selection")

		// --- direct sky term is independently estimated ----------------------
		// An isolated small target with no other geometry must produce exactly
		// target color times the mean sky gradient, with no surface term.
		skyProto := []SpherePrototype{{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.6, G: 0.5, B: 0.4}}}
		isolated := mustScene(t, skyProto, []InstanceDesc{{ID: 0, Prototype: 0, Matrix: identity(0.5, 0, 0, 0)}})
		isoTarget := SurfaceRef{Instance: 0, LocalNormal: primitive.Vector{Z: 1}}
		isoSurf, _ := isolated.Surface(isoTarget)
		skyRnd := rand.New(rand.NewSource(2468))
		const skySamples = 120000
		var skySum [3]float64
		for i := 0; i < skySamples; i++ {
			rgb, stats, err := EstimateReconnect(isolated, isoTarget, nil, 3, skyRnd)
			if err != nil {
				c.Check(false, "sky estimate: %v", err)
				return
			}
			if stats.Positive == 0 && stats.ContinuationCalls != 0 {
				c.Check(false, "continuation without a selection")
				return
			}
			skySum[0] += rgb.R
			skySum[1] += rgb.G
			skySum[2] += rgb.B
		}
		// Reference: target color times an independent cosine integral of the
		// Gradient sky.
		refSkyRnd := rand.New(rand.NewSource(8642))
		var refSky [3]float64
		for i := 0; i < skySamples; i++ {
			dir := cosineHemisphere(isoSurf.Normal, refSkyRnd)
			g := primitive.Gradient(primitive.White, primitive.Blue, dir.Y)
			refSky[0] += isoSky(isoSurf.Color.R, g.R)
			refSky[1] += isoSky(isoSurf.Color.G, g.G)
			refSky[2] += isoSky(isoSurf.Color.B, g.B)
		}
		for k, name := range []string{"R", "G", "B"} {
			est := skySum[k] / skySamples
			ref := refSky[k] / skySamples
			c.Check(math.Abs(est-ref) <= 0.006,
				"direct sky %s matches independent integral: est %v ref %v", name, est, ref)
		}

		// --- cross-worker determinism ---------------------------------------
		camera := primitive.NewCamera(primitive.Vector{X: 0.3, Y: 1.1, Z: 4.6}, primitive.Vector{X: 0.1, Y: 0.2, Z: 0}, 50, 1.5, 0)
		base, err := RenderReconnect(scene, camera, proposals, 24, 16, 3, 4, 1, 31415)
		c.Check(err == nil, "render reconnect: %v", err)
		glance, err := RenderReconnect(scene, camera, proposals, 24, 16, 3, 4, 4, 31415)
		c.Check(err == nil, "render reconnect multi-worker: %v", err)
		another, err := RenderReconnect(scene, camera, proposals, 24, 16, 3, 4, 7, 31415)
		c.Check(err == nil, "render reconnect seven workers: %v", err)
		c.Check(framesEqual(base, glance), "identical frame for 1 vs 4 workers")
		c.Check(framesEqual(base, another), "identical frame for 1 vs 7 workers")

		// Frame layout is row-major with height rows and width columns.
		c.Check(len(base) == 16, "frame has height rows: %d", len(base))
		c.Check(len(base[0]) == 24, "each row has width pixels: %d", len(base[0]))
		nonZero := 0
		for y := range base {
			for x := range base[y] {
				if base[y][x][0]+base[y][x][1]+base[y][x][2] > 0 {
					nonZero++
				}
			}
		}
		c.Check(nonZero > 0, "rendered frame is not black")

		// A different seed produces a different image, but the same seed is
		// reproducible.
		otherSeed, err := RenderReconnect(scene, camera, proposals, 24, 16, 3, 4, 2, 27182)
		c.Check(err == nil, "render reconnect other seed: %v", err)
		c.Check(!framesEqual(base, otherSeed), "different seed yields a different frame")
		repeat, err := RenderReconnect(scene, camera, proposals, 24, 16, 3, 4, 2, 31415)
		c.Check(err == nil, "render reconnect repeat: %v", err)
		c.Check(framesEqual(base, repeat), "same seed and workers reproduce the frame")

		// --- old scene still queries while the new one is in use -------------
		oldQueryOK := true
		for i := 0; i < 100; i++ {
			if _, err := oldPose.Surface(oldRef); err != nil {
				oldQueryOK = false
			}
			hit, _ := oldPose.Hit(primitive.Ray{Origin: primitive.Vector{Z: 5}, Direction: primitive.Vector{Z: -1}}, tMin, math.MaxFloat64)
			if !hit {
				oldQueryOK = false
			}
		}
		c.Check(oldQueryOK, "old scene remains queryable during current-frame estimates")

		// --- cross-pose scaling ---------------------------------------------
		// Same target with the candidate sphere scaled non-uniformly between
		// poses; the estimator must remain finite and correctly normalized
		// against the current-frame integral.
		scaledScene := mustScene(t, protos, []InstanceDesc{
			{ID: 0, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
			{ID: 1, Prototype: 1, Matrix: identity(0.9, 0.0, 0.0, 2.2)},
			{ID: 2, Prototype: 2, Matrix: identity(0.7, 1.4, 0.5, 1.4)},
		})
		scaledOld := mustScene(t, protos, []InstanceDesc{
			{ID: 0, Prototype: 0, Matrix: identity(1, 0, 0, 0)},
			{ID: 1, Prototype: 1, Matrix: [3][4]float64{{1.5, 0.3, 0, 0.0}, {0, 0.5, 0, 0.0}, {0.1, 0, 0.8, 2.2}}},
			{ID: 2, Prototype: 2, Matrix: identity(0.7, 1.9, 0.8, 1.1)},
		})
		scaledProps := []ProposalSource{Source{Scene: scaledOld, Surface: oldRef}}
		scRnd := rand.New(rand.NewSource(13579))
		scSum := [3]float64{}
		const scSamples = 40000
		for i := 0; i < scSamples; i++ {
			rgb, _, err := EstimateReconnect(scaledScene, target, scaledProps, 4, scRnd)
			if err != nil {
				c.Check(false, "scaled estimate: %v", err)
				return
			}
			scSum[0] += rgb.R
			scSum[1] += rgb.G
			scSum[2] += rgb.B
		}
		scRefRnd := rand.New(rand.NewSource(97531))
		var scRef [3]float64
		for i := 0; i < scSamples; i++ {
			dir := cosineHemisphere(targetSurf.Normal, scRefRnd)
			val := targetSurf.Color.Multiply(refColor(primitive.Ray{Origin: targetSurf.Point, Direction: dir}, scaledScene, scRefRnd, 1))
			scRef[0] += val.R
			scRef[1] += val.G
			scRef[2] += val.B
		}
		for k, name := range []string{"R", "G", "B"} {
			est := scSum[k] / scSamples
			ref := scRef[k] / scSamples
			c.Check(math.Abs(est-ref) <= 0.02+0.1*math.Abs(ref),
				"cross-pose scaled %s close: est %v ref %v", name, est, ref)
		}
	})
}

func isoSky(albedo, sky float64) float64 { return albedo * sky }

func framesEqual(a, b [][][3]float64) bool {
	if len(a) != len(b) {
		return false
	}
	for y := range a {
		if len(a[y]) != len(b[y]) {
			return false
		}
		for x := range a[y] {
			for k := 0; k < 3; k++ {
				if a[y][x][k] != b[y][x][k] {
					return false
				}
			}
		}
	}
	return true
}
