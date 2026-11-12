// Pixel-level contract tests for radial gradients: eccentric focus, default
// focus, xlink:href inheritance, coordinate spaces, spread methods and
// rasterizer reuse. Expected values come from hand-computable on-axis points
// and an independent double precision reference model.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#include "../src/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../src/nanosvgrast.h"

static int failures = 0;

// Per channel tolerance: 2 bytes cover the 256 entry color table
// quantization (index truncation plus integer lerp rounding), the small
// extra covers float rounding of the transform chain against the double
// precision reference.
#define TOL 2.05

// ---------------------------------------------------------------------------
// Independent double precision reference model.
// Matrices use the same row vector layout as NanoSVG:
//   x' = x*m0 + y*m2 + m4
//   y' = x*m1 + y*m3 + m5
// ---------------------------------------------------------------------------

static void mat_mul(double* o, const double* a, const double* b)
{
	double t0 = a[0]*b[0] + a[1]*b[2];
	double t1 = a[0]*b[1] + a[1]*b[3];
	double t2 = a[2]*b[0] + a[3]*b[2];
	double t3 = a[2]*b[1] + a[3]*b[3];
	double t4 = a[4]*b[0] + a[5]*b[2] + b[4];
	double t5 = a[4]*b[1] + a[5]*b[3] + b[5];
	o[0] = t0; o[1] = t1; o[2] = t2; o[3] = t3; o[4] = t4; o[5] = t5;
}

static void mat_inv(double* o, const double* m)
{
	double det = m[0]*m[3] - m[2]*m[1];
	o[0] = m[3] / det;
	o[1] = -m[1] / det;
	o[2] = -m[2] / det;
	o[3] = m[0] / det;
	o[4] = (m[2]*m[5] - m[3]*m[4]) / det;
	o[5] = (m[1]*m[4] - m[0]*m[5]) / det;
}

// Color position for a sample at (gx,gy) in the unit circle space of a
// radial gradient whose focus is at (fx,fy) in the same space. Zero at the
// focus, one on the outer circle: the ray from the focus through the sample
// reaches the circle at Q and the position is FP/FQ.
static double ref_position(double gx, double gy, double fx, double fy)
{
	double fd = hypot(fx, fy);
	double dx, dy, a, b, c, disc;
	if (fd >= 1.0) {
		// Focus on or outside the circle is pulled back towards the center.
		double s = 0.99999 / fd;
		fx *= s;
		fy *= s;
	}
	dx = gx - fx;
	dy = gy - fy;
	a = dx*dx + dy*dy;
	if (a < 1e-18) return 0.0;
	b = fx*dx + fy*dy;
	c = fx*fx + fy*fy - 1.0;
	disc = b*b - a*c;
	if (disc < 0.0) disc = 0.0;
	return (-b - sqrt(disc)) / c;
}

static double ref_spread(double t, int spread)
{
	if (spread == NSVG_SPREAD_REPEAT) {
		t = t - floor(t);
	} else if (spread == NSVG_SPREAD_REFLECT) {
		t = t - 2.0*floor(t*0.5);
		if (t > 1.0) t = 2.0 - t;
	}
	if (t < 0.0) t = 0.0;
	if (t > 1.0) t = 1.0;
	return t;
}

// ---------------------------------------------------------------------------
// Rendering and checking helpers.
// ---------------------------------------------------------------------------

static void render_svg(const char* svg, float tx, float ty, float scale, int w, int h, unsigned char* out)
{
	char buf[4096];
	NSVGimage* img;
	NSVGrasterizer* r;
	snprintf(buf, sizeof(buf), "%s", svg);
	img = nsvgParse(buf, "px", 96);
	if (img == NULL) {
		printf("FAIL: could not parse svg\n");
		failures++;
		return;
	}
	r = nsvgCreateRasterizer();
	nsvgRasterize(r, img, tx, ty, scale, out, w, h, w*4);
	nsvgDeleteRasterizer(r);
	nsvgDelete(img);
}

static void check_px(const unsigned char* img, int stride, int x, int y,
					 double r, double g, double b, double a, const char* what)
{
	const unsigned char* p = img + (size_t)y*(size_t)stride + (size_t)x*4;
	double want[4];
	int k;
	want[0] = r; want[1] = g; want[2] = b; want[3] = a;
	for (k = 0; k < 4; k++) {
		if (fabs((double)p[k] - want[k]) > TOL) {
			printf("FAIL: %s px(%d,%d) ch%d got %d want %.2f\n", what, x, y, k, (int)p[k], want[k]);
			failures++;
			return;
		}
	}
}

