// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
//
// Target cases for ExtractVP9Tiles(). Packets are assembled with a small
// independent C++ bit writer; expected offsets come from the layout recorded
// while writing, never from the production parser.

#include "cases.h"

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "common/vp9_header_parser.h"
#include "common/vp9_level_stats.h"
#include "common/vp9_tiles.h"

namespace {

// ---------------------------------------------------------------------------
// Bit writer.
// ---------------------------------------------------------------------------

class BitWriter {
 public:
  void Put(uint32_t value, int bits) {
    for (int bit = bits - 1; bit >= 0; --bit) {
      if ((bit_count_ & 7) == 0) bytes_.push_back(0);
      const uint32_t one = (value >> bit) & 1u;
      bytes_.back() |= static_cast<unsigned char>(one << (7 - (bit_count_ & 7)));
      ++bit_count_;
    }
  }

  // Magnitude bits followed by a sign bit, which is present even for zero.
  void PutSigned(int magnitude_bits, int value) {
    const uint32_t magnitude =
        value < 0 ? static_cast<uint32_t>(-value) : static_cast<uint32_t>(value);
    Put(magnitude, magnitude_bits);
    Put(value < 0 ? 1 : 0, 1);
  }

  void AlignZero() {
    while ((bit_count_ & 7) != 0) Put(0, 1);
  }

  size_t size() const { return bytes_.size(); }
  const std::vector<unsigned char>& data() const { return bytes_; }

 private:
  std::vector<unsigned char> bytes_;
  size_t bit_count_ = 0;
};

// ---------------------------------------------------------------------------
// Header description.
// ---------------------------------------------------------------------------

struct SegmentFeature {
  bool enabled = false;
  int value = 0;  // Signed for features 0/1, unsigned for feature 2.
};

struct SegmentationSpec {
  bool enabled = false;
  bool update_map = false;
  bool temporal_update = false;
  bool tree_prob_present[7] = {};
  int tree_prob[7] = {};
  bool pred_prob_present[3] = {};
  int pred_prob[3] = {};
  bool update_data = false;
  bool abs_or_delta = false;
  SegmentFeature feature[8][4];
};

struct HeaderSpec {
  int profile = 0;
  bool show_existing = false;
  int existing_index = 0;

  bool key = true;
  bool show_frame = true;
  bool intra_only = false;
  bool error_resilient = false;

  int width = 512;
  int height = 256;
  bool render_size = false;
  int display_width = 0;
  int display_height = 0;

  int bit_depth = 8;
  int color_space = 1;
  int color_range = 0;
  int subsampling_x = 1;
  int subsampling_y = 1;

  int reset_frame_context = 0;
  int refresh_frame_flags = 0xff;
  int ref_frame_idx[3] = {0, 0, 0};
  int ref_frame_sign_bias[3] = {0, 0, 0};
  bool found_ref = false;
  bool high_precision_mv = false;
  bool switchable_filter = false;
  int interpolation_filter = 0;

  bool refresh_frame_context = false;
  bool frame_parallel_mode = false;
  int frame_context_idx = 0;

  bool lf_delta_enabled = false;
  bool lf_delta_update = false;
  bool lf_ref_update[4] = {};
  int lf_ref_delta[4] = {};
  bool lf_mode_update[2] = {};
  int lf_mode_delta[2] = {};

  int base_q = 0;
  bool delta_q_present[3] = {};
  int delta_q[3] = {};

  int tile_columns_log2 = 0;
  int tile_rows_log2 = 0;

