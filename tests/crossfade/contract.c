#include "miniaudio.h"
#include "extras/ma_crossfade/ma_crossfade.h"
#include "task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef MA_CROSSFADE_TEST_PI
#define MA_CROSSFADE_TEST_PI 3.14159265358979323846
#endif

#define TEST_SENTINEL 1234.0f
#define TEST_SAMPLE_RATE 48000u

/* A scripted, seekable f32 data source whose every read and seek is counted. */
typedef float (*script_value_proc)(ma_uint64 frame, ma_uint32 channel, void* pUserData);

typedef struct
{
    ma_uint64 framesGranted;  /* Frames delivered on this call; UINT64_MAX means "everything requested". */
    ma_result result;         /* Result reported when the grant is shorter than the request. */
} script_step;

typedef struct
{
    ma_data_source_base base;
    ma_uint32 channels;
    ma_uint32 sampleRate;
    ma_uint64 lengthInFrames;
    ma_uint64 cursor;
    script_value_proc valueProc;
    void* pUserData;
    const script_step* pScript;     /* NULL means every request is fulfilled. */
    ma_uint32 scriptLength;
    ma_uint64 readCallCount;
    ma_uint64 framesConsumed;
    ma_uint64 seekCount;
} script_source;

static float script_default_value(ma_uint64 frame, ma_uint32 channel, void* pUserData)
{
    double seed = *(const double*)pUserData;
    return (float)(seed + (double)frame * 0.5 + (double)channel * 10.0);
}

static ma_result script_on_read(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
{
    script_source* pSource = (script_source*)pDataSource;
    ma_uint64 grant;
    ma_result shortResult = MA_AT_END;
    ma_uint64 available;

    if (frameCount == 0) {
        return MA_INVALID_ARGS;
    }

    if (pSource->pScript != NULL) {
        ma_uint32 index = (ma_uint32)pSource->readCallCount;
        if (index >= pSource->scriptLength) {
            index = pSource->scriptLength - 1;
        }

        grant = pSource->pScript[index].framesGranted;
        shortResult = pSource->pScript[index].result;
        if (grant > frameCount) {
            grant = frameCount;
        }
    } else {
        grant = frameCount;
    }

    available = pSource->lengthInFrames - pSource->cursor;
    if (grant > available) {
        grant = available;
    }

    if (pFramesOut != NULL) {
        float* pOut = (float*)pFramesOut;
        ma_uint64 f;
        ma_uint32 c;
        for (f = 0; f < grant; ++f) {
            for (c = 0; c < pSource->channels; ++c) {
                pOut[f * pSource->channels + c] =
                    pSource->valueProc(pSource->cursor + f, c, pSource->pUserData);
            }
        }
    }

    pSource->cursor += grant;
    pSource->framesConsumed += grant;
    pSource->readCallCount += 1;

    if (pFramesRead != NULL) {
        *pFramesRead = grant;
    }

    if (grant == frameCount) {
        return MA_SUCCESS;
    }

    return shortResult;
}

static ma_result script_on_seek(ma_data_source* pDataSource, ma_uint64 frameIndex)
{
    script_source* pSource = (script_source*)pDataSource;

    if (frameIndex > pSource->lengthInFrames) {
        return MA_INVALID_OPERATION;
    }

    pSource->cursor = frameIndex;
    pSource->seekCount += 1;
    return MA_SUCCESS;
}

static ma_result script_on_get_data_format(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap)
{
    script_source* pSource = (script_source*)pDataSource;

    if (pFormat != NULL) {
        *pFormat = ma_format_f32;
    }
    if (pChannels != NULL) {
        *pChannels = pSource->channels;
    }
    if (pSampleRate != NULL) {
        *pSampleRate = pSource->sampleRate;
    }
    if (pChannelMap != NULL && channelMapCap > 0) {
        ma_channel_map_init_standard(ma_standard_channel_map_default, pChannelMap, channelMapCap, pSource->channels);
    }

    return MA_SUCCESS;
}

static ma_result script_on_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor)
{
    script_source* pSource = (script_source*)pDataSource;
    *pCursor = pSource->cursor;
    return MA_SUCCESS;
}

static ma_result script_on_get_length(ma_data_source* pDataSource, ma_uint64* pLength)
{
    script_source* pSource = (script_source*)pDataSource;
    *pLength = pSource->lengthInFrames;
    return MA_SUCCESS;
}

static ma_data_source_vtable g_script_vtable =
{
    script_on_read,
    script_on_seek,
    script_on_get_data_format,
    script_on_get_cursor,
    script_on_get_length,
    NULL,
    0
};

static void script_source_init(script_source* pSource, ma_uint32 channels, ma_uint64 lengthInFrames, double seed, const script_step* pScript, ma_uint32 scriptLength)
{
    ma_data_source_config config = ma_data_source_config_init();
    config.vtable = &g_script_vtable;

    memset(pSource, 0, sizeof(*pSource));
    pSource->channels = channels;
    pSource->sampleRate = TEST_SAMPLE_RATE;
    pSource->lengthInFrames = lengthInFrames;
    pSource->cursor = 0;
    pSource->valueProc = script_default_value;
    pSource->pUserData = NULL;
    pSource->pScript = pScript;
    pSource->scriptLength = scriptLength;

    {
        double* pSeed = (double*)ma_malloc(sizeof(double), NULL);
        *pSeed = seed;
        pSource->pUserData = pSeed;
    }

    ma_data_source_init(&config, &pSource->base);
}

static void script_source_uninit(script_source* pSource)
{
    ma_data_source_uninit(&pSource->base);
    ma_free(pSource->pUserData, NULL);
    pSource->pUserData = NULL;
}

/* Test-side clip description plus the derived timeline geometry. */
typedef struct
{
    script_source* pSource;
    ma_uint64 clipStart;
    ma_uint64 length;
    ma_uint64 overlap;
} test_clip;

