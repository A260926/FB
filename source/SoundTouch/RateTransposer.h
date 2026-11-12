////////////////////////////////////////////////////////////////////////////////
/// 
/// Sample rate transposer. Changes sample rate by using linear interpolation 
/// together with anti-alias filtering (first order interpolation with anti-
/// alias filtering should be quite adequate for this application).
///
/// Use either of the derived classes of 'RateTransposerInteger' or 
/// 'RateTransposerFloat' for corresponding integer/floating point tranposing
/// algorithm implementation.
///
/// Author        : Copyright (c) Olli Parviainen
/// Author e-mail : oparviai 'at' iki.fi
/// SoundTouch WWW: http://www.surina.net/soundtouch
///
////////////////////////////////////////////////////////////////////////////////
//
// Last changed  : $Date: 2014-01-06 13:40:23 -0500 (Mon, 06 Jan 2014) $
// File revision : $Revision: 4 $
//
// $Id: RateTransposer.h 178 2014-01-06 18:40:23Z oparviai $
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

#ifndef RateTransposer_H
#define RateTransposer_H

#include <stddef.h>
#include "AAFilter.h"
#include "FIFOSamplePipe.h"
#include "FIFOSampleBuffer.h"

#include "STTypes.h"

namespace soundtouch
{

/// Abstract base class for transposer implementations (linear, advanced vs integer, float etc)
class TransposerBase
{
public:
        enum ALGORITHM {
        LINEAR = 0,
        CUBIC,
        SHANNON
    };

protected:
    virtual void resetRegisters() = 0;

    virtual int transposeMono(SAMPLETYPE *dest, 
                        const SAMPLETYPE *src, 
                        int &srcSamples)  = 0;
    virtual int transposeStereo(SAMPLETYPE *dest, 
                        const SAMPLETYPE *src, 
                        int &srcSamples) = 0;
    virtual int transposeMulti(SAMPLETYPE *dest, 
                        const SAMPLETYPE *src, 
                        int &srcSamples) = 0;

    static ALGORITHM algorithm;

public:
    float rate;
    int numChannels;

    TransposerBase();
    virtual ~TransposerBase();

    virtual int transpose(FIFOSampleBuffer &dest, FIFOSampleBuffer &src);
    virtual void setRate(float newRate);
    virtual void setChannels(int channels);

    // static factory function
    static TransposerBase *newInstance();

    // static function to set interpolation algorithm
    static void setAlgorithm(ALGORITHM a);

    // static function to query the current interpolation algorithm
    static ALGORITHM getAlgorithm();
};


/// A common linear samplerate transposer class.
///
class RateTransposer : public FIFOProcessor
{
protected:
    /// Anti-alias filter object
    AAFilter *pAAFilter;
    TransposerBase *pTransposer;

    /// Buffer for collecting samples to feed the anti-alias filter between
    /// two batches
    FIFOSampleBuffer inputBuffer;

    /// Buffer for keeping samples between transposing & anti-alias filter
    FIFOSampleBuffer midBuffer;

    /// Output sample buffer
    FIFOSampleBuffer outputBuffer;

    BOOL bUseAAFilter;

    /// Rate ramp mode state. In ramp mode the transposer walks a continuous
    /// source coordinate (in input frames, double precision) that starts at 0
    /// after construction/clear and is advanced by the per-output-frame
    /// increment. All channels share the same coordinate.
    BOOL bRateRamp;

    /// Nonzero once a non-empty input batch has been fed since construction
    /// or the last clear(). The ramp mode can only be toggled while zero.
    BOOL rampInputSeen;

    /// Source coordinate of the next output frame to generate.
    double rampSrcPos;

    /// Source coordinate increment used for the next output frame when no
    /// ramp is active (and the value the active ramp was started from).
    double rampCurRate;

    /// Active ramp: increment for frame k (0-based) is
    /// rampR0 + (rampRt - rampR0) * k / rampTotal for k < rampTotal,
    /// rampRt afterwards.
    double rampR0;
    double rampRt;
    uint rampTotal;
    uint rampDone;

    /// Source coordinate (input frame index) of the first frame currently in
    /// 'inputBuffer'. In ramp mode inputBuffer holds the still-needed input
    /// history; frames older than floor(rampSrcPos) - 31 are discarded.
    long long rampHistBase;

