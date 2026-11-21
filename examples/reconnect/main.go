// Command reconnect renders the same diffuse instance scene in two poses and
// writes before.png and after.png into the given output directory. The
// "after" frame reuses the "before" scene as a proposal source through the
// two-bounce reconnect estimator.
package main

import (
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"

	"github.com/markphelps/go-trace/primitive"
	"github.com/markphelps/go-trace/render"
)

func affine(m [3][3]float64, tx, ty, tz float64) [12]float64 {
	return [12]float64{
		m[0][0], m[0][1], m[0][2], tx,
		m[1][0], m[1][1], m[1][2], ty,
		m[2][0], m[2][1], m[2][2], tz,
	}
}

func uniform(s, tx, ty, tz float64) [12]float64 {
	return affine([3][3]float64{{s, 0, 0}, {0, s, 0}, {0, 0, s}}, tx, ty, tz)
}

func clamp01(v float64) float64 {
	switch {
	case v < 0:
		return 0
	case v > 1:
		return 1
	}
	return v
}

func writePNG(path string, frame []primitive.Color, width, height int) error {
	img := image.NewNRGBA(image.Rect(0, 0, width, height))
	for row := 0; row < height; row++ {
		for col := 0; col < width; col++ {
			c := frame[row*width+col]
			img.Set(col, row, primitive.Color{
				R: clamp01(c.R),
				G: clamp01(c.G),
				B: clamp01(c.B),
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

func run(dir string) error {
	protos := []render.SphereProto{
		{Center: primitive.Vector{}, Radius: 1, Albedo: primitive.Color{R: 0.72, G: 0.72, B: 0.72}}, // ground
		{Center: primitive.Vector{}, Radius: 1, Albedo: primitive.Color{R: 0.78, G: 0.25, B: 0.20}}, // red
		{Center: primitive.Vector{}, Radius: 1, Albedo: primitive.Color{R: 0.25, G: 0.72, B: 0.30}}, // green
		{Center: primitive.Vector{}, Radius: 1, Albedo: primitive.Color{R: 0.30, G: 0.42, B: 0.85}}, // blue
	}

	// Pose "before".
	before, err := render.NewInstanceScene(protos, []render.Instance{
		{ID: 1, Proto: 0, Matrix: affine([3][3]float64{{14, 0, 0}, {0, 0.6, 0}, {0, 0, 14}}, 0, -0.6, 0)},
		{ID: 2, Proto: 1, Matrix: uniform(1.0, -2.2, 1.05, 0.5)},
		{ID: 3, Proto: 2, Matrix: uniform(0.8, 0.6, 0.85, 2.2)},
		{ID: 4, Proto: 3, Matrix: uniform(1.4, 2.6, 1.45, -2.2)},
	})
	if err != nil {
		return err
	}

	// Pose "after": same IDs, moved and rescaled, one sheared.
	after, err := render.NewInstanceScene(protos, []render.Instance{
		{ID: 1, Proto: 0, Matrix: affine([3][3]float64{{14, 0, 0}, {0, 0.6, 0}, {0, 0, 14}}, 0, -0.6, 0)},
		{ID: 2, Proto: 1, Matrix: uniform(1.3, 2.0, 1.35, -1.2)},
		{ID: 3, Proto: 2, Matrix: uniform(0.95, -1.6, 1.0, 1.2)},
		{ID: 4, Proto: 3, Matrix: affine([3][3]float64{{1.1, 0.35, 0}, {0, 1.1, 0}, {0.1, 0, 1.1}}, 0.2, 1.45, -3.6)},
	})
	if err != nil {
		return err
	}

	const (
		width      = 96
		height     = 72
		samples    = 8
		candidates = 4
		workers    = 4
		seed       = 20261121
	)
	camera := primitive.NewCamera(
		primitive.Vector{X: 0, Y: 4.5, Z: 12},
		primitive.Vector{X: 0, Y: 0.8, Z: 0},
		45, float64(width)/float64(height), 0)

	frameBefore, err := render.RenderReconnect(before, camera, nil, width, height, samples, candidates, workers, seed)
	if err != nil {
		return err
	}
	frameAfter, err := render.RenderReconnect(after, camera, []*render.InstanceScene{before}, width, height, samples, candidates, workers, seed)
	if err != nil {
		return err
	}

	if err := writePNG(filepath.Join(dir, "before.png"), frameBefore, width, height); err != nil {
		return err
	}
	if err := writePNG(filepath.Join(dir, "after.png"), frameAfter, width, height); err != nil {
		return err
	}
	return nil
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: reconnect <output-dir>")
		os.Exit(1)
	}
	dir := os.Args[1]
	if err := os.MkdirAll(dir, 0o755); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
	if err := run(dir); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
	fmt.Printf("reconnect example: wrote %s and %s\n",
		filepath.Join(dir, "before.png"), filepath.Join(dir, "after.png"))
}
