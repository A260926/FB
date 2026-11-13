// Task tests for the NanoVG CPU back-end.
//
// Registered through register_task_tests() in four groups: fills, strokes,
// textures, frames. All assertions go through Case::check; there is no main()
// here (task_support/main.cpp drives the registry). Geometry expectations for
// holes and self-intersection come from a small independent winding
// reference implemented in this file, not from the back-end.

#include "registry.hpp"
#include "nanovg_cpu.h"
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct Surface {
	int w, h, stride;
	std::vector<unsigned char> px;
	explicit Surface(int w_, int h_, int pad = 0)
		: w(w_), h(h_), stride(4 * w_ + pad), px((size_t)stride * h_, 0) {}
	void fill(unsigned char v) { std::fill(px.begin(), px.end(), v); }
	void zero() { std::fill(px.begin(), px.end(), (unsigned char)0); }
	const unsigned char* at(int x, int y) const { return &px[(size_t)y * stride + (size_t)x * 4]; }
	unsigned char* at(int x, int y) { return &px[(size_t)y * stride + (size_t)x * 4]; }
};

static bool nearByte(int a, int b, int tol) { return std::abs(a - b) <= tol; }

static void expectPixel(Case& c, const Surface& s, int x, int y,
						int r, int g, int b, int a, int tol, const char* msg)
{
	const unsigned char* p = s.at(x, y);
	c.check(nearByte(p[0], r, tol) && nearByte(p[1], g, tol) &&
			nearByte(p[2], b, tol) && nearByte(p[3], a, tol), msg);
}

// Independent premultiplied source-over expectation (separate math).
static void premulOver(int sr, int sg, int sb, int sa,
					   int dr, int dg, int db, int da, int out[4])
{
	float s[4] = { sr / 255.0f, sg / 255.0f, sb / 255.0f, sa / 255.0f };
	float d[4] = { dr / 255.0f, dg / 255.0f, db / 255.0f, da / 255.0f };
	float sp[3] = { s[0] * s[3], s[1] * s[3], s[2] * s[3] };
	float oa = s[3] + d[3] * (1.0f - s[3]);
	float outf[4];
	outf[0] = sp[0] + d[0] * (1.0f - s[3]);
	outf[1] = sp[1] + d[1] * (1.0f - s[3]);
	outf[2] = sp[2] + d[2] * (1.0f - s[3]);
	outf[3] = oa;
	for (int i = 0; i < 4; ++i) {
		float v = outf[i] < 0 ? 0 : (outf[i] > 1 ? 1 : outf[i]);
		out[i] = (int)std::lround(v * 255.0f);
	}
}

static int premulByte(int comp, int a)
{
	return (int)std::lround((comp / 255.0f) * (a / 255.0f) * 255.0f);
}

// Independent non-zero winding reference on a small float grid. Contours are
// closed polygon loops; holes are simply oppositely wound loops.
struct RefPoint { float x, y; };
struct RefContour { std::vector<RefPoint> pt; };

static int refWinding(const std::vector<RefContour>& contours, float px, float py)
{
	int wind = 0;
	for (const auto& con : contours) {
		int n = (int)con.pt.size();
		for (int i = 0; i < n; ++i) {
			const RefPoint& a = con.pt[i];
			const RefPoint& b = con.pt[(i + 1) % n];
			if ((a.y <= py && b.y > py) || (b.y <= py && a.y > py)) {
				float xi = a.x + (py - a.y) * (b.x - a.x) / (b.y - a.y);
				if (xi > px) wind += (b.y > a.y) ? 1 : -1;
			}
		}
	}
	return wind;
}

// ---------------------------------------------------------------------------
// fills
// ---------------------------------------------------------------------------

static void testFillInvalid(Case& c)
{
	Surface s(8, 8);
	c.check(nvgCreateCPU(nullptr, 8, 8, s.stride) == nullptr, "null target rejected");
	c.check(nvgCreateCPU(s.px.data(), 0, 8, s.stride) == nullptr, "zero width rejected");
	c.check(nvgCreateCPU(s.px.data(), 8, -1, s.stride) == nullptr, "negative height rejected");
	c.check(nvgCreateCPU(s.px.data(), 8, 8, 4 * 8 - 1) == nullptr, "short stride rejected");
	NVGcontext* vg = nvgCreateCPU(s.px.data(), 8, 8, s.stride);
	c.check(vg != nullptr, "valid construction");
	nvgDeleteCPU(vg);
	nvgDeleteCPU(nullptr); // must be a safe no-op
}

static void testFillSolid(Case& c)
{
	Surface s(16, 16);
	s.fill(0x77);
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 16, 16, 1);
	nvgBeginPath(vg);
	// Half-pixel placement so sub-pixel centers straddle the edges.
	nvgRect(vg, 4.5f, 4.5f, 7, 7);
	nvgFillColor(vg, nvgRGBA(200, 100, 50, 255));
	nvgFill(vg);
	nvgEndFrame(vg);
	// Fully interior pixel: exact opaque color.
	expectPixel(c, s, 8, 8, 200, 100, 50, 255, 2, "opaque interior pixel");
	// Fully outside pixel: target untouched.
	expectPixel(c, s, 1, 1, 0x77, 0x77, 0x77, 0x77, 0, "outside pixel untouched");
	// Edge pixel is partially covered (blended, neither full color nor marker).
	const unsigned char* e = s.at(4, 8);
	c.check(e[3] > 0x77 && e[3] < 255, "edge pixel gets fractional coverage");
	nvgDeleteCPU(vg);
}