  SegmentationSpec seg;
};

// --- Independent copies of the tile sizing rules, for emitting legal bits and
// --- computing expectations only.

int AlignPowerOfTwo(int value, int n) {
  return (value + (1 << n) - 1) & ~((1 << n) - 1);
}

int SuperblockColumns(int width) {
  const int aligned_width = AlignPowerOfTwo(width, 3);
  const int mi_cols = aligned_width >> 3;
  const int aligned_mi_cols = AlignPowerOfTwo(mi_cols, 3);
  return aligned_mi_cols >> 3;
}

void TileColumnLog2Range(int sb_cols, int* min_log2, int* max_log2) {
  int min_value = 0;
  while ((64 << min_value) < sb_cols) ++min_value;
  int max_value = 0;
  while ((sb_cols >> max_value) >= 4) ++max_value;
  --max_value;
  if (max_value < 0) max_value = 0;
  if (max_value < min_value) max_value = min_value;
  if (max_value > 6) max_value = 6;
  *min_log2 = min_value;
  *max_log2 = max_value;
}

// ---------------------------------------------------------------------------
// Header writer.
// ---------------------------------------------------------------------------

void WriteColorConfig(BitWriter* w, const HeaderSpec& s) {
  if (s.profile >= 2) w->Put(s.bit_depth == 12 ? 1 : 0, 1);
  w->Put(static_cast<uint32_t>(s.color_space), 3);
  if (s.color_space != 7) {
    w->Put(static_cast<uint32_t>(s.color_range), 1);
    if (s.profile == 1 || s.profile == 3) {
      w->Put(static_cast<uint32_t>(s.subsampling_x), 1);
      w->Put(static_cast<uint32_t>(s.subsampling_y), 1);
      w->Put(0, 1);  // reserved_zero
    }
  } else if (s.profile == 1 || s.profile == 3) {
    w->Put(0, 1);  // subsampling_x
    w->Put(0, 1);  // subsampling_y
    w->Put(0, 1);  // reserved_zero
  }
}

void WriteFrameSize(BitWriter* w, const HeaderSpec& s) {
  w->Put(static_cast<uint32_t>(s.width - 1), 16);
  w->Put(static_cast<uint32_t>(s.height - 1), 16);
  w->Put(s.render_size ? 1 : 0, 1);
  if (s.render_size) {
    w->Put(static_cast<uint32_t>(s.display_width - 1), 16);
    w->Put(static_cast<uint32_t>(s.display_height - 1), 16);
  }
}

void WriteSegmentation(BitWriter* w, const SegmentationSpec& seg) {
  w->Put(seg.enabled ? 1 : 0, 1);
  if (!seg.enabled) return;
  w->Put(seg.update_map ? 1 : 0, 1);
  if (seg.update_map) {
    for (int i = 0; i < 7; ++i) {
      w->Put(seg.tree_prob_present[i] ? 1 : 0, 1);
      if (seg.tree_prob_present[i]) w->Put(static_cast<uint32_t>(seg.tree_prob[i]), 8);
    }
    // segmentation_temporal_update is only coded when the map is updated.
    w->Put(seg.temporal_update ? 1 : 0, 1);
    if (seg.temporal_update) {
      for (int i = 0; i < 3; ++i) {
        w->Put(seg.pred_prob_present[i] ? 1 : 0, 1);
        if (seg.pred_prob_present[i])
          w->Put(static_cast<uint32_t>(seg.pred_prob[i]), 8);
      }
    }
  }
  w->Put(seg.update_data ? 1 : 0, 1);
  if (!seg.update_data) return;
  w->Put(seg.abs_or_delta ? 1 : 0, 1);
  for (int segment = 0; segment < 8; ++segment) {
    for (int feature = 0; feature < 4; ++feature) {
      const SegmentFeature& f = seg.feature[segment][feature];
      w->Put(f.enabled ? 1 : 0, 1);
      if (!f.enabled) continue;
      switch (feature) {
        case 0:
          w->PutSigned(8, f.value);
          break;
        case 1:
          w->PutSigned(6, f.value);
          break;
        case 2:
          w->Put(static_cast<uint32_t>(f.value), 2);
          break;
        default:  // SEG_LVL_SKIP has no value bits.
          break;
      }
    }
  }
}

void WriteTileInfo(BitWriter* w, const HeaderSpec& s) {
  const int sb_cols = SuperblockColumns(s.width);
  int min_log2 = 0;
  int max_log2 = 0;
  TileColumnLog2Range(sb_cols, &min_log2, &max_log2);

  int current = min_log2;
  for (int bit = min_log2; bit < max_log2; ++bit) {
    if (current < s.tile_columns_log2) {
      w->Put(1, 1);
      ++current;
    } else {
      w->Put(0, 1);
      break;
    }
  }

  if (s.tile_rows_log2 == 0) {
    w->Put(0, 1);
  } else {
    w->Put(1, 1);
    w->Put(static_cast<uint32_t>(s.tile_rows_log2 - 1), 1);
  }
}

void WriteUncompressedHeader(BitWriter* w, const HeaderSpec& s,
                             size_t compressed_header_size) {
  w->Put(2, 2);  // frame_marker
  // The profile is stored least significant bit first.
  w->Put(static_cast<uint32_t>(s.profile) & 1u, 1);
  w->Put((static_cast<uint32_t>(s.profile) >> 1) & 1u, 1);
  if (s.profile == 3) w->Put(0, 1);  // reserved_zero

  w->Put(s.show_existing ? 1 : 0, 1);
  if (s.show_existing) {
    w->Put(static_cast<uint32_t>(s.existing_index), 3);
    w->AlignZero();
    return;
  }

  w->Put(s.key ? 0 : 1, 1);  // key_frame is the inverse of the bit.
  w->Put(s.show_frame ? 1 : 0, 1);
  w->Put(s.error_resilient ? 1 : 0, 1);

  if (s.key) {
    w->Put(0x498342, 24);
    WriteColorConfig(w, s);
    WriteFrameSize(w, s);
  } else {
    if (!s.show_frame) w->Put(s.intra_only ? 1 : 0, 1);
    if (!s.error_resilient)
      w->Put(static_cast<uint32_t>(s.reset_frame_context), 2);

    if (s.intra_only) {
      w->Put(0x498342, 24);
      if (s.profile > 0) WriteColorConfig(w, s);
      w->Put(static_cast<uint32_t>(s.refresh_frame_flags), 8);
      WriteFrameSize(w, s);
    } else {
      w->Put(static_cast<uint32_t>(s.refresh_frame_flags), 8);
      for (int i = 0; i < 3; ++i) {
        w->Put(static_cast<uint32_t>(s.ref_frame_idx[i]), 3);
        w->Put(static_cast<uint32_t>(s.ref_frame_sign_bias[i]), 1);
      }
      if (s.found_ref) {
        w->Put(1, 1);  // frame_size_with_refs() picks a reference.
        w->AlignZero();
        return;
      }
      w->Put(0, 1);
      w->Put(0, 1);
      w->Put(0, 1);
      WriteFrameSize(w, s);
      w->Put(s.high_precision_mv ? 1 : 0, 1);
      w->Put(s.switchable_filter ? 1 : 0, 1);
      if (!s.switchable_filter)
        w->Put(static_cast<uint32_t>(s.interpolation_filter), 2);
    }
  }

  if (!s.error_resilient) {
    w->Put(s.refresh_frame_context ? 1 : 0, 1);
    w->Put(s.frame_parallel_mode ? 1 : 0, 1);
  }
  w->Put(static_cast<uint32_t>(s.frame_context_idx), 2);

  w->Put(0, 6);  // loop_filter_level
  w->Put(0, 3);  // loop_filter_sharpness
  w->Put(s.lf_delta_enabled ? 1 : 0, 1);
  if (s.lf_delta_enabled) {
    w->Put(s.lf_delta_update ? 1 : 0, 1);
    if (s.lf_delta_update) {
      for (int i = 0; i < 4; ++i) {
        w->Put(s.lf_ref_update[i] ? 1 : 0, 1);
        if (s.lf_ref_update[i]) w->PutSigned(6, s.lf_ref_delta[i]);
      }
      for (int i = 0; i < 2; ++i) {
        w->Put(s.lf_mode_update[i] ? 1 : 0, 1);
        if (s.lf_mode_update[i]) w->PutSigned(6, s.lf_mode_delta[i]);
      }
    }
  }

  w->Put(static_cast<uint32_t>(s.base_q), 8);
  for (int i = 0; i < 3; ++i) {
    w->Put(s.delta_q_present[i] ? 1 : 0, 1);
    if (s.delta_q_present[i]) w->PutSigned(4, s.delta_q[i]);
  }

  WriteSegmentation(w, s.seg);
  WriteTileInfo(w, s);

  w->Put(static_cast<uint32_t>(compressed_header_size), 16);
  w->AlignZero();
}

// ---------------------------------------------------------------------------
// Packet construction with recorded expected layout.
// ---------------------------------------------------------------------------

struct ExpectedFrame {
  std::vector<unsigned char> bytes;
  size_t uncompressed_header_bytes = 0;
  size_t compressed_header_offset = 0;
  size_t compressed_header_size = 0;
  int width = 0;
  int height = 0;
  int tile_rows = 0;
  int tile_columns = 0;
  bool show_existing = false;
  int existing_index = 0;
  std::vector<size_t> tile_offsets;
  std::vector<size_t> tile_lengths;
};

ExpectedFrame BuildFrame(const HeaderSpec& header,
                         const std::vector<unsigned char>& compressed_header,
                         const std::vector<std::vector<unsigned char>>& tiles) {
  ExpectedFrame frame;
  BitWriter writer;
  WriteUncompressedHeader(&writer, header, compressed_header.size());
  frame.uncompressed_header_bytes = writer.size();
  frame.bytes = writer.data();

  frame.width = header.width;
  frame.height = header.height;
  frame.tile_columns = 1 << header.tile_columns_log2;
  frame.tile_rows = 1 << header.tile_rows_log2;

  frame.compressed_header_offset = frame.bytes.size();
  frame.compressed_header_size = compressed_header.size();
  frame.bytes.insert(frame.bytes.end(), compressed_header.begin(),
                     compressed_header.end());

  for (size_t i = 0; i < tiles.size(); ++i) {
    if (i + 1 < tiles.size()) {
      const size_t length = tiles[i].size();
      frame.bytes.push_back(static_cast<unsigned char>((length >> 24) & 0xff));
      frame.bytes.push_back(static_cast<unsigned char>((length >> 16) & 0xff));
      frame.bytes.push_back(static_cast<unsigned char>((length >> 8) & 0xff));
      frame.bytes.push_back(static_cast<unsigned char>(length & 0xff));
    }
    frame.tile_offsets.push_back(frame.bytes.size());
    frame.tile_lengths.push_back(tiles[i].size());
    frame.bytes.insert(frame.bytes.end(), tiles[i].begin(), tiles[i].end());
  }

  // A frame whose final byte matches the superframe marker pattern would be
  // (correctly) treated as a superframe by any VP9 reader. Payload bytes are
  // opaque, so flip such a payload byte to keep the frame a plain single frame
  // without changing any recorded offset or length. Header bytes are never
  // touched.
  if (frame.bytes.size() > frame.uncompressed_header_bytes &&
      (frame.bytes.back() & 0xe0) == 0xc0)
    frame.bytes.back() &= 0x1f;
  return frame;
}

ExpectedFrame BuildShowExistingFrame(int profile, int existing_index) {
  ExpectedFrame frame;
  BitWriter writer;
  writer.Put(2, 2);
  writer.Put(static_cast<uint32_t>(profile) & 1u, 1);
  writer.Put((static_cast<uint32_t>(profile) >> 1) & 1u, 1);
  if (profile == 3) writer.Put(0, 1);
  writer.Put(1, 1);  // show_existing_frame
  writer.Put(static_cast<uint32_t>(existing_index), 3);
  writer.AlignZero();
  frame.bytes = writer.data();
  if (!frame.bytes.empty() && (frame.bytes.back() & 0xe0) == 0xc0)
    frame.bytes.back() ^= 0x01;  // Avoid an accidental superframe marker.
  frame.uncompressed_header_bytes = writer.size();
  frame.show_existing = true;
  frame.existing_index = existing_index;
  return frame;
}

// Appends a superframe index with |length_field_width| byte little-endian
// child lengths. Returns the assembled packet.
std::vector<unsigned char> MakeSuperframe(
    const std::vector<std::vector<unsigned char>>& children,
    int length_field_width) {
  std::vector<unsigned char> packet;
  for (size_t i = 0; i < children.size(); ++i)
    packet.insert(packet.end(), children[i].begin(), children[i].end());

  const unsigned char marker = static_cast<unsigned char>(
      0xc0 | ((length_field_width - 1) << 3) | (children.size() - 1));
  packet.push_back(marker);
  for (size_t i = 0; i < children.size(); ++i) {
    size_t length = children[i].size();
    for (int byte = 0; byte < length_field_width; ++byte) {
      packet.push_back(static_cast<unsigned char>((length >> (8 * byte)) & 0xff));
    }
  }
  packet.push_back(marker);
  return packet;
}

std::vector<unsigned char> Payload(size_t size, unsigned char seed) {
  std::vector<unsigned char> bytes(size);
  for (size_t i = 0; i < size; ++i)
    bytes[i] = static_cast<unsigned char>(seed + i * 17);
  return bytes;
}

// ---------------------------------------------------------------------------
// Comparison helpers.
// ---------------------------------------------------------------------------

bool MatchesFrame(const vp9_parser::Vp9FrameLayout& layout,
                  const ExpectedFrame& expected, size_t base_offset) {
  if (layout.offset != base_offset + 0) return false;
  if (layout.length != expected.bytes.size()) return false;
  if (layout.uncompressed_header_bytes != expected.uncompressed_header_bytes)
    return false;
  if (layout.show_existing != expected.show_existing) return false;
  if (layout.existing_index != expected.existing_index) return false;

  if (expected.show_existing) {
    // A show-existing frame carries no compressed header and no tiles, and no
    // width/height is invented for it.
    if (layout.compressed_header_offset != 0) return false;
    if (layout.compressed_header_size != 0) return false;
    if (layout.width != 0 || layout.height != 0) return false;
    if (layout.tile_rows != 0 || layout.tile_columns != 0) return false;
    return layout.tiles.empty();
  }

  if (layout.compressed_header_offset !=
      base_offset + expected.compressed_header_offset)
    return false;
  if (layout.compressed_header_size != expected.compressed_header_size)
    return false;

  if (layout.width != expected.width) return false;
  if (layout.height != expected.height) return false;
  if (layout.tile_rows != expected.tile_rows) return false;
  if (layout.tile_columns != expected.tile_columns) return false;
  if (layout.tiles.size() != expected.tile_offsets.size()) return false;

  for (size_t i = 0; i < layout.tiles.size(); ++i) {
    const vp9_parser::Vp9TileRange& tile = layout.tiles[i];
    if (tile.offset != base_offset + expected.tile_offsets[i]) return false;
    if (tile.length != expected.tile_lengths[i]) return false;
    if (tile.row != static_cast<int>(i / expected.tile_columns)) return false;
    if (tile.column != static_cast<int>(i % expected.tile_columns)) return false;
  }
  return true;
}

// Every tile is non-empty, ranges are ordered and non-overlapping, and the
// ranges cover the whole tile payload: the only gaps allowed are the four byte
// length prefixes that precede every tile but the last.
bool RangesTilePayload(const std::vector<vp9_parser::Vp9TileRange>& tiles,
                       size_t payload_start) {
  size_t cursor = payload_start;
  for (size_t i = 0; i < tiles.size(); ++i) {
    if (tiles[i].length == 0) return false;
    // Every tile but the last is preceded by a four byte length prefix; the
    // last tile has no prefix and starts where the previous one ended.
    const size_t expected_offset =
        (i + 1 < tiles.size()) ? cursor + 4 : cursor;
    if (tiles[i].offset != expected_offset) return false;
    cursor = tiles[i].offset + tiles[i].length;
  }
  return true;
}

bool HasSentinel(const std::vector<vp9_parser::Vp9FrameLayout>& out) {
  return out.size() == 1 && out[0].width == 12345 && out[0].height == 54321 &&
         out[0].tiles.size() == 1 && out[0].tiles[0].offset == 7 &&
         out[0].tiles[0].length == 11;
}

std::vector<vp9_parser::Vp9FrameLayout> MakeSentinel() {
  std::vector<vp9_parser::Vp9FrameLayout> out(1);
  out[0].width = 12345;
  out[0].height = 54321;
  vp9_parser::Vp9TileRange tile;
  tile.offset = 7;
  tile.length = 11;
  out[0].tiles.push_back(tile);
  return out;
}

HeaderSpec BaseKeyHeader(int width, int height) {
  HeaderSpec header;
  header.key = true;
  header.width = width;
  header.height = height;
  return header;
}

}  // namespace