// Checks a pixel against a hand-computed color position on a two stop ramp.
static void check_t(const unsigned char* img, int stride, int x, int y, double t,
					const double* c0, const double* c1, const char* what)
{
	double col[4];
	int k;
	for (k = 0; k < 4; k++)
		col[k] = c0[k] + (c1[k] - c0[k]) * t;
	check_px(img, stride, x, y, col[0], col[1], col[2], col[3], what);
}

// Sweeps a rectangle of pixels (kept away from shape edges) and compares
// every pixel against the double precision reference. The rasterizer samples
// gradients at integer pixel coordinates: user = (pixel - translate) / scale.
// minv maps user space to the unit circle space of the gradient, (fx,fy) is
// the focus there.
static void sweep(const unsigned char* img, int stride, int x0, int y0, int x1, int y1,
				  float tx, float ty, float scale,
				  const double* minv, double fx, double fy, int spread,
				  const double* c0, const double* c1, const char* what)
{
	int x, y, k;
	for (y = y0; y <= y1; y++) {
		for (x = x0; x <= x1; x++) {
			double ux = ((double)x - tx) / scale;
			double uy = ((double)y - ty) / scale;
			double gx = ux*minv[0] + uy*minv[2] + minv[4];
			double gy = ux*minv[1] + uy*minv[3] + minv[5];
			double t = ref_spread(ref_position(gx, gy, fx, fy), spread);
			const unsigned char* p = img + (size_t)y*(size_t)stride + (size_t)x*4;
			for (k = 0; k < 4; k++) {
				double want = c0[k] + (c1[k] - c0[k]) * t;
				if (fabs((double)p[k] - want) > TOL) {
					printf("FAIL: %s px(%d,%d) ch%d got %d want %.2f (t=%.6f)\n",
						   what, x, y, k, (int)p[k], want, t);
					failures++;
					break;
				}
			}
		}
	}
}

static const double RED[4] = {255, 0, 0, 255};
static const double BLUE[4] = {0, 0, 255, 255};
static const double GREEN[4] = {0, 128, 0, 255};
static const double YELLOW[4] = {255, 255, 0, 255};

// Builds a 64x40 image with a userSpaceOnUse radial gradient centered at
// (32,20), extra holds extra gradient attributes (focus, spread, ...).
static void focus_svg(char* out, size_t cap, const char* r, const char* extra)
{
	snprintf(out, cap,
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='g' gradientUnits='userSpaceOnUse' cx='32' cy='20' r='%s' %s>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/>"
		"</radialGradient></defs>"
		"<rect width='64' height='40' fill='url(#g)'/></svg>", r, extra);
}

// ---------------------------------------------------------------------------
// Focus behavior.
// ---------------------------------------------------------------------------

