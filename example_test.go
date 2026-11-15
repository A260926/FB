package simplify_test

import (
	"fmt"
	"math"

	simplify "github.com/fogleman/simplify"
)

// ExampleSimplifyViews_budgets demonstrates view-budget simplification at
// three pixel budgets. The maximum pixel displacement is recomputed
// independently from the returned correspondence rather than trusted from
// the implementation.
//
// Scope: the guarantee covers the screen-space displacement of the
// original vertices only. It does not claim that every point on a face
// stays within budget, nor that visibility or occlusion are preserved.
func ExampleSimplifyViews_budgets() {
	const n = 17
	gridPoint := func(x, y int) simplify.Vector {
		xx := float64(x)/8 - 1
		yy := float64(y)/8 - 1
		return simplify.Vector{xx, yy, 0.15 * math.Sin(xx*2) * math.Cos(yy*3)}
	}
	var triangles []*simplify.Triangle
	for y := 0; y < n-1; y++ {
		for x := 0; x < n-1; x++ {
			a := gridPoint(x, y)
			b := gridPoint(x+1, y)
			c := gridPoint(x+1, y+1)
			d := gridPoint(x, y+1)
			triangles = append(triangles,
				simplify.NewTriangle(a, b, c),
				simplify.NewTriangle(a, c, d),
			)
		}
	}
	input := simplify.NewMesh(triangles)

	view := simplify.View{
		Matrix: [16]float64{
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1,
		},
		Width: 16, Height: 16,
	}

	project := func(p simplify.Vector) (float64, float64) {
		m := view.Matrix
		x := m[0]*p.X + m[1]*p.Y + m[2]*p.Z + m[3]
		y := m[4]*p.X + m[5]*p.Y + m[6]*p.Z + m[7]
		w := m[12]*p.X + m[13]*p.Y + m[14]*p.Z + m[15]
		return (x/w + 1) * view.Width / 2, (y/w + 1) * view.Height / 2
	}
	pixelDistance := func(a, b simplify.Vector) float64 {
		ax, ay := project(a)
		bx, by := project(b)
		return math.Hypot(ax-bx, ay-by)
	}

	fmt.Printf("input faces=%d; guarantee is per original vertex, not face coverage or occlusion\n",
		len(input.Triangles))
	for _, budget := range []float64{0, 0.5, 1} {
		result, err := simplify.SimplifyViews(input, 0.5, []simplify.View{view}, budget)
		if err != nil {
			fmt.Println("error:", err)
			return
		}
		maxDisplacement := 0.0
		for i, original := range result.Original {
			if d := pixelDistance(original, result.Representative[i]); d > maxDisplacement {
				maxDisplacement = d
			}
		}
		fmt.Printf("budget=%gpx faces=%3d max_vertex_pixel_displacement=%.4f\n",
			budget, len(result.Mesh.Triangles), maxDisplacement)
	}
	// Output:
	// input faces=512; guarantee is per original vertex, not face coverage or occlusion
	// budget=0px faces=512 max_vertex_pixel_displacement=0.0000
	// budget=0.5px faces=279 max_vertex_pixel_displacement=0.5000
	// budget=1px faces=262 max_vertex_pixel_displacement=1.0000
}
