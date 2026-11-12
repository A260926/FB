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

#ifndef _FFTCONVOLVER_SNAPSHOT_H
#define _FFTCONVOLVER_SNAPSHOT_H

// Internal, self-contained serialization helpers for FFTConvolver and
// TwoStageFFTConvolver state snapshots.
//
// The wire format is defined independently of any in-memory C++ layout:
//
//   - All multi-byte integers are unsigned little-endian.
//   - All samples are IEEE-754 binary32 floats (the project's Sample type),
//     stored by their exact 32-bit bit pattern in little-endian order.
//   - Nothing is dumped from a C++ struct: there are no pointers, no padding
//     bytes and no implementation-dependent sizes on the wire.
//
// Every length field is validated against the number of bytes actually
// present before any memory is allocated, so a corrupt or hostile blob can
// never cause out-of-bounds reads or allocations larger than the blob itself.

#include "Utilities.h"

#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>


namespace fftconvolver
{
namespace snapshot
{

// File identification (4 bytes each) - the two convolver classes use
// different magic values so a state can never be imported into the wrong
// class. Internal linkage is fine: only the byte pattern is on the wire.
static const char MAGIC_FFT[4] = { 'F', 'C', 'V', '1' }; // FFTConvolver
static const char MAGIC_TWO[4] = { 'F', '2', 'S', '1' }; // TwoStageFFTConvolver

constexpr uint8_t FORMAT_VERSION = 1u;

constexpr uint8_t CLASS_FFT = 1u;
constexpr uint8_t CLASS_TWO = 2u;

// Section tags
constexpr uint8_t TAG_FFT_SECTION = 1u;
constexpr uint8_t TAG_TWO_BODY    = 2u;
constexpr uint8_t TAG_HEAD        = 3u;
constexpr uint8_t TAG_TAIL0       = 4u;
constexpr uint8_t TAG_TAIL        = 5u;

// File header: magic(4) + version(1) + class(1) + reserved(2) + payloadLen(4)
constexpr size_t HEADER_SIZE = 12u;

// Body flags
constexpr uint32_t FLAG_EMPTY = 1u;
constexpr uint32_t FLAG_TAIL0 = 2u;
constexpr uint32_t FLAG_TAIL  = 4u;

// Same threshold as the IR trimming in the convolvers' init()
constexpr float IR_TRIM_THRESHOLD = 0.000001f;


inline bool IsPowerOf2(uint32_t val)
{
  return val > 0u && (val & (val - 1u)) == 0u;
}


/**
 * @class Writer
 * @brief Grows a byte buffer using only explicit, portable encodings
 */
class Writer
{
public:
  Writer() : _data()
  {
  }

  void u8(uint8_t val)
  {
    _data.push_back(static_cast<char>(val));
  }

  void u16(uint16_t val)
  {
    _data.push_back(static_cast<char>(val & 0xFFu));
    _data.push_back(static_cast<char>((val >> 8) & 0xFFu));
  }

  void u32(uint32_t val)
  {
    _data.push_back(static_cast<char>(val & 0xFFu));
    _data.push_back(static_cast<char>((val >> 8) & 0xFFu));
    _data.push_back(static_cast<char>((val >> 16) & 0xFFu));
    _data.push_back(static_cast<char>((val >> 24) & 0xFFu));
  }

  void f32(float val)
  {
    uint32_t bits = 0;
    ::memcpy(&bits, &val, sizeof(bits));
    u32(bits);
  }

  void bytes(const void* data, size_t len)
  {
    if (len > 0)
    {
      const char* p = static_cast<const char*>(data);
      _data.insert(_data.end(), p, p + len);
    }
  }

  void append(const Writer& other)
  {
    bytes(other._data.data(), other._data.size());
  }

  size_t size() const
  {
    return _data.size();
  }

  const uint8_t* data() const
  {
    return reinterpret_cast<const uint8_t*>(_data.data());
  }

  std::vector<uint8_t>& buffer()
  {
    return _data;
  }

private:
  std::vector<uint8_t> _data;
};


/**
 * @class Reader
 * @brief Bounds-checked cursor over an immutable byte range
 *
 * Every accessor fails (returns false) instead of reading past the end of
 * the supplied buffer.
 */
class Reader
{
public:
  Reader(const uint8_t* data, size_t size) :
    _data(data),
    _size(size),
    _pos(0)
  {
  }

