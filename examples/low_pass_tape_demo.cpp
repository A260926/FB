// Dsp::LowPassTape runnable example.
//
// Demonstrates:
//   * a multichannel (6) cascade of four RBJ low-pass stages,
//   * per-stage control events with smooth parameter ramps,
//   * block-by-block recording with push(),
//   * an offline reverse pass (backward) that maps an output error signal
//     back to every input sample and to every event's cutoff/Q.
//
// Build:
//   c++ -std=c++17 -O2 -ffp-contract=off -I. -Ishared/DSPFilters/include \
//       shared/DSPFilters/source/*.cpp examples/low_pass_tape_demo.cpp \
//       -o low_pass_tape_demo
//   ./low_pass_tape_demo

#include "DspFilters/LowPassTape.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

int main ()
{
  const double sampleRate = 48000.0;
  const std::size_t channels = 6;
  const std::size_t stages = 4;
  const std::size_t frames = 2400;
  const std::size_t blockSize = 128;

  // One track per stage, all channels share it.
  std::vector<Dsp::LowPassTape::Event> events = {
    {0,   0,  900.0, 0.70, 0},
    {0,   1, 1800.0, 0.90, 0},
    {0,   2, 3200.0, 1.10, 0},
    {0,   3, 5600.0, 1.30, 0},
    {400, 0, 1400.0, 1.20, 256},   // 256-frame ramp
    {900, 1, 2400.0, 0.60, 0},     // immediate
    {1500, 2, 2000.0, 1.80, 512},  // long ramp, still moving at the end
    {1800, 0, 1200.0, 0.80, 128}   // interrupts nothing; new ramp
  };

  Dsp::LowPassTape tape (sampleRate, channels, stages, events, /*K=*/64);

  std::vector<std::vector<double>> input (channels,
                                           std::vector<double> (frames));
  for (std::size_t c = 0; c < channels; ++c)
    for (std::size_t n = 0; n < frames; ++n)
      input[c][n] = 0.25 * std::sin (0.0013 * double (n) * (1.0 + 0.2 * double (c)))
                  + 0.02 * std::cos (0.017 * double (n + c));

  std::vector<std::vector<double>> output (channels,
                                            std::vector<double> (frames));

  std::size_t position = 0;
  while (position < frames)
  {
    const std::size_t count = std::min (blockSize, frames - position);
    std::vector<const double*> in (channels);
    std::vector<double*> out (channels);
    for (std::size_t c = 0; c < channels; ++c)
    {
      in[c] = input[c].data () + position;
      out[c] = output[c].data () + position;
    }
    tape.push (in.data (), count, out.data ());
    position += count;
  }
  tape.seal ();

  double energy = 0.0;
  for (const std::vector<double>& channel : output)
    for (double sample : channel)
      energy += sample * sample;

  // Pretend we want a flat-ish response: a simple error signal.
  std::vector<std::vector<double>> adjoint (channels,
                                             std::vector<double> (frames));
  for (std::size_t c = 0; c < channels; ++c)
    for (std::size_t n = 0; n < frames; ++n)
      adjoint[c][n] = output[c][n] * 0.5;

  std::vector<const double*> adj (channels);
  for (std::size_t c = 0; c < channels; ++c)
    adj[c] = adjoint[c].data ();

  Dsp::LowPassTape::Gradients gradients = tape.backward (adj.data ());

  double inputGradNorm = 0.0;
  for (const std::vector<double>& channel : gradients.input)
    for (double g : channel)
      inputGradNorm += g * g;

  std::printf ("frames=%zu channels=%zu stages=%zu events=%zu\n",
               tape.numFrames (), tape.numChannels (), tape.numStages (),
               tape.numEvents ());
  std::printf ("output energy = %.6f\n", energy);
  std::printf ("input gradient norm = %.6f\n", std::sqrt (inputGradNorm));
  std::printf ("replay frames = %zu (bound 2N = %zu)\n",
               tape.lastReplayFrames (), 2 * frames);
  std::printf ("peak saved state = %zu doubles\n", tape.peakSavedStateCount ());
  std::printf ("event cutoff / q gradients:\n");
  for (std::size_t i = 0; i < events.size (); ++i)
    std::printf ("  [%zu] stage=%zu frame=%llu  dCut=% .6e  dQ=% .6e\n",
                 i, events[i].stage,
                 static_cast<unsigned long long> (events[i].frame),
                 gradients.cutoff[i], gradients.q[i]);
  return 0;
}
