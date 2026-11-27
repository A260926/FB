// Task tests for the NanoVG CPU backend. Registered through
// register_task_tests(); this file has no main() (see task_support/main.cpp).
//
// Expected coverage is derived from small independent grid references rather
// than hard-coded screenshots: a standalone winding-number routine for
// fills, an inverse-transform rectangle test for scissors and a float blend
// equation reference for the composite factors.

#include "registry.hpp"
#include "nanovg_cpu.h"

#include <vector>
#include <array>
#include <string>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace {

struct Image {
	int w, h;
	std::vector<unsigned char> rgba;
	explicit Image(int w_, int h_) : w(w_), h(h_), rgba((size_t)w_*h_*4, 0) {}
	void set(int x, int y, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
		unsigned char* p = rgba.data() + ((size_t)y*w + x)*4;
		p[0]=r; p[1]=g; p[2]=b; p[3]=a;
	}
	const unsigned char* data() const { return rgba.data(); }
};

struct Buffer {
	int w, h, stride;
	std::vector<unsigned char> bytes;
	Buffer(int w_, int h_, int extraStride = 0, int tail = 0)
		: w(w_), h(h_), stride(w_*4 + extraStride),
		  bytes((size_t)h_*stride + tail, 0) {}
	unsigned char* px(int x, int y) { return bytes.data() + (size_t)y*stride + x*4; }
	const unsigned char* px(int x, int y) const { return bytes.data() + (size_t)y*stride + x*4; }
};

constexpr int SUB = 4;

float sampleX(int px, int s) { return px + (s % SUB + 0.5f) / SUB; }
float sampleY(int py, int s) { return py + (s / SUB + 0.5f) / SUB; }

// Independent winding-number reference. Implemented in a y-up space by
// negating the NanoVG y-down coordinates, so it does not share any sign
// convention with the backend under test.
int refWinding(const std::vector<std::array<float,2>>& poly, float qx, float qy)
{
	float y = -qy;
	int wn = 0;
	for (size_t i = 0; i < poly.size(); i++) {
		float ax = poly[i][0], ay = -poly[i][1];
		float bx = poly[(i+1)%poly.size()][0], by = -poly[(i+1)%poly.size()][1];
		float isLeft = (bx-ax)*(y-ay) - (by-ay)*(qx-ax);
		if (ay <= y) {
			if (by > y && isLeft > 0.0f) wn++;
		} else {
			if (by <= y && isLeft < 0.0f) wn--;
		}
	}
	return wn;
}

int refCoverage(const std::vector<std::vector<std::array<float,2>>>& contours,
				int px, int py)
{
	int n = 0;
	for (int s = 0; s < SUB*SUB; s++) {
		int wn = 0;
		for (const auto& c : contours) wn += refWinding(c, sampleX(px,s), sampleY(py,s));
		if (wn != 0) n++;
	}
	return n;
}

// Independent rotated-rectangle test for scissors: transform the point back
// into the rectangle's local space and compare against its half extents.
bool refInsideRect(float px, float py, float cx, float cy, float angle,
				   float halfW, float halfH)
{
	float dx = px - cx, dy = py - cy;
	float c = std::cos(-angle), sn = std::sin(-angle);
	float lx = c*dx - sn*dy;
	float ly = sn*dx + c*dy;
	return std::fabs(lx) <= halfW && std::fabs(ly) <= halfH;
}

// Float blend equation reference, RGB and alpha factors evaluated separately.
float refFactor(int f, bool alpha, const float s[4], const float d[4])
{
	switch (f) {
	case NVG_ZERO: return 0.0f;
	case NVG_ONE: return 1.0f;
	case NVG_SRC_COLOR: return alpha ? s[3] : s[0];
	case NVG_ONE_MINUS_SRC_COLOR: return 1.0f - (alpha ? s[3] : s[0]);
	case NVG_DST_COLOR: return alpha ? d[3] : d[0];
	case NVG_ONE_MINUS_DST_COLOR: return 1.0f - (alpha ? d[3] : d[0]);
	case NVG_SRC_ALPHA: return s[3];
	case NVG_ONE_MINUS_SRC_ALPHA: return 1.0f - s[3];
	case NVG_DST_ALPHA: return d[3];
	case NVG_ONE_MINUS_DST_ALPHA: return 1.0f - d[3];
	case NVG_SRC_ALPHA_SATURATE:
		return alpha ? 1.0f : std::fmin(s[3], 1.0f - d[3]);
	default: return 1.0f;
	}
}

unsigned char roundByte(float v)
{
	if (v < 0.0f) v = 0.0f;
	if (v > 1.0f) v = 1.0f;
	return (unsigned char)(int)(v*255.0f + 0.5f);
}

void refBlend(const unsigned char dst[4], const float srcIn[4],
			  const NVGcompositeOperationState& op, unsigned char out[4])
{
	float s[4], d[4];
	for (int k = 0; k < 4; k++) {
		s[k] = std::fmin(std::fmax(srcIn[k],0.0f),1.0f);
		d[k] = dst[k] / 255.0f;
	}
	float sr = refFactor(op.srcRGB,false,s,d), dr = refFactor(op.dstRGB,false,s,d);
	float sa = refFactor(op.srcAlpha,true,s,d),  da = refFactor(op.dstAlpha,true,s,d);
	out[0] = roundByte(s[0]*sr + d[0]*dr);
	out[1] = roundByte(s[1]*sr + d[1]*dr);
	out[2] = roundByte(s[2]*sr + d[2]*dr);
	out[3] = roundByte(s[3]*sa + d[3]*da);
}

bool nearByte(int a, int b, int tol = 2) { return std::abs(a-b) <= tol; }

// ---------------------------------------------------------------------------
// fills
// ---------------------------------------------------------------------------

