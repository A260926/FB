/******************************************************************************
 *
 *  Copyright 2022 Google LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/**
 * Offline rate-distortion joint-stereo encoding.
 *
 * Unlike the legacy packer, which couples a sub-band whenever the local
 * pair of scale factors gets smaller, this entry point enumerates every
 * legal joint-stereo mask of the frame and keeps the mask minimizing the
 * total sub-band squared error for the fixed bitpool. Every candidate runs
 * its own scale-factor computation and full LOUDNESS / SNR allocation, and
 * is quantized from the same single QMF analysis of the input PCM.
 */

#include <stdint.h>

#include <sbc.h>

#include "sbc_internal.h"


/**
 * Fixed-point `1 / (2^nbit - 1)` coefficients, identical to the ones used
 * by the decoder (see decode_frame()).
 */
static const int sbc_rd_range_scale[16] = {
    0xFFFFFFF, 0x5555556, 0x2492492, 0x1111111,
    0x0842108, 0x0410410, 0x0204081, 0x0101010,
    0x0080402, 0x0040100, 0x0020040, 0x0010010,
    0x0008004, 0x0004001, 0x0002000, 0x0001000
};

/**
 * Return 2^(-exponent) without pulling a libm dependency
 */
static double pow2_neg(int exponent)
{
    double value = 1.0;

    if (exponent >= 0) {
        double divisor = (double)(1L << exponent);
        value /= divisor;
    } else {
        value *= (double)(1L << -exponent);
    }

    return value;
}


/**
 * Compute the scale factors of one candidate joint-stereo mask, following
 * compute_scale_factors_js() : coupled bands measure the raw, unwrapped
 * L+R / L-R sums (the halves written in the stream are derived afterwards
 * and may wrap on 16 bits).
 */
static void candidate_scale_factors(const struct sbc_frame *frame,
    const int16_t sb_samples[2][SBC_MAX_SAMPLES], unsigned mjoint,
    int scale_factors[2][SBC_MAX_SUBBANDS])
{
    int nsubbands = frame->nsubbands;

    for (int isb = 0; isb < nsubbands; isb++) {
        unsigned m[2] = { };

        for (int iblk = 0; iblk < frame->nblocks; iblk++) {
            int s0 = sb_samples[0][iblk * nsubbands + isb];
            int s1 = sb_samples[1][iblk * nsubbands + isb];

            int c0 = ((mjoint >> isb) & 1) ? s0 + s1 : s0;
            int c1 = ((mjoint >> isb) & 1) ? s0 - s1 : s1;

            m[0] |= c0 < 0 ? ~c0 : c0;
            m[1] |= c1 < 0 ? ~c1 : c1;
        }

        scale_factors[0][isb] =
            m[0] ? (int)(8 * sizeof(unsigned) - 1) - __builtin_clz(m[0]) : 0;
        scale_factors[1][isb] =
            m[1] ? (int)(8 * sizeof(unsigned) - 1) - __builtin_clz(m[1]) : 0;
    }
}

/**
 * Reconstruct a candidate exactly like the library decoder would, back to
 * the input sub-band scale, and return the total squared error against the
 * raw analyzed L/R samples. No synthesis filter bank is involved.
 */
