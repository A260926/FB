// Copyright (c) 2016 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#include "common/vp9_header_parser.h"

#include "common/vp9_tiles.h"

namespace vp9_parser {

bool Vp9HeaderParser::SetFrame(const uint8_t* frame, size_t length) {
  if (!frame || length == 0) return false;

  frame_ = frame;
  frame_size_ = length;
  bit_offset_ = 0;
  profile_ = -1;
  show_existing_frame_ = 0;
  key_ = 0;
  altref_ = 0;
  error_resilient_mode_ = 0;
  intra_only_ = 0;
  reset_frame_context_ = 0;
  color_space_ = 0;
  color_range_ = 0;
  subsampling_x_ = 0;
  subsampling_y_ = 0;
  refresh_frame_flags_ = 0;
  return true;
}

bool Vp9HeaderParser::ParseUncompressedHeader(const uint8_t* frame,
                                              size_t length) {
  if (!SetFrame(frame, length)) return false;

  // Share the uncompressed-header syntax, bit reader, and bounds checks with
  // ExtractVP9Tiles(). Non-strict mode preserves this parser's historical
  // behavior: truncated reads yield zero bits and a frame whose size comes
  // from frame_size_with_refs() is accepted without a reference slot model.
  internal::Vp9HeaderBits bits;
  if (!internal::ParseVp9UncompressedHeader(frame, length,
                                            /*strict=*/false, &bits))
    return false;

  profile_ = bits.profile;
  show_existing_frame_ = bits.show_existing ? 1 : 0;
  bit_offset_ = bits.uncompressed_header_bytes * 8;
  if (bits.show_existing) return true;

  key_ = bits.key ? 1 : 0;
  altref_ = bits.show_frame ? 0 : 1;
  error_resilient_mode_ = bits.error_resilient ? 1 : 0;
  intra_only_ = bits.intra_only ? 1 : 0;
  reset_frame_context_ = bits.reset_frame_context;

  if (!bits.key) refresh_frame_flags_ = bits.refresh_frame_flags;

  if (bits.size_inherited) {
    // frame_size_with_refs() selected a reference slot. Historically this
    // parser stopped here and left the size fields from the previous frame
    // untouched, and it keeps doing so.
    return true;
  }

  width_ = bits.width;
  height_ = bits.height;
  display_width_ = bits.display_width;
  display_height_ = bits.display_height;

  // The historical parser only decoded color config for key and intra-only
  // frames, and only decoded frame_parallel_mode and tile_info for key frames.
  // Keep that observable behavior; ExtractVP9Tiles() itself always consumes
  // the full header.
  if (bits.key || bits.intra_only) {
    bit_depth_ = bits.bit_depth;
    color_space_ = bits.color_space;
    color_range_ = bits.color_range;
    subsampling_x_ = bits.subsampling_x;
    subsampling_y_ = bits.subsampling_y;
  }
  if (bits.key) {
    frame_parallel_mode_ = bits.frame_parallel_mode ? 1 : 0;
    row_tiles_ = bits.row_tiles;
    column_tiles_ = bits.column_tiles;
  }
  return true;
}

}  // namespace vp9_parser
