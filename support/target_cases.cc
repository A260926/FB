// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.

// Target cases for VP9 tile range extraction. Every bitstream is assembled
// with a standalone bit writer below; expected offsets are recorded while
// writing, never produced by the production parser. There is deliberately no
// main(): support/driver.c provides the fixed registration driver.

#include "cases.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "common/libwebm_util.h"
#include "common/vp9_header_parser.h"
#include "common/vp9_level_stats.h"
#include "common/vp9_tiles.h"

namespace {

using vp9_parser::Vp9FrameLayout;
using vp9_parser::Vp9TileRange;

// MSB-first bit writer. Independent of the production bit reader.
class BitWriter {
 public:
  void Put(uint32_t value, unsigned bits) {
    for (int k = static_cast<int>(bits) - 1; k >= 0; --k) {
      if ((bit_count_ & 7u) == 0)
        bytes_.push_back(0);
      if ((value >> k) & 1u)
        bytes_.back() |= static_cast<uint8_t>(1u << (7 - (bit_count_ & 7u)));
      ++bit_count_;
    }
  }

  // Writes |magnitude_bits| value bits followed by one sign bit, matching the
  // VP9 signed literal syntax (sign bit set means negative).
  void PutSigned(int value, int magnitude_bits) {
    const int magnitude = value < 0 ? -value : value;
    Put(static_cast<uint32_t>(magnitude), magnitude_bits);
    Put(value < 0 ? 1u : 0u, 1);
  }

  void Align() {
    while (bit_count_ & 7u)
      Put(0, 1);
  }

  // Forces a one padding bit on purpose to exercise padding validation.
  void PutBadAlignment() {
    if (bit_count_ & 7u)
      Put(1, 1);
    Align();
  }

  void Byte(uint8_t value) {
    Align();
    Put(value, 8);
  }

  void AppendBytes(const std::vector<uint8_t>& data) {
    for (uint8_t b : data)
      Byte(b);
  }

  void Be32(uint32_t value) {
    Byte(static_cast<uint8_t>(value >> 24));
    Byte(static_cast<uint8_t>(value >> 16));
    Byte(static_cast<uint8_t>(value >> 8));
    Byte(static_cast<uint8_t>(value));
  }

  std::size_t size() const { return bytes_.size(); }
  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
  std::size_t bit_count_ = 0;
};

struct SegmentFeature {
  bool enabled = false;
  int value = 0;
};

struct FrameSpec {
  enum Kind {
    KEY,
    INTRA_ONLY,
    INTER_EXPLICIT_SIZE,
    INTER_WITH_REFS,
    SHOW_EXISTING
  } kind = KEY;

  int profile = 0;
  bool error_resilient = false;
  int width = 512;
  int height = 256;
  int log2_tile_columns = 0;
  int log2_tile_rows = 0;
  int existing_index = 0;
  bool render_different = false;
  bool bad_padding = false;

  // Frame context / parallel mode (non error-resilient frames only).
  bool refresh_frame_context = false;
  bool frame_parallel_mode = false;
  int frame_context_idx = 0;

  // Inter frame reference syntax.
  uint8_t refresh_frame_flags = 0;
  uint8_t ref_idx[3] = {0, 0, 0};
  bool ref_bias[3] = {false, false, false};
  bool found_ref[3] = {false, false, false};
  bool allow_high_precision_mv = false;
  int interpolation_filter = 0;

  // Loop filter deltas.
  bool lf_delta_enabled = false;
  bool lf_delta_update = false;
  bool lf_ref_present[4] = {false, false, false, false};
  int lf_ref_value[4] = {0, 0, 0, 0};
  bool lf_mode_present[2] = {false, false};
  int lf_mode_value[2] = {0, 0};

  // Quantizer deltas.
  bool q_present[3] = {false, false, false};
  int q_value[3] = {0, 0, 0};

  // Segmentation.
  bool segmentation_enabled = false;
  bool segmentation_update_map = false;
  bool segmentation_temporal_update = false;
  bool segmentation_update_data = false;
  bool segmentation_abs_or_delta = false;
  bool map_prob_present[7] = {false, false, false, false,
                             false, false, false};
  uint8_t map_prob[7] = {0, 0, 0, 0, 0, 0, 0};
  bool temporal_prob_present[3] = {false, false, false};
  uint8_t temporal_prob[3] = {0, 0, 0};
  SegmentFeature features[8][4];

  // Color config.
  bool ten_or_twelve_bit = false;
  int color_space = 1;  // BT.601
  int color_range = 0;
  int subsampling_x = 1;
  int subsampling_y = 1;

  // Compressed header: when empty a deterministic pattern of the requested
  // size is generated.
  std::size_t compressed_header_size = 1;
  std::vector<uint8_t> compressed_bytes;
  // When nonzero, the 16-bit header_size field carries this value instead of
  // the number of compressed bytes actually appended. Used to build a frame
  // whose declared compressed header runs past the end of the buffer.
  std::size_t force_header_size_field = 0;
};

struct BuiltFrame {
  std::vector<uint8_t> bytes;
  Vp9FrameLayout expected;
};

// Independent copy of the VP9 tile column legality calculation.
void TileColumnLog2Range(int width, int* min_log2, int* max_log2) {
  const int mi_cols = ((width + 7) & ~7) >> 3;
  const int sb_cols = ((mi_cols + 7) & ~7) >> 3;
  int mx = 0;
  while ((sb_cols >> mx) >= 4)
    ++mx;
  --mx;
  if (mx < 0)
    mx = 0;
  int mn = 0;
  while ((64 << mn) < sb_cols)
    ++mn;
  *min_log2 = mn;
  *max_log2 = mx;
}

std::vector<uint8_t> MakePattern(std::size_t length, uint8_t seed) {
  std::vector<uint8_t> out;
  out.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    // Keep every byte away from the 0xC0..0xDF superframe marker range when
    // it could land at a packet boundary.
    out.push_back(static_cast<uint8_t>(seed + static_cast<uint8_t>(i) & 0x3F));
  }
  return out;
}

