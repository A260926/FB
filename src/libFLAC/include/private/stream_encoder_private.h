/* libFLAC - Free Lossless Audio Codec library
 * Copyright (C) 2000-2009  Josh Coalson
 * Copyright (C) 2011-2025  Xiph.Org Foundation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of the Xiph.org Foundation nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef FLAC__PRIVATE__STREAM_ENCODER_PRIVATE_H
#define FLAC__PRIVATE__STREAM_ENCODER_PRIVATE_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "FLAC/format.h"
#include "FLAC/stream_decoder.h"
#include "FLAC/stream_encoder.h"
#include "private/bitwriter.h"
#include "private/cpu.h"
#include "private/float.h"
#include "private/md5.h"
#include "private/stream_encoder.h"
#include "protected/stream_encoder.h"
#include "share/compat_threads.h"

/*
 * These structures used to live at the top of stream_encoder.c.  They were
 * moved here so that the checkpoint persistence code in
 * stream_encoder_checkpoint.c can inspect and restore the exact encoder
 * state without duplicating the layout.
 */

typedef struct {
	FLAC__int32 *data[FLAC__MAX_CHANNELS];
	uint32_t size; /* of each data[] in samples */
	uint32_t tail;
} verify_input_fifo;

typedef struct {
	const FLAC__byte *data;
	uint32_t capacity;
	uint32_t bytes;
} verify_output;

typedef enum {
	ENCODER_IN_MAGIC = 0,
	ENCODER_IN_METADATA = 1,
	ENCODER_IN_AUDIO = 2
} EncoderStateHint;

typedef struct FLAC__StreamEncoderThreadTask {
	FLAC__int32 *integer_signal[FLAC__MAX_CHANNELS];  /* the integer version of the input signal */
	FLAC__int32 *integer_signal_mid_side[2];          /* the integer version of the mid-side input signal (stereo only) */
	FLAC__int64 *integer_signal_33bit_side;           /* 33-bit side for 32-bit stereo decorrelation */
#ifndef FLAC__INTEGER_ONLY_LIBRARY
	FLAC__real *windowed_signal;                      /* the integer_signal[] * current window[] */
#endif
	uint32_t subframe_bps[FLAC__MAX_CHANNELS];        /* the effective bits per sample of the input signal (stream bps - wasted bits) */
	uint32_t subframe_bps_mid_side[2];                /* the effective bits per sample of the mid-side input signal (stream bps - wasted bits + 0/1) */
	FLAC__int32 *residual_workspace[FLAC__MAX_CHANNELS][2]; /* each channel has a candidate and best workspace where the subframe residual signals will be stored */
	FLAC__int32 *residual_workspace_mid_side[2][2];
	FLAC__Subframe subframe_workspace[FLAC__MAX_CHANNELS][2];
	FLAC__Subframe subframe_workspace_mid_side[2][2];
	FLAC__Subframe *subframe_workspace_ptr[FLAC__MAX_CHANNELS][2];
	FLAC__Subframe *subframe_workspace_ptr_mid_side[2][2];
	FLAC__EntropyCodingMethod_PartitionedRiceContents partitioned_rice_contents_workspace[FLAC__MAX_CHANNELS][2];
	FLAC__EntropyCodingMethod_PartitionedRiceContents partitioned_rice_contents_workspace_mid_side[FLAC__MAX_CHANNELS][2];
	FLAC__EntropyCodingMethod_PartitionedRiceContents *partitioned_rice_contents_workspace_ptr[FLAC__MAX_CHANNELS][2];
	FLAC__EntropyCodingMethod_PartitionedRiceContents *partitioned_rice_contents_workspace_ptr_mid_side[FLAC__MAX_CHANNELS][2];
	uint32_t best_subframe[FLAC__MAX_CHANNELS];       /* index (0 or 1) into 2nd dimension of the above workspaces */
	uint32_t best_subframe_mid_side[2];
	uint32_t best_subframe_bits[FLAC__MAX_CHANNELS];  /* size in bits of the best subframe for each channel */
	uint32_t best_subframe_bits_mid_side[2];
	FLAC__uint64 *abs_residual_partition_sums;        /* workspace where the sum of abs(candidate residual) for each partition is stored */
	uint32_t *raw_bits_per_partition;                 /* workspace where the sum of silog2(candidate residual) for each partition is stored */
	FLAC__BitWriter *frame;                           /* the current frame being worked on */
	uint32_t current_frame_number;
	/* unaligned (original) pointers to allocated data */
	FLAC__int32 *integer_signal_unaligned[FLAC__MAX_CHANNELS];
	FLAC__int32 *integer_signal_mid_side_unaligned[2];
	FLAC__int64 *integer_signal_33bit_side_unaligned;
#ifndef FLAC__INTEGER_ONLY_LIBRARY
	FLAC__real *windowed_signal_unaligned;
#endif
	FLAC__int32 *residual_workspace_unaligned[FLAC__MAX_CHANNELS][2];
	FLAC__int32 *residual_workspace_mid_side_unaligned[2][2];
	FLAC__uint64 *abs_residual_partition_sums_unaligned;
	uint32_t *raw_bits_per_partition_unaligned;
	/*
	 * These fields have been moved here from private function local
	 * declarations merely to save stack space during encoding.
	 */
#ifndef FLAC__INTEGER_ONLY_LIBRARY
	FLAC__real lp_coeff[FLAC__MAX_LPC_ORDER][FLAC__MAX_LPC_ORDER]; /* from process_subframe_() */
#endif
	FLAC__EntropyCodingMethod_PartitionedRiceContents partitioned_rice_contents_extra[2]; /* from find_best_partition_order_() */
	FLAC__bool disable_constant_subframes;
#ifdef FLAC__USE_THREADS
	FLAC__mtx_t mutex_this_task;      /* To lock whole threadtask */
	FLAC__cnd_t cond_task_done;
	FLAC__bool task_done;
	FLAC__bool returnvalue;
#endif
} FLAC__StreamEncoderThreadTask;

