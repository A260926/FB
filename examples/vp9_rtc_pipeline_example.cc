/*
 *  Copyright (c) 2026 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// Example of the pipelined VP9 RTC rate control interface (libvpxrc).
//
// The pipelined interface lets the rate controller issue QP/loopfilter/drop
// decisions for new frames before the encoded sizes of earlier frames are
// known: PrepareFrame() books each new frame with an estimated size, and
// SubmitFrameSize() later reports the actual size, possibly out of order.
// Decisions already returned are never revised; only the adaptive
// bookkeeping is recomputed with the actual costs.
//
// Build: link against libvpxrc.a and libvpx.a.

#include <stdint.h>
#include <stdio.h>

#include <memory>

#include "vp9/ratectrl_rtc.h"

namespace {

// Stands in for an encoder: returns the "encoded" size of a frame. In a real
// application this is the packet length produced by the VP9 encoder.
uint64_t FakeEncodeFrame(int frame, int qp) {
  const uint64_t base = 900 + (frame % 5) * 130;
  const int shift = (qp > 200) ? 2 : (qp > 120 ? 1 : 0);
  return base >> shift;
}

}  // namespace

int main() {
  libvpx::VP9RateControlRtcConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.target_bandwidth = 400;  // kbps
  cfg.framerate = 30;
  cfg.min_quantizer = 4;
  cfg.max_quantizer = 56;
  cfg.max_intra_bitrate_pct = 1000;
  cfg.aq_mode = 0;
  cfg.frame_drop_thresh = 0;
  cfg.ts_number_layers = 1;
  cfg.layer_target_bitrate[0] = 400;
  cfg.ts_rate_decimator[0] = 1;
  cfg.min_quantizers[0] = 4;
  cfg.max_quantizers[0] = 56;
  // Enable the pipelined interface with up to 4 frames in flight.
  cfg.max_pending_frames = 4;

  std::unique_ptr<libvpx::VP9RateControlRTC> rc =
      libvpx::VP9RateControlRTC::Create(cfg);
  if (!rc) {
    fprintf(stderr, "Failed to create rate control\n");
    return 1;
  }

  const int kNumFrames = 12;
  uint64_t frame_ids[kNumFrames];
  int qps[kNumFrames];
  uint64_t estimated[kNumFrames];
  int prepared = 0;
  int submitted = 0;
  // Keep at most cfg.max_pending_frames frames waiting for feedback: prepare
  // a new frame, then submit the actual size of the oldest outstanding one.
  while (prepared < kNumFrames || submitted < prepared) {
    if (prepared < kNumFrames && prepared - submitted < cfg.max_pending_frames) {
      libvpx::VP9FrameParamsQpRTC params = {};
      params.spatial_layer_id = 0;
      params.temporal_layer_id = 0;
      params.frame_type = (prepared % 8 == 0)
                              ? libvpx::RcFrameType::kKeyFrame
                              : libvpx::RcFrameType::kInterFrame;
      // The encoder's size estimate for the upcoming frame.
      estimated[prepared] = 1000;
      libvpx::VP9RcFrameDecision decision;
      const libvpx::PipelineStatus status =
          rc->PrepareFrame(params, estimated[prepared], &decision);
      if (status == libvpx::PipelineStatus::kFull) {
        // Should not happen given the bounded loop above, but a real
        // application would submit an outstanding frame and retry.
        fprintf(stderr, "Rate control ring is full\n");
        return 1;
      }
      if (status != libvpx::PipelineStatus::kOk) {
        fprintf(stderr, "PrepareFrame failed\n");
        return 1;
      }
      frame_ids[prepared] = decision.frame_id;
      qps[prepared] = decision.qp;
      if (decision.dropped) {
        printf("frame %2d: dropped (id=%llu)\n", prepared,
               (unsigned long long)decision.frame_id);
        ++prepared;  // Dropped frames need no SubmitFrameSize().
        continue;
      }
      printf("frame %2d: id=%llu qp=%d loopfilter=%d\n", prepared,
             (unsigned long long)decision.frame_id, decision.qp,
             decision.loopfilter_level);
      ++prepared;
    } else {
      // "Encode" the oldest outstanding frame and report its real size.
      const uint64_t actual = FakeEncodeFrame(submitted, qps[submitted]);
      if (rc->SubmitFrameSize(frame_ids[submitted], actual) !=
          libvpx::PipelineStatus::kOk) {
        fprintf(stderr, "SubmitFrameSize failed for frame %d\n", submitted);
        return 1;
      }
      printf("  feedback frame %d: estimated=%llu actual=%llu\n", submitted,
             (unsigned long long)estimated[submitted],
             (unsigned long long)actual);
      ++submitted;
    }
  }
  printf("Pipelined rate control example completed.\n");
  return 0;
}
