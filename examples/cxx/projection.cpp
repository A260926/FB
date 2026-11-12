#include <iostream>
#include "tinysplinecxx.h"

/*
 * Global point projection ("snapping") in C++.
 *
 * A ProjectionIndex finds the globally nearest curve point. Unlike uniform
 * sampling it does not miss fold-backs, and unlike BSpline::bisect it does
 * not assume a monotone coordinate. The index is a reusable, thread-safe
 * snapshot of the spline passed to its constructor.
 */
int main(int argc, char **argv)
{
	using namespace tinyspline;

	(void) argc;
	(void) argv;

	try {
		/* A cubic Bezier whose x coordinate doubles back. */
		BSpline curve(4, 2, 3, BSpline::Beziers);
		curve.setControlPoints({
			0.0, 0.0,
			10.0, 5.0,
			10.0, 15.0,
			0.0, 20.0});

		/* Build the index once; it copies the spline's data, so editing
		 * or destroying `curve` later does not affect it. */
		ProjectionIndex index(curve);

		/* epsilon is a distance in curve space, not a parameter step. */
		const real epsilon = 1e-7;
		Projection nearest = index.query({5.0, 10.0}, epsilon);

		std::cout << "nearest knot:       " << nearest.knot() << "\n";
		/* spanIndex() identifies the original knot interval. At a
		 * discontinuous (full-multiplicity) break knot it tells the
		 * left-hand one-sided limit apart from the right-hand one. */
		std::cout << "span index:         " << nearest.spanIndex()
		          << "\n";
		std::vector<real> point = nearest.point();
		std::cout << "nearest curve point:(" << point[0] << ", "
		          << point[1] << ")\n";
		/* Certified bracket of the true shortest distance. */
		std::cout << "distance bracket:   [" << nearest.distanceLower()
		          << ", " << nearest.distanceUpper() << "]\n";
		std::cout << "refined pieces:     " << nearest.visitedPieces()
		          << "\n";

		/* Repeated queries reuse the same index and stay deterministic. */
		Projection again = index.query({5.0, 10.0}, epsilon);
		std::cout << "repeat gives knot:  " << again.knot()
		          << " (deterministic)\n";
	} catch (const std::runtime_error &e) {
		std::cerr << e.what() << "\n";
		return 1;
	}
	return 0;
}
