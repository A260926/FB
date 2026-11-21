// Tests for the pipelined VP9 RTC rate control interface
// (VP9RateControlRTC::PrepareFrame/SubmitFrameSize). Registered through
// task_support/check.h; no main() here.

#include "task_support/check.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

// Public codec headers must come before the internal encoder headers:
// vpx_codec_internal.h redefines VPX_CTRL_USE_TYPE for the codec
// implementation, which would otherwise hide the type-safe
// vpx_codec_control() wrappers.
#include "vpx/vp8cx.h"
#include "vpx/vpx_encoder.h"

#include "vp9/ratectrl_rtc.h"
#include "vp9/ratectrl_rtc_pipeline.h"
#include "vp9/encoder/vp9_encoder.h"
#include "vp9/encoder/vp9_svc_layercontext.h"

namespace {

using libvpx::FrameDropDecision;
using libvpx::PipelineStatus;
using libvpx::RcFrameType;
using libvpx::VP9FrameParamsQpRTC;
using libvpx::VP9RateControlRTC;
using libvpx::VP9RateControlRtcConfig;
using libvpx::VP9RcFrameDecision;
using libvpx::VP9RcObservation;

// ---------------------------------------------------------------------------
// Config helpers
// ---------------------------------------------------------------------------

VP9RateControlRtcConfig MakeCfg(int ts_layers, int max_pending,
                                int frame_drop_thresh) {
  VP9RateControlRtcConfig cfg;
  cfg.width = 96;
  cfg.height = 64;
  cfg.target_bandwidth = 80;
  cfg.framerate = 30;
  cfg.min_quantizer = 2;
  cfg.max_quantizer = 56;
  cfg.max_intra_bitrate_pct = 1000;
  cfg.aq_mode = 0;
  cfg.frame_drop_thresh = frame_drop_thresh;
  cfg.ts_number_layers = ts_layers;
  cfg.layer_target_bitrate[0] = ts_layers == 2 ? 48 : 80;
  cfg.ts_rate_decimator[0] = ts_layers == 2 ? 2 : 1;
  cfg.layer_target_bitrate[1] = 80;
  cfg.ts_rate_decimator[1] = 1;
  for (int i = 0; i < ts_layers; ++i) {
    cfg.min_quantizers[i] = 2;
    cfg.max_quantizers[i] = 56;
  }
  cfg.max_pending_frames = max_pending;
  return cfg;
}

VP9RateControlRtcConfig SyncCfg(VP9RateControlRtcConfig cfg) {
  cfg.max_pending_frames = 0;
  return cfg;
}

VP9FrameParamsQpRTC MakeParams(int sl, int tl, bool key) {
  VP9FrameParamsQpRTC p;
  p.spatial_layer_id = sl;
  p.temporal_layer_id = tl;
  p.frame_type = key ? RcFrameType::kKeyFrame : RcFrameType::kInterFrame;
  return p;
}

// ---------------------------------------------------------------------------
// Independent fixed-decision serial reference. Drives a VP9_COMP created by
// VP9RcCpiCreate() through the original scalar bookkeeping core (income,
// drop bookkeeping, post-encode update) with frozen decisions. It neither
// calls the pipelined queue (PrepareFrame/SubmitFrameSize) nor reuses its
// replay loop.
// ---------------------------------------------------------------------------

void RefApplyFrame(VP9_COMP *cpi, int ts_layers, RcFrameType ftype, int tl,
                   bool dropped, int qp, uint64_t cost) {
  VP9_COMMON *const cm = &cpi->common;
  SVC *const svc = &cpi->svc;
  const bool use_svc = ts_layers > 1;
  svc->spatial_layer_id = 0;
  svc->temporal_layer_id = tl;
  cm->frame_type = (ftype == RcFrameType::kKeyFrame) ? KEY_FRAME : INTER_FRAME;
  cpi->frame_flags = (cm->frame_type == KEY_FRAME) ? FRAMEFLAGS_KEY : 0;
  cpi->refresh_golden_frame = (cm->frame_type == KEY_FRAME) ? 1 : 0;
  if (use_svc) {
    vp9_update_temporal_layer_framerate(cpi);
    vp9_restore_layer_context(cpi);
    vp9_rc_get_svc_params(cpi);
  } else {
    const int target = (cm->frame_type == KEY_FRAME)
                           ? vp9_calc_iframe_target_size_one_pass_cbr(cpi)
                           : vp9_calc_pframe_target_size_one_pass_cbr(cpi);
    vp9_rc_set_frame_target(cpi, target);
    vp9_update_buffer_level_preencode(cpi);
  }
  if (svc->spatial_layer_id == 0) vp9_zero(svc->drop_spatial_layer);
  if (dropped) {
    vp9_rc_drop_frame_apply(cpi);
    if (use_svc) vp9_save_layer_context(cpi);
    return;
  }
  cm->base_qindex = qp;
  if (use_svc) vp9_save_layer_context(cpi);
  cpi->last_frame_dropped = 0;
  svc->last_layer_dropped[0] = 0;
  if (svc->spatial_layer_id == svc->number_spatial_layers - 1)
    svc->num_encoded_top_layer++;
  if (use_svc) {
    vp9_restore_layer_context(cpi);
    LAYER_CONTEXT *const lc =
        &svc->layer_context[LAYER_IDS_TO_IDX(0, tl, ts_layers)];
    cm->base_qindex = lc->frame_qp;
    cm->MBs = lc->MBs;
  }
  vp9_rc_postencode_update(cpi, cost);
  if (use_svc) vp9_save_layer_context(cpi);
  cm->current_video_frame++;
}

struct RefFrame {
  VP9FrameParamsQpRTC params;
  bool dropped;
  int qp;
  uint64_t cost;  // Cost to bookkeep: actual size, or 0 for drops.
};

// Runs the serial reference over |frames| and returns the resulting state.
bool RunSerialReference(const VP9RateControlRtcConfig &cfg,
                        const std::vector<RefFrame> &frames,
                        VP9RcObservation *obs) {
  VP9_COMP *cpi = libvpx::VP9RcCpiCreate(cfg);
  if (cpi == NULL) return false;
  for (size_t i = 0; i < frames.size(); ++i) {
    RefApplyFrame(cpi, cfg.ts_number_layers, frames[i].params.frame_type,
                  frames[i].params.temporal_layer_id, frames[i].dropped,
                  frames[i].qp, frames[i].cost);
  }
  libvpx::Vp9RcObserveCpi(cpi, obs);
  libvpx::VP9RcCpiDestroy(cpi);
  return true;
}

// Builds a RefFrame vector from pipeline decisions and per-frame costs.
std::vector<RefFrame> MakeRefFrames(const std::vector<VP9FrameParamsQpRTC> &params,
                                    const std::vector<VP9RcFrameDecision> &dec,
                                    const std::vector<uint64_t> &cost) {
  std::vector<RefFrame> frames;
  for (size_t i = 0; i < params.size(); ++i) {
    RefFrame f;
    f.params = params[i];
    f.dropped = dec[i].dropped;
    f.qp = dec[i].qp;
    f.cost = dec[i].dropped ? 0 : cost[i];
    frames.push_back(f);
  }
  return frames;
}

// ---------------------------------------------------------------------------
// Observation comparison: global and per-layer buffer levels, rate
// correction factors, QP history and frame counters.
// ---------------------------------------------------------------------------

bool RcCoreEq(const RATE_CONTROL &a, const RATE_CONTROL &b) {
  static const char *names[] = { "INTER_NORMAL", "INTER_HIGH", "GF_ARF_LOW",
                                 "GF_ARF_STD", "KF_STD" };
  for (int i = 0; i < RATE_FACTOR_LEVELS; ++i) {
    if (a.rate_correction_factors[i] != b.rate_correction_factors[i]) {
      std::fprintf(stderr, "rcf[%s] %.9f vs %.9f\n", names[i],
                   a.rate_correction_factors[i], b.rate_correction_factors[i]);
      return false;
    }
    if (a.damped_adjustment[i] != b.damped_adjustment[i]) return false;
  }
  return a.buffer_level == b.buffer_level &&
         a.bits_off_target == b.bits_off_target &&
         a.last_q[0] == b.last_q[0] && a.last_q[1] == b.last_q[1] &&
         a.avg_frame_qindex[0] == b.avg_frame_qindex[0] &&
         a.avg_frame_qindex[1] == b.avg_frame_qindex[1] &&
         a.ni_av_qi == b.ni_av_qi && a.ni_tot_qi == b.ni_tot_qi &&
         a.ni_frames == b.ni_frames && a.tot_q == b.tot_q &&
         a.avg_q == b.avg_q &&
         a.last_boosted_qindex == b.last_boosted_qindex &&
         a.last_kf_qindex == b.last_kf_qindex &&
         a.q_1_frame == b.q_1_frame && a.q_2_frame == b.q_2_frame &&
         a.rc_1_frame == b.rc_1_frame && a.rc_2_frame == b.rc_2_frame &&
         a.frames_since_key == b.frames_since_key &&
         a.frames_to_key == b.frames_to_key &&
         a.frames_since_golden == b.frames_since_golden &&
         a.frames_till_gf_update_due == b.frames_till_gf_update_due &&
         a.total_actual_bits == b.total_actual_bits &&
         a.total_target_bits == b.total_target_bits &&
         a.total_target_vs_actual == b.total_target_vs_actual &&
         a.rolling_actual_bits == b.rolling_actual_bits &&
         a.long_rolling_actual_bits == b.long_rolling_actual_bits &&
         a.avg_frame_bandwidth == b.avg_frame_bandwidth &&
         a.min_frame_bandwidth == b.min_frame_bandwidth &&
         a.max_frame_bandwidth == b.max_frame_bandwidth &&
         a.last_avg_frame_bandwidth == b.last_avg_frame_bandwidth &&
         a.projected_frame_size == b.projected_frame_size;
}

bool ObsEq(const VP9RcObservation &a, const VP9RcObservation &b,
           int ts_layers) {
  if (!RcCoreEq(a.rc, b.rc)) {
    std::fprintf(stderr,
                 "global rc mismatch: buffer %lld/%lld bits_off %lld/%lld "
                 "last_q %d/%d vs %d/%d cvf n/a\n",
                 (long long)a.rc.buffer_level, (long long)b.rc.buffer_level,
                 (long long)a.rc.bits_off_target,
                 (long long)b.rc.bits_off_target, a.rc.last_q[0],
                 a.rc.last_q[1], b.rc.last_q[0], b.rc.last_q[1]);
    return false;
  }
  for (int l = 0; l < ts_layers; ++l) {
    if (!RcCoreEq(a.layer_rc[l], b.layer_rc[l])) {
      std::fprintf(stderr, "layer %d rc mismatch: buffer %lld/%lld\n", l,
                   (long long)a.layer_rc[l].buffer_level,
                   (long long)b.layer_rc[l].buffer_level);
      return false;
    }
    if (a.current_video_frame_in_layer[l] != b.current_video_frame_in_layer[l] ||
        a.frames_from_key_frame[l] != b.frames_from_key_frame[l] ||
        a.is_key_frame[l] != b.is_key_frame[l] ||
        a.drop_count[l] != b.drop_count[l])
      return false;
  }
  return a.current_video_frame == b.current_video_frame &&
         a.current_superframe == b.current_superframe &&
         a.num_encoded_top_layer == b.num_encoded_top_layer;
}

// ---------------------------------------------------------------------------
// Scenario driver: prepares frames with estimates, then submits actual sizes
// in the given order. Returns false on unexpected API failure.
// ---------------------------------------------------------------------------

bool DrivePipeline(VP9RateControlRTC *rc,
                   const std::vector<VP9FrameParamsQpRTC> &params,
                   const std::vector<uint64_t> &est,
                   const std::vector<uint64_t> &actual,
                   const std::vector<int> &submit_order,
                   std::vector<VP9RcFrameDecision> *dec) {
  const size_t n = params.size();
  dec->assign(n, VP9RcFrameDecision());
  for (size_t f = 0; f < n; ++f) {
    if (rc->PrepareFrame(params[f], est[f], &(*dec)[f]) != PipelineStatus::kOk)
      return false;
  }
  for (size_t k = 0; k < submit_order.size(); ++k) {
    const int f = submit_order[k];
    if ((*dec)[f].dropped) return false;  // scenario must not produce drops
    if (rc->SubmitFrameSize((*dec)[f].frame_id, actual[f]) !=
        PipelineStatus::kOk)
      return false;
  }
  return true;
}

std::vector<int> ForwardOrder(int n) {
  std::vector<int> v;
  for (int i = 0; i < n; ++i) v.push_back(i);
  return v;
}

std::vector<int> ReverseOrder(int n) {
  std::vector<int> v;
  for (int i = n - 1; i >= 0; --i) v.push_back(i);
  return v;
}

std::vector<int> OddEvenOrder(int n) {
  std::vector<int> v;
  for (int i = 1; i < n; i += 2) v.push_back(i);
  for (int i = 0; i < n; i += 2) v.push_back(i);
  return v;
}

// ---------------------------------------------------------------------------
// replay group
// ---------------------------------------------------------------------------

// Same issued records and same known size set: the order in which feedback
// arrives must not change the resulting state or the next frame's QP.
int ReplayFeedbackOrderInvariance() {
  const int kN = 8;
  VP9RateControlRtcConfig cfg = MakeCfg(1, 8, 0);
  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<uint64_t> est, actual;
  for (int f = 0; f < kN; ++f) {
    params.push_back(MakeParams(0, 0, f == 0));
    est.push_back(600 + 91 * f);
    actual.push_back(300 + 233 * f);
  }
  const std::vector<int> orders[3] = { ForwardOrder(kN), ReverseOrder(kN),
                                       OddEvenOrder(kN) };
  VP9RcObservation obs[3];
  int next_qp[3] = { 0, 0, 0 };
  int next_lf[3] = { 0, 0, 0 };
  for (int k = 0; k < 3; ++k) {
    std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
    TASK_CHECK(rc != NULL);
    std::vector<VP9RcFrameDecision> dec;
    TASK_CHECK(DrivePipeline(rc.get(), params, est, actual, orders[k], &dec));
    rc->GetRcObservation(&obs[k]);
    // One more frame prepared after all feedback arrived.
    VP9RcFrameDecision next;
    TASK_CHECK(rc->PrepareFrame(MakeParams(0, 0, false), 500, &next) ==
               PipelineStatus::kOk);
    TASK_CHECK(!next.dropped);
    next_qp[k] = next.qp;
    next_lf[k] = next.loopfilter_level;
  }
  TASK_CHECK(ObsEq(obs[0], obs[1], 1));
  TASK_CHECK(ObsEq(obs[0], obs[2], 1));
  TASK_CHECK(next_qp[0] == next_qp[1]);
  TASK_CHECK(next_qp[0] == next_qp[2]);
  TASK_CHECK(next_lf[0] == next_lf[1]);
  TASK_CHECK(next_lf[0] == next_lf[2]);
  // And the state matches the independent serial reference.
  std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc != NULL);
  std::vector<VP9RcFrameDecision> dec;
  TASK_CHECK(DrivePipeline(rc.get(), params, est, actual, ReverseOrder(kN),
                           &dec));
  VP9RcObservation ref;
  TASK_CHECK(RunSerialReference(cfg, MakeRefFrames(params, dec, actual), &ref));
  TASK_CHECK(ObsEq(obs[0], ref, 1));
  return 0;
}

