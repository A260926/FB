// ==================================================================================
// Tests for fftconvolver::CooperativeFFTConvolver.
//
// Three groups:
//   pcm       - numerical correctness against an independent double-precision
//               time-domain reference, including sparse echoes that cross the
//               2*T head/tail boundary.
//   schedule  - the real work recorded through FFTCONVOLVER_TAIL_WORK is summed
//               per quantum and checked against the per-quantum budget, the
//               on-time delivery of tail blocks and deterministic rhythm across
//               fragmented and tail-block-spanning calls (with ring wraparound).
//   lifecycle - uninitialized/zero-length/empty-response behaviour, full
//               in-place processing, rebuild after reset, and the absence of
//               heap allocation or release while processing.
// ==================================================================================

#include "suite.h"
#include "work_trace.h"

#include "CooperativeFFTConvolver.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <vector>


namespace
{

using fftconvolver::CooperativeFFTConvolver;
typedef fftconvolver::Sample Sample;


// ----- Deterministic synthetic signal and impulse response -----

Sample synthSample(size_t i)
{
  const float v = float(int((i * 37 + 11) % 23) - 11) / 16.0f;
  return v; // always within [-1, 1]
}

Sample synthIR(size_t i)
{
  const float v = float(int((i * 53 + 7) % 19) - 9) / 16.0f;
  return v; // always within [-1, 1]
}

std::vector<Sample> makeInput(size_t len)
{
  std::vector<Sample> v(len);
  for (size_t i = 0; i < len; ++i) v[i] = synthSample(i);
  return v;
}

std::vector<Sample> makeIR(size_t len)
{
  std::vector<Sample> v(len);
  for (size_t i = 0; i < len; ++i) v[i] = synthIR(i);
  return v;
}

// Independent double-precision causal time-domain convolution.
double referenceSample(const std::vector<Sample>& ir,
                       const std::vector<Sample>& in,
                       size_t n)
{
  double acc = 0.0;
  const size_t kmax = std::min(ir.size(), n + 1);
  for (size_t k = 0; k < kmax; ++k)
  {
    acc += double(ir[k]) * double(in[n - k]);
  }
  return acc;
}

double irAbsSum(const std::vector<Sample>& ir)
{
  double s = 0.0;
  for (Sample v : ir) s += std::fabs(double(v));
  return s;
}

// Feeds a stream using a repeating chunk pattern and returns the output.
std::vector<Sample> feedChunked(CooperativeFFTConvolver& conv,
                                const std::vector<Sample>& in,
                                const std::vector<size_t>& chunks)
{
  std::vector<Sample> out(in.size(), 0.0f);
  size_t pos = 0, ci = 0;
  while (pos < in.size())
  {
    const size_t n = std::min(chunks[ci % chunks.size()], in.size() - pos);
    conv.process(in.data() + pos, out.data() + pos, n);
    pos += n;
    ++ci;
  }
  return out;
}

// Per-quantum sum of the recorded real work.
std::vector<size_t> workPerQuantum()
{
  std::vector<size_t> per;
  for (size_t i = 0; i < task::work_count(); ++i)
  {
    const task::WorkEvent e = task::work_event(i);
    while (per.size() <= e.quantum) per.push_back(0);
    per[e.quantum] += e.units;
  }
  return per;
}

// The budget formula from the specification.
size_t specBudget(size_t B, size_t T, size_t irLen)
{
  const size_t N = 2 * T;
  size_t log2n = 0;
  size_t v = N;
  while (v > 1) { v >>= 1; ++log2n; }
  const size_t tailLen = (irLen > 2 * T) ? (irLen - 2 * T) : 0;
  const size_t P = (tailLen + T - 1) / T;
  const size_t Q = T / B;
  const size_t F = (N / 2) * log2n;
  const size_t total = 2 * F + P * N + 12 * N;
  return (total + Q - 1) / Q + 16;
}

size_t partitionCount(size_t T, size_t irLen)
{
  const size_t tailLen = (irLen > 2 * T) ? (irLen - 2 * T) : 0;
  return (tailLen + T - 1) / T;
}

// Checks every output sample against the double reference and returns the
// maximum absolute error.
double maxReferenceError(const std::vector<Sample>& ir,
                         const std::vector<Sample>& in,
                         const std::vector<Sample>& out)
{
  double maxErr = 0.0;
  for (size_t n = 0; n < out.size(); ++n)
  {
    TASK_CHECK(std::isfinite(out[n]));
    const double ref = referenceSample(ir, in, n);
    maxErr = std::max(maxErr, std::fabs(double(out[n]) - ref));
  }
  return maxErr;
}

// ----- Heap allocation tracking (replaces the global allocators) -----

std::size_t g_allocEvents = 0;
std::size_t g_freeEvents = 0;

} // anonymous namespace


