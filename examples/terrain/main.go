// Command terrain renders an in-memory height field through the Terrain LOD
// pipeline: a coarse view, a fine view and a view after a height edit. It
// writes three PNGs next to the given output path (suffixed _coarse, _fine
// and _edited). It uses only the Go standard library plus fauxgl.
package main

import (
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"

	"github.com/fogleman/fauxgl"
)

func main() {
	out := "terrain.png"
	if len(os.Args) > 1 {
		out = os.Args[1]
	}

	// 1. build a 65x65 (k=6) in-memory height field
	const size = 65
	heights := make([]float64, size*size)
	for z := 0; z < size; z++ {
		for x := 0; x < size; x++ {
			fx, fz := float64(x)/(size-1), float64(z)/(size-1)
			heights[z*size+x] = 2*math.Sin(fx*2*math.Pi) + 1.5*math.Cos(fz*3*math.Pi)
		}
	}
	terrain, err := fauxgl.NewTerrain(size, heights)
	if err != nil {
		panic(err)
	}

	// two placements share the same sampled resource
	instances := []fauxgl.TerrainInstance{
		{Terrain: terrain, Origin: fauxgl.Vector{X: -32}, Scale: 1, Visible: true},
		{Terrain: terrain, Origin: fauxgl.Vector{X: 34, Z: 6}, Scale: 0.5, Visible: true},
	}

	render := func(eye fauxgl.Vector, splits []float64) *fauxgl.Context {
		matrix := fauxgl.Perspective(45, 1, 0.1, 400).
			Mul(fauxgl.LookAt(eye, fauxgl.Vector{X: 0, Y: 0, Z: 20}, fauxgl.Vector{X: 0, Y: 1}))
		plan, err := fauxgl.PrepareTerrain(instances, fauxgl.TerrainView{
			Eye: eye, Matrix: matrix, SplitDistances: splits,
		})
		if err != nil {
			panic(err)
		}
		dc := fauxgl.NewContext(320, 320)
		dc.Cull = fauxgl.CullNone
		dc.AlphaBlend = false
		dc.ClearColorBufferWith(fauxgl.HexColor("#101820"))
		shader := fauxgl.NewPhongShader(matrix,
			fauxgl.Vector{X: -0.4, Y: 1, Z: -0.3}.Normalize(), eye)
		dc.Shader = shader
		if _, err := fauxgl.DrawTerrain(plan, dc); err != nil {
			panic(err)
		}
		fmt.Printf("leaves=%d submitted vertices=%d triangles=%d revision=%d\n",
			len(plan.Leaves()), plan.DrawStats().Vertices, plan.DrawStats().Triangles,
			terrain.Revision())
		return dc
	}

	// 2a. coarse LOD: camera far away, small split distances
	coarse := render(fauxgl.Vector{X: 0, Y: 70, Z: 110},
		[]float64{6, 5, 4})
	// 2b. fine LOD: same field, camera close in, large split distances
	fine := render(fauxgl.Vector{X: 0, Y: 18, Z: 45},
		[]float64{120, 90, 60})

	// 3. edit the shared source data and render again
	if err := terrain.UpdateHeights([]fauxgl.HeightEdit{
		{X: 32, Z: 32, Height: 9},
		{X: 33, Z: 32, Height: 7},
	}); err != nil {
		panic(err)
	}
	edited := render(fauxgl.Vector{X: 0, Y: 18, Z: 45},
		[]float64{120, 90, 60})

	base := strings.TrimSuffix(out, filepath.Ext(out))
	for _, item := range []struct {
		name string
		dc   *fauxgl.Context
	}{
		{base + "_coarse.png", coarse},
		{base + "_fine.png", fine},
		{base + "_edited.png", edited},
	} {
		if err := fauxgl.SavePNG(item.name, item.dc.Image()); err != nil {
			panic(err)
		}
		fmt.Println("wrote", item.name)
	}
}
