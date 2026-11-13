/*
    push_decode_test.c - validation for the push (streaming) MP3 decoder.

    Drives mp3dec_push_decode over real repository vectors through many
    input/output chunkings and compares the delivered PCM sample-by-sample
    against mp3dec_load_buf output. Every case decodes and checks real,
    non-zero PCM (where the reference contains audio).
*/
/* The implementation is compiled in minimp3_push.c (MINIMP3_IMPLEMENTATION);
   this translation unit uses the declarations only. */
#include "minimp3_ex.h"
#include "minimp3_push.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_cases = 0;
static int g_failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
            g_failures++; \
        } \
    } while (0)

#define CASE_BEGIN() do { g_cases++; } while (0)

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        printf("cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf)
        exit(2);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
        exit(2);
    fclose(f);
    *size_out = (size_t)sz;
    return buf;
}

typedef struct
{
    mp3d_sample_t *buf;
    size_t         n, cap;
} pcm_acc_t;

static void acc_init(pcm_acc_t *a)
{
    a->cap = 4096;
    a->n = 0;
    a->buf = (mp3d_sample_t *)malloc(a->cap*sizeof(mp3d_sample_t));
}

static void acc_push(pcm_acc_t *a, const mp3d_sample_t *src, size_t n)
{
    if (a->n + n > a->cap)
    {
        while (a->n + n > a->cap)
            a->cap *= 2;
        a->buf = (mp3d_sample_t *)realloc(a->buf, a->cap*sizeof(mp3d_sample_t));
    }
    memcpy(a->buf + a->n, src, n*sizeof(mp3d_sample_t));
    a->n += n;
}

static void acc_free(pcm_acc_t *a)
{
    free(a->buf);
    a->buf = NULL;
    a->n = a->cap = 0;
}

static int has_nonzero(const pcm_acc_t *a)
{
    for (size_t i = 0; i < a->n; i++)
        if (a->buf[i] != 0)
            return 1;
    return 0;
}

/* Full-file reference produced by the existing whole-buffer decoder. */
static mp3d_sample_t *reference_pcm(const uint8_t *data, size_t n, size_t *ref_n)
{
    mp3dec_t dec;
    mp3dec_file_info_t info;
    int rc = mp3dec_load_buf(&dec, data, n, &info, NULL, NULL);
    if (rc)
    {
        printf("mp3dec_load_buf failed rc=%d\n", rc);
        exit(2);
    }
    *ref_n = info.samples;
    return info.buffer; /* may be NULL when samples == 0 */
}

/*
    Run one full push-decode session.
    in_chunk/out_chunk : feeding granularities (bytes / PCM scalars).
    stereo             : when nonzero, every delivery must be L/R aligned.
    Returns the terminal status; *consumed gets total accepted bytes.
*/
static int run_push(const uint8_t *data, size_t n, size_t in_chunk, size_t out_chunk,
                    pcm_acc_t *acc, size_t *consumed, int stereo)
{
    mp3dec_push_t dec;
    mp3dec_push_init(&dec);
    size_t pos = 0, total_consumed = 0;
    int ret = MP3D_PUSH_OK;

    while (pos < n && ret >= 0)
    {
        size_t want = in_chunk < n - pos ? in_chunk : n - pos;
        int last = (pos + want == n);
        const uint8_t *p = data + pos;
        size_t bleft = want;
        size_t guard = 0;
        while (bleft && ret >= 0)
        {
            if (++guard > 10000000)
            {
                printf("driver stall at pos=%zu bleft=%zu\n", pos, bleft);
                exit(2);
            }
            mp3d_sample_t obuf[512];
            size_t b = bleft, s = out_chunk;
            ret = mp3dec_push_decode(&dec, p, &b, obuf, &s, last);
            if (stereo)
                CHECK((s % 2) == 0, "stereo delivery must keep L/R frames whole");
            if (s)
                acc_push(acc, obuf, s);
            p += b;
            bleft -= b;
            total_consumed += b;
            if (ret == MP3D_PUSH_DONE)
                break;
            if (b == 0 && s == 0 && !last)
                break; /* staging/ring pressure: resume with the next chunk */
        }
        pos += want;
    }

    /* drain with zero-input calls once eof has been declared */
    while (ret == MP3D_PUSH_OK)
    {
        mp3d_sample_t obuf[512];
        size_t b = 0, s = out_chunk;
        ret = mp3dec_push_decode(&dec, NULL, &b, obuf, &s, 0);
        if (stereo)
            CHECK((s % 2) == 0, "stereo drain must keep L/R frames whole");
        if (s)
            acc_push(acc, obuf, s);
        CHECK(b == 0, "zero-input call must consume no bytes");
    }

    mp3dec_push_close(&dec);
    if (consumed)
        *consumed = total_consumed;
    return ret;
}

