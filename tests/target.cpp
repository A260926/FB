// Tests for Dsp::LowPassTape (DSPFilters).
//
// Forward   : the new entry point is compared, sample by sample, against the
//             existing RBJ::LowPass design + DirectFormII recursion, and
//             against 1 / 7 / 127 frame block splits.
// Gradients : analytic input and event derivatives are compared against
//             central differences of an independent per-sample simulation.
// Replay    : checkpoint intervals, block splits, repeated / different
//             adjoints, diagnostics, empty segments and failure recovery.

#include "DspFilters/LowPassTape.h"
#include "DspFilters/RBJ.h"
#include "DspFilters/State.h"
#include "DspFilters/MathSupplement.h"

#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

using Dsp::LowPassTape;
using Dsp::LowPassTapeEvent;

//------------------------------------------------------------------------------
// Independent reference: production RBJ coefficients and production
// DirectFormII, driven by a straightforward per-frame event interpreter.

namespace {

struct RefStage
{
  double c;
  double q;
  long remaining;     // ramp steps still to be taken after this frame
  double stepC;
  double stepQ;

  RefStage () : c (0), q (0), remaining (0), stepC (0), stepQ (0) {}
};

std::vector<std::vector<double> >
referenceRender (double sampleRate,
                 std::size_t channels,
                 std::size_t stages,
                 std::size_t frames,
                 const std::vector<LowPassTapeEvent>& events,
                 const std::vector<std::vector<double> >& input)
{
  std::vector<std::vector<double> > output (channels,
                                            std::vector<double> (frames, 0.0));

  // Per-stage event lists (events are globally sorted, hence stage sorted).
  std::vector<std::vector<const LowPassTapeEvent*> > perStage (stages);
  for (std::size_t i = 0; i < events.size (); ++i)
    perStage[events[i].stage].push_back (&events[i]);

  std::vector<std::size_t> epos (stages, 0);
  std::vector<RefStage> rs (stages);
  std::vector<Dsp::RBJ::LowPass> filters (stages);
  std::vector<std::vector<Dsp::DirectFormII> > state (
    channels, std::vector<Dsp::DirectFormII> (stages));

  Dsp::DenormalPrevention denormal;

  for (std::size_t k = 0; k < frames; ++k)
  {
    for (std::size_t s = 0; s < stages; ++s)
    {
      const std::vector<const LowPassTapeEvent*>& list = perStage[s];
      bool consumed = false;

      if (epos[s] < list.size () && list[epos[s]]->frame == k)
      {
        const LowPassTapeEvent& e = *list[epos[s]++];
        consumed = true;

        if (k == 0 || e.transition == 0)
        {
          rs[s].c = e.cutoff;
          rs[s].q = e.q;
          rs[s].remaining = 0;
        }
        else
        {
          const double t = static_cast<double> (e.transition);
          rs[s].stepC = (e.cutoff - rs[s].c) / t;
          rs[s].stepQ = (e.q - rs[s].q) / t;
          rs[s].c += rs[s].stepC;
          rs[s].q += rs[s].stepQ;
          rs[s].remaining = static_cast<long> (e.transition) - 1;
        }
      }

      if (!consumed && rs[s].remaining > 0)
      {
        rs[s].c += rs[s].stepC;
        rs[s].q += rs[s].stepQ;
        --rs[s].remaining;
      }

      filters[s].setup (sampleRate, rs[s].c, rs[s].q);
    }

    const double vsa = denormal.ac ();

    for (std::size_t ch = 0; ch < channels; ++ch)
    {
      double v = input[ch][k];
      for (std::size_t s = 0; s < stages; ++s)
        v = state[ch][s].process1 (v, filters[s], vsa);
      output[ch][k] = v;
    }
  }

  return output;
}

double referenceLoss (double sampleRate,
                      std::size_t channels,
                      std::size_t stages,
                      std::size_t frames,
                      const std::vector<LowPassTapeEvent>& events,
                      const std::vector<std::vector<double> >& input,
                      const std::vector<std::vector<double> >& adjoint)
{
  std::vector<std::vector<double> > y =
    referenceRender (sampleRate, channels, stages, frames, events, input);

  double sum = 0;
  for (std::size_t ch = 0; ch < channels; ++ch)
    for (std::size_t n = 0; n < frames; ++n)
      sum += adjoint[ch][n] * y[ch][n];
  return sum;
}

std::vector<std::vector<double> >
makeSignal (std::size_t channels, std::size_t frames, double scale)
{
  std::vector<std::vector<double> > x (channels,
                                       std::vector<double> (frames, 0.0));
  for (std::size_t ch = 0; ch < channels; ++ch)
    for (std::size_t n = 0; n < frames; ++n)
      x[ch][n] = scale * (std::sin (0.31 * n + 0.7 * ch) +
                          0.35 * std::cos (0.071 * n * (ch + 1)));
  return x;
}

std::vector<std::vector<double> >
makeAdjoint (std::size_t channels, std::size_t frames)
{
  std::vector<std::vector<double> > a (channels,
                                       std::vector<double> (frames, 0.0));
  for (std::size_t ch = 0; ch < channels; ++ch)
    for (std::size_t n = 0; n < frames; ++n)
      a[ch][n] = std::sin (0.13 * n + 0.9 * ch) -
                 0.2 * std::cos (0.29 * n - 0.4 * ch);
  return a;
}

std::vector<const double*> channelPointers (
  const std::vector<std::vector<double> >& x)
{
  std::vector<const double*> p (x.size ());
  for (std::size_t ch = 0; ch < x.size (); ++ch)
    p[ch] = x[ch].data ();
  return p;
}

std::vector<const double*> constPointers (
  std::vector<std::vector<double> >& x)
{
  std::vector<const double*> p (x.size ());
  for (std::size_t ch = 0; ch < x.size (); ++ch)
    p[ch] = x[ch].data ();
  return p;
}

std::vector<double*> channelPointers (std::vector<std::vector<double> >& x)
{
  std::vector<double*> p (x.size ());
  for (std::size_t ch = 0; ch < x.size (); ++ch)
    p[ch] = x[ch].data ();
  return p;
}

LowPassTapeEvent ev (std::uint64_t frame, std::size_t stage,
                     double cutoff, double q, std::uint32_t transition)
{
  LowPassTapeEvent e;
  e.frame = frame;
  e.stage = stage;
  e.cutoff = cutoff;
  e.q = q;
  e.transition = transition;
  return e;
}

// Render through the new class, pushing in the requested blocks.
struct TapeResult
{
  std::vector<std::vector<double> > output;
  LowPassTape::Gradients gradients;
  std::size_t checkpointCount;
  std::size_t checkpointStateCount;
  std::size_t peakLocalActivations;
  std::size_t peakSavedStateCount;
  std::size_t lastReplayFrames;
  std::size_t peakReplayFrames;
};

TapeResult runTape (double sampleRate,
                    std::size_t channels,
                    std::size_t stages,
                    std::size_t K,
                    const std::vector<LowPassTapeEvent>& events,
                    const std::vector<std::vector<double> >& input,
                    const std::vector<std::vector<double> >& adjoint,
                    const std::vector<std::size_t>& blocks)
{
  const std::size_t frames = input.empty () ? 0 : input[0].size ();

  LowPassTape tape (sampleRate, channels, stages, K, events);

  TapeResult r;
  r.output.assign (channels, std::vector<double> (frames, 0.0));

  std::size_t pos = 0;
  std::size_t bi = 0;
  while (pos < frames)
  {
    std::size_t len = (bi < blocks.size ()) ? blocks[bi] : frames - pos;
    if (len > frames - pos)
      len = frames - pos;

    std::vector<const double*> in (channels);
    std::vector<double*> out (channels);
    for (std::size_t ch = 0; ch < channels; ++ch)
    {
      in[ch] = input[ch].data () + pos;
      out[ch] = r.output[ch].data () + pos;
    }

    tape.push (in.data (), len, out.data ());
    pos += len;
    ++bi;
  }

  tape.seal ();
  r.gradients = tape.backward (adjoint.empty () ? 0 : channelPointers (adjoint).data ());

  r.checkpointCount = tape.checkpointCount ();
  r.checkpointStateCount = tape.checkpointStateCount ();
  r.peakLocalActivations = tape.peakLocalActivations ();
  r.peakSavedStateCount = tape.peakSavedStateCount ();
  r.lastReplayFrames = tape.lastReplayFrameCount ();
  r.peakReplayFrames = tape.peakReplayFrameCount ();
  return r;
}

bool closeWithin (double got, double reference)
{
  return std::fabs (got - reference) <=
         2e-7 + 2e-5 * std::fabs (reference);
}

bool inputsFiniteAndNonZero (const std::vector<std::vector<double> >& y,
                             std::size_t frames)
{
  double energy = 0;
  for (std::size_t ch = 0; ch < y.size (); ++ch)
    for (std::size_t n = 0; n < frames; ++n)
    {
      if (!std::isfinite (y[ch][n]))
        return false;
      energy += std::fabs (y[ch][n]);
    }
  return energy > 0;
}

} // namespace

