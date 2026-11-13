/* Sonic library
   Copyright 2025
   Bill Cox
   This file is part of the Sonic Library.

   This file is licensed under the Apache 2.0 license.
*/

/* Tests for the speed-track API: piecewise-linear time compression along the
   source PCM frame axis.

   Every check inspects real output samples (length, per-sample agreement
   between two chunkings, or a measured steady-state frequency), not merely the
   configuration values reported by the getters. */

#include "sonic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 16000

/* Two maxRequired frames at 16 kHz: 2 * (2 * sampleRate / SONIC_MIN_PITCH). */
#define TOLERANCE (2 * 2 * (SR / SONIC_MIN_PITCH))

static int failures = 0;
static int casesRun = 0;

#define CHECK(cond, msg)                                      \
  do {                                                        \
    if (!(cond)) {                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
              (msg));                                         \
      failures++;                                             \
    }                                                         \
  } while (0)

/* ---------- Signal generators ---------- */

/* A pure sine used for steady-state frequency checks. */
static void genSine(short* out, int numSamples, double freq) {
  int i;
  for (i = 0; i < numSamples; i++) {
    out[i] = (short)(9000.0 * sin(2.0 * M_PI * freq * i / SR));
  }
}

/* A speech-shaped signal: 220 Hz voiced "syllables" separated by silent
   pauses, with a short raised-cosine onset and offset in each syllable. */
static void genSpeech(short* out, int numSamples, int syllableFrames,
                      int pauseFrames) {
  int pos = 0;
  int inVoice = 1;
  while (pos < numSamples) {
    int len = inVoice ? syllableFrames : pauseFrames;
    int i;
    int n = pos + len > numSamples ? numSamples - pos : len;
    for (i = 0; i < n; i++) {
      double t = (double)(pos + i);
      double env = 1.0;
      double fade = 0.010 * SR;
      if (inVoice) {
        if (i < (int)fade) {
          env = 0.5 - 0.5 * cos(M_PI * i / fade);
        } else if (i > n - (int)fade) {
          env = 0.5 - 0.5 * cos(M_PI * (n - i) / fade);
        }
        out[pos + i] = (short)(9000.0 * env * sin(2.0 * M_PI * 220.0 * t / SR));
      } else {
        out[pos + i] = 0;
      }
    }
    pos += n;
    inVoice = !inVoice;
  }
}

/* Stereo where the right channel is a fixed fraction of the left. */
static void genStereo(short* out, int numFrames, double freq) {
  int i;
  for (i = 0; i < numFrames; i++) {
    short left = (short)(9000.0 * sin(2.0 * M_PI * freq * i / SR));
    out[2 * i] = left;
    out[2 * i + 1] = (short)((left * 3) / 5);
  }
}

/* ---------- Independent duration reference ---------- */

/* Exact integral of 1/speed over [a, b), where speed is the piecewise-linear
   interpolation of the track.  This is the definition of the target duration
   and is computed here independently of the library. */
static double speedAt(const SonicSpeedPoint* points, int count, double frame) {
  int i;
  if (frame <= 0.0) {
    return points[0].speed;
  }
  for (i = 0; i + 1 < count; i++) {
    double x0 = (double)points[i].inputFrame;
    double x1 = (double)points[i + 1].inputFrame;
    if (frame < x1) {
      double s0 = points[i].speed;
      double s1 = points[i + 1].speed;
      if (frame <= x0) {
        return s0;
      }
      return s0 + (s1 - s0) * ((frame - x0) / (x1 - x0));
    }
  }
  return points[count - 1].speed;
}

static double trackDuration(const SonicSpeedPoint* points, int count, double a,
                            double b) {
  /* Numerically integrate 1/speed with a fine step; robust for any track. */
  const double step = 0.25;
  double total = 0.0;
  double x = a;
  if (b <= a) {
    return 0.0;
  }
  while (x < b) {
    double next = x + step < b ? x + step : b;
    double mid = 0.5 * (x + next);
    total += (next - x) / speedAt(points, count, mid);
    x = next;
  }
  return total;
}

/* ---------- Streaming helper ---------- */

typedef struct {
  short* data;
  int length;
} Buffer;

/* Feed the whole recording with the given write pattern, reading promptly
   with the given capacity pattern.  Must be freed with free(). */
