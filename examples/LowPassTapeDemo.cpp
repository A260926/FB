// Offline automatic-equaliser demo for Dsp::LowPassTape.
//
//   c++ -std=c++17 -O2 -I. -Ishared/DSPFilters/include \
//       shared/DSPFilters/source/*.cpp examples/LowPassTapeDemo.cpp -o lowpass_tape_demo
//   ./lowpass_tape_demo
//
// The program renders a short multichannel segment through a four stage
// time varying low pass cascade, then replays it backwards with a synthetic
// error signal and reports the input / parameter gradients.

#include "DspFilters/LowPassTape.h"
#include "DspFilters/RBJ.h"
#include "DspFilters/State.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

const double kSampleRate = 48000;
const std::size_t kChannels = 2;
const std::size_t kStages = 4;
const std::size_t kFrames = 512;
const std::size_t kCheckpoint = 64;

std::vector<Dsp::LowPassTapeEvent> makeEvents ()
{
  std::vector<Dsp::LowPassTapeEvent> e;

  // One immediate initial event per stage (frame 0).
  for (std::size_t s = 0; s < kStages; ++s)
  {
    Dsp::LowPassTapeEvent ev;
    ev.frame = 0;
    ev.stage = s;
    ev.cutoff = 800.0 + 900.0 * static_cast<double> (s);
    ev.q = 0.7 + 0.3 * static_cast<double> (s);
    ev.transition = 0;
    e.push_back (ev);
  }

  // Sweep the first stage down, then retarget it mid ramp.
  {
    Dsp::LowPassTapeEvent ev;
    ev.frame = 96;
    ev.stage = 0;
    ev.cutoff = 3500;
    ev.q = 1.6;
    ev.transition = 120;
    e.push_back (ev);
  }
  {
    Dsp::LowPassTapeEvent ev;
    ev.frame = 150;   // cancels the remainder of the previous ramp
    ev.stage = 0;
    ev.cutoff = 600;
    ev.q = 0.9;
    ev.transition = 40;
    e.push_back (ev);
  }

  // A one frame (immediate) switch on stage two.
  {
    Dsp::LowPassTapeEvent ev;
    ev.frame = 300;
    ev.stage = 2;
    ev.cutoff = 1400;
    ev.q = 1.9;
    ev.transition = 0;
    e.push_back (ev);
  }

  return e;
}

} // namespace

int main ()
{
  const std::vector<Dsp::LowPassTapeEvent> events = makeEvents ();

  Dsp::LowPassTape tape (kSampleRate, kChannels, kStages, kCheckpoint, events);

  std::vector<std::vector<double> > x (kChannels,
                                       std::vector<double> (kFrames, 0.0));
  std::vector<std::vector<double> > y (kChannels,
                                       std::vector<double> (kFrames, 0.0));

  for (std::size_t ch = 0; ch < kChannels; ++ch)
    for (std::size_t n = 0; n < kFrames; ++n)
      x[ch][n] = 0.4 * std::sin (0.021 * n + 0.4 * ch) +
                 0.1 * std::sin (0.35 * n);

  // Record in uneven blocks, exactly as an offline renderer would.
  const std::size_t blocks[5] = {1, 7, 127, 33, 344};
  std::size_t pos = 0, bi = 0;
  while (pos < kFrames)
  {
    const std::size_t len = std::min (blocks[bi % 5], kFrames - pos);
    std::vector<const double*> in (kChannels);
    std::vector<double*> out (kChannels);
    for (std::size_t ch = 0; ch < kChannels; ++ch)
    {
      in[ch] = x[ch].data () + pos;
      out[ch] = y[ch].data () + pos;
    }
    tape.push (in.data (), len, out.data ());
    pos += len;
    ++bi;
  }
  tape.seal ();

  std::printf ("rendered %zu frames, checkpoints=%zu, saved states=%zu\n",
               tape.framesSeen (), tape.checkpointCount (),
               tape.checkpointStateCount ());

  // Synthetic error signal: pretend the target output is half the render.
  std::vector<std::vector<double> > adjoint (kChannels,
                                             std::vector<double> (kFrames, 0.0));
  for (std::size_t ch = 0; ch < kChannels; ++ch)
    for (std::size_t n = 0; n < kFrames; ++n)
      adjoint[ch][n] = y[ch][n] - 0.5 * x[ch][n];

  std::vector<const double*> ap (kChannels);
  for (std::size_t ch = 0; ch < kChannels; ++ch)
    ap[ch] = adjoint[ch].data ();

  const Dsp::LowPassTape::Gradients g = tape.backward (ap.data ());

  double inputEnergy = 0;
  for (std::size_t i = 0; i < g.input.size (); ++i)
    inputEnergy += std::fabs (g.input[i]);

  std::printf ("input gradient energy=%.6g, replayed %zu frames "
               "(peak saved state=%zu)\n",
               inputEnergy, g.replayFrames, tape.peakSavedStateCount ());

  for (std::size_t e = 0; e < events.size (); ++e)
    std::printf ("event %zu (frame %llu, stage %zu): d/dcutoff=% .6e  d/dq=% .6e\n",
                 e,
                 static_cast<unsigned long long> (events[e].frame),
                 events[e].stage,
                 g.eventCutoff[e],
                 g.eventQ[e]);

  // The same sealed segment can be differentiated again with another error.
  for (std::size_t ch = 0; ch < kChannels; ++ch)
    for (std::size_t n = 0; n < kFrames; ++n)
      adjoint[ch][n] = x[ch][n];
  for (std::size_t ch = 0; ch < kChannels; ++ch)
    ap[ch] = adjoint[ch].data ();

  const Dsp::LowPassTape::Gradients g2 = tape.backward (ap.data ());
  std::printf ("second backward: event 0 d/dcutoff=% .6e\n",
               g2.eventCutoff[0]);
  return 0;
}
