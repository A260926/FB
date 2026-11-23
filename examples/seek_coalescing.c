/*
Demonstrates seek coalescing on a resource manager data stream.

Ownership / threading model
---------------------------
A coalescing stream is driven by two roles, which must stay on separate threads in real use:

  * The control thread (here the main thread) owns the public API. It may call
    ma_resource_manager_data_stream_seek_to_pcm_frame(), the seek state query and the read API. Those
    calls never decode and never block on a worker: a seek only updates a mailbox, a read while a
    generation is outstanding returns MA_BUSY with zero frames. All control-thread calls for one
    stream must be serialized.
  * The job thread owns the decoder and the page buffers. In this example the resource manager is
    created with MA_RESOURCE_MANAGER_FLAG_NO_THREADING and the application pumps the queue itself
    through ma_resource_manager_process_next_job(). A real application would instead leave the
    resource manager's internal job thread (or its own worker) running; the coalescing logic is
    identical either way.

A burst of seek requests produces at most one queued or executing seek work unit per stream: an
in-flight unit simply picks up the newest mailbox target when it runs, so requests never pile jobs up
in the queue. The example then verifies the decoded PCM against the formula used to generate the WAV,
and renders the same stream through a device-less ma_engine/ma_sound to show that a sound outputs
silence while a target is outstanding and the exact fresh-sound output once it settles.

No audio device is opened.
*/
#include "miniaudio.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#define DEMO_SAMPLE_RATE   8000
#define DEMO_FRAMES        120000

/* Same asymmetric two-channel formula the file is generated with. */
static int16_t demo_sample(ma_uint64 frame, unsigned channel)
{
    return (int16_t)(((frame * 37u + channel * 113u + 7u) % 60001u) - 30000);
}

static void write_le(FILE* f, unsigned value, unsigned bytes)
{
    while (bytes--) {
        fputc((int)(value & 0xFF), f);
        value >>= 8;
    }
}

static int write_demo_wav(const char* path)
{
    ma_uint32 frame, channel;
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return 1;
    }

    fwrite("RIFF", 1, 4, f); write_le(f, 36u + DEMO_FRAMES * 4u, 4); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); write_le(f, 16, 4);
    write_le(f, 1, 2); write_le(f, 2, 2);                                  /* PCM, stereo. */
    write_le(f, DEMO_SAMPLE_RATE, 4); write_le(f, DEMO_SAMPLE_RATE * 4, 4);
    write_le(f, 4, 2); write_le(f, 16, 2);
    fwrite("data", 1, 4, f); write_le(f, DEMO_FRAMES * 4u, 4);

    for (frame = 0; frame < DEMO_FRAMES; ++frame) {
        for (channel = 0; channel < 2; ++channel) {
            int16_t s = demo_sample(frame, channel);
            write_le(f, (unsigned)(uint16_t)s, 2);
        }
    }

    return fclose(f) != 0;
}

/* Drains every queued job. In a threaded deployment the manager's job thread does this continuously. */
static void pump_jobs(ma_resource_manager* rm)
{
    for (;;) {
        ma_result r = ma_resource_manager_process_next_job(rm);
        if (r == MA_NO_DATA_AVAILABLE || r == MA_CANCELLED) {
            return;
        }
    }
}

static void wait_for_settle(ma_resource_manager* rm, ma_resource_manager_data_stream* stream)
{
    ma_resource_manager_seek_state state;
    do {
        pump_jobs(rm);
        if (ma_resource_manager_data_stream_get_seek_state(stream, &state) != MA_SUCCESS) {
            return;
        }
    } while (state.result == MA_BUSY);
}

static int verify_pcm(ma_resource_manager* rm, ma_resource_manager_data_stream* stream, ma_uint64 target, ma_uint32 frameCount)
{
    int16_t* out = (int16_t*)malloc((size_t)frameCount * 2 * sizeof(int16_t));
    ma_uint32 i, channel;
    ma_uint64 totalRead = 0;
    int ok = 1;

    if (out == NULL) {
        return 0;
    }

    while (totalRead < frameCount) {
        ma_uint64 framesRead = 0;
        ma_result r = ma_resource_manager_data_stream_read_pcm_frames(stream,
            ma_offset_pcm_frames_ptr(out, (ma_uint32)totalRead, ma_format_s16, 2),
            frameCount - (ma_uint32)totalRead, &framesRead);
        if (r == MA_BUSY) {
            pump_jobs(rm);          /* Control thread never decodes; it only lets the job queue drain. */
            continue;
        }
        if (r != MA_SUCCESS) {
            ok = 0;
            break;
        }
        totalRead += framesRead;
    }

    if (totalRead == frameCount) {
        for (i = 0; i < frameCount; ++i) {
            for (channel = 0; channel < 2; ++channel) {
                if (out[(size_t)i * 2 + channel] != demo_sample(target + i, channel)) {
                    ok = 0;
                }
            }
        }
    } else {
        ok = 0;
    }

    free(out);
    return ok;
}

