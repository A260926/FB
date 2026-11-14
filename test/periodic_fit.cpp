/*
 * Tests for ts_bspline_fit_periodic_cubic / BSpline::fitPeriodicCubic.
 *
 * The tests are organized into three groups: `periodic', `objective', and
 * `api'. They deliberately verify the production implementation against an
 * independent reference (a nodal periodic cubic spline with moments and a
 * truncated-power B-spline basis) rather than comparing the two new
 * interfaces with each other.
 */

#include "task_support/check.h"
#include <tinyspline.h>
#include <tinysplinecxx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

/* ------------------------------------------------------------------------ */
/* Independent reference implementation                                     */
/* ------------------------------------------------------------------------ */

namespace ref {

static size_t
idx(size_t r, size_t c, size_t n)
{
	return r * n + c;
}

/*
 * Solves the dense system A x = b (A row-major n x n) with Gaussian
 * elimination and partial pivoting. Returns false if A is singular.
 */
static bool
solve(std::vector<double> A, std::vector<double> &b, size_t n)
{
	for (size_t col = 0; col < n; ++col) {
		size_t piv = col;
		for (size_t r = col + 1; r < n; ++r)
			if (std::fabs(A[idx(r, col, n)]) >
			    std::fabs(A[idx(piv, col, n)]))
				piv = r;
		if (std::fabs(A[idx(piv, col, n)]) < 1e-13)
			return false;
		if (piv != col) {
			for (size_t k = col; k < n; ++k)
				std::swap(A[idx(col, k, n)],
				          A[idx(piv, k, n)]);
			std::swap(b[col], b[piv]);
		}
		for (size_t r = col + 1; r < n; ++r) {
			const double f = A[idx(r, col, n)] /
			                 A[idx(col, col, n)];
			if (f == 0.0) continue;
			A[idx(r, col, n)] = 0.0;
			for (size_t k = col + 1; k < n; ++k)
				A[idx(r, k, n)] -= f * A[idx(col, k, n)];
			b[r] -= f * b[col];
		}
	}
	for (size_t i = n; i-- > 0; ) {
		double s = b[i];
		for (size_t k = i + 1; k < n; ++k)
			s -= A[idx(i, k, n)] * b[k];
		b[i] = s / A[idx(i, i, n)];
	}
	return true;
}

/*
 * Solves the cyclic tridiagonal system with lower entries a, diagonal b,
 * upper c (each of length m, all indices modulo m) and right-hand side rhs.
 */
static std::vector<double>
solve_cyclic(const std::vector<double> &a,
             const std::vector<double> &b,
             const std::vector<double> &c,
             const std::vector<double> &rhs)
{
	const size_t m = b.size();
	std::vector<double> M(m * m, 0.0), x(rhs);
	for (size_t i = 0; i < m; ++i) {
		M[idx(i, i, m)] += b[i];
		M[idx(i, (i + m - 1) % m, m)] += a[i];
		M[idx(i, (i + 1) % m, m)] += c[i];
	}
	if (!solve(M, x, m))
		throw std::runtime_error("reference cyclic solve failed");
	return x;
}

/*
 * Returns the second derivatives (moments) M_0 .. M_{m-1} of the periodic
 * cubic spline that interpolates the nodal values y_0 .. y_{m-1} at the
 * parameters x_0 .. x_m (with x_m = x_0 + period and y_m = y_0). The
 * reference uses the classic periodic moment equations, i.e., an entirely
 * different formulation than the production normal equations.
 */
static std::vector<double>
moments(const std::vector<double> &x, const std::vector<double> &y)
{
	const size_t m = y.size();
	std::vector<double> a(m), b(m), c(m), rhs(m), h(m);
	for (size_t i = 0; i < m; ++i)
		h[i] = x[i + 1] - x[i];
	for (size_t i = 0; i < m; ++i) {
		const double hm = h[(i + m - 1) % m];
		const double hi = h[i];
		const double ym = y[(i + m - 1) % m];
		const double yc = y[i];
		const double yp = y[(i + 1) % m];
		a[i] = hm;
		b[i] = 2.0 * (hm + hi);
		c[i] = hi;
		rhs[i] = 6.0 * ((yp - yc) / hi - (yc - ym) / hm);
	}
	return solve_cyclic(a, b, c, rhs);
}

/*
 * Evaluates the reference periodic cubic spline and/or its derivatives at tv.
 * The spline is the piecewise cubic with nodal values y and moments M.
 */
static std::vector<double>
eval_ref(const std::vector<double> &x,
         const std::vector<std::vector<double> > &y,
         size_t dim,
         double tv,
         int order)
{
	const size_t m = y[0].size();
	if (tv >= x[m] - 1e-12)
		tv = x[0];
	size_t i = 0;
	for (size_t k = 0; k < m; ++k) {
		if (x[k] <= tv && tv <= x[k + 1]) {
			i = k;
			break;
		}
	}
	const double h = x[i + 1] - x[i];
	const double s = tv - x[i];
	std::vector<double> out(dim, 0.0);
	for (size_t d = 0; d < dim; ++d) {
		const std::vector<double> M = moments(x, y[d]);
		const double Mi = M[i];
		const double Mi1 = M[(i + 1) % m];
		const double b = (y[d][(i + 1) % m] - y[d][i]) / h
		               - h * (2.0 * Mi + Mi1) / 6.0;
		if (order == 0)
			out[d] = y[d][i] + b * s + Mi / 2.0 * s * s
			       + (Mi1 - Mi) / (6.0 * h) * s * s * s;
		else if (order == 1)
			out[d] = b + Mi * s + (Mi1 - Mi) / (2.0 * h) * s * s;
		else
			out[d] = Mi + (Mi1 - Mi) / h * s;
	}
	return out;
}

/*
 * Returns the exact value of the regularization term
 *
 *     integral_{x_0}^{x_m} |C''(t)|^2 dt
 *
 * using the piecewise-linear second derivative implied by the moments:
 * on [x_i, x_{i+1}], C'' is the linear interpolation of M_i and M_{i+1}.
 * The integral of a squared linear function over a segment of length h is
 * h/3 * (M_i^2 + M_i M_{i+1} + M_{i+1}^2).
 */
static double
energy(const std::vector<double> &x,
       const std::vector<std::vector<double> > &y,
       size_t dim)
{
	const size_t m = y[0].size();
	double total = 0.0;
	for (size_t d = 0; d < dim; ++d) {
		const std::vector<double> M = moments(x, y[d]);
		for (size_t i = 0; i < m; ++i) {
			const double h = x[i + 1] - x[i];
			const double Mi = M[i];
			const double Mi1 = M[(i + 1) % m];
			total += h / 3.0 *
				(Mi * Mi + Mi * Mi1 + Mi1 * Mi1);
		}
	}
	return total;
}

/*
 * Minimizes the objective J = sum_i w_i |y_i - P_i|^2 + lambda * E(y)
 * over the nodal values y_i, subject to y_i = P_i for the hard samples.
 * This is a completely independent derivation: instead of the normal
 * equations of the B-spline basis, it builds the moment matrix by solving
 * the periodic moment system for unit vectors and substitutes the hard
 * constraints directly.
 */
static std::vector<std::vector<double> >
fit_nodal(const std::vector<double> &x,
          const std::vector<std::vector<double> > &P,
          const std::vector<double> &w,
          const std::vector<int> &hard,
          double lambda,
          size_t dim)
{
	const size_t m = P.size();
	std::vector<double> h(m);
	for (size_t i = 0; i < m; ++i)
		h[i] = x[i + 1] - x[i];

	/* Moment matrix T: M = T y. */
	std::vector<double> T(m * m, 0.0);
	for (size_t j = 0; j < m; ++j) {
		std::vector<double> unit(m, 0.0);
		unit[j] = 1.0;
		const std::vector<double> col = moments(x, unit);
		for (size_t i = 0; i < m; ++i)
			T[idx(i, j, m)] = col[i];
	}
	/* Gram matrix of the energy. */
	std::vector<double> G(m * m, 0.0);
	for (size_t a = 0; a < m; ++a) {
		for (size_t b = 0; b < m; ++b) {
			double val = 0.0;
			for (size_t i = 0; i < m; ++i) {
				const double Mia = T[idx(i, a, m)];
				const double Mib = T[idx(i, b, m)];
				const double Mia1 =
					T[idx((i + 1) % m, a, m)];
				const double Mib1 =
					T[idx((i + 1) % m, b, m)];
				val += h[i] / 3.0 *
					(Mia * Mib +
					 0.5 * (Mia * Mib1 + Mia1 * Mib) +
					 Mia1 * Mib1);
			}
			G[idx(a, b, m)] = lambda * val;
		}
	}
	for (size_t i = 0; i < m; ++i)
		if (!hard[i]) G[idx(i, i, m)] += w[i];

	std::vector<size_t> freeidx;
	for (size_t i = 0; i < m; ++i)
		if (!hard[i]) freeidx.push_back(i);
	const size_t n = freeidx.size();

	std::vector<std::vector<double> > y(dim,
		std::vector<double>(m, 0.0));
	for (size_t d = 0; d < dim; ++d) {
		for (size_t i = 0; i < m; ++i)
			if (hard[i]) y[d][i] = P[i][d];
		if (n == 0)
			continue;
		std::vector<double> A(n * n, 0.0), rhs(n, 0.0);
		for (size_t r = 0; r < n; ++r) {
			const size_t i = freeidx[r];
			for (size_t c = 0; c < n; ++c)
				A[idx(r, c, n)] = G[idx(i, freeidx[c], m)];
			double v = w[i] * P[i][d];
			for (size_t k = 0; k < m; ++k)
				if (hard[k])
					v -= G[idx(i, k, m)] * P[k][d];
			rhs[r] = v;
		}
		if (!solve(A, rhs, n))
			throw std::runtime_error("reference nodal solve failed");
		for (size_t r = 0; r < n; ++r)
			y[d][freeidx[r]] = rhs[r];
	}
	return y;
}

/*
 * Returns the periodic knot vector of the spline produced by the
 * production implementation.
 */
static std::vector<double>
periodic_knots(const std::vector<double> &t)
{
	const size_t m = t.size() - 1;
	const double L = t[m] - t[0];
	std::vector<double> U;
	U.push_back(t[m - 3] - L);
	U.push_back(t[m - 2] - L);
	U.push_back(t[m - 1] - L);
	for (size_t i = 0; i <= m; ++i)
		U.push_back(t[i]);
	U.push_back(t[1] + L);
	U.push_back(t[2] + L);
	U.push_back(t[3] + L);
	return U;
}

/*
 * Evaluates the j'th B-spline basis function of degree p at t using the
 * truncated-power (divided difference) representation
 *
 *     N_{j,p}(t) = (u_{j+p+1} - u_j) * sum_k (u_{j+k} - t)_+^p
 *                                    / prod_{l != k} (u_{j+l} - u_{j+k}).
 *
 * This is independent of the recursive Cox-de Boor implementation used by
 * the production code. Only valid for simple (non-repeated) knots.
 */
static double
tp_basis(const std::vector<double> &U,
         size_t j,
         size_t p,
         double t)
{
	double s = 0.0;
	for (size_t k = 0; k <= p + 1; ++k) {
		double den = 1.0;
		for (size_t l = 0; l <= p + 1; ++l) {
			if (l != k)
				den *= U[j + l] - U[j + k];
		}
		const double pos = U[j + k] - t > 0.0
		                 ? U[j + k] - t : 0.0;
		s += std::pow(pos, (double) p) / den;
	}
	return (U[j + p + 1] - U[j]) * s;
}

/*
 * Second derivative of tp_basis at t.
 */
static double
tp_basis_d2(const std::vector<double> &U,
            size_t j,
            size_t p,
            double t)
{
	double s = 0.0;
	for (size_t k = 0; k <= p + 1; ++k) {
		double den = 1.0;
		for (size_t l = 0; l <= p + 1; ++l) {
			if (l != k)
				den *= U[j + l] - U[j + k];
		}
		const double pos = U[j + k] - t > 0.0
		                 ? U[j + k] - t : 0.0;
		if (p >= 2)
			s += (double) p * (double) (p - 1) *
			     std::pow(pos, (double) (p - 2)) / den;
	}
	return (U[j + p + 1] - U[j]) * s;
}

/*
 * Computes the control points of the periodic spline that interpolates the
 * given nodal values: solves the collocation system in the independent
 * truncated-power basis and wraps the resulting m independent control points
 * into the cyclic (m+3)-point net.
 */
static std::vector<double>
collocation_net(const std::vector<double> &t,
                const std::vector<std::vector<double> > &y,
                size_t dim)
{
	const size_t m = y[0].size();
	const std::vector<double> U = periodic_knots(t);
	std::vector<double> A(m * m, 0.0);
	for (size_t i = 0; i < m; ++i) {
		for (size_t j = 0; j < 4; ++j)
			A[idx(i, (i + j) % m, m)] +=
				tp_basis(U, i + j, 3, t[i]);
	}
	std::vector<double> net(dim * (m + 3), 0.0);
	for (size_t d = 0; d < dim; ++d) {
		std::vector<double> rhs(m);
		for (size_t i = 0; i < m; ++i)
			rhs[i] = y[d][i];
		if (!solve(A, rhs, m))
			throw std::runtime_error("reference collocation failed");
		for (size_t j = 0; j < m + 3; ++j)
			net[j * dim + d] = rhs[j % m];
	}
	return net;
}

} /* namespace ref */

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