void fillsHole(Case& c)
{
	Buffer buf(32, 24);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), buf.w, buf.h, buf.stride);
	c.check(vg != nullptr, "create");
	nvgBeginFrame(vg, 32, 24, 1.0f);
	nvgBeginPath(vg);
	nvgRect(vg, 2, 2, 12, 12);
	nvgRect(vg, 6, 6, 4, 4);
	nvgPathWinding(vg, NVG_HOLE);
	nvgFillColor(vg, nvgRGBA(255, 0, 0, 255));
	nvgFill(vg);
	nvgEndFrame(vg);

	std::vector<std::vector<std::array<float,2>>> contours;
	contours.push_back({{{2,2},{2,14},{14,14},{14,2}}});
	contours.push_back({{{6,6},{10,6},{10,10},{6,10}}});

	int checked = 0;
	for (int y = 1; y < 16; y++) {
		for (int x = 1; x < 16; x++) {
			int cov = refCoverage(contours, x, y);
			if (cov != 0 && cov != 16) continue; // boundary pixels are tested elsewhere
			const unsigned char* p = buf.px(x, y);
			if (cov == 16) {
				c.check(p[3] == 255 && p[0] == 255 && p[1] == 0 && p[2] == 0,
						"solid contour pixel");
			} else {
				c.check(p[3] == 0 && p[0] == 0, "hole and outside stay empty");
			}
			checked++;
		}
	}
	c.check(checked > 100, "reference grid exercised");
	nvgDeleteCPU(vg);
}

void fillsSelfIntersect(Case& c)
{
	// Bowtie drawn as one self-intersecting contour. The two lobes have
	// opposite orientation, yet the non-zero rule fills both; a backend that
	// rasterizes the fan triangles as independent solid faces would paint the
	// crossing seam a second time.
	Buffer buf(20, 18);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), buf.w, buf.h, buf.stride);
	unsigned char alpha = 200;
	nvgBeginFrame(vg, 20, 18, 1.0f);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 2, 2);
	nvgLineTo(vg, 12, 12);
	nvgLineTo(vg, 12, 2);
	nvgLineTo(vg, 2, 12);
	nvgClosePath(vg);
	nvgFillColor(vg, nvgRGBA(10, 200, 40, alpha));
	nvgFill(vg);
	nvgEndFrame(vg);

	std::vector<std::vector<std::array<float,2>>> contours;
	contours.push_back({{{2,2},{12,12},{12,2},{2,12}}});

	c.check(buf.px(4, 4)[3] > 0 && buf.px(10, 4)[3] > 0, "both lobes are filled");
	c.check(buf.px(2, 12)[3] == 0, "outside the bowtie stays empty");

	int checked = 0;
	for (int y = 2; y < 13; y++) {
		for (int x = 2; x < 13; x++) {
			int cov = refCoverage(contours, x, y);
			if (cov != 0 && cov != 16) continue;
			const unsigned char* p = buf.px(x, y);
			unsigned char expectA = roundByte((alpha/255.0f) * (cov/16.0f));
			c.check(nearByte(p[3], expectA, 2), "bowtie winding reference coverage");
			checked++;
		}
	}
	c.check(checked > 40, "bowtie reference grid exercised");
	nvgDeleteCPU(vg);
}

void fillsSlitHole(Case& c)
{
	// A single contour with a zero-width slit leading into an inner box wound
	// in the reverse direction. The inner region has winding zero and must be
	// empty even though it is enclosed by filled geometry.
	static const float pts[][2] = {
		{2,2},{18,2},{18,18},{2,18},{2,10},{8,10},
		{8,14},{14,14},{14,8},{8,8},{8,10},{2,10}
	};
	std::vector<std::array<float,2>> ref;
	for (const auto& p : pts) ref.push_back({p[0], p[1]});

	Buffer buf(22, 22);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 22, 22, 22*4);
	nvgBeginFrame(vg, 22, 22, 1.0f);
	nvgBeginPath(vg);
	nvgMoveTo(vg, pts[0][0], pts[0][1]);
	for (size_t i = 1; i < sizeof(pts)/sizeof(pts[0]); i++)
		nvgLineTo(vg, pts[i][0], pts[i][1]);
	nvgClosePath(vg);
	nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
	nvgFill(vg);
	nvgEndFrame(vg);

	std::vector<std::vector<std::array<float,2>>> contours;
	contours.push_back(ref);
	c.check(buf.px(11, 11)[3] == 0, "reverse-wound inner box cancels");
	c.check(buf.px(4, 4)[3] == 255, "outer region remains solid");
	c.check(buf.px(16, 16)[3] == 255, "far corner remains solid");

	int checked = 0;
	for (int y = 2; y < 19; y++) {
		for (int x = 2; x < 19; x++) {
			int cov = refCoverage(contours, x, y);
			if (cov != 0 && cov != 16) continue;
			const unsigned char* p = buf.px(x, y);
			c.check((p[3] == 255) == (cov == 16), "slit winding reference coverage");
			checked++;
		}
	}
	c.check(checked > 150, "slit reference grid exercised");
	nvgDeleteCPU(vg);
}

void fillsMultipleContours(Case& c)
{
	Buffer buf(40, 12);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), buf.w, buf.h, buf.stride);
	nvgBeginFrame(vg, 40, 12, 1.0f);
	nvgBeginPath(vg);
	nvgRect(vg, 1, 2, 6, 6);
	nvgRect(vg, 20, 2, 6, 6);
	nvgFillColor(vg, nvgRGBA(0, 0, 255, 255));
	nvgFill(vg);
	nvgEndFrame(vg);

	c.check(buf.px(4, 5)[3] == 255 && buf.px(23, 5)[3] == 255, "both contours fill");
	c.check(buf.px(13, 5)[3] == 0, "gap between contours stays empty");
	c.check(buf.px(0, 0)[3] == 0, "outside stays empty");
	nvgDeleteCPU(vg);
}

void fillsLinearGradient(Case& c)
{
	Buffer buf(20, 6);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), buf.w, buf.h, buf.stride);
	nvgBeginFrame(vg, 20, 6, 1.0f);
	NVGpaint paint = nvgLinearGradient(vg, 2, 0, 18, 0,
									  nvgRGBA(255, 0, 0, 255), nvgRGBA(0, 0, 255, 255));
	nvgBeginPath(vg);
	nvgRect(vg, 2, 1, 16, 4);
	nvgFillPaint(vg, paint);
	nvgFill(vg);
	nvgEndFrame(vg);

	const unsigned char* left = buf.px(2, 3);
	const unsigned char* right = buf.px(17, 3);
	c.check(left[0] > 200 && left[2] < 60, "gradient start color");
	c.check(right[2] > 200 && right[0] < 60, "gradient end color");

	// Translucent premultiplied gradient: premultiplied bytes never exceed
	// the alpha byte.
	Buffer buf2(20, 6);
	NVGcontext* vg2 = nvgCreateCPU(buf2.bytes.data(), 20, 6, 20*4);
	nvgBeginFrame(vg2, 20, 6, 1.0f);
	NVGpaint p2 = nvgLinearGradient(vg2, 2, 0, 18, 0,
								   nvgRGBA(255, 255, 255, 128), nvgRGBA(0, 0, 0, 128));
	nvgBeginPath(vg2); nvgRect(vg2, 2, 1, 16, 4); nvgFillPaint(vg2, p2); nvgFill(vg2);
	nvgEndFrame(vg2);
	const unsigned char* q = buf2.px(3, 3);
	c.check(q[3] > 0 && q[0] <= q[3] && q[1] <= q[3] && q[2] <= q[3],
			"premultiplied gradient bytes");
	nvgDeleteCPU(vg);
	nvgDeleteCPU(vg2);
}

