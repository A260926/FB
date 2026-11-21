package render

import (
	"errors"
	"fmt"
	"math"
	"math/rand"
	"sync"

	"github.com/markphelps/go-trace/primitive"
)

// hitTol is the relative tolerance used when matching a first hit against the
// expected surface point in visibility and density reverse lookups.
const hitTol = 1e-9

// ProposalSource couples an old-pose scene with the SurfaceRef of the source
// point in that scene. The source point is the same material point as the
// current target, identified by instance ID and unit sphere direction.
type ProposalSource struct {
	Scene *InstanceScene
	Ref   SurfaceRef
}

// ReconnectStats is a read-only report of one EstimateReconnect evaluation.
type ReconnectStats struct {
	m                 int
	positiveWeights   int
	totalWeight       float64
	selected          SurfaceRef
	hasSelected       bool
	selectedQmix      float64
	visible           bool
	continuationCalls int
}

// M is the fixed total number of candidates: candidates per source times the
// number of sources (including the automatically added current target).
func (s ReconnectStats) M() int { return s.m }

// PositiveWeights counts candidates with a strictly positive K/qmix weight.
// Misses, vanished IDs and zero-kernel candidates are included in M but not
// here.
func (s ReconnectStats) PositiveWeights() int { return s.positiveWeights }

// TotalWeight is S, the sum of all candidate weights.
func (s ReconnectStats) TotalWeight() float64 { return s.totalWeight }

// SelectedRef reports the SurfaceRef chosen by the weighted selection, if any
// candidate had positive weight.
func (s ReconnectStats) SelectedRef() (SurfaceRef, bool) { return s.selected, s.hasSelected }

// SelectedQmix is the mixture density of the selected candidate.
func (s ReconnectStats) SelectedQmix() float64 { return s.selectedQmix }

// Visible reports whether the selected point was visible from the target in
// the current scene.
func (s ReconnectStats) Visible() bool { return s.visible }

// ContinuationCalls is the number of continuation radiance evaluations, at
// most one per estimate.
func (s ReconnectStats) ContinuationCalls() int { return s.continuationCalls }

// EstimateReconnect evaluates the two-bounce reconnect estimator at a target
// surface point of the current scene. Each proposal source (plus the current
// target itself, which is always added) emits candidates independent cosine
// hemisphere rays in its own scene; hits are transported into the current
// frame by instance ID and unit sphere direction. The returned color is the
// linear RGB contribution of the surface term plus an independently sampled
// direct sky term.
func EstimateReconnect(current *InstanceScene, target SurfaceRef, sources []ProposalSource, candidates int, rnd *rand.Rand) (primitive.Color, ReconnectStats, error) {
	var stats ReconnectStats
	switch {
	case current == nil:
		return primitive.Black, stats, errors.New("render: nil current scene")
	case rnd == nil:
		return primitive.Black, stats, errors.New("render: nil rand source")
	case candidates <= 0:
		return primitive.Black, stats, fmt.Errorf("render: candidate count must be positive, got %d", candidates)
	}

	tgt, err := current.Resolve(target)
	if err != nil {
		return primitive.Black, stats, fmt.Errorf("render: invalid target: %w", err)
	}

	type proposal struct {
		scene *InstanceScene
		point SurfacePoint
	}
	all := make([]proposal, 0, len(sources)+1)
	for i, s := range sources {
		if s.Scene == nil {
			return primitive.Black, stats, fmt.Errorf("render: proposal source %d has nil scene", i)
		}
		pt, err := s.Scene.Resolve(s.Ref)
		if err != nil {
			return primitive.Black, stats, fmt.Errorf("render: proposal source %d: %w", i, err)
		}
		all = append(all, proposal{s.Scene, pt})
	}
	// The current target always joins the mixture.
	all = append(all, proposal{current, tgt})

	numSources := len(all)
	stats.m = candidates * numSources

	var totalWeight float64
	var selected SurfacePoint
	var selectedQmix float64
	haveSelection := false

	// Streaming weighted selection over all M candidates.
	for _, src := range all {
		for i := 0; i < candidates; i++ {
			dir := cosineDirection(src.point.Normal, rnd)
			ok, hit := src.scene.HitSurface(primitive.Ray{Origin: src.point.Point, Direction: dir}, tMin, math.MaxFloat64)
			if !ok {
				continue // miss: zero weight, still counted in M
			}
			y, err := current.Resolve(hit.Ref)
			if err != nil {
				continue // instance gone in the current frame
			}
			k := reconnectKernel(tgt, y)
			if k <= 0 {
				continue
			}
			qmix := 0.0
			for _, other := range all {
				qmix += transportedDensity(current, other.scene, other.point, y)
			}
			qmix /= float64(numSources)
			if qmix <= 0 {
				continue
			}
			w := k / qmix
			totalWeight += w
			stats.positiveWeights++
			if rnd.Float64() < w/totalWeight {
				selected = y
				selectedQmix = qmix
				haveSelection = true
			}
		}
	}
	stats.totalWeight = totalWeight

	result := primitive.Black

	if haveSelection {
		stats.selected = selected.Ref
		stats.hasSelected = true
		stats.selectedQmix = selectedQmix
		if visibleBetween(current, tgt.Point, selected) {
			stats.visible = true
			stats.continuationCalls = 1
			ly := continuationRadiance(current, tgt, selected, rnd)
			result = tgt.Albedo.Multiply(ly).MultiplyScalar(totalWeight / float64(stats.m))
		}
	}

	// Independent direct sky term, never part of the surface candidate pool.
	skyDir := cosineDirection(tgt.Normal, rnd)
	if ok, _ := current.Hit(primitive.Ray{Origin: tgt.Point, Direction: skyDir}, tMin, math.MaxFloat64); !ok {
		result = result.Add(tgt.Albedo.Multiply(primitive.Gradient(primitive.White, primitive.Blue, skyDir.Y)))
	}

	return result, stats, nil
}