namespace {

static const double TOL = 1e-7;

struct FitCase {
	std::vector<tinyspline::real> points;
	std::vector<tinyspline::real> parameters;
	std::vector<tinyspline::real> weights;
	std::vector<int> hard;
	tinyspline::real lambda;
	size_t dimension;
};

/* Collects the components of points as a vector of vectors. */
static std::vector<std::vector<double> >
as_points(const std::vector<tinyspline::real> &points, size_t dim)
{
	const size_t m = points.size() / dim;
	std::vector<std::vector<double> > out(
		m, std::vector<double>(dim, 0.0));
	for (size_t i = 0; i < m; ++i)
		for (size_t d = 0; d < dim; ++d)
			out[i][d] = points[i * dim + d];
	return out;
}

static std::vector<double>
as_doubles(const std::vector<tinyspline::real> &v)
{
	return std::vector<double>(v.begin(), v.end());
}

/* Evaluates the spline at tv and returns the result components. */
static std::vector<double>
eval_at(const tinyspline::BSpline &spline,
        double tv,
        int order = 0)
{
	tinyspline::BSpline s = spline;
	if (order > 0)
		s = spline.derive((size_t) order);
	std::vector<tinyspline::real> r =
		s.eval((tinyspline::real) tv).result();
	return std::vector<double>(r.begin(), r.end());
}

static double
max_abs_diff(const std::vector<double> &a, const std::vector<double> &b)
{
	double m = 0.0;
	for (size_t i = 0; i < a.size() && i < b.size(); ++i)
		m = std::max(m, std::fabs(a[i] - b[i]));
	return m;
}

/* Returns 0 if the vectors are component-wise within tol, 1 otherwise. */
static int
check_close(const std::vector<double> &got,
            const std::vector<double> &want,
            double tol,
            const char *what)
{
	int ok = got.size() == want.size();
	double worst = ok ? max_abs_diff(got, want) : 0.0;
	ok = ok && worst <= tol;
	if (!ok)
		std::fprintf(stderr, "%s: diff=%g > %g\n", what, worst, tol);
	return task_check(ok, what, __FILE__, __LINE__) ? 0 : 1;
}

struct NodalCtx {
	std::vector<double> t;
	std::vector<std::vector<double> > y;
	size_t dim;
};

static std::vector<double>
nodal_eval(const void *ctx, double tv, int order)
{
	const NodalCtx *c = (const NodalCtx *) ctx;
	return ref::eval_ref(c->t, c->y, c->dim, tv, order);
}

typedef std::vector<double> (*RefEval)(const void *, double, int);

/*
 * Compares the spline with the reference at all samples, at interior points,
 * and close to the seam, for the value and the first two derivatives.
 */
static int
compare_with_reference(const tinyspline::BSpline &spline,
                       const std::vector<double> &parameters,
                       size_t dim,
                       const void *ctx,
                       RefEval ref_eval,
                       double tol)
{
	const size_t m = parameters.size() - 1;
	const double t0 = parameters[0];
	const double tm = parameters[m];
	std::vector<double> ts;
	for (size_t i = 0; i < m; ++i)
		ts.push_back(parameters[i]);
	for (size_t k = 1; k < 128; ++k)
		ts.push_back(t0 + (tm - t0) * (double) k / 128.0);
	ts.push_back(tm - 1e-9);
	for (size_t i = 0; i < ts.size(); ++i) {
		/* The library treats knots that are closer than
		 * TS_KNOT_EPSILON as equal and snaps evaluations to the
		 * knot. Skip points that fall strictly inside this
		 * tolerance window (but not exact knot positions, which are
		 * meaningful samples) so that the comparison is not
		 * affected by that documented behaviour. */
		bool skip = false;
		for (size_t k = 0; k <= m; ++k) {
			const double delta = std::fabs(ts[i] - parameters[k]);
			if (delta > 0.0 &&
			    delta < 1.5 * TS_KNOT_EPSILON)
				skip = true;
		}
		if (skip)
			continue;
		for (int order = 0; order <= 2; ++order) {
			const std::vector<double> got =
				eval_at(spline, ts[i], order);
			const std::vector<double> want =
				ref_eval(ctx, ts[i], order);
			if (got.size() != dim || want.size() != dim)
				return 1;
			const double diff = max_abs_diff(got, want);
			if (!(diff <= tol)) {
				std::fprintf(stderr,
					"reference mismatch t=%.17g order=%d"
					" diff=%g\n", ts[i], order, diff);
				return 1;
			}
		}
	}
	return 0;
}

/* The reduced objective J = sum w_i |C(t_i) - P_i|^2 + lambda E(C). */
static double
objective(const tinyspline::BSpline &spline,
          const std::vector<tinyspline::real> &points,
          const std::vector<tinyspline::real> &parameters,
          const std::vector<tinyspline::real> &weights,
          tinyspline::real lambda,
          size_t dim)
{
	const size_t m = parameters.size() - 1;
	double J = 0.0;
	for (size_t i = 0; i < m; ++i) {
		if (weights[i] == 0)
			continue;
		const std::vector<double> c = eval_at(spline, parameters[i]);
		for (size_t d = 0; d < dim; ++d) {
			const double diff = c[d] - points[i * dim + d];
			J += weights[i] * diff * diff;
		}
	}
	const double gx[3] = { -0.77459666924148337704, 0.0,
	                       0.77459666924148337704 };
	const double gw[3] = { 0.55555555555555555556,
	                       0.88888888888888888889,
	                       0.55555555555555555556 };
	tinyspline::BSpline d2 = spline.derive(2);
	for (size_t k = 0; k < m; ++k) {
		const double a0 = parameters[k];
		const double a1 = parameters[k + 1];
		const double mid = 0.5 * (a0 + a1);
		const double half = 0.5 * (a1 - a0);
		for (int g = 0; g < 3; ++g) {
			const std::vector<double> dd =
				eval_at(d2, mid + half * gx[g]);
			for (size_t d = 0; d < dim; ++d)
				J += lambda * half * gw[g] * dd[d] * dd[d];
		}
	}
	return J;
}

} /* anonymous namespace */

