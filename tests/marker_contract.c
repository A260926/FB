/* Marker support contract tests.
 *
 * Registered through support/marker_test.h; this file provides only the
 * MarkerCase table (no main, no NanoSVG implementation expansion). Expected
 * positions, angles and matrices are recomputed here from each test's own
 * input, never read back from the product's placement logic.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "marker_test.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define W 80
#define H 80

static NSVGimage* parse(const char* text)
{
	size_t n = strlen(text);
	char* buf = (char*)malloc(n + 1);
	NSVGimage* img;
	if (!buf) return NULL;
	memcpy(buf, text, n + 1);
	img = nsvgParse(buf, "px", 96);
	free(buf);
	return img;
}

static unsigned char* render(NSVGrasterizer* r, NSVGimage* img)
{
	unsigned char* px = (unsigned char*)calloc(W*H*4, 1);
	if (!px) return NULL;
	nsvgRasterize(r, img, 0, 0, 1, px, W, H, W*4);
	return px;
}

static unsigned char* at(unsigned char* px, int x, int y)
{
	return px + (y*W + x)*4;
}

static int countSlots(NSVGshape* sh, int slot, int* total)
{
	NSVGmarkerPlacement* pl;
	int n = 0;
	char prev = -1;
	*total = 0;
	for (pl = sh->markerPlacements; pl != NULL; pl = pl->next) {
		/* The linked list must stay in start -> mid -> end slot order. */
		if (pl->slot < prev) return -1;
		prev = pl->slot;
		(*total)++;
		if (pl->slot == slot) n++;
	}
	return n;
}

static int shapeCount(NSVGimage* img)
{
	NSVGshape* sh;
	int n = 0;
	for (sh = img->shapes; sh != NULL; sh = sh->next) n++;
	return n;
}