//==============================================================================
// Forward

static int testForwardSingleStageStatic ()
{
  const double sr = 48000;
  const std::size_t ch = 1, st = 1, n = 257;

  std::vector<LowPassTapeEvent> events;
  events.push_back (ev (0, 0, 1500, 0.9, 0));

  std::vector<std::vector<double> > x = makeSignal (ch, n, 0.4);
  std::vector<std::vector<double> > adj = makeAdjoint (ch, n);

  TapeResult r = runTape (sr, ch, st, 64, events, x, adj,
                          std::vector<std::size_t> (1, n));
  std::vector<std::vector<double> > ref =
    referenceRender (sr, ch, st, n, events, x);

  TASK_CHECK (r.output[0].size () == n);
  TASK_CHECK (inputsFiniteAndNonZero (r.output, n));

  double maxDiff = 0;
  for (std::size_t i = 0; i < n; ++i)
    maxDiff = std::max (maxDiff, std::fabs (r.output[0][i] - ref[0][i]));
  TASK_CHECK (maxDiff <= 1e-13);
  return 0;
}

static int testForwardFourStagesEvents ()
{
  const double sr = 44100;
  const std::size_t ch = 2, st = 4, n = 503;

  std::vector<LowPassTapeEvent> events;
  events.push_back (ev (0, 0, 800, 0.6, 0));
  events.push_back (ev (0, 1, 2200, 1.1, 0));
  events.push_back (ev (0, 2, 3500, 0.75, 0));
  events.push_back (ev (0, 3, 5200, 1.6, 0));
  events.push_back (ev (37, 1, 900, 1.9, 21));
  events.push_back (ev (51, 1, 4000, 0.55, 9));   // interrupts the ramp
  events.push_back (ev (120, 2, 1200, 2.0, 1));   // one frame transition
  events.push_back (ev (201, 0, 4200, 1.3, 0));
  events.push_back (ev (260, 3, 700, 0.5, 47));
  events.push_back (ev (401, 2, 6000, 1.0, 180)); // extends past the end

  std::vector<std::vector<double> > x = makeSignal (ch, n, 0.35);
  std::vector<std::vector<double> > adj = makeAdjoint (ch, n);

  TapeResult r = runTape (sr, ch, st, 37, events, x, adj,
                          std::vector<std::size_t> (1, n));
  std::vector<std::vector<double> > ref =
    referenceRender (sr, ch, st, n, events, x);

  TASK_CHECK (inputsFiniteAndNonZero (r.output, n));

  double maxDiff = 0;
  for (std::size_t c = 0; c < ch; ++c)
    for (std::size_t i = 0; i < n; ++i)
      maxDiff = std::max (maxDiff, std::fabs (r.output[c][i] - ref[c][i]));
  TASK_CHECK (maxDiff <= 1e-12);
  return 0;
}

