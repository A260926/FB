/*
fanout_contract.c

Contract tests for shared upstream nodes feeding a splitter/passthrough fan-out graph at one
sample rate. A single shared data source is pulled exactly once; every output bus must see the
same samples at their true global frame position, regardless of per-branch start/stop timing,
connection order, read chunking or processing-cache configuration.

Build modes (selected at compile time):
  Default      : defines MINIAUDIO_IMPLEMENTATION and includes the amalgamated miniaudio.h.
  CHECK_SPLIT  : includes only the split declaration header and links against the generated
                 extras/miniaudio_split/miniaudio.c translation unit.
*/
#ifdef CHECK_SPLIT
#include "miniaudio.h"
#else
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#endif

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <assert.h>

static int g_failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); g_failures += 1; } \
} while (0)

/* A plain passthrough node: it only exists so a branch can carry its own volume and timing. */
static void passthrough_process(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
    (void)pNode; (void)ppFramesIn; (void)pFrameCountIn; (void)ppFramesOut; (void)pFrameCountOut;
}
static ma_node_vtable g_passthrough_vtable =
{
    passthrough_process,
    NULL,
    1,
    1,
    MA_NODE_FLAG_PASSTHROUGH
};

/*
Shared upstream data source. Emits a deterministic per-channel ramp and records exactly how many
frames the graph pulled and how many read calls that took. One instance is shared by every branch
through the splitter - it must never be read once per output bus.
*/
typedef struct
{
    ma_data_source_base ds;
    ma_uint32 channels;
    ma_uint64 sizeInFrames;
    ma_uint64 cursor;
    ma_uint64 readCallCount;
    ma_uint64 framesPulled;
} counting_source;

static ma_result counting_source_on_read(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
{
    counting_source* pSource = (counting_source*)pDataSource;
    ma_uint64 framesToRead = frameCount;
    ma_uint64 iFrame;
    ma_uint32 iChannel;

    if (pSource->cursor + framesToRead > pSource->sizeInFrames) {
        framesToRead = pSource->sizeInFrames - pSource->cursor;
    }

    if (pFramesOut != NULL) {
        float* pOut = (float*)pFramesOut;
        for (iFrame = 0; iFrame < framesToRead; iFrame += 1) {
            ma_uint64 globalFrame = pSource->cursor + iFrame;
            for (iChannel = 0; iChannel < pSource->channels; iChannel += 1) {
                /* Left and right deliberately differ so a frame offset cannot be mistaken for a float offset. */
                pOut[(iFrame * pSource->channels) + iChannel] =
                    (float)(((double)(globalFrame + 1) / 64.0) + ((double)iChannel * 0.001));
            }
        }
    }

    pSource->cursor         += framesToRead;
    pSource->readCallCount  += 1;
    pSource->framesPulled   += framesToRead;

    if (pFramesRead != NULL) {
        *pFramesRead = framesToRead;
    }

    return (framesToRead < frameCount) ? MA_AT_END : MA_SUCCESS;
}

static ma_result counting_source_on_seek(ma_data_source* pDataSource, ma_uint64 frameIndex)
{
    (void)pDataSource; (void)frameIndex;
    return MA_NOT_IMPLEMENTED;
}

static ma_result counting_source_on_get_data_format(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap)
{
    counting_source* pSource = (counting_source*)pDataSource;
    (void)pChannelMap; (void)channelMapCap;

    if (pFormat)     *pFormat = ma_format_f32;
    if (pChannels)   *pChannels = pSource->channels;
    if (pSampleRate) *pSampleRate = 48000;
    return MA_SUCCESS;
}

static ma_result counting_source_on_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor)
{
    counting_source* pSource = (counting_source*)pDataSource;
    if (pCursor) *pCursor = pSource->cursor;
    return MA_SUCCESS;
}

static ma_result counting_source_on_get_length(ma_data_source* pDataSource, ma_uint64* pLength)
{
    counting_source* pSource = (counting_source*)pDataSource;
    if (pLength) *pLength = pSource->sizeInFrames;
    return MA_SUCCESS;
}

