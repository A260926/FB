/**
 * @file
 * Regression tests for the two-curve intersection query.
 *
 * The expected parameters and positions are derived independently of the
 * intersection engine:
 *
 *  - curve points are checked with a hand-written Bernstein evaluator that
 *    does not call any intersection code;
 *  - isolated roots are taken from analytic polynomial solutions (linear
 *    and cubic equations solved explicitly);
 *  - coincidence is checked by evaluating the recorded affine
 *    correspondence v = a*u + b on a parameter grid.
 */
#include "qa.hpp"
#include "tinysplinecxx.h"

#include <cmath>
#include <vector>
#include <stdexcept>

using namespace tinyspline;

namespace {

constexpr double EPS = 1e-7;

/* Independent binomial coefficient, degree at most 3. */
size_t choose(size_t n, size_t k)
{
	static const size_t c[4][4] = {
		{1, 0, 0, 0},
		{1, 1, 0, 0},
		{1, 2, 1, 0},
		{1, 3, 3, 1}
	};
	return c[n][k];
}

/* Independent de Casteljaj-free Bernstein evaluation of a degree-`deg`
 * 2D or 3D Bezier segment at local parameter t in [0,1]. */
std::vector<real>
bernstein(const std::vector<real> &points, size_t dim, size_t deg, real t)
{
	std::vector<real> out(dim, 0);
	for (size_t i = 0; i <= deg; ++i) {
		real b = static_cast<real>(choose(deg, i));
		real ti = std::pow(t, static_cast<int>(i));
		real tj = std::pow(1 - t, static_cast<int>(deg - i));
		for (size_t d = 0; d < dim; ++d)
			out[d] += b * ti * tj * points[i * dim + d];
	}
	return out;
}

/* Evaluate a clamped single-piece B-spline (a raw Bezier) at original
 * parameter u, mapping [knot0,knot1] onto local [0,1]. */
std::vector<real>
evalBezier(const BSpline &s, real u, real knot0, real knot1)
{
	const real t = (u - knot0) / (knot1 - knot0);
	return bernstein(s.controlPoints(), s.dimension(), s.degree(), t);
}

real dist2(const std::vector<real> &p, const std::vector<real> &q)
{
	real s = 0;
	for (size_t i = 0; i < p.size(); ++i)
		s += (p[i] - q[i]) * (p[i] - q[i]);
	return s;
}

BSpline line2D(real x0, real y0, real x1, real y1)
{
	BSpline s(2, 2, 1, BSpline::Clamped);
	s.setControlPoints({x0, y0, x1, y1});
	return s;
}

/* The self-intersecting cubic c with c(1/3) == c(2/3), dimension 2. */
BSpline selfCrossCubic()
{
	BSpline s(4, 2, 3, BSpline::Clamped);
	s.setControlPoints({0, 0,
	                    static_cast<real>(7.0 / 6), 2,
	                    0, 2,
	                    1, 0});
	return s;
}

} // namespace

/* ------------------------------------------------------------------ */
/* isolated group                                                      */
/* ------------------------------------------------------------------ */

/* Transversal intersection of two lines: analytic solution of two linear
 * parametric equations. */
static int isolated_transversal_lines(Check &check)
{
	BSpline a = line2D(0, 0, 4, 2);
	BSpline b = line2D(0, 3, 4, -1);
	IntersectionResult r = a.intersect(b, EPS, EPS, EPS);
	check.require(r.complete(), "query must complete");
	check.require(r.boxes().empty(), "no unresolved boxes");
	auto iso = r.isolated();
	check.require(iso.size() == 1, "exactly one isolated intersection");
	const auto &q = iso[0];
	/* Analytic root: u=v=0.5, point (2,1). */
	check.require(std::fabs(q.u() - 0.5) <= EPS, "u parameter");
	check.require(std::fabs(q.v() - 0.5) <= EPS, "v parameter");
	check.require(std::fabs(q.point().x() - 2) <= EPS, "point x");
	check.require(std::fabs(q.point().y() - 1) <= EPS, "point y");
	check.require(q.uMax() - q.uMin() <= EPS, "u interval width");
	check.require(q.vMax() - q.vMin() <= EPS, "v interval width");
	/* Independent back-substitution through a Bernstein evaluator. */
	auto pa = evalBezier(a, q.u(), 0, 1);
	auto pb = evalBezier(b, q.v(), 0, 1);
	check.require(std::sqrt(dist2(pa, pb)) <= EPS,
	               "back-substituted positions agree");
	return 0;
}