/* ------------------------------------------------------------------------ */
/* Main data set                                                            */
/* ------------------------------------------------------------------------ */

static FitCase
main_case()
{
	FitCase c;
	const double pts[] = {
		0.0, 0.0, 0.0,
		1.0, 0.4, -0.2,
		1.8, 1.3, 0.5,
		1.2, 2.6, 1.4,
		-0.3, 2.9, 2.2,
		-1.4, 1.7, 2.8,
		-0.8, 0.4, 2.1,
		0.2, -0.5, 1.2
	};
	const double par[] = {
		0.0, 0.43, 1.17, 1.86, 2.71, 3.52, 4.60, 5.81, 6.95
	};
	const double w[] = { 1.0, 2.0, 0.3, 1.0, 0.0, 1.5, 0.8, 1.2 };
	const int h[] = { 0, 0, 1, 0, 0, 0, 0, 1 };
	c.points.assign(pts, pts + sizeof(pts) / sizeof(pts[0]));
	c.parameters.assign(par, par + sizeof(par) / sizeof(par[0]));
	c.weights.assign(w, w + sizeof(w) / sizeof(w[0]));
	c.hard.assign(h, h + sizeof(h) / sizeof(h[0]));
	c.lambda = 0.7;
	c.dimension = 3;
	return c;
}

/* ------------------------------------------------------------------------ */
/* Group: periodic                                                          */
/* ------------------------------------------------------------------------ */

