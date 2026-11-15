// Command dynamic renders the same scene before and after an occluder is
// moved with Scene.ReplaceShape. The two frames are written side by side to
// the SVG path given as the first argument.
package main

import (
	"fmt"
	"os"

	"github.com/fogleman/ln/ln"
)

const (
	width  = 256.0
	height = 256.0
)

// buildScene creates the example scene. Slot 0 is the occluder that moves,
// slot 1 is the object it hides, and the remaining slots are small padding
// cubes that give the compiled tree more than one leaf.
func buildScene() *ln.Scene {
	scene := &ln.Scene{}
	scene.Add(ln.NewSphere(ln.Vector{X: 0, Y: 0, Z: 0}, 1.2))
	scene.Add(ln.NewSphere(ln.Vector{X: 0, Y: 0, Z: -4}, 1.2))
	for i := 0; i < 8; i++ {
		x := -14 + float64(i)*4
		scene.Add(ln.NewCube(
			ln.Vector{X: x, Y: 7.7, Z: -2.3},
			ln.Vector{X: x + 0.6, Y: 8.3, Z: -1.7},
		))
	}
	return scene
}

// renderFrame renders one frame of the example with the production API.
func renderFrame(scene *ln.Scene, eye ln.Vector) ln.Paths {
	return scene.Render(eye, ln.Vector{}, ln.Vector{Y: 1}, width, height, 45, 0.1, 100, 0.05)
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: dynamic <out.svg>")
		os.Exit(2)
	}
	scene := buildScene()
	eye := ln.Vector{X: 0, Y: 0, Z: 12}
	scene.Compile()

	// A point on the surface of the rear sphere, facing the camera.
	probe := ln.Vector{X: 0, Y: 0, Z: -2.8}
	before := renderFrame(scene, eye)
	visibleBefore := scene.Visible(eye, probe)

	if err := scene.ReplaceShape(0, ln.NewSphere(ln.Vector{X: 5, Y: 0, Z: 0}, 1.2)); err != nil {
		panic(err)
	}
	after := renderFrame(scene, eye)
	visibleAfter := scene.Visible(eye, probe)

	// Place the second frame to the right of the first.
	combined := append(ln.Paths{}, before...)
	combined = append(combined, after.Transform(ln.Translate(ln.Vector{X: width}))...)
	if err := combined.WriteToSVG(os.Args[1], width*2, height); err != nil {
		panic(err)
	}
	fmt.Printf("DYNAMIC visible_before=%v visible_after=%v paths_before=%d paths_after=%d\n",
		visibleBefore, visibleAfter, len(before), len(after))
}
