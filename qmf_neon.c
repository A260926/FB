/*
 * Open Source implementation of Audio Processing Technology codec (aptX)
 * Copyright (C) 2017       Aurelien Jacobs <aurel@gnuage.org>
 * Copyright (C) 2018-2020  Pali Rohár <pali.rohar@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/*
 * AArch64 NEON kernels for the two stage aptX QMF analysis / synthesis tree.
 *
 * Design notes:
 *
 *  - The 16 32bit signal*coefficient products of every convolution are
 *    accumulated in 64bit NEON lanes (vmlal_s32 widening multiply-add),
 *    never truncated to 32bit. The horizontal reduction is plain 64bit
 *    integer addition.
 *
 *  - Rounding and 24bit saturation are not done with NEON rounding/narrow
 *    instructions. Each convolution result goes through the same scalar
 *    rshift64() midpoint rule and clip_intp2(..., 23) saturation as the C
 *    implementation, so every stage keeps its own scaling position
 *    (analysis 23/23, synthesis 22/21).
 *
 *  - The stereo entry points process the left and right channels together,
 *    polyphase branch by polyphase branch: each coefficient vector is
 *    loaded once and shared by both channels, and the two channels are
 *    reduced together. Both the outer and the inner tree stage are covered,
 *    i.e. all eight convolutions of a stereo QMF tree execution.
 *
 *  - The circular buffer layout, the duplicated 32 entry buffer, the push
 *    order (sample written at pos and pos+FILTER_TAPS, then pos advanced)
 *    and the coefficient direction are kept identical to the scalar code,
 *    including wrapping across the 16 taps. Buffers are only read with
 *    unaligned vld1 loads, so callers keep the original alignment
 *    guarantees (byte readable input only).
 */

#if defined(__aarch64__) && !defined(APTX_FORCE_SCALAR)

#include <arm_neon.h>

#include "openaptx_internal.h"

unsigned long long aptx_qmf_neon_kernel_calls = 0;

/* Sum the two 64bit lanes of an accumulator vector. */
static inline int64_t hsum_s64(int64x2_t v)
{
    return vgetq_lane_s64(v, 0) + vgetq_lane_s64(v, 1);
}

/*
 * 16 tap dot product for one signal/coefficient pair. Four widening
 * multiply-adds keep the 32bit products in independent 64bit accumulators
 * until the final scalar reduction.
 */
static inline int64_t dot16_neon(const int32_t *sig, const int32_t *coeffs)
{
    int64x2_t a0 = vdupq_n_s64(0);
    int64x2_t a1 = vdupq_n_s64(0);
    unsigned i;

    for (i = 0; i < FILTER_TAPS; i += 4) {
        int32x4_t c = vld1q_s32(coeffs + i);
        int32x4_t s = vld1q_s32(sig + i);
        a0 = vmlal_s32(a0, vget_low_s32(s), vget_low_s32(c));
        a1 = vmlal_s32(a1, vget_high_s32(s), vget_high_s32(c));
    }

    return hsum_s64(a0) + hsum_s64(a1);
}

/*
 * 16 tap dot products for the same polyphase branch of both stereo
 * channels. The coefficient vector is loaded once and reused for the left
 * and right signals, and both channels stay in NEON registers up to their
 * reduction.
 */
static inline void dot16_stereo_neon(const int32_t *sig_left,
                                     const int32_t *sig_right,
                                     const int32_t *coeffs,
                                     int64_t *sum_left,
                                     int64_t *sum_right)
{
    int64x2_t l0 = vdupq_n_s64(0);
    int64x2_t l1 = vdupq_n_s64(0);
    int64x2_t r0 = vdupq_n_s64(0);
    int64x2_t r1 = vdupq_n_s64(0);
    unsigned i;

    for (i = 0; i < FILTER_TAPS; i += 4) {
        int32x4_t c = vld1q_s32(coeffs + i);
        int32x4_t sl = vld1q_s32(sig_left + i);
        int32x4_t sr = vld1q_s32(sig_right + i);
        l0 = vmlal_s32(l0, vget_low_s32(sl), vget_low_s32(c));
        l1 = vmlal_s32(l1, vget_high_s32(sl), vget_high_s32(c));
        r0 = vmlal_s32(r0, vget_low_s32(sr), vget_low_s32(c));
        r1 = vmlal_s32(r1, vget_high_s32(sr), vget_high_s32(c));
    }

    *sum_left = hsum_s64(l0) + hsum_s64(l1);
    *sum_right = hsum_s64(r0) + hsum_s64(r1);
}

