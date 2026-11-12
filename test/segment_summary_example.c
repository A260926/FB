/* See COPYING file for copyright and license details. */

/* Example: build loudness summaries for PCM sections independently (for
 * instance on worker threads that only see their own section), release the
 * PCM, then concatenate the summaries in programme order and measure.
 *
 * The synthetic audio is generated deterministically, so the example only
 * needs the C standard library and libebur128. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ebur128.h"
#include "ebur128_segment.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHANNELS 2
#define RATE 48000ul
#define HOP ((RATE + 5) / 10) /* 100 ms in frames */

/* One independently measured section of the programme. */
struct section_job {
  size_t frames;
  double* pcm;
};

static void fill_section(struct section_job* job, size_t index) {
  size_t i;
  /* Section 0 and 2 are loud, section 1 is quiet: the splices carry the
   * K-weighting filter history continuously. */
  double amplitude = (index == 1) ? 0.004 : 0.35;
  for (i = 0; i < job->frames; ++i) {
    double t = (double) i / (double) RATE;
    double tone = sin(2.0 * M_PI * (440.0 + 40.0 * (double) index) * t);
    job->pcm[i * CHANNELS + 0] = amplitude * tone;
    job->pcm[i * CHANNELS + 1] =
        amplitude * 0.9 * sin(2.0 * M_PI * 660.0 * t);
  }
}

int main(void) {
  const int channel_map[CHANNELS] = { EBUR128_LEFT, EBUR128_RIGHT };
  /* Three sections of different lengths, each a whole number of 100 ms
   * blocks as required by ebur128_segment_create(). */
  const size_t section_blocks[3] = { 50, 30, 40 };
  ebur128_segment* summaries[3];
  ebur128_segment* combined = NULL;
  ebur128_segment_result result;
  size_t i;

  for (i = 0; i < 3; ++i) {
    struct section_job job;
    job.frames = section_blocks[i] * HOP;
    job.pcm = (double*) malloc(job.frames * CHANNELS * sizeof(double));
    if (!job.pcm) {
      fprintf(stderr, "allocation failed\n");
      return 1;
    }
    fill_section(&job, i);

    /* This is the only call that touches the PCM.  It could run on a worker
     * thread: the returned summary owns everything it needs. */
    summaries[i] = ebur128_segment_create(CHANNELS, RATE, channel_map,
                                          job.pcm, job.frames);
    if (!summaries[i]) {
      fprintf(stderr, "segment %zu creation failed\n", i);
      return 1;
    }

    /* The PCM can be overwritten or freed immediately; the summary keeps no
     * reference to it. */
    free(job.pcm);
  }

  /* Splice the summaries in programme order.  Each concat produces a new
   * owning object and leaves its inputs unchanged. */
  combined = summaries[0];
  summaries[0] = NULL; /* ownership moved into combined */
  for (i = 1; i < 3; ++i) {
    ebur128_segment* next;
    if (ebur128_segment_concat(combined, summaries[i], &next) !=
        EBUR128_SUCCESS) {
      fprintf(stderr, "concat %zu failed\n", i);
      return 1;
    }
    ebur128_segment_destroy(&combined);
    combined = next;
  }

  if (ebur128_segment_measure(combined, &result) != EBUR128_SUCCESS) {
    fprintf(stderr, "measure failed\n");
    return 1;
  }

  printf("frames:                 %zu\n", result.frames);
  printf("100 ms blocks:          %zu\n", result.energy_blocks);
  printf("400 ms windows:         %zu\n", result.integrated_windows);
  printf("3 s windows:            %zu\n", result.shortterm_windows);
  printf("integrated loudness:    %.3f LUFS\n", result.integrated_lufs);
  printf("loudness range:         %.3f LU\n", result.lra_lu);
  printf("summary storage:        %zu bytes\n",
         ebur128_segment_storage_bytes(combined));

  for (i = 0; i < 3; ++i) {
    ebur128_segment_destroy(&summaries[i]);
  }
  ebur128_segment_destroy(&combined);
  return 0;
}
