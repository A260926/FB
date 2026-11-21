// Command cage renders the same mesh bound to a tetrahedral control cage in
// two different poses, side by side, and verifies that the two poses produce
// different geometry. The first argument is the output SVG path.
package main

import (
	"fmt"
	"math"
	"os"

	"github.com/fogleman/ln/ln"
)

// sheetMesh returns a subdivided vertical sheet inside the beam cage.
func sheetMesh(nx, nz int) *ln.Mesh {
	var tris []*ln.Triangle
	for i := 0; i < nx; i++ {
		x0 := 0.2 + 0.6*float64(i)/float64(nx)
		x1 := 0.2 + 0.6*float64(i+1)/float64(nx)
		for j := 0; j < nz; j++ {
			z0 := 3 * float64(j) / float64(nz)
			z1 := 3 * float64(j+1) / float64(nz)
			a := ln.Vector{x0, 0.5, z0}
			b := ln.Vector{x1, 0.5, z0}
			c := ln.Vector{x1, 0.5, z1}
			d := ln.Vector{x0, 0.5, z1}
			tris = append(tris, ln.NewTriangle(a, b, c), ln.NewTriangle(a, c, d))
		}
	}
	return ln.NewMesh(tris)
}

// beamCage returns a 1x1x3 beam of three cubes, each split into six
// tetrahedra around the shared body diagonal so adjacent cubes stay
// conforming.
func beamCage() ([]ln.Vector, [][4]int) {
	var rest []ln.Vector
	idx := func(x, y, z int) int { return z*4 + y*2 + x }
	for z := 0; z <= 3; z++ {
		for y := 0; y <= 1; y++ {
			for x := 0; x <= 1; x++ {
				rest = append(rest, ln.Vector{float64(x), float64(y), float64(z)})
			}
		}
	}
	var cells [][4]int
	for z := 0; z < 3; z++ {
		a := idx(0, 0, z)
		b := idx(1, 0, z)
		c := idx(1, 1, z)
		d := idx(0, 1, z)
		a1 := idx(0, 0, z+1)
		b1 := idx(1, 0, z+1)
		c1 := idx(1, 1, z+1)
		d1 := idx(0, 1, z+1)
		cells = append(cells,
			[4]int{a, c1, b, c},
			[4]int{a, c1, c, d},
			[4]int{a, c1, d, d1},
			[4]int{a, c1, d1, a1},
			[4]int{a, c1, a1, b1},
			[4]int{a, c1, b1, b},
		)
	}
	return rest, cells
}

// bendPose shears the cage sideways by an amount growing quadratically with
// z, which bends the beam while keeping every cell volume positive.
func bendPose(rest []ln.Vector, amount float64) []ln.Vector {
	posed := make([]ln.Vector, len(rest))
	for i, p := range rest {
		posed[i] = ln.Vector{p.X + amount*p.Z*p.Z, p.Y, p.Z}
	}
	return posed
}

func pathLength(p ln.Path) float64 {
	total := 0.0
	for i := 0; i+1 < len(p); i++ {
		total += p[i].Distance(p[i+1])
	}
	return total
}

func pathsLength(paths ln.Paths) float64 {
	total := 0.0
	for _, p := range paths {
		total += pathLength(p)
	}
	return total
}

// pathsDifference measures how much two renders differ geometrically.
func pathsDifference(a, b ln.Paths) float64 {
	ba := a.BoundingBox()
	bb := b.BoundingBox()
	d := ba.Min.Distance(bb.Min) + ba.Max.Distance(bb.Max)
	return d + math.Abs(pathsLength(a)-pathsLength(b))
}

func fail(format string, args ...interface{}) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(1)
}

func main() {
	out := "cage.svg"
	if len(os.Args) > 1 {
		out = os.Args[1]
	}
	mesh := sheetMesh(4, 30)
	rest, cells := beamCage()
	cage, err := ln.NewCageMesh(mesh, rest, cells)
	if err != nil {
		fail("NewCageMesh: %v", err)
	}
	scene := &ln.Scene{}
	scene.Add(cage)
	eye := ln.Vector{4.5, -3.5, 1.5}
	center := ln.Vector{0.9, 0.5, 1.5}
	up := ln.Vector{0, 0, 1}
	render := func() ln.Paths {
		return scene.Render(eye, center, up, 400, 400, 35, 0.1, 50, 0.02)
	}
	// Pose A: gentle bend. The scene recompiles against the new pose.
	if err := cage.Pose(bendPose(rest, 0.03)); err != nil {
		fail("pose A: %v", err)
	}
	pathsA := render()
	// Pose B: strong bend, recomputed from the rest pose.
	if err := cage.Pose(bendPose(rest, 0.18)); err != nil {
		fail("pose B: %v", err)
	}
	pathsB := render()
	if len(pathsA) == 0 || len(pathsB) == 0 {
		fail("empty render: %d / %d paths", len(pathsA), len(pathsB))
	}
	diff := pathsDifference(pathsA, pathsB)
	if diff < 1 {
		fail("cage poses did not change the geometry (diff=%g)", diff)
	}
	offset := ln.Translate(ln.Vector{410, 0, 0})
	combined := append(pathsA, pathsB.Transform(offset)...)
	if err := combined.WriteToSVG(out, 820, 400); err != nil {
		fail("WriteToSVG: %v", err)
	}
	stats := cage.BindingStats()
	fmt.Printf("cage: cells=%d slices=%d clips=%d generation=%d diff=%.3f paths=%d+%d svg=%s\n",
		stats.Cells, stats.Slices, stats.TriangleCellClips,
		cage.Generation(), diff, len(pathsA), len(pathsB), out)
}
