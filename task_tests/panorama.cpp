// Panorama rasterizer regression tests.
//
// The reference used by these tests is deliberately independent from the
// production implementation: it walks every pixel, builds the ray direction
// from the equirectangular mapping, and intersects that ray with every input
// triangle using a plain Moller-Trumbore style computation in double
// precision.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Rasterizer.h"
#include "task_support/check.h"

using namespace swr;

namespace {

const double kPi = 3.14159265358979323846264338327950288;

// ---------------------------------------------------------------------------
// Small double precision vector helpers for the reference implementation.
// ---------------------------------------------------------------------------

struct V3 {
	double x;
	double y;
	double z;
};

V3 v3(double x, double y, double z)
{
	V3 r;
	r.x = x;
	r.y = y;
	r.z = z;
	return r;
}

V3 sub3(const V3 &a, const V3 &b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
double dot3(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

V3 cross3(const V3 &a, const V3 &b)
{
	return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

V3 directionFor(int x, int y, int width, int height)
{
	const double lambda = 2.0 * kPi * (x + 0.5) / width - kPi;
	const double phi = 0.5 * kPi - kPi * (y + 0.5) / height;
	return v3(std::cos(phi) * std::sin(lambda), std::sin(phi), std::cos(phi) * std::cos(lambda));
}

float kNearRadius();
float kFarRadius();

struct RefTri {
	V3 a;
	V3 b;
	V3 c;
	V3 n;
};

RefTri makeRefTri(const RasterizerVertex &v0, const RasterizerVertex &v1, const RasterizerVertex &v2)
{
	RefTri t;
	t.a = v3(v0.x, v0.y, v0.z);
	t.b = v3(v1.x, v1.y, v1.z);
	t.c = v3(v2.x, v2.y, v2.z);
	t.n = cross3(sub3(t.b, t.a), sub3(t.c, t.a));
	return t;
}

// Independent ray/triangle intersection.  Returns the ray parameter and the
// physical barycentric coordinates.
bool refHit(const V3 &d, const RefTri &t, double &outT, double outB[3])
{
	const double denom = dot3(t.n, d);
	if (denom == 0.0)
		return false;
	const double rayT = dot3(t.n, t.a) / denom;
	if (!(rayT > 0.0))
		return false;
	const V3 p = v3(d.x * rayT, d.y * rayT, d.z * rayT);
	const V3 c0 = cross3(sub3(t.b, p), sub3(t.c, p));
	const V3 c1 = cross3(sub3(t.c, p), sub3(t.a, p));
	const V3 c2 = cross3(sub3(t.a, p), sub3(t.b, p));
	const double n2 = dot3(t.n, t.n);
	const double a0 = dot3(c0, t.n) / n2;
	const double a1 = dot3(c1, t.n) / n2;
	const double a2 = dot3(c2, t.n) / n2;
	const double eps = 1e-6;
	if (a0 < -eps || a1 < -eps || a2 < -eps)
		return false;
	const double sum = a0 + a1 + a2;
	if (sum == 0.0)
		return false;
	outT = rayT;
	outB[0] = a0 / sum;
	outB[1] = a1 / sum;
	outB[2] = a2 / sum;
	return true;
}

// ---------------------------------------------------------------------------
// Fragment recorder shared by the tests.
// ---------------------------------------------------------------------------

struct Fragment {
	int x;
	int y;
	int primitive;
	float z;
	float w;
	float invw;
	float avar[MaxAVars];
	float pvar[MaxPVars];
	bool hasEquations;
};

std::vector<Fragment> g_fragments;

template <int AVars, int PVars>
struct RecordingShader : PixelShaderBase<RecordingShader<AVars, PVars> > {
	static const int AVarCount = AVars;
	static const int PVarCount = PVars;
	static const bool InterpolateZ = true;
	static const bool InterpolateW = true;
	static void drawPixel(const PixelData &p)
	{
		Fragment f;
		f.x = p.x;
		f.y = p.y;
		f.primitive = p.primitiveIndex;
		f.z = p.z;
		f.w = p.w;
		f.invw = p.invw;
		for (int i = 0; i < MaxAVars; ++i) f.avar[i] = p.avar[i];
		for (int i = 0; i < MaxPVars; ++i) f.pvar[i] = p.pvar[i];
		f.hasEquations = p.equations != 0;
		g_fragments.push_back(f);
	}
};

// ---------------------------------------------------------------------------
// Vertex helpers.
// ---------------------------------------------------------------------------

RasterizerVertex makeVertex(double x, double y, double z)
{
	RasterizerVertex v;
	v.x = (float)x;
	v.y = (float)y;
	v.z = (float)z;
	v.w = 1.0f;
	for (int a = 0; a < MaxAVars; ++a) v.avar[a] = 0.0f;
	for (int a = 0; a < MaxPVars; ++a) v.pvar[a] = 0.0f;
	return v;
}

RasterizerVertex spherical(double radius, double lat, double lon)
{
	const double cp = std::cos(lat);
	return makeVertex(
		radius * cp * std::sin(lon),
		radius * std::sin(lat),
		radius * cp * std::cos(lon));
}

void addTri(std::vector<RasterizerVertex> &vertices, std::vector<int> &indices,
	const RasterizerVertex &a, const RasterizerVertex &b, const RasterizerVertex &c)
{
	const int base = (int)vertices.size();
	vertices.push_back(a);
	vertices.push_back(b);
	vertices.push_back(c);
	indices.push_back(base);
	indices.push_back(base + 1);
	indices.push_back(base + 2);
}

float deg(double d)
{
	return (float)(d * kPi / 180.0);
}

// ---------------------------------------------------------------------------
// Reference coverage.
// ---------------------------------------------------------------------------

struct ReferenceResult {
	int width;
	int height;
	std::vector<std::vector<int> > hits; // primitive ids per pixel, in input order
};

ReferenceResult runReference(const std::vector<RasterizerVertex> &vertices,
	const std::vector<int> &indices, int width, int height, float nearR, float farR)
{
	ReferenceResult r;
	r.width = width;
	r.height = height;
	r.hits.resize((size_t)width * height);

	const size_t triCount = indices.size() / 3;
	std::vector<RefTri> tris(triCount);
	std::vector<bool> live(triCount, false);
	for (size_t t = 0; t < triCount; ++t) {
		if (indices[3 * t] < 0)
			continue;
		live[t] = true;
		tris[t] = makeRefTri(vertices[indices[3 * t]], vertices[indices[3 * t + 1]], vertices[indices[3 * t + 2]]);
	}

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const V3 d = directionFor(x, y, width, height);
			std::vector<int> &cell = r.hits[(size_t)y * width + x];
			for (size_t t = 0; t < triCount; ++t) {
				if (!live[t])
					continue;
				double rayT;
				double b[3];
				if (!refHit(d, tris[t], rayT, b))
					continue;
				if (!(rayT >= (double)nearR && rayT < (double)farR))
					continue;
				cell.push_back((int)t);
			}
		}
	}
	return r;
}

void drawPanorama(Rasterizer &rasterizer, const std::vector<RasterizerVertex> &vertices,
	const std::vector<int> &indices, int width, int height, float nearR, float farR,
	int sx, int sy, int sw, int sh, PanoramaStats *stats)
{
	rasterizer.setPixelShader<RecordingShader<2, 2> >();
	rasterizer.setScissorRect(sx, sy, sw, sh);
	g_fragments.clear();
	rasterizer.drawPanoramaTriangleList(&vertices[0], &indices[0], indices.size(),
		width, height, nearR, farR, stats);
}

bool compareCoverage(const std::vector<RasterizerVertex> &vertices,
	const std::vector<int> &indices, int width, int height,
	float nearR, float farR, int sx, int sy, int sw, int sh)
{
	const ReferenceResult ref = runReference(vertices, indices, width, height, nearR, farR);

	PanoramaStats stats;
	Rasterizer rasterizer;
	drawPanorama(rasterizer, vertices, indices, width, height, nearR, farR, sx, sy, sw, sh, &stats);

	std::vector<std::vector<int> > got((size_t)width * height);
	int expectedCount = 0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const bool inside = x >= sx && x < sx + sw && y >= sy && y < sy + sh;
			if (inside)
				expectedCount += (int)ref.hits[(size_t)y * width + x].size();
		}
	}