static void test_timeline_starts(const test_clip* pClips, ma_uint32 clipCount, ma_uint64* pStarts)
{
    ma_uint32 i;
    pStarts[0] = 0;
    for (i = 1; i < clipCount; ++i) {
        pStarts[i] = pStarts[i - 1] + pClips[i - 1].length - pClips[i - 1].overlap;
    }
}

static float test_source_value(const script_source* pSource, ma_uint64 materialFrame, ma_uint32 channel)
{
    return pSource->valueProc(materialFrame, channel, pSource->pUserData);
}

/* Frame-accurate reference renderer implementing the spec formula directly. */
static float reference_sample(const test_clip* pClips, const ma_uint64* pStarts, ma_uint32 clipCount, ma_uint64 outputFrame, ma_uint32 channel)
{
    ma_uint32 i;
    ma_uint64 local;
    ma_uint64 tailBegin;

    for (i = 0; i < clipCount; ++i) {
        if (outputFrame >= pStarts[i] && outputFrame < pStarts[i] + pClips[i].length) {
            break;
        }
    }

    if (i >= clipCount) {
        return 0.0f;  /* Callers only query in-range frames. */
    }

    local = outputFrame - pStarts[i];
    tailBegin = pClips[i].length - pClips[i].overlap;

    if (pClips[i].overlap > 0 && local >= tailBegin) {
        ma_uint64 k = local - tailBegin;
        double theta = MA_CROSSFADE_TEST_PI * ((double)k + 0.5) / (2.0 * (double)pClips[i].overlap);
        float gainOld = (float)cos(theta);
        float gainNew = (float)sin(theta);
        float oldSample = test_source_value(pClips[i].pSource, pClips[i].clipStart + local, channel);
        float newSample = test_source_value(pClips[i + 1].pSource, pClips[i + 1].clipStart + k, channel);
        return oldSample * gainOld + newSample * gainNew;
    }

    return test_source_value(pClips[i].pSource, pClips[i].clipStart + local, channel);
}

static void build_clip_table(const test_clip* pClips, ma_uint32 clipCount, ma_crossfade_clip* pOut)
{
    ma_uint32 i;
    for (i = 0; i < clipCount; ++i) {
        pOut[i].pDataSource = &pClips[i].pSource->base;
        pOut[i].clipStartInFrames = pClips[i].clipStart;
        pOut[i].lengthInFrames = pClips[i].length;
        pOut[i].overlapInFrames = pClips[i].overlap;
    }
}

static void fill_sentinel(float* pBuffer, ma_uint64 frameCount, ma_uint32 channels)
{
    ma_uint64 i;
    for (i = 0; i < frameCount * channels; ++i) {
        pBuffer[i] = TEST_SENTINEL;
    }
}

static int buffers_match_reference(const float* pBuffer, const test_clip* pClips, const ma_uint64* pStarts, ma_uint32 clipCount, ma_uint64 firstFrame, ma_uint64 frameCount, ma_uint32 channels)
{
    ma_uint64 f;
    ma_uint32 c;

    for (f = 0; f < frameCount; ++f) {
        for (c = 0; c < channels; ++c) {
            float want = reference_sample(pClips, pStarts, clipCount, firstFrame + f, c);
            float got = pBuffer[f * channels + c];
            if (fabsf(got - want) > 1e-4f) {
                TASK_CHECK(0);
                return 1;
            }
        }
    }

    TASK_CHECK(1);
    return 0;
}

/*
Drives the crossfade to the end in caller-sized chunks. BUSY results are retried without changing
the request; every returned frame count is checked against the request and accumulated.
*/
static int drain_in_chunks(ma_crossfade* pCrossfade, float* pOut, ma_uint64 totalFrames, ma_uint32 channels, const ma_uint64* pChunkSizes, ma_uint32 chunkCount, ma_uint64 firstFrame, const test_clip* pClips, const ma_uint64* pStarts, ma_uint32 clipCount)
{
    ma_uint64 pos = 0;
    ma_uint32 chunkIndex = 0;
    unsigned iterations = 0;

    while (pos < totalFrames) {
        ma_uint64 want = pChunkSizes[chunkIndex % chunkCount];
        ma_uint64 got = 0;
        ma_result result;
        float* pDst = (pOut != NULL) ? pOut + pos * channels : NULL;

        if (want > totalFrames - pos) {
            want = totalFrames - pos;
        }

        result = ma_data_source_read_pcm_frames(&pCrossfade->base, pDst, want, &got);
        TASK_CHECK(got <= want);
        TASK_CHECK(result == MA_SUCCESS || result == MA_BUSY);

        if (result == MA_BUSY && got == 0) {
            iterations += 1;
            TASK_CHECK(iterations < 100000);  /* No busy loop / deadlock. */
            continue;
        }

        iterations = 0;
        pos += got;
        chunkIndex += 1;

        if (pOut != NULL) {
            if (buffers_match_reference(pDst, pClips, pStarts, clipCount, firstFrame + pos - got, got, channels) != 0) {
                return 1;
            }
        }
    }

    TASK_CHECK(pos == totalFrames);
    return 0;
}

/************************************************************************************************************************************************************
timeline group: frame-by-frame formula coverage
************************************************************************************************************************************************************/