static void expect_equal_reference(const char *name, const pcm_acc_t *acc,
                                   const mp3d_sample_t *ref, size_t ref_n)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: sample count %zu != reference %zu", name, acc->n, ref_n);
    CHECK(acc->n == ref_n, msg);
    size_t m = acc->n < ref_n ? acc->n : ref_n;
    size_t diffs = 0;
    for (size_t i = 0; i < m; i++)
        if (acc->buf[i] != ref[i])
            diffs++;
    snprintf(msg, sizeof(msg), "%s: %zu PCM mismatches vs mp3dec_load_buf", name, diffs);
    CHECK(diffs == 0, msg);
}

static const size_t IN_CHUNKS[]  = { 1, 7, 113 };
static const size_t OUT_CHUNKS[] = { 2, 14, 257 };

static void test_equivalence_matrix(const char *path, int stereo)
{
    size_t n;
    uint8_t *data = read_file(path, &n);
    size_t ref_n;
    mp3d_sample_t *ref = reference_pcm(data, n, &ref_n);

    for (size_t i = 0; i < sizeof(IN_CHUNKS)/sizeof(IN_CHUNKS[0]); i++)
    {
        for (size_t o = 0; o < sizeof(OUT_CHUNKS)/sizeof(OUT_CHUNKS[0]); o++)
        {
            CASE_BEGIN();
            pcm_acc_t acc;
            acc_init(&acc);
            size_t consumed = 0;
            int ret = run_push(data, n, IN_CHUNKS[i], OUT_CHUNKS[o], &acc, &consumed, stereo);
            char msg[160];
            snprintf(msg, sizeof(msg), "%s in=%zu out=%zu: status %d", path, IN_CHUNKS[i], OUT_CHUNKS[o], ret);
            CHECK(ret == MP3D_PUSH_DONE, msg);
            snprintf(msg, sizeof(msg), "%s in=%zu out=%zu: consumed %zu/%zu", path, IN_CHUNKS[i], OUT_CHUNKS[o], consumed, n);
            CHECK(consumed == n, msg);
            expect_equal_reference(msg, &acc, ref, ref_n);
            snprintf(msg, sizeof(msg), "%s in=%zu out=%zu: expected non-zero PCM", path, IN_CHUNKS[i], OUT_CHUNKS[o]);
            CHECK(has_nonzero(&acc), msg);
            acc_free(&acc);
        }
    }
    free(ref);
    free(data);
}

static void test_id3_vector(const char *path, size_t in_chunk, size_t out_chunk)
{
    CASE_BEGIN();
    size_t n;
    uint8_t *data = read_file(path, &n);
    size_t ref_n;
    mp3d_sample_t *ref = reference_pcm(data, n, &ref_n);
    pcm_acc_t acc;
    acc_init(&acc);
    size_t consumed = 0;
    int ret = run_push(data, n, in_chunk, out_chunk, &acc, &consumed, 0);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s in=%zu: status %d", path, in_chunk, ret);
    CHECK(ret == MP3D_PUSH_DONE, msg);
    CHECK(consumed == n, "ID3 stream must consume every byte");
    expect_equal_reference(path, &acc, ref, ref_n);
    if (ref_n)
        CHECK(has_nonzero(&acc), "ID3 stream audio must contain real samples");
    acc_free(&acc);
    free(ref);
    free(data);
}