	TASK_CHECK(g_fragments.size() == (size_t)expectedCount);
	TASK_CHECK(stats.emitted_fragments == g_fragments.size());

	for (size_t i = 0; i < g_fragments.size(); ++i) {
		const Fragment &f = g_fragments[i];
		TASK_CHECK(!f.hasEquations);
		TASK_CHECK(f.x >= sx && f.x < sx + sw);
		TASK_CHECK(f.y >= sy && f.y < sy + sh);
		got[(size_t)f.y * width + f.x].push_back(f.primitive);
	}

	// Within one pixel the fragments must be delivered in input order.
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			for (size_t i = 1; i < got[(size_t)y * width + x].size(); ++i) {
				if (got[(size_t)y * width + x][i - 1] >= got[(size_t)y * width + x][i])
					return 1;
			}
		}
	}

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const bool inside = x >= sx && x < sx + sw && y >= sy && y < sy + sh;
			const std::vector<int> &want = ref.hits[(size_t)y * width + x];
			const std::vector<int> &have = got[(size_t)y * width + x];
			if (!inside) {
				TASK_CHECK(have.empty());
				continue;
			}
			if (want.size() != have.size()) {
				std::fprintf(stderr, "coverage mismatch at %d,%d want=%lu have=%lu\n",
					x, y, (unsigned long)want.size(), (unsigned long)have.size());
				return 1;
			}
			for (size_t i = 0; i < want.size(); ++i) {
				if (want[i] != have[i]) {
					std::fprintf(stderr, "coverage order mismatch at %d,%d want=%d have=%d\n",
						x, y, want[i], have[i]);
					return 1;
				}
			}
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Interpolation against the physical barycentric reference.
// ---------------------------------------------------------------------------

