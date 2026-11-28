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

#include "DspFilters/Common.h"
#include "DspFilters/LowPassTape.h"
#include "DspFilters/MathSupplement.h"
#include "DspFilters/RBJ.h"
#include "DspFilters/State.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Dsp {

namespace {

const double kMinCutoff      = 200.0;
const double kMaxCutoffRatio = 0.2;
const double kMinQ           = 0.5;
const double kMaxQ           = 2.0;
const double kMaxAbsInput    = 2.0;

// Approximate scalar footprint of one StageControl record, used only for the
// honest peak-state accounting reported to the caller.
const std::size_t kControlScalars = 12;

inline bool isSupportedRate (double sampleRate)
{
  return sampleRate == 44100.0 || sampleRate == 48000.0 || sampleRate == 96000.0;
}

inline bool isSupportedChannels (std::size_t channels)
{
  return channels == 1 || channels == 2 || channels == 6;
}

inline bool isSupportedStages (std::size_t stages)
{
  return stages >= 1 && stages <= 4;
}

void validateEventParams (double sampleRate, double cutoff, double q)
{
  if (!std::isfinite (cutoff) || !std::isfinite (q))
    throw std::invalid_argument ("Dsp::LowPassTape: non-finite cutoff or Q");
  if (!(cutoff >= kMinCutoff && cutoff <= kMaxCutoffRatio * sampleRate))
    throw std::out_of_range ("Dsp::LowPassTape: cutoff out of supported range");
  if (!(q >= kMinQ && q <= kMaxQ))
    throw std::out_of_range ("Dsp::LowPassTape: Q out of supported range");
}

// Per-stage control bookkeeping. This is not a filter: it only decides which
// cutoff/Q values feed the production RBJ design at each frame.
struct StageControl
{
  bool active = false;
  std::uint64_t startFrame = 0;
  std::uint32_t len = 0;
  double startCutoff = 0.0;
  double startQ = 0.0;
  double targetCutoff = 0.0;
  double targetQ = 0.0;
  double curCutoff = 0.0;
  double curQ = 0.0;
  std::size_t nextEvent = 0;  // index into the stage's event list
};

enum ControlKind
{
  ControlHeld,
  ControlImmediate,
  ControlRamp
};

struct ControlInfo
{
  int kind = ControlHeld;
  std::size_t owner = 0;
  double frac = 1.0;
};

struct CellState
{
  double v1 = 0.0;
  double v2 = 0.0;
};

// The denormal-prevention bias used by Dsp::BiquadBase::State. It is a known
// constant sequence, independent of input and parameters, so the adjoint pass
// treats it as a constant.
inline double denormalBias (std::uint64_t frame)
{
  return ((frame & 1u) == 0) ? -anti_denormal_vsa : anti_denormal_vsa;
}

// Normalized RBJ low-pass coefficient partial derivatives with respect to
// (cutoff, q). Results are ordered [m_a1, m_a2, m_b0, m_b1, m_b2].
void coefficientDerivatives (double sampleRate,
                             double cutoff,
                             double q,
                             double* dCut,
                             double* dQ)
{
  const double w0 = 2.0 * doublePi * cutoff / sampleRate;
  const double cs = std::cos (w0);
  const double sn = std::sin (w0);
  const double AL = sn / (2.0 * q);

  const double a0 = 1.0 + AL;
  const double a1 = -2.0 * cs;
  const double a2 = 1.0 - AL;
  const double b0 = (1.0 - cs) / 2.0;
  const double b1 = 1.0 - cs;
  const double b2 = (1.0 - cs) / 2.0;
  const double a0sq = a0 * a0;

  // d(num/a0) for a perturbation of the numerator and denominator.
  auto dnorm = [a0, a0sq] (double num, double dNum, double da0) -> double
  {
    return (dNum * a0 - num * da0) / a0sq;
  };

  {
    const double dw0 = 2.0 * doublePi / sampleRate;
    const double dcs = -sn * dw0;
    const double dsn =  cs * dw0;
    const double dAL = dsn / (2.0 * q);
    const double da0 = dAL;
    dCut[0] = dnorm (a1, -2.0 * dcs, da0);
    dCut[1] = dnorm (a2, -dAL,        da0);
    dCut[2] = dnorm (b0, -dcs / 2.0,  da0);
    dCut[3] = dnorm (b1, -dcs,        da0);
    dCut[4] = dnorm (b2, -dcs / 2.0,  da0);
  }

  {
    const double dAL = -sn / (2.0 * q * q);
    const double da0 = dAL;
    dQ[0] = dnorm (a1, 0.0, da0);
    dQ[1] = dnorm (a2, -dAL, da0);
    dQ[2] = dnorm (b0, 0.0, da0);
    dQ[3] = dnorm (b1, 0.0, da0);
    dQ[4] = dnorm (b2, 0.0, da0);
  }
}

} // namespace