/* Double tangency: parabola y=(2x-1)^2 - 1 in Bezier form touches the line
 * y=0 at the single point u=0.5. */
static int isolated_parabola_tangency(Check &check)
{
	BSpline a(3, 2, 2, BSpline::Clamped);
	a.setControlPoints({-1, 1, 0, -1, 1, 1});
	BSpline b = line2D(-1, 0, 1, 0);
	IntersectionResult r = a.intersect(b, EPS, EPS, EPS);
	check.require(r.complete(), "tangency query completes");
	auto iso = r.isolated();
	check.require(iso.size() == 1, "one tangency point, not a tube");
	check.require(std::fabs(iso[0].u() - 0.5) <= EPS, "tangency u");
	return 0;
}

/* Cubic x(u)=2u-1 against the line y=0 form yields three explicit roots
 * u=0.25, 0.5, 0.75 of the constructed cubic. */
static int isolated_cubic_three_roots(Check &check)
{
	BSpline a(4, 2, 3, BSpline::Clamped);
	a.setControlPoints({-2, -6,
	                    -2.0 / 3, 26.0 / 3,
	                    2.0 / 3, -26.0 / 3,
	                    2, 6});
	BSpline b = line2D(-2, 0, 2, 0);
	IntersectionResult r = a.intersect(b, EPS, EPS, EPS);
	check.require(r.complete(), "three-root query completes");
	auto iso = r.isolated();
	check.require(iso.size() == 3, "three separated roots");
	const real expected[3] = {0.25, 0.5, 0.75};
	for (size_t i = 0; i < 3; ++i) {
		bool found = false;
		for (const auto &q : iso) {
			if (std::fabs(q.u() - expected[i]) <= EPS &&
			    std::fabs(q.v() - expected[i]) <= EPS)
				found = true;
		}
		check.require(found, "analytic cubic root present");
	}
	return 0;
}

/* Contact exactly at a domain endpoint. */
static int isolated_endpoint_contact(Check &check)
{
	BSpline a = line2D(0, 0, 1, 1);
	BSpline b = line2D(1, 1, 2, 0);
	IntersectionResult r = a.intersect(b, EPS, EPS, EPS);
	check.require(r.complete(), "endpoint query completes");
	auto iso = r.isolated();
	check.require(iso.size() == 1, "one endpoint contact");
	check.require(std::fabs(iso[0].u() - 1) <= EPS, "first curve endpoint");
	check.require(std::fabs(iso[0].v() - 0) <= EPS, "second curve endpoint");
	return 0;
}

/* A 3D projection crossing that does not intersect in space. */
static int isolated_3d_projection_miss(Check &check)
{
	BSpline a(2, 3, 1, BSpline::Clamped);
	a.setControlPoints({-1, -1, 0, 1, 1, 0});
	BSpline b(2, 3, 1, BSpline::Clamped);
	b.setControlPoints({-1, -1, 1, 1, 1, 1});
	IntersectionResult r = a.intersect(b, EPS, EPS, EPS);
	check.require(r.complete(), "projection query completes");
	check.require(r.isolated().empty(), "no 3D intersection");
	check.require(r.overlaps().empty(), "no coincidence in 3D");
	return 0;
}

/* A near miss whose true minimum distance is more than 16 spatial
 * tolerances must report an empty, complete result. */
static int isolated_far_miss(Check &check)
{
	BSpline a = line2D(0, 0, 4, 0);
	BSpline b = line2D(0, 16e-6, 4, 16e-6);
	IntersectionResult r = a.intersect(b, 1e-7, EPS, EPS);
	check.require(r.complete(), "far-miss query completes");
	check.require(r.isolated().empty(), "no intersection for far miss");
	check.require(r.overlaps().empty(), "no coincidence for far miss");
	return 0;
}

