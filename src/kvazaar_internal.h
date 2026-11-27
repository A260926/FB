#ifndef KVAZAAR_INTERNAL_H_
#define KVAZAAR_INTERNAL_H_
/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (c) 2021, Tampere University, ITU/ISO/IEC, project contributors
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 * 
 * * Neither the name of the Tampere University or ITU/ISO/IEC nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * INCLUDING NEGLIGENCE OR OTHERWISE ARISING IN ANY WAY OUT OF THE USE OF THIS
 ****************************************************************************/

/**
 * \ingroup Control
 * \file
 * \brief Definitions for opaque structs in kvazaar.h
 */

#include "global.h" // IWYU pragma: keep

#include "kvazaar.h"
#include "input_frame_buffer.h"

/* ROI-aware rate control observation support. Always compiled; recording is
 * gated at run time by kvz_roi_rc_enable_observation. */
#ifndef KVZ_ROI_RC_OBSERVATION
#define KVZ_ROI_RC_OBSERVATION 1
#endif

/**
 * \brief Internal ROI-aware rate control observation used by the tests.
 *
 * The observation stays inactive (and allocation-free) until
 * kvz_roi_rc_enable_observation(1) is called. Each sample describes one fully
 * encoded frame.
 */
typedef struct kvz_roi_rc_frame_sample {
  int frame_num;
  int frame_poc;
  int frame_qp;
  int is_irap;
  int ctu_count;
  int budget_count;     /*!< Times a whole-frame budget was formed. */
  int effective;        /*!< Nonzero if a nonzero ROI budget was applied. */
  double frame_target_bits;
  double planned_bits;  /*!< Sum of per-CTU planned targets (budget only). */
  double actual_bits;   /*!< Bits actually produced for the frame. */
  double *targets;      /*!< Per-CTU planned target bits (owned). */
  double *final_lambdas;/*!< Per-CTU final lambda actually used. */
  double *final_qps;    /*!< Per-CTU final QP actually used. */
  double *actual_ctu_bits; /*!< Per-CTU bits from CABAC. */
  double *model_lambdas; /*!< Per-CTU lambda fed to the model next frame. */
  double *model_raw_lambdas; /*!< Per-CTU open-loop model lambda from target */
  int *model_clamped;   /*!< Per-CTU flag: final QP was clamped */
  int *roi_final_qp;    /*!< Per-CTU final QP recorded during encoding */
  double *rc_alphas;    /*!< Per-CTU alpha used to derive this frame. */
  double *rc_betas;     /*!< Per-CTU beta used to derive this frame. */
  double *sse;          /*!< Per-CTU luma SSE of reconstruction vs source. */
  uint32_t *ctu_pixels; /*!< Per-CTU valid luma area. */
  int8_t *roi_offsets;  /*!< Per-CTU ROI delta QP mapped to global grid. */
  double *base_weights; /*!< Per-CTU base spatial weight b_i used for budget. */
} kvz_roi_rc_frame_sample;

/**
 * \brief Enable or disable ROI-aware observation recording.
 *
 * Off by default. The test program enables it explicitly; ordinary encoder
 * users never see the recording overhead.
 */
void kvz_roi_rc_enable_observation(int enable);

/**
 * \brief Copy the ROI-aware observation for an encoded frame.
 *
 * \return 1 on success, 0 if no sample is available yet.
 */
int kvz_roi_rc_get_sample(int index, kvz_roi_rc_frame_sample *out);

/**
 * \brief Get the number of observation samples recorded so far.
 */
int kvz_roi_rc_sample_count(void);

/**
 * \brief Free and reset the observation buffer (for a fresh encode in tests).
 */
void kvz_roi_rc_clear_samples(void);

// Forward declarations.
struct encoder_state_t;
struct encoder_control_t;

struct kvz_encoder {
  const struct encoder_control_t* control;
  struct encoder_state_t* states;
  unsigned num_encoder_states;

  /**
   * \brief Number of the current encoder state.
   *
   * The current state is the one that will be used for encoding the frame
   * that is started next.
   */
  unsigned cur_state_num;

  /**
   * \brief Number of the next encoder state to be finished.
   */
  unsigned out_state_num;

  /**
   * \brief Buffer for input frames.
   */
  input_frame_buffer_t input_buffer;

  unsigned frames_started;
  unsigned frames_done;
};

#endif // KVAZAAR_INTERNAL_H_