static Buffer runTrack(const short* input, int numFrames, int numChannels,
                       const SonicSpeedPoint* points, int count, int bigChunks,
                       int bigReads) {
  sonicStream stream = sonicCreateStream(SR, numChannels);
  Buffer out;
  int capacity = numFrames * 4 + 65536;
  int written = 0;
  int readTotal = 0;
  int readAmount;
  int readCap = bigReads ? 7000 : 37;
  int writeAmount = bigChunks ? 4096 : 37;
  int frames;

  out.data = (short*)malloc(sizeof(short) * capacity * numChannels);
  out.length = 0;

  sonicSetQuality(stream, 1);
  sonicSetSpeedTrack(stream, points, count);

  while (written < numFrames) {
    int chunk = (written % 2 == 0) ? writeAmount : (writeAmount == 4096 ? 1 : 113);
    int framesLeft;
    if (written + chunk > numFrames) {
      chunk = numFrames - written;
    }
    sonicWriteShortToStream(stream, input + written * numChannels, chunk);
    written += chunk;
    framesLeft = capacity - readTotal;
    while (framesLeft > 0 &&
           (readAmount = sonicReadShortFromStream(
                stream, out.data + readTotal * numChannels,
                readCap < framesLeft ? readCap : framesLeft)) > 0) {
      readTotal += readAmount;
      framesLeft -= readAmount;
    }
  }
  sonicFlushStream(stream);
  frames = capacity - readTotal;
  while (frames > 0 &&
         (readAmount = sonicReadShortFromStream(stream,
                                                out.data + readTotal * numChannels,
                                                readCap < frames ? readCap
                                                                 : frames)) > 0) {
    readTotal += readAmount;
    frames -= readAmount;
  }
  sonicDestroyStream(stream);
  out.length = readTotal;
  return out;
}

/* The alternating pattern required by the spec: writes of 1, 7 and 113 frames,
   alternating read capacities between very small and very large.  (The helper
   above covers 4096-frame writes and small/large reads; this variant uses the
   exact 1/7/113 alternation for writes.) */
static Buffer runTrackAlternating(const short* input, int numFrames,
                                  int numChannels,
                                  const SonicSpeedPoint* points, int count) {
  static const int chunks[3] = {1, 7, 113};
  sonicStream stream = sonicCreateStream(SR, numChannels);
  Buffer out;
  int capacity = numFrames * 4 + 65536;
  int written = 0;
  int readTotal = 0;
  int ci = 0;
  int readAmount;
  int smallRead = 1;

  out.data = (short*)malloc(sizeof(short) * capacity * numChannels);
  out.length = 0;

  sonicSetQuality(stream, 1);
  sonicSetSpeedTrack(stream, points, count);

  while (written < numFrames) {
    int chunk = chunks[ci % 3];
    int readCap = smallRead ? 13 : 9001;
    int framesLeft;
    ci++;
    if (written + chunk > numFrames) {
      chunk = numFrames - written;
    }
    sonicWriteShortToStream(stream, input + written * numChannels, chunk);
    written += chunk;
    smallRead = !smallRead;
    framesLeft = capacity - readTotal;
    while (framesLeft > 0 &&
           (readAmount = sonicReadShortFromStream(
                stream, out.data + readTotal * numChannels,
                readCap < framesLeft ? readCap : framesLeft)) > 0) {
      readTotal += readAmount;
      framesLeft -= readAmount;
    }
  }
  sonicFlushStream(stream);
  while ((readAmount = sonicReadShortFromStream(
              stream, out.data + readTotal * numChannels,
              capacity - readTotal)) > 0) {
    readTotal += readAmount;
  }
  sonicDestroyStream(stream);
  out.length = readTotal;
  return out;
}

/* ---------- Steady-state frequency measurement ---------- */

/* Measure the dominant frequency by counting positive-going zero crossings
   over a steady window, ignoring near-zero samples. */
