package trace

import (
	"encoding/binary"
	"image"
	"math"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/peterstace/grayt/colour"
	"github.com/peterstace/grayt/scene"
	"github.com/peterstace/grayt/task_support/taskcheck"
	"github.com/peterstace/grayt/xmath"
)

var liveTestCamera = scene.Camera{
	Location:             xmath.Vect(0, 0, 0),
	LookingAt:            xmath.Vect(0, 0, -1),
	UpDirection:          xmath.Vect(0, 1, 0),
	FieldOfViewInRadians: math.Pi / 2,
	FocalLength:          1,
	FocalRatio:           1000,
	AspectWide:           1,
	AspectHigh:           1,
}

func identityTransform() [12]float64 {
	return [12]float64{
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
	}
}

func translationTransform(x, y, z float64) [12]float64 {
	return [12]float64{
		1, 0, 0, x,
		0, 1, 0, y,
		0, 0, 1, z,
	}
}

// glowingLiveScene returns a live scene whose single prototype is a huge
// emissive sphere surrounding the camera, so every camera ray hits it and
// every pixel takes on the given colour once a pass completes.
func glowingLiveScene(c colour.Colour) scene.LiveScene {
	return scene.LiveScene{
		Camera: liveTestCamera,
		Prototypes: []scene.Prototype{{
			Objects: []scene.Object{{
				Surface: scene.Surface{Spheres: []scene.Sphere{{
					Center: xmath.Vect(0, 0, 0),
					Radius: 100,
				}}},
				Material: scene.Material{Colour: c, Emittance: 1},
			}},
		}},
		Placements: []scene.Placement{{
			Prototype: 0,
			Transform: identityTransform(),
			Visible:   true,
		}},
	}
}

func glowingScene(c colour.Colour) func() scene.Scene {
	return func() scene.Scene {
		return scene.Scene{
			Camera: liveTestCamera,
			Objects: []scene.Object{{
				Surface: scene.Surface{Spheres: []scene.Sphere{{
					Center: xmath.Vect(0, 0, 0),
					Radius: 100,
				}}},
				Material: scene.Material{Colour: c, Emittance: 1},
			}},
		}
	}
}

func waitFor(c *taskcheck.C, what string, cond func() bool) {
	deadline := time.Now().Add(60 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			c.Check(false, "timed out waiting for %v", what)
			return
		}
		time.Sleep(time.Millisecond)
	}
}

// checkImage verifies that every pixel of the image satisfies ok.
func checkImage(c *taskcheck.C, img image.Image, ok func(r, g, b uint8) bool, msg string) {
	bounds := img.Bounds()
	bad := 0
	for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
		for x := bounds.Min.X; x < bounds.Max.X; x++ {
			r, g, b, _ := img.At(x, y).RGBA()
			if !ok(uint8(r>>8), uint8(g>>8), uint8(b>>8)) {
				bad++
			}
		}
	}
	c.Check(bad == 0, "%v (%d of %d pixels unexpected)", msg, bad, bounds.Dx()*bounds.Dy())
}

func allBlack(r, g, b uint8) bool { return r == 0 && g == 0 && b == 0 }

func colourPredicate(want colour.Colour) func(r, g, b uint8) bool {
	return func(r, g, b uint8) bool {
		return (want.R > 0) == (r > 0) && (want.G > 0) == (g > 0) && (want.B > 0) == (b > 0)
	}
}

// readAccumSnapshot reads the header and first aggregated colour of an
// accumulator state file, in the existing binary format.
func readAccumSnapshot(path string) (passes int64, r, g, b float64, err error) {
	f, err := os.Open(path)
	if err != nil {
		return 0, 0, 0, 0, err
	}
	defer f.Close()
	var wide, high int64
	for _, ptr := range []interface{}{&wide, &high, &passes, &r, &g, &b} {
		if err := binary.Read(f, binary.BigEndian, ptr); err != nil {
			return 0, 0, 0, 0, err
		}
	}
	return passes, r, g, b, nil
}

