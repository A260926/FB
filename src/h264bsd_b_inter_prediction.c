/*
 * Copyright (C) 2009 The Android Open Source Project
 * Modified for use by h264bsd standalone library
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*------------------------------------------------------------------------------

    B slice explicit inter prediction. Implements the explicit macroblock
    types 1..22 and sub-macroblock types 1..12, motion vector derivation of
    H.264 8.4.1.3 and unweighted bi-prediction (8.4.2.3.2, average of the
    two list predictions).

------------------------------------------------------------------------------*/

#include "h264bsd_b_inter_prediction.h"
#include "h264bsd_neighbour.h"
#include "h264bsd_util.h"
#include "h264bsd_reconstruct.h"
#include "h264bsd_dpb.h"
#include "h264bsd_slice_header.h"

/*------------------------------------------------------------------------------
    Module defines
------------------------------------------------------------------------------*/

#define USE_L0  0x1
#define USE_L1  0x2

typedef struct
{
    u32 available;
    u32 listMask;   /* lists used by the neighbour block */
    u32 refIndex;
    u8 *refAddr;
    mv_t mv;
} bInterNeighbour_t;

/*------------------------------------------------------------------------------
    Local function prototypes
------------------------------------------------------------------------------*/

static u32 BMvPred16x16(mbStorage_t *pMb, mbPred_t *mbPred,
    dpbStorage_t *dpb);
static u32 BMvPred16x8(mbStorage_t *pMb, mbPred_t *mbPred,
    dpbStorage_t *dpb);
static u32 BMvPred8x16(mbStorage_t *pMb, mbPred_t *mbPred,
    dpbStorage_t *dpb);
static u32 BMvPred8x8(mbStorage_t *pMb, subMbPred_t *subMbPred,
    dpbStorage_t *dpb);
static u32 BMvPrediction(mbStorage_t *pMb, subMbPred_t *subMbPred,
    dpbStorage_t *dpb, u32 mbPartIdx, u32 subMbPartIdx, u32 list);

static void GetBInterNeighbour(u32 sliceId, mbStorage_t *nMb,
    bInterNeighbour_t *n, u32 index, u32 list);
static i32 MedianFilter3(i32 a, i32 b, i32 c);
static u32 ScaledMv(dpbStorage_t *dpb, mv_t *out, mv_t in,
    u8 *refAddrCur, u8 *refAddrNeigh, u32 list);
static u32 ClipMv(mv_t *mv);

static void PredictPart(u8 *data, mv_t *mv, u8 *refAddr, image_t *refImage,
    u32 col, u32 row, u32 partX, u32 partY, u32 w, u32 h);
static void BiPredictPart(u8 *data, mv_t *mv0, u8 *addr0, mv_t *mv1,
    u8 *addr1, image_t *refImage, u32 col, u32 row,
    u32 partX, u32 partY, u32 w, u32 h);
static void AvgBlocks(u8 *dst, const u8 *p0, const u8 *p1, u32 n);

/*------------------------------------------------------------------------------

    Function: h264bsdBInterPrediction

------------------------------------------------------------------------------*/

