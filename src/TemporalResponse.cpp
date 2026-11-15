#include "TemporalResponse.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
constexpr double kInf = std::numeric_limits<double>::infinity();

// Scaling bound for the order-(13,13) Pade approximant of the matrix exponential
// (Higham, "Functions of Matrices", Table 10.4): with ||A||_1 <= theta_13 the
// approximant has a forward error at machine-rounding level. The evaluation
// scales to half the bound for margin, then squares the result back.
constexpr double kThetaPade13 = 5.371920351148152e-1;
constexpr int kPadeOrder = 13;

// Diagonal Pade coefficients b_j = (2n-j)! n! / ((2n)! j! (n-j)!). Factorials
// up to 26! fit exactly in a double's mantissa range.
std::array<double, kPadeOrder + 1> padeCoefficients() {
	std::array<double, 2 * kPadeOrder + 1> fact{};
	fact[0] = 1.0;
	for (int i = 1; i <= 2 * kPadeOrder; ++i)
		fact[i] = fact[i - 1] * static_cast<double>(i);
	std::array<double, kPadeOrder + 1> b{};
	const int n = kPadeOrder;
	for (int j = 0; j <= n; ++j)
		b[j] = fact[2 * n - j] * fact[n] / (fact[2 * n] * fact[j] * fact[n - j]);
	return b;
}

// Dense y = M x for a small row-major matrix (n <= 64).
void matVec(const std::vector<double> &m, const std::vector<double> &x, std::vector<double> &y, int n) {
	for (int i = 0; i < n; ++i) {
		double acc = 0.0;
		const double *row = &m[static_cast<std::size_t>(i) * n];
		for (int j = 0; j < n; ++j)
			acc += row[j] * x[j];
		y[i] = acc;
	}
}

// Survival P(T > t) for the sum of n independent Exp(rate_i) variables through
// their phase-type (Coxian) subgenerator Q: Q[i][i] = -rate_i,
// Q[i][i+1] = rate_i, so the survival is component 0 of exp(Q t) 1. The
// exponential is a scaling-and-squaring Pade approximation, an error-controlled
// matrix method: scaling keeps ||Q t / 2^s||_1 below theta_13, which bounds the
// Pade truncation error by machine epsilon, and every squaring is a
// substochastic contraction, so equal rates (Erlang) and near-equal rates are
// handled identically, with no coefficient cancellation anywhere.
double survival(const std::vector<double> &rates, double t) {
	const int n = static_cast<int>(rates.size());
	if (n == 0)
		return t > 0.0 ? 0.0 : 1.0; // unit atom at time 0
	if (t <= 0.0)
		return 1.0;

	std::vector<double> q(static_cast<std::size_t>(n) * n, 0.0);
	for (int i = 0; i < n; ++i) {
		q[static_cast<std::size_t>(i) * n + i] = -rates[i];
		if (i + 1 < n)
			q[static_cast<std::size_t>(i) * n + (i + 1)] = rates[i];
	}

	// 1-norm of Q: column j holds -rate_j on the diagonal and rate_{j-1} on the
	// superdiagonal.
	double normQ = 0.0;
	for (int j = 0; j < n; ++j) {
		double col = rates[j];
		if (j > 0)
			col += rates[j - 1];
		normQ = std::max(normQ, col);
	}

	// Smallest s with ||Q * t / 2^s||_1 <= theta_13 / 2.
	const double target = kThetaPade13 * 0.5;
	int s = 0;
	double scale = t;
	while (normQ * scale > target) {
		scale *= 0.5;
		++s;
		if (s > 4096)
			throw std::runtime_error("TemporalResponse: time argument out of evaluable range");
	}

	std::vector<double> a(static_cast<std::size_t>(n) * n);
	for (std::size_t k = 0; k < q.size(); ++k)
		a[k] = q[k] * scale;

	const auto bcoef = padeCoefficients();

	// Numerator N(A) = sum_j b_j A^j and denominator D(A) = sum_j b_j (-A)^j,
	// built together from the running matrix power.
	std::vector<double> num(n * n, 0.0);
	std::vector<double> den(n * n, 0.0);
	std::vector<double> power(n * n, 0.0);
	for (int i = 0; i < n; ++i)
		power[static_cast<std::size_t>(i) * n + i] = 1.0; // A^0 = I
	for (int j = 0; j <= kPadeOrder; ++j) {
		const double sign = (j % 2) ? -1.0 : 1.0;
		for (std::size_t k = 0; k < den.size(); ++k) {
			den[k] += sign * bcoef[j] * power[k];
			num[k] += bcoef[j] * power[k];
		}
		if (j < kPadeOrder) {
			std::vector<double> next(n * n, 0.0);
			for (int i = 0; i < n; ++i)
				for (int kk = 0; kk < n; ++kk) {
					const double aik = power[static_cast<std::size_t>(i) * n + kk];
					if (aik == 0.0)
						continue;
					for (int jcol = 0; jcol < n; ++jcol)
						next[static_cast<std::size_t>(i) * n + jcol] +=
						    aik * a[static_cast<std::size_t>(kk) * n + jcol];
				}
			power.swap(next);
		}
	}

	// R(A) = D(A)^{-1} N(A): LU-factor D once, then solve against every column
	// of N to materialise the dense approximant of exp(A).
	std::vector<double> lu = den;
	std::vector<int> piv(n);
	for (int k = 0; k < n; ++k) {
		int pivot = k;
		double best = std::abs(lu[static_cast<std::size_t>(k) * n + k]);
		for (int i = k + 1; i < n; ++i) {
			const double v = std::abs(lu[static_cast<std::size_t>(i) * n + k]);
			if (v > best) {
				best = v;
				pivot = i;
			}
		}
		piv[k] = pivot;
		if (pivot != k)
			for (int j = 0; j < n; ++j)
				std::swap(lu[static_cast<std::size_t>(k) * n + j],
				          lu[static_cast<std::size_t>(pivot) * n + j]);
		const double diag = lu[static_cast<std::size_t>(k) * n + k];
		if (diag == 0.0)
			throw std::runtime_error("TemporalResponse: singular Pade denominator");
		for (int i = k + 1; i < n; ++i) {
			const double f = lu[static_cast<std::size_t>(i) * n + k] / diag;
			lu[static_cast<std::size_t>(i) * n + k] = f;
			for (int j = k + 1; j < n; ++j)
				lu[static_cast<std::size_t>(i) * n + j] -=
				    f * lu[static_cast<std::size_t>(k) * n + j];
		}
	}
	std::vector<double> rmat(n * n, 0.0);
	for (int col = 0; col < n; ++col) {
		std::vector<double> rhs(n);
		for (int i = 0; i < n; ++i)
			rhs[i] = num[static_cast<std::size_t>(i) * n + col];
		for (int k = 0; k < n; ++k) {
			if (piv[k] != k)
				std::swap(rhs[k], rhs[piv[k]]);
			for (int i = k + 1; i < n; ++i)
				rhs[i] -= lu[static_cast<std::size_t>(i) * n + k] * rhs[k];
		}
		for (int i = n - 1; i >= 0; --i) {
			double acc = rhs[i];
			for (int j = i + 1; j < n; ++j)
				acc -= lu[static_cast<std::size_t>(i) * n + j] * rhs[j];
			rhs[i] = acc / lu[static_cast<std::size_t>(i) * n + i];
		}
		for (int i = 0; i < n; ++i)
			rmat[static_cast<std::size_t>(i) * n + col] = rhs[i];
	}

	// Undo the scaling by repeated matrix squaring: R(A)^{2^s} = exp(Qt).
	for (int k = 0; k < s; ++k) {
		std::vector<double> squared(n * n, 0.0);
		for (int i = 0; i < n; ++i)
			for (int kk = 0; kk < n; ++kk) {
				const double rik = rmat[static_cast<std::size_t>(i) * n + kk];
				if (rik == 0.0)
					continue;
				for (int jcol = 0; jcol < n; ++jcol)
					squared[static_cast<std::size_t>(i) * n + jcol] +=
					    rik * rmat[static_cast<std::size_t>(kk) * n + jcol];
			}
		rmat.swap(squared);
	}

	std::vector<double> v(n, 1.0);
	std::vector<double> out(n, 0.0);
	matVec(rmat, v, out, n);
	return out[0];
}
} // namespace