static void testFillPremul(Case& c)
{
	Surface s(12, 12);
	s.zero();
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 12, 12, 1);
	nvgBeginPath(vg);
	nvgRect(vg, 2, 2, 8, 8);
	nvgFillColor(vg, nvgRGBA(200, 100, 50, 128));
	nvgFill(vg);
	nvgEndFrame(vg);
	int er = premulByte(200, 128), eg = premulByte(100, 128), eb = premulByte(50, 128);
	expectPixel(c, s, 6, 6, er, eg, eb, 128, 2, "translucent fill is premultiplied");
	const unsigned char* p = s.at(6, 6);
	c.check(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3], "channels never exceed alpha");
	nvgDeleteCPU(vg);
}

static void testFillHoleAndContours(Case& c)
{
	Surface s(24, 24);
	s.fill(0x77);
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 24, 24, 1);
	nvgBeginPath(vg);
	nvgRect(vg, 2, 2, 20, 20);
	nvgRect(vg, 8, 8, 8, 8);
	nvgPathWinding(vg, NVG_HOLE);
	nvgFillColor(vg, nvgRGBA(0, 0, 255, 255));
	nvgFill(vg);
	nvgEndFrame(vg);
	// Hole interior is empty even though both fans cover it.
	expectPixel(c, s, 12, 12, 0x77, 0x77, 0x77, 0x77, 0, "hole stays transparent");
	// Ring interior is filled.
	expectPixel(c, s, 4, 12, 0, 0, 255, 255, 2, "ring body filled");
	nvgDeleteCPU(vg);

	// Two overlapping, equally wound contours: overlap has winding 2, which
	// the non-zero rule still fills (an even-odd renderer would leave it empty).
	Surface s2(24, 24);
	s2.zero();
	vg = nvgCreateCPU(s2.px.data(), s2.w, s2.h, s2.stride);
	nvgBeginFrame(vg, 24, 24, 1);
	nvgBeginPath(vg);
	nvgRect(vg, 2, 2, 12, 12);
	nvgRect(vg, 8, 8, 12, 12);
	nvgFillColor(vg, nvgRGBA(0, 255, 0, 255));
	nvgFill(vg);
	nvgEndFrame(vg);
	expectPixel(c, s2, 12, 12, 0, 255, 0, 255, 2, "winding-2 overlap still filled");
	expectPixel(c, s2, 4, 4, 0, 255, 0, 255, 2, "single contour region filled");
	expectPixel(c, s2, 22, 22, 0, 0, 0, 0, 0, "union exterior empty");
	nvgDeleteCPU(vg);
}

static void testFillSelfIntersect(Case& c)
{
	// Bow-tie drawn as one crossing sub-path.
	const std::vector<RefPoint> loop = {
		{2, 2}, {10, 2}, {2, 10}, {10, 10}
	};
	std::vector<RefContour> ref = { RefContour{ loop } };

	Surface s(14, 14);
	s.zero();
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 14, 14, 1);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 2, 2);
	nvgLineTo(vg, 10, 2);
	nvgLineTo(vg, 2, 10);
	nvgLineTo(vg, 10, 10);
	nvgClosePath(vg);
	nvgFillColor(vg, nvgRGBA(255, 0, 0, 255));
	nvgFill(vg);
	nvgEndFrame(vg);

	// Evaluate the independent reference on the same 4x4 sub-pixel grid the
	// back-end uses, so an opaque fill's alpha must equal its coverage.
	const int SUB = 4;
	int compared = 0, matched = 0;
	for (int y = 1; y < 12; ++y) {
		for (int x = 1; x < 12; ++x) {
			int cover = 0;
			for (int sy = 0; sy < SUB; ++sy)
				for (int sx = 0; sx < SUB; ++sx) {
					float spx = x + (2.0f * sx + 1.0f) / (2.0f * SUB);
					float spy = y + (2.0f * sy + 1.0f) / (2.0f * SUB);
					if (refWinding(ref, spx, spy) != 0) ++cover;
				}
			int expectedA = (int)std::lround(cover * 255.0f / (SUB * SUB));
			int gotA = s.at(x, y)[3];
			// Tolerate one sub-pixel step of difference at float boundaries.
			if (std::abs(gotA - expectedA) <= 17) ++matched;
			++compared;
		}
	}
	c.check(compared >= 80, "reference grid actually exercised");
	c.check(matched == compared, "self-intersection coverage matches winding reference");
	// Sanity: the bow-tie has both empty and filled interior pixels.
	bool hasEmpty = false, hasFull = false;
	for (int y = 2; y < 10; ++y)
		for (int x = 2; x < 10; ++x) {
			int a = s.at(x, y)[3];
			if (a == 0) hasEmpty = true;
			if (a == 255) hasFull = true;
		}
	c.check(hasEmpty && hasFull, "bow-tie has both wound and unwound interior regions");
	nvgDeleteCPU(vg);
}

