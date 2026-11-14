/*
dr_flac_timeline_test.c - Regression tests for drflac_set_preserve_sample_timeline().

All test streams are produced by the auditable generator in flac_gen.h (verbatim FLAC frames with real CRC-8/CRC-16, no external
encoder). Every check compares individual samples: valid frames must match the generator's exact waveform and hole samples must be
exact zeros. The tests also exercise all three output formats, NULL-buffer skip reads, seeking in original sample coordinates,
output buffer guards, and the original frame-skipping behavior with the mode disabled.
*/
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#include "dr_flac.h"

#include "flac_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, msg) do { \
    g_checks += 1; \
    if (!(cond)) { g_failures += 1; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); } \
} while (0)

#define MAX_CHANNELS 4
#define MAX_SAMPLES 256

static int expected_s16(unsigned long long frame, int channel)
{
    return flac_gen_sample(frame, channel);
}

static int expected_s32(unsigned long long frame, int channel)
{
    return flac_gen_sample(frame, channel) * 65536;
}

static float expected_f32(unsigned long long frame, int channel)
{
    return (float)flac_gen_sample(frame, channel) / 32768.0f;
}

/* Fills a guard region around buf and returns the guard fill so callers can verify it was untouched. */
static unsigned char guard_fill(void* buf, size_t bytes)
{
    memset(buf, 0xA5, bytes);
    return 0xA5;
}

/* Verifies a decoded timeline: zero frames in [zeroFirst,zeroLast), exact generator samples everywhere else. */
static void verify_timeline_s32(const drflac_int32* got, int channels, int totalFrames,
                                int zeroFirst, int zeroLast, const char* tag)
{
    int f;
    for (f = 0; f < totalFrames; ++f) {
        int c;
        for (c = 0; c < channels; ++c) {
            int want = (f >= zeroFirst && f < zeroLast) ? 0 : expected_s32((unsigned long long)f, c);
            if (got[(long long)f * channels + c] != want) {
                printf("  FAIL %s: frame %d ch %d got %d want %d\n", tag, f, c,
                       got[(long long)f * channels + c], want);
                g_failures += 1;
                return;
            }
        }
    }
    g_checks += 1;
}

static void verify_timeline_s16(const drflac_int16* got, int channels, int totalFrames,
                                int zeroFirst, int zeroLast, const char* tag)
{
    int f;
    for (f = 0; f < totalFrames; ++f) {
        int c;
        for (c = 0; c < channels; ++c) {
            int want = (f >= zeroFirst && f < zeroLast) ? 0 : expected_s16((unsigned long long)f, c);
            if ((int)got[(long long)f * channels + c] != want) {
                printf("  FAIL %s: frame %d ch %d got %d want %d\n", tag, f, c,
                       (int)got[(long long)f * channels + c], want);
                g_failures += 1;
                return;
            }
        }
    }
    g_checks += 1;
}

/* Fixed block size stream: blockSize-sized frames, frame k declares coordinate k. */
static unsigned int build_fixed(flac_gen_bw* bw, int channels, const int* blockSizes, int frameCount,
                                int totalSamples, flac_gen_frame_span* spans, unsigned long long* coords)
{
    int k;
    for (k = 0; k < frameCount; ++k) {
        coords[k] = (unsigned long long)k;
    }
    return flac_gen_build(bw, 0, channels, blockSizes, coords, frameCount, totalSamples, spans);
}

static unsigned int build_variable(flac_gen_bw* bw, int channels, const int* blockSizes, int frameCount,
                                   int totalSamples, flac_gen_frame_span* spans, unsigned long long* coords)
{
    int k;
    unsigned long long running = 0;
    for (k = 0; k < frameCount; ++k) {
        coords[k] = running;
        running += (unsigned long long)blockSizes[k];
    }
    return flac_gen_build(bw, 1, channels, blockSizes, coords, frameCount, totalSamples, spans);
}