static int
periodic_shape_and_domain()
{
	const FitCase c = main_case();
	const tinyspline::BSpline spline =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);
	const size_t m = c.points.size() / 3;

	TASK_CHECK(spline.degree() == 3);
	TASK_CHECK(spline.dimension() == 3);
	TASK_CHECK(spline.numControlPoints() == m + 3);
	TASK_CHECK(spline.knots().size() == m + 7);
	TASK_CHECK(std::fabs(spline.domain().min() -
	                     c.parameters.front()) <= 1e-12);
	TASK_CHECK(std::fabs(spline.domain().max() -
	                     c.parameters.back()) <= 1e-12);

	/* Knots are the periodic extension of the parameters. */
	const std::vector<double> want = ref::periodic_knots(
		as_doubles(c.parameters));
	if (check_close(as_doubles(spline.knots()), want,
	                1e-12, "knots")) return 1;

	/* The last three control points copy the first three. */
	const std::vector<tinyspline::real> cp = spline.controlPoints();
	for (size_t j = 0; j < 3; ++j)
		for (size_t d = 0; d < 3; ++d)
			TASK_CHECK(cp[(m + j) * 3 + d] == cp[j * 3 + d]);
	return 0;
}

static int
periodic_reference_values()
{
	const FitCase c = main_case();
	const tinyspline::BSpline spline =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);
	const size_t m = c.points.size() / 3;

	/* Independent reference: nodal periodic cubic spline. */
	NodalCtx ctx;
	ctx.t = as_doubles(c.parameters);
	ctx.y = ref::fit_nodal(ctx.t, as_points(c.points, 3),
	                       as_doubles(c.weights), c.hard,
	                       c.lambda, 3);
	ctx.dim = 3;
	TASK_CHECK(compare_with_reference(spline, ctx.t, 3, &ctx,
	                                  &nodal_eval, TOL) == 0);

	/* Hard samples are satisfied exactly. */
	for (size_t i = 0; i < m; ++i) {
		if (!c.hard[i]) continue;
		const std::vector<double> got =
			eval_at(spline, c.parameters[i]);
		for (size_t d = 0; d < 3; ++d)
			TASK_CHECK(std::fabs(got[d] -
			           c.points[i * 3 + d]) <= 1e-9);
	}
	return 0;
}

static int
periodic_control_net_reference()
{
	const FitCase c = main_case();
	const tinyspline::BSpline spline =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);

	const std::vector<double> t = as_doubles(c.parameters);
	const std::vector<std::vector<double> > y =
		ref::fit_nodal(t, as_points(c.points, 3),
		               as_doubles(c.weights), c.hard,
		               c.lambda, 3);
	/* The reference nodal curve is exactly the fitted spline, so its
	 * collocation control net must match (independent basis). */
	const std::vector<double> want = ref::collocation_net(t, y, 3);
	const std::vector<double> got =
		as_doubles(spline.controlPoints());
	if (check_close(got, want, TOL, "control net")) return 1;

	/* Spot-check values of the published reference net. */
	const size_t at[] = { 0, 1, 2, 3, 9, 18, 24, 25, 26 };
	const double expect[] = {
		0.21701614389144008, -0.79452237509067902,
		1.2480114111299945, 0.84770412843708454,
		1.9716067414000862, -0.78407653075570605,
		0.21701614389144008, -0.79452237509067902,
		1.2480114111299945
	};
	for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); ++i)
		TASK_CHECK(std::fabs(got[at[i]] - expect[i]) <= TOL);

	/* Cross-check the truncated-power basis against the production
	 * implementation: the integral of |C''|^2 evaluated with the
	 * independent tp_basis_d2 must agree with the spline's own second
	 * derivative. */
	const double gx[3] = { -0.77459666924148337704, 0.0,
	                       0.77459666924148337704 };
	const double gw[3] = { 0.55555555555555555556,
	                       0.88888888888888888889,
	                       0.55555555555555555556 };
	const std::vector<double> U = ref::periodic_knots(t);
	const size_t m = t.size() - 1;
	double from_tp = 0.0;
	for (size_t k = 0; k < m; ++k) {
		const double a0 = t[k];
		const double a1 = t[k + 1];
		const double mid = 0.5 * (a0 + a1);
		const double half = 0.5 * (a1 - a0);
		for (int g = 0; g < 3; ++g) {
			const double u = mid + half * gx[g];
			double dd[3] = { 0.0, 0.0, 0.0 };
			/* On span [t_k, t_{k+1}] = [U_{k+3}, U_{k+4}]
			 * only the basis functions k..k+3 are active. */
			for (size_t j = k; j <= k + 3; ++j) {
				const double b = ref::tp_basis_d2(
					U, j, 3, u);
				for (size_t d = 0; d < 3; ++d)
					dd[d] += b * got[j * 3 + d];
			}
			for (size_t d = 0; d < 3; ++d)
				from_tp += half * gw[g] * dd[d] * dd[d];
		}
	}
	{
		const double gx2[3] = { -0.77459666924148337704, 0.0,
		                        0.77459666924148337704 };
		const double gw2[3] = { 0.55555555555555555556,
		                        0.88888888888888888889,
		                        0.55555555555555555556 };
		tinyspline::BSpline d2 = spline.derive(2);
		double from_prod = 0.0;
		for (size_t k = 0; k < m; ++k) {
			const double a0 = t[k];
			const double a1 = t[k + 1];
			const double mid = 0.5 * (a0 + a1);
			const double half = 0.5 * (a1 - a0);
			for (int g = 0; g < 3; ++g) {
				const std::vector<double> dd = eval_at(
					d2, mid + half * gx2[g]);
				for (size_t d = 0; d < 3; ++d)
					from_prod += half * gw2[g] *
						dd[d] * dd[d];
			}
		}
		if (check_close(std::vector<double>(1, from_tp),
		                std::vector<double>(1, from_prod),
		                TOL, "truncated power energy"))
			return 1;
	}
	return 0;
}

static int
periodic_seam_continuity()
{
	const FitCase c = main_case();
	const tinyspline::BSpline spline =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);
	const double t0 = c.parameters.front();
	const double tm = c.parameters.back();
	for (int order = 0; order <= 2; ++order) {
		const std::vector<double> a = eval_at(spline, t0, order);
		const std::vector<double> b =
			eval_at(spline, tm - 1e-9, order);
		const std::vector<double> z = eval_at(spline, tm, order);
		if (check_close(b, a, 1e-6, "seam continuity"))
			return 1;
		if (check_close(z, a, 1e-9, "seam endpoint"))
			return 1;
	}
	/* The derivative at the seam must not vanish (unlike natural). */
	const std::vector<double> d1 = eval_at(spline, t0, 1);
	double norm = 0.0;
	for (size_t d = 0; d < d1.size(); ++d)
		norm += d1[d] * d1[d];
	TASK_CHECK(std::sqrt(norm) > 1e-3);
	return 0;
}

