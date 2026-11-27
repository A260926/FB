/* Tests for the Sonic speed track API (sonicSetSpeedTrack).

   These tests generate their own signals (a 220 Hz sine, a speech-shaped
   signal with pauses, and a stereo signal with a fixed channel ratio) and
   check the real audio output of the library: total durations are compared
   against an independent integration of 1/speed over the source frames, and
   steady-state pitch is measured on the produced samples with an AMDF
   estimator. */

#include "sonic.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TEST_FREQ 220.0

static int gCases = 0;
static int gFailures = 0;

#define CHECK(cond)                                             \
  do {                                                          \
    if (!(cond)) {                                              \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond);   \
      gFailures++;                                              \
    }                                                           \
  } while (0)

#define BEGIN_CASE(name)                        \
  do {                                          \
    gCases++;                                   \
    printf("case %d: %s\n", gCases, name);      \
  } while (0)

/* ------------------------------------------------------------------ */
/* Signal generators                                                   */
/* ------------------------------------------------------------------ */

/* 220 Hz sine wave. */
static void genSine(short* out, int frames, int sampleRate, double amp) {
  int i;
  for (i = 0; i < frames; i++) {
    out[i] = (short)(amp * sin(2.0 * M_PI * TEST_FREQ * i / sampleRate));
  }
}

/* Speech-shaped signal: voiced segments (220 Hz fundamental plus harmonics,
   vibrato, raised-cosine envelope) separated by silent pauses. */
static void genSpeech(short* out, int frames, int sampleRate) {
  int voiced = (int)(0.45 * sampleRate);
  int pause = (int)(0.55 * sampleRate);
  int cycle = voiced + pause;
  double phase = 0.0;
  int i;
  for (i = 0; i < frames; i++) {
    int pos = i % cycle;
    double v = 0.0;
    double f0 = TEST_FREQ + 6.0 * sin(2.0 * M_PI * 4.0 * i / sampleRate);
    /* Integrate the frequency so the vibrato does not accumulate phase
       error. */
    phase += 2.0 * M_PI * f0 / sampleRate;
    if (pos < voiced) {
      double env = sin(M_PI * (double)pos / voiced);
      v = env * (0.55 * sin(phase) + 0.30 * sin(2.0 * phase) +
                 0.15 * sin(3.0 * phase));
    }
    out[i] = (short)(v * 9000.0);
  }
}

/* Stereo signal with a fixed ratio between channels: right = 0.4 * left. */
static void genStereoRatio(short* out, int frames, int sampleRate) {
  int i;
  for (i = 0; i < frames; i++) {
    short l = (short)(8000.0 * sin(2.0 * M_PI * TEST_FREQ * i / sampleRate));
    out[2 * i] = l;
    out[2 * i + 1] = (short)(0.4 * l);
  }
}

/* ------------------------------------------------------------------ */
/* Independent duration computation                                    */
/* ------------------------------------------------------------------ */

/* Speed of the track at source frame f (linear between nodes, last speed
   held past the final node). */
static double speedAtPt(const SonicSpeedPoint* p, int n, double f) {
  int i;
  if (f <= (double)p[0].inputFrame) {
    return p[0].speed;
  }
  for (i = 0; i + 1 < n; i++) {
    if (f < (double)p[i + 1].inputFrame) {
      double t = (f - (double)p[i].inputFrame) /
                 (double)(p[i + 1].inputFrame - p[i].inputFrame);
      return p[i].speed + t * (p[i + 1].speed - p[i].speed);
    }
  }
  return p[n - 1].speed;
}

/* Expected number of output frames for inputFrames source frames: the
   integral of 1/speed over the source frames, computed analytically per
   track segment. */
static double expectedFrames(const SonicSpeedPoint* p, int n,
                             double inputFrames) {
  double total = 0.0;
  double pos = 0.0;
  while (pos < inputFrames) {
    double next = inputFrames;
    double s0, s1;
    int i;
    for (i = 0; i < n; i++) {
      if ((double)p[i].inputFrame > pos && (double)p[i].inputFrame < next) {
        next = (double)p[i].inputFrame;
      }
    }
    s0 = speedAtPt(p, n, pos);
    s1 = speedAtPt(p, n, next);
    if (s0 == s1) {
      total += (next - pos) / s0;
    } else {
      total += (next - pos) * log(s1 / s0) / (s1 - s0);
    }
    pos = next;
  }
  return total;
}