void WriteColorConfig(BitWriter* w, const FrameSpec& spec) {
  if (spec.profile >= 2)
    w->Put(spec.ten_or_twelve_bit ? 1u : 0u, 1);
  w->Put(static_cast<uint32_t>(spec.color_space), 3);
  if (spec.color_space != 7) {
    w->Put(static_cast<uint32_t>(spec.color_range), 1);
    if (spec.profile == 1 || spec.profile == 3) {
      w->Put(static_cast<uint32_t>(spec.subsampling_x), 1);
      w->Put(static_cast<uint32_t>(spec.subsampling_y), 1);
      w->Put(0, 1);  // reserved zero
    }
  } else if (spec.profile == 1 || spec.profile == 3) {
    w->Put(0, 1);  // reserved zero
  }
}

void WriteLoopFilter(BitWriter* w, const FrameSpec& spec) {
  w->Put(12, 6);  // filter_level
  w->Put(2, 3);   // sharpness_level
  w->Put(spec.lf_delta_enabled ? 1u : 0u, 1);
  if (!spec.lf_delta_enabled)
    return;
  w->Put(spec.lf_delta_update ? 1u : 0u, 1);
  if (!spec.lf_delta_update)
    return;
  for (int i = 0; i < 4; ++i) {
    w->Put(spec.lf_ref_present[i] ? 1u : 0u, 1);
    if (spec.lf_ref_present[i])
      w->PutSigned(spec.lf_ref_value[i], 6);
  }
  for (int i = 0; i < 2; ++i) {
    w->Put(spec.lf_mode_present[i] ? 1u : 0u, 1);
    if (spec.lf_mode_present[i])
      w->PutSigned(spec.lf_mode_value[i], 6);
  }
}

void WriteQuantization(BitWriter* w, const FrameSpec& spec) {
  w->Put(100, 8);  // base_q_idx
  for (int i = 0; i < 3; ++i) {
    w->Put(spec.q_present[i] ? 1u : 0u, 1);
    if (spec.q_present[i])
      w->PutSigned(spec.q_value[i], 4);
  }
}

void WriteSegmentation(BitWriter* w, const FrameSpec& spec) {
  static const int kFeatureBits[4] = {8, 6, 2, 0};
  static const bool kFeatureSigned[4] = {true, true, false, false};

  w->Put(spec.segmentation_enabled ? 1u : 0u, 1);
  if (!spec.segmentation_enabled)
    return;
  w->Put(spec.segmentation_update_map ? 1u : 0u, 1);
  if (!spec.segmentation_update_map)
    return;

  for (int i = 0; i < 7; ++i) {
    w->Put(spec.map_prob_present[i] ? 1u : 0u, 1);
    if (spec.map_prob_present[i])
      w->Put(spec.map_prob[i], 8);
  }

  w->Put(spec.segmentation_temporal_update ? 1u : 0u, 1);
  if (spec.segmentation_temporal_update) {
    for (int i = 0; i < 3; ++i) {
      w->Put(spec.temporal_prob_present[i] ? 1u : 0u, 1);
      if (spec.temporal_prob_present[i])
        w->Put(spec.temporal_prob[i], 8);
    }
  }

  w->Put(spec.segmentation_update_data ? 1u : 0u, 1);
  if (!spec.segmentation_update_data)
    return;

  w->Put(spec.segmentation_abs_or_delta ? 1u : 0u, 1);
  for (int segment = 0; segment < 8; ++segment) {
    for (int feature = 0; feature < 4; ++feature) {
      const SegmentFeature& f = spec.features[segment][feature];
      w->Put(f.enabled ? 1u : 0u, 1);
      if (!f.enabled)
        continue;
      const int width = kFeatureBits[feature];
      if (width == 0)
        continue;  // Zero-width enabled features carry no value bits.
      if (kFeatureSigned[feature])
        w->PutSigned(f.value, width);
      else
        w->Put(static_cast<uint32_t>(f.value), width);
    }
  }
}

void WriteTileInfo(BitWriter* w, const FrameSpec& spec) {
  int min_log2 = 0;
  int max_log2 = 0;
  TileColumnLog2Range(spec.width, &min_log2, &max_log2);
  for (int i = min_log2; i < spec.log2_tile_columns; ++i)
    w->Put(1, 1);
  if (spec.log2_tile_columns < max_log2)
    w->Put(0, 1);

  if (spec.log2_tile_rows == 0) {
    w->Put(0, 1);
  } else {
    w->Put(1, 1);
    w->Put(spec.log2_tile_rows == 2 ? 1u : 0u, 1);
  }
}

