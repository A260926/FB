/*
Contract tests for MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_SEEK_COALESCING.

Everything here is driven through the real public miniaudio API against real WAV files and the real
resource manager job queue. Job processing is externally scheduled: the no-threading resource
manager is pumped by hand, or by a real POSIX worker thread coordinated through pthread condition
variables and a barrier VFS. No sleeps and no self-reported statistics are used for synchronization;
the only counters consulted are VFS operation ordinals recorded by the barrier VFS.
*/
#include "miniaudio.h"
#include "task_support/check.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SAMPLE_RATE    8000
/* Resource manager pages hold 1000 ms of audio; at 8 kHz that is exactly SAMPLE_RATE frames. */
#define PAGE_FRAMES    SAMPLE_RATE
#define WAV_FRAMES     20000

/* Asymmetric two channel formula so a swapped or stale channel cannot pass. */
static int16_t formula_s16(ma_uint64 frame, unsigned channel)
{
    return (int16_t)(((frame * 37u + channel * 113u + 7u) % 60001u) - 30000);
}

static float formula_f32(ma_uint64 frame, unsigned channel)
{
    int32_t v = (int32_t)(((frame * 41u + channel * 97u + 13u) % 60001u) - 30000);
    return (float)v / 30000.0f;
}

static void le16(FILE* f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void le32(FILE* f, unsigned v) { unsigned i; for (i = 0; i < 4; ++i) { fputc(v & 255, f); v >>= 8; } }

static int write_wav_s16(const char* path, ma_uint32 frames, ma_uint32 channels)
{
    ma_uint32 i, c;
    FILE* f = fopen(path, "wb");
    TASK_CHECK(f != NULL);
    fwrite("RIFF", 1, 4, f); le32(f, 36u + frames * channels * 2); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); le32(f, 16); le16(f, 1); le16(f, channels);
    le32(f, SAMPLE_RATE); le32(f, SAMPLE_RATE * channels * 2); le16(f, channels * 2); le16(f, 16);
    fwrite("data", 1, 4, f); le32(f, frames * channels * 2);
    for (i = 0; i < frames; ++i) {
        for (c = 0; c < channels; ++c) {
            int16_t s = formula_s16(i, c);
            le16(f, (uint16_t)s);
        }
    }
    TASK_CHECK(fclose(f) == 0);
    return 0;
}

