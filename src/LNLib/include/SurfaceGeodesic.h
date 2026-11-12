/*
 * SurfaceGeodesic.h
 * Arc-length initial-value geodesic tracing on clamped tensor-product NURBS surfaces.
 *
 * Use of this source code is governed by a GPL-3.0 license that can be found in
 * the LICENSE file.
 */

#pragma once

#include "LNLibDefinitions.h"
#include "LNObject.h"
#include "UV.h"
#include "XYZ.h"

#include <cstddef>
#include <vector>

namespace LNLib
{
	enum class GeodesicStatus : int
	{
		/// Every requested distance station was reached while the trace stayed
		/// strictly inside the valid parameter rectangle.
		completed = 0,
		/// The trace touched the outer boundary of the parameter rectangle;
		/// the final sample sits exactly on the first edge (or corner) met.
		boundary = 1,
		/// The accepted/rejected integration trial budget (maxTrials) was
		/// exhausted; only accepted states are published.
		step_limit = 2,
	};

	struct LNLIB_EXPORT GeodesicOptions
	{
		/// Local error tolerance for spatial position and length-scaled tangent.
		double tolerance = 1e-8;
		/// Upper bound on the spatial arc length of an accepted step.
		double maxStep = 0.1;
		/// Maximum number of integration trials (accepted plus rejected).
		std::size_t maxTrials = 100000;
	};

	struct LNLIB_EXPORT GeodesicSample
	{
		/// Cumulative spatial arc length travelled from the start point.
		double distance = 0.0;
		/// Surface parameter of the sample.
		UV parameter;
		/// Spatial position S(parameter).
		XYZ position;
		/// Unit spatial tangent of the trace at the sample.
		XYZ tangent;
	};

	struct LNLIB_EXPORT GeodesicTrace
	{
		GeodesicStatus status = GeodesicStatus::completed;
		std::vector<GeodesicSample> samples;
		std::size_t acceptedSteps = 0;
		std::size_t rejectedSteps = 0;
	};

	class LNLIB_EXPORT SurfaceGeodesic
	{
	public:
		/// <summary>
		/// Trace a geodesic by integrating the geodesic ODE in arc length on
		/// the surface metric. The initial UV direction is normalized to unit
		/// spatial speed through the first fundamental form, so scaling it by
		/// any positive factor produces the same trace.
		///
		/// The surface must be a clamped tensor-product NURBS surface with
		/// degrees 2..7, strictly positive finite weights, a rectangular
		/// control net matching the knot vectors, positive valid domain width
		/// in both directions and interior knot multiplicities at most
		/// degree-1 (at least C1). The surface data is never modified.
		/// </summary>
		/// <param name="surface">Clamped NURBS surface, read only.</param>
		/// <param name="start">Start parameter, strictly inside the valid rectangle.</param>
		/// <param name="direction">Initial UV velocity in the original parameter domain.</param>
		/// <param name="distances">Cumulative arc-length stations: non-empty,
		/// first value 0, remaining values finite and strictly increasing.</param>
		/// <param name="options">Tolerance, maximum accepted step and trial budget.</param>
		static GeodesicTrace Trace(const LN_NurbsSurface& surface,
			UV start,
			UV direction,
			const std::vector<double>& distances,
			const GeodesicOptions& options);
	};
}
