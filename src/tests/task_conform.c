/*
 * task_conform.c -- regression tests for the conform producer.
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

#include <framework/mlt.h>
#include "../task_support/check.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * A deterministic C producer used as the conform source.
 *
 * Video is 16x8 yuv422 with luma 40 + source_frame_index so the frame number
 * is visible in the image. PCM is interleaved f32le where the sample with
 * absolute source-sample index s has value s + 0.25 * channel. Its per-frame
 * sample window is derived from the library's cumulative boundary function,
 * so it intentionally varies when the frame rate does not divide the sample
 * rate.
 * ------------------------------------------------------------------------- */

typedef struct
{
    int rate;
    int channels;
    int length;
    int created;
    int destroyed;
    int audio_calls;
    int fail_from; /* fail audio for source frames >= fail_from; -1 for never */
    int bad_format;
    int bad_rate;
    int bad_channels;
    int bad_samples;
} test_state;

typedef struct
{
    test_state *state;
} test_token;

static void test_token_release(void *p)
{
    test_token *token = p;
    token->state->destroyed++;
    free(token);
}

static int test_image(mlt_frame frame,
                      uint8_t **buffer,
                      mlt_image_format *format,
                      int *width,
                      int *height,
                      int writable)
{
    (void) writable;
    int position = mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "test.position");
    uint8_t *image = mlt_pool_alloc(256);
    if (!image)
        return 1;
    for (int i = 0; i < 256; i += 2) {
        image[i] = (uint8_t) (40 + position);
        image[i + 1] = 128;
    }
    mlt_frame_set_image(frame, image, 256, mlt_pool_release);
    *buffer = image;
    *format = mlt_image_yuv422;
    *width = 16;
    *height = 8;
    return 0;
}

static int test_audio(mlt_frame frame,
                      void **buffer,
                      mlt_audio_format *format,
                      int *rate,
                      int *channels,
                      int *samples)
{
    test_state *state = mlt_properties_get_data(MLT_FRAME_PROPERTIES(frame), "test.state", NULL);
    int64_t start = mlt_properties_get_int64(MLT_FRAME_PROPERTIES(frame), "test.start");
    int count = mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "test.count");
    int position = mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "test.position");
    state->audio_calls++;

    if (state->fail_from >= 0 && position >= state->fail_from) {
        *buffer = NULL;
        return 1;
    }

    int reported_channels = state->channels + state->bad_channels;
    int reported_count = count + state->bad_samples;
    if (reported_count < 0)
        reported_count = 0;
    size_t bytes = (size_t) reported_count * (size_t) reported_channels * sizeof(float);
    float *pcm = mlt_pool_alloc(bytes ? (int) bytes : (int) sizeof(float));
    if (!pcm) {
        *buffer = NULL;
        return 1;
    }
    memset(pcm, 0, bytes ? bytes : sizeof(float));
    for (int i = 0; i < count; i++) {
        for (int c = 0; c < state->channels; c++) {
            int64_t index = start + i;
            pcm[(size_t) i * reported_channels + c] = (float) index + 0.25f * c;
        }
    }
    *buffer = pcm;
    *format = state->bad_format ? mlt_audio_s16 : mlt_audio_f32le;
    *rate = state->rate + state->bad_rate;
    *channels = reported_channels;
    *samples = reported_count;
    mlt_frame_set_audio(frame, pcm, *format, (int) bytes, mlt_pool_release);
    return 0;
}

static int test_frame(mlt_producer producer, mlt_frame_ptr frame, int track)
{
    (void) track;
    test_state *state = mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(producer),
                                                "test.state",
                                                NULL);
    int position = mlt_producer_position(producer);
    if (!state)
        return 1;

    float fps = (float) mlt_profile_fps(mlt_service_profile(MLT_PRODUCER_SERVICE(producer)));
    int64_t start = mlt_audio_calculate_samples_to_position(fps, state->rate, position);
    int64_t end = mlt_audio_calculate_samples_to_position(fps, state->rate, position + 1);

    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    if (!*frame)
        return 1;
    mlt_frame_set_position(*frame, position);

    mlt_properties properties = MLT_FRAME_PROPERTIES(*frame);
    mlt_properties_set_data(properties, "test.state", state, 0, NULL, NULL);
    mlt_properties_set_int64(properties, "test.start", start);
    mlt_properties_set_int(properties, "test.count", (int) (end - start));
    mlt_properties_set_int(properties, "test.position", position);

    test_token *token = malloc(sizeof(*token));
    if (!token) {
        mlt_frame_close(*frame);
        *frame = NULL;
        return 1;
    }
    token->state = state;
    state->created++;
    mlt_properties_set_data(properties, "test.token", token, 0, test_token_release, NULL);

    mlt_frame_push_get_image(*frame, test_image);
    mlt_frame_push_audio(*frame, test_audio);
    mlt_producer_prepare_next(producer);
    return 0;
}

/* Shared factory init is idempotent. */
static void test_init(void)
{
    static int initialized;
    if (!initialized) {
        mlt_factory_init(".task-build/empty-modules");
        initialized = 1;
    }
}

static mlt_profile test_profile(int num, int den)
{
    test_init();
    mlt_profile profile = calloc(1, sizeof(*profile));
    if (profile) {
        profile->frame_rate_num = num;
        profile->frame_rate_den = den;
        profile->width = 16;
        profile->height = 8;
        profile->progressive = 1;
        profile->sample_aspect_num = profile->sample_aspect_den = 1;
        profile->display_aspect_num = 2;
        profile->display_aspect_den = 1;
        profile->colorspace = 601;
    }
    return profile;
}