typedef struct FLAC__StreamEncoderPrivate {
	FLAC__StreamEncoderThreadTask * threadtask[FLAC__STREAM_ENCODER_MAX_THREADTASKS];
#ifdef FLAC__USE_THREADS
	FLAC__thrd_t thread[FLAC__STREAM_ENCODER_MAX_THREADS];
#endif
	uint32_t input_capacity;                          /* current size (in samples) of the signal and residual buffers */
#ifndef FLAC__INTEGER_ONLY_LIBRARY
	FLAC__real *window[FLAC__MAX_APODIZATION_FUNCTIONS]; /* the pre-computed floating-point window for each apodization function */
	FLAC__real *window_unaligned[FLAC__MAX_APODIZATION_FUNCTIONS];
#endif
	FLAC__StreamMetadata streaminfo;                  /* scratchpad for STREAMINFO as it is built */
	FLAC__StreamMetadata_SeekTable *seek_table;       /* pointer into encoder->protected_->metadata_ where the seek table is */
	uint32_t current_sample_number;
	uint32_t current_frame_number;
	FLAC__MD5Context md5context;
	FLAC__CPUInfo cpuinfo;
	void (*local_precompute_partition_info_sums)(const FLAC__int32 residual[], FLAC__uint64 abs_residual_partition_sums[], uint32_t residual_samples, uint32_t predictor_order, uint32_t min_partition_order, uint32_t max_partition_order, uint32_t bps);
#ifndef FLAC__INTEGER_ONLY_LIBRARY
	uint32_t (*local_fixed_compute_best_predictor)(const FLAC__int32 data[], uint32_t data_len, float residual_bits_per_sample[FLAC__MAX_FIXED_ORDER+1]);
	uint32_t (*local_fixed_compute_best_predictor_wide)(const FLAC__int32 data[], uint32_t data_len, float residual_bits_per_sample[FLAC__MAX_FIXED_ORDER+1]);
	uint32_t (*local_fixed_compute_best_predictor_limit_residual)(const FLAC__int32 data[], uint32_t data_len, float residual_bits_per_sample[FLAC__MAX_FIXED_ORDER+1]);
#else
	uint32_t (*local_fixed_compute_best_predictor)(const FLAC__int32 data[], uint32_t data_len, FLAC__fixedpoint residual_bits_per_sample[FLAC__MAX_FIXED_ORDER+1]);
	uint32_t (*local_fixed_compute_best_predictor_wide)(const FLAC__int32 data[], uint32_t data_len, FLAC__fixedpoint residual_bits_per_sample[FLAC__MAX_FIXED_ORDER+1]);
	uint32_t (*local_fixed_compute_best_predictor_limit_residual)(const FLAC__int32 data[], uint32_t data_len, FLAC__fixedpoint residual_bits_per_sample[FLAC__MAX_FIXED_ORDER+1]);
#endif
#ifndef FLAC__INTEGER_ONLY_LIBRARY
	void (*local_lpc_compute_autocorrelation)(const FLAC__real data[], uint32_t data_len, uint32_t lag, double autoc[]);
	void (*local_lpc_compute_residual_from_qlp_coefficients)(const FLAC__int32 *data, uint32_t data_len, const FLAC__int32 qlp_coeff[], uint32_t order, int lp_quantization, FLAC__int32 residual[]);
	void (*local_lpc_compute_residual_from_qlp_coefficients_64bit)(const FLAC__int32 *data, uint32_t data_len, const FLAC__int32 qlp_coeff[], uint32_t order, int lp_quantization, FLAC__int32 residual[]);
	void (*local_lpc_compute_residual_from_qlp_coefficients_16bit)(const FLAC__int32 *data, uint32_t data_len, const FLAC__int32 qlp_coeff[], uint32_t order, int lp_quantization, FLAC__int32 residual[]);
#endif
	FLAC__bool disable_mmx;
	FLAC__bool disable_sse2;
	FLAC__bool disable_ssse3;
	FLAC__bool disable_sse41;
	FLAC__bool disable_sse42;
	FLAC__bool disable_avx2;
	FLAC__bool disable_fma;
	FLAC__bool disable_constant_subframes;
	FLAC__bool disable_fixed_subframes;
	FLAC__bool disable_verbatim_subframes;
	FLAC__bool is_ogg;
	FLAC__StreamEncoderReadCallback read_callback; /* currently only needed for Ogg FLAC */
	FLAC__StreamEncoderSeekCallback seek_callback;
	FLAC__StreamEncoderTellCallback tell_callback;
	FLAC__StreamEncoderWriteCallback write_callback;
	FLAC__StreamEncoderMetadataCallback metadata_callback;
	FLAC__StreamEncoderProgressCallback progress_callback;
	void *client_data;
	uint32_t first_seekpoint_to_check;
	FILE *file;                            /* only used when encoding to a file */
	FLAC__uint64 bytes_written;
	FLAC__uint64 samples_written;
	uint32_t frames_written;
	uint32_t total_frames_estimate;
	/*
	 * The data for the verify section
	 */
	struct {
		FLAC__StreamDecoder *decoder;
		EncoderStateHint state_hint;
		FLAC__bool needs_magic_hack;
		verify_input_fifo input_fifo;
		verify_output output;
		struct {
			FLAC__uint64 absolute_sample;
			uint32_t frame_number;
			uint32_t channel;
			uint32_t sample;
			FLAC__int32 expected;
			FLAC__int32 got;
		} error_stats;
	} verify;
	FLAC__bool is_being_deleted; /* if true, call to ..._finish() from ..._delete() will not call the callbacks */
	uint32_t num_threadtasks;
	/*
	 * A metadata object owned by the checkpoint resume code (the restored
	 * SEEKTABLE).  It has to outlive the resumed encoder because the encoder
	 * keeps a pointer into it and rewrites it during finish.  Normal encoders
	 * leave this NULL.
	 */
	FLAC__StreamMetadata *checkpoint_owned_seektable;
	/*
	 * Set once the stream header and metadata have already been emitted by a
	 * previous encoder and only the audio frame stream has to continue.
	 */
	FLAC__bool checkpoint_resumed;
#ifdef FLAC__USE_THREADS
	uint32_t num_created_threads;
	uint32_t next_thread; /* This is the next thread that needs start, or needs to finish and be restarted */
	uint32_t num_started_threadtasks;
	uint32_t num_available_threadtasks; /* Number of threadtasks that are available to work on */
	uint32_t num_running_threads;
	uint32_t next_threadtask; /* Next threadtask that is available to work on */
	FLAC__mtx_t mutex_md5_fifo;
	FLAC__mtx_t mutex_work_queue; /* To lock work related variables in this struct */
	FLAC__cnd_t cond_md5_emptied; /* To signal to main thread that MD5 queue has been emptied */
	FLAC__cnd_t cond_work_available; /* To signal to threads that work is available */
	FLAC__cnd_t cond_wake_up_thread; /* To signal that one sleeping thread can wake up */
	FLAC__bool md5_active;
	FLAC__bool finish_work_threads;
	int32_t overcommitted_indicator;
	verify_input_fifo md5_fifo;
#endif
} FLAC__StreamEncoderPrivate;

/*
 * Extra initialization information for resuming an existing stream.  The
 * stream header and metadata have already been written by the encoder that
 * produced the checkpoint, so they must not be written again.
 */
typedef struct {
	FLAC__uint64 streaminfo_offset;
	FLAC__uint64 seektable_offset;
	FLAC__uint64 audio_offset;
} FLAC__StreamEncoderCheckpointResume;

/*
 * Initialize like FLAC__stream_encoder_init_stream() but skip the stream
 * header and metadata, continuing an existing stream at the given offsets.
 */
FLAC__StreamEncoderInitStatus FLAC__stream_encoder_init_stream_checkpoint_(
	FLAC__StreamEncoder *encoder,
	FLAC__StreamEncoderWriteCallback write_callback,
	FLAC__StreamEncoderSeekCallback seek_callback,
	FLAC__StreamEncoderTellCallback tell_callback,
	FLAC__StreamEncoderMetadataCallback metadata_callback,
	void *client_data,
	const FLAC__StreamEncoderCheckpointResume *resume
);

#endif