static double candidate_cost(const struct sbc_frame *frame,
    const int16_t sb_samples[2][SBC_MAX_SAMPLES], unsigned mjoint,
    const int scale_factors[2][SBC_MAX_SUBBANDS])
{
    int nsubbands = frame->nsubbands;
    int nblocks   = frame->nblocks;

    int nbits[2][SBC_MAX_SUBBANDS];
    sbc_compute_nbits(frame, scale_factors, nbits);

    /* --- Decoder sb_scale, shared between the two channels --- */

    int sb_scale[2];

    for (int ich = 0; ich < 2; ich++) {
        int max_scf = 0;

        for (int isb = 0; isb < nsubbands; isb++) {
            int scf = scale_factors[ich][isb] + ((mjoint >> isb) & 1);
            if (scf > max_scf)
                max_scf = scf;
        }

        sb_scale[ich] = (15 - max_scf) - (17 - 16);
    }

    if (sb_scale[0] > sb_scale[1])
        sb_scale[0] = sb_scale[1];
    else
        sb_scale[1] = sb_scale[0];

    /* --- Quantize with the encoder formula and rebuild with the
     *     decoder range_scale / signed shift, per channel ------------- */

    int16_t reconstructed[2][SBC_MAX_SAMPLES];

    for (int ich = 0; ich < 2; ich++)
        for (int iblk = 0; iblk < nblocks; iblk++)
            for (int isb = 0; isb < nsubbands; isb++) {
                int idx = iblk * nsubbands + isb;
                int s0 = sb_samples[0][idx];
                int s1 = sb_samples[1][idx];

                /* The encoder stores the coupled half in an int16 */
                int s = ich == 0 ? s0 : s1;
                if ((mjoint >> isb) & 1)
                    s = (int16_t)(ich == 0 ? (s0 + s1) >> 1
                                           : (s0 - s1) >> 1);

                int nbit = nbits[ich][isb];
                int scf  = scale_factors[ich][isb];
                int value = 0;

                if (nbit) {
                    int range = ~(UINT_MAX << nbit);
                    int q = (((s * range) >> (scf + 1)) + range) >> 1;

                    int built = ((q << 1) | 1) *
                                sbc_rd_range_scale[nbit - 1];

                    value = (built - (1 << 28)) >>
                            (28 - ((scf + 1) + sb_scale[ich]));
                }

                reconstructed[ich][idx] = (int16_t)value;
            }

    /* --- Inverse joint-stereo uncoupling, with the same 16-bit wrap --- */

    for (int isb = 0; isb < nsubbands; isb++) {
        if (((mjoint >> isb) & 1) == 0)
            continue;

        for (int iblk = 0; iblk < nblocks; iblk++) {
            int idx = iblk * nsubbands + isb;
            int16_t s0 = reconstructed[0][idx];
            int16_t s1 = reconstructed[1][idx];

            reconstructed[0][idx] = (int16_t)(s0 + s1);
            reconstructed[1][idx] = (int16_t)(s0 - s1);
        }
    }

    /* --- Squared error on the input sub-band scale --------------------- */

    double gain = pow2_neg(sb_scale[0]);
    double cost = 0;

    for (int ich = 0; ich < 2; ich++)
        for (int i = 0; i < nblocks * nsubbands; i++) {
            double error =
                (double)reconstructed[ich][i] * gain -
                (double)sb_samples[ich][i];
            cost += error * error;
        }

    return cost;
}

/**
 * Encode a frame with global rate-distortion joint-stereo selection
 */
int sbc_encode_rd(sbc_t *sbc,
    const int16_t *pcml, int pitchl, const int16_t *pcmr, int pitchr,
    const struct sbc_frame *frame, void *data, unsigned size)
{
    /* --- Only non-mSBC joint-stereo is supported : refuse anything else
     *     before touching the analysis history or the output buffer ----- */

    if (frame->msbc || frame->mode != SBC_MODE_JOINT_STEREO)
        return -1;

    if (!sbc_check_frame(frame) || size < sbc_get_frame_size(frame))
        return -1;

    /* --- Single QMF analysis shared read-only by every candidate ------- */

    int16_t alignas(sizeof(int)) sb_samples[2][SBC_MAX_SAMPLES];

    sbc_analyze_frame(sbc, frame, pcml, pitchl, pcmr, pitchr, sb_samples);

    /* --- Enumerate the legal masks : the highest sub-band stays free --- */

    int nsubbands = frame->nsubbands;
    int nmasks = 1 << (nsubbands - 1);

    double costs[1 << (SBC_MAX_SUBBANDS - 1)];
    int    scale_factors[1 << (SBC_MAX_SUBBANDS - 1)][2][SBC_MAX_SUBBANDS];

    double min_cost = 0;

    for (int m = 0; m < nmasks; m++) {
        candidate_scale_factors(frame, sb_samples, (unsigned)m,
                                scale_factors[m]);
        costs[m] = candidate_cost(frame, sb_samples, (unsigned)m,
                                  scale_factors[m]);
        if (m == 0 || costs[m] < min_cost)
            min_cost = costs[m];
    }

    /* --- Keep the smallest mask inside the numerical tie window ------- */

    double reference = min_cost > 1.0 ? min_cost : 1.0;
    double tolerance = 1e-12 * reference;
    unsigned mjoint = 0;

    for (int m = 0; m < nmasks; m++)
        if (costs[m] <= min_cost + tolerance) {
            mjoint = (unsigned)m;
            break;
        }

    /* --- Pack from the selected decisions; the pristine analysis output
     *     is copied so candidate evaluation never mutated it ------------- */

    int16_t alignas(sizeof(int)) pack_samples[2][SBC_MAX_SAMPLES];
    int nsamples = frame->nblocks * nsubbands;

    for (int ich = 0; ich < 2; ich++)
        for (int i = 0; i < nsamples; i++)
            pack_samples[ich][i] = sb_samples[ich][i];

    sbc_bits_t bits;

    sbc_setup_bits(&bits, SBC_BITS_WRITE,
        (void *)((uintptr_t)data + SBC_HEADER_SIZE),
        sbc_get_frame_size(frame) - SBC_HEADER_SIZE);
    sbc_encode_frame_decided(&bits, frame, pack_samples, mjoint,
                             scale_factors[mjoint]);
    sbc_flush_bits(&bits);

    sbc_setup_bits(&bits, SBC_BITS_WRITE, data, SBC_HEADER_SIZE);
    sbc_encode_header(&bits, frame);
    sbc_flush_bits(&bits);

    return sbc_put_crc(frame, data, size);
}