static int case_timeline_stereo(void)
{
    script_source sourceA, sourceB;
    test_clip clips[2];
    ma_crossfade_clip table[2];
    ma_uint64 starts[2];
    ma_crossfade* pCrossfade = NULL;
    float out[64];
    ma_format format;
    ma_uint32 channels = 0, sampleRate = 0;
    ma_uint64 cursor = 999, length = 999;
    ma_uint64 chunkSizes[] = {3, 5, 7};
    ma_result result;

    script_source_init(&sourceA, 2, 32, 100.0, NULL, 0);
    script_source_init(&sourceB, 2, 32, 200.0, NULL, 0);

    clips[0] = (test_clip){&sourceA, 2, 10, 4};
    clips[1] = (test_clip){&sourceB, 3, 8, 0};
    test_timeline_starts(clips, 2, starts);
    TASK_CHECK(starts[1] == 6);

    build_clip_table(clips, 2, table);
    result = ma_crossfade_init(NULL, table, 2, &pCrossfade);
    TASK_CHECK(result == MA_SUCCESS && pCrossfade != NULL);

    TASK_CHECK(ma_data_source_get_data_format(&pCrossfade->base, &format, &channels, &sampleRate, NULL, 0) == MA_SUCCESS);
    TASK_CHECK(format == ma_format_f32 && channels == 2 && sampleRate == TEST_SAMPLE_RATE);
    TASK_CHECK(ma_data_source_get_length_in_pcm_frames(&pCrossfade->base, &length) == MA_SUCCESS && length == 14);
    TASK_CHECK(ma_data_source_get_cursor_in_pcm_frames(&pCrossfade->base, &cursor) == MA_SUCCESS && cursor == 0);

    /* Whole stream in one call, every frame checked against the independent formula. */
    fill_sentinel(out, 32, 2);
    result = ma_data_source_read_pcm_frames(&pCrossfade->base, out, 14, &cursor);
    TASK_CHECK(result == MA_SUCCESS && cursor == 14);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 2, 0, 14, 2) == 0);
    TASK_CHECK(out[28] == TEST_SENTINEL && out[29] == TEST_SENTINEL);

    /* Re-read from zero through uneven chunk boundaries that cut across the overlap. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 0) == MA_SUCCESS);
    fill_sentinel(out, 32, 2);
    TASK_CHECK(drain_in_chunks(pCrossfade, out, 14, 2, chunkSizes, 3, 0, clips, starts, 2) == 0);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 2, 0, 14, 2) == 0);

    TASK_CHECK(ma_data_source_get_cursor_in_pcm_frames(&pCrossfade->base, &cursor) == MA_SUCCESS && cursor == 14);
    ma_crossfade_uninit(pCrossfade);
    script_source_uninit(&sourceA);
    script_source_uninit(&sourceB);
    return 0;
}

static int case_timeline_three_segments(void)
{
    script_source sourceA, sourceB, sourceC;
    test_clip clips[3];
    ma_crossfade_clip table[3];
    ma_uint64 starts[3];
    ma_crossfade* pCrossfade = NULL;
    float out[64];
    ma_uint64 chunkSizes[] = {3, 4, 2};
    ma_uint64 got = 0;
    ma_result result;

    script_source_init(&sourceA, 1, 32, 10.0, NULL, 0);
    script_source_init(&sourceB, 1, 32, 40.0, NULL, 0);
    script_source_init(&sourceC, 1, 32, 70.0, NULL, 0);

    /* Bordering overlaps of B are 3 + 5 = 8, which does not exceed B's length of 10. */
    clips[0] = (test_clip){&sourceA, 1, 8, 3};
    clips[1] = (test_clip){&sourceB, 2, 10, 5};
    clips[2] = (test_clip){&sourceC, 0, 6, 0};
    test_timeline_starts(clips, 3, starts);
    TASK_CHECK(starts[1] == 5 && starts[2] == 10);

    build_clip_table(clips, 3, table);
    result = ma_crossfade_init(NULL, table, 3, &pCrossfade);
    TASK_CHECK(result == MA_SUCCESS);

    fill_sentinel(out, 64, 1);
    TASK_CHECK(drain_in_chunks(pCrossfade, out, 16, 1, chunkSizes, 3, 0, clips, starts, 3) == 0);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 3, 0, 16, 1) == 0);

    /* Each borrowed source is consumed exactly once, over exactly its clip length. */
    TASK_CHECK(sourceA.framesConsumed == 8 && sourceB.framesConsumed == 10 && sourceC.framesConsumed == 6);
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, out, 1, &got) == MA_AT_END && got == 0);

    ma_crossfade_uninit(pCrossfade);
    script_source_uninit(&sourceA);
    script_source_uninit(&sourceB);
    script_source_uninit(&sourceC);
    return 0;
}

static int case_timeline_single_frame(void)
{
    script_source sourceA, sourceB;
    test_clip clips[2];
    ma_crossfade_clip table[2];
    ma_uint64 starts[2];
    ma_crossfade* pCrossfade = NULL;
    float out[16];
    ma_uint64 got = 0;
    float oldSample, newSample, want;
    ma_result result;

    script_source_init(&sourceA, 1, 16, 1.0, NULL, 0);
    script_source_init(&sourceB, 1, 16, 9.0, NULL, 0);

    clips[0] = (test_clip){&sourceA, 0, 5, 1};
    clips[1] = (test_clip){&sourceB, 0, 4, 0};
    test_timeline_starts(clips, 2, starts);
    TASK_CHECK(starts[1] == 4);

    build_clip_table(clips, 2, table);
    result = ma_crossfade_init(NULL, table, 2, &pCrossfade);
    TASK_CHECK(result == MA_SUCCESS);

    result = ma_data_source_read_pcm_frames(&pCrossfade->base, out, 8, &got);
    TASK_CHECK(result == MA_SUCCESS && got == 8);

    /* N = 1, k = 0: theta = pi/4, so both gains are 1/sqrt(2). */
    oldSample = test_source_value(&sourceA, 4, 0);
    newSample = test_source_value(&sourceB, 0, 0);
    want = (oldSample + newSample) * 0.70710678118654752f;
    TASK_CHECK(fabsf(out[4] - want) < 1e-6f);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 2, 0, 8, 1) == 0);

    ma_crossfade_uninit(pCrossfade);
    script_source_uninit(&sourceA);
    script_source_uninit(&sourceB);
    return 0;
}

