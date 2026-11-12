#include "qa.hpp"
#include "tinysplinecxx.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

/*
 * Contract tests for the global projection index.
 *
 * The reference nearest distance used below is computed independently of the
 * projection index: the spline is decomposed into Bezier segments (a separate
 * code path) and the squared distance is minimized on every segment with a
 * dense bracketing scan followed by golden-section refinement. The dense scan
 * only locates candidate intervals -- it is never treated as a strict lower
 * bound. Golden section converges the extreme to high precision.
 */

namespace {

const double INF = std::numeric_limits<double>::infinity();

/* Deterministic pseudo-random generator (no global state). */
struct Rng {
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed) {}
	double next()
	{
		/* LCG constants from Numerical Recipes. */
		s = 6364136223846793005ULL * s + 1442695040888963407ULL;
		return (double) (s >> 11) /
		       (double) (1ULL << 53); /* [0, 1) */
	}
	double range(double lo, double hi) { return lo + next() * (hi - lo); }
};

double vecDist(const std::vector<tinyspline::real> &a,
               const std::vector<tinyspline::real> &b)
{
	double sum = 0.0;
	for (size_t i = 0; i < a.size(); i++) {
		double d = (double) a[i] - (double) b[i];
		sum += d * d;
	}
	return std::sqrt(sum);
}

/* Diagonal of the control point bounding box; mirrors the index's own
 * computation. */
double bboxDiagonal(const tinyspline::BSpline &curve)
{
	std::vector<tinyspline::real> cp = curve.controlPoints();
	const size_t dim = curve.dimension();
	std::vector<double> lo(dim, INF), hi(dim, -INF);
	for (size_t i = 0; i < cp.size(); i += dim) {
		for (size_t d = 0; d < dim; d++) {
			double v = cp[i + d];
			lo[d] = std::min(lo[d], v);
			hi[d] = std::max(hi[d], v);
		}
	}
	double sum = 0.0;
	for (size_t d = 0; d < dim; d++) {
		double w = hi[d] - lo[d];
		sum += w * w;
	}
	return std::sqrt(sum);
}

/* A tolerance comfortably above the index's admissible floor, yet still
 * tight: epsilon is a space distance, not a parameter step. */
double admissibleEpsilon(const tinyspline::BSpline &curve)
{
	return 4.0e-10 * std::max(1.0, bboxDiagonal(curve));
}

/* De Casteljau evaluation of one Bezier segment (control points laid out
 * consecutively, `order` points of `dim` components each). */
std::vector<double>
bezierEval(const std::vector<double> &ctrlp,
           size_t order, size_t dim, size_t seg, double t)
{
	std::vector<double> w(order * dim);
	for (size_t i = 0; i < order * dim; i++)
		w[i] = ctrlp[seg * order * dim + i];
	for (size_t r = 1; r < order; r++) {
		for (size_t i = 0; i + 1 < order - r + 1 && i < order - r; i++) {
			for (size_t d = 0; d < dim; d++) {
				w[i * dim + d] =
				    (1.0 - t) * w[i * dim + d] +
				    t * w[(i + 1) * dim + d];
			}
		}
	}
	return std::vector<double>(w.begin(), w.begin() + dim);
}

double sqDist(const std::vector<double> &p, const std::vector<double> &q)
{
	double s = 0.0;
	for (size_t i = 0; i < p.size(); i++) {
		double d = p[i] - q[i];
		s += d * d;
	}
	return s;
}

/* Golden-section minimization of squared distance on [lo, hi]. The caller
 * guarantees a minimum is bracketed inside. */
double goldenMinSq(const std::vector<double> &ctrlp, size_t order,
                   size_t dim, size_t seg, const std::vector<double> &q,
                   double lo, double hi)
{
	const double gr = (std::sqrt(5.0) - 1.0) * 0.5; /* 0.618... */
	double a = lo, b = hi;
	double c = b - gr * (b - a);
	double d = a + gr * (b - a);
	auto f = [&](double t) {
		return sqDist(bezierEval(ctrlp, order, dim, seg, t), q);
	};
	double fc = f(c), fd = f(d);
	for (int it = 0; it < 200 && (b - a) > 1e-14; it++) {
		if (fc < fd) {
			b = d;
			d = c;
			fd = fc;
			c = b - gr * (b - a);
			fc = f(c);
		} else {
			a = c;
			c = d;
			fc = fd;
			d = a + gr * (b - a);
			fd = f(d);
		}
	}
	double tm = 0.5 * (a + b);
	return std::sqrt(f(tm));
}