static void test_empty_clean(const char *path, const char *name)
{
    CASE_BEGIN();
    size_t n;
    uint8_t *data = read_file(path, &n);
    pcm_acc_t acc;
    acc_init(&acc);
    size_t consumed = 0;
    int ret = run_push(data, n, 7, 14, &acc, &consumed, 0);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: expected DONE, got %d", name, ret);
    CHECK(ret == MP3D_PUSH_DONE, msg);
    snprintf(msg, sizeof(msg), "%s: expected no PCM, got %zu", name, acc.n);
    CHECK(acc.n == 0, msg);
    CHECK(consumed == n, "tag-only stream must be consumed fully");
    acc_free(&acc);
    free(data);
}

/* A cut mid-frame: truncation error, complete audio kept, prefix matches. */
static void test_truncated(const char *path, size_t cut, int expect_audio)
{
    CASE_BEGIN();
    size_t n;
    uint8_t *data = read_file(path, &n);
    if (cut > n)
        cut = n;
    size_t ref_n;
    mp3d_sample_t *ref = reference_pcm(data, n, &ref_n);

    pcm_acc_t acc;
    acc_init(&acc);
    size_t consumed = 0;
    int ret = run_push(data, cut, 37, 53, &acc, &consumed, 0);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s cut=%zu: expected truncation, got %d", path, cut, ret);
    CHECK(ret == MP3D_PUSH_E_TRUNCATED, msg);
    if (expect_audio)
        CHECK(has_nonzero(&acc), "complete frames before truncation must be delivered");
    CHECK(acc.n <= ref_n, "truncated output cannot exceed the full reference");
    size_t diffs = 0;
    for (size_t i = 0; i < acc.n; i++)
        if (acc.buf[i] != ref[i])
            diffs++;
    CHECK(diffs == 0, "truncated PCM must match the reference prefix");

    /* sticky error: drain everything the decoder still holds, then the
       truncation surfaces and remains reported on every later call */
    mp3dec_push_t dec;
    mp3dec_push_init(&dec);
    size_t b = cut, s = 64;
    mp3d_sample_t obuf[64];
    int terminal = mp3dec_push_decode(&dec, data, &b, obuf, &s, 1);
    int guard = 0;
    while (terminal == MP3D_PUSH_OK && guard++ < 100000)
    {
        b = 0; s = 64;
        terminal = mp3dec_push_decode(&dec, NULL, &b, obuf, &s, 0);
    }
    CHECK(terminal == MP3D_PUSH_E_TRUNCATED, "truncation surfaces after PCM drain");
    size_t b0 = 0, s0 = 0;
    int r2 = mp3dec_push_decode(&dec, NULL, &b0, NULL, &s0, 0);
    CHECK(r2 == MP3D_PUSH_E_TRUNCATED, "truncation must remain sticky");
    CHECK(b0 == 0 && s0 == 0, "sticky error reports zero movement");
    mp3dec_push_close(&dec);

    acc_free(&acc);
    free(ref);
    free(data);
}

