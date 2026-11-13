// ==================================================================================
// Calling example for fftconvolver::CooperativeFFTConvolver.
//
// The cooperative convolver keeps the exact public shape of the classic
// two-stage convolver (init / process / reset), but spreads the long-tail FFT
// work across subsequent B-sample quanta on the single calling thread. This
// example shows:
//   1. validating initialization,
//   2. feeding arbitrary-length fragments (including input == output),
//   3. zero-length calls and an empty (silent) response,
//   4. reset() returning to the uninitialized state,
//   5. how the tail work is reported when work tracing is enabled.
//
// Build (plain product build; the tail-work macro is a no-op here):
//   c++ -std=c++11 -O2 -DFFTCONVOLVER_DONT_USE_SSE -I.. \
//       cooperative_example.cpp ../CooperativeFFTConvolver.cpp \
//       ../Utilities.cpp -o cooperative_example
//
// The test harness force-includes support/work_trace.h instead, which turns
// FFTCONVOLVER_TAIL_WORK into a recorder; no source change is required.
// ==================================================================================

#include "CooperativeFFTConvolver.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

using fftconvolver::CooperativeFFTConvolver;
typedef fftconvolver::Sample Sample;

namespace
{

// A short, deterministic impulse response: a few taps spread across the head
// region and into the long tail.
std::vector<Sample> makeImpulseResponse(size_t length)
{
  std::vector<Sample> ir(length, 0.0f);
  for (size_t i = 0; i < length; ++i)
  {
    ir[i] = Sample(0.25) * std::sin(double(i) * 0.07);
  }
  return ir;
}

// Feeds `input` through `conv` in arbitrary fragments and returns the output.
// Demonstrates that the result does not depend on the host call boundaries.
std::vector<Sample> runFragmented(CooperativeFFTConvolver& conv,
                                  const std::vector<Sample>& input,
                                  const std::vector<size_t>& fragmentSizes)
{
  std::vector<Sample> output(input.size(), 0.0f);
  size_t pos = 0;
  size_t step = 0;
  while (pos < input.size())
  {
    const size_t n = std::min(fragmentSizes[step % fragmentSizes.size()],
                              input.size() - pos);
    conv.process(input.data() + pos, output.data() + pos, n);
    pos += n;
    ++step;
  }
  return output;
}

} // namespace

int main()
{
  // B = 16 samples per scheduling quantum, T = 256 samples per tail block.
  // T must be at least 8*B; here T/B = 16 quanta are available per tail job.
  const size_t headBlockSize = 16;
  const size_t tailBlockSize = 256;
  const size_t irLength = 2000; // spans the 2*T head and several tail partitions

  const std::vector<Sample> ir = makeImpulseResponse(irLength);

  CooperativeFFTConvolver convolver;
  if (!convolver.init(headBlockSize, tailBlockSize, ir.data(), ir.size()))
  {
    std::fprintf(stderr, "initialization rejected the configuration\n");
    return 1;
  }

  const size_t frameCount = 4096;
  std::vector<Sample> input(frameCount);
  for (size_t i = 0; i < frameCount; ++i)
  {
    input[i] = Sample(0.5) * std::sin(double(i) * 0.03);
  }

  // Arbitrary host call lengths, deliberately unrelated to B and T.
  const std::vector<size_t> fragments = {1, 7, headBlockSize, tailBlockSize + 3};
  const std::vector<Sample> output = runFragmented(convolver, input, fragments);

  double peak = 0.0;
  for (Sample v : output) peak = std::max(peak, double(std::fabs(v)));
  std::printf("processed %zu frames in %zu-sample fragments, peak=%.4f\n",
              frameCount, fragments.size(), peak);

  // In-place processing is supported and gives the same convolution.
  CooperativeFFTConvolver inPlace;
  inPlace.init(headBlockSize, tailBlockSize, ir.data(), ir.size());
  std::vector<Sample> inOut = input;
  size_t pos = 0, step = 0;
  while (pos < inOut.size())
  {
    const size_t n = std::min(fragments[step % fragments.size()], inOut.size() - pos);
    inPlace.process(inOut.data() + pos, inOut.data() + pos, n);
    pos += n;
    ++step;
  }
  double maxDiff = 0.0;
  for (size_t i = 0; i < output.size(); ++i)
  {
    maxDiff = std::max(maxDiff, double(std::fabs(inOut[i] - output[i])));
  }
  std::printf("in-place vs out-of-place max difference=%.3g\n", maxDiff);

  // A zero-length call must not move the sample position or start tail work;
  // null pointers are legal when len == 0.
  convolver.process(static_cast<const Sample*>(0), static_cast<Sample*>(0), 0);

  // An empty response is valid and produces silence.
  CooperativeFFTConvolver silent;
  if (!silent.init(headBlockSize, tailBlockSize, static_cast<const Sample*>(0), 0))
  {
    std::fprintf(stderr, "empty response should be accepted\n");
    return 2;
  }
  std::vector<Sample> silentOut(64, 1.0f);
  silent.process(input.data(), silentOut.data(), silentOut.size());
  bool allZero = true;
  for (Sample v : silentOut) allZero = allZero && (v == 0.0f);
  std::printf("empty response produces silence: %s\n", allZero ? "yes" : "no");

  // reset() releases all configuration and history; further calls write zeros.
  convolver.reset();
  std::vector<Sample> afterReset(32, 1.0f);
  convolver.process(afterReset.data(), afterReset.data(), afterReset.size());
  bool resetZero = true;
  for (Sample v : afterReset) resetZero = resetZero && (v == 0.0f);
  std::printf("processing after reset writes zeros: %s\n", resetZero ? "yes" : "no");

  std::printf("EXAMPLE_COMPLETE\n");
  return 0;
}
