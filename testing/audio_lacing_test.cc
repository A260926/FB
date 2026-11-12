// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.

#include <stdint.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "common/webmids.h"
#include "mkvmuxer/mkvmuxer.h"
#include "mkvparser/mkvparser.h"

namespace {

const uint64_t kMs = 1000000ULL;

uint64_t Millis(uint64_t value) { return value * kMs; }

std::vector<uint8_t> MakePayload(int tag, size_t length) {
  std::vector<uint8_t> data(length);
  for (size_t i = 0; i < length; ++i) {
    data[i] =
        static_cast<uint8_t>((tag * 131 + static_cast<int>(i) * 7) & 0xff);
  }
  return data;
}

class MemoryWriter : public mkvmuxer::IMkvWriter {
 public:
  std::vector<uint8_t> bytes;

  mkvmuxer::int32 Write(const void* data, mkvmuxer::uint32 size) override {
    if (fail_ && size >= fail_min_len_)
      return -1;
    const size_t pos = static_cast<size_t>(pos_);
    if (pos + size > bytes.size())
      bytes.resize(pos + size);
    std::memcpy(bytes.data() + pos, data, size);
    pos_ += size;
    return 0;
  }

  mkvmuxer::int64 Position() const override { return pos_; }

  mkvmuxer::int32 Position(mkvmuxer::int64 position) override {
    if (position < 0)
      return -1;
    pos_ = position;
    return 0;
  }

  bool Seekable() const override { return true; }

  void ElementStartNotify(mkvmuxer::uint64, mkvmuxer::int64) override {}

  void StartFailing(mkvmuxer::uint32 min_len) {
    fail_ = true;
    fail_min_len_ = min_len;
  }

 private:
  mkvmuxer::int64 pos_ = 0;
  bool fail_ = false;
  mkvmuxer::uint32 fail_min_len_ = 0;
};

class MemoryReader : public mkvparser::IMkvReader {
 public:
  explicit MemoryReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  int Read(long long pos, long len, unsigned char* out) override {
    if (pos < 0 || len < 0 ||
        static_cast<unsigned long long>(pos) > bytes_.size() ||
        static_cast<unsigned long long>(len) >
            bytes_.size() - static_cast<size_t>(pos)) {
      return -1;
    }
    std::memcpy(out, bytes_.data() + pos, static_cast<size_t>(len));
    return 0;
  }

  int Length(long long* total, long long* available) override {
    *total = static_cast<long long>(bytes_.size());
    *available = static_cast<long long>(bytes_.size());
    return 0;
  }

 private:
  const std::vector<uint8_t>& bytes_;
};

// Reads an EBML-coded unsigned integer, independently of mkvparser.
bool ReadEbmlUInt(const uint8_t* data, size_t size, size_t* consumed,
                  uint64_t* value) {
  if (size == 0)
    return false;

  int length = 1;
  uint8_t mask = 0x80;
  while (length <= 8 && (data[0] & mask) == 0) {
    mask >>= 1;
    ++length;
  }
  if (length > 8 || static_cast<size_t>(length) > size)
    return false;

  uint64_t result = data[0] & (mask - 1);
  for (int i = 1; i < length; ++i)
    result = (result << 8) | data[i];

  *consumed = static_cast<size_t>(length);
  *value = result;
  return true;
}

struct RawBlockHeader {
  RawBlockHeader()
      : track(0), timecode(0), flags(0), lacing(0), frame_count(0) {}

  uint64_t track;
  int16_t timecode;
  uint8_t flags;
  int lacing;  // 0 = none, 1 = Xiph.
  int frame_count;
  std::vector<size_t> frame_lengths;
  std::vector<std::vector<uint8_t> > raw_frames;
};

// Independently decodes a Block payload from the container bytes. This
// validates the block header and Xiph length table without going through
// mkvparser.
bool ParseRawBlock(const std::vector<uint8_t>& file, long long start,
                   long long size, RawBlockHeader* header) {
  if (!header || start < 0 || size <= 0 ||
      static_cast<size_t>(start + size) > file.size()) {
    return false;
  }

  const uint8_t* data = file.data() + start;
  const size_t total = static_cast<size_t>(size);
  size_t pos = 0;
  if (!ReadEbmlUInt(data, total, &pos, &header->track) ||
      header->track == 0) {
    return false;
  }
  if (total - pos < 3)
    return false;

  header->timecode = static_cast<int16_t>((data[pos] << 8) | data[pos + 1]);
  pos += 2;
  header->flags = data[pos++];
  header->lacing = (header->flags & 0x06) >> 1;

  if (header->lacing == 0) {
    header->frame_count = 1;
    const size_t length = total - pos;
    if (length == 0)
      return false;
    header->frame_lengths.push_back(length);
    std::vector<uint8_t> frame(data + pos, data + pos + length);
    header->raw_frames.push_back(frame);
    return true;
  }

  if (header->lacing != 1)  // Tests only emit none or Xiph lacing.
    return false;

  if (pos >= total)
    return false;
  header->frame_count = static_cast<int>(data[pos++]) + 1;

  size_t table_sum = 0;
  for (int i = 0; i + 1 < header->frame_count; ++i) {
    size_t length = 0;
    for (;;) {
      if (pos >= total)
        return false;
      const uint8_t value = data[pos++];
      length += value;
      if (value < 255)
        break;
    }
    if (length == 0)
      return false;
    header->frame_lengths.push_back(length);
    table_sum += length;
  }

  if (pos > total || table_sum > total - pos)
    return false;
  const size_t last_length = total - pos - table_sum;
  if (last_length == 0)
    return false;
  header->frame_lengths.push_back(last_length);

  for (int i = 0; i < header->frame_count; ++i) {
    const size_t length = header->frame_lengths[static_cast<size_t>(i)];
    if (pos + length > total)
      return false;
    std::vector<uint8_t> frame(data + pos, data + pos + length);
    header->raw_frames.push_back(frame);
    pos += length;
  }

  return pos == total;
}

struct BlockInfo {
  BlockInfo()
      : cluster_index(0),
        block_index(0),
        cluster_time_ns(0),
        block_time_ns(0),
        track(0),
        key(false),
        block_group(false),
        discard_padding(0),
        lacing(mkvparser::Block::kLacingNone),
        frame_count(0),
        raw_start(0),
        raw_size(0) {}

  long cluster_index;
  long block_index;  // 1-based within the cluster.
  int64_t cluster_time_ns;
  int64_t block_time_ns;
  uint64_t track;
  bool key;
  bool block_group;
  int64_t discard_padding;
  mkvparser::Block::Lacing lacing;
  int frame_count;
  std::vector<std::vector<uint8_t> > frames;
  long long raw_start;
  long long raw_size;
  RawBlockHeader raw;
};

struct CueInfo {
  CueInfo()
      : time_ns(0),
        track(0),
        cluster_pos(0),
        block_number(0),
        referenced_frame_count(0),
        referenced_key(false),
        referenced_lacing(mkvparser::Block::kLacingNone) {}

  int64_t time_ns;
  uint64_t track;
  long long cluster_pos;
  long long block_number;
  int referenced_frame_count;
  bool referenced_key;
  mkvparser::Block::Lacing referenced_lacing;
};

class ParsedFile {
 public:
  ParsedFile()
      : segment_(NULL),
        timecode_scale_ns_(0),
        duration_ns_(0),
        cluster_count_(0) {}
  ~ParsedFile() { delete segment_; }

