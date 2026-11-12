/* See COPYING file for copyright and license details. */

#ifndef EBUR128_SEGMENT_H_
#define EBUR128_SEGMENT_H_

/** \file ebur128_segment.h
 *  \brief Concatenatable loudness summaries for independently measured PCM
 *         sections.
 *
 *  An ebur128_segment is an owning summary built from one contiguous section
 *  of interleaved double PCM.  Each section can be measured on its own (for
 *  example on a worker thread that only sees that section), and the resulting
 *  summaries can later be concatenated in programme order.
 *
 *  Concatenation models splicing the underlying PCM end to end, not the
 *  ebur128_loudness_global_multiple() merge of independent programmes:
 *  - the K-weighting filter state is carried continuously across the splice,
 *  - the 400 ms and 3 s gating windows keep a single phase across the splice,
 *  - absolute and relative gating are applied once to the whole concatenated
 *    sequence.
 *
 *  The summary never retains the input PCM, per-sample filtered values, audio
 *  files or read callbacks.  At most one fixed-size record is kept per
 *  100 ms block and channel, plus the filter state at the end of the section.
 *  After ebur128_segment_create() returns, the caller may overwrite or free
 *  the PCM and the channel map immediately.
 */

#include <stddef.h>

#include "ebur128.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque, owning loudness summary of one PCM section. */
typedef struct ebur128_segment ebur128_segment;

/** Query results for a (possibly concatenated) summary. */
typedef struct {
  /** Total number of PCM frames represented by the summary. */
  size_t frames;
  /** Number of complete 100 ms gating blocks. */
  size_t energy_blocks;
  /** Number of complete 400 ms integrated windows formed before any gating. */
  size_t integrated_windows;
  /** Number of complete 3 s short-term windows formed before any gating. */
  size_t shortterm_windows;
  /** Integrated loudness in LUFS; -HUGE_VAL when no I block passes the gates. */
  double integrated_lufs;
  /** Loudness range in LU; 0.0 for an empty short-term set, as in ebur128. */
  double lra_lu;
} ebur128_segment_result;

/** \brief Build an owning loudness summary from one PCM section.
 *
 *  The section is measured with zero K-weighting filter history.
 *
 *  @param channels number of channels, 1 to 6.
 *  @param samplerate sample rate; only 44100 and 48000 are supported.
 *  @param channel_map channel_map[i] must be one of EBUR128_LEFT,
 *         EBUR128_RIGHT, EBUR128_CENTER, EBUR128_LEFT_SURROUND,
 *         EBUR128_RIGHT_SURROUND, EBUR128_UNUSED or, for a single-channel
 *         section, EBUR128_DUAL_MONO.  Must contain channels entries.
 *  @param pcm interleaved double PCM, channels values per frame; may be NULL
 *         only when frames is zero.
 *  @param frames number of frames; must be a multiple of samplerate/10 (the
 *         100 ms block size).  Every sample must be finite.
 *  @return a new owning summary, or NULL on invalid arguments, non-finite
 *          samples or allocation failure.  The returned object does not alias
 *          pcm or channel_map.
 */
ebur128_segment*
ebur128_segment_create(unsigned int channels,
                       unsigned long samplerate,
                       const int* channel_map,
                       const double* pcm,
                       size_t frames);

/** \brief Concatenate two summaries: the result represents a followed by b.
 *
 *  Both summaries must use the same sample rate, channel count and channel
 *  map.  A summary may be concatenated with itself.  The returned object is
 *  independently owned; a and b are never modified.
 *
 *  @param a first summary, must not be NULL.
 *  @param b second summary, must not be NULL.
 *  @param out receives the new summary.  On failure *out is left untouched.
 *  @return EBUR128_SUCCESS on success,
 *          EBUR128_ERROR_INVALID_ARGUMENT for NULL pointers or mismatched
 *          configurations, EBUR128_ERROR_NOMEM on allocation failure.
 */
int ebur128_segment_concat(const ebur128_segment* a,
                           const ebur128_segment* b,
                           ebur128_segment** out);

/** \brief Destroy a summary and set *p to NULL.
 *
 *  Does nothing when *p is NULL.  A NULL p pointer is ignored.
 */
void ebur128_segment_destroy(ebur128_segment** p);

/** \brief Number of heap bytes owned by the summary.
 *
 *  Counts the object itself and every buffer and cache it owns, using the
 *  requested allocation sizes (allocator metadata excluded).  An empty
 *  summary is counted too.  The result never exceeds
 *  2048 * channels * (1 + frames / (samplerate / 10)) bytes.
 */
size_t
ebur128_segment_storage_bytes(const ebur128_segment* s);

/** \brief Measure integrated loudness and loudness range.
 *
 *  The result matches feeding the concatenated PCM once through a regular
 *  ebur128 instance running EBUR128_MODE_I | EBUR128_MODE_LRA without
 *  EBUR128_MODE_HISTOGRAM and without a history limit.  For ordinary material
 *  away from gating and percentile boundaries the agreement is within
 *  1e-4 LU.  When no I block survives gating, integrated_lufs is -HUGE_VAL;
 *  lra_lu is 0.0 when the short-term set is empty.
 *
 *  @return EBUR128_SUCCESS, or EBUR128_ERROR_INVALID_ARGUMENT if either
 *          argument is NULL.
 */
int ebur128_segment_measure(const ebur128_segment* s,
                            ebur128_segment_result* out);

/** \brief Energy of one 100 ms block of the whole concatenated sequence.
 *
 *  The sequence is evaluated with zero filter history; block index counts
 *  from the start of the represented audio.  The value is the channel-weighted
 *  sum of squared K-weighted samples divided by the number of frames in the
 *  block, exactly as ebur128_calc_gating_block() would produce for the
 *  concatenated PCM.  For ordinary material it differs from a direct
 *  K-weighting recursion by at most 1e-10 + 1e-5 times the reference energy.
 *
 *  @param index block index, must be smaller than the number of complete
 *         100 ms blocks.
 *  @param out receives the energy; not written on failure.
 *  @return EBUR128_SUCCESS, or EBUR128_ERROR_INVALID_ARGUMENT on a NULL
 *          argument or an out-of-range index.
 */
int ebur128_segment_block_energy(const ebur128_segment* s,
                                 size_t index,
                                 double* out);

#ifdef __cplusplus
}
#endif

#endif /* EBUR128_SEGMENT_H_ */