static int write_wav_f32(const char* path, ma_uint32 frames, ma_uint32 channels)
{
    ma_uint32 i, c;
    FILE* f = fopen(path, "wb");
    TASK_CHECK(f != NULL);
    fwrite("RIFF", 1, 4, f); le32(f, 36u + frames * channels * 4); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); le32(f, 16); le16(f, 3); le16(f, channels);
    le32(f, SAMPLE_RATE); le32(f, SAMPLE_RATE * channels * 4); le16(f, channels * 4); le16(f, 32);
    fwrite("data", 1, 4, f); le32(f, frames * channels * 4);
    for (i = 0; i < frames; ++i) {
        for (c = 0; c < channels; ++c) {
            float s = formula_f32(i, c);
            fwrite(&s, sizeof(float), 1, f);
        }
    }
    TASK_CHECK(fclose(f) == 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Barrier VFS: wraps stdio with a controllable gate and fault injector. */
/* ------------------------------------------------------------------ */

typedef struct
{
    ma_vfs_callbacks cb;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    ma_bool32 gateClosed;      /* Worker threads block inside the hooked operation while true. */
    ma_bool32 armed;           /* Only hook operations after the test arms the barrier. */
    ma_bool32 blockOnRead;     /* Block in onRead instead of onSeek. */
    int blockedCount;          /* Number of worker threads currently waiting at the gate. */
    ma_bool32 failSeek;        /* Inject MA_ERROR from onSeek while true. */
    int seekOrdinal;          /* Total decoder (VFS level) seek operations observed. */
    int readOrdinal;          /* Total VFS read operations observed. */
} barrier_vfs;

typedef struct
{
    FILE* fp;
    barrier_vfs* vfs;
} barrier_file;

static ma_result bvfs_open(ma_vfs* pVFS, const char* pFilePath, ma_uint32 openMode, ma_vfs_file* pFile)
{
    barrier_vfs* v = (barrier_vfs*)pVFS;
    barrier_file* bf;
    (void)openMode;
    bf = (barrier_file*)malloc(sizeof(*bf));
    if (bf == NULL) {
        return MA_OUT_OF_MEMORY;
    }
    bf->fp = fopen(pFilePath, "rb");
    if (bf->fp == NULL) {
        free(bf);
        return MA_DOES_NOT_EXIST;
    }
    bf->vfs = v;
    *pFile = (ma_vfs_file)bf;
    return MA_SUCCESS;
}

static ma_result bvfs_close(ma_vfs* pVFS, ma_vfs_file file)
{
    barrier_file* bf = (barrier_file*)file;
    (void)pVFS;
    if (bf == NULL) {
        return MA_INVALID_ARGS;
    }
    fclose(bf->fp);
    free(bf);
    return MA_SUCCESS;
}

static void bvfs_gate(barrier_vfs* v)
{
    if (!v->armed) {
        return;
    }
    pthread_mutex_lock(&v->lock);
    v->blockedCount += 1;
    pthread_cond_broadcast(&v->cond);
    while (v->gateClosed) {
        pthread_cond_wait(&v->cond, &v->lock);
    }
    v->blockedCount -= 1;
    pthread_mutex_unlock(&v->lock);
}

static ma_result bvfs_read(ma_vfs* pVFS, ma_vfs_file file, void* pDst, size_t sizeInBytes, size_t* pBytesRead)
{
    barrier_file* bf = (barrier_file*)file;
    size_t n;
    (void)pVFS;
    if (bf->vfs->blockOnRead) {
        bvfs_gate(bf->vfs);
    }
    bf->vfs->readOrdinal += 1;
    n = fread(pDst, 1, sizeInBytes, bf->fp);
    if (pBytesRead != NULL) {
        *pBytesRead = n;
    }
    /* Like the stdio VFS, a short read at EOF is still a successful call with fewer bytes. */
    return MA_SUCCESS;
}

static ma_result bvfs_seek(ma_vfs* pVFS, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin)
{
    barrier_file* bf = (barrier_file*)file;
    int whence = (origin == ma_seek_origin_start) ? SEEK_SET : (origin == ma_seek_origin_end ? SEEK_END : SEEK_CUR);
    (void)pVFS;

    bf->vfs->seekOrdinal += 1;
    if (bf->vfs->failSeek) {
        return MA_ERROR;
    }
    if (!bf->vfs->blockOnRead) {
        bvfs_gate(bf->vfs);
    }
    if (fseek(bf->fp, (long)offset, whence) != 0) {
        return MA_BAD_SEEK;
    }
    return MA_SUCCESS;
}

static ma_result bvfs_tell(ma_vfs* pVFS, ma_vfs_file file, ma_int64* pCursor)
{
    barrier_file* bf = (barrier_file*)file;
    long p;
    (void)pVFS;
    p = ftell(bf->fp);
    if (p < 0) {
        return MA_ERROR;
    }
    *pCursor = (ma_int64)p;
    return MA_SUCCESS;
}

static ma_result bvfs_info(ma_vfs* pVFS, ma_vfs_file file, ma_file_info* pInfo)
{
    barrier_file* bf = (barrier_file*)file;
    long p;
    (void)pVFS;
    p = ftell(bf->fp);
    if (fseek(bf->fp, 0, SEEK_END) != 0) {
        return MA_ERROR;
    }
    pInfo->sizeInBytes = (ma_uint64)ftell(bf->fp);
    fseek(bf->fp, p, SEEK_SET);
    return MA_SUCCESS;
}

static void bvfs_init(barrier_vfs* v)
{
    memset(v, 0, sizeof(*v));
    v->cb.onOpen  = bvfs_open;
    v->cb.onOpenW = NULL;
    v->cb.onClose = bvfs_close;
    v->cb.onRead  = bvfs_read;
    v->cb.onWrite = NULL;
    v->cb.onSeek  = bvfs_seek;
    v->cb.onTell  = bvfs_tell;
    v->cb.onInfo  = bvfs_info;
    pthread_mutex_init(&v->lock, NULL);
    pthread_cond_init(&v->cond, NULL);
}

static void bvfs_uninit(barrier_vfs* v)
{
    pthread_cond_destroy(&v->cond);
    pthread_mutex_destroy(&v->lock);
}

static void bvfs_wait_blocked(barrier_vfs* v)
{
    pthread_mutex_lock(&v->lock);
    while (v->blockedCount == 0) {
        pthread_cond_wait(&v->cond, &v->lock);
    }
    pthread_mutex_unlock(&v->lock);
}

static void bvfs_open_gate(barrier_vfs* v)
{
    pthread_mutex_lock(&v->lock);
    v->gateClosed = MA_FALSE;
    pthread_cond_broadcast(&v->cond);
    pthread_mutex_unlock(&v->lock);
}

/* ------------------------------------------------------------------ */
/* Resource manager / stream helpers.                                  */
/* ------------------------------------------------------------------ */

static ma_resource_manager_config rm_config(ma_uint32 queueCapacity, ma_vfs* vfs, ma_format format, ma_uint32 channels)
{
    ma_resource_manager_config cfg = ma_resource_manager_config_init();
    cfg.jobThreadCount = 0;
    cfg.flags = MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
    cfg.jobQueueCapacity = queueCapacity;
    cfg.pVFS = vfs;
    cfg.decodedFormat = format;
    cfg.decodedChannels = channels;
    cfg.decodedSampleRate = SAMPLE_RATE;
    return cfg;
}

static int pump_all(ma_resource_manager* rm)
{
    int guard = 0;
    for (;;) {
        ma_result r = ma_resource_manager_process_next_job(rm);
        if (r == MA_NO_DATA_AVAILABLE || r == MA_CANCELLED) {
            return 0;
        }
        /* A job may complete with an error that is delivered through the stream's own state; every call still drains one job. */
        TASK_CHECK(++guard < 1000000);
    }
}

static int wait_settled(ma_resource_manager* rm, ma_resource_manager_data_stream* s)
{
    ma_resource_manager_seek_state st;
    int guard = 0;
    do {
        TASK_CHECK(pump_all(rm) == 0);
        TASK_CHECK(ma_resource_manager_data_stream_get_seek_state(s, &st) == MA_SUCCESS);
        TASK_CHECK(++guard < 1000000);
    } while (st.result == MA_BUSY);
    return 0;
}

static int check_state(ma_resource_manager_data_stream* s, ma_uint64 requested, ma_uint64 settled, ma_uint64 frameIndex, ma_result result)
{
    ma_resource_manager_seek_state st;
    TASK_CHECK(ma_resource_manager_data_stream_get_seek_state(s, &st) == MA_SUCCESS);
    TASK_CHECK(st.requested == requested);
    TASK_CHECK(st.settled == settled);
    TASK_CHECK(st.frameIndex == frameIndex);
    TASK_CHECK(st.result == result);
    return 0;
}

static int open_stream(ma_resource_manager* rm, const char* path, ma_uint32 extraFlags, ma_resource_manager_data_stream* s)
{
    ma_result r = ma_resource_manager_data_stream_init(rm, path,
        MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM | MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_SEEK_COALESCING | extraFlags, NULL, s);
    TASK_CHECK(r == MA_SUCCESS);
    return 0;
}

/* Reads frames, pumping jobs through any BUSY windows. Returns the final result. */
static ma_result read_frames(ma_resource_manager* rm, ma_resource_manager_data_stream* s, void* out, ma_uint32 frameCount, ma_uint32 channels, ma_uint64* pFramesRead)
{
    ma_format fmt;
    ma_uint32 ch;
    ma_result r;
    ma_uint64 total = 0;
    int guard = 0;
    TASK_CHECK(ma_resource_manager_data_stream_get_data_format(s, &fmt, &ch, NULL, NULL, 0) == MA_SUCCESS);
    TASK_CHECK(ch == channels);
    while (total < frameCount) {
        ma_uint64 n = 0;
        r = ma_resource_manager_data_stream_read_pcm_frames(s, ma_offset_pcm_frames_ptr(out, (ma_uint32)total, fmt, channels), frameCount - (ma_uint32)total, &n);
        if (r == MA_BUSY) {
            TASK_CHECK(pump_all(rm) == 0);
            TASK_CHECK(++guard < 1000000);
            continue;
        }
        if (r != MA_SUCCESS) {
            if (pFramesRead != NULL) {
                *pFramesRead = total;
            }
            return r;
        }
        total += n;
        if (n == 0) {
            break;
        }
    }
    if (pFramesRead != NULL) {
        *pFramesRead = total;
    }
    return MA_SUCCESS;
}

static int check_s16_block(const int16_t* got, ma_uint64 target, ma_uint32 frameCount, ma_uint32 channels)
{
    ma_uint32 i, c;
    for (i = 0; i < frameCount; ++i) {
        for (c = 0; c < channels; ++c) {
            TASK_CHECK(got[(size_t)i * channels + c] == formula_s16(target + i, c));
        }
    }
    return 0;
}

static int check_f32_block(const float* got, ma_uint64 target, ma_uint32 frameCount, ma_uint32 channels)
{
    ma_uint32 i, c;
    for (i = 0; i < frameCount; ++i) {
        for (c = 0; c < channels; ++c) {
            TASK_CHECK(got[(size_t)i * channels + c] == formula_f32(target + i, c));
        }
    }
    return 0;
}

/* Real worker thread driven by a condition variable; the queue stays non-blocking. */
typedef struct
{
    ma_resource_manager* rm;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_t       thread;
    volatile int    wakeFlag;
    volatile int    stop;
    int             jobsProcessed;
} worker;

static void* worker_entry(void* arg)
{
    worker* w = (worker*)arg;
    for (;;) {
        ma_job job;
        ma_result r;
        pthread_mutex_lock(&w->lock);
        while (!w->stop && !w->wakeFlag) {
            pthread_cond_wait(&w->cond, &w->lock);
        }
        w->wakeFlag = 0;
        pthread_mutex_unlock(&w->lock);
        if (w->stop) {
            return NULL;
        }
        r = ma_resource_manager_next_job(w->rm, &job);
        if (r == MA_SUCCESS) {
            ma_job_process(&job);
            w->jobsProcessed += 1;
            pthread_mutex_lock(&w->lock);
            w->wakeFlag = 1;   /* More jobs may be queued behind it. */
            pthread_cond_signal(&w->cond);
            pthread_mutex_unlock(&w->lock);
        }
    }
}

static int worker_start(worker* w, ma_resource_manager* rm)
{
    memset(w, 0, sizeof(*w));
    w->rm = rm;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    TASK_CHECK(pthread_create(&w->thread, NULL, worker_entry, w) == 0);
    return 0;
}

static void worker_kick(worker* w)
{
    pthread_mutex_lock(&w->lock);
    w->wakeFlag = 1;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
}

static int worker_join(worker* w)
{
    pthread_mutex_lock(&w->lock);
    w->stop = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
    TASK_CHECK(pthread_join(w->thread, NULL) == 0);
    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->lock);
    return 0;
}

