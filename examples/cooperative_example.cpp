// ==================================================================================
// Minimal C++ calling example for fftconvolver::CooperativeFFTConvolver.
//
// Build (from the repository root), no audio hardware or files needed:
//
//   c++ -std=c++11 -O2 -DFFTCONVOLVER_DONT_USE_SSE -I. \
//       AudioFFT.cpp FFTConvolver.cpp TwoStageFFTConvolver.cpp \
//       Utilities.cpp CooperativeFFTConvolver.cpp \
//       examples/cooperative_example.cpp -o cooperative_example
//
// The example feeds a synthetic impulse response and signal through the
// convolver using arbitrary call lengths (including fully in-place calls),
// verifies the first audible tail sample lands exactly 2T samples after the
// first input sample, and shows reset()/re-init.
// ==================================================================================

#include "CooperativeFFTConvolver.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

int main()
{
  // B = head block size, T = tail partition size (T must be >= 8 * B).
  const size_t B = 16;
  const size_t T = 128;
  const size_t irLen = 900;

  // A response that is silent except for a marker exactly at index 2T, so
  // the first cooperative-tail contribution is trivially observable.
  std::vector<fftconvolver::Sample> ir(irLen, 0.0f);
  ir[2 * T] = 1.0f;

  fftconvolver::CooperativeFFTConvolver convolver;
  if (!convolver.init(B, T, ir.data(), ir.size()))
  {
    std::fprintf(stderr, "init failed\n");
    return 1;
  }

  // First sample is an impulse: the marker at ir[2T] must echo at sample 2T.
  const size_t total = 2 * T + 64;
  std::vector<fftconvolver::Sample> buffer(total, 0.0f);
  buffer[0] = 1.0f;

  // Drive in place with an awkward call length that crosses tail blocks.
  size_t pos = 0;
  while (pos < total)
  {
    const size_t n = std::min(size_t(37), total - pos);
    convolver.process(buffer.data() + pos, buffer.data() + pos, n);
    pos += n;
  }

  std::printf("tail marker at sample %zu: got %.6f (expect 1.0)\n",
              static_cast<size_t>(2 * T), buffer[2 * T]);
  std::printf("one sample before it:        got %.6f (expect ~0.0)\n",
              buffer[2 * T - 1]);

  // A zero-length call with null pointers is legal and changes nothing.
  convolver.process(0, 0, 0);

  // reset() discards the response and all history; process now emits zeros.
  convolver.reset();
  std::vector<fftconvolver::Sample> silent(32, 1.0f);
  convolver.process(silent.data(), silent.data(), silent.size());
  bool allZero = true;
  for (fftconvolver::Sample v : silent)
  {
    if (v != 0.0f)
    {
      allZero = false;
    }
  }
  std::printf("after reset, process writes zeros: %s\n", allZero ? "yes" : "no");

  // The object can be reconfigured afterwards.
  const bool reinitialized = convolver.init(B, T, ir.data(), ir.size());
  std::printf("re-init after reset: %s\n", reinitialized ? "ok" : "failed");

  // An empty response is a legal silent configuration.
  fftconvolver::CooperativeFFTConvolver silentConv;
  const bool emptyOk = silentConv.init(B, T, 0, 0);
  std::printf("empty (silent) response init: %s\n", emptyOk ? "ok" : "failed");

  return (std::fabs(buffer[2 * T] - 1.0f) < 1e-5f &&
          allZero && reinitialized && emptyOk) ? 0 : 2;
}