/*
 * Independent high-precision global minimum distance from `q` to `curve`.
 * Every Bezier segment is scanned to bracket its minima and refined with
 * golden section; all one-sided junction points are evaluated explicitly so
 * full-multiplicity breaks are handled without connecting the segments.
 */
double referenceMinDistance(const tinyspline::BSpline &curve,
                            const std::vector<double> &q)
{
	const size_t order = curve.order();
	const size_t dim = curve.dimension();
	tinyspline::BSpline bez = curve.toBeziers();
	std::vector<tinyspline::real> raw = bez.controlPoints();
	std::vector<double> ctrlp(raw.begin(), raw.end());
	const size_t nseg = ctrlp.size() / (order * dim);

	double best = INF;
	const size_t scans = 256;
	for (size_t s = 0; s < nseg; s++) {
		/* Explicit endpoints (one-sided limits). */
		best = std::min(best,
		    std::sqrt(sqDist(bezierEval(ctrlp, order, dim, s, 0.0), q)));
		best = std::min(best,
		    std::sqrt(sqDist(bezierEval(ctrlp, order, dim, s, 1.0), q)));
		/* Dense scan to bracket minima, then refine each bracket. */
		double prev = std::sqrt(sqDist(
		    bezierEval(ctrlp, order, dim, s, 0.0), q));
		for (size_t i = 1; i <= scans; i++) {
			double t = (double) i / (double) scans;
			double cur = std::sqrt(sqDist(
			    bezierEval(ctrlp, order, dim, s, t), q));
			if (cur <= prev) {
				/* [t-dt, t+dt] brackets a local minimum. */
				double lo = (double) (i - 1) / (double) scans;
				double hi = std::min(1.0,
				    (double) (i + 1) / (double) scans);
				best = std::min(best, goldenMinSq(
				    ctrlp, order, dim, s, q, lo, hi));
			}
			prev = cur;
		}
	}
	return best;
}

/* Evaluates the curve point at a global knot, restricted to a given span,
 * using the Bezier decomposition (independent of the projection index). */
std::vector<double>
referencePointAtSpan(const tinyspline::BSpline &curve,
                     size_t spanIndex, double knot)
{
	const size_t order = curve.order();
	const size_t dim = curve.dimension();
	std::vector<tinyspline::real> knots = curve.knots();
	tinyspline::BSpline bez = curve.toBeziers();
	std::vector<tinyspline::real> raw = bez.controlPoints();
	std::vector<double> ctrlp(raw.begin(), raw.end());

	/* Map the span's left index to the ordinal of its non-zero interval. */
	size_t seg = 0;
	for (size_t k = curve.degree(); k < spanIndex; k++) {
		if (knots[k] < knots[k + 1])
			seg++;
	}
	double u0 = knots[spanIndex];
	double u1 = knots[spanIndex + 1];
	double t = (u1 > u0) ? (knot - u0) / (u1 - u0) : 0.0;
	if (t < 0.0)
		t = 0.0;
	if (t > 1.0)
		t = 1.0;
	return bezierEval(ctrlp, order, dim, seg, t);
}

tinyspline::BSpline makeCubic2D(const std::vector<double> &ctrlp,
                                tinyspline::BSpline::Type type)
{
	tinyspline::BSpline s(ctrlp.size() / 2, 2, 3, type);
	std::vector<tinyspline::real> cp(ctrlp.begin(), ctrlp.end());
	s.setControlPoints(cp);
	return s;
}

/* Asserts the certified bracket of a projection against the independent
 * high-precision reference. */
