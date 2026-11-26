/**
 * \file mlt_conform.c
 * \brief frame-rate and audio-rate conforming producer
 *
 * Copyright (C) 2026 Meltytech, LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "mlt_conform.h"
#include "mlt_audio.h"
#include "mlt_frame.h"
#include "mlt_log.h"
#include "mlt_profile.h"
#include "mlt_properties.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Property used to opt a frame into propagating a get_audio error instead of
 * falling back to silence. It is only set on frames owned by the conform
 * producer and on the source frames read by it, so other producers retain the
 * historical behaviour. */
#define CONFORM_AUDIO_ERROR "conform.audio_error"
#define CONFORM_STATE "conform.state"
#define CONFORM_FRAME "conform.frame"

/** Private data of the conform producer. */
typedef struct
{
    mlt_producer source; /**< source producer, one reference held */
    mlt_profile profile; /**< owned copy of the output profile */
    int source_rate;
    int output_rate;
    int channels;
    int src_num;
    int src_den;
    int out_num;
    int out_den;
    int64_t source_length;
    int64_t output_length;
    int64_t source_samples;
    float src_fps;
    float out_fps;
} conform_data;

/** State copied onto every delivered frame.
 *
 * It owns its own reference to the source, so a frame keeps working after the
 * conform producer and the caller's source reference are closed.
 */
typedef struct
{
    mlt_producer source;
    int source_rate;
    int output_rate;
    int channels;
    int src_num;
    int src_den;
    int out_num;
    int out_den;
    int64_t source_length;
    int64_t output_length;
    int64_t source_samples;
    float src_fps;
    float out_fps;
    mlt_position position;
} conform_frame;

/* Copy an output profile, including a private copy of its description. */
static mlt_profile conform_profile_clone(mlt_profile profile)
{
    if (!profile)
        return NULL;
    mlt_profile clone = calloc(1, sizeof(*clone));
    if (!clone)
        return NULL;
    memcpy(clone, profile, sizeof(*clone));
    clone->description = profile->description ? strdup(profile->description) : NULL;
    return clone;
}

/* ---- small checked-integer helpers ---- */
static int mul_i64(int64_t a, int64_t b, int64_t *result)
{
    if (a < 0 || b < 0)
        return 0;
    if (a == 0 || b == 0) {
        *result = 0;
        return 1;
    }
    if (b > INT64_MAX / a)
        return 0;
    *result = a * b;
    return 1;
}

static int64_t calc_samples(float fps, int frequency, int64_t position)
{
    return mlt_audio_calculate_samples_to_position(fps, frequency, position);
}

static double ratio_ok(int64_t count, int frequency, float fps)
{
    if (count <= 0 || frequency <= 0 || !(fps > 0.0f))
        return 0.0;
    return (double) count * (double) frequency / (double) fps;
}

/* ---- frame access ---- */

static void restore_source(mlt_producer source, mlt_position position, double speed)
{
    mlt_producer_seek(source, position);
    mlt_producer_set_speed(source, speed);
}

static void conform_state_release(void *data)
{
    conform_frame *state = data;
    if (state) {
        if (state->source)
            mlt_producer_close(state->source);
        free(state);
    }
}

static int64_t conform_video_index(const conform_frame *state, int64_t position)
{
    /* floor(position * source_fps / output_fps), computed exactly. The
     * constructor guarantees these products fit in int64_t. */
    int64_t numerator = position * (int64_t) state->src_num;
    numerator *= (int64_t) state->out_den;
    return numerator / ((int64_t) state->src_den * (int64_t) state->out_num);
}

/* Return the source frame index that contains global source sample q. */
static int64_t conform_frame_for_sample(const conform_frame *state, int64_t q)
{
    int64_t length = state->source_length;
    if (length <= 0)
        return 0;
    if (q < 0)
        q = 0;

    int64_t frame = 0;
    if (state->source_rate > 0 && state->src_fps > 0.0f)
        frame = (int64_t) ((double) q * (double) state->src_fps / (double) state->source_rate);
    if (frame < 0)
        frame = 0;
    if (frame >= length)
        frame = length - 1;

    while (frame > 0 && calc_samples(state->src_fps, state->source_rate, frame) > q)
        frame--;
    while (frame + 1 < length
           && calc_samples(state->src_fps, state->source_rate, frame + 1) <= q)
        frame++;
    return frame;
}