static void test_focus(void)
{
	unsigned char* img = (unsigned char*)malloc(64*40*4);
	unsigned char* img2 = (unsigned char*)malloc(64*40*4);
	unsigned char* img3 = (unsigned char*)malloc(64*40*4);
	char svg[1024];
	// unit circle space of cx=32 cy=20 r=16: gx=(ux-32)/16, gy=(uy-20)/16
	double minv[6] = {1.0/16, 0, 0, 1.0/16, -32.0/16, -20.0/16};
	// unit circle space of cx=32 cy=20 r=8
	double minv8[6] = {1.0/8, 0, 0, 1.0/8, -32.0/8, -20.0/8};

	// Concentric control: plain distance gradient, hand-computed axis points.
	focus_svg(svg, sizeof(svg), "16", "");
	render_svg(svg, 0.0f, 0.0f, 1.0f, 64, 40, img);
	check_t(img, 64*4, 32, 20, 0.0, RED, BLUE, "concentric center");
	check_t(img, 64*4, 40, 20, 0.5, RED, BLUE, "concentric +x half");
	check_t(img, 64*4, 24, 20, 0.5, RED, BLUE, "concentric -x half");
	check_t(img, 64*4, 32, 28, 0.5, RED, BLUE, "concentric +y half");
	check_t(img, 64*4, 48, 20, 1.0, RED, BLUE, "concentric +x rim");
	check_t(img, 64*4, 16, 20, 1.0, RED, BLUE, "concentric -x rim");
	sweep(img, 64*4, 2, 2, 61, 37, 0.0f, 0.0f, 1.0f, minv, 0, 0, NSVG_SPREAD_PAD, RED, BLUE, "concentric");

	// Eccentric focus at (24,20): focus unit space position (-0.5, 0).
	focus_svg(svg, sizeof(svg), "16", "fx='24' fy='20'");
	render_svg(svg, 0.0f, 0.0f, 1.0f, 64, 40, img2);
	check_t(img2, 64*4, 24, 20, 0.0, RED, BLUE, "focus at focus");
	check_t(img2, 64*4, 32, 20, 1.0/3.0, RED, BLUE, "focus +x third");
	check_t(img2, 64*4, 28, 20, 1.0/6.0, RED, BLUE, "focus +x sixth");
	check_t(img2, 64*4, 20, 20, 0.5, RED, BLUE, "focus -x half");
	check_t(img2, 64*4, 48, 20, 1.0, RED, BLUE, "focus +x rim");
	check_t(img2, 64*4, 16, 20, 1.0, RED, BLUE, "focus -x rim");
	sweep(img2, 64*4, 2, 2, 61, 37, 0.0f, 0.0f, 1.0f, minv, -0.5, 0, NSVG_SPREAD_PAD, RED, BLUE, "eccentric");

	// Undeclared focus equals an explicit centered focus, and differs from
	// an eccentric one.
	focus_svg(svg, sizeof(svg), "16", "fx='32' fy='20'");
	render_svg(svg, 0.0f, 0.0f, 1.0f, 64, 40, img3);
	if (memcmp(img, img3, 64*40*4) != 0) {
		printf("FAIL: default focus != explicit centered focus\n");
		failures++;
	}
	if (memcmp(img, img2, 64*40*4) == 0) {
		printf("FAIL: eccentric focus did not change the image\n");
		failures++;
	}

	// Focus outside the circle is pulled back to 0.99999 of the radius:
	// focus (-8,20) ends up at (32,20) + 0.99999*(-16,0).
	focus_svg(svg, sizeof(svg), "16", "fx='-8' fy='20'");
	render_svg(svg, 0.0f, 0.0f, 1.0f, 64, 40, img3);
	check_t(img3, 64*4, 32, 20, 0.5, RED, BLUE, "outside focus center");
	sweep(img3, 64*4, 2, 2, 61, 37, 0.0f, 0.0f, 1.0f, minv, -2.5, 0, NSVG_SPREAD_PAD, RED, BLUE, "outside focus");

	// Focus exactly on the circle is pulled back the same way, no NaN rows.
	focus_svg(svg, sizeof(svg), "16", "fx='48' fy='20'");
	render_svg(svg, 0.0f, 0.0f, 1.0f, 64, 40, img3);
	check_t(img3, 64*4, 32, 20, 0.5, RED, BLUE, "rim focus center");
	sweep(img3, 64*4, 2, 2, 61, 37, 0.0f, 0.0f, 1.0f, minv, 1.0, 0, NSVG_SPREAD_PAD, RED, BLUE, "rim focus");

	// Zero radius paints the last stop everywhere.
	focus_svg(svg, sizeof(svg), "0", "");
	render_svg(svg, 0.0f, 0.0f, 1.0f, 64, 40, img3);
	check_px(img3, 64*4, 10, 10, 0, 0, 255, 255, "zero radius a");
	check_px(img3, 64*4, 32, 20, 0, 0, 255, 255, "zero radius b");
	check_px(img3, 64*4, 60, 35, 0, 0, 255, 255, "zero radius c");

	// Negative radius draws nothing.
	focus_svg(svg, sizeof(svg), "-4", "");
	render_svg(svg, 0.0f, 0.0f, 1.0f, 64, 40, img3);
	check_px(img3, 64*4, 10, 10, 0, 0, 0, 0, "negative radius a");
	check_px(img3, 64*4, 32, 20, 0, 0, 0, 0, "negative radius b");

	// Spread methods on a small concentric gradient (r=8): the color
	// position crosses period boundaries along the axis.
	focus_svg(svg, sizeof(svg), "8", "spreadMethod='repeat'");
	render_svg(svg, 0.0f, 0.0f, 1.0f, 64, 40, img3);
	check_t(img3, 64*4, 40, 20, 0.0, RED, BLUE, "repeat t=1 wraps");
	check_t(img3, 64*4, 44, 20, 0.5, RED, BLUE, "repeat t=1.5");
	check_t(img3, 64*4, 48, 20, 0.0, RED, BLUE, "repeat t=2 wraps");
	check_t(img3, 64*4, 52, 20, 0.5, RED, BLUE, "repeat t=2.5");
	check_t(img3, 64*4, 36, 20, 0.5, RED, BLUE, "repeat t=0.5");
	sweep(img3, 64*4, 2, 20, 61, 20, 0.0f, 0.0f, 1.0f, minv8, 0, 0, NSVG_SPREAD_REPEAT, RED, BLUE, "repeat row");

	focus_svg(svg, sizeof(svg), "8", "spreadMethod='reflect'");
	render_svg(svg, 0.0f, 0.0f, 1.0f, 64, 40, img3);
	check_t(img3, 64*4, 40, 20, 1.0, RED, BLUE, "reflect t=1");
	check_t(img3, 64*4, 44, 20, 0.5, RED, BLUE, "reflect t=1.5");
	check_t(img3, 64*4, 48, 20, 0.0, RED, BLUE, "reflect t=2");
	check_t(img3, 64*4, 52, 20, 0.5, RED, BLUE, "reflect t=2.5");
	check_t(img3, 64*4, 56, 20, 1.0, RED, BLUE, "reflect t=3");
	sweep(img3, 64*4, 2, 20, 61, 20, 0.0f, 0.0f, 1.0f, minv8, 0, 0, NSVG_SPREAD_REFLECT, RED, BLUE, "reflect row");

	focus_svg(svg, sizeof(svg), "8", "");
	render_svg(svg, 0.0f, 0.0f, 1.0f, 64, 40, img3);
	check_t(img3, 64*4, 40, 20, 1.0, RED, BLUE, "pad t=1");
	check_t(img3, 64*4, 48, 20, 1.0, RED, BLUE, "pad t=2 clamps");
	check_t(img3, 64*4, 36, 20, 0.5, RED, BLUE, "pad t=0.5");
	sweep(img3, 64*4, 2, 2, 61, 37, 0.0f, 0.0f, 1.0f, minv8, 0, 0, NSVG_SPREAD_PAD, RED, BLUE, "pad");

	// Solid color control.
	render_svg("<svg width='64' height='40'><rect width='64' height='40' fill='#00ff00'/></svg>",
			   0.0f, 0.0f, 1.0f, 64, 40, img3);
	check_px(img3, 64*4, 10, 10, 0, 255, 0, 255, "solid a");
	check_px(img3, 64*4, 32, 20, 0, 255, 0, 255, "solid b");

	free(img);
	free(img2);
	free(img3);
}

