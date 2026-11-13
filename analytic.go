package fauxgl

import (
	"errors"
	"math"
	"sort"
)

// Analytic anti-aliased rendering path.
//
// The center-sampled rasterizer picks one face at each pixel center. This
// path instead partitions every pixel square [x,x+1]x[y,y+1] into the exact
// visible regions of the submitted faces and the base image snapshot, then
// integrates the affine vertex-color function over each region by area and
// first area moment. No finite set of sample points is used: the partition
// is the exact planar arrangement of polygon edges and depth-equality
// lines, evaluated with a trapezoidal sweep.

// analyticTriangle holds one original submitted triangle's affine depth and
// vertex-color planes fitted in screen space. Clipping fans reference the
// same plane object, so shared edges and repeated faces evaluate identically.
type analyticTriangle struct {
	order int
	dA    float64
	dbx   float64
	dby   float64
	cA    [3]float64
	cbx   [3]float64
	cby   [3]float64
}

func (t *analyticTriangle) depthAt(x, y float64) float64 {
	return t.dA + t.dbx*x + t.dby*y
}

// analyticPolygon is a convex clipping-fan piece of one original triangle,
// already clipped to a pixel square.
type analyticPolygon struct {
	order int
	plane *analyticTriangle
	pt    []Vector
}

type analyticPass struct {
	triangles []*analyticTriangle
	polygons  []analyticPolygon // in NDC clipping space, before per-pixel clipping
	baseRGBA  []byte            // snapshot of ColorBuffer.Pix
	baseZ     []float64
}

// BeginAnalytic starts an analytic anti-aliasing pass against a snapshot of
// the current color and depth buffers. The buffers are not modified until
// ResolveAnalytic is called.
func (dc *Context) BeginAnalytic() error {
	if dc.analytic != nil {
		return errors.New("fauxgl: analytic pass already active")
	}
	if dc.Width <= 0 || dc.Height <= 0 {
		return errors.New("fauxgl: invalid context dimensions")
	}
	n := dc.Width * dc.Height
	if len(dc.DepthBuffer) != n || dc.ColorBuffer == nil ||
		len(dc.ColorBuffer.Pix) != 4*n {
		return errors.New("fauxgl: context buffers have invalid size")
	}
	rgba := make([]byte, len(dc.ColorBuffer.Pix))
	copy(rgba, dc.ColorBuffer.Pix)
	z := make([]float64, n)
	copy(z, dc.DepthBuffer)
	for i := 0; i < n; i++ {
		if rgba[4*i+3] != 255 {
			return errors.New("fauxgl: analytic base image must be fully opaque")
		}
		d := z[i]
		if math.IsNaN(d) || math.IsInf(d, 0) {
			return errors.New("fauxgl: analytic base depth must be finite in [0,1] or MaxFloat64")
		}
		if d != math.MaxFloat64 && (d < 0 || d > 1) {
			return errors.New("fauxgl: analytic base depth must be finite in [0,1] or MaxFloat64")
		}
	}
	dc.analytic = &analyticPass{baseRGBA: rgba, baseZ: z}
	return nil
}

// CancelAnalytic discards the active pass without touching the buffers.
// It is a no-op when no pass is active.
func (dc *Context) CancelAnalytic() {
	dc.analytic = nil
}

func finiteColor(c Color) bool {
	good := func(f float64) bool {
		return !math.IsNaN(f) && !math.IsInf(f, 0) && f >= 0 && f <= 1
	}
	return good(c.R) && good(c.G) && good(c.B) && c.A == 1
}

func finiteAffineMatrix(m Matrix) bool {
	v := []float64{
		m.X00, m.X01, m.X02, m.X03,
		m.X10, m.X11, m.X12, m.X13,
		m.X20, m.X21, m.X22, m.X23,
	}
	for _, f := range v {
		if math.IsNaN(f) || math.IsInf(f, 0) {
			return false
		}
	}
	return m.X30 == 0 && m.X31 == 0 && m.X32 == 0 && m.X33 == 1
}

