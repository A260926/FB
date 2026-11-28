// Copyright (c) 2016 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#include "common/vp9_header_parser.h"

#include <stddef.h>
#include <stdint.h>

#include "common/vp9_syntax.h"

namespace vp9_parser {

bool Vp9HeaderParser::SetFrame(const uint8_t* frame, size_t length) {
  if (frame == NULL || length == 0)
    return false;

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
  if (!SetFrame(frame, length))
    return false;

  // The parser must not carry state from a previous frame into this one, but
  // it must also not clobber fields that the header does not mention. Seed the
  // fields that SetFrame() historically reset with their reset values, seed
  // the fields that historically persisted with the current member values,
  // then merge only the fields the parsed header actually contained.
  internal::Vp9HeaderFields fields;
  fields.profile = -1;
  fields.show_existing = 0;
  fields.key = 0;
  fields.show_frame = 0;
  fields.error_resilient_mode = 0;
  fields.intra_only = 0;
  fields.reset_frame_context = 0;
  fields.color_space = 0;
  fields.color_range = 0;
  fields.subsampling_x = 0;
  fields.subsampling_y = 0;
  fields.refresh_frame_flags = 0;
  fields.bit_depth = bit_depth_;
  fields.width = width_;
  fields.height = height_;
  fields.display_width = display_width_;
  fields.display_height = display_height_;
  fields.row_tiles = row_tiles_;
  fields.column_tiles = column_tiles_;
  fields.frame_parallel_mode = frame_parallel_mode_;

  internal::Vp9BitReader reader(frame, length);
  if (!internal::ParseUncompressedHeaderInternal(
          &reader, internal::kParseLegacy, &fields)) {
    return false;
  }

  profile_ = fields.profile;
  show_existing_frame_ = fields.show_existing;
  key_ = fields.key;
  // Historical behavior: show_existing frames do not report altref; other
  // frames report the inverse of show_frame.
  altref_ = fields.show_existing ? 0 : !fields.show_frame;
  error_resilient_mode_ = fields.error_resilient_mode;
  intra_only_ = fields.intra_only;
  reset_frame_context_ = fields.reset_frame_context;
  // SetFrame() historically reset these fields on every parse, so they take
  // the (possibly zero) values from |fields| even when the header omits them.
  color_space_ = fields.color_space;
  color_range_ = fields.color_range;
  subsampling_x_ = fields.subsampling_x;
  subsampling_y_ = fields.subsampling_y;
  refresh_frame_flags_ = fields.refresh_frame_flags;
  // bit_depth_ historically persisted unless a color config was parsed.
  if (fields.has_color_config)
    bit_depth_ = fields.bit_depth;
  if (fields.has_frame_size) {
    width_ = fields.width;
    height_ = fields.height;
  }
  if (fields.has_render_size) {
    display_width_ = fields.display_width;
    display_height_ = fields.display_height;
  }
  if (fields.has_frame_parallel_mode)
    frame_parallel_mode_ = fields.frame_parallel_mode;
  if (fields.has_tile_info) {
    row_tiles_ = fields.row_tiles;
    column_tiles_ = fields.column_tiles;
  }
  bit_offset_ = reader.bit_position();
  return true;
}

}  // namespace vp9_parser
