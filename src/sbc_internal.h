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
 * Internal interfaces shared between the legacy encoder and the offline
 * rate-distortion joint-stereo selector (sbc_rd.c).
 *
 * Nothing declared here is part of the public ABI : the public `sbc_t`
 * layout and the legacy entry points are left untouched.
 */

#ifndef __SBC_INTERNAL_H
#define __SBC_INTERNAL_H

#include <stdbool.h>

#include <sbc.h>

#include "bits.h"


/**
 * Validate a frame description (bitpool, blocks and sub-bands limits)
 */
bool sbc_check_frame(const struct sbc_frame *frame);

/**
 * Run the LOUDNESS / SNR bit allocation for a set of scale factors
 */
void sbc_compute_nbits(const struct sbc_frame *frame,
    const int (*scale_factors)[SBC_MAX_SUBBANDS],
    int (*nbits)[SBC_MAX_SUBBANDS]);

/**
 * Run the analysis filter bank once per channel, advancing the encoder
 * history exactly like the legacy encoding path, and return the raw
 * sub-band samples (fixed 17.14) consumed by the packers.
 */
void sbc_analyze_frame(sbc_t *sbc, const struct sbc_frame *frame,
    const int16_t *pcml, int pitchl,
    const int16_t *pcmr, int pitchr,
    int16_t sb_samples[2][SBC_MAX_SAMPLES]);

/**
 * Pack a frame from already decided joint-stereo mask and scale factors.
 * The bit allocation, coupling, quantization, zero-bit bands and padding
 * are the exact same operations as the legacy packer, so the emitted
 * syntax matches the decisions used during candidate evaluation.
 * `sb_samples` is modified in place by the coupling step.
 */
void sbc_encode_frame_decided(sbc_bits_t *bits,
    const struct sbc_frame *frame,
    int16_t sb_samples[2][SBC_MAX_SAMPLES],
    unsigned mjoint,
    const int scale_factors[2][SBC_MAX_SUBBANDS]);

/**
 * Encode the 4 bytes frame header
 */
void sbc_encode_header(sbc_bits_t *bits, const struct sbc_frame *frame);

/**
 * Write the CRC of an assembled frame
 * return          0 on success, -1 when the buffer is too small
 */
int sbc_put_crc(const struct sbc_frame *frame, void *data, unsigned size);


#endif /* !__SBC_INTERNAL_H */
