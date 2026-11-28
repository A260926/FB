// Copyright (c) 2024 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
//
// VP9 tile range extraction.
//
// ExtractVP9Tiles() walks one VP9 packet (or each child of a superframe) far
// enough to locate every coded tile without decoding entropy-coded data. The
// returned ranges are relative to the start of the packet and describe only
// tile payload bytes: four-byte big-endian tile size prefixes are excluded,
// the compressed header is excluded, and the last tile of each frame extends
// to the end of that frame's data.

#ifndef LIBWEBM_COMMON_VP9_TILES_H_
#define LIBWEBM_COMMON_VP9_TILES_H_

#include <stddef.h>

#include <vector>

namespace vp9_parser {

// The byte range of a single tile within the input packet.
struct Vp9TileRange {
  Vp9TileRange()
      : offset(0), length(0), row(0), column(0) {}
  Vp9TileRange(size_t tile_offset, size_t tile_length, int tile_row,
               int tile_column)
      : offset(tile_offset), length(tile_length), row(tile_row),
        column(tile_column) {}

  size_t offset;
  size_t length;
  int row;
  int column;
};

// The layout of one frame within the input packet.
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

  // Byte range of the frame (or superframe child) in the input packet.
  size_t offset;
  size_t length;
  // Number of bytes occupied by the uncompressed header, measured from
  // |offset| (i.e. relative to the frame start). The compressed header starts
  // at |offset| + |uncompressed_header_bytes|.
  size_t uncompressed_header_bytes;
  // Byte range of the compressed header within the input packet.
  size_t compressed_header_offset;
  size_t compressed_header_size;
  int width;
  int height;
  int tile_rows;
  int tile_columns;
  // True for show_existing_frame headers. For such frames width, height, the
  // header byte counts, and |tiles| are left zero/empty.
  bool show_existing;
  int existing_index;
  std::vector<Vp9TileRange> tiles;
};

// Extracts the per-frame tile layout for a VP9 packet. Handles a single frame,
// a superframe with a trailing superframe index, and show_existing children.
//
// On success |*out| is replaced with the parsed layouts and true is returned.
// On failure false is returned and both |*out| and |packet| are left
// unchanged. |packet| must point to |size| readable bytes. No compressed data
// is copied; only offsets, lengths, and metadata are stored.
bool ExtractVP9Tiles(const uint8_t* packet, size_t size,
                     std::vector<Vp9FrameLayout>* out);

}  // namespace vp9_parser

#endif  // LIBWEBM_COMMON_VP9_TILES_H_