static int run_sound_check(ma_resource_manager* rm, const char* path)
{
    ma_engine_config engineConfig = ma_engine_config_init();
    ma_engine mainEngine, referenceEngine;
    ma_resource_manager_data_stream mainStream, referenceStream;
    ma_sound mainSound, referenceSound;
    enum { PERIOD = 256 };
    float busyOutput[PERIOD * 2];
    float mainOutput[PERIOD * 2];
    float referenceOutput[PERIOD * 2];
    ma_uint64 framesRead = 0;
    const ma_uint64 target = 24000;
    ma_uint32 i;
    int ok = 1;

    engineConfig.noDevice = MA_TRUE;
    engineConfig.channels = 2;
    engineConfig.sampleRate = DEMO_SAMPLE_RATE;
    engineConfig.periodSizeInFrames = PERIOD;
    engineConfig.pResourceManager = rm;

    /* Separate engines so the reference sound cannot be mixed into the main sound's output. */
    if (ma_engine_init(&engineConfig, &mainEngine) != MA_SUCCESS ||
        ma_engine_init(&engineConfig, &referenceEngine) != MA_SUCCESS) {
        return 0;
    }

    /* The coalescing stream under test, and a fresh stream used only to build the reference output. */
    if (ma_resource_manager_data_stream_init(rm, path,
            MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM | MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_SEEK_COALESCING,
            NULL, &mainStream) != MA_SUCCESS) {
        ma_engine_uninit(&referenceEngine);
        ma_engine_uninit(&mainEngine);
        return 0;
    }
    if (ma_resource_manager_data_stream_init(rm, path,
            MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM | MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_SEEK_COALESCING,
            NULL, &referenceStream) != MA_SUCCESS) {
        ma_resource_manager_data_stream_uninit(&mainStream);
        ma_engine_uninit(&referenceEngine);
        ma_engine_uninit(&mainEngine);
        return 0;
    }
    pump_jobs(rm);

    if (ma_sound_init_from_data_source(&mainEngine, (ma_data_source*)&mainStream, MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &mainSound) != MA_SUCCESS ||
        ma_sound_init_from_data_source(&referenceEngine, (ma_data_source*)&referenceStream, MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &referenceSound) != MA_SUCCESS) {
        ok = 0;
        goto done_sound;
    }
    ma_sound_set_pitch(&mainSound, 1.25f);
    ma_sound_set_pitch(&referenceSound, 1.25f);
    ma_sound_start(&mainSound);

    /* Submit the target but do not pump: the next engine render must be silence from this sound. */
    if (ma_sound_seek_to_pcm_frame(&mainSound, target) != MA_SUCCESS) {
        ok = 0;
        goto done_sound;
    }
    if (ma_engine_read_pcm_frames(&mainEngine, busyOutput, PERIOD, &framesRead) != MA_SUCCESS || framesRead != PERIOD) {
        ok = 0;
        goto done_sound;
    }
    for (i = 0; i < PERIOD * 2; ++i) {
        if (busyOutput[i] != 0.0f) {
            ok = 0;   /* Stale cache/resampler history leaked while the target was outstanding. */
        }
    }

    /* Reference: a brand new sound started at the same target. Its first render dispatches the seek and is silent; settle, then render the target output. */
    ma_sound_start(&referenceSound);
    if (ma_sound_seek_to_pcm_frame(&referenceSound, target) != MA_SUCCESS) {
        ok = 0;
        goto done_sound;
    }
    {
        float referenceScratch[PERIOD * 2];
        if (ma_engine_read_pcm_frames(&referenceEngine, referenceScratch, PERIOD, NULL) != MA_SUCCESS) {
            ok = 0;
            goto done_sound;
        }
    }
    wait_for_settle(rm, &referenceStream);
    if (ma_engine_read_pcm_frames(&referenceEngine, referenceOutput, PERIOD, NULL) != MA_SUCCESS) {
        ok = 0;
        goto done_sound;
    }

    /* Now settle the main sound. Its next render must match the fresh reference sample-for-sample. */
    wait_for_settle(rm, &mainStream);
    if (ma_engine_read_pcm_frames(&mainEngine, mainOutput, PERIOD, NULL) != MA_SUCCESS) {
        ok = 0;
        goto done_sound;
    }
    for (i = 0; i < PERIOD * 2; ++i) {
        if (fabs((double)mainOutput[i] - (double)referenceOutput[i]) > 0.000001) {
            ok = 0;
        }
    }

done_sound:
    ma_sound_uninit(&referenceSound);
    ma_sound_uninit(&mainSound);
    ma_resource_manager_data_stream_uninit(&referenceStream);
    ma_resource_manager_data_stream_uninit(&mainStream);
    ma_engine_uninit(&referenceEngine);
    ma_engine_uninit(&mainEngine);
    return ok;
}

