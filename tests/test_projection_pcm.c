/*
 * test_projection_pcm.c
 *
 * Regression test for the Opus projection decoder's demixing matrix
 * applied to real decoded PCM.
 *
 * A continuous four-channel signal is encoded with the public
 * multistream encoder (two coupled streams, 48 kHz, 960 frames per
 * packet, 14 packets). Every packet is decoded twice, side by side:
 *
 *   - an OpusProjectionDecoder configured with a test-owned asymmetric
 *     4x4 signed-Q15 demixing matrix (handed over column-major,
 *     little-endian int16, as the public API requires), and
 *   - an independent OpusMSDecoder with the identity channel mapping
 *     that preserves the four coded channels.
 *
 * The expected demixing is computed by this test's own scalar
 * multiply-accumulate over the independently decoded PCM. It neither
 * calls the library's internal mapping_matrix routines nor includes
 * their headers, and it is written output-centric from the mathematical
 * definition rather than copied from the production loop nest.
 *
 * The float entry point is compared per frame and per channel with a
 * 2e-6 error bound, plus finite-value checks and sentinels around the
 * output buffers. The int16 entry point runs on its own identically
 * initialized decoder pair; each input's contribution is Q15-rounded
 * before summing (the established fixed-point rule), with negative
 * rounding expressed through well-defined integer operations only.
 *
 * The schedule includes one single packet loss, then two consecutive
 * losses (both sides request PLC with NULL/0 for the same frame count),
 * a decode-gain change at a non-first packet, and finally an
 * OPUS_RESET_STATE whose replay is compared against a freshly
 * initialized decoder carrying the same matrix and gain. Output buffers
 * are poisoned with non-zero values before every decode call.
 *
 * Exit codes: 0 = pass, 1 = numeric assertion failed, 2 = internal
 * error (API failure or test-design violation).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opus_types.h"
#include "opus_defines.h"
#include "opus_multistream.h"
#include "opus_projection.h"

#define SAMPLE_RATE 48000
#define NB_CHANNELS 4       /* projection output channels */
#define NB_STREAMS 2
#define NB_COUPLED 2
#define NB_CODED (NB_STREAMS + NB_COUPLED)  /* coded channels: 4 */
#define FRAME_SIZE 960      /* 20 ms at 48 kHz */
#define NB_PACKETS 14       /* >= 12 packets */
#define MAX_PACKET_BYTES 4000

#define EXIT_MISMATCH 1
#define EXIT_INTERNAL 2

/* Decode gain (Q7.8 dB units, -3 dB) applied from GAIN_PACKET onwards. */
#define GAIN_PACKET 10
#define GAIN_Q8DB (-768)

/* Float comparison bound and int16 tolerance (<= one quantization unit;
   the int16 reference is exact integer arithmetic, so 0 is used). */
#define FLOAT_TOL 2e-6
#define SHORT_TOL 0

#define GUARD 8
#define FLOAT_GUARD_VALUE 7.3456789e10f
#define SHORT_GUARD_VALUE ((opus_int16)0x5A5A)

#define PI_D 3.14159265358979323846

/* Loss schedule: one single lost packet, then two consecutive losses. */
static int packet_is_lost(int p)
{
  return p == 4 || p == 7 || p == 8;
}

/* Demixing matrix in mathematical layout: dm_q15[row][col] with
   row = output channel (0..3), col = coded channel (0..3). Signed Q15
   (coefficient = value / 32768). Every row mixes positive and negative
   coefficients with non-integer ratios, every row combines all four
   coded inputs, each row's absolute coefficient sum stays below 1.0 to
   avoid clipping, and the matrix is asymmetric so that row/column
   transpositions are observable. */
static const opus_int16 dm_q15[NB_CHANNELS][NB_CODED] = {
  {  9830,  -8192,   6554,  -3277 },
  { -11469,  4915,  -6554,   8192 },
  {   4096, 12288, -10240,   2048 },
  {  -6554, -4915,   9830,   6554 }
};

