// Tests for Dsp::LowPassTape.
//
// Groups: Forward  - compare the public entry point against an independent
//                    per-sample production RBJ / DirectFormII chain.
//         Gradients - central finite differences of an independent
//                     difference-equation reference.
//         Replay    - checkpoint/replay behaviour, chunking, repeated
//                     backward passes, empty segments and failure reuse.

#include "task_support/check.h"
#include "DspFilters/LowPassTape.h"
#include "DspFilters/RBJ.h"
#include "DspFilters/State.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <limits>

namespace {

typedef Dsp::LowPassTape Tape;

// ---------------------------------------------------------------------------
// Independent reference: one sample at a time, using the production RBJ
// design and Direct Form II recursion.

struct RefEvent
{
  std::uint64_t frame = 0;
  std::size_t stage = 0;
  double cutoff = 0.0;
  double q = 0.0;
  std::uint32_t transition = 0;
};

struct RefConfig
{
  double sampleRate = 48000.0;
  std::size_t channels = 1;
  std::size_t stages = 1;
  std::vector<RefEvent> events;
};

// Renders the whole segment. out[channel][frame].
void renderReference (const RefConfig& cfg,
                      const std::vector<std::vector<double>>& input,
                      std::vector<std::vector<double>>& out)
{
  const std::size_t N = input.empty () ? 0 : input[0].size ();
  const std::size_t ch = cfg.channels;
  const std::size_t st = cfg.stages;

  out.assign (ch, std::vector<double> (N, 0.0));

  std::vector<std::vector<std::size_t>> stageEvents (st);
  for (std::size_t i = 0; i < cfg.events.size (); ++i)
    stageEvents[cfg.events[i].stage].push_back (i);

  struct Ctl
  {
    std::size_t next = 0;
    bool active = false;
    std::uint64_t startFrame = 0;
    std::uint32_t len = 0;
    double startCut = 0, startQ = 0;
    double targetCut = 0, targetQ = 0;
    double curCut = 0, curQ = 0;
  };
  std::vector<Ctl> ctl (st);
  std::vector<Dsp::DirectFormII> state (st * ch);

  double bias = -Dsp::anti_denormal_vsa;

  for (std::size_t n = 0; n < N; ++n)
  {
    for (std::size_t s = 0; s < st; ++s)
    {
      Ctl& c = ctl[s];
      const std::vector<std::size_t>& evs = stageEvents[s];
      if (c.next < evs.size () && cfg.events[evs[c.next]].frame == n)
      {
        const RefEvent& e = cfg.events[evs[c.next]];
        if (n == 0 || e.transition == 0)
        {
          c.curCut = e.cutoff;
          c.curQ = e.q;
          c.active = false;
        }
        else
        {
          c.active = true;
          c.startFrame = n;
          c.len = e.transition;
          c.startCut = c.curCut;
          c.startQ = c.curQ;
          c.targetCut = e.cutoff;
          c.targetQ = e.q;
          const double f = 1.0 / double (c.len);
          c.curCut = c.startCut + (c.targetCut - c.startCut) * f;
          c.curQ   = c.startQ   + (c.targetQ   - c.startQ)   * f;
        }
        ++c.next;
      }
      else if (c.active)
      {
        if (n >= c.startFrame + c.len)
        {
          c.curCut = c.targetCut;
          c.curQ = c.targetQ;
          c.active = false;
        }
        else
        {
          const double f = double (n - c.startFrame + 1) / double (c.len);
          c.curCut = c.startCut + (c.targetCut - c.startCut) * f;
          c.curQ   = c.startQ   + (c.targetQ   - c.startQ)   * f;
        }
      }
    }

    for (std::size_t c = 0; c < ch; ++c)
    {
      double x = input[c][n];
      for (std::size_t s = 0; s < st; ++s)
      {
        Dsp::RBJ::LowPass section;
        section.setup (cfg.sampleRate, ctl[s].curCut, ctl[s].curQ);
        x = state[s * ch + c].process1 (x, section, bias);
      }
      out[c][n] = x;
    }
    bias = -bias;
  }
}

double lossReference (const RefConfig& cfg,
                      const std::vector<std::vector<double>>& input,
                      const std::vector<std::vector<double>>& adj)
{
  std::vector<std::vector<double>> out;
  renderReference (cfg, input, out);
  double loss = 0.0;
  for (std::size_t c = 0; c < out.size (); ++c)
    for (std::size_t n = 0; n < out[c].size (); ++n)
      loss += adj[c][n] * out[c][n];
  return loss;
}

// Independent hand-coded RBJ low-pass design plus Direct Form II recursion.
// Deliberately does not use the library filter types, so the finite-difference
// gradient reference is a genuinely separate difference equation.
void rbjLowPassCoefficients (double sr, double fc, double q,
                             double& nA1, double& nA2,
                             double& nB0, double& nB1, double& nB2)
{
  const double pi = 3.141592653589793238462643383279502884;
  const double w0 = 2.0 * pi * fc / sr;
  const double cs = std::cos (w0);
  const double sn = std::sin (w0);
  const double alpha = sn / (2.0 * q);
  const double a0 = 1.0 + alpha;
  const double a1 = -2.0 * cs;
  const double a2 = 1.0 - alpha;
  const double b0 = (1.0 - cs) / 2.0;
  const double b1 = 1.0 - cs;
  const double b2 = (1.0 - cs) / 2.0;
  nA1 = a1 / a0;
  nA2 = a2 / a0;
  nB0 = b0 / a0;
  nB1 = b1 / a0;
  nB2 = b2 / a0;
}

// Hand-coded reference used by the central-difference gradient checks.
void renderDifferenceEquation (const RefConfig& cfg,
                               const std::vector<std::vector<double>>& input,
                               std::vector<std::vector<double>>& out)
{
  const std::size_t N = input.empty () ? 0 : input[0].size ();
  const std::size_t ch = cfg.channels;
  const std::size_t st = cfg.stages;

  out.assign (ch, std::vector<double> (N, 0.0));

  std::vector<std::vector<std::size_t>> stageEvents (st);
  for (std::size_t i = 0; i < cfg.events.size (); ++i)
    stageEvents[cfg.events[i].stage].push_back (i);

  struct Ctl
  {
    std::size_t next = 0;
    bool active = false;
    std::uint64_t startFrame = 0;
    std::uint32_t len = 0;
    double startCut = 0, startQ = 0;
    double targetCut = 0, targetQ = 0;
    double curCut = 0, curQ = 0;
  };
  std::vector<Ctl> ctl (st);
  std::vector<double> delay1 (ch * st, 0.0); // v[n-1]
  std::vector<double> delay2 (ch * st, 0.0); // v[n-2]

  double bias = -Dsp::anti_denormal_vsa;

  for (std::size_t n = 0; n < N; ++n)
  {
    for (std::size_t s = 0; s < st; ++s)
    {
      Ctl& c = ctl[s];
      const std::vector<std::size_t>& evs = stageEvents[s];
      if (c.next < evs.size () && cfg.events[evs[c.next]].frame == n)
      {
        const RefEvent& e = cfg.events[evs[c.next]];
        if (n == 0 || e.transition == 0)
        {
          c.curCut = e.cutoff;
          c.curQ = e.q;
          c.active = false;
        }
        else
        {
          c.active = true;
          c.startFrame = n;
          c.len = e.transition;
          c.startCut = c.curCut;
          c.startQ = c.curQ;
          c.targetCut = e.cutoff;
          c.targetQ = e.q;
          const double f = 1.0 / double (c.len);
          c.curCut = c.startCut + (c.targetCut - c.startCut) * f;
          c.curQ   = c.startQ   + (c.targetQ   - c.startQ)   * f;
        }
        ++c.next;
      }
      else if (c.active)
      {
        if (n >= c.startFrame + c.len)
        {
          c.curCut = c.targetCut;
          c.curQ = c.targetQ;
          c.active = false;
        }
        else
        {
          const double f = double (n - c.startFrame + 1) / double (c.len);
          c.curCut = c.startCut + (c.targetCut - c.startCut) * f;
          c.curQ   = c.startQ   + (c.targetQ   - c.startQ)   * f;
        }
      }
    }

    for (std::size_t c = 0; c < ch; ++c)
    {
      double x = input[c][n];
      for (std::size_t s = 0; s < st; ++s)
      {
        double A1, A2, B0, B1, B2;
        rbjLowPassCoefficients (cfg.sampleRate, ctl[s].curCut, ctl[s].curQ,
                                A1, A2, B0, B1, B2);
        const std::size_t idx = c * st + s;
        const double w = x - A1 * delay1[idx] - A2 * delay2[idx] + bias;
        const double y = B0 * w + B1 * delay1[idx] + B2 * delay2[idx];
        delay2[idx] = delay1[idx];
        delay1[idx] = w;
        x = y;
      }
      out[c][n] = x;
    }
    bias = -bias;
  }
}

double lossIndependent (const RefConfig& cfg,
                        const std::vector<std::vector<double>>& input,
                        const std::vector<std::vector<double>>& adj)
{
  std::vector<std::vector<double>> out;
  renderDifferenceEquation (cfg, input, out);
  double loss = 0.0;
  for (std::size_t c = 0; c < out.size (); ++c)
    for (std::size_t n = 0; n < out[c].size (); ++n)
      loss += adj[c][n] * out[c][n];
  return loss;
}

// ---------------------------------------------------------------------------
// Helpers

std::vector<std::vector<double>> makeInput (std::size_t ch, std::size_t N,
                                            double phase = 0.0)
{
  std::vector<std::vector<double>> x (ch, std::vector<double> (N));
  for (std::size_t c = 0; c < ch; ++c)
    for (std::size_t n = 0; n < N; ++n)
      x[c][n] = 0.35 * std::sin (0.021 * double (n + 1) * (1.0 + 0.3 * double (c)) + phase)
              + 0.05 * std::cos (0.11 * double (n) + 0.7 * double (c));
  return x;
}

std::vector<std::vector<double>> makeAdjoint (std::size_t ch, std::size_t N)
{
  std::vector<std::vector<double>> a (ch, std::vector<double> (N, 0.0));
  for (std::size_t c = 0; c < ch; ++c)
    for (std::size_t n = 0; n < N; ++n)
      a[c][n] = ((n + 3 * c) % 7 == 0) ? 0.5 - 0.01 * double (n % 5)
                                       : 0.001 * double ((n + c) % 3);
  return a;
}

std::vector<double*> ptrs (std::vector<std::vector<double>>& v)
{
  std::vector<double*> p (v.size ());
  for (std::size_t i = 0; i < v.size (); ++i)
    p[i] = v[i].data ();
  return p;
}

std::vector<const double*> cptrs (const std::vector<std::vector<double>>& v)
{
  std::vector<const double*> p (v.size ());
  for (std::size_t i = 0; i < v.size (); ++i)
    p[i] = v[i].data ();
  return p;
}

std::vector<Tape::Event> toTapeEvents (const std::vector<RefEvent>& evs)
{
  std::vector<Tape::Event> out;
  for (const RefEvent& e : evs)
    out.push_back (Tape::Event {e.frame, e.stage, e.cutoff, e.q, e.transition});
  return out;
}

// Push input in the supplied chunk sizes (cycled) and return output.
void runTape (Tape& tape,
              const std::vector<std::vector<double>>& input,
              std::vector<std::vector<double>>& output,
              const std::vector<std::size_t>& chunkSizes)
{
  const std::size_t ch = input.size ();
  const std::size_t N = input.empty () ? 0 : input[0].size ();
  output.assign (ch, std::vector<double> (N, 0.0));

  std::size_t n = 0;
  std::size_t ci = 0;
  while (n < N)
  {
    const std::size_t k = std::min (N - n, chunkSizes[ci % chunkSizes.size ()]);
    ++ci;
    std::vector<std::vector<double>> inChunk (ch, std::vector<double> (k));
    std::vector<std::vector<double>> outChunk (ch, std::vector<double> (k));
    for (std::size_t c = 0; c < ch; ++c)
      std::copy (input[c].begin () + n, input[c].begin () + n + k,
                 inChunk[c].begin ());
    std::vector<const double*> ip = cptrs (inChunk);
    std::vector<double*> op = ptrs (outChunk);
    tape.push (ip.data (), k, op.data ());
    for (std::size_t c = 0; c < ch; ++c)
      std::copy (outChunk[c].begin (), outChunk[c].end (),
                 output[c].begin () + n);
    n += k;
  }
}

double maxAbsDiff (const std::vector<std::vector<double>>& a,
                   const std::vector<std::vector<double>>& b)
{
  double m = 0.0;
  for (std::size_t c = 0; c < a.size (); ++c)
    for (std::size_t n = 0; n < a[c].size (); ++n)
      m = std::max (m, std::abs (a[c][n] - b[c][n]));
  return m;
}

bool allFiniteNonEmpty (const std::vector<std::vector<double>>& v, double& energy)
{
  energy = 0.0;
  bool any = false;
  for (std::size_t c = 0; c < v.size (); ++c)
  {
    if (v[c].empty ())
      return false;
    for (double x : v[c])
    {
      if (!std::isfinite (x))
        return false;
      energy += x * x;
      any = true;
    }
  }
  return any;
}

bool closeEnough (double got, double ref)
{
  return std::abs (got - ref) <= 2e-7 + 2e-5 * std::abs (ref);
}

// The "everything at once" event set.
std::vector<RefEvent> complexEvents ()
{
  return {
    {0,   0, 800.0,  0.70, 0},  // initial
    {0,   1, 2500.0, 1.00, 0},  // initial
    {0,   2, 5000.0, 0.80, 0},  // initial
    {0,   3, 7000.0, 1.20, 0},  // initial
    {20,  0, 1200.0, 0.90, 8},  // ramp
    {26,  0, 1600.0, 1.10, 0},  // cancels the ramp before it finishes
    {40,  1, 3000.0, 1.30, 15}, // second stage feedback
    {60,  2, 4000.0, 1.60, 0},  // zero-length transition
    {90,  3, 6000.0, 0.90, 4},  // short ramp
    {120, 1, 1800.0, 0.70, 30}  // target not reached by the end
  };
}

RefConfig complexConfig (std::size_t channels)
{
  RefConfig cfg;
  cfg.sampleRate = 48000.0;
  cfg.channels = channels;
  cfg.stages = 4;
  cfg.events = complexEvents ();
  return cfg;
}

// ===========================================================================
// Forward

int forwardProductionChain (void)
{
  // Static settings: the tape must equal a direct RBJ/DFII chain sample for
  // sample.
  for (std::size_t stages = 1; stages <= 2; ++stages)
  {
    RefConfig cfg;
    cfg.sampleRate = 44100.0;
    cfg.channels = 2;
    cfg.stages = stages;
    cfg.events.push_back (RefEvent {0, 0, 1300.0, 0.707, 0});
    if (stages == 2)
      cfg.events.push_back (RefEvent {0, 1, 4100.0, 1.2, 0});

    const std::size_t N = 401;
    std::vector<std::vector<double>> input = makeInput (2, N, 0.3);
    std::vector<std::vector<double>> expected;
    renderReference (cfg, input, expected);

    Tape tape (cfg.sampleRate, cfg.channels, stages,
               toTapeEvents (cfg.events), 16);
    std::vector<std::vector<double>> got;
    runTape (tape, input, got, std::vector<std::size_t> {1, 7, 127});
    tape.seal ();

    TASK_CHECK (maxAbsDiff (got, expected) < 1e-13);
    double energy = 0;
    TASK_CHECK (allFiniteNonEmpty (got, energy) && energy > 1.0);
  }
  return 0;
}

int forwardTimeVarying (void)
{
  RefConfig cfg = complexConfig (6);
  const std::size_t N = 140;
  std::vector<std::vector<double>> input = makeInput (6, N, 0.1);
  std::vector<std::vector<double>> expected;
  renderReference (cfg, input, expected);

  Tape tape (cfg.sampleRate, 6, 4, toTapeEvents (cfg.events), 7);
  std::vector<std::vector<double>> got;
  runTape (tape, input, got, std::vector<std::size_t> {3, 11, 53});
  tape.seal ();

  TASK_CHECK (maxAbsDiff (got, expected) < 1e-12);
  double energy = 0;
  TASK_CHECK (allFiniteNonEmpty (got, energy));
  TASK_CHECK (energy > 1.0);
  TASK_CHECK (tape.numFrames () == N);
  return 0;
}

int forwardChunking (void)
{
  RefConfig cfg = complexConfig (2);
  const std::size_t N = 211;
  std::vector<std::vector<double>> input = makeInput (2, N, 0.6);

  const std::vector<std::vector<std::size_t>> chunkings = {
    {1}, {7}, {127}, {5, 9, 2}
  };
  std::vector<std::vector<double>> reference;
  for (std::size_t i = 0; i < chunkings.size (); ++i)
  {
    Tape tape (cfg.sampleRate, 2, 4, toTapeEvents (cfg.events), 13);
    std::vector<std::vector<double>> got;
    runTape (tape, input, got, chunkings[i]);
    tape.seal ();
    if (i == 0)
      reference = got;
    else
      TASK_CHECK (maxAbsDiff (got, reference) == 0.0);
  }
  TASK_CHECK (!reference.empty ());
  return 0;
}

int forwardZeroFrames (void)
{
  RefConfig cfg = complexConfig (1);
  const std::size_t N = 33;
  std::vector<std::vector<double>> input = makeInput (1, N);

  Tape tape (cfg.sampleRate, 1, 4, toTapeEvents (cfg.events), 8);
  // Zero-frame push must not touch pointers or advance the clock.
  std::vector<const double*> nullIn (1, nullptr);
  std::vector<double*> nullOut (1, nullptr);
  tape.push (nullIn.data (), 0, nullOut.data ());
  TASK_CHECK (tape.numFrames () == 0);

  std::vector<std::vector<double>> a, b;
  runTape (tape, input, a, std::vector<std::size_t> {1});
  TASK_CHECK (tape.numFrames () == N);

  tape.seal ();
  // A second tape with a zero push interleaved yields identical audio.
  Tape tape2 (cfg.sampleRate, 1, 4, toTapeEvents (cfg.events), 8);
  std::vector<std::vector<double>> in0 (1, std::vector<double> (0));
  std::vector<std::vector<double>> out0 (1, std::vector<double> (0));
  std::vector<const double*> ip0 = cptrs (in0);
  std::vector<double*> op0 = ptrs (out0);
  tape2.push (ip0.data (), 0, op0.data ());
  runTape (tape2, input, b, std::vector<std::size_t> {1});
  tape2.seal ();
  TASK_CHECK (maxAbsDiff (a, b) == 0.0);
  return 0;
}

int forwardImmediate (void)
{
  RefConfig cfg;
  cfg.sampleRate = 96000.0;
  cfg.channels = 2;
  cfg.stages = 3;
  cfg.events = {
    {0, 0, 1000.0, 0.8, 0},
    {0, 1, 4000.0, 1.4, 0},
    {0, 2, 9000.0, 0.6, 0},
    {10, 1, 3000.0, 0.9, 0},
    {50, 0, 1500.0, 1.7, 0}
  };
  const std::size_t N = 120;
  std::vector<std::vector<double>> input = makeInput (2, N, 1.1);
  std::vector<std::vector<double>> expected;
  renderReference (cfg, input, expected);

  Tape tape (cfg.sampleRate, 2, 3, toTapeEvents (cfg.events), 9);
  std::vector<std::vector<double>> got;
  runTape (tape, input, got, std::vector<std::size_t> {4});
  tape.seal ();
  TASK_CHECK (maxAbsDiff (got, expected) < 1e-12);
  return 0;
}

// ===========================================================================
// Gradients

struct GradCheck
{
  const RefConfig& cfg;
  const std::vector<std::vector<double>>& input;
  const std::vector<std::vector<double>>& adj;

