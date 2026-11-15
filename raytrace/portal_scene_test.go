package raytrace_test

import (
	"math"
	"sync"
	"testing"

	"github.com/go-gl/mathgl/mgl64"
	rt "github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/hitables"
	"github.com/markzuber/zgotrace/raytrace/materials"
	"github.com/markzuber/zgotrace/raytrace/textures"
	"github.com/markzuber/zgotrace/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// fixedCamera always returns one predetermined ray, so renderer tests are
// independent of lens/aperture sampling.
type fixedCamera struct {
	ray *rt.Ray
}

func (c fixedCamera) GetRay(s float64, t float64) *rt.Ray { return c.ray }

// emptyHitable is a region with no solid geometry.
type emptyHitable struct{}

func (emptyHitable) Hit(ray *rt.Ray, tMin float64, tMax float64) *rt.HitRecord { return nil }
func (emptyHitable) GetBoundingBox(t0 float64, t1 float64) *rt.AABB {
	return rt.NewAABB(mgl64.Vec3{-1e6, -1e6, -1e6}, mgl64.Vec3{1e6, 1e6, 1e6})
}
func (emptyHitable) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 { return 0 }
func (emptyHitable) Random(origin mgl64.Vec3) mgl64.Vec3                 { return origin }

// rayRecorder is a solid Hitable that records every ray reaching its region,
// giving an independent observation of transported ray coordinates.
type rayRecorder struct {
	mu   sync.Mutex
	rays []*rt.Ray
	mat  rt.Material
}

func newRayRecorder(mat rt.Material) *rayRecorder {
	return &rayRecorder{mat: mat}
}

func (r *rayRecorder) Hit(ray *rt.Ray, tMin float64, tMax float64) *rt.HitRecord {
	r.mu.Lock()
	r.rays = append(r.rays, ray)
	r.mu.Unlock()
	return rt.NewHitRecord(1.0, ray.GetPointAtParameter(1.0), mgl64.Vec3{0, 0, 1}, mgl64.Vec2{}, r.mat)
}
func (r *rayRecorder) GetBoundingBox(t0 float64, t1 float64) *rt.AABB {
	return rt.NewAABB(mgl64.Vec3{-1e6, -1e6, -1e6}, mgl64.Vec3{1e6, 1e6, 1e6})
}
func (r *rayRecorder) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 { return 1 }
func (r *rayRecorder) Random(origin mgl64.Vec3) mgl64.Vec3                 { return origin }
func (r *rayRecorder) snapshot() []*rt.Ray {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]*rt.Ray, len(r.rays))
	copy(out, r.rays)
	return out
}

// noopMaterial absorbs light and emits a configurable color.
type noopMaterial struct {
	emission rt.ColorVector
}

func (m *noopMaterial) Scatter(rayIn *rt.Ray, hitRecord *rt.HitRecord) *rt.ScatterResult {
	return rt.NewFalseScatterResult()
}
func (m *noopMaterial) ScatteringPdf(rayIn *rt.Ray, hitRecord *rt.HitRecord, scattered *rt.Ray) float64 {
	return 0
}
func (m *noopMaterial) Emitted(rayIn *rt.Ray, hitRecord *rt.HitRecord, uvCoords mgl64.Vec2, p mgl64.Vec3) rt.ColorVector {
	return m.emission
}

// recordingPdf records the coordinates it is queried with.
type recordingPdf struct {
	mu        sync.Mutex
	generates []mgl64.Vec3
	getValues []mgl64.Vec3
	direction mgl64.Vec3
	value     float64
}

func (p *recordingPdf) GetValue(direction mgl64.Vec3) float64 {
	p.mu.Lock()
	p.getValues = append(p.getValues, direction)
	p.mu.Unlock()
	return p.value
}
func (p *recordingPdf) Generate() mgl64.Vec3 {
	p.mu.Lock()
	p.generates = append(p.generates, p.direction)
	p.mu.Unlock()
	return p.direction
}

// recordingMaterial records the coordinates of every shading call and
// delegates scattering to a recording PDF.
type recordingMaterial struct {
	mu            sync.Mutex
	emittedPoints []mgl64.Vec3
	scatterPoints []mgl64.Vec3
	pdfPoints     []mgl64.Vec3
	attenuation   rt.ColorVector
	pdf           *recordingPdf
	scatterPdf    float64
}

func (m *recordingMaterial) Scatter(rayIn *rt.Ray, hitRecord *rt.HitRecord) *rt.ScatterResult {
	m.mu.Lock()
	m.scatterPoints = append(m.scatterPoints, hitRecord.P())
	m.pdfPoints = append(m.pdfPoints, hitRecord.P())
	m.mu.Unlock()
	return rt.NewScatterResult(true, m.attenuation, nil, m.pdf)
}
func (m *recordingMaterial) ScatteringPdf(rayIn *rt.Ray, hitRecord *rt.HitRecord, scattered *rt.Ray) float64 {
	m.mu.Lock()
	m.pdfPoints = append(m.pdfPoints, hitRecord.P())
	m.mu.Unlock()
	return m.scatterPdf
}
func (m *recordingMaterial) Emitted(rayIn *rt.Ray, hitRecord *rt.HitRecord, uvCoords mgl64.Vec2, p mgl64.Vec3) rt.ColorVector {
	m.mu.Lock()
	m.emittedPoints = append(m.emittedPoints, p)
	m.mu.Unlock()
	return rt.NewColorVector(0, 0, 0)
}
func (m *recordingMaterial) pointSnapshot() []mgl64.Vec3 {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]mgl64.Vec3, len(m.scatterPoints))
	copy(out, m.scatterPoints)
	return out
}

// recordingLight is a light Hitable that records the origins next-event
// estimation is proposed from.
type recordingLight struct {
	mu      sync.Mutex
	origins []mgl64.Vec3
}

