//////////////////////////////////////////////////////////////////////////////
///
/// SoundTouch - main class for tempo/pitch/rate adjusting routines. 
///
/// Notes:
/// - Initialize the SoundTouch object instance by setting up the sound stream 
///   parameters with functions 'setSampleRate' and 'setChannels', then set 
///   desired tempo/pitch/rate settings with the corresponding functions.
///
/// - The SoundTouch class behaves like a first-in-first-out pipeline: The 
///   samples that are to be processed are fed into one of the pipe by calling
///   function 'putSamples', while the ready processed samples can be read 
///   from the other end of the pipeline with function 'receiveSamples'.
/// 
/// - The SoundTouch processing classes require certain sized 'batches' of 
///   samples in order to process the sound. For this reason the classes buffer 
///   incoming samples until there are enough of samples available for 
///   processing, then they carry out the processing step and consequently
///   make the processed samples available for outputting.
/// 
/// - For the above reason, the processing routines introduce a certain 
///   'latency' between the input and output, so that the samples input to
///   SoundTouch may not be immediately available in the output, and neither 
///   the amount of outputtable samples may not immediately be in direct 
///   relationship with the amount of previously input samples.
///
/// - The tempo/pitch/rate control parameters can be altered during processing.
///   Please notice though that they aren't currently protected by semaphores,
///   so in multi-thread application external semaphore protection may be
///   required.
///
/// - This class utilizes classes 'TDStretch' for tempo change (without modifying
///   pitch) and 'RateTransposer' for changing the playback rate (that is, both 
///   tempo and pitch in the same ratio) of the sound. The third available control 
///   'pitch' (change pitch but maintain tempo) is produced by a combination of
///   combining the two other controls.
///
/// Author        : Copyright (c) Olli Parviainen
/// Author e-mail : oparviai 'at' iki.fi
/// SoundTouch WWW: http://www.surina.net/soundtouch
///
////////////////////////////////////////////////////////////////////////////////
//
// Last changed  : $Date: 2014-01-05 16:40:22 -0500 (Sun, 05 Jan 2014) $
// File revision : $Revision: 4 $
//
// $Id: SoundTouch.cpp 177 2014-01-05 21:40:22Z oparviai $
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

#include <assert.h>
#include <stdlib.h>
#include <memory.h>
#include <math.h>
#include <stdio.h>
#include <stdexcept>

#include "SoundTouch.h"
#include "TDStretch.h"
#include "AnchorTDStretch.h"
#include "RateTransposer.h"
#include "cpu_detect.h"

using namespace soundtouch;

/// test if two floating point numbers are equal
#define TEST_FLOAT_EQUAL(a, b)  (fabs(a - b) < 1e-10)

/// Largest coordinate accepted in an anchor list (2^53).
static const uint64_t ANCHOR_MAX_COORD = (uint64_t)1 << 53;


/// Print library version string for autoconf
extern "C" void soundtouch_ac_test()
{
    printf("SoundTouch Version: %s\n",SOUNDTOUCH_VERSION);
} 


SoundTouch::SoundTouch()
{
    // Initialize rate transposer and tempo changer instances

    pRateTransposer = new RateTransposer();
    pTDStretch = TDStretch::newInstance();
    pAnchor = new AnchorTDStretch();

    setOutPipe(pTDStretch);

    rate = tempo = 0;

    virtualPitch =
    virtualRate =
    virtualTempo = 1.0;

    calcEffectiveRateAndTempo();

    channels = 0;
    bSrateSet = FALSE;

    anchorMode = false;
    streamStarted = false;
    anchorFinished = false;
    anchorProtectFrames = 0;
    savedVirtualRate = savedVirtualTempo = savedVirtualPitch = 1.0f;
    haveSavedVirtual = false;
}



SoundTouch::~SoundTouch()
{
    delete pRateTransposer;
    delete pTDStretch;
    delete pAnchor;
}



/// Get SoundTouch library version string
const char *SoundTouch::getVersionString()
{
    static const char *_version = SOUNDTOUCH_VERSION;

    return _version;
}


/// Get SoundTouch library version Id
uint SoundTouch::getVersionId()
{
    return SOUNDTOUCH_VERSION_ID;
}


