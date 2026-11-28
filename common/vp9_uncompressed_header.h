// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#ifndef LIBWEBM_COMMON_VP9_UNCOMPRESSED_HEADER_H_
#define LIBWEBM_COMMON_VP9_UNCOMPRESSED_HEADER_H_

#include <cstddef>
#include <cstdint>

#include "common/vp9_bit_reader.h"

namespace vp9_parser {

// Outcome of ParseVp9UncompressedHeader().
enum Vp9HeaderParseStatus {
  // A full header with an explicit frame size was parsed.
  kVp9HeaderOk = 0,
  // The frame is valid so far but uses frame_size_with_refs: its dimensions
  // and tile layout are inherited from reference frames that are not modeled
  // here. Partial fields up to that point are populated.
  kVp9HeaderReferenceSize,
  // The bitstream was truncated, had an invalid marker or bad padding.
  kVp9HeaderError
};

// Fields of a VP9 uncompressed header. Only structural fields are stored;
// entropy coded data is never interpreted.
struct Vp9UncompressedHeaderInfo {
  Vp9UncompressedHeaderInfo()
      : profile(0),
        show_existing(false),
        existing_index(0),
        key(false),
        altref(false),
        intra_only(false),
        error_resilient(false),
        frame_parallel_mode(0),
        bit_depth(0),
        color_space(0),
        color_range(0),
        subsampling_x(0),
        subsampling_y(0),
        refresh_frame_flags(0),
        width(0),
        height(0),
        display_width(0),
        display_height(0),
        tile_rows(0),
        tile_columns(0),
        compressed_header_size(0),
        uncompressed_header_bytes(0) {}

  int profile;
  bool show_existing;
  int existing_index;
  bool key;
  bool altref;
  bool intra_only;
  bool error_resilient;
  bool frame_parallel_mode;
  int bit_depth;
  int color_space;
  int color_range;
  int subsampling_x;
  int subsampling_y;
  int refresh_frame_flags;
  int width;
  int height;
  int display_width;
  int display_height;
  int tile_rows;
  int tile_columns;

  // Value of header_size_in_bytes: the size of the range-coded compressed
  // header that immediately follows the uncompressed header.
  std::size_t compressed_header_size;

  // Byte offset at which the uncompressed header ends and the compressed
  // header begins, relative to the start of the parsed frame.
  std::size_t uncompressed_header_bytes;
};

// Parses one complete VP9 uncompressed header from |reader|, including the
// full color, reference, loop filter, quantization and segmentation syntax,
// and fills |info|. Returns kVp9HeaderOk on success. A frame_size_with_refs
// inter frame returns kVp9HeaderReferenceSize with the fields parsed up to
// that point. Any truncated or invalid field returns kVp9HeaderError.
Vp9HeaderParseStatus ParseVp9UncompressedHeader(
    Vp9BitReader* reader, Vp9UncompressedHeaderInfo* info);

}  // namespace vp9_parser

#endif  // LIBWEBM_COMMON_VP9_UNCOMPRESSED_HEADER_H_