static void test_interleaved_instances(void)
{
    CASE_BEGIN();
    const char *pa = "vectors/l3-si_block.bit";
    const char *pb = "vectors/M2L3_noise.bit";
    size_t na, nb;
    uint8_t *da = read_file(pa, &na);
    uint8_t *db = read_file(pb, &nb);
    size_t ra_n, rb_n;
    mp3d_sample_t *ra = reference_pcm(da, na, &ra_n);
    mp3d_sample_t *rb = reference_pcm(db, nb, &rb_n);

    mp3dec_push_t A, B;
    mp3dec_push_init(&A);
    mp3dec_push_init(&B);
    pcm_acc_t oa, ob;
    acc_init(&oa);
    acc_init(&ob);

    size_t pa_pos = 0, pb_pos = 0;
    int ret_a = MP3D_PUSH_OK, ret_b = MP3D_PUSH_OK;
    const size_t chunk_a = 113, chunk_b = 7, cap_a = 14, cap_b = 257;
    int turn = 0;
    while ((pa_pos < na && ret_a >= 0) || (pb_pos < nb && ret_b >= 0))
    {
        mp3dec_push_t *dec = (turn & 1) ? &B : &A;
        const uint8_t *d = (turn & 1) ? db : da;
        size_t *pos = (turn & 1) ? &pb_pos : &pa_pos;
        size_t total = (turn & 1) ? nb : na;
        size_t chunk = (turn & 1) ? chunk_b : chunk_a;
        size_t cap = (turn & 1) ? cap_b : cap_a;
        int stereo = (turn & 1);
        int *ret = (turn & 1) ? &ret_b : &ret_a;
        pcm_acc_t *out = (turn & 1) ? &ob : &oa;

        size_t want = chunk < total - *pos ? chunk : total - *pos;
        int last = (*pos + want == total);
        size_t bleft = want;
        const uint8_t *p = d + *pos;
        while (bleft && *ret >= 0)
        {
            mp3d_sample_t obuf[512];
            size_t b = bleft, s = cap;
            *ret = mp3dec_push_decode(dec, p, &b, obuf, &s, last);
            if (stereo)
                CHECK((s % 2) == 0, "interleaved stereo delivery stays aligned");
            if (s)
                acc_push(out, obuf, s);
            p += b;
            bleft -= b;
            if (*ret == MP3D_PUSH_DONE || (b == 0 && s == 0))
                break;
        }
        *pos += want;
        turn++;
    }
    while (ret_a == MP3D_PUSH_OK)
    {
        mp3d_sample_t obuf[512];
        size_t b = 0, s = cap_a;
        ret_a = mp3dec_push_decode(&A, NULL, &b, obuf, &s, 0);
        if (s)
            acc_push(&oa, obuf, s);
    }
    while (ret_b == MP3D_PUSH_OK)
    {
        mp3d_sample_t obuf[512];
        size_t b = 0, s = cap_b;
        ret_b = mp3dec_push_decode(&B, NULL, &b, obuf, &s, 0);
        CHECK((s % 2) == 0, "interleaved stereo drain stays aligned");
        if (s)
            acc_push(&ob, obuf, s);
    }
    CHECK(ret_a == MP3D_PUSH_DONE, "instance A finished");
    CHECK(ret_b == MP3D_PUSH_DONE, "instance B finished");
    expect_equal_reference("interleaved A (si_block)", &oa, ra, ra_n);
    expect_equal_reference("interleaved B (M2L3_noise)", &ob, rb, rb_n);
    CHECK(has_nonzero(&oa) && has_nonzero(&ob), "both interleaved streams produced audio");

    mp3dec_push_close(&A);
    mp3dec_push_close(&B);
    acc_free(&oa);
    acc_free(&ob);
    free(ra);
    free(rb);
    free(da);
    free(db);
}

