// Command portals renders two frames of a scene with two doors into one
// shared region. before.png shows both open doorways; between the two renders
// the left doorway is moved to the center and the right doorway is closed via
// PortalScene.ReplacePortals, producing after.png.
//
// Usage:
//
//	portals <output-directory>
package main

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/cameras"
	"github.com/markzuber/zgotrace/raytrace/hitables"
	"github.com/markzuber/zgotrace/raytrace/materials"
	"github.com/markzuber/zgotrace/raytrace/textures"
)

// identityFrame is a row-major frame with no rotation and the given
// translation.
func identityFrame(tx, ty, tz float64) [12]float64 {
	return [12]float64{
		1, 0, 0, tx,
		0, 1, 0, ty,
		0, 0, 1, tz,
	}
}

// flipFrame is rotated pi about the Y axis. Used as an Exit frame so the
// mandatory in-transport Y rotation and this frame cancel and the ray keeps
// travelling through the destination region along its original direction.
func flipFrame(tx, ty, tz float64) [12]float64 {
	return [12]float64{
		-1, 0, 0, tx,
		0, 1, 0, ty,
		0, 0, -1, tz,
	}
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: portals <output-directory>")
		os.Exit(1)
	}
	outputDir := os.Args[1]
	if err := os.MkdirAll(outputDir, 0o755); err != nil {
		fmt.Fprintf(os.Stderr, "cannot create output directory: %v\n", err)
		os.Exit(1)
	}

	const (
		doorHalfWidth  = 2.0
		doorHalfHeight = 3.0
		maxHops        = 2
	)

	// The shared destination region: one BVH, one emissive sphere, referenced
	// by both doors and never copied.
	emissive := materials.NewDiffuseLight(
		textures.NewColorTexture(raytrace.NewColorVector(2.0, 1.4, 0.7)))
	sharedSphere := hitables.NewSphere(mgl64.Vec3{0, 2, -4}, 3.0, emissive)
	sharedWorld := hitables.NewBvhNode([]raytrace.Hitable{sharedSphere}, 0, 1)
	sharedBackground := func(*raytrace.Ray) raytrace.ColorVector {
		return raytrace.NewColorVector(0.75, 0.55, 0.35)
	}

	roomBackground := func(*raytrace.Ray) raytrace.ColorVector {
		return raytrace.NewColorVector(0.02, 0.03, 0.08)
	}

	camera := cameras.NewNormalCamera(
		mgl64.Vec3{0, 2, 9},
		mgl64.Vec3{0, 2, 0},
		mgl64.Vec3{0, 1, 0},
		70.0, 1.0, 0.0, 10.0)

	regions := []raytrace.PortalRegion{
		// Region 0: the camera room, no solid geometry.
		{World: hitables.NewHitableList(nil), Background: roomBackground},
		// Region 1: the shared region reached through either door.
		{World: sharedWorld, Lights: nil, Background: sharedBackground},
	}

	// Two side-by-side entrances in the camera room, both opening onto the
	// same shared region at different exit positions.
	beforePortals := []raytrace.Portal{
		{
			From: 0, To: 1,
			Entry:      identityFrame(-3, 2, 0),
			Exit:       flipFrame(-2, 2, 0),
			HalfWidth:  doorHalfWidth,
			HalfHeight: doorHalfHeight,
			Enabled:    true,
		},
		{
			From: 0, To: 1,
			Entry:      identityFrame(3, 2, 0),
			Exit:       flipFrame(2, 2, 0),
			HalfWidth:  doorHalfWidth,
			HalfHeight: doorHalfHeight,
			Enabled:    true,
		},
	}

	scene, err := raytrace.NewPortalScene(camera, regions, 0, beforePortals, maxHops)
	if err != nil {
		fmt.Fprintf(os.Stderr, "cannot build portal scene: %v\n", err)
		os.Exit(1)
	}

	renderConfig := raytrace.NewRenderConfig(4, 1, false)
	tracer := raytrace.NewMonteCarloTracer()

	// Frame 1: both doors open, two bright doorways into the shared region.
	beforeBuffer := raytrace.NewPixelBuffer(160, 160)
	raytrace.NewRenderer(renderConfig).Render(tracer, beforeBuffer, scene, renderConfig)
	beforePath := filepath.Join(outputDir, "before.png")
	beforeBuffer.SavePng(beforePath)

	// Frame 2: move the left doorway to the center and close the right one.
	// Region and BVH identities are untouched; only the portal table changes.
	afterPortals := []raytrace.Portal{
		{
			From: 0, To: 1,
			Entry:      identityFrame(0, 2, 0),
			Exit:       flipFrame(0, 2, 0),
			HalfWidth:  doorHalfWidth,
			HalfHeight: doorHalfHeight,
			Enabled:    true,
		},
		{
			From: 0, To: 1,
			Entry:      identityFrame(3, 2, 0),
			Exit:       flipFrame(2, 2, 0),
			HalfWidth:  doorHalfWidth,
			HalfHeight: doorHalfHeight,
			Enabled:    false,
		},
	}
	if err := scene.ReplacePortals(afterPortals); err != nil {
		fmt.Fprintf(os.Stderr, "cannot replace portals: %v\n", err)
		os.Exit(1)
	}

	afterBuffer := raytrace.NewPixelBuffer(160, 160)
	raytrace.NewRenderer(renderConfig).Render(tracer, afterBuffer, scene, renderConfig)
	afterPath := filepath.Join(outputDir, "after.png")
	afterBuffer.SavePng(afterPath)

	fmt.Printf("wrote %s and %s\n", beforePath, afterPath)
}
