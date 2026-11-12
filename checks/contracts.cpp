/*
 * contracts.cpp
 * Contract tests for LNLib::SurfaceGeodesic::Trace.
 *
 * The analytic references are derived independently of the tracer: straight
 * lines on a constant-metric plane, unfolded straight lines on a parabolic
 * cylinder, and closed-form arc-length integrals of the C1 piecewise graph
 * S(u,v)=(u,v,f(u)). Curved, non-developable paths are checked through the
 * tangential component of the spatial tangent derivative, plus step-size
 * convergence.
 */

#include "qa.hpp"

#include "SurfaceGeodesic.h"
#include "NurbsSurface.h"
#include "NurbsCurve.h"
#include "Polynomials.h"
#include "Constants.h"
#include "LNObject.h"
#include "UV.h"
#include "XYZ.h"
#include "XYZW.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace LNLib;

	constexpr double PosTol = 2e-6;
	constexpr double SpeedTol = 1e-7;

	double Square(double x) { return x * x; }

	std::vector<double> ClampedKnots(int degree, const std::vector<double>& interior)
	{
		std::vector<double> k;
		for (int i = 0; i <= degree; ++i) { k.push_back(0.0); }
		for (double t : interior) { k.push_back(t); }
		for (int i = 0; i <= degree; ++i) { k.push_back(1.0); }
		return k;
	}

	// Constant-metric skew plane S(u,v) = (u + 2v, 3u - v, 0), arbitrary rectangle.
	LN_NurbsSurface MakeSkewPlane(double u0, double u1, double v0, double v1,
		const std::vector<double>& innerU = {}, const std::vector<double>& innerV = {})
	{
		const int p = 2;
		LN_NurbsSurface s;
		s.DegreeU = s.DegreeV = p;
		auto mapKnots = [&](double a, double b, const std::vector<double>& inner)
		{
			std::vector<double> k;
			for (int i = 0; i <= p; ++i) { k.push_back(a); }
			for (double t : inner) { k.push_back(a + (b - a) * t); }
			for (int i = 0; i <= p; ++i) { k.push_back(b); }
			return k;
		};
		s.KnotVectorU = mapKnots(u0, u1, innerU);
		s.KnotVectorV = mapKnots(v0, v1, innerV);
		std::size_t nu = s.KnotVectorU.size() - p - 1;
		std::size_t nv = s.KnotVectorV.size() - p - 1;
		for (std::size_t i = 0; i < nu; ++i)
		{
			// Greville abscissae give exact linear (and hence bilinear) reproduction.
			double gu = 0.0;
			for (int j = 0; j < p; ++j) { gu += s.KnotVectorU[i + 1 + j]; }
			gu /= p;
			std::vector<XYZW> row;
			for (std::size_t j = 0; j < nv; ++j)
			{
				double gv = 0.0;
				for (int k = 0; k < p; ++k) { gv += s.KnotVectorV[j + 1 + k]; }
				gv /= p;
				row.emplace_back(XYZW(XYZ(gu + 2.0 * gv, 3.0 * gu - gv, 0.0), 1.0));
			}
			s.ControlPoints.push_back(row);
		}
		return s;
	}

	// Positive-weight quadratic parabolic cylinder S(u,v) = (v, k v^2, h u).
	// The cross-section is an exact quadratic Bezier parabola: its middle
	// control point is the tangent intersection (x = 0.5, y = 0), not the
	// sampled midpoint of the curve.
	LN_NurbsSurface MakeParabolicCylinder(double k, double h)
	{
		const int p = 2;
		LN_NurbsSurface s;
		s.DegreeU = s.DegreeV = p;
		s.KnotVectorU = ClampedKnots(p, {});
		s.KnotVectorV = ClampedKnots(p, {});
		double us[3] = { 0.0, 0.5, 1.0 };
		XYZW vSection[3] = {
			XYZW(XYZ(0.0, 0.0, 0.0), 1.0),
			XYZW(XYZ(0.5, 0.0, 0.0), 1.0),
			XYZW(XYZ(1.0, k, 0.0), 1.0),
		};
		for (double u : us)
		{
			std::vector<XYZW> row;
			for (const XYZW& cp : vSection)
			{
				XYZ base = cp.ToXYZ(true);
				row.emplace_back(XYZW(XYZ(base.X(), base.Y(), h * u), 1.0));
			}
			s.ControlPoints.push_back(row);
		}
		return s;
	}

	// Unfolded cross-section arc length Y(v) = integral_0^v sqrt(1 + 4 k^2 t^2) dt.
	double UnfoldY(double v, double k)
	{
		double z = 2.0 * k * v;
		return 0.5 * (v * std::sqrt(1.0 + z * z) + std::asinh(z) / (2.0 * k));
	}

	// Invert UnfoldY by bisection on [0,1].
	double InvertUnfoldY(double y, double k)
	{
		double lo = 0.0, hi = 1.0;
		for (int i = 0; i < 80; ++i)
		{
			double mid = 0.5 * (lo + hi);
			if (UnfoldY(mid, k) < y) { lo = mid; } else { hi = mid; }
		}
		return 0.5 * (lo + hi);
	}

	// C1 piecewise graph f(u): 0 for u <= 0.5, 4(u-0.5)^2 for u > 0.5.
	double FPiece(double u)
	{
		return u <= 0.5 ? 0.0 : 4.0 * Square(u - 0.5);
	}
	double FPrime(double u)
	{
		return u <= 0.5 ? 0.0 : 8.0 * (u - 0.5);
	}

	// L(u) = integral_0^u sqrt(1 + f'(t)^2) dt, closed form on each piece.
	double ArcL(double u)
	{
		if (u <= 0.5) { return u; }
		double x = u - 0.5;
		double tail = 0.5 * (x * std::sqrt(1.0 + 64.0 * x * x) + std::asinh(8.0 * x) / 8.0);
		return 0.5 + tail;
	}
	double InvertArcL(double length)
	{
		double lo = 0.0, hi = 1.0;
		for (int i = 0; i < 80; ++i)
		{
			double mid = 0.5 * (lo + hi);
			if (ArcL(mid) < length) { lo = mid; } else { hi = mid; }
		}
		return 0.5 * (lo + hi);
	}

	// Solve a small dense linear system by Gaussian elimination with partial pivoting.
	std::vector<double> SolveDense(std::vector<std::vector<double>> a, std::vector<double> b)
	{
		int n = (int)a.size();
		for (int col = 0; col < n; ++col)
		{
			int pivot = col;
			for (int r = col + 1; r < n; ++r)
			{
				if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) { pivot = r; }
			}
			std::swap(a[pivot], a[col]);
			std::swap(b[pivot], b[col]);
			for (int r = 0; r < n; ++r)
			{
				if (r == col) { continue; }
				double factor = a[r][col] / a[col][col];
				for (int c = col; c < n; ++c) { a[r][c] -= factor * a[col][c]; }
				b[r] -= factor * b[col];
			}
		}
		std::vector<double> x(n);
		for (int i = 0; i < n; ++i) { x[i] = b[i] / a[i][i]; }
		return x;
	}

	// NURBS representation of S(u,v) = (u, v, f(u)) with a simple interior knot
	// at 0.5 of multiplicity 1 (at least C1). Degree 2 in both directions.
	LN_NurbsSurface MakePiecewiseGraph()
	{
		const int p = 2;
		LN_NurbsSurface s;
		s.DegreeU = s.DegreeV = p;
		s.KnotVectorU = ClampedKnots(p, { 0.5 });
		s.KnotVectorV = ClampedKnots(p, {});
		int nU = (int)s.KnotVectorU.size() - p - 1;
		int nV = p + 1;

		std::vector<double> greville(nU);
		for (int i = 0; i < nU; ++i)
		{
			double g = 0.0;
			for (int j = 0; j < p; ++j) { g += s.KnotVectorU[i + 1 + j]; }
			greville[i] = g / p;
		}

		// Interpolation matrix A_ij = N_j,p(g_i), built from the library basis
		// routines (construction helper only; never compared against Trace).
		std::vector<std::vector<double>> a(nU, std::vector<double>(nU, 0.0));
		for (int i = 0; i < nU; ++i)
		{
			int span = Polynomials::GetKnotSpanIndex(p, s.KnotVectorU, greville[i]);
			double basis[Constants::NURBSMaxDegree + 1];
			Polynomials::BasisFunctions(span, p, s.KnotVectorU, greville[i], basis);
			for (int r = 0; r <= p; ++r) { a[i][span - p + r] = basis[r]; }
		}
		std::vector<double> fValues(nU);
		for (int i = 0; i < nU; ++i) { fValues[i] = FPiece(greville[i]); }
		std::vector<double> fControl = SolveDense(a, fValues);

		double vGreville[3] = { 0.0, 0.5, 1.0 };
		for (int i = 0; i < nU; ++i)
		{
			std::vector<XYZW> row;
			for (int j = 0; j < nV; ++j)
			{
				row.emplace_back(XYZW(XYZ(greville[i], vGreville[j], fControl[i]), 1.0));
			}
			s.ControlPoints.push_back(row);
		}
		return s;
	}

	// Asymmetric graph z = g(u,v) with non-zero Gaussian curvature, degree 3.
	LN_NurbsSurface MakeAsymmetricGraph()
	{
		const int n = 7;
		std::vector<std::vector<XYZ>> grid(n, std::vector<XYZ>(n));
		for (int i = 0; i < n; ++i)
		{
			double u = (double)i / (n - 1);
			for (int j = 0; j < n; ++j)
			{
				double v = (double)j / (n - 1);
				double z = 0.3 * u * u + 0.4 * v * v + 0.5 * u * v + 0.2 * u;
				grid[i][j] = XYZ(u, v, z);
			}
		}
		LN_NurbsSurface s;
		NurbsSurface::GlobalInterpolation(grid, 3, 3, s);
		return s;
	}

	void ExpectInvalidArgument(Check& check, const std::string& label,
		const std::function<void()>& call)
	{
		bool thrown = false;
		try { call(); }
		catch (const std::invalid_argument&) { thrown = true; }
		catch (...) { check.require(false, (label + ": wrong exception type").c_str()); }
		check.require(thrown, (label + ": invalid_argument was not thrown").c_str());
	}

	void ExpectRuntimeError(Check& check, const std::string& label,
		const std::function<void()>& call)
	{
		bool thrown = false;
		try { call(); }
		catch (const std::runtime_error&) { thrown = true; }
		catch (...) { check.require(false, (label + ": wrong exception type").c_str()); }
		check.require(thrown, (label + ": runtime_error was not thrown").c_str());
	}

	void CheckMonotoneSamples(Check& check, const GeodesicTrace& trace)
	{
		check.require(!trace.samples.empty(), "trace must publish at least the start sample");
		for (std::size_t i = 1; i < trace.samples.size(); ++i)
		{
			check.require(trace.samples[i].distance > trace.samples[i - 1].distance,
				"sample distances must be strictly increasing");
		}
		check.require(trace.samples.front().distance == 0.0, "start sample distance must be zero");
		for (const auto& q : trace.samples)
		{
			check.require(std::isfinite(q.position.X()) && std::isfinite(q.position.Y()) &&
				std::isfinite(q.position.Z()), "sample positions must be finite");
			double speedError = std::fabs(q.tangent.Length() - 1.0);
			check.require(speedError <= SpeedTol, "tangent must be a unit spatial vector");
		}
	}

	// Chord-length speed between successive stations must match arc length.
	void CheckChordSpeed(Check& check, const GeodesicTrace& trace)
	{
		for (std::size_t i = 1; i < trace.samples.size(); ++i)
		{
			double ds = trace.samples[i].distance - trace.samples[i - 1].distance;
			double chord = trace.samples[i].position.Distance(trace.samples[i - 1].position);
			check.require(ds > 0.0 && std::fabs(chord / ds - 1.0) <= 1e-5,
				"chord length must match travelled arc length at dense stations");
		}
	}

	// Pointwise geodesic residual. At every station away from a knot line,
	// recover the unit-speed UV velocity from the tangent and evaluate the
	// geodesic equation's spatial acceleration; its tangent-plane component
	// must vanish. Uses only the surface's own derivatives, not the tracer.
	double MaxGeodesicResidual(const LN_NurbsSurface& surface, const GeodesicTrace& trace)
	{
		std::vector<double> innerU;
		for (std::size_t i = surface.DegreeU + 1;
			i + surface.DegreeU + 1 < surface.KnotVectorU.size(); ++i)
		{
			innerU.push_back(surface.KnotVectorU[i]);
		}
		std::vector<double> innerV;
		for (std::size_t i = surface.DegreeV + 1;
			i + surface.DegreeV + 1 < surface.KnotVectorV.size(); ++i)
		{
			innerV.push_back(surface.KnotVectorV[i]);
		}
		const double knotBand = 1e-6;
		double worst = 0.0;
		for (const auto& sample : trace.samples)
		{
			bool onKnot = false;
			for (double k : innerU)
			{
				if (std::fabs(sample.parameter.U() - k) <= knotBand) { onKnot = true; }
			}
			for (double k : innerV)
			{
				if (std::fabs(sample.parameter.V() - k) <= knotBand) { onKnot = true; }
			}
			if (onKnot) { continue; }

			UV q;
			bool inverted = NurbsSurface::GetUVTangent(surface, sample.parameter, sample.tangent, q);
			if (!inverted) { continue; }
			auto d = NurbsSurface::ComputeRationalSurfaceDerivatives(surface, 2, sample.parameter);
			XYZ Su = d[1][0], Sv = d[0][1], Suu = d[2][0], Suv = d[1][1], Svv = d[0][2];
			double E = Su.DotProduct(Su), F = Su.DotProduct(Sv), G = Sv.DotProduct(Sv);
			double det = E * G - F * F;
			double Eu = 2.0 * Suu.DotProduct(Su);
			double Ev = 2.0 * Suv.DotProduct(Su);
			double Fu = Suu.DotProduct(Sv) + Su.DotProduct(Suv);
			double Fv = Suv.DotProduct(Sv) + Su.DotProduct(Svv);
			double Gu = 2.0 * Suv.DotProduct(Sv);
			double Gv = 2.0 * Svv.DotProduct(Sv);
			double inv2Det = 0.5 / det;
			double Guu = (G * Eu - F * (2.0 * Fu - Ev)) * inv2Det;
			double Guv = (G * Ev - F * Gu) * inv2Det;
			double Gvv = (G * (2.0 * Fv - Gu) - F * Gv) * inv2Det;
			double Vuu = (E * (2.0 * Fu - Ev) - F * Eu) * inv2Det;
			double Vuv = (E * Gu - F * Ev) * inv2Det;
			double Vvv = (E * Gv - F * (2.0 * Fv - Gu)) * inv2Det;
			double up = q.U(), vp = q.V();
			double upp = -(Guu * up * up + 2.0 * Guv * up * vp + Gvv * vp * vp);
			double vpp = -(Vuu * up * up + 2.0 * Vuv * up * vp + Vvv * vp * vp);
			XYZ acceleration = Su * upp + Sv * vpp +
				Suu * up * up + Suv * (2.0 * up * vp) + Svv * vp * vp;
			XYZ normal = NurbsSurface::Normal(surface, sample.parameter);
			XYZ tangential = acceleration - normal * acceleration.DotProduct(normal);
			worst = std::max(worst, tangential.Length());
		}
		return worst;
	}
}