// ---------------------------------------------------------------------------
// strokes
// ---------------------------------------------------------------------------

static void testStrokeUnion(Case& c)
{
	// A polyline whose segments cross. One renderStroke must merge overlaps
	// into a coverage union: no sub-pixel is painted twice, so alpha never
	// exceeds a single full-coverage stroke.
	Surface s(32, 24);
	s.zero();
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 32, 24, 1);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 6, 6);
	nvgLineTo(vg, 26, 18);
	nvgLineTo(vg, 26, 6);
	nvgLineTo(vg, 6, 18);
	nvgStrokeWidth(vg, 4);
	nvgStrokeColor(vg, nvgRGBA(255, 0, 0, 128));
	nvgStroke(vg);
	nvgEndFrame(vg);

	int maxA = 0, fullCount = 0;
	for (int y = 0; y < s.h; ++y)
		for (int x = 0; x < s.w; ++x) {
			int a = s.at(x, y)[3];
			if (a > maxA) maxA = a;
			if (a >= 126) ++fullCount;
		}
	// A second blend at the crossing would reach ~192 alpha; the union caps
	// the result at one stroke's full-coverage alpha (128).
	c.check(maxA <= 130, "overlapping stroke never double-covers a sub-pixel");
	c.check(fullCount >= 4, "stroke has fully covered pixels");
	nvgDeleteCPU(vg);
}

static void testStrokeThin(Case& c)
{
	// Width below one pixel: the core already scaled the alpha; the back-end
	// must not scale it a second time.
	Surface s(40, 8);
	s.zero();
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 40, 8, 1);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 2, 4);
	nvgLineTo(vg, 38, 4);
	nvgStrokeWidth(vg, 0.5f);
	nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 255));
	nvgStroke(vg);
	nvgEndFrame(vg);
	int maxA = 0;
	for (int y = 0; y < s.h; ++y)
		for (int x = 0; x < s.w; ++x)
			maxA = std::max(maxA, (int)s.at(x, y)[3]);
	// 0.5 px -> core factor 0.25 -> alpha ~64. Squaring again would give ~16.
	c.check(maxA > 30 && maxA < 110, "thin stroke keeps the core's alpha factor");
	nvgDeleteCPU(vg);
}

static void testStrokeOrder(Case& c)
{
	// Two separate strokes blend in call order.
	Surface a(16, 16), b(16, 16);
	a.zero(); b.zero();
	auto draw = [](Surface& s, bool redFirst) {
		NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
		nvgBeginFrame(vg, 16, 16, 1);
		auto red = [&]() {
			nvgBeginPath(vg); nvgRect(vg, 2, 2, 12, 12);
			nvgStrokeWidth(vg, 6); nvgStrokeColor(vg, nvgRGBA(255, 0, 0, 128)); nvgStroke(vg);
		};
		auto blue = [&]() {
			nvgBeginPath(vg); nvgRect(vg, 2, 2, 12, 12);
			nvgStrokeWidth(vg, 6); nvgStrokeColor(vg, nvgRGBA(0, 0, 255, 128)); nvgStroke(vg);
		};
		if (redFirst) { red(); blue(); } else { blue(); red(); }
		nvgEndFrame(vg);
		nvgDeleteCPU(vg);
	};
	draw(a, true);
	draw(b, false);
	bool differs = false;
	for (int i = 0; i < (int)a.px.size(); ++i)
		if (a.px[i] != b.px[i]) { differs = true; break; }
	c.check(differs, "stroke draw order changes the result");

	// Hand-checked source-over at a fully covered point (red then blue).
	int expected[4];
	int afterRed[4];
	premulOver(255, 0, 0, 128, 0, 0, 0, 0, afterRed);
	premulOver(0, 0, 255, 128, afterRed[0], afterRed[1], afterRed[2], afterRed[3], expected);
	// Interior of the stroked ring around (3,8).
	expectPixel(c, a, 3, 8, expected[0], expected[1], expected[2], expected[3], 6,
				"ordered source-over matches reference");
}

// ---------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------

static void putRGBA(std::vector<unsigned char>& d, int w, int x, int y,
					int r, int g, int b, int a)
{
	unsigned char* p = &d[((size_t)y * w + x) * 4];
	p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b; p[3] = (unsigned char)a;
}