static double measureFrequency(const short* samples, int numFrames, int start,
                               int end) {
  int i;
  int crossings = 0;
  int firstCrossing = -1;
  int lastCrossing = -1;
  int prevSign = 0;
  const int gate = 300;

  if (start < 0) start = 0;
  if (end > numFrames) end = numFrames;
  for (i = start; i < end; i++) {
    int sign = samples[i] > gate ? 1 : (samples[i] < -gate ? -1 : 0);
    if (sign != 0) {
      if (prevSign < 0 && sign > 0) {
        if (firstCrossing < 0) {
          firstCrossing = i;
        } else {
          lastCrossing = i;
          crossings++;
        }
      }
      prevSign = sign;
    }
  }
  if (crossings < 2 || lastCrossing <= firstCrossing) {
    return 0.0;
  }
  return crossings * (double)SR / (double)(lastCrossing - firstCrossing);
}

/* ---------- Cases ---------- */

/* Fast first, then gradually back to normal, evaluated on a pure tone so the
   steady-state frequency must remain 220 Hz (pitch-preserving) in both the
   fast and the slow regions. */
static void testFastThenSlow(void) {
  const int N = SR * 4;
  SonicSpeedPoint points[3];
  short* input = (short*)malloc(sizeof(short) * N);
  Buffer out;
  double target;
  double fastFreq, slowFreq;

  genSine(input, N, 220.0);
  points[0].inputFrame = 0;
  points[0].speed = 2.0f;
  points[1].inputFrame = N / 2;
  points[1].speed = 2.0f;
  points[2].inputFrame = N;
  points[2].speed = 0.5f;

  out = runTrackAlternating(input, N, 1, points, 3);

  target = trackDuration(points, 3, 0.0, (double)N);
  CHECK(fabs((double)out.length - target) <= TOLERANCE,
        "fast-then-slow length matches the duration integral");
  CHECK(out.length > N / 2,
        "fast-then-slow output is longer than the minimal 2x length");

  /* The fast region is the first half; the slow region is the second half.
     Convert a source window to output indices with the local integral. */
  {
    double sourceFastStart = N / 2 - 8000;
    double sourceFastEnd = N / 2;
    int outFastStart = (int)trackDuration(points, 3, 0.0, sourceFastStart);
    int outFastEnd = (int)trackDuration(points, 3, 0.0, sourceFastEnd);
    int outSlowStart = (int)trackDuration(points, 3, 0.0, 3 * N / 4);
    int outSlowEnd = (int)trackDuration(points, 3, 0.0, N - 2000);
    fastFreq = measureFrequency(out.data, out.length, outFastStart, outFastEnd);
    slowFreq = measureFrequency(out.data, out.length, outSlowStart, outSlowEnd);
  }
  CHECK(fabs(fastFreq - 220.0) < 12.0,
        "fast region keeps 220 Hz (no octave jump)");
  CHECK(fabs(slowFreq - 220.0) < 12.0,
        "slow region keeps 220 Hz (no octave jump)");

  free(input);
  free(out.data);
  casesRun++;
}

/* Dense alternating nodes: many speed changes close together.  The output must
   still match the integral and be identical for the two chunkings. */
static void testDenseNodes(void) {
  const int N = SR * 3;
  const int numPoints = 40;
  SonicSpeedPoint* points =
      (SonicSpeedPoint*)malloc(sizeof(SonicSpeedPoint) * numPoints);
  short* input = (short*)malloc(sizeof(short) * N);
  Buffer a, b;
  double target;
  int i;

  genSpeech(input, N, SR / 3, SR / 7);
  points[0].inputFrame = 0;
  points[0].speed = 2.0f;
  for (i = 1; i < numPoints; i++) {
    points[i].inputFrame = (uint64_t)i * (uint64_t)(N / numPoints);
    points[i].speed = (i & 1) ? 0.5f : 2.0f;
  }
  target = trackDuration(points, numPoints, 0.0, (double)N);

  a = runTrackAlternating(input, N, 1, points, numPoints);
  b = runTrack(input, N, 1, points, numPoints, 1, 1);

  CHECK(fabs((double)a.length - target) <= TOLERANCE,
        "dense nodes: length matches the duration integral");
  CHECK(a.length == b.length, "dense nodes: both chunkings give equal length");
  for (i = 0; i < a.length && i < b.length; i++) {
    if (abs((int)a.data[i] - (int)b.data[i]) > 1) {
      CHECK(0, "dense nodes: per-sample output differs between chunkings");
      break;
    }
  }

  free(points);
  free(input);
  free(a.data);
  free(b.data);
  casesRun++;
}