static int conform_get_audio(mlt_frame self,
                             void **buffer,
                             mlt_audio_format *format,
                             int *frequency,
                             int *channels,
                             int *samples)
{
    conform_frame *state = mlt_frame_pop_audio(self);
    if (!state)
        return 1;

    int error = 1;
    float *output = NULL;
    float *source_samples = NULL;
    size_t output_bytes = 0;
    int64_t sample_count = 0;
    mlt_producer source = state->source;
    mlt_position saved_position = mlt_producer_position(source);
    double saved_speed = mlt_producer_get_speed(source);

    int64_t start = calc_samples(state->out_fps, state->output_rate, state->position);
    int64_t end = calc_samples(state->out_fps, state->output_rate, (int64_t) state->position + 1);
    sample_count = end - start;
    if (sample_count < 0)
        sample_count = 0;

    if (sample_count > INT_MAX || state->channels <= 0)
        goto done;
    if (sample_count > (int64_t) INT_MAX / (int64_t) state->channels / 4)
        goto done;
    output_bytes = (size_t) sample_count * (size_t) state->channels * sizeof(float);
    output = mlt_pool_alloc(output_bytes ? (int) output_bytes : (int) sizeof(float));
    if (!output)
        goto done;
    memset(output, 0, output_bytes ? output_bytes : sizeof(float));

    if (sample_count > 0) {
        int64_t total = state->source_samples;
        int64_t q_first
            = (start * (int64_t) state->source_rate) / (int64_t) state->output_rate;
        int64_t q_last_unclamped
            = (((end - 1) * (int64_t) state->source_rate) / (int64_t) state->output_rate) + 1;

        if (q_first < total) {
            int64_t q_last = q_last_unclamped < total ? q_last_unclamped : total - 1;
            if (q_last >= q_first) {
                int64_t span = q_last - q_first + 1;
                if (span > (int64_t) INT_MAX / (int64_t) state->channels / 4)
                    goto done;
                size_t span_bytes = (size_t) span * (size_t) state->channels * sizeof(float);
                source_samples = malloc(span_bytes);
                if (!source_samples)
                    goto done;
                memset(source_samples, 0, span_bytes);

                /* Fill the needed span of global source samples from the real
                 * source frames, fetching each contributing frame lazily. */
                int64_t frame = conform_frame_for_sample(state, q_first);
                int64_t frame_start
                    = calc_samples(state->src_fps, state->source_rate, frame);
                while (frame < state->source_length && frame_start <= q_last) {
                    int64_t frame_end
                        = calc_samples(state->src_fps, state->source_rate, frame + 1);
                    int64_t count = frame_end - frame_start;

                    if (count > 0 && frame_end > q_first) {
                        mlt_producer_seek(source, (mlt_position) frame);
                        mlt_frame source_frame = NULL;
                        if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(source),
                                                  &source_frame,
                                                  0)
                                || !source_frame) {
                            if (source_frame)
                                mlt_frame_close(source_frame);
                            goto done;
                        }

                        /* Ask the source frame to propagate audio errors. */
                        mlt_properties_set_int(MLT_FRAME_PROPERTIES(source_frame),
                                               CONFORM_AUDIO_ERROR,
                                               1);
                        void *audio = NULL;
                        mlt_audio_format audio_format = mlt_audio_f32le;
                        int audio_rate = 0;
                        int audio_channels = 0;
                        int audio_count = 0;
                        int audio_error = mlt_frame_get_audio(source_frame,
                                                              &audio,
                                                              &audio_format,
                                                              &audio_rate,
                                                              &audio_channels,
                                                              &audio_count);
                        if (audio_error || !audio || audio_format != mlt_audio_f32le
                            || audio_rate != state->source_rate
                            || audio_channels != state->channels
                            || (int64_t) audio_count != count) {
                            mlt_frame_close(source_frame);
                            goto done;
                        }

                        int64_t first = frame_start > q_first ? frame_start : q_first;
                        int64_t last
                            = (frame_start + count - 1) < q_last ? (frame_start + count - 1) : q_last;
                        const float *src = audio;
                        for (int64_t g = first; g <= last; g++) {
                            int64_t from = (g - frame_start) * state->channels;
                            int64_t to = (g - q_first) * state->channels;
                            for (int c = 0; c < state->channels; c++)
                                source_samples[to + c] = src[from + c];
                        }
                        mlt_frame_close(source_frame);
                    }

                    frame_start = frame_end;
                    frame++;
                }

                /* Linear interpolation with a fractional phase. */
                for (int64_t i = 0; i < sample_count; i++) {
                    int64_t k = start + i;
                    int64_t numerator = k * (int64_t) state->source_rate;
                    int64_t q = numerator / (int64_t) state->output_rate;
                    int64_t remainder = numerator % (int64_t) state->output_rate;

                    if (q >= total)
                        continue; /* beyond the source's audio duration */

                    float *dst = output + i * state->channels;
                    const float *a = source_samples + (q - q_first) * state->channels;
                    const float *b = NULL;
                    if (q + 1 < total)
                        b = source_samples + (q + 1 - q_first) * state->channels;
                    double fraction = (double) remainder / (double) state->output_rate;
                    for (int c = 0; c < state->channels; c++) {
                        if (b)
                            dst[c] = (float) (a[c] + ((double) b[c] - a[c]) * fraction);
                        else
                            dst[c] = (float) (a[c] * (1.0 - fraction));
                    }
                }
            }
        }
    }

    *buffer = output;
    *format = mlt_audio_f32le;
    *frequency = state->output_rate;
    *channels = state->channels;
    *samples = (int) sample_count;
    mlt_frame_set_audio(self,
                        output,
                        mlt_audio_f32le,
                        (int) output_bytes,
                        (mlt_destructor) mlt_pool_release);
    output = NULL; /* owned by the frame now */
    error = 0;