// ---------------------------------------------------------------------------
// xlink:href inheritance between radial gradients.
// ---------------------------------------------------------------------------

static void test_inherit(void)
{
	unsigned char* img = (unsigned char*)malloc(64*40*4);
	double minv[6];
	double minv2[6];

	// Inherited cx/r/units/stops, local cy, undeclared focus follows the
	// resolved center (40,24): the result is concentric there.
	render_svg(
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='base' gradientUnits='userSpaceOnUse' cx='40' cy='20' r='16'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"<radialGradient id='child' xlink:href='#base' cy='24'/>"
		"</defs><rect width='64' height='40' fill='url(#child)'/></svg>",
		0.0f, 0.0f, 1.0f, 64, 40, img);
	{
		double m[6] = {1.0/16, 0, 0, 1.0/16, -40.0/16, -24.0/16};
		memcpy(minv, m, sizeof(minv));
	}
	check_t(img, 64*4, 40, 24, 0.0, RED, BLUE, "inherit center");
	check_t(img, 64*4, 48, 24, 0.5, RED, BLUE, "inherit +x half");
	check_t(img, 64*4, 56, 24, 1.0, RED, BLUE, "inherit +x rim");
	check_t(img, 64*4, 40, 16, 0.5, RED, BLUE, "inherit -y half");
	sweep(img, 64*4, 2, 2, 61, 37, 0.0f, 0.0f, 1.0f, minv, 0, 0, NSVG_SPREAD_PAD, RED, BLUE, "inherit default focus");

	// An explicitly declared zero focus must not be treated as undeclared:
	// focus (0,0) is far outside the circle at (40,20) r=16 and gets
	// pulled back towards the center.
	render_svg(
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='base' gradientUnits='userSpaceOnUse' cx='40' cy='20' r='16'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"<radialGradient id='child' xlink:href='#base' fx='0' fy='0'/>"
		"</defs><rect width='64' height='40' fill='url(#child)'/></svg>",
		0.0f, 0.0f, 1.0f, 64, 40, img);
	{
		double m[6] = {1.0/16, 0, 0, 1.0/16, -40.0/16, -20.0/16};
		memcpy(minv, m, sizeof(minv));
	}
	// focus (0,0) pulled back to (40,20)+0.99999*16*(-40,-20)/|(-40,-20)|:
	// the center (40,20) sits exactly halfway between focus and rim.
	check_t(img, 64*4, 40, 20, 0.5, RED, BLUE, "explicit zero focus center");
	sweep(img, 64*4, 2, 2, 61, 37, 0.0f, 0.0f, 1.0f, minv, -2.5, -1.25, NSVG_SPREAD_PAD, RED, BLUE, "explicit zero focus");

	// Three link chain: gA overrides r, gB overrides cx, gC has the rest.
	// Resolved: cx=20 cy=20 r=8 fx=12 fy=20 userSpaceOnUse, stops of gC.
	render_svg(
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='gC' gradientUnits='userSpaceOnUse' cx='44' cy='20' r='16' fx='12'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"<radialGradient id='gB' xlink:href='#gC' cx='20'/>"
		"<radialGradient id='gA' xlink:href='#gB' r='8'/>"
		"</defs><rect width='64' height='40' fill='url(#gA)'/></svg>",
		0.0f, 0.0f, 1.0f, 64, 40, img);
	{
		double m[6] = {1.0/8, 0, 0, 1.0/8, -20.0/8, -20.0/8};
		memcpy(minv, m, sizeof(minv));
	}
	// focus (12,20) pulled back to (20,20)+0.99999*(-8,0).
	check_t(img, 64*4, 20, 20, 0.5, RED, BLUE, "chain center");
	check_t(img, 64*4, 24, 20, 0.75, RED, BLUE, "chain +x");
	check_t(img, 64*4, 28, 20, 1.0, RED, BLUE, "chain +x rim");
	check_t(img, 64*4, 12, 20, 1.0, RED, BLUE, "chain -x rim");
	sweep(img, 64*4, 13, 20, 27, 20, 0.0f, 0.0f, 1.0f, minv, -1.0, 0, NSVG_SPREAD_PAD, RED, BLUE, "chain row");

	// Local stops win over referenced stops, and the referenced definition
	// stays intact for its own users.
	render_svg(
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='gC' gradientUnits='userSpaceOnUse' cx='44' cy='20' r='16' fx='12'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"<radialGradient id='gStops' xlink:href='#gC'>"
		"<stop offset='0' stop-color='green'/><stop offset='1' stop-color='yellow'/></radialGradient>"
		"</defs>"
		"<rect x='0' y='0' width='32' height='40' fill='url(#gStops)'/>"
		"<rect x='32' y='0' width='32' height='40' fill='url(#gC)'/></svg>",
		0.0f, 0.0f, 1.0f, 64, 40, img);
	// focus (12,20) pulled back to (44,20)+0.99999*(-16,0) = (28.00016,20).
	check_t(img, 64*4, 30, 20, 0.0625, GREEN, YELLOW, "local stops inner");
	check_t(img, 64*4, 28, 20, 1.0, GREEN, YELLOW, "local stops rim");
	check_t(img, 64*4, 44, 20, 0.5, RED, BLUE, "referenced unmodified");
	{
		double m[6] = {1.0/16, 0, 0, 1.0/16, -44.0/16, -20.0/16};
		memcpy(minv, m, sizeof(minv));
	}
	sweep(img, 64*4, 34, 2, 61, 37, 0.0f, 0.0f, 1.0f, minv, -2.0, 0, NSVG_SPREAD_PAD, RED, BLUE, "referenced def");

	// Missing reference and reference cycles make the paint none, later
	// shapes still draw.
	render_svg(
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='cycA' xlink:href='#cycB'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"<radialGradient id='cycB' xlink:href='#cycA'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"<radialGradient id='cycSelf' xlink:href='#cycSelf'>"
		"<stop offset='0' stop-color='red'/></radialGradient>"
		"<radialGradient id='okG' gradientUnits='userSpaceOnUse' cx='52' cy='20' r='8'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"</defs>"
		"<rect x='0' y='0' width='16' height='40' fill='url(#nope)'/>"
		"<rect x='16' y='0' width='16' height='40' fill='url(#cycA)'/>"
		"<rect x='32' y='0' width='16' height='40' fill='url(#cycSelf)'/>"
		"<rect x='48' y='0' width='16' height='40' fill='url(#okG)'/></svg>",
		0.0f, 0.0f, 1.0f, 64, 40, img);
	check_px(img, 64*4, 8, 20, 0, 0, 0, 0, "missing ref is none");
	check_px(img, 64*4, 24, 20, 0, 0, 0, 0, "cycle is none");
	check_px(img, 64*4, 40, 20, 0, 0, 0, 0, "self cycle is none");
	check_t(img, 64*4, 52, 20, 0.0, RED, BLUE, "later shape draws");
	check_t(img, 64*4, 56, 20, 0.5, RED, BLUE, "later shape half");

	// One objectBoundingBox definition shared by two shapes of different
	// sizes: each evaluates it against its own bounds.
	render_svg(
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='bboxG'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"</defs>"
		"<rect x='0' y='0' width='40' height='40' fill='url(#bboxG)'/>"
		"<rect x='44' y='0' width='20' height='40' fill='url(#bboxG)'/></svg>",
		0.0f, 0.0f, 1.0f, 64, 40, img);
	// shape 1: normalized circle -> unit space xform [20,0,0,20,20,20]
	{
		double f1[6] = {20, 0, 0, 20, 20, 20};
		mat_inv(minv, f1);
	}
	// shape 2: normalized circle -> unit space xform [10,0,0,20,54,20]
	{
		double f2[6] = {10, 0, 0, 20, 54, 20};
		mat_inv(minv2, f2);
	}
	check_t(img, 64*4, 20, 20, 0.0, RED, BLUE, "bbox 1 center");
	check_t(img, 64*4, 30, 20, 0.5, RED, BLUE, "bbox 1 half");
	check_t(img, 64*4, 54, 20, 0.0, RED, BLUE, "bbox 2 center");
	check_t(img, 64*4, 59, 20, 0.5, RED, BLUE, "bbox 2 half");
	sweep(img, 64*4, 2, 2, 37, 37, 0.0f, 0.0f, 1.0f, minv, 0, 0, NSVG_SPREAD_PAD, RED, BLUE, "bbox shape 1");
	sweep(img, 64*4, 46, 2, 61, 37, 0.0f, 0.0f, 1.0f, minv2, 0, 0, NSVG_SPREAD_PAD, RED, BLUE, "bbox shape 2");

	free(img);
}