static int testForwardBlockSplits ()
{
  const double sr = 96000;
  const std::size_t ch = 6, st = 3, n = 401;

  std::vector<LowPassTapeEvent> events;
  events.push_back (ev (0, 0, 1200, 0.8, 0));
  events.push_back (ev (0, 1, 9000, 1.4, 0));
  events.push_back (ev (0, 2, 18000, 0.9, 0));
  events.push_back (ev (13, 0, 8000, 1.7, 33));
  events.push_back (ev (70, 1, 1500, 0.5, 0));
  events.push_back (ev (150, 2, 3000, 1.95, 12));
  events.push_back (ev (151, 0, 3000, 1.2, 3));

  std::vector<std::vector<double> > x = makeSignal (ch, n, 0.3);
  std::vector<std::vector<double> > adj = makeAdjoint (ch, n);

  TapeResult whole = runTape (sr, ch, st, 32, events, x, adj,
                              std::vector<std::size_t> ());

  const std::size_t sizes[3] = {1, 7, 127};
  for (int i = 0; i < 3; ++i)
  {
    TapeResult split = runTape (sr, ch, st, 32, events, x, adj,
                                std::vector<std::size_t> (1, sizes[i]));
    double maxDiff = 0;
    for (std::size_t c = 0; c < ch; ++c)
      for (std::size_t k = 0; k < n; ++k)
        maxDiff = std::max (maxDiff,
                            std::fabs (whole.output[c][k] - split.output[c][k]));
    TASK_CHECK (maxDiff == 0);
  }

  // Mixed block pattern.
  std::vector<std::size_t> mixed;
  for (int i = 0; i < 30; ++i)
    mixed.push_back (static_cast<std::size_t> (1 + (i * 7) % 31));
  TapeResult m = runTape (sr, ch, st, 32, events, x, adj, mixed);
  double maxDiff = 0;
  for (std::size_t c = 0; c < ch; ++c)
    for (std::size_t k = 0; k < n; ++k)
      maxDiff = std::max (maxDiff,
                          std::fabs (whole.output[c][k] - m.output[c][k]));
  TASK_CHECK (maxDiff == 0);
  return 0;
}