// ---------------------------------------------------------------------------
// headers
// ---------------------------------------------------------------------------

TASK_CASE("headers", SegmentationMapTemporalAndFeatures) {
  HeaderSpec header = BaseKeyHeader(512, 256);
  header.tile_columns_log2 = 1;  // 2 columns
  header.tile_rows_log2 = 2;     // 4 rows
  header.seg.enabled = true;
  header.seg.update_map = true;
  header.seg.temporal_update = true;
  for (int i = 0; i < 7; ++i) {
    header.seg.tree_prob_present[i] = true;
    header.seg.tree_prob[i] = 120 + i;
  }
  for (int i = 0; i < 3; ++i) {
    header.seg.pred_prob_present[i] = true;
    header.seg.pred_prob[i] = 200 + i;
  }
  header.seg.update_data = true;
  header.seg.abs_or_delta = false;
  // Positive, zero, and negative signed magnitudes, plus the zero-width skip
  // feature.
  const int values[8] = {7, 0, -7, 1, -1, 0, 3, -3};
  for (int segment = 0; segment < 8; ++segment) {
    header.seg.feature[segment][0].enabled = true;
    header.seg.feature[segment][0].value = values[segment];
    header.seg.feature[segment][1].enabled = (segment % 2) == 0;
    header.seg.feature[segment][1].value = -values[segment];
    header.seg.feature[segment][2].enabled = (segment % 3) == 0;
    header.seg.feature[segment][2].value = segment & 3;
    header.seg.feature[segment][3].enabled = true;  // no value bits
  }

  std::vector<std::vector<unsigned char>> tiles;
  for (int i = 0; i < 8; ++i) tiles.push_back(Payload(3 + i, 0x40 + i));
  const ExpectedFrame expected =
      BuildFrame(header, Payload(6, 0x90), tiles);

  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(expected.bytes.data(),
                                         expected.bytes.size(), &out));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(MatchesFrame(out[0], expected, 0));
  TASK_CHECK(RangesTilePayload(
      out[0].tiles, expected.compressed_header_offset +
                        expected.compressed_header_size));

  // The legacy parser must now reach tile_info through a full segmentation
  // header, and Vp9LevelStats must observe the corrected column count.
  vp9_parser::Vp9HeaderParser parser;
  TASK_CHECK(parser.ParseUncompressedHeader(expected.bytes.data(),
                                            expected.bytes.size()));
  TASK_CHECK(parser.column_tiles() == 2);
  TASK_CHECK(parser.row_tiles() == 4);
  vp9_parser::Vp9LevelStats stats;
  stats.AddFrame(parser, 0);
  TASK_CHECK(stats.GetMaxColumnTiles() == 2);
}