func TestLiveGeometry(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {

		// A prototype shared by two placements, one of which applies a
		// non-uniform scale and translation. The ray is chosen to graze the
		// ellipsoid at local point (0.6, 0, -0.8) so that both the world
		// distance and the inverse-transpose world normal are checked.
		sphereProto := scene.Prototype{Objects: []scene.Object{{
			Surface: scene.Surface{Spheres: []scene.Sphere{{
				Center: xmath.Vect(0, 0, 0),
				Radius: 1,
			}}},
			Material: scene.Material{Colour: colour.Colour{R: 1}, Emittance: 1},
		}}}
		scaled := [12]float64{
			2, 0, 0, 10,
			0, 3, 0, 20,
			0, 0, 4, 30,
		}
		_, objs, err := compileLiveScene(scene.LiveScene{
			Camera:     liveTestCamera,
			Prototypes: []scene.Prototype{sphereProto},
			Placements: []scene.Placement{
				{Prototype: 0, Transform: scaled, Visible: true},
				{Prototype: 0, Transform: translationTransform(0, 0, -10), Visible: true},
			},
		})
		c.Check(err == nil, "compile failed: %v", err)
		if err != nil {
			return
		}
		c.Check(len(objs) == 2, "expected 2 wrapped objects, got %d", len(objs))

		// Both placements wrap the same compiled local primitive.
		ts0, ok0 := objs[0].Surface.(*transformedSurface)
		ts1, ok1 := objs[1].Surface.(*transformedSurface)
		c.Check(ok0 && ok1, "placements must be wrapped in transformedSurface")
		if ok0 && ok1 {
			c.Check(ts0 != ts1, "each placement must get its own wrapper")
			c.Check(ts0.local == ts1.local, "placements of one prototype must share local primitives")
		}

		accel := accelForObjects(objs)
		ray := xmath.Ray{Start: xmath.Vect(11.2, 20, 21.8), Dir: xmath.Vect(0, 0, 1)}
		isect, _, hit := accel.closestHit(ray)
		c.Check(hit, "expected hit against transformed sphere")
		if hit {
			// The local ray direction is not normalised, so the distance
			// stays in the world ray's parameterisation.
			c.Check(math.Abs(isect.distance-5) < 1e-9,
				"world distance = %v, want 5", isect.distance)
			// The world normal is the inverse-transpose of the local
			// normal, renormalised.
			wantNorm := xmath.Vect(1.2, 0, -0.8).Unit()
			c.Check(isect.unitNormal.Sub(wantNorm).Length() < 1e-9,
				"world normal = %v, want %v", isect.unitNormal, wantNorm)
		}

		// The world bound of a rotated unit sphere covers all eight
		// transformed corners of the local bound.
		sin, cos := math.Sin(math.Pi/4), math.Cos(math.Pi/4)
		rotZ := [12]float64{
			cos, -sin, 0, 0,
			sin, cos, 0, 0,
			0, 0, 1, 0,
		}
		_, rotObjs, err := compileLiveScene(scene.LiveScene{
			Camera:     liveTestCamera,
			Prototypes: []scene.Prototype{sphereProto},
			Placements: []scene.Placement{{Prototype: 0, Transform: rotZ, Visible: true}},
		})
		c.Check(err == nil, "compile of rotated placement failed: %v", err)
		if err == nil && len(rotObjs) == 1 {
			minB, maxB := rotObjs[0].Surface.bound()
			sqrt2 := math.Sqrt2
			c.Check(math.Abs(minB.X+sqrt2) < 1e-6 && math.Abs(minB.Y+sqrt2) < 1e-6 &&
				math.Abs(minB.Z+1) < 1e-6,
				"rotated bound min = %v, want approx (%v, %v, -1)", minB, -sqrt2, -sqrt2)
			c.Check(math.Abs(maxB.X-sqrt2) < 1e-6 && math.Abs(maxB.Y-sqrt2) < 1e-6 &&
				math.Abs(maxB.Z-1) < 1e-6,
				"rotated bound max = %v, want approx (%v, %v, 1)", maxB, sqrt2, sqrt2)
		}

		// Hidden placements contribute no objects and are never hit, even
		// when a visible sibling of the same prototype is present.
		_, visObjs, err := compileLiveScene(scene.LiveScene{
			Camera:     liveTestCamera,
			Prototypes: []scene.Prototype{sphereProto},
			Placements: []scene.Placement{
				{Prototype: 0, Transform: translationTransform(0, 0, -5), Visible: true},
				{Prototype: 0, Transform: translationTransform(3, 0, -5), Visible: false},
			},
		})
		c.Check(err == nil, "compile with hidden placement failed: %v", err)
		c.Check(len(visObjs) == 1, "hidden placement produced objects: %d", len(visObjs))
		visAccel := accelForObjects(visObjs)
		_, _, hitVisible := visAccel.closestHit(xmath.Ray{
			Start: xmath.Vect(0, 0, 0), Dir: xmath.Vect(0, 0, -1)})
		c.Check(hitVisible, "visible placement should be hit")
		_, _, hitHidden := visAccel.closestHit(xmath.Ray{
			Start: xmath.Vect(3, 0, 0), Dir: xmath.Vect(0, 0, -1)})
		c.Check(!hitHidden, "hidden placement must not be hit")

		// All-hidden and empty scenes compile to a trivially missing
		// structure rather than an empty grid.
		_, hiddenObjs, err := compileLiveScene(scene.LiveScene{
			Camera:     liveTestCamera,
			Prototypes: []scene.Prototype{sphereProto},
			Placements: []scene.Placement{
				{Prototype: 0, Transform: identityTransform(), Visible: false},
			},
		})
		c.Check(err == nil, "compile of all-hidden scene failed: %v", err)
		c.Check(len(hiddenObjs) == 0, "all-hidden scene produced objects: %d", len(hiddenObjs))
		_, isList := accelForObjects(hiddenObjs).(listAccelerationStructure)
		c.Check(isList, "all-hidden scene must not be built into a grid")
		_, _, hitAny := accelForObjects(hiddenObjs).closestHit(xmath.Ray{
			Start: xmath.Vect(0, 0, 0), Dir: xmath.Vect(0, 0, -1)})
		c.Check(!hitAny, "all-hidden scene must miss every ray")

		_, emptyObjs, err := compileLiveScene(scene.LiveScene{Camera: liveTestCamera})
		c.Check(err == nil, "compile of empty scene failed: %v", err)
		c.Check(len(emptyObjs) == 0, "empty scene produced objects: %d", len(emptyObjs))
		_, isList = accelForObjects(emptyObjs).(listAccelerationStructure)
		c.Check(isList, "empty scene must not be built into a grid")

		// Every existing surface type compiles and intersects through a
		// placement transform.
		surfs := []struct {
			name     string
			surface  scene.Surface
			ray      xmath.Ray
			wantDist float64
		}{
			{"triangle", scene.Surface{Triangles: []scene.Triangle{{
				A: xmath.Vect(-1, -1, 0), B: xmath.Vect(1, -1, 0), C: xmath.Vect(0, 1, 0)}}},
				xmath.Ray{Start: xmath.Vect(5, -3, 7), Dir: xmath.Vect(0, 0, -1)}, 5},
			{"aligned_box", scene.Surface{AlignedBoxes: []scene.AlignedBox{{
				CornerA: xmath.Vect(-1, -1, -1), CornerB: xmath.Vect(1, 1, 1)}}},
				xmath.Ray{Start: xmath.Vect(5, -3, 7), Dir: xmath.Vect(0, 0, -1)}, 4},
			{"sphere", scene.Surface{Spheres: []scene.Sphere{{
				Center: xmath.Vect(0, 0, 0), Radius: 1}}},
				xmath.Ray{Start: xmath.Vect(5, -3, 7), Dir: xmath.Vect(0, 0, -1)}, 4},
			{"align_x_square", scene.Surface{AlignXSquares: []scene.AlignXSquare{{
				X: 0, Y1: -1, Y2: 1, Z1: -1, Z2: 1}}},
				xmath.Ray{Start: xmath.Vect(9, -3, 2), Dir: xmath.Vect(-1, 0, 0)}, 4},
			{"align_y_square", scene.Surface{AlignYSquares: []scene.AlignYSquare{{
				X1: -1, X2: 1, Y: 0, Z1: -1, Z2: 1}}},
				xmath.Ray{Start: xmath.Vect(5, 1, 2), Dir: xmath.Vect(0, -1, 0)}, 4},
			{"align_z_square", scene.Surface{AlignZSquares: []scene.AlignZSquare{{
				X1: -1, X2: 1, Y1: -1, Y2: 1, Z: 0}}},
				xmath.Ray{Start: xmath.Vect(5, -3, 7), Dir: xmath.Vect(0, 0, -1)}, 5},
			{"disc", scene.Surface{Discs: []scene.Disc{{
				Center: xmath.Vect(0, 0, 0), Radius: 1, UnitNorm: xmath.Vect(0, 0, 1)}}},
				xmath.Ray{Start: xmath.Vect(5, -3, 7), Dir: xmath.Vect(0, 0, -1)}, 5},
			{"pipe", scene.Surface{Pipes: []scene.Pipe{{
				EndpointA: xmath.Vect(-1, 0, 0), EndpointB: xmath.Vect(1, 0, 0), Radius: 0.5}}},
				xmath.Ray{Start: xmath.Vect(5, -3, 7), Dir: xmath.Vect(0, 0, -1)}, 4.5},
		}
		for _, tc := range surfs {
			_, objs, err := compileLiveScene(scene.LiveScene{
				Camera: liveTestCamera,
				Prototypes: []scene.Prototype{{Objects: []scene.Object{{
					Surface:  tc.surface,
					Material: scene.Material{Colour: colour.Colour{R: 1}, Emittance: 1},
				}}}},
				Placements: []scene.Placement{{
					Prototype: 0,
					Transform: translationTransform(5, -3, 2),
					Visible:   true,
				}},
			})
			c.Check(err == nil, "%v: compile failed: %v", tc.name, err)
			if err != nil || len(objs) != 1 {
				c.Check(len(objs) == 1, "%v: expected 1 object, got %d", tc.name, len(objs))
				continue
			}
			isect, _, hit := accelForObjects(objs).closestHit(tc.ray)
			c.Check(hit, "%v: expected hit", tc.name)
			if hit {
				c.Check(math.Abs(isect.distance-tc.wantDist) < 1e-9,
					"%v: distance = %v, want %v", tc.name, isect.distance, tc.wantDist)
			}
		}

		// Validation failures.
		cases := []struct {
			name   string
			mutate func(*scene.LiveScene)
		}{
			{"prototype index out of range", func(l *scene.LiveScene) {
				l.Placements[0].Prototype = 1
			}},
			{"negative prototype index", func(l *scene.LiveScene) {
				l.Placements[0].Prototype = -1
			}},
			{"non-finite transform", func(l *scene.LiveScene) {
				l.Placements[0].Transform[0] = math.NaN()
			}},
			{"infinite transform", func(l *scene.LiveScene) {
				l.Placements[0].Transform[5] = math.Inf(1)
			}},
			{"zero transform", func(l *scene.LiveScene) {
				l.Placements[0].Transform = [12]float64{}
			}},
			{"singular transform", func(l *scene.LiveScene) {
				l.Placements[0].Transform[0] = 0
			}},
			{"non-finite geometry", func(l *scene.LiveScene) {
				l.Prototypes[0].Objects[0].Surface.Spheres[0].Center.X = math.NaN()
			}},
			{"sphere radius zero", func(l *scene.LiveScene) {
				l.Prototypes[0].Objects[0].Surface.Spheres[0].Radius = 0
			}},
			{"sphere radius negative", func(l *scene.LiveScene) {
				l.Prototypes[0].Objects[0].Surface.Spheres[0].Radius = -2
			}},
			{"degenerate triangle", func(l *scene.LiveScene) {
				l.Prototypes[0].Objects[0].Surface.Triangles = []scene.Triangle{{
					A: xmath.Vect(0, 0, 0), B: xmath.Vect(1, 0, 0), C: xmath.Vect(2, 0, 0)}}
			}},
			{"pipe coincident endpoints", func(l *scene.LiveScene) {
				l.Prototypes[0].Objects[0].Surface.Pipes = []scene.Pipe{{
					EndpointA: xmath.Vect(1, 1, 1), EndpointB: xmath.Vect(1, 1, 1), Radius: 1}}
			}},
			{"box axis not increasing", func(l *scene.LiveScene) {
				l.Prototypes[0].Objects[0].Surface.AlignedBoxes = []scene.AlignedBox{{
					CornerA: xmath.Vect(0, 0, 0), CornerB: xmath.Vect(0, 1, 1)}}
			}},
			{"square interval not increasing", func(l *scene.LiveScene) {
				l.Prototypes[0].Objects[0].Surface.AlignXSquares = []scene.AlignXSquare{{
					X: 0, Y1: 1, Y2: 1, Z1: 0, Z2: 1}}
			}},
			{"disc radius zero", func(l *scene.LiveScene) {
				l.Prototypes[0].Objects[0].Surface.Discs = []scene.Disc{{
					Center: xmath.Vect(0, 0, 0), Radius: 0, UnitNorm: xmath.Vect(0, 0, 1)}}
			}},
			{"disc norm not unit", func(l *scene.LiveScene) {
				l.Prototypes[0].Objects[0].Surface.Discs = []scene.Disc{{
					Center: xmath.Vect(0, 0, 0), Radius: 1, UnitNorm: xmath.Vect(0, 0, 2)}}
			}},
			{"camera viewpoints coincide", func(l *scene.LiveScene) {
				l.Camera.LookingAt = l.Camera.Location
			}},
			{"camera up zero", func(l *scene.LiveScene) {
				l.Camera.UpDirection = xmath.Vect(0, 0, 0)
			}},
			{"camera up parallel", func(l *scene.LiveScene) {
				l.Camera.UpDirection = xmath.Vect(0, 0, -1)
			}},
			{"camera FOV zero", func(l *scene.LiveScene) {
				l.Camera.FieldOfViewInRadians = 0
			}},
			{"camera FOV pi", func(l *scene.LiveScene) {
				l.Camera.FieldOfViewInRadians = math.Pi
			}},
			{"camera focal length zero", func(l *scene.LiveScene) {
				l.Camera.FocalLength = 0
			}},
			{"camera focal ratio negative", func(l *scene.LiveScene) {
				l.Camera.FocalRatio = -1
			}},
			{"camera aspect zero", func(l *scene.LiveScene) {
				l.Camera.AspectWide = 0
			}},
			{"camera non-finite", func(l *scene.LiveScene) {
				l.Camera.Location.X = math.Inf(-1)
			}},
		}
		for _, tc := range cases {
			live := glowingLiveScene(colour.Colour{R: 1})
			tc.mutate(&live)
			_, _, err := compileLiveScene(live)
			c.Check(err != nil, "%v: expected error, got nil", tc.name)
		}

		// The unmutated control scene is valid.
		_, _, err = compileLiveScene(glowingLiveScene(colour.Colour{R: 1}))
		c.Check(err == nil, "control scene failed validation: %v", err)
	})
}

