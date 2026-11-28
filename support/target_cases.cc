// Copyright (c) 2024 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
//
// Target cases for vp9_parser::ExtractVP9Tiles and the corrected uncompressed
// header parser. Headers and opaque payloads are synthesized with the local
// BitWriter below; every expected byte range comes from the layout recorded
// while writing, never from the production parser.

#include "cases.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "common/libwebm_util.h"
#include "common/vp9_header_parser.h"
#include "common/vp9_level_stats.h"
#include "common/vp9_tiles.h"

namespace {

// ---------------------------------------------------------------------------
// Local bit writer and frame/packet builders.
// ---------------------------------------------------------------------------

class BitWriter {
 public:
  void Put(uint32_t value, int bits) {
    for (int i = bits - 1; i >= 0; --i) {
      if ((bit_position_ & 7) == 0)
        bytes_.push_back(0);
      if ((value >> i) & 1u) {
        bytes_.back() = static_cast<uint8_t>(
            bytes_.back() | (1u << (7 - (bit_position_ & 7))));
      }
      ++bit_position_;
    }
  }

  // Writes an s(n) value: |magnitude_bits| magnitude bits then a sign bit.
  void PutSigned(int value, int magnitude_bits) {
    const uint32_t magnitude = value < 0 ? static_cast<uint32_t>(-value)
                                         : static_cast<uint32_t>(value);
    Put(magnitude, magnitude_bits);
    Put(value < 0 ? 1u : 0u, 1);
  }

  void AlignZero() {
    while (bit_position_ & 7)
      Put(0, 1);
  }

  size_t byte_position() const { return bytes_.size(); }
  size_t bit_position() const { return bit_position_; }
  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
  size_t bit_position_ = 0;
};

struct SegmentationSpec {
  SegmentationSpec()
      : enabled(false),
        update_map(false),
        temporal_update(false),
        update_data(false),
        abs_or_delta(false) {
    for (int s = 0; s < 8; ++s) {
      for (int f = 0; f < 4; ++f) {
        feature_enabled[s][f] = false;
        feature_value[s][f] = 0;
      }
    }
  }

  bool enabled;
  bool update_map;
  bool temporal_update;
  bool update_data;
  bool abs_or_delta;
  bool feature_enabled[8][4];
  int feature_value[8][4];
};

struct HeaderSpec {
  HeaderSpec()
      : profile(0),
        key(true),
        intra_only(false),
        show_existing(false),
        existing_index(0),
        error_resilient(false),
        show_frame(true),
        reset_frame_context(0),
        refresh_frame_context(true),
        frame_parallel_decoding_mode(false),
        color_space(1),
        color_range(0),
        ten_or_twelve_bit(false),
        subsampling_x(1),
        subsampling_y(1),
        width(64),
        height(64),
        render_diff(false),
        render_width(1),
        render_height(1),
        refresh_frame_flags(0),
        found_ref(false),
        allow_high_precision_mv(false),
        switchable_filter(true),
        raw_interp_filter(0),
        lf_delta_enabled(false),
        lf_delta_update(false),
        compressed_header_size(0),
        tile_cols_log2(0),
        tile_rows_log2(0) {
    for (int i = 0; i < 3; ++i) {
      ref_frame_idx[i] = 0;
      ref_sign_bias[i] = 0;
      delta_q_coded[i] = false;
      delta_q[i] = 0;
    }
    for (int i = 0; i < 4; ++i) {
      lf_ref_update[i] = false;
      lf_ref_delta[i] = 0;
    }
    for (int i = 0; i < 2; ++i) {
      lf_mode_update[i] = false;
      lf_mode_delta[i] = 0;
    }
  }

  int profile;
  bool key;
  bool intra_only;
  bool show_existing;
  int existing_index;
  bool error_resilient;
  bool show_frame;
  int reset_frame_context;
  bool refresh_frame_context;
  bool frame_parallel_decoding_mode;
  int color_space;
  int color_range;
  bool ten_or_twelve_bit;
  int subsampling_x;
  int subsampling_y;
  int width;
  int height;
  bool render_diff;
  int render_width;
  int render_height;
  int refresh_frame_flags;
  int ref_frame_idx[3];
  int ref_sign_bias[3];
  bool found_ref;
  bool allow_high_precision_mv;
  bool switchable_filter;
  int raw_interp_filter;
  bool lf_delta_enabled;
  bool lf_delta_update;
  bool lf_ref_update[4];
  int lf_ref_delta[4];
  bool lf_mode_update[2];
  int lf_mode_delta[2];
  bool delta_q_coded[3];
  int delta_q[3];
  SegmentationSpec seg;
  int compressed_header_size;
  int tile_cols_log2;
  int tile_rows_log2;
};

struct HeaderLayout {
  HeaderLayout()
      : uncompressed_header_bytes(0),
        compressed_header_size_bit_offset(0),
        padding_bits(0),
        width(0),
        height(0),
        tile_rows(0),
        tile_columns(0),
        show_existing(false),
        existing_index(0) {}