    /// Rate limits accepted in ramp mode
    enum { RAMP_FILTER_HALFLEN = 32 };
    static double rampMinRate() { return 0.5; }
    static double rampMaxRate() { return 2.0; }

    /// Throws std::invalid_argument if the rate is not finite or outside
    /// the [0.5, 2.0] range supported in ramp mode.
    static void checkRampRate(double rate);

    /// Ramp-mode processing: generates as many output frames as the buffered
    /// input history allows, advancing the rate trajectory only for frames
    /// that actually enter the output FIFO.
    void processSamplesRamp(const SAMPLETYPE *src,
                            uint numSamples);

    /// Legacy constant-rate setRate body (also updates AA filter cutoff).
    void setRateLegacy(float newRate);


    /// Transposes sample rate by applying anti-alias filter to prevent folding.
    /// Returns amount of samples returned in the "dest" buffer.
    /// The maximum amount of samples that can be returned at a time is set by
    /// the 'set_returnBuffer_size' function.
    void processSamples(const SAMPLETYPE *src,
                        uint numSamples);

public:
    RateTransposer();
    virtual ~RateTransposer();

    /// Operator 'new' is overloaded so that it automatically creates a suitable instance 
    /// depending on if we're to use integer or floating point arithmetics.
//    static void *operator new(size_t s);

    /// Use this function instead of "new" operator to create a new instance of this class. 
    /// This function automatically chooses a correct implementation, depending on if 
    /// integer ot floating point arithmetics are to be used.
//    static RateTransposer *newInstance();

    /// Returns the output buffer object
    FIFOSamplePipe *getOutput() { return &outputBuffer; };

    /// Returns the store buffer object
//    FIFOSamplePipe *getStore() { return &storeBuffer; };

    /// Return anti-alias filter object
    AAFilter *getAAFilter();

    /// Enables/disables the anti-alias filter. Zero to disable, nonzero to enable
    void enableAAFilter(BOOL newMode);

    /// Returns nonzero if anti-alias filter is enabled.
    BOOL isAAFilterEnabled() const;

    /// Sets new target rate. Normal rate = 1.0, smaller values represent slower
    /// rate, larger faster rates.
    ///
    /// In rate ramp mode this sets the source coordinate increment of the next
    /// output frame immediately and cancels any active ramp; the new rate must
    /// be finite and within [0.5, 2.0] or std::invalid_argument is thrown and
    /// the object state is left untouched.
    virtual void setRate(float newRate);

    /// Enables/disables the output-frame clocked rate ramp mode. The mode can
    /// only be switched right after construction or clear(), before any
    /// non-empty input has been fed; switching does not change the current
    /// rate. Throws std::invalid_argument if the interpolation algorithm does
    /// not support ramping (SHANNON, or integer sample builds), if the current
    /// rate is outside [0.5, 2.0], or if input has already been fed.
    void enableRateRamp(BOOL enabled);

    /// Returns nonzero if rate ramp mode is enabled.
    BOOL isRateRampEnabled() const;

    /// Schedules a rate ramp starting from the next not-yet-generated output
    /// frame: the source coordinate increment of frame k (0-based) is
    /// getCurrentRate() + (targetRate - getCurrentRate()) * k / outputFrames
    /// for k < outputFrames, and targetRate afterwards. Sound already queued
    /// in the output FIFO is not affected. A repeated call continues from the
    /// current increment, replacing the unfinished ramp. outputFrames == 0
    /// sets the next increment immediately and cancels the ramp.
    /// Throws std::invalid_argument if ramp mode is off or targetRate is not
    /// finite and within [0.5, 2.0]; the object state is then left untouched.
    void setRateRamp(double targetRate, uint outputFrames);

    /// Returns the source coordinate increment that will be used for the next
    /// output frame that gets generated into the output FIFO.
    double getCurrentRate() const;

    /// Returns how many output frames of the active ramp remain to be
    /// generated into the output FIFO; zero when no ramp is active.
    uint getRateRampRemaining() const;

    /// Sets the number of channels, 1 = mono, 2 = stereo
    void setChannels(int channels);

    /// Adds 'numSamples' pcs of samples from the 'samples' memory position into
    /// the input of the object.
    void putSamples(const SAMPLETYPE *samples, uint numSamples);

    /// Clears all the samples in the object
    void clear();

    /// Returns nonzero if there aren't any samples available for outputting.
    int isEmpty() const;
};

}

#endif
