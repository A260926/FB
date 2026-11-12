////////////////////////////////////////////////////////////////////////////////
/// 
/// Sample rate transposer. Changes sample rate by using linear interpolation 
/// together with anti-alias filtering (first order interpolation with anti-
/// alias filtering should be quite adequate for this application)
///
/// Author        : Copyright (c) Olli Parviainen
/// Author e-mail : oparviai 'at' iki.fi
/// SoundTouch WWW: http://www.surina.net/soundtouch
///
////////////////////////////////////////////////////////////////////////////////
//
// Last changed  : $Date: 2014-01-06 14:19:38 -0500 (Mon, 06 Jan 2014) $
// File revision : $Revision: 4 $
//
// $Id: RateTransposer.cpp 181 2014-01-06 19:19:38Z oparviai $
//
////////////////////////////////////////////////////////////////////////////////
//
// License :
//
//  SoundTouch audio processing library
//  Copyright (c) Olli Parviainen
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
////////////////////////////////////////////////////////////////////////////////

#include <memory.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stdexcept>
#include "RateTransposer.h"
#include "InterpolateLinear.h"
#include "InterpolateCubic.h"
#include "InterpolateShannon.h"
#include "AAFilter.h"

using namespace soundtouch;

// Define default interpolation algorithm here
TransposerBase::ALGORITHM TransposerBase::algorithm = TransposerBase::CUBIC;

#define RAMP_PI 3.14159265358979323846


// Constructor
RateTransposer::RateTransposer() : FIFOProcessor(&outputBuffer)
{
    bUseAAFilter = TRUE;

    // Instantiates the anti-alias filter
    pAAFilter = new AAFilter(64);
    pTransposer = TransposerBase::newInstance();

    // Rate ramp mode starts disabled; trajectory state is initialized so
    // that enabling the mode before any input starts from source coordinate
    // zero with the current (unity) rate.
    bRateRamp = FALSE;
    rampInputSeen = FALSE;
    rampSrcPos = 0.0;
    rampCurRate = 1.0;
    rampR0 = 1.0;
    rampRt = 1.0;
    rampTotal = 0;
    rampDone = 0;
    rampHistBase = 0;
}



RateTransposer::~RateTransposer()
{
    delete pAAFilter;
    delete pTransposer;
}



/// Enables/disables the anti-alias filter. Zero to disable, nonzero to enable
void RateTransposer::enableAAFilter(BOOL newMode)
{
    bUseAAFilter = newMode;
}


/// Returns nonzero if anti-alias filter is enabled.
BOOL RateTransposer::isAAFilterEnabled() const
{
    return bUseAAFilter;
}


AAFilter *RateTransposer::getAAFilter()
{
    return pAAFilter;
}



// Sets new target iRate. Normal iRate = 1.0, smaller values represent slower
// iRate, larger faster iRates.
void RateTransposer::setRate(float newRate)
{
    if (bRateRamp)
    {
        // Ramp mode: set the increment of the next output frame immediately
        // and cancel any unfinished ramp. The anti-alias filtering follows
        // the per-frame increment automatically, no filter redesign here.
        checkRampRate(newRate);
        rampCurRate = newRate;
        rampTotal = 0;
        rampDone = 0;
        return;
    }
    setRateLegacy(newRate);
}


// Legacy constant-rate setRate: stores the rate and redesigns the anti-alias
// filter for it.
void RateTransposer::setRateLegacy(float newRate)
{
    double fCutoff;

    pTransposer->setRate(newRate);

    // design a new anti-alias filter
    if (newRate > 1.0f)
    {
        fCutoff = 0.5f / newRate;
    }
    else
    {
        fCutoff = 0.5f * newRate;
    }
    pAAFilter->setCutoffFreq(fCutoff);
}