static int testForwardZeroFramePush ()
{
  const double sr = 48000;
  const std::size_t ch = 2, st = 2, n = 32;

  std::vector<LowPassTapeEvent> events;
  events.push_back (ev (0, 0, 1000, 0.7, 0));
  events.push_back (ev (0, 1, 2000, 1.0, 0));

  LowPassTape tape (sr, ch, st, 8, events);

  // A zero frame block must not touch either pointer nor advance the clock.
  tape.push (0, 0, 0);
  TASK_CHECK (tape.framesSeen () == 0);

  std::vector<std::vector<double> > x = makeSignal (ch, n, 0.2);
  std::vector<std::vector<double> > y (ch, std::vector<double> (n, 7.0));

  std::vector<const double*> in = constPointers (x);
  std::vector<double*> out = channelPointers (y);
  tape.push (in.data (), n, out.data ());
  TASK_CHECK (tape.framesSeen () == n);
  TASK_CHECK (inputsFiniteAndNonZero (y, n));
  return 0;
}

static int testForwardInPlaceAndSampleRates ()
{
  const double rates[3] = {44100, 48000, 96000};
  const std::size_t n = 211;

  for (int ri = 0; ri < 3; ++ri)
  {
    const double sr = rates[ri];
    const std::size_t ch = 2, st = 2;

    std::vector<LowPassTapeEvent> events;
    events.push_back (ev (0, 0, 0.15 * sr, 1.3, 0));
    events.push_back (ev (0, 1, 0.05 * sr, 0.6, 0));
    events.push_back (ev (40, 0, 400, 1.9, 25));

    std::vector<std::vector<double> > x = makeSignal (ch, n, 0.25);
    std::vector<std::vector<double> > adj = makeAdjoint (ch, n);

    // In place: output pointers alias the input buffers.
    std::vector<std::vector<double> > y = x;
    LowPassTape tape (sr, ch, st, 16, events);
    std::vector<double*> io = channelPointers (y);
    tape.push (io.data (), n, io.data ());
    tape.seal ();

    std::vector<std::vector<double> > ref =
      referenceRender (sr, ch, st, n, events, x);

    double maxDiff = 0;
    for (std::size_t c = 0; c < ch; ++c)
      for (std::size_t k = 0; k < n; ++k)
        maxDiff = std::max (maxDiff, std::fabs (y[c][k] - ref[c][k]));
    TASK_CHECK (maxDiff <= 1e-13);
  }
  return 0;
}

//==============================================================================
// Gradients

namespace {

// Central differences of the independent reference simulation, compared with
// the analytic derivatives from the tape.
int checkGradients (double sampleRate,
                    std::size_t channels,
                    std::size_t stages,
                    std::size_t K,
                    const std::vector<LowPassTapeEvent>& events,
                    const std::vector<std::vector<double> >& input,
                    const std::vector<std::vector<double> >& adj)
{
  const std::size_t frames = input[0].size ();

  TapeResult r = runTape (sampleRate, channels, stages, K, events, input, adj,
                          std::vector<std::size_t> (1, frames));

  // Event parameter derivatives.
  for (std::size_t e = 0; e < events.size (); ++e)
  {
    {
      std::vector<LowPassTapeEvent> pe = events;
      std::vector<LowPassTapeEvent> me = events;
      const double h = 1e-6 * (1.0 + std::fabs (events[e].cutoff));
      pe[e].cutoff += h;
      me[e].cutoff -= h;

      const double ref =
        (referenceLoss (sampleRate, channels, stages, frames, pe, input, adj) -
         referenceLoss (sampleRate, channels, stages, frames, me, input, adj)) /
        (2 * h);
      TASK_CHECK (closeWithin (r.gradients.cutoffGradient (e), ref));
    }
    {
      std::vector<LowPassTapeEvent> pe = events;
      std::vector<LowPassTapeEvent> me = events;
      const double h = 1e-6 * (1.0 + std::fabs (events[e].q));
      pe[e].q += h;
      me[e].q -= h;

      const double ref =
        (referenceLoss (sampleRate, channels, stages, frames, pe, input, adj) -
         referenceLoss (sampleRate, channels, stages, frames, me, input, adj)) /
        (2 * h);
      TASK_CHECK (closeWithin (r.gradients.qGradient (e), ref));
    }
  }

  // Input sample derivatives.
  for (std::size_t ch = 0; ch < channels; ++ch)
  {
    for (std::size_t n = 0; n < frames; ++n)
    {
      const double h = 1e-6 * (1.0 + std::fabs (input[ch][n]));

      std::vector<std::vector<double> > xp = input;
      std::vector<std::vector<double> > xm = input;
      xp[ch][n] += h;
      xm[ch][n] -= h;

      const double ref =
        (referenceLoss (sampleRate, channels, stages, frames, events, xp, adj) -
         referenceLoss (sampleRate, channels, stages, frames, events, xm, adj)) /
        (2 * h);

      TASK_CHECK (closeWithin (r.gradients.inputAt (ch, n), ref));
    }
  }

  return 0;
}

} // namespace

