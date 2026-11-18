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
#include "DspFilters/MathSupplement.h"
#include "DspFilters/Biquad.h"
#include "DspFilters/State.h"
#include "DspFilters/RBJ.h"
#include "DspFilters/LowPassTape.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Dsp {

namespace {

typedef BiquadBase::State<DirectFormII> TapeState;

// Alternating denormal-prevention bias matching DenormalPrevention::ac().
// A state is called exactly once per frame, so the first call happens on
// frame 0 and returns -anti_denormal_vsa.
inline double vsaForFrame (std::uint64_t frame)
{
  return (frame & 1) ? anti_denormal_vsa : -anti_denormal_vsa;
}

// Normalised RBJ low pass coefficients plus their analytic derivatives with
// respect to cutoff and Q.  The value expressions mirror
// RBJ::LowPass::setup exactly; only the derivative terms are additional.
struct Coefs
{
  double a1, a2, b0, b1, b2;
  double da1dc, da2dc, db0dc, db1dc, db2dc;
  double da1dq, da2dq, db0dq, db1dq, db2dq;
};

inline Coefs designDerivatives (double sampleRate, double cutoff, double q)
{
  const double w0 = 2 * doublePi * cutoff / sampleRate;
  const double cs = std::cos (w0);
  const double sn = std::sin (w0);
  const double AL = sn / (2 * q);
  const double a0 = 1 + AL;
  const double a1r = -2 * cs;
  const double a2r = 1 - AL;
  const double b0r = (1 - cs) / 2;
  const double b1r = 1 - cs;
  const double b2r = (1 - cs) / 2;

  Coefs o;
  o.a1 = a1r / a0;
  o.a2 = a2r / a0;
  o.b0 = b0r / a0;
  o.b1 = b1r / a0;
  o.b2 = b2r / a0;

  const double inva0 = 1 / a0;
  const double inva02 = inva0 * inva0;

  // d/d(cutoff)
  const double W = 2 * doublePi / sampleRate;
  const double dcs = -sn * W;
  const double dsn = cs * W;
  const double dAL = dsn / (2 * q);
  const double da0 = dAL;
  const double da1r = -2 * dcs;
  const double da2r = -dAL;
  const double db0r = -dcs / 2;
  const double db1r = -dcs;
  const double db2r = -dcs / 2;

  o.da1dc = (da1r * a0 - a1r * da0) * inva02;
  o.da2dc = (da2r * a0 - a2r * da0) * inva02;
  o.db0dc = (db0r * a0 - b0r * da0) * inva02;
  o.db1dc = (db1r * a0 - b1r * da0) * inva02;
  o.db2dc = (db2r * a0 - b2r * da0) * inva02;

  // d/d(q)
  const double dALq = -sn / (2 * q * q);
  const double da0q = dALq;

  o.da1dq = (0 * a0 - a1r * da0q) * inva02;
  o.da2dq = (-dALq * a0 - a2r * da0q) * inva02;
  o.db0dq = (0 * a0 - b0r * da0q) * inva02;
  o.db1dq = (0 * a0 - b1r * da0q) * inva02;
  o.db2dq = (0 * a0 - b2r * da0q) * inva02;

  return o;
}

} // namespace

//==============================================================================

struct LowPassTape::Impl
{
  struct StageEvent
  {
    std::uint64_t frame;
    double cutoff;
    double q;
    std::uint32_t transition;
    std::size_t global;
  };

  // A piece of the deterministic parameter trajectory of one stage.
  // kind 0 (steady): value c0/q0 for the whole span.
  // kind 1 (ramp):   value starts at (c0,q0) at start and advances by
  //                  (c1-c0, q1-q0) * scale per frame, taking the first step
  //                  before the frame is filtered.
  struct Seg
  {
    std::uint64_t start;
    std::uint64_t end;
    int kind;
    double c0, q0;
    double c1, q1;
    double scale;
    std::size_t local;   // index into stageEvents[stage]
    std::size_t global;  // index into the user supplied event vector
    std::int64_t baseFrame; // frame of the value the ramp starts from, or -1
  };

