/*!
 * \copy
 *     Copyright (c) 2024
 *
 * \brief   Implementation of the ENCODER_OPTION_FRAME_BUDGET fixed byte cap.
 *
 *          See codec/encoder/core/inc/frame_budget.h for the behavioural
 *          contract. The rollback memento stored here only grows with the
 *          configured picture size and the number of allocated reference
 *          pictures; no second encoder and no frame history are kept.
 */

#include "frame_budget.h"

#include <new>
#include <vector>
#include <cstring>

#include "extern.h"
#include "rc.h"
#include "picture_handle.h"
#include "wels_common_defs.h"

namespace WelsEnc {

namespace {

/* Per-MB RC entry signature, used to chain back into the production hook. */
typedef void (*BudgetMbInitFn) (sWelsEncCtx* pCtx, SMB* pCurMb, SSlice* pSlice);

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static bool DimAligned16 (int32_t v) {
  return v >= 16 && (v & 15) == 0;
}

/* Exact size in bytes of the single SPicture::pBuffer allocation, mirroring
 * AllocPicture() in picture_handle.cpp. */
static size_t PictureBufferAllocSize (int32_t kiWidth, int32_t kiHeight) {
  int32_t iPicWidth       = WELS_ALIGN (kiWidth, MB_WIDTH_LUMA) + (PADDING_LENGTH << 1);
  int32_t iPicHeight      = WELS_ALIGN (kiHeight, MB_HEIGHT_LUMA) + (PADDING_LENGTH << 1);
  int32_t iPicChromaWidth = iPicWidth >> 1;
  int32_t iPicChromaHeight = iPicHeight >> 1;
  int32_t iLumaSize        = 0;
  int32_t iChromaSize      = 0;

  iPicWidth        = WELS_ALIGN (iPicWidth, 32);
  iPicChromaWidth  = WELS_ALIGN (iPicChromaWidth, 16);
  iLumaSize        = iPicWidth * iPicHeight;
  iChromaSize      = iPicChromaWidth * iPicChromaHeight;
  return static_cast<size_t> (iLumaSize) + 2u * static_cast<size_t> (iChromaSize);
}

static uint32_t PictureMbCount (int32_t w, int32_t h) {
  return static_cast<uint32_t> (((15 + w) >> 4) * ((15 + h) >> 4));
}

/* Typed byte helpers: silence -Wnontrivial-memcall on the parameter object and
 * make the byte-wise snapshot intent explicit. */
static void BudgetCopyIn (void* pDst, const void* pSrc, size_t n) {
  std::memcpy (pDst, pSrc, n);
}

/* ------------------------------------------------------------------ */
/* Rollback memento                                                     */
/* ------------------------------------------------------------------ */

struct SPicSave {
  SPicture* pObj;
  SPicture  sMeta;
  std::vector<uint8_t>   vBuffer;
  std::vector<uint32_t>  vRefMbType;
  std::vector<uint8_t>   vRefMbQp;
  std::vector<SMVUnitXY> vMvList;
  std::vector<int32_t>   vMbSkipSad;
};

struct SFrameBudgetBob {
  bool             bHookInstalled;
  BudgetMbInitFn   pfOrigMbInit;
  int32_t          iLayerCount;

  sWelsEncCtx             sCtxSave;
  SWelsSvcCodingParam     sParamSave;
  SWelsEncoderOutput      sOutSave;
  bool                    bHaveVaa;
  SVAAFrameInfo           sVaaSave;

  std::vector<SWelsSvcRc>  vRcSave;
  std::vector<SLTRState>   vLtrSave;
  std::vector<SRefList>    vRefListSave;
  std::vector<SPicSave>    vPics;
  std::vector<uint8_t>     vVppSave;

