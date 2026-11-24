#include "task_support/check.h"

#include "../cpp/libAACenc/src/aacenc_adts_seal.h"
#include "aacdecoder_lib.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Cfg {
  int N = 4;
  int k = 2;
  int p = 0;
  int ch = 2;
  int sr = 48000;
  int br = 128000;
};

struct Range {
  UINT bitPos;
  UINT bitLen;
};

struct Adts {
  int id = 0;
  int protectionAbsent = 1;
  int profile = 0;
  int srIndex = 3;
  int channel = 2;
  int rawBlocksField = 0;
  int numAUs = 1;
  int frameLength = 0;
  int headerBytes = 7;
  std::vector<int> cumulative;
};

static USHORT testCrcStep(USHORT crc, UINT bit) {
  UINT feedback = (bit ^ ((crc >> 15) & 1u)) & 1u;
  crc = (USHORT)((crc << 1) & 0xffffu);
  if (feedback) crc ^= 0x8005u;
  return crc;
}

static USHORT testCrcBits(USHORT crc, const UCHAR *data, UINT startBit,
                          UINT numBits) {
  for (UINT i = 0; i < numBits; ++i) {
    UINT pos = startBit + i;
    UINT bit = (data[pos >> 3] >> (7 - (pos & 7))) & 1u;
    crc = testCrcStep(crc, bit);
  }
  return crc;
}

static USHORT testCrcDomain(USHORT crc, const UCHAR *data, UINT startBit,
                            UINT actualBits, UINT fixedBits) {
  UINT processed = actualBits;
  if (fixedBits && processed > fixedBits) processed = fixedBits;
  crc = testCrcBits(crc, data, startBit, processed);
  UINT pad = fixedBits ? fixedBits - processed : 0;
  while (pad--) crc = testCrcStep(crc, 0);
  return crc;
}

static USHORT readBe16(const UCHAR *p) {
  return (USHORT)(((USHORT)p[0] << 8) | p[1]);
}

static int openEncoder(HANDLE_AACENCODER *e, const Cfg &c,
                       bool initialize = true) {
  TASK_CHECK(aacEncOpen(e, 0, c.ch) == AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(*e, AACENC_AOT, AOT_AAC_LC) == AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(*e, AACENC_SAMPLERATE, c.sr) == AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(*e, AACENC_CHANNELMODE,
                                c.ch == 1 ? MODE_1 : MODE_2) == AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(*e, AACENC_BITRATE, c.br) == AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(*e, AACENC_TRANSMUX, TT_MP4_ADTS) ==
             AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(*e, AACENC_TPSUBFRAMES, c.N) == AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(*e, AACENC_PROTECTION, c.p) == AACENC_OK);
  if (initialize) {
    TASK_CHECK(aacEncEncode(*e, NULL, NULL, NULL, NULL) == AACENC_OK);
  }
  return 0;
}

static std::vector<INT_PCM> makePcm(int aus, int ch, uint32_t seed = 0x12345u) {
  std::vector<INT_PCM> pcm((size_t)aus * 1024 * ch);
  uint32_t rng = seed;
  for (int a = 0; a < aus; ++a) {
    for (int i = 0; i < 1024; ++i) {
      rng = rng * 1664525u + 1013904223u;
      int tonal = ((i + a * 73) % 197) - 98;
      int v = tonal * 95 + (int)(rng >> 27) - 8;
      v = std::max(-14000, std::min(14000, v));
      for (int c = 0; c < ch; ++c) {
        int channelValue = (c == 0) ? v : -v * 7 / 11;
        pcm[(size_t)a * 1024 * ch + (size_t)i * ch + c] =
            (INT_PCM)channelValue;
      }
    }
  }
  return pcm;
}

static int encodeAu(HANDLE_AACENCODER e, const INT_PCM *frame, int ch,
                    std::vector<UCHAR> &sink, int *outBytes = NULL,
                    AACENC_OutArgs *savedOutArgs = NULL) {
  UCHAR bitBuffer[8192];
  void *inPtr = (void *)frame;
  void *outPtr = bitBuffer;
  INT inId = IN_AUDIO_DATA, outId = OUT_BITSTREAM_DATA;
  INT inSize = 1024 * ch * (INT)sizeof(INT_PCM);
  INT outSize = (INT)sizeof(bitBuffer);
  INT inEl = sizeof(INT_PCM), outEl = 1;
  AACENC_BufDesc in = {1, &inPtr, &inId, &inSize, &inEl};
  AACENC_BufDesc out = {1, &outPtr, &outId, &outSize, &outEl};
  AACENC_InArgs inArgs = {};
  AACENC_OutArgs outArgs = {};
  inArgs.numInSamples = 1024 * ch;

  AACENC_ERROR err = aacEncEncode(e, &in, &out, &inArgs, &outArgs);
  TASK_CHECK(err == AACENC_OK);
  TASK_CHECK(outArgs.numInSamples == 1024 * ch);
  if (outBytes) *outBytes = outArgs.numOutBytes;
  if (savedOutArgs) *savedOutArgs = outArgs;
  if (outArgs.numOutBytes > 0) {
    sink.insert(sink.end(), bitBuffer, bitBuffer + outArgs.numOutBytes);
  }
  return 0;
}

static int feedSamples(HANDLE_AACENCODER e, const INT_PCM *samples,
                       int numSamples, int ch, int *consumed) {
  UCHAR bitBuffer[8192];
  void *inPtr = (void *)samples;
  void *outPtr = bitBuffer;
  INT inId = IN_AUDIO_DATA, outId = OUT_BITSTREAM_DATA;
  INT inSize = numSamples * (INT)sizeof(INT_PCM);
  INT outSize = sizeof(bitBuffer);
  INT inEl = sizeof(INT_PCM), outEl = 1;
  AACENC_BufDesc in = {1, &inPtr, &inId, &inSize, &inEl};
  AACENC_BufDesc out = {1, &outPtr, &outId, &outSize, &outEl};
  AACENC_InArgs inArgs = {};
  AACENC_OutArgs outArgs = {};
  inArgs.numInSamples = numSamples;
  TASK_CHECK(aacEncEncode(e, &in, &out, &inArgs, &outArgs) == AACENC_OK);
  if (consumed) *consumed = outArgs.numInSamples;
  if (outArgs.numOutBytes > 0) {
    /* Partial-PCM tests do not expect output; keep this explicit. */
    return 2;
  }
  return 0;
}