  double inputDerivative (std::size_t c, std::size_t n) const
  {
    const double h = 1e-6;
    std::vector<std::vector<double>> plus = input;
    std::vector<std::vector<double>> minus = input;
    plus[c][n] += h;
    minus[c][n] -= h;
    return (lossIndependent (cfg, plus, adj) - lossIndependent (cfg, minus, adj)) / (2.0 * h);
  }

  double eventDerivative (std::size_t ei, bool withCutoff) const
  {
    const double h = withCutoff ? 1e-4 : 1e-6;
    RefConfig plus = cfg;
    RefConfig minus = cfg;
    if (withCutoff)
    {
      plus.events[ei].cutoff += h;
      minus.events[ei].cutoff -= h;
    }
    else
    {
      plus.events[ei].q += h;
      minus.events[ei].q -= h;
    }
    return (lossIndependent (plus, input, adj) - lossIndependent (minus, input, adj))
           / (2.0 * h);
  }
};

int checkGradients (const RefConfig& cfg, const std::vector<std::size_t>& chunkSizes)
{
  const std::size_t ch = cfg.channels;
  const std::size_t N = 128;

  std::vector<std::vector<double>> input = makeInput (ch, N, 0.2);
  std::vector<std::vector<double>> adj = makeAdjoint (ch, N);

  // The hand-coded difference equation must agree with the production
  // renderer before it is trusted as a finite-difference reference.
  TASK_CHECK (std::abs (lossReference (cfg, input, adj)
                        - lossIndependent (cfg, input, adj))
              <= 1e-9 * (1.0 + std::abs (lossReference (cfg, input, adj))));

  Tape tape (cfg.sampleRate, ch, cfg.stages, toTapeEvents (cfg.events), 11);
  std::vector<std::vector<double>> out;
  runTape (tape, input, out, chunkSizes);
  tape.seal ();

  std::vector<const double*> ap = cptrs (adj);
  Tape::Gradients g = tape.backward (ap.data ());

  TASK_CHECK (g.input.size () == ch);
  TASK_CHECK (g.cutoff.size () == cfg.events.size ());
  TASK_CHECK (g.q.size () == cfg.events.size ());

  GradCheck check {cfg, input, adj};

  for (std::size_t c = 0; c < ch; ++c)
  {
    TASK_CHECK (g.input[c].size () == N);
    for (std::size_t n = 0; n < N; ++n)
    {
      const double ref = check.inputDerivative (c, n);
      TASK_CHECK (closeEnough (g.input[c][n], ref));
    }
  }

  for (std::size_t i = 0; i < cfg.events.size (); ++i)
  {
    TASK_CHECK (closeEnough (g.cutoff[i], check.eventDerivative (i, true)));
    TASK_CHECK (closeEnough (g.q[i], check.eventDerivative (i, false)));
  }
  return 0;
}

int gradientsSharedSix (void)
{
  RefConfig cfg = complexConfig (6);
  return checkGradients (cfg, std::vector<std::size_t> {1});
}

int gradientsSecondStage (void)
{
  RefConfig cfg;
  cfg.sampleRate = 44100.0;
  cfg.channels = 2;
  cfg.stages = 2;
  cfg.events = {
    {0, 0, 1500.0, 0.8, 0},
    {0, 1, 3500.0, 1.5, 0},
    {30, 1, 2500.0, 0.7, 12}
  };
  // Focus on the stage-1 event gradient but check everything.
  return checkGradients (cfg, std::vector<std::size_t> {7, 5});
}

int gradientsChangedBeforeCompletion (void)
{
  RefConfig cfg;
  cfg.sampleRate = 48000.0;
  cfg.channels = 2;
  cfg.stages = 1;
  cfg.events = {
    {0, 0, 1000.0, 0.7, 0},
    {10, 0, 2000.0, 1.3, 20}, // long ramp
    {17, 0, 3200.0, 0.8, 6}   // interrupts it
  };
  return checkGradients (cfg, std::vector<std::size_t> {3});
}

int gradientsZeroLengthTransition (void)
{
  RefConfig cfg;
  cfg.sampleRate = 96000.0;
  cfg.channels = 2;
  cfg.stages = 1;
  cfg.events = {
    {0, 0, 2000.0, 1.0, 0},
    {40, 0, 6000.0, 0.9, 0},  // instantaneous
    {70, 0, 3000.0, 1.8, 0}
  };
  return checkGradients (cfg, std::vector<std::size_t> {1, 7});
}

int gradientsTailTarget (void)
{
  RefConfig cfg;
  cfg.sampleRate = 48000.0;
  cfg.channels = 2;
  cfg.stages = 1;
  cfg.events = {
    {0, 0, 1200.0, 0.8, 0},
    {100, 0, 4000.0, 1.5, 200} // never reached inside 128 frames
  };
  return checkGradients (cfg, std::vector<std::size_t> {1});
}

// ===========================================================================
// Replay

int replayMultipleK (void)
{
  RefConfig cfg = complexConfig (2);
  const std::size_t N = 96;
  std::vector<std::vector<double>> input = makeInput (2, N, 0.4);
  std::vector<std::vector<double>> adj = makeAdjoint (2, N);
  std::vector<const double*> ap = cptrs (adj);

  Tape::Gradients baseline;
  bool haveBaseline = false;

  const std::size_t ks[] = {1, 3, 7, 32, 1000};
  for (std::size_t ki = 0; ki < sizeof (ks) / sizeof (ks[0]); ++ki)
  {
    const std::size_t K = ks[ki];
    Tape tape (cfg.sampleRate, 2, 4, toTapeEvents (cfg.events), K);
    std::vector<std::vector<double>> out;
    runTape (tape, input, out, std::vector<std::size_t> {7});
    tape.seal ();

    Tape::Gradients g = tape.backward (ap.data ());

    // Replay bound: at most 2N production frames, and at least the segment.
    TASK_CHECK (tape.lastReplayFrames () <= 2 * N);
    TASK_CHECK (tape.lastReplayFrames () >= N);
    TASK_CHECK (tape.maxReplayFrames () <= 2 * N);

    // Peak saved state must be O((N/K + K) * channels * stages).
    const double bound = 8.0 * (double (N) / double (K) + double (K) + 4.0)
                       * double (2) * double (4);
    TASK_CHECK (double (tape.peakSavedStateCount ()) <= bound);
    TASK_CHECK (tape.peakSavedStateCount () > 0);

    if (!haveBaseline)
    {
      baseline = g;
      haveBaseline = true;
    }
    else
    {
      for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t n = 0; n < N; ++n)
          TASK_CHECK (g.input[c][n] == baseline.input[c][n]);
      for (std::size_t i = 0; i < cfg.events.size (); ++i)
      {
        TASK_CHECK (g.cutoff[i] == baseline.cutoff[i]);
        TASK_CHECK (g.q[i] == baseline.q[i]);
      }
    }
  }
  return 0;
}