/* The allowed duration error: two maxRequired frames. */
static double durationTol(int sampleRate) {
  int maxPeriod = sampleRate / 65; /* SONIC_MIN_PITCH */
  return 2.0 * 2.0 * maxPeriod;
}

/* ------------------------------------------------------------------ */
/* Steady-state frequency measurement (AMDF on the loudest window)     */
/* ------------------------------------------------------------------ */

static double measureFreq(const short* x, int frames, int stride,
                          int sampleRate) {
  int win = 4096;
  int minLag = sampleRate / 400;
  int maxLag = sampleRate / 65;
  int bestStart = 0;
  double bestEnergy = -1.0;
  int start, lag, i;
  int cnt, bestLag;
  double bestDiff;

  if (frames < win + maxLag) {
    return 0.0;
  }
  for (start = 0; start + win + maxLag <= frames; start += 512) {
    double e = 0.0;
    for (i = 0; i < win; i++) {
      double v = x[(start + i) * stride];
      e += v * v;
    }
    if (e > bestEnergy) {
      bestEnergy = e;
      bestStart = start;
    }
  }
  cnt = win - maxLag;
  bestLag = minLag;
  bestDiff = -1.0;
  for (lag = minLag; lag <= maxLag; lag++) {
    double d = 0.0;
    for (i = 0; i < cnt; i++) {
      d += fabs((double)x[(bestStart + i) * stride] -
                (double)x[(bestStart + i + lag) * stride]);
    }
    d /= cnt;
    if (bestDiff < 0.0 || d < bestDiff) {
      bestDiff = d;
      bestLag = lag;
    }
  }
  return (double)sampleRate / bestLag;
}

/* ------------------------------------------------------------------ */
/* Stream runner: feed input in chunks, read with alternating caps     */
/* ------------------------------------------------------------------ */

static short inBuf[500000];
static short outBufA[520000];
static short outBufB[520000];
static short inStereo[300000];
static short outStereoA[400000];
static short outStereoB[400000];

/* Returns the number of output frames, or a negative value on error. */
static int runTrack(const short* in, int inFrames, int channels,
                    int sampleRate, const SonicSpeedPoint* track,
                    int trackCount, const int* writeChunks, int nWrite,
                    const int* readCaps, int nRead, short* out, int maxOut,
                    int* maxBuffered) {
  sonicStream s = sonicCreateStream(sampleRate, channels);
  int inPos = 0, outPos = 0, wi = 0, ri = 0;

  if (s == NULL) {
    return -1;
  }
  sonicSetQuality(s, 1);
  if (track != NULL && sonicSetSpeedTrack(s, track, trackCount) != 1) {
    sonicDestroyStream(s);
    return -2;
  }
  while (inPos < inFrames) {
    int c = writeChunks[wi++ % nWrite];
    if (c > inFrames - inPos) {
      c = inFrames - inPos;
    }
    if (!sonicWriteShortToStream(s, in + (size_t)inPos * channels, c)) {
      sonicDestroyStream(s);
      return -3;
    }
    inPos += c;
    for (;;) {
      int avail = sonicSamplesAvailable(s);
      int cap;
      if (avail == 0) {
        break;
      }
      if (maxBuffered != NULL && avail > *maxBuffered) {
        *maxBuffered = avail;
      }
      cap = readCaps[ri++ % nRead];
      if (cap > avail) {
        cap = avail;
      }
      if (outPos + cap > maxOut) {
        sonicDestroyStream(s);
        return -4;
      }
      outPos += sonicReadShortFromStream(s, out + (size_t)outPos * channels,
                                         cap);
    }
  }
  if (!sonicFlushStream(s)) {
    sonicDestroyStream(s);
    return -5;
  }
  for (;;) {
    int avail = sonicSamplesAvailable(s);
    int cap;
    if (avail == 0) {
      break;
    }
    cap = readCaps[ri++ % nRead];
    if (cap > avail) {
      cap = avail;
    }
    if (outPos + cap > maxOut) {
      sonicDestroyStream(s);
      return -6;
    }
    outPos +=
        sonicReadShortFromStream(s, out + (size_t)outPos * channels, cap);
  }
  sonicDestroyStream(s);
  return outPos;
}

