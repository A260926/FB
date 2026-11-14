/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Example:  splitting one variable-rate resampling pass into two audition
 * branches at a chosen point, without converting the earlier audio twice.
 *
 * A single soxr instance is created (SOXR_VR) and driven part-way through a
 * signal.  At that point soxr_clone() makes a second, independent resampler
 * holding the *entire* live state: input still buffered in the engine, the
 * fractional resampling position, any unfinished ratio slew and rate-stage
 * cross-fade, and the filter history.  The two instances then continue with
 * different I/O ratios; each produces the same audio it would have produced
 * had it been driven from the start along that path, but only one pass was
 * made over the prefix.
 *
 * Usage: ./6-branch-preview
 * Output: two raw float32 signals, branch A then branch B, on stdout.
 */

#include <soxr.h>
#include "examples-common.h"

#define PREFIX_LEN  4000        /* Samples converted once, then shared. */
#define BRANCH_LEN 20000        /* Samples produced per branch. */
#define BLOCK      512          /* Input block, samples per channel. */
#define OBUF       1024         /* Output block, samples per channel. */

int main(void)
{
  float ibuf[BLOCK], obuf[OBUF];
  soxr_error_t error;
  soxr_quality_spec_t q_spec = soxr_quality_spec(SOXR_HQ, SOXR_VR);
  soxr_t soxr;
  soxr_t branch[2];
  double ratios[2] = {1.75, 0.52};    /* The two paths to audition. */
  int b, i;

  /* As for any variable-rate resampler, the create-time ratio is the maximum
   * I/O ratio that will be used: */
  soxr = soxr_create(1 << 3, 1, 1, &error, NULL, &q_spec, NULL);
  if (error || !soxr) {
    fprintf(stderr, "%s\n", soxr_strerror(error));
    return 1;
  }

  /* First pass: convert the common prefix.  A slew is deliberately still
   * in progress when we stop, to demonstrate that the clone captures it. */
  soxr_set_io_ratio(soxr, 1.0, 0);
  soxr_set_io_ratio(soxr, ratios[0], PREFIX_LEN / 2);
  for (i = 0; i < PREFIX_LEN; i += BLOCK) {
    size_t odone = 0, j;
    for (j = 0; j < BLOCK; ++j)
      ibuf[j] = (float)(.8 * sin(2 * M_PI * (i + j) / 37.));
    soxr_process(soxr, ibuf, BLOCK, NULL, obuf, OBUF, &odone);
    fwrite(obuf, sizeof(*obuf), odone, stdout);   /* Common prefix output. */
  }

  /* Fork the live state.  The original becomes branch 0; the clone is branch
   * 1.  Cloning neither replays the prefix nor calls any input function. */
  branch[0] = soxr;
  branch[1] = soxr_clone(soxr, &error);
  if (error || !branch[1]) {
    fprintf(stderr, "%s\n", soxr_strerror(error));
    soxr_delete(soxr);
    return 1;
  }

  /* Each branch now follows its own subsequent programme. */
  for (b = 0; b < 2; ++b) {
    size_t produced = 0, odone = 0;
    if (b)
      soxr_set_io_ratio(branch[b], ratios[1], 0);
    /* First, play out the audio already buffered in the fork state -- with
     * no new input at all: */
    do {
      soxr_process(branch[b], NULL, 0, NULL, obuf, OBUF, &odone);
      fwrite(obuf, sizeof(*obuf), odone, stdout);
      produced += odone;
    } while (odone && produced < BRANCH_LEN / 4);

    /* Then continue resampling new input along this branch's ratio path. */
    for (i = PREFIX_LEN; produced < BRANCH_LEN; i += BLOCK) {
      size_t j, idone = 0;
      double pos = (double)produced / BRANCH_LEN;
      /* Sweep the ratio smoothly; the branch keeps its own copy of the slew
       * state and is unaffected by the other branch. */
      soxr_set_io_ratio(branch[b], ratios[b] * (1 + .3 * sin(pos * 6)),
          BLOCK);
      for (j = 0; j < BLOCK; ++j)
        ibuf[j] = (float)(.8 * sin(2 * M_PI * (i + j) / 37.));
      soxr_process(branch[b], ibuf, BLOCK, &idone, obuf, OBUF, &odone);
      fwrite(obuf, sizeof(*obuf), odone, stdout);
      produced += odone;
    }
    soxr_delete(branch[b]);
  }
  return 0;
}