// Checks that a rate is finite and inside the [0.5, 2.0] range supported in
// ramp mode; throws std::invalid_argument otherwise.
void RateTransposer::checkRampRate(double rate)
{
    if (!(rate >= rampMinRate() && rate <= rampMaxRate()))
    {
        // note: the comparison above also rejects NaN
        throw std::invalid_argument("RateTransposer: rate outside [0.5, 2.0] in ramp mode");
    }
}


// Enables/disables the rate ramp mode. Only allowed before any non-empty
// input has been fed since construction or the last clear().
void RateTransposer::enableRateRamp(BOOL enabled)
{
    BOOL newMode = (enabled != 0) ? TRUE : FALSE;

    if (newMode == bRateRamp) return;   // no mode switch, nothing to do

    if (rampInputSeen)
    {
        throw std::invalid_argument(
            "RateTransposer: rate ramp mode can only be toggled before input is fed");
    }

    if (newMode)
    {
#ifdef SOUNDTOUCH_INTEGER_SAMPLES
        throw std::invalid_argument(
            "RateTransposer: rate ramp mode requires floating point samples");
#else
        if (TransposerBase::getAlgorithm() == TransposerBase::SHANNON)
        {
            throw std::invalid_argument(
                "RateTransposer: rate ramp mode is not supported with SHANNON interpolation");
        }
#endif
        // the current rate becomes the initial increment; it must be within
        // the ramp mode limits. The rate itself is not changed by the switch.
        checkRampRate(pTransposer->rate);

        rampSrcPos = 0.0;
        rampCurRate = pTransposer->rate;
        rampR0 = rampCurRate;
        rampRt = rampCurRate;
        rampTotal = 0;
        rampDone = 0;
        rampHistBase = 0;
        bRateRamp = TRUE;
    }
    else
    {
        // switch back to legacy mode, keeping the current increment as the
        // constant rate.
        bRateRamp = FALSE;
        setRateLegacy((float)rampCurRate);
    }
}


// Returns nonzero if rate ramp mode is enabled.
BOOL RateTransposer::isRateRampEnabled() const
{
    return bRateRamp;
}


// Schedules a rate ramp starting from the next not-yet-generated output frame.
void RateTransposer::setRateRamp(double targetRate, uint outputFrames)
{
    if (!bRateRamp)
    {
        throw std::invalid_argument(
            "RateTransposer: setRateRamp requires rate ramp mode to be enabled");
    }
    checkRampRate(targetRate);

    if (outputFrames == 0)
    {
        // zero-length ramp: set the next increment immediately
        rampCurRate = targetRate;
        rampTotal = 0;
        rampDone = 0;
        return;
    }

    // continue from the current increment, replacing any unfinished ramp
    rampR0 = getCurrentRate();
    rampRt = targetRate;
    rampTotal = outputFrames;
    rampDone = 0;
}


// Returns the source coordinate increment that the next generated output
// frame will use.
double RateTransposer::getCurrentRate() const
{
    if (bRateRamp)
    {
        if (rampDone < rampTotal)
        {
            return rampR0 + (rampRt - rampR0) * (double)rampDone / (double)rampTotal;
        }
        return rampCurRate;
    }
    return pTransposer->rate;
}


// Returns how many output frames of the active ramp are still ungenerated.
uint RateTransposer::getRateRampRemaining() const
{
    if (!bRateRamp || rampDone >= rampTotal) return 0;
    return rampTotal - rampDone;
}


// Adds 'nSamples' pcs of samples from the 'samples' memory position into
// the input of the object.
void RateTransposer::putSamples(const SAMPLETYPE *samples, uint nSamples)
{
    if (nSamples == 0)
    {
        // zero-length input never advances the rate trajectory
        return;
    }
    rampInputSeen = TRUE;
    if (bRateRamp)
    {
        processSamplesRamp(samples, nSamples);
        return;
    }
    processSamples(samples, nSamples);
}