func finiteVector(v Vector) bool {
	return !math.IsNaN(v.X) && !math.IsInf(v.X, 0) &&
		!math.IsNaN(v.Y) && !math.IsInf(v.Y, 0) &&
		!math.IsNaN(v.Z) && !math.IsInf(v.Z, 0)
}

func (c Color) channel(i int) float64 {
	switch i {
	case 0:
		return c.R
	case 1:
		return c.G
	default:
		return c.B
	}
}

// DrawAnalyticTriangles stages one batch of opaque, double-sided,
// vertex-colored triangles. A rejected batch leaves previously staged
// geometry untouched.
func (dc *Context) DrawAnalyticTriangles(triangles []*Triangle, matrix Matrix) error {
	pass := dc.analytic
	if pass == nil {
		return errors.New("fauxgl: no active analytic pass")
	}
	if !finiteAffineMatrix(matrix) {
		return errors.New("fauxgl: analytic matrix must be finite and affine")
	}

	// Validate and snapshot every input before touching staged geometry.
	type stagedTri struct {
		p [3]Vector
		c [3]Color
	}
	staged := make([]stagedTri, 0, len(triangles))
	for _, t := range triangles {
		if t == nil {
			return errors.New("fauxgl: analytic triangle is nil")
		}
		verts := [3]Vertex{t.V1, t.V2, t.V3}
		var p [3]Vector
		var c [3]Color
		for k, v := range verts {
			if !finiteVector(v.Position) {
				return errors.New("fauxgl: analytic triangle has non-finite position")
			}
			if !finiteColor(v.Color) {
				return errors.New("fauxgl: analytic triangle has invalid vertex color")
			}
			q := matrix.MulPosition(v.Position)
			if !finiteVector(q) {
				return errors.New("fauxgl: analytic transform produced non-finite coordinates")
			}
			p[k] = q
			c[k] = v.Color
		}
		staged = append(staged, stagedTri{p, c})
	}

	nextOrder := 0
	for _, tri := range pass.triangles {
		if tri.order >= nextOrder {
			nextOrder = tri.order + 1
		}
	}

	for _, s := range staged {
		order := nextOrder
		nextOrder++

		screen := dc.screenMatrix
		sp0 := screen.MulPosition(s.p[0])
		sp1 := screen.MulPosition(s.p[1])
		sp2 := screen.MulPosition(s.p[2])
		det := (sp1.X-sp0.X)*(sp2.Y-sp0.Y) - (sp2.X-sp0.X)*(sp1.Y-sp0.Y)
		if det == 0 {
			// Zero projected area: edge-on, never covers positive area.
			continue
		}

		plane := &analyticTriangle{order: order}
		fitPlane := func(v0, v1, v2 float64) (a, bx, by float64) {
			bx = ((v1-v0)*(sp2.Y-sp0.Y) - (v2-v0)*(sp1.Y-sp0.Y)) / det
			by = ((sp1.X-sp0.X)*(v2-v0) - (sp2.X-sp0.X)*(v1-v0)) / det
			a = v0 - bx*sp0.X - by*sp0.Y
			return
		}
		// Depth is the window-z produced by Screen, matching the existing
		// depth buffer; ordering is identical to NDC z either way.
		plane.dA, plane.dbx, plane.dby = fitPlane(sp0.Z, sp1.Z, sp2.Z)
		for ch := 0; ch < 3; ch++ {
			plane.cA[ch], plane.cbx[ch], plane.cby[ch] = fitPlane(
				s.c[0].channel(ch), s.c[1].channel(ch), s.c[2].channel(ch))
		}
		pass.triangles = append(pass.triangles, plane)

		// Clip against the NDC cube. The shared Sutherland-Hodgman clip
		// planes are reused, but with non-strict membership so faces ending
		// exactly on a clip plane keep their full area. An affine matrix
		// gives w == 1, so window z and color stay linear through the fan.
		clipped := analyticClipNDC(s.p, s.c)
		if len(clipped) == 0 {
			continue
		}

		// Every clipping-fan piece stays tagged with the original face's
		// order and affine planes.
		for k := 1; k < len(clipped); k++ {
			pts := []Vector{
				screen.MulPosition(clipped[0].p),
				screen.MulPosition(clipped[k-1].p),
				screen.MulPosition(clipped[k].p),
			}
			if polygonArea(pts) == 0 {
				continue
			}
			pass.polygons = append(pass.polygons, analyticPolygon{
				order: order,
				plane: plane,
				pt:    pts,
			})
		}
	}
	return nil
}

