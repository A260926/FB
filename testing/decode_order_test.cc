// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
//
// Standalone test for the Segment decode order mode. It muxes short Matroska
// streams through the public mkvmuxer interface, then reads them back both with
// mkvparser and with an independent EBML field reader implemented below. It
// does not use GoogleTest or any external media.

#include <stdint.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mkvmuxer/mkvmuxer.h"
#include "mkvparser/mkvparser.h"

namespace {

const uint64_t kMs = 1000000ULL;       // One millisecond in nanoseconds.
const uint64_t kScale3Ms = 3000000ULL;  // Non-default timecode scale.

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,        \
                   __LINE__, #condition);                                  \
      std::abort();                                                        \
    }                                                                      \
  } while (0)

// In-memory IMkvWriter.
class MemoryWriter : public mkvmuxer::IMkvWriter {
 public:
  MemoryWriter() : pos_(0) {}

  mkvmuxer::int32 Write(const void* buf, mkvmuxer::uint32 len) override {
    const int64_t end = pos_ + static_cast<int64_t>(len);
    if (end > static_cast<int64_t>(bytes.size()))
      bytes.resize(static_cast<size_t>(end));
    std::memcpy(bytes.data() + pos_, buf, len);
    pos_ = end;
    return 0;
  }
  mkvmuxer::int64 Position() const override { return pos_; }
  mkvmuxer::int32 Position(mkvmuxer::int64 position) override {
    if (position < 0)
      return -1;
    pos_ = position;
    if (pos_ > static_cast<int64_t>(bytes.size()))
      bytes.resize(static_cast<size_t>(pos_));
    return 0;
  }
  bool Seekable() const override { return seekable_; }
  void ElementStartNotify(mkvmuxer::uint64, mkvmuxer::int64) override {}

  void set_seekable(bool seekable) { seekable_ = seekable; }

  std::vector<uint8_t> bytes;

 private:
  int64_t pos_;
  bool seekable_ = true;
};

// In-memory IMkvReader.
class MemoryReader : public mkvparser::IMkvReader {
 public:
  explicit MemoryReader(const std::vector<uint8_t>& data) : data_(data) {}

  int Read(long long pos, long len, unsigned char* buf) override {
    if (pos < 0 || len < 0 ||
        pos + len > static_cast<long long>(data_.size())) {
      return -1;
    }
    std::memcpy(buf, data_.data() + pos, static_cast<size_t>(len));
    return 0;
  }
  int Length(long long* total, long long* available) override {
    if (total)
      *total = static_cast<long long>(data_.size());
    if (available)
      *available = static_cast<long long>(data_.size());
    return 0;
  }

 private:
  const std::vector<uint8_t>& data_;
};

///////////////////////////////////////////////////////////////
// Independent EBML field reader. It knows nothing about mkvmuxer internals.
///////////////////////////////////////////////////////////////

struct BlockRecord {
  uint64_t track = 0;
  int64_t relative_timecode = 0;
  bool is_simple_block = false;
  std::vector<int64_t> reference_blocks;
  bool has_duration = false;
  uint64_t duration = 0;
  std::vector<uint8_t> payload;
};

struct ClusterRecord {
  uint64_t timecode = 0;
  uint64_t segment_relative_offset = 0;
  std::vector<BlockRecord> blocks;
};

struct CueRecord {
  uint64_t time = 0;
  uint64_t track = 0;
  uint64_t cluster_position = 0;
  bool has_block_number = false;
  uint64_t block_number = 0;
};

struct ParsedMatroska {
  uint64_t timecode_scale = 1000000;
  double info_duration = 0.0;
  int64_t segment_payload_offset = 0;
  std::vector<ClusterRecord> clusters;
  std::vector<CueRecord> cues;
};

class EbmlReader {
 public:
  explicit EbmlReader(const std::vector<uint8_t>& data) : data_(data) {}

  ParsedMatroska Parse() {
    ParsedMatroska out;
    size_t pos = 0;
    while (pos < data_.size()) {
      const VInt id = ReadVInt(pos);
      const VInt size = ReadVInt(pos + id.length);
      const size_t payload = pos + id.length + size.length;
      if (id.raw == libwebm::kMkvSegment) {
        out.segment_payload_offset = static_cast<int64_t>(payload);
        const size_t stop = size.unknown ? data_.size() : payload + size.value;
        ParseSegmentPayload(payload, stop, &out);
        return out;
      }
      pos = size.unknown ? data_.size() : payload + static_cast<size_t>(size.value);
    }
    CHECK(false);  // A Segment element must be present.
    return out;
  }

 private:
  struct VInt {
    uint64_t value = 0;   // Value with the length marker removed.
    uint64_t raw = 0;    // Full value including the length marker bit.
    int length = 0;
    bool unknown = false;
  };

  VInt ReadVInt(size_t pos) {
    CHECK(pos < data_.size());
    const uint8_t first = data_[pos];
    int num_bytes = 1;
    for (uint8_t mask = 0x80; mask != 0 && (first & mask) == 0; mask >>= 1)
      ++num_bytes;
    CHECK(num_bytes <= 8 && pos + static_cast<size_t>(num_bytes) <= data_.size());

    VInt result;
    result.length = num_bytes;
    uint64_t raw = 0;
    for (int i = 0; i < num_bytes; ++i)
      raw = (raw << 8) | data_[pos + i];
    result.raw = raw;

    const int data_bits = 7 * num_bytes;
    const uint64_t all_ones =
        (data_bits == 64) ? ~0ULL : ((1ULL << data_bits) - 1);
    result.unknown = (raw & all_ones) == all_ones;
    result.value = data_bits == 64 ? raw : (raw & ((1ULL << data_bits) - 1));
    return result;
  }

