// ==================================================================================
// CooperativeFFTConvolver - implementation
//
// Data path
// ---------
// The first 2*T impulse response samples are processed by a zero-latency
// partitioned convolution using block size B and a 2B-point complex FFT (the
// "head path"). Every processing call recomputes the FFT of the partially
// filled input block (unfilled samples are zero), so no future input is read
// and no block latency is added. This is the classic FFTConvolver algorithm.
//
// The remaining IR samples are partitioned into P blocks of length T and
// transformed with N=2T-point FFTs (the "tail path"). Every T captured input
// samples produce one tail job. Job j captures input [jT,(j+1)T); because the
// tail IR begins at offset 2T, its earliest acoustic contribution appears at
// absolute sample jT+2T. The job is created when sample (j+1)T is reached, so
// exactly T samples, i.e. Q=T/B quanta, are available to finish it.
//
// The resumable tail pipeline is:
//   JOB_COPY          : pack the T captured samples into N complex slots
//   JOB_FORWARD       : bit reversal + radix-2 forward FFT (per butterfly)
//   JOB_FORWARD_STORE : store X_j in the P-slot spectrum ring, clear accumulator
//   JOB_MAC           : P*N complex multiply-accumulates with the IR partitions
//   JOB_INVERSE_LOAD  : copy the accumulator into the inverse FFT buffer
//   JOB_INVERSE       : bit reversal + radix-2 inverse FFT (per butterfly)
//   JOB_SCALE         : 1/N scaling of every complex result slot
//   JOB_OVERLAP       : overlap-add of all N real samples into the output ring
//
// Jobs run strictly one at a time. Job j-1's output window opens at jT+T,
// before job j is even captured at (j+1)T, so the per-quantum budget (which
// covers a full job over Q quanta) guarantees every block is ready before its
// window starts. Captured blocks are snapshotted in a small fixed ring so a
// job never reads input that newer calls have overwritten.
//
// All twiddle factors and bit-reversal tables are computed in init(); process()
// never calls trigonometric functions, never allocates or frees heap memory and
// never blocks on a lock.
// ==================================================================================

#include "CooperativeFFTConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>


