// Copyright (c) 2012 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.

// Regression tests for Segment::SetStandaloneChunking().
//
// The tests write chunk files into a temporary directory under the current
// working directory, then parse each file on its own with mkvparser and
// verify that payloads, timings, tracks and cues all survive the split.

#include "task_support/check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"
#include "mkvparser/mkvparser.h"
#include "mkvparser/mkvreader.h"

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct TestFrame {
  int track;
  uint64_t timestamp_ns;
  uint64_t duration_ns;
  bool key;
  int64_t discard_padding;
  uint8_t payload[6];
  int payload_length;

  TestFrame()
      : track(0),
        timestamp_ns(0),
        duration_ns(0),
        key(false),
        discard_padding(0),
        payload_length(4) {}
};

TestFrame MakeFrame(int track, uint64_t timestamp_ns, uint64_t duration_ns,
                    bool key, uint32_t serial) {
  TestFrame frame;
  frame.track = track;
  frame.timestamp_ns = timestamp_ns;
  frame.duration_ns = duration_ns;
  frame.key = key;
  frame.payload[0] = static_cast<uint8_t>(track);
  frame.payload[1] = static_cast<uint8_t>(serial & 0xff);
  frame.payload[2] = static_cast<uint8_t>((serial >> 8) & 0xff);
  frame.payload[3] = 0x55;
  frame.payload_length = 4;
  return frame;
}

bool AddTestFrame(mkvmuxer::Segment* segment, const TestFrame& test_frame) {
  mkvmuxer::Frame frame;
  if (!frame.Init(test_frame.payload, test_frame.payload_length))
    return false;
  frame.set_track_number(test_frame.track);
  frame.set_timestamp(test_frame.timestamp_ns);
  frame.set_duration(test_frame.duration_ns);
  frame.set_is_key(test_frame.key);
  if (test_frame.discard_padding != 0)
    frame.set_discard_padding(test_frame.discard_padding);
  return segment->AddGenericFrame(&frame);
}

// Parsed view of one output chunk.
struct ParsedFrame {
  long long track;
  long long timestamp_ns;
  bool key;
  bool has_duration;
  long long duration_ns;
  bool has_discard_padding;
  long long discard_padding;
  std::vector<uint8_t> payload;
};

struct ParsedChunk {
  ParsedChunk()
      : segment(NULL),
        reader(NULL),
        duration_ns(-1),
        timecode_scale(0),
        segment_payload_start(0) {}
  ~ParsedChunk() {
    delete segment;
    delete reader;
  }

  std::vector<ParsedFrame> frames;
  mkvparser::Segment* segment;
  mkvparser::MkvReader* reader;
  long long duration_ns;
  long long timecode_scale;
  long long segment_payload_start;
};

// Opens |path| with a fresh mkvparser instance and walks every block.
bool ParseChunk(const std::string& path, ParsedChunk* out) {
  if (!out)
    return false;

  std::unique_ptr<mkvparser::MkvReader> reader(new mkvparser::MkvReader());
  if (reader->Open(path.c_str()))
    return false;

  long long pos = 0;
  mkvparser::EBMLHeader header;
  if (header.Parse(reader.get(), pos))
    return false;
  if (header.m_docType == NULL || strcmp(header.m_docType, "webm") != 0)
    return false;

  mkvparser::Segment* raw_segment = NULL;
  if (mkvparser::Segment::CreateInstance(reader.get(), pos, raw_segment))
    return false;
  if (!raw_segment)
    return false;

  std::unique_ptr<mkvparser::Segment> segment(raw_segment);
  if (segment->Load())
    return false;

  const mkvparser::SegmentInfo* const info = segment->GetInfo();
  if (!info)
    return false;
  if (!segment->GetTracks())
    return false;
  if (!segment->GetSeekHead()) {
    return false;
  }

  out->duration_ns = info->GetDuration();
  out->timecode_scale = info->GetTimeCodeScale();
  out->segment_payload_start = segment->m_start;

  for (const mkvparser::Cluster* cluster = segment->GetFirst();
       cluster && !cluster->EOS(); cluster = segment->GetNext(cluster)) {
    const mkvparser::BlockEntry* entry = NULL;
    if (cluster->GetFirst(entry))
      return false;
    while (entry && !entry->EOS()) {
      const mkvparser::Block* const block = entry->GetBlock();
      const int frame_count = block->GetFrameCount();
      for (int i = 0; i < frame_count; ++i) {
        const mkvparser::Block::Frame& block_frame = block->GetFrame(i);
        ParsedFrame parsed;
        parsed.track = block->GetTrackNumber();
        parsed.timestamp_ns = block->GetTime(cluster);
        parsed.key = block->IsKey();
        parsed.has_duration = false;
        parsed.duration_ns = 0;
        parsed.has_discard_padding = false;
        parsed.discard_padding = block->GetDiscardPadding();
        if (entry->GetKind() == mkvparser::BlockEntry::kBlockGroup) {
          const mkvparser::BlockGroup* const group =
              static_cast<const mkvparser::BlockGroup*>(entry);
          const long long duration_tc = group->GetDurationTimeCode();
          if (duration_tc >= 0) {
            parsed.has_duration = true;
            parsed.duration_ns = duration_tc * out->timecode_scale;
          }
        }
        if (parsed.discard_padding != 0)
          parsed.has_discard_padding = true;
        parsed.payload.resize(static_cast<size_t>(block_frame.len));
        if (block_frame.Read(reader.get(), parsed.payload.data()))
          return false;
        out->frames.push_back(parsed);
      }
      if (cluster->GetNext(entry, entry))
        return false;
    }
  }

  out->segment = segment.release();
  out->reader = reader.release();
  return true;
}