  bool Parse(const std::vector<uint8_t>& bytes) {
    bytes_ = bytes;
    reader_.reset(new MemoryReader(bytes_));

    mkvparser::EBMLHeader header;
    long long pos = 0;
    if (header.Parse(reader_.get(), pos) != 0)
      return false;

    mkvparser::Segment* raw = NULL;
    if (mkvparser::Segment::CreateInstance(reader_.get(), pos, raw) != 0 ||
        !raw) {
      return false;
    }
    segment_ = raw;
    if (segment_->Load() != 0)
      return false;

    timecode_scale_ns_ =
        segment_->GetInfo() ? segment_->GetInfo()->GetTimeCodeScale() : 0;
    duration_ns_ = static_cast<int64_t>(segment_->GetDuration());

    const mkvparser::Tracks* const tracks = segment_->GetTracks();
    if (tracks) {
      for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i) {
        const mkvparser::Track* const track = tracks->GetTrackByIndex(i);
        if (track)
          default_durations_[static_cast<uint64_t>(track->GetNumber())] =
              track->GetDefaultDuration();
      }
    }

    for (const mkvparser::Cluster* cluster = segment_->GetFirst();
         cluster && !cluster->EOS(); cluster = segment_->GetNext(cluster)) {
      ++cluster_count_;
      cluster_positions_.push_back(cluster->GetPosition());
      long block_index = 1;
      const mkvparser::BlockEntry* entry = NULL;
      if (cluster->GetFirst(entry) != 0)
        return false;

      for (; entry && !entry->EOS();) {
        const mkvparser::Block* const block = entry->GetBlock();
        if (!block)
          return false;

        BlockInfo info;
        info.cluster_index = cluster_count_ - 1;
        info.block_index = block_index++;
        info.cluster_time_ns = cluster->GetTime();
        info.block_time_ns = block->GetTime(cluster);
        info.track = static_cast<uint64_t>(block->GetTrackNumber());
        info.key = block->IsKey();
        info.block_group =
            entry->GetKind() == mkvparser::BlockEntry::kBlockGroup;
        info.discard_padding = block->GetDiscardPadding();
        info.lacing = block->GetLacing();
        info.frame_count = block->GetFrameCount();
        info.raw_start = block->m_start;
        info.raw_size = block->m_size;
        if (!ParseRawBlock(bytes_, info.raw_start, info.raw_size, &info.raw))
          return false;

        for (int i = 0; i < info.frame_count; ++i) {
          const mkvparser::Block::Frame& frame = block->GetFrame(i);
          std::vector<uint8_t> frame_bytes(static_cast<size_t>(frame.len));
          if (frame.Read(reader_.get(), frame_bytes.data()) != 0)
            return false;
          info.frames.push_back(frame_bytes);
        }

        blocks_.push_back(info);

        const mkvparser::BlockEntry* next = NULL;
        if (cluster->GetNext(entry, next) != 0)
          return false;
        entry = next;
      }
    }

    return true;
  }

  bool ParseCues(uint64_t cues_track, std::vector<CueInfo>* cues) {
    if (!cues || !segment_)
      return false;

    const mkvparser::SeekHead* const seek_head = segment_->GetSeekHead();
    if (!seek_head)
      return true;

    long long cues_offset = 0;
    for (int i = 0; i < seek_head->GetCount(); ++i) {
      const mkvparser::SeekHead::Entry* const entry = seek_head->GetEntry(i);
      if (entry && entry->id == libwebm::kMkvCues)
        cues_offset = entry->pos;
    }
    if (cues_offset <= 0)
      return true;

    long long cues_pos = 0;
    long cues_len = 0;
    if (segment_->ParseCues(cues_offset, cues_pos, cues_len) != 0)
      return false;

    const mkvparser::Cues* const parsed_cues = segment_->GetCues();
    const mkvparser::Track* const track =
        segment_->GetTracks()
            ? segment_->GetTracks()->GetTrackByNumber(
                  static_cast<long>(cues_track))
            : NULL;
    if (!parsed_cues || !track)
      return false;

    while (parsed_cues->LoadCuePoint()) {
    }

    for (const mkvparser::CuePoint* cue_point = parsed_cues->GetFirst();
         cue_point; cue_point = parsed_cues->GetNext(cue_point)) {
      const mkvparser::CuePoint::TrackPosition* const track_position =
          cue_point->Find(track);
      if (!track_position)
        continue;

      CueInfo info;
      info.time_ns = cue_point->GetTime(segment_);
      info.track = cues_track;
      info.cluster_pos = track_position->m_pos;
      info.block_number = track_position->m_block;

      const mkvparser::BlockEntry* const blocked =
          parsed_cues->GetBlock(cue_point, track_position);
      if (blocked && blocked->GetBlock()) {
        info.referenced_frame_count = blocked->GetBlock()->GetFrameCount();
        info.referenced_key = blocked->GetBlock()->IsKey();
        info.referenced_lacing = blocked->GetBlock()->GetLacing();
      }

      cues->push_back(info);
    }

    return true;
  }

  const std::vector<BlockInfo>& blocks() const { return blocks_; }
  const std::vector<long long>& cluster_positions() const {
    return cluster_positions_;
  }
  int cluster_count() const { return cluster_count_; }
  int64_t timecode_scale_ns() const { return timecode_scale_ns_; }
  int64_t duration_ns() const { return duration_ns_; }
  uint64_t default_duration(uint64_t track_number) const {
    const std::map<uint64_t, uint64_t>::const_iterator found =
        default_durations_.find(track_number);
    return found == default_durations_.end() ? 0 : found->second;
  }

  std::vector<BlockInfo> blocks_in_cluster(long cluster_index) const {
    std::vector<BlockInfo> result;
    for (size_t i = 0; i < blocks_.size(); ++i) {
      if (blocks_[i].cluster_index == cluster_index)
        result.push_back(blocks_[i]);
    }
    return result;
  }

 private:
  std::unique_ptr<MemoryReader> reader_;
  std::vector<uint8_t> bytes_;
  mkvparser::Segment* segment_;
  std::vector<BlockInfo> blocks_;
  std::vector<long long> cluster_positions_;
  std::map<uint64_t, uint64_t> default_durations_;
  int64_t timecode_scale_ns_;
  int64_t duration_ns_;
  int cluster_count_;
};

uint64_t AddAudioTrack(mkvmuxer::Segment* segment, int number,
                       uint64_t default_duration_ns) {
  const uint64_t track = segment->AddAudioTrack(48000, 2, number);
  if (track == 0)
    return 0;
  mkvmuxer::Track* const added = segment->GetTrackByNumber(track);
  added->set_uid(track);
  added->set_default_duration(default_duration_ns);
  return track;
}

uint64_t AddVideoTrack(mkvmuxer::Segment* segment, int number) {
  const uint64_t track = segment->AddVideoTrack(16, 16, number);
  if (track == 0)
    return 0;
  segment->GetTrackByNumber(track)->set_uid(track);
  return track;
}

typedef std::pair<uint64_t, uint64_t> FrameKey;  // (track, timestamp in ns)
typedef std::map<FrameKey, std::vector<uint8_t> > ExpectedFrames;