void register_contracts(Registry& registry)
{
	using namespace LNLib;

	// ======================== analytic-paths ========================

	registry.add("analytic-paths", "skew-plane-straight-line", [](Check& check) -> int
	{
		LN_NurbsSurface plane = MakeSkewPlane(0.0, 2.0, 0.0, 3.0);
		UV start(0.4, 0.6);
		UV direction(0.7, -1.1);
		XYZ Su(1, 3, 0), Sv(2, -1, 0);
		double E = Su.DotProduct(Su), F = Su.DotProduct(Sv), G = Sv.DotProduct(Sv);
		double speed0 = std::sqrt(E * Square(direction.U()) +
			2.0 * F * direction.U() * direction.V() + G * Square(direction.V()));
		UV q(direction.U() / speed0, direction.V() / speed0);
		XYZ t0 = (Su * q.U() + Sv * q.V()).Normalize();

		std::vector<double> distances;
		for (int i = 0; i <= 20; ++i) { distances.push_back(0.05 * i); }
		GeodesicOptions opt;
		GeodesicTrace trace = SurfaceGeodesic::Trace(plane, start, direction, distances, opt);
		check.require(trace.status == GeodesicStatus::completed, "plane trace must complete");
		CheckMonotoneSamples(check, trace);
		CheckChordSpeed(check, trace);

		for (const auto& sample : trace.samples)
		{
			double s = sample.distance;
			UV expectUV(start.U() + q.U() * s, start.V() + q.V() * s);
			XYZ expectPos(expectUV.U() + 2.0 * expectUV.V(),
				3.0 * expectUV.U() - expectUV.V(), 0.0);
			check.require(sample.parameter.Distance(expectUV) <= PosTol, "plane UV reference");
			check.require(sample.position.Distance(expectPos) <= PosTol, "plane position reference");
			check.require(sample.tangent.Distance(t0) <= PosTol, "plane tangent reference");
		}
		return 0;
	});

	registry.add("analytic-paths", "direction-scaling-invariance", [](Check& check) -> int
	{
		LN_NurbsSurface plane = MakeSkewPlane(0.0, 2.0, 0.0, 3.0);
		std::vector<double> distances = { 0.0, 0.1, 0.3, 0.7 };
		GeodesicOptions opt;
		GeodesicTrace a = SurfaceGeodesic::Trace(plane, UV(0.5, 1.0), UV(0.6, 0.9), distances, opt);
		GeodesicTrace b = SurfaceGeodesic::Trace(plane, UV(0.5, 1.0), UV(0.6 * 4.2, 0.9 * 4.2), distances, opt);
		check.require(a.samples.size() == b.samples.size(), "scaled traces must align sample for sample");
		for (std::size_t i = 0; i < a.samples.size(); ++i)
		{
			check.require(a.samples[i].parameter.Distance(b.samples[i].parameter) <= PosTol,
				"scaled direction must give the same UV trace");
			check.require(a.samples[i].position.Distance(b.samples[i].position) <= PosTol,
				"scaled direction must give the same spatial trace");
			check.require(a.samples[i].tangent.Distance(b.samples[i].tangent) <= PosTol,
				"scaled direction must give the same tangent");
		}
		return 0;
	});

	registry.add("analytic-paths", "parabolic-cylinder-unfolded-line", [](Check& check) -> int
	{
		const double k = 0.8;
		const double h = 2.0;
		LN_NurbsSurface cylinder = MakeParabolicCylinder(k, h);
		UV start(0.3, 0.2);
		UV direction(0.5, 0.8);

		// Metric at the start: E = h^2, F = 0, G = 1 + (2 k v)^2.
		double g0 = 1.0 + Square(2.0 * k * start.V());
		double speed0 = std::sqrt(Square(h) * Square(direction.U()) + g0 * Square(direction.V()));
		double du = direction.U() / speed0;
		double dv = direction.V() / speed0;
		double a = h * du;                    // unfolded X speed (X = h u)
		double b = std::sqrt(g0) * dv;        // unfolded Y speed
		double x0 = h * start.U();
		double y0 = UnfoldY(start.V(), k);

		std::vector<double> distances;
		for (int i = 0; i <= 16; ++i) { distances.push_back(0.05 * i); }
		GeodesicOptions opt;
		GeodesicTrace trace = SurfaceGeodesic::Trace(cylinder, start, direction, distances, opt);
		check.require(trace.status == GeodesicStatus::completed, "cylinder trace must complete");
		CheckMonotoneSamples(check, trace);

		for (const auto& sample : trace.samples)
		{
			double s = sample.distance;
			double expectU = start.U() + a * s / h;
			double expectV = InvertUnfoldY(y0 + b * s, k);
			XYZ expectPos(expectV, k * Square(expectV), h * expectU);
			check.require(std::fabs(sample.parameter.U() - expectU) <= PosTol, "cylinder u reference");
			check.require(std::fabs(sample.parameter.V() - expectV) <= PosTol, "cylinder v reference");
			check.require(sample.position.Distance(expectPos) <= PosTol, "cylinder position reference");

			// Independent unit tangent from the unfolded line direction. The
			// unfolded speeds a, b are constant; the UV speed dv/ds is not,
			// because Y'(v) = sqrt(1 + 4 k^2 v^2).
			XYZ Su(0, 0, h);
			XYZ Sv(1, 2.0 * k * expectV, 0);
			double currentDu = a / h;
			double currentDv = b / std::sqrt(1.0 + Square(2.0 * k * expectV));
			double uvSpeed = std::sqrt(Square(h) * Square(currentDu) +
				(1.0 + Square(2.0 * k * expectV)) * Square(currentDv));
			XYZ expectTangent = (Su * currentDu + Sv * currentDv) / uvSpeed;
			check.require(sample.tangent.Distance(expectTangent) <= PosTol, "cylinder tangent reference");
		}
		return 0;
	});

	registry.add("analytic-paths", "rational-cylinder-unfolded-line", [](Check& check) -> int
	{
		// Factory rational quadratic cylinder. The cross-section is a unit
		// circular arc whose rational parameter v is not linear in the angle;
		// the independent unfolding uses Y(v) = angle of the evaluated point,
		// X(u) = 2(1-u). In (X, Y) the metric is Euclidean, so a geodesic is
		// a straight line there.
		LN_NurbsSurface cylinder;
		bool created = NurbsSurface::CreateCylindricalSurface(XYZ(0, 0, 0),
			XYZ(1, 0, 0), XYZ(0, 1, 0), 0.0, Constants::Pi / 2.0, 1.0, 2.0, cylinder);
		check.require(created, "rational cylinder construction");
		bool hasRationalWeight = false;
		for (const auto& row : cylinder.ControlPoints)
		{
			for (const XYZW& q : row)
			{
				if (q.GetW() != 1.0) { hasRationalWeight = true; }
			}
		}
		check.require(hasRationalWeight, "cylinder must carry genuine positive rational weights");

		auto angleAt = [&](double v)
		{
			XYZ p = NurbsSurface::GetPointOnSurface(cylinder, UV(0.0, v));
			return std::atan2(p.Y(), p.X());
		};
		auto invertAngle = [&](double angle)
		{
			double lo = 0.0, hi = 1.0;
			for (int i = 0; i < 80; ++i)
			{
				double mid = 0.5 * (lo + hi);
				if (angleAt(mid) < angle) { lo = mid; } else { hi = mid; }
			}
			return 0.5 * (lo + hi);
		};

		UV start(0.3, 0.25);
		UV raw(0.7, 0.9);
		auto startDer = NurbsSurface::ComputeRationalSurfaceDerivatives(cylinder, 1, start);
		double E0 = startDer[1][0].SqrLength();
		double G0 = startDer[0][1].SqrLength();
		double speed0 = std::sqrt(E0 * Square(raw.U()) + G0 * Square(raw.V()));
		double du = raw.U() / speed0;
		double dv = raw.V() / speed0;
		double alpha = -2.0 * du;                 // unfolded X speed, X = 2(1-u)
		double beta = std::sqrt(G0) * dv;         // unfolded Y speed, Y = angle
		check.require(std::fabs(Square(alpha) + Square(beta) - 1.0) <= 1e-12,
			"unfolded reference direction is unit speed");
		double x0 = 2.0 * (1.0 - start.U());
		double y0 = angleAt(start.V());

		std::vector<double> distances;
		for (int i = 0; i <= 12; ++i) { distances.push_back(0.02 * i); }
		GeodesicOptions opt;
		GeodesicTrace trace = SurfaceGeodesic::Trace(cylinder, start, raw, distances, opt);
		check.require(trace.status == GeodesicStatus::completed, "rational cylinder trace completes");
		CheckMonotoneSamples(check, trace);

		for (const auto& sample : trace.samples)
		{
			double s = sample.distance;
			double expectX = x0 + alpha * s;
			double expectY = y0 + beta * s;
			double expectU = 1.0 - expectX / 2.0;
			double expectV = invertAngle(expectY);
			XYZ expectPos(std::cos(expectY), std::sin(expectY), expectX);
			check.require(std::fabs(sample.parameter.U() - expectU) <= PosTol,
				"rational cylinder u reference");
			check.require(std::fabs(sample.parameter.V() - expectV) <= PosTol,
				"rational cylinder v reference");
			check.require(sample.position.Distance(expectPos) <= PosTol,
				"rational cylinder position reference");

			auto ders = NurbsSurface::ComputeRationalSurfaceDerivatives(cylinder, 1,
				UV(expectU, expectV));
			// du is constant; dv(s) = beta / (d angle / dv) = beta / |Sv|.
			double currentDv = beta / ders[0][1].Length();
			XYZ expectTangent = (ders[1][0] * du + ders[0][1] * currentDv).Normalize();
			check.require(sample.tangent.Distance(expectTangent) <= PosTol,
				"rational cylinder tangent reference");
		}

		// Independent first-boundary stop: first of the u = 0/1 and v = 1 edges.
		double stopU1 = alpha < 0.0 ? -x0 / alpha : std::numeric_limits<double>::infinity();
		double stopU0 = alpha > 0.0 ? (2.0 - x0) / alpha : std::numeric_limits<double>::infinity();
		double stopV1 = beta > 0.0 ? (Constants::Pi / 2.0 - y0) / beta
			: std::numeric_limits<double>::infinity();
		double stopDistance = std::min(std::min(stopU0, stopU1), stopV1);
		GeodesicTrace stopped = SurfaceGeodesic::Trace(cylinder, start, raw,
			{ 0.0, stopDistance + 1.0 }, opt);
		check.require(stopped.status == GeodesicStatus::boundary, "cylinder reaches boundary");
		check.require(std::fabs(stopped.samples.back().distance - stopDistance) <= PosTol,
			"cylinder boundary stop distance reference");
		const UV& endParam = stopped.samples.back().parameter;
		bool onEdge = std::fabs(endParam.U()) <= 1e-10 || std::fabs(endParam.U() - 1.0) <= 1e-10 ||
			std::fabs(endParam.V() - 1.0) <= 1e-10;
		check.require(onEdge, "cylinder boundary sample sits exactly on an edge");
		return 0;
	});

	registry.add("analytic-paths", "piecewise-graph-arc-length", [](Check& check) -> int
	{
		LN_NurbsSurface graph = MakePiecewiseGraph();

		// Sanity: the constructed NURBS really is S(u,v)=(u,v,f(u)).
		for (double u : {0.1, 0.3, 0.5, 0.7, 0.9})
		{
			for (double v : {0.0, 0.5, 1.0})
			{
				XYZ p = NurbsSurface::GetPointOnSurface(graph, UV(u, v));
				check.require(p.Distance(XYZ(u, v, FPiece(u))) <= 1e-9, "piecewise graph construction");
			}
		}

		UV start(0.25, 0.5);
		UV direction(1.0, 0.0); // pure-u path crosses the knot at u = 0.5.
		std::vector<double> distances;
		for (int i = 0; i <= 24; ++i) { distances.push_back(0.025 * i); }
		GeodesicOptions opt;
		GeodesicTrace trace = SurfaceGeodesic::Trace(graph, start, direction, distances, opt);
		check.require(trace.status == GeodesicStatus::completed, "graph trace must complete");
		CheckMonotoneSamples(check, trace);

		bool sawKnot = false;
		for (const auto& sample : trace.samples)
		{
			double s = sample.distance;
			double expectU = InvertArcL(ArcL(start.U()) + s);
			XYZ expectPos(expectU, start.V(), FPiece(expectU));
			check.require(std::fabs(sample.parameter.U() - expectU) <= PosTol, "graph u reference");
			check.require(sample.position.Distance(expectPos) <= PosTol, "graph position reference");
			double fp = FPrime(expectU);
			XYZ expectTangent(1.0, 0.0, fp);
			expectTangent = expectTangent / std::sqrt(1.0 + fp * fp);
			check.require(sample.tangent.Distance(expectTangent) <= PosTol, "graph tangent reference");
			if (std::fabs(sample.parameter.U() - 0.5) <= 1e-10) { sawKnot = true; }
		}
		check.require(sawKnot, "crossing of the interior knot at 0.5 must be sampled");
		return 0;
	});

	registry.add("analytic-paths", "piecewise-graph-oblique-reference", [](Check& check) -> int
	{
		LN_NurbsSurface graph = MakePiecewiseGraph();
		UV start(0.25, 0.3);
		UV raw(0.8, 0.6);
		// Unit-speed normalization at the start, where f' = 0 and the metric is identity.
		double c = raw.V() / std::sqrt(Square(raw.U()) + Square(raw.V()));
		double root = std::sqrt(1.0 - c * c);

		std::vector<double> distances;
		for (int i = 0; i <= 20; ++i) { distances.push_back(0.03 * i); }
		GeodesicOptions opt;
		GeodesicTrace trace = SurfaceGeodesic::Trace(graph, start, raw, distances, opt);
		check.require(trace.status == GeodesicStatus::completed, "oblique graph trace must complete");
		CheckMonotoneSamples(check, trace);

		double l0 = ArcL(start.U());
		for (const auto& sample : trace.samples)
		{
			double s = sample.distance;
			double expectU = InvertArcL(l0 + s * root);
			double expectV = start.V() + c * s;
			XYZ expectPos(expectU, expectV, FPiece(expectU));
			check.require(std::fabs(sample.parameter.U() - expectU) <= PosTol, "oblique u reference");
			check.require(std::fabs(sample.parameter.V() - expectV) <= PosTol, "oblique v reference");
			check.require(sample.position.Distance(expectPos) <= PosTol, "oblique position reference");
		}
		return 0;
	});

	registry.add("analytic-paths", "curved-surface-geodesicity-and-convergence", [](Check& check) -> int
	{
		LN_NurbsSurface graph = MakeAsymmetricGraph();
		double gauss = NurbsSurface::Curvature(graph, SurfaceCurvature::Gauss, UV(0.5, 0.5));
		check.require(std::fabs(gauss) > 1e-6, "reference surface must have non-zero Gaussian curvature");

		std::vector<double> distances;
		for (int i = 0; i <= 60; ++i) { distances.push_back(0.01 * i); }
		UV start(0.2, 0.3);
		UV direction(0.9, 0.4);

		GeodesicOptions fine;
		fine.tolerance = 1e-10;
		GeodesicOptions coarse;
		coarse.tolerance = 1e-8;
		GeodesicTrace tFine = SurfaceGeodesic::Trace(graph, start, direction, distances, fine);
		GeodesicTrace tCoarse = SurfaceGeodesic::Trace(graph, start, direction, distances, coarse);
		check.require(tFine.status == GeodesicStatus::completed &&
			tCoarse.status == GeodesicStatus::completed, "curved traces must complete");
		check.require(tFine.samples.size() == tCoarse.samples.size(), "convergence sample alignment");

		double worstPosition = 0.0;
		for (std::size_t i = 0; i < tFine.samples.size(); ++i)
		{
			worstPosition = std::max(worstPosition,
				tFine.samples[i].position.Distance(tCoarse.samples[i].position));
		}
		check.require(worstPosition <= 1e-6, "traces must converge as the step tolerance tightens");

		double residualFine = MaxGeodesicResidual(graph, tFine);
		double residualCoarse = MaxGeodesicResidual(graph, tCoarse);
		check.require(residualFine < 1e-3, "fine trace must follow a geodesic (tangential acceleration)");
		check.require(residualFine <= residualCoarse + 1e-12,
			"tighter tolerance must not worsen the geodesic residual");
		CheckMonotoneSamples(check, tFine);
		return 0;
	});

	registry.add("analytic-paths", "revolution-meridian-geodesic", [](Check& check) -> int
	{
		// A smooth C2 profile revolved into a surface of revolution. Meridians
		// (constant revolution angle) are exact geodesics of any such surface.
		const double R = 2.0;
		const double r = 0.5;
		std::vector<XYZ> profilePoints;
		for (int i = 0; i <= 8; ++i)
		{
			double theta = 0.4 + (2.4 * i) / 8.0;
			profilePoints.emplace_back(XYZ(R + r * std::cos(theta), 0.0, r * std::sin(theta)));
		}
		LN_NurbsCurve profile;
		NurbsCurve::GlobalInterpolation(3, profilePoints, profile);
		LN_NurbsSurface revolved;
		bool revOk = NurbsSurface::CreateRevolvedSurface(XYZ(0, 0, 0), XYZ(0, 0, 1),
			Constants::Pi / 2.0, profile, revolved);
		check.require(revOk, "revolved surface construction");

		std::vector<double> distances;
		for (int i = 0; i <= 40; ++i) { distances.push_back(0.01 * i); }
		GeodesicOptions opt;
		GeodesicTrace trace = SurfaceGeodesic::Trace(revolved, UV(0.5, 0.5), UV(0.0, 1.0), distances, opt);
		check.require(trace.status == GeodesicStatus::completed, "revolution meridian trace must complete");
		CheckMonotoneSamples(check, trace);
		double residual = MaxGeodesicResidual(revolved, trace);
		check.require(residual < 1e-3, "revolution meridian must be a geodesic");

		// Meridians stay in the revolution plane at fixed angle theta = u * pi/2.
		double theta = 0.5 * Constants::Pi / 2.0;
		for (const auto& sample : trace.samples)
		{
			double ratio = std::atan2(sample.position.Y(), sample.position.X());
			check.require(std::fabs(ratio - theta) <= PosTol, "meridian must stay in its revolution plane");
		}
		return 0;
	});

	// ======================== knot-events ========================

	registry.add("knot-events", "forward-and-reverse-u-crossing", [](Check& check) -> int
	{
		LN_NurbsSurface graph = MakePiecewiseGraph();
		GeodesicOptions opt;
		std::vector<double> distances = { 0.0, 0.1, 0.2, 0.3, 0.45 };

		GeodesicTrace forward = SurfaceGeodesic::Trace(graph, UV(0.25, 0.5), UV(1, 0), distances, opt);
		GeodesicTrace reverse = SurfaceGeodesic::Trace(graph, UV(0.75, 0.5), UV(-1, 0), distances, opt);
		check.require(forward.status == GeodesicStatus::completed, "forward trace completes");
		check.require(reverse.status == GeodesicStatus::completed, "reverse trace completes");

		auto countAtKnot = [](const GeodesicTrace& t)
		{
			int count = 0;
			for (const auto& q : t.samples) { if (std::fabs(q.parameter.U() - 0.5) <= 1e-10) { ++count; } }
			return count;
		};
		check.require(countAtKnot(forward) == 1, "forward crossing published exactly once");
		check.require(countAtKnot(reverse) == 1, "reverse crossing published exactly once");

		// C1 continuity: position and tangent at the knot agree from both sides.
		const GeodesicSample* fKnot = nullptr;
		const GeodesicSample* rKnot = nullptr;
		for (const auto& q : forward.samples) { if (std::fabs(q.parameter.U() - 0.5) <= 1e-10) { fKnot = &q; } }
		for (const auto& q : reverse.samples) { if (std::fabs(q.parameter.U() - 0.5) <= 1e-10) { rKnot = &q; } }
		check.require(fKnot != nullptr && rKnot != nullptr, "both traces reach the knot");
		check.require(fKnot->position.Distance(rKnot->position) <= PosTol, "position continuous at knot");
		// The two traces travel in opposite directions, so compare tangent lines:
		// their unit tangents must be parallel (dot product +-1).
		double tangentAlignment = std::fabs(fKnot->tangent.DotProduct(rKnot->tangent));
		check.require(std::fabs(tangentAlignment - 1.0) <= PosTol,
			"unit tangent line continuous at the C1 knot (opposite travel directions)");
		check.require(forward.samples.back().parameter.U() > 0.5, "forward trace enters the right cell");
		check.require(reverse.samples.back().parameter.U() < 0.5, "reverse trace enters the left cell");
		return 0;
	});

	registry.add("knot-events", "v-direction-crossing", [](Check& check) -> int
	{
		// Plane with an interior V knot; metric is constant so the path is still exact.
		LN_NurbsSurface plane = MakeSkewPlane(0.0, 1.0, 0.0, 1.0, {}, { 0.5 });
		GeodesicOptions opt;
		std::vector<double> distances = { 0.0, 0.2, 0.4, 0.6, 0.8 };
		GeodesicTrace trace = SurfaceGeodesic::Trace(plane, UV(0.2, 0.2), UV(0.0, 1.0), distances, opt);
		check.require(trace.status == GeodesicStatus::completed, "v crossing trace completes");
		int knotHits = 0;
		for (const auto& q : trace.samples)
		{
			if (std::fabs(q.parameter.V() - 0.5) <= 1e-10) { ++knotHits; }
		}
		check.require(knotHits == 1, "v knot crossing published exactly once");
		check.require(trace.samples.back().parameter.V() > 0.5, "trace continues into the next v cell");
		CheckMonotoneSamples(check, trace);
		return 0;
	});

	registry.add("knot-events", "simultaneous-two-axis-crossing", [](Check& check) -> int
	{
		LN_NurbsSurface plane = MakeSkewPlane(0.0, 1.0, 0.0, 1.0, { 0.5 }, { 0.5 });
		GeodesicOptions opt;
		std::vector<double> distances = { 0.0, 0.3, 0.6, 0.9, 1.2 };
		GeodesicTrace trace = SurfaceGeodesic::Trace(plane, UV(0.25, 0.25), UV(1, 1), distances, opt);
		check.require(trace.status == GeodesicStatus::completed, "diagonal trace completes");
		int jointHits = 0;
		for (const auto& q : trace.samples)
		{
			if (std::fabs(q.parameter.U() - 0.5) <= 1e-10 &&
				std::fabs(q.parameter.V() - 0.5) <= 1e-10) { ++jointHits; }
		}
		check.require(jointHits == 1, "simultaneous two-axis crossing is a single event");
		// No duplicate event and the trace keeps progressing afterwards.
		check.require(trace.samples.back().parameter.U() > 0.5 &&
			trace.samples.back().parameter.V() > 0.5, "trace leaves the joint crossing");
		CheckMonotoneSamples(check, trace);
		return 0;
	});

	registry.add("knot-events", "travel-along-knot-line", [](Check& check) -> int
	{
		LN_NurbsSurface plane = MakeSkewPlane(0.0, 1.0, 0.0, 1.0, { 0.5 }, {});
		GeodesicOptions opt;
		std::vector<double> distances;
		for (int i = 0; i <= 10; ++i) { distances.push_back(0.03 * i); }
		GeodesicTrace trace = SurfaceGeodesic::Trace(plane, UV(0.5, 0.2), UV(0, 1), distances, opt);
		check.require(trace.status == GeodesicStatus::completed, "along-knot trace completes without stalling");
		int knotSamples = 0;
		for (const auto& q : trace.samples)
		{
			check.require(std::fabs(q.parameter.U() - 0.5) <= 1e-10, "path stays on the u knot line");
			++knotSamples;
		}
		check.require(knotSamples == (int)trace.samples.size(), "all samples remain on the knot line");
		check.require(trace.samples.back().parameter.V() > 0.2, "along-line travel progresses in v");
		return 0;
	});

	registry.add("knot-events", "event-coincides-with-requested-station", [](Check& check) -> int
	{
		LN_NurbsSurface plane = MakeSkewPlane(0.0, 1.0, 0.0, 1.0, { 0.5 }, {});
		// On the identity-metric-like plane S=(u+2v,3u-v), pick pure u travel:
		// spatial speed for direction (1,0) is sqrt(10), so the knot at u=0.5
		// from u=0.25 is reached after 0.25 * sqrt(10).
		double knotDistance = 0.25 * std::sqrt(10.0);
		std::vector<double> distances = { 0.0, knotDistance, knotDistance + 0.1 };
		GeodesicOptions opt;
		GeodesicTrace trace = SurfaceGeodesic::Trace(plane, UV(0.25, 0.5), UV(1, 0), distances, opt);
		check.require(trace.status == GeodesicStatus::completed, "coincident event trace completes");
		int knotHits = 0;
		for (const auto& q : trace.samples)
		{
			if (std::fabs(q.parameter.U() - 0.5) <= 1e-9) { ++knotHits; }
		}
		check.require(knotHits == 1, "event and station at the same point store one sample");
		return 0;
	});

	// ======================== termination ========================

	registry.add("termination", "four-edge-and-corner-stops", [](Check& check) -> int
	{
		LN_NurbsSurface plane = MakeSkewPlane(0.0, 1.0, 0.0, 1.0);
		GeodesicOptions opt;
		std::vector<double> far = { 0.0, 5.0 };

		struct Case { UV dir; double uEdge; double vEdge; };
		Case cases[4] = {
			{ UV(-1, 0), 0.0, 0.5 },
			{ UV(1, 0), 1.0, 0.5 },
			{ UV(0, -1), 0.5, 0.0 },
			{ UV(0, 1), 0.5, 1.0 },
		};
		for (const auto& c : cases)
		{
			GeodesicTrace t = SurfaceGeodesic::Trace(plane, UV(0.5, 0.5), c.dir, far, opt);
			check.require(t.status == GeodesicStatus::boundary, "edge contact gives boundary status");
			const GeodesicSample& last = t.samples.back();
			check.require(std::fabs(last.parameter.U() - c.uEdge) <= 1e-10 &&
				std::fabs(last.parameter.V() - c.vEdge) <= 1e-10, "final sample sits exactly on the edge");
			check.require(last.distance > 0.0, "boundary distance is positive");
		}

		GeodesicTrace corner = SurfaceGeodesic::Trace(plane, UV(0.1, 0.1), UV(1, 1), far, opt);
		check.require(corner.status == GeodesicStatus::boundary, "corner contact gives boundary status");
		check.require(std::fabs(corner.samples.back().parameter.U() - 1.0) <= 1e-10 &&
			std::fabs(corner.samples.back().parameter.V() - 1.0) <= 1e-10,
			"corner sample lies on both edges");
		return 0;
	});

	registry.add("termination", "station-exactly-at-boundary", [](Check& check) -> int
	{
		LN_NurbsSurface plane = MakeSkewPlane(0.0, 1.0, 0.0, 1.0);
		// Direction (0,1): spatial speed sqrt(G) = sqrt(5), edge v=1 from v=0.25.
		double stop = 0.75 * std::sqrt(5.0);
		std::vector<double> distances = { 0.0, stop };
		GeodesicOptions opt;
		GeodesicTrace trace = SurfaceGeodesic::Trace(plane, UV(0.5, 0.25), UV(0, 1), distances, opt);
		check.require(trace.status == GeodesicStatus::boundary,
			"a last station exactly on the boundary still reports boundary");
		check.require(std::fabs(trace.samples.back().parameter.V() - 1.0) <= 1e-10,
			"boundary sample on the edge");
		return 0;
	});

	registry.add("termination", "zero-only-station-list", [](Check& check) -> int
	{
		LN_NurbsSurface plane = MakeSkewPlane(0.0, 1.0, 0.0, 1.0);
		GeodesicOptions opt;
		GeodesicTrace trace = SurfaceGeodesic::Trace(plane, UV(0.5, 0.5), UV(1, 0), { 0.0 }, opt);
		check.require(trace.status == GeodesicStatus::completed, "{0} completes immediately");
		check.require(trace.samples.size() == 1, "{0} publishes only the start sample");
		check.require(trace.acceptedSteps == 0 && trace.rejectedSteps == 0,
			"{0} spends no integration trials");
		return 0;
	});

	registry.add("termination", "forced-trial-exhaustion", [](Check& check) -> int
	{
		LN_NurbsSurface plane = MakeSkewPlane(0.0, 1.0, 0.0, 1.0);
		GeodesicOptions opt;
		opt.maxTrials = 5;
		std::vector<double> distances = { 0.0, 0.4, 0.8, 1.2 };
		GeodesicTrace trace = SurfaceGeodesic::Trace(plane, UV(0.1, 0.1), UV(1, 0), distances, opt);
		check.require(trace.status == GeodesicStatus::step_limit, "exhausted budget gives step_limit");
		check.require(trace.acceptedSteps + trace.rejectedSteps <= opt.maxTrials,
			"trial accounting stays within the budget");
		check.require(trace.acceptedSteps + trace.rejectedSteps > 0, "some trials were recorded");
		CheckMonotoneSamples(check, trace);
		// Every published sample corresponds to an accepted state.
		check.require(trace.samples.back().distance <= 1.2, "no unreachable station is published");
		return 0;
	});

	registry.add("termination", "affine-parameter-invariance", [](Check& check) -> int
	{
		LN_NurbsSurface base = MakeSkewPlane(0.0, 1.0, 0.0, 1.0, { 0.5 }, { 0.5 });
		LN_NurbsSurface mapped;
		const double a = 3.0, b = 2.0, c = 2.5, d = -1.0; // u' = a u + b, v' = c v + d
		NurbsSurface::Reparametrize(base, b, a + b, d, c + d, mapped);
		check.require(mapped.KnotVectorU.front() == b && mapped.KnotVectorU.back() == a + b,
			"reparametrized U domain");
		check.require(mapped.KnotVectorV.front() == d && mapped.KnotVectorV.back() == c + d,
			"reparametrized V domain");

		std::vector<double> distances = { 0.0, 0.1, 0.25, 0.45 };
		GeodesicOptions opt;
		UV dir(0.6, -0.8);
		GeodesicTrace t0 = SurfaceGeodesic::Trace(base, UV(0.4, 0.6), dir, distances, opt);
		GeodesicTrace t1 = SurfaceGeodesic::Trace(mapped,
			UV(a * 0.4 + b, c * 0.6 + d), UV(a * dir.U(), c * dir.V()), distances, opt);
		check.require(t0.samples.size() == t1.samples.size(), "affine traces align sample for sample");
		for (std::size_t i = 0; i < t0.samples.size(); ++i)
		{
			check.require(t0.samples[i].position.Distance(t1.samples[i].position) <= 1e-9,
				"spatial trace invariant under positive affine reparametrization");
			check.require(t0.samples[i].tangent.Distance(t1.samples[i].tangent) <= 1e-9,
				"spatial tangent invariant under positive affine reparametrization");
			check.require(std::fabs(t0.samples[i].distance - t1.samples[i].distance) <= 1e-12,
				"stop/station distances invariant under positive affine reparametrization");
		}
		check.require((int)t0.status == (int)t1.status, "status invariant under affine reparametrization");
		return 0;
	});

	registry.add("termination", "rejected-inputs", [](Check& check) -> int
	{
		LN_NurbsSurface good = MakeSkewPlane(0.0, 1.0, 0.0, 1.0);
		GeodesicOptions opt;
		std::vector<double> ds = { 0.0, 0.2 };
		auto traceGood = [&](const LN_NurbsSurface& s, UV st, UV dir,
			const std::vector<double>& stations, const GeodesicOptions& o)
		{
			SurfaceGeodesic::Trace(s, st, dir, stations, o);
		};

		ExpectInvalidArgument(check, "degree-1", [&] {
			LN_NurbsSurface bad = good; bad.DegreeU = 1;
			traceGood(bad, UV(.5, .5), UV(1, 0), ds, opt);
		});
		ExpectInvalidArgument(check, "degree-8", [&] {
			LN_NurbsSurface bad = good; bad.DegreeU = 8;
			traceGood(bad, UV(.5, .5), UV(1, 0), ds, opt);
		});
		ExpectInvalidArgument(check, "interior-knot-multiplicity", [&] {
			LN_NurbsSurface bad = good;
			bad.KnotVectorU = { 0,0,0,.5,.5,1,1,1 }; // multiplicity 2 at degree 2 breaks C1
			traceGood(bad, UV(.5, .5), UV(1, 0), ds, opt);
		});
		ExpectInvalidArgument(check, "knot-control-count-mismatch", [&] {
			LN_NurbsSurface bad = good; bad.KnotVectorU.push_back(1.0);
			traceGood(bad, UV(.5, .5), UV(1, 0), ds, opt);
		});
		ExpectInvalidArgument(check, "non-rectangular-net", [&] {
			LN_NurbsSurface bad = good; bad.ControlPoints[0].pop_back();
			traceGood(bad, UV(.5, .5), UV(1, 0), ds, opt);
		});
		ExpectInvalidArgument(check, "non-positive-weight", [&] {
			LN_NurbsSurface bad = good; bad.ControlPoints[0][0].W() = 0.0;
			traceGood(bad, UV(.5, .5), UV(1, 0), ds, opt);
		});
		ExpectInvalidArgument(check, "nan-weight", [&] {
			LN_NurbsSurface bad = good; bad.ControlPoints[0][0].W() =
				std::numeric_limits<double>::quiet_NaN();
			traceGood(bad, UV(.5, .5), UV(1, 0), ds, opt);
		});
		ExpectInvalidArgument(check, "start-on-boundary", [&] {
			traceGood(good, UV(0.0, .5), UV(1, 0), ds, opt);
		});
		ExpectInvalidArgument(check, "start-outside", [&] {
			traceGood(good, UV(1.2, .5), UV(1, 0), ds, opt);
		});
		ExpectInvalidArgument(check, "zero-direction", [&] {
			traceGood(good, UV(.5, .5), UV(0, 0), ds, opt);
		});
		ExpectInvalidArgument(check, "nan-direction", [&] {
			traceGood(good, UV(.5, .5),
				UV(std::numeric_limits<double>::quiet_NaN(), 1), ds, opt);
		});
		ExpectInvalidArgument(check, "empty-distances", [&] {
			traceGood(good, UV(.5, .5), UV(1, 0), {}, opt);
		});
		ExpectInvalidArgument(check, "first-distance-nonzero", [&] {
			traceGood(good, UV(.5, .5), UV(1, 0), { 0.1, 0.2 }, opt);
		});
		ExpectInvalidArgument(check, "non-increasing-distances", [&] {
			traceGood(good, UV(.5, .5), UV(1, 0), { 0.0, 0.2, 0.2 }, opt);
		});
		ExpectInvalidArgument(check, "nan-distance", [&] {
			traceGood(good, UV(.5, .5), UV(1, 0),
				{ 0.0, std::numeric_limits<double>::quiet_NaN() }, opt);
		});
		ExpectInvalidArgument(check, "zero-tolerance", [&] {
			GeodesicOptions bad = opt; bad.tolerance = 0.0;
			traceGood(good, UV(.5, .5), UV(1, 0), ds, bad);
		});
		ExpectInvalidArgument(check, "nan-maxstep", [&] {
			GeodesicOptions bad = opt;
			bad.maxStep = std::numeric_limits<double>::quiet_NaN();
			traceGood(good, UV(.5, .5), UV(1, 0), ds, bad);
		});
		ExpectInvalidArgument(check, "negative-maxstep", [&] {
			GeodesicOptions bad = opt; bad.maxStep = -0.1;
			traceGood(good, UV(.5, .5), UV(1, 0), ds, bad);
		});
		ExpectInvalidArgument(check, "zero-maxtrials", [&] {
			GeodesicOptions bad = opt; bad.maxTrials = 0;
			traceGood(good, UV(.5, .5), UV(1, 0), ds, bad);
		});
		return 0;
	});

	registry.add("termination", "degenerate-metric-runtime-error", [](Check& check) -> int
	{
		// Positive finite weights, but every u-row is identical: Su vanishes, E = 0.
		const int p = 2;
		LN_NurbsSurface bad;
		bad.DegreeU = bad.DegreeV = p;
		bad.KnotVectorU = ClampedKnots(p, {});
		bad.KnotVectorV = ClampedKnots(p, {});
		double vs[3] = { 0.0, 0.5, 1.0 };
		for (int i = 0; i <= p; ++i)
		{
			std::vector<XYZW> row;
			for (double v : vs) { row.emplace_back(XYZW(XYZ(v, 0.0, 0.0), 1.0)); }
			bad.ControlPoints.push_back(row);
		}
		GeodesicOptions opt;
		ExpectRuntimeError(check, "degenerate-metric", [&] {
			SurfaceGeodesic::Trace(bad, UV(0.5, 0.5), UV(1, 0), { 0.0, 0.2 }, opt);
		});
		return 0;
	});
}
