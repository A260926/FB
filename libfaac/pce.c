/*
 * FAAC - Freeware Advanced Audio Coder
 * Copyright (C) 2026 Nils Schimmelmann
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <string.h>
#include <stdint.h>

#include <faac.h>
#include "pce.h"
#include "util.h"

/* Region traversal order for element_order[]: front, side, back, then LFE. */
static const enum faac_pce_region region_order[] = {
    FAAC_PCE_REGION_FRONT,
    FAAC_PCE_REGION_SIDE,
    FAAC_PCE_REGION_BACK,
    FAAC_PCE_REGION_LFE,
};

int PcePlanBuild(PcePlan *plan, const faac_pce_element *list,
                 uint32_t count, uint32_t numChannels)
{
    uint8_t used[MAX_CHANNELS];
    uint32_t i, slot, lfe_count, front_count;
    int out, sce_tag, cpe_tag, lfe_tag;

    memset(plan, 0, sizeof(*plan));

    if (list == NULL || count == 0 || count > MAX_CHANNELS)
        return 0;
    if (numChannels < 1 || numChannels > MAX_CHANNELS)
        return 0;

    memset(used, 0, sizeof(used));
    lfe_count = 0;
    front_count = 0;

    for (i = 0; i < count; i++) {
        const faac_pce_element *e = &list[i];
        uint32_t c0, c1;

        if (e->reserved[0] || e->reserved[1] || e->reserved[2])
            return 0;
        switch (e->region) {
            case FAAC_PCE_REGION_FRONT: front_count++; break;
            case FAAC_PCE_REGION_SIDE:
            case FAAC_PCE_REGION_BACK:  break;
            case FAAC_PCE_REGION_LFE:
                if (e->is_cpe)
                    return 0;              /* an LFE can never be paired */
                lfe_count++;
                break;
            default:
                return 0;                  /* invalid region */
        }
        if (lfe_count > 1)
            return 0;

        c0 = e->channels[0];
        c1 = e->channels[1];

        if (e->is_cpe) {
            if (c0 >= numChannels || c1 >= numChannels || c0 == c1)
                return 0;
        } else {
            if (c0 >= numChannels || c1 != UINT32_MAX)
                return 0;
        }
        if (used[c0])
            return 0;                      /* slot used twice */
        used[c0] = 1;
        if (e->is_cpe) {
            if (used[c1])
                return 0;
            used[c1] = 1;
        }
    }

    /* Every logical slot must appear exactly once. */
    for (slot = 0; slot < numChannels; slot++)
        if (!used[slot])
            return 0;
    if (front_count == 0)
        return 0;

    /* Materialize elements in declaration order first. */
    for (i = 0; i < count; i++) {
        const faac_pce_element *e = &list[i];
        AACElement *el = &plan->elems[i];

        el->region = (uint8_t)e->region;
        el->common_window = false;
        el->channels[0] = (int)e->channels[0];
        if (e->is_cpe) {
            el->type = ID_CPE;
            el->channels[1] = (int)e->channels[1];
        } else {
            el->type = (e->region == FAAC_PCE_REGION_LFE) ? ID_LFE : ID_SCE;
            el->channels[1] = -1;
        }
    }
    plan->num_elements = (int)count;

    /* Group by region, keeping declaration order inside each group. */
    out = 0;
    for (unsigned r = 0; r < sizeof(region_order) / sizeof(region_order[0]); r++) {
        for (i = 0; i < count; i++) {
            if (list[i].region == region_order[r])
                plan->element_order[out++] = (int)i;
        }
    }

    /* Per-type tags number the final, grouped order from 0. */
    sce_tag = cpe_tag = lfe_tag = 0;
    for (i = 0; i < (uint32_t)plan->num_elements; i++) {
        AACElement *el = &plan->elems[plan->element_order[i]];
        switch (el->type) {
            case ID_SCE: el->tag = (uint8_t)sce_tag++; plan->num_sce++; break;
            case ID_CPE: el->tag = (uint8_t)cpe_tag++; plan->num_cpe++; break;
            case ID_LFE: el->tag = (uint8_t)lfe_tag++; plan->num_lfe++; break;
            default: return 0;
        }
    }

    return 1;
}

