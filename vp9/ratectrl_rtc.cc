/*
 *  Copyright (c) 2020 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "vp9/ratectrl_rtc.h"

#include <new>

#include "vp9/common/vp9_common.h"
#include "vp9/encoder/vp9_aq_cyclicrefresh.h"
#include "vp9/encoder/vp9_encoder.h"
#include "vp9/encoder/vp9_picklpf.h"
#include "vp9/ratectrl_rtc_pipeline.h"
#include "vpx/vp8cx.h"
#include "vpx/vpx_codec.h"
#include "vpx_mem/vpx_mem.h"

namespace libvpx {

// Maximum number of frames that may wait for size feedback in pipeline mode.
static const int kMaxPendingFramesLimit = 8;

std::unique_ptr<VP9RateControlRTC> VP9RateControlRTC::Create(
    const VP9RateControlRtcConfig &cfg) {
  if (cfg.max_pending_frames != 0) {
    // Pipelined interface: validate the supported configuration subset.
    if (cfg.max_pending_frames < 1 ||
        cfg.max_pending_frames > kMaxPendingFramesLimit)
      return nullptr;
    if (cfg.ss_number_layers != 1) return nullptr;
    if (cfg.ts_number_layers < 1 || cfg.ts_number_layers > 2) return nullptr;
    if (cfg.rc_mode != VPX_CBR) return nullptr;
    if (cfg.aq_mode != 0) return nullptr;
  }
  std::unique_ptr<VP9RateControlRTC> rc_api(new (std::nothrow)
                                                VP9RateControlRTC());
  if (!rc_api) return nullptr;
  rc_api->cpi_ = static_cast<VP9_COMP *>(vpx_memalign(32, sizeof(*cpi_)));
  if (!rc_api->cpi_) return nullptr;
  vp9_zero(*rc_api->cpi_);

  if (!rc_api->InitRateControl(cfg)) return nullptr;
  if (cfg.aq_mode) {
    if (!VP9RcCpiAllocAqData(rc_api->cpi_)) return nullptr;
  }
  if (cfg.max_pending_frames != 0) {
    rc_api->pipeline_mode_ = true;
    rc_api->max_pending_frames_ = cfg.max_pending_frames;
    rc_api->pipeline_.reset(new (std::nothrow) VP9RcPipelineState());
    if (!rc_api->pipeline_) return nullptr;
    Vp9RcSaveSnapshot(rc_api->cpi_, &rc_api->pipeline_->snapshot);
  }
  return rc_api;
}

VP9RateControlRTC::~VP9RateControlRTC() {
  pipeline_.reset();
  if (cpi_) {
    VP9RcCpiFree(cpi_);
    cpi_ = nullptr;
  }
}

void VP9RcCpiFree(VP9_COMP *cpi) {
  if (!cpi) return;
  for (int layer = 0; layer < VPX_MAX_LAYERS; layer++) {
    LAYER_CONTEXT *const lc = &cpi->svc.layer_context[layer];
    vpx_free(lc->map);
    vpx_free(lc->last_coded_q_map);
    vpx_free(lc->consec_zero_mv);
  }
  if (cpi->oxcf.aq_mode == CYCLIC_REFRESH_AQ) {
    vpx_free(cpi->segmentation_map);
    cpi->segmentation_map = NULL;
    vp9_cyclic_refresh_free(cpi->cyclic_refresh);
  }
  vpx_free(cpi);
}

bool VP9RcCpiAllocAqData(VP9_COMP *cpi) {
  cpi->segmentation_map = static_cast<uint8_t *>(
      vpx_calloc(cpi->common.mi_rows * cpi->common.mi_cols,
                 sizeof(*cpi->segmentation_map)));
  if (!cpi->segmentation_map) return false;
  cpi->cyclic_refresh =
      vp9_cyclic_refresh_alloc(cpi->common.mi_rows, cpi->common.mi_cols);
  if (!cpi->cyclic_refresh) return false;
  cpi->cyclic_refresh->content_mode = 0;
  return true;
}

bool VP9RateControlRTC::InitRateControl(const VP9RateControlRtcConfig &cfg) {
  return VP9RcInitCpi(cpi_, cfg, &initial_width_, &initial_height_,
                      &rc_is_valid_);
}

bool VP9RcInitCpi(VP9_COMP *cpi, const VP9RateControlRtcConfig &rc_cfg,
                  int *initial_width, int *initial_height,
                  bool *rc_is_valid) {
  VP9_COMMON *cm = &cpi->common;
  VP9EncoderConfig *oxcf = &cpi->oxcf;
  RATE_CONTROL *const rc = &cpi->rc;
  cm->profile = PROFILE_0;
  cm->bit_depth = VPX_BITS_8;
  cm->show_frame = 1;
  oxcf->profile = cm->profile;
  oxcf->bit_depth = cm->bit_depth;
  oxcf->rc_mode = rc_cfg.rc_mode;
  oxcf->pass = 0;
  oxcf->aq_mode = rc_cfg.aq_mode ? CYCLIC_REFRESH_AQ : NO_AQ;
  oxcf->content = VP9E_CONTENT_DEFAULT;
  oxcf->drop_frames_water_mark = 0;
  cm->current_video_frame = 0;
  rc->kf_boost = DEFAULT_KF_BOOST;

  if (!VP9RcUpdateCpiConfig(cpi, rc_cfg, initial_width, initial_height,
                            rc_is_valid))
    return false;
  vp9_set_mb_mi(cm, cm->width, cm->height);

  cpi->use_svc = (cpi->svc.number_spatial_layers > 1 ||
                  cpi->svc.number_temporal_layers > 1)
                     ? 1
                     : 0;

  rc->rc_1_frame = 0;
  rc->rc_2_frame = 0;
  vp9_rc_init_minq_luts();
  vp9_rc_init(oxcf, 0, rc);
  rc->constrain_gf_key_freq_onepass_vbr = 0;
  cpi->sf.use_nonrd_pick_mode = 1;
  *rc_is_valid = true;
  return true;
}

bool VP9RateControlRTC::UpdateRateControl(
    const VP9RateControlRtcConfig &rc_cfg) {
  // In pipeline mode the configuration is fixed after Create(); report
  // failure and keep the current state.
  if (pipeline_mode_) return false;
  return VP9RcUpdateCpiConfig(cpi_, rc_cfg, &initial_width_, &initial_height_,
                              &rc_is_valid_);
}

bool VP9RcUpdateCpiConfig(VP9_COMP *cpi, const VP9RateControlRtcConfig &rc_cfg,
                          int *initial_width, int *initial_height,
                          bool *rc_is_valid) {
  // Since VPX_MAX_LAYERS (12) is less than the product of VPX_SS_MAX_LAYERS (5)
  // and VPX_TS_MAX_LAYERS (5), check all three.
  if (rc_cfg.ss_number_layers < 1 ||
      rc_cfg.ss_number_layers > VPX_SS_MAX_LAYERS ||
      rc_cfg.ts_number_layers < 1 ||
      rc_cfg.ts_number_layers > VPX_TS_MAX_LAYERS ||
      rc_cfg.ss_number_layers * rc_cfg.ts_number_layers > VPX_MAX_LAYERS) {
    *rc_is_valid = false;
    return false;
  }
  if (*initial_width == 0 && *initial_height == 0) {
    *initial_width = rc_cfg.width;
    *initial_height = rc_cfg.height;
  }
  if (rc_cfg.width > *initial_width || rc_cfg.height > *initial_height) {
    *rc_is_valid = false;
    return false;
  }

  VP9_COMMON *cm = &cpi->common;
  VP9EncoderConfig *oxcf = &cpi->oxcf;
  RATE_CONTROL *const rc = &cpi->rc;

  cm->width = rc_cfg.width;
  cm->height = rc_cfg.height;
  oxcf->width = rc_cfg.width;
  oxcf->height = rc_cfg.height;
  oxcf->worst_allowed_q = vp9_quantizer_to_qindex(rc_cfg.max_quantizer);
  oxcf->best_allowed_q = vp9_quantizer_to_qindex(rc_cfg.min_quantizer);
  rc->worst_quality = oxcf->worst_allowed_q;
  rc->best_quality = oxcf->best_allowed_q;
  oxcf->init_framerate = rc_cfg.framerate;
  oxcf->target_bandwidth = 1000 * rc_cfg.target_bandwidth;
  oxcf->starting_buffer_level_ms = rc_cfg.buf_initial_sz;
  oxcf->optimal_buffer_level_ms = rc_cfg.buf_optimal_sz;
  oxcf->maximum_buffer_size_ms = rc_cfg.buf_sz;
  oxcf->under_shoot_pct = rc_cfg.undershoot_pct;
  oxcf->over_shoot_pct = rc_cfg.overshoot_pct;
  oxcf->drop_frames_water_mark = rc_cfg.frame_drop_thresh;
  oxcf->content = rc_cfg.is_screen ? VP9E_CONTENT_SCREEN : VP9E_CONTENT_DEFAULT;
  oxcf->ss_number_layers = rc_cfg.ss_number_layers;
  oxcf->ts_number_layers = rc_cfg.ts_number_layers;
  oxcf->temporal_layering_mode =
      (VP9E_TEMPORAL_LAYERING_MODE)((rc_cfg.ts_number_layers > 1)
                                        ? rc_cfg.ts_number_layers
                                        : 0);

  oxcf->rc_max_intra_bitrate_pct = rc_cfg.max_intra_bitrate_pct;
  oxcf->rc_max_inter_bitrate_pct = rc_cfg.max_inter_bitrate_pct;
  cpi->framerate = rc_cfg.framerate;
  cpi->svc.number_spatial_layers = rc_cfg.ss_number_layers;
  cpi->svc.number_temporal_layers = rc_cfg.ts_number_layers;

  vp9_set_mb_mi(cm, cm->width, cm->height);

  if (setjmp(cpi->common.error.jmp)) {
    cpi->common.error.setjmp = 0;
    vpx_clear_system_state();
    return false;
  }
  cpi->common.error.setjmp = 1;

  for (int tl = 0; tl < cpi->svc.number_temporal_layers; ++tl) {
    oxcf->ts_rate_decimator[tl] = rc_cfg.ts_rate_decimator[tl];
  }
  for (int sl = 0; sl < cpi->svc.number_spatial_layers; ++sl) {
    if (rc_cfg.scaling_factor_den[sl] < rc_cfg.scaling_factor_num[sl]) {
      *rc_is_valid = false;
      return false;
    }
    for (int tl = 0; tl < cpi->svc.number_temporal_layers; ++tl) {
      const int layer =
          LAYER_IDS_TO_IDX(sl, tl, cpi->svc.number_temporal_layers);
      LAYER_CONTEXT *lc = &cpi->svc.layer_context[layer];
      RATE_CONTROL *const lrc = &lc->rc;
      oxcf->layer_target_bitrate[layer] =
          1000 * rc_cfg.layer_target_bitrate[layer];
      if (rc_cfg.max_quantizers[layer] > 63 ||
          rc_cfg.min_quantizers[layer] < 0 ||
          rc_cfg.min_quantizers[layer] > rc_cfg.max_quantizers[layer]) {
        *rc_is_valid = false;
        return false;
      }
      lrc->worst_quality =
          vp9_quantizer_to_qindex(rc_cfg.max_quantizers[layer]);
      lrc->best_quality = vp9_quantizer_to_qindex(rc_cfg.min_quantizers[layer]);
      lc->scaling_factor_num = rc_cfg.scaling_factor_num[sl];
      lc->scaling_factor_den = rc_cfg.scaling_factor_den[sl];
    }
  }
  vp9_set_rc_buffer_sizes(cpi);
  vp9_new_framerate(cpi, cpi->framerate);
  if (cpi->svc.number_temporal_layers > 1 ||
      cpi->svc.number_spatial_layers > 1) {
    if (cm->current_video_frame == 0) {
      for (int layer = 0; layer < VPX_MAX_LAYERS; layer++) {
        LAYER_CONTEXT *const lc = &cpi->svc.layer_context[layer];
        vpx_free(lc->map);
        vpx_free(lc->last_coded_q_map);
        vpx_free(lc->consec_zero_mv);
        lc->map = nullptr;
        lc->last_coded_q_map = nullptr;
        lc->consec_zero_mv = nullptr;
      }
      vp9_init_layer_context(cpi);
      // svc->framedrop_mode is not currently exposed, so only allow for
      // full superframe drop for now.
      cpi->svc.framedrop_mode = FULL_SUPERFRAME_DROP;
    }
    vp9_update_layer_context_change_config(cpi,
                                           (int)cpi->oxcf.target_bandwidth);
    cpi->svc.max_consec_drop = rc_cfg.max_consec_drop;
  }
  vp9_check_reset_rc_flag(cpi);

  cpi->common.error.setjmp = 0;
  *rc_is_valid = true;
  return true;
}

// Compute the QP for the frame. If the frame is dropped this function
// returns kDrop, and no QP is computed. If the frame is encoded (not dropped)
// the QP is computed and kOk is returned.
FrameDropDecision VP9RateControlRTC::ComputeQP(
    const VP9FrameParamsQpRTC &frame_params) {
  if (pipeline_mode_) return FrameDropDecision::kDrop;
  if (!rc_is_valid_) return FrameDropDecision::kDrop;
  if (frame_params.spatial_layer_id < 0 ||
      frame_params.spatial_layer_id >= cpi_->svc.number_spatial_layers ||
      frame_params.temporal_layer_id < 0 ||
      frame_params.temporal_layer_id >= cpi_->svc.number_temporal_layers ||
      initial_width_ == 0 || initial_height_ == 0) {
    return FrameDropDecision::kDrop;
  }
  Vp9RcFrameTrace trace;
  Vp9RcDecideFrame(cpi_, frame_params, &trace);
  return trace.dropped ? FrameDropDecision::kDrop : FrameDropDecision::kOk;
}

int VP9RateControlRTC::GetQP() const {
  if (!rc_is_valid_ || pipeline_mode_) return 0;
  return cpi_->common.base_qindex;
}

int VP9RateControlRTC::GetLoopfilterLevel() const {
  if (!rc_is_valid_ || pipeline_mode_) return 0;
  return Vp9RcPickLoopfilter(cpi_);
}

bool VP9RateControlRTC::GetSegmentationData(
    VP9SegmentationData *segmentation_data) const {
  if (!rc_is_valid_ || pipeline_mode_) return false;
  if (!cpi_->cyclic_refresh || !cpi_->cyclic_refresh->apply_cyclic_refresh) {
    return false;
  }

  segmentation_data->segmentation_map = cpi_->segmentation_map;
  segmentation_data->segmentation_map_size =
      cpi_->common.mi_cols * cpi_->common.mi_rows;
  segmentation_data->delta_q = cpi_->cyclic_refresh->qindex_delta;
  segmentation_data->delta_q_size = 3u;
  return true;
}

void VP9RateControlRTC::PostEncodeUpdate(
    uint64_t encoded_frame_size, const VP9FrameParamsQpRTC &frame_params) {
  if (!rc_is_valid_ || pipeline_mode_) return;
  cpi_->common.frame_type = static_cast<FRAME_TYPE>(frame_params.frame_type);
  cpi_->svc.spatial_layer_id = frame_params.spatial_layer_id;
  cpi_->svc.temporal_layer_id = frame_params.temporal_layer_id;
  if (cpi_->svc.number_spatial_layers > 1 ||
      cpi_->svc.number_temporal_layers > 1) {
    vp9_restore_layer_context(cpi_);
    const int layer = LAYER_IDS_TO_IDX(cpi_->svc.spatial_layer_id,
                                       cpi_->svc.temporal_layer_id,
                                       cpi_->svc.number_temporal_layers);
    LAYER_CONTEXT *lc = &cpi_->svc.layer_context[layer];
    cpi_->common.base_qindex = lc->frame_qp;
    cpi_->common.MBs = lc->MBs;
    // For spatial-svc, allow cyclic-refresh to be applied on the spatial
    // layers, for the base temporal layer.
    if (cpi_->oxcf.aq_mode == CYCLIC_REFRESH_AQ &&
        cpi_->svc.number_spatial_layers > 1 &&
        cpi_->svc.temporal_layer_id == 0) {
      CYCLIC_REFRESH *const cr = cpi_->cyclic_refresh;
      cr->qindex_delta[0] = lc->qindex_delta[0];
      cr->qindex_delta[1] = lc->qindex_delta[1];
      cr->qindex_delta[2] = lc->qindex_delta[2];
    }
  }
  vp9_rc_postencode_update(cpi_, encoded_frame_size);
  if (cpi_->svc.number_spatial_layers > 1 ||
      cpi_->svc.number_temporal_layers > 1)
    vp9_save_layer_context(cpi_);
  cpi_->common.current_video_frame++;
}

void VP9RateControlRTC::GetRcObservation(VP9RcObservation *obs) {
  if (obs == nullptr) return;
  if (pipeline_mode_) {
    // Rebuild the current state: snapshot of the reclaimed prefix plus the
    // pending records replayed with their current costs.
    Vp9RcRestoreSnapshot(cpi_, pipeline_->snapshot);
    for (const Vp9RcPendingFrame &rec : pipeline_->records)
      Vp9RcReplayRecord(cpi_, rec);
  }
  Vp9RcObserveCpi(cpi_, obs);
}

}  // namespace libvpx