void requireValidBracket(Check &chk,
                         const tinyspline::BSpline &curve,
                         const tinyspline::ProjectionIndex &idx,
                         const std::vector<double> &q,
                         double epsilon)
{
	std::vector<tinyspline::real> qr(q.begin(), q.end());
	tinyspline::Projection r = idx.query(qr, epsilon);
	const double ref = referenceMinDistance(curve, q);
	/* Numerical slack scales with the coordinate magnitude. */
	const double slack = std::max(1e-9, 1e-10 * epsilon + 1e-9);

	chk.require(r.distanceLower() >= 0.0,
	            "lower bound must be non-negative");
	chk.require(r.distanceLower() <= r.distanceUpper() + slack,
	            "lower bound must not exceed upper bound");
	chk.require(r.distanceUpper() - r.distanceLower() <=
	                epsilon + slack,
	            "distance bracket must be no wider than epsilon");
	/* The reference minimum approximates the true minimum from above. */
	chk.require(r.distanceLower() <= ref + slack,
	            "lower bound must not exceed the true shortest distance");
	chk.require(ref <= r.distanceUpper() + slack,
	            "upper bound must be at least the true shortest distance");
	/* Upper equals the Euclidean distance to the returned point. */
	std::vector<tinyspline::real> p = r.point();
	chk.require(std::abs(vecDist(p, qr) - r.distanceUpper()) <=
	                std::max(1e-9, 1e-9 * r.distanceUpper()),
	            "upper bound must equal the point's Euclidean distance");
	/* The returned point is a real curve point on the reported span. */
	std::vector<double> expected =
	    referencePointAtSpan(curve, r.spanIndex(), r.knot());
	double mismatch = 0.0;
	for (size_t i = 0; i < expected.size(); i++)
		mismatch = std::max(mismatch,
		    std::abs(expected[i] - (double) p[i]));
	chk.require(mismatch <= std::max(1e-7, 1e-8 * bboxDiagonal(curve)),
	            "returned point must lie on the curve at the reported knot");
}

} // namespace