  uint64_t ReadUInt(size_t pos, size_t size) {
    CHECK(size <= 8 && pos + size <= data_.size());
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i)
      value = (value << 8) | data_[pos + i];
    return value;
  }

  int64_t ReadSInt(size_t pos, size_t size) {
    CHECK(size <= 8 && pos + size <= data_.size());
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i)
      value = (value << 8) | data_[pos + i];
    if (size < 8 && (value & (1ULL << (size * 8 - 1))))
      value |= ~((1ULL << (size * 8)) - 1);
    return static_cast<int64_t>(value);
  }

  double ReadFloat(size_t pos, size_t size) {
    if (size == 4) {
      uint32_t bits = static_cast<uint32_t>(ReadUInt(pos, size));
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return static_cast<double>(f);
    }
    CHECK(size == 8);
    uint64_t bits = ReadUInt(pos, size);
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
  }

  void ParseSegmentPayload(size_t start, size_t stop, ParsedMatroska* out) {
    size_t pos = start;
    while (pos < stop) {
      const VInt id = ReadVInt(pos);
      const VInt size = ReadVInt(pos + id.length);
      const size_t payload = pos + id.length + size.length;
      const size_t child_stop =
          size.unknown ? stop : payload + static_cast<size_t>(size.value);
      switch (id.raw) {
        case libwebm::kMkvInfo:
          ParseInfo(payload, child_stop, out);
          break;
        case libwebm::kMkvCluster:
          ParseCluster(pos, payload, child_stop, out);
          break;
        case libwebm::kMkvCues:
          ParseCues(payload, child_stop, out);
          break;
        default:
          break;
      }
      pos = child_stop;
    }
  }

  void ParseInfo(size_t start, size_t stop, ParsedMatroska* out) {
    size_t pos = start;
    while (pos < stop) {
      const VInt id = ReadVInt(pos);
      const VInt size = ReadVInt(pos + id.length);
      const size_t payload = pos + id.length + size.length;
      if (id.raw == libwebm::kMkvTimecodeScale)
        out->timecode_scale = ReadUInt(payload, static_cast<size_t>(size.value));
      else if (id.raw == libwebm::kMkvDuration)
        out->info_duration = ReadFloat(payload, static_cast<size_t>(size.value));
      pos = payload + static_cast<size_t>(size.value);
    }
  }

  void ParseCluster(size_t element_start, size_t start, size_t stop,
                    ParsedMatroska* out) {
    ClusterRecord cluster;
    cluster.segment_relative_offset =
        static_cast<uint64_t>(static_cast<int64_t>(element_start) -
                              out->segment_payload_offset);
    size_t pos = start;
    while (pos < stop) {
      const VInt id = ReadVInt(pos);
      const VInt size = ReadVInt(pos + id.length);
      const size_t payload = pos + id.length + size.length;
      const size_t child_stop = payload + static_cast<size_t>(size.value);
      if (id.raw == libwebm::kMkvTimecode) {
        cluster.timecode = ReadUInt(payload, static_cast<size_t>(size.value));
      } else if (id.raw == libwebm::kMkvSimpleBlock) {
        cluster.blocks.push_back(
            ParseBlock(payload, child_stop, true));
      } else if (id.raw == libwebm::kMkvBlockGroup) {
        cluster.blocks.push_back(ParseBlockGroup(payload, child_stop));
      }
      pos = child_stop;
    }
    out->clusters.push_back(cluster);
  }

  BlockRecord ParseBlock(size_t payload, size_t stop, bool is_simple) {
    BlockRecord block;
    block.is_simple_block = is_simple;
    const VInt track = ReadVInt(payload);
    block.track = track.value;
    size_t pos = payload + track.length;
    block.relative_timecode = ReadSInt(pos, 2);
    pos += 2;
    ++pos;  // Block flags byte.
    block.payload.assign(data_.begin() + pos, data_.begin() + stop);
    return block;
  }

  BlockRecord ParseBlockGroup(size_t start, size_t stop) {
    BlockRecord block;
    block.is_simple_block = false;
    size_t pos = start;
    while (pos < stop) {
      const VInt id = ReadVInt(pos);
      const VInt size = ReadVInt(pos + id.length);
      const size_t payload = pos + id.length + size.length;
      const size_t child_stop = payload + static_cast<size_t>(size.value);
      if (id.raw == libwebm::kMkvBlock) {
        block = ParseBlock(payload, child_stop, false);
      } else if (id.raw == libwebm::kMkvReferenceBlock) {
        block.reference_blocks.push_back(
            ReadSInt(payload, static_cast<size_t>(size.value)));
      } else if (id.raw == libwebm::kMkvBlockDuration) {
        block.has_duration = true;
        block.duration = ReadUInt(payload, static_cast<size_t>(size.value));
      }
      pos = child_stop;
    }
    return block;
  }

  void ParseCues(size_t start, size_t stop, ParsedMatroska* out) {
    size_t pos = start;
    while (pos < stop) {
      const VInt id = ReadVInt(pos);
      const VInt size = ReadVInt(pos + id.length);
      const size_t payload = pos + id.length + size.length;
      const size_t child_stop = payload + static_cast<size_t>(size.value);
      if (id.raw == libwebm::kMkvCuePoint)
        ParseCuePoint(payload, child_stop, out);
      pos = child_stop;
    }
  }

  void ParseCuePoint(size_t start, size_t stop, ParsedMatroska* out) {
    CueRecord cue;
    size_t pos = start;
    while (pos < stop) {
      const VInt id = ReadVInt(pos);
      const VInt size = ReadVInt(pos + id.length);
      const size_t payload = pos + id.length + size.length;
      const size_t child_stop = payload + static_cast<size_t>(size.value);
      if (id.raw == libwebm::kMkvCueTime) {
        cue.time = ReadUInt(payload, static_cast<size_t>(size.value));
      } else if (id.raw == libwebm::kMkvCueTrackPositions) {
        ParseCueTrackPositions(payload, child_stop, &cue);
      }
      pos = child_stop;
    }
    out->cues.push_back(cue);
  }

  void ParseCueTrackPositions(size_t start, size_t stop, CueRecord* cue) {
    size_t pos = start;
    while (pos < stop) {
      const VInt id = ReadVInt(pos);
      const VInt size = ReadVInt(pos + id.length);
      const size_t payload = pos + id.length + size.length;
      switch (id.raw) {
        case libwebm::kMkvCueTrack:
          cue->track = ReadUInt(payload, static_cast<size_t>(size.value));
          break;
        case libwebm::kMkvCueClusterPosition:
          cue->cluster_position =
              ReadUInt(payload, static_cast<size_t>(size.value));
          break;
        case libwebm::kMkvCueBlockNumber:
          cue->has_block_number = true;
          cue->block_number = ReadUInt(payload, static_cast<size_t>(size.value));
          break;
        default:
          break;
      }
      pos = payload + static_cast<size_t>(size.value);
    }
  }

  const std::vector<uint8_t>& data_;
};