func polygonArea(pts []Vector) float64 {
	var a float64
	for i := range pts {
		p := pts[i]
		q := pts[(i+1)%len(pts)]
		a += p.X*q.Y - q.X*p.Y
	}
	return a / 2
}

// analyticClipNDC clips one triangle to the -1..1 NDC cube using the shared
// clip planes with non-strict inclusion. The returned convex polygon carries
// interpolated NDC positions and vertex colors for the affine clip fan.
type analyticClipVert struct {
	p Vector
	c Color
}

func analyticClipNDC(p [3]Vector, c [3]Color) []analyticClipVert {
	poly := []analyticClipVert{{p[0], c[0]}, {p[1], c[1]}, {p[2], c[2]}}
	clipOne := func(plane clipPlane, in []analyticClipVert) []analyticClipVert {
		if len(in) == 0 {
			return nil
		}
		val := func(v analyticClipVert) float64 {
			return v.p.Sub(plane.P.Vector()).Dot(plane.N.Vector())
		}
		var out []analyticClipVert
		s := in[len(in)-1]
		sv := val(s)
		for _, e := range in {
			ev := val(e)
			if ev >= 0 {
				if sv < 0 {
					t := sv / (sv - ev)
					out = append(out, analyticClipVert{
						s.p.Lerp(e.p, t),
						s.c.Lerp(e.c, t),
					})
				}
				out = append(out, e)
			} else if sv >= 0 {
				t := sv / (sv - ev)
				out = append(out, analyticClipVert{
					s.p.Lerp(e.p, t),
					s.c.Lerp(e.c, t),
				})
			}
			s, sv = e, ev
		}
		return out
	}
	for _, plane := range clipPlanes {
		poly = clipOne(plane, poly)
		if len(poly) == 0 {
			return nil
		}
	}
	return poly
}

// analyticClipPolygon clips a convex polygon to the square
// [x0,x1] x [y0,y1] with Sutherland-Hodgman.
func analyticClipPolygon(pts []Vector, x0, y0, x1, y1 float64) []Vector {
	inside := func(p Vector, side int) bool {
		switch side {
		case 0:
			return p.X >= x0
		case 1:
			return p.X <= x1
		case 2:
			return p.Y >= y0
		default:
			return p.Y <= y1
		}
	}
	intersect := func(a, b Vector, side int) Vector {
		var t float64
		switch side {
		case 0:
			t = (x0 - a.X) / (b.X - a.X)
		case 1:
			t = (x1 - a.X) / (b.X - a.X)
		case 2:
			t = (y0 - a.Y) / (b.Y - a.Y)
		default:
			t = (y1 - a.Y) / (b.Y - a.Y)
		}
		return Vector{a.X + t*(b.X-a.X), a.Y + t*(b.Y-a.Y), 0}
	}
	output := pts
	for side := 0; side < 4; side++ {
		if len(output) == 0 {
			return nil
		}
		input := output
		output = nil
		s := input[len(input)-1]
		sIn := inside(s, side)
		for _, e := range input {
			eIn := inside(e, side)
			if eIn {
				if !sIn {
					output = append(output, intersect(s, e, side))
				}
				output = append(output, e)
			} else if sIn {
				output = append(output, intersect(s, e, side))
			}
			s, sIn = e, eIn
		}
	}
	return output
}

