// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#include "common/vp9_tiles.h"

#include <stdint.h>

namespace vp9_parser {
namespace internal {
namespace {

const int kVp9FrameMarker = 2;
const uint32_t kVp9SyncCode = 0x498342u;
const int kMiSizeLog2 = 3;
const int kMinTileWidthB64 = 4;
const int kMaxTileWidthB64 = 64;
const int kMaxTileColumnsLog2 = 6;  // VP9 allows at most 64 tile columns.
const int kMaxTileRowsLog2 = 2;     // VP9 allows at most 4 tile rows.
const int kRefsPerFrame = 3;
const int kRefFramesLog2 = 3;
const int kSrgbColorSpace = 7;

// Bounds-checked bit reader for the VP9 uncompressed header. In strict mode a
// read past the end of the buffer marks the parse as failed. The bit offset
// always advances so bounded syntax loops terminate even for truncated input.
class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size, bool strict)
      : data_(data), size_(size), bit_offset_(0), strict_(strict), ok_(true) {}

  bool ok() const { return ok_; }
  size_t bit_offset() const { return bit_offset_; }
  size_t byte_offset() const { return (bit_offset_ + 7) >> 3; }

  int ReadBit() {
    const size_t byte_offset = bit_offset_ >> 3;
    const int bit_shift = 7 - static_cast<int>(bit_offset_ & 0x7);
    int bit = 0;
    if (byte_offset < size_) {
      bit = (data_[byte_offset] >> bit_shift) & 1;
    } else if (strict_) {
      ok_ = false;
    }
    ++bit_offset_;
    return bit;
  }

  uint32_t ReadLiteral(int bits) {
    uint32_t value = 0;
    for (int bit = bits - 1; bit >= 0; --bit)
      value |= static_cast<uint32_t>(ReadBit()) << bit;
    return value;
  }

  // VP9 signed literal: |magnitude_bits| magnitude bits followed by a sign
  // bit, which is present even when the magnitude is zero.
  int ReadSignedLiteral(int magnitude_bits) {
    const uint32_t magnitude = ReadLiteral(magnitude_bits);
    const int sign = ReadBit();
    return sign ? -static_cast<int>(magnitude) : static_cast<int>(magnitude);
  }