u32 h264bsdBInterPrediction(mbStorage_t *pMb, macroblockLayer_t *pMbLayer,
    dpbStorage_t *dpb, u32 mbNum, image_t *currImage, u8 *data)
{

    u32 row, col, mbType, partMode, i, x, y;
    subMbPartMode_e subPartMode;
    image_t refImage;
    u32 tmp;

/* Code */

    ASSERT(pMb);
    ASSERT(pMbLayer);

    row = mbNum / currImage->width;
    col = mbNum - row * currImage->width;
    row *= 16;
    col *= 16;

    refImage.width = currImage->width;
    refImage.height = currImage->height;

    mbType = (u32)pMbLayer->mbType;

    if (mbType == B_8x8)
    {
        if (BMvPred8x8(pMb, &pMbLayer->subMbPred, dpb) != HANTRO_OK)
            return(HANTRO_NOK);
        for (i = 0; i < 4; i++)
        {
            u32 mode = h264bsdBSubMbPartPredMode(pMbLayer->subMbPred.subMbType[i]);
            subPartMode =
                h264bsdBSubMbPartMode(pMbLayer->subMbPred.subMbType[i]);
            x = i & 0x1 ? 8 : 0;
            y = i < 2 ? 0 : 8;
            switch (subPartMode)
            {
                case MB_SP_8x8:
                    if (mode == B_PRED_BI)
                        BiPredictPart(data, pMb->mv+4*i, pMb->refAddr[i],
                            pMb->mvL1+4*i, pMb->refAddrL1[i], &refImage,
                            col, row, x, y, 8, 8);
                    else
                        PredictPart(data, pMb->mv+4*i,
                            mode==B_PRED_L0 ? pMb->refAddr[i] :
                                             pMb->refAddrL1[i],
                            &refImage, col, row, x, y, 8, 8);
                    break;
                case MB_SP_8x4:
                    /* two 8x4 sub-partitions stacked vertically */
                    if (mode == B_PRED_BI)
                    {
                        BiPredictPart(data, pMb->mv+4*i, pMb->refAddr[i],
                            pMb->mvL1+4*i, pMb->refAddrL1[i], &refImage,
                            col, row, x, y, 8, 4);
                        BiPredictPart(data, pMb->mv+4*i+2, pMb->refAddr[i],
                            pMb->mvL1+4*i+2, pMb->refAddrL1[i], &refImage,
                            col, row, x, y+4, 8, 4);
                    }
                    else
                    {
                        u8 *addr = mode==B_PRED_L0 ? pMb->refAddr[i] :
                                                     pMb->refAddrL1[i];
                        PredictPart(data, pMb->mv+4*i, addr, &refImage,
                            col, row, x, y, 8, 4);
                        PredictPart(data, pMb->mv+4*i+2, addr, &refImage,
                            col, row, x, y+4, 8, 4);
                    }
                    break;
                case MB_SP_4x8:
                    /* two 4x8 sub-partitions placed side by side */
                    if (mode == B_PRED_BI)
                    {
                        BiPredictPart(data, pMb->mv+4*i, pMb->refAddr[i],
                            pMb->mvL1+4*i, pMb->refAddrL1[i], &refImage,
                            col, row, x, y, 4, 8);
                        BiPredictPart(data, pMb->mv+4*i+1, pMb->refAddr[i],
                            pMb->mvL1+4*i+1, pMb->refAddrL1[i], &refImage,
                            col, row, x+4, y, 4, 8);
                    }
                    else
                    {
                        u8 *addr = mode==B_PRED_L0 ? pMb->refAddr[i] :
                                                     pMb->refAddrL1[i];
                        PredictPart(data, pMb->mv+4*i, addr, &refImage,
                            col, row, x, y, 4, 8);
                        PredictPart(data, pMb->mv+4*i+1, addr, &refImage,
                            col, row, x+4, y, 4, 8);
                    }
                    break;
                default: /* 4x4 */
                    if (mode == B_PRED_BI)
                    {
                        BiPredictPart(data, pMb->mv+4*i, pMb->refAddr[i],
                            pMb->mvL1+4*i, pMb->refAddrL1[i], &refImage,
                            col, row, x, y, 4, 4);
                        BiPredictPart(data, pMb->mv+4*i+1, pMb->refAddr[i],
                            pMb->mvL1+4*i+1, pMb->refAddrL1[i], &refImage,
                            col, row, x+4, y, 4, 4);
                        BiPredictPart(data, pMb->mv+4*i+2, pMb->refAddr[i],
                            pMb->mvL1+4*i+2, pMb->refAddrL1[i], &refImage,
                            col, row, x, y+4, 4, 4);
                        BiPredictPart(data, pMb->mv+4*i+3, pMb->refAddr[i],
                            pMb->mvL1+4*i+3, pMb->refAddrL1[i], &refImage,
                            col, row, x+4, y+4, 4, 4);
                    }
                    else
                    {
                        u8 *addr = mode==B_PRED_L0 ? pMb->refAddr[i] :
                                                     pMb->refAddrL1[i];
                        PredictPart(data, pMb->mv+4*i, addr, &refImage,
                            col, row, x, y, 4, 4);
                        PredictPart(data, pMb->mv+4*i+1, addr, &refImage,
                            col, row, x+4, y, 4, 4);
                        PredictPart(data, pMb->mv+4*i+2, addr, &refImage,
                            col, row, x, y+4, 4, 4);
                        PredictPart(data, pMb->mv+4*i+3, addr, &refImage,
                            col, row, x+4, y+4, 4, 4);
                    }
                    break;
            }
        }
    }
    else
    {
        partMode = h264bsdBMbPartMode(mbType);

        if (partMode == MB_P_16x16)
            tmp = BMvPred16x16(pMb, &pMbLayer->mbPred, dpb);
        else if (partMode == MB_P_16x8)
            tmp = BMvPred16x8(pMb, &pMbLayer->mbPred, dpb);
        else
            tmp = BMvPred8x16(pMb, &pMbLayer->mbPred, dpb);

        if (tmp != HANTRO_OK)
            return(HANTRO_NOK);

        if (partMode == MB_P_16x16)
        {
            u32 mode = h264bsdBMbPartPredMode(mbType, 0);
            if (mode == B_PRED_BI)
                BiPredictPart(data, pMb->mv, pMb->refAddr[0],
                    pMb->mvL1, pMb->refAddrL1[0], &refImage,
                    col, row, 0, 0, 16, 16);
            else
                PredictPart(data, pMb->mv,
                    mode==B_PRED_L0 ? pMb->refAddr[0] : pMb->refAddrL1[0],
                    &refImage, col, row, 0, 0, 16, 16);
        }
        else if (partMode == MB_P_16x8)
        {
            u32 m0 = h264bsdBMbPartPredMode(mbType, 0);
            u32 m1 = h264bsdBMbPartPredMode(mbType, 1);
            if (m0 == B_PRED_BI)
                BiPredictPart(data, pMb->mv, pMb->refAddr[0],
                    pMb->mvL1, pMb->refAddrL1[0], &refImage,
                    col, row, 0, 0, 16, 8);
            else
                PredictPart(data, pMb->mv,
                    m0==B_PRED_L0 ? pMb->refAddr[0] : pMb->refAddrL1[0],
                    &refImage, col, row, 0, 0, 16, 8);
            if (m1 == B_PRED_BI)
                BiPredictPart(data, pMb->mv+8, pMb->refAddr[2],
                    pMb->mvL1+8, pMb->refAddrL1[2], &refImage,
                    col, row, 0, 8, 16, 8);
            else
                PredictPart(data, pMb->mv+8,
                    m1==B_PRED_L0 ? pMb->refAddr[2] : pMb->refAddrL1[2],
                    &refImage, col, row, 0, 8, 16, 8);
        }
        else /* 8x16 */
        {
            u32 m0 = h264bsdBMbPartPredMode(mbType, 0);
            u32 m1 = h264bsdBMbPartPredMode(mbType, 1);
            if (m0 == B_PRED_BI)
                BiPredictPart(data, pMb->mv, pMb->refAddr[0],
                    pMb->mvL1, pMb->refAddrL1[0], &refImage,
                    col, row, 0, 0, 8, 16);
            else
                PredictPart(data, pMb->mv,
                    m0==B_PRED_L0 ? pMb->refAddr[0] : pMb->refAddrL1[0],
                    &refImage, col, row, 0, 0, 8, 16);
            if (m1 == B_PRED_BI)
                BiPredictPart(data, pMb->mv+4, pMb->refAddr[1],
                    pMb->mvL1+4, pMb->refAddrL1[1], &refImage,
                    col, row, 8, 0, 8, 16);
            else
                PredictPart(data, pMb->mv+4,
                    m1==B_PRED_L0 ? pMb->refAddr[1] : pMb->refAddrL1[1],
                    &refImage, col, row, 8, 0, 8, 16);
        }
    }

    /* combine prediction and the decoded residual */
    h264bsdWriteOutputBlocks(currImage, mbNum, data,
        pMbLayer->residual.level);

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: ClipMv

------------------------------------------------------------------------------*/

u32 ClipMv(mv_t *mv)
{

    /* horizontal motion vector range [-2048, 2047.75] */
    if ((u32)(i32)(mv->hor+8192) >= (16384))
        return(HANTRO_NOK);
    /* vertical motion vector range [-512, 511.75] */
    if ((u32)(i32)(mv->ver+2048) >= (4096))
        return(HANTRO_NOK);

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: MedianFilter3

------------------------------------------------------------------------------*/

i32 MedianFilter3(i32 a, i32 b, i32 c)
{

    i32 max, min, med;

    max = min = med = a;
    if (b > max) max = b;
    else if (b < min) min = b;
    if (c > max) med = max;
    else if (c < min) med = min;
    else med = c;

    return(med);

}

/*------------------------------------------------------------------------------

    Function: GetBInterNeighbour

        Functional description:
            Fetch a neighbour 4x4 block's availability, reference index and
            motion vector for the given list. The neighbour only counts when
            it uses the requested list.

------------------------------------------------------------------------------*/

void GetBInterNeighbour(u32 sliceId, mbStorage_t *nMb,
    bInterNeighbour_t *n, u32 index, u32 list)
{

    n->available = HANTRO_FALSE;
    n->listMask = 0;
    n->refIndex = 0xFFFFFFFF;
    n->refAddr = NULL;
    n->mv.hor = n->mv.ver = 0;

    if (nMb && (sliceId == nMb->sliceId) && !nMb->intraFlag &&
        nMb->bSliceFlag)
    {
        u8 mask = nMb->listUsage[index];
        u32 uses = (list == 0) ? (mask & USE_L0) : (mask & USE_L1);
        if (uses)
        {
            n->available = HANTRO_TRUE;
            n->listMask = mask;
            if (list == 0)
            {
                n->refIndex = nMb->refPic[index >> 2];
                n->refAddr = nMb->refAddr[index >> 2];
                n->mv = nMb->mv[index];
            }
            else
            {
                n->refIndex = nMb->refPicL1[index >> 2];
                n->refAddr = nMb->refAddrL1[index >> 2];
                n->mv = nMb->mvL1[index];
            }
        }
    }

}

/*------------------------------------------------------------------------------

    Function: ScaledMv

        Functional description:
            Temporal distance based motion vector scaling (H.264 8.4.1.3.1).
            In the restricted short term frame case the temporal distance is
            derived from picture order counts.

------------------------------------------------------------------------------*/

u32 ScaledMv(dpbStorage_t *dpb, mv_t *out, mv_t in,
    u8 *refAddrCur, u8 *refAddrNeigh, u32 list)
{

    i32 td, tb, abstb;
    i32 pocCur, pocRefCur = 0, pocRefNeigh = 0;
    u32 foundCur = 0, foundNeigh = 0;

/* Code */

    (void)list;
    pocCur = dpb->currPicOrderCnt;

    /* same reference picture -> no scaling */
    if (refAddrCur == refAddrNeigh)
    {
        *out = in;
        return(HANTRO_OK);
    }

    {
        u32 fc = 0, fn = 0;
        pocRefCur = h264bsdRefPicOrderCnt(dpb, refAddrCur, &fc);
        pocRefNeigh = h264bsdRefPicOrderCnt(dpb, refAddrNeigh, &fn);
        foundCur = fc;
        foundNeigh = fn;
    }

    if (!foundCur || !foundNeigh)
    {
        *out = in;
        return(HANTRO_OK);
    }

    td = pocCur - pocRefCur;
    tb = pocCur - pocRefNeigh;
    td = CLIP3(-128, 127, td);
    tb = CLIP3(-128, 127, tb);

    if (td == tb)
    {
        *out = in;
        return(HANTRO_OK);
    }

    abstb = ABS(tb);
    if (abstb == 0)
    {
        *out = in;
        return(HANTRO_OK);
    }

    /* software form of H.264 8.4.1.3.1: scale by the signed ratio td/tb,
     * with rounding to the nearest quarter-pel unit */
    {
        i32 sx = (i32)in.hor;
        i32 sy = (i32)in.ver;
        i32 flip = ((td > 0) != (tb > 0)) ? -1 : 1;

        i32 nx = ABS(sx) * ABS(td);
        i32 ny = ABS(sy) * ABS(td);
        i32 rx = (nx + (abstb >> 1)) / abstb;
        i32 ry = (ny + (abstb >> 1)) / abstb;

        if (sx < 0) rx = -rx;
        if (sy < 0) ry = -ry;
        if (flip < 0)
        {
            rx = -rx;
            ry = -ry;
        }

        out->hor = (i16)CLIP3(-32768, 32767, rx);
        out->ver = (i16)CLIP3(-32768, 32767, ry);
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------
    Neighbour 4x4 block mapping for sub-macroblock motion prediction,
    copied from the H.264 block neighbour tables.
------------------------------------------------------------------------------*/

static const neighbour_t B_N_A_SUB[4][4][4] = {
    { { {MB_A,5}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_A,5}, {MB_A,7}, {MB_NA,0}, {MB_NA,0} },
      { {MB_A,5}, {MB_CURR,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_A,5}, {MB_CURR,0}, {MB_A,7}, {MB_CURR,2} } },
    { { {MB_CURR,1}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,1}, {MB_CURR,3}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,1}, {MB_CURR,4}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,1}, {MB_CURR,4}, {MB_CURR,3}, {MB_CURR,6} } },
    { { {MB_A,13}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_A,13}, {MB_A,15}, {MB_NA,0}, {MB_NA,0} },
      { {MB_A,13}, {MB_CURR,8}, {MB_NA,0}, {MB_NA,0} },
      { {MB_A,13}, {MB_CURR,8}, {MB_A,15}, {MB_CURR,10} } },
    { { {MB_CURR,9}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,9}, {MB_CURR,11}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,9}, {MB_CURR,12}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,9}, {MB_CURR,12}, {MB_CURR,11}, {MB_CURR,14} } } };