/* T1: fixed block size, one CRC-bad frame in the middle; mode off skips, mode on fills exact silence. */
static void test_fixed_single_bad_frame(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 16};
    unsigned int size;
    drflac_int32 bulk[MAX_SAMPLES * MAX_CHANNELS];
    drflac_int32 oneAtATime[MAX_SAMPLES * MAX_CHANNELS];
    drflac* d;
    drflac_uint64 n;
    int f;

    printf("test_fixed_single_bad_frame\n");
    size = build_fixed(&bw, 2, blockSizes, 3, 48, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[1]);

    /* Mode disabled: the historical behavior is to skip the bad frame entirely (48 declared -> 32 delivered). */
    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(d != NULL, "open (mode off)");
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_FALSE) == DRFLAC_TRUE, "explicit disable accepted");
    n = drflac_read_pcm_frames_s32(d, 48, bulk);
    CHECK(n == 32, "mode off delivers 32 frames after skipped bad frame");
    drflac_close(d);

    /* Mode enabled: 48 frames, zeros exactly on [16,32). */
    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(d != NULL, "open (mode on)");
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    n = drflac_read_pcm_frames_s32(d, 48, bulk);
    CHECK(n == 48, "mode on delivers all 48 frames");
    CHECK(d->currentPCMFrame == 48, "currentPCMFrame ends at 48");
    verify_timeline_s32(bulk, 2, 48, 16, 32, "fixed single bad s32");
    drflac_close(d);

    /* One frame per read must produce the identical timeline and currentPCMFrame sequence. */
    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted (per-frame)");
    for (f = 0; f < 48; ++f) {
        drflac_int32 pair[2];
        drflac_uint64 got = drflac_read_pcm_frames_s32(d, 1, pair);
        CHECK(got == 1, "per-frame read returns one frame");
        CHECK(d->currentPCMFrame == (drflac_uint64)(f + 1), "per-frame currentPCMFrame");
        oneAtATime[f * 2 + 0] = pair[0];
        oneAtATime[f * 2 + 1] = pair[1];
    }
    CHECK(memcmp(bulk, oneAtATime, sizeof(drflac_int32) * 96u) == 0, "per-frame equals bulk read");
    drflac_close(d);
}

/* T2: two adjacent CRC-bad frames form one wider hole. */
static void test_fixed_two_adjacent_bad_frames(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[4] = {16, 16, 16, 16};
    unsigned int size;
    drflac_int32 out[MAX_SAMPLES * MAX_CHANNELS];
    drflac* d;

    printf("test_fixed_two_adjacent_bad_frames\n");
    size = build_fixed(&bw, 2, blockSizes, 4, 64, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[1]);
    flac_gen_corrupt_frame_crc16(&bw, &spans[2]);

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 64, out) == 64, "all 64 frames delivered");
    verify_timeline_s32(out, 2, 64, 16, 48, "adjacent bad frames hole [16,48)");
    drflac_close(d);
}

/* T3: a completely deleted frame is a hole confirmed by the frame after it. */
static void test_fixed_deleted_frame(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[4] = {16, 16, 16, 16};
    unsigned int size;
    drflac_int32 out[MAX_SAMPLES * MAX_CHANNELS];
    drflac* d;

    printf("test_fixed_deleted_frame\n");
    size = build_fixed(&bw, 2, blockSizes, 4, 64, spans, coords);
    size = flac_gen_delete_frame(&bw, &spans[2]);  /* samples 32..47 vanish from the file */

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 64, out) == 64, "deleted frame replaced on timeline");
    verify_timeline_s32(out, 2, 64, 32, 48, "deleted frame hole [32,48)");
    drflac_close(d);
}

/* T4: a legal short final block still lands on its declared coordinate after a hole. */
static void test_fixed_short_tail_block(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 8};
    unsigned int size;
    drflac_int32 out[MAX_SAMPLES * MAX_CHANNELS];
    drflac* d;

    printf("test_fixed_short_tail_block\n");

    /* Clean stream first: 40 frames, last frame only 8 samples. */
    size = build_fixed(&bw, 2, blockSizes, 3, 40, spans, coords);
    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 40, out) == 40, "clean short-tail stream reads 40");
    verify_timeline_s32(out, 2, 40, -1, -1, "clean short tail has no zeros");
    drflac_close(d);

    /* Corrupt the middle frame; the 8-sample tail must resume at sample 32. */
    size = build_fixed(&bw, 2, blockSizes, 3, 40, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[1]);
    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 40, out) == 40, "short-tail hole stream reads 40");
    verify_timeline_s32(out, 2, 40, 16, 32, "short-tail hole [16,32)");
    CHECK(out[32 * 2] == expected_s32(32, 0), "tail first sample lands at frame 32");
    CHECK(out[39 * 2 + 1] == expected_s32(39, 1), "tail last sample lands at frame 39");
    drflac_close(d);
}