TASK_CASE("headers", SegmentationFourFeatureValues) {
  HeaderSpec header = BaseKeyHeader(1024, 512);
  header.tile_columns_log2 = 2;  // 4 columns
  header.tile_rows_log2 = 0;
  header.seg.enabled = true;
  header.seg.update_map = false;
  header.seg.update_data = true;
  header.seg.abs_or_delta = true;
  for (int segment = 0; segment < 8; ++segment) {
    const int sign = (segment % 2) ? -1 : 1;
    header.seg.feature[segment][0].enabled = true;
    header.seg.feature[segment][0].value = sign * (segment * 31);
    header.seg.feature[segment][1].enabled = true;
    header.seg.feature[segment][1].value = (segment == 0) ? 0 : sign * segment;
    header.seg.feature[segment][2].enabled = (segment % 4) != 0;
    header.seg.feature[segment][2].value = segment % 4;
    header.seg.feature[segment][3].enabled = (segment % 2) == 0;
  }

  std::vector<std::vector<unsigned char>> tiles;
  for (int i = 0; i < 4; ++i) tiles.push_back(Payload(5 + i, 0x20 + i));
  const ExpectedFrame expected = BuildFrame(header, Payload(2, 0xaa), tiles);

  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(expected.bytes.data(),
                                         expected.bytes.size(), &out));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(MatchesFrame(out[0], expected, 0));

  vp9_parser::Vp9HeaderParser parser;
  TASK_CHECK(parser.ParseUncompressedHeader(expected.bytes.data(),
                                            expected.bytes.size()));
  TASK_CHECK(parser.column_tiles() == 4);
  TASK_CHECK(parser.row_tiles() == 1);
}