void ExpectFrame(const ExpectedFrames& expected, uint64_t track,
                 uint64_t timestamp_ns, const std::vector<uint8_t>& bytes,
                 ExpectedFrames* remaining) {
  const ExpectedFrames::const_iterator found =
      expected.find(FrameKey(track, timestamp_ns));
  ASSERT_TRUE(found != expected.end())
      << "unexpected frame track=" << track << " ts=" << timestamp_ns;
  EXPECT_EQ(found->second, bytes)
      << "frame content mismatch at track=" << track << " ts=" << timestamp_ns;
  if (remaining)
    remaining->erase(FrameKey(track, timestamp_ns));
}

// Verifies every block's frames, logical timestamps, block headers, and the
// independently decoded Xiph length tables against the submitted frames.
void VerifyBlocks(ParsedFile* parsed, const ExpectedFrames& expected,
                  const std::map<uint64_t, uint64_t>& default_durations,
                  size_t expected_frame_count, bool check_all_consumed) {
  ExpectedFrames remaining = expected;
  size_t total_frames = 0;

  for (size_t i = 0; i < parsed->blocks().size(); ++i) {
    const BlockInfo& block = parsed->blocks()[i];
    total_frames += static_cast<size_t>(block.frame_count);

    EXPECT_EQ(block.raw.track, block.track) << "raw block track mismatch";
    if (!block.block_group) {
      EXPECT_EQ(block.raw.flags & 0x80, block.key ? 0x80 : 0)
          << "raw block key flag mismatch";
    } else {
      // A Block inside a BlockGroup always carries a zero flags byte; keyframe
      // status is expressed through the absence of ReferenceBlock elements.
      EXPECT_EQ(block.raw.flags, 0);
    }

    const int64_t scale = parsed->timecode_scale_ns();
    ASSERT_GT(scale, 0);
    const int64_t expected_relative =
        (block.block_time_ns - block.cluster_time_ns) / scale;
    EXPECT_EQ(static_cast<int64_t>(block.raw.timecode), expected_relative)
        << "raw block timecode mismatch";

    if (block.lacing == mkvparser::Block::kLacingNone) {
      EXPECT_EQ(block.frame_count, 1);
      EXPECT_EQ(block.raw.lacing, 0) << "parser/raw lacing mismatch";
      EXPECT_EQ(block.raw.frame_count, 1);
      ASSERT_EQ(block.raw.raw_frames.size(), 1u);
    } else {
      ASSERT_EQ(block.lacing, mkvparser::Block::kLacingXiph)
          << "only Xiph lacing is expected";
      EXPECT_EQ(block.raw.lacing, 1) << "raw header does not use Xiph lacing";
      EXPECT_EQ(block.raw.frame_count, block.frame_count);
      ASSERT_EQ(block.raw.frame_lengths.size(),
                static_cast<size_t>(block.frame_count));
      for (int j = 0; j < block.frame_count; ++j) {
        EXPECT_EQ(block.raw.frame_lengths[static_cast<size_t>(j)],
                  block.frames[static_cast<size_t>(j)].size())
            << "raw Xiph length does not match frame size";
        EXPECT_EQ(block.raw.raw_frames[static_cast<size_t>(j)],
                  block.frames[static_cast<size_t>(j)])
            << "raw Xiph frame bytes do not match parser frame bytes";
      }
    }

    if (block.lacing == mkvparser::Block::kLacingNone) {
      ExpectFrame(expected, block.track,
                  static_cast<uint64_t>(block.block_time_ns), block.frames[0],
                  &remaining);
      continue;
    }

    const std::map<uint64_t, uint64_t>::const_iterator duration =
        default_durations.find(block.track);
    ASSERT_TRUE(duration != default_durations.end())
        << "missing DefaultDuration for laced track " << block.track;

    EXPECT_EQ(parsed->default_duration(block.track), duration->second)
        << "DefaultDuration was not preserved for track " << block.track;

    for (int j = 0; j < block.frame_count; ++j) {
      const uint64_t timestamp =
          static_cast<uint64_t>(block.block_time_ns) +
          static_cast<uint64_t>(j) * duration->second;
      ExpectFrame(expected, block.track, timestamp,
                  block.frames[static_cast<size_t>(j)], &remaining);
    }
  }

  EXPECT_EQ(total_frames, expected_frame_count);
  if (check_all_consumed)
    EXPECT_TRUE(remaining.empty()) << "not every submitted frame was found";
}

// Verifies that audio-only clusters contain no laced block whose frames cross
// the cluster boundary. Because each frame's logical time is derived from the
// block time plus its index, this also checks that lacing never hides frames
// that belong to another cluster.
void VerifyNoCrossClusterGroup(const ParsedFile& parsed, uint64_t track) {
  for (size_t i = 0; i < parsed.blocks().size(); ++i) {
    const BlockInfo& block = parsed.blocks()[i];
    if (block.track != track ||
        block.lacing == mkvparser::Block::kLacingNone) {
      continue;
    }
    // All frames of a laced block are in one cluster by construction, and the
    // independent raw decoder must agree with mkvparser about the frame count.
    EXPECT_EQ(block.frame_count, block.raw.frame_count);
  }
}

// Finds the block containing |track| at |timestamp_ns| (as a laced frame
// index) and returns its cluster index. Returns -1 when not found.
long FindClusterForFrame(const ParsedFile& parsed, uint64_t track,
                         uint64_t timestamp_ns,
                         uint64_t default_duration_ns) {
  for (size_t i = 0; i < parsed.blocks().size(); ++i) {
    const BlockInfo& block = parsed.blocks()[i];
    if (block.track != track)
      continue;
    if (block.lacing == mkvparser::Block::kLacingNone) {
      if (static_cast<uint64_t>(block.block_time_ns) == timestamp_ns)
        return block.cluster_index;
      continue;
    }
    for (int j = 0; j < block.frame_count; ++j) {
      const uint64_t frame_time =
          static_cast<uint64_t>(block.block_time_ns) +
          static_cast<uint64_t>(j) * default_duration_ns;
      if (frame_time == timestamp_ns)
        return block.cluster_index;
    }
  }
  return -1;
}

}  // namespace

TEST(AudioLacing, ApiValidation) {
  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, 0);
  const uint64_t video = AddVideoTrack(&segment, 2);
  ASSERT_NE(audio, 0u);
  ASSERT_NE(video, 0u);

  // DefaultDuration is required when enabling lacing.
  EXPECT_FALSE(segment.SetAudioLacing(audio, 2));

  segment.GetTrackByNumber(audio)->set_default_duration(Millis(20));

  EXPECT_FALSE(segment.SetAudioLacing(0, 2));
  EXPECT_FALSE(segment.SetAudioLacing(999, 2));
  EXPECT_FALSE(segment.SetAudioLacing(3, 2));  // No such track.
  EXPECT_FALSE(segment.SetAudioLacing(video, 2));  // Video track.
  EXPECT_FALSE(segment.SetAudioLacing(audio, 0));
  EXPECT_FALSE(segment.SetAudioLacing(audio, 257));

  // A failing call must not change the existing configuration.
  EXPECT_TRUE(segment.SetAudioLacing(audio, 2));
  EXPECT_FALSE(segment.SetAudioLacing(audio, 0));
  EXPECT_FALSE(segment.SetAudioLacing(video, 2));

  // max_frames == 1 is the legacy mode and is always accepted.
  EXPECT_TRUE(segment.SetAudioLacing(audio, 1));
  EXPECT_TRUE(segment.SetAudioLacing(audio, 2));
  EXPECT_TRUE(segment.SetAudioLacing(audio, 256));

  // Header is written by the first frame; configuration is no longer allowed.
  const std::vector<uint8_t> payload = MakePayload(1, 10);
  const std::vector<uint8_t> video_payload = MakePayload(2, 8);
  EXPECT_TRUE(segment.AddFrame(video_payload.data(), video_payload.size(),
                               video, 0, true));
  EXPECT_TRUE(segment.AddFrame(payload.data(), payload.size(), audio,
                               Millis(10), true));
  EXPECT_FALSE(segment.SetAudioLacing(audio, 2));
  EXPECT_FALSE(segment.SetAudioLacing(audio, 1));
  EXPECT_TRUE(segment.Finalize());
}