///////////////////////////////////////////////////////////////
// Frame construction helpers.
///////////////////////////////////////////////////////////////

void FillFrame(mkvmuxer::Frame* frame, uint8_t marker, uint64_t track,
                bool key, uint64_t pts, uint64_t dts, uint64_t duration) {
  CHECK(frame->Init(&marker, 1));
  frame->set_track_number(track);
  frame->set_is_key(key);
  frame->set_timestamp(pts);
  frame->set_decode_timestamp(dts);
  frame->set_duration(duration);
}

const BlockRecord* FindBlock(const ParsedMatroska& parsed, uint8_t marker,
                             const ClusterRecord** cluster_out) {
  for (const ClusterRecord& cluster : parsed.clusters) {
    for (const BlockRecord& block : cluster.blocks) {
      if (block.payload.size() == 1 && block.payload[0] == marker) {
        if (cluster_out)
          *cluster_out = &cluster;
        return &block;
      }
    }
  }
  return NULL;
}

///////////////////////////////////////////////////////////////
// Comprehensive decode order stream.
///////////////////////////////////////////////////////////////

void TestComprehensiveStream() {
  MemoryWriter writer;
  mkvmuxer::Segment segment;
  CHECK(segment.Init(&writer));
  segment.set_mode(mkvmuxer::Segment::kFile);
  segment.SetDecodeOrder(true);
  segment.GetSegmentInfo()->set_timecode_scale(kScale3Ms);
  // Disable the duration threshold so the boundary frames stay in the
  // intended clusters.
  segment.set_max_cluster_duration(0);

  const uint64_t video = segment.AddVideoTrack(16, 16, 1);
  CHECK(video == 1);
  const uint64_t audio = segment.AddAudioTrack(48000, 2, 2);
  CHECK(audio == 2);
  CHECK(segment.CuesTrack(video));

  const uint64_t vdur = 2 * kScale3Ms;
  const uint64_t adur = kScale3Ms;

  // Decode order delivery: I, P, B, where B has the middle PTS.
  mkvmuxer::Frame i;
  FillFrame(&i, 'I', video, true, 4 * kMs, 0, vdur);
  CHECK(segment.AddGenericFrame(&i));

  mkvmuxer::Frame p;
  FillFrame(&p, 'P', video, false, 10 * kMs, 3 * kMs, vdur);
  CHECK(p.AddReferenceBlockTimestamp(4 * kMs));  // tick 1 vs frame tick 3.
  CHECK(segment.AddGenericFrame(&p));

  // Dual references, and a PTS that is not an integer number of ticks.
  mkvmuxer::Frame b;
  FillFrame(&b, 'B', video, false, 7 * kMs, 6 * kMs, vdur);
  CHECK(b.AddReferenceBlockTimestamp(4 * kMs));   // tick 1
  CHECK(b.AddReferenceBlockTimestamp(10 * kMs));  // tick 3
  CHECK(segment.AddGenericFrame(&b));

  mkvmuxer::Frame a;
  FillFrame(&a, 'a', audio, true, 5 * kMs, 9 * kMs, adur);
  CHECK(segment.AddGenericFrame(&a));

  mkvmuxer::Frame p2;
  FillFrame(&p2, 'Q', video, false, 13 * kMs, 12 * kMs, vdur);
  CHECK(p2.AddReferenceBlockTimestamp(10 * kMs));
  CHECK(segment.AddGenericFrame(&p2));

  // Reference whose quantized difference is zero: the ReferenceBlock element
  // must still be present.
  mkvmuxer::Frame b2;
  FillFrame(&b2, 'b', video, false, 8 * kMs, 15 * kMs, vdur);
  CHECK(b2.AddReferenceBlockTimestamp(7 * kMs));  // both quantize to tick 2.
  CHECK(segment.AddGenericFrame(&b2));

  // Force a new cluster before the audio block. The following video keyframe
  // opens yet another cluster of its own, exercising both cluster triggers.
  segment.ForceNewClusterOnNextFrame();
  mkvmuxer::Frame a2;
  FillFrame(&a2, 'c', audio, true, 14 * kMs, 18 * kMs, adur);
  CHECK(segment.AddGenericFrame(&a2));

  mkvmuxer::Frame i2;
  FillFrame(&i2, 'J', video, true, 13500000, 21 * kMs, vdur);
  CHECK(segment.AddGenericFrame(&i2));

  // Boundary cluster: keyframe at tick 40000.
  const uint64_t boundary_pts = 40000ULL * kScale3Ms;
  mkvmuxer::Frame i3;
  FillFrame(&i3, 'K', video, true, boundary_pts, 24 * kMs, vdur);
  CHECK(segment.AddGenericFrame(&i3));

  // Relative block timecode -32768.
  const int64_t neg_limit_tick = 40000 - 32768;
  mkvmuxer::Frame x;
  FillFrame(&x, 'X', video, false, static_cast<uint64_t>(neg_limit_tick) * kScale3Ms, 27 * kMs, vdur);
  CHECK(x.AddReferenceBlockTimestamp(boundary_pts));
  CHECK(segment.AddGenericFrame(&x));

  // Relative block timecode +32767.
  const int64_t pos_limit_tick = 40000 + 32767;
  mkvmuxer::Frame y;
  FillFrame(&y, 'Y', video, false, static_cast<uint64_t>(pos_limit_tick) * kScale3Ms, 30 * kMs, vdur);
  CHECK(y.AddReferenceBlockTimestamp(boundary_pts));
  CHECK(segment.AddGenericFrame(&y));

  // Relative block timecode +32768: positive overflow opens a new cluster
  // instead of being rejected.
  const int64_t pos_over_tick = 40000 + 32768;
  mkvmuxer::Frame z;
  FillFrame(&z, 'Z', video, false, static_cast<uint64_t>(pos_over_tick) * kScale3Ms, 33 * kMs, vdur);
  CHECK(z.AddReferenceBlockTimestamp(boundary_pts));
  CHECK(segment.AddGenericFrame(&z));

  // Relative block timecode -32769: negative overflow is rejected.
  const int64_t neg_over_tick = 40000 - 32769;
  mkvmuxer::Frame r;
  FillFrame(&r, 'R', video, false, static_cast<uint64_t>(neg_over_tick) * kScale3Ms, 36 * kMs, vdur);
  CHECK(r.AddReferenceBlockTimestamp(boundary_pts));
  CHECK(!segment.AddGenericFrame(&r));

  // A legal frame after the rejected one must still be accepted. Its PTS is
  // within the signed range of the current cluster (opened by Z).
  mkvmuxer::Frame w;
  FillFrame(&w, 'W', video, false, 72760ULL * kScale3Ms, 39 * kMs, vdur);
  CHECK(w.AddReferenceBlockTimestamp(boundary_pts));
  CHECK(segment.AddGenericFrame(&w));

  CHECK(segment.Finalize());

  // Independent EBML verification.
  EbmlReader reader(writer.bytes);
  const ParsedMatroska parsed = reader.Parse();
  CHECK(parsed.timecode_scale == kScale3Ms);
  // Clusters: [I P B a Q b], [c], [J], [K X Y], [Z W].
  CHECK(parsed.clusters.size() == 5);

  // Payload order matches submission order, with the rejected frame absent.
  const char expected_payloads[] = {'I', 'P', 'B', 'a', 'Q', 'b',
                                    'c', 'J', 'K', 'X', 'Y', 'Z', 'W'};
  size_t payload_index = 0;
  for (const ClusterRecord& cluster : parsed.clusters) {
    for (const BlockRecord& block : cluster.blocks) {
      CHECK(payload_index < sizeof(expected_payloads));
      CHECK(block.payload.size() == 1 &&
            block.payload[0] ==
                static_cast<uint8_t>(expected_payloads[payload_index]));
      ++payload_index;
    }
  }
  CHECK(payload_index == sizeof(expected_payloads));

  // Every block is a BlockGroup carrying a duration and no SimpleBlock was
  // emitted.
  for (const ClusterRecord& cluster : parsed.clusters) {
    for (const BlockRecord& block : cluster.blocks) {
      CHECK(!block.is_simple_block);
      CHECK(block.has_duration);
      CHECK(block.duration > 0);
    }
  }

  const ClusterRecord* cluster = NULL;

  // P frame: one reference, floor(4ms/3ms) - floor(10ms/3ms) = 1 - 3 = -2.
  const BlockRecord* p_block = FindBlock(parsed, 'P', &cluster);
  CHECK(p_block && p_block->reference_blocks.size() == 1);
  CHECK(p_block->reference_blocks[0] == -2);

  // B frame: two references, values 1 - 2 = -1 and 3 - 2 = 1.
  const BlockRecord* b_block = FindBlock(parsed, 'B', &cluster);
  CHECK(b_block && b_block->reference_blocks.size() == 2);
  CHECK(b_block->reference_blocks[0] == -1);
  CHECK(b_block->reference_blocks[1] == 1);

  // Zero difference still writes a ReferenceBlock.
  const BlockRecord* b2_block = FindBlock(parsed, 'b', &cluster);
  CHECK(b2_block && b2_block->reference_blocks.size() == 1);
  CHECK(b2_block->reference_blocks[0] == 0);

  // Keyframes and audio carry no ReferenceBlock.
  for (const char marker : {'I', 'J', 'K', 'a', 'c'}) {
    const BlockRecord* key_block = FindBlock(parsed,
                                             static_cast<uint8_t>(marker), NULL);
    CHECK(key_block && key_block->reference_blocks.empty());
  }

  // Signed 16-bit block timecode boundaries.
  const BlockRecord* x_block = FindBlock(parsed, 'X', NULL);
  CHECK(x_block && x_block->relative_timecode == -32768);
  const BlockRecord* y_block = FindBlock(parsed, 'Y', NULL);
  CHECK(y_block && y_block->relative_timecode == 32767);
  const BlockRecord* z_block = FindBlock(parsed, 'Z', &cluster);
  CHECK(z_block && z_block->relative_timecode == 0);
  CHECK(cluster && cluster->timecode == 72768);

  // Large signed reference values are preserved verbatim.
  CHECK(x_block->reference_blocks.size() == 1 &&
        x_block->reference_blocks[0] == 32768);
  CHECK(z_block->reference_blocks.size() == 1 &&
        z_block->reference_blocks[0] == -32768);

  // Cues point only at the three video keyframes. Every keyframe starts its
  // own cluster, so the physical block number is always 1 and no
  // CueBlockNumber element is written.
  CHECK(parsed.cues.size() == 3);
  const uint64_t expected_cue_times[3] = {1, 4, 40000};
  const uint8_t cue_markers[3] = {'I', 'J', 'K'};
  for (int i = 0; i < 3; ++i) {
    const ClusterRecord* key_cluster = NULL;
    const BlockRecord* key_block =
        FindBlock(parsed, cue_markers[i], &key_cluster);
    CHECK(key_block && key_cluster);
    CHECK(parsed.cues[i].time == expected_cue_times[i]);
    CHECK(parsed.cues[i].track == video);
    CHECK(!parsed.cues[i].has_block_number);
    CHECK(parsed.cues[i].cluster_position ==
          key_cluster->segment_relative_offset);
  }

  // Segment duration is the largest PTS plus duration endpoint, in ticks.
  const uint64_t max_end_ns = 72768ULL * kScale3Ms + vdur;
  const double expected_duration =
      static_cast<double>(max_end_ns) / static_cast<double>(kScale3Ms);
  const double duration_error =
      parsed.info_duration > expected_duration
          ? parsed.info_duration - expected_duration
          : expected_duration - parsed.info_duration;
  CHECK(duration_error < 1.0);

  // mkvparser cross-check.
  MemoryReader mkv_reader(writer.bytes);
  long long parser_pos = 0;
  mkvparser::Segment* parser_segment = NULL;
  CHECK(mkvparser::Segment::CreateInstance(&mkv_reader, parser_pos,
                                           parser_segment) == 0);
  CHECK(parser_segment);
  CHECK(parser_segment->Load() >= 0);
  CHECK(parser_segment->GetCount() ==
        static_cast<unsigned long>(parsed.clusters.size()));

  const mkvparser::Cues* cues = parser_segment->GetCues();
  CHECK(cues);
  while (!cues->DoneParsing())
    CHECK(cues->LoadCuePoint() >= 0);
  CHECK(cues->GetCount() == 3);

  const mkvparser::Track* const parser_video =
      parser_segment->GetTracks()->GetTrackByNumber(
          static_cast<long>(video));
  CHECK(parser_video);

  const mkvparser::CuePoint* cue = cues->GetFirst();
  for (int cue_index = 0; cue; ++cue_index, cue = cues->GetNext(cue)) {
    const mkvparser::CuePoint::TrackPosition* const position =
        cue->Find(parser_video);
    CHECK(position);
    const mkvparser::BlockEntry* const entry =
        cues->GetBlock(cue, position);
    CHECK(entry);
    const mkvparser::Block* const block = entry->GetBlock();
    CHECK(block && block->IsKey());
    CHECK(position->m_pos ==
          static_cast<long long>(parsed.cues[cue_index].cluster_position));
    CHECK(position->m_block ==
          static_cast<long long>(
              parsed.cues[cue_index].has_block_number
                  ? parsed.cues[cue_index].block_number
                  : 1));
  }

  // The dual-reference B frame is exposed by the parser with its negative and
  // positive reference timecodes.
  const mkvparser::Cluster* parser_cluster = parser_segment->GetFirst();
  bool found_dual_reference = false;
  while (parser_cluster && !parser_cluster->EOS()) {
    const mkvparser::BlockEntry* entry = NULL;
    CHECK(parser_cluster->GetFirst(entry) == 0);
    while (entry && !entry->EOS()) {
      if (entry->GetKind() == mkvparser::BlockEntry::kBlockGroup) {
        const mkvparser::BlockGroup* const group =
            static_cast<const mkvparser::BlockGroup*>(entry);
        if (group->GetPrevTimeCode() == -1 &&
            group->GetNextTimeCode() == 1) {
          found_dual_reference = true;
        }
      }
      const mkvparser::BlockEntry* next = NULL;
      CHECK(parser_cluster->GetNext(entry, next) == 0);
      entry = next;
    }
    parser_cluster = parser_segment->GetNext(parser_cluster);
  }
  CHECK(found_dual_reference);

  delete parser_segment;
}