// Decisions made with estimates must match a synchronous run fed the same
// estimates; after out-of-order feedback the state must match the serial
// reference with actual costs.
int ReplayOutOfOrderReverse() {
  const int kN = 6;
  VP9RateControlRtcConfig cfg = MakeCfg(1, 8, 0);
  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<uint64_t> est, actual;
  for (int f = 0; f < kN; ++f) {
    params.push_back(MakeParams(0, 0, f == 0));
    est.push_back(700 + 131 * f);
    actual.push_back(400 + 257 * f);
  }
  std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc != NULL);
  std::vector<VP9RcFrameDecision> dec;
  TASK_CHECK(DrivePipeline(rc.get(), params, est, actual, ReverseOrder(kN),
                           &dec));
  // Synchronous interface fed with the same estimates picks the same QP/CF.
  {
    std::unique_ptr<VP9RateControlRTC> sync =
        VP9RateControlRTC::Create(SyncCfg(cfg));
    TASK_CHECK(sync != NULL);
    for (int f = 0; f < kN; ++f) {
      TASK_CHECK(sync->ComputeQP(params[f]) == FrameDropDecision::kOk);
      TASK_CHECK(sync->GetQP() == dec[f].qp);
      TASK_CHECK(sync->GetLoopfilterLevel() == dec[f].loopfilter_level);
      sync->PostEncodeUpdate(est[f], params[f]);
    }
  }
  VP9RcObservation obs, ref;
  rc->GetRcObservation(&obs);
  TASK_CHECK(RunSerialReference(cfg, MakeRefFrames(params, dec, actual), &ref));
  TASK_CHECK(ObsEq(obs, ref, 1));
  TASK_CHECK(obs.current_video_frame == (unsigned int)kN);
  return 0;
}