// Returns true when |path| exists.
bool FileExists(const std::string& path) {
  FILE* const file = fopen(path.c_str(), "rb");
  if (!file)
    return false;
  fclose(file);
  return true;
}

// Removes all chunk files matching a base name.
void RemoveChunksWithPrefix(const std::string& prefix) {
  for (uint32_t i = 0; i < 64; ++i) {
    char suffix[32];
    snprintf(suffix, sizeof(suffix), "_%06u.webm", i);
    remove((prefix + suffix).c_str());
  }
}

std::string ChunkPath(const std::string& base_name, uint32_t index) {
  char suffix[32];
  snprintf(suffix, sizeof(suffix), "_%06u.webm", index);
  return base_name + suffix;
}

uint32_t CountChunks(const std::string& base_name) {
  uint32_t count = 0;
  while (FileExists(ChunkPath(base_name, count)))
    ++count;
  return count;
}

// Configuration shared by the test writers.
struct WriterOptions {
  std::string base_name;
  uint64_t max_cluster_duration;
  bool use_vp9;
  bool with_audio;
  uint64_t timecode_scale;

  WriterOptions()
      : max_cluster_duration(1000000000ULL),
        use_vp9(false),
        with_audio(true),
        timecode_scale(1000000ULL) {}
};

const uint8_t kOpusHead[] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2,
                             0,   0,   0x80, 0xbb, 0,   0,   0,   0,   0};

// A CodecPrivate payload for the video track. Its contents are opaque to the
// container, so it only needs to round-trip byte for byte.
const uint8_t kVideoCodecPrivate[] = {0xde, 0xad, 0xbe, 0xef, 0x42};

// Opens a writer, configures the tracks and enables standalone chunking.
// |writer| is required by Segment::Init() but is not written to on the
// standalone path, so callers that only produce chunks leave it unopened.
bool StartWriter(const WriterOptions& options, mkvmuxer::MkvWriter* writer,
                 mkvmuxer::Segment* segment) {
  if (!segment->Init(writer))
    return false;
  segment->GetSegmentInfo()->set_timecode_scale(options.timecode_scale);
  segment->GetSegmentInfo()->set_writing_app("standalone-chunk-tests");
  if (!segment->SetStandaloneChunking(options.base_name.c_str()))
    return false;

  const uint64_t video_number =
      segment->AddVideoTrack(320, 180, 1);
  if (video_number != 1)
    return false;
  if (options.use_vp9)
    segment->GetTrackByNumber(1)->set_codec_id("V_VP9");
  segment->GetTrackByNumber(1)->set_uid(101);
  if (!segment->GetTrackByNumber(1)->SetCodecPrivate(
          kVideoCodecPrivate, sizeof(kVideoCodecPrivate)))
    return false;

  if (options.with_audio) {
    const uint64_t audio_number = segment->AddAudioTrack(48000, 2, 2);
    if (audio_number != 2)
      return false;
    mkvmuxer::Track* const audio = segment->GetTrackByNumber(2);
    audio->set_codec_id("A_OPUS");
    audio->set_uid(202);
    audio->set_codec_delay(6500000);
    audio->set_seek_pre_roll(80000000);
    if (!audio->SetCodecPrivate(kOpusHead, sizeof(kOpusHead)))
      return false;
  }

  segment->set_max_cluster_duration(options.max_cluster_duration);
  return true;
}

// Writes |frames| through a standalone chunking segment and finalizes it.
bool WriteChunks(const WriterOptions& options,
                 const std::vector<TestFrame>& frames) {
  RemoveChunksWithPrefix(options.base_name);
  mkvmuxer::MkvWriter writer;
  mkvmuxer::Segment segment;
  if (!StartWriter(options, &writer, &segment))
    return false;
  for (size_t i = 0; i < frames.size(); ++i) {
    if (!AddTestFrame(&segment, frames[i]))
      return false;
  }
  const bool result = segment.Finalize();
  writer.Close();
  return result;
}

// ---------------------------------------------------------------------------
// Sync-safe frame builders
// ---------------------------------------------------------------------------

// Video key frame every |gop| frames at |frame_duration| spacing.
std::vector<TestFrame> MakeVideoOnly(int video_frames, uint64_t frame_duration,
                                     int gop) {
  std::vector<TestFrame> frames;
  uint32_t serial = 1;
  for (int i = 0; i < video_frames; ++i) {
    const bool key = (i % gop) == 0;
    frames.push_back(MakeFrame(1, static_cast<uint64_t>(i) * frame_duration,
                               frame_duration, key, serial++));
  }
  return frames;
}