// Replace the global allocators so any process()-time heap activity is counted.
void* operator new(std::size_t n)
{
  ++g_allocEvents;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n)
{
  ++g_allocEvents;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
  ++g_allocEvents;
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
  ++g_allocEvents;
  return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept
{
  if (p) { ++g_freeEvents; std::free(p); }
}
void operator delete[](void* p) noexcept
{
  if (p) { ++g_freeEvents; std::free(p); }
}
void operator delete(void* p, std::size_t) noexcept
{
  if (p) { ++g_freeEvents; std::free(p); }
}
void operator delete[](void* p, std::size_t) noexcept
{
  if (p) { ++g_freeEvents; std::free(p); }
}


namespace
{

// =====================================================================
// pcm group
// =====================================================================

TASK_CASE("pcm", pcmRandomLongResponse)
{
  const size_t B = 16, T = 128;
  const size_t irLen = 1500;                 // well past 2*T = 256
  const std::vector<Sample> ir = makeIR(irLen);
  const size_t inLen = 2600;
  const std::vector<Sample> in = makeInput(inLen);

  CooperativeFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));

  const std::vector<size_t> chunks = {1, 7, B, T + 3, 5, 29};
  const std::vector<Sample> out = feedChunked(conv, in, chunks);

  const double tol = 2e-5 * (1.0 + irAbsSum(ir));
  const double maxErr = maxReferenceError(ir, in, out);
  TASK_CHECK(maxErr <= tol);
  TASK_CHECK(maxErr >= 0.0);
}

TASK_CASE("pcm", pcmSparseEchoAcrossTailBoundary)
{
  const size_t B = 8, T = 64;                // smallest legal geometry
  const size_t N = 2 * T;
  const size_t irLen = 4 * T + 5;
  std::vector<Sample> ir(irLen, 0.0f);
  // One tap inside the head region and several across the 2*T tail boundary.
  ir[0] = 0.5f;
  ir[N - 1] = 0.4f;      // last head coefficient
  ir[N] = 0.7f;          // first tail coefficient (exactly on the boundary)
  ir[2 * N + 3] = -0.6f; // deep tail coefficient
  ir[irLen - 1] = 0.3f;  // final coefficient

  const size_t inLen = 1200;
  const std::vector<Sample> in = makeInput(inLen);

  CooperativeFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));
  const std::vector<size_t> chunks = {7, B, T + 3, 1};
  const std::vector<Sample> out = feedChunked(conv, in, chunks);

  // Multi-tap correctness is fully covered by the double reference.
  const double tol = 2e-5 * (1.0 + irAbsSum(ir));
  TASK_CHECK(maxReferenceError(ir, in, out) <= tol);

  // A single tap exactly on the tail boundary must reproduce the delayed input
  // at its exact causal position, proving the first tail window opens on time.
  std::vector<Sample> oneTap(N + 1, 0.0f);
  oneTap[N] = 0.7f;
  CooperativeFFTConvolver single;
  TASK_CHECK(single.init(B, T, oneTap.data(), oneTap.size()));
  std::vector<Sample> sOut(inLen, 0.0f);
  size_t pos = 0, ci = 0;
  while (pos < inLen)
  {
    const size_t n = std::min(chunks[ci % chunks.size()], inLen - pos);
    single.process(in.data() + pos, sOut.data() + pos, n);
    pos += n; ++ci;
  }
  // Before the delay there must be silence; from N on the delayed input appears.
  for (size_t n = 0; n < N && n < inLen; ++n)
  {
    TASK_CHECK(sOut[n] == 0.0f);
  }
  const double oneTapTol = 1e-5;
  for (size_t n = N; n < inLen; ++n)
  {
    TASK_CHECK(std::fabs(double(sOut[n]) - 0.7 * double(in[n - N])) <= oneTapTol);
  }
  // Explicit first two boundary samples.
  TASK_CHECK(std::fabs(double(sOut[N]) - 0.7 * double(in[0])) <= oneTapTol);
  TASK_CHECK(std::fabs(double(sOut[N + 1]) - 0.7 * double(in[1])) <= oneTapTol);
}