void register_contracts(Registry &reg)
{
	using namespace tinyspline;

	/* ============================================================= *
	 * global-distance: certified distance brackets on curves where  *
	 * uniform sampling and single-coordinate bisection fail.        *
	 * ============================================================= */

	reg.add("global-distance", "analytic-line", [](Check &chk) -> int {
		BSpline s(2, 2, 1, BSpline::Clamped);
		s.setControlPoints({0.0, 0.0, 10.0, 0.0});
		ProjectionIndex idx(s);
		const real eps = admissibleEpsilon(s);
		Projection r = idx.query({4.0, 3.0}, eps);
		chk.require(std::abs(r.point()[0] - 4.0) <= 1e-7,
		            "analytic foot x");
		chk.require(std::abs(r.point()[1]) <= 1e-7,
		            "analytic foot y");
		chk.require(std::abs(r.distanceUpper() - 3.0) <= 1e-7,
		            "analytic distance");
		chk.require(r.distanceLower() <= 3.0 &&
		            3.0 - r.distanceLower() <= eps + 1e-12,
		            "analytic bracket");
		return 0;
	});

	reg.add("global-distance", "foldback-curve", [](Check &chk) -> int {
		/* x rises and then returns to zero: a genuine fold-back that a
		 * single-coordinate bisection cannot resolve. */
		BSpline s = makeCubic2D(
		    {0, 0, 10, 5, 10, 15, 0, 20}, BSpline::Beziers);
		ProjectionIndex idx(s);
		const real eps = admissibleEpsilon(s);
		const std::vector<double> q = {5.0, 10.0};
		requireValidBracket(chk, s, idx, q, eps);
		Projection r = idx.query({5.0, 10.0}, eps);
		chk.require(r.visitedPieces() >= 1, "foldback visited a piece");
		return 0;
	});

	reg.add("global-distance", "narrow-loop", [](Check &chk) -> int {
		/* Tight loop whose two branches nearly meet. */
		BSpline s = makeCubic2D(
		    {0, 0, 2, 9, -2, 9, 0, 18}, BSpline::Beziers);
		ProjectionIndex idx(s);
		const real eps = admissibleEpsilon(s);
		for (const double qx : {-1.5, 0.0, 1.5}) {
			requireValidBracket(chk, s, idx, {qx, 9.0}, eps);
		}
		return 0;
	});

	reg.add("global-distance", "cubic-golden-reference",
	[](Check &chk) -> int {
		BSpline s = makeCubic2D(
		    {-1, 0, 2, 3, -2, 3, 1, 0}, BSpline::Clamped);
		ProjectionIndex idx(s);
		const real eps = admissibleEpsilon(s);
		Rng rng(42);
		for (int trial = 0; trial < 120; trial++) {
			std::vector<double> q = {
			    rng.range(-3.0, 3.0), rng.range(-1.0, 4.0)};
			requireValidBracket(chk, s, idx, q, eps);
		}
		return 0;
	});

	reg.add("global-distance", "three-and-four-dim",
	[](Check &chk) -> int {
		Rng rng(7);
		for (size_t dim : { (size_t) 3, (size_t) 4 }) {
			const size_t nctrl = 8;
			BSpline s(nctrl, dim, 3, BSpline::Beziers);
			std::vector<real> cp(nctrl * dim);
			for (double &v : cp)
				v = rng.range(-5.0, 5.0);
			s.setControlPoints(cp);
			ProjectionIndex idx(s);
			const real eps = admissibleEpsilon(s);
			for (int trial = 0; trial < 40; trial++) {
				std::vector<double> q(dim);
				for (double &v : q)
					v = rng.range(-6.0, 6.0);
				requireValidBracket(chk, s, idx, q, eps);
			}
		}
		return 0;
	});

	reg.add("global-distance", "broken-segments", [](Check &chk) -> int {
		/* Two cubic segments separated by a full-multiplicity break. */
		BSpline s(8, 2, 3, BSpline::Beziers);
		s.setControlPoints({
		    0, 0, 0.3, 0, 0.7, 0, 1, 0,
		    10, 10, 10.3, 10, 10.7, 10, 11, 10});
		ProjectionIndex idx(s);
		const real eps = admissibleEpsilon(s);
		requireValidBracket(chk, s, idx, {10.5, 11.0}, eps);
		Projection nearSecond = idx.query({10.5, 11.0}, eps);
		chk.require(nearSecond.spanIndex() == 7,
		            "query near second segment selects second span");
		chk.require(nearSecond.point()[0] > 9.0,
		            "projection never jumps across the break");
		/* A point in the gap snaps to an endpoint, never an interior
		 * point bridging the discontinuity. */
		Projection gap = idx.query({5.0, 5.0}, eps);
		bool onFirst = gap.point()[0] <= 1.0 + 1e-9;
		bool onSecond = gap.point()[0] >= 10.0 - 1e-9;
		chk.require(onFirst || onSecond,
		            "gap query lands on a segment endpoint only");
		return 0;
	});

	reg.add("global-distance", "equidistant-branches",
	[](Check &chk) -> int {
		/* Two symmetric line segments; (0,0) is equidistant from both
		 * nearest endpoints. */
		BSpline s(4, 2, 1, BSpline::Beziers);
		s.setControlPoints({-12, 0, -8, 0, 8, 0, 12, 0});
		ProjectionIndex idx(s);
		const real eps = admissibleEpsilon(s);
		Projection first = idx.query({0.0, 0.0}, eps);
		for (int i = 0; i < 10; i++) {
			Projection again = idx.query({0.0, 0.0}, eps);
			chk.require(again.knot() == first.knot() &&
			            again.spanIndex() == first.spanIndex(),
			            "equidistant choice is deterministic");
		}
		chk.require(std::abs(first.distanceUpper() - 8.0) <= 1e-7,
		            "equidistant distance is 8");
		return 0;
	});

	reg.add("global-distance", "coordinate-scale", [](Check &chk) -> int {
		/* Large coordinates near the supported limit. */
		BSpline big(4, 2, 3, BSpline::Clamped);
		big.setControlPoints({
		    1.0e6, 1.0e6, 1.0e6 + 10, 1.0e6 - 10,
		    1.0e6 - 10, 1.0e6 + 10, 1.0e6, 1.0e6});
		ProjectionIndex idx(big);
		/* Query components stay within the mandated |coord| <= 1e6. */
		requireValidBracket(chk, big, idx,
		    {1.0e6 - 1.0, 1.0e6}, 1e-3);
		/* A tolerance below the admissible floor is rejected. */
		bool threw = false;
		try {
			idx.query({1.0e6, 1.0e6}, 1e-12);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		chk.require(threw, "sub-floor epsilon is rejected at large scale");

		/* Small coordinates: the floor is 1e-10. */
		BSpline small(4, 2, 3, BSpline::Clamped);
		small.setControlPoints({0, 0, 0.2, 0.2, -0.2, 0.2, 0, 0});
		ProjectionIndex idxSmall(small);
		requireValidBracket(chk, small, idxSmall, {0.1, 0.1},
		                    admissibleEpsilon(small));
		threw = false;
		try {
			idxSmall.query({0.1, 0.1}, 1e-12);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		chk.require(threw, "sub-floor epsilon is rejected at small scale");
		return 0;
	});

	/* ============================================================= *
	 * span-limits: span indices, break-knot sides, endpoints, domain *
	 * ============================================================= */

	reg.add("span-limits", "span-index-mapping", [](Check &chk) -> int {
		BSpline s(8, 2, 3, BSpline::Beziers);
		s.setControlPoints({
		    0, 0, 0.3, 0, 0.7, 0, 1, 0,
		    2, 0, 2.3, 0, 2.7, 0, 3, 0});
		ProjectionIndex idx(s);
		const real eps = admissibleEpsilon(s);
		std::vector<real> knots = s.knots();
		/* First span left index is degree (3), second is 7. */
		Projection a = idx.query({0.5, 1.0}, eps);
		chk.require(a.spanIndex() == 3 &&
		            knots[a.spanIndex()] == 0.0,
		            "first span left index");
		Projection b = idx.query({2.5, 1.0}, eps);
		chk.require(b.spanIndex() == 7 &&
		            knots[b.spanIndex()] == 0.5,
		            "second span left index");
		return 0;
	});

	reg.add("span-limits", "break-knot-sides", [](Check &chk) -> int {
		/* The break knot 0.5 has two different one-sided limits. */
		BSpline s(8, 2, 3, BSpline::Beziers);
		s.setControlPoints({
		    -1, -1, -0.5, -1, -0.5, 0, 0, 0,
		    10, 10, 10.5, 10, 10.5, 11, 11, 11});
		ProjectionIndex idx(s);
		const real eps = admissibleEpsilon(s);
		Projection leftSide = idx.query({0.0, 0.0}, eps);
		Projection rightSide = idx.query({10.0, 10.0}, eps);
		chk.require(std::abs(leftSide.knot() - 0.5) <= 1e-12 &&
		            leftSide.spanIndex() == 3,
		            "left one-sided limit keeps the left span index");
		chk.require(std::abs(rightSide.knot() - 0.5) <= 1e-12 &&
		            rightSide.spanIndex() == 7,
		            "right one-sided limit keeps the right span index");
		chk.require(std::abs(leftSide.point()[0]) <= 1e-9 &&
		            std::abs(leftSide.point()[1]) <= 1e-9,
		            "left point is the left limit");
		chk.require(std::abs(rightSide.point()[0] - 10.0) <= 1e-9 &&
		            std::abs(rightSide.point()[1] - 10.0) <= 1e-9,
		            "right point is the right limit");
		return 0;
	});

	reg.add("span-limits", "endpoints", [](Check &chk) -> int {
		BSpline s = makeCubic2D(
		    {1, 1, 2, 4, 0, 4, 3, 1}, BSpline::Clamped);
		ProjectionIndex idx(s);
		const real eps = admissibleEpsilon(s);
		/* Far queries on the endpoint's own y-level make that endpoint
		 * the unique nearest point (every interior point has both a
		 * larger horizontal gap and a non-zero vertical gap). */
		Projection first = idx.query({-100.0, 1.0}, eps);
		Projection last = idx.query({100.0, 1.0}, eps);
		chk.require(std::abs(first.knot() - 0.0) <= 1e-12,
		            "far query before start returns domain minimum");
		chk.require(std::abs(last.knot() - 1.0) <= 1e-12,
		            "far query after end returns domain maximum");
		chk.require(std::abs(first.point()[0] - 1.0) <= 1e-9 &&
		            std::abs(first.point()[1] - 1.0) <= 1e-9,
		            "start endpoint point");
		chk.require(std::abs(last.point()[0] - 3.0) <= 1e-9 &&
		            std::abs(last.point()[1] - 1.0) <= 1e-9,
		            "end endpoint point");
		return 0;
	});

	reg.add("span-limits", "nonunit-domain", [](Check &chk) -> int {
		BSpline s(4, 2, 3, BSpline::Clamped);
		s.setControlPoints({0, 0, 1, 2, 2, 2, 3, 0});
		/* Clamped knot vector on domain [3, 7]. */
		s.setKnots({3, 3, 3, 3, 7, 7, 7, 7});
		ProjectionIndex idx(s);
		Projection r = idx.query({1.5, 1.0}, admissibleEpsilon(s));
		chk.require(r.knot() >= 3.0 - 1e-12 && r.knot() <= 7.0 + 1e-12,
		            "returned knot stays inside the non-unit domain");
		std::vector<double> expected =
		    referencePointAtSpan(s, r.spanIndex(), r.knot());
		double mismatch = 0.0;
		for (size_t i = 0; i < expected.size(); i++)
			mismatch = std::max(mismatch,
			    std::abs(expected[i] - (double) r.point()[i]));
		chk.require(mismatch <= 1e-7,
		            "point matches the curve on the non-unit domain");
		return 0;
	});

	reg.add("span-limits", "point-on-curve", [](Check &chk) -> int {
		BSpline s = makeCubic2D(
		    {-1, 0, 2, 3, -2, 3, 1, 0}, BSpline::Clamped);
		ProjectionIndex idx(s);
		const real eps = admissibleEpsilon(s);
		for (double u : {0.1, 0.47, 0.73, 0.95}) {
			std::vector<real> on = s.eval(u).result();
			Projection r = idx.query(on, eps);
			chk.require(r.distanceUpper() <= 1e-7,
			            "querying an actual curve point gives ~zero distance");
		}
		return 0;
	});

	/* ============================================================= *
	 * snapshot-index: ownership, copy/move, determinism, pruning    *
	 * ============================================================= */

	reg.add("snapshot-index", "source-edit", [](Check &chk) -> int {
		BSpline s = makeCubic2D(
		    {-1, 0, 2, 3, -2, 3, 1, 0}, BSpline::Clamped);
		ProjectionIndex idx(s);
		Projection before = idx.query({0.3, 2.0}, 1e-9);
		s.setControlPoints({0, 0, 0, 0, 0, 0, 0, 0});
		Projection after = idx.query({0.3, 2.0}, 1e-9);
		chk.require(before.knot() == after.knot() &&
		            before.spanIndex() == after.spanIndex(),
		            "editing the source spline does not change the index");
		return 0;
	});

	reg.add("snapshot-index", "source-release", [](Check &chk) -> int {
		Projection captured = [] {
			BSpline local = makeCubic2D(
			    {-1, 0, 2, 3, -2, 3, 1, 0}, BSpline::Clamped);
			ProjectionIndex built(local);
			/* `local` is destroyed when the lambda returns. */
			return built.query({0.3, 2.0}, 1e-9);
		}();
		chk.require(captured.point().size() == 2,
		            "index survives the destruction of its source spline");
		chk.require(std::isfinite(captured.point()[0]) &&
		            std::isfinite(captured.point()[1]),
		            "snapshot point is finite after source release");
		return 0;
	});

	reg.add("snapshot-index", "copy-move", [](Check &chk) -> int {
		BSpline s = makeCubic2D(
		    {-1, 0, 2, 3, -2, 3, 1, 0}, BSpline::Clamped);
		ProjectionIndex idx(s);
		Projection ref = idx.query({0.3, 2.0}, 1e-9);

		ProjectionIndex copied(idx);
		Projection cp = copied.query({0.3, 2.0}, 1e-9);
		chk.require(cp.knot() == ref.knot(), "deep copy queries alike");

		ProjectionIndex moved(std::move(copied));
		Projection mv = moved.query({0.3, 2.0}, 1e-9);
		chk.require(mv.knot() == ref.knot(), "moved index queries alike");

		/* The moved-from index is empty and must reject queries. */
		bool threw = false;
		try {
			copied.query({0.3, 2.0}, 1e-9);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		chk.require(threw, "moved-from index is empty");

		/* Copy assignment then move assignment keep behavior correct. */
		ProjectionIndex assigned = idx;
		assigned = idx;
		Projection asg = assigned.query({0.3, 2.0}, 1e-9);
		chk.require(asg.knot() == ref.knot(), "copy assignment works");
		return 0;
	});

	reg.add("snapshot-index", "a-b-a-determinism", [](Check &chk) -> int {
		BSpline a = makeCubic2D(
		    {-1, 0, 2, 3, -2, 3, 1, 0}, BSpline::Clamped);
		BSpline b = makeCubic2D(
		    {5, 5, 6, 9, 4, 9, 7, 5}, BSpline::Clamped);
		ProjectionIndex idxA(a), idxB(b);
		const std::vector<real> q = {0.7, 2.3};
		Projection a1 = idxA.query(q, 1e-9);
		Projection b1 = idxB.query(q, 1e-9);
		Projection a2 = idxA.query(q, 1e-9);
		chk.require(a1.knot() == a2.knot() &&
		            a1.spanIndex() == a2.spanIndex() &&
		            a1.point() == a2.point(),
		            "A/B/A: A is unchanged after querying B");
		chk.require(b1.spanIndex() != a1.spanIndex() ||
		            b1.point() != a1.point(),
		            "A/B/A: B actually produced a different projection");
		return 0;
	});

	reg.add("snapshot-index", "concurrent-queries", [](Check &chk) -> int {
		BSpline s = makeCubic2D(
		    {-1, 0, 2, 3, -2, 3, 1, 0}, BSpline::Clamped);
		const ProjectionIndex idx(s);
		const std::vector<real> q = {0.2, 1.7};
		Projection expected = idx.query(q, 1e-9);

		std::atomic<int> mismatches(0);
		std::vector<std::thread> threads;
		for (int t = 0; t < 8; t++) {
			threads.emplace_back([&] {
				for (int i = 0; i < 300; i++) {
					Projection r = idx.query(q, 1e-9);
					if (r.knot() != expected.knot() ||
					    r.spanIndex() != expected.spanIndex() ||
					    r.point() != expected.point())
						mismatches++;
				}
			});
		}
		for (auto &th : threads)
			th.join();
		chk.require(mismatches.load() == 0,
		            "concurrent const queries all agree");
		return 0;
	});

	reg.add("snapshot-index", "pruning-512", [](Check &chk) -> int {
		const size_t n = 512;
		const size_t order = 4;
		BSpline s(n * order, 2, 3, BSpline::Beziers);
		std::vector<real> cp(n * order * 2);
		/* Each piece has length 1 in x and consecutive pieces are
		 * separated by a one-unit gap, so the 512 pieces are mutually
		 * disjoint. */
		for (size_t i = 0; i < n; i++) {
			double x0 = 2.0 * (double) i;
			cp[(i * order + 0) * 2 + 0] = x0;
			cp[(i * order + 1) * 2 + 0] = x0 + 0.33;
			cp[(i * order + 2) * 2 + 0] = x0 + 0.66;
			cp[(i * order + 3) * 2 + 0] = x0 + 1.0;
		}
		s.setControlPoints(cp);
		ProjectionIndex idx(s);
		const real eps = admissibleEpsilon(s);
		Projection first = idx.query({0.5, 1.0}, eps);
		Projection again = idx.query({0.5, 1.0}, eps);
		chk.require(first.visitedPieces() < 64,
		            "first query prunes to fewer than 64 pieces");
		chk.require(again.visitedPieces() < 64,
		            "repeated query prunes to fewer than 64 pieces");
		chk.require(first.spanIndex() == 3,
		            "nearest point is found on the first piece");
		chk.require(first.knot() == again.knot(),
		            "repeated query is deterministic");
		return 0;
	});

	reg.add("snapshot-index", "degenerate-constant", [](Check &chk) -> int {
		BSpline s(4, 2, 3, BSpline::Clamped);
		s.setControlPoints({5, 5, 5, 5, 5, 5, 5, 5});
		ProjectionIndex idx(s);
		Projection r = idx.query({8.0, 9.0}, 1e-9);
		chk.require(std::abs(r.point()[0] - 5.0) <= 1e-9 &&
		            std::abs(r.point()[1] - 5.0) <= 1e-9,
		            "constant spline projects to its only point");
		chk.require(std::abs(r.distanceUpper() - 5.0) <= 1e-8,
		            "constant spline distance");
		chk.require(r.distanceUpper() - r.distanceLower() <=
		            1e-9 + 1e-12,
		            "constant spline bracket width");
		return 0;
	});

	reg.add("snapshot-index", "invalid-inputs-unchanged",
	[](Check &chk) -> int {
		/* Unsupported degree. */
		{
			BSpline bad(6, 2, 4, BSpline::Clamped);
			ProjectionIndex idx;
			bool threw = false;
			try {
				ProjectionIndex tmp(bad);
			} catch (const std::runtime_error &) {
				threw = true;
			}
			chk.require(threw, "degree 4 is rejected");
			/* Default-constructed index is empty. */
			threw = false;
			try {
				idx.query({0.0, 0.0}, 1e-7);
			} catch (const std::runtime_error &) {
				threw = true;
			}
			chk.require(threw, "empty index rejects queries");
		}
		/* Unsupported dimension and non-finite control points. */
		{
			BSpline dim5(4, 5, 3, BSpline::Clamped);
			bool threw = false;
			try {
				ProjectionIndex tmp(dim5);
			} catch (const std::runtime_error &) {
				threw = true;
			}
			chk.require(threw, "dimension 5 is rejected");

			BSpline nanCurve(4, 2, 3, BSpline::Clamped);
			std::vector<real> cp = {
			    0, 0, 1, 1, 1, 1,
			    std::numeric_limits<real>::quiet_NaN(), 0};
			nanCurve.setControlPoints(cp);
			threw = false;
			try {
				ProjectionIndex tmp(nanCurve);
			} catch (const std::runtime_error &) {
				threw = true;
			}
			chk.require(threw, "non-finite control point is rejected");
		}
		/* Non-clamped spline. */
		{
			BSpline opened(5, 2, 2, BSpline::Opened);
			bool threw = false;
			try {
				ProjectionIndex tmp(opened);
			} catch (const std::runtime_error &) {
				threw = true;
			}
			chk.require(threw, "non-clamped spline is rejected");
		}
		/* Failing queries leave the index and result untouched. */
		{
			BSpline s(4, 2, 3, BSpline::Clamped);
			s.setControlPoints({0, 0, 1, 2, 2, 2, 3, 0});
			ProjectionIndex idx(s);
			Projection good = idx.query({1.0, 1.0}, 1e-7);

			bool threw = false;
			try {
				idx.query({1.0, 2.0, 3.0}, 1e-7);
			} catch (const std::runtime_error &) {
				threw = true;
			}
			chk.require(threw, "dimension mismatch is rejected");
			threw = false;
			try {
				idx.query({std::numeric_limits<real>::infinity(), 0.0},
				          1e-7);
			} catch (const std::runtime_error &) {
				threw = true;
			}
			chk.require(threw, "non-finite query is rejected");
			threw = false;
			try {
				idx.query({1.0, 1.0}, 1e-20);
			} catch (const std::runtime_error &) {
				threw = true;
			}
			chk.require(threw, "bad tolerance is rejected");

			/* The index still works and yields the same projection. */
			Projection after = idx.query({1.0, 1.0}, 1e-7);
			chk.require(after.knot() == good.knot() &&
			            after.spanIndex() == good.spanIndex(),
			            "index is unchanged after rejected queries");
		}
		/* A failed construction must leave an already built index
		 * untouched (exercised through the owning C API, which can rebuild
		 * into an existing handle). */
		{
			tsBSpline good = ts_bspline_init();
			tsBSpline bad = ts_bspline_init();
			tsProjectionIndex raw = ts_projection_index_init();
			tsStatus st;
			tsReal goodCp[8] = {0, 0, 1, 2, 2, 2, 3, 0};
			tsReal q[2] = {1.0, 1.0};
			tsProjection before, again;

			chk.require(ts_bspline_new(4, 2, 3, TS_CLAMPED, &good,
			                           &st) == 0,
			            "reference spline builds");
			ts_bspline_set_control_points(&good, goodCp, NULL);
			chk.require(ts_projection_index_new(&good, &raw, &st) == 0,
			            "reference index builds");
			chk.require(ts_projection_index_query(&raw, q, 2, 1e-7,
			                                      &before, &st) == 0,
			            "reference index queries");

			chk.require(ts_bspline_new(6, 2, 4, TS_CLAMPED, &bad,
			                           &st) == 0,
			            "invalid spline builds");
			chk.require(ts_projection_index_new(&bad, &raw, &st) != 0,
			            "rebuild with an invalid spline fails");

			chk.require(ts_projection_index_query(&raw, q, 2, 1e-7,
			                                      &again, &st) == 0,
			            "existing index survives the failed rebuild");
			chk.require(again.knot == before.knot &&
			            again.span_index == before.span_index,
			            "failed rebuild does not change the index");
			ts_projection_index_free(&raw);
			ts_bspline_free(&bad);
			ts_bspline_free(&good);
		}
		return 0;
	});
}