/* ------------------------------------------------------------------ */
/* Cases                                                               */
/* ------------------------------------------------------------------ */

/* Track validation: malformed tracks are rejected, well-formed ones (with
   boundary values) are accepted, and rejections keep the old config. */
static void testValidation(void) {
  sonicStream s;
  SonicSpeedPoint good[3] = {{0, 1.0f}, {1000, 1.5f}, {2000, 0.75f}};
  SonicSpeedPoint pts[3];
  short buf[100];
  int i;

  BEGIN_CASE("validation of track points and stream state");

  memset(buf, 0, sizeof(buf));

  /* NULL stream. */
  CHECK(sonicSetSpeedTrack(NULL, good, 3) == 0);

  s = sonicCreateStream(16000, 1);
  CHECK(s != NULL);
  /* Default quality is 0: track mode requires quality 1. */
  CHECK(sonicSetSpeedTrack(s, good, 3) == 0);
  sonicSetQuality(s, 1);
  CHECK(sonicSetSpeedTrack(s, good, 3) == 1);

  /* Bad speeds are rejected; the previously installed track is kept. */
  pts[0].inputFrame = 0;
  pts[0].speed = 0.49f;
  CHECK(sonicSetSpeedTrack(s, pts, 1) == 0);
  pts[0].speed = 2.01f;
  CHECK(sonicSetSpeedTrack(s, pts, 1) == 0);
  pts[0].speed = (float)NAN;
  CHECK(sonicSetSpeedTrack(s, pts, 1) == 0);
  pts[0].speed = (float)INFINITY;
  CHECK(sonicSetSpeedTrack(s, pts, 1) == 0);
  pts[0].speed = -1.0f;
  CHECK(sonicSetSpeedTrack(s, pts, 1) == 0);
  /* Bad counts / pointers. */
  CHECK(sonicSetSpeedTrack(s, good, -1) == 0);
  CHECK(sonicSetSpeedTrack(s, NULL, 2) == 0);
  /* First point must be at frame 0. */
  pts[0].inputFrame = 7;
  pts[0].speed = 1.0f;
  CHECK(sonicSetSpeedTrack(s, pts, 1) == 0);
  /* Coordinates must be strictly increasing. */
  pts[0].inputFrame = 0;
  pts[0].speed = 1.0f;
  pts[1].inputFrame = 1000;
  pts[1].speed = 1.2f;
  pts[2].inputFrame = 1000;
  pts[2].speed = 1.3f;
  CHECK(sonicSetSpeedTrack(s, pts, 3) == 0);
  pts[2].inputFrame = 999;
  CHECK(sonicSetSpeedTrack(s, pts, 3) == 0);
  /* Coordinates up to 2^53 are accepted, beyond that rejected. */
  pts[1].inputFrame = SONIC_SPEED_TRACK_MAX_FRAME;
  CHECK(sonicSetSpeedTrack(s, pts, 2) == 1);
  pts[1].inputFrame = SONIC_SPEED_TRACK_MAX_FRAME + 1;
  CHECK(sonicSetSpeedTrack(s, pts, 2) == 0);
  /* Boundary speeds 0.5 and 2.0 are accepted. */
  pts[0].speed = 0.5f;
  CHECK(sonicSetSpeedTrack(s, pts, 1) == 1);
  pts[0].speed = 2.0f;
  CHECK(sonicSetSpeedTrack(s, pts, 1) == 1);
  /* An empty track restores the original mode. */
  CHECK(sonicSetSpeedTrack(s, NULL, 0) == 1);
  sonicSetSpeed(s, 1.5f);
  CHECK(sonicGetSpeed(s) == 1.5f);
  sonicDestroyStream(s);

  /* Unsupported stream configurations are rejected. */
  s = sonicCreateStream(8000, 1);
  sonicSetQuality(s, 1);
  CHECK(sonicSetSpeedTrack(s, good, 3) == 0);
  sonicDestroyStream(s);
  s = sonicCreateStream(44100, 1);
  sonicSetQuality(s, 1);
  CHECK(sonicSetSpeedTrack(s, good, 3) == 0);
  sonicDestroyStream(s);
  s = sonicCreateStream(16000, 3);
  sonicSetQuality(s, 1);
  CHECK(sonicSetSpeedTrack(s, good, 3) == 0);
  sonicDestroyStream(s);
  s = sonicCreateStream(48000, 2);
  sonicSetQuality(s, 1);
  CHECK(sonicSetSpeedTrack(s, good, 3) == 1);
  sonicDestroyStream(s);
  s = sonicCreateStream(16000, 1);
  sonicSetQuality(s, 1);
  sonicSetPitch(s, 1.2f);
  CHECK(sonicSetSpeedTrack(s, good, 3) == 0);
  sonicDestroyStream(s);
  s = sonicCreateStream(16000, 1);
  sonicSetQuality(s, 1);
  sonicSetRate(s, 0.9f);
  CHECK(sonicSetSpeedTrack(s, good, 3) == 0);
  sonicDestroyStream(s);

  /* A track cannot be installed (or removed) once non-empty input was
     written, even after a flush. */
  s = sonicCreateStream(16000, 1);
  sonicSetQuality(s, 1);
  for (i = 0; i < 100; i++) {
    buf[i] = (short)(i * 10);
  }
  CHECK(sonicWriteShortToStream(s, buf, 100) == 1);
  CHECK(sonicSetSpeedTrack(s, good, 3) == 0);
  CHECK(sonicFlushStream(s) == 1);
  CHECK(sonicSetSpeedTrack(s, good, 3) == 0);
  CHECK(sonicSetSpeedTrack(s, NULL, 0) == 0);
  sonicDestroyStream(s);
}