static bool parseAdts(const UCHAR *p, size_t n, Adts &a) {
  if (n < 7 || p[0] != 0xff || (p[1] & 0xf0) != 0xf0) return false;
  a.id = (p[1] >> 3) & 1;
  a.protectionAbsent = p[1] & 1;
  a.profile = (p[2] >> 6) & 3;
  a.srIndex = (p[2] >> 2) & 0xf;
  a.channel = ((p[2] & 1) << 2) | ((p[3] >> 6) & 3);
  a.frameLength = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
  a.rawBlocksField = p[6] & 3;
  a.numAUs = a.rawBlocksField + 1;
  if (a.frameLength <= 7 || (size_t)a.frameLength > n) return false;
  if (a.protectionAbsent) {
    a.headerBytes = 7;
    return true;
  }
  int m = a.rawBlocksField;
  a.headerBytes = 7 + 2 * m + 2;
  if (a.frameLength < a.headerBytes) return false;
  a.cumulative.clear();
  for (int i = 0; i < m; ++i) {
    int v = (p[7 + 2 * i] << 8) | p[8 + 2 * i];
    if (v <= 0) return false;
    a.cumulative.push_back(v);
  }
  return true;
}

static int debugState(
    HANDLE_AACENCODER e, UINT *k, UINT *N, UCHAR **stage = NULL,
    UINT *ends = NULL, INT *res = NULL, INT *resMax = NULL,
    AACENC_AdtsSealCrcDomain *domains = NULL, INT *numDomains = NULL) {
  INT localRes = 0, localMax = 0, localDomains = 0;
  UINT localK = 0, localN = 0;
  UCHAR *localStage = NULL;
  UINT localEnds[4] = {};
  AACENC_AdtsSealCrcDomain localDomainStorage[3] = {};
  INT rc = aacEncAdtsSeal_DebugState(
      e, &localK, &localN, &localStage, localEnds, 4, &localRes, &localMax,
      localDomainStorage, 3, &localDomains);
  TASK_CHECK(rc == 0);
  if (k) *k = localK;
  if (N) *N = localN;
  if (stage) *stage = localStage;
  if (ends) std::memcpy(ends, localEnds, sizeof(localEnds));
  if (res) *res = localRes;
  if (resMax) *resMax = localMax;
  if (domains && numDomains) {
    std::memcpy(domains, localDomainStorage,
                localDomains * sizeof(*domains));
    *numDomains = localDomains;
  }
  if (numDomains && !domains) *numDomains = localDomains;
  return 0;
}

static int collectOriginalAudio(
    HANDLE_AACENCODER e, const Cfg &c,
    std::vector<std::vector<UCHAR>> &audio) {
  UINT k = 0, N = 0;
  UCHAR *stage = NULL;
  UINT ends[4] = {};
  TASK_CHECK(debugState(e, &k, &N, &stage, ends) == 0);
  TASK_CHECK((k == (UINT)c.k || (c.k == c.N && k == 0)) &&
             N == (UINT)c.N && stage != NULL);
  UINT count = (UINT)c.k; /* c.k may be N for a delivered reference group. */
  UINT prev = 56 + (c.p ? 16u * N : 0u);
  audio.clear();
  for (UINT i = 0; i < count; ++i) {
    TASK_CHECK(ends[i] > prev);
    UINT audioBits = ends[i] - prev - (c.p ? 16u : 0u);
    TASK_CHECK((audioBits & 7u) == 0 && audioBits > 0);
    UINT bytePos = prev >> 3;
    UINT byteLen = audioBits >> 3;
    audio.emplace_back(stage + bytePos, stage + bytePos + byteLen);
    prev = ends[i];
  }
  return 0;
}

static int checkSealedPayload(
    const std::vector<UCHAR> &sealed,
    const std::vector<std::vector<UCHAR>> &original, const Cfg &c) {
  Adts a;
  TASK_CHECK(parseAdts(sealed.data(), sealed.size(), a));
  TASK_CHECK(a.numAUs == c.k);
  TASK_CHECK(a.protectionAbsent == !c.p);
  TASK_CHECK(a.channel == c.ch);
  TASK_CHECK(a.profile == 1); /* AAC-LC object type 2 -> profile 1 */
  TASK_CHECK(a.frameLength == (int)sealed.size());
  TASK_CHECK((a.srIndex == 5 && c.sr == 32000) ||
             (a.srIndex == 4 && c.sr == 44100) ||
             (a.srIndex == 3 && c.sr == 48000));

  UINT pos = c.p ? (c.k == 1 ? 72u : 56u + 16u * c.k) : 56u;
  for (int i = 0; i < c.k; ++i) {
    const std::vector<UCHAR> &ref = original[i];
    UINT bitLen = (UINT)ref.size() * 8u;
    TASK_CHECK(pos % 8 == 0);
    TASK_CHECK(pos / 8 + ref.size() <= sealed.size());
    TASK_CHECK(std::memcmp(sealed.data() + pos / 8, ref.data(), ref.size()) ==
               0);
    pos += bitLen;
    if (c.p && c.k >= 2) pos += 16; /* old independent raw CRC */
  }
  return 0;
}

static int checkHeaderCrcScalar(const std::vector<UCHAR> &sealed,
                                const Cfg &c) {
  if (!c.p) return 0;
  int m = c.k - 1;
  if (m > 0) {
    UINT regionBits = 56 + 16u * m;
    USHORT expected = testCrcBits(0xffff, sealed.data(), 0, regionBits);
    int crcOffset = 7 + 2 * m;
    TASK_CHECK(readBe16(sealed.data() + crcOffset) == expected);
  }
  return 0;
}

