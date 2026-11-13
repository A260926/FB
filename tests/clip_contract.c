/* Clip-path contract tests.
 *
 * Every expectation is computed here from the test's own inputs and matrices;
 * product helpers (bounds, flattening, fills) are never used to derive answers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "clip_test.h"

/* ------------------------------------------------------------------ */
/* helpers                                                            */

static NSVGimage* parseStr(const char* s)
{
	char* copy = (char*)malloc(strlen(s) + 1);
	NSVGimage* image;
	if (copy == NULL) return NULL;
	strcpy(copy, s);
	image = nsvgParse(copy, "px", 96.0f);
	free(copy);
	return image;
}

typedef struct Canvas {
	int w, h, stride;
	unsigned char* px;
} Canvas;

static int canvasInit(Canvas* c, int w, int h, int stride)
{
	c->w = w; c->h = h; c->stride = stride;
	c->px = (unsigned char*)malloc((size_t)stride * h);
	if (c->px == NULL) return 0;
	memset(c->px, 0, (size_t)stride * h);
	return 1;
}

static void canvasFree(Canvas* c)
{
	free(c->px);
	c->px = NULL;
}

static void render(NSVGrasterizer* r, NSVGimage* image, Canvas* c,
				   float tx, float ty, float scale)
{
	nsvgRasterize(r, image, tx, ty, scale, c->px, c->w, c->h, c->stride);
}

static int alphaAt(const Canvas* c, int x, int y)
{
	if (x < 0 || y < 0 || x >= c->w || y >= c->h) return 0;
	return c->px[y * c->stride + x * 4 + 3];
}

static int redAt(const Canvas* c, int x, int y)
{
	if (x < 0 || y < 0 || x >= c->w || y >= c->h) return 0;
	return c->px[y * c->stride + x * 4 + 0];
}

/* Render one SVG string and return the alpha at a pixel. */
static int alphaOf(const char* svg, int x, int y, int w, int h)
{
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r;
	Canvas c;
	int a;
	if (image == NULL) return -1;
	r = nsvgCreateRasterizer();
	if (r == NULL) { nsvgDelete(image); return -1; }
	if (!canvasInit(&c, w, h, w * 4)) { nsvgDeleteRasterizer(r); nsvgDelete(image); return -1; }
	render(r, image, &c, 0, 0, 1.0f);
	a = alphaAt(&c, x, y);
	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
	return a;
}

/* ------------------------------------------------------------------ */
/* geometry: compound outlines, per-child rules, child union          */