static void testTextureNearest(Case& c)
{
	// Asymmetric 3x2 RGBA texture, opaque, nearest sampling.
	std::vector<unsigned char> d(3 * 2 * 4, 0);
	putRGBA(d, 3, 0, 0, 255, 0, 0, 255);
	putRGBA(d, 3, 1, 0, 0, 255, 0, 255);
	putRGBA(d, 3, 2, 0, 0, 0, 255, 255);
	putRGBA(d, 3, 0, 1, 10, 20, 30, 255);
	putRGBA(d, 3, 1, 1, 40, 50, 60, 255);
	putRGBA(d, 3, 2, 1, 70, 80, 90, 255);

	Surface s(6, 4);
	s.zero();
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	int img = nvgCreateImageRGBA(vg, 3, 2, NVG_IMAGE_NEAREST, d.data());
	c.check(img != 0, "image created");
	int iw, ih;
	nvgImageSize(vg, img, &iw, &ih);
	c.check(iw == 3 && ih == 2, "reported texture size");
	nvgBeginFrame(vg, 6, 4, 1);
	NVGpaint pat = nvgImagePattern(vg, 0, 0, 3, 2, 0, img, 1.0f);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 3, 2); nvgFillPaint(vg, pat); nvgFill(vg);
	nvgEndFrame(vg);
	expectPixel(c, s, 0, 0, 255, 0, 0, 255, 2, "nearest texel (0,0)");
	expectPixel(c, s, 2, 0, 0, 0, 255, 255, 2, "nearest texel (2,0)");
	expectPixel(c, s, 1, 1, 40, 50, 60, 255, 2, "nearest texel (1,1)");
	nvgDeleteCPU(vg);
}

static void testTextureStraightVsPremul(Case& c)
{
	std::vector<unsigned char> d(4, 0);
	putRGBA(d, 1, 0, 0, 200, 100, 50, 128); // straight alpha
	Surface sa(2, 2), sp(2, 2);
	sa.zero(); sp.zero();

	NVGcontext* va = nvgCreateCPU(sa.px.data(), sa.w, sa.h, sa.stride);
	int ia = nvgCreateImageRGBA(va, 1, 1, 0, d.data());
	nvgBeginFrame(va, 2, 2, 1);
	NVGpaint pa = nvgImagePattern(va, 0, 0, 1, 1, 0, ia, 1.0f);
	nvgBeginPath(va); nvgRect(va, 0, 0, 1, 1); nvgFillPaint(va, pa); nvgFill(va);
	nvgEndFrame(va);

	// Premultiplied upload of the same logical color.
	std::vector<unsigned char> dp(4);
	int pr = premulByte(200, 128), pg = premulByte(100, 128), pb = premulByte(50, 128);
	putRGBA(dp, 1, 0, 0, pr, pg, pb, 128);
	NVGcontext* vb = nvgCreateCPU(sp.px.data(), sp.w, sp.h, sp.stride);
	int ib = nvgCreateImageRGBA(vb, 1, 1, NVG_IMAGE_PREMULTIPLIED, dp.data());
	nvgBeginFrame(vb, 2, 2, 1);
	NVGpaint pb2 = nvgImagePattern(vb, 0, 0, 1, 1, 0, ib, 1.0f);
	nvgBeginPath(vb); nvgRect(vb, 0, 0, 1, 1); nvgFillPaint(vb, pb2); nvgFill(vb);
	nvgEndFrame(vb);

	int expected[4];
	premulOver(200, 100, 50, 128, 0, 0, 0, 0, expected);
	expectPixel(c, sa, 0, 0, expected[0], expected[1], expected[2], expected[3], 2,
				"straight-alpha texture premultiplied on sampling");
	expectPixel(c, sp, 0, 0, expected[0], expected[1], expected[2], expected[3], 2,
				"premultiplied texture used directly");
	nvgDeleteCPU(va);
	nvgDeleteCPU(vb);
}

static void testTextureFilterModes(Case& c)
{
	// 2x1 texture: black then white, stretched over 8 physical pixels.
	std::vector<unsigned char> d(2 * 1 * 4, 0);
	putRGBA(d, 2, 0, 0, 0, 0, 0, 255);
	putRGBA(d, 2, 1, 0, 255, 255, 255, 255);

	Surface sn(8, 1), sl(8, 1);
	sn.zero(); sl.zero();
	NVGcontext* vn = nvgCreateCPU(sn.px.data(), sn.w, sn.h, sn.stride);
	int in = nvgCreateImageRGBA(vn, 2, 1, NVG_IMAGE_NEAREST, d.data());
	nvgBeginFrame(vn, 8, 1, 1);
	NVGpaint pn = nvgImagePattern(vn, 0, 0, 8, 1, 0, in, 1.0f);
	nvgBeginPath(vn); nvgRect(vn, 0, 0, 8, 1); nvgFillPaint(vn, pn); nvgFill(vn);
	nvgEndFrame(vn);

	NVGcontext* vl = nvgCreateCPU(sl.px.data(), sl.w, sl.h, sl.stride);
	int il = nvgCreateImageRGBA(vl, 2, 1, 0, d.data());
	nvgBeginFrame(vl, 8, 1, 1);
	NVGpaint pl = nvgImagePattern(vl, 0, 0, 8, 1, 0, il, 1.0f);
	nvgBeginPath(vl); nvgRect(vl, 0, 0, 8, 1); nvgFillPaint(vl, pl); nvgFill(vl);
	nvgEndFrame(vl);

	// Nearest: hard band, every pixel is either black or white.
	bool hardBand = true;
	for (int x = 0; x < 8; ++x) {
		unsigned char r = sn.at(x, 0)[0];
		if (r != 0 && r != 255) hardBand = false;
	}
	c.check(hardBand, "nearest produces hard bands");
	// Bilinear: boundary region contains intermediate gray texel values.
	bool foundGray = false;
	for (int x = 0; x < 8; ++x) {
		unsigned char r = sl.at(x, 0)[0];
		if (r > 20 && r < 235) foundGray = true;
	}
	c.check(foundGray, "texel-center bilinear produces intermediate values");
	nvgDeleteCPU(vn);
	nvgDeleteCPU(vl);
}