static int
periodic_parametric_invariance()
{
	const FitCase c = main_case();
	const tinyspline::BSpline base =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);

	/* t -> a*t + b with a > 0 and lambda -> a^3 * lambda. */
	const double a = 2.3;
	const double b = -1.1;
	std::vector<tinyspline::real> par2(c.parameters.size());
	for (size_t i = 0; i < c.parameters.size(); ++i)
		par2[i] = (tinyspline::real)
			(a * c.parameters[i] + b);
	const tinyspline::BSpline scaled =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, par2,
			c.weights, c.hard,
			(tinyspline::real) (c.lambda * a * a * a));

	/* The control nets coincide. */
	if (check_close(as_doubles(scaled.controlPoints()),
	                as_doubles(base.controlPoints()),
	                TOL, "parametric control net"))
		return 1;

	/* The geometry at corresponding parameters coincides. */
	for (int k = 0; k <= 20; ++k) {
		const double u = c.parameters.front() +
			(c.parameters.back() - c.parameters.front()) *
			k / 20.0;
		const double v = a * u + b;
		for (int order = 0; order <= 2; ++order) {
			std::vector<double> p = eval_at(base, u, order);
			std::vector<double> q = eval_at(scaled, v, order);
			if (order == 1)
				for (size_t d = 0; d < q.size(); ++d)
					q[d] *= a;
			else if (order == 2)
				for (size_t d = 0; d < q.size(); ++d)
					q[d] *= a * a;
			if (check_close(p, q, 1e-6, "parametric geometry"))
				return 1;
		}
	}
	return 0;
}

static int
periodic_weight_scaling_invariance()
{
	const FitCase c = main_case();
	const tinyspline::BSpline base =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);

	/* Scaling all weights and lambda by the same positive factor
	 * leaves the solution unchanged. */
	const double s = 3.7;
	std::vector<tinyspline::real> w2(c.weights.size());
	for (size_t i = 0; i < c.weights.size(); ++i)
		w2[i] = (tinyspline::real) (c.weights[i] * s);
	const tinyspline::BSpline scaled =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			w2, c.hard, (tinyspline::real) (c.lambda * s));
	if (check_close(as_doubles(scaled.controlPoints()),
	                as_doubles(base.controlPoints()),
	                TOL, "weight scaling"))
		return 1;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Group: objective                                                         */
/* ------------------------------------------------------------------------ */

static int
objective_minimum_matches_reference()
{
	const FitCase c = main_case();
	const tinyspline::BSpline spline =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);

	const std::vector<double> t = as_doubles(c.parameters);
	const std::vector<double> w = as_doubles(c.weights);
	const std::vector<std::vector<double> > P = as_points(c.points, 3);
	const std::vector<std::vector<double> > y =
		ref::fit_nodal(t, P, w, c.hard, c.lambda, 3);

	/* J at the fitted spline computed from the spline itself. */
	const double J = objective(spline, c.points, c.parameters,
	                           c.weights, c.lambda, 3);
	/* J at the reference nodal solution (closed form). */
	double Jref = 0.0;
	for (size_t i = 0; i < w.size(); ++i) {
		if (w[i] == 0) continue;
		for (size_t d = 0; d < 3; ++d) {
			const double diff = y[d][i] - P[i][d];
			Jref += w[i] * diff * diff;
		}
	}
	Jref += c.lambda * ref::energy(t, y, 3);
	if (check_close(std::vector<double>(1, J),
	                std::vector<double>(1, Jref), 1e-6,
	                "objective"))
		return 1;

	/* Perturbing a free nodal value must increase J. */
	const double delta = 1e-3;
	for (size_t i = 0; i < w.size(); ++i) {
		if (c.hard[i]) continue;
		std::vector<std::vector<double> > yp = y;
		yp[0][i] += delta;
		double Jp = 0.0;
		for (size_t k = 0; k < w.size(); ++k) {
			if (w[k] == 0) continue;
			for (size_t kd = 0; kd < 3; ++kd) {
				const double diff = yp[kd][k] - P[k][kd];
				Jp += w[k] * diff * diff;
			}
		}
		Jp += c.lambda * ref::energy(t, yp, 3);
		TASK_CHECK(Jp > Jref - 1e-9);
		break;
	}
	return 0;
}

static int
objective_not_cyclic_second_difference()
{
	const FitCase c = main_case();
	const tinyspline::BSpline spline =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);
	const size_t m = c.points.size() / 3;
	const std::vector<double> t = as_doubles(c.parameters);

	std::vector<std::vector<double> > y(3, std::vector<double>(m));
	for (size_t i = 0; i < m; ++i) {
		const std::vector<double> v = eval_at(spline, t[i]);
		for (size_t d = 0; d < 3; ++d)
			y[d][i] = v[d];
	}
	const double want = ref::energy(t, y, 3);

	/* The cyclic second difference of control points gives a different
	 * value, so the check below cannot pass by accident. */
	const std::vector<double> cp = as_doubles(spline.controlPoints());
	double wrong = 0.0;
	for (size_t j = 0; j < m; ++j) {
		for (size_t d = 0; d < 3; ++d) {
			const double qm = cp[((j + m - 1) % m) * 3 + d];
			const double q0 = cp[j * 3 + d];
			const double qp = cp[((j + 1) % m) * 3 + d];
			wrong += (qp - 2.0 * q0 + qm) *
			         (qp - 2.0 * q0 + qm);
		}
	}
	TASK_CHECK(std::fabs(wrong - want) > 1e-2);

	/* Energy computed from the spline's second derivative. */
	const double gx[3] = { -0.77459666924148337704, 0.0,
	                       0.77459666924148337704 };
	const double gw[3] = { 0.55555555555555555556,
	                       0.88888888888888888889,
	                       0.55555555555555555556 };
	tinyspline::BSpline d2 = spline.derive(2);
	double got = 0.0;
	for (size_t k = 0; k < m; ++k) {
		const double a0 = t[k], a1 = t[k + 1];
		const double mid = 0.5 * (a0 + a1);
		const double half = 0.5 * (a1 - a0);
		for (int g = 0; g < 3; ++g) {
			const std::vector<double> dd =
				eval_at(d2, mid + half * gx[g]);
			for (size_t d = 0; d < 3; ++d)
				got += half * gw[g] * dd[d] * dd[d];
		}
	}
	if (check_close(std::vector<double>(1, got),
	                std::vector<double>(1, want), 1e-6,
	                "energy"))
		return 1;
	return 0;
}

static int
objective_weights_and_zero_weights()
{
	const FitCase c = main_case();
	const tinyspline::BSpline base =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);

	/* Moving the point of a zero-weight sample must not change the
	 * control net at all (sample 4 has weight 0). */
	std::vector<tinyspline::real> pts2 = c.points;
	for (size_t d = 0; d < 3; ++d)
		pts2[4 * 3 + d] += 5.0;
	const tinyspline::BSpline moved =
		tinyspline::BSpline::fitPeriodicCubic(
			pts2, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);
	if (check_close(as_doubles(moved.controlPoints()),
	                as_doubles(base.controlPoints()),
	                1e-9, "zero-weight invariance")) return 1;
	/* Its parameter is still a knot. */
	TASK_CHECK(moved.knots().size() ==
	           c.parameters.size() + 6);

	/* Changing a positive weight changes the solution. */
	std::vector<tinyspline::real> w2 = c.weights;
	w2[0] *= 2.0;
	const tinyspline::BSpline reweighted =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			w2, c.hard, c.lambda);
	TASK_CHECK(max_abs_diff(
		as_doubles(reweighted.controlPoints()),
		as_doubles(base.controlPoints())) > 1e-4);

	/* Hard samples remain exact even with a weight. */
	for (size_t i = 0; i < c.hard.size(); ++i) {
		if (!c.hard[i]) continue;
		const std::vector<double> v =
			eval_at(reweighted, c.parameters[i]);
		for (size_t d = 0; d < 3; ++d)
			TASK_CHECK(std::fabs(v[d] -
			           c.points[i * 3 + d]) <= 1e-9);
	}
	return 0;
}

