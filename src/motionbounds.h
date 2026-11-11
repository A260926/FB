// This file is part of meshoptimizer library; see meshoptimizer.h for version/license details
//
// Shared internal helpers for motion (morph) bounds and motion-aware meshlet building.
// Positions are interpolated linearly: p(t) = (1-t)*begin + t*end, t in [0,1].
#pragma once

#include <math.h>

namespace meshopt
{

// Unnormalized geometric normal of a triangle at time t, q(t) = e1(t) x e2(t),
// is a quadratic polynomial in t: q(t) = a + b*t + c*t^2
// Coefficients are kept in double because they combine float positions and feed exact interval tests;
// float-rounded intermediates alone could differ from a double-precision recomputation by ~1e-4 on unit normals at 1e3 scale.
struct MotionPoly
{
	double a[3]; // q(0)
	double b[3]; // first derivative coefficient
	double c[3]; // second derivative coefficient
	float scale; // reference magnitude for relative tolerances
};

inline void motionCrossD(double result[3], const double a[3], const double b[3])
{
	result[0] = a[1] * b[2] - a[2] * b[1];
	result[1] = a[2] * b[0] - a[0] * b[2];
	result[2] = a[0] * b[1] - a[1] * b[0];
}

// Computes q(t) coefficients for one triangle referencing vertices i0, i1, i2 in the two position buffers.
// vertex_stride_float is the stride in floats; positions_begin/end may point to the same buffer.
inline MotionPoly motionComputePoly(const float* positions_begin, const float* positions_end, size_t vertex_stride_float, unsigned int i0, unsigned int i1, unsigned int i2)
{
	MotionPoly result = {};

	// a repeated vertex makes the face algebraically zero-area for every t; return exact zeros so that
	// FMA contraction in cross products can't turn the identically-zero result into roundoff noise
	if (i0 == i1 || i1 == i2 || i0 == i2)
		return result;

	const float* pb0 = positions_begin + vertex_stride_float * i0;
	const float* pb1 = positions_begin + vertex_stride_float * i1;
	const float* pb2 = positions_begin + vertex_stride_float * i2;
	const float* pe0 = positions_end + vertex_stride_float * i0;
	const float* pe1 = positions_end + vertex_stride_float * i1;
	const float* pe2 = positions_end + vertex_stride_float * i2;

	// promote float inputs to double before any subtraction so the coefficients match a double-precision reference
	double e1b[3] = {double(pb1[0]) - pb0[0], double(pb1[1]) - pb0[1], double(pb1[2]) - pb0[2]};
	double e2b[3] = {double(pb2[0]) - pb0[0], double(pb2[1]) - pb0[1], double(pb2[2]) - pb0[2]};
	double e1e[3] = {double(pe1[0]) - pe0[0], double(pe1[1]) - pe0[1], double(pe1[2]) - pe0[2]};
	double e2e[3] = {double(pe2[0]) - pe0[0], double(pe2[1]) - pe0[1], double(pe2[2]) - pe0[2]};

	// edge velocities
	double d1[3] = {e1e[0] - e1b[0], e1e[1] - e1b[1], e1e[2] - e1b[2]};
	double d2[3] = {e2e[0] - e2b[0], e2e[1] - e2b[1], e2e[2] - e2b[2]};

	// q(t) = (e1b + d1*t) x (e2b + d2*t) = e1b x e2b + (d1 x e2b + e1b x d2)*t + (d1 x d2)*t^2
	motionCrossD(result.a, e1b, e2b);

	double d1e2b[3], e1bd2[3], d1d2[3];
	motionCrossD(d1e2b, d1, e2b);
	motionCrossD(e1bd2, e1b, d2);
	motionCrossD(d1d2, d1, d2);

	result.b[0] = d1e2b[0] + e1bd2[0];
	result.b[1] = d1e2b[1] + e1bd2[1];
	result.b[2] = d1e2b[2] + e1bd2[2];

	result.c[0] = d1d2[0];
	result.c[1] = d1d2[1];
	result.c[2] = d1d2[2];

	// reference magnitude: largest |q| at the endpoints and at t=0.5, never zero for a nondegenerate face
	double qm[3];
	for (int k = 0; k < 3; ++k)
		qm[k] = result.a[k] + result.b[k] * 0.5 + result.c[k] * 0.25;

	double sb = sqrt(result.a[0] * result.a[0] + result.a[1] * result.a[1] + result.a[2] * result.a[2]);
	double q1[3] = {result.a[0] + result.b[0] + result.c[0], result.a[1] + result.b[1] + result.c[1], result.a[2] + result.b[2] + result.c[2]};
	double se = sqrt(q1[0] * q1[0] + q1[1] * q1[1] + q1[2] * q1[2]);
	double sm = sqrt(qm[0] * qm[0] + qm[1] * qm[1] + qm[2] * qm[2]);

	double scale = sb > se ? sb : se;
	scale = scale > sm ? scale : sm;
	result.scale = float(scale);

	return result;
}

// A face that has zero area for every t has q(t) identically (or nearly) zero; it must not constrain the cone.
inline bool motionPolyDegenerate(const MotionPoly& p)
{
	// bound each component of q on [0,1] using endpoint + parabola vertex values; compare against the face scale
	double max2 = 0.0;

	for (int k = 0; k < 3; ++k)
	{
		double end = p.a[k] + p.b[k] + p.c[k];
		double lo = p.a[k] < end ? p.a[k] : end;
		double hi = p.a[k] > end ? p.a[k] : end;

		if (p.c[k] != 0.0)
		{
			double tv = -p.b[k] / (2.0 * p.c[k]);

			if (tv > 0.0 && tv < 1.0)
			{
				double v = p.a[k] + p.b[k] * tv + p.c[k] * tv * tv;
				lo = lo < v ? lo : v;
				hi = hi > v ? hi : v;
			}
		}

		double m = fabs(lo) > fabs(hi) ? fabs(lo) : fabs(hi);
		max2 += m * m;
	}

	// 1e-10 relative area threshold: anything smaller is treated as always degenerate
	double scale = double(p.scale);
	return max2 <= scale * scale * 1e-10;
}

// Representative (unit) direction of a face; returns false for always-degenerate faces.
inline bool motionPolyDirection(const MotionPoly& p, float direction[3])
{
	if (motionPolyDegenerate(p))
		return false;

	// average the normalized directions at t=0, 0.5 and 1; this makes the axis robust for static and translated planes
	double sum[3] = {0.0, 0.0, 0.0};

	for (int sample = 0; sample < 3; ++sample)
	{
		double t = sample == 0 ? 0.0 : (sample == 1 ? 0.5 : 1.0);
		double q[3];
		for (int k = 0; k < 3; ++k)
			q[k] = p.a[k] + p.b[k] * t + p.c[k] * t * t;

		double len = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);

		if (len != 0.0)
		{
			sum[0] += q[0] / len;
			sum[1] += q[1] / len;
			sum[2] += q[2] / len;
		}
	}

