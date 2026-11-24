/*
 * adts_seal_example.cpp - Encode AAC-LC and seal an incomplete ADTS group.
 *
 * The example uses AACENC_TPSUBFRAMES=4, so the ordinary transport path emits
 * one ADTS frame after every four AUs.  After encoding fourteen AUs there is
 * an incomplete group of two AUs; aacEncSealAdts() emits it immediately
 * without padding PCM or closing the encoder.  Four more AUs are then encoded
 * to demonstrate that aggregation restarts at the normal N-block boundary.
 */

#include "aacenc_lib.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kChannels = 2;
constexpr int kSampleRate = 48000;
constexpr int kBitrate = 128000;
constexpr int kSubFrames = 4;
constexpr int kInitialAUs = 14;
constexpr int kContinuationAUs = 4;

static std::vector<INT_PCM> makePcm(int numAUs) {
  std::vector<INT_PCM> pcm(static_cast<size_t>(numAUs) * 1024 * kChannels);
  uint32_t rng = 0x2468ace0u;
  for (int au = 0; au < numAUs; ++au) {
    for (int i = 0; i < 1024; ++i) {
      rng = rng * 1664525u + 1013904223u;
      int tone = ((i + au * 61) % 173) - 86;
      int value = tone * 110 + static_cast<int>(rng >> 27) - 8;
      if (value > 15000) value = 15000;
      if (value < -15000) value = -15000;
      size_t base = static_cast<size_t>(au) * 2048 + 2ULL * i;
      pcm[base] = static_cast<INT_PCM>(value);
      pcm[base + 1] = static_cast<INT_PCM>(-value * 8 / 13);
    }
  }
  return pcm;
}

static int encodeOneAU(HANDLE_AACENCODER encoder, const INT_PCM *frame,
                       std::vector<UCHAR> &stream, int &transportFrames) {
  UCHAR outBuffer[8192];
  void *inBuffer = const_cast<INT_PCM *>(frame);
  void *outPtr = outBuffer;
  INT inId = IN_AUDIO_DATA;
  INT outId = OUT_BITSTREAM_DATA;
  INT inSize = 1024 * kChannels * static_cast<INT>(sizeof(INT_PCM));
  INT outSize = static_cast<INT>(sizeof(outBuffer));
  INT inElementSize = sizeof(INT_PCM);
  INT outElementSize = 1;

  AACENC_BufDesc inDesc = {1, &inBuffer, &inId, &inSize, &inElementSize};
  AACENC_BufDesc outDesc = {1, &outPtr, &outId, &outSize, &outElementSize};
  AACENC_InArgs inArgs = {};
  AACENC_OutArgs outArgs = {};
  inArgs.numInSamples = 1024 * kChannels;

  AACENC_ERROR err = aacEncEncode(encoder, &inDesc, &outDesc, &inArgs,
                                   &outArgs);
  if (err != AACENC_OK || outArgs.numInSamples != 1024 * kChannels) {
    std::fprintf(stderr, "aacEncEncode failed: 0x%04x\n", err);
    return 1;
  }
  if (outArgs.numOutBytes > 0) {
    stream.insert(stream.end(), outBuffer,
                  outBuffer + outArgs.numOutBytes);
    ++transportFrames;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s output.adts\n",
                 argc > 0 ? argv[0] : "adts_seal_example");
    return 2;
  }

  HANDLE_AACENCODER encoder = nullptr;
  if (aacEncOpen(&encoder, 0, kChannels) != AACENC_OK) return 1;

  struct Param {
    AACENC_PARAM id;
    UINT value;
  };
  const Param params[] = {
      {AACENC_AOT, AOT_AAC_LC},
      {AACENC_SAMPLERATE, kSampleRate},
      {AACENC_CHANNELMODE, MODE_2},
      {AACENC_BITRATE, kBitrate},
      {AACENC_TRANSMUX, TT_MP4_ADTS},
      {AACENC_TPSUBFRAMES, kSubFrames},
      {AACENC_PROTECTION, 1},
  };
  for (const Param &p : params) {
    if (aacEncoder_SetParam(encoder, p.id, p.value) != AACENC_OK) return 1;
  }
  if (aacEncEncode(encoder, nullptr, nullptr, nullptr, nullptr) !=
      AACENC_OK) {
    return 1;
  }

  const int totalAUs = kInitialAUs + kContinuationAUs;
  std::vector<INT_PCM> pcm = makePcm(totalAUs);
  std::vector<UCHAR> stream;
  int auCount = 0;
  int transportFrames = 0;

  for (int au = 0; au < kInitialAUs; ++au) {
    if (encodeOneAU(encoder, &pcm[static_cast<size_t>(au) * 2048], stream,
                    transportFrames) != 0) {
      aacEncClose(&encoder);
      return 1;
    }
    ++auCount;
  }

  AACENC_AdtsSealInfo query = {};
  if (aacEncSealAdts(encoder, nullptr, 0, &query) != AACENC_OK ||
      query.numAUs != 2) {
    std::fprintf(stderr, "expected two staged AUs\n");
    aacEncClose(&encoder);
    return 1;
  }
  std::vector<UCHAR> sealed(query.numBytes);
  AACENC_AdtsSealInfo sealedInfo = {};
  if (aacEncSealAdts(encoder, sealed.data(),
                     static_cast<UINT>(sealed.size()), &sealedInfo) !=
      AACENC_OK) {
    std::fprintf(stderr, "aacEncSealAdts failed\n");
    aacEncClose(&encoder);
    return 1;
  }
  stream.insert(stream.end(), sealed.begin(), sealed.end());
  ++transportFrames;
  std::printf("sealed AUs=%u adts_bytes=%u removedBits=%u creditedBits=%u\n",
              sealedInfo.numAUs, sealedInfo.numBytes,
              sealedInfo.removedBits, sealedInfo.creditedBits);

  /* The encoder remains live and starts a fresh four-AU aggregation. */
  for (int au = kInitialAUs; au < totalAUs; ++au) {
    if (encodeOneAU(encoder, &pcm[static_cast<size_t>(au) * 2048], stream,
                    transportFrames) != 0) {
      aacEncClose(&encoder);
      return 1;
    }
    ++auCount;
  }

  if (aacEncClose(&encoder) != AACENC_OK) return 1;

  std::FILE *file = std::fopen(argv[1], "wb");
  if (file == nullptr) {
    std::perror("fopen output");
    return 1;
  }
  size_t written = std::fwrite(stream.data(), 1, stream.size(), file);
  int closeStatus = std::fclose(file);
  if (written != stream.size() || closeStatus != 0) return 1;

  std::printf("AUs=%d adts_frames=%d bytes=%zu output=%s\n", auCount,
              transportFrames, stream.size(), argv[1]);
  return 0;
}