int main(void)
{
    const char* wavPath = ".task-build/seek_demo.wav";
    const ma_uint64 burstTargets[] = { 1000, 90000, 4242, 77777, 33333, 56000 };
    const ma_uint32 burstCount = (ma_uint32)(sizeof(burstTargets) / sizeof(burstTargets[0]));
    ma_resource_manager_config managerConfig;
    ma_resource_manager manager;
    ma_resource_manager_data_stream stream;
    ma_resource_manager_seek_state state;
    ma_uint32 i;

    if (write_demo_wav(wavPath) != 0) {
        fprintf(stderr, "failed to generate demo WAV\n");
        return 1;
    }

    managerConfig = ma_resource_manager_config_init();
    managerConfig.jobThreadCount = 0;
    managerConfig.flags = MA_RESOURCE_MANAGER_FLAG_NO_THREADING;     /* The example pumps jobs itself. */
    managerConfig.decodedFormat = ma_format_s16;
    managerConfig.decodedChannels = 2;
    managerConfig.decodedSampleRate = DEMO_SAMPLE_RATE;

    if (ma_resource_manager_init(&managerConfig, &manager) != MA_SUCCESS) {
        return 2;
    }

    if (ma_resource_manager_data_stream_init(&manager, wavPath,
            MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM | MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_SEEK_COALESCING,
            NULL, &stream) != MA_SUCCESS) {
        ma_resource_manager_uninit(&manager);
        return 3;
    }

    if (ma_resource_manager_data_stream_get_seek_state(&stream, &state) != MA_SUCCESS ||
        state.requested != 0 || state.settled != 0 || state.frameIndex != 0 || state.result != MA_SUCCESS) {
        fprintf(stderr, "bad initial seek state\n");
        return 4;
    }

    /* A whole burst, submitted before any job runs. Every request is accepted; only one work unit exists. */
    for (i = 0; i < burstCount; ++i) {
        if (ma_resource_manager_data_stream_seek_to_pcm_frame(&stream, burstTargets[i]) != MA_SUCCESS) {
            fprintf(stderr, "seek %u rejected\n", i);
            return 5;
        }
    }

    if (ma_resource_manager_data_stream_get_seek_state(&stream, &state) != MA_SUCCESS ||
        state.requested != burstCount || state.settled != 0 ||
        state.frameIndex != burstTargets[burstCount - 1] || state.result != MA_BUSY) {
        fprintf(stderr, "bad burst seek state\n");
        return 6;
    }

    wait_for_settle(&manager, &stream);
    if (ma_resource_manager_data_stream_get_seek_state(&stream, &state) != MA_SUCCESS ||
        state.settled != burstCount || state.result != MA_SUCCESS) {
        fprintf(stderr, "burst did not settle on the latest target\n");
        return 7;
    }

    if (!verify_pcm(&manager, &stream, burstTargets[burstCount - 1], 128)) {
        fprintf(stderr, "decoded PCM does not match the target\n");
        return 8;
    }
    printf("SEEK_COALESCING_PCM_OK burst=%u target=%llu\n",
        burstCount, (unsigned long long)burstTargets[burstCount - 1]);

    if (!run_sound_check(&manager, wavPath)) {
        fprintf(stderr, "device-less sound rendering check failed\n");
        return 9;
    }
    printf("SEEK_COALESCING_SOUND_OK silence_while_busy=1 fresh_output_match=1\n");

    ma_resource_manager_data_stream_uninit(&stream);
    ma_resource_manager_uninit(&manager);
    remove(wavPath);
    printf("SEEK_COALESCING_DEMO_OK\n");
    return 0;
}