void fillsBoxGradient(Case& c)
{
	// Rounded box gradient with a feather band: solid inner color well inside
	// the box, a partial band across the edge, and the outer color beyond it.
	Buffer buf(28, 20);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 28, 20, 28*4);
	nvgBeginFrame(vg, 28, 20, 1);
	NVGpaint paint = nvgBoxGradient(vg, 4, 4, 16, 12, 2, 4,
								   nvgRGBA(255, 255, 255, 255),
								   nvgRGBA(0, 0, 0, 0));
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 28, 20);
	nvgFillPaint(vg, paint);
	nvgFill(vg);
	nvgEndFrame(vg);

	c.check(buf.px(12, 10)[3] == 255, "box gradient interior is the inner color");
	c.check(buf.px(26, 10)[3] == 0, "box gradient far exterior is the outer color");
	int band = 0;
	for (int x = 4; x < 28; x++) {
		unsigned char a = buf.px(x, 10)[3];
		if (a > 0 && a < 255) band++;
	}
	c.check(band >= 3, "box gradient feather band is gradual");
	nvgDeleteCPU(vg);
}

void fillsRotatedScissor(Case& c)
{
	Buffer buf(30, 30);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 30, 30, 30*4);
	const float angle = 3.14159265f / 4.0f;
	nvgBeginFrame(vg, 30, 30, 1.0f);
	nvgTranslate(vg, 15, 15);
	nvgRotate(vg, angle);
	nvgScissor(vg, -8, -4, 16, 8);
	nvgBeginPath(vg);
	nvgRect(vg, -15, -15, 30, 30);
	nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
	nvgFill(vg);
	nvgEndFrame(vg);

	int insideOk = 0, outsideOk = 0;
	for (int y = 0; y < 30; y++) {
		for (int x = 0; x < 30; x++) {
			bool ref = refInsideRect(x + 0.5f, y + 0.5f, 15, 15, angle, 8.0f, 4.0f);
			bool got = buf.px(x, y)[3] > 0;
			if (ref) insideOk += got;
			else outsideOk += !got;
		}
	}
	c.check(insideOk > 30, "rotated scissor interior painted");
	c.check(outsideOk > 700, "rotated scissor exterior hard clipped");
	nvgDeleteCPU(vg);
}

// ---------------------------------------------------------------------------
// strokes
// ---------------------------------------------------------------------------

void strokesJoinUnion(Case& c)
{
	// Translucent polyline: overlapping join geometry must not darken because
	// one renderStroke call merges coverage as a union.
	Buffer buf(40, 20);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 40, 20, 40*4);
	nvgBeginFrame(vg, 40, 20, 1.0f);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 4, 10);
	nvgLineTo(vg, 20, 10);
	nvgLineTo(vg, 36, 10);
	nvgStrokeWidth(vg, 3);
	nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 128));
	nvgStroke(vg);
	nvgEndFrame(vg);

	int midA = buf.px(10, 10)[3];
	int joinA = buf.px(20, 10)[3];
	c.check(midA > 20 && midA < 200, "translucent coverage visible");
	c.check(std::abs(midA - joinA) <= 3, "join does not double-cover");
	c.check(buf.px(10, 2)[3] == 0, "stroke has finite width");
	nvgDeleteCPU(vg);
}

void strokesSelfIntersectUnion(Case& c)
{
	Buffer buf(30, 30);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 30, 30, 30*4);
	nvgBeginFrame(vg, 30, 30, 1.0f);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 6, 6);
	nvgLineTo(vg, 24, 24);
	nvgLineTo(vg, 24, 6);
	nvgLineTo(vg, 6, 24);
	nvgStrokeWidth(vg, 2);
	nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 128));
	nvgStroke(vg);
	nvgEndFrame(vg);

	// Crossing pixel coverage must match the single-stroke alpha level, not
	// two layers blended on top of each other.
	Buffer ref(30, 30);
	NVGcontext* vg2 = nvgCreateCPU(ref.bytes.data(), 30, 30, 30*4);
	nvgBeginFrame(vg2, 30, 30, 1.0f);
	nvgBeginPath(vg2);
	nvgMoveTo(vg2, 6, 6);
	nvgLineTo(vg2, 24, 24);
	nvgStrokeWidth(vg2, 2);
	nvgStrokeColor(vg2, nvgRGBA(255, 255, 255, 128));
	nvgStroke(vg2);
	nvgEndFrame(vg2);

	c.check(buf.px(15, 15)[3] > 0, "crossing is painted");
	c.check(std::abs((int)buf.px(15, 15)[3] - (int)ref.px(10, 10)[3]) <= 3,
			"crossing equals single segment coverage");
	nvgDeleteCPU(vg);
	nvgDeleteCPU(vg2);
}

void strokesThinAlpha(Case& c)
{
	// Width below one pixel: the core already folds width/fringe into the
	// paint alpha (alpha*alpha) and shrinks the geometry. The backend must
	// not scale the alpha a second time. Coverage is measured independently
	// from an opaque stroke of the same geometry.
	const unsigned char paintA = 200;
	Buffer cov(40, 8), buf(40, 8);
	NVGcontext* vgCov = nvgCreateCPU(cov.bytes.data(), 40, 8, 40*4);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 40, 8, 40*4);

	auto drawLine = [](NVGcontext* v, unsigned char a) {
		nvgBeginFrame(v, 40, 8, 1.0f);
		nvgBeginPath(v);
		nvgMoveTo(v, 2, 4); nvgLineTo(v, 38, 4);
		nvgStrokeWidth(v, 0.5f);
		nvgStrokeColor(v, nvgRGBA(255, 255, 255, a));
		nvgStroke(v);
		nvgEndFrame(v);
	};
	drawLine(vgCov, 255);
	drawLine(vg, paintA);

	int covered = 0, matched = 0;
	for (int x = 4; x < 36; x++) {
		for (int y = 2; y < 7; y++) {
			int covA = cov.px(x, y)[3];
			if (covA == 0) continue;
			covered++;
			// The opaque reference already includes the core's width^2 alpha
			// factor, so the translucent result differs only by its paint
			// alpha: any extra backend scaling would break this equality.
			float coverage = covA / 255.0f;
			unsigned char expect = roundByte(coverage * (paintA/255.0f));
			if (nearByte(buf.px(x, y)[3], expect, 2)) matched++;
		}
	}
	c.check(covered > 30, "thin stroke geometry covers a measurable band");
	c.check(matched == covered, "thin stroke alpha applied exactly once");
	nvgDeleteCPU(vg);
	nvgDeleteCPU(vgCov);
}

