// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.

#include <cstdio>
#include <cstring>
#include <vector>

#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"

namespace {

const uint64_t kMilliseconds = 1000000ULL;

std::vector<unsigned char> MakePacket(int tag, size_t length) {
  std::vector<unsigned char> packet(length);
  for (size_t i = 0; i < length; ++i)
    packet[i] = static_cast<unsigned char>(
        (tag * 37 + static_cast<int>(i)) & 0xff);
  return packet;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: %s <output.webm>\n", argv[0]);
    return 1;
  }

  mkvmuxer::MkvWriter writer;
  if (!writer.Open(argv[1])) {
    std::fprintf(stderr, "Failed to open %s\n", argv[1]);
    return 1;
  }

  mkvmuxer::Segment segment;
  if (!segment.Init(&writer)) {
    std::fprintf(stderr, "Failed to initialize the muxer\n");
    return 1;
  }

  // The audio track must exist before SetAudioLacing() is called.
  const uint64_t audio_track = segment.AddAudioTrack(48000, 2, 1);
  if (audio_track == 0) {
    std::fprintf(stderr, "Failed to add the audio track\n");
    return 1;
  }

  mkvmuxer::Track* const track = segment.GetTrackByNumber(audio_track);
  // Lacing requires a positive DefaultDuration that is a multiple of the
  // Segment timecode scale. The default scale is 1 ms.
  track->set_default_duration(20 * kMilliseconds);
  track->set_uid(audio_track);

  // Keep the legacy one-frame-per-block behavior by passing 1, or enable
  // Xiph lacing with any value in [2, 256]. This must happen after the track
  // is added and before the first frame (which writes the segment header).
  // Lacing is supported in kFile mode with a seekable writer and cannot be
  // combined with chunking, kLive mode, or AccurateClusterDuration.
  if (!segment.SetAudioLacing(audio_track, 2)) {
    std::fprintf(stderr, "Failed to enable audio lacing\n");
    return 1;
  }

  // Adjacent frames on the same track whose timestamps differ by exactly one
  // DefaultDuration are written as a single Xiph-laced block. The payload is
  // copied by the muxer, so the caller may release each buffer immediately.
  const size_t kPacketSizes[] = {1, 254, 255, 256, 510};
  for (size_t i = 0; i < sizeof(kPacketSizes) / sizeof(kPacketSizes[0]); ++i) {
    const std::vector<unsigned char> packet =
        MakePacket(static_cast<int>(i), kPacketSizes[i]);
    if (!segment.AddFrame(packet.data(), packet.size(), audio_track,
                          i * 20 * kMilliseconds, true)) {
      std::fprintf(stderr, "Failed to add audio frame %zu\n", i);
      return 1;
    }
  }

  if (!segment.Finalize()) {
    std::fprintf(stderr, "Failed to finalize the segment\n");
    return 1;
  }

  writer.Close();
  std::fprintf(stderr, "Wrote %s with Xiph-laced audio frames\n", argv[1]);
  return 0;
}