  // Requires the bits between the current position and the next byte boundary
  // to be zero.
  bool ReadZeroAlignment() {
    while ((bit_offset_ & 0x7) != 0) {
      if (ReadBit() != 0) return false;
      if (!ok_) return false;
    }
    return true;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t bit_offset_;
  bool strict_;
  bool ok_;
};

int AlignToPowerOfTwo(int value, int n) {
  return (value + (1 << n) - 1) & ~((1 << n) - 1);
}

// Number of 64 pixel superblock columns in a frame |width| pixels wide.
int SuperblockColumns(int width) {
  if (width <= 0) return 1;
  const int aligned_width = AlignToPowerOfTwo(width, kMiSizeLog2);
  const int mi_cols = aligned_width >> kMiSizeLog2;
  const int aligned_mi_cols = AlignToPowerOfTwo(mi_cols, kMiSizeLog2);
  return aligned_mi_cols >> kMiSizeLog2;
}

// Valid range of log2(tile_columns). The lower bound keeps a tile from
// spanning more than 64 superblocks; the upper bound keeps every tile but the
// last at least 4 superblocks wide and caps the count at 64 columns.
void TileColumnLog2Range(int sb_cols, int* min_log2, int* max_log2) {
  int min_value = 0;
  while ((kMaxTileWidthB64 << min_value) < sb_cols) ++min_value;

  int max_value = 0;
  while ((sb_cols >> max_value) >= kMinTileWidthB64) ++max_value;
  --max_value;
  if (max_value < 0) max_value = 0;
  if (max_value < min_value) max_value = min_value;
  if (max_value > kMaxTileColumnsLog2) max_value = kMaxTileColumnsLog2;

  *min_log2 = min_value;
  *max_log2 = max_value;
}

void ReadColorConfig(BitReader* reader, int profile, Vp9HeaderBits* bits) {
  if (profile >= 2)
    bits->bit_depth = reader->ReadBit() ? 12 : 10;
  else
    bits->bit_depth = 8;

  bits->color_space = static_cast<int>(reader->ReadLiteral(3));
  if (bits->color_space != kSrgbColorSpace) {
    bits->color_range = reader->ReadBit();
    if (profile == 1 || profile == 3) {
      bits->subsampling_x = reader->ReadBit();
      bits->subsampling_y = reader->ReadBit();
      reader->ReadBit();  // reserved_zero
    } else {
      bits->subsampling_x = 1;
      bits->subsampling_y = 1;
    }
  } else {
    bits->color_range = 1;
    if (profile == 1 || profile == 3) {
      bits->subsampling_x = 0;
      bits->subsampling_y = 0;
      reader->ReadBit();  // reserved_zero
    } else {
      bits->subsampling_x = 1;
      bits->subsampling_y = 1;
    }
  }
}

void ReadFrameSize(BitReader* reader, Vp9HeaderBits* bits) {
  bits->width = static_cast<int>(reader->ReadLiteral(16)) + 1;
  bits->height = static_cast<int>(reader->ReadLiteral(16)) + 1;
  if (reader->ReadBit()) {
    bits->display_width = static_cast<int>(reader->ReadLiteral(16)) + 1;
    bits->display_height = static_cast<int>(reader->ReadLiteral(16)) + 1;
  } else {
    bits->display_width = bits->width;
    bits->display_height = bits->height;
  }
}

bool ReadSyncCode(BitReader* reader) {
  return reader->ReadLiteral(24) == kVp9SyncCode;
}

void ReadLoopFilterParams(BitReader* reader) {
  reader->ReadLiteral(6);  // loop_filter_level
  reader->ReadLiteral(3);  // loop_filter_sharpness
  if (reader->ReadBit()) {  // loop_filter_delta_enabled
    if (reader->ReadBit()) {  // loop_filter_delta_update
      for (int i = 0; i < 4; ++i) {
        if (reader->ReadBit()) reader->ReadSignedLiteral(6);  // ref_deltas
      }
      for (int i = 0; i < 2; ++i) {
        if (reader->ReadBit()) reader->ReadSignedLiteral(6);  // mode_deltas
      }
    }
  }
}

void ReadQuantizationParams(BitReader* reader) {
  reader->ReadLiteral(8);  // base_q_idx
  for (int i = 0; i < 3; ++i) {
    if (reader->ReadBit()) {  // delta_coded
      reader->ReadSignedLiteral(4);
    }
  }
}

void ReadSegmentationParams(BitReader* reader) {
  if (!reader->ReadBit()) return;  // segmentation_enabled

  if (reader->ReadBit()) {  // segmentation_update_map
    // Seven optional tree probabilities, then the temporal update flag and up
    // to three optional prediction probabilities.
    for (int i = 0; i < 7; ++i) {
      if (reader->ReadBit()) reader->ReadLiteral(8);  // tree_probs
    }
    if (reader->ReadBit()) {  // segmentation_temporal_update
      for (int i = 0; i < 3; ++i) {
        if (reader->ReadBit()) reader->ReadLiteral(8);  // pred_probs
      }
    }
  }

  // Data update: absolute/delta mode followed by eight segments, each with
  // four feature-enabled flags.
  if (reader->ReadBit()) {  // segmentation_update_data
    reader->ReadBit();      // abs_or_delta
    for (int segment = 0; segment < 8; ++segment) {
      for (int feature = 0; feature < 4; ++feature) {
        if (!reader->ReadBit()) continue;  // feature_enabled
        switch (feature) {
          case 0:  // SEG_LVL_ALT_Q: 8 bit magnitude plus a sign bit.
            reader->ReadSignedLiteral(8);
            break;
          case 1:  // SEG_LVL_ALT_L: 6 bit magnitude plus a sign bit.
            reader->ReadSignedLiteral(6);
            break;
          case 2:  // SEG_LVL_REF_FRAME: 2 unsigned bits.
            reader->ReadLiteral(2);
            break;
          default:  // SEG_LVL_SKIP: zero bit width, no value bits.
            break;
        }
      }
    }
  }
}

void ReadTileInfo(BitReader* reader, Vp9HeaderBits* bits) {
  const int sb_cols = SuperblockColumns(bits->width);
  int min_log2_tile_cols = 0;
  int max_log2_tile_cols = 0;
  TileColumnLog2Range(sb_cols, &min_log2_tile_cols, &max_log2_tile_cols);

  int log2_tile_cols = min_log2_tile_cols;
  int max_ones = max_log2_tile_cols - min_log2_tile_cols;
  while (max_ones > 0) {
    --max_ones;
    if (!reader->ReadBit()) break;
    ++log2_tile_cols;
  }

  int log2_tile_rows = reader->ReadBit();
  if (log2_tile_rows) log2_tile_rows += reader->ReadBit();

  bits->tile_columns_log2 = log2_tile_cols;
  bits->tile_rows_log2 = log2_tile_rows;
  bits->column_tiles = 1 << log2_tile_cols;
  bits->row_tiles = 1 << log2_tile_rows;
  bits->parsed_tile_info = true;
}

}  // namespace

bool ParseVp9UncompressedHeader(const uint8_t* data, size_t size, bool strict,
                                Vp9HeaderBits* bits) {
  if (data == nullptr || size == 0 || bits == nullptr) return false;

  *bits = Vp9HeaderBits();
  BitReader reader(data, size, strict);

  if (reader.ReadLiteral(2) != static_cast<uint32_t>(kVp9FrameMarker))
    return false;

  bits->profile = static_cast<int>(reader.ReadBit());
  bits->profile |= static_cast<int>(reader.ReadBit()) << 1;
  if (bits->profile > 2) {
    const int reserved_zero = reader.ReadBit();
    bits->profile += reserved_zero;  // Historical arithmetic, valid streams add 0.
    if (strict && reserved_zero != 0) return false;
  }

  bits->show_existing = reader.ReadBit() != 0;
  if (strict && !reader.ok()) return false;
  if (bits->show_existing) {
    bits->existing_index = static_cast<int>(reader.ReadLiteral(3));
    if (strict) {
      if (!reader.ok()) return false;
      // The header is padded to a byte boundary with zero bits.
      if (!reader.ReadZeroAlignment()) return false;
    }
    bits->uncompressed_header_bytes = reader.byte_offset();
    return true;
  }

  bits->key = reader.ReadBit() == 0;
  bits->show_frame = reader.ReadBit() != 0;
  bits->error_resilient = reader.ReadBit() != 0;

  if (bits->key) {
    if (!ReadSyncCode(&reader)) return false;
    ReadColorConfig(&reader, bits->profile, bits);
    ReadFrameSize(&reader, bits);
  } else {
    if (!bits->show_frame) bits->intra_only = reader.ReadBit() != 0;
    if (!bits->error_resilient) {
      bits->reset_frame_context = static_cast<int>(reader.ReadLiteral(2));
    }

    if (bits->intra_only) {
      if (!ReadSyncCode(&reader)) return false;
      if (bits->profile > 0) {
        ReadColorConfig(&reader, bits->profile, bits);
      } else {
        bits->bit_depth = 8;
        bits->color_space = 1;  // BT.601
        bits->color_range = 0;  // Studio range
        bits->subsampling_x = 1;
        bits->subsampling_y = 1;
      }
      bits->refresh_frame_flags = static_cast<int>(reader.ReadLiteral(8));
      ReadFrameSize(&reader, bits);
    } else {
      bits->refresh_frame_flags = static_cast<int>(reader.ReadLiteral(8));
      for (int i = 0; i < kRefsPerFrame; ++i) {
        reader.ReadLiteral(kRefFramesLog2);  // ref_frame_idx
        reader.ReadBit();                    // ref_frame_sign_bias
      }

      bool found_ref = false;
      for (int i = 0; i < kRefsPerFrame; ++i) {
        if (reader.ReadBit()) {
          found_ref = true;
          break;
        }
      }

      if (found_ref) {
        // frame_size_with_refs() selected a reference frame and the size is
        // not in the bitstream. ExtractVP9Tiles has no reference slot model
        // and refuses such frames instead of guessing a size. Vp9HeaderParser
        // keeps accepting them and retains its previous size fields.
        bits->size_inherited = true;
        bits->uncompressed_header_bytes = reader.byte_offset();
        if (strict && !reader.ok()) return false;
        return true;
      }
      ReadFrameSize(&reader, bits);

      reader.ReadBit();  // allow_high_precision_mv
      if (!reader.ReadBit()) reader.ReadLiteral(2);  // interpolation_filter
    }
  }
  if (!bits->error_resilient) {
    reader.ReadBit();  // refresh_frame_context
    bits->frame_parallel_mode = reader.ReadBit() != 0;
  } else {
    bits->frame_parallel_mode = true;
  }

  reader.ReadLiteral(2);  // frame_context_idx

  ReadLoopFilterParams(&reader);
  ReadQuantizationParams(&reader);

  ReadSegmentationParams(&reader);

  ReadTileInfo(&reader, bits);
  if (strict &&
      (bits->tile_rows_log2 > kMaxTileRowsLog2 ||
       bits->tile_columns_log2 > kMaxTileColumnsLog2))
    return false;

  bits->compressed_header_size = reader.ReadLiteral(16);

  if (strict) {
    if (!reader.ok()) return false;
    if (!reader.ReadZeroAlignment()) return false;
  }

  bits->uncompressed_header_bytes = reader.byte_offset();
  if (strict) {
    if (bits->uncompressed_header_bytes > size) return false;
    if (bits->compressed_header_size >
        size - bits->uncompressed_header_bytes)
      return false;
  }
  return true;
}

}  // namespace internal