// RenderReconnect renders a full frame of the current scene, evaluating
// EstimateReconnect at the first hit of every camera ray. It returns the
// row-major linear RGB frame (row 0 is the top scanline). The per-pixel
// random streams depend only on the seed, so the result is identical for any
// worker count.
func RenderReconnect(current *InstanceScene, camera *primitive.Camera, oldScenes []*InstanceScene, width, height, samples, candidates, workers int, seed int64) ([]primitive.Color, error) {
	switch {
	case current == nil:
		return nil, errors.New("render: nil current scene")
	case camera == nil:
		return nil, errors.New("render: nil camera")
	case width <= 0:
		return nil, fmt.Errorf("render: non-positive width %d", width)
	case height <= 0:
		return nil, fmt.Errorf("render: non-positive height %d", height)
	case samples <= 0:
		return nil, fmt.Errorf("render: non-positive sample count %d", samples)
	case candidates <= 0:
		return nil, fmt.Errorf("render: non-positive candidate count %d", candidates)
	case workers <= 0:
		return nil, fmt.Errorf("render: non-positive worker count %d", workers)
	}
	for i, s := range oldScenes {
		if s == nil {
			return nil, fmt.Errorf("render: nil old scene %d", i)
		}
	}

	frame := make([]primitive.Color, width*height)
	errs := make([]error, height)

	var wg sync.WaitGroup
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func(worker int) {
			defer wg.Done()
			for row := worker; row < height; row += workers {
				for col := 0; col < width; col++ {
					idx := row*width + col
					rnd := rand.New(rand.NewSource(seed + int64(idx)))
					acc := primitive.Black
					failed := false
					for sample := 0; sample < samples; sample++ {
						u := (float64(col) + rnd.Float64()) / float64(width)
						v := (float64(height-1-row) + rnd.Float64()) / float64(height)
						ray := camera.RayAt(u, v, rnd)
						ok, hit := current.HitSurface(ray, tMin, math.MaxFloat64)
						if !ok {
							acc = acc.Add(primitive.Gradient(primitive.White, primitive.Blue, ray.Direction.Normalize().Y))
							continue
						}
						sources := make([]ProposalSource, len(oldScenes))
						for j, old := range oldScenes {
							sources[j] = ProposalSource{Scene: old, Ref: hit.Ref}
						}
						c, _, err := EstimateReconnect(current, hit.Ref, sources, candidates, rnd)
						if err != nil {
							errs[row] = err
							failed = true
							break
						}
						acc = acc.Add(c)
					}
					if failed {
						break
					}
					frame[idx] = acc.DivideScalar(float64(samples))
				}
			}
		}(w)
	}
	wg.Wait()

	for _, err := range errs {
		if err != nil {
			return nil, err
		}
	}
	return frame, nil
}

// reconnectKernel is the target geometry kernel K, without visibility.
func reconnectKernel(x, y SurfacePoint) float64 {
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
	cosY := math.Abs(y.Normal.Dot(w))
	return cosX / math.Pi * cosY / (dist * dist)
}