func (l *recordingLight) Hit(ray *rt.Ray, tMin float64, tMax float64) *rt.HitRecord { return nil }
func (l *recordingLight) GetBoundingBox(t0 float64, t1 float64) *rt.AABB {
	return rt.NewAABB(mgl64.Vec3{-1e6, -1e6, -1e6}, mgl64.Vec3{1e6, 1e6, 1e6})
}
func (l *recordingLight) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	l.mu.Lock()
	l.origins = append(l.origins, origin)
	l.mu.Unlock()
	return 0.5
}
func (l *recordingLight) Random(origin mgl64.Vec3) mgl64.Vec3 {
	l.mu.Lock()
	l.origins = append(l.origins, origin)
	l.mu.Unlock()
	return mgl64.Vec3{0, 0, 1}
}
func (l *recordingLight) snapshot() []mgl64.Vec3 {
	l.mu.Lock()
	defer l.mu.Unlock()
	out := make([]mgl64.Vec3, len(l.origins))
	copy(out, l.origins)
	return out
}

// ---------------------------------------------------------------------------
// Frame builders and hand-derived reference math
// ---------------------------------------------------------------------------

func identityFrame(tx, ty, tz float64) [12]float64 {
	return [12]float64{
		1, 0, 0, tx,
		0, 1, 0, ty,
		0, 0, 1, tz,
	}
}

// rotYFrame builds the row-major frame for Rotate3DY(theta) plus translation.
// R_y rows are [c 0 s], [0 1 0], [-s 0 c].
func rotYFrame(theta, tx, ty, tz float64) [12]float64 {
	c, s := math.Cos(theta), math.Sin(theta)
	return [12]float64{
		c, 0, s, tx,
		0, 1, 0, ty,
		-s, 0, c, tz,
	}
}

// flipFrame is an Exit frame rotated pi about Y. Paired with an identity
// Entry, the mandatory in-transport Y flip and this frame's rotation cancel,
// so a ray leaves the destination door travelling in its original direction.
func flipFrame(tx, ty, tz float64) [12]float64 {
	return [12]float64{
		-1, 0, 0, tx,
		0, 1, 0, ty,
		0, 0, -1, tz,
	}
}

func vecApprox(a, b mgl64.Vec3, tol float64) bool {
	return math.Abs(a.X()-b.X()) <= tol && math.Abs(a.Y()-b.Y()) <= tol && math.Abs(a.Z()-b.Z()) <= tol
}

func colorApprox(a, b rt.ColorVector, tol float64) bool {
	return math.Abs(a.R()-b.R()) <= tol && math.Abs(a.G()-b.G()) <= tol && math.Abs(a.B()-b.B()) <= tol
}

func bg(linear rt.ColorVector) rt.ColorVector {
	return rt.NewColorVector(math.Sqrt(linear.R()), math.Sqrt(linear.G()), math.Sqrt(linear.B()))
}

func newTestRenderer(width, height int) (*rt.Renderer, *rt.PixelBuffer, *rt.RenderConfig) {
	cfg := rt.NewRenderConfig(4, 1, false)
	return rt.NewRenderer(cfg), rt.NewPixelBuffer(width, height), cfg
}

// ---------------------------------------------------------------------------
// TestPortalGeometry
// ---------------------------------------------------------------------------