// Builds one complete VP9 frame plus the layout recorded while writing.
// |tile_payloads| must already hold one opaque payload per tile.
BuiltFrame BuildFrame(const FrameSpec& spec,
                      const std::vector<std::vector<uint8_t> >& tile_payloads) {
  BitWriter w;
  BuiltFrame built;

  w.Put(2, 2);  // frame_marker
  // Profile encoding mirrors the spec: profile_low_bit, profile_high_bit,
  // then a reserved-zero third bit for profile 3.
  w.Put(static_cast<uint32_t>(spec.profile & 1), 1);
  w.Put(static_cast<uint32_t>((spec.profile >> 1) & 1), 1);
  if (spec.profile > 2)
    w.Put(0, 1);

  if (spec.kind == FrameSpec::SHOW_EXISTING) {
    w.Put(1, 1);  // show_existing_frame
    w.Put(static_cast<uint32_t>(spec.existing_index), 3);
    if (spec.bad_padding)
      w.PutBadAlignment();
    else
      w.Align();
    built.bytes = w.bytes();
    built.expected.offset = 0;
    built.expected.length = w.size();
    built.expected.show_existing = true;
    built.expected.existing_index = spec.existing_index;
    return built;
  }

  w.Put(0, 1);  // show_existing_frame = 0
  const bool is_key = spec.kind == FrameSpec::KEY;
  const bool is_intra = spec.kind == FrameSpec::INTRA_ONLY;
  const bool show_frame = !(is_intra);
  w.Put(is_key ? 0u : 1u, 1);          // frame_type
  w.Put(show_frame ? 1u : 0u, 1);      // show_frame
  w.Put(spec.error_resilient ? 1u : 0u, 1);

  if (is_key) {
    w.Put(0x49, 8);
    w.Put(0x83, 8);
    w.Put(0x42, 8);
    WriteColorConfig(&w, spec);
  } else if (is_intra) {
    // The intra-only flag exists only in error-resilient inter frames.
    w.Put(1, 1);  // intra_only
    w.Put(0x49, 8);
    w.Put(0x83, 8);
    w.Put(0x42, 8);
    if (spec.profile > 0)
      WriteColorConfig(&w, spec);
    w.Put(spec.refresh_frame_flags, 8);
  } else {
    if (!spec.error_resilient)
      w.Put(0, 2);  // reset_frame_context
    w.Put(spec.refresh_frame_flags, 8);
    for (int i = 0; i < 3; ++i) {
      w.Put(spec.ref_idx[i], 3);
      w.Put(spec.ref_bias[i] ? 1u : 0u, 1);
    }
    for (int i = 0; i < 3; ++i)
      w.Put(spec.found_ref[i] ? 1u : 0u, 1);
  }

  const bool uses_reference_size = spec.kind == FrameSpec::INTER_WITH_REFS;
  if (!uses_reference_size) {
    if (is_key || is_intra || spec.kind == FrameSpec::INTER_EXPLICIT_SIZE) {
      w.Put(static_cast<uint32_t>(spec.width - 1), 16);
      w.Put(static_cast<uint32_t>(spec.height - 1), 16);
      w.Put(spec.render_different ? 1u : 0u, 1);
      if (spec.render_different) {
        w.Put(static_cast<uint32_t>(spec.width - 1), 16);
        w.Put(static_cast<uint32_t>(spec.height - 1), 16);
      }
    }
  }

  if (uses_reference_size) {
    // A frame_size_with_refs header ends here; nothing layout-related follows.
    if (spec.bad_padding)
      w.PutBadAlignment();
    else
      w.Align();
    built.bytes = w.bytes();
    return built;
  }

  if (!spec.error_resilient) {
    w.Put(spec.refresh_frame_context ? 1u : 0u, 1);
    w.Put(spec.frame_parallel_mode ? 1u : 0u, 1);
  }

  if (!is_key && !is_intra && !spec.error_resilient) {
    w.Put(spec.allow_high_precision_mv ? 1u : 0u, 1);
    if (!spec.allow_high_precision_mv)
      w.Put(static_cast<uint32_t>(spec.interpolation_filter), 2);
  }

  if (!spec.error_resilient)
    w.Put(static_cast<uint32_t>(spec.frame_context_idx), 2);

  WriteLoopFilter(&w, spec);
  WriteQuantization(&w, spec);
  WriteSegmentation(&w, spec);
  WriteTileInfo(&w, spec);

  const std::vector<uint8_t> compressed =
      spec.compressed_bytes.empty()
          ? MakePattern(spec.compressed_header_size, 0x10)
          : spec.compressed_bytes;
  const std::size_t declared_compressed_size =
      spec.force_header_size_field != 0 ? spec.force_header_size_field
                                        : compressed.size();
  w.Put(static_cast<uint32_t>(declared_compressed_size), 16);
  if (spec.bad_padding)
    w.PutBadAlignment();
  else
    w.Align();

  const std::size_t uncompressed_bytes = w.size();
  w.AppendBytes(compressed);

  const int tile_columns = 1 << spec.log2_tile_columns;
  const int tile_rows = 1 << spec.log2_tile_rows;
  const int tile_count = tile_rows * tile_columns;

  Vp9FrameLayout& expected = built.expected;
  expected.tile_rows = tile_rows;
  expected.tile_columns = tile_columns;
  expected.width = spec.width;
  expected.height = spec.height;
  expected.uncompressed_header_bytes = uncompressed_bytes;
  expected.compressed_header_offset = uncompressed_bytes;
  expected.compressed_header_size = compressed.size();

  for (int tile = 0; tile < tile_count; ++tile) {
    const std::vector<uint8_t>& payload = tile_payloads[tile];
    if (tile < tile_count - 1)
      w.Be32(static_cast<uint32_t>(payload.size()));
    const std::size_t payload_offset = w.size();
    expected.tiles.push_back(
        Vp9TileRange(payload_offset, payload.size(),
                     tile / tile_columns, tile % tile_columns));
    w.AppendBytes(payload);
  }

  built.bytes = w.bytes();
  expected.offset = 0;
  expected.length = built.bytes.size();
  return built;
}

std::vector<std::vector<uint8_t> > MakeTiles(int count,
                                             std::size_t base_length) {
  std::vector<std::vector<uint8_t> > tiles;
  for (int i = 0; i < count; ++i)
    tiles.push_back(MakePattern(base_length + static_cast<std::size_t>(i),
                                static_cast<uint8_t>(0x20 + i)));
  return tiles;
}

struct BuiltPacket {
  std::vector<uint8_t> bytes;
  std::vector<Vp9FrameLayout> expected;
};

// Concatenates sub-frames and appends a superframe index. |forced_width| in
// 1..4 overrides the minimal length encoding (high bytes are simply zero);
// zero selects the minimal width.
BuiltPacket AssemblePacket(const std::vector<BuiltFrame>& children,
                           int forced_width) {
  BuiltPacket packet;
  std::vector<std::size_t> lengths;
  std::size_t offset = 0;
  for (std::size_t i = 0; i < children.size(); ++i) {
    Vp9FrameLayout expected = children[i].expected;
    const std::size_t base = offset;
    expected.offset += base;
    // Show-existing frames keep every layout range except the frame itself at
    // zero.
    if (!expected.show_existing) {
      expected.compressed_header_offset += base;
      for (std::size_t t = 0; t < expected.tiles.size(); ++t)
        expected.tiles[t].offset += base;
    }
    packet.expected.push_back(expected);
    packet.bytes.insert(packet.bytes.end(), children[i].bytes.begin(),
                        children[i].bytes.end());
    lengths.push_back(children[i].bytes.size());
    offset += children[i].bytes.size();
  }

  std::size_t max_length = 0;
  for (std::size_t length : lengths)
    max_length = length > max_length ? length : max_length;
  int width = forced_width;
  if (width == 0) {
    width = 1;
    std::size_t limit = 0xFF;
    while (max_length > limit) {
      ++width;
      limit = (limit << 8) | 0xFF;
    }
  }

  const uint8_t marker = static_cast<uint8_t>(
      0xC0 | static_cast<uint8_t>((width - 1) << 3) |
      static_cast<uint8_t>(children.size() - 1));
  packet.bytes.push_back(marker);
  for (std::size_t length : lengths) {
    for (int byte = 0; byte < width; ++byte)
      packet.bytes.push_back(
          static_cast<uint8_t>((length >> (byte * 8)) & 0xFF));
  }
  packet.bytes.push_back(marker);
  return packet;
}

