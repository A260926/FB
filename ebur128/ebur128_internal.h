/* See COPYING file for copyright and license details. */

/* Internal helpers shared by the streaming implementation in ebur128.c and
 * the concatenatable segment summaries in ebur128_segment.c.  Not installed
 * and not part of the public API. */

#ifndef EBUR128_INTERNAL_H_
#define EBUR128_INTERNAL_H_

#include "ebur128.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Number of BS.1770 K-weighting filter coefficients / state slots. */
#define EBUR128_FILTER_STATE_SIZE 5

/** Calculate the K-weighting numerator/denominator coefficients. */
void ebur128_priv_filter_coeffs(unsigned long samplerate,
                                double b[EBUR128_FILTER_STATE_SIZE],
                                double a[EBUR128_FILTER_STATE_SIZE]);

/** Calculate the two K-weighting biquads (pre-filter pb/pa, high-pass
 *  rb/ra) whose convolution gives the 5-tap coefficients above. */
void ebur128_priv_biquad_coeffs(unsigned long samplerate,
                                double pb[3],
                                double pa[3],
                                double rb[3],
                                double ra[3]);

/** Make sure the lazily initialized gating constants are available. */
void ebur128_priv_ensure_gating_constants(void);

/** 10 * log10(energy) - 0.691. */
double ebur128_priv_energy_to_loudness(double energy);

/** 10^(-10/10), the relative gate factor. */
double ebur128_priv_relative_gate_factor(void);

/** 10^(-20/10), the factor used by the LRA absolute gate. */
double ebur128_priv_minus_twenty_decibels(void);

/** Lowest block energy kept for gating (10^((-70 + 0.691)/10)). */
double ebur128_priv_min_gating_energy(void);

/** BS.1770 channel weighting factor applied to a squared-sum channel. */
double ebur128_priv_channel_weight(int channel);

/* Test instrumentation for the per-sample K-weighting recursion inside
 * ebur128_segment.c.  Concatenation and queries must leave the counter
 * untouched; only segment creation advances it. */
void ebur128_priv_reset_filter_steps(void);
size_t ebur128_priv_filter_steps(void);

#ifdef __cplusplus
}
#endif

#endif /* EBUR128_INTERNAL_H_ */