// Sets the number of channels, 1 = mono, 2 = stereo
void SoundTouch::setChannels(uint numChannels)
{
    if (anchorMode)
    {
        // The audio configuration is fixed for the whole anchored run.
        throw std::logic_error("SoundTouch : audio configuration is fixed in anchor mode, call clear() first");
    }
    /*if (numChannels != 1 && numChannels != 2)
    {
        //ST_THROW_RT_ERROR("Illegal number of channels");
		return;
    }*/
    channels = numChannels;
    pRateTransposer->setChannels((int)numChannels);
    pTDStretch->setChannels((int)numChannels);
}



// Sets new rate control value. Normal rate = 1.0, smaller values
// represent slower rate, larger faster rates.
void SoundTouch::setRate(float newRate)
{
    if (anchorMode)
    {
        throw std::logic_error("SoundTouch : rate/pitch/tempo are fixed in anchor mode, call clear() first");
    }
    virtualRate = newRate;
    calcEffectiveRateAndTempo();
}



// Sets new rate control value as a difference in percents compared
// to the original rate (-50 .. +100 %)
void SoundTouch::setRateChange(float newRate)
{
    if (anchorMode)
    {
        throw std::logic_error("SoundTouch : rate/pitch/tempo are fixed in anchor mode, call clear() first");
    }
    virtualRate = 1.0f + 0.01f * newRate;
    calcEffectiveRateAndTempo();
}



// Sets new tempo control value. Normal tempo = 1.0, smaller values
// represent slower tempo, larger faster tempo.
void SoundTouch::setTempo(float newTempo)
{
    if (anchorMode)
    {
        throw std::logic_error("SoundTouch : rate/pitch/tempo are fixed in anchor mode, call clear() first");
    }
    virtualTempo = newTempo;
    calcEffectiveRateAndTempo();
}



// Sets new tempo control value as a difference in percents compared
// to the original tempo (-50 .. +100 %)
void SoundTouch::setTempoChange(float newTempo)
{
    if (anchorMode)
    {
        throw std::logic_error("SoundTouch : rate/pitch/tempo are fixed in anchor mode, call clear() first");
    }
    virtualTempo = 1.0f + 0.01f * newTempo;
    calcEffectiveRateAndTempo();
}



// Sets new pitch control value. Original pitch = 1.0, smaller values
// represent lower pitches, larger values higher pitch.
void SoundTouch::setPitch(float newPitch)
{
    if (anchorMode)
    {
        throw std::logic_error("SoundTouch : rate/pitch/tempo are fixed in anchor mode, call clear() first");
    }
    virtualPitch = newPitch;
    calcEffectiveRateAndTempo();
}



// Sets pitch change in octaves compared to the original pitch
// (-1.00 .. +1.00)
void SoundTouch::setPitchOctaves(float newPitch)
{
    if (anchorMode)
    {
        throw std::logic_error("SoundTouch : rate/pitch/tempo are fixed in anchor mode, call clear() first");
    }
    virtualPitch = (float)exp(0.69314718056f * newPitch);
    calcEffectiveRateAndTempo();
}



// Sets pitch change in semi-tones compared to the original pitch
// (-12 .. +12)
void SoundTouch::setPitchSemiTones(int newPitch)
{
    setPitchOctaves((float)newPitch / 12.0f);
}



void SoundTouch::setPitchSemiTones(float newPitch)
{
    setPitchOctaves(newPitch / 12.0f);
}