/* Exact replica of the scalar circular buffer push. */
static inline void filter_signal_push(struct aptx_filter_signal *signal,
                                      int32_t sample)
{
    signal->buffer[signal->pos] = sample;
    signal->buffer[signal->pos + FILTER_TAPS] = sample;
    signal->pos = (uint8_t)((signal->pos + 1) & (FILTER_TAPS - 1));
}

/* Convolution followed by the per-stage scalar rounding / saturation. */
static inline int32_t convolve_neon(const struct aptx_filter_signal *signal,
                                    const int32_t coeffs[FILTER_TAPS],
                                    unsigned shift)
{
    int64_t e = dot16_neon(&signal->buffer[signal->pos], coeffs);
    return rshift64_clip24(e, shift);
}

int32_t aptx_qmf_convolution_neon(const struct aptx_filter_signal *signal,
                                  const int32_t coeffs[FILTER_TAPS],
                                  unsigned shift)
{
    return convolve_neon(signal, coeffs, shift);
}

/*
 * One polyphase analysis stage for both stereo channels at once.
 * Pushes are grouped, then the two channels are convolved branch by
 * branch with a single coefficient load per branch.
 */
static inline void polyphase_analysis_stereo(
    struct aptx_filter_signal left_signal[NB_FILTERS],
    struct aptx_filter_signal right_signal[NB_FILTERS],
    const int32_t coeffs[NB_FILTERS][FILTER_TAPS],
    unsigned shift,
    const int32_t left_samples[NB_FILTERS],
    const int32_t right_samples[NB_FILTERS],
    int32_t *left_low, int32_t *left_high,
    int32_t *right_low, int32_t *right_high)
{
    int32_t left_sub[NB_FILTERS];
    int32_t right_sub[NB_FILTERS];
    int64_t sum_left, sum_right;
    unsigned i;

    /* Same push order as the scalar polyphase analysis, all four signals. */
    for (i = 0; i < NB_FILTERS; i++) {
        filter_signal_push(&left_signal[i], left_samples[NB_FILTERS - 1 - i]);
        filter_signal_push(&right_signal[i], right_samples[NB_FILTERS - 1 - i]);
    }

    for (i = 0; i < NB_FILTERS; i++) {
        dot16_stereo_neon(&left_signal[i].buffer[left_signal[i].pos],
                          &right_signal[i].buffer[right_signal[i].pos],
                          coeffs[i], &sum_left, &sum_right);
        left_sub[i] = rshift64_clip24(sum_left, shift);
        right_sub[i] = rshift64_clip24(sum_right, shift);
    }

    *left_low = clip_intp2(left_sub[0] + left_sub[1], 23);
    *left_high = clip_intp2(left_sub[0] - left_sub[1], 23);
    *right_low = clip_intp2(right_sub[0] + right_sub[1], 23);
    *right_high = clip_intp2(right_sub[0] - right_sub[1], 23);
}

/*
 * One polyphase synthesis stage for both stereo channels at once. The
 * subband join is plain integer arithmetic; pushes and convolutions are
 * again grouped branch by branch with shared coefficient loads.
 */