TASK_CASE("headers", ProfilesAndErrorResilient) {
  for (int profile = 0; profile < 4; ++profile) {
    HeaderSpec header = BaseKeyHeader(640, 360);
    header.profile = profile;
    header.error_resilient = (profile % 2) == 1;
    header.bit_depth = profile >= 2 ? 10 : 8;
    header.color_space = profile == 0 ? 1 : 2;
    if (profile == 1 || profile == 3) {
      header.color_range = 1;
      header.subsampling_x = 0;
      header.subsampling_y = 0;
    }
    const ExpectedFrame expected = BuildFrame(header, Payload(4, 0x10),
                                              {Payload(9, 0x30)});
    std::vector<vp9_parser::Vp9FrameLayout> out;
    const bool ok = vp9_parser::ExtractVP9Tiles(expected.bytes.data(),
                                                expected.bytes.size(), &out);
    TASK_CHECK(ok);
    TASK_CHECK(out.size() == 1);
    TASK_CHECK(MatchesFrame(out[0], expected, 0));
  }

  // Intra-only frames: profile 0 uses the implicit color config and profiles
  // 1-3 carry one; error_resilient changes whether frame_parallel_mode and the
  // reset_frame_context bits are coded.
  for (int profile = 0; profile < 4; ++profile) {
    HeaderSpec header;
    header.profile = profile;
    header.key = false;
    header.show_frame = false;
    header.intra_only = true;
    header.error_resilient = (profile % 2) == 0;
    header.width = 320;
    header.height = 240;
    // Profile 0 intra-only frames do not code a color config; the VP9 default
    // is BT.601 4:2:0. Profiles 1-3 code one explicitly.
    header.color_space = profile == 0 ? 1 : 3;
    header.color_range = profile == 0 ? 0 : 1;
    header.subsampling_x = (profile == 1 || profile == 3) ? 0 : 1;
    header.subsampling_y = (profile == 1 || profile == 3) ? 0 : 1;
    header.bit_depth = profile >= 2 ? 12 : 8;
    header.refresh_frame_flags = 0x55;
    header.reset_frame_context = 2;
    const ExpectedFrame expected = BuildFrame(header, Payload(3, 0x50),
                                              {Payload(7, 0x60)});
    std::vector<vp9_parser::Vp9FrameLayout> out;
    const bool ok = vp9_parser::ExtractVP9Tiles(expected.bytes.data(),
                                                expected.bytes.size(), &out);
    TASK_CHECK(ok);
    TASK_CHECK(out.size() == 1);
    TASK_CHECK(MatchesFrame(out[0], expected, 0));
    TASK_CHECK(out[0].width == 320 && out[0].height == 240);
    // The legacy parser also decodes the color config for intra-only frames.
    vp9_parser::Vp9HeaderParser parser;
    TASK_CHECK(parser.ParseUncompressedHeader(expected.bytes.data(),
                                              expected.bytes.size()));
    TASK_CHECK(parser.profile() == profile);
    TASK_CHECK(parser.width() == 320 && parser.height() == 240);
    TASK_CHECK(parser.bit_depth() == header.bit_depth);
    TASK_CHECK(parser.color_space() == header.color_space);
  }
}

TASK_CASE("headers", ExplicitSizeInterFrames) {
  HeaderSpec header;
  header.key = false;
  header.show_frame = true;
  header.error_resilient = false;
  header.width = 2048;
  header.height = 1024;
  header.refresh_frame_flags = 0x03;
  header.ref_frame_idx[0] = 1;
  header.ref_frame_idx[1] = 6;
  header.ref_frame_sign_bias[1] = 1;
  header.high_precision_mv = true;
  header.switchable_filter = true;
  header.reset_frame_context = 3;
  header.frame_parallel_mode = true;
  header.tile_columns_log2 = 1;
  header.tile_rows_log2 = 1;
  std::vector<std::vector<unsigned char>> tiles;
  for (int i = 0; i < 4; ++i) tiles.push_back(Payload(4 + 2 * i, 0x70 + i));
  ExpectedFrame expected = BuildFrame(header, Payload(5, 0x15), tiles);

  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(expected.bytes.data(),
                                         expected.bytes.size(), &out));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(MatchesFrame(out[0], expected, 0));

  // Same but error resilient (no reset_frame_context / frame_parallel_mode
  // bits) and a non-switchable interpolation filter.
  header.error_resilient = true;
  header.switchable_filter = false;
  header.interpolation_filter = 2;
  header.tile_columns_log2 = 2;
  header.tile_rows_log2 = 0;
  tiles.clear();
  for (int i = 0; i < 4; ++i) tiles.push_back(Payload(6 + i, 0x11 + i));
  expected = BuildFrame(header, Payload(1, 0xee), tiles);
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(expected.bytes.data(),
                                         expected.bytes.size(), &out));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(MatchesFrame(out[0], expected, 0));
}

TASK_CASE("headers", LoopFilterAndQuantizationDeltas) {
  HeaderSpec header = BaseKeyHeader(1024, 576);
  header.tile_columns_log2 = 2;
  header.tile_rows_log2 = 1;
  header.lf_delta_enabled = true;
  header.lf_delta_update = true;
  header.lf_ref_update[0] = true;
  header.lf_ref_delta[0] = 3;
  header.lf_ref_update[2] = true;
  header.lf_ref_delta[2] = 0;  // zero magnitude still carries a sign bit.
  header.lf_ref_update[3] = true;
  header.lf_ref_delta[3] = -2;
  header.lf_mode_update[1] = true;
  header.lf_mode_delta[1] = -1;
  header.base_q = 77;
  header.delta_q_present[0] = true;
  header.delta_q[0] = -4;
  header.delta_q_present[1] = true;
  header.delta_q[1] = 0;
  header.delta_q_present[2] = true;
  header.delta_q[2] = 5;

  std::vector<std::vector<unsigned char>> tiles;
  for (int i = 0; i < 8; ++i) tiles.push_back(Payload(3 + (i % 4), 0x01 + i));
  const ExpectedFrame expected = BuildFrame(header, Payload(7, 0x77), tiles);

  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(expected.bytes.data(),
                                         expected.bytes.size(), &out));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(MatchesFrame(out[0], expected, 0));
}