void strokesSeparateCallsOrder(Case& c)
{
	// Two stroke calls blend in submission order, each as its own layer.
	Buffer buf(20, 10);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 20, 10, 20*4);
	nvgBeginFrame(vg, 20, 10, 1.0f);
	nvgBeginPath(vg); nvgMoveTo(vg, 1, 5); nvgLineTo(vg, 19, 5);
	nvgStrokeWidth(vg, 3);
	nvgStrokeColor(vg, nvgRGBA(255, 0, 0, 255));
	nvgStroke(vg);
	nvgBeginPath(vg); nvgMoveTo(vg, 1, 5); nvgLineTo(vg, 19, 5);
	nvgStrokeWidth(vg, 3);
	nvgStrokeColor(vg, nvgRGBA(0, 0, 255, 128));
	nvgStroke(vg);
	nvgEndFrame(vg);

	const unsigned char* p = buf.px(10, 5);
	// Source-over: red premultiplied 1, then blue at alpha 0.5.
	c.check(p[2] > p[0], "second stroke blends over the first");
	c.check(p[0] > 40, "first stroke still contributes");
	nvgDeleteCPU(vg);
}

// ---------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------

void texturesAsymmetricNearest(Case& c)
{
	Image img(3, 2);
	img.set(0,0,255,0,0,255);   img.set(1,0,0,255,0,255);  img.set(2,0,0,0,255,255);
	img.set(0,1,255,255,0,255); img.set(1,1,255,0,255,255);img.set(2,2-1,0,255,255,255);
	Buffer buf(16, 10);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 16, 10, 16*4);
	int id = nvgCreateImageRGBA(vg, 3, 2, NVG_IMAGE_NEAREST, img.data());
	c.check(id > 0, "image created");
	nvgBeginFrame(vg, 16, 10, 1.0f);
	nvgBeginPath(vg); nvgRect(vg, 2, 3, 3, 2);
	nvgFillPaint(vg, nvgImagePattern(vg, 2, 3, 3, 2, 0, id, 1.0f));
	nvgFill(vg);
	nvgEndFrame(vg);

	struct Expect { int x,y; unsigned char r,g,b; };
	Expect expects[6] = {
		{2,3,255,0,0},{3,3,0,255,0},{4,3,0,0,255},
		{2,4,255,255,0},{3,4,255,0,255},{4,4,0,255,255},
	};
	for (const auto& e : expects) {
		const unsigned char* p = buf.px(e.x, e.y);
		c.check(p[0]==e.r && p[1]==e.g && p[2]==e.b && p[3]==255,
				"nearest asymmetric texel");
	}
	c.check(buf.px(8, 8)[3] == 0, "image confined to pattern rect");
	nvgDeleteCPU(vg);
}

void texturesStraightVsPremult(Case& c)
{
	// Straight alpha texture (no flag) and an equivalent premultiplied copy
	// must produce identical pixels.
	Image straight(1, 1);
	straight.set(0, 0, 255, 0, 0, 128);
	Image premul(1, 1);
	premul.set(0, 0, 128, 0, 0, 128);

	Buffer b1(4, 4), b2(4, 4);
	NVGcontext* v1 = nvgCreateCPU(b1.bytes.data(), 4, 4, 4*4);
	NVGcontext* v2 = nvgCreateCPU(b2.bytes.data(), 4, 4, 4*4);
	int i1 = nvgCreateImageRGBA(v1, 1, 1, NVG_IMAGE_NEAREST, straight.data());
	int i2 = nvgCreateImageRGBA(v2, 1, 1, NVG_IMAGE_NEAREST | NVG_IMAGE_PREMULTIPLIED,
								premul.data());
	nvgBeginFrame(v1, 4, 4, 1);
	nvgBeginPath(v1); nvgRect(v1, 0, 0, 4, 4);
	nvgFillPaint(v1, nvgImagePattern(v1, 0, 0, 4, 4, 0, i1, 1.0f));
	nvgFill(v1);
	nvgEndFrame(v1);
	nvgBeginFrame(v2, 4, 4, 1);
	nvgBeginPath(v2); nvgRect(v2, 0, 0, 4, 4);
	nvgFillPaint(v2, nvgImagePattern(v2, 0, 0, 4, 4, 0, i2, 1.0f));
	nvgFill(v2);
	nvgEndFrame(v2);

	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			for (int k = 0; k < 4; k++)
				c.check(b1.px(x,y)[k] == b2.px(x,y)[k],
						"straight and premultiplied agree");
	c.check(b1.px(1,1)[0] == 128 && b1.px(1,1)[3] == 128, "premultiplied on load");
	nvgDeleteCPU(v1);
	nvgDeleteCPU(v2);
}

