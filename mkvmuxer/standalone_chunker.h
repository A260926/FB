// Copyright (c) 2012 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.

#ifndef MKVMUXER_STANDALONE_CHUNKER_H_
#define MKVMUXER_STANDALONE_CHUNKER_H_

#include <stdint.h>

#include <string>
#include <vector>

namespace mkvmuxer {

class Frame;
class Segment;

// Splits the output of a Segment into independently playable WebM files.
//
// Every output file contains its own EBML header, SegmentInfo, Tracks,
// Clusters, SeekHead and Cues elements, so a player can open any single file
// and seek to that file's key frames through its own cues.
//
// Chunk boundaries are only created at video key frames whose timestamp is at
// least max_cluster_duration past the start of the current chunk. Frames
// belonging to the current chunk are held until the chunk is known to be
// complete (a later key frame crosses the target duration or Finalize is
// called). At a boundary, frames with a timestamp earlier than the key frame
// stay in the old chunk, while frames with a timestamp equal to or later than
// the key frame are moved to the new chunk. This is what places audio
// submitted before a same-timestamp key frame into the new chunk.
class StandaloneChunker {
 public:
  StandaloneChunker();
  ~StandaloneChunker();

  // |segment| is used as the source of track and SegmentInfo configuration and
  // must outlive this object. Must be called after Segment::Init() and before
  // the first frame is added to |segment|.
  bool Init(Segment* segment, const char* base_name);

  // Routes |frame| into the current chunk, writing and starting a new chunk
  // when |frame| is a video key frame that reaches the target chunk duration.
  bool AddFrame(const Frame* frame);

  // Writes the chunk holding any frames that have not been flushed yet. No
  // output file is created when no frame was ever added.
  bool Finalize();

  // True once Finalize() has run.
  bool finalized() const { return finalized_; }

  // True once any frame has been added.
  bool started() const { return tracks_snapshotted_; }

  // Base filename these chunks are written to.
  const std::string& base_name() const { return base_name_; }

 private:
  // Track configuration shared by every output file.
  struct TrackConfig {
    TrackConfig();
    ~TrackConfig();

    uint64_t number;
    uint64_t type;
    uint64_t uid;
    std::string codec_id;
    std::vector<uint8_t> codec_private;
    std::string language;
    std::string name;
    uint64_t codec_delay;
    uint64_t seek_pre_roll;
    uint64_t default_duration;
    uint64_t max_block_additional_id;

    // Video specific.
    uint64_t width;
    uint64_t height;
    uint64_t display_width;
    uint64_t display_height;
    uint64_t pixel_width;
    uint64_t pixel_height;
    uint64_t stereo_mode;
    uint64_t alpha_mode;

    // Audio specific.
    double sample_rate;
    uint64_t channels;
    uint64_t bit_depth;
  };

  bool SnapshotTracks();
  bool WriteChunk(std::vector<Frame*>* frames, uint64_t start_ns);
  bool WriteTrackConfig(Segment* segment, const TrackConfig& config) const;
  void DeleteFrames(std::vector<Frame*>* frames);

  Segment* segment_;
  std::string base_name_;
  uint32_t chunk_index_;
  bool tracks_snapshotted_;
  std::vector<TrackConfig*> tracks_;

  // True once at least one video key frame established the start time of the
  // chunk currently being accumulated.
  bool chunk_open_;
  uint64_t chunk_start_ns_;

  // Frames assigned to the chunk currently being accumulated.
  std::vector<Frame*> chunk_frames_;

  uint64_t last_timestamp_ns_;
  bool has_last_timestamp_;

  bool finalized_;

  StandaloneChunker(const StandaloneChunker&);
  StandaloneChunker& operator=(const StandaloneChunker&);
};

}  // namespace mkvmuxer

#endif  // MKVMUXER_STANDALONE_CHUNKER_H_