// A hole in the confirmation sequence: a large frame confirmed behind the
// hole must be visible to the next PrepareFrame even before the hole is
// filled; once the hole is filled the whole prefix folds.
int ReplayHoleThenBigFrame() {
  VP9RateControlRtcConfig cfg = MakeCfg(1, 8, 0);
  std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc != NULL);
  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<VP9RcFrameDecision> dec;
  std::vector<uint64_t> est, actual;
  for (int f = 0; f < 5; ++f) {
    params.push_back(MakeParams(0, 0, f == 0));
    est.push_back(500);
    actual.push_back(600 + 100 * f);
    dec.push_back(VP9RcFrameDecision());
    TASK_CHECK(rc->PrepareFrame(params[f], est[f], &dec[f]) ==
               PipelineStatus::kOk);
  }
  // f1 stays unconfirmed (the hole); confirm f0, then a big f2, then f3, f4.
  actual[2] = 9000;
  actual[1] = 12000;
  TASK_CHECK(rc->SubmitFrameSize(dec[0].frame_id, actual[0]) ==
             PipelineStatus::kOk);
  TASK_CHECK(rc->SubmitFrameSize(dec[2].frame_id, actual[2]) ==
             PipelineStatus::kOk);
  TASK_CHECK(rc->SubmitFrameSize(dec[3].frame_id, actual[3]) ==
             PipelineStatus::kOk);
  TASK_CHECK(rc->SubmitFrameSize(dec[4].frame_id, actual[4]) ==
             PipelineStatus::kOk);
  // Prepare one more frame: it must see f1's estimate and f2..f4 actuals.
  params.push_back(MakeParams(0, 0, false));
  est.push_back(500);
  actual.push_back(700);
  dec.push_back(VP9RcFrameDecision());
  TASK_CHECK(rc->PrepareFrame(params[5], est[5], &dec[5]) ==
             PipelineStatus::kOk);
  {
    VP9RcObservation obs, ref;
    rc->GetRcObservation(&obs);
    std::vector<uint64_t> known = actual;
    known[1] = est[1];  // f1 still at its estimate
    known[5] = est[5];  // f5 just prepared with its estimate
    TASK_CHECK(RunSerialReference(cfg, MakeRefFrames(params, dec, known),
                                  &ref));
    TASK_CHECK(ObsEq(obs, ref, 1));
  }
  // Fill the hole with a big actual size: the whole prefix folds.
  TASK_CHECK(rc->SubmitFrameSize(dec[1].frame_id, actual[1]) ==
             PipelineStatus::kOk);
  params.push_back(MakeParams(0, 0, false));
  est.push_back(500);
  actual.push_back(650);
  dec.push_back(VP9RcFrameDecision());
  TASK_CHECK(rc->PrepareFrame(params[6], est[6], &dec[6]) ==
             PipelineStatus::kOk);
  {
    VP9RcObservation obs, ref;
    rc->GetRcObservation(&obs);
    std::vector<uint64_t> known = actual;
    known[5] = est[5];
    known[6] = est[6];
    TASK_CHECK(RunSerialReference(cfg, MakeRefFrames(params, dec, known),
                                  &ref));
    TASK_CHECK(ObsEq(obs, ref, 1));
  }
  return 0;
}