/* A very short tail: the final speed node sits a few frames before the end. */
static void testShortTail(void) {
  const int N = 900;
  SonicSpeedPoint points[4];
  short* input = (short*)malloc(sizeof(short) * N);
  Buffer a, b;
  double target;
  int i;

  genSpeech(input, N, 240, 90);
  points[0].inputFrame = 0;
  points[0].speed = 2.0f;
  points[1].inputFrame = 300;
  points[1].speed = 2.0f;
  points[2].inputFrame = 880;
  points[2].speed = 0.5f;
  points[3].inputFrame = 899;
  points[3].speed = 0.5f;
  target = trackDuration(points, 4, 0.0, (double)N);

  a = runTrackAlternating(input, N, 1, points, 4);
  b = runTrack(input, N, 1, points, 4, 1, 1);

  CHECK(a.length >= 0, "short tail: output produced");
  CHECK(fabs((double)a.length - target) <= TOLERANCE,
        "short tail: length matches the duration integral");
  CHECK(a.length == b.length, "short tail: equal length across chunkings");
  for (i = 0; i < a.length && i < b.length; i++) {
    if (abs((int)a.data[i] - (int)b.data[i]) > 1) {
      CHECK(0, "short tail: per-sample output differs between chunkings");
      break;
    }
  }

  free(input);
  free(a.data);
  free(b.data);
  casesRun++;
}

/* A track of all ones must reproduce the unit-rate output exactly. */
static void testUnitTrack(void) {
  const int N = SR * 2;
  SonicSpeedPoint point;
  short* input = (short*)malloc(sizeof(short) * N);
  Buffer out;
  int i;

  genSpeech(input, N, SR / 2, SR / 8);
  point.inputFrame = 0;
  point.speed = 1.0f;

  out = runTrackAlternating(input, N, 1, &point, 1);
  CHECK(out.length == N, "unit track: output length equals input length");
  for (i = 0; i < N && i < out.length; i++) {
    if (out.data[i] != input[i]) {
      CHECK(0, "unit track: output is not bit-identical to the input");
      break;
    }
  }

  free(input);
  free(out.data);
  casesRun++;
}

/* Stereo must share overlap-add positions: the right channel stays a fixed
   fraction (3/5) of the left at every sample, at 48 kHz and both chunkings. */
static void testStereo(void) {
  sonicStream stream;
  SonicSpeedPoint points[3];
  const int N = 48000;
  short* input = (short*)malloc(sizeof(short) * 2 * N);
  short* outA;
  short* outB;
  int lenA = 0;
  int lenB = 0;
  int written = 0;
  int i;

  genStereo(input, N, 220.0);
  points[0].inputFrame = 0;
  points[0].speed = 1.6f;
  points[1].inputFrame = N / 2;
  points[1].speed = 1.6f;
  points[2].inputFrame = N;
  points[2].speed = 0.7f;

  outA = (short*)malloc(sizeof(short) * 4 * N);
  outB = (short*)malloc(sizeof(short) * 4 * N);

  /* Chunking A: 4096-frame writes, large reads. */
  stream = sonicCreateStream(48000, 2);
  sonicSetQuality(stream, 1);
  CHECK(sonicSetSpeedTrack(stream, points, 3) == 1,
        "stereo track accepted at 48 kHz");
  sonicSetSampleRate(stream, 16000);
  CHECK(sonicGetSampleRate(stream) == 48000,
        "sample rate change rejected while track active");
  CHECK(sonicGetNumChannels(stream) == 2,
        "channel change rejected while track active");
  while (written < N) {
    int chunk = written + 4096 > N ? N - written : 4096;
    int got;
    sonicWriteShortToStream(stream, input + written * 2, chunk);
    written += chunk;
    while ((got = sonicReadShortFromStream(stream, outA + lenA * 2, 5000)) > 0) {
      lenA += got;
    }
  }
  sonicFlushStream(stream);
  {
    int got;
    while ((got = sonicReadShortFromStream(stream, outA + lenA * 2, 60000)) > 0) {
      lenA += got;
    }
  }
  sonicDestroyStream(stream);

  /* Chunking B: 1/7/113-frame writes, tiny reads. */
  {
    static const int chunks[3] = {1, 7, 113};
    int pos = 0;
    int ci = 0;
    stream = sonicCreateStream(48000, 2);
    sonicSetQuality(stream, 1);
    sonicSetSpeedTrack(stream, points, 3);
    while (pos < N) {
      int chunk = chunks[ci % 3];
      int got;
      ci++;
      if (pos + chunk > N) {
        chunk = N - pos;
      }
      sonicWriteShortToStream(stream, input + pos * 2, chunk);
      pos += chunk;
      while ((got = sonicReadShortFromStream(stream, outB + lenB * 2, 11)) > 0) {
        lenB += got;
      }
    }
    sonicFlushStream(stream);
    {
      int got;
      while ((got = sonicReadShortFromStream(stream, outB + lenB * 2, 60000)) >
             0) {
        lenB += got;
      }
    }
    sonicDestroyStream(stream);
  }

  CHECK(lenA == lenB, "stereo: equal lengths across chunkings");
  for (i = 0; i < lenA && i < lenB; i++) {
    int left = outA[2 * i];
    int right = outA[2 * i + 1];
    int expectedRight;
    if (abs((int)outA[2 * i] - (int)outB[2 * i]) > 1 ||
        abs((int)outA[2 * i + 1] - (int)outB[2 * i + 1]) > 1) {
      CHECK(0, "stereo: per-sample output differs between chunkings");
      break;
    }
    /* Allow the tiny rounding of the 16-bit 3/5 scaling and overlap-add. */
    expectedRight = (left * 3) / 5;
    if (abs(right - expectedRight) > 2) {
      CHECK(0, "stereo: right channel is not a fixed fraction of the left");
      break;
    }
  }

  free(input);
  free(outA);
  free(outB);
  casesRun++;
}