int replayChunkingAndTwoBackward (void)
{
  RefConfig cfg = complexConfig (6);
  const std::size_t N = 75;
  std::vector<std::vector<double>> input = makeInput (6, N, 0.9);
  std::vector<std::vector<double>> adj1 = makeAdjoint (6, N);

  // Second, different error signal.
  std::vector<std::vector<double>> adj2 (6, std::vector<double> (N));
  for (std::size_t c = 0; c < 6; ++c)
    for (std::size_t n = 0; n < N; ++n)
      adj2[c][n] = std::sin (0.05 * double (n) + double (c)) * 0.2;

  std::vector<const double*> a1 = cptrs (adj1);
  std::vector<const double*> a2 = cptrs (adj2);

  Tape tape (cfg.sampleRate, 6, 4, toTapeEvents (cfg.events), 5);
  std::vector<std::vector<double>> out;
  runTape (tape, input, out, std::vector<std::size_t> {1, 7, 127});
  tape.seal ();

  Tape::Gradients g1 = tape.backward (a1.data ());
  Tape::Gradients g2 = tape.backward (a2.data ());
  // Repeating the first backward must reproduce it exactly.
  Tape::Gradients g1b = tape.backward (a1.data ());

  // A second tape fed in one block must agree with the chunked one.
  Tape tape2 (cfg.sampleRate, 6, 4, toTapeEvents (cfg.events), 5);
  std::vector<std::vector<double>> out2;
  runTape (tape2, input, out2, std::vector<std::size_t> {N});
  tape2.seal ();
  Tape::Gradients g1c = tape2.backward (a1.data ());

  TASK_CHECK (maxAbsDiff (out, out2) == 0.0);
  TASK_CHECK (tape.lastReplayFrames () <= 2 * N);

  for (std::size_t c = 0; c < 6; ++c)
    for (std::size_t n = 0; n < N; ++n)
    {
      TASK_CHECK (g1.input[c][n] == g1b.input[c][n]);
      TASK_CHECK (g1.input[c][n] == g1c.input[c][n]);
    }
  for (std::size_t i = 0; i < cfg.events.size (); ++i)
  {
    TASK_CHECK (g1.cutoff[i] == g1b.cutoff[i]);
    TASK_CHECK (g1.q[i] == g1b.q[i]);
  }
  // The two different adjoints must produce different gradients.
  double diff = 0.0;
  for (std::size_t c = 0; c < 6; ++c)
    for (std::size_t n = 0; n < N; ++n)
      diff += std::abs (g1.input[c][n] - g2.input[c][n]);
  TASK_CHECK (diff > 1e-9);
  return 0;
}