/* T5: variable block sizes with multiple channels; holes use absolute sample numbers. */
static void test_variable_multichannel(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[4] = {4, 6, 8, 3};   /* absolute starts: 0, 4, 10, 18; total 21 */
    unsigned int size;
    drflac_int32 out[MAX_SAMPLES * MAX_CHANNELS];
    drflac* d;

    printf("test_variable_multichannel\n");
    size = build_variable(&bw, 3, blockSizes, 4, 21, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[2]);   /* hole covers absolute samples [10,18) */

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 21, out) == 21, "variable stream reads 21");
    verify_timeline_s32(out, 3, 21, 10, 18, "variable hole [10,18)");
    drflac_close(d);

    /* Deleting a variable block frame works the same way. */
    size = build_variable(&bw, 3, blockSizes, 4, 21, spans, coords);
    size = flac_gen_delete_frame(&bw, &spans[1]);   /* samples [4,10) missing */
    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 21, out) == 21, "variable deletion reads 21");
    verify_timeline_s32(out, 3, 21, 4, 10, "variable deleted hole [4,10)");
    drflac_close(d);
}

/* T6: pseudo sync inside a frame whose header CRC-8 is fine; only CRC-16 may reject it. */
static void test_pseudo_sync_header_crc8_ok(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 16};
    unsigned int size;
    drflac_int32 out[MAX_SAMPLES * MAX_CHANNELS];
    drflac* d;
    drflac_uint64 n;

    printf("test_pseudo_sync_header_crc8_ok\n");
    size = build_fixed(&bw, 2, blockSizes, 3, 48, spans, coords);
    flac_gen_make_pseudo_sync_frame(&bw, &spans[1]);

    /* Mode off: the frame must still be rejected (by CRC-16), never partially delivered. */
    d = drflac_open_memory(bw.data, size, NULL);
    n = drflac_read_pcm_frames_s32(d, 48, out);
    CHECK(n == 32, "pseudo sync frame rejected, mode off skips to 32");
    drflac_close(d);

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 48, out) == 48, "pseudo sync replaced by silence on timeline");
    verify_timeline_s32(out, 2, 48, 16, 32, "pseudo sync hole [16,32)");
    drflac_close(d);
}

/* T7: damage at the tail with no valid frame on the right yields only the confirmed prefix, never guessed silence. */
static void test_tail_damage_no_right_frame(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 16};
    unsigned int size;
    drflac_int32 out[MAX_SAMPLES * MAX_CHANNELS];
    drflac* d;

    printf("test_tail_damage_no_right_frame\n");
    size = build_fixed(&bw, 2, blockSizes, 3, 48, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[2]);

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 48, out) == 32, "only the 32 confirmed frames are returned");
    CHECK(d->currentPCMFrame == 32, "position stops at 32; no STREAMINFO-guessed tail");
    /* A second read must return nothing rather than fabricate silence. */
    CHECK(drflac_read_pcm_frames_s32(d, 16, out) == 0, "no further samples after unconfirmed tail");
    drflac_close(d);
}