/* While a track is installed, the ordinary speed/pitch/rate/quality/
   sampleRate/numChannels settings are rejected; volume keeps working. */
static void testSetterLockout(void) {
  sonicStream s;
  SonicSpeedPoint track[2] = {{0, 1.0f}, {16000, 1.0f}};
  short out[40000];
  int n, i, maxAbs;

  BEGIN_CASE("setters locked while track active, volume alive");

  s = sonicCreateStream(16000, 1);
  sonicSetQuality(s, 1);
  CHECK(sonicSetSpeedTrack(s, track, 2) == 1);

  sonicSetSpeed(s, 2.0f);
  CHECK(sonicGetSpeed(s) == 1.0f);
  sonicSetPitch(s, 0.5f);
  CHECK(sonicGetPitch(s) == 1.0f);
  sonicSetRate(s, 2.0f);
  CHECK(sonicGetRate(s) == 1.0f);
  sonicSetQuality(s, 0);
  CHECK(sonicGetQuality(s) == 1);
  sonicSetSampleRate(s, 8000);
  CHECK(sonicGetSampleRate(s) == 16000);
  sonicSetNumChannels(s, 2);
  CHECK(sonicGetNumChannels(s) == 1);
  sonicSetVolume(s, 0.5f);
  CHECK(sonicGetVolume(s) == 0.5f);

  /* The stream still works, and the volume setting really scales the
     output of this unity track. */
  genSine(inBuf, 16000, 16000, 9000.0);
  CHECK(sonicWriteShortToStream(s, inBuf, 16000) == 1);
  CHECK(sonicFlushStream(s) == 1);
  n = 0;
  for (;;) {
    int got = sonicReadShortFromStream(s, out + n, 40000 - n);
    if (got <= 0) {
      break;
    }
    n += got;
  }
  CHECK(n == 16000);
  maxAbs = 0;
  for (i = 0; i < n; i++) {
    int v = out[i] < 0 ? -out[i] : out[i];
    if (v > maxAbs) {
      maxAbs = v;
    }
  }
  CHECK(maxAbs >= 4000 && maxAbs <= 5000);
  sonicDestroyStream(s);
}