/* Same spatial point reached by two different parameter pairs. */
static int isolated_same_point_diff_params(Check &check)
{
	BSpline c = selfCrossCubic();
	IntersectionResult r = c.intersect(c, EPS, EPS, EPS);
	check.require(r.complete(), "self-cross query completes");
	auto iso = r.isolated();
	check.require(iso.size() == 2, "two off-diagonal parameter pairs");
	/* The two swapped pairs (1/3,2/3) and (2/3,1/3). */
	check.require(
	    (std::fabs(iso[0].u() - 1.0 / 3) <= EPS &&
	     std::fabs(iso[0].v() - 2.0 / 3) <= EPS) ||
	    (std::fabs(iso[0].u() - 2.0 / 3) <= EPS &&
	     std::fabs(iso[0].v() - 1.0 / 3) <= EPS),
	    "first swapped pair");
	/* Both records describe the same world position. */
	check.require(std::fabs(iso[0].point().x() - iso[1].point().x()) <= EPS,
	               "same point x");
	check.require(std::fabs(iso[0].point().y() - iso[1].point().y()) <= EPS,
	               "same point y");
	/* Dedup is on parameter pairs: the two must be distinct records. */
	check.require(iso[0].u() != iso[1].u() || iso[0].v() != iso[1].v(),
	               "parameter-pair dedup keeps both");
	return 0;
}

/* ------------------------------------------------------------------ */
/* overlap group                                                       */
/* ------------------------------------------------------------------ */

/* A curve compared with itself is one full-domain affine coincidence. */
static int overlap_identity(Check &check)
{
	BSpline a(3, 2, 2, BSpline::Clamped);
	a.setControlPoints({-1, 1, 0, -1, 1, 1});
	IntersectionResult r = a.intersect(a, EPS, EPS, EPS);
	check.require(r.complete(), "self overlap completes");
	auto ov = r.overlaps();
	check.require(ov.size() >= 1, "overlap recorded");
	check.require(r.isolated().empty(), "no fake isolated points");
	const auto &o = ov[0];
	check.require(std::fabs(o.a() - 1) <= 1e-9, "identity slope");
	check.require(std::fabs(o.b()) <= 1e-9, "identity intercept");
	/* Independent check of the correspondence. */
	for (int i = 0; i <= 10; ++i) {
		real u = i / 10.0;
		real v = o.a() * u + o.b();
		auto pa = evalBezier(a, u, 0, 1);
		auto pb = evalBezier(a, v, 0, 1);
		check.require(std::sqrt(dist2(pa, pb)) <= EPS,
		               "identity correspondence holds");
	}
	return 0;
}

/* Same geometry on a non-unit parameter domain [0,2]: slope 2. */
static int overlap_nonunit_domain(Check &check)
{
	BSpline a(3, 2, 2, BSpline::Clamped);
	a.setControlPoints({-1, 1, 0, -1, 1, 1});
	BSpline b(3, 2, 2, BSpline::Clamped);
	b.setControlPoints({-1, 1, 0, -1, 1, 1});
	b.setKnots({0, 0, 0, 2, 2, 2});
	IntersectionResult r = a.intersect(b, EPS, EPS, EPS);
	check.require(r.complete(), "reparametrized overlap completes");
	auto ov = r.overlaps();
	check.require(ov.size() >= 1, "reparam overlap recorded");
	check.require(std::fabs(ov[0].a() - 2) <= 1e-9, "reparam slope");
	return 0;
}

/* A reversed traversal of the self-crossing cubic must still detect both
 * off-diagonal isolated roots rather than hiding them. */
static int overlap_reversed_keeps_roots(Check &check)
{
	BSpline a = selfCrossCubic();
	BSpline b(4, 2, 3, BSpline::Clamped);
	b.setControlPoints({1, 0, 0, 2, static_cast<real>(7.0 / 6), 2, 0, 0});
	IntersectionResult r = a.intersect(b, EPS, EPS, EPS);
	check.require(r.complete(), "reversed query completes");
	check.require(r.isolated().size() == 2,
	               "reversed traversal keeps off-diagonal roots");
	check.require(!r.overlaps().empty(),
	               "reversed traversal also records the coincidence");
	return 0;
}