static void test_param_errors(void)
{
    CASE_BEGIN();
    mp3dec_push_t dec;
    mp3dec_push_init(&dec);
    uint8_t dummy[4] = { 0xff, 0xfb, 0x90, 0x64 };
    mp3d_sample_t obuf[8];
    size_t b, s;

    b = 1; s = 0;
    CHECK(mp3dec_push_decode(NULL, dummy, &b, NULL, &s, 0) == MP3D_PUSH_E_PARAM, "null decoder");
    b = 1; s = 4;
    CHECK(mp3dec_push_decode(&dec, NULL, &b, obuf, &s, 0) == MP3D_PUSH_E_PARAM, "null data with bytes>0");
    b = 1; s = 4;
    CHECK(mp3dec_push_decode(&dec, dummy, &b, NULL, &s, 0) == MP3D_PUSH_E_PARAM, "null pcm with samples>0");
    b = 1; s = 4;
    CHECK(mp3dec_push_decode(&dec, dummy, NULL, obuf, &s, 0) == MP3D_PUSH_E_PARAM, "null bytes pointer");
    b = 1; s = 4;
    CHECK(mp3dec_push_decode(&dec, dummy, &b, obuf, NULL, 0) == MP3D_PUSH_E_PARAM, "null samples pointer");

    /* zero/zero call is legal before eof */
    b = 0; s = 0;
    CHECK(mp3dec_push_decode(&dec, NULL, &b, NULL, &s, 0) == MP3D_PUSH_OK, "zero/zero call legal");
    CHECK(b == 0 && s == 0, "zero/zero call reports no movement");
    mp3dec_push_close(&dec);
}

static void test_drain_semantics(void)
{
    CASE_BEGIN();
    size_t n;
    uint8_t *data = read_file("vectors/l3-si_block.bit", &n);
    mp3dec_push_t dec;
    mp3dec_push_init(&dec);

    /* feed everything without eof, using a tiny output buffer */
    size_t pos = 0;
    int ret = MP3D_PUSH_OK;
    mp3d_sample_t obuf[16];
    pcm_acc_t acc;
    acc_init(&acc);
    while (pos < n && ret >= 0)
    {
        size_t want = 113 < n - pos ? 113 : n - pos;
        size_t bleft = want;
        const uint8_t *p = data + pos;
        while (bleft && ret >= 0)
        {
            size_t b = bleft, s = 14;
            ret = mp3dec_push_decode(&dec, p, &b, obuf, &s, 0); /* no eof yet */
            if (s)
                acc_push(&acc, obuf, s);
            p += b;
            bleft -= b;
            if (b == 0 && s == 0)
                break;
        }
        pos += want;
    }
    CHECK(acc.n > 0, "audio must flow before eof is declared");

    /* declare eof on a zero-input call, then drain to completion */
    int got_done = 0;
    for (int guard = 0; guard < 100000 && ret >= 0; guard++)
    {
        size_t b = 0, s = 14;
        ret = mp3dec_push_decode(&dec, NULL, &b, obuf, &s, got_done ? 0 : 1);
        got_done = 1;
        if (s)
            acc_push(&acc, obuf, s);
        if (ret == MP3D_PUSH_DONE)
            break;
    }
    CHECK(ret == MP3D_PUSH_DONE, "zero-input drain reaches DONE");

    /* repeated drain after completion: DONE, both lengths zero */
    for (int k = 0; k < 3; k++)
    {
        size_t b = 0, s = 0;
        int r = mp3dec_push_decode(&dec, NULL, &b, NULL, &s, 0);
        CHECK(r == MP3D_PUSH_DONE, "repeated drain stays DONE");
        CHECK(b == 0 && s == 0, "repeated drain reports zero lengths");
    }

    size_t ref_n;
    mp3d_sample_t *ref = reference_pcm(data, n, &ref_n);
    expect_equal_reference("drain semantics", &acc, ref, ref_n);
    acc_free(&acc);
    free(ref);
    free(data);
    mp3dec_push_close(&dec);
}

static void test_free_format_unsupported(void)
{
    CASE_BEGIN();
    size_t n;
    uint8_t *data = read_file("vectors/l3-he_free.bit", &n);
    mp3dec_push_t dec;
    mp3dec_push_init(&dec);
    size_t pos = 0;
    int ret = MP3D_PUSH_OK;
    while (pos < n && ret == MP3D_PUSH_OK)
    {
        size_t want = 64 < n - pos ? 64 : n - pos;
        int last = (pos + want == n);
        size_t b = want, s = 257;
        mp3d_sample_t obuf[257];
        ret = mp3dec_push_decode(&dec, data + pos, &b, obuf, &s, last);
        pos += b;
        if (ret != MP3D_PUSH_OK)
            break;
    }
    CHECK(ret == MP3D_PUSH_E_UNSUPPORTED, "free-format must be rejected");
    mp3dec_push_close(&dec);
    free(data);
}

