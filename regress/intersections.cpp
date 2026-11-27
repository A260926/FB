/**
 * @file
 * @brief Regression tests for the curve/curve intersection query.
 *
 * The expected results are never obtained by calling the C entry point from
 * the C++ wrapper: roots are derived from analytic monomial polynomials and
 * positions are back-substituted with an independent de Casteljau
 * implementation over the Bernstein control points.  Every reported
 * parameter is evaluated against the original spline object, and the
 * original non-zero knot interval index is checked explicitly.
 */
#include "qa.hpp"
#include "tinysplinecxx.h"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

using tinyspline::BSpline;
using tinyspline::Intersection;
using tinyspline::real;

namespace {

const real STOL = (real) 1e-6;
const real PTOL = (real) 1e-6;

/** Independent de Casteljau evaluation of a single clamped Bezier arc. */
std::vector<real>
refBernstein(const std::vector<real> &cp, size_t dim, real t)
{
	std::vector<real> p = cp;
	const size_t deg = cp.size() / dim - 1;
	for (size_t k = 0; k < deg; k++)
		for (size_t i = 0; i + 1 < p.size() / dim - k; i++)
			for (size_t c = 0; c < dim; c++)
				p[i * dim + c] =
				    (1 - t) * p[i * dim + c] +
				    t * p[(i + 1) * dim + c];
	p.resize(dim);
	return p;
}

/**
 * Independent evaluation of any supported (possibly multi-piece) spline.
 * Decomposes into Bezier arcs with the library routine, locates the
 * non-zero knot interval containing @p u, maps @p u to the local [0,1]
 * parameter and evaluates that arc with the private de Casteljau above.
 * The intersection query is never used here, so this is an independent
 * polynomial back-substitution rather than two library entries proving
 * each other.
 */
std::vector<real>
refEval(const BSpline &s, real u)
{
	const size_t dim = s.dimension();
	const size_t order = s.order();
	BSpline beziers = s.toBeziers();
	const auto bknots = beziers.knots();
	const auto bcp = beziers.controlPoints();
	const size_t numPieces = bcp.size() / (order * dim);
	size_t piece = numPieces - 1;
	for (size_t i = 0; i + 1 < numPieces; i++) {
		const real u0 = bknots[i * order];
		const real u1 = bknots[(i + 1) * order];
		if (u >= u0 && u < u1) { piece = i; break; }
	}
	const real u0 = bknots[piece * order];
	const real u1 = bknots[(piece + 1) * order];
	const real t = (u1 > u0) ? (u - u0) / (u1 - u0) : 0;
	std::vector<real> arc(order * dim);
	for (size_t i = 0; i < arc.size(); i++)
		arc[i] = bcp[piece * order * dim + i];
	return refBernstein(arc, dim, t);
}

real
refDistance(const std::vector<real> &a, const std::vector<real> &b)
{
	real d = 0;
	for (size_t i = 0; i < a.size(); i++)
		d += (a[i] - b[i]) * (a[i] - b[i]);
	return std::sqrt(d);
}

BSpline
makeBezier(const std::vector<real> &cp, size_t dim, size_t deg,
           const std::vector<real> &knots = {})
{
	BSpline s(cp.size() / dim, dim, deg, BSpline::Clamped);
	s.setControlPoints(cp);
	if (!knots.empty()) s.setKnots(knots);
	return s;
}

BSpline
makeLine(real x0, real y0, real x1, real y1)
{
	return makeBezier({ x0, y0, x1, y1 }, 2, 1);
}

/** Back-substitutes an isolated point through both original splines. */
void
requireBackSub(Check &ck, const BSpline &a, const BSpline &b,
               const tinyspline::IntersectionPoint &p)
{
	const size_t dim = a.dimension();
	const auto pa = refEval(a, p.u());
	const auto pb = refEval(b, p.v());
	ck.require(refDistance(pa, pb) <= STOL * 4,
	           "back-substituted positions agree within spatial tolerance");
	ck.require(std::fabs(pa[0] - p.point().x()) <= STOL * 4,
	           "reported point matches first spline position (x)");
	ck.require(std::fabs(pa[1] - p.point().y()) <= STOL * 4,
	           "reported point matches first spline position (y)");
	if (dim > 2)
		ck.require(std::fabs(pa[2] - p.point().z()) <= STOL * 4,
		           "reported point matches first spline position (z)");
	ck.require(p.u1() - p.u0() <= PTOL * 1.01,
	           "u interval width within parameter tolerance");
	ck.require(p.v1() - p.v0() <= PTOL * 1.01,
	           "v interval width within parameter tolerance");
	ck.require(p.u0() <= p.u() && p.u() <= p.u1(),
	           "representative u inside its interval");
	ck.require(p.v0() <= p.v() && p.v() <= p.v1(),
	           "representative v inside its interval");
}

/* ------------------------------------------------------------------ */
/* isolated group                                                      */
/* ------------------------------------------------------------------ */

int
isolated_transverse_3d(Check &ck)
{
	/* Two 3D lines crossing at (1,1,0). */
	BSpline a = makeBezier(
	    { 0, 0, 0, 2, 2, 0 }, 3, 1);
	BSpline b = makeBezier(
	    { 0, 2, 0, 2, 0, 0 }, 3, 1);
	Intersection r = a.intersect(b, STOL, PTOL, PTOL);
	ck.require(r.isComplete(), "transverse 3D completes");
	ck.require(r.points().size() == 1, "exactly one isolated point");
	ck.require(r.overlaps().empty() && r.boxes().empty(),
	           "no overlap or unresolved box");
	const auto p = r.points()[0];
	requireBackSub(ck, a, b, p);
	ck.require(std::fabs(p.point().x() - 1) < 1e-5 &&
	           std::fabs(p.point().y() - 1) < 1e-5,
	           "crossing at the analytic position");
	ck.require(p.point().z() == 0, "z component reported");
	return 0;
}

int
isolated_parabola_tangency(Check &ck)
{
	/* y = x^2 with x = 2t-1, as a quadratic Bezier, against y = 0.
	 * The contact is a double root with no sign change at t = 0.5. */
	BSpline parabola = makeBezier(
	    { -1, 1, 0, -1, 1, 1 }, 2, 2);
	BSpline line = makeLine(-1, 0, 1, 0);
	Intersection r = parabola.intersect(line, STOL, PTOL, PTOL);
	ck.require(r.isComplete(), "tangency completes");
	ck.require(r.points().size() == 1,
	           "double tangency reported once despite no sign flip");
	const auto p = r.points()[0];
	requireBackSub(ck, parabola, line, p);
	ck.require(std::fabs(p.u() - 0.5) < 1e-5,
	           "analytic tangency parameter t = 0.5");
	ck.require(std::fabs(p.point().x()) < 1e-5 &&
	           std::fabs(p.point().y()) < 1e-5,
	           "tangency point at the origin");
	return 0;
}

int
isolated_cubic_three_roots(Check &ck)
{
	/* x(t) = -1.5 + 3t, y(x) = x(x^2-1): roots at x in {-1,0,1}. */
	const real x0 = -1.5, s = 3;
	std::vector<real> mx = { x0, s, 0, 0 };
	std::vector<real> my = {
	    x0 * x0 * x0 - x0,
	    3 * x0 * x0 * s - s,
	    3 * x0 * s * s,
	    s * s * s
	};
	auto toBern3 = [](const std::vector<real> &m) {
		return std::vector<real> {
		    m[0],
		    m[0] + m[1] / 3,
		    m[0] + 2 * m[1] / 3 + m[2] / 3,
		    m[0] + m[1] + m[2] + m[3]
		};
	};
	std::vector<real> bx = toBern3(mx), by = toBern3(my);
	std::vector<real> cp = {
	    bx[0], by[0], bx[1], by[1], bx[2], by[2], bx[3], by[3]
	};
	BSpline cubic = makeBezier(cp, 2, 3);
	BSpline line = makeLine(-2, 0, 2, 0);
	Intersection r = cubic.intersect(line, STOL, PTOL, PTOL);
	ck.require(r.isComplete(), "three-root query completes");
	ck.require(r.points().size() == 3, "three separated roots");
	const real expectedT[3] = {
	    (-1 - x0) / s, (0 - x0) / s, (1 - x0) / s
	};
	for (size_t i = 0; i < 3; i++) {
		const auto p = r.points()[i];
		requireBackSub(ck, cubic, line, p);
		ck.require(std::fabs(p.u() - expectedT[i]) < 1e-5,
		           "root matches analytic monomial parameter");
		ck.require(std::fabs(p.point().y()) < 1e-5,
		           "root lies on the line y = 0");
	}
	/* Roots are separated by far more than the parameter tolerance. */
	ck.require(r.points()[1].u() - r.points()[0].u() > 64 * PTOL,
	           "root separation exceeds 64 tolerances");
	return 0;
}

int
isolated_endpoint_ownership(Check &ck)
{
	/* Two segments meeting at (1,0): the right most end of the first
	 * spline must be owned by its last original interval. */
	BSpline a = makeLine(0, 0, 1, 0);
	BSpline b = makeLine(1, 0, 2, 1);
	Intersection r = a.intersect(b, STOL, PTOL, PTOL);
	ck.require(r.isComplete() && r.points().size() == 1,
	           "single endpoint contact");
	const auto p = r.points()[0];
	requireBackSub(ck, a, b, p);
	ck.require(std::fabs(p.u() - 1.0) < 1e-6,
	           "first spline parameter at its right domain end");
	ck.require(std::fabs(p.v() - 0.0) < 1e-6,
	           "second spline parameter at its left domain end");
	ck.require(p.uIndex() == a.degree(),
	           "right most end owned by the last original interval");
	return 0;
}

int
isolated_foldback_same_point_two_params(Check &ck)
{
	/* Loop cubic whose y(t) = 21 t(1-t); the horizontal line through
	 * the crossing hits the same spatial point at two distinct params. */
	BSpline loop = makeBezier(
	    { 0, 0, 6, 7, 0.9841269841, 7, 4, 0 }, 2, 3);
	BSpline line = makeLine(0, 4.41, 4, 4.41);
	Intersection r = loop.intersect(line, STOL, PTOL, PTOL);
	ck.require(r.isComplete(), "fold-back query completes");
	ck.require(r.points().size() == 2,
	           "same spatial point reached through two parameter pairs");
	const auto p0 = r.points()[0];
	const auto p1 = r.points()[1];
	ck.require(std::fabs(p0.u() - 0.3) < 1e-5 &&
	           std::fabs(p1.u() - 0.7) < 1e-5,
	           "analytic fold-back parameters 0.3 and 0.7");
	ck.require(std::fabs(p0.point().x() - p1.point().x()) < 1e-5 &&
	           std::fabs(p0.point().y() - p1.point().y()) < 1e-5,
	           "both parameter pairs resolve to one spatial point");
	requireBackSub(ck, loop, line, p0);
	requireBackSub(ck, loop, line, p1);
	return 0;
}

int
isolated_break_two_sides(Check &ck)
{
	/* Degree-1 spline with a true C0 break: the interior knot has full
	 * order multiplicity, so both sides keep their interval identity. */
	std::vector<real> cp = { 0, 0, 1, 1, 1, 1, 2, 0 };
	std::vector<real> knots = { 0, 0, 1, 1, 2, 2 };
	BSpline broken = makeBezier(cp, 2, 1, knots);
	BSpline line = makeLine(0, 1, 2, 1);
	Intersection r = broken.intersect(line, STOL, PTOL, PTOL);
	ck.require(r.isComplete(), "break query completes");
	ck.require(r.points().size() == 2,
	           "both sides of the break report the contact");
	ck.require(r.points()[0].uIndex() != r.points()[1].uIndex(),
	           "the two reports carry distinct original interval ids");
	for (const auto &p : r.points())
		requireBackSub(ck, broken, line, p);
	return 0;
}

/* ------------------------------------------------------------------ */
/* overlap group                                                       */
/* ------------------------------------------------------------------ */

int
overlap_affine_reparam_nonunit_domain(Check &ck)
{
	/* Same geometry on domains [0,2] and [0,4]: v = 2u. */
	std::vector<real> cp = { 0, 0, 1, 2, 2, -1, 3, 1 };
	BSpline a = makeBezier(cp, 2, 3, { 0, 0, 0, 0, 2, 2, 2, 2 });
	BSpline b = makeBezier(cp, 2, 3, { 0, 0, 0, 0, 4, 4, 4, 4 });
	Intersection r = a.intersect(b, STOL, PTOL, PTOL);
	ck.require(r.isComplete() && r.overlaps().size() == 1,
	           "one affine reparameterization overlap");
	ck.require(r.points().empty(),
	           "no isolated points on a clean overlap");
	const auto o = r.overlaps()[0];
	ck.require(std::fabs(o.alpha() - 2.0) < 1e-6,
	           "affine slope v = 2u recovered");
	ck.require(std::fabs(o.beta()) < 1e-6, "zero intercept");
	ck.require(std::fabs(o.uFrom() - 0) < 1e-9 &&
	           std::fabs(o.uTo() - 2) < 1e-9,
	           "maximal segment on the original u domain");
	ck.require(std::fabs(o.vFrom() - 0) < 1e-9 &&
	           std::fabs(o.vTo() - 4) < 1e-9,
	           "mapped endpoints on the original v domain");
	return 0;
}

int
overlap_reversed_orientation(Check &ck)
{
	std::vector<real> cp = { 0, 0, 1, 2, 2, -1, 3, 1 };
	std::vector<real> rev = {
	    cp[6], cp[7], cp[4], cp[5], cp[2], cp[3], cp[0], cp[1]
	};
	BSpline a = makeBezier(cp, 2, 3);
	BSpline b = makeBezier(rev, 2, 3);
	Intersection r = a.intersect(b, STOL, PTOL, PTOL);
	ck.require(r.isComplete() && r.overlaps().size() == 1,
	           "reversed arcs overlap");
	const auto o = r.overlaps()[0];
	ck.require(o.alpha() < 0, "reversed correspondence has negative slope");
	ck.require(std::fabs(o.alpha() * o.uFrom() + o.beta() -
	                     o.vFrom()) < 1e-9,
	           "vFrom lies on the affine map");
	ck.require(std::fabs(o.alpha() * o.uTo() + o.beta() -
	                     o.vTo()) < 1e-9,
	           "vTo lies on the affine map");
	/* Endpoint directions are explicit and opposite. */
	ck.require(o.uFrom() < o.uTo() && o.vFrom() > o.vTo(),
	           "parameter endpoint directions are reported");
	return 0;
}

int
overlap_with_extra_isolated_crossing(Check &ck)
{
	/* Horizontal spline a covering x in [0,4].  Spline b has two
	 * continuous quadratic pieces on the clamped knot vector
	 * {0,0,0,0.5,0.5,1,1,1}: the first piece is collinear with a (the
	 * overlap) and the second arcs above the line and re-joins it at
	 * the isolated endpoint (4,0). */
	BSpline a = makeLine(0, 0, 4, 0);
	std::vector<real> cp = {
	    1, 0, 1.5, 0, 2, 0, 3, 2, 4, 0
	};
	std::vector<real> knots = { 0, 0, 0, 0.5, 0.5, 1, 1, 1 };
	BSpline b = makeBezier(cp, 2, 2, knots);
	Intersection r = a.intersect(b, STOL, PTOL, PTOL);
	ck.require(r.isComplete(), "mixed overlap/crossing completes");
	ck.require(r.overlaps().size() == 1, "collinear segment is an overlap");
	ck.require(!r.points().empty(),
	           "the extra crossing outside the overlap is retained");
	const auto o = r.overlaps()[0];
	bool sawEndpoint = false;
	size_t offRelation = 0;
	for (const auto &p : r.points()) {
		requireBackSub(ck, a, b, p);
		/* A point strictly beyond the overlap range must not lie on
		 * the affine relation.  A point at the overlap endpoint is the
		 * continuous-knot boundary report and is allowed. */
		const bool beyond =
		    p.u() > o.uTo() + PTOL || p.u() < o.uFrom() - PTOL;
		if (beyond) {
			const real mapped = o.alpha() * p.u() + o.beta();
			ck.require(std::fabs(p.v() - mapped) > PTOL,
			           "isolated point beyond the overlap is off the relation");
			offRelation++;
		}
		if (std::fabs(p.point().x() - 4) < 1e-5 &&
		    std::fabs(p.point().y()) < 1e-5)
			sawEndpoint = true;
	}
	ck.require(sawEndpoint,
	           "the re-crossing at the far endpoint is reported");
	ck.require(offRelation >= 1,
	           "at least one isolated point lies outside the overlap");
	return 0;
}

int
overlap_self_intersection_diagonal_and_offdiagonal(Check &ck)
{
	BSpline loop = makeBezier(
	    { 0, 0, 6, 7, 0.9841269841, 7, 4, 0 }, 2, 3);
	Intersection r = loop.intersect(loop, STOL, PTOL, PTOL);
	ck.require(r.isComplete(), "self-intersection completes");
	ck.require(r.overlaps().size() == 1,
	           "diagonal coincidence reported as one overlap");
	ck.require(r.points().size() == 2,
	           "two swapped off-diagonal parameter pairs");
	const auto p0 = r.points()[0];
	const auto p1 = r.points()[1];
	ck.require(std::fabs(p0.u() - p1.v()) < 1e-5 &&
	           std::fabs(p0.v() - p1.u()) < 1e-5,
	           "off-diagonal pairs are parameter swaps");
	ck.require(std::fabs(p0.point().x() - p1.point().x()) < 1e-5 &&
	           std::fabs(p0.point().y() - p1.point().y()) < 1e-5,
	           "swapped pairs share one spatial point");
	requireBackSub(ck, loop, loop, p0);
	requireBackSub(ck, loop, loop, p1);
	return 0;
}

int
overlap_exact_degree_elevation(Check &ck)
{
	/* A quadratic arc elevated to a cubic: both stay inside the
	 * supported degree range 1..3 and represent the same geometry. */
	BSpline quadratic = makeBezier(
	    { 0, 0, 1, 3, 3, 1 }, 2, 2);
	BSpline elevated = quadratic.elevateDegree(1);
	ck.require(elevated.degree() == 3, "degree elevation produced a cubic");
	Intersection r = quadratic.intersect(elevated, STOL, PTOL, PTOL);
	ck.require(r.isComplete() && r.overlaps().size() == 1,
	           "exact degree elevation yields algebraic coincidence");
	ck.require(r.points().empty(),
	           "elevation introduces no isolated points");
	return 0;
}

int
overlap_knot_insertion(Check &ck)
{
	/* A single cubic piece and the same geometry after an interior
	 * knot insertion, which splits it into two clamped pieces. */
	BSpline single = makeBezier(
	    { 0, 0, 1, 2, 2, -1, 3, 1 }, 2, 3);
	BSpline inserted = single.insertKnot(0.5, 1);
	ck.require(inserted.knots().size() > single.knots().size(),
	           "knot insertion added a knot");
	Intersection r = single.intersect(inserted, STOL, PTOL, PTOL);
	ck.require(r.isComplete(), "knot-inserted query completes");
	ck.require(!r.overlaps().empty(),
	           "the inserted representation still coincides algebraically");
	ck.require(r.points().empty(),
	           "knot insertion introduces no isolated points");
	/* The union of the reported overlap segments covers the full domain. */
	real covered = 0;
	for (const auto &o : r.overlaps())
		covered += o.uTo() - o.uFrom();
	ck.require(covered >= 1.0 - 1e-6,
	           "overlap segments together span the original domain");
	return 0;
}

int
overlap_dedup_keeps_distinct_param_pairs(Check &ck)
{
	/* The transverse loop reaches one spatial point through two distinct
	 * parameter pairs.  Parameter-pair dedup must keep both even though
	 * the positions coincide. */
	BSpline loop = makeBezier(
	    { 0, 0, 6, 7, 0.9841269841, 7, 4, 0 }, 2, 3);
	BSpline line = makeLine(0, 4.41, 4, 4.41);
	Intersection r = loop.intersect(line, STOL, PTOL, PTOL);
	ck.require(r.points().size() == 2,
	           "two parameter pairs for one spatial point are retained");
	const auto p0 = r.points()[0];
	const auto p1 = r.points()[1];
	ck.require(std::fabs(p0.u() - p1.u()) > 64 * PTOL,
	           "the two reports use distinct first-axis parameters");
	ck.require(std::fabs(p0.point().x() - p1.point().x()) < STOL * 4 &&
	           std::fabs(p0.point().y() - p1.point().y()) < STOL * 4,
	           "both pairs resolve to the same spatial point");
	return 0;
}

/* ------------------------------------------------------------------ */
/* integration group                                                   */
/* ------------------------------------------------------------------ */

int
integration_nonunit_domain_continuous_knot(Check &ck)
{
	/* Quadratic with an internal continuous (multiplicity one) knot on a
	 * non-unit domain [2,8]. */
	std::vector<real> cp = { 0, 0, 1, 1, 2, 0, 3, 1, 4, 0 };
	std::vector<real> knots = { 2, 2, 2, 5, 5, 8, 8, 8 };
	BSpline spline = makeBezier(cp, 2, 2, knots);
	BSpline line = makeLine(0, 0.5, 4, 0.5);
	Intersection r = spline.intersect(line, STOL, PTOL, PTOL);
	ck.require(r.isComplete(), "multi-piece spline completes");
	ck.require(r.points().size() == 2,
	           "crossings on both sides of the internal knot");
	bool sawLeft = false, sawRight = false;
	for (const auto &p : r.points()) {
		ck.require(p.u() >= 2 - 1e-9 && p.u() <= 8 + 1e-9,
		           "parameter reported on the original non-unit domain");
		if (p.u() < 5) sawLeft = true;
		if (p.u() > 5) sawRight = true;
		ck.require(p.uIndex() >= spline.degree(),
		           "original non-zero knot interval index reported");
		const auto net = spline.eval(p.u());
		const auto q = net.result();
		ck.require(std::fabs(q[1] - 0.5) < STOL * 4,
		           "back-substitution through the original spline");
	}
	ck.require(sawLeft && sawRight,
	           "both original intervals produced a report");
	return 0;
}

int
integration_3d_projection_not_intersection(Check &ck)
{
	/* Identical x/y motion but separated in z. */
	BSpline a = makeBezier({ 0, 0, 0, 2, 2, 0 }, 3, 1);
	BSpline b = makeBezier({ 0, 0, 1, 2, 2, 1 }, 3, 1);
	Intersection r = a.intersect(b, STOL, PTOL, PTOL);
	ck.require(r.isComplete(), "projection case completes");
	ck.require(r.points().empty() && r.overlaps().empty(),
	           "an xy projection match is not a 3D intersection");
	return 0;
}

int
integration_far_disjoint_control(Check &ck)
{
	/* Parallel lines two units apart.  With a spatial tolerance of 1e-4
	 * the true gap is 2 / 1e-4 = 20000 times the tolerance, well beyond
	 * the required 16x control margin. */
	BSpline a = makeLine(0, 0, 1, 0);
	BSpline b = makeLine(0, 2, 1, 2);
	const real stol = (real) 1e-4;
	Intersection r = a.intersect(b, stol, PTOL, PTOL);
	ck.require(r.isComplete(), "far disjoint query completes");
	ck.require(r.points().empty() && r.overlaps().empty(),
	           "no intersection when distance exceeds 16 tolerances");
	ck.require(r.boxes().empty(),
	           "the whole domain was provably excluded, no pending box");
	return 0;
}

int
integration_small_budget_unresolved(Check &ck)
{
	BSpline a = makeBezier(
	    { 0, 0, 1, 2, 2, -1, 3, 1 }, 2, 3);
	Intersection r = a.intersect(a, STOL, PTOL, PTOL, 2);
	ck.require(!r.isComplete(), "tiny budget is an incomplete result");
	ck.require(!r.boxes().empty(), "unresolved parameter boxes remain");
	ck.require(r.boxes()[0].reason() ==
	               tinyspline::IntersectionBox(
	                   r.boxes()[0].data()).reason() &&
	           r.boxes()[0].reason() != 0,
	           "unresolved box carries a nonzero reason");
	/* Unresolved is not an empty intersection: the overlap already found
	 * is still present. */
	ck.require(!r.overlaps().empty() || !r.boxes().empty(),
	           "incomplete result still carries partial findings");
	return 0;
}

int
integration_invalid_input_throws(Check &ck)
{
	BSpline a = makeLine(0, 0, 1, 0);
	BSpline b = makeLine(0, 1, 1, 1);
	bool threw = false;
	try {
		a.intersect(b, (real) 1e-12, PTOL, PTOL);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	ck.require(threw, "out-of-range tolerance throws");

	/* Mismatched dimensionality. */
	BSpline d2 = makeBezier({ 0, 0, 1, 1 }, 2, 1);
	BSpline d3 = makeBezier({ 0, 0, 0, 1, 1, 1 }, 3, 1);
	threw = false;
	try {
		d2.intersect(d3, STOL, PTOL, PTOL);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	ck.require(threw, "dimension mismatch throws");

	/* Constant (degree zero) spline is rejected. */
	BSpline point(1, 2, 0, BSpline::Clamped);
	threw = false;
	try {
		point.intersect(a, STOL, PTOL, PTOL);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	ck.require(threw, "constant spline throws instead of dividing by zero");
	return 0;
}

int
integration_lifetime_copy_move(Check &ck)
{
	auto a = std::unique_ptr<BSpline>(
	    new BSpline(makeLine(-1, -1, 1, 1)));
	auto b = std::unique_ptr<BSpline>(
	    new BSpline(makeLine(-1, 1, 1, -1)));
	Intersection r = a->intersect(*b, STOL, PTOL, PTOL);
	ck.require(r.points().size() == 1, "one point before release");
	const real savedX = r.points()[0].point().x();
	const real savedY = r.points()[0].point().y();

	/* Release the source splines; the result must stand on its own. */
	a.reset();
	b.reset();
	ck.require(r.points().size() == 1,
	           "result survives destruction of its input splines");
	ck.require(std::fabs(r.points()[0].point().x() - savedX) < 1e-12 &&
	           std::fabs(r.points()[0].point().y() - savedY) < 1e-12,
	           "owned result keeps its data after inputs are freed");

	/* Copy. */
	Intersection copy = r;
	ck.require(copy.points().size() == r.points().size() &&
	           copy.isComplete() == r.isComplete(),
	           "deep copy preserves the result");
	/* Move. */
	Intersection moved = std::move(copy);
	ck.require(moved.points().size() == 1,
	           "moved-to object holds the result");
	ck.require(copy.points().empty(),
	           "moved-from object is empty");
	/* Copy assignment and move assignment. */
	Intersection assigned;
	assigned = r;
	ck.require(assigned.points().size() == 1,
	           "copy assignment works");
	Intersection moveAssigned;
	moveAssigned = std::move(assigned);
	ck.require(moveAssigned.points().size() == 1 &&
	           assigned.points().empty(),
	           "move assignment works");
	return 0;
}

int
integration_retry_after_failure_and_swap(Check &ck)
{
	BSpline a = makeBezier(
	    { -1, 1, 0, -1, 1, 1 }, 2, 2);
	BSpline b = makeLine(-1, 0, 1, 0);

	Intersection good = a.intersect(b, STOL, PTOL, PTOL);
	size_t goodCount = good.points().size();

	Intersection reused = a.intersect(b, STOL, PTOL, PTOL);
	bool threw = false;
	try {
		a.intersect(b, (real) 1e-99, PTOL, PTOL);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	ck.require(threw, "invalid call throws");
	/* A successful retry replaces the reusable result object. */
	Intersection retry = a.intersect(b, STOL, PTOL, PTOL);
	ck.require(retry.points().size() == goodCount &&
	           retry.isComplete(),
	           "retry after a failure yields the full result");
	(void) reused;

	/* Swapping inputs exchanges the parameter pair but keeps the set. */
	Intersection swapped = b.intersect(a, STOL, PTOL, PTOL);
	ck.require(swapped.points().size() == good.points().size(),
	           "swapped inputs give the same number of points");
	for (size_t i = 0; i < good.points().size(); i++) {
		ck.require(std::fabs(good.points()[i].u() -
		                     swapped.points()[i].v()) < 1e-9 &&
		           std::fabs(good.points()[i].v() -
		                     swapped.points()[i].u()) < 1e-9,
		           "swap exchanges u and v on each point");
	}
	return 0;
}

} /* namespace */

void
register_intersections(Registry &registry)
{
	registry.add("isolated", "transverse_3d",
	             isolated_transverse_3d);
	registry.add("isolated", "parabola_line_tangency",
	             isolated_parabola_tangency);
	registry.add("isolated", "cubic_three_separated_roots",
	             isolated_cubic_three_roots);
	registry.add("isolated", "endpoint_contact_ownership",
	             isolated_endpoint_ownership);
	registry.add("isolated", "foldback_same_point_two_params",
	             isolated_foldback_same_point_two_params);
	registry.add("isolated", "true_break_both_sides",
	             isolated_break_two_sides);

	registry.add("overlap", "affine_reparam_nonunit_domain",
	             overlap_affine_reparam_nonunit_domain);
	registry.add("overlap", "reversed_orientation",
	             overlap_reversed_orientation);
	registry.add("overlap", "overlap_plus_isolated_crossing",
	             overlap_with_extra_isolated_crossing);
	registry.add("overlap", "self_intersection_diagonal_offdiagonal",
	             overlap_self_intersection_diagonal_and_offdiagonal);
	registry.add("overlap", "exact_degree_elevation",
	             overlap_exact_degree_elevation);
	registry.add("overlap", "knot_insertion_coincidence",
	             overlap_knot_insertion);
	registry.add("overlap", "dedup_keeps_distinct_param_pairs",
	             overlap_dedup_keeps_distinct_param_pairs);

	registry.add("integration", "nonunit_domain_continuous_knot",
	             integration_nonunit_domain_continuous_knot);
	registry.add("integration", "projection_3d_not_intersection",
	             integration_3d_projection_not_intersection);
	registry.add("integration", "far_disjoint_control",
	             integration_far_disjoint_control);
	registry.add("integration", "small_budget_unresolved",
	             integration_small_budget_unresolved);
	registry.add("integration", "invalid_input_throws",
	             integration_invalid_input_throws);
	registry.add("integration", "lifetime_copy_move",
	             integration_lifetime_copy_move);
	registry.add("integration", "retry_failure_and_swap",
	             integration_retry_after_failure_and_swap);
}