TEST(AudioLacing, ConflictingConfiguration) {
  // kLive mode.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    ASSERT_TRUE(segment.Init(&writer));
    const uint64_t audio = AddAudioTrack(&segment, 1, Millis(20));
    ASSERT_NE(audio, 0u);
    segment.set_mode(mkvmuxer::Segment::kLive);
    EXPECT_FALSE(segment.SetAudioLacing(audio, 2));
  }

  // AccurateClusterDuration.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    ASSERT_TRUE(segment.Init(&writer));
    const uint64_t audio = AddAudioTrack(&segment, 1, Millis(20));
    ASSERT_NE(audio, 0u);
    segment.AccurateClusterDuration(true);
    EXPECT_FALSE(segment.SetAudioLacing(audio, 2));
  }

  // Chunking.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    ASSERT_TRUE(segment.Init(&writer));
    const uint64_t audio = AddAudioTrack(&segment, 1, Millis(20));
    ASSERT_NE(audio, 0u);
    const std::string base = ::testing::TempDir() + "audio_lacing_chunk";
    ASSERT_TRUE(segment.SetChunking(true, base.c_str()));
    EXPECT_FALSE(segment.SetAudioLacing(audio, 2));
    segment.SetChunking(false, NULL);
    std::remove((base + ".hdr").c_str());
    std::remove((base + "_000000.chk").c_str());
  }

  // Enabling lacing first also prevents switching to chunking.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    ASSERT_TRUE(segment.Init(&writer));
    const uint64_t audio = AddAudioTrack(&segment, 1, Millis(20));
    ASSERT_NE(audio, 0u);
    ASSERT_TRUE(segment.SetAudioLacing(audio, 2));
    EXPECT_FALSE(segment.SetChunking(true, "/tmp/audio_lacing_never"));
  }

  // DefaultDuration must be positive and divisible by the timecode scale.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    ASSERT_TRUE(segment.Init(&writer));
    const uint64_t audio = AddAudioTrack(&segment, 1, Millis(20));
    ASSERT_NE(audio, 0u);
    segment.GetSegmentInfo()->set_timecode_scale(3);
    segment.GetTrackByNumber(audio)->set_default_duration(1000);  // 1000 % 3.
    EXPECT_FALSE(segment.SetAudioLacing(audio, 2));
    segment.GetTrackByNumber(audio)->set_default_duration(1002);
    EXPECT_TRUE(segment.SetAudioLacing(audio, 2));
  }

  // Conflicting settings detected before the header is written also fail the
  // first AddFrame.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    ASSERT_TRUE(segment.Init(&writer));
    const uint64_t audio = AddAudioTrack(&segment, 1, Millis(20));
    ASSERT_NE(audio, 0u);
    ASSERT_TRUE(segment.SetAudioLacing(audio, 2));
    segment.set_mode(mkvmuxer::Segment::kLive);
    const std::vector<uint8_t> payload = MakePayload(1, 10);
    EXPECT_FALSE(
        segment.AddFrame(payload.data(), payload.size(), audio, 0, true));
  }
}

TEST(AudioLacing, DefaultDisabledMatchesLegacy) {
  const std::vector<uint8_t> payload1 = MakePayload(1, 17);
  const std::vector<uint8_t> payload2 = MakePayload(2, 300);

  std::vector<uint8_t> files[2];
  for (int variant = 0; variant < 2; ++variant) {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    ASSERT_TRUE(segment.Init(&writer));
    const uint64_t audio = AddAudioTrack(&segment, 1, Millis(20));
    ASSERT_NE(audio, 0u);
    if (variant == 1)
      EXPECT_TRUE(segment.SetAudioLacing(audio, 1));

    ASSERT_TRUE(segment.AddFrame(payload1.data(), payload1.size(), audio, 0,
                                 true));
    ASSERT_TRUE(segment.AddFrame(payload2.data(), payload2.size(), audio,
                                 Millis(20), true));
    ASSERT_TRUE(segment.Finalize());
    files[variant] = writer.bytes;
  }

  EXPECT_EQ(files[0], files[1]) << "legacy and explicit max_frames=1 differ";

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(files[0]));
  ASSERT_EQ(parsed.blocks().size(), 2u);
  for (size_t i = 0; i < parsed.blocks().size(); ++i) {
    EXPECT_EQ(parsed.blocks()[i].lacing, mkvparser::Block::kLacingNone);
    EXPECT_EQ(parsed.blocks()[i].frame_count, 1);
  }
}

TEST(AudioLacing, XiphLacingLengthsAndContent) {
  const size_t lengths[7] = {1, 254, 255, 256, 510, 3, 700};
  ExpectedFrames expected;
  std::vector<uint8_t> payloads[7];
  for (int i = 0; i < 7; ++i) {
    payloads[i] = MakePayload(i + 1, lengths[i]);
    expected[FrameKey(1, Millis(static_cast<uint64_t>(i) * 20))] = payloads[i];
  }

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, Millis(20));
  ASSERT_NE(audio, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 2));

  for (int i = 0; i < 7; ++i) {
    ASSERT_TRUE(segment.AddFrame(payloads[i].data(), payloads[i].size(), audio,
                                 Millis(static_cast<uint64_t>(i) * 20), true));
  }
  ASSERT_TRUE(segment.Finalize());

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  ASSERT_EQ(parsed.blocks().size(), 4u);  // 2 + 2 + 2 + 1.

  const int expected_counts[4] = {2, 2, 2, 1};
  for (int i = 0; i < 4; ++i) {
    const BlockInfo& block = parsed.blocks()[static_cast<size_t>(i)];
    EXPECT_EQ(block.frame_count, expected_counts[i]);
    EXPECT_EQ(block.lacing == mkvparser::Block::kLacingXiph,
              expected_counts[i] > 1);
    EXPECT_EQ(block.frames[0], payloads[static_cast<size_t>(2 * i)]);
  }

  std::map<uint64_t, uint64_t> durations;
  durations[audio] = Millis(20);
  VerifyBlocks(&parsed, expected, durations, 7, true);
}