void CheckTile(const Vp9TileRange& got, const Vp9TileRange& expected) {
  TASK_CHECK(got.offset == expected.offset);
  TASK_CHECK(got.length == expected.length);
  TASK_CHECK(got.row == expected.row);
  TASK_CHECK(got.column == expected.column);
  TASK_CHECK(got.length > 0);
}

void CheckFrame(const Vp9FrameLayout& got, const Vp9FrameLayout& expected) {
  TASK_CHECK(got.offset == expected.offset);
  TASK_CHECK(got.length == expected.length);
  TASK_CHECK(got.uncompressed_header_bytes ==
             expected.uncompressed_header_bytes);
  TASK_CHECK(got.compressed_header_offset ==
             expected.compressed_header_offset);
  TASK_CHECK(got.compressed_header_size == expected.compressed_header_size);
  TASK_CHECK(got.width == expected.width);
  TASK_CHECK(got.height == expected.height);
  TASK_CHECK(got.tile_rows == expected.tile_rows);
  TASK_CHECK(got.tile_columns == expected.tile_columns);
  TASK_CHECK(got.show_existing == expected.show_existing);
  TASK_CHECK(got.existing_index == expected.existing_index);
  TASK_CHECK(got.tiles.size() == expected.tiles.size());
  for (std::size_t i = 0; i < expected.tiles.size(); ++i)
    CheckTile(got.tiles[i], expected.tiles[i]);
}

bool Extract(const std::vector<uint8_t>& packet,
             std::vector<Vp9FrameLayout>* out) {
  return vp9_parser::ExtractVP9Tiles(packet.data(), packet.size(), out);
}

Vp9FrameLayout SentinelLayout(std::size_t tag) {
  Vp9FrameLayout layout;
  layout.offset = tag;
  layout.length = tag + 1;
  layout.uncompressed_header_bytes = tag + 2;
  layout.compressed_header_offset = tag + 3;
  layout.compressed_header_size = tag + 4;
  layout.width = static_cast<int>(tag + 5);
  layout.height = static_cast<int>(tag + 6);
  layout.tile_rows = 7;
  layout.tile_columns = 8;
  layout.show_existing = true;
  layout.existing_index = static_cast<int>(tag & 7);
  layout.tiles.push_back(Vp9TileRange(tag + 9, 10, 1, 2));
  return layout;
}

// ---------------------------------------------------------------------------
// headers group: uncompressed header syntax coverage.
// ---------------------------------------------------------------------------

TASK_CASE("headers", headers_segmentation_map_and_temporal) {
  FrameSpec spec;
  spec.kind = FrameSpec::KEY;
  spec.width = 512;
  spec.height = 256;
  spec.log2_tile_columns = 1;
  spec.log2_tile_rows = 2;
  spec.segmentation_enabled = true;
  spec.segmentation_update_map = true;
  for (int i = 0; i < 7; ++i) {
    spec.map_prob_present[i] = (i % 2 == 0);
    spec.map_prob[i] = static_cast<uint8_t>(20 + i);
  }
  spec.segmentation_temporal_update = true;
  for (int i = 0; i < 3; ++i) {
    spec.temporal_prob_present[i] = (i != 1);
    spec.temporal_prob[i] = static_cast<uint8_t>(100 + i);
  }
  // Map update without a data update must also be consumed correctly.
  spec.segmentation_update_data = false;

  const BuiltFrame frame = BuildFrame(spec, MakeTiles(8, 3));
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(frame.bytes, &layouts));
  TASK_CHECK(layouts.size() == 1);
  CheckFrame(layouts[0], frame.expected);
}

TASK_CASE("headers", headers_segment_feature_widths_and_signs) {
  FrameSpec spec;
  spec.kind = FrameSpec::KEY;
  spec.width = 640;
  spec.height = 360;
  spec.segmentation_enabled = true;
  spec.segmentation_update_map = true;
  spec.segmentation_update_data = true;
  spec.segmentation_abs_or_delta = true;

  // Feature widths are 8, 6, 2, 0; only the first two are signed.
  spec.features[0][0].enabled = true;
  spec.features[0][0].value = 200;   // 8-bit signed, positive
  spec.features[1][0].enabled = true;
  spec.features[1][0].value = -200;  // 8-bit signed, negative
  spec.features[2][0].enabled = true;
  spec.features[2][0].value = 0;     // signed zero still carries a sign bit
  spec.features[3][1].enabled = true;
  spec.features[3][1].value = 31;    // 6-bit signed, positive
  spec.features[4][1].enabled = true;
  spec.features[4][1].value = -31;   // 6-bit signed, negative
  spec.features[5][1].enabled = true;
  spec.features[5][1].value = 0;
  spec.features[6][2].enabled = true;
  spec.features[6][2].value = 3;     // 2-bit unsigned maximum
  spec.features[7][2].enabled = true;
  spec.features[7][2].value = 0;     // unsigned zero
  spec.features[0][3].enabled = true;  // zero-width enabled feature
  spec.features[7][3].enabled = true;

  const BuiltFrame frame = BuildFrame(spec, MakeTiles(1, 5));
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(frame.bytes, &layouts));
  TASK_CHECK(layouts.size() == 1);
  CheckFrame(layouts[0], frame.expected);
}

TASK_CASE("headers", headers_all_profiles) {
  for (int profile = 0; profile <= 3; ++profile) {
    FrameSpec spec;
    spec.kind = FrameSpec::KEY;
    spec.profile = profile;
    spec.width = 512;
    spec.height = 256;
    if (profile >= 2)
      spec.ten_or_twelve_bit = (profile == 3);
    if (profile == 1 || profile == 3) {
      spec.subsampling_x = profile == 1 ? 1 : 0;
      spec.subsampling_y = profile == 1 ? 1 : 0;
    }
    const BuiltFrame frame = BuildFrame(spec, MakeTiles(1, 2));
    std::vector<Vp9FrameLayout> layouts;
    TASK_CHECK(Extract(frame.bytes, &layouts));
    TASK_CHECK(layouts.size() == 1);
    TASK_CHECK(layouts[0].width == 512);
    TASK_CHECK(layouts[0].height == 256);
    CheckFrame(layouts[0], frame.expected);
  }
}

