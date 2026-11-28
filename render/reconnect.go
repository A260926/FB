package render

import (
	"errors"
	"fmt"
	"math"
	"math/rand"
	"sync"

	"github.com/markphelps/go-trace/primitive"
)

// ProposalSource supplies reconnection proposals drawn from an old scene.
// The caller owns OldScene and Source; EstimateReconnect reads them through
// this interface and never mutates them.
type ProposalSource interface {
	ReconnectScene() *InstanceScene
	ReconnectSurface() SurfaceRef
}

// Source is a concrete ProposalSource.
type Source struct {
	Scene   *InstanceScene
	Surface SurfaceRef
}

// ReconnectScene implements ProposalSource.
func (s Source) ReconnectScene() *InstanceScene { return s.Scene }

// ReconnectSurface implements ProposalSource.
func (s Source) ReconnectSurface() SurfaceRef { return s.Surface }

// ReconnectStats is the read-only record of one EstimateReconnect call.
type ReconnectStats struct {
	// M is the fixed proposal budget: candidate count times the number of
	// sources (including the implicit current-target source).
	M int
	// Positive is the number of proposals with strictly positive weight.
	Positive int
	// SumWeight is the total streamed weight S (sum of K_i/qmix_i).
	SumWeight float64
	// Selected is the SurfaceRef chosen by the weighted reservoir, valid
	// only when Positive > 0.
	Selected SurfaceRef
	// QMix is the proposal mixture density at the selected surface point,
	// valid only when Positive > 0.
	QMix float64
	// Visible reports the current-frame first-hit visibility from target to
	// the selected point.
	Visible bool
	// ContinuationCalls is the number of real recursive Lambertian bounce
	// evaluations performed for surface transport (at most one).
	ContinuationCalls int
}

type reconnectParams struct {
	scene   *InstanceScene
	target  SurfaceRef
	sources []*InstanceScene
	srcRefs []SurfaceRef
	n       int
}

func validateReconnectParams(scene *InstanceScene, target SurfaceRef, proposals []ProposalSource, n int, rnd *rand.Rand) (*reconnectParams, error) {
	if scene == nil {
		return nil, errors.New("reconnect: nil current scene")
	}
	if rnd == nil {
		return nil, errors.New("reconnect: nil random source")
	}
	if n <= 0 {
		return nil, fmt.Errorf("reconnect: candidate count must be positive, got %d", n)
	}

	if _, err := scene.Surface(target); err != nil {
		return nil, fmt.Errorf("reconnect: invalid target: %w", err)
	}

	p := &reconnectParams{scene: scene, target: target, n: n}

	for i, proposal := range proposals {
		if proposal == nil {
			return nil, fmt.Errorf("reconnect: proposal source %d is nil", i)
		}
		srcScene := proposal.ReconnectScene()
		srcRef := proposal.ReconnectSurface()
		if srcScene == nil {
			return nil, fmt.Errorf("reconnect: proposal source %d has nil scene", i)
		}
		if _, err := srcScene.Surface(srcRef); err != nil {
			return nil, fmt.Errorf("reconnect: proposal source %d has invalid surface ref: %w", i, err)
		}
		p.sources = append(p.sources, srcScene)
		p.srcRefs = append(p.srcRefs, srcRef)
	}

	// The current target is always a source. M is N times the number of
	// sources including this automatic one; qmix divides by that same count,
	// so no extra source-count factor remains in the estimator.
	p.sources = append(p.sources, scene)
	p.srcRefs = append(p.srcRefs, target)

	return p, nil
}

func (p *reconnectParams) numSources() int { return len(p.sources) }

