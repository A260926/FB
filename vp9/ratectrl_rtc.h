/*
 *  Copyright (c) 2020 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef VPX_VP9_RATECTRL_RTC_H_
#define VPX_VP9_RATECTRL_RTC_H_

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

#include "vpx/vpx_encoder.h"
#include "vpx/internal/vpx_ratectrl_rtc.h"

struct VP9_COMP;

namespace libvpx {
struct VP9RateControlRtcConfig : public VpxRateControlRtcConfig {
  VP9RateControlRtcConfig() {
    memset(layer_target_bitrate, 0, sizeof(layer_target_bitrate));
    memset(ts_rate_decimator, 0, sizeof(ts_rate_decimator));
    scaling_factor_num[0] = 1;
    scaling_factor_den[0] = 1;
    max_quantizers[0] = max_quantizer;
    min_quantizers[0] = min_quantizer;
  }

  // Number of spatial layers
  int ss_number_layers = 1;
  int max_quantizers[VPX_MAX_LAYERS] = {};
  int min_quantizers[VPX_MAX_LAYERS] = {};
  int scaling_factor_num[VPX_SS_MAX_LAYERS] = {};
  int scaling_factor_den[VPX_SS_MAX_LAYERS] = {};
  // This is only for SVC for now.
  int max_consec_drop = std::numeric_limits<int>::max();
  // Number of frames that may be waiting for encoded-size feedback at the
  // same time. 0 (default) keeps the synchronous ComputeQP/PostEncodeUpdate
  // interface. 1..8 enables the pipelined PrepareFrame/SubmitFrameSize
  // interface, which allows rate control decisions to be issued before the
  // sizes of earlier frames are known. When enabled, the configuration is
  // fixed after Create() (UpdateRateControl() fails and keeps the state),
  // and only fixed frame dimensions, VPX_CBR, aq_mode = 0, a single spatial
  // layer and one or two temporal layers are supported.
  int max_pending_frames = 0;
};

struct VP9FrameParamsQpRTC {
  RcFrameType frame_type;
  int spatial_layer_id;
  int temporal_layer_id;
};

struct VP9SegmentationData {
  const uint8_t *segmentation_map;
  size_t segmentation_map_size;
  const int *delta_q;
  size_t delta_q_size;
};

// Status returned by the pipelined rate control interface.
enum class PipelineStatus {
  kOk,       // Operation succeeded.
  kFull,     // Too many frames are waiting for size feedback.
  kInvalid,  // Invalid argument or unknown frame id.
};

// Rate control decision for one frame, returned by
// VP9RateControlRTC::PrepareFrame(). For dropped frames qp and
// loopfilter_level are set to -1 and no SubmitFrameSize() call is needed.
struct VP9RcFrameDecision {
  uint64_t frame_id = 0;
  bool dropped = false;
  int qp = -1;
  int loopfilter_level = -1;
};

struct VP9RcPipelineState;
struct VP9RcObservation;

// This interface allows using VP9 real-time rate control without initializing
// the encoder. To use this interface, you need to link with libvpxrc.a.
//
// #include "vp9/ratectrl_rtc.h"
// VP9RateControlRtcConfig cfg;
// VP9FrameParamsQpRTC frame_params;
//
// YourFunctionToInitializeConfig(cfg);
// std::unique_ptr<VP9RateControlRTC> rc_api = VP9RateControlRTC::Create(cfg);
// // start encoding
// while (frame_to_encode) {
//   if (config_changed)
//     rc_api->UpdateRateControl(cfg);
//   YourFunctionToFillFrameParams(frame_params);
//   rc_api->ComputeQP(frame_params);
//   YourFunctionToUseQP(rc_api->GetQP());
//   YourFunctionToUseLoopfilter(rc_api->GetLoopfilterLevel());
//   // After encoding
//   rc_api->PostEncode(encoded_frame_size, frame_params);
// }
//
// When VP9RateControlRtcConfig::max_pending_frames is in [1, 8], the
// pipelined interface is enabled instead: PrepareFrame() issues decisions
// (QP, loopfilter level, drop) for new frames without waiting for the
// encoded sizes of earlier frames, and SubmitFrameSize() feeds the actual
// sizes back, possibly out of order. Decisions already returned are never
// revised; only the adaptive bookkeeping is recomputed with the actual
// costs. In this mode ComputeQP()/PostEncodeUpdate()/UpdateRateControl()
// must not be used.
class VP9RateControlRTC {
 public:
  static std::unique_ptr<VP9RateControlRTC> Create(
      const VP9RateControlRtcConfig &cfg);
  ~VP9RateControlRTC();

  bool UpdateRateControl(const VP9RateControlRtcConfig &rc_cfg);
  // GetQP() needs to be called after ComputeQP() to get the latest QP
  int GetQP() const;
  int GetLoopfilterLevel() const;
  bool GetSegmentationData(VP9SegmentationData *segmentation_data) const;
  // ComputeQP computes the QP if the frame is not dropped (kOk return),
  // otherwise it returns kDrop and subsequent GetQP and PostEncodeUpdate
  // are not to be called (vp9_rc_postencode_update_drop_frame is already
  // called via ComputeQP if drop is decided).
  FrameDropDecision ComputeQP(const VP9FrameParamsQpRTC &frame_params);
  // Feedback to rate control with the size of current encoded frame
  void PostEncodeUpdate(uint64_t encoded_frame_size,
                        const VP9FrameParamsQpRTC &frame_params);

  // Pipelined interface (enabled when max_pending_frames is in [1, 8]).
  //
  // PrepareFrame() selects the rate control decision for the next frame and
  // returns it in *decision (with a fresh, monotonically increasing frame_id
  // starting at 1). The frame is immediately accounted for with
  // estimated_bytes (clamped to [1, INT_MAX/8]) so that subsequent frames
  // can be prepared before any feedback arrives. If the frame is dropped,
  // the estimate is ignored, the cost is fixed to 0 and no SubmitFrameSize()
  // is expected. Returns kFull without side effects when max_pending_frames
  // frames are already waiting for feedback, and kInvalid for invalid
  // arguments; in both cases *decision is left untouched.
  PipelineStatus PrepareFrame(const VP9FrameParamsQpRTC &frame_params,
                              uint64_t estimated_bytes,
                              VP9RcFrameDecision *decision);
  // SubmitFrameSize() reports the actual encoded size (clamped to
  // [1, INT_MAX/8]) of a frame previously returned by PrepareFrame(). Sizes
  // may arrive out of order. Submitting the same size twice for the same
  // frame is a no-op returning kOk; a different size, an unknown or already
  // reclaimed frame_id, or the id of a dropped frame returns kInvalid.
  PipelineStatus SubmitFrameSize(uint64_t frame_id, uint64_t actual_bytes);
  // Fills *obs with a read-only observation of the current rate control
  // state (global and per-layer buffers, rate correction factors, QP
  // history and frame counters). VP9RcObservation is defined in
  // vp9/ratectrl_rtc_pipeline.h. Intended for tests and diagnostics.
  void GetRcObservation(VP9RcObservation *obs);

 private:
  VP9RateControlRTC() = default;
  bool InitRateControl(const VP9RateControlRtcConfig &cfg);
  struct VP9_COMP *cpi_ = nullptr;
  int initial_width_ = 0;
  int initial_height_ = 0;
  bool rc_is_valid_ = false;
  // Pipelined interface state (only used when max_pending_frames > 0).
  bool pipeline_mode_ = false;
  int max_pending_frames_ = 0;
  std::unique_ptr<VP9RcPipelineState> pipeline_;
};

}  // namespace libvpx

#endif  // VPX_VP9_RATECTRL_RTC_H_