namespace fftconvolver
{

namespace
{

const double kPi = 3.14159265358979323846;

// Number of captured T-sample blocks kept as snapshots. A job for block j is
// always finished (deadline jT+T) before block j+1 is even captured (at
// (j+1)T), so four slots comfortably outlive any in-flight job.
const size_t kCaptureSlots = 4;


bool isPowerOf2Impl(size_t v)
{
  return v > 0 && (v & (v - 1)) == 0;
}


size_t log2Impl(size_t v)
{
  size_t l = 0;
  while (v > 1) { v >>= 1; ++l; }
  return l;
}


void buildTables(size_t n,
                 bool inverse,
                 std::vector<Sample>& twiddle,
                 std::vector<size_t>& bitrev)
{
  const size_t log2n = log2Impl(n);
  twiddle.assign(n, 0.0f); // n/2 complex twiddles => n interleaved samples
  bitrev.assign(n, 0);
  for (size_t i = 0; i < n; ++i)
  {
    size_t r = 0;
    for (size_t b = 0; b < log2n; ++b)
    {
      r = (r << 1) | ((i >> b) & 1u);
    }
    bitrev[i] = r;
  }
  const double sign = inverse ? 1.0 : -1.0;
  for (size_t k = 0; k < n / 2; ++k)
  {
    const double angle = sign * 2.0 * kPi * double(k) / double(n);
    twiddle[2 * k] = static_cast<Sample>(std::cos(angle));
    twiddle[2 * k + 1] = static_cast<Sample>(std::sin(angle));
  }
}


// In-place radix-2 DIT transform on interleaved complex data.
void fftInPlace(Sample* buf,
                size_t n,
                size_t log2n,
                const std::vector<size_t>& bitrev,
                const std::vector<Sample>& twiddle)
{
  for (size_t i = 0; i < n; ++i)
  {
    const size_t j = bitrev[i];
    if (j > i)
    {
      std::swap(buf[2 * i], buf[2 * j]);
      std::swap(buf[2 * i + 1], buf[2 * j + 1]);
    }
  }
  for (size_t stage = 0; stage < log2n; ++stage)
  {
    const size_t half = size_t(1) << stage;
    const size_t m = half << 1;
    for (size_t k = 0; k < n; k += m)
    {
      for (size_t j = 0; j < half; ++j)
      {
        const size_t tw = j * (n / m);
        const Sample wr = twiddle[2 * tw];
        const Sample wi = twiddle[2 * tw + 1];
        const size_t ia = 2 * (k + j);
        const size_t ib = 2 * (k + j + half);
        const Sample ar = buf[ia];
        const Sample ai = buf[ia + 1];
        const Sample br = buf[ib];
        const Sample bi = buf[ib + 1];
        const Sample tr = wr * br - wi * bi;
        const Sample ti = wr * bi + wi * br;
        buf[ia] = ar + tr;
        buf[ia + 1] = ai + ti;
        buf[ib] = ar - tr;
        buf[ib + 1] = ai - ti;
      }
    }
  }
}

} // anonymous namespace


// ============================================================
// ResumableFFT
// ============================================================

CooperativeFFTConvolver::ResumableFFT::ResumableFFT() :
  _n(0),
  _log2n(0),
  _inverse(false),
  _stage(0),
  _butterfly(0)
{
}


void CooperativeFFTConvolver::ResumableFFT::configure(size_t n, bool inverse)
{
  _n = n;
  _log2n = log2Impl(n);
  _inverse = inverse;
  _stage = 0;
  _butterfly = 0;
  _buf.assign(2 * n, 0.0f);
  _twiddle.assign(n, 0.0f);
  const double sign = inverse ? 1.0 : -1.0;
  for (size_t k = 0; k < n / 2; ++k)
  {
    const double angle = sign * 2.0 * kPi * double(k) / double(n);
    _twiddle[2 * k] = static_cast<Sample>(std::cos(angle));
    _twiddle[2 * k + 1] = static_cast<Sample>(std::sin(angle));
  }
}


void CooperativeFFTConvolver::ResumableFFT::release()
{
  _n = 0;
  _log2n = 0;
  _inverse = false;
  _stage = 0;
  _butterfly = 0;
  std::vector<Sample>().swap(_buf);
  std::vector<Sample>().swap(_twiddle);
}


size_t CooperativeFFTConvolver::ResumableFFT::run(size_t quantum, size_t budget)
{
  // `quantum` only appears in the (possibly compiled-out) trace macro.
  (void)quantum;
  size_t used = 0;

  // Phase 0: bit-reversal permutation. Index work is free; only complex slots
  // actually moved count (two slots per performed swap). A swap is only started
  // when both slots fit into the remaining budget.
  size_t moved = 0;
  while (_stage == 0 && _butterfly < _n)
  {
    const size_t i = _butterfly;
    size_t j = 0;
    for (size_t b = 0; b < _log2n; ++b)
    {
      j = (j << 1) | ((i >> b) & 1u);
    }
    if (j > i)
    {
      if (used + 2 > budget)
      {
        if (moved > 0)
        {
          FFTCONVOLVER_TAIL_WORK(quantum, "move", moved);
        }
        return used;
      }
      std::swap(_buf[2 * i], _buf[2 * j]);
      std::swap(_buf[2 * i + 1], _buf[2 * j + 1]);
      used += 2;
      moved += 2;
    }
    ++_butterfly;
  }
  if (moved > 0)
  {
    FFTCONVOLVER_TAIL_WORK(quantum, "move", moved);
  }
  if (_stage == 0)
  {
    // Bit reversal did not finish within this budget: resume next time.
    if (_butterfly < _n)
    {
      return used;
    }
    _stage = 1;
    _butterfly = 0;
  }
  if (finished())
  {
    return used;
  }

  // Phases 1..log2n: radix-2 butterfly stages, one unit per butterfly.
  size_t butterflies = 0;
  while (_stage <= _log2n && used < budget)
  {
    const size_t stage = _stage - 1;
    const size_t half = size_t(1) << stage;
    const size_t m = half << 1;
    const size_t group = _butterfly / half;
    const size_t j = _butterfly % half;
    const size_t k = group * m;
    const size_t tw = j * (_n / m);
    const Sample wr = _twiddle[2 * tw];
    const Sample wi = _twiddle[2 * tw + 1];
    const size_t ia = 2 * (k + j);
    const size_t ib = 2 * (k + j + half);
    const Sample ar = _buf[ia];
    const Sample ai = _buf[ia + 1];
    const Sample br = _buf[ib];
    const Sample bi = _buf[ib + 1];
    const Sample tr = wr * br - wi * bi;
    const Sample ti = wr * bi + wi * br;
    _buf[ia] = ar + tr;
    _buf[ia + 1] = ai + ti;
    _buf[ib] = ar - tr;
    _buf[ib + 1] = ai - ti;
    ++_butterfly;
    ++used;
    ++butterflies;
    if (_butterfly == _n / 2)
    {
      _stage++;
      _butterfly = 0;
    }
  }
  if (butterflies > 0)
  {
    FFTCONVOLVER_TAIL_WORK(quantum, "butterfly", butterflies);
  }
  return used;
}


// ============================================================
// Construction / teardown
// ============================================================

CooperativeFFTConvolver::CooperativeFFTConvolver() :
  _B(0),
  _T(0),
  _N(0),
  _Q(0),
  _P(0),
  _budget(0),
  _hasTail(false),
  _initialized(false),
  _headSegCount(0),
  _headLog2(0),
  _headFill(0),
  _headCurrent(0),
  _captureCount(0),
  _emitted(0),
  _macPartition(0),
  _macBin(0),
  _copyPos(0),
  _storePos(0),
  _inverseLoadPos(0),
  _scalePos(0),
  _overlapPos(0),
  _stage(JOB_IDLE),
  _jobBlock(0),
  _ringMask(0),
  _ringRead(0),
  _samplePos(0),
  _quantumsDone(0)
{
}


CooperativeFFTConvolver::~CooperativeFFTConvolver()
{
  reset();
}


bool CooperativeFFTConvolver::isPowerOf2(size_t v) const
{
  return isPowerOf2Impl(v);
}


void CooperativeFFTConvolver::reset()
{
  _B = 0;
  _T = 0;
  _N = 0;
  _Q = 0;
  _P = 0;
  _budget = 0;
  _hasTail = false;
  _initialized = false;

  std::vector<Sample>().swap(_ir);

  _headSegCount = 0;
  _headLog2 = 0;
  _headFill = 0;
  _headCurrent = 0;
  std::vector<std::vector<Sample> >().swap(_headSpectrum);
  std::vector<std::vector<Sample> >().swap(_headIRSpectrum);
  std::vector<Sample>().swap(_headFFT);
  std::vector<Sample>().swap(_headPreMultiplied);
  std::vector<Sample>().swap(_headConv);
  std::vector<Sample>().swap(_headOverlap);
  std::vector<Sample>().swap(_headInput);
  std::vector<Sample>().swap(_headTwiddleFwd);
  std::vector<Sample>().swap(_headTwiddleInv);
  std::vector<size_t>().swap(_headBitRev);

  std::vector<std::vector<Sample> >().swap(_tailIRSpectrum);
  std::vector<Sample>().swap(_accum);
  std::vector<Sample>().swap(_specRing);
  std::vector<Sample>().swap(_captureRing);
  std::vector<Sample>().swap(_tailCap);
  _fwd.release();
  _inv.release();
  _captureCount = 0;
  _emitted = 0;
  _macPartition = 0;
  _macBin = 0;
  _copyPos = 0;
  _storePos = 0;
  _inverseLoadPos = 0;
  _scalePos = 0;
  _overlapPos = 0;
  _stage = JOB_IDLE;
  _jobBlock = 0;

  std::vector<Sample>().swap(_outRing);
  _ringMask = 0;
  _ringRead = 0;
  _samplePos = 0;
  _quantumsDone = 0;
}


// ============================================================
// Initialization
// ============================================================

bool CooperativeFFTConvolver::init(size_t headBlockSize,
                                   size_t tailBlockSize,
                                   const Sample* ir,
                                   size_t irLen)
{
  reset();

  // Dimensions are validated and used exactly as passed in (no rounding).
  if (!isPowerOf2(headBlockSize) || headBlockSize < 8 || headBlockSize > 256)
  {
    return false;
  }
  if (!isPowerOf2(tailBlockSize) || tailBlockSize < 64 || tailBlockSize > 4096)
  {
    return false;
  }
  if (tailBlockSize < 8 * headBlockSize)
  {
    return false;
  }
  if (irLen > 65536)
  {
    return false;
  }
  if (irLen > 0 && ir == 0)
  {
    return false;
  }
  for (size_t i = 0; i < irLen; ++i)
  {
    if (!std::isfinite(ir[i]))
    {
      reset();
      return false;
    }
  }

  // Owned copy of the response (empty response is valid and means silence).
  if (irLen > 0)
  {
    _ir.assign(ir, ir + irLen);
  }

  _B = headBlockSize;
  _T = tailBlockSize;
  _N = 2 * _T;
  _Q = _T / _B;
  _initialized = true;

  // ----- Head path: the first 2*T coefficients, block-B partitions -----
  const size_t headIrLen = std::min(irLen, 2 * _T);
  _headSegCount = (headIrLen + _B - 1) / _B;
  _headLog2 = log2Impl(2 * _B);
  buildTables(2 * _B, false, _headTwiddleFwd, _headBitRev);
  buildTables(2 * _B, true, _headTwiddleInv, _headBitRev);

  _headFFT.assign(4 * _B, 0.0f);
  _headPreMultiplied.assign(4 * _B, 0.0f);
  _headConv.assign(4 * _B, 0.0f);
  _headOverlap.assign(_B, 0.0f);
  _headInput.assign(_B, 0.0f);
  _headFill = 0;
  _headCurrent = 0;

  if (_headSegCount > 0)
  {
    _headSpectrum.assign(_headSegCount, std::vector<Sample>(4 * _B, 0.0f));
    _headIRSpectrum.assign(_headSegCount, std::vector<Sample>(4 * _B, 0.0f));
    std::vector<Sample> complexBuf(4 * _B, 0.0f);
    for (size_t s = 0; s < _headSegCount; ++s)
    {
      for (size_t i = 0; i < 2 * _B; ++i)
      {
        complexBuf[2 * i] = 0.0f;
        complexBuf[2 * i + 1] = 0.0f;
      }
      const size_t offset = s * _B;
      const size_t take = std::min(_B, headIrLen - offset);
      for (size_t i = 0; i < take; ++i)
      {
        complexBuf[2 * i] = _ir[offset + i];
      }
      fftInPlace(complexBuf.data(), 2 * _B, _headLog2, _headBitRev, _headTwiddleFwd);
      std::copy(complexBuf.begin(), complexBuf.end(), _headIRSpectrum[s].begin());
    }
  }

  // ----- Tail path: coefficients from 2*T onward, N=2T FFT partitions -----
  const size_t tailIrLen = (irLen > 2 * _T) ? (irLen - 2 * _T) : 0;
  _P = (tailIrLen + _T - 1) / _T;
  _hasTail = (_P > 0);
  const size_t tailLog2 = log2Impl(_N);

  // Per-quantum work budget exactly as specified.
  const size_t F = (_N / 2) * tailLog2;
  const size_t total = 2 * F + _P * _N + 12 * _N;
  _budget = (total + _Q - 1) / _Q + 16;

  // Tail output ring: 4T slots. A job created at sample (j+1)T never writes
  // past jT+4T-1, which is at most 3T ahead of the current read position, so a
  // slot is never reused before it has been read back and cleared.
  _outRing.assign(4 * _T, 0.0f);
  _ringMask = 4 * _T - 1;

  if (_hasTail)
  {
    std::vector<Sample> twiddleN;
    std::vector<size_t> bitrevN;
    buildTables(_N, false, twiddleN, bitrevN);

    _fwd.configure(_N, false);
    _inv.configure(_N, true);
    _accum.assign(2 * _N, 0.0f);
    _specRing.assign(_P * 2 * _N, 0.0f);
    // Four captured-block snapshots are more than enough: job j-1 always
    // finishes (deadline jT+T) before job j is even captured (at (j+1)T).
    _captureRing.assign(kCaptureSlots * _T, 0.0f);
    _tailCap.assign(_T, 0.0f);
    _tailIRSpectrum.assign(_P, std::vector<Sample>(2 * _N, 0.0f));

    std::vector<Sample> complexBuf(2 * _N, 0.0f);
    for (size_t p = 0; p < _P; ++p)
    {
      for (size_t i = 0; i < _N; ++i)
      {
        complexBuf[2 * i] = 0.0f;
        complexBuf[2 * i + 1] = 0.0f;
      }
      const size_t offset = 2 * _T + p * _T;
      const size_t take = std::min(_T, irLen - offset);
      for (size_t i = 0; i < take; ++i)
      {
        complexBuf[2 * i] = _ir[offset + i];
      }
      fftInPlace(complexBuf.data(), _N, tailLog2, bitrevN, twiddleN);
      std::copy(complexBuf.begin(), complexBuf.end(), _tailIRSpectrum[p].begin());
    }
  }

  _stage = JOB_IDLE;
  _jobBlock = 0;
  _captureCount = 0;
  _emitted = 0;
  _ringRead = 0;
  _samplePos = 0;
  _quantumsDone = 0;
  return true;
}


// ============================================================
// Tail scheduling
// ============================================================

void CooperativeFFTConvolver::startNextJob()
{
  // Jobs are processed in capture order; _emitted is also the next block index.
  _jobBlock = _emitted;
  _stage = JOB_COPY;
  _copyPos = 0;
  _storePos = 0;
  _macPartition = 0;
  _macBin = 0;
  _inverseLoadPos = 0;
  _scalePos = 0;
  _overlapPos = 0;
}


size_t CooperativeFFTConvolver::runTail(size_t quantum, size_t budget)
{
  size_t used = 0;

  while (used < budget)
  {
    if (_stage == JOB_IDLE || _stage == JOB_DONE)
    {
      // Block j is fully captured only at sample (j+1)T, i.e. at the end of
      // quantum (j+1)*Q-1. A job must never start before that interval has
      // actually completed, even when one process() call batches several
      // quanta together.
      const bool captureComplete = ((_emitted + 1) * _Q <= quantum + 1);
      if (_emitted < _captureCount && captureComplete)
      {
        startNextJob();
      }
      else
      {
        return used;
      }
    }

    // Snapshot of the input block processed by the active job.
    const Sample* capture = _captureRing.data() + (_jobBlock % kCaptureSlots) * _T;

    switch (_stage)
    {
      case JOB_COPY:
      {
        // Pack T real samples plus T zeros into N complex slots (1 unit each).
        size_t moved = 0;
        Sample* dst = _fwd.data();
        while (_copyPos < _N && used < budget)
        {
          dst[2 * _copyPos] = (_copyPos < _T) ? capture[_copyPos] : 0.0f;
          dst[2 * _copyPos + 1] = 0.0f;
          ++_copyPos;
          ++used;
          ++moved;
        }
        if (moved > 0)
        {
          FFTCONVOLVER_TAIL_WORK(quantum, "move", moved);
        }
        if (_copyPos == _N)
        {
          _fwd.rewind();
          _stage = JOB_FORWARD;
        }
        else
        {
          return used;
        }
        break;
      }

      case JOB_FORWARD:
      {
        used += _fwd.run(quantum, budget - used);
        if (!_fwd.finished())
        {
          return used;
        }
        _stage = JOB_FORWARD_STORE;
        _storePos = 0;
        break;
      }

      case JOB_FORWARD_STORE:
      {
        // Store X_j in the spectrum ring (one complex-slot move) and clear the
        // accumulator slot (one complex-slot clear): two move units per bin.
        const size_t ringSlot = (_jobBlock % _P) * 2 * _N;
        size_t moved = 0;
        const Sample* src = _fwd.data();
        while (_storePos < _N)
        {
          if (used + 2 > budget)
          {
            if (moved > 0)
            {
              FFTCONVOLVER_TAIL_WORK(quantum, "move", moved);
            }
            return used;
          }
          const size_t re = 2 * _storePos;
          _specRing[ringSlot + re] = src[re];
          _specRing[ringSlot + re + 1] = src[re + 1];
          _accum[re] = 0.0f;
          _accum[re + 1] = 0.0f;
          ++_storePos;
          used += 2;
          moved += 2;
        }
        if (moved > 0)
        {
          FFTCONVOLVER_TAIL_WORK(quantum, "move", moved);
        }
        if (_storePos == _N)
        {
          _stage = JOB_MAC;
          _macPartition = 0;
          _macBin = 0;
        }
        else
        {
          return used;
        }
        break;
      }

      case JOB_MAC:
      {
        // accum += X_(j-p) * H_p over every partition and frequency bin.
        size_t macs = 0;
        while (_macPartition < _P && used < budget)
        {
          const size_t historySlot =
            ((_jobBlock + _P - _macPartition) % _P) * 2 * _N;
          const Sample* xp = _specRing.data() + historySlot;
          const Sample* hp = _tailIRSpectrum[_macPartition].data();
          while (_macBin < _N && used < budget)
          {
            const size_t re = 2 * _macBin;
            const size_t im = re + 1;
            const Sample xr = xp[re];
            const Sample xi = xp[im];
            const Sample hr = hp[re];
            const Sample hi = hp[im];
            _accum[re] += xr * hr - xi * hi;
            _accum[im] += xr * hi + xi * hr;
            ++_macBin;
            ++used;
            ++macs;
          }
          if (_macBin == _N)
          {
            ++_macPartition;
            _macBin = 0;
          }
        }
        if (macs > 0)
        {
          FFTCONVOLVER_TAIL_WORK(quantum, "mac", macs);
        }
        if (_macPartition == _P)
        {
          _stage = JOB_INVERSE_LOAD;
          _inverseLoadPos = 0;
        }
        else
        {
          return used;
        }
        break;
      }

      case JOB_INVERSE_LOAD:
      {
        // Copy the accumulated spectrum into the inverse FFT (complex moves).
        size_t moved = 0;
        Sample* dst = _inv.data();
        while (_inverseLoadPos < _N && used < budget)
        {
          const size_t re = 2 * _inverseLoadPos;
          dst[re] = _accum[re];
          dst[re + 1] = _accum[re + 1];
          ++_inverseLoadPos;
          ++used;
          ++moved;
        }
        if (moved > 0)
        {
          FFTCONVOLVER_TAIL_WORK(quantum, "move", moved);
        }
        if (_inverseLoadPos == _N)
        {
          _inv.rewind();
          _stage = JOB_INVERSE;
        }
        else
        {
          return used;
        }
        break;
      }

      case JOB_INVERSE:
      {
        used += _inv.run(quantum, budget - used);
        if (!_inv.finished())
        {
          return used;
        }
        _stage = JOB_SCALE;
        _scalePos = 0;
        break;
      }

      case JOB_SCALE:
      {
        // Unscaled inverse FFT: scale each complex slot by 1/N (1 unit/slot).
        const Sample scale = 1.0f / static_cast<Sample>(_N);
        size_t moved = 0;
        Sample* dst = _inv.data();
        while (_scalePos < _N && used < budget)
        {
          const size_t re = 2 * _scalePos;
          dst[re] *= scale;
          dst[re + 1] *= scale;
          ++_scalePos;
          ++used;
          ++moved;
        }
        if (moved > 0)
        {
          FFTCONVOLVER_TAIL_WORK(quantum, "move", moved);
        }
        if (_scalePos == _N)
        {
          _stage = JOB_OVERLAP;
          _overlapPos = 0;
        }
        else
        {
          return used;
        }
        break;
      }

      case JOB_OVERLAP:
      {
        // Job j's window starts at jT+2T. The first T inverse samples belong
        // to that window, the second T to the following window (overlap-save),
        // where the next job's first half is summed into the same slots.
        const size_t base = _jobBlock * _T + 2 * _T;
        const Sample* src = _inv.data();
        size_t moved = 0;
        while (_overlapPos < _N && used < budget)
        {
          const size_t absolute = base + _overlapPos;
          _outRing[absolute & _ringMask] += src[2 * _overlapPos];
          ++_overlapPos;
          ++used;
          ++moved;
        }
        if (moved > 0)
        {
          FFTCONVOLVER_TAIL_WORK(quantum, "move", moved);
        }
        if (_overlapPos == _N)
        {
          ++_emitted;
          _stage = JOB_DONE;
          return used;
        }
        return used;
      }

      default:
        return used;
    }
  }
  return used;
}


// ============================================================
// Head path (block-B partitioned convolution, zero latency)
// ============================================================

void CooperativeFFTConvolver::headProcess(const Sample* input, Sample* output, size_t len)
{
  const Sample invScale = 1.0f / static_cast<Sample>(2 * _B);

  size_t processed = 0;
  while (processed < len)
  {
    const bool inputBufferWasEmpty = (_headFill == 0);
    const size_t processing = std::min(len - processed, _B - _headFill);
    const size_t inputBufferPos = _headFill;

    // Capture the input before any output write, so input == output is safe.
    for (size_t i = 0; i < processing; ++i)
    {
      _headInput[inputBufferPos + i] = input[processed + i];
    }
    _headFill += processing;

    // Forward FFT of the partially filled block (unfilled samples are zero =>
    // strictly causal, no look-ahead).
    for (size_t i = 0; i < _B; ++i)
    {
      _headFFT[2 * i] = (i < _headFill) ? _headInput[i] : 0.0f;
      _headFFT[2 * i + 1] = 0.0f;
    }
    for (size_t i = _B; i < 2 * _B; ++i)
    {
      _headFFT[2 * i] = 0.0f;
      _headFFT[2 * i + 1] = 0.0f;
    }
    fftInPlace(_headFFT.data(), 2 * _B, _headLog2, _headBitRev, _headTwiddleFwd);

    // Older input segments only need to be accumulated when a fresh block
    // starts; within a partial block the set of older segments is unchanged.
    if (inputBufferWasEmpty)
    {
      for (size_t i = 0; i < 4 * _B; ++i)
      {
        _headPreMultiplied[i] = 0.0f;
      }
      for (size_t i = 1; i < _headSegCount; ++i)
      {
        const size_t indexAudio = (_headCurrent + i) % _headSegCount;
        const Sample* xa = _headSpectrum[indexAudio].data();
        const Sample* ha = _headIRSpectrum[i].data();
        for (size_t bin = 0; bin < 2 * _B; ++bin)
        {
          const size_t re = 2 * bin;
          const size_t im = re + 1;
          _headPreMultiplied[re] += xa[re] * ha[re] - xa[im] * ha[im];
          _headPreMultiplied[im] += xa[re] * ha[im] + xa[im] * ha[re];
        }
      }
    }

    // Current (partial) block against IR segment 0.
    std::copy(_headPreMultiplied.begin(), _headPreMultiplied.end(), _headConv.begin());
    {
      const Sample* xa = _headFFT.data();
      const Sample* ha = _headIRSpectrum[0].data();
      for (size_t bin = 0; bin < 2 * _B; ++bin)
      {
        const size_t re = 2 * bin;
        const size_t im = re + 1;
        _headConv[re] += xa[re] * ha[re] - xa[im] * ha[im];
        _headConv[im] += xa[re] * ha[im] + xa[im] * ha[re];
      }
    }
    fftInPlace(_headConv.data(), 2 * _B, _headLog2, _headBitRev, _headTwiddleInv);

    for (size_t i = 0; i < processing; ++i)
    {
      const size_t pos = inputBufferPos + i;
      output[processed + i] = _headConv[2 * pos] * invScale + _headOverlap[pos];
    }

    if (_headFill == _B)
    {
      // Persist the completed block spectrum and the overlap tail.
      std::copy(_headFFT.begin(), _headFFT.end(), _headSpectrum[_headCurrent].begin());
      for (size_t i = 0; i < _B; ++i)
      {
        _headOverlap[i] = _headConv[2 * (_B + i)] * invScale;
      }
      for (size_t i = 0; i < _B; ++i)
      {
        _headInput[i] = 0.0f;
      }
      _headFill = 0;
      _headCurrent = (_headCurrent > 0) ? (_headCurrent - 1) : (_headSegCount - 1);
    }

    processed += processing;
  }
}


// ============================================================
// Public process()
// ============================================================

void CooperativeFFTConvolver::process(const Sample* input, Sample* output, size_t len)
{
  if (len == 0)
  {
    // No sample position or task advancement, no pointer dereference.
    return;
  }

  if (!_initialized)
  {
    for (size_t i = 0; i < len; ++i)
    {
      output[i] = 0.0f;
    }
    return;
  }

  if (_headSegCount == 0)
  {
    // A valid but empty impulse response means silence.
    for (size_t i = 0; i < len; ++i)
    {
      output[i] = 0.0f;
    }
    _samplePos += len;
    _quantumsDone = _samplePos / _B;
    return;
  }

  // Step 1: capture the tail input before anything is written to the caller's
  // buffer, since input and output may alias. Each completed T-sample block is
  // snapshotted so pending jobs never see overwritten data.
  if (_hasTail)
  {
    for (size_t i = 0; i < len; ++i)
    {
      const size_t capPos = _samplePos % _T;
      _tailCap[capPos] = input[i];
      ++_samplePos;
      if (_samplePos % _T == 0)
      {
        const size_t block = _captureCount;
        Sample* snapshot = _captureRing.data() + (block % kCaptureSlots) * _T;
        for (size_t k = 0; k < _T; ++k)
        {
          snapshot[k] = _tailCap[k];
        }
        ++_captureCount;
      }
    }
  }
  else
  {
    _samplePos += len;
  }

  // Step 2: head convolution directly into the caller's output buffer.
  headProcess(input, output, len);

  // Step 3: advance the cooperative tail once per completed B-sample quantum.
  // Sub-B fragments do not trigger any tail work.
  if (_hasTail)
  {
    const size_t newQuantums = _samplePos / _B;
    for (size_t q = _quantumsDone; q < newQuantums; ++q)
    {
      runTail(q, _budget);
    }
    _quantumsDone = newQuantums;
  }
  else
  {
    _quantumsDone = _samplePos / _B;
  }

  // Step 4: sum the prepared tail output into the result and clear the slots.
  // Every tail block needed by these samples was finished before its window
  // opened (see the scheduling notes at the top of this file).
  for (size_t i = 0; i < len; ++i)
  {
    Sample& slot = _outRing[(_ringRead + i) & _ringMask];
    output[i] += slot;
    slot = 0.0f;
  }
  _ringRead += len;
}

} // End of namespace fftconvolver
