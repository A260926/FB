/*
    Tests for the push (forward-only streaming) decoder.
    Real repository vectors are decoded through mp3dec_push_decode() under many
    fragmentation patterns and compared sample-by-sample with mp3dec_load_buf().
    The minimp3 implementation lives in minimp3_push.c; this is a client TU.
*/
#include "minimp3_ex.h"
#include "minimp3_push.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_cases = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("FAIL at %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        exit(1); \
    } } while (0)

#define CASE() do { g_cases++; } while (0)

typedef struct
{
    mp3d_sample_t *data;
    size_t         size, cap;
} pcm_vec;

static void vec_init(pcm_vec *v)
{
    v->data = NULL;
    v->size = v->cap = 0;
}

static void vec_append(pcm_vec *v, const mp3d_sample_t *src, size_t n)
{
    if (v->size + n > v->cap)
    {
        size_t cap = v->cap ? v->cap : 4096;
        while (cap < v->size + n)
            cap *= 2;
        mp3d_sample_t *p = (mp3d_sample_t *)realloc(v->data, cap*sizeof(mp3d_sample_t));
        CHECK(p != NULL, "out of memory");
        v->data = p;
        v->cap = cap;
    }
    if (n)
        memcpy(v->data + v->size, src, n*sizeof(mp3d_sample_t));
    v->size += n;
}

static uint8_t *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    CHECK(n > 0, "empty file %s", path);
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    CHECK(b != NULL, "out of memory");
    CHECK(fread(b, 1, (size_t)n, f) == (size_t)n, "short read %s", path);
    fclose(f);
    *out_size = (size_t)n;
    return b;
}

static mp3d_sample_t *reference_decode(const uint8_t *buf, size_t size, mp3dec_file_info_t *info)
{
    mp3dec_t dec;
    memset(info, 0, sizeof(*info));
    int r = mp3dec_load_buf(&dec, buf, size, info, NULL, NULL);
    CHECK(r == 0, "mp3dec_load_buf failed r=%d", r);
    return info->buffer;
}

static int vec_has_nonzero(const pcm_vec *v)
{
    size_t i;
    for (i = 0; i < v->size; i++)
        if (v->data[i] != 0)
            return 1;
    return 0;
}

static void check_matches_reference(const char *tag, const pcm_vec *got,
                                    const mp3d_sample_t *ref, size_t ref_samples, int channels)
{
    CHECK(got->size == ref_samples, "%s: sample count %zu != reference %zu",
          tag, got->size, ref_samples);
    CHECK(memcmp(got->data, ref, ref_samples*sizeof(mp3d_sample_t)) == 0,
          "%s: PCM differs from mp3dec_load_buf reference", tag);
    CHECK(vec_has_nonzero(got), "%s: decoded PCM is all zero", tag);
    if (channels == 2)
        CHECK((got->size & 1) == 0, "%s: stereo stream ended on an unpaired scalar", tag);
}

/*
    Feed the whole buffer using `in_chunk` byte fragments and drain with
    `out_chunk` scalar buffers; the final fragment carries eof. Returns the last
    decoder status and reports every byte consumed.
*/
static int run_stream(const uint8_t *buf, size_t size, size_t in_chunk, size_t out_chunk,
                      pcm_vec *out, size_t *consumed_total)
{
    mp3dec_push_t dec;
    mp3d_sample_t tmp[257];
    size_t pos = 0, guard = 0;
    int r, status = MP3D_PUSH_OK;
    CHECK(out_chunk <= sizeof(tmp)/sizeof(tmp[0]), "test output chunk too large");
    CHECK(mp3dec_push_init(&dec) == MP3D_PUSH_OK, "init failed");
    *consumed_total = 0;

    for (;;)
    {
        size_t ib = 0, ob = out_chunk;
        const uint8_t *in = NULL;
        int eof = 0;
        if (pos < size)
        {
            ib = size - pos < in_chunk ? size - pos : in_chunk;
            in = buf + pos;
            eof = (pos + ib == size);
        } else
        {
            eof = 1; /* post-eof drain calls carry no input */
        }
        r = mp3dec_push_decode(&dec, in, &ib, out_chunk ? tmp : NULL, &ob, eof);
        pos += ib;
        *consumed_total += ib;
        if (ob)
            vec_append(out, tmp, ob);
        guard++;
        CHECK(guard < 50000000, "stream did not terminate");

        if (r < 0)
        {
            status = r;
            break;
        }
        /* zero progress with OK is legal: waiting for more input or drain room */
        if (r == MP3D_PUSH_DONE)
        {
            size_t zb = 0, zs = 0;
            int r2 = mp3dec_push_decode(&dec, NULL, &zb, NULL, &zs, 1);
            CHECK(r2 == MP3D_PUSH_DONE && zb == 0 && zs == 0,
                  "repeated drain after DONE must stay DONE with zero lengths");
            status = MP3D_PUSH_DONE;
            break;
        }
    }
    CHECK(pos == size, "not all input was consumed (%zu/%zu)", pos, size);
    mp3dec_push_close(&dec);
    return status;
}

