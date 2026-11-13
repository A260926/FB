/*
 * FAAC - Freeware Advanced Audio Coder
 * Copyright (C) 2026 Nils Schimmelmann
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * Explicit element plans (PCE-driven mode): validation/normalization of the
 * public faac_pce_element list into the handle's PcePlan, and Program Config
 * Element bitstream writing for the AudioSpecificConfig and for in-band PCEs
 * at the start of every raw_data_block.
 */

#ifndef PCE_H
#define PCE_H

#include <stdint.h>
#include <faac.h>
#include "bitstream.h"
#include "channels.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Validate list[0..count) against numChannels and build a normalized plan in
 * *plan. Returns 1 on success, 0 if any rule is violated (NULL pointer with a
 * non-zero count, an out-of-range slot or region, duplicated/missing slots,
 * SCE/LFE with a second slot that is not UINT32_MAX, equal CPE slots, a paired
 * LFE, more than one LFE, or no front element). On failure *plan is left
 * zeroed. numChannels must be in 1..MAX_CHANNELS.
 */
int PcePlanBuild(PcePlan *plan, const faac_pce_element *list,
                 uint32_t count, uint32_t numChannels);

/*
 * Write one program_config_element. With withId != 0 the 3-bit ID_PCE
 * prefix of a raw_data_block is written first; the PCE embedded in an
 * AudioSpecificConfig omits it. The stream is byte-aligned at the call site,
 * so the trailing byte_alignment() pad is computed from bit 0 and included in
 * the returned bit count. object_type is LC and the rate index is coreRateIdx.
 */
unsigned int PceWrite(BitStream *bs, const PcePlan *plan,
                      unsigned int coreRateIdx, int withId);

/* Size PceWrite() would produce, in bits (including alignment padding). */
unsigned int PceSizeBits(const PcePlan *plan, int withId);

#ifdef __cplusplus
}
#endif

#endif /* PCE_H */