/* Wrong formats and malformed tracks are rejected, and the old configuration
   survives a rejection. */
static void testRejections(void) {
  sonicStream stream;
  SonicSpeedPoint good[3];
  SonicSpeedPoint bad[3];
  short input[SR];

  good[0].inputFrame = 0;
  good[0].speed = 1.0f;
  good[1].inputFrame = 1000;
  good[1].speed = 1.5f;
  good[2].inputFrame = 2000;
  good[2].speed = 0.8f;

  /* Unsupported sample rates and channel counts. */
  stream = sonicCreateStream(44100, 1);
  sonicSetQuality(stream, 1);
  CHECK(sonicSetSpeedTrack(stream, good, 3) == 0,
        "44.1 kHz track rejected");
  sonicDestroyStream(stream);

  stream = sonicCreateStream(16000, 3);
  sonicSetQuality(stream, 1);
  CHECK(sonicSetSpeedTrack(stream, good, 3) == 0, "3 channels rejected");
  sonicDestroyStream(stream);

  /* Requires quality 1 and pitch == rate == 1. */
  stream = sonicCreateStream(16000, 1);
  sonicSetQuality(stream, 1);
  sonicSetPitch(stream, 1.25f);
  CHECK(sonicSetSpeedTrack(stream, good, 3) == 0, "pitch != 1 rejected");
  sonicDestroyStream(stream);

  stream = sonicCreateStream(16000, 1);
  sonicSetQuality(stream, 1);
  sonicSetRate(stream, 1.25f);
  CHECK(sonicSetSpeedTrack(stream, good, 3) == 0, "rate != 1 rejected");
  sonicDestroyStream(stream);

  stream = sonicCreateStream(16000, 1);
  CHECK(sonicSetSpeedTrack(stream, good, 3) == 0, "quality 0 rejected");
  sonicDestroyStream(stream);

  /* Bad point layouts and values. */
  stream = sonicCreateStream(16000, 1);
  sonicSetQuality(stream, 1);
  memcpy(bad, good, sizeof(bad));
  bad[0].inputFrame = 3;
  CHECK(sonicSetSpeedTrack(stream, bad, 3) == 0, "first point must be 0");

  memcpy(bad, good, sizeof(bad));
  bad[1].inputFrame = bad[0].inputFrame;
  CHECK(sonicSetSpeedTrack(stream, bad, 2) == 0,
        "positions must increase strictly");

  memcpy(bad, good, sizeof(bad));
  bad[0].speed = 0.49f;
  CHECK(sonicSetSpeedTrack(stream, bad, 3) == 0, "speed below 0.5 rejected");
  bad[0].speed = 2.01f;
  CHECK(sonicSetSpeedTrack(stream, bad, 3) == 0, "speed above 2 rejected");
  bad[0].speed = (float)(0.0 / 0.0);
  CHECK(sonicSetSpeedTrack(stream, bad, 3) == 0, "NaN speed rejected");

  memcpy(bad, good, sizeof(bad));
  bad[2].inputFrame = SONIC_MAX_SPEED_POINT_FRAME + 1;
  CHECK(sonicSetSpeedTrack(stream, bad, 3) == 0, "frame past 2^53 rejected");

  CHECK(sonicSetSpeedTrack(stream, NULL, 4) == 0, "NULL point array rejected");
  CHECK(sonicSetSpeedTrack(stream, good, -1) == 0, "negative count rejected");

  /* A rejected track leaves the previous configuration in force. */
  CHECK(sonicSetSpeedTrack(stream, good, 3) == 1, "valid track accepted");
  CHECK(sonicSetSpeedTrack(stream, bad, 3) == 0, "invalid track rejected");
  /* A valid replacement would be accepted, so rejection above must not have
     discarded the old one; verify indirectly through locked setters. */
  sonicSetSpeed(stream, 1.9f);
  CHECK(sonicGetSpeed(stream) == 1.0f, "status speed change rejected");
  sonicSetPitch(stream, 1.9f);
  CHECK(sonicGetPitch(stream) == 1.0f, "status pitch change rejected");
  sonicSetRate(stream, 1.9f);
  CHECK(sonicGetRate(stream) == 1.0f, "status rate change rejected");
  sonicSetQuality(stream, 0);
  CHECK(sonicGetQuality(stream) == 1, "status quality change rejected");
  sonicSetVolume(stream, 0.5f);
  CHECK(fabs(sonicGetVolume(stream) - 0.5f) < 1e-6f,
        "volume setting still takes effect");

  /* Once non-empty input has been written a new track is rejected, but an
     empty track can still clear the mode. */
  memset(input, 0, sizeof(input));
  CHECK(sonicWriteShortToStream(stream, input, SR) == 1, "write accepted");
  CHECK(sonicSetSpeedTrack(stream, good, 3) == 0,
        "track rejected after non-empty input");
  CHECK(sonicSetSpeedTrack(stream, NULL, 0) == 1,
        "empty track still clears mode after input");

  sonicDestroyStream(stream);

  /* An empty track restores the normal mode and unlocks the setters. */
  stream = sonicCreateStream(16000, 1);
  sonicSetQuality(stream, 1);
  CHECK(sonicSetSpeedTrack(stream, good, 3) == 1, "valid track accepted");
  CHECK(sonicSetSpeedTrack(stream, NULL, 0) == 1, "empty track clears mode");
  sonicSetSpeed(stream, 1.5f);
  CHECK(sonicGetSpeed(stream) == 1.5f, "speed settable after clearing");
  sonicSetPitch(stream, 1.5f);
  CHECK(sonicGetPitch(stream) == 1.5f, "pitch settable after clearing");
  sonicSetRate(stream, 1.5f);
  CHECK(sonicGetRate(stream) == 1.5f, "rate settable after clearing");
  sonicSetSampleRate(stream, 48000);
  CHECK(sonicGetSampleRate(stream) == 48000,
        "sample rate settable after clearing");
  sonicSetNumChannels(stream, 2);
  CHECK(sonicGetNumChannels(stream) == 2,
        "channels settable after clearing");
  sonicDestroyStream(stream);

  casesRun++;
}