  size_t uncompressed_header_bytes;
  size_t compressed_header_size_bit_offset;  // absolute bit position
  int padding_bits;                         // zero bits after the 16-bit size
  int width;
  int height;
  int tile_rows;
  int tile_columns;
  bool show_existing;
  int existing_index;
};

int Sb64Cols(int width) {
  const int mi_cols = (width + 7) >> 3;
  return (mi_cols + 7) >> 3;
}

void TileColsRange(int width, int* min_log2, int* max_log2) {
  const int sb64_cols = Sb64Cols(width);
  int min_l = 0;
  while ((64 << min_l) < sb64_cols)
    ++min_l;
  int max_l = 1;
  while ((sb64_cols >> max_l) >= 4)
    ++max_l;
  --max_l;
  if (max_l < 0)
    max_l = 0;
  *min_log2 = min_l;
  *max_log2 = max_l;
}

bool TileLog2IsLegal(int width, int log2) {
  int min_l = 0;
  int max_l = 0;
  TileColsRange(width, &min_l, &max_l);
  return log2 >= min_l && log2 <= max_l;
}

void WriteColorConfig(const HeaderSpec& spec, BitWriter* writer) {
  if (spec.profile >= 2)
    writer->Put(spec.ten_or_twelve_bit ? 1u : 0u, 1);
  writer->Put(static_cast<uint32_t>(spec.color_space & 7), 3);
  if (spec.color_space != 7) {
    writer->Put(static_cast<uint32_t>(spec.color_range & 1), 1);
    if (spec.profile == 1 || spec.profile == 3) {
      writer->Put(static_cast<uint32_t>(spec.subsampling_x & 1), 1);
      writer->Put(static_cast<uint32_t>(spec.subsampling_y & 1), 1);
      writer->Put(0, 1);  // reserved_zero
    }
  } else {
    if (spec.profile == 1 || spec.profile == 3)
      writer->Put(0, 1);  // reserved_zero
  }
}

void WriteFrameSize(const HeaderSpec& spec, BitWriter* writer) {
  writer->Put(static_cast<uint32_t>(spec.width - 1), 16);
  writer->Put(static_cast<uint32_t>(spec.height - 1), 16);
}

void WriteRenderSize(const HeaderSpec& spec, BitWriter* writer) {
  writer->Put(spec.render_diff ? 1u : 0u, 1);
  if (spec.render_diff) {
    writer->Put(static_cast<uint32_t>(spec.render_width - 1), 16);
    writer->Put(static_cast<uint32_t>(spec.render_height - 1), 16);
  }
}

void WriteProbability(BitWriter* writer, int prob) {
  if (prob < 0) {
    writer->Put(0, 1);  // prob_coded == 0 -> 255
  } else {
    writer->Put(1, 1);
    writer->Put(static_cast<uint32_t>(prob & 0xff), 8);
  }
}

void WriteLoopFilter(const HeaderSpec& spec, BitWriter* writer) {
  writer->Put(0, 6);  // loop_filter_level
  writer->Put(0, 3);  // loop_filter_sharpness
  writer->Put(spec.lf_delta_enabled ? 1u : 0u, 1);
  if (!spec.lf_delta_enabled)
    return;
  writer->Put(spec.lf_delta_update ? 1u : 0u, 1);
  if (!spec.lf_delta_update)
    return;
  for (int i = 0; i < 4; ++i) {
    writer->Put(spec.lf_ref_update[i] ? 1u : 0u, 1);
    if (spec.lf_ref_update[i])
      writer->PutSigned(spec.lf_ref_delta[i], 6);
  }
  for (int i = 0; i < 2; ++i) {
    writer->Put(spec.lf_mode_update[i] ? 1u : 0u, 1);
    if (spec.lf_mode_update[i])
      writer->PutSigned(spec.lf_mode_delta[i], 6);
  }
}

void WriteQuantization(const HeaderSpec& spec, BitWriter* writer) {
  writer->Put(0, 8);  // base_q_idx
  for (int i = 0; i < 3; ++i) {
    writer->Put(spec.delta_q_coded[i] ? 1u : 0u, 1);
    if (spec.delta_q_coded[i])
      writer->PutSigned(spec.delta_q[i], 4);
  }
}

void WriteSegmentation(const HeaderSpec& spec, BitWriter* writer) {
  writer->Put(spec.seg.enabled ? 1u : 0u, 1);
  if (!spec.seg.enabled)
    return;

  writer->Put(spec.seg.update_map ? 1u : 0u, 1);
  if (spec.seg.update_map) {
    for (int i = 0; i < 7; ++i)
      WriteProbability(writer, -1);  // 255
    writer->Put(spec.seg.temporal_update ? 1u : 0u, 1);
    if (spec.seg.temporal_update) {
      for (int i = 0; i < 3; ++i)
        WriteProbability(writer, -1);
    }
  }

  writer->Put(spec.seg.update_data ? 1u : 0u, 1);
  if (!spec.seg.update_data)
    return;
  writer->Put(spec.seg.abs_or_delta ? 1u : 0u, 1);

  // Feature index: 0 -> 8 bits signed, 1 -> 6 bits signed,
  // 2 -> 2 bits unsigned, 3 -> 0 bits unsigned.
  const int feature_bits[4] = {8, 6, 2, 0};
  const bool feature_signed[4] = {true, true, false, false};
  for (int s = 0; s < 8; ++s) {
    for (int f = 0; f < 4; ++f) {
      const bool enabled = spec.seg.feature_enabled[s][f];
      writer->Put(enabled ? 1u : 0u, 1);
      if (!enabled)
        continue;
      if (feature_signed[f]) {
        writer->PutSigned(spec.seg.feature_value[s][f], feature_bits[f]);
      } else if (feature_bits[f] > 0) {
        writer->Put(static_cast<uint32_t>(spec.seg.feature_value[s][f]),
                    feature_bits[f]);
      }
    }
  }
}

void WriteTileInfo(const HeaderSpec& spec, BitWriter* writer) {
  int min_l = 0;
  int max_l = 0;
  TileColsRange(spec.width, &min_l, &max_l);
  int current = min_l;
  while (current < max_l) {
    if (current < spec.tile_cols_log2) {
      writer->Put(1, 1);
      ++current;
    } else {
      writer->Put(0, 1);
      break;
    }
  }

  if (spec.tile_rows_log2 == 0) {
    writer->Put(0, 1);
  } else {
    writer->Put(1, 1);
    writer->Put(spec.tile_rows_log2 == 2 ? 1u : 0u, 1);
  }
}

HeaderLayout WriteHeader(const HeaderSpec& spec, BitWriter* writer) {
  HeaderLayout layout;

  writer->Put(2, 2);  // frame_marker
  writer->Put(static_cast<uint32_t>(spec.profile & 1), 1);
  writer->Put(static_cast<uint32_t>((spec.profile >> 1) & 1), 1);
  if (spec.profile == 3)
    writer->Put(0, 1);  // reserved_zero
  writer->Put(spec.show_existing ? 1u : 0u, 1);

  if (spec.show_existing) {
    writer->Put(static_cast<uint32_t>(spec.existing_index & 7), 3);
    writer->AlignZero();
    layout.uncompressed_header_bytes = writer->byte_position();
    layout.show_existing = true;
    layout.existing_index = spec.existing_index;
    return layout;
  }

  writer->Put(spec.key ? 0u : 1u, 1);  // frame_type
  writer->Put(spec.show_frame ? 1u : 0u, 1);
  writer->Put(spec.error_resilient ? 1u : 0u, 1);

  if (spec.key) {
    writer->Put(0x498342, 24);
    WriteColorConfig(spec, writer);
    WriteFrameSize(spec, writer);
    WriteRenderSize(spec, writer);
  } else {
    if (!spec.show_frame)
      writer->Put(spec.intra_only ? 1u : 0u, 1);
    if (!spec.error_resilient)
      writer->Put(static_cast<uint32_t>(spec.reset_frame_context & 3), 2);

    if (spec.intra_only) {
      writer->Put(0x498342, 24);
      if (spec.profile > 0)
        WriteColorConfig(spec, writer);
      writer->Put(static_cast<uint32_t>(spec.refresh_frame_flags & 0xff), 8);
      WriteFrameSize(spec, writer);
      WriteRenderSize(spec, writer);
    } else {
      writer->Put(static_cast<uint32_t>(spec.refresh_frame_flags & 0xff), 8);
      for (int i = 0; i < 3; ++i) {
        writer->Put(static_cast<uint32_t>(spec.ref_frame_idx[i] & 7), 3);
        writer->Put(static_cast<uint32_t>(spec.ref_sign_bias[i] & 1), 1);
      }
      if (spec.found_ref) {
        writer->Put(1, 1);  // found_ref[0] == 1
        writer->Put(0, 1);
        writer->Put(0, 1);
        // frame_size() is skipped (size inherited from the reference);
        // frame_size_with_refs() still calls render_size().
        WriteRenderSize(spec, writer);
        writer->AlignZero();
        layout.uncompressed_header_bytes = writer->byte_position();
        return layout;
      }
      writer->Put(0, 1);
      writer->Put(0, 1);
      writer->Put(0, 1);
      WriteFrameSize(spec, writer);
      WriteRenderSize(spec, writer);
      writer->Put(spec.allow_high_precision_mv ? 1u : 0u, 1);
      writer->Put(spec.switchable_filter ? 1u : 0u, 1);
      if (!spec.switchable_filter)
        writer->Put(static_cast<uint32_t>(spec.raw_interp_filter & 3), 2);
    }
  }

  if (!spec.error_resilient) {
    writer->Put(spec.refresh_frame_context ? 1u : 0u, 1);
    writer->Put(spec.frame_parallel_decoding_mode ? 1u : 0u, 1);
  }

  writer->Put(0, 2);  // frame_context_idx
  WriteLoopFilter(spec, writer);
  WriteQuantization(spec, writer);
  WriteSegmentation(spec, writer);
  WriteTileInfo(spec, writer);
  layout.compressed_header_size_bit_offset = writer->bit_position();
  writer->Put(static_cast<uint32_t>(spec.compressed_header_size & 0xffff), 16);
  layout.padding_bits =
      static_cast<int>((8 - (writer->bit_position() & 7)) & 7);
  writer->AlignZero();

  layout.uncompressed_header_bytes = writer->byte_position();
  layout.width = spec.width;
  layout.height = spec.height;
  layout.tile_rows = 1 << spec.tile_rows_log2;
  layout.tile_columns = 1 << spec.tile_cols_log2;
  return layout;
}

struct FrameBuild {
  FrameBuild() : compressed_header_offset(0), compressed_header_size(0) {}

  std::vector<uint8_t> bytes;
  HeaderLayout header;
  size_t compressed_header_offset;
  size_t compressed_header_size;
  std::vector<vp9_parser::Vp9TileRange> tiles;  // frame-relative
};

std::vector<uint8_t> OpaqueBytes(size_t count, uint8_t seed) {
  // Keep every byte below 0xc0 so that a truncated packet's final byte can
  // never be mistaken for a superframe marker. The bytes stay distinct from
  // the header regions the tests are checking.
  std::vector<uint8_t> bytes(count);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t mixed = 0x20u + ((static_cast<uint32_t>(seed) +
                                     41u * static_cast<uint32_t>(i)) %
                                    0x60u);
    bytes[i] = static_cast<uint8_t>(mixed);
  }
  return bytes;
}

