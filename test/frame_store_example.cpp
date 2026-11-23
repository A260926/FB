// Copyright © 2026 Steve Lhomme.
// SPDX-License-Identifier: LGPL-2.1-or-later

/*!
  \file Runnable example for KaxFrameStore.

  Builds a small Matroska file with one track and one cluster, moves every
  compressed frame payload into an in-memory KaxFrameStore once the batch is
  complete, then renders the cluster straight from the store. The resulting
  file .task-build/frame-store-example.mkv is a non-empty, re-readable
  Matroska file and is verified by task_support/check_example.cpp.
*/
#include "matroska/KaxBlock.h"
#include "matroska/KaxCluster.h"
#include "matroska/KaxCues.h"
#include "matroska/KaxFrameStore.h"
#include "matroska/KaxSegment.h"
#include "matroska/KaxSemantic.h"
#include "matroska/KaxTracks.h"

#include <ebml/EbmlHead.h>
#include <ebml/MemIOCallback.h>
#include <ebml/StdIOCallback.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace libebml;
using namespace libmatroska;

namespace {

constexpr std::uint64_t TIMESTAMP_SCALE = 1000000;
constexpr const char * OUTPUT_PATH = ".task-build/frame-store-example.mkv";

std::vector<binary> deterministicFrame(std::uint32_t size, std::uint8_t seed)
{
  std::vector<binary> bytes(size);
  for (std::uint32_t i = 0; i < size; ++i)
    bytes[i] = static_cast<binary>((static_cast<unsigned>(seed) * 31u + i * 17u + 3u) & 0xFFu);
  return bytes;
}

} // namespace

int main()
{
  try {
    // EBML document head
    EbmlHead head;
    GetChild<EDocType>(head).SetValue("matroska");
    GetChild<EDocTypeVersion>(head).SetValue(2);
    GetChild<EDocTypeReadVersion>(head).SetValue(1);

    // one laced audio track
    KaxSegment segment;
    KaxTracks & tracks = GetChild<KaxTracks>(segment);
    KaxTrackEntry & track = GetChild<KaxTrackEntry>(tracks);
    track.SetGlobalTimestampScale(TIMESTAMP_SCALE);
    GetChild<KaxTrackNumber>(track).SetValue(1);
    GetChild<KaxTrackUID>(track).SetValue(7);
    GetChild<KaxTrackType>(track).SetValue(track_audio);
    GetChild<KaxCodecID>(track).SetValue("A_OPUS");
    track.EnableLacing(true);

    // render the tracks element up front so the segment size can be finite
    MemIOCallback tracksOutput;
    tracks.Render(tracksOutput, EbmlElement::WriteSkipDefault);
    const std::vector<binary> tracksBytes(tracksOutput.GetDataBuffer(),
                                          tracksOutput.GetDataBuffer() + tracksOutput.getFilePointer());

    // accumulate a batch of compressed frames
    KaxCluster cluster;
    cluster.SetParent(segment);
    cluster.SetPreviousTimestamp(0, TIMESTAMP_SCALE);
    GetChild<KaxClusterTimestamp>(cluster).SetValue(0);

    const std::uint32_t frameSizes[] = {1200, 305, 17, 4096, 256};
    std::vector<std::vector<binary>> payloads;
    KaxBlockGroup * block = nullptr;
    for (std::uint32_t i = 0; i < 5; ++i) {
      payloads.push_back(deterministicFrame(frameSizes[i], static_cast<std::uint8_t>(i + 1)));
      // the DataBuffer copies the bytes and frees them through this owned buffer
      auto * buffer = new DataBuffer(payloads.back().data(), frameSizes[i], nullptr, true);
      cluster.AddFrame(track, static_cast<std::uint64_t>(i + 1) * TIMESTAMP_SCALE,
                       *buffer, block, LACING_XIPH);
    }

    // the batch is complete: move the payloads out of the DataBuffer objects
    auto backingStream = std::make_shared<MemIOCallback>();
    auto frameStore = std::make_shared<KaxFrameStore>(backingStream);
    if (!cluster.SpoolFrames(frameStore)) {
      std::fprintf(stderr, "failed to spool frames\n");
      return 1;
    }
    std::printf("spooled %llu payload bytes into the frame store\n",
                static_cast<unsigned long long>(frameStore->SpooledBytes()));

    // render straight from the backing store, no media decoding involved
    MemIOCallback clusterOutput;
    KaxCues cues;
    cues.SetGlobalTimestampScale(TIMESTAMP_SCALE);
    const auto clusterSize = cluster.Render(clusterOutput, cues);
    const std::vector<binary> clusterBytes(clusterOutput.GetDataBuffer(),
                                           clusterOutput.GetDataBuffer() + clusterOutput.getFilePointer());

    // assemble a re-readable Matroska file with a finite, minimal segment size
    StdIOCallback output(OUTPUT_PATH, MODE_CREATE);
    head.Render(output, EbmlElement::WriteSkipDefault);
    {
      binary idBytes[4];
      EbmlId(0x18538067).Fill(idBytes); // KaxSegment element id
      output.writeFully(idBytes, EbmlId(0x18538067).GetLength());

      const auto payloadSize = tracksBytes.size() + clusterBytes.size();
      const auto sizeLength = CodedSizeLength(payloadSize, 0, true);
      binary sizeBytes[8];
      CodedValueLength(payloadSize, static_cast<int>(sizeLength), sizeBytes);
      output.writeFully(sizeBytes, sizeLength);
    }
    output.writeFully(tracksBytes.data(), tracksBytes.size());
    output.writeFully(clusterBytes.data(), clusterBytes.size());
    output.close();

    std::printf("wrote %s (%llu cluster bytes)\n", OUTPUT_PATH,
                static_cast<unsigned long long>(clusterSize));
    return 0;
  } catch (const std::exception & error) {
    std::fprintf(stderr, "example failed: %s\n", error.what());
    return 2;
  }
}