// Calculates 'effective' rate and tempo values from the
// nominal control values.
void SoundTouch::calcEffectiveRateAndTempo()
{
    float oldTempo = tempo;
    float oldRate = rate;

    tempo = virtualTempo / virtualPitch;
    rate = virtualPitch * virtualRate;

    if (!TEST_FLOAT_EQUAL(rate,oldRate)) pRateTransposer->setRate(rate);
    if (!TEST_FLOAT_EQUAL(tempo, oldTempo)) pTDStretch->setTempo(tempo);

#ifndef SOUNDTOUCH_PREVENT_CLICK_AT_RATE_CROSSOVER
    if (rate <= 1.0f) 
    {
        if (output != pTDStretch) 
        {
            FIFOSamplePipe *tempoOut;

            assert(output == pRateTransposer);
            // move samples in the current output buffer to the output of pTDStretch
            tempoOut = pTDStretch->getOutput();
            tempoOut->moveSamples(*output);
            // move samples in pitch transposer's store buffer to tempo changer's input
            // deprecated : pTDStretch->moveSamples(*pRateTransposer->getStore());

            output = pTDStretch;
        }
    }
    else
#endif
    {
        if (output != pRateTransposer) 
        {
            FIFOSamplePipe *transOut;

            assert(output == pTDStretch);
            // move samples in the current output buffer to the output of pRateTransposer
            transOut = pRateTransposer->getOutput();
            transOut->moveSamples(*output);
            // move samples in tempo changer's input to pitch transposer's input
            pRateTransposer->moveSamples(*pTDStretch->getInput());

            output = pRateTransposer;
        }
    } 
}


// Returns the configured sample rate, or 0 if it has not been set yet.
uint SoundTouch::sampleRate() const
{
    int srate = 0;
    int sequenceMs = 0, seekWindowMs = 0, overlapMs = 0;
    pTDStretch->getParameters(&srate, &sequenceMs, &seekWindowMs, &overlapMs);
    return (uint)srate;
}


// Sets sample rate.
void SoundTouch::setSampleRate(uint srate)
{
    if (anchorMode)
    {
        throw std::logic_error("SoundTouch : audio configuration is fixed in anchor mode, call clear() first");
    }
    bSrateSet = TRUE;
    // set sample rate, leave other tempo changer parameters as they are.
    pTDStretch->setParameters((int)srate);
}


// Validates an anchor list and the protection width. Any failure throws
// std::invalid_argument and leaves the caller's state untouched.
void SoundTouch::validateTimeAnchors(const std::vector<TimeAnchor> &anchors,
                                     uint protectFrames) const
{
    // The anchored mode is intentionally limited to 48 kHz float mono/stereo.
    if (bSrateSet == FALSE)
    {
        throw std::invalid_argument("SoundTouch : set sample rate before configuring time anchors");
    }
    if (sampleRate() != 48000)
    {
        throw std::invalid_argument("SoundTouch : anchored mode only supports 48 kHz");
    }
    if (channels != 1 && channels != 2)
    {
        throw std::invalid_argument("SoundTouch : anchored mode only supports 1 or 2 channels");
    }
    if (protectFrames < 16 || protectFrames > 256)
    {
        throw std::invalid_argument("SoundTouch : protectFrames must be in [16, 256]");
    }
    if (anchors.empty())
    {
        return;     // empty list simply disables the mode
    }

    const size_t count = anchors.size();
    if (anchors.front().inputFrame != 0 || anchors.front().outputFrame != 0)
    {
        throw std::invalid_argument("SoundTouch : first anchor must be (0, 0)");
    }

    // First pass: per-anchor coordinate rules.
    for (size_t i = 0; i < count; i ++)
    {
        const TimeAnchor &cur = anchors[i];
        if (cur.inputFrame > ANCHOR_MAX_COORD || cur.outputFrame > ANCHOR_MAX_COORD)
        {
            throw std::invalid_argument("SoundTouch : anchor coordinates must be <= 2^53");
        }
        if (i + 1 < count)
        {
            const TimeAnchor &nxt = anchors[i + 1];
            // both columns must be strictly increasing
            if (nxt.inputFrame <= cur.inputFrame || nxt.outputFrame <= cur.outputFrame)
            {
                throw std::invalid_argument("SoundTouch : anchor columns must be strictly increasing");
            }
        }
    }

    // Second pass: protected-zone geometry between neighbouring anchors.
    for (size_t i = 0; i + 1 < count; i ++)
    {
        const TimeAnchor &cur = anchors[i];
        const TimeAnchor &nxt = anchors[i + 1];

        // Input gaps and output gaps must leave at least 8192 frames between
        // neighbouring protected zones on both time axes.
        uint64_t inGap = nxt.inputFrame - cur.inputFrame - 2ull * protectFrames;
        uint64_t outGap = nxt.outputFrame - cur.outputFrame - 2ull * protectFrames;
        if (inGap < 8192 || outGap < 8192)
        {
            throw std::invalid_argument("SoundTouch : protected zones must be at least 8192 frames apart on both axes");
        }
        // gap length ratio output/input must stay within [0.5, 2]
        // (checked with integer arithmetic to stay exact at 2^53)
        if (outGap * 2ull < inGap || inGap * 2ull < outGap)
        {
            throw std::invalid_argument("SoundTouch : output/input gap ratio must be in [0.5, 2]");
        }
    }
}


