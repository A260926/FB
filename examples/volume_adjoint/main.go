// Command volume_adjoint renders an off-screen participating-medium
// beauty image and the analytic gradients of an image-error objective
// through the exact piecewise volume transport and its reverse
// interface. It takes a single argument: the output directory.
//
// It writes beauty.ppm (linear RGB, display-tonemapped for byte output)
// and gradients.csv (shared source vertex XYZ gradients plus per-medium
// sigma and j gradients). No GPU, display or downloaded assets are used.
package main

import (
	"bufio"
	"fmt"
	"math"
	"os"
	"path/filepath"

	"github.com/fogleman/fauxgl"
)

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: volume_adjoint <output-directory>")
		os.Exit(2)
	}
	outDir := os.Args[1]
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	const w, h = 24, 24

	// two colored, emissive media
	media := []fauxgl.VolumeMedia{
		{Sigma: fauxgl.Color{R: 0.55, G: 0.25, B: 0.15, A: 0}, J: fauxgl.Color{R: 0.35, G: 0.12, B: 0.05, A: 0}},
		{Sigma: fauxgl.Color{R: 0.15, G: 0.35, B: 0.6, A: 0}, J: fauxgl.Color{R: 0.05, G: 0.2, B: 0.45, A: 0}},
	}

	cube := fauxgl.NewCube()
	view := fauxgl.LookAt(
		fauxgl.V(2.5, 1.8, 5), fauxgl.V(0, 0, 0), fauxgl.V(0, 1, 0)).
		Perspective(50, 1, 1, 30)

	scene := &fauxgl.VolumeScene{
		Meshes: []*fauxgl.Mesh{cube},
		Instances: []fauxgl.VolumeInstance{
			{Mesh: cube, Matrix: fauxgl.Translate(fauxgl.V(-0.55, 0, 0)), Media: 0},
			{Mesh: cube, Matrix: fauxgl.Translate(fauxgl.V(0.55, 0, 0)), Media: 1},
		},
		Media: media, ViewMatrix: view, Width: w, Height: h,
	}

	// fixed opaque backdrop snapshot from the context
	dc := fauxgl.NewContext(w, h)
	dc.ClearColorBufferWith(fauxgl.Color{R: 0.08, G: 0.09, B: 0.12, A: 1})
	for i := range dc.DepthBuffer {
		dc.DepthBuffer[i] = math.MaxFloat64
	}

	record, err := fauxgl.CaptureVolume(dc, scene)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	result, err := fauxgl.SolveVolume(record)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	if err := writePPM(filepath.Join(outDir, "beauty.ppm"), result.Color, w, h); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	// image-error adjoints with both signs: the left half pushes the
	// objective toward more light, the right half toward less, so the
	// reduced medium and geometry gradients span positive and negative.
	colorAdj := make([]fauxgl.Color, w*h)
	transAdj := make([]fauxgl.Color, w*h)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			i := y*w + x
			sign := 1.0
			if x >= w/2 {
				sign = -1.0
			}
			colorAdj[i] = fauxgl.Color{R: sign, G: 0.7 * sign, B: 0.3 * sign}
			transAdj[i] = fauxgl.Color{R: 0.2 * sign, G: 0.4 * sign, B: 0.6 * sign}
		}
	}

	grad, err := fauxgl.AdjointVolume(record, colorAdj, transAdj)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	n, err := writeGradients(filepath.Join(outDir, "gradients.csv"), grad)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	// confirm the deliverable contains both gradient signs
	var gPos, gNeg, mPos, mNeg bool
	for _, vs := range grad.Vertex {
		for _, v := range vs {
			if v.X > 0 || v.Y > 0 || v.Z > 0 {
				gPos = true
			}
			if v.X < 0 || v.Y < 0 || v.Z < 0 {
				gNeg = true
			}
		}
	}
	for _, m := range grad.Sigma {
		if m.R > 0 || m.G > 0 || m.B > 0 {
			mPos = true
		}
		if m.R < 0 || m.G < 0 || m.B < 0 {
			mNeg = true
		}
	}
	for _, m := range grad.J {
		if m.R > 0 || m.G > 0 || m.B > 0 {
			mPos = true
		}
		if m.R < 0 || m.G < 0 || m.B < 0 {
			mNeg = true
		}
	}
	if !(gPos && gNeg && mPos && mNeg) {
		fmt.Fprintf(os.Stderr, "gradients missing required signs: geom +/- %v/%v medium +/- %v/%v\n",
			gPos, gNeg, mPos, mNeg)
		os.Exit(1)
	}

	fmt.Printf("wrote %s and %s (%d gradient rows); geometry and medium gradients span +/- signs\n",
		filepath.Join(outDir, "beauty.ppm"), filepath.Join(outDir, "gradients.csv"), n)
}

// writePPM writes a binary P6 PPM. The transport result is kept in
// linear, unclamped floating point internally; only the byte display
// image is tonemapped and clamped, since PPM stores 8-bit channels.
func writePPM(path string, colors []fauxgl.Color, w, h int) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	bw := bufio.NewWriter(f)
	if _, err := fmt.Fprintf(bw, "P6\n%d %d\n255\n", w, h); err != nil {
		return err
	}
	const exposure = 1.0
	for _, c := range colors {
		buf := make([]byte, 3)
		for i, v := range []float64{c.R, c.G, c.B} {
			x := 1 - math.Exp(-v*exposure) // display-only tonemap
			if x < 0 {
				x = 0
			} else if x > 1 {
				x = 1
			}
			buf[i] = byte(math.Round(x * 255))
		}
		if _, err := bw.Write(buf); err != nil {
			return err
		}
	}
	return bw.Flush()
}

// writeGradients writes one row per scalar gradient. Layout:
// kind,mesh,index,component,value
// kind is vertex (component X/Y/Z), sigma or j (component R/G/B).
func writeGradients(path string, g *fauxgl.VolumeGradients) (int, error) {
	f, err := os.Create(path)
	if err != nil {
		return 0, err
	}
	defer f.Close()
	bw := bufio.NewWriter(f)
	if _, err := fmt.Fprintln(bw, "kind,mesh,index,component,value"); err != nil {
		return 0, err
	}
	rows := 0
	components := []string{"X", "Y", "Z"}
	for mi, vs := range g.Vertex {
		for vi, v := range vs {
			vals := []float64{v.X, v.Y, v.Z}
			for k, comp := range components {
				if _, err := fmt.Fprintf(bw, "vertex,%d,%d,%s,%.12g\n", mi, vi, comp, vals[k]); err != nil {
					return rows, err
				}
				rows++
			}
		}
	}
	channels := []string{"R", "G", "B"}
	for mi := range g.Sigma {
		s := []float64{g.Sigma[mi].R, g.Sigma[mi].G, g.Sigma[mi].B}
		j := []float64{g.J[mi].R, g.J[mi].G, g.J[mi].B}
		for k, comp := range channels {
			if _, err := fmt.Fprintf(bw, "sigma,%d,%d,%s,%.12g\n", mi, k, comp, s[k]); err != nil {
				return rows, err
			}
			rows++
			if _, err := fmt.Fprintf(bw, "j,%d,%d,%s,%.12g\n", mi, k, comp, j[k]); err != nil {
				return rows, err
			}
			rows++
		}
	}
	return rows, bw.Flush()
}