// Over- and under-estimates: once the actual sizes arrive, each run's state
// must equal the serial reference with that run's frozen decisions and the
// actual costs (the estimates only live on in the frozen decisions).
int ReplayEstimateOverUnder() {
  const int kN = 7;
  VP9RateControlRtcConfig cfg = MakeCfg(1, 8, 0);
  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<uint64_t> actual;
  for (int f = 0; f < kN; ++f) {
    params.push_back(MakeParams(0, 0, f % 4 == 0));
    actual.push_back(800 + 157 * f);
  }
  VP9RcObservation obs[2];
  int qp_sum[2] = { 0, 0 };
  for (int k = 0; k < 2; ++k) {
    std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
    TASK_CHECK(rc != NULL);
    std::vector<VP9RcFrameDecision> dec;
    std::vector<uint64_t> est;
    for (int f = 0; f < kN; ++f)
      est.push_back(k == 0 ? 3 * actual[f] : (actual[f] / 3 + 1));
    // Interleave prepares and submits differently in the two runs.
    int submitted = 0;
    for (int f = 0; f < kN; ++f) {
      dec.push_back(VP9RcFrameDecision());
      TASK_CHECK(rc->PrepareFrame(params[f], est[f], &dec[f]) ==
                 PipelineStatus::kOk);
      qp_sum[k] += dec[f].qp;
      if (k == 1 && f >= 2) {
        TASK_CHECK(rc->SubmitFrameSize(dec[f - 2].frame_id, actual[f - 2]) ==
                   PipelineStatus::kOk);
        submitted = f - 1;
      }
    }
    for (int f = submitted; f < kN; ++f) {
      TASK_CHECK(rc->SubmitFrameSize(dec[f].frame_id, actual[f]) ==
                 PipelineStatus::kOk);
    }
    rc->GetRcObservation(&obs[k]);
    VP9RcObservation ref;
    TASK_CHECK(
        RunSerialReference(cfg, MakeRefFrames(params, dec, actual), &ref));
    TASK_CHECK(ObsEq(obs[k], ref, 1));
  }
  // The different estimates did steer the decisions.
  TASK_CHECK(qp_sum[0] != qp_sum[1]);
  return 0;
}

// Drops with delayed feedback: an oversized first frame pushes the buffer
// negative, so subsequent frames are dropped. The frozen drop decisions and
// their bookkeeping must replay exactly with the actual costs.
int ReplayDropDelayedFeedback() {
  VP9RateControlRtcConfig cfg = MakeCfg(1, 8, 50);
  std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc != NULL);
  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<VP9RcFrameDecision> dec;
  std::vector<uint64_t> actual;
  params.push_back(MakeParams(0, 0, true));
  actual.push_back(9000);
  dec.push_back(VP9RcFrameDecision());
  TASK_CHECK(rc->PrepareFrame(params[0], 400, &dec[0]) == PipelineStatus::kOk);
  TASK_CHECK(!dec[0].dropped);
  TASK_CHECK(rc->SubmitFrameSize(dec[0].frame_id, actual[0]) ==
             PipelineStatus::kOk);
  int drops = 0;
  for (int f = 1; f < 10; ++f) {
    params.push_back(MakeParams(0, 0, false));
    actual.push_back(400 + 25 * f);
    dec.push_back(VP9RcFrameDecision());
    TASK_CHECK(rc->PrepareFrame(params[f], 400, &dec[f]) ==
               PipelineStatus::kOk);
    if (dec[f].dropped) {
      ++drops;
      TASK_CHECK(dec[f].qp == -1);
      TASK_CHECK(dec[f].loopfilter_level == -1);
      // Dropped frames need no submission and reject one.
      TASK_CHECK(rc->SubmitFrameSize(dec[f].frame_id, 400) ==
                 PipelineStatus::kInvalid);
    } else {
      TASK_CHECK(rc->SubmitFrameSize(dec[f].frame_id, actual[f]) ==
                 PipelineStatus::kOk);
    }
  }
  TASK_CHECK(drops > 0);
  VP9RcObservation obs, ref;
  rc->GetRcObservation(&obs);
  TASK_CHECK(RunSerialReference(cfg, MakeRefFrames(params, dec, actual), &ref));
  TASK_CHECK(ObsEq(obs, ref, 1));
  return 0;
}

// ---------------------------------------------------------------------------
// layers group
// ---------------------------------------------------------------------------

// Two temporal layers with alternating layer ids and reverse-order feedback.
int LayersTwoTemporalReverse() {
  const int kN = 8;
  VP9RateControlRtcConfig cfg = MakeCfg(2, 8, 0);
  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<uint64_t> est, actual;
  for (int f = 0; f < kN; ++f) {
    params.push_back(MakeParams(0, f % 2, f == 0));
    est.push_back(500 + 67 * f);
    actual.push_back(450 + 193 * f);
  }
  std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc != NULL);
  std::vector<VP9RcFrameDecision> dec;
  TASK_CHECK(DrivePipeline(rc.get(), params, est, actual, ReverseOrder(kN),
                           &dec));
  VP9RcObservation obs, ref;
  rc->GetRcObservation(&obs);
  TASK_CHECK(RunSerialReference(cfg, MakeRefFrames(params, dec, actual), &ref));
  TASK_CHECK(ObsEq(obs, ref, 2));
  // Per-layer buffers must have moved and must match the reference.
  TASK_CHECK(obs.layer_rc[0].bits_off_target == ref.layer_rc[0].bits_off_target);
  TASK_CHECK(obs.layer_rc[1].bits_off_target == ref.layer_rc[1].bits_off_target);
  return 0;
}