// Enables / replaces / disables the anchored time-stretch mode.
void SoundTouch::setTimeAnchors(const std::vector<TimeAnchor> &anchors,
                                uint protectFrames)
{
    // Configuration is only allowed while no non-empty input has been seen.
    if (streamStarted || anchorFinished)
    {
        throw std::logic_error("SoundTouch : time anchors can only be changed before streaming starts");
    }

    if (anchors.empty())
    {
        // Disable the mode. No non-empty input has been accepted in this idle
        // state, so drop any anchor engine state and restore the numeric
        // tempo/pitch/rate values that were captured when the mode was enabled.
        pAnchor->clear();
        anchorMode = false;
        anchorProtectFrames = 0;
        if (haveSavedVirtual)
        {
            virtualRate = savedVirtualRate;
            virtualTempo = savedVirtualTempo;
            virtualPitch = savedVirtualPitch;
            calcEffectiveRateAndTempo();
        }
        haveSavedVirtual = false;
        return;
    }

    // Validate before touching any existing configuration.
    validateTimeAnchors(anchors, protectFrames);

    // Remember the caller's ordinary numeric settings so clear() can restore
    // them; anchored processing itself always runs at nominal tempo/pitch/rate.
    if (!haveSavedVirtual)
    {
        savedVirtualRate = virtualRate;
        savedVirtualTempo = virtualTempo;
        savedVirtualPitch = virtualPitch;
        haveSavedVirtual = true;
    }
    virtualRate = virtualTempo = virtualPitch = 1.0f;
    calcEffectiveRateAndTempo();
    pTDStretch->enableQuickSeek(FALSE);
    pTDStretch->setParameters(48000, 40, 15, 8);

    // Configure the dedicated anchored engine with the required window set.
    pAnchor->configure((int)channels, 48000, 40, 15, 8);
    pAnchor->reset(anchors, protectFrames);

    anchorMode = true;
    anchorProtectFrames = protectFrames;
    anchorFinished = false;
}


// Adds 'numSamples' pcs of samples from the 'samples' memory position into
// the input of the object.
void SoundTouch::putSamples(const SAMPLETYPE *samples, uint nSamples)
{
    if (bSrateSet == FALSE)
    {
        ST_THROW_RT_ERROR("SoundTouch : Sample rate not defined");
    }
    else if (channels == 0)
    {
        ST_THROW_RT_ERROR("SoundTouch : Number of channels not defined");
    }

    if (anchorMode)
    {
        if (nSamples == 0)
        {
            // zero-length input does not advance the stream
            return;
        }
        if (anchorFinished)
        {
            throw std::logic_error("SoundTouch : anchored run already finished, call clear() to start another");
        }

        uint64_t fed = pAnchor->getInputFramesFed();
        uint64_t limit = pAnchor->getExpectedInputFrames();
        // Reject the whole call, before writing anything, if it would
        // overshoot the declared input length N.
        if (fed + (uint64_t)nSamples > limit)
        {
            throw std::invalid_argument("SoundTouch : input exceeds the declared anchor input length");
        }

        pAnchor->putSamples(samples, nSamples);
        streamStarted = true;
        return;
    }

    // Transpose the rate of the new samples if necessary
    /* Bypass the nominal setting - can introduce a click in sound when tempo/pitch control crosses the nominal value...
    if (rate == 1.0f)
    {
        // The rate value is same as the original, simply evaluate the tempo changer.
        assert(output == pTDStretch);
        if (pRateTransposer->isEmpty() == 0)
        {
            // yet flush the last samples in the pitch transposer buffer
            // (may happen if 'rate' changes from a non-zero value to zero)
            pTDStretch->moveSamples(*pRateTransposer);
        }
        pTDStretch->putSamples(samples, nSamples);
    }
    */
#ifndef SOUNDTOUCH_PREVENT_CLICK_AT_RATE_CROSSOVER
    else if (rate <= 1.0f)
    {
        // transpose the rate down, output the transposed sound to tempo changer buffer
        assert(output == pTDStretch);
        pRateTransposer->putSamples(samples, nSamples);
        pTDStretch->moveSamples(*pRateTransposer);
    }
    else
#endif
    {
        // evaluate the tempo changer, then transpose the rate up,
        assert(output == pRateTransposer);
        pTDStretch->putSamples(samples, nSamples);
        pRateTransposer->moveSamples(*pTDStretch);
    }

    if (nSamples != 0)
    {
        streamStarted = true;
    }
}