  size_t remaining() const
  {
    return _size - _pos;
  }

  bool fullyConsumed() const
  {
    return _pos == _size;
  }

  bool u8(uint8_t& val)
  {
    if (remaining() < 1u)
    {
      return false;
    }
    val = _data[_pos];
    ++_pos;
    return true;
  }

  bool u16(uint16_t& val)
  {
    if (remaining() < 2u)
    {
      return false;
    }
    val = static_cast<uint16_t>(_data[_pos])
        | (static_cast<uint16_t>(_data[_pos + 1]) << 8);
    _pos += 2;
    return true;
  }

  bool u32(uint32_t& val)
  {
    if (remaining() < 4u)
    {
      return false;
    }
    val = static_cast<uint32_t>(_data[_pos])
        | (static_cast<uint32_t>(_data[_pos + 1]) << 8)
        | (static_cast<uint32_t>(_data[_pos + 2]) << 16)
        | (static_cast<uint32_t>(_data[_pos + 3]) << 24);
    _pos += 4;
    return true;
  }

  bool f32(float& val)
  {
    uint32_t bits = 0;
    if (!u32(bits))
    {
      return false;
    }
    ::memcpy(&val, &bits, sizeof(val));
    return true;
  }

  bool bytes(void* dest, size_t len)
  {
    if (len > remaining())
    {
      return false;
    }
    if (len > 0)
    {
      ::memcpy(dest, _data + _pos, len);
      _pos += len;
    }
    return true;
  }

  // Creates a sub-reader of exactly [len] bytes. The length is checked
  // before anything (including allocations on the caller side) happens.
  bool subReader(uint32_t len, Reader& out)
  {
    if (static_cast<size_t>(len) > remaining())
    {
      return false;
    }
    out = Reader(_data + _pos, len);
    _pos += len;
    return true;
  }

private:
  const uint8_t* _data;
  size_t _size;
  size_t _pos;
};


inline bool AllFinite(const std::vector<float>& values)
{
  for (size_t i = 0; i < values.size(); ++i)
  {
    if (!std::isfinite(values[i]))
    {
      return false;
    }
  }
  return true;
}


// Writes [count][count floats]. Returns false if the count does not fit
// the uint32 length field.
inline bool WriteFloats(Writer& writer, const float* values, size_t count)
{
  if (count > UINT32_MAX)
  {
    return false;
  }
  writer.u32(static_cast<uint32_t>(count));
  for (size_t i = 0; i < count; ++i)
  {
    writer.f32(values[i]);
  }
  return true;
}


inline bool WriteFloats(Writer& writer, const std::vector<float>& values)
{
  return WriteFloats(writer, values.empty() ? 0 : &values[0], values.size());
}


// Reads a [count][floats] array and requires an exact expected length.
// The byte count is verified against the remaining input before the
// destination storage is allocated, so allocation is always bounded by the
// input blob.
inline bool ReadFloatsExact(Reader& reader, std::vector<float>& values, size_t expected)
{
  uint32_t count = 0;
  if (!reader.u32(count))
  {
    return false;
  }
  if (static_cast<size_t>(count) != expected)
  {
    return false;
  }
  if (expected > reader.remaining() / sizeof(float))
  {
    return false;
  }
  values.resize(expected);
  for (size_t i = 0; i < expected; ++i)
  {
    if (!reader.f32(values[i]))
    {
      return false;
    }
  }
  return true;
}


// Wraps a section body with [tag][body length][body].
inline bool WriteSection(Writer& file, uint8_t tag, const Writer& body)
{
  if (body.size() > UINT32_MAX)
  {
    return false;
  }
  file.u8(tag);
  file.u32(static_cast<uint32_t>(body.size()));
  file.append(body);
  return true;
}


// Reads a section header and provides a bounded sub-reader for its body.
inline bool ReadSectionHeader(Reader& file, uint8_t expectedTag, Reader& body)
{
  uint8_t tag = 0;
  uint32_t len = 0;
  if (!file.u8(tag) || !file.u32(len))
  {
    return false;
  }
  if (tag != expectedTag)
  {
    return false;
  }
  return file.subReader(len, body);
}


inline bool WriteFileHeader(Writer& file, const char magic[4], uint8_t classTag, const Writer& payload)
{
  if (payload.size() > static_cast<size_t>(UINT32_MAX) - HEADER_SIZE)
  {
    return false;
  }
  file.bytes(magic, 4);
  file.u8(FORMAT_VERSION);
  file.u8(classTag);
  file.u16(0); // Reserved, must be zero
  file.u32(static_cast<uint32_t>(payload.size()));
  file.append(payload);
  return true;
}


inline bool ReadFileHeader(Reader& file, const char expectedMagic[4], uint8_t expectedClass)
{
  if (file.remaining() < HEADER_SIZE)
  {
    return false;
  }
  char magic[4];
  if (!file.bytes(magic, 4) || ::memcmp(magic, expectedMagic, 4) != 0)
  {
    return false;
  }
  uint8_t version = 0;
  uint8_t classTag = 0;
  uint16_t reserved = 0;
  uint32_t payloadLen = 0;
  if (!file.u8(version) || !file.u8(classTag) ||
      !file.u16(reserved) || !file.u32(payloadLen))
  {
    return false;
  }
  if (version != FORMAT_VERSION || classTag != expectedClass || reserved != 0)
  {
    return false;
  }
  // The declared payload length must match the actual bytes present exactly.
  return payloadLen == file.remaining();
}


/**
 * @struct FFTSection
 * @brief Portable representation of one FFTConvolver's complete state
 */
struct FFTSection
{
  bool empty;
  uint32_t blockSize;
  uint32_t segCount;
  uint32_t current;
  uint32_t inputFill;
  std::vector<float> ir;             // Trimmed time-domain impulse response
  std::vector<float> segments;       // segCount * 2 * complexSize floats
  std::vector<float> preMultiplied;  // 2 * complexSize floats
  std::vector<float> overlap;        // blockSize floats
  std::vector<float> inputBuffer;    // blockSize floats