// cosineHemisphere draws a true cosine-weighted direction in the hemisphere
// around n. It uses the caller's rand.Rand and never falls back to the global
// source.
func cosineHemisphere(n primitive.Vector, rnd *rand.Rand) primitive.Vector {
	u1 := rnd.Float64()
	u2 := rnd.Float64()

	r := math.Sqrt(u1)
	phi := 2 * math.Pi * u2
	z := math.Sqrt(math.Max(0, 1-u1))

	local := primitive.Vector{X: r * math.Cos(phi), Y: r * math.Sin(phi), Z: z}

	// Build an orthonormal basis around n.
	var helper primitive.Vector
	if math.Abs(n.Z) < 0.9 {
		helper = primitive.Vector{Z: 1}
	} else {
		helper = primitive.Vector{X: 1}
	}
	t := n.Cross(helper).Normalize()
	b := n.Cross(t)

	return t.MultiplyScalar(local.X).Add(b.MultiplyScalar(local.Y)).Add(n.MultiplyScalar(local.Z)).Normalize()
}

// visible reports whether the segment from the current target to point is
// unobstructed in the current frame, using the same tMin exclusion for the
// origin and the same distance rule for the destination.
func visibleBetween(scene *InstanceScene, from primitive.Vector, to primitive.Vector, tMin float64) bool {
	d := to.Subtract(from)
	dist := d.Length()
	if !(dist > tMin) || !finite(dist) {
		return false
	}
	dir := d.DivideScalar(dist)
	limit := dist * (1 - 1e-9)
	if limit <= tMin {
		return false
	}
	hit, _ := scene.Hit(primitive.Ray{Origin: from, Direction: dir}, tMin, limit)
	return !hit
}

// currentMapping validates the candidate identity and returns the current
// world surface for a valid current-frame mapping.
func currentSurface(scene *InstanceScene, id uint64, dir primitive.Vector) (Surface, bool) {
	surf, err := scene.Surface(SurfaceRef{Instance: id, LocalNormal: dir})
	if err != nil {
		return Surface{}, false
	}
	return surf, true
}

// densityForSource recomputes the proposal density that source i would place
// on the current-frame point ys. The current point's SurfaceRef is mapped back
// into the source scene; if the source point does not see it as a first hit,
// the source density is zero. The result is converted from source-surface area
// to current-surface area by the ratio of the unit-sphere area factors.
func densityForSource(current, src *InstanceScene, srcRef, ysRef SurfaceRef, tMin float64) float64 {
	s, err := src.Surface(srcRef)
	if err != nil || !finiteVec(s.Point) {
		return 0
	}
	mapped, ok := src.MapSurface(ysRef)
	if !ok {
		return 0
	}
	v, err := src.Surface(mapped)
	if err != nil || !finiteVec(v.Point) {
		return 0
	}

	d := v.Point.Subtract(s.Point)
	dist := d.Length()
	if !(dist > tMin) || !finite(dist) {
		return 0
	}
	dir := d.DivideScalar(dist)

	// Endpoint tolerance: the point reconstructed from the candidate's
	// stored direction and the point reconstructed from the mapped reference
	// differ by a few ulps. Without slack the re-intersection lands just
	// before tMax and falsely reports a blocker at the candidate itself.
	limit := dist * (1 - 1e-9)
	if limit <= tMin {
		return 0
	}
	if hit, _ := src.Hit(primitive.Ray{Origin: s.Point, Direction: dir}, tMin, limit); hit {
		return 0
	}

	cosSource := s.Normal.Dot(dir)
	cosTarget := math.Abs(v.Normal.Dot(dir.MultiplyScalar(-1)))
	if !(cosSource > 0) {
		return 0
	}

	srcArea := src.areaFactor(mapped)
	curArea := current.areaFactor(ysRef)
	ratio := 1.0
	if srcArea > 0 && finite(srcArea) && curArea > 0 && finite(curArea) {
		ratio = srcArea / curArea
	}

	density := cosSource / math.Pi * cosTarget / (dist * dist) * ratio
	if !finite(density) || density < 0 {
		return 0
	}
	return density
}

