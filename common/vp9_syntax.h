// Copyright (c) 2024 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
//
// Internal, header-only VP9 uncompressed-header parsing helpers.
//
// This file exists so that Vp9HeaderParser::ParseUncompressedHeader and
// ExtractVP9Tiles share a single implementation of the uncompressed header
// syntax (including the full segmentation_params() grammar) and a single set
// of bounds-checked bit reads. It is not part of the public API.

#ifndef LIBWEBM_COMMON_VP9_SYNTAX_H_
#define LIBWEBM_COMMON_VP9_SYNTAX_H_

#include <stddef.h>
#include <stdint.h>

namespace vp9_parser {
namespace internal {

// VP9 defines the following constants in the tile_info() syntax.
const int kMaxTileWidthB64 = 64;
const int kMinTileWidthB64 = 4;
const int kMaxTileRowsLog2 = 2;

// Bit widths of the segmentation features, in segment_feature order.
const int kSegmentationFeatureBits[4] = {8, 6, 2, 0};
const bool kSegmentationFeatureSigned[4] = {true, true, false, false};
const int kSegmentationFeatureCount = 4;
const int kSegmentationSegmentCount = 8;

// Bounds-checked, big-endian bit reader. Every read fails (and latches the
// failure for subsequent calls) when it would run past the end of |data|.
class Vp9BitReader {
 public:
  Vp9BitReader(const uint8_t* data, size_t size)
      : data_(data), size_(size), bit_position_(0), ok_(data != NULL) {}

  bool ok() const { return ok_; }
  size_t bit_position() const { return bit_position_; }
  size_t byte_position() const {
    return (bit_position_ >> 3) + ((bit_position_ & 7) ? 1 : 0);
  }

  bool ReadBit(int* value) {
    uint32_t bit = 0;
    if (!ReadBits(1, &bit))
      return false;
    *value = static_cast<int>(bit);
    return true;
  }

  bool ReadBits(int num_bits, uint32_t* value) {
    if (num_bits < 0 || num_bits > 32 || value == NULL)
      return Fail();
    if (!Ensure(num_bits))
      return false;
    uint32_t result = 0;
    for (int i = 0; i < num_bits; ++i) {
      const size_t position = bit_position_++;
      const size_t byte = position >> 3;
      const int shift = 7 - static_cast<int>(position & 7);
      result = (result << 1) | ((data_[byte] >> shift) & 1);
    }
    *value = result;
    return true;
  }

  bool ReadLiteral(int num_bits, int* value) {
    uint32_t result = 0;
    if (value == NULL || !ReadBits(num_bits, &result))
      return false;
    *value = static_cast<int>(result);
    return true;
  }

  // Reads an s(n) value: n magnitude bits followed by one sign bit. The sign
  // bit is consumed even when the magnitude is zero.
  bool ReadSigned(int num_bits, int* value) {
    if (value == NULL || num_bits < 0 || num_bits > 31)
      return Fail();
    uint32_t magnitude = 0;
    if (!ReadBits(num_bits, &magnitude))
      return false;
    uint32_t sign = 0;
    if (!ReadBits(1, &sign))
      return false;
    const int32_t signed_magnitude = static_cast<int32_t>(magnitude);
    *value = sign ? -signed_magnitude : signed_magnitude;
    return true;
  }

  bool SkipBits(size_t num_bits) {
    while (num_bits > 0) {
      const int chunk = num_bits > 32 ? 32 : static_cast<int>(num_bits);
      uint32_t ignored = 0;
      if (!ReadBits(chunk, &ignored))
        return false;
      num_bits -= static_cast<size_t>(chunk);
    }
    return true;
  }

  // Discards bits up to the next byte boundary. When |require_zero| is true
  // any non-zero padding bit is a parse failure.
  bool AlignToByte(bool require_zero) {
    while (bit_position_ & 7) {
      int bit = 0;
      if (!ReadBit(&bit))
        return false;
      if (require_zero && bit != 0)
        return Fail();
    }
    return true;
  }

 private:
  bool Fail() {
    ok_ = false;
    return false;
  }

  bool Ensure(size_t num_bits) {
    if (!ok_)
      return false;
    if (num_bits > 32)
      return Fail();
    const size_t byte = bit_position_ >> 3;
    const size_t used = bit_position_ & 7;
    if (byte > size_)
      return Fail();
    const size_t available = size_ - byte;
    const size_t needed = (used + num_bits + 7) >> 3;
    if (needed > available)
      return Fail();
    return true;
  }