static int verifySingleSharedCrc(
    const std::vector<UCHAR> &sealed, const UCHAR *savedStage,
    const AACENC_AdtsSealCrcDomain *domains, INT numDomains,
    const UINT ends[4], UINT nominalBlocks) {
  TASK_CHECK(numDomains >= 1 && numDomains <= 2);
  USHORT crc = testCrcBits(0xffff, sealed.data(), 0, 56);
  for (INT i = 0; i < numDomains; ++i) {
    crc = testCrcDomain(crc, savedStage, domains[i].startBit,
                        domains[i].actualBits, domains[i].fixedBits);
  }
  TASK_CHECK(readBe16(sealed.data() + 7) == crc);

  UINT oldCrcByte = (ends[0] - 16) / 8;
  USHORT oldRawCrc = readBe16(savedStage + oldCrcByte);
  TASK_CHECK(oldRawCrc != crc);
  UINT audioBytes = (ends[0] - (56 + 16 * nominalBlocks) - 16) / 8;
  USHORT wholePayloadCrc =
      testCrcBits(0xffff, sealed.data(), 72, audioBytes * 8);
  TASK_CHECK(wholePayloadCrc != crc);
  return 0;
}

struct DecodeStats {
  HANDLE_AACDECODER dec = NULL;
  int frames = 0;
  int64_t energy = 0;
};

static int initAdtsDecoder(DecodeStats &s) {
  s.dec = aacDecoder_Open(TT_MP4_ADTS, 1);
  TASK_CHECK(s.dec != NULL);
  return 0;
}

static int pumpAdtsDecoder(DecodeStats &s) {
  INT_PCM pcm[4096];
  for (;;) {
    AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(s.dec, pcm, 2048, 0);
    if (err == AAC_DEC_NOT_ENOUGH_BITS) break;
    TASK_CHECK(err == AAC_DEC_OK);
    CStreamInfo *info = aacDecoder_GetStreamInfo(s.dec);
    TASK_CHECK(info != NULL && info->frameSize == 1024);
    TASK_CHECK(info->numChannels >= 1 && info->numChannels <= 2);
    int64_t frameEnergy = 0;
    for (int i = 0; i < 1024 * info->numChannels; ++i) {
      frameEnergy += std::abs((int)pcm[i]);
    }
    /* The first one or two core frames can carry codec-delay silence; the
       suite separately checks substantial aggregate non-zero PCM energy. */
    s.energy += frameEnergy;
    ++s.frames;
  }
  return 0;
}

static int feedAdts(DecodeStats &s, const std::vector<UCHAR> &bytes) {
  UCHAR *p = const_cast<UCHAR *>(bytes.data());
  UINT bufferSize = (UINT)bytes.size();
  UINT valid = bufferSize;
  TASK_CHECK(aacDecoder_Fill(s.dec, &p, &bufferSize, &valid) == AAC_DEC_OK);
  TASK_CHECK(valid == 0);
  return pumpAdtsDecoder(s);
}

static int decodeRawAudio(
    const AACENC_InfoStruct &encInfo,
    const std::vector<std::vector<UCHAR>> &aus, int ch,
    std::vector<int64_t> *energiesOut = NULL) {
  HANDLE_AACDECODER d = aacDecoder_Open(TT_MP4_RAW, 1);
  TASK_CHECK(d != NULL);
  UCHAR *conf = const_cast<UCHAR *>(encInfo.confBuf);
  UINT confSize = encInfo.confSize;
  TASK_CHECK(aacDecoder_ConfigRaw(d, &conf, &confSize) == AAC_DEC_OK);
  int frames = 0;
  int64_t energy = 0;
  for (size_t i = 0; i < aus.size(); ++i) {
    UCHAR *p = const_cast<UCHAR *>(aus[i].data());
    UINT size = (UINT)aus[i].size(), valid = size;
    TASK_CHECK(aacDecoder_Fill(d, &p, &size, &valid) == AAC_DEC_OK);
    INT_PCM pcm[2048];
    AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(d, pcm, 2048, 0);
    TASK_CHECK(err == AAC_DEC_OK);
    CStreamInfo *si = aacDecoder_GetStreamInfo(d);
    TASK_CHECK(si && si->frameSize == 1024 && si->numChannels == ch);
    int64_t e = 0;
    for (int j = 0; j < 1024 * ch; ++j) e += std::abs((int)pcm[j]);
    energy += e;
    if (energiesOut) energiesOut->push_back(e);
    ++frames;
  }
  TASK_CHECK(frames == (int)aus.size() && energy > 10000 * frames);
  aacDecoder_Close(d);
  return 0;
}

