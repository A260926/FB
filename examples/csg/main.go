// CSG example: renders a perforated solid built entirely from analytic
// Boolean operations (no intermediate triangle mesh) and writes a small
// PPM image to .cache/csg.ppm.
package main

import (
	"fmt"
	"math"
	"math/rand"
	"os"
	"path/filepath"

	. "github.com/fogleman/pt/pt"
)

func main() {
	// outer block and the materials that identify each primitive
	blockMat := DiffuseMaterial(Color{0.85, 0.25, 0.15})
	tunnelMat := DiffuseMaterial(Color{0.15, 0.35, 0.85})
	block := NewCube(V(-1.5, -1.5, -1.5), V(1.5, 1.5, 1.5), blockMat)

	// three square tunnels: subtract the union of three thin cubes that
	// run through the block along the X, Y and Z axes
	bitX := NewCube(V(-3, -0.35, -0.35), V(3, 0.35, 0.35), tunnelMat)
	bitY := NewCube(V(-0.35, -3, -0.35), V(0.35, 3, 0.35), tunnelMat)
	bitZ := NewCube(V(-0.35, -0.35, -3), V(0.35, 0.35, 3), tunnelMat)
	bits, err := NewCSG(CSGUnion, bitX, bitY)
	if err != nil {
		panic(err)
	}
	bits, err = NewCSG(CSGUnion, bits, bitZ)
	if err != nil {
		panic(err)
	}
	body, err := NewCSG(CSGDifference, block, bits)
	if err != nil {
		panic(err)
	}
	// a second, smaller perforated sphere shows nested CSG and transformed
	// operands: a spherical shell with an off-center cavity
	shell, err := NewCSG(
		CSGDifference,
		NewTransformedShape(NewSphere(V(0, 0, 0), 0.7, DiffuseMaterial(Color{0.2, 0.7, 0.3})),
			Translate(V(-2.6, -0.9, 0.8))),
		NewTransformedShape(NewSphere(V(0.4, 0, 0), 0.45, tunnelMat),
			Translate(V(-2.6, -0.9, 0.8))),
	)
	if err != nil {
		panic(err)
	}

	scene := &Scene{Color: Color{0.04, 0.05, 0.08}}
	scene.Add(NewPlane(V(0, -1.5, 0), V(0, 1, 0), DiffuseMaterial(Color{0.8, 0.8, 0.8})))
	scene.Add(body)
	scene.Add(shell)
	scene.Add(NewSphere(V(3, 4, 3), 0.8, LightMaterial(White, 6)))
	scene.Add(NewSphere(V(-4, 2, -2), 0.6, LightMaterial(Color{0.7, 0.8, 1}, 4)))
	scene.Compile()

	camera := LookAt(V(4.2, 3.2, 4.8), V(0, 0, 0), V(0, 1, 0), 50)
	sampler := NewSampler(1, 3)
	width, height := 160, 120

	if err := os.MkdirAll(".cache", 0o755); err != nil {
		panic(err)
	}
	f, err := os.Create(filepath.FromSlash(".cache/csg.ppm"))
	if err != nil {
		panic(err)
	}
	defer f.Close()
	fmt.Fprintf(f, "P6\n%d %d\n255\n", width, height)

	rnd := rand.New(rand.NewSource(1234))
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			ray := camera.CastRay(x, y, width, height, 0.5, 0.5, rnd)
			c := sampler.Sample(scene, ray, rnd)
			// simple gamma correction and clamping for 8-bit PPM output
			write := func(v float64) byte {
				v = math.Pow(math.Min(math.Max(v, 0), 1), 1/2.2)
				return byte(v*255 + 0.5)
			}
			if _, err := f.Write([]byte{write(c.R), write(c.G), write(c.B)}); err != nil {
				panic(err)
			}
		}
	}
	fmt.Println("wrote .cache/csg.ppm")
}