/* T8: seeking uses original coordinates inside holes, good frames, forward/backward/zero, across all formats and NULL skips. */
static void test_seeking_and_formats(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 16};
    unsigned int size;
    drflac* d;
    drflac_int32 s32[32];
    drflac_int16 s16[8];
    float f32[8];
    drflac_uint64 n;

    printf("test_seeking_and_formats\n");
    size = build_fixed(&bw, 2, blockSizes, 3, 48, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[1]);   /* hole [16,32) */

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");

    /* Seek into the middle of the hole: the next samples are silence, then the right frame resumes at 32. */
    CHECK(drflac_seek_to_pcm_frame(d, 20) == DRFLAC_TRUE, "seek into hole succeeds");
    CHECK(d->currentPCMFrame == 20, "seek sets currentPCMFrame 20");
    n = drflac_read_pcm_frames_s32(d, 4, s32);
    CHECK(n == 4, "read 4 in hole");
    CHECK(s32[0] == 0 && s32[7] == 0, "hole samples are zero");
    CHECK(d->currentPCMFrame == 24, "position advanced inside hole");
    n = drflac_read_pcm_frames_s32(d, 12, s32);     /* 8 more zeros then frame 3 audio at 32 */
    CHECK(n == 12, "read across hole boundary");
    CHECK(s32[8 * 2] == expected_s32(32, 0) && s32[8 * 2 + 1] == expected_s32(32, 1), "frame 3 resumes at sample 32");
    CHECK(d->currentPCMFrame == 36, "position after boundary read");

    /* Seek backward to a good frame: exact first sample, no duplication or swallowing. */
    CHECK(drflac_seek_to_pcm_frame(d, 5) == DRFLAC_TRUE, "backward seek");
    n = drflac_read_pcm_frames_s16(d, 2, s16);
    CHECK(n == 2 && s16[0] == expected_s16(5, 0) && s16[3] == expected_s16(6, 1), "backward seek first samples");

    /* Seek back to zero and confirm the whole timeline is reproducible byte for byte. */
    CHECK(drflac_seek_to_pcm_frame(d, 0) == DRFLAC_TRUE, "seek to zero");
    {
        drflac_int32 full[MAX_SAMPLES * 2];
        CHECK(drflac_read_pcm_frames_s32(d, 48, full) == 48, "reread full stream");
        verify_timeline_s32(full, 2, 48, 16, 32, "reread after seek(0)");
    }

    /* NULL-buffer skip read advances through the hole without writing, landing on the right frame. */
    CHECK(drflac_seek_to_pcm_frame(d, 0) == DRFLAC_TRUE, "seek to zero again");
    CHECK(drflac_read_pcm_frames_s32(d, 20, NULL) == 20, "NULL skip 20");
    CHECK(d->currentPCMFrame == 20, "position 20 after NULL skip");
    CHECK(drflac_read_pcm_frames_s32(d, 10, NULL) == 10, "NULL skip 10 more");
    CHECK(d->currentPCMFrame == 30, "position 30 after second NULL skip");
    n = drflac_read_pcm_frames_f32(d, 4, f32);       /* 2 zeros (30,31) then audio 32,33 */
    CHECK(n == 4, "f32 read after NULL skips");
    CHECK(f32[0] == 0.0f && f32[2] == 0.0f, "remaining hole silence in f32");
    CHECK(f32[4] == expected_f32(32, 0), "f32 audio resumes at 32");

    /* Alternating formats share one timeline; hole progress must not reset between calls. */
    CHECK(drflac_seek_to_pcm_frame(d, 14) == DRFLAC_TRUE, "seek to 14 for mixed formats");
    n = drflac_read_pcm_frames_s16(d, 4, s16);      /* 14,15 good; 16,17 zero */
    CHECK(n == 4 && s16[0] == expected_s16(14, 0) && s16[4] == 0, "s16 enters hole at 16");
    n = drflac_read_pcm_frames_f32(d, 4, f32);      /* 18..21 still zero, same hole */
    CHECK(n == 4 && f32[0] == 0.0f && f32[6] == 0.0f, "f32 continues same hole");
    n = drflac_read_pcm_frames_s32(d, 4, s32);      /* 22..25 still zero */
    CHECK(n == 4 && s32[0] == 0 && s32[6] == 0, "s32 still in same hole");
    CHECK(d->currentPCMFrame == 26, "mixed formats advanced one timeline to 26");
    drflac_close(d);

    /* Variable block stream: seek into an absolute-sample hole and out the other side. */
    {
        int vblocks[4] = {4, 6, 8, 3};
        flac_gen_frame_span vspans[8];
        unsigned long long vcoords[8];
        drflac_int32 vout[32];
        size = build_variable(&bw, 3, vblocks, 4, 21, vspans, vcoords);
        flac_gen_corrupt_frame_crc16(&bw, &vspans[2]);  /* hole [10,18) */
        d = drflac_open_memory(bw.data, size, NULL);
        CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted (variable)");
        CHECK(drflac_seek_to_pcm_frame(d, 12) == DRFLAC_TRUE, "variable seek into hole");
        n = drflac_read_pcm_frames_s32(d, 9, vout);     /* silence 12..17, then frame at 18..20 */
        CHECK(n == 9, "variable read across hole");
        CHECK(vout[0] == 0 && vout[5 * 3 + 2] == 0, "variable hole zeros");
        CHECK(vout[6 * 3] == expected_s32(18, 0), "variable frame resumes at absolute 18");
        drflac_close(d);
    }
}

