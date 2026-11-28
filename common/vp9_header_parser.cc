// Copyright (c) 2016 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#include "common/vp9_header_parser.h"

#include <stdio.h>

#include "common/vp9_bit_reader.h"
#include "common/vp9_uncompressed_header.h"

namespace vp9_parser {

bool Vp9HeaderParser::ParseUncompressedHeader(const uint8_t* frame,
                                              size_t length) {
  if (!frame || length == 0)
    return false;

  Vp9BitReader reader(frame, length);
  Vp9UncompressedHeaderInfo info;
  const Vp9HeaderParseStatus status =
      ParseVp9UncompressedHeader(&reader, &info);
  if (status == kVp9HeaderError) {
    fprintf(stderr, "Invalid VP9 uncompressed header.\n");
    return false;
  }

  frame_size_ = length;
  profile_ = info.profile;
  show_existing_frame_ = info.show_existing ? 1 : 0;
  key_ = info.key ? 1 : 0;
  altref_ = info.altref ? 1 : 0;
  intra_only_ = info.intra_only ? 1 : 0;
  error_resilient_mode_ = info.error_resilient ? 1 : 0;
  bit_depth_ = info.bit_depth;
  color_space_ = info.color_space;
  color_range_ = info.color_range;
  subsampling_x_ = info.subsampling_x;
  subsampling_y_ = info.subsampling_y;
  refresh_frame_flags_ = info.refresh_frame_flags;
  width_ = info.width;
  height_ = info.height;
  display_width_ = info.display_width;
  display_height_ = info.display_height;
  row_tiles_ = info.tile_rows;
  column_tiles_ = info.tile_columns;
  frame_parallel_mode_ = info.frame_parallel_mode ? 1 : 0;

  // A frame_size_with_refs frame is accepted as it always was: its dimensions
  // and tile layout come from reference frames that are not modeled here, so
  // those fields keep their reset values. The strict tile extractor rejects
  // this case explicitly.
  return true;
}

}  // namespace vp9_parser
