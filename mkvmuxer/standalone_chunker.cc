// Copyright (c) 2012 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.

#include "mkvmuxer/standalone_chunker.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <new>

#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"

namespace mkvmuxer {

namespace {

std::string ChunkFilename(const std::string& base_name, uint32_t index) {
  char ext[32];
  snprintf(ext, sizeof(ext), "_%06u.webm", index);
  return base_name + ext;
}

bool IsVideoCodecSupported(const char* codec_id) {
  if (!codec_id)
    return false;
  return !strcmp(codec_id, Tracks::kVp8CodecId) ||
         !strcmp(codec_id, Tracks::kVp9CodecId);
}

bool IsAudioCodecSupported(const char* codec_id) {
  return codec_id && !strcmp(codec_id, Tracks::kOpusCodecId);
}

}  // namespace

StandaloneChunker::TrackConfig::TrackConfig()
    : number(0),
      type(0),
      uid(0),
      codec_delay(0),
      seek_pre_roll(0),
      default_duration(0),
      max_block_additional_id(0),
      width(0),
      height(0),
      display_width(0),
      display_height(0),
      pixel_width(0),
      pixel_height(0),
      stereo_mode(0),
      alpha_mode(0),
      sample_rate(0.0),
      channels(0),
      bit_depth(0) {}

StandaloneChunker::TrackConfig::~TrackConfig() {}

StandaloneChunker::StandaloneChunker()
    : segment_(NULL),
      chunk_index_(0),
      tracks_snapshotted_(false),
      chunk_open_(false),
      chunk_start_ns_(0),
      last_timestamp_ns_(0),
      has_last_timestamp_(false),
      finalized_(false) {}

StandaloneChunker::~StandaloneChunker() {
  DeleteFrames(&chunk_frames_);
  for (size_t i = 0; i < tracks_.size(); ++i)
    delete tracks_[i];
  tracks_.clear();
}

bool StandaloneChunker::Init(Segment* segment, const char* base_name) {
  if (!segment || !base_name || !*base_name)
    return false;
  segment_ = segment;
  base_name_ = base_name;
  return true;
}

bool StandaloneChunker::SnapshotTracks() {
  if (!segment_)
    return false;

  tracks_.clear();

  int video_track_count = 0;
  int audio_track_count = 0;

  for (uint64_t number = 1; number <= kMaxTrackNumber; ++number) {
    const Track* const track = segment_->GetTrackByNumber(number);
    if (!track)
      continue;

    TrackConfig* config = new (std::nothrow) TrackConfig();
    if (!config)
      return false;

    config->number = number;
    config->type = track->type();
    config->uid = track->uid();
    if (track->codec_id())
      config->codec_id = track->codec_id();
    if (track->codec_private() && track->codec_private_length() > 0) {
      config->codec_private.assign(
          track->codec_private(),
          track->codec_private() + track->codec_private_length());
    }
    if (track->language())
      config->language = track->language();
    if (track->name())
      config->name = track->name();
    config->codec_delay = track->codec_delay();
    config->seek_pre_roll = track->seek_pre_roll();
    config->default_duration = track->default_duration();
    config->max_block_additional_id = track->max_block_additional_id();

    if (config->type == Tracks::kVideo) {
      ++video_track_count;
      if (video_track_count > 1 || !IsVideoCodecSupported(track->codec_id())) {
        delete config;
        return false;
      }
      const VideoTrack* const video_track =
          static_cast<const VideoTrack*>(track);
      config->width = video_track->width();
      config->height = video_track->height();
      config->display_width = video_track->display_width();
      config->display_height = video_track->display_height();
      config->pixel_width = video_track->pixel_width();
      config->pixel_height = video_track->pixel_height();
      config->stereo_mode = video_track->stereo_mode();
      config->alpha_mode = video_track->alpha_mode();
    } else if (config->type == Tracks::kAudio) {
      ++audio_track_count;
      if (audio_track_count > 1 || !IsAudioCodecSupported(track->codec_id())) {
        delete config;
        return false;
      }
      const AudioTrack* const audio_track =
          static_cast<const AudioTrack*>(track);
      config->sample_rate = audio_track->sample_rate();
      config->channels = audio_track->channels();
      config->bit_depth = audio_track->bit_depth();
    } else {
      // Only one VP8/VP9 video track plus an optional Opus audio track are
      // supported on the standalone chunking path.
      delete config;
      return false;
    }

    tracks_.push_back(config);
  }

  if (video_track_count != 1)
    return false;

  tracks_snapshotted_ = true;
  return true;
}

bool StandaloneChunker::WriteTrackConfig(Segment* segment,
                                         const TrackConfig& config) const {
  if (!segment)
    return false;

  if (config.type == Tracks::kVideo) {
    const uint64_t number = segment->AddVideoTrack(
        static_cast<int32_t>(config.width), static_cast<int32_t>(config.height),
        static_cast<int32_t>(config.number));
    if (number != config.number)
      return false;
    VideoTrack* const video_track =
        static_cast<VideoTrack*>(segment->GetTrackByNumber(number));
    video_track->set_width(config.width);
    video_track->set_height(config.height);
    video_track->set_display_width(config.display_width);
    video_track->set_display_height(config.display_height);
    video_track->set_pixel_width(config.pixel_width);
    video_track->set_pixel_height(config.pixel_height);
    if (config.stereo_mode > 0) {
      if (!video_track->SetStereoMode(config.stereo_mode))
        return false;
    }
    if (config.alpha_mode > 0) {
      if (!video_track->SetAlphaMode(config.alpha_mode))
        return false;
    }
  } else if (config.type == Tracks::kAudio) {
    const uint64_t number = segment->AddAudioTrack(
        static_cast<int32_t>(config.sample_rate),
        static_cast<int32_t>(config.channels),
        static_cast<int32_t>(config.number));
    if (number != config.number)
      return false;
    AudioTrack* const audio_track =
        static_cast<AudioTrack*>(segment->GetTrackByNumber(number));
    audio_track->set_sample_rate(config.sample_rate);
    audio_track->set_channels(config.channels);
    audio_track->set_bit_depth(config.bit_depth);
  } else {
    return false;
  }

  Track* const track = segment->GetTrackByNumber(config.number);
  if (!track)
    return false;

  track->set_uid(config.uid);
  if (!config.codec_id.empty())
    track->set_codec_id(config.codec_id.c_str());
  if (!config.codec_private.empty()) {
    if (!track->SetCodecPrivate(config.codec_private.data(),
                                config.codec_private.size()))
      return false;
  }
  if (!config.language.empty())
    track->set_language(config.language.c_str());
  if (!config.name.empty())
    track->set_name(config.name.c_str());
  track->set_codec_delay(config.codec_delay);
  track->set_seek_pre_roll(config.seek_pre_roll);
  track->set_default_duration(config.default_duration);
  if (config.max_block_additional_id > 0) {
    track->set_max_block_additional_id(config.max_block_additional_id);
  }

  return true;
}

void StandaloneChunker::DeleteFrames(std::vector<Frame*>* frames) {
  if (!frames)
    return;
  for (size_t i = 0; i < frames->size(); ++i)
    delete (*frames)[i];
  frames->clear();
}

bool StandaloneChunker::WriteChunk(std::vector<Frame*>* frames,
                                   uint64_t start_ns) {
  if (!frames || frames->empty())
    return true;

  const std::string filename = ChunkFilename(base_name_, chunk_index_);

  Segment chunk;
  MkvWriter writer;
  if (!writer.Open(filename.c_str()))
    return false;
  if (!chunk.Init(&writer)) {
    writer.Close();
    return false;
  }

  const SegmentInfo* const source_info = segment_->GetSegmentInfo();
  SegmentInfo* const chunk_info = chunk.GetSegmentInfo();
  chunk_info->set_timecode_scale(source_info->timecode_scale());
  if (source_info->muxing_app())
    chunk_info->set_muxing_app(source_info->muxing_app());
  if (source_info->writing_app())
    chunk_info->set_writing_app(source_info->writing_app());

  // Every file must carry its own Cues element.
  chunk.OutputCues(true);
  chunk.set_max_cluster_duration(segment_->max_cluster_duration());

  for (size_t i = 0; i < tracks_.size(); ++i) {
    if (!WriteTrackConfig(&chunk, *tracks_[i])) {
      writer.Close();
      return false;
    }
  }

  const uint64_t timecode_scale = source_info->timecode_scale();

  // Segment duration is the latest presentation end of any frame in this
  // chunk, expressed in timecode units.
  uint64_t max_end_ns = 0;
  for (size_t i = 0; i < frames->size(); ++i) {
    const Frame* const frame = (*frames)[i];
    const uint64_t end_ns = frame->timestamp() + frame->duration();
    if (end_ns > max_end_ns)
      max_end_ns = end_ns;
  }

  if (max_end_ns > start_ns) {
    chunk.set_duration(static_cast<double>(max_end_ns - start_ns) /
                       static_cast<double>(timecode_scale));
  }

  // Rewrite timestamps relative to the start of this chunk. Compressed bytes,
  // the key flag, explicit durations and DiscardPadding are left untouched.
  for (size_t i = 0; i < frames->size(); ++i) {
    Frame* const frame = (*frames)[i];
    if (frame->timestamp() < start_ns) {
      writer.Close();
      return false;
    }
    frame->set_timestamp(frame->timestamp() - start_ns);
    if (frame->reference_block_timestamp_set()) {
      if (frame->reference_block_timestamp() >=
          static_cast<int64_t>(start_ns)) {
        frame->set_reference_block_timestamp(
            frame->reference_block_timestamp() -
            static_cast<int64_t>(start_ns));
      } else {
        frame->set_reference_block_timestamp(0);
      }
    }
    if (!chunk.AddGenericFrame(frame)) {
      writer.Close();
      return false;
    }
  }

  const bool finalized = chunk.Finalize();
  writer.Close();
  if (!finalized)
    return false;

  ++chunk_index_;
  return true;
}

bool StandaloneChunker::AddFrame(const Frame* frame) {
  if (!frame || !segment_ || finalized_)
    return false;

  if (!tracks_snapshotted_) {
    if (!SnapshotTracks())
      return false;
  }

  const Track* const track = segment_->GetTrackByNumber(frame->track_number());
  if (!track)
    return false;

  const bool is_video = track->type() == Tracks::kVideo;
  if (!is_video && track->type() != Tracks::kAudio)
    return false;

  if (has_last_timestamp_ && frame->timestamp() < last_timestamp_ns_)
    return false;

  Frame* const copy = new (std::nothrow) Frame();
  if (!copy || !copy->CopyFrom(*frame)) {
    delete copy;
    return false;
  }

  chunk_frames_.push_back(copy);
  last_timestamp_ns_ = frame->timestamp();
  has_last_timestamp_ = true;

  if (!chunk_open_) {
    // The first video frame establishes where the first chunk begins.
    if (is_video) {
      chunk_start_ns_ = frame->timestamp();
      chunk_open_ = true;
    }
    return true;
  }

  const uint64_t max_cluster_duration = segment_->max_cluster_duration();
  if (!is_video || !frame->is_key() || frame->timestamp() < chunk_start_ns_)
    return true;
  if (max_cluster_duration == 0)
    return true;
  if (frame->timestamp() - chunk_start_ns_ < max_cluster_duration)
    return true;

  // Frames at or after the cut timestamp belong to the new chunk, including
  // audio submitted before a video key frame sharing the same timestamp.
  const uint64_t cut_ns = frame->timestamp();
  std::vector<Frame*> old_chunk;
  std::vector<Frame*> new_chunk;
  old_chunk.reserve(chunk_frames_.size());
  new_chunk.reserve(chunk_frames_.size());
  for (size_t i = 0; i < chunk_frames_.size(); ++i) {
    Frame* const queued = chunk_frames_[i];
    if (queued->timestamp() < cut_ns)
      old_chunk.push_back(queued);
    else
      new_chunk.push_back(queued);
  }
  chunk_frames_.clear();

  if (!WriteChunk(&old_chunk, chunk_start_ns_)) {
    DeleteFrames(&old_chunk);
    DeleteFrames(&new_chunk);
    return false;
  }
  DeleteFrames(&old_chunk);

  chunk_frames_ = new_chunk;
  chunk_start_ns_ = cut_ns;
  return true;
}

bool StandaloneChunker::Finalize() {
  if (finalized_)
    return true;
  finalized_ = true;

  if (chunk_frames_.empty()) {
    // Empty input must not leave an empty output file behind.
    return true;
  }

  const bool result = WriteChunk(&chunk_frames_, chunk_start_ns_);
  DeleteFrames(&chunk_frames_);
  return result;
}

}  // namespace mkvmuxer
