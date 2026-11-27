#include <iostream>
#include <iomanip>
#include "tinysplinecxx.h"

/*
 * Three dimensional rail intersection example.
 *
 * Two guide rails are modeled as cubic B-spline curves in space.  The
 * example first shows an ordinary transverse intersection, then a query that
 * cannot be finished inside a deliberately tiny box budget and how the
 * caller must treat the resulting pending boxes as a normal (not empty)
 * result.
 */
int
main()
{
	using tinyspline::BSpline;
	using tinyspline::Intersection;
	using tinyspline::real;

	std::cout << std::fixed << std::setprecision(4);

	/* Rail A: a cubic guide rail in space. */
	BSpline railA(4, 3, 3, BSpline::Clamped);
	railA.setControlPoints({
	    0.0, 0.0, 0.0,
	    2.0, 4.0, 1.0,
	    4.0, -2.0, 1.0,
	    6.0, 2.0, 0.0
	});

	/* Rail B: a straight transverse rail that passes through the midpoint
	 * of rail A, so the two meet at exactly one known 3D point. */
	const auto atMid = railA.eval(0.5).result();
	BSpline railB(2, 3, 1, BSpline::Clamped);
	railB.setControlPoints({
	    atMid[0] - 2.0, atMid[1] + 2.0, atMid[2],
	    atMid[0] + 2.0, atMid[1] - 2.0, atMid[2]
	});

	const real spatialTol = 1e-6;
	const real paramTol = 1e-6;

	Intersection result = railA.intersect(
	    railB, spatialTol, paramTol, paramTol);

	std::cout << "completed query: complete=" << std::boolalpha
	          << result.isComplete()
	          << ", boxes expanded=" << result.boxesExpanded()
	          << "\n";
	std::cout << "isolated intersections: "
	          << result.points().size() << "\n";
	for (const auto &p : result.points()) {
		const auto pos = p.point();
		std::cout << "  u=" << p.u() << " v=" << p.v()
		          << " at (" << pos.x() << ", " << pos.y()
		          << ", " << pos.z() << ")"
		          << " original intervals [" << p.uIndex()
		          << ", " << p.vIndex() << "]\n";
		/* All parameters live on the splines' own knot domains. */
	}
	std::cout << "coincident segments: "
	          << result.overlaps().size() << "\n";
	for (const auto &o : result.overlaps())
		std::cout << "  v = " << o.alpha() << " * u + "
		          << o.beta() << " on u in [" << o.uFrom()
		          << ", " << o.uTo() << "]\n";

	/* A budget that is too small to finish the same query.  The pending
	 * boxes are part of the answer: the intersection is not empty. */
	Intersection pending = railA.intersect(
	    railB, spatialTol, paramTol, paramTol, 4);
	std::cout << "\nbudget limited query: complete="
	          << pending.isComplete()
	          << ", pending boxes=" << pending.boxes().size()
	          << "\n";
	if (!pending.isComplete()) {
		for (const auto &box : pending.boxes()) {
			std::cout << "  unresolved u in [" << box.u0()
			          << ", " << box.u1() << "], v in ["
			          << box.v0() << ", " << box.v1()
			          << "], reason=" << box.reason() << "\n";
		}
		std::cout << "  (retry with a larger budget instead of "
		             "treating this as an empty intersection)\n";
	}

	return 0;
}