TASK_CASE("headers", headers_srgb_color_config) {
  FrameSpec spec;
  spec.kind = FrameSpec::KEY;
  spec.profile = 1;
  spec.color_space = 7;  // sRGB skips color range and drives fixed defaults.
  const BuiltFrame frame = BuildFrame(spec, MakeTiles(1, 2));
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(frame.bytes, &layouts));
  CheckFrame(layouts[0], frame.expected);
}

TASK_CASE("headers", headers_error_resilient_combos) {
  for (int resilient = 0; resilient <= 1; ++resilient) {
    FrameSpec key_spec;
    key_spec.kind = FrameSpec::KEY;
    key_spec.error_resilient = resilient != 0;
    key_spec.frame_parallel_mode = resilient == 0;
    const BuiltFrame key_frame = BuildFrame(key_spec, MakeTiles(1, 2));
    std::vector<Vp9FrameLayout> layouts;
    TASK_CHECK(Extract(key_frame.bytes, &layouts));
    CheckFrame(layouts[0], key_frame.expected);
  }

  // Intra-only frames only exist in error-resilient mode.
  FrameSpec intra_spec;
  intra_spec.kind = FrameSpec::INTRA_ONLY;
  intra_spec.error_resilient = true;
  intra_spec.refresh_frame_flags = 0x5A;
  const BuiltFrame intra_frame = BuildFrame(intra_spec, MakeTiles(1, 2));
  std::vector<Vp9FrameLayout> intra_layouts;
  TASK_CHECK(Extract(intra_frame.bytes, &intra_layouts));
  CheckFrame(intra_layouts[0], intra_frame.expected);

  // A non error-resilient explicit-size inter frame carries reset context,
  // high-precision-MV/interpolation and frame-context syntax instead.
  FrameSpec inter_spec;
  inter_spec.kind = FrameSpec::INTER_EXPLICIT_SIZE;
  inter_spec.error_resilient = false;
  inter_spec.allow_high_precision_mv = true;
  const BuiltFrame inter_frame = BuildFrame(inter_spec, MakeTiles(1, 2));
  std::vector<Vp9FrameLayout> inter_layouts;
  TASK_CHECK(Extract(inter_frame.bytes, &inter_layouts));
  CheckFrame(inter_layouts[0], inter_frame.expected);
}

TASK_CASE("headers", headers_explicit_size_inter) {
  FrameSpec spec;
  spec.kind = FrameSpec::INTER_EXPLICIT_SIZE;
  spec.width = 1280;
  spec.height = 720;
  spec.log2_tile_columns = 1;
  spec.refresh_frame_flags = 0x81;
  spec.ref_idx[0] = 1;
  spec.ref_bias[0] = true;
  spec.ref_idx[1] = 4;
  spec.ref_idx[2] = 7;
  spec.found_ref[0] = false;
  spec.found_ref[1] = false;
  spec.found_ref[2] = false;
  spec.allow_high_precision_mv = false;
  spec.interpolation_filter = 2;
  spec.frame_context_idx = 3;
  spec.q_present[0] = true;
  spec.q_value[0] = -7;
  spec.q_present[2] = true;
  spec.q_value[2] = 9;
  spec.lf_delta_enabled = true;
  spec.lf_delta_update = true;
  spec.lf_ref_present[2] = true;
  spec.lf_ref_value[2] = -15;
  spec.lf_mode_present[1] = true;
  spec.lf_mode_value[1] = 20;

  const BuiltFrame frame = BuildFrame(spec, MakeTiles(2, 4));
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(frame.bytes, &layouts));
  TASK_CHECK(layouts.size() == 1);
  TASK_CHECK(layouts[0].width == 1280);
  TASK_CHECK(layouts[0].tile_columns == 2);
  CheckFrame(layouts[0], frame.expected);
}

TASK_CASE("headers", headers_inter_with_refs_rejected) {
  FrameSpec spec;
  spec.kind = FrameSpec::INTER_WITH_REFS;
  spec.found_ref[1] = true;
  const BuiltFrame frame = BuildFrame(spec, MakeTiles(1, 2));

  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(!Extract(frame.bytes, &layouts));

  // The legacy entry point keeps accepting the historically accepted input.
  vp9_parser::Vp9HeaderParser parser;
  TASK_CHECK(parser.ParseUncompressedHeader(frame.bytes.data(),
                                            frame.bytes.size()));
}

TASK_CASE("headers", headers_show_existing) {
  FrameSpec spec;
  spec.kind = FrameSpec::SHOW_EXISTING;
  spec.existing_index = 5;
  const BuiltFrame frame = BuildFrame(spec, {});
  TASK_CHECK(frame.bytes.size() == 1);  // Exactly one byte for profile 0.

  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(frame.bytes, &layouts));
  TASK_CHECK(layouts.size() == 1);
  const Vp9FrameLayout& got = layouts[0];
  TASK_CHECK(got.show_existing);
  TASK_CHECK(got.existing_index == 5);
  TASK_CHECK(got.offset == 0);
  TASK_CHECK(got.length == 1);
  TASK_CHECK(got.width == 0);
  TASK_CHECK(got.height == 0);
  TASK_CHECK(got.uncompressed_header_bytes == 0);
  TASK_CHECK(got.compressed_header_size == 0);
  TASK_CHECK(got.tiles.empty());
}

TASK_CASE("headers", headers_rejected_marker_sync_padding) {
  std::vector<uint8_t> bad;

  FrameSpec marker_spec;
  BuiltFrame marker_frame = BuildFrame(marker_spec, MakeTiles(1, 1));
  bad = marker_frame.bytes;
  bad[0] = static_cast<uint8_t>(bad[0] ^ 0xC0);  // Destroy frame marker.
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(!Extract(bad, &layouts));

  FrameSpec sync_spec;
  BuiltFrame sync_frame = BuildFrame(sync_spec, MakeTiles(1, 1));
  bad = sync_frame.bytes;
  bad[2] ^= 0x01;  // Corrupt the sync code.
  TASK_CHECK(!Extract(bad, &layouts));

  FrameSpec padding_spec;
  padding_spec.bad_padding = true;
  const BuiltFrame padding_frame = BuildFrame(padding_spec, MakeTiles(1, 1));
  TASK_CHECK(!Extract(padding_frame.bytes, &layouts));

  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(nullptr, 4, &layouts));
  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(sync_frame.bytes.data(), 0,
                                          &layouts));
}