/* An all-1.0 track must produce exactly the original unit-speed output. */
static void testUnityTrack(void) {
  SonicSpeedPoint one[1] = {{0, 1.0f}};
  SonicSpeedPoint three[3] = {{0, 1.0f}, {10000, 1.0f}, {20000, 1.0f}};
  int chunks[1] = {997};
  int caps[2] = {13, 100000};
  int nA, nB, nC;

  BEGIN_CASE("all-1.0 track matches plain unit-speed output");

  genSine(inBuf, 48000, 16000, 10000.0);
  nA = runTrack(inBuf, 48000, 1, 16000, one, 1, chunks, 1, caps, 2, outBufA,
                520000, NULL);
  nB = runTrack(inBuf, 48000, 1, 16000, NULL, 0, chunks, 1, caps, 2, outBufB,
                520000, NULL);
  nC = runTrack(inBuf, 48000, 1, 16000, three, 3, chunks, 1, caps, 2,
                outStereoA, 400000, NULL);
  CHECK(nA == 48000);
  CHECK(nB == 48000);
  CHECK(nC == 48000);
  CHECK(nA == nB && memcmp(outBufA, outBufB, nA * sizeof(short)) == 0);
  CHECK(nA == nC && memcmp(outBufA, outStereoA, nA * sizeof(short)) == 0);
}

/* Fast first, then gradually back to normal speed (the pause-skipping use
   case), checked against the independent duration and pitch measurements. */
static void testFastThenSlow(void) {
  SonicSpeedPoint track[7] = {{0, 2.0f},     {8000, 2.0f},  {12000, 1.0f},
                              {36000, 1.0f}, {40000, 2.0f}, {56000, 2.0f},
                              {60000, 1.0f}};
  int chunks[3] = {113, 7, 1};
  int caps[2] = {9, 100000};
  int inFrames = 70400;
  int n;
  double expect, freq;

  BEGIN_CASE("fast then slow speech-shaped signal");

  genSpeech(inBuf, inFrames, 16000);
  n = runTrack(inBuf, inFrames, 1, 16000, track, 7, chunks, 3, caps, 2,
               outBufA, 520000, NULL);
  CHECK(n > 0);
  expect = expectedFrames(track, 7, inFrames);
  CHECK(fabs(n - expect) <= durationTol(16000));
  /* The 220 Hz voice must not be shifted to a multiple of itself. */
  freq = measureFreq(outBufA, n, 1, 16000);
  CHECK(freq > 200.0 && freq < 245.0);
}

/* Dense nodes: many closely spaced speed changes, with single writes
   crossing many nodes at once. */
static void testDenseNodes(void) {
  static SonicSpeedPoint track[112];
  float speeds[8] = {1.0f, 1.4f, 1.1f, 1.8f, 0.9f, 1.0f, 1.6f, 0.8f};
  int chunks[1] = {4096};
  int caps[2] = {11, 200000};
  int inFrames = 35200;
  int count = 0;
  uint64_t f;
  int n, i;
  double expect, freq;

  BEGIN_CASE("dense nodes crossed by single writes");

  for (f = 0, i = 0; f <= (uint64_t)inFrames; f += 320, i++) {
    track[count].inputFrame = f;
    track[count].speed = speeds[i % 8];
    count++;
  }
  genSpeech(inBuf, inFrames, 16000);
  n = runTrack(inBuf, inFrames, 1, 16000, track, count, chunks, 1, caps, 2,
               outBufA, 520000, NULL);
  CHECK(n > 0);
  expect = expectedFrames(track, count, inFrames);
  CHECK(fabs(n - expect) <= durationTol(16000));
  freq = measureFreq(outBufA, n, 1, 16000);
  CHECK(freq > 200.0 && freq < 245.0);
}

/* Very short tails: a final segment only a few frames long, an input
   shorter than one analysis window, and an input ending mid-ramp. */
