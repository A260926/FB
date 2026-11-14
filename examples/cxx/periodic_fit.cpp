/*
 * Fits a closed, periodic cubic spline to a set of 3D points.
 *
 * This example shows how to use BSpline::fitPeriodicCubic. In contrast to
 * interpolateCubicNatural and interpolateCatmullRom, the fitted spline is
 * periodic: its position as well as its first and second derivative are
 * continuous at the seam. Repeating the first point is neither necessary nor
 * allowed.
 *
 * Build and run this example with CMake (the target is named
 * `periodic_fit_cxx') and it will print the control points, the knots, and a
 * few evaluated points of the resulting spline.
 */

#include <cmath>
#include <cstdio>
#include <vector>

#include "tinysplinecxx.h"

int main()
{
	/* The points of a closed profile (the first point is not repeated).
	 * Each point is made up of three components (x, y, z). */
	std::vector<tinyspline::real> points = {
		0.0,  0.0, 0.0,
		1.0,  0.4, -0.2,
		1.8,  1.3,  0.5,
		1.2,  2.6,  1.4,
		-0.3, 2.9,  2.2,
		-1.4, 1.7,  2.8,
		-0.8, 0.4,  2.1,
		0.2, -0.5,  1.2
	};
	/* One parameter per point plus one value that specifies the end of
	 * the period. The parameters are arbitrary and need not be uniform;
	 * they are not normalized. */
	std::vector<tinyspline::real> parameters = {
		0.0, 0.43, 1.17, 1.86, 2.71, 3.52, 4.60, 5.81, 6.95
	};
	/* Non-negative weights. A weight of 0 excludes the corresponding
	 * point from the error term (the point's parameter is still a knot,
	 * however). */
	std::vector<tinyspline::real> weights = {
		1.0, 2.0, 0.3, 1.0, 0.0, 1.5, 0.8, 1.2
	};
	/* Hard constraints: the sample is satisfied exactly. Here, the
	 * second and the last point are hard. */
	std::vector<int> hard = { 0, 0, 1, 0, 0, 0, 0, 1 };
	const tinyspline::real lambda = 0.7;

	tinyspline::BSpline spline = tinyspline::BSpline::fitPeriodicCubic(
		points, 3, parameters, weights, hard, lambda);

	std::printf("degree:             %zu\n", (size_t) spline.degree());
	std::printf("dimension:          %zu\n", spline.dimension());
	std::printf("num control points: %zu\n",
	            spline.numControlPoints());
	std::printf("num knots:          %zu\n", spline.knots().size());
	std::printf("domain:             [%f, %f]\n",
	            (double) spline.domain().min(),
	            (double) spline.domain().max());

	std::printf("\ncontrol points:\n");
	std::vector<tinyspline::real> controlPoints =
		spline.controlPoints();
	for (size_t i = 0; i < spline.numControlPoints(); ++i) {
		std::printf("  %2zu: (%f, %f, %f)\n", i,
		            (double) controlPoints[i * 3 + 0],
		            (double) controlPoints[i * 3 + 1],
		            (double) controlPoints[i * 3 + 2]);
	}

	std::printf("\nsamples:\n");
	for (size_t i = 0; i < parameters.size() - 1; ++i) {
		std::vector<tinyspline::real> result =
			spline.eval(parameters[i]).result();
		std::printf("  t=%f -> (%f, %f, %f)%s\n",
		            (double) parameters[i],
		            (double) result[0],
		            (double) result[1],
		            (double) result[2],
		            hard[i] ? "  (hard)" : "");
	}

	/* The seam is C2-continuous. */
	const double t0 = parameters.front();
	const double tm = parameters.back();
	std::printf("\nseam continuity:\n");
	for (size_t order = 0; order <= 2; ++order) {
		tinyspline::BSpline deriv = order == 0
			? spline
			: spline.derive(order);
		std::vector<tinyspline::real> a =
			deriv.eval((tinyspline::real) t0).result();
		std::vector<tinyspline::real> b =
			deriv.eval((tinyspline::real) tm).result();
		double gap = 0.0;
		for (size_t d = 0; d < a.size(); ++d)
			gap += (a[d] - b[d]) * (a[d] - b[d]);
		std::printf("  derivative %zu: gap = %e\n", order,
		            std::sqrt(gap));
	}

	return 0;
}