TASK_CASE("headers", InheritedSizeInterIsRejected) {
  HeaderSpec header;
  header.key = false;
  header.show_frame = true;
  header.width = 1920;
  header.height = 1080;
  header.found_ref = true;
  header.refresh_frame_flags = 0x0f;
  const ExpectedFrame unsupported =
      BuildFrame(header, Payload(2, 0x00), {});

  std::vector<vp9_parser::Vp9FrameLayout> out = MakeSentinel();
  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(unsupported.bytes.data(),
                                          unsupported.bytes.size(), &out));
  TASK_CHECK(HasSentinel(out));
}

// ---------------------------------------------------------------------------
// tiles
// ---------------------------------------------------------------------------

TASK_CASE("tiles", SingleTile) {
  HeaderSpec header = BaseKeyHeader(256, 144);
  header.tile_columns_log2 = 0;
  header.tile_rows_log2 = 0;
  const ExpectedFrame expected =
      BuildFrame(header, Payload(5, 0xc0), {Payload(18, 0x31)});

  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(expected.bytes.data(),
                                         expected.bytes.size(), &out));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(MatchesFrame(out[0], expected, 0));
  TASK_CHECK(out[0].tiles.size() == 1);
  TASK_CHECK(out[0].tiles[0].offset ==
             expected.compressed_header_offset + expected.compressed_header_size);
  TASK_CHECK(out[0].tiles[0].offset + out[0].tiles[0].length ==
             expected.bytes.size());
  TASK_CHECK(out[0].tiles[0].row == 0 && out[0].tiles[0].column == 0);
}

TASK_CASE("tiles", MultiRowColumnUnequalPayloads) {
  HeaderSpec header = BaseKeyHeader(2048, 1024);
  header.tile_columns_log2 = 2;  // 4 columns
  header.tile_rows_log2 = 2;     // 4 rows
  std::vector<std::vector<unsigned char>> tiles;
  for (int i = 0; i < 16; ++i) tiles.push_back(Payload(2 + (i * 7) % 23, i));

  const ExpectedFrame expected = BuildFrame(header, Payload(9, 0x42), tiles);
  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(expected.bytes.data(),
                                         expected.bytes.size(), &out));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(MatchesFrame(out[0], expected, 0));
  TASK_CHECK(out[0].tiles.size() == 16);
  TASK_CHECK(out[0].tile_columns == 4 && out[0].tile_rows == 4);

  // Row-major order and contiguous coverage of the tile payload only.
  bool row_major = true;
  for (int i = 0; i < 16; ++i) {
    if (out[0].tiles[i].row != i / 4) row_major = false;
    if (out[0].tiles[i].column != i % 4) row_major = false;
  }
  TASK_CHECK(row_major);
  TASK_CHECK(RangesTilePayload(
      out[0].tiles, expected.compressed_header_offset +
                        expected.compressed_header_size));
  TASK_CHECK(out[0].tiles[0].offset ==
             expected.compressed_header_offset +
                 expected.compressed_header_size + 4);
}

TASK_CASE("tiles", OpaqueCompressedHeaderWithFakeLengths) {
  HeaderSpec header = BaseKeyHeader(1024, 512);
  header.tile_columns_log2 = 1;  // 2 columns
  header.tile_rows_log2 = 0;

  // The compressed header starts with bytes that look exactly like a tile
  // length prefix followed by tile data. The parser must skip it wholesale.
  std::vector<unsigned char> compressed;
  compressed.push_back(0x00);
  compressed.push_back(0x00);
  compressed.push_back(0x00);
  compressed.push_back(0x05);
  compressed.push_back(0xde);
  compressed.push_back(0xad);
  compressed.push_back(0xbe);
  compressed.push_back(0xef);

  std::vector<std::vector<unsigned char>> tiles;
  tiles.push_back(Payload(11, 0x03));
  tiles.push_back(Payload(7, 0x09));
  const ExpectedFrame expected = BuildFrame(header, compressed, tiles);

  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(expected.bytes.data(),
                                         expected.bytes.size(), &out));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(out[0].compressed_header_size == compressed.size());
  TASK_CHECK(out[0].tiles.size() == 2);
  TASK_CHECK(out[0].tiles[0].offset ==
             expected.compressed_header_offset + compressed.size() + 4);
  TASK_CHECK(out[0].tiles[0].length == 11);
  TASK_CHECK(MatchesFrame(out[0], expected, 0));
}

TASK_CASE("tiles", TruncationAtEveryByteRejected) {
  HeaderSpec header = BaseKeyHeader(1024, 512);
  header.tile_columns_log2 = 1;
  header.tile_rows_log2 = 0;
  std::vector<std::vector<unsigned char>> tiles;
  tiles.push_back(Payload(9, 0x22));
  tiles.push_back(Payload(13, 0x44));
  const ExpectedFrame expected = BuildFrame(header, Payload(6, 0x66), tiles);
  const size_t last_tile_offset = expected.tile_offsets[1];

  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(expected.bytes.data(),
                                         expected.bytes.size(), &out));
  TASK_CHECK(out.size() == 1 && MatchesFrame(out[0], expected, 0));

  // Cutting anywhere before the last tile has at least one byte makes the
  // packet structurally invalid.
  bool all_rejected = true;
  for (size_t length = 0; length <= last_tile_offset; ++length) {
    std::vector<vp9_parser::Vp9FrameLayout> truncated = MakeSentinel();
    if (vp9_parser::ExtractVP9Tiles(expected.bytes.data(), length, &truncated))
      all_rejected = false;
    if (!HasSentinel(truncated)) all_rejected = false;
  }
  TASK_CHECK(all_rejected);

  // A compressed header length that overruns the frame is rejected.
  std::vector<unsigned char> overrun = expected.bytes;
  const size_t size_field_bit = 8 * (expected.uncompressed_header_bytes - 2);
  overrun[size_field_bit / 8] = 0x0f;
  overrun[size_field_bit / 8 + 1] = 0xff;
  std::vector<vp9_parser::Vp9FrameLayout> out2 = MakeSentinel();
  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(overrun.data(), overrun.size(), &out2));
  TASK_CHECK(HasSentinel(out2));
}