// Ramp mode processing.
//
// The output frame k uses source coordinate x_k (double precision, x_0 = 0
// after construction/clear) and increment r_k; afterwards x_{k+1} = x_k + r_k.
// Frames are generated only while the required input history is available:
//  - interpolation needs samples up to floor(x) + 1 (linear) or +2 (cubic),
//  - anti-alias filtering (active when the frame's increment is > 1) centers
//    a 64-tap Hamming-windowed sinc kernel on the exact source coordinate x
//    and needs samples up to floor(x) + 32 (a lookahead of at most 32 input
//    frames). Input frames with negative coordinate are treated as zero.
// When input runs out the trajectory simply stops; it advances only for
// frames that actually enter the output FIFO.
void RateTransposer::processSamplesRamp(const SAMPLETYPE *src, uint numSamples)
{
    const int nch = pTransposer->numChannels;
    const BOOL cubic = (TransposerBase::getAlgorithm() == TransposerBase::CUBIC) ? TRUE : FALSE;

    // number of output frames generated per inner batch
    enum { RAMP_CHUNK = 256 };

    inputBuffer.putSamples(src, numSamples);

    if (nch <= 0) return;   // channel count not configured yet

    // Local copies of the trajectory state; committed back only for frames
    // that have actually been pushed into the output FIFO.
    double x = rampSrcPos;
    double curRate = rampCurRate;
    double r0 = rampR0;
    double rt = rampRt;
    uint total = rampTotal;
    uint done = rampDone;

    for (;;)
    {
        SAMPLETYPE *dest = outputBuffer.ptrEnd(RAMP_CHUNK);
        uint produced = 0;

        while (produced < RAMP_CHUNK)
        {
            // increment for this frame
            double r = (done < total) ? r0 + (rt - r0) * (double)done / (double)total
                                      : curRate;

            double iFloor = floor(x);
            long long i0 = (long long)iFloor;
            BOOL filtered = (bUseAAFilter && r > 1.0) ? TRUE : FALSE;

            // highest input frame index needed for this output frame
            long long needLast = filtered ? (i0 + RAMP_FILTER_HALFLEN)
                                          : (cubic ? i0 + 2 : i0 + 1);
            if (rampHistBase + (long long)inputBuffer.numSamples() <= needLast)
            {
                break;  // input starved: trajectory halts here
            }

            const SAMPLETYPE *history = inputBuffer.ptrBegin();
            double f = x - iFloor;

            if (filtered)
            {
                // Anti-alias low-pass aligned to the output coordinate: a
                // Hamming-windowed sinc in input-frame units, centered on the
                // exact source coordinate x, cutoff 0.5/r (Nyquist = 0.5).
                // Coefficients are normalized per frame so DC gain is 1.
                double wc = RAMP_PI / r;    // 2*pi*cutoff
                double coeffs[2 * RAMP_FILTER_HALFLEN];
                double sum = 0.0;
                int t, c;

                for (t = 0; t < 2 * RAMP_FILTER_HALFLEN; t ++)
                {
                    long long j = i0 - (RAMP_FILTER_HALFLEN - 1) + t;
                    double d = (double)j - x;
                    double h = (d == 0.0) ? 1.0 : sin(d * wc) / (d * wc);
                    double w = 0.54 + 0.46 * cos(RAMP_PI * d / (double)RAMP_FILTER_HALFLEN);
                    coeffs[t] = h * w;
                    sum += coeffs[t];
                }
                assert(sum > 0.0);

                for (c = 0; c < nch; c ++)
                {
                    double acc = 0.0;
                    for (t = 0; t < 2 * RAMP_FILTER_HALFLEN; t ++)
                    {
                        long long j = i0 - (RAMP_FILTER_HALFLEN - 1) + t;
                        if (j >= 0)
                        {
                            acc += coeffs[t] * (double)history[(j - rampHistBase) * nch + c];
                        }
                        // history before source coordinate zero is zero
                    }
                    dest[produced * nch + c] = (SAMPLETYPE)(acc / sum);
                }
            }
            else if (cubic)
            {
                // Catmull-Rom cubic on points i0-1 .. i0+2, fraction f
                double f2 = f * f;
                double f3 = f2 * f;
                double y0 = -0.5 * f3 +       f2 - 0.5 * f;
                double y1 =  1.5 * f3 - 2.5 * f2           + 1.0;
                double y2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
                double y3 =  0.5 * f3 - 0.5 * f2;
                int c;

                for (c = 0; c < nch; c ++)
                {
                    double s0 = (i0 - 1 >= 0) ? (double)history[(i0 - 1 - rampHistBase) * nch + c] : 0.0;
                    double s1 = (double)history[(i0     - rampHistBase) * nch + c];
                    double s2 = (double)history[(i0 + 1 - rampHistBase) * nch + c];
                    double s3 = (double)history[(i0 + 2 - rampHistBase) * nch + c];
                    dest[produced * nch + c] = (SAMPLETYPE)(y0 * s0 + y1 * s1 + y2 * s2 + y3 * s3);
                }
            }
            else
            {
                // plain linear interpolation
                int c;

                for (c = 0; c < nch; c ++)
                {
                    double s1 = (double)history[(i0     - rampHistBase) * nch + c];
                    double s2 = (double)history[(i0 + 1 - rampHistBase) * nch + c];
                    dest[produced * nch + c] = (SAMPLETYPE)((1.0 - f) * s1 + f * s2);
                }
            }

            // frame generated: advance the trajectory
            x += r;
            if (done < total)
            {
                done ++;
                if (done == total)
                {
                    curRate = rt;   // ramp completed, continue at target rate
                }
            }
            produced ++;
        }

        // move the produced frames into the output FIFO ...
        outputBuffer.putSamples(produced);

        // ... and commit the trajectory only for frames that really entered it
        rampSrcPos = x;
        rampCurRate = curRate;
        rampR0 = r0;
        rampRt = rt;
        rampTotal = total;
        rampDone = done;

        if (produced < RAMP_CHUNK) break;
    }

    // discard input history that can never be needed again: the source
    // coordinate never decreases, and the filter kernel reaches at most
    // RAMP_FILTER_HALFLEN-1 frames below floor(x)
    {
        long long drop = (long long)floor(rampSrcPos) - (RAMP_FILTER_HALFLEN - 1) - rampHistBase;
        if (drop > 0)
        {
            inputBuffer.receiveSamples((uint)drop);
            rampHistBase += drop;
        }
    }
}