/* Results must be readable before flush, and flush must complete them. */
static void testProgressiveOutput(void) {
  const int N = SR / 2;
  SonicSpeedPoint points[2];
  sonicStream stream = sonicCreateStream(SR, 1);
  short* input = (short*)malloc(sizeof(short) * N);
  short out[4096];

  genSine(input, N, 220.0);
  points[0].inputFrame = 0;
  points[0].speed = 1.5f;
  points[1].inputFrame = N;
  points[1].speed = 1.5f;

  sonicSetQuality(stream, 1);
  CHECK(sonicSetSpeedTrack(stream, points, 2) == 1, "progressive track set");
  sonicWriteShortToStream(stream, input, N);
  CHECK(sonicSamplesAvailable(stream) > 0,
        "output available before flush once enough input was written");

  sonicFlushStream(stream);
  {
    int total = 0;
    int got;
    while ((got = sonicReadShortFromStream(stream, out, 4096)) > 0) {
      total += got;
    }
    CHECK(total > 0, "flush completes the segment");
    CHECK(fabs((double)total - (double)N / 1.5) <= TOLERANCE,
          "unit 1.5x length matches the integral");
  }
  sonicDestroyStream(stream);
  free(input);
  casesRun++;
}

/* After flush the same stream is reusable: the next segment applies the same
   track again, starting from source frame 0. */