TASK_CASE("tiles", BrokenTilePrefixesRejected) {
  HeaderSpec header = BaseKeyHeader(1024, 512);
  header.tile_columns_log2 = 1;
  header.tile_rows_log2 = 0;

  // First tile declares a zero length.
  std::vector<std::vector<unsigned char>> zero_tiles;
  zero_tiles.push_back(std::vector<unsigned char>());
  zero_tiles.push_back(Payload(6, 0x01));
  ExpectedFrame zero = BuildFrame(header, Payload(2, 0x00), zero_tiles);
  std::vector<vp9_parser::Vp9FrameLayout> out = MakeSentinel();
  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(zero.bytes.data(), zero.bytes.size(),
                                          &out));
  TASK_CHECK(HasSentinel(out));

  // First tile declares far more bytes than the subframe holds.
  ExpectedFrame over = BuildFrame(header, Payload(2, 0x00),
                                  {Payload(3, 0x07), Payload(4, 0x08)});
  const size_t prefix_position = over.compressed_header_offset +
                                 over.compressed_header_size;
  over.bytes[prefix_position] = 0x7f;
  over.bytes[prefix_position + 1] = 0xff;
  over.bytes[prefix_position + 2] = 0xff;
  over.bytes[prefix_position + 3] = 0xff;
  out = MakeSentinel();
  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(over.bytes.data(), over.bytes.size(),
                                          &out));
  TASK_CHECK(HasSentinel(out));

  // Missing bytes of the length prefix itself.
  ExpectedFrame short_prefix = BuildFrame(header, Payload(2, 0x00),
                                          {Payload(3, 0x07), Payload(4, 0x08)});
  const size_t cut = prefix_position + 2;
  out = MakeSentinel();
  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(short_prefix.bytes.data(), cut, &out));
  TASK_CHECK(HasSentinel(out));
}

// ---------------------------------------------------------------------------
// packets
// ---------------------------------------------------------------------------

TASK_CASE("packets", SuperframeWithMixedShowExisting) {
  HeaderSpec first = BaseKeyHeader(512, 256);
  first.tile_columns_log2 = 1;
  first.tile_rows_log2 = 1;
  std::vector<std::vector<unsigned char>> tiles;
  for (int i = 0; i < 4; ++i) tiles.push_back(Payload(4 + i, 0x30 + i));
  const ExpectedFrame frame0 = BuildFrame(first, Payload(3, 0xab), tiles);
  const ExpectedFrame frame1 = BuildShowExistingFrame(0, 5);
  // A show-existing subframe is exactly one byte in profiles 0-2.
  TASK_CHECK(frame1.bytes.size() == 1);

  HeaderSpec third = BaseKeyHeader(512, 256);
  third.tile_columns_log2 = 0;
  third.tile_rows_log2 = 2;
  std::vector<std::vector<unsigned char>> tiles3;
  for (int i = 0; i < 4; ++i) tiles3.push_back(Payload(2 + i, 0x55 + i));
  const ExpectedFrame frame2 = BuildFrame(third, Payload(4, 0xcd), tiles3);

  const std::vector<unsigned char> packet = MakeSuperframe(
      {frame0.bytes, frame1.bytes, frame2.bytes}, 1);

  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(packet.data(), packet.size(), &out));
  TASK_CHECK(out.size() == 3);
  TASK_CHECK(MatchesFrame(out[0], frame0, 0));
  TASK_CHECK(MatchesFrame(out[1], frame1, frame0.bytes.size()));
  TASK_CHECK(out[1].show_existing);
  TASK_CHECK(out[1].existing_index == 5);
  TASK_CHECK(out[1].width == 0 && out[1].height == 0);
  TASK_CHECK(out[1].tiles.empty());
  TASK_CHECK(MatchesFrame(out[2], frame2,
                          frame0.bytes.size() + frame1.bytes.size()));
}

TASK_CASE("packets", AllIndexLengthWidths) {
  for (int width = 1; width <= 4; ++width) {
    HeaderSpec a = BaseKeyHeader(512, 256);
    a.tile_columns_log2 = 1;
    a.tile_rows_log2 = 0;
    const ExpectedFrame frame_a =
        BuildFrame(a, Payload(2, 0x10), {Payload(5, 0x11), Payload(6, 0x12)});

    HeaderSpec b = BaseKeyHeader(640, 360);
    b.tile_columns_log2 = 0;
    b.tile_rows_log2 = 1;
    const ExpectedFrame frame_b = BuildFrame(
        b, Payload(3, 0x20), {Payload(4, 0x21), Payload(9, 0x22)});

    const std::vector<unsigned char> packet =
        MakeSuperframe({frame_a.bytes, frame_b.bytes}, width);
    TASK_CHECK(packet.back() == static_cast<unsigned char>(
                                    0xc0 | ((width - 1) << 3) | 1));

    std::vector<vp9_parser::Vp9FrameLayout> out;
    TASK_CHECK(vp9_parser::ExtractVP9Tiles(packet.data(), packet.size(), &out));
    TASK_CHECK(out.size() == 2);
    TASK_CHECK(MatchesFrame(out[0], frame_a, 0));
    TASK_CHECK(MatchesFrame(out[1], frame_b, frame_a.bytes.size()));
  }
}

TASK_CASE("packets", SuperframeOneAndEightSubframes) {
  HeaderSpec header = BaseKeyHeader(512, 256);
  header.tile_columns_log2 = 1;
  header.tile_rows_log2 = 0;
  const ExpectedFrame frame =
      BuildFrame(header, Payload(2, 0x5b), {Payload(4, 0x5c), Payload(5, 0x5d)});

  // A single-subframe superframe keeps its index and yields one layout.
  const std::vector<unsigned char> single = MakeSuperframe({frame.bytes}, 2);
  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(single.back() == static_cast<unsigned char>(0xc0 | (1 << 3)));
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(single.data(), single.size(), &out));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(MatchesFrame(out[0], frame, 0));

  // Eight subframes, the maximum VP9 allows, with a mix of layouts. The width
  // is wide enough that every column count used below is legal.
  std::vector<std::vector<unsigned char>> children;
  std::vector<ExpectedFrame> expected;
  for (int i = 0; i < 8; ++i) {
    HeaderSpec spec = BaseKeyHeader(2048, 512);
    spec.tile_columns_log2 = i % 3;
    spec.tile_rows_log2 = i % 3 == 0 ? 0 : 1;
    const int count = (1 << spec.tile_columns_log2) * (1 << spec.tile_rows_log2);
    std::vector<std::vector<unsigned char>> tiles;
    for (int t = 0; t < count; ++t)
      tiles.push_back(Payload(2 + (t + i) % 5, static_cast<unsigned char>(0xa0 + i)));
    expected.push_back(BuildFrame(spec, Payload(1 + i % 4, 0x0e), tiles));
    children.push_back(expected.back().bytes);
  }
  const std::vector<unsigned char> packet = MakeSuperframe(children, 2);
  TASK_CHECK(packet.back() == static_cast<unsigned char>(0xc0 | (1 << 3) | 7));
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(packet.data(), packet.size(), &out));
  TASK_CHECK(out.size() == 8);
  size_t base = 0;
  for (size_t i = 0; i < expected.size(); ++i) {
    TASK_CHECK(MatchesFrame(out[i], expected[i], base));
    base += expected[i].bytes.size();
  }
}