func analyticPointInPolygon(x, y float64, pts []Vector) bool {
	const eps = 1e-10
	neg, pos := false, false
	for i := range pts {
		a := pts[i]
		b := pts[(i+1)%len(pts)]
		ex, ey := b.X-a.X, b.Y-a.Y
		cross := ex*(y-a.Y) - ey*(x-a.X)
		tol := eps * math.Max(1, math.Hypot(ex, ey))
		if cross < -tol {
			neg = true
		} else if cross > tol {
			pos = true
		}
		if neg && pos {
			return false
		}
	}
	return true
}

// analyticWinner returns the submission order of the winning original face at
// a point, or -1 for the base image. Clipping-fan pieces share one order and
// one affine plane, so the specific fan piece is irrelevant. Ties favor newer
// faces; the base loses every tie.
func analyticWinner(polys []analyticPolygon, baseDepth float64, x, y float64) int {
	win := -1
	winZ := baseDepth
	winOrder := -1
	for i := range polys {
		p := &polys[i]
		if !analyticPointInPolygon(x, y, p.pt) {
			continue
		}
		z := p.plane.depthAt(x, y)
		if z < winZ || (z == winZ && p.order > winOrder) {
			winZ = z
			winOrder = p.order
			win = p.order
		}
	}
	return win
}

// analyticPlaneForOrder resolves a winning face order to its affine planes.
// All clipping-fan pieces of one face share the same plane object.
func analyticPlaneForOrder(polys []analyticPolygon, order int) *analyticTriangle {
	for i := range polys {
		if polys[i].order == order {
			return polys[i].plane
		}
	}
	return nil
}

// analyticLineSegment is an infinite-line fragment clipped to convex regions.
type analyticLineSegment struct {
	ax, ay, bx, by float64
}

// analyticClipLineToConvex restricts p0 + t*d to a CCW convex polygon.
func analyticClipLineToConvex(p0, d Vector, pts []Vector) (float64, float64, bool) {
	t0 := math.Inf(-1)
	t1 := math.Inf(1)
	for i := range pts {
		a := pts[i]
		b := pts[(i+1)%len(pts)]
		ex, ey := b.X-a.X, b.Y-a.Y
		c := ex*(p0.Y-a.Y) - ey*(p0.X-a.X)
		g := ex*d.Y - ey*d.X
		switch {
		case g > 0:
			t := -c / g
			if t > t0 {
				t0 = t
			}
		case g < 0:
			t := -c / g
			if t < t1 {
				t1 = t
			}
		default:
			if c < 0 {
				return 0, 0, false
			}
		}
		if t0 > t1 {
			return 0, 0, false
		}
	}
	return t0, t1, true
}

// analyticSignedAreaMoment returns the signed area and signed first area
// moments of a polygon about the origin.
func analyticSignedAreaMoment(pts []Vector) (area, mx, my float64) {
	for i := range pts {
		p := pts[i]
		q := pts[(i+1)%len(pts)]
		c := p.X*q.Y - q.X*p.Y
		area += c
		mx += (p.X + q.X) * c
		my += (p.Y + q.Y) * c
	}
	area /= 2
	mx /= 6
	my /= 6
	return
}

// analyticRegion is one convex fragment of the exact visible partition of a
// pixel. Winner -1 denotes the base image; otherwise it is a face order.
type analyticRegion struct {
	win int
	pt  []Vector
}