static int testGradientsSingleStage ()
{
  const double sr = 48000;
  const std::size_t ch = 1, st = 1, n = 40;

  std::vector<LowPassTapeEvent> events;
  events.push_back (ev (0, 0, 1800, 0.8, 0));
  events.push_back (ev (9, 0, 3600, 1.6, 11));
  events.push_back (ev (14, 0, 900, 0.55, 5)); // retarget mid-ramp
  events.push_back (ev (30, 0, 2500, 1.9, 0)); // zero length transition

  std::vector<std::vector<double> > x = makeSignal (ch, n, 0.5);
  std::vector<std::vector<double> > adj = makeAdjoint (ch, n);

  return checkGradients (sr, ch, st, 8, events, x, adj);
}

static int testGradientsSixChannelsTwoStages ()
{
  const double sr = 48000;
  const std::size_t ch = 6, st = 2, n = 46;

  std::vector<LowPassTapeEvent> events;
  events.push_back (ev (0, 0, 1200, 0.7, 0));
  events.push_back (ev (0, 1, 2500, 1.0, 0));
  events.push_back (ev (7, 1, 700, 1.8, 24));    // second stage ramp
  events.push_back (ev (12, 0, 4000, 1.5, 10));  // first stage ramp
  events.push_back (ev (17, 1, 5200, 0.6, 6));   // interrupt second stage
  events.push_back (ev (24, 0, 1000, 1.05, 4));  // interrupt first stage
  events.push_back (ev (33, 1, 4500, 1.2, 0));   // immediate
  events.push_back (ev (38, 0, 6000, 1.4, 90));  // tail target never reached

  std::vector<std::vector<double> > x = makeSignal (ch, n, 0.3);
  std::vector<std::vector<double> > adj = makeAdjoint (ch, n);

  return checkGradients (sr, ch, st, 6, events, x, adj);
}

static int testGradientsTailAndImmediate ()
{
  const double sr = 96000;
  const std::size_t ch = 2, st = 3, n = 29;

  std::vector<LowPassTapeEvent> events;
  events.push_back (ev (0, 0, 500, 0.9, 0));
  events.push_back (ev (0, 1, 4000, 0.5, 0));
  events.push_back (ev (0, 2, 12000, 1.9, 0));
  events.push_back (ev (12, 0, 8000, 1.1, 6));
  events.push_back (ev (15, 1, 900, 1.4, 0));
  events.push_back (ev (23, 2, 1800, 0.75, 40)); // tail, unreached target
  events.push_back (ev (25, 0, 3000, 0.6, 1));   // single frame transition

  std::vector<std::vector<double> > x = makeSignal (ch, n, 0.45);
  std::vector<std::vector<double> > adj = makeAdjoint (ch, n);

  return checkGradients (sr, ch, st, 5, events, x, adj);
}

static int testGradientsRepeatability ()
{
  const double sr = 44100;
  const std::size_t ch = 2, st = 2, n = 33;

  std::vector<LowPassTapeEvent> events;
  events.push_back (ev (0, 0, 900, 1.0, 0));
  events.push_back (ev (0, 1, 3000, 0.65, 0));
  events.push_back (ev (11, 0, 5000, 1.85, 9));
  events.push_back (ev (20, 1, 600, 1.25, 100));

  std::vector<std::vector<double> > x = makeSignal (ch, n, 0.4);
  std::vector<std::vector<double> > a1 = makeAdjoint (ch, n);
  std::vector<std::vector<double> > a2 = makeAdjoint (ch, n);
  for (std::size_t c = 0; c < ch; ++c)
    for (std::size_t k = 0; k < n; ++k)
      a2[c][k] = -0.7 * a1[c][k] + 0.1;

  LowPassTape tape (sr, ch, st, 7, events);
  std::vector<std::vector<double> > y (ch, std::vector<double> (n, 0.0));
  std::vector<const double*> in = constPointers (x);
  std::vector<double*> out = channelPointers (y);
  tape.push (in.data (), n, out.data ());
  tape.seal ();

  LowPassTape::Gradients g1 = tape.backward (channelPointers (a1).data ());
  LowPassTape::Gradients g2 = tape.backward (channelPointers (a2).data ());
  LowPassTape::Gradients g1b = tape.backward (channelPointers (a1).data ());

  TASK_CHECK (g1.input.size () == ch * n);
  TASK_CHECK (g1.eventCutoff.size () == events.size ());
  TASK_CHECK (g2.input.size () == ch * n);

  double d1 = 0, d2 = 0, maxRepeat = 0;
  for (std::size_t i = 0; i < g1.input.size (); ++i)
  {
    TASK_CHECK (std::isfinite (g1.input[i]) && std::isfinite (g2.input[i]));
    d1 += std::fabs (g1.input[i]);
    d2 += std::fabs (g2.input[i]);
    maxRepeat = std::max (maxRepeat, std::fabs (g1.input[i] - g1b.input[i]));
  }
  TASK_CHECK (d1 > 0 && d2 > 0);
  TASK_CHECK (maxRepeat == 0);
  TASK_CHECK (g1.input != g2.input);

  for (std::size_t e = 0; e < events.size (); ++e)
  {
    TASK_CHECK (std::isfinite (g1.eventCutoff[e]));
    TASK_CHECK (std::isfinite (g1.eventQ[e]));
  }
  return 0;
}

