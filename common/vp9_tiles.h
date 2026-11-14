// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#ifndef LIBWEBM_COMMON_VP9_TILES_H_
#define LIBWEBM_COMMON_VP9_TILES_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace vp9_parser {

// Byte range of one VP9 tile payload. All offsets are relative to the start of
// the packet passed to ExtractVP9Tiles(). |length| counts payload bytes only:
// the four byte big-endian length prefix that precedes every tile except the
// last one is not part of the range.
struct Vp9TileRange {
  Vp9TileRange() : offset(0), length(0), row(0), column(0) {}

  size_t offset;
  size_t length;
  int row;
  int column;
};

// Layout of a single frame in a VP9 packet. A packet either contains one frame
// or a superframe of up to eight subframes; one Vp9FrameLayout is produced per
// subframe. Every offset is relative to the start of the packet.
struct Vp9FrameLayout {
  Vp9FrameLayout()
      : offset(0),
        length(0),
        uncompressed_header_bytes(0),
        compressed_header_offset(0),
        compressed_header_size(0),
        width(0),
        height(0),
        tile_rows(0),
        tile_columns(0),
        show_existing(false),
        existing_index(0) {}

  size_t offset;
  size_t length;
  size_t uncompressed_header_bytes;
  size_t compressed_header_offset;
  size_t compressed_header_size;
  int width;
  int height;
  int tile_rows;
  int tile_columns;
  bool show_existing;
  int existing_index;
  std::vector<Vp9TileRange> tiles;
};

// Parses the uncompressed header, compressed header range, and tile ranges of
// every frame in |packet|. No compressed data is interpreted or copied. On
// success |out| is replaced with one layout per frame. On failure false is
// returned and |out| is left untouched.
bool ExtractVP9Tiles(const uint8_t* packet, size_t size,
                     std::vector<Vp9FrameLayout>* out);

// Shared uncompressed-header syntax used by both Vp9HeaderParser and
// ExtractVP9Tiles so that the segmentation and tile grammar and its bounds
// checks exist in exactly one place. Not part of the public API.
namespace internal {

struct Vp9HeaderBits {
  Vp9HeaderBits()
      : profile(-1),
        show_existing(false),
        existing_index(0),
        key(false),
        show_frame(false),
        error_resilient(false),
        intra_only(false),
        reset_frame_context(0),
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
        size_inherited(false),
        frame_parallel_mode(false),
        parsed_tile_info(false),
        tile_rows_log2(0),
        tile_columns_log2(0),
        row_tiles(0),
        column_tiles(0),
        uncompressed_header_bytes(0),
        compressed_header_size(0) {}

  int profile;
  bool show_existing;
  int existing_index;
  bool key;
  bool show_frame;
  bool error_resilient;
  bool intra_only;
  int reset_frame_context;
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
  // frame_size_with_refs() picked a reference frame instead of carrying an
  // explicit size. Only the size fields are left unset in that case.
  bool size_inherited;
  bool frame_parallel_mode;
  bool parsed_tile_info;
  int tile_rows_log2;
  int tile_columns_log2;
  int row_tiles;
  int column_tiles;
  size_t uncompressed_header_bytes;
  size_t compressed_header_size;
};

// Parses the VP9 uncompressed header. When |strict| is true, truncation and
// non-zero alignment padding are failures and the compressed header length is
// included. When false, the reader matches Vp9HeaderParser's historical
// behavior: reads past the end yield zero bits and an inherited frame size
// simply ends the parse successfully.
bool ParseVp9UncompressedHeader(const uint8_t* data, size_t size, bool strict,
                                Vp9HeaderBits* bits);

}  // namespace internal

}  // namespace vp9_parser

#endif  // LIBWEBM_COMMON_VP9_TILES_H_