	double len = sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);

	if (len == 0.0)
		return false;

	direction[0] = float(sum[0] / len);
	direction[1] = float(sum[1] / len);
	direction[2] = float(sum[2] / len);
	return true;
}

// Power-basis quartic p[0..4]: non-negative on [0,1] iff its minimum there is.
// The minimum is at an endpoint or at a stationary point, i.e. a root of the cubic derivative p'(t);
// the cubic is solved analytically (Cardano/trigonometric form) so the test needs no sampling or subdivision depth cap.
inline double motionQuarticValue(const double p[5], double t)
{
	return p[0] + (p[1] + (p[2] + (p[3] + p[4] * t) * t) * t) * t;
}

inline bool motionPolynomialNonNegative(const double power[5], double eps)
{
	double mn = motionQuarticValue(power, 0.0);
	double end1 = motionQuarticValue(power, 1.0);
	mn = end1 < mn ? end1 : mn;

	// derivative: d1 + 2 d2 t + 3 d3 t^2 + 4 d4 t^3
	double a = 4.0 * power[4];
	double b = 3.0 * power[3];
	double c = 2.0 * power[2];
	double d = power[1];

	double roots[3];
	int root_count = 0;

	if (a == 0.0)
	{
		// quadratic or lower
		if (b == 0.0)
		{
			if (c != 0.0)
				roots[root_count++] = -d / c;
		}
		else
		{
			double disc = c * c - 4.0 * b * d;

			if (disc >= 0.0)
			{
				double sq = sqrt(disc);
				roots[root_count++] = (-c + sq) / (2.0 * b);

				if (disc > 0.0)
					roots[root_count++] = (-c - sq) / (2.0 * b);
			}
		}
	}
	else
	{
		// depress the cubic: t = u - b/(3a)
		double bn = b / a;
		double p = c / a - bn * bn / 3.0;
		double q = 2.0 * bn * bn * bn / 27.0 - bn * c / (3.0 * a) + d / a;
		double disc = (q * 0.5) * (q * 0.5) + (p / 3.0) * (p / 3.0) * (p / 3.0);
		double shift = bn / 3.0;

		if (disc > 0.0)
		{
			double sq = sqrt(disc);
			double u = cbrt(-q * 0.5 + sq) + cbrt(-q * 0.5 - sq);
			roots[root_count++] = u - shift;
		}
		else
		{
			// three real roots (disc == 0 yields repeated roots, handled by the same formula)
			const double pi = 3.14159265358979323846;
			double radius = 2.0 * sqrt(-p / 3.0);
			double angle = acos((3.0 * q / (2.0 * p)) * sqrt(-3.0 / p)) / 3.0;

			for (int k = 0; k < 3; ++k)
				roots[root_count++] = radius * cos(angle - 2.0 * double(k) * pi / 3.0) - shift;
		}
	}

	for (int i = 0; i < root_count; ++i)
	{
		double t = roots[i];

		if (t > 0.0 && t < 1.0)
		{
			double v = motionQuarticValue(power, t);
			mn = v < mn ? v : mn;
		}
	}

	return mn >= -eps;
}