/* ================================================================== */
/* mailbox group                                                       */
/* ================================================================== */

static int case_initial_and_basic_dispatch(void)
{
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_seek_state st;
    ma_resource_manager_config cfg = rm_config(64, NULL, ma_format_s16, 2);
    int16_t out[128];
    ma_uint64 n = 0;
    ma_uint64 cursor = 0;

    TASK_CHECK(write_wav_s16(".task-build/mb_basic.wav", WAV_FRAMES, 2) == 0);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/mb_basic.wav", 0, &s) == 0);

    TASK_CHECK(check_state(&s, 0, 0, 0, MA_SUCCESS) == 0);
    TASK_CHECK(ma_resource_manager_data_stream_get_seek_state(NULL, &st) == MA_INVALID_ARGS);
    TASK_CHECK(ma_resource_manager_data_stream_get_seek_state(&s, NULL) == MA_INVALID_ARGS);

    /* Initial pages decode straight from the load job. */
    TASK_CHECK(read_frames(&rm, &s, out, 32, 2, &n) == MA_SUCCESS);
    TASK_CHECK(n == 32);
    TASK_CHECK(check_s16_block(out, 0, 32, 2) == 0);

    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 1234) == MA_SUCCESS);
    TASK_CHECK(check_state(&s, 1, 0, 1234, MA_BUSY) == 0);
    TASK_CHECK(ma_resource_manager_data_stream_get_cursor_in_pcm_frames(&s, &cursor) == MA_SUCCESS);
    TASK_CHECK(cursor == 1234);

    /* BUSY reads return zero frames and never touch the output. */
    memset(out, 0x55, sizeof(out));
    {
        ma_uint64 busyRead = 99;
        TASK_CHECK(ma_resource_manager_data_stream_read_pcm_frames(&s, out, 16, &busyRead) == MA_BUSY);
        TASK_CHECK(busyRead == 0);
        TASK_CHECK(out[0] == (int16_t)0x5555);
    }

    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(check_state(&s, 1, 1, 1234, MA_SUCCESS) == 0);
    TASK_CHECK(ma_resource_manager_data_stream_get_cursor_in_pcm_frames(&s, &cursor) == MA_SUCCESS);
    TASK_CHECK(cursor == 1234);
    TASK_CHECK(read_frames(&rm, &s, out, 48, 2, &n) == MA_SUCCESS);
    TASK_CHECK(n == 48);
    TASK_CHECK(check_s16_block(out, 1234, 48, 2) == 0);

    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    remove(".task-build/mb_basic.wav");
    return 0;
}

static int case_burst_presubmit_one_decode(void)
{
    /* Overrides all submitted before the worker runs: one queued job, one decode. */
    barrier_vfs vfs;
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_config cfg;
    int16_t out[64];
    ma_uint64 n = 0;
    const ma_uint64 targets[] = { 100, 5000, 7777, 333 };
    unsigned i;

    bvfs_init(&vfs);
    TASK_CHECK(write_wav_s16(".task-build/mb_burst.wav", WAV_FRAMES, 2) == 0);
    cfg = rm_config(64, (ma_vfs*)&vfs, ma_format_s16, 2);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/mb_burst.wav", 0, &s) == 0);
    TASK_CHECK(pump_all(&rm) == 0);
    vfs.seekOrdinal = 0;

    for (i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, targets[i]) == MA_SUCCESS);
    }
    TASK_CHECK(check_state(&s, 4, 0, targets[3], MA_BUSY) == 0);

    /* Same-target requests still count as new generations. */
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, targets[3]) == MA_SUCCESS);
    TASK_CHECK(check_state(&s, 5, 0, targets[3], MA_BUSY) == 0);

    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(check_state(&s, 5, 5, targets[3], MA_SUCCESS) == 0);
    TASK_CHECK(vfs.seekOrdinal == 2);   /* Exactly one decoder seek (2 VFS seeks) for the whole burst. */

    TASK_CHECK(read_frames(&rm, &s, out, 32, 2, &n) == MA_SUCCESS);
    TASK_CHECK(check_s16_block(out, targets[3], 32, 2) == 0);

    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    bvfs_uninit(&vfs);
    remove(".task-build/mb_burst.wav");
    return 0;
}

static int case_mid_decode_override_with_worker(void)
{
    /* Real worker thread frozen inside the first seek decode; override lands, old target still completes but is never published. */
    barrier_vfs vfs;
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_config cfg;
    worker w;
    int16_t out[64];
    ma_uint64 n = 0;

    bvfs_init(&vfs);
    TASK_CHECK(write_wav_s16(".task-build/mb_mid.wav", WAV_FRAMES, 2) == 0);
    cfg = rm_config(64, (ma_vfs*)&vfs, ma_format_s16, 2);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/mb_mid.wav", 0, &s) == 0);
    TASK_CHECK(pump_all(&rm) == 0);
    vfs.seekOrdinal = 0;

    vfs.armed = MA_TRUE;
    vfs.gateClosed = MA_TRUE;
    TASK_CHECK(worker_start(&w, &rm) == 0);

    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 2000) == MA_SUCCESS);
    worker_kick(&w);
    bvfs_wait_blocked(&vfs);                 /* Worker is frozen inside the decoder seek of generation 1. */

    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 9000) == MA_SUCCESS);
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 4000) == MA_SUCCESS);
    TASK_CHECK(check_state(&s, 3, 0, 4000, MA_BUSY) == 0);

    bvfs_open_gate(&vfs);
    TASK_CHECK(worker_join(&w) == 0);
    TASK_CHECK(pump_all(&rm) == 0);

    TASK_CHECK(check_state(&s, 3, 3, 4000, MA_SUCCESS) == 0);
    TASK_CHECK(vfs.seekOrdinal == 4);        /* Two decoder seeks (2 VFS seeks each): gen 1 hand-off plus the published gen 3. */
    TASK_CHECK(read_frames(&rm, &s, out, 32, 2, &n) == MA_SUCCESS);
    TASK_CHECK(check_s16_block(out, 4000, 32, 2) == 0);

    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    bvfs_uninit(&vfs);
    remove(".task-build/mb_mid.wav");
    return 0;
}

static int case_mid_read_override_with_worker(void)
{
    /* Same hand-off with the gate inside page reads. */
    barrier_vfs vfs;
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_config cfg;
    worker w;
    int16_t out[64];
    ma_uint64 n = 0;

    bvfs_init(&vfs);
    TASK_CHECK(write_wav_s16(".task-build/mb_read.wav", WAV_FRAMES, 2) == 0);
    cfg = rm_config(64, (ma_vfs*)&vfs, ma_format_s16, 2);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/mb_read.wav", 0, &s) == 0);
    TASK_CHECK(pump_all(&rm) == 0);

    vfs.armed = MA_TRUE;
    vfs.gateClosed = MA_TRUE;
    vfs.blockOnRead = MA_TRUE;
    TASK_CHECK(worker_start(&w, &rm) == 0);

    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 1500) == MA_SUCCESS);
    worker_kick(&w);
    bvfs_wait_blocked(&vfs);
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 6500) == MA_SUCCESS);
    bvfs_open_gate(&vfs);
    TASK_CHECK(worker_join(&w) == 0);
    TASK_CHECK(pump_all(&rm) == 0);

    TASK_CHECK(check_state(&s, 2, 2, 6500, MA_SUCCESS) == 0);
    TASK_CHECK(read_frames(&rm, &s, out, 24, 2, &n) == MA_SUCCESS);
    TASK_CHECK(check_s16_block(out, 6500, 24, 2) == 0);

    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    bvfs_uninit(&vfs);
    remove(".task-build/mb_read.wav");
    return 0;
}

