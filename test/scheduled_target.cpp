// ==================================================================================
// Tests for the cooperative, thread-free FFT convolver.
//
// Three groups:
//
// - "pcm":       numerical correctness against an independent double-precision
//                time-domain sum, for a synthetic signal and for sparse echoes
//                crossing the 2T head/tail boundary.
// - "schedule":  real traced work (FFTCONVOLVER_TAIL_WORK) summed per quantum,
//                checked against the configured bound over long runs with ring
//                history wrap-around, fragmented calls and calls spanning tail
//                blocks; also verifies on-time tail output and identical
//                schedules for identical signals split differently.
// - "lifecycle": fully in-place processing, zero-length calls, empty responses,
//                reset/rebuild, owned IR copies and absence of heap
//                allocation/deallocation during process().
//
// The file deliberately defines no main(): support/driver.cpp provides it.
// ==================================================================================

#include "suite.h"
#include "work_trace.h"
#include "../CooperativeFFTConvolver.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <vector>

namespace
{

// ----- Independent double-precision causal reference ------------------------

void referenceConvolve(const std::vector<float>& input,
                       const std::vector<float>& ir,
                       std::vector<double>& output)
{
  output.assign(input.size() + ir.size() - 1, 0.0);
  for (size_t n = 0; n < output.size(); ++n)
  {
    double acc = 0.0;
    const size_t kMax = std::min(ir.size(), n + 1);
    for (size_t k = 0; k < kMax; ++k)
    {
      const size_t inIdx = n - k;
      if (inIdx < input.size())
      {
        acc += static_cast<double>(ir[k]) * static_cast<double>(input[inIdx]);
      }
    }
    output[n] = acc;
  }
}

float irAbsSum(const std::vector<float>& ir)
{
  double s = 0.0;
  for (float v : ir)
  {
    s += std::fabs(static_cast<double>(v));
  }
  return static_cast<float>(s);
}

// Drives a convolver over the input using the given call chunk size, then
// drains the impulse-response tail with silence. Returns the full causal
// convolution (length inLen + irLen - 1).
std::vector<float> drive(fftconvolver::CooperativeFFTConvolver& conv,
                         const std::vector<float>& input,
                         size_t irLen,
                         size_t chunk)
{
  const size_t total = input.size() + irLen - 1;
  std::vector<float> output(total, 0.0f);
  size_t pos = 0;
  while (pos < input.size())
  {
    const size_t n = std::min(chunk, input.size() - pos);
    conv.process(input.data() + pos, output.data() + pos, n);
    pos += n;
  }
  std::vector<float> silence(chunk, 0.0f);
  while (pos < total)
  {
    const size_t n = std::min(chunk, total - pos);
    conv.process(silence.data(), output.data() + pos, n);
    pos += n;
  }
  return output;
}

size_t configBudget(size_t B, size_t T, size_t irLen)
{
  const size_t N = 2 * T;
  const size_t P = (irLen > 2 * T) ? (irLen - 2 * T + T - 1) / T : 0;
  const size_t Q = T / B;
  size_t logN = 0;
  while ((size_t(1) << logN) < N)
  {
    ++logN;
  }
  const size_t F = (N / 2) * logN;
  const size_t workTotal = 2 * F + P * N + 12 * N;
  return (workTotal + Q - 1) / Q + 16;
}

void makeSynthetic(std::vector<float>& v, size_t len, unsigned seed)
{
  v.resize(len);
  unsigned state = seed;
  for (size_t i = 0; i < len; ++i)
  {
    state = state * 1103515245u + 12345u;
    const int signed_ = static_cast<int>((state >> 16) & 0x7fff) - 0x4000;
    v[i] = static_cast<float>(signed_) / static_cast<float>(0x4000);
  }
}

} // End of anonymous namespace


// ============================================================================
//                              PCM correctness
// ============================================================================