// analyticPixelRegions builds the exact visible partition of one pixel. The
// square is split by a trapezoidal sweep over all polygon edges and
// depth-equality lines, so the visible winner is constant inside every
// returned region and the regions tile the pixel up to boundary-only contacts.
func analyticPixelRegions(polys []analyticPolygon, baseDepth float64, px, py int) []analyticRegion {
	x0, y0 := float64(px), float64(py)
	x1, y1 := x0+1, y0+1
	square := []Vector{{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}}

	// Normalize all polygons to CCW and collect boundary segments.
	var segs []analyticLineSegment
	for i := range polys {
		if polygonArea(polys[i].pt) < 0 {
			p := polys[i].pt
			for k, j := 0, len(p)-1; k < j; k, j = k+1, j-1 {
				p[k], p[j] = p[j], p[k]
			}
		}
		pts := polys[i].pt
		for k := range pts {
			a := pts[k]
			b := pts[(k+1)%len(pts)]
			if a.X != b.X || a.Y != b.Y {
				segs = append(segs, analyticLineSegment{a.X, a.Y, b.X, b.Y})
			}
		}
	}

	// Depth-equality lines: winner can only swap where two affine depth
	// planes coincide, or where a face meets the constant base depth.
	addEqualityLine := func(a, bx, by float64, regions ...[]Vector) {
		if bx == 0 && by == 0 {
			return // planes coincide everywhere; order tie is constant
		}
		var p0 Vector
		if math.Abs(bx) >= math.Abs(by) {
			p0 = Vector{-a / bx, 0, 0}
		} else {
			p0 = Vector{0, -a / by, 0}
		}
		d := Vector{by, -bx, 0}
		t0, t1 := math.Inf(-1), math.Inf(1)
		ok := true
		for _, region := range regions {
			lo, hi, good := analyticClipLineToConvex(p0, d, region)
			if !good {
				ok = false
				break
			}
			if lo > t0 {
				t0 = lo
			}
			if hi < t1 {
				t1 = hi
			}
		}
		if ok && t1 > t0 {
			pa := Vector{p0.X + t0*d.X, p0.Y + t0*d.Y, 0}
			pb := Vector{p0.X + t1*d.X, p0.Y + t1*d.Y, 0}
			segs = append(segs, analyticLineSegment{pa.X, pa.Y, pb.X, pb.Y})
		}
	}
	for i := range polys {
		pi := polys[i].plane
		if baseDepth != math.MaxFloat64 {
			a := pi.dA - baseDepth
			addEqualityLine(a, pi.dbx, pi.dby, square, polys[i].pt)
		}
		for j := i + 1; j < len(polys); j++ {
			pj := polys[j].plane
			a := pi.dA - pj.dA
			bx := pi.dbx - pj.dbx
			by := pi.dby - pj.dby
			addEqualityLine(a, bx, by, square, polys[i].pt, polys[j].pt)
		}
	}

	// Vertical slab cuts: every segment endpoint and every pairwise segment
	// intersection. No segment can cross another inside a slab.
	cuts := map[float64]bool{x0: true, x1: true}
	for _, s := range segs {
		cuts[s.ax] = true
		cuts[s.bx] = true
	}
	for i := 0; i < len(segs); i++ {
		for j := i + 1; j < len(segs); j++ {
			s, t := segs[i], segs[j]
			rx, ry := s.bx-s.ax, s.by-s.ay
			qx, qy := t.bx-t.ax, t.by-t.ay
			den := rx*qy - qx*ry
			if den == 0 {
				continue
			}
			sx, sy := t.ax-s.ax, t.ay-s.ay
			u := (sx*qy - qx*sy) / den
			v := (sx*ry - rx*sy) / den
			if u > 0 && u < 1 && v > 0 && v < 1 {
				cuts[s.ax+u*rx] = true
			}
		}
	}
	var xs []float64
	for x := range cuts {
		if x >= x0 && x <= x1 {
			xs = append(xs, x)
		}
	}
	sort.Float64s(xs)

	type level struct {
		yL, yR float64
	}
	var result []analyticRegion
	addFragment := func(pts ...Vector) {
		a, mx, my := analyticSignedAreaMoment(pts)
		if a == 0 {
			return
		}
		if a < 0 {
			a, mx, my = -a, -mx, -my
		}
		gx := mx / a
		gy := my / a
		win := analyticWinner(polys, baseDepth, gx, gy)
		cp := make([]Vector, len(pts))
		copy(cp, pts)
		result = append(result, analyticRegion{win, cp})
	}

	for k := 0; k+1 < len(xs); k++ {
		xa, xb := xs[k], xs[k+1]
		if xb-xa <= 1e-15 {
			continue
		}
		var levels []level
		for _, s := range segs {
			minX, maxX := math.Min(s.ax, s.bx), math.Max(s.ax, s.bx)
			if maxX <= xa+1e-12 || minX >= xb-1e-12 {
				continue
			}
			tL := (xa - s.ax) / (s.bx - s.ax)
			tR := (xb - s.ax) / (s.bx - s.ax)
			yL := s.ay + tL*(s.by-s.ay)
			yR := s.ay + tR*(s.by-s.ay)
			yM := s.ay + (tL+tR)/2*(s.by-s.ay)
			if yM < y0-1e-12 || yM > y1+1e-12 {
				continue
			}
			levels = append(levels, level{yL, yR})
		}
		sort.Slice(levels, func(i, j int) bool {
			mi := (levels[i].yL + levels[i].yR) / 2
			mj := (levels[j].yL + levels[j].yR) / 2
			return mi < mj
		})
		// Merge coincident crossings (shared edges, overlapping lines).
		merged := levels[:0]
		for _, lv := range levels {
			if len(merged) > 0 {
				last := &merged[len(merged)-1]
				mLast := (last.yL + last.yR) / 2
				mCur := (lv.yL + lv.yR) / 2
				if math.Abs(mCur-mLast) <= 1e-12 {
					continue
				}
			}
			merged = append(merged, lv)
		}
		levels = merged

		// Fragments between consecutive levels (square walls bound the rest).
		prevL, prevR := y0, y0
		for _, lv := range levels {
			if lv.yL > prevL+1e-12 || lv.yR > prevR+1e-12 {
				addFragment(
					Vector{xa, prevL, 0}, Vector{xb, prevR, 0},
					Vector{xb, lv.yR, 0}, Vector{xa, lv.yL, 0})
			}
			prevL, prevR = lv.yL, lv.yR
		}
		if y1 > prevL+1e-12 || y1 > prevR+1e-12 {
			addFragment(
				Vector{xa, prevL, 0}, Vector{xb, prevR, 0},
				Vector{xb, y1, 0}, Vector{xa, y1, 0})
		}
	}
	return result
}