//------------------------------------------------------------------------------

struct LowPassTape::Impl
{
  struct Snapshot
  {
    std::vector<CellState> audio;      // [stage * channels + channel]
    std::vector<StageControl> control; // [stage]
  };

  Impl (double sampleRate_,
        std::size_t channels,
        std::size_t stages,
        std::vector<Event> eventList,
        std::size_t checkpointInterval)
    : sampleRate (sampleRate_)
    , numCh (channels)
    , numSt (stages)
    , checkpointK (checkpointInterval)
    , events (std::move (eventList))
  {
    validate ();
    buildStageEvents ();

    live.assign (numSt * numCh, DirectFormII ());
    control.assign (numSt, StageControl ());

    // Checkpoint at frame 0: zero audio state, fresh control tracks.
    Snapshot initial;
    initial.audio.assign (numSt * numCh, CellState ());
    initial.control.assign (numSt, StageControl ());
    addCheckpoint (initial);
  }

  // ---- configuration -------------------------------------------------
  double sampleRate;
  std::size_t numCh;
  std::size_t numSt;
  std::size_t checkpointK;
  std::vector<Event> events;
  std::vector<std::vector<std::size_t>> stageEvents;

  // ---- live recording state ------------------------------------------
  std::vector<double> inputTape;       // frame-major: [frame * channels + channel]
  std::vector<DirectFormII> live;      // [stage * channels + channel]
  std::vector<StageControl> control;   // [stage]
  std::uint64_t clock = 0;
  bool isSealed = false;

  // ---- checkpoints ---------------------------------------------------
  std::vector<Snapshot> checkpoints;
  std::size_t checkpointScalars = 0;
  std::size_t peakSavedScalars = 0;

  std::size_t lastReplayFrames = 0;
  std::size_t maxReplayFrames = 0;

  std::size_t frames () const
  {
    return inputTape.size () / numCh;
  }

  // ---- validation ----------------------------------------------------
  void validate ()
  {
    if (!isSupportedRate (sampleRate))
      throw std::invalid_argument ("Dsp::LowPassTape: unsupported sample rate");
    if (!isSupportedChannels (numCh))
      throw std::invalid_argument ("Dsp::LowPassTape: unsupported channel count");
    if (!isSupportedStages (numSt))
      throw std::invalid_argument ("Dsp::LowPassTape: unsupported stage count");
    if (checkpointK == 0)
      throw std::invalid_argument ("Dsp::LowPassTape: checkpoint interval must be positive");

    for (std::size_t i = 0; i < events.size (); ++i)
    {
      const Event& e = events[i];
      if (e.stage >= numSt)
        throw std::out_of_range ("Dsp::LowPassTape: event stage out of range");
      validateEventParams (sampleRate, e.cutoff, e.q);
      if (i > 0 && e.frame < events[i - 1].frame)
        throw std::invalid_argument ("Dsp::LowPassTape: events not sorted by frame");
    }
  }

  void buildStageEvents ()
  {
    stageEvents.assign (numSt, std::vector<std::size_t> ());
    for (std::size_t i = 0; i < events.size (); ++i)
      stageEvents[events[i].stage].push_back (i);

    for (std::size_t s = 0; s < numSt; ++s)
    {
      const std::vector<std::size_t>& evs = stageEvents[s];
      if (evs.empty () || events[evs.front ()].frame != 0)
        throw std::invalid_argument ("Dsp::LowPassTape: every stage needs a frame-0 event");
      for (std::size_t j = 1; j < evs.size (); ++j)
        if (events[evs[j]].frame == events[evs[j - 1]].frame)
          throw std::invalid_argument ("Dsp::LowPassTape: duplicate event for a stage at one frame");
    }
  }