TEST(AudioLacing, MaxFrames256) {
  const uint64_t kFrameCount = 258;
  ExpectedFrames expected;
  std::vector<std::vector<uint8_t> > payloads(kFrameCount);
  for (uint64_t i = 0; i < kFrameCount; ++i) {
    payloads[i] = MakePayload(static_cast<int>(i + 1),
                              static_cast<size_t>(i % 7 + 1));
    expected[FrameKey(1, Millis(i * 10))] = payloads[i];
  }

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, Millis(10));
  ASSERT_NE(audio, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 256));

  for (uint64_t i = 0; i < kFrameCount; ++i) {
    ASSERT_TRUE(segment.AddFrame(payloads[i].data(), payloads[i].size(), audio,
                                 Millis(i * 10), true));
  }
  ASSERT_TRUE(segment.Finalize());

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  ASSERT_EQ(parsed.blocks().size(), 2u);
  EXPECT_EQ(parsed.blocks()[0].frame_count, 256);
  EXPECT_EQ(parsed.blocks()[0].lacing, mkvparser::Block::kLacingXiph);
  EXPECT_EQ(parsed.blocks()[0].raw.frame_count, 256);
  // The biased frame count byte must be 255 for a 256-frame block.
  EXPECT_EQ(parsed.blocks()[1].frame_count, 2);
  EXPECT_EQ(parsed.blocks()[1].lacing, mkvparser::Block::kLacingXiph);

  std::map<uint64_t, uint64_t> durations;
  durations[audio] = Millis(10);
  VerifyBlocks(&parsed, expected, durations, kFrameCount, true);
}

TEST(AudioLacing, IncompleteTailGroup) {
  const int kFrameCount = 5;
  ExpectedFrames expected;
  std::vector<std::vector<uint8_t> > payloads(kFrameCount);
  for (int i = 0; i < kFrameCount; ++i) {
    payloads[i] = MakePayload(i + 1, static_cast<size_t>(i + 1) * 13);
    expected[FrameKey(1, Millis(static_cast<uint64_t>(i) * 20))] = payloads[i];
  }

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, Millis(20));
  ASSERT_NE(audio, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 3));

  for (int i = 0; i < kFrameCount; ++i) {
    ASSERT_TRUE(segment.AddFrame(payloads[i].data(), payloads[i].size(), audio,
                                 Millis(static_cast<uint64_t>(i) * 20), true));
  }
  ASSERT_TRUE(segment.Finalize());

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  ASSERT_EQ(parsed.blocks().size(), 2u);
  EXPECT_EQ(parsed.blocks()[0].frame_count, 3);
  EXPECT_EQ(parsed.blocks()[1].frame_count, 2);
  EXPECT_EQ(parsed.blocks()[1].lacing, mkvparser::Block::kLacingXiph);

  std::map<uint64_t, uint64_t> durations;
  durations[audio] = Millis(20);
  VerifyBlocks(&parsed, expected, durations, kFrameCount, true);
}

TEST(AudioLacing, SegmentDurationPreserved) {
  const int kFrameCount = 5;
  const uint64_t kDuration = Millis(20);
  std::vector<std::vector<uint8_t> > payloads(kFrameCount);
  for (int i = 0; i < kFrameCount; ++i)
    payloads[i] = MakePayload(i + 1, 40);
  const uint64_t last_timestamp = Millis(static_cast<uint64_t>(
      (kFrameCount - 1) * 20));

  for (int laced = 0; laced < 2; ++laced) {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    ASSERT_TRUE(segment.Init(&writer));
    const uint64_t audio = AddAudioTrack(&segment, 1, kDuration);
    ASSERT_NE(audio, 0u);
    if (laced)
      ASSERT_TRUE(segment.SetAudioLacing(audio, 2));

    for (int i = 0; i < kFrameCount; ++i) {
      ASSERT_TRUE(segment.AddFrame(payloads[i].data(), payloads[i].size(),
                                   audio, Millis(static_cast<uint64_t>(i) * 20),
                                   true));
    }
    ASSERT_TRUE(segment.Finalize());

    ParsedFile parsed;
    ASSERT_TRUE(parsed.Parse(writer.bytes));
    EXPECT_EQ(parsed.duration_ns(), static_cast<int64_t>(last_timestamp))
        << (laced ? "laced" : "legacy")
        << " duration dropped the final group's tail";
    if (laced) {
      // The final group has a single frame, whose block time must still be its
      // own timestamp.
      EXPECT_EQ(parsed.blocks().back().block_time_ns,
                static_cast<int64_t>(last_timestamp));
    }
  }
}

TEST(AudioLacing, InterleavedAudioTracks) {
  ExpectedFrames expected;
  std::vector<std::vector<uint8_t> > payloads;
  std::vector<uint64_t> tracks;
  std::vector<uint64_t> timestamps;

  // Same-track timestamps are exactly one DefaultDuration apart, but frames
  // from the other track are interleaved between them (and share the same
  // timestamps), so no group may span the track change.
  for (int i = 0; i < 4; ++i) {
    const uint64_t ts = Millis(static_cast<uint64_t>(i) * 10);
    const std::vector<uint8_t> a = MakePayload(1, static_cast<size_t>(20 + i));
    const std::vector<uint8_t> b = MakePayload(2, static_cast<size_t>(30 + i));
    expected[FrameKey(1, ts)] = a;
    expected[FrameKey(2, ts)] = b;
    payloads.push_back(a);
    tracks.push_back(1);
    timestamps.push_back(ts);
    payloads.push_back(b);
    tracks.push_back(2);
    timestamps.push_back(ts);
  }

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio1 = AddAudioTrack(&segment, 1, Millis(10));
  const uint64_t audio2 = AddAudioTrack(&segment, 2, Millis(10));
  ASSERT_EQ(audio1, 1u);
  ASSERT_EQ(audio2, 2u);
  ASSERT_TRUE(segment.SetAudioLacing(audio1, 4));
  ASSERT_TRUE(segment.SetAudioLacing(audio2, 4));

  for (size_t i = 0; i < payloads.size(); ++i) {
    ASSERT_TRUE(segment.AddFrame(payloads[i].data(), payloads[i].size(),
                                 tracks[i], timestamps[i], true));
  }
  ASSERT_TRUE(segment.Finalize());

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  for (size_t i = 0; i < parsed.blocks().size(); ++i) {
    EXPECT_EQ(parsed.blocks()[i].frame_count, 1)
        << "interleaved tracks must truncate lacing groups";
    EXPECT_EQ(parsed.blocks()[i].lacing, mkvparser::Block::kLacingNone);
  }

  std::map<uint64_t, uint64_t> durations;
  durations[audio1] = Millis(10);
  durations[audio2] = Millis(10);
  VerifyBlocks(&parsed, expected, durations, payloads.size(), true);
}