//==============================================================================
// Replay

static int testReplayCheckpointIntervals ()
{
  const double sr = 48000;
  const std::size_t ch = 6, st = 3, n = 389;

  std::vector<LowPassTapeEvent> events;
  events.push_back (ev (0, 0, 1100, 0.75, 0));
  events.push_back (ev (0, 1, 2600, 1.2, 0));
  events.push_back (ev (0, 2, 7000, 0.9, 0));
  events.push_back (ev (21, 0, 4800, 1.7, 45));
  events.push_back (ev (60, 1, 800, 0.5, 8));
  events.push_back (ev (61, 2, 1500, 1.99, 0));
  events.push_back (ev (200, 0, 1500, 1.1, 2));
  events.push_back (ev (300, 2, 9000, 1.5, 200));

  std::vector<std::vector<double> > x = makeSignal (ch, n, 0.25);
  std::vector<std::vector<double> > adj = makeAdjoint (ch, n);

  const std::size_t ks[5] = {1, 7, 16, 97, 1000};
  TapeResult base = runTape (sr, ch, st, ks[0], events, x, adj,
                             std::vector<std::size_t> ());

  TASK_CHECK (inputsFiniteAndNonZero (base.output, n));
  TASK_CHECK (base.gradients.replayFrames <= 2 * n);
  TASK_CHECK (base.lastReplayFrames == n);
  TASK_CHECK (base.peakReplayFrames <= 2 * n);

  for (int i = 1; i < 5; ++i)
  {
    const std::size_t K = ks[i];
    TapeResult r = runTape (sr, ch, st, K, events, x, adj,
                            std::vector<std::size_t> (1, 13));

    TASK_CHECK (inputsFiniteAndNonZero (r.output, n));
    TASK_CHECK (r.gradients.replayFrames <= 2 * n);
    TASK_CHECK (r.lastReplayFrames == n);

    const std::size_t expectedCheckpoints = (n + K - 1) / K;
    TASK_CHECK (r.checkpointCount == expectedCheckpoints);
    TASK_CHECK (r.checkpointStateCount == expectedCheckpoints * ch * st);
    TASK_CHECK (r.peakLocalActivations <= K * ch * st);
    TASK_CHECK (r.peakSavedStateCount ==
                r.checkpointStateCount + r.peakLocalActivations);

    // O((N/K + K) * channels * stages) bound on saved state.
    TASK_CHECK (r.peakSavedStateCount <=
                (expectedCheckpoints + K + 1) * ch * st);

    double maxOut = 0, maxGrad = 0;
    for (std::size_t c = 0; c < ch; ++c)
      for (std::size_t k = 0; k < n; ++k)
        maxOut = std::max (maxOut,
                           std::fabs (r.output[c][k] - base.output[c][k]));
    for (std::size_t j = 0; j < base.gradients.input.size (); ++j)
      maxGrad = std::max (maxGrad,
                          std::fabs (r.gradients.input[j] -
                                     base.gradients.input[j]));
    for (std::size_t e = 0; e < events.size (); ++e)
      maxGrad = std::max (maxGrad,
                          std::fabs (r.gradients.cutoffGradient (e) -
                                     base.gradients.cutoffGradient (e)));
    TASK_CHECK (maxOut <= 1e-12);
    TASK_CHECK (maxGrad <= 1e-10);

    std::printf ("REPLAY K=%zu checkpoints=%zu saved=%zu local=%zu "
                 "replayFrames=%zu\n",
                 K, r.checkpointCount, r.peakSavedStateCount,
                 r.peakLocalActivations, r.lastReplayFrames);
  }
  return 0;
}

static int testReplayEmptySegment ()
{
  std::vector<LowPassTapeEvent> events;
  events.push_back (ev (0, 0, 1000, 0.7, 0));
  events.push_back (ev (0, 1, 2000, 1.0, 0));

  LowPassTape tape (48000, 2, 2, 16, events);
  tape.seal ();
  TASK_CHECK (tape.sealed ());
  TASK_CHECK (tape.framesSeen () == 0);

  LowPassTape::Gradients g = tape.backward (0);
  TASK_CHECK (g.frames == 0);
  TASK_CHECK (g.input.empty ());
  TASK_CHECK (g.eventCutoff.size () == events.size ());
  TASK_CHECK (g.replayFrames == 0);
  TASK_CHECK (tape.lastReplayFrameCount () == 0);
  TASK_CHECK (tape.peakReplayFrameCount () == 0);
  TASK_CHECK (tape.checkpointCount () == 0);
  TASK_CHECK (tape.checkpointStateCount () == 0);

  // sealing twice is harmless
  tape.seal ();
  TASK_CHECK (tape.sealed ());
  return 0;
}