/* T9: setter gating - unknown length, late calls, headerless streams, NULL, and no side effects on rejection. */
static void test_setter_gating(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[2] = {16, 16};
    unsigned int size;
    drflac* d;
    drflac_int32 out[64];

    printf("test_setter_gating\n");
    CHECK(drflac_set_preserve_sample_timeline(NULL, DRFLAC_TRUE) == DRFLAC_FALSE, "NULL decoder rejected");

    /* STREAMINFO with a zero total sample count cannot define a timeline. */
    size = build_fixed(&bw, 2, blockSizes, 2, 0, spans, coords);
    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(d != NULL, "open unknown-length stream");
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_FALSE, "zero total samples rejected");
    CHECK(d->timelinePreserveEnabled == 0, "object unchanged after rejection");
    CHECK(drflac_read_pcm_frames_s32(d, 32, out) == 32, "rejected object still reads normally");
    drflac_close(d);

    /* The window closes after the first read and after the first seek. */
    size = build_fixed(&bw, 2, blockSizes, 2, 32, spans, coords);
    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_read_pcm_frames_s32(d, 1, out) == 1, "priming read");
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_FALSE, "enable after read rejected");
    drflac_close(d);

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_seek_to_pcm_frame(d, 1) == DRFLAC_TRUE, "priming seek");
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_FALSE, "enable after seek rejected");
    drflac_close(d);

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_FALSE) == DRFLAC_TRUE, "disable accepted up front");
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "re-enable accepted before any read");
    CHECK(d->timelinePreserveEnabled == 1, "mode finally enabled");
    drflac_close(d);
}

/* Headerless (relaxed, no STREAMINFO) opens are rejected. */
typedef struct
{
    const unsigned char* data;
    size_t dataSize;
    size_t currentReadPos;
} test_mem_stream;

static size_t relaxed_read(void* pUserData, void* bufferOut, size_t bytesToRead)
{
    test_mem_stream* ms = (test_mem_stream*)pUserData;
    size_t avail = ms->dataSize - ms->currentReadPos;
    if (bytesToRead > avail) bytesToRead = avail;
    memcpy(bufferOut, ms->data + ms->currentReadPos, bytesToRead);
    ms->currentReadPos += bytesToRead;
    return bytesToRead;
}

static drflac_bool32 relaxed_seek(void* pUserData, int offset, drflac_seek_origin origin)
{
    test_mem_stream* ms = (test_mem_stream*)pUserData;
    long target;
    if (origin == DRFLAC_SEEK_SET) target = offset;
    else if (origin == DRFLAC_SEEK_CUR) target = (long)ms->currentReadPos + offset;
    else target = (long)ms->dataSize + offset;
    if (target < 0 || (size_t)target > ms->dataSize) return DRFLAC_FALSE;
    ms->currentReadPos = (size_t)target;
    return DRFLAC_TRUE;
}

static drflac_bool32 relaxed_tell(void* pUserData, drflac_int64* pCursor)
{
    *pCursor = (drflac_int64)((test_mem_stream*)pUserData)->currentReadPos;
    return DRFLAC_TRUE;
}

static void test_headerless_rejected(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[4];
    unsigned long long coords[4];
    int blockSizes[2] = {16, 16};
    unsigned int size;
    test_mem_stream ms;
    drflac* d;

    printf("test_headerless_rejected\n");
    size = build_fixed(&bw, 1, blockSizes, 2, 32, spans, coords);
    ms.data = bw.data + spans[0].start;   /* feed raw frames with no fLaC marker or STREAMINFO */
    ms.dataSize = size - spans[0].start;
    ms.currentReadPos = 0;

    d = drflac_open_relaxed(relaxed_read, relaxed_seek, relaxed_tell, drflac_container_native, &ms, NULL);
    CHECK(d != NULL, "relaxed headerless open succeeds");
    if (d != NULL) {
        CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_FALSE, "headerless stream rejected");
        CHECK(d->timelinePreserveEnabled == 0, "headerless object unchanged");
        drflac_close(d);
    }
}

/* T10: illegal frame coordinates stop decoding instead of being treated as a giant hole. */
static void test_illegal_coordinates_stop(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[4];
    unsigned long long payloadStart[4];
    int blockSizes[4] = {16, 16, 16, 16};
    unsigned int size;
    drflac* d;
    drflac_int32 out[MAX_SAMPLES * MAX_CHANNELS];
    int k;

    printf("test_illegal_coordinates_stop\n");

    /* Frame 3 declares frame number 1, so it moves backward to sample 16 while output is already at 48. */
    for (k = 0; k < 4; ++k) {
        coords[k] = (unsigned long long)k;
        payloadStart[k] = (unsigned long long)(k * 16);
    }
    coords[3] = 1;
    size = flac_gen_build_ex(&bw, 0, 2, blockSizes, coords, payloadStart, 4, 64, spans);
    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 64, out) == 48, "backward frame stops decode after 48, no giant hole");
    CHECK(d->currentPCMFrame == 48, "position held at 48");
    drflac_close(d);

    /* A frame extending past the declared total length is likewise a stop, not a hole. */
    for (k = 0; k < 4; ++k) {
        coords[k] = (unsigned long long)k;
        payloadStart[k] = (unsigned long long)(k * 16);
    }
    coords[3] = 5;   /* frame 5 * 16 = 80 > total 64 */
    size = flac_gen_build_ex(&bw, 0, 2, blockSizes, coords, payloadStart, 4, 64, spans);
    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 64, out) == 48, "out-of-range frame stops decode after 48");
    drflac_close(d);
}