// Single-face cone test; motionConeTest below is the set variant.
// Returns true if the face (unless always degenerate) satisfies dot(n(t), axis) >= limit for the whole interval.
// limit must be in [0,1]; numeric uncertainty is resolved on the side that preserves geometry (rejection).
inline bool motionConeTestPoly(const float axis[3], const MotionPoly& p, float limit)
{
	if (motionPolyDegenerate(p))
		return true;

	double a[3] = {p.a[0], p.a[1], p.a[2]};
	double b[3] = {p.b[0], p.b[1], p.b[2]};
	double c[3] = {p.c[0], p.c[1], p.c[2]};
	double ax[3] = {axis[0], axis[1], axis[2]};

	// f(t) = q(t) . axis
	double f0 = a[0] * ax[0] + a[1] * ax[1] + a[2] * ax[2];
	double f1 = b[0] * ax[0] + b[1] * ax[1] + b[2] * ax[2];
	double f2 = c[0] * ax[0] + c[1] * ax[1] + c[2] * ax[2];

	// f must be non-negative on [0,1] (quadratic): endpoints plus parabola vertex.
	// This is a structural sign question evaluated in doubles, so only roundoff-level slack is allowed;
	// a 1e-5 relative tolerance would hide genuine (if narrow) sign flips such as the 0.371-t/0.373-t interval.
	double feps = 1e-12 * double(p.scale);

	if (f0 < -feps || f0 + f1 + f2 < -feps)
		return false;

	if (f2 != 0.0)
	{
		double tv = -f1 / (2.0 * f2);

		if (tv > 0.0 && tv < 1.0 && f0 - f1 * f1 / (4.0 * f2) < -feps)
			return false;
	}

	// h(t) = |q(t)|^2, degree 4 coefficients
	double h0 = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
	double h1 = 2.0 * (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
	double h2 = b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + 2.0 * (a[0] * c[0] + a[1] * c[1] + a[2] * c[2]);
	double h3 = 2.0 * (b[0] * c[0] + b[1] * c[1] + b[2] * c[2]);
	double h4 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];

	// g(t) = f(t)^2 - limit^2 * h(t); the 1e-5 relative-scale tolerance is added inside the polynomial as
	// G(t) = g(t) + tol * h(t) >= 0  <=>  dp(t)^2 >= limit^2 - tol, which keeps the slack uniform over the
	// interval even when |q(t)| varies by orders of magnitude between its minimum and maximum
	double d2 = double(limit) * double(limit);
	const double tol = 1e-5;

	double g[5];
	g[0] = f0 * f0 - (d2 - tol) * h0;
	g[1] = 2.0 * f0 * f1 - (d2 - tol) * h1;
	g[2] = f1 * f1 + 2.0 * f0 * f2 - (d2 - tol) * h2;
	g[3] = 2.0 * f1 * f2 - (d2 - tol) * h3;
	g[4] = f2 * f2 - (d2 - tol) * h4;

	// only double roundoff goes into the test epsilon; the geometric tolerance already lives in the polynomial
	double geps = 1e-12 * double(p.scale) * double(p.scale);

	return motionPolynomialNonNegative(g, geps);
}

