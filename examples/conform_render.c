/*
 * conform_render.c -- render a short PCM16 stereo WAV through mlt_producer_conform.
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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A source producer that generates non-zero interleaved stereo material.
 * Video is a deterministic gradient stamped with the source frame number and
 * audio is a mixture of tones indexed by the absolute sample number, so every
 * output sample written to the file is non-zero. */

static int source_image(mlt_frame frame,
                        uint8_t **buffer,
                        mlt_image_format *format,
                        int *width,
                        int *height,
                        int writable)
{
    (void) writable;
    int position = mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "example.position");
    uint8_t *image = mlt_pool_alloc(256);
    if (!image)
        return 1;
    for (int i = 0; i < 256; i += 2) {
        image[i] = (uint8_t) (32 + position * 3 + (i % 32));
        image[i + 1] = 128;
    }
    mlt_frame_set_image(frame, image, 256, mlt_pool_release);
    *buffer = image;
    *format = mlt_image_yuv422;
    *width = 16;
    *height = 8;
    return 0;
}

static int source_audio(mlt_frame frame,
                        void **buffer,
                        mlt_audio_format *format,
                        int *rate,
                        int *channels,
                        int *samples)
{
    mlt_properties properties = MLT_FRAME_PROPERTIES(frame);
    int64_t start = mlt_properties_get_int64(properties, "example.start");
    int count = mlt_properties_get_int(properties, "example.count");
    int reported_rate = mlt_properties_get_int(properties, "example.rate");
    int reported_channels = mlt_properties_get_int(properties, "example.channels");
    size_t bytes = (size_t) count * reported_channels * sizeof(float);
    float *pcm = mlt_pool_alloc(bytes ? (int) bytes : (int) sizeof(float));
    if (!pcm) {
        *buffer = NULL;
        return 1;
    }
    for (int i = 0; i < count; i++) {
        for (int c = 0; c < reported_channels; c++) {
            int64_t index = start + i;
            double phase = (double) index / (double) reported_rate * 2.0 * M_PI * (220.0 + 55.0 * c);
            /* A drift term keeps the signal from collapsing to zero. */
            pcm[(size_t) i * reported_channels + c] = (float) (0.35 + 0.3 * sin(phase));
        }
    }
    *buffer = pcm;
    *format = mlt_audio_f32le;
    *rate = reported_rate;
    *channels = reported_channels;
    *samples = count;
    mlt_frame_set_audio(frame, pcm, *format, (int) bytes, mlt_pool_release);
    return 0;
}

static int source_frame(mlt_producer producer, mlt_frame_ptr frame, int track)
{
    (void) track;
    mlt_properties producer_properties = MLT_PRODUCER_PROPERTIES(producer);
    mlt_profile profile = mlt_service_profile(MLT_PRODUCER_SERVICE(producer));
    int position = mlt_producer_position(producer);
    int rate = mlt_properties_get_int(producer_properties, "example.rate");
    int channels = mlt_properties_get_int(producer_properties, "example.channels");

    float fps = (float) mlt_profile_fps(profile);
    int64_t start = mlt_audio_calculate_samples_to_position(fps, rate, position);
    int64_t end = mlt_audio_calculate_samples_to_position(fps, rate, position + 1);

    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    if (!*frame)
        return 1;
    mlt_frame_set_position(*frame, position);

    mlt_properties properties = MLT_FRAME_PROPERTIES(*frame);
    mlt_properties_set_int64(properties, "example.start", start);
    mlt_properties_set_int(properties, "example.count", (int) (end - start));
    mlt_properties_set_int(properties, "example.rate", rate);
    mlt_properties_set_int(properties, "example.channels", channels);
    mlt_properties_set_int(properties, "example.position", position);

    mlt_frame_push_get_image(*frame, source_image);
    mlt_frame_push_audio(*frame, source_audio);
    mlt_producer_prepare_next(producer);
    return 0;
}