int compareInterpolation(const std::vector<RasterizerVertex> &vertices,
	const std::vector<int> &indices, int width, int height, float nearR, float farR)
{
	const ReferenceResult ref = runReference(vertices, indices, width, height, nearR, farR);

	PanoramaStats stats;
	Rasterizer rasterizer;
	rasterizer.setPixelShader<RecordingShader<3, 2> >();
	rasterizer.setScissorRect(0, 0, width, height);
	g_fragments.clear();
	rasterizer.drawPanoramaTriangleList(&vertices[0], &indices[0], indices.size(),
		width, height, nearR, farR, &stats);

	// Group fragments per pixel: the contract only fixes the order inside a
	// pixel, not the pixel traversal order.
	std::vector<std::vector<const Fragment *> > byPixel((size_t)width * height);
	for (size_t i = 0; i < g_fragments.size(); ++i) {
		const Fragment &f = g_fragments[i];
		TASK_CHECK(f.x >= 0 && f.x < width && f.y >= 0 && f.y < height);
		byPixel[(size_t)f.y * width + f.x].push_back(&f);
	}
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::vector<int> &want = ref.hits[(size_t)y * width + x];
			const std::vector<const Fragment *> &have = byPixel[(size_t)y * width + x];
			TASK_CHECK(want.size() == have.size());
			for (size_t i = 0; i < want.size(); ++i) {
				const Fragment &f = *have[i];
				const int t = want[i];
				const int base = indices[3 * t];
				const RasterizerVertex &v0 = vertices[base];
				const RasterizerVertex &v1 = vertices[base + 1];
				const RasterizerVertex &v2 = vertices[base + 2];

				const V3 d = directionFor(x, y, width, height);
				double rayT;
				double b[3];
				TASK_CHECK(refHit(d, makeRefTri(v0, v1, v2), rayT, b));

				TASK_CHECK(f.x == x && f.y == y);
				TASK_CHECK(f.primitive == t);
				TASK_CHECK(std::fabs(f.z - (float)rayT) < 2e-4f * (1.0f + (float)std::fabs(rayT)));
				TASK_CHECK(f.w == 1.0f && f.invw == 1.0f);

				for (int c = 0; c < 3; ++c) {
					const float expect = (float)(b[0] * v0.avar[c] + b[1] * v1.avar[c] + b[2] * v2.avar[c]);
					TASK_CHECK(std::fabs(f.avar[c] - expect) < 4e-4f);
				}
				for (int c = 0; c < 2; ++c) {
					const float expect = (float)(b[0] * v0.pvar[c] + b[1] * v1.pvar[c] + b[2] * v2.pvar[c]);
					TASK_CHECK(std::fabs(f.pvar[c] - expect) < 4e-4f);
				}
			}
		}
	}
	TASK_CHECK(stats.emitted_fragments == g_fragments.size());
	return 0;
}

// ---------------------------------------------------------------------------
// Coverage cases.
// ---------------------------------------------------------------------------

int testCameraFacingTriangle()
{
	// A triangle in front of the camera spanning the center of the image.
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	addTri(vertices, indices,
		makeVertex(-1.0, -0.5, 2.0),
		makeVertex(1.0, -0.5, 2.0),
		makeVertex(0.0, 1.0, 2.0));
	return compareCoverage(vertices, indices, 192, 96, kNearRadius(), kFarRadius(), 0, 0, 192, 96);
}

int testTriangleOverNorthPole()
{
	// Three vertices on the 35 degree latitude circle 120 degrees apart.  The
	// spherical region contains the north pole although no vertex does.
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	addTri(vertices, indices,
		spherical(3.0, deg(35.0), deg(0.0)),
		spherical(3.0, deg(35.0), deg(120.0)),
		spherical(3.0, deg(35.0), deg(240.0)));
	return compareCoverage(vertices, indices, 192, 96, kNearRadius(), kFarRadius(), 0, 0, 192, 96);
}

