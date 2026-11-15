package pt

import (
	"math"
	"math/rand"
	"os"
	"strconv"
	"strings"
	"sync"
	"testing"

	"github.com/fogleman/pt/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// independent brute-force references (exhaustive filtering, no k-d tree)
// ---------------------------------------------------------------------------

// bruteVisibleHit intersects every visible shape directly, mirroring the
// visibility rule that Scene.Intersect applies through the k-d tree.
func bruteVisibleHit(scene *Scene, r Ray) Hit {
	hit := NoHit
	for _, shape := range scene.Shapes {
		if inst, ok := shape.(*LinkedInstance); ok && !inst.links.Visible {
			continue
		}
		h := shape.Intersect(r)
		if h.T < hit.T {
			hit = h
		}
	}
	return hit
}

// bruteOccluded exhaustively tests every shape that may cast a shadow toward
// the target light, independently of the acceleration structure.
func bruteOccluded(scene *Scene, r Ray, maxDistance float64, light Shape) bool {
	mask := emitMask(light)
	for _, shape := range scene.Shapes {
		if shape == light {
			continue
		}
		if inst, ok := shape.(*LinkedInstance); ok && inst.links.CastMask&mask == 0 {
			continue
		}
		h := shape.Intersect(r)
		if h.Ok() && h.T > EPS && h.T < maxDistance-EPS {
			return true
		}
	}
	return false
}

// oldCoverage reproduces the pre-existing hemisphere-coverage formula using
// only a bounding-sphere center and radius, so direct-light numbers can be
// checked against an independent calculation.
func oldCoverage(center, receiver Vector, radius float64) float64 {
	hyp := center.Sub(receiver).Length()
	opp := radius
	theta := math.Asin(opp / hyp)
	adj := opp / math.Tan(theta)
	d := math.Cos(theta) * adj
	r := math.Sin(theta) * adj
	coverage := (r * r) / (d * d)
	if hyp < opp {
		coverage = 1
	}
	return math.Min(coverage, 1)
}

// linkedLightSphere returns the bounding sphere used when sampling a linked
// light, matching the TransformedShape branch of sampleLight.
func linkedLightSphere(light *LinkedInstance) (Vector, float64) {
	box := light.BoundingBox()
	return box.Center(), box.OuterRadius()
}

func approx(c *taskcheck.C, got, want float64, message string, args ...interface{}) {
	c.Check(math.Abs(got-want) <= 1e-9+1e-7*math.Max(1, math.Abs(want)), message, args...)
}

func approxVector(c *taskcheck.C, got, want Vector, message string, args ...interface{}) {
	approx(c, got.X, want.X, message+" (X)", args...)
	approx(c, got.Y, want.Y, message+" (Y)", args...)
	approx(c, got.Z, want.Z, message+" (Z)", args...)
}

func approxColor(c *taskcheck.C, got, want Color, message string, args ...interface{}) {
	approx(c, got.R, want.R, message+" (R)", args...)
	approx(c, got.G, want.G, message+" (G)", args...)
	approx(c, got.B, want.B, message+" (B)", args...)
}

func nearColor(c *taskcheck.C, got, want Color, tolerance float64, message string, args ...interface{}) {
	c.Check(math.Abs(got.R-want.R) <= tolerance && math.Abs(got.G-want.G) <= tolerance &&
		math.Abs(got.B-want.B) <= tolerance, message, args...)
}

// ---------------------------------------------------------------------------
// TestLinkedInstances: identities, shared geometry, normals, visibility
// ---------------------------------------------------------------------------

func TestLinkedInstances(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// default links: every bit set, visible
		def := DefaultInstanceLinks()
		c.Check(def.ReceiveMask == ^uint64(0) && def.EmitMask == ^uint64(0) &&
			def.CastMask == ^uint64(0) && def.Visible, "default links should be all-bits visible")

		// explicit zero value is interpreted as zero
		zero := InstanceLinks{}
		c.Check(zero.ReceiveMask == 0 && zero.EmitMask == 0 && zero.CastMask == 0 && !zero.Visible,
			"zero InstanceLinks must be zero masks and invisible")

		// nested LinkedInstances are rejected
		sphereSource := NewSphere(Vector{}, 1, DiffuseMaterial(White))
		inner := NewLinkedInstance(sphereSource, Identity(), def)
		func() {
			defer func() { c.Check(recover() != nil, "nesting a LinkedInstance must panic") }()
			NewLinkedInstance(inner, Identity(), def)
		}()

		// two instances share one sphere source but keep distinct identities
		ma := Translate(Vector{0, 0, 4})
		mb := Translate(Vector{3, 0, 4})
		instA := NewLinkedInstance(sphereSource, ma, def)
		instB := NewLinkedInstance(sphereSource, mb, def)
		c.Check(instA.Source() == sphereSource && instB.Source() == sphereSource,
			"both instances must expose the shared source")
		c.Check(instA != instB, "instances must have distinct identities")

		scene := &Scene{}
		scene.Add(instA)
		scene.Add(instB)
		scene.Compile()
		rayA := Ray{Vector{}, Vector{0, 0, 1}}
		hitA := scene.Intersect(rayA)
		c.Check(hitA.Ok(), "ray toward instance A must hit")
		c.Check(hitA.Shape == sphereSource, "Hit.Shape must be the original leaf sphere, got %T", hitA.Shape)
		c.Check(hitA.HitInfo.Instance == instA, "HitInfo.Instance must identify instance A")
		approx(c, hitA.T, 3, "instance A hit distance")
		approxVector(c, hitA.HitInfo.Position, Vector{0, 0, 3}, "instance A hit position")
		approxVector(c, hitA.HitInfo.Normal, Vector{0, 0, -1}, "instance A world normal")

		rayB := Ray{Vector{3, 0, 0}, Vector{0, 0, 1}}
		hitB := scene.Intersect(rayB)
		c.Check(hitB.Ok() && hitB.HitInfo.Instance == instB, "ray toward instance B must identify B")
		approxVector(c, hitB.HitInfo.Position, Vector{3, 0, 3}, "instance B hit position")

		// an unwrapped shape hit reports a nil Instance
		plain := NewSphere(Vector{-3, 0, 1}, 1, DiffuseMaterial(White))
		scene.Add(plain)
		scene.Compile()
		plainRay := Ray{Vector{-3, 0, 0}, Vector{0, 0, 1}}
		plainHit := scene.Intersect(plainRay)
		plainInfo := plainHit.Info(plainRay)
		c.Check(plainHit.Ok() && plainInfo.Instance == nil, "unwrapped hit must have nil Instance")
		c.Check(plainHit.Shape == plain, "unwrapped Hit.Shape is the leaf itself")

		// geometric equivalence with a plain TransformedShape, including a
		// non-commutative affine (shear * non-uniform scale * rotation)
		shear := Matrix{1, 0.3, 0, 0, 0, 1, 0, 0, 0.1, 0, 1, 0, 0, 0, 0, 1}
		affine := shear.Mul(Scale(Vector{1.5, 0.7, 2.0})).Mul(Rotate(Vector{0, 0, 1}, 0.6))
		affine = affine.Translate(Vector{2, -1, 3})
		linked := NewLinkedInstance(sphereSource, affine, def)
		transformed := NewTransformedShape(sphereSource, affine)
		probe := Ray{Vector{4, -2, 1}, Vector{-0.3, 0.5, 0.8}.Normalize()}
		hl := linked.Intersect(probe)
		ht := transformed.Intersect(probe)
		c.Check(hl.Ok() && ht.Ok(), "affine-linked sphere must intersect")
		approx(c, hl.T, ht.T, "linked and transformed hit distance must match")
		approxVector(c, hl.HitInfo.Position, ht.HitInfo.Position, "linked/transformed position")
		approxVector(c, hl.HitInfo.Normal, ht.HitInfo.Normal, "linked/transformed normal")

		// a shared triangle mesh under a non-commutative affine: leaf shape,
		// instance identity and independently computed world normal
		tri := NewTriangle(Vector{0, 0, 0}, Vector{1, 0, 0}, Vector{0, 1, 0},
			Vector{}, Vector{}, Vector{}, DiffuseMaterial(White))
		mesh := NewMesh([]*Triangle{tri})
		meshM := shear.Mul(Scale(Vector{0.8, 1.3, 2.0})).Mul(Rotate(Vector{0, 1, 0}, 0.4))
		meshM = meshM.Translate(Vector{1, 2, 5})
		meshA := NewLinkedInstance(mesh, meshM, def)
		meshB := NewLinkedInstance(mesh, meshM.Translate(Vector{4, 0, 0}), def)
		meshScene := &Scene{}
		meshScene.Add(meshA)
		meshScene.Add(meshB)
		meshScene.Compile()
		localPoint := Vector{1.0 / 3, 1.0 / 3, 0}
		worldPoint := meshM.MulPosition(localPoint)
		wantNormal := meshM.Inverse().Transpose().MulDirection(Vector{0, 0, 1})
		meshRay := Ray{worldPoint.Add(wantNormal.MulScalar(2)), wantNormal.Negate()}
		meshHit := meshScene.Intersect(meshRay)
		c.Check(meshHit.Ok(), "mesh instance must be hit")
		c.Check(meshHit.Shape == tri, "mesh hit Shape must be the original triangle, got %T", meshHit.Shape)
		c.Check(meshHit.HitInfo.Instance == meshA, "mesh hit must identify the instance")
		approxVector(c, meshHit.HitInfo.Normal, wantNormal, "inverse-transpose world normal")
		c.Check(meshHit.HitInfo.Instance != meshB, "the other instance must not be reported")

		// TransformedShape source: the inner transform is honored and the
		// reported leaf is still the original geometry
		innerTS := NewTransformedShape(sphereSource, Scale(Vector{1, 2, 1}))
		tsInstance := NewLinkedInstance(innerTS, Translate(Vector{0, 0, 6}), def)
		c.Check(tsInstance.Source() == innerTS, "TransformedShape source is exposed verbatim")
		equiv := NewTransformedShape(innerTS, Translate(Vector{0, 0, 6}))
		tsRay := Ray{Vector{}, Vector{0, 0, 1}}
		hitLinked := tsInstance.Intersect(tsRay)
		hitEquiv := equiv.Intersect(tsRay)
		c.Check(hitLinked.Ok() && hitEquiv.Ok(), "TransformedShape-source instance must hit")
		c.Check(hitLinked.Shape == sphereSource, "leaf under TransformedShape source must be the sphere")
		approx(c, hitLinked.T, hitEquiv.T, "TransformedShape-source distance matches equivalent shape")
		approxVector(c, hitLinked.HitInfo.Normal, hitEquiv.HitInfo.Normal,
			"TransformedShape-source normal matches equivalent shape")

		// reflection (negative determinant) stays invertible and flips normals
		mirror := NewLinkedInstance(sphereSource, Scale(Vector{-1, 1, 1}).Translate(Vector{0, 0, 4}), def)
		mirrorHit := mirror.Intersect(Ray{Vector{}, Vector{0, 0, 1}})
		c.Check(mirrorHit.Ok(), "reflected instance must intersect")
		approx(c, mirrorHit.HitInfo.Normal.X, 0, "reflected sphere normal x is zero at the front pole")

		// cube and triangle instances work as non-emissive objects
		cube := NewLinkedInstance(
			NewCube(Vector{-1, -1, -1}, Vector{1, 1, 1}, DiffuseMaterial(White)),
			Translate(Vector{0, 0, 3}), def)
		triInstance := NewLinkedInstance(tri, Translate(Vector{0, 0, 3}), def)
		basic := &Scene{}
		basic.Add(cube)
		basic.Add(triInstance)
		basic.Compile()
		basicHit := basic.Intersect(Ray{Vector{0, 0, 0}, Vector{0, 0, 1}})
		c.Check(basicHit.Ok(), "cube/triangle instances must be intersectable")

		// hidden near surface cannot block a visible object behind it
		near := NewLinkedInstance(
			NewSphere(Vector{}, 1, DiffuseMaterial(White)),
			Translate(Vector{0, 0, 2}), InstanceLinks{0, 0, 0, false})
		far := NewLinkedInstance(
			NewSphere(Vector{}, 1, DiffuseMaterial(White)),
			Translate(Vector{0, 0, 6}), def)
		hideScene := &Scene{}
		hideScene.Add(near)
		hideScene.Add(far)
		hideScene.Compile()
		hideRay := Ray{Vector{}, Vector{0, 0, 1}}
		hideHit := hideScene.Intersect(hideRay)
		c.Check(hideHit.Ok() && hideHit.HitInfo.Instance == far,
			"hidden near instance must not occlude the visible far instance")
		refHit := bruteVisibleHit(hideScene, hideRay)
		c.Check(refHit.Ok() && refHit.HitInfo.Instance == far, "brute-force visibility reference agrees")
	})
}

