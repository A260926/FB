// ==================================================================================
// CooperativeFFTConvolver
//
// A single-threaded, cooperatively scheduled variant of the 2-stage FFT
// convolver. The short head of the impulse response is processed by the
// existing partitioned FFTConvolver (block size B), while the long tail is
// partitioned at size T and processed by resumable N=2T point FFTs. Instead
// of completing a whole tail batch inside one audio call (as
// TwoStageFFTConvolver does on its default path), every tail job is split
// into small work units and spread over the audio quanta that lie between
// the capture of a T-sample input block and the deadline of its first
// audible contribution (2T samples later). No worker threads, no locks and
// no heap activity are used while process() is running.
// ==================================================================================

#ifndef _FFTCONVOLVER_COOPERATIVEFFTCONVOLVER_H
#define _FFTCONVOLVER_COOPERATIVEFFTCONVOLVER_H

#include "Utilities.h"


namespace fftconvolver
{

/**
* @class CooperativeFFTConvolver
* @brief Cooperative (thread-free, resumable) 2-stage FFT convolution
*
* Usage contract:
*
* - init() validates the geometry (B: power of 2 in [8,256], T: power of 2
*   in [64,4096], T >= 8B, impulse response length <= 65536), keeps its own
*   copy of the impulse response and precomputes every spectrum/twiddle.
*   An empty impulse response is legal and produces silence.
*
* - process() accepts arbitrary lengths (including input == output and
*   len == 0) and never allocates, frees, locks or blocks. Quantums are
*   defined by the cumulative stream position in units of B samples, so the
*   tail schedule is identical no matter how the stream is split into calls.
*
* - reset() releases all configuration and history and returns the object to
*   the uninitialized state, in which process() fills the output with zeros.
*/
class CooperativeFFTConvolver
{
public:
  CooperativeFFTConvolver();
  ~CooperativeFFTConvolver();

  /**
  * @brief Initializes the convolver
  * @param headBlockSize Head block size B (power of 2, 8..256, used as given)
  * @param tailBlockSize Tail partition size T (power of 2, 64..4096, T >= 8B)
  * @param ir The impulse response (may be null only if irLen == 0)
  * @param irLen Number of impulse response coefficients (<= 65536)
  * @return true: Success - false: invalid configuration (object is reset)
  */
  bool init(size_t headBlockSize,
            size_t tailBlockSize,
            const Sample* ir,
            size_t irLen);

  /**
  * @brief Convolves the given input samples and outputs the result
  * @param input Input samples (may alias output, may be null if len == 0)
  * @param output Convolution result (may be null if len == 0)
  * @param len Number of samples
  */
  void process(const Sample* input, Sample* output, size_t len);

  /**
  * @brief Discards the impulse response and all history
  */
  void reset();

private:
  struct Impl;
  Impl* _impl;

  // Prevent uncontrolled usage
  CooperativeFFTConvolver(const CooperativeFFTConvolver&);
  CooperativeFFTConvolver& operator=(const CooperativeFFTConvolver&);
};

} // End of namespace fftconvolver

#endif // Header guard