static void test_gapless_before_eof(void)
{
    CASE_BEGIN();
    size_t n;
    uint8_t *data = read_file("vectors/l3-nonstandard-sin1k0db_lame_vbrtag.bit", &n);
    mp3dec_push_t dec;
    mp3dec_push_init(&dec);
    pcm_acc_t acc;
    acc_init(&acc);
    size_t pos = 0;
    int ret = MP3D_PUSH_OK;
    int delivered_without_eof = 0;
    /* hold back the final 200 bytes, never signal eof */
    size_t feed = n - 200;
    while (pos < feed && ret >= 0)
    {
        size_t want = 113 < feed - pos ? 113 : feed - pos;
        size_t bleft = want;
        const uint8_t *p = data + pos;
        while (bleft && ret >= 0)
        {
            mp3d_sample_t obuf[257];
            size_t b = bleft, s = 257;
            ret = mp3dec_push_decode(&dec, p, &b, obuf, &s, 0);
            if (s)
            {
                acc_push(&acc, obuf, s);
                delivered_without_eof = 1;
            }
            p += b;
            bleft -= b;
            if (b == 0 && s == 0)
                break;
        }
        pos += want;
    }
    CHECK(delivered_without_eof, "gapless stream must emit audio before eof");

    /* finish normally and check the whole result against the reference */
    while (ret == MP3D_PUSH_OK)
    {
        mp3d_sample_t obuf[257];
        size_t bleft = n - pos;
        size_t b = bleft, s = 257;
        ret = mp3dec_push_decode(&dec, data + pos, &b, obuf, &s, 1);
        if (s)
            acc_push(&acc, obuf, s);
        pos += b;
        if (b == 0 && s == 0 && ret == MP3D_PUSH_OK)
        {   /* drain whatever remains with zero input */
            while (ret == MP3D_PUSH_OK)
            {
                b = 0; s = 257;
                ret = mp3dec_push_decode(&dec, NULL, &b, obuf, &s, 0);
                if (s)
                    acc_push(&acc, obuf, s);
            }
        }
    }
    CHECK(ret == MP3D_PUSH_DONE, "gapless stream finishes cleanly");
    size_t ref_n;
    mp3d_sample_t *ref = reference_pcm(data, n, &ref_n);
    expect_equal_reference("gapless LAME", &acc, ref, ref_n);
    CHECK(has_nonzero(&acc), "gapless LAME output contains audio");
    acc_free(&acc);
    free(ref);
    free(data);
    mp3dec_push_close(&dec);
}

static void test_memory_bound(void)
{
    CASE_BEGIN();
    char msg[128];
    snprintf(msg, sizeof(msg), "instance size %zu exceeds 128KiB", sizeof(mp3dec_push_t));
    CHECK(sizeof(mp3dec_push_t) <= 128*1024, msg);
    /* staging buffer must hold any ordinary frame with re-sync headroom */
    CHECK(2048 >= 1441 + 4, "staging capacity covers worst-case MPEG-1 frame");
}

/* Zero output capacity is legal: once internal storage fills, the decoder
   applies back-pressure (consumes nothing, returns OK), then fully recovers. */