static int case_timeline_hard_cut(void)
{
    script_source sourceA, sourceB, sourceC;
    test_clip clips[3];
    ma_crossfade_clip table[3];
    ma_uint64 starts[3];
    ma_crossfade* pCrossfade = NULL;
    float out[32];
    ma_uint64 got = 0;
    ma_uint64 chunkSizes[] = {3, 5};
    ma_result result;

    script_source_init(&sourceA, 1, 32, 1.0, NULL, 0);
    script_source_init(&sourceB, 1, 32, 4.0, NULL, 0);
    script_source_init(&sourceC, 1, 32, 7.0, NULL, 0);

    /* A -> B is a hard cut (N = 0); B -> C crossfades. Each clip still consumes exactly once. */
    clips[0] = (test_clip){&sourceA, 0, 4, 0};
    clips[1] = (test_clip){&sourceB, 0, 6, 2};
    clips[2] = (test_clip){&sourceC, 0, 4, 0};
    test_timeline_starts(clips, 3, starts);
    TASK_CHECK(starts[1] == 4 && starts[2] == 8);

    build_clip_table(clips, 3, table);
    result = ma_crossfade_init(NULL, table, 3, &pCrossfade);
    TASK_CHECK(result == MA_SUCCESS);
    TASK_CHECK(ma_data_source_get_length_in_pcm_frames(&pCrossfade->base, &got) == MA_SUCCESS && got == 12);

    fill_sentinel(out, 32, 1);
    TASK_CHECK(drain_in_chunks(pCrossfade, out, 12, 1, chunkSizes, 2, 0, clips, starts, 3) == 0);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 3, 0, 12, 1) == 0);
    TASK_CHECK(sourceA.framesConsumed == 4 && sourceB.framesConsumed == 6 && sourceC.framesConsumed == 4);

    /* No fade gain at the hard boundary: frame 3 is pure A, frame 4 pure B. */
    TASK_CHECK(fabsf(out[3] - test_source_value(&sourceA, 3, 0)) < 1e-6f);
    TASK_CHECK(fabsf(out[4] - test_source_value(&sourceB, 0, 0)) < 1e-6f);

    ma_crossfade_uninit(pCrossfade);
    script_source_uninit(&sourceA);
    script_source_uninit(&sourceB);
    script_source_uninit(&sourceC);
    return 0;
}

static int case_timeline_validation(void)
{
    script_source monoA, monoB, stereoA;
    ma_crossfade* pCrossfade = NULL;
    ma_crossfade_clip good[2];
    ma_crossfade_clip bad[3];
    ma_crossfade_config config;

    script_source_init(&monoA, 1, 32, 1.0, NULL, 0);
    script_source_init(&monoB, 1, 32, 5.0, NULL, 0);
    script_source_init(&stereoA, 2, 32, 9.0, NULL, 0);

    good[0] = (ma_crossfade_clip){&monoA.base, 0, 6, 2};
    good[1] = (ma_crossfade_clip){&monoB.base, 0, 4, 0};

    /* Null table, zero clip count and null output are all rejected. */
    TASK_CHECK(ma_crossfade_init(NULL, NULL, 2, &pCrossfade) == MA_INVALID_ARGS);
    TASK_CHECK(ma_crossfade_init(NULL, good, 0, &pCrossfade) == MA_INVALID_ARGS);
    TASK_CHECK(ma_crossfade_init(NULL, good, 2, NULL) == MA_INVALID_ARGS);

    /* A valid table still initializes, proving the checks are scoped to the bad cases. */
    TASK_CHECK(ma_crossfade_init(NULL, good, 2, &pCrossfade) == MA_SUCCESS);
    ma_crossfade_uninit(pCrossfade);
    pCrossfade = NULL;

    /* Null borrowed source. */
    bad[0] = (ma_crossfade_clip){NULL, 0, 6, 2};
    bad[1] = (ma_crossfade_clip){&monoB.base, 0, 4, 0};
    TASK_CHECK(ma_crossfade_init(NULL, bad, 2, &pCrossfade) == MA_INVALID_ARGS);

    /* Zero-length clip. */
    bad[0] = (ma_crossfade_clip){&monoA.base, 0, 0, 0};
    bad[1] = (ma_crossfade_clip){&monoB.base, 0, 4, 0};
    TASK_CHECK(ma_crossfade_init(NULL, bad, 2, &pCrossfade) == MA_INVALID_ARGS);

    /* The last clip must declare zero overlap. */
    bad[0] = (ma_crossfade_clip){&monoA.base, 0, 6, 2};
    bad[1] = (ma_crossfade_clip){&monoB.base, 0, 4, 1};
    TASK_CHECK(ma_crossfade_init(NULL, bad, 2, &pCrossfade) == MA_INVALID_ARGS);

    /* Bordering overlaps of the middle clip exceed its own length (three clips would collide). */
    bad[0] = (ma_crossfade_clip){&monoA.base, 0, 8, 3};
    bad[1] = (ma_crossfade_clip){&monoB.base, 0, 6, 5};   /* 3 + 5 > 6 */
    bad[2] = (ma_crossfade_clip){&stereoA.base, 0, 4, 0};
    TASK_CHECK(ma_crossfade_init(NULL, bad, 3, &pCrossfade) == MA_INVALID_ARGS);

    /* One borrowed source pointer may not appear twice. */
    bad[0] = (ma_crossfade_clip){&monoA.base, 0, 4, 1};
    bad[1] = (ma_crossfade_clip){&monoA.base, 0, 4, 0};
    TASK_CHECK(ma_crossfade_init(NULL, bad, 2, &pCrossfade) == MA_INVALID_ARGS);

    /* Channel count mismatch across sources. */
    bad[0] = (ma_crossfade_clip){&monoA.base, 0, 4, 1};
    bad[1] = (ma_crossfade_clip){&stereoA.base, 0, 4, 0};
    TASK_CHECK(ma_crossfade_init(NULL, bad, 2, &pCrossfade) == MA_INVALID_ARGS);

    /* Explicit config channels must agree with the sources. */
    config = ma_crossfade_config_init();
    config.channels = 2;
    TASK_CHECK(ma_crossfade_init(&config, good, 2, &pCrossfade) == MA_INVALID_ARGS);

    /* Material window overflow: clipStart + length wraps past UINT64_MAX. */
    bad[0] = (ma_crossfade_clip){&monoA.base, UINT64_MAX - 1u, 4, 0};
    bad[1] = (ma_crossfade_clip){&monoB.base, 0, 4, 0};
    TASK_CHECK(ma_crossfade_init(NULL, bad, 2, &pCrossfade) == MA_INVALID_ARGS);

    /* Timeline geometry overflow: a near-maximum length cannot be extended by another clip. */
    bad[0] = (ma_crossfade_clip){&monoA.base, 0, UINT64_MAX - 2u, 0};
    bad[1] = (ma_crossfade_clip){&monoB.base, 0, 8, 0};
    TASK_CHECK(ma_crossfade_init(NULL, bad, 2, &pCrossfade) == MA_INVALID_ARGS);

    /* Every failed initialization must leave the output pointer untouched. */
    TASK_CHECK(pCrossfade == NULL);

    script_source_uninit(&monoA);
    script_source_uninit(&monoB);
    script_source_uninit(&stereoA);
    return 0;
}