TASK_CASE("packets", AdjacentSubframeBoundaries) {
  std::vector<ExpectedFrame> frames;
  int widths[3] = {1024, 512, 2048};
  int columns_log2[3] = {1, 0, 2};
  int rows_log2[3] = {1, 2, 0};
  for (int i = 0; i < 3; ++i) {
    HeaderSpec header = BaseKeyHeader(widths[i], 512);
    header.tile_columns_log2 = columns_log2[i];
    header.tile_rows_log2 = rows_log2[i];
    const int count = (1 << columns_log2[i]) * (1 << rows_log2[i]);
    std::vector<std::vector<unsigned char>> tiles;
    for (int t = 0; t < count; ++t)
      tiles.push_back(Payload(2 + (t + i) % 7, static_cast<unsigned char>(0x80 + t)));
    frames.push_back(BuildFrame(header, Payload(2 + i, 0x3c), tiles));
  }

  std::vector<std::vector<unsigned char>> children;
  for (size_t i = 0; i < frames.size(); ++i) children.push_back(frames[i].bytes);
  const std::vector<unsigned char> packet = MakeSuperframe(children, 2);
  const size_t index_length = 2 + 2 * frames.size();
  const size_t payload_length = packet.size() - index_length;

  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(packet.data(), packet.size(), &out));
  TASK_CHECK(out.size() == frames.size());

  size_t base = 0;
  for (size_t i = 0; i < frames.size(); ++i) {
    TASK_CHECK(MatchesFrame(out[i], frames[i], base));
    const vp9_parser::Vp9TileRange& last = out[i].tiles.back();
    TASK_CHECK(last.offset + last.length == base + frames[i].bytes.size());
    if (i + 1 < frames.size()) {
      // The final tile must stop exactly at the next subframe, never entering
      // it, the payload end, or the index.
      TASK_CHECK(last.offset + last.length == out[i + 1].offset);
      TASK_CHECK(out[i + 1].offset == base + frames[i].bytes.size());
    } else {
      TASK_CHECK(last.offset + last.length == payload_length);
    }
    base += frames[i].bytes.size();
  }
}

TASK_CASE("packets", SentinelPreservedOnCorruptIndex) {
  HeaderSpec header = BaseKeyHeader(512, 256);
  header.tile_columns_log2 = 1;
  header.tile_rows_log2 = 0;
  const ExpectedFrame frame =
      BuildFrame(header, Payload(3, 0x01), {Payload(5, 0x02), Payload(6, 0x03)});

  // Well-formed superframe.
  std::vector<unsigned char> packet =
      MakeSuperframe({frame.bytes, frame.bytes}, 1);
  std::vector<vp9_parser::Vp9FrameLayout> out = MakeSentinel();
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(packet.data(), packet.size(), &out));
  TASK_CHECK(out.size() == 2 && MatchesFrame(out[0], frame, 0));

  // Leading marker corrupt: the packet advertises an index but it is broken,
  // so it must not degrade to a single frame.
  std::vector<unsigned char> bad_marker = packet;
  bad_marker[bad_marker.size() - (2 + 1 * 2)] = 0xd0;
  out = MakeSentinel();
  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(bad_marker.data(), bad_marker.size(),
                                           &out));
  TASK_CHECK(HasSentinel(out));

  // Child lengths that do not add up to the payload.
  std::vector<unsigned char> bad_total = packet;
  bad_total[bad_total.size() - 2] =
      static_cast<unsigned char>(bad_total[bad_total.size() - 2] + 1);
  out = MakeSentinel();
  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(bad_total.data(), bad_total.size(),
                                           &out));
  TASK_CHECK(HasSentinel(out));

  // A frame inside the superframe has an unsupported inherited size.
  HeaderSpec inherited;
  inherited.key = false;
  inherited.show_frame = true;
  inherited.found_ref = true;
  inherited.width = 640;
  inherited.height = 360;
  const ExpectedFrame unsupported = BuildFrame(inherited, Payload(1, 0), {});
  std::vector<unsigned char> mixed =
      MakeSuperframe({frame.bytes, unsupported.bytes}, 1);
  out = MakeSentinel();
  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(mixed.data(), mixed.size(), &out));
  TASK_CHECK(HasSentinel(out));
}

TASK_CASE("packets", PlainPacketAndNonIndexTail) {
  HeaderSpec header = BaseKeyHeader(256, 144);
  header.tile_columns_log2 = 0;
  header.tile_rows_log2 = 0;
  const ExpectedFrame frame =
      BuildFrame(header, Payload(4, 0x19), {Payload(12, 0x2a)});

  // No superframe marker at all: treated as a single frame.
  std::vector<vp9_parser::Vp9FrameLayout> out;
  TASK_CHECK(frame.bytes.back() != 0xc0);
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(frame.bytes.data(), frame.bytes.size(),
                                         &out));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(MatchesFrame(out[0], frame, 0));

  // A plain packet still parses when its last byte merely has the high bits of
  // a marker cleared, and a single-frame packet whose marker pattern is absent
  // is never split.
  std::vector<unsigned char> plain = frame.bytes;
  plain.push_back(0x9f);
  ExpectedFrame extended = frame;
  extended.bytes = plain;
  // The appended byte belongs to the final tile, which owns the remainder of
  // the frame.
  extended.tile_lengths.back() += 1;
  out.clear();
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(plain.data(), plain.size(), &out));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(MatchesFrame(out[0], extended, 0));

  // Truncating the plain packet into its uncompressed header fails.
  out = MakeSentinel();
  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(plain.data(), 4, &out));
  TASK_CHECK(HasSentinel(out));
}