static void internal_error(const char *what, long a, long b)
{
  fprintf(stderr, "INTERNAL_ERROR %s (%ld, %ld)\n", what, a, b);
  exit(EXIT_INTERNAL);
}

static void report_mismatch(const char *group, int packet, int frame,
                            int channel, double got, double expected,
                            double err)
{
  fprintf(stderr,
          "MISMATCH group=%s packet=%d frame=%d channel=%d got=%.9g "
          "expected=%.9g err=%.9g\n",
          group, packet, frame, channel, got, expected, err);
  exit(EXIT_MISMATCH);
}

/* Verify the test-owned matrix has the properties this regression relies
   on. A violation here is a test-design error, not a library failure. */
static void check_matrix(void)
{
  int row, col;
  int asymmetric = 0;
  int full_row_seen = 0;
  for (row = 0; row < NB_CHANNELS; row++)
  {
    int has_pos = 0, has_neg = 0, has_frac = 0, nonzero = 0;
    long abssum = 0;
    for (col = 0; col < NB_CODED; col++)
    {
      opus_int16 v = dm_q15[row][col];
      if (v > 0) has_pos = 1;
      if (v < 0) has_neg = 1;
      if (v != 0 && v != 32767 && v != (opus_int16)-32768) has_frac = 1;
      if (v != 0) nonzero++;
      abssum += v < 0 ? -(long)v : (long)v;
      if (dm_q15[row][col] != dm_q15[col][row]) asymmetric = 1;
    }
    if (!has_pos || !has_neg || !has_frac)
      internal_error("matrix row lacks signed fractional coefficients", row, 0);
    if (abssum > 32768)
      internal_error("matrix row absolute sum exceeds 1.0", row, abssum);
    if (nonzero == NB_CODED) full_row_seen = 1;
  }
  if (!asymmetric)
    internal_error("matrix must be asymmetric", 0, 0);
  if (!full_row_seen)
    internal_error("no matrix row combines all four inputs", 0, 0);
}

/* Serialize the matrix the way the public projection API expects it:
   nb_coded*nb_channels little-endian int16 values, column-major (the
   coded-channel index changes slowest): element (row, col) is stored at
   int16 index col*NB_CHANNELS + row. */
static void serialize_matrix(unsigned char *out)
{
  int row, col;
  for (col = 0; col < NB_CODED; col++)
  {
    for (row = 0; row < NB_CHANNELS; row++)
    {
      opus_uint16 u = (opus_uint16)dm_q15[row][col]; /* well-defined mod 2^16 */
      int k = col * NB_CHANNELS + row;
      out[2 * k]     = (unsigned char)(u & 0xFFu);
      out[2 * k + 1] = (unsigned char)(u >> 8);
    }
  }
}

/* Deterministic low-amplitude signal: each channel is a distinct tone
   pair with its own frequency and starting phase. */
static void generate_signal(opus_int16 *sig, int nframes)
{
  static const double freq[NB_CHANNELS] = { 397.0, 613.0, 859.0, 1123.0 };
  static const double phase[NB_CHANNELS] = { 0.0, 0.7, 1.3, 2.1 };
  int i, ch;
  for (i = 0; i < nframes; i++)
  {
    double t = (double)i / SAMPLE_RATE;
    for (ch = 0; ch < NB_CHANNELS; ch++)
    {
      double v = 2500.0 * sin(2.0 * PI_D * freq[ch] * t + phase[ch])
               +  700.0 * sin(2.0 * PI_D * 0.5 * freq[ch] * t
                              + 0.31 * (double)(ch + 1));
      sig[i * NB_CHANNELS + ch] = (opus_int16)(v < 0 ? v - 0.5 : v + 0.5);
    }
  }
}

/* Round one Q15 product to an integer contribution using the established
   demixing rule: floor((m*x + 16384) / 32768). Written with truncating
   C99 division plus a remainder fix-up so the result is exact floor
   division for negative values on any compiler, without relying on
   arithmetic right shifts of negative signed values. */