static void run_matrix(const char *path, const uint8_t *buf, size_t size,
                       const mp3d_sample_t *ref, size_t ref_samples, int channels)
{
    static const size_t in_chunks[]  = { 1, 7, 113 };
    static const size_t out_chunks[] = { 2, 14, 257 };
    size_t i, j;
    for (i = 0; i < sizeof(in_chunks)/sizeof(in_chunks[0]); i++)
    {
        for (j = 0; j < sizeof(out_chunks)/sizeof(out_chunks[0]); j++)
        {
            pcm_vec got;
            size_t consumed = 0;
            char tag[128];
            vec_init(&got);
            CASE();
            snprintf(tag, sizeof(tag), "%s in=%zu out=%zu",
                     path, in_chunks[i], out_chunks[j]);
            int r = run_stream(buf, size, in_chunks[i], out_chunks[j], &got, &consumed);
            CHECK(r == MP3D_PUSH_DONE, "%s: status %d", tag, r);
            CHECK(consumed == size, "%s: consumed %zu != %zu", tag, consumed, size);
            check_matches_reference(tag, &got, ref, ref_samples, channels);
            free(got.data);
        }
    }
}

/* exercise legal zero-length calls: accept input with no output room, then drain */
static void run_zero_caps(const uint8_t *buf, size_t size,
                          const mp3d_sample_t *ref, size_t ref_samples)
{
    mp3dec_push_t dec;
    pcm_vec got;
    size_t pos = 0, guard = 0;
    int r;
    vec_init(&got);
    CHECK(mp3dec_push_init(&dec) == MP3D_PUSH_OK, "init failed");
    CASE();

    while (1)
    {
        size_t ib, ob;
        guard++;
        CHECK(guard < 50000000, "zero-cap stream did not terminate");
        if (pos < size)
        {   /* accept bytes, provide no PCM capacity at all */
            ib = size - pos < 37 ? size - pos : 37;
            ob = 0;
            r = mp3dec_push_decode(&dec, buf + pos, &ib, NULL, &ob, pos + ib == size);
            CHECK(ob == 0, "samples reported with zero capacity");
            pos += ib;
            if (r < 0)
                break;
        } else
        {   /* no input, drain whatever is buffered */
            mp3d_sample_t tmp[11];
            ib = 0;
            ob = sizeof(tmp)/sizeof(tmp[0]);
            r = mp3dec_push_decode(&dec, NULL, &ib, tmp, &ob, 1);
            CHECK(ib == 0, "bytes consumed from a zero-length input call");
            if (ob)
                vec_append(&got, tmp, ob);
            if (r == MP3D_PUSH_DONE)
                break;
            if (r < 0)
                break;
        }
    }
    CHECK(r == MP3D_PUSH_DONE, "zero-cap run status %d", r);
    CHECK(pos == size, "zero-cap run left input unconsumed");
    check_matches_reference("zero-caps l3-si_block", &got, ref, ref_samples, 1);
    free(got.data);
    mp3dec_push_close(&dec);
}

