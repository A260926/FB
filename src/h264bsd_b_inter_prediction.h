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

/* B slice explicit inter prediction: macroblock and sub-macroblock
 * partitions using reference lists L0 and L1, including unweighted
 * bi-prediction. Motion vector derivation follows H.264 8.4.1.3 with
 * reference index and temporal distance scaling. */

#ifndef H264SWDEC_B_INTER_PREDICTION_H
#define H264SWDEC_B_INTER_PREDICTION_H

#include "h264bsd_macroblock_layer.h"
#include "h264bsd_dpb.h"
#include "h264bsd_image.h"

u32 h264bsdBInterPrediction(mbStorage_t *pMb, macroblockLayer_t *pMbLayer,
    dpbStorage_t *dpb, u32 mbNum, image_t *currImage, u8 *data);

#endif /* H264SWDEC_B_INTER_PREDICTION_H */
