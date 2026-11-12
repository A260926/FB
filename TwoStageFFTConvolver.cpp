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

#include "TwoStageFFTConvolver.h"
#include "Snapshot.h"

#include <algorithm>
#include <cmath>


namespace fftconvolver
{

TwoStageFFTConvolver::TwoStageFFTConvolver() :
  _headBlockSize(0),
  _tailBlockSize(0),
  _headConvolver(),
  _tailConvolver0(),
  _tailOutput0(),
  _tailPrecalculated0(0),
  _tailConvolver(),
  _tailOutput(),
  _tailPrecalculated(0),
  _tailInput(),
  _tailInputFill(0),
  _precalculatedPos(0),
  _backgroundProcessingInput(),
  _ir()
{
}


TwoStageFFTConvolver::~TwoStageFFTConvolver()
{
  reset();
}


void TwoStageFFTConvolver::reset()
{
  _headBlockSize = 0;
  _tailBlockSize = 0;
  _headConvolver.reset();
  _tailConvolver0.reset();
  _tailOutput0.clear();
  _tailPrecalculated0.clear();
  _tailConvolver.reset();
  _tailOutput.clear();
  _tailPrecalculated.clear();
  _tailInput.clear();
  _tailInputFill = 0;
  _tailInputFill = 0;
  _precalculatedPos = 0;
  _backgroundProcessingInput.clear();
  _ir.clear();
}

  
bool TwoStageFFTConvolver::init(size_t headBlockSize,
                                size_t tailBlockSize,
                                const Sample* ir,
                                size_t irLen)
{
  reset();

  if (headBlockSize == 0 || tailBlockSize == 0)
  {
    return false;
  }
  
  headBlockSize = std::max(size_t(1), headBlockSize);
  if (headBlockSize > tailBlockSize)
  {
    assert(false);
    std::swap(headBlockSize, tailBlockSize);
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

  // Keep a self-contained copy of the actually used (trimmed) impulse
  // response so that snapshots do not depend on the caller's IR memory.
  _ir.assign(ir, ir + irLen);

  _headBlockSize = NextPowerOf2(headBlockSize);
  _tailBlockSize = NextPowerOf2(tailBlockSize);

  const size_t headIrLen = std::min(irLen, _tailBlockSize);
  _headConvolver.init(_headBlockSize, ir, headIrLen);

  if (irLen > _tailBlockSize)
  {
    const size_t conv1IrLen = std::min(irLen-_tailBlockSize, _tailBlockSize);
    _tailConvolver0.init(_headBlockSize, ir+_tailBlockSize, conv1IrLen);
    _tailOutput0.resize(_tailBlockSize);
    _tailPrecalculated0.resize(_tailBlockSize);
  }

  if (irLen > 2 * _tailBlockSize)
  {
    const size_t tailIrLen = irLen - (2*_tailBlockSize);
    _tailConvolver.init(_tailBlockSize, ir+(2*_tailBlockSize), tailIrLen);
    _tailOutput.resize(_tailBlockSize);
    _tailPrecalculated.resize(_tailBlockSize);
    _backgroundProcessingInput.resize(_tailBlockSize);
  }

  if (_tailPrecalculated0.size() > 0 || _tailPrecalculated.size() > 0)
  {
    _tailInput.resize(_tailBlockSize);
  }
  _tailInputFill = 0;
  _precalculatedPos = 0;

  return true;
}


void TwoStageFFTConvolver::process(const Sample* input, Sample* output, size_t len)
{
  // Head
  _headConvolver.process(input, output, len);

  // Tail
  if (_tailInput.size() > 0)
  {
    size_t processed = 0;
    while (processed < len)
    {
      const size_t remaining = len - processed;
      const size_t processing = std::min(remaining, _headBlockSize - (_tailInputFill % _headBlockSize));
      assert(_tailInputFill + processing <= _tailBlockSize);

      // Sum head and tail
      const size_t sumBegin = processed;
      const size_t sumEnd = processed + processing;
      {
        // Sum: 1st tail block
        if (_tailPrecalculated0.size() > 0)
        {      
          size_t precalculatedPos = _precalculatedPos;
          for (size_t i=sumBegin; i<sumEnd; ++i)
          {
            output[i] += _tailPrecalculated0[precalculatedPos];
            ++precalculatedPos;
          }
        }

        // Sum: 2nd-Nth tail block
        if (_tailPrecalculated.size() > 0)
        {      
          size_t precalculatedPos = _precalculatedPos;
          for (size_t i=sumBegin; i<sumEnd; ++i)
          {
            output[i] += _tailPrecalculated[precalculatedPos];
            ++precalculatedPos;
          }
        }

        _precalculatedPos += processing;
      }

      // Fill input buffer for tail convolution
      ::memcpy(_tailInput.data()+_tailInputFill, input+processed, processing * sizeof(Sample));
      _tailInputFill += processing;
      assert(_tailInputFill <= _tailBlockSize);

      // Convolution: 1st tail block
      if (_tailPrecalculated0.size() > 0 && _tailInputFill % _headBlockSize == 0)
      {
        assert(_tailInputFill >= _headBlockSize);
        const size_t blockOffset = _tailInputFill - _headBlockSize;
        _tailConvolver0.process(_tailInput.data()+blockOffset, _tailOutput0.data()+blockOffset, _headBlockSize);
        if (_tailInputFill == _tailBlockSize)
        {          
          SampleBuffer::Swap(_tailPrecalculated0, _tailOutput0);
        }
      }

      // Convolution: 2nd-Nth tail block (might be done in some background thread)
      if (_tailPrecalculated.size() > 0 &&
          _tailInputFill == _tailBlockSize &&
          _backgroundProcessingInput.size() == _tailBlockSize &&
          _tailOutput.size() == _tailBlockSize)
      {
        waitForBackgroundProcessing();
        SampleBuffer::Swap(_tailPrecalculated, _tailOutput);
        _backgroundProcessingInput.copyFrom(_tailInput);
        startBackgroundProcessing();
      }
        
      if (_tailInputFill == _tailBlockSize)
      {
        _tailInputFill = 0;
        _precalculatedPos = 0;
      }

      processed += processing;
    }
  }
}


void TwoStageFFTConvolver::startBackgroundProcessing()
{
  doBackgroundProcessing();
}


void TwoStageFFTConvolver::waitForBackgroundProcessing()
{
}


void TwoStageFFTConvolver::doBackgroundProcessing()
{
  _tailConvolver.process(_backgroundProcessingInput.data(), _tailOutput.data(), _tailBlockSize);
}


namespace
{

void CopyBufferToVector(std::vector<float>& dest, const SampleBuffer& src)
{
  const size_t len = src.size();
  dest.resize(len);
  if (len > 0)
  {
    ::memcpy(&dest[0], src.data(), len * sizeof(float));
  }
}

void CopyVectorToBuffer(SampleBuffer& dest, const std::vector<float>& src)
{
  dest.resize(src.size());
  if (!src.empty())
  {
    ::memcpy(dest.data(), &src[0], src.size() * sizeof(float));
  }
}

} // End of anonymous namespace


bool TwoStageFFTConvolver::exportState(std::vector<uint8_t>& out) const
{
  out.clear();

  snapshot::TwoStageSection section;

  if (!_ir.empty())
  {
    section.empty = false;
    section.headBlockSize = static_cast<uint32_t>(_headBlockSize);
    section.tailBlockSize = static_cast<uint32_t>(_tailBlockSize);
    section.tailInputFill = static_cast<uint32_t>(_tailInputFill);
    section.precalculatedPos = static_cast<uint32_t>(_precalculatedPos);
    section.hasTail0 = (_tailPrecalculated0.size() > 0);
    section.hasTail = (_tailPrecalculated.size() > 0);
    section.ir = _ir;

    CopyBufferToVector(section.tailOutput0, _tailOutput0);
    CopyBufferToVector(section.tailPrecalculated0, _tailPrecalculated0);
    CopyBufferToVector(section.tailOutput, _tailOutput);
    CopyBufferToVector(section.tailPrecalculated, _tailPrecalculated);
    CopyBufferToVector(section.tailInput, _tailInput);
    CopyBufferToVector(section.backgroundProcessingInput, _backgroundProcessingInput);
  }

  // The nested convolver sections carry the frequency-domain histories and
  // the per-stage impulse responses.
  _headConvolver.captureSection(section.head);
  if (section.hasTail0)
  {
    _tailConvolver0.captureSection(section.tail0);
  }
  if (section.hasTail)
  {
    _tailConvolver.captureSection(section.tail);
  }

  snapshot::Writer payload;
  if (!snapshot::WriteTwoStageBody(payload, section) ||
      !snapshot::WriteFFTSection(payload, snapshot::TAG_HEAD, section.head))
  {
    out.clear();
    return false;
  }
  if (section.hasTail0 &&
      !snapshot::WriteFFTSection(payload, snapshot::TAG_TAIL0, section.tail0))
  {
    out.clear();
    return false;
  }
  if (section.hasTail &&
      !snapshot::WriteFFTSection(payload, snapshot::TAG_TAIL, section.tail))
  {
    out.clear();
    return false;
  }

  snapshot::Writer file;
  if (!snapshot::WriteFileHeader(file, snapshot::MAGIC_TWO, snapshot::CLASS_TWO, payload))
  {
    out.clear();
    return false;
  }

  out.swap(file.buffer());
  return true;
}


bool TwoStageFFTConvolver::importState(const uint8_t* data, size_t size)
{
  if (data == 0 || size == 0)
  {
    return false;
  }

  snapshot::Reader file(data, size);
  if (!snapshot::ReadFileHeader(file, snapshot::MAGIC_TWO, snapshot::CLASS_TWO))
  {
    return false;
  }

  snapshot::TwoStageSection section;
  if (!snapshot::ReadTwoStageBody(file, section))
  {
    return false;
  }

  if (!snapshot::ReadFFTSection(file, snapshot::TAG_HEAD, section.head))
  {
    return false;
  }

  if (section.empty)
  {
    if (!file.fullyConsumed() || !section.head.empty)
    {
      return false;
    }
  }
  else
  {
    if (section.head.empty ||
        section.head.blockSize != section.headBlockSize)
    {
      return false;
    }

    if (section.hasTail0)
    {
      if (!snapshot::ReadFFTSection(file, snapshot::TAG_TAIL0, section.tail0) ||
          section.tail0.empty ||
          section.tail0.blockSize != section.headBlockSize)
      {
        return false;
      }
    }
    if (section.hasTail)
    {
      if (!snapshot::ReadFFTSection(file, snapshot::TAG_TAIL, section.tail) ||
          section.tail.empty ||
          section.tail.blockSize != section.tailBlockSize)
      {
        return false;
      }
    }
    if (!file.fullyConsumed())
    {
      return false;
    }

    // The per-stage impulse responses stored in the nested sections must
    // match the corresponding slices of the full impulse response.
    const size_t irLen = section.ir.size();
    const size_t tailBlock = section.tailBlockSize;

    const size_t headIrLen = std::min(irLen, tailBlock);
    if (section.head.ir.size() != headIrLen ||
        ::memcmp(&section.head.ir[0], &section.ir[0], headIrLen * sizeof(float)) != 0)
    {
      return false;
    }

    if (section.hasTail0)
    {
      const size_t conv1IrLen = std::min(irLen - tailBlock, tailBlock);
      if (section.tail0.ir.size() != conv1IrLen ||
          ::memcmp(&section.tail0.ir[0], &section.ir[tailBlock],
                   conv1IrLen * sizeof(float)) != 0)
      {
        return false;
      }
    }

    if (section.hasTail)
    {
      const size_t tailIrLen = irLen - (2u * tailBlock);
      if (section.tail.ir.size() != tailIrLen ||
          ::memcmp(&section.tail.ir[0], &section.ir[2u * tailBlock],
                   tailIrLen * sizeof(float)) != 0)
      {
        return false;
      }
    }
  }

  // Everything has been parsed and validated. Build the replacement state
  // in a temporary object first, so that a failure here still leaves this
  // instance untouched.
  TwoStageFFTConvolver fresh;

  if (!section.empty)
  {
    fresh._headBlockSize = section.headBlockSize;
    fresh._tailBlockSize = section.tailBlockSize;
    fresh._tailInputFill = section.tailInputFill;
    fresh._precalculatedPos = section.precalculatedPos;
    fresh._ir = section.ir;

    if (!FFTConvolver::applySection(fresh._headConvolver, section.head))
    {
      return false;
    }
    if (section.hasTail0)
    {
      if (!FFTConvolver::applySection(fresh._tailConvolver0, section.tail0))
      {
        return false;
      }
      CopyVectorToBuffer(fresh._tailOutput0, section.tailOutput0);
      CopyVectorToBuffer(fresh._tailPrecalculated0, section.tailPrecalculated0);
    }
    if (section.hasTail)
    {
      if (!FFTConvolver::applySection(fresh._tailConvolver, section.tail))
      {
        return false;
      }
      CopyVectorToBuffer(fresh._tailOutput, section.tailOutput);
      CopyVectorToBuffer(fresh._tailPrecalculated, section.tailPrecalculated);
      CopyVectorToBuffer(fresh._backgroundProcessingInput,
                         section.backgroundProcessingInput);
    }
    if (section.hasTail0 || section.hasTail)
    {
      CopyVectorToBuffer(fresh._tailInput, section.tailInput);
    }
  }

  swapState(fresh);
  return true;
}


// Exchanges all internal resources with another instance. The SampleBuffer
// and FFTConvolver swaps move pointers only, so no audio data is copied and
// no additional buffers are allocated.
void TwoStageFFTConvolver::swapState(TwoStageFFTConvolver& other)
{
  std::swap(_headBlockSize, other._headBlockSize);
  std::swap(_tailBlockSize, other._tailBlockSize);
  _headConvolver.swapState(other._headConvolver);
  _tailConvolver0.swapState(other._tailConvolver0);
  SampleBuffer::Swap(_tailOutput0, other._tailOutput0);
  SampleBuffer::Swap(_tailPrecalculated0, other._tailPrecalculated0);
  _tailConvolver.swapState(other._tailConvolver);
  SampleBuffer::Swap(_tailOutput, other._tailOutput);
  SampleBuffer::Swap(_tailPrecalculated, other._tailPrecalculated);
  SampleBuffer::Swap(_tailInput, other._tailInput);
  std::swap(_tailInputFill, other._tailInputFill);
  std::swap(_precalculatedPos, other._precalculatedPos);
  SampleBuffer::Swap(_backgroundProcessingInput, other._backgroundProcessingInput);
  std::swap(_ir, other._ir);
}

} // End of namespace fftconvolver