static int case_concurrent_hammer_no_lost_wake(void)
{
    /* Control thread and a real worker thread overlap for hundreds of generations; a lost wake-up would strand MA_BUSY. */
    barrier_vfs vfs;
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_config cfg;
    worker w;
    int16_t out[64];
    ma_uint64 n = 0;
    unsigned i;

    bvfs_init(&vfs);
    TASK_CHECK(write_wav_s16(".task-build/mb_hammer.wav", WAV_FRAMES, 2) == 0);
    cfg = rm_config(256, (ma_vfs*)&vfs, ma_format_s16, 2);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/mb_hammer.wav", 0, &s) == 0);
    TASK_CHECK(pump_all(&rm) == 0);

    TASK_CHECK(worker_start(&w, &rm) == 0);
    for (i = 0; i < 400; ++i) {
        ma_uint64 target = 1000 + ((ma_uint64)i * 37) % (WAV_FRAMES - 2000);
        TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, target) == MA_SUCCESS);
        worker_kick(&w);
    }
    TASK_CHECK(worker_join(&w) == 0);
    TASK_CHECK(pump_all(&rm) == 0);

    {
        ma_resource_manager_seek_state st;
        TASK_CHECK(ma_resource_manager_data_stream_get_seek_state(&s, &st) == MA_SUCCESS);
        TASK_CHECK(st.requested == 400);
        TASK_CHECK(st.settled == 400);
        TASK_CHECK(st.result == MA_SUCCESS);
        TASK_CHECK(read_frames(&rm, &s, out, 32, 2, &n) == MA_SUCCESS);
        TASK_CHECK(check_s16_block(out, st.frameIndex, 32, 2) == 0);
        TASK_CHECK(vfs.seekOrdinal <= 400);
    }

    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    bvfs_uninit(&vfs);
    remove(".task-build/mb_hammer.wav");
    return 0;
}

static int case_f32_stream(void)
{
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_config cfg = rm_config(64, NULL, ma_format_f32, 2);
    float out[64];
    ma_uint64 n = 0;

    TASK_CHECK(write_wav_f32(".task-build/mb_f32.wav", WAV_FRAMES, 2) == 0);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/mb_f32.wav", 0, &s) == 0);
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 5050) == MA_SUCCESS);
    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(check_state(&s, 1, 1, 5050, MA_SUCCESS) == 0);
    TASK_CHECK(read_frames(&rm, &s, out, 24, 2, &n) == MA_SUCCESS);
    TASK_CHECK(check_f32_block(out, 5050, 24, 2) == 0);

    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    remove(".task-build/mb_f32.wav");
    return 0;
}

static int case_mono_stream(void)
{
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_config cfg = rm_config(64, NULL, ma_format_s16, 1);
    int16_t out[64];
    ma_uint64 n = 0;

    TASK_CHECK(write_wav_s16(".task-build/mb_mono.wav", WAV_FRAMES, 1) == 0);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/mb_mono.wav", 0, &s) == 0);
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 202) == MA_SUCCESS);
    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(read_frames(&rm, &s, out, 32, 1, &n) == MA_SUCCESS);
    TASK_CHECK(check_s16_block(out, 202, 32, 1) == 0);

    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    remove(".task-build/mb_mono.wav");
    return 0;
}

/* ================================================================== */
/* publication group                                                   */
/* ================================================================== */

static int case_bounded_jobs_and_pages(void)
{
    /* A large burst with no pumping uses one seek slot and settles with one decode. */
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    barrier_vfs vfs;
    ma_resource_manager_config cfg;
    int16_t out[64];
    ma_uint64 n = 0;
    ma_uint64 lastTarget = 0;
    unsigned i;

    bvfs_init(&vfs);
    TASK_CHECK(write_wav_s16(".task-build/pub_bound.wav", WAV_FRAMES, 2) == 0);
    cfg = rm_config(8, (ma_vfs*)&vfs, ma_format_s16, 2);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/pub_bound.wav", 0, &s) == 0);
    TASK_CHECK(pump_all(&rm) == 0);
    vfs.seekOrdinal = 0;

    for (i = 0; i < 200; ++i) {
        ma_uint64 target = 500 + ((ma_uint64)i * 53) % (WAV_FRAMES - 1000);
        TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, target) == MA_SUCCESS);
        lastTarget = target;
    }
    TASK_CHECK(check_state(&s, 200, 0, lastTarget, MA_BUSY) == 0);

    TASK_CHECK(pump_all(&rm) == 0);
    {
        ma_resource_manager_seek_state st;
        TASK_CHECK(ma_resource_manager_data_stream_get_seek_state(&s, &st) == MA_SUCCESS);
        TASK_CHECK(st.requested == 200 && st.settled == 200 && st.result == MA_SUCCESS);
        TASK_CHECK(vfs.seekOrdinal == 2);
        TASK_CHECK(read_frames(&rm, &s, out, 32, 2, &n) == MA_SUCCESS);
        TASK_CHECK(check_s16_block(out, st.frameIndex, 32, 2) == 0);
    }

    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    bvfs_uninit(&vfs);
    remove(".task-build/pub_bound.wav");
    return 0;
}

static int case_queued_page_job_ordering(void)
{
    /* An old page job already in flight must run before the seek job, which still lands on target. */
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_config cfg = rm_config(64, NULL, ma_format_s16, 2);
    int16_t* bigOut;
    int16_t out[64];
    ma_uint64 n = 0;
    ma_result r;

    TASK_CHECK(write_wav_s16(".task-build/pub_order.wav", WAV_FRAMES, 2) == 0);
    bigOut = (int16_t*)malloc((size_t)(PAGE_FRAMES + 64) * 2 * sizeof(int16_t));
    TASK_CHECK(bigOut != NULL);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/pub_order.wav", 0, &s) == 0);

    /* Read across a page boundary; this leaves a PAGE_DATA_STREAM job queued and then returns BUSY. */
    {
        ma_uint32 got = 0;
        while (got < PAGE_FRAMES + 10) {
            ma_uint64 rn = 0;
            r = ma_resource_manager_data_stream_read_pcm_frames(&s, ma_offset_pcm_frames_ptr(bigOut, got, ma_format_s16, 2), 64, &rn);
            if (r == MA_BUSY) {
                break;
            }
            TASK_CHECK(r == MA_SUCCESS);
            got += (ma_uint32)rn;
        }
        TASK_CHECK(got >= PAGE_FRAMES);
    }

    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, PAGE_FRAMES + 123) == MA_SUCCESS);
    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(check_state(&s, 1, 1, PAGE_FRAMES + 123, MA_SUCCESS) == 0);
    TASK_CHECK(read_frames(&rm, &s, out, 32, 2, &n) == MA_SUCCESS);
    TASK_CHECK(check_s16_block(out, PAGE_FRAMES + 123, 32, 2) == 0);

    free(bigOut);
    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    remove(".task-build/pub_order.wav");
    return 0;
}