func TestLivePublish(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dim := xmath.Dimensions{Wide: 32, High: 32}
		np := dim.Wide * dim.High

		// --- A commit racing the startup load wins; the startup load must
		// not overwrite it. ---
		release := make(chan struct{})
		started := make(chan struct{})
		var once sync.Once
		dir := t.TempDir()
		inst := NewInstance(dim, func() scene.Scene {
			once.Do(func() { close(started) })
			<-release
			return glowingScene(colour.Colour{R: 1})()
		}, filepath.Join(dir, "accum.data"))
		defer inst.SetWorkers(0)
		inst.SetWorkers(1)
		<-started

		id1, err := inst.UpdateScene(glowingLiveScene(colour.Colour{G: 1}))
		c.Check(err == nil, "commit during startup load failed: %v", err)
		c.Check(id1 == 1, "first committed generation = %d, want 1", id1)
		stats := inst.GetStats()
		c.Check(stats.Generation == 1, "generation after commit = %d, want 1", stats.Generation)
		c.Check(stats.LoadState == "loaded", "load state after commit = %q, want loaded", stats.LoadState)

		close(release)
		waitFor(c, "generation 1 pass", func() bool { return inst.GetStats().Passes >= 1 })
		stats = inst.GetStats()
		c.Check(stats.Generation == 1, "startup load overwrote committed generation: %d", stats.Generation)
		c.Check(stats.Completed >= np, "completed = %d, want at least %d", stats.Completed, np)
		// The first pass really computes every pixel, including the first.
		checkImage(c, inst.Image(), colourPredicate(colour.Colour{G: 1}),
			"generation 1 image not fully green")

		// --- Concurrent commits get distinct generations in publish order. ---
		const n = 8
		ids := make([]uint64, n)
		errs := make([]error, n)
		var wg sync.WaitGroup
		for i := 0; i < n; i++ {
			wg.Add(1)
			go func(i int) {
				defer wg.Done()
				ids[i], errs[i] = inst.UpdateScene(glowingLiveScene(colour.Colour{R: 1, G: 1}))
			}(i)
		}
		wg.Wait()
		seen := map[uint64]bool{}
		maxID := uint64(0)
		for i := 0; i < n; i++ {
			c.Check(errs[i] == nil, "concurrent commit %d failed: %v", i, errs[i])
			c.Check(!seen[ids[i]], "duplicate generation id %d", ids[i])
			seen[ids[i]] = true
			if ids[i] > maxID {
				maxID = ids[i]
			}
		}
		c.Check(maxID == uint64(n+1), "highest generation = %d, want %d", maxID, n+1)
		c.Check(inst.GetStats().Generation == maxID,
			"current generation = %d, want last published %d", inst.GetStats().Generation, maxID)
		waitFor(c, "post-commit pass", func() bool { return inst.GetStats().Passes >= 1 })
		checkImage(c, inst.Image(), colourPredicate(colour.Colour{R: 1, G: 1}),
			"image after concurrent commits not fully yellow")

		// --- The published generation owns an independent copy of the
		// submitted data. ---
		aliased := glowingLiveScene(colour.Colour{B: 1})
		idA, err := inst.UpdateScene(aliased)
		c.Check(err == nil, "aliasing commit failed: %v", err)
		aliased.Prototypes[0].Objects[0].Surface.Spheres[0].Center = xmath.Vect(1e6, 1e6, 1e6)
		aliased.Prototypes[0].Objects[0].Material.Colour = colour.Colour{R: 1}
		aliased.Placements[0].Visible = false
		aliased.Placements[0].Transform[0] = 1e9
		aliased.Camera.Location = xmath.Vect(1e3, 0, 0)
		waitFor(c, "aliased generation pass", func() bool {
			s := inst.GetStats()
			return s.Generation == idA && s.Passes >= 1
		})
		checkImage(c, inst.Image(), colourPredicate(colour.Colour{B: 1}),
			"published generation affected by caller mutating inputs")

		// --- Failed commits change nothing. ---
		before := inst.GetStats()
		badScenes := []scene.LiveScene{
			func() scene.LiveScene {
				l := glowingLiveScene(colour.Colour{R: 1})
				l.Placements[0].Prototype = 7
				return l
			}(),
			func() scene.LiveScene {
				l := glowingLiveScene(colour.Colour{R: 1})
				l.Placements[0].Transform = [12]float64{}
				return l
			}(),
			func() scene.LiveScene {
				l := glowingLiveScene(colour.Colour{R: 1})
				l.Camera.FieldOfViewInRadians = 0
				return l
			}(),
			func() scene.LiveScene {
				l := glowingLiveScene(colour.Colour{R: 1})
				l.Prototypes[0].Objects[0].Surface.Spheres[0].Radius = -1
				return l
			}(),
		}
		for i, bad := range badScenes {
			_, err := inst.UpdateScene(bad)
			c.Check(err != nil, "bad scene %d: expected error, got nil", i)
		}
		after := inst.GetStats()
		c.Check(after.Generation == before.Generation,
			"failed commit changed generation: %d -> %d", before.Generation, after.Generation)
		c.Check(after.Passes >= before.Passes,
			"failed commit reset passes: %d -> %d", before.Passes, after.Passes)
		c.Check(after.Workers == 1, "failed commit changed workers: %d", after.Workers)
		checkImage(c, inst.Image(), colourPredicate(colour.Colour{B: 1}),
			"failed commit changed the image")

		// --- An all-hidden scene renders black rather than hitting an
		// empty grid. ---
		hidden := glowingLiveScene(colour.Colour{R: 1})
		hidden.Placements[0].Visible = false
		idH, err := inst.UpdateScene(hidden)
		c.Check(err == nil, "all-hidden commit failed: %v", err)
		waitFor(c, "all-hidden pass", func() bool {
			s := inst.GetStats()
			return s.Generation == idH && s.Passes >= 1
		})
		checkImage(c, inst.Image(), allBlack, "all-hidden scene not black")

		// --- A save belonging to a retired generation must not rename its
		// stale image over the accumulator file after a publish. ---
		dir2 := t.TempDir()
		accumPath := filepath.Join(dir2, "accum.data")
		instS := NewInstance(dim, glowingScene(colour.Colour{G: 1}), accumPath)
		defer instS.SetWorkers(0)
		instS.SetWorkers(1)
		waitFor(c, "save-race instance load", func() bool {
			return instS.GetStats().LoadState == "loaded"
		})
		waitFor(c, "first periodic save", func() bool {
			_, err := os.Stat(accumPath)
			return err == nil
		})
		pFile, _, gFile, bFile, err := readAccumSnapshot(accumPath)
		c.Check(err == nil, "could not read accum file: %v", err)
		c.Check(gFile > 0 && bFile == 0, "accum file does not contain green startup data")
		// Let generation 0 accumulate further passes so that a later save
		// would observably differ from what is on disk.
		waitFor(c, "further passes", func() bool {
			return int64(instS.GetStats().Passes) > pFile
		})

		entered := make(chan struct{})
		releaseSave := make(chan struct{})
		var hookOnce sync.Once
		hook := func() {
			hookOnce.Do(func() { close(entered) })
			<-releaseSave
		}
		saveAccumTestHook.fn.Store(&hook)
		saveDone := make(chan struct{})
		go func() {
			instS.saveAccum()
			close(saveDone)
		}()
		<-entered
		idB, err := instS.UpdateScene(glowingLiveScene(colour.Colour{B: 1}))
		c.Check(err == nil, "commit during blocked save failed: %v", err)
		close(releaseSave)
		<-saveDone
		saveAccumTestHook.fn.Store(nil)
		pAfter, rAfter, gAfter, bAfter, err := readAccumSnapshot(accumPath)
		c.Check(err == nil, "could not read accum file after blocked save: %v", err)
		c.Check(pAfter == pFile && rAfter == 0 && gAfter > 0 && bAfter == 0,
			"retired generation's save renamed over the accumulator file "+
				"(passes %d -> %d, colour (%v, %v, %v))",
			pFile, pAfter, rAfter, gAfter, bAfter)

		// A save taken after the publish persists the current generation.
		waitFor(c, "published generation pass", func() bool {
			s := instS.GetStats()
			return s.Generation == idB && s.Passes >= 1
		})
		instS.saveAccum()
		_, _, _, bSaved, err := readAccumSnapshot(accumPath)
		c.Check(err == nil, "could not read accum file after direct save: %v", err)
		c.Check(bSaved > 0, "current generation was not saved to the accumulator file")

		// --- The existing binary accumulator format remains readable by a
		// fresh instance, and a later update starts from a reset state. ---
		pSaved, _, _, _, err := readAccumSnapshot(accumPath)
		c.Check(err == nil, "could not read accum file before reload: %v", err)
		inst2 := NewInstance(dim, glowingScene(colour.Colour{R: 1}), accumPath)
		inst2.SetWorkers(1)
		waitFor(c, "reload instance load", func() bool {
			return inst2.GetStats().LoadState == "loaded"
		})
		c.Check(inst2.GetStats().Generation == 0,
			"reloaded instance generation = %d, want 0", inst2.GetStats().Generation)
		c.Check(int64(inst2.GetStats().Passes) >= pSaved,
			"reloaded instance passes = %d, want at least %d from the accum file",
			inst2.GetStats().Passes, pSaved)
		// Let the reloaded instance finish its first save (which writes
		// more than the reloaded passes) before the temporary directories
		// are cleaned up.
		inst2.SetWorkers(0)
		waitFor(c, "reload instance save", func() bool {
			p, _, _, _, err := readAccumSnapshot(accumPath)
			return err == nil && p > pSaved
		})
	})
}

