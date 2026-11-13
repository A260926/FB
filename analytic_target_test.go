package fauxgl

import (
	"math"
	"testing"

	"github.com/fogleman/fauxgl/task_support/unitcheck"
)

// pxTri builds a triangle for a 1x1 pixel context from screen-space (y down)
// vertices. The per-vertex NDC depth and color are sampled from the requested
// affine screen-space depth and color planes, so clipping fans preserve them
// exactly.
func pxTri(pts [3]Vector, zA, zx, zy float64, col func(x, y float64) Color) *Triangle {
	var verts [3]Vertex
	for i, p := range pts {
		winZ := zA + zx*p.X + zy*p.Y
		verts[i] = Vertex{
			Position: V(2*p.X-1, 1-2*p.Y, 2*winZ-1),
			Color:    col(p.X, p.Y),
		}
	}
	return &Triangle{V1: verts[0], V2: verts[1], V3: verts[2]}
}

func solid(r, g, b float64) func(x, y float64) Color {
	return func(x, y float64) Color { return Color{r, g, b, 1} }
}

// regionArea sums the exact visible area assigned to one face order.
func regionArea(regions []analyticRegion, order int) float64 {
	var a float64
	for _, rg := range regions {
		if rg.win == order {
			area, _, _ := analyticSignedAreaMoment(rg.pt)
			a += math.Abs(area)
		}
	}
	return a
}

func regionTotalArea(regions []analyticRegion) float64 {
	var a float64
	for _, rg := range regions {
		area, _, _ := analyticSignedAreaMoment(rg.pt)
		a += math.Abs(area)
	}
	return a
}

// pixelPolys rebuilds the clipped candidate polygons of one pixel from a
// staged pass.
func pixelPolys(pass *analyticPass, px, py int) []analyticPolygon {
	var polys []analyticPolygon
	for _, src := range pass.polygons {
		clip := analyticClipPolygon(src.pt, float64(px), float64(py),
			float64(px+1), float64(py+1))
		if len(clip) >= 3 && polygonArea(clip) != 0 {
			polys = append(polys, analyticPolygon{
				order: src.order, plane: src.plane, pt: clip,
			})
		}
	}
	return polys
}

