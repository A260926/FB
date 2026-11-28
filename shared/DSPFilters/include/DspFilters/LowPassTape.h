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
 * An offline, multichannel, time-varying cascade of RBJ low-pass biquads
 * with a reverse-mode (adjoint) pass for automatic equalization.
 *
 * Signal path
 * -----------
 * Every channel is filtered by `numStages` cascaded RBJ low-pass biquads,
 * realized as Direct Form II.  The forward pass deliberately reuses
 * Dsp::RBJ::LowPass::setup() for coefficient design and
 * Dsp::DirectFormII::process1() for the actual recursion, so the output is
 * the production DSPFilters sound and not a parallel reimplementation.
 * All channels share the same per-stage parameter tracks.
 *
 * Control events
 * --------------
 * An Event addresses one stage at one output frame:
 *
 *   frame       absolute output frame (0 based)
 *   stage       zero based cascade index, < numStages
 *   cutoff      RBJ cutoff frequency in Hz
 *   q           RBJ Q
 *   transition  length of the linear ramp in output frames
 *
 * Events are supplied to the constructor and must be sorted by nondecreasing
 * frame.  There may be at most one event per (frame, stage) pair.  Every
 * stage must have exactly one event at frame 0; this is its initial value and
 * it takes effect immediately (its transition field is ignored).
 *
 * For a later event the ramp starts from the value the stage actually
 * reached on the previous output frame.  A ramp of T > 0 output frames moves
 * in equal steps toward the new target and steps once before filtering the
 * event frame: frame f receives start + 1*(target-start)/T, frame
 * f+T-1 receives exactly the target.  T == 0 switches immediately.  A new
 * event cancels an unfinished ramp.  A target that is never reached still
 * influences the samples already produced and the starting point of the
 * next event.
 *
 * Recording and reverse pass
 * --------------------------
 *   push(input, numFrames, output)
 *     Record and filter a block.  input/output are arrays of `numChannels`
 *     channel pointers.  The tape keeps a private copy of the input so it
 *     can be replayed later.  A zero-frame push touches no pointers and
 *     does not advance the clock.
 *
 *   seal()
 *     Finish recording.  push() is rejected afterwards.
 *
 *   backward(outputAdjoint)
 *     Return first-order derivatives of
 *
 *        sum over c,n of outputAdjoint[c][n] * output[c][n]
 *
 *     with respect to every input sample and every event's cutoff and q.
 *     The adjoint is accumulated through every channel, through all later
 *     frames, and through the parameter ramp history (including ramps that
 *     were abandoned).  backward() may be called repeatedly with different
 *     error signals.
 *
 *     Gradients::input is [channel][frame]; Gradients::cutoff and
 *     Gradients::q are indexed like the event vector passed to the
 *     constructor (including frame-0 initial events).
 *
 * Replay state
 * ------------
 * The tape stores the full input plus checkpoints every `checkpointInterval`
 * (K) frames and, during backward(), one block of per-frame Direct Form II
 * delay elements.  It does not store per-frame activations for the whole
 * segment.  The number of retained state doubles is at most
 * O((N/K + K) * channels * stages), reported by peakSavedStateCount().
 * Each backward() replays at most N output frames through the production
 * filter (at most 2N, as required).
 *
 * Supported range
 * ---------------
 * sampleRate is one of 44100, 48000, 96000; channels is 1, 2, or 6; stages
 * is 1..4; cutoff is in [200, 0.2*sampleRate]; q is in [0.5, 2];
 * checkpointInterval is at least 1.  Invalid arguments, invalid events,
 * non-finite input, and invalid operations throw; already written audio and
 * previously returned gradients are left untouched.
 */

class LowPassTape
{
public:
  struct Event
  {
    std::uint64_t frame;
    std::size_t stage;
    double cutoff;
    double q;
    std::uint32_t transition;
  };

  struct Gradients
  {
    // input[channel][frame] = d(loss)/d(input sample)
    std::vector<std::vector<double>> input;

    // Per event, in the same order as the constructor's event vector.
    std::vector<double> cutoff;
    std::vector<double> q;
  };

  LowPassTape (double sampleRate,
               std::size_t numChannels,
               std::size_t numStages,
               std::vector<Event> events,
               std::size_t checkpointInterval);

  // Alternate argument order: events first.
  LowPassTape (std::vector<Event> events,
               double sampleRate,
               std::size_t numChannels,
               std::size_t numStages,
               std::size_t checkpointInterval);

  ~LowPassTape ();

  LowPassTape (const LowPassTape&) = delete;
  LowPassTape& operator= (const LowPassTape&) = delete;
  LowPassTape (LowPassTape&&) noexcept;
  LowPassTape& operator= (LowPassTape&&) noexcept;

  // Record and filter one block.
  void push (const double* const* input,
             std::size_t numFrames,
             double* const* output);

  // Finish recording.  Idempotent.
  void seal ();

  // Reverse pass for the supplied output error signal.
  Gradients backward (const double* const* outputAdjoint);

  bool sealed () const noexcept;
  std::size_t numFrames () const noexcept;
  std::size_t numChannels () const noexcept;
  std::size_t numStages () const noexcept;
  std::size_t numEvents () const noexcept;
  std::size_t checkpointInterval () const noexcept;

  // Number of output frames replayed through the production filter during
  // the most recent / any backward() call.
  std::size_t lastReplayFrames () const noexcept;
  std::size_t maxReplayFrames () const noexcept;

  // Peak number of retained filter-state doubles (checkpoints plus the
  // per-block replay activation buffer) during the tape's lifetime.
  std::size_t peakSavedStateCount () const noexcept;

  // Current number of retained checkpoint state doubles.
  std::size_t savedStateCount () const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace Dsp

#endif