// analyticPixelIntegral integrates vertex color over the exact visible
// partition of one pixel using each region's area and first area moments.
func analyticPixelIntegral(polys []analyticPolygon, baseDepth float64,
	baseRGBA []byte, baseIdx int, px, py int) Color {

	regions := analyticPixelRegions(polys, baseDepth, px, py)
	var sumR, sumG, sumB, totalArea float64
	for _, rg := range regions {
		area, mx, my := analyticSignedAreaMoment(rg.pt)
		if area < 0 {
			area, mx, my = -area, -mx, -my
		}
		totalArea += area
		if rg.win < 0 {
			sumR += float64(baseRGBA[4*baseIdx+0]) / 255 * area
			sumG += float64(baseRGBA[4*baseIdx+1]) / 255 * area
			sumB += float64(baseRGBA[4*baseIdx+2]) / 255 * area
			continue
		}
		plane := analyticPlaneForOrder(polys, rg.win)
		for ch := 0; ch < 3; ch++ {
			integral := plane.cA[ch]*area + plane.cbx[ch]*mx + plane.cby[ch]*my
			switch ch {
			case 0:
				sumR += integral
			case 1:
				sumG += integral
			default:
				sumB += integral
			}
		}
	}

	// The partition tiles the pixel exactly; any residual is numerical
	// boundary noise assigned to the base.
	if math.Abs(totalArea-1) > 1e-12 {
		missing := 1 - totalArea
		sumR += float64(baseRGBA[4*baseIdx+0]) / 255 * missing
		sumG += float64(baseRGBA[4*baseIdx+1]) / 255 * missing
		sumB += float64(baseRGBA[4*baseIdx+2]) / 255 * missing
	}
	return Color{sumR, sumG, sumB, 1}
}

