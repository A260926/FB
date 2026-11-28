// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#ifndef LIBWEBM_COMMON_VP9_BIT_READER_H_
#define LIBWEBM_COMMON_VP9_BIT_READER_H_

#include <cstddef>
#include <cstdint>

namespace vp9_parser {

// MSB-first bit reader over a fixed byte buffer. Every read is bounds checked:
// once the buffer is exhausted (or an invalid alignment padding bit is seen)
// the reader latches an error and all subsequent reads return zero. Callers
// must check error() before using the parsed values. The reader never owns the
// buffer.
class Vp9BitReader {
 public:
  Vp9BitReader(const std::uint8_t* data, std::size_t size)
      : data_(data), size_(size), bit_offset_(0), error_(data == nullptr) {}

  bool error() const { return error_; }
  std::size_t bit_offset() const { return bit_offset_; }
  std::size_t bytes_consumed() const { return (bit_offset_ + 7u) >> 3; }

  // Reads one bit, MSB first within each byte.
  bool ReadBit() {
    if (error_)
      return false;
    const std::size_t byte_offset = bit_offset_ >> 3;
    if (byte_offset >= size_) {
      error_ = true;
      return false;
    }
    const int shift = 7 - static_cast<int>(bit_offset_ & 7u);
    const bool bit = ((data_[byte_offset] >> shift) & 1) != 0;
    ++bit_offset_;
    return bit;
  }

  // Reads |bits| bits as an unsigned literal, |bits| must be positive and at
  // most 31.
  int ReadLiteral(int bits) {
    int value = 0;
    for (int i = bits - 1; i >= 0; --i)
      value |= static_cast<int>(ReadBit()) << i;
    return value;
  }

  // Reads a VP9 signed literal: |magnitude_bits| value bits followed by one
  // sign bit (1 means negative). Returns false when the read is truncated.
  bool ReadSigned(int magnitude_bits, int* value) {
    const int magnitude = ReadLiteral(magnitude_bits);
    const bool negative = ReadBit();
    if (error_)
      return false;
    *value = negative ? -magnitude : magnitude;
    return true;
  }

  // Consumes the zero padding bits that terminate the uncompressed header and
  // bring the reader back to a byte boundary. A nonzero padding bit is an
  // error.
  bool ReadByteAlignment() {
    while ((bit_offset_ & 7u) != 0 && !error_) {
      if (ReadBit()) {
        error_ = true;
        return false;
      }
    }
    return !error_;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t bit_offset_;
  bool error_;
};

}  // namespace vp9_parser

#endif  // LIBWEBM_COMMON_VP9_BIT_READER_H_