static int
objective_all_hard_interpolates()
{
	const FitCase c = main_case();
	const tinyspline::BSpline spline =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights,
			std::vector<int>(c.hard.size(), 1),
			c.lambda);
	const size_t m = c.points.size() / 3;
	for (size_t i = 0; i < m; ++i) {
		const std::vector<double> v =
			eval_at(spline, c.parameters[i]);
		for (size_t d = 0; d < 3; ++d)
			TASK_CHECK(std::fabs(v[d] -
			           c.points[i * 3 + d]) <= 1e-9);
	}
	for (int order = 0; order <= 2; ++order) {
		const std::vector<double> a = eval_at(spline,
			c.parameters.front(), order);
		const std::vector<double> b = eval_at(spline,
			c.parameters.back() - 1e-9, order);
		if (check_close(a, b, 1e-6, "all-hard seam"))
			return 1;
	}
	return 0;
}

static int
objective_hard_overrides_weight()
{
	/* A hard sample with zero weight must still be satisfied exactly. */
	FitCase c = main_case();
	c.hard[3] = 1;
	std::vector<tinyspline::real> w = c.weights;
	w[3] = 0.0;
	const tinyspline::BSpline spline =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters, w,
			c.hard, c.lambda);
	const std::vector<double> v = eval_at(spline, c.parameters[3]);
	for (size_t d = 0; d < 3; ++d)
		TASK_CHECK(std::fabs(v[d] -
		           c.points[3 * 3 + d]) <= 1e-9);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Group: api                                                               */
/* ------------------------------------------------------------------------ */

static int
api_c_matches_cxx()
{
	const FitCase c = main_case();
	const tinyspline::BSpline cxx =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);

	tsBSpline raw = ts_bspline_init();
	tsStatus status;
	const tsError err = ts_bspline_fit_periodic_cubic(
		c.points.data(), c.points.size() / c.dimension,
		c.dimension, c.parameters.data(), c.weights.data(),
		c.hard.data(), c.lambda, &raw, &status);
	TASK_CHECK(err == TS_SUCCESS);
	TASK_CHECK(status.code == TS_SUCCESS);

	const tsReal *c_ctrlp = ts_bspline_control_points_ptr(&raw);
	const tsReal *c_knots = ts_bspline_knots_ptr(&raw);
	TASK_CHECK(raw.pImpl != NULL);
	TASK_CHECK(ts_bspline_degree(&raw) == cxx.degree());
	TASK_CHECK(ts_bspline_num_control_points(&raw) ==
	           cxx.numControlPoints());
	{
		const std::vector<tinyspline::real> cc(
			c_ctrlp, c_ctrlp +
			ts_bspline_len_control_points(&raw));
		const std::vector<tinyspline::real> ck(
			c_knots, c_knots + ts_bspline_num_knots(&raw));
		if (check_close(as_doubles(cc),
		                as_doubles(cxx.controlPoints()), 1e-12,
		                "C/C++ control points"))
			return 1;
		if (check_close(as_doubles(ck),
		                as_doubles(cxx.knots()), 1e-12,
		                "C/C++ knots"))
			return 1;
	}
	ts_bspline_free(&raw);
	return 0;
}

static int
api_inputs_are_not_modified()
{
	const FitCase c = main_case();
	std::vector<tinyspline::real> p = c.points;
	std::vector<tinyspline::real> t = c.parameters;
	std::vector<tinyspline::real> w = c.weights;
	std::vector<int> h = c.hard;
	(void) tinyspline::BSpline::fitPeriodicCubic(
		p, c.dimension, t, w, h, c.lambda);
	TASK_CHECK(p == c.points);
	TASK_CHECK(t == c.parameters);
	TASK_CHECK(w == c.weights);
	TASK_CHECK(h == c.hard);
	return 0;
}

static int
api_result_owns_data_and_copies()
{
	const FitCase c = main_case();
	tinyspline::BSpline spline =
		tinyspline::BSpline::fitPeriodicCubic(
			c.points, c.dimension, c.parameters,
			c.weights, c.hard, c.lambda);
	const std::vector<tinyspline::real> before =
		spline.controlPoints();

	/* The result is an ordinary spline: copy it, mutate the copy. */
	tinyspline::BSpline copy = spline;
	std::vector<tinyspline::real> cp = copy.controlPoints();
	for (size_t i = 0; i < cp.size(); ++i)
		cp[i] += 1.0;
	copy.setControlPoints(cp);
	if (check_close(as_doubles(spline.controlPoints()), before,
	                0.0, "copy independence"))
		return 1;

	/* Eval, derive, sample, and JSON serialization work. */
	TASK_CHECK(spline.eval((tinyspline::real) c.parameters[0])
		.result().size() == 3);
	TASK_CHECK(spline.derive().degree() == 2);
	TASK_CHECK(spline.sample(17).size() == 17 * 3);
	const std::string json = spline.toJson();
	const tinyspline::BSpline parsed =
		tinyspline::BSpline::parseJson(json);
	if (check_close(as_doubles(parsed.controlPoints()), before,
	                1e-12, "json roundtrip"))
		return 1;
	if (check_close(as_doubles(parsed.knots()),
	                as_doubles(spline.knots()), 1e-12,
	                "json knots"))
		return 1;
	return 0;
}

static int
api_2d_dataset()
{
	/* A 2D, non-uniform case with mixed hard/soft samples. */
	const double pts[] = {
		1.0, 0.0, 1.4, 1.1, 0.6, 1.7,
		-0.4, 1.5, -1.0, 0.6, -0.7, -0.4,
		0.1, -0.9
	};
	const double par[] = { 0.0, 0.5, 1.7, 2.2, 3.9, 4.4, 5.8, 6.4 };
	const double w[] = { 1.0, 1.0, 0.0, 2.0, 1.0, 1.0, 0.7 };
	const int h[] = { 1, 0, 0, 0, 1, 0, 0 };
	std::vector<tinyspline::real> points(
		pts, pts + sizeof(pts) / sizeof(pts[0]));
	std::vector<tinyspline::real> parameters(
		par, par + sizeof(par) / sizeof(par[0]));
	std::vector<tinyspline::real> weights(
		w, w + sizeof(w) / sizeof(w[0]));
	std::vector<int> hard(h, h + sizeof(h) / sizeof(h[0]));
	const tinyspline::BSpline spline =
		tinyspline::BSpline::fitPeriodicCubic(
			points, 2, parameters, weights, hard, 0.4);
	TASK_CHECK(spline.dimension() == 2);
	TASK_CHECK(spline.numControlPoints() == 10);
	TASK_CHECK(spline.knots().size() == 14);

	/* Compare with the independent reference in 2D. */
	NodalCtx ctx;
	ctx.t = as_doubles(parameters);
	ctx.y = ref::fit_nodal(ctx.t, as_points(points, 2),
	                       as_doubles(weights), hard, 0.4, 2);
	ctx.dim = 2;
	TASK_CHECK(compare_with_reference(spline, ctx.t, 2, &ctx,
	                                  &nodal_eval, TOL) == 0);

	for (size_t i = 0; i < hard.size(); ++i) {
		if (!hard[i]) continue;
		const std::vector<double> v =
			eval_at(spline, parameters[i]);
		TASK_CHECK(std::fabs(v[0] - points[i * 2]) <= 1e-9);
		TASK_CHECK(std::fabs(v[1] - points[i * 2 + 1]) <= 1e-9);
	}
	return 0;
}