namespace {

// Splits |packet| into its subframes when it carries a VP9 superframe index.
// Returns false only for a corrupt index: |has_index| tells the caller whether
// an index was present at all, so a bad index is never treated as a plain
// single-frame packet.
bool ParseSuperFrameIndex(const uint8_t* packet, size_t size, bool* has_index,
                          std::vector<size_t>* frame_offsets,
                          std::vector<size_t>* frame_lengths) {
  *has_index = false;
  frame_offsets->clear();
  frame_lengths->clear();

  const uint8_t marker = packet[size - 1];
  if ((marker & 0xe0) != 0xc0) return true;  // No superframe index.

  *has_index = true;
  const size_t num_frames = (marker & 0x7) + 1;                // 1..8
  const size_t length_field_width = ((marker >> 3) & 0x3) + 1;  // 1..4
  const size_t index_length = 2 + length_field_width * num_frames;
  if (size < index_length) return false;
  // Both markers must match. The trailing one is the byte just inspected; the
  // leading one opens the index.
  if (packet[size - index_length] != marker) return false;

  const size_t payload_length = size - index_length;
  const uint8_t* lengths = packet + size - index_length + 1;

  size_t total = 0;
  for (size_t i = 0; i < num_frames; ++i) {
    size_t child_length = 0;
    for (size_t byte = 0; byte < length_field_width; ++byte) {
      child_length |= static_cast<size_t>(lengths[i * length_field_width + byte])
                      << (8 * byte);
    }
    if (child_length == 0) return false;
    if (child_length > payload_length - total) return false;
    frame_offsets->push_back(total);
    frame_lengths->push_back(child_length);
    total += child_length;
  }

  // Subframes must exactly cover the payload; the index length is implicit.
  return total == payload_length;
}

bool ParseFrameLayout(const uint8_t* packet, size_t packet_size,
                      size_t frame_offset, size_t frame_length,
                      Vp9FrameLayout* layout) {
  if (frame_offset > packet_size || frame_length > packet_size - frame_offset)
    return false;

  internal::Vp9HeaderBits bits;
  if (!internal::ParseVp9UncompressedHeader(packet + frame_offset, frame_length,
                                            /*strict=*/true, &bits))
    return false;
  if (bits.size_inherited) return false;

  layout->offset = frame_offset;
  layout->length = frame_length;
  layout->uncompressed_header_bytes = bits.uncompressed_header_bytes;
  layout->width = bits.width;
  layout->height = bits.height;
  layout->tile_rows = bits.row_tiles;
  layout->tile_columns = bits.column_tiles;
  layout->show_existing = bits.show_existing;
  layout->existing_index = bits.show_existing ? bits.existing_index : 0;

  if (bits.show_existing) {
    // A show-existing frame carries only the index: no compressed header and
    // no tiles are described, and no layout of the shown picture is invented.
    layout->width = 0;
    layout->height = 0;
    layout->tile_rows = 0;
    layout->tile_columns = 0;
    return bits.uncompressed_header_bytes <= frame_length;
  }

  if (!bits.parsed_tile_info) return false;
  if (bits.row_tiles <= 0 || bits.column_tiles <= 0) return false;

  const size_t header_bytes = bits.uncompressed_header_bytes;
  const size_t compressed_size = bits.compressed_header_size;
  if (header_bytes > frame_length) return false;
  if (compressed_size > frame_length - header_bytes) return false;

  layout->compressed_header_offset = frame_offset + header_bytes;
  layout->compressed_header_size = compressed_size;

  const size_t tile_count = static_cast<size_t>(bits.row_tiles) *
                            static_cast<size_t>(bits.column_tiles);
  if (tile_count == 0 || tile_count > 256) return false;

  const size_t frame_end = frame_offset + frame_length;
  size_t position = frame_offset + header_bytes + compressed_size;

  layout->tiles.clear();
  layout->tiles.reserve(tile_count);
  for (size_t i = 0; i < tile_count; ++i) {
    Vp9TileRange range;
    range.row = static_cast<int>(i / static_cast<size_t>(bits.column_tiles));
    range.column = static_cast<int>(i % static_cast<size_t>(bits.column_tiles));

    if (i + 1 < tile_count) {
      // Every tile but the last is preceded by a four byte big-endian length.
      // The prefix is not part of the returned range.
      if (position > frame_end || frame_end - position < 4) return false;
      const size_t tile_length =
          (static_cast<size_t>(packet[position]) << 24) |
          (static_cast<size_t>(packet[position + 1]) << 16) |
          (static_cast<size_t>(packet[position + 2]) << 8) |
          static_cast<size_t>(packet[position + 3]);
      position += 4;
      if (tile_length == 0) return false;
      if (tile_length > frame_end - position) return false;
      range.offset = position;
      range.length = tile_length;
      position += tile_length;
    } else {
      // The last tile consumes exactly what remains of this subframe and never
      // reaches into the next subframe or the superframe index.
      if (position >= frame_end) return false;
      range.offset = position;
      range.length = frame_end - position;
      position = frame_end;
    }
    layout->tiles.push_back(range);
  }

  return position == frame_end;
}

}  // namespace

bool ExtractVP9Tiles(const uint8_t* packet, size_t size,
                     std::vector<Vp9FrameLayout>* out) {
  if (packet == nullptr || size == 0 || out == nullptr) return false;

  bool has_index = false;
  std::vector<size_t> frame_offsets;
  std::vector<size_t> frame_lengths;
  if (!ParseSuperFrameIndex(packet, size, &has_index, &frame_offsets,
                            &frame_lengths))
    return false;

  if (!has_index) {
    frame_offsets.push_back(0);
    frame_lengths.push_back(size);
  }

  std::vector<Vp9FrameLayout> layouts(frame_offsets.size());
  for (size_t i = 0; i < frame_offsets.size(); ++i) {
    if (!ParseFrameLayout(packet, size, frame_offsets[i], frame_lengths[i],
                          &layouts[i]))
      return false;
  }

  // Replace the caller's output only after every subframe parsed successfully.
  out->swap(layouts);
  return true;
}

}  // namespace vp9_parser
