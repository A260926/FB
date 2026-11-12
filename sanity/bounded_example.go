//-----------------------------------------------------------------------------

//go:build ignore

/*

Bounded Dual Contouring sanity example.

Builds a sharp-edged part (a box with a transverse rib, rotated and moved far
from the origin), renders it with the box-constrained QEF dual contourer and
writes an STL, printing the resulting file path.

Run individually:

	go run ./sanity/bounded_example.go

*/
//-----------------------------------------------------------------------------

package main

import (
	"fmt"
	"math"
	"os"
	"path/filepath"

	"github.com/deadsy/sdfx/render"
	"github.com/deadsy/sdfx/render/dc"
	"github.com/deadsy/sdfx/sdf"
	v3 "github.com/deadsy/sdfx/vec/v3"
)

func main() {
	// Two sharp (round = 0) boxes: a base block and a rib across it. Their
	// union keeps genuine sharp edges that dual contouring is meant to retain.
	base, err := sdf.Box3D(v3.Vec{X: 4, Y: 2, Z: 1}, 0)
	if err != nil {
		panic(err)
	}
	rib, err := sdf.Box3D(v3.Vec{X: 1, Y: 2, Z: 3}, 0)
	if err != nil {
		panic(err)
	}
	part := sdf.Union3D(base, rib)

	// Rotate, then translate far from the origin: the bounded solver works in
	// voxel-local coordinates, so the result is unchanged by this placement.
	matrix := sdf.Translate3d(v3.Vec{X: 1e5, Y: -2e5, Z: 3e4}).
		Mul(sdf.RotateZ(0.6)).
		Mul(sdf.RotateX(0.35))
	part = sdf.Transform3D(part, matrix)

	// Validate the mesh through the public render.Render3 entry point.
	renderer := dc.NewDualContouringBounded(80)
	tris := render.CollectTriangles(part, renderer)
	if len(tris) == 0 {
		panic("bounded dual contouring produced no triangles")
	}
	for _, t := range tris {
		for _, p := range t {
			if math.IsNaN(p.X) || math.IsInf(p.X, 0) {
				panic("non-finite vertex in bounded dual contour mesh")
			}
		}
		if t.Degenerate(0) {
			panic("degenerate triangle in bounded dual contour mesh")
		}
	}
	if !render.IsWatertight(tris) {
		panic("bounded dual contour mesh is not watertight")
	}

	// Write the STL next to the OS temporary directory and print its path.
	path := filepath.Join(os.TempDir(), "sdfx_bounded_sharp_part.stl")
	render.ToSTL(part, path, dc.NewDualContouringBounded(80))
	if info, err := os.Stat(path); err != nil || info.Size() == 0 {
		panic(fmt.Sprintf("STL output missing or empty at %s: %v", path, err))
	}
	fmt.Printf("BOUNDED DC sharp-edged STL: triangles=%d watertight=true path=%s\n", len(tris), path)
}