// Flushes the last samples from the processing pipeline to the output.
// Clears also the internal processing buffers.
//
// Note: This function is meant for extracting the last samples of a sound
// stream. This function may introduce additional blank samples in the end
// of the sound stream, and thus it's not recommended to call this function
// in the middle of a sound stream.
void SoundTouch::flush()
{
    if (anchorMode)
    {
        if (anchorFinished)
        {
            // Repeated flush is a no-op: no more sound is produced.
            return;
        }
        uint64_t fed = pAnchor->getInputFramesFed();
        uint64_t limit = pAnchor->getExpectedInputFrames();
        if (fed < limit)
        {
            // Not all declared input has arrived yet; refuse and keep the
            // state so the caller can keep feeding samples afterwards.
            throw std::logic_error("SoundTouch : anchored flush requires all declared input frames first");
        }

        // Drain the remaining segments and the final protected zone without
        // feeding any zero padding; total output is exactly M frames.
        pAnchor->finish();
        anchorFinished = true;
        return;
    }

    int i;
    int nUnprocessed;
    int nOut;
    SAMPLETYPE *buff=(SAMPLETYPE*)alloca(64*channels*sizeof(SAMPLETYPE));

    // check how many samples still await processing, and scale
    // that by tempo & rate to get expected output sample count
    nUnprocessed = numUnprocessedSamples();
    nUnprocessed = (int)((double)nUnprocessed / (tempo * rate) + 0.5);

    nOut = numSamples();        // ready samples currently in buffer ...
    nOut += nUnprocessed;       // ... and how many we expect there to be in the end

    memset(buff, 0, 64 * channels * sizeof(SAMPLETYPE));
    // "Push" the last active samples out from the processing pipeline by
    // feeding blank samples into the processing pipeline until new,
    // processed samples appear in the output (not however, more than
    // 8ksamples in any case)
    for (i = 0; i < 128; i ++)
    {
        putSamples(buff, 64);
        if ((int)numSamples() >= nOut)
        {
            // Enough new samples have appeared into the output!
            // As samples come from processing with bigger chunks, now truncate it
            // back to maximum "nOut" samples to improve duration accuracy
            adjustAmountOfSamples(nOut);

            // finish
            break;
        }
    }

    // Clear working buffers
    pRateTransposer->clear();
    pTDStretch->clearInput();
    // yet leave the 'tempoChanger' output intouched as that's where the
    // flushed samples are!
}


// Changes a setting controlling the processing system behaviour. See the
// 'SETTING_...' defines for available setting ID's.
BOOL SoundTouch::setSetting(int settingId, int value)
{
    if (anchorMode)
    {
        // Windowing / anti-alias configuration is fixed for the anchored run.
        throw std::logic_error("SoundTouch : processing settings are fixed in anchor mode, call clear() first");
    }
    int sampleRate, sequenceMs, seekWindowMs, overlapMs;

    // read current tdstretch routine parameters
    pTDStretch->getParameters(&sampleRate, &sequenceMs, &seekWindowMs, &overlapMs);

    switch (settingId) 
    {
        case SETTING_USE_AA_FILTER :
            // enables / disabless anti-alias filter
            pRateTransposer->enableAAFilter((value != 0) ? TRUE : FALSE);
            return TRUE;

        case SETTING_AA_FILTER_LENGTH :
            // sets anti-alias filter length
            pRateTransposer->getAAFilter()->setLength(value);
            return TRUE;

        case SETTING_USE_QUICKSEEK :
            // enables / disables tempo routine quick seeking algorithm
            pTDStretch->enableQuickSeek((value != 0) ? TRUE : FALSE);
            return TRUE;

        case SETTING_SEQUENCE_MS:
            // change time-stretch sequence duration parameter
            pTDStretch->setParameters(sampleRate, value, seekWindowMs, overlapMs);
            return TRUE;

        case SETTING_SEEKWINDOW_MS:
            // change time-stretch seek window length parameter
            pTDStretch->setParameters(sampleRate, sequenceMs, value, overlapMs);
            return TRUE;

        case SETTING_OVERLAP_MS:
            // change time-stretch overlap length parameter
            pTDStretch->setParameters(sampleRate, sequenceMs, seekWindowMs, value);
            return TRUE;

        default :
            return FALSE;
    }
}