  struct StageSchedule
  {
    std::vector<Seg> segs;
    std::vector<std::uint64_t> starts;
  };

  struct Checkpoint
  {
    std::vector<double> v1; // past w values, size channels*stages
    std::vector<double> v2;
  };

  Impl (const Spec& spec_,
        const std::vector<LowPassTapeEvent>& inputEvents)
    : spec (spec_)
    , channels (spec_.channels)
    , stages (spec_.stages)
    , K (spec_.checkpointInterval)
    , events (inputEvents)
    , sealedFlag (false)
    , frames (0)
    , lastReplayFrames (0)
    , peakReplayFrames (0)
    , peakLocalActivations (0)
  {
    validateSpec ();
    validateEvents ();
    buildStageEvents ();

    states.resize (channels * stages);
    filters.resize (stages);
    inputByChannel.resize (channels);
    sched.resize (stages);
  }

  //--------------------------------------------------------------------------

  void validateSpec ()
  {
    if (!(spec.sampleRate > 0) || !std::isfinite (spec.sampleRate))
      throw std::invalid_argument ("LowPassTape: invalid sample rate");

    if (channels != 1 && channels != 2 && channels != 6)
      throw std::invalid_argument ("LowPassTape: channels must be 1, 2 or 6");

    if (stages < 1 || stages > 4)
      throw std::invalid_argument ("LowPassTape: stages must be in 1..4");

    if (K == 0)
      throw std::invalid_argument ("LowPassTape: checkpoint interval must be positive");
  }

  void validateEvents ()
  {
    if (events.empty ())
      throw std::invalid_argument ("LowPassTape: events must not be empty");

    const double minCutoff = 200.0;
    const double maxCutoff = 0.2 * spec.sampleRate;

    std::uint64_t previousFrame = 0;
    bool havePrevious = false;

    std::vector<int> countAtZero (stages, 0);
    std::vector<std::uint64_t> lastFrame (stages, 0);
    std::vector<char> haveFrame (stages, 0);

    for (std::size_t i = 0; i < events.size (); ++i)
    {
      const LowPassTapeEvent& e = events[i];

      if (e.stage >= stages)
        throw std::invalid_argument ("LowPassTape: event stage out of range");

      if (!std::isfinite (e.cutoff) || !std::isfinite (e.q))
        throw std::invalid_argument ("LowPassTape: non-finite event parameter");

      if (e.cutoff < minCutoff || e.cutoff > maxCutoff)
        throw std::invalid_argument ("LowPassTape: cutoff out of range");

      if (e.q < 0.5 || e.q > 2.0)
        throw std::invalid_argument ("LowPassTape: Q out of range");

      if (havePrevious && e.frame < previousFrame)
        throw std::invalid_argument ("LowPassTape: events are not sorted by frame");

      previousFrame = e.frame;
      havePrevious = true;

      if (haveFrame[e.stage] && e.frame == lastFrame[e.stage])
        throw std::invalid_argument ("LowPassTape: duplicate event frame for a stage");

      lastFrame[e.stage] = e.frame;
      haveFrame[e.stage] = 1;

      if (e.frame == 0)
        ++countAtZero[e.stage];
    }

    for (std::size_t s = 0; s < stages; ++s)
      if (countAtZero[s] != 1)
        throw std::invalid_argument ("LowPassTape: every stage needs one frame 0 event");
  }

  void buildStageEvents ()
  {
    stageEvents.resize (stages);
    for (std::size_t i = 0; i < events.size (); ++i)
    {
      StageEvent se;
      se.frame = events[i].frame;
      se.cutoff = events[i].cutoff;
      se.q = events[i].q;
      se.transition = events[i].transition;
      se.global = i;
      stageEvents[events[i].stage].push_back (se);
    }
  }

  //--------------------------------------------------------------------------

  static void segValue (const Seg& s, std::uint64_t k, double& c, double& q)
  {
    if (s.kind == 0)
    {
      c = s.c0;
      q = s.q0;
    }
    else
    {
      const double j = static_cast<double> (k - s.start);
      const double frac = (j + 1.0) * s.scale;
      c = s.c0 + frac * (s.c1 - s.c0);
      q = s.q0 + frac * (s.q1 - s.q0);
    }
  }