// ---------------------------------------------------------------------------
// TestLinkedLighting: masks, direct light numbers, shadows, random mode
// ---------------------------------------------------------------------------

func TestLinkedLighting(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			bit1 = uint64(1) << 0
			bit2 = uint64(1) << 1
			bit4 = uint64(1) << 2
			bit8 = uint64(1) << 3
		)
		red := Color{1, 0, 0}
		green := Color{0, 1, 0}
		blue := Color{0, 0, 1}

		// three emissive sphere lights, identical sampling geometry but
		// different colors and link bits
		lightPos := Translate(Vector{0, 0, 5})
		light1 := NewLinkedInstance(NewSphere(Vector{}, 1, LightMaterial(red, 1)),
			lightPos, InstanceLinks{bit1, bit1, bit1, true})
		light2 := NewLinkedInstance(NewSphere(Vector{}, 1, LightMaterial(green, 1)),
			lightPos, InstanceLinks{bit2, bit2, bit2, true})
		light3 := NewLinkedInstance(NewSphere(Vector{}, 1, LightMaterial(blue, 1)),
			lightPos, InstanceLinks{bit4, bit4, bit4, true})

		// linked cube receiver whose ReceiveMask can be chosen per scene
		receiverCube := NewCube(Vector{-100, -100, -100}, Vector{100, 100, 0}, DiffuseMaterial(White))
		newLightingScene := func(receiveMask uint64, extra ...Shape) *Scene {
			scene := &Scene{}
			scene.Add(NewLinkedInstance(receiverCube, Identity(),
				InstanceLinks{receiveMask, 0, allLinkBits, true}))
			scene.Add(light1)
			scene.Add(light2)
			scene.Add(light3)
			for _, shape := range extra {
				scene.Add(shape)
			}
			scene.Compile()
			return scene
		}
		sampleRay := Ray{Vector{0, 0, 1}, Vector{0, 0, -1}}
		directSampler := NewDirectSampler()
		directSampler.SoftShadows = false

		// independent per-light expected value from the old coverage formula.
		// The engine measures distances from the receiver surface hit point,
		// not the camera ray origin, so derive both from a real receiver hit.
		allScene := newLightingScene(allLinkBits)
		recvHit := allScene.Intersect(sampleRay)
		recvInfo := recvHit.Info(sampleRay)
		refPoint, refNormal := recvInfo.Position, recvInfo.Normal
		center, radius := linkedLightSphere(light1)
		coverage := oldCoverage(center, refPoint, radius)
		diffuse := refNormal.Dot(center.Sub(refPoint).Normalize()) // cosine toward the light
		perLight := coverage * diffuse                             // emittance == 1
		want := func(col Color) Color { return col.MulScalar(perLight) }

		// LightModeAll: every compatible light accumulates
		gotAll := directSampler.Sample(allScene, sampleRay, rand.New(rand.NewSource(1)))
		approxColor(c, gotAll, want(red).Add(want(green)).Add(want(blue)),
			"All mode sums all three compatible lights")

		onlyRed := newLightingScene(bit1)
		approxColor(c, directSampler.Sample(onlyRed, sampleRay, rand.New(rand.NewSource(2))),
			want(red), "ReceiveMask bit1 selects only the red light")
		onlyGreen := newLightingScene(bit2)
		approxColor(c, directSampler.Sample(onlyGreen, sampleRay, rand.New(rand.NewSource(3))),
			want(green), "ReceiveMask bit2 selects only the green light")
		onlyBlue := newLightingScene(bit4)
		approxColor(c, directSampler.Sample(onlyBlue, sampleRay, rand.New(rand.NewSource(4))),
			want(blue), "ReceiveMask bit4 selects only the blue light")

		// empty compatible set returns black without touching the rng
		noneScene := newLightingScene(bit8)
		fresh := rand.New(rand.NewSource(99))
		noneHit := noneScene.Intersect(sampleRay)
		c.Check(noneHit.Ok(), "receiver must be hit")
		blackDirect := directSampler.sampleLights(noneScene, noneHit.HitInfo, fresh)
		approxColor(c, blackDirect, Black, "incompatible receiver gets black direct light")
		c.Check(fresh.Int63() == rand.New(rand.NewSource(99)).Int63(),
			"empty compatible set must not draw random numbers")

		// EmitMask=0 drops the light from explicit direct lighting only
		silent := NewLinkedInstance(
			NewSphere(Vector{}, 1, LightMaterial(White, 1)), lightPos,
			InstanceLinks{bit1, 0, bit1, true})
		silentScene := &Scene{}
		silentScene.Add(NewLinkedInstance(receiverCube, Identity(),
			InstanceLinks{bit1, 0, allLinkBits, true}))
		silentScene.Add(silent)
		silentScene.Compile()
		approxColor(c, directSampler.Sample(silentScene, sampleRay, rand.New(rand.NewSource(5))),
			Black, "EmitMask=0 light contributes no explicit direct light")
		// ...but camera-visible self-emission is unchanged
		emissionRay := Ray{Vector{0, 0, 1}, Vector{0, 0, 1}}
		emissive := directSampler.Sample(silentScene, emissionRay, rand.New(rand.NewSource(6)))
		c.Check(emissive.R > 0 && emissive.G > 0 && emissive.B > 0,
			"EmitMask=0 must not suppress directly visible emission, got %+v", emissive)

		// a hidden light still illuminates compatible receivers
		hiddenLight := NewLinkedInstance(
			NewSphere(Vector{}, 1, LightMaterial(White, 1)), lightPos,
			InstanceLinks{bit1, bit1, bit1, false})
		hiddenScene := &Scene{}
		hiddenScene.Add(NewLinkedInstance(receiverCube, Identity(),
			InstanceLinks{bit1, 0, allLinkBits, true}))
		hiddenScene.Add(hiddenLight)
		hiddenScene.Compile()
		emissionProbe := hiddenScene.Intersect(emissionRay)
		c.Check(!emissionProbe.Ok(), "hidden light is not hit by camera rays")
		hiddenLit := directSampler.Sample(hiddenScene, sampleRay, rand.New(rand.NewSource(7)))
		hc, hr := linkedLightSphere(hiddenLight)
		hiddenExpected := oldCoverage(hc, refPoint, hr) *
			refNormal.Dot(hc.Sub(refPoint).Normalize())
		approx(c, hiddenLit.R, hiddenExpected,
			"hidden light still provides direct illumination")

		// LightModeRandom: fixed-seed determinism, package-seed independence
		randomSampler := NewDirectSampler()
		randomSampler.SoftShadows = false
		randomSampler.LightMode = LightModeRandom
		r1 := randomSampler.Sample(allScene, sampleRay, rand.New(rand.NewSource(123)))
		r2 := randomSampler.Sample(allScene, sampleRay, rand.New(rand.NewSource(123)))
		approxColor(c, r1, r2, "same rnd seed must give identical Random-mode results")
		rand.Seed(424242) // changing the package-global source must not matter
		r3 := randomSampler.Sample(allScene, sampleRay, rand.New(rand.NewSource(123)))
		approxColor(c, r1, r3, "package rand seed must not affect the passed-in rnd")

		// uniform choice and compensation by the compatible set size only
		const nSamples = 600
		sum := Black
		counts := map[int]int{}
		for i := 0; i < nSamples; i++ {
			col := randomSampler.Sample(allScene, sampleRay, rand.New(rand.NewSource(int64(1000+i))))
			sum = sum.Add(col)
			switch {
			case col.R > 0:
				counts[0]++
			case col.G > 0:
				counts[1]++
			default:
				counts[2]++
			}
		}
		mean := sum.DivScalar(nSamples)
		nearColor(c, mean, gotAll, 0.035, "Random-mode mean converges to the All-mode value")
		for i := 0; i < 3; i++ {
			c.Check(counts[i] > nSamples/3-80 && counts[i] < nSamples/3+80,
				"light %d chosen roughly uniformly, count=%d", i, counts[i])
		}
		// a receiver compatible with one light compensates by one, not three
		for mask, wantCol := range map[uint64]Color{bit1: red, bit2: green, bit4: blue} {
			scene := newLightingScene(mask)
			acc := Black
			for i := 0; i < 120; i++ {
				acc = acc.Add(randomSampler.Sample(scene, sampleRay,
					rand.New(rand.NewSource(int64(7000+i)))))
			}
			approxColor(c, acc.DivScalar(120), want(wantCol),
				"single-compatible-light Random mean must not count incompatible lights")
		}

		// ----- shadow-link scenarios checked against brute force -----
		occluderSource := NewSphere(Vector{}, 0.5, DiffuseMaterial(White))
		shadowRay := Ray{Vector{0, 0, 1}, Vector{0, 0, 1}}
		// distance to the light surface along +z
		lightHit := allScene.intersectShape(shadowRay, light1)
		c.Check(lightHit.Ok(), "phase-1 query must reach the target light itself")
		lightDistance := lightHit.T
		c.Check(math.IsInf(lightDistance, 0) == false && lightDistance > 0, "light distance must be finite")

		// short shadow ray: occluder beyond the light cannot block
		beyond := NewLinkedInstance(occluderSource, Translate(Vector{0, 0, 7}),
			InstanceLinks{allLinkBits, 0, allLinkBits, true})
		shortScene := newLightingScene(allLinkBits, beyond)
		c.Check(!shortScene.OccludedForLight(shadowRay, lightDistance, light1),
			"occluder past the light must not block")
		c.Check(!bruteOccluded(shortScene, shadowRay, lightDistance, light1),
			"brute force agrees on the short shadow ray")
		c.Check(!shortScene.OccludedForLight(shadowRay, 2*EPS, light1) &&
			!shortScene.OccludedForLight(shadowRay, math.Inf(1), light1),
			"invalid maxDistance must report unoccluded")

		// hidden occluder that casts a shadow for light1 only
		hiddenCaster := NewLinkedInstance(occluderSource, Translate(Vector{0, 0, 2.5}),
			InstanceLinks{0, 0, bit1, false})
		castScene := newLightingScene(allLinkBits, hiddenCaster)
		c.Check(castScene.OccludedForLight(shadowRay, lightDistance, light1),
			"hidden CastMask bit1 occluder blocks the bit1 light")
		c.Check(!castScene.OccludedForLight(shadowRay, lightDistance, light2),
			"the same hidden occluder must not block the bit2 light")
		c.Check(bruteOccluded(castScene, shadowRay, lightDistance, light1) &&
			!bruteOccluded(castScene, shadowRay, lightDistance, light2),
			"brute force agrees on per-light shadow links")
		// camera rays still ignore the hidden caster
		casterProbe := castScene.Intersect(shadowRay)
		c.Check(!casterProbe.Ok() || casterProbe.HitInfo.Instance != hiddenCaster,
			"hidden caster stays invisible to camera rays")

		// near incompatible occluder skipped, farther compatible one found
		nearBlocked := NewLinkedInstance(occluderSource, Translate(Vector{0, 0, 2}),
			InstanceLinks{0, 0, bit8, true})
		farBlocked := NewLinkedInstance(occluderSource, Translate(Vector{0, 0, 3.2}),
			InstanceLinks{0, 0, bit1, true})
		twoScene := newLightingScene(allLinkBits, nearBlocked, farBlocked)
		c.Check(twoScene.OccludedForLight(shadowRay, lightDistance, light1),
			"after filtering the near occluder the far one must still block")
		c.Check(bruteOccluded(twoScene, shadowRay, lightDistance, light1),
			"brute force agrees on the filtered-near/found-far case")

		// two lights from the same source: the other instance is not ignored
		sharedLightSource := NewSphere(Vector{}, 1, LightMaterial(White, 1))
		sameA := NewLinkedInstance(sharedLightSource, Translate(Vector{0, 0, 5}),
			InstanceLinks{bit1, bit1, bit1, true})
		sameB := NewLinkedInstance(sharedLightSource, Translate(Vector{0, 0, 2.5}),
			InstanceLinks{bit2, bit2, allLinkBits, true})
		sameScene := &Scene{}
		sameScene.Add(NewLinkedInstance(receiverCube, Identity(),
			InstanceLinks{allLinkBits, 0, allLinkBits, true}))
		sameScene.Add(sameA)
		sameScene.Add(sameB)
		sameScene.Compile()
		distA := sameScene.intersectShape(shadowRay, sameA).T
		c.Check(sameScene.OccludedForLight(shadowRay, distA, sameA),
			"a different instance of the same source must still occlude")
		c.Check(bruteOccluded(sameScene, shadowRay, distA, sameA),
			"brute force distinguishes same-source instances")

		// target self-exclusion: with no other shape present, the target
		// light must never count as its own occluder, even past its surface
		selfScene := &Scene{}
		selfScene.Add(NewLinkedInstance(receiverCube, Identity(),
			InstanceLinks{allLinkBits, 0, allLinkBits, true}))
		selfScene.Add(sameA)
		selfScene.Compile()
		selfDist := selfScene.intersectShape(shadowRay, sameA).T
		c.Check(!selfScene.OccludedForLight(shadowRay, selfDist+10, sameA),
			"the target light itself must never be its own occluder")
		c.Check(!bruteOccluded(selfScene, shadowRay, selfDist+10, sameA),
			"brute force agrees on target self-exclusion")
	})
}