static void testTextureRepeatFlip(Case& c)
{
	std::vector<unsigned char> d(3 * 1 * 4, 0);
	putRGBA(d, 3, 0, 0, 10, 0, 0, 255);
	putRGBA(d, 3, 1, 0, 20, 0, 0, 255);
	putRGBA(d, 3, 2, 0, 30, 0, 0, 255);

	Surface s(9, 2);
	s.zero();
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	int img = nvgCreateImageRGBA(vg, 3, 1, NVG_IMAGE_REPEATX, d.data());
	nvgBeginFrame(vg, 9, 2, 1);
	NVGpaint pat = nvgImagePattern(vg, 0, 0, 3, 1, 0, img, 1.0f);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 9, 1); nvgFillPaint(vg, pat); nvgFill(vg);
	nvgEndFrame(vg);
	// Second tile mirrors the first: pixel 4 == pixel 1.
	c.check(std::memcmp(s.at(4, 0), s.at(1, 0), 4) == 0, "REPEATX wraps the pattern");
	c.check(std::memcmp(s.at(7, 0), s.at(1, 0), 4) == 0, "REPEATX third tile consistent");
	nvgDeleteCPU(vg);

	// FLIPY: top image row samples the texture's bottom row. Nearest filtering
	// keeps the row mapping exact (bilinear would legitimately blend the two
	// texels across sub-pixel positions).
	std::vector<unsigned char> d2(1 * 2 * 4, 0);
	putRGBA(d2, 1, 0, 0, 11, 0, 0, 255);
	putRGBA(d2, 1, 0, 1, 22, 0, 0, 255);
	Surface f(1, 2), nf(1, 2);
	f.zero(); nf.zero();
	NVGcontext* vf = nvgCreateCPU(f.px.data(), f.w, f.h, f.stride);
	int jf = nvgCreateImageRGBA(vf, 1, 2, NVG_IMAGE_FLIPY | NVG_IMAGE_NEAREST, d2.data());
	nvgBeginFrame(vf, 1, 2, 1);
	NVGpaint pf = nvgImagePattern(vf, 0, 0, 1, 2, 0, jf, 1.0f);
	nvgBeginPath(vf); nvgRect(vf, 0, 0, 1, 2); nvgFillPaint(vf, pf); nvgFill(vf);
	nvgEndFrame(vf);
	NVGcontext* vo = nvgCreateCPU(nf.px.data(), nf.w, nf.h, nf.stride);
	int jo = nvgCreateImageRGBA(vo, 1, 2, NVG_IMAGE_NEAREST, d2.data());
	nvgBeginFrame(vo, 1, 2, 1);
	NVGpaint po = nvgImagePattern(vo, 0, 0, 1, 2, 0, jo, 1.0f);
	nvgBeginPath(vo); nvgRect(vo, 0, 0, 1, 2); nvgFillPaint(vo, po); nvgFill(vo);
	nvgEndFrame(vo);
	c.check(f.at(0, 0)[0] == 22 && f.at(0, 1)[0] == 11, "FLIPY swaps texture rows");
	c.check(nf.at(0, 0)[0] == 11 && nf.at(0, 1)[0] == 22, "non-flipped keeps texture row order");
	nvgDeleteCPU(vf);
	nvgDeleteCPU(vo);
}

static void testTextureUpdateDelete(Case& c)
{
	std::vector<unsigned char> a(2 * 2 * 4, 0), b(2 * 2 * 4, 0);
	putRGBA(a, 2, 0, 0, 255, 0, 0, 255);
	putRGBA(a, 2, 1, 0, 255, 0, 0, 255);
	putRGBA(a, 2, 0, 1, 255, 0, 0, 255);
	putRGBA(a, 2, 1, 1, 255, 0, 0, 255);
	putRGBA(b, 2, 0, 0, 0, 255, 0, 255);
	putRGBA(b, 2, 1, 0, 0, 255, 0, 255);
	putRGBA(b, 2, 0, 1, 0, 255, 0, 255);
	putRGBA(b, 2, 1, 1, 0, 255, 0, 255);

	Surface s(8, 4);
	s.zero();
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	int img = nvgCreateImageRGBA(vg, 2, 2, NVG_IMAGE_NEAREST, a.data());
	nvgBeginFrame(vg, 8, 4, 1);
	NVGpaint p1 = nvgImagePattern(vg, 0, 0, 2, 2, 0, img, 1.0f);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 2, 2); nvgFillPaint(vg, p1); nvgFill(vg);
	// Update after the draw was recorded: the first draw keeps the old content.
	nvgUpdateImage(vg, img, b.data());
	NVGpaint p2 = nvgImagePattern(vg, 3, 0, 2, 2, 0, img, 1.0f);
	nvgBeginPath(vg); nvgRect(vg, 3, 0, 2, 2); nvgFillPaint(vg, p2); nvgFill(vg);
	nvgEndFrame(vg);
	expectPixel(c, s, 0, 0, 255, 0, 0, 255, 2, "recorded draw kept pre-update texture");
	expectPixel(c, s, 3, 0, 0, 255, 0, 255, 2, "later draw reads updated texture");

	// Deleting the image must not break a draw that was recorded first.
	nvgBeginFrame(vg, 8, 4, 1);
	NVGpaint p3 = nvgImagePattern(vg, 0, 2, 2, 2, 0, img, 1.0f);
	nvgBeginPath(vg); nvgRect(vg, 0, 2, 2, 2); nvgFillPaint(vg, p3); nvgFill(vg);
	nvgDeleteImage(vg, img);
	nvgEndFrame(vg);
	expectPixel(c, s, 0, 2, 0, 255, 0, 255, 2, "recorded draw survives texture deletion");
	int dw = -1, dh = -1;
	nvgImageSize(vg, img, &dw, &dh); // deleted handle leaves outputs untouched
	c.check(dw == -1 && dh == -1, "deleted handle no longer resolves");
	nvgDeleteCPU(vg);
}