  void buildSchedule (std::uint64_t total)
  {
    for (std::size_t st = 0; st < stages; ++st)
    {
      StageSchedule& ss = sched[st];
      ss.segs.clear ();
      ss.starts.clear ();

      const std::vector<StageEvent>& ev = stageEvents[st];
      if (total == 0 || ev.empty ())
        continue;

      std::uint64_t nextF = (ev.size () > 1) ? ev[1].frame : total;
      if (nextF > total)
        nextF = total;

      Seg first;
      first.start = 0;
      first.end = nextF;
      first.kind = 0;
      first.c0 = ev[0].cutoff;
      first.q0 = ev[0].q;
      first.c1 = first.q1 = 0;
      first.scale = 0;
      first.local = 0;
      first.global = ev[0].global;
      first.baseFrame = -1;
      ss.segs.push_back (first);

      for (std::size_t i = 1; i < ev.size (); ++i)
      {
        const std::uint64_t f = ev[i].frame;
        if (f >= total)
          break;

        std::uint64_t nf = (i + 1 < ev.size ()) ? ev[i + 1].frame : total;
        if (nf > total)
          nf = total;

        double baseC = 0, baseQ = 0;
        segValue (ss.segs.back (), f - 1, baseC, baseQ);

        if (ev[i].transition == 0)
        {
          Seg s;
          s.start = f;
          s.end = nf;
          s.kind = 0;
          s.c0 = ev[i].cutoff;
          s.q0 = ev[i].q;
          s.c1 = s.q1 = 0;
          s.scale = 0;
          s.local = i;
          s.global = ev[i].global;
          s.baseFrame = -1;
          ss.segs.push_back (s);
        }
        else
        {
          const std::uint64_t avail = nf - f;
          const std::uint64_t rampLen =
            std::min<std::uint64_t> (static_cast<std::uint64_t> (ev[i].transition), avail);
          const std::uint64_t rampEnd = f + rampLen;

          Seg s;
          s.start = f;
          s.end = rampEnd;
          s.kind = 1;
          s.c0 = baseC;
          s.q0 = baseQ;
          s.c1 = ev[i].cutoff;
          s.q1 = ev[i].q;
          s.scale = 1.0 / static_cast<double> (ev[i].transition);
          s.local = i;
          s.global = ev[i].global;
          s.baseFrame = static_cast<std::int64_t> (f) - 1;
          ss.segs.push_back (s);

          if (rampEnd < nf)
          {
            Seg t;
            t.start = rampEnd;
            t.end = nf;
            t.kind = 0;
            t.c0 = ev[i].cutoff;
            t.q0 = ev[i].q;
            t.c1 = t.q1 = 0;
            t.scale = 0;
            t.local = i;
            t.global = ev[i].global;
            t.baseFrame = -1;
            ss.segs.push_back (t);
          }
        }
      }

      ss.starts.reserve (ss.segs.size ());
      for (std::size_t i = 0; i < ss.segs.size (); ++i)
        ss.starts.push_back (ss.segs[i].start);
    }
  }

  const Seg* segAt (std::size_t st, std::uint64_t k) const
  {
    const StageSchedule& ss = sched[st];
    if (ss.segs.empty ())
      return nullptr;
    std::vector<std::uint64_t>::const_iterator it =
      std::upper_bound (ss.starts.begin (), ss.starts.end (), k);
    if (it == ss.starts.begin ())
      return nullptr;
    return &ss.segs[static_cast<std::size_t> (it - ss.starts.begin () - 1)];
  }

  void paramAt (std::size_t st, std::uint64_t k, double& c, double& q) const
  {
    const Seg* s = segAt (st, k);
    if (!s)
    {
      const std::vector<StageEvent>& ev = stageEvents[st];
      c = ev.empty () ? 200.0 : ev.back ().cutoff;
      q = ev.empty () ? 0.707 : ev.back ().q;
      return;
    }
    segValue (*s, k, c, q);
  }

  //--------------------------------------------------------------------------