FrameBuild BuildFrame(const HeaderSpec& input_spec,
                      const std::vector<uint8_t>& compressed_header,
                      const std::vector<std::vector<uint8_t> >& payloads) {
  HeaderSpec spec = input_spec;
  spec.compressed_header_size = static_cast<int>(compressed_header.size());

  FrameBuild frame;
  BitWriter writer;
  frame.header = WriteHeader(spec, &writer);
  frame.bytes = writer.bytes();
  frame.compressed_header_offset = frame.bytes.size();
  frame.compressed_header_size = compressed_header.size();
  frame.bytes.insert(frame.bytes.end(), compressed_header.begin(),
                     compressed_header.end());

  const int rows = 1 << spec.tile_rows_log2;
  const int cols = 1 << spec.tile_cols_log2;
  for (int row = 0; row < rows; ++row) {
    for (int column = 0; column < cols; ++column) {
      const size_t index =
          static_cast<size_t>(row) * static_cast<size_t>(cols) + column;
      const bool last = (row == rows - 1) && (column == cols - 1);
      const std::vector<uint8_t>& payload = payloads[index];
      const uint32_t length = static_cast<uint32_t>(payload.size());

      if (!last) {
        frame.bytes.push_back(static_cast<uint8_t>((length >> 24) & 0xff));
        frame.bytes.push_back(static_cast<uint8_t>((length >> 16) & 0xff));
        frame.bytes.push_back(static_cast<uint8_t>((length >> 8) & 0xff));
        frame.bytes.push_back(static_cast<uint8_t>(length & 0xff));
      }
      vp9_parser::Vp9TileRange tile;
      tile.offset = frame.bytes.size();
      tile.length = payload.size();
      tile.row = row;
      tile.column = column;
      frame.tiles.push_back(tile);

      frame.bytes.insert(frame.bytes.end(), payload.begin(), payload.end());
    }
  }
  return frame;
}

std::vector<std::vector<uint8_t> > PayloadsFor(const HeaderSpec& spec,
                                               size_t count) {
  std::vector<std::vector<uint8_t> > payloads;
  const int total = (1 << spec.tile_rows_log2) * (1 << spec.tile_cols_log2);
  for (int i = 0; i < total; ++i) {
    const size_t size = 1 + (static_cast<size_t>(i) * 3) % 29;
    payloads.push_back(OpaqueBytes(size, static_cast<uint8_t>(0x10 + i * 5)));
  }
  (void)count;
  return payloads;
}

vp9_parser::Vp9FrameLayout ExpectedLayout(const FrameBuild& frame,
                                           size_t base) {
  vp9_parser::Vp9FrameLayout layout;
  layout.offset = base;
  layout.length = frame.bytes.size();
  if (frame.header.show_existing) {
    layout.show_existing = true;
    layout.existing_index = frame.header.existing_index;
    return layout;
  }
  layout.uncompressed_header_bytes = frame.header.uncompressed_header_bytes;
  layout.compressed_header_offset = base + frame.compressed_header_offset;
  layout.compressed_header_size = frame.compressed_header_size;
  layout.width = frame.header.width;
  layout.height = frame.header.height;
  layout.tile_rows = frame.header.tile_rows;
  layout.tile_columns = frame.header.tile_columns;
  for (size_t i = 0; i < frame.tiles.size(); ++i) {
    vp9_parser::Vp9TileRange tile = frame.tiles[i];
    tile.offset += base;
    layout.tiles.push_back(tile);
  }
  return layout;
}

struct PacketBuild {
  std::vector<uint8_t> bytes;
  std::vector<vp9_parser::Vp9FrameLayout> expected;
};

PacketBuild BuildSinglePacket(const FrameBuild& frame) {
  PacketBuild packet;
  packet.bytes = frame.bytes;
  packet.expected.push_back(ExpectedLayout(frame, 0));
  return packet;
}

PacketBuild BuildSuperframePacket(const std::vector<FrameBuild>& frames,
                                  int size_bytes) {
  PacketBuild packet;
  std::vector<uint32_t> lengths;
  for (size_t i = 0; i < frames.size(); ++i) {
    const size_t base = packet.bytes.size();
    packet.expected.push_back(ExpectedLayout(frames[i], base));
    packet.bytes.insert(packet.bytes.end(), frames[i].bytes.begin(),
                        frames[i].bytes.end());
    lengths.push_back(static_cast<uint32_t>(frames[i].bytes.size()));
  }

  const uint8_t marker = static_cast<uint8_t>(
      0xc0 | (((size_bytes - 1) & 3) << 3) | ((frames.size() - 1) & 7));
  packet.bytes.push_back(marker);
  for (size_t i = 0; i < lengths.size(); ++i) {
    for (int j = 0; j < size_bytes; ++j)
      packet.bytes.push_back(
          static_cast<uint8_t>((lengths[i] >> (8 * j)) & 0xff));
  }
  packet.bytes.push_back(marker);
  return packet;
}

void PrintLayout(const vp9_parser::Vp9FrameLayout& layout, const char* prefix) {
  fprintf(stderr,
          "%s offset=%zu length=%zu uh=%zu ch_off=%zu ch_size=%zu %dx%d "
          "tiles=%d x %d show_existing=%d index=%d ranges=%zu\n",
          prefix, layout.offset, layout.length,
          layout.uncompressed_header_bytes, layout.compressed_header_offset,
          layout.compressed_header_size, layout.width, layout.height,
          layout.tile_rows, layout.tile_columns, layout.show_existing ? 1 : 0,
          layout.existing_index, layout.tiles.size());
}

bool SameLayout(const vp9_parser::Vp9FrameLayout& actual,
                const vp9_parser::Vp9FrameLayout& expected,
                const char* label) {
  bool ok = true;
  const bool scalars_match =
      actual.offset == expected.offset && actual.length == expected.length &&
      actual.uncompressed_header_bytes == expected.uncompressed_header_bytes &&
      actual.compressed_header_offset == expected.compressed_header_offset &&
      actual.compressed_header_size == expected.compressed_header_size &&
      actual.width == expected.width && actual.height == expected.height &&
      actual.tile_rows == expected.tile_rows &&
      actual.tile_columns == expected.tile_columns &&
      actual.show_existing == expected.show_existing &&
      actual.existing_index == expected.existing_index;
  if (!scalars_match) {
    PrintLayout(actual, "actual");
    PrintLayout(expected, "expected");
    ok = false;
  }
  if (actual.tiles.size() != expected.tiles.size()) {
    fprintf(stderr, "%s tile count actual=%zu expected=%zu\n", label,
            actual.tiles.size(), expected.tiles.size());
    ok = false;
  } else {
    for (size_t i = 0; i < expected.tiles.size(); ++i) {
      const vp9_parser::Vp9TileRange& a = actual.tiles[i];
      const vp9_parser::Vp9TileRange& e = expected.tiles[i];
      if (a.offset != e.offset || a.length != e.length || a.row != e.row ||
          a.column != e.column) {
        fprintf(stderr,
                "%s tile[%zu] actual=(%zu,%zu,%d,%d) "
                "expected=(%zu,%zu,%d,%d)\n",
                label, i, a.offset, a.length, a.row, a.column, e.offset,
                e.length, e.row, e.column);
        ok = false;
      }
    }
  }
  return ok;
}

