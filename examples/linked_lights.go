// linked_lights demonstrates per-instance light links.
//
// Two spheres share one source geometry: the left instance only receives the
// red light while the right instance only receives the blue light. The blue
// light is invisible to the camera yet still illuminates its linked objects,
// and a hidden cube casts a shadow only for the red light. Run it with:
//
//	go run examples/linked_lights.go output.ppm
//
// It writes a small ASCII P3 PPM by calling DefaultSampler.Sample once per
// pixel, using only the Go standard library.
package main

import (
	"fmt"
	"math"
	"math/rand"
	"os"
	"path/filepath"

	. "github.com/fogleman/pt/pt"
)

const (
	redBit  = uint64(1) << 0
	blueBit = uint64(1) << 1
)

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: go run examples/linked_lights.go output.ppm")
		os.Exit(2)
	}
	outputPath := os.Args[1]

	const width, height = 64, 48
	scene := buildScene()
	scene.Compile()

	camera := LookAt(V(0, -7, 2.6), V(0, 0, 1.4), V(0, 0, 1), 50)
	// direct-only, all lights, no soft shadows: deterministic per pixel
	sampler := NewDirectSampler()
	sampler.SoftShadows = false
	rnd := rand.New(rand.NewSource(20261116))

	pixels := make([]byte, 0, width*height*3)
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			ray := camera.CastRay(x, y, width, height, 0.5, 0.5, rnd)
			color := sampler.Sample(&scene, ray, rnd)
			pixels = append(pixels,
				toByte(color.R), toByte(color.G), toByte(color.B))
		}
	}

	if err := os.MkdirAll(filepath.Dir(outputPath), 0o755); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	file, err := os.Create(outputPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer file.Close()
	fmt.Fprintf(file, "P3\n%d %d\n255\n", width, height)
	for i := 0; i < len(pixels); i += 3 {
		fmt.Fprintf(file, "%d %d %d\n", pixels[i], pixels[i+1], pixels[i+2])
	}
}

func buildScene() Scene {
	scene := Scene{}

	// unwrapped floor receives every light
	scene.Add(NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(White)))

	// one shared sphere source, two independently linked instances
	sphere := NewSphere(V(0, 0, 0), 1, DiffuseMaterial(White))
	left := NewLinkedInstance(sphere, Translate(V(-2, 0, 1)),
		InstanceLinks{ReceiveMask: redBit, EmitMask: 0, CastMask: DefaultInstanceLinks().CastMask, Visible: true})
	right := NewLinkedInstance(sphere, Translate(V(2, 0, 1)),
		InstanceLinks{ReceiveMask: blueBit, EmitMask: 0, CastMask: DefaultInstanceLinks().CastMask, Visible: true})
	scene.Add(left)
	scene.Add(right)

	// visible red light and hidden blue light (hidden lights still shine)
	redLight := NewLinkedInstance(
		NewSphere(V(0, 0, 0), 0.4, LightMaterial(Color{1, 0.15, 0.1}, 6)),
		Translate(V(-2.4, 0, 4.2)),
		InstanceLinks{ReceiveMask: 0, EmitMask: redBit, CastMask: 0, Visible: true})
	blueLight := NewLinkedInstance(
		NewSphere(V(0, 0, 0), 0.4, LightMaterial(Color{0.2, 0.3, 1}, 6)),
		Translate(V(2.4, 0, 4.2)),
		InstanceLinks{ReceiveMask: 0, EmitMask: blueBit, CastMask: 0, Visible: false})
	scene.Add(redLight)
	scene.Add(blueLight)

	// hidden occluder that casts a shadow toward the red light only
	caster := NewLinkedInstance(
		NewCube(V(-0.35, -0.15, -0.35), V(0.35, 0.15, 0.35), DiffuseMaterial(White)),
		Translate(V(-2, 0, 2.4)),
		InstanceLinks{ReceiveMask: 0, EmitMask: 0, CastMask: redBit, Visible: false})
	scene.Add(caster)

	return scene
}

func toByte(x float64) byte {
	// simple gamma on the linear radiance so the lit regions stay visible
	v := math.Pow(math.Max(0, math.Min(1, x)), 1/2.2) * 255
	return byte(math.Round(v))
}