// Adds interleaved audio at |audio_duration| intervals to |video|. Audio and
// video at the same timestamp are submitted audio-first to exercise same-tick
// boundary handling.
std::vector<TestFrame> WithAudio(const std::vector<TestFrame>& video,
                                 uint64_t audio_duration) {
  std::vector<TestFrame> merged;
  uint32_t serial = 5000;
  size_t video_index = 0;
  uint64_t audio_time = 0;
  uint64_t video_end = 0;
  if (!video.empty()) {
    video_end = video.back().timestamp_ns + video.back().duration_ns;
  }

  while (video_index < video.size() || audio_time < video_end) {
    const uint64_t next_video =
        video_index < video.size() ? video[video_index].timestamp_ns
                                   : UINT64_MAX;
    if (audio_time <= next_video && audio_time < video_end) {
      merged.push_back(MakeFrame(2, audio_time, audio_duration, true, serial++));
      audio_time += audio_duration;
    }
    if (video_index < video.size() &&
        (audio_time > next_video || audio_time >= video_end)) {
      merged.push_back(video[video_index++]);
    }
  }
  return merged;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

const char kTempDir[] = ".task-build/standalone-chunks-test";

int SetupTempDir() {
  // mkdir() fails with EEXIST, which is fine for a directory that is already
  // there; treat that as success by probing whether it is usable.
  if (mkdir(kTempDir, 0777) == 0)
    return 0;
  FILE* const probe = fopen((std::string(kTempDir) + "/.probe").c_str(), "wb");
  if (!probe)
    return 1;
  fclose(probe);
  remove((std::string(kTempDir) + "/.probe").c_str());
  return 0;
}

// Split: key frames cross the target duration; check file count, cut points,
// track config stability and per-file duration.
int TestBasicSplit() {
  TASK_CHECK(SetupTempDir() == 0);
  const uint64_t kFrameDuration = 10000000;  // 10 ms
  const uint64_t kChunkTarget = 100000000;   // 100 ms
  WriterOptions options;
  options.base_name = std::string(kTempDir) + "/split";
  options.max_cluster_duration = kChunkTarget;
  options.with_audio = false;

  // 60 frames at 10 ms each: key frames every 10 frames (100 ms apart).
  const std::vector<TestFrame> frames = MakeVideoOnly(60, kFrameDuration, 10);
  TASK_CHECK(WriteChunks(options, frames));

  const uint32_t chunk_count = CountChunks(options.base_name);
  TASK_CHECK(chunk_count == 6);

  uint64_t expected_start = 0;
  uint32_t serial = 1;
  for (uint32_t i = 0; i < chunk_count; ++i) {
    ParsedChunk chunk;
    TASK_CHECK(ParseChunk(ChunkPath(options.base_name, i), &chunk));

    // Every chunk carries EBML, Info, Tracks and Cues.
    TASK_CHECK(chunk.segment != NULL);
    TASK_CHECK(chunk.segment->GetInfo() != NULL);
    TASK_CHECK(chunk.segment->GetTracks() != NULL);
    TASK_CHECK(chunk.segment->GetSeekHead() != NULL);
    TASK_CHECK(chunk.segment->GetCues() != NULL);
    TASK_CHECK(chunk.timecode_scale == 1000000);

    const size_t expected_frames = 10;
    TASK_CHECK(chunk.frames.size() == expected_frames);

    // First frame of every chunk is the key frame that started it.
    TASK_CHECK(chunk.frames.front().key);
    TASK_CHECK(chunk.frames.front().timestamp_ns == 0);

    uint64_t expected_presentation_end = 0;
    uint32_t expected_payload = serial;
    for (size_t j = 0; j < chunk.frames.size(); ++j) {
      const ParsedFrame& frame = chunk.frames[j];
      TASK_CHECK(frame.track == 1);
      TASK_CHECK(frame.timestamp_ns ==
                 static_cast<long long>(j * kFrameDuration));
      TASK_CHECK(frame.has_duration);
      TASK_CHECK(frame.duration_ns == static_cast<long long>(kFrameDuration));
      TASK_CHECK(frame.key == ((j % 10) == 0));
      TASK_CHECK(frame.payload.size() == 4);
      TASK_CHECK(frame.payload[0] == 1);
      TASK_CHECK(frame.payload[3] == 0x55);
      const uint32_t payload_serial =
          frame.payload[1] | (static_cast<uint32_t>(frame.payload[2]) << 8);
      TASK_CHECK(payload_serial == expected_payload);
      ++expected_payload;
      expected_presentation_end = static_cast<uint64_t>(frame.timestamp_ns) +
                                  static_cast<uint64_t>(frame.duration_ns);
    }
    serial += static_cast<uint32_t>(expected_frames);

    // Duration covers only this chunk's frames.
    TASK_CHECK(chunk.duration_ns ==
               static_cast<long long>(expected_presentation_end));
    TASK_CHECK(chunk.duration_ns <= static_cast<long long>(kChunkTarget));
    expected_start += expected_presentation_end;
  }
  // The full recording spans 600 ms.
  TASK_CHECK(expected_start == 600000000ULL);
  return 0;
}

// Split: a long GOP delays the cut until the next key frame; non-key frames
// and audio must not force a boundary.
int TestLongGopDelaysCut() {
  TASK_CHECK(SetupTempDir() == 0);
  const uint64_t kFrameDuration = 10000000;  // 10 ms
  const uint64_t kChunkTarget = 100000000;   // 100 ms
  WriterOptions options;
  options.base_name = std::string(kTempDir) + "/long_gop";
  options.max_cluster_duration = kChunkTarget;
  options.with_audio = false;

  // 40 frames: keys at 0, 18 and 36. Frames 10..17 pass the target duration
  // but are not key frames, so the first cut is delayed to frame 18 (180 ms).
  // The key frame at 36 (360 ms) is another 180 ms past that chunk's start,
  // cutting again, and the remaining frames form the tail chunk.
  std::vector<TestFrame> frames;
  uint32_t serial = 1;
  for (int i = 0; i < 40; ++i) {
    const bool key = (i == 0 || i == 18 || i == 36);
    frames.push_back(MakeFrame(1, static_cast<uint64_t>(i) * kFrameDuration,
                               kFrameDuration, key, serial++));
  }
  TASK_CHECK(WriteChunks(options, frames));

  const uint32_t chunk_count = CountChunks(options.base_name);
  TASK_CHECK(chunk_count == 3);

  ParsedChunk first;
  TASK_CHECK(ParseChunk(ChunkPath(options.base_name, 0), &first));
  TASK_CHECK(first.frames.size() == 18);
  TASK_CHECK(first.frames.back().timestamp_ns ==
             static_cast<long long>(17 * kFrameDuration));

  ParsedChunk second;
  TASK_CHECK(ParseChunk(ChunkPath(options.base_name, 1), &second));
  TASK_CHECK(second.frames.size() == 18);
  TASK_CHECK(second.frames.front().key);
  TASK_CHECK(second.frames.front().timestamp_ns == 0);

  ParsedChunk third;
  TASK_CHECK(ParseChunk(ChunkPath(options.base_name, 2), &third));
  TASK_CHECK(third.frames.size() == 4);
  TASK_CHECK(third.frames.front().key);
  TASK_CHECK(third.frames.front().timestamp_ns == 0);

  // No cut was placed on a non-key frame: only key frames have time zero.
  TASK_CHECK(!first.frames[10].key);
  return 0;
}

// Split: no cut during the tail — only the final chunk is emitted when the
// recording ends before the target duration is reached.
int TestShortRecordingSingleChunk() {
  TASK_CHECK(SetupTempDir() == 0);
  const uint64_t kFrameDuration = 10000000;
  WriterOptions options;
  options.base_name = std::string(kTempDir) + "/short";
  options.max_cluster_duration = 1000000000ULL;  // 1 s target
  options.with_audio = false;

  const std::vector<TestFrame> frames = MakeVideoOnly(5, kFrameDuration, 5);
  TASK_CHECK(WriteChunks(options, frames));
  TASK_CHECK(CountChunks(options.base_name) == 1);

  ParsedChunk chunk;
  TASK_CHECK(ParseChunk(ChunkPath(options.base_name, 0), &chunk));
  TASK_CHECK(chunk.frames.size() == 5);
  TASK_CHECK(chunk.duration_ns == 50000000);
  return 0;
}

// Split: empty input must not create a file.
int TestEmptyInputNoOutput() {
  TASK_CHECK(SetupTempDir() == 0);
  WriterOptions options;
  options.base_name = std::string(kTempDir) + "/empty";
  options.with_audio = false;

  RemoveChunksWithPrefix(options.base_name);
  mkvmuxer::MkvWriter writer;
  mkvmuxer::Segment segment;
  TASK_CHECK(StartWriter(options, &writer, &segment));
  TASK_CHECK(segment.Finalize());
  writer.Close();
  TASK_CHECK(!FileExists(ChunkPath(options.base_name, 0)));
  TASK_CHECK(CountChunks(options.base_name) == 0);
  return 0;
}

// Audio: audio on the cut timestamp belongs to the new chunk even when it is
// submitted before the video key frame at the same timestamp. Audio strictly
// before the cut stays in the old chunk.
int TestAudioAtBoundary() {
  TASK_CHECK(SetupTempDir() == 0);
  const uint64_t kFrameDuration = 10000000;   // video every 10 ms
  const uint64_t kAudioDuration = 10000000;   // audio every 10 ms
  const uint64_t kChunkTarget = 100000000;    // cut at the 100 ms key frame
  WriterOptions options;
  options.base_name = std::string(kTempDir) + "/audio_boundary";
  options.max_cluster_duration = kChunkTarget;

  // Video: key frames at 0 and 100 ms, further key frame at 200 ms.
  std::vector<TestFrame> video;
  uint32_t serial = 1;
  for (int i = 0; i < 21; ++i) {
    const bool key = (i % 10) == 0;
    video.push_back(MakeFrame(1, static_cast<uint64_t>(i) * kFrameDuration,
                              kFrameDuration, key, serial++));
  }
  const std::vector<TestFrame> frames = WithAudio(video, kAudioDuration);
  TASK_CHECK(WriteChunks(options, frames));

  const uint32_t chunk_count = CountChunks(options.base_name);
  TASK_CHECK(chunk_count == 3);

  ParsedChunk first;
  TASK_CHECK(ParseChunk(ChunkPath(options.base_name, 0), &first));
  ParsedChunk second;
  TASK_CHECK(ParseChunk(ChunkPath(options.base_name, 1), &second));

  // The audio at exactly 100 ms must be in the second chunk, not the first.
  bool first_has_cut_audio = false;
  bool second_has_cut_audio = false;
  for (size_t i = 0; i < first.frames.size(); ++i) {
    if (first.frames[i].track == 2 &&
        first.frames[i].timestamp_ns == static_cast<long long>(kChunkTarget)) {
      first_has_cut_audio = true;
    }
  }
  for (size_t i = 0; i < second.frames.size(); ++i) {
    if (second.frames[i].track == 2 &&
        second.frames[i].timestamp_ns == 0) {
      second_has_cut_audio = true;
    }
  }
  TASK_CHECK(!first_has_cut_audio);
  TASK_CHECK(second_has_cut_audio);

  // Audio strictly before the cut belongs to the old chunk.
  bool first_has_prior_audio = false;
  for (size_t i = 0; i < first.frames.size(); ++i) {
    if (first.frames[i].track == 2 &&
        first.frames[i].timestamp_ns ==
            static_cast<long long>(kChunkTarget - kAudioDuration)) {
      first_has_prior_audio = true;
    }
  }
  TASK_CHECK(first_has_prior_audio);

  // Every frame appears exactly once across the chunks, and each chunk's
  // SegmentInfo duration equals its own latest presentation end.
  uint32_t video_count = 0;
  uint32_t audio_count = 0;
  uint32_t expected_video = 0;
  uint32_t expected_audio = 0;
  for (size_t i = 0; i < frames.size(); ++i) {
    if (frames[i].track == 1)
      ++expected_video;
    else
      ++expected_audio;
  }
  for (uint32_t c = 0; c < chunk_count; ++c) {
    ParsedChunk chunk;
    TASK_CHECK(ParseChunk(ChunkPath(options.base_name, c), &chunk));
    long long chunk_end = 0;
    for (size_t i = 0; i < chunk.frames.size(); ++i) {
      if (chunk.frames[i].track == 1)
        ++video_count;
      else
        ++audio_count;
      const long long end = chunk.frames[i].timestamp_ns +
                            (chunk.frames[i].has_duration
                                 ? chunk.frames[i].duration_ns
                                 : 0);
      if (end > chunk_end)
        chunk_end = end;
    }
    TASK_CHECK(chunk.duration_ns == chunk_end);
  }
  TASK_CHECK(video_count == expected_video);
  TASK_CHECK(audio_count == expected_audio);

  // Audio track configuration is preserved in every chunk.
  for (uint32_t c = 0; c < chunk_count; ++c) {
    ParsedChunk chunk;
    TASK_CHECK(ParseChunk(ChunkPath(options.base_name, c), &chunk));
    const mkvparser::Track* const audio =
        chunk.segment->GetTracks()->GetTrackByNumber(2);
    TASK_CHECK(audio != NULL);
    TASK_CHECK(audio->GetType() == mkvparser::Track::kAudio);
    TASK_CHECK(strcmp(audio->GetCodecId(), "A_OPUS") == 0);
    TASK_CHECK(audio->GetUid() == 202);
    TASK_CHECK(audio->GetCodecDelay() == 6500000);
    TASK_CHECK(audio->GetSeekPreRoll() == 80000000);
    size_t private_size = 0;
    const unsigned char* const private_data = audio->GetCodecPrivate(private_size);
    TASK_CHECK(private_data != NULL);
    TASK_CHECK(private_size == sizeof(kOpusHead));
    TASK_CHECK(memcmp(private_data, kOpusHead, sizeof(kOpusHead)) == 0);

    const mkvparser::Track* const video =
        chunk.segment->GetTracks()->GetTrackByNumber(1);
    TASK_CHECK(video != NULL);
    TASK_CHECK(video->GetType() == mkvparser::Track::kVideo);
    TASK_CHECK(strcmp(video->GetCodecId(), "V_VP8") == 0);
    TASK_CHECK(video->GetUid() == 101);
    size_t video_private_size = 0;
    const unsigned char* const video_private =
        video->GetCodecPrivate(video_private_size);
    TASK_CHECK(video_private != NULL);
    TASK_CHECK(video_private_size == sizeof(kVideoCodecPrivate));
    TASK_CHECK(memcmp(video_private, kVideoCodecPrivate,
                      sizeof(kVideoCodecPrivate)) == 0);
  }
  return 0;
}

// Audio: audio must never trigger a boundary on its own.
int TestAudioDoesNotSplit() {
  TASK_CHECK(SetupTempDir() == 0);
  WriterOptions options;
  options.base_name = std::string(kTempDir) + "/audio_only_boundary";
  options.max_cluster_duration = 50000000;  // 50 ms target

  // Video key frames only at 0 and 200 ms, audio every 10 ms.
  std::vector<TestFrame> video;
  uint32_t serial = 1;
  video.push_back(MakeFrame(1, 0, 100000000, true, serial++));
  video.push_back(MakeFrame(1, 200000000, 100000000, true, serial++));
  const std::vector<TestFrame> frames = WithAudio(video, 10000000);
  TASK_CHECK(WriteChunks(options, frames));

  // Two chunks: the 200 ms key frame starts the second one.
  TASK_CHECK(CountChunks(options.base_name) == 2);
  ParsedChunk first;
  TASK_CHECK(ParseChunk(ChunkPath(options.base_name, 0), &first));
  for (size_t i = 0; i < first.frames.size(); ++i) {
    TASK_CHECK(first.frames[i].timestamp_ns < 200000000);
  }
  ParsedChunk second;
  TASK_CHECK(ParseChunk(ChunkPath(options.base_name, 1), &second));
  TASK_CHECK(!second.frames.empty());
  TASK_CHECK(second.frames.front().timestamp_ns == 0);
  // The cut key frame opens the second chunk; same-timestamp audio that was
  // submitted before it may precede it in submission order.
  bool second_has_key_video = false;
  for (size_t i = 0; i < second.frames.size(); ++i) {
    if (second.frames[i].track == 1 && second.frames[i].key &&
        second.frames[i].timestamp_ns == 0) {
      second_has_key_video = true;
    }
  }
  TASK_CHECK(second_has_key_video);
  return 0;
}

// Audio: DiscardPadding and explicit durations survive the split, and every
// timestamp/duration is a whole number of timecode units.
int TestAudioDiscardPaddingAndDurations() {
  TASK_CHECK(SetupTempDir() == 0);
  const uint64_t kTimecodeScale = 1000000;  // 1 ms
  const uint64_t kVideoDuration = 40000000;
  const uint64_t kAudioDuration = 20000000;
  WriterOptions options;
  options.base_name = std::string(kTempDir) + "/audio_padding";
  options.max_cluster_duration = 100000000;
  options.timecode_scale = kTimecodeScale;

  std::vector<TestFrame> frames;
  uint32_t serial = 1;
  // 6 video frames, key at 0 and 120 ms.
  for (int i = 0; i < 6; ++i) {
    const bool key = (i == 0 || i == 3);
    frames.push_back(MakeFrame(1, static_cast<uint64_t>(i) * kVideoDuration,
                               kVideoDuration, key, serial++));
  }
  // Audio every 20 ms on top of that, with DiscardPadding on the first and the
  // last audio frames.
  for (int i = 0; i < 13; ++i) {
    TestFrame audio =
        MakeFrame(2, static_cast<uint64_t>(i) * kAudioDuration, kAudioDuration,
                  true, serial++);
    if (i == 0)
      audio.discard_padding = -2000000;
    if (i == 12)
      audio.discard_padding = 3000000;
    frames.push_back(audio);
  }
  std::stable_sort(frames.begin(), frames.end(),
                   [](const TestFrame& a, const TestFrame& b) {
                     if (a.timestamp_ns != b.timestamp_ns)
                       return a.timestamp_ns < b.timestamp_ns;
                     return a.track > b.track;  // audio first at equal times
                   });
  TASK_CHECK(WriteChunks(options, frames));

  const uint32_t chunk_count = CountChunks(options.base_name);
  TASK_CHECK(chunk_count == 2);

  bool found_negative_padding = false;
  bool found_positive_padding = false;
  bool found_last_audio_padding = false;
  long long last_audio_timestamp = -1;
  const long long expected_negative_padding = -2000000;
  const long long expected_positive_padding = 3000000;
  for (uint32_t c = 0; c < chunk_count; ++c) {
    ParsedChunk chunk;
    TASK_CHECK(ParseChunk(ChunkPath(options.base_name, c), &chunk));
    TASK_CHECK(chunk.timecode_scale == static_cast<long long>(kTimecodeScale));
    for (size_t i = 0; i < chunk.frames.size(); ++i) {
      const ParsedFrame& frame = chunk.frames[i];
      TASK_CHECK((frame.timestamp_ns % kTimecodeScale) == 0);
      TASK_CHECK(frame.has_duration);
      TASK_CHECK((frame.duration_ns % kTimecodeScale) == 0);
      if (frame.track == 1)
        TASK_CHECK(frame.duration_ns == static_cast<long long>(kVideoDuration));
      else
        TASK_CHECK(frame.duration_ns == static_cast<long long>(kAudioDuration));
      if (frame.has_discard_padding) {
        if (frame.discard_padding < 0) {
          found_negative_padding = true;
          TASK_CHECK(frame.discard_padding == expected_negative_padding);
        }
        if (frame.discard_padding > 0) {
          found_positive_padding = true;
          TASK_CHECK(frame.discard_padding == expected_positive_padding);
        }
      }
      if (frame.track == 2 && frame.timestamp_ns > last_audio_timestamp) {
        last_audio_timestamp = frame.timestamp_ns;
        found_last_audio_padding = frame.has_discard_padding;
      }
    }
  }
  TASK_CHECK(found_negative_padding);
  TASK_CHECK(found_positive_padding);
  // The final audio packet keeps its DiscardPadding after the split.
  TASK_CHECK(found_last_audio_padding);
  TASK_CHECK(found_negative_padding);
  TASK_CHECK(found_positive_padding);
  return 0;
}

// Index: each chunk's Cues point at clusters using positions relative to that
// chunk's own Segment, and seeking through a cue lands on the right payload.
int TestIndexedSeekPerChunk() {
  TASK_CHECK(SetupTempDir() == 0);
  const uint64_t kFrameDuration = 10000000;
  const uint64_t kChunkTarget = 100000000;
  WriterOptions options;
  options.base_name = std::string(kTempDir) + "/index";
  options.max_cluster_duration = kChunkTarget;
  options.with_audio = false;

  const std::vector<TestFrame> frames = MakeVideoOnly(60, kFrameDuration, 5);
  TASK_CHECK(WriteChunks(options, frames));

  const uint32_t chunk_count = CountChunks(options.base_name);
  // Key frames every 50 ms; the cut happens at the first key frame at least
  // 100 ms past a chunk start, giving six chunks of 10 frames each.
  TASK_CHECK(chunk_count == 6);

  for (uint32_t c = 0; c < chunk_count; ++c) {
    ParsedChunk chunk;
    TASK_CHECK(ParseChunk(ChunkPath(options.base_name, c), &chunk));

    const mkvparser::Tracks* const tracks = chunk.segment->GetTracks();
    const mkvparser::Track* const video = tracks->GetTrackByNumber(1);
    TASK_CHECK(video != NULL);

    // Locate the Cues through this chunk's own SeekHead.
    const mkvparser::SeekHead* const seek_head = chunk.segment->GetSeekHead();
    long long cues_offset = -1;
    for (int i = 0; i < seek_head->GetCount(); ++i) {
      const mkvparser::SeekHead::Entry* const entry = seek_head->GetEntry(i);
      if (entry->id == libwebm::kMkvCues)
        cues_offset = entry->pos;
    }
    TASK_CHECK(cues_offset >= 0);

    long long cues_pos = 0;
    long cues_len = 0;
    TASK_CHECK(chunk.segment->ParseCues(cues_offset, cues_pos, cues_len) == 0);

    const mkvparser::Cues* const cues = chunk.segment->GetCues();
    TASK_CHECK(cues != NULL);
    while (cues->LoadCuePoint()) {
    }

    const mkvparser::CuePoint* cue = cues->GetFirst();
    TASK_CHECK(cue != NULL);
    uint32_t cue_count = 0;
    while (cue != NULL) {
      const mkvparser::CuePoint::TrackPosition* const position =
          cue->Find(video);
      TASK_CHECK(position != NULL);
      // Cue positions are relative to this chunk's Segment payload.
      TASK_CHECK(position->m_pos >= 0);
      TASK_CHECK(position->m_pos <
                 chunk.segment->m_start + chunk.segment->m_size);

      const mkvparser::BlockEntry* const entry = cues->GetBlock(cue, position);
      TASK_CHECK(entry != NULL);
      if (entry) {
        const mkvparser::Block* const block = entry->GetBlock();
        TASK_CHECK(block != NULL);
        TASK_CHECK(block->GetTrackNumber() == 1);
        TASK_CHECK(block->IsKey());
        TASK_CHECK(block->GetTime(entry->GetCluster()) == cue->GetTime(chunk.segment));

        // Read the compressed payload the cue points at and confirm it is
        // one of the video records written for this file. Payload serial
        // ranges are per chunk: 10 frames each, starting at 1.
        TASK_CHECK(block->GetFrameCount() == 1);
        const mkvparser::Block::Frame& block_frame = block->GetFrame(0);
        TASK_CHECK(block_frame.len == 4);
        uint8_t data[4] = {0, 0, 0, 0};
        TASK_CHECK(block_frame.Read(chunk.reader, data) == 0);
        TASK_CHECK(data[0] == 1);
        TASK_CHECK(data[3] == 0x55);
        const uint32_t serial =
            data[1] | (static_cast<uint32_t>(data[2]) << 8);
        const uint32_t frame_index = (serial - 1) % 10;
        TASK_CHECK(data[1] != 0 || data[2] != 0);
        // The frame index within the chunk matches the cue timestamp.
        TASK_CHECK(frame_index * kFrameDuration ==
                   static_cast<uint64_t>(block->GetTime(entry->GetCluster())));
      }
      ++cue_count;
      cue = cues->GetNext(cue);
    }
    // Key frames every 5 frames, 10 frames per chunk -> 2 cues per chunk.
    TASK_CHECK(cue_count == 2);
    // Seek using the cues API: the earliest key frame at time zero resolves.
    const mkvparser::CuePoint* found = NULL;
    const mkvparser::CuePoint::TrackPosition* found_position = NULL;
    TASK_CHECK(cues->Find(0, video, found, found_position));
    TASK_CHECK(found != NULL);
    TASK_CHECK(found->GetTime(chunk.segment) == 0);
    const mkvparser::BlockEntry* const seeked =
        cues->GetBlock(found, found_position);
    TASK_CHECK(seeked != NULL);
    if (seeked) {
      const mkvparser::Block* const block = seeked->GetBlock();
      TASK_CHECK(block->GetTime(seeked->GetCluster()) == 0);
      TASK_CHECK(block->IsKey());
      const mkvparser::Block::Frame& block_frame = block->GetFrame(0);
      TASK_CHECK(block_frame.len == 4);
    }
  }
  return 0;
}

// Index: cue positions must not be inherited from a previous output file.
int TestCuePositionsAreRelative() {
  TASK_CHECK(SetupTempDir() == 0);
  WriterOptions options;
  options.base_name = std::string(kTempDir) + "/cue_rel";
  options.max_cluster_duration = 100000000;
  options.with_audio = false;

  const std::vector<TestFrame> frames = MakeVideoOnly(40, 10000000, 10);
  TASK_CHECK(WriteChunks(options, frames));
  const uint32_t chunk_count = CountChunks(options.base_name);
  TASK_CHECK(chunk_count == 4);

  long long previous_first_position = -1;
  for (uint32_t c = 0; c < chunk_count; ++c) {
    ParsedChunk chunk;
    TASK_CHECK(ParseChunk(ChunkPath(options.base_name, c), &chunk));

    const mkvparser::Cues* cues = chunk.segment->GetCues();
    TASK_CHECK(cues != NULL);
    while (cues->LoadCuePoint()) {
    }
    const mkvparser::CuePoint* const cue = cues->GetFirst();
    TASK_CHECK(cue != NULL);
    const mkvparser::Track* const video =
        chunk.segment->GetTracks()->GetTrackByNumber(1);
    const mkvparser::CuePoint::TrackPosition* const position = cue->Find(video);
    TASK_CHECK(position != NULL);

    // The first cluster of each file sits immediately after that file's
    // header, so the payload offset is small and independent of prior chunks.
    TASK_CHECK(position->m_pos >= 0);
    TASK_CHECK(position->m_pos < 1024);
    if (previous_first_position >= 0) {
      // Repeated files have identical layout; positions must not accumulate.
      TASK_CHECK(position->m_pos == previous_first_position);
    }
    previous_first_position = position->m_pos;
  }
  return 0;
}

// Compatibility: ordinary single-file output keeps its behavior.
int TestOrdinarySegmentUnchanged() {
  TASK_CHECK(SetupTempDir() == 0);
  const std::string path = std::string(kTempDir) + "/ordinary.webm";
  remove(path.c_str());

  mkvmuxer::MkvWriter writer;
  TASK_CHECK(writer.Open(path.c_str()));
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&writer));
  TASK_CHECK(!segment.standalone_chunking());
  TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
  segment.GetTrackByNumber(1)->set_uid(101);
  segment.GetTrackByNumber(1)->set_codec_id("V_VP8");
  segment.set_max_cluster_duration(40000000);

  const std::vector<TestFrame> frames = MakeVideoOnly(11, 20000000, 3);
  for (size_t i = 0; i < frames.size(); ++i)
    TASK_CHECK(AddTestFrame(&segment, frames[i]));
  TASK_CHECK(segment.Finalize());
  writer.Close();

  ParsedChunk chunk;
  TASK_CHECK(ParseChunk(path, &chunk));
  TASK_CHECK(chunk.frames.size() == 11);
  TASK_CHECK(chunk.duration_ns == 11 * 20000000);
  // Single-file output has no chunk files next to it.
  TASK_CHECK(CountChunks(path) == 0);
  return 0;
}