static inline void polyphase_synthesis_stereo(
    struct aptx_filter_signal left_signal[NB_FILTERS],
    struct aptx_filter_signal right_signal[NB_FILTERS],
    const int32_t coeffs[NB_FILTERS][FILTER_TAPS],
    unsigned shift,
    int32_t left_low, int32_t left_high,
    int32_t right_low, int32_t right_high,
    int32_t left_samples[NB_FILTERS],
    int32_t right_samples[NB_FILTERS])
{
    int32_t left_sub[NB_FILTERS];
    int32_t right_sub[NB_FILTERS];
    int64_t sum_left, sum_right;
    unsigned i;

    left_sub[0] = left_low + left_high;
    left_sub[1] = left_low - left_high;
    right_sub[0] = right_low + right_high;
    right_sub[1] = right_low - right_high;

    for (i = 0; i < NB_FILTERS; i++) {
        filter_signal_push(&left_signal[i], left_sub[1 - i]);
        filter_signal_push(&right_signal[i], right_sub[1 - i]);
    }

    for (i = 0; i < NB_FILTERS; i++) {
        dot16_stereo_neon(&left_signal[i].buffer[left_signal[i].pos],
                          &right_signal[i].buffer[right_signal[i].pos],
                          coeffs[i], &sum_left, &sum_right);
        left_samples[i] = rshift64_clip24(sum_left, shift);
        right_samples[i] = rshift64_clip24(sum_right, shift);
    }
}

void aptx_qmf_tree_analysis_stereo_neon(struct aptx_QMF_analysis *left_qmf,
                                        struct aptx_QMF_analysis *right_qmf,
                                        const int32_t left_samples[4],
                                        const int32_t right_samples[4],
                                        int32_t left_subbands[NB_SUBBANDS],
                                        int32_t right_subbands[NB_SUBBANDS])
{
    int32_t left_inter[4];
    int32_t right_inter[4];
    unsigned i;

    /* Outer QMF: 4 input samples -> 2 downsampled intermediate samples. */
    for (i = 0; i < 2; i++)
        polyphase_analysis_stereo(left_qmf->outer_filter_signal,
                                  right_qmf->outer_filter_signal,
                                  aptx_qmf_outer_coeffs, 23,
                                  &left_samples[2 * i], &right_samples[2 * i],
                                  &left_inter[0 + i], &left_inter[2 + i],
                                  &right_inter[0 + i], &right_inter[2 + i]);

    /* Inner QMF: intermediate samples -> 4 final subbands. */
    for (i = 0; i < 2; i++)
        polyphase_analysis_stereo(left_qmf->inner_filter_signal[i],
                                  right_qmf->inner_filter_signal[i],
                                  aptx_qmf_inner_coeffs, 23,
                                  &left_inter[2 * i], &right_inter[2 * i],
                                  &left_subbands[2 * i + 0],
                                  &left_subbands[2 * i + 1],
                                  &right_subbands[2 * i + 0],
                                  &right_subbands[2 * i + 1]);

    aptx_qmf_neon_kernel_calls += 2;
}

void aptx_qmf_tree_synthesis_stereo_neon(struct aptx_QMF_analysis *left_qmf,
                                         struct aptx_QMF_analysis *right_qmf,
                                         const int32_t left_subbands[NB_SUBBANDS],
                                         const int32_t right_subbands[NB_SUBBANDS],
                                         int32_t left_samples[4],
                                         int32_t right_samples[4])
{
    int32_t left_inter[4];
    int32_t right_inter[4];
    unsigned i;

    /* Inner QMF: 4 subbands -> 2 intermediate samples (22bit scaling). */
    for (i = 0; i < 2; i++)
        polyphase_synthesis_stereo(left_qmf->inner_filter_signal[i],
                                   right_qmf->inner_filter_signal[i],
                                   aptx_qmf_inner_coeffs, 22,
                                   left_subbands[2 * i + 0],
                                   left_subbands[2 * i + 1],
                                   right_subbands[2 * i + 0],
                                   right_subbands[2 * i + 1],
                                   &left_inter[2 * i], &right_inter[2 * i]);

    /* Outer QMF: intermediate samples -> 4 output samples (21bit scaling). */
    for (i = 0; i < 2; i++)
        polyphase_synthesis_stereo(left_qmf->outer_filter_signal,
                                   right_qmf->outer_filter_signal,
                                   aptx_qmf_outer_coeffs, 21,
                                   left_inter[0 + i], left_inter[2 + i],
                                   right_inter[0 + i], right_inter[2 + i],
                                   &left_samples[2 * i], &right_samples[2 * i]);

    aptx_qmf_neon_kernel_calls += 2;
}