void texturesBilinear(Case& c)
{
	Image img(2, 1);
	img.set(0,0,0,0,0,255);
	img.set(1,0,255,255,255,255);
	Buffer buf(10, 3);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 10, 3, 10*4);
	int id = nvgCreateImageRGBA(vg, 2, 1, 0, img.data()); // bilinear, clamp
	nvgBeginFrame(vg, 10, 3, 1);
	nvgBeginPath(vg); nvgRect(vg, 1, 1, 4, 1);
	nvgFillPaint(vg, nvgImagePattern(vg, 1, 1, 4, 1, 0, id, 1.0f));
	nvgFill(vg);
	nvgEndFrame(vg);

	// Independent texel-center bilinear reference, averaged over the same
	// 4x4 sub-pixel grid the backend uses. Written separately so it shares no
	// code with the rasterizer.
	auto refBilinear = [&](int px, int py) {
		double acc = 0.0;
		for (int sy = 0; sy < SUB; sy++) {
			for (int sx = 0; sx < SUB; sx++) {
				float fxpx = px + (sx + 0.5f) / SUB;
				float fypx = py + (sy + 0.5f) / SUB;
				float u = std::fmin(std::fmax((fxpx - 1.0f) / 4.0f, 0.0f), 1.0f);
				float v = std::fmin(std::fmax((fypx - 1.0f) / 1.0f, 0.0f), 1.0f);
				(void)v;
				float g = u * 2.0f - 0.5f;
				int x0 = (int)std::floor(g);
				float f = g - x0;
				int i0 = std::max(0, std::min(1, x0));
				int i1 = std::max(0, std::min(1, x0+1));
				float v0 = i0 == 0 ? 0.0f : 255.0f;
				float v1 = i1 == 0 ? 0.0f : 255.0f;
				acc += v0 * (1.0f - f) + v1 * f;
			}
		}
		return roundByte((float)(acc / (SUB*SUB)) / 255.0f);
	};
	bool sawIntermediate = false;
	for (int x = 1; x <= 4; x++) {
		unsigned char expect = refBilinear(x, 1);
		c.check(std::abs((int)buf.px(x,1)[0] - (int)expect) <= 1,
				"bilinear texel-center subpixel average");
		if (expect != 0 && expect != 255) sawIntermediate = true;
	}
	c.check(sawIntermediate, "bilinear produces filtered intermediate values");
	nvgDeleteCPU(vg);
}

void texturesRepeat(Case& c)
{
	Image img(2, 1);
	img.set(0,0,255,0,0,255);
	img.set(1,0,0,255,0,255);
	Buffer buf(10, 3);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 10, 3, 10*4);
	int id = nvgCreateImageRGBA(vg, 2, 1, NVG_IMAGE_NEAREST | NVG_IMAGE_REPEATX,
								img.data());
	nvgBeginFrame(vg, 10, 3, 1);
	nvgBeginPath(vg); nvgRect(vg, 1, 1, 6, 1);
	nvgFillPaint(vg, nvgImagePattern(vg, 1, 1, 6, 1, 0, id, 1.0f));
	nvgFill(vg);
	nvgEndFrame(vg);

	// Nearest columns over a 6-wide tile of a 2-wide image: three pixels per
	// texel, so R R R G G G.
	for (int i = 0; i < 6; i++) {
		const unsigned char* p = buf.px(1+i, 1);
		if (i < 3)
			c.check(p[0] == 255 && p[1] == 0, "repeat X red column");
		else
			c.check(p[0] == 0 && p[1] == 255, "repeat X green column");
	}
	nvgDeleteCPU(vg);
}

void texturesRepeatY(Case& c)
{
	// Non-power-of-two texture (3x2) repeating on both axes.
	Image img(3, 2);
	img.set(0,0,255,0,0,255);  img.set(1,0,0,255,0,255);  img.set(2,0,0,0,255,255);
	img.set(0,1,255,255,0,255);img.set(1,1,255,0,255,255);img.set(2,1,0,255,255,255);
	Buffer buf(12, 8);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 12, 8, 12*4);
	int id = nvgCreateImageRGBA(
		vg, 3, 2, NVG_IMAGE_NEAREST | NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY,
		img.data());
	nvgBeginFrame(vg, 12, 8, 1);
	nvgBeginPath(vg); nvgRect(vg, 1, 1, 9, 6);
	nvgFillPaint(vg, nvgImagePattern(vg, 1, 1, 9, 6, 0, id, 1.0f));
	nvgFill(vg);
	nvgEndFrame(vg);

	auto expectTexel = [&](int tx, int ty) {
		tx = ((tx % 3) + 3) % 3;
		ty = ((ty % 2) + 2) % 2;
		return img.rgba.data() + (ty*3 + tx)*4;
	};
	bool ok = true;
	for (int y = 1; y < 7; y++) {
		for (int x = 1; x < 10; x++) {
			int tx = (int)std::floor(((x + 0.5f) - 1.0f) / 3.0f);
			int ty = (int)std::floor(((y + 0.5f) - 1.0f) / 3.0f);
			const unsigned char* e = expectTexel(tx, ty);
			const unsigned char* got = buf.px(x, y);
			for (int k = 0; k < 4; k++) ok &= got[k] == e[k];
		}
	}
	c.check(ok, "NPOT repeat on X and Y wraps independently");
	nvgDeleteCPU(vg);
}

void texturesFlipY(Case& c)
{
	Image img(1, 2);
	img.set(0, 0, 255, 0, 0, 255); // top row red
	img.set(0, 1, 0, 255, 0, 255); // bottom row green
	Buffer normal(4, 6), flipped(4, 6);
	NVGcontext* v1 = nvgCreateCPU(normal.bytes.data(), 4, 6, 4*4);
	NVGcontext* v2 = nvgCreateCPU(flipped.bytes.data(), 4, 6, 4*4);
	int i1 = nvgCreateImageRGBA(v1, 1, 2, NVG_IMAGE_NEAREST, img.data());
	int i2 = nvgCreateImageRGBA(v2, 1, 2, NVG_IMAGE_NEAREST | NVG_IMAGE_FLIPY,
								img.data());
	for (int pass = 0; pass < 2; pass++) {
		NVGcontext* vg = pass == 0 ? v1 : v2;
		Buffer& b = pass == 0 ? normal : flipped;
		int id = pass == 0 ? i1 : i2;
		nvgBeginFrame(vg, 4, 6, 1);
		nvgBeginPath(vg); nvgRect(vg, 1, 1, 1, 2);
		nvgFillPaint(vg, nvgImagePattern(vg, 1, 1, 1, 2, 0, id, 1.0f));
		nvgFill(vg);
		nvgEndFrame(vg);
		(void)b;
	}
	c.check(normal.px(1,1)[0] == 255, "no flip samples top row");
	c.check(flipped.px(1,1)[1] == 255, "flip Y samples bottom row at top");
	nvgDeleteCPU(v1);
	nvgDeleteCPU(v2);
}