static int testReplayFailureRecovery ()
{
  const double sr = 48000;
  const std::size_t ch = 2, st = 2, n = 64;

  std::vector<LowPassTapeEvent> events;
  events.push_back (ev (0, 0, 1000, 0.8, 0));
  events.push_back (ev (0, 1, 3000, 1.3, 0));
  events.push_back (ev (31, 0, 1500, 0.6, 10));

  std::vector<std::vector<double> > x = makeSignal (ch, n, 0.3);
  std::vector<std::vector<double> > adj = makeAdjoint (ch, n);

  LowPassTape tape (sr, ch, st, 8, events);
  std::vector<std::vector<double> > y (ch, std::vector<double> (n, 0.0));
  std::vector<const double*> in = constPointers (x);
  std::vector<double*> out = channelPointers (y);
  tape.push (in.data (), n, out.data ());

  // push before seal is fine; a bad block must throw without side effects.
  {
    std::vector<std::vector<double> > bad = makeSignal (ch, 4, 0.3);
    bad[1][2] = std::numeric_limits<double>::quiet_NaN ();
    std::vector<const double*> bi = constPointers (bad);
    std::vector<double> scratch (4 * ch, 123.0);
    std::vector<double*> bo (ch);
    for (std::size_t c = 0; c < ch; ++c) bo[c] = scratch.data () + c * 4;

    bool threw = false;
    try { tape.push (bi.data (), 4, bo.data ()); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
    TASK_CHECK (tape.framesSeen () == n);
    for (std::size_t i = 0; i < scratch.size (); ++i)
      TASK_CHECK (scratch[i] == 123.0);
  }
  {
    std::vector<std::vector<double> > bad = makeSignal (ch, 4, 0.3);
    bad[0][0] = 2.5;
    std::vector<const double*> bi = constPointers (bad);
    std::vector<double> scratch (4 * ch, 9.0);
    std::vector<double*> bo (ch);
    for (std::size_t c = 0; c < ch; ++c) bo[c] = scratch.data () + c * 4;

    bool threw = false;
    try { tape.push (bi.data (), 4, bo.data ()); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
    TASK_CHECK (tape.framesSeen () == n);
    for (std::size_t i = 0; i < scratch.size (); ++i)
      TASK_CHECK (scratch[i] == 9.0);
  }

  tape.seal ();

  // push after seal throws and leaves the recording intact.
  {
    std::vector<std::vector<double> > z = makeSignal (ch, 4, 0.1);
    std::vector<std::vector<double> > zy (ch, std::vector<double> (4, 0.0));
    std::vector<const double*> zi = constPointers (z);
    std::vector<double*> zo = channelPointers (zy);
    bool threw = false;
    try { tape.push (zi.data (), 4, zo.data ()); }
    catch (const std::logic_error&) { threw = true; }
    TASK_CHECK (threw);
    TASK_CHECK (tape.framesSeen () == n);
    for (std::size_t c = 0; c < ch; ++c)
      for (std::size_t k = 0; k < n; ++k)
        TASK_CHECK (std::isfinite (y[c][k]));
  }

  // A null adjoint throws; a valid call still succeeds afterwards.
  {
    bool threw = false;
    try { tape.backward (0); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }

  LowPassTape::Gradients before = tape.backward (channelPointers (adj).data ());

  // failed push must not alter gradients either
  {
    std::vector<std::vector<double> > bad = makeSignal (ch, 4, 0.3);
    bad[0][1] = std::numeric_limits<double>::infinity ();
    std::vector<const double*> bi = constPointers (bad);
    std::vector<std::vector<double> > tmp (ch, std::vector<double> (4, 0.0));
    std::vector<double*> bo = channelPointers (tmp);
    bool threw = false;
    try { tape.push (bi.data (), 4, bo.data ()); }
    catch (const std::exception&) { threw = true; }
    TASK_CHECK (threw);
  }

  LowPassTape::Gradients after = tape.backward (channelPointers (adj).data ());
  TASK_CHECK (before.input == after.input);
  TASK_CHECK (before.eventCutoff == after.eventCutoff);
  TASK_CHECK (before.eventQ == after.eventQ);
  return 0;
}

static int testReplayValidation ()
{
  // Construction rejects invalid specifications and event lists.
  {
    std::vector<LowPassTapeEvent> ok;
    ok.push_back (ev (0, 0, 1000, 1.0, 0));

    bool threw = false;
    try { LowPassTape t (-48000, 1, 1, 8, ok); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }
  {
    std::vector<LowPassTapeEvent> ok;
    ok.push_back (ev (0, 0, 1000, 1.0, 0));
    bool threw = false;
    try { LowPassTape t (48000, 3, 1, 8, ok); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }
  {
    std::vector<LowPassTapeEvent> ok;
    ok.push_back (ev (0, 0, 1000, 1.0, 0));
    bool threw = false;
    try { LowPassTape t (48000, 1, 5, 8, ok); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }
  {
    std::vector<LowPassTapeEvent> ok;
    ok.push_back (ev (0, 0, 1000, 1.0, 0));
    bool threw = false;
    try { LowPassTape t (48000, 1, 1, 0, ok); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }
  {
    // cutoff outside [200, 0.2*sampleRate]
    std::vector<LowPassTapeEvent> e;
    e.push_back (ev (0, 0, 100, 1.0, 0));
    bool threw = false;
    try { LowPassTape t (48000, 1, 1, 8, e); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }
  {
    std::vector<LowPassTapeEvent> e;
    e.push_back (ev (0, 0, 1000, 0.4, 0));
    bool threw = false;
    try { LowPassTape t (48000, 1, 1, 8, e); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }
  {
    // out of order / duplicate / missing initial events
    std::vector<LowPassTapeEvent> e;
    e.push_back (ev (5, 0, 1000, 1.0, 0));
    bool threw = false;
    try { LowPassTape t (48000, 1, 1, 8, e); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }
  {
    std::vector<LowPassTapeEvent> e;
    e.push_back (ev (0, 0, 1000, 1.0, 0));
    e.push_back (ev (0, 0, 1200, 1.0, 0));
    bool threw = false;
    try { LowPassTape t (48000, 1, 1, 8, e); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }
  {
    std::vector<LowPassTapeEvent> e;
    e.push_back (ev (0, 0, 1000, 1.0, 0));
    e.push_back (ev (9, 0, 1200, 1.0, 0));
    e.push_back (ev (4, 0, 900, 1.0, 0));
    bool threw = false;
    try { LowPassTape t (48000, 1, 1, 8, e); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }
  {
    // non-finite parameter
    std::vector<LowPassTapeEvent> e;
    e.push_back (ev (0, 0, 1000, 1.0, 0));
    e.push_back (ev (4, 0, std::numeric_limits<double>::infinity (), 1.0, 0));
    bool threw = false;
    try { LowPassTape t (48000, 1, 1, 8, e); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }
  {
    // event stage out of range
    std::vector<LowPassTapeEvent> e;
    e.push_back (ev (0, 0, 1000, 1.0, 0));
    e.push_back (ev (4, 2, 1000, 1.0, 0));
    bool threw = false;
    try { LowPassTape t (48000, 1, 2, 8, e); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK (threw);
  }

  // backward before seal is rejected, then works after sealing.
  {
    std::vector<LowPassTapeEvent> e;
    e.push_back (ev (0, 0, 1000, 1.0, 0));

    LowPassTape t (48000, 1, 1, 8, e);
    std::vector<std::vector<double> > x = makeSignal (1, 5, 0.2);
    std::vector<std::vector<double> > y (1, std::vector<double> (5, 0.0));
    std::vector<const double*> in = constPointers (x);
    std::vector<double*> out = channelPointers (y);
    t.push (in.data (), 5, out.data ());

    bool threw = false;
    try { t.backward (0); }
    catch (const std::logic_error&) { threw = true; }
    TASK_CHECK (threw);

    t.seal ();
    std::vector<std::vector<double> > a = makeAdjoint (1, 5);
    LowPassTape::Gradients g = t.backward (channelPointers (a).data ());
    TASK_CHECK (g.frames == 5);
    TASK_CHECK (g.input.size () == 5);
    for (std::size_t i = 0; i < g.input.size (); ++i)
      TASK_CHECK (std::isfinite (g.input[i]));
  }
  return 0;
}

//==============================================================================

extern const struct task_case task_cases[] = {
  {"Forward", "single_stage_static", testForwardSingleStageStatic},
  {"Forward", "four_stages_events", testForwardFourStagesEvents},
  {"Forward", "block_splits", testForwardBlockSplits},
  {"Forward", "zero_frame_push", testForwardZeroFramePush},
  {"Forward", "in_place_sample_rates", testForwardInPlaceAndSampleRates},

  {"Gradients", "single_stage", testGradientsSingleStage},
  {"Gradients", "six_channels_two_stages", testGradientsSixChannelsTwoStages},
  {"Gradients", "tail_and_immediate", testGradientsTailAndImmediate},
  {"Gradients", "repeatability", testGradientsRepeatability},

  {"Replay", "checkpoint_intervals", testReplayCheckpointIntervals},
  {"Replay", "empty_segment", testReplayEmptySegment},
  {"Replay", "failure_recovery", testReplayFailureRecovery},
  {"Replay", "validation", testReplayValidation},
};

extern const std::size_t task_case_count =
  sizeof (task_cases) / sizeof (task_cases[0]);