  void captureCheckpoint (std::uint64_t frame)
  {
    Checkpoint cp;
    cp.v1.resize (channels * stages);
    cp.v2.resize (channels * stages);
    for (std::size_t i = 0; i < channels * stages; ++i)
    {
      cp.v1[i] = states[i].getState1 ();
      cp.v2[i] = states[i].getState2 ();
    }
    (void) frame;
    checkpoints.push_back (cp);
  }

  // Render frames [start,end) with production RBJ design and Direct Form II.
  // If wStore is non-null it receives one w value per (channel, stage, frame)
  // in the layout ((channel * stages + stage) * length + localFrame).
  void renderRange (std::uint64_t start,
                    std::uint64_t end,
                    std::vector<TapeState>& localStates,
                    const std::vector<double>& initialV1,
                    const std::vector<double>& initialV2,
                    double* wStore)
  {
    const std::size_t length = static_cast<std::size_t> (end - start);
    const std::size_t cs = channels * stages;

    for (std::size_t i = 0; i < cs; ++i)
      localStates[i].setState (initialV1[i], initialV2[i]);

    for (std::size_t t = 0; t < length; ++t)
    {
      const std::uint64_t k = start + t;

      for (std::size_t st = 0; st < stages; ++st)
      {
        double c, q;
        paramAt (st, k, c, q);
        filters[st].setup (spec.sampleRate, c, q);
      }

      const double vsa = vsaForFrame (k);

      for (std::size_t ch = 0; ch < channels; ++ch)
      {
        double x = inputByChannel[ch][static_cast<std::size_t> (k)];
        for (std::size_t st = 0; st < stages; ++st)
        {
          const std::size_t idx = ch * stages + st;
          x = localStates[idx].process1 (x, filters[st], vsa);
          if (wStore)
            wStore[(idx) * length + t] = localStates[idx].getState1 ();
        }
      }
    }
  }

  //--------------------------------------------------------------------------

  void push (const double* const* input,
             std::size_t blockFrames,
             double* const* output)
  {
    if (sealedFlag)
      throw std::logic_error ("LowPassTape: push() after seal()");

    if (blockFrames == 0)
      return;

    if (!input || !output)
      throw std::invalid_argument ("LowPassTape: null block pointer");

    for (std::size_t ch = 0; ch < channels; ++ch)
      if (!input[ch] || !output[ch])
        throw std::invalid_argument ("LowPassTape: null channel pointer");

    // Validate the whole block before any observable state changes.
    for (std::size_t ch = 0; ch < channels; ++ch)
      for (std::size_t n = 0; n < blockFrames; ++n)
      {
        const double x = input[ch][n];
        if (!std::isfinite (x) || std::fabs (x) > 2.0)
          throw std::invalid_argument ("LowPassTape: input out of range");
      }

    const std::uint64_t oldFrames = frames;
    const std::uint64_t total = oldFrames + static_cast<std::uint64_t> (blockFrames);

    buildSchedule (total);

    // Keep a private copy of the input for replay.  This also makes an
    // in-place (aliased) call safe.
    for (std::size_t ch = 0; ch < channels; ++ch)
      inputByChannel[ch].insert (inputByChannel[ch].end (),
                                 input[ch], input[ch] + blockFrames);

    for (std::size_t n = 0; n < blockFrames; ++n)
    {
      const std::uint64_t k = oldFrames + static_cast<std::uint64_t> (n);

      if (k % static_cast<std::uint64_t> (K) == 0)
        captureCheckpoint (k);

      for (std::size_t st = 0; st < stages; ++st)
      {
        double c, q;
        paramAt (st, k, c, q);
        filters[st].setup (spec.sampleRate, c, q);
      }

      const double vsa = vsaForFrame (k);

      for (std::size_t ch = 0; ch < channels; ++ch)
      {
        double x = inputByChannel[ch][static_cast<std::size_t> (k)];
        for (std::size_t st = 0; st < stages; ++st)
          x = states[ch * stages + st].process1 (x, filters[st], vsa);
        output[ch][n] = x;
      }
    }

    frames = total;
  }

  //--------------------------------------------------------------------------