  const uint8_t* data_;
  size_t size_;
  size_t bit_position_;
  bool ok_;
};

// Fields produced by parsing an uncompressed header. Callers may pre-populate
// the structure; fields that are not present in the parsed header are left
// untouched.
struct Vp9HeaderFields {
  Vp9HeaderFields()
      : profile(-1),
        show_existing(0),
        existing_index(0),
        frame_type(0),
        key(0),
        show_frame(0),
        error_resilient_mode(0),
        intra_only(0),
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
        row_tiles(0),
        column_tiles(0),
        tile_rows_log2(0),
        tile_cols_log2(0),
        frame_parallel_mode(0),
        frame_context_idx(0),
        size_from_ref(false),
        show_existing_frame(false),
        compressed_header_size(0),
        uncompressed_header_bytes(0),
        has_color_config(false),
        has_frame_size(false),
        has_render_size(false),
        has_refresh_frame_flags(false),
        has_frame_parallel_mode(false),
        has_tile_info(false) {}

  int profile;
  int show_existing;
  int existing_index;
  int frame_type;
  int key;
  int show_frame;
  int error_resilient_mode;
  int intra_only;
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
  int row_tiles;
  int column_tiles;
  int tile_rows_log2;
  int tile_cols_log2;
  int frame_parallel_mode;
  int frame_context_idx;
  bool size_from_ref;
  bool show_existing_frame;
  uint32_t compressed_header_size;
  size_t uncompressed_header_bytes;
  bool has_color_config;
  bool has_frame_size;
  bool has_render_size;
  bool has_refresh_frame_flags;
  bool has_frame_parallel_mode;
  bool has_tile_info;
};

enum Vp9ParseMode {
  // Matches the historical Vp9HeaderParser behavior: parses the fields the
  // old parser consumed, stops early for non-key frames, and accepts frames
  // whose dimensions are inherited from a reference.
  kParseLegacy,
  // Parses the complete uncompressed header for key, intra-only, and
  // explicit-size inter frames. Inherited-size inter frames are rejected.
  kParseStrict,
};

inline bool ParseFrameSyncCode(Vp9BitReader* reader) {
  uint32_t sync = 0;
  if (!reader->ReadBits(24, &sync))
    return false;
  return sync == 0x498342;
}

inline bool ReadProbability(Vp9BitReader* reader) {
  int coded = 0;
  if (!reader->ReadBit(&coded))
    return false;
  if (!coded)
    return true;
  uint32_t prob = 0;
  return reader->ReadBits(8, &prob);
}

inline bool ParseColorConfig(Vp9BitReader* reader, int profile,
                             Vp9HeaderFields* fields) {
  if (profile >= 2) {
    int twelve_bit = 0;
    if (!reader->ReadBit(&twelve_bit))
      return false;
    fields->bit_depth = twelve_bit ? 12 : 10;
  } else {
    fields->bit_depth = 8;
  }

  int color_space = 0;
  if (!reader->ReadLiteral(3, &color_space))
    return false;
  fields->color_space = color_space;

  if (color_space != 7 /* CS_RGB */) {
    int color_range = 0;
    if (!reader->ReadBit(&color_range))
      return false;
    fields->color_range = color_range;
    if (profile == 1 || profile == 3) {
      int subsampling_x = 0;
      int subsampling_y = 0;
      int reserved_zero = 0;
      if (!reader->ReadBit(&subsampling_x) ||
          !reader->ReadBit(&subsampling_y) ||
          !reader->ReadBit(&reserved_zero)) {
        return false;
      }
      fields->subsampling_x = subsampling_x;
      fields->subsampling_y = subsampling_y;
    } else {
      fields->subsampling_x = 1;
      fields->subsampling_y = 1;
    }
  } else {
    fields->color_range = 1;
    if (profile == 1 || profile == 3) {
      int reserved_zero = 0;
      if (!reader->ReadBit(&reserved_zero))
        return false;
      fields->subsampling_x = 0;
      fields->subsampling_y = 0;
    }
  }
  fields->has_color_config = true;
  return true;
}

inline bool ParseFrameSize(Vp9BitReader* reader, Vp9HeaderFields* fields) {
  int width_minus_1 = 0;
  int height_minus_1 = 0;
  if (!reader->ReadLiteral(16, &width_minus_1) ||
      !reader->ReadLiteral(16, &height_minus_1)) {
    return false;
  }
  fields->width = width_minus_1 + 1;
  fields->height = height_minus_1 + 1;
  fields->has_frame_size = true;
  return true;
}

inline bool ParseRenderSize(Vp9BitReader* reader, Vp9HeaderFields* fields) {
  int different = 0;
  if (!reader->ReadBit(&different))
    return false;
  if (different) {
    int width_minus_1 = 0;
    int height_minus_1 = 0;
    if (!reader->ReadLiteral(16, &width_minus_1) ||
        !reader->ReadLiteral(16, &height_minus_1)) {
      return false;
    }
    fields->display_width = width_minus_1 + 1;
    fields->display_height = height_minus_1 + 1;
  } else {
    fields->display_width = fields->width;
    fields->display_height = fields->height;
  }
  fields->has_render_size = true;
  return true;
}

inline bool ParseLoopFilterParams(Vp9BitReader* reader) {
  uint32_t ignored = 0;
  if (!reader->ReadBits(6, &ignored) || !reader->ReadBits(3, &ignored))
    return false;
  int delta_enabled = 0;
  if (!reader->ReadBit(&delta_enabled))
    return false;
  if (!delta_enabled)
    return true;
  int delta_update = 0;
  if (!reader->ReadBit(&delta_update))
    return false;
  if (!delta_update)
    return true;

  for (int i = 0; i < 4; ++i) {
    int update = 0;
    if (!reader->ReadBit(&update))
      return false;
    if (update) {
      int delta = 0;
      if (!reader->ReadSigned(6, &delta))
        return false;
    }
  }
  for (int i = 0; i < 2; ++i) {
    int update = 0;
    if (!reader->ReadBit(&update))
      return false;
    if (update) {
      int delta = 0;
      if (!reader->ReadSigned(6, &delta))
        return false;
    }
  }
  return true;
}

inline bool ParseQuantizationParams(Vp9BitReader* reader) {
  uint32_t ignored = 0;
  if (!reader->ReadBits(8, &ignored))
    return false;
  for (int i = 0; i < 3; ++i) {
    int delta_coded = 0;
    if (!reader->ReadBit(&delta_coded))
      return false;
    if (delta_coded) {
      int delta = 0;
      if (!reader->ReadSigned(4, &delta))
        return false;
    }
  }
  return true;
}

inline bool ParseSegmentationParams(Vp9BitReader* reader) {
  int segmentation_enabled = 0;
  if (!reader->ReadBit(&segmentation_enabled))
    return false;
  if (!segmentation_enabled)
    return true;

  int update_map = 0;
  if (!reader->ReadBit(&update_map))
    return false;
  if (update_map) {
    for (int i = 0; i < 7; ++i) {
      if (!ReadProbability(reader))
        return false;
    }
    int temporal_update = 0;
    if (!reader->ReadBit(&temporal_update))
      return false;
    if (temporal_update) {
      for (int i = 0; i < 3; ++i) {
        if (!ReadProbability(reader))
          return false;
      }
    }
  }

  int update_data = 0;
  if (!reader->ReadBit(&update_data))
    return false;
  if (!update_data)
    return true;

  int abs_or_delta = 0;
  if (!reader->ReadBit(&abs_or_delta))
    return false;

  for (int segment = 0; segment < kSegmentationSegmentCount; ++segment) {
    for (int feature = 0; feature < kSegmentationFeatureCount; ++feature) {
      int feature_enabled = 0;
      if (!reader->ReadBit(&feature_enabled))
        return false;
      if (!feature_enabled)
        continue;

      const int bits = kSegmentationFeatureBits[feature];
      uint32_t magnitude = 0;
      if (bits > 0 && !reader->ReadBits(bits, &magnitude))
        return false;
      if (kSegmentationFeatureSigned[feature]) {
        int sign = 0;
        if (!reader->ReadBit(&sign))
          return false;
      }
    }
  }
  return true;
}

// Computes Sb64Cols exactly as compute_image_size() does.
inline int ComputeSb64Cols(int width) {
  const int mi_cols = (width + 7) >> 3;
  return (mi_cols + 7) >> 3;
}

inline bool ParseTileInfo(Vp9BitReader* reader, Vp9HeaderFields* fields) {
  const int sb64_cols = ComputeSb64Cols(fields->width);

  int min_log2_tile_cols = 0;
  while ((kMaxTileWidthB64 << min_log2_tile_cols) < sb64_cols)
    ++min_log2_tile_cols;

  int max_log2_tile_cols = 1;
  while ((sb64_cols >> max_log2_tile_cols) >= kMinTileWidthB64)
    ++max_log2_tile_cols;
  --max_log2_tile_cols;
  if (max_log2_tile_cols < 0)
    max_log2_tile_cols = 0;

  int tile_cols_log2 = min_log2_tile_cols;
  while (tile_cols_log2 < max_log2_tile_cols) {
    int increment = 0;
    if (!reader->ReadBit(&increment))
      return false;
    if (!increment)
      break;
    ++tile_cols_log2;
  }

  int tile_rows_log2 = 0;
  if (!reader->ReadBit(&tile_rows_log2))
    return false;
  if (tile_rows_log2 == 1) {
    int increment = 0;
    if (!reader->ReadBit(&increment))
      return false;
    tile_rows_log2 += increment;
  }
  if (tile_rows_log2 > kMaxTileRowsLog2)
    return false;

  fields->tile_cols_log2 = tile_cols_log2;
  fields->tile_rows_log2 = tile_rows_log2;
  fields->column_tiles = 1 << tile_cols_log2;
  fields->row_tiles = 1 << tile_rows_log2;
  fields->has_tile_info = true;
  return true;
}

// Parses an uncompressed header (VP9 section 6.2). In kParseStrict mode the
// reader must be positioned at the start of a complete header, the 16-bit
// compressed header size is consumed, trailing zero padding is validated, and
// |fields->uncompressed_header_bytes| is set. In kParseLegacy mode the
// historical early-out points are preserved and no trailing padding or
// compressed-header size is consumed.
inline bool ParseUncompressedHeaderInternal(Vp9BitReader* reader,
                                            Vp9ParseMode mode,
                                            Vp9HeaderFields* fields) {
  if (reader == NULL || fields == NULL)
    return false;

  uint32_t frame_marker = 0;
  if (!reader->ReadBits(2, &frame_marker) || frame_marker != 2)
    return false;

  int profile_low_bit = 0;
  int profile_high_bit = 0;
  if (!reader->ReadBit(&profile_low_bit) ||
      !reader->ReadBit(&profile_high_bit)) {
    return false;
  }
  const int profile = (profile_high_bit << 1) + profile_low_bit;
  fields->profile = profile;
  if (profile == 3) {
    int reserved_zero = 0;
    if (!reader->ReadBit(&reserved_zero))
      return false;
  }

  int show_existing = 0;
  if (!reader->ReadBit(&show_existing))
    return false;
  fields->show_existing = show_existing;
  if (show_existing) {
    fields->show_existing_frame = true;
    if (mode == kParseStrict) {
      int frame_to_show_map_idx = 0;
      if (!reader->ReadLiteral(3, &frame_to_show_map_idx))
        return false;
      fields->existing_index = frame_to_show_map_idx;
      if (!reader->AlignToByte(true))
        return false;
      fields->uncompressed_header_bytes = reader->byte_position();
    }
    return true;
  }

  int frame_type = 0;
  int show_frame = 0;
  int error_resilient_mode = 0;
  if (!reader->ReadBit(&frame_type) || !reader->ReadBit(&show_frame) ||
      !reader->ReadBit(&error_resilient_mode)) {
    return false;
  }
  fields->frame_type = frame_type;
  fields->key = (frame_type == 0);
  fields->show_frame = show_frame;
  fields->error_resilient_mode = error_resilient_mode;

  if (fields->key) {
    if (!ParseFrameSyncCode(reader))
      return false;
    if (!ParseColorConfig(reader, profile, fields))
      return false;
    if (!ParseFrameSize(reader, fields))
      return false;
    if (!ParseRenderSize(reader, fields))
      return false;
    if (mode == kParseStrict)
      fields->refresh_frame_flags = 0xFF;
    fields->has_refresh_frame_flags = true;
  } else {
    int intra_only = 0;
    if (show_frame == 0) {
      if (!reader->ReadBit(&intra_only))
        return false;
    }
    fields->intra_only = intra_only;

    if (error_resilient_mode == 0) {
      int reset_frame_context = 0;
      if (!reader->ReadLiteral(2, &reset_frame_context))
        return false;
      fields->reset_frame_context = reset_frame_context;
    } else {
      fields->reset_frame_context = 0;
    }

    if (intra_only) {
      if (!ParseFrameSyncCode(reader))
        return false;
      if (profile > 0) {
        if (!ParseColorConfig(reader, profile, fields))
          return false;
      } else {
        fields->color_space = 1;   // CS_BT_601
        fields->color_range = 0;   // studio swing
        fields->subsampling_x = 1;
        fields->subsampling_y = 1;
        fields->bit_depth = 8;
        fields->has_color_config = true;
      }
      int refresh_frame_flags = 0;
      if (!reader->ReadLiteral(8, &refresh_frame_flags))
        return false;
      fields->refresh_frame_flags = refresh_frame_flags;
      fields->has_refresh_frame_flags = true;
      if (!ParseFrameSize(reader, fields))
        return false;
      if (!ParseRenderSize(reader, fields))
        return false;
      if (mode == kParseLegacy)
        return true;
    } else {
      int refresh_frame_flags = 0;
      if (!reader->ReadLiteral(8, &refresh_frame_flags))
        return false;
      fields->refresh_frame_flags = refresh_frame_flags;
      fields->has_refresh_frame_flags = true;

      for (int i = 0; i < 3; ++i) {
        int ref_frame_idx = 0;
        int ref_frame_sign_bias = 0;
        if (!reader->ReadLiteral(3, &ref_frame_idx) ||
            !reader->ReadBit(&ref_frame_sign_bias)) {
          return false;
        }
      }

      bool found_ref = false;
      for (int i = 0; i < 3; ++i) {
        int bit = 0;
        if (!reader->ReadBit(&bit))
          return false;
        if (bit) {
          found_ref = true;
          break;
        }
      }
      if (found_ref) {
        fields->size_from_ref = true;
        // Without a reference-slot model the frame dimensions cannot be
        // determined. The strict entry point rejects this rather than
        // guessing from a previous parse; the legacy parser keeps its
        // historical acceptance.
        if (mode == kParseStrict)
          return false;
        return true;
      }

      if (!ParseFrameSize(reader, fields))
        return false;
      if (!ParseRenderSize(reader, fields))
        return false;
      if (mode == kParseLegacy)
        return true;

      int allow_high_precision_mv = 0;
      if (!reader->ReadBit(&allow_high_precision_mv))
        return false;
      int is_filter_switchable = 0;
      if (!reader->ReadBit(&is_filter_switchable))
        return false;
      if (!is_filter_switchable) {
        int raw_interpolation_filter = 0;
        if (!reader->ReadLiteral(2, &raw_interpolation_filter))
          return false;
      }
    }
  }

  // Context flags, loop filter, quantization, segmentation, and tile layout.
  if (error_resilient_mode == 0) {
    int refresh_frame_context = 0;
    int frame_parallel_decoding_mode = 0;
    if (!reader->ReadBit(&refresh_frame_context) ||
        !reader->ReadBit(&frame_parallel_decoding_mode)) {
      return false;
    }
    fields->frame_parallel_mode = frame_parallel_decoding_mode;
    fields->has_frame_parallel_mode = true;
  } else {
    fields->frame_parallel_mode = 1;
    fields->has_frame_parallel_mode = true;
  }

  int frame_context_idx = 0;
  if (!reader->ReadLiteral(2, &frame_context_idx))
    return false;
  fields->frame_context_idx = frame_context_idx;

  if (!ParseLoopFilterParams(reader))
    return false;
  if (!ParseQuantizationParams(reader))
    return false;
  if (!ParseSegmentationParams(reader))
    return false;
  if (!ParseTileInfo(reader, fields))
    return false;

  if (mode == kParseStrict) {
    int header_size_in_bytes = 0;
    if (!reader->ReadLiteral(16, &header_size_in_bytes))
      return false;
    fields->compressed_header_size =
        static_cast<uint32_t>(header_size_in_bytes);
    if (!reader->AlignToByte(true))
      return false;
    fields->uncompressed_header_bytes = reader->byte_position();
  }
  return reader->ok();
}

}  // namespace internal
}  // namespace vp9_parser

#endif  // LIBWEBM_COMMON_VP9_SYNTAX_H_