// ---------------------------------------------------------------------------
// tiles group: tile payload splitting.
// ---------------------------------------------------------------------------

TASK_CASE("tiles", tiles_single_tile_has_no_prefix) {
  FrameSpec spec;
  spec.compressed_header_size = 3;
  const BuiltFrame frame = BuildFrame(spec, MakeTiles(1, 12));
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(frame.bytes, &layouts));
  CheckFrame(layouts[0], frame.expected);
  TASK_CHECK(layouts[0].tiles.size() == 1);
  // The single tile starts right after the compressed header and consumes the
  // whole remainder.
  TASK_CHECK(layouts[0].tiles[0].offset ==
             layouts[0].compressed_header_offset +
                 layouts[0].compressed_header_size);
  TASK_CHECK(layouts[0].tiles[0].offset + layouts[0].tiles[0].length ==
             frame.bytes.size());
}

TASK_CASE("tiles", tiles_multi_row_multi_column) {
  FrameSpec spec;
  spec.width = 1920;
  spec.height = 1080;
  spec.log2_tile_columns = 2;  // 4 columns
  spec.log2_tile_rows = 2;     // 4 rows -> 16 tiles
  const BuiltFrame frame = BuildFrame(spec, MakeTiles(16, 6));
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(frame.bytes, &layouts));
  CheckFrame(layouts[0], frame.expected);

  // Row-major ordering and correct row/column labels.
  for (int i = 0; i < 16; ++i) {
    TASK_CHECK(layouts[0].tiles[i].row == i / 4);
    TASK_CHECK(layouts[0].tiles[i].column == i % 4);
  }
}

TASK_CASE("tiles", tiles_unequal_payloads) {
  FrameSpec spec;
  spec.width = 1280;
  spec.height = 720;
  spec.log2_tile_columns = 2;  // 4 columns, 1 row
  std::vector<std::vector<uint8_t> > payloads;
  const std::size_t lengths[4] = {1, 17, 250, 999};
  for (int i = 0; i < 4; ++i)
    payloads.push_back(MakePattern(lengths[i], 0x30));
  const BuiltFrame frame = BuildFrame(spec, payloads);
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(frame.bytes, &layouts));
  CheckFrame(layouts[0], frame.expected);
  TASK_CHECK(layouts[0].tiles[3].length == 999);
}

TASK_CASE("tiles", tiles_opaque_compressed_header_length_bytes) {
  FrameSpec spec;
  spec.width = 1280;
  spec.height = 720;
  spec.log2_tile_columns = 1;
  // Bytes that look like huge big-endian tile lengths must be ignored: the
  // compressed header is skipped by size, never scanned.
  spec.compressed_bytes = std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF,
                                               0x00, 0x12, 0x34, 0x56};
  spec.compressed_header_size = spec.compressed_bytes.size();
  const BuiltFrame frame = BuildFrame(spec, MakeTiles(2, 7));
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(frame.bytes, &layouts));
  CheckFrame(layouts[0], frame.expected);
  TASK_CHECK(layouts[0].compressed_header_size == 8);
}

TASK_CASE("tiles", tiles_byte_by_byte_truncation) {
  FrameSpec spec;
  spec.width = 512;
  spec.height = 256;
  spec.log2_tile_columns = 1;
  spec.log2_tile_rows = 1;  // 4 tiles, final tile is exactly one byte.
  std::vector<std::vector<uint8_t> > payloads;
  payloads.push_back(MakePattern(5, 0x20));
  payloads.push_back(MakePattern(6, 0x21));
  payloads.push_back(MakePattern(7, 0x22));
  payloads.push_back(MakePattern(1, 0x23));
  const BuiltFrame frame = BuildFrame(spec, payloads);
  std::vector<Vp9FrameLayout> full;
  TASK_CHECK(Extract(frame.bytes, &full));  // Sanity: full frame parses.
  TASK_CHECK(full[0].tiles.back().length == 1);

  // The final tile is exactly one byte, so no cut can leave a shortened but
  // still parseable final tile: every strict prefix must fail while only the
  // complete buffer succeeds.
  for (std::size_t cut = 1; cut < frame.bytes.size(); ++cut) {
    std::vector<Vp9FrameLayout> layouts;
    const bool ok = vp9_parser::ExtractVP9Tiles(frame.bytes.data(), cut,
                                                &layouts);
    TASK_CHECK(!ok);
  }
}

TASK_CASE("tiles", tiles_bad_prefix_and_empty_tiles) {
  FrameSpec spec;
  spec.log2_tile_columns = 1;  // 2 tiles
  const BuiltFrame frame = BuildFrame(spec, MakeTiles(2, 4));

  // Declare an oversized first tile.
  std::vector<uint8_t> overlong = frame.bytes;
  const std::size_t prefix = frame.expected.tiles[0].offset - 4;
  overlong[prefix] = 0x7F;
  overlong[prefix + 1] = 0xFF;
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(!Extract(overlong, &layouts));

  // Declare an empty first tile.
  std::vector<uint8_t> empty = frame.bytes;
  empty[prefix] = empty[prefix + 1] = empty[prefix + 2] =
      empty[prefix + 3] = 0;
  TASK_CHECK(!Extract(empty, &layouts));

  // Remove the prefix bytes entirely.
  std::vector<uint8_t> missing;
  missing.insert(missing.end(), frame.bytes.begin(),
                 frame.bytes.begin() + static_cast<std::ptrdiff_t>(prefix));
  missing.insert(missing.end(),
                 frame.bytes.begin() +
                     static_cast<std::ptrdiff_t>(prefix + 4),
                 frame.bytes.end());
  TASK_CHECK(!Extract(missing, &layouts));
}

TASK_CASE("tiles", tiles_compressed_header_out_of_bounds) {
  FrameSpec spec;
  // The frame contains only one compressed byte plus a tile, but the 16-bit
  // header field claims the maximum compressed header length.
  spec.compressed_header_size = 1;
  spec.force_header_size_field = 65535;
  const BuiltFrame frame = BuildFrame(spec, MakeTiles(1, 4));
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(!Extract(frame.bytes, &layouts));
}