TASK_CASE("pcm", pcmAllGeometryBoundaries)
{
  // Exercise the smallest and largest legal block sizes and several ratios.
  const size_t geometries[][2] = {
    {8, 64}, {8, 4096}, {256, 2048}, {32, 256}, {64, 4096}
  };
  for (size_t g = 0; g < 5; ++g)
  {
    const size_t B = geometries[g][0];
    const size_t T = geometries[g][1];
    const size_t irLen = std::min(size_t(65536), 3 * T + 17);
    const std::vector<Sample> ir = makeIR(irLen);
    const size_t inLen = std::min(size_t(5000), 4 * T + 200);
    const std::vector<Sample> in = makeInput(inLen);

    CooperativeFFTConvolver conv;
    TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));
    const std::vector<size_t> chunks = {B, 1, T + 3};
    const std::vector<Sample> out = feedChunked(conv, in, chunks);

    const double tol = 2e-5 * (1.0 + irAbsSum(ir));
    TASK_CHECK(maxReferenceError(ir, in, out) <= tol);
  }

  // Maximum legal response length (65536) with the smallest geometry, forcing
  // the largest possible number of tail partitions and spectrum-ring wraps.
  {
    const size_t B = 8, T = 64, irLen = 65536;
    const std::vector<Sample> ir = makeIR(irLen);
    const size_t inLen = 1400;
    const std::vector<Sample> in = makeInput(inLen);
    CooperativeFFTConvolver conv;
    TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));
    const std::vector<size_t> chunks = {1, 7, B, T + 3};
    const std::vector<Sample> out = feedChunked(conv, in, chunks);
    const double tol = 2e-5 * (1.0 + irAbsSum(ir));
    TASK_CHECK(maxReferenceError(ir, in, out) <= tol);
  }
}


// =====================================================================
// schedule group
// =====================================================================

TASK_CASE("schedule", scheduleLongResponseBudgetAndDeadline)
{
  const size_t B = 8, T = 64;                 // minimal geometry, many partitions
  const size_t N = 2 * T;
  const size_t irLen = 4096;                  // long response, P = 60
  const std::vector<Sample> ir = makeIR(irLen);
  const size_t P = partitionCount(T, irLen);
  TASK_CHECK(P > 1);
  const size_t budget = specBudget(B, T, irLen);

  const size_t inLen = 4096;
  const std::vector<Sample> in = makeInput(inLen);

  CooperativeFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));

  task::clear_work();
  const std::vector<size_t> chunks = {T + 3, 7, B, 1};
  const std::vector<Sample> out = feedChunked(conv, in, chunks);

  // Every quantum's real work must respect the budget.
  const std::vector<size_t> per = workPerQuantum();
  TASK_CHECK(per.size() > 0);
  size_t maxWork = 0;
  for (size_t q = 0; q < per.size(); ++q)
  {
    TASK_CHECK(per[q] <= budget);
    maxWork = std::max(maxWork, per[q]);
  }
  TASK_CHECK(maxWork > 0);
  TASK_CHECK(maxWork <= budget);

  // Only the three documented kinds may be reported.
  for (size_t i = 0; i < task::work_count(); ++i)
  {
    const task::WorkEvent e = task::work_event(i);
    const std::string kind = e.kind;
    TASK_CHECK(kind == "butterfly" || kind == "mac" || kind == "move");
    TASK_CHECK(e.units > 0);
  }

  // On-time delivery: a sparse IR whose only tail tap sits at offset 2*T must
  // reproduce input[n-2*T] exactly starting at sample 2*T (first tail window).
  std::vector<Sample> sir(2 * T + 1, 0.0f);
  sir[N] = 1.0f;
  CooperativeFFTConvolver sparse;
  TASK_CHECK(sparse.init(B, T, sir.data(), sir.size()));
  std::vector<Sample> sout(inLen, 0.0f);
  size_t pos = 0, ci = 0;
  const std::vector<size_t> sparseChunks = {1, 7, B, T + 3};
  while (pos < inLen)
  {
    const size_t n = std::min(sparseChunks[ci % sparseChunks.size()], inLen - pos);
    sparse.process(in.data() + pos, sout.data() + pos, n);
    pos += n; ++ci;
  }
  for (size_t n = N; n < inLen; ++n)
  {
    TASK_CHECK(std::fabs(double(sout[n]) - double(in[n - N])) <= 1e-5);
  }
  // The dense convolution itself must remain numerically correct.
  const double tol = 2e-5 * (1.0 + irAbsSum(ir));
  TASK_CHECK(maxReferenceError(ir, in, out) <= tol);
}