// Transposes sample rate by applying anti-alias filter to prevent folding. 
// Returns amount of samples returned in the "dest" buffer.
// The maximum amount of samples that can be returned at a time is set by
// the 'set_returnBuffer_size' function.
void RateTransposer::processSamples(const SAMPLETYPE *src, uint nSamples)
{
    uint count;

    if (nSamples == 0) return;

    // Store samples to input buffer
    inputBuffer.putSamples(src, nSamples);

    // If anti-alias filter is turned off, simply transpose without applying
    // the filter
    if (bUseAAFilter == FALSE) 
    {
        count = pTransposer->transpose(outputBuffer, inputBuffer);
        return;
    }

    assert(pAAFilter);

    // Transpose with anti-alias filter
    if (pTransposer->rate < 1.0f) 
    {
        // If the parameter 'Rate' value is smaller than 1, first transpose
        // the samples and then apply the anti-alias filter to remove aliasing.

        // Transpose the samples, store the result to end of "midBuffer"
        pTransposer->transpose(midBuffer, inputBuffer);

        // Apply the anti-alias filter for transposed samples in midBuffer
        pAAFilter->evaluate(outputBuffer, midBuffer);
    } 
    else  
    {
        // If the parameter 'Rate' value is larger than 1, first apply the
        // anti-alias filter to remove high frequencies (prevent them from folding
        // over the lover frequencies), then transpose.

        // Apply the anti-alias filter for samples in inputBuffer
        pAAFilter->evaluate(midBuffer, inputBuffer);

        // Transpose the AA-filtered samples in "midBuffer"
        pTransposer->transpose(outputBuffer, midBuffer);
    }
}


