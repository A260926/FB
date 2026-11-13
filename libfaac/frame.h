/*
 * FAAC - Freeware Advanced Audio Coder
 * Copyright (C) 2001 Menno Bakker
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

#ifndef FRAME_H
#define FRAME_H

/* Input sample FIFO slots, each one frame (FRAME_LEN samples) wide, relative to
   the frame currently being coded (FIFO_CURR): one frame behind (FIFO_PAST,
   reused as the MDCT overlap) and two frames ahead. The two ahead slots are
   needed because the block-switch energy analysis works on 2-frame-wide windows
   and keeps one window of lookahead, whose far edge reaches two frames ahead. */
#define LOOKAHEAD_DEPTH 2
#define FIFO_PAST       0
#define FIFO_CURR       1
#define FIFO_AHEAD1     2
#define FIFO_AHEAD2     3

#include "faac_internal.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "coder.h"
#include "channels.h"
#include "blockswitch.h"
#include "fft.h"
#include "quantize.h"
#include "sbr.h"
#include "stats.h"
#include "ratecontrol.h"
#include "stereo.h"

typedef struct faacEncStruct {
    /* number of channels in AAC file */
    unsigned int numChannels;

    /* samplerate of AAC file */
    unsigned long sampleRate;
    unsigned int sampleRateIdx;

    unsigned int usedBytes;

    /* frame number */
    unsigned int frameNum;
    unsigned int flushFrame;

    /* Scalefactorband data */
    SR_INFO *srInfo;
    /* Prefix sums of srInfo's cb_width tables, built once per config. */
    int sfbOffsetLong[NSFB_LONG + 1];
    int sfbOffsetShort[NSFB_SHORT + 1];

    /* sample buffers: FIFO_PAST (MDCT overlap), FIFO_CURR, FIFO_AHEAD1, FIFO_AHEAD2 */
    float *audioFIFO[MAX_CHANNELS][4];

    /* Filterbank buffers */
    float *freqBuff[MAX_CHANNELS];

    /* Channel and Coder data for all channels */
    CoderInfo coderInfo[MAX_CHANNELS];

    /* Element-centric configuration */
    AACElement elements[MAX_CHANNELS];
    int numElements;
    bool isLfeChannel[MAX_CHANNELS]; /* per-channel LFE lookup, derived from elements[] whenever it changes */

    /* Explicit PCE-driven layout. customPce is false for legacy,
     * channel-count-derived layouts; when true pcePlan owns the element
     * ordering and every raw_data_block/ASC carries the matching PCE. */
    bool customPce;
    PcePlan pcePlan;

    /* Psychoacoustics data */
    PsyInfo psyInfo[MAX_CHANNELS];
    GlobalPsyInfo gpsyInfo;

    /* Configuration data */
    faacEncConfiguration config;

    /* quantizer specific config */
    AACQuantCfg aacquantCfg;

    StereoConfig stereoCfg;

    /* FFT Tables */

    /* Input FIFO: decouples the caller's per-call chunk size from the encoder
     * frame size. faacEncEncode appends whatever it is handed (any count) and
     * emits one frame once a full frame (mult*FRAME_LEN samples/ch, mult = 2 for
     * HE-AAC, 1 for LC) has accumulated. Stores format-converted float. */
    float    *inputFifo[MAX_CHANNELS];
    unsigned int  inputFifoFill;     /* samples per channel currently buffered */
    unsigned int  inputFifoCap;      /* per-channel capacity in samples */

    /* faac_* API: AudioSpecificConfig cached on first request and owned by the
     * handle (freed at close), so faac_encoder_asc() can hand back a pointer
     * the caller never frees. NULL until first built. */
    unsigned char *ascCache;
    unsigned long  ascCacheLen;

    /* HE-AAC / SBR state */
    struct SBRContext *sbrContext;   /* SBR analysis state and bitstream data */

    /* Peak-limiter retry scratch: one buffer per channel holding book[] at
     * [0] and sf[] at [MAX_SCFAC_BANDS]. */
    int *peakSnap[MAX_CHANNELS];

    RateControl rc;

    /* Read-only observation of emitted frames, for tests and diagnostics. All
     * bit offsets are relative to the frame's first bit (so they include the
     * 56-bit ADTS header in ADTS output). BuildFrame fills the transient obs*
     * fields on every write (the peak-limiter retry loop rewrites them); once
     * a frame is actually produced faacEncEncode commits them into the ring,
     * one entry per output frame. The encoder never reads these back; they do
     * not affect coding decisions. */
    int obsPceStart;                     /* in-band PCE start bit, custom mode */
    int obsPceLen;                       /* in-band PCE length, bits */
    int obsElemStart[MAX_CHANNELS];      /* each element's ID field start */
    int obsElemLen[MAX_CHANNELS];        /* each element's total length */
    int obsEndBit;                       /* ID_END position */

#define OBS_RING 48
    struct {
        int valid;
        int pceStart, pceLen, endBit;
        int nElems;
        int elemStart[MAX_CHANNELS];
        int elemLen[MAX_CHANNELS];
        int elemType[MAX_CHANNELS];
        int elemTag[MAX_CHANNELS];
    } obsRing[OBS_RING];
    unsigned int obsRingCount;          /* produced frames committed */
} faacEncStruct;

/* Configuration worker behind faac_encoder_open(): validates the config,
 * resolves AUTO/HE-AAC, and (re)initializes the encoder. Returns 1 on success,
 * 0 on failure. */
int faacEncApplyConfig(faacEncStruct* hEncoder,
                       faacEncConfigurationPtr config);

/* Samples/channel per full frame: HE-AAC's core runs dual-rate at Fs/2, so it
 * needs two FRAME_LENs of input to emit one frame at the full rate; LC needs one. */
static inline unsigned int faacFrameSamples(const faacEncStruct *hEncoder)
{
    return (hEncoder->config.aacObjectType == HE_V1) ? 2 * FRAME_LEN : FRAME_LEN;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* FRAME_H */