bool CheckInvariants(const std::vector<uint8_t>& packet,
                     const std::vector<vp9_parser::Vp9FrameLayout>& layouts,
                     const char* label) {
  bool ok = true;
  for (size_t fi = 0; fi < layouts.size(); ++fi) {
    const vp9_parser::Vp9FrameLayout& layout = layouts[fi];
    if (layout.offset > packet.size() ||
        layout.length > packet.size() - layout.offset) {
      fprintf(stderr, "%s frame %zu exceeds packet\n", label, fi);
      ok = false;
      continue;
    }
    if (layout.show_existing) {
      if (!layout.tiles.empty() || layout.width != 0 || layout.height != 0 ||
          layout.uncompressed_header_bytes != 0 ||
          layout.compressed_header_offset != 0 ||
          layout.compressed_header_size != 0) {
        fprintf(stderr, "%s show_existing frame %zu not zeroed\n", label, fi);
        ok = false;
      }
      continue;
    }

    const size_t payload_start = layout.offset +
                                 layout.uncompressed_header_bytes +
                                 layout.compressed_header_size;
    if (layout.compressed_header_offset !=
        layout.offset + layout.uncompressed_header_bytes) {
      fprintf(stderr, "%s frame %zu compressed header offset mismatch\n",
              label, fi);
      ok = false;
    }
    if (layout.tiles.empty()) {
      fprintf(stderr, "%s frame %zu has no tiles\n", label, fi);
      ok = false;
      continue;
    }
    if (layout.tiles.size() !=
        static_cast<size_t>(layout.tile_rows) * layout.tile_columns) {
      fprintf(stderr, "%s frame %zu tile count mismatch\n", label, fi);
      ok = false;
    }
    // The first tile is preceded by a four-byte length prefix whenever there
    // is more than one tile; that prefix is not part of the returned range.
    const size_t first_tile_offset =
        payload_start + (layout.tiles.size() > 1 ? 4 : 0);
    if (layout.tiles[0].offset != first_tile_offset) {
      fprintf(stderr, "%s frame %zu first tile not after headers\n", label, fi);
      ok = false;
    }
    for (size_t i = 0; i < layout.tiles.size(); ++i) {
      const vp9_parser::Vp9TileRange& tile = layout.tiles[i];
      if (tile.length == 0) {
        fprintf(stderr, "%s frame %zu tile %zu empty\n", label, fi, i);
        ok = false;
      }
      if (tile.offset < layout.offset ||
          tile.length > layout.offset + layout.length - tile.offset) {
        fprintf(stderr, "%s frame %zu tile %zu outside frame\n", label, fi, i);
        ok = false;
      }
      const int expected_row =
          static_cast<int>(i / static_cast<size_t>(layout.tile_columns));
      const int expected_column =
          static_cast<int>(i % static_cast<size_t>(layout.tile_columns));
      if (tile.row != expected_row || tile.column != expected_column) {
        fprintf(stderr, "%s frame %zu tile %zu not row-major\n", label, fi, i);
        ok = false;
      }
      if (i + 1 < layout.tiles.size()) {
        // Every non-final tile is preceded by its own four-byte length prefix,
        // so the gap before the next tile is 4 bytes unless the next tile is
        // the final one (which has no prefix).
        const size_t gap =
            (i + 1 == layout.tiles.size() - 1) ? 0 : 4;
        if (tile.offset + tile.length + gap != layout.tiles[i + 1].offset) {
          fprintf(stderr, "%s frame %zu tiles not contiguous\n", label, fi);
          ok = false;
        }
      } else if (tile.offset + tile.length != layout.offset + layout.length) {
        fprintf(stderr, "%s frame %zu last tile not at frame end\n", label,
                fi);
        ok = false;
      }
    }
  }
  return ok;
}

bool CheckPacket(const PacketBuild& packet, size_t* last_tile_offset) {
  std::vector<vp9_parser::Vp9FrameLayout> actual;
  if (!vp9_parser::ExtractVP9Tiles(packet.bytes.data(), packet.bytes.size(),
                                   &actual)) {
    fprintf(stderr, "ExtractVP9Tiles failed\n");
    return false;
  }
  if (actual.size() != packet.expected.size()) {
    fprintf(stderr, "frame count actual=%zu expected=%zu\n", actual.size(),
            packet.expected.size());
    return false;
  }
  bool ok = true;
  for (size_t i = 0; i < actual.size(); ++i) {
    if (!SameLayout(actual[i], packet.expected[i], "frame")) {
      ok = false;
      break;
    }
  }
  if (ok && !CheckInvariants(packet.bytes, actual, "packet")) {
    ok = false;
  }
  if (last_tile_offset != NULL && !actual.empty() && !actual[0].tiles.empty()) {
    *last_tile_offset = actual[0].tiles.back().offset;
  }
  return ok;
}

HeaderSpec KeySpec(int profile, int width, int height, int cols_log2,
                   int rows_log2) {
  HeaderSpec spec;
  spec.key = true;
  spec.profile = profile;
  spec.width = width;
  spec.height = height;
  spec.tile_cols_log2 = cols_log2;
  spec.tile_rows_log2 = rows_log2;
  return spec;
}

}  // namespace

// ---------------------------------------------------------------------------
// headers
// ---------------------------------------------------------------------------

TASK_CASE("headers", key_profiles_and_error_resilient) {
  const int profiles[] = {0, 1, 2, 3};
  for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
    for (int er = 0; er <= 1; ++er) {
      HeaderSpec spec = KeySpec(profiles[i], 512, 256, 1, 2);
      spec.error_resilient = (er != 0);
      spec.ten_or_twelve_bit = (profiles[i] >= 2) && (er != 0);
      BitWriter writer;
      const HeaderLayout layout = WriteHeader(spec, &writer);

      TASK_CHECK(TileLog2IsLegal(spec.width, spec.tile_cols_log2));
      vp9_parser::Vp9HeaderParser parser;
      TASK_CHECK(
          parser.ParseUncompressedHeader(writer.bytes().data(),
                                          writer.bytes().size()));
      TASK_CHECK(parser.profile() == profiles[i]);
      TASK_CHECK(parser.width() == 512);
      TASK_CHECK(parser.height() == 256);
      TASK_CHECK(parser.row_tiles() == 4);
      TASK_CHECK(parser.column_tiles() == 2);
      TASK_CHECK(parser.error_resilient_mode() == er);
      TASK_CHECK(parser.frame_parallel_mode() == er);
      TASK_CHECK(layout.tile_rows == 4 && layout.tile_columns == 2);
    }
  }

  // RGB color space takes the other color_config() branch for profiles 1/3.
  for (int profile = 1; profile <= 3; profile += 2) {
    HeaderSpec spec = KeySpec(profile, 320, 240, 0, 0);
    spec.color_space = 7;
    BitWriter writer;
    WriteHeader(spec, &writer);
    vp9_parser::Vp9HeaderParser parser;
    TASK_CHECK(parser.ParseUncompressedHeader(writer.bytes().data(),
                                              writer.bytes().size()));
    TASK_CHECK(parser.profile() == profile);
    TASK_CHECK(parser.width() == 320);
    TASK_CHECK(parser.height() == 240);
    TASK_CHECK(parser.row_tiles() == 1);
    TASK_CHECK(parser.column_tiles() == 1);
  }
}

TASK_CASE("headers", intra_only_and_explicit_inter) {
  // Intra-only frames, with and without error resilience.
  const int intra_profiles[] = {0, 2};
  for (size_t p = 0; p < sizeof(intra_profiles) / sizeof(intra_profiles[0]);
       ++p) {
    for (int er = 0; er <= 1; ++er) {
      HeaderSpec spec = KeySpec(intra_profiles[p], 128, 96, 0, 1);
      spec.key = false;
      spec.show_frame = false;
      spec.intra_only = true;
      spec.error_resilient = (er != 0);
      spec.refresh_frame_flags = 0x11;
      spec.ten_or_twelve_bit = true;

      const std::vector<std::vector<uint8_t> > payloads =
          PayloadsFor(spec, 0);
      const FrameBuild frame =
          BuildFrame(spec, OpaqueBytes(6, 0x50), payloads);
      const PacketBuild packet = BuildSinglePacket(frame);
      TASK_CHECK(CheckPacket(packet, NULL));

      std::vector<vp9_parser::Vp9FrameLayout> layouts;
      TASK_CHECK(vp9_parser::ExtractVP9Tiles(
          packet.bytes.data(), packet.bytes.size(), &layouts));
      TASK_CHECK(layouts.size() == 1);
      TASK_CHECK(layouts[0].width == 128 && layouts[0].height == 96);
      TASK_CHECK(layouts[0].tile_rows == 2);
      TASK_CHECK(layouts[0].tile_columns == 1);
    }
  }

  // Explicit-size inter frames: high precision MV and both interpolation
  // filter encodings.
  for (int variant = 0; variant < 3; ++variant) {
    HeaderSpec spec = KeySpec(0, 512, 256, 1, 0);
    spec.key = false;
    spec.intra_only = false;
    spec.show_frame = true;
    spec.refresh_frame_flags = 0x03;
    spec.ref_frame_idx[0] = 1;
    spec.ref_frame_idx[1] = 2;
    spec.ref_frame_idx[2] = 3;
    spec.ref_sign_bias[0] = 1;
    spec.allow_high_precision_mv = (variant != 0);
    spec.switchable_filter = (variant != 1);
    spec.raw_interp_filter = variant;
    if (variant == 2) {
      spec.error_resilient = true;
      spec.profile = 1;
    }
    TASK_CHECK(TileLog2IsLegal(spec.width, spec.tile_cols_log2));

    const std::vector<std::vector<uint8_t> > payloads =
        PayloadsFor(spec, 0);
    const FrameBuild frame = BuildFrame(spec, OpaqueBytes(4, 0x33), payloads);
    const PacketBuild packet = BuildSinglePacket(frame);
    TASK_CHECK(CheckPacket(packet, NULL));

    std::vector<vp9_parser::Vp9FrameLayout> layouts;
    TASK_CHECK(vp9_parser::ExtractVP9Tiles(
        packet.bytes.data(), packet.bytes.size(), &layouts));
    TASK_CHECK(layouts.size() == 1);
    TASK_CHECK(layouts[0].width == 512 && layouts[0].height == 256);
    TASK_CHECK(layouts[0].tile_rows == 1);
    TASK_CHECK(layouts[0].tile_columns == 2);
  }
}

