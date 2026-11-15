// Command live-scene demonstrates updating the scene of a running render
// instance without restarting it. It starts an instance rendering a startup
// scene, commits a live scene with two visible copies of one prototype, then
// commits a second live scene that moves one copy and hides the other, waits
// for the new generation to complete a pass, and writes the result to a PNG
// file named on the command line.
package main

import (
	"fmt"
	"image/png"
	"log"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"time"

	"github.com/peterstace/grayt/colour"
	"github.com/peterstace/grayt/scene"
	"github.com/peterstace/grayt/trace"
	"github.com/peterstace/grayt/xmath"
)

var camera = scene.Camera{
	Location:             xmath.Vect(0, 0, 0),
	LookingAt:            xmath.Vect(0, 0, -5),
	UpDirection:          xmath.Vect(0, 1, 0),
	FieldOfViewInRadians: math.Pi / 3,
	FocalLength:          1,
	FocalRatio:           100,
	AspectWide:           4,
	AspectHigh:           3,
}

func main() {
	if len(os.Args) != 2 {
		log.Fatalf("usage: %v <output.png>", os.Args[0])
	}
	outPath := os.Args[1]

	tmpDir, err := os.MkdirTemp("", "grayt-live-scene")
	if err != nil {
		log.Fatalf("could not create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	dim := xmath.Dimensions{Wide: 320, High: 240}
	inst := trace.NewInstance(dim, startupScene, filepath.Join(tmpDir, "accum.data"))
	inst.SetWorkers(runtime.NumCPU())

	waitFor("startup scene to load", func() bool {
		return inst.GetStats().LoadState == "loaded"
	})

	// Commit the first live scene: two visible copies of the same glowing
	// sphere prototype, placed either side of the view axis.
	gen1, err := inst.UpdateScene(liveScene(
		placementAt(-2, 0, -6, true),
		placementAt(+2, 0, -6, true),
	))
	if err != nil {
		log.Fatalf("could not commit first live scene: %v", err)
	}
	waitFor(fmt.Sprintf("generation %d to complete a pass", gen1), generationPassed(inst, gen1))

	// Commit the second live scene: the left copy moves to the centre and
	// the right copy is hidden. The instance keeps running throughout.
	gen2, err := inst.UpdateScene(liveScene(
		placementAt(0, 0, -5, true),
		placementAt(+2, 0, -6, false),
	))
	if err != nil {
		log.Fatalf("could not commit second live scene: %v", err)
	}
	waitFor(fmt.Sprintf("generation %d to complete a pass", gen2), generationPassed(inst, gen2))

	f, err := os.Create(outPath)
	if err != nil {
		log.Fatalf("could not create output file: %v", err)
	}
	defer f.Close()
	if err := png.Encode(f, inst.Image()); err != nil {
		log.Fatalf("could not encode image: %v", err)
	}
	stats := inst.GetStats()
	fmt.Printf("wrote %v (generation %v, %v passes, %v paths)\n",
		outPath, stats.Generation, stats.Passes, stats.Completed)
}

// startupScene is the generation 0 scene loaded when the instance starts.
func startupScene() scene.Scene {
	return scene.Scene{
		Camera: camera,
		Objects: []scene.Object{{
			Surface: scene.Surface{Spheres: []scene.Sphere{{
				Center: xmath.Vect(0, 0, -6),
				Radius: 1,
			}}},
			Material: scene.Material{
				Colour:    colour.Colour{R: 0.2, G: 0.2, B: 0.9},
				Emittance: 0.5,
			},
		}},
	}
}

// liveScene builds a live scene with a single glowing sphere prototype and
// the given placements of it.
func liveScene(placements ...scene.Placement) scene.LiveScene {
	return scene.LiveScene{
		Camera: camera,
		Prototypes: []scene.Prototype{{
			Objects: []scene.Object{{
				Surface: scene.Surface{Spheres: []scene.Sphere{{
					Center: xmath.Vect(0, 0, 0),
					Radius: 1,
				}}},
				Material: scene.Material{
					Colour:    colour.Colour{R: 1.0, G: 0.6, B: 0.2},
					Emittance: 4,
				},
			}},
		}},
		Placements: placements,
	}
}

// placementAt places the prototype with a translation transform (a row major
// 3x4 affine matrix, with the identity linear part written out explicitly).
func placementAt(x, y, z float64, visible bool) scene.Placement {
	return scene.Placement{
		Prototype: 0,
		Transform: [12]float64{
			1, 0, 0, x,
			0, 1, 0, y,
			0, 0, 1, z,
		},
		Visible: visible,
	}
}

func generationPassed(inst *trace.Instance, gen uint64) func() bool {
	return func() bool {
		stats := inst.GetStats()
		return stats.Generation == gen && stats.Passes >= 1
	}
}

func waitFor(what string, cond func() bool) {
	deadline := time.Now().Add(2 * time.Minute)
	for !cond() {
		if time.Now().After(deadline) {
			log.Fatalf("timed out waiting for %v", what)
		}
		time.Sleep(10 * time.Millisecond)
	}
}