///////////////////////////////////////////////////////////////
// Rejection handling: invalid frames never advance any clock, and legal
// frames keep working afterwards.
///////////////////////////////////////////////////////////////

void TestRejections() {
  MemoryWriter writer;
  mkvmuxer::Segment segment;
  CHECK(segment.Init(&writer));
  segment.set_mode(mkvmuxer::Segment::kFile);
  segment.SetDecodeOrder(true);
  const uint64_t video = segment.AddVideoTrack(16, 16, 1);
  CHECK(video == 1);
  const uint64_t audio = segment.AddAudioTrack(48000, 2, 2);
  CHECK(audio == 2);

  // Missing DTS.
  mkvmuxer::Frame fresh;
  CHECK(fresh.Init(reinterpret_cast<const uint8_t*>("1"), 1));
  fresh.set_track_number(video);
  fresh.set_is_key(true);
  fresh.set_timestamp(0);
  fresh.set_duration(kMs);
  CHECK(!fresh.decode_timestamp_set());
  CHECK(!segment.AddGenericFrame(&fresh));

  // Zero duration.
  mkvmuxer::Frame zero_dur;
  FillFrame(&zero_dur, '2', video, true, 0, 0, 0);
  CHECK(!segment.AddGenericFrame(&zero_dur));

  // Unknown track number.
  mkvmuxer::Frame bad_track;
  FillFrame(&bad_track, '3', 99, true, 0, 0, kMs);
  CHECK(!segment.AddGenericFrame(&bad_track));

  // Keyframe carrying a reference.
  mkvmuxer::Frame key_with_ref;
  FillFrame(&key_with_ref, '4', video, true, 0, 0, kMs);
  CHECK(key_with_ref.AddReferenceBlockTimestamp(0));
  CHECK(!segment.AddGenericFrame(&key_with_ref));

  // Non-key video frame without references.
  mkvmuxer::Frame no_refs;
  FillFrame(&no_refs, '5', video, false, kMs, kMs, kMs);
  CHECK(!segment.AddGenericFrame(&no_refs));

  // Negative reference timestamps are invalid absolute PTS values.
  mkvmuxer::Frame negative_ref;
  FillFrame(&negative_ref, '8', video, false, kMs, kMs, kMs);
  CHECK(negative_ref.AddReferenceBlockTimestamp(0));
  CHECK(negative_ref.AddReferenceBlockTimestamp(-1));
  CHECK(negative_ref.reference_block_timestamps_size() == 2);
  CHECK(!segment.AddGenericFrame(&negative_ref));

  // Reference list limits and duplicate rejection.
  mkvmuxer::Frame many_refs;
  CHECK(many_refs.Init(reinterpret_cast<const uint8_t*>("6"), 1));
  CHECK(many_refs.AddReferenceBlockTimestamp(1));
  CHECK(many_refs.AddReferenceBlockTimestamp(2));
  CHECK(!many_refs.AddReferenceBlockTimestamp(3));
  CHECK(!many_refs.AddReferenceBlockTimestamp(2));
  CHECK(many_refs.reference_block_timestamps_size() == 2);

  // PTS plus duration overflow.
  mkvmuxer::Frame overflow;
  FillFrame(&overflow, '7', video, true, UINT64_MAX, 0, 1);
  CHECK(!segment.AddGenericFrame(&overflow));

  // Establish a valid timeline starting at a non-zero DTS.
  mkvmuxer::Frame first;
  FillFrame(&first, 'I', video, true, 0, 5 * kMs, kMs);
  CHECK(segment.AddGenericFrame(&first));

  // Per-track DTS must be strictly increasing; an equal DTS is rejected.
  mkvmuxer::Frame equal_dts;
  FillFrame(&equal_dts, 'P', video, false, 2 * kMs, 5 * kMs, kMs);
  CHECK(equal_dts.AddReferenceBlockTimestamp(0));
  CHECK(!segment.AddGenericFrame(&equal_dts));

  // Global DTS must not decrease, even on another track's first frame.
  mkvmuxer::Frame audio_back;
  FillFrame(&audio_back, 'a', audio, true, kMs, 2 * kMs, kMs);
  CHECK(!segment.AddGenericFrame(&audio_back));

  // After every rejection, legal frames continue.
  mkvmuxer::Frame second;
  FillFrame(&second, 'P', video, false, 2 * kMs, 6 * kMs, kMs);
  CHECK(second.AddReferenceBlockTimestamp(0));
  CHECK(segment.AddGenericFrame(&second));

  mkvmuxer::Frame audio_ok;
  FillFrame(&audio_ok, 'a', audio, true, kMs, 7 * kMs, kMs);
  CHECK(segment.AddGenericFrame(&audio_ok));

  CHECK(segment.Finalize());

  EbmlReader reader(writer.bytes);
  const ParsedMatroska parsed = reader.Parse();
  CHECK(parsed.clusters.size() == 1);
  CHECK(parsed.clusters[0].blocks.size() == 3);
  CHECK(parsed.clusters[0].blocks[0].payload[0] == 'I');
  CHECK(parsed.clusters[0].blocks[1].payload[0] == 'P');
  CHECK(parsed.clusters[0].blocks[2].payload[0] == 'a');
  // Only the accepted frames contribute to the segment duration.
  const double duration_error =
      parsed.info_duration > 3.0 ? parsed.info_duration - 3.0
                                 : 3.0 - parsed.info_duration;
  CHECK(duration_error < 0.01);
}

