/*
 * Independent scalar reference QMF implementation for the NEON kernel
 * tests. It is deliberately written from the mathematical specification
 * (16 tap int64 dot product, explicit midpoint rounding, explicit 24bit
 * saturation, circular buffer semantics) rather than calling any code under
 * test, so it can serve as an oracle for qmf_neon.c.
 */

#ifndef QMF_REF_H
#define QMF_REF_H

#include <stdint.h>

#include "openaptx_internal.h"

static inline int64_t ref_rshift64(int64_t value, unsigned shift)
{
    const int64_t rounding = (int64_t)1 << (shift - 1);
    const int64_t mask = ((int64_t)1 << (shift + 1)) - 1;
    return ((value + rounding) >> shift) - ((value & mask) == rounding);
}

static inline int32_t ref_clip_intp2(int32_t a, unsigned p)
{
    if (((uint32_t)a + ((uint32_t)1 << p)) & ~(((uint32_t)2 << p) - 1))
        return (a >> 31) ^ ((1 << p) - 1);
    else
        return a;
}

static inline int32_t ref_rshift64_clip24(int64_t value, unsigned shift)
{
    return ref_clip_intp2((int32_t)ref_rshift64(value, shift), 23);
}

static inline void ref_push(struct aptx_filter_signal *signal, int32_t sample)
{
    signal->buffer[signal->pos] = sample;
    signal->buffer[signal->pos + FILTER_TAPS] = sample;
    signal->pos = (uint8_t)((signal->pos + 1) & (FILTER_TAPS - 1));
}

/* Independent 64bit scalar dot product, one multiplication at a time. */
static inline int64_t ref_dot16(const struct aptx_filter_signal *signal,
                                const int32_t coeffs[FILTER_TAPS])
{
    const int32_t *sig = &signal->buffer[signal->pos];
    int64_t e = 0;
    unsigned i;

    for (i = 0; i < FILTER_TAPS; i++)
        e += (int64_t)sig[i] * (int64_t)coeffs[i];

    return e;
}

static inline int32_t ref_convolution(const struct aptx_filter_signal *signal,
                                      const int32_t coeffs[FILTER_TAPS],
                                      unsigned shift)
{
    return ref_rshift64_clip24(ref_dot16(signal, coeffs), shift);
}

static inline void ref_polyphase_analysis(
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
        ref_push(&signal[i], samples[NB_FILTERS - 1 - i]);
        subbands[i] = ref_convolution(&signal[i], coeffs[i], shift);
    }

    *low_subband_output = ref_clip_intp2(subbands[0] + subbands[1], 23);
    *high_subband_output = ref_clip_intp2(subbands[0] - subbands[1], 23);
}

static inline void ref_polyphase_synthesis(
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
        ref_push(&signal[i], subbands[1 - i]);
        samples[i] = ref_convolution(&signal[i], coeffs[i], shift);
    }
}

static inline void ref_tree_analysis(struct aptx_QMF_analysis *qmf,
                                     const int32_t samples[4],
                                     int32_t subband_samples[NB_SUBBANDS])
{
    int32_t intermediate_samples[4];
    unsigned i;

    for (i = 0; i < 2; i++)
        ref_polyphase_analysis(qmf->outer_filter_signal,
                               aptx_qmf_outer_coeffs, 23,
                               &samples[2 * i],
                               &intermediate_samples[0 + i],
                               &intermediate_samples[2 + i]);

    for (i = 0; i < 2; i++)
        ref_polyphase_analysis(qmf->inner_filter_signal[i],
                               aptx_qmf_inner_coeffs, 23,
                               &intermediate_samples[2 * i],
                               &subband_samples[2 * i + 0],
                               &subband_samples[2 * i + 1]);
}

static inline void ref_tree_synthesis(struct aptx_QMF_analysis *qmf,
                                      const int32_t subband_samples[NB_SUBBANDS],
                                      int32_t samples[4])
{
    int32_t intermediate_samples[4];
    unsigned i;

    for (i = 0; i < 2; i++)
        ref_polyphase_synthesis(qmf->inner_filter_signal[i],
                                aptx_qmf_inner_coeffs, 22,
                                subband_samples[2 * i + 0],
                                subband_samples[2 * i + 1],
                                &intermediate_samples[2 * i]);

    for (i = 0; i < 2; i++)
        ref_polyphase_synthesis(qmf->outer_filter_signal,
                                aptx_qmf_outer_coeffs, 21,
                                intermediate_samples[0 + i],
                                intermediate_samples[2 + i],
                                &samples[2 * i]);
}

#endif