  // ---- control track -------------------------------------------------
  void advanceControl (StageControl& c,
                       const std::vector<std::size_t>& evs,
                       std::uint64_t frame)
  {
    if (c.nextEvent < evs.size () && events[evs[c.nextEvent]].frame == frame)
    {
      const std::size_t ei = evs[c.nextEvent];
      const Event& e = events[ei];
      if (frame == 0 || e.transition == 0)
      {
        c.curCutoff = e.cutoff;
        c.curQ = e.q;
        c.active = false;
      }
      else
      {
        c.active = true;
        c.startFrame = frame;
        c.len = e.transition;
        c.startCutoff = c.curCutoff;
        c.startQ = c.curQ;
        c.targetCutoff = e.cutoff;
        c.targetQ = e.q;
        const double f = 1.0 / double (c.len);
        c.curCutoff = c.startCutoff + (c.targetCutoff - c.startCutoff) * f;
        c.curQ      = c.startQ      + (c.targetQ      - c.startQ)      * f;
      }
      ++c.nextEvent;
      return;
    }

    if (c.active)
    {
      if (frame >= c.startFrame + c.len)
      {
        c.curCutoff = c.targetCutoff;
        c.curQ = c.targetQ;
        c.active = false;
      }
      else
      {
        const double f = double (frame - c.startFrame + 1) / double (c.len);
        c.curCutoff = c.startCutoff + (c.targetCutoff - c.startCutoff) * f;
        c.curQ      = c.startQ      + (c.targetQ      - c.startQ)      * f;
      }
    }
  }

  // Reconstructs the reverse-pass classification for `frame` from the event
  // list. `cursor` is the index into evs of the latest event with
  // frame <= the current frame.
  ControlInfo reverseInfoAt (std::size_t stage,
                             std::size_t cursor,
                             std::uint64_t frame) const
  {
    const std::vector<std::size_t>& evs = stageEvents[stage];
    const std::size_t ei = evs[cursor];
    const Event& e = events[ei];
    ControlInfo info;
    info.owner = ei;

    if (e.frame == frame && frame != 0 && e.transition > 0)
    {
      info.kind = ControlRamp;
      info.frac = 1.0 / double (e.transition);
    }
    else if (frame > e.frame && e.transition > 0
             && frame < e.frame + std::uint64_t (e.transition))
    {
      info.kind = ControlRamp;
      info.frac = double (frame - e.frame + 1) / double (e.transition);
    }
    else
    {
      // Held value; if the event lands on this exact frame it is immediate.
      info.kind = (e.frame == frame) ? ControlImmediate : ControlHeld;
      info.frac = 1.0;
    }
    return info;
  }

  // ---- checkpoints ---------------------------------------------------
  void addCheckpoint (const Snapshot& snapshot)
  {
    checkpoints.push_back (snapshot);
    checkpointScalars +=
      snapshot.audio.size () * 2 + snapshot.control.size () * kControlScalars;
    peakSavedScalars = std::max (peakSavedScalars, checkpointScalars);
  }

  void captureCheckpointAt (std::uint64_t frame)
  {
    const std::size_t index = std::size_t (frame / checkpointK);
    if (index < checkpoints.size ())
      return;
    if (index != checkpoints.size ())
      throw std::logic_error ("Dsp::LowPassTape: non-contiguous checkpoint");

    Snapshot snap;
    snap.audio.resize (numSt * numCh);
    for (std::size_t i = 0; i < numSt * numCh; ++i)
      snap.audio[i] = CellState {live[i].getV1 (), live[i].getV2 ()};
    snap.control = control;
    addCheckpoint (snap);
  }

  Snapshot snapshotAt (std::size_t frame) const
  {
    const std::size_t index = frame / checkpointK;
    if (index >= checkpoints.size () || checkpoints[index].audio.size () != numSt * numCh)
      throw std::logic_error ("Dsp::LowPassTape: missing checkpoint for replay");
    return checkpoints[index];
  }

