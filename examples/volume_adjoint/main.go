// Command volume_adjoint renders two colored emissive media with the
// volume capture/transport pipeline and writes a beauty image plus the
// geometry and medium gradients of a simple loss.
//
// Usage: volume_adjoint <output-dir>
package main

import (
	"fmt"
	"math"
	"os"
	"path/filepath"

	f "github.com/fogleman/fauxgl"
)

func boxMesh(min, max f.Vector) f.VolumeMesh {
	p := []f.Vector{
		f.V(min.X, min.Y, min.Z),
		f.V(max.X, min.Y, min.Z),
		f.V(max.X, max.Y, min.Z),
		f.V(min.X, max.Y, min.Z),
		f.V(min.X, min.Y, max.Z),
		f.V(max.X, min.Y, max.Z),
		f.V(max.X, max.Y, max.Z),
		f.V(min.X, max.Y, max.Z),
	}
	faces := [][3]int{
		{0, 2, 1}, {0, 3, 2},
		{4, 5, 6}, {4, 6, 7},
		{0, 1, 5}, {0, 5, 4},
		{2, 3, 7}, {2, 7, 6},
		{0, 4, 7}, {0, 7, 3},
		{1, 2, 6}, {1, 6, 5},
	}
	return f.VolumeMesh{Positions: p, Faces: faces}
}

func writePPM(path string, img *f.VolumeImage) error {
	file, err := os.Create(path)
	if err != nil {
		return err
	}
	defer file.Close()
	fmt.Fprintf(file, "P6\n%d %d\n255\n", img.Width, img.Height)
	buf := make([]byte, 3*img.Width*img.Height)
	for i, c := range img.Color {
		for ch := 0; ch < 3; ch++ {
			v := c
			x := v.X
			if ch == 1 {
				x = v.Y
			} else if ch == 2 {
				x = v.Z
			}
			x = math.Pow(math.Max(x, 0), 1/2.2)
			if x > 1 {
				x = 1
			}
			buf[3*i+ch] = byte(x*255 + 0.5)
		}
	}
	_, err = file.Write(buf)
	return err
}

type gradientStats struct {
	rows               int
	vertexLo, vertexHi float64
	sigmaLo, sigmaHi   float64
	jLo, jHi           float64
}

func (s *gradientStats) add(kind string, v float64) {
	switch kind {
	case "vertex":
		s.vertexLo = math.Min(s.vertexLo, v)
		s.vertexHi = math.Max(s.vertexHi, v)
	case "sigma":
		s.sigmaLo = math.Min(s.sigmaLo, v)
		s.sigmaHi = math.Max(s.sigmaHi, v)
	case "j":
		s.jLo = math.Min(s.jLo, v)
		s.jHi = math.Max(s.jHi, v)
	}
}

func (s *gradientStats) complete() bool {
	return s.vertexLo < 0 && s.vertexHi > 0 &&
		s.sigmaLo < 0 && s.sigmaHi > 0 &&
		s.jLo < 0 && s.jHi > 0
}