static const neighbour_t B_N_B_SUB[4][4][4] = {
    { { {MB_B,10}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,10}, {MB_CURR,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,10}, {MB_B,11}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,10}, {MB_B,11}, {MB_CURR,0}, {MB_CURR,1} } },
    { { {MB_B,14}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,14}, {MB_CURR,4}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,14}, {MB_B,15}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,14}, {MB_B,15}, {MB_CURR,4}, {MB_CURR,5} } },
    { { {MB_CURR,2}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,2}, {MB_CURR,8}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,2}, {MB_CURR,3}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,2}, {MB_CURR,3}, {MB_CURR,8}, {MB_CURR,9} } },
    { { {MB_CURR,6}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,6}, {MB_CURR,12}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,6}, {MB_CURR,7}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,6}, {MB_CURR,7}, {MB_CURR,12}, {MB_CURR,13} } } };

static const neighbour_t B_N_C_SUB[4][4][4] = {
    { { {MB_B,14}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,14}, {MB_NA,4}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,11}, {MB_B,14}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,11}, {MB_B,14}, {MB_CURR,1}, {MB_NA,4} } },
    { { {MB_C,10}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_C,10}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,15}, {MB_C,10}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,15}, {MB_C,10}, {MB_CURR,5}, {MB_NA,0} } },
    { { {MB_CURR,6}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,6}, {MB_NA,12}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,3}, {MB_CURR,6}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,3}, {MB_CURR,6}, {MB_CURR,9}, {MB_NA,12} } },
    { { {MB_NA,2}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_NA,2}, {MB_NA,8}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,7}, {MB_NA,2}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,7}, {MB_NA,2}, {MB_CURR,13}, {MB_NA,8} } } };

