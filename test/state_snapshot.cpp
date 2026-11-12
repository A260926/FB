// ==================================================================================
// State snapshot tests for FFTConvolver and TwoStageFFTConvolver.
//
// The tests verify that exportState()/importState() resume a convolver at
// exactly the absolute sample position where the snapshot was taken, with
// every accumulated overlap, frequency-domain history and (for the
// two-stage convolver) tail timing relationship preserved. All audio
// results are cross-checked against an independent time-domain convolution
// and against a convolver that runs continuously without snapshots.
// ==================================================================================

#include "../AudioFFT.h"
#include "../FFTConvolver.h"
#include "../TwoStageFFTConvolver.h"
#include "../Snapshot.h"
#include "../Utilities.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>


using fftconvolver::Sample;
using fftconvolver::FFTConvolver;
using fftconvolver::TwoStageFFTConvolver;

namespace
{

// ----------------------------------------------------------------------------
// Heap accounting: the real-time contract says process() neither allocates
// nor frees heap memory, including for the very first block after a state
// import. The global new/delete operators below count every heap operation
// in the final binary.
// ----------------------------------------------------------------------------

size_t gNewCount = 0;
size_t gDeleteCount = 0;

struct AllocGuard
{
  size_t newCount;
  size_t deleteCount;

  AllocGuard() :
    newCount(gNewCount),
    deleteCount(gDeleteCount)
  {
  }