static opus_int32 q15_contribution(opus_int16 m, opus_int16 x)
{
  opus_int32 t = (opus_int32)m * (opus_int32)x + 16384;
  opus_int32 q = t / 32768;
  opus_int32 r = t % 32768;
  if (r < 0)
    q -= 1;
  return q;
}

/* Independent demixing reference, float path: plain scalar
   multiply-accumulate per output sample, accumulated in double from the
   mathematical definition. */
static double ref_demix_float_sample(int row, const float *coded)
{
  double acc = 0.0;
  int col;
  for (col = 0; col < NB_CODED; col++)
    acc += ((double)dm_q15[row][col] / 32768.0) * (double)coded[col];
  return acc;
}

/* Independent demixing reference, int16 path: each coded channel's
   contribution is Q15-rounded first, then summed. */
static opus_int32 ref_demix_short_sample(int row, const opus_int16 *coded)
{
  opus_int32 acc = 0;
  int col;
  for (col = 0; col < NB_CODED; col++)
    acc += q15_contribution(dm_q15[row][col], coded[col]);
  return acc;
}

/* Alternative rounding order (sum the products first, round once). Used
   only to prove the signal/coefficient set can distinguish the two
   rounding orders; never used as the expected value. */
static opus_int32 ref_demix_short_sum_first(int row, const opus_int16 *coded)
{
  opus_int64 t = 16384;
  opus_int64 q, r;
  int col;
  for (col = 0; col < NB_CODED; col++)
    t += (opus_int64)dm_q15[row][col] * (opus_int64)coded[col];
  q = t / 32768;
  r = t % 32768;
  if (r < 0)
    q -= 1;
  return (opus_int32)q;
}

static float fproj[GUARD + FRAME_SIZE * NB_CHANNELS + GUARD];
static float fms[GUARD + FRAME_SIZE * NB_CODED + GUARD];
static float fproj2[GUARD + FRAME_SIZE * NB_CHANNELS + GUARD];
static opus_int16 sproj[GUARD + FRAME_SIZE * NB_CHANNELS + GUARD];
static opus_int16 sms[GUARD + FRAME_SIZE * NB_CODED + GUARD];

static void set_float_guards(float *buf, int ndata)
{
  int i;
  for (i = 0; i < GUARD; i++)
  {
    buf[i] = FLOAT_GUARD_VALUE;
    buf[GUARD + ndata + i] = FLOAT_GUARD_VALUE;
  }
}

static int float_guards_ok(const float *buf, int ndata)
{
  int i;
  for (i = 0; i < GUARD; i++)
  {
    if (buf[i] != FLOAT_GUARD_VALUE) return 0;
    if (buf[GUARD + ndata + i] != FLOAT_GUARD_VALUE) return 0;
  }
  return 1;
}

static void set_short_guards(opus_int16 *buf, int ndata)
{
  int i;
  for (i = 0; i < GUARD; i++)
  {
    buf[i] = SHORT_GUARD_VALUE;
    buf[GUARD + ndata + i] = SHORT_GUARD_VALUE;
  }
}

static int short_guards_ok(const opus_int16 *buf, int ndata)
{
  int i;
  for (i = 0; i < GUARD; i++)
  {
    if (buf[i] != SHORT_GUARD_VALUE) return 0;
    if (buf[GUARD + ndata + i] != SHORT_GUARD_VALUE) return 0;
  }
  return 1;
}

/* Fill the output region with non-zero values so that a decoder which
   fails to clear/overwrite it on every call is observable. */
static void poison_float(float *p, int n, int seed)
{
  int i;
  for (i = 0; i < n; i++)
    p[i] = 0.03125f * (float)((seed % 7) + 1) + 0.001f * (float)(i % 3);
}

static void poison_short(opus_int16 *p, int n, int seed)
{
  int i;
  for (i = 0; i < n; i++)
    p[i] = (opus_int16)(1234 + 97 * ((seed + i) % 5));
}