TASK_CASE("headers", loop_filter_and_quantizer_deltas) {
  // Loop-filter deltas use s(6) (six magnitude bits plus a sign bit); the
  // quantizer deltas use s(4). Exercise all present/absent/sign combinations
  // and confirm the tile fields still land correctly afterwards.
  HeaderSpec spec = KeySpec(1, 1024, 512, 2, 1);
  TASK_CHECK(TileLog2IsLegal(spec.width, spec.tile_cols_log2));
  spec.lf_delta_enabled = true;
  spec.lf_delta_update = true;
  spec.lf_ref_update[0] = true;
  spec.lf_ref_delta[0] = -7;
  spec.lf_ref_update[1] = false;  // absent
  spec.lf_ref_update[2] = true;
  spec.lf_ref_delta[2] = 63;
  spec.lf_ref_update[3] = true;
  spec.lf_ref_delta[3] = 0;
  spec.lf_mode_update[0] = true;
  spec.lf_mode_delta[0] = -32;
  spec.lf_mode_update[1] = false;
  spec.delta_q_coded[0] = true;
  spec.delta_q[0] = -8;
  spec.delta_q_coded[1] = false;
  spec.delta_q_coded[2] = true;
  spec.delta_q[2] = 7;

  BitWriter writer;
  const HeaderLayout layout = WriteHeader(spec, &writer);
  TASK_CHECK(layout.tile_rows == 2);
  TASK_CHECK(layout.tile_columns == 4);

  vp9_parser::Vp9HeaderParser parser;
  TASK_CHECK(parser.ParseUncompressedHeader(writer.bytes().data(),
                                            writer.bytes().size()));
  TASK_CHECK(parser.width() == 1024);
  TASK_CHECK(parser.height() == 512);
  TASK_CHECK(parser.row_tiles() == 2);
  TASK_CHECK(parser.column_tiles() == 4);

  // Without delta updates the same header must still land on the same tiles.
  HeaderSpec plain = KeySpec(1, 1024, 512, 2, 1);
  BitWriter plain_writer;
  const HeaderLayout plain_layout = WriteHeader(plain, &plain_writer);
  TASK_CHECK(plain_layout.tile_rows == layout.tile_rows);
  TASK_CHECK(plain_layout.tile_columns == layout.tile_columns);
  vp9_parser::Vp9HeaderParser plain_parser;
  TASK_CHECK(plain_parser.ParseUncompressedHeader(plain_writer.bytes().data(),
                                                   plain_writer.bytes().size()));
  TASK_CHECK(plain_parser.row_tiles() == 2);
  TASK_CHECK(plain_parser.column_tiles() == 4);

  // End-to-end tile extraction with the delta-heavy header.
  const std::vector<std::vector<uint8_t> > payloads = PayloadsFor(spec, 0);
  const FrameBuild frame = BuildFrame(spec, OpaqueBytes(3, 0x28), payloads);
  const PacketBuild packet = BuildSinglePacket(frame);
  TASK_CHECK(CheckPacket(packet, NULL));
}

TASK_CASE("headers", segmentation_map_features_and_level_stats) {
  // Variant 0: map update, temporal prediction update, absolute data.
  // Variant 1: map update without temporal update, delta data.
  for (int variant = 0; variant < 2; ++variant) {
    HeaderSpec spec = KeySpec(0, 512, 256, 1, 2);
    spec.seg.enabled = true;
    spec.seg.update_map = true;
    spec.seg.temporal_update = (variant == 0);
    spec.seg.update_data = true;
    spec.seg.abs_or_delta = (variant == 0);

    // Segment 0: signed negative, zero magnitude with sign, unsigned, and a
    // zero-width enabled feature.
    spec.seg.feature_enabled[0][0] = true;
    spec.seg.feature_value[0][0] = -3;
    spec.seg.feature_enabled[0][1] = true;
    spec.seg.feature_value[0][1] = 0;
    spec.seg.feature_enabled[0][2] = true;
    spec.seg.feature_value[0][2] = 2;
    spec.seg.feature_enabled[0][3] = true;
    // Segment 1: positive signed values.
    spec.seg.feature_enabled[1][0] = true;
    spec.seg.feature_value[1][0] = 5;
    spec.seg.feature_enabled[1][1] = true;
    spec.seg.feature_value[1][1] = -10;
    spec.seg.feature_enabled[1][2] = true;
    spec.seg.feature_value[1][2] = 0;
    // Segment 2: zero magnitude signed and zero-width unsigned.
    spec.seg.feature_enabled[2][0] = true;
    spec.seg.feature_value[2][0] = 0;
    spec.seg.feature_enabled[2][3] = true;
    // Segment 4: mixed.
    spec.seg.feature_enabled[4][1] = true;
    spec.seg.feature_value[4][1] = 7;
    spec.seg.feature_enabled[4][2] = true;
    spec.seg.feature_value[4][2] = 3;

    BitWriter writer;
    const HeaderLayout header = WriteHeader(spec, &writer);
    TASK_CHECK(header.tile_rows == 4 && header.tile_columns == 2);
    TASK_CHECK(header.width == 512 && header.height == 256);

    // The legacy entry point must consume the full segmentation syntax and
    // still land on the tile fields.
    vp9_parser::Vp9HeaderParser parser;
    TASK_CHECK(parser.ParseUncompressedHeader(writer.bytes().data(),
                                              writer.bytes().size()));
    TASK_CHECK(parser.width() == 512);
    TASK_CHECK(parser.height() == 256);
    TASK_CHECK(parser.row_tiles() == 4);
    TASK_CHECK(parser.column_tiles() == 2);

    // The corrected parser must feed the real level stats the right column
    // count.
    vp9_parser::Vp9LevelStats stats;
    stats.AddFrame(parser, 0);
    TASK_CHECK(stats.GetMaxColumnTiles() == 2);

    // And the range extractor must agree end-to-end.
    const std::vector<std::vector<uint8_t> > payloads =
        PayloadsFor(spec, 0);
    const FrameBuild frame = BuildFrame(spec, OpaqueBytes(5, 0x77), payloads);
    const PacketBuild packet = BuildSinglePacket(frame);
    TASK_CHECK(CheckPacket(packet, NULL));
  }
}

TASK_CASE("headers", legacy_plain_and_superframe_index) {
  // Ordinary, non-segmented header through the historical entry point.
  HeaderSpec spec = KeySpec(0, 512, 256, 1, 2);
  BitWriter writer;
  WriteHeader(spec, &writer);
  vp9_parser::Vp9HeaderParser parser;
  TASK_CHECK(parser.ParseUncompressedHeader(writer.bytes().data(),
                                            writer.bytes().size()));
  TASK_CHECK(parser.width() == 512);
  TASK_CHECK(parser.height() == 256);
  TASK_CHECK(parser.row_tiles() == 4);
  TASK_CHECK(parser.column_tiles() == 2);

  // The existing superframe index helper must keep its documented behavior.
  libwebm::Ranges ranges;
  bool error = false;
  const unsigned char indexed[] = {1, 2, 3, 4, 5, 6, 0xc0, 6, 0xc0};
  TASK_CHECK(libwebm::ParseVP9SuperFrameIndex(indexed, sizeof(indexed), &ranges,
                                              &error));
  TASK_CHECK(!error);
  TASK_CHECK(ranges.size() == 1);
  TASK_CHECK(ranges[0].offset == 0);
  TASK_CHECK(ranges[0].length == 6);
}