  // ---- recording -----------------------------------------------------
  void push (const double* const* input,
             std::size_t numFrames,
             double* const* output)
  {
    if (isSealed)
      throw std::logic_error ("Dsp::LowPassTape: push() after seal()");
    if (numFrames == 0)
      return;
    if (!input || !output)
      throw std::invalid_argument ("Dsp::LowPassTape: null channel array");

    // Validate everything before touching the tape or the output buffer.
    for (std::size_t c = 0; c < numCh; ++c)
    {
      if (!input[c] || !output[c])
        throw std::invalid_argument ("Dsp::LowPassTape: null channel pointer");
      for (std::size_t n = 0; n < numFrames; ++n)
      {
        const double v = input[c][n];
        if (!std::isfinite (v) || std::abs (v) > kMaxAbsInput)
          throw std::out_of_range ("Dsp::LowPassTape: input sample out of range");
      }
    }

    const std::size_t oldFrames = frames ();
    const std::size_t newFrames = oldFrames + numFrames;
    inputTape.resize (newFrames * numCh);
    for (std::size_t n = 0; n < numFrames; ++n)
      for (std::size_t c = 0; c < numCh; ++c)
        inputTape[(oldFrames + n) * numCh + c] = input[c][n];

    for (std::size_t n = 0; n < numFrames; ++n)
    {
      const std::uint64_t frame = clock;
      for (std::size_t s = 0; s < numSt; ++s)
        advanceControl (control[s], stageEvents[s], frame);

      const double vsa = denormalBias (frame);
      for (std::size_t c = 0; c < numCh; ++c)
      {
        double x = inputTape[(oldFrames + n) * numCh + c];
        for (std::size_t s = 0; s < numSt; ++s)
        {
          RBJ::LowPass section;
          section.setup (sampleRate, control[s].curCutoff, control[s].curQ);
          x = live[s * numCh + c].process1 (x, section, vsa);
        }
        output[c][n] = x;
      }

      ++clock;
      if (clock % checkpointK == 0)
        captureCheckpointAt (clock);
    }
  }

  void seal ()
  {
    isSealed = true;
  }

