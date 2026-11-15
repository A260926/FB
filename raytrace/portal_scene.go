package raytrace

import (
	"errors"
	"fmt"
	"math"

	"github.com/go-gl/mathgl/mgl64"
)

const (
	// portalHitTMin is the same ray-parameter floor the existing tracer
	// passes to every Hitable.Hit call.
	portalHitTMin = 0.001
	// portalExitOffset nudges the transported ray origin through the exit
	// frame along its local +Z axis so the ray cannot immediately re-cross
	// the destination portal plane.
	portalExitOffset = 1.0e-5
	// portalFrameTolerance is the element-wise validation tolerance for the
	// rigid-body Entry/Exit frames.
	portalFrameTolerance = 1.0e-9
)

// PortalRegion bundles everything a ray can interact with while it is
// travelling inside one connected piece of a scene: solid geometry, the
// light sources used for next-event estimation, and the environment color
// returned when a ray escapes through every portal and misses every solid.
//
// The Hitables are shared read-only exactly like the ones handed to the
// ordinary Scene: their BVHs, materials and lights are never copied.
type PortalRegion struct {
	World      Hitable
	Lights     Hitable
	Background BackgroundFunc
}

// Portal is a one-way rigid-body doorway from region From into region To.
//
// Entry and Exit are row-major 3x4 affine frames
// (three rows of [axis-x, axis-y, axis-z, translation]); the implicit fourth
// row is [0 0 0 1]. The local portal plane is z=0 and the aperture is the
// closed rectangle [-HalfWidth, HalfWidth] x [-HalfHeight, HalfHeight]. A
// ray crosses only while travelling from local z>0 toward z<0. On crossing,
// the hit point and direction are moved into the Entry frame, rotated by pi
// about the local Y axis, and moved back out through the Exit frame.
type Portal struct {
	From       int
	To         int
	Entry      [12]float64
	Exit       [12]float64
	HalfWidth  float64
	HalfHeight float64
	Enabled    bool
}

// PortalScene is a multi-region Scene whose solid hitables are stitched
// together by rigid-body portals. It implements the ordinary Scene interface
// and renders unchanged through Renderer.Render(NewMonteCarloTracer(), ...).
type PortalScene struct {
	camera         Camera
	regions        []PortalRegion
	root           int
	portals        []Portal
	maxHops        int
	portalWorld    *portalWorld
	regionPortals  [][]int
	portalRuntimes []portalRuntime
	portalTracer   *portalTracer
}

// portalRuntime holds the precomputed matrices for one portal. The Entry
// inverse maps world rays into the portal's local plane coordinates; the
// Exit frame maps the rotated local result into the destination region.
type portalRuntime struct {
	entry     mgl64.Mat4
	entryInv  mgl64.Mat4
	exit      mgl64.Mat4
	exitZAxis mgl64.Vec3
}

// NewPortalScene validates the full description and builds a PortalScene.
// The region and portal slices are copied; region Hitables are shared
// read-only. Any invalid input returns an error and never leaves a partially
// usable object behind.
func NewPortalScene(camera Camera, regions []PortalRegion, root int, portals []Portal, maxHops int) (*PortalScene, error) {
	if camera == nil {
		return nil, errors.New("portal scene: camera must not be nil")
	}
	if len(regions) == 0 {
		return nil, errors.New("portal scene: at least one region is required")
	}
	if root < 0 || root >= len(regions) {
		return nil, fmt.Errorf("portal scene: root region index %d is out of range (have %d regions)", root, len(regions))
	}
	if maxHops <= 0 {
		return nil, fmt.Errorf("portal scene: maxHops must be positive, got %d", maxHops)
	}
	for i, region := range regions {
		if region.World == nil {
			return nil, fmt.Errorf("portal scene: region %d has a nil World", i)
		}
		if region.Background == nil {
			return nil, fmt.Errorf("portal scene: region %d has a nil Background", i)
		}
	}

	runtimes, regionPortals, err := buildPortalRuntime(len(regions), portals)
	if err != nil {
		return nil, err
	}

	scene := &PortalScene{
		camera:         camera,
		regions:        append([]PortalRegion(nil), regions...),
		root:           root,
		portals:        append([]Portal(nil), portals...),
		maxHops:        maxHops,
		regionPortals:  regionPortals,
		portalRuntimes: runtimes,
	}
	scene.portalWorld = &portalWorld{scene: scene}
	scene.portalTracer = &portalTracer{scene: scene}
	return scene, nil
}