done:
    if (output)
        mlt_pool_release(output);
    free(source_samples);
    restore_source(source, saved_position, saved_speed);
    if (error) {
        /* Re-publish the callback so a repeated read fails the same way
         * instead of falling back to the framework's silence. */
        mlt_frame_push_audio(self, state);
        mlt_frame_push_audio(self, conform_get_audio);
        if (buffer)
            *buffer = NULL;
        if (samples)
            *samples = 0;
        if (format)
            *format = mlt_audio_f32le;
        if (frequency)
            *frequency = state->output_rate;
        if (channels)
            *channels = state->channels;
    }
    return error;
}

static int conform_get_image(mlt_frame self,
                             uint8_t **buffer,
                             mlt_image_format *format,
                             int *width,
                             int *height,
                             int writable)
{
    mlt_properties properties = MLT_FRAME_PROPERTIES(self);
    conform_frame *state = mlt_properties_get_data(properties, CONFORM_STATE, NULL);
    if (!state)
        return 1;

    mlt_producer source = state->source;
    mlt_position saved_position = mlt_producer_position(source);
    double saved_speed = mlt_producer_get_speed(source);
    int error = 1;
    mlt_frame source_frame = NULL;

    int64_t source_index = conform_video_index(state, state->position);
    if (source_index < 0)
        source_index = 0;
    if (source_index >= state->source_length)
        source_index = state->source_length - 1;

    mlt_producer_seek(source, (mlt_position) source_index);
    if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(source), &source_frame, 0) == 0
        && source_frame) {
        uint8_t *image = NULL;
        mlt_image_format image_format = *format;
        int image_width = *width;
        int image_height = *height;
        int image_error = mlt_frame_get_image(source_frame,
                                              &image,
                                              &image_format,
                                              &image_width,
                                              &image_height,
                                              writable);
        if (!image_error && image && image_width > 0 && image_height > 0) {
            /* Keep the source frame alive on the output frame: the image
             * buffer belongs to it. The output frame's image pointer is a
             * borrowed alias, deliberately without a destructor. */
            mlt_properties_set_data(properties,
                                    CONFORM_FRAME,
                                    source_frame,
                                    0,
                                    (mlt_destructor) mlt_frame_close,
                                    NULL);
            source_frame = NULL;
            mlt_frame_set_image(self, image, 0, NULL);
            *buffer = image;
            *format = image_format;
            *width = image_width;
            *height = image_height;
            error = 0;
        }
    }

    if (source_frame)
        mlt_frame_close(source_frame);
    restore_source(source, saved_position, saved_speed);
    return error;
}