void texturesFontTriangles(Case& c)
{
	// Glyphs arrive through renderTriangles with an alpha atlas. White tint
	// gives gray pixels where r == g == b == a, with several coverage levels
	// (bilinear atlas sampling plus barycentric UV interpolation).
	Buffer buf(48, 24);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 48, 24, 48*4);
	int font = nvgCreateFont(vg, "roboto", "example/Roboto-Regular.ttf");
	c.check(font >= 0, "font loaded");
	nvgBeginFrame(vg, 48, 24, 1);
	nvgFontFaceId(vg, font);
	nvgFontSize(vg, 18);
	nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
	nvgText(vg, 2, 18, "AV", nullptr);
	nvgEndFrame(vg);

	int levels = 0;
	bool gray = true;
	for (int y = 2; y < 22; y++) {
		for (int x = 2; x < 46; x++) {
			const unsigned char* p = buf.px(x, y);
			if (p[3] == 0) continue;
			if (!(p[0] == p[1] && p[1] == p[2] && p[2] == p[3])) gray = false;
		}
	}
	for (int v = 1; v < 255; v += 16) {
		for (int y = 2; y < 22; y++)
			for (int x = 2; x < 46; x++)
				if (std::abs((int)buf.px(x,y)[3] - v) <= 8) { levels++; goto next; }
	next:;
	}
	c.check(gray, "alpha atlas expands to four-channel gray coverage");
	c.check(levels >= 6, "glyph coverage has multiple filtered levels");

	// The crossbar band is solid glyph interior: the half-open edge rule must
	// leave no zero-coverage seam between the tessellated triangles.
	Buffer big(60, 40);
	NVGcontext* vg2 = nvgCreateCPU(big.bytes.data(), 60, 40, 60*4);
	int f2 = nvgCreateFont(vg2, "roboto", "example/Roboto-Regular.ttf");
	nvgBeginFrame(vg2, 60, 40, 1);
	nvgFontFaceId(vg2, f2);
	nvgFontSize(vg2, 32);
	nvgFillColor(vg2, nvgRGBA(255,255,255,255));
	nvgText(vg2, 4, 32, "A", nullptr);
	nvgEndFrame(vg2);
	int strong = 0, gaps = 0, minA = 255;
	for (int x = 12; x <= 16; x++) {
		unsigned char a = big.px(x, 24)[3];
		if (a == 0) gaps++;
		if (a < minA) minA = a;
		if (a >= 200) strong++;
	}
	c.check(gaps == 0, "no triangle-seam gap inside glyph crossbar");
	c.check(minA >= 200 && strong == 5, "glyph crossbar strongly covered");
	// The A counter stays empty, proving holes survive the triangle path.
	c.check(big.px(14, 19)[3] == 0, "glyph counter (hole) preserved");
	nvgDeleteCPU(vg);
	nvgDeleteCPU(vg2);
}

void texturesUpdateAndDelete(Case& c)
{
	Image red(2, 2);
	for (int i = 0; i < 4; i++) red.set(i%2, i/2, 255, 0, 0, 255);
	Buffer buf(32, 12);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 32, 12, 32*4);
	int id = nvgCreateImageRGBA(vg, 2, 2, NVG_IMAGE_NEAREST, red.data());

	nvgBeginFrame(vg, 32, 12, 1);
	// Recorded first, then the texture is updated; the first draw keeps the
	// content it had when it was rasterized.
	nvgBeginPath(vg); nvgRect(vg, 1, 2, 8, 8);
	nvgFillPaint(vg, nvgImagePattern(vg, 1, 2, 8, 8, 0, id, 1.0f));
	nvgFill(vg);
	Image green(2, 2);
	for (int i = 0; i < 4; i++) green.set(i%2, i/2, 0, 255, 0, 255);
	nvgUpdateImage(vg, id, green.data());
	nvgBeginPath(vg); nvgRect(vg, 12, 2, 8, 8);
	nvgFillPaint(vg, nvgImagePattern(vg, 12, 2, 8, 8, 0, id, 1.0f));
	nvgFill(vg);
	nvgEndFrame(vg);
	c.check(buf.px(2,5)[0] == 255 && buf.px(2,5)[1] == 0, "recorded draw snapshot");
	c.check(buf.px(13,5)[1] == 255 && buf.px(13,5)[0] == 0, "later draw reads update");

	// NULL create data is a zeroed texture, updated through a sub-rectangle.
	int zid = nvgCreateImageRGBA(vg, 3, 2, NVG_IMAGE_NEAREST, nullptr);
	Image patch(3, 2);
	patch.set(2, 1, 17, 34, 51, 255);
	nvgBeginFrame(vg, 32, 12, 1);
	nvgUpdateImage(vg, zid, patch.data());
	nvgBeginPath(vg); nvgRect(vg, 20, 2, 3, 2);
	nvgFillPaint(vg, nvgImagePattern(vg, 20, 2, 3, 2, 0, zid, 1.0f));
	nvgFill(vg);
	nvgEndFrame(vg);
	c.check(buf.px(20,2)[3] == 0, "untouched texels stay zero");
	c.check(buf.px(22,3)[0] == 17 && buf.px(22,3)[1] == 34 &&
			buf.px(22,3)[2] == 51, "sub-rectangle update");

	// Deleting an image cannot corrupt earlier draws; new draws referencing it
	// are skipped.
	nvgDeleteImage(vg, id);
	nvgBeginFrame(vg, 32, 12, 1);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 32, 12);
	nvgFillPaint(vg, nvgImagePattern(vg, 0, 0, 8, 8, 0, id, 1.0f));
	nvgFill(vg);
	nvgEndFrame(vg);
	c.check(buf.px(2,5)[0] == 255, "earlier draw survives texture delete");
	c.check(buf.px(30,10)[3] == 0, "draw with deleted image is skipped");
	nvgDeleteCPU(vg);
}

void texturesTwoContexts(Case& c)
{
	Image red(2, 2), green(2, 2);
	for (int i = 0; i < 4; i++) { red.set(i%2,i/2,255,0,0,255); green.set(i%2,i/2,0,255,0,255); }
	Buffer b1(12, 12), b2(12, 12);
	NVGcontext* v1 = nvgCreateCPU(b1.bytes.data(), 12, 12, 12*4);
	NVGcontext* v2 = nvgCreateCPU(b2.bytes.data(), 12, 12, 12*4);
	int id1 = nvgCreateImageRGBA(v1, 2, 2, NVG_IMAGE_NEAREST, red.data());
	int id2 = nvgCreateImageRGBA(v2, 2, 2, NVG_IMAGE_NEAREST, green.data());
	c.check(id1 == id2, "handle ids are per-context");

	nvgBeginFrame(v1, 12, 12, 1);
	nvgBeginFrame(v2, 12, 12, 1);
	nvgBeginPath(v1); nvgRect(v1, 0, 0, 8, 8);
	nvgFillPaint(v1, nvgImagePattern(v1, 0, 0, 8, 8, 0, id1, 1.0f));
	nvgFill(v1);
	nvgBeginPath(v2); nvgRect(v2, 0, 0, 8, 8);
	nvgFillPaint(v2, nvgImagePattern(v2, 0, 0, 8, 8, 0, id2, 1.0f));
	nvgFill(v2);
	nvgEndFrame(v1);
	nvgEndFrame(v2);
	c.check(b1.px(2,2)[0] == 255 && b1.px(2,2)[1] == 0, "context one texture");
	c.check(b2.px(2,2)[1] == 255 && b2.px(2,2)[0] == 0, "context two texture");
	nvgDeleteCPU(v1);
	nvgDeleteCPU(v2);
}

