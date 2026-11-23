// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.

// Standalone test for Segment decode-order mode. Generates short Matroska
// files through the public mkvmuxer interface, then verifies the output
// twice: with mkvparser, and with an independent EBML field reader that
// checks payload order, signed block timecodes, ReferenceBlock values,
// BlockDuration, and cue targets. No GoogleTest, no test vectors.

#include <stdint.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "common/webmids.h"
#include "mkvmuxer/mkvmuxer.h"
#include "mkvparser/mkvparser.h"

namespace {

// ---------------------------------------------------------------------------
// Minimal test harness.

int g_checks = 0;
int g_failures = 0;

void ExpectTrue(bool cond, const char* expr, int line) {
  ++g_checks;
  if (!cond) {
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
  }
}

void ExpectI64(int64_t actual, int64_t expected, const char* expr, int line) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s = %lld, expected %lld\n", line,
                 expr, static_cast<long long>(actual),
                 static_cast<long long>(expected));
  }
}

void ExpectU64(uint64_t actual, uint64_t expected, const char* expr,
               int line) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s = %llu, expected %llu\n", line,
                 expr, static_cast<unsigned long long>(actual),
                 static_cast<unsigned long long>(expected));
  }
}

#define EXPECT_TRUE(cond) ExpectTrue((cond), #cond, __LINE__)
#define EXPECT_I64(actual, expected) ExpectI64((actual), (expected), #actual, __LINE__)
#define EXPECT_U64(actual, expected) ExpectU64((actual), (expected), #actual, __LINE__)

// ---------------------------------------------------------------------------
// In-memory seekable writer.

class MemoryWriter : public mkvmuxer::IMkvWriter {
 public:
  std::vector<uint8_t> bytes;
  int64_t pos;

  MemoryWriter() : pos(0) {}

  mkvmuxer::int32 Write(const void* buf, mkvmuxer::uint32 len) override {
    if (pos + len > static_cast<int64_t>(bytes.size()))
      bytes.resize(static_cast<size_t>(pos + len));
    std::memcpy(bytes.data() + pos, buf, len);
    pos += len;
    return 0;
  }

  mkvmuxer::int64 Position() const override { return pos; }

  mkvmuxer::int32 Position(mkvmuxer::int64 position) override {
    if (position < 0)
      return -1;
    pos = position;
    return 0;
  }

  bool Seekable() const override { return true; }
  void ElementStartNotify(mkvmuxer::uint64, mkvmuxer::int64) override {}
};

// In-memory reader for mkvparser.
class VectorReader : public mkvparser::IMkvReader {
 public:
  explicit VectorReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  int Read(long long position, long length, unsigned char* buffer) override {
    if (position < 0 || length < 0 ||
        static_cast<uint64_t>(position) + static_cast<uint64_t>(length) >
            bytes_.size()) {
      return -1;
    }
    std::memcpy(buffer, bytes_.data() + position, static_cast<size_t>(length));
    return 0;
  }

  int Length(long long* total, long long* available) override {
    if (!total || !available)
      return -1;
    *total = *available = static_cast<long long>(bytes_.size());
    return 0;
  }

 private:
  const std::vector<uint8_t>& bytes_;
};

// ---------------------------------------------------------------------------
// Independent EBML field reader. Knows just enough of the container format
// to walk the elements this test produces.

struct EbmlElement {
  uint64_t id;
  uint64_t size;         // payload size; UINT64_MAX when unknown
  uint64_t payload_pos;  // absolute offset of the payload
};

