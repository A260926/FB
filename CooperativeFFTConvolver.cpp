// ==================================================================================
// CooperativeFFTConvolver implementation
//
// Tail pipeline overview (see docs/cooperative_scheduling.md for the timing
// and work-unit accounting):
//
// - The first 2T impulse response coefficients are handled by the existing
//   FFTConvolver at block size B (the "head path"). It is never part of the
//   cooperatively scheduled work budget.
//
// - The remaining coefficients g[m] = ir[2T + m] are split into P uniform
//   partitions of T samples. Every captured T-sample input batch spawns one
//   tail job. The tail math is the uniform-partition overlap-add algorithm
//   (the same one FFTConvolver uses), driven by a resumable engine:
//
//     1. Build the 2T vector [batch, zeros] and run the resumable forward
//        complex FFT directly into a rotating ring of input spectra.
//     2. Sum the P partition products X[(j-p) mod P] * G[p], one complex
//        bin at a time, into the job's accumulator spectrum.
//     3. Run the resumable inverse FFT (conjugate FFT trick), then overlap-
//        add the first T samples with the carried overlap and carry the
//        second T samples into the shared overlap buffer.
//
//   Job j (input [jT, (j+1)T)) emits the T tail samples at absolute positions
//   [(j+2)T, (j+3)T): the first audible contribution is exactly 2T samples
//   after the batch start, leaving Q = T/B audio quanta between capture and
//   deadline. Every phase is a plain indexed loop, so a job can be suspended
//   after any butterfly / bin / slot and resumed in a later quantum.
//
// - Quantums are B-sample intervals of the cumulative input stream. At each
//   completed quantum the oldest active job receives at most the configured
//   amount of real work; every executed loop batch is reported through
//   FFTCONVOLVER_TAIL_WORK(). Three job slots are kept (job j reuses slot
//   j mod 3); a slot is recycled only after the previous owner's whole
//   output interval has been mixed, so spectra, ready blocks and history
//   never overwrite each other.
// ==================================================================================

#include "CooperativeFFTConvolver.h"
#include "FFTConvolver.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// In ordinary production builds the tracing header is not force-included:
// turn the reporting hook into a no-op there while still referencing its
// arguments so the phase drivers stay warning-free.
#ifndef FFTCONVOLVER_TAIL_WORK
  #define FFTCONVOLVER_TAIL_WORK(quantum, kind, units) \
    ((void)(quantum), (void)(kind), (void)(units))
#endif

namespace fftconvolver
{

namespace
{

const size_t MAX_IR_LEN = 65536;
const size_t JOB_SLOTS = 3;

bool IsPowerOfTwo(size_t x)
{
  return x >= 2 && (x & (x - 1)) == 0;
}

size_t Log2(size_t x)
{
  size_t l = 0;
  while ((size_t(1) << l) < x)
  {
    ++l;
  }
  return l;
}

// Tail job phases. Each phase only touches indexed loops whose progress is
// fully described by the job's cursor members, so suspension is lossless.
enum JobPhase
{
  PHASE_IDLE = 0,
  PHASE_LOAD,          // Capture -> ring slot real part, zero the rest
  PHASE_BITREV_FWD,    // Bit-reversal permutation before the forward FFT
  PHASE_FORWARD,       // Resumable forward complex FFT (into the ring)
  PHASE_MAC_FIRST,     // Write product X[j] * G[0] into the accumulator
  PHASE_MAC_REST,      // Accumulate X[j-p] * G[p] for p = 1 .. P-1
  PHASE_INV_CONJ,      // Conjugate the accumulator before the inverse
  PHASE_BITREV_INV,    // Bit-reversal permutation before the inverse FFT
  PHASE_INVERSE,       // Resumable inverse complex FFT (conjugate FFT)
  PHASE_INV_SCALE,     // Conjugate + 1/N scale after the inverse
  PHASE_OVERLAP        // Overlap-add the result and refresh the overlap
};

struct Job
{
  SplitComplex accum;     // Length N: MAC accumulator / inverse FFT data
  SampleBuffer output;    // Length T: finished tail output block
  int phase;
  size_t stage;           // FFT stage cursor
  size_t pair;            // Flat butterfly cursor inside the stage
  size_t pos;             // Linear cursor for move-style phases
  size_t part;            // Partition cursor (MAC)
  size_t bin;             // Frequency bin cursor
  size_t ringSlot;        // Ring slot holding this job's input spectrum
  size_t snapshotSlot;    // Snapshot buffer holding the captured T samples
  uint64_t outputIndex;   // Absolute stream position of the T result samples
  bool finished;          // Result block is complete and published