static const neighbour_t B_N_D_SUB[4][4][4] = {
    { { {MB_D,15}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_D,15}, {MB_A,5}, {MB_NA,0}, {MB_NA,0} },
      { {MB_D,15}, {MB_B,10}, {MB_NA,0}, {MB_NA,0} },
      { {MB_D,15}, {MB_B,10}, {MB_A,5}, {MB_CURR,0} } },
    { { {MB_B,11}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,11}, {MB_CURR,1}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,11}, {MB_B,14}, {MB_NA,0}, {MB_NA,0} },
      { {MB_B,11}, {MB_B,14}, {MB_CURR,1}, {MB_CURR,4} } },
    { { {MB_A,7}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_A,7}, {MB_A,13}, {MB_NA,0}, {MB_NA,0} },
      { {MB_A,7}, {MB_CURR,2}, {MB_NA,0}, {MB_NA,0} },
      { {MB_A,7}, {MB_CURR,2}, {MB_A,13}, {MB_CURR,8} } },
    { { {MB_CURR,3}, {MB_NA,0}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,3}, {MB_CURR,9}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,3}, {MB_CURR,6}, {MB_NA,0}, {MB_NA,0} },
      { {MB_CURR,3}, {MB_CURR,6}, {MB_CURR,9}, {MB_CURR,12} } } };