/* Fixed header: element_instance_tag(4) + object_type(2) + sr index(4) +
 * six 4-bit element/count fields + the three 1-bit mixdown present flags.
 * The in-band form additionally carries the 3-bit ID_PCE prefix. */
static unsigned int pce_header_bits(int withId)
{
    return (withId ? 3u : 0u) + 4u + 2u + 4u + 6u * 4u + 3u;
}

unsigned int PceSizeBits(const PcePlan *plan, int withId)
{
    unsigned int bits = pce_header_bits(withId);
    int i;

    for (i = 0; i < plan->num_elements; i++) {
        const AACElement *el = &plan->elems[plan->element_order[i]];
        /* front/side/back: is_cpe(1) + tag(4); LFE: tag(4) only. */
        bits += (el->type == ID_LFE) ? 4u : 5u;
    }
    bits += 8u;                 /* comment_field_bytes = 0 */
    return (bits + 7u) & ~7u;  /* byte_alignment() */
}

unsigned int PceWrite(BitStream *bs, const PcePlan *plan,
                      unsigned int coreRateIdx, int withId)
{
    const unsigned int start = bs->currentBit;
    unsigned int n_front = 0, n_side = 0, n_back = 0, n_lfe = 0;
    int i;

    if (withId)
        PutBit(bs, ID_PCE, 3);

    PutBit(bs, 0, 4);                        /* element_instance_tag */
    /* PCE object_type carries the profile number (LC = AOT-1), the same
     * encoding as the ADTS profile field. */
    PutBit(bs, FAAC_OBJ_LOW - 1, 2);
    PutBit(bs, coreRateIdx, 4);             /* core sample rate index */

    for (i = 0; i < plan->num_elements; i++) {
        switch ((enum faac_pce_region)plan->elems[plan->element_order[i]].region) {
            case FAAC_PCE_REGION_FRONT: n_front++; break;
            case FAAC_PCE_REGION_SIDE:  n_side++;  break;
            case FAAC_PCE_REGION_BACK:  n_back++;  break;
            case FAAC_PCE_REGION_LFE:   n_lfe++;   break;
            default: break;
        }
    }
    PutBit(bs, n_front, 4);                 /* num_front_channel_elements */
    PutBit(bs, n_side, 4);                  /* num_side_channel_elements */
    PutBit(bs, n_back, 4);                  /* num_back_channel_elements */
    PutBit(bs, n_lfe, 4);                   /* num_lfe_channel_elements */
    PutBit(bs, 0, 4);                       /* num_assoc_data_elements */
    PutBit(bs, 0, 4);                       /* num_valid_cc_elements */
    PutBit(bs, 0, 1);                       /* mono_mixdown_present */
    PutBit(bs, 0, 1);                       /* stereo_mixdown_present */
    PutBit(bs, 0, 1);                       /* matrix_mixdown_present */

    /* Element loops, in region group order. */
    for (i = 0; i < plan->num_elements; i++) {
        const AACElement *el = &plan->elems[plan->element_order[i]];
        enum faac_pce_region region = (enum faac_pce_region)el->region;
        if (region == FAAC_PCE_REGION_LFE)
            continue;
        PutBit(bs, el->type == ID_CPE ? 1 : 0, 1);
        PutBit(bs, el->tag, 4);
    }
    for (i = 0; i < plan->num_elements; i++) {
        const AACElement *el = &plan->elems[plan->element_order[i]];
        if ((enum faac_pce_region)el->region == FAAC_PCE_REGION_LFE)
            PutBit(bs, el->tag, 4);
    }
    /* assoc-data and valid-CC loops are empty: their counts are zero. */

    PutBit(bs, 0, 8);                       /* comment_field_bytes = 0 */

    {
        unsigned int pad = (8u - ((bs->currentBit - start) & 7u)) & 7u;
        if (pad)
            PutBit(bs, 0, (int)pad);        /* byte_alignment() */
    }

    return bs->currentBit - start;
}