TASK_CASE("schedule", scheduleWraparoundFragmentsAndRhythm)
{
  const size_t B = 16, T = 128;
  const size_t irLen = 2 * T + 3 * T + 11;   // only P = 4 tail partitions
  const std::vector<Sample> ir = makeIR(irLen);
  const size_t P = partitionCount(T, irLen);
  TASK_CHECK(P == 4);
  const size_t budget = specBudget(B, T, irLen);

  const size_t inLen = 3200;                 // many tail blocks => ring wraparound
  const std::vector<Sample> in = makeInput(inLen);

  // Reference rhythm produced by B-aligned calls.
  std::vector<size_t> rhythmRef;
  {
    CooperativeFFTConvolver conv;
    TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));
    task::clear_work();
    const std::vector<size_t> chunks = {B};
    const std::vector<Sample> out = feedChunked(conv, in, chunks);
    rhythmRef = workPerQuantum();
    const double tol = 2e-5 * (1.0 + irAbsSum(ir));
    TASK_CHECK(maxReferenceError(ir, in, out) <= tol);
  }

  // Fragmented pattern including 1, 7 and a call (T+3) that spans a whole tail
  // block boundary.
  const std::vector<size_t> patterns[3] = {
    {1}, {7}, {T + 3, B, 1, 7}
  };
  for (int pat = 0; pat < 3; ++pat)
  {
    CooperativeFFTConvolver conv;
    TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));
    task::clear_work();
    const std::vector<Sample> out = feedChunked(conv, in, patterns[pat]);
    const std::vector<size_t> rhythm = workPerQuantum();

    // Identical sample positions => identical per-quantum work rhythm.
    TASK_CHECK(rhythm.size() == rhythmRef.size());
    for (size_t q = 0; q < rhythmRef.size(); ++q)
    {
      TASK_CHECK(rhythm[q] == rhythmRef[q]);
      TASK_CHECK(rhythm[q] <= budget);
    }
    const double tol = 2e-5 * (1.0 + irAbsSum(ir));
    TASK_CHECK(maxReferenceError(ir, in, out) <= tol);
  }

  // A trailing sub-B fragment must not start any new tail work.
  {
    CooperativeFFTConvolver conv;
    TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));
    const size_t aligned = (inLen / B) * B;
    task::clear_work();
    std::vector<Sample> out(inLen, 0.0f);
    size_t pos = 0;
    while (pos < aligned)
    {
      conv.process(in.data() + pos, out.data() + pos, B);
      pos += B;
    }
    const size_t eventsBefore = task::work_count();
    const size_t fragment = inLen - aligned;       // 0 .. B-1
    TASK_CHECK(fragment < B);
    if (fragment > 0)
    {
      conv.process(in.data() + aligned, out.data() + aligned, fragment);
    }
    TASK_CHECK(task::work_count() == eventsBefore);
  }

  // Zero-input flush: after the signal ends, keep feeding zeros. The already
  // captured tail jobs must keep running on the same quantum rhythm and the
  // reverb tail must ring out exactly as the double reference predicts, all
  // the way to silence.
  {
    CooperativeFFTConvolver conv;
    TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));

    // Build a longer input that ends in zeros within one contiguous stream.
    const size_t total = inLen + ir.size() + T;
    std::vector<Sample> extended(total, 0.0f);
    for (size_t i = 0; i < inLen; ++i) extended[i] = in[i];

    std::vector<Sample> out(total, 0.0f);
    size_t pos = 0, ci = 0;
    const std::vector<size_t> chunks = {T + 3, B, 1, 7};
    while (pos < total)
    {
      const size_t n = std::min(chunks[ci % chunks.size()], total - pos);
      conv.process(extended.data() + pos, out.data() + pos, n);
      pos += n; ++ci;
    }

    const double tol = 2e-5 * (1.0 + irAbsSum(ir));
    double tailErr = 0.0;
    for (size_t n = inLen; n < total; ++n)
    {
      const double ref = referenceSample(ir, extended, n);
      tailErr = std::max(tailErr, std::fabs(double(out[n]) - ref));
    }
    TASK_CHECK(tailErr <= tol);
    // The final samples of a fully flushed causal convolution must be (near) zero.
    TASK_CHECK(std::fabs(double(out[total - 1])) <= tol);
  }
}


// =====================================================================
// lifecycle group
// =====================================================================