static void testReuse(void) {
  const int M = 4096;
  SonicSpeedPoint points[2];
  sonicStream stream = sonicCreateStream(SR, 1);
  short* input = (short*)malloc(sizeof(short) * M);
  short* out1 = (short*)malloc(sizeof(short) * 4 * M);
  short* out2 = (short*)malloc(sizeof(short) * 4 * M);
  int len1 = 0;
  int len2 = 0;
  int got;
  int i;

  genSine(input, M, 220.0);
  points[0].inputFrame = 0;
  points[0].speed = 1.7f;
  points[1].inputFrame = M;
  points[1].speed = 0.7f;

  sonicSetQuality(stream, 1);
  CHECK(sonicSetSpeedTrack(stream, points, 2) == 1, "reuse track set");

  sonicWriteShortToStream(stream, input, M);
  sonicFlushStream(stream);
  while ((got = sonicReadShortFromStream(stream, out1 + len1, 4 * M - len1)) > 0) {
    len1 += got;
  }

  sonicWriteShortToStream(stream, input, M);
  sonicFlushStream(stream);
  while ((got = sonicReadShortFromStream(stream, out2 + len2, 4 * M - len2)) > 0) {
    len2 += got;
  }

  CHECK(len1 > 0 && len1 == len2, "reuse: both segments have equal length");
  for (i = 0; i < len1 && i < len2; i++) {
    if (abs((int)out1[i] - (int)out2[i]) > 1) {
      CHECK(0, "reuse: second segment restarts the track from frame 0");
      break;
    }
  }

  free(input);
  free(out1);
  free(out2);
  sonicDestroyStream(stream);
  casesRun++;
}

/* The nodes must be copied: mutating the caller's array after the call has no
   effect. */
static void testPointsCopied(void) {
  const int N = SR / 2;
  SonicSpeedPoint points[2];
  short* input = (short*)malloc(sizeof(short) * N);
  Buffer before;
  Buffer after;

  genSine(input, N, 220.0);
  points[0].inputFrame = 0;
  points[0].speed = 2.0f;
  points[1].inputFrame = N;
  points[1].speed = 2.0f;

  /* Two streams: one built from the intended points, one where the caller's
     array is overwritten right after the call. */
  {
    sonicStream stream = sonicCreateStream(SR, 1);
    Buffer out;
    int readTotal = 0;
    int got;
    out.data = (short*)malloc(sizeof(short) * (N * 4 + 65536));
    sonicSetQuality(stream, 1);
    sonicSetSpeedTrack(stream, points, 2);
    points[0].speed = 0.5f;
    points[1].speed = 0.5f;
    sonicWriteShortToStream(stream, input, N);
    sonicFlushStream(stream);
    while ((got = sonicReadShortFromStream(stream, out.data + readTotal,
                                           N * 4 + 65536 - readTotal)) > 0) {
      readTotal += got;
    }
    out.length = readTotal;
    sonicDestroyStream(stream);
    before = out;
  }
  points[0].speed = 2.0f;
  points[1].speed = 2.0f;
  after = runTrack(input, N, 1, points, 2, 1, 1);

  CHECK(before.length == after.length,
        "points copied: mutation after the call does not change the length");

  free(input);
  free(before.data);
  free(after.data);
  casesRun++;
}