static ma_data_source_vtable g_counting_source_vtable =
{
    counting_source_on_read,
    counting_source_on_seek,
    counting_source_on_get_data_format,
    counting_source_on_get_cursor,
    counting_source_on_get_length,
    NULL,
    0
};

static void counting_source_init(counting_source* pSource, ma_uint32 channels, ma_uint64 sizeInFrames)
{
    ma_data_source_config config = ma_data_source_config_init();
    config.vtable = &g_counting_source_vtable;

    memset(pSource, 0, sizeof(*pSource));
    assert(ma_data_source_init(&config, &pSource->ds) == MA_SUCCESS);
    pSource->channels     = channels;
    pSource->sizeInFrames = sizeInFrames;
}

static void counting_source_uninit(counting_source* pSource)
{
    ma_data_source_uninit(&pSource->ds);
}

/* Independent per-frame reference: x[n] = (n+1)/64 (+ channel offset), full scale inside the window, 0.25x outside. */
static float reference_sample(ma_uint64 globalFrame, ma_uint32 channel, ma_uint64 windowBeg, ma_uint64 windowEnd)
{
    float base = (float)(((double)(globalFrame + 1) / 64.0) + ((double)channel * 0.001));
    int inside = (globalFrame >= windowBeg && globalFrame < windowEnd);
    return base * (inside ? 1.0f : 0.25f);
}

/*
Canonical mono graph:

    source -> splitter -- bus 0 (volume 0.25) ----------------------> endpoint
                      \-> bus 1 -> passthrough branch (volume 0.75) -> endpoint

The branch only contributes between [branchBeg, branchEnd).
*/
typedef struct
{
    ma_node_graph nodeGraph;
    counting_source source;
    ma_data_source_node sourceNode;
    ma_splitter_node splitter;
    ma_node_base branch;
} fanout_mono_graph;

static void fanout_mono_init(fanout_mono_graph* pGraph, ma_uint32 processingSizeInFrames,
                             ma_uint64 branchBeg, ma_uint64 branchEnd, int reverseAttachOrder)
{
    ma_node_graph_config graphConfig = ma_node_graph_config_init(1);
    ma_splitter_node_config splitterConfig = ma_splitter_node_config_init(1);
    ma_node_config branchConfig = ma_node_config_init();
    ma_uint32 channels = 1;

    graphConfig.processingSizeInFrames = processingSizeInFrames;
    assert(ma_node_graph_init(&graphConfig, NULL, &pGraph->nodeGraph) == MA_SUCCESS);

    counting_source_init(&pGraph->source, 1, 4096);
    {
        ma_data_source_node_config sourceConfig = ma_data_source_node_config_init(&pGraph->source.ds);
        assert(ma_data_source_node_init(&pGraph->nodeGraph, &sourceConfig, NULL, &pGraph->sourceNode) == MA_SUCCESS);
    }

    assert(ma_splitter_node_init(&pGraph->nodeGraph, &splitterConfig, NULL, &pGraph->splitter) == MA_SUCCESS);

    branchConfig.vtable = &g_passthrough_vtable;
    branchConfig.pInputChannels = &channels;
    branchConfig.pOutputChannels = &channels;
    assert(ma_node_init(&pGraph->nodeGraph, &branchConfig, NULL, &pGraph->branch) == MA_SUCCESS);

    assert(ma_node_attach_output_bus(&pGraph->sourceNode, 0, &pGraph->splitter, 0) == MA_SUCCESS);
    if (!reverseAttachOrder) {
        assert(ma_node_attach_output_bus(&pGraph->splitter, 0, ma_node_graph_get_endpoint(&pGraph->nodeGraph), 0) == MA_SUCCESS);
        assert(ma_node_attach_output_bus(&pGraph->splitter, 1, &pGraph->branch, 0) == MA_SUCCESS);
    } else {
        assert(ma_node_attach_output_bus(&pGraph->splitter, 1, &pGraph->branch, 0) == MA_SUCCESS);
        assert(ma_node_attach_output_bus(&pGraph->splitter, 0, ma_node_graph_get_endpoint(&pGraph->nodeGraph), 0) == MA_SUCCESS);
    }
    assert(ma_node_attach_output_bus(&pGraph->branch, 0, ma_node_graph_get_endpoint(&pGraph->nodeGraph), 0) == MA_SUCCESS);

    assert(ma_node_set_output_bus_volume(&pGraph->splitter, 0, 0.25f) == MA_SUCCESS);
    assert(ma_node_set_output_bus_volume(&pGraph->branch, 0, 0.75f) == MA_SUCCESS);

    if (branchBeg != (ma_uint64)-1) {
        assert(ma_node_set_state_time(&pGraph->branch, ma_node_state_started, branchBeg) == MA_SUCCESS);
    }
    if (branchEnd != (ma_uint64)-1) {
        assert(ma_node_set_state_time(&pGraph->branch, ma_node_state_stopped, branchEnd) == MA_SUCCESS);
    }
}