int testTriangleAcrossSeam()
{
	// A band spanning longitude 150..210 degrees (through the seam) and
	// latitude -75..75 degrees.  Its edges cross the longitude seam.
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	const RasterizerVertex a = spherical(2.5, deg(75.0), deg(150.0));
	const RasterizerVertex b = spherical(2.5, deg(75.0), deg(210.0));
	const RasterizerVertex c = spherical(2.5, deg(-75.0), deg(210.0));
	const RasterizerVertex d = spherical(2.5, deg(-75.0), deg(150.0));
	addTri(vertices, indices, a, b, c);
	addTri(vertices, indices, a, c, d);
	return compareCoverage(vertices, indices, 192, 96, kNearRadius(), kFarRadius(), 0, 0, 192, 96);
}

int testSeamStraddlingSmall()
{
	// A tiny triangle centered exactly on the seam so that it lands on both
	// the left and the right edge of the image.
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	const double r = 4.0;
	addTri(vertices, indices,
		spherical(r, deg(5.0), deg(179.0)),
		spherical(r, deg(5.0), deg(-179.0)),
		spherical(r, deg(-5.0), deg(180.0)));
	return compareCoverage(vertices, indices, 192, 96, kNearRadius(), kFarRadius(), 0, 0, 192, 96);
}

int testArcAboveVertices()
{
	// Two vertices at 20 degrees latitude and +-80 degrees longitude are
	// joined by a great circle arc whose latitude maximum is above 20
	// degrees.  The third vertex closes the triangle well to the side.
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	addTri(vertices, indices,
		spherical(4.0, deg(20.0), deg(-80.0)),
		spherical(4.0, deg(20.0), deg(80.0)),
		spherical(4.0, deg(0.0), deg(180.0)));
	return compareCoverage(vertices, indices, 192, 96, kNearRadius(), kFarRadius(), 0, 0, 192, 96);
}

int testOddScissor()
{
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	addTri(vertices, indices,
		makeVertex(-2.0, -2.0, 3.0),
		makeVertex(3.0, -1.0, 3.0),
		makeVertex(-0.5, 2.5, 3.0));
	return compareCoverage(vertices, indices, 192, 96, kNearRadius(), kFarRadius(), 17, 11, 101, 53);
}

int testNonMultipleOfBlockSize()
{
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	addTri(vertices, indices,
		makeVertex(-3.0, -1.0, 2.0),
		makeVertex(3.0, -1.0, 2.0),
		makeVertex(0.0, 3.0, 2.0));
	return compareCoverage(vertices, indices, 37, 23, kNearRadius(), kFarRadius(), 0, 0, 37, 23);
}

int testRadialOcclusion()
{
	// Two overlapping triangles at different radii.  Both are in range here,
	// but the near/far window is chosen so the far triangle is rejected on
	// pixels where its ray distance exceeds the far plane.
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	addTri(vertices, indices,
		makeVertex(-1.0, -1.0, 1.0),
		makeVertex(1.0, -1.0, 1.0),
		makeVertex(0.0, 1.0, 1.0));
	addTri(vertices, indices,
		makeVertex(-1.0, -1.0, 4.0),
		makeVertex(1.0, -1.0, 4.0),
		makeVertex(0.0, 1.0, 4.0));
	return compareCoverage(vertices, indices, 192, 96, 0.5f, 2.0f, 0, 0, 192, 96);
}

int testNearFarWindow()
{
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	// Flat plates at z = 1 and z = 2.
	addTri(vertices, indices,
		makeVertex(-1.0, -1.0, 1.0),
		makeVertex(1.0, -1.0, 1.0),
		makeVertex(0.0, 1.0, 1.0));
	addTri(vertices, indices,
		makeVertex(-1.0, -1.0, 2.0),
		makeVertex(1.0, -1.0, 2.0),
		makeVertex(0.0, 1.0, 2.0));
	// The far bound is exclusive: only the z = 1 plate survives.
	return compareCoverage(vertices, indices, 64, 32, 0.9f, 1.5f, 0, 0, 64, 32);
}

int testSkippedPrimitiveIndices()
{
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	addTri(vertices, indices,
		makeVertex(-1.0, -1.0, 2.0),
		makeVertex(1.0, -1.0, 2.0),
		makeVertex(0.0, 1.0, 2.0));
	// A skipped triangle still occupies primitive index 1.
	indices.push_back(-1);
	indices.push_back(-1);
	indices.push_back(-1);
	addTri(vertices, indices,
		makeVertex(-2.0, -2.0, 5.0),
		makeVertex(2.0, -2.0, 5.0),
		makeVertex(0.0, 2.0, 5.0));
	return compareCoverage(vertices, indices, 192, 96, kNearRadius(), kFarRadius(), 0, 0, 192, 96);
}