// Two temporal layers with a mid-stream key frame and interleaved feedback.
int LayersTwoTemporalKeyframe() {
  const int kN = 12;
  VP9RateControlRtcConfig cfg = MakeCfg(2, 3, 0);
  std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc != NULL);
  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<VP9RcFrameDecision> dec;
  std::vector<uint64_t> est, actual;
  std::vector<uint64_t> pending_id;
  std::vector<int> pending_idx;
  for (int f = 0; f < kN; ++f) {
    const bool key = (f == 0 || f == 6);
    params.push_back(MakeParams(0, key ? 0 : f % 2, key));
    est.push_back(550 + 43 * f);
    actual.push_back(500 + 117 * f);
    dec.push_back(VP9RcFrameDecision());
    // Keep at most 2 frames outstanding (capacity is 3).
    while ((int)pending_id.size() >= 2) {
      TASK_CHECK(rc->SubmitFrameSize(pending_id.front(),
                                     actual[pending_idx.front()]) ==
                 PipelineStatus::kOk);
      pending_id.erase(pending_id.begin());
      pending_idx.erase(pending_idx.begin());
    }
    TASK_CHECK(rc->PrepareFrame(params[f], est[f], &dec[f]) ==
               PipelineStatus::kOk);
    TASK_CHECK(!dec[f].dropped);
    pending_id.push_back(dec[f].frame_id);
    pending_idx.push_back(f);
  }
  while (!pending_id.empty()) {
    TASK_CHECK(
        rc->SubmitFrameSize(pending_id.front(), actual[pending_idx.front()]) ==
        PipelineStatus::kOk);
    pending_id.erase(pending_id.begin());
    pending_idx.erase(pending_idx.begin());
  }
  VP9RcObservation obs, ref;
  rc->GetRcObservation(&obs);
  TASK_CHECK(RunSerialReference(cfg, MakeRefFrames(params, dec, actual), &ref));
  TASK_CHECK(ObsEq(obs, ref, 2));
  TASK_CHECK(obs.current_video_frame == (unsigned int)kN);
  return 0;
}

// Many consecutive frames prepared in the same (single) layer before any
// feedback arrives.
int LayersSingleConsecutive() {
  VP9RateControlRtcConfig cfg = MakeCfg(1, 8, 0);
  std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc != NULL);
  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<VP9RcFrameDecision> dec;
  std::vector<uint64_t> est, actual;
  // Eight consecutive prepares in layer 0 fill the capacity exactly.
  for (int f = 0; f < 8; ++f) {
    params.push_back(MakeParams(0, 0, f == 0));
    est.push_back(450 + 29 * f);
    actual.push_back(500 + 61 * f);
    dec.push_back(VP9RcFrameDecision());
    TASK_CHECK(rc->PrepareFrame(params[f], est[f], &dec[f]) ==
               PipelineStatus::kOk);
  }
  // The ninth would exceed the capacity.
  {
    VP9RcFrameDecision blocked;
    TASK_CHECK(rc->PrepareFrame(MakeParams(0, 0, false), 500, &blocked) ==
               PipelineStatus::kFull);
  }
  for (int f = 0; f < 8; ++f)
    TASK_CHECK(rc->SubmitFrameSize(dec[f].frame_id, actual[f]) ==
               PipelineStatus::kOk);
  // Another batch of consecutive frames in the same layer.
  for (int f = 8; f < 12; ++f) {
    params.push_back(MakeParams(0, 0, false));
    est.push_back(450 + 29 * f);
    actual.push_back(500 + 61 * f);
    dec.push_back(VP9RcFrameDecision());
    TASK_CHECK(rc->PrepareFrame(params[f], est[f], &dec[f]) ==
               PipelineStatus::kOk);
    TASK_CHECK(rc->SubmitFrameSize(dec[f].frame_id, actual[f]) ==
               PipelineStatus::kOk);
  }
  VP9RcObservation obs, ref;
  rc->GetRcObservation(&obs);
  TASK_CHECK(RunSerialReference(cfg, MakeRefFrames(params, dec, actual), &ref));
  TASK_CHECK(ObsEq(obs, ref, 1));
  return 0;
}

// Rate-control-triggered frame drops on two temporal layers: dropped frames
// get qp/loopfilter of -1, need no size submission, and their bookkeeping is
// still applied exactly once.
int LayersDropTwoTemporal() {
  VP9RateControlRtcConfig cfg = MakeCfg(2, 4, 50);
  std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc != NULL);
  std::unique_ptr<VP9RateControlRTC> sync =
      VP9RateControlRTC::Create(SyncCfg(cfg));
  TASK_CHECK(sync != NULL);
  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<VP9RcFrameDecision> dec;
  std::vector<uint64_t> actual;
  int drops = 0;
  // Immediate feedback of oversized frames forces the buffer negative and
  // triggers drops. The synchronous interface must agree on every decision.
  for (int f = 0; f < 10; ++f) {
    params.push_back(MakeParams(0, f % 2, f == 0));
    actual.push_back(f == 0 ? 3000 : 60000);
    dec.push_back(VP9RcFrameDecision());
    TASK_CHECK(rc->PrepareFrame(params[f], 500, &dec[f]) ==
               PipelineStatus::kOk);
    const FrameDropDecision sync_dec = sync->ComputeQP(params[f]);
    if (dec[f].dropped) {
      ++drops;
      TASK_CHECK(dec[f].qp == -1);
      TASK_CHECK(dec[f].loopfilter_level == -1);
      TASK_CHECK(sync_dec == FrameDropDecision::kDrop);
      // A dropped frame needs no size submission; submitting one fails.
      TASK_CHECK(rc->SubmitFrameSize(dec[f].frame_id, 1000) ==
                 PipelineStatus::kInvalid);
    } else {
      TASK_CHECK(sync_dec == FrameDropDecision::kOk);
      TASK_CHECK(sync->GetQP() == dec[f].qp);
      TASK_CHECK(sync->GetLoopfilterLevel() == dec[f].loopfilter_level);
      TASK_CHECK(rc->SubmitFrameSize(dec[f].frame_id, actual[f]) ==
                 PipelineStatus::kOk);
      sync->PostEncodeUpdate(actual[f], params[f]);
    }
  }
  TASK_CHECK(drops > 0);
  // State matches the synchronous interface exactly (immediate feedback).
  VP9RcObservation obs, ref;
  rc->GetRcObservation(&obs);
  sync->GetRcObservation(&ref);
  TASK_CHECK(ObsEq(obs, ref, 2));
  // And also the independent serial reference with frozen decisions.
  VP9RcObservation ref2;
  TASK_CHECK(
      RunSerialReference(cfg, MakeRefFrames(params, dec, actual), &ref2));
  TASK_CHECK(ObsEq(obs, ref2, 2));
  return 0;
}

// ---------------------------------------------------------------------------
// lifecycle group
// ---------------------------------------------------------------------------

