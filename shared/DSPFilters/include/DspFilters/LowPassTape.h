/*******************************************************************************

"A Collection of Useful C++ Classes for Digital Signal Processing"
 By Vinnie Falco

Official project location:
https://github.com/vinniefalco/DSPFilters

See Documentation.cpp for contact information, notes, and bibliography.

--------------------------------------------------------------------------------

License: MIT License (http://www.opensource.org/licenses/mit-license.php)
Copyright (c) 2009 by Vinnie Falco

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*******************************************************************************/

#ifndef DSPFILTERS_LOWPASSTAPE_H
#define DSPFILTERS_LOWPASSTAPE_H

#include "DspFilters/Common.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Dsp {

/*
 * LowPassTape
 *
 * A recordable, replayable cascade of RBJ low pass biquads intended for
 * offline automatic equalisation.  Channels share one control track per
 * stage; the control track is a list of time stamped parameter events with
 * sample accurate linear transitions.
 *
 * The forward path is built from the production RBJ::LowPass coefficient
 * design and the production DirectFormII recursion (through the normal
 * Biquad/State route), so the rendered sound is exactly the sound of the
 * existing filter classes.  After seal() the recorded input can be replayed
 * any number of times with different output adjoints; backward() returns the
 * first order derivative of
 *
 *     sum over channels c and frames n of outputAdjoint[c][n] * output[c][n]
 *
 * with respect to every input sample and every event's cutoff and q.
 *
 * Event semantics
 * ---------------
 * Events are ordered by frame.  Each stage must have exactly one event at
 * frame 0; that initial value is effective immediately (its transition field
 * is ignored).  A later event at frame f starts from the value the stage
 * actually used at frame f-1.  With transition T > 0 the value moves in T
 * equal steps, the first step being taken before frame f is filtered, so the
 * value equals the new target exactly on frame f+T-1.  T == 0 switches
 * immediately.  A new event cancels the remainder of an older transition;
 * the partially traversed history is still audible and is differentiated
 * correctly.
 *
 * Memory
 * ------
 * The complete input is retained for replay.  Filter activations are not
 * retained per frame: at every checkpointInterval samples a checkpoint of
 * the production DirectFormII state is captured, and backward() replays at
 * most one interval at a time.  The saved checkpoint state plus the local
 * replay activations are O((N/K + K) * channels * stages); the real peak
 * counts are available from the methods below.
 */

struct LowPassTapeEvent
{
  std::uint64_t frame;      // output frame index
  std::size_t stage;        // stage index, 0 .. stages-1
  double cutoff;            // Hz
  double q;                 // Q
  std::uint32_t transition; // transition length in output frames
};

class LowPassTape
{
public:
  struct Spec
  {
    double sampleRate;
    std::size_t channels;
    std::size_t stages;
    std::size_t checkpointInterval; // K, in frames

    Spec ()
      : sampleRate (48000)
      , channels (1)
      , stages (1)
      , checkpointInterval (1024)
    {
    }

    Spec (double sampleRate_,
          std::size_t channels_,
          std::size_t stages_,
          std::size_t checkpointInterval_)
      : sampleRate (sampleRate_)
      , channels (channels_)
      , stages (stages_)
      , checkpointInterval (checkpointInterval_)
    {
    }
  };

  struct Gradients
  {
    // channel major: input[channel * frames + frame]
    std::vector<double> input;

    // one entry per event, in the order the events were supplied
    std::vector<double> eventCutoff;
    std::vector<double> eventQ;

    std::size_t frames;
    std::size_t channels;
    std::size_t stages;

    // diagnostics for this backward() call
    std::size_t replayFrames;          // production filter frames replayed
    std::size_t checkpointStateCount;  // saved production states at call time
    std::size_t peakLocalActivations;  // per-frame activations held locally

    Gradients ()
      : frames (0)
      , channels (0)
      , stages (0)
      , replayFrames (0)
      , checkpointStateCount (0)
      , peakLocalActivations (0)
    {
    }

    double inputAt (std::size_t channel, std::size_t frame) const
    {
      return input[channel * frames + frame];
    }

    const double* inputChannel (std::size_t channel) const
    {
      return input.data() + channel * frames;
    }

    double cutoffGradient (std::size_t event) const
    {
      return eventCutoff[event];
    }

    double qGradient (std::size_t event) const
    {
      return eventQ[event];
    }
  };

public:
  LowPassTape (const Spec& spec,
               const std::vector<LowPassTapeEvent>& events);

  LowPassTape (double sampleRate,
               std::size_t channels,
               std::size_t stages,
               std::size_t checkpointInterval,
               const std::vector<LowPassTapeEvent>& events);

  ~LowPassTape ();

  LowPassTape (LowPassTape&&) noexcept;
  LowPassTape& operator= (LowPassTape&&) noexcept;

  LowPassTape (const LowPassTape&) = delete;
  LowPassTape& operator= (const LowPassTape&) = delete;

  // Record and render one block.  input and output may alias.  A zero frame
  // block does not dereference either pointer and does not advance the clock.
  // Throws std::logic_error after seal(); throws std::invalid_argument (with
  // no output written and no clock advance) for unusable arguments.
  void push (const double* const* input,
             std::size_t frames,
             double* const* output);

  // Finish recording.  Further push() calls are rejected.  Calling seal()
  // more than once is harmless.
  void seal ();

  // Replay the sealed segment and return derivatives.  May be called any
  // number of times, with a different adjoint each time.  outputAdjoint is
  // channel major (channel * frames + frame) just like Gradients::input.
  // It may be null only when no frames were recorded.
  Gradients backward (const double* const* outputAdjoint);

  bool sealed () const;
  bool isSealed () const { return sealed (); }

  std::size_t framesSeen () const;
  std::size_t framesPushed () const { return framesSeen (); }
  std::size_t channelCount () const;
  std::size_t stageCount () const;
  std::size_t checkpointInterval () const;
  double sampleRate () const;

  const std::vector<LowPassTapeEvent>& events () const;

  // Real peak counts (not estimates).
  std::size_t checkpointCount () const;
  std::size_t checkpointStateCount () const;   // checkpoints * channels * stages
  std::size_t peakLocalActivations () const;   // peak per-frame w values
  std::size_t peakSavedStateCount () const;    // checkpointStateCount + peakLocalActivations
  std::size_t lastReplayFrameCount () const;
  std::size_t peakReplayFrameCount () const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}

#endif