/* Parallel offset curves are not coincident and must not be merged. */
static int overlap_parallel_not_coincident(Check &check)
{
	BSpline a = line2D(0, 0, 4, 2);
	BSpline b(2, 2, 1, BSpline::Clamped);
	b.setControlPoints({0, 1, 4, 3});
	IntersectionResult r = a.intersect(b, EPS, EPS, EPS);
	check.require(r.complete(), "parallel query completes");
	check.require(r.overlaps().empty(), "parallel lines are not an overlap");
	return 0;
}

/* Knot insertion produces an exactly coincident arc. Build a clamped cubic,
 * insert an interior knot, and intersect the original with the refined
 * curve: coincidence must be recognized via polynomial identity, not point
 * sampling. */
static int overlap_after_knot_insertion(Check &check)
{
	BSpline a(4, 2, 3, BSpline::Clamped);
	a.setControlPoints({0, 0, 1, 2, 2, 2, 3, 0});
	BSpline b = a.insertKnot(0.5, 1);
	IntersectionResult r = a.intersect(b, EPS, EPS, EPS);
	check.require(r.complete(), "knot-insertion query completes");
	check.require(!r.overlaps().empty(),
	               "knot-inserted curve recognized as coincident");
	return 0;
}

/* Degree elevation keeps the same geometry, which must be recognized as a
 * polynomial-identical coincidence across the degree change. */
static int overlap_after_degree_elevation(Check &check)
{
	BSpline a(3, 2, 2, BSpline::Clamped);
	a.setControlPoints({0, 0, 1, 2, 2, 0});
	BSpline e = a.elevateDegree(1, 1e-12);
	IntersectionResult r = a.intersect(e, EPS, EPS, EPS);
	check.require(r.complete(), "degree-elevation query completes");
	check.require(!r.overlaps().empty(),
	               "elevated curve recognized as coincident");
	check.require(r.isolated().empty(),
	               "coincidence not reported as isolated points");
	return 0;
}

/* A true break (interior knot of multiplicity order) splits the curve into
 * independent intervals. The two resulting intersections must report the two
 * distinct original interval indices, proving the break sides keep their
 * identity. */
static int isolated_true_break_intervals(Check &check)
{
	BSpline broken(4, 2, 1, BSpline::Clamped);
	broken.setControlPoints({0, 0, 1, 1, 2, 2, 3, 0});
	broken.setKnots({0, 0, 1, 1, 2, 2});
	BSpline h = line2D(-1, 0.5, 4, 0.5);
	IntersectionResult r = broken.intersect(h, EPS, EPS, EPS);
	check.require(r.complete(), "broken-curve query completes");
	auto iso = r.isolated();
	check.require(iso.size() == 2, "one intersection per piece");
	size_t seen[2] = {99, 99};
	size_t count = 0;
	for (const auto &q : iso) {
		check.require(q.intervalA() < 2, "valid original interval");
		seen[count++] = q.intervalA();
	}
	check.require((seen[0] == 0 && seen[1] == 1) ||
	                  (seen[0] == 1 && seen[1] == 0),
	              "both break sides report their own interval");
	return 0;
}

/* ------------------------------------------------------------------ */
/* integration group                                                   */
/* ------------------------------------------------------------------ */

/* A small work-box budget leaves unresolved boxes: incomplete is a valid
 * result and never masquerades as an empty intersection. */
static int integration_small_budget(Check &check)
{
	BSpline a(4, 2, 3, BSpline::Clamped);
	a.setControlPoints({-2, -6,
	                    -2.0 / 3, 26.0 / 3,
	                    2.0 / 3, -26.0 / 3,
	                    2, 6});
	BSpline b = line2D(-2, 0, 2, 0);
	IntersectionResult r = a.intersect(b, EPS, EPS, EPS, 5);
	check.require(!r.complete(), "tiny budget is incomplete");
	check.require(!r.boxes().empty(), "unresolved boxes are returned");
	for (const auto &bx : r.boxes())
		check.require(bx.reason() == 0, "budget reason is zero");
	return 0;
}

/* Result is an owning value: it survives destruction of the input curves
 * and a copied result. */