static void testFontTriangles(Case& c)
{
	// Font glyphs arrive through renderTriangles with an alpha atlas texture.
	// A large size gives fully covered core pixels to test shared-edge handling.
	Surface s(96, 48);
	s.zero();
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	int font = nvgCreateFont(vg, "roboto", "example/Roboto-Regular.ttf");
	c.check(font >= 0, "roboto font loaded");
	nvgBeginFrame(vg, 96, 48, 1);
	nvgFontFaceId(vg, font);
	nvgFontSize(vg, 40);
	nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
	nvgText(vg, 6, 38, "AV", nullptr);
	nvgEndFrame(vg);

	int lit = 0, solid = 0;
	for (int y = 0; y < s.h; ++y)
		for (int x = 0; x < s.w; ++x) {
			const unsigned char* p = s.at(x, y);
			if (p[3] > 0) ++lit;
			if (p[3] == 255) ++solid;
		}
	c.check(lit > 100, "glyph triangles cover pixels");
	c.check(solid >= 16, "glyph interiors are fully covered");

	// Every fully covered glyph-core pixel must be exact, uniform white. If the
	// shared diagonal of the two glyph triangles were covered twice (or UVs
	// taken from the fill paint), these pixels would differ or over-accumulate.
	int uniformWhite = 0;
	for (int y = 0; y < s.h; ++y)
		for (int x = 0; x < s.w; ++x) {
			const unsigned char* p = s.at(x, y);
			if (p[3] == 255 && p[0] == 255 && p[1] == 255 && p[2] == 255)
				++uniformWhite;
		}
	c.check(uniformWhite == solid, "glyph core is uniform white (half-open shared edges)");

	// Glyph coverage ramps through intermediate alpha at the edges (the alpha
	// atlas is sampled per sub-pixel, not thresholded).
	bool hasPartial = false;
	for (int y = 0; y < s.h; ++y)
		for (int x = 0; x < s.w; ++x) {
			int a = s.at(x, y)[3];
			if (a > 0 && a < 255) hasPartial = true;
		}
	c.check(hasPartial, "glyph edges show intermediate coverage");
	nvgDeleteCPU(vg);
}

// ---------------------------------------------------------------------------
// frames
// ---------------------------------------------------------------------------

static void testFrameCancel(Case& c)
{
	Surface s(12, 12);
	s.fill(0x77);
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 12, 12, 1);
	nvgBeginPath(vg); nvgRect(vg, 2, 2, 8, 8);
	nvgFillColor(vg, nvgRGBA(255, 0, 0, 255)); nvgFill(vg);
	nvgCancelFrame(vg);
	bool untouched = true;
	for (unsigned char v : s.px) if (v != 0x77) { untouched = false; break; }
	c.check(untouched, "cancel leaves every target byte unchanged");

	// The next frame starts from the target's current content.
	nvgBeginFrame(vg, 12, 12, 1);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 4, 12);
	nvgFillColor(vg, nvgRGBA(0, 255, 0, 255)); nvgFill(vg);
	nvgEndFrame(vg);
	expectPixel(c, s, 2, 6, 0, 255, 0, 255, 2, "frame after cancel renders");
	expectPixel(c, s, 8, 6, 0x77, 0x77, 0x77, 0x77, 0, "cancelled region still holds old bytes");
	nvgDeleteCPU(vg);
}

static void testFrameDeferredPublish(Case& c)
{
	Surface s(10, 10);
	s.fill(0x33);
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 10, 10, 1);
	nvgBeginPath(vg); nvgRect(vg, 1, 1, 8, 8);
	nvgFillColor(vg, nvgRGBA(255, 0, 0, 255)); nvgFill(vg);
	// Before EndFrame the borrowed bytes are still the original content.
	bool deferred = true;
	for (unsigned char v : s.px) if (v != 0x33) { deferred = false; break; }
	c.check(deferred, "target untouched until EndFrame");
	nvgEndFrame(vg);
	c.check(s.at(5, 5)[0] == 255, "EndFrame publishes the accumulated frame");
	nvgDeleteCPU(vg);
}

