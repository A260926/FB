#pragma once

#include <cstddef>
#include <vector>

// A causal temporal point-spread function built as the convolution of
// independent exponential dwell kernels, one per diffuse interaction along a
// light path. Each kernel is either:
//   - instantaneous (tau = 0): a unit delta at time 0, contributing no delay;
//   - exponential (tau in [1e-6, 1e6] ns): density (1/tau) exp(-t/tau), t >= 0,
//     a normalised probability measure of mean tau.
//
// The response owns its lifetime array; once constructed it is immutable.
// Appending another kernel returns a *new* response, so response prefixes handed
// to earlier path contributions can never be changed by later bounces. An empty
// chain (and a chain of only zero lifetimes) is the unit impulse at 0.
//
// All evaluation is analytic: repeated equal lifetimes fall out as the Erlang
// distribution automatically, and near-equal lifetimes are handled by a
// numerically stable series expansion, never by merging lifetimes or sampling
// random delays. Up to 64 kernels are allowed.
class TemporalResponse {
public:
	// Valid lifetime limits for a non-instantaneous kernel (nanoseconds).
	static constexpr double kMinTau = 1e-6;
	static constexpr double kMaxTau = 1e6;
	static constexpr std::size_t kMaxKernels = 64;

	// Unit impulse at time 0.
	TemporalResponse() = default;

	// From an explicit list of lifetimes. Each entry must be 0 or a finite value
	// in [kMinTau, kMaxTau]; at most kMaxKernels entries. Throws std::invalid_argument
	// on any violation.
	explicit TemporalResponse(const std::vector<double> &lifetimes);

	// Number of appended kernels (zero lifetimes included).
	std::size_t size() const { return taus_.size(); }

	// The owned lifetime at index i (0 = instantaneous).
	double tauAt(std::size_t i) const { return taus_.at(i); }

	// A copy of the owned lifetime array.
	std::vector<double> lifetimes() const { return taus_; }

	// Mean delay in nanoseconds (sum of the lifetimes).
	double mean() const;

	// Convolve this response with one more exponential (tau = 0 allowed) and
	// return the new response; this object is untouched. Throws
	// std::invalid_argument on a bad tau or when the chain is already full.
	TemporalResponse append(double tau) const;

	// Probability mass in the half-open interval [a, b): 0 when a == b. Either
	// endpoint may be +/- infinity. Throws std::invalid_argument on NaN or an
	// inverted interval (a > b). The result is always in [0, 1].
	double mass(double a, double b) const;

	// Cumulative distribution function P(T <= t); t may be -infinity (0) or
	// +infinity (1). NaN is rejected.
	double cdf(double t) const;

private:
	// Instantaneous kernels are not stored: they do not change the measure.
	std::vector<double> taus_;

	static void validateTau(double tau);
};
