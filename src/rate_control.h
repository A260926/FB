#ifndef RATE_CONTROL_H_
#define RATE_CONTROL_H_
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
 * \brief Functions related to rate control.
 */

#include "global.h" // IWYU pragma: keep

#include "encoderstate.h"
#include "pthread.h"

typedef struct kvz_rc_data {
  double *c_para[KVZ_MAX_GOP_LAYERS];
  double *k_para[KVZ_MAX_GOP_LAYERS];
  double pic_c_para[KVZ_MAX_GOP_LAYERS];
  double pic_k_para[KVZ_MAX_GOP_LAYERS];
  double previous_lambdas[KVZ_MAX_GOP_LAYERS + 1];
  double previous_frame_lambda;
  double *intra_bpp;
  double *intra_dis;
  double intra_pic_distortion;
  double intra_pic_bpp;

  double intra_alpha;
  double intra_beta;

  pthread_rwlock_t ck_ctu_lock[KVZ_MAX_GOP_LAYERS];
  pthread_mutex_t ck_frame_lock;
  pthread_mutex_t lambda_lock;
  pthread_mutex_t intra_lock;

  int smoothing_window;

  FILE* dist_file;
  FILE* bits_file;
  FILE* qp_file;
  FILE* lambda_file;
} kvz_rc_data;

kvz_rc_data * kvz_get_rc_data(const encoder_control_t * const encoder);
void kvz_free_rc_data(kvz_rc_data* data);

void kvz_set_picture_lambda_and_qp(encoder_state_t * const state);

void kvz_set_lcu_lambda_and_qp(encoder_state_t * const state,
                               vector2d_t pos);

void kvz_set_ctu_qp_lambda(encoder_state_t * const state, vector2d_t pos);
void kvz_update_after_picture(encoder_state_t * const state);
void kvz_estimate_pic_lambda(encoder_state_t * const state);

/**
 * \brief Build the per-frame ROI-aware spatial bit budget.
 *
 * Must be called once per frame, on the main encoder state and after the
 * frame-level target bits have been set, before any CTU of the frame is
 * coded. When no usable non-zero ROI map is present the frame is left on the
 * classic rate control path and no budget is stored.
 */
void kvz_roi_rc_prepare_picture(encoder_state_t * const state);

/**
 * \brief Pure numeric core of the ROI spatial budget solver.
 *
 * Solves the per-CTU target t_i = clamp(c*b_i*2^(-d_i/6), B*p_i/(4P),
 * 4*B*p_i/P) with a single non-negative c so that the targets sum to B.
 * Exposed for internal tests; targets_out is only written on success.
 *
 * \return 1 on success, 0 on invalid input or an all-zero ROI map
 */
int kvz_roi_rc_solve_targets(int num_ctus,
                             const double *pixels,
                             const double *base_weights,
                             const int8_t *roi_delta,
                             double B,
                             double *targets_out);

#endif // RATE_CONTROL_H_