static int
api_invalid_inputs()
{
	const FitCase c = main_case();
	const size_t m = c.points.size() / 3;
	tsStatus status;

	/* Too few points. */
	{
		tsBSpline s = ts_bspline_init();
		const tsError e = ts_bspline_fit_periodic_cubic(
			c.points.data(), 3, 3, c.parameters.data(),
			c.weights.data(), c.hard.data(), c.lambda,
			&s, &status);
		TASK_CHECK(e == TS_NUM_POINTS);
		TASK_CHECK(std::strlen(status.message) > 0);
		ts_bspline_free(&s);
	}
	/* Unsupported dimensions. */
	{
		const size_t dims[] = { 0, 1, 4 };
		for (size_t i = 0; i < 3; ++i) {
			tsBSpline s = ts_bspline_init();
			const tsError e = ts_bspline_fit_periodic_cubic(
				c.points.data(), m, dims[i],
				c.parameters.data(), c.weights.data(),
				c.hard.data(), c.lambda, &s, &status);
			TASK_CHECK(e == TS_DIM_ZERO);
			TASK_CHECK(std::strlen(status.message) > 0);
			ts_bspline_free(&s);
		}
	}
	/* Non-monotonic parameters. */
	{
		std::vector<tinyspline::real> p = c.parameters;
		std::swap(p[2], p[3]);
		tsBSpline s = ts_bspline_init();
		const tsError e = ts_bspline_fit_periodic_cubic(
			c.points.data(), m, 3, p.data(),
			c.weights.data(), c.hard.data(), c.lambda,
			&s, &status);
		TASK_CHECK(e == TS_KNOTS_DECR);
		TASK_CHECK(std::strlen(status.message) > 0);
		ts_bspline_free(&s);
	}
	/* Adjacent parameters too close. */
	{
		std::vector<tinyspline::real> p = c.parameters;
		p[2] = (tinyspline::real)
			(p[1] + 0.5 * TS_KNOT_EPSILON);
		tsBSpline s = ts_bspline_init();
		const tsError e = ts_bspline_fit_periodic_cubic(
			c.points.data(), m, 3, p.data(),
			c.weights.data(), c.hard.data(), c.lambda,
			&s, &status);
		TASK_CHECK(e == TS_KNOTS_DECR);
		TASK_CHECK(std::strlen(status.message) > 0);
		ts_bspline_free(&s);
	}
	/* NaN parameter. */
	{
		std::vector<tinyspline::real> p = c.parameters;
		p[3] = std::numeric_limits<tinyspline::real>::
			quiet_NaN();
		tsBSpline s = ts_bspline_init();
		const tsError e = ts_bspline_fit_periodic_cubic(
			c.points.data(), m, 3, p.data(),
			c.weights.data(), c.hard.data(), c.lambda,
			&s, &status);
		TASK_CHECK(e != TS_SUCCESS);
		TASK_CHECK(std::strlen(status.message) > 0);
		ts_bspline_free(&s);
	}
	/* Negative and non-finite weights. */
	for (int variant = 0; variant < 2; ++variant) {
		std::vector<tinyspline::real> w = c.weights;
		w[1] = variant == 0
			? (tinyspline::real) -1.0
			: std::numeric_limits<tinyspline::real>::
				infinity();
		tsBSpline s = ts_bspline_init();
		const tsError e = ts_bspline_fit_periodic_cubic(
			c.points.data(), m, 3, c.parameters.data(),
			w.data(), c.hard.data(), c.lambda,
			&s, &status);
		TASK_CHECK(e == TS_NO_RESULT);
		TASK_CHECK(std::strlen(status.message) > 0);
		ts_bspline_free(&s);
	}
	/* lambda <= 0 and NaN lambda. */
	for (int variant = 0; variant < 3; ++variant) {
		const double lam = variant == 0 ? 0.0
			: variant == 1 ? -1.0
			: std::numeric_limits<double>::quiet_NaN();
		tsBSpline s = ts_bspline_init();
		const tsError e = ts_bspline_fit_periodic_cubic(
			c.points.data(), m, 3, c.parameters.data(),
			c.weights.data(), c.hard.data(),
			(tinyspline::real) lam, &s, &status);
		TASK_CHECK(e == TS_NO_RESULT);
		TASK_CHECK(std::strlen(status.message) > 0);
		ts_bspline_free(&s);
	}
	/* Invalid hard flag. */
	{
		std::vector<int> h = c.hard;
		h[2] = 2;
		tsBSpline s = ts_bspline_init();
		const tsError e = ts_bspline_fit_periodic_cubic(
			c.points.data(), m, 3, c.parameters.data(),
			c.weights.data(), h.data(), c.lambda,
			&s, &status);
		TASK_CHECK(e == TS_NO_RESULT);
		TASK_CHECK(std::strlen(status.message) > 0);
		ts_bspline_free(&s);
	}
	/* No weighted and no hard sample. */
	{
		std::vector<tinyspline::real> w(m, 0.0);
		std::vector<int> h(m, 0);
		tsBSpline s = ts_bspline_init();
		const tsError e = ts_bspline_fit_periodic_cubic(
			c.points.data(), m, 3, c.parameters.data(),
			w.data(), h.data(), c.lambda, &s, &status);
		TASK_CHECK(e == TS_NO_RESULT);
		TASK_CHECK(std::strlen(status.message) > 0);
		ts_bspline_free(&s);
	}
	/* NULL input pointers. */
	{
		tsBSpline s = ts_bspline_init();
		const tsError e = ts_bspline_fit_periodic_cubic(
			NULL, m, 3, c.parameters.data(),
			c.weights.data(), c.hard.data(), c.lambda,
			&s, &status);
		TASK_CHECK(e != TS_SUCCESS);
		TASK_CHECK(std::strlen(status.message) > 0);
		ts_bspline_free(&s);
	}
	/* NULL status must be accepted on failure. */
	{
		tsBSpline s = ts_bspline_init();
		const tsError e = ts_bspline_fit_periodic_cubic(
			c.points.data(), 3, 3, c.parameters.data(),
			c.weights.data(), c.hard.data(), c.lambda,
			&s, NULL);
		TASK_CHECK(e == TS_NUM_POINTS);
		ts_bspline_free(&s);
	}
	/* After a failure, the output is empty and safe to free twice. */
	{
		tsBSpline s = ts_bspline_init();
		const tsError e = ts_bspline_fit_periodic_cubic(
			c.points.data(), 3, 3, c.parameters.data(),
			c.weights.data(), c.hard.data(), c.lambda,
			&s, &status);
		TASK_CHECK(e != TS_SUCCESS);
		TASK_CHECK(s.pImpl == NULL);
		ts_bspline_free(&s);
		ts_bspline_free(&s);
	}
	return 0;
}

