/* Copyright (c) 2026 Xiph.Org Foundation
   Redistribution and use in source and binary forms are governed by the
   same BSD-style license as the rest of Opus; see COPYING for details. */

/* 48 kHz -> 44.1 kHz causal resampling engine for the Opus float decoder.

   The Opus decoder always runs internally at 48 kHz.  Instances created with
   Fs=44100 keep that 48 kHz codec and attach one of these engines per
   OpusDecoder.  After S source frames have been delivered, exactly
   floor(S*147/160) output frames have been produced, driven by a single clock
   that is never reset on packet boundaries.  Only the most recent 64 source
   frames per channel are retained; no whole PCM segment is stored and no
   heap allocation is ever performed. */

#ifndef OPUS_RESAMPLER_44100_H
#define OPUS_RESAMPLER_44100_H

#include "opus.h"

/* FIR length, number of fractional phases and per-channel history depth. */
#define OPUS_R44100_TAPS 64
#define OPUS_R44100_PHASES 147
#define OPUS_R44100_HISTORY OPUS_R44100_TAPS

typedef struct OpusResampler44100 {
   int channels;
   int initialized;
   /* Source frames (48 kHz domain) already handed to the engine. */
   opus_int64 src_count;
   /* Output frames (44.1 kHz domain) already produced. */
   opus_int64 out_count;
   /* Frames written by the last successful process call (output domain). */
   int last_out;
   /* Slot holding the newest frame in the circular history buffer. */
   int hist_head;
   /* Last OPUS_R44100_HISTORY source frames, interleaved, bounded storage. */
   float history[OPUS_R44100_HISTORY*2];
   /* At most one output frame can be computed before the clock is ready to
      deliver it; it is parked here across process() calls. */
   int hold_valid;
   opus_int64 hold_index;
   float hold[2];
} OpusResampler44100;

/* Initializes (or clears) an engine for 1 or 2 channels. */
int opus_r44100_init(OpusResampler44100 *st, int channels);

/* Clears clock and FIR history back to the fresh-instance state. */
int opus_r44100_reset(OpusResampler44100 *st);

/* Total output frames produced once src_total source frames have been
   delivered: floor(src_total*147/160). */
opus_int64 opus_r44100_outputs_for(opus_int64 src_total);

/* Number of output frames that the next process() call adding src_frame_count
   source frames would produce.  Pure query: no state is advanced. */
int opus_r44100_pending(const OpusResampler44100 *st, int src_frame_count);

/* Number of source frames that must be supplied to produce out_frame_count
   more output frames at the current clock position.  Pure query. */
int opus_r44100_source_needed(const OpusResampler44100 *st, int out_frame_count);

/* Resamples src_frame_count interleaved frames from `in` (stride in_stride)
   into `out` (stride out_stride), which has room for out_capacity frames per
   channel.  On success returns the number of frames written per channel and
   commits the clock/FIR state exactly once.  On OPUS_BUFFER_TOO_SMALL no
   state is modified and no output is written; in-place operation (in == out)
   is supported. */
int opus_r44100_process(OpusResampler44100 *st, const float *in, int in_stride,
      int src_frame_count, float *out, int out_stride, int out_capacity);

#endif /* OPUS_RESAMPLER_44100_H */