/* T11: output buffers are never overrun; guard bytes around exact-sized allocations stay intact. */
static void test_output_bounds(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 16};
    unsigned int size;
    drflac* d;
    unsigned char* guarded;
    const size_t guardBytes = 16;
    const size_t sampleBytes = 48u * 2u * sizeof(drflac_int32);
    size_t i;
    drflac_int32* out;

    printf("test_output_bounds\n");
    size = build_fixed(&bw, 2, blockSizes, 3, 48, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[1]);

    guarded = (unsigned char*)malloc(guardBytes * 2u + sampleBytes);
    CHECK(guarded != NULL, "guard allocation");
    guard_fill(guarded, guardBytes);
    guard_fill(guarded + guardBytes + sampleBytes, guardBytes);
    out = (drflac_int32*)(guarded + guardBytes);

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 48, out) == 48, "exact-size buffer filled");
    for (i = 0; i < guardBytes; ++i) {
        CHECK(guarded[i] == 0xA5, "leading guard intact");
        CHECK(guarded[guardBytes + sampleBytes + i] == 0xA5, "trailing guard intact");
    }

    /* Short read must write only the requested prefix even when the rest is a hole. */
    {
        drflac_int16 small[6];
        memset(small, 0, sizeof(small));
        CHECK(drflac_seek_to_pcm_frame(d, 14) == DRFLAC_TRUE, "seek before hole");
        CHECK(drflac_read_pcm_frames_s16(d, 3, small) == 3, "short s16 read");
        CHECK(small[2] == expected_s16(15, 0) && small[4] == 0, "short read crosses into hole correctly");
    }
    free(guarded);
    drflac_close(d);
}

/* T12: s16 and f32 silence and samples are exact, not smoothed or resampled. */
static void test_s16_f32_exactness(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 16};
    unsigned int size;
    drflac* d;
    drflac_int16 s16[MAX_SAMPLES * 2];
    float f32[MAX_SAMPLES * 2];
    int f;

    printf("test_s16_f32_exactness\n");
    size = build_fixed(&bw, 2, blockSizes, 3, 48, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[1]);

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s16(d, 48, s16) == 48, "s16 full read");
    verify_timeline_s16(s16, 2, 48, 16, 32, "s16 hole exactness");
    drflac_close(d);

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_f32(d, 48, f32) == 48, "f32 full read");
    for (f = 0; f < 48; ++f) {
        int c;
        for (c = 0; c < 2; ++c) {
            float want = (f >= 16 && f < 32) ? 0.0f : expected_f32((unsigned long long)f, c);
            if (f32[f * 2 + c] != want) {
                printf("  FAIL f32 frame %d ch %d got %g want %g\n", f, c, f32[f * 2 + c], want);
                g_failures += 1;
                break;
            }
        }
    }
    g_checks += 1;
    drflac_close(d);
}

/* The source's first frame must itself be a fully validated frame at sample 0; a bad first frame yields nothing, not silence. */
static void test_bad_first_frame(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 16};
    unsigned int size;
    drflac* d;
    drflac_int32 out[64];

    printf("test_bad_first_frame\n");
    size = build_fixed(&bw, 2, blockSizes, 3, 48, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[0]);

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(d != NULL, "open stream with bad first frame");
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 48, out) == 0, "no output before a validated frame at sample 0");
    CHECK(d->currentPCMFrame == 0, "position stays at 0");
    drflac_close(d);
}