static int
api_too_many_knots()
{
	/* m + 7 > TS_MAX_NUM_KNOTS must be rejected before allocating the
	 * quadratic fitting system. */
	const size_t m = (size_t) TS_MAX_NUM_KNOTS - 6;
	std::vector<tinyspline::real> points(m * 2, 0.0);
	std::vector<tinyspline::real> parameters(m + 1);
	std::vector<tinyspline::real> weights(m, 1.0);
	std::vector<int> hard(m, 0);
	for (size_t i = 0; i < m; ++i)
		points[i * 2] = (tinyspline::real) std::sin(0.01 * (double) i);
	for (size_t i = 0; i <= m; ++i)
		parameters[i] = (tinyspline::real) i;
	tsBSpline s = ts_bspline_init();
	tsStatus status;
	const tsError e = ts_bspline_fit_periodic_cubic(
		points.data(), m, 2, parameters.data(),
		weights.data(), hard.data(), 1.0, &s, &status);
	TASK_CHECK(e == TS_NUM_KNOTS);
	TASK_CHECK(std::strlen(status.message) > 0);
	ts_bspline_free(&s);

	/* A size that would overflow m + 7 must also be rejected cleanly
	 * (the function never dereferences the arrays in that case). */
	{
		const size_t huge = (size_t) -1;
		tsBSpline s2 = ts_bspline_init();
		tsStatus status2;
		const tsError e2 = ts_bspline_fit_periodic_cubic(
			points.data(), huge, 2, parameters.data(),
			weights.data(), hard.data(), 1.0, &s2,
			&status2);
		TASK_CHECK(e2 == TS_NUM_KNOTS);
		TASK_CHECK(std::strlen(status2.message) > 0);
		ts_bspline_free(&s2);
	}
	return 0;
}

static int
api_cxx_throws_on_bad_sizes()
{
	const FitCase c = main_case();
	bool threw;

	/* #points not divisible by dimension. */
	threw = false;
	try {
		std::vector<tinyspline::real> p = c.points;
		p.pop_back();
		(void) tinyspline::BSpline::fitPeriodicCubic(
			p, 3, c.parameters, c.weights, c.hard,
			c.lambda);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	TASK_CHECK(threw);

	/* Wrong number of parameters. */
	threw = false;
	try {
		std::vector<tinyspline::real> par = c.parameters;
		par.pop_back();
		(void) tinyspline::BSpline::fitPeriodicCubic(
			c.points, 3, par, c.weights, c.hard,
			c.lambda);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	TASK_CHECK(threw);

	/* Wrong number of weights. */
	threw = false;
	try {
		std::vector<tinyspline::real> w = c.weights;
		w.pop_back();
		(void) tinyspline::BSpline::fitPeriodicCubic(
			c.points, 3, c.parameters, w, c.hard,
			c.lambda);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	TASK_CHECK(threw);

	/* Wrong number of hard flags. */
	threw = false;
	try {
		std::vector<int> h = c.hard;
		h.pop_back();
		(void) tinyspline::BSpline::fitPeriodicCubic(
			c.points, 3, c.parameters, c.weights, h,
			c.lambda);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	TASK_CHECK(threw);

	/* Invalid data is reported through std::runtime_error too. */
	threw = false;
	try {
		std::vector<tinyspline::real> w = c.weights;
		w[1] = (tinyspline::real) -1.0;
		(void) tinyspline::BSpline::fitPeriodicCubic(
			c.points, 3, c.parameters, w, c.hard,
			c.lambda);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	TASK_CHECK(threw);
	return 0;
}

static int
api_existing_interpolation_unchanged()
{
	/* The old natural interpolation and Catmull-Rom remain available and
	 * are not periodic (first derivative gap at the seam). */
	const std::vector<tinyspline::real> pts = {
		0, 0, 2, 0, 2, 1, -1, 2, 0, 0
	};
	const tinyspline::BSpline nat =
		tinyspline::BSpline::interpolateCubicNatural(pts, 2);
	TASK_CHECK(nat.numControlPoints() == 16);
	const std::vector<double> a = eval_at(nat, 0.0);
	const std::vector<double> b = eval_at(nat, 1.0);
	if (check_close(a, b, 1e-8, "natural position"))
		return 1;
	const std::vector<double> da = eval_at(nat, 0.0, 1);
	const std::vector<double> db = eval_at(nat, 1.0, 1);
	double gap = 0.0;
	for (size_t d = 0; d < 2; ++d)
		gap += (da[d] - db[d]) * (da[d] - db[d]);
	TASK_CHECK(std::sqrt(gap) > 1.0);

	const tinyspline::BSpline cr =
		tinyspline::BSpline::interpolateCatmullRom(pts, 2);
	TASK_CHECK(cr.degree() == 3);
	TASK_CHECK(cr.numControlPoints() > 0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Registration                                                             */
/* ------------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	{ "periodic", "shape_and_domain", periodic_shape_and_domain },
	{ "periodic", "reference_values", periodic_reference_values },
	{ "periodic", "control_net_reference",
	  periodic_control_net_reference },
	{ "periodic", "seam_continuity", periodic_seam_continuity },
	{ "periodic", "parametric_invariance",
	  periodic_parametric_invariance },
	{ "periodic", "weight_scaling_invariance",
	  periodic_weight_scaling_invariance },
	{ "objective", "minimum_matches_reference",
	  objective_minimum_matches_reference },
	{ "objective", "not_cyclic_second_difference",
	  objective_not_cyclic_second_difference },
	{ "objective", "weights_and_zero_weights",
	  objective_weights_and_zero_weights },
	{ "objective", "all_hard_interpolates",
	  objective_all_hard_interpolates },
	{ "objective", "hard_overrides_weight",
	  objective_hard_overrides_weight },
	{ "api", "c_matches_cxx", api_c_matches_cxx },
	{ "api", "inputs_are_not_modified", api_inputs_are_not_modified },
	{ "api", "result_owns_data_and_copies",
	  api_result_owns_data_and_copies },
	{ "api", "2d_dataset", api_2d_dataset },
	{ "api", "invalid_inputs", api_invalid_inputs },
	{ "api", "too_many_knots", api_too_many_knots },
	{ "api", "cxx_throws_on_bad_sizes", api_cxx_throws_on_bad_sizes },
	{ "api", "existing_interpolation_unchanged",
	  api_existing_interpolation_unchanged },
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