static int runSealMatrix(const Cfg &c, bool decodeChecks) {
  HANDLE_AACENCODER e;
  TASK_CHECK(openEncoder(&e, c) == 0);
  std::vector<INT_PCM> pcm = makePcm(c.k + 3 * c.N + 2, c.ch);
  for (int i = 0; i < c.k; ++i) {
    std::vector<UCHAR> dummy;
    TASK_CHECK(encodeAu(e, &pcm[(size_t)i * 1024 * c.ch], c.ch, dummy) == 0);
  }

  std::vector<std::vector<UCHAR>> original;
  TASK_CHECK(collectOriginalAudio(e, c, original) == 0);

  AACENC_AdtsSealInfo q = {};
  TASK_CHECK(aacEncSealAdts(e, NULL, 0, &q) == AACENC_OK);
  TASK_CHECK(q.numAUs == (UINT)c.k);
  TASK_CHECK(q.creditedBits == 0);
  TASK_CHECK(q.bitResBefore == q.bitResAfter);

  std::vector<UCHAR> sealed(q.numBytes);
  AACENC_AdtsSealInfo done = {};
  TASK_CHECK(aacEncSealAdts(e, sealed.data(), (UINT)sealed.size(), &done) ==
             AACENC_OK);
  TASK_CHECK(done.numBytes == q.numBytes && done.numAUs == q.numAUs);
  TASK_CHECK(done.bitResAfter == done.bitResBefore + (INT)done.creditedBits);
  TASK_CHECK(checkSealedPayload(sealed, original, c) == 0);
  TASK_CHECK(checkHeaderCrcScalar(sealed, c) == 0);

  /* Re-query is empty. */
  AACENC_AdtsSealInfo after = {};
  TASK_CHECK(aacEncSealAdts(e, NULL, 0, &after) == AACENC_OK);
  TASK_CHECK(after.numAUs == 0 && after.numBytes == 0);

  if (decodeChecks) {
    AACENC_InfoStruct info = {};
    TASK_CHECK(aacEncInfo(e, &info) == AACENC_OK);
    TASK_CHECK(info.frameLength == 1024);
    if (!c.p) {
      TASK_CHECK(decodeRawAudio(info, original, c.ch) == 0);
    } else {
      DecodeStats ds;
      TASK_CHECK(initAdtsDecoder(ds) == 0);
      TASK_CHECK(feedAdts(ds, sealed) == 0);
      int fullFrames = 0;
      std::vector<UCHAR> fullStream;
      for (int g = 0; g < 3; ++g) {
        for (int i = 0; i < c.N; ++i) {
          int au = c.k + g * c.N + i;
          TASK_CHECK(au * 1024 * c.ch + 1024 * c.ch <= (int)pcm.size());
          int got = 0;
          TASK_CHECK(encodeAu(e, &pcm[(size_t)au * 1024 * c.ch], c.ch,
                              fullStream, &got) == 0);
          if (i == c.N - 1) TASK_CHECK(got > 0);
        }
        TASK_CHECK(feedAdts(ds, fullStream) == 0);
        fullStream.clear();
        fullFrames += c.N;
      }
      /* Three trailing full groups provide enough ADTS look-ahead to move
         well past every sealed AU and produce non-zero PCM. */
      TASK_CHECK(ds.frames >= c.k + c.N);
      TASK_CHECK(ds.energy > 2000 * ds.frames);
      aacDecoder_Close(ds.dec);
      (void)fullFrames;
    }
  }

  TASK_CHECK(aacEncClose(&e) == AACENC_OK);
  return 0;
}

/* ------------------------------- framing -------------------------------- */

static int framing_unprotected_matrix(void) {
  for (int N = 2; N <= 4; ++N) {
    for (int k = 1; k < N; ++k) {
      Cfg c;
      c.N = N;
      c.k = k;
      c.p = 0;
      c.ch = (N == 3) ? 1 : 2;
      TASK_CHECK(runSealMatrix(c, false) == 0);
    }
  }
  return 0;
}

static int framing_protected_matrix(void) {
  for (int N = 2; N <= 4; ++N) {
    for (int k = 1; k < N; ++k) {
      Cfg c;
      c.N = N;
      c.k = k;
      c.p = 1;
      c.ch = (k == 1 && N == 3) ? 1 : 2;
      TASK_CHECK(runSealMatrix(c, false) == 0);
    }
  }
  return 0;
}

static int framing_supported_rate_channel_matrix(void) {
  const int srates[3] = {32000, 44100, 48000};
  const int bitrates[3] = {96000, 128000, 192000};
  for (int sr : srates) {
    for (int br : bitrates) {
      for (int ch = 1; ch <= 2; ++ch) {
        Cfg c;
        c.N = 2;
        c.k = 1;
        c.p = (sr == 44100 && br == 192000) ? 1 : 0;
        c.ch = ch;
        c.sr = sr;
        c.br = br;
        TASK_CHECK(runSealMatrix(c, false) == 0);
      }
    }
  }
  return 0;
}

static int framing_single_shared_crc_case(void) {
  Cfg c;
  c.N = 4;
  c.k = 1;
  c.p = 1;
  c.ch = 2;
  HANDLE_AACENCODER e;
  TASK_CHECK(openEncoder(&e, c) == 0);
  std::vector<INT_PCM> pcm = makePcm(2, c.ch);
  std::vector<UCHAR> dummy;
  TASK_CHECK(encodeAu(e, pcm.data(), c.ch, dummy) == 0);

  AACENC_AdtsSealInfo q = {};
  TASK_CHECK(aacEncSealAdts(e, NULL, 0, &q) == AACENC_OK);

  AACENC_AdtsSealCrcDomain domains[3] = {};
  INT numDomains = 0;
  UINT dbgK = 0, dbgN = 0, ends[4] = {};
  UCHAR *stage = NULL;
  TASK_CHECK(debugState(e, &dbgK, &dbgN, &stage, ends, NULL, NULL, domains,
                        &numDomains) == 0);
  TASK_CHECK(dbgK == 1 && dbgN == 4);
  std::vector<UCHAR> savedStage(stage, stage + (ends[0] >> 3));

  std::vector<UCHAR> sealed(q.numBytes);
  AACENC_AdtsSealInfo s = {};
  TASK_CHECK(aacEncSealAdts(e, sealed.data(), sealed.size(), &s) ==
             AACENC_OK);
  TASK_CHECK(verifySingleSharedCrc(sealed, savedStage.data(), domains,
                                   numDomains, ends, dbgN) == 0);
  TASK_CHECK(checkHeaderCrcScalar(sealed, c) == 0);
  TASK_CHECK(aacEncClose(&e) == AACENC_OK);
  return 0;
}