static mlt_profile example_profile(int num, int den)
{
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

static mlt_producer example_source(mlt_profile profile, int length, int rate, int channels)
{
    mlt_producer producer = mlt_producer_new(profile);
    if (!producer)
        return NULL;
    producer->get_frame = source_frame;
    mlt_properties properties = MLT_PRODUCER_PROPERTIES(producer);
    mlt_properties_set_int(properties, "length", length);
    mlt_properties_set_int(properties, "example.rate", rate);
    mlt_properties_set_int(properties, "example.channels", channels);
    mlt_properties_set_string(properties, "resource", "<example-source>");
    mlt_producer_set_in_and_out(producer, 0, length - 1);
    mlt_producer_set_speed(producer, 1.0);
    return producer;
}

/* Write a canonical 16-bit PCM stereo WAV. Returns 0 on success. */
static int write_wav(const char *path,
                     const float *samples,
                     int64_t sample_count,
                     int channels,
                     int rate)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return 1;

    uint32_t data_bytes = (uint32_t) (sample_count * channels * sizeof(int16_t));
    unsigned char header[44];
    uint32_t byte_rate = (uint32_t) rate * (uint32_t) channels * 2;

#define PUT32(offset, value)                                     \
    do {                                                         \
        header[(offset) + 0] = (unsigned char) ((value) & 0xFF); \
        header[(offset) + 1] = (unsigned char) (((value) >> 8) & 0xFF); \
        header[(offset) + 2] = (unsigned char) (((value) >> 16) & 0xFF); \
        header[(offset) + 3] = (unsigned char) (((value) >> 24) & 0xFF); \
    } while (0)
#define PUT16(offset, value)                                    \
    do {                                                        \
        header[(offset) + 0] = (unsigned char) ((value) & 0xFF); \
        header[(offset) + 1] = (unsigned char) (((value) >> 8) & 0xFF); \
    } while (0)

    memcpy(header, "RIFF", 4);
    PUT32(4, 36 + data_bytes);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    PUT32(16, 16);
    PUT16(20, 1); /* PCM */
    PUT16(22, (uint16_t) channels);
    PUT32(24, (uint32_t) rate);
    PUT32(28, byte_rate);
    PUT16(32, (uint16_t) (channels * 2));
    PUT16(34, 16);
    memcpy(header + 36, "data", 4);
    PUT32(40, data_bytes);

    if (fwrite(header, sizeof(header), 1, file) != 1) {
        fclose(file);
        return 1;
    }

    int error = 0;
    for (int64_t i = 0; i < sample_count * channels; i++) {
        double value = samples[i];
        if (value > 1.0)
            value = 1.0;
        if (value < -1.0)
            value = -1.0;
        int16_t pcm = (int16_t) lrint(value * 32767.0);
        if (fwrite(&pcm, sizeof(pcm), 1, file) != 1) {
            error = 1;
            break;
        }
    }
    if (fclose(file) != 0)
        error = 1;
    return error;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output.wav>\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    int exit_code = 1;
    int wrote = 0;

    mlt_factory_init(".task-build/empty-modules");

    mlt_profile source_profile = example_profile(25, 1);
    mlt_profile output_profile = example_profile(30, 1);
    mlt_producer source = NULL, conform = NULL;
    float *rendered = NULL;

    if (!source_profile || !output_profile)
        goto cleanup;

    source = example_source(source_profile, 8, 200, 2);
    if (!source)
        goto cleanup;

    conform = mlt_producer_conform(output_profile, source, 200, 300, 2);
    if (!conform) {
        fprintf(stderr, "conform_render: mlt_producer_conform failed\n");
        goto cleanup;
    }

    int64_t length = mlt_producer_get_length(conform);
    if (length <= 0 || length > 100000)
        goto cleanup;

    float out_fps = 30.0f;
    int64_t total = 0;
    int64_t *offsets = calloc((size_t) length, sizeof(*offsets));
    int *counts = calloc((size_t) length, sizeof(*counts));
    if (!offsets || !counts)
        goto cleanup;

    /* First pass: read each frame's audio window and accumulate samples. */
    for (int64_t position = 0; position < length; position++) {
        mlt_producer_seek(conform, position);
        mlt_frame frame = NULL;
        if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(conform), &frame, 0) || !frame)
            goto cleanup;

        void *audio = NULL;
        mlt_audio_format format = mlt_audio_f32le;
        int rate = 0, channels = 0, samples = 0;
        if (mlt_frame_get_audio(frame, &audio, &format, &rate, &channels, &samples)
            || !audio || format != mlt_audio_f32le || rate != 300 || channels != 2) {
            fprintf(stderr, "conform_render: frame %lld audio failed\n", (long long) position);
            mlt_frame_close(frame);
            goto cleanup;
        }
        offsets[position] = total;
        counts[position] = samples;
        total += samples;
        mlt_frame_close(frame);
    }

    if (total <= 0)
        goto cleanup;
    rendered = malloc((size_t) total * 2 * sizeof(float));
    if (!rendered)
        goto cleanup;

    /* Second pass: collect the samples. Re-reading is deterministic. */
    for (int64_t position = 0; position < length; position++) {
        mlt_producer_seek(conform, position);
        mlt_frame frame = NULL;
        if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(conform), &frame, 0) || !frame)
            goto cleanup;

        void *audio = NULL;
        mlt_audio_format format = mlt_audio_f32le;
        int rate = 0, channels = 0, samples = 0;
        if (mlt_frame_get_audio(frame, &audio, &format, &rate, &channels, &samples)
            || !audio || samples != counts[position] || channels != 2) {
            mlt_frame_close(frame);
            goto cleanup;
        }
        memcpy(rendered + offsets[position] * 2,
               audio,
               (size_t) samples * (size_t) channels * sizeof(float));

        /* Render the image too, so the example exercises the video path. */
        uint8_t *image = NULL;
        mlt_image_format image_format = mlt_image_yuv422;
        int width = 16, height = 8;
        if (mlt_frame_get_image(frame, &image, &image_format, &width, &height, 0) || !image) {
            mlt_frame_close(frame);
            goto cleanup;
        }
        mlt_frame_close(frame);
    }

    if (write_wav(path, rendered, total, 2, 300)) {
        fprintf(stderr, "conform_render: failed to write %s\n", path);
        goto cleanup;
    }
    wrote = 1;

    printf("CONFORM_RENDER_OK path=%s frames=%lld samples=%lld rate=300 channels=2\n",
           path,
           (long long) length,
           (long long) total);
    exit_code = 0;

cleanup:
    free(rendered);
    mlt_producer_close(conform);
    mlt_producer_close(source);
    mlt_profile_close(output_profile);
    mlt_profile_close(source_profile);
    mlt_factory_close();

    if (!wrote && path) {
        /* Remove any partially written file from this run. */
        remove(path);
    }
    return exit_code;
}