static int case_queue_full_rejects_then_recovers(void)
{
    enum { CAP = 8 };
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_data_stream fillers[CAP - 1];
    ma_resource_manager_config cfg = rm_config(CAP, NULL, ma_format_s16, 2);
    ma_resource_manager_seek_state before;
    ma_uint64 cursorBefore;
    int16_t out[64];
    ma_uint64 n = 0;
    unsigned i;

    TASK_CHECK(write_wav_s16(".task-build/pub_full.wav", WAV_FRAMES, 2) == 0);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/pub_full.wav", 0, &s) == 0);
    TASK_CHECK(pump_all(&rm) == 0);

    TASK_CHECK(ma_resource_manager_data_stream_get_seek_state(&s, &before) == MA_SUCCESS);
    TASK_CHECK(ma_resource_manager_data_stream_get_cursor_in_pcm_frames(&s, &cursorBefore) == MA_SUCCESS);

    /* The queue keeps one internal dummy slot, so CAP-1 queued jobs fill every usable slot. */
    for (i = 0; i < CAP - 1; ++i) {
        ma_result r = ma_resource_manager_data_stream_init(&rm, ".task-build/pub_full.wav",
            MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM | MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_ASYNC, NULL, &fillers[i]);
        TASK_CHECK(r == MA_SUCCESS);
    }

    /* First wake-up has no slot: rejected with no observable change. */
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 7000) == MA_OUT_OF_MEMORY);
    TASK_CHECK(check_state(&s, before.requested, before.settled, before.frameIndex, before.result) == 0);
    {
        ma_uint64 cursorAfter;
        TASK_CHECK(ma_resource_manager_data_stream_get_cursor_in_pcm_frames(&s, &cursorAfter) == MA_SUCCESS);
        TASK_CHECK(cursorAfter == cursorBefore);
    }

    TASK_CHECK(pump_all(&rm) == 0);
    for (i = 0; i < CAP - 1; ++i) {
        ma_resource_manager_data_stream_uninit(&fillers[i]);
    }

    /* Recovery: a legal request is accepted and settles normally. */
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 7000) == MA_SUCCESS);
    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(check_state(&s, 1, 1, 7000, MA_SUCCESS) == 0);
    TASK_CHECK(read_frames(&rm, &s, out, 32, 2, &n) == MA_SUCCESS);
    TASK_CHECK(check_s16_block(out, 7000, 32, 2) == 0);

    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    remove(".task-build/pub_full.wav");
    return 0;
}

static int case_override_needs_no_slot_when_full(void)
{
    enum { CAP = 8 };
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_data_stream fillers[CAP - 2];
    ma_resource_manager_config cfg = rm_config(CAP, NULL, ma_format_s16, 2);
    int16_t out[64];
    ma_uint64 n = 0;
    unsigned i;

    TASK_CHECK(write_wav_s16(".task-build/pub_slot.wav", WAV_FRAMES, 2) == 0);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/pub_slot.wav", 0, &s) == 0);
    TASK_CHECK(pump_all(&rm) == 0);

    /* The first coalesced seek occupies exactly one usable slot; fill the remaining CAP-2. */
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 1111) == MA_SUCCESS);

    /* Fill every remaining usable slot. */
    for (i = 0; i < CAP - 2; ++i) {
        ma_result r = ma_resource_manager_data_stream_init(&rm, ".task-build/pub_slot.wav",
            MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM | MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_ASYNC, NULL, &fillers[i]);
        TASK_CHECK(r == MA_SUCCESS);
    }

    /* Overrides while the seek work unit is still queued require no new slot. */
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 8888) == MA_SUCCESS);
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 3333) == MA_SUCCESS);
    TASK_CHECK(check_state(&s, 3, 0, 3333, MA_BUSY) == 0);

    TASK_CHECK(pump_all(&rm) == 0);
    for (i = 0; i < CAP - 2; ++i) {
        ma_resource_manager_data_stream_uninit(&fillers[i]);
    }
    TASK_CHECK(check_state(&s, 3, 3, 3333, MA_SUCCESS) == 0);
    TASK_CHECK(read_frames(&rm, &s, out, 32, 2, &n) == MA_SUCCESS);
    TASK_CHECK(check_s16_block(out, 3333, 32, 2) == 0);

    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    remove(".task-build/pub_slot.wav");
    return 0;
}

static int case_eof_and_backward_seek(void)
{
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_config cfg = rm_config(64, NULL, ma_format_s16, 2);
    int16_t out[64];
    ma_uint64 n = 0;
    ma_result r;

    TASK_CHECK(write_wav_s16(".task-build/pub_eof.wav", WAV_FRAMES, 2) == 0);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/pub_eof.wav", 0, &s) == 0);

    /* Seeking to the exact end is valid; the first read is MA_AT_END with zero frames. */
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, WAV_FRAMES) == MA_SUCCESS);
    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(check_state(&s, 1, 1, WAV_FRAMES, MA_SUCCESS) == 0);
    r = read_frames(&rm, &s, out, 16, 2, &n);
    TASK_CHECK(r == MA_AT_END);
    TASK_CHECK(n == 0);

    /* Backward seek recovers and crosses the two-page boundary cleanly. */
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, PAGE_FRAMES - 5) == MA_SUCCESS);
    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(check_state(&s, 2, 2, PAGE_FRAMES - 5, MA_SUCCESS) == 0);
    TASK_CHECK(read_frames(&rm, &s, out, 20, 2, &n) == MA_SUCCESS);
    TASK_CHECK(n == 20);
    TASK_CHECK(check_s16_block(out, PAGE_FRAMES - 5, 20, 2) == 0);

    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    remove(".task-build/pub_eof.wav");
    return 0;
}

static int case_invalid_args_and_legacy_stream(void)
{
    ma_resource_manager rm;
    ma_resource_manager_data_stream coal;
    ma_resource_manager_data_stream legacy;
    ma_resource_manager_config cfg = rm_config(64, NULL, ma_format_s16, 2);

    TASK_CHECK(write_wav_s16(".task-build/pub_inv.wav", WAV_FRAMES, 2) == 0);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/pub_inv.wav", 0, &coal) == 0);

    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&coal, WAV_FRAMES + 1) == MA_INVALID_ARGS);
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(NULL, 10) == MA_INVALID_ARGS);
    TASK_CHECK(check_state(&coal, 0, 0, 0, MA_SUCCESS) == 0);

    /* A stream without the flag keeps the legacy behavior and has no seek state query. */
    TASK_CHECK(ma_resource_manager_data_stream_init(&rm, ".task-build/pub_inv.wav",
        MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM, NULL, &legacy) == MA_SUCCESS);
    {
        ma_resource_manager_seek_state legacyState;
        TASK_CHECK(ma_resource_manager_data_stream_get_seek_state(&legacy, &legacyState) == MA_INVALID_OPERATION);
    }
    TASK_CHECK(ma_resource_manager_data_stream_get_seek_state(NULL, NULL) == MA_INVALID_ARGS);
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&legacy, 4321) == MA_SUCCESS);
    TASK_CHECK(pump_all(&rm) == 0);
    {
        int16_t out[32];
        ma_uint64 n = 0;
        TASK_CHECK(read_frames(&rm, &legacy, out, 16, 2, &n) == MA_SUCCESS);
        TASK_CHECK(check_s16_block(out, 4321, 16, 2) == 0);
    }

    ma_resource_manager_data_stream_uninit(&legacy);
    ma_resource_manager_data_stream_uninit(&coal);
    ma_resource_manager_uninit(&rm);
    remove(".task-build/pub_inv.wav");
    return 0;
}