static int integration_owning_result(Check &check)
{
	IntersectionResult copied;
	{
		BSpline a = line2D(0, 0, 4, 2);
		BSpline b = line2D(0, 3, 4, -1);
		IntersectionResult r = a.intersect(b, EPS, EPS, EPS);
		copied = r;
	}
	check.require(copied.complete(), "copied result stays complete");
	check.require(copied.isolated().size() == 1, "copied result keeps data");
	IntersectionResult moved = std::move(copied);
	check.require(moved.isolated().size() == 1, "moved result keeps data");
	return 0;
}

/* Invalid arguments throw rather than returning a silent empty result. */
static int integration_invalid_throws(Check &check)
{
	BSpline a = line2D(0, 0, 1, 1);
	bool threw = false;
	try {
		a.intersect(a, 1e-9, EPS, EPS); /* spatial tolerance out of range */
	} catch (const std::runtime_error &) {
		threw = true;
	}
	check.require(threw, "out-of-range tolerance throws");

	threw = false;
	try {
		a.intersect(a, EPS, EPS, EPS, 0); /* zero budget */
	} catch (const std::runtime_error &) {
		threw = true;
	}
	check.require(threw, "zero budget throws");

	/* Dimension mismatch. */
	BSpline d(2, 4, 1, BSpline::Clamped);
	d.setControlPoints({0, 0, 0, 0, 1, 1, 1, 1});
	threw = false;
	try {
		a.intersect(d, EPS, EPS, EPS);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	check.require(threw, "dimension mismatch throws");
	return 0;
}

/* A constant (zero-velocity) curve is rejected explicitly. */
static int integration_constant_rejected(Check &check)
{
	BSpline a(2, 2, 1, BSpline::Clamped);
	a.setControlPoints({1, 1, 1, 1});
	bool threw = false;
	try {
		a.intersect(a, EPS, EPS, EPS);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	check.require(threw, "constant curve rejected");
	return 0;
}

/* Swapping the input curves swaps the parameter axes of every record and
 * preserves the world positions. */
static int integration_swap_symmetry(Check &check)
{
	BSpline a = line2D(0, 0, 4, 2);
	BSpline b = line2D(0, 3, 4, -1);
	IntersectionResult r1 = a.intersect(b, EPS, EPS, EPS);
	IntersectionResult r2 = b.intersect(a, EPS, EPS, EPS);
	check.require(r1.isolated().size() == r2.isolated().size(),
	               "same cardinality after swap");
	const auto v1 = r1.isolated();
	const auto v2 = r2.isolated();
	const auto &q1 = v1[0];
	const auto &q2 = v2[0];
	check.require(std::fabs(q1.u() - q2.v()) <= EPS, "u/v swapped u");
	check.require(std::fabs(q1.v() - q2.u()) <= EPS, "u/v swapped v");
	check.require(std::fabs(q1.point().x() - q2.point().x()) <= EPS,
	               "position preserved");
	return 0;
}

void register_intersections(Registry &registry)
{
	registry.add("isolated", "transversal_lines", isolated_transversal_lines);
	registry.add("isolated", "parabola_tangency", isolated_parabola_tangency);
	registry.add("isolated", "cubic_three_roots", isolated_cubic_three_roots);
	registry.add("isolated", "endpoint_contact", isolated_endpoint_contact);
	registry.add("isolated", "projection_miss_3d", isolated_3d_projection_miss);
	registry.add("isolated", "far_miss", isolated_far_miss);
	registry.add("isolated", "same_point_diff_params", isolated_same_point_diff_params);
	registry.add("isolated", "true_break_intervals", isolated_true_break_intervals);

	registry.add("overlap", "identity", overlap_identity);
	registry.add("overlap", "nonunit_domain", overlap_nonunit_domain);
	registry.add("overlap", "reversed_keeps_roots", overlap_reversed_keeps_roots);
	registry.add("overlap", "parallel_not_coincident", overlap_parallel_not_coincident);
	registry.add("overlap", "after_knot_insertion", overlap_after_knot_insertion);
	registry.add("overlap", "after_degree_elevation", overlap_after_degree_elevation);

	registry.add("integration", "small_budget_unresolved", integration_small_budget);
	registry.add("integration", "owning_value_result", integration_owning_result);
	registry.add("integration", "invalid_arguments_throw", integration_invalid_throws);
	registry.add("integration", "constant_curve_rejected", integration_constant_rejected);
	registry.add("integration", "swap_input_symmetry", integration_swap_symmetry);
}