static void testFrameRepeated(Case& c)
{
	Surface s(12, 6);
	s.zero();
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 12, 6, 1);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 6, 6);
	nvgFillColor(vg, nvgRGBA(255, 0, 0, 255)); nvgFill(vg);
	nvgEndFrame(vg);
	nvgBeginFrame(vg, 12, 6, 1);
	nvgBeginPath(vg); nvgRect(vg, 6, 0, 6, 6);
	nvgFillColor(vg, nvgRGBA(0, 0, 255, 255)); nvgFill(vg);
	nvgEndFrame(vg);
	expectPixel(c, s, 3, 3, 255, 0, 0, 255, 2, "first frame persists");
	expectPixel(c, s, 9, 3, 0, 0, 255, 255, 2, "second frame draws alongside");

	// Third frame blends on top of the latest target content.
	nvgBeginFrame(vg, 12, 6, 1);
	nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 6, 6);
	nvgFillColor(vg, nvgRGBA(0, 128, 0, 255)); nvgFill(vg);
	nvgEndFrame(vg);
	// Lighter adds green onto the existing red region.
	c.check(s.at(3, 3)[1] >= 120, "later frame reads the target's latest content");
	nvgDeleteCPU(vg);
}

static void testFramePaddingSentinel(Case& c)
{
	const int pad = 8;
	Surface s(8, 4, pad);
	s.fill(0x5A);
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 8, 4, 1);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 8, 4);
	nvgFillColor(vg, nvgRGBA(10, 20, 30, 255)); nvgFill(vg);
	nvgEndFrame(vg);
	bool paddingKept = true;
	for (int y = 0; y < s.h; ++y)
		for (int k = 4 * s.w; k < s.stride; ++k)
			if (s.px[(size_t)y * s.stride + k] != 0x5A) paddingKept = false;
	c.check(paddingKept, "row padding bytes are never written");
	expectPixel(c, s, 7, 3, 10, 20, 30, 255, 2, "last valid pixel written");
	nvgDeleteCPU(vg);
}

static void testFrameBlendFactors(Case& c)
{
	// Seed a known destination, then use separate RGB and alpha factors.
	Surface s(8, 8);
	s.zero();
	unsigned char* d = s.at(3, 3);
	d[0] = 100; d[1] = 110; d[2] = 120; d[3] = 200;
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 8, 8, 1);
	// RGB: src*ONE + dst*ZERO (replace color). Alpha: src*ZERO + dst*ONE
	// (keep destination alpha).
	nvgGlobalCompositeBlendFuncSeparate(vg, NVG_ONE, NVG_ZERO, NVG_ZERO, NVG_ONE);
	nvgBeginPath(vg); nvgRect(vg, 2, 2, 4, 4);
	nvgFillColor(vg, nvgRGBA(10, 20, 30, 64)); nvgFill(vg);
	nvgEndFrame(vg);
	const unsigned char* p = s.at(3, 3);
	// Premultiplied source color with alpha 64; RGB replaced, alpha kept 200.
	c.check(p[3] == 200, "alpha channel keeps destination under separate factors");
	c.check(p[0] == premulByte(10, 64) && p[1] == premulByte(20, 64) &&
			p[2] == premulByte(30, 64), "rgb channel replaced under separate factors");

	// NVG_COPY on a zero target reproduces the premultiplied source exactly.
	Surface s2(8, 8);
	s2.zero();
	NVGcontext* v2 = nvgCreateCPU(s2.px.data(), s2.w, s2.h, s2.stride);
	nvgBeginFrame(v2, 8, 8, 1);
	nvgGlobalCompositeOperation(v2, NVG_COPY);
	nvgBeginPath(v2); nvgRect(v2, 2, 2, 4, 4);
	nvgFillColor(v2, nvgRGBA(90, 180, 30, 140)); nvgFill(v2);
	nvgEndFrame(v2);
	expectPixel(c, s2, 4, 4, premulByte(90, 140), premulByte(180, 140),
				premulByte(30, 140), 140, 2, "COPY blend reproduces premul source");
	nvgDeleteCPU(vg);
	nvgDeleteCPU(v2);
}

