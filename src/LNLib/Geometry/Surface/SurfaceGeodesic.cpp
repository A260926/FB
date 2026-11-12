/*
 * SurfaceGeodesic.cpp
 * Arc-length initial-value geodesic tracing on clamped tensor-product NURBS surfaces.
 *
 * Use of this source code is governed by a GPL-3.0 license that can be found in
 * the LICENSE file.
 */

#include "SurfaceGeodesic.h"

#include "NurbsSurface.h"
#include "LNObject.h"
#include "UV.h"
#include "XYZ.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace LNLib
{
	namespace
	{
		constexpr double DegeneracyRatio = 1e-12;
		constexpr int MaxRootIterations = 64;
		constexpr double SideShiftScale = 1e-12;
		constexpr double SnapScale = 1e-12;

		void RequireArgument(bool condition, const char* argument, const char* message)
		{
			if (!condition)
			{
				throw std::invalid_argument(std::string(message) + "\r\nParameter name: " + argument);
			}
		}

		bool AllFinite(const XYZW& p)
		{
			return std::isfinite(p.GetWX()) && std::isfinite(p.GetWY()) &&
				std::isfinite(p.GetWZ()) && std::isfinite(p.GetW());
		}

		/// One-sided second-order frame of the surface at a parameter.
		/// sideU/sideV select the knot element at knot lines: -1 left, 0 exact, +1 right.
		struct Frame
		{
			XYZ S;
			XYZ Su;
			XYZ Sv;
			XYZ Suu;
			XYZ Suv;
			XYZ Svv;
			double E = 0.0;
			double F = 0.0;
			double G = 0.0;
			double Guu = 0.0, Guv = 0.0, Gvv = 0.0; // Christoffel Gamma^u_**
			double Vuu = 0.0, Vuv = 0.0, Vvv = 0.0; // Christoffel Gamma^v_**
		};

		struct TraceEngine
		{
			const LN_NurbsSurface& Surface;
			GeodesicOptions Options;

			std::vector<double> KnotsU; // distinct knots, including domain ends
			std::vector<double> KnotsV;
			double UMin = 0.0, UMax = 0.0, VMin = 0.0, VMax = 0.0;
			double BandU = 0.0, BandV = 0.0;

			int CellU = 0; // KnotsU[CellU] <= u <= KnotsU[CellU+1]
			int CellV = 0;

			TraceEngine(const LN_NurbsSurface& surface, const GeodesicOptions& options)
				: Surface(surface), Options(options) {}

			Frame Evaluate(UV uv, int sideU, int sideV) const
			{
				double u = uv.GetU();
				double v = uv.GetV();

				// Shift onto the requested one-sided element when sitting on a knot line.
				if (sideU != 0)
				{
					int idx = sideU > 0 ? CellU : CellU - 1;
					idx = std::min(std::max(idx, 0), (int)KnotsU.size() - 2);
					double width = KnotsU[idx + 1] - KnotsU[idx];
					double delta = std::min(SideShiftScale * std::max(1.0, std::fabs(width)),
						std::fabs(width) * 0.5);
					u += sideU > 0 ? delta : -delta;
					u = std::min(std::max(u, UMin), UMax);
				}
				if (sideV != 0)
				{
					int idx = sideV > 0 ? CellV : CellV - 1;
					idx = std::min(std::max(idx, 0), (int)KnotsV.size() - 2);
					double width = KnotsV[idx + 1] - KnotsV[idx];
					double delta = std::min(SideShiftScale * std::max(1.0, std::fabs(width)),
						std::fabs(width) * 0.5);
					v += sideV > 0 ? delta : -delta;
					v = std::min(std::max(v, VMin), VMax);
				}

				std::vector<std::vector<XYZ>> d =
					NurbsSurface::ComputeRationalSurfaceDerivatives(Surface, 2, UV(u, v));

				Frame fr;
				fr.S = d[0][0];
				fr.Su = d[1][0];
				fr.Sv = d[0][1];
				fr.Suu = d[2][0];
				fr.Suv = d[1][1];
				fr.Svv = d[0][2];

				fr.E = fr.Su.DotProduct(fr.Su);
				fr.F = fr.Su.DotProduct(fr.Sv);
				fr.G = fr.Sv.DotProduct(fr.Sv);

				if (!std::isfinite(fr.E) || !std::isfinite(fr.F) || !std::isfinite(fr.G) ||
					fr.E <= 0.0 || fr.G <= 0.0)
				{
					throw std::runtime_error("SurfaceGeodesic::Trace encountered a degenerate surface metric (E or G non-positive).");
				}
				double det = fr.E * fr.G - fr.F * fr.F;
				if (!std::isfinite(det) || det / (fr.E * fr.G) <= DegeneracyRatio)
				{
					throw std::runtime_error("SurfaceGeodesic::Trace encountered a degenerate surface metric (EG-F^2 non-positive).");
				}

				// Derivatives of the metric components from the second surface derivatives.
				double Eu = 2.0 * fr.Suu.DotProduct(fr.Su);
				double Ev = 2.0 * fr.Suv.DotProduct(fr.Su);
				double Fu = fr.Suu.DotProduct(fr.Sv) + fr.Su.DotProduct(fr.Suv);
				double Fv = fr.Suv.DotProduct(fr.Sv) + fr.Su.DotProduct(fr.Svv);
				double Gu = 2.0 * fr.Suv.DotProduct(fr.Sv);
				double Gv = 2.0 * fr.Svv.DotProduct(fr.Sv);

				double inv2Det = 0.5 / det;
				fr.Guu = (fr.G * Eu - fr.F * (2.0 * Fu - Ev)) * inv2Det;
				fr.Guv = (fr.G * Ev - fr.F * Gu) * inv2Det;
				fr.Gvv = (fr.G * (2.0 * Fv - Gu) - fr.F * Gv) * inv2Det;
				fr.Vuu = (fr.E * (2.0 * Fu - Ev) - fr.F * Eu) * inv2Det;
				fr.Vuv = (fr.E * Gu - fr.F * Ev) * inv2Det;
				fr.Vvv = (fr.E * Gv - fr.F * (2.0 * Fv - Gu)) * inv2Det;
				return fr;
			}

			/// Select one-sided elements when the state sits on a knot line.
			void SideAt(double u, double v, int& sideU, int& sideV) const
			{
				sideU = 0;
				sideV = 0;
				if (u <= KnotsU[CellU] + BandU) { sideU = +1; }
				else if (u >= KnotsU[CellU + 1] - BandU) { sideU = -1; }
				if (v <= KnotsV[CellV] + BandV) { sideV = +1; }
				else if (v >= KnotsV[CellV + 1] - BandV) { sideV = -1; }
			}

			bool InsideDomain(const Eigen::Vector4d& y) const
			{
				return std::isfinite(y[0]) && std::isfinite(y[1]) &&
					y[0] >= UMin && y[0] <= UMax &&
					y[1] >= VMin && y[1] <= VMax;
			}

			/// Closed parameter interval of the current knot element, exact bounds.
			bool InsideCell(const Eigen::Vector4d& y) const
			{
				return y[0] >= KnotsU[CellU] && y[0] <= KnotsU[CellU + 1] &&
					y[1] >= KnotsV[CellV] && y[1] <= KnotsV[CellV + 1];
			}

			Eigen::Vector4d RHS(const Eigen::Vector4d& y, int sideU, int sideV) const
			{
				Frame fr = Evaluate(UV(y[0], y[1]), sideU, sideV);
				Eigen::Vector2d q(y[2], y[3]);
				Eigen::Matrix2d metric;
				metric << fr.E, fr.F, fr.F, fr.G;
				double speed = std::sqrt(q.transpose() * metric * q);
				if (!std::isfinite(speed) || speed <= 0.0)
				{
					throw std::runtime_error("SurfaceGeodesic::Trace lost the integration direction (zero progress).");
				}
				q /= speed; // arc-length parametrization: unit spatial speed
				double up = q[0];
				double vp = q[1];
				Eigen::Vector4d rhs;
				rhs[0] = up;
				rhs[1] = vp;
				rhs[2] = -(fr.Guu * up * up + 2.0 * fr.Guv * up * vp + fr.Gvv * vp * vp);
				rhs[3] = -(fr.Vuu * up * up + 2.0 * fr.Vuv * up * vp + fr.Vvv * vp * vp);
				return rhs;
			}
		};

		/// Runge-Kutta-Fehlberg 4(5) trial step. Sets aborted when an intermediate
		/// stage leaves the parameter domain, so no out-of-range evaluator call occurs.
		bool Rkf45Trial(const TraceEngine& engine, const Eigen::Vector4d& y0, double h,
			int sideU, int sideV, Eigen::Vector4d& y4, Eigen::Vector4d& y5, bool& aborted)
		{
			aborted = false;
			auto rhsAt = [&](const Eigen::Vector4d& stage) -> Eigen::Vector4d
			{
				if (!engine.InsideDomain(stage))
				{
					aborted = true;
					return Eigen::Vector4d::Zero();
				}
				return engine.RHS(stage, sideU, sideV);
			};

			Eigen::Vector4d k1 = engine.RHS(y0, sideU, sideV);
			Eigen::Vector4d k2 = rhsAt(y0 + h * (1.0 / 4.0) * k1);
			if (aborted) return false;
			Eigen::Vector4d k3 = rhsAt(y0 + h * ((3.0 / 32.0) * k1 + (9.0 / 32.0) * k2));
			if (aborted) return false;
			Eigen::Vector4d k4 = rhsAt(y0 + h * ((1932.0 / 2197.0) * k1 - (7200.0 / 2197.0) * k2 + (7296.0 / 2197.0) * k3));
			if (aborted) return false;
			Eigen::Vector4d k5 = rhsAt(y0 + h * ((439.0 / 216.0) * k1 - 8.0 * k2 + (3680.0 / 513.0) * k3 - (845.0 / 4104.0) * k4));
			if (aborted) return false;
			Eigen::Vector4d k6 = rhsAt(y0 + h * (-(8.0 / 27.0) * k1 + 2.0 * k2 - (3544.0 / 2565.0) * k3 + (1859.0 / 4104.0) * k4 - (11.0 / 40.0) * k5));
			if (aborted) return false;

			y4 = y0 + h * ((25.0 / 216.0) * k1 + (1408.0 / 2565.0) * k3 +
				(2197.0 / 4104.0) * k4 - (1.0 / 5.0) * k5);
			y5 = y0 + h * ((16.0 / 135.0) * k1 + (6656.0 / 12825.0) * k3 +
				(28561.0 / 56430.0) * k4 - (9.0 / 50.0) * k5 + (2.0 / 55.0) * k6);
			if (!engine.InsideDomain(y5) || !engine.InsideDomain(y4))
			{
				aborted = true;
				return false;
			}
			return true;
		}
	}

	GeodesicTrace SurfaceGeodesic::Trace(const LN_NurbsSurface& surface,
		UV start,
		UV direction,
		const std::vector<double>& distances,
		const GeodesicOptions& options)
	{
		// ----- Structural validation, before any legacy evaluator is touched. -----
		RequireArgument(std::isfinite(options.tolerance) && options.tolerance > 0.0,
			"options.tolerance", "tolerance must be finite and positive.");
		RequireArgument(std::isfinite(options.maxStep) && options.maxStep > 0.0,
			"options.maxStep", "maxStep must be finite and positive.");
		RequireArgument(options.maxTrials != 0, "options.maxTrials", "maxTrials must be positive.");

		RequireArgument(surface.DegreeU >= 2 && surface.DegreeU <= 7,
			"surface.DegreeU", "DegreeU must be between 2 and 7 inclusive.");
		RequireArgument(surface.DegreeV >= 2 && surface.DegreeV <= 7,
			"surface.DegreeV", "DegreeV must be between 2 and 7 inclusive.");

		const std::vector<double>& ku = surface.KnotVectorU;
		const std::vector<double>& kv = surface.KnotVectorV;
		const auto& cp = surface.ControlPoints;

		RequireArgument(ku.size() >= static_cast<std::size_t>(2 * (surface.DegreeU + 1)),
			"surface.KnotVectorU", "KnotVectorU must be a clamped knot vector.");
		RequireArgument(kv.size() >= static_cast<std::size_t>(2 * (surface.DegreeV + 1)),
			"surface.KnotVectorV", "KnotVectorV must be a clamped knot vector.");

		RequireArgument(std::isfinite(ku.front()) && std::isfinite(ku.back()) && ku.back() > ku.front(),
			"surface.KnotVectorU", "U domain must have finite, positive width.");
		RequireArgument(std::isfinite(kv.front()) && std::isfinite(kv.back()) && kv.back() > kv.front(),
			"surface.KnotVectorV", "V domain must have finite, positive width.");

		RequireArgument(!cp.empty(), "surface.ControlPoints", "Control net must be non-empty.");
		std::size_t columns = cp[0].size();
		RequireArgument(columns > 0, "surface.ControlPoints", "Control net must be rectangular.");
		for (const auto& row : cp)
		{
			RequireArgument(row.size() == columns, "surface.ControlPoints", "Control net must be rectangular.");
			for (const XYZW& p : row)
			{
				RequireArgument(AllFinite(p), "surface.ControlPoints",
					"Homogeneous control coordinates must be finite.");
				RequireArgument(p.GetW() > 0.0, "surface.ControlPoints",
					"Weights must be strictly positive.");
			}
		}
		RequireArgument(static_cast<int>(ku.size()) == static_cast<int>(cp.size()) + surface.DegreeU + 1,
			"surface.KnotVectorU", "Knot count must match the control net (m = n + p + 1).");
		RequireArgument(static_cast<int>(kv.size()) == static_cast<int>(columns) + surface.DegreeV + 1,
			"surface.KnotVectorV", "Knot count must match the control net (m = n + p + 1).");

		auto checkKnots = [](const std::vector<double>& knots, int degree, const char* name)
		{
			for (double k : knots)
			{
				RequireArgument(std::isfinite(k), name, "Knots must be finite.");
			}
			for (std::size_t i = 1; i < knots.size(); ++i)
			{
				RequireArgument(knots[i] >= knots[i - 1], name,
					"Knot vector must be nondecreasing.");
			}
			int endMultiplicity = 0;
			while (endMultiplicity < static_cast<int>(knots.size()) &&
				knots[endMultiplicity] == knots.front()) { ++endMultiplicity; }
			RequireArgument(endMultiplicity == degree + 1, name,
				"Surface must be clamped (end knot multiplicity degree+1).");
			int tailMultiplicity = 0;
			while (tailMultiplicity < static_cast<int>(knots.size()) &&
				knots[knots.size() - 1 - tailMultiplicity] == knots.back()) { ++tailMultiplicity; }
			RequireArgument(tailMultiplicity == degree + 1, name,
				"Surface must be clamped (end knot multiplicity degree+1).");
			for (std::size_t i = degree + 1; i < knots.size() - (degree + 1);)
			{
				std::size_t j = i;
				while (j < knots.size() && knots[j] == knots[i]) { ++j; }
				int multiplicity = static_cast<int>(j - i);
				RequireArgument(multiplicity <= degree - 1, name,
					"Interior knot multiplicity must not exceed degree-1 (at least C1).");
				i = j;
			}
		};
		checkKnots(ku, surface.DegreeU, "surface.KnotVectorU");
		checkKnots(kv, surface.DegreeV, "surface.KnotVectorV");

		RequireArgument(std::isfinite(start.GetU()) && std::isfinite(start.GetV()),
			"start", "Start parameter must be finite.");
		RequireArgument(start.GetU() > ku.front() && start.GetU() < ku.back(),
			"start", "Start must lie strictly inside the valid U domain.");
		RequireArgument(start.GetV() > kv.front() && start.GetV() < kv.back(),
			"start", "Start must lie strictly inside the valid V domain.");

		RequireArgument(std::isfinite(direction.GetU()) && std::isfinite(direction.GetV()),
			"direction", "Direction must be finite.");
		RequireArgument(direction.GetU() != 0.0 || direction.GetV() != 0.0,
			"direction", "Direction must be non-zero.");

		RequireArgument(!distances.empty(), "distances", "Distances must be non-empty.");
		RequireArgument(distances[0] == 0.0, "distances", "First distance must be zero.");
		for (std::size_t i = 1; i < distances.size(); ++i)
		{
			RequireArgument(std::isfinite(distances[i]), "distances", "Distances must be finite.");
			RequireArgument(distances[i] > distances[i - 1], "distances",
				"Distances must be strictly increasing.");
		}

		GeodesicTrace trace;
		TraceEngine engine(surface, options);
		engine.UMin = ku.front();
		engine.UMax = ku.back();
		engine.VMin = kv.front();
		engine.VMax = kv.back();
		engine.KnotsU.assign(ku.begin(), ku.end());
		engine.KnotsU.erase(std::unique(engine.KnotsU.begin(), engine.KnotsU.end()), engine.KnotsU.end());
		engine.KnotsV.assign(kv.begin(), kv.end());
		engine.KnotsV.erase(std::unique(engine.KnotsV.begin(), engine.KnotsV.end()), engine.KnotsV.end());
		engine.BandU = SnapScale * std::max(1.0, engine.UMax - engine.UMin);
		engine.BandV = SnapScale * std::max(1.0, engine.VMax - engine.VMin);

		// Initial cell (start may sit on an interior knot line). When it does, pick
		// the element the initial direction leaves into, so one-sided second
		// derivatives are taken on the correct side of the C1 junction.
		auto initialCell = [](const std::vector<double>& knots, double value,
			double band, double dir) -> int
		{
			int right = static_cast<int>(std::upper_bound(knots.begin(), knots.end(), value)
				- knots.begin()) - 1;
			right = std::min(std::max(right, 0), static_cast<int>(knots.size()) - 2);
			if (right > 0 && std::fabs(value - knots[right]) <= band && dir < 0.0)
			{
				return right - 1;
			}
			return right;
		};
		engine.CellU = initialCell(engine.KnotsU, start.GetU(), engine.BandU, direction.GetU());
		engine.CellV = initialCell(engine.KnotsV, start.GetV(), engine.BandV, direction.GetV());

		Frame initial = engine.Evaluate(start, 0, 0);
		Eigen::Matrix2d metric;
		metric << initial.E, initial.F, initial.F, initial.G;
		Eigen::Vector2d d0(direction.GetU(), direction.GetV());
		double initialSpeed2 = d0.transpose() * metric * d0;
		if (!std::isfinite(initialSpeed2) || initialSpeed2 <= 0.0)
		{
			throw std::runtime_error("SurfaceGeodesic::Trace encountered a degenerate initial metric.");
		}
		d0 /= std::sqrt(initialSpeed2); // unit spatial speed in the original UV domain

		Eigen::Vector4d state(start.GetU(), start.GetV(), d0[0], d0[1]);
		double s = 0.0;
		std::size_t trials = 0;
		std::size_t nextStation = 1;
		int zeroProgressEvents = 0;

		auto distanceGuard = [&](double distance)
		{
			return SnapScale * std::max(1.0, std::fabs(distance));
		};
		auto makeSample = [&](double distance, const Eigen::Vector4d& y) -> GeodesicSample
		{
			UV uv(y[0], y[1]);
			Frame fr = engine.Evaluate(uv, 0, 0);
			GeodesicSample sample;
			sample.distance = distance;
			sample.parameter = uv;
			sample.position = fr.S;
			sample.tangent = (fr.Su * y[2] + fr.Sv * y[3]).Normalize();
			return sample;
		};
		auto emitSample = [&](const GeodesicSample& sample)
		{
			if (!trace.samples.empty() &&
				sample.distance <= trace.samples.back().distance + distanceGuard(sample.distance))
			{
				return; // coincident station/event/stop: keep the first publication
			}
			trace.samples.push_back(sample);
		};

		// Snap a state lying on a cell edge to the exact knot, move into the next
		// element in the velocity direction, and report outer-boundary contact.
		// Interior edges require an outward velocity sign, so grazing/along-line
		// motion never re-publishes the same event or gets stuck.
		auto snapEdges = [&](Eigen::Vector4d& y, bool& interiorEvent) -> bool
		{
			bool boundary = false;
			interiorEvent = false;
			double loU = engine.KnotsU[engine.CellU];
			double hiU = engine.KnotsU[engine.CellU + 1];
			double loV = engine.KnotsV[engine.CellV];
			double hiV = engine.KnotsV[engine.CellV + 1];

			if (y[0] >= hiU - engine.BandU)
			{
				y[0] = hiU;
				if (engine.CellU == static_cast<int>(engine.KnotsU.size()) - 2) { boundary = true; }
				else if (y[2] > 0.0) { ++engine.CellU; interiorEvent = true; }
			}
			else if (y[0] <= loU + engine.BandU)
			{
				y[0] = loU;
				if (engine.CellU == 0) { boundary = true; }
				else if (y[2] < 0.0) { --engine.CellU; interiorEvent = true; }
			}
			if (y[1] >= hiV - engine.BandV)
			{
				y[1] = hiV;
				if (engine.CellV == static_cast<int>(engine.KnotsV.size()) - 2) { boundary = true; }
				else if (y[3] > 0.0) { ++engine.CellV; interiorEvent = true; }
			}
			else if (y[1] <= loV + engine.BandV)
			{
				y[1] = loV;
				if (engine.CellV == 0) { boundary = true; }
				else if (y[3] < 0.0) { --engine.CellV; interiorEvent = true; }
			}
			return boundary;
		};

		emitSample(makeSample(0.0, state));

		bool finished = distances.size() == 1;
		bool hitBoundary = false;
		double h = std::min(options.maxStep,
			distances.size() > 1 ? distances[1] : options.maxStep);

		while (!finished)
		{
			double remainingStation = (nextStation < distances.size())
				? distances[nextStation] - s
				: options.maxStep;
			double hTry = std::min(h, std::min(options.maxStep, remainingStation));
			if (!(hTry > 0.0))
			{
				throw std::runtime_error("SurfaceGeodesic::Trace cannot make positive progress.");
			}

			if (trials >= options.maxTrials)
			{
				trace.status = GeodesicStatus::step_limit;
				break;
			}
			++trials;

			int sideU = 0, sideV = 0;
			engine.SideAt(state[0], state[1], sideU, sideV);

			Eigen::Vector4d y4, y5;
			bool aborted = false;
			Rkf45Trial(engine, state, hTry, sideU, sideV, y4, y5, aborted);

			bool crossesEvent = aborted || !engine.InsideCell(y5);
			if (crossesEvent)
			{
				// The crossing trial is rejected; its candidate is never published.
				++trace.rejectedSteps;

				// Bounded bisection on the step length to land on the first knot/edge event.
				// These root iterations are geometry localization, not integration trials.
				double lo = 0.0;
				double hi = hTry;
				for (int iter = 0; iter < MaxRootIterations; ++iter)
				{
					double mid = 0.5 * (lo + hi);
					if (mid == lo || mid == hi) { break; }
					Eigen::Vector4d a4, a5;
					bool aAbort = false;
					Rkf45Trial(engine, state, mid, sideU, sideV, a4, a5, aAbort);
					if (aAbort || !engine.InsideCell(a5))
					{
						hi = mid;
					}
					else
					{
						lo = mid;
					}
				}

				if (lo <= 0.0)
				{
					++zeroProgressEvents;
					if (zeroProgressEvents >= 2)
					{
						throw std::runtime_error("SurfaceGeodesic::Trace cannot make positive progress at a knot event.");
					}
				}
				else
				{
					zeroProgressEvents = 0;
				}

				if (trials >= options.maxTrials)
				{
					trace.status = GeodesicStatus::step_limit;
					break;
				}
				++trials; // shortened step ending on the event is an accepted integration trial
				Eigen::Vector4d e4, e5;
				bool eAbort = false;
				Rkf45Trial(engine, state, lo, sideU, sideV, e4, e5, eAbort);
				if (eAbort)
				{
					throw std::runtime_error("SurfaceGeodesic::Trace event localization failed to converge.");
				}
				state = e5;
				s += lo;

				// Snap to the reached edge(s); simultaneous U and V crossings are handled together.
				bool interiorEvent = false;
				hitBoundary = snapEdges(state, interiorEvent);

				++trace.acceptedSteps;

				// A pending station can only coincide with this event distance.
				while (nextStation < distances.size() &&
					distances[nextStation] <= s + distanceGuard(s))
				{
					emitSample(makeSample(distances[nextStation], state));
					++nextStation;
				}

				if (hitBoundary)
				{
					trace.status = GeodesicStatus::boundary;
					emitSample(makeSample(s, state));
					finished = true;
				}
				else if (interiorEvent)
				{
					emitSample(makeSample(s, state)); // interior knot crossing (suppressed if coincident)
				}
				h = std::min(options.maxStep, std::max(hTry, std::max(lo, hTry * 0.5)));
				continue;
			}

			// Spatial error control: position error and length-scaled unit tangent error.
			Frame endFrame = engine.Evaluate(UV(y5[0], y5[1]), sideU, sideV);
			XYZ positionError = endFrame.Su * (y5[0] - y4[0]) + endFrame.Sv * (y5[1] - y4[1]);
			XYZ tangent4 = (endFrame.Su * y4[2] + endFrame.Sv * y4[3]).Normalize();
			XYZ tangent5 = (endFrame.Su * y5[2] + endFrame.Sv * y5[3]).Normalize();
			double errPos = positionError.Length();
			double errTangent = (tangent5 - tangent4).Length();
			double errMetric = std::max(errPos / options.tolerance,
				(errTangent * hTry) / options.tolerance);
			if (!std::isfinite(errMetric))
			{
				throw std::runtime_error("SurfaceGeodesic::Trace produced a non-finite error estimate.");
			}

			if (errMetric > 1.0)
			{
				++trace.rejectedSteps;
				double shrink = 0.9 * std::pow(errMetric, -0.25);
				shrink = std::min(std::max(shrink, 0.1), 0.5);
				h = hTry * shrink;
				double minProgress = SnapScale * std::max(1.0, std::max(engine.UMax - engine.UMin,
					engine.VMax - engine.VMin));
				if (h <= minProgress * 1e-3)
				{
					throw std::runtime_error("SurfaceGeodesic::Trace cannot make positive progress.");
				}
				continue;
			}

			// Accepted trial.
			++trace.acceptedSteps;
			state = y5;
			s += hTry;
			double growth = errMetric > 0.0 ? 0.9 * std::pow(errMetric, -0.25) : 5.0;
			growth = std::min(std::max(growth, 0.2), 5.0);
			h = std::min(options.maxStep, std::max(hTry, hTry * growth));

			// A step may land exactly on a cell edge; classify it in the velocity direction.
			bool interiorEvent = false;
			bool boundaryNow = snapEdges(state, interiorEvent);

			while (nextStation < distances.size() &&
				distances[nextStation] <= s + distanceGuard(s))
			{
				emitSample(makeSample(distances[nextStation], state));
				++nextStation;
			}

			if (boundaryNow)
			{
				trace.status = GeodesicStatus::boundary;
				emitSample(makeSample(s, state));
				finished = true;
			}
			else
			{
				if (interiorEvent)
				{
					emitSample(makeSample(s, state));
				}
				if (nextStation >= distances.size())
				{
					trace.status = GeodesicStatus::completed;
					finished = true;
				}
			}
		}

		// Ensure the last accepted state is represented without duplicating a distance.
		if (trace.status == GeodesicStatus::step_limit)
		{
			emitSample(makeSample(s, state));
		}
		return trace;
	}
}