static void fanout_mono_uninit(fanout_mono_graph* pGraph)
{
    ma_node_uninit(&pGraph->branch, NULL);
    ma_splitter_node_uninit(&pGraph->splitter, NULL);
    ma_data_source_node_uninit(&pGraph->sourceNode, NULL);
    counting_source_uninit(&pGraph->source);
    ma_node_graph_uninit(&pGraph->nodeGraph, NULL);
}

/* Read totalFrames using the given (possibly unequal) chunk sizes and verify every frame and the guard sentinel. */
static void read_and_verify_mono(const char* tag, fanout_mono_graph* pGraph, const int* chunkSizes, int chunkCount,
                                 int totalFrames, ma_uint64 windowBeg, ma_uint64 windowEnd)
{
    enum { CAPACITY = 256 };
    float frames[CAPACITY];
    const float sentinel = -777.0f;
    int framesDelivered = 0;
    int iChunk;
    int iFrame;

    assert((ma_uint32)(totalFrames + 16) <= CAPACITY);
    for (iFrame = 0; iFrame < totalFrames + 16; iFrame += 1) {
        frames[iFrame] = sentinel;   /* Guard region after the requested range. */
    }

    for (iChunk = 0; iChunk < chunkCount && framesDelivered < totalFrames; iChunk += 1) {
        int framesRequested = chunkSizes[iChunk];
        ma_uint64 framesRead = 0;

        if (framesDelivered + framesRequested > totalFrames) {
            framesRequested = totalFrames - framesDelivered;
        }

        assert(ma_node_graph_read_pcm_frames(&pGraph->nodeGraph, &frames[framesDelivered], (ma_uint64)framesRequested, &framesRead) == MA_SUCCESS);
        CHECK((int)framesRead == framesRequested);  /* Returned frame count must match this request. */
        framesDelivered += (int)framesRead;
    }

    CHECK(framesDelivered == totalFrames);          /* Write range must match the request. */

    for (iFrame = 0; iFrame < totalFrames; iFrame += 1) {
        float expected = reference_sample((ma_uint64)iFrame, 0, windowBeg, windowEnd);
        if (fabsf(frames[iFrame] - expected) > 1e-6f) {
            printf("  FAIL: %s frame %d got %.8f expected %.8f\n", tag, iFrame, frames[iFrame], expected);
            g_failures += 1;
        }
    }

    /* The branch starts at global frame 5: it must read x[5], not its own first frame x[0]. */
    if (windowBeg == 5 && totalFrames > 5) {
        CHECK(fabsf(frames[5] - 0.09375f) < 1e-7f);
    }
    /* The stop boundary is exclusive: frame 13 is back to the 0.25 path. */
    if (windowEnd == 13 && totalFrames > 13) {
        CHECK(fabsf(frames[13] - reference_sample(13, 0, windowBeg, windowEnd)) < 1e-7f);
    }

    /* Sentinel: nothing may be written past the requested range. */
    for (iFrame = totalFrames; iFrame < totalFrames + 16; iFrame += 1) {
        CHECK(frames[iFrame] == sentinel);
    }

    /* The single shared source is consumed exactly once. */
    CHECK(pGraph->source.framesPulled == (ma_uint64)totalFrames);
}