static int framing_reset_and_continuous_case(void) {
  Cfg c;
  c.N = 4;
  c.k = 2;
  c.p = 1;
  c.ch = 2;
  HANDLE_AACENCODER e;
  TASK_CHECK(openEncoder(&e, c) == 0);
  std::vector<INT_PCM> pcm = makePcm(16, c.ch);

  /* First short group. */
  for (int i = 0; i < 2; ++i) {
    std::vector<UCHAR> sink;
    TASK_CHECK(encodeAu(e, &pcm[i * 1024 * c.ch], c.ch, sink) == 0);
  }
  AACENC_AdtsSealInfo s1 = {};
  std::vector<UCHAR> g1(4096);
  TASK_CHECK(aacEncSealAdts(e, g1.data(), g1.size(), &s1) == AACENC_OK);
  g1.resize(s1.numBytes);

  /* A second short group starts aggregation from zero. */
  for (int i = 2; i < 3; ++i) {
    std::vector<UCHAR> sink;
    TASK_CHECK(encodeAu(e, &pcm[i * 1024 * c.ch], c.ch, sink) == 0);
  }
  AACENC_AdtsSealInfo q2 = {};
  TASK_CHECK(aacEncSealAdts(e, NULL, 0, &q2) == AACENC_OK);
  TASK_CHECK(q2.numAUs == 1);
  std::vector<UCHAR> g2(q2.numBytes);
  TASK_CHECK(aacEncSealAdts(e, g2.data(), g2.size(), &q2) == AACENC_OK);

  /* Then a complete N-block group must have fresh N-1 position fields. */
  std::vector<UCHAR> full;
  for (int i = 3; i < 7; ++i) {
    int got = 0;
    TASK_CHECK(encodeAu(e, &pcm[i * 1024 * c.ch], c.ch, full, &got) == 0);
    if (i == 6) {
      TASK_CHECK(got > 0);
    } else {
      TASK_CHECK(got == 0);
    }
  }
  Adts a1, a2, af;
  TASK_CHECK(parseAdts(g1.data(), g1.size(), a1) && a1.numAUs == 2);
  TASK_CHECK(parseAdts(g2.data(), g2.size(), a2) && a2.numAUs == 1);
  TASK_CHECK(parseAdts(full.data(), full.size(), af) && af.numAUs == 4);
  TASK_CHECK(af.cumulative.size() == 3);
  Cfg crc21; crc21.N = 4; crc21.k = 2; crc21.p = 1;
  Cfg crc11; crc11.N = 4; crc11.k = 1; crc11.p = 1;
  Cfg crc41; crc41.N = 4; crc41.k = 4; crc41.p = 1;
  TASK_CHECK(checkHeaderCrcScalar(g1, crc21) == 0);
  TASK_CHECK(checkHeaderCrcScalar(g2, crc11) == 0);
  TASK_CHECK(checkHeaderCrcScalar(full, crc41) == 0);
  TASK_CHECK(aacEncClose(&e) == AACENC_OK);
  return 0;
}

/* ------------------------------ transaction ----------------------------- */

static int tx_invalid_arguments_and_support(void) {
  AACENC_AdtsSealInfo info;
  TASK_CHECK(aacEncSealAdts(NULL, NULL, 0, &info) ==
             AACENC_INVALID_HANDLE);

  HANDLE_AACENCODER uninit;
  TASK_CHECK(aacEncOpen(&uninit, 0, 2) == AACENC_OK);
  TASK_CHECK(aacEncSealAdts(uninit, NULL, 0, &info) ==
             AACENC_INVALID_CONFIG);
  TASK_CHECK(aacEncSealAdts(uninit, NULL, 0, NULL) ==
             AACENC_INVALID_CONFIG);
  TASK_CHECK(aacEncClose(&uninit) == AACENC_OK);

  /* TPSUBFRAMES=1 is outside the seal support contract. */
  HANDLE_AACENCODER one;
  TASK_CHECK(aacEncOpen(&one, 0, 2) == AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(one, AACENC_AOT, 2) == AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(one, AACENC_SAMPLERATE, 48000) == AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(one, AACENC_CHANNELMODE, MODE_2) ==
             AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(one, AACENC_BITRATE, 128000) == AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(one, AACENC_TRANSMUX, TT_MP4_ADTS) ==
             AACENC_OK);
  TASK_CHECK(aacEncoder_SetParam(one, AACENC_TPSUBFRAMES, 1) == AACENC_OK);
  TASK_CHECK(aacEncEncode(one, NULL, NULL, NULL, NULL) == AACENC_OK);
  TASK_CHECK(aacEncSealAdts(one, NULL, 0, &info) ==
             AACENC_UNSUPPORTED_PARAMETER);
  TASK_CHECK(aacEncClose(&one) == AACENC_OK);

  /* Pending reinitialization has precedence over the supported settings. */
  Cfg c;
  HANDLE_AACENCODER e;
  TASK_CHECK(openEncoder(&e, c) == 0);
  TASK_CHECK(aacEncoder_SetParam(e, AACENC_BITRATE, 96000) == AACENC_OK);
  TASK_CHECK(aacEncSealAdts(e, NULL, 0, &info) == AACENC_INVALID_CONFIG);
  TASK_CHECK(aacEncClose(&e) == AACENC_OK);
  return 0;
}