TASK_CASE("tiles", tiles_coverage_is_complete_and_disjoint) {
  FrameSpec spec;
  spec.width = 1920;
  spec.height = 1080;
  spec.log2_tile_columns = 1;
  spec.log2_tile_rows = 1;  // 4 tiles
  const BuiltFrame frame = BuildFrame(spec, MakeTiles(4, 11));
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(frame.bytes, &layouts));
  const Vp9FrameLayout& got = layouts[0];

  std::size_t cursor = got.compressed_header_offset +
                       got.compressed_header_size;
  for (std::size_t i = 0; i < got.tiles.size(); ++i) {
    if (i + 1 < got.tiles.size()) {
      TASK_CHECK(got.tiles[i].offset == cursor + 4);  // Skip prefix.
      cursor += 4;
    } else {
      TASK_CHECK(got.tiles[i].offset == cursor);
    }
    TASK_CHECK(got.tiles[i].length > 0);
    cursor += got.tiles[i].length;
  }
  TASK_CHECK(cursor == frame.bytes.size());

  // Compressed header bytes are never part of a tile.
  for (const Vp9TileRange& tile : got.tiles)
    TASK_CHECK(tile.offset >=
               got.compressed_header_offset + got.compressed_header_size);
}

// ---------------------------------------------------------------------------
// packets group: superframe index handling and end-to-end integration.
// ---------------------------------------------------------------------------

TASK_CASE("packets", packets_superframe_mixed_show_existing) {
  FrameSpec show_spec;
  show_spec.kind = FrameSpec::SHOW_EXISTING;
  show_spec.existing_index = 2;
  const BuiltFrame show_frame = BuildFrame(show_spec, {});

  FrameSpec key_spec;
  key_spec.width = 512;
  key_spec.height = 256;
  key_spec.log2_tile_columns = 1;
  const BuiltFrame key_frame = BuildFrame(key_spec, MakeTiles(2, 5));

  FrameSpec intra_spec;
  intra_spec.kind = FrameSpec::INTRA_ONLY;
  intra_spec.error_resilient = true;
  const BuiltFrame intra_frame = BuildFrame(intra_spec, MakeTiles(1, 3));

  const BuiltPacket packet =
      AssemblePacket({show_frame, key_frame, intra_frame}, 1);
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(packet.bytes, &layouts));
  TASK_CHECK(layouts.size() == 3);
  for (std::size_t i = 0; i < 3; ++i)
    CheckFrame(layouts[i], packet.expected[i]);
  TASK_CHECK(layouts[0].show_existing);
  TASK_CHECK(!layouts[1].show_existing);
}

TASK_CASE("packets", packets_all_index_widths) {
  FrameSpec spec;
  spec.log2_tile_columns = 1;
  const BuiltFrame first = BuildFrame(spec, MakeTiles(2, 4));
  const BuiltFrame second = BuildFrame(spec, MakeTiles(2, 8));

  for (int width = 1; width <= 4; ++width) {
    const BuiltPacket packet = AssemblePacket({first, second}, width);
    std::vector<Vp9FrameLayout> layouts;
    TASK_CHECK(Extract(packet.bytes, &layouts));
    TASK_CHECK(layouts.size() == 2);
    CheckFrame(layouts[0], packet.expected[0]);
    CheckFrame(layouts[1], packet.expected[1]);
  }

  // A naturally selected two-byte width needs a child longer than 255 bytes.
  FrameSpec large_spec;
  const BuiltFrame large = BuildFrame(large_spec, MakeTiles(1, 300));
  const BuiltPacket wide_packet = AssemblePacket({large}, 0);
  TASK_CHECK(wide_packet.bytes[wide_packet.bytes.size() - 1] ==
             static_cast<uint8_t>(0xC8));  // width 2, one child.
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(wide_packet.bytes, &layouts));
  CheckFrame(layouts[0], wide_packet.expected[0]);
}

TASK_CASE("packets", packets_eight_subframes_and_boundaries) {
  std::vector<BuiltFrame> children;
  for (int i = 0; i < 8; ++i) {
    FrameSpec spec;
    spec.existing_index = i & 7;
    spec.kind = (i % 2 == 0) ? FrameSpec::SHOW_EXISTING : FrameSpec::KEY;
    children.push_back(BuildFrame(spec, MakeTiles(1, 2 + i)));
  }
  const BuiltPacket packet = AssemblePacket(children, 1);
  TASK_CHECK(packet.bytes.back() == static_cast<uint8_t>(0xC7));  // 8 frames.

  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(Extract(packet.bytes, &layouts));
  TASK_CHECK(layouts.size() == 8);
  for (std::size_t i = 0; i < 8; ++i) {
    CheckFrame(layouts[i], packet.expected[i]);
    if (i > 0) {
      // Adjacent sub-frames share an exact boundary.
      TASK_CHECK(layouts[i - 1].offset + layouts[i - 1].length ==
                 layouts[i].offset);
    }
  }
  TASK_CHECK(layouts[7].offset + layouts[7].length <=
             packet.bytes.size());
  // Payload of all children ends exactly where the index begins. The index is
  // two marker bytes plus one length byte per child here.
  const std::size_t index_size = 2 + 8;
  TASK_CHECK(layouts[7].offset + layouts[7].length + index_size ==
             packet.bytes.size());
}