static int test_windows(void)
{
    static const int uneven[] = { 1, 3, 5, 2, 4, 1 };   /* Sums to 16. */
    static const int even[]   = { 8, 8 };
    int failuresBefore = g_failures;

    /* Delayed start (frame 5) plus early stop (frame 13, exclusive), both processing cache modes and both read patterns. */
    {
        fanout_mono_graph graph;
        fanout_mono_init(&graph, 0, 5, 13, 0);
        read_and_verify_mono("proc0 uneven", &graph, uneven, (int)(sizeof(uneven) / sizeof(uneven[0])), 16, 5, 13);
        fanout_mono_uninit(&graph);
    }
    {
        fanout_mono_graph graph;
        fanout_mono_init(&graph, 8, 5, 13, 0);
        read_and_verify_mono("proc8 uneven", &graph, uneven, (int)(sizeof(uneven) / sizeof(uneven[0])), 16, 5, 13);
        fanout_mono_uninit(&graph);
    }
    {
        fanout_mono_graph graph;
        fanout_mono_init(&graph, 0, 5, 13, 0);
        read_and_verify_mono("proc0 even", &graph, even, (int)(sizeof(even) / sizeof(even[0])), 16, 5, 13);
        fanout_mono_uninit(&graph);
    }
    {
        fanout_mono_graph graph;
        fanout_mono_init(&graph, 8, 5, 13, 0);
        read_and_verify_mono("proc8 even", &graph, even, (int)(sizeof(even) / sizeof(even[0])), 16, 5, 13);
        fanout_mono_uninit(&graph);
    }

    /* Start and stop exactly on internal processing block boundaries. */
    {
        fanout_mono_graph graph;
        ma_uint64 framesRead;
        float frames[32];
        int iFrame;

        fanout_mono_init(&graph, 8, 8, 16, 0);
        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, frames, 24, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 24);
        for (iFrame = 0; iFrame < 24; iFrame += 1) {
            CHECK(fabsf(frames[iFrame] - reference_sample((ma_uint64)iFrame, 0, 8, 16)) < 1e-6f);
        }
        CHECK(graph.source.framesPulled == 24);
        fanout_mono_uninit(&graph);
    }

    /* A zero-length read delivers nothing, pulls nothing and does not move the source. */
    {
        fanout_mono_graph graph;
        ma_uint64 framesRead = 123;
        float frames[8];

        fanout_mono_init(&graph, 8, 5, 13, 0);
        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, frames, 0, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 0);
        CHECK(graph.source.framesPulled == 0);
        CHECK(graph.source.readCallCount == 0);

        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, frames, 8, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 8);
        {
            int iFrame;
            for (iFrame = 0; iFrame < 8; iFrame += 1) {
                CHECK(fabsf(frames[iFrame] - reference_sample((ma_uint64)iFrame, 0, 5, 13)) < 1e-6f);
            }
        }
        CHECK(graph.source.framesPulled == 8);  /* Position is still global frame 8. */
        fanout_mono_uninit(&graph);
    }

    /* A fully stopped branch leaves only the 0.25 direct path, still from the one shared source. */
    {
        fanout_mono_graph graph;
        ma_uint64 framesRead;
        float frames[16];
        int iFrame;

        fanout_mono_init(&graph, 8, (ma_uint64)-1, (ma_uint64)-1, 0);
        assert(ma_node_set_state(&graph.branch, ma_node_state_stopped) == MA_SUCCESS);
        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, frames, 16, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 16);
        for (iFrame = 0; iFrame < 16; iFrame += 1) {
            float expected = (float)(iFrame + 1) / 64.0f * 0.25f;
            CHECK(fabsf(frames[iFrame] - expected) < 1e-6f);
        }
        CHECK(graph.source.framesPulled == 16);
        fanout_mono_uninit(&graph);
    }

    /* Asymmetric stereo: distinct L/R content proves frame offsets are whole frames, not single floats. */
    {
        ma_node_graph_config graphConfig = ma_node_graph_config_init(2);
        ma_node_graph nodeGraph;
        counting_source source;
        ma_data_source_node sourceNode;
        ma_splitter_node splitter;
        ma_node_base branch;
        ma_splitter_node_config splitterConfig = ma_splitter_node_config_init(2);
        ma_node_config branchConfig = ma_node_config_init();
        ma_uint32 channels = 2;
        ma_uint64 framesRead;
        float frames[64];
        int iFrame;

        graphConfig.processingSizeInFrames = 8;
        assert(ma_node_graph_init(&graphConfig, NULL, &nodeGraph) == MA_SUCCESS);
        counting_source_init(&source, 2, 4096);
        {
            ma_data_source_node_config sourceConfig = ma_data_source_node_config_init(&source.ds);
            assert(ma_data_source_node_init(&nodeGraph, &sourceConfig, NULL, &sourceNode) == MA_SUCCESS);
        }
        assert(ma_splitter_node_init(&nodeGraph, &splitterConfig, NULL, &splitter) == MA_SUCCESS);
        branchConfig.vtable = &g_passthrough_vtable;
        branchConfig.pInputChannels = &channels;
        branchConfig.pOutputChannels = &channels;
        assert(ma_node_init(&nodeGraph, &branchConfig, NULL, &branch) == MA_SUCCESS);

        assert(ma_node_attach_output_bus(&sourceNode, 0, &splitter, 0) == MA_SUCCESS);
        assert(ma_node_attach_output_bus(&splitter, 0, ma_node_graph_get_endpoint(&nodeGraph), 0) == MA_SUCCESS);
        assert(ma_node_attach_output_bus(&splitter, 1, &branch, 0) == MA_SUCCESS);
        assert(ma_node_attach_output_bus(&branch, 0, ma_node_graph_get_endpoint(&nodeGraph), 0) == MA_SUCCESS);
        assert(ma_node_set_output_bus_volume(&splitter, 0, 0.25f) == MA_SUCCESS);
        assert(ma_node_set_output_bus_volume(&branch, 0, 0.75f) == MA_SUCCESS);
        assert(ma_node_set_state_time(&branch, ma_node_state_started, 5) == MA_SUCCESS);
        assert(ma_node_set_state_time(&branch, ma_node_state_stopped, 13) == MA_SUCCESS);

        assert(ma_node_graph_read_pcm_frames(&nodeGraph, frames, 16, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 16);
        for (iFrame = 0; iFrame < 16; iFrame += 1) {
            ma_uint32 iChannel;
            for (iChannel = 0; iChannel < 2; iChannel += 1) {
                float expected = reference_sample((ma_uint64)iFrame, iChannel, 5, 13);
                CHECK(fabsf(frames[(iFrame * 2) + iChannel] - expected) < 1e-6f);
            }
        }
        CHECK(source.framesPulled == 16);

        ma_node_uninit(&branch, NULL);
        ma_splitter_node_uninit(&splitter, NULL);
        ma_data_source_node_uninit(&sourceNode, NULL);
        counting_source_uninit(&source);
        ma_node_graph_uninit(&nodeGraph, NULL);
    }

    return g_failures == failuresBefore ? 0 : 1;
}

