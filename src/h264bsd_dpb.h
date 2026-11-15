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

#ifndef H264SWDEC_DPB_H
#define H264SWDEC_DPB_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_slice_header.h"
#include "h264bsd_image.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/

/* enumeration to represent status of buffered image */
typedef enum {
    UNUSED = 0,
    NON_EXISTING,
    SHORT_TERM,
    LONG_TERM
} dpbPictureStatus_e;

/* structure to represent a buffered picture */
typedef struct {
    u8 *data;           /* 16-byte aligned pointer of pAllocatedData */
    u8 *pAllocatedData; /* allocated picture pointer; (size + 15) bytes */
    i32 picNum;
    u32 frameNum;
    i32 picOrderCnt;
    dpbPictureStatus_e status;
    u32 toBeDisplayed;
    u32 picId;
    u32 numErrMbs;
    u32 isIdr;
} dpbPicture_t;

/* structure to represent display image output from the buffer */
typedef struct {
    u8 *data;
    u32 picId;
    u32 numErrMbs;
    u32 isIdr;
} dpbOutPicture_t;

/* structure to represent DPB */
typedef struct {
    dpbPicture_t *buffer;
    dpbPicture_t **list;
    /* second reference picture list used by B slices; 'list' remains the
     * L0 list for the existing P slice path */
    dpbPicture_t **list1;
    dpbPicture_t *currentOut;
    dpbOutPicture_t *outBuf;
    u32 numOut;
    u32 outIndex;
    u32 maxRefFrames;
    u32 dpbSize;
    u32 maxFrameNum;
    u32 maxLongTermFrameIdx;
    u32 numRefFrames;
    u32 fullness;
    u32 prevRefFrameNum;
    u32 lastContainsMmco5;
    u32 noReordering;
    u32 flushed;
    /* picture order count of the picture currently being decoded, used for
     * motion vector temporal scaling in B slices */
    i32 currPicOrderCnt;
} dpbStorage_t;

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

u32 h264bsdInitDpb(
  dpbStorage_t *dpb,
  u32 picSizeInMbs,
  u32 dpbSize,
  u32 numRefFrames,
  u32 maxFrameNum,
  u32 noReordering);

u32 h264bsdResetDpb(
  dpbStorage_t *dpb,
  u32 picSizeInMbs,
  u32 dpbSize,
  u32 numRefFrames,
  u32 maxFrameNum,
  u32 noReordering);

void h264bsdInitRefPicList(dpbStorage_t *dpb);

/* build the initial L0 and L1 reference lists for a B slice from the short
 * term reference pictures, ordered by picture order count relative to the
 * current picture POC, including the L1 first-two-entries swap */
u32 h264bsdInitBRefPicLists(dpbStorage_t *dpb, i32 currPicOrderCnt);

/* apply the L0 and L1 reordering commands of a B slice */
u32 h264bsdReorderBRefPicLists(dpbStorage_t *dpb,
    refPicListReordering_t *order, u32 currFrameNum,
    u32 numRefIdxL0Active, u32 numRefIdxL1Active);

u8* h264bsdAllocateDpbImage(dpbStorage_t *dpb);

u8* h264bsdGetRefPicData(dpbStorage_t *dpb, u32 index);

/* fetch reference picture data from the L1 list */
u8* h264bsdGetRefPicDataL1(dpbStorage_t *dpb, u32 index);

/* return the picture order count of the reference picture whose image data
 * pointer matches, or 0 if the picture is not an existing reference */
i32 h264bsdRefPicOrderCnt(dpbStorage_t *dpb, u8 *data, u32 *found);

u32 h264bsdReorderRefPicList(
  dpbStorage_t *dpb,
  refPicListReordering_t *order,
  u32 currFrameNum,
  u32 numRefIdxActive);

u32 h264bsdMarkDecRefPic(
  dpbStorage_t *dpb,
  decRefPicMarking_t *mark,
  image_t *image,
  u32 frameNum,
  i32 picOrderCnt,
  u32 isIdr,
  u32 picId,
  u32 numErrMbs);

u32 h264bsdCheckGapsInFrameNum(dpbStorage_t *dpb, u32 frameNum, u32 isRefPic,
                               u32 gapsAllowed);

dpbOutPicture_t* h264bsdDpbOutputPicture(dpbStorage_t *dpb);

void h264bsdFlushDpb(dpbStorage_t *dpb);

void h264bsdFreeDpb(dpbStorage_t *dpb);

#endif /* #ifdef H264SWDEC_DPB_H */