TASK_CASE("headers", inherited_size_inter_rejected) {
  // An inter frame whose size is inherited from a reference: the legacy parser
  // still accepts it, but the range extractor must refuse rather than guess.
  HeaderSpec spec = KeySpec(0, 512, 256, 0, 0);
  spec.key = false;
  spec.show_frame = true;
  spec.refresh_frame_flags = 0x01;
  spec.ref_frame_idx[0] = 0;
  spec.ref_frame_idx[1] = 1;
  spec.ref_frame_idx[2] = 2;
  spec.found_ref = true;

  BitWriter writer;
  WriteHeader(spec, &writer);

  vp9_parser::Vp9HeaderParser parser;
  TASK_CHECK(parser.ParseUncompressedHeader(writer.bytes().data(),
                                            writer.bytes().size()));

  std::vector<vp9_parser::Vp9FrameLayout> sentinel(1);
  sentinel[0].width = 4242;
  std::vector<uint8_t> packet = writer.bytes();
  TASK_CHECK(!vp9_parser::ExtractVP9Tiles(packet.data(), packet.size(),
                                          &sentinel));
  TASK_CHECK(sentinel.size() == 1);
  TASK_CHECK(sentinel[0].width == 4242);
}

// ---------------------------------------------------------------------------
// tiles
// ---------------------------------------------------------------------------

TASK_CASE("tiles", single_tile_range) {
  HeaderSpec spec = KeySpec(0, 64, 64, 0, 0);
  const std::vector<std::vector<uint8_t> > payloads =
      PayloadsFor(spec, 0);
  const FrameBuild frame = BuildFrame(spec, OpaqueBytes(3, 0x21), payloads);
  const PacketBuild packet = BuildSinglePacket(frame);

  TASK_CHECK(packet.expected.size() == 1);
  TASK_CHECK(packet.expected[0].tiles.size() == 1);
  TASK_CHECK(packet.expected[0].tiles[0].length == payloads[0].size());

  std::vector<vp9_parser::Vp9FrameLayout> layouts;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(
      packet.bytes.data(), packet.bytes.size(), &layouts));
  TASK_CHECK(layouts.size() == 1);
  TASK_CHECK(layouts[0].tiles.size() == 1);

  const vp9_parser::Vp9TileRange& tile = layouts[0].tiles[0];
  TASK_CHECK(tile.offset ==
             layouts[0].offset + layouts[0].uncompressed_header_bytes +
                 layouts[0].compressed_header_size);
  TASK_CHECK(tile.offset == packet.expected[0].tiles[0].offset);
  TASK_CHECK(tile.length == packet.expected[0].tiles[0].length);
  TASK_CHECK(tile.offset + tile.length ==
             layouts[0].offset + layouts[0].length);
  TASK_CHECK(CheckInvariants(packet.bytes, layouts, "single"));
}

TASK_CASE("tiles", multi_row_column_unequal_payloads) {
  HeaderSpec spec = KeySpec(1, 4096, 512, 4, 1);  // 16 columns, 2 rows.
  TASK_CHECK(TileLog2IsLegal(spec.width, spec.tile_cols_log2));

  std::vector<std::vector<uint8_t> > payloads;
  for (int i = 0; i < 32; ++i) {
    const size_t sizes[] = {1, 17, 3, 64, 250, 2, 33, 5};
    payloads.push_back(
        OpaqueBytes(sizes[i % 8], static_cast<uint8_t>(0x30 + i)));
  }

  const FrameBuild frame = BuildFrame(spec, OpaqueBytes(9, 0x11), payloads);
  const PacketBuild packet = BuildSinglePacket(frame);
  TASK_CHECK(packet.expected[0].tiles.size() == 32);
  TASK_CHECK(CheckPacket(packet, NULL));

  std::vector<vp9_parser::Vp9FrameLayout> layouts;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(
      packet.bytes.data(), packet.bytes.size(), &layouts));
  TASK_CHECK(layouts.size() == 1);
  TASK_CHECK(layouts[0].tile_rows == 2);
  TASK_CHECK(layouts[0].tile_columns == 16);
  for (size_t i = 0; i < layouts[0].tiles.size(); ++i) {
    TASK_CHECK(layouts[0].tiles[i].row == static_cast<int>(i / 16));
    TASK_CHECK(layouts[0].tiles[i].column == static_cast<int>(i % 16));
    TASK_CHECK(layouts[0].tiles[i].length == payloads[i].size());
  }
}

TASK_CASE("tiles", compressed_header_pseudo_lengths) {
  // Bytes inside the compressed header that look like tile length prefixes
  // must be skipped wholesale; they are not tile boundaries.
  HeaderSpec spec = KeySpec(0, 512, 256, 1, 1);  // 2 columns, 2 rows.
  std::vector<uint8_t> compressed_header;
  const uint8_t fake[] = {0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
                          0x00, 0x09, 0xff, 0xff, 0xff, 0xff,
                          0x00, 0x00, 0x00, 0x00};
  compressed_header.assign(fake, fake + sizeof(fake));

  std::vector<std::vector<uint8_t> > payloads;
  payloads.push_back(OpaqueBytes(6, 0xa1));
  payloads.push_back(OpaqueBytes(4, 0xb2));
  payloads.push_back(OpaqueBytes(8, 0xc3));
  payloads.push_back(OpaqueBytes(2, 0xd4));

  const FrameBuild frame = BuildFrame(spec, compressed_header, payloads);
  const PacketBuild packet = BuildSinglePacket(frame);

  std::vector<vp9_parser::Vp9FrameLayout> layouts;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(
      packet.bytes.data(), packet.bytes.size(), &layouts));
  TASK_CHECK(layouts.size() == 1);
  TASK_CHECK(layouts[0].compressed_header_size == compressed_header.size());
  // The compressed header starts immediately after the uncompressed header.
  TASK_CHECK(layouts[0].compressed_header_offset ==
             layouts[0].uncompressed_header_bytes);
  const size_t payload_start = layouts[0].uncompressed_header_bytes +
                               layouts[0].compressed_header_size;
  // The first of four tiles is preceded by its own 4-byte length prefix.
  TASK_CHECK(layouts[0].tiles[0].offset == payload_start + 4);
  // If the fake prefix were interpreted, the first tile would begin earlier
  // or the lengths would differ from the written layout.
  TASK_CHECK(SameLayout(layouts[0], packet.expected[0], "pseudo_lengths"));
  TASK_CHECK(CheckInvariants(packet.bytes, layouts, "pseudo_lengths"));
}