/*------------------------------------------------------------------------------

    Function: BuildCandidate

        Functional description:
            Resolve one neighbour into a scaled predictor MV for the given
            list and current partition reference picture.

------------------------------------------------------------------------------*/

static u32 BuildCandidate(dpbStorage_t *dpb, bInterNeighbour_t *n, u32 list,
    u8 *curRefAddr, mv_t *out)
{

    if (!n->available)
        return(HANTRO_FALSE);

    if (n->refAddr == curRefAddr)
    {
        *out = n->mv;
    }
    else
    {
        if (ScaledMv(dpb, out, n->mv, curRefAddr, n->refAddr, list) !=
            HANTRO_OK)
            return(HANTRO_FALSE);
    }

    return(HANTRO_TRUE);

}

/*------------------------------------------------------------------------------

    Function: PredictFromNeighbours

        Functional description:
            Compute the motion vector predictor from A, B and C (D fallback)
            candidates for one list.

------------------------------------------------------------------------------*/

static void PredictFromNeighbours(dpbStorage_t *dpb,
    u32 list, u8 *curRefAddr, u32 curRefIdx,
    bInterNeighbour_t *a, bInterNeighbour_t *b, bInterNeighbour_t *c,
    mv_t *mvPred)
{

    mv_t cand[3];
    u32 valid[3];
    u32 count;

    (void)curRefIdx;

    valid[0] = BuildCandidate(dpb, a, list, curRefAddr, &cand[0]);
    valid[1] = BuildCandidate(dpb, b, list, curRefAddr, &cand[1]);
    valid[2] = BuildCandidate(dpb, c, list, curRefAddr, &cand[2]);

    count = valid[0] + valid[1] + valid[2];

    if (count == 0)
    {
        mvPred->hor = mvPred->ver = 0;
    }
    else if (count == 1)
    {
        if (valid[0]) *mvPred = cand[0];
        else if (valid[1]) *mvPred = cand[1];
        else *mvPred = cand[2];
    }
    else
    {
        mv_t v[3];
        u32 k = 0, i;
        for (i = 0; i < 3; i++)
            if (valid[i])
                v[k++] = cand[i];
        if (count == 2)
        {
            mvPred->hor = (i16)MedianFilter3(v[0].hor, v[1].hor, v[1].hor);
            mvPred->ver = (i16)MedianFilter3(v[0].ver, v[1].ver, v[1].ver);
        }
        else
        {
            mvPred->hor = (i16)MedianFilter3(v[0].hor, v[1].hor, v[2].hor);
            mvPred->ver = (i16)MedianFilter3(v[0].ver, v[1].ver, v[2].ver);
        }
    }

}

/*------------------------------------------------------------------------------

    Function: SetBlockStorage

        Functional description:
            Store the resolved reference and motion vector of one list across
            the 4x4 blocks covered by a partition, and update listUsage.

------------------------------------------------------------------------------*/

static void SetBlockStorage(mbStorage_t *pMb, u32 list, u32 partSlot,
    u32 refIdx, u8 *refAddr, mv_t mv, const u32 *blocks, u32 numBlocks)
{

    u32 k;

    for (k = 0; k < numBlocks; k++)
    {
        u32 blk = blocks[k];
        if (list == 0)
        {
            pMb->refPic[partSlot] = refIdx;
            pMb->refAddr[partSlot] = refAddr;
            pMb->mv[blk] = mv;
            pMb->listUsage[blk] |= USE_L0;
        }
        else
        {
            pMb->refPicL1[partSlot] = refIdx;
            pMb->refAddrL1[partSlot] = refAddr;
            pMb->mvL1[blk] = mv;
            pMb->listUsage[blk] |= USE_L1;
        }
    }

}