/* ================================================================== */
/* recovery group                                                      */
/* ================================================================== */

static int case_seek_failure_publishes_error_and_recovers(void)
{
    barrier_vfs vfs;
    ma_resource_manager rm;
    ma_resource_manager_data_stream s;
    ma_resource_manager_data_stream other;
    ma_resource_manager_config cfg;
    int16_t out[64];
    ma_uint64 n = 0;
    int16_t sentinel[4];

    bvfs_init(&vfs);
    TASK_CHECK(write_wav_s16(".task-build/rec_fail.wav", WAV_FRAMES, 2) == 0);
    cfg = rm_config(64, (ma_vfs*)&vfs, ma_format_s16, 2);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/rec_fail.wav", 0, &s) == 0);
    TASK_CHECK(open_stream(&rm, ".task-build/rec_fail.wav", 0, &other) == 0);
    TASK_CHECK(pump_all(&rm) == 0);

    memset(sentinel, 0x33, sizeof(sentinel));
    memcpy(out, sentinel, sizeof(sentinel));

    vfs.failSeek = MA_TRUE;
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 9000) == MA_SUCCESS);
    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(check_state(&s, 1, 1, 9000, MA_ERROR) == 0);

    /* The real error surfaces on read; output is untouched and no frames are reported. */
    {
        ma_uint64 rn = 77;
        TASK_CHECK(ma_resource_manager_data_stream_read_pcm_frames(&s, out, 2, &rn) == MA_ERROR);
        TASK_CHECK(rn == 0);
        TASK_CHECK(memcmp(out, sentinel, sizeof(sentinel)) == 0);
    }

    /* The fault was one generation's; clear it so the sibling can prove its state is independent. */
    vfs.failSeek = MA_FALSE;

    /* The sibling stream never saw the fault and is unaffected by the other stream's error. */
    TASK_CHECK(check_state(&other, 0, 0, 0, MA_SUCCESS) == 0);
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&other, 2500) == MA_SUCCESS);
    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(check_state(&other, 1, 1, 2500, MA_SUCCESS) == 0);
    TASK_CHECK(read_frames(&rm, &other, out, 16, 2, &n) == MA_SUCCESS);
    TASK_CHECK(check_s16_block(out, 2500, 16, 2) == 0);

    /* A later legal request recovers the failed stream. */
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&s, 4500) == MA_SUCCESS);
    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(check_state(&s, 2, 2, 4500, MA_SUCCESS) == 0);
    TASK_CHECK(read_frames(&rm, &s, out, 16, 2, &n) == MA_SUCCESS);
    TASK_CHECK(check_s16_block(out, 4500, 16, 2) == 0);

    ma_resource_manager_data_stream_uninit(&other);
    ma_resource_manager_data_stream_uninit(&s);
    ma_resource_manager_uninit(&rm);
    bvfs_uninit(&vfs);
    remove(".task-build/rec_fail.wav");
    return 0;
}

static int case_two_streams_independent(void)
{
    ma_resource_manager rm;
    ma_resource_manager_data_stream a, b;
    ma_resource_manager_config cfg = rm_config(64, NULL, ma_format_s16, 2);
    int16_t out[64];
    ma_uint64 n = 0;

    TASK_CHECK(write_wav_s16(".task-build/rec_two.wav", WAV_FRAMES, 2) == 0);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);
    TASK_CHECK(open_stream(&rm, ".task-build/rec_two.wav", 0, &a) == 0);
    TASK_CHECK(open_stream(&rm, ".task-build/rec_two.wav", 0, &b) == 0);

    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&a, 1000) == MA_SUCCESS);
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&a, 2000) == MA_SUCCESS);
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&b, 9000) == MA_SUCCESS);
    TASK_CHECK(check_state(&a, 2, 0, 2000, MA_BUSY) == 0);
    TASK_CHECK(check_state(&b, 1, 0, 9000, MA_BUSY) == 0);

    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(check_state(&a, 2, 2, 2000, MA_SUCCESS) == 0);
    TASK_CHECK(check_state(&b, 1, 1, 9000, MA_SUCCESS) == 0);

    TASK_CHECK(read_frames(&rm, &a, out, 16, 2, &n) == MA_SUCCESS);
    TASK_CHECK(check_s16_block(out, 2000, 16, 2) == 0);
    TASK_CHECK(read_frames(&rm, &b, out, 16, 2, &n) == MA_SUCCESS);
    TASK_CHECK(check_s16_block(out, 9000, 16, 2) == 0);

    ma_resource_manager_data_stream_uninit(&b);
    ma_resource_manager_data_stream_uninit(&a);
    ma_resource_manager_uninit(&rm);
    remove(".task-build/rec_two.wav");
    return 0;
}

/* ---- device-less engine rendering through a real ma_sound ---- */

typedef struct
{
    ma_engine engine;
    ma_resource_manager* rm;
} engine_bundle;

static int engine_setup(engine_bundle* e, ma_resource_manager* rm)
{
    ma_engine_config ec = ma_engine_config_init();
    ec.noDevice = MA_TRUE;
    ec.channels = 2;
    ec.sampleRate = SAMPLE_RATE;
    ec.periodSizeInFrames = 256;
    ec.pResourceManager = rm;
    e->rm = rm;
    TASK_CHECK(ma_engine_init(&ec, &e->engine) == MA_SUCCESS);
    return 0;
}

static int render(engine_bundle* e, float* dst, ma_uint32 frames, ma_uint64* pRead)
{
    ma_uint64 got = 0;
    TASK_CHECK(ma_engine_read_pcm_frames(&e->engine, dst, frames, &got) == MA_SUCCESS);
    TASK_CHECK(got == frames);
    if (pRead != NULL) {
        *pRead = got;
    }
    return 0;
}

static int is_silent(const float* x, ma_uint32 count)
{
    ma_uint32 i;
    for (i = 0; i < count; ++i) {
        if (x[i] != 0.0f) {
            return 0;
        }
    }
    return 1;
}

/* A freshly initialized engine+sound rendering one target with pristine cache and resampler history. */
typedef struct
{
    ma_engine engine;
    ma_sound  sound;
    ma_resource_manager_data_stream* pStream;
} fresh_rig;

static int fresh_rig_setup(fresh_rig* rig, ma_resource_manager* rm, ma_data_source* ds, float pitch)
{
    ma_engine_config ec = ma_engine_config_init();
    ec.noDevice = MA_TRUE;
    ec.channels = 2;
    ec.sampleRate = SAMPLE_RATE;
    ec.periodSizeInFrames = 256;
    ec.pResourceManager = rm;
    TASK_CHECK(ma_engine_init(&ec, &rig->engine) == MA_SUCCESS);
    TASK_CHECK(ma_sound_init_from_data_source(&rig->engine, ds, MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &rig->sound) == MA_SUCCESS);
    ma_sound_set_pitch(&rig->sound, pitch);
    TASK_CHECK(ma_sound_start(&rig->sound) == MA_SUCCESS);
    rig->pStream = (ma_resource_manager_data_stream*)ds;
    return 0;
}

