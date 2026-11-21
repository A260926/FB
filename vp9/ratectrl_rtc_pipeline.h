/*
 *  Copyright (c) 2026 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// Internal machinery for the pipelined VP9 RTC rate control interface
// (VP9RateControlRTC::PrepareFrame/SubmitFrameSize). This header is not
// part of the public API, but is shipped with the library sources so that
// tests can observe the rate control state and build independent serial
// references on top of the same scalar bookkeeping core.

#ifndef VPX_VP9_RATECTRL_RTC_PIPELINE_H_
#define VPX_VP9_RATECTRL_RTC_PIPELINE_H_

#include <cstdint>
#include <deque>

#include "vp9/encoder/vp9_ratectrl.h"
#include "vp9/ratectrl_rtc.h"

struct VP9_COMP;

namespace libvpx {

// Frozen per-frame decision. Once a frame has been prepared, these values
// never change; only the adaptive bookkeeping is recomputed when the actual
// encoded size becomes known.
struct Vp9RcFrameTrace {
  // Decision outputs.
  bool dropped = false;
  int qp = -1;
  int loopfilter_level = -1;
  int this_frame_target = 0;
  // FRAME_TYPE (KEY_FRAME/INTER_FRAME) after any override by the rate
  // control core (e.g. a forced key frame on the very first frame).
  int effective_frame_type = 0;
  // Side effects of the drop-decision machinery (vp9_rc_drop_frame_decision)
  // captured right after the decision was made. These are part of the frozen
  // decision, not of the adaptive bookkeeping.
  int decimation_factor = 0;
  int decimation_count = 0;
  int svc_drop_count = 0;
};

// One frame that has been prepared but not yet folded into the reclaimed
// prefix snapshot.
struct Vp9RcPendingFrame {
  uint64_t frame_id = 0;
  VP9FrameParamsQpRTC params = {};
  Vp9RcFrameTrace trace;
  // Cost currently used for bookkeeping, in bytes: the actual size once
  // size_known, otherwise the (clamped) estimate. Always 0 for drops.
  uint64_t cost = 0;
  // Clamped estimate passed to PrepareFrame().
  uint64_t estimate = 0;
  // True once the actual size has been submitted. Dropped frames are known
  // from the start (their cost is fixed to 0).
  bool size_known = false;
};

// Ownable snapshot of the complete RTC rate control state: every scalar the
// decision and bookkeeping logic reads or writes across frames. Contains no
// pointers, so it can be copied and stored independently of VP9_COMP.
struct Vp9RcSnapshot {
  RATE_CONTROL rc;
  struct {
    RATE_CONTROL rc;
    int target_bandwidth = 0;
    double framerate = 0.0;
    int avg_frame_size = 0;
    unsigned int current_video_frame_in_layer = 0;
    int is_key_frame = 0;
    int frames_from_key_frame = 0;
    int frame_qp = 0;
    int MBs = 0;
  } layers[VPX_MAX_LAYERS];
  int current_superframe = 0;
  int num_encoded_top_layer = 0;
  int drop_count[VPX_MAX_LAYERS] = {};
  int last_layer_dropped[VPX_MAX_LAYERS] = {};
  int drop_spatial_layer[VPX_MAX_LAYERS] = {};
  int skip_enhancement_layer = 0;
  unsigned int current_video_frame = 0;
  // Last frame type/qp, read by the dropped-frame bookkeeping.
  int frame_type = 0;
  int base_qindex = 0;
  int last_frame_dropped = 0;
  int ext_refresh_frame_flags_pending = 0;
  int deadline_mode_previous_frame = 0;
};

// State of the pipelined interface: the snapshot after the longest
// contiguous confirmed prefix, plus the records still waiting for feedback
// (bounded by max_pending_frames, so memory does not grow with the number of
// processed frames).
struct VP9RcPipelineState {
  Vp9RcSnapshot snapshot;
  std::deque<Vp9RcPendingFrame> records;
  uint64_t next_frame_id = 1;
};

// Read-only observation of the rate control state, for tests and
// diagnostics. Mirrors the parts of RATE_CONTROL and the layer contexts that
// the RTC decision/bookkeeping path maintains.
struct VP9RcObservation {
  RATE_CONTROL rc;
  RATE_CONTROL layer_rc[VPX_MAX_LAYERS];
  unsigned int current_video_frame = 0;
  int current_superframe = 0;
  int num_encoded_top_layer = 0;
  int drop_count[VPX_MAX_LAYERS] = {};
  unsigned int current_video_frame_in_layer[VPX_MAX_LAYERS] = {};
  int frames_from_key_frame[VPX_MAX_LAYERS] = {};
  int is_key_frame[VPX_MAX_LAYERS] = {};
};

// Initialization helpers shared by VP9RateControlRTC and the test factory
// below. They perform exactly the same setup as the default (synchronous)
// interface.
bool VP9RcInitCpi(VP9_COMP *cpi, const VP9RateControlRtcConfig &cfg,
                  int *initial_width, int *initial_height, bool *rc_is_valid);
bool VP9RcUpdateCpiConfig(VP9_COMP *cpi, const VP9RateControlRtcConfig &cfg,
                          int *initial_width, int *initial_height,
                          bool *rc_is_valid);
bool VP9RcCpiAllocAqData(VP9_COMP *cpi);
void VP9RcCpiFree(VP9_COMP *cpi);

// Test/diagnostics factory: creates a VP9_COMP initialized exactly like the
// synchronous interface does, so tests can drive the original scalar
// bookkeeping core (vp9_rc_postencode_update and friends) directly with
// fixed decisions. Destroy with VP9RcCpiDestroy().
VP9_COMP *VP9RcCpiCreate(const VP9RateControlRtcConfig &cfg);
void VP9RcCpiDestroy(VP9_COMP *cpi);

// Runs the pre-encode rate control flow for the next frame on cpi (frame
// target, buffer income, drop decision, QP selection) and fills *trace with
// the frozen decision. Returns true if the frame is dropped (in which case
// the drop bookkeeping has already been applied). This is the same logic
// used by VP9RateControlRTC::ComputeQP().
bool Vp9RcDecideFrame(VP9_COMP *cpi, const VP9FrameParamsQpRTC &frame_params,
                      Vp9RcFrameTrace *trace);

// Computes the loopfilter level for the frame currently set up on cpi.
int Vp9RcPickLoopfilter(VP9_COMP *cpi);

// Applies the post-encode bookkeeping for an encoded (not dropped) frame
// with the given cost in bytes, using the frame's frozen qp.
void Vp9RcApplyEncodedCost(VP9_COMP *cpi, const Vp9RcPendingFrame &rec,
                           uint64_t cost_bytes);

// Replays one pending record on cpi: re-applies the record's frozen decision
// and its bookkeeping with the record's current cost. cpi must hold the
// state preceding the record (see Vp9RcRestoreSnapshot).
void Vp9RcReplayRecord(VP9_COMP *cpi, const Vp9RcPendingFrame &rec);

// Saves/restores the complete rate control state of cpi.
void Vp9RcSaveSnapshot(const VP9_COMP *cpi, Vp9RcSnapshot *snapshot);
void Vp9RcRestoreSnapshot(VP9_COMP *cpi, const Vp9RcSnapshot &snapshot);

// Folds the longest contiguous confirmed prefix of pipe->records into
// pipe->snapshot and reclaims those records.
void Vp9RcFoldPrefix(VP9_COMP *cpi, VP9RcPipelineState *pipe);

// Fills *obs with the current rate control state of cpi.
void Vp9RcObserveCpi(const VP9_COMP *cpi, VP9RcObservation *obs);

}  // namespace libvpx

#endif  // VPX_VP9_RATECTRL_RTC_PIPELINE_H_