// ---------------------------------------------------------------------------
// frames
// ---------------------------------------------------------------------------

void framesInvalidArgs(Case& c)
{
	Buffer buf(8, 8);
	c.check(nvgCreateCPU(nullptr, 8, 8, 32) == nullptr, "NULL buffer rejected");
	c.check(nvgCreateCPU(buf.bytes.data(), 0, 8, 32) == nullptr, "zero width rejected");
	c.check(nvgCreateCPU(buf.bytes.data(), 8, -1, 32) == nullptr, "negative height rejected");
	c.check(nvgCreateCPU(buf.bytes.data(), 8, 8, 31) == nullptr, "short stride rejected");
	c.check(nvgCreateCPU(buf.bytes.data(), 8, 8, 32) != nullptr, "valid args accepted");
	nvgDeleteCPU(nvgCreateCPU(buf.bytes.data(), 8, 8, 32));
}

void framesCancel(Case& c)
{
	Buffer buf(24, 16);
	for (auto& b : buf.bytes) b = 0x7B;
	std::vector<unsigned char> saved = buf.bytes;
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 24, 16, buf.stride);
	nvgBeginFrame(vg, 24, 16, 1);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 24, 16);
	nvgFillColor(vg, nvgRGBA(10, 20, 30, 255));
	nvgFill(vg);
	// Before EndFrame the borrowed target is untouched.
	c.check(buf.bytes == saved, "target untouched before EndFrame");
	nvgCancelFrame(vg);
	c.check(buf.bytes == saved, "CancelFrame restores previous contents");

	// A fresh frame after cancel renders normally.
	nvgBeginFrame(vg, 24, 16, 1);
	nvgBeginPath(vg); nvgRect(vg, 1, 1, 4, 4);
	nvgFillColor(vg, nvgRGBA(255,255,255,255));
	nvgFill(vg);
	nvgEndFrame(vg);
	c.check(buf.px(2,2)[3] == 255, "frame after cancel renders");
	c.check(buf.px(20,14)[3] == 0x7B, "undrawn region keeps prior bytes");
	nvgDeleteCPU(vg);
}

void framesRepeat(Case& c)
{
	Buffer buf(20, 10);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 20, 10, 20*4);
	nvgBeginFrame(vg, 20, 10, 1);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 10, 10);
	nvgFillColor(vg, nvgRGBA(255, 0, 0, 255));
	nvgFill(vg);
	nvgEndFrame(vg);

	nvgBeginFrame(vg, 20, 10, 1);
	nvgBeginPath(vg); nvgRect(vg, 10, 0, 10, 10);
	nvgFillColor(vg, nvgRGBA(0, 0, 255, 255));
	nvgFill(vg);
	nvgEndFrame(vg);
	c.check(buf.px(3,5)[0] == 255 && buf.px(3,5)[2] == 0, "frame one content retained");
	c.check(buf.px(15,5)[2] == 255 && buf.px(15,5)[0] == 0, "frame two published");

	// Third frame blends on top of the latest target contents.
	nvgBeginFrame(vg, 20, 10, 1);
	nvgBeginPath(vg); nvgRect(vg, 0, 0, 10, 10);
	nvgFillColor(vg, nvgRGBA(255, 255, 255, 128));
	nvgFill(vg);
	nvgEndFrame(vg);
	const unsigned char* p = buf.px(3, 5);
	c.check(p[0] == 255 && p[1] > 100 && p[3] == 255, "blend over accumulated target");
	nvgDeleteCPU(vg);
}

void framesRowTailSentinel(Case& c)
{
	const int extra = 7, tail = 11;
	Buffer buf(13, 9, extra, tail);
	for (auto& b : buf.bytes) b = 0xC3;
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 13, 9, buf.stride);
	for (int frame = 0; frame < 3; frame++) {
		nvgBeginFrame(vg, 13, 9, 1);
		nvgBeginPath(vg); nvgRect(vg, 0, 0, 13, 9);
		nvgFillColor(vg, nvgRGBA(frame*40, 10, 200, 200));
		nvgFill(vg);
		nvgEndFrame(vg);
	}
	bool paddingIntact = true;
	for (int y = 0; y < 9; y++)
		for (int k = 13*4; k < buf.stride; k++)
			if (buf.bytes[(size_t)y*buf.stride + k] != 0xC3) paddingIntact = false;
	bool tailIntact = true;
	for (size_t k = (size_t)9*buf.stride; k < buf.bytes.size(); k++)
		if (buf.bytes[k] != 0xC3) tailIntact = false;
	c.check(paddingIntact, "row padding never written");
	c.check(tailIntact, "bytes beyond the target never written");
	nvgDeleteCPU(vg);
}