// Capacity limits, id reuse rules and invalid calls; the state must be
// untouched by every rejected call.
int LifecycleCapacityAndInvalid() {
  VP9RateControlRtcConfig cfg = MakeCfg(1, 4, 0);
  std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc != NULL);
  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<VP9RcFrameDecision> dec;
  std::vector<uint64_t> actual;
  for (int f = 0; f < 4; ++f) {
    params.push_back(MakeParams(0, 0, f == 0));
    actual.push_back(500 + 70 * f);
    dec.push_back(VP9RcFrameDecision());
    TASK_CHECK(rc->PrepareFrame(params[f], 500, &dec[f]) ==
               PipelineStatus::kOk);
    TASK_CHECK(dec[f].frame_id == (uint64_t)(f + 1));
  }
  // Capacity is 4: the next prepare fails with kFull and does not consume an
  // id or modify the caller's decision.
  {
    VP9RcFrameDecision blocked;
    blocked.frame_id = 9999;
    blocked.qp = 777;
    TASK_CHECK(rc->PrepareFrame(MakeParams(0, 0, false), 500, &blocked) ==
               PipelineStatus::kFull);
    TASK_CHECK(blocked.frame_id == 9999);
    TASK_CHECK(blocked.qp == 777);
  }
  // Confirm f1 behind the hole at f0: it stays in the ring (still counts
  // towards the capacity), so the next prepare is still full.
  TASK_CHECK(rc->SubmitFrameSize(dec[1].frame_id, actual[1]) ==
             PipelineStatus::kOk);
  TASK_CHECK(rc->PrepareFrame(MakeParams(0, 0, false), 500, &dec[0]) ==
             PipelineStatus::kFull);
  // Repeating the same submission is a no-op success; a different size is
  // rejected.
  TASK_CHECK(rc->SubmitFrameSize(dec[1].frame_id, actual[1]) ==
             PipelineStatus::kOk);
  TASK_CHECK(rc->SubmitFrameSize(dec[1].frame_id, actual[1] + 1) ==
             PipelineStatus::kInvalid);
  // Fill the hole: f0 and f1 fold, freeing slots.
  TASK_CHECK(rc->SubmitFrameSize(dec[0].frame_id, actual[0]) ==
             PipelineStatus::kOk);
  // The folded ids are reclaimed: submitting them now fails.
  TASK_CHECK(rc->SubmitFrameSize(dec[0].frame_id, actual[0]) ==
             PipelineStatus::kInvalid);
  TASK_CHECK(rc->SubmitFrameSize(dec[1].frame_id, actual[1]) ==
             PipelineStatus::kInvalid);
  // The kFull calls did not burn an id: the next one is 5.
  params.push_back(MakeParams(0, 0, false));
  actual.push_back(640);
  dec.push_back(VP9RcFrameDecision());
  TASK_CHECK(rc->PrepareFrame(params[4], 500, &dec[4]) == PipelineStatus::kOk);
  TASK_CHECK(dec[4].frame_id == 5);

  VP9RcObservation before;
  rc->GetRcObservation(&before);
  // Invalid submissions: unknown id, conflicting sizes.
  TASK_CHECK(rc->SubmitFrameSize(999, 1000) == PipelineStatus::kInvalid);
  TASK_CHECK(rc->SubmitFrameSize(0, 1000) == PipelineStatus::kInvalid);
  // Invalid prepare arguments.
  TASK_CHECK(rc->PrepareFrame(MakeParams(1, 0, false), 500, &dec[4]) ==
             PipelineStatus::kInvalid);
  TASK_CHECK(rc->PrepareFrame(MakeParams(0, -1, false), 500, &dec[4]) ==
             PipelineStatus::kInvalid);
  TASK_CHECK(rc->PrepareFrame(MakeParams(0, 3, false), 500, &dec[4]) ==
             PipelineStatus::kInvalid);
  {
    VP9FrameParamsQpRTC bad = MakeParams(0, 0, false);
    bad.frame_type = static_cast<RcFrameType>(7);
    TASK_CHECK(rc->PrepareFrame(bad, 500, &dec[4]) == PipelineStatus::kInvalid);
    TASK_CHECK(rc->PrepareFrame(MakeParams(0, 0, false), 500, NULL) ==
               PipelineStatus::kInvalid);
  }
  // Configuration is frozen in pipeline mode.
  TASK_CHECK(!rc->UpdateRateControl(cfg));
  // The synchronous entry points are disabled in pipeline mode.
  TASK_CHECK(rc->ComputeQP(MakeParams(0, 0, false)) == FrameDropDecision::kDrop);
  TASK_CHECK(rc->GetQP() == 0);
  TASK_CHECK(rc->GetLoopfilterLevel() == 0);
  rc->PostEncodeUpdate(1000, MakeParams(0, 0, false));
  // None of the rejected calls changed the state.
  VP9RcObservation after;
  rc->GetRcObservation(&after);
  TASK_CHECK(ObsEq(before, after, 1));

  // Unsupported configurations are rejected by Create().
  TASK_CHECK(VP9RateControlRTC::Create(MakeCfg(1, 0, 0)) != NULL);
  {
    VP9RateControlRtcConfig bad = MakeCfg(1, 9, 0);
    TASK_CHECK(VP9RateControlRTC::Create(bad) == NULL);
    bad = MakeCfg(1, -1, 0);
    TASK_CHECK(VP9RateControlRTC::Create(bad) == NULL);
    bad = MakeCfg(1, 1, 0);
    bad.ss_number_layers = 2;
    TASK_CHECK(VP9RateControlRTC::Create(bad) == NULL);
    bad = MakeCfg(3, 1, 0);
    TASK_CHECK(VP9RateControlRTC::Create(bad) == NULL);
    bad = MakeCfg(1, 1, 0);
    bad.rc_mode = VPX_VBR;
    TASK_CHECK(VP9RateControlRTC::Create(bad) == NULL);
    bad = MakeCfg(1, 1, 0);
    bad.aq_mode = 3;
    TASK_CHECK(VP9RateControlRTC::Create(bad) == NULL);
  }
  // The instance still works afterwards: drain the ring and compare against
  // the serial reference.
  TASK_CHECK(rc->SubmitFrameSize(dec[2].frame_id, actual[2]) ==
             PipelineStatus::kOk);
  TASK_CHECK(rc->SubmitFrameSize(dec[3].frame_id, actual[3]) ==
             PipelineStatus::kOk);
  TASK_CHECK(rc->SubmitFrameSize(dec[4].frame_id, actual[4]) ==
             PipelineStatus::kOk);
  VP9RcObservation obs, ref;
  rc->GetRcObservation(&obs);
  TASK_CHECK(RunSerialReference(cfg, MakeRefFrames(params, dec, actual), &ref));
  TASK_CHECK(ObsEq(obs, ref, 1));
  return 0;
}