int replayEmptySegment (void)
{
  RefConfig cfg = complexConfig (1);
  Tape tape (cfg.sampleRate, 1, 4, toTapeEvents (cfg.events), 8);
  tape.seal ();
  TASK_CHECK (tape.numFrames () == 0);

  Tape::Gradients g = tape.backward (nullptr);
  TASK_CHECK (g.input.size () == 1);
  TASK_CHECK (g.input[0].empty ());
  TASK_CHECK (g.cutoff.size () == cfg.events.size ());
  TASK_CHECK (g.q.size () == cfg.events.size ());
  for (double v : g.cutoff)
    TASK_CHECK (v == 0.0);
  TASK_CHECK (tape.lastReplayFrames () == 0);
  TASK_CHECK (tape.peakSavedStateCount () > 0);
  return 0;
}

int replayFailureReuse (void)
{
  RefConfig cfg = complexConfig (2);
  const std::size_t N = 40;
  std::vector<std::vector<double>> input = makeInput (2, N, 0.5);
  std::vector<std::vector<double>> adj = makeAdjoint (2, N);
  std::vector<const double*> ap = cptrs (adj);

  Tape tape (cfg.sampleRate, 2, 4, toTapeEvents (cfg.events), 6);

  // backward before seal must throw and leave the tape usable.
  bool threw = false;
  try { tape.backward (ap.data ()); }
  catch (const std::exception&) { threw = true; }
  TASK_CHECK (threw);

  std::vector<std::vector<double>> out;
  runTape (tape, input, out, std::vector<std::size_t> {7});
  tape.seal ();

  // A rejected push (non-finite input) must not change the recorded audio.
  std::vector<std::vector<double>> bad (2, std::vector<double> (3, 0.1));
  bad[1][2] = std::numeric_limits<double>::infinity ();
  std::vector<const double*> bp = cptrs (bad);
  std::vector<std::vector<double>> dummy (2, std::vector<double> (3));
  std::vector<double*> dp = ptrs (dummy);
  threw = false;
  try { tape.push (bp.data (), 3, dp.data ()); }
  catch (const std::exception&) { threw = true; }
  TASK_CHECK (threw);
  TASK_CHECK (tape.numFrames () == N);

  // push after seal must throw, too.
  threw = false;
  std::vector<std::vector<double>> more (2, std::vector<double> (1, 0.0));
  std::vector<const double*> mp = cptrs (more);
  std::vector<std::vector<double>> mo (2, std::vector<double> (1));
  std::vector<double*> mop = ptrs (mo);
  try { tape.push (mp.data (), 1, mop.data ()); }
  catch (const std::exception&) { threw = true; }
  TASK_CHECK (threw);

  // The tape still produces correct gradients after those failures.
  Tape::Gradients g = tape.backward (ap.data ());
  Tape fresh (cfg.sampleRate, 2, 4, toTapeEvents (cfg.events), 6);
  std::vector<std::vector<double>> out2;
  runTape (fresh, input, out2, std::vector<std::size_t> {1});
  fresh.seal ();
  Tape::Gradients expected = fresh.backward (ap.data ());

  TASK_CHECK (maxAbsDiff (out, out2) == 0.0);
  for (std::size_t c = 0; c < 2; ++c)
    for (std::size_t n = 0; n < N; ++n)
      TASK_CHECK (g.input[c][n] == expected.input[c][n]);
  for (std::size_t i = 0; i < cfg.events.size (); ++i)
  {
    TASK_CHECK (g.cutoff[i] == expected.cutoff[i]);
    TASK_CHECK (g.q[i] == expected.q[i]);
  }
  return 0;
}