// ---------------------------------------------------------------------------
// TestLinkedUpdates: frame-to-frame changes, repeated compiles, concurrency
// ---------------------------------------------------------------------------

func TestLinkedUpdates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		source := NewSphere(Vector{}, 1, DiffuseMaterial(White))
		// keep the light off the z-axis so +z probe rays and the floor
		// shadow ray never run through the test instances
		light := NewLinkedInstance(
			NewSphere(Vector{}, 1, LightMaterial(White, 1)),
			Translate(Vector{8, 0, 6}), DefaultInstanceLinks())
		instance := NewLinkedInstance(source, Translate(Vector{0, 0, 3}), DefaultInstanceLinks())
		other := NewLinkedInstance(source, Translate(Vector{-4, 0, 3}), DefaultInstanceLinks())
		scene := &Scene{}
		scene.Add(NewPlane(Vector{}, Vector{0, 0, 1}, DiffuseMaterial(White)))
		scene.Add(instance)
		scene.Add(other)
		scene.Add(light)
		scene.Compile()
		nShapes := len(scene.Shapes)

		probe := func(x float64) Ray { return Ray{Vector{x, 0, 0}, Vector{0, 0, 1}} }
		queryRay := Ray{Vector{0, 0, 1}, Vector{0, 0, -1}}
		direct := NewDirectSampler()
		direct.SoftShadows = false

		// frame 1: baseline hit and light list
		hit := scene.Intersect(probe(0))
		c.Check(hit.Ok() && hit.HitInfo.Instance == instance, "initial instance hit")
		c.Check(len(scene.Lights) == 1, "exactly one light after compile")

		// frame 2: move the instance sideways and back; its old bounds vanish
		instance.SetTransform(Translate(Vector{6, 0, 8}))
		scene.Compile()
		c.Check(len(scene.Lights) == 1, "recompile must not duplicate lights")
		staleProbe := scene.Intersect(probe(0))
		c.Check(!staleProbe.Ok() || staleProbe.HitInfo.Instance != instance,
			"the instance's old pose must leave no stale hit on its old axis")
		moved := scene.Intersect(probe(6))
		c.Check(moved.Ok() && moved.HitInfo.Instance == instance,
			"moved instance is hit at its new pose")
		approx(c, moved.T, 7, "moved instance hit distance")
		otherHit := scene.Intersect(probe(-4))
		c.Check(otherHit.Ok() && otherHit.HitInfo.Instance == other,
			"the untouched instance must not change")

		// frame 3: hide the instance, then change its receive mask
		instance.SetLinks(InstanceLinks{0, 0, 0, false})
		scene.Compile()
		hiddenProbe := scene.Intersect(probe(0))
		c.Check(!hiddenProbe.Ok() || hiddenProbe.HitInfo.Instance != instance,
			"hidden instance is skipped by camera rays")
		litBefore := direct.Sample(scene, queryRay, rand.New(rand.NewSource(11)))
		instance.SetLinks(InstanceLinks{allLinkBits, 0, allLinkBits, false})
		scene.Compile()
		litAfter := direct.Sample(scene, queryRay, rand.New(rand.NewSource(11)))
		c.Check(litBefore.R > 0 && litAfter.R > 0,
			"the floor stays lit while the hidden instance's masks change")
		nearColor(c, litAfter, litBefore, 1e-12, "mask-only changes must not alter floor lighting")

		// frame 4: toggle the light's EmitMask without touching geometry
		light.SetLinks(InstanceLinks{allLinkBits, 0, allLinkBits, true})
		scene.Compile()
		approxColor(c, direct.Sample(scene, queryRay, rand.New(rand.NewSource(12))), Black,
			"EmitMask=0 removes explicit direct lighting after recompile")
		light.SetLinks(DefaultInstanceLinks())
		scene.Compile()
		c.Check(direct.Sample(scene, queryRay, rand.New(rand.NewSource(13))).R > 0,
			"restoring EmitMask brings direct lighting back")
		c.Check(len(scene.Shapes) == nShapes && len(scene.Lights) == 1,
			"repeated compiles keep one shape set and one light")

		// frame 5: move it back; repeated and interleaved read-only queries
		instance.SetTransform(Translate(Vector{0, 0, 3}))
		instance.SetLinks(DefaultInstanceLinks())
		scene.Compile()
		var wg sync.WaitGroup
		for g := 0; g < 8; g++ {
			wg.Add(1)
			go func(seed int64) {
				defer wg.Done()
				rnd := rand.New(rand.NewSource(seed))
				for i := 0; i < 200; i++ {
					x := rnd.Float64()*6 - 3
					h := scene.Intersect(probe(x))
					if h.Ok() {
						_ = h.Info(probe(x))
					}
					_ = scene.OccludedForLight(
						Ray{Vector{x, 0, 1}, Vector{0, 0, 1}}, 5, light)
				}
			}(int64(g))
		}
		wg.Wait()
		final := scene.Intersect(probe(0))
		c.Check(final.Ok() && final.HitInfo.Instance == instance,
			"interleaved concurrent reads leave the scene intact")
		// links never leak back into the shared source
		c.Check(instance.Source() == source && other.Source() == source,
			"both instances still reference the untouched shared source")

		// plain scene control group keeps the original direct-light number
		plain := &Scene{}
		plain.Add(NewPlane(Vector{}, Vector{0, 0, 1}, DiffuseMaterial(White)))
		plain.Add(NewSphere(Vector{0, 0, 5}, 1, LightMaterial(White, 1)))
		plain.Compile()
		plainColor := NewDirectSampler().Sample(plain,
			Ray{Vector{0, 0, 1}, Vector{0, 0, -1}}, rand.New(rand.NewSource(7)))
		approxColor(c, plainColor, Color{1.0 / 24, 1.0 / 24, 1.0 / 24},
			"unwrapped scene keeps the baseline 1/24 direct value")

		// example output: render-produced PPM exists and contains real content
		path := locateLinkedLightsPPM(t)
		data, err := os.ReadFile(path)
		c.Check(err == nil, "linked-lights PPM must exist at %s", path)
		if err == nil {
			w, h, maxVal, pixels, ok := parseP3(t, c, string(data))
			c.Check(ok, "PPM must parse as ASCII P3")
			c.Check(w > 0 && h > 0 && maxVal == 255 && len(pixels) == w*h*3,
				"PPM header and pixel count must be consistent")
			distinct := map[[3]int]bool{}
			peak := 0
			for i := 0; i+2 < len(pixels); i += 3 {
				distinct[[3]int{pixels[i], pixels[i+1], pixels[i+2]}] = true
				if pixels[i]+pixels[i+1]+pixels[i+2] > peak {
					peak = pixels[i] + pixels[i+1] + pixels[i+2]
				}
			}
			c.Check(len(distinct) > 1 && peak > 0, "PPM must show lit, varied pixels")
		}
	})
}

