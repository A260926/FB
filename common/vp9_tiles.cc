// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#include "common/vp9_tiles.h"

#include <limits>

#include "common/vp9_bit_reader.h"
#include "common/vp9_uncompressed_header.h"

namespace vp9_parser {

namespace {

const std::uint8_t kSuperFrameMarkerMask = 0xe0;
const std::uint8_t kSuperFrameMarker = 0xc0;

bool AddSizes(std::size_t a, std::size_t b, std::size_t* sum) {
  if (a > std::numeric_limits<std::size_t>::max() - b)
    return false;
  *sum = a + b;
  return true;
}

// Validates the superframe index at the end of |packet| and returns the byte
// ranges of its sub-frames. Unlike a permissive scan, a trailing marker byte
// forces full validation: any malformed index fails the whole packet instead
// of falling back to single-frame parsing.
bool ParseSuperFrame(const std::uint8_t* packet, std::size_t size,
                     std::vector<Vp9TileRange>* subframes) {
  const std::uint8_t marker = packet[size - 1];
  if ((marker & kSuperFrameMarkerMask) != kSuperFrameMarker)
    return false;

  const int num_frames = (marker & 0x7) + 1;
  const int length_field_size = ((marker >> 3) & 0x3) + 1;
  const std::size_t index_length =
      2 + static_cast<std::size_t>(length_field_size) * num_frames;
  if (size < index_length)
    return false;

  const std::size_t payload_length = size - index_length;
  if (packet[payload_length] != marker)
    return false;

  const std::uint8_t* lengths = packet + payload_length + 1;
  std::size_t offset = 0;
  std::vector<Vp9TileRange> parsed;
  for (int i = 0; i < num_frames; ++i) {
    std::size_t child_length = 0;
    for (int j = 0; j < length_field_size; ++j) {
      const std::size_t byte = lengths[i * length_field_size + j];
      child_length |= byte << (j * 8);
    }
    if (child_length == 0 || child_length > payload_length - offset)
      return false;
    parsed.push_back(Vp9TileRange(offset, child_length, 0, 0));
    offset += child_length;
  }

  // The declared sub-frames must cover the payload exactly, with no gap.
  if (offset != payload_length)
    return false;

  *subframes = std::move(parsed);
  return true;
}

// Reads a four byte big-endian tile size prefix.
bool ReadBe32(const std::uint8_t* data, std::size_t available,
              std::size_t* value) {
  if (available < 4)
    return false;
  *value = (static_cast<std::size_t>(data[0]) << 24) |
           (static_cast<std::size_t>(data[1]) << 16) |
           (static_cast<std::size_t>(data[2]) << 8) |
           static_cast<std::size_t>(data[3]);
  return true;
}

// Splits the tile payload of one non show-existing sub-frame into row-major
// tile ranges. |frame_base| is the offset of the sub-frame inside the packet.
bool ExtractTiles(const std::uint8_t* frame, std::size_t frame_length,
                  const Vp9UncompressedHeaderInfo& header,
                  std::size_t frame_base, Vp9FrameLayout* layout) {
  const std::size_t header_end = header.uncompressed_header_bytes;
  const std::size_t compressed_size = header.compressed_header_size;

  std::size_t tile_start = 0;
  if (!AddSizes(header_end, compressed_size, &tile_start) ||
      tile_start > frame_length) {
    return false;
  }

  const std::size_t tile_payload_length = frame_length - tile_start;
  const int tile_count = header.tile_rows * header.tile_columns;
  if (tile_count <= 0 || tile_payload_length == 0)
    return false;

  std::vector<Vp9TileRange> tiles;
  tiles.reserve(static_cast<std::size_t>(tile_count));

  std::size_t cursor = tile_start;
  for (int index = 0; index < tile_count - 1; ++index) {
    std::size_t tile_length = 0;
    if (!ReadBe32(frame + cursor, frame_length - cursor, &tile_length))
      return false;
    cursor += 4;
    if (tile_length == 0 || tile_length > frame_length - cursor)
      return false;

    const int row = index / header.tile_columns;
    const int column = index % header.tile_columns;
    std::size_t packet_offset = 0;
    if (!AddSizes(frame_base, cursor, &packet_offset))
      return false;
    tiles.push_back(Vp9TileRange(packet_offset, tile_length, row, column));
    cursor += tile_length;
    if (cursor > frame_length)
      return false;
  }

  // The final tile takes the remainder of the sub-frame, which cannot reach
  // into the next sub-frame or the superframe index because |frame_length| is
  // bounded by the validated index.
  const std::size_t last_length = frame_length - cursor;
  if (last_length == 0)
    return false;
  std::size_t last_offset = 0;
  if (!AddSizes(frame_base, cursor, &last_offset))
    return false;
  tiles.push_back(Vp9TileRange(last_offset, last_length,
                               (tile_count - 1) / header.tile_columns,
                               (tile_count - 1) % header.tile_columns));

  // Re-walk the payload: every tile must be non-empty, payloads must not
  // overlap (the four byte size prefixes sit between them) and together they
  // must cover the complete tile payload of the sub-frame.
  std::size_t expected_cursor = tile_start;
  for (int index = 0; index < tile_count - 1; ++index) {
    expected_cursor += 4;
    std::size_t expected_offset = 0;
    if (!AddSizes(frame_base, expected_cursor, &expected_offset) ||
        expected_offset != tiles[index].offset ||
        tiles[index].length == 0) {
      return false;
    }
    expected_cursor += tiles[index].length;
  }
  std::size_t last_expected = 0;
  if (!AddSizes(frame_base, expected_cursor, &last_expected) ||
      last_expected != tiles.back().offset ||
      expected_cursor + tiles.back().length != frame_length) {
    return false;
  }

  layout->tiles = std::move(tiles);
  return true;
}

bool ParseSingleFrame(const std::uint8_t* frame, std::size_t frame_length,
                      std::size_t frame_base, Vp9FrameLayout* layout) {
  Vp9BitReader reader(frame, frame_length);
  Vp9UncompressedHeaderInfo header;
  const Vp9HeaderParseStatus status =
      ParseVp9UncompressedHeader(&reader, &header);
  if (status == kVp9HeaderError || status == kVp9HeaderReferenceSize)
    return false;

  Vp9FrameLayout parsed;
  parsed.offset = frame_base;
  parsed.length = frame_length;

  if (header.show_existing) {
    // A show-existing frame references another frame: it carries no layout of
    // its own, so only the reference index and frame range are reported.
    parsed.show_existing = true;
    parsed.existing_index = header.existing_index;
    *layout = std::move(parsed);
    return true;
  }

  if (header.tile_rows != 1 && header.tile_rows != 2 &&
      header.tile_rows != 4) {
    return false;
  }
  if (header.width <= 0 || header.height <= 0)
    return false;

  parsed.uncompressed_header_bytes = header.uncompressed_header_bytes;
  std::size_t compressed_offset = 0;
  if (!AddSizes(frame_base, header.uncompressed_header_bytes,
                &compressed_offset)) {
    return false;
  }
  parsed.compressed_header_offset = compressed_offset;
  parsed.compressed_header_size = header.compressed_header_size;
  parsed.width = header.width;
  parsed.height = header.height;
  parsed.tile_rows = header.tile_rows;
  parsed.tile_columns = header.tile_columns;

  if (!ExtractTiles(frame, frame_length, header, frame_base, &parsed))
    return false;

  *layout = std::move(parsed);
  return true;
}

}  // namespace

bool ExtractVP9Tiles(const std::uint8_t* packet, std::size_t size,
                     std::vector<Vp9FrameLayout>* out) {
  if (packet == nullptr || size == 0 || out == nullptr)
    return false;

  std::vector<Vp9TileRange> subframes;
  const bool has_index =
      (packet[size - 1] & kSuperFrameMarkerMask) == kSuperFrameMarker;
  if (has_index) {
    if (!ParseSuperFrame(packet, size, &subframes))
      return false;
  } else {
    subframes.push_back(Vp9TileRange(0, size, 0, 0));
  }

  std::vector<Vp9FrameLayout> layouts;
  layouts.reserve(subframes.size());
  for (const Vp9TileRange& subframe : subframes) {
    if (subframe.length == 0 ||
        subframe.offset > size - subframe.length) {
      return false;
    }
    Vp9FrameLayout layout;
    if (!ParseSingleFrame(packet + subframe.offset, subframe.length,
                          subframe.offset, &layout)) {
      return false;
    }
    layouts.push_back(std::move(layout));
  }

  *out = std::move(layouts);
  return true;
}

}  // namespace vp9_parser