// Compatibility: the shared-header chunking path is untouched.
int TestSharedHeaderChunkingUnchanged() {
  TASK_CHECK(SetupTempDir() == 0);
  const std::string base = std::string(kTempDir) + "/shared";
  // Clean leftovers from earlier runs.
  remove((base + ".hdr").c_str());
  for (uint32_t i = 0; i < 64; ++i) {
    char suffix[32];
    snprintf(suffix, sizeof(suffix), "_%06u.chk", i);
    remove((base + suffix).c_str());
    snprintf(suffix, sizeof(suffix), "_%06u.cues", i);
    remove((base + suffix).c_str());
  }

  mkvmuxer::MkvWriter writer;
  TASK_CHECK(writer.Open((std::string(kTempDir) + "/unused.webm").c_str()));
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&writer));
  TASK_CHECK(segment.SetChunking(true, base.c_str()));
  TASK_CHECK(segment.chunking());
  TASK_CHECK(!segment.standalone_chunking());
  TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
  segment.set_max_cluster_duration(40000000);

  const std::vector<TestFrame> frames = MakeVideoOnly(11, 20000000, 3);
  for (size_t i = 0; i < frames.size(); ++i)
    TASK_CHECK(AddTestFrame(&segment, frames[i]));
  TASK_CHECK(segment.Finalize());
  writer.Close();

  TASK_CHECK(FileExists(base + ".hdr"));
  uint32_t chk_count = 0;
  while (FileExists(base + "_" + [](uint32_t index) {
    char suffix[16];
    snprintf(suffix, sizeof(suffix), "%06u.chk", index);
    return std::string(suffix);
  }(chk_count))) {
    ++chk_count;
  }
  TASK_CHECK(chk_count >= 2);

  // A shared-header chunk on its own is not a complete WebM file.
  ParsedChunk parsed;
  TASK_CHECK(!ParseChunk(base + "_000000.chk", &parsed));
  return 0;
}