  Job() :
    accum(),
    output(),
    phase(PHASE_IDLE),
    stage(0),
    pair(0),
    pos(0),
    part(0),
    bin(0),
    ringSlot(0),
    snapshotSlot(0),
    outputIndex(0),
    finished(false)
  {
  }
};

} // End of anonymous namespace


struct CooperativeFFTConvolver::Impl
{
  bool initialized;
  bool silent;            // Valid init with an empty impulse response
  size_t B;
  size_t T;
  size_t N;               // FFT length = 2T
  size_t P;               // Number of tail partitions
  size_t Q;               // T/B quanta per tail block
  size_t logN;
  size_t budget;          // Work units permitted per quantum
  float invN;

  std::vector<float> irCopy;             // Own copy of the impulse response
  FFTConvolver head;                     // Short-block path for ir[0 .. 2T)

  std::vector<SplitComplex*> irSpectra;  // P precomputed partition spectra
  std::vector<SplitComplex*> inputRing;  // P rotating input spectra
  SampleBuffer overlap;                  // T carried overlap samples
  SampleBuffer twCos;                    // N precomputed twiddle cosines
  SampleBuffer twSin;                    // N precomputed twiddle sines

  Job jobs[JOB_SLOTS];
  uint64_t jobCount;                     // Number of launched jobs

  // Per-block input snapshots, rotated by block index. A snapshot is loaded
  // by its job during the block in which it is captured (the per-block work
  // budget finishes a whole job within that block), so the slot is free to
  // be rewritten three blocks later even inside one very long process call.
  SampleBuffer snapshots[JOB_SLOTS];

  uint64_t totalSamples;                 // Absolute stream position

  Impl() :
    initialized(false),
    silent(false),
    B(0),
    T(0),
    N(0),
    P(0),
    Q(0),
    logN(0),
    budget(0),
    invN(0.0f),
    irCopy(),
    head(),
    irSpectra(),
    inputRing(),
    overlap(),
    twCos(),
    twSin(),
    jobs(),
    jobCount(0),
    snapshots(),
    totalSamples(0)
  {
  }

  ~Impl()
  {
    releaseTail();
  }

  void resetJob(Job& job)
  {
    job.accum.clear();
    job.output.clear();
    job.phase = PHASE_IDLE;
    job.stage = 0;
    job.pair = 0;
    job.pos = 0;
    job.part = 0;
    job.bin = 0;
    job.ringSlot = 0;
    job.snapshotSlot = 0;
    job.outputIndex = 0;
    job.finished = false;
  }

  void releaseTail()
  {
    for (size_t i=0; i<irSpectra.size(); ++i)
    {
      delete irSpectra[i];
    }
    irSpectra.clear();
    for (size_t i=0; i<inputRing.size(); ++i)
    {
      delete inputRing[i];
    }
    inputRing.clear();
    overlap.clear();
    twCos.clear();
    twSin.clear();
    for (size_t i=0; i<JOB_SLOTS; ++i)
    {
      resetJob(jobs[i]);
    }
    jobCount = 0;
    for (size_t i=0; i<JOB_SLOTS; ++i)
    {
      snapshots[i].clear();
    }
  }

  void resetAll()
  {
    head.reset();
    releaseTail();
    irCopy.clear();
    initialized = false;
    silent = false;
    B = T = N = P = Q = logN = budget = 0;
    invN = 0.0f;
    totalSamples = 0;
  }

  // ----- One-shot complex FFT used at initialization time -----------------
  // Same math as the resumable engine, but run to completion. Init work is
  // not reported through FFTCONVOLVER_TAIL_WORK (no quantum exists yet).