  // ---- backward -------------------------------------------------------
  Gradients backward (const double* const* outputAdjoint)
  {
    if (!isSealed)
      throw std::logic_error ("Dsp::LowPassTape: backward() before seal()");

    const std::size_t N = frames ();
    Gradients g;
    g.input.assign (numCh, std::vector<double> (N, 0.0));
    g.cutoff.assign (events.size (), 0.0);
    g.q.assign (events.size (), 0.0);
    if (N == 0)
    {
      lastReplayFrames = 0;
      return g;
    }

    if (!outputAdjoint)
      throw std::invalid_argument ("Dsp::LowPassTape: null adjoint array");
    for (std::size_t c = 0; c < numCh; ++c)
    {
      if (!outputAdjoint[c])
        throw std::invalid_argument ("Dsp::LowPassTape: null adjoint channel");
      for (std::size_t n = 0; n < N; ++n)
        if (!std::isfinite (outputAdjoint[c][n]))
          throw std::out_of_range ("Dsp::LowPassTape: non-finite output adjoint");
    }

    const std::size_t blockSize = std::min (N, checkpointK);
    const std::size_t numBlocks = (N + blockSize - 1) / blockSize;

    // Audio adjoint registers, indexed [stage * channels + channel].
    // aCur holds the contribution to v[n] coming from frame n+1;
    // aPrev holds the contribution to v[n-1] coming from frame n.
    std::vector<double> aCur (numSt * numCh, 0.0);
    std::vector<double> aPrev (numSt * numCh, 0.0);

    // Control-track adjoint: a pending dependency on the value at the anchor
    // frame (event frame - 1) of a ramp.
    std::vector<double> pendCut (numSt, 0.0);
    std::vector<double> pendQ (numSt, 0.0);
    std::vector<std::uint64_t> pendFrame (numSt, 0);
    std::vector<char> hasPend (numSt, 0);

    std::vector<std::size_t> eventCursor (numSt, 0);
    for (std::size_t s = 0; s < numSt; ++s)
      eventCursor[s] = stageEvents[s].size () - 1;

    std::vector<double> dCut (numSt, 0.0);
    std::vector<double> dQ (numSt, 0.0);

    std::size_t replayFrames = 0;

    for (std::size_t b = numBlocks; b-- > 0;)
    {
      const std::size_t blockStart = b * blockSize;
      const std::size_t blockEnd = std::min (N, blockStart + blockSize);
      const std::size_t L = blockEnd - blockStart;

      Snapshot snapshot = snapshotAt (blockStart);
      std::vector<DirectFormII> state (numSt * numCh);
      for (std::size_t i = 0; i < numSt * numCh; ++i)
        state[i].setV1V2 (snapshot.audio[i].v1, snapshot.audio[i].v2);
      std::vector<StageControl> ctl = snapshot.control;

      // Local activations for this block only.
      std::vector<double> wAct (L * numSt * numCh, 0.0);
      std::vector<double> cutVal (L * numSt, 0.0);
      std::vector<double> qVal (L * numSt, 0.0);

      // Forward replay of the block through the production filter.
      for (std::size_t l = 0; l < L; ++l)
      {
        const std::uint64_t frame = std::uint64_t (blockStart + l);
        for (std::size_t s = 0; s < numSt; ++s)
        {
          advanceControl (ctl[s], stageEvents[s], frame);
          cutVal[l * numSt + s] = ctl[s].curCutoff;
          qVal[l * numSt + s] = ctl[s].curQ;
        }

        const double vsa = denormalBias (frame);
        for (std::size_t c = 0; c < numCh; ++c)
        {
          double x = inputTape[(blockStart + l) * numCh + c];
          for (std::size_t s = 0; s < numSt; ++s)
          {
            RBJ::LowPass section;
            section.setup (sampleRate, cutVal[l * numSt + s], qVal[l * numSt + s]);
            x = state[s * numCh + c].process1 (x, section, vsa);
            wAct[(l * numSt + s) * numCh + c] = state[s * numCh + c].getV1 ();
          }
        }
      }
      replayFrames += L;

      double a1v[4], a2v[4], b0v[4], b1v[4], b2v[4];
      double pc[4][5], pq[4][5];

      for (std::size_t l = L; l-- > 0;)
      {
        const std::uint64_t frame = std::uint64_t (blockStart + l);

        for (std::size_t s = 0; s < numSt; ++s)
        {
          std::vector<std::size_t>& evs = stageEvents[s];
          while (eventCursor[s] > 0 && events[evs[eventCursor[s]]].frame > frame)
            --eventCursor[s];

          RBJ::LowPass section;
          section.setup (sampleRate, cutVal[l * numSt + s], qVal[l * numSt + s]);
          a1v[s] = section.m_a1;
          a2v[s] = section.m_a2;
          b0v[s] = section.m_b0;
          b1v[s] = section.m_b1;
          b2v[s] = section.m_b2;
          coefficientDerivatives (sampleRate, cutVal[l * numSt + s],
                                  qVal[l * numSt + s], pc[s], pq[s]);
          dCut[s] = 0.0;
          dQ[s] = 0.0;
        }

        for (std::size_t c = 0; c < numCh; ++c)
        {
          double dY = outputAdjoint[c][blockStart + l];
          for (std::size_t s = numSt; s-- > 0;)
          {
            const std::size_t cell = s * numCh + c;
            const double w = wAct[(l * numSt + s) * numCh + c];

            double v1;
            double v2;
            if (l > 0)
              v1 = wAct[((l - 1) * numSt + s) * numCh + c];
            else
              v1 = snapshot.audio[cell].v1;

            if (l >= 2)
              v2 = wAct[((l - 2) * numSt + s) * numCh + c];
            else if (l == 1)
              v2 = snapshot.audio[cell].v1;
            else
              v2 = snapshot.audio[cell].v2;

            // DFII: v[n] = x[n] - A1*v[n-1] - A2*v[n-2] + vsa
            //       y[n] = B0*v[n] + B1*v[n-1] + B2*v[n-2]
            const double dv = aCur[cell] + dY * b0v[s];
            const double dx = dv;

            const double toPrev1 = dY * b1v[s] - dv * a1v[s];
            const double toPrev2 = dY * b2v[s] - dv * a2v[s];
            aCur[cell] = aPrev[cell] + toPrev1;
            aPrev[cell] = toPrev2;

            const double dA1 = -dv * v1;
            const double dA2 = -dv * v2;
            const double dB0 = dY * w;
            const double dB1 = dY * v1;
            const double dB2 = dY * v2;

            dCut[s] += dA1 * pc[s][0] + dA2 * pc[s][1] + dB0 * pc[s][2]
                     + dB1 * pc[s][3] + dB2 * pc[s][4];
            dQ[s]   += dA1 * pq[s][0] + dA2 * pq[s][1] + dB0 * pq[s][2]
                     + dB1 * pq[s][3] + dB2 * pq[s][4];

            dY = dx;
          }
          g.input[c][blockStart + l] = dY;
        }

        // Reverse the control track for this frame.
        for (std::size_t s = 0; s < numSt; ++s)
        {
          double advC = dCut[s];
          double advQ = dQ[s];
          if (hasPend[s] && pendFrame[s] == frame)
          {
            advC += pendCut[s];
            advQ += pendQ[s];
            pendCut[s] = 0.0;
            pendQ[s] = 0.0;
            hasPend[s] = 0;
          }

          const ControlInfo info = reverseInfoAt (s, eventCursor[s], frame);
          if (info.kind == ControlRamp)
          {
            g.cutoff[info.owner] += info.frac * advC;
            g.q[info.owner]      += info.frac * advQ;

            // The ramp starts from the value used at (event frame - 1).
            const std::uint64_t anchor = events[info.owner].frame - 1;
            if (hasPend[s] && pendFrame[s] != anchor)
              throw std::logic_error ("Dsp::LowPassTape: control adjoint state");
            pendCut[s] += (1.0 - info.frac) * advC;
            pendQ[s]   += (1.0 - info.frac) * advQ;
            pendFrame[s] = anchor;
            hasPend[s] = 1;
          }
          else
          {
            g.cutoff[info.owner] += advC;
            g.q[info.owner]      += advQ;
          }
        }
      }

      const std::size_t localScalars =
        wAct.size () + cutVal.size () + qVal.size ()
        + numSt * numCh * 4          // aCur, aPrev, replay state, spare
        + numSt * 16;                // control registers, cursors, pending
      peakSavedScalars = std::max (peakSavedScalars, checkpointScalars + localScalars);
    }

    lastReplayFrames = replayFrames;
    maxReplayFrames = std::max (maxReplayFrames, replayFrames);
    return g;
  }
};