int replayInvalidEventRejected (void)
{
  // Out-of-range cutoff, unsorted frames, duplicate stage/frame, missing
  // frame-0 event and K=0 must all be rejected up front.
  std::vector<Tape::Event> base = {
    {0, 0, 1000.0, 1.0, 0},
    {5, 1, 2000.0, 1.0, 0}
  };
  bool threw = false;

  threw = false;
  try { Tape t (48000.0, 2, 1, base, 4); }
  catch (const std::exception&) { threw = true; }
  TASK_CHECK (threw); // stage 1 >= 1 stage

  threw = false;
  try { Tape t (48000.0, 2, 2, base, 0); }
  catch (const std::exception&) { threw = true; }
  TASK_CHECK (threw); // K == 0

  std::vector<Tape::Event> tooHigh = {
    {0, 0, 12000.0, 1.0, 0}, {0, 1, 2000.0, 1.0, 0}};
  threw = false;
  try { Tape t (48000.0, 2, 2, tooHigh, 4); }
  catch (const std::exception&) { threw = true; }
  TASK_CHECK (threw);

  std::vector<Tape::Event> unsorted = {
    {0, 0, 1000.0, 1.0, 0}, {0, 1, 2000.0, 1.0, 0},
    {10, 0, 1500.0, 1.0, 0}, {5, 1, 2500.0, 1.0, 0}};
  threw = false;
  try { Tape t (48000.0, 2, 2, unsorted, 4); }
  catch (const std::exception&) { threw = true; }
  TASK_CHECK (threw);

  std::vector<Tape::Event> duplicate = {
    {0, 0, 1000.0, 1.0, 0}, {0, 1, 2000.0, 1.0, 0},
    {10, 0, 1500.0, 1.0, 0}, {10, 0, 1600.0, 1.0, 0}};
  threw = false;
  try { Tape t (48000.0, 2, 2, duplicate, 4); }
  catch (const std::exception&) { threw = true; }
  TASK_CHECK (threw);

  std::vector<Tape::Event> missing = {{0, 0, 1000.0, 1.0, 0}};
  threw = false;
  try { Tape t (48000.0, 2, 2, missing, 4); }
  catch (const std::exception&) { threw = true; }
  TASK_CHECK (threw);
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
  {"Forward", "production_chain", forwardProductionChain},
  {"Forward", "time_varying", forwardTimeVarying},
  {"Forward", "chunking", forwardChunking},
  {"Forward", "zero_frames", forwardZeroFrames},
  {"Forward", "immediate", forwardImmediate},

  {"Gradients", "shared_six", gradientsSharedSix},
  {"Gradients", "second_stage", gradientsSecondStage},
  {"Gradients", "changed_before_completion", gradientsChangedBeforeCompletion},
  {"Gradients", "zero_length_transition", gradientsZeroLengthTransition},
  {"Gradients", "tail_target", gradientsTailTarget},

  {"Replay", "multiple_k", replayMultipleK},
  {"Replay", "chunking_two_backward", replayChunkingAndTwoBackward},
  {"Replay", "empty_segment", replayEmptySegment},
  {"Replay", "failure_reuse", replayFailureReuse},
  {"Replay", "invalid_event_rejected", replayInvalidEventRejected}};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
