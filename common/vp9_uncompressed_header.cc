// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#include "common/vp9_uncompressed_header.h"

namespace vp9_parser {

namespace {

const int kVp9FrameMarker = 2;
const int kMinTileWidthB64 = 4;
const int kMaxTileWidthB64 = 64;
const int kRefFrames = 8;
const int kRefsPerFrame = 3;
const int kRefFramesLog2 = 3;
const int kVpxCsBt601 = 1;
const int kVpxCsSrgb = 7;
const int kVpxCrStudioRange = 0;
const int kVpxCrFullRange = 1;
const int kMiSizeLog2 = 3;

// Number of segments and per-segment feature syntax elements, matching the
// MAX_SEGMENTS / SEG_LVL_MAX limits in the VP9 specification.
const int kVp9Segments = 8;
const int kVp9SegmentFeatures = 4;

int AlignPowerOfTwo(int value, int n) {
  return (value + ((1 << n) - 1)) & ~((1 << n) - 1);
}

// Reads profile, frame type flags and (for non show-existing frames) the
// frame sync code. Mirrors the opening of uncompressed_header() in the VP9
// specification.
bool ReadFrameTag(Vp9BitReader* r, Vp9UncompressedHeaderInfo* info,
                  int* reset_frame_context) {
  if (r->ReadLiteral(2) != kVp9FrameMarker)
    return false;

  int profile = r->ReadBit();
  profile |= r->ReadBit() << 1;
  if (profile > 2)
    profile += r->ReadBit();
  info->profile = profile;

  info->show_existing = r->ReadBit();
  if (info->show_existing) {
    info->existing_index = r->ReadLiteral(3);
    return !r->error();
  }

  // frame_type: 0 means key frame.
  info->key = r->ReadBit() == 0;
  const bool show_frame = r->ReadBit();
  info->error_resilient = r->ReadBit();
  if (r->error())
    return false;

  // The bitstream has no altref flag: the historical accessor reported the
  // inverse of show_frame, so keep that mapping.
  info->altref = !show_frame;

  if (info->key) {
    *reset_frame_context = 0;
    return r->ReadLiteral(8) == 0x49 && r->ReadLiteral(8) == 0x83 &&
           r->ReadLiteral(8) == 0x42 && !r->error();
  }

  // The reset context field exists only for non error-resilient frames; the
  // intra-only flag exists only for error-resilient inter frames.
  if (info->error_resilient) {
    *reset_frame_context = 0;
    info->intra_only = r->ReadBit();
  } else {
    *reset_frame_context = r->ReadLiteral(2);
    info->intra_only = false;
  }
  if (r->error())
    return false;

  if (info->intra_only) {
    if (r->ReadLiteral(8) != 0x49 || r->ReadLiteral(8) != 0x83 ||
        r->ReadLiteral(8) != 0x42)
      return false;
  }
  return !r->error();
}

// Consumes color_config() and records the color fields in |info|.
void ReadColorConfig(Vp9BitReader* r, Vp9UncompressedHeaderInfo* info) {
  const int profile = info->profile;
  info->bit_depth = profile >= 2 ? (r->ReadBit() ? 12 : 10) : 8;
  info->color_space = r->ReadLiteral(3);
  if (info->color_space != kVpxCsSrgb) {
    info->color_range = r->ReadBit();
    if (profile == 1 || profile == 3) {
      info->subsampling_x = r->ReadBit();
      info->subsampling_y = r->ReadBit();
      r->ReadBit();  // reserved, must be zero
    } else {
      info->subsampling_y = info->subsampling_x = 1;
    }
  } else {
    info->color_range = kVpxCrFullRange;
    if (profile == 1 || profile == 3) {
      info->subsampling_y = info->subsampling_x = 0;
      r->ReadBit();  // reserved, must be zero
    }
  }
}

// Consumes frame_size() and render_and_frame_size(). On success fills the
// explicit frame dimensions in |info|.
bool ReadFrameSize(Vp9BitReader* r, Vp9UncompressedHeaderInfo* info) {
  info->width = r->ReadLiteral(16) + 1;
  info->height = r->ReadLiteral(16) + 1;
  if (r->error())
    return false;
  if (r->ReadBit()) {  // render_and_frame_size_different
    info->display_width = r->ReadLiteral(16) + 1;
    info->display_height = r->ReadLiteral(16) + 1;
  } else {
    info->display_width = info->width;
    info->display_height = info->height;
  }
  return !r->error();
}

// Consumes interpolation_filter() and the frame context / loop filter level
// prologue shared by every non show-existing frame.
void ReadInterpolationAndContext(Vp9BitReader* r, bool key, bool intra_only,
                                 bool error_resilient) {
  // High precision MV and the interpolation filter selection exist only on
  // inter frames that are not error resilient.
  if (!key && !intra_only && !error_resilient) {
    if (!r->ReadBit())    // allow_high_precision_mv
      r->ReadLiteral(2);  // is_filter_switchable == 0: interpolation_filter
    if (r->error())
      return;
  }

  if (!error_resilient)
    r->ReadLiteral(2);  // frame_context_idx

  // loop_filter_params()
  r->ReadLiteral(6);  // filter_level
  r->ReadLiteral(3);  // sharpness_level
  if (r->ReadBit()) {  // mode_ref_delta_enabled
    if (r->ReadBit()) {  // mode_ref_delta_update
      int unused_delta = 0;
      for (int i = 0; i < 4; ++i) {  // ref_deltas
        if (r->ReadBit())
          r->ReadSigned(6, &unused_delta);
      }
      for (int i = 0; i < 2; ++i) {  // mode_deltas
        if (r->ReadBit())
          r->ReadSigned(6, &unused_delta);
      }
    }
  }
}

// Consumes quantization_params().
void ReadQuantization(Vp9BitReader* r) {
  r->ReadLiteral(8);  // base_q_idx
  int unused_delta = 0;
  for (int i = 0; i < 3; ++i) {
    if (r->ReadBit())  // delta_coded
      r->ReadSigned(4, &unused_delta);
  }
}

// Reads one segment feature data entry. Feature magnitude widths are 8, 6, 2
// and 0 bits; only the first two carry a sign bit, so a zero-width enabled
// feature consumes no value bits at all.
void ReadSegmentFeature(Vp9BitReader* r, int feature) {
  static const int kFeatureBits[kVp9SegmentFeatures] = {8, 6, 2, 0};
  static const bool kFeatureSigned[kVp9SegmentFeatures] = {true, true, false,
                                                           false};
  if (!r->ReadBit())  // feature_enabled
    return;
  const int width = kFeatureBits[feature];
  if (width == 0)
    return;  // Enabled zero-width features still consume no value bits.
  int value = 0;
  if (kFeatureSigned[feature])
    r->ReadSigned(width, &value);
  else
    value = r->ReadLiteral(width);
}

// Consumes segmentation_params() in full, including the map probabilities,
// temporal update probabilities and all eight segments' feature data.
void ReadSegmentation(Vp9BitReader* r) {
  if (!r->ReadBit())  // segmentation_enabled
    return;
  if (!r->ReadBit())  // segmentation_update_map
    return;

  for (int i = 0; i < 7; ++i) {
    if (r->ReadBit())  // segment_pred_prob[i] present
      r->ReadLiteral(8);
  }

  if (r->ReadBit()) {  // segmentation_temporal_update
    for (int i = 0; i < 3; ++i) {
      if (r->ReadBit())  // segment_pred_prob temporal
        r->ReadLiteral(8);
    }
  }

  if (!r->ReadBit())  // segmentation_update_data
    return;

  r->ReadBit();  // segmentation_abs_or_delta_update
  for (int segment = 0; segment < kVp9Segments; ++segment) {
    for (int feature = 0; feature < kVp9SegmentFeatures; ++feature)
      ReadSegmentFeature(r, feature);
  }
}

// Computes the legal tile column range from the frame width and reads
// tile_info(), filling the tile row/column counts in |info|.
bool ReadTileInfo(Vp9BitReader* r, Vp9UncompressedHeaderInfo* info) {
  const int aligned_width = AlignPowerOfTwo(info->width, kMiSizeLog2);
  const int mi_cols = aligned_width >> kMiSizeLog2;
  const int aligned_mi_cols = AlignPowerOfTwo(mi_cols, kMiSizeLog2);
  const int sb_cols = aligned_mi_cols >> 3;

  int max_log2 = 0;
  while ((sb_cols >> max_log2) >= kMinTileWidthB64)
    ++max_log2;
  --max_log2;
  if (max_log2 < 0)
    max_log2 = 0;

  int min_log2 = 0;
  while ((kMaxTileWidthB64 << min_log2) < sb_cols)
    ++min_log2;

  // Tile columns are encoded as a unary run of ones bounded by the legal
  // log2 range.
  int log2_tile_cols = min_log2;
  for (int extra = max_log2 - min_log2; extra > 0 && r->ReadBit(); --extra)
    ++log2_tile_cols;

  int log2_tile_rows = r->ReadBit();
  if (log2_tile_rows)
    log2_tile_rows += r->ReadBit();
  if (r->error())
    return false;

  info->tile_columns = 1 << log2_tile_cols;
  info->tile_rows = 1 << log2_tile_rows;
  return true;
}

}  // namespace

Vp9HeaderParseStatus ParseVp9UncompressedHeader(
    Vp9BitReader* reader, Vp9UncompressedHeaderInfo* info) {
  Vp9UncompressedHeaderInfo parsed;
  int reset_frame_context = 0;

  if (!ReadFrameTag(reader, &parsed, &reset_frame_context))
    return kVp9HeaderError;

  if (parsed.show_existing) {
    if (reader->error())
      return kVp9HeaderError;
    parsed.uncompressed_header_bytes = reader->bytes_consumed();
    *info = parsed;
    return kVp9HeaderOk;
  }

  if (parsed.key) {
    ReadColorConfig(reader, &parsed);
    if (reader->error() || !ReadFrameSize(reader, &parsed))
      return kVp9HeaderError;
  } else if (parsed.intra_only) {
    if (parsed.profile > 0) {
      ReadColorConfig(reader, &parsed);
    } else {
      // Profile 0 intra-only frames always use BT.601 studio range YUV 4:2:0.
      parsed.color_space = kVpxCsBt601;
      parsed.color_range = kVpxCrStudioRange;
      parsed.subsampling_y = parsed.subsampling_x = 1;
      parsed.bit_depth = 8;
    }
    parsed.refresh_frame_flags = reader->ReadLiteral(kRefFrames);
    if (reader->error() || !ReadFrameSize(reader, &parsed))
      return kVp9HeaderError;
  } else {
    parsed.refresh_frame_flags = reader->ReadLiteral(kRefFrames);
    for (int i = 0; i < kRefsPerFrame; ++i) {
      reader->ReadLiteral(kRefFramesLog2);  // ref_frame_idx
      reader->ReadBit();                    // ref_ref_sign_bias
    }

    bool found_reference = false;
    for (int i = 0; i < kRefsPerFrame; ++i) {
      if (reader->ReadBit()) {  // found_ref
        found_reference = true;
        break;
      }
    }
    if (reader->error())
      return kVp9HeaderError;
    if (found_reference) {
      // frame_size_with_refs: the resolution is inherited from reference
      // frames. No reference slot model is maintained, so report the partial
      // header and let the caller decide whether it can continue.
      *info = parsed;
      return kVp9HeaderReferenceSize;
    }
    if (!ReadFrameSize(reader, &parsed))
      return kVp9HeaderError;
  }

  // refresh_frame_context and frame_parallel_mode follow the frame size for
  // every frame that is not error resilient.
  if (!parsed.error_resilient) {
    reader->ReadBit();  // refresh_frame_context
    parsed.frame_parallel_mode = reader->ReadBit();
  } else {
    parsed.frame_parallel_mode = true;
  }

  ReadInterpolationAndContext(reader, parsed.key, parsed.intra_only,
                              parsed.error_resilient);
  ReadQuantization(reader);
  ReadSegmentation(reader);
  if (reader->error() || !ReadTileInfo(reader, &parsed))
    return kVp9HeaderError;

  // header_size_in_bytes is the last field of the uncompressed header.
  parsed.compressed_header_size =
      static_cast<std::size_t>(reader->ReadLiteral(16));
  if (!reader->ReadByteAlignment() || reader->error())
    return kVp9HeaderError;

  parsed.uncompressed_header_bytes = reader->bytes_consumed();
  *info = parsed;
  return kVp9HeaderOk;
}

}  // namespace vp9_parser
