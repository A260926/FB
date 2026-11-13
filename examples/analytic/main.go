// Analytic anti-aliasing example.
//
// It builds thin slivers and crossing colored slices entirely in memory, then
// renders the same geometry twice into 48x48 panels: with the legacy
// center-sampled rasterizer on the left and the exact analytic path on the
// right. The paired panels are written side by side to .cache/analytic.png.
//
// Vertex coordinates carry small, distinct jitter so no triangle edge is
// perfectly axis-aligned. The legacy forward-difference rasterizer divides by
// edge deltas that vanish for such edges; the analytic path itself has no
// such restriction.
package main

import (
	"os"

	"github.com/fogleman/fauxgl"
)

const panel = 48

// screenPoint converts a panel-space point (y down) to NDC coordinates.
func screenPoint(sx, sy, z float64) fauxgl.Vector {
	return fauxgl.Vector{(sx - panel/2) / (panel / 2), (panel/2 - sy) / (panel / 2), z}
}

func coloredTriangle(a, b, c fauxgl.Vector, col fauxgl.Color) *fauxgl.Triangle {
	return &fauxgl.Triangle{
		V1: fauxgl.Vertex{Position: a, Color: col},
		V2: fauxgl.Vertex{Position: b, Color: col},
		V3: fauxgl.Vertex{Position: c, Color: col},
	}
}

// buildScene returns slivers and overlapping crossing slices as NDC triangles.
func buildScene() []*fauxgl.Triangle {
	var tris []*fauxgl.Triangle

	// Thin slivers: about a third of a pixel wide, so center sampling drops
	// most of the coverage while the analytic path keeps partial pixels.
	sliverColors := []fauxgl.Color{
		{1, 1, 1, 1},
		{1, 0.9, 0.2, 1},
		{0.2, 0.9, 1, 1},
		{0.9, 0.3, 0.8, 1},
	}
	for i, col := range sliverColors {
		x := 4 + float64(i)*10
		y := 34 + float64(i)*2
		a := screenPoint(x+0.13, y+0.07, 0.2)
		b := screenPoint(x+0.47, y+0.21, 0.2)
		c := screenPoint(x+14.31, y-21.73, 0.2)
		tris = append(tris, coloredTriangle(a, b, c, col))
	}

	// Crossing color slices: two overlapping quads whose depth planes cross
	// inside the panel, so neither face is globally in front. Every corner is
	// jittered independently to avoid axis-aligned edges.
	red := fauxgl.Color{0.9, 0.15, 0.1, 1}
	blue := fauxgl.Color{0.1, 0.35, 0.95, 1}
	// Red slice: nearer on the left, farther on the right.
	r00 := screenPoint(5.87, 6.13, -0.6)
	r10 := screenPoint(42.21, 5.89, 0.6)
	r11 := screenPoint(41.77, 26.19, 0.6)
	r01 := screenPoint(6.23, 25.71, -0.6)
	tris = append(tris, coloredTriangle(r00, r10, r11, red))
	tris = append(tris, coloredTriangle(r00, r11, r01, red))
	// Blue slice: the opposite depth gradient, submitted second.
	b00 := screenPoint(6.19, 5.93, 0.6)
	b10 := screenPoint(41.83, 6.27, -0.6)
	b11 := screenPoint(42.13, 25.81, -0.6)
	b01 := screenPoint(5.91, 26.23, 0.6)
	tris = append(tris, coloredTriangle(b00, b10, b11, blue))
	tris = append(tris, coloredTriangle(b00, b11, b01, blue))

	return tris
}

func renderLegacy(tris []*fauxgl.Triangle) *fauxgl.Context {
	dc := fauxgl.NewContext(panel, panel)
	dc.ClearColorBufferWith(fauxgl.Black)
	dc.Cull = fauxgl.CullNone
	dc.AlphaBlend = false
	for _, t := range tris {
		dc.Shader = fauxgl.NewSolidColorShader(fauxgl.Identity(), t.V1.Color)
		dc.DrawTriangle(t)
	}
	return dc
}

func renderAnalytic(tris []*fauxgl.Triangle) *fauxgl.Context {
	dc := fauxgl.NewContext(panel, panel)
	dc.ClearColorBufferWith(fauxgl.Black)
	if err := dc.BeginAnalytic(); err != nil {
		panic(err)
	}
	if err := dc.DrawAnalyticTriangles(tris, fauxgl.Identity()); err != nil {
		panic(err)
	}
	if _, err := dc.ResolveAnalytic(); err != nil {
		panic(err)
	}
	return dc
}

func main() {
	tris := buildScene()
	legacy := renderLegacy(tris)
	analytic := renderAnalytic(tris)

	if err := os.MkdirAll(".cache", 0o755); err != nil {
		panic(err)
	}
	out := fauxgl.NewContext(2*panel, panel)
	copy(out.ColorBuffer.Pix[:panel*panel*4], legacy.ColorBuffer.Pix)
	copy(out.ColorBuffer.Pix[panel*panel*4:], analytic.ColorBuffer.Pix)
	if err := fauxgl.SavePNG(".cache/analytic.png", out.Image()); err != nil {
		panic(err)
	}
}