TASK_CASE("tiles", byte_by_byte_truncation) {
  HeaderSpec spec = KeySpec(0, 1024, 256, 2, 0);  // 4 columns, 1 row.
  std::vector<std::vector<uint8_t> > payloads;
  payloads.push_back(OpaqueBytes(5, 0x01));
  payloads.push_back(OpaqueBytes(11, 0x02));
  payloads.push_back(OpaqueBytes(7, 0x03));
  payloads.push_back(OpaqueBytes(9, 0x04));

  const FrameBuild frame = BuildFrame(spec, OpaqueBytes(4, 0x99), payloads);
  const PacketBuild packet = BuildSinglePacket(frame);
  TASK_CHECK(CheckPacket(packet, NULL));

  const std::vector<uint8_t>& full = packet.bytes;
  const std::vector<vp9_parser::Vp9TileRange>& expected_tiles =
      packet.expected[0].tiles;
  const size_t last_tile_offset = expected_tiles.back().offset;
  const size_t last_tile_length = expected_tiles.back().length;

  // For a multi-tile frame the "last tile" is only valid once its preceding
  // four-byte length prefix is complete. The prefix sits immediately before
  // the final tile's first payload byte. Compute that boundary from the
  // recorded layout so the loop below does not rely on production parsing.
  const size_t last_prefix_offset = last_tile_offset - 4;

  // Every truncation that leaves a tile prefix incomplete, or that removes any
  // earlier tile's payload, must be rejected and must leave the output
  // untouched.
  for (size_t length = 1; length <= last_prefix_offset + 4; ++length) {
    std::vector<vp9_parser::Vp9FrameLayout> out(1);
    out[0].width = 777;
    out[0].height = 888;
    std::vector<uint8_t> truncated(full.begin(), full.begin() + length);
    const bool parsed =
        vp9_parser::ExtractVP9Tiles(truncated.data(), truncated.size(), &out);
    TASK_CHECK(!parsed);
    TASK_CHECK(out.size() == 1);
    TASK_CHECK(out[0].width == 777 && out[0].height == 888);
    TASK_CHECK(truncated.size() == length);
  }

  // A truncation exactly at the final tile's first payload byte leaves a
  // zero-length final tile, which must also be rejected.
  {
    std::vector<vp9_parser::Vp9FrameLayout> out(1);
    out[0].width = 321;
    std::vector<uint8_t> truncated(full.begin(),
                                   full.begin() + last_tile_offset);
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(truncated.data(), truncated.size(),
                                            &out));
    TASK_CHECK(out.size() == 1 && out[0].width == 321);
  }

  // Truncations strictly inside the final tile produce a shorter but still
  // valid last tile; every preceding range is unchanged.
  for (size_t length = last_tile_offset + 1; length <= full.size(); ++length) {
    std::vector<vp9_parser::Vp9FrameLayout> out;
    std::vector<uint8_t> truncated(full.begin(), full.begin() + length);
    TASK_CHECK(vp9_parser::ExtractVP9Tiles(truncated.data(), truncated.size(),
                                           &out));
    TASK_CHECK(out.size() == 1);
    TASK_CHECK(out[0].tiles.size() == expected_tiles.size());
    TASK_CHECK(out[0].tiles.back().length == length - last_tile_offset);
    TASK_CHECK(out[0].tiles.back().length <= last_tile_length);
    for (size_t i = 0; i + 1 < out[0].tiles.size(); ++i) {
      TASK_CHECK(out[0].tiles[i].offset == expected_tiles[i].offset);
      TASK_CHECK(out[0].tiles[i].length == expected_tiles[i].length);
    }
    TASK_CHECK(CheckInvariants(truncated, out, "truncated"));
  }
}

TASK_CASE("tiles", malformed_tile_structure_rejected) {
  // Build a valid 2x2 frame, then introduce each structural defect in turn and
  // require rejection with the output sentinel preserved.
  HeaderSpec spec = KeySpec(0, 512, 256, 1, 1);
  std::vector<std::vector<uint8_t> > payloads;
  payloads.push_back(OpaqueBytes(6, 0x51));
  payloads.push_back(OpaqueBytes(4, 0x52));
  payloads.push_back(OpaqueBytes(8, 0x53));
  payloads.push_back(OpaqueBytes(2, 0x54));
  const FrameBuild frame = BuildFrame(spec, OpaqueBytes(4, 0x61), payloads);
  const PacketBuild packet = BuildSinglePacket(frame);
  TASK_CHECK(CheckPacket(packet, NULL));

  const size_t first_prefix = frame.header.uncompressed_header_bytes +
                              frame.compressed_header_size;

  // 1) Compressed header size declared past the frame end.
  {
    std::vector<uint8_t> bad = packet.bytes;
    // Overwrite the 16-bit compressed header size using the recorded bit
    // position, then pad with ones so the padding check cannot be blamed.
    size_t bit = frame.header.compressed_header_size_bit_offset;
    for (int i = 0; i < 16; ++i, ++bit) {
      const uint8_t mask = static_cast<uint8_t>(1u << (7 - (bit & 7)));
      bad[bit >> 3] = static_cast<uint8_t>(bad[bit >> 3] | mask);
    }
    std::vector<vp9_parser::Vp9FrameLayout> out(1);
    out[0].width = 1111;
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(bad.data(), bad.size(), &out));
    TASK_CHECK(out.size() == 1 && out[0].width == 1111);
  }

  // 2) Missing tile prefix: truncate two bytes into the first prefix.
  {
    std::vector<uint8_t> bad(packet.bytes.begin(),
                             packet.bytes.begin() + first_prefix + 2);
    std::vector<vp9_parser::Vp9FrameLayout> out(1);
    out[0].width = 2222;
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(bad.data(), bad.size(), &out));
    TASK_CHECK(out.size() == 1 && out[0].width == 2222);
  }

  // 3) Over-long declared tile length: the first prefix claims more bytes than
  // the frame holds.
  {
    std::vector<uint8_t> bad = packet.bytes;
    bad[first_prefix + 0] = 0x7f;
    bad[first_prefix + 1] = 0xff;
    bad[first_prefix + 2] = 0xff;
    bad[first_prefix + 3] = 0xff;
    std::vector<vp9_parser::Vp9FrameLayout> out(1);
    out[0].width = 3333;
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(bad.data(), bad.size(), &out));
    TASK_CHECK(out.size() == 1 && out[0].width == 3333);
  }

  // 4) Empty tile: the first prefix declares zero bytes.
  {
    std::vector<uint8_t> bad = packet.bytes;
    bad[first_prefix + 0] = 0;
    bad[first_prefix + 1] = 0;
    bad[first_prefix + 2] = 0;
    bad[first_prefix + 3] = 0;
    std::vector<vp9_parser::Vp9FrameLayout> out(1);
    out[0].width = 4444;
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(bad.data(), bad.size(), &out));
    TASK_CHECK(out.size() == 1 && out[0].width == 4444);
  }

  // 5) A huge frame whose tile payload cannot possibly fit in the packet: the
  // header parses, but the tile structure does not fit.
  {
    HeaderSpec big = KeySpec(0, 65536, 65536, 4, 2);
    TASK_CHECK(TileLog2IsLegal(big.width, big.tile_cols_log2));
    BitWriter writer;
    WriteHeader(big, &writer);
    std::vector<uint8_t> bad = writer.bytes();
    std::vector<vp9_parser::Vp9FrameLayout> out(1);
    out[0].width = 5555;
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(bad.data(), bad.size(), &out));
    TASK_CHECK(out.size() == 1 && out[0].width == 5555);
  }

  // 6) Non-zero padding after the uncompressed header must be rejected. Set
  // the last padding bit to one using the recorded bit layout.
  {
    TASK_CHECK(frame.header.padding_bits > 0);
    std::vector<uint8_t> bad = packet.bytes;
    const size_t padding_bit =
        frame.header.compressed_header_size_bit_offset + 16 +
        static_cast<size_t>(frame.header.padding_bits - 1);
    bad[padding_bit >> 3] |=
        static_cast<uint8_t>(1u << (7 - (padding_bit & 7)));
    std::vector<vp9_parser::Vp9FrameLayout> out(1);
    out[0].width = 6666;
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(bad.data(), bad.size(), &out));
    TASK_CHECK(out.size() == 1 && out[0].width == 6666);
  }
}

// ---------------------------------------------------------------------------
// packets
// ---------------------------------------------------------------------------

