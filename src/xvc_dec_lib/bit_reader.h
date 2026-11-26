/******************************************************************************
* Copyright (C) 2018, Divideon.
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*
* This library is also available under a commercial license.
* Please visit https://xvc.io/license/ for more information.
******************************************************************************/

#ifndef XVC_DEC_LIB_BIT_READER_H_
#define XVC_DEC_LIB_BIT_READER_H_

#include <fstream>
#include <vector>

#include "xvc_common_lib/common.h"

namespace xvc {

class BitReader {
public:
  BitReader(const uint8_t *buffer, size_t length)
    : buffer_(buffer), length_(length) {
  }

  size_t GetPosition() const;
  int ReadBit();
  uint32_t ReadBits(int num_bits);
  void SkipBits();
  uint8_t ReadByte();
  void ReadBytes(uint8_t *bytes, size_t len);
  void Rewind(int num_bits);

  // When enabled every read past the end of the buffer throws
  // std::runtime_error instead of relying on asserts/out-of-bounds access.
  // Used for per CTU row substream readers with tightly bounded storage.
  void SetSafeBounds(bool safe) { safe_bounds_ = safe; }
  bool GetSafeBounds() const { return safe_bounds_; }
  const uint8_t *GetBuffer() const { return buffer_; }
  size_t GetLength() const { return length_; }
  // Byte aligns and moves the read position to an absolute byte offset.
  void SeekToByte(size_t byte_pos) {
    bit_mask_ = 0x80;
    consumed_ = byte_pos <= length_ ? byte_pos : length_;
  }

private:
  void Refill();

  int bit_mask_ = 0x80;
  size_t consumed_ = 0;
  const uint8_t *buffer_ = nullptr;
  size_t length_ = 0;
  bool safe_bounds_ = false;
};

}   // namespace xvc

#endif    // XVC_DEC_LIB_BIT_READER_H_
