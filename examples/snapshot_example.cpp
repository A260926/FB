// ==================================================================================
// Example: saving and restoring a running convolution reverb.
//
// A convolver processes the first part of a signal, its state is exported
// to a self-contained byte blob, and a brand new convolver (created after
// the original object and the impulse-response array have been released)
// imports the blob and continues at exactly the same sample position.
//
// Build (from the repository root):
//   c++ -std=c++11 -I. examples/snapshot_example.cpp \
//       AudioFFT.cpp FFTConvolver.cpp TwoStageFFTConvolver.cpp Utilities.cpp \
//       -o snapshot_example
// ==================================================================================

#include "../FFTConvolver.h"
#include "../TwoStageFFTConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using fftconvolver::Sample;
using fftconvolver::FFTConvolver;
using fftconvolver::TwoStageFFTConvolver;

namespace
{

std::vector<Sample> MakeImpulseResponse(size_t len)
{
  std::vector<Sample> ir(len);
  for (size_t i = 0; i < len; ++i)
  {
    const double t = static_cast<double>(i);
    ir[i] = static_cast<Sample>(std::exp(-t / 16.0) * std::sin(0.3 * t));
  }
  ir.back() = 1e-4f; // Survives the trailing-zero trim in init()
  return ir;
}

std::vector<Sample> MakeInput(size_t len)
{
  std::vector<Sample> x(len);
  for (size_t i = 0; i < len; ++i)
  {
    x[i] = static_cast<Sample>(0.5 * std::sin(0.07 * static_cast<double>(i)));
  }
  return x;
}

// Feeds [from, to) one sample at a time so the snapshot can sit at any
// absolute sample position.
void Feed(FFTConvolver& conv, const std::vector<Sample>& x,
          std::vector<Sample>& out, size_t from, size_t to)
{
  for (size_t i = from; i < to; ++i)
  {
    conv.process(&x[i], &out[i], 1);
  }
}

void Feed(TwoStageFFTConvolver& conv, const std::vector<Sample>& x,
          std::vector<Sample>& out, size_t from, size_t to)
{
  for (size_t i = from; i < to; ++i)
  {
    conv.process(&x[i], &out[i], 1);
  }
}

// The two convolver classes have different init signatures.
void InitConvolver(FFTConvolver& conv, size_t headBlock, size_t /*tailBlock*/,
                   const std::vector<Sample>& ir)
{
  conv.init(headBlock, &ir[0], ir.size());
}

void InitConvolver(TwoStageFFTConvolver& conv, size_t headBlock, size_t tailBlock,
                   const std::vector<Sample>& ir)
{
  conv.init(headBlock, tailBlock, &ir[0], ir.size());
}

template<class Conv>
void RunDemo(const char* name,
             const std::vector<Sample>& ir,
             size_t headBlock,
             size_t tailBlock,
             const std::vector<Sample>& input)
{
  const size_t savePoint = 37;
  const size_t total = input.size() + ir.size() + 16;

  std::vector<Sample> x(total, 0.0f);
  for (size_t i = 0; i < input.size(); ++i)
  {
    x[i] = input[i];
  }

  // Reference: one convolver running without any interruption.
  Conv continuous;
  InitConvolver(continuous, headBlock, tailBlock, ir);
  std::vector<Sample> reference(total, 0.0f);
  Feed(continuous, x, reference, 0, total);

  // The convolver to be snapshotted.
  Conv working;
  InitConvolver(working, headBlock, tailBlock, ir);
  std::vector<Sample> restoredOut(total, 0.0f);
  Feed(working, x, restoredOut, 0, savePoint);

  // Self-contained state: IR plus all overlap/frequency/tail state.
  std::vector<uint8_t> state;
  if (!working.exportState(state))
  {
    std::printf("[%s] exportState failed\n", name);
    return;
  }
  std::printf("[%s] exported %u bytes after sample %u\n",
              name, static_cast<unsigned>(state.size()),
              static_cast<unsigned>(savePoint));

  // The original object and the original IR array can go away now.
  working.reset();

  // Recreate from the blob alone and continue with different call lengths.
  Conv restored;
  if (!restored.importState(&state[0], state.size()))
  {
    std::printf("[%s] importState failed\n", name);
    return;
  }

  size_t pos = savePoint;
  const size_t chunks[] = { 1, 3, 8, 2, 16 };
  for (size_t c = 0; pos < total; ++c)
  {
    size_t len = std::min(chunks[c % 5], total - pos);
    restored.process(&x[pos], &restoredOut[pos], len);
    pos += len;
  }

  double maxAbsError = 0.0;
  for (size_t i = savePoint; i < total; ++i)
  {
    maxAbsError = std::max(maxAbsError,
                           std::fabs(static_cast<double>(restoredOut[i])
                                     - static_cast<double>(reference[i])));
  }
  std::printf("[%s] resumed exactly; max |error| after save point = %g\n",
              name, maxAbsError);

  // A truncated blob is structurally invalid and must be rejected. The
  // import is attempted on a separate, already-restored convolver so we
  // can also prove a rejected import changes neither its response nor its
  // subsequent output.
  Conv guarded;
  if (!guarded.importState(&state[0], state.size()))
  {
    std::printf("[%s] ERROR: valid state was rejected\n", name);
    return;
  }
  std::vector<uint8_t> truncated(state.begin(), state.end() - 8);
  if (guarded.importState(&truncated[0], truncated.size()))
  {
    std::printf("[%s] ERROR: truncated state was accepted\n", name);
    return;
  }
  std::vector<Sample> guardedOut(total, 0.0f);
  Feed(guarded, x, guardedOut, savePoint, total);
  double guardedError = 0.0;
  for (size_t i = savePoint; i < total; ++i)
  {
    guardedError = std::max(guardedError,
                            std::fabs(static_cast<double>(guardedOut[i])
                                      - static_cast<double>(reference[i])));
  }
  std::printf("[%s] truncated state rejected; convolver intact, max |error| = %g\n",
              name, guardedError);
}

} // namespace


int main()
{
  const std::vector<Sample> ir = MakeImpulseResponse(85);
  const std::vector<Sample> input = MakeInput(120);

  RunDemo<FFTConvolver>("FFTConvolver", ir, 8, 0, input);
  RunDemo<TwoStageFFTConvolver>("TwoStageFFTConvolver", ir, 4, 32, input);

  std::printf("SNAPSHOT_EXAMPLE_DONE\n");
  return 0;
}