///////////////////////////////////////////////////////////////
// Configuration incompatibilities are rejected before the header is written.
///////////////////////////////////////////////////////////////

void TestConfigurationRejection() {
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    CHECK(segment.Init(&writer));
    segment.set_mode(mkvmuxer::Segment::kLive);
    segment.SetDecodeOrder(true);
    mkvmuxer::Frame frame;
  FillFrame(&frame, 'I', 1, true, 0, 0, kMs);
    CHECK(!segment.AddGenericFrame(&frame));
  }
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    CHECK(segment.Init(&writer));
    segment.set_mode(mkvmuxer::Segment::kFile);
    segment.SetDecodeOrder(true);
    segment.AccurateClusterDuration(true);
    mkvmuxer::Frame frame;
  FillFrame(&frame, 'I', 1, true, 0, 0, kMs);
    CHECK(!segment.AddGenericFrame(&frame));
  }
  {
    MemoryWriter writer;
    writer.set_seekable(false);
    mkvmuxer::Segment segment;
    CHECK(segment.Init(&writer));
    segment.set_mode(mkvmuxer::Segment::kFile);
    segment.SetDecodeOrder(true);
    mkvmuxer::Frame frame;
    FillFrame(&frame, 'I', 1, true, 0, 0, kMs);
    CHECK(!segment.AddGenericFrame(&frame));
  }
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    CHECK(segment.Init(&writer));
    segment.set_mode(mkvmuxer::Segment::kFile);
    segment.SetDecodeOrder(true);
    const std::string chunk_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/decode_order_chunk";
    CHECK(segment.SetChunking(true, chunk_path.c_str()));
    mkvmuxer::Frame frame;
    FillFrame(&frame, 'I', 1, true, 0, 0, kMs);
    CHECK(!segment.AddGenericFrame(&frame));
  }
}