// ReplacePortals swaps the whole portal table between renders. The new table
// is fully validated before anything is replaced: on failure the old table,
// its matrices and every previously rendered result stay intact. On success
// moved frames, new targets and enable/disable flips take effect on the next
// Render; region and BVH identities are untouched. Editing during an active
// render is not supported.
func (s *PortalScene) ReplacePortals(portals []Portal) error {
	runtimes, regionPortals, err := buildPortalRuntime(len(s.regions), portals)
	if err != nil {
		return err
	}
	s.portals = append([]Portal(nil), portals...)
	s.portalRuntimes = runtimes
	s.regionPortals = regionPortals
	return nil
}

// GetCamera implements Scene.
func (s *PortalScene) GetCamera(width int, height int) Camera {
	return s.camera
}

// GetWorld implements Scene. The returned Hitable performs full portal
// traversal, including after specular and diffuse scattering.
func (s *PortalScene) GetWorld() Hitable {
	return s.portalWorld
}

// GetLightHitable implements Scene. Per-region light tables are selected by
// the portal tracer while tracing; this is the root region's table and may
// legitimately be nil.
func (s *PortalScene) GetLightHitable() Hitable {
	return s.regions[s.root].Lights
}

// GetBackgroundFunc implements Scene. The portal tracer uses the background
// of whatever region the escaping ray currently occupies.
func (s *PortalScene) GetBackgroundFunc() BackgroundFunc {
	return s.regions[s.root].Background
}

// buildPortalRuntime validates every portal against the region table and
// precomputes its frames. It never mutates any existing scene state.
func buildPortalRuntime(numRegions int, portals []Portal) ([]portalRuntime, [][]int, error) {
	runtimes := make([]portalRuntime, len(portals))
	regionPortals := make([][]int, numRegions)

	for i, portal := range portals {
		if portal.From < 0 || portal.From >= numRegions {
			return nil, nil, fmt.Errorf("portal scene: portal %d From index %d is out of range", i, portal.From)
		}
		if portal.To < 0 || portal.To >= numRegions {
			return nil, nil, fmt.Errorf("portal scene: portal %d To index %d is out of range", i, portal.To)
		}
		if !isFinite(portal.HalfWidth) || portal.HalfWidth <= 0.0 {
			return nil, nil, fmt.Errorf("portal scene: portal %d HalfWidth must be finite and positive, got %v", i, portal.HalfWidth)
		}
		if !isFinite(portal.HalfHeight) || portal.HalfHeight <= 0.0 {
			return nil, nil, fmt.Errorf("portal scene: portal %d HalfHeight must be finite and positive, got %v", i, portal.HalfHeight)
		}
		entry, err := parsePortalFrame(portal.Entry)
		if err != nil {
			return nil, nil, fmt.Errorf("portal scene: portal %d Entry frame invalid: %w", i, err)
		}
		exit, err := parsePortalFrame(portal.Exit)
		if err != nil {
			return nil, nil, fmt.Errorf("portal scene: portal %d Exit frame invalid: %w", i, err)
		}

		runtimes[i] = portalRuntime{
			entry:     entry,
			entryInv:  entry.Inv(),
			exit:      exit,
			exitZAxis: exit.Col(2).Vec3(),
		}
		regionPortals[portal.From] = append(regionPortals[portal.From], i)
	}
	return runtimes, regionPortals, nil
}

// isFinite reports whether value is neither NaN nor an infinity. Defined
// locally so the package keeps building on its declared Go 1.20 level.
func isFinite(value float64) bool {
	return !math.IsNaN(value) && !math.IsInf(value, 0)
}