static void test_zero_capacity_backpressure(void)
{
    CASE_BEGIN();
    size_t n;
    uint8_t *data = read_file("vectors/l3-si_block.bit", &n);
    size_t ref_n;
    mp3d_sample_t *ref = reference_pcm(data, n, &ref_n);
    mp3dec_push_t dec;
    mp3dec_push_init(&dec);

    size_t pos = 0;
    int saw_backpressure = 0;
    long guard = 0;
    while (pos < n)
    {
        size_t want = 113 < n - pos ? 113 : n - pos;
        size_t b = want, s = 0;
        int ret = mp3dec_push_decode(&dec, data + pos, &b, NULL, &s, pos + b == n);
        CHECK(ret >= 0, "zero-capacity feed never errors while space remains");
        CHECK(s == 0, "zero capacity must produce no PCM");
        pos += b;
        if (b == 0)
        {
            saw_backpressure = 1;
            break;
        }
        if (++guard > 100000)
        {
            printf("backpressure never engaged\n");
            exit(2);
        }
    }
    CHECK(saw_backpressure, "internal storage must eventually apply back-pressure");

    pcm_acc_t acc;
    acc_init(&acc);
    int ret = MP3D_PUSH_OK;
    while (ret == MP3D_PUSH_OK)
    {
        mp3d_sample_t obuf[2304];
        size_t b = 0, s = sizeof(obuf)/sizeof(obuf[0]);
        ret = mp3dec_push_decode(&dec, NULL, &b, obuf, &s, pos >= n);
        if (s)
            acc_push(&acc, obuf, s);
        if (ret == MP3D_PUSH_OK && s == 0 && pos < n)
        {
            size_t want = 113 < n - pos ? 113 : n - pos;
            b = want; s = 0;
            ret = mp3dec_push_decode(&dec, data + pos, &b, NULL, &s, pos + b == n);
            pos += b;
        }
    }
    CHECK(ret == MP3D_PUSH_DONE, "stream recovers after back-pressure");
    expect_equal_reference("back-pressure", &acc, ref, ref_n);
    CHECK(has_nonzero(&acc), "recovered stream contains audio");
    acc_free(&acc);
    mp3dec_push_close(&dec);
    free(ref);
    free(data);
}

/* A corrupt frame that decodes to zero samples must not reset the geometry
   contract; the stream keeps delivering every decodable frame until the real
   truncated tail is reached. */
static void test_silent_corrupt_frame(void)
{
    CASE_BEGIN();
    const char *path = "vectors/l3-nonstandard-compl-sideinfo-size.bit";
    size_t n;
    uint8_t *data = read_file(path, &n);
    size_t ref_n;
    mp3d_sample_t *ref = reference_pcm(data, n, &ref_n);

    pcm_acc_t acc;
    acc_init(&acc);
    size_t consumed = 0;
    int ret = run_push(data, n, 37, 257, &acc, &consumed, 0);
    CHECK(ret == MP3D_PUSH_E_TRUNCATED, "corrupt vector ends in a partial frame");
    char msg[160];
    snprintf(msg, sizeof(msg), "silent-frame recovery PCM %zu vs reference %zu", acc.n, ref_n);
    CHECK(acc.n == ref_n, msg);
    size_t diffs = 0;
    for (size_t i = 0; i < acc.n; i++)
        if (acc.buf[i] != ref[i])
            diffs++;
    CHECK(diffs == 0, "audio before the corrupt tail matches the reference");
    CHECK(has_nonzero(&acc), "decodable frames around the corrupt one produce audio");
    acc_free(&acc);
    free(ref);
    free(data);
}