/************************************************************************************************************************************************************
backpressure group: short reads, BUSY prefixes, premature ends and sentinels
************************************************************************************************************************************************************/

static int case_backpressure_short_reads(void)
{
    static const script_step oldScript[] = {{3, MA_BUSY}, {3, MA_BUSY}};
    static const script_step newScript[] = {{2, MA_BUSY}, {2, MA_BUSY}};
    script_source sourceA, sourceB;
    test_clip clips[2];
    ma_crossfade_clip table[2];
    ma_uint64 starts[2];
    ma_crossfade* pCrossfade = NULL;
    float out[32];
    ma_uint64 chunkSizes[] = {4};
    ma_result result;

    script_source_init(&sourceA, 1, 32, 1.0, oldScript, 2);
    script_source_init(&sourceB, 1, 32, 5.0, newScript, 2);

    clips[0] = (test_clip){&sourceA, 0, 12, 6};
    clips[1] = (test_clip){&sourceB, 0, 8, 0};
    test_timeline_starts(clips, 2, starts);
    TASK_CHECK(starts[1] == 6);

    build_clip_table(clips, 2, table);
    result = ma_crossfade_init(NULL, table, 2, &pCrossfade);
    TASK_CHECK(result == MA_SUCCESS);

    fill_sentinel(out, 16, 1);
    TASK_CHECK(drain_in_chunks(pCrossfade, out, 14, 1, chunkSizes, 1, 0, clips, starts, 2) == 0);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 2, 0, 14, 1) == 0);
    TASK_CHECK(out[14] == TEST_SENTINEL && out[15] == TEST_SENTINEL);

    /* Asymmetric short reads (A grants 3, B grants 2) really happened, yet no frame was pulled twice. */
    TASK_CHECK(sourceA.readCallCount >= 4 && sourceB.readCallCount >= 4);
    TASK_CHECK(sourceA.framesConsumed == 12 && sourceB.framesConsumed == 8);

    ma_crossfade_uninit(pCrossfade);
    script_source_uninit(&sourceA);
    script_source_uninit(&sourceB);
    return 0;
}

static int case_backpressure_busy_prefix(void)
{
    /* A: first overlap pull yields a two-frame BUSY prefix, then full delivery. */
    static const script_step oldScript[] = {{2, MA_BUSY}, {UINT64_MAX, MA_SUCCESS}};
    /* B: one frame plus BUSY, then an empty BUSY, then full delivery. */
    static const script_step newScript[] = {{1, MA_BUSY}, {0, MA_BUSY}, {UINT64_MAX, MA_SUCCESS}};
    script_source sourceA, sourceB;
    test_clip clips[2];
    ma_crossfade_clip table[2];
    ma_uint64 starts[2];
    ma_crossfade* pCrossfade = NULL;
    float out[16];
    ma_uint64 got = 0;
    ma_result result;

    script_source_init(&sourceA, 1, 16, 2.0, oldScript, 2);
    script_source_init(&sourceB, 1, 16, 8.0, newScript, 3);

    clips[0] = (test_clip){&sourceA, 0, 8, 4};
    clips[1] = (test_clip){&sourceB, 0, 6, 0};
    test_timeline_starts(clips, 2, starts);
    TASK_CHECK(starts[1] == 4);

    build_clip_table(clips, 2, table);
    result = ma_crossfade_init(NULL, table, 2, &pCrossfade);
    TASK_CHECK(result == MA_SUCCESS);

    /* Jump straight to the overlap so the first pull exercises both scripts' step zero. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 4) == MA_SUCCESS);
    fill_sentinel(out, 16, 1);

    /* First overlap pull: A gives 2, B gives 1 + BUSY. Only the paired prefix is submitted. */
    result = ma_data_source_read_pcm_frames(&pCrossfade->base, out + 4, 2, &got);
    TASK_CHECK(result == MA_BUSY && got == 1);
    TASK_CHECK(fabsf(out[4] - reference_sample(clips, starts, 2, 4, 0)) < 1e-4f);
    TASK_CHECK(out[5] == TEST_SENTINEL);
    /* After submitting the paired frame the same call refills A's side, so A holds all four
       tail frames while B has only delivered its single head frame. */
    TASK_CHECK(sourceA.framesConsumed == 4 && sourceB.framesConsumed == 1);

    /* Retry: the cached A frames are kept and B is asked for the remainder instead of re-reading. */
    result = ma_data_source_read_pcm_frames(&pCrossfade->base, out + 5, 2, &got);
    TASK_CHECK(result == MA_SUCCESS && got == 2);
    TASK_CHECK(buffers_match_reference(out + 5, clips, starts, 2, 5, 2, 1) == 0);
    TASK_CHECK(sourceB.readCallCount == 3);

    /* Drain the remaining frames 7..9 and prove nothing was consumed twice. */
    {
        ma_uint64 chunkSizes[] = {3};
        TASK_CHECK(drain_in_chunks(pCrossfade, out + 7, 3, 1, chunkSizes, 1, 7, clips, starts, 2) == 0);
    }
    TASK_CHECK(buffers_match_reference(out + 4, clips, starts, 2, 4, 6, 1) == 0);
    /* The seek skipped the solo prefix, so A only delivered its four overlap tail frames. */
    TASK_CHECK(sourceA.framesConsumed == 4 && sourceB.framesConsumed == 6);

    ma_crossfade_uninit(pCrossfade);
    script_source_uninit(&sourceA);
    script_source_uninit(&sourceB);
    return 0;
}

