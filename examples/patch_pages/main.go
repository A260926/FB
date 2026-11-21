// Command patch_pages renders a two-frame before/after comparison of a
// page-resident Bezier micro-mesh.  It uses the real Renderer, the original
// sphere and diffuse-light materials, and the existing BVH; the second frame
// applies a control-net edit through ReplacePatch followed by Refit.
package main

import (
	"fmt"
	"math"
	"math/rand"
	"os"
	"path/filepath"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/cameras"
	"github.com/markzuber/zgotrace/raytrace/hitables"
	"github.com/markzuber/zgotrace/raytrace/materials"
	"github.com/markzuber/zgotrace/raytrace/textures"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

const (
	imageWidth  = 120
	imageHeight = 120
)

// makeSurface builds a curved patch with an optional central bulge.  The
// control net spans x in [-0.5,1.5] and y in [-0.2,1.4]; z arches upward.
func makeSurface(bulge float64) hitables.PatchDesc {
	var control [16]mgl64.Vec3
	for j := 0; j < 4; j++ {
		for i := 0; i < 4; i++ {
			u := float64(i) / 3.0
			v := float64(j) / 3.0
			arch := 0.30 * math.Sin(math.Pi*u) * math.Sin(math.Pi*v)
			dx := u - 0.5
			dy := v - 0.5
			dome := bulge * math.Exp(-((dx*dx + dy*dy) / 0.045))
			control[j*4+i] = mgl64.Vec3{
				2.0*u - 0.5,
				1.6*v - 0.2,
				arch + dome,
			}
		}
	}
	return hitables.PatchDesc{
		Control:  control,
		Material: materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(0.88, 0.42, 0.16))),
	}
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: patch_pages <output-dir>")
		os.Exit(2)
	}
	outDir := os.Args[1]
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		fmt.Fprintf(os.Stderr, "create output dir: %v\n", err)
		os.Exit(1)
	}

	rand.Seed(4242)

	beforeDesc := makeSurface(0.0)
	afterDesc := makeSurface(0.55)

	patchSet, err := hitables.NewPatchSet([]hitables.PatchDesc{beforeDesc}, 5, 12)
	if err != nil {
		fmt.Fprintf(os.Stderr, "NewPatchSet: %v\n", err)
		os.Exit(1)
	}

	sphereMat := materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(0.22, 0.52, 0.86)))
	sphere := hitables.NewSphere(mgl64.Vec3{-0.35, 0.32, 0.55}, 0.34, sphereMat)

	floorMat := materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(0.62, 0.62, 0.60)))
	floor := hitables.NewXzRect(-3.0, 3.0, -2.0, 3.0, -0.25, floorMat)

	lightMat := materials.NewDiffuseLight(textures.NewColorTexture(raytrace.NewColorVector(16.0, 15.0, 13.0)))
	light := hitables.NewSphere(mgl64.Vec3{0.65, 1.55, 1.45}, 0.30, lightMat)

	root := hitables.NewBvhNode([]raytrace.Hitable{patchSet, sphere, floor, light}, 0, 1)

	camera := cameras.NewNormalCamera(
		mgl64.Vec3{0.35, 0.70, 3.05},
		mgl64.Vec3{0.45, 0.55, 0.35},
		vectorextensions.UnitY(),
		40.0,
		float64(imageWidth)/float64(imageHeight),
		0.0,
		2.0)

	background := func(ray *raytrace.Ray) raytrace.ColorVector {
		// A gentle vertical gradient so the surface silhouette reads clearly.
		t := ray.Direction().Normalize().Y()
		if t < 0 {
			t = 0
		}
		return raytrace.NewColorVector(0.03+0.05*t, 0.04+0.07*t, 0.09+0.10*t)
	}

	config := raytrace.NewRenderConfig(6, 6, false)
	scene := raytrace.NewScene(camera, root, hitables.NewHitableList([]raytrace.Hitable{light}), background)

	tracer := raytrace.NewMonteCarloTracer()
	buffer := raytrace.NewPixelBuffer(imageWidth, imageHeight)
	renderer := raytrace.NewRenderer(config)

	renderer.Render(tracer, buffer, scene, config)
	if err := checkSurfaceVisible(buffer, "before"); err != nil {
		fmt.Fprintf(os.Stderr, "%v\n", err)
		os.Exit(1)
	}
	buffer.SavePng(filepath.Join(outDir, "before.png"))
	fmt.Printf("before.png stats: %+v\n", patchSet.Stats())

	// Between frames: edit the control net, then refit the original root.
	if err := patchSet.ReplacePatch(0, afterDesc); err != nil {
		fmt.Fprintf(os.Stderr, "ReplacePatch: %v\n", err)
		os.Exit(1)
	}
	root.Refit(0, 1)

	renderer = raytrace.NewRenderer(config)
	renderer.Render(tracer, buffer, scene, config)
	if err := checkSurfaceVisible(buffer, "after"); err != nil {
		fmt.Fprintf(os.Stderr, "%v\n", err)
		os.Exit(1)
	}
	buffer.SavePng(filepath.Join(outDir, "after.png"))
	fmt.Printf("after.png stats: %+v\n", patchSet.Stats())
}

// checkSurfaceVisible verifies that at least some pixels carry the patch's
// warm orange albedo rather than only the blue background and sphere.
func checkSurfaceVisible(buffer *raytrace.PixelBuffer, label string) error {
	surfacePixels := 0
	for y := 0; y < buffer.Height(); y++ {
		for x := 0; x < buffer.Width(); x++ {
			clr := buffer.GetPixelColor(x, y)
			if clr.R() > clr.G()+0.10 && clr.R() > 0.15 {
				surfacePixels++
			}
		}
	}
	if surfacePixels == 0 {
		return fmt.Errorf("%s: no surface pixels rendered", label)
	}
	fmt.Printf("%s: surface pixels=%d\n", label, surfacePixels)
	return nil
}
