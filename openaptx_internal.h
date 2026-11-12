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
 * Private shared interface between the scalar codec in openaptx.c and the
 * AArch64 NEON QMF kernels in qmf_neon.c. Nothing declared here is part of
 * the public API (see openaptx.h); the public context stays opaque.
 *
 * The NEON kernels keep the exact in-memory QMF layout used by the scalar
 * implementation, so the two paths can be compared state-for-state and the
 * ring buffer / pos semantics remain identical.
 */

#ifndef OPENAPTX_INTERNAL_H
#define OPENAPTX_INTERNAL_H

#include <stdint.h>

#if (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L) && !defined(inline)
#define inline
#endif

#define NB_SUBBANDS 4
#define NB_FILTERS 2
#define FILTER_TAPS 16
#define LATENCY_SAMPLES 90

enum channels {
    LEFT,
    RIGHT,
    NB_CHANNELS
};

struct aptx_filter_signal {
    int32_t buffer[2*FILTER_TAPS];
    uint8_t pos;
};

struct aptx_QMF_analysis {
    struct aptx_filter_signal outer_filter_signal[NB_FILTERS];
    struct aptx_filter_signal inner_filter_signal[NB_FILTERS][NB_FILTERS];
};

/*
 * Clip a signed integer into the -(2^p),(2^p-1) range.
 * Bit-for-bit identical to the original scalar helper.
 */
static inline int32_t clip_intp2(int32_t a, unsigned p)
{
    if (((uint32_t)a + ((uint32_t)1 << p)) & ~(((uint32_t)2 << p) - 1))
        return (a >> 31) ^ ((1 << p) - 1);
    else
        return a;
}

/*
 * Rounded 64bit right shift with the original midpoint convention.
 * The exact midpoint rounds towards negative infinity, everything else
 * rounds to nearest; this must not be replaced by NEON rounding/narrow
 * instructions, which implement a different tie rule.
 */
static inline int64_t rshift64(int64_t value, unsigned shift)
{
    const int64_t rounding = (int64_t)1 << (shift - 1);
    const int64_t mask = ((int64_t)1 << (shift + 1)) - 1;
    return ((value + rounding) >> shift) - ((value & mask) == rounding);
}

static inline int32_t rshift64_clip24(int64_t value, unsigned shift)
{
    return clip_intp2((int32_t)rshift64(value, shift), 23);
}

/*
 * QMF convolution coefficients, defined in openaptx.c.
 * The 2 sets of each table are mirrors of each other.
 */
extern const int32_t aptx_qmf_outer_coeffs[NB_FILTERS][FILTER_TAPS];
extern const int32_t aptx_qmf_inner_coeffs[NB_FILTERS][FILTER_TAPS];

#if defined(__aarch64__) && !defined(APTX_FORCE_SCALAR)
#define APTX_NEON_KERNELS 1
#else
#define APTX_NEON_KERNELS 0
#endif

#if APTX_NEON_KERNELS

/*
 * Number of QMF tree executions performed by the NEON kernels (a stereo
 * entry counts one call per channel tree). Test and benchmark code uses it
 * to prove that the NEON path was really exercised.
 */
extern unsigned long long aptx_qmf_neon_kernel_calls;

/* One 16-tap convolution, 32bit products accumulated in 64bit, scalar
 * rshift64() midpoint rounding and 24bit saturation afterwards. */
int32_t aptx_qmf_convolution_neon(const struct aptx_filter_signal *signal,
                                  const int32_t coeffs[FILTER_TAPS],
                                  unsigned shift);

/* Single channel two stage QMF trees. */
void aptx_qmf_tree_analysis_neon(struct aptx_QMF_analysis *qmf,
                                 const int32_t samples[4],
                                 int32_t subband_samples[NB_SUBBANDS]);
void aptx_qmf_tree_synthesis_neon(struct aptx_QMF_analysis *qmf,
                                  const int32_t subband_samples[NB_SUBBANDS],
                                  int32_t samples[4]);

/*
 * Stereo variants: the left/right polyphase branches are processed together
 * so each coefficient vector is loaded once for both channels and the two
 * channels share every horizontal reduction. Both QMF stages (outer and
 * inner) are covered.
 */
void aptx_qmf_tree_analysis_stereo_neon(struct aptx_QMF_analysis *left_qmf,
                                        struct aptx_QMF_analysis *right_qmf,
                                        const int32_t left_samples[4],
                                        const int32_t right_samples[4],
                                        int32_t left_subbands[NB_SUBBANDS],
                                        int32_t right_subbands[NB_SUBBANDS]);
void aptx_qmf_tree_synthesis_stereo_neon(struct aptx_QMF_analysis *left_qmf,
                                         struct aptx_QMF_analysis *right_qmf,
                                         const int32_t left_subbands[NB_SUBBANDS],
                                         const int32_t right_subbands[NB_SUBBANDS],
                                         int32_t left_samples[4],
                                         int32_t right_samples[4]);

#endif

#endif
