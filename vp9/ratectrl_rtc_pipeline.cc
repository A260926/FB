/*
 *  Copyright (c) 2026 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "vp9/ratectrl_rtc_pipeline.h"

#include <climits>
#include <new>

#include "vp9/common/vp9_common.h"
#include "vp9/encoder/vp9_aq_cyclicrefresh.h"
#include "vp9/encoder/vp9_encoder.h"
#include "vp9/encoder/vp9_picklpf.h"
#include "vp9/encoder/vp9_svc_layercontext.h"
#include "vpx/vp8cx.h"
#include "vpx/vpx_codec.h"
#include "vpx_mem/vpx_mem.h"

namespace libvpx {

namespace {

// Frame costs (estimated or actual) are always clamped to [1, INT_MAX/8] so
// that the bit counts computed from them (bytes << 3) fit into an int.
uint64_t ClampFrameBytes(uint64_t bytes) {
  const uint64_t kMaxFrameBytes = static_cast<uint64_t>(INT_MAX) / 8;
  if (bytes < 1) return 1;
  if (bytes > kMaxFrameBytes) return kMaxFrameBytes;
  return bytes;
}

bool UseSvc(const VP9_COMP *cpi) {
  return cpi->svc.number_spatial_layers > 1 ||
         cpi->svc.number_temporal_layers > 1;
}

}  // namespace

VP9_COMP *VP9RcCpiCreate(const VP9RateControlRtcConfig &cfg) {
  VP9_COMP *cpi = static_cast<VP9_COMP *>(vpx_memalign(32, sizeof(*cpi)));
  if (!cpi) return nullptr;
  vp9_zero(*cpi);
  int initial_width = 0;
  int initial_height = 0;
  bool rc_is_valid = false;
  if (!VP9RcInitCpi(cpi, cfg, &initial_width, &initial_height,
                    &rc_is_valid)) {
    VP9RcCpiFree(cpi);
    return nullptr;
  }
  if (cfg.aq_mode && !VP9RcCpiAllocAqData(cpi)) {
    VP9RcCpiFree(cpi);
    return nullptr;
  }
  return cpi;
}

void VP9RcCpiDestroy(VP9_COMP *cpi) { VP9RcCpiFree(cpi); }

// Runs the pre-encode rate control flow for the next frame (same logic as
// the synchronous VP9RateControlRTC::ComputeQP()) and records the resulting
// frozen decision in *trace. Returns true if the frame is dropped; in that
// case the drop bookkeeping has already been applied.
bool Vp9RcDecideFrame(VP9_COMP *cpi, const VP9FrameParamsQpRTC &frame_params,
                      Vp9RcFrameTrace *trace) {
  VP9_COMMON *const cm = &cpi->common;
  SVC *const svc = &cpi->svc;
  const bool use_svc = UseSvc(cpi);
  int width = 0;
  int height = 0;
  svc->spatial_layer_id = frame_params.spatial_layer_id;
  svc->temporal_layer_id = frame_params.temporal_layer_id;
  if (svc->number_spatial_layers > 1) {
    const int layer = LAYER_IDS_TO_IDX(svc->spatial_layer_id,
                                       svc->temporal_layer_id,
                                       svc->number_temporal_layers);
    LAYER_CONTEXT *lc = &svc->layer_context[layer];
    get_layer_resolution(cpi->oxcf.width, cpi->oxcf.height,
                         lc->scaling_factor_num, lc->scaling_factor_den,
                         &width, &height);
    cm->width = width;
    cm->height = height;
  }
  vp9_set_mb_mi(cm, cm->width, cm->height);
  cm->frame_type = static_cast<FRAME_TYPE>(frame_params.frame_type);
  // This is needed to ensure key frame does not get unset in rc_get_svc_params.
  cpi->frame_flags = (cm->frame_type == KEY_FRAME) ? FRAMEFLAGS_KEY : 0;
  cpi->refresh_golden_frame = (cm->frame_type == KEY_FRAME) ? 1 : 0;
  cpi->sf.use_nonrd_pick_mode = 1;
  if (svc->number_spatial_layers == 1 && svc->number_temporal_layers == 1) {
    int target = 0;
    if (cpi->oxcf.rc_mode == VPX_CBR) {
      if (cpi->oxcf.aq_mode == CYCLIC_REFRESH_AQ)
        vp9_cyclic_refresh_update_parameters(cpi);
      if (frame_is_intra_only(cm))
        target = vp9_calc_iframe_target_size_one_pass_cbr(cpi);
      else
        target = vp9_calc_pframe_target_size_one_pass_cbr(cpi);
    } else if (cpi->oxcf.rc_mode == VPX_VBR) {
      if (cm->frame_type == KEY_FRAME) {
        cpi->rc.this_key_frame_forced = cm->current_video_frame != 0;
        cpi->rc.frames_to_key = cpi->oxcf.key_freq;
      }
      vp9_set_gf_update_one_pass_vbr(cpi);
      if (cpi->oxcf.aq_mode == CYCLIC_REFRESH_AQ)
        vp9_cyclic_refresh_update_parameters(cpi);
      if (frame_is_intra_only(cm))
        target = vp9_calc_iframe_target_size_one_pass_vbr(cpi);
      else
        target = vp9_calc_pframe_target_size_one_pass_vbr(cpi);
    }
    vp9_rc_set_frame_target(cpi, target);
    vp9_update_buffer_level_preencode(cpi);
  } else {
    vp9_update_temporal_layer_framerate(cpi);
    vp9_restore_layer_context(cpi);
    vp9_rc_get_svc_params(cpi);
  }
  if (svc->spatial_layer_id == 0) vp9_zero(svc->drop_spatial_layer);
  // SVC: check for skip encoding of enhancement layer if the
  // layer target bandwidth = 0.
  if (vp9_svc_check_skip_enhancement_layer(cpi)) {
    trace->dropped = true;
    trace->qp = -1;
    trace->loopfilter_level = -1;
    trace->this_frame_target = cpi->rc.this_frame_target;
    trace->effective_frame_type = cm->frame_type;
    trace->decimation_factor = cpi->rc.decimation_factor;
    trace->decimation_count = cpi->rc.decimation_count;
    trace->svc_drop_count = svc->drop_count[svc->spatial_layer_id];
    return true;
  }
  trace->this_frame_target = cpi->rc.this_frame_target;
  trace->effective_frame_type = cm->frame_type;
  // Check for dropping this frame based on buffer level.
  // Never drop on key frame, or if base layer is key for svc.
  bool drop = false;
  if (!frame_is_intra_only(cm) &&
      (!cpi->use_svc ||
       !svc->layer_context[svc->temporal_layer_id].is_key_frame)) {
    drop = vp9_rc_drop_frame_decision(cpi) != 0;
  }
  // Freeze the side effects of the drop-decision machinery; they are part of
  // the decision, not of the adaptive bookkeeping.
  trace->decimation_factor = cpi->rc.decimation_factor;
  trace->decimation_count = cpi->rc.decimation_count;
  trace->svc_drop_count = svc->drop_count[svc->spatial_layer_id];
  if (drop) {
    vp9_rc_drop_frame_apply(cpi);
    // For FULL_SUPERFRAME_DROP mode (the only mode considered here):
    // if the superframe drop is decided we need to save the layer context for
    // all spatial layers, and call update_buffer_level and postencode_drop
    // for all spatial layers.
    if (use_svc) {
      vp9_save_layer_context(cpi);
      for (int sl = 1; sl < svc->number_spatial_layers; sl++) {
        svc->spatial_layer_id = sl;
        vp9_restore_layer_context(cpi);
        vp9_update_buffer_level_svc_preencode(cpi);
        vp9_rc_postencode_update_drop_frame(cpi);
        vp9_save_layer_context(cpi);
      }
    }
    trace->dropped = true;
    trace->qp = -1;
    trace->loopfilter_level = -1;
    return true;
  }
  // Compute the QP for the frame.
  int bottom_index = 0;
  int top_index = 0;
  cm->base_qindex = vp9_rc_pick_q_and_bounds(cpi, &bottom_index, &top_index);

  if (cpi->oxcf.aq_mode == CYCLIC_REFRESH_AQ) vp9_cyclic_refresh_setup(cpi);
  if (use_svc) vp9_save_layer_context(cpi);

  cpi->last_frame_dropped = 0;
  svc->last_layer_dropped[svc->spatial_layer_id] = 0;
  if (svc->spatial_layer_id == svc->number_spatial_layers - 1)
    svc->num_encoded_top_layer++;

  trace->dropped = false;
  trace->qp = cm->base_qindex;
  trace->loopfilter_level = -1;
  return false;
}

int Vp9RcPickLoopfilter(VP9_COMP *cpi) {
  struct loopfilter *const lf = &cpi->common.lf;
  vp9_pick_filter_level(nullptr, cpi, LPF_PICK_FROM_Q);
  return lf->filter_level;
}

// Applies the post-encode bookkeeping for an encoded frame with the given
// cost in bytes. Mirrors VP9RateControlRTC::PostEncodeUpdate() but takes the
// frame type and qp from the frozen decision.
void Vp9RcApplyEncodedCost(VP9_COMP *cpi, const Vp9RcPendingFrame &rec,
                           uint64_t cost_bytes) {
  VP9_COMMON *const cm = &cpi->common;
  SVC *const svc = &cpi->svc;
  cm->frame_type = static_cast<FRAME_TYPE>(rec.trace.effective_frame_type);
  svc->spatial_layer_id = rec.params.spatial_layer_id;
  svc->temporal_layer_id = rec.params.temporal_layer_id;
  if (UseSvc(cpi)) {
    vp9_restore_layer_context(cpi);
    const int layer = LAYER_IDS_TO_IDX(svc->spatial_layer_id,
                                       svc->temporal_layer_id,
                                       svc->number_temporal_layers);
    LAYER_CONTEXT *lc = &svc->layer_context[layer];
    cm->base_qindex = lc->frame_qp;
    cm->MBs = lc->MBs;
  }
  vp9_rc_postencode_update(cpi, cost_bytes);
  if (UseSvc(cpi)) vp9_save_layer_context(cpi);
  cm->current_video_frame++;
}

// Replays one pending record: re-applies the frozen decision (frame setup,
// buffer income, drop-decision side effects, drop bookkeeping or qp) and the
// post-encode bookkeeping with the record's current cost. The decisions are
// never re-selected; only the adaptive bookkeeping is recomputed.
void Vp9RcReplayRecord(VP9_COMP *cpi, const Vp9RcPendingFrame &rec) {
  VP9_COMMON *const cm = &cpi->common;
  SVC *const svc = &cpi->svc;
  const bool use_svc = UseSvc(cpi);
  svc->spatial_layer_id = rec.params.spatial_layer_id;
  svc->temporal_layer_id = rec.params.temporal_layer_id;
  cm->frame_type = static_cast<FRAME_TYPE>(rec.trace.effective_frame_type);
  cpi->frame_flags =
      (rec.params.frame_type == RcFrameType::kKeyFrame) ? FRAMEFLAGS_KEY : 0;
  cpi->refresh_golden_frame =
      (rec.params.frame_type == RcFrameType::kKeyFrame) ? 1 : 0;
  cpi->refresh_alt_ref_frame = 0;
  if (use_svc) {
    vp9_update_temporal_layer_framerate(cpi);
    vp9_restore_layer_context(cpi);
    vp9_rc_get_svc_params(cpi);
    // The frame target is a frozen decision: restore it (the call above
    // recomputed it from the current state).
    vp9_rc_set_frame_target(cpi, rec.trace.this_frame_target);
  } else {
    vp9_rc_set_frame_target(cpi, rec.trace.this_frame_target);
    vp9_update_buffer_level_preencode(cpi);
  }
  if (svc->spatial_layer_id == 0) vp9_zero(svc->drop_spatial_layer);
  // Restore the frozen side effects of the drop-decision machinery.
  cpi->rc.decimation_factor = rec.trace.decimation_factor;
  cpi->rc.decimation_count = rec.trace.decimation_count;
  svc->drop_count[svc->spatial_layer_id] = rec.trace.svc_drop_count;
  if (rec.trace.dropped) {
    vp9_rc_drop_frame_apply(cpi);
    if (use_svc) vp9_save_layer_context(cpi);
    return;
  }
  cm->base_qindex = rec.trace.qp;
  if (use_svc) vp9_save_layer_context(cpi);
  cpi->last_frame_dropped = 0;
  svc->last_layer_dropped[svc->spatial_layer_id] = 0;
  if (svc->spatial_layer_id == svc->number_spatial_layers - 1)
    svc->num_encoded_top_layer++;
  Vp9RcApplyEncodedCost(cpi, rec, rec.cost);
}

void Vp9RcSaveSnapshot(const VP9_COMP *cpi, Vp9RcSnapshot *snapshot) {
  const SVC *const svc = &cpi->svc;
  snapshot->rc = cpi->rc;
  for (int layer = 0; layer < VPX_MAX_LAYERS; ++layer) {
    const LAYER_CONTEXT &lc = svc->layer_context[layer];
    snapshot->layers[layer].rc = lc.rc;
    snapshot->layers[layer].target_bandwidth = lc.target_bandwidth;
    snapshot->layers[layer].framerate = lc.framerate;
    snapshot->layers[layer].avg_frame_size = lc.avg_frame_size;
    snapshot->layers[layer].current_video_frame_in_layer =
        lc.current_video_frame_in_layer;
    snapshot->layers[layer].is_key_frame = lc.is_key_frame;
    snapshot->layers[layer].frames_from_key_frame = lc.frames_from_key_frame;
    snapshot->layers[layer].frame_qp = lc.frame_qp;
    snapshot->layers[layer].MBs = lc.MBs;
  }
  snapshot->current_superframe = svc->current_superframe;
  snapshot->num_encoded_top_layer = svc->num_encoded_top_layer;
  for (int layer = 0; layer < VPX_MAX_LAYERS; ++layer) {
    snapshot->drop_count[layer] = svc->drop_count[layer];
    snapshot->last_layer_dropped[layer] = svc->last_layer_dropped[layer];
    snapshot->drop_spatial_layer[layer] = svc->drop_spatial_layer[layer];
  }
  snapshot->skip_enhancement_layer = svc->skip_enhancement_layer;
  snapshot->current_video_frame = cpi->common.current_video_frame;
  snapshot->frame_type = cpi->common.frame_type;
  snapshot->base_qindex = cpi->common.base_qindex;
  snapshot->last_frame_dropped = cpi->last_frame_dropped;
  snapshot->ext_refresh_frame_flags_pending =
      cpi->ext_refresh_frame_flags_pending;
  snapshot->deadline_mode_previous_frame = cpi->deadline_mode_previous_frame;
}

void Vp9RcRestoreSnapshot(VP9_COMP *cpi, const Vp9RcSnapshot &snapshot) {
  SVC *const svc = &cpi->svc;
  cpi->rc = snapshot.rc;
  for (int layer = 0; layer < VPX_MAX_LAYERS; ++layer) {
    LAYER_CONTEXT *const lc = &svc->layer_context[layer];
    lc->rc = snapshot.layers[layer].rc;
    lc->target_bandwidth = snapshot.layers[layer].target_bandwidth;
    lc->framerate = snapshot.layers[layer].framerate;
    lc->avg_frame_size = snapshot.layers[layer].avg_frame_size;
    lc->current_video_frame_in_layer =
        snapshot.layers[layer].current_video_frame_in_layer;
    lc->is_key_frame = snapshot.layers[layer].is_key_frame;
    lc->frames_from_key_frame = snapshot.layers[layer].frames_from_key_frame;
    lc->frame_qp = snapshot.layers[layer].frame_qp;
    lc->MBs = snapshot.layers[layer].MBs;
  }
  svc->current_superframe = snapshot.current_superframe;
  svc->num_encoded_top_layer = snapshot.num_encoded_top_layer;
  for (int layer = 0; layer < VPX_MAX_LAYERS; ++layer) {
    svc->drop_count[layer] = snapshot.drop_count[layer];
    svc->last_layer_dropped[layer] = snapshot.last_layer_dropped[layer];
    svc->drop_spatial_layer[layer] = snapshot.drop_spatial_layer[layer];
  }
  svc->skip_enhancement_layer = snapshot.skip_enhancement_layer;
  cpi->common.current_video_frame = snapshot.current_video_frame;
  cpi->common.frame_type = static_cast<FRAME_TYPE>(snapshot.frame_type);
  cpi->common.base_qindex = snapshot.base_qindex;
  cpi->last_frame_dropped = snapshot.last_frame_dropped;
  cpi->ext_refresh_frame_flags_pending =
      snapshot.ext_refresh_frame_flags_pending;
  cpi->deadline_mode_previous_frame =
      static_cast<MODE>(snapshot.deadline_mode_previous_frame);
}

void Vp9RcFoldPrefix(VP9_COMP *cpi, VP9RcPipelineState *pipe) {
  while (!pipe->records.empty() && pipe->records.front().size_known) {
    Vp9RcRestoreSnapshot(cpi, pipe->snapshot);
    Vp9RcReplayRecord(cpi, pipe->records.front());
    Vp9RcSaveSnapshot(cpi, &pipe->snapshot);
    pipe->records.pop_front();
  }
}

void Vp9RcObserveCpi(const VP9_COMP *cpi, VP9RcObservation *obs) {
  const SVC *const svc = &cpi->svc;
  obs->rc = cpi->rc;
  for (int layer = 0; layer < VPX_MAX_LAYERS; ++layer) {
    obs->layer_rc[layer] = svc->layer_context[layer].rc;
    obs->current_video_frame_in_layer[layer] =
        svc->layer_context[layer].current_video_frame_in_layer;
    obs->frames_from_key_frame[layer] =
        svc->layer_context[layer].frames_from_key_frame;
    obs->is_key_frame[layer] = svc->layer_context[layer].is_key_frame;
    obs->drop_count[layer] = svc->drop_count[layer];
  }
  obs->current_video_frame = cpi->common.current_video_frame;
  obs->current_superframe = svc->current_superframe;
  obs->num_encoded_top_layer = svc->num_encoded_top_layer;
}

PipelineStatus VP9RateControlRTC::PrepareFrame(
    const VP9FrameParamsQpRTC &frame_params, uint64_t estimated_bytes,
    VP9RcFrameDecision *decision) {
  if (!pipeline_mode_ || !rc_is_valid_ || decision == nullptr)
    return PipelineStatus::kInvalid;
  if (frame_params.spatial_layer_id != 0 ||
      frame_params.temporal_layer_id < 0 ||
      frame_params.temporal_layer_id >= cpi_->svc.number_temporal_layers ||
      (frame_params.frame_type != RcFrameType::kKeyFrame &&
       frame_params.frame_type != RcFrameType::kInterFrame)) {
    return PipelineStatus::kInvalid;
  }
  VP9RcPipelineState &pipe = *pipeline_;
  // Capacity is the number of records not yet contiguously confirmed and
  // reclaimed (confirmed records behind a hole still count).
  if (static_cast<int>(pipe.records.size()) >= max_pending_frames_)
    return PipelineStatus::kFull;
  const uint64_t estimate = ClampFrameBytes(estimated_bytes);
  // Rebuild the current state: the snapshot of the reclaimed prefix plus all
  // pending records replayed in issue order with their current costs, so
  // this frame sees every cost known so far.
  Vp9RcRestoreSnapshot(cpi_, pipe.snapshot);
  for (const Vp9RcPendingFrame &pending : pipe.records)
    Vp9RcReplayRecord(cpi_, pending);
  Vp9RcPendingFrame rec;
  rec.frame_id = pipe.next_frame_id;
  rec.params = frame_params;
  rec.estimate = estimate;
  if (Vp9RcDecideFrame(cpi_, frame_params, &rec.trace)) {
    // Dropped frame: the estimate is ignored, the cost is fixed to 0 and no
    // size submission is expected.
    rec.cost = 0;
    rec.size_known = true;
  } else {
    rec.trace.loopfilter_level = Vp9RcPickLoopfilter(cpi_);
    rec.cost = estimate;
    // Account for the new frame immediately with the estimated size, so the
    // next frame can be prepared without waiting for feedback.
    Vp9RcApplyEncodedCost(cpi_, rec, estimate);
  }
  pipe.records.push_back(rec);
  ++pipe.next_frame_id;
  // Fold any newly confirmed prefix (e.g. this frame if it was dropped).
  Vp9RcFoldPrefix(cpi_, &pipe);
  decision->frame_id = rec.frame_id;
  decision->dropped = rec.trace.dropped;
  decision->qp = rec.trace.qp;
  decision->loopfilter_level = rec.trace.loopfilter_level;
  return PipelineStatus::kOk;
}

PipelineStatus VP9RateControlRTC::SubmitFrameSize(uint64_t frame_id,
                                                  uint64_t actual_bytes) {
  if (!pipeline_mode_ || !rc_is_valid_) return PipelineStatus::kInvalid;
  const uint64_t cost = ClampFrameBytes(actual_bytes);
  VP9RcPipelineState &pipe = *pipeline_;
  for (Vp9RcPendingFrame &rec : pipe.records) {
    if (rec.frame_id != frame_id) continue;
    if (rec.trace.dropped) return PipelineStatus::kInvalid;
    if (rec.size_known) {
      // Repeated submission with the same size succeeds without side
      // effects; a different size is rejected.
      return rec.cost == cost ? PipelineStatus::kOk
                              : PipelineStatus::kInvalid;
    }
    rec.cost = cost;
    rec.size_known = true;
    // Immediately fold the longest contiguous confirmed prefix into the
    // snapshot and reclaim those records.
    Vp9RcFoldPrefix(cpi_, &pipe);
    return PipelineStatus::kOk;
  }
  return PipelineStatus::kInvalid;
}

}  // namespace libvpx