TEST(AudioLacing, AdjacentAudioRunsLaced) {
  ExpectedFrames expected;
  std::vector<std::vector<uint8_t> > payloads;
  std::vector<uint64_t> tracks;
  std::vector<uint64_t> timestamps;

  // A0 A1, then B0 B1, then A2 A3. Timestamps increase globally; each run is
  // adjacent within its own track, so each run forms one laced group.
  uint64_t global_timestamp = 0;
  for (int run = 0; run < 3; ++run) {
    for (int in_run = 0; in_run < 2; ++in_run) {
      const int track = (run == 1) ? 2 : 1;
      const uint64_t ts = Millis(global_timestamp);
      global_timestamp += 10;
      const std::vector<uint8_t> payload =
          MakePayload(run * 10 + track * 2 + in_run, 15 + in_run);
      expected[FrameKey(static_cast<uint64_t>(track), ts)] = payload;
      payloads.push_back(payload);
      tracks.push_back(static_cast<uint64_t>(track));
      timestamps.push_back(ts);
    }
  }

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio1 = AddAudioTrack(&segment, 1, Millis(10));
  const uint64_t audio2 = AddAudioTrack(&segment, 2, Millis(10));
  ASSERT_TRUE(segment.SetAudioLacing(audio1, 2));
  ASSERT_TRUE(segment.SetAudioLacing(audio2, 2));

  for (size_t i = 0; i < payloads.size(); ++i) {
    ASSERT_TRUE(segment.AddFrame(payloads[i].data(), payloads[i].size(),
                                 tracks[i], timestamps[i], true));
  }
  ASSERT_TRUE(segment.Finalize());

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  ASSERT_EQ(parsed.blocks().size(), 3u);
  for (size_t i = 0; i < parsed.blocks().size(); ++i)
    EXPECT_EQ(parsed.blocks()[i].frame_count, 2);

  std::map<uint64_t, uint64_t> durations;
  durations[audio1] = Millis(10);
  durations[audio2] = Millis(10);
  VerifyBlocks(&parsed, expected, durations, payloads.size(), true);
}

TEST(AudioLacing, VideoKeyframeBoundary) {
  const uint64_t kDuration = Millis(10);
  ExpectedFrames expected;
  std::map<uint64_t, uint64_t> durations;
  durations[1] = kDuration;  // audio track

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, kDuration);
  const uint64_t video = AddVideoTrack(&segment, 2);
  ASSERT_NE(audio, 0u);
  ASSERT_NE(video, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 4));

  const std::vector<uint8_t> video_payload = MakePayload(99, 8);
  std::vector<std::vector<uint8_t> > audio_payloads;
  for (int i = 0; i < 6; ++i)
    audio_payloads.push_back(MakePayload(10 + i, static_cast<size_t>(30 + i)));

  // Audio at 0 and 10 are adjacent, but the keyframe at 20 forces a new
  // cluster. Likewise audio at 20 and 30 are adjacent with a keyframe at 40.
  // No laced group may combine frames from both sides of a cluster boundary.
  const uint64_t audio_times[6] = {0, 10, 20, 30, 40, 50};
  const uint64_t video_times[3] = {0, 20, 40};

  ASSERT_TRUE(segment.AddFrame(video_payload.data(), video_payload.size(),
                               video, Millis(0), true));
  ASSERT_TRUE(segment.AddFrame(audio_payloads[0].data(),
                               audio_payloads[0].size(), audio, Millis(0),
                               true));
  ASSERT_TRUE(segment.AddFrame(audio_payloads[1].data(),
                               audio_payloads[1].size(), audio, Millis(10),
                               true));
  ASSERT_TRUE(segment.AddFrame(video_payload.data(), video_payload.size(),
                               video, Millis(20), true));
  ASSERT_TRUE(segment.AddFrame(audio_payloads[2].data(),
                               audio_payloads[2].size(), audio, Millis(20),
                               true));
  ASSERT_TRUE(segment.AddFrame(audio_payloads[3].data(),
                               audio_payloads[3].size(), audio, Millis(30),
                               true));
  ASSERT_TRUE(segment.AddFrame(video_payload.data(), video_payload.size(),
                               video, Millis(40), true));
  ASSERT_TRUE(segment.AddFrame(audio_payloads[4].data(),
                               audio_payloads[4].size(), audio, Millis(40),
                               true));
  ASSERT_TRUE(segment.AddFrame(audio_payloads[5].data(),
                               audio_payloads[5].size(), audio, Millis(50),
                               true));
  ASSERT_TRUE(segment.Finalize());

  for (int i = 0; i < 6; ++i)
    expected[FrameKey(audio, Millis(audio_times[i]))] = audio_payloads[i];
  for (int i = 0; i < 3; ++i)
    expected[FrameKey(video, Millis(video_times[i]))] = video_payload;

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  ASSERT_GE(parsed.cluster_count(), 3);

  // The adjacent frames around the 20 ms keyframe must not share a block, even
  // though their timestamps are exactly one DefaultDuration apart.
  EXPECT_NE(FindClusterForFrame(parsed, audio, Millis(0), kDuration),
            FindClusterForFrame(parsed, audio, Millis(10), kDuration))
      << "lacing carried audio across a keyframe cluster boundary";
  EXPECT_NE(FindClusterForFrame(parsed, audio, Millis(20), kDuration),
            FindClusterForFrame(parsed, audio, Millis(30), kDuration))
      << "lacing carried audio across a keyframe cluster boundary";

  // Every cluster that starts with a video keyframe must contain the audio
  // from both sides only in separate blocks.
  VerifyNoCrossClusterGroup(parsed, audio);
  VerifyBlocks(&parsed, expected, durations,
               static_cast<size_t>(6 + 3), true);
}

TEST(AudioLacing, ForceNewClusterFlushesGroup) {
  const uint64_t kDuration = Millis(10);
  ExpectedFrames expected;
  std::vector<std::vector<uint8_t> > payloads;
  for (int i = 0; i < 5; ++i)
    payloads.push_back(MakePayload(i + 1, static_cast<size_t>(20 + i)));
  for (int i = 0; i < 5; ++i)
    expected[FrameKey(1, Millis(static_cast<uint64_t>(i) * 10))] = payloads[i];

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, kDuration);
  ASSERT_NE(audio, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 3));

  ASSERT_TRUE(segment.AddFrame(payloads[0].data(), payloads[0].size(), audio, 0,
                               true));
  ASSERT_TRUE(segment.AddFrame(payloads[1].data(), payloads[1].size(), audio,
                               Millis(10), true));
  segment.ForceNewClusterOnNextFrame();
  ASSERT_TRUE(segment.AddFrame(payloads[2].data(), payloads[2].size(), audio,
                               Millis(20), true));
  ASSERT_TRUE(segment.AddFrame(payloads[3].data(), payloads[3].size(), audio,
                               Millis(30), true));
  ASSERT_TRUE(segment.AddFrame(payloads[4].data(), payloads[4].size(), audio,
                               Millis(40), true));
  ASSERT_TRUE(segment.Finalize());

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  ASSERT_GE(parsed.cluster_count(), 2);
  const std::vector<BlockInfo> first_cluster = parsed.blocks_in_cluster(0);
  ASSERT_EQ(first_cluster.size(), 1u);
  EXPECT_EQ(first_cluster[0].frame_count, 2)
      << "the incomplete group must be flushed before the forced cluster";
  const std::vector<BlockInfo> second_cluster = parsed.blocks_in_cluster(1);
  ASSERT_EQ(second_cluster.size(), 1u);
  EXPECT_EQ(second_cluster[0].frame_count, 3);

  std::map<uint64_t, uint64_t> durations;
  durations[audio] = kDuration;
  VerifyBlocks(&parsed, expected, durations, 5, true);
}