/* Single channel polyphase analysis stage, same ordering as the scalar one. */
static inline void polyphase_analysis_single(
    struct aptx_filter_signal signal[NB_FILTERS],
    const int32_t coeffs[NB_FILTERS][FILTER_TAPS],
    unsigned shift,
    const int32_t samples[NB_FILTERS],
    int32_t *low_subband_output,
    int32_t *high_subband_output)
{
    int32_t subbands[NB_FILTERS];
    unsigned i;

    for (i = 0; i < NB_FILTERS; i++) {
        filter_signal_push(&signal[i], samples[NB_FILTERS - 1 - i]);
        subbands[i] = convolve_neon(&signal[i], coeffs[i], shift);
    }

    *low_subband_output = clip_intp2(subbands[0] + subbands[1], 23);
    *high_subband_output = clip_intp2(subbands[0] - subbands[1], 23);
}

/* Single channel polyphase synthesis stage, same ordering as the scalar one. */
static inline void polyphase_synthesis_single(
    struct aptx_filter_signal signal[NB_FILTERS],
    const int32_t coeffs[NB_FILTERS][FILTER_TAPS],
    unsigned shift,
    int32_t low_subband_input,
    int32_t high_subband_input,
    int32_t samples[NB_FILTERS])
{
    int32_t subbands[NB_FILTERS];
    unsigned i;

    subbands[0] = low_subband_input + high_subband_input;
    subbands[1] = low_subband_input - high_subband_input;

    for (i = 0; i < NB_FILTERS; i++) {
        filter_signal_push(&signal[i], subbands[1 - i]);
        samples[i] = convolve_neon(&signal[i], coeffs[i], shift);
    }
}

void aptx_qmf_tree_analysis_neon(struct aptx_QMF_analysis *qmf,
                                 const int32_t samples[4],
                                 int32_t subband_samples[NB_SUBBANDS])
{
    int32_t intermediate_samples[4];
    unsigned i;

    for (i = 0; i < 2; i++)
        polyphase_analysis_single(qmf->outer_filter_signal,
                                  aptx_qmf_outer_coeffs, 23,
                                  &samples[2 * i],
                                  &intermediate_samples[0 + i],
                                  &intermediate_samples[2 + i]);

    for (i = 0; i < 2; i++)
        polyphase_analysis_single(qmf->inner_filter_signal[i],
                                  aptx_qmf_inner_coeffs, 23,
                                  &intermediate_samples[2 * i],
                                  &subband_samples[2 * i + 0],
                                  &subband_samples[2 * i + 1]);

    aptx_qmf_neon_kernel_calls++;
}

void aptx_qmf_tree_synthesis_neon(struct aptx_QMF_analysis *qmf,
                                  const int32_t subband_samples[NB_SUBBANDS],
                                  int32_t samples[4])
{
    int32_t intermediate_samples[4];
    unsigned i;

    for (i = 0; i < 2; i++)
        polyphase_synthesis_single(qmf->inner_filter_signal[i],
                                   aptx_qmf_inner_coeffs, 22,
                                   subband_samples[2 * i + 0],
                                   subband_samples[2 * i + 1],
                                   &intermediate_samples[2 * i]);

    for (i = 0; i < 2; i++)
        polyphase_synthesis_single(qmf->outer_filter_signal,
                                   aptx_qmf_outer_coeffs, 21,
                                   intermediate_samples[0 + i],
                                   intermediate_samples[2 + i],
                                   &samples[2 * i]);

    aptx_qmf_neon_kernel_calls++;
}

#endif