// Sets the number of channels, 1 = mono, 2 = stereo
void RateTransposer::setChannels(int nChannels)
{
    assert(nChannels > 0);

    if (pTransposer->numChannels == nChannels) return;
    pTransposer->setChannels(nChannels);

    inputBuffer.setChannels(nChannels);
    midBuffer.setChannels(nChannels);
    outputBuffer.setChannels(nChannels);
}


// Clears all the samples in the object
void RateTransposer::clear()
{
    outputBuffer.clear();
    midBuffer.clear();
    inputBuffer.clear();

    // Ramp mode: drop the sound and the fractional phase and cancel the
    // ramp, but keep the current increment and the mode itself. The next
    // segment starts again from source coordinate zero.
    if (bRateRamp)
    {
        // fold the increment the next frame would have used into the flat
        // rate before cancelling the ramp
        rampCurRate = getCurrentRate();
    }
    rampSrcPos = 0.0;
    rampHistBase = 0;
    rampTotal = 0;
    rampDone = 0;
    rampInputSeen = FALSE;
}


// Returns nonzero if there aren't any samples available for outputting.
int RateTransposer::isEmpty() const
{
    int res;

    res = FIFOProcessor::isEmpty();
    if (res == 0) return 0;
    return inputBuffer.isEmpty();
}


//////////////////////////////////////////////////////////////////////////////
//
// TransposerBase - Base class for interpolation
// 

// static function to set interpolation algorithm
void TransposerBase::setAlgorithm(TransposerBase::ALGORITHM a)
{
    TransposerBase::algorithm = a;
}


// static function to query interpolation algorithm
TransposerBase::ALGORITHM TransposerBase::getAlgorithm()
{
    return TransposerBase::algorithm;
}


// Transposes the sample rate of the given samples using linear interpolation. 
// Returns the number of samples returned in the "dest" buffer
int TransposerBase::transpose(FIFOSampleBuffer &dest, FIFOSampleBuffer &src)
{
    int numSrcSamples = src.numSamples();
    int sizeDemand = (int)((float)numSrcSamples / rate) + 8;
    int numOutput;
    SAMPLETYPE *psrc = src.ptrBegin();
    SAMPLETYPE *pdest = dest.ptrEnd(sizeDemand);

#ifndef USE_MULTICH_ALWAYS
    if (numChannels == 1)
    {
        numOutput = transposeMono(pdest, psrc, numSrcSamples);
    }
    else if (numChannels == 2) 
    {
        numOutput = transposeStereo(pdest, psrc, numSrcSamples);
    } 
    else 
#endif // USE_MULTICH_ALWAYS
    {
        assert(numChannels > 0);
        numOutput = transposeMulti(pdest, psrc, numSrcSamples);
    }
    dest.putSamples(numOutput);
    src.receiveSamples(numSrcSamples);
    return numOutput;
}


TransposerBase::TransposerBase()
{
    numChannels = 0;
    rate = 1.0f;
}


TransposerBase::~TransposerBase()
{
}


void TransposerBase::setChannels(int channels)
{
    numChannels = channels;
    resetRegisters();
}


void TransposerBase::setRate(float newRate)
{
    rate = newRate;
}


// static factory function
TransposerBase *TransposerBase::newInstance()
{
#ifdef SOUNDTOUCH_INTEGER_SAMPLES
    // Notice: For integer arithmetics support only linear algorithm (due to simplest calculus)
    return ::new InterpolateLinearInteger;
#else
    switch (algorithm)
    {
        case LINEAR:
            return new InterpolateLinearFloat;

        case CUBIC:
            return new InterpolateCubic;

        case SHANNON:
            return new InterpolateShannon;

        default:
            assert(false);
            return NULL;
    }
#endif
}