TASK_CASE("packets", packets_corrupt_index_fails_without_fallback) {
  FrameSpec spec;
  spec.log2_tile_columns = 1;
  const BuiltFrame first = BuildFrame(spec, MakeTiles(2, 4));
  const BuiltFrame second = BuildFrame(spec, MakeTiles(2, 6));
  const BuiltPacket packet = AssemblePacket({first, second}, 1);

  // Corrupt the opening marker of the index.
  std::vector<uint8_t> bad_marker = packet.bytes;
  const std::size_t index_start = bad_marker.size() - 4;
  bad_marker[index_start] ^= 0x01;
  std::vector<Vp9FrameLayout> layouts;
  TASK_CHECK(!Extract(bad_marker, &layouts));

  // Overstate a sub-frame length so the declared total no longer matches.
  std::vector<uint8_t> bad_length = packet.bytes;
  bad_length[index_start + 1] = 0xFE;
  TASK_CHECK(!Extract(bad_length, &layouts));

  // Drop one payload byte while keeping the index intact: the trailing marker
  // still forces index parsing, but the declared sub-frame total no longer
  // matches the (shortened) payload, so it must fail rather than fall back to
  // single-frame parsing.
  std::vector<uint8_t> short_payload(packet.bytes.begin() + 1,
                                     packet.bytes.end());
  TASK_CHECK(!Extract(short_payload, &layouts));

  // A trailing marker whose index cannot physically fit in the packet.
  std::vector<uint8_t> tiny_marker{0xC0};
  TASK_CHECK(!Extract(tiny_marker, &layouts));
  std::vector<uint8_t> wide_marker{0x00, 0x01, 0xCF};
  TASK_CHECK(!Extract(wide_marker, &layouts));

  // A marker byte on random garbage must not be treated as a single frame.
  std::vector<uint8_t> garbage{0x00, 0x11, 0x22, 0xC1};
  TASK_CHECK(!Extract(garbage, &layouts));
}

TASK_CASE("packets", packets_output_sentinel_and_input_preserved) {
  std::vector<Vp9FrameLayout> sentinel;
  sentinel.push_back(SentinelLayout(100));
  sentinel.push_back(SentinelLayout(200));
  const std::vector<Vp9FrameLayout> saved = sentinel;

  FrameSpec spec;
  spec.kind = FrameSpec::INTER_WITH_REFS;
  spec.found_ref[0] = true;
  const BuiltFrame rejected = BuildFrame(spec, MakeTiles(1, 2));

  FrameSpec bad_spec;
  bad_spec.force_header_size_field = 60000;
  const BuiltFrame oversized = BuildFrame(bad_spec, MakeTiles(1, 2));

  std::vector<uint8_t> garbage{0x55, 0xAA, 0xC2};
  const std::vector<uint8_t> rejected_copy = rejected.bytes;
  const std::vector<uint8_t> oversized_copy = oversized.bytes;

  TASK_CHECK(!Extract(rejected.bytes, &sentinel));
  TASK_CHECK(!Extract(oversized.bytes, &sentinel));
  TASK_CHECK(!Extract(garbage, &sentinel));
  TASK_CHECK(sentinel.size() == saved.size());
  for (std::size_t i = 0; i < saved.size(); ++i)
    CheckFrame(sentinel[i], saved[i]);

  // Failed calls never modify the input.
  TASK_CHECK(std::memcmp(rejected.bytes.data(), rejected_copy.data(),
                         rejected_copy.size()) == 0);
  TASK_CHECK(std::memcmp(oversized.bytes.data(), oversized_copy.data(),
                         oversized_copy.size()) == 0);

  // A successful call replaces the sentinel rather than appending to it.
  const BuiltFrame good = BuildFrame(FrameSpec(), MakeTiles(1, 3));
  TASK_CHECK(Extract(good.bytes, &sentinel));
  TASK_CHECK(sentinel.size() == 1);
  TASK_CHECK(!sentinel[0].show_existing);
}

TASK_CASE("packets", packets_level_stats_max_column_tiles) {
  FrameSpec spec;
  spec.kind = FrameSpec::KEY;
  spec.width = 1920;
  spec.height = 1080;
  spec.log2_tile_columns = 2;  // 4 columns
  spec.log2_tile_rows = 1;     // 2 rows
  spec.segmentation_enabled = true;
  spec.segmentation_update_map = true;
  spec.segmentation_temporal_update = true;
  spec.segmentation_update_data = true;
  spec.features[0][0].enabled = true;
  spec.features[0][0].value = 17;
  const BuiltFrame frame = BuildFrame(spec, MakeTiles(8, 9));

  vp9_parser::Vp9HeaderParser parser;
  TASK_CHECK(parser.ParseUncompressedHeader(frame.bytes.data(),
                                            frame.bytes.size()));
  TASK_CHECK(parser.column_tiles() == 4);
  TASK_CHECK(parser.row_tiles() == 2);
  TASK_CHECK(parser.width() == 1920);

  vp9_parser::Vp9LevelStats stats;
  stats.AddFrame(parser, 0);
  stats.AddFrame(parser, 33333333);
  TASK_CHECK(stats.GetMaxColumnTiles() == 4);

  // The same corrected parser also reports the widest legal layout. Eight
  // columns need at least 2048 pixels of width (minimum 256px tile width), so
  // use a 4096 pixel frame.
  FrameSpec wide_spec;
  wide_spec.width = 4096;
  wide_spec.height = 2160;
  wide_spec.log2_tile_columns = 3;  // 8 columns
  const BuiltFrame wide = BuildFrame(wide_spec, MakeTiles(8, 5));
  vp9_parser::Vp9HeaderParser wide_parser;
  TASK_CHECK(wide_parser.ParseUncompressedHeader(wide.bytes.data(),
                                                 wide.bytes.size()));
  TASK_CHECK(wide_parser.column_tiles() == 8);
}

TASK_CASE("packets", packets_legacy_plain_header_and_index) {
  // Plain (non segmented) header still parses through the legacy entry point.
  FrameSpec spec;
  spec.width = 512;
  spec.height = 256;
  spec.log2_tile_columns = 1;
  spec.log2_tile_rows = 2;
  const BuiltFrame frame = BuildFrame(spec, MakeTiles(8, 2));
  vp9_parser::Vp9HeaderParser parser;
  TASK_CHECK(parser.ParseUncompressedHeader(frame.bytes.data(),
                                            frame.bytes.size()));
  TASK_CHECK(parser.width() == 512);
  TASK_CHECK(parser.height() == 256);
  TASK_CHECK(parser.column_tiles() == 2);
  TASK_CHECK(parser.row_tiles() == 4);

  // The existing superframe index parser keeps its documented behavior.
  const uint8_t indexed[] = {1, 2, 3, 4, 5, 6, 0xC0, 6, 0xC0};
  libwebm::Ranges ranges;
  bool error = false;
  TASK_CHECK(libwebm::ParseVP9SuperFrameIndex(indexed, sizeof(indexed),
                                              &ranges, &error));
  TASK_CHECK(!error);
  TASK_CHECK(ranges.size() == 1);
  TASK_CHECK(ranges[0].offset == 0);
  TASK_CHECK(ranges[0].length == 6);
}

}  // namespace
