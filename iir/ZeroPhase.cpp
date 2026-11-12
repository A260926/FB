/**
 *
 * "A Collection of Useful C++ Classes for Digital Signal Processing"
 * By Vinnie Falco and Bernd Porr
 *
 * Official project location:
 * https://github.com/berndporr/iir1
 *
 * See Documentation.cpp for contact information, notes, and bibliography.
 *
 * -----------------------------------------------------------------
 *
 * License: MIT License (http://www.opensource.org/licenses/mit-license.php)
 * Copyright (c) 2009 by Vinnie Falco
 * Copyright (c) 2011 by Bernd Porr
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 **/

#include "Common.h"
#include "Biquad.h"
#include "Cascade.h"
#include "ZeroPhase.h"

#include <algorithm>
#include <cmath>

namespace Iir
{

namespace
{

const char errStorage[] = "ZeroPhasePlan needs a non-null coefficient array.";
const char errStages[] = "ZeroPhasePlan supports between 1 and 6 sections.";
const char errCapacity[] = "ZeroPhasePlan needs coefficient storage for every used section.";
const char errLength[] = "ZeroPhasePlan needs a segment length of at least one sample.";
const char errChannels[] = "ZeroPhasePlan needs a channel capacity of at least one.";
const char errCoefficient[] = "ZeroPhasePlan received a non-finite or degenerate coefficient.";
const char errUnstable[] = "ZeroPhasePlan received a section whose poles are not strictly inside the unit circle.";
const char errChannelRange[] = "ZeroPhasePlan::process needs a channel count within its configured capacity.";
const char errPlanes[] = "ZeroPhasePlan::process received a null channel array or plane pointer.";
const char errStaleChannel[] = "ZeroPhasePlan diagnostics were queried for a channel of an earlier call.";

bool isStableSection(const Biquad& s)
{
	const double a1 = s.m_a1;
	const double a2 = s.m_a2;

	if (a2 == 0.0)
	{
		// First order section or no feedback at all.
		return a1 == 0.0 || std::abs(a1) < 1.0;
	}

	const double discriminant = a1 * a1 - 4.0 * a2;
	if (discriminant >= 0.0)
	{
		const double root = std::sqrt(discriminant);
		const double r1 = (-a1 + root) * 0.5;
		const double r2 = (-a1 - root) * 0.5;
		return std::abs(r1) < 1.0 && std::abs(r2) < 1.0;
	}

	// Complex conjugate pair: the product of the poles is a2.
	return a2 > 0.0 && a2 < 1.0;
}

// One pass of the cascade.  The two delay values of every stage live
// as consecutive pairs in state and are updated in place.
void runSegment(const Biquad* stage, int numStages,
                const double* in, double* out, std::size_t length,
                double* state)
{
	for (std::size_t k = 0; k < length; ++k)
	{
		double value = in[k];
		for (int i = 0; i < numStages; ++i)
		{
			const Biquad& s = stage[i];
			const double y = state[2 * i] + s.m_b0 * value;
			const double next1 = state[2 * i + 1] + s.m_b1 * value - s.m_a1 * y;
			const double next2 = s.m_b2 * value - s.m_a2 * y;
			state[2 * i] = next1;
			state[2 * i + 1] = next2;
			value = y;
		}
		out[k] = value;
	}
}

// Same cascade, traversed from the last sample to the first.
void runSegmentBackward(const Biquad* stage, int numStages,
                        const double* in, double* out, std::size_t length,
                        double* state)
{
	for (std::size_t k = length; k-- > 0; )
	{
		double value = in[k];
		for (int i = 0; i < numStages; ++i)
		{
			const Biquad& s = stage[i];
			const double y = state[2 * i] + s.m_b0 * value;
			const double next1 = state[2 * i + 1] + s.m_b1 * value - s.m_a1 * y;
			const double next2 = s.m_b2 * value - s.m_a2 * y;
			state[2 * i] = next1;
			state[2 * i + 1] = next2;
			value = y;
		}
		out[k] = value;
	}
}

void clearState(double* state, int stateSize)
{
	for (int i = 0; i < stateSize; ++i)
		state[i] = 0.0;
}

}

//------------------------------------------------------------------------------

ZeroPhasePlan::ZeroPhasePlan(const Cascade::Storage& storage,
                             int stages,
                             std::size_t length,
                             std::size_t maxChannels)
{
	if (storage.stageArray == nullptr)
		throw_invalid_argument(errStorage);
	if (stages < 1 || stages > maxSupportedStages)
		throw_invalid_argument(errStages);
	if (storage.maxStages < stages)
		throw_invalid_argument(errCapacity);
	if (length < 1)
		throw_invalid_argument(errLength);
	if (maxChannels < 1)
		throw_invalid_argument(errChannels);

	for (int i = 0; i < stages; ++i)
	{
		const Biquad& source = storage.stageArray[i];

		const double c[6] = { source.m_a0, source.m_a1, source.m_a2,
		                      source.m_b0, source.m_b1, source.m_b2 };
		for (int j = 0; j < 6; ++j)
			if (!std::isfinite(c[j]))
				throw_invalid_argument(errCoefficient);
		if (source.m_a0 == 0.0)
			throw_invalid_argument(errCoefficient);
		if (!isStableSection(source))
			throw_invalid_argument(errUnstable);

		// Copy the coefficients; the caller's storage is not borrowed.
		m_stage[i] = source;
	}

	m_stages = stages;
	m_length = length;
	m_maxChannels = maxChannels;

	const int stateSize = 2 * m_stages;
	const int parameters = 2 * stateSize;

	// Everything which depends on the segment length or on the channel
	// capacity is allocated here, never inside process().
	m_x.resize(m_length);
	m_b0.resize(m_length);
	m_b1.resize(m_length);
	m_b2.resize(m_length);
	m_c.resize(m_length);
	m_res.resize(m_length);
	m_yf.resize(m_length);
	m_state.resize(stateSize);
	m_z.resize(parameters);
	m_work.resize(m_length * static_cast<std::size_t>(parameters));
	m_v.resize(static_cast<std::size_t>(parameters) * parameters);
	m_sigma.resize(parameters);
	m_zero.resize(m_length);

	for (std::size_t k = 0; k < m_length; ++k)
		m_zero[k] = 0.0;

	m_diagnostics.stages = m_stages;
	m_diagnostics.channels = 0;
	m_diagnostics.decompositions = 0;
	m_diagnostics.rank = 0;
	m_diagnostics.forwardStates.assign(maxChannels, std::vector<double>(stateSize));
	m_diagnostics.reverseStates.assign(maxChannels, std::vector<double>(stateSize));
	m_diagnostics.boundaryResiduals.assign(maxChannels, 0.0);

	buildBoundarySystem();
	decompose();

	m_decompositions = 1;
	m_diagnostics.decompositions = m_decompositions;
	m_diagnostics.rank = m_rank;
}

//------------------------------------------------------------------------------

void ZeroPhasePlan::buildBoundarySystem()
{
	const std::size_t n = m_length;
	const int stateSize = 2 * m_stages;
	const int parameters = 2 * stateSize;

	// With zero input the state decays without any excitation.  The
	// response to e_j is independent of the segment and becomes one of
	// the two boundary columns of that state coordinate.
	for (int j = 0; j < stateSize; ++j)
	{
		clearState(m_state.data(), stateSize);
		m_state[j] = 1.0;
		runSegment(m_stage, m_stages, m_zero.data(), m_yf.data(), n, m_state.data());
		clearState(m_state.data(), stateSize);

		// Forward state block: B0(T_j) - T_j, where
		// B0(y) = R F0(R y).
		for (std::size_t k = 0; k < n; ++k)
			m_res[k] = m_yf[n - 1 - k];
		runSegment(m_stage, m_stages, m_res.data(), m_b0.data(), n, m_state.data());

		for (std::size_t k = 0; k < n; ++k)
			m_work[k * parameters + j] = m_b0[n - 1 - k] - m_yf[k];

		// Backward state block: R T_j - F0(R T_j).  At this point
		// m_res holds R T_j and m_b0 holds F0(R T_j).
		for (std::size_t k = 0; k < n; ++k)
			m_work[k * parameters + stateSize + j] = m_res[k] - m_b0[k];
	}
}

//------------------------------------------------------------------------------

void ZeroPhasePlan::decompose()
{
	const int parameters = 2 * 2 * m_stages;
	const int n = static_cast<int>(m_length);

	// One-sided Jacobi: rotate the columns of A until they are pairwise
	// orthogonal.  Then A*V = W, the norms of the columns of W are the
	// singular values, and their normalized columns are the left
	// singular vectors.  It handles tall, square and wide matrices.
	for (int i = 0; i < parameters; ++i)
		for (int j = 0; j < parameters; ++j)
			m_v[i * parameters + j] = (i == j) ? 1.0 : 0.0;

	for (int sweep = 0; sweep < 100; ++sweep)
	{
		bool changed = false;

		for (int p = 0; p < parameters; ++p)
		{
			for (int q = p + 1; q < parameters; ++q)
			{
				double alpha = 0.0;
				double beta = 0.0;
				double gamma = 0.0;
				for (int k = 0; k < n; ++k)
				{
					const double wp = m_work[k * parameters + p];
					const double wq = m_work[k * parameters + q];
					alpha += wp * wp;
					beta += wq * wq;
					gamma += wp * wq;
				}

				if (gamma == 0.0 || alpha == 0.0 || beta == 0.0)
					continue;
				if (std::abs(gamma) <= 1.0e-17 * std::sqrt(alpha * beta))
					continue;

				changed = true;

				const double tau = (beta - alpha) / (2.0 * gamma);
				double t;
				if (tau >= 0.0)
					t = 1.0 / (tau + std::sqrt(1.0 + tau * tau));
				else
					t = -1.0 / (-tau + std::sqrt(1.0 + tau * tau));
				const double c = 1.0 / std::sqrt(1.0 + t * t);
				const double s = c * t;

				for (int k = 0; k < n; ++k)
				{
					double& wp = m_work[k * parameters + p];
					double& wq = m_work[k * parameters + q];
					const double a = wp;
					const double b = wq;
					wp = c * a - s * b;
					wq = s * a + c * b;
				}

				for (int k = 0; k < parameters; ++k)
				{
					double& vp = m_v[k * parameters + p];
					double& vq = m_v[k * parameters + q];
					const double a = vp;
					const double b = vq;
					vp = c * a - s * b;
					vq = s * a + c * b;
				}
			}
		}

		if (!changed)
			break;
	}

	double largest = 0.0;
	for (int i = 0; i < parameters; ++i)
	{
		double sum = 0.0;
		for (int k = 0; k < n; ++k)
		{
			const double w = m_work[k * parameters + i];
			sum += w * w;
		}
		m_sigma[i] = std::sqrt(sum);
		if (m_sigma[i] > largest)
			largest = m_sigma[i];
	}

	m_singularMax = largest;
	const double cutoff = largest * 1.0e-12;
	int rank = 0;
	if (largest > 0.0)
	{
		for (int i = 0; i < parameters; ++i)
			if (m_sigma[i] > cutoff)
				++rank;
	}
	m_rank = rank;
}

//------------------------------------------------------------------------------

void ZeroPhasePlan::process(const double* const* input,
                            double* const* output,
                            std::size_t channels)
{
	if (channels < 1 || channels > m_maxChannels)
		throw_invalid_argument(errChannelRange);
	if (input == nullptr || output == nullptr)
		throw_invalid_argument(errPlanes);

	const std::size_t n = m_length;
	const int stateSize = 2 * m_stages;
	const int parameters = 2 * stateSize;
	const double rankCutoff = m_singularMax * 1.0e-12;

	for (std::size_t ch = 0; ch < channels; ++ch)
	{
		if (input[ch] == nullptr || output[ch] == nullptr)
			throw_invalid_argument(errPlanes);

		const double* in = input[ch];
		double* out = output[ch];

		// Copy the segment first so that out may be identical to in.
		for (std::size_t k = 0; k < n; ++k)
			m_x[k] = in[k];

		// B0(F0(x)) = R F0(R u); m_b0 holds F0(R u), the un-reversed
		// form, which is B0(F0(x)) read from the tail.
		clearState(m_state.data(), stateSize);
		runSegment(m_stage, m_stages, m_x.data(), m_yf.data(), n, m_state.data());
		for (std::size_t k = 0; k < n; ++k)
			m_res[k] = m_yf[n - 1 - k];
		clearState(m_state.data(), stateSize);
		runSegment(m_stage, m_stages, m_res.data(), m_b0.data(), n, m_state.data());

		// B0(x) = R F0(R x).  m_b1 receives F0(R x); reversing it into
		// m_res gives B0(x) in the index order used below.
		for (std::size_t k = 0; k < n; ++k)
			m_res[k] = m_x[n - 1 - k];
		clearState(m_state.data(), stateSize);
		runSegment(m_stage, m_stages, m_res.data(), m_b1.data(), n, m_state.data());
		for (std::size_t k = 0; k < n; ++k)
			m_res[k] = m_b1[n - 1 - k];

		// c = B0(F0(x)) - F0(B0(x)).
		clearState(m_state.data(), stateSize);
		runSegment(m_stage, m_stages, m_res.data(), m_b2.data(), n, m_state.data());
		for (std::size_t k = 0; k < n; ++k)
			m_c[k] = m_b0[n - 1 - k] - m_b2[k];

		// Minimum norm least squares solution of A z + c = 0.
		for (int i = 0; i < parameters; ++i)
			m_z[i] = 0.0;

		if (m_singularMax > 0.0)
		{
			for (int i = 0; i < parameters; ++i)
			{
				if (!(m_sigma[i] > rankCutoff))
					continue;

				double dot = 0.0;
				for (std::size_t k = 0; k < n; ++k)
					dot += m_work[k * parameters + i] * m_c[k];
				const double coefficient = -dot / (m_sigma[i] * m_sigma[i]);

				for (int j = 0; j < parameters; ++j)
					m_z[j] += coefficient * m_v[j * parameters + i];
			}
		}

		const double* forwardState = m_z.data();
		const double* reverseState = m_z.data() + stateSize;

		// First ordering: B_t(F_s(x)) = R F_t(R F_s(x)).
		// F_s starts from the solved forward state s.
		for (int i = 0; i < stateSize; ++i)
			m_state[i] = forwardState[i];
		runSegment(m_stage, m_stages, m_x.data(), m_yf.data(), n, m_state.data());
		for (std::size_t k = 0; k < n; ++k)
			m_res[k] = m_yf[n - 1 - k];
		// F_t starts from the solved backward state t.
		for (int i = 0; i < stateSize; ++i)
			m_state[i] = reverseState[i];
		runSegment(m_stage, m_stages, m_res.data(), m_yf.data(), n, m_state.data());
		for (std::size_t k = 0; k < n; ++k)
			out[k] = m_yf[n - 1 - k];

		// Second ordering: F_s(B_t(x)) = F_s(R F_t(R x)).
		// Running the backward pass over x in reverse index order is
		// exactly R F_t(R x), so m_b0 needs no separate reversal.
		for (int i = 0; i < stateSize; ++i)
			m_state[i] = reverseState[i];
		runSegmentBackward(m_stage, m_stages, m_x.data(), m_b0.data(), n,
		                   m_state.data());
		for (int i = 0; i < stateSize; ++i)
			m_state[i] = forwardState[i];
		runSegment(m_stage, m_stages, m_b0.data(), m_b2.data(), n, m_state.data());

		double squared = 0.0;
		for (std::size_t k = 0; k < n; ++k)
		{
			const double difference = out[k] - m_b2[k];
			squared += difference * difference;
		}

		// Publish the states and residual which were just used.
		for (int i = 0; i < stateSize; ++i)
		{
			m_diagnostics.forwardStates[ch][i] = m_z[i];
			m_diagnostics.reverseStates[ch][i] = m_z[stateSize + i];
		}
		m_diagnostics.boundaryResiduals[ch] = std::sqrt(squared);
	}

	m_diagnostics.channels = channels;
	m_diagnostics.stages = m_stages;
	m_diagnostics.rank = m_rank;
	m_diagnostics.decompositions = m_decompositions;
	m_diagnostics.singularMax = m_singularMax;
}

//------------------------------------------------------------------------------

const std::vector<double>& ZeroPhasePlan::getForwardState(std::size_t channel) const
{
	if (channel >= m_diagnostics.channels)
		throw_invalid_argument(errStaleChannel);
	return m_diagnostics.forwardStates[channel];
}

const std::vector<double>& ZeroPhasePlan::getReverseState(std::size_t channel) const
{
	if (channel >= m_diagnostics.channels)
		throw_invalid_argument(errStaleChannel);
	return m_diagnostics.reverseStates[channel];
}

double ZeroPhasePlan::getBoundaryResidual(std::size_t channel) const
{
	if (channel >= m_diagnostics.channels)
		throw_invalid_argument(errStaleChannel);
	return m_diagnostics.boundaryResiduals[channel];
}

}