  FFTSection() :
    empty(true),
    blockSize(0),
    segCount(0),
    current(0),
    inputFill(0),
    ir(),
    segments(),
    preMultiplied(),
    overlap(),
    inputBuffer()
  {
  }
};


inline void WriteFFTSectionBody(Writer& body, const FFTSection& section)
{
  body.u32(section.empty ? FLAG_EMPTY : 0u);
  if (section.empty)
  {
    return;
  }

  body.u32(section.blockSize);
  body.u32(section.segCount);
  body.u32(section.current);
  body.u32(section.inputFill);
  WriteFloats(body, section.ir);
  WriteFloats(body, section.segments);
  WriteFloats(body, section.preMultiplied);
  WriteFloats(body, section.overlap);
  WriteFloats(body, section.inputBuffer);
}


inline bool WriteFFTSection(Writer& file, uint8_t tag, const FFTSection& section)
{
  Writer body;
  WriteFFTSectionBody(body, section);
  return WriteSection(file, tag, body);
}


// Parses and fully validates one FFTConvolver section. All bounds are
// checked against the section body before storage is allocated.
inline bool ReadFFTSection(Reader& file, uint8_t expectedTag, FFTSection& out)
{
  Reader body(0, 0);
  if (!ReadSectionHeader(file, expectedTag, body))
  {
    return false;
  }

  uint32_t flags = 0;
  if (!body.u32(flags))
  {
    return false;
  }

  if (flags & FLAG_EMPTY)
  {
    if (flags != FLAG_EMPTY || !body.fullyConsumed())
    {
      return false;
    }
    out = FFTSection();
    return true;
  }

  if (flags != 0u)
  {
    return false;
  }

  FFTSection section;
  section.empty = false;

  if (!body.u32(section.blockSize) ||
      !body.u32(section.segCount) ||
      !body.u32(section.current) ||
      !body.u32(section.inputFill))
  {
    return false;
  }

  if (!IsPowerOf2(section.blockSize) || section.segCount == 0u)
  {
    return false;
  }
  if (section.current >= section.segCount)
  {
    return false;
  }
  // A full block is always consumed immediately by process(), so the
  // partial-block fill is strictly below the block size.
  if (section.inputFill >= section.blockSize)
  {
    return false;
  }

  const size_t complexSize = static_cast<size_t>(section.blockSize) + 1u;
  const size_t blockSize = section.blockSize;
  const size_t segCount = section.segCount;

  // Impulse response: (segCount-1) * blockSize < irLen <= segCount * blockSize.
  // All multiplications are guarded so no size_t wrap is possible even on
  // 32-bit builds.
  if (segCount > SIZE_MAX / blockSize)
  {
    return false;
  }
  const size_t maxIrLen = segCount * blockSize;
  const size_t minIrLenExclusive = (segCount - 1u) * blockSize;

  uint32_t irCount = 0;
  if (!body.u32(irCount) || irCount == 0u)
  {
    return false;
  }
  const size_t irLen = irCount;
  if (irLen > maxIrLen || irLen <= minIrLenExclusive)
  {
    return false;
  }
  if (irLen > body.remaining() / sizeof(float))
  {
    return false;
  }
  section.ir.resize(irLen);
  for (size_t i = 0; i < irLen; ++i)
  {
    if (!body.f32(section.ir[i]))
    {
      return false;
    }
  }
  // The convolvers trim trailing |x| < 1e-6 on init, so a valid snapshot
  // can never end in such a sample.
  if (::fabs(section.ir.back()) < IR_TRIM_THRESHOLD)
  {
    return false;
  }

  // Frequency-domain input history: segCount SplitComplex buffers, each
  // with separate real and imaginary arrays. Guard every multiplication
  // against size_t overflow.
  if (segCount > SIZE_MAX / 2u ||
      segCount * 2u > SIZE_MAX / complexSize)
  {
    return false;
  }
  const size_t segmentFloats = segCount * 2u * complexSize;
  const size_t preMultipliedFloats = 2u * complexSize;
  if (segmentFloats > body.remaining() / sizeof(float) ||
      !ReadFloatsExact(body, section.segments, segmentFloats) ||
      !ReadFloatsExact(body, section.preMultiplied, preMultipliedFloats) ||
      !ReadFloatsExact(body, section.overlap, blockSize) ||
      !ReadFloatsExact(body, section.inputBuffer, blockSize))
  {
    return false;
  }

  if (!body.fullyConsumed())
  {
    return false;
  }

  if (!AllFinite(section.ir) ||
      !AllFinite(section.segments) ||
      !AllFinite(section.preMultiplied) ||
      !AllFinite(section.overlap) ||
      !AllFinite(section.inputBuffer))
  {
    return false;
  }

  out = std::move(section);
  return true;
}


/**
 * @struct TwoStageSection
 * @brief Portable representation of one TwoStageFFTConvolver state
 */
struct TwoStageSection
{
  bool empty;
  uint32_t headBlockSize;
  uint32_t tailBlockSize;
  uint32_t tailInputFill;
  uint32_t precalculatedPos;
  bool hasTail0;
  bool hasTail;
  std::vector<float> ir;
  std::vector<float> tailOutput0;
  std::vector<float> tailPrecalculated0;
  std::vector<float> tailOutput;
  std::vector<float> tailPrecalculated;
  std::vector<float> tailInput;
  std::vector<float> backgroundProcessingInput;
  FFTSection head;
  FFTSection tail0;
  FFTSection tail;