  Gradients backward (const double* const* adjoint)
  {
    if (!sealedFlag)
      throw std::logic_error ("LowPassTape: backward() before seal()");

    if (frames > 0)
    {
      if (!adjoint)
        throw std::invalid_argument ("LowPassTape: null adjoint");
      for (std::size_t ch = 0; ch < channels; ++ch)
        if (!adjoint[ch])
          throw std::invalid_argument ("LowPassTape: null adjoint channel");
    }

    Gradients g;
    g.frames = static_cast<std::size_t> (frames);
    g.channels = channels;
    g.stages = stages;
    g.input.assign (channels * static_cast<std::size_t> (frames), 0.0);
    g.eventCutoff.assign (events.size (), 0.0);
    g.eventQ.assign (events.size (), 0.0);
    g.checkpointStateCount = checkpoints.size () * channels * stages;

    if (frames == 0)
    {
      lastReplayFrames = 0;
      g.replayFrames = 0;
      g.peakLocalActivations = peakLocalActivations;
      return g;
    }

    buildSchedule (frames);

    const std::size_t cs = channels * stages;
    const std::size_t N = static_cast<std::size_t> (frames);

    const std::size_t segCount =
      static_cast<std::size_t> ((frames + static_cast<std::uint64_t> (K) - 1) /
                                static_cast<std::uint64_t> (K));

    // Audio adjoints of the deterministic parameter trajectory, accumulated
    // per frame and stage while walking frames in reverse.
    std::vector<double> accC (stages), accQ (stages);

    // Reverse-mode state of the parameter trajectory program.
    std::vector<std::vector<double> > pendC (stages), pendQ (stages);
    for (std::size_t st = 0; st < stages; ++st)
    {
      pendC[st].assign (stageEvents[st].size (), 0.0);
      pendQ[st].assign (stageEvents[st].size (), 0.0);
    }

    // Frame ordered list of ramp base frames (back edges of the trajectory
    // program), consumed while walking frames in reverse.
    struct BaseRef
    {
      std::uint64_t frame;
      std::size_t stage;
      std::size_t local;
    };
    std::vector<BaseRef> baseRefs;
    for (std::size_t st = 0; st < stages; ++st)
      for (std::size_t i = 1; i < stageEvents[st].size (); ++i)
      {
        const StageEvent& e = stageEvents[st][i];
        if (e.transition == 0 || e.frame >= frames)
          continue;
        BaseRef b;
        b.frame = e.frame - 1;
        b.stage = st;
        b.local = i;
        baseRefs.push_back (b);
      }
    std::sort (baseRefs.begin (), baseRefs.end (),
               [](const BaseRef& a, const BaseRef& b) { return a.frame > b.frame; });
    std::size_t baseRefPos = 0;

    std::vector<double> evC (events.size (), 0.0);
    std::vector<double> evQ (events.size (), 0.0);

    // Rolling derivatives of the loss with respect to w at frames k+1, k+2.
    std::vector<double> lam1 (cs, 0.0), lam2 (cs, 0.0);
    std::vector<double> old1 (cs, 0.0), old2 (cs, 0.0), cur (cs, 0.0);

    std::vector<Coefs> dk (stages), dk1 (stages), dk2 (stages);

    // Replay is performed one checkpoint interval at a time, from the end.
    for (std::size_t sj = segCount; sj-- > 0; )
    {
      const std::uint64_t start = static_cast<std::uint64_t> (sj) * static_cast<std::uint64_t> (K);
      const std::uint64_t end = std::min<std::uint64_t> (frames, start + static_cast<std::uint64_t> (K));
      const std::size_t length = static_cast<std::size_t> (end - start);

      g.replayFrames += length;

      const Checkpoint& cp = checkpoints[sj];

      std::vector<TapeState> localStates (cs);
      std::vector<double> w (cs * length);

      renderRange (start, end, localStates, cp.v1, cp.v2, w.data ());

      const std::size_t localAct = cs * length;
      if (localAct > peakLocalActivations)
        peakLocalActivations = localAct;

      std::size_t ti = length;
      while (ti-- > 0)
      {
        const std::uint64_t k = start + ti;

        // Snapshot the future lambdas before they are updated.
        std::copy (lam1.begin (), lam1.end (), old1.begin ());
        std::copy (lam2.begin (), lam2.end (), old2.begin ());

        // Coefficient data for frames k, k+1, k+2.
        for (std::size_t st = 0; st < stages; ++st)
        {
          double c, q;
          paramAt (st, k, c, q);
          dk[st] = designDerivatives (spec.sampleRate, c, q);
          if (k + 1 < frames)
          {
            paramAt (st, k + 1, c, q);
            dk1[st] = designDerivatives (spec.sampleRate, c, q);
          }
          if (k + 2 < frames)
          {
            paramAt (st, k + 2, c, q);
            dk2[st] = designDerivatives (spec.sampleRate, c, q);
          }
        }

        // Compute lambda(k) for every channel and stage, descending stage.
        for (std::size_t ch = 0; ch < channels; ++ch)
        {
          for (std::size_t s2 = stages; s2-- > 0; )
          {
            const std::size_t idx = ch * stages + s2;

            double B0, B1, B2;
            if (s2 == stages - 1)
            {
              B0 = adjoint[ch][static_cast<std::size_t> (k)];
              B1 = (k + 1 < frames) ? adjoint[ch][static_cast<std::size_t> (k + 1)] : 0.0;
              B2 = (k + 2 < frames) ? adjoint[ch][static_cast<std::size_t> (k + 2)] : 0.0;
            }
            else
            {
              const std::size_t up = ch * stages + s2 + 1;
              B0 = cur[up];
              B1 = old1[up];
              B2 = old2[up];
            }

            double lam = dk[s2].b0 * B0;
            if (k + 1 < frames)
              lam += dk1[s2].b1 * B1 + (-dk1[s2].a1) * old1[idx];
            if (k + 2 < frames)
              lam += dk2[s2].b2 * B2 + (-dk2[s2].a2) * old2[idx];

            cur[idx] = lam;
          }
        }

        // Parameter adjoints at this frame, plus the input adjoint.
        std::fill (accC.begin (), accC.end (), 0.0);
        std::fill (accQ.begin (), accQ.end (), 0.0);

        for (std::size_t ch = 0; ch < channels; ++ch)
        {
          for (std::size_t s2 = 0; s2 < stages; ++s2)
          {
            const std::size_t idx = ch * stages + s2;
            const double lam = cur[idx];
            const double B = (s2 == stages - 1)
                               ? adjoint[ch][static_cast<std::size_t> (k)]
                               : cur[ch * stages + s2 + 1];

            const double wk = w[idx * length + ti];

            double w1, w2;
            if (ti >= 1)
              w1 = w[idx * length + ti - 1];
            else
              w1 = cp.v1[idx];

            if (ti >= 2)
              w2 = w[idx * length + ti - 2];
            else if (ti == 1)
              w2 = cp.v1[idx];
            else
              w2 = cp.v2[idx];

            const Coefs& cf = dk[s2];

            accC[s2] += lam * (-cf.da1dc * w1 - cf.da2dc * w2)
                      + B * (cf.db0dc * wk + cf.db1dc * w1 + cf.db2dc * w2);
            accQ[s2] += lam * (-cf.da1dq * w1 - cf.da2dq * w2)
                      + B * (cf.db0dq * wk + cf.db1dq * w1 + cf.db2dq * w2);

            if (s2 == 0)
              g.input[ch * N + static_cast<std::size_t> (k)] += lam;
          }
        }

        // Reverse the deterministic parameter trajectory for this frame.
        std::vector<double> scC (accC), scQ (accQ);
        while (baseRefPos < baseRefs.size () && baseRefs[baseRefPos].frame == k)
        {
          const BaseRef& b = baseRefs[baseRefPos];
          scC[b.stage] += pendC[b.stage][b.local];
          scQ[b.stage] += pendQ[b.stage][b.local];
          ++baseRefPos;
        }

        for (std::size_t st = 0; st < stages; ++st)
        {
          const Seg* sg = segAt (st, k);
          if (!sg)
            continue;

          const double AC = scC[st];
          const double AQ = scQ[st];

          if (sg->kind == 0)
          {
            evC[sg->global] += AC;
            evQ[sg->global] += AQ;
          }
          else
          {
            const double j = static_cast<double> (k - sg->start);
            const double ct = (j + 1.0) * sg->scale;
            const double cb = 1.0 - ct;
            evC[sg->global] += AC * ct;
            evQ[sg->global] += AQ * ct;
            pendC[st][sg->local] += AC * cb;
            pendQ[st][sg->local] += AQ * cb;
          }
        }

        // Advance the rolling lambda window.
        for (std::size_t idx = 0; idx < cs; ++idx)
        {
          lam2[idx] = old1[idx];
          lam1[idx] = cur[idx];
        }
      }
    }

    g.eventCutoff.swap (evC);
    g.eventQ.swap (evQ);

    lastReplayFrames = g.replayFrames;
    if (g.replayFrames > peakReplayFrames)
      peakReplayFrames = g.replayFrames;

    g.peakLocalActivations = peakLocalActivations;
    return g;
  }