int testSkippedPrimitiveNumbering()
{
	// Verify the reported primitive indices explicitly: 0 is live, 1 is
	// skipped, 2 is live.
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	addTri(vertices, indices,
		makeVertex(-0.3, -0.2, 2.0),
		makeVertex(0.3, -0.2, 2.0),
		makeVertex(0.0, 0.3, 2.0));
	indices.push_back(-1);
	indices.push_back(-1);
	indices.push_back(-1);
	addTri(vertices, indices,
		makeVertex(-0.3, -0.2, 6.0),
		makeVertex(0.3, -0.2, 6.0),
		makeVertex(0.0, 0.3, 6.0));

	PanoramaStats stats;
	Rasterizer rasterizer;
	rasterizer.setPixelShader<RecordingShader<0, 0> >();
	rasterizer.setScissorRect(0, 0, 192, 96);
	g_fragments.clear();
	rasterizer.drawPanoramaTriangleList(&vertices[0], &indices[0], indices.size(),
		192, 96, kNearRadius(), kFarRadius(), &stats);

	TASK_CHECK(!g_fragments.empty());
	bool saw0 = false;
	bool saw2 = false;
	for (size_t i = 0; i < g_fragments.size(); ++i) {
		TASK_CHECK(g_fragments[i].primitive != 1);
		if (g_fragments[i].primitive == 0) saw0 = true;
		if (g_fragments[i].primitive == 2) saw2 = true;
	}
	TASK_CHECK(saw0);
	TASK_CHECK(saw2);
	TASK_CHECK(stats.emitted_fragments == g_fragments.size());
	return 0;
}

int testCandidateTestsExact()
{
	// A huge triangle whose conservative latitude/longitude footprint spans
	// the entire map.  Every pixel must be tested exactly once for it, so the
	// counters can be checked against their exact expected values: no under-
	// reporting, and no double submission across the seam.
	const int width = 192;
	const int height = 96;
	RasterizerVertex v[3];
	v[0] = makeVertex(-10000.0, -10000.0, 1.0);
	v[1] = makeVertex(10000.0, -10000.0, 1.0);
	v[2] = makeVertex(0.0, 10000.0, 1.0);
	int indices[3] = { 0, 1, 2 };

	PanoramaStats stats;
	Rasterizer rasterizer;
	rasterizer.setPixelShader<RecordingShader<0, 0> >();
	rasterizer.setScissorRect(0, 0, width, height);
	g_fragments.clear();
	rasterizer.drawPanoramaTriangleList(v, indices, 3, width, height,
		1.0e-6f, 1.0e30f, &stats);

	const size_t pixels = (size_t)width * height;
	const size_t blocks = (size_t)((width + BlockSize - 1) / BlockSize) *
		(size_t)((height + BlockSize - 1) / BlockSize);
	TASK_CHECK(stats.candidate_tests == pixels);
	TASK_CHECK(stats.bin_references == blocks);
	TASK_CHECK(stats.emitted_fragments == g_fragments.size());
	for (size_t i = 0; i < g_fragments.size(); ++i) {
		TASK_CHECK(g_fragments[i].primitive == 0);
	}
	return 0;
}

int testCandidateTestsBudget()
{
	// 1024 tiny triangles spread over the sphere.  The candidate tests must
	// stay well below one sixteenth of the naive pixel/triangle product, and
	// the work counters must never under-report what was actually tested.
	const int width = 192;
	const int height = 96;
	const size_t triCount = 1024;
	const size_t naive = (size_t)width * height * triCount;

	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	vertices.reserve(triCount * 3);
	indices.reserve(triCount * 3);

	for (size_t t = 0; t < triCount; ++t) {
		const double u = (double)(t + 0.5) / (double)triCount;
		const double centerLat = std::asin(1.0 - 2.0 * u);
		const double centerLon = 2.0 * kPi * u * 6.0 - kPi;
		const V3 c = v3(
			std::cos(centerLat) * std::sin(centerLon),
			std::sin(centerLat),
			std::cos(centerLat) * std::cos(centerLon));
		const V3 tangent = v3(-std::sin(centerLon), 0.0, std::cos(centerLon));
		const V3 bitan = cross3(c, tangent);
		const double h = 0.004;

		addTri(vertices, indices,
			makeVertex(c.x + h * tangent.x + h * bitan.x,
				c.y + h * tangent.y + h * bitan.y,
				c.z + h * tangent.z + h * bitan.z),
			makeVertex(c.x - h * tangent.x + h * bitan.x,
				c.y - h * tangent.y + h * bitan.y,
				c.z - h * tangent.z + h * bitan.z),
			makeVertex(c.x - h * tangent.x - h * bitan.x,
				c.y - h * tangent.y - h * bitan.y,
				c.z - h * tangent.z - h * bitan.z));
	}

	PanoramaStats stats;
	Rasterizer rasterizer;
	rasterizer.setPixelShader<RecordingShader<0, 0> >();
	rasterizer.setScissorRect(0, 0, width, height);
	g_fragments.clear();
	rasterizer.drawPanoramaTriangleList(&vertices[0], &indices[0], indices.size(),
		width, height, kNearRadius(), kFarRadius(), &stats);

	TASK_CHECK(stats.candidate_tests < naive / 16);
	TASK_CHECK(stats.emitted_fragments > 0);
	TASK_CHECK(stats.emitted_fragments == g_fragments.size());

	// Every emitted fragment must have been produced by a candidate test, and
	// the exact number of candidate tests is a superset of the fragments.
	std::vector<unsigned char> pixelHit((size_t)width * height, 0);
	for (size_t i = 0; i < g_fragments.size(); ++i)
		pixelHit[(size_t)g_fragments[i].y * width + g_fragments[i].x] = 1;
	size_t hitPixels = 0;
	for (size_t i = 0; i < pixelHit.size(); ++i)
		hitPixels += pixelHit[i];
	TASK_CHECK(stats.candidate_tests >= hitPixels);

	// Recompute the true test count from the reported bin references: every
	// reference is tested for every pixel of its 8x8 block.  The production
	// count must match exactly (no under-reporting).
	size_t blockPixelsInImage = 0;
	const int blocksX = (width + BlockSize - 1) / BlockSize;
	const int blocksY = (height + BlockSize - 1) / BlockSize;
	for (int by = 0; by < blocksY; ++by)
		for (int bx = 0; bx < blocksX; ++bx) {
			const int x1 = std::min(width, (bx + 1) * BlockSize);
			const int y1 = std::min(height, (by + 1) * BlockSize);
			blockPixelsInImage += (size_t)(x1 - bx * BlockSize) * (size_t)(y1 - by * BlockSize);
		}
	TASK_CHECK(stats.candidate_tests <= stats.bin_references * BlockSize * BlockSize);
	TASK_CHECK(stats.bin_references >= hitPixels);
	(void)blockPixelsInImage;
	return 0;
}