// parsePortalFrame turns the row-major 3x4 description into a 4x4 affine
// matrix, verifying that every entry is finite, the rotation part is
// orthonormal within the element-wise tolerance, and its determinant is +1.
func parsePortalFrame(flat [12]float64) (mgl64.Mat4, error) {
	for i, value := range flat {
		if !isFinite(value) {
			return mgl64.Mat4{}, fmt.Errorf("entry %d is not finite", i)
		}
	}

	row0 := mgl64.Vec3{flat[0], flat[1], flat[2]}
	row1 := mgl64.Vec3{flat[4], flat[5], flat[6]}
	row2 := mgl64.Vec3{flat[8], flat[9], flat[10]}
	rows := []mgl64.Vec3{row0, row1, row2}

	// Element-wise check that R*R^T is the identity: every diagonal entry is
	// a unit row length and every off-diagonal entry is an orthogonal pair.
	gram := [3][3]float64{}
	for i := 0; i < 3; i++ {
		for j := 0; j < 3; j++ {
			gram[i][j] = rows[i].Dot(rows[j])
		}
	}
	for i := 0; i < 3; i++ {
		for j := 0; j < 3; j++ {
			want := 0.0
			if i == j {
				want = 1.0
			}
			if math.Abs(gram[i][j]-want) > portalFrameTolerance {
				return mgl64.Mat4{}, fmt.Errorf("rotation is not orthonormal at entry (%d,%d): got %v want %v", i, j, gram[i][j], want)
			}
		}
	}
	det := row0.Dot(row1.Cross(row2))
	if math.Abs(det-1.0) > portalFrameTolerance {
		return mgl64.Mat4{}, fmt.Errorf("rotation determinant must be +1, got %v", det)
	}

	return mgl64.Mat4FromRows(
		mgl64.Vec4{flat[0], flat[1], flat[2], flat[3]},
		mgl64.Vec4{flat[4], flat[5], flat[6], flat[7]},
		mgl64.Vec4{flat[8], flat[9], flat[10], flat[11]},
		mgl64.Vec4{0.0, 0.0, 0.0, 1.0},
	), nil
}

// portalWorld is the Hitable handed to the renderer. Its Hit walks portal
// hops until a solid surface in some region is reached, so specular and
// diffuse rays use exactly the same transport logic as camera rays.
type portalWorld struct {
	scene *PortalScene
}

func (w *portalWorld) Hit(ray *Ray, tMin float64, tMax float64) *HitRecord {
	hr, _, _ := w.scene.portalTracer.traceToEntity(ray, w.scene.root, 0, tMin, tMax)
	return hr
}

func (w *portalWorld) GetBoundingBox(t0 float64, t1 float64) *AABB {
	return w.scene.regions[w.scene.root].World.GetBoundingBox(t0, t1)
}

func (w *portalWorld) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	return w.scene.regions[w.scene.root].World.GetPdfValue(origin, v)
}

func (w *portalWorld) Random(origin mgl64.Vec3) mgl64.Vec3 {
	return w.scene.regions[w.scene.root].World.Random(origin)
}

// portalIntersection records one accepted portal crossing in Entry-local
// coordinates so the transport step needs no further matrix work.
type portalIntersection struct {
	portalIndex      int
	t                float64
	entryLocalOrigin mgl64.Vec3
	entryLocalDir    mgl64.Vec3
}

// portalEnv is the rayEnvironment of one region in a PortalScene. Every hop
// produces a fresh portalEnv for the destination region carrying the updated
// hop count, so hit records, material UV/normals, light proposals and
// background values always use destination-region coordinates, and the hop
// budget survives specular and diffuse scattering along the whole path.
type portalEnv struct {
	tracer    *portalTracer
	regionIdx int
	hopsUsed  int
}

func (e portalEnv) hit(ray *Ray, tMin float64, tMax float64) (*HitRecord, rayEnvironment) {
	hitRecord, hitRegion, hopsUsed := e.tracer.traceToEntity(ray, e.regionIdx, e.hopsUsed, tMin, tMax)
	if hitRecord == nil && hitRegion < 0 {
		// Portal hop budget exhausted: shadeRayColor receives a nil record
		// and then calls background on this black environment.
		return nil, portalBlackEnv{}
	}
	if hitRecord == nil {
		return nil, portalEnv{tracer: e.tracer, regionIdx: hitRegion, hopsUsed: hopsUsed}
	}
	return hitRecord, portalEnv{tracer: e.tracer, regionIdx: hitRegion, hopsUsed: hopsUsed}
}

func (e portalEnv) lights() Hitable {
	return e.tracer.scene.regions[e.regionIdx].Lights
}

func (e portalEnv) background(ray *Ray) ColorVector {
	return e.tracer.scene.regions[e.regionIdx].Background(ray)
}

// portalBlackEnv is the environment a path lands in when its next portal hop
// would exceed maxHops. Such a path must return black regardless of what the
// ray would otherwise have hit.
type portalBlackEnv struct{}

func (portalBlackEnv) hit(ray *Ray, tMin float64, tMax float64) (*HitRecord, rayEnvironment) {
	return nil, portalBlackEnv{}
}

func (portalBlackEnv) lights() Hitable { return nil }

func (portalBlackEnv) background(ray *Ray) ColorVector {
	return NewColorVector(0.0, 0.0, 0.0)
}

// portalTracer holds the portal-aware counterpart of MonteCarloTracer's
// recursive getRayColor. It carries no per-render state of its own: the
// current portal table is always read through the scene.
type portalTracer struct {
	scene *PortalScene
}