/* NULL-buffer skip reads from a fresh decoder walk holes correctly and leave the next typed read on the right coordinate. */
static void test_null_skip_from_start(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 16};
    unsigned int size;
    drflac* d;
    drflac_int16 s16[4];
    drflac_uint64 skipped;

    printf("test_null_skip_from_start\n");
    size = build_fixed(&bw, 2, blockSizes, 3, 48, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[1]);   /* hole [16,32) */

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");

    /* Skip straight across the hole before any sample is produced: 32 frames must be consumed, no buffer written. */
    skipped = drflac_read_pcm_frames_s16(d, 32, NULL);
    CHECK(skipped == 32, "NULL skip covers good frame plus whole hole");
    CHECK(d->currentPCMFrame == 32, "NULL skip lands on right-frame coordinate 32");

    /* The next real samples are the right frame's audio, proving the skip did not swallow or duplicate the hole. */
    CHECK(drflac_read_pcm_frames_s16(d, 2, s16) == 2, "typed read after NULL skip");
    CHECK(s16[0] == expected_s16(32, 0) && s16[3] == expected_s16(33, 1), "first samples after NULL skip are frame 3 audio");
    CHECK(d->currentPCMFrame == 34, "position after typed read");

    /* A skip past the confirmed end returns only the frames that exist and stops at the declared content boundary. */
    skipped = drflac_read_pcm_frames_s32(d, 100, NULL);
    CHECK(skipped == 14, "over-long NULL skip clamps to remaining 14 frames");
    CHECK(d->currentPCMFrame == 48, "final position is the stream end");
    drflac_close(d);
}

/* Exact seek boundaries: hole edges, frame edges, the stream end, and repeated back-and-forth seeks. */
typedef struct
{
    int target;
    int expectZero;
} seek_boundary_case;

static void test_seek_boundaries(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 16};
    unsigned int size;
    drflac* d;
    drflac_int32 out[48 * 2];   /* full reread holds 48 stereo frames */
    seek_boundary_case cases[10];
    int i;

    printf("test_seek_boundaries\n");
    size = build_fixed(&bw, 2, blockSizes, 3, 48, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[1]);   /* hole [16,32) */

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");

    cases[0] = (seek_boundary_case){16, 1};   /* first hole sample */
    cases[1] = (seek_boundary_case){17, 1};
    cases[2] = (seek_boundary_case){31, 1};   /* last hole sample */
    cases[3] = (seek_boundary_case){32, 0};   /* first good sample after hole */
    cases[4] = (seek_boundary_case){15, 0};   /* last good sample before hole */
    cases[5] = (seek_boundary_case){0, 0};
    cases[6] = (seek_boundary_case){20, 1};
    cases[7] = (seek_boundary_case){10, 0};
    cases[8] = (seek_boundary_case){33, 0};
    cases[9] = (seek_boundary_case){47, 0};   /* very last sample */

    for (i = 0; i < 10; ++i) {
        drflac_uint64 n;
        int want;
        CHECK(drflac_seek_to_pcm_frame(d, (drflac_uint64)cases[i].target) == DRFLAC_TRUE, "edge seek succeeds");
        CHECK(d->currentPCMFrame == (drflac_uint64)cases[i].target, "edge seek coordinate");
        n = drflac_read_pcm_frames_s32(d, 1, out);
        CHECK(n == 1, "one frame read after edge seek");
        want = cases[i].expectZero ? 0 : expected_s32((unsigned long long)cases[i].target, 0);
        CHECK(out[0] == want, "edge first sample value");
    }

    /* Seeking to the declared end succeeds but yields no samples, and the decoder can still seek back. */
    CHECK(drflac_seek_to_pcm_frame(d, 48) == DRFLAC_TRUE, "seek to stream end");
    CHECK(d->currentPCMFrame == 48, "position at end");
    CHECK(drflac_read_pcm_frames_s32(d, 4, out) == 0, "no samples beyond the end");
    CHECK(drflac_seek_to_pcm_frame(d, 0) == DRFLAC_TRUE, "seek back to zero after end");
    CHECK(drflac_read_pcm_frames_s32(d, 48, out) == 48, "full reproducible reread");
    verify_timeline_s32(out, 2, 48, 16, 32, "boundary reread timeline");
    drflac_close(d);
}