static mlt_producer test_source(mlt_profile profile, test_state *state)
{
    mlt_producer producer = mlt_producer_new(profile);
    if (!producer)
        return NULL;
    producer->get_frame = test_frame;
    mlt_properties_set_data(MLT_PRODUCER_PROPERTIES(producer), "test.state", state, 0, NULL, NULL);
    mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(producer), "length", state->length);
    mlt_producer_set_in_and_out(producer, 0, state->length - 1);
    mlt_producer_set_speed(producer, 1.0);
    return producer;
}

/* ---------------------------------------------------------------------------
 * Reference model for the expected output.
 * ------------------------------------------------------------------------- */

/* Source sample value for global source sample s and channel c. */
static double test_source_value(int64_t s, int channel)
{
    return (double) s + 0.25 * (double) channel;
}

static int64_t test_window_start(float fps, int rate, int64_t position)
{
    return mlt_audio_calculate_samples_to_position(fps, rate, position);
}

/* Total source samples represented by the material. */
static int64_t test_source_total(int64_t length, int rate, float fps)
{
    return mlt_audio_calculate_samples_to_position(fps, rate, length);
}

/* Expected output sample k for channel c, with zero past the source end and a
 * zero neighbor past the end. */
static double test_expected_sample(int64_t k, int source_rate, int output_rate, int64_t total,
                                   int channel)
{
    int64_t numerator = k * (int64_t) source_rate;
    int64_t q = numerator / (int64_t) output_rate;
    int64_t remainder = numerator % (int64_t) output_rate;
    if (q >= total || q < 0)
        return 0.0;
    double a = test_source_value(q, channel);
    double b = (q + 1 < total) ? test_source_value(q + 1, channel) : 0.0;
    return a + (b - a) * ((double) remainder / (double) output_rate);
}

/* Video source index for output frame n. */
static int test_video_index(int n, int src_num, int src_den, int out_num, int out_den)
{
    return (int) ((int64_t) n * src_num * out_den / ((int64_t) src_den * out_num));
}

/* ---------------------------------------------------------------------------
 * Convenience readers.
 * ------------------------------------------------------------------------- */

static int read_output_frame(mlt_producer producer, int64_t position, mlt_frame *frame)
{
    mlt_producer_seek(producer, position);
    *frame = NULL;
    return mlt_service_get_frame(MLT_PRODUCER_SERVICE(producer), frame, 0);
}

static int read_audio(mlt_frame frame,
                      void **buffer,
                      int *rate,
                      int *channels,
                      int *samples)
{
    mlt_audio_format format = mlt_audio_f32le;
    *buffer = NULL;
    *rate = 0;
    *channels = 0;
    *samples = 0;
    return mlt_frame_get_audio(frame, buffer, &format, rate, channels, samples);
}

static int read_image_y(mlt_frame frame)
{
    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int width = 16;
    int height = 8;
    if (mlt_frame_get_image(frame, &image, &format, &width, &height, 0) || !image)
        return -1;
    if (width != 16 || height != 8)
        return -1;
    return image[0];
}

#define CHECK_SAMPLE(got, expected) \
    TASK_CHECK(fabs((double) (got) - (double) (expected)) < 1.0e-4)

/* ---------------------------------------------------------------------------
 * clock group
 * ------------------------------------------------------------------------- */