TASK_CASE("pcm", pcm_synthetic)
{
  struct Cfg { size_t B, T, irLen, inLen; };
  const Cfg cfgs[] = {
    {8, 64, 200, 900},      // P = 2 tail partitions
    {8, 64, 401, 1200},     // P = 5
    {16, 128, 1000, 2200},  // P = 6
    {32, 256, 4096, 4000},  // P = 14
    {64, 512, 9000, 6000}   // long response, many wraps
  };
  const size_t chunks[] = {1, 7, 8, 67};

  size_t comparisons = 0;
  for (size_t ci = 0; ci < sizeof(cfgs) / sizeof(cfgs[0]); ++ci)
  {
    const Cfg& c = cfgs[ci];
    std::vector<float> ir;
    makeSynthetic(ir, c.irLen, 17u + static_cast<unsigned>(ci));
    std::vector<float> input;
    makeSynthetic(input, c.inLen, 101u + static_cast<unsigned>(ci));

    std::vector<double> reference;
    referenceConvolve(input, ir, reference);
    const double bound = 2e-5 * (1.0 + static_cast<double>(irAbsSum(ir)));

    for (size_t chi = 0; chi < sizeof(chunks) / sizeof(chunks[0]); ++chi)
    {
      fftconvolver::CooperativeFFTConvolver conv;
      TASK_CHECK(conv.init(c.B, c.T, ir.data(), ir.size()));
      const std::vector<float> got = drive(conv, input, ir.size(), chunks[chi]);
      TASK_CHECK(got.size() == reference.size());

      double maxError = 0.0;
      for (size_t n = 0; n < reference.size(); ++n)
      {
        TASK_CHECK(std::isfinite(got[n]));
        maxError = std::max(maxError, std::fabs(static_cast<double>(got[n]) - reference[n]));
      }
      TASK_CHECK(maxError <= bound);
      ++comparisons;
    }
  }
  TASK_CHECK(comparisons == sizeof(cfgs) / sizeof(cfgs[0]) *
                            (sizeof(chunks) / sizeof(chunks[0])));
}

TASK_CASE("pcm", pcm_sparse_echoes)
{
  // Sparse impulse response whose impulses sit on both sides of the 2T
  // head/tail boundary and inside different tail partitions. A single input
  // impulse must come back as exact delayed echoes.
  const size_t B = 16;
  const size_t T = 128;
  const size_t irLen = 1400;
  std::vector<float> ir(irLen, 0.0f);
  const size_t echoes[] = {0, 7, 255, 256, 257, 384, 512, 1000, 1399};
  for (size_t i = 0; i < sizeof(echoes) / sizeof(echoes[0]); ++i)
  {
    ir[echoes[i]] = (i % 2 == 0) ? 0.8f : -0.6f;
  }

  const size_t inLen = 600;
  std::vector<float> input(inLen, 0.0f);
  input[0] = 1.0f;          // Impulse at the very start
  input[300] = 0.5f;        // Second impulse crossing tail blocks later

  std::vector<double> reference;
  referenceConvolve(input, ir, reference);

  fftconvolver::CooperativeFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));
  const std::vector<float> got = drive(conv, input, ir.size(), T + 3);
  TASK_CHECK(got.size() == reference.size());

  // Every echo of the first impulse must land at its exact delay, including
  // echoes beyond 2T (i.e. produced by the cooperative tail).
  for (size_t i = 0; i < sizeof(echoes) / sizeof(echoes[0]); ++i)
  {
    const size_t at = echoes[i];
    TASK_CHECK(std::fabs(got[at] - ir[echoes[i]]) <= 2e-5f);
    if (at > 0)
    {
      // The sample before the echo is zero unless another echo sits there.
      bool neighborEcho = false;
      for (size_t j = 0; j < sizeof(echoes) / sizeof(echoes[0]); ++j)
      {
        if (echoes[j] == at - 1)
        {
          neighborEcho = true;
        }
      }
      if (!neighborEcho)
      {
        TASK_CHECK(std::fabs(got[at - 1]) <= 2e-5f);
      }
    }
  }

  // Full comparison against the double-precision time-domain sum, including
  // the region around every 2T and T partition boundary.
  const double bound = 2e-5 * (1.0 + static_cast<double>(irAbsSum(ir)));
  double maxError = 0.0;
  for (size_t n = 0; n < reference.size(); ++n)
  {
    TASK_CHECK(std::isfinite(got[n]));
    maxError = std::max(maxError, std::fabs(static_cast<double>(got[n]) - reference[n]));
  }
  TASK_CHECK(maxError <= bound);
}


// ============================================================================
//                          Schedule and work budget
// ============================================================================