/* cut the stream at several byte positions: complete sound stays, tail is an error */
static void run_truncation(const uint8_t *buf, size_t size,
                           const mp3d_sample_t *ref, size_t ref_samples)
{
    static const size_t tail_cuts[] = { 1, 7, 50 };
    size_t k;
    for (k = 0; k < sizeof(tail_cuts)/sizeof(tail_cuts[0]); k++)
    {
        pcm_vec got;
        size_t cut_size = size - tail_cuts[k], consumed = 0;
        vec_init(&got);
        CASE();
        int r = run_stream(buf, cut_size, 113, 14, &got, &consumed);
        CHECK(r == MP3D_PUSH_E_TRUNC, "cut %zu: expected truncation, got %d", tail_cuts[k], r);
        CHECK(consumed == cut_size, "cut %zu: not all presented bytes accounted", tail_cuts[k]);
        CHECK(got.size > 0 && got.size < ref_samples, "cut %zu: unexpected PCM length %zu",
              tail_cuts[k], got.size);
        CHECK(memcmp(got.data, ref, got.size*sizeof(mp3d_sample_t)) == 0,
              "cut %zu: delivered PCM is not a reference prefix", tail_cuts[k]);
        CHECK(vec_has_nonzero(&got), "cut %zu: delivered PCM is all zero", tail_cuts[k]);
        free(got.data);
    }

    /* cut inside the first frame: nothing is deliverable, the partial frame is reported */
    {
        pcm_vec got;
        size_t consumed = 0;
        vec_init(&got);
        CASE();
        int r = run_stream(buf, 50, 1, 2, &got, &consumed);
        CHECK(r == MP3D_PUSH_E_TRUNC, "first-frame cut: expected truncation, got %d", r);
        CHECK(got.size == 0, "first-frame cut: unexpected PCM");
        free(got.data);
    }

    /* one big eof fragment ending in a cut: the error call itself must still
       report the bytes it accepted and the PCM it delivered */
    {
        mp3dec_push_t dec;
        static mp3d_sample_t tmp[64];
        size_t cut_size = size - 7, ib = cut_size, ob = sizeof(tmp)/sizeof(tmp[0]);
        size_t seen_bytes = 0, seen_pcm = 0;
        int r;
        CHECK(mp3dec_push_init(&dec) == MP3D_PUSH_OK, "init failed");
        CASE();
        r = mp3dec_push_decode(&dec, buf, &ib, tmp, &ob, 1);
        seen_bytes += ib;
        seen_pcm += ob;
        CHECK(ib > 0, "single-fragment cut: first call reported no accepted bytes");
        while (r == MP3D_PUSH_OK)
        {
            ib = 0;
            ob = sizeof(tmp)/sizeof(tmp[0]);
            r = mp3dec_push_decode(&dec, NULL, &ib, tmp, &ob, 1);
            seen_bytes += ib;
            seen_pcm += ob;
        }
        CHECK(r == MP3D_PUSH_E_TRUNC, "single-fragment cut: expected truncation, got %d", r);
        CHECK(seen_bytes == cut_size, "single-fragment cut: consumed %zu != %zu",
              seen_bytes, cut_size);
        CHECK(seen_pcm > 0 && seen_pcm < ref_samples, "single-fragment cut: bad PCM count %zu",
              seen_pcm);
        mp3dec_push_close(&dec);
    }
}

/* two independent instances fed interleaved must keep fully separate state */
static void run_interleaved(const uint8_t *buf_a, size_t size_a,
                            const mp3d_sample_t *ref_a, size_t ref_a_n, int ch_a,
                            const uint8_t *buf_b, size_t size_b,
                            const mp3d_sample_t *ref_b, size_t ref_b_n, int ch_b)
{
    mp3dec_push_t da, db;
    pcm_vec out_a, out_b;
    mp3d_sample_t tmp_a[257], tmp_b[14];
    size_t pos_a = 0, pos_b = 0, guard = 0;
    int done_a = 0, done_b = 0;
    vec_init(&out_a);
    vec_init(&out_b);
    CHECK(mp3dec_push_init(&da) == MP3D_PUSH_OK && mp3dec_push_init(&db) == MP3D_PUSH_OK,
          "init failed");
    CASE();

    while (!done_a || !done_b)
    {
        guard++;
        CHECK(guard < 50000000, "interleaved stream did not terminate");
        if (!done_a)
        {
            size_t ib = 0, ob = sizeof(tmp_a)/sizeof(tmp_a[0]);
            const uint8_t *in = NULL;
            int eof = 1;
            if (pos_a < size_a)
            {
                ib = 1;
                in = buf_a + pos_a;
                eof = (pos_a + 1 == size_a);
            }
            int r = mp3dec_push_decode(&da, in, &ib, tmp_a, &ob, eof);
            pos_a += ib;
            if (ob)
                vec_append(&out_a, tmp_a, ob);
            CHECK(r >= 0, "instance A error %d", r);
            if (r == MP3D_PUSH_DONE)
                done_a = 1;
        }
        if (!done_b)
        {
            size_t ib = 0, ob = sizeof(tmp_b)/sizeof(tmp_b[0]);
            const uint8_t *in = NULL;
            int eof = 1;
            if (pos_b < size_b)
            {
                ib = size_b - pos_b < 7 ? size_b - pos_b : 7;
                in = buf_b + pos_b;
                eof = (pos_b + ib == size_b);
            }
            int r = mp3dec_push_decode(&db, in, &ib, tmp_b, &ob, eof);
            pos_b += ib;
            if (ob)
                vec_append(&out_b, tmp_b, ob);
            CHECK(r >= 0, "instance B error %d", r);
            if (r == MP3D_PUSH_DONE)
                done_b = 1;
        }
    }
    CHECK(pos_a == size_a && pos_b == size_b, "interleaved: input left unconsumed");
    check_matches_reference("interleaved A (LAME gapless)", &out_a, ref_a, ref_a_n, ch_a);
    check_matches_reference("interleaved B (M2L3)", &out_b, ref_b, ref_b_n, ch_b);
    free(out_a.data);
    free(out_b.data);
    mp3dec_push_close(&da);
    mp3dec_push_close(&db);
}

