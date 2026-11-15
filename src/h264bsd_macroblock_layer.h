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

    Table of contents

    1. Include headers
    2. Module defines
    3. Data types
    4. Function prototypes

------------------------------------------------------------------------------*/

#ifndef H264SWDEC_MACROBLOCK_LAYER_H
#define H264SWDEC_MACROBLOCK_LAYER_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_stream.h"
#include "h264bsd_image.h"
#include "h264bsd_dpb.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

/* Macro to determine if a mb is an intra mb. The numeric mb_type ranges of
 * P and B slices overlap, so an explicit flag stored in mbStorage is used.
 * For a macroblockLayer the helper h264bsdMbPartPredMode() is used. */
#define IS_INTRA_MB(a) ((a).intraFlag != 0)

/* Macro to determine if a mb is an I_PCM mb */
#define IS_I_PCM_MB(a) ((a).mbType == I_PCM)

/* B slice macroblock prediction attributes are derived from mb_type 0..22
 * (table 7-11). The B_LAST_INTER_MB value separates inter from intra. */
enum {
    B_LAST_INTER_MB = 22,   /* B_8x8 */
    B_I_NxN         = 23,
    B_I_PCM         = 31
};

/* B slice macroblock types, mb_type values 0..22 (table 7-11) */
typedef enum {
    B_Direct_16x16  = 0,
    B_L0_16x16      = 1,
    B_L1_16x16      = 2,
    B_Bi_16x16      = 3,
    B_L0_L0_16x8    = 4,
    B_L0_L0_8x16    = 5,
    B_L1_L1_16x8    = 6,
    B_L1_L1_8x16    = 7,
    B_L0_L1_16x8    = 8,
    B_L0_L1_8x16    = 9,
    B_L1_L0_16x8    = 10,
    B_L1_L0_8x16    = 11,
    B_L0_Bi_16x8    = 12,
    B_L0_Bi_8x16    = 13,
    B_L1_Bi_16x8    = 14,
    B_L1_Bi_8x16    = 15,
    B_Bi_L0_16x8    = 16,
    B_Bi_L0_8x16    = 17,
    B_Bi_L1_16x8    = 18,
    B_Bi_L1_8x16    = 19,
    B_Bi_Bi_16x8    = 20,
    B_Bi_Bi_8x16    = 21,
    B_8x8           = 22
} bMbType_e;

typedef enum {
    P_Skip          = 0,
    P_L0_16x16      = 1,
    P_L0_L0_16x8    = 2,
    P_L0_L0_8x16    = 3,
    P_8x8           = 4,
    P_8x8ref0       = 5,
    I_4x4           = 6,
    I_16x16_0_0_0   = 7,
    I_16x16_1_0_0   = 8,
    I_16x16_2_0_0   = 9,
    I_16x16_3_0_0   = 10,
    I_16x16_0_1_0   = 11,
    I_16x16_1_1_0   = 12,
    I_16x16_2_1_0   = 13,
    I_16x16_3_1_0   = 14,
    I_16x16_0_2_0   = 15,
    I_16x16_1_2_0   = 16,
    I_16x16_2_2_0   = 17,
    I_16x16_3_2_0   = 18,
    I_16x16_0_0_1   = 19,
    I_16x16_1_0_1   = 20,
    I_16x16_2_0_1   = 21,
    I_16x16_3_0_1   = 22,
    I_PCM           = 31
} mbType_e;

typedef enum {
    P_L0_8x8 = 0,
    P_L0_8x4 = 1,
    P_L0_4x8 = 2,
    P_L0_4x4 = 3
} subMbType_e;

/* B slice sub-macroblock types, sub_mb_type values 0..12 (table 7-13) */
typedef enum {
    B_B_Direct_8x8 = 0,
    B_B_L0_8x8     = 1,
    B_B_L1_8x8     = 2,
    B_B_Bi_8x8     = 3,
    B_B_L0_8x4     = 4,
    B_B_L0_4x8     = 5,
    B_B_L1_8x4     = 6,
    B_B_L1_4x8     = 7,
    B_B_L0_4x4     = 8,
    B_B_L1_4x4     = 9,
    B_B_Bi_8x4     = 10,
    B_B_Bi_4x8     = 11,
    B_B_Bi_4x4     = 12
} bSubMbType_e;

typedef enum {
    MB_P_16x16 = 0,
    MB_P_16x8,
    MB_P_8x16,
    MB_P_8x8
} mbPartMode_e;

typedef enum {
    MB_SP_8x8 = 0,
    MB_SP_8x4,
    MB_SP_4x8,
    MB_SP_4x4
} subMbPartMode_e;

typedef enum {
    PRED_MODE_INTRA4x4 = 0,
    PRED_MODE_INTRA16x16  ,
    PRED_MODE_INTER
} mbPartPredMode_e;

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/

typedef struct
{
    /* MvPrediction16x16 assumes that MVs are 16bits */
    i16 hor;
    i16 ver;
} mv_t;