static int tx_query_empty_partial_pcm_capacity(void) {
  Cfg c;
  c.N = 4;
  c.k = 1;
  c.ch = 1;
  HANDLE_AACENCODER e;
  TASK_CHECK(openEncoder(&e, c) == 0);

  AACENC_AdtsSealInfo empty = {};
  TASK_CHECK(aacEncSealAdts(e, NULL, 0, &empty) == AACENC_OK);
  TASK_CHECK(empty.numAUs == 0 && empty.numBytes == 0);
  TASK_CHECK(empty.creditedBits == 0 &&
             empty.bitResBefore == empty.bitResAfter);

  /* Accumulate half an AU worth of PCM.  It must remain pending. */
  std::vector<INT_PCM> pcm = makePcm(2, 1);
  int consumed = -1;
  TASK_CHECK(feedSamples(e, pcm.data(), 512, 1, &consumed) == 0);
  TASK_CHECK(consumed == 512);
  AACENC_AdtsSealInfo partial = {};
  TASK_CHECK(aacEncSealAdts(e, NULL, 0, &partial) == AACENC_OK);
  TASK_CHECK(partial.numAUs == 0);

  consumed = -1;
  TASK_CHECK(feedSamples(e, pcm.data() + 512, 512, 1, &consumed) == 0);
  TASK_CHECK(consumed == 512);
  UINT k = 0;
  TASK_CHECK(debugState(e, &k, NULL) == 0 && k == 1);

  AACENC_AdtsSealInfo q = {};
  TASK_CHECK(aacEncSealAdts(e, NULL, 0, &q) == AACENC_OK);
  TASK_CHECK(q.numAUs == 1 && q.numBytes > 9);

  INT resBefore = 0, resMax = 0;
  TASK_CHECK(debugState(e, NULL, NULL, NULL, NULL, &resBefore, &resMax) == 0);
  std::vector<UCHAR> canary(q.numBytes - 1, 0xA5);
  AACENC_AdtsSealInfo small = {};
  TASK_CHECK(aacEncSealAdts(e, canary.data(), (UINT)canary.size(), &small) ==
             AACENC_OUTPUT_BUFFER_TOO_SMALL);
  TASK_CHECK(small.numBytes == q.numBytes && small.numAUs == 1);
  for (UCHAR x : canary) TASK_CHECK(x == 0xA5);
  INT resAfterFail = 0;
  TASK_CHECK(debugState(e, NULL, NULL, NULL, NULL, &resAfterFail) == 0);
  TASK_CHECK(resAfterFail == resBefore);
  TASK_CHECK(debugState(e, &k, NULL) == 0 && k == 1);

  std::vector<UCHAR> out(q.numBytes);
  AACENC_AdtsSealInfo ok = {};
  TASK_CHECK(aacEncSealAdts(e, out.data(), out.size(), &ok) == AACENC_OK);
  TASK_CHECK(ok.numBytes == q.numBytes);
  Adts adts;
  TASK_CHECK(parseAdts(out.data(), out.size(), adts) && adts.numAUs == 1);
  TASK_CHECK(aacEncClose(&e) == AACENC_OK);
  return 0;
}

static int tx_two_instances_retry(void) {
  Cfg aCfg;
  aCfg.N = 3;
  aCfg.k = 1;
  aCfg.p = 1;
  aCfg.ch = 1;
  Cfg bCfg;
  bCfg.N = 4;
  bCfg.k = 2;
  bCfg.p = 0;
  bCfg.ch = 2;

  HANDLE_AACENCODER a, b;
  TASK_CHECK(openEncoder(&a, aCfg) == 0);
  TASK_CHECK(openEncoder(&b, bCfg) == 0);
  std::vector<INT_PCM> pa = makePcm(2, 1, 7);
  std::vector<INT_PCM> pb = makePcm(3, 2, 9);
  std::vector<UCHAR> sink;
  TASK_CHECK(encodeAu(a, pa.data(), 1, sink) == 0);
  for (int i = 0; i < 2; ++i)
    TASK_CHECK(encodeAu(b, &pb[i * 2048], 2, sink) == 0);

  AACENC_AdtsSealInfo qa = {}, qb = {};
  TASK_CHECK(aacEncSealAdts(a, NULL, 0, &qa) == AACENC_OK);
  TASK_CHECK(aacEncSealAdts(b, NULL, 0, &qb) == AACENC_OK);
  TASK_CHECK(qa.numAUs == 1 && qb.numAUs == 2);

  std::vector<UCHAR> aa(qa.numBytes), bb(qb.numBytes);
  TASK_CHECK(aacEncSealAdts(a, aa.data(), 1, &qa) ==
             AACENC_OUTPUT_BUFFER_TOO_SMALL);
  TASK_CHECK(aacEncSealAdts(b, bb.data(), 1, &qb) ==
             AACENC_OUTPUT_BUFFER_TOO_SMALL);
  TASK_CHECK(aacEncSealAdts(a, aa.data(), aa.size(), &qa) == AACENC_OK);
  TASK_CHECK(aacEncSealAdts(b, bb.data(), bb.size(), &qb) == AACENC_OK);
  Adts xa, xb;
  TASK_CHECK(parseAdts(aa.data(), aa.size(), xa) && xa.numAUs == 1);
  TASK_CHECK(parseAdts(bb.data(), bb.size(), xb) && xb.numAUs == 2);
  TASK_CHECK(aacEncClose(&a) == AACENC_OK);
  TASK_CHECK(aacEncClose(&b) == AACENC_OK);
  return 0;
}

/* ------------------------------ reservoir ------------------------------- */

static int reservoir_info_and_qc_state(void) {
  for (int p = 0; p <= 1; ++p) {
    Cfg c;
    c.N = 4;
    c.k = 2;
    c.p = p;
    HANDLE_AACENCODER e;
    TASK_CHECK(openEncoder(&e, c) == 0);
    std::vector<INT_PCM> pcm = makePcm(2, 2);
    std::vector<UCHAR> sink;
    for (int i = 0; i < 2; ++i)
      TASK_CHECK(encodeAu(e, &pcm[i * 2048], 2, sink) == 0);

    INT realBefore = 0, realMax = 0;
    TASK_CHECK(debugState(e, NULL, NULL, NULL, NULL, &realBefore,
                          &realMax) == 0);
    AACENC_AdtsSealInfo q = {};
    TASK_CHECK(aacEncSealAdts(e, NULL, 0, &q) == AACENC_OK);
    TASK_CHECK(q.bitResBefore == realBefore);
    TASK_CHECK(q.bitResAfter == realBefore);
    TASK_CHECK(q.creditedBits == 0);

    std::vector<UCHAR> out(q.numBytes);
    AACENC_AdtsSealInfo s = {};
    TASK_CHECK(aacEncSealAdts(e, out.data(), out.size(), &s) == AACENC_OK);
    UINT expectedCredit =
        std::min(s.removedBits, (UINT)(realMax - realBefore));
    TASK_CHECK(s.creditedBits == expectedCredit);
    TASK_CHECK(s.bitResAfter == realBefore + (INT)expectedCredit);
    INT realAfter = 0;
    TASK_CHECK(debugState(e, NULL, NULL, NULL, NULL, &realAfter) == 0);
    TASK_CHECK(realAfter == s.bitResAfter);
    TASK_CHECK(realAfter > 1000 && realAfter <= realMax);
    TASK_CHECK(aacEncClose(&e) == AACENC_OK);
  }
  return 0;
}