  TwoStageSection() :
    empty(true),
    headBlockSize(0),
    tailBlockSize(0),
    tailInputFill(0),
    precalculatedPos(0),
    hasTail0(false),
    hasTail(false),
    ir(),
    tailOutput0(),
    tailPrecalculated0(),
    tailOutput(),
    tailPrecalculated(),
    tailInput(),
    backgroundProcessingInput(),
    head(),
    tail0(),
    tail()
  {
  }
};


inline bool WriteTwoStageBody(Writer& file, const TwoStageSection& section)
{
  Writer body;
  if (section.empty)
  {
    body.u32(FLAG_EMPTY);
    return WriteSection(file, TAG_TWO_BODY, body);
  }

  uint32_t flags = 0u;
  if (section.hasTail0)
  {
    flags |= FLAG_TAIL0;
  }
  if (section.hasTail)
  {
    flags |= FLAG_TAIL;
  }
  body.u32(flags);
  body.u32(section.headBlockSize);
  body.u32(section.tailBlockSize);
  body.u32(section.tailInputFill);
  body.u32(section.precalculatedPos);
  WriteFloats(body, section.ir);
  WriteFloats(body, section.tailOutput0);
  WriteFloats(body, section.tailPrecalculated0);
  WriteFloats(body, section.tailOutput);
  WriteFloats(body, section.tailPrecalculated);
  WriteFloats(body, section.tailInput);
  WriteFloats(body, section.backgroundProcessingInput);
  return WriteSection(file, TAG_TWO_BODY, body);
}


// Parses the TwoStage body section (everything except the nested convolver
// sections) and validates all internal size relationships.
inline bool ReadTwoStageBody(Reader& file, TwoStageSection& out)
{
  Reader body(0, 0);
  if (!ReadSectionHeader(file, TAG_TWO_BODY, body))
  {
    return false;
  }

  uint32_t flags = 0;
  if (!body.u32(flags))
  {
    return false;
  }

  if (flags & FLAG_EMPTY)
  {
    if (flags != FLAG_EMPTY || !body.fullyConsumed())
    {
      return false;
    }
    out = TwoStageSection();
    return true;
  }

  TwoStageSection section;
  section.empty = false;
  section.hasTail0 = (flags & FLAG_TAIL0) != 0;
  section.hasTail = (flags & FLAG_TAIL) != 0;
  if (flags & ~(FLAG_TAIL0 | FLAG_TAIL))
  {
    return false;
  }

  if (!body.u32(section.headBlockSize) ||
      !body.u32(section.tailBlockSize) ||
      !body.u32(section.tailInputFill) ||
      !body.u32(section.precalculatedPos))
  {
    return false;
  }

  if (!IsPowerOf2(section.headBlockSize) ||
      !IsPowerOf2(section.tailBlockSize) ||
      section.headBlockSize > section.tailBlockSize)
  {
    return false;
  }

  // Full trimmed impulse response.
  uint32_t irCount = 0;
  if (!body.u32(irCount) || irCount == 0u)
  {
    return false;
  }
  if (static_cast<size_t>(irCount) > body.remaining() / sizeof(float))
  {
    return false;
  }
  section.ir.resize(irCount);
  for (uint32_t i = 0; i < irCount; ++i)
  {
    if (!body.f32(section.ir[i]))
    {
      return false;
    }
  }
  if (::fabs(section.ir.back()) < IR_TRIM_THRESHOLD)
  {
    return false;
  }

  const size_t irLen = section.ir.size();
  const size_t tailBlock = section.tailBlockSize;
  if (tailBlock > SIZE_MAX / 2u)
  {
    return false;
  }
  const size_t twoTailBlocks = 2u * tailBlock;

  // Presence flags must be consistent with the impulse response length.
  if (section.hasTail0 != (irLen > tailBlock) ||
      section.hasTail != (irLen > twoTailBlocks))
  {
    return false;
  }

  if (!ReadFloatsExact(body, section.tailOutput0, section.hasTail0 ? tailBlock : 0u) ||
      !ReadFloatsExact(body, section.tailPrecalculated0, section.hasTail0 ? tailBlock : 0u) ||
      !ReadFloatsExact(body, section.tailOutput, section.hasTail ? tailBlock : 0u) ||
      !ReadFloatsExact(body, section.tailPrecalculated, section.hasTail ? tailBlock : 0u) ||
      !ReadFloatsExact(body, section.tailInput,
                       (section.hasTail0 || section.hasTail) ? tailBlock : 0u) ||
      !ReadFloatsExact(body, section.backgroundProcessingInput,
                       section.hasTail ? tailBlock : 0u))
  {
    return false;
  }

  // The two positions advance and reset together inside process(), so they
  // are always equal; both stay strictly below the tail block size.
  if (section.hasTail0 || section.hasTail)
  {
    if (section.tailInputFill >= tailBlock ||
        section.precalculatedPos >= tailBlock ||
        section.tailInputFill != section.precalculatedPos)
    {
      return false;
    }
  }
  else if (section.tailInputFill != 0u || section.precalculatedPos != 0u)
  {
    return false;
  }

  if (!body.fullyConsumed())
  {
    return false;
  }

  if (!AllFinite(section.ir) ||
      !AllFinite(section.tailOutput0) ||
      !AllFinite(section.tailPrecalculated0) ||
      !AllFinite(section.tailOutput) ||
      !AllFinite(section.tailPrecalculated) ||
      !AllFinite(section.tailInput) ||
      !AllFinite(section.backgroundProcessingInput))
  {
    return false;
  }

  out = std::move(section);
  return true;
}


} // End of namespace snapshot
} // End of namespace fftconvolver

#endif // Header guard