namespace
{

struct TracedRun
{
  std::vector<float> output;
  std::map<uint64_t, size_t> workPerQuantum;
  std::map<std::string, size_t> workPerKind;
  size_t eventCount;
};

TracedRun tracedDrive(size_t B, size_t T, const std::vector<float>& ir,
                      const std::vector<float>& input, size_t chunk)
{
  fftconvolver::CooperativeFFTConvolver conv;
  const bool initialized = conv.init(B, T, ir.data(), ir.size());
  TASK_CHECK(initialized);

  task::clear_work();
  TracedRun run;
  run.output = drive(conv, input, ir.size(), chunk);
  run.eventCount = task::work_count();
  for (size_t e = 0; e < run.eventCount; ++e)
  {
    const task::WorkEvent event = task::work_event(e);
    TASK_CHECK(event.units > 0);
    run.workPerQuantum[event.quantum] += event.units;
    run.workPerKind[event.kind] += event.units;
  }
  return run;
}

} // End of anonymous namespace

TASK_CASE("schedule", schedule_budget_and_wraparound)
{
  // Long enough to wrap the P-slot input ring and the 3 job slots many times.
  const size_t B = 8;
  const size_t T = 64;
  const size_t irLen = 401;            // P = 5
  const size_t inLen = 4000;           // 62 tail blocks, > 12 ring wraps
  std::vector<float> ir;
  makeSynthetic(ir, irLen, 7u);
  std::vector<float> input;
  makeSynthetic(input, inLen, 23u);

  const size_t bound = configBudget(B, T, irLen);
  const TracedRun run = tracedDrive(B, T, ir, input, 7);

  // All three work kinds must come from real loops of both FFTs and the MAC.
  TASK_CHECK(run.workPerKind.count("butterfly") == 1);
  TASK_CHECK(run.workPerKind.count("mac") == 1);
  TASK_CHECK(run.workPerKind.count("move") == 1);
  TASK_CHECK(run.workPerKind.find("butterfly")->second > 0);
  TASK_CHECK(run.workPerKind.find("mac")->second > 0);
  TASK_CHECK(run.workPerKind.find("move")->second > 0);

  // Quantum keys are strictly increasing (idle quanta simply carry no
  // events: the first job only launches once a full T-sample block is
  // captured, at the end of quantum Q-1 = T/B - 1), and every active
  // quantum stays within the work bound.
  uint64_t previousQuantum = 0;
  bool first = true;
  size_t activeQuanta = 0;
  for (std::map<uint64_t, size_t>::const_iterator it = run.workPerQuantum.begin();
       it != run.workPerQuantum.end(); ++it)
  {
    if (!first)
    {
      TASK_CHECK(it->first > previousQuantum);
    }
    first = false;
    previousQuantum = it->first;
    TASK_CHECK(it->second <= bound);
    ++activeQuanta;
  }
  TASK_CHECK(activeQuanta > 20);
  TASK_CHECK(run.workPerQuantum.begin()->first == T / B - 1);

  // On-time output: the full result matches the double reference, which also
  // covers zero-input draining after the input signal ends.
  std::vector<double> reference;
  referenceConvolve(input, ir, reference);
  const double errorBound = 2e-5 * (1.0 + static_cast<double>(irAbsSum(ir)));
  for (size_t n = 0; n < reference.size(); ++n)
  {
    TASK_CHECK(std::fabs(static_cast<double>(run.output[n]) - reference[n]) <= errorBound);
  }

  // Explicit deadline probe: with a coefficient at index 2T, its echo of the
  // first sample must be ready exactly at sample 2T.
  std::vector<float> echoIr(2 * T + 1, 0.0f);
  echoIr[2 * T] = 1.0f;
  std::vector<float> impulse(1, 1.0f);
  fftconvolver::CooperativeFFTConvolver echoConv;
  TASK_CHECK(echoConv.init(B, T, echoIr.data(), echoIr.size()));
  const std::vector<float> echoOut = drive(echoConv, impulse, echoIr.size(), B);
  TASK_CHECK(std::fabs(echoOut[2 * T] - 1.0f) <= 2e-5f);
  for (size_t n = 0; n < 2 * T; ++n)
  {
    TASK_CHECK(std::fabs(echoOut[n]) <= 2e-5f);
  }
}