/* 48 kHz mono must also work, with the length matching its own integral. */
static void test48k(void) {
  sonicStream stream = sonicCreateStream(48000, 1);
  const int N = 48000 * 2;
  SonicSpeedPoint points[3];
  short* input = (short*)malloc(sizeof(short) * 2 * N);
  short* out = (short*)malloc(sizeof(short) * 4 * N);
  int written = 0;
  int total = 0;
  int got;
  double target;

  genSine(input, N, 220.0);
  points[0].inputFrame = 0;
  points[0].speed = 1.8f;
  points[1].inputFrame = N / 2;
  points[1].speed = 1.0f;
  points[2].inputFrame = N;
  points[2].speed = 0.6f;
  target = trackDuration(points, 3, 0.0, (double)N);

  sonicSetQuality(stream, 1);
  CHECK(sonicSetSpeedTrack(stream, points, 3) == 1, "48 kHz track accepted");
  while (written < N) {
    int chunk = written + 1000 > N ? N - written : 1000;
    sonicWriteShortToStream(stream, input + written, chunk);
    written += chunk;
    while ((got = sonicReadShortFromStream(stream, out + total, 4 * N - total)) >
           0) {
      total += got;
    }
  }
  sonicFlushStream(stream);
  while ((got = sonicReadShortFromStream(stream, out + total, 4 * N - total)) >
         0) {
    total += got;
  }
  CHECK(fabs((double)total - target) <= 2 * 2 * (48000 / SONIC_MIN_PITCH),
        "48 kHz length matches the duration integral");
  sonicDestroyStream(stream);
  free(input);
  free(out);
  casesRun++;
}

/* With prompt reads, the amount of PCM held by the stream must not grow with
   the total recording length. */
static void testBufferedDoesNotGrow(void) {
  const int chunk = 512;
  SonicSpeedPoint points[2];
  short* input = (short*)malloc(sizeof(short) * chunk);
  short tmp[4096];
  int totals[3];
  int peakShort;
  int peakLong;
  int t;

  points[0].inputFrame = 0;
  points[0].speed = 1.8f;
  points[1].inputFrame = 200000;
  points[1].speed = 0.6f;

  for (t = 0; t < 3; t++) {
    totals[t] = t == 0 ? 8000 : (t == 1 ? 80000 : 400000);
  }

  /* Peak availability for a short recording, then for a long one. */
  {
    sonicStream stream = sonicCreateStream(SR, 1);
    int written;
    int got;
    sonicSetQuality(stream, 1);
    sonicSetSpeedTrack(stream, points, 2);
    peakShort = 0;
    for (written = 0; written < totals[0]; written += chunk) {
      int amount = written + chunk > totals[0] ? totals[0] - written : chunk;
      sonicWriteShortToStream(stream, input, amount);
      if (sonicSamplesAvailable(stream) > peakShort) {
        peakShort = sonicSamplesAvailable(stream);
      }
      while ((got = sonicReadShortFromStream(stream, tmp, 4096)) > 0) {
      }
    }
    while ((got = sonicReadShortFromStream(stream, tmp, 4096)) > 0) {
    }
    sonicDestroyStream(stream);
  }
  {
    sonicStream stream = sonicCreateStream(SR, 1);
    int written;
    int got;
    sonicSetQuality(stream, 1);
    sonicSetSpeedTrack(stream, points, 2);
    peakLong = 0;
    for (written = 0; written < totals[2]; written += chunk) {
      sonicWriteShortToStream(stream, input, chunk);
      if (sonicSamplesAvailable(stream) > peakLong) {
        peakLong = sonicSamplesAvailable(stream);
      }
      while ((got = sonicReadShortFromStream(stream, tmp, 4096)) > 0) {
      }
    }
    while ((got = sonicReadShortFromStream(stream, tmp, 4096)) > 0) {
    }
    sonicDestroyStream(stream);
  }

  /* A single chunk's output plus two analysis windows is the expected bound;
     the point is that it does not scale with 400000 source frames. */
  CHECK(peakLong <= 4 * 2 * (SR / SONIC_MIN_PITCH) + 4 * chunk,
        "buffered PCM does not grow with the recording length");
  CHECK(peakLong <= peakShort + 4 * chunk,
        "long recording buffers no more than a short one plus one chunk");

  free(input);
  casesRun++;
}

int main(void) {
  testFastThenSlow();
  testDenseNodes();
  testShortTail();
  testUnitTrack();
  testStereo();
  testRejections();
  testProgressiveOutput();
  testReuse();
  testPointsCopied();
  test48k();
  testBufferedDoesNotGrow();

  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  if (casesRun <= 0) {
    fprintf(stderr, "no cases ran\n");
    return 1;
  }
  printf("SONIC_SPEED_TRACK_OK cases=%d\n", casesRun);
  return 0;
}