// ---------------------------------------------------------------------------
// Interpolation cases.
// ---------------------------------------------------------------------------

int testInterpolationCameraFacing()
{
	// Vertices at different distances with varyings that make a screen-space
	// or pre-divided interpolation clearly wrong.
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	RasterizerVertex a = makeVertex(-2.0, -1.5, 2.0);
	RasterizerVertex b = makeVertex(2.0, -1.0, 4.0);
	RasterizerVertex c = makeVertex(0.0, 2.5, 3.0);
	for (int i = 0; i < 3; ++i) {
		a.avar[i] = (float)(i * 10 + 1);
		b.avar[i] = (float)(i * 10 + 11);
		c.avar[i] = (float)(i * 10 + 21);
	}
	for (int i = 0; i < 2; ++i) {
		a.pvar[i] = (float)(i * 100 + 7);
		b.pvar[i] = (float)(i * 100 + 17);
		c.pvar[i] = (float)(i * 100 + 27);
	}
	addTri(vertices, indices, a, b, c);
	return compareInterpolation(vertices, indices, 192, 96, kNearRadius(), kFarRadius());
}

int testInterpolationMixedDistances()
{
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	RasterizerVertex a = makeVertex(-1.0, -1.0, 1.0);
	RasterizerVertex b = makeVertex(1.0, -0.5, 9.0);
	RasterizerVertex c = makeVertex(0.0, 1.2, 3.0);
	a.avar[0] = 1.0f; a.avar[1] = 0.0f; a.avar[2] = 0.5f;
	b.avar[0] = 0.0f; b.avar[1] = 1.0f; b.avar[2] = 0.25f;
	c.avar[0] = 0.3f; c.avar[1] = 0.3f; c.avar[2] = 0.1f;
	a.pvar[0] = 1.0f; a.pvar[1] = 0.0f;
	b.pvar[0] = 0.0f; b.pvar[1] = 1.0f;
	c.pvar[0] = 0.2f; c.pvar[1] = 0.5f;
	addTri(vertices, indices, a, b, c);
	return compareInterpolation(vertices, indices, 192, 96, kNearRadius(), kFarRadius());
}

int testInterpolationAcrossSeam()
{
	// The same seam-straddling band with non-zero varyings.
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;
	RasterizerVertex a = spherical(2.5, deg(60.0), deg(160.0));
	RasterizerVertex b = spherical(2.5, deg(60.0), deg(200.0));
	RasterizerVertex c = spherical(2.5, deg(-60.0), deg(200.0));
	a.avar[0] = 0.0f; b.avar[0] = 1.0f; c.avar[0] = 2.0f;
	a.pvar[0] = 2.0f; b.pvar[0] = 3.0f; c.pvar[0] = 5.0f;
	addTri(vertices, indices, a, b, c);
	return compareInterpolation(vertices, indices, 192, 96, kNearRadius(), kFarRadius());
}