static OpusProjectionDecoder *new_projection_decoder(const unsigned char *matrix_bytes)
{
  int err = OPUS_OK;
  OpusProjectionDecoder *st = opus_projection_decoder_create(
      SAMPLE_RATE, NB_CHANNELS, NB_STREAMS, NB_COUPLED,
      (unsigned char *)matrix_bytes, 2 * NB_CHANNELS * NB_CODED, &err);
  if (err != OPUS_OK || st == NULL)
    internal_error("opus_projection_decoder_create", err, 0);
  return st;
}

static OpusMSDecoder *new_ms_decoder(const unsigned char *mapping)
{
  int err = OPUS_OK;
  OpusMSDecoder *st = opus_multistream_decoder_create(
      SAMPLE_RATE, NB_CHANNELS, NB_STREAMS, NB_COUPLED, mapping, &err);
  if (err != OPUS_OK || st == NULL)
    internal_error("opus_multistream_decoder_create", err, 0);
  return st;
}

static void set_gain_pair(OpusProjectionDecoder *proj, OpusMSDecoder *ms)
{
  if (opus_projection_decoder_ctl(proj, OPUS_SET_GAIN(GAIN_Q8DB)) != OPUS_OK)
    internal_error("projection OPUS_SET_GAIN", 0, 0);
  if (opus_multistream_decoder_ctl(ms, OPUS_SET_GAIN(GAIN_Q8DB)) != OPUS_OK)
    internal_error("multistream OPUS_SET_GAIN", 0, 0);
}

/* Float entry point: full schedule, per-frame/per-channel comparison
   against the independent reference, finite and sentinel checks. */
static void run_float_group(const unsigned char *pkts, const int *plen,
                            const unsigned char *matrix_bytes,
                            const unsigned char *mapping)
{
  OpusProjectionDecoder *proj;
  OpusMSDecoder *ms;
  long compared = 0;
  double max_err = 0.0;
  int p, i, o;

  proj = new_projection_decoder(matrix_bytes);
  ms = new_ms_decoder(mapping);

  for (p = 0; p < NB_PACKETS; p++)
  {
    const unsigned char *data;
    opus_int32 len;
    int r1, r2;

    if (p == GAIN_PACKET)
      set_gain_pair(proj, ms);

    if (packet_is_lost(p)) { data = NULL; len = 0; }
    else { data = pkts + (size_t)p * MAX_PACKET_BYTES; len = plen[p]; }

    set_float_guards(fproj, FRAME_SIZE * NB_CHANNELS);
    set_float_guards(fms, FRAME_SIZE * NB_CODED);
    poison_float(fproj + GUARD, FRAME_SIZE * NB_CHANNELS, p);
    poison_float(fms + GUARD, FRAME_SIZE * NB_CODED, p + 3);

    r1 = opus_projection_decode_float(proj, data, len, fproj + GUARD,
                                      FRAME_SIZE, 0);
    r2 = opus_multistream_decode_float(ms, data, len, fms + GUARD,
                                       FRAME_SIZE, 0);
    if (r1 != FRAME_SIZE || r2 != FRAME_SIZE)
      internal_error("float decode return", r1, r2);
    if (!float_guards_ok(fproj, FRAME_SIZE * NB_CHANNELS) ||
        !float_guards_ok(fms, FRAME_SIZE * NB_CODED))
    {
      fprintf(stderr, "SENTINEL_VIOLATION group=float packet=%d\n", p);
      exit(EXIT_MISMATCH);
    }

    for (i = 0; i < FRAME_SIZE; i++)
    {
      const float *coded = fms + GUARD + (size_t)i * NB_CODED;
      for (o = 0; o < NB_CHANNELS; o++)
      {
        double got = fproj[GUARD + (size_t)i * NB_CHANNELS + o];
        double ref = ref_demix_float_sample(o, coded);
        double e;
        if (!isfinite(got))
        {
          fprintf(stderr, "NONFINITE group=float packet=%d frame=%d "
                  "channel=%d\n", p, i, o);
          exit(EXIT_MISMATCH);
        }
        e = fabs(got - ref);
        if (e > max_err) max_err = e;
        if (e > FLOAT_TOL)
          report_mismatch("float", p, i, o, got, ref, e);
      }
    }
    compared += (long)FRAME_SIZE * NB_CHANNELS;
  }

  printf("GROUP_OK float samples=%ld max_err=%.3g\n", compared, max_err);
  opus_projection_decoder_destroy(proj);
  opus_multistream_decoder_destroy(ms);
}