func writeGradients(path string, grad *f.VolumeGradients) (*gradientStats, error) {
	file, err := os.Create(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	stats := &gradientStats{
		vertexLo: math.Inf(1), vertexHi: math.Inf(-1),
		sigmaLo: math.Inf(1), sigmaHi: math.Inf(-1),
		jLo: math.Inf(1), jHi: math.Inf(-1),
	}
	fmt.Fprintln(file, "kind,mesh,index,component,value")
	components := []string{"x", "y", "z"}
	channels := []string{"r", "g", "b"}
	value := func(v f.Vector, c int) float64 {
		switch c {
		case 0:
			return v.X
		case 1:
			return v.Y
		default:
			return v.Z
		}
	}
	for m, positions := range grad.Positions {
		for v, g := range positions {
			for c := 0; c < 3; c++ {
				x := value(g, c)
				fmt.Fprintf(file, "vertex,%d,%d,%s,%.9e\n", m, v, components[c], x)
				stats.add("vertex", x)
				stats.rows++
			}
		}
	}
	for m := range grad.Sigma {
		for c := 0; c < 3; c++ {
			x := value(grad.Sigma[m], c)
			fmt.Fprintf(file, "sigma,%d,,%s,%.9e\n", m, channels[c], x)
			stats.add("sigma", x)
			stats.rows++
		}
	}
	for m := range grad.J {
		for c := 0; c < 3; c++ {
			x := value(grad.J[m], c)
			fmt.Fprintf(file, "j,%d,,%s,%.9e\n", m, channels[c], x)
			stats.add("j", x)
			stats.rows++
		}
	}
	return stats, nil
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: volume_adjoint <output-dir>")
		os.Exit(2)
	}
	outDir := os.Args[1]
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		panic(err)
	}

	const width, height = 320, 240

	// one shared box mesh, two instances (one mirrored and sheared), two media
	mesh := boxMesh(f.V(-1, -1, -1), f.V(1, 1, 1))
	warm := f.VolumeMedium{
		Sigma: f.V(0.55, 0.30, 0.18),
		J:     f.V(0.85, 0.40, 0.12),
	}
	cool := f.VolumeMedium{
		Sigma: f.V(0.15, 0.35, 0.60),
		J:     f.V(0.05, 0.25, 0.70),
	}
	m0 := f.Identity().
		Rotate(f.V(0.3, 1, 0.15), 0.6).
		Scale(f.V(1.35, 0.85, 1.0)).
		Translate(f.V(-0.15, 0.05, 0.1))
	shear := f.Matrix{
		X00: 1, X01: 0.15, X02: 0.05, X03: 0,
		X10: 0.05, X11: 1, X12: 0.10, X13: 0,
		X20: 0.02, X21: 0.06, X22: 1, X23: 0,
		X30: 0, X31: 0, X32: 0, X33: 1,
	}
	m1 := f.Identity().Scale(f.V(0.9, 1.05, 0.8))
	m1 = shear.Mul(m1)
	m1 = m1.Rotate(f.V(0, 1, 0), -0.35)
	m1 = m1.Scale(f.V(-1, 1, 1))
	m1 = m1.Translate(f.V(0.55, -0.25, -0.15))
	scene := &f.VolumeScene{
		Meshes: []f.VolumeMesh{mesh},
		Media:  []f.VolumeMedium{warm, cool},
		Instances: []f.VolumeInstance{
			{Mesh: 0, Matrix: m0, Medium: 0},
			{Mesh: 0, Matrix: m1, Medium: 1},
		},
	}

	// camera and background: an opaque ground quad through the normal pipeline
	camera := f.LookAt(f.V(2.6, 1.9, 3.4), f.V(0, 0, 0), f.V(0, 1, 0)).
		Perspective(38, float64(width)/float64(height), 1, 9)
	dc := f.NewContext(width, height)
	dc.ClearColorBufferWith(f.Color{R: 0.07, G: 0.09, B: 0.12, A: 1})
	dc.Cull = f.CullNone
	dc.Shader = f.NewSolidColorShader(camera, f.Color{R: 0.42, G: 0.40, B: 0.36, A: 1})
	dc.DrawTriangles([]*f.Triangle{
		f.NewTriangleForPoints(f.V(-3, -1.05, -3), f.V(3, -1.05, -3), f.V(3, -1.05, 3)),
		f.NewTriangleForPoints(f.V(-3, -1.05, -3), f.V(3, -1.05, 3), f.V(-3, -1.05, 3)),
	})

	// volume capture and exact piecewise transport
	capture, err := f.CaptureVolume(dc, scene, camera)
	if err != nil {
		panic(err)
	}
	img, rec := f.TransportVolume(capture)

	// adjoint of a matching loss against a target render whose media
	// differ in opposite directions per instance; the gradient of
	// sum((C - Ctarget)^2 + (T - Ttarget)^2) then points from the
	// current media toward the target media with mixed signs
	targetScene := &f.VolumeScene{
		Meshes: scene.Meshes,
		Media: []f.VolumeMedium{
			{Sigma: warm.Sigma.MulScalar(1.2), J: warm.J.MulScalar(1.2)},
			{Sigma: cool.Sigma.MulScalar(0.8), J: cool.J.MulScalar(0.8)},
		},
		Instances: scene.Instances,
	}
	targetCapture, err := f.CaptureVolume(dc, targetScene, camera)
	if err != nil {
		panic(err)
	}
	targetImg, _ := f.TransportVolume(targetCapture)
	dColor := make([]f.Vector, width*height)
	dTrans := make([]f.Vector, width*height)
	for i := range dColor {
		dColor[i] = img.Color[i].Sub(targetImg.Color[i])
		dTrans[i] = img.Transmittance[i].Sub(targetImg.Transmittance[i])
	}
	grad, err := f.AdjointVolume(rec, dColor, dTrans)
	if err != nil {
		panic(err)
	}

	beautyPath := filepath.Join(outDir, "beauty.ppm")
	if err := writePPM(beautyPath, img); err != nil {
		panic(err)
	}
	gradPath := filepath.Join(outDir, "gradients.csv")
	stats, err := writeGradients(gradPath, grad)
	if err != nil {
		panic(err)
	}
	if !stats.complete() {
		panic(fmt.Sprintf("gradients.csv: need positive and negative geometry/medium gradients, got %+v", stats))
	}
	fmt.Printf("beauty: %dx%d -> %s\n", width, height, beautyPath)
	fmt.Printf("gradients: %d rows -> %s\n", stats.rows, gradPath)
	fmt.Printf("vertex gradient range: [%+.6e, %+.6e]\n", stats.vertexLo, stats.vertexHi)
	fmt.Printf("sigma gradient range:  [%+.6e, %+.6e]\n", stats.sigmaLo, stats.sigmaHi)
	fmt.Printf("emission gradient range: [%+.6e, %+.6e]\n", stats.jLo, stats.jHi)
}