TASK_CASE("lifecycle", lifecycleInvalidEmptyAndZeroLength)
{
  const size_t B = 16, T = 128;
  std::vector<Sample> ir = makeIR(100);

  // Invalid geometries must fail and leave the convolver uninitialized.
  CooperativeFFTConvolver bad;
  TASK_CHECK(!bad.init(12, T, ir.data(), ir.size()));      // B not power of 2
  TASK_CHECK(!bad.init(4, T, ir.data(), ir.size()));       // B too small
  TASK_CHECK(!bad.init(512, T, ir.data(), ir.size()));     // B too large
  TASK_CHECK(!bad.init(B, 100, ir.data(), ir.size()));     // T not power of 2
  TASK_CHECK(!bad.init(B, 32, ir.data(), ir.size()));      // T too small
  TASK_CHECK(!bad.init(B, 8192, ir.data(), ir.size()));    // T too large
  TASK_CHECK(!bad.init(64, 64, ir.data(), ir.size()));     // T < 8*B
  TASK_CHECK(!bad.init(B, T, ir.data(), 65537));           // IR too long
  TASK_CHECK(!bad.init(B, T, static_cast<const Sample*>(0), 4)); // null + len

  // Non-finite coefficients fail.
  std::vector<Sample> badIR = makeIR(50);
  badIR[25] = std::numeric_limits<Sample>::infinity();
  TASK_CHECK(!bad.init(B, T, badIR.data(), badIR.size()));
  badIR[25] = std::numeric_limits<Sample>::quiet_NaN();
  TASK_CHECK(!bad.init(B, T, badIR.data(), badIR.size()));

  // Uninitialized processing writes zeros.
  std::vector<Sample> out(32, 1.0f);
  bad.process(out.data(), out.data(), out.size());
  for (Sample v : out) TASK_CHECK(v == 0.0f);

  // Empty response is valid and means silence.
  CooperativeFFTConvolver empty;
  TASK_CHECK(empty.init(B, T, static_cast<const Sample*>(0), 0));
  std::vector<Sample> ein = makeInput(100);
  std::vector<Sample> eout(100, 1.0f);
  empty.process(ein.data(), eout.data(), ein.size());
  for (Sample v : eout) TASK_CHECK(v == 0.0f);

  // Zero-length call: null pointers allowed, no advancement, still silent.
  empty.process(static_cast<const Sample*>(0), static_cast<Sample*>(0), 0);
}

TASK_CASE("lifecycle", lifecycleRebuildInPlaceAndNoAllocation)
{
  const size_t B = 16, T = 128;
  const size_t irLen = 900;
  const std::vector<Sample> ir = makeIR(irLen);
  const size_t inLen = 1600;
  const std::vector<Sample> in = makeInput(inLen);

  CooperativeFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size()));

  // Reference out-of-place result.
  std::vector<Sample> ref = feedChunked(conv, in, std::vector<size_t>{B, T + 3, 7, 1});

  // Fully in-place processing must match bit-for-bit.
  CooperativeFFTConvolver inplace;
  TASK_CHECK(inplace.init(B, T, ir.data(), ir.size()));
  std::vector<Sample> io = in;
  size_t pos = 0, ci = 0;
  const std::vector<size_t> chunks = {B, T + 3, 7, 1};
  while (pos < inLen)
  {
    const size_t n = std::min(chunks[ci % chunks.size()], inLen - pos);
    inplace.process(io.data() + pos, io.data() + pos, n);
    pos += n; ++ci;
  }
  for (size_t i = 0; i < inLen; ++i) TASK_CHECK(io[i] == ref[i]);

  // No heap allocation or release while processing (covers ring wraparound).
  CooperativeFFTConvolver tracked;
  TASK_CHECK(tracked.init(B, T, ir.data(), ir.size()));
  std::vector<Sample> tout(inLen, 0.0f);
  const std::size_t allocsBefore = g_allocEvents;
  const std::size_t freesBefore = g_freeEvents;
  pos = 0; ci = 0;
  while (pos < inLen)
  {
    const size_t n = std::min(chunks[ci % chunks.size()], inLen - pos);
    tracked.process(in.data() + pos, tout.data() + pos, n);
    pos += n; ++ci;
  }
  TASK_CHECK(g_allocEvents == allocsBefore);
  TASK_CHECK(g_freeEvents == freesBefore);
  const double tol = 2e-5 * (1.0 + irAbsSum(ir));
  TASK_CHECK(maxReferenceError(ir, in, tout) <= tol);

  // Rebuild after reset with different geometry must work and clear history.
  conv.reset();
  std::vector<Sample> afterReset(32, 1.0f);
  conv.process(afterReset.data(), afterReset.data(), afterReset.size());
  for (Sample v : afterReset) TASK_CHECK(v == 0.0f);

  const size_t B2 = 32, T2 = 256;
  const std::vector<Sample> ir2 = makeIR(700);
  TASK_CHECK(conv.init(B2, T2, ir2.data(), ir2.size()));
  const std::vector<Sample> in2 = makeInput(1400);
  const std::vector<Sample> out2 = feedChunked(conv, in2, std::vector<size_t>{B2, 1, T2 + 3});
  TASK_CHECK(maxReferenceError(ir2, in2, out2) <= 2e-5 * (1.0 + irAbsSum(ir2)));
}

} // anonymous namespace