// kernel returns the cosine sine-form (target kernel) between target x and
// current surface point ys.
func kernel(target Surface, ys Surface) float64 {
	d := ys.Point.Subtract(target.Point)
	dist := d.Length()
	if !(dist > 0) || !finite(dist) {
		return 0
	}
	w := d.DivideScalar(dist) // unit direction from the target x to ys
	cosX := target.Normal.Dot(w)
	cosY := math.Abs(ys.Normal.Dot(w.MultiplyScalar(-1)))
	if !(cosX > 0) {
		return 0
	}
	k := cosX / math.Pi * cosY / (dist * dist)
	if !finite(k) || k < 0 {
		return 0
	}
	return k
}

// EstimateReconnect estimates one-bounce indirect illumination at target in
// scene by reconnecting current-frame surfaces to surfaces sampled in the
// supplied old scenes. It returns linear RGB and read-only statistics.
func EstimateReconnect(scene *InstanceScene, target SurfaceRef, proposals []ProposalSource, n int, rnd *rand.Rand) (primitive.Color, ReconnectStats, error) {
	var stats ReconnectStats
	p, err := validateReconnectParams(scene, target, proposals, n, rnd)
	if err != nil {
		return primitive.Black, stats, err
	}

	targetSurf, err := scene.Surface(target)
	if err != nil {
		return primitive.Black, stats, err
	}

	numSources := p.numSources()
	stats.M = n * numSources

	sum := 0.0
	var selected Surface

	for i := 0; i < numSources; i++ {
		srcScene := p.sources[i]
		srcRef := p.srcRefs[i]
		srcSurf, err := srcScene.Surface(srcRef)
		if err != nil {
			continue
		}

		for k := 0; k < n; k++ {
			// Every source, including the automatic current-target source,
			// samples true cosine-weighted directions around its own surface
			// normal.
			dir := cosineHemisphere(srcSurf.Normal, rnd)

			hit, _, ref := srcScene.HitRef(primitive.Ray{Origin: srcSurf.Point, Direction: dir}, tMin, math.MaxFloat64)
			if !hit {
				continue
			}
			ys, ok := currentSurface(scene, ref.Instance, ref.LocalNormal)
			if !ok {
				continue
			}

			qmix := 0.0
			for j := 0; j < numSources; j++ {
				qmix += densityForSource(scene, p.sources[j], p.srcRefs[j], ys.Ref, tMin)
			}
			qmix /= float64(numSources)
			if !(qmix > 0) || !finite(qmix) {
				continue
			}

			kernelValue := kernel(targetSurf, ys)
			if !(kernelValue > 0) || !finite(kernelValue) {
				continue
			}
			w := kernelValue / qmix
			if !finite(w) || w <= 0 {
				continue
			}

			sum += w
			stats.Positive++
			if rnd.Float64()*sum < w {
				selected = ys
			}
		}
	}

	stats.SumWeight = sum

	surface := primitive.Black
	if stats.Positive > 0 && sum > 0 {
		stats.Selected = selected.Ref
		stats.QMix = 0
		for j := 0; j < numSources; j++ {
			stats.QMix += densityForSource(scene, p.sources[j], p.srcRefs[j], selected.Ref, tMin)
		}
		stats.QMix /= float64(numSources)

		stats.Visible = visibleBetween(scene, targetSurf.Point, selected.Point, tMin)
		if stats.Visible {
			ray := primitive.Ray{Origin: targetSurf.Point, Direction: selected.Point.Subtract(targetSurf.Point)}
			hit, record := scene.Hit(ray, tMin, math.MaxFloat64)
			if hit {
				bounced, bouncedRay := record.Bounce(ray, record, rnd)
				if bounced {
					stats.ContinuationCalls++
					continuation := color(bouncedRay, scene, rnd, 2)
					surface = record.Material.Color().Multiply(continuation)
				}
			}
		}
	}

	direct := primitive.Black
	// An independent target cosine ray estimates the direct sky contribution.
	// It does not feed the surface candidate pool, so background light is
	// neither lost nor double counted.
	skyDir := cosineHemisphere(targetSurf.Normal, rnd)
	if hit, _ := scene.Hit(primitive.Ray{Origin: targetSurf.Point, Direction: skyDir}, tMin, math.MaxFloat64); !hit {
		direct = primitive.Gradient(primitive.White, primitive.Blue, skyDir.Normalize().Y)
	}

	result := surface.MultiplyScalar(sum / float64(stats.M)).Multiply(targetSurf.Color)
	result = result.Add(direct.Multiply(targetSurf.Color))
	return result, stats, nil
}