/* ID3v2 prefix (including a split header) must be skipped, not cached or decoded */
static void run_id3v2(const uint8_t *id3_buf, size_t id3_size,
                      const mp3d_sample_t *ref, size_t ref_samples)
{
    static const size_t in_chunks[]  = { 1, 3, 7, 113 };
    static const size_t out_chunks[] = { 2, 257 };
    size_t i, j;
    for (i = 0; i < sizeof(in_chunks)/sizeof(in_chunks[0]); i++)
    {
        for (j = 0; j < sizeof(out_chunks)/sizeof(out_chunks[0]); j++)
        {
            pcm_vec got;
            size_t consumed = 0;
            char tag[96];
            vec_init(&got);
            CASE();
            snprintf(tag, sizeof(tag), "id3v2 in=%zu out=%zu", in_chunks[i], out_chunks[j]);
            int r = run_stream(id3_buf, id3_size, in_chunks[i], out_chunks[j], &got, &consumed);
            CHECK(r == MP3D_PUSH_DONE, "%s: status %d", tag, r);
            CHECK(consumed == id3_size, "%s: consumed %zu != %zu", tag, consumed, id3_size);
            check_matches_reference(tag, &got, ref, ref_samples, 1);
            free(got.data);
        }
    }
}

static void put_synchsafe(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)((v >> 21) & 0x7f);
    p[1] = (uint8_t)((v >> 14) & 0x7f);
    p[2] = (uint8_t)((v >> 7) & 0x7f);
    p[3] = (uint8_t)(v & 0x7f);
}

/* synthetic ID3v2.4 tag with the footer flag set, prepended to a real stream */
static void run_id3v2_footer(const uint8_t *audio, size_t audio_size,
                             const mp3d_sample_t *ref, size_t ref_samples)
{
    static const size_t in_chunks[]  = { 1, 7, 113 };
    static const size_t out_chunks[] = { 2, 257 };
    unsigned body = 345;
    size_t total = 10 + body + 10 + audio_size;
    uint8_t *blob = (uint8_t *)calloc(1, total);
    size_t i, j;
    CHECK(blob != NULL, "out of memory");
    memcpy(blob, "ID3", 3);
    blob[3] = 4;          /* version 2.4 */
    blob[5] = 0x10;       /* footer flag */
    put_synchsafe(blob + 6, body);
    memcpy(blob + 10 + body, "3DI", 3);
    blob[10 + body + 3] = 4;
    blob[10 + body + 5] = 0x10;
    put_synchsafe(blob + 10 + body + 6, body);
    memcpy(blob + 10 + body + 10, audio, audio_size);

    for (i = 0; i < sizeof(in_chunks)/sizeof(in_chunks[0]); i++)
    {
        for (j = 0; j < sizeof(out_chunks)/sizeof(out_chunks[0]); j++)
        {
            pcm_vec got;
            size_t consumed = 0;
            char tag[96];
            vec_init(&got);
            CASE();
            snprintf(tag, sizeof(tag), "id3v2-footer in=%zu out=%zu", in_chunks[i], out_chunks[j]);
            int r = run_stream(blob, total, in_chunks[i], out_chunks[j], &got, &consumed);
            CHECK(r == MP3D_PUSH_DONE, "%s: status %d", tag, r);
            CHECK(consumed == total, "%s: consumption mismatch", tag);
            check_matches_reference(tag, &got, ref, ref_samples, 1);
            free(got.data);
        }
    }
    free(blob);
}