  void bitReverseOnce(float* re, float* im)
  {
    size_t j = 0;
    for (size_t i=1; i<N; ++i)
    {
      size_t bit = N >> 1;
      while (j & bit)
      {
        j ^= bit;
        bit >>= 1;
      }
      j ^= bit;
      if (j > i)
      {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
  }

  void fullFft(float* re, float* im)
  {
    bitReverseOnce(re, im);
    const float* cosT = twCos.data();
    const float* sinT = twSin.data();
    for (size_t s=0; s<logN; ++s)
    {
      const size_t half = size_t(1) << s;
      const size_t full = half << 1;
      const size_t twStep = N / full;
      for (size_t base=0; base<N; base+=full)
      {
        for (size_t p=0; p<half; ++p)
        {
          const size_t top = base + p;
          const size_t bot = top + half;
          const size_t tw = (p * twStep) & (N - 1);
          const float wr = cosT[tw];
          const float wi = sinT[tw];
          const float tr = wr * re[bot] - wi * im[bot];
          const float ti = wr * im[bot] + wi * re[bot];
          re[bot] = re[top] - tr;
          im[bot] = im[top] - ti;
          re[top] += tr;
          im[top] += ti;
        }
      }
    }
  }

  // ----- Resumable radix-2 DIT complex FFT ---------------------------------
  //
  // One radix-2 butterfly counts as one work unit. The twiddle factors are
  // precomputed at init time; no trigonometry happens here.

  size_t stepFft(Job& job, SplitComplex& target, size_t cap, uint64_t quantum)
  {
    size_t done = 0;
    float* re = target.re();
    float* im = target.im();
    const float* cosT = twCos.data();
    const float* sinT = twSin.data();

    while (done < cap && job.stage < logN)
    {
      const size_t half = size_t(1) << job.stage;
      const size_t full = half << 1;
      const size_t twStep = N / full;
      // Flat cursor over all N/2 butterflies of this stage.
      const size_t total = N / 2;
      const size_t batch = std::min(cap - done, total - job.pair);

      for (size_t k=0; k<batch; ++k)
      {
        const size_t g = job.pair + k;
        const size_t base = (g / half) * full;
        const size_t p = g % half;
        const size_t top = base + p;
        const size_t bot = top + half;
        const size_t tw = (p * twStep) & (N - 1);
        const float wr = cosT[tw];
        const float wi = sinT[tw];
        const float tr = wr * re[bot] - wi * im[bot];
        const float ti = wr * im[bot] + wi * re[bot];
        re[bot] = re[top] - tr;
        im[bot] = im[top] - ti;
        re[top] += tr;
        im[top] += ti;
      }
      FFTCONVOLVER_TAIL_WORK(quantum, "butterfly", batch);

      done += batch;
      job.pair += batch;
      if (job.pair == total)
      {
        job.pair = 0;
        ++job.stage;
      }
    }
    return done;
  }

  // ----- Resumable bit-reversal permutation --------------------------------
  //
  // Only the actually swapped complex slots are counted (4 slots per pair).

  size_t stepBitRev(Job& job, SplitComplex& target, int nextPhase,
                    size_t cap, uint64_t quantum)
  {
    size_t done = 0;
    float* re = target.re();
    float* im = target.im();

    // A swapped pair costs 4 slots and runs as one unit, so the loop always
    // makes progress whenever at least 4 units remain.
    while (job.pos < N)
    {
      const size_t i = job.pos;
      size_t j = 0;
      {
        size_t x = i;
        for (size_t b=0; b<logN; ++b)
        {
          j = (j << 1) | (x & 1u);
          x >>= 1;
        }
      }
      if (j <= i)
      {
        // No swap at this index: pure index bookkeeping, not a work unit.
        ++job.pos;
        continue;
      }
      if (done + 4 > cap)
      {
        break;
      }
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
      done += 4;
      ++job.pos;
    }
    if (done > 0)
    {
      FFTCONVOLVER_TAIL_WORK(quantum, "move", done);
    }
    if (job.pos == N)
    {
      job.pos = 0;
      job.stage = 0;
      job.pair = 0;
      job.phase = nextPhase;
    }
    return done;
  }

  // ----- Linear phase drivers: each returns the real units executed --------

  size_t stepLoad(Job& job, size_t cap, uint64_t quantum)
  {
    // Cursor 0 .. 2N-1: real part first (T captured samples, then zeros),
    // then the N imaginary slots. The target is the rotating ring slot,
    // whose stale contents are fully overwritten.
    SplitComplex& target = *inputRing[job.ringSlot];
    const Sample* snap = snapshots[job.snapshotSlot].data();
    const size_t total = 2 * N;
    const size_t batch = std::min(cap, total - job.pos);
    float* re = target.re();
    float* im = target.im();
    for (size_t k=0; k<batch; ++k)
    {
      const size_t idx = job.pos + k;
      if (idx < T)
      {
        re[idx] = snap[idx];
      }
      else if (idx < N)
      {
        re[idx] = 0.0f;
      }
      else
      {
        im[idx - N] = 0.0f;
      }
    }
    FFTCONVOLVER_TAIL_WORK(quantum, "move", batch);
    job.pos += batch;
    if (job.pos == total)
    {
      job.pos = 0;
      job.phase = PHASE_BITREV_FWD;
    }
    return batch;
  }

  size_t stepMacFirst(Job& job, size_t cap, uint64_t quantum)
  {
    // Partition 0 is written (one complex product per bin) instead of
    // clearing an accumulator and adding into it, saving one full pass. It
    // is still a per-bin complex multiplication, so it is reported as mac.
    const SplitComplex& src = *inputRing[job.ringSlot];
    const SplitComplex& irSpec = *irSpectra[0];
    const size_t batch = std::min(cap, N - job.bin);
    float* re = job.accum.re();
    float* im = job.accum.im();
    for (size_t k=0; k<batch; ++k)
    {
      const size_t i = job.bin + k;
      const float xr = src.re()[i];
      const float xi = src.im()[i];
      re[i] = xr * irSpec.re()[i] - xi * irSpec.im()[i];
      im[i] = xr * irSpec.im()[i] + xi * irSpec.re()[i];
    }
    FFTCONVOLVER_TAIL_WORK(quantum, "mac", batch);
    job.bin += batch;
    if (job.bin == N)
    {
      job.bin = 0;
      job.part = 1;
      job.phase = (P > 1) ? PHASE_MAC_REST : PHASE_INV_CONJ;
    }
    return batch;
  }

  size_t stepMacRest(Job& job, size_t cap, uint64_t quantum)
  {
    size_t done = 0;
    float* re = job.accum.re();
    float* im = job.accum.im();
    // Job j emits at (j+2)T, so its input block index is outputIndex/T - 2.
    const uint64_t blockIndex = job.outputIndex / static_cast<uint64_t>(T) - 2;
    while (job.part < P)
    {
      // A partition older than the first captured input block refers to a
      // pre-start (silent) block whose spectrum is zero, so it adds nothing.
      if (static_cast<uint64_t>(job.part) > blockIndex)
      {
        ++job.part;
        continue;
      }
      if (done >= cap)
      {
        break;
      }
      // Ring slot of input block (j - part).
      const size_t blockP = static_cast<size_t>(blockIndex - job.part);
      const size_t slot = blockP % P;
      const SplitComplex& src = *inputRing[slot];
      const SplitComplex& irSpec = *irSpectra[job.part];
      const float* srcRe = src.re();
      const float* srcIm = src.im();
      const float* irRe = irSpec.re();
      const float* irIm = irSpec.im();
      const size_t batch = std::min(cap - done, N - job.bin);
      for (size_t k=0; k<batch; ++k)
      {
        const size_t i = job.bin + k;
        const float xr = srcRe[i];
        const float xi = srcIm[i];
        re[i] += xr * irRe[i] - xi * irIm[i];
        im[i] += xr * irIm[i] + xi * irRe[i];
      }
      FFTCONVOLVER_TAIL_WORK(quantum, "mac", batch);

      done += batch;
      job.bin += batch;
      if (job.bin == N)
      {
        job.bin = 0;
        ++job.part;
      }
    }
    if (job.part == P)
    {
      job.pos = 0;
      job.phase = PHASE_INV_CONJ;
    }
    return done;
  }

  size_t stepInvConj(Job& job, size_t cap, uint64_t quantum)
  {
    const size_t batch = std::min(cap, N - job.pos);
    float* im = job.accum.im();
    for (size_t k=0; k<batch; ++k)
    {
      const size_t idx = job.pos + k;
      im[idx] = -im[idx];
    }
    FFTCONVOLVER_TAIL_WORK(quantum, "move", batch);
    job.pos += batch;
    if (job.pos == N)
    {
      job.pos = 0;
      job.phase = PHASE_BITREV_INV;
    }
    return batch;
  }

  size_t stepInvScale(Job& job, size_t cap, uint64_t quantum)
  {
    // Cursor 0 .. 2N-1: scale the real part, conjugate+scale the imag part.
    const size_t total = 2 * N;
    const size_t batch = std::min(cap, total - job.pos);
    float* re = job.accum.re();
    float* im = job.accum.im();
    for (size_t k=0; k<batch; ++k)
    {
      const size_t idx = job.pos + k;
      if (idx < N)
      {
        re[idx] *= invN;
      }
      else
      {
        const size_t j = idx - N;
        im[j] = -im[j] * invN;
      }
    }
    FFTCONVOLVER_TAIL_WORK(quantum, "move", batch);
    job.pos += batch;
    if (job.pos == total)
    {
      job.pos = 0;
      job.phase = PHASE_OVERLAP;
    }
    return batch;
  }

  size_t stepOverlap(Job& job, size_t cap, uint64_t quantum)
  {
    // Cursor 0 .. 2T-1: first T slots overlap-add with the carried overlap,
    // the next T slots become the new carried overlap.
    const size_t total = 2 * T;
    const size_t batch = std::min(cap, total - job.pos);
    const float* re = job.accum.re();
    float* out = job.output.data();
    float* ov = overlap.data();
    for (size_t k=0; k<batch; ++k)
    {
      const size_t idx = job.pos + k;
      if (idx < T)
      {
        out[idx] = re[idx] + ov[idx];
      }
      else
      {
        ov[idx - T] = re[idx];
      }
    }
    FFTCONVOLVER_TAIL_WORK(quantum, "move", batch);
    job.pos += batch;
    if (job.pos == total)
    {
      job.pos = 0;
      job.phase = PHASE_IDLE;
      job.finished = true;
    }
    return batch;
  }

  size_t stepJob(Job& job, size_t cap, uint64_t quantum)
  {
    switch (job.phase)
    {
      case PHASE_LOAD:
        return stepLoad(job, cap, quantum);
      case PHASE_BITREV_FWD:
        return stepBitRev(job, *inputRing[job.ringSlot], PHASE_FORWARD,
                          cap, quantum);
      case PHASE_FORWARD:
      {
        const size_t done = stepFft(job, *inputRing[job.ringSlot],
                                    cap, quantum);
        if (job.phase == PHASE_FORWARD && job.stage == logN)
        {
          job.stage = 0;
          job.bin = 0;
          job.part = 0;
          job.phase = PHASE_MAC_FIRST;
        }
        return done;
      }
      case PHASE_MAC_FIRST:
        return stepMacFirst(job, cap, quantum);
      case PHASE_MAC_REST:
        return stepMacRest(job, cap, quantum);
      case PHASE_INV_CONJ:
        return stepInvConj(job, cap, quantum);
      case PHASE_BITREV_INV:
        return stepBitRev(job, job.accum, PHASE_INVERSE, cap, quantum);
      case PHASE_INVERSE:
      {
        const size_t done = stepFft(job, job.accum, cap, quantum);
        if (job.phase == PHASE_INVERSE && job.stage == logN)
        {
          job.stage = 0;
          job.pos = 0;
          job.phase = PHASE_INV_SCALE;
        }
        return done;
      }
      case PHASE_INV_SCALE:
        return stepInvScale(job, cap, quantum);
      case PHASE_OVERLAP:
        return stepOverlap(job, cap, quantum);
      default:
        assert(false);
        return 0;
    }
  }

  Job* pickOldestActive()
  {
    Job* picked = 0;
    for (size_t i=0; i<JOB_SLOTS; ++i)
    {
      if (jobs[i].phase != PHASE_IDLE)
      {
        if (picked == 0 || jobs[i].outputIndex < picked->outputIndex)
        {
          picked = &jobs[i];
        }
      }
    }
    return picked;
  }

  void runQuantum(uint64_t quantum)
  {
    size_t used = 0;
    while (used < budget)
    {
      Job* job = pickOldestActive();
      if (job == 0)
      {
        break;
      }
      const size_t executed = stepJob(*job, budget - used, quantum);
      if (executed == 0)
      {
        // The remaining budget cannot fit the next atomic loop unit.
        break;
      }
      used += executed;
    }
  }

  void enqueueJob(size_t snapshotSlot, uint64_t outputIndex)
  {
    const size_t slot = static_cast<size_t>(jobCount % JOB_SLOTS);
    Job& job = jobs[slot];

    // The slot previously held job (jobCount - JOB_SLOTS), whose whole
    // output interval has already been mixed before this enqueue.
    assert(job.phase == PHASE_IDLE);

    job.phase = PHASE_LOAD;
    job.stage = 0;
    job.pair = 0;
    job.pos = 0;
    job.part = 0;
    job.bin = 0;
    job.ringSlot = static_cast<size_t>(jobCount % P);
    job.snapshotSlot = snapshotSlot;
    job.outputIndex = outputIndex;
    job.finished = false;
    ++jobCount;
  }

  // Mix the finished tail block covering absolute sample n, if any. Job
  // output intervals are disjoint T-sample chunks in launch order.
  void mixTail(uint64_t n, Sample& dst)
  {
    for (size_t i=0; i<JOB_SLOTS; ++i)
    {
      const Job& job = jobs[i];
      if (job.finished &&
          job.outputIndex <= n &&
          n < job.outputIndex + T)
      {
        dst += job.output.data()[static_cast<size_t>(n - job.outputIndex)];
      }
    }
  }
};


// ============================================================================


CooperativeFFTConvolver::CooperativeFFTConvolver() :
  _impl(new Impl())
{
}


CooperativeFFTConvolver::~CooperativeFFTConvolver()
{
  delete _impl;
}


void CooperativeFFTConvolver::reset()
{
  _impl->resetAll();
}


bool CooperativeFFTConvolver::init(size_t headBlockSize,
                                   size_t tailBlockSize,
                                   const Sample* ir,
                                   size_t irLen)
{
  Impl* d = _impl;
  d->resetAll();

  // Geometry validation (no rounding: values are used as given).
  if (!IsPowerOfTwo(headBlockSize) || headBlockSize < 8 || headBlockSize > 256)
  {
    return false;
  }
  if (!IsPowerOfTwo(tailBlockSize) || tailBlockSize < 64 || tailBlockSize > 4096)
  {
    return false;
  }
  if (tailBlockSize < 8 * headBlockSize)
  {
    return false;
  }
  if (irLen > MAX_IR_LEN)
  {
    return false;
  }
  if (irLen > 0 && ir == 0)
  {
    return false;
  }
  for (size_t i=0; i<irLen; ++i)
  {
    if (!std::isfinite(ir[i]))
    {
      return false;
    }
  }

  d->initialized = true;
  d->B = headBlockSize;
  d->T = tailBlockSize;
  d->N = 2 * tailBlockSize;
  d->Q = tailBlockSize / headBlockSize;
  d->logN = Log2(d->N);
  d->invN = 1.0f / static_cast<float>(d->N);

  // Keep an owned copy of the full response.
  d->irCopy.assign(ir, ir + irLen);

  if (irLen == 0)
  {
    // Legal configuration: a zero-length response means silence.
    d->silent = true;
    return true;
  }
  d->silent = false;

  // Head path: the first 2T coefficients, partitioned at B.
  const size_t headIrLen = std::min(irLen, 2 * d->T);
  if (!d->head.init(d->B, ir, headIrLen))
  {
    d->resetAll();
    return false;
  }

  // Tail partitions: ir[2T .. irLen), each zero-padded to N and transformed.
  if (irLen > 2 * d->T)
  {
    const size_t tailLen = irLen - 2 * d->T;
    d->P = (tailLen + d->T - 1) / d->T;

    // Precomputed twiddles for the radix-2 engine (negative-exponent DFT).
    d->twCos.resize(d->N);
    d->twSin.resize(d->N);
    const double twoPi = 2.0 * 3.14159265358979323846;
    for (size_t i=0; i<d->N; ++i)
    {
      const double angle = twoPi * static_cast<double>(i) / static_cast<double>(d->N);
      d->twCos.data()[i] = static_cast<Sample>(std::cos(angle));
      d->twSin.data()[i] = static_cast<Sample>(-std::sin(angle));
    }

    SampleBuffer timeBuffer(d->N);
    for (size_t p=0; p<d->P; ++p)
    {
      const size_t start = 2 * d->T + p * d->T;
      const size_t remaining = (irLen > start) ? (irLen - start) : 0;
      const size_t copyLen = std::min(remaining, d->T);
      ::memcpy(timeBuffer.data(), ir + start, copyLen * sizeof(Sample));
      ::memset(timeBuffer.data() + copyLen, 0, (d->N - copyLen) * sizeof(Sample));

      SplitComplex* spectrum = new SplitComplex(d->N);
      spectrum->setZero();
      ::memcpy(spectrum->re(), timeBuffer.data(), d->N * sizeof(Sample));
      d->fullFft(spectrum->re(), spectrum->im());
      d->irSpectra.push_back(spectrum);
    }

    // Rotating input spectrum ring (zero-initialized: startup slots are
    // silent input blocks).
    for (size_t p=0; p<d->P; ++p)
    {
      SplitComplex* slot = new SplitComplex(d->N);
      slot->setZero();
      d->inputRing.push_back(slot);
    }
    d->overlap.resize(d->T);

    // Per-quantum budget:
    //   F = (N/2) * log2(N)
    //   budget = ceil((2F + P*N + 12N) / Q) + 16
    const size_t F = (d->N / 2) * d->logN;
    const size_t workTotal = 2 * F + d->P * d->N + 12 * d->N;
    d->budget = (workTotal + d->Q - 1) / d->Q + 16;

    // Fixed job workspaces: three in-flight slots plus three snapshots.
    for (size_t i=0; i<JOB_SLOTS; ++i)
    {
      d->jobs[i].accum.resize(d->N);
      d->jobs[i].output.resize(d->T);
      d->jobs[i].snapshotSlot = 0;
      d->snapshots[i].resize(d->T);
      d->jobs[i].phase = PHASE_IDLE;
      d->jobs[i].finished = false;
    }
    d->jobCount = 0;
  }
  else
  {
    d->P = 0;
  }

  d->totalSamples = 0;
  return true;
}


void CooperativeFFTConvolver::process(const Sample* input, Sample* output, size_t len)
{
  Impl* d = _impl;

  if (len == 0)
  {
    // No sample position advancement, no tail work, nothing to write.
    return;
  }

  if (!d->initialized || d->silent || d->irCopy.empty())
  {
    ::memset(output, 0, len * sizeof(Sample));
    if (d->initialized)
    {
      d->totalSamples += len;
    }
    return;
  }

  // The stream is processed in internal chunks aligned to B-sample quantum
  // boundaries. Everything (capture, enqueue, scheduled work, head
  // processing, tail mixing) is driven by the absolute stream position, so
  // the schedule and the output are identical for any split of the stream
  // into process() calls.
  size_t done = 0;
  while (done < len)
  {
    const uint64_t absStart = d->totalSamples + done;
    const size_t toBoundary = d->B - static_cast<size_t>(absStart % d->B);
    const size_t chunk = std::min(len - done, toBoundary);
    const uint64_t absEnd = absStart + chunk;

    if (d->P > 0)
    {
      // (a) Snapshot every tail input sample before any in-place output can
      //     be overwritten, into the rotating slot of its T-sample block.
      for (size_t i=0; i<chunk; ++i)
      {
        const uint64_t pos = absStart + i;
        const size_t blockIndex = static_cast<size_t>(pos / d->T);
        const size_t inBlock = static_cast<size_t>(pos % d->T);
        d->snapshots[blockIndex % JOB_SLOTS].data()[inBlock] = input[done + i];
      }

      // (b) If a T-sample block ends in this chunk, launch its job now.
      //     Job of block j emits at (j+2)T = block start + 2T.
      if ((absEnd % d->T) == 0)
      {
        const uint64_t blockIndex = absEnd / d->T - 1;
        const size_t snapSlot = static_cast<size_t>(blockIndex % JOB_SLOTS);
        d->enqueueJob(snapSlot, (blockIndex + 2) * d->T);
      }

      // (c) A chunk ending on a B boundary completes exactly one quantum.
      if ((absEnd % d->B) == 0)
      {
        d->runQuantum(absEnd / d->B - 1);
      }
    }

    // (d) Short-block head path for this chunk.
    d->head.process(input + done, output + done, chunk);

    // (e) Mix the tail outputs that have already been prepared.
    if (d->P > 0)
    {
      for (size_t i=0; i<chunk; ++i)
      {
        d->mixTail(absStart + i, output[done + i]);
      }
    }

    done += chunk;
  }

  d->totalSamples += len;
}

} // End of namespace fftconvolver
