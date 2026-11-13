/*
 ** Copyright 2003-2010, VisualOn, Inc.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */
/*******************************************************************************
	File:		aacenc_core.h

	Content:	aac encoder interface functions

*******************************************************************************/

#ifndef _aacenc_core_h_
#define _aacenc_core_h_


#include "typedef.h"
#include "config.h"
#include "bitenc.h"

#include "psy_configuration.h"
#include "psy_main.h"
#include "qc_main.h"
#include "psy_main.h"
/*-------------------------- defines --------------------------------------*/


/*-------------------- structure definitions ------------------------------*/
typedef  struct {
  Word32   sampleRate;            /* audio file sample rate */
  Word32   bitRate;               /* encoder bit rate in bits/sec */
  Word16   nChannelsIn;           /* number of channels on input (1,2) */
  Word16   nChannelsOut;          /* number of channels on output (1,2) */
  Word16   bandWidth;             /* targeted audio bandwidth in Hz */
  Word16   adtsUsed;			  /* whether write adts header */
} AACENC_CONFIG;


typedef struct {

  AACENC_CONFIG config;     /* Word16 size: 8 */

  ELEMENT_INFO elInfo;      /* Word16 size: 4 */

  QC_STATE qcKernel;        /* Word16 size: 6 + 5(PADDING) + 7(ELEMENT_BITS) + 54(ADJ_THR_STATE) = 72 */
  QC_OUT   qcOut;           /* Word16 size: MAX_CHANNELS*920(QC_OUT_CHANNEL) + 5(QC_OUT_ELEMENT) + 7 = 932 / 1852 */

  PSY_OUT    psyOut;        /* Word16 size: MAX_CHANNELS*186 + 2 = 188 / 374 */
  PSY_KERNEL psyKernel;     /* Word16 size:  2587 / 4491 */

  struct BITSTREAMENCODER_INIT bseInit; /* Word16 size: 6 */
  struct BIT_BUF  bitStream;            /* Word16 size: 8 */
  HANDLE_BIT_BUF  hBitStream;
  int			  initOK;

  /*
   * Bit rate requested through VO_PID_AAC_LIVE_BITRATE, waiting to be
   * applied before the next fully encoded 1024-sample frame. 0 means no
   * request is pending. Never applied on partial input, so the accumulated
   * MDCT overlap, window/block-switch history and threshold history survive.
   */
  Word32          liveBitratePending;

  short			*intbuf;
  short			*encbuf;
  short			*inbuf;
  int			enclen;
  int			inlen;
  int			intlen;
  int			uselength;

  void			*hCheck;
  VO_MEM_OPERATOR *voMemop;
  VO_MEM_OPERATOR voMemoprator;

}AAC_ENCODER; /* Word16 size: 3809 / 6851 */

/*-----------------------------------------------------------------------------

functionname: AacInitDefaultConfig
description:  gives reasonable default configuration
returns:      ---

------------------------------------------------------------------------------*/
void AacInitDefaultConfig(AACENC_CONFIG *config);

/*---------------------------------------------------------------------------

functionname:AacEncOpen
description: allocate and initialize a new encoder instance
returns:     AACENC_OK if success

---------------------------------------------------------------------------*/

Word16  AacEncOpen (AAC_ENCODER				*hAacEnc,       /* pointer to an encoder handle, initialized on return */
                    const  AACENC_CONFIG     config);        /* pre-initialized config struct */

/*
 * Schedule a continuous bit-rate change. The request is validated against
 * the active sample rate/channel configuration and stored; it is applied in
 * the next AacEncEncode call without resetting any audio history. Returns 0
 * on success, non-zero for an out-of-range bit rate.
 */
Word16 AacEncSetLiveBitrate(AAC_ENCODER *hAacEnc,
                            Word32       newBitrate);

/*
 * Apply a pending live bit-rate request immediately before the next complete
 * frame is encoded. No-op when nothing is pending. Keeps the bit reservoir
 * fill and all psychoacoustic/threshold history.
 */
void AacEncApplyLiveBitrate(AAC_ENCODER *hAacEnc);

Word16 AacEncEncode(AAC_ENCODER		   *hAacEnc,
                    Word16             *timeSignal,
                    const UWord8       *ancBytes,      /*!< pointer to ancillary data bytes */
                    Word16             *numAncBytes,   /*!< number of ancillary Data Bytes, send as fill element  */
                    UWord8             *outBytes,      /*!< pointer to output buffer            */
                    VO_U32             *numOutBytes    /*!< number of bytes in output buffer */
                    );

/*---------------------------------------------------------------------------

functionname:AacEncClose
description: deallocate an encoder instance

---------------------------------------------------------------------------*/

void AacEncClose (AAC_ENCODER* hAacEnc, VO_MEM_OPERATOR *pMemOP); /* an encoder handle */

#endif /* _aacenc_h_ */