TEST(AudioLacing, MaxClusterDurationPerInputFrame) {
  const uint64_t kDuration = Millis(10);
  const uint64_t kMaxClusterDuration = Millis(25);
  ExpectedFrames expected;
  std::vector<std::vector<uint8_t> > payloads(7);
  for (int i = 0; i < 7; ++i) {
    payloads[i] = MakePayload(i + 1, static_cast<size_t>(10 + i));
    expected[FrameKey(1, Millis(static_cast<uint64_t>(i) * 10))] = payloads[i];
  }

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, kDuration);
  ASSERT_NE(audio, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 4));
  segment.set_max_cluster_duration(kMaxClusterDuration);

  for (int i = 0; i < 7; ++i) {
    ASSERT_TRUE(segment.AddFrame(payloads[i].data(), payloads[i].size(), audio,
                                 Millis(static_cast<uint64_t>(i) * 10), true));
  }
  ASSERT_TRUE(segment.Finalize());

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  // The timestamp of the fourth input frame (30 ms) triggers the switch even
  // though the pending group begins at 0 ms.
  ASSERT_EQ(parsed.cluster_count(), 3);
  const std::vector<BlockInfo> cluster0 = parsed.blocks_in_cluster(0);
  const std::vector<BlockInfo> cluster1 = parsed.blocks_in_cluster(1);
  const std::vector<BlockInfo> cluster2 = parsed.blocks_in_cluster(2);
  ASSERT_EQ(cluster0.size(), 1u);
  ASSERT_EQ(cluster1.size(), 1u);
  ASSERT_EQ(cluster2.size(), 1u);
  EXPECT_EQ(cluster0[0].frame_count, 3);  // 0, 10, 20
  EXPECT_EQ(cluster1[0].frame_count, 3);  // 30, 40, 50
  EXPECT_EQ(cluster2[0].frame_count, 1);  // 60

  std::map<uint64_t, uint64_t> durations;
  durations[audio] = kDuration;
  VerifyBlocks(&parsed, expected, durations, 7, true);
}

TEST(AudioLacing, MaxClusterSizeIncludesPendingGroup) {
  const uint64_t kDuration = Millis(10);
  const size_t kPayloadSize = 500;
  ExpectedFrames expected;
  std::vector<std::vector<uint8_t> > payloads(8);
  for (int i = 0; i < 8; ++i) {
    payloads[i] = MakePayload(i + 1, kPayloadSize);
    expected[FrameKey(1, Millis(static_cast<uint64_t>(i) * 10))] = payloads[i];
  }

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, kDuration);
  ASSERT_NE(audio, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 4));
  // 1200 bytes sits between the size of a two-frame and a three-frame pending
  // group. Without counting the pending group, all four frames would be laced
  // into the first block; with it, the cluster switches first.
  segment.set_max_cluster_size(1200);

  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(segment.AddFrame(payloads[i].data(), payloads[i].size(), audio,
                                 Millis(static_cast<uint64_t>(i) * 10), true));
  }
  ASSERT_TRUE(segment.Finalize());

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  ASSERT_GE(parsed.cluster_count(), 2);
  const std::vector<BlockInfo> cluster0 = parsed.blocks_in_cluster(0);
  ASSERT_EQ(cluster0.size(), 1u);
  EXPECT_EQ(cluster0[0].frame_count, 3)
      << "pending group size was not included in the cluster size test";

  std::map<uint64_t, uint64_t> durations;
  durations[audio] = kDuration;
  VerifyBlocks(&parsed, expected, durations, 8, true);
}

TEST(AudioLacing, SpecialFramesTruncateGroup) {
  const uint64_t kDuration = Millis(10);
  const uint64_t kAdditionalId = 4;
  ExpectedFrames expected;
  std::vector<uint8_t> payloads[8];
  for (int i = 0; i < 8; ++i)
    payloads[i] = MakePayload(i + 1, static_cast<size_t>(25 + i));

  // Normal 0, 10; discard-padded 20; normal 30, 40; additional-data 50;
  // metadata (explicit duration) 60; normal 70.
  for (int i = 0; i < 8; ++i)
    expected[FrameKey(1, Millis(static_cast<uint64_t>(i) * 10))] = payloads[i];

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, kDuration);
  ASSERT_NE(audio, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 4));

  ASSERT_TRUE(segment.AddFrame(payloads[0].data(), payloads[0].size(), audio, 0,
                               true));
  ASSERT_TRUE(segment.AddFrame(payloads[1].data(), payloads[1].size(), audio,
                               Millis(10), true));
  ASSERT_TRUE(segment.AddFrameWithDiscardPadding(
      payloads[2].data(), payloads[2].size(), -5, audio, Millis(20), true));
  ASSERT_TRUE(segment.AddFrame(payloads[3].data(), payloads[3].size(), audio,
                               Millis(30), true));
  ASSERT_TRUE(segment.AddFrame(payloads[4].data(), payloads[4].size(), audio,
                               Millis(40), true));

  const std::vector<uint8_t> additional = MakePayload(77, 6);
  // A non-key frame with additional data also carries a ReferenceBlock, and
  // must still be written individually.
  ASSERT_TRUE(segment.AddFrameWithAdditional(
      payloads[5].data(), payloads[5].size(), additional.data(),
      additional.size(), kAdditionalId, audio, Millis(50), false));

  ASSERT_TRUE(segment.AddMetadata(payloads[6].data(), payloads[6].size(), audio,
                                  Millis(60), Millis(5)));
  ASSERT_TRUE(segment.AddFrame(payloads[7].data(), payloads[7].size(), audio,
                               Millis(70), true));
  ASSERT_TRUE(segment.Finalize());

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  // Groups: [0,10], [20], [30,40], [50], [60], [70].
  ASSERT_EQ(parsed.blocks().size(), 6u);
  EXPECT_EQ(parsed.blocks()[0].frame_count, 2);
  EXPECT_EQ(parsed.blocks()[1].frame_count, 1);
  EXPECT_EQ(parsed.blocks()[1].lacing, mkvparser::Block::kLacingNone);
  EXPECT_EQ(parsed.blocks()[2].frame_count, 2);
  EXPECT_EQ(parsed.blocks()[3].frame_count, 1);
  EXPECT_EQ(parsed.blocks()[4].frame_count, 1);
  EXPECT_EQ(parsed.blocks()[5].frame_count, 1);

  const BlockInfo* discard_block = NULL;
  const BlockInfo* additional_block = NULL;
  const BlockInfo* metadata_block = NULL;
  for (size_t i = 0; i < parsed.blocks().size(); ++i) {
    const BlockInfo& block = parsed.blocks()[i];
    if (block.block_time_ns == static_cast<int64_t>(Millis(20)))
      discard_block = &block;
    if (block.block_time_ns == static_cast<int64_t>(Millis(50)))
      additional_block = &block;
    if (block.block_time_ns == static_cast<int64_t>(Millis(60)))
      metadata_block = &block;
  }

  ASSERT_TRUE(discard_block != NULL);
  EXPECT_TRUE(discard_block->block_group);
  EXPECT_EQ(discard_block->frame_count, 1);
  EXPECT_EQ(discard_block->discard_padding, -5);
  ASSERT_TRUE(additional_block != NULL);
  EXPECT_TRUE(additional_block->block_group);
  EXPECT_EQ(additional_block->frame_count, 1);
  EXPECT_FALSE(additional_block->key)
      << "the reference-block frame must not be treated as a keyframe";
  ASSERT_TRUE(metadata_block != NULL);
  EXPECT_TRUE(metadata_block->block_group);
  EXPECT_EQ(metadata_block->frame_count, 1);

  std::map<uint64_t, uint64_t> durations;
  durations[audio] = kDuration;
  VerifyBlocks(&parsed, expected, durations, 8, true);
}