  //--------------------------------------------------------------------------

  Spec spec;
  std::size_t channels;
  std::size_t stages;
  std::size_t K;
  std::vector<LowPassTapeEvent> events;
  std::vector<std::vector<StageEvent> > stageEvents;
  std::vector<StageSchedule> sched;

  bool sealedFlag;
  std::uint64_t frames;

  std::vector<std::vector<double> > inputByChannel;
  std::vector<TapeState> states;
  std::vector<RBJ::LowPass> filters;
  std::vector<Checkpoint> checkpoints;

  std::size_t lastReplayFrames;
  std::size_t peakReplayFrames;
  std::size_t peakLocalActivations;
};

//==============================================================================

LowPassTape::LowPassTape (const Spec& spec,
                          const std::vector<LowPassTapeEvent>& events)
  : m_impl (new Impl (spec, events))
{
}

LowPassTape::LowPassTape (double sampleRate,
                          std::size_t channels,
                          std::size_t stages,
                          std::size_t checkpointInterval,
                          const std::vector<LowPassTapeEvent>& events)
  : m_impl (new Impl (Spec (sampleRate, channels, stages, checkpointInterval),
                      events))
{
}

LowPassTape::~LowPassTape ()
{
}

LowPassTape::LowPassTape (LowPassTape&&) noexcept = default;