// Compatibility: standalone setup is rejected after the first frame.
int TestStandaloneSetupOrdering() {
  TASK_CHECK(SetupTempDir() == 0);
  WriterOptions options;
  options.base_name = std::string(kTempDir) + "/ordering";
  options.max_cluster_duration = 100000000;
  options.with_audio = false;

  RemoveChunksWithPrefix(options.base_name);
  mkvmuxer::MkvWriter writer;
  TASK_CHECK(writer.Open(options.base_name.c_str()));
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&writer));
  TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
  segment.GetTrackByNumber(1)->set_uid(101);

  const std::vector<TestFrame> frames = MakeVideoOnly(4, 10000000, 2);
  TASK_CHECK(AddTestFrame(&segment, frames[0]));
  // Enabling after frames were added must fail.
  TASK_CHECK(!segment.SetStandaloneChunking(options.base_name.c_str()));
  TASK_CHECK(!segment.standalone_chunking());
  writer.Close();

  // Enabling before Init must fail too.
  mkvmuxer::Segment late_init;
  TASK_CHECK(!late_init.SetStandaloneChunking(options.base_name.c_str()));

  // A fresh segment accepts it.
  mkvmuxer::MkvWriter writer2;
  mkvmuxer::Segment segment2;
  TASK_CHECK(writer2.Open((std::string(kTempDir) + "/ordering2.webm").c_str()));
  TASK_CHECK(segment2.Init(&writer2));
  TASK_CHECK(segment2.SetStandaloneChunking(options.base_name.c_str()));
  TASK_CHECK(segment2.standalone_chunking());
  writer2.Close();
  return 0;
}