  bool unchanged() const
  {
    return gNewCount == newCount && gDeleteCount == deleteCount;
  }
};

// ----------------------------------------------------------------------------
// Small test framework
// ----------------------------------------------------------------------------

int gFailures = 0;
int gChecks = 0;

void Fail(const char* expr, const char* file, int line)
{
  ++gFailures;
  std::printf("FAIL: %s (%s:%d)\n", expr, file, line);
}

#define REQUIRE(cond) \
  do { \
    ++gChecks; \
    if (!(cond)) Fail(#cond, __FILE__, __LINE__); \
  } while (0)


// Deterministic LCG so the test itself never depends on rand() state.
class Lcg
{
public:
  explicit Lcg(uint32_t seed) : _state(seed)
  {
  }

  uint32_t next()
  {
    _state = _state * 1664525u + 1013904223u;
    return _state;
  }

  // Value in [0, max] (max itself may be returned).
  size_t bounded(size_t max)
  {
    return static_cast<size_t>(next() % static_cast<uint32_t>(max + 1u));
  }

private:
  uint32_t _state;
};


// Yields process() chunk sizes from a deterministic pattern, occasionally
// including zero-length calls.
class Feeder
{
public:
  Feeder(uint32_t seed, size_t maxChunk) :
    _pattern(),
    _index(0)
  {
    Lcg lcg(seed);
    for (size_t i = 0; i < 24; ++i)
    {
      size_t v = lcg.bounded(maxChunk);
      if ((i % 7u) == 3u)
      {
        v = 0; // Periodic zero-length processing call
      }
      _pattern.push_back(v);
    }
    // Guarantee progress is always possible eventually.
    _pattern.push_back(1);
  }

  size_t next(size_t remaining)
  {
    const size_t v = _pattern[_index % _pattern.size()];
    ++_index;
    return std::min(v, remaining);
  }

private:
  std::vector<size_t> _pattern;
  size_t _index;
};


void initConv(FFTConvolver& conv,
              size_t headBlock,
              size_t /*tailBlock*/,
              const Sample* ir,
              size_t irLen)
{
  REQUIRE(conv.init(headBlock, ir, irLen));
}

void initConv(TwoStageFFTConvolver& conv,
              size_t headBlock,
              size_t tailBlock,
              const Sample* ir,
              size_t irLen)
{
  REQUIRE(conv.init(headBlock, tailBlock, ir, irLen));
}


// Independent time-domain reference convolution (double precision).
std::vector<double> ReferenceConvolve(const std::vector<float>& input,
                                      const std::vector<float>& ir)
{
  const size_t inLen = input.size();
  const size_t outLen = inLen + ir.size() - 1u;
  std::vector<double> out(outLen, 0.0);
  for (size_t n = 0; n < outLen; ++n)
  {
    double acc = 0.0;
    // k indexes the IR; the input index n-k must stay inside [0, inLen).
    const size_t kBegin = (n + 1u > inLen) ? (n + 1u - inLen) : 0u;
    const size_t kEnd = std::min(ir.size(), n + 1u);
    for (size_t k = kBegin; k < kEnd; ++k)
    {
      acc += static_cast<double>(ir[k]) * static_cast<double>(input[n - k]);
    }
    out[n] = acc;
  }
  return out;
}


// Required sample agreement: abs error <= 1e-5, plus relative error <= 1e-4.
bool SamplesClose(float produced, double reference)
{
  const double absError = std::fabs(static_cast<double>(produced) - reference);
  return absError <= 1e-5 + 1e-4 * std::fabs(reference);
}


// Feeds [from, to) of [x] through the convolver using arbitrary chunk sizes.
template<class Conv>
void FeedRange(Conv& conv,
               const float* x,
               float* out,
               size_t from,
               size_t to,
               Feeder& feeder)
{
  size_t pos = from;
  while (pos < to)
  {
    const size_t len = feeder.next(to - pos);
    conv.process(x + pos, out + pos, len);
    pos += len; // len == 0 is fine: the feeder index still advances
  }
}


// ----------------------------------------------------------------------------
// Signal construction
// ----------------------------------------------------------------------------

std::vector<float> BuildContinuousSignal(size_t len)
{
  std::vector<float> x(len);
  for (size_t i = 0; i < len; ++i)
  {
    const double t = static_cast<double>(i);
    x[i] = static_cast<float>(0.5 * std::sin(0.07 * t)
                             + 0.3 * std::sin(0.013 * t + 1.3)
                             + 0.1 * std::cos(0.21 * t));
  }
  return x;
}


std::vector<float> BuildSparsePulses(size_t len)
{
  std::vector<float> x(len, 0.0f);
  Lcg lcg(0x5EED1234u);
  size_t pos = 0;
  float sign = 1.0f;
  while (pos < len)
  {
    x[pos] = sign * (0.4f + 0.5f * static_cast<float>(lcg.bounded(100)) / 100.0f);
    sign = -sign;
    // Occasionally place two adjacent pulses.
    if (lcg.bounded(3u) == 0u && pos + 1u < len)
    {
      x[pos + 1u] = -0.5f * sign;
      pos += 1u;
    }
    pos += 3u + lcg.bounded(11u);
  }
  return x;
}


// Short energetic head, long decaying tail. The caller picks the length so
// that the tail crosses the two-stage boundaries.
std::vector<float> BuildImpulseResponse(size_t len)
{
  std::vector<float> ir(len);
  for (size_t i = 0; i < len; ++i)
  {
    const double t = static_cast<double>(i);
    ir[i] = static_cast<float>(std::exp(-t / 18.0)
                               * (0.8 * std::sin(0.31 * t)
                                  + 0.25 * std::cos(0.11 * t + 0.7)));
  }
  // Guarantee the last sample survives the convolver's trailing-zero trim.
  if (std::fabs(ir.back()) < 1e-5f)
  {
    ir.back() = 1.234567e-4f;
  }
  return ir;
}


// ----------------------------------------------------------------------------
// Core audio round-trip: one instance runs continuously, another is
// exported at sample [savePoint] and imported into a brand new object.
// ----------------------------------------------------------------------------

template<class Conv>
bool RunAudioCase(const char* label,
                  const std::vector<float>& input,
                  const std::vector<float>& irSource,
                  size_t headBlock,
                  size_t tailBlock,
                  size_t savePoint,
                  uint32_t seedContinuous,
                  uint32_t seedBefore,
                  uint32_t seedAfter,
                  size_t totalFeed)
{
  // Local, releasable copies: nothing imported may rely on this memory.
  const size_t irLen = irSource.size();
  std::vector<float>* irOwner = new std::vector<float>(irSource);
  const std::vector<float>& ir = *irOwner;

  std::vector<float> x(totalFeed, 0.0f);
  for (size_t i = 0; i < input.size() && i < totalFeed; ++i)
  {
    x[i] = input[i];
  }

  const std::vector<double> reference = ReferenceConvolve(x, ir);

  std::vector<float> contOut(totalFeed, 0.0f);
  std::vector<float> snapOut(totalFeed, 0.0f);

  // Instance A: runs the whole signal without any snapshot.
  Conv continuous;
  initConv(continuous, headBlock, tailBlock, &ir[0], ir.size());
  {
    Feeder feeder(seedContinuous, 13u);
    FeedRange(continuous, &x[0], &contOut[0], 0u, totalFeed, feeder);
  }

  // Instance B: runs up to the save point, then is snapshotted and destroyed.
  Conv* snapshotConv = new Conv();
  initConv(*snapshotConv, headBlock, tailBlock, &ir[0], ir.size());
  {
    Feeder feeder(seedBefore, 9u);
    FeedRange(*snapshotConv, &x[0], &snapOut[0], 0u, savePoint, feeder);
  }

  std::vector<uint8_t> blob1;
  std::vector<uint8_t> blob2;
  REQUIRE(snapshotConv->exportState(blob1));
  REQUIRE(!blob1.empty());

  // Exporting must not advance the audio state and must be deterministic.
  REQUIRE(snapshotConv->exportState(blob2));
  REQUIRE(blob1 == blob2);

  // The same absolute sample position reached through different chunking
  // must serialize to identical bytes.
  {
    Conv twin;
    std::vector<float> twinOut(totalFeed, 0.0f);
    initConv(twin, headBlock, tailBlock, &ir[0], ir.size());
    Feeder otherFeeder(seedBefore ^ 0x9E3779B9u, 7u);
    FeedRange(twin, &x[0], &twinOut[0], 0u, savePoint, otherFeeder);
    std::vector<uint8_t> blobTwin;
    REQUIRE(twin.exportState(blobTwin));
    REQUIRE(blobTwin == blob1);
    for (size_t i = 0; i < savePoint; ++i)
    {
      REQUIRE(SamplesClose(twinOut[i], static_cast<double>(contOut[i])));
    }
  }

  // From here on neither the original instance nor the IR array exists.
  delete snapshotConv;
  delete irOwner;

  // Import into a fresh object, then continue with completely different
  // call lengths than before the snapshot.
  Conv* restored = new Conv();
  REQUIRE(restored->importState(&blob1[0], blob1.size()));

  // The first small blocks after recovery must not touch the heap at all.
  {
    const size_t probeEnd = std::min(totalFeed, savePoint + 8u);
    Feeder probe(seedAfter, 5u);
    AllocGuard guard;
    FeedRange(*restored, &x[0], &snapOut[0], savePoint, probeEnd, probe);
    REQUIRE(guard.unchanged());
  }
  {
    Feeder feeder(seedAfter, 17u);
    FeedRange(*restored, &x[0], &snapOut[0],
              std::min(totalFeed, savePoint + 8u), totalFeed, feeder);
  }

  // Every output sample: continuous instance vs. restored instance, and
  // both vs. the time-domain reference.
  for (size_t i = 0; i < totalFeed; ++i)
  {
    if (!SamplesClose(snapOut[i], static_cast<double>(contOut[i])) ||
        !SamplesClose(contOut[i], reference[i]) ||
        !SamplesClose(snapOut[i], reference[i]))
    {
      std::printf("FAIL: audio mismatch in case '%s' at sample %u "
                  "(continuous=%f snapshot=%f reference=%f)\n",
                  label, static_cast<unsigned>(i),
                  static_cast<double>(contOut[i]),
                  static_cast<double>(snapOut[i]),
                  reference[i]);
      ++gFailures;
      break;
    }
  }

  // Repeated import of the same bytes into the very same object must always
  // land at the same save point again.
  std::vector<float> reimportOut(totalFeed, 0.0f);
  REQUIRE(restored->importState(&blob1[0], blob1.size()));
  {
    Feeder feeder(seedAfter ^ 0x12345u, 11u);
    FeedRange(*restored, &x[0], &reimportOut[0], savePoint, totalFeed, feeder);
  }
  for (size_t i = savePoint; i < totalFeed; ++i)
  {
    REQUIRE(SamplesClose(reimportOut[i], static_cast<double>(contOut[i])));
  }

  // Import target that has already processed a completely different signal.
  {
    Conv busy;
    std::vector<float> otherIr(irLen > 5u ? irLen : 6u, 0.0f);
    for (size_t i = 0; i < otherIr.size(); ++i)
    {
      otherIr[i] = static_cast<float>(i % 3u) * 0.25f + 0.1f;
    }
    initConv(busy,
             std::max(size_t(1), headBlock / 2u),
             std::max(headBlock + 1u, tailBlock / 2u),
             &otherIr[0], otherIr.size());
    std::vector<float> noise(50u, 0.0f);
    std::vector<float> noiseOut(50u, 0.0f);
    for (size_t i = 0; i < noise.size(); ++i)
    {
      noise[i] = static_cast<float>(static_cast<int>(i * 37u % 11u) - 5u) * 0.07f;
    }
    busy.process(&noise[0], &noiseOut[0], noise.size());

    REQUIRE(busy.importState(&blob1[0], blob1.size()));
    std::vector<float> busyOut(totalFeed, 0.0f);
    Feeder feeder(seedAfter ^ 0xABCDEFu, 6u);
    FeedRange(busy, &x[0], &busyOut[0], savePoint, totalFeed, feeder);
    for (size_t i = savePoint; i < totalFeed; ++i)
    {
      REQUIRE(SamplesClose(busyOut[i], static_cast<double>(contOut[i])));
    }
  }

  delete restored;
  return gFailures == 0;
}


// Runs the full matrix of save point positions for one convolver type.
template<class Conv>
void RunSavePointMatrix(const char* label,
                        const std::vector<float>& input,
                        const std::vector<float>& ir,
                        size_t headBlock,
                        size_t tailBlock)
{
  const size_t flush = ir.size() + 2u * tailBlock + 4u;
  const size_t totalFeed = input.size() + flush;

  // Deliberately covers: mid head partition, partition boundary, sample
  // before a tail block boundary, the boundary itself (right after the
  // precalculated-tail swap in the two-stage convolver), and odd offsets.
  std::vector<size_t> savePoints;
  savePoints.push_back(1u);
  savePoints.push_back(headBlock / 2u);
  savePoints.push_back(headBlock - 1u);
  savePoints.push_back(headBlock);
  savePoints.push_back(headBlock + headBlock / 2u);
  savePoints.push_back(tailBlock - 1u);
  savePoints.push_back(tailBlock);
  savePoints.push_back(tailBlock + 1u);
  savePoints.push_back(2u * tailBlock - 1u);
  savePoints.push_back(2u * tailBlock);
  savePoints.push_back(2u * tailBlock + headBlock / 2u);
  savePoints.push_back(3u * tailBlock + 3u);

  uint32_t run = 0;
  for (size_t s = 0; s < savePoints.size(); ++s)
  {
    const size_t savePoint = savePoints[s];
    if (savePoint >= totalFeed - flush)
    {
      continue;
    }
    RunAudioCase<Conv>(label, input, ir, headBlock, tailBlock, savePoint,
                       0x1000u + run * 17u,
                       0x2000u + run * 31u,
                       0x3000u + run * 47u,
                       totalFeed);
    ++run;
  }
}


// ----------------------------------------------------------------------------
// Corruption handling: rejected imports must leave the target untouched.
// ----------------------------------------------------------------------------

void PutU32(std::vector<uint8_t>& blob, size_t offset, uint32_t value)
{
  REQUIRE(offset + 4u <= blob.size());
  blob[offset + 0u] = static_cast<uint8_t>(value & 0xFFu);
  blob[offset + 1u] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  blob[offset + 2u] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  blob[offset + 3u] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void PutU16(std::vector<uint8_t>& blob, size_t offset, uint16_t value)
{
  REQUIRE(offset + 2u <= blob.size());
  blob[offset + 0u] = static_cast<uint8_t>(value & 0xFFu);
  blob[offset + 1u] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void PutFloatBits(std::vector<uint8_t>& blob, size_t offset, uint32_t bits)
{
  PutU32(blob, offset, bits);
}


// Feeds identical chunks to two convolvers and requires equal results.
template<class Conv>
void RequireIdenticalAudio(Conv& a, Conv& b, const std::vector<float>& x)
{
  std::vector<float> outA(x.size(), 0.0f);
  std::vector<float> outB(x.size(), 0.0f);
  Feeder feeder(0x7777u, 7u);
  size_t pos = 0;
  while (pos < x.size())
  {
    const size_t len = feeder.next(x.size() - pos);
    AllocGuard guard;
    a.process(&x[pos], &outA[pos], len);
    b.process(&x[pos], &outB[pos], len);
    REQUIRE(guard.unchanged());
    pos += len;
  }
  for (size_t i = 0; i < x.size(); ++i)
  {
    REQUIRE(SamplesClose(outA[i], static_cast<double>(outB[i])));
  }
}


template<class Conv>
void RequireRejected(Conv& target,
                     const std::vector<uint8_t>& bad,
                     const char* what)
{
  const bool accepted = target.importState(bad.empty() ? 0 : &bad[0], bad.size());
  if (accepted)
  {
    std::printf("FAIL: corrupt state accepted (%s)\n", what);
    ++gFailures;
  }
}


template<class Conv>
void RunCorruptionBattery(const char* label,
                          const std::vector<uint8_t>& valid,
                          const std::vector<float>& ir,
                          size_t headBlock,
                          size_t tailBlock,
                          const std::vector<float>& signal)
{
  // Healthy twin used to prove rejected imports change nothing.
  Conv target;
  Conv twin;
  initConv(target, headBlock, tailBlock, &ir[0], ir.size());
  initConv(twin, headBlock, tailBlock, &ir[0], ir.size());
  std::vector<float> warm(40u);
  std::vector<float> warmOutA(40u, 0.0f);
  std::vector<float> warmOutB(40u, 0.0f);
  for (size_t i = 0; i < warm.size(); ++i)
  {
    warm[i] = static_cast<float>(static_cast<int>(i * 13u % 9u) - 4u) * 0.11f;
  }
  target.process(&warm[0], &warmOutA[0], warm.size());
  twin.process(&warm[0], &warmOutB[0], warm.size());

  // Null / zero-length arguments.
  REQUIRE(!target.importState(0, valid.size()));
  REQUIRE(!target.importState(&valid[0], 0u));

  // Truncation at a range of lengths, including "length validated before
  // any allocation" boundary cases.
  const size_t truncations[] = { 0u, 1u, 4u, 11u, 12u,
                                 valid.size() / 2u,
                                 valid.size() - 1u,
                                 valid.size() - 5u };
  for (size_t t = 0; t < sizeof(truncations) / sizeof(truncations[0]); ++t)
  {
    if (truncations[t] >= valid.size())
    {
      continue;
    }
    std::vector<uint8_t> bad(valid.begin(), valid.begin() + truncations[t]);
    RequireRejected(target, bad, "truncated");
  }

  // Unknown format version.
  {
    std::vector<uint8_t> bad = valid;
    bad[4] = 99u;
    RequireRejected(target, bad, "unknown version");
  }

  // Non-zero reserved field.
  {
    std::vector<uint8_t> bad = valid;
    PutU16(bad, 6u, 1u);
    RequireRejected(target, bad, "reserved field");
  }

  // Contradictory outer payload length.
  {
    std::vector<uint8_t> bad = valid;
    PutU32(bad, 8u, static_cast<uint32_t>(valid.size()));
    RequireRejected(target, bad, "payload length mismatch");
  }

  // First section declares a gigantic body: must be rejected before any
  // allocation of that size.
  {
    std::vector<uint8_t> bad = valid;
    PutU32(bad, 13u, 0xFFFFFFFFu);
    RequireRejected(target, bad, "overflow section length");
  }

  // FFT section body starts at offset 17; fields are:
  // flags(0) blockSize(4) segCount(8) current(12) inputFill(16) irCount(20)
  {
    std::vector<uint8_t> bad = valid;
    PutU32(bad, 17u + 4u, 7u); // blockSize: not a power of 2
    RequireRejected(target, bad, "block size not power of two");
  }
  {
    std::vector<uint8_t> bad = valid;
    PutU32(bad, 17u + 8u, 0u); // segCount: must not be zero
    RequireRejected(target, bad, "zero segment count");
  }
  {
    std::vector<uint8_t> bad = valid;
    PutU32(bad, 17u + 16u, 0xFFFFFFFFu); // inputFill beyond block
    RequireRejected(target, bad, "input fill overflow");
  }
  {
    std::vector<uint8_t> bad = valid;
    PutU32(bad, 17u + 20u, 0xFFFFFFFFu); // IR length overflow
    RequireRejected(target, bad, "IR count overflow");
  }

  // Non-finite sample values (NaN and +Inf) in the last stored float.
  {
    std::vector<uint8_t> nan = valid;
    PutFloatBits(nan, nan.size() - 4u, 0x7FC00000u);
    RequireRejected(target, nan, "NaN sample");

    std::vector<uint8_t> inf = valid;
    PutFloatBits(inf, inf.size() - 4u, 0x7F800000u);
    RequireRejected(target, inf, "infinite sample");
  }

  // Garbage of various sizes.
  {
    Lcg lcg(0xBADF00Du);
    for (size_t trial = 0; trial < 8u; ++trial)
    {
      std::vector<uint8_t> garbage(12u + trial * 7u);
      for (size_t i = 0; i < garbage.size(); ++i)
      {
        garbage[i] = static_cast<uint8_t>(lcg.next());
      }
      RequireRejected(target, garbage, "random garbage");
    }
  }

  // After every rejection the original response and subsequent audio are
  // still intact.
  RequireIdenticalAudio(target, twin, signal);

  (void)label;
}


// Builds a two-stage blob whose body flags promise a long-tail convolver
// that the impulse response length cannot support.
std::vector<uint8_t> BuildContradictoryTwoStageBlob()
{
  using namespace fftconvolver::snapshot;

  TwoStageSection section;
  section.empty = false;
  section.hasTail0 = true;
  section.hasTail = true; // Contradicts the short IR below
  section.headBlockSize = 4u;
  section.tailBlockSize = 32u;
  section.tailInputFill = 2u;
  section.precalculatedPos = 2u;
  section.ir.assign(40u, 0.1f); // 32 < 40 < 64: no long tail possible
  section.ir.back() = 0.5f;
  section.tailOutput0.assign(32u, 0.0f);
  section.tailPrecalculated0.assign(32u, 0.0f);
  section.tailOutput.assign(32u, 0.0f);
  section.tailPrecalculated.assign(32u, 0.0f);
  section.tailInput.assign(32u, 0.0f);
  section.backgroundProcessingInput.assign(32u, 0.0f);

  Writer payload;
  WriteTwoStageBody(payload, section);

  Writer file;
  WriteFileHeader(file, MAGIC_TWO, CLASS_TWO, payload);
  return file.buffer();
}


// Builds an FFT blob whose tail timing positions contradict each other
// (input fill differs from the precalculated position).
std::vector<uint8_t> BuildContradictoryPositionsBlob()
{
  using namespace fftconvolver::snapshot;

  TwoStageSection section;
  section.empty = false;
  section.hasTail0 = true;
  section.hasTail = false;
  section.headBlockSize = 4u;
  section.tailBlockSize = 32u;
  section.tailInputFill = 5u;
  section.precalculatedPos = 7u; // Must equal tailInputFill
  section.ir.assign(40u, 0.1f);
  section.ir.back() = 0.5f;
  section.tailOutput0.assign(32u, 0.0f);
  section.tailPrecalculated0.assign(32u, 0.0f);
  section.tailInput.assign(32u, 0.0f);

  Writer payload;
  WriteTwoStageBody(payload, section);

  Writer file;
  WriteFileHeader(file, MAGIC_TWO, CLASS_TWO, payload);
  return file.buffer();
}


// ----------------------------------------------------------------------------
// Explicit round-trip behavior of the edge states
// ----------------------------------------------------------------------------

void RunEdgeStateTests()
{
  // 1) Uninitialized object: empty snapshot, round-trips to zero output.
  {
    FFTConvolver fresh;
    std::vector<uint8_t> blobA;
    REQUIRE(fresh.exportState(blobA));
    REQUIRE(!blobA.empty());

    FFTConvolver restored;
    REQUIRE(restored.importState(&blobA[0], blobA.size()));
    float zeros[8] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    float in[8] = { 0.5f, -0.5f, 0.2f, 0.0f, 0.1f, -0.1f, 0.3f, 0.0f };
    restored.process(in, zeros, 8u);
    for (size_t i = 0; i < 8u; ++i)
    {
      REQUIRE(zeros[i] == 0.0f);
    }

    // Empty snapshot also clears a fully working instance.
    const size_t block = 8u;
    std::vector<float> ir(5u, 0.2f);
    FFTConvolver busy;
    REQUIRE(busy.init(block, &ir[0], ir.size()));
    busy.process(in, zeros, 8u);
    REQUIRE(zeros[0] != 0.0f);
    REQUIRE(busy.importState(&blobA[0], blobA.size()));
    std::fill(zeros, zeros + 8u, 1.0f);
    busy.process(in, zeros, 8u);
    for (size_t i = 0; i < 8u; ++i)
    {
      REQUIRE(zeros[i] == 0.0f);
    }
  }

  // 2) Empty / all-zero IR behaves exactly like the uninitialized state.
  {
    std::vector<float> zeroIr(12u, 0.0f);
    FFTConvolver conv;
    REQUIRE(conv.init(8u, &zeroIr[0], zeroIr.size()));
    std::vector<uint8_t> blobEmptyIr;
    REQUIRE(conv.exportState(blobEmptyIr));

    FFTConvolver plain;
    std::vector<uint8_t> blobFresh;
    REQUIRE(plain.exportState(blobFresh));
    REQUIRE(blobEmptyIr == blobFresh);

    FFTConvolver restored;
    REQUIRE(restored.importState(&blobEmptyIr[0], blobEmptyIr.size()));
    float in[4] = { 0.9f, -0.2f, 0.1f, 0.0f };
    float out[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    restored.process(in, out, 4u);
    for (size_t i = 0; i < 4u; ++i)
    {
      REQUIRE(out[i] == 0.0f);
    }
  }

  // 3) Trailing zero segment trimmed by init(): the snapshot stores the
  // trimmed IR and is identical to the one built from the trimmed IR.
  {
    std::vector<float> irCore = BuildImpulseResponse(23u);
    std::vector<float> irPadded = irCore;
    irPadded.resize(irCore.size() + 9u, 0.0f);

    FFTConvolver a;
    FFTConvolver b;
    REQUIRE(a.init(8u, &irPadded[0], irPadded.size()));
    REQUIRE(b.init(8u, &irCore[0], irCore.size()));

    std::vector<uint8_t> blobA;
    std::vector<uint8_t> blobB;
    REQUIRE(a.exportState(blobA));
    REQUIRE(b.exportState(blobB));
    REQUIRE(blobA == blobB);

    // Audio still matches the convolution with the padded IR (trailing
    // zeros do not change the mathematical result anyway).
    std::vector<float> input = BuildContinuousSignal(70u);
    std::vector<float> x = input;
    x.resize(input.size() + irPadded.size() + 16u, 0.0f);
    const std::vector<double> ref = ReferenceConvolve(x, irPadded);

    FFTConvolver restored;
    REQUIRE(restored.importState(&blobA[0], blobA.size()));
    std::vector<float> out(x.size(), 0.0f);
    restored.process(&x[0], &out[0], x.size());
    for (size_t i = 0; i < out.size(); ++i)
    {
      REQUIRE(SamplesClose(out[i], ref[i]));
    }
  }

  // 4) Freshly reset state is the empty state.
  {
    std::vector<float> ir = BuildImpulseResponse(17u);
    FFTConvolver conv;
    REQUIRE(conv.init(4u, &ir[0], ir.size()));
    float in[16] = { 0.3f, -0.3f, 0.2f, 0.1f, 0.0f, -0.2f, 0.4f, 0.1f,
                     0.1f, -0.1f, 0.2f, 0.0f, 0.3f, -0.4f, 0.2f, 0.1f };
    float out[16];
    conv.process(in, out, 16u);
    conv.reset();

    std::vector<uint8_t> blobReset;
    REQUIRE(conv.exportState(blobReset));
    FFTConvolver plain;
    std::vector<uint8_t> blobFresh;
    REQUIRE(plain.exportState(blobFresh));
    REQUIRE(blobReset == blobFresh);

    FFTConvolver restored;
    REQUIRE(restored.importState(&blobReset[0], blobReset.size()));
    std::fill(out, out + 16u, 1.0f);
    restored.process(in, out, 16u);
    for (size_t i = 0; i < 16u; ++i)
    {
      REQUIRE(out[i] == 0.0f);
    }
  }

  // 5) Zero-length processing calls before and after a round trip.
  {
    std::vector<float> ir = BuildImpulseResponse(13u);
    FFTConvolver conv;
    REQUIRE(conv.init(4u, &ir[0], ir.size()));
    float dummyIn[2] = { 0.5f, -0.5f };
    float dummyOut[2] = { 0.0f, 0.0f };
    AllocGuard guard;
    conv.process(dummyIn, dummyOut, 0u);
    conv.process(dummyIn, dummyOut, 0u);
    REQUIRE(guard.unchanged());

    std::vector<uint8_t> blob;
    REQUIRE(conv.exportState(blob));
    FFTConvolver restored;
    REQUIRE(restored.importState(&blob[0], blob.size()));
    AllocGuard guard2;
    restored.process(dummyIn, dummyOut, 0u);
    REQUIRE(guard2.unchanged());
  }

  // 6) Two-stage empty state and cross-class rejection.
  {
    TwoStageFFTConvolver twoEmpty;
    std::vector<uint8_t> blobTwo;
    REQUIRE(twoEmpty.exportState(blobTwo));
    TwoStageFFTConvolver twoRestored;
    REQUIRE(twoRestored.importState(&blobTwo[0], blobTwo.size()));

    FFTConvolver fft;
    REQUIRE(!fft.importState(&blobTwo[0], blobTwo.size())); // Wrong class

    std::vector<float> ir = BuildImpulseResponse(85u);
    FFTConvolver fftReady;
    REQUIRE(fftReady.init(8u, &ir[0], ir.size()));
    std::vector<uint8_t> blobFft;
    REQUIRE(fftReady.exportState(blobFft));
    REQUIRE(!twoRestored.importState(&blobFft[0], blobFft.size())); // Wrong class
  }
}


// ----------------------------------------------------------------------------
// Allocation-free processing after recovery, across many call sizes.
// ----------------------------------------------------------------------------

void RunProcessAllocationTest()
{
  const std::vector<float> ir = BuildImpulseResponse(85u);
  const std::vector<float> input = BuildContinuousSignal(220u);
  const size_t totalFeed = input.size() + ir.size() + 80u;
  std::vector<float> x(totalFeed, 0.0f);
  for (size_t i = 0; i < input.size(); ++i)
  {
    x[i] = input[i];
  }

  // FFTConvolver.
  {
    FFTConvolver conv;
    REQUIRE(conv.init(8u, &ir[0], ir.size()));
    Feeder before(0x4242u, 11u);
    std::vector<float> out(totalFeed, 0.0f);
    FeedRange(conv, &x[0], &out[0], 0u, 73u, before);

    std::vector<uint8_t> blob;
    REQUIRE(conv.exportState(blob));
    FFTConvolver restored;
    REQUIRE(restored.importState(&blob[0], blob.size()));

    size_t pos = 73u;
    const size_t lengths[] = { 1u, 0u, 3u, 2u, 7u, 0u, 1u, 13u, 5u, 8u, 0u, 4u };
    for (size_t t = 0; pos < totalFeed; ++t)
    {
      size_t len = lengths[t % (sizeof(lengths) / sizeof(lengths[0]))];
      len = std::min(len, totalFeed - pos);
      AllocGuard guard;
      restored.process(&x[pos], &out[pos], len);
      REQUIRE(guard.unchanged());
      pos += len;
    }
  }

  // TwoStageFFTConvolver, including blocks straddling tail swaps.
  {
    TwoStageFFTConvolver conv;
    REQUIRE(conv.init(4u, 32u, &ir[0], ir.size()));
    Feeder before(0x5353u, 9u);
    std::vector<float> out(totalFeed, 0.0f);
    FeedRange(conv, &x[0], &out[0], 0u, 70u, before);

    std::vector<uint8_t> blob;
    REQUIRE(conv.exportState(blob));
    TwoStageFFTConvolver restored;
    REQUIRE(restored.importState(&blob[0], blob.size()));

    size_t pos = 70u;
    const size_t lengths[] = { 1u, 1u, 0u, 4u, 2u, 32u, 0u, 31u, 33u, 5u, 8u, 16u };
    for (size_t t = 0; pos < totalFeed; ++t)
    {
      size_t len = lengths[t % (sizeof(lengths) / sizeof(lengths[0]))];
      len = std::min(len, totalFeed - pos);
      AllocGuard guard;
      restored.process(&x[pos], &out[pos], len);
      REQUIRE(guard.unchanged());
      pos += len;
    }
  }
}


} // End of anonymous namespace


// Global heap operators: kept outside the anonymous namespace so they
// replace the library-wide defaults.
void* operator new(std::size_t n)
{
  ++gNewCount;
  void* p = std::malloc(n);
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
    ++gDeleteCount;
  }
  std::free(p);
}

void* operator new[](std::size_t n)
{
  ++gNewCount;
  void* p = std::malloc(n);
  if (!p)
  {
    throw std::bad_alloc();
  }
  return p;
}

void operator delete[](void* p) noexcept
{
  if (p)
  {
    ++gDeleteCount;
  }
  std::free(p);
}


int main()
{
  // ------------------------------------------------------------------
  // Real use cases: continuous signal and sparse pulses, both convolver
  // classes, with a short head and a tail crossing both stage boundaries.
  // ------------------------------------------------------------------
  const size_t headBlock = 4u;
  const size_t tailBlock = 32u;

  const std::vector<float> longIr = BuildImpulseResponse(85u); // > 2 * tailBlock
  const std::vector<float> continuous = BuildContinuousSignal(190u);
  const std::vector<float> pulses = BuildSparsePulses(190u);

  // FFTConvolver, two block sizes, both signal families.
  RunSavePointMatrix<FFTConvolver>("fft/continuous", continuous, longIr, 4u, 4u);
  RunSavePointMatrix<FFTConvolver>("fft/pulses", pulses, longIr, 8u, 8u);

  // TwoStageFFTConvolver with the tail crossing both stage boundaries.
  RunSavePointMatrix<TwoStageFFTConvolver>("twostage/continuous",
                                           continuous, longIr,
                                           headBlock, tailBlock);
  RunSavePointMatrix<TwoStageFFTConvolver>("twostage/pulses",
                                           pulses, longIr,
                                           2u, 16u);

  // Two-stage configurations where only the first tail stage, or only the
  // head, exists.
  {
    const std::vector<float> midIr = BuildImpulseResponse(47u); // tailBlock < len < 2*tailBlock
    RunSavePointMatrix<TwoStageFFTConvolver>("twostage/head+tail0",
                                             continuous, midIr, 4u, 32u);
    const std::vector<float> shortIr = BuildImpulseResponse(25u); // len < tailBlock
    RunSavePointMatrix<TwoStageFFTConvolver>("twostage/head-only",
                                             pulses, shortIr, 4u, 32u);
  }

  // Corruption batteries.
  {
    FFTConvolver conv;
    REQUIRE(conv.init(8u, &longIr[0], longIr.size()));
    std::vector<float> warm(60u);
    std::vector<float> warmOut(60u, 0.0f);
    for (size_t i = 0; i < warm.size(); ++i)
    {
      warm[i] = static_cast<float>(static_cast<int>(i * 17u % 13u) - 6u) * 0.05f;
    }
    conv.process(&warm[0], &warmOut[0], warm.size());
    std::vector<uint8_t> blob;
    REQUIRE(conv.exportState(blob));
    RunCorruptionBattery<FFTConvolver>("fft", blob, longIr, 8u, 8u, continuous);
  }
  {
    TwoStageFFTConvolver conv;
    REQUIRE(conv.init(4u, 32u, &longIr[0], longIr.size()));
    std::vector<float> warm(96u);
    std::vector<float> warmOut(96u, 0.0f);
    for (size_t i = 0; i < warm.size(); ++i)
    {
      warm[i] = static_cast<float>(static_cast<int>(i * 11u % 9u) - 4u) * 0.07f;
    }
    conv.process(&warm[0], &warmOut[0], warm.size());
    std::vector<uint8_t> blob;
    REQUIRE(conv.exportState(blob));
    RunCorruptionBattery<TwoStageFFTConvolver>("twostage", blob,
                                               longIr, 4u, 32u, continuous);

    // Two-stage-specific contradictory states.
    TwoStageFFTConvolver target;
    REQUIRE(target.init(4u, 32u, &longIr[0], longIr.size()));
    target.process(&warm[0], &warmOut[0], warm.size());
    RequireRejected(target, BuildContradictoryTwoStageBlob(),
                    "tail flags contradict IR length");
    RequireRejected(target, BuildContradictoryPositionsBlob(),
                    "tail positions contradict each other");

    TwoStageFFTConvolver twin;
    REQUIRE(twin.init(4u, 32u, &longIr[0], longIr.size()));
    std::vector<float> twinOut(96u, 0.0f);
    twin.process(&warm[0], &twinOut[0], warm.size());
    RequireIdenticalAudio(target, twin, continuous);
  }

  RunEdgeStateTests();
  RunProcessAllocationTest();

  if (gFailures == 0)
  {
    std::printf("SNAPSHOT_AUDIO_OK checks=%d\n", gChecks);
    return 0;
  }

  std::printf("SNAPSHOT_FAILED failures=%d checks=%d\n", gFailures, gChecks);
  return 1;
}
