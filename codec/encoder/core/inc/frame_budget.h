/*!
 * \copy
 *     Copyright (c) 2024
 *
 * \brief   Per-frame hard Annex-B byte budget (ENCODER_OPTION_FRAME_BUDGET).
 *
 *          The public option value SFrameBudget is declared in
 *          codec/api/wels/codec_app_def.h. This header exposes the small core
 *          entry points used by the encoder extension and by the core frame
 *          encoding loop. It is intentionally self contained so it can be
 *          force-included on its own by the task build script.
 */
#ifndef WELS_FRAME_BUDGET_H__
#define WELS_FRAME_BUDGET_H__

#include <stdint.h>

/* Relative to this header so it compiles both under the project make include
 * flags (-Icodec/api/wels) and under the task driver (-I. -Icodec/api ...). */
#include "../../../api/wels/codec_app_def.h"

#ifdef __cplusplus

namespace WelsEnc {

struct TagWelsEncCtx;

/*---------------------------------------------------------------------------*/
/* Option validation                                                          */
/*---------------------------------------------------------------------------*/

/*
 * \brief Validate the integer fields of an ENCODER_OPTION_FRAME_BUDGET value.
 *        iMaxBytes == 0 is always valid (it means "switch the mode off" and the
 *        QP fields are then ignored). When iMaxBytes > 0 the QP range must obey
 *        0 <= iFirstQp <= iLastQp <= 51 and iMaxBytes must be positive.
 * \return true when the value may be copied as the new active configuration.
 */
bool FrameBudgetOptionValid (const SFrameBudget* pBudget);

/*
 * \brief Check a (possibly not yet applied) extended parameter structure against
 *        the supported frame-budget envelope. Used both for the active context
 *        and to reject unsupported SetOption() parameter changes while the mode
 *        is enabled.
 */
bool FrameBudgetParamSupported (const SEncParamExt* pParam);

/*
 * \brief Check whether the current encoder configuration is inside the
 *        supported envelope of the frame budget mode:
 *        one spatial layer, one thread, 16-aligned dimensions, 1..3 temporal
 *        layers, RC_BITRATE_MODE, frame skip/padding/LTR/denoise/background/
 *        scene-detection off and CONSTANT_ID parameter sets. CAVLC/CABAC and
 *        single/fixed-number multi slice are both accepted.
 */
bool FrameBudgetConfigSupported (TagWelsEncCtx* pCtx);

/*---------------------------------------------------------------------------*/
/* Retry entry point                                                          */
/*---------------------------------------------------------------------------*/

/*
 * \brief Encode pSrcPic once per fixed QP from iFirstQp to iLastQp, returning
 *        the first candidate whose complete Annex-B access unit is no larger
 *        than pBudget->iMaxBytes.
 *
 * The encoder state is snapshotted before the attempts and fully restored after
 * every rejected candidate, so only the returned successful candidate becomes a
 * reference and updates RC/counters/timestamps. When no candidate fits the
 * state is restored to "no input received", pFbi is zeroed and
 * ENC_RETURN_FRAME_BUDGET_EXCEEDED is returned.
 *
 * \return ENC_RETURN_SUCCESS, ENC_RETURN_FRAME_BUDGET_EXCEEDED or a fatal
 *         ENC_RETURN_* code propagated from WelsEncoderEncodeExt.
 */
int32_t FrameBudgetEncodeFrame (TagWelsEncCtx* pCtx, SFrameBSInfo* pFbi,
                                const SSourcePicture* pSrcPic,
                                const SFrameBudget* pBudget);

/*
 * \brief Hook invoked by the core encoder immediately after
 *        pfWelsRcPictureInit() for a layer. Pins iGlobalQp to the per-attempt
 *        QP while an attempt is active.
 */
void FrameBudgetAfterRcPictureInit (TagWelsEncCtx* pCtx);

/*
 * \brief Release the lazily allocated rollback memento during teardown.
 */
void FrameBudgetDestroyBob (TagWelsEncCtx* pCtx);

} // namespace WelsEnc

#endif /* __cplusplus */

#endif /* WELS_FRAME_BUDGET_H__ */
