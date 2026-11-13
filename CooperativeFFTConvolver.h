// ==================================================================================
// CooperativeFFTConvolver
//
// A single-threaded, cooperatively scheduled variant of the 2-stage partitioned
// FFT convolution. The head of the impulse response (the first 2*T samples) is
// processed with a zero-latency, block-B partitioned convolution, while the
// long tail is processed with size-N=2T FFT partitions. All tail work (packing,
// forward FFT, frequency-domain multiply-accumulate, inverse FFT, scaling and
// overlap-add) is spread over subsequent B-sample quanta instead of being
// computed in one burst when a tail block fills up. No worker threads are
// created, no extra block latency is introduced and no heap memory is
// allocated or released during process().
// ==================================================================================

#ifndef _FFTCONVOLVER_COOPERATIVEFFTCONVOLVER_H
#define _FFTCONVOLVER_COOPERATIVEFFTCONVOLVER_H

#include "Utilities.h"

#include <cstddef>
#include <vector>


namespace fftconvolver
{

// The test build force-includes support/work_trace.h which provides the work
// tracing macro. In a normal build it is a no-op.
#ifndef FFTCONVOLVER_TAIL_WORK
  #define FFTCONVOLVER_TAIL_WORK(quantum, kind, units) ((void)0)
#endif


class CooperativeFFTConvolver
{
public:
  CooperativeFFTConvolver();
  ~CooperativeFFTConvolver();

  /**
  * @brief Initializes the convolver
  * @param headBlockSize Head block size B (power of 2, 8..256, used as-is)
  * @param tailBlockSize Tail block size T (power of 2, 64..4096, T >= 8*B)
  * @param ir The impulse response (may be null if irLen == 0)
  * @param irLen Length of the impulse response (0..65536)
  * @return true: Success - false: invalid configuration (uninitialized state)
  */
  bool init(size_t headBlockSize, size_t tailBlockSize, const Sample* ir, size_t irLen);

  /**
  * @brief Convolves the given input samples and writes the result
  *
  * Arbitrary lengths are supported, input == output is supported and a
  * zero-length call does not advance the sample position or any tail task.
  * When uninitialized, the output is simply filled with zeros.
  */
  void process(const Sample* input, Sample* output, size_t len);

  /**
  * @brief Releases all configuration and history (returns to uninitialized state)
  */
  void reset();

private:
  // Resumable radix-2 DIT complex FFT working on interleaved complex data.
  // All twiddle factors are precomputed during init(), so no trigonometric
  // functions are called while processing.
  class ResumableFFT
  {
  public:
    ResumableFFT();

    void configure(size_t n, bool inverse);

    // Frees all workspace memory (used by the owning convolver's reset()).
    void release();

    // Runs at most `budget` work units of the transform and reports the number
    // of units actually consumed. The transform resumes where the last call
    // stopped and is fully done once finished() returns true.
    size_t run(size_t quantum, size_t budget);

    bool finished() const { return _stage > _log2n; }
    void rewind() { _stage = 0; _butterfly = 0; }

    Sample* data() { return _buf.data(); }
    const Sample* data() const { return _buf.data(); }
    size_t complexSize() const { return _n; }

  private:
    size_t _n;
    size_t _log2n;
    bool _inverse;
    size_t _stage;      // 0 = bit reversal, 1..log2n = butterfly stages
    size_t _butterfly;  // Progress within the current phase
    std::vector<Sample> _buf;      // Interleaved complex, 2*_n samples
    std::vector<Sample> _twiddle;  // Interleaved complex twiddles, 2*_n samples
  };

  // Stages of the resumable tail job pipeline.
  enum JobStage
  {
    JOB_IDLE = 0,
    JOB_COPY,          // Pack the captured T input samples into the FFT buffer
    JOB_FORWARD,       // Forward FFT of the captured block
    JOB_FORWARD_STORE, // Store X_j into the spectrum ring and clear accumulator
    JOB_MAC,           // Frequency-domain multiply-accumulate with all partitions
    JOB_INVERSE_LOAD,  // Copy the accumulator into the inverse FFT buffer
    JOB_INVERSE,       // Inverse FFT of the accumulated spectrum
    JOB_SCALE,         // 1/N scaling of the inverse result
    JOB_OVERLAP,       // Overlap-add the 2T result into the output ring
    JOB_DONE
  };

  bool isPowerOf2(size_t v) const;
  void startNextJob();
  size_t runTail(size_t quantum, size_t budget);
  void headProcess(const Sample* input, Sample* output, size_t len);

  // Configuration
  size_t _B;
  size_t _T;
  size_t _N;
  size_t _Q;
  size_t _P;                 // Number of tail IR partitions
  size_t _budget;            // Per-quantum tail work budget
  bool _hasTail;
  bool _initialized;

  // Owned copy of the impulse response
  std::vector<Sample> _ir;

  // ----- Head path: block-B partitioned convolution with a 2B FFT -----
  size_t _headSegCount;
  std::vector<std::vector<Sample> > _headSpectrum;   // Circular input spectra
  std::vector<std::vector<Sample> > _headIRSpectrum; // IR partition spectra
  std::vector<Sample> _headFFT;          // Scratch complex buffer (4B samples)
  std::vector<Sample> _headPreMultiplied;
  std::vector<Sample> _headConv;
  std::vector<Sample> _headOverlap;      // B-sample overlap from the previous block
  std::vector<Sample> _headInput;        // B-sample input accumulator
  std::vector<Sample> _headTwiddleFwd;   // Twiddles for the small forward FFT
  std::vector<Sample> _headTwiddleInv;   // Twiddles for the small inverse FFT
  std::vector<size_t> _headBitRev;       // Bit-reversal table for the small FFT
  size_t _headLog2;
  size_t _headFill;
  size_t _headCurrent;

  // ----- Tail path -----
  std::vector<std::vector<Sample> > _tailIRSpectrum; // P spectra, N complex each
  ResumableFFT _fwd;
  ResumableFFT _inv;
  std::vector<Sample> _accum;       // Frequency-domain accumulator (N complex)
  std::vector<Sample> _specRing;    // Circular ring of the last P input spectra
  std::vector<Sample> _captureRing; // Snapshots of captured T-blocks (4 blocks)
  std::vector<Sample> _tailCap;     // T-sample capture accumulator
  size_t _captureCount;             // Blocks captured so far
  size_t _emitted;                  // Jobs fully overlap-added so far
  size_t _macPartition;             // Next IR partition for JOB_MAC
  size_t _macBin;                   // Next frequency bin within a partition
  size_t _copyPos;                  // Progress of JOB_COPY
  size_t _storePos;                 // Progress of JOB_FORWARD_STORE
  size_t _inverseLoadPos;           // Progress of JOB_INVERSE_LOAD
  size_t _scalePos;                 // Progress of JOB_SCALE
  size_t _overlapPos;               // Progress of JOB_OVERLAP
  JobStage _stage;
  size_t _jobBlock;                 // Absolute block number of the active job

  // Causal output ring: head and tail contributions are summed here and read
  // back in sample order. Slots are cleared when read.
  std::vector<Sample> _outRing;
  size_t _ringMask;
  size_t _ringRead;      // Absolute sample index of the next unread sample

  size_t _samplePos;     // Total samples received since init()
  size_t _quantumsDone;  // Number of completed B-sample quantums

  // Prevent uncontrolled usage
  CooperativeFFTConvolver(const CooperativeFFTConvolver&);
  CooperativeFFTConvolver& operator=(const CooperativeFFTConvolver&);
};

} // End of namespace fftconvolver

#endif // Header guard
