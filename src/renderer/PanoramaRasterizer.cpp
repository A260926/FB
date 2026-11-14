/*
MIT License

Copyright (c) 2017-2020 Markus Trenkwalder

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "PanoramaRasterizer.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace swr {

namespace {

const double kPi = 3.14159265358979323846264338327950288;

struct Vec3 {
	double x;
	double y;
	double z;
};

inline Vec3 makeVec3(double x, double y, double z)
{
	Vec3 r = { x, y, z };
	return r;
}

inline Vec3 subVec(const Vec3 &a, const Vec3 &b)
{
	return makeVec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

inline Vec3 scaleVec(const Vec3 &a, double s)
{
	return makeVec3(a.x * s, a.y * s, a.z * s);
}

inline double dotVec(const Vec3 &a, const Vec3 &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 crossVec(const Vec3 &a, const Vec3 &b)
{
	return makeVec3(
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x);
}

inline double lengthVec(const Vec3 &a)
{
	return std::sqrt(dotVec(a, a));
}

inline Vec3 normalizeVec(const Vec3 &a)
{
	const double l = lengthVec(a);
	if (l == 0.0)
		return makeVec3(0.0, 0.0, 0.0);
	return scaleVec(a, 1.0 / l);
}

inline double clampUnit(double v)
{
	if (v < -1.0) return -1.0;
	if (v > 1.0) return 1.0;
	return v;
}

// Wrap an angle into (-pi, pi].
inline double wrapPi(double a)
{
	const double twoPi = 2.0 * kPi;
	a = std::fmod(a + kPi, twoPi);
	if (a < 0.0)
		a += twoPi;
	return a - kPi;
}

inline double latitudeOf(const Vec3 &d)
{
	return std::asin(clampUnit(d.y));
}

// Longitude of the equirectangular map: lambda = atan2(x, z).
inline double longitudeOf(const Vec3 &d)
{
	return std::atan2(d.x, d.z);
}

struct TriangleWork {
	const RasterizerVertex *v[3];
	Vec3 p[3];
	Vec3 u[3];
	Vec3 normal;
	double normalSq;
	double planeOffset;

	double latLo;
	double latHi;
	double lonLo;
	double lonHi;
	bool fullLon;
	bool valid;
};

// Direction through the center of the given pixel.
inline Vec3 pixelDirection(int x, int y, int width, int height)
{
	const double lambda = 2.0 * kPi * (x + 0.5) / width - kPi;
	const double phi = 0.5 * kPi - kPi * (y + 0.5) / height;
	const double cosPhi = std::cos(phi);
	return makeVec3(
		cosPhi * std::sin(lambda),
		std::sin(phi),
		cosPhi * std::cos(lambda));
}

struct Hit {
	double t;
	double b[3];
};

// Exact double sided ray/triangle intersection.  d is a unit direction and the
// ray starts at the camera origin.  The barycentric coordinates refer to the
// physical intersection point on the original triangle.
bool intersectTriangle(const Vec3 &d, const TriangleWork &tri, double tolerance, Hit &hit)
{
	const double denom = dotVec(tri.normal, d);
	if (!(denom != 0.0) || !std::isfinite(denom))
		return false;

	const double t = tri.planeOffset / denom;
	if (!(t > 0.0) || !std::isfinite(t))
		return false;

	const Vec3 p = scaleVec(d, t);

	const Vec3 c0 = crossVec(subVec(tri.p[1], p), subVec(tri.p[2], p));
	const Vec3 c1 = crossVec(subVec(tri.p[2], p), subVec(tri.p[0], p));
	const Vec3 c2 = crossVec(subVec(tri.p[0], p), subVec(tri.p[1], p));

	const double invNormalSq = 1.0 / tri.normalSq;
	double a0 = dotVec(c0, tri.normal) * invNormalSq;
	double a1 = dotVec(c1, tri.normal) * invNormalSq;
	double a2 = dotVec(c2, tri.normal) * invNormalSq;

	if (a0 < -tolerance || a1 < -tolerance || a2 < -tolerance)
		return false;

	const double sum = a0 + a1 + a2;
	if (!(sum != 0.0) || !std::isfinite(sum))
		return false;

	hit.t = t;
	hit.b[0] = a0 / sum;
	hit.b[1] = a1 / sum;
	hit.b[2] = a2 / sum;
	return true;
}

// Inside test for the two poles.  The tolerance keeps shared edges watertight.
bool poleInside(const Vec3 &pole, const TriangleWork &tri)
{
	Hit hit;
	return intersectTriangle(pole, tri, 1e-6, hit);
}

// Test whether x lies on the minor arc between a and b.
inline bool onMinorArc(const Vec3 &x, const Vec3 &a, const Vec3 &b)
{
	const double ca = dotVec(a, b);
	const double eps = 1e-7;
	return dotVec(x, a) >= ca - eps && dotVec(x, b) >= ca - eps;
}

void computeLatitudeBounds(TriangleWork &tri)
{
	const Vec3 north = makeVec3(0.0, 1.0, 0.0);
	const Vec3 south = makeVec3(0.0, -1.0, 0.0);

	double latLo = latitudeOf(tri.u[0]);
	double latHi = latLo;
	for (int i = 1; i < 3; ++i) {
		const double lat = latitudeOf(tri.u[i]);
		if (lat < latLo) latLo = lat;
		if (lat > latHi) latHi = lat;
	}

	// An edge whose direction arc passes through a pole has a discontinuous
	// longitude and a very ill conditioned great circle.  Detect both cases
	// and fall back to a full-sphere conservative bound.
	for (int e = 0; e < 3; ++e) {
		const Vec3 &a = tri.u[e];
		const Vec3 &b = tri.u[(e + 1) % 3];
		if (dotVec(a, b) < -0.999) {
			tri.latLo = -0.5 * kPi;
			tri.latHi = 0.5 * kPi;
			tri.fullLon = true;
			return;
		}
	}

	// The only latitude extrema in the interior of a spherical region are
	// the poles.  The extrema on a great circle arc are the two directions
	// parallel to e_y - (m_y / |m|^2) m, where m is the great circle normal.
	for (int e = 0; e < 3; ++e) {
		const Vec3 &a = tri.u[e];
		const Vec3 &b = tri.u[(e + 1) % 3];

		// An edge whose direction arc passes through a pole reaches a
		// latitude of +-90 degrees even when neither endpoint does.  This
		// also covers the exactly equatorial great circle (s2 == 0), where
		// the extremum formula below would divide by a tiny value.
		if (onMinorArc(north, a, b))
			latHi = 0.5 * kPi;
		if (onMinorArc(south, a, b))
			latLo = -0.5 * kPi;

		const Vec3 m = crossVec(a, b);
		const double m2 = dotVec(m, m);
		if (m2 <= 1e-24)
			continue;

		const Vec3 base = makeVec3(
			-(m.y / m2) * m.x,
			1.0 - (m.y / m2) * m.y,
			-(m.y / m2) * m.z);
		const double baseLen = lengthVec(base);
		if (!(baseLen > 1e-12))
			continue;
		const Vec3 extreme = scaleVec(base, 1.0 / baseLen);

		const Vec3 candidates[2] = { extreme, scaleVec(extreme, -1.0) };
		for (int c = 0; c < 2; ++c) {
			if (!onMinorArc(candidates[c], a, b))
				continue;
			const double lat = latitudeOf(candidates[c]);
			if (lat < latLo) latLo = lat;
			if (lat > latHi) latHi = lat;
		}
	}

	// A region that contains a pole has no meaningful longitude extent.
	if (poleInside(north, tri))
		latHi = 0.5 * kPi;
	if (poleInside(south, tri))
		latLo = -0.5 * kPi;

	tri.latLo = latLo;
	tri.latHi = latHi;

	// A region reaching this close to a pole has an ill conditioned
	// longitude range; cover every column.
	if (latHi > 0.5 * kPi - 1e-7 || latLo < -0.5 * kPi + 1e-7)
		tri.fullLon = true;
}

void computeLongitudeBounds(TriangleWork &tri)
{
	if (tri.fullLon)
		return;

	const double l0 = longitudeOf(tri.u[0]);
	const double l1 = longitudeOf(tri.u[1]);
	const double l2 = longitudeOf(tri.u[2]);

	// Unwrap the longitude values along the closed boundary walk.  Without a
	// pole inside the region the total winding is zero and the longitude
	// range is just the span of the three unwrapped values.
	const double d01 = wrapPi(l1 - l0);
	const double d12 = wrapPi(l2 - l1);
	const double d20 = wrapPi(l0 - l2);
	const double winding = d01 + d12 + d20;

	if (std::fabs(winding) > kPi) {
		// The boundary walk winds around the vertical axis: exactly one of
		// the two regions bounded by the curve contains a pole, and the
		// longitude interval covers every column.  The winding sign does not
		// identify which pole, so rely on the pole point tests.  If neither
		// fired (only possible through numeric grazing) fall back to the
		// full latitude range, which is always conservative.
		if (!(tri.latHi > 0.5 * kPi - 1e-7) && !(tri.latLo < -0.5 * kPi + 1e-7)) {
			tri.latLo = -0.5 * kPi;
			tri.latHi = 0.5 * kPi;
		}
		tri.fullLon = true;
		return;
	}

	const double u1 = l0 + d01;
	const double u2 = u1 + d12;

	double lonLo = std::min(l0, std::min(u1, u2));
	double lonHi = std::max(l0, std::max(u1, u2));

	// Small safety margin for the conservative bin selection.
	const double margin = 1e-9;
	lonLo -= margin;
	lonHi += margin;

	if (lonHi - lonLo >= 2.0 * kPi - 1e-6) {
		tri.fullLon = true;
		return;
	}

	tri.lonLo = lonLo;
	tri.lonHi = lonHi;
}

void prepareTriangle(TriangleWork &tri, const RasterizerVertex *vertices, const int *indices, size_t t)
{
	tri.valid = false;
	tri.fullLon = false;

	const int i0 = indices[3 * t + 0];
	const int i1 = indices[3 * t + 1];
	const int i2 = indices[3 * t + 2];
	if (i0 < 0 || i1 < 0 || i2 < 0)
		return;

	tri.v[0] = &vertices[i0];
	tri.v[1] = &vertices[i1];
	tri.v[2] = &vertices[i2];

	for (int i = 0; i < 3; ++i)
		tri.p[i] = makeVec3(tri.v[i]->x, tri.v[i]->y, tri.v[i]->z);

	tri.normal = crossVec(subVec(tri.p[1], tri.p[0]), subVec(tri.p[2], tri.p[0]));
	tri.normalSq = dotVec(tri.normal, tri.normal);
	tri.planeOffset = dotVec(tri.normal, tri.p[0]);
	if (!(tri.normalSq > 0.0) || !(tri.planeOffset != 0.0) || !std::isfinite(tri.planeOffset))
		return;

	for (int i = 0; i < 3; ++i)
		tri.u[i] = normalizeVec(tri.p[i]);

	tri.latLo = 0.0;
	tri.latHi = 0.0;
	tri.lonLo = 0.0;
	tri.lonHi = 0.0;

	computeLatitudeBounds(tri);
	computeLongitudeBounds(tri);

	tri.valid = true;
}

inline int clampInt(int v, int lo, int hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

struct XRange {
	int x0;
	int x1;
};

// Convert a continuous longitude interval (in pixel center coordinates) into
// at most two half open column ranges, splitting at the seam.  margin expands
// the interval on both sides so that numeric error can never drop a pixel.
int xRangesForLongitude(const TriangleWork &tri, int width, double margin, XRange ranges[2])
{
	const double uLo = (tri.lonLo + kPi) * width / (2.0 * kPi) - 0.5 - margin;
	const double uHi = (tri.lonHi + kPi) * width / (2.0 * kPi) - 0.5 + margin;
	const double span = uHi - uLo;

	if (tri.fullLon || span >= width - 1e-9) {
		ranges[0].x0 = 0;
		ranges[0].x1 = width;
		return 1;
	}

	const double w = width;
	const double start = uLo - std::floor(uLo / w) * w; // [0, width)
	const double end = start + span;

	const int first0 = (int)std::floor(start);
	const int first1 = (int)std::floor(end) + 1;

	if (end <= w) {
		ranges[0].x0 = clampInt(first0, 0, width);
		ranges[0].x1 = clampInt(first1, 0, width);
		return 1;
	}

	const int second1 = (int)std::floor(end - w) + 1;

	if (second1 > first0) {
		// The two pieces overlap; cover the whole row once.
		ranges[0].x0 = 0;
		ranges[0].x1 = width;
		return 1;
	}

	ranges[0].x0 = clampInt(first0, 0, width);
	ranges[0].x1 = width;
	ranges[1].x0 = 0;
	ranges[1].x1 = clampInt(second1, 0, width);
	return 2;
}

struct PixelRange {
	int x0;
	int x1;
	int y0;
	int y1;
};

int trianglePixelRanges(const TriangleWork &tri, int width, int height, PixelRange out[2])
{
	// One pixel of slack in each direction: the latitude/longitude bounds are
	// exact but their conversion uses transcendental functions.
	double yTop = (0.5 * kPi - tri.latHi) * height / kPi - 0.5 - 1.0;
	double yBottom = (0.5 * kPi - tri.latLo) * height / kPi - 0.5 + 1.0;
	if (yTop < -1.0) yTop = -1.0;
	if (yBottom > height) yBottom = height;

	int y0 = clampInt((int)std::floor(yTop), 0, height);
	int y1 = clampInt((int)std::floor(yBottom) + 1, 0, height);
	if (y0 >= y1)
		return 0;

	XRange xs[2];
	const int xCount = xRangesForLongitude(tri, width, 1.0, xs);
	int count = 0;
	for (int i = 0; i < xCount; ++i) {
		if (xs[i].x0 >= xs[i].x1)
			continue;
		out[count].x0 = xs[i].x0;
		out[count].x1 = xs[i].x1;
		out[count].y0 = y0;
		out[count].y1 = y1;
		++count;
	}
	return count;
}

} // namespace

void rasterizePanoramaTriangleListClipped(
	const RasterizerVertex *vertices,
	const int *indices,
	size_t indexCount,
	int width,
	int height,
	float nearRadius,
	float farRadius,
	PanoramaFragmentSink sink,
	void *sinkContext,
	PanoramaStats *stats,
	int clipMinX,
	int clipMinY,
	int clipMaxX,
	int clipMaxY)
{
	if (stats) {
		stats->bin_references = 0;
		stats->candidate_tests = 0;
		stats->emitted_fragments = 0;
	}

	if (!sink || !vertices || !indices || width <= 0 || height <= 0)
		return;

	clipMinX = std::max(0, clipMinX);
	clipMinY = std::max(0, clipMinY);
	clipMaxX = std::min(width, clipMaxX);
	clipMaxY = std::min(height, clipMaxY);
	if (clipMinX >= clipMaxX || clipMinY >= clipMaxY)
		return;

	const size_t triangleCount = indexCount / 3;
	if (triangleCount == 0)
		return;

	std::vector<TriangleWork> triangles(triangleCount);
	for (size_t t = 0; t < triangleCount; ++t)
		prepareTriangle(triangles[t], vertices, indices, t);

	const int blockColumns = (width + BlockSize - 1) / BlockSize;
	const int blockRows = (height + BlockSize - 1) / BlockSize;
	const size_t blockCount = (size_t)blockColumns * (size_t)blockRows;

	std::vector<std::vector<int> > bins(blockCount);
	std::vector<size_t> stamp(blockCount, (size_t)-1);

	// Insert every triangle into each 8x8 block touched by its conservative
	// latitude/longitude range.  A triangle is inserted at most once per
	// block and keeps its input order inside the block.
	for (size_t t = 0; t < triangleCount; ++t) {
		const TriangleWork &tri = triangles[t];
		if (!tri.valid)
			continue;

		PixelRange ranges[2];
		const int rangeCount = trianglePixelRanges(tri, width, height, ranges);
		if (rangeCount == 0)
			continue;

		for (int r = 0; r < rangeCount; ++r) {
			const int x0 = std::max(ranges[r].x0, clipMinX);
			const int x1 = std::min(ranges[r].x1, clipMaxX);
			const int y0 = std::max(ranges[r].y0, clipMinY);
			const int y1 = std::min(ranges[r].y1, clipMaxY);
			if (x0 >= x1 || y0 >= y1)
				continue;

			const int bx0 = x0 / BlockSize;
			const int bx1 = (x1 - 1) / BlockSize;
			const int by0 = y0 / BlockSize;
			const int by1 = (y1 - 1) / BlockSize;

			for (int by = by0; by <= by1; ++by) {
				for (int bx = bx0; bx <= bx1; ++bx) {
					const size_t block = (size_t)by * blockColumns + bx;
					if (stamp[block] == t)
						continue;
					stamp[block] = t;
					bins[block].push_back((int)t);
					if (stats)
						++stats->bin_references;
				}
			}
		}
	}

	size_t candidateTests = 0;
	size_t emitted = 0;

	for (int by = 0; by < blockRows; ++by) {
		const int rowY0 = by * BlockSize;
		const int rowY1 = std::min(rowY0 + BlockSize, height);
		if (rowY1 <= clipMinY || rowY0 >= clipMaxY)
			continue;

		for (int bx = 0; bx < blockColumns; ++bx) {
			const int colX0 = bx * BlockSize;
			const int colX1 = std::min(colX0 + BlockSize, width);
			if (colX1 <= clipMinX || colX0 >= clipMaxX)
				continue;

			const size_t block = (size_t)by * blockColumns + bx;
			const std::vector<int> &refs = bins[block];
			if (refs.empty())
				continue;

			const int yBegin = std::max(rowY0, clipMinY);
			const int yEnd = std::min(rowY1, clipMaxY);
			const int xBegin = std::max(colX0, clipMinX);
			const int xEnd = std::min(colX1, clipMaxX);

			for (int y = yBegin; y < yEnd; ++y) {
				for (int x = xBegin; x < xEnd; ++x) {
					const Vec3 d = pixelDirection(x, y, width, height);

					for (size_t r = 0; r < refs.size(); ++r) {
						const int t = refs[r];
						const TriangleWork &tri = triangles[t];
						++candidateTests;

						Hit hit;
						if (!intersectTriangle(d, tri, 1e-6, hit))
							continue;

						if (!(hit.t >= (double)nearRadius && hit.t < (double)farRadius))
							continue;

						++emitted;
						sink(sinkContext, x, y, (float)hit.t, t,
							(float)hit.b[0], (float)hit.b[1], (float)hit.b[2],
							tri.v[0], tri.v[1], tri.v[2]);
					}
				}
			}
		}
	}

	if (stats) {
		stats->candidate_tests = candidateTests;
		stats->emitted_fragments = emitted;
	}
}

void rasterizePanoramaTriangleList(
	const RasterizerVertex *vertices,
	const int *indices,
	size_t indexCount,
	int width,
	int height,
	float nearRadius,
	float farRadius,
	PanoramaFragmentSink sink,
	void *sinkContext,
	PanoramaStats *stats)
{
	rasterizePanoramaTriangleListClipped(
		vertices, indices, indexCount, width, height,
		nearRadius, farRadius, sink, sinkContext, stats,
		0, 0, width, height);
}

} // end namespace swr