static int conform_get_frame(mlt_producer producer, mlt_frame_ptr frame, int index)
{
    conform_data *data = producer->child;
    if (!data || !frame)
        return 1;

    mlt_position position = mlt_producer_position(producer);
    if (position < 0)
        position = 0;
    if (position >= data->output_length)
        position = (mlt_position) (data->output_length - 1);

    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    if (!*frame)
        return 1;

    mlt_frame_set_position(*frame, position);
    mlt_properties properties = MLT_FRAME_PROPERTIES(*frame);

    conform_frame *state = calloc(1, sizeof(*state));
    if (!state) {
        mlt_frame_close(*frame);
        *frame = NULL;
        return 1;
    }
    state->source = data->source;
    state->source_rate = data->source_rate;
    state->output_rate = data->output_rate;
    state->channels = data->channels;
    state->src_num = data->src_num;
    state->src_den = data->src_den;
    state->out_num = data->out_num;
    state->out_den = data->out_den;
    state->source_length = data->source_length;
    state->output_length = data->output_length;
    state->source_samples = data->source_samples;
    state->src_fps = data->src_fps;
    state->out_fps = data->out_fps;
    state->position = position;

    /* Hold our own source reference for the lifetime of this frame. */
    mlt_properties_inc_ref(MLT_PRODUCER_PROPERTIES(data->source));
    mlt_properties_set_data(properties,
                            CONFORM_STATE,
                            state,
                            0,
                            conform_state_release,
                            NULL);
    mlt_properties_set_int(properties, CONFORM_AUDIO_ERROR, 1);

    /* Lazy callbacks: neither video nor audio is decoded in get_frame. */
    mlt_frame_push_get_image(*frame, conform_get_image);
    mlt_frame_push_audio(*frame, state);
    mlt_frame_push_audio(*frame, conform_get_audio);

    /* Advance the play head. Seeking explicitly (rather than using
     * mlt_producer_prepare_next()) never seeks past the last frame, so the
     * producer does not clamp its speed to zero at the end. Pausing via speed
     * zero is still honoured. */
    if (mlt_producer_get_speed(producer) != 0.0 && position + 1 < data->output_length)
        mlt_producer_seek(producer, position + 1);
    return 0;
}

static void conform_close(mlt_producer producer)
{
    conform_data *data = producer->child;
    if (data) {
        if (data->source)
            mlt_producer_close(data->source);
        if (data->profile)
            mlt_profile_close(data->profile);
        free(data);
    }
    producer->child = NULL;
    producer->close = NULL;
    mlt_producer_close(producer);
    free(producer);
}