static void testShortTail(void) {
  SonicSpeedPoint tailTrack[3] = {{0, 1.0f}, {12000, 1.7f}, {19997, 0.6f}};
  SonicSpeedPoint tinyTrack[1] = {{0, 1.5f}};
  SonicSpeedPoint rampTrack[2] = {{0, 1.0f}, {100000, 2.0f}};
  int chunks[1] = {500};
  int caps[2] = {7, 50000};
  int inFrames = 20000;
  int n;
  double expect;

  BEGIN_CASE("very short tail segments");

  /* The last track segment is only 3 frames long. */
  genSpeech(inBuf, inFrames, 16000);
  n = runTrack(inBuf, inFrames, 1, 16000, tailTrack, 3, chunks, 1, caps, 2,
               outBufA, 520000, NULL);
  CHECK(n > 0);
  expect = expectedFrames(tailTrack, 3, inFrames);
  CHECK(fabs(n - expect) <= durationTol(16000));

  /* An input shorter than one analysis window still flushes out. */
  genSine(inBuf, 300, 16000, 10000.0);
  n = runTrack(inBuf, 300, 1, 16000, tinyTrack, 1, chunks, 1, caps, 2,
               outBufA, 520000, NULL);
  CHECK(n > 0);
  expect = expectedFrames(tinyTrack, 1, 300);
  CHECK(fabs(n - expect) <= durationTol(16000));

  /* The recording ends in the middle of a long ramp. */
  genSpeech(inBuf, inFrames, 16000);
  n = runTrack(inBuf, inFrames, 1, 16000, rampTrack, 2, chunks, 1, caps, 2,
               outBufA, 520000, NULL);
  CHECK(n > 0);
  expect = expectedFrames(rampTrack, 2, inFrames);
  CHECK(fabs(n - expect) <= durationTol(16000));
}

/* Two chunkings: the same recording fed as alternating 1/7/113-frame writes
   versus 4096-frame writes, with read capacities alternating between very
   small and very large, must produce the same total length and (within one
   integer unit) the same samples. */
static void testChunking(void) {
  SonicSpeedPoint track[6] = {{0, 1.0f},      {24000, 1.8f}, {48000, 1.8f},
                              {72000, 0.75f}, {96000, 1.0f}, {120000, 1.5f}};
  int chunksA[3] = {1, 7, 113};
  int capsA[3] = {5, 3, 100000};
  int chunksB[1] = {4096};
  int capsB[2] = {17, 500000};
  int inFrames = 144000;
  int nA, nB, i;
  int maxDiff = 0;
  double expect;

  BEGIN_CASE("two chunkings give the same sound");

  genStereoRatio(inStereo, inFrames, 48000);
  nA = runTrack(inStereo, inFrames, 2, 48000, track, 6, chunksA, 3, capsA, 3,
                outStereoA, 400000, NULL);
  nB = runTrack(inStereo, inFrames, 2, 48000, track, 6, chunksB, 1, capsB, 2,
                outStereoB, 400000, NULL);
  CHECK(nA > 0);
  CHECK(nA == nB);
  for (i = 0; i < nA * 2; i++) {
    int d = outStereoA[i] - outStereoB[i];
    if (d < 0) {
      d = -d;
    }
    if (d > maxDiff) {
      maxDiff = d;
    }
  }
  CHECK(maxDiff <= 1);
  expect = expectedFrames(track, 6, inFrames);
  CHECK(fabs(nA - expect) <= durationTol(48000));
}

/* Streaming: a long recording produces output before flush, and the PCM
   buffered inside the stream stays bounded when read promptly. */
static void testStreaming(void) {
  SonicSpeedPoint track[5] = {{0, 2.0f},      {48000, 2.0f}, {56000, 1.0f},
                              {400000, 1.0f}, {408000, 0.6f}};
  int chunks[1] = {997};
  int caps[1] = {1000000};
  int inFrames = 480000;
  int maxBuffered = 0;
  int n;
  sonicStream s;
  int inPos = 0, outPos = 0;

  BEGIN_CASE("long recording streams with bounded buffering");

  genSpeech(inBuf, inFrames, 16000);

  /* Output must be available before the flush. */
  s = sonicCreateStream(16000, 1);
  sonicSetQuality(s, 1);
  CHECK(sonicSetSpeedTrack(s, track, 5) == 1);
  while (inPos < inFrames) {
    int c = 997;
    if (c > inFrames - inPos) {
      c = inFrames - inPos;
    }
    CHECK(sonicWriteShortToStream(s, inBuf + inPos, c) == 1);
    inPos += c;
    outPos += sonicReadShortFromStream(s, outBufA + outPos, 520000 - outPos);
  }
  CHECK(outPos > 0);
  CHECK(sonicFlushStream(s) == 1);
  sonicDestroyStream(s);

  /* With prompt reads, the buffered PCM does not grow with the recording
     length (480000 frames in, always far less than 20000 frames buffered). */
  n = runTrack(inBuf, inFrames, 1, 16000, track, 5, chunks, 1, caps, 1,
               outBufA, 520000, &maxBuffered);
  CHECK(n > 0);
  CHECK(maxBuffered > 0);
  CHECK(maxBuffered < 20000);
  CHECK(fabs(n - expectedFrames(track, 5, inFrames)) <= durationTol(16000));
}