/* 32-bit samples use the decoder's 64-bit decoded storage; the hole must still be exact zeros through every output format. */
static void test_32bit_storage_path(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 16};
    unsigned int size;
    drflac* d;
    drflac_int32 s32[48 * 2];
    drflac_int16 s16[48 * 2];
    float f32[48 * 2];
    int f;
    int c;

    printf("test_32bit_storage_path\n");
    for (f = 0; f < 3; ++f) {
        coords[f] = (unsigned long long)f;
    }
    size = flac_gen_build_bps(&bw, 0, 2, blockSizes, coords, 3, 48, 32, spans);
    flac_gen_corrupt_frame_crc16(&bw, &spans[1]);   /* hole [16,32) */

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(d != NULL, "32-bit open");
    CHECK(d->bitsPerSample == 32, "32 bits per sample reported");
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");

    CHECK(drflac_read_pcm_frames_s32(d, 48, s32) == 48, "32-bit s32 full read");
    for (f = 0; f < 48; ++f) {
        for (c = 0; c < 2; ++c) {
            drflac_int32 want = (f >= 16 && f < 32) ? 0 : (drflac_int32)flac_gen_sample32((unsigned long long)f, c);
            if (s32[f * 2 + c] != want) {
                printf("  FAIL 32bit s32 frame %d ch %d got %d want %d\n", f, c, s32[f * 2 + c], want);
                g_failures += 1;
            }
        }
    }
    g_checks += 1;
    drflac_close(d);

    /* The same timeline through the s16 and f32 s64 conversion paths. */
    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted (s16)");
    CHECK(drflac_read_pcm_frames_s16(d, 48, s16) == 48, "32-bit s16 full read");
    for (f = 0; f < 48; ++f) {
        for (c = 0; c < 2; ++c) {
            drflac_int16 want = (f >= 16 && f < 32) ? 0
                : (drflac_int16)((drflac_int32)flac_gen_sample32((unsigned long long)f, c) >> 16);
            if (s16[f * 2 + c] != want) {
                printf("  FAIL 32bit s16 frame %d ch %d\n", f, c);
                g_failures += 1;
            }
        }
    }
    g_checks += 1;
    drflac_close(d);

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted (f32)");
    CHECK(drflac_read_pcm_frames_f32(d, 48, f32) == 48, "32-bit f32 full read");
    for (f = 0; f < 48; ++f) {
        for (c = 0; c < 2; ++c) {
            float want = (f >= 16 && f < 32) ? 0.0f
                : (float)(drflac_int32)flac_gen_sample32((unsigned long long)f, c) * (1.0f / 2147483648.0f);
            if (f32[f * 2 + c] != want) {
                printf("  FAIL 32bit f32 frame %d ch %d got %g want %g\n", f, c, f32[f * 2 + c], want);
                g_failures += 1;
            }
        }
    }
    g_checks += 1;
    drflac_close(d);
}

/* Four independent channels: every channel of a hole must be zero, and every channel around it keeps its own waveform. */
static void test_four_channel_hole(void)
{
    flac_gen_bw bw;
    flac_gen_frame_span spans[8];
    unsigned long long coords[8];
    int blockSizes[3] = {16, 16, 16};
    unsigned int size;
    drflac* d;
    drflac_int32 out[48 * 4];
    int f;
    int c;

    printf("test_four_channel_hole\n");
    size = build_fixed(&bw, 4, blockSizes, 3, 48, spans, coords);
    flac_gen_corrupt_frame_crc16(&bw, &spans[1]);   /* hole [16,32) across all four channels */

    d = drflac_open_memory(bw.data, size, NULL);
    CHECK(d->channels == 4, "four channels reported");
    CHECK(drflac_set_preserve_sample_timeline(d, DRFLAC_TRUE) == DRFLAC_TRUE, "enable accepted");
    CHECK(drflac_read_pcm_frames_s32(d, 48, out) == 48, "four-channel stream reads 48 frames");

    for (f = 0; f < 48; ++f) {
        for (c = 0; c < 4; ++c) {
            int want = (f >= 16 && f < 32) ? 0 : expected_s32((unsigned long long)f, c);
            if (out[f * 4 + c] != want) {
                printf("  FAIL 4ch frame %d ch %d got %d want %d\n", f, c, out[f * 4 + c], want);
                g_failures += 1;
                break;
            }
        }
    }
    g_checks += 1;
    drflac_close(d);
}

int main(void)
{
    test_fixed_single_bad_frame();
    test_fixed_two_adjacent_bad_frames();
    test_fixed_deleted_frame();
    test_fixed_short_tail_block();
    test_variable_multichannel();
    test_four_channel_hole();
    test_32bit_storage_path();
    test_pseudo_sync_header_crc8_ok();
    test_tail_damage_no_right_frame();
    test_seeking_and_formats();
    test_seek_boundaries();
    test_setter_gating();
    test_headerless_rejected();
    test_illegal_coordinates_stop();
    test_output_bounds();
    test_s16_f32_exactness();
    test_bad_first_frame();
    test_null_skip_from_start();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