// getRayColor enters the shared MonteCarloTracer shading logic in the root
// region. The production emission/scattering/recursion code in
// shadeRayColor is reused as-is; this type only supplies transport.
func (pt *portalTracer) getRayColor(ray *Ray, regionIdx int, depth int, maxDepth int) ColorVector {
	return shadeRayColor(portalEnv{tracer: pt, regionIdx: regionIdx}, ray, depth, maxDepth)
}

// intersectPortals tests every enabled portal owned by the current region
// against the ray and returns the nearest accepted crossing. Solid geometry
// wins ties against portals, and portal-portal ties resolve to the portal
// with the lower input index, which a strict nearest comparison preserves
// because regionPortals keeps input order.
func (pt *portalTracer) intersectPortals(regionIdx int, ray *Ray, tMin float64, tMax float64) *portalIntersection {
	var best *portalIntersection
	for _, idx := range pt.scene.regionPortals[regionIdx] {
		if !pt.scene.portals[idx].Enabled {
			continue
		}
		runtime := &pt.scene.portalRuntimes[idx]
		localOrigin := runtime.entryInv.Mul4x1(ray.Origin().Vec4(1.0)).Vec3()
		localDir := runtime.entryInv.Mul4x1(ray.Direction().Vec4(0.0)).Vec3()

		// Only local z>0 -> z<0 travel crosses the plane.
		if localDir.Z() >= 0.0 {
			continue
		}
		t := -localOrigin.Z() / localDir.Z()
		if !(t > tMin) || !(t < tMax) {
			continue
		}
		localX := localOrigin.X() + t*localDir.X()
		localY := localOrigin.Y() + t*localDir.Y()
		portal := &pt.scene.portals[idx]
		if localX < -portal.HalfWidth || localX > portal.HalfWidth ||
			localY < -portal.HalfHeight || localY > portal.HalfHeight {
			continue
		}
		if best == nil || t < best.t {
			best = &portalIntersection{
				portalIndex:      idx,
				t:                t,
				entryLocalOrigin: localOrigin,
				entryLocalDir:    localDir,
			}
		}
	}
	return best
}

// transport performs one hop: Entry-local hit point and direction are rotated
// pi about Y and pushed through the Exit frame. The rotation part of each
// frame is orthonormal, so the direction length is unchanged. The new origin
// is nudged along the Exit frame's +Z axis by portalExitOffset world units.
func (pt *portalTracer) transport(intersection *portalIntersection) (*Ray, int) {
	runtime := &pt.scene.portalRuntimes[intersection.portalIndex]
	localHit := intersection.entryLocalOrigin.Add(intersection.entryLocalDir.Mul(intersection.t))

	// Rotation by pi about Y flips X and Z and keeps Y.
	midPoint := mgl64.Vec3{-localHit.X(), localHit.Y(), -localHit.Z()}
	midDir := mgl64.Vec3{-intersection.entryLocalDir.X(), intersection.entryLocalDir.Y(), -intersection.entryLocalDir.Z()}

	exitPoint := runtime.exit.Mul4x1(midPoint.Vec4(1.0)).Vec3()
	exitDir := runtime.exit.Mul4x1(midDir.Vec4(0.0)).Vec3()
	exitOrigin := exitPoint.Add(runtime.exitZAxis.Mul(portalExitOffset))

	return NewRay(exitOrigin, exitDir), pt.scene.portals[intersection.portalIndex].To
}

// traceToEntity advances a ray across zero or more portal hops until it meets
// solid geometry or escapes every region. hopsIn is the number of hops already
// consumed by earlier segments of the same sample path. A hop that would
// exceed maxHops terminates the whole path as black (region index -1).
func (pt *portalTracer) traceToEntity(ray *Ray, regionIdx int, hopsIn int, tMin float64, tMax float64) (*HitRecord, int, int) {
	hops := hopsIn
	for {
		region := &pt.scene.regions[regionIdx]
		hitRecord := region.World.Hit(ray, tMin, tMax)
		intersection := pt.intersectPortals(regionIdx, ray, tMin, tMax)

		if hitRecord != nil && (intersection == nil || hitRecord.T() <= intersection.t) {
			return hitRecord, regionIdx, hops
		}
		if intersection != nil {
			if hops >= pt.scene.maxHops {
				return nil, -1, hops
			}
			ray, regionIdx = pt.transport(intersection)
			hops++
			continue
		}
		return nil, regionIdx, hops
	}
}
