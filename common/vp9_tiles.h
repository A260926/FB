// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#ifndef LIBWEBM_COMMON_VP9_TILES_H_
#define LIBWEBM_COMMON_VP9_TILES_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vp9_parser {

// Byte range of one tile inside the packet passed to ExtractVP9Tiles(). The
// range covers the tile payload only: the four byte big-endian size prefix of
// every non-final tile is excluded.
struct Vp9TileRange {
  Vp9TileRange() : offset(0), length(0), row(0), column(0) {}
  Vp9TileRange(std::size_t off, std::size_t len, int tile_row, int tile_column)
      : offset(off), length(len), row(tile_row), column(tile_column) {}

  std::size_t offset;
  std::size_t length;
  int row;
  int column;
};

// Structural layout of one VP9 (sub)frame. Every offset is relative to the
// start of the packet handed to ExtractVP9Tiles(), and every length covers the
// corresponding payload only. No compressed bytes are copied.
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

  // Range of the whole sub-frame within the input packet.
  std::size_t offset;
  std::size_t length;

  // Size in bytes of the uncompressed header (including its alignment
  // padding). The compressed header starts at |offset| +
  // |uncompressed_header_bytes|.
  std::size_t uncompressed_header_bytes;
  std::size_t compressed_header_offset;
  std::size_t compressed_header_size;

  int width;
  int height;
  int tile_rows;
  int tile_columns;

  // Show-existing frames carry only a frame reference: width, height and every
  // range beyond the frame itself stay zero and |tiles| is empty.
  bool show_existing;
  int existing_index;

  std::vector<Vp9TileRange> tiles;
};

// Extracts the tile byte ranges of every VP9 frame in |packet| without
// decoding any pixels. A packet without a superframe index is treated as a
// single frame; a packet whose trailing byte carries the superframe marker
// must have a fully valid index or the call fails. On success |out| is
// replaced with one entry per (sub)frame and true is returned; on failure
// false is returned and both |out| and the input buffer are left unchanged.
bool ExtractVP9Tiles(const std::uint8_t* packet, std::size_t size,
                     std::vector<Vp9FrameLayout>* out);

}  // namespace vp9_parser

#endif  // LIBWEBM_COMMON_VP9_TILES_H_