/* After a flush the stream can be reused; the same track is applied to the
   next segment starting again at source frame 0. */
static void testReuse(void) {
  SonicSpeedPoint track[3] = {{0, 2.0f}, {4800, 2.0f}, {7200, 1.0f}};
  sonicStream s;
  int inFrames = 17600;
  int n1 = 0, n2 = 0, got, i, maxDiff = 0;
  double expect;

  BEGIN_CASE("flush then reuse restarts the track at frame 0");

  genSpeech(inBuf, inFrames, 16000);
  s = sonicCreateStream(16000, 1);
  sonicSetQuality(s, 1);
  CHECK(sonicSetSpeedTrack(s, track, 3) == 1);

  CHECK(sonicWriteShortToStream(s, inBuf, inFrames) == 1);
  CHECK(sonicFlushStream(s) == 1);
  while ((got = sonicReadShortFromStream(s, outBufA + n1, 520000 - n1)) > 0) {
    n1 += got;
  }
  /* Reuse the same stream for a second, identical recording. */
  CHECK(sonicWriteShortToStream(s, inBuf, inFrames) == 1);
  CHECK(sonicFlushStream(s) == 1);
  while ((got = sonicReadShortFromStream(s, outBufB + n2, 520000 - n2)) > 0) {
    n2 += got;
  }
  sonicDestroyStream(s);

  CHECK(n1 > 0);
  CHECK(n1 == n2);
  for (i = 0; i < n1; i++) {
    int d = outBufA[i] - outBufB[i];
    if (d < 0) {
      d = -d;
    }
    if (d > maxDiff) {
      maxDiff = d;
    }
  }
  CHECK(maxDiff <= 1);
  /* The track really restarted at frame 0: the duration matches the
     integral over one segment, not two. */
  expect = expectedFrames(track, 3, inFrames);
  CHECK(fabs(n1 - expect) <= durationTol(16000));
}

/* Stereo: both channels share the same overlap/splice positions, so a fixed
   channel ratio is preserved through the time stretch. */
static void testStereoRatio(void) {
  SonicSpeedPoint track[3] = {{0, 1.5f}, {32000, 1.5f}, {64000, 0.8f}};
  int chunks[1] = {2048};
  int caps[2] = {3, 250000};
  int inFrames = 96000;
  int n, i;
  double ratioErr = 0.0;
  double expect, freq;

  BEGIN_CASE("stereo fixed-ratio signal keeps both channels aligned");

  genStereoRatio(inStereo, inFrames, 48000);
  n = runTrack(inStereo, inFrames, 2, 48000, track, 3, chunks, 1, caps, 2,
               outStereoA, 400000, NULL);
  CHECK(n > 0);
  for (i = 0; i < n; i++) {
    double l = outStereoA[2 * i];
    double r = outStereoA[2 * i + 1];
    double e = fabs(r - 0.4 * l);
    if (e > ratioErr) {
      ratioErr = e;
    }
  }
  CHECK(ratioErr <= 2.0);
  expect = expectedFrames(track, 3, inFrames);
  CHECK(fabs(n - expect) <= durationTol(48000));
  freq = measureFreq(outStereoA, n, 2, 48000);
  CHECK(freq > 200.0 && freq < 245.0);
}

int main(void) {
  testValidation();
  testSetterLockout();
  testUnityTrack();
  testFastThenSlow();
  testDenseNodes();
  testShortTail();
  testChunking();
  testStreaming();
  testReuse();
  testStereoRatio();

  if (gFailures == 0 && gCases > 0) {
    printf("SONIC_SPEED_TRACK_OK cases=%d\n", gCases);
    return 0;
  }
  fprintf(stderr, "speed track tests failed: %d failures over %d cases\n",
          gFailures, gCases);
  return 1;
}