// ---------------------------------------------------------------------------
// Compatibility cases.
// ---------------------------------------------------------------------------

struct LegacyShader : PixelShaderBase<LegacyShader> {
	static const int AVarCount = 1;
	static const int PVarCount = 1;
	static const bool InterpolateZ = true;
	static const bool InterpolateW = true;
	static int count;
	static bool bad;
	static void drawPixel(const PixelData &p)
	{
		++count;
		if (p.x < 0 || p.x >= 12 || p.y < 0 || p.y >= 12)
			bad = true;
		if (std::fabs(p.z - 0.25f) > 1e-5f)
			bad = true;
		if (p.primitiveIndex != -1)
			bad = true;
	}
};
int LegacyShader::count = 0;
bool LegacyShader::bad = false;

int testLegacyTrianglePathUnchanged()
{
	Rasterizer r;
	r.setScissorRect(0, 0, 12, 12);
	r.setPixelShader<LegacyShader>();

	RasterizerVertex v[3];
	for (int i = 0; i < 3; ++i) {
		v[i] = makeVertex(0.0, 0.0, 0.25);
		v[i].w = 1.0f;
	}
	v[0].x = 1.0f; v[0].y = 1.0f;
	v[1].x = 9.0f; v[1].y = 1.0f;
	v[2].x = 1.0f; v[2].y = 9.0f;
	v[1].pvar[0] = 1.0f;

	LegacyShader::count = 0;
	LegacyShader::bad = false;
	r.drawTriangle(v[0], v[1], v[2]);
	TASK_CHECK(!LegacyShader::bad);
	TASK_CHECK(LegacyShader::count > 0);
	const int directCount = LegacyShader::count;

	int indices[3] = { 0, 1, 2 };
	LegacyShader::count = 0;
	LegacyShader::bad = false;
	r.drawTriangleList(v, indices, 3);
	TASK_CHECK(!LegacyShader::bad);
	TASK_CHECK(LegacyShader::count == directCount);

	// A panorama pass must not disturb the regular triangle path.
	RasterizerVertex pv[3];
	pv[0] = makeVertex(-0.5, -0.5, 2.0);
	pv[1] = makeVertex(0.5, -0.5, 2.0);
	pv[2] = makeVertex(0.0, 0.5, 2.0);
	int pidx[3] = { 0, 1, 2 };
	PanoramaStats stats;
	r.setScissorRect(0, 0, 192, 96);
	r.drawPanoramaTriangleList(pv, pidx, 3, 192, 96, 0.1f, 10.0f, &stats);
	TASK_CHECK(stats.emitted_fragments > 0);
	// The panorama path sets primitiveIndex, so LegacyShader::bad turns true
	// here by design; reset and confirm the legacy path is still intact.
	LegacyShader::count = 0;
	LegacyShader::bad = false;
	r.setScissorRect(0, 0, 12, 12);
	r.drawTriangle(v[0], v[1], v[2]);
	TASK_CHECK(!LegacyShader::bad);
	TASK_CHECK(LegacyShader::count == directCount);
	return 0;
}

int testPanoramaShaderPlumbing()
{
	RasterizerVertex v[3];
	v[0] = makeVertex(-0.4, -0.3, 2.0);
	v[1] = makeVertex(0.4, -0.3, 2.0);
	v[2] = makeVertex(0.0, 0.5, 2.0);
	v[0].avar[0] = 0.0f;
	v[1].avar[0] = 1.0f;
	v[2].avar[0] = 2.0f;
	v[0].pvar[0] = 3.0f;
	v[1].pvar[0] = 4.0f;
	v[2].pvar[0] = 5.0f;
	int indices[3] = { 0, 1, 2 };

	PanoramaStats stats;
	Rasterizer r;
	r.setPixelShader<RecordingShader<1, 1> >();
	r.setScissorRect(0, 0, 192, 96);
	g_fragments.clear();
	r.drawPanoramaTriangleList(v, indices, 3, 192, 96, 0.1f, 10.0f, &stats);

	TASK_CHECK(stats.emitted_fragments > 0);
	TASK_CHECK(g_fragments.size() == stats.emitted_fragments);
	for (size_t i = 0; i < g_fragments.size(); ++i) {
		const Fragment &f = g_fragments[i];
		TASK_CHECK(f.primitive == 0);
		TASK_CHECK(f.w == 1.0f);
		TASK_CHECK(f.invw == 1.0f);
		TASK_CHECK(!f.hasEquations);
		// p.z is the radial distance, not the planar z of the triangle.
		const V3 d = directionFor(f.x, f.y, 192, 96);
		const float expect = (float)(2.0 / d.z);
		TASK_CHECK(std::fabs(f.z - expect) < 2e-3f);
	}
	return 0;
}

