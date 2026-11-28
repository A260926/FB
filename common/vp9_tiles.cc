// Copyright (c) 2024 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#include "common/vp9_tiles.h"

#include <stddef.h>
#include <stdint.h>

#include <limits>
#include <vector>

#include "common/vp9_syntax.h"

namespace vp9_parser {
namespace {

const uint8_t kSuperframeMarkerMask = 0xe0;
const uint8_t kSuperframeMarker = 0xc0;
const int kMaxSuperframeFrames = 8;
const int kMaxSuperframeSizeBytes = 4;
const size_t kTileLengthPrefixBytes = 4;

// A frame (or superframe child) located within the input packet.
struct FrameSpan {
  FrameSpan() : offset(0), length(0) {}
  size_t offset;
  size_t length;
};

// Adds two size_t values, reporting overflow. Every offset computation in this
// file goes through this helper so malformed headers cannot wrap around.
bool AddOverflows(size_t a, size_t b, size_t* result) {
  if (a > std::numeric_limits<size_t>::max() - b)
    return true;
  *result = a + b;
  return false;
}

// Splits |packet| into frame ranges. Without a superframe index a single range
// covering the whole packet is produced. A present-but-corrupt index is a hard
// failure; it is never reinterpreted as an ordinary frame.
bool ParseSuperframe(const uint8_t* packet, size_t size,
                     std::vector<FrameSpan>* spans) {
  spans->clear();
  if (size == 0)
    return false;

  const uint8_t marker = packet[size - 1];
  if ((marker & kSuperframeMarkerMask) != kSuperframeMarker) {
    FrameSpan span;
    span.offset = 0;
    span.length = size;
    spans->push_back(span);
    return true;
  }

  const int num_frames = (marker & 0x7) + 1;
  const int size_bytes = ((marker >> 3) & 0x3) + 1;
  if (num_frames < 1 || num_frames > kMaxSuperframeFrames ||
      size_bytes < 1 || size_bytes > kMaxSuperframeSizeBytes) {
    return false;
  }

  const size_t index_length =
      2 + static_cast<size_t>(size_bytes) * static_cast<size_t>(num_frames);
  if (index_length > size)
    return false;

  // The index begins with a copy of the final marker byte.
  if (packet[size - index_length] != marker)
    return false;

  const size_t payload_length = size - index_length;
  const uint8_t* cursor = packet + payload_length + 1;
  const uint8_t* index_end = packet + size - 1;

  size_t total = 0;
  for (int i = 0; i < num_frames; ++i) {
    uint32_t child_length = 0;
    for (int j = 0; j < size_bytes; ++j) {
      if (cursor >= index_end)
        return false;
      child_length |= static_cast<uint32_t>(*cursor++) << (8 * j);
    }
    if (child_length == 0)
      return false;
    size_t next_total = 0;
    if (AddOverflows(total, child_length, &next_total))
      return false;
    total = next_total;
    if (total > payload_length)
      return false;
    FrameSpan span;
    span.offset = total - child_length;
    span.length = child_length;
    spans->push_back(span);
  }

  // The index must account for every payload byte: no gaps, no leftovers.
  return total == payload_length;
}

// Builds a show_existing frame layout. Only the index and the frame range are
// reported; width/height, header byte counts, and tiles stay zero/empty
// because the layout of the displayed picture is not known here.
void BuildShowExistingLayout(size_t packet_offset, size_t frame_size,
                             const internal::Vp9HeaderFields& fields,
                             Vp9FrameLayout* layout) {
  layout->offset = packet_offset;
  layout->length = frame_size;
  layout->show_existing = true;
  layout->existing_index = fields.existing_index;
}

// Builds an ordinary frame layout and splits its tile payload into ranges.
bool BuildFrameLayout(const uint8_t* packet, size_t packet_size,
                      size_t packet_offset, size_t frame_size,
                      const internal::Vp9HeaderFields& fields,
                      Vp9FrameLayout* layout) {
  if (packet_offset > packet_size || frame_size > packet_size - packet_offset)
    return false;
  if (fields.size_from_ref)
    return false;
  if (fields.width <= 0 || fields.height <= 0)
    return false;

  const size_t uncompressed_header_bytes = fields.uncompressed_header_bytes;
  if (uncompressed_header_bytes > frame_size)
    return false;

  size_t compressed_header_offset = 0;
  if (AddOverflows(packet_offset, uncompressed_header_bytes,
                   &compressed_header_offset)) {
    return false;
  }
  size_t payload_offset = 0;
  if (AddOverflows(packet_offset, uncompressed_header_bytes,
                   &payload_offset) ||
      AddOverflows(payload_offset, fields.compressed_header_size,
                   &payload_offset)) {
    return false;
  }
  if (payload_offset > packet_size)
    return false;

  size_t frame_end = 0;
  if (AddOverflows(packet_offset, frame_size, &frame_end) ||
      payload_offset > frame_end) {
    return false;
  }

  const size_t tile_payload_size = frame_end - payload_offset;
  const int tile_rows = fields.row_tiles;
  const int tile_columns = fields.column_tiles;
  if (tile_rows <= 0 || tile_columns <= 0)
    return false;

  const size_t tile_count = static_cast<size_t>(tile_rows) *
                            static_cast<size_t>(tile_columns);
  // Each non-final tile needs a four-byte prefix plus at least one payload
  // byte; the final tile needs at least one payload byte. This bounds the
  // loop before it touches the packet.
  if (tile_payload_size == 0)
    return false;
  const size_t min_payload =
      (tile_count - 1) * (kTileLengthPrefixBytes + 1) + 1;
  if (min_payload > tile_payload_size)
    return false;

  std::vector<Vp9TileRange> tiles;
  tiles.reserve(tile_count);

  size_t position = payload_offset;
  for (int row = 0; row < tile_rows; ++row) {
    for (int column = 0; column < tile_columns; ++column) {
      const bool last_tile =
          (row == tile_rows - 1) && (column == tile_columns - 1);
      size_t tile_length = 0;
      if (last_tile) {
        // The final tile consumes the remainder of this frame's data and must
        // not extend into a following child or the superframe index.
        tile_length = frame_end - position;
      } else {
        if (kTileLengthPrefixBytes > frame_end - position)
          return false;
        const uint8_t* prefix = packet + position;
        const uint32_t declared = (static_cast<uint32_t>(prefix[0]) << 24) |
                                  (static_cast<uint32_t>(prefix[1]) << 16) |
                                  (static_cast<uint32_t>(prefix[2]) << 8) |
                                  static_cast<uint32_t>(prefix[3]);
        tile_length = declared;
        position += kTileLengthPrefixBytes;
        if (tile_length > frame_end - position)
          return false;
      }

      if (tile_length == 0)
        return false;

      Vp9TileRange tile;
      tile.offset = position;
      tile.length = tile_length;
      tile.row = row;
      tile.column = column;
      tiles.push_back(tile);

      position += tile_length;
    }
  }

  // The tiles must cover the payload exactly with no gaps or overlap.
  if (position != frame_end)
    return false;

  layout->offset = packet_offset;
  layout->length = frame_size;
  layout->uncompressed_header_bytes = uncompressed_header_bytes;
  layout->compressed_header_offset = compressed_header_offset;
  layout->compressed_header_size = fields.compressed_header_size;
  layout->width = fields.width;
  layout->height = fields.height;
  layout->tile_rows = tile_rows;
  layout->tile_columns = tile_columns;
  layout->show_existing = false;
  layout->existing_index = 0;
  layout->tiles.swap(tiles);
  return true;
}

}  // namespace

bool ExtractVP9Tiles(const uint8_t* packet, size_t size,
                     std::vector<Vp9FrameLayout>* out) {
  if (packet == NULL || size == 0 || out == NULL)
    return false;

  std::vector<FrameSpan> spans;
  if (!ParseSuperframe(packet, size, &spans) || spans.empty())
    return false;

  std::vector<Vp9FrameLayout> layouts;
  layouts.reserve(spans.size());
  for (size_t i = 0; i < spans.size(); ++i) {
    const FrameSpan& span = spans[i];
    const uint8_t* frame = packet + span.offset;

    internal::Vp9BitReader reader(frame, span.length);
    internal::Vp9HeaderFields fields;
    if (!internal::ParseUncompressedHeaderInternal(
            &reader, internal::kParseStrict, &fields)) {
      return false;
    }

    Vp9FrameLayout layout;
    if (fields.show_existing_frame) {
      BuildShowExistingLayout(span.offset, span.length, fields, &layout);
    } else if (!BuildFrameLayout(packet, size, span.offset, span.length, fields,
                                 &layout)) {
      return false;
    }
    layouts.push_back(layout);
  }

  out->swap(layouts);
  return true;
}

}  // namespace vp9_parser
