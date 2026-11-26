#include "registry.hpp"

// The task tests intentionally compile the amalgamated implementation once,
// so the internal OpenNL assembly and Mesh helpers are directly reachable.
#include "../source/xatlas/xatlas.cpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

using namespace xatlas::internal;

namespace {

struct DenseMetrics {
	double eta = 0.0;
	double rho = 0.0;
	double error = 0.0;
};

static double denseNorm(const std::vector<double> &v)
{
	double scale = 0.0;
	double ss = 1.0;
	for (double value : v) {
		const double ax = std::fabs(value);
		if (ax == 0.0)
			continue;
		if (scale < ax) {
			const double r = scale / ax;
			ss = 1.0 + ss * r * r;
			scale = ax;
		} else {
			const double r = ax / scale;
			ss += r * r;
		}
	}
	return scale == 0.0 ? 0.0 : scale * std::sqrt(ss);
}

static double denseFrobenius(const std::vector<std::vector<double> > &a)
{
	double scale = 0.0;
	double ss = 1.0;
	for (const auto &row : a) {
		for (double value : row) {
			const double ax = std::fabs(value);
			if (ax == 0.0)
				continue;
			if (scale < ax) {
				const double r = scale / ax;
				ss = 1.0 + ss * r * r;
				scale = ax;
			} else {
				const double r = ax / scale;
				ss += r * r;
			}
		}
	}
	return scale == 0.0 ? 0.0 : scale * std::sqrt(ss);
}

static DenseMetrics denseStoppingMetrics(
	const std::vector<std::vector<double> > &a,
	const std::vector<double> &b,
	const std::vector<double> &x)
{
	const size_t m = a.size();
	const size_t n = x.size();
	std::vector<double> r(m, 0.0), atr(n, 0.0);
	for (size_t i = 0; i < m; ++i) {
		for (size_t j = 0; j < n; ++j)
			r[i] += a[i][j] * x[j];
		r[i] -= b[i];
	}
	for (size_t j = 0; j < n; ++j) {
		for (size_t i = 0; i < m; ++i)
			atr[j] += a[i][j] * r[i];
	}
	const double rn = denseNorm(r);
	const double atrn = denseNorm(atr);
	const double xn = denseNorm(x);
	const double bn = denseNorm(b);
	const double af = denseFrobenius(a);
	DenseMetrics out;
	const double etaDen = af * rn;
	if (etaDen == 0.0 && atrn == 0.0)
		out.eta = 0.0;
	else
		out.eta = atrn / etaDen;
	const double rhoDen = af * xn + bn;
	if (rhoDen == 0.0 && rn == 0.0)
		out.rho = 0.0;
	else
		out.rho = rn / rhoDen;
	out.error = std::min(out.eta, out.rho);
	return out;
}

// Independent dense Householder QR least-squares solve for full-column-rank
// systems.  It is deliberately not based on normal equations.
static std::vector<double> householderLeastSquares(
	std::vector<std::vector<double> > a,
	std::vector<double> y)
{
	const int m = (int)a.size();
	const int n = m ? (int)a[0].size() : 0;
	for (int k = 0; k < std::min(m, n); ++k) {
		double nx = 0.0;
		for (int i = k; i < m; ++i)
			nx += a[i][k] * a[i][k];
		nx = std::sqrt(nx);
		if (nx == 0.0)
			continue;
		const double alpha = a[k][k] >= 0.0 ? -nx : nx;
		std::vector<double> v(m - k, 0.0);
		v[0] = a[k][k] - alpha;
		for (int i = 1; i < m - k; ++i)
			v[i] = a[k + i][k];
		const double vn = denseNorm(v);
		for (double &value : v)
			value /= vn;
		for (int j = k; j < n; ++j) {
			double dot = 0.0;
			for (int i = 0; i < m - k; ++i)
				dot += v[i] * a[k + i][j];
			for (int i = 0; i < m - k; ++i)
				a[k + i][j] -= 2.0 * v[i] * dot;
		}
		double dot = 0.0;
		for (int i = 0; i < m - k; ++i)
			dot += v[i] * y[k + i];
		for (int i = 0; i < m - k; ++i)
			y[k + i] -= 2.0 * v[i] * dot;
	}
	std::vector<double> x(n, 0.0);
	for (int i = n - 1; i >= 0; --i) {
		double rhs = y[i];
		for (int j = i + 1; j < n; ++j)
			rhs -= a[i][j] * x[j];
		x[i] = rhs / a[i][i];
	}
	return x;
}

struct SystemSpec {
	int variables = 0;
	std::vector<char> locked;
	std::vector<double> initial;
	std::vector<std::vector<std::pair<int, double> > > rows;
	uint32_t maxIterations = 100;
	double threshold = 1e-12;
	bool badCoefficient = false;
	bool badLockedValue = false;
	bool overrideThreshold = false;
	double thresholdOverride = 0.0;
};

struct AssembledSystem {
	opennl::NLContext *context = nullptr;
	std::vector<int> freeToGlobal;
	std::vector<std::vector<double> > a;
	std::vector<double> b;
};

static AssembledSystem assembleSystem(const SystemSpec &spec)
{
	AssembledSystem out;
	out.context = opennl::nlNewContext();
	out.context->use_lsqr = true;
	opennl::nlSolverParameteri(out.context, NL_NB_VARIABLES, spec.variables);
	if (spec.maxIterations)
		opennl::nlSolverParameteri(out.context, NL_MAX_ITERATIONS, (int)spec.maxIterations);
	else {
		out.context->max_iterations = 0;
		out.context->max_iterations_defined = true;
	}
	out.context->threshold = spec.overrideThreshold ? spec.thresholdOverride : spec.threshold;

	opennl::nlBegin(out.context, NL_SYSTEM);
	for (int i = 0; i < spec.variables; ++i) {
		double value = i < (int)spec.initial.size() ? spec.initial[i] : 0.0;
		if (spec.badLockedValue && spec.locked[i])
			value = std::numeric_limits<double>::quiet_NaN();
		opennl::nlSetVariable(out.context, (uint32_t)i, value);
		if (i < (int)spec.locked.size() && spec.locked[i])
			opennl::nlLockVariable(out.context, (uint32_t)i);
	}
	opennl::nlBegin(out.context, NL_MATRIX);
	for (const auto &row : spec.rows) {
		opennl::nlBegin(out.context, NL_ROW);
		for (const auto &term : row) {
			double value = term.second;
			if (spec.badCoefficient)
				value = std::numeric_limits<double>::infinity();
			opennl::nlCoefficient(out.context, (uint32_t)term.first, value);
		}
		opennl::nlEnd(out.context, NL_ROW);
	}
	opennl::nlEnd(out.context, NL_MATRIX);
	opennl::nlEnd(out.context, NL_SYSTEM);

	for (int i = 0; i < spec.variables; ++i) {
		if (!(i < (int)spec.locked.size() && spec.locked[i]))
			out.freeToGlobal.push_back(i);
	}
	const int n = (int)out.freeToGlobal.size();
	out.a.assign(spec.rows.size(), std::vector<double>(n, 0.0));
	out.b.assign(spec.rows.size(), 0.0);
	for (size_t r = 0; r < spec.rows.size(); ++r) {
		std::vector<double> all(spec.variables, 0.0);
		for (const auto &term : spec.rows[r])
			all[term.first] += term.second;
		double lockedSum = 0.0;
		for (int j = 0; j < spec.variables; ++j) {
			if (j < (int)spec.locked.size() && spec.locked[j])
				lockedSum += all[j] * (spec.badLockedValue
					? std::numeric_limits<double>::quiet_NaN()
					: (j < (int)spec.initial.size() ? spec.initial[j] : 0.0));
			else {
				int freeIndex = -1;
				for (int k = 0; k < n; ++k)
					if (out.freeToGlobal[k] == j)
						freeIndex = k;
				if (freeIndex >= 0)
					out.a[r][freeIndex] += all[j];
			}
		}
		out.b[r] = -lockedSum;
	}
	return out;
}

struct SolvedSystem {
	bool solved = false;
	opennl::NLLeastSquaresStatus status = opennl::InvalidInput;
	uint32_t iterations = 0;
	double error = 0.0;
	std::vector<double> values;
	DenseMetrics metrics;
};

static SolvedSystem runSystem(const SystemSpec &spec)
{
	AssembledSystem assembled = assembleSystem(spec);
	opennl::NLContext *c = assembled.context;
	SolvedSystem result;
	result.solved = opennl::nlSolve(c);
	result.status = c->lsqr_status;
	result.iterations = c->used_iterations;
	result.error = c->error;
	result.values.resize(spec.variables);
	for (int i = 0; i < spec.variables; ++i)
		result.values[i] = opennl::nlGetVariable(c, (uint32_t)i);
	std::vector<double> x(assembled.freeToGlobal.size());
	for (size_t i = 0; i < x.size(); ++i)
		x[i] = result.values[assembled.freeToGlobal[i]];
	result.metrics = denseStoppingMetrics(assembled.a, assembled.b, x);
	opennl::nlDeleteContext(c);
	return result;
}

static void checkNear(Case &cs, double value, double expected, double tolerance, const char *message)
{
	const double scale = std::max(1.0, std::fabs(expected));
	cs.check(std::isfinite(value) && std::fabs(value - expected) <= tolerance * scale, message);
}

static SystemSpec baselineSpec()
{
	SystemSpec s;
	s.variables = 3;
	s.locked = {0, 0, 1};
	s.initial = {0.0, 0.0, 1.0};
	s.rows = {
		{{0, 1.0}, {1, 1.0}, {2, -3.0}},
		{{0, 1.0}, {1, -1.0}, {2, 1.0}}
	};
	return s;
}

static int rectangular_reference(Case &cs)
{
	cs.check(!xatlas::ChartOptions().useLsqr, "ChartOptions::useLsqr must default to false");
	xatlasChartOptions co;
	xatlasChartOptionsInit(&co);
	cs.check(!co.useLsqr, "xatlasChartOptionsInit must default useLsqr to false");
	static_assert(offsetof(xatlas::ChartOptions, useLsqr) == offsetof(xatlasChartOptions, useLsqr),
		"C and C++ useLsqr positions must match");

	const SolvedSystem baseline = runSystem(baselineSpec());
	cs.check(baseline.solved && baseline.status == opennl::Converged, "locked two-variable rectangular system must converge");
	checkNear(cs, baseline.values[0], 1.0, 1e-9, "locked system x0");
	checkNear(cs, baseline.values[1], 2.0, 1e-9, "locked system x1");
	checkNear(cs, baseline.values[2], 1.0, 0.0, "locked value must be exact");
	cs.check(baseline.metrics.eta <= 1e-10 || baseline.metrics.rho <= 1e-10, "baseline independent stopping metric");
	checkNear(cs, baseline.error, baseline.metrics.error, 1e-9, "context error must be min(eta,rho)");

	SystemSpec inconsistent;
	inconsistent.variables = 3;
	inconsistent.locked = {0, 0, 1};
	inconsistent.initial = {0, 0, 1};
	inconsistent.rows = {
		{{0, 1}, {2, -1}},
		{{1, 1}, {2, -2}},
		{{0, 1}, {1, 1}, {2, -4}}
	};
	const SolvedSystem inc = runSystem(inconsistent);
	cs.check(inc.solved && inc.status == opennl::Converged, "incompatible least-squares system must converge at nonzero residual");
	checkNear(cs, inc.values[0], 4.0 / 3.0, 1e-8, "incompatible LS x0");
	checkNear(cs, inc.values[1], 7.0 / 3.0, 1e-8, "incompatible LS x1");
	cs.check(inc.metrics.rho > 1e-8, "incompatible system should retain a nonzero backward-error residual");
	cs.check(inc.metrics.eta <= 1e-10, "incompatible system must satisfy stationarity");

	SystemSpec qrSpec;
	qrSpec.variables = 4;
	qrSpec.locked = {0, 0, 0, 1};
	qrSpec.initial = {0, 0, 0, 1};
	const std::vector<std::vector<double> > qrA = {
		{1, 2, 0}, {0, 1, 1}, {2, 0, 1}, {1, 1, 1}, {1, -1, 2}
	};
	const std::vector<double> qrB = {3, 5, 6, 7, 4};
	for (size_t i = 0; i < qrA.size(); ++i) {
		std::vector<std::pair<int, double> > row;
		for (int j = 0; j < 3; ++j)
			row.push_back({j, qrA[i][j]});
		row.push_back({3, -qrB[i]});
		qrSpec.rows.push_back(row);
	}
	const std::vector<double> qrX = householderLeastSquares(qrA, qrB);
	const SolvedSystem qrSolved = runSystem(qrSpec);
	cs.check(qrSolved.solved && qrSolved.status == opennl::Converged, "Householder cross-check system must converge");
	for (int j = 0; j < 3; ++j)
		checkNear(cs, qrSolved.values[j], qrX[j], 1e-8, "LSQR must agree with independent Householder QR");
	return 0;
}

static int rectangular_duplicates_locks(Case &cs)
{
	SystemSpec duplicate = baselineSpec();
	duplicate.rows = {
		{{0, 0.5}, {0, 0.5}, {1, 2.0}, {1, -1.0}, {2, -1.0}, {2, -2.0}},
		{{0, 1.0}, {1, -1.0}, {2, 1.0}}
	};
	AssembledSystem assembled = assembleSystem(duplicate);
	cs.check(assembled.context->lsqr_m == 2, "two raw rows must be retained");
	cs.check(assembled.context->lsqr_rows[0].size == 2, "duplicate free coefficients must be merged");
	opennl::nlDeleteContext(assembled.context);
	const SolvedSystem dup = runSystem(duplicate);
	cs.check(dup.solved, "duplicate-coefficient system must solve");
	checkNear(cs, dup.values[0], 1.0, 1e-9, "duplicate coefficients x0");
	checkNear(cs, dup.values[1], 2.0, 1e-9, "duplicate coefficients x1");

	SystemSpec locks;
	locks.variables = 4;
	locks.locked = {0, 0, 1, 1};
	locks.initial = {0, 0, 1.0, -1.0};
	locks.rows = {
		{{0, 1}, {1, 1}, {2, -3}},
		{{0, 1}, {1, -1}, {3, -1}}
	};
	const SolvedSystem multi = runSystem(locks);
	cs.check(multi.solved && multi.status == opennl::Converged, "multiple-lock system must converge");
	checkNear(cs, multi.values[0], 1.0, 1e-9, "multiple locks x0");
	checkNear(cs, multi.values[1], 2.0, 1e-9, "multiple locks x1");
	checkNear(cs, multi.values[2], 1.0, 0.0, "first lock preserved");
	checkNear(cs, multi.values[3], -1.0, 0.0, "second lock preserved");
	return 0;
}

static int rectangular_degenerate_scales(Case &cs)
{
	SystemSpec rank;
	rank.variables = 3;
	rank.locked = {0, 0, 1};
	rank.initial = {0, 0, 1};
	rank.rows = {
		{{0, 1}, {1, 1}, {2, -3}},
		{{0, 2}, {1, 2}, {2, -6}}
	};
	SolvedSystem solvedRank = runSystem(rank);
	cs.check(solvedRank.solved && solvedRank.status == opennl::Converged, "consistent rank-deficient system must converge to a stationary point");
	checkNear(cs, solvedRank.values[0] + solvedRank.values[1], 3.0, 1e-8, "rank-deficient consistent equation");
	cs.check(solvedRank.metrics.eta <= 1e-10 || solvedRank.metrics.rho <= 1e-10, "rank-deficient stationarity");

	SystemSpec inconsistentRank;
	inconsistentRank.variables = 3;
	inconsistentRank.locked = {0, 0, 1};
	inconsistentRank.initial = {0, 0, 1};
	inconsistentRank.rows = {
		{{0, 1}, {1, 1}, {2, -3}},
		{{0, 1}, {1, 1}, {2, -7}}
	};
	SolvedSystem badRank = runSystem(inconsistentRank);
	cs.check(badRank.solved && badRank.status == opennl::Converged, "inconsistent rank-deficient system may finish at a stationary point");
	checkNear(cs, badRank.values[0] + badRank.values[1], 5.0, 1e-8, "rank-deficient least-squares stationary sum");
	checkNear(cs, denseNorm(std::vector<double>{-2.0, 2.0}), std::sqrt(8.0), 0.0, "test residual expectation sanity");
	cs.check(badRank.metrics.eta <= 1e-10, "rank-deficient incompatible stationarity");

	SystemSpec zeroColumn;
	zeroColumn.variables = 3;
	zeroColumn.locked = {0, 0, 1};
	zeroColumn.initial = {0, 0, 1};
	zeroColumn.rows = {
		{{0, 2}, {2, -2}},
		{{0, 1}, {2, -1}}
	};
	SolvedSystem zeroCol = runSystem(zeroColumn);
	cs.check(zeroCol.solved && zeroCol.status == opennl::Converged, "zero-column system must converge");
	checkNear(cs, zeroCol.values[0], 1.0, 1e-9, "nonzero column in zero-column system");
	cs.check(std::isfinite(zeroCol.values[1]), "zero column may remain any finite stationary value");
	cs.check(zeroCol.metrics.eta <= 1e-10 || zeroCol.metrics.rho <= 1e-10, "zero-column metric");

	SystemSpec allLocked;
	allLocked.variables = 2;
	allLocked.locked = {1, 1};
	allLocked.initial = {2.0, 1.0};
	allLocked.rows = {
		{{0, 1}, {1, -2}},
		{{0, 3}, {1, -6}}
	};
	SolvedSystem locked = runSystem(allLocked);
	cs.check(locked.solved && locked.status == opennl::Converged, "all-locked consistent system is stationary");
	checkNear(cs, locked.values[0], 2.0, 0.0, "all-locked value 0");
	checkNear(cs, locked.values[1], 1.0, 0.0, "all-locked value 1");
	cs.check(locked.iterations == 0, "all-locked system can converge before bidiagonal updates");

	SystemSpec lockedOnlyResidual = allLocked;
	lockedOnlyResidual.rows = {{{0, 1}, {1, 1}}}; // 2 + 1 != 0
	SolvedSystem lockedResidual = runSystem(lockedOnlyResidual);
	cs.check(lockedResidual.solved, "all-locked zero-column stationary point is allowed even with residual");
	cs.check(lockedResidual.metrics.eta == 0.0, "zero-column projected gradient metric is zero by convention");
	cs.check(lockedResidual.metrics.rho > 0.9, "locked-only row must contribute its real residual");

	SystemSpec scale;
	scale.variables = 3;
	scale.locked = {0, 0, 1};
	scale.initial = {0, 0, 1};
	scale.threshold = 1e-14;
	scale.rows = {
		{{0, 1e8}, {2, -2e8}},
		{{1, 1e-4}, {2, -3e-4}}
	};
	SolvedSystem scaled = runSystem(scale);
	cs.check(scaled.solved && scaled.status == opennl::Converged, "extreme column-scale system must converge");
	checkNear(cs, scaled.values[0], 2.0, 1e-7, "large column target");
	checkNear(cs, scaled.values[1], 3.0, 1e-6, "small column target");
	return 0;
}

static int stopping_iteration_limit(Case &cs)
{
	SystemSpec s;
	s.variables = 4;
	s.locked = {0, 0, 0, 1};
	s.initial = {0, 0, 0, 1};
	const std::vector<std::vector<double> > a = {
		{1, 2, 0}, {0, 1, 1}, {2, 0, 1}, {1, 1, 1}, {1, -1, 2}
	};
	const std::vector<double> b = {3, 5, 6, 7, 4};
	for (size_t i = 0; i < a.size(); ++i) {
		std::vector<std::pair<int, double> > row;
		for (int j = 0; j < 3; ++j)
			row.push_back({j, a[i][j]});
		row.push_back({3, -b[i]});
		s.rows.push_back(row);
	}
	s.maxIterations = 1;
	s.threshold = 1e-14;
	SolvedSystem one = runSystem(s);
	cs.check(!one.solved, "one iteration must not be reported as success for a strict tolerance");
	cs.check(one.status == opennl::IterationLimit, "exhausted budget must be IterationLimit");
	cs.check(one.iterations == 1, "used_iterations must be the actual bidiagonal step count");
	cs.check(one.metrics.eta > s.threshold && one.metrics.rho > s.threshold, "forced-short solve must independently fail both metrics");
	checkNear(cs, one.error, one.metrics.error, 1e-9, "iteration-limit error must be recomputed min(eta,rho)");

	s.maxIterations = 0;
	SolvedSystem zero = runSystem(s);
	cs.check(!zero.solved && zero.status == opennl::IterationLimit, "zero budget on an unconverged initial point is IterationLimit");
	cs.check(zero.iterations == 0, "zero iterations reported");
	return 0;
}

static int stopping_invalid_input(Case &cs)
{
	SystemSpec good = baselineSpec();
	good.overrideThreshold = true;
	good.thresholdOverride = 0.0;
	SolvedSystem zeroTol = runSystem(good);
	cs.check(!zeroTol.solved && zeroTol.status == opennl::InvalidInput, "zero tolerance is InvalidInput");

	good.thresholdOverride = -1e-12;
	SolvedSystem negativeTol = runSystem(good);
	cs.check(!negativeTol.solved && negativeTol.status == opennl::InvalidInput, "negative tolerance is InvalidInput");

	good.thresholdOverride = std::numeric_limits<double>::quiet_NaN();
	SolvedSystem nanTol = runSystem(good);
	cs.check(!nanTol.solved && nanTol.status == opennl::InvalidInput, "non-finite tolerance is InvalidInput");

	SystemSpec badA = baselineSpec();
	badA.badCoefficient = true;
	SolvedSystem badCoefficient = runSystem(badA);
	cs.check(!badCoefficient.solved && badCoefficient.status == opennl::InvalidInput, "non-finite A is InvalidInput");

	SystemSpec badLock = baselineSpec();
	badLock.badLockedValue = true;
	SolvedSystem badLocked = runSystem(badLock);
	cs.check(!badLocked.solved && badLocked.status == opennl::InvalidInput, "non-finite locked value/b is InvalidInput");

	opennl::NLContext *fresh = opennl::nlNewContext();
	cs.check(!fresh->use_lsqr, "nlNewContext must leave use_lsqr false");
	opennl::nlDeleteContext(fresh);
	return 0;
}

static int stopping_breakdown(Case &cs)
{
	// One free column [-2,-3], b=[0,-3].  The second alpha is exactly zero
	// in ordinary double arithmetic, while an absurd tolerance prevents the
	// rounded stationarity metric from accepting the natural termination.
	SystemSpec s;
	s.variables = 2;
	s.locked = {0, 1};
	s.initial = {0.0, 1.0};
	s.rows = {
		{{0, -2.0}, {1, 0.0}},
		{{0, -3.0}, {1, 3.0}} // -3*x + 3*1 = 0 => -3x = -3? A=-3,b=-3 yes
	};
	s.maxIterations = 10;
	s.threshold = 1e-18;
	SolvedSystem broken = runSystem(s);
	cs.check(!broken.solved, "stagnated solve below requested precision must fail");
	cs.check(broken.status == opennl::Breakdown, "zero bidiagonal scalar without stationarity is Breakdown");
	cs.check(broken.iterations >= 1, "breakdown reports actual bidiagonal steps");
	cs.check(broken.metrics.eta > s.threshold || broken.metrics.rho > s.threshold, "breakdown must not satisfy requested metrics");

	// The same natural termination at a normal tolerance is convergence.
	s.threshold = 1e-10;
	SolvedSystem converged = runSystem(s);
	cs.check(converged.solved && converged.status == opennl::Converged, "natural bidiagonal termination must first be tested by true metrics");
	checkNear(cs, converged.values[0], 9.0 / 13.0, 1e-9, "breakdown-shaped system LS solution");
	return 0;
}

// ---------- Mesh / chart helpers ----------

static void buildCurvedStrip(Mesh &m, uint32_t segments = 12)
{
	const uint32_t columns = segments + 1;
	for (uint32_t i = 0; i <= segments; ++i) {
		const float x = float(i);
		const float width = 0.75f + 0.55f * float(i) / float(segments);
		const float z = 0.22f * x * x;
		const uint32_t bottom = i;
		const uint32_t top = columns + i;
		m.addVertex(Vector3(x, 0.0f, z), Vector3(0), Vector2(0.31f * x + 0.2f * bottom, -0.47f * x + 1.7f));
		m.addVertex(Vector3(x, width, z + 0.05f * width), Vector3(0), Vector2(0.19f * x + 0.4f * top, 0.53f * x - 0.8f));
	}
	for (uint32_t i = 0; i < segments; ++i) {
		const uint32_t b0 = i, b1 = i + 1;
		const uint32_t t0 = columns + i, t1 = columns + i + 1;
		uint32_t tri[3];
		tri[0] = b0; tri[1] = b1; tri[2] = t1; m.addFace(tri);
		tri[0] = b0; tri[1] = t1; tri[2] = t0; m.addFace(tri);
	}
	m.createColocals();
	m.createBoundaries();
}

static void buildBumpyPatch(Mesh &m)
{
	const int n = 4;
	const float h = 1.0f;
	for (int y = 0; y <= n; ++y) {
		for (int x = 0; x <= n; ++x) {
			const float fx = (float(x) - n * 0.5f) * h;
			const float fy = (float(y) - n * 0.5f) * h;
			const float r2 = fx * fx + fy * fy;
			const float z = 0.28f * std::exp(-r2 * 0.35f) + 0.06f * fx;
			const uint32_t id = y * (n + 1) + x;
			m.addVertex(Vector3(fx, fy, z), Vector3(0), Vector2(0.13f * id - 1.2f, 0.29f * id + 0.4f));
		}
	}
	for (int y = 0; y < n; ++y) {
		for (int x = 0; x < n; ++x) {
			const uint32_t a = y * (n + 1) + x;
			const uint32_t b = a + 1;
			const uint32_t c = a + (n + 1);
			const uint32_t d = c + 1;
			uint32_t tri[3];
			tri[0] = a; tri[1] = b; tri[2] = d; m.addFace(tri);
			tri[0] = a; tri[1] = d; tri[2] = c; m.addFace(tri);
		}
	}
	m.createColocals();
	m.createBoundaries();
}

struct RelationEnergy {
	double squared = 0.0;
	double normalized = 0.0;
	uint32_t rows = 0;
};

// Evaluate the exact OpenNL conformal relation rows at the mesh UVs without
// solving or constructing A^T A.
static RelationEnergy conformalRelationEnergy(const Mesh &m)
{
	const uint32_t vertexCount = m.vertexCount();
	opennl::NLContext *c = opennl::nlNewContext();
	c->use_lsqr = true;
	opennl::nlSolverParameteri(c, NL_NB_VARIABLES, (int)(2 * vertexCount));
	opennl::nlBegin(c, NL_SYSTEM);
	for (uint32_t i = 0; i < vertexCount; ++i) {
		opennl::nlSetVariable(c, 2 * i, m.texcoord(i).x);
		opennl::nlSetVariable(c, 2 * i + 1, m.texcoord(i).y);
	}
	opennl::nlBegin(c, NL_MATRIX);
	ConstArrayView<Vector3> positions = m.positions();
	ConstArrayView<uint32_t> indices = m.indices();
	bool allAbf = true;
	for (uint32_t f = 0; f < m.faceCount(); ++f) {
		const uint32_t v0 = indices[f * 3 + 0];
		const uint32_t v1 = indices[f * 3 + 1];
		const uint32_t v2 = indices[f * 3 + 2];
		if (!param::setup_abf_relations(c, (int)v0, (int)v1, (int)v2, positions[v0], positions[v1], positions[v2])) {
			allAbf = false;
			Vector2 z0, z1, z2;
			param::projectTriangle(positions[v0], positions[v1], positions[v2], &z0, &z1, &z2);
			const double a = z1.x - z0.x, bb = z1.y - z0.y;
			const double cc = z2.x - z0.x, d = z2.y - z0.y;
			const uint32_t u0 = 2 * v0, v0id = 2 * v0 + 1;
			const uint32_t u1 = 2 * v1, v1id = 2 * v1 + 1;
			const uint32_t u2 = 2 * v2, v2id = 2 * v2 + 1;
			opennl::nlBegin(c, NL_ROW);
			opennl::nlCoefficient(c, u0, -a + cc); opennl::nlCoefficient(c, v0id, bb - d);
			opennl::nlCoefficient(c, u1, -cc); opennl::nlCoefficient(c, v1id, d);
			opennl::nlCoefficient(c, u2, a);
			opennl::nlEnd(c, NL_ROW);
			opennl::nlBegin(c, NL_ROW);
			opennl::nlCoefficient(c, u0, -bb + d); opennl::nlCoefficient(c, v0id, -a + cc);
			opennl::nlCoefficient(c, u1, -d); opennl::nlCoefficient(c, v1id, -cc);
			opennl::nlCoefficient(c, v2id, a);
			opennl::nlEnd(c, NL_ROW);
		}
	}
	opennl::nlEnd(c, NL_MATRIX);
	opennl::nlEnd(c, NL_SYSTEM);

	std::vector<double> uv(2 * vertexCount);
	double uvNormSq = 0.0;
	for (uint32_t i = 0; i < vertexCount; ++i) {
		uv[2 * i] = m.texcoord(i).x;
		uv[2 * i + 1] = m.texcoord(i).y;
		uvNormSq += uv[2 * i] * uv[2 * i] + uv[2 * i + 1] * uv[2 * i + 1];
	}
	RelationEnergy energy;
	energy.rows = c->lsqr_m;
	double rowScaleSq = 0.0;
	for (uint32_t r = 0; r < c->lsqr_m; ++r) {
		double residual = -c->lsqr_b[r];
		double rowNormSq = 0.0;
		const opennl::NLRowColumn *row = &c->lsqr_rows[r];
		for (uint32_t j = 0; j < row->size; ++j) {
			// No locks were made, so system index == original variable index.
			residual += row->coeff[j].value * uv[row->coeff[j].index];
			rowNormSq += row->coeff[j].value * row->coeff[j].value;
		}
		energy.squared += residual * residual;
		rowScaleSq += rowNormSq;
	}
	energy.normalized = energy.squared / std::max(1e-300, rowScaleSq * uvNormSq);
	opennl::nlDeleteContext(c);
	(void)allAbf;
	return energy;
}

static void checkDiameterLocks(Case &cs, const Mesh &m)
{
	uint32_t a = 0, b = 0;
	const bool found = param::findApproximateDiameterVertices(const_cast<Mesh *>(&m), &a, &b);
	cs.check(found && a != b, "diameter lock vertices must be found");
	cs.check(std::isfinite(m.texcoord(a).x) && std::isfinite(m.texcoord(a).y), "lock a finite");
	cs.check(std::isfinite(m.texcoord(b).x) && std::isfinite(m.texcoord(b).y), "lock b finite");
}

static int lscm_direct_strip(Case &cs)
{
	Mesh lsqrMesh(1e-6f, 24, 22);
	buildCurvedStrip(lsqrMesh, 14);
	Mesh cgMesh(1e-6f, 24, 22);
	buildCurvedStrip(cgMesh, 14);

	uint32_t la = 0, lb = 0;
	param::findApproximateDiameterVertices(&lsqrMesh, &la, &lb);
	const Vector2 lockA = lsqrMesh.texcoord(la);
	const Vector2 lockB = lsqrMesh.texcoord(lb);

	const bool lsqrOk = param::computeLeastSquaresConformalMap(&lsqrMesh, true);
	const bool cgOk = param::computeLeastSquaresConformalMap(&cgMesh, false);
	cs.check(lsqrOk, "LSQR LSCM must parameterize the long unequal-width strip");
	cs.check(cgOk, "CG LSCM control must still succeed");
	checkNear(cs, lsqrMesh.texcoord(la).x, lockA.x, 1e-6, "LSQR lock u preserved through float mesh");
	checkNear(cs, lsqrMesh.texcoord(la).y, lockA.y, 1e-6, "LSQR lock v preserved through float mesh");
	checkNear(cs, lsqrMesh.texcoord(lb).x, lockB.x, 1e-6, "LSQR second lock u preserved");
	checkNear(cs, lsqrMesh.texcoord(lb).y, lockB.y, 1e-6, "LSQR second lock v preserved");

	const RelationEnergy le = conformalRelationEnergy(lsqrMesh);
	const RelationEnergy ce = conformalRelationEnergy(cgMesh);
	cs.check(le.rows > 0 && le.squared >= 0.0 && std::isfinite(le.normalized), "strip relation energy must be finite");
	cs.check(le.normalized < 1e-8, "developable curved strip should have very small LSCM relation energy");
	cs.check(le.normalized <= ce.normalized * 10.0 + 1e-12, "LSQR energy should be comparable to CG control");
	cs.check(lsqrMesh.computeFaceParametricArea(0) != 0.0f, "strip face has parametric area");
	return 0;
}

static int lscm_direct_patch(Case &cs)
{
	Mesh m(1e-6f, 25, 32);
	buildBumpyPatch(m);
	const bool ok = param::computeLeastSquaresConformalMap(&m, true);
	cs.check(ok, "LSQR must solve the non-planar open patch with an interior vertex");
	for (uint32_t i = 0; i < m.vertexCount(); ++i)
		cs.check(isFinite(m.texcoord(i)), "patch UV must be finite");
	const RelationEnergy energy = conformalRelationEnergy(m);
	cs.check(energy.rows == 2 * m.faceCount(), "patch should provide two conformal rows per face");
	cs.check(energy.normalized < 1e-7, "gentle non-planar patch should have small normalized LSCM energy");
	checkDiameterLocks(cs, m);

	// Re-running the same fixed chart must not depend on prior solver state.
	const RelationEnergy first = energy;
	cs.check(param::computeLeastSquaresConformalMap(&m, true), "A/A rerun must succeed");
	const RelationEnergy second = conformalRelationEnergy(m);
	cs.check(std::fabs(first.normalized - second.normalized) <= 1e-10 * std::max(1.0, first.normalized),
		"fixed chart LSQR rerun must be stable");
	return 0;
}

static std::atomic<int> g_paramFuncCalls(0);
static void priorityParamFunc(const float *positions, float *texcoords, uint32_t vertexCount, const uint32_t *, uint32_t)
{
	++g_paramFuncCalls;
	for (uint32_t i = 0; i < vertexCount; ++i) {
		texcoords[2 * i + 0] = positions[3 * i + 0];
		texcoords[2 * i + 1] = positions[3 * i + 1];
	}
}

static void appendStripDeclGeometry(std::vector<float> &positions, std::vector<uint32_t> &indices, uint32_t segments)
{
	const uint32_t columns = segments + 1;
	const uint32_t base = (uint32_t)positions.size() / 3;
	for (uint32_t i = 0; i <= segments; ++i) {
		const float x = float(i) * 0.35f;
		const float width = 0.6f + 0.4f * float(i) / float(segments);
		const float z = 0.12f * x * x;
		positions.insert(positions.end(), {x, 0.0f, z});
		positions.insert(positions.end(), {x, width, z + 0.03f * width});
	}
	for (uint32_t i = 0; i < segments; ++i) {
		const uint32_t b0 = base + i, b1 = base + i + 1;
		const uint32_t t0 = base + columns + i, t1 = base + columns + i + 1;
		const uint32_t tris[6] = {b0, b1, t1, b0, t1, t0};
		for (uint32_t v : tris)
			indices.push_back(v);
	}
}

static void appendPatchDeclGeometry(std::vector<float> &positions, std::vector<uint32_t> &indices, int n = 6)
{
	const uint32_t base = (uint32_t)positions.size() / 3;
	for (int y = 0; y <= n; ++y) {
		for (int x = 0; x <= n; ++x) {
			const float fx = float(x - n / 2);
			const float fy = float(y - n / 2);
			const float z = 0.8f * std::sin(fx) * std::cos(fy) + 0.3f * fx;
			positions.insert(positions.end(), {fx, fy, z});
		}
	}
	for (int y = 0; y < n; ++y) {
		for (int x = 0; x < n; ++x) {
			const uint32_t a = base + y * (n + 1) + x;
			const uint32_t b = a + 1;
			const uint32_t c = a + (n + 1);
			const uint32_t d = c + 1;
			indices.insert(indices.end(), {a, b, d, a, d, c});
		}
	}
}

static int lscm_public_pipeline(Case &cs)
{
	xatlas::Atlas *atlas = xatlas::Create();
	cs.check(atlas != nullptr, "atlas created");

	for (int meshIndex = 0; meshIndex < 4; ++meshIndex) {
		std::vector<float> positions;
		std::vector<uint32_t> indices;
		appendStripDeclGeometry(positions, indices, meshIndex % 2 ? 10 : 16);
		xatlas::MeshDecl decl;
		decl.vertexCount = (uint32_t)positions.size() / 3;
		decl.vertexPositionData = positions.data();
		decl.vertexPositionStride = 3 * sizeof(float);
		decl.indexCount = (uint32_t)indices.size();
		decl.indexData = indices.data();
		decl.indexFormat = xatlas::IndexFormat::UInt32;
		const xatlas::AddMeshError err = xatlas::AddMesh(atlas, decl);
		cs.check(err == xatlas::AddMeshError::Success, "AddMesh strip must succeed");
	}

	std::vector<float> patchPositions;
	std::vector<uint32_t> patchIndices;
	appendPatchDeclGeometry(patchPositions, patchIndices, 5);
	xatlas::MeshDecl patchDecl;
	patchDecl.vertexCount = (uint32_t)patchPositions.size() / 3;
	patchDecl.vertexPositionData = patchPositions.data();
	patchDecl.vertexPositionStride = 3 * sizeof(float);
	patchDecl.indexCount = (uint32_t)patchIndices.size();
	patchDecl.indexData = patchIndices.data();
	patchDecl.indexFormat = xatlas::IndexFormat::UInt32;
	cs.check(xatlas::AddMesh(atlas, patchDecl) == xatlas::AddMeshError::Success, "AddMesh non-planar patch must succeed");
	xatlas::AddMeshJoin(atlas);

	xatlas::ChartOptions options;
	options.useLsqr = true;
	xatlas::ComputeCharts(atlas, options);
	xatlas::PackOptions pack;
	pack.padding = 2;
	xatlas::PackCharts(atlas, pack);
	cs.check(atlas->meshCount == 5, "all five parallel chart meshes remain");
	cs.check(atlas->chartCount > 0, "LSQR option public ComputeCharts produced charts");
	cs.check(atlas->width > 0 && atlas->height > 0, "LSQR option PackCharts produced an atlas");
	cs.check(atlas->meshes != nullptr, "packed output meshes exist");
	xatlas::Destroy(atlas);
	return 0;
}

static int lscm_option_controls(Case &cs)
{
	// A/B/A recomputation on one atlas must not leak rows or locked values.
	xatlas::Atlas *atlas = xatlas::Create();
	std::vector<float> positions;
	std::vector<uint32_t> indices;
	appendStripDeclGeometry(positions, indices, 14);
	xatlas::MeshDecl decl;
	decl.vertexCount = (uint32_t)positions.size() / 3;
	decl.vertexPositionData = positions.data();
	decl.vertexPositionStride = 3 * sizeof(float);
	decl.indexCount = (uint32_t)indices.size();
	decl.indexData = indices.data();
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	cs.check(xatlas::AddMesh(atlas, decl) == xatlas::AddMeshError::Success, "control mesh added");
	xatlas::AddMeshJoin(atlas);

	xatlas::PackOptions pack;
	xatlas::ChartOptions lsqrOn;
	lsqrOn.useLsqr = true;
	xatlas::ComputeCharts(atlas, lsqrOn);
	xatlas::PackCharts(atlas, pack);
	const uint32_t firstCharts = atlas->chartCount;
	cs.check(firstCharts > 0 && atlas->width > 0, "A: LSQR run packed");

	xatlas::ChartOptions cgOff;
	cgOff.useLsqr = false;
	xatlas::ComputeCharts(atlas, cgOff);
	xatlas::PackCharts(atlas, pack);
	cs.check(atlas->chartCount > 0 && atlas->width > 0, "B: default CG control packed");

	xatlas::ComputeCharts(atlas, lsqrOn);
	xatlas::PackCharts(atlas, pack);
	cs.check(atlas->chartCount > 0 && atlas->width > 0, "A: repeated LSQR run packed");
	xatlas::Destroy(atlas);

	// A strongly non-planar patch reaches the LSCM branch, where paramFunc
	// must take priority over the useLsqr solver selection.
	atlas = xatlas::Create();
	std::vector<float> patchPositions;
	std::vector<uint32_t> patchIndices;
	appendPatchDeclGeometry(patchPositions, patchIndices, 6);
	xatlas::MeshDecl patchDecl;
	patchDecl.vertexCount = (uint32_t)patchPositions.size() / 3;
	patchDecl.vertexPositionData = patchPositions.data();
	patchDecl.vertexPositionStride = 3 * sizeof(float);
	patchDecl.indexCount = (uint32_t)patchIndices.size();
	patchDecl.indexData = patchIndices.data();
	patchDecl.indexFormat = xatlas::IndexFormat::UInt32;
	cs.check(xatlas::AddMesh(atlas, patchDecl) == xatlas::AddMeshError::Success, "paramFunc patch added");
	xatlas::AddMeshJoin(atlas);

	g_paramFuncCalls.store(0);
	xatlas::ChartOptions paramFirst;
	paramFirst.useLsqr = true;
	paramFirst.paramFunc = priorityParamFunc;
	xatlas::ComputeCharts(atlas, paramFirst);
	xatlas::PackCharts(atlas, pack);
	cs.check(g_paramFuncCalls.load() > 0, "paramFunc must be called and take priority when useLsqr is true");
	cs.check(atlas->chartCount > 0, "paramFunc control still produces charts");
	xatlas::Destroy(atlas);
	return 0;
}

} // namespace

void register_task_tests(Registry &registry)
{
	registry.add("rectangular-ls", "reference", rectangular_reference);
	registry.add("rectangular-ls", "duplicates-and-locks", rectangular_duplicates_locks);
	registry.add("rectangular-ls", "degenerate-and-scale", rectangular_degenerate_scales);
	registry.add("stopping", "iteration-limit", stopping_iteration_limit);
	registry.add("stopping", "invalid-input", stopping_invalid_input);
	registry.add("stopping", "breakdown", stopping_breakdown);
	registry.add("lscm-integration", "direct-curved-strip", lscm_direct_strip);
	registry.add("lscm-integration", "direct-bumpy-patch", lscm_direct_patch);
	registry.add("lscm-integration", "public-add-compute-pack", lscm_public_pipeline);
	registry.add("lscm-integration", "cg-off-and-paramfunc-controls", lscm_option_controls);
}