// ---------------------------------------------------------------------------
// Coordinate spaces and transforms.
// ---------------------------------------------------------------------------

static void test_coordinates(void)
{
	unsigned char* img = (unsigned char*)malloc(128*80*4);
	double minv[6];
	double fwd[6];

	// objectBoundingBox on a non-square shape: the circle is established in
	// the normalized box and becomes an ellipse through the box transform.
	render_svg(
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='g'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"</defs><rect x='8' y='16' width='48' height='16' fill='url(#g)'/></svg>",
		0.0f, 0.0f, 1.0f, 64, 40, img);
	// normalized circle (0.5,0.5) r=0.5 in box (8,16,48,16):
	// unit space -> user space is [24,0,0,8,32,24]
	{
		double f[6] = {24, 0, 0, 8, 32, 24};
		mat_inv(minv, f);
	}
	check_t(img, 64*4, 32, 24, 0.0, RED, BLUE, "ellipse center");
	check_t(img, 64*4, 44, 24, 0.5, RED, BLUE, "ellipse +x half");
	check_t(img, 64*4, 32, 20, 0.5, RED, BLUE, "ellipse -y half");
	check_t(img, 64*4, 32, 28, 0.5, RED, BLUE, "ellipse +y half");
	check_t(img, 64*4, 20, 24, 0.5, RED, BLUE, "ellipse -x half");
	check_t(img, 64*4, 55, 24, 23.0/24.0, RED, BLUE, "ellipse +x rim");
	sweep(img, 64*4, 10, 18, 53, 30, 0.0f, 0.0f, 1.0f, minv, 0, 0, NSVG_SPREAD_PAD, RED, BLUE, "bbox ellipse");

	// gradientTransform with non-commuting parts: translate(8 4) scale(2 0.5)
	// scales the circle first, then translates: center (40,14), rx=16, ry=4.
	render_svg(
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='g' gradientUnits='userSpaceOnUse' cx='16' cy='20' r='8'"
		" gradientTransform='translate(8 4) scale(2 0.5)'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"</defs><rect width='64' height='40' fill='url(#g)'/></svg>",
		0.0f, 0.0f, 1.0f, 64, 40, img);
	{
		double circ[6] = {8, 0, 0, 8, 16, 20};
		double sc[6] = {2, 0, 0, 0.5, 0, 0};
		double tr[6] = {1, 0, 0, 1, 8, 4};
		double tmp[6];
		mat_mul(tmp, circ, sc);
		mat_mul(fwd, tmp, tr);
		mat_inv(minv, fwd);
	}
	check_t(img, 64*4, 40, 14, 0.0, RED, BLUE, "gradTransform center");
	check_t(img, 64*4, 56, 14, 1.0, RED, BLUE, "gradTransform +x rim");
	check_t(img, 64*4, 40, 10, 1.0, RED, BLUE, "gradTransform -y rim");
	check_t(img, 64*4, 48, 14, 0.5, RED, BLUE, "gradTransform +x half");
	sweep(img, 64*4, 2, 2, 61, 37, 0.0f, 0.0f, 1.0f, minv, 0, 0, NSVG_SPREAD_PAD, RED, BLUE, "gradTransform");

	// The swapped list means something else: scale(2 0.5) translate(8 4)
	// moves the center to (48,12).
	render_svg(
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='g' gradientUnits='userSpaceOnUse' cx='16' cy='20' r='8'"
		" gradientTransform='scale(2 0.5) translate(8 4)'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"</defs><rect width='64' height='40' fill='url(#g)'/></svg>",
		0.0f, 0.0f, 1.0f, 64, 40, img);
	{
		double circ[6] = {8, 0, 0, 8, 16, 20};
		double tr[6] = {1, 0, 0, 1, 8, 4};
		double sc[6] = {2, 0, 0, 0.5, 0, 0};
		double tmp[6];
		mat_mul(tmp, circ, tr);
		mat_mul(fwd, tmp, sc);
		mat_inv(minv, fwd);
	}
	check_t(img, 64*4, 48, 12, 0.0, RED, BLUE, "swapped center");
	check_t(img, 64*4, 40, 14, sqrt(0.5), RED, BLUE, "swapped off center");
	sweep(img, 64*4, 2, 2, 61, 37, 0.0f, 0.0f, 1.0f, minv, 0, 0, NSVG_SPREAD_PAD, RED, BLUE, "swapped gradTransform");

	// Shape and group transforms compose with the gradient: circle (8,4) r=4
	// in the rect's local space, rect translated by (1,2), group scales by 2.
	render_svg(
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='g' gradientUnits='userSpaceOnUse' cx='8' cy='4' r='4'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"</defs>"
		"<g transform='scale(2)'>"
		"<rect x='4' y='2' width='24' height='12' transform='translate(1 2)' fill='url(#g)'/>"
		"</g></svg>",
		0.0f, 0.0f, 1.0f, 64, 40, img);
	// local -> user: translate(1,2) then scale(2); circle -> [8,0,0,8,18,12]
	{
		double circ[6] = {4, 0, 0, 4, 8, 4};
		double tr[6] = {1, 0, 0, 1, 1, 2};
		double sc[6] = {2, 0, 0, 2, 0, 0};
		double tmp[6];
		mat_mul(tmp, circ, tr);
		mat_mul(fwd, tmp, sc);
		mat_inv(minv, fwd);
	}
	check_t(img, 64*4, 18, 12, 0.0, RED, BLUE, "shape xform center");
	check_t(img, 64*4, 26, 12, 1.0, RED, BLUE, "shape xform +x rim");
	check_t(img, 64*4, 18, 20, 1.0, RED, BLUE, "shape xform +y rim");
	check_t(img, 64*4, 22, 12, 0.5, RED, BLUE, "shape xform +x half");
	sweep(img, 64*4, 12, 10, 55, 29, 0.0f, 0.0f, 1.0f, minv, 0, 0, NSVG_SPREAD_PAD, RED, BLUE, "shape xform");

	// viewBox scales the whole scene, focus included.
	render_svg(
		"<svg width='128' height='80' viewBox='0 0 64 40'><defs>"
		"<radialGradient id='g' gradientUnits='userSpaceOnUse' cx='32' cy='20' r='16' fx='24' fy='20'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"</defs><rect width='64' height='40' fill='url(#g)'/></svg>",
		0.0f, 0.0f, 1.0f, 128, 80, img);
	// circle -> image space: [32,0,0,32,64,40], focus stays (-0.5,0) in
	// unit circle space.
	{
		double circ[6] = {16, 0, 0, 16, 32, 20};
		double vb[6] = {2, 0, 0, 2, 0, 0};
		mat_mul(fwd, circ, vb);
		mat_inv(minv, fwd);
	}
	check_t(img, 128*4, 48, 40, 0.0, RED, BLUE, "viewBox focus");
	check_t(img, 128*4, 64, 40, 1.0/3.0, RED, BLUE, "viewBox third");
	check_t(img, 128*4, 80, 40, 2.0/3.0, RED, BLUE, "viewBox two thirds");
	check_t(img, 128*4, 96, 40, 1.0, RED, BLUE, "viewBox rim");
	sweep(img, 128*4, 4, 4, 121, 74, 0.0f, 0.0f, 1.0f, minv, -0.5, 0, NSVG_SPREAD_PAD, RED, BLUE, "viewBox");

	// The rasterizer tx/ty/scale map device pixels back to user space, the
	// focus is not translated separately in device space.
	render_svg(
		"<svg width='64' height='40'><defs>"
		"<radialGradient id='g' gradientUnits='userSpaceOnUse' cx='32' cy='20' r='16' fx='24' fy='20'>"
		"<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient>"
		"</defs><rect width='64' height='40' fill='url(#g)'/></svg>",
		10.0f, 6.0f, 2.0f, 128, 80, img);
	{
		double m[6] = {1.0/16, 0, 0, 1.0/16, -32.0/16, -20.0/16};
		memcpy(minv, m, sizeof(minv));
	}
	// device (74,46) <-> user (32,20); (58,46) <-> (24,20); (106,46) <-> (48,20)
	check_t(img, 128*4, 74, 46, 1.0/3.0, RED, BLUE, "raster xform third");
	check_t(img, 128*4, 58, 46, 0.0, RED, BLUE, "raster xform focus");
	check_t(img, 128*4, 106, 46, 1.0, RED, BLUE, "raster xform rim");
	sweep(img, 128*4, 24, 10, 103, 69, 10.0f, 6.0f, 2.0f, minv, -0.5, 0, NSVG_SPREAD_PAD, RED, BLUE, "raster xform");

	free(img);
}