static int fresh_rig_render_at(fresh_rig* rig, ma_resource_manager* rm, ma_uint64 target, float* dst, ma_uint32 frames)
{
    float scratch[256 * 2];
    ma_uint64 got = 0;
    TASK_CHECK(frames <= 256);
    TASK_CHECK(ma_sound_seek_to_pcm_frame(&rig->sound, target) == MA_SUCCESS);

    /* The first render dispatches the seek into the stream and is silent while the generation is BUSY. */
    TASK_CHECK(ma_engine_read_pcm_frames(&rig->engine, scratch, frames, &got) == MA_SUCCESS);
    TASK_CHECK(got == frames);
    TASK_CHECK(is_silent(scratch, frames * 2));

    /* Once settled, the next render is the fresh-sound output at the target. */
    TASK_CHECK(wait_settled(rm, rig->pStream) == 0);
    TASK_CHECK(ma_engine_read_pcm_frames(&rig->engine, dst, frames, &got) == MA_SUCCESS);
    TASK_CHECK(got == frames);
    return 0;
}

static int case_sound_recovery_pitch(float pitch)
{
    enum { RENDER_FRAMES = 256, WARM_FRAMES = 512, BUFFER_FRAMES = 2048 };
    ma_resource_manager rmRef, rmMain, rmOther;
    ma_resource_manager_data_stream refStream1, refStream2, mainStream;
    ma_resource_manager_config cfgRef  = rm_config(64, NULL, ma_format_s16, 2);
    ma_resource_manager_config cfgMain = rm_config(64, NULL, ma_format_s16, 2);
    ma_resource_manager_config cfgOther = rm_config(64, NULL, ma_format_s16, 2);
    fresh_rig ref1, ref2;
    engine_bundle mainEng, otherEng;
    ma_sound mainSound, otherSound, otherSoundRef;
    ma_audio_buffer buffer, bufferRef;
    ma_audio_buffer_config bcfg;
    const ma_uint64 target1 = PAGE_FRAMES + 300;
    const ma_uint64 target2 = 1234;
    float warm[WARM_FRAMES * 2];
    float mainBusy[RENDER_FRAMES * 2];
    float otherBusy[RENDER_FRAMES * 2];
    float otherOnly1[RENDER_FRAMES * 2];
    float refOut1[RENDER_FRAMES * 2];
    float mainAfter1[RENDER_FRAMES * 2];
    float otherOnly2[RENDER_FRAMES * 2];
    float refOut2[RENDER_FRAMES * 2];
    float mainAfter2[RENDER_FRAMES * 2];
    float bufferPcm[BUFFER_FRAMES * 2];
    ma_uint64 cursorBefore, cursorAfter, got = 0;
    ma_uint32 i;

    TASK_CHECK(write_wav_s16(".task-build/rec_sound.wav", WAV_FRAMES, 2) == 0);
    for (i = 0; i < BUFFER_FRAMES * 2; ++i) {
        bufferPcm[i] = 0.125f * sinf((float)(i / 2) * 0.07f + (float)(i & 1) * 0.5f);
    }

    TASK_CHECK(ma_resource_manager_init(&cfgRef, &rmRef) == MA_SUCCESS);
    TASK_CHECK(ma_resource_manager_init(&cfgMain, &rmMain) == MA_SUCCESS);
    TASK_CHECK(ma_resource_manager_init(&cfgOther, &rmOther) == MA_SUCCESS);

    TASK_CHECK(open_stream(&rmRef, ".task-build/rec_sound.wav", 0, &refStream1) == 0);
    TASK_CHECK(open_stream(&rmRef, ".task-build/rec_sound.wav", 0, &refStream2) == 0);
    TASK_CHECK(pump_all(&rmRef) == 0);
    TASK_CHECK(open_stream(&rmMain, ".task-build/rec_sound.wav", 0, &mainStream) == 0);
    TASK_CHECK(pump_all(&rmMain) == 0);

    /* Two fresh reference rigs, one per target, each with pristine resampler history. */
    TASK_CHECK(fresh_rig_setup(&ref1, &rmRef, (ma_data_source*)&refStream1, pitch) == 0);
    TASK_CHECK(fresh_rig_setup(&ref2, &rmRef, (ma_data_source*)&refStream2, pitch) == 0);

    /* Main engine: the coalescing-stream sound plus an independent in-memory "other" sound. */
    TASK_CHECK(engine_setup(&mainEng, &rmMain) == 0);
    TASK_CHECK(ma_sound_init_from_data_source(&mainEng.engine, (ma_data_source*)&mainStream,
        MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &mainSound) == MA_SUCCESS);
    ma_sound_set_pitch(&mainSound, pitch);
    TASK_CHECK(ma_sound_start(&mainSound) == MA_SUCCESS);

    bcfg = ma_audio_buffer_config_init(ma_format_f32, 2, BUFFER_FRAMES, bufferPcm, NULL);
    TASK_CHECK(ma_audio_buffer_init(&bcfg, &buffer) == MA_SUCCESS);
    TASK_CHECK(ma_sound_init_from_data_source(&mainEng.engine, (ma_data_source*)&buffer.ref,
        MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &otherSound) == MA_SUCCESS);
    TASK_CHECK(ma_sound_start(&otherSound) == MA_SUCCESS);

    /* Lockstep engine holding only the other sound, with identical PCM and the same period schedule. */
    TASK_CHECK(engine_setup(&otherEng, &rmOther) == 0);
    TASK_CHECK(ma_audio_buffer_init(&bcfg, &bufferRef) == MA_SUCCESS);
    TASK_CHECK(ma_sound_init_from_data_source(&otherEng.engine, (ma_data_source*)&bufferRef.ref,
        MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &otherSoundRef) == MA_SUCCESS);
    TASK_CHECK(ma_sound_start(&otherSoundRef) == MA_SUCCESS);

    /* Pre-populate the main sound's processing cache and resampler history with real pre-seek audio; advance both engines in lockstep. */
    TASK_CHECK(render(&mainEng, warm, WARM_FRAMES, &got) == 0);
    TASK_CHECK(!is_silent(warm, WARM_FRAMES * 2));
    {
        float warmOther[WARM_FRAMES * 2];
        TASK_CHECK(render(&otherEng, warmOther, WARM_FRAMES, NULL) == 0);
        TASK_CHECK(!is_silent(warmOther, WARM_FRAMES * 2));
    }

    /* Submit the coalesced seek but leave its job unprocessed. */
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&mainStream, target1) == MA_SUCCESS);
    TASK_CHECK(ma_resource_manager_data_stream_get_cursor_in_pcm_frames(&mainStream, &cursorBefore) == MA_SUCCESS);
    TASK_CHECK(cursorBefore == target1);

    /* BUSY window: the coalescing sound alone is muted while the other sound keeps playing, so the buses match exactly. */
    TASK_CHECK(render(&mainEng, mainBusy, RENDER_FRAMES, NULL) == 0);
    TASK_CHECK(!is_silent(mainBusy, RENDER_FRAMES * 2));
    TASK_CHECK(ma_sound_at_end(&mainSound) == MA_FALSE);
    TASK_CHECK(ma_resource_manager_data_stream_get_cursor_in_pcm_frames(&mainStream, &cursorAfter) == MA_SUCCESS);
    TASK_CHECK(cursorAfter == cursorBefore);
    TASK_CHECK(render(&otherEng, otherBusy, RENDER_FRAMES, NULL) == 0);
    for (i = 0; i < RENDER_FRAMES * 2; ++i) {
        TASK_CHECK(mainBusy[i] == otherBusy[i]);
    }

    /* Settle generation 1 and compare against a brand new sound at the same target. */
    TASK_CHECK(fresh_rig_render_at(&ref1, &rmRef, target1, refOut1, RENDER_FRAMES) == 0);
    TASK_CHECK(wait_settled(&rmMain, &mainStream) == 0);
    TASK_CHECK(check_state(&mainStream, 1, 1, target1, MA_SUCCESS) == 0);
    TASK_CHECK(render(&mainEng, mainAfter1, RENDER_FRAMES, NULL) == 0);
    TASK_CHECK(render(&otherEng, otherOnly1, RENDER_FRAMES, NULL) == 0);
    for (i = 0; i < RENDER_FRAMES * 2; ++i) {
        float expected = otherOnly1[i] + refOut1[i];
        TASK_CHECK(fabs((double)mainAfter1[i] - (double)expected) <= 0.000001);
    }

    /* Generation 2: another override, same mute/reset/match contract with the second fresh rig. */
    TASK_CHECK(ma_resource_manager_data_stream_seek_to_pcm_frame(&mainStream, target2) == MA_SUCCESS);
    TASK_CHECK(render(&mainEng, mainBusy, RENDER_FRAMES, NULL) == 0);
    TASK_CHECK(render(&otherEng, otherBusy, RENDER_FRAMES, NULL) == 0);
    for (i = 0; i < RENDER_FRAMES * 2; ++i) {
        TASK_CHECK(mainBusy[i] == otherBusy[i]);
    }

    TASK_CHECK(fresh_rig_render_at(&ref2, &rmRef, target2, refOut2, RENDER_FRAMES) == 0);
    TASK_CHECK(wait_settled(&rmMain, &mainStream) == 0);
    TASK_CHECK(check_state(&mainStream, 2, 2, target2, MA_SUCCESS) == 0);
    TASK_CHECK(render(&mainEng, mainAfter2, RENDER_FRAMES, NULL) == 0);
    TASK_CHECK(render(&otherEng, otherOnly2, RENDER_FRAMES, NULL) == 0);
    for (i = 0; i < RENDER_FRAMES * 2; ++i) {
        float expected = otherOnly2[i] + refOut2[i];
        TASK_CHECK(fabs((double)mainAfter2[i] - (double)expected) <= 0.000001);
    }

    ma_sound_uninit(&otherSoundRef);
    ma_sound_uninit(&otherSound);
    ma_sound_uninit(&mainSound);
    ma_sound_uninit(&ref2.sound);
    ma_sound_uninit(&ref1.sound);
    ma_engine_uninit(&otherEng.engine);
    ma_engine_uninit(&mainEng.engine);
    ma_engine_uninit(&ref2.engine);
    ma_engine_uninit(&ref1.engine);
    ma_audio_buffer_uninit(&bufferRef);
    ma_audio_buffer_uninit(&buffer);
    ma_resource_manager_data_stream_uninit(&mainStream);
    ma_resource_manager_data_stream_uninit(&refStream2);
    ma_resource_manager_data_stream_uninit(&refStream1);
    ma_resource_manager_uninit(&rmOther);
    ma_resource_manager_uninit(&rmMain);
    ma_resource_manager_uninit(&rmRef);
    remove(".task-build/rec_sound.wav");
    return 0;
}