void framesBlendFactors(Case& c)
{
	// Every documented factor is exercised on the RGB channel and again on the
	// alpha channel, each against the independent float equation reference.
	const int factors[] = {
		NVG_ZERO, NVG_ONE, NVG_SRC_COLOR, NVG_ONE_MINUS_SRC_COLOR,
		NVG_DST_COLOR, NVG_ONE_MINUS_DST_COLOR, NVG_SRC_ALPHA,
		NVG_ONE_MINUS_SRC_ALPHA, NVG_DST_ALPHA, NVG_ONE_MINUS_DST_ALPHA,
		NVG_SRC_ALPHA_SATURATE
	};
	auto runTrial = [&](int srgb, int drgb, int salpha, int dalpha) {
		Buffer buf(6, 6);
		NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 6, 6, 6*4);
		nvgBeginFrame(vg, 6, 6, 1);
		nvgBeginPath(vg); nvgRect(vg, 0, 0, 6, 6);
		nvgFillColor(vg, nvgRGBA(40, 90, 160, 255));
		nvgFill(vg);
		nvgEndFrame(vg);
		std::vector<unsigned char> dst = buf.bytes;

		nvgBeginFrame(vg, 6, 6, 1);
		nvgGlobalCompositeBlendFuncSeparate(vg, srgb, drgb, salpha, dalpha);
		nvgBeginPath(vg); nvgRect(vg, 1, 1, 4, 4);
		nvgFillColor(vg, nvgRGBA(220, 30, 90, 130));
		nvgFill(vg);
		nvgEndFrame(vg);

		NVGcompositeOperationState op;
		op.srcRGB = srgb; op.dstRGB = drgb;
		op.srcAlpha = salpha; op.dstAlpha = dalpha;
		float src[4] = {220.0f/255.0f * (130.0f/255.0f),
						30.0f/255.0f * (130.0f/255.0f),
						90.0f/255.0f * (130.0f/255.0f),
						130.0f/255.0f};
		for (int y = 1; y < 5; y++) {
			for (int x = 1; x < 5; x++) {
				unsigned char e[4];
				refBlend(dst.data() + (y*6 + x)*4, src, op, e);
				const unsigned char* got = buf.px(x, y);
				for (int k = 0; k < 4; k++)
					c.check(std::abs((int)got[k] - (int)e[k]) <= 1,
							"separate blend factor channel");
			}
		}
		c.check(buf.px(0,0)[0] == 40, "blend confined to geometry");
		nvgDeleteCPU(vg);
	};
	for (int f : factors) {
		runTrial(f, NVG_ZERO, NVG_ONE, NVG_ZERO);          // factor on RGB
		runTrial(NVG_ONE, NVG_ZERO, f, NVG_ZERO);          // factor on alpha
	}
	// A few realistic named composites end to end.
	runTrial(NVG_ONE, NVG_ONE, NVG_ONE, NVG_ONE);                       // LIGHTER
	runTrial(NVG_ONE, NVG_ZERO, NVG_ONE, NVG_ZERO);                     // COPY
	runTrial(NVG_ONE_MINUS_DST_ALPHA, NVG_ONE_MINUS_SRC_ALPHA,
			 NVG_ONE_MINUS_DST_ALPHA, NVG_ONE_MINUS_SRC_ALPHA);         // XOR
}

void framesDprAndScissor(Case& c)
{
	// Physical 24x20, logical 12x10 at DPR 2. The caller scales geometry by
	// the DPR itself; the backend must not multiply by it a second time.
	Buffer buf(24, 20);
	NVGcontext* vg = nvgCreateCPU(buf.bytes.data(), 24, 20, 24*4);
	nvgBeginFrame(vg, 12, 10, 2.0f);
	nvgScale(vg, 2.0f, 2.0f);
	nvgBeginPath(vg); nvgRect(vg, 1, 1, 4, 4);
	nvgFillColor(vg, nvgRGBA(255,255,255,255));
	nvgFill(vg);
	nvgEndFrame(vg);
	c.check(buf.px(1,1)[3] == 0, "DPR not applied twice (outer)");
	c.check(buf.px(3,3)[3] == 255, "DPR-scaled geometry maps 1:1 (inner)");
	c.check(buf.px(11,11)[3] == 0, "outside rect empty");

	// Rotated scissor under the same DPR transform.
	Buffer buf2(24, 20);
	NVGcontext* vg2 = nvgCreateCPU(buf2.bytes.data(), 24, 20, 24*4);
	const float angle = 3.14159265f / 4.0f;
	nvgBeginFrame(vg2, 12, 10, 2.0f);
	nvgScale(vg2, 2.0f, 2.0f);
	nvgSave(vg2);
	nvgTranslate(vg2, 6, 5);
	nvgRotate(vg2, angle);
	nvgScissor(vg2, -3, -2, 6, 4); // logical half extents 3,2 -> device 6,4
	nvgBeginPath(vg2); nvgRect(vg2, -6, -5, 12, 10);
	nvgFillColor(vg2, nvgRGBA(255,255,255,255));
	nvgFill(vg2);
	nvgRestore(vg2);
	nvgEndFrame(vg2);
	int inside = 0, outside = 0;
	for (int y = 0; y < 20; y++) {
		for (int x = 0; x < 24; x++) {
			// Reference in device coordinates: center (12,10), half 6,4.
			bool ref = refInsideRect(x+0.5f, y+0.5f, 12, 10, angle, 6.0f, 4.0f);
			bool got = buf2.px(x,y)[3] > 0;
			if (ref) inside += got;
			else outside += !got;
		}
	}
	c.check(inside > 20 && outside > 350, "rotated scissor under DPR");
	nvgDeleteCPU(vg);
	nvgDeleteCPU(vg2);
}

} // namespace

void register_task_tests(Registry& r)
{
	r.add("fills", "rect_with_hole", fillsHole);
	r.add("fills", "self_intersect_nonzero", fillsSelfIntersect);
	r.add("fills", "slit_reverse_winding", fillsSlitHole);
	r.add("fills", "multiple_contours", fillsMultipleContours);
	r.add("fills", "linear_gradient", fillsLinearGradient);
	r.add("fills", "box_gradient", fillsBoxGradient);
	r.add("fills", "rotated_scissor", fillsRotatedScissor);

	r.add("strokes", "join_coverage_union", strokesJoinUnion);
	r.add("strokes", "self_intersect_union", strokesSelfIntersectUnion);
	r.add("strokes", "thin_stroke_alpha_once", strokesThinAlpha);
	r.add("strokes", "separate_calls_order", strokesSeparateCallsOrder);

	r.add("textures", "asymmetric_nearest", texturesAsymmetricNearest);
	r.add("textures", "straight_vs_premult", texturesStraightVsPremult);
	r.add("textures", "bilinear_centers", texturesBilinear);
	r.add("textures", "repeat_xy", texturesRepeat);
	r.add("textures", "repeat_npot_both_axes", texturesRepeatY);
	r.add("textures", "flip_y", texturesFlipY);
	r.add("textures", "font_alpha_triangles", texturesFontTriangles);
	r.add("textures", "update_delete_snapshot", texturesUpdateAndDelete);
	r.add("textures", "two_contexts_isolated", texturesTwoContexts);

	r.add("frames", "invalid_args", framesInvalidArgs);
	r.add("frames", "cancel_discards", framesCancel);
	r.add("frames", "repeat_frames", framesRepeat);
	r.add("frames", "row_tail_sentinel", framesRowTailSentinel);
	r.add("frames", "blend_factors", framesBlendFactors);
	r.add("frames", "dpr_rotated_scissor", framesDprAndScissor);
}