static int nearly(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

/* Affine helpers used to compute expectations from the test inputs. */
static void tIdentity(float* t)
{
	t[0]=1; t[1]=0; t[2]=0; t[3]=1; t[4]=0; t[5]=0;
}
static void tMul(float* t, const float* s)
{
	float t0=t[0]*s[0]+t[1]*s[2], t2=t[2]*s[0]+t[3]*s[2];
	float t4=t[4]*s[0]+t[5]*s[2]+s[4];
	t[1]=t[0]*s[1]+t[1]*s[3];
	t[3]=t[2]*s[1]+t[3]*s[3];
	t[5]=t[4]*s[1]+t[5]*s[3]+s[5];
	t[0]=t0; t[2]=t2; t[4]=t4;
}
static void tMap(const float* t, float x, float y, float* ox, float* oy)
{
	*ox = x*t[0] + y*t[2] + t[4];
	*oy = x*t[1] + y*t[3] + t[5];
}

/* ------------------------------------------------------------------ */
/* topology                                                            */
/* ------------------------------------------------------------------ */

static void topo_arc_one_segment(MarkerTest* ctx)
{
	/* One source arc, normalized into several cubics, still yields no mid. */
	const char* svg =
		"<svg width='80' height='80'>"
		"<defs><marker id='m' markerWidth='4' markerHeight='4'><path d='M0 0L2 1L0 2Z'/></marker></defs>"
		"<path d='M10 30 A10 10 0 1 1 30 30' fill='none' stroke='black' "
		"marker-start='url(#m)' marker-mid='url(#m)' marker-end='url(#m)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGshape* sh;
	int total = 0, mids;
	MARKER_CHECK(ctx, img != NULL);
	sh = img->shapes;
	MARKER_CHECK(ctx, sh != NULL && sh->paths != NULL);
	MARKER_CHECK(ctx, sh->paths->npts > 4); /* arc split into >1 cubic */
	MARKER_CHECK(ctx, countSlots(sh, NSVG_MARKER_SLOT_MID, &total) == 0);
	mids = countSlots(sh, NSVG_MARKER_SLOT_MID, &total);
	MARKER_CHECK(ctx, mids == 0 && total == 2); /* start + end only */
	nsvgDelete(img);
}

static void topo_polyline_vertices(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'>"
		"<defs><marker id='m' markerWidth='2' markerHeight='2'><path d='M0 0L1 1L0 2Z'/></marker></defs>"
		"<polyline points='10,10 20,10 20,10 30,10' fill='none' stroke='black' "
		"marker-start='url(#m)' marker-mid='url(#m)' marker-end='url(#m)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGshape* sh = img->shapes;
	int total = 0, starts, mids, ends, coincident = 0;
	NSVGmarkerPlacement* pl;
	MARKER_CHECK(ctx, img != NULL);
	starts = countSlots(sh, NSVG_MARKER_SLOT_START, &total);
	mids = countSlots(sh, NSVG_MARKER_SLOT_MID, &total);
	ends = countSlots(sh, NSVG_MARKER_SLOT_END, &total);
	MARKER_CHECK(ctx, starts == 1 && mids == 2 && ends == 1 && total == 4);
	/* Coincident connection points stay two distinct marker positions. */
	for (pl = sh->markerPlacements; pl != NULL; pl = pl->next) {
		if (pl->slot == NSVG_MARKER_SLOT_MID &&
			nearly(pl->x, 20.0f) && nearly(pl->y, 10.0f))
			coincident++;
	}
	MARKER_CHECK(ctx, coincident == 2);
	nsvgDelete(img);
}

static void topo_closed_seam(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'>"
		"<defs><marker id='m' markerWidth='2' markerHeight='2' orient='auto'><path d='M0 0L1 1L0 2Z'/></marker></defs>"
		"<polygon points='10,10 30,10 20,25' fill='none' stroke='black' "
		"marker-start='url(#m)' marker-mid='url(#m)' marker-end='url(#m)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGshape* sh = img->shapes;
	int total = 0, starts, mids, ends;
	int seamStart = 0, seamEnd = 0;
	NSVGmarkerPlacement* pl;
	MARKER_CHECK(ctx, img != NULL);
	starts = countSlots(sh, NSVG_MARKER_SLOT_START, &total);
	mids = countSlots(sh, NSVG_MARKER_SLOT_MID, &total);
	ends = countSlots(sh, NSVG_MARKER_SLOT_END, &total);
	/* Closed: Z is the last segment; the seam gets both start and end. */
	MARKER_CHECK(ctx, starts == 1 && mids == 3 && ends == 1 && total == 5);
	for (pl = sh->markerPlacements; pl != NULL; pl = pl->next) {
		if (!nearly(pl->x, 10.0f) || !nearly(pl->y, 10.0f)) continue;
		if (pl->slot == NSVG_MARKER_SLOT_START) {
			seamStart++;
			/* Outgoing edge (10,10)->(30,10): angle 0. */
			MARKER_CHECK(ctx, nearly(pl->angle, 0.0f));
		} else if (pl->slot == NSVG_MARKER_SLOT_END) {
			seamEnd++;
			/* Incoming edge (20,25)->(10,10): angle atan2(-15,-10). */
			MARKER_CHECK(ctx, nearly(pl->angle, atan2f(-15.0f, -10.0f)));
		}
	}
	MARKER_CHECK(ctx, seamStart == 1 && seamEnd == 1);
	nsvgDelete(img);
}

static void topo_zero_length_and_m_only(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'>"
		"<defs><marker id='m' markerWidth='2' markerHeight='2' orient='auto'><path d='M0 0L1 1L0 2Z'/></marker></defs>"
		"<path d='M5 5 M10 10 L20 10 L20 10 L30 10' fill='none' stroke='black' "
		"marker-start='url(#m)' marker-mid='url(#m)' marker-end='url(#m)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGshape* sh = img->shapes;
	int total = 0, starts, mids, ends;
	int zeroVertex = 0;
	NSVGmarkerPlacement* pl;
	NSVGmarkerTrace* tr;
	MARKER_CHECK(ctx, img != NULL);
	/* The M-only subpath produces no trace. */
	MARKER_CHECK(ctx, sh->markerTrace != NULL && sh->markerTrace->next == NULL);
	tr = sh->markerTrace;
	MARKER_CHECK(ctx, tr->npts == 4);
	starts = countSlots(sh, NSVG_MARKER_SLOT_START, &total);
	mids = countSlots(sh, NSVG_MARKER_SLOT_MID, &total);
	ends = countSlots(sh, NSVG_MARKER_SLOT_END, &total);
	MARKER_CHECK(ctx, starts == 1 && mids == 2 && ends == 1 && total == 4);
	for (pl = sh->markerPlacements; pl != NULL; pl = pl->next) {
		if (pl->slot == NSVG_MARKER_SLOT_MID &&
			nearly(pl->x, 20.0f) && nearly(pl->y, 10.0f)) {
			zeroVertex++;
			/* Zero-length segment falls back to the horizontal neighbor. */
			MARKER_CHECK(ctx, nearly(pl->angle, 0.0f));
		}
	}
	MARKER_CHECK(ctx, zeroVertex == 2);
	nsvgDelete(img);
}

static void topo_closed_then_continue(MarkerTest* ctx)
{
	/* After Z, drawing continues from the seam start; the next subpath must
	 * still record its move vertex for marker placement. */
	const char* svg =
		"<svg width='80' height='80'>"
		"<defs><marker id='m' markerWidth='2' markerHeight='2' orient='auto'><path d='M0 0L1 1L0 2Z'/></marker></defs>"
		"<path d='M10 10 L30 10 L20 25 Z L40 20' fill='none' stroke='black' "
		"marker-start='url(#m)' marker-mid='url(#m)' marker-end='url(#m)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGshape* sh = img->shapes;
	int total = 0;
	int starts, mids, ends;
	int secondStart = 0, secondEnd = 0;
	NSVGmarkerPlacement* pl;
	MARKER_CHECK(ctx, img != NULL);
	MARKER_CHECK(ctx, sh->markerTrace != NULL && sh->markerTrace->next != NULL);
	MARKER_CHECK(ctx, sh->markerTrace->next->npts == 2);
	starts = countSlots(sh, NSVG_MARKER_SLOT_START, &total);
	mids = countSlots(sh, NSVG_MARKER_SLOT_MID, &total);
	ends = countSlots(sh, NSVG_MARKER_SLOT_END, &total);
	/* Closed triangle (3 mids incl. seam) + open 2-vertex subpath: 2 starts,
	 * 3 mids, 2 ends. */
	MARKER_CHECK(ctx, starts == 2 && mids == 3 && ends == 2 && total == 7);
	for (pl = sh->markerPlacements; pl != NULL; pl = pl->next) {
		/* The continued open subpath restarts at the original move point
		 * (10,10), matching NanoSVG's post-Z current-point behavior... */
		if (nearly(pl->x, 10.0f) && nearly(pl->y, 10.0f) &&
		    pl->slot == NSVG_MARKER_SLOT_START)
			secondStart++;
		/* ...and ends at the new vertex (40,20). */
		if (nearly(pl->x, 40.0f) && nearly(pl->y, 20.0f) &&
		    pl->slot == NSVG_MARKER_SLOT_END)
			secondEnd++;
	}
	MARKER_CHECK(ctx, secondStart == 2 && secondEnd == 1);
	nsvgDelete(img);
}

static void topo_none_and_missing(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'>"
		"<defs><marker id='m' markerWidth='2' markerHeight='2'><path d='M0 0L1 1L0 2Z'/></marker></defs>"
		"<line x1='5' y1='40' x2='70' y2='40' stroke='blue' stroke-width='2' "
		"marker-start='none' marker-mid='url(#m)' marker-end='url(#ghost)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	NSVGshape* sh = img->shapes;
	int total = 0;
	unsigned char* px;
	MARKER_CHECK(ctx, img != NULL);
	/* "none" disables start; missing id drops end; open line has no mid vertex. */
	MARKER_CHECK(ctx, countSlots(sh, NSVG_MARKER_SLOT_START, &total) == 0);
	MARKER_CHECK(ctx, total == 0);
	/* The line itself still paints. */
	px = render(r, img);
	MARKER_CHECK(ctx, at(px, 40, 40)[2] == 255 && at(px, 40, 40)[3] == 255);
	free(px);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
}

/* ------------------------------------------------------------------ */
/* coordinates                                                         */
/* ------------------------------------------------------------------ */

#define TRI_MARKER \
	"<marker id='tri' markerWidth='10' markerHeight='10' refX='0' refY='0' " \
	"markerUnits='userSpaceOnUse' orient='auto' overflow='visible'>" \
	"<path d='M0 -4 L8 0 L0 4 Z' fill='red'/></marker>"

static void coord_orient_auto(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'><defs>" TRI_MARKER "</defs>"
		"<path d='M10 10 L60 10 M10 60 L10 20' fill='none' stroke='black' "
	"marker-end='url(#tri)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGshape* sh = img->shapes;
	NSVGmarkerPlacement* pl;
	float angles[2];
	int n = 0;
	MARKER_CHECK(ctx, img != NULL);
	for (pl = sh->markerPlacements; pl != NULL; pl = pl->next) {
		MARKER_CHECK(ctx, pl->slot == NSVG_MARKER_SLOT_END);
		angles[n++] = pl->angle;
	}
	MARKER_CHECK(ctx, n == 2);
	MARKER_CHECK(ctx, nearly(angles[0], 0.0f));              /* rightward */
	MARKER_CHECK(ctx, nearly(angles[1], -M_PI/2.0f));        /* upward */
	nsvgDelete(img);
}

static void coord_orient_fixed_and_reverse(MarkerTest* ctx)
{
	const char* svgFixed =
		"<svg width='80' height='80'><defs>"
		"<marker id='f' markerWidth='4' markerHeight='4' orient='45'><path d='M0 0L1 1L0 2Z'/></marker>"
		"</defs><path d='M10 10 L60 60' fill='none' stroke='black' "
		"marker-start='url(#f)' marker-end='url(#f)'/></svg>";
	const char* svgRev =
		"<svg width='80' height='80'><defs>"
		"<marker id='r' markerWidth='4' markerHeight='4' orient='auto-start-reverse'><path d='M0 0L1 1L0 2Z'/></marker>"
		"</defs><path d='M10 40 L60 40' fill='none' stroke='black' "
		"marker-start='url(#r)' marker-end='url(#r)'/></svg>";
	NSVGimage* img = parse(svgFixed);
	NSVGmarkerPlacement* pl;
	MARKER_CHECK(ctx, img != NULL);
	for (pl = img->shapes->markerPlacements; pl != NULL; pl = pl->next)
		MARKER_CHECK(ctx, nearly(pl->angle, M_PI/4.0f));
	nsvgDelete(img);

	img = parse(svgRev);
	MARKER_CHECK(ctx, img != NULL);
	for (pl = img->shapes->markerPlacements; pl != NULL; pl = pl->next) {
		if (pl->slot == NSVG_MARKER_SLOT_START)
			MARKER_CHECK(ctx, nearly(pl->angle, M_PI));
		else
			MARKER_CHECK(ctx, nearly(pl->angle, 0.0f));
	}
	nsvgDelete(img);
}

static void coord_mid_bisector(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'><defs>"
		"<marker id='m' markerWidth='2' markerHeight='2' orient='auto'><path d='M0 0L1 1L0 2Z'/></marker>"
		"</defs><path d='M0 0 L10 0 L10 10 M30 0 L40 0 L30 0' fill='none' stroke='black' "
		"marker-mid='url(#m)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGmarkerPlacement* pl;
	int n = 0;
	MARKER_CHECK(ctx, img != NULL);
	for (pl = img->shapes->markerPlacements; pl != NULL; pl = pl->next) {
		MARKER_CHECK(ctx, pl->slot == NSVG_MARKER_SLOT_MID);
		if (n == 0) {
			/* 0 deg in, 90 deg out -> 45 deg. */
			MARKER_CHECK(ctx, nearly(pl->x, 10.0f) && nearly(pl->y, 0.0f));
			MARKER_CHECK(ctx, nearly(pl->angle, M_PI/4.0f));
		} else {
			/* Exactly opposite directions -> +180 deg. */
			MARKER_CHECK(ctx, nearly(pl->angle, M_PI));
		}
		n++;
	}
	MARKER_CHECK(ctx, n == 2);
	nsvgDelete(img);
}

static void coord_ref_anchor_pixels(MarkerTest* ctx)
{
	/* refX=5 shifts the anchored content point; compare against the same
	 * triangle written out as an ordinary path with hand-computed coords. */
	const char* svgMarked =
		"<svg width='80' height='80'><defs>"
		"<marker id='tri' markerWidth='10' markerHeight='10' refX='5' refY='0' "
		"markerUnits='userSpaceOnUse' orient='auto' overflow='visible'>"
		"<path d='M0 -4 L8 0 L0 4 Z' fill='red'/></marker></defs>"
		"<line x1='10' y1='30' x2='40' y2='30' stroke='blue' stroke-width='2' "
		"marker-end='url(#tri)'/></svg>";
	const char* svgPlain =
		"<svg width='80' height='80'>"
		"<line x1='10' y1='30' x2='40' y2='30' stroke='blue' stroke-width='2'/>"
		"<path d='M35 26 L43 30 L35 34 Z' fill='red'/></svg>";
	NSVGimage* a = parse(svgMarked);
	NSVGimage* b = parse(svgPlain);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* pa = render(r, a);
	unsigned char* pb = render(r, b);
	int x, y, diffs = 0;
	MARKER_CHECK(ctx, a != NULL && b != NULL);
	/* Interior pixels, away from antialiased edges, must match exactly. */
	for (y = 28; y <= 32; y++)
		for (x = 37; x <= 41; x++) {
			int k;
			for (k = 0; k < 4; k++) {
				if (abs((int)at(pa,x,y)[k] - (int)at(pb,x,y)[k]) > 2)
					diffs++;
			}
		}
	MARKER_CHECK(ctx, diffs == 0);
	/* Anchor content point (5,0) lands on the vertex (40,30). */
	MARKER_CHECK(ctx, at(pa, 40, 30)[0] == 255 && at(pa, 40, 30)[3] == 255);
	free(pa); free(pb);
	nsvgDeleteRasterizer(r);
	nsvgDelete(a); nsvgDelete(b);
}

static void coord_viewbox_meet_and_none(MarkerTest* ctx)
{
	const char* meet =
		"<svg width='80' height='80'><defs>"
		"<marker id='m' markerWidth='10' markerHeight='10' refX='0' refY='0' "
		"viewBox='0 0 20 10' markerUnits='userSpaceOnUse' orient='0' overflow='visible'>"
		"<path d='M0 0 L20 0 L20 10 L0 10 Z' fill='#00ff00'/></marker></defs>"
		"<line x1='10' y1='40' x2='40' y2='40' marker-end='url(#m)'/></svg>";
	const char* none =
		"<svg width='80' height='80'><defs>"
		"<marker id='m' markerWidth='10' markerHeight='10' refX='0' refY='0' "
		"viewBox='0 0 20 10' preserveAspectRatio='none' "
		"markerUnits='userSpaceOnUse' orient='0' overflow='visible'>"
		"<path d='M0 0 L20 0 L20 10 L0 10 Z' fill='#00ff00'/></marker></defs>"
		"<line x1='10' y1='40' x2='40' y2='40' marker-end='url(#m)'/></svg>";
	NSVGrasterizer* r = nsvgCreateRasterizer();
	NSVGimage* img = parse(meet);
	unsigned char* pm = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* meet: uniform 0.5 scale; refY=0 anchoring keeps the content origin at
	 * the vertex, so the rect covers x 40..50 and y 40..45. */
	MARKER_CHECK(ctx, at(pm, 45, 42)[1] == 255 && at(pm, 45, 42)[3] == 255);
	MARKER_CHECK(ctx, at(pm, 45, 47)[3] == 0);
	nsvgDelete(img); free(pm);
	img = parse(none);
	pm = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* none: independent scales -> content fills 10x10, y 40..50. */
	MARKER_CHECK(ctx, at(pm, 45, 45)[1] == 255 && at(pm, 45, 45)[3] == 255);
	MARKER_CHECK(ctx, at(pm, 45, 52)[3] == 0);
	nsvgDelete(img); free(pm);
	nsvgDeleteRasterizer(r);
}

static void coord_nonuniform_transform(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'><defs>" TRI_MARKER "</defs>"
		"<g transform='matrix(2 0 0 1 0 0)'>"
		"<line x1='10' y1='10' x2='20' y2='20' stroke='black' marker-end='url(#tri)'/>"
		"</g></svg>";
	NSVGimage* img = parse(svg);
	NSVGshape* sh = img->shapes;
	NSVGmarkerPlacement* pl = sh->markerPlacements;
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* px;
	/* Expectation matrix built in this test from the documented order.
	 * NanoSVG uses column vectors, so later stages are post-multiplied:
	 * root * shapeXform * translate(vertex) * rotate(angle). */
	float m[6], s[6];
	float cx, cy;
	MARKER_CHECK(ctx, img != NULL && pl != NULL);
	/* Direction is taken in local coordinates first. */
	MARKER_CHECK(ctx, nearly(pl->angle, M_PI/4.0f));
	MARKER_CHECK(ctx, nearly(pl->x, 20.0f) && nearly(pl->y, 20.0f));
	tIdentity(m);
	tIdentity(s);
	s[0]=cosf(M_PI/4.0f); s[1]=sinf(M_PI/4.0f);
	s[2]=-sinf(M_PI/4.0f); s[3]=cosf(M_PI/4.0f);
	tMul(m, s);                                      /* orient auto */
	tIdentity(s); s[4]=20.0f; s[5]=20.0f; tMul(m, s);/* local vertex */
	tIdentity(s); s[0]=2.0f; tMul(m, s);             /* shape xform (root identity) */
	/* Triangle centroid in marker content, well away from every edge. */
	tMap(m, 8.0f/3.0f, 0.0f, &cx, &cy);
	px = render(r, img);
	MARKER_CHECK(ctx, at(px, (int)lroundf(cx), (int)lroundf(cy))[0] == 255);
	MARKER_CHECK(ctx, at(px, (int)lroundf(cx), (int)lroundf(cy))[3] >= 200);
	free(px);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
}

static void coord_marker_stroke_scale(MarkerTest* ctx)
{
	/* A stroked marker at raster scale 2 must match the same geometry and
	 * stroke width expanded by hand as an ordinary open path. */
	const char* marked =
		"<svg width='80' height='80' xmlns='http://www.w3.org/2000/svg'><defs>"
		"<marker id='m' markerWidth='8' markerHeight='8' refX='0' refY='0' orient='0' "
	"markerUnits='userSpaceOnUse' overflow='visible'>"
		"<path d='M0 -2 L4 0 L0 2' fill='none' stroke='#ffff00' stroke-width='1'/></marker>"
		"</defs><line x1='5' y1='30' x2='10' y2='30' marker-end='url(#m)'/></svg>";
	/* The same content as an ordinary open path rendered at the same scale
	 * must match the marker's stroke exactly. */
	const char* plain =
		"<svg width='80' height='80' xmlns='http://www.w3.org/2000/svg'>"
		"<path d='M10 28 L14 30 L10 32' fill='none' stroke='#ffff00' stroke-width='1'/></svg>";
	NSVGimage* a = parse(marked);
	NSVGimage* b = parse(plain);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* pa = (unsigned char*)calloc(W*H*4, 1);
	unsigned char* pb = (unsigned char*)calloc(W*H*4, 1);
	int x, y, diffs = 0;
	MARKER_CHECK(ctx, a != NULL && b != NULL);
	nsvgRasterize(r, a, 0, 0, 2, pa, W, H, W*4);
	nsvgRasterize(r, b, 0, 0, 2, pb, W, H, W*4);
	for (y = 52; y <= 68; y++)
		for (x = 16; x <= 32; x++) {
			int k;
			for (k = 0; k < 4; k++)
				if (abs((int)at(pa,x,y)[k] - (int)at(pb,x,y)[k]) > 2)
					diffs++;
		}
	MARKER_CHECK(ctx, diffs == 0);
	free(pa); free(pb);
	nsvgDeleteRasterizer(r);
	nsvgDelete(a); nsvgDelete(b);
}

static void coord_nested_group_transform(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'><defs>"
		"<marker id='m' markerWidth='8' markerHeight='8' refX='0' refY='0' "
	"markerUnits='userSpaceOnUse' orient='0' overflow='visible'>"
		"<g transform='translate(2,0)'><path d='M0 -1 L4 0 L0 1 Z' fill='red'/></g>"
		"</marker></defs><line x1='10' y1='30' x2='30' y2='30' marker-end='url(#m)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* px = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* Content tip at x=4 plus the group's translate(2): 6px past the vertex;
	 * the thin triangle only covers one full-coverage pixel column. */
	MARKER_CHECK(ctx, at(px, 32, 30)[0] == 255 && at(px, 32, 30)[3] >= 150);
	free(px);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
}

static void coord_marker_units(MarkerTest* ctx)
{
	const char* swSvg =
		"<svg width='80' height='80'><defs>"
		"<marker id='m' markerWidth='6' markerHeight='6' refX='0' refY='0' orient='0' overflow='visible'>"
		"<path d='M0 -1 L2 0 L0 1 Z' fill='red'/></marker></defs>"
		"<line x1='5' y1='20' x2='20' y2='20' stroke='black' stroke-width='4' marker-end='url(#m)'/></svg>";
	const char* userSvg =
		"<svg width='80' height='80'><defs>"
		"<marker id='m' markerWidth='6' markerHeight='6' refX='0' refY='0' orient='0' "
	"markerUnits='userSpaceOnUse' overflow='visible'>"
		"<path d='M0 -1 L2 0 L0 1 Z' fill='red'/></marker></defs>"
		"<line x1='5' y1='20' x2='20' y2='20' stroke='black' stroke-width='4' marker-end='url(#m)'/></svg>";
	const char* zeroSvg =
		"<svg width='80' height='80'><defs>"
		"<marker id='m' markerWidth='6' markerHeight='6' refX='0' refY='0' orient='0' overflow='visible'>"
		"<path d='M0 -1 L2 0 L0 1 Z' fill='red'/></marker></defs>"
		"<line x1='5' y1='50' x2='30' y2='50' stroke='black' stroke-width='0' marker-end='url(#m)'/></svg>";
	const char* noStrokeSvg =
		"<svg width='80' height='80'><defs>"
		"<marker id='m' markerWidth='6' markerHeight='6' refX='0' refY='0' orient='0' "
	"markerUnits='userSpaceOnUse' overflow='visible'>"
		"<path d='M0 -1 L2 0 L0 1 Z' fill='red'/></marker></defs>"
		"<line x1='5' y1='50' x2='30' y2='50' marker-end='url(#m)'/></svg>";
	NSVGrasterizer* r = nsvgCreateRasterizer();
	NSVGimage* img;
	unsigned char* px;

	img = parse(swSvg); px = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* strokeWidth units: content scaled by 4 -> tip 8px past the vertex. */
	MARKER_CHECK(ctx, at(px, 24, 20)[0] == 255 && at(px, 24, 20)[3] == 255);
	MARKER_CHECK(ctx, at(px, 29, 20)[3] == 0); /* beyond the scaled marker */
	nsvgDelete(img); free(px);

	img = parse(userSvg); px = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* userSpaceOnUse: unscaled marker starts exactly at the vertex. */
	MARKER_CHECK(ctx, at(px, 20, 20)[0] == 255 && at(px, 20, 20)[3] >= 150);
	nsvgDelete(img); free(px);

	img = parse(zeroSvg); px = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* Zero reference stroke width suppresses strokeWidth markers. */
	MARKER_CHECK(ctx, at(px, 31, 50)[3] == 0);
	nsvgDelete(img); free(px);

	img = parse(noStrokeSvg); px = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* userSpaceOnUse markers show even when the line has no stroke. */
	MARKER_CHECK(ctx, at(px, 30, 50)[0] == 255 && at(px, 30, 50)[3] >= 150);
	nsvgDelete(img); free(px);
	nsvgDeleteRasterizer(r);
}

static void topo_style_scope_and_dimensions(MarkerTest* ctx)
{
	/* Inline style, group-scoped references, "none" override and invalid
	 * dimensions all exercise the same reference resolution path. */
	const char* svg =
		"<svg width='80' height='80'>"
		"<g marker-end='url(#m)'>"
		"<line x1='5' y1='10' x2='30' y2='10' stroke='black' "
		"style='marker-start: url(#m); marker-mid:none'/>"
		"<line x1='5' y1='30' x2='30' y2='30' stroke='black' marker-end='none'/>"
		"</g>"
		"<line x1='5' y1='50' x2='30' y2='50' stroke='black' marker-end='url(#zero)'/>"
		"<defs>"
		"<marker id='m' markerWidth='4' markerHeight='4' orient='0' "
	"markerUnits='userSpaceOnUse' overflow='visible'>"
		"<path d='M0 -1 L2 0 L0 1 Z' fill='red'/></marker>"
		"<marker id='zero' markerWidth='0' markerHeight='4' orient='0' "
	"markerUnits='userSpaceOnUse' overflow='visible'>"
		"<path d='M0 -1 L2 0 L0 1 Z' fill='red'/></marker>"
		"</defs></svg>";
	NSVGimage* img = parse(svg);
	NSVGshape *s1, *s2, *s3;
	int total = 0;
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* px;
	MARKER_CHECK(ctx, img != NULL);
	s1 = img->shapes; s2 = s1->next; s3 = s2->next;
	/* First line: style start + inherited group end, mid explicitly none. */
	MARKER_CHECK(ctx, countSlots(s1, NSVG_MARKER_SLOT_START, &total) == 1);
	MARKER_CHECK(ctx, countSlots(s1, NSVG_MARKER_SLOT_END, &total) == 1);
	MARKER_CHECK(ctx, total == 2);
	/* Second line overrides the inherited group marker with none. */
	MARKER_CHECK(ctx, s2->markerPlacements == NULL);
	/* Third line references a marker with non-positive width: no pixels. */
	px = render(r, img);
	MARKER_CHECK(ctx, s3->markerPlacements != NULL);
	MARKER_CHECK(ctx, at(px, 31, 50)[3] == 0);
	free(px);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
}

/* ------------------------------------------------------------------ */
/* paint                                                               */
/* ------------------------------------------------------------------ */
static const char* ORDER_SVG_HEAD =
	"<svg width='80' height='80'><defs>"
	"<marker id='m' markerWidth='20' markerHeight='20' refX='0' refY='0' "
	"markerUnits='userSpaceOnUse' orient='0' overflow='visible'>"
	"<path d='M0 -8 L16 0 L0 8 Z' fill='red'/></marker></defs>"
	"<polyline points='10,40 40,40 70,40' fill='none' stroke='green' stroke-width='10' "
	"marker-mid='url(#m)' paint-order='";

static void paint_order_slots(MarkerTest* ctx)
{
	char svgA[512], svgB[512];
	NSVGrasterizer* r = nsvgCreateRasterizer();
	NSVGimage* img;
	unsigned char* pa;
	snprintf(svgA, sizeof(svgA), "%smarkers stroke fill'/></svg>", ORDER_SVG_HEAD);
	snprintf(svgB, sizeof(svgB), "%sfill stroke markers'/></svg>", ORDER_SVG_HEAD);

	img = parse(svgA); pa = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* Markers first: the later green stroke covers the marker interior. */
	MARKER_CHECK(ctx, at(pa, 42, 40)[1] > at(pa, 42, 40)[0] && at(pa, 42, 40)[3] == 255);
	nsvgDelete(img); free(pa);

	img = parse(svgB); pa = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* Markers last: red marker stays on top. */
	MARKER_CHECK(ctx, at(pa, 42, 40)[0] == 255 && at(pa, 42, 40)[1] == 0);
	nsvgDelete(img); free(pa);
	nsvgDeleteRasterizer(r);
}

static void paint_sibling_occlusion(MarkerTest* ctx)
{
	const char* under =
		"<svg width='80' height='80'><defs>"
		"<marker id='m' markerWidth='20' markerHeight='20' refX='0' refY='0' "
	"markerUnits='userSpaceOnUse' orient='0' overflow='visible'>"
		"<path d='M0 -8 L16 0 L0 8 Z' fill='red'/></marker></defs>"
		"<polyline points='10,40 40,40 70,40' fill='none' stroke='green' stroke-width='6' marker-mid='url(#m)'/>"
		"<rect x='32' y='32' width='16' height='16' fill='blue'/></svg>";
	const char* over =
		"<svg width='80' height='80'><defs>"
		"<marker id='m' markerWidth='20' markerHeight='20' refX='0' refY='0' "
	"markerUnits='userSpaceOnUse' orient='0' overflow='visible'>"
		"<path d='M0 -8 L16 0 L0 8 Z' fill='red'/></marker></defs>"
		"<rect x='32' y='32' width='16' height='16' fill='blue'/>"
		"<polyline points='10,40 40,40 70,40' fill='none' stroke='green' stroke-width='6' marker-mid='url(#m)'/></svg>";
	NSVGrasterizer* r = nsvgCreateRasterizer();
	NSVGimage* img = parse(under);
	unsigned char* px = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* Later ordinary sibling paints over the earlier shape's marker. */
	MARKER_CHECK(ctx, at(px, 40, 40)[2] == 255 && at(px, 40, 40)[0] == 0);
	nsvgDelete(img); free(px);
	img = parse(over); px = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* Marker of the later sibling wins. */
	MARKER_CHECK(ctx, at(px, 40, 40)[0] == 255 && at(px, 40, 40)[2] == 0);
	nsvgDelete(img); free(px);
	nsvgDeleteRasterizer(r);
}

static void paint_marker_own_style(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'><defs>"
		"<marker id='def' markerWidth='8' markerHeight='8' refX='0' refY='0' orient='0' "
	"markerUnits='userSpaceOnUse' overflow='visible'>"
		"<path d='M0 -2 L6 0 L0 2 Z'/></marker>"
		"<marker id='stk' markerWidth='8' markerHeight='8' refX='0' refY='0' orient='0' "
	"markerUnits='userSpaceOnUse' overflow='visible'>"
		"<path d='M0 -4 L8 0 L0 4' fill='none' stroke='yellow' stroke-width='2'/></marker>"
		"</defs><line x1='5' y1='20' x2='30' y2='20' stroke='blue' stroke-width='2' marker-end='url(#def)'/>"
		"<line x1='5' y1='60' x2='30' y2='60' stroke='blue' stroke-width='2' marker-end='url(#stk)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* px = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* Marker keeps its definition-scope default fill (black), not the line blue. */
	MARKER_CHECK(ctx, at(px, 31, 20)[0] == 0 && at(px, 31, 20)[1] == 0 &&
	             at(px, 31, 20)[2] == 0 && at(px, 31, 20)[3] == 255);
	/* Stroke-only marker: yellow outline along the first segment. */
	MARKER_CHECK(ctx, at(px, 34, 58)[0] == 255 && at(px, 34, 58)[1] == 255 &&
	             at(px, 34, 58)[3] == 255);
	/* Marker definitions never enter image->shapes. */
	MARKER_CHECK(ctx, shapeCount(img) == 2);
	free(px);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
}

static void paint_definition_scope_inheritance(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'><defs>"
		"<marker id='m' markerWidth='8' markerHeight='8' refX='0' refY='0' orient='0' "
	"fill='#ff00ff' markerUnits='userSpaceOnUse' overflow='visible'>"
		"<path d='M0 -2 L6 0 L0 2 Z'/></marker></defs>"
		"<line x1='5' y1='30' x2='30' y2='30' stroke='blue' stroke-width='2' marker-end='url(#m)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* px = render(r, img);
	MARKER_CHECK(ctx, img != NULL);
	/* The child inherits the marker element's magenta fill, not line blue. */
	MARKER_CHECK(ctx, at(px, 32, 30)[0] == 255 && at(px, 32, 30)[1] == 0 &&
	             at(px, 32, 30)[2] == 255 && at(px, 32, 30)[3] >= 150);
	free(px);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
}

/* ------------------------------------------------------------------ */
/* reuse                                                               */
/* ------------------------------------------------------------------ */

static void reuse_shared_definition(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'><defs>" TRI_MARKER "</defs>"
		"<line x1='5' y1='20' x2='20' y2='20' stroke='black' marker-end='url(#tri)'/>"
		"<line x1='5' y1='60' x2='30' y2='60' stroke='black' marker-end='url(#tri)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	NSVGshape *s1, *s2;
	NSVGmarker* m = img->markers;
	float w0, h0, rx0, ry0;
	unsigned char *pa, *pb;
	int x, y, diffs = 0;
	MARKER_CHECK(ctx, img != NULL && m != NULL && m->next == NULL);
	s1 = img->shapes; s2 = s1->next;
	MARKER_CHECK(ctx, s1->markerPlacements != NULL && s2->markerPlacements != NULL);
	MARKER_CHECK(ctx, s1->markerPlacements->marker == s2->markerPlacements->marker);
	/* Independent vertices. */
	MARKER_CHECK(ctx, nearly(s1->markerPlacements->x, 20.0f) &&
	             nearly(s2->markerPlacements->x, 30.0f));
	w0 = m->markerWidth; h0 = m->markerHeight; rx0 = m->refX; ry0 = m->refY;
	pa = render(r, img);
	pb = render(r, img);
	/* Repeated rasterization is stable and leaves the definition untouched. */
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			int k;
			for (k = 0; k < 4; k++)
				if (abs((int)at(pa,x,y)[k] - (int)at(pb,x,y)[k]) > 2) diffs++;
		}
	MARKER_CHECK(ctx, diffs == 0);
	MARKER_CHECK(ctx, m->markerWidth == w0 && m->markerHeight == h0 &&
	             m->refX == rx0 && m->refY == ry0);
	free(pa); free(pb);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
}