static void caseRectClipBaseline(ClipTest* t)
{
	/* 16x16 red rect [2,14]x[2,14], clip rect [2,7]x[2,14]. */
	const char* svg =
		"<svg width='16' height='16'><defs><clipPath id='c'>"
		"<rect x='2' y='2' width='5' height='12'/></clipPath></defs>"
		"<rect x='2' y='2' width='12' height='12' fill='red' clip-path='url(#c)'/></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r;
	Canvas c;
	int inside, outside, untouched;

	CLIP_CHECK(t, image != NULL);
	if (image == NULL) return;
	r = nsvgCreateRasterizer();
	CLIP_CHECK(t, r != NULL);
	if (!canvasInit(&c, 16, 16, 16 * 4)) { nsvgDeleteRasterizer(r); nsvgDelete(image); return; }
	render(r, image, &c, 0, 0, 1.0f);

	inside = alphaAt(&c, 4, 6);
	outside = alphaAt(&c, 11, 6);
	untouched = alphaAt(&c, 0, 0);
	CLIP_CHECK(t, inside == 255);
	CLIP_CHECK(t, outside == 0);
	CLIP_CHECK(t, untouched == 0);
	CLIP_CHECK(t, redAt(&c, 4, 6) == 255);

	/* Shape list order, path coordinates and bounds stay usable. */
	{
		NSVGshape* shape = image->shapes;
		int count = 0;
		CLIP_CHECK(t, shape != NULL);
		for (; shape != NULL; shape = shape->next) count++;
		CLIP_CHECK(t, count == 1);
		shape = image->shapes;
		CLIP_CHECK(t, shape->paths != NULL);
		CLIP_CHECK(t, shape->paths->npts >= 4);
		CLIP_CHECK(t, shape->bounds[0] == 2.0f && shape->bounds[1] == 2.0f);
		CLIP_CHECK(t, shape->bounds[2] == 14.0f && shape->bounds[3] == 14.0f);
	}

	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseEvenOddHole(ClipTest* t)
{
	/* Square with a square hole, evenodd.  Centre must stay transparent. */
	const char* svg =
		"<svg width='20' height='20'><defs>"
		"<clipPath id='c' clip-rule='evenodd'>"
		"<path d='M2,2 L18,2 L18,18 L2,18 Z M6,6 L14,6 L14,14 L6,14 Z'/>"
		"</clipPath></defs>"
		"<rect x='0' y='0' width='20' height='20' fill='red' clip-path='url(#c)'/></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r;
	Canvas c;

	CLIP_CHECK(t, image != NULL);
	if (image == NULL) return;
	r = nsvgCreateRasterizer();
	if (!canvasInit(&c, 20, 20, 20 * 4)) { nsvgDeleteRasterizer(r); nsvgDelete(image); return; }
	render(r, image, &c, 0, 0, 1.0f);

	CLIP_CHECK(t, alphaAt(&c, 3, 3) == 255);	/* in the ring */
	CLIP_CHECK(t, alphaAt(&c, 10, 10) == 0);	/* in the hole */
	CLIP_CHECK(t, alphaAt(&c, 1, 1) == 0);		/* outside */

	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseEvenOddPropertyAndStyle(ClipTest* t)
{
	/* clip-rule via style must behave like the attribute form. */
	const char* attrSvg =
		"<svg width='20' height='20'><defs>"
		"<clipPath id='c' clip-rule='evenodd'>"
		"<path d='M2,2 L18,2 L18,18 L2,18 Z M6,6 L14,6 L14,14 L6,14 Z'/></clipPath></defs>"
		"<rect width='20' height='20' fill='red' clip-path='url(#c)'/></svg>";
	const char* styleSvg =
		"<svg width='20' height='20'><defs>"
		"<clipPath id='c'><path d='M2,2 L18,2 L18,18 L2,18 Z M6,6 L14,6 L14,14 L6,14 Z'"
		" style='clip-rule:evenodd'/></clipPath></defs>"
		"<rect width='20' height='20' fill='red' style='clip-path:url(#c)'/></svg>";
	NSVGimage* a = parseStr(attrSvg);
	NSVGimage* b = parseStr(styleSvg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	Canvas ca, cb;

	CLIP_CHECK(t, a != NULL && b != NULL && r != NULL);
	if (!a || !b || !r) { nsvgDelete(a); nsvgDelete(b); nsvgDeleteRasterizer(r); return; }
	if (!canvasInit(&ca, 20, 20, 20 * 4) || !canvasInit(&cb, 20, 20, 20 * 4)) {
		nsvgDelete(a); nsvgDelete(b); nsvgDeleteRasterizer(r);
		return;
	}
	render(r, a, &ca, 0, 0, 1.0f);
	render(r, b, &cb, 0, 0, 1.0f);
	CLIP_CHECK(t, alphaAt(&ca, 10, 10) == 0);
	CLIP_CHECK(t, alphaAt(&cb, 10, 10) == 0);
	CLIP_CHECK(t, alphaAt(&ca, 3, 10) == 255);
	CLIP_CHECK(t, alphaAt(&cb, 3, 10) == 255);

	canvasFree(&ca);
	canvasFree(&cb);
	nsvgDeleteRasterizer(r);
	nsvgDelete(a);
	nsvgDelete(b);
}

static void caseChildUnionOppositeWinding(ClipTest* t)
{
	/* Two overlapping children, one wound the other way.  The union must not
	   punch a hole where they overlap. */
	const char* svg =
		"<svg width='24' height='16'><defs><clipPath id='c'>"
		"<path d='M2,2 L12,2 L12,14 L2,14 Z'/>"
		"<path d='M8,16 L8,4 L20,4 L20,16 Z'/>"
		"</clipPath></defs>"
		"<rect width='24' height='16' fill='red' clip-path='url(#c)'/></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r;
	Canvas c;

	CLIP_CHECK(t, image != NULL);
	if (image == NULL) return;
	r = nsvgCreateRasterizer();
	if (!canvasInit(&c, 24, 16, 24 * 4)) { nsvgDeleteRasterizer(r); nsvgDelete(image); return; }
	render(r, image, &c, 0, 0, 1.0f);

	CLIP_CHECK(t, alphaAt(&c, 3, 8) == 255);	/* first child */
	CLIP_CHECK(t, alphaAt(&c, 16, 8) == 255);	/* second child */
	CLIP_CHECK(t, alphaAt(&c, 9, 8) == 255);	/* overlap union */
	CLIP_CHECK(t, alphaAt(&c, 1, 8) == 0);
	CLIP_CHECK(t, alphaAt(&c, 22, 8) == 0);

	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseChildFillAndOpacityIgnored(ClipTest* t)
{
	/* fill:none and opacity:0 children still clip; display:none does not. */
	const char* svg =
		"<svg width='20' height='20'><defs><clipPath id='c'>"
		"<rect x='1' y='1' width='5' height='18' fill='none' opacity='0'/>"
		"<rect x='12' y='1' width='5' height='18' display='none'/>"
		"</clipPath></defs>"
		"<rect width='20' height='20' fill='red' clip-path='url(#c)'/></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r;
	Canvas c;

	CLIP_CHECK(t, image != NULL);
	if (image == NULL) return;
	r = nsvgCreateRasterizer();
	if (!canvasInit(&c, 20, 20, 20 * 4)) { nsvgDeleteRasterizer(r); nsvgDelete(image); return; }
	render(r, image, &c, 0, 0, 1.0f);

	CLIP_CHECK(t, alphaAt(&c, 3, 10) == 255);
	CLIP_CHECK(t, alphaAt(&c, 14, 10) == 0);
	CLIP_CHECK(t, alphaAt(&c, 9, 10) == 0);

	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseDefinitionAndChildTransforms(ClipTest* t)
{
	/* clipPath transform plus a transformed child.  Expected coverage is
	   derived by applying the same matrices in the test. */
	/* clipPath translate(3,0), child translate(1,0): effective +4. */
	CLIP_CHECK(t, alphaOf("<svg width='20' height='20'><defs><clipPath id='c'"
						  " transform='translate(3,0)'>"
						  "<rect x='0' y='0' width='4' height='20'"
						  " transform='translate(1,0)'/></clipPath></defs>"
						  "<rect width='20' height='20' fill='red'"
						  " clip-path='url(#c)'/></svg>", 5, 10, 20, 20) == 255);
	CLIP_CHECK(t, alphaOf("<svg width='20' height='20'><defs><clipPath id='c'"
						  " transform='translate(3,0)'>"
						  "<rect x='0' y='0' width='4' height='20'"
						  " transform='translate(1,0)'/></clipPath></defs>"
						  "<rect width='20' height='20' fill='red'"
						  " clip-path='url(#c)'/></svg>", 3, 10, 20, 20) == 0);
	/* Child scale about the origin: unit clip becomes 2x2 at (2,2). */
	CLIP_CHECK(t, alphaOf("<svg width='16' height='16'><defs><clipPath id='c'>"
						  "<rect x='1' y='1' width='1' height='1'"
						  " transform='translate(2,2) scale(2)'/></clipPath></defs>"
						  "<rect width='16' height='16' fill='red'"
						  " clip-path='url(#c)'/></svg>", 5, 5, 16, 16) == 255);
	CLIP_CHECK(t, alphaOf("<svg width='16' height='16'><defs><clipPath id='c'>"
						  "<rect x='1' y='1' width='1' height='1'"
						  " transform='translate(2,2) scale(2)'/></clipPath></defs>"
						  "<rect width='16' height='16' fill='red'"
						  " clip-path='url(#c)'/></svg>", 1, 5, 16, 16) == 0);
	/* Rotated child: the region moves to the opposite side of the shape. */
	CLIP_CHECK(t, alphaOf("<svg width='20' height='20'><defs><clipPath id='c'>"
						  "<rect x='0' y='0' width='5' height='5'"
						  " transform='rotate(180 10 10)'/></clipPath></defs>"
						  "<rect width='20' height='20' fill='red'"
						  " clip-path='url(#c)'/></svg>", 15, 15, 20, 20) == 255);
	CLIP_CHECK(t, alphaOf("<svg width='20' height='20'><defs><clipPath id='c'>"
						  "<rect x='0' y='0' width='5' height='5'"
						  " transform='rotate(180 10 10)'/></clipPath></defs>"
						  "<rect width='20' height='20' fill='red'"
						  " clip-path='url(#c)'/></svg>", 2, 2, 20, 20) == 0);
}

static void caseCurveInsideOutsideAndEdge(ClipTest* t)
{
	/* Curved clip (circle) clipped against an identical circle: every pixel
	   matches the unclipped render, and a hole of the same curve is checked. */
	const char* plain =
		"<svg width='32' height='32'><circle cx='16' cy='16' r='10' fill='red'/></svg>";
	const char* clipped =
		"<svg width='32' height='32'><defs><clipPath id='c'>"
		"<circle cx='16' cy='16' r='10'/></clipPath></defs>"
		"<circle cx='16' cy='16' r='10' fill='red' clip-path='url(#c)'/></svg>";
	const char* donut =
		"<svg width='32' height='32'><defs><clipPath id='c' clip-rule='evenodd'>"
		"<path d='M16,4 a12,12 0 1,1 0,24 a12,12 0 1,1 0,-24 z"
		" M16,10 a6,6 0 1,1 0,12 a6,6 0 1,1 0,-12 z'/></clipPath></defs>"
		"<rect width='32' height='32' fill='red' clip-path='url(#c)'/></svg>";
	NSVGimage* a = parseStr(plain);
	NSVGimage* b = parseStr(clipped);
	NSVGimage* d = parseStr(donut);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	Canvas ca, cb, cd;
	int x, y, maxDiff = 0, edge = 0;

	CLIP_CHECK(t, a && b && d && r);
	if (!a || !b || !d || !r) { nsvgDelete(a); nsvgDelete(b); nsvgDelete(d); nsvgDeleteRasterizer(r); return; }
	if (!canvasInit(&ca, 32, 32, 32 * 4) || !canvasInit(&cb, 32, 32, 32 * 4)) {
		nsvgDelete(a); nsvgDelete(b); nsvgDelete(d); nsvgDeleteRasterizer(r); return;
	}
	render(r, a, &ca, 0, 0, 1.0f);
	render(r, b, &cb, 0, 0, 1.0f);
	for (y = 0; y < 32; y++) {
		for (x = 0; x < 32; x++) {
			int av = alphaAt(&ca, x, y);
			int bv = alphaAt(&cb, x, y);
			int diff = av > bv ? av - bv : bv - av;
			if (diff > maxDiff) maxDiff = diff;
			if (av > 0 && av < 255) edge++;
		}
	}
	CLIP_CHECK(t, edge > 0);
	CLIP_CHECK(t, maxDiff <= 2);

	if (canvasInit(&cd, 32, 32, 32 * 4)) {
		render(r, d, &cd, 0, 0, 1.0f);
		CLIP_CHECK(t, alphaAt(&cd, 16, 16) == 0);	/* hole */
		CLIP_CHECK(t, alphaAt(&cd, 16, 6) == 255);	/* ring */
		CLIP_CHECK(t, alphaAt(&cd, 1, 16) == 0);	/* outside */
		canvasFree(&cd);
	} else {
		CLIP_CHECK(t, 0);
	}

	canvasFree(&ca);
	canvasFree(&cb);
	nsvgDeleteRasterizer(r);
	nsvgDelete(a);
	nsvgDelete(b);
	nsvgDelete(d);
}

/* ------------------------------------------------------------------ */
/* scopes: group bounds, nesting, non-inheritance                     */

static void caseGroupClipAppliesAndRestores(ClipTest* t)
{
	const char* svg =
		"<svg width='20' height='20'><defs><clipPath id='c'>"
		"<rect x='0' y='0' width='8' height='20'/></clipPath></defs>"
		"<g clip-path='url(#c)'>"
		"<rect x='0' y='0' width='20' height='10' fill='red'/>"
		"</g>"
		"<rect x='12' y='12' width='6' height='6' fill='red'/></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r;
	Canvas c;

	CLIP_CHECK(t, image != NULL);
	if (image == NULL) return;
	r = nsvgCreateRasterizer();
	if (!canvasInit(&c, 20, 20, 20 * 4)) { nsvgDeleteRasterizer(r); nsvgDelete(image); return; }
	render(r, image, &c, 0, 0, 1.0f);

	CLIP_CHECK(t, alphaAt(&c, 3, 5) == 255);	/* group content inside clip */
	CLIP_CHECK(t, alphaAt(&c, 15, 5) == 0);		/* group content outside clip */
	CLIP_CHECK(t, alphaAt(&c, 15, 15) == 255);	/* sibling after group restored */
	CLIP_CHECK(t, alphaAt(&c, 3, 15) == 0);

	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseNestedGroupIntersection(ClipTest* t)
{
	/* Two nested group clips: their regions intersect. */
	const char* svg =
		"<svg width='20' height='20'><defs>"
		"<clipPath id='v'><rect x='0' y='0' width='12' height='20'/></clipPath>"
		"<clipPath id='h'><rect x='0' y='0' width='20' height='12'/></clipPath>"
		"</defs>"
		"<g clip-path='url(#v)'><g clip-path='url(#h)'>"
		"<rect width='20' height='20' fill='red'/></g></g></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r;
	Canvas c;

	CLIP_CHECK(t, image != NULL);
	if (image == NULL) return;
	r = nsvgCreateRasterizer();
	if (!canvasInit(&c, 20, 20, 20 * 4)) { nsvgDeleteRasterizer(r); nsvgDelete(image); return; }
	render(r, image, &c, 0, 0, 1.0f);

	CLIP_CHECK(t, alphaAt(&c, 5, 5) == 255);
	CLIP_CHECK(t, alphaAt(&c, 15, 5) == 0);
	CLIP_CHECK(t, alphaAt(&c, 5, 15) == 0);
	CLIP_CHECK(t, alphaAt(&c, 15, 15) == 0);

	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseClipNoneDoesNotOverrideAncestor(ClipTest* t)
{
	const char* svg =
		"<svg width='20' height='20'><defs><clipPath id='c'>"
		"<rect x='0' y='0' width='8' height='20'/></clipPath></defs>"
		"<g clip-path='url(#c)'><rect width='20' height='20' fill='red'"
		" clip-path='none'/></g></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r;
	Canvas c;

	CLIP_CHECK(t, image != NULL);
	if (image == NULL) return;
	r = nsvgCreateRasterizer();
	if (!canvasInit(&c, 20, 20, 20 * 4)) { nsvgDeleteRasterizer(r); nsvgDelete(image); return; }
	render(r, image, &c, 0, 0, 1.0f);
	CLIP_CHECK(t, alphaAt(&c, 3, 10) == 255);
	CLIP_CHECK(t, alphaAt(&c, 15, 10) == 0);
	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseGroupBoundingBoxUsesDescendants(ClipTest* t)
{
	/* The group's bbox spans both children: x 2..18, y 2..14.  A unit-square
	   half clip covers x 2..10.  Individual-child normalisation would give a
	   different split. */
	const char* svg =
		"<svg width='24' height='20'><defs><clipPath id='c'"
		" clipPathUnits='objectBoundingBox'><rect x='0' y='0' width='0.5' height='1'/>"
		"</clipPath></defs>"
		"<g clip-path='url(#c)'>"
		"<rect x='2' y='2' width='6' height='12' fill='red'/>"
		"<rect x='12' y='4' width='6' height='8' fill='red'/>"
		"</g></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r;
	Canvas c;

	CLIP_CHECK(t, image != NULL);
	if (image == NULL) return;
	r = nsvgCreateRasterizer();
	if (!canvasInit(&c, 24, 20, 24 * 4)) { nsvgDeleteRasterizer(r); nsvgDelete(image); return; }
	render(r, image, &c, 0, 0, 1.0f);

	CLIP_CHECK(t, alphaAt(&c, 3, 8) == 255);	/* first child, left half */
	CLIP_CHECK(t, alphaAt(&c, 14, 8) == 0);		/* second child beyond half */
	CLIP_CHECK(t, alphaAt(&c, 8, 8) == 0);		/* gap between children */

	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseGroupBoundingBoxTransform(ClipTest* t)
{
	/* Group transform scales its children; the bbox is measured in the
	   group's own coordinates (children span x 1..7, y 1..7). */
	const char* svg =
		"<svg width='32' height='20'><defs><clipPath id='c'"
		" clipPathUnits='objectBoundingBox'><rect x='0' y='0' width='0.5' height='1'/>"
		"</clipPath></defs>"
		"<g clip-path='url(#c)' transform='translate(4,0) scale(2)'>"
		"<rect x='1' y='1' width='6' height='6' fill='red'/>"
		"</g></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r;
	Canvas c;

	CLIP_CHECK(t, image != NULL);
	if (image == NULL) return;
	r = nsvgCreateRasterizer();
	if (!canvasInit(&c, 32, 20, 32 * 4)) { nsvgDeleteRasterizer(r); nsvgDelete(image); return; }
	render(r, image, &c, 0, 0, 1.0f);

	/* Child occupies x 6..18, y 2..14; left half is x 6..12. */
	CLIP_CHECK(t, alphaAt(&c, 8, 8) == 255);
	CLIP_CHECK(t, alphaAt(&c, 15, 8) == 0);
	CLIP_CHECK(t, alphaAt(&c, 4, 8) == 0);

	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseDefinitionAfterReference(ClipTest* t)
{
	const char* svg =
		"<svg width='16' height='16'>"
		"<rect x='2' y='2' width='12' height='12' fill='red' clip-path='url(#late)'/>"
		"<defs><clipPath id='late'><rect x='2' y='2' width='5' height='12'/></clipPath></defs>"
		"</svg>";
	CLIP_CHECK(t, alphaOf(svg, 4, 6, 16, 16) == 255);
	CLIP_CHECK(t, alphaOf(svg, 11, 6, 16, 16) == 0);
}

static void caseEmptyMissingAndInvalid(ClipTest* t)
{
	const char* empty =
		"<svg width='20' height='20'><defs><clipPath id='e'/></defs>"
		"<rect width='20' height='20' fill='red' clip-path='url(#e)'/>"
		"<rect x='0' y='0' width='4' height='4' fill='blue'/></svg>";
	const char* missing =
		"<svg width='20' height='20'>"
		"<rect width='20' height='20' fill='red' clip-path='url(#gone)'/>"
		"<rect x='0' y='0' width='4' height='4' fill='blue'/></svg>";
	const char* badTransform =
		"<svg width='20' height='20'><defs><clipPath id='b' transform='scale(0,1)'>"
		"<rect x='2' y='2' width='5' height='16'/></clipPath></defs>"
		"<rect width='20' height='20' fill='red' clip-path='url(#b)'/>"
		"<rect x='0' y='0' width='4' height='4' fill='blue'/></svg>";
	const char* zeroBox =
		"<svg width='20' height='20'><defs><clipPath id='z'"
		" clipPathUnits='objectBoundingBox'><rect x='0' y='0' width='1' height='1'/>"
		"</clipPath></defs>"
		"<rect x='8' y='4' width='0' height='8' fill='red' stroke='red'"
		" stroke-width='2' clip-path='url(#z)'/>"
		"<rect x='0' y='0' width='4' height='4' fill='blue'/></svg>";

	CLIP_CHECK(t, alphaOf(empty, 10, 10, 20, 20) == 0);
	CLIP_CHECK(t, alphaOf(empty, 2, 2, 20, 20) == 255);	/* sibling still drawn */
	CLIP_CHECK(t, alphaOf(missing, 10, 10, 20, 20) == 0);
	CLIP_CHECK(t, alphaOf(missing, 2, 2, 20, 20) == 255);
	CLIP_CHECK(t, alphaOf(badTransform, 10, 10, 20, 20) == 0);
	CLIP_CHECK(t, alphaOf(badTransform, 2, 2, 20, 20) == 255);
	CLIP_CHECK(t, alphaOf(zeroBox, 8, 8, 20, 20) == 0);
	CLIP_CHECK(t, alphaOf(zeroBox, 2, 2, 20, 20) == 255);
}

static void caseGroupBoundsIgnoreFillAndOpacityOnlyDisplay(ClipTest* t)
{
	/* The second child has fill:none / opacity:0 but still contributes its
	   geometry to the group bbox; a display:none child does not. */
	const char* svg =
		"<svg width='40' height='20'><defs><clipPath id='c'"
		" clipPathUnits='objectBoundingBox'><rect x='0' y='0' width='0.25' height='1'/>"
		"</clipPath></defs>"
		"<g clip-path='url(#c)'>"
		"<rect x='0' y='0' width='4' height='10' fill='red'/>"
		"<rect x='28' y='0' width='8' height='10' fill='none' opacity='0'/>"
		"</g></svg>";
	const char* hidden =
		"<svg width='40' height='20'><defs><clipPath id='c'"
		" clipPathUnits='objectBoundingBox'><rect x='0' y='0' width='0.875' height='1'/>"
		"</clipPath></defs>"
		"<g clip-path='url(#c)'>"
		"<rect x='0' y='0' width='4' height='10' fill='red'/>"
		"<rect x='28' y='0' width='8' height='10' display='none' fill='red'/>"
		"</g></svg>";

	/* bbox 0..36 -> quarter is 0..9: only the first child shows. */
	CLIP_CHECK(t, alphaOf(svg, 2, 5, 40, 20) == 255);
	CLIP_CHECK(t, alphaOf(svg, 20, 5, 40, 20) == 0);
	CLIP_CHECK(t, alphaOf(svg, 30, 5, 40, 20) == 0);
	/* With the far child hidden, bbox is 0..4 so 0.875 covers x 0..3.5. */
	CLIP_CHECK(t, alphaOf(hidden, 2, 5, 40, 20) == 255);
	CLIP_CHECK(t, alphaOf(hidden, 5, 5, 40, 20) == 0);
}

/* ------------------------------------------------------------------ */
/* coverage: sub-pixel geometric intersection                         */

/* Independent reference coverage for one pixel of a row.  Along each of the
   five horizontal sample lines y+0.1 .. y+0.9, compute the length of the
   geometric intersection of the fill and clip x intervals within the pixel
   span [px, px+1), then average and scale by 255.  Row 8 lies fully inside
   both rectangles vertically, so the vertical extent is a full pixel. */
static double refPixelCoverage(double px, double fillX0, double fillX1,
							   double clipX0, double clipX1)
{
	static const double offs[5] = {0.1, 0.3, 0.5, 0.7, 0.9};
	double total = 0;
	int i;
	for (i = 0; i < 5; i++) {
		double a = fillX0 > clipX0 ? fillX0 : clipX0;
		double b = fillX1 < clipX1 ? fillX1 : clipX1;
		double lo = a > px ? a : px;
		double hi = b < px + 1.0 ? b : px + 1.0;
		(void)offs[i];
		if (hi > lo) total += hi - lo;
	}
	return total / 5.0 * 255.0;
}

static void caseSharedFractionalEdge(ClipTest* t)
{
	/* Fill and clip share the same fractional left edge at x=2.3.  The clipped
	   result must match the unclipped one exactly. */
	const char* plain =
		"<svg width='16' height='16'><rect x='2.3' y='0' width='10' height='16' fill='red'/></svg>";
	const char* clipped =
		"<svg width='16' height='16'><defs><clipPath id='c'>"
		"<rect x='2.3' y='0' width='10' height='16'/></clipPath></defs>"
		"<rect x='2.3' y='0' width='10' height='16' fill='red' clip-path='url(#c)'/></svg>";
	NSVGimage* a = parseStr(plain);
	NSVGimage* b = parseStr(clipped);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	Canvas ca, cb;
	int x, y, maxDiff = 0, halfPixel = 0;

	CLIP_CHECK(t, a && b && r);
	if (!a || !b || !r) { nsvgDelete(a); nsvgDelete(b); nsvgDeleteRasterizer(r); return; }
	if (!canvasInit(&ca, 16, 16, 16 * 4) || !canvasInit(&cb, 16, 16, 16 * 4)) {
		nsvgDelete(a); nsvgDelete(b); nsvgDeleteRasterizer(r); return;
	}
	render(r, a, &ca, 0, 0, 1.0f);
	render(r, b, &cb, 0, 0, 1.0f);
	for (y = 0; y < 16; y++) {
		for (x = 0; x < 16; x++) {
			int av = alphaAt(&ca, x, y);
			int bv = alphaAt(&cb, x, y);
			int diff = av > bv ? av - bv : bv - av;
			if (diff > maxDiff) maxDiff = diff;
			if (av > 0 && av < 255) halfPixel++;
		}
	}
	CLIP_CHECK(t, halfPixel > 0);
	CLIP_CHECK(t, maxDiff <= 2);
	/* x=2 must keep partial coverage near 0.7 * 255; not a 0.35 artefact. */
	CLIP_CHECK(t, alphaAt(&cb, 2, 8) >= 170 && alphaAt(&cb, 2, 8) <= 185);
	canvasFree(&ca);
	canvasFree(&cb);
	nsvgDeleteRasterizer(r);
	nsvgDelete(a);
	nsvgDelete(b);
}

static void caseDisjointHalves(ClipTest* t)
{
	/* Shape covers the left half of a pixel, clip covers the right half.
	   Each alone is ~50% covered; the intersection must be 0. */
	const char* svg =
		"<svg width='16' height='16'><defs><clipPath id='c'>"
		"<rect x='8.5' y='0' width='6' height='16'/></clipPath></defs>"
		"<rect x='0' y='0' width='8.5' height='16' fill='red'"
		" clip-path='url(#c)'/></svg>";
	const char* fillOnly =
		"<svg width='16' height='16'><rect x='0' y='0' width='8.5' height='16'"
		" fill='red'/></svg>";
	const char* clipOnly =
		"<svg width='16' height='16'><defs><clipPath id='c'>"
		"<rect x='8.5' y='0' width='6' height='16'/></clipPath></defs>"
		"<rect x='8.5' y='0' width='6' height='16' fill='red'/></svg>";
	CLIP_CHECK(t, alphaOf(svg, 8, 8, 16, 16) == 0);
	CLIP_CHECK(t, alphaOf(fillOnly, 8, 8, 16, 16) > 100);
	CLIP_CHECK(t, alphaOf(clipOnly, 8, 8, 16, 16) > 100);
}

static void caseRepeatedClipNoDarkening(ClipTest* t)
{
	/* Clipping twice with the same region must equal clipping once. */
	const char* once =
		"<svg width='16' height='16'><defs><clipPath id='c'>"
		"<rect x='2.4' y='1.6' width='9.2' height='12.8'/></clipPath></defs>"
		"<rect x='1.7' y='1.9' width='10.6' height='12.2' fill='red'"
		" clip-path='url(#c)'/></svg>";
	const char* twice =
		"<svg width='16' height='16'><defs>"
		"<clipPath id='c'><rect x='2.4' y='1.6' width='9.2' height='12.8'/></clipPath>"
		"<clipPath id='d'><rect x='2.4' y='1.6' width='9.2' height='12.8'/></clipPath>"
		"</defs>"
		"<g clip-path='url(#d)'>"
		"<rect x='1.7' y='1.9' width='10.6' height='12.2' fill='red'"
		" clip-path='url(#c)'/></g></svg>";
	NSVGimage* a = parseStr(once);
	NSVGimage* b = parseStr(twice);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	Canvas ca, cb;
	int x, y, maxDiff = 0;

	CLIP_CHECK(t, a && b && r);
	if (!a || !b || !r) { nsvgDelete(a); nsvgDelete(b); nsvgDeleteRasterizer(r); return; }
	if (!canvasInit(&ca, 16, 16, 16 * 4) || !canvasInit(&cb, 16, 16, 16 * 4)) {
		nsvgDelete(a); nsvgDelete(b); nsvgDeleteRasterizer(r); return;
	}
	render(r, a, &ca, 0, 0, 1.0f);
	render(r, b, &cb, 0, 0, 1.0f);
	for (y = 0; y < 16; y++) {
		for (x = 0; x < 16; x++) {
			int av = alphaAt(&ca, x, y);
			int bv = alphaAt(&cb, x, y);
			int diff = av > bv ? av - bv : bv - av;
			if (diff > maxDiff) maxDiff = diff;
		}
	}
	CLIP_CHECK(t, maxDiff <= 2);
	canvasFree(&ca);
	canvasFree(&cb);
	nsvgDeleteRasterizer(r);
	nsvgDelete(a);
	nsvgDelete(b);
}

static void caseReferenceSamples(ClipTest* t)
{
	/* Whole-row comparison against independently computed interval lengths.
	   Fill x [2.3,12.3], clip x [4.35,9.6]; row 8 is fully inside vertically. */
	const char* svg =
		"<svg width='16' height='16'><defs><clipPath id='c'>"
		"<rect x='4.35' y='0' width='5.25' height='16'/></clipPath></defs>"
		"<rect x='2.3' y='0' width='10' height='16' fill='red'"
		" clip-path='url(#c)'/></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	Canvas c;
	int x, checked = 0;

	CLIP_CHECK(t, image && r);
	if (!image || !r) { nsvgDelete(image); nsvgDeleteRasterizer(r); return; }
	if (!canvasInit(&c, 16, 16, 16 * 4)) { nsvgDelete(image); nsvgDeleteRasterizer(r); return; }
	render(r, image, &c, 0, 0, 1.0f);

	for (x = 0; x < 16; x++) {
		double expected = refPixelCoverage((double)x, 2.3, 12.3, 4.35, 9.6);
		int got = alphaAt(&c, x, 8);
		if (fabs(expected - got) > 5.0) {
			CLIP_CHECK(t, 0);
			fprintf(stderr, "  coverage x=%d expected=%.1f got=%d\n", x, expected, got);
		} else {
			checked++;
		}
	}
	CLIP_CHECK(t, checked == 16);
	CLIP_CHECK(t, alphaAt(&c, 6, 8) == 255);
	CLIP_CHECK(t, alphaAt(&c, 3, 8) == 0);

	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

/* ------------------------------------------------------------------ */
/* reuse: same definition across transforms, canvas sizes and strides */

static void caseReuseDifferentStrides(ClipTest* t)
{
	/* One image rendered into three different canvases/strides with
	   different tx/ty/scale, alternating with another image. */
	const char* clippedSvg =
		"<svg width='16' height='16'><defs><clipPath id='c'>"
		"<rect x='0' y='0' width='8' height='16'/></clipPath></defs>"
		"<rect x='0' y='0' width='16' height='16' fill='red' clip-path='url(#c)'/></svg>";
	const char* otherSvg =
		"<svg width='8' height='8'><rect x='0' y='0' width='8' height='8' fill='green'/></svg>";
	NSVGimage* image = parseStr(clippedSvg);
	NSVGimage* other = parseStr(otherSvg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	Canvas c1, c2;
	int i;

	CLIP_CHECK(t, image && other && r);
	if (!image || !other || !r) { nsvgDelete(image); nsvgDelete(other); nsvgDeleteRasterizer(r); return; }

	if (!canvasInit(&c1, 32, 32, 32 * 4 + 12) || !canvasInit(&c2, 48, 24, 48 * 4)) {
		nsvgDelete(image); nsvgDelete(other); nsvgDeleteRasterizer(r);
		return;
	}

	for (i = 0; i < 3; i++) {
		int padGood = 1, x, y;
		memset(c1.px, 0x5a, (size_t)c1.stride * c1.h);
		render(r, image, &c1, 2.0f, 3.0f, 1.5f);
		/* Left half painted, right half transparent. */
		CLIP_CHECK(t, alphaAt(&c1, 5, 12) == 255);
		CLIP_CHECK(t, alphaAt(&c1, 25, 12) == 0);
		for (y = 0; y < c1.h; y++)
			for (x = c1.w * 4; x < c1.stride; x++)
				if (c1.px[y * c1.stride + x] != 0x5a) padGood = 0;
		CLIP_CHECK(t, padGood);

		/* Alternate image must not inherit the previous clip. */
		render(r, other, &c1, 0, 0, 2.0f);
		CLIP_CHECK(t, alphaAt(&c1, 10, 5) == 255);
		CLIP_CHECK(t, alphaAt(&c1, 20, 5) == 0);	/* outside the 16x16 image */
	}

	/* Different size and scale: clip follows world coordinates. */
	render(r, image, &c2, 0, 0, 2.0f);
	CLIP_CHECK(t, alphaAt(&c2, 8, 10) == 255);
	CLIP_CHECK(t, alphaAt(&c2, 30, 10) == 0);

	canvasFree(&c1);
	canvasFree(&c2);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
	nsvgDelete(other);
}

static void caseReuseObjectBoundingBox(ClipTest* t)
{
	/* One objectBoundingBox definition used by two shapes of different size,
	   one of them mirrored.  Each element uses its own bounding box. */
	const char* svg =
		"<svg width='40' height='20'><defs>"
		"<clipPath id='c' clipPathUnits='objectBoundingBox'>"
		"<rect x='0' y='0' width='0.5' height='1'/></clipPath></defs>"
		"<rect x='2' y='2' width='10' height='10' fill='red' clip-path='url(#c)'/>"
		"<g transform='translate(40,0) scale(-1,1)'>"
		"<rect x='2' y='2' width='16' height='14' fill='red' clip-path='url(#c)'/>"
		"</g></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	Canvas c;

	CLIP_CHECK(t, image && r);
	if (!image || !r) { nsvgDelete(image); nsvgDeleteRasterizer(r); return; }
	if (!canvasInit(&c, 40, 20, 40 * 4)) {
		nsvgDelete(image); nsvgDeleteRasterizer(r); return;
	}
	render(r, image, &c, 0, 0, 1.0f);

	/* First shape: bbox x 2..12, clip x 2..7. */
	CLIP_CHECK(t, alphaAt(&c, 4, 8) == 255);
	CLIP_CHECK(t, alphaAt(&c, 9, 8) == 0);
	/* Mirrored second shape: bbox in world is x 24..40; clip covers the
	   half nearest its local x origin, i.e. world x 32..40. */
	CLIP_CHECK(t, alphaAt(&c, 36, 8) == 255);
	CLIP_CHECK(t, alphaAt(&c, 27, 8) == 0);

	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseReuseDefinitionNoCoordinateBleed(ClipTest* t)
{
	/* Two distant uses of the same userSpaceOnUse definition: the first
	   must not shift the second. */
	const char* svg =
		"<svg width='60' height='20'><defs><clipPath id='c'>"
		"<rect x='0' y='0' width='6' height='20'/></clipPath></defs>"
		"<rect x='0' y='0' width='20' height='10' fill='red' clip-path='url(#c)'/>"
		"<rect x='30' y='10' width='20' height='10' fill='red' clip-path='url(#c)'/>"
		"</svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	Canvas c;

	CLIP_CHECK(t, image && r);
	if (!image || !r) { nsvgDelete(image); nsvgDeleteRasterizer(r); return; }
	if (!canvasInit(&c, 60, 20, 60 * 4)) {
		nsvgDelete(image); nsvgDeleteRasterizer(r); return;
	}
	render(r, image, &c, 0, 0, 1.0f);
	CLIP_CHECK(t, alphaAt(&c, 3, 5) == 255);
	CLIP_CHECK(t, alphaAt(&c, 10, 5) == 0);
	/* The second use is far to the right: a user-space clip region must not
	   follow it there (no coordinate bleed from the first use). */
	CLIP_CHECK(t, alphaAt(&c, 33, 15) == 0);
	CLIP_CHECK(t, alphaAt(&c, 40, 15) == 0);
	CLIP_CHECK(t, alphaAt(&c, 3, 15) == 0);
	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseReuseFileEntry(ClipTest* t)
{
	/* Exercise nsvgParseFromFile on a local fixture with a viewBox scale. */
	NSVGimage* image = nsvgParseFromFile("tests/clip_material.svg", "px", 96.0f);
	NSVGrasterizer* r;
	Canvas c;

	CLIP_CHECK(t, image != NULL);
	if (image == NULL) return;
	CLIP_CHECK(t, image->width == 24.0f && image->height == 24.0f);
	r = nsvgCreateRasterizer();
	CLIP_CHECK(t, r != NULL);
	if (r == NULL) { nsvgDelete(image); return; }
	if (!canvasInit(&c, 24, 24, 24 * 4)) { nsvgDeleteRasterizer(r); nsvgDelete(image); return; }
	render(r, image, &c, 0, 0, 1.0f);

	/* viewBox 12x12 -> 24x24 means x2.  Shape x 1..11 -> 2..22; clip x 1..5
	   -> 2..10. */
	CLIP_CHECK(t, alphaAt(&c, 6, 12) == 255);
	CLIP_CHECK(t, alphaAt(&c, 14, 12) == 0);
	CLIP_CHECK(t, alphaAt(&c, 1, 12) == 0);
	CLIP_CHECK(t, redAt(&c, 6, 12) == 0x20);

	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseReuseStrokeClip(ClipTest* t)
{
	/* Clip limits stroke geometry as well as fill. */
	const char* svg =
		"<svg width='32' height='32'><defs><clipPath id='c'>"
		"<rect x='16' y='0' width='16' height='32'/></clipPath></defs>"
		"<rect x='4' y='4' width='20' height='20' fill='none' stroke='red'"
		" stroke-width='4' clip-path='url(#c)'/></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	Canvas c;

	CLIP_CHECK(t, image && r);
	if (!image || !r) { nsvgDelete(image); nsvgDeleteRasterizer(r); return; }
	if (!canvasInit(&c, 32, 32, 32 * 4)) { nsvgDelete(image); nsvgDeleteRasterizer(r); return; }
	render(r, image, &c, 0, 0, 1.0f);
	CLIP_CHECK(t, alphaAt(&c, 12, 3) == 0);		/* top stroke, outside clip */
	CLIP_CHECK(t, alphaAt(&c, 20, 3) == 255);	/* top stroke, inside clip */
	CLIP_CHECK(t, alphaAt(&c, 3, 14) == 0);		/* left stroke, outside clip */
	CLIP_CHECK(t, alphaAt(&c, 12, 6) == 0);		/* interior, fill none */
	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void caseRasterizerAlternation(ClipTest* t)
{
	/* Same rasterizer draws clipped, unclipped and clipped again with
	   differing tx/ty/scale and strides; no residue may leak. */
	const char* aSvg =
		"<svg width='16' height='16'><defs><clipPath id='c'>"
		"<rect x='8' y='0' width='8' height='16'/></clipPath></defs>"
		"<rect width='16' height='16' fill='red' clip-path='url(#c)'/></svg>";
	const char* bSvg =
		"<svg width='16' height='16'><rect width='16' height='16' fill='green'/></svg>";
	NSVGimage* a = parseStr(aSvg);
	NSVGimage* b = parseStr(bSvg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	Canvas cA, cB;

	CLIP_CHECK(t, a && b && r);
	if (!a || !b || !r) { nsvgDelete(a); nsvgDelete(b); nsvgDeleteRasterizer(r); return; }
	if (!canvasInit(&cA, 20, 20, 20 * 4 + 8) || !canvasInit(&cB, 40, 40, 40 * 4)) {
		nsvgDelete(a); nsvgDelete(b); nsvgDeleteRasterizer(r); return;
	}

	render(r, a, &cA, 2, 2, 1.0f);
	CLIP_CHECK(t, alphaAt(&cA, 4, 10) == 0);
	CLIP_CHECK(t, alphaAt(&cA, 15, 10) == 255);

	render(r, b, &cB, 0, 0, 2.5f);
	CLIP_CHECK(t, alphaAt(&cB, 5, 5) == 255);
	CLIP_CHECK(t, alphaAt(&cB, 35, 35) == 255);

	render(r, a, &cB, -4, -4, 1.0f);
	CLIP_CHECK(t, alphaAt(&cB, 2, 2) == 0);		/* left of clip after offset */
	CLIP_CHECK(t, alphaAt(&cB, 9, 2) == 255);	/* right half painted */

	canvasFree(&cA);
	canvasFree(&cB);
	nsvgDeleteRasterizer(r);
	nsvgDelete(a);
	nsvgDelete(b);
}

static void caseGradientWithClip(ClipTest* t)
{
	/* Paint rules stay unchanged: a gradient fill is clipped geometrically. */
	const char* svg =
		"<svg width='20' height='20'><defs>"
		"<linearGradient id='g'><stop offset='0' stop-color='red'/>"
		"<stop offset='1' stop-color='blue'/></linearGradient>"
		"<clipPath id='c'><rect x='0' y='0' width='10' height='20'/></clipPath>"
		"</defs>"
		"<rect width='20' height='20' fill='url(#g)' clip-path='url(#c)'/></svg>";
	NSVGimage* image = parseStr(svg);
	NSVGrasterizer* r = nsvgCreateRasterizer();
	Canvas c;

	CLIP_CHECK(t, image && r);
	if (!image || !r) { nsvgDelete(image); nsvgDeleteRasterizer(r); return; }
	if (!canvasInit(&c, 20, 20, 20 * 4)) { nsvgDelete(image); nsvgDeleteRasterizer(r); return; }
	render(r, image, &c, 0, 0, 1.0f);
	CLIP_CHECK(t, alphaAt(&c, 5, 10) == 255);
	CLIP_CHECK(t, alphaAt(&c, 15, 10) == 0);
	CLIP_CHECK(t, redAt(&c, 2, 10) > redAt(&c, 8, 10));
	canvasFree(&c);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

/* ------------------------------------------------------------------ */
/* registration                                                       */

const ClipCase clip_cases[] = {
	{"geometry", "rect-clip-baseline", caseRectClipBaseline},
	{"geometry", "evenodd-hole", caseEvenOddHole},
	{"geometry", "evenodd-style-and-attribute", caseEvenOddPropertyAndStyle},
	{"geometry", "child-union-opposite-winding", caseChildUnionOppositeWinding},
	{"geometry", "child-fill-opacity-display", caseChildFillAndOpacityIgnored},
	{"geometry", "definition-and-child-transforms", caseDefinitionAndChildTransforms},
	{"geometry", "curve-inside-outside-and-edge", caseCurveInsideOutsideAndEdge},
	{"scopes", "group-applies-and-restores", caseGroupClipAppliesAndRestores},
	{"scopes", "nested-group-intersection", caseNestedGroupIntersection},
	{"scopes", "clip-none-keeps-ancestor", caseClipNoneDoesNotOverrideAncestor},
	{"scopes", "group-bounding-box-descendants", caseGroupBoundingBoxUsesDescendants},
	{"scopes", "group-bounding-box-transform", caseGroupBoundingBoxTransform},
	{"scopes", "definition-after-reference", caseDefinitionAfterReference},
	{"scopes", "empty-missing-invalid", caseEmptyMissingAndInvalid},
	{"scopes", "group-bounds-visible-descendants", caseGroupBoundsIgnoreFillAndOpacityOnlyDisplay},
	{"coverage", "shared-fractional-edge", caseSharedFractionalEdge},
	{"coverage", "disjoint-halves", caseDisjointHalves},
	{"coverage", "repeated-clip-no-darkening", caseRepeatedClipNoDarkening},
	{"coverage", "reference-samples", caseReferenceSamples},
	{"reuse", "different-strides", caseReuseDifferentStrides},
	{"reuse", "object-bounding-box-reuse", caseReuseObjectBoundingBox},
	{"reuse", "definition-no-coordinate-bleed", caseReuseDefinitionNoCoordinateBleed},
	{"reuse", "file-entry", caseReuseFileEntry},
	{"reuse", "stroke-clipped", caseReuseStrokeClip},
	{"reuse", "rasterizer-alternation", caseRasterizerAlternation},
	{"reuse", "gradient-with-clip", caseGradientWithClip}
};

const size_t clip_case_count = sizeof(clip_cases) / sizeof(clip_cases[0]);