///////////////////////////////////////////////////////////////
// Cluster thresholds: the duration threshold follows the DTS span even when
// the PTS goes backwards, and the size threshold still triggers new clusters.
///////////////////////////////////////////////////////////////

void TestClusterThresholds() {
  // Duration threshold measured on DTS, immune to backwards PTS.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    CHECK(segment.Init(&writer));
    segment.set_mode(mkvmuxer::Segment::kFile);
    segment.SetDecodeOrder(true);
    segment.set_max_cluster_duration(10 * kMs);
    const uint64_t video = segment.AddVideoTrack(16, 16, 1);
    CHECK(video == 1);

    mkvmuxer::Frame i;
    FillFrame(&i, 'I', video, true, 0, 0, kMs);
    CHECK(segment.AddGenericFrame(&i));

    mkvmuxer::Frame p;
    FillFrame(&p, 'P', video, false, 6 * kMs, 5 * kMs, kMs);
    CHECK(p.AddReferenceBlockTimestamp(0));
    CHECK(segment.AddGenericFrame(&p));

    // DTS span reaches the 10ms threshold even though the PTS moves backwards
    // to 1ms; this must open a new cluster.
    mkvmuxer::Frame b;
    FillFrame(&b, 'B', video, false, kMs, 10 * kMs, kMs);
    CHECK(b.AddReferenceBlockTimestamp(0));
    CHECK(b.AddReferenceBlockTimestamp(6 * kMs));
    CHECK(segment.AddGenericFrame(&b));

    // A later B frame with an even smaller PTS stays in the new cluster; the
    // DTS span since the new cluster's first frame is only 5ms.
    mkvmuxer::Frame b2;
    FillFrame(&b2, 'b', video, false, 0, 15 * kMs, kMs);
    CHECK(b2.AddReferenceBlockTimestamp(kMs));
    CHECK(segment.AddGenericFrame(&b2));

    CHECK(segment.Finalize());
    EbmlReader reader(writer.bytes);
    const ParsedMatroska parsed = reader.Parse();
    CHECK(parsed.clusters.size() == 2);
    CHECK(parsed.clusters[0].blocks.size() == 2);
    CHECK(parsed.clusters[1].timecode == 1);  // Quantized PTS of frame B.
    CHECK(parsed.clusters[1].blocks.size() == 2);
    // Frame b2 has a negative cluster-relative block timecode.
    CHECK(parsed.clusters[1].blocks[1].relative_timecode == -1);
  }

  // Size threshold opens clusters without any keyframe or time overflow.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    CHECK(segment.Init(&writer));
    segment.set_mode(mkvmuxer::Segment::kFile);
    segment.SetDecodeOrder(true);
    segment.set_max_cluster_duration(0);
    segment.set_max_cluster_size(50);
    const uint64_t audio = segment.AddAudioTrack(48000, 2, 2);
    CHECK(audio == 2);

    for (int i = 0; i < 6; ++i) {
      mkvmuxer::Frame audio_frame;
      FillFrame(&audio_frame, static_cast<uint8_t>('a' + i), audio, true,
                static_cast<uint64_t>(i) * kMs,
                static_cast<uint64_t>(i) * kMs, kMs);
      CHECK(segment.AddGenericFrame(&audio_frame));
    }
    CHECK(segment.Finalize());
    EbmlReader reader(writer.bytes);
    const ParsedMatroska parsed = reader.Parse();
    CHECK(parsed.clusters.size() >= 2);
    size_t total_blocks = 0;
    for (const ClusterRecord& cluster : parsed.clusters)
      total_blocks += cluster.blocks.size();
    CHECK(total_blocks == 6);  // Nothing was reordered or dropped.
  }
}

