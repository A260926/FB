// Command cage demonstrates a triangle mesh bound to a tetrahedral control
// cage. The same mesh is bound twice and shown in two different cage poses in
// a single SVG.
//
// Usage: cage output.svg
package main

import (
	"fmt"
	"math"
	"os"

	"github.com/fogleman/ln/ln"
)

// plateMesh builds an n x n grid of triangles covering [-half,half]^2 at z=z.
func plateMesh(n int, half, z float64) *ln.Mesh {
	point := func(i, j int) ln.Vector {
		u := float64(i) / float64(n)
		v := float64(j) / float64(n)
		return ln.Vector{-half + 2*half*u, -half + 2*half*v, z}
	}
	var triangles []*ln.Triangle
	for i := 0; i < n; i++ {
		for j := 0; j < n; j++ {
			a := point(i, j)
			b := point(i+1, j)
			c := point(i+1, j+1)
			d := point(i, j+1)
			triangles = append(triangles, ln.NewTriangle(a, b, c))
			triangles = append(triangles, ln.NewTriangle(a, c, d))
		}
	}
	return ln.NewMesh(triangles)
}

func cubeCage() ([]ln.Vector, [][4]int) {
	rest := []ln.Vector{
		{-1, -1, -1}, {1, -1, -1}, {-1, 1, -1}, {1, 1, -1},
		{-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, 1, 1},
	}
	cells := [][4]int{
		{0, 1, 3, 7},
		{0, 1, 5, 7},
		{0, 2, 3, 7},
		{0, 2, 6, 7},
		{0, 4, 5, 7},
		{0, 4, 6, 7},
	}
	return rest, cells
}

func translate(points []ln.Vector, offset ln.Vector) []ln.Vector {
	out := make([]ln.Vector, len(points))
	for i, p := range points {
		out[i] = p.Add(offset)
	}
	return out
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: cage output.svg")
		os.Exit(2)
	}
	output := os.Args[1]

	mesh := plateMesh(12, 0.9, 0)
	rest, cells := cubeCage()

	// Both instances share the same source mesh and resting cage, but are
	// posed independently.
	left, err := ln.NewCageMesh(mesh, rest, cells)
	check(err)
	right, err := ln.NewCageMesh(mesh, rest, cells)
	check(err)

	// Left: rest pose, moved aside.
	check(left.Pose(translate(rest, ln.Vector{-1.6, 0, 0})))

	// Right: a folded pose. The near (-y) top edge is lifted a little, the
	// far (+y) top edge much more, so the cells bend the plate.
	posed := translate(rest, ln.Vector{1.6, 0, 0})
	posed[4].Z += 0.25 // (-x, -y, +z)
	posed[5].Z += 0.25 // (+x, -y, +z)
	posed[6].Z += 1.60 // (-x, +y, +z)
	posed[7].Z += 1.60 // (+x, +y, +z)
	check(right.Pose(posed))

	if left.BindingStats().TriangleCellClips != right.BindingStats().TriangleCellClips {
		check(fmt.Errorf("clip counts differ"))
	}

	rightPaths := right.Paths()
	if len(rightPaths) == 0 {
		check(fmt.Errorf("deformed surface produced no paths"))
	}

	// The deformed surface must actually lift and every path point must sit
	// inside the deformed bounding box.
	minZ, maxZ := math.Inf(1), math.Inf(-1)
	for _, path := range rightPaths {
		for _, v := range path {
			minZ = math.Min(minZ, v.Z)
			maxZ = math.Max(maxZ, v.Z)
		}
	}
	if maxZ-minZ < 0.1 {
		check(fmt.Errorf("deformed surface did not lift (z range %g)", maxZ-minZ))
	}
	box := right.BoundingBox()
	for _, path := range rightPaths {
		for _, v := range path {
			if !box.Contains(v) {
				check(fmt.Errorf("path point %v lies outside the reported bounding box %v", v, box))
			}
		}
	}
	if !(left.BoundingBox().Max.Z < 0.01) {
		check(fmt.Errorf("rest pose was unexpectedly moved in z: %v", left.BoundingBox()))
	}

	// Compare the two poses: the paths must correspond one-to-one, yet the
	// geometry must actually differ.
	leftPaths := left.Paths()
	if len(leftPaths) != len(rightPaths) {
		check(fmt.Errorf("path count differs: %d vs %d", len(leftPaths), len(rightPaths)))
	}
	offset := ln.Vector{1.6 - -1.6, 0, 0}
	diff := 0.0
	for i := range leftPaths {
		if len(leftPaths[i]) != len(rightPaths[i]) {
			check(fmt.Errorf("path %d length differs: %d vs %d", i, len(leftPaths[i]), len(rightPaths[i])))
			continue
		}
		for j := range leftPaths[i] {
			lv := leftPaths[i][j]
			rv := rightPaths[i][j].Sub(offset)
			diff = math.Max(diff, lv.Distance(rv))
		}
	}
	if diff < 0.05 {
		check(fmt.Errorf("the two cage poses are visually identical (max deviation %g)", diff))
	}

	// Each instance answers rays independently: a downward ray through each
	// plate's own local center hits that plate and reports it as the shape.
	for _, probe := range []struct {
		target ln.Shape
		x      float64
	}{
		{left, -1.6},
		{right, 1.6},
	} {
		ray := ln.Ray{
			Origin:    ln.Vector{probe.x, 0, 4},
			Direction: ln.Vector{0, 0, -1},
		}
		hit := probe.target.Intersect(ray)
		if !hit.Ok() {
			check(fmt.Errorf("ray through plate center at x=%g missed", probe.x))
		}
		if hit.Ok() && hit.Shape != probe.target {
			check(fmt.Errorf("ray at x=%g reports %T, want its own CageMesh", probe.x, hit.Shape))
		}
	}

	// Render both poses into one SVG.
	scene := &ln.Scene{}
	scene.Add(left)
	scene.Add(right)

	eye := ln.Vector{0, -7.5, 5.0}
	center := ln.Vector{0, 0.1, 0.3}
	up := ln.Vector{0, 0, 1}
	const width, height = 1200.0, 700.0
	paths := scene.Render(eye, center, up, width, height, 40, 0.1, 50, 0.03)
	if len(paths) == 0 {
		check(fmt.Errorf("render produced no paths"))
	}
	check(paths.WriteToSVG(output, width, height))

	fmt.Printf("wrote %s\n", output)
	fmt.Printf("paths: %d, clips: %d\n",
		len(rightPaths), right.BindingStats().TriangleCellClips)
	fmt.Printf("max deviation between poses: %.6f\n", diff)
	fmt.Printf("deformed surface z range: [%.4f, %.4f]\n", minZ, maxZ)
}

func check(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, "cage:", err)
		os.Exit(1)
	}
}