//------------------------------------------------------------------------------

LowPassTape::LowPassTape (double sampleRate,
                          std::size_t numChannels,
                          std::size_t numStages,
                          std::vector<Event> events,
                          std::size_t checkpointInterval)
  : impl_ (new Impl (sampleRate, numChannels, numStages,
                     std::move (events), checkpointInterval))
{
}

LowPassTape::LowPassTape (std::vector<Event> events,
                          double sampleRate,
                          std::size_t numChannels,
                          std::size_t numStages,
                          std::size_t checkpointInterval)
  : impl_ (new Impl (sampleRate, numChannels, numStages,
                     std::move (events), checkpointInterval))
{
}

LowPassTape::~LowPassTape () = default;
LowPassTape::LowPassTape (LowPassTape&&) noexcept = default;
LowPassTape& LowPassTape::operator= (LowPassTape&&) noexcept = default;

void LowPassTape::push (const double* const* input,
                        std::size_t numFrames,
                        double* const* output)
{
  impl_->push (input, numFrames, output);
}

void LowPassTape::seal ()
{
  impl_->seal ();
}

LowPassTape::Gradients LowPassTape::backward (const double* const* outputAdjoint)
{
  return impl_->backward (outputAdjoint);
}

bool LowPassTape::sealed () const noexcept { return impl_->isSealed; }
std::size_t LowPassTape::numFrames () const noexcept { return impl_->frames (); }
std::size_t LowPassTape::numChannels () const noexcept { return impl_->numCh; }
std::size_t LowPassTape::numStages () const noexcept { return impl_->numSt; }
std::size_t LowPassTape::numEvents () const noexcept { return impl_->events.size (); }
std::size_t LowPassTape::checkpointInterval () const noexcept { return impl_->checkpointK; }
std::size_t LowPassTape::lastReplayFrames () const noexcept { return impl_->lastReplayFrames; }
std::size_t LowPassTape::maxReplayFrames () const noexcept { return impl_->maxReplayFrames; }
std::size_t LowPassTape::peakSavedStateCount () const noexcept
{
  return impl_->peakSavedScalars;
}
std::size_t LowPassTape::savedStateCount () const noexcept
{
  return impl_->checkpointScalars;
}

} // namespace Dsp