///////////////////////////////////////////////////////////////
// Frame API semantics and default mode compatibility.
///////////////////////////////////////////////////////////////

void TestFrameApiAndDefaultMode() {
  // The legacy single-reference interface stays in sync with the list.
  mkvmuxer::Frame frame;
  CHECK(frame.Init(reinterpret_cast<const uint8_t*>("x"), 1));
  frame.set_reference_block_timestamp(1234);
  CHECK(frame.reference_block_timestamp_set());
  CHECK(frame.reference_block_timestamp() == 1234);
  CHECK(frame.reference_block_timestamps_size() == 1);
  CHECK(frame.reference_block_timestamps(0) == 1234);

  // CopyFrom duplicates the complete reference list, including a second
  // entry added through the new API.
  CHECK(frame.AddReferenceBlockTimestamp(5678));
  CHECK(frame.reference_block_timestamps_size() == 2);
  mkvmuxer::Frame copy;
  CHECK(copy.CopyFrom(frame));
  CHECK(copy.reference_block_timestamps_size() == 2);
  CHECK(copy.reference_block_timestamps(0) == 1234);
  CHECK(copy.reference_block_timestamps(1) == 5678);
  CHECK(copy.decode_timestamp_set() == false);
  copy.set_decode_timestamp(42);
  mkvmuxer::Frame copy2;
  CHECK(copy2.CopyFrom(copy));
  CHECK(copy2.decode_timestamp_set() && copy2.decode_timestamp() == 42);

  copy.ClearReferenceBlockTimestamps();
  CHECK(copy.reference_block_timestamps_size() == 0);
  CHECK(!copy.reference_block_timestamp_set());

  // Default mode still rejects a backwards PTS.
  MemoryWriter writer;
  mkvmuxer::Segment segment;
  CHECK(segment.Init(&writer));
  segment.set_mode(mkvmuxer::Segment::kFile);
  const uint64_t track = segment.AddVideoTrack(16, 16, 1);
  CHECK(track == 1);
  uint8_t data[] = {0};
  mkvmuxer::Frame f;
  CHECK(f.Init(data, sizeof(data)));
  f.set_track_number(track);
  f.set_is_key(true);
  f.set_duration(40 * kMs);
  f.set_timestamp(80 * kMs);
  CHECK(segment.AddGenericFrame(&f));
  f.set_timestamp(160 * kMs);
  f.set_is_key(false);
  f.set_reference_block_timestamp(80 * kMs);
  CHECK(segment.AddGenericFrame(&f));
  f.set_timestamp(120 * kMs);
  CHECK(!segment.AddGenericFrame(&f));
  CHECK(segment.Finalize());

  // The legacy single-reference interface also drives decode order mode and
  // produces exactly one ReferenceBlock.
  MemoryWriter writer2;
  mkvmuxer::Segment segment2;
  CHECK(segment2.Init(&writer2));
  segment2.set_mode(mkvmuxer::Segment::kFile);
  segment2.SetDecodeOrder(true);
  const uint64_t track2 = segment2.AddVideoTrack(16, 16, 1);
  mkvmuxer::Frame key;
  FillFrame(&key, 'I', track2, true, 0, 0, kMs);
  CHECK(segment2.AddGenericFrame(&key));
  mkvmuxer::Frame inter;
  FillFrame(&inter, 'P', track2, false, kMs, kMs, kMs);
  inter.set_reference_block_timestamp(0);
  CHECK(inter.reference_block_timestamps_size() == 1);
  CHECK(segment2.AddGenericFrame(&inter));
  CHECK(segment2.Finalize());
  EbmlReader reader(writer2.bytes);
  const ParsedMatroska parsed = reader.Parse();
  CHECK(parsed.clusters.size() == 1);
  CHECK(parsed.clusters[0].blocks.size() == 2);
  CHECK(parsed.clusters[0].blocks[1].reference_blocks.size() == 1);
  CHECK(parsed.clusters[0].blocks[1].reference_blocks[0] == -1);
}

}  // namespace

int main() {
  TestComprehensiveStream();
  TestRejections();
  TestConfigurationRejection();
  TestClusterThresholds();
  TestFrameApiAndDefaultMode();
  std::printf("decode_order_test: all checks passed\n");
  return 0;
}