static int conform_check_inputs(mlt_profile output_profile,
                                mlt_producer source,
                                int source_audio_rate,
                                int output_audio_rate,
                                int channels,
                                conform_data *data)
{
    if (!output_profile || !source || !data)
        return 1;
    if (source_audio_rate <= 0 || output_audio_rate <= 0 || channels <= 0)
        return 1;
    if (output_profile->frame_rate_num <= 0 || output_profile->frame_rate_den <= 0)
        return 1;

    mlt_profile source_profile = mlt_service_profile(MLT_PRODUCER_SERVICE(source));
    if (!source_profile || source_profile->frame_rate_num <= 0
        || source_profile->frame_rate_den <= 0)
        return 1;

    int64_t source_length = mlt_producer_get_length(source);
    if (source_length <= 0)
        return 1;

    /* Only plain, unedited sources are supported. */
    if (mlt_producer_is_cut(source))
        return 1;
    if (mlt_producer_get_in(source) != 0)
        return 1;
    if (mlt_producer_get_out(source) != source_length - 1)
        return 1;
    if (mlt_producer_get_speed(source) != 1.0)
        return 1;

    data->source = source;
    data->source_rate = source_audio_rate;
    data->output_rate = output_audio_rate;
    data->channels = channels;
    data->src_num = source_profile->frame_rate_num;
    data->src_den = source_profile->frame_rate_den;
    data->out_num = output_profile->frame_rate_num;
    data->out_den = output_profile->frame_rate_den;
    data->source_length = source_length;
    data->src_fps = (float) ((double) data->src_num / (double) data->src_den);
    data->out_fps = (float) ((double) data->out_num / (double) data->out_den);

    /* output_length = ceil(source_length * output_fps / source_fps) */
    int64_t numerator = 0;
    if (!mul_i64(source_length, data->out_num, &numerator)
        || !mul_i64(numerator, data->src_den, &numerator))
        return 1;
    int64_t denominator = 0;
    if (!mul_i64(data->src_num, data->out_den, &denominator) || denominator <= 0)
        return 1;
    if (numerator > INT64_MAX - (denominator - 1))
        return 1;
    int64_t output_length = (numerator + denominator - 1) / denominator;
    if (output_length <= 0 || output_length > INT_MAX)
        return 1;
    data->output_length = output_length;

    /* Reject sample counts or products the code can not represent safely. */
    double source_total = ratio_ok(source_length, source_audio_rate, data->src_fps);
    if (!(source_total > 0.0) || source_total >= 9.0e18)
        return 1;
    double output_total = ratio_ok(output_length, output_audio_rate, data->out_fps);
    if (output_total < 0.0 || output_total >= 9.0e18)
        return 1;
    /* k * source_rate must stay in int64_t for every output sample. */
    if (output_total > 0.0
        && output_total * (double) (source_audio_rate > output_audio_rate ? source_audio_rate
                                                                         : output_audio_rate)
               >= 9.0e18)
        return 1;

    /* Video index products: (output_length - 1) * src_num * out_den */
    int64_t index_numerator = 0;
    if (!mul_i64(output_length - 1, data->src_num, &index_numerator)
        || !mul_i64(index_numerator, data->out_den, &index_numerator))
        return 1;
    data->source_samples = calc_samples(data->src_fps,
                                        data->source_rate,
                                        data->source_length);

    /* Per-frame window sizes must remain allocatable (int sized buffers). */
    double max_output_window = (double) output_audio_rate / (double) data->out_fps + 3.0;
    double max_source_span
        = max_output_window * ((double) source_audio_rate / (double) output_audio_rate) + 3.0;
    if (!(max_output_window > 0.0) || !(max_source_span > 0.0)
        || max_output_window * (double) channels * 4.0 > (double) INT_MAX
        || max_source_span * (double) channels * 4.0 > (double) INT_MAX)
        return 1;

    return 0;
}

mlt_producer mlt_producer_conform(mlt_profile output_profile,
                                  mlt_producer source,
                                  int source_audio_rate,
                                  int output_audio_rate,
                                  int channels)
{
    conform_data *data = calloc(1, sizeof(*data));
    if (!data)
        return NULL;

    if (conform_check_inputs(output_profile,
                             source,
                             source_audio_rate,
                             output_audio_rate,
                             channels,
                             data)) {
        free(data);
        return NULL;
    }

    data->profile = conform_profile_clone(output_profile);
    if (!data->profile) {
        free(data);
        return NULL;
    }

    mlt_producer producer = mlt_producer_new(data->profile);
    if (!producer) {
        mlt_profile_close(data->profile);
        free(data);
        return NULL;
    }

    mlt_properties properties = MLT_PRODUCER_PROPERTIES(producer);
    mlt_properties_set(properties, "resource", "<conform>");
    mlt_properties_set_int(properties, "conform.source_audio_rate", source_audio_rate);
    mlt_properties_set_int(properties, "conform.output_audio_rate", output_audio_rate);
    mlt_properties_set_int(properties, "conform.channels", channels);
    mlt_properties_set_position(properties, "length", data->output_length);
    mlt_producer_set_in_and_out(producer, 0, data->output_length - 1);
    mlt_producer_set_speed(producer, 1.0);

    /* One reference on the source, released when the conform producer closes. */
    mlt_properties_inc_ref(MLT_PRODUCER_PROPERTIES(source));

    producer->child = data;
    producer->get_frame = conform_get_frame;
    producer->close = (mlt_destructor) conform_close;

    return producer;
}
