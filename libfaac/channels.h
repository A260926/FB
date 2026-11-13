/*
 * FAAC - Freeware Advanced Audio Coder
 * Copyright (C) 2026 Nils Schimmelmann
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
 */

#ifndef CHANNEL_H
#define CHANNEL_H

#include <stdbool.h>
#include <stdint.h>
#include "bitstream.h"
#include "coder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration for core struct to avoid circular dependency */
struct faacEncStruct;

/**
 * AAC Syntax Element identifiers (ISO/IEC 14496-3, Table 4.1).
 */
typedef enum {
    ID_SCE = 0, /**< Single Channel Element */
    ID_CPE = 1, /**< Channel Pair Element   */
    ID_CCE = 2, /**< Coupling Channel Element */
    ID_LFE = 3, /**< Low Frequency Element    */
    ID_DSE = 4, /**< Data Stream Element      */
    ID_PCE = 5, /**< Program Config Element   */
    ID_FIL = 6, /**< Fill Element             */
    ID_END = 7  /**< Terminator               */
} ElementID;

/**
 * Bitstream field lengths (ISO/IEC 14496-3).
 */
enum {
    LEN_SE_ID           = 3,
    LEN_TAG             = 4,
    LEN_COM_WIN         = 1,
    LEN_ICS_RESERV      = 1,
    LEN_WIN_SEQ         = 2,
    LEN_WIN_SH          = 1,
    LEN_MAX_SFBL        = 6,
    LEN_MAX_SFBS        = 4,
    LEN_GLOB_GAIN       = 8,
    LEN_PRED_PRES       = 1,
    LEN_MASK_PRES       = 2,
    LEN_MASK            = 1,
    LEN_PULSE_PRES      = 1,
    LEN_TNS_PRES        = 1,
    LEN_GAIN_PRES       = 1,
    LEN_TNS_NFILTL      = 2,
    LEN_TNS_NFILTS      = 1,
    LEN_TNS_COEFF_RES   = 1,
    LEN_TNS_LENGTHL     = 6,
    LEN_TNS_LENGTHS     = 4,
    LEN_TNS_ORDERL      = 5,
    LEN_TNS_ORDERS      = 3,
    LEN_TNS_DIRECTION   = 1,
    LEN_TNS_COMPRESS    = 1,
    LEN_ADTS_SYNC       = 12,
    LEN_ADTS_ID         = 1,
    LEN_ADTS_LAYER      = 2,
    LEN_ADTS_ABSENT     = 1,
    LEN_ADTS_PROFILE    = 2,
    LEN_ADTS_FREQ       = 4,
    LEN_ADTS_PRIV       = 1,
    LEN_ADTS_CH_CFG     = 3,
    LEN_ADTS_ORIG       = 1,
    LEN_ADTS_HOME       = 1,
    LEN_ADTS_COPY_ID    = 1,
    LEN_ADTS_COPY_ST    = 1,
    LEN_ADTS_FRAME      = 13,
    LEN_ADTS_FULL       = 11,
    LEN_ADTS_BLOCKS     = 2
};

typedef struct {
    bool is_present;
    uint8_t ms_used[MAX_SCFAC_BANDS];
} MSInfo;

typedef struct {
    ElementID type;
    uint8_t tag;
    int channels[2];
    bool common_window;
    MSInfo msInfo;
    /* PCE soundfield region (mirrors faac_pce_region); only meaningful on a
     * handle running an explicit PCE plan. Legacy layouts leave it FRONT. */
    uint8_t region;
} AACElement;

/*
 * Resolved explicit element plan (PCE-driven mode). Built once at open() from
 * the caller's faac_pce_element list and copied into the handle; the encoder
 * then derives elements[], every in-stream PCE and the ASC from this one copy.
 *
 * element_order[] holds indices into elems[] sorted by region (front, side,
 * back, LFE), declaration order kept within each region. elems[].tag is the
 * per-type (SCE/CPE, then LFE) numbering over that final order.
 */
#define PCE_MAX_ELEMENTS MAX_CHANNELS

typedef struct {
    int num_elements;
    int num_sce;
    int num_cpe;
    int num_lfe;
    AACElement elems[PCE_MAX_ELEMENTS];
    int element_order[PCE_MAX_ELEMENTS];
} PcePlan;

/**
 * Maps channel count to ISO/IEC 14496-3 Table 1.19 channelConfiguration value.
 * Returns 1..6 for 1..6 channels, 7 for 8 channels (7.1 surround), or 0 if unsupported.
 */
static inline int GetChannelConfig(int numChannels)
{
    return (numChannels >= 1 && numChannels <= 6) ? numChannels : (numChannels == 8 ? 7 : 0);
}

int InitElements(AACElement * __restrict elements, int *numElements, int numChannels, bool useLfe);

int WriteElement(BitStream *bs, AACElement *elem, CoderInfo *coder);

int WriteBitstream(struct faacEncStruct* hEncoder,
                   CoderInfo *coderInfo,
                   AACElement *elements,
                   int numElements,
                   BitStream *bitStream);

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_H */