TASK_CASE("schedule", schedule_fragments_are_identical)
{
  const size_t B = 32;
  const size_t T = 256;
  const size_t irLen = 2000;          // P = 6
  const size_t inLen = 3000;
  std::vector<float> ir;
  makeSynthetic(ir, irLen, 31u);
  std::vector<float> input;
  makeSynthetic(input, inLen, 47u);

  const size_t chunks[] = {1, 7, B, T + 3};
  std::vector<TracedRun> runs;
  for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i)
  {
    runs.push_back(tracedDrive(B, T, ir, input, chunks[i]));
  }

  // PCM identity across call splits (within the numerical tolerance: the
  // short-block head accumulates in different buffer-fill orders) and an
  // exactly identical per-quantum real-work rhythm (the actual requirement
  // on sample positions and tail scheduling).
  const double splitBound = 2e-5 * (1.0 + static_cast<double>(irAbsSum(ir)));
  for (size_t r = 1; r < runs.size(); ++r)
  {
    TASK_CHECK(runs[r].output.size() == runs[0].output.size());
    TASK_CHECK(runs[r].workPerQuantum.size() == runs[0].workPerQuantum.size());
    for (size_t n = 0; n < runs[0].output.size(); ++n)
    {
      TASK_CHECK(std::fabs(static_cast<double>(runs[r].output[n]) -
                           static_cast<double>(runs[0].output[n])) <= splitBound);
    }
    std::map<uint64_t, size_t>::const_iterator a = runs[0].workPerQuantum.begin();
    std::map<uint64_t, size_t>::const_iterator b = runs[r].workPerQuantum.begin();
    for (; a != runs[0].workPerQuantum.end(); ++a, ++b)
    {
      TASK_CHECK(a->first == b->first);
      TASK_CHECK(a->second == b->second);
    }
  }

  // Every quantum stays within the bound even for the most fragmented calls.
  const size_t bound = configBudget(B, T, irLen);
  for (size_t r = 0; r < runs.size(); ++r)
  {
    for (std::map<uint64_t, size_t>::const_iterator it = runs[r].workPerQuantum.begin();
         it != runs[r].workPerQuantum.end(); ++it)
    {
      TASK_CHECK(it->second <= bound);
    }
  }
}

TASK_CASE("schedule", schedule_zero_length_does_not_advance)
{
  const size_t B = 16;
  const size_t T = 128;
  std::vector<float> ir;
  makeSynthetic(ir, 700, 5u);
  std::vector<float> input;
  makeSynthetic(input, 1500, 9u);

  fftconvolver::CooperativeFFTConvolver a;
  fftconvolver::CooperativeFFTConvolver b;
  TASK_CHECK(a.init(B, T, ir.data(), ir.size()));
  TASK_CHECK(b.init(B, T, ir.data(), ir.size()));

  task::clear_work();
  std::vector<float> outA(input.size());
  for (size_t pos = 0; pos < input.size(); pos += B)
  {
    const size_t n = std::min(B, input.size() - pos);
    a.process(input.data() + pos, outA.data() + pos, n);
  }
  const size_t eventsWithoutEmpty = task::work_count();

  // Interleave zero-length calls (null pointers allowed): they must neither
  // move the sample position nor create tail work.
  task::clear_work();
  std::vector<float> outB(input.size());
  for (size_t pos = 0; pos < input.size(); pos += B)
  {
    const size_t n = std::min(B, input.size() - pos);
    b.process(0, 0, 0);
    b.process(input.data() + pos, outB.data() + pos, n);
    b.process(0, 0, 0);
  }
  TASK_CHECK(task::work_count() == eventsWithoutEmpty);
  for (size_t n = 0; n < input.size(); ++n)
  {
    TASK_CHECK(outA[n] == outB[n]);
  }
}


// ============================================================================
//                               Lifecycle
// ============================================================================

namespace
{

long g_newCalls = 0;
long g_deleteCalls = 0;

} // End of anonymous namespace

// Process-wide counters: init() is allowed to allocate, process() is not.
void* operator new(std::size_t size)
{
  ++g_newCalls;
  void* p = std::malloc(size);
  if (!p)
  {
    throw std::bad_alloc();
  }
  return p;
}

void operator delete(void* p) noexcept
{
  if (p)
  {
    ++g_deleteCalls;
    std::free(p);
  }
}