// ---------------------------------------------------------------------------
// Rasterizer and image reuse.
// ---------------------------------------------------------------------------

static void test_reuse(void)
{
	char svg[1024];
	NSVGimage* img;
	NSVGrasterizer* rast;
	unsigned char* bufA = (unsigned char*)malloc(64*40*4);
	unsigned char* bufB = (unsigned char*)malloc(140*20);
	unsigned char* bufC = (unsigned char*)malloc(64*40*4);
	unsigned char* bufD = (unsigned char*)malloc(140*20);
	unsigned char* bufE = (unsigned char*)malloc(272*40);
	int i, ok;

	focus_svg(svg, sizeof(svg), "16", "fx='24' fy='20'");
	img = nsvgParse(svg, "px", 96);
	rast = nsvgCreateRasterizer();
	if (img == NULL || rast == NULL) {
		printf("FAIL: reuse setup\n");
		failures++;
		return;
	}

	// Alternate output translation, scale and stride on one image and one
	// rasterizer, then check the runs are repeatable.
	nsvgRasterize(rast, img, 0.0f, 0.0f, 1.0f, bufA, 64, 40, 64*4);

	memset(bufB, 0xA5, 140*20);
	nsvgRasterize(rast, img, 3.5f, 1.5f, 0.5f, bufB, 32, 20, 140);

	nsvgRasterize(rast, img, 0.0f, 0.0f, 1.0f, bufC, 64, 40, 64*4);
	if (memcmp(bufA, bufC, 64*40*4) != 0) {
		printf("FAIL: rerun with same params differs\n");
		failures++;
	}

	memset(bufD, 0xA5, 140*20);
	nsvgRasterize(rast, img, 3.5f, 1.5f, 0.5f, bufD, 32, 20, 140);
	if (memcmp(bufB, bufD, 140*20) != 0) {
		printf("FAIL: rerun with padding differs\n");
		failures++;
	}

	// Padding bytes past w*4 are never written.
	ok = 1;
	for (i = 0; i < 20; i++) {
		int k;
		for (k = 32*4; k < 140; k++) {
			if (bufD[i*140 + k] != 0xA5) { ok = 0; break; }
		}
	}
	if (!ok) {
		printf("FAIL: padding bytes were written\n");
		failures++;
	}

	// Content of the translated/scaled run matches the reference.
	{
		double minv[6] = {1.0/16, 0, 0, 1.0/16, -32.0/16, -20.0/16};
		sweep(bufD, 140, 5, 3, 31, 19, 3.5f, 1.5f, 0.5f, minv, -0.5, 0, NSVG_SPREAD_PAD, RED, BLUE, "reuse scaled");
	}

	// Same logical output with a wider stride matches the tight buffer.
	nsvgRasterize(rast, img, 0.0f, 0.0f, 1.0f, bufE, 64, 40, 272);
	ok = 1;
	for (i = 0; i < 40; i++) {
		if (memcmp(bufE + i*272, bufA + i*64*4, 64*4) != 0) { ok = 0; break; }
	}
	if (!ok) {
		printf("FAIL: stride changed the pixels\n");
		failures++;
	}

	// Once more with the first parameter set after all of the above.
	nsvgRasterize(rast, img, 0.0f, 0.0f, 1.0f, bufC, 64, 40, 64*4);
	if (memcmp(bufA, bufC, 64*40*4) != 0) {
		printf("FAIL: final rerun differs\n");
		failures++;
	}

	nsvgDelete(img);
	nsvgDeleteRasterizer(rast);
	free(bufA);
	free(bufB);
	free(bufC);
	free(bufD);
	free(bufE);
}

int main(void)
{
	int f0;

	f0 = failures;
	test_focus();
	if (failures == f0) printf("RADIAL_FOCUS_OK\n");

	f0 = failures;
	test_inherit();
	if (failures == f0) printf("RADIAL_INHERIT_OK\n");

	f0 = failures;
	test_coordinates();
	if (failures == f0) printf("RADIAL_COORDINATES_OK\n");

	f0 = failures;
	test_reuse();
	if (failures == f0) printf("RADIAL_REUSE_OK\n");

	if (failures != 0) {
		printf("FAILURES: %d\n", failures);
		return 1;
	}
	return 0;
}