func TestLiveWorkers(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dim := xmath.Dimensions{Wide: 32, High: 32}
		np := int64(dim.Wide * dim.High)
		dir := t.TempDir()
		inst := NewInstance(dim, glowingScene(colour.Colour{R: 1}), filepath.Join(dir, "accum.data"))
		defer inst.SetWorkers(0)
		inst.SetWorkers(1)
		waitFor(c, "initial load", func() bool {
			return inst.GetStats().LoadState == "loaded"
		})
		waitFor(c, "initial passes", func() bool { return inst.GetStats().Passes >= 1 })
		c.Check(inst.GetStats().Generation == 0,
			"startup generation = %d, want 0", inst.GetStats().Generation)

		// --- Pausing and resuming follows the next-pass scheduling rules,
		// and an update committed while paused resets the counters without
		// any tracing happening. ---
		inst.SetWorkers(0)
		waitFor(c, "in-flight pass to drain", func() bool {
			s := inst.GetStats()
			return int64(s.Completed) == int64(s.Passes)*np
		})
		id1, err := inst.UpdateScene(glowingLiveScene(colour.Colour{G: 1}))
		c.Check(err == nil, "update while paused failed: %v", err)
		c.Check(id1 == 1, "generation after paused update = %d, want 1", id1)
		stats := inst.GetStats()
		c.Check(stats.Generation == 1 && stats.Completed == 0 && stats.Passes == 0,
			"counters not reset on publish: %+v", stats)
		inst.SetWorkers(2)
		waitFor(c, "resumed pass", func() bool { return inst.GetStats().Passes >= 1 })
		stats = inst.GetStats()
		c.Check(stats.Workers == 2, "workers after resume = %d, want 2", stats.Workers)
		c.Check(int64(stats.Completed) >= np, "completed after resume = %d, want at least %d",
			stats.Completed, np)
		checkImage(c, inst.Image(), colourPredicate(colour.Colour{G: 1}),
			"resumed generation image not fully green")

		// --- An update returns even while the old generation's pass cannot
		// retire, and the retired pass never lands in the new generation. ---
		inst.cond.L.Lock()
		genGreen := inst.current
		inst.cond.L.Unlock()
		mergedPasses := genGreen.accum.getPasses()
		genGreen.accum.mu.Lock()
		// Wait for a full pass to be traced; its merge is blocked on the
		// accumulator lock held above.
		target := int64(mergedPasses+1) * np
		waitFor(c, "unmerged green pass", func() bool {
			return atomic.LoadInt64(&genGreen.completed) >= target
		})
		type updateResult struct {
			id  uint64
			err error
		}
		updateCh := make(chan updateResult, 1)
		go func() {
			id, err := inst.UpdateScene(glowingLiveScene(colour.Colour{B: 1}))
			updateCh <- updateResult{id, err}
		}()
		select {
		case res := <-updateCh:
			c.Check(res.err == nil && res.id == 2,
				"update while old paths blocked = (%d, %v), want (2, nil)", res.id, res.err)
		case <-time.After(30 * time.Second):
			c.Check(false, "update waited for blocked old paths")
			genGreen.accum.mu.Unlock()
			return
		}
		stats = inst.GetStats()
		c.Check(stats.Generation == 2 && stats.Completed == 0 && stats.Passes == 0,
			"counters not reset after blocked update: %+v", stats)
		// The blocked dispatcher cannot have started a pass of the new
		// generation, so the published image is still black.
		checkImage(c, inst.Image(), allBlack, "image not black immediately after publish")
		genGreen.accum.mu.Unlock()
		waitFor(c, "blue pass", func() bool { return inst.GetStats().Passes >= 1 })
		checkImage(c, inst.Image(), colourPredicate(colour.Colour{B: 1}),
			"retired pass leaked into the new generation")

		// --- Worker count changes apply from the next pass. ---
		inst.cond.L.Lock()
		genBlue := inst.current
		inst.cond.L.Unlock()
		p0 := genBlue.accum.getPasses()
		genBlue.accum.mu.Lock()
		target = int64(p0+1) * np
		waitFor(c, "unmerged blue pass", func() bool {
			return atomic.LoadInt64(&genBlue.completed) >= target
		})
		inst.SetWorkers(3)
		// The in-flight pass keeps the worker count it started with.
		c.Check(atomic.LoadInt64(&inst.actualWorkers) == 2,
			"worker count changed mid-pass: %d", atomic.LoadInt64(&inst.actualWorkers))
		genBlue.accum.mu.Unlock()
		waitFor(c, "worker count to apply", func() bool {
			return atomic.LoadInt64(&inst.actualWorkers) == 3
		})
		c.Check(inst.GetStats().Workers == 3,
			"workers = %d, want 3", inst.GetStats().Workers)
	})
}