// Compatibility: VP9 video is supported on the standalone path.
int TestVp9Standalone() {
  TASK_CHECK(SetupTempDir() == 0);
  WriterOptions options;
  options.base_name = std::string(kTempDir) + "/vp9";
  options.max_cluster_duration = 100000000;
  options.use_vp9 = true;
  options.with_audio = false;

  const std::vector<TestFrame> frames = MakeVideoOnly(20, 10000000, 10);
  TASK_CHECK(WriteChunks(options, frames));
  TASK_CHECK(CountChunks(options.base_name) == 2);

  ParsedChunk chunk;
  TASK_CHECK(ParseChunk(ChunkPath(options.base_name, 0), &chunk));
  const mkvparser::Track* const video =
      chunk.segment->GetTracks()->GetTrackByNumber(1);
  TASK_CHECK(video != NULL);
  TASK_CHECK(strcmp(video->GetCodecId(), "V_VP9") == 0);
  TASK_CHECK(video->GetUid() == 101);
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Case registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    {"Split", "BasicSplit", TestBasicSplit},
    {"Split", "LongGopDelaysCut", TestLongGopDelaysCut},
    {"Split", "ShortRecordingSingleChunk", TestShortRecordingSingleChunk},
    {"Split", "EmptyInputNoOutput", TestEmptyInputNoOutput},
    {"Audio", "AudioAtBoundary", TestAudioAtBoundary},
    {"Audio", "AudioDoesNotSplit", TestAudioDoesNotSplit},
    {"Audio", "DiscardPaddingAndDurations", TestAudioDiscardPaddingAndDurations},
    {"Index", "IndexedSeekPerChunk", TestIndexedSeekPerChunk},
    {"Index", "CuePositionsAreRelative", TestCuePositionsAreRelative},
    {"Compatibility", "OrdinarySegmentUnchanged", TestOrdinarySegmentUnchanged},
    {"Compatibility", "SharedHeaderChunkingUnchanged",
     TestSharedHeaderChunkingUnchanged},
    {"Compatibility", "StandaloneSetupOrdering", TestStandaloneSetupOrdering},
    {"Compatibility", "Vp9Standalone", TestVp9Standalone},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