TASK_CASE("packets", superframe_with_show_existing) {
  HeaderSpec key_spec = KeySpec(0, 512, 256, 1, 1);
  const std::vector<std::vector<uint8_t> > key_payloads =
      PayloadsFor(key_spec, 0);
  const FrameBuild key_frame =
      BuildFrame(key_spec, OpaqueBytes(6, 0x44), key_payloads);

  // A one-byte show_existing frame: marker(2) + profile(2) + show_existing(1)
  // + frame_to_show_map_idx(3).
  HeaderSpec show_spec;
  show_spec.show_existing = true;
  show_spec.existing_index = 5;
  BitWriter show_writer;
  const HeaderLayout show_header = WriteHeader(show_spec, &show_writer);
  TASK_CHECK(show_header.show_existing);
  TASK_CHECK(show_header.existing_index == 5);
  TASK_CHECK(show_writer.bytes().size() == 1);

  FrameBuild show_frame;
  show_frame.bytes = show_writer.bytes();
  show_frame.header = show_header;

  std::vector<FrameBuild> frames;
  frames.push_back(key_frame);
  frames.push_back(show_frame);
  const PacketBuild packet = BuildSuperframePacket(frames, 1);

  std::vector<vp9_parser::Vp9FrameLayout> layouts;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(
      packet.bytes.data(), packet.bytes.size(), &layouts));
  TASK_CHECK(layouts.size() == 2);
  TASK_CHECK(layouts[0].show_existing == false);
  TASK_CHECK(layouts[1].show_existing == true);
  TASK_CHECK(layouts[1].existing_index == 5);
  TASK_CHECK(layouts[1].width == 0 && layouts[1].height == 0);
  TASK_CHECK(layouts[1].uncompressed_header_bytes == 0);
  TASK_CHECK(layouts[1].compressed_header_offset == 0);
  TASK_CHECK(layouts[1].compressed_header_size == 0);
  TASK_CHECK(layouts[1].tiles.empty());
  TASK_CHECK(layouts[1].offset == layouts[0].offset + layouts[0].length);
  TASK_CHECK(layouts[1].length == 1);
  TASK_CHECK(SameLayout(layouts[0], packet.expected[0], "key"));
  TASK_CHECK(SameLayout(layouts[1], packet.expected[1], "show_existing"));
  TASK_CHECK(CheckInvariants(packet.bytes, layouts, "show_existing"));
}

TASK_CASE("packets", index_widths_and_frame_counts) {
  for (int size_bytes = 1; size_bytes <= 4; ++size_bytes) {
    for (int count = 1; count <= 8; ++count) {
      std::vector<FrameBuild> frames;
      for (int i = 0; i < count; ++i) {
        HeaderSpec spec = KeySpec(0, 64, 64, 0, 0);
        // Vary payload length so child sizes are not uniform.
        std::vector<std::vector<uint8_t> > payloads;
        payloads.push_back(OpaqueBytes(static_cast<size_t>(1 + i * 3),
                                       static_cast<uint8_t>(0x60 + i)));
        const FrameBuild frame =
            BuildFrame(spec, OpaqueBytes(2, static_cast<uint8_t>(i)), payloads);
        TASK_CHECK(frame.bytes.size() <= 255);
        frames.push_back(frame);
      }

      const PacketBuild packet = BuildSuperframePacket(frames, size_bytes);
      std::vector<vp9_parser::Vp9FrameLayout> layouts;
      TASK_CHECK(vp9_parser::ExtractVP9Tiles(
          packet.bytes.data(), packet.bytes.size(), &layouts));
      TASK_CHECK(layouts.size() == static_cast<size_t>(count));
      for (int i = 0; i < count; ++i) {
        TASK_CHECK(SameLayout(layouts[i], packet.expected[i], "index_width"));
        const size_t next_offset =
            (i + 1 < count) ? layouts[i + 1].offset : packet.bytes.size();
        TASK_CHECK(layouts[i].offset + layouts[i].length <= next_offset);
      }
      TASK_CHECK(CheckInvariants(packet.bytes, layouts, "index_width"));
    }
  }
}

TASK_CASE("packets", adjacent_subframe_boundaries) {
  HeaderSpec spec = KeySpec(2, 1024, 512, 2, 1);  // 4 columns, 2 rows.
  TASK_CHECK(TileLog2IsLegal(spec.width, spec.tile_cols_log2));
  std::vector<std::vector<uint8_t> > payloads;
  for (int i = 0; i < 8; ++i)
    payloads.push_back(OpaqueBytes(static_cast<size_t>(4 + i),
                                   static_cast<uint8_t>(0x80 + i)));

  FrameBuild first = BuildFrame(spec, OpaqueBytes(7, 0x01), payloads);
  FrameBuild second = BuildFrame(spec, OpaqueBytes(5, 0x02), payloads);
  std::vector<FrameBuild> frames;
  frames.push_back(first);
  frames.push_back(second);
  const PacketBuild packet = BuildSuperframePacket(frames, 2);

  std::vector<vp9_parser::Vp9FrameLayout> layouts;
  TASK_CHECK(vp9_parser::ExtractVP9Tiles(
      packet.bytes.data(), packet.bytes.size(), &layouts));
  TASK_CHECK(layouts.size() == 2);

  const vp9_parser::Vp9FrameLayout& a = layouts[0];
  const vp9_parser::Vp9FrameLayout& b = layouts[1];
  TASK_CHECK(a.offset + a.length == b.offset);
  TASK_CHECK(a.tiles.back().offset + a.tiles.back().length ==
             a.offset + a.length);
  TASK_CHECK(b.tiles.front().offset >= b.offset);
  TASK_CHECK(a.tiles.back().offset + a.tiles.back().length <= b.offset);
  for (size_t i = 0; i < a.tiles.size(); ++i) {
    TASK_CHECK(a.tiles[i].offset + a.tiles[i].length <= b.offset);
  }
  TASK_CHECK(SameLayout(a, packet.expected[0], "first"));
  TASK_CHECK(SameLayout(b, packet.expected[1], "second"));
  TASK_CHECK(CheckInvariants(packet.bytes, layouts, "boundaries"));
}

TASK_CASE("packets", out_sentinel_and_corrupt_index) {
  HeaderSpec spec = KeySpec(0, 128, 128, 0, 1);
  const std::vector<std::vector<uint8_t> > payloads =
      PayloadsFor(spec, 0);
  FrameBuild frame = BuildFrame(spec, OpaqueBytes(4, 0x12), payloads);
  std::vector<FrameBuild> frames;
  frames.push_back(frame);
  frames.push_back(frame);
  const PacketBuild good = BuildSuperframePacket(frames, 1);

  // Success replaces the previous contents of out.
  {
    std::vector<vp9_parser::Vp9FrameLayout> out(3);
    out[0].width = 999;
    TASK_CHECK(vp9_parser::ExtractVP9Tiles(good.bytes.data(), good.bytes.size(),
                                           &out));
    TASK_CHECK(out.size() == 2);
    TASK_CHECK(out[0].width == 128);
  }

  // A corrupt index (first marker byte differs) must fail, not fall back to
  // treating the packet as one ordinary frame.
  {
    std::vector<uint8_t> corrupt = good.bytes;
    const int num_frames = 2;
    const int size_bytes = 1;
    const size_t index_length = 2 + num_frames * size_bytes;
    corrupt[corrupt.size() - index_length] ^= 0xff;

    std::vector<uint8_t> before = corrupt;
    std::vector<vp9_parser::Vp9FrameLayout> out(1);
    out[0].width = 1234;
    out[0].height = 5678;
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(corrupt.data(), corrupt.size(),
                                            &out));
    TASK_CHECK(out.size() == 1);
    TASK_CHECK(out[0].width == 1234 && out[0].height == 5678);
    TASK_CHECK(corrupt == before);
  }

  // A corrupt index whose child sizes no longer sum to the payload length must
  // also fail.
  {
    std::vector<uint8_t> corrupt = good.bytes;
    const int num_frames = 2;
    const int size_bytes = 1;
    const size_t index_length = 2 + num_frames * size_bytes;
    // First size byte follows the leading marker.
    const size_t first_size = corrupt.size() - index_length + 1;
    corrupt[first_size] = static_cast<uint8_t>(corrupt[first_size] - 1);

    std::vector<vp9_parser::Vp9FrameLayout> out(1);
    out[0].width = 4321;
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(corrupt.data(), corrupt.size(),
                                            &out));
    TASK_CHECK(out.size() == 1);
    TASK_CHECK(out[0].width == 4321);
  }

  // Null / empty inputs are rejected without touching out.
  {
    std::vector<vp9_parser::Vp9FrameLayout> out(1);
    out[0].width = 2468;
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(NULL, 0, &out));
    TASK_CHECK(out.size() == 1 && out[0].width == 2468);
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(good.bytes.data(), 0, &out));
    TASK_CHECK(out.size() == 1 && out[0].width == 2468);
    TASK_CHECK(!vp9_parser::ExtractVP9Tiles(good.bytes.data(),
                                            good.bytes.size(), NULL));
  }
}