static int test_order(void)
{
    static const int uneven[] = { 1, 3, 5, 2, 4, 1 };
    int failuresBefore = g_failures;

    /* Reversing the order branches attach to the endpoint must not change the assembled PCM. */
    {
        fanout_mono_graph forward;
        fanout_mono_graph reversed;
        float forwardFrames[32];
        float reversedFrames[32];
        ma_uint64 forwardRead = 0;
        ma_uint64 reversedRead = 0;
        int delivered = 0;
        int iChunk;

        fanout_mono_init(&forward, 8, 5, 13, 0);
        fanout_mono_init(&reversed, 8, 5, 13, 1);

        for (iChunk = 0; iChunk < (int)(sizeof(uneven) / sizeof(uneven[0])); iChunk += 1) {
            ma_uint64 a = 0;
            ma_uint64 b = 0;
            assert(ma_node_graph_read_pcm_frames(&forward.nodeGraph, &forwardFrames[delivered], uneven[iChunk], &a) == MA_SUCCESS);
            assert(ma_node_graph_read_pcm_frames(&reversed.nodeGraph, &reversedFrames[delivered], uneven[iChunk], &b) == MA_SUCCESS);
            forwardRead += a;
            reversedRead += b;
            delivered += uneven[iChunk];
        }

        CHECK(forwardRead == 16);
        CHECK(reversedRead == 16);
        CHECK(memcmp(forwardFrames, reversedFrames, sizeof(forwardFrames[0]) * 16) == 0);
        CHECK(forward.source.framesPulled == 16);
        CHECK(reversed.source.framesPulled == 16);

        fanout_mono_uninit(&forward);
        fanout_mono_uninit(&reversed);
    }

    /* Two branches with different start/stop windows off one three-bus splitter and one shared source. */
    {
        ma_node_graph_config graphConfig = ma_node_graph_config_init(1);
        ma_node_graph nodeGraph;
        counting_source source;
        ma_data_source_node sourceNode;
        ma_splitter_node splitter;
        ma_splitter_node_config splitterConfig = ma_splitter_node_config_init(1);
        ma_node_base branch1;
        ma_node_base branch2;
        ma_node_config branchConfig = ma_node_config_init();
        ma_uint32 channels = 1;
        ma_uint64 framesRead;
        float frames[32];
        int iFrame;

        graphConfig.processingSizeInFrames = 8;
        assert(ma_node_graph_init(&graphConfig, NULL, &nodeGraph) == MA_SUCCESS);
        counting_source_init(&source, 1, 4096);
        {
            ma_data_source_node_config sourceConfig = ma_data_source_node_config_init(&source.ds);
            assert(ma_data_source_node_init(&nodeGraph, &sourceConfig, NULL, &sourceNode) == MA_SUCCESS);
        }
        splitterConfig.outputBusCount = 3;
        assert(ma_splitter_node_init(&nodeGraph, &splitterConfig, NULL, &splitter) == MA_SUCCESS);

        branchConfig.vtable = &g_passthrough_vtable;
        branchConfig.pInputChannels = &channels;
        branchConfig.pOutputChannels = &channels;
        assert(ma_node_init(&nodeGraph, &branchConfig, NULL, &branch1) == MA_SUCCESS);
        assert(ma_node_init(&nodeGraph, &branchConfig, NULL, &branch2) == MA_SUCCESS);

        assert(ma_node_attach_output_bus(&sourceNode, 0, &splitter, 0) == MA_SUCCESS);
        assert(ma_node_attach_output_bus(&splitter, 0, ma_node_graph_get_endpoint(&nodeGraph), 0) == MA_SUCCESS);
        assert(ma_node_attach_output_bus(&splitter, 1, &branch1, 0) == MA_SUCCESS);
        assert(ma_node_attach_output_bus(&branch1, 0, ma_node_graph_get_endpoint(&nodeGraph), 0) == MA_SUCCESS);
        assert(ma_node_attach_output_bus(&splitter, 2, &branch2, 0) == MA_SUCCESS);
        assert(ma_node_attach_output_bus(&branch2, 0, ma_node_graph_get_endpoint(&nodeGraph), 0) == MA_SUCCESS);

        /* Direct 0.2 always; branch1 adds 0.3 over [3,9); branch2 adds 0.5 over [7,14). */
        assert(ma_node_set_output_bus_volume(&splitter, 0, 0.2f) == MA_SUCCESS);
        assert(ma_node_set_output_bus_volume(&branch1, 0, 0.3f) == MA_SUCCESS);
        assert(ma_node_set_output_bus_volume(&branch2, 0, 0.5f) == MA_SUCCESS);
        assert(ma_node_set_state_time(&branch1, ma_node_state_started, 3) == MA_SUCCESS);
        assert(ma_node_set_state_time(&branch1, ma_node_state_stopped, 9) == MA_SUCCESS);
        assert(ma_node_set_state_time(&branch2, ma_node_state_started, 7) == MA_SUCCESS);
        assert(ma_node_set_state_time(&branch2, ma_node_state_stopped, 14) == MA_SUCCESS);

        assert(ma_node_graph_read_pcm_frames(&nodeGraph, frames, 16, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 16);
        for (iFrame = 0; iFrame < 16; iFrame += 1) {
            float base = (float)(iFrame + 1) / 64.0f;
            float gain = 0.2f;
            if (iFrame >= 3 && iFrame < 9)  gain += 0.3f;
            if (iFrame >= 7 && iFrame < 14) gain += 0.5f;
            CHECK(fabsf(frames[iFrame] - (base * gain)) < 1e-6f);
        }
        CHECK(source.framesPulled == 16);  /* Three buses, one shared pull. */

        ma_node_uninit(&branch2, NULL);
        ma_node_uninit(&branch1, NULL);
        ma_splitter_node_uninit(&splitter, NULL);
        ma_data_source_node_uninit(&sourceNode, NULL);
        counting_source_uninit(&source);
        ma_node_graph_uninit(&nodeGraph, NULL);
    }

    return g_failures == failuresBefore ? 0 : 1;
}

static int test_processing_cache(void)
{
    int failuresBefore = g_failures;

    /*
    Fixed processing size: a small request fills one whole internal block into the graph's
    processing cache. Continuing to drain that cache must not pull the source again.
    */
    {
        fanout_mono_graph graph;
        ma_uint64 framesRead;
        float frames[16];
        int iFrame;
        ma_uint64 pulledAfterFirstRead;

        fanout_mono_init(&graph, 8, 5, 13, 0);

        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, frames, 3, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 3);
        pulledAfterFirstRead = graph.source.framesPulled;
        CHECK(pulledAfterFirstRead == 8);  /* One 8-frame internal block was produced. */

        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, &frames[3], 5, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 5);
        CHECK(graph.source.framesPulled == pulledAfterFirstRead);  /* Served from cache, no new source pull. */

        for (iFrame = 0; iFrame < 8; iFrame += 1) {
            CHECK(fabsf(frames[iFrame] - reference_sample((ma_uint64)iFrame, 0, 5, 13)) < 1e-6f);
        }

        /* Crossing into the next block pulls the next contiguous range of the one source. */
        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, &frames[8], 8, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 8);
        CHECK(graph.source.framesPulled == 16);
        for (iFrame = 8; iFrame < 16; iFrame += 1) {
            CHECK(fabsf(frames[iFrame] - reference_sample((ma_uint64)iFrame, 0, 5, 13)) < 1e-6f);
        }

        fanout_mono_uninit(&graph);
    }

    /* Processing size zero: the same scenario runs without a fixed block size and stays single-pull. */
    {
        fanout_mono_graph graph;
        ma_uint64 framesRead;
        float frames[16];
        int iFrame;

        fanout_mono_init(&graph, 0, 5, 13, 0);
        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, frames, 3, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 3);
        CHECK(graph.source.framesPulled == 3);
        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, &frames[3], 13, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 13);
        CHECK(graph.source.framesPulled == 16);
        for (iFrame = 0; iFrame < 16; iFrame += 1) {
            CHECK(fabsf(frames[iFrame] - reference_sample((ma_uint64)iFrame, 0, 5, 13)) < 1e-6f);
        }
        fanout_mono_uninit(&graph);
    }

    /* A large request spanning several internal windows still reads the shared source exactly once per frame. */
    {
        fanout_mono_graph graph;
        enum { TOTAL = 1000 };
        static float frames[TOTAL + 16];
        ma_uint64 framesRead;
        int iFrame;
        const float sentinel = -777.0f;

        for (iFrame = 0; iFrame < TOTAL + 16; iFrame += 1) {
            frames[iFrame] = sentinel;
        }

        fanout_mono_init(&graph, 0, 500, 1500, 0);
        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, frames, TOTAL, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == TOTAL);
        CHECK(graph.source.framesPulled == TOTAL);
        for (iFrame = 0; iFrame < TOTAL; iFrame += 1) {
            CHECK(fabsf(frames[iFrame] - reference_sample((ma_uint64)iFrame, 0, 500, 1500)) < 1e-5f);
        }
        for (iFrame = TOTAL; iFrame < TOTAL + 16; iFrame += 1) {
            CHECK(frames[iFrame] == sentinel);
        }
        fanout_mono_uninit(&graph);
    }

    return g_failures == failuresBefore ? 0 : 1;
}