typedef struct
{
    u32 prevIntra4x4PredModeFlag[16];
    u32 remIntra4x4PredMode[16];
    u32 intraChromaPredMode;
    u32 refIdxL0[4];
    u32 refIdxL1[4];
    mv_t mvdL0[4];
    mv_t mvdL1[4];
} mbPred_t;

typedef struct
{
    /* subMbType holds P subMbType_e (0..3) or B bSubMbType_e (0..12) */
    u32 subMbType[4];
    u32 refIdxL0[4];
    u32 refIdxL1[4];
    mv_t mvdL0[4][4];
    mv_t mvdL1[4][4];
} subMbPred_t;

typedef struct
{
#ifdef H264DEC_OMXDL
    u8 posCoefBuf[27*16*3];
    u8 totalCoeff[27];
#else
    i16 totalCoeff[27];
#endif
    i32 level[26][16];
    u32 coeffMap[24];
} residual_t;

typedef struct
{
    mbType_e mbType;
    u32 codedBlockPattern;
    i32 mbQpDelta;
    /* set for macroblocks decoded from a B slice; selects B-specific
     * mb_type/sub_mb_type interpretation and L1 handling */
    u32 bSliceFlag;
    mbPred_t mbPred;
    subMbPred_t subMbPred;
    residual_t residual;
} macroblockLayer_t;

typedef struct mbStorage
{
    mbType_e mbType;
    u32 sliceId;
    u32 disableDeblockingFilterIdc;
    i32 filterOffsetA;
    i32 filterOffsetB;
    u32 qpY;
    i32 chromaQpIndexOffset;
    /* intraFlag: macroblock uses intra prediction (I_NxN/I_16x16/I_PCM) */
    u32 intraFlag;
    /* bSliceFlag: macroblock belongs to a B slice */
    u32 bSliceFlag;
#ifdef H264DEC_OMXDL
    u8 totalCoeff[27];
#else
    i16 totalCoeff[27];
#endif
    u8 intra4x4PredMode[16];
    /* reference indices, picture data pointers and motion vectors for both
     * reference lists. refPic/refAddr/mv keep the L0 (P) meaning and remain
     * unchanged for the existing I/P path; the L1 counterparts are used by B
     * slices. A Bi partition occupies the same storage slot in both lists. */
    u32 refPic[4];
    u8* refAddr[4];
    mv_t mv[16];
    u32 refPicL1[4];
    u8* refAddrL1[4];
    mv_t mvL1[16];
    /* per 4x4 block (index 0..15) list usage for B slices: bit0 uses L0,
     * bit1 uses L1. A bi-predicted block has both bits set. */
    u8 listUsage[16];
    u32 decoded;
    struct mbStorage *mbA;
    struct mbStorage *mbB;
    struct mbStorage *mbC;
    struct mbStorage *mbD;
} mbStorage_t;

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

u32 h264bsdDecodeMacroblockLayer(strmData_t *pStrmData,
    macroblockLayer_t *pMbLayer, mbStorage_t *pMb, u32 sliceType,
    u32 numRefIdxL0Active, u32 numRefIdxL1Active);

u32 h264bsdNumMbPart(mbType_e mbType);
u32 h264bsdNumSubMbPart(subMbType_e subMbType);

subMbPartMode_e h264bsdSubMbPartMode(subMbType_e subMbType);

/* B slice partition geometry helpers, argument is the raw mb_type /
 * sub_mb_type value from the stream */
u32 h264bsdBNumMbPart(u32 mbType);
mbPartMode_e h264bsdBMbPartMode(u32 mbType);
u32 h264bsdBNumSubMbPart(u32 subMbType);
subMbPartMode_e h264bsdBSubMbPartMode(u32 subMbType);
/* prediction mode (Pred_L0, Pred_L1, Pred_Bi) of a B macroblock partition */
u32 h264bsdBMbPartPredMode(u32 mbType, u32 partition);
/* prediction mode of a B sub-macroblock partition */
u32 h264bsdBSubMbPartPredMode(u32 subMbType);

u32 h264bsdDecodeMacroblock(mbStorage_t *pMb, macroblockLayer_t *pMbLayer,
    image_t *currImage, dpbStorage_t *dpb, i32 *qpY, u32 mbNum,
    u32 constrainedIntraPredFlag, u8* data);

u32 h264bsdPredModeIntra16x16(mbType_e mbType);

mbPartPredMode_e h264bsdMbPartPredMode(mbType_e mbType);
mbPartPredMode_e h264bsdMbPartPredModeEx(u32 mbType, u32 bSliceFlag);

/* B slice prediction mode values returned by the helpers above */
enum {
    B_PRED_L0 = 0,
    B_PRED_L1 = 1,
    B_PRED_BI = 2
};
#ifdef H264DEC_NEON
u32 h264bsdClearMbLayer(macroblockLayer_t *pMbLayer, u32 size);
#endif

#endif /* #ifdef H264SWDEC_MACROBLOCK_LAYER_H */