// Returns true if every nondegenerate face in the set satisfies dot(n(t), axis) >= limit for the whole interval.
inline bool motionConeTest(const float axis[3], const MotionPoly* polys, size_t poly_count, float limit)
{
	for (size_t i = 0; i < poly_count; ++i)
		if (!motionConeTestPoly(axis, polys[i], limit))
			return false;

	return true;
}

// Builds a cone axis from representative face directions; returns false when all faces are always degenerate.
inline bool motionConeAxis(const MotionPoly* polys, size_t poly_count, float axis[3])
{
	float sum[3] = {0, 0, 0};

	for (size_t i = 0; i < poly_count; ++i)
	{
		float dir[3];

		if (motionPolyDirection(polys[i], dir))
		{
			sum[0] += dir[0];
			sum[1] += dir[1];
			sum[2] += dir[2];
		}
	}

	float len = sqrtf(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);

	if (len == 0.f)
		return false;

	axis[0] = sum[0] / len;
	axis[1] = sum[1] / len;
	axis[2] = sum[2] / len;
	return true;
}

// Absolute cutoff safety margin, expressed in the same units as max_cone_cutoff.
// The bounds generator adds it to the proven cutoff; the builder subtracts it from its gate budget so that
// recomputed cutoffs never exceed the requested budget even for very tight cones.
const float kMotionConeCutoffMargin = 3e-5f;

// Computes a conservative cone cutoff for the face set around the given axis; returns 1 when no useful cone can be proven.
inline float motionConeCutoff(const MotionPoly* polys, size_t poly_count, const float axis[3])
{
	// first make sure the normals never point into the negative axis half-space; otherwise no cone exists
	if (!motionConeTest(axis, polys, poly_count, 0.f))
		return 1.f;

	// binary search for the largest provable dot-product limit; feasibility is monotone in the limit.
	// 24 iterations bound the limit quantization by 2^-24 so that tight cutoffs keep an accurate margin.
	double lo = 0.0, hi = 1.0;

	for (int iter = 0; iter < 24; ++iter)
	{
		double mid = (lo + hi) * 0.5;

		if (motionConeTest(axis, polys, poly_count, float(mid)))
			lo = mid;
		else
			hi = mid;
	}

	float cutoff = float(sqrt(1.0 - lo * lo)) + kMotionConeCutoffMargin;

	return cutoff >= 1.f - 1e-6f ? 1.f : cutoff;
}

// Ritter-style bounding sphere over the full motion segments [begin, end] of a set of unique vertices.
inline void motionBoundingSphere(float result_center[3], float& result_radius, const float* positions_begin, const float* positions_end, size_t vertex_stride_float, const unsigned int* vertices, size_t vertex_count)
{
	result_center[0] = result_center[1] = result_center[2] = 0.f;
	result_radius = 0.f;

	if (vertex_count == 0)
		return;

	// seed with the first segment endpoint
	const float* p0 = positions_begin + vertex_stride_float * vertices[0];
	float center[3] = {p0[0], p0[1], p0[2]};
	float radius = 0.f;

	for (size_t i = 0; i < vertex_count; ++i)
	{
		const float* pb = positions_begin + vertex_stride_float * vertices[i];
		const float* pe = positions_end + vertex_stride_float * vertices[i];

		float endpoints[2][3] = {{pb[0], pb[1], pb[2]}, {pe[0], pe[1], pe[2]}};

		for (int e = 0; e < 2; ++e)
		{
			float dx = endpoints[e][0] - center[0];
			float dy = endpoints[e][1] - center[1];
			float dz = endpoints[e][2] - center[2];
			float d = sqrtf(dx * dx + dy * dy + dz * dz);

			if (d > radius)
			{
				float k = d > 0.f ? (d - radius) / (2 * d) : 0.f;

				center[0] += k * dx;
				center[1] += k * dy;
				center[2] += k * dz;
				radius = (radius + d) / 2;
			}
		}
	}

	result_center[0] = center[0];
	result_center[1] = center[1];
	result_center[2] = center[2];
	// relative slack so that floating point drift can never leave a trajectory point outside
	result_radius = radius * (1.f + 1e-5f) + 1e-5f;
}

} // namespace meshopt