TASK_CASE("lifecycle", lifecycle_inplace_empty_and_uninitialized)
{
  const size_t B = 16;
  const size_t T = 128;
  std::vector<float> ir;
  makeSynthetic(ir, 900, 13u);
  std::vector<float> input;
  makeSynthetic(input, 2000, 21u);

  // Reference run with separate buffers.
  fftconvolver::CooperativeFFTConvolver ref;
  TASK_CHECK(ref.init(B, T, ir.data(), ir.size()));
  const std::vector<float> expected = drive(ref, input, ir.size(), 37);

  // Fully in-place: one buffer carries input, then zeros for draining.
  std::vector<float> buffer(expected.size(), 0.0f);
  for (size_t i = 0; i < input.size(); ++i)
  {
    buffer[i] = input[i];
  }
  fftconvolver::CooperativeFFTConvolver inplace;
  TASK_CHECK(inplace.init(B, T, ir.data(), ir.size()));
  size_t pos = 0;
  while (pos < expected.size())
  {
    const size_t n = std::min(size_t(37), expected.size() - pos);
    if (pos >= input.size())
    {
      for (size_t i = pos; i < pos + n; ++i)
      {
        buffer[i] = 0.0f;
      }
    }
    inplace.process(buffer.data() + pos, buffer.data() + pos, n);
    pos += n;
  }
  for (size_t n = 0; n < expected.size(); ++n)
  {
    TASK_CHECK(std::fabs(buffer[n] - expected[n]) <=
               2e-5f * (1.0f + irAbsSum(ir)));
  }

  // Empty response is legal and produces silence.
  fftconvolver::CooperativeFFTConvolver silent;
  TASK_CHECK(silent.init(B, T, 0, 0));
  std::vector<float> silentOut(100, 7.0f);
  silent.process(input.data(), silentOut.data(), silentOut.size());
  for (float v : silentOut)
  {
    TASK_CHECK(v == 0.0f);
  }

  // Uninitialized convolver writes zeros.
  fftconvolver::CooperativeFFTConvolver uninitialized;
  std::vector<float> zeros(50, 3.0f);
  uninitialized.process(input.data(), zeros.data(), zeros.size());
  for (float v : zeros)
  {
    TASK_CHECK(v == 0.0f);
  }
}

TASK_CASE("lifecycle", lifecycle_rebuild_owned_copy_and_no_alloc)
{
  const size_t B = 8;
  const size_t T = 64;
  std::vector<float> ir;
  makeSynthetic(ir, 401, 3u);
  std::vector<float> input;
  makeSynthetic(input, 1600, 11u);

  fftconvolver::CooperativeFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));
  const std::vector<float> expected = drive(conv, input, ir.size(), B);

  // The convolver owns its response copy: zeroing the caller's array after
  // init must not change the output of an already initialized convolver.
  fftconvolver::CooperativeFFTConvolver convCopy;
  TASK_CHECK(convCopy.init(B, T, ir.data(), ir.size()));
  for (float& v : ir)
  {
    v = 0.0f;
  }
  const std::vector<float> afterMutation = drive(convCopy, input, 401, B);
  TASK_CHECK(afterMutation.size() == expected.size());
  for (size_t n = 0; n < expected.size(); ++n)
  {
    TASK_CHECK(afterMutation[n] == expected[n]);
  }

  // No heap activity while processing (the counters are sampled after init
  // and after every test-side buffer has been allocated).
  fftconvolver::CooperativeFFTConvolver measured;
  std::vector<float> ir2;
  makeSynthetic(ir2, 401, 3u);
  TASK_CHECK(measured.init(B, T, ir2.data(), ir2.size()));
  std::vector<float> out(input.size());
  const long newBefore = g_newCalls;
  const long deleteBefore = g_deleteCalls;
  for (size_t p = 0; p < input.size(); p += 13)
  {
    const size_t n = std::min(size_t(13), input.size() - p);
    measured.process(input.data() + p, out.data() + p, n);
  }
  TASK_CHECK(g_newCalls == newBefore);
  TASK_CHECK(g_deleteCalls == deleteBefore);

  // reset releases the configuration and history; process then writes zeros,
  // and a fresh init rebuilds a working convolver.
  measured.reset();
  std::vector<float> afterReset(64, 4.0f);
  measured.process(input.data(), afterReset.data(), afterReset.size());
  for (float v : afterReset)
  {
    TASK_CHECK(v == 0.0f);
  }
  TASK_CHECK(measured.init(B, T, ir2.data(), ir2.size()));
  const std::vector<float> rebuilt = drive(measured, input, ir2.size(), T + 3);
  const std::vector<float> expected2 = [&] {
    fftconvolver::CooperativeFFTConvolver c;
    c.init(B, T, ir2.data(), ir2.size());
    return drive(c, input, ir2.size(), B);
  }();
  TASK_CHECK(rebuilt.size() == expected2.size());
  const double bound = 2e-5 * (1.0 + static_cast<double>(irAbsSum(ir2)));
  for (size_t n = 0; n < expected2.size(); ++n)
  {
    TASK_CHECK(std::fabs(static_cast<double>(rebuilt[n]) -
                         static_cast<double>(expected2[n])) <= bound);
  }
}