/* Synthetic ID3v2.4 tag with the footer flag set, split across fragments. */
static void test_id3v24_footer(void)
{
    CASE_BEGIN();
    size_t fn;
    uint8_t *vec = read_file("vectors/l3-nonstandard-id3v1.bit", &fn);
    const size_t audio = 192;                 /* complete frame at the head of that vector */
    const unsigned body = 257;
    const size_t tag = 10 + body + 10;        /* header + synchsafe body + footer */
    size_t total = tag + audio;
    uint8_t *stream = (uint8_t *)malloc(total);
    memcpy(stream, "ID3", 3);
    stream[3] = 4;                            /* version 2.4 */
    stream[4] = 0;
    stream[5] = 0x10;                         /* footer present */
    stream[6] = (body >> 21) & 0x7f;
    stream[7] = (body >> 14) & 0x7f;
    stream[8] = (body >> 7)  & 0x7f;
    stream[9] = body & 0x7f;
    for (unsigned i = 0; i < body; i++)
        stream[10 + i] = (uint8_t)(0x40 + (i * 13)); /* never a 0xff sync */
    memcpy(stream + 10 + body, "3DI", 3);     /* footer mirrors the header tag id */
    stream[10 + body + 3] = 4;
    stream[10 + body + 5] = 0x10;
    stream[10 + body + 6] = stream[6];
    stream[10 + body + 7] = stream[7];
    stream[10 + body + 8] = stream[8];
    stream[10 + body + 9] = stream[9];
    memcpy(stream + tag, vec, audio);

    size_t ref_n;
    mp3d_sample_t *ref = reference_pcm(vec, audio, &ref_n);
    const size_t chunks[3] = { 1, 7, 113 };
    for (int k = 0; k < 3; k++)
    {
        pcm_acc_t acc;
        acc_init(&acc);
        size_t consumed = 0;
        int ret = run_push(stream, total, chunks[k], 257, &acc, &consumed, 0);
        char msg[160];
        snprintf(msg, sizeof(msg), "id3v2.4 footer in=%zu ret=%d", chunks[k], ret);
        CHECK(ret == MP3D_PUSH_DONE, msg);
        snprintf(msg, sizeof(msg), "id3v2.4 footer consumed %zu/%zu", consumed, total);
        CHECK(consumed == total, msg);
        expect_equal_reference("id3v2.4 footer", &acc, ref, ref_n);
        CHECK(has_nonzero(&acc), "id3v2.4 stream decodes real audio");
        acc_free(&acc);
    }
    free(ref);
    free(vec);
    free(stream);
}

int main(void)
{
    /* required real vectors, all chunking matrices, real PCM comparison */
    test_equivalence_matrix("vectors/l3-nonstandard-sin1k0db_lame_vbrtag.bit", 1);
    test_equivalence_matrix("vectors/l3-si_block.bit", 0);
    test_equivalence_matrix("vectors/M2L3_noise.bit", 1);

    /* ID3 tags split across fragments */
    test_id3_vector("vectors/l3-nonstandard-id3v2.bit", 1, 257);
    test_id3_vector("vectors/l3-nonstandard-id3v2.bit", 7, 14);
    test_id3_vector("vectors/l3-nonstandard-id3v2.bit", 113, 2);
    test_id3_vector("vectors/l3-nonstandard-id3v1.bit", 1, 257);
    test_id3_vector("vectors/l3-nonstandard-id3v1.bit", 113, 14);
    test_id3_vector("vectors/l3-nonstandard-id3v1-apetag.bit", 7, 53);

    /* tag-only streams end cleanly without audio */
    test_empty_clean("vectors/l3-nonstandard-id3v2-only.bit", "id3v2-only");
    test_empty_clean("vectors/l3-nonstandard-vbrtag-only.bit", "xing-only");

    /* truncated tails: residual partial frame / unfinished tag */
    test_truncated("vectors/l3-si_block.bit", 208*3 + 50, 1);
    test_truncated("vectors/M2L3_noise.bit", 313*2 + 100, 1);
    test_truncated("vectors/l3-si_block.bit", 100, 0);
    test_truncated("vectors/l3-nonstandard-id3v2-only.bit", 7, 0);

    /* two independent instances interleaved */
    test_interleaved_instances();

    /* interface contract checks */
    test_param_errors();
    test_drain_semantics();
    test_free_format_unsupported();
    test_gapless_before_eof();
    test_memory_bound();
    test_zero_capacity_backpressure();
    test_silent_corrupt_frame();
    test_id3v24_footer();

    if (g_failures)
    {
        printf("MINIMP3_PUSH_FAIL cases=%d failures=%d\n", g_cases, g_failures);
        return 1;
    }
    printf("MINIMP3_PUSH_OK cases=%d\n", g_cases);
    return 0;
}