LowPassTape& LowPassTape::operator= (LowPassTape&&) noexcept = default;

void LowPassTape::push (const double* const* input,
                        std::size_t frames,
                        double* const* output)
{
  m_impl->push (input, frames, output);
}

void LowPassTape::seal ()
{
  m_impl->sealedFlag = true;
}

LowPassTape::Gradients LowPassTape::backward (const double* const* outputAdjoint)
{
  return m_impl->backward (outputAdjoint);
}

bool LowPassTape::sealed () const
{
  return m_impl->sealedFlag;
}

std::size_t LowPassTape::framesSeen () const
{
  return static_cast<std::size_t> (m_impl->frames);
}

std::size_t LowPassTape::channelCount () const
{
  return m_impl->channels;
}

std::size_t LowPassTape::stageCount () const
{
  return m_impl->stages;
}

std::size_t LowPassTape::checkpointInterval () const
{
  return m_impl->K;
}

double LowPassTape::sampleRate () const
{
  return m_impl->spec.sampleRate;
}

const std::vector<LowPassTapeEvent>& LowPassTape::events () const
{
  return m_impl->events;
}

std::size_t LowPassTape::checkpointCount () const
{
  return m_impl->checkpoints.size ();
}

std::size_t LowPassTape::checkpointStateCount () const
{
  return m_impl->checkpoints.size () * m_impl->channels * m_impl->stages;
}

std::size_t LowPassTape::peakLocalActivations () const
{
  return m_impl->peakLocalActivations;
}

std::size_t LowPassTape::peakSavedStateCount () const
{
  return checkpointStateCount () + peakLocalActivations ();
}

std::size_t LowPassTape::lastReplayFrameCount () const
{
  return m_impl->lastReplayFrames;
}

std::size_t LowPassTape::peakReplayFrameCount () const
{
  return m_impl->peakReplayFrames;
}

} // namespace Dsp