static int case_backpressure_busy_empty(void)
{
    static const script_step busyScript[] = {{0, MA_BUSY}, {UINT64_MAX, MA_SUCCESS}};
    script_source sourceA, sourceB;
    test_clip clips[2];
    ma_crossfade_clip table[2];
    ma_crossfade* pCrossfade = NULL;
    float out[8];
    ma_uint64 got = 123;
    ma_uint64 readsBeforeA, readsBeforeB;
    ma_result result;

    script_source_init(&sourceA, 1, 16, 0.0, busyScript, 2);
    script_source_init(&sourceB, 1, 16, 6.0, busyScript, 2);

    clips[0] = (test_clip){&sourceA, 0, 4, 2};
    clips[1] = (test_clip){&sourceB, 0, 3, 0};
    build_clip_table(clips, 2, table);

    result = ma_crossfade_init(NULL, table, 2, &pCrossfade);
    TASK_CHECK(result == MA_SUCCESS);

    /* Jump to the overlap start (clip 0 solo region is frames 0..1) so the next pull is paired. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 2) == MA_SUCCESS);
    readsBeforeA = sourceA.readCallCount;
    readsBeforeB = sourceB.readCallCount;

    /* Both sides report BUSY with nothing: immediate MA_BUSY, zero frames, one pull per side. */
    result = ma_data_source_read_pcm_frames(&pCrossfade->base, out, 2, &got);
    TASK_CHECK(result == MA_BUSY && got == 0);
    TASK_CHECK(sourceA.readCallCount == readsBeforeA + 1);
    TASK_CHECK(sourceB.readCallCount == readsBeforeB + 1);

    /* After the sources become ready the stream completes with the reference samples. */
    {
        ma_uint64 starts[2];
        test_timeline_starts(clips, 2, starts);
        {
            ma_uint64 chunkSizes[] = {2};
            TASK_CHECK(drain_in_chunks(pCrossfade, out, 3, 1, chunkSizes, 1, 2, clips, starts, 2) == 0);
        }
        TASK_CHECK(buffers_match_reference(out, clips, starts, 2, 2, 3, 1) == 0);
    }

    ma_crossfade_uninit(pCrossfade);
    script_source_uninit(&sourceA);
    script_source_uninit(&sourceB);
    return 0;
}

static int case_backpressure_early_end(void)
{
    /* B claims six clip frames but holds only two material frames, then reports a hard end. */
    static const script_step truncatedScript[] = {{2, MA_BUSY}, {0, MA_AT_END}};
    script_source sourceA;
    script_source sourceB;
    test_clip clips[2];
    ma_crossfade_clip table[2];
    ma_uint64 starts[2];
    ma_crossfade* pCrossfade = NULL;
    float out[16];
    ma_uint64 got = 0;
    ma_result result;

    script_source_init(&sourceA, 1, 16, 1.0, NULL, 0);
    script_source_init(&sourceB, 1, 2, 7.0, truncatedScript, 2);

    clips[0] = (test_clip){&sourceA, 0, 8, 4};
    clips[1] = (test_clip){&sourceB, 0, 6, 0};
    test_timeline_starts(clips, 2, starts);

    build_clip_table(clips, 2, table);
    result = ma_crossfade_init(NULL, table, 2, &pCrossfade);
    TASK_CHECK(result == MA_SUCCESS);

    fill_sentinel(out, 16, 1);

    /* Solo prefix is delivered normally. */
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, out, 4, &got) == MA_SUCCESS && got == 4);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 2, 0, 4, 1) == 0);

    /* The overlap needs B's missing frames: the paired prefix ships, then a data error is raised
       rather than treating the truncated material as a normal end of stream. */
    result = ma_data_source_read_pcm_frames(&pCrossfade->base, out + 4, 4, &got);
    TASK_CHECK(result == MA_ERROR);
    TASK_CHECK(got == 2);
    TASK_CHECK(buffers_match_reference(out + 4, clips, starts, 2, 4, got, 1) == 0);
    /* Everything past the delivered prefix keeps the sentinel: truncated data is not emitted. */
    {
        ma_uint64 i;
        for (i = 4 + got; i < 16; ++i) {
            TASK_CHECK(out[i] == TEST_SENTINEL);
        }
    }

    ma_crossfade_uninit(pCrossfade);
    script_source_uninit(&sourceA);
    script_source_uninit(&sourceB);
    return 0;
}

/************************************************************************************************************************************************************
seek_graph group: addressing, NULL discards and node graph integration
************************************************************************************************************************************************************/