static int reservoir_cap_and_failed_no_credit(void) {
  Cfg c;
  c.N = 4;
  c.k = 1;
  c.p = 1;
  HANDLE_AACENCODER e;
  TASK_CHECK(openEncoder(&e, c) == 0);
  std::vector<INT_PCM> pcm = makePcm(1, 2);
  std::vector<UCHAR> sink;
  TASK_CHECK(encodeAu(e, pcm.data(), 2, sink) == 0);
  INT before = 0, maxRes = 0;
  TASK_CHECK(debugState(e, NULL, NULL, NULL, NULL, &before, &maxRes) == 0);
  AACENC_AdtsSealInfo q = {};
  TASK_CHECK(aacEncSealAdts(e, NULL, 0, &q) == AACENC_OK);
  /* A fresh LC reservoir is normally full: the cap must make credit zero even
     though removedBits is non-zero.  If not full in this build, verify the
     explicit cap formula instead. */
  UINT credit = std::min(q.removedBits, (UINT)(maxRes - before));
  std::vector<UCHAR> tiny(1);
  TASK_CHECK(aacEncSealAdts(e, tiny.data(), 0, &q) ==
             AACENC_OUTPUT_BUFFER_TOO_SMALL);
  INT afterFail = 0;
  TASK_CHECK(debugState(e, NULL, NULL, NULL, NULL, &afterFail) == 0);
  TASK_CHECK(afterFail == before);

  std::vector<UCHAR> out(q.numBytes);
  AACENC_AdtsSealInfo s = {};
  TASK_CHECK(aacEncSealAdts(e, out.data(), out.size(), &s) == AACENC_OK);
  TASK_CHECK(s.creditedBits == credit);
  TASK_CHECK(s.bitResAfter <= maxRes);
  TASK_CHECK(aacEncClose(&e) == AACENC_OK);
  return 0;
}

static int reservoir_successor_budget_uses_credit(void) {
  Cfg c;
  c.N = 4;
  c.k = 2;
  c.p = 0;
  c.ch = 2;

  HANDLE_AACENCODER credited, control;
  TASK_CHECK(openEncoder(&credited, c) == 0);
  TASK_CHECK(openEncoder(&control, c) == 0);
  std::vector<INT_PCM> pcm = makePcm(4, 2, 0xabcdefu);
  std::vector<UCHAR> sink;
  for (int i = 0; i < 2; ++i) {
    TASK_CHECK(encodeAu(credited, &pcm[i * 2048], 2, sink) == 0);
    TASK_CHECK(encodeAu(control, &pcm[i * 2048], 2, sink) == 0);
  }

  AACENC_AdtsSealInfo s = {};
  std::vector<UCHAR> out(8192);
  TASK_CHECK(aacEncSealAdts(credited, out.data(), out.size(), &s) ==
             AACENC_OK);
  /* Control gets identical fresh transport assembly, but no reservoir
     credit. */
  TASK_CHECK(aacEncAdtsSeal_DebugResetAssembly(control) == 0);

  INT resCredited = 0, resControl = 0;
  TASK_CHECK(debugState(credited, NULL, NULL, NULL, NULL, &resCredited) == 0);
  TASK_CHECK(debugState(control, NULL, NULL, NULL, NULL, &resControl) == 0);
  TASK_CHECK(resCredited == resControl + (INT)s.creditedBits);

  AACENC_OutArgs oaA = {}, oaB = {};
  int gotA = 0, gotB = 0;
  TASK_CHECK(encodeAu(credited, &pcm[2 * 2048], 2, sink, &gotA, &oaA) == 0);
  TASK_CHECK(encodeAu(control, &pcm[2 * 2048], 2, sink, &gotB, &oaB) == 0);
  INT grantedA = 0, grantedB = 0;
  TASK_CHECK(aacEncAdtsSeal_DebugBudget(credited, &grantedA, NULL) == 0);
  TASK_CHECK(aacEncAdtsSeal_DebugBudget(control, &grantedB, NULL) == 0);
  TASK_CHECK(grantedA > 100 && grantedB > 100);
  TASK_CHECK(grantedA >= grantedB);
  INT qcA = 0, qcB = 0;
  TASK_CHECK(debugState(credited, NULL, NULL, NULL, NULL, &qcA) == 0);
  TASK_CHECK(debugState(control, NULL, NULL, NULL, NULL, &qcB) == 0);
  TASK_CHECK(oaA.bitResState == qcA);
  TASK_CHECK(oaB.bitResState == qcB);
  TASK_CHECK(aacEncClose(&credited) == AACENC_OK);
  TASK_CHECK(aacEncClose(&control) == AACENC_OK);
  return 0;
}

/* -------------------------------- audio --------------------------------- */