func TestPortalGeometry(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		srcBg := func(*rt.Ray) rt.ColorVector { return rt.NewColorVector(0.01, 0.02, 0.03) }
		dstBg := func(*rt.Ray) rt.ColorVector { return rt.NewColorVector(0.3, 0.2, 0.1) }
		marker := &noopMaterial{emission: rt.NewColorVector(0, 0, 0)}

		// Non-unit ray. Rows of R_y(theta) are [c 0 s],[0 1 0],[-s 0 c] and
		// the inverse is R_y(-theta). The ray is first chosen in Entry-local
		// coordinates and moved into world coordinates with the forward frame;
		// all expected transport values below are then re-derived by hand with
		// the inverse formulas rather than the production matrix code.
		theta := math.Pi / 3.0
		phi := 37.0 * math.Pi / 180.0
		entry := rotYFrame(theta, 1, 2, 3)
		exit := rotYFrame(phi, 10, -7, 2.5)

		// Local ray: starts on z>0 and crosses z=0 inside the aperture.
		localOrigin := mgl64.Vec3{0.3, 0.4, 5.0}
		localDir := mgl64.Vec3{0.06, -0.08, -2.5}
		ct, st := math.Cos(theta), math.Sin(theta)
		entryTranslation := mgl64.Vec3{1, 2, 3}
		// R_y(theta) * v = (c*vx + s*vz, vy, -s*vx + c*vz)
		rotOrigin := mgl64.Vec3{
			ct*localOrigin.X() + st*localOrigin.Z(),
			localOrigin.Y(),
			-st*localOrigin.X() + ct*localOrigin.Z(),
		}
		rotDir := mgl64.Vec3{
			ct*localDir.X() + st*localDir.Z(),
			localDir.Y(),
			-st*localDir.X() + ct*localDir.Z(),
		}
		p := entryTranslation.Add(rotOrigin)
		d := rotDir

		cp, sp := math.Cos(phi), math.Sin(phi)
		// R_y(-theta) * v = (c*vx - s*vz, vy, s*vx + c*vz)
		pmt := p.Sub(entryTranslation)
		pLocal := mgl64.Vec3{
			ct*pmt.X() - st*pmt.Z(),
			pmt.Y(),
			st*pmt.X() + ct*pmt.Z(),
		}
		dLocal := mgl64.Vec3{
			ct*d.X() - st*d.Z(),
			d.Y(),
			st*d.X() + ct*d.Z(),
		}
		// Ray starts on local z>0 and crosses z=0 going to z<0.
		c.Check(pLocal.Z() > 0 && dLocal.Z() < 0, "hand ray must approach the portal plane from +Z")
		hitT := -pLocal.Z() / dLocal.Z()
		localHit := pLocal.Add(dLocal.Mul(hitT))
		// Aperture interior check against the closed rectangle.
		c.Check(math.Abs(localHit.X()) < 2.0 && math.Abs(localHit.Y()) < 2.0,
			"hand ray should land inside the aperture, got %v", localHit)

		recorder := newRayRecorder(marker)
		regions := []rt.PortalRegion{
			{World: emptyHitable{}, Background: srcBg},
			{World: recorder, Lights: nil, Background: dstBg},
		}
		portals := []rt.Portal{{
			From: 0, To: 1, Entry: entry, Exit: exit,
			HalfWidth: 2.0, HalfHeight: 2.0, Enabled: true,
		}}
		scene, err := rt.NewPortalScene(
			fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{0, 0, -1})},
			regions, 0, portals, 4)
		c.Check(err == nil, "hand-computed scene should build: %v", err)

		hr := scene.GetWorld().Hit(rt.NewRay(p, d), 0.001, math.MaxFloat64)
		c.Check(hr != nil, "transported ray should hit the destination recorder")
		rays := recorder.snapshot()
		c.Check(len(rays) == 1, "recorder should see exactly one transported ray, got %d", len(rays))
		if len(rays) == 1 {
			// pi about Y flips X and Z.
			midPoint := mgl64.Vec3{-localHit.X(), localHit.Y(), -localHit.Z()}
			midDir := mgl64.Vec3{-dLocal.X(), dLocal.Y(), -dLocal.Z()}
			// R_y(phi) * v = (cp*vx + sp*vz, vy, -sp*vx + cp*vz)
			rotPoint := mgl64.Vec3{
				cp*midPoint.X() + sp*midPoint.Z(),
				midPoint.Y(),
				-sp*midPoint.X() + cp*midPoint.Z(),
			}
			rotDir := mgl64.Vec3{
				cp*midDir.X() + sp*midDir.Z(),
				midDir.Y(),
				-sp*midDir.X() + cp*midDir.Z(),
			}
			exitTranslation := mgl64.Vec3{10, -7, 2.5}
			exitZ := mgl64.Vec3{sp, 0, cp}
			expectedOrigin := exitTranslation.Add(rotPoint).Add(exitZ.Mul(1e-5))
			c.Check(vecApprox(rays[0].Origin(), expectedOrigin, 1e-9),
				"transported origin mismatch: got %v want %v", rays[0].Origin(), expectedOrigin)
			c.Check(vecApprox(rays[0].Direction(), rotDir, 1e-12),
				"transported direction mismatch: got %v want %v", rays[0].Direction(), rotDir)
			c.Check(math.Abs(rays[0].Direction().Len()-d.Len()) <= 1e-12,
				"direction length must be preserved: got %v want %v", rays[0].Direction().Len(), d.Len())
		}

		// Aperture edge: the closed rectangle includes its boundary.
		edgeRecorder := newRayRecorder(marker)
		edgeRegions := []rt.PortalRegion{
			{World: emptyHitable{}, Background: srcBg},
			{World: edgeRecorder, Background: dstBg},
		}
		edgePortals := []rt.Portal{{
			From: 0, To: 1, Entry: identityFrame(0, 0, 0), Exit: identityFrame(0, 0, 0),
			HalfWidth: 2.0, HalfHeight: 2.0, Enabled: true,
		}}
		edgeScene, err := rt.NewPortalScene(fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{0, 0, -1})},
			edgeRegions, 0, edgePortals, 2)
		c.Check(err == nil, "edge scene should build: %v", err)
		// Crosses z=0 at exactly x=+2, the aperture boundary.
		edgeRay := rt.NewRay(mgl64.Vec3{2, 0, 5}, mgl64.Vec3{0, 0, -1})
		c.Check(edgeScene.GetWorld().Hit(edgeRay, 0.001, math.MaxFloat64) != nil, "edge ray must cross on the closed boundary")
		c.Check(len(edgeRecorder.snapshot()) == 1, "edge crossing must reach the destination region")
		// Just outside the aperture: normal tracing in the source region, no hop.
		outRay := rt.NewRay(mgl64.Vec3{2.001, 0, 5}, mgl64.Vec3{0, 0, -1})
		c.Check(edgeScene.GetWorld().Hit(outRay, 0.001, math.MaxFloat64) == nil, "ray outside the aperture must not teleport")
		c.Check(len(edgeRecorder.snapshot()) == 1, "outside ray must not reach the destination")

		// Back side: origin on local z<0 travelling toward +Z never crosses.
		backRay := rt.NewRay(mgl64.Vec3{0, 0, -5}, mgl64.Vec3{0, 0, 1})
		c.Check(edgeScene.GetWorld().Hit(backRay, 0.001, math.MaxFloat64) == nil, "back-side ray must not teleport")
		c.Check(len(edgeRecorder.snapshot()) == 1, "back-side ray must stay in the source region")

		// Solid geometry blocking the doorway wins over the portal.
		blockMat := &noopMaterial{emission: rt.NewColorVector(0.4, 0.5, 0.6)}
		blockSphere := hitables.NewSphere(mgl64.Vec3{0, 0, 2}, 1.0, blockMat)
		blockRegions := []rt.PortalRegion{
			{World: hitables.NewBvhNode([]rt.Hitable{blockSphere}, 0, 1), Background: srcBg},
			{World: edgeRecorder, Background: dstBg},
		}
		blockScene, err := rt.NewPortalScene(fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{0, 0, -1})},
			blockRegions, 0, edgePortals, 2)
		c.Check(err == nil, "blocking scene should build: %v", err)
		blockHr := blockScene.GetWorld().Hit(rt.NewRay(mgl64.Vec3{0, 0, 5}, mgl64.Vec3{0, 0, -1}), 0.001, math.MaxFloat64)
		c.Check(blockHr != nil && blockHr.Material() == blockMat, "solid in front of the door must be hit instead of teleporting")

		// Equal distance: solid and portal plane at the same t, solid wins.
		tieSphere := hitables.NewSphere(mgl64.Vec3{0, 0, -1}, 1.0, blockMat)
		tieRegions := []rt.PortalRegion{
			{World: hitables.NewBvhNode([]rt.Hitable{tieSphere}, 0, 1), Background: srcBg},
			{World: edgeRecorder, Background: dstBg},
		}
		tieScene, err := rt.NewPortalScene(fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{0, 0, -1})},
			tieRegions, 0, edgePortals, 2)
		c.Check(err == nil, "tie scene should build: %v", err)
		// Sphere surface and portal plane are both at t=5.
		tieHr := tieScene.GetWorld().Hit(rt.NewRay(mgl64.Vec3{0, 0, 5}, mgl64.Vec3{0, 0, -1}), 0.001, math.MaxFloat64)
		c.Check(tieHr != nil && math.Abs(tieHr.T()-5.0) <= 1e-12 && tieHr.Material() == blockMat,
			"on equal t the solid surface must win")

		// Portal-portal tie at the same plane resolves to input order.
		recA, recB := newRayRecorder(marker), newRayRecorder(marker)
		shared := newRayRecorder(marker)
		twoDoorRegions := []rt.PortalRegion{
			{World: emptyHitable{}, Background: srcBg},
			{World: recA, Background: dstBg},
			{World: recB, Background: dstBg},
			{World: shared, Background: dstBg},
		}
		coLocated := identityFrame(0, 0, 0)
		twoDoorPortals := []rt.Portal{
			{From: 0, To: 1, Entry: coLocated, Exit: identityFrame(0, 0, 0), HalfWidth: 2, HalfHeight: 2, Enabled: true},
			{From: 0, To: 2, Entry: coLocated, Exit: identityFrame(0, 0, 0), HalfWidth: 2, HalfHeight: 2, Enabled: true},
			// Two separate entrances into the one shared region 3, each with
			// its own entry position and its own shifted exit. The shared
			// region's BVH/material are referenced, never copied.
			{From: 0, To: 3, Entry: identityFrame(0, 5, 0), Exit: identityFrame(7, 0, 0), HalfWidth: 2, HalfHeight: 2, Enabled: true},
			{From: 0, To: 3, Entry: identityFrame(10, 0, 0), Exit: identityFrame(20, 0, 0), HalfWidth: 2, HalfHeight: 2, Enabled: true},
		}
		twoDoorScene, err := rt.NewPortalScene(fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{0, 0, -1})},
			twoDoorRegions, 0, twoDoorPortals, 3)
		c.Check(err == nil, "two-door scene should build: %v", err)
		// Aim through the co-located pair: the lower input index must win.
		twoDoorScene.GetWorld().Hit(rt.NewRay(mgl64.Vec3{0, 0, 5}, mgl64.Vec3{0, 0, -1}), 0.001, math.MaxFloat64)
		c.Check(len(recA.snapshot()) == 1 && len(recB.snapshot()) == 0,
			"co-located portals must resolve by input index, got A=%d B=%d", len(recA.snapshot()), len(recB.snapshot()))

		// The two entrances into the shared region must each land at their
		// own coordinates and never inherit the other doorway's position.
		// Door at entry (0,5,0): local hit (0,0,0) -> exit world (7,0,0).
		twoDoorScene.GetWorld().Hit(rt.NewRay(mgl64.Vec3{0, 5, 5}, mgl64.Vec3{0, 0, -1}), 0.001, math.MaxFloat64)
		// Door at entry (10,0,0): local hit (0,0,0) -> exit world (20,0,0).
		twoDoorScene.GetWorld().Hit(rt.NewRay(mgl64.Vec3{10, 0, 5}, mgl64.Vec3{0, 0, -1}), 0.001, math.MaxFloat64)
		sharedRays := shared.snapshot()
		c.Check(len(sharedRays) == 2, "shared region should be reached once per entrance, got %d", len(sharedRays))
		if len(sharedRays) == 2 {
			c.Check(math.Abs(sharedRays[0].Origin().X()-7.0) <= 1e-9 && math.Abs(sharedRays[1].Origin().X()-20.0) <= 1e-9,
				"shared region entrances must keep independent positions, got %v and %v",
				sharedRays[0].Origin(), sharedRays[1].Origin())
		}

		// Disabled portals never participate.
		disabledPortals := []rt.Portal{{
			From: 0, To: 1, Entry: identityFrame(0, 0, 0), Exit: identityFrame(0, 0, 0),
			HalfWidth: 2, HalfHeight: 2, Enabled: false,
		}}
		disabledScene, err := rt.NewPortalScene(fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{0, 0, -1})},
			edgeRegions, 0, disabledPortals, 2)
		c.Check(err == nil, "disabled scene should build: %v", err)
		before := len(edgeRecorder.snapshot())
		c.Check(disabledScene.GetWorld().Hit(rt.NewRay(mgl64.Vec3{0, 0, 5}, mgl64.Vec3{0, 0, -1}), 0.001, math.MaxFloat64) == nil,
			"disabled portal must not transport the ray")
		c.Check(len(edgeRecorder.snapshot()) == before, "disabled portal must not touch the destination region")

		// Validation: malformed input returns errors and no partial object.
		bgFunc := func(*rt.Ray) rt.ColorVector { return rt.NewColorVector(0, 0, 0) }
		goodRegions := []rt.PortalRegion{{World: emptyHitable{}, Background: bgFunc}}
		validFrame := identityFrame(0, 0, 0)
		badCases := []struct {
			name    string
			camera  rt.Camera
			regions []rt.PortalRegion
			root    int
			portals []rt.Portal
			maxHops int
		}{
			{"nil camera", nil, goodRegions, 0, nil, 1},
			{"no regions", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})}, nil, 0, nil, 1},
			{"root out of range", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})}, goodRegions, 1, nil, 1},
			{"zero hops", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})}, goodRegions, 0, nil, 0},
			{"negative hops", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})}, goodRegions, 0, nil, -1},
			{"nil world", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})},
				[]rt.PortalRegion{{World: nil, Background: bgFunc}}, 0, nil, 1},
			{"nil background", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})},
				[]rt.PortalRegion{{World: emptyHitable{}, Background: nil}}, 0, nil, 1},
			{"bad From", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})}, goodRegions, 0,
				[]rt.Portal{{From: 5, To: 0, Entry: validFrame, Exit: validFrame, HalfWidth: 1, HalfHeight: 1, Enabled: true}}, 1},
			{"bad To", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})}, goodRegions, 0,
				[]rt.Portal{{From: 0, To: -1, Entry: validFrame, Exit: validFrame, HalfWidth: 1, HalfHeight: 1, Enabled: true}}, 1},
			{"zero half width", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})}, goodRegions, 0,
				[]rt.Portal{{From: 0, To: 0, Entry: validFrame, Exit: validFrame, HalfWidth: 0, HalfHeight: 1, Enabled: true}}, 1},
			{"NaN half height", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})}, goodRegions, 0,
				[]rt.Portal{{From: 0, To: 0, Entry: validFrame, Exit: validFrame, HalfWidth: 1, HalfHeight: math.NaN(), Enabled: true}}, 1},
			{"mirror frame det -1", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})}, goodRegions, 0,
				[]rt.Portal{{From: 0, To: 0,
					Entry: [12]float64{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
					Exit:  validFrame, HalfWidth: 1, HalfHeight: 1, Enabled: true}}, 1},
			{"non orthogonal frame", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})}, goodRegions, 0,
				[]rt.Portal{{From: 0, To: 0,
					Entry: [12]float64{1, 0.1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
					Exit:  validFrame, HalfWidth: 1, HalfHeight: 1, Enabled: true}}, 1},
			{"non finite frame", fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})}, goodRegions, 0,
				[]rt.Portal{{From: 0, To: 0,
					Entry: [12]float64{1, 0, 0, math.Inf(1), 0, 1, 0, 0, 0, 0, 1, 0},
					Exit:  validFrame, HalfWidth: 1, HalfHeight: 1, Enabled: true}}, 1},
		}
		for _, bc := range badCases {
			s, badErr := rt.NewPortalScene(bc.camera, bc.regions, bc.root, bc.portals, bc.maxHops)
			c.Check(badErr != nil && s == nil, "case %q must fail validation without a partial object", bc.name)
		}
		// Nil Lights are explicitly allowed.
		okScene, err := rt.NewPortalScene(fixedCamera{rt.NewRay(mgl64.Vec3{}, mgl64.Vec3{})},
			[]rt.PortalRegion{{World: emptyHitable{}, Lights: nil, Background: bgFunc}}, 0, nil, 1)
		c.Check(err == nil && okScene != nil, "nil Lights must be allowed")
	})
}