/* Int16 entry point: identical schedule on its own identically
   initialized decoder pair. Expected values round each contribution to
   Q15 before summing; comparison is exact (tolerance 0 <= 1 LSB). Also
   counts samples where sum-first rounding would give a different answer,
   proving the rounding order is observable with this signal/matrix. */
static void run_short_group(const unsigned char *pkts, const int *plen,
                            const unsigned char *matrix_bytes,
                            const unsigned char *mapping)
{
  OpusProjectionDecoder *proj;
  OpusMSDecoder *ms;
  long compared = 0;
  long discriminating = 0;
  long max_err = 0;
  int p, i, o;

  proj = new_projection_decoder(matrix_bytes);
  ms = new_ms_decoder(mapping);

  for (p = 0; p < NB_PACKETS; p++)
  {
    const unsigned char *data;
    opus_int32 len;
    int r1, r2;

    if (p == GAIN_PACKET)
      set_gain_pair(proj, ms);

    if (packet_is_lost(p)) { data = NULL; len = 0; }
    else { data = pkts + (size_t)p * MAX_PACKET_BYTES; len = plen[p]; }

    set_short_guards(sproj, FRAME_SIZE * NB_CHANNELS);
    set_short_guards(sms, FRAME_SIZE * NB_CODED);
    poison_short(sproj + GUARD, FRAME_SIZE * NB_CHANNELS, p);
    poison_short(sms + GUARD, FRAME_SIZE * NB_CODED, p + 3);

    r1 = opus_projection_decode(proj, data, len, sproj + GUARD,
                                FRAME_SIZE, 0);
    r2 = opus_multistream_decode(ms, data, len, sms + GUARD,
                                 FRAME_SIZE, 0);
    if (r1 != FRAME_SIZE || r2 != FRAME_SIZE)
      internal_error("short decode return", r1, r2);
    if (!short_guards_ok(sproj, FRAME_SIZE * NB_CHANNELS) ||
        !short_guards_ok(sms, FRAME_SIZE * NB_CODED))
    {
      fprintf(stderr, "SENTINEL_VIOLATION group=int16 packet=%d\n", p);
      exit(EXIT_MISMATCH);
    }

    for (i = 0; i < FRAME_SIZE; i++)
    {
      const opus_int16 *coded = sms + GUARD + (size_t)i * NB_CODED;
      for (o = 0; o < NB_CHANNELS; o++)
      {
        opus_int32 got = sproj[GUARD + (size_t)i * NB_CHANNELS + o];
        opus_int32 ref = ref_demix_short_sample(o, coded);
        opus_int32 alt = ref_demix_short_sum_first(o, coded);
        opus_int32 e = got - ref;
        if (e < 0) e = -e;
        if (ref != alt) discriminating++;
        if (e > max_err) max_err = e;
        if (e > SHORT_TOL)
          report_mismatch("int16", p, i, o, (double)got, (double)ref,
                          (double)e);
      }
    }
    compared += (long)FRAME_SIZE * NB_CHANNELS;
  }

  if (discriminating <= 0)
    internal_error("signal/matrix cannot distinguish rounding orders", 0, 0);

  printf("GROUP_OK int16 samples=%ld max_err=%ld discriminating=%ld\n",
         compared, max_err, discriminating);
  opus_projection_decoder_destroy(proj);
  opus_multistream_decoder_destroy(ms);
}

/* Reset timing: run the full schedule once to build history, then
   OPUS_RESET_STATE. A freshly initialized decoder with the same matrix
   and the same gain replays the same compressed packets; outputs must
   match exactly. The reset decoder is also checked against the
   independent demixing reference to prove the matrix survived the reset
   (i.e. it was not replaced by an identity matrix). */