// Reads a setting controlling the processing system behaviour. See the
// 'SETTING_...' defines for available setting ID's.
//
// Returns the setting value.
int SoundTouch::getSetting(int settingId) const
{
    int temp;

    switch (settingId) 
    {
        case SETTING_USE_AA_FILTER :
            return (uint)pRateTransposer->isAAFilterEnabled();

        case SETTING_AA_FILTER_LENGTH :
            return pRateTransposer->getAAFilter()->getLength();

        case SETTING_USE_QUICKSEEK :
            return (uint)   pTDStretch->isQuickSeekEnabled();

        case SETTING_SEQUENCE_MS:
            pTDStretch->getParameters(NULL, &temp, NULL, NULL);
            return temp;

        case SETTING_SEEKWINDOW_MS:
            pTDStretch->getParameters(NULL, NULL, &temp, NULL);
            return temp;

        case SETTING_OVERLAP_MS:
            pTDStretch->getParameters(NULL, NULL, NULL, &temp);
            return temp;

		case SETTING_NOMINAL_INPUT_SEQUENCE :
			return pTDStretch->getInputSampleReq();

		case SETTING_NOMINAL_OUTPUT_SEQUENCE :
			return pTDStretch->getOutputBatchSize();

		default :
            return 0;
    }
}


// Clears all the samples in the object's output and internal processing
// buffers. In anchor mode the anchor configuration is dropped as well while
// the sample rate, channel count and numeric settings are retained, so a new
// run can be configured afterwards.
void SoundTouch::clear()
{
    pRateTransposer->clear();
    pTDStretch->clear();
    pAnchor->clear();

    bool wasAnchor = anchorMode;
    anchorMode = false;
    anchorProtectFrames = 0;
    anchorFinished = false;
    streamStarted = false;

    // Restore the ordinary numeric tempo/pitch/rate settings captured when the
    // anchor mode was enabled, while keeping sample rate and channel count.
    if (wasAnchor && haveSavedVirtual)
    {
        virtualRate = savedVirtualRate;
        virtualTempo = savedVirtualTempo;
        virtualPitch = savedVirtualPitch;
        calcEffectiveRateAndTempo();
    }
    haveSavedVirtual = false;
}



// Reads ready output samples. Routed to the anchored engine while anchor
// mode is enabled, otherwise to the ordinary processing pipeline.
uint SoundTouch::receiveSamples(SAMPLETYPE *outBuffer, uint maxSamples)
{
    if (anchorMode)
    {
        return pAnchor->receiveSamples(outBuffer, maxSamples);
    }
    return FIFOProcessor::receiveSamples(outBuffer, maxSamples);
}


// Number of output frames ready to be received.
uint SoundTouch::numSamples() const
{
    if (anchorMode)
    {
        return pAnchor->numSamples();
    }
    return FIFOProcessor::numSamples();
}


int SoundTouch::isEmpty() const
{
    if (anchorMode)
    {
        return pAnchor->numSamples() == 0 ? 1 : 0;
    }
    return FIFOProcessor::isEmpty();
}



/// Returns number of samples currently unprocessed.
uint SoundTouch::numUnprocessedSamples() const
{
    if (anchorMode)
    {
        return pAnchor->numUnprocessedFrames();
    }
    FIFOSamplePipe * psp;
    if (pTDStretch)
    {
        psp = pTDStretch->getInput();
        if (psp)
        {
            return psp->numSamples();
        }
    }
    return 0;
}