/* block indices for the four 8x8 quadrants and rectangular partitions */
static const u32 BLOCKS_ALL[16] =
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
static const u32 BLOCKS_TOP[8] = {0,1,2,3,4,5,6,7};
static const u32 BLOCKS_BOTTOM[8] = {8,9,10,11,12,13,14,15};
static const u32 BLOCKS_LEFT[8] = {0,1,4,5,8,9,12,13};
static const u32 BLOCKS_RIGHT[8] = {2,3,6,7,10,11,14,15};

/*------------------------------------------------------------------------------

    Function: DerivePartMv

        Functional description:
            Derive the L0/L1 motion vectors and references for one macroblock
            partition described by its candidate neighbour block indices.

------------------------------------------------------------------------------*/

static u32 DerivePartMv(mbStorage_t *pMb, dpbStorage_t *dpb, u32 mode,
    u32 part, mbPred_t *mbPred, u32 idxA, u32 idxB, u32 idxC, u32 idxD,
    const u32 *blocks, u32 numBlocks, u32 partSlot)
{

    bInterNeighbour_t a, b, c;
    mv_t mvPred, mv;
    u32 list;

    for (list = 0; list < 2; list++)
    {
        u32 refIdx;
        u8 *refAddr;

        if (list == 0)
        {
            if (!(mode == B_PRED_L0 || mode == B_PRED_BI))
                continue;
            refIdx = mbPred->refIdxL0[part];
            refAddr = h264bsdGetRefPicData(dpb, refIdx);
        }
        else
        {
            if (!(mode == B_PRED_L1 || mode == B_PRED_BI))
                continue;
            refIdx = mbPred->refIdxL1[part];
            refAddr = h264bsdGetRefPicDataL1(dpb, refIdx);
        }

        if (refAddr == NULL)
            return(HANTRO_NOK);

        GetBInterNeighbour(pMb->sliceId, pMb->mbA, &a, idxA, list);
        GetBInterNeighbour(pMb->sliceId, pMb->mbB, &b, idxB, list);
        GetBInterNeighbour(pMb->sliceId, pMb->mbC, &c, idxC, list);
        if (!c.available)
            GetBInterNeighbour(pMb->sliceId, pMb->mbD, &c, idxD, list);

        PredictFromNeighbours(dpb, list, refAddr, refIdx,
            &a, &b, &c, &mvPred);

        if (list == 0)
        {
            mv.hor = mbPred->mvdL0[part].hor + mvPred.hor;
            mv.ver = mbPred->mvdL0[part].ver + mvPred.ver;
        }
        else
        {
            mv.hor = mbPred->mvdL1[part].hor + mvPred.hor;
            mv.ver = mbPred->mvdL1[part].ver + mvPred.ver;
        }

        if (ClipMv(&mv) != HANTRO_OK)
            return(HANTRO_NOK);

        SetBlockStorage(pMb, list, partSlot, refIdx, refAddr, mv,
            blocks, numBlocks);
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: BMvPred16x16 / 16x8 / 8x16

------------------------------------------------------------------------------*/

u32 BMvPred16x16(mbStorage_t *pMb, mbPred_t *mbPred, dpbStorage_t *dpb)
{

    u32 mode = h264bsdBMbPartPredMode((u32)pMb->mbType, 0);

    return DerivePartMv(pMb, dpb, mode, 0, mbPred, 5, 10, 10, 15,
        BLOCKS_ALL, 16, 0);

}

u32 BMvPred16x8(mbStorage_t *pMb, mbPred_t *mbPred, dpbStorage_t *dpb)
{

    u32 mode0 = h264bsdBMbPartPredMode((u32)pMb->mbType, 0);
    u32 mode1 = h264bsdBMbPartPredMode((u32)pMb->mbType, 1);
    u32 tmp;

    tmp = DerivePartMv(pMb, dpb, mode0, 0, mbPred, 5, 10, 10, 15,
        BLOCKS_TOP, 8, 0);
    if (tmp != HANTRO_OK)
        return(tmp);

    tmp = DerivePartMv(pMb, dpb, mode1, 1, mbPred, 13, 14, 10, 15,
        BLOCKS_BOTTOM, 8, 2);

    return(tmp);

}

u32 BMvPred8x16(mbStorage_t *pMb, mbPred_t *mbPred, dpbStorage_t *dpb)
{

    u32 mode0 = h264bsdBMbPartPredMode((u32)pMb->mbType, 0);
    u32 mode1 = h264bsdBMbPartPredMode((u32)pMb->mbType, 1);
    u32 tmp;

    tmp = DerivePartMv(pMb, dpb, mode0, 0, mbPred, 5, 10, 14, 15,
        BLOCKS_LEFT, 8, 0);
    if (tmp != HANTRO_OK)
        return(tmp);

    tmp = DerivePartMv(pMb, dpb, mode1, 1, mbPred, 7, 14, 14, 15,
        BLOCKS_RIGHT, 8, 1);

    return(tmp);

}

/*------------------------------------------------------------------------------

    Function: BMvPred8x8 / BMvPrediction

------------------------------------------------------------------------------*/

u32 BMvPred8x8(mbStorage_t *pMb, subMbPred_t *subMbPred, dpbStorage_t *dpb)
{

    u32 i, j;

    for (i = 0; i < 4; i++)
    {
        u32 numSub = h264bsdBNumSubMbPart(subMbPred->subMbType[i]);
        u32 mode = h264bsdBSubMbPartPredMode(subMbPred->subMbType[i]);
        u32 subMode = h264bsdBSubMbPartMode(subMbPred->subMbType[i]);
        u32 refIdx0 = subMbPred->refIdxL0[i];
        u32 refIdx1 = subMbPred->refIdxL1[i];
        u8 *addr0 = h264bsdGetRefPicData(dpb, refIdx0);
        u8 *addr1 = h264bsdGetRefPicDataL1(dpb, refIdx1);

        if ((mode == B_PRED_L0 || mode == B_PRED_BI) && addr0 == NULL)
            return(HANTRO_NOK);
        if ((mode == B_PRED_L1 || mode == B_PRED_BI) && addr1 == NULL)
            return(HANTRO_NOK);

        pMb->refPic[i] = refIdx0;
        pMb->refAddr[i] = addr0;
        pMb->refPicL1[i] = refIdx1;
        pMb->refAddrL1[i] = addr1;

        for (j = 0; j < numSub; j++)
        {
            if (BMvPrediction(pMb, subMbPred, dpb, i, j, 0) != HANTRO_OK)
                return(HANTRO_NOK);
            if (BMvPrediction(pMb, subMbPred, dpb, i, j, 1) != HANTRO_OK)
                return(HANTRO_NOK);
        }

        (void)subMode;
    }

    return(HANTRO_OK);

}

u32 BMvPrediction(mbStorage_t *pMb, subMbPred_t *subMbPred,
    dpbStorage_t *dpb, u32 mbPartIdx, u32 subMbPartIdx, u32 list)
{

    bInterNeighbour_t a, b, c;
    const neighbour_t *n;
    mbStorage_t *nMb;
    subMbPartMode_e subMode;
    mv_t mvPred, mv;
    u32 mode, refIdx, blk, numBlk, k;
    u8 *refAddr;

    subMode = h264bsdBSubMbPartMode(subMbPred->subMbType[mbPartIdx]);
    mode = h264bsdBSubMbPartPredMode(subMbPred->subMbType[mbPartIdx]);

    if (list == 0 && !(mode == B_PRED_L0 || mode == B_PRED_BI))
        return(HANTRO_OK);
    if (list == 1 && !(mode == B_PRED_L1 || mode == B_PRED_BI))
        return(HANTRO_OK);

    n = B_N_A_SUB[mbPartIdx][subMode] + subMbPartIdx;
    nMb = h264bsdGetNeighbourMb(pMb, n->mb);
    GetBInterNeighbour(pMb->sliceId, nMb, &a, n->index, list);

    n = B_N_B_SUB[mbPartIdx][subMode] + subMbPartIdx;
    nMb = h264bsdGetNeighbourMb(pMb, n->mb);
    GetBInterNeighbour(pMb->sliceId, nMb, &b, n->index, list);

    n = B_N_C_SUB[mbPartIdx][subMode] + subMbPartIdx;
    nMb = h264bsdGetNeighbourMb(pMb, n->mb);
    GetBInterNeighbour(pMb->sliceId, nMb, &c, n->index, list);

    if (!c.available)
    {
        n = B_N_D_SUB[mbPartIdx][subMode] + subMbPartIdx;
        nMb = h264bsdGetNeighbourMb(pMb, n->mb);
        GetBInterNeighbour(pMb->sliceId, nMb, &c, n->index, list);
    }

    if (list == 0)
    {
        refIdx = subMbPred->refIdxL0[mbPartIdx];
        refAddr = pMb->refAddr[mbPartIdx];
    }
    else
    {
        refIdx = subMbPred->refIdxL1[mbPartIdx];
        refAddr = pMb->refAddrL1[mbPartIdx];
    }

    PredictFromNeighbours(dpb, list, refAddr, refIdx,
        &a, &b, &c, &mvPred);

    if (list == 0)
    {
        mv.hor = subMbPred->mvdL0[mbPartIdx][subMbPartIdx].hor + mvPred.hor;
        mv.ver = subMbPred->mvdL0[mbPartIdx][subMbPartIdx].ver + mvPred.ver;
    }
    else
    {
        mv.hor = subMbPred->mvdL1[mbPartIdx][subMbPartIdx].hor + mvPred.hor;
        mv.ver = subMbPred->mvdL1[mbPartIdx][subMbPartIdx].ver + mvPred.ver;
    }

    if (ClipMv(&mv) != HANTRO_OK)
        return(HANTRO_NOK);

    /* store across the 4x4 blocks covered by this sub-partition */
    switch (subMode)
    {
        case MB_SP_8x8:
            blk = 4*mbPartIdx;
            numBlk = 4;
            break;
        case MB_SP_8x4:
            /* vertically stacked 8x4 parts cover two rows of 4x4 blocks,
             * two luma indices apart */
            blk = 4*mbPartIdx + 2*subMbPartIdx;
            numBlk = 2;
            break;
        case MB_SP_4x8:
            /* side-by-side 4x8 parts use consecutive luma block indices */
            blk = 4*mbPartIdx + subMbPartIdx;
            numBlk = 2;
            break;
        default:
            blk = 4*mbPartIdx + subMbPartIdx;
            numBlk = 1;
            break;
    }

    for (k = 0; k < numBlk; k++)
    {
        if (subMode == MB_SP_4x8)
        {
            if (list == 0) { pMb->mv[blk+2*k] = mv; pMb->listUsage[blk+2*k] |= USE_L0; }
            else { pMb->mvL1[blk+2*k] = mv; pMb->listUsage[blk+2*k] |= USE_L1; }
        }
        else
        {
            if (list == 0) { pMb->mv[blk+k] = mv; pMb->listUsage[blk+k] |= USE_L0; }
            else { pMb->mvL1[blk+k] = mv; pMb->listUsage[blk+k] |= USE_L1; }
        }
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: PredictPart / BiPredictPart / AvgBlocks

        Functional description:
            Render one partition from a single list, or the average of the
            L0 and L1 predictions (unweighted bi-prediction, H.264
            8.4.2.3.2). Each list prediction is interpolated into a private
            scratch macroblock using the existing quarter/eighth-pel
            machinery and then averaged sample by sample.

------------------------------------------------------------------------------*/

void PredictPart(u8 *data, mv_t *mv, u8 *refAddr, image_t *refImage,
    u32 col, u32 row, u32 partX, u32 partY, u32 w, u32 h)
{

    image_t img;

    img.width = refImage->width;
    img.height = refImage->height;
    img.data = refAddr;

    h264bsdPredictSamples(data, mv, &img, col, row, partX, partY, w, h);

}

void AvgBlocks(u8 *dst, const u8 *p0, const u8 *p1, u32 n)
{

    u32 i;

    /* luma (256) and both chroma planes (64 each) */
    for (i = 0; i < n; i++)
        dst[i] = (u8)(((u32)p0[i] + (u32)p1[i] + 1) >> 1);

}

void BiPredictPart(u8 *data, mv_t *mv0, u8 *addr0, mv_t *mv1,
    u8 *addr1, image_t *refImage, u32 col, u32 row,
    u32 partX, u32 partY, u32 w, u32 h)
{

    /* stack-local scratch so concurrent macroblocks never overwrite each
     * other's prediction; a 16-byte aligned base is obtained by over-
     * allocating and adjusting the pointers */
    u8 raw0[384 + 15];
    u8 raw1[384 + 15];
    u8 *buf0 = (u8*)ALIGN(raw0, 16);
    u8 *buf1 = (u8*)ALIGN(raw1, 16);
    image_t img0, img1;

    img0.width = img1.width = refImage->width;
    img0.height = img1.height = refImage->height;
    img0.data = addr0;
    img1.data = addr1;

    memset(buf0, 0, 384);
    memset(buf1, 0, 384);

    h264bsdPredictSamples(buf0, mv0, &img0, col, row, partX, partY, w, h);
    h264bsdPredictSamples(buf1, mv1, &img1, col, row, partX, partY, w, h);

    /* luma region of the partition */
    {
        u32 y, x;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                data[16*(partY + y) + partX + x] =
                    (u8)(((u32)buf0[16*(partY + y) + partX + x] +
                          (u32)buf1[16*(partY + y) + partX + x] + 1) >> 1);
    }

    /* chroma: each plane is 8x8 laid out at offset 256 and 320 */
    {
        u32 cw = w >> 1;
        u32 ch = h >> 1;
        u32 cx = partX >> 1;
        u32 cy = partY >> 1;
        u32 plane, y, x;
        for (plane = 0; plane < 2; plane++)
        {
            u32 base = 256 + plane*64;
            for (y = 0; y < ch; y++)
                for (x = 0; x < cw; x++)
                    data[base + 8*(cy + y) + cx + x] =
                        (u8)(((u32)buf0[base + 8*(cy + y) + cx + x] +
                              (u32)buf1[base + 8*(cy + y) + cx + x] + 1) >> 1);
        }
    }

    (void)AvgBlocks;

}