static void testFrameDprScissor(Case& c)
{
	// Logical 20x10 at DPR 2 -> physical 40x20 target. The application applies
	// the DPR scale itself (as the NanoVG demo does); the back-end must not
	// apply DPR a second time.
	Surface s(40, 20);
	s.zero();
	NVGcontext* vg = nvgCreateCPU(s.px.data(), s.w, s.h, s.stride);
	nvgBeginFrame(vg, 20, 10, 2.0f);
	nvgScale(vg, 2.0f, 2.0f);
	nvgBeginPath(vg); nvgRect(vg, 5, 5, 5, 5);
	nvgFillColor(vg, nvgRGBA(255, 0, 0, 255)); nvgFill(vg);
	nvgEndFrame(vg);
	expectPixel(c, s, 15, 15, 255, 0, 0, 255, 2, "DPR scales logical rect to physical");
	expectPixel(c, s, 5, 5, 0, 0, 0, 0, 0, "DPR does not double-apply outside rect");
	nvgDeleteCPU(vg);

	// Rotated scissor at DPR 2: clip a large fill to a rotated square.
	Surface s2(40, 20);
	s2.zero();
	vg = nvgCreateCPU(s2.px.data(), s2.w, s2.h, s2.stride);
	nvgBeginFrame(vg, 20, 10, 2.0f);
	nvgScale(vg, 2.0f, 2.0f);
	// Build the scissor while rotated; it bakes the current transform into its
	// own matrix, so resetting the draw transform afterwards leaves it active.
	nvgTranslate(vg, 10, 5);
	nvgRotate(vg, NVG_PI / 4.0f);
	nvgScissor(vg, -4, -4, 8, 8);
	nvgResetTransform(vg);
	nvgScale(vg, 2.0f, 2.0f);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 20, 10);
	nvgFillColor(vg, nvgRGBA(0, 255, 0, 255)); nvgFill(vg);
	nvgResetScissor(vg);
	nvgEndFrame(vg);
	// Center of the rotated scissor is painted.
	c.check(s2.at(20, 10)[3] == 255, "rotated scissor keeps its center");
	// A far corner is outside the rotated square and stays empty.
	c.check(s2.at(2, 2)[3] == 0, "rotated scissor clips a far corner");
	// After resetting the scissor a following fill is no longer clipped.
	nvgBeginFrame(vg, 20, 10, 2.0f);
	nvgScale(vg, 2.0f, 2.0f);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 20, 10);
	nvgFillColor(vg, nvgRGBA(0, 0, 255, 255)); nvgFill(vg);
	nvgEndFrame(vg);
	c.check(s2.at(2, 2)[2] == 255, "scissor reset restores full drawing");
	nvgDeleteCPU(vg);
}

static void testTwoContexts(Case& c)
{
	Surface a(12, 12), b(12, 12);
	a.zero(); b.zero();
	NVGcontext* va = nvgCreateCPU(a.px.data(), a.w, a.h, a.stride);
	NVGcontext* vb = nvgCreateCPU(b.px.data(), b.w, b.h, b.stride);
	c.check(va != vb, "two distinct contexts");

	// Interleave frames; each context keeps its own frame state.
	nvgBeginFrame(va, 12, 12, 1);
	nvgBeginPath(va); nvgRect(va, 0, 0, 6, 12);
	nvgFillColor(va, nvgRGBA(255, 0, 0, 255)); nvgFill(va);

	nvgBeginFrame(vb, 12, 12, 1);
	nvgBeginPath(vb); nvgRect(vb, 6, 0, 6, 12);
	nvgFillColor(vb, nvgRGBA(0, 0, 255, 255)); nvgFill(vb);

	nvgEndFrame(va);
	nvgEndFrame(vb);

	expectPixel(c, a, 3, 6, 255, 0, 0, 255, 2, "context A drew to its own target");
	expectPixel(c, a, 9, 6, 0, 0, 0, 0, 0, "context A target unaffected by B");
	expectPixel(c, b, 9, 6, 0, 0, 255, 255, 2, "context B drew to its own target");
	expectPixel(c, b, 3, 6, 0, 0, 0, 0, 0, "context B target unaffected by A");

	// Textures are per-context: an id from A is unknown inside B.
	std::vector<unsigned char> d(4, 0);
	putRGBA(d, 1, 0, 0, 1, 2, 3, 255);
	int imgA = nvgCreateImageRGBA(va, 1, 1, 0, d.data());
	int w = -1, h = -1;
	nvgImageSize(va, imgA, &w, &h);
	c.check(w == 1 && h == 1, "image resolves in owning context");
	w = h = -1;
	nvgImageSize(vb, imgA, &w, &h);
	c.check(w == -1 && h == -1, "image handle is isolated per context");
	nvgDeleteCPU(va);
	nvgDeleteCPU(vb);
}

} // namespace

void register_task_tests(Registry& r)
{
	r.add("fills", "invalid arguments", testFillInvalid);
	r.add("fills", "solid rect and coverage", testFillSolid);
	r.add("fills", "premultiplied translucent fill", testFillPremul);
	r.add("fills", "hole and overlapping contours", testFillHoleAndContours);
	r.add("fills", "self intersecting winding", testFillSelfIntersect);

	r.add("strokes", "overlap union", testStrokeUnion);
	r.add("strokes", "sub-pixel width alpha", testStrokeThin);
	r.add("strokes", "call order blending", testStrokeOrder);

	r.add("textures", "nearest asymmetric texture", testTextureNearest);
	r.add("textures", "straight vs premultiplied", testTextureStraightVsPremul);
	r.add("textures", "nearest vs bilinear", testTextureFilterModes);
	r.add("textures", "repeat and flipy", testTextureRepeatFlip);
	r.add("textures", "update and delete timing", testTextureUpdateDelete);
	r.add("textures", "font alpha atlas triangles", testFontTriangles);

	r.add("frames", "cancel frame", testFrameCancel);
	r.add("frames", "deferred publish", testFrameDeferredPublish);
	r.add("frames", "repeated frames", testFrameRepeated);
	r.add("frames", "padding sentinel", testFramePaddingSentinel);
	r.add("frames", "separate blend factors", testFrameBlendFactors);
	r.add("frames", "dpr and rotated scissor", testFrameDprScissor);
	r.add("frames", "two isolated contexts", testTwoContexts);
}