TEST(AudioLacing, WriteFailurePropagatesFromAddFrame) {
  const uint64_t kDuration = Millis(10);
  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, kDuration);
  ASSERT_NE(audio, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 2));

  const std::vector<uint8_t> payload1 = MakePayload(1, 100);
  const std::vector<uint8_t> payload2 = MakePayload(2, 100);
  ASSERT_TRUE(
      segment.AddFrame(payload1.data(), payload1.size(), audio, 0, true));

  writer.StartFailing(50);
  EXPECT_FALSE(segment.AddFrame(payload2.data(), payload2.size(), audio,
                                Millis(10), true))
      << "a failed laced group write must not be reported as success";
}

TEST(AudioLacing, WriteFailurePropagatesFromFinalize) {
  const uint64_t kDuration = Millis(10);
  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, kDuration);
  ASSERT_NE(audio, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 4));

  const std::vector<uint8_t> payload1 = MakePayload(1, 100);
  const std::vector<uint8_t> payload2 = MakePayload(2, 100);
  ASSERT_TRUE(
      segment.AddFrame(payload1.data(), payload1.size(), audio, 0, true));
  ASSERT_TRUE(segment.AddFrame(payload2.data(), payload2.size(), audio,
                               Millis(10), true));

  writer.StartFailing(50);
  EXPECT_FALSE(segment.Finalize())
      << "a failed laced group write during Finalize must be reported";
}

TEST(AudioLacing, AudioCuePointsToGroup) {
  const uint64_t kDuration = Millis(20);
  const size_t kFrameCount = 6;
  ExpectedFrames expected;
  std::vector<std::vector<uint8_t> > payloads(kFrameCount);
  for (size_t i = 0; i < kFrameCount; ++i) {
    payloads[i] = MakePayload(static_cast<int>(i + 1), 20 + i);
    expected[FrameKey(1, Millis(static_cast<uint64_t>(i) * 20))] = payloads[i];
  }

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, kDuration);
  ASSERT_NE(audio, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 3));
  ASSERT_TRUE(segment.CuesTrack(audio));

  for (size_t i = 0; i < kFrameCount; ++i) {
    ASSERT_TRUE(segment.AddFrame(payloads[i].data(), payloads[i].size(), audio,
                                 Millis(static_cast<uint64_t>(i) * 20), true));
  }
  ASSERT_TRUE(segment.Finalize());

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  std::vector<CueInfo> cues;
  ASSERT_TRUE(parsed.ParseCues(audio, &cues));
  ASSERT_EQ(cues.size(), 1u);

  const CueInfo& cue = cues[0];
  EXPECT_EQ(cue.time_ns, 0);
  EXPECT_EQ(cue.track, audio);
  EXPECT_EQ(cue.referenced_frame_count, 3)
      << "audio cue must point at the laced group's first block";
  EXPECT_EQ(cue.referenced_lacing, mkvparser::Block::kLacingXiph);
  EXPECT_TRUE(cue.referenced_key);

  // The block number must be the physical block index (1-based), not the
  // number of input frames that preceded the group.
  ASSERT_FALSE(parsed.blocks().empty());
  EXPECT_EQ(cue.block_number, parsed.blocks()[0].block_index);
  EXPECT_EQ(cue.block_number, 1);

  // Independently verify that the cue's cluster position names an actual
  // cluster start in the container.
  EXPECT_TRUE(std::find(parsed.cluster_positions().begin(),
                        parsed.cluster_positions().end(),
                        cue.cluster_pos) != parsed.cluster_positions().end())
      << "cue cluster position does not point at a cluster";
}

TEST(AudioLacing, VideoCuesRemainKeyframes) {
  const uint64_t kDuration = Millis(10);
  ExpectedFrames expected;
  std::map<uint64_t, uint64_t> durations;
  durations[1] = kDuration;

  MemoryWriter writer;
  mkvmuxer::Segment segment;
  ASSERT_TRUE(segment.Init(&writer));
  const uint64_t audio = AddAudioTrack(&segment, 1, kDuration);
  const uint64_t video = AddVideoTrack(&segment, 2);
  ASSERT_NE(audio, 0u);
  ASSERT_NE(video, 0u);
  ASSERT_TRUE(segment.SetAudioLacing(audio, 4));
  // Default cues track is the video track.

  const std::vector<uint8_t> video_payload = MakePayload(9, 8);
  std::vector<std::vector<uint8_t> > audio_payloads;
  for (int i = 0; i < 6; ++i)
    audio_payloads.push_back(MakePayload(20 + i, static_cast<size_t>(20 + i)));

  const uint64_t video_times[3] = {0, 20, 40};
  for (int v = 0; v < 3; ++v) {
    ASSERT_TRUE(segment.AddFrame(video_payload.data(), video_payload.size(),
                                 video, Millis(video_times[v]), true));
    ASSERT_TRUE(segment.AddFrame(
        audio_payloads[static_cast<size_t>(2 * v)].data(),
        audio_payloads[static_cast<size_t>(2 * v)].size(), audio,
        Millis(video_times[v]), true));
    ASSERT_TRUE(segment.AddFrame(
        audio_payloads[static_cast<size_t>(2 * v + 1)].data(),
        audio_payloads[static_cast<size_t>(2 * v + 1)].size(), audio,
        Millis(video_times[v] + 10), true));
  }
  ASSERT_TRUE(segment.Finalize());

  for (int v = 0; v < 3; ++v)
    expected[FrameKey(video, Millis(video_times[v]))] = video_payload;
  for (int i = 0; i < 6; ++i)
    expected[FrameKey(audio, Millis(static_cast<uint64_t>(i) * 10))] =
        audio_payloads[static_cast<size_t>(i)];

  ParsedFile parsed;
  ASSERT_TRUE(parsed.Parse(writer.bytes));
  std::vector<CueInfo> cues;
  ASSERT_TRUE(parsed.ParseCues(video, &cues));
  ASSERT_EQ(cues.size(), 3u);

  for (size_t i = 0; i < cues.size(); ++i) {
    EXPECT_EQ(cues[i].track, video);
    EXPECT_TRUE(cues[i].referenced_key) << "video cue must point at a keyframe";
    EXPECT_EQ(cues[i].time_ns, static_cast<int64_t>(Millis(video_times[i])));

    const std::vector<long long>::const_iterator position =
        std::find(parsed.cluster_positions().begin(),
                  parsed.cluster_positions().end(), cues[i].cluster_pos);
    ASSERT_TRUE(position != parsed.cluster_positions().end())
        << "cue cluster position does not point at a cluster";
    const long cluster_index =
        static_cast<long>(position - parsed.cluster_positions().begin());

    const BlockInfo* video_block = NULL;
    for (size_t j = 0; j < parsed.blocks().size(); ++j) {
      const BlockInfo& block = parsed.blocks()[j];
      if (block.cluster_index == cluster_index && block.track == video &&
          block.block_time_ns == static_cast<int64_t>(Millis(video_times[i]))) {
        video_block = &block;
        break;
      }
    }
    ASSERT_TRUE(video_block != NULL);
    EXPECT_EQ(cues[i].block_number, video_block->block_index)
        << "video cue must point at the physical keyframe block";
  }

  VerifyBlocks(&parsed, expected, durations, 9, true);
}