static void run_param_checks(void)
{
    mp3dec_push_t dec;
    uint8_t dummy[4] = { 0xff, 0xfb, 0x90, 0x64 };
    mp3d_sample_t pcm[8];
    size_t b = 1, s = 8;
    CASE();
    CHECK(mp3dec_push_init(NULL) == MP3D_PUSH_E_PARAM, "null init accepted");
    CHECK(mp3dec_push_init(&dec) == MP3D_PUSH_OK, "init failed");
    CHECK(mp3dec_push_decode(NULL, dummy, &b, pcm, &s, 0) == MP3D_PUSH_E_PARAM, "null dec accepted");
    b = 1; s = 8;
    CHECK(mp3dec_push_decode(&dec, NULL, NULL, pcm, &s, 0) == MP3D_PUSH_E_PARAM, "null bytes accepted");
    b = 1; s = 8;
    CHECK(mp3dec_push_decode(&dec, dummy, &b, NULL, NULL, 0) == MP3D_PUSH_E_PARAM, "null samples accepted");
    b = 1; s = 8;
    CHECK(mp3dec_push_decode(&dec, NULL, &b, pcm, &s, 0) == MP3D_PUSH_E_PARAM, "null input with bytes>0 accepted");
    b = 0; s = 8;
    CHECK(mp3dec_push_decode(&dec, NULL, &b, NULL, &s, 0) == MP3D_PUSH_E_PARAM, "null output with samples>0 accepted");
    mp3dec_push_close(&dec);
}