static int case_seek_chunks(void)
{
    script_source sourceA, sourceB, sourceC;
    test_clip clips[3];
    ma_crossfade_clip table[3];
    ma_uint64 starts[3];
    ma_crossfade* pCrossfade = NULL;
    float out[64];
    ma_uint64 cursor = 0, got = 0;
    ma_uint64 chunkSizes[] = {2, 3};
    ma_uint64 seekCountBefore;
    ma_result result;

    script_source_init(&sourceA, 1, 32, 1.0, NULL, 0);
    script_source_init(&sourceB, 1, 32, 5.0, NULL, 0);
    script_source_init(&sourceC, 1, 32, 9.0, NULL, 0);

    clips[0] = (test_clip){&sourceA, 0, 8, 3};   /* First overlap: output frames 5..7. */
    clips[1] = (test_clip){&sourceB, 0, 10, 4};  /* Second overlap: output frames 11..14. */
    clips[2] = (test_clip){&sourceC, 0, 6, 0};
    test_timeline_starts(clips, 3, starts);
    TASK_CHECK(starts[1] == 5 && starts[2] == 11);

    build_clip_table(clips, 3, table);
    result = ma_crossfade_init(NULL, table, 3, &pCrossfade);
    TASK_CHECK(result == MA_SUCCESS);
    TASK_CHECK(ma_data_source_get_length_in_pcm_frames(&pCrossfade->base, &got) == MA_SUCCESS && got == 17);

    /* Seek into the middle of the first overlap (k = 1): the phase must continue, not restart. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 6) == MA_SUCCESS);
    TASK_CHECK(ma_data_source_get_cursor_in_pcm_frames(&pCrossfade->base, &cursor) == MA_SUCCESS && cursor == 6);
    fill_sentinel(out, 32, 1);
    TASK_CHECK(drain_in_chunks(pCrossfade, out, 11, 1, chunkSizes, 2, 6, clips, starts, 3) == 0);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 3, 6, 11, 1) == 0);

    /* Seek back to a solo point and render again. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 2) == MA_SUCCESS);
    fill_sentinel(out, 32, 1);
    TASK_CHECK(drain_in_chunks(pCrossfade, out, 8, 1, chunkSizes, 2, 2, clips, starts, 3) == 0);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 3, 2, 8, 1) == 0);

    /* Seeking to the exact total length is legal and the following read is a zero-frame end. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 17) == MA_SUCCESS);
    TASK_CHECK(ma_data_source_get_cursor_in_pcm_frames(&pCrossfade->base, &cursor) == MA_SUCCESS && cursor == 17);
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, out, 4, &got) == MA_AT_END && got == 0);

    /* Out-of-range seeks are rejected before any source is touched and leave the cursor alone. */
    seekCountBefore = sourceA.seekCount + sourceB.seekCount + sourceC.seekCount;
    result = ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 18);
    TASK_CHECK(result == MA_INVALID_OPERATION);
    TASK_CHECK(sourceA.seekCount + sourceB.seekCount + sourceC.seekCount == seekCountBefore);
    TASK_CHECK(ma_data_source_get_cursor_in_pcm_frames(&pCrossfade->base, &cursor) == MA_SUCCESS && cursor == 17);

    /* The rejected seek did not corrupt the cached state: re-seeking and rendering still matches. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 12) == MA_SUCCESS);
    fill_sentinel(out, 32, 1);
    TASK_CHECK(drain_in_chunks(pCrossfade, out, 5, 1, chunkSizes, 2, 12, clips, starts, 3) == 0);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 3, 12, 5, 1) == 0);

    ma_crossfade_uninit(pCrossfade);
    script_source_uninit(&sourceA);
    script_source_uninit(&sourceB);
    script_source_uninit(&sourceC);
    return 0;
}

static int case_seek_discard_and_end(void)
{
    script_source sourceA, sourceB;
    test_clip clips[2];
    ma_crossfade_clip table[2];
    ma_uint64 starts[2];
    ma_crossfade* pCrossfade = NULL;
    float out[32];
    ma_uint64 cursor = 0, got = 0;
    ma_result result;

    script_source_init(&sourceA, 1, 32, 1.0, NULL, 0);
    script_source_init(&sourceB, 1, 32, 5.0, NULL, 0);

    clips[0] = (test_clip){&sourceA, 0, 10, 4};
    clips[1] = (test_clip){&sourceB, 0, 6, 0};
    test_timeline_starts(clips, 2, starts);
    TASK_CHECK(starts[1] == 6);

    build_clip_table(clips, 2, table);
    result = ma_crossfade_init(NULL, table, 2, &pCrossfade);
    TASK_CHECK(result == MA_SUCCESS);
    TASK_CHECK(ma_data_source_get_length_in_pcm_frames(&pCrossfade->base, &got) == MA_SUCCESS && got == 12);

    /* Zero-length reads follow the library convention. */
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, out, 0, &got) == MA_INVALID_ARGS);

    /* A NULL forward discard consumes exactly the same frames as a real read. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 3) == MA_SUCCESS);
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, NULL, 4, &got) == MA_SUCCESS && got == 4);
    TASK_CHECK(ma_data_source_get_cursor_in_pcm_frames(&pCrossfade->base, &cursor) == MA_SUCCESS && cursor == 7);
    fill_sentinel(out, 32, 1);
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, out, 5, &got) == MA_SUCCESS && got == 5);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 2, 7, 5, 1) == 0);

    /* Reading past the total length returns exactly the remainder; the next read ends cleanly. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 9) == MA_SUCCESS);
    fill_sentinel(out, 32, 1);
    result = ma_data_source_read_pcm_frames(&pCrossfade->base, out, 20, &got);
    TASK_CHECK(result == MA_SUCCESS && got == 3);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 2, 9, 3, 1) == 0);
    TASK_CHECK(out[3] == TEST_SENTINEL);
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, out, 20, &got) == MA_AT_END && got == 0);

    /* In-range NULL discards are equivalent to real reads and may cross region boundaries.
       NULL equivalence is only guaranteed while frameCount <= the remaining frames. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 4) == MA_SUCCESS);
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, NULL, 4, &got) == MA_SUCCESS && got == 4);
    TASK_CHECK(ma_data_source_get_cursor_in_pcm_frames(&pCrossfade->base, &cursor) == MA_SUCCESS && cursor == 8);
    fill_sentinel(out, 32, 1);
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, out, 4, &got) == MA_SUCCESS && got == 4);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 2, 8, 4, 1) == 0);

    /* A NULL discard of exactly the remaining frames lands on the end cleanly. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 10) == MA_SUCCESS);
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, NULL, 2, &got) == MA_SUCCESS && got == 2);
    TASK_CHECK(ma_data_source_get_cursor_in_pcm_frames(&pCrossfade->base, &cursor) == MA_SUCCESS && cursor == 12);
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, out, 1, &got) == MA_AT_END && got == 0);
    (void)result;

    ma_crossfade_uninit(pCrossfade);
    script_source_uninit(&sourceA);
    script_source_uninit(&sourceB);
    return 0;
}

static int case_seek_graph_node(void)
{
    script_source sourceA, sourceB;
    test_clip clips[2];
    ma_crossfade_clip table[2];
    ma_uint64 starts[2];
    ma_crossfade* pCrossfade = NULL;
    ma_node_graph graph;
    ma_node_graph_config graphConfig;
    ma_data_source_node node;
    ma_data_source_node_config nodeConfig;
    float out[64];
    ma_uint64 got = 0, pos = 0;
    ma_result result;

    script_source_init(&sourceA, 2, 32, 3.0, NULL, 0);
    script_source_init(&sourceB, 2, 32, 8.0, NULL, 0);

    clips[0] = (test_clip){&sourceA, 1, 9, 5};
    clips[1] = (test_clip){&sourceB, 2, 7, 0};
    test_timeline_starts(clips, 2, starts);
    TASK_CHECK(starts[1] == 4);

    build_clip_table(clips, 2, table);
    result = ma_crossfade_init(NULL, table, 2, &pCrossfade);
    TASK_CHECK(result == MA_SUCCESS);

    graphConfig = ma_node_graph_config_init(2);
    graphConfig.processingSizeInFrames = 0;   /* No internal processing cache; chunks below cut the overlap. */
    TASK_CHECK(ma_node_graph_init(&graphConfig, NULL, &graph) == MA_SUCCESS);

    nodeConfig = ma_data_source_node_config_init(&pCrossfade->base);
    TASK_CHECK(ma_data_source_node_init(&graph, &nodeConfig, NULL, &node) == MA_SUCCESS);
    TASK_CHECK(ma_node_attach_output_bus(&node, 0, ma_node_graph_get_endpoint(&graph), 0) == MA_SUCCESS);

    /* Start inside the overlap (k = 2 of the 5-frame transition) so the pull resumes mid-fade. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 6) == MA_SUCCESS);
    fill_sentinel(out, 32, 2);
    pos = 0;
    while (pos < 5) {
        ma_uint64 framesRead = 0;
        ma_uint64 want = (5 - pos < 2) ? (5 - pos) : 2;   /* 2, 2, 1 */
        result = ma_node_graph_read_pcm_frames(&graph, out + pos * 2, want, &framesRead);
        TASK_CHECK(result == MA_SUCCESS && framesRead == want);
        pos += framesRead;
    }
    TASK_CHECK(pos == 5);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 2, 6, 5, 2) == 0);

    /* A full render from zero through the same graph, in chunks that straddle the overlap. */
    TASK_CHECK(ma_data_source_seek_to_pcm_frame(&pCrossfade->base, 0) == MA_SUCCESS);
    fill_sentinel(out, 32, 2);
    {
        const ma_uint64 chunks[] = {2, 3, 2, 3, 1};   /* Sums to 11; boundaries land inside frames 4..8. */
        ma_uint32 ci;
        pos = 0;
        for (ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ++ci) {
            ma_uint64 framesRead = 0;
            result = ma_node_graph_read_pcm_frames(&graph, out + pos * 2, chunks[ci], &framesRead);
            TASK_CHECK(result == MA_SUCCESS && framesRead == chunks[ci]);
            pos += framesRead;
        }
    }
    TASK_CHECK(pos == 11);
    TASK_CHECK(buffers_match_reference(out, clips, starts, 2, 0, 11, 2) == 0);
    TASK_CHECK(ma_data_source_read_pcm_frames(&pCrossfade->base, out, 1, &got) == MA_AT_END && got == 0);

    ma_data_source_node_uninit(&node, NULL);
    ma_node_graph_uninit(&graph, NULL);
    ma_crossfade_uninit(pCrossfade);

    /* Ownership: borrowed sources survive the crossfade and remain queryable. */
    TASK_CHECK(ma_data_source_get_cursor_in_pcm_frames(&sourceA.base, &got) == MA_SUCCESS);
    TASK_CHECK(ma_data_source_get_cursor_in_pcm_frames(&sourceB.base, &got) == MA_SUCCESS);

    script_source_uninit(&sourceA);
    script_source_uninit(&sourceB);
    return 0;
}

const struct task_case task_cases[] =
{
    {"timeline",     "stereo_asymmetric", case_timeline_stereo},
    {"timeline",     "three_segments",    case_timeline_three_segments},
    {"timeline",     "single_frame",      case_timeline_single_frame},
    {"timeline",     "hard_cut",          case_timeline_hard_cut},
    {"timeline",     "validation",        case_timeline_validation},
    {"backpressure", "short_reads",       case_backpressure_short_reads},
    {"backpressure", "busy_prefix",       case_backpressure_busy_prefix},
    {"backpressure", "busy_empty",        case_backpressure_busy_empty},
    {"backpressure", "early_end",         case_backpressure_early_end},
    {"seek_graph",   "seek_chunks",       case_seek_chunks},
    {"seek_graph",   "discard_and_end",   case_seek_discard_and_end},
    {"seek_graph",   "node_graph",        case_seek_graph_node}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