static void run_reset_group(const unsigned char *pkts, const int *plen,
                            const unsigned char *matrix_bytes,
                            const unsigned char *mapping)
{
  OpusProjectionDecoder *projA; /* history, then reset */
  OpusProjectionDecoder *projB; /* fresh reference state */
  OpusMSDecoder *msB;
  long compared = 0;
  long nonidentity = 0;
  double max_err = 0.0;
  int p, i, o;

  projA = new_projection_decoder(matrix_bytes);

  /* Build a non-trivial decode history, including the gain change. */
  for (p = 0; p < NB_PACKETS; p++)
  {
    const unsigned char *data;
    opus_int32 len;
    int r1;
    if (p == GAIN_PACKET)
    {
      if (opus_projection_decoder_ctl(projA, OPUS_SET_GAIN(GAIN_Q8DB)) != OPUS_OK)
        internal_error("history OPUS_SET_GAIN", 0, 0);
    }
    if (packet_is_lost(p)) { data = NULL; len = 0; }
    else { data = pkts + (size_t)p * MAX_PACKET_BYTES; len = plen[p]; }
    set_float_guards(fproj, FRAME_SIZE * NB_CHANNELS);
    poison_float(fproj + GUARD, FRAME_SIZE * NB_CHANNELS, p + 5);
    r1 = opus_projection_decode_float(projA, data, len, fproj + GUARD,
                                      FRAME_SIZE, 0);
    if (r1 != FRAME_SIZE)
      internal_error("history decode return", r1, 0);
  }

  if (opus_projection_decoder_ctl(projA, OPUS_RESET_STATE) != OPUS_OK)
    internal_error("OPUS_RESET_STATE", 0, 0);
  if (opus_projection_decoder_ctl(projA, OPUS_SET_GAIN(GAIN_Q8DB)) != OPUS_OK)
    internal_error("post-reset OPUS_SET_GAIN", 0, 0);

  projB = new_projection_decoder(matrix_bytes);
  msB = new_ms_decoder(mapping);
  if (opus_projection_decoder_ctl(projB, OPUS_SET_GAIN(GAIN_Q8DB)) != OPUS_OK)
    internal_error("fresh OPUS_SET_GAIN", 0, 0);
  if (opus_multistream_decoder_ctl(msB, OPUS_SET_GAIN(GAIN_Q8DB)) != OPUS_OK)
    internal_error("fresh ms OPUS_SET_GAIN", 0, 0);

  /* Replay the same compressed packets (same loss schedule) through all
     three decoders, which share the same gain. */
  for (p = 0; p < NB_PACKETS; p++)
  {
    const unsigned char *data;
    opus_int32 len;
    int r1, r2, r3;

    if (packet_is_lost(p)) { data = NULL; len = 0; }
    else { data = pkts + (size_t)p * MAX_PACKET_BYTES; len = plen[p]; }

    set_float_guards(fproj, FRAME_SIZE * NB_CHANNELS);
    set_float_guards(fproj2, FRAME_SIZE * NB_CHANNELS);
    set_float_guards(fms, FRAME_SIZE * NB_CODED);
    poison_float(fproj + GUARD, FRAME_SIZE * NB_CHANNELS, p + 1);
    poison_float(fproj2 + GUARD, FRAME_SIZE * NB_CHANNELS, p + 2);
    poison_float(fms + GUARD, FRAME_SIZE * NB_CODED, p + 3);

    r1 = opus_projection_decode_float(projA, data, len, fproj + GUARD,
                                      FRAME_SIZE, 0);
    r2 = opus_projection_decode_float(projB, data, len, fproj2 + GUARD,
                                      FRAME_SIZE, 0);
    r3 = opus_multistream_decode_float(msB, data, len, fms + GUARD,
                                       FRAME_SIZE, 0);
    if (r1 != FRAME_SIZE || r2 != FRAME_SIZE || r3 != FRAME_SIZE)
      internal_error("reset decode return", r1, r2);
    if (!float_guards_ok(fproj, FRAME_SIZE * NB_CHANNELS) ||
        !float_guards_ok(fproj2, FRAME_SIZE * NB_CHANNELS) ||
        !float_guards_ok(fms, FRAME_SIZE * NB_CODED))
    {
      fprintf(stderr, "SENTINEL_VIOLATION group=reset packet=%d\n", p);
      exit(EXIT_MISMATCH);
    }

    for (i = 0; i < FRAME_SIZE; i++)
    {
      const float *coded = fms + GUARD + (size_t)i * NB_CODED;
      for (o = 0; o < NB_CHANNELS; o++)
      {
        double gotA = fproj[GUARD + (size_t)i * NB_CHANNELS + o];
        double gotB = fproj2[GUARD + (size_t)i * NB_CHANNELS + o];
        double ref = ref_demix_float_sample(o, coded);
        double e;
        if (!isfinite(gotA) || !isfinite(gotB))
        {
          fprintf(stderr, "NONFINITE group=reset packet=%d frame=%d "
                  "channel=%d\n", p, i, o);
          exit(EXIT_MISMATCH);
        }
        /* Reset decoder must equal the fresh decoder exactly. */
        if (gotA != gotB)
          report_mismatch("reset", p, i, o, gotA, gotB, fabs(gotA - gotB));
        /* Reset decoder must still apply the test matrix, not identity. */
        e = fabs(gotA - ref);
        if (e > max_err) max_err = e;
        if (e > FLOAT_TOL)
          report_mismatch("reset", p, i, o, gotA, ref, e);
        if (fabs(gotA - (double)coded[o]) > 1e-3)
          nonidentity++;
      }
    }
    compared += (long)FRAME_SIZE * NB_CHANNELS;
  }

  if (nonidentity <= 0)
    internal_error("reset decoder output equals identity demixing", 0, 0);

  printf("GROUP_OK reset samples=%ld max_err=%.3g nonidentity=%ld\n",
         compared, max_err, nonidentity);
  opus_projection_decoder_destroy(projA);
  opus_projection_decoder_destroy(projB);
  opus_multistream_decoder_destroy(msB);
}

