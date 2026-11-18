/**
 * @file
 * @brief Three-dimensional guide-rail intersection example.
 *
 * Two clamped cubic B-spline curves model the rails of a 3D guide. The
 * example runs the intersection query, prints isolated crossings and
 * coincident parameter ranges, and shows how an unresolved (incomplete)
 * result is handled through the returned boxes instead of being mistaken for
 * an empty intersection.
 */
#include <tinysplinecxx.h>

#include <cstdio>

using namespace tinyspline;

int main()
{
	/* Two cubic rails in 3D that cross once near their middles. */
	BSpline railA(4, 3, 3, BSpline::Clamped);
	railA.setControlPoints({
		0, 0, 0,
		1, 2, 1,
		3, 2, 1,
		4, 0, 0
	});
	BSpline railB(4, 3, 3, BSpline::Clamped);
	railB.setControlPoints({
		0, 2, 0,
		1, 0, 1,
		3, 0, 1,
		4, 2, 0
	});

	const real spatialEps = 1e-7;
	const real paramEps = 1e-7;
	IntersectionResult result =
	    railA.intersect(railB, spatialEps, paramEps, paramEps);

	std::printf("complete: %s\n", result.complete() ? "yes" : "no");
	std::printf("work boxes expanded: %zu\n", result.expanded());

	const auto isolated = result.isolated();
	std::printf("isolated intersections: %zu\n", isolated.size());
	for (const auto &q : isolated) {
		const Vec3 p = q.point();
		std::printf("  railA u=%.6f, railB v=%.6f at (%.6f, %.6f, %.6f)"
		            " intervals (%zu,%zu)\n",
		            q.u(), q.v(), p.x(), p.y(), p.z(),
		            q.intervalA(), q.intervalB());
	}

	const auto overlaps = result.overlaps();
	std::printf("coincident ranges: %zu\n", overlaps.size());
	for (const auto &o : overlaps) {
		std::printf("  intervals (%zu,%zu): railA [%.6f,%.6f], "
		            "railB [%.6f,%.6f], v = %.6f*u + %.6f\n",
		            o.intervalA(), o.intervalB(),
		            o.u0(), o.u1(), o.v0(), o.v1(), o.a(), o.b());
	}

	/* An incomplete query is a valid outcome: the unresolved boxes cover
	 * the remaining possible solutions. */
	if (!result.complete()) {
		const auto boxes = result.boxes();
		std::printf("unresolved parameter boxes: %zu\n", boxes.size());
		for (const auto &bx : boxes) {
			std::printf("  railA [%.6f,%.6f] x railB [%.6f,%.6f]"
			            " reason=%d\n",
			            bx.uMin(), bx.uMax(), bx.vMin(), bx.vMax(),
			            bx.reason());
		}
	}

	return 0;
}