// Capacity 1 with immediate feedback and estimate == actual must reproduce
// the synchronous interface frame by frame: QP, loopfilter and state.
int LifecycleSyncIdentity() {
  for (int ts = 1; ts <= 2; ++ts) {
    VP9RateControlRtcConfig cfg = MakeCfg(ts, 1, 0);
    std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
    TASK_CHECK(rc != NULL);
    std::unique_ptr<VP9RateControlRTC> sync =
        VP9RateControlRTC::Create(SyncCfg(cfg));
    TASK_CHECK(sync != NULL);
    for (int f = 0; f < 12; ++f) {
      const bool key = (f % 6 == 0);
      VP9FrameParamsQpRTC p = MakeParams(0, ts == 2 ? (key ? 0 : f % 2) : 0, key);
      const uint64_t size = 400 + 113 * f;
      VP9RcFrameDecision d;
      TASK_CHECK(rc->PrepareFrame(p, size, &d) == PipelineStatus::kOk);
      TASK_CHECK(!d.dropped);
      TASK_CHECK(d.frame_id == (uint64_t)(f + 1));
      TASK_CHECK(sync->ComputeQP(p) == FrameDropDecision::kOk);
      TASK_CHECK(d.qp == sync->GetQP());
      TASK_CHECK(d.loopfilter_level == sync->GetLoopfilterLevel());
      TASK_CHECK(rc->SubmitFrameSize(d.frame_id, size) == PipelineStatus::kOk);
      sync->PostEncodeUpdate(size, p);
      VP9RcObservation obs, ref;
      rc->GetRcObservation(&obs);
      sync->GetRcObservation(&ref);
      TASK_CHECK(ObsEq(obs, ref, ts));
    }
  }
  return 0;
}

// Multiple ring reuse cycles with capacity 2 over many frames.
int LifecycleRingReuse() {
  const int kN = 24;
  VP9RateControlRtcConfig cfg = MakeCfg(1, 2, 0);
  std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc != NULL);
  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<VP9RcFrameDecision> dec;
  std::vector<uint64_t> actual;
  for (int f = 0; f < kN; ++f) {
    params.push_back(MakeParams(0, 0, f % 8 == 0));
    actual.push_back(350 + 89 * f);
    dec.push_back(VP9RcFrameDecision());
    TASK_CHECK(rc->PrepareFrame(params[f], 400, &dec[f]) ==
               PipelineStatus::kOk);
    TASK_CHECK(dec[f].frame_id == (uint64_t)(f + 1));
    if (f >= 1) {
      // Ring capacity 2: submit the previous frame's size.
      TASK_CHECK(rc->SubmitFrameSize(dec[f - 1].frame_id, actual[f - 1]) ==
                 PipelineStatus::kOk);
    }
  }
  TASK_CHECK(rc->SubmitFrameSize(dec[kN - 1].frame_id, actual[kN - 1]) ==
             PipelineStatus::kOk);
  VP9RcObservation obs, ref;
  rc->GetRcObservation(&obs);
  TASK_CHECK(RunSerialReference(cfg, MakeRefFrames(params, dec, actual), &ref));
  TASK_CHECK(ObsEq(obs, ref, 1));
  TASK_CHECK(obs.current_video_frame == (unsigned int)kN);
  return 0;
}

// Two pipeline instances interleaved: they evolve independently.
int LifecycleTwoInstances() {
  VP9RateControlRtcConfig cfg1 = MakeCfg(1, 4, 0);
  VP9RateControlRtcConfig cfg2 = MakeCfg(2, 4, 0);
  std::unique_ptr<VP9RateControlRTC> a = VP9RateControlRTC::Create(cfg1);
  std::unique_ptr<VP9RateControlRTC> b = VP9RateControlRTC::Create(cfg2);
  TASK_CHECK(a != NULL && b != NULL);
  std::vector<VP9FrameParamsQpRTC> pa, pb;
  std::vector<VP9RcFrameDecision> da, db;
  std::vector<uint64_t> ca, cb;
  for (int f = 0; f < 6; ++f) {
    pa.push_back(MakeParams(0, 0, f == 0));
    pb.push_back(MakeParams(0, f % 2, f == 0));
    ca.push_back(300 + 97 * f);
    cb.push_back(250 + 131 * f);
    da.push_back(VP9RcFrameDecision());
    db.push_back(VP9RcFrameDecision());
  }
  // Interleave prepares and submits across the two instances.
  for (int f = 0; f < 6; ++f) {
    TASK_CHECK(a->PrepareFrame(pa[f], 400, &da[f]) == PipelineStatus::kOk);
    TASK_CHECK(b->PrepareFrame(pb[f], 400, &db[f]) == PipelineStatus::kOk);
    if (f >= 2) {
      TASK_CHECK(a->SubmitFrameSize(da[f - 2].frame_id, ca[f - 2]) ==
                 PipelineStatus::kOk);
      TASK_CHECK(b->SubmitFrameSize(db[f - 2].frame_id, cb[f - 2]) ==
                 PipelineStatus::kOk);
    }
  }
  for (int f = 4; f < 6; ++f) {
    TASK_CHECK(a->SubmitFrameSize(da[f].frame_id, ca[f]) == PipelineStatus::kOk);
    TASK_CHECK(b->SubmitFrameSize(db[f].frame_id, cb[f]) == PipelineStatus::kOk);
  }
  VP9RcObservation oa, ob, ra, rb;
  a->GetRcObservation(&oa);
  b->GetRcObservation(&ob);
  TASK_CHECK(RunSerialReference(cfg1, MakeRefFrames(pa, da, ca), &ra));
  TASK_CHECK(RunSerialReference(cfg2, MakeRefFrames(pb, db, cb), &rb));
  TASK_CHECK(ObsEq(oa, ra, 1));
  TASK_CHECK(ObsEq(ob, rb, 2));
  return 0;
}