int main(void)
{
    size_t lame_size, sib_size, m2_size, id3v2_size, id3v1_size, free_size, tag_only_size;
    mp3dec_file_info_t ref_lame, ref_sib, ref_m2, ref_id3, ref_id3v1, ref_concat;

    /* bounded-memory guarantee: input buffer + pending PCM stay within 128 KiB */
    CHECK(sizeof(mp3dec_push_t) <= 128*1024, "instance storage exceeds 128 KiB");

    uint8_t *lame = load_file("vectors/l3-nonstandard-sin1k0db_lame_vbrtag.bit", &lame_size);
    uint8_t *sib  = load_file("vectors/l3-si_block.bit", &sib_size);
    uint8_t *m2   = load_file("vectors/M2L3_noise.bit", &m2_size);
    uint8_t *id3v2 = load_file("vectors/l3-nonstandard-id3v2.bit", &id3v2_size);
    uint8_t *id3v1 = load_file("vectors/l3-nonstandard-id3v1.bit", &id3v1_size);
    uint8_t *freef = load_file("vectors/l3-he_free.bit", &free_size);
    uint8_t *tagonly = load_file("vectors/l3-nonstandard-id3v2-only.bit", &tag_only_size);

    mp3d_sample_t *ref_lame_pcm = reference_decode(lame, lame_size, &ref_lame);
    mp3d_sample_t *ref_sib_pcm  = reference_decode(sib, sib_size, &ref_sib);
    mp3d_sample_t *ref_m2_pcm   = reference_decode(m2, m2_size, &ref_m2);
    mp3d_sample_t *ref_id3_pcm  = reference_decode(id3v2, id3v2_size, &ref_id3);
    mp3d_sample_t *ref_id3v1_pcm = reference_decode(id3v1, id3v1_size, &ref_id3v1);

    /* required vectors through every fragmentation combination */
    run_matrix("LAME-1kHz", lame, lame_size, ref_lame_pcm, ref_lame.samples, ref_lame.channels);
    run_matrix("l3-si_block", sib, sib_size, ref_sib_pcm, ref_sib.samples, ref_sib.channels);
    run_matrix("M2L3_noise", m2, m2_size, ref_m2_pcm, ref_m2.samples, ref_m2.channels);

    /* legal zero byte / zero sample capacity calls */
    run_zero_caps(sib, sib_size, ref_sib_pcm, ref_sib.samples);

    /* truncated tails and a cut inside the first frame */
    run_truncation(sib, sib_size, ref_sib_pcm, ref_sib.samples);

    /* fragmented ID3v2 tag, header split across fragments */
    run_id3v2(id3v2, id3v2_size, ref_id3_pcm, ref_id3.samples);

    /* synthetic ID3v2.4 tag carrying a footer */
    run_id3v2_footer(sib, sib_size, ref_sib_pcm, ref_sib.samples);

    /* large ID3v2 tag body streamed byte-wise in front of the LAME vector */
    {
        size_t concat_size = tag_only_size + lame_size;
        uint8_t *concat = (uint8_t *)malloc(concat_size);
        pcm_vec got;
        size_t consumed = 0;
        CHECK(concat != NULL, "out of memory");
        memcpy(concat, tagonly, tag_only_size);
        memcpy(concat + tag_only_size, lame, lame_size);
        mp3d_sample_t *ref_concat_pcm = reference_decode(concat, concat_size, &ref_concat);
        CHECK(ref_concat.samples == ref_lame.samples, "ID3-prefixed reference mismatch");
        vec_init(&got);
        CASE();
        int r = run_stream(concat, concat_size, 1, 257, &got, &consumed);
        CHECK(r == MP3D_PUSH_DONE, "id3-prefixed LAME status %d", r);
        CHECK(consumed == concat_size, "id3-prefixed LAME consumption mismatch");
        check_matches_reference("id3v2-only + LAME", &got, ref_concat_pcm, ref_concat.samples, 2);
        free(got.data);
        free(ref_concat.buffer);
        free(concat);
    }

    /* trailing ID3v1 tag skipped at eof */
    {
        pcm_vec got;
        size_t consumed = 0;
        vec_init(&got);
        CASE();
        int r = run_stream(id3v1, id3v1_size, 7, 14, &got, &consumed);
        CHECK(r == MP3D_PUSH_DONE, "id3v1 status %d", r);
        check_matches_reference("id3v1 tail", &got, ref_id3v1_pcm, ref_id3v1.samples, 1);
        free(got.data);
    }

    /* two instances interleaved */
    run_interleaved(lame, lame_size, ref_lame_pcm, ref_lame.samples, ref_lame.channels,
                    m2, m2_size, ref_m2_pcm, ref_m2.samples, ref_m2.channels);

    /* free-format is explicitly unsupported */
    {
        mp3dec_push_t dec;
        mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        size_t pos = 0, guard = 0;
        int got_format = 0;
        CHECK(mp3dec_push_init(&dec) == MP3D_PUSH_OK, "init failed");
        CASE();
        while (pos < free_size)
        {
            size_t ib = free_size - pos < 4096 ? free_size - pos : 4096;
            size_t ob = sizeof(pcm)/sizeof(pcm[0]);
            int eof = (pos + ib == free_size);
            int r = mp3dec_push_decode(&dec, freef + pos, &ib, pcm, &ob, eof);
            pos += ib;
            guard++;
            CHECK(guard < 100000, "free-format run did not terminate");
            if (r == MP3D_PUSH_E_FORMAT)
            {
                got_format = 1;
                break;
            }
            CHECK(r >= 0, "free-format: unexpected error %d", r);
        }
        CHECK(got_format, "free-format stream was not rejected");
        CHECK(pos > 0, "free-format: no input accepted before rejection");
        mp3dec_push_close(&dec);
    }

    /* cut ID3v2 header: unfinished tag is a truncation error */
    {
        static const uint8_t partial_tag[5] = { 'I', 'D', '3', 3, 0 };
        mp3dec_push_t dec;
        size_t ib = sizeof(partial_tag), ob = 16;
        mp3d_sample_t pcm[16];
        CHECK(mp3dec_push_init(&dec) == MP3D_PUSH_OK, "init failed");
        CASE();
        int r = mp3dec_push_decode(&dec, partial_tag, &ib, pcm, &ob, 1);
        CHECK(r == MP3D_PUSH_E_TRUNC, "partial ID3 header: got %d", r);
        CHECK(ib == sizeof(partial_tag), "partial ID3 header: accepted bytes not reported");
        mp3dec_push_close(&dec);
    }

    run_param_checks();

    free(ref_lame_pcm);
    free(ref_sib_pcm);
    free(ref_m2_pcm);
    free(ref_id3_pcm);
    free(ref_id3v1_pcm);
    free(lame);
    free(sib);
    free(m2);
    free(id3v2);
    free(id3v1);
    free(freef);
    free(tagonly);

    printf("MINIMP3_PUSH_OK cases=%d\n", g_cases);
    return 0;
}