// locateLinkedLightsPPM finds the example output whether tests run from the
// pt package directory or the repository root.
func locateLinkedLightsPPM(t *testing.T) string {
	candidates := []string{
		"../.task-build/linked-lights.ppm",
		".task-build/linked-lights.ppm",
	}
	for _, path := range candidates {
		if _, err := os.Stat(path); err == nil {
			return path
		}
	}
	t.Fatalf("linked-lights.ppm not found; run examples/linked_lights.go first")
	return ""
}

// parseP3 parses an ASCII P3 PPM into width, height, max value and samples.
func parseP3(t *testing.T, c *taskcheck.C, data string) (int, int, int, []int, bool) {
	tokens := strings.Fields(data)
	if len(tokens) < 4 || tokens[0] != "P3" {
		return 0, 0, 0, nil, false
	}
	w, err1 := strconv.Atoi(tokens[1])
	h, err2 := strconv.Atoi(tokens[2])
	maxVal, err3 := strconv.Atoi(tokens[3])
	if err1 != nil || err2 != nil || err3 != nil {
		return 0, 0, 0, nil, false
	}
	pixels := make([]int, 0, w*h*3)
	for _, token := range tokens[4:] {
		value, err := strconv.Atoi(token)
		if err != nil {
			return 0, 0, 0, nil, false
		}
		pixels = append(pixels, value)
	}
	return w, h, maxVal, pixels, true
}