static int case_same_clock(void)
{
    test_state state = {200, 2, 3, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(25, 1);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);
    mlt_producer conform = mlt_producer_conform(output_profile, source, 200, 200, 2);
    TASK_CHECK(conform);
    TASK_CHECK(mlt_producer_get_length(conform) == 3);
    TASK_CHECK(mlt_producer_get_fps(conform) == 25.0);

    float fps = 25.0f;
    int64_t total = test_source_total(3, 200, fps);

    for (int n = 0; n < 3; n++) {
        mlt_frame frame = NULL;
        TASK_CHECK(!read_output_frame(conform, n, &frame) && frame);
        TASK_CHECK(mlt_frame_get_position(frame) == n);
        TASK_CHECK(read_image_y(frame) == 40 + n);

        void *audio = NULL;
        int rate = 0, channels = 0, samples = 0;
        TASK_CHECK(!read_audio(frame, &audio, &rate, &channels, &samples));
        TASK_CHECK(audio && rate == 200 && channels == 2);
        int64_t start = test_window_start(fps, 200, n);
        TASK_CHECK(samples == (int) (test_window_start(fps, 200, n + 1) - start));
        CHECK_SAMPLE(((float *) audio)[0], test_expected_sample(start, 200, 200, total, 0));
        CHECK_SAMPLE(((float *) audio)[1], test_expected_sample(start, 200, 200, total, 1));
        CHECK_SAMPLE(((float *) audio)[(samples - 1) * channels],
                     test_expected_sample(start + samples - 1, 200, 200, total, 0));

        /* Repeated reads are stable. */
        void *again = NULL;
        int arate = 0, ach = 0, acount = 0;
        TASK_CHECK(!read_audio(frame, &again, &arate, &ach, &acount));
        TASK_CHECK(again == audio && acount == samples);
        TASK_CHECK(memcmp(audio, again, (size_t) samples * channels * sizeof(float)) == 0);

        mlt_frame_close(frame);
    }

    mlt_producer_close(conform);
    mlt_producer_close(source);
    TASK_CHECK(state.created == state.destroyed);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

static int case_clock_25_to_30(void)
{
    test_state state = {200, 2, 4, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(30, 1);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);
    mlt_producer conform = mlt_producer_conform(output_profile, source, 200, 300, 2);
    TASK_CHECK(conform);
    /* ceil(4 * 30 / 25) == 5 */
    TASK_CHECK(mlt_producer_get_length(conform) == 5);

    float src_fps = 25.0f;
    float out_fps = 30.0f;
    int64_t total = test_source_total(4, 200, src_fps);

    int expected_video[5] = {0, 0, 1, 2, 3};
    for (int n = 0; n < 5; n++) {
        mlt_frame frame = NULL;
        TASK_CHECK(!read_output_frame(conform, n, &frame) && frame);
        TASK_CHECK(mlt_frame_get_position(frame) == n);
        TASK_CHECK(read_image_y(frame) == 40 + expected_video[n]);

        void *audio = NULL;
        int rate = 0, channels = 0, samples = 0;
        TASK_CHECK(!read_audio(frame, &audio, &rate, &channels, &samples));
        TASK_CHECK(audio && rate == 300 && channels == 2);
        int64_t start = test_window_start(out_fps, 300, n);
        int64_t end = test_window_start(out_fps, 300, n + 1);
        TASK_CHECK(samples == (int) (end - start));
        TASK_CHECK(samples == 10);

        if (n == 1) {
            /* start == 10 maps to source samples [20/3, 38/3), crossing source
             * frames 0 and 1. The first value is the 2/3 interpolation of
             * source samples 6 and 7, not the start of source frame 0. */
            CHECK_SAMPLE(((float *) audio)[0],
                         test_expected_sample(start, 200, 300, total, 0));
            CHECK_SAMPLE(((float *) audio)[0], 6.0 + (7.0 - 6.0) * 2.0 / 3.0);
            CHECK_SAMPLE(((float *) audio)[1], 6.25 + (7.25 - 6.25) * 2.0 / 3.0);
            int64_t k = start + samples - 1;
            CHECK_SAMPLE(((float *) audio)[(samples - 1) * channels],
                         test_expected_sample(k, 200, 300, total, 0));
            CHECK_SAMPLE(((float *) audio)[(samples - 1) * channels],
                         12.0 + (13.0 - 12.0) * 2.0 / 3.0);
        }

        if (n == 4) {
            /* Tail beyond the exact source duration is zero, and is not a
             * repeat of the first audio from source frame 0. */
            int64_t k = start + samples - 1;
            CHECK_SAMPLE(((float *) audio)[(samples - 1) * channels],
                         test_expected_sample(k, 200, 300, total, 0));
            TASK_CHECK(((float *) audio)[(samples - 1) * channels] == 0.0f);
            TASK_CHECK(((float *) audio)[(samples - 2) * channels] == 0.0f);
            CHECK_SAMPLE(((float *) audio)[0],
                         test_expected_sample(start, 200, 300, total, 0));
            TASK_CHECK(((float *) audio)[0] != 0.0f);
            TASK_CHECK(((float *) audio)[0] != 0.001f);
        }

        mlt_frame_close(frame);
    }

    mlt_producer_close(conform);
    mlt_producer_close(source);
    TASK_CHECK(state.created == state.destroyed);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

static int case_clock_ntsc(void)
{
    /* 24000/1001 -> 30000/1001 and 44100 -> 48000. */
    test_state state = {44100, 2, 24, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(24000, 1001);
    mlt_profile output_profile = test_profile(30000, 1001);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);
    mlt_producer conform = mlt_producer_conform(output_profile, source, 44100, 48000, 2);
    TASK_CHECK(conform);
    /* ceil(24 * 30000 / 24000) == 30 */
    TASK_CHECK(mlt_producer_get_length(conform) == 30);

    float src_fps = (float) ((double) 24000 / 1001);
    float out_fps = (float) ((double) 30000 / 1001);
    int64_t source_total = test_source_total(24, 44100, src_fps);
    int64_t output_total = test_source_total(30, 48000, out_fps);

    /* Source frames do not all have the same number of samples. */
    TASK_CHECK(test_window_start(src_fps, 44100, 1)
                   - test_window_start(src_fps, 44100, 0)
               != test_window_start(src_fps, 44100, 2)
                      - test_window_start(src_fps, 44100, 1));

    int64_t sum = 0;
    for (int n = 0; n < 30; n++) {
        mlt_frame frame = NULL;
        TASK_CHECK(!read_output_frame(conform, n, &frame) && frame);
        TASK_CHECK(read_image_y(frame) == 40 + test_video_index(n, 24000, 1001, 30000, 1001));

        void *audio = NULL;
        int rate = 0, channels = 0, samples = 0;
        TASK_CHECK(!read_audio(frame, &audio, &rate, &channels, &samples));
        TASK_CHECK(audio && rate == 48000 && channels == 2);
        int64_t start = test_window_start(out_fps, 48000, n);
        int64_t end = test_window_start(out_fps, 48000, n + 1);
        TASK_CHECK(samples == (int) (end - start));
        sum += samples;

        if (n == 0) {
            /* Baseline window sequence: 1602, 1601. */
            TASK_CHECK(samples == 1602);
            CHECK_SAMPLE(((float *) audio)[0],
                         test_expected_sample(start, 44100, 48000, source_total, 0));
            int64_t k = start + samples - 1; /* k == 1601 */
            CHECK_SAMPLE(((float *) audio)[(samples - 1) * channels],
                         test_expected_sample(k, 44100, 48000, source_total, 0));
            CHECK_SAMPLE(((float *) audio)[(samples - 1) * channels], 1470.91875);
            CHECK_SAMPLE(((float *) audio)[(samples - 1) * channels + 1], 1471.16875);
        }

        if (n == 1) {
            TASK_CHECK(samples == 1601);
            CHECK_SAMPLE(((float *) audio)[0],
                         test_expected_sample(start, 44100, 48000, source_total, 0));
            CHECK_SAMPLE(((float *) audio)[0], 1471.8375);
        }

        mlt_frame_close(frame);
    }
    TASK_CHECK(sum == output_total);

    mlt_producer_close(conform);
    mlt_producer_close(source);
    TASK_CHECK(state.created == state.destroyed);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

static int case_clock_no_source_mutation(void)
{
    test_state state = {200, 2, 4, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(30, 1);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);

    int src_num = source_profile->frame_rate_num;
    int src_den = source_profile->frame_rate_den;
    mlt_position in = mlt_producer_get_in(source);
    mlt_position out = mlt_producer_get_out(source);
    mlt_position length = mlt_producer_get_length(source);
    double speed = mlt_producer_get_speed(source);

    mlt_producer conform = mlt_producer_conform(output_profile, source, 200, 300, 2);
    TASK_CHECK(conform);

    /* The output profile is copied: closing the caller's copy is safe. */
    mlt_profile_close(output_profile);

    mlt_frame frame = NULL;
    TASK_CHECK(!read_output_frame(conform, 2, &frame) && frame);
    int cursor = mlt_producer_position(source);
    TASK_CHECK(cursor == 0);
    void *audio = NULL;
    int rate = 0, channels = 0, samples = 0;
    TASK_CHECK(!read_audio(frame, &audio, &rate, &channels, &samples));
    TASK_CHECK(mlt_producer_position(source) == cursor);
    TASK_CHECK(mlt_producer_get_speed(source) == 1.0);

    /* Source profile, edit points, length, and speed are unchanged. */
    TASK_CHECK(mlt_service_profile(MLT_PRODUCER_SERVICE(source)) == source_profile);
    TASK_CHECK(source_profile->frame_rate_num == src_num
               && source_profile->frame_rate_den == src_den);
    TASK_CHECK(mlt_producer_get_in(source) == in);
    TASK_CHECK(mlt_producer_get_out(source) == out);
    TASK_CHECK(mlt_producer_get_length(source) == length);
    TASK_CHECK(mlt_producer_get_speed(source) == speed);
    TASK_CHECK(!mlt_producer_is_cut(source));

    /* The conform producer reports its own output profile. */
    TASK_CHECK(mlt_producer_get_fps(conform) == 30.0);

    mlt_frame_close(frame);
    mlt_producer_close(conform);
    mlt_producer_close(source);
    TASK_CHECK(state.created == state.destroyed);
    mlt_profile_close(source_profile);
    return 0;
}

static int case_clock_rejects(void)
{
    test_state state = {200, 2, 4, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(30, 1);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);

    /* NULL inputs. */
    TASK_CHECK(mlt_producer_conform(NULL, source, 200, 300, 2) == NULL);
    TASK_CHECK(mlt_producer_conform(output_profile, NULL, 200, 300, 2) == NULL);

    /* Non-positive rates and channels. */
    TASK_CHECK(mlt_producer_conform(output_profile, source, 0, 300, 2) == NULL);
    TASK_CHECK(mlt_producer_conform(output_profile, source, -200, 300, 2) == NULL);
    TASK_CHECK(mlt_producer_conform(output_profile, source, 200, 0, 2) == NULL);
    TASK_CHECK(mlt_producer_conform(output_profile, source, 200, -300, 2) == NULL);
    TASK_CHECK(mlt_producer_conform(output_profile, source, 200, 300, 0) == NULL);
    TASK_CHECK(mlt_producer_conform(output_profile, source, 200, 300, -2) == NULL);

    /* Non-positive frame rate terms on either side. */
    mlt_profile bad_output = test_profile(0, 1);
    TASK_CHECK(mlt_producer_conform(bad_output, source, 200, 300, 2) == NULL);
    mlt_profile_close(bad_output);
    mlt_profile zero_source_profile = test_profile(1, 0);
    mlt_producer zero_source = test_source(zero_source_profile, &state);
    TASK_CHECK(zero_source);
    TASK_CHECK(mlt_producer_conform(output_profile, zero_source, 200, 300, 2) == NULL);
    mlt_producer_close(zero_source);
    mlt_profile_close(zero_source_profile);

    /* Zero length is rejected. */
    int saved_length = state.length;
    mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(source), "length", 0);
    TASK_CHECK(mlt_producer_conform(output_profile, source, 200, 300, 2) == NULL);
    mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(source), "length", saved_length);

    /* A cut, non-zero in, non-full out, and non-unit speed are rejected. */
    mlt_producer cut = mlt_producer_cut(source, 0, saved_length - 1);
    TASK_CHECK(cut);
    TASK_CHECK(mlt_producer_conform(output_profile, cut, 200, 300, 2) == NULL);
    mlt_producer_close(cut);

    mlt_producer_set_in_and_out(source, 1, saved_length - 1);
    TASK_CHECK(mlt_producer_conform(output_profile, source, 200, 300, 2) == NULL);
    mlt_producer_set_in_and_out(source, 0, saved_length - 2);
    TASK_CHECK(mlt_producer_conform(output_profile, source, 200, 300, 2) == NULL);
    mlt_producer_set_in_and_out(source, 0, saved_length - 1);
    mlt_producer_set_speed(source, 2.0);
    TASK_CHECK(mlt_producer_conform(output_profile, source, 200, 300, 2) == NULL);
    mlt_producer_set_speed(source, 1.0);

    /* A length whose scaled output cannot be represented is rejected. */
    mlt_properties_set_position(MLT_PRODUCER_PROPERTIES(source), "length", INT_MAX);
    mlt_producer_set_in_and_out(source, 0, INT_MAX - 1);
    TASK_CHECK(mlt_producer_conform(output_profile, source, 200, 300, 2) == NULL);
    mlt_properties_set_position(MLT_PRODUCER_PROPERTIES(source), "length", saved_length);
    mlt_producer_set_in_and_out(source, 0, saved_length - 1);

    /* A window too large to allocate safely is rejected. */
    mlt_profile tiny_output = test_profile(1, 1000); /* fps 0.001 */
    test_state big_channels = {200, 2, 4, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_producer big_source = test_source(source_profile, &big_channels);
    TASK_CHECK(big_source);
    TASK_CHECK(mlt_producer_conform(tiny_output, big_source, 200, 100000, 8) == NULL);
    mlt_producer_close(big_source);
    mlt_profile_close(tiny_output);

    mlt_producer_close(source);
    TASK_CHECK(state.created == state.destroyed);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

/* ---------------------------------------------------------------------------
 * lazy group
 * ------------------------------------------------------------------------- */

static int case_lazy_construct(void)
{
    test_state state = {200, 2, 4, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(30, 1);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);
    TASK_CHECK(state.created == 0 && state.audio_calls == 0);

    mlt_producer conform = mlt_producer_conform(output_profile, source, 200, 300, 2);
    TASK_CHECK(conform);
    /* Construction decodes nothing. */
    TASK_CHECK(state.created == 0 && state.audio_calls == 0);

    mlt_frame frame = NULL;
    TASK_CHECK(!read_output_frame(conform, 1, &frame) && frame);
    /* get_frame decodes nothing either. */
    TASK_CHECK(state.created == 0 && state.audio_calls == 0);

    /* get_image decodes the one mapped source frame, no audio yet. */
    TASK_CHECK(read_image_y(frame) == 40);
    TASK_CHECK(state.audio_calls == 0);
    TASK_CHECK(state.created == state.destroyed + 1); /* held by the output frame */

    /* Reading audio lazily decodes the frames that the window needs. */
    void *audio = NULL;
    int rate = 0, channels = 0, samples = 0;
    TASK_CHECK(!read_audio(frame, &audio, &rate, &channels, &samples));
    TASK_CHECK(state.audio_calls >= 1);
    TASK_CHECK(samples == 10);

    mlt_frame_close(frame);
    TASK_CHECK(state.created == state.destroyed);
    mlt_producer_close(conform);
    mlt_producer_close(source);
    TASK_CHECK(state.created == state.destroyed);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

static int case_lazy_reverse_and_repeat(void)
{
    test_state state = {200, 2, 4, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(30, 1);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);
    mlt_producer conform = mlt_producer_conform(output_profile, source, 200, 300, 2);
    TASK_CHECK(conform);

    mlt_frame frames[3] = {NULL, NULL, NULL};
    for (int n = 0; n < 3; n++) {
        TASK_CHECK(!read_output_frame(conform, n, &frames[n]) && frames[n]);
    }

    float out_fps = 30.0f;
    int64_t total = test_source_total(4, 200, 25.0f);
    float first_values[3];
    for (int n = 2; n >= 0; n--) {
        /* Sentinel cursor so a failure to restore is visible. */
        mlt_producer_seek(source, 3);
        void *audio = NULL;
        int rate = 0, channels = 0, samples = 0;
        TASK_CHECK(!read_audio(frames[n], &audio, &rate, &channels, &samples));
        TASK_CHECK(audio && rate == 300 && channels == 2);
        int64_t start = test_window_start(out_fps, 300, n);
        TASK_CHECK(samples == 10);
        CHECK_SAMPLE(((float *) audio)[0],
                     test_expected_sample(start, 200, 300, total, 0));
        first_values[n] = ((float *) audio)[0];
        /* Downstream play position is restored to the sentinel. */
        TASK_CHECK(mlt_producer_position(source) == 3);
        TASK_CHECK(mlt_producer_get_speed(source) == 1.0);
    }
    /* Reverse order reads produce distinct, correct values. */
    TASK_CHECK(first_values[0] != first_values[1]);
    TASK_CHECK(first_values[1] != first_values[2]);

    /* Repeating a read gives identical bytes. */
    mlt_producer_seek(source, 1);
    void *audio = NULL;
    int rate = 0, channels = 0, samples = 0;
    float saved[20];
    TASK_CHECK(!read_audio(frames[1], &audio, &rate, &channels, &samples));
    memcpy(saved, audio, (size_t) samples * channels * sizeof(float));
    TASK_CHECK(!read_audio(frames[1], &audio, &rate, &channels, &samples));
    TASK_CHECK(memcmp(saved, audio, (size_t) samples * channels * sizeof(float)) == 0);
    TASK_CHECK(mlt_producer_position(source) == 1);

    for (int n = 0; n < 3; n++)
        mlt_frame_close(frames[n]);
    mlt_producer_close(conform);
    mlt_producer_close(source);
    TASK_CHECK(state.created == state.destroyed);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

static int case_lazy_after_close(void)
{
    test_state state = {200, 2, 4, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(25, 1);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);
    mlt_producer conform = mlt_producer_conform(output_profile, source, 200, 200, 2);
    TASK_CHECK(conform);

    mlt_frame frame = NULL;
    TASK_CHECK(!read_output_frame(conform, 2, &frame) && frame);

    /* Drop both the conform producer and the caller's source reference. */
    mlt_producer_close(conform);
    mlt_producer_close(source);

    /* The delivered frame still reads image and audio on its own. */
    TASK_CHECK(read_image_y(frame) == 40 + 2);
    void *audio = NULL;
    int rate = 0, channels = 0, samples = 0;
    TASK_CHECK(!read_audio(frame, &audio, &rate, &channels, &samples));
    TASK_CHECK(audio && rate == 200 && channels == 2 && samples == 8);
    int64_t total = test_source_total(4, 200, 25.0f);
    CHECK_SAMPLE(((float *) audio)[0],
                 test_expected_sample(test_window_start(25.0f, 200, 2), 200, 200, total, 0));

    mlt_frame_close(frame);
    TASK_CHECK(state.created == state.destroyed);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

static int case_lazy_shared_source(void)
{
    test_state state = {200, 2, 4, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile out_a = test_profile(30, 1);
    mlt_profile out_b = test_profile(20, 1);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);
    mlt_producer a = mlt_producer_conform(out_a, source, 200, 300, 2);
    mlt_producer b = mlt_producer_conform(out_b, source, 200, 200, 2);
    TASK_CHECK(a && b);
    TASK_CHECK(mlt_producer_get_length(a) == 5); /* ceil(4*30/25) */
    TASK_CHECK(mlt_producer_get_length(b) == 4); /* ceil(4*20/25) -> 4 */

    mlt_frame fa = NULL, fb = NULL;
    /* Interleave frame acquisition and reading. */
    TASK_CHECK(!read_output_frame(a, 1, &fa) && fa);
    TASK_CHECK(!read_output_frame(b, 2, &fb) && fb);
    TASK_CHECK(read_image_y(fa) == 40 + test_video_index(1, 25, 1, 30, 1));
    TASK_CHECK(read_image_y(fb) == 40 + test_video_index(2, 25, 1, 20, 1));

    int64_t total = test_source_total(4, 200, 25.0f);
    void *audio = NULL;
    int rate = 0, channels = 0, samples = 0;
    TASK_CHECK(!read_audio(fa, &audio, &rate, &channels, &samples));
    TASK_CHECK(rate == 300 && samples == 10);
    CHECK_SAMPLE(((float *) audio)[0],
                 test_expected_sample(test_window_start(30.0f, 300, 1), 200, 300, total, 0));

    TASK_CHECK(!read_audio(fb, &audio, &rate, &channels, &samples));
    TASK_CHECK(rate == 200 && samples == 10);
    CHECK_SAMPLE(((float *) audio)[0],
                 test_expected_sample(test_window_start(20.0f, 200, 2), 200, 200, total, 0));

    /* Reading A again after B must not disturb A's cached audio. */
    void *again = NULL;
    int arate = 0, ach = 0, acount = 0;
    TASK_CHECK(!read_audio(fa, &again, &arate, &ach, &acount));
    TASK_CHECK(arate == 300 && acount == 10);

    mlt_frame_close(fa);
    mlt_frame_close(fb);
    mlt_producer_close(a);
    mlt_producer_close(b);
    mlt_producer_close(source);
    TASK_CHECK(state.created == state.destroyed);
    mlt_profile_close(out_a);
    mlt_profile_close(out_b);
    mlt_profile_close(source_profile);
    return 0;
}

static int case_lazy_source_audio_failure(void)
{
    /* Every source frame fails. */
    test_state state = {200, 2, 4, 0, 0, 0, 0, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(30, 1);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);
    mlt_producer conform = mlt_producer_conform(output_profile, source, 200, 300, 2);
    TASK_CHECK(conform);

    mlt_frame frame = NULL;
    TASK_CHECK(!read_output_frame(conform, 1, &frame) && frame);
    void *audio = (void *) 1;
    int rate = -1, channels = -1, samples = -1;
    int error = read_audio(frame, &audio, &rate, &channels, &samples);
    /* The error is propagated, no partial PCM is published, and the
     * framework's silence fallback does not swallow it. */
    TASK_CHECK(error != 0);
    TASK_CHECK(audio == NULL);
    TASK_CHECK(samples == 0);
    TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "test_audio") == 0);

    /* A repeated read fails the same way rather than returning silence. */
    void *again = (void *) 1;
    int arate = 0, ach = 0, acount = 0;
    TASK_CHECK(read_audio(frame, &again, &arate, &ach, &acount) != 0);
    TASK_CHECK(again == NULL && acount == 0);

    mlt_frame_close(frame);

    /* A window that succeeds until a later source frame fails still publishes
     * nothing. Frame 3 of the output needs source frames 2 and 3. */
    test_state later = {200, 2, 4, 0, 0, 0, 2, 0, 0, 0, 0};
    mlt_producer later_source = test_source(source_profile, &later);
    TASK_CHECK(later_source);
    mlt_producer later_conform = mlt_producer_conform(output_profile, later_source, 200, 300, 2);
    TASK_CHECK(later_conform);
    mlt_frame later_frame = NULL;
    TASK_CHECK(!read_output_frame(later_conform, 0, &later_frame) && later_frame);
    void *ok_audio = NULL;
    int orate = 0, och = 0, ocount = 0;
    TASK_CHECK(!read_audio(later_frame, &ok_audio, &orate, &och, &ocount));
    TASK_CHECK(ok_audio && ocount == 10);
    mlt_frame_close(later_frame);

    mlt_frame failed = NULL;
    TASK_CHECK(!read_output_frame(later_conform, 3, &failed) && failed);
    void *fail_audio = (void *) 1;
    int frate = 0, fch = 0, fcount = -1;
    TASK_CHECK(read_audio(failed, &fail_audio, &frate, &fch, &fcount) != 0);
    TASK_CHECK(fail_audio == NULL && fcount == 0);
    mlt_frame_close(failed);

    mlt_producer_close(later_conform);
    mlt_producer_close(later_source);
    mlt_producer_close(conform);
    mlt_producer_close(source);
    TASK_CHECK(state.created == state.destroyed);
    TASK_CHECK(later.created == later.destroyed);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

static int case_lazy_source_format_mismatch(void)
{
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(30, 1);

    struct
    {
        int bad_format;
        int bad_rate;
        int bad_channels;
        int bad_samples;
    } variants[] = {
        {1, 0, 0, 0}, /* s16 instead of f32le */
        {0, 1, 0, 0}, /* frequency off by one */
        {0, 0, 1, 0}, /* channels off by one */
        {0, 0, 0, 3}, /* sample count too large */
    };

    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        test_state state = {200, 2, 3, 0, 0, 0,             -1,
                            variants[i].bad_format, variants[i].bad_rate,
                            variants[i].bad_channels, variants[i].bad_samples};
        mlt_producer source = test_source(source_profile, &state);
        TASK_CHECK(source);
        mlt_producer conform = mlt_producer_conform(output_profile, source, 200, 300, 2);
        TASK_CHECK(conform);

        mlt_frame frame = NULL;
        TASK_CHECK(!read_output_frame(conform, 0, &frame) && frame);
        void *audio = (void *) 1;
        int rate = 0, channels = 0, samples = -1;
        TASK_CHECK(read_audio(frame, &audio, &rate, &channels, &samples) != 0);
        TASK_CHECK(audio == NULL);
        TASK_CHECK(samples == 0);

        mlt_frame_close(frame);
        mlt_producer_close(conform);
        mlt_producer_close(source);
        TASK_CHECK(state.created == state.destroyed);
    }

    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

static int case_lazy_legacy_audio(void)
{
    /* The ordinary producer interface keeps working unchanged. */
    test_state state = {200, 2, 3, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);

    mlt_producer_seek(source, 1);
    mlt_frame frame = NULL;
    TASK_CHECK(!mlt_service_get_frame(MLT_PRODUCER_SERVICE(source), &frame, 0) && frame);

    void *audio = NULL;
    int rate = 0, channels = 0, samples = 0;
    TASK_CHECK(!read_audio(frame, &audio, &rate, &channels, &samples));
    TASK_CHECK(audio && rate == 200 && channels == 2 && samples == 8);
    TASK_CHECK(fabs(((float *) audio)[0] - 8.0f) < 1.0e-6);
    TASK_CHECK(fabs(((float *) audio)[1] - 8.25f) < 1.0e-6);

    mlt_frame_close(frame);
    mlt_producer_close(source);
    TASK_CHECK(state.created == state.destroyed);
    mlt_profile_close(source_profile);
    return 0;
}

/* ---------------------------------------------------------------------------
 * graph group
 * ------------------------------------------------------------------------- */

static int case_graph_playlist(void)
{
    test_state state_a = {200, 2, 3, 0, 0, 0, -1, 0, 0, 0, 0};
    test_state state_b = {200, 2, 2, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(30, 1);
    mlt_producer source_a = test_source(source_profile, &state_a);
    mlt_producer source_b = test_source(source_profile, &state_b);
    TASK_CHECK(source_a && source_b);
    mlt_producer conform_a = mlt_producer_conform(output_profile, source_a, 200, 300, 2);
    mlt_producer conform_b = mlt_producer_conform(output_profile, source_b, 200, 300, 2);
    TASK_CHECK(conform_a && conform_b);
    TASK_CHECK(mlt_producer_get_length(conform_a) == 4); /* ceil(3*30/25) */
    TASK_CHECK(mlt_producer_get_length(conform_b) == 3); /* ceil(2*30/25) -> 3 */

    mlt_playlist playlist = mlt_playlist_new(output_profile);
    TASK_CHECK(playlist);
    TASK_CHECK(!mlt_playlist_append(playlist, conform_a));
    TASK_CHECK(!mlt_playlist_append(playlist, conform_b));
    mlt_producer playlist_producer = mlt_playlist_producer(playlist);
    TASK_CHECK(mlt_producer_get_length(playlist_producer) == 7);

    float out_fps = 30.0f;
    int64_t total_a = test_source_total(3, 200, 25.0f);
    int64_t total_b = test_source_total(2, 200, 25.0f);

    for (int position = 0; position < 7; position++) {
        mlt_producer_seek(playlist_producer, position);
        mlt_frame frame = NULL;
        TASK_CHECK(!mlt_service_get_frame(mlt_playlist_service(playlist), &frame, 0) && frame);

        int clip_frame = position < 4 ? position : position - 4;
        int expected_y = 40 + test_video_index(clip_frame, 25, 1, 30, 1);
        TASK_CHECK(read_image_y(frame) == expected_y);

        void *audio = NULL;
        int rate = 0, channels = 0, samples = 0;
        TASK_CHECK(!read_audio(frame, &audio, &rate, &channels, &samples));
        TASK_CHECK(audio && rate == 300 && channels == 2);
        int64_t start = test_window_start(out_fps, 300, clip_frame);
        TASK_CHECK(samples == (int) (test_window_start(out_fps, 300, clip_frame + 1) - start));
        int64_t total = position < 4 ? total_a : total_b;
        CHECK_SAMPLE(((float *) audio)[0],
                     test_expected_sample(start, 200, 300, total, 0));
        if (clip_frame == 1) {
            /* The clip's second frame starts at output sample 10, whose first
             * value is the 2/3 interpolation of source samples 6 and 7. */
            CHECK_SAMPLE(((float *) audio)[0], 6.6667);
        }
        mlt_frame_close(frame);
    }

    /* Boundary: playlist frame 4 is source B frame 0. */
    mlt_producer_seek(playlist_producer, 4);
    mlt_frame boundary = NULL;
    TASK_CHECK(!mlt_service_get_frame(mlt_playlist_service(playlist), &boundary, 0) && boundary);
    TASK_CHECK(read_image_y(boundary) == 40);
    mlt_frame_close(boundary);

    mlt_playlist_close(playlist);
    mlt_producer_close(conform_a);
    mlt_producer_close(conform_b);
    mlt_producer_close(source_a);
    mlt_producer_close(source_b);
    TASK_CHECK(state_a.created == state_a.destroyed);
    TASK_CHECK(state_b.created == state_b.destroyed);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

static int case_graph_tractor(void)
{
    test_state state_a = {200, 2, 3, 0, 0, 0, -1, 0, 0, 0, 0};
    test_state state_b = {200, 2, 3, 0, 0, 0, -1, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(30, 1);
    mlt_producer source_a = test_source(source_profile, &state_a);
    mlt_producer source_b = test_source(source_profile, &state_b);
    TASK_CHECK(source_a && source_b);
    mlt_producer conform_a = mlt_producer_conform(output_profile, source_a, 200, 300, 2);
    mlt_producer conform_b = mlt_producer_conform(output_profile, source_b, 200, 300, 2);
    TASK_CHECK(conform_a && conform_b);
    TASK_CHECK(mlt_producer_get_length(conform_a) == 4);
    TASK_CHECK(mlt_producer_get_length(conform_b) == 4);

    mlt_tractor tractor = mlt_tractor_new();
    TASK_CHECK(tractor);
    mlt_service_set_profile(MLT_TRACTOR_SERVICE(tractor), output_profile);
    mlt_service_set_profile(MLT_MULTITRACK_SERVICE(mlt_tractor_multitrack(tractor)),
                            output_profile);
    TASK_CHECK(!mlt_tractor_set_track(tractor, conform_a, 0));
    TASK_CHECK(!mlt_tractor_set_track(tractor, conform_b, 1));
    mlt_producer tractor_producer = mlt_tractor_producer(tractor);
    TASK_CHECK(mlt_producer_get_length(tractor_producer) == 4);

    float out_fps = 30.0f;
    int64_t total = test_source_total(3, 200, 25.0f);

    for (int position = 0; position < 4; position++) {
        mlt_producer_seek(tractor_producer, position);
        mlt_frame frame = NULL;
        TASK_CHECK(!mlt_service_get_frame(mlt_tractor_service(tractor), &frame, 0) && frame);
        TASK_CHECK(mlt_frame_get_position(frame) == position);

        /* The top track supplies video: source B's mapped frame. */
        TASK_CHECK(read_image_y(frame) == 40 + test_video_index(position, 25, 1, 30, 1));

        void *audio = NULL;
        int rate = 0, channels = 0, samples = 0;
        TASK_CHECK(!read_audio(frame, &audio, &rate, &channels, &samples));
        TASK_CHECK(audio && rate == 300 && channels == 2);
        int64_t start = test_window_start(out_fps, 300, position);
        TASK_CHECK(samples == 10);
        /* Both tracks are identical sources, so either track's audio matches. */
        CHECK_SAMPLE(((float *) audio)[0],
                     test_expected_sample(start, 200, 300, total, 0));
        mlt_frame_close(frame);
    }

    mlt_tractor_close(tractor);
    mlt_producer_close(conform_a);
    mlt_producer_close(conform_b);
    mlt_producer_close(source_a);
    mlt_producer_close(source_b);
    TASK_CHECK(state_a.created == state_a.destroyed);
    TASK_CHECK(state_b.created == state_b.destroyed);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

/* A tractor whose selected audio track is a failing conform producer must
 * surface the error through its own mlt_frame_get_audio(), not silence. */
static int case_graph_tractor_audio_error(void)
{
    test_state state = {200, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0};
    mlt_profile source_profile = test_profile(25, 1);
    mlt_profile output_profile = test_profile(30, 1);
    mlt_producer source = test_source(source_profile, &state);
    TASK_CHECK(source);
    mlt_producer conform = mlt_producer_conform(output_profile, source, 200, 300, 2);
    TASK_CHECK(conform);

    mlt_tractor tractor = mlt_tractor_new();
    TASK_CHECK(tractor);
    mlt_service_set_profile(MLT_TRACTOR_SERVICE(tractor), output_profile);
    mlt_service_set_profile(MLT_MULTITRACK_SERVICE(mlt_tractor_multitrack(tractor)),
                            output_profile);
    TASK_CHECK(!mlt_tractor_set_track(tractor, conform, 0));
    mlt_producer tractor_producer = mlt_tractor_producer(tractor);

    mlt_producer_seek(tractor_producer, 0);
    mlt_frame frame = NULL;
    TASK_CHECK(!mlt_service_get_frame(MLT_TRACTOR_SERVICE(tractor), &frame, 0) && frame);

    void *audio = (void *) 1;
    int rate = 0, channels = 0, samples = -1;
    TASK_CHECK(read_audio(frame, &audio, &rate, &channels, &samples) != 0);
    TASK_CHECK(audio == NULL && samples == 0);
    /* A repeat fails identically rather than returning a published empty
     * buffer. */
    void *again = (void *) 1;
    int arate = 0, ach = 0, acount = -1;
    TASK_CHECK(read_audio(frame, &again, &arate, &ach, &acount) != 0);
    TASK_CHECK(again == NULL && acount == 0);

    mlt_frame_close(frame);
    mlt_tractor_close(tractor);
    mlt_producer_close(conform);
    mlt_producer_close(source);
    TASK_CHECK(state.created == state.destroyed);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */

const struct task_case task_cases[] = {
    {"clock", "same_clock", case_same_clock},
    {"clock", "clock_25_to_30", case_clock_25_to_30},
    {"clock", "clock_ntsc", case_clock_ntsc},
    {"clock", "no_source_mutation", case_clock_no_source_mutation},
    {"clock", "reject_inputs", case_clock_rejects},
    {"lazy", "lazy_construct", case_lazy_construct},
    {"lazy", "reverse_and_repeat", case_lazy_reverse_and_repeat},
    {"lazy", "after_close", case_lazy_after_close},
    {"lazy", "shared_source", case_lazy_shared_source},
    {"lazy", "source_audio_failure", case_lazy_source_audio_failure},
    {"lazy", "source_format_mismatch", case_lazy_source_format_mismatch},
    {"lazy", "legacy_audio", case_lazy_legacy_audio},
    {"graph", "playlist_two_clips", case_graph_playlist},
    {"graph", "tractor_two_tracks", case_graph_tractor},
    {"graph", "tractor_audio_error", case_graph_tractor_audio_error},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
