// ==================================================================================
// Copyright (c) 2017 HiFi-LoFi
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is furnished
// to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ==================================================================================

#include "FFTConvolver.h"
#include "Snapshot.h"

#include <cassert>
#include <cmath>

#if defined (FFTCONVOLVER_USE_SSE)
  #include <xmmintrin.h>
#endif


namespace fftconvolver
{

FFTConvolver::FFTConvolver() :
  _blockSize(0),
  _segSize(0),
  _segCount(0),
  _fftComplexSize(0),
  _segments(),
  _segmentsIR(),
  _fftBuffer(),
  _fft(),
  _preMultiplied(),
  _conv(),
  _overlap(),
  _current(0),
  _inputBuffer(),
  _inputBufferFill(0),
  _ir()
{
}


FFTConvolver::~FFTConvolver()
{
  reset();
}


void FFTConvolver::reset()
{
  for (size_t i=0; i<_segCount; ++i)
  {
    delete _segments[i];
    delete _segmentsIR[i];
  }

  _blockSize = 0;
  _segSize = 0;
  _segCount = 0;
  _fftComplexSize = 0;
  _segments.clear();
  _segmentsIR.clear();
  _fftBuffer.clear();
  _fft.init(0);
  _preMultiplied.clear();
  _conv.clear();
  _overlap.clear();
  _current = 0;
  _inputBuffer.clear();
  _inputBufferFill = 0;
  _ir.clear();
}


bool FFTConvolver::init(size_t blockSize, const Sample* ir, size_t irLen)
{
  reset();

  if (blockSize == 0)
  {
    return false;
  }

  // Ignore zeros at the end of the impulse response because they only waste computation time
  while (irLen > 0 && ::fabs(ir[irLen-1]) < 0.000001f)
  {
    --irLen;
  }

  if (irLen == 0)
  {
    return true;
  }

  _blockSize = NextPowerOf2(blockSize);
  _segSize = 2 * _blockSize;
  _segCount = static_cast<size_t>(::ceil(static_cast<float>(irLen) / static_cast<float>(_blockSize)));
  _fftComplexSize = audiofft::AudioFFT::ComplexSize(_segSize);

  // FFT
  _fft.init(_segSize);
  _fftBuffer.resize(_segSize);

  // Prepare segments
  for (size_t i=0; i<_segCount; ++i)
  {
    _segments.push_back(new SplitComplex(_fftComplexSize));
  }

  // Prepare IR
  for (size_t i=0; i<_segCount; ++i)
  {
    SplitComplex* segment = new SplitComplex(_fftComplexSize);
    const size_t remaining = irLen - (i * _blockSize);
    const size_t sizeCopy = (remaining >= _blockSize) ? _blockSize : remaining;
    CopyAndPad(_fftBuffer, &ir[i*_blockSize], sizeCopy);
    _fft.fft(_fftBuffer.data(), segment->re(), segment->im());
    _segmentsIR.push_back(segment);
  }

  // Keep a self-contained copy of the actually used (trimmed) impulse
  // response so that snapshots do not depend on the caller's IR memory.
  _ir.assign(ir, ir + irLen);

  // Prepare convolution buffers
  _preMultiplied.resize(_fftComplexSize);
  _conv.resize(_fftComplexSize);
  _overlap.resize(_blockSize);

  // Prepare input buffer
  _inputBuffer.resize(_blockSize);
  _inputBufferFill = 0;

  // Reset current position
  _current = 0;

  return true;
}


void FFTConvolver::process(const Sample* input, Sample* output, size_t len)
{
  if (_segCount == 0)
  {
    ::memset(output, 0, len * sizeof(Sample));
    return;
  }

  size_t processed = 0;
  while (processed < len)
  {
    const bool inputBufferWasEmpty = (_inputBufferFill == 0);
    const size_t processing = std::min(len-processed, _blockSize-_inputBufferFill);
    const size_t inputBufferPos = _inputBufferFill;
    ::memcpy(_inputBuffer.data()+inputBufferPos, input+processed, processing * sizeof(Sample));

    // Forward FFT
    CopyAndPad(_fftBuffer, &_inputBuffer[0], _blockSize);
    _fft.fft(_fftBuffer.data(), _segments[_current]->re(), _segments[_current]->im());

    // Complex multiplication
    if (inputBufferWasEmpty)
    {
      _preMultiplied.setZero();
      for (size_t i=1; i<_segCount; ++i)
      {
        const size_t indexIr = i;
        const size_t indexAudio = (_current + i) % _segCount;
        ComplexMultiplyAccumulate(_preMultiplied, *_segmentsIR[indexIr], *_segments[indexAudio]);
      }
    }
    _conv.copyFrom(_preMultiplied);
    ComplexMultiplyAccumulate(_conv, *_segments[_current], *_segmentsIR[0]);

    // Backward FFT
    _fft.ifft(_fftBuffer.data(), _conv.re(), _conv.im());

    // Add overlap
    Sum(output+processed, _fftBuffer.data()+inputBufferPos, _overlap.data()+inputBufferPos, processing);

    // Input buffer full => Next block
    _inputBufferFill += processing;
    if (_inputBufferFill == _blockSize)
    {
      // Input buffer is empty again now
      _inputBuffer.setZero();
      _inputBufferFill = 0;

      // Save the overlap
      ::memcpy(_overlap.data(), _fftBuffer.data()+_blockSize, _blockSize * sizeof(Sample));

      // Update current segment
      _current = (_current > 0) ? (_current - 1) : (_segCount - 1);
    }

    processed += processing;
  }
}


namespace
{

// Copies the real and imaginary arrays of one split-complex buffer into
// the flat snapshot layout [re(0..n-1)][im(0..n-1)].
void StoreSplitComplex(std::vector<float>& dest, const SplitComplex& src)
{
  const size_t n = src.size();
  dest.resize(2u * n);
  if (n > 0)
  {
    ::memcpy(&dest[0], src.re(), n * sizeof(float));
    ::memcpy(&dest[n], src.im(), n * sizeof(float));
  }
}

} // End of anonymous namespace


// Fills a portable section from this instance. Used by exportState() and
// by TwoStageFFTConvolver for its nested convolvers.
void FFTConvolver::captureSection(snapshot::FFTSection& section) const
{
  if (_segCount == 0)
  {
    section = snapshot::FFTSection();
    return;
  }

  section = snapshot::FFTSection();
  section.empty = false;
  section.blockSize = static_cast<uint32_t>(_blockSize);
  section.segCount = static_cast<uint32_t>(_segCount);
  section.current = static_cast<uint32_t>(_current);
  section.inputFill = static_cast<uint32_t>(_inputBufferFill);
  section.ir = _ir;

  const size_t complexSize = _fftComplexSize;
  section.segments.resize(_segCount * 2u * complexSize);
  for (size_t i = 0; i < _segCount; ++i)
  {
    float* dst = &section.segments[i * 2u * complexSize];
    ::memcpy(dst, _segments[i]->re(), complexSize * sizeof(float));
    ::memcpy(dst + complexSize, _segments[i]->im(), complexSize * sizeof(float));
  }

  StoreSplitComplex(section.preMultiplied, _preMultiplied);

  section.overlap.resize(_blockSize);
  ::memcpy(&section.overlap[0], _overlap.data(), _blockSize * sizeof(float));

  section.inputBuffer.resize(_blockSize);
  ::memcpy(&section.inputBuffer[0], _inputBuffer.data(), _blockSize * sizeof(float));
}


// Rebuilds a complete convolver from a validated section into [target].
// All allocations happen here (during import, never during process()).
// Returns false only if the required sizes do not fit the uint32-based
// snapshot limits.
bool FFTConvolver::applySection(FFTConvolver& target, const snapshot::FFTSection& section)
{
  if (section.empty)
  {
    target.reset();
    return true;
  }

  const size_t blockSize = section.blockSize;
  const size_t segCount = section.segCount;
  const size_t segSize = 2u * blockSize;
  const size_t complexSize = audiofft::AudioFFT::ComplexSize(segSize);
  const size_t irLen = section.ir.size();

  FFTConvolver fresh;

  fresh._blockSize = blockSize;
  fresh._segSize = segSize;
  fresh._segCount = segCount;
  fresh._fftComplexSize = complexSize;

  fresh._fft.init(segSize);
  fresh._fftBuffer.resize(segSize);

  fresh._segments.reserve(segCount);
  fresh._segmentsIR.reserve(segCount);
  for (size_t i = 0; i < segCount; ++i)
  {
    fresh._segments.push_back(new SplitComplex(complexSize));
    fresh._segmentsIR.push_back(new SplitComplex(complexSize));
  }

  // Rebuild the IR spectra from the stored time-domain impulse response,
  // which is exactly what init() would compute.
  for (size_t i = 0; i < segCount; ++i)
  {
    const size_t remaining = irLen - (i * blockSize);
    const size_t sizeCopy = (remaining >= blockSize) ? blockSize : remaining;
    CopyAndPad(fresh._fftBuffer, &section.ir[i * blockSize], sizeCopy);
    fresh._fft.fft(fresh._fftBuffer.data(),
                   fresh._segmentsIR[i]->re(),
                   fresh._segmentsIR[i]->im());
  }

  fresh._ir = section.ir;

  fresh._preMultiplied.resize(complexSize);
  fresh._conv.resize(complexSize);
  fresh._overlap.resize(blockSize);
  fresh._inputBuffer.resize(blockSize);

  // Restore the dynamic processing state.
  for (size_t i = 0; i < segCount; ++i)
  {
    const float* src = &section.segments[i * 2u * complexSize];
    ::memcpy(fresh._segments[i]->re(), src, complexSize * sizeof(float));
    ::memcpy(fresh._segments[i]->im(), src + complexSize, complexSize * sizeof(float));
  }

  ::memcpy(fresh._preMultiplied.re(), &section.preMultiplied[0], complexSize * sizeof(float));
  ::memcpy(fresh._preMultiplied.im(), &section.preMultiplied[complexSize], complexSize * sizeof(float));

  ::memcpy(fresh._overlap.data(), &section.overlap[0], blockSize * sizeof(float));
  ::memcpy(fresh._inputBuffer.data(), &section.inputBuffer[0], blockSize * sizeof(float));

  fresh._current = section.current;
  fresh._inputBufferFill = section.inputFill;

  // Take over the freshly built state; the old state of [target] is
  // destroyed with [fresh].
  target.swapState(fresh);
  return true;
}


// Exchanges all internal resources with another instance. All swaps move
// pointers only - no per-sample copies and no additional allocations.
void FFTConvolver::swapState(FFTConvolver& other)
{
  std::swap(_blockSize, other._blockSize);
  std::swap(_segSize, other._segSize);
  std::swap(_segCount, other._segCount);
  std::swap(_fftComplexSize, other._fftComplexSize);
  std::swap(_segments, other._segments);
  std::swap(_segmentsIR, other._segmentsIR);
  SampleBuffer::Swap(_fftBuffer, other._fftBuffer);
  _fft.swap(other._fft);
  SplitComplex::Swap(_preMultiplied, other._preMultiplied);
  SplitComplex::Swap(_conv, other._conv);
  SampleBuffer::Swap(_overlap, other._overlap);
  std::swap(_current, other._current);
  SampleBuffer::Swap(_inputBuffer, other._inputBuffer);
  std::swap(_inputBufferFill, other._inputBufferFill);
  std::swap(_ir, other._ir);
}


bool FFTConvolver::exportState(std::vector<uint8_t>& out) const
{
  out.clear();

  snapshot::FFTSection section;
  captureSection(section);

  snapshot::Writer payload;
  if (!snapshot::WriteFFTSection(payload, snapshot::TAG_FFT_SECTION, section))
  {
    out.clear();
    return false;
  }

  snapshot::Writer file;
  if (!snapshot::WriteFileHeader(file, snapshot::MAGIC_FFT, snapshot::CLASS_FFT, payload))
  {
    out.clear();
    return false;
  }

  out.swap(file.buffer());
  return true;
}


bool FFTConvolver::importState(const uint8_t* data, size_t size)
{
  if (data == 0 || size == 0)
  {
    return false;
  }

  snapshot::Reader file(data, size);
  if (!snapshot::ReadFileHeader(file, snapshot::MAGIC_FFT, snapshot::CLASS_FFT))
  {
    return false;
  }

  snapshot::FFTSection section;
  if (!snapshot::ReadFFTSection(file, snapshot::TAG_FFT_SECTION, section) ||
      !file.fullyConsumed())
  {
    return false;
  }

  // Parsing and validation are complete at this point; only now may the
  // current (valid) state of this instance be replaced.
  return applySection(*this, section);
}

} // End of namespace fftconvolver