// ---------------------------------------------------------------------------
// TestPortalTransport
// ---------------------------------------------------------------------------

func TestPortalTransport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		blackBg := func(*rt.Ray) rt.ColorVector { return rt.NewColorVector(0, 0, 0) }
		srcBg := func(*rt.Ray) rt.ColorVector { return rt.NewColorVector(0.9, 0.05, 0.05) }
		dstBg := func(*rt.Ray) rt.ColorVector { return rt.NewColorVector(0.05, 0.2, 0.9) }
		emission := rt.NewColorVector(0.8, 0.7, 0.6)
		emissive := materials.NewDiffuseLight(textures.NewColorTexture(emission))

		render := func(scene rt.Scene) rt.ColorVector {
			renderer, buf, cfg := newTestRenderer(2, 2)
			renderer.Render(rt.NewMonteCarloTracer(), buf, scene, cfg)
			return buf.GetPixelColor(0, 0)
		}
		cam := func(ray *rt.Ray) rt.Camera { return fixedCamera{ray} }
		straightRay := rt.NewRay(mgl64.Vec3{0, 0, 5}, mgl64.Vec3{0, 0, -1})
		door := func(to int, entry, exit [12]float64) rt.Portal {
			return rt.Portal{From: 0, To: to, Entry: entry, Exit: exit,
				HalfWidth: 20, HalfHeight: 20, Enabled: true}
		}

		// 1. Emission from a solid on the far side of the door.
		emissiveSphere := hitables.NewSphere(mgl64.Vec3{0, 0, -5}, 1.0, emissive)
		emissionScene, err := rt.NewPortalScene(cam(straightRay),
			[]rt.PortalRegion{
				{World: emptyHitable{}, Background: blackBg},
				{World: hitables.NewBvhNode([]rt.Hitable{emissiveSphere}, 0, 1), Background: blackBg},
			}, 0,
			[]rt.Portal{door(1, identityFrame(0, 0, 0), flipFrame(0, 0, 0))}, 2)
		c.Check(err == nil, "emission scene should build: %v", err)
		c.Check(colorApprox(render(emissionScene), bg(emission), 1.0/255),
			"far-side emissive solid must be visible through the portal")

		// 2. A miss on the far side returns the destination background, not
		// the source region's background.
		bgScene, err := rt.NewPortalScene(cam(straightRay),
			[]rt.PortalRegion{
				{World: emptyHitable{}, Background: srcBg},
				{World: emptyHitable{}, Background: dstBg},
			}, 0,
			[]rt.Portal{door(1, identityFrame(0, 0, 0), flipFrame(0, 0, 0))}, 2)
		c.Check(err == nil, "background scene should build: %v", err)
		c.Check(colorApprox(render(bgScene), bg(dstBg(nil)), 1.0/255),
			"escaping ray must use the destination region background")

		// 3. One specular reflection, then through a door. The reflected ray
		// travels back toward +Z, so the Entry frame is rotated pi about Y at
		// z=3: its local +Z points to world +Z and accepts that ray. The Exit
		// is the identity frame.
		mirror := hitables.NewXyRect(-100, 100, -100, 100, -3.0,
			materials.NewMetalMaterial(rt.NewColorVector(1, 1, 1), 0.0))
		specRecorder := newRayRecorder(&noopMaterial{emission: emission})
		flippedEntry := [12]float64{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 3}
		specScene, err := rt.NewPortalScene(
			cam(rt.NewRay(mgl64.Vec3{0, 0, 0}, mgl64.Vec3{0, -1, -1})),
			[]rt.PortalRegion{
				{World: hitables.NewHitableList([]rt.Hitable{mirror}), Background: blackBg},
				{World: specRecorder, Background: blackBg},
			}, 0,
			[]rt.Portal{door(1, flippedEntry, identityFrame(0, 0, 0))}, 2)
		c.Check(err == nil, "specular scene should build: %v", err)
		c.Check(colorApprox(render(specScene), bg(emission), 1.0/255),
			"a specular ray must reach the far side and pick up its emission")
		specRays := specRecorder.snapshot()
		// The 2x2 buffer runs the same fixed ray once per pixel.
		c.Check(len(specRays) == 4, "every pixel's specular path must reach the far side, got %d", len(specRays))
		if len(specRays) == 4 {
			// Hand-derived: the metal normalizes the input (0,-1,-1), reflects
			// it off the +Z rect to unit dir (0,-1/√2,+1/√2) from (0,-3,-3).
			// The pi-Y entry maps that to local dir (0,-1/√2,-1/√2), crossing
			// at local (0,-9,0); after the in-transport Y flip the exit ray
			// is origin (0,-9,1e-5), direction (0,-1/√2,+1/√2).
			invSqrt2 := 1.0 / math.Sqrt2
			c.Check(vecApprox(specRays[0].Origin(), mgl64.Vec3{0, -9, 1e-5}, 1e-9),
				"specular portal exit origin mismatch: %v", specRays[0].Origin())
			c.Check(vecApprox(specRays[0].Direction(), mgl64.Vec3{0, -invSqrt2, invSqrt2}, 1e-12),
				"specular portal exit direction mismatch: %v", specRays[0].Direction())
		}

		// 4. Diffuse scattering on each side: light/material PDFs after the
		// hop must be evaluated in destination-region coordinates.
		matPdf0 := &recordingPdf{direction: mgl64.Vec3{0, 0, -1}, value: 0.5}
		mat0 := &recordingMaterial{
			attenuation: rt.NewColorVector(0.4, 0.8, 0.2), pdf: matPdf0, scatterPdf: 0.25,
		}
		matPdf1 := &recordingPdf{direction: mgl64.Vec3{0, 0, 1}, value: 0.5}
		mat1 := &recordingMaterial{
			attenuation: rt.NewColorVector(0.2, 0.3, 0.4), pdf: matPdf1, scatterPdf: 0.25,
		}
		diffuseRect0 := hitables.NewXyRect(-10, 10, -10, 10, 2.0, mat0)
		diffuseRect1 := hitables.NewXyRect(-10, 10, -10, 10, -5.0, mat1)
		light1 := &recordingLight{}
		diffuseScene, err := rt.NewPortalScene(cam(straightRay),
			[]rt.PortalRegion{
				{World: hitables.NewHitableList([]rt.Hitable{diffuseRect0}), Background: blackBg},
				{World: hitables.NewHitableList([]rt.Hitable{diffuseRect1}), Lights: light1, Background: blackBg},
			}, 0,
			[]rt.Portal{door(1, identityFrame(0, 0, 0), flipFrame(0, 0, 0))}, 2)
		c.Check(err == nil, "diffuse scene should build: %v", err)
		render(diffuseScene)
		c.Check(len(mat0.pointSnapshot()) > 0 && vecApprox(mat0.pointSnapshot()[0], mgl64.Vec3{0, 0, 2}, 1e-12),
			"source material must be shaded in source coordinates")
		mat1Points := mat1.pointSnapshot()
		c.Check(len(mat1Points) > 0 && vecApprox(mat1Points[0], mgl64.Vec3{0, 0, -5}, 1e-12),
			"destination material must be shaded in destination coordinates, got %v", mat1Points)
		lightOrigins := light1.snapshot()
		c.Check(len(lightOrigins) > 0, "destination light PDF must be consulted after the hop")
		if len(lightOrigins) > 0 {
			allDest := true
			for _, origin := range lightOrigins {
				if !vecApprox(origin, mgl64.Vec3{0, 0, -5}, 1e-12) {
					allDest = false
				}
			}
			c.Check(allDest, "every light proposal origin must use destination coordinates, got %v", lightOrigins)
		}

		// 5. A region without lights uses the material PDF directly: the
		// render must not panic or wrap an empty mixture component.
		noLightScene, err := rt.NewPortalScene(cam(straightRay),
			[]rt.PortalRegion{
				{World: hitables.NewHitableList([]rt.Hitable{diffuseRect0}), Lights: nil, Background: blackBg},
				{World: hitables.NewHitableList([]rt.Hitable{diffuseRect1}), Lights: nil, Background: blackBg},
			}, 0,
			[]rt.Portal{door(1, identityFrame(0, 0, 0), flipFrame(0, 0, 0))}, 2)
		c.Check(err == nil, "no-light scene should build: %v", err)
		matPdf1.getValues = nil
		render(noLightScene)
		c.Check(len(matPdf1.getValues) > 0, "material PDF must still be evaluated with no lights")

		// 6. Portal cycles and a hop budget that survives scattering.
		// The pi-rotated exits keep every transported ray moving along -Z,
		// so A then B then A again is a genuine multi-hop cycle.
		cyclePortals := []rt.Portal{
			{From: 0, To: 1, Entry: identityFrame(0, 0, 0), Exit: flipFrame(0, 0, 0),
				HalfWidth: 20, HalfHeight: 20, Enabled: true},
			{From: 1, To: 0, Entry: identityFrame(0, 0, -10), Exit: flipFrame(0, 0, 8),
				HalfWidth: 20, HalfHeight: 20, Enabled: true},
		}
		cycleScene, err := rt.NewPortalScene(cam(straightRay),
			[]rt.PortalRegion{
				{World: emptyHitable{}, Background: blackBg},
				{World: emptyHitable{}, Background: blackBg},
			}, 0, cyclePortals, 2)
		c.Check(err == nil, "cycle scene should build: %v", err)
		c.Check(colorApprox(render(cycleScene), rt.NewColorVector(0, 0, 0), 1e-12),
			"exhausting the hop budget inside a cycle must return black")

		// With a solid one hop away, maxHops=1 still reaches it.
		oneHopScene, err := rt.NewPortalScene(cam(straightRay),
			[]rt.PortalRegion{
				{World: emptyHitable{}, Background: blackBg},
				{World: hitables.NewBvhNode([]rt.Hitable{
					hitables.NewSphere(mgl64.Vec3{0, 0, -5}, 1.0, emissive),
				}, 0, 1), Background: blackBg},
			}, 0, cyclePortals[:1], 1)
		c.Check(err == nil, "one-hop scene should build: %v", err)
		c.Check(colorApprox(render(oneHopScene), bg(emission), 1.0/255),
			"the single allowed hop must still reach a far-side emissive solid")

		// Scattering must not reset the budget: camera -> diffuse bounce ->
		// hop 1 -> diffuse bounce -> hop 2 is refused when maxHops is 1, but
		// succeeds when it is 2.
		bouncePdf0 := &recordingPdf{direction: mgl64.Vec3{0, 0, -1}, value: 0.5}
		bounceMat0 := &recordingMaterial{attenuation: rt.NewColorVector(1, 1, 1), pdf: bouncePdf0, scatterPdf: 0.5}
		bouncePdf1 := &recordingPdf{direction: mgl64.Vec3{0, 0, -1}, value: 0.5}
		bounceMat1 := &recordingMaterial{attenuation: rt.NewColorVector(1, 1, 1), pdf: bouncePdf1, scatterPdf: 0.5}
		budgetRegions := []rt.PortalRegion{
			{World: hitables.NewHitableList([]rt.Hitable{
				hitables.NewXyRect(-10, 10, -10, 10, 2.0, bounceMat0),
				// Reached only after the second hop lands back in region 0.
				hitables.NewSphere(mgl64.Vec3{0, 0, 6}, 1.0, emissive),
			}), Background: blackBg},
			{World: hitables.NewHitableList([]rt.Hitable{
				hitables.NewXyRect(-10, 10, -10, 10, -3.0, bounceMat1),
			}), Background: blackBg},
		}
		budgetPortals := []rt.Portal{
			{From: 0, To: 1, Entry: identityFrame(0, 0, 0), Exit: flipFrame(0, 0, 0),
				HalfWidth: 20, HalfHeight: 20, Enabled: true},
			{From: 1, To: 0, Entry: identityFrame(0, 0, -5), Exit: flipFrame(0, 0, 8),
				HalfWidth: 20, HalfHeight: 20, Enabled: true},
		}
		budgetScene1, err := rt.NewPortalScene(cam(straightRay), budgetRegions, 0, budgetPortals, 1)
		c.Check(err == nil, "budget-1 scene should build: %v", err)
		c.Check(colorApprox(render(budgetScene1), rt.NewColorVector(0, 0, 0), 1e-12),
			"hop count must carry across the diffuse bounce: second crossing returns black")
		budgetScene2, err := rt.NewPortalScene(cam(straightRay), budgetRegions, 0, budgetPortals, 2)
		c.Check(err == nil, "budget-2 scene should build: %v", err)
		c.Check(colorApprox(render(budgetScene2), bg(emission), 1.0/255),
			"with two hops the same path reaches the emissive sphere after scattering")

		// 7. Numeric parity with an ordinary Scene when no portals exist.
		plainWorld := hitables.NewHitableList([]rt.Hitable{
			hitables.NewSphere(mgl64.Vec3{0, 0, -5}, 1.0, emissive),
		})
		plainCamera := cam(straightRay)
		plain := rt.NewScene(plainCamera, plainWorld, nil, dstBg)
		portalEquiv, err := rt.NewPortalScene(plainCamera,
			[]rt.PortalRegion{{World: plainWorld, Background: dstBg}}, 0, nil, 1)
		c.Check(err == nil, "equivalent portal scene should build: %v", err)
		renderer, bufA, cfg := newTestRenderer(4, 4)
		renderer.Render(rt.NewMonteCarloTracer(), bufA, plain, cfg)
		renderer2, bufB, cfg2 := newTestRenderer(4, 4)
		renderer2.Render(rt.NewMonteCarloTracer(), bufB, portalEquiv, cfg2)
		allEqual := true
		for y := 0; y < 4; y++ {
			for x := 0; x < 4; x++ {
				if !colorApprox(bufA.GetPixelColor(x, y), bufB.GetPixelColor(x, y), 1e-12) {
					allEqual = false
				}
			}
		}
		c.Check(allEqual, "portal scene with no enabled doors must match the ordinary Scene numerically")
	})
}