static int test_reattach(void)
{
    int failuresBefore = g_failures;

    /*
    Detach a branch on a fully consumed internal block boundary, then re-attach it. The next read
    must continue at the current track position rather than reuse cache from before the detach.
    */
    {
        fanout_mono_graph graph;
        ma_uint64 framesRead;
        float firstBlock[8];
        float secondBlock[8];
        int iFrame;

        fanout_mono_init(&graph, 8, 5, 13, 0);

        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, firstBlock, 8, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 8);
        CHECK(graph.source.framesPulled == 8);
        for (iFrame = 0; iFrame < 8; iFrame += 1) {
            CHECK(fabsf(firstBlock[iFrame] - reference_sample((ma_uint64)iFrame, 0, 5, 13)) < 1e-6f);
        }

        assert(ma_node_detach_output_bus(&graph.branch, 0) == MA_SUCCESS);
        assert(ma_node_attach_output_bus(&graph.branch, 0, ma_node_graph_get_endpoint(&graph.nodeGraph), 0) == MA_SUCCESS);

        /* Global frames 8..15: branch window [5,13) is active for 8..12 and inactive from 13. */
        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, secondBlock, 8, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 8);
        CHECK(graph.source.framesPulled == 16);  /* Continues at the current position, no replay. */
        for (iFrame = 0; iFrame < 8; iFrame += 1) {
            ma_uint64 globalFrame = 8 + (ma_uint64)iFrame;
            CHECK(fabsf(secondBlock[iFrame] - reference_sample(globalFrame, 0, 5, 13)) < 1e-6f);
        }

        fanout_mono_uninit(&graph);
    }

    /* A detached branch contributes silence while it is gone; the direct path and the source position keep advancing. */
    {
        fanout_mono_graph graph;
        ma_uint64 framesRead;
        float firstBlock[8];
        float detachedBlock[8];
        int iFrame;

        fanout_mono_init(&graph, 8, (ma_uint64)-1, (ma_uint64)-1, 0);  /* Branch always scheduled. */
        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, firstBlock, 8, &framesRead) == MA_SUCCESS);
        CHECK(graph.source.framesPulled == 8);

        assert(ma_node_detach_output_bus(&graph.branch, 0) == MA_SUCCESS);
        assert(ma_node_graph_read_pcm_frames(&graph.nodeGraph, detachedBlock, 8, &framesRead) == MA_SUCCESS);
        CHECK(framesRead == 8);
        CHECK(graph.source.framesPulled == 16);
        for (iFrame = 0; iFrame < 8; iFrame += 1) {
            ma_uint64 globalFrame = 8 + (ma_uint64)iFrame;
            float expected = (float)(globalFrame + 1) / 64.0f * 0.25f;  /* Only the direct bus remains. */
            CHECK(fabsf(detachedBlock[iFrame] - expected) < 1e-6f);
        }

        fanout_mono_uninit(&graph);
    }

    return g_failures == failuresBefore ? 0 : 1;
}

int main(void)
{
    int result = 0;

    result |= test_windows();
    if (g_failures == 0) {
        puts("FANOUT_WINDOWS_OK");
    }

    result |= test_order();
    if (g_failures == 0) {
        puts("FANOUT_ORDER_OK");
    }

    result |= test_processing_cache();
    if (g_failures == 0) {
        puts("FANOUT_PROCESSING_CACHE_OK");
    }

    result |= test_reattach();
    if (g_failures == 0) {
        puts("FANOUT_REATTACH_OK");
    }

    if (g_failures != 0) {
        printf("FANOUT_CONTRACT_FAILURES=%d\n", g_failures);
    }

    return result;
}