class EbmlCursor {
 public:
  explicit EbmlCursor(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  bool ReadHeader(uint64_t pos, EbmlElement* out) const {
    uint32_t id_len = 0;
    uint32_t size_len = 0;
    if (!VintLength(pos, &id_len) || !VintLength(pos + id_len, &size_len))
      return false;
    if (pos + id_len + size_len > bytes_.size())
      return false;
    out->id = ReadId(pos, id_len);
    out->size = ReadSize(pos + id_len, size_len);
    out->payload_pos = pos + id_len + size_len;
    return true;
  }

  uint64_t ReadUIntPayload(uint64_t pos, uint64_t len) const {
    uint64_t value = 0;
    for (uint64_t i = 0; i < len; ++i)
      value = (value << 8) | bytes_[pos + i];
    return value;
  }

  int64_t ReadSIntPayload(uint64_t pos, uint64_t len) const {
    int64_t value = 0;
    for (uint64_t i = 0; i < len; ++i)
      value = (value << 8) | bytes_[pos + i];
    if (len > 0 && len < 8 && (bytes_[pos] & 0x80))
      value -= (int64_t{1} << (len * 8));
    return value;
  }

  double ReadFloatPayload(uint64_t pos, uint64_t len) const {
    if (len == 4) {
      const uint32_t bits =
          static_cast<uint32_t>(ReadUIntPayload(pos, len));
      float value = 0.0f;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    }
    if (len == 8) {
      const uint64_t bits = ReadUIntPayload(pos, len);
      double value = 0.0;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    }
    return 0.0;
  }

  uint64_t size() const { return bytes_.size(); }
  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  bool VintLength(uint64_t pos, uint32_t* len) const {
    if (pos >= bytes_.size())
      return false;
    const uint8_t first = bytes_[pos];
    for (uint32_t i = 1; i <= 8; ++i) {
      if (first & (0x80 >> (i - 1))) {
        *len = i;
        return true;
      }
    }
    return false;
  }

  uint64_t ReadId(uint64_t pos, uint32_t len) const {
    uint64_t value = 0;
    for (uint32_t i = 0; i < len; ++i)
      value = (value << 8) | bytes_[pos + i];
    return value;
  }

  uint64_t ReadSize(uint64_t pos, uint32_t len) const {
    uint64_t value = bytes_[pos] & (0xFF >> len);
    for (uint32_t i = 1; i < len; ++i)
      value = (value << 8) | bytes_[pos + i];
    const uint64_t unknown = (uint64_t{1} << (7 * len)) - 1;
    if (value == unknown)
      return UINT64_MAX;
    return value;
  }

  const std::vector<uint8_t>& bytes_;
};

struct BlockRec {
  bool simple = false;
  uint64_t track = 0;
  int16_t timecode = 0;  // signed, relative to the cluster
  std::vector<uint8_t> payload;
  std::vector<int64_t> references;  // ReferenceBlock values, in file order
  bool has_duration = false;
  uint64_t duration = 0;  // BlockDuration, in timecode units
};

struct ClusterRec {
  uint64_t timecode = 0;
  uint64_t element_pos = 0;  // absolute offset of the Cluster element
  std::vector<BlockRec> blocks;
};

struct CueRec {
  uint64_t time = 0;
  uint64_t track = 0;
  uint64_t cluster_pos = 0;  // relative to the segment payload
  uint64_t block_number = 0;
};

struct MkvModel {
  uint64_t timecode_scale = 1000000;
  bool has_duration = false;
  double duration = 0.0;  // in timecode units
  uint64_t segment_payload_pos = 0;
  std::vector<ClusterRec> clusters;
  std::vector<CueRec> cues;
};

// Parses a Block or SimpleBlock payload: track number, signed 16-bit
// timecode, flags, and the (unlaced) frame data.
bool ParseBlockPayload(const std::vector<uint8_t>& bytes, uint64_t pos,
                       uint64_t size, BlockRec* out) {
  if (pos + size > bytes.size() || size < 4)
    return false;
  const uint8_t first = bytes[pos];
  uint32_t track_len = 0;
  for (uint32_t i = 1; i <= 8; ++i) {
    if (first & (0x80 >> (i - 1))) {
      track_len = i;
      break;
    }
  }
  if (track_len == 0 || track_len + 3 > size)
    return false;
  uint64_t track = bytes[pos] & (0xFF >> track_len);
  for (uint32_t i = 1; i < track_len; ++i)
    track = (track << 8) | bytes[pos + i];
  out->track = track;
  const uint64_t tc_pos = pos + track_len;
  out->timecode = static_cast<int16_t>((bytes[tc_pos] << 8) | bytes[tc_pos + 1]);
  const uint64_t data_pos = tc_pos + 3;  // skip timecode and flags
  out->payload.assign(bytes.begin() + data_pos, bytes.begin() + pos + size);
  return true;
}

bool ParseCluster(const EbmlCursor& cursor, uint64_t element_pos,
                  uint64_t payload_pos, uint64_t end, ClusterRec* out) {
  out->element_pos = element_pos;
  uint64_t pos = payload_pos;
  while (pos < end) {
    EbmlElement child;
    if (!cursor.ReadHeader(pos, &child))
      return false;
    const uint64_t child_end = child.payload_pos + child.size;
    if (child.size == UINT64_MAX || child_end > end)
      return false;
    if (child.id == libwebm::kMkvTimecode) {
      out->timecode = cursor.ReadUIntPayload(child.payload_pos, child.size);
    } else if (child.id == libwebm::kMkvSimpleBlock) {
      BlockRec block;
      block.simple = true;
      if (!ParseBlockPayload(cursor.bytes(), child.payload_pos, child.size,
                             &block))
        return false;
      out->blocks.push_back(block);
    } else if (child.id == libwebm::kMkvBlockGroup) {
      BlockRec block;
      block.simple = false;
      bool saw_block = false;
      uint64_t group_pos = child.payload_pos;
      while (group_pos < child_end) {
        EbmlElement part;
        if (!cursor.ReadHeader(group_pos, &part))
          return false;
        if (part.size == UINT64_MAX ||
            part.payload_pos + part.size > child_end) {
          return false;
        }
        if (part.id == libwebm::kMkvBlock) {
          if (!ParseBlockPayload(cursor.bytes(), part.payload_pos, part.size,
                                 &block))
            return false;
          saw_block = true;
        } else if (part.id == libwebm::kMkvReferenceBlock) {
          block.references.push_back(
              cursor.ReadSIntPayload(part.payload_pos, part.size));
        } else if (part.id == libwebm::kMkvBlockDuration) {
          block.has_duration = true;
          block.duration = cursor.ReadUIntPayload(part.payload_pos, part.size);
        }
        group_pos = part.payload_pos + part.size;
      }
      if (!saw_block)
        return false;
      out->blocks.push_back(block);
    }
    pos = child_end;
  }
  return pos == end;
}

bool ParseMkv(const std::vector<uint8_t>& bytes, MkvModel* out) {
  const EbmlCursor cursor(bytes);
  uint64_t pos = 0;
  EbmlElement header;
  if (!cursor.ReadHeader(pos, &header) || header.id != libwebm::kMkvEBML)
    return false;
  pos = header.payload_pos + header.size;

  EbmlElement segment;
  if (!cursor.ReadHeader(pos, &segment) || segment.id != libwebm::kMkvSegment)
    return false;
  out->segment_payload_pos = segment.payload_pos;
  const uint64_t segment_end =
      (segment.size == UINT64_MAX) ? bytes.size()
                                   : segment.payload_pos + segment.size;

  pos = segment.payload_pos;
  while (pos < segment_end) {
    EbmlElement child;
    if (!cursor.ReadHeader(pos, &child))
      return false;
    const uint64_t child_end = (child.size == UINT64_MAX)
                                   ? segment_end
                                   : child.payload_pos + child.size;
    if (child_end > segment_end)
      return false;

    if (child.id == libwebm::kMkvInfo) {
      uint64_t info_pos = child.payload_pos;
      while (info_pos < child_end) {
        EbmlElement info_child;
        if (!cursor.ReadHeader(info_pos, &info_child))
          return false;
        if (info_child.id == libwebm::kMkvTimecodeScale) {
          out->timecode_scale = cursor.ReadUIntPayload(
              info_child.payload_pos, info_child.size);
        } else if (info_child.id == libwebm::kMkvDuration) {
          out->has_duration = true;
          out->duration =
              cursor.ReadFloatPayload(info_child.payload_pos, info_child.size);
        }
        info_pos = info_child.payload_pos + info_child.size;
      }
    } else if (child.id == libwebm::kMkvCluster) {
      ClusterRec cluster;
      if (!ParseCluster(cursor, pos, child.payload_pos, child_end, &cluster))
        return false;
      out->clusters.push_back(cluster);
    } else if (child.id == libwebm::kMkvCues) {
      uint64_t cues_pos = child.payload_pos;
      while (cues_pos < child_end) {
        EbmlElement cue_point;
        if (!cursor.ReadHeader(cues_pos, &cue_point) ||
            cue_point.id != libwebm::kMkvCuePoint) {
          return false;
        }
        CueRec cue;
        cue.block_number = 1;  // default when CueBlockNumber is absent
        const uint64_t cue_end = cue_point.payload_pos + cue_point.size;
        uint64_t entry_pos = cue_point.payload_pos;
        while (entry_pos < cue_end) {
          EbmlElement entry;
          if (!cursor.ReadHeader(entry_pos, &entry))
            return false;
          if (entry.id == libwebm::kMkvCueTime) {
            cue.time = cursor.ReadUIntPayload(entry.payload_pos, entry.size);
          } else if (entry.id == libwebm::kMkvCueTrackPositions) {
            const uint64_t tp_end = entry.payload_pos + entry.size;
            uint64_t tp_pos = entry.payload_pos;
            while (tp_pos < tp_end) {
              EbmlElement tp;
              if (!cursor.ReadHeader(tp_pos, &tp))
                return false;
              if (tp.id == libwebm::kMkvCueTrack) {
                cue.track = cursor.ReadUIntPayload(tp.payload_pos, tp.size);
              } else if (tp.id == libwebm::kMkvCueClusterPosition) {
                cue.cluster_pos =
                    cursor.ReadUIntPayload(tp.payload_pos, tp.size);
              } else if (tp.id == libwebm::kMkvCueBlockNumber) {
                cue.block_number =
                    cursor.ReadUIntPayload(tp.payload_pos, tp.size);
              }
              tp_pos = tp.payload_pos + tp.size;
            }
          }
          entry_pos = entry.payload_pos + entry.size;
        }
        out->cues.push_back(cue);
        cues_pos = cue_end;
      }
    }
    pos = child_end;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Frame submission helpers.

const uint64_t kScaleMs = 1000000;     // 1 ms per tick (muxer default)
const uint64_t kScale10Ms = 10000000;  // 10 ms per tick
const uint64_t kVideoTrack = 1;
const uint64_t kAudioTrack = 2;
const uint64_t kVideoDurationNs = 40000000;
const uint64_t kAudioDurationNs = 20000000;

struct TestFrame {
  uint64_t track = kVideoTrack;
  uint64_t pts = 0;
  uint64_t dts = 0;
  uint64_t duration = kVideoDurationNs;
  bool is_key = false;
  std::vector<uint64_t> refs;
  uint8_t tag = 0;
  uint32_t payload_size = 4;
};

// Builds a mkvmuxer::Frame from |tf| and submits it to |segment|. Returns
// what AddGenericFrame returns. The payload is a deterministic pattern
// tagged with |tf.tag| so the written order can be verified later.
bool Submit(mkvmuxer::Segment* segment, const TestFrame& tf) {
  std::vector<uint8_t> payload(tf.payload_size);
  for (uint32_t i = 0; i < tf.payload_size; ++i)
    payload[i] = static_cast<uint8_t>(tf.tag + i * 37);
  mkvmuxer::Frame frame;
  if (!frame.Init(payload.data(), payload.size()))
    return false;
  frame.set_track_number(tf.track);
  frame.set_timestamp(tf.pts);
  frame.set_decode_timestamp(tf.dts);
  frame.set_duration(tf.duration);
  frame.set_is_key(tf.is_key);
  if (!tf.refs.empty() &&
      !frame.set_reference_timestamps(tf.refs.data(),
                                      static_cast<uint32_t>(tf.refs.size()))) {
    return false;
  }
  return segment->AddGenericFrame(&frame);
}

// The bytes Submit() writes for a given tag, for payload verification.
std::vector<uint8_t> ExpectedPayload(uint8_t tag, uint32_t payload_size) {
  std::vector<uint8_t> payload(payload_size);
  for (uint32_t i = 0; i < payload_size; ++i)
    payload[i] = static_cast<uint8_t>(tag + i * 37);
  return payload;
}

// ---------------------------------------------------------------------------
// Expectation model used to verify a produced file.

struct ExpectedBlock {
  uint8_t tag;
  uint64_t track;
  int64_t rel_timecode;  // signed, relative to its cluster
  uint64_t duration_ticks;
  std::vector<int64_t> references;
  uint32_t payload_size;
};

struct ExpectedCluster {
  uint64_t timecode;
  std::vector<ExpectedBlock> blocks;
};

struct ExpectedCue {
  uint64_t time;
  uint64_t track;
  int cluster;  // index into ExpectedFile::clusters the cue must point at
  uint64_t block_number;
};

struct ExpectedFile {
  uint64_t timecode_scale;
  double duration_ticks;
  std::vector<ExpectedCluster> clusters;
  std::vector<ExpectedCue> cues;
};

void VerifyBlockPayload(const BlockRec& block, uint8_t tag,
                        uint32_t payload_size, int line) {
  const std::vector<uint8_t> expected = ExpectedPayload(tag, payload_size);
  ++g_checks;
  if (block.payload != expected) {
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: payload mismatch for tag %d\n", line,
                 tag);
  }
}

// Verifies the file against the expectation using only the independent
// EBML reader: cluster timecodes, block order (via payload tags), signed
// block timecodes, ReferenceBlock lists, BlockDuration, segment duration,
// and cue fields (including that cue cluster positions match the actual
// cluster offsets).
void VerifyWithEbml(const std::vector<uint8_t>& bytes,
                    const ExpectedFile& expected, int line) {
  MkvModel model;
  ExpectTrue(ParseMkv(bytes, &model), "ParseMkv", line);
  EXPECT_U64(model.timecode_scale, expected.timecode_scale);
  EXPECT_TRUE(model.has_duration);
  EXPECT_I64(static_cast<int64_t>(model.duration * 1000 + 0.5),
             static_cast<int64_t>(expected.duration_ticks * 1000 + 0.5));

  EXPECT_U64(model.clusters.size(), expected.clusters.size());
  const size_t cluster_count =
      model.clusters.size() < expected.clusters.size()
          ? model.clusters.size()
          : expected.clusters.size();
  for (size_t c = 0; c < cluster_count; ++c) {
    const ClusterRec& cluster = model.clusters[c];
    const ExpectedCluster& want = expected.clusters[c];
    EXPECT_U64(cluster.timecode, want.timecode);
    EXPECT_U64(cluster.blocks.size(), want.blocks.size());
    const size_t block_count = cluster.blocks.size() < want.blocks.size()
                                   ? cluster.blocks.size()
                                   : want.blocks.size();
    for (size_t b = 0; b < block_count; ++b) {
      const BlockRec& block = cluster.blocks[b];
      const ExpectedBlock& want_block = want.blocks[b];
      // Every frame in these tests carries a duration, so every frame must
      // be written as a BlockGroup.
      EXPECT_TRUE(!block.simple);
      EXPECT_U64(block.track, want_block.track);
      EXPECT_I64(block.timecode, want_block.rel_timecode);
      EXPECT_TRUE(block.has_duration);
      EXPECT_U64(block.duration, want_block.duration_ticks);
      EXPECT_U64(block.references.size(), want_block.references.size());
      const size_t ref_count = block.references.size() < want_block.references.size()
                                   ? block.references.size()
                                   : want_block.references.size();
      for (size_t r = 0; r < ref_count; ++r)
        EXPECT_I64(block.references[r], want_block.references[r]);
      VerifyBlockPayload(block, want_block.tag, want_block.payload_size, line);
    }
  }

  EXPECT_U64(model.cues.size(), expected.cues.size());
  const size_t cue_count =
      model.cues.size() < expected.cues.size() ? model.cues.size()
                                               : expected.cues.size();
  for (size_t i = 0; i < cue_count; ++i) {
    const CueRec& cue = model.cues[i];
    const ExpectedCue& want = expected.cues[i];
    EXPECT_U64(cue.time, want.time);
    EXPECT_U64(cue.track, want.track);
    EXPECT_U64(cue.block_number, want.block_number);
    // The cue must name the actual position of its cluster.
    if (static_cast<size_t>(want.cluster) < model.clusters.size()) {
      const uint64_t cluster_pos =
          model.clusters[want.cluster].element_pos - model.segment_payload_pos;
      EXPECT_U64(cue.cluster_pos, cluster_pos);
    }
  }
}

// Verifies the file against the expectation using mkvparser: cluster and
// block iteration order, absolute block timecodes, payload bytes,
// BlockGroup durations, segment duration, and that each cue resolves to
// the expected keyframe block.
void VerifyWithParser(const std::vector<uint8_t>& bytes,
                      const ExpectedFile& expected, int line) {
  VectorReader reader(bytes);
  mkvparser::EBMLHeader header;
  long long pos = 0;
  header.Parse(&reader, pos);
  mkvparser::Segment* segment = NULL;
  ExpectTrue(mkvparser::Segment::CreateInstance(&reader, pos, segment) == 0 &&
                 segment != NULL,
             "Segment::CreateInstance", line);
  if (!segment)
    return;
  ExpectTrue(segment->Load() == 0, "Segment::Load", line);

  const mkvparser::SegmentInfo* info = segment->GetInfo();
  EXPECT_TRUE(info != NULL);
  if (info) {
    EXPECT_U64(static_cast<uint64_t>(info->GetTimeCodeScale()),
               expected.timecode_scale);
    EXPECT_I64(segment->GetDuration(),
               static_cast<int64_t>(expected.duration_ticks) *
                   static_cast<int64_t>(expected.timecode_scale));
  }

  EXPECT_U64(segment->GetCount(), expected.clusters.size());

  size_t cluster_index = 0;
  for (const mkvparser::Cluster* cluster = segment->GetFirst();
       cluster != NULL && !cluster->EOS() &&
       cluster_index < expected.clusters.size();
       cluster = segment->GetNext(cluster), ++cluster_index) {
    const ExpectedCluster& want = expected.clusters[cluster_index];
    EXPECT_I64(cluster->GetTimeCode(), static_cast<int64_t>(want.timecode));

    size_t block_index = 0;
    const mkvparser::BlockEntry* entry = NULL;
    long status = cluster->GetFirst(entry);
    while (status == 0 && entry != NULL && !entry->EOS() &&
           block_index < want.blocks.size()) {
      const ExpectedBlock& want_block = want.blocks[block_index];
      const mkvparser::Block* block = entry->GetBlock();
      EXPECT_TRUE(block != NULL);
      EXPECT_TRUE(entry->GetKind() == mkvparser::BlockEntry::kBlockGroup);
      if (block) {
        EXPECT_U64(static_cast<uint64_t>(block->GetTrackNumber()),
                   want_block.track);
        // Absolute, unscaled timecode = cluster timecode + relative.
        EXPECT_I64(block->GetTimeCode(cluster),
                   static_cast<int64_t>(want.timecode) +
                       want_block.rel_timecode);
        EXPECT_I64(block->GetFrameCount(), 1);
        if (block->GetFrameCount() == 1) {
          const mkvparser::Block::Frame& frame = block->GetFrame(0);
          std::vector<uint8_t> payload(static_cast<size_t>(frame.len));
          EXPECT_TRUE(frame.len > 0 &&
                      frame.Read(&reader, payload.data()) == 0);
          const std::vector<uint8_t> want_payload =
              ExpectedPayload(want_block.tag, want_block.payload_size);
          ++g_checks;
          if (payload != want_payload) {
            ++g_failures;
            std::fprintf(stderr,
                         "FAIL line %d: parser payload mismatch for tag %d\n",
                         line, want_block.tag);
          }
        }
      }
      if (entry->GetKind() == mkvparser::BlockEntry::kBlockGroup) {
        const mkvparser::BlockGroup* group =
            static_cast<const mkvparser::BlockGroup*>(entry);
        EXPECT_I64(group->GetDurationTimeCode(),
                   static_cast<int64_t>(want_block.duration_ticks));
      }
      ++block_index;
      status = cluster->GetNext(entry, entry);
    }
    EXPECT_U64(block_index, want.blocks.size());
  }
  EXPECT_U64(cluster_index, expected.clusters.size());

  // Cues: every cue must point at a real keyframe, with time, cluster
  // position, and block number matching the output.
  const mkvparser::Cues* cues = segment->GetCues();
  EXPECT_TRUE(cues != NULL);
  if (cues) {
    while (cues->LoadCuePoint()) {
    }
    EXPECT_I64(cues->GetCount(),
               static_cast<int64_t>(expected.cues.size()));
    const mkvparser::Track* video_track =
        segment->GetTracks()->GetTrackByNumber(kVideoTrack);
    EXPECT_TRUE(video_track != NULL);
    size_t cue_index = 0;
    for (const mkvparser::CuePoint* cue = cues->GetFirst();
         cue != NULL && cue_index < expected.cues.size();
         cue = cues->GetNext(cue), ++cue_index) {
      const ExpectedCue& want = expected.cues[cue_index];
      EXPECT_I64(cue->GetTimeCode(), static_cast<int64_t>(want.time));
      const mkvparser::CuePoint::TrackPosition* tp =
          video_track ? cue->Find(video_track) : NULL;
      EXPECT_TRUE(tp != NULL);
      if (tp) {
        EXPECT_U64(static_cast<uint64_t>(tp->m_track), want.track);
        EXPECT_U64(static_cast<uint64_t>(tp->m_block), want.block_number);
        // The cue's cluster position must match the cluster's actual
        // position within the segment.
        const mkvparser::Cluster* cluster = segment->GetFirst();
        size_t c = 0;
        for (; cluster != NULL && !cluster->EOS();
             cluster = segment->GetNext(cluster), ++c) {
          if (c == static_cast<size_t>(want.cluster)) {
            EXPECT_I64(cluster->GetPosition(), tp->m_pos);
            break;
          }
        }
        // The cue must resolve to the expected keyframe block.
        const mkvparser::BlockEntry* target = cues->GetBlock(cue, tp);
        EXPECT_TRUE(target != NULL);
        if (target) {
          const mkvparser::Block* block = target->GetBlock();
          EXPECT_TRUE(block != NULL);
          if (block) {
            EXPECT_U64(static_cast<uint64_t>(block->GetTrackNumber()),
                       want.track);
            EXPECT_I64(block->GetTimeCode(target->GetCluster()),
                       static_cast<int64_t>(want.time));
          }
        }
      }
    }
    EXPECT_U64(cue_index, expected.cues.size());
  }

  delete segment;
}

// Builds a segment with one video track and an optional audio track, in
// kFile mode with decode-order mode enabled.
bool CreateSegment(MemoryWriter* writer, mkvmuxer::Segment* segment,
                   uint64_t timecode_scale, bool with_audio) {
  if (!segment->Init(writer))
    return false;
  segment->set_mode(mkvmuxer::Segment::kFile);
  if (segment->GetSegmentInfo()->timecode_scale() != timecode_scale)
    segment->GetSegmentInfo()->set_timecode_scale(timecode_scale);
  if (segment->AddVideoTrack(16, 16, static_cast<int>(kVideoTrack)) !=
      kVideoTrack)
    return false;
  mkvmuxer::Track* video = segment->GetTrackByNumber(kVideoTrack);
  if (!video)
    return false;
  video->set_codec_id("V_MPEG4/ISO/AVC");
  const uint8_t codec_private[] = {1, 66, 0, 10, 255, 225, 0, 1, 103, 1, 0, 1, 104};
  if (!video->SetCodecPrivate(codec_private, sizeof(codec_private)))
    return false;
  if (with_audio) {
    if (segment->AddAudioTrack(48000, 2, static_cast<int>(kAudioTrack)) !=
        kAudioTrack)
      return false;
    mkvmuxer::Track* audio = segment->GetTrackByNumber(kAudioTrack);
    if (!audio)
      return false;
    audio->set_codec_id(mkvmuxer::Tracks::kOpusCodecId);
  }
  return segment->SetDecodeOrderMode(true);
}

TestFrame VideoFrame(uint8_t tag, uint64_t pts, uint64_t dts, bool is_key) {
  TestFrame tf;
  tf.tag = tag;
  tf.track = kVideoTrack;
  tf.pts = pts;
  tf.dts = dts;
  tf.is_key = is_key;
  tf.duration = kVideoDurationNs;
  return tf;
}

TestFrame AudioFrame(uint8_t tag, uint64_t pts, uint64_t dts) {
  TestFrame tf;
  tf.tag = tag;
  tf.track = kAudioTrack;
  tf.pts = pts;
  tf.dts = dts;
  tf.is_key = true;
  tf.duration = kAudioDurationNs;
  return tf;
}

// ---------------------------------------------------------------------------
// Scenario A: I/P/B in decode order, interleaved audio, a forced cluster
// split, and rejected frames interleaved with valid ones.
void TestDecodeOrderBasics() {
  MemoryWriter writer;
  mkvmuxer::Segment segment;
  EXPECT_TRUE(CreateSegment(&writer, &segment, kScaleMs, true));

  ExpectedFile expected;
  expected.timecode_scale = kScaleMs;
  expected.duration_ticks = 360.0;  // max(PTS + duration) = 360 ms

  ExpectedCluster c1;
  c1.timecode = 0;
  ExpectedCluster c2;
  c2.timecode = 160;
  ExpectedCluster c3;
  c3.timecode = 250;
  ExpectedCluster c4;
  c4.timecode = 280;

  const uint32_t kPayload = 4;

  // Display order of the first GOP would be I0 B1 B2 P3; submitted in
  // decode order I0 P3 B1 B2, with audio interleaved.
  TestFrame f = VideoFrame(1, 0, 0, true);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({1, kVideoTrack, 0, 40, {}, kPayload});

  f = AudioFrame(2, 10000000, 10000000);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({2, kAudioTrack, 10, 20, {}, kPayload});

  f = VideoFrame(3, 120000000, 40000000, false);
  f.refs.push_back(0);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({3, kVideoTrack, 120, 40, {-120}, kPayload});

  // This audio frame has a smaller PTS than the previously written video
  // frame; decode-order mode must not hold it back or reorder it.
  f = AudioFrame(4, 50000000, 50000000);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({4, kAudioTrack, 50, 20, {}, kPayload});

  // B frame with two references.
  f = VideoFrame(5, 40000000, 80000000, false);
  f.refs.push_back(0);
  f.refs.push_back(120000000);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({5, kVideoTrack, 40, 40, {-40, 80}, kPayload});

  f = AudioFrame(6, 90000000, 90000000);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({6, kAudioTrack, 90, 20, {}, kPayload});

  f = VideoFrame(7, 80000000, 120000000, false);
  f.refs.push_back(0);
  f.refs.push_back(120000000);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({7, kVideoTrack, 80, 40, {-80, 40}, kPayload});

  // Next GOP keyframe opens a new cluster.
  f = VideoFrame(8, 160000000, 160000000, true);
  EXPECT_TRUE(Submit(&segment, f));
  c2.blocks.push_back({8, kVideoTrack, 0, 40, {}, kPayload});

  // B frame whose PTS is smaller than its cluster's start: negative block
  // timecode.
  f = VideoFrame(9, 140000000, 200000000, false);
  f.refs.push_back(160000000);
  EXPECT_TRUE(Submit(&segment, f));
  c2.blocks.push_back({9, kVideoTrack, -20, 40, {20}, kPayload});

  f = VideoFrame(10, 200000000, 240000000, false);
  f.refs.push_back(160000000);
  EXPECT_TRUE(Submit(&segment, f));
  c2.blocks.push_back({10, kVideoTrack, 40, 40, {-40}, kPayload});

  // Forced cluster split on an audio frame; the cluster's timecode is the
  // audio frame's quantized PTS.
  segment.ForceNewClusterOnNextFrame();
  f = AudioFrame(11, 250000000, 250000000);
  EXPECT_TRUE(Submit(&segment, f));
  c3.blocks.push_back({11, kAudioTrack, 0, 20, {}, kPayload});

  f = VideoFrame(12, 280000000, 280000000, true);
  EXPECT_TRUE(Submit(&segment, f));
  c4.blocks.push_back({12, kVideoTrack, 0, 40, {}, kPayload});

  // Rejections: none of these may be written or advance any clock.
  f = VideoFrame(13, 300000000, 100000000, false);  // DTS goes backwards
  f.refs.push_back(280000000);
  EXPECT_TRUE(!Submit(&segment, f));

  f = VideoFrame(14, 300000000, 290000000, false);  // non-key, no references
  EXPECT_TRUE(!Submit(&segment, f));

  f = VideoFrame(15, 300000000, 290000000, true);  // keyframe with references
  f.refs.push_back(280000000);
  EXPECT_TRUE(!Submit(&segment, f));

  f = AudioFrame(16, 290000000, 290000000);  // audio with references
  f.refs.push_back(280000000);
  EXPECT_TRUE(!Submit(&segment, f));

  f = VideoFrame(17, 290000000, 290000000, true);  // unknown track number
  f.track = 99;
  EXPECT_TRUE(!Submit(&segment, f));

  f = VideoFrame(18, UINT64_MAX - 1000, 290000000, false);  // PTS+dur overflow
  f.refs.push_back(280000000);
  EXPECT_TRUE(!Submit(&segment, f));

  f = VideoFrame(19, 300000000, 280000000, false);  // same-track DTS not
  f.refs.push_back(280000000);                      // strictly increasing
  EXPECT_TRUE(!Submit(&segment, f));

  // Rejected for missing references; its large DTS must not be registered.
  f = VideoFrame(20, 300000000, 500000000, false);
  EXPECT_TRUE(!Submit(&segment, f));

  // Accepted only if frame 20's DTS was discarded.
  f = VideoFrame(21, 320000000, 300000000, false);
  f.refs.push_back(280000000);
  EXPECT_TRUE(Submit(&segment, f));
  c4.blocks.push_back({21, kVideoTrack, 40, 40, {-40}, kPayload});

  // Equal DTS across different tracks is allowed.
  f = AudioFrame(22, 300000000, 300000000);
  EXPECT_TRUE(Submit(&segment, f));
  c4.blocks.push_back({22, kAudioTrack, 20, 20, {}, kPayload});

  EXPECT_TRUE(segment.Finalize());

  expected.clusters.push_back(c1);
  expected.clusters.push_back(c2);
  expected.clusters.push_back(c3);
  expected.clusters.push_back(c4);
  expected.cues.push_back({0, kVideoTrack, 0, 1});
  expected.cues.push_back({160, kVideoTrack, 1, 1});
  expected.cues.push_back({280, kVideoTrack, 3, 1});

  VerifyWithEbml(writer.bytes, expected, __LINE__);
  VerifyWithParser(writer.bytes, expected, __LINE__);
}

// ---------------------------------------------------------------------------
// Scenario B: full signed 16-bit block timecode range, non-default timecode
// scale, and endpoint-first reference quantization.
void TestBlockTimecodeBounds() {
  MemoryWriter writer;
  mkvmuxer::Segment segment;
  EXPECT_TRUE(CreateSegment(&writer, &segment, kScale10Ms, false));

  ExpectedFile expected;
  expected.timecode_scale = kScale10Ms;
  // Frame 5 has the largest end point: 400000000000 + 40000000 ns.
  expected.duration_ticks = 40004.0;

  const uint32_t kPayload = 4;

  ExpectedCluster c1;
  c1.timecode = 0;
  ExpectedCluster c2;
  c2.timecode = 32768;
  ExpectedCluster c3;
  c3.timecode = 40000;

  TestFrame f = VideoFrame(1, 0, 0, true);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({1, kVideoTrack, 0, 4, {}, kPayload});

  // Largest representable positive block timecode.
  f = VideoFrame(2, 327670000000ULL, 40000000, false);
  f.refs.push_back(0);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({2, kVideoTrack, 32767, 4, {-32767}, kPayload});

  // One tick further: positive overflow opens a new cluster.
  f = VideoFrame(3, 327680000000ULL, 80000000, false);
  f.refs.push_back(327670000000ULL);
  EXPECT_TRUE(Submit(&segment, f));
  c2.blocks.push_back({3, kVideoTrack, 0, 4, {-1}, kPayload});

  // Smallest representable (most negative) block timecode.
  f = VideoFrame(4, 0, 120000000, false);
  f.refs.push_back(327680000000ULL);
  EXPECT_TRUE(Submit(&segment, f));
  c2.blocks.push_back({4, kVideoTrack, -32768, 4, {32768}, kPayload});

  f = VideoFrame(5, 400000000000ULL, 160000000, true);
  EXPECT_TRUE(Submit(&segment, f));
  c3.blocks.push_back({5, kVideoTrack, 0, 4, {}, kPayload});

  // Exactly -32768 relative to the new cluster.
  f = VideoFrame(6, 72320000000ULL, 200000000, false);
  f.refs.push_back(400000000000ULL);
  EXPECT_TRUE(Submit(&segment, f));
  c3.blocks.push_back({6, kVideoTrack, -32768, 4, {32768}, kPayload});

  // One tick further down: negative underflow is rejected outright.
  f = VideoFrame(7, 72310000000ULL, 240000000, false);
  f.refs.push_back(400000000000ULL);
  EXPECT_TRUE(!Submit(&segment, f));

  // Non-integral-tick PTS (7232.5 ticks) with a reference that separates
  // endpoint-first quantization (7231 - 7232 = -1) from quantizing the
  // difference (floor(-1.5) = -2).
  f = VideoFrame(8, 72325000000ULL, 280000000, false);
  f.refs.push_back(72310000000ULL);
  EXPECT_TRUE(Submit(&segment, f));
  c3.blocks.push_back({8, kVideoTrack, -32768, 4, {-1}, kPayload});

  // A reference landing in the same tick as the frame produces a zero
  // ReferenceBlock delta, which must still be written.
  f = VideoFrame(9, 72326000000ULL, 320000000, false);
  f.refs.push_back(72320000000ULL);
  EXPECT_TRUE(Submit(&segment, f));
  c3.blocks.push_back({9, kVideoTrack, -32768, 4, {0}, kPayload});

  // Block timecode of exactly -1: a legal value, not an error sentinel.
  f = VideoFrame(10, 399990000000ULL, 360000000, false);
  f.refs.push_back(400000000000ULL);
  EXPECT_TRUE(Submit(&segment, f));
  c3.blocks.push_back({10, kVideoTrack, -1, 4, {1}, kPayload});

  EXPECT_TRUE(segment.Finalize());

  expected.clusters.push_back(c1);
  expected.clusters.push_back(c2);
  expected.clusters.push_back(c3);
  expected.cues.push_back({0, kVideoTrack, 0, 1});
  expected.cues.push_back({40000, kVideoTrack, 2, 1});

  VerifyWithEbml(writer.bytes, expected, __LINE__);
  VerifyWithParser(writer.bytes, expected, __LINE__);
}

// ---------------------------------------------------------------------------
// Scenario C: the cluster duration threshold is measured on the DTS span
// since the cluster's first frame, not on PTS.
void TestClusterDurationThreshold() {
  MemoryWriter writer;
  mkvmuxer::Segment segment;
  EXPECT_TRUE(CreateSegment(&writer, &segment, kScaleMs, false));
  segment.set_max_cluster_duration(100000000);  // 100 ms

  ExpectedFile expected;
  expected.timecode_scale = kScaleMs;
  expected.duration_ticks = 240.0;

  const uint32_t kPayload = 4;

  ExpectedCluster c1;
  c1.timecode = 100;
  ExpectedCluster c2;
  c2.timecode = 60;
  ExpectedCluster c3;
  c3.timecode = 200;

  TestFrame f = VideoFrame(1, 100000000, 0, true);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({1, kVideoTrack, 0, 40, {}, kPayload});

  f = VideoFrame(2, 140000000, 40000000, false);
  f.refs.push_back(100000000);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({2, kVideoTrack, 40, 40, {-40}, kPayload});

  // DTS span since the cluster's first frame is 120 ms >= 100 ms, so this
  // frame opens a new cluster even though its PTS is smaller than the
  // previous cluster's start. The new cluster starts at this frame's
  // quantized PTS.
  f = VideoFrame(3, 60000000, 120000000, false);
  f.refs.push_back(100000000);
  f.refs.push_back(140000000);
  EXPECT_TRUE(Submit(&segment, f));
  c2.blocks.push_back({3, kVideoTrack, 0, 40, {40, 80}, kPayload});

  f = VideoFrame(4, 150000000, 160000000, false);
  f.refs.push_back(140000000);
  EXPECT_TRUE(Submit(&segment, f));
  c2.blocks.push_back({4, kVideoTrack, 90, 40, {-10}, kPayload});

  f = VideoFrame(5, 200000000, 200000000, true);
  EXPECT_TRUE(Submit(&segment, f));
  c3.blocks.push_back({5, kVideoTrack, 0, 40, {}, kPayload});

  EXPECT_TRUE(segment.Finalize());

  expected.clusters.push_back(c1);
  expected.clusters.push_back(c2);
  expected.clusters.push_back(c3);
  expected.cues.push_back({100, kVideoTrack, 0, 1});
  expected.cues.push_back({200, kVideoTrack, 2, 1});

  VerifyWithEbml(writer.bytes, expected, __LINE__);
  VerifyWithParser(writer.bytes, expected, __LINE__);
}

// ---------------------------------------------------------------------------
// Scenario D: the cluster size threshold follows the payload written so
// far. With a tiny threshold every frame lands in its own cluster.
void TestClusterSizeThreshold() {
  MemoryWriter writer;
  mkvmuxer::Segment segment;
  EXPECT_TRUE(CreateSegment(&writer, &segment, kScaleMs, false));
  segment.set_max_cluster_duration(0);  // disable the duration threshold
  segment.set_max_cluster_size(10);

  ExpectedFile expected;
  expected.timecode_scale = kScaleMs;
  expected.duration_ticks = 160.0;

  const uint32_t kPayload = 32;

  TestFrame f = VideoFrame(1, 0, 0, true);
  f.payload_size = kPayload;
  EXPECT_TRUE(Submit(&segment, f));

  ExpectedCluster c1;
  c1.timecode = 0;
  c1.blocks.push_back({1, kVideoTrack, 0, 40, {}, kPayload});
  expected.clusters.push_back(c1);

  for (uint8_t tag = 2; tag <= 4; ++tag) {
    TestFrame p = VideoFrame(tag, (tag - 1) * 40000000ULL,
                             (tag - 1) * 40000000ULL, false);
    p.refs.push_back((tag - 2) * 40000000ULL);
    p.payload_size = kPayload;
    EXPECT_TRUE(Submit(&segment, p));
    ExpectedCluster cluster;
    cluster.timecode = (tag - 1) * 40;
    cluster.blocks.push_back(
        {tag, kVideoTrack, 0, 40, {-40}, kPayload});
    expected.clusters.push_back(cluster);
  }

  EXPECT_TRUE(segment.Finalize());

  expected.cues.push_back({0, kVideoTrack, 0, 1});

  VerifyWithEbml(writer.bytes, expected, __LINE__);
  VerifyWithParser(writer.bytes, expected, __LINE__);
}

// ---------------------------------------------------------------------------
// Scenario F: cues on the audio track. Cue block numbers must be the
// physical block index within the cluster, not the input frame count.
void TestCuesOnAudioTrack() {
  MemoryWriter writer;
  mkvmuxer::Segment segment;
  EXPECT_TRUE(CreateSegment(&writer, &segment, kScaleMs, true));
  EXPECT_TRUE(segment.CuesTrack(kAudioTrack));

  ExpectedFile expected;
  expected.timecode_scale = kScaleMs;
  expected.duration_ticks = 170.0;  // max(PTS + duration) = 170 ms

  const uint32_t kPayload = 4;

  ExpectedCluster c1;
  c1.timecode = 0;
  ExpectedCluster c2;
  c2.timecode = 120;

  TestFrame f = VideoFrame(1, 0, 0, true);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({1, kVideoTrack, 0, 40, {}, kPayload});

  // First audio frame of the cluster: the cue must name physical block 2.
  f = AudioFrame(2, 30000000, 40000000);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({2, kAudioTrack, 30, 20, {}, kPayload});

  f = VideoFrame(3, 40000000, 80000000, false);
  f.refs.push_back(0);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({3, kVideoTrack, 40, 40, {-40}, kPayload});

  f = AudioFrame(4, 70000000, 100000000);
  EXPECT_TRUE(Submit(&segment, f));
  c1.blocks.push_back({4, kAudioTrack, 70, 20, {}, kPayload});

  f = VideoFrame(5, 120000000, 120000000, true);
  EXPECT_TRUE(Submit(&segment, f));
  c2.blocks.push_back({5, kVideoTrack, 0, 40, {}, kPayload});

  // First audio frame of the second cluster: again physical block 2.
  f = AudioFrame(6, 150000000, 160000000);
  EXPECT_TRUE(Submit(&segment, f));
  c2.blocks.push_back({6, kAudioTrack, 30, 20, {}, kPayload});

  EXPECT_TRUE(segment.Finalize());

  expected.clusters.push_back(c1);
  expected.clusters.push_back(c2);
  expected.cues.push_back({30, kAudioTrack, 0, 2});
  expected.cues.push_back({150, kAudioTrack, 1, 2});

  VerifyWithEbml(writer.bytes, expected, __LINE__);

  // The parser-side cue resolution below uses the audio track.
  VectorReader reader(writer.bytes);
  mkvparser::EBMLHeader header;
  long long pos = 0;
  header.Parse(&reader, pos);
  mkvparser::Segment* parsed = NULL;
  EXPECT_TRUE(mkvparser::Segment::CreateInstance(&reader, pos, parsed) == 0 &&
              parsed != NULL);
  if (parsed) {
    EXPECT_TRUE(parsed->Load() == 0);
    const mkvparser::Cues* cues = parsed->GetCues();
    EXPECT_TRUE(cues != NULL);
    const mkvparser::Track* audio_track =
        parsed->GetTracks()->GetTrackByNumber(kAudioTrack);
    EXPECT_TRUE(audio_track != NULL);
    if (cues && audio_track) {
      while (cues->LoadCuePoint()) {
      }
      EXPECT_I64(cues->GetCount(), 2);
      const mkvparser::CuePoint* cue = cues->GetFirst();
      EXPECT_TRUE(cue != NULL);
      if (cue) {
        const mkvparser::CuePoint::TrackPosition* tp = cue->Find(audio_track);
        EXPECT_TRUE(tp != NULL);
        if (tp) {
          EXPECT_I64(tp->m_block, 2);
          const mkvparser::BlockEntry* target = cues->GetBlock(cue, tp);
          EXPECT_TRUE(target != NULL);
          if (target) {
            EXPECT_I64(target->GetBlock()->GetTimeCode(target->GetCluster()),
                       30);
            EXPECT_U64(
                static_cast<uint64_t>(target->GetBlock()->GetTrackNumber()),
                kAudioTrack);
          }
        }
      }
    }
    delete parsed;
  }
}

// ---------------------------------------------------------------------------
// Scenario E: Frame-level API and mode-switch validation.
void TestFrameAndModeApi() {
  // set_reference_timestamps validation.
  mkvmuxer::Frame frame;
  const uint64_t one_ref[] = {1000};
  const uint64_t two_refs[] = {1000, 2000};
  const uint64_t dup_refs[] = {1000, 1000};
  const uint64_t three_refs[] = {1000, 2000, 3000};
  EXPECT_TRUE(frame.set_reference_timestamps(one_ref, 1));
  EXPECT_U64(frame.reference_timestamps_size(), 1);
  EXPECT_U64(frame.reference_timestamp(0), 1000);
  // Failed sets leave the list unchanged.
  EXPECT_TRUE(!frame.set_reference_timestamps(three_refs, 3));
  EXPECT_TRUE(!frame.set_reference_timestamps(dup_refs, 2));
  EXPECT_TRUE(!frame.set_reference_timestamps(NULL, 1));
  EXPECT_U64(frame.reference_timestamps_size(), 1);
  EXPECT_U64(frame.reference_timestamp(0), 1000);
  EXPECT_TRUE(frame.set_reference_timestamps(two_refs, 2));
  EXPECT_U64(frame.reference_timestamps_size(), 2);
  EXPECT_U64(frame.reference_timestamp(1), 2000);
  EXPECT_TRUE(frame.set_reference_timestamps(NULL, 0));
  EXPECT_U64(frame.reference_timestamps_size(), 0);

  // CopyFrom copies the decode timestamp and the full reference list.
  const uint8_t data[] = {1, 2, 3, 4};
  EXPECT_TRUE(frame.Init(data, sizeof(data)));
  frame.set_track_number(1);
  frame.set_timestamp(5000);
  frame.set_decode_timestamp(4000);
  frame.set_duration(1000);
  frame.set_reference_timestamps(two_refs, 2);
  mkvmuxer::Frame copy;
  EXPECT_TRUE(copy.CopyFrom(frame));
  EXPECT_U64(copy.decode_timestamp(), 4000);
  EXPECT_TRUE(copy.decode_timestamp_set());
  EXPECT_U64(copy.reference_timestamps_size(), 2);
  EXPECT_U64(copy.reference_timestamp(0), 1000);
  EXPECT_U64(copy.reference_timestamp(1), 2000);

  // Decode-order mode cannot be enabled after incompatible options.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    EXPECT_TRUE(segment.Init(&writer));
    segment.set_mode(mkvmuxer::Segment::kLive);
    EXPECT_TRUE(!segment.SetDecodeOrderMode(true));
  }
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    EXPECT_TRUE(segment.Init(&writer));
    segment.AccurateClusterDuration(true);
    EXPECT_TRUE(!segment.SetDecodeOrderMode(true));
  }
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    EXPECT_TRUE(segment.Init(&writer));
    EXPECT_TRUE(segment.SetChunking(true, "decode_order_chunk_test"));
    EXPECT_TRUE(!segment.SetDecodeOrderMode(true));
    std::remove("decode_order_chunk_test_000000.chk");
    std::remove("decode_order_chunk_test.hdr");
  }
  {
    // Chunking cannot be enabled after decode-order mode.
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    EXPECT_TRUE(segment.Init(&writer));
    EXPECT_TRUE(segment.SetDecodeOrderMode(true));
    EXPECT_TRUE(!segment.SetChunking(true, "decode_order_chunk_test"));
  }
  {
    // Incompatible options enabled after the mode are rejected before the
    // header is written; restoring a compatible configuration recovers.
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    EXPECT_TRUE(segment.Init(&writer));
    segment.set_mode(mkvmuxer::Segment::kFile);
    EXPECT_TRUE(segment.AddVideoTrack(16, 16, 1) == 1);
    EXPECT_TRUE(segment.SetDecodeOrderMode(true));
    segment.set_mode(mkvmuxer::Segment::kLive);
    TestFrame f = VideoFrame(1, 0, 0, true);
    EXPECT_TRUE(!Submit(&segment, f));
    segment.set_mode(mkvmuxer::Segment::kFile);
    EXPECT_TRUE(Submit(&segment, f));
    EXPECT_TRUE(segment.Finalize());
  }
  {
    // The mode cannot be changed once the header has been written.
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    EXPECT_TRUE(CreateSegment(&writer, &segment, kScaleMs, false));
    TestFrame f = VideoFrame(1, 0, 0, true);
    EXPECT_TRUE(Submit(&segment, f));
    EXPECT_TRUE(!segment.SetDecodeOrderMode(false));
    EXPECT_TRUE(segment.Finalize());
  }
  {
    // Frames without an explicit decode timestamp or a positive duration
    // are rejected in decode-order mode.
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    EXPECT_TRUE(CreateSegment(&writer, &segment, kScaleMs, false));
    const uint8_t data[] = {1, 2, 3};

    mkvmuxer::Frame no_dts;
    EXPECT_TRUE(no_dts.Init(data, sizeof(data)));
    no_dts.set_track_number(kVideoTrack);
    no_dts.set_timestamp(0);
    no_dts.set_is_key(true);
    no_dts.set_duration(1000);
    EXPECT_TRUE(!segment.AddGenericFrame(&no_dts));

    mkvmuxer::Frame no_duration;
    EXPECT_TRUE(no_duration.Init(data, sizeof(data)));
    no_duration.set_track_number(kVideoTrack);
    no_duration.set_timestamp(0);
    no_duration.set_is_key(true);
    no_duration.set_decode_timestamp(0);
    EXPECT_TRUE(!segment.AddGenericFrame(&no_duration));

    mkvmuxer::Frame zero_duration;
    EXPECT_TRUE(zero_duration.CopyFrom(no_duration));
    zero_duration.set_duration(0);
    EXPECT_TRUE(!segment.AddGenericFrame(&zero_duration));

    // None of the rejections consumed the first-frame position.
    TestFrame f = VideoFrame(1, 0, 0, true);
    EXPECT_TRUE(Submit(&segment, f));
    EXPECT_TRUE(segment.Finalize());
  }
}

}  // namespace

int main() {
  TestDecodeOrderBasics();
  TestBlockTimecodeBounds();
  TestClusterDurationThreshold();
  TestClusterSizeThreshold();
  TestCuesOnAudioTrack();
  TestFrameAndModeApi();

  std::printf("decode_order_test: %d checks, %d failures\n", g_checks,
              g_failures);
  if (g_failures != 0) {
    std::printf("decode_order_test: FAILED\n");
    return 1;
  }
  std::printf("decode_order_test: PASSED\n");
  return 0;
}