// Real VP9 encoder packets as feedback: the corrections applied once the
// true sizes arrive must have a visible effect on subsequent decisions.
int LifecycleRealEncoder() {
  const int kN = 16;
  const int w = 96, h = 64;
  VP9RateControlRtcConfig cfg = MakeCfg(1, 4, 0);
  std::unique_ptr<VP9RateControlRTC> rc = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc != NULL);
  // A second instance that is misled with 4x the real sizes.
  std::unique_ptr<VP9RateControlRTC> rc_big = VP9RateControlRTC::Create(cfg);
  TASK_CHECK(rc_big != NULL);

  vpx_codec_enc_cfg_t enc_cfg;
  std::memset(&enc_cfg, 0, sizeof(enc_cfg));
  TASK_CHECK(vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &enc_cfg, 0) ==
             VPX_CODEC_OK);
  enc_cfg.g_w = w;
  enc_cfg.g_h = h;
  enc_cfg.g_threads = 1;
  enc_cfg.g_timebase.num = 1;
  enc_cfg.g_timebase.den = 30;
  enc_cfg.g_lag_in_frames = 0;
  enc_cfg.rc_end_usage = VPX_CBR;
  enc_cfg.rc_target_bitrate = 80;
  enc_cfg.rc_min_quantizer = 2;
  enc_cfg.rc_max_quantizer = 56;
  enc_cfg.rc_buf_initial_sz = 600;
  enc_cfg.rc_buf_optimal_sz = 600;
  enc_cfg.rc_buf_sz = 1000;
  enc_cfg.rc_dropframe_thresh = 0;
  enc_cfg.kf_mode = VPX_KF_DISABLED;
  vpx_codec_ctx_t enc;
  TASK_CHECK(vpx_codec_enc_init(&enc, vpx_codec_vp9_cx(), &enc_cfg, 0) ==
             VPX_CODEC_OK);
  TASK_CHECK(vpx_codec_control(&enc, VP8E_SET_CPUUSED, 7) == VPX_CODEC_OK);
  TASK_CHECK(vpx_codec_control(&enc, VP9E_SET_AQ_MODE, 0) == VPX_CODEC_OK);
  TASK_CHECK(vpx_codec_control(&enc, VP8E_SET_ENABLEAUTOALTREF, 0) ==
             VPX_CODEC_OK);
  TASK_CHECK(vpx_codec_control(&enc, VP8E_SET_MAX_INTRA_BITRATE_PCT, 1000) ==
             VPX_CODEC_OK);
  TASK_CHECK(vpx_codec_control(&enc, VP9E_SET_RTC_EXTERNAL_RATECTRL, 1) ==
             VPX_CODEC_OK);
  vpx_image_t *img = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, w, h, 1);
  TASK_CHECK(img != NULL);

  std::vector<VP9FrameParamsQpRTC> params;
  std::vector<VP9RcFrameDecision> dec, dec_big;
  std::vector<uint64_t> actual;
  std::vector<uint64_t> pending_id;
  std::vector<int> pending_idx;
  int qp_sum = 0, qp_sum_big = 0;
  for (int f = 0; f < kN; ++f) {
    params.push_back(MakeParams(0, 0, f % 8 == 0));
    dec.push_back(VP9RcFrameDecision());
    dec_big.push_back(VP9RcFrameDecision());
    // Estimate: the previous frame's actual size (or a guess for the first).
    const uint64_t est = f == 0 ? 2000 : actual[f - 1];
    TASK_CHECK(rc->PrepareFrame(params[f], est, &dec[f]) == PipelineStatus::kOk);
    TASK_CHECK(rc_big->PrepareFrame(params[f], est, &dec_big[f]) ==
               PipelineStatus::kOk);
    TASK_CHECK(!dec[f].dropped && !dec_big[f].dropped);
    qp_sum += dec[f].qp;
    qp_sum_big += dec_big[f].qp;
    // Encode a synthetic frame and harvest the real packet size.
    for (int c = 0; c < 3; ++c) {
      const int cw = w >> (c != 0), ch = h >> (c != 0);
      for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x)
          img->planes[c][y * img->stride[c] + x] =
              (unsigned char)(((x * 19 + y * 31 + f * 17 + c * 59) ^
                               (x * y + f * 7)) &
                              255);
    }
    const vpx_enc_frame_flags_t flags =
        (f % 8 == 0) ? VPX_EFLAG_FORCE_KF
                     : (VP8_EFLAG_NO_UPD_GF | VP8_EFLAG_NO_UPD_ARF);
    TASK_CHECK(vpx_codec_encode(&enc, img, f, 1, flags, VPX_DL_REALTIME) ==
               VPX_CODEC_OK);
    size_t frame_bytes = 0;
    vpx_codec_iter_t iter = NULL;
    const vpx_codec_cx_pkt_t *pkt;
    unsigned packets = 0;
    while ((pkt = vpx_codec_get_cx_data(&enc, &iter)) != NULL) {
      if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
        ++packets;
        frame_bytes += pkt->data.frame.sz;
      }
    }
    TASK_CHECK(packets == 1);
    TASK_CHECK(frame_bytes > 0);
    actual.push_back(frame_bytes);
    // Feedback arrives one frame late for both instances.
    if (!pending_id.empty()) {
      const int idx = pending_idx.front();
      TASK_CHECK(rc->SubmitFrameSize(dec[idx].frame_id, actual[idx]) ==
                 PipelineStatus::kOk);
      TASK_CHECK(rc_big->SubmitFrameSize(dec_big[idx].frame_id,
                                         4 * actual[idx]) ==
                 PipelineStatus::kOk);
      pending_id.erase(pending_id.begin());
      pending_idx.erase(pending_idx.begin());
    }
    pending_id.push_back(dec[f].frame_id);
    pending_idx.push_back(f);
  }
  {
    const int idx = pending_idx.front();
    TASK_CHECK(rc->SubmitFrameSize(dec[idx].frame_id, actual[idx]) ==
               PipelineStatus::kOk);
    TASK_CHECK(rc_big->SubmitFrameSize(dec_big[idx].frame_id,
                                       4 * actual[idx]) ==
               PipelineStatus::kOk);
  }
  vpx_img_free(img);
  TASK_CHECK(vpx_codec_destroy(&enc) == VPX_CODEC_OK);

  // The real sizes drove the state to exactly the serial reference's state.
  VP9RcObservation obs, ref;
  rc->GetRcObservation(&obs);
  TASK_CHECK(RunSerialReference(cfg, MakeRefFrames(params, dec, actual), &ref));
  TASK_CHECK(ObsEq(obs, ref, 1));
  // The corrections had a real effect: the misled instance diverged.
  VP9RcObservation obs_big;
  rc_big->GetRcObservation(&obs_big);
  TASK_CHECK(qp_sum != qp_sum_big ||
             obs.rc.buffer_level != obs_big.rc.buffer_level);
  TASK_CHECK(obs.rc.total_actual_bits != obs_big.rc.total_actual_bits);
  return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

}  // namespace

const struct task_case task_cases[] = {
  { "replay", "feedback_order_invariance", ReplayFeedbackOrderInvariance },
  { "replay", "out_of_order_reverse", ReplayOutOfOrderReverse },
  { "replay", "hole_then_big_frame", ReplayHoleThenBigFrame },
  { "replay", "estimate_over_under", ReplayEstimateOverUnder },
  { "replay", "drop_delayed_feedback", ReplayDropDelayedFeedback },
  { "layers", "two_temporal_reverse", LayersTwoTemporalReverse },
  { "layers", "two_temporal_keyframe", LayersTwoTemporalKeyframe },
  { "layers", "single_consecutive", LayersSingleConsecutive },
  { "layers", "drop_two_temporal", LayersDropTwoTemporal },
  { "lifecycle", "capacity_and_invalid", LifecycleCapacityAndInvalid },
  { "lifecycle", "sync_identity", LifecycleSyncIdentity },
  { "lifecycle", "ring_reuse", LifecycleRingReuse },
  { "lifecycle", "two_instances", LifecycleTwoInstances },
  { "lifecycle", "real_encoder", LifecycleRealEncoder },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