int main(void)
{
  static opus_int16 sig[NB_PACKETS * FRAME_SIZE * NB_CHANNELS];
  static unsigned char pkts[NB_PACKETS * MAX_PACKET_BYTES];
  int plen[NB_PACKETS];
  unsigned char matrix_bytes[2 * NB_CHANNELS * NB_CODED];
  unsigned char mapping[NB_CHANNELS] = { 0, 1, 2, 3 };
  OpusMSEncoder *enc;
  int err = OPUS_OK;
  int p;

  check_matrix();
  serialize_matrix(matrix_bytes);
  generate_signal(sig, NB_PACKETS * FRAME_SIZE);

  /* Public multistream encoder: 4 channels as two coupled streams. */
  enc = opus_multistream_encoder_create(SAMPLE_RATE, NB_CHANNELS,
      NB_STREAMS, NB_COUPLED, mapping, OPUS_APPLICATION_AUDIO, &err);
  if (err != OPUS_OK || enc == NULL)
    internal_error("opus_multistream_encoder_create", err, 0);
  if (opus_multistream_encoder_ctl(enc, OPUS_SET_BITRATE(256000)) != OPUS_OK)
    internal_error("OPUS_SET_BITRATE", 0, 0);

  /* One continuous encode: the encoder stays alive across all packets. */
  for (p = 0; p < NB_PACKETS; p++)
  {
    int len = opus_multistream_encode(enc,
        sig + (size_t)p * FRAME_SIZE * NB_CHANNELS, FRAME_SIZE,
        pkts + (size_t)p * MAX_PACKET_BYTES, MAX_PACKET_BYTES);
    if (len <= 0)
      internal_error("opus_multistream_encode", len, p);
    plen[p] = len;
  }
  opus_multistream_encoder_destroy(enc);

  run_float_group(pkts, plen, matrix_bytes, mapping);
  run_short_group(pkts, plen, matrix_bytes, mapping);
  run_reset_group(pkts, plen, matrix_bytes, mapping);

  printf("PROJECTION_PCM_PASS\n");
  return 0;
}