  SFrameBudgetBob()
    : bHookInstalled (false), pfOrigMbInit (NULL), iLayerCount (0), bHaveVaa (false) {
    std::memset (&sCtxSave, 0, sizeof (sCtxSave));
    std::memset (&sOutSave, 0, sizeof (sOutSave));
    std::memset (&sVaaSave, 0, sizeof (sVaaSave));
  }
};

static SPicSave* FindPicSave (SFrameBudgetBob* pBob, const SPicture* pPic) {
  for (size_t i = 0; i < pBob->vPics.size(); ++i) {
    if (pBob->vPics[i].pObj == pPic)
      return &pBob->vPics[i];
  }
  return NULL;
}

static void CapturePicture (SFrameBudgetBob* pBob, SPicture* pPic) {
  if (pPic == NULL || FindPicSave (pBob, pPic) != NULL)
    return;

  SPicSave sSave;
  sSave.pObj = pPic;
  std::memset (&sSave.sMeta, 0, sizeof (SPicture));
  std::memcpy (&sSave.sMeta, pPic, sizeof (SPicture));

  const int32_t iW = pPic->iWidthInPixel;
  const int32_t iH = pPic->iHeightInPixel;
  if (pPic->pBuffer != NULL && iW > 0 && iH > 0) {
    sSave.vBuffer.resize (PictureBufferAllocSize (iW, iH));
    std::memcpy (&sSave.vBuffer[0], pPic->pBuffer, sSave.vBuffer.size());
  }

  const uint32_t uiMb = PictureMbCount (iW, iH);
  if (pPic->uiRefMbType != NULL) {
    sSave.vRefMbType.resize (uiMb);
    std::memcpy (&sSave.vRefMbType[0], pPic->uiRefMbType, uiMb * sizeof (uint32_t));
  }
  if (pPic->pRefMbQp != NULL) {
    sSave.vRefMbQp.resize (uiMb);
    std::memcpy (&sSave.vRefMbQp[0], pPic->pRefMbQp, uiMb * sizeof (uint8_t));
  }
  if (pPic->sMvList != NULL) {
    sSave.vMvList.resize (uiMb);
    std::memcpy (&sSave.vMvList[0], pPic->sMvList, uiMb * sizeof (SMVUnitXY));
  }
  if (pPic->pMbSkipSad != NULL) {
    sSave.vMbSkipSad.resize (uiMb);
    std::memcpy (&sSave.vMbSkipSad[0], pPic->pMbSkipSad, uiMb * sizeof (int32_t));
  }

  pBob->vPics.push_back (sSave);
}

static void CaptureBob (sWelsEncCtx* pCtx, SFrameBudgetBob* pBob) {
  const int32_t n = WELS_CLIP3 (pCtx->pSvcParam->iSpatialLayerNum, 1, MAX_DEPENDENCY_LAYER);
  pBob->iLayerCount = n;

  std::memcpy (&pBob->sCtxSave, pCtx, sizeof (sWelsEncCtx));
  BudgetCopyIn (&pBob->sParamSave, pCtx->pSvcParam, sizeof (SWelsSvcCodingParam));
  std::memcpy (&pBob->sOutSave, pCtx->pOut, sizeof (SWelsEncoderOutput));

  pBob->vRcSave.resize (n);
  pBob->vLtrSave.resize (n);
  pBob->vRefListSave.resize (n);
  for (int32_t d = 0; d < n; ++d) {
    if (pCtx->pWelsSvcRc != NULL)
      std::memcpy (&pBob->vRcSave[d], &pCtx->pWelsSvcRc[d], sizeof (SWelsSvcRc));
    if (pCtx->pLtr != NULL)
      std::memcpy (&pBob->vLtrSave[d], &pCtx->pLtr[d], sizeof (SLTRState));
    if (pCtx->ppRefPicListExt != NULL && pCtx->ppRefPicListExt[d] != NULL)
      std::memcpy (&pBob->vRefListSave[d], pCtx->ppRefPicListExt[d], sizeof (SRefList));
    else
      std::memset (&pBob->vRefListSave[d], 0, sizeof (SRefList));
  }

  pBob->vPics.clear();
  if (pCtx->ppRefPicListExt != NULL) {
    for (int32_t d = 0; d < n; ++d) {
      SRefList* pList = pCtx->ppRefPicListExt[d];
      if (pList == NULL)
        continue;
      for (int32_t k = 0; k <= MAX_REF_PIC_COUNT; ++k)
        CapturePicture (pBob, pList->pRef[k]);
    }
  }

  pBob->bHaveVaa = (pCtx->pVaa != NULL);
  if (pCtx->pVaa != NULL)
    std::memcpy (&pBob->sVaaSave, pCtx->pVaa, sizeof (SVAAFrameInfo));

  // The pre-processor rotates its internal source-picture pointer ring on every
  // BuildSpatialPicList(); roll that object image back too. Both concrete
  // strategies (Video/Screen) add no data members, so the base size covers them.
  if (pCtx->pVpp != NULL) {
    pBob->vVppSave.resize (sizeof (CWelsPreProcess));
    std::memcpy (&pBob->vVppSave[0], pCtx->pVpp, sizeof (CWelsPreProcess));
  } else {
    pBob->vVppSave.clear();
  }
}

static void RestoreBob (sWelsEncCtx* pCtx, SFrameBudgetBob* pBob) {
  /* Restore the picture payloads/metadata first (same SPicture objects stay
   * allocated for the whole fixed-configuration lifetime). */
  for (size_t i = 0; i < pBob->vPics.size(); ++i) {
    SPicSave& s = pBob->vPics[i];
    SPicture* pPic = s.pObj;
    if (pPic == NULL)
      continue;

    if (!s.vBuffer.empty() && pPic->pBuffer != NULL)
      std::memcpy (pPic->pBuffer, &s.vBuffer[0], s.vBuffer.size());
    if (!s.vRefMbType.empty() && pPic->uiRefMbType != NULL)
      std::memcpy (pPic->uiRefMbType, &s.vRefMbType[0], s.vRefMbType.size() * sizeof (uint32_t));
    if (!s.vRefMbQp.empty() && pPic->pRefMbQp != NULL)
      std::memcpy (pPic->pRefMbQp, &s.vRefMbQp[0], s.vRefMbQp.size() * sizeof (uint8_t));
    if (!s.vMvList.empty() && pPic->sMvList != NULL)
      std::memcpy (pPic->sMvList, &s.vMvList[0], s.vMvList.size() * sizeof (SMVUnitXY));
    if (!s.vMbSkipSad.empty() && pPic->pMbSkipSad != NULL)
      std::memcpy (pPic->pMbSkipSad, &s.vMbSkipSad[0], s.vMbSkipSad.size() * sizeof (int32_t));

    std::memcpy (pPic, &s.sMeta, sizeof (SPicture));
  }

  /* Whole-context byte image brings back every inline scalar/array and all
   * stable pointers. Restore the pointed-to parameter/RC/ref-list blocks too
   * because their contents are not inside sWelsEncCtx. */
  std::memcpy (pCtx, &pBob->sCtxSave, sizeof (sWelsEncCtx));

  const int32_t n = pBob->iLayerCount;
  if (pCtx->pSvcParam != NULL)
    BudgetCopyIn (pCtx->pSvcParam, &pBob->sParamSave, sizeof (SWelsSvcCodingParam));
  if (pCtx->pWelsSvcRc != NULL) {
    for (int32_t d = 0; d < n && d < static_cast<int32_t> (pBob->vRcSave.size()); ++d)
      std::memcpy (&pCtx->pWelsSvcRc[d], &pBob->vRcSave[d], sizeof (SWelsSvcRc));
  }
  if (pCtx->pLtr != NULL) {
    for (int32_t d = 0; d < n && d < static_cast<int32_t> (pBob->vLtrSave.size()); ++d)
      std::memcpy (&pCtx->pLtr[d], &pBob->vLtrSave[d], sizeof (SLTRState));
  }
  if (pCtx->ppRefPicListExt != NULL) {
    for (int32_t d = 0; d < n && d < static_cast<int32_t> (pBob->vRefListSave.size()); ++d) {
      if (pCtx->ppRefPicListExt[d] != NULL)
        std::memcpy (pCtx->ppRefPicListExt[d], &pBob->vRefListSave[d], sizeof (SRefList));
    }
  }
  if (pCtx->pOut != NULL)
    std::memcpy (pCtx->pOut, &pBob->sOutSave, sizeof (SWelsEncoderOutput));
  if (pBob->bHaveVaa && pCtx->pVaa != NULL)
    std::memcpy (pCtx->pVaa, &pBob->sVaaSave, sizeof (SVAAFrameInfo));
  if (!pBob->vVppSave.empty() && pCtx->pVpp != NULL)
    std::memcpy (pCtx->pVpp, &pBob->vVppSave[0], pBob->vVppSave.size());

  /* The byte restore resets the memento back-pointer field to the saved value;
   * make sure it still names this (stable) object. */
  pCtx->pFrameBudgetBob = pBob;
}

/* Chain through the production per-MB RC initialisation (which keeps all GOM
 * bookkeeping correct) and then pin the QP actually used for quantization so no
 * adaptive/macroblock QP adjustment survives. */
static void FrameBudgetMbInit (sWelsEncCtx* pCtx, SMB* pCurMb, SSlice* pSlice) {
  SFrameBudgetBob* pBob = static_cast<SFrameBudgetBob*> (pCtx->pFrameBudgetBob);
  if (pBob != NULL && pBob->pfOrigMbInit != NULL)
    pBob->pfOrigMbInit (pCtx, pCurMb, pSlice);

  if (pCtx->bFrameBudgetConstQp && pCurMb != NULL) {
    int32_t iQp = pCtx->iFrameBudgetForceQp;
    iQp = WELS_CLIP3 (iQp, 0, 51);
    uint8_t uiChromaOffset = 0;
    if (pCtx->pCurDqLayer != NULL && pCtx->pCurDqLayer->sLayerInfo.pPpsP != NULL)
      uiChromaOffset = pCtx->pCurDqLayer->sLayerInfo.pPpsP->uiChromaQpIndexOffset;
    pCurMb->uiLumaQp = static_cast<uint8_t> (iQp);
    pCurMb->uiChromaQp = g_kuiChromaQpTable[CLIP3_QP_0_51 (iQp + uiChromaOffset)];
  }
}

static void EnsureHookInstalled (sWelsEncCtx* pCtx, SFrameBudgetBob* pBob) {
  if (pBob->bHookInstalled || pCtx->pFuncList == NULL)
    return;
  pBob->pfOrigMbInit = pCtx->pFuncList->pfRc.pfWelsRcMbInit;
  pCtx->pFuncList->pfRc.pfWelsRcMbInit = FrameBudgetMbInit;
  pBob->bHookInstalled = true;
}

static void ZeroFrameBsInfo (SFrameBSInfo* pFbi) {
  if (pFbi == NULL)
    return;
  const int32_t iMaxLayer = MAX_LAYER_NUM_OF_FRAME;
  for (int32_t i = 0; i < iMaxLayer; ++i) {
    pFbi->sLayerInfo[i].iNalCount = 0;
    pFbi->sLayerInfo[i].eFrameType = videoFrameTypeSkip;
  }
  pFbi->iLayerNum = 0;
  pFbi->iFrameSizeInBytes = 0;
  pFbi->eFrameType = videoFrameTypeInvalid;
}

} // anonymous namespace

/* ================================================================== */
/* Public core API                                                      */
/* ================================================================== */

bool FrameBudgetOptionValid (const SFrameBudget* pBudget) {
  if (pBudget == NULL)
    return false;
  if (pBudget->iMaxBytes == 0)
    return true; // switch off; QP fields are ignored
  if (pBudget->iMaxBytes < 0)
    return false;
  return pBudget->iFirstQp >= 0 && pBudget->iFirstQp <= pBudget->iLastQp
         && pBudget->iLastQp <= 51;
}

bool FrameBudgetParamSupported (const SEncParamExt* p) {
  if (p == NULL)
    return false;

  if (p->iSpatialLayerNum != 1)
    return false;
  if (p->iMultipleThreadIdc != 1)
    return false;
  if (p->iTemporalLayerNum < 1 || p->iTemporalLayerNum > 3)
    return false;

  if (!DimAligned16 (p->iPicWidth) || !DimAligned16 (p->iPicHeight))
    return false;

  const SSpatialLayerConfig& sLayer = p->sSpatialLayers[0];
  if (!DimAligned16 (sLayer.iVideoWidth) || !DimAligned16 (sLayer.iVideoHeight))
    return false;
  if (sLayer.iVideoWidth != p->iPicWidth || sLayer.iVideoHeight != p->iPicHeight)
    return false;

  if (p->iRCMode != RC_BITRATE_MODE)
    return false;
  if (p->bEnableFrameSkip)
    return false;
  if (p->iPaddingFlag != 0)
    return false;
  if (p->bEnableLongTermReference)
    return false;
  if (p->bEnableDenoise)
    return false;
  if (p->bEnableBackgroundDetection)
    return false;
  if (p->bEnableSceneChangeDetect)
    return false;
  if (p->eSpsPpsIdStrategy != CONSTANT_ID)
    return false;

  const SliceModeEnum eSliceMode = sLayer.sSliceArgument.uiSliceMode;
  if (eSliceMode != SM_SINGLE_SLICE && eSliceMode != SM_FIXEDSLCNUM_SLICE)
    return false;
  if (eSliceMode == SM_FIXEDSLCNUM_SLICE && sLayer.sSliceArgument.uiSliceNum < 1)
    return false;

  if (p->iEntropyCodingModeFlag != 0 && p->iEntropyCodingModeFlag != 1)
    return false;

  return true;
}

bool FrameBudgetConfigSupported (TagWelsEncCtx* pRawCtx) {
  sWelsEncCtx* pCtx = reinterpret_cast<sWelsEncCtx*> (pRawCtx);
  if (pCtx == NULL || pCtx->pSvcParam == NULL)
    return false;
  return FrameBudgetParamSupported (static_cast<const SEncParamExt*> (pCtx->pSvcParam));
}

void FrameBudgetAfterRcPictureInit (TagWelsEncCtx* pRawCtx) {
  sWelsEncCtx* pCtx = reinterpret_cast<sWelsEncCtx*> (pRawCtx);
  if (pCtx == NULL || !pCtx->bFrameBudgetConstQp)
    return;

  int32_t iQp = WELS_CLIP3 (pCtx->iFrameBudgetForceQp, 0, 51);
  pCtx->iGlobalQp = iQp;
  if (pCtx->pWelsSvcRc != NULL)
    pCtx->pWelsSvcRc[pCtx->uiDependencyId].iAverageFrameQp = iQp;
}

int32_t FrameBudgetEncodeFrame (TagWelsEncCtx* pRawCtx, SFrameBSInfo* pFbi,
                                const SSourcePicture* pSrcPic,
                                const SFrameBudget* pBudget) {
  sWelsEncCtx* pCtx = reinterpret_cast<sWelsEncCtx*> (pRawCtx);
  if (pCtx == NULL || pFbi == NULL || pSrcPic == NULL || !FrameBudgetOptionValid (pBudget)
      || pBudget->iMaxBytes <= 0) {
    return ENC_RETURN_INVALIDINPUT;
  }
  if (!FrameBudgetConfigSupported (pRawCtx))
    return ENC_RETURN_UNSUPPORTED_PARA;

  SFrameBudgetBob* pBob = static_cast<SFrameBudgetBob*> (pCtx->pFrameBudgetBob);
  if (pBob == NULL) {
    pBob = new (std::nothrow) SFrameBudgetBob();
    if (pBob == NULL)
      return ENC_RETURN_MEMALLOCERR;
    pCtx->pFrameBudgetBob = pBob;
  }
  EnsureHookInstalled (pCtx, pBob);

  /* Pristine pre-frame image: every rejected candidate is rolled back to it. */
  CaptureBob (pCtx, pBob);

  const int32_t iFirstQp = pBudget->iFirstQp;
  const int32_t iLastQp  = pBudget->iLastQp;

  for (int32_t iQp = iFirstQp; iQp <= iLastQp; ++iQp) {
    RestoreBob (pCtx, pBob);
    EnsureHookInstalled (pCtx, pBob);
    /* RestoreBob() brought back the inactive flag; arm this attempt. */
    pCtx->bFrameBudgetConstQp = true;
    pCtx->iFrameBudgetForceQp = iQp;
    if (pCtx->pFuncList != NULL)
      pCtx->pFuncList->pfRc.pfWelsRcMbInit = FrameBudgetMbInit;

    int32_t iRet = WelsEncoderEncodeExt (pCtx, pFbi, pSrcPic);

    pCtx->bFrameBudgetConstQp = false;

    if (iRet == ENC_RETURN_MEMALLOCERR || iRet == ENC_RETURN_MEMOVERFLOWFOUND
        || iRet == ENC_RETURN_VLCOVERFLOWFOUND) {
      // Catastrophic bitstream-buffer failures keep the historical contract:
      // the plus layer reports cmMallocMemeError and tears the context down.
      // Do not roll the context image back here because an overflow recovery may
      // have reallocated the bitstream buffers and invalidated saved pointers.
      return iRet;
    }
    if (iRet == ENC_RETURN_INVALIDINPUT) {
      RestoreBob (pCtx, pBob);
      pCtx->bFrameBudgetConstQp = false;
      return iRet;
    }
    if (iRet != ENC_RETURN_SUCCESS) {
      // Any other coding failure: roll this candidate back and try the next QP.
      continue;
    }

    const int32_t iBytes  = pFbi->iFrameSizeInBytes;
    const int32_t iLayers = pFbi->iLayerNum;
    if (iLayers <= 0 || iBytes <= 0) {
      // A skipped frame is not a valid budget candidate (frame skip is off in
      // the supported envelope); roll back and continue.
      continue;
    }

    if (iBytes <= pBudget->iMaxBytes) {
      // Commit: deliberately keep the encoder state produced by this candidate.
      pCtx->bFrameBudgetConstQp = false;
      pCtx->iFrameBudgetForceQp = iQp;
      if (pCtx->pFuncList != NULL && pBob->bHookInstalled)
        pCtx->pFuncList->pfRc.pfWelsRcMbInit = FrameBudgetMbInit; // delegates when inactive
      return ENC_RETURN_SUCCESS;
    }
    // Too large: loop top restores the pristine image before the next attempt.
  }

  /* No candidate fit: observable state is exactly "no input frame received". */
  RestoreBob (pCtx, pBob);
  pCtx->bFrameBudgetConstQp = false;
  if (pCtx->pFuncList != NULL && pBob->bHookInstalled)
    pCtx->pFuncList->pfRc.pfWelsRcMbInit = FrameBudgetMbInit;
  ZeroFrameBsInfo (pFbi);
  return ENC_RETURN_FRAME_BUDGET_EXCEEDED;
}

void FrameBudgetDestroyBob (TagWelsEncCtx* pRawCtx) {
  sWelsEncCtx* pCtx = reinterpret_cast<sWelsEncCtx*> (pRawCtx);
  if (pCtx == NULL)
    return;
  SFrameBudgetBob* pBob = static_cast<SFrameBudgetBob*> (pCtx->pFrameBudgetBob);
  if (pBob != NULL) {
    // Restore the original production MB hook before dropping the memento.
    if (pCtx->pFuncList != NULL && pBob->bHookInstalled && pBob->pfOrigMbInit != NULL)
      pCtx->pFuncList->pfRc.pfWelsRcMbInit = pBob->pfOrigMbInit;
    delete pBob;
    pCtx->pFrameBudgetBob = NULL;
  }
}

} // namespace WelsEnc