static int audio_unprotected_raw_decode_matrix(void) {
  for (int N = 2; N <= 4; ++N) {
    for (int k = 1; k < N; ++k) {
      Cfg c;
      c.N = N;
      c.k = k;
      c.p = 0;
      c.ch = (N & 1) ? 1 : 2;
      HANDLE_AACENCODER e;
      TASK_CHECK(openEncoder(&e, c) == 0);
      std::vector<INT_PCM> pcm =
          makePcm(2 * N + k, c.ch, 0x333 + N * 10 + k);
      std::vector<std::vector<UCHAR>> allAudio;
      Cfg warmCfg = c;
      warmCfg.k = N;
      for (int g = 0; g < 2; ++g) {
        std::vector<UCHAR> warmup;
        for (int i = 0; i < N; ++i) {
          int pcmAu = g * N + i;
          TASK_CHECK(encodeAu(e, &pcm[(size_t)pcmAu * 1024 * c.ch], c.ch,
                              warmup) == 0);
        }
        std::vector<std::vector<UCHAR>> groupAudio;
        TASK_CHECK(collectOriginalAudio(e, warmCfg, groupAudio) == 0);
        allAudio.insert(allAudio.end(), groupAudio.begin(), groupAudio.end());
      }

      for (int i = 0; i < k; ++i) {
        std::vector<UCHAR> sink;
        TASK_CHECK(encodeAu(e, &pcm[(size_t)(2 * N + i) * 1024 * c.ch], c.ch,
                            sink) == 0);
      }
      std::vector<std::vector<UCHAR>> audio;
      TASK_CHECK(collectOriginalAudio(e, c, audio) == 0);
      allAudio.insert(allAudio.end(), audio.begin(), audio.end());
      AACENC_InfoStruct info = {};
      TASK_CHECK(aacEncInfo(e, &info) == AACENC_OK);
      std::vector<int64_t> energies;
      TASK_CHECK(decodeRawAudio(info, allAudio, c.ch, &energies) == 0);
      /* The target AUs are decoded after the warm-up stream. */
      for (int i = 0; i < k; ++i) {
        TASK_CHECK(energies[2 * N + i] > 100000);
      }
      TASK_CHECK(aacEncClose(&e) == AACENC_OK);
    }
  }
  return 0;
}

static int audio_protected_adts_decode(void) {
  for (int N = 2; N <= 4; ++N) {
    for (int k = 1; k < N; ++k) {
      Cfg c;
      c.N = N;
      c.k = k;
      c.p = 1;
      c.ch = (N == 3 && (k == 1 || k == 2)) ? 1 : 2;
      TASK_CHECK(runSealMatrix(c, true) == 0);
    }
  }
  return 0;
}

static int audio_no_call_byte_comparison_and_flush(void) {
  /* Two untouched runs must be byte-identical.  Interleaving read-only
     queries and failed retries must also leave ordinary delivery unchanged. */
  Cfg c;
  c.N = 4;
  c.k = 3;
  HANDLE_AACENCODER plain, probed;
  TASK_CHECK(openEncoder(&plain, c) == 0);
  TASK_CHECK(openEncoder(&probed, c) == 0);
  std::vector<INT_PCM> pcm = makePcm(12, 2, 0x5151);
  std::vector<UCHAR> a, b;
  for (int au = 0; au < 12; ++au) {
    int ga = 0, gb = 0;
    TASK_CHECK(encodeAu(plain, &pcm[au * 2048], 2, a, &ga) == 0);
    TASK_CHECK(encodeAu(probed, &pcm[au * 2048], 2, b, &gb) == 0);
    TASK_CHECK(ga == gb);
    if (au < 3) {
      AACENC_AdtsSealInfo q = {};
      TASK_CHECK(aacEncSealAdts(probed, NULL, 0, &q) == AACENC_OK);
      if (au == 2) {
        UCHAR tiny[2] = {};
        TASK_CHECK(aacEncSealAdts(probed, tiny, sizeof(tiny), &q) ==
                   AACENC_OUTPUT_BUFFER_TOO_SMALL);
      }
    }
  }
  TASK_CHECK(a == b);

  /* Original flush remains available and reaches EOF. */
  AACENC_InArgs flushArgs = {};
  flushArgs.numInSamples = -1;
  AACENC_OutArgs flushOut = {};
  UCHAR flushBuffer[8192];
  void *outPtr = flushBuffer;
  INT outId = OUT_BITSTREAM_DATA, outSize = sizeof(flushBuffer), outEl = 1;
  AACENC_BufDesc outDesc = {1, &outPtr, &outId, &outSize, &outEl};
  AACENC_ERROR err =
      aacEncEncode(plain, NULL, &outDesc, &flushArgs, &flushOut);
  TASK_CHECK(err == AACENC_OK || err == AACENC_ENCODE_EOF);
  for (int flushIter = 0;
       err == AACENC_OK && flushIter < 8; ++flushIter) {
    err = aacEncEncode(plain, NULL, &outDesc, &flushArgs, &flushOut);
    TASK_CHECK(err == AACENC_OK || err == AACENC_ENCODE_EOF);
  }
  TASK_CHECK(err == AACENC_ENCODE_EOF);
  TASK_CHECK(aacEncClose(&plain) == AACENC_OK);
  TASK_CHECK(aacEncClose(&probed) == AACENC_OK);
  return 0;
}

} // namespace

extern "C" const struct task_case task_cases[] = {
    {"framing", "unprotected_n2_to_n4", framing_unprotected_matrix},
    {"framing", "protected_n2_to_n4", framing_protected_matrix},
    {"framing", "supported_rates_and_channels",
     framing_supported_rate_channel_matrix},
    {"framing", "single_shared_crc", framing_single_shared_crc_case},
    {"framing", "positions_reset_after_seal",
     framing_reset_and_continuous_case},
    {"transaction", "invalid_arguments_and_support",
     tx_invalid_arguments_and_support},
    {"transaction", "empty_query_partial_pcm_capacity",
     tx_query_empty_partial_pcm_capacity},
    {"transaction", "two_instances_and_retry", tx_two_instances_retry},
    {"reservoir", "info_matches_real_qc", reservoir_info_and_qc_state},
    {"reservoir", "cap_and_failed_has_no_credit",
     reservoir_cap_and_failed_no_credit},
    {"reservoir", "successor_budget_uses_credit",
     reservoir_successor_budget_uses_credit},
    {"audio", "unprotected_raw_decode", audio_unprotected_raw_decode_matrix},
    {"audio", "protected_adts_decode", audio_protected_adts_decode},
    {"audio", "no_call_bytes_and_flush",
     audio_no_call_byte_comparison_and_flush},
};

extern "C" const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