// ---------------------------------------------------------------------------
// TestPortalUpdates
// ---------------------------------------------------------------------------

func TestPortalUpdates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		blackBg := func(*rt.Ray) rt.ColorVector { return rt.NewColorVector(0, 0, 0) }
		srcBg := func(*rt.Ray) rt.ColorVector { return rt.NewColorVector(0.9, 0.05, 0.05) }
		emission := rt.NewColorVector(0.8, 0.7, 0.6)
		emissiveSphere := hitables.NewSphere(mgl64.Vec3{0, 0, -5}, 1.0,
			materials.NewDiffuseLight(textures.NewColorTexture(emission)))

		cam := fixedCamera{rt.NewRay(mgl64.Vec3{0, 0, 5}, mgl64.Vec3{0, 0, -1})}
		regions := []rt.PortalRegion{
			{World: emptyHitable{}, Background: srcBg},
			{World: hitables.NewBvhNode([]rt.Hitable{emissiveSphere}, 0, 1), Background: blackBg},
		}
		initialDoor := rt.Portal{From: 0, To: 1,
			Entry: identityFrame(0, 0, 0), Exit: flipFrame(0, 0, 0),
			HalfWidth: 20, HalfHeight: 20, Enabled: true}
		scene, err := rt.NewPortalScene(cam, regions, 0, []rt.Portal{initialDoor}, 2)
		c.Check(err == nil, "updates scene should build: %v", err)
		worldIdentity := scene.GetWorld()
		regionWorld := regions[0].World

		render := func() rt.ColorVector {
			renderer, buf, cfg := newTestRenderer(2, 2)
			renderer.Render(rt.NewMonteCarloTracer(), buf, scene, cfg)
			return buf.GetPixelColor(0, 0)
		}

		// Frame 1: the door looks into the emissive sphere.
		before := render()
		c.Check(colorApprox(before, bg(emission), 1.0/255), "initial render must see the far-side emission")

		// Move the exit sideways so the transported ray misses the sphere and
		// escapes into the destination background.
		movedDoor := initialDoor
		movedDoor.Exit = flipFrame(10, 0, 0)
		c.Check(scene.ReplacePortals([]rt.Portal{movedDoor}) == nil, "moving a door must succeed")
		moved := render()
		c.Check(colorApprox(moved, bg(blackBg(nil)), 1.0/255), "moved door must make the ray miss the far-side solid")

		// Region/BVH identities are untouched by replacement.
		c.Check(scene.GetWorld() == worldIdentity, "portal world identity must be preserved across replacement")
		c.Check(regions[0].World == regionWorld, "region Hitable/BVH identity must be preserved")

		// Disable, then re-enable: closed doors never participate.
		closedDoor := movedDoor
		closedDoor.Enabled = false
		c.Check(scene.ReplacePortals([]rt.Portal{closedDoor}) == nil, "closing a door must succeed")
		closed := render()
		c.Check(colorApprox(closed, bg(srcBg(nil)), 1.0/255), "a closed door must leave the ray in the source region")
		c.Check(scene.ReplacePortals([]rt.Portal{movedDoor}) == nil, "reopening a door must succeed")
		reopened := render()
		c.Check(colorApprox(reopened, moved, 1e-12), "reopening must restore the moved-door result")

		// A failed replacement keeps the old table, matrices and renders.
		badDoor := movedDoor
		badDoor.Exit = [12]float64{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0} // last row zero: not unit length
		replaceErr := scene.ReplacePortals([]rt.Portal{badDoor})
		c.Check(replaceErr != nil, "an invalid replacement must return an error")
		afterFailed := render()
		c.Check(colorApprox(afterFailed, reopened, 1e-12), "failed replacement must preserve the old render result")

		// Retargeting a door to another region takes effect immediately.
		retargeted := movedDoor
		retargeted.To = 0
		c.Check(scene.ReplacePortals([]rt.Portal{retargeted}) == nil, "retargeting must succeed")
		retargetedColor := render()
		// Door now loops into the empty source region: ray escapes to its bg.
		c.Check(colorApprox(retargetedColor, bg(srcBg(nil)), 1.0/255),
			"retargeted door must render the new destination region")

		// Repeated toggling between renders keeps behaving consistently.
		for i := 0; i < 3; i++ {
			off := retargeted
			off.Enabled = false
			c.Check(scene.ReplacePortals([]rt.Portal{off}) == nil, "toggle off must succeed")
			c.Check(colorApprox(render(), bg(srcBg(nil)), 1.0/255), "toggle cycle %d: closed door", i)
			c.Check(scene.ReplacePortals([]rt.Portal{retargeted}) == nil, "toggle on must succeed")
			c.Check(colorApprox(render(), retargetedColor, 1e-12), "toggle cycle %d: reopened door", i)
		}

		// Moving the ENTRY frame proves stale inverse matrices do not linger:
		// the new intersection coordinates must come from the new inverse.
		entryMoved := initialDoor
		entryMoved.Entry = identityFrame(0.5, 0, 0)
		c.Check(scene.ReplacePortals([]rt.Portal{entryMoved}) == nil, "entry move must succeed")
		// A straight ray at world x=0 lands at entry-local x=-0.5; after the
		// Y flips it exits at destination x=-0.5 and still intersects the
		// radius-1 sphere centered at x=0.
		probe := scene.GetWorld().Hit(rt.NewRay(mgl64.Vec3{0, 0, 5}, mgl64.Vec3{0, 0, -1}), 0.001, math.MaxFloat64)
		c.Check(probe != nil && math.Abs(probe.P().X()-(-0.5)) <= 1e-9,
			"replaced entry inverse must drive new transport coordinates, got %v", probe)
	})
}