// ResolveAnalytic partitions every pixel exactly, integrates vertex color
// over the visible regions, samples depth at the pixel center, and publishes
// the result to the context buffers. Staged geometry is cleared afterwards.
func (dc *Context) ResolveAnalytic() ([]Color, error) {
	pass := dc.analytic
	if pass == nil {
		return nil, errors.New("fauxgl: no active analytic pass")
	}
	defer func() { dc.analytic = nil }()

	w, h := dc.Width, dc.Height
	n := w * h
	floats := make([]Color, n)

	// Per-pixel candidate buckets from screen-space bounding boxes.
	buckets := make([][]int, n)
	for pi := range pass.polygons {
		src := pass.polygons[pi]
		minX, minY := math.Inf(1), math.Inf(1)
		maxX, maxY := math.Inf(-1), math.Inf(-1)
		for _, q := range src.pt {
			minX = math.Min(minX, q.X)
			maxX = math.Max(maxX, q.X)
			minY = math.Min(minY, q.Y)
			maxY = math.Max(maxY, q.Y)
		}
		ix0 := int(math.Floor(minX))
		iy0 := int(math.Floor(minY))
		ix1 := int(math.Ceil(maxX)) - 1
		iy1 := int(math.Ceil(maxY)) - 1
		if ix0 < 0 {
			ix0 = 0
		}
		if iy0 < 0 {
			iy0 = 0
		}
		if ix1 >= w {
			ix1 = w - 1
		}
		if iy1 >= h {
			iy1 = h - 1
		}
		for yy := iy0; yy <= iy1; yy++ {
			for xx := ix0; xx <= ix1; xx++ {
				buckets[yy*w+xx] = append(buckets[yy*w+xx], pi)
			}
		}
	}

	for py := 0; py < h; py++ {
		for px := 0; px < w; px++ {
			idx := py*w + px
			baseDepth := pass.baseZ[idx]

			var polys []analyticPolygon
			for _, pi := range buckets[idx] {
				src := pass.polygons[pi]
				clip := analyticClipPolygon(src.pt, float64(px), float64(py),
					float64(px+1), float64(py+1))
				if len(clip) < 3 || polygonArea(clip) == 0 {
					continue
				}
				polys = append(polys, analyticPolygon{
					order: src.order,
					plane: src.plane,
					pt:    clip,
				})
			}

			// Depth buffer: winner at the pixel center only, never an
			// area average. No face at the center keeps the base depth.
			cx, cy := float64(px)+0.5, float64(py)+0.5
			winOrder := -1
			if len(polys) > 0 {
				winOrder = analyticWinner(polys, baseDepth, cx, cy)
			}
			if winOrder < 0 {
				dc.DepthBuffer[idx] = baseDepth
			} else {
				plane := analyticPlaneForOrder(polys, winOrder)
				dc.DepthBuffer[idx] = plane.depthAt(cx, cy)
			}

			// Color buffer: exact area integral over the visible partition.
			if len(polys) == 0 {
				floats[idx] = Color{
					float64(pass.baseRGBA[4*idx+0]) / 255,
					float64(pass.baseRGBA[4*idx+1]) / 255,
					float64(pass.baseRGBA[4*idx+2]) / 255,
					1,
				}
			} else {
				floats[idx] = analyticPixelIntegral(polys, baseDepth,
					pass.baseRGBA, idx, px, py)
			}
		}
	}

	// Publish with the same quantization rules as Color.NRGBA.
	for i, c := range floats {
		nrgba := c.NRGBA()
		j := dc.ColorBuffer.PixOffset(i%w, i/w)
		dc.ColorBuffer.Pix[j+0] = nrgba.R
		dc.ColorBuffer.Pix[j+1] = nrgba.G
		dc.ColorBuffer.Pix[j+2] = nrgba.B
		dc.ColorBuffer.Pix[j+3] = 255
	}

	out := make([]Color, len(floats))
	copy(out, floats)
	return out, nil
}