// transportedDensity evaluates the area density of the current-frame point y
// under one source's sampling process, expressed per unit of current surface
// area. It is zero when the source cannot see the corresponding point as a
// first hit, or when the instance is absent from the source scene.
func transportedDensity(current *InstanceScene, srcScene *InstanceScene, srcPoint SurfacePoint, y SurfacePoint) float64 {
	ys, err := srcScene.Resolve(y.Ref)
	if err != nil {
		return 0
	}
	d := ys.Point.Subtract(srcPoint.Point)
	dist := d.Length()
	if dist <= tMin {
		return 0
	}
	w := d.DivideScalar(dist)
	cosSource := srcPoint.Normal.Dot(w)
	if cosSource <= 0 {
		return 0
	}
	ok, hit := srcScene.HitSurface(primitive.Ray{Origin: srcPoint.Point, Direction: w}, tMin, math.MaxFloat64)
	if !ok || hit.Ref.Instance != y.Ref.Instance {
		return 0
	}
	if hit.Hit.Point.Subtract(ys.Point).Length() > hitTol*math.Max(1, dist) {
		return 0
	}
	cosY := math.Abs(ys.Normal.Dot(w))
	areaDensity := cosSource / math.Pi * cosY / (dist * dist)
	srcFactor, err := srcScene.AreaFactor(y.Ref)
	if err != nil {
		return 0
	}
	curFactor, err := current.AreaFactor(y.Ref)
	if err != nil || curFactor <= 0 {
		return 0
	}
	// Densities transform with the area ratio: probability is conserved,
	// p_current * dA_current = p_source * dA_source.
	return areaDensity * srcFactor / curFactor
}

// visibleBetween reports whether y is the true first hit from `from` in the
// current scene.
func visibleBetween(scene *InstanceScene, from primitive.Vector, y SurfacePoint) bool {
	d := y.Point.Subtract(from)
	dist := d.Length()
	if dist <= tMin {
		return false
	}
	ok, hit := scene.HitSurface(primitive.Ray{Origin: from, Direction: d.DivideScalar(dist)}, tMin, math.MaxFloat64)
	if !ok || hit.Ref.Instance != y.Ref.Instance {
		return false
	}
	return hit.Hit.Point.Subtract(y.Point).Length() <= hitTol*math.Max(1, dist)
}

// continuationRadiance estimates the radiance leaving y toward x using the
// original Lambertian bounce and the existing recursive color, including y's
// own color. The recursion depth starts at 2, the next bounce after the
// second surface of the path.
func continuationRadiance(scene *InstanceScene, x, y SurfacePoint, rnd *rand.Rand) primitive.Color {
	material := primitive.Lambertian{Attenuation: y.Albedo}
	incoming := primitive.Ray{Origin: x.Point, Direction: y.Point.Subtract(x.Point)}
	rec := primitive.Hit{
		T:        y.Point.Subtract(x.Point).Length(),
		Point:    y.Point,
		Normal:   y.Normal,
		Material: material,
	}
	bounced, bouncedRay := material.Bounce(incoming, rec, rnd)
	if !bounced {
		return primitive.Black
	}
	return material.Color().Multiply(color(bouncedRay, scene, rnd, 2))
}

// cosineDirection samples the cosine-weighted hemisphere about n with density
// max(0, n·w)/π, using exactly two random numbers.
func cosineDirection(n primitive.Vector, rnd *rand.Rand) primitive.Vector {
	u1 := rnd.Float64()
	u2 := rnd.Float64()
	phi := 2 * math.Pi * u1
	sinTheta := math.Sqrt(u2)
	cosTheta := math.Sqrt(1 - u2)
	t, b := orthoBasis(n)
	return t.MultiplyScalar(math.Cos(phi) * sinTheta).
		Add(b.MultiplyScalar(math.Sin(phi) * sinTheta)).
		Add(n.MultiplyScalar(cosTheta))
}

// orthoBasis builds a tangent frame around the unit vector n.
func orthoBasis(n primitive.Vector) (t, b primitive.Vector) {
	if math.Abs(n.X) > math.Abs(n.Y) {
		invLen := 1 / math.Sqrt(n.X*n.X+n.Z*n.Z)
		t = primitive.Vector{X: -n.Z * invLen, Y: 0, Z: n.X * invLen}
	} else {
		invLen := 1 / math.Sqrt(n.Y*n.Y+n.Z*n.Z)
		t = primitive.Vector{X: 0, Y: n.Z * invLen, Z: -n.Y * invLen}
	}
	return t, n.Cross(t)
}