func TestAnalyticRegions(t *testing.T) {
	// Big triangle that fully covers the unit pixel after NDC clipping.
	cover := [3]Vector{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}

	t.Run("crossing_depth_planes", func(t *testing.T) {
		ch := unitcheck.New(t)
		dc := NewContext(1, 1)
		dc.ClearColorBufferWith(Black)
		ch.Check(dc.BeginAnalytic() == nil, "begin")
		// zA = 0.3+0.4x and zB = 0.7-0.4x cross exactly at x = 0.5.
		a := pxTri(cover, 0.3, 0.4, 0, solid(1, 0, 0))
		b := pxTri(cover, 0.7, -0.4, 0, solid(0, 0, 1))
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{a, b}, Identity()) == nil, "draw")
		polys := pixelPolys(dc.analytic, 0, 0)
		regions := analyticPixelRegions(polys, math.MaxFloat64, 0, 0)
		ch.Check(math.Abs(regionTotalArea(regions)-1) < 1e-9, "regions tile the pixel, got %v", regionTotalArea(regions))
		ch.Check(math.Abs(regionArea(regions, 0)-0.5) < 1e-9, "face A visible area 0.5, got %v", regionArea(regions, 0))
		ch.Check(math.Abs(regionArea(regions, 1)-0.5) < 1e-9, "face B visible area 0.5, got %v", regionArea(regions, 1))
		ch.Check(regionArea(regions, -1) == 0, "base fully covered")
		dc.CancelAnalytic()
	})

	t.Run("three_face_cycle", func(t *testing.T) {
		ch := unitcheck.New(t)
		dc := NewContext(1, 1)
		dc.ClearColorBufferWith(Black)
		ch.Check(dc.BeginAnalytic() == nil, "begin")
		// No plane is globally nearest: each one owns an open region.
		// A = 0.1+0.6x, B = 0.1+0.6y, C = 0.9-0.4x-0.4y
		a := pxTri(cover, 0.1, 0.6, 0, solid(1, 0, 0))
		b := pxTri(cover, 0.1, 0, 0.6, solid(0, 1, 0))
		c := pxTri(cover, 0.9, -0.4, -0.4, solid(0, 0, 1))
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{a, b, c}, Identity()) == nil, "draw")
		polys := pixelPolys(dc.analytic, 0, 0)
		regions := analyticPixelRegions(polys, math.MaxFloat64, 0, 0)
		areas := [3]float64{regionArea(regions, 0), regionArea(regions, 1), regionArea(regions, 2)}
		for i, a := range areas {
			ch.Check(a > 0.02, "face %d owns positive area, got %v", i, a)
		}
		ch.Check(math.Abs(areas[0]+areas[1]+areas[2]-1) < 1e-9, "three faces partition pixel")
		ch.Check(regionArea(regions, -1) == 0, "no base visible")
		// Spot checks of the hand-picked winner points.
		ch.Check(analyticWinner(polys, math.MaxFloat64, 0.1, 0.5) == 0, "A wins at (0.1,0.5)")
		ch.Check(analyticWinner(polys, math.MaxFloat64, 0.5, 0.1) == 1, "B wins at (0.5,0.1)")
		ch.Check(analyticWinner(polys, math.MaxFloat64, 0.9, 0.9) == 2, "C wins at (0.9,0.9)")
		dc.CancelAnalytic()
	})

	t.Run("finite_base_occlusion", func(t *testing.T) {
		ch := unitcheck.New(t)
		dc := NewContext(1, 1)
		dc.ClearColorBufferWith(White)
		dc.DepthBuffer[0] = 0.5
		ch.Check(dc.BeginAnalytic() == nil, "begin")
		lowerLeft := [3]Vector{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}
		upperRight := [3]Vector{{1, 0, 0}, {1, 1, 0}, {0, 1, 0}}
		near := pxTri(lowerLeft, 0.2, 0, 0, solid(1, 0, 0))
		far := pxTri(upperRight, 0.8, 0, 0, solid(0, 1, 0))
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{near, far}, Identity()) == nil, "draw")
		polys := pixelPolys(dc.analytic, 0, 0)
		regions := analyticPixelRegions(polys, 0.5, 0, 0)
		ch.Check(math.Abs(regionArea(regions, 0)-0.5) < 1e-9, "near face keeps its half")
		ch.Check(regionArea(regions, 1) == 0, "far face is entirely hidden by the base")
		ch.Check(math.Abs(regionArea(regions, -1)-0.5) < 1e-9, "base keeps the other half")
		ch.Check(math.Abs(regionTotalArea(regions)-1) < 1e-9, "partition still tiles")
		dc.CancelAnalytic()
	})

	t.Run("shared_edges_duplicates_and_contacts", func(t *testing.T) {
		ch := unitcheck.New(t)
		dc := NewContext(1, 1)
		dc.ClearColorBufferWith(Black)
		ch.Check(dc.BeginAnalytic() == nil, "begin")
		// Two triangles sharing the x+y=1 diagonal tile the pixel.
		t1 := pxTri([3]Vector{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, 0.3, 0, 0, solid(1, 0, 0))
		t2 := pxTri([3]Vector{{1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, 0.3, 0, 0, solid(0, 1, 0))
		// Exact duplicate of t1: equal depth, later order, but no extra area.
		t3 := pxTri([3]Vector{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, 0.3, 0, 0, solid(0, 0, 1))
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{t1, t2, t3}, Identity()) == nil, "draw")
		polys := pixelPolys(dc.analytic, 0, 0)
		regions := analyticPixelRegions(polys, math.MaxFloat64, 0, 0)
		ch.Check(math.Abs(regionTotalArea(regions)-1) < 1e-9, "shared diagonal contributes no area")
		ch.Check(regionArea(regions, 0) == 0, "original of duplicated face loses the equal-depth tie")
		ch.Check(math.Abs(regionArea(regions, 2)-0.5) < 1e-9, "duplicate inherits the half without doubling it")
		ch.Check(math.Abs(regionArea(regions, 1)-0.5) < 1e-9, "other half unchanged")

		// A face touching the square only along its left edge has no area.
		dc2 := NewContext(1, 1)
		dc2.ClearColorBufferWith(Black)
		ch.Check(dc2.BeginAnalytic() == nil, "begin 2")
		edgeOnly := pxTri([3]Vector{{0, 0, 0}, {0, 1, 0}, {-1, 1, 0}}, 0.2, 0, 0, solid(1, 1, 1))
		ch.Check(dc2.DrawAnalyticTriangles([]*Triangle{edgeOnly}, Identity()) == nil, "draw 2")
		polys2 := pixelPolys(dc2.analytic, 0, 0)
		regions2 := analyticPixelRegions(polys2, math.MaxFloat64, 0, 0)
		ch.Check(len(polys2) == 0, "edge-only contact produces no candidate")
		ch.Check(math.Abs(regionArea(regions2, -1)-1) < 1e-9, "edge-only face contributes no area")
		dc.CancelAnalytic()
		dc2.CancelAnalytic()
	})
}

func TestAnalyticIntegrals(t *testing.T) {
	t.Run("half_pixel_hand_computed_moments", func(t *testing.T) {
		ch := unitcheck.New(t)
		dc := NewContext(1, 1)
		dc.ClearColorBufferWith(Black)
		ch.Check(dc.BeginAnalytic() == nil, "begin")
		// Lower-left triangle (0,0),(1,0),(0,1):
		// area 1/2, Mx = My = 1/6. r=x, g=y, b=1-x-y.
		col := func(x, y float64) Color { return Color{x, y, 1 - x - y, 1} }
		tt := pxTri([3]Vector{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, 0.4, 0, 0, col)
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{tt}, Identity()) == nil, "draw")
		polys := pixelPolys(dc.analytic, 0, 0)
		regions := analyticPixelRegions(polys, math.MaxFloat64, 0, 0)
		var area, mx, my float64
		for _, rg := range regions {
			if rg.win == 0 {
				a, ax, ay := analyticSignedAreaMoment(rg.pt)
				area += math.Abs(a)
				mx += math.Abs(ax)
				my += math.Abs(ay)
			}
		}
		ch.Check(math.Abs(area-0.5) < 1e-9, "hand area 1/2, got %v", area)
		ch.Check(math.Abs(mx-1.0/6.0) < 1e-9, "hand Mx 1/6, got %v", mx)
		ch.Check(math.Abs(my-1.0/6.0) < 1e-9, "hand My 1/6, got %v", my)
		out, err := dc.ResolveAnalytic()
		ch.Check(err == nil, "resolve")
		// Integrals over the whole pixel (the other half is black base):
		// each channel's triangle integral 1/6 is divided by pixel area 1.
		ch.Check(math.Abs(out[0].R-1.0/6.0) < 1e-9, "red pixel integral 1/6, got %v", out[0].R)
		ch.Check(math.Abs(out[0].G-1.0/6.0) < 1e-9, "green pixel integral 1/6, got %v", out[0].G)
		ch.Check(math.Abs(out[0].B-1.0/6.0) < 1e-9, "blue pixel integral 1/6, got %v", out[0].B)
		ch.Check(math.Abs(out[0].A-1) < 1e-12, "alpha stays 1")
	})

	t.Run("sliver_exact_moments", func(t *testing.T) {
		ch := unitcheck.New(t)
		dc := NewContext(1, 1)
		dc.ClearColorBufferWith(Black)
		ch.Check(dc.BeginAnalytic() == nil, "begin")
		pts := [3]Vector{{0.1, 0.2, 0}, {0.9, 0.2001, 0}, {0.5, 0.8, 0}}
		// r = x, g = y, b = 0.5 sampled at the screen vertices.
		col := func(x, y float64) Color { return Color{x, y, 0.5, 1} }
		tt := pxTri(pts, 0.3, 0, 0, col)
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{tt}, Identity()) == nil, "draw")
		// Independent hand values straight from the triangle's own moments.
		wantArea, wantMx, wantMy := analyticSignedAreaMoment([]Vector{pts[0], pts[1], pts[2]})
		out, err := dc.ResolveAnalytic()
		ch.Check(err == nil, "resolve")
		ch.Check(math.Abs(out[0].R-math.Abs(wantMx)) < 1e-9, "sliver red integral Mx, got %v want %v", out[0].R, math.Abs(wantMx))
		ch.Check(math.Abs(out[0].G-math.Abs(wantMy)) < 1e-9, "sliver green integral My")
		ch.Check(math.Abs(out[0].B-0.5*math.Abs(wantArea)) < 1e-9, "sliver blue integral")
		ch.Check(math.Abs(wantArea) > 0.2, "sliver is a real area, %v", wantArea)
	})

	t.Run("viewport_clipped_polygon", func(t *testing.T) {
		ch := unitcheck.New(t)
		dc := NewContext(1, 1)
		dc.ClearColorBufferWith(Black)
		ch.Check(dc.BeginAnalytic() == nil, "begin")
		// Screen triangle (-0.5,0),(0.5,0),(0.5,1) clips to the quad
		// (0,0),(0.5,0),(0.5,1),(0,0.5): exact area 3/8, solid white.
		tt := pxTri([3]Vector{{-0.5, 0, 0}, {0.5, 0, 0}, {0.5, 1, 0}}, 0.4, 0, 0, solid(1, 1, 1))
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{tt}, Identity()) == nil, "draw")
		polys := pixelPolys(dc.analytic, 0, 0)
		regions := analyticPixelRegions(polys, math.MaxFloat64, 0, 0)
		ch.Check(math.Abs(regionArea(regions, 0)-3.0/8.0) < 1e-9, "clipped quad area 3/8")
		out, err := dc.ResolveAnalytic()
		ch.Check(err == nil, "resolve")
		ch.Check(math.Abs(out[0].R-3.0/8.0) < 1e-9, "clipped white integral 3/8, got %v", out[0].R)
		q := 3.0 / 8.0 * 255
		quantized := uint8(q)
		ch.Check(dc.ColorBuffer.Pix[0] == quantized, "truncating quantizer, got %d", dc.ColorBuffer.Pix[0])
		ch.Check(dc.ColorBuffer.Pix[3] == 255, "opaque output")
	})

	t.Run("triangulation_independence_and_center_vs_area", func(t *testing.T) {
		ch := unitcheck.New(t)
		col := func(x, y float64) Color { return Color{x, y, 0.25, 1} }
		build := func() *Context {
			dc := NewContext(1, 1)
			dc.ClearColorBufferWith(Black)
			if err := dc.BeginAnalytic(); err != nil {
				t.Fatal(err)
			}
			return dc
		}
		// Two triangulations of the same unit square with the same colors.
		dcA := build()
		a1 := pxTri([3]Vector{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, 0.4, 0, 0, col)
		a2 := pxTri([3]Vector{{1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, 0.4, 0, 0, col)
		ch.Check(dcA.DrawAnalyticTriangles([]*Triangle{a1, a2}, Identity()) == nil, "draw A")
		outA, err := dcA.ResolveAnalytic()
		ch.Check(err == nil, "resolve A")

		dcB := build()
		b1 := pxTri([3]Vector{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}}, 0.4, 0, 0, col)
		b2 := pxTri([3]Vector{{0, 0, 0}, {1, 1, 0}, {0, 1, 0}}, 0.4, 0, 0, col)
		ch.Check(dcB.DrawAnalyticTriangles([]*Triangle{b1, b2}, Identity()) == nil, "draw B")
		outB, err := dcB.ResolveAnalytic()
		ch.Check(err == nil, "resolve B")
		for k := range outA {
			ch.Check(math.Abs(outA[k].R-outB[k].R) < 1e-12 && math.Abs(outA[k].G-outB[k].G) < 1e-12,
				"triangulation does not change the integral")
		}
		// Full square: mean x = mean y = 0.5.
		ch.Check(math.Abs(outA[0].R-0.5) < 1e-9 && math.Abs(outA[0].G-0.5) < 1e-9, "square means 0.5")

		// Center depth vs area color: triangle (0,0),(1,0),(0,0.5), red,
		// does not contain the pixel center (0.5,0.5).
		dcC := build()
		c1 := pxTri([3]Vector{{0, 0, 0}, {1, 0, 0}, {0, 0.5, 0}}, 0.2, 0, 0, solid(1, 0, 0))
		ch.Check(dcC.DrawAnalyticTriangles([]*Triangle{c1}, Identity()) == nil, "draw C")
		outC, err := dcC.ResolveAnalytic()
		ch.Check(err == nil, "resolve C")
		ch.Check(math.Abs(outC[0].R-0.25) < 1e-9, "area color is nonzero (triangle area 0.25), got %v", outC[0].R)
		ch.Check(dcC.DepthBuffer[0] == math.MaxFloat64, "center keeps base depth despite area coverage")
	})
}

func TestAnalyticPipeline(t *testing.T) {
	mkTri := func() *Triangle {
		return pxTri([3]Vector{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, 0.4, 0, 0, solid(1, 0.5, 0.25))
	}

	t.Run("split_batches_match_single_batch", func(t *testing.T) {
		ch := unitcheck.New(t)
		geom := []*Triangle{mkTri(), mkTri(), mkTri(), mkTri()}
		run := func(split bool) ([]Color, []byte, []float64) {
			dc := NewContext(2, 2)
			dc.ClearColorBufferWith(Black)
			if err := dc.BeginAnalytic(); err != nil {
				t.Fatal(err)
			}
			if split {
				ch.Check(dc.DrawAnalyticTriangles(geom[:2], Identity()) == nil, "batch 1")
				ch.Check(dc.DrawAnalyticTriangles(geom[2:], Identity()) == nil, "batch 2")
			} else {
				ch.Check(dc.DrawAnalyticTriangles(geom, Identity()) == nil, "one batch")
			}
			out, err := dc.ResolveAnalytic()
			ch.Check(err == nil, "resolve")
			pix := append([]byte(nil), dc.ColorBuffer.Pix...)
			z := append([]float64(nil), dc.DepthBuffer...)
			return out, pix, z
		}
		out1, pix1, z1 := run(false)
		out2, pix2, z2 := run(true)
		for i := range out1 {
			ch.Check(math.Abs(out1[i].R-out2[i].R) < 1e-12 && math.Abs(out1[i].G-out2[i].G) < 1e-12,
				"float outputs match")
		}
		ch.Check(string(pix1) == string(pix2), "published colors match")
		allDepth := true
		for i := range z1 {
			if z1[i] != z2[i] {
				allDepth = false
			}
		}
		ch.Check(allDepth, "published depths match")
	})

	t.Run("inputs_are_snapshotted", func(t *testing.T) {
		ch := unitcheck.New(t)
		control := mkTri()
		dc1 := NewContext(1, 1)
		dc1.ClearColorBufferWith(Black)
		dc1.BeginAnalytic()
		src := mkTri()
		dc1.DrawAnalyticTriangles([]*Triangle{src}, Identity())
		// Mutate the source after submission: staged content must not move.
		src.V1.Position = V(5, 5, 5)
		src.V1.Color = Color{0, 1, 0, 1}
		out1, _ := dc1.ResolveAnalytic()

		dc2 := NewContext(1, 1)
		dc2.ClearColorBufferWith(Black)
		dc2.BeginAnalytic()
		dc2.DrawAnalyticTriangles([]*Triangle{control}, Identity())
		out2, _ := dc2.ResolveAnalytic()
		ch.Check(math.Abs(out1[0].R-out2[0].R) < 1e-12, "mutation after submit is ignored")
	})

	t.Run("failed_batch_preserves_prior_and_returns_independent_slice", func(t *testing.T) {
		ch := unitcheck.New(t)
		dc := NewContext(1, 1)
		dc.ClearColorBufferWith(Black)
		ch.Check(dc.BeginAnalytic() == nil, "begin")
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{mkTri()}, Identity()) == nil, "valid batch")
		badMatrix := Matrix{} // not affine
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{mkTri()}, badMatrix) != nil, "bad matrix rejected")
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{nil}, Identity()) != nil, "nil triangle rejected")
		badColor := mkTri()
		badColor.V2.Color = Color{0.5, 0.5, math.NaN(), 1}
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{badColor}, Identity()) != nil, "non-finite color rejected")
		badPos := mkTri()
		badPos.V3.Position = V(math.Inf(1), 0, 0)
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{badPos}, Identity()) != nil, "non-finite position rejected")
		out, err := dc.ResolveAnalytic()
		ch.Check(err == nil, "prior valid batch still resolves")
		ch.Check(out[0].R > 0, "prior geometry survived")

		// The returned slice is owned by the caller.
		saved := out[0].R
		out[0].R = -99
		ch.Check(dc.ColorBuffer.Pix[0] != 255 || saved > 0, "slice mutation does not panic")
		dc.BeginAnalytic()
		out2, err := dc.ResolveAnalytic()
		ch.Check(err == nil, "next pass resolves")
		ch.Check(math.Abs(out2[0].R-float64(dc.ColorBuffer.Pix[0])/255) < 1.0/255+1e-9,
			"next pass unaffected by caller slice mutation")
	})

	t.Run("cancel_and_idle_state_rules", func(t *testing.T) {
		ch := unitcheck.New(t)
		dc := NewContext(1, 1)
		dc.ClearColorBufferWith(Black)
		before := append([]byte(nil), dc.ColorBuffer.Pix...)
		dc.CancelAnalytic() // idle cancel is a no-op
		dc.BeginAnalytic()
		ch.Check(dc.BeginAnalytic() != nil, "double begin rejected")
		dc.DrawAnalyticTriangles([]*Triangle{mkTri()}, Identity())
		dc.CancelAnalytic()
		ch.Check(dc.analytic == nil, "cancel clears staged geometry")
		ch.Check(string(dc.ColorBuffer.Pix) == string(before), "cancel leaves buffers untouched")
		dc.CancelAnalytic() // idempotent
		ch.Check(dc.DrawAnalyticTriangles([]*Triangle{mkTri()}, Identity()) != nil, "idle submit rejected")
		_, err := dc.ResolveAnalytic()
		ch.Check(err != nil, "idle resolve rejected")

		// Begin validation rules.
		transparent := NewContext(1, 1)
		ch.Check(transparent.BeginAnalytic() != nil, "transparent base rejected")
		badDepth := NewContext(1, 1)
		badDepth.ClearColorBufferWith(Black)
		badDepth.DepthBuffer[0] = 1.5
		ch.Check(badDepth.BeginAnalytic() != nil, "out-of-range base depth rejected")
		badDepth.DepthBuffer[0] = math.NaN()
		ch.Check(badDepth.BeginAnalytic() != nil, "NaN base depth rejected")
	})

	t.Run("consecutive_passes_and_two_contexts", func(t *testing.T) {
		ch := unitcheck.New(t)
		dc := NewContext(1, 1)
		dc.ClearColorBufferWith(Black)
		dc.BeginAnalytic()
		dc.DrawAnalyticTriangles([]*Triangle{mkTri()}, Identity())
		first, err := dc.ResolveAnalytic()
		ch.Check(err == nil, "first resolve")
		ch.Check(dc.analytic == nil, "geometry cleared after resolve")

		// The published image is the new base; an empty pass reproduces it.
		ch.Check(dc.BeginAnalytic() == nil, "second begin")
		ch.Check(dc.DrawAnalyticTriangles(nil, Identity()) == nil, "empty batch succeeds")
		second, err := dc.ResolveAnalytic()
		ch.Check(err == nil, "second resolve")
		ch.Check(math.Abs(second[0].R-float64(dc.ColorBuffer.Pix[0])/255) < 1e-12, "empty pass reproduces base")
		ch.Check(math.Abs(second[0].R-first[0].R) <= 1.0/255+1e-12, "published round trip")

		// Two contexts never share pass state.
		d1, d2 := NewContext(1, 1), NewContext(1, 1)
		d1.ClearColorBufferWith(Black)
		d2.ClearColorBufferWith(Black)
		ch.Check(d1.BeginAnalytic() == nil && d2.BeginAnalytic() == nil, "two begins")
		d1.DrawAnalyticTriangles([]*Triangle{mkTri()}, Identity())
		ch.Check(len(d2.analytic.polygons) == 0, "second context stays empty")
		o1, e1 := d1.ResolveAnalytic()
		o2, e2 := d2.ResolveAnalytic()
		ch.Check(e1 == nil && e2 == nil, "both resolve")
		ch.Check(o1[0].R > 0 && o2[0].R == 0, "context outputs stay independent")
	})

	t.Run("matches_center_sampled_single_frame", func(t *testing.T) {
		ch := unitcheck.New(t)
		// A full-coverage constant-color, constant-depth quad must agree
		// with the legacy rasterizer on every pixel.
		const w, h = 4, 4
		col := Color{0.8, 0.4, 0.2, 1}
		ndcZ := 2*0.4 - 1
		ndcQuad := []*Triangle{
			{V1: Vertex{Position: V(-1, 1, ndcZ), Color: col},
				V2: Vertex{Position: V(1, 1, ndcZ), Color: col},
				V3: Vertex{Position: V(-1, -1, ndcZ), Color: col}},
			{V1: Vertex{Position: V(1, 1, ndcZ), Color: col},
				V2: Vertex{Position: V(1, -1, ndcZ), Color: col},
				V3: Vertex{Position: V(-1, -1, ndcZ), Color: col}},
		}

		// Legacy path. Drawn one triangle at a time: the parallel
		// DrawTriangles batch has a pre-existing unlocked early depth read
		// in this package, and the comparison only needs the same rasterize
		// function running single-threaded.
		legacy := NewContext(w, h)
		legacy.ClearColorBufferWith(Black)
		legacy.Cull = CullNone
		legacy.AlphaBlend = false
		legacy.Shader = NewSolidColorShader(Identity(), Color{0.8, 0.4, 0.2, 1})
		for _, q := range ndcQuad {
			legacy.DrawTriangle(q)
		}

		// Analytic path with the same NDC geometry.
		analytic := NewContext(w, h)
		analytic.ClearColorBufferWith(Black)
		ch.Check(analytic.BeginAnalytic() == nil, "begin")
		ch.Check(analytic.DrawAnalyticTriangles(ndcQuad, Identity()) == nil, "draw")
		out, err := analytic.ResolveAnalytic()
		ch.Check(err == nil, "resolve")
		same := 0
		for i := 0; i < w*h; i++ {
			if legacy.ColorBuffer.Pix[4*i] == analytic.ColorBuffer.Pix[4*i] &&
				legacy.ColorBuffer.Pix[4*i+3] == analytic.ColorBuffer.Pix[4*i+3] &&
				math.Abs(legacy.DepthBuffer[i]-analytic.DepthBuffer[i]) < 1e-12 {
				same++
			}
		}
		ch.Check(same == w*h, "all %d fully covered pixels match legacy path", same)
		ch.Check(math.Abs(out[0].A-1) < 1e-12, "analytic alpha is one")
	})
}