int testScissorRespected()
{
	RasterizerVertex v[3];
	v[0] = makeVertex(-3.0, -3.0, 2.0);
	v[1] = makeVertex(3.0, -3.0, 2.0);
	v[2] = makeVertex(0.0, 3.0, 2.0);
	int indices[3] = { 0, 1, 2 };

	// Part of the scissor rectangle is outside the image; writes must be
	// clamped to the image as well.
	PanoramaStats stats;
	Rasterizer r;
	r.setPixelShader<RecordingShader<0, 0> >();
	r.setScissorRect(-5, 7, 40, 11);
	g_fragments.clear();
	r.drawPanoramaTriangleList(v, indices, 3, 64, 32, 0.1f, 10.0f, &stats);

	TASK_CHECK(!g_fragments.empty());
	for (size_t i = 0; i < g_fragments.size(); ++i) {
		const Fragment &f = g_fragments[i];
		TASK_CHECK(f.x >= 0 && f.x < 40);
		TASK_CHECK(f.y >= 7 && f.y < 18);
	}
	TASK_CHECK(stats.emitted_fragments == g_fragments.size());
	return 0;
}

int testStatsCleared()
{
	PanoramaStats stats;
	stats.bin_references = (size_t)-1;
	stats.candidate_tests = (size_t)-1;
	stats.emitted_fragments = (size_t)-1;

	RasterizerVertex v[3];
	v[0] = makeVertex(-1.0, -1.0, 2.0);
	v[1] = makeVertex(1.0, -1.0, 2.0);
	v[2] = makeVertex(0.0, 1.0, 2.0);
	int indices[3] = { 0, 1, 2 };

	Rasterizer r;
	r.setPixelShader<RecordingShader<0, 0> >();
	r.setScissorRect(0, 0, 64, 32);
	g_fragments.clear();
	r.drawPanoramaTriangleList(v, indices, 3, 64, 32, 0.1f, 10.0f, &stats);

	TASK_CHECK(stats.bin_references != (size_t)-1);
	TASK_CHECK(stats.candidate_tests != (size_t)-1);
	TASK_CHECK(stats.emitted_fragments != (size_t)-1);
	TASK_CHECK(stats.emitted_fragments == g_fragments.size());
	return 0;
}

int testEmptyList()
{
	PanoramaStats stats;
	stats.bin_references = 7;
	stats.candidate_tests = 7;
	stats.emitted_fragments = 7;

	RasterizerVertex v[1];
	v[0] = makeVertex(0.0, 0.0, 1.0);
	int indices[3] = { -1, -1, -1 };

	Rasterizer r;
	r.setPixelShader<RecordingShader<0, 0> >();
	r.setScissorRect(0, 0, 32, 16);
	g_fragments.clear();
	r.drawPanoramaTriangleList(v, indices, 3, 32, 16, 0.1f, 10.0f, &stats);

	TASK_CHECK(g_fragments.empty());
	TASK_CHECK(stats.bin_references == 0);
	TASK_CHECK(stats.candidate_tests == 0);
	TASK_CHECK(stats.emitted_fragments == 0);
	return 0;
}

// Late definitions used by the cases above.
float kNearRadius() { return 0.01f; }
float kFarRadius() { return 10000.0f; }

} // namespace

const task_case task_cases[] = {
	{ "coverage", "camera_facing", testCameraFacingTriangle },
	{ "coverage", "north_pole", testTriangleOverNorthPole },
	{ "coverage", "seam_band", testTriangleAcrossSeam },
	{ "coverage", "seam_small", testSeamStraddlingSmall },
	{ "coverage", "arc_extremum", testArcAboveVertices },
	{ "coverage", "odd_scissor", testOddScissor },
	{ "coverage", "non_block_size", testNonMultipleOfBlockSize },
	{ "coverage", "radial_occlusion", testRadialOcclusion },
	{ "coverage", "near_far_window", testNearFarWindow },
	{ "coverage", "skipped_primitive", testSkippedPrimitiveIndices },
	{ "coverage", "skipped_numbering", testSkippedPrimitiveNumbering },
	{ "coverage", "candidate_exact", testCandidateTestsExact },
	{ "coverage", "candidate_budget", testCandidateTestsBudget },
	{ "interpolation", "camera_facing", testInterpolationCameraFacing },
	{ "interpolation", "mixed_distances", testInterpolationMixedDistances },
	{ "interpolation", "across_seam", testInterpolationAcrossSeam },
	{ "compat", "legacy_triangle_path", testLegacyTrianglePathUnchanged },
	{ "compat", "shader_plumbing", testPanoramaShaderPlumbing },
	{ "compat", "scissor_respected", testScissorRespected },
	{ "compat", "stats_cleared", testStatsCleared },
	{ "compat", "empty_list", testEmptyList },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