// RenderReconnect renders a full frame using the reconnect estimator along
// true Camera.RayAt first hits. It returns a row-major linear RGB frame.
// Output depends only on the inputs, not on the worker count.
func RenderReconnect(scene *InstanceScene, camera *primitive.Camera, proposals []ProposalSource,
	width, height, samples, candidates, workers int, seed int64) ([][][3]float64, error) {

	if scene == nil {
		return nil, errors.New("render reconnect: nil scene")
	}
	if camera == nil {
		return nil, errors.New("render reconnect: nil camera")
	}
	if width <= 0 || height <= 0 {
		return nil, fmt.Errorf("render reconnect: size must be positive, got %dx%d", width, height)
	}
	if samples <= 0 {
		return nil, fmt.Errorf("render reconnect: sample count must be positive, got %d", samples)
	}
	if workers <= 0 {
		return nil, fmt.Errorf("render reconnect: worker count must be positive, got %d", workers)
	}
	for i, proposal := range proposals {
		if proposal == nil {
			return nil, fmt.Errorf("render reconnect: proposal source %d is nil", i)
		}
	}

	frame := make([][][3]float64, height)
	for y := range frame {
		frame[y] = make([][3]float64, width)
	}

	rows := make(chan int)
	var wg sync.WaitGroup
	var pixelErr error
	var errOnce sync.Once

	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for row := range rows {
				// Each row owns its RNG stream, so the frame is identical for
				// any worker count and any goroutine interleaving.
				rnd := rand.New(rand.NewSource(rowSeed(seed, row)))
				for col := 0; col < width; col++ {
					var scratch [3]float64
					for s := 0; s < samples; s++ {
						u := (float64(col) + rnd.Float64()) / float64(width)
						v := (float64(row) + 1 - rnd.Float64()) / float64(height)
						ray := camera.RayAt(u, v, rnd)
						rgb, err := reconnectPixel(scene, ray, proposals, candidates, rnd)
						if err != nil {
							errOnce.Do(func() { pixelErr = err })
							rgb = primitive.Black
						}
						scratch[0] += rgb.R
						scratch[1] += rgb.G
						scratch[2] += rgb.B
					}
					frame[row][col] = [3]float64{
						scratch[0] / float64(samples),
						scratch[1] / float64(samples),
						scratch[2] / float64(samples),
					}
				}
			}
		}()
	}

	for row := 0; row < height; row++ {
		rows <- row
	}
	close(rows)
	wg.Wait()
	if pixelErr != nil {
		return nil, pixelErr
	}

	return frame, nil
}

// rowSeed derives a per-row stream from the caller's seed. It uses only
// well-defined signed arithmetic.
func rowSeed(seed int64, row int) int64 {
	return seed*6364136223846793005 + int64(row)*1442695040888963407 + 1
}

func reconnectPixel(scene *InstanceScene, ray primitive.Ray, proposals []ProposalSource, candidates int, rnd *rand.Rand) (primitive.Color, error) {
	hit, _, ref := scene.HitRef(ray, tMin, math.MaxFloat64)
	if !hit {
		return primitive.Gradient(primitive.White, primitive.Blue, ray.Direction.Normalize().Y), nil
	}
	rgb, _, err := EstimateReconnect(scene, ref, proposals, candidates, rnd)
	return rgb, err
}

// compile-time assertion that InstanceScene satisfies primitive.Hitable.
var _ primitive.Hitable = (*InstanceScene)(nil)