static void reuse_definition_after_reference(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'>"
		"<line x1='10' y1='30' x2='40' y2='30' stroke='black' marker-end='url(#late)'/>"
		"<defs><marker id='late' markerWidth='8' markerHeight='8' refX='0' refY='0' "
	"markerUnits='userSpaceOnUse' orient='0' overflow='visible'>"
		"<path d='M0 -2 L6 0 L0 2 Z' fill='red'/></marker></defs></svg>";
	NSVGimage* img = parse(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* px;
	MARKER_CHECK(ctx, img != NULL);
	MARKER_CHECK(ctx, img->shapes->markerPlacements != NULL);
	px = render(r, img);
	MARKER_CHECK(ctx, at(px, 41, 30)[0] == 255 && at(px, 41, 30)[3] == 255);
	free(px);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
}

static void reuse_rasterizer_padded_stride(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='40' height='40'><defs>"
		"<marker id='m' markerWidth='6' markerHeight='6' refX='0' refY='0' "
	"markerUnits='userSpaceOnUse' orient='0' overflow='visible'>"
		"<path d='M0 -2 L6 0 L0 2 Z' fill='red'/></marker></defs>"
		"<line x1='5' y1='20' x2='20' y2='20' stroke='blue' stroke-width='2' marker-end='url(#m)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	const int w = 40, h = 40, pad = 8;
	const int stride = w*4 + pad;
	unsigned char* buf = (unsigned char*)malloc((size_t)h*stride);
	int y, x, paddingTouched = 0;
	MARKER_CHECK(ctx, img != NULL && buf != NULL);
	memset(buf, 0xAA, (size_t)h*stride);
	nsvgRasterize(r, img, 0, 0, 1, buf, w, h, stride);
	for (y = 0; y < h; y++)
		for (x = w*4; x < stride; x++)
			if (buf[y*stride + x] != 0xAA) paddingTouched++;
	MARKER_CHECK(ctx, paddingTouched == 0);
	MARKER_CHECK(ctx, buf[(20*stride) + 23*4] == 255); /* red marker interior */

	/* Different translate/scale on the reused rasterizer: no stale device
	 * coordinates are carried into this run. */
	memset(buf, 0xAA, (size_t)h*stride);
	nsvgRasterize(r, img, 4, 4, 1, buf, w, h, stride);
	MARKER_CHECK(ctx, buf[(24*stride) + 27*4] == 255); /* shifted by (4,4) */
	MARKER_CHECK(ctx, buf[(20*stride) + 23*4 + 3] == 0); /* old spot cleared */
	for (y = 0; y < h; y++)
		for (x = w*4; x < stride; x++)
			if (buf[y*stride + x] != 0xAA) paddingTouched++;
	MARKER_CHECK(ctx, paddingTouched == 0);
	free(buf);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
}

static void reuse_image_scale(MarkerTest* ctx)
{
	const char* svg =
		"<svg width='80' height='80'><defs>" TRI_MARKER "</defs>"
		"<line x1='10' y1='30' x2='20' y2='30' stroke='black' marker-end='url(#tri)'/></svg>";
	NSVGimage* img = parse(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* a = (unsigned char*)calloc(W*H*4, 1);
	unsigned char* b = (unsigned char*)calloc(W*H*4, 1);
	MARKER_CHECK(ctx, img != NULL);
	nsvgRasterize(r, img, 0, 0, 1, a, W, H, W*4);
	nsvgRasterize(r, img, 0, 0, 2, b, W, H, W*4);
	/* Vertex (20,30), tip content x=8: tip at 28 at scale 1, 56 at scale 2. */
	MARKER_CHECK(ctx, at(a, 27, 30)[0] == 255);
	MARKER_CHECK(ctx, at(b, 55, 60)[0] == 255);
	MARKER_CHECK(ctx, at(b, 27, 30)[3] == 0);
	free(a); free(b);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
}

static void reuse_file_entry(MarkerTest* ctx)
{
	const char* dir = getenv("TMPDIR");
	char path[256];
	const char* svg =
		"<svg width='40' height='40' xmlns='http://www.w3.org/2000/svg'><defs>"
		"<marker id='m' markerWidth='6' markerHeight='6' refX='0' refY='0' "
	"markerUnits='userSpaceOnUse' orient='0' overflow='visible'>"
		"<path d='M0 -2 L6 0 L0 2 Z' fill='red'/></marker></defs>"
		"<line x1='5' y1='20' x2='20' y2='20' stroke='blue' stroke-width='2' marker-end='url(#m)'/></svg>";
	FILE* fp;
	NSVGimage* img;
	NSVGrasterizer* r;
	unsigned char* px;
	MARKER_CHECK(ctx, dir != NULL);
	snprintf(path, sizeof(path), "%s/nanosvg_marker_file_entry.svg", dir);
	fp = fopen(path, "wb");
	MARKER_CHECK(ctx, fp != NULL);
	fputs(svg, fp);
	fclose(fp);

	img = nsvgParseFromFile(path, "px", 96);
	MARKER_CHECK(ctx, img != NULL);
	r = nsvgCreateRasterizer();
	px = (unsigned char*)calloc(40*40*4, 1);
	nsvgRasterize(r, img, 0, 0, 1, px, 40, 40, 160);
	MARKER_CHECK(ctx, (px+(20*160) + 23*4)[0] == 255);
	free(px);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
	remove(path);
}

const MarkerCase marker_cases[] = {
	/* topology */
	{ "topology", "arc_one_segment", topo_arc_one_segment },
	{ "topology", "polyline_vertices", topo_polyline_vertices },
	{ "topology", "closed_seam", topo_closed_seam },
	{ "topology", "closed_then_continue", topo_closed_then_continue },
	{ "topology", "zero_length_and_m_only", topo_zero_length_and_m_only },
	{ "topology", "none_and_missing", topo_none_and_missing },
	{ "topology", "style_scope_and_dimensions", topo_style_scope_and_dimensions },
	/* coordinates */
	{ "coordinates", "orient_auto", coord_orient_auto },
	{ "coordinates", "orient_fixed_and_reverse", coord_orient_fixed_and_reverse },
	{ "coordinates", "mid_bisector", coord_mid_bisector },
	{ "coordinates", "ref_anchor_pixels", coord_ref_anchor_pixels },
	{ "coordinates", "viewbox_meet_and_none", coord_viewbox_meet_and_none },
	{ "coordinates", "nonuniform_transform", coord_nonuniform_transform },
	{ "coordinates", "nested_group_transform", coord_nested_group_transform },
	{ "coordinates", "marker_units", coord_marker_units },
	{ "coordinates", "marker_stroke_scale", coord_marker_stroke_scale },
	/* paint */
	{ "paint", "order_slots", paint_order_slots },
	{ "paint", "sibling_occlusion", paint_sibling_occlusion },
	{ "paint", "marker_own_style", paint_marker_own_style },
	{ "paint", "definition_scope_inheritance", paint_definition_scope_inheritance },
	/* reuse */
	{ "reuse", "shared_definition", reuse_shared_definition },
	{ "reuse", "definition_after_reference", reuse_definition_after_reference },
	{ "reuse", "rasterizer_padded_stride", reuse_rasterizer_padded_stride },
	{ "reuse", "image_scale", reuse_image_scale },
	{ "reuse", "file_entry", reuse_file_entry },
};

const size_t marker_case_count = sizeof(marker_cases)/sizeof(marker_cases[0]);
