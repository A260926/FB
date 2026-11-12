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

#pragma once

#include "Common.h"
#include "Biquad.h"
#include "Cascade.h"

namespace Iir
{

/**
 * Zero-phase processing of a finite segment with a fixed cascade.
 *
 * The forward pass F_s starts at the first sample with the two
 * transposed direct form II delay values of every stage held in the
 * state s and processes towards the last sample.  The backward pass is
 * R F_t R, where R reverses the sample order.  Both passes use the
 * same section order.
 *
 * For a fixed segment x the plan finds the forward state s and the
 * backward state t which minimise the two norm of
 *
 *     B_t(F_s(x)) - F_s(B_t(x)),  B_t(y) = R F_t(R y).
 *
 * The output is the first ordering B_t(F_s(x)).  If several state
 * pairs attain the minimum, the one with the smallest Euclidean norm
 * of the concatenated vector [s,t] is used.  The effective rank of the
 * boundary system (length rows, 4*stages columns) uses a singular
 * value threshold of 1e-12 relative to the largest singular value; a
 * zero boundary matrix has rank zero and a zero state.
 *
 * The plan owns a copy of the coefficients.  It never reads, borrows or
 * resets the delay lines of the Cascade the storage came from.  All
 * coefficient dependent work, including the singular value
 * decomposition of the boundary system, happens once in the
 * constructor and serves every process() call and every channel.
 *
 * process() does no heap allocation or deallocation.  The input and
 * output planes of one channel may be identical; planes of different
 * channels must not overlap.  A plan object is not used concurrently.
 **/
class IIR_EXPORT ZeroPhasePlan
{
public:
    /// The largest number of sections supported by this class.
    enum { maxSupportedStages = 6 };

    /**
     * Diagnostic values of the most recent process() call.
     *
     * forwardStates and reverseStates hold, per channel, the 2*stages
     * delay values which were actually used to produce that channel's
     * samples.  boundaryResiduals holds the two norm of the remaining
     * boundary difference after the minimisation.  rank is the
     * effective rank of the boundary system.  decompositions counts the
     * coefficient-dependent factorisations of this plan; it is one
     * because the constructor performs the only one.
     **/
    struct IIR_EXPORT Diagnostics
    {
        Diagnostics() = default;

        /// Number of sections actually used (1..6).
        int stages = 0;
        /// Effective rank of the boundary system (0..4*stages).
        int rank = 0;
        /// Number of channels described by this snapshot.
        std::size_t channels = 0;
        /// Number of coefficient-dependent factorisations done by this plan.
        unsigned long long decompositions = 0;
        /// Largest singular value of the boundary system.
        double singularMax = 0.0;
        /// Forward delay states, one entry per channel.
        std::vector< std::vector<double> > forwardStates;
        /// Backward delay states, one entry per channel.
        std::vector< std::vector<double> > reverseStates;
        /// Boundary residuals (two norm), one entry per channel.
        std::vector<double> boundaryResiduals;
    };

    /**
     * Prepares a plan for segments of exactly length samples.
     *
     * \param storage      Coefficient storage.  Only the first
     *                     stages sections are copied; the remaining
     *                     capacity is never read or counted.
     * \param stages       Number of sections to use, 1..6.
     * \param length       Fixed segment length in samples, >= 1.
     * \param maxChannels  Maximum number of planar channels accepted by
     *                     process(), >= 1.
     *
     * Invalid lengths, non-finite coefficients, an unusable storage
     * pointer and sections whose poles are not strictly inside the
     * unit circle are rejected with std::invalid_argument.  Identity,
     * pure gain and first order sections without feedback are allowed.
     **/
    ZeroPhasePlan(const Cascade::Storage& storage,
                  int stages,
                  std::size_t length,
                  std::size_t maxChannels);

    ZeroPhasePlan() = delete;

    /**
     * Processes channels planar segments in place or out of place.
     *
     * \param input    Array of channel pointers, each to length samples.
     * \param output   Array of channel pointers, each to length samples.
     * \param channels Number of channels, 1..maxChannels.
     *
     * Zero channels, more channels than the configured capacity and
     * null plane pointers are rejected with std::invalid_argument.
     * This call performs no heap allocation or deallocation.
     **/
    void process(const double* const* input,
                 double* const* output,
                 std::size_t channels);

    /// Diagnostics of the most recent process() call.
    const Diagnostics& diagnostics() const
    {
        return m_diagnostics;
    }

    /// Number of sections in use.
    int getStages() const
    {
        return m_stages;
    }

    /// Fixed segment length.
    std::size_t getLength() const
    {
        return m_length;
    }

    /// Configured channel capacity.
    std::size_t getMaxChannels() const
    {
        return m_maxChannels;
    }

    /// Effective rank of the boundary system.
    int getRank() const
    {
        return m_rank;
    }

    /// Number of coefficient-dependent factorisations performed.
    unsigned long long getDecompositionCount() const
    {
        return m_decompositions;
    }

    /// State of the forward pass used for a channel of the last call.
    const std::vector<double>& getForwardState(std::size_t channel) const;

    /// State of the backward pass used for a channel of the last call.
    const std::vector<double>& getReverseState(std::size_t channel) const;

    /// Boundary residual of a channel of the last call.
    double getBoundaryResidual(std::size_t channel) const;

private:
    void buildBoundarySystem();
    void decompose();

    Biquad m_stage[maxSupportedStages];
    int m_stages = 0;
    std::size_t m_length = 0;
    std::size_t m_maxChannels = 0;

    // Boundary system A (length x 4*stages), row major.  The one-sided
    // Jacobi decomposition in the constructor rotates it into W (kept
    // in m_work) and produces the right singular vectors in m_v.
    std::vector<double> m_work;
    std::vector<double> m_v;
    std::vector<double> m_sigma;
    double m_singularMax = 0.0;
    int m_rank = 0;
    unsigned long long m_decompositions = 0;

    // Workspaces, sized in the constructor and reused by every channel.
    // process() never resizes any of them.
    std::vector<double> m_zero;
    std::vector<double> m_x;
    std::vector<double> m_b0;
    std::vector<double> m_b1;
    std::vector<double> m_b2;
    std::vector<double> m_c;
    std::vector<double> m_res;
    std::vector<double> m_yf;
    std::vector<double> m_state;
    std::vector<double> m_z;

    Diagnostics m_diagnostics;
};

}
