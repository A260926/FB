// Command reconnect renders the same diffuse-sphere scene in two poses using
// the two-hop reconnection renderer. The second pose reuses the first pose's
// sampling distribution as an indirect-light proposal source.
package main

import (
	"fmt"
	"image"
	"image/color"
	"image/png"
	"math"
	"os"
	"path/filepath"

	"github.com/markphelps/go-trace/primitive"
	"github.com/markphelps/go-trace/render"
)

const (
	width      = 160
	height     = 120
	samples    = 6
	candidates = 4
	workers    = 4
	seed       = int64(20261129)
)

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: reconnect <output-directory>")
		os.Exit(2)
	}
	outDir := os.Args[1]
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		fmt.Fprintln(os.Stderr, "create output directory:", err)
		os.Exit(1)
	}

	beforeScene, beforeTarget, afterScene, _ := scenes()

	camera := primitive.NewCamera(
		primitive.Vector{X: 0.2, Y: 1.5, Z: 5.4},
		primitive.Vector{X: 0.2, Y: 0.1, Z: 0},
		45, float64(width)/float64(height), 0,
	)

	// The "before" frame has no old scene to draw from; the estimator falls
	// back to the automatic current-target proposal source.
	before, err := render.RenderReconnect(beforeScene, camera, nil, width, height, samples, candidates, workers, seed)
	if err != nil {
		fmt.Fprintln(os.Stderr, "render before:", err)
		os.Exit(1)
	}
	if err := writePNG(filepath.Join(outDir, "before.png"), before); err != nil {
		fmt.Fprintln(os.Stderr, "write before:", err)
		os.Exit(1)
	}

	// The "after" frame moves the spheres but reconnects to the surfaces that
	// were sampled in the old pose.
	after, err := render.RenderReconnect(afterScene, camera,
		[]render.ProposalSource{
			render.Source{Scene: beforeScene, Surface: beforeTarget},
		},
		width, height, samples, candidates, workers, seed+1)
	if err != nil {
		fmt.Fprintln(os.Stderr, "render after:", err)
		os.Exit(1)
	}
	if err := writePNG(filepath.Join(outDir, "after.png"), after); err != nil {
		fmt.Fprintln(os.Stderr, "write after:", err)
		os.Exit(1)
	}

	fmt.Printf("wrote %s and %s\n", filepath.Join(outDir, "before.png"), filepath.Join(outDir, "after.png"))
}

func scenes() (*render.InstanceScene, render.SurfaceRef, *render.InstanceScene, render.SurfaceRef) {
	prototypes := []render.SpherePrototype{
		{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.55, G: 0.25, B: 0.18}},
		{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.20, G: 0.42, B: 0.62}},
		{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.68, G: 0.62, B: 0.36}},
		{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.34, G: 0.58, B: 0.30}},
		{Center: primitive.Vector{}, Radius: 1, Color: primitive.Color{R: 0.62, G: 0.34, B: 0.58}},
	}

	// Pose A: front sphere, mid sphere behind it, two side spheres.
	beforeInstances := []render.InstanceDesc{
		{ID: 10, Prototype: 0, Matrix: affine(0.9, 0.9, 0.9, -1.55, 0.55, 0.6)},
		{ID: 11, Prototype: 1, Matrix: affine(0.8, 0.7, 0.8, 0.15, 0.45, -1.2)},
		{ID: 12, Prototype: 2, Matrix: affine(0.75, 0.75, 0.75, 1.7, 0.5, 0.2)},
		{ID: 13, Prototype: 3, Matrix: affine(0.6, 0.6, 0.6, 0.0, 0.9, -2.8)},
		{ID: 14, Prototype: 4, Matrix: affine(0.5, 0.5, 0.5, 2.4, 0.35, -1.6)},
	}

	// Pose B: move the side spheres and squash one, keeping IDs so surfaces
	// reconnect by identity and unit-sphere direction.
	afterInstances := []render.InstanceDesc{
		{ID: 10, Prototype: 0, Matrix: affine(0.9, 0.9, 0.9, -1.15, 0.55, 1.0)},
		{ID: 11, Prototype: 1, Matrix: affine(0.8, 0.7, 0.8, -0.05, 0.45, -1.35)},
		{ID: 12, Prototype: 2, Matrix: affine(0.55, 0.95, 0.7, 1.35, 0.5, 0.55)},
		{ID: 13, Prototype: 3, Matrix: affine(0.62, 0.62, 0.62, 0.35, 0.62, -2.4)},
		{ID: 14, Prototype: 4, Matrix: affine(0.5, 0.5, 0.5, 2.0, 0.35, -2.1)},
	}

	before, err := render.NewInstanceScene(prototypes, beforeInstances)
	if err != nil {
		panic(err)
	}
	after, err := render.NewInstanceScene(prototypes, afterInstances)
	if err != nil {
		panic(err)
	}

	beforeTarget := render.SurfaceRef{Instance: 10, LocalNormal: primitive.Vector{Z: 1}}
	afterTarget := render.SurfaceRef{Instance: 10, LocalNormal: primitive.Vector{Z: 1}}
	return before, beforeTarget, after, afterTarget
}

// affine builds a row-major 3x4 matrix with independent axis scales and a
// translation. Scale and translation are kept finite and non-zero.
func affine(sx, sy, sz, tx, ty, tz float64) [3][4]float64 {
	return [3][4]float64{
		{sx, 0, 0, tx},
		{0, sy, 0, ty},
		{0, 0, sz, tz},
	}
}

func writePNG(path string, frame [][][3]float64) error {
	h := len(frame)
	w := len(frame[0])
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			rgb := frame[y][x]
			img.SetNRGBA(x, h-y-1, color.NRGBA{
				R: channel(rgb[0]),
				G: channel(rgb[1]),
				B: channel(rgb[2]),
				A: 0xff,
			})
		}
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}

// channel applies the same square-root gamma used by primitive.Color.RGBA.
func channel(v float64) uint8 {
	if math.IsNaN(v) || v < 0 {
		v = 0
	}
	if v > 1 {
		v = 1
	}
	return uint8(math.Sqrt(v)*255 + 0.5)
}