void TemporalResponse::validateTau(double tau) {
	if (std::isnan(tau))
		throw std::invalid_argument("TemporalResponse: tau must not be NaN");
	if (tau == 0.0)
		return;
	if (!std::isfinite(tau) || tau < kMinTau || tau > kMaxTau)
		throw std::invalid_argument("TemporalResponse: tau must be 0 or a finite value in [1e-6, 1e6] ns");
}

TemporalResponse::TemporalResponse(const std::vector<double> &lifetimes) {
	if (lifetimes.size() > kMaxKernels)
		throw std::invalid_argument("TemporalResponse: at most 64 kernels are allowed");
	taus_.reserve(lifetimes.size());
	for (double tau : lifetimes) {
		validateTau(tau);
		taus_.push_back(tau);
	}
}

double TemporalResponse::mean() const {
	double total = 0.0;
	for (double tau : taus_)
		total += tau;
	return total;
}

TemporalResponse TemporalResponse::append(double tau) const {
	validateTau(tau);
	if (taus_.size() >= kMaxKernels)
		throw std::length_error("TemporalResponse: the kernel chain is already at its 64-kernel limit");
	TemporalResponse result;
	result.taus_ = taus_;
	result.taus_.push_back(tau);
	return result;
}

double TemporalResponse::cdf(double t) const {
	if (std::isnan(t))
		throw std::invalid_argument("TemporalResponse::cdf: t must not be NaN");

	std::vector<double> rates;
	rates.reserve(taus_.size());
	for (double tau : taus_)
		if (tau > 0.0)
			rates.push_back(1.0 / tau);

	if (rates.empty())
		return t >= 0.0 ? 1.0 : 0.0; // unit impulse at time 0
	if (t < 0.0)
		return 0.0;
	if (t == kInf)
		return 1.0;

	const double s = survival(rates, t);
	const double result = 1.0 - s;
	if (result < 0.0 && result > -1e-12)
		return 0.0;
	if (result > 1.0 && result < 1.0 + 1e-12)
		return 1.0;
	return std::clamp(result, 0.0, 1.0);
}

double TemporalResponse::mass(double a, double b) const {
	if (std::isnan(a) || std::isnan(b))
		throw std::invalid_argument("TemporalResponse::mass: endpoints must not be NaN");
	if (a > b)
		throw std::invalid_argument("TemporalResponse::mass: inverted interval [a, b)");
	if (a == b)
		return 0.0;

	// An all-instantaneous chain is a unit atom at time 0, included by the
	// half-open interval exactly when a <= 0 < b.
	bool hasPositive = false;
	for (double tau : taus_)
		if (tau > 0.0)
			hasPositive = true;
	if (!hasPositive)
		return (a <= 0.0 && b > 0.0) ? 1.0 : 0.0;

	const double m = cdf(b) - cdf(a);
	if (m < 0.0 && m > -1e-12)
		return 0.0;
	if (m > 1.0 && m < 1.0 + 1e-12)
		return 1.0;
	return std::clamp(m, 0.0, 1.0);
}