static int case_sound_recovery_pitch_075(void) { return case_sound_recovery_pitch(0.75f); }
static int case_sound_recovery_pitch_125(void) { return case_sound_recovery_pitch(1.25f); }

static int case_sound_flag_file_based_init(void)
{
    /* MA_SOUND_FLAG_SEEK_COALESCING must flow through ma_sound_init_from_file into the backing stream. */
    ma_resource_manager_config cfg = rm_config(64, NULL, ma_format_s16, 2);
    ma_resource_manager rm;
    ma_engine_config ec = ma_engine_config_init();
    ma_engine engine;
    ma_sound sound;
    ma_resource_manager_data_source* backing;
    ma_resource_manager_data_stream* stream;
    ma_resource_manager_seek_state st;
    float output[256 * 2];
    ma_uint64 got = 0;

    TASK_CHECK(write_wav_s16(".task-build/rec_flag.wav", WAV_FRAMES, 2) == 0);
    TASK_CHECK(ma_resource_manager_init(&cfg, &rm) == MA_SUCCESS);

    ec.noDevice = MA_TRUE;
    ec.channels = 2;
    ec.sampleRate = SAMPLE_RATE;
    ec.periodSizeInFrames = 256;
    ec.pResourceManager = &rm;
    TASK_CHECK(ma_engine_init(&ec, &engine) == MA_SUCCESS);

    TASK_CHECK(ma_sound_init_from_file(&engine, ".task-build/rec_flag.wav",
        MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_SEEK_COALESCING | MA_SOUND_FLAG_NO_SPATIALIZATION,
        NULL, NULL, &sound) == MA_SUCCESS);

    backing = sound.pResourceManagerDataSource;
    TASK_CHECK(backing != NULL);
    TASK_CHECK((backing->flags & MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_SEEK_COALESCING) != 0);
    stream = &backing->backend.stream;
    TASK_CHECK(ma_resource_manager_data_stream_get_seek_state(stream, &st) == MA_SUCCESS);
    TASK_CHECK(st.requested == 0 && st.settled == 0 && st.result == MA_SUCCESS);

    TASK_CHECK(ma_sound_start(&sound) == MA_SUCCESS);
    TASK_CHECK(ma_sound_seek_to_pcm_frame(&sound, 3000) == MA_SUCCESS);
    TASK_CHECK(ma_engine_read_pcm_frames(&engine, output, 256, &got) == MA_SUCCESS);
    TASK_CHECK(got == 256);
    TASK_CHECK(is_silent(output, 256 * 2));          /* BUSY generation mutes the sound. */
    TASK_CHECK(pump_all(&rm) == 0);
    TASK_CHECK(ma_resource_manager_data_stream_get_seek_state(stream, &st) == MA_SUCCESS);
    TASK_CHECK(st.settled == 1 && st.result == MA_SUCCESS);

    ma_sound_uninit(&sound);
    ma_engine_uninit(&engine);
    ma_resource_manager_uninit(&rm);
    remove(".task-build/rec_flag.wav");
    return 0;
}

/* ================================================================== */

const struct task_case task_cases[] = {
    { "mailbox", "initial_and_basic_dispatch",      case_initial_and_basic_dispatch },
    { "mailbox", "burst_presubmit_one_decode",      case_burst_presubmit_one_decode },
    { "mailbox", "mid_decode_override_worker",      case_mid_decode_override_with_worker },
    { "mailbox", "mid_read_override_worker",        case_mid_read_override_with_worker },
    { "mailbox", "concurrent_hammer_no_lost_wake",  case_concurrent_hammer_no_lost_wake },
    { "mailbox", "f32_stream",                      case_f32_stream },
    { "mailbox", "mono_stream",                     case_mono_stream },

    { "publication", "bounded_jobs_and_pages",          case_bounded_jobs_and_pages },
    { "publication", "queued_page_job_ordering",        case_queued_page_job_ordering },
    { "publication", "queue_full_rejects_then_recovers", case_queue_full_rejects_then_recovers },
    { "publication", "override_needs_no_slot_when_full", case_override_needs_no_slot_when_full },
    { "publication", "eof_and_backward_seek",           case_eof_and_backward_seek },
    { "publication", "invalid_args_and_legacy_stream",  case_invalid_args_and_legacy_stream },

    { "recovery", "seek_failure_publishes_error",   case_seek_failure_publishes_error_and_recovers },
    { "recovery", "two_streams_independent",        case_two_streams_independent },
    { "recovery", "sound_recovery_pitch_075",       case_sound_recovery_pitch_075 },
    { "recovery", "sound_recovery_pitch_125",       case_sound_recovery_pitch_125 },
    { "recovery", "sound_flag_file_based_init",     case_sound_flag_file_based_init },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
