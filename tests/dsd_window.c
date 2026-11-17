/*
 * tests/dsd_window.c
 *
 * Streaming DSD sample-window tests for the production DSF and DSDIFF
 * handlers. Each channel gets an independently constructed MSB-first bit
 * sequence; expected values are extracted bit by bit and the produced
 * container blocks are walked independently.
 *
 * Registered through task_support/check.h; task_support/driver.c provides
 * main().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../task_support/check.h"
#include "scarletbook_output.h"
#include "scarletbook.h"
#include "dsf.h"
#include "dsdiff.h"
#include "dsd_window.h"

extern const scarletbook_format_handler_t *dsf_format_fn(void);
extern const scarletbook_format_handler_t *dsdiff_format_fn(void);
extern const scarletbook_format_handler_t *dsdiff_edit_master_format_fn(void);

#define SAMPLE_RATE 2822400u
#define BLOCK 4096u

/* ------------------------------------------------------------------ */
/* small byte helpers                                                  */
/* ------------------------------------------------------------------ */

static uint64_t le64(const uint8_t *p)
{
    uint64_t n = 0;
    int i;
    for (i = 7; i >= 0; i--)
        n = (n << 8) | p[i];
    return n;
}

static uint32_t le32(const uint8_t *p)
{
    return ((uint32_t) p[3] << 24) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[1] << 8) | (uint32_t) p[0];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static uint64_t be64(const uint8_t *p)
{
    uint64_t n = 0;
    int i;
    for (i = 0; i < 8; i++)
        n = (n << 8) | p[i];
    return n;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t) ((p[0] << 8) | p[1]);
}

static uint8_t reverse_byte(uint8_t n)
{
    uint8_t r = 0;
    int i;
    for (i = 0; i < 8; i++)
    {
        r = (uint8_t) ((r << 1) | (n & 1));
        n >>= 1;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* hand-built DSD source: distinct, channel-specific bit sequences     */
/* ------------------------------------------------------------------ */

/* Bit (g*8+b) of channel c in the interleaved MSB-first stream. */
static int source_bit(const uint8_t *input, int channels, int channel, uint64_t bit_index)
{
    uint64_t group = bit_index / 8;
    int in_byte = (int) (bit_index % 8);
    uint64_t byte_offset = group * (uint64_t) channels + channel;
    return (input[byte_offset] >> (7 - in_byte)) & 1;
}

/* Expected selected output byte (MSB-first DSD ordering) for one channel;
   bits beyond the window read as zero, matching the required tail fill. */
static uint8_t expected_msb_byte(const uint8_t *input, int channels, int channel,
                                 uint64_t begin, uint64_t end, uint64_t output_byte)
{
    uint8_t out = 0;
    int b;
    for (b = 0; b < 8; b++)
    {
        uint64_t bit = begin + output_byte * 8 + (uint64_t) b;
        int value = (bit < end) ? source_bit(input, channels, channel, bit) : 0;
        out = (uint8_t) ((out << 1) | value);
    }
    return out;
}

static uint8_t *make_input(int channels, uint64_t groups)
{
    uint8_t *input = malloc((size_t) (groups * channels));
    uint64_t g;
    int c;
    if (!input)
        return NULL;
    for (g = 0; g < groups; g++)
        for (c = 0; c < channels; c++)
            input[g * channels + c] =
                (uint8_t) ((g * 131u + c * 71u + 17u) ^ (g >> 3) ^ (c * 0x55));
    return input;
}

/* ------------------------------------------------------------------ */
/* handler scaffolding                                                 */
/* ------------------------------------------------------------------ */

typedef struct
{
    scarletbook_handle_t sb;
    area_toc_t toc;
    master_toc_t master;
    area_tracklist_t times;
} test_env_t;

static void env_init(test_env_t *env, int channels)
{
    memset(env, 0, sizeof(*env));
    env->toc.channel_count = (uint8_t) channels;
    env->toc.track_count = 1;
    env->toc.extra_settings = (channels == 5) ? 3 : (channels == 6) ? 4 : 0;
    env->sb.area[0].area_toc = &env->toc;
    env->sb.area[0].area_tracklist_time = &env->times;
    env->sb.master_toc = &env->master;
    env->sb.id3_tag_mode = 2;
}

typedef struct
{
    scarletbook_output_format_t ft;
    const scarletbook_format_handler_t *handler;
} test_output_t;

static int output_init(test_output_t *out, test_env_t *env, int dsf,
                       int window, uint64_t begin, uint64_t end)
{
    memset(out, 0, sizeof(*out));
    out->handler = dsf ? dsf_format_fn() : dsdiff_format_fn();
    out->ft.sb_handle = &env->sb;
    out->ft.fd = tmpfile();
    out->ft.filename = dsf ? "test.dsf" : "test.dff";
    out->ft.dsd_encoded_export = 1;
    out->ft.handler = *out->handler;
    out->ft.priv = calloc(1, out->handler->priv_size);
    out->ft.sample_window_enabled = window;
    out->ft.sample_window_begin = begin;
    out->ft.sample_window_end = end;
    if (!out->ft.fd || !out->ft.priv)
        return -1;
    return 0;
}

static void output_free(test_output_t *out)
{
    if (out->ft.priv)
        free(out->ft.priv);
    if (out->ft.fd)
        fclose(out->ft.fd);
    out->ft.priv = NULL;
    out->ft.fd = NULL;
}

static uint8_t *slurp(FILE *fd, long *size_out)
{
    long size;
    uint8_t *buf;
    if (fseek(fd, 0, SEEK_END) != 0)
        return NULL;
    size = ftell(fd);
    if (size <= 0)
        return NULL;
    buf = malloc((size_t) size);
    if (!buf)
        return NULL;
    rewind(fd);
    if (fread(buf, 1, (size_t) size, fd) != (size_t) size)
    {
        free(buf);
        return NULL;
    }
    *size_out = size;
    return buf;
}

/* Deterministic fragmentation patterns; pattern 3 deliberately splits
   channel groups in the middle. Returns cumulative written bytes or -1. */
static int feed_pattern(test_output_t *out, const uint8_t *input, size_t len,
                        int channels, int pattern)
{
    static const size_t cycle[] = {1, 3, 2, 7, 1, 4, 2, 5};
    size_t pos = 0;
    size_t idx = 0;
    int total = 0;

    if (pattern == 0)
        return out->ft.handler.write(&out->ft, input, len);

    while (pos < len)
    {
        size_t chunk;
        int r;
        if (pattern == 1)
            chunk = 1;
        else if (pattern == 3)
            chunk = (idx % 3 == 0) ? (size_t) (channels - 1)
                                  : (idx % 3 == 1) ? 1u : (size_t) (channels + 2);
        else
            chunk = cycle[idx % (sizeof(cycle) / sizeof(cycle[0]))];
        if (chunk > len - pos)
            chunk = len - pos;
        /* Pattern 2 injects zero-length calls, which must not advance state. */
        if (pattern == 2 && (idx % 2 == 1))
        {
            r = out->ft.handler.write(&out->ft, input + pos, 0);
            if (r != 0)
                return -1;
        }
        r = out->ft.handler.write(&out->ft, input + pos, chunk);
        if (r < 0)
            return -1;
        total += r;
        pos += chunk;
        idx++;
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* DSF verification                                                    */
/* ------------------------------------------------------------------ */

static int verify_dsf(const uint8_t *b, long size, int channels,
                      const uint8_t *input, uint64_t begin, uint64_t end)
{
    uint64_t window_bits = end - begin;
    uint64_t bytes_per_channel = (window_bits + 7) / 8;
    uint64_t blocks = (bytes_per_channel + BLOCK - 1) / BLOCK;
    uint64_t audio_bytes = blocks * BLOCK * (uint64_t) channels;
    uint64_t audio_start = 92;
    int c;

    if (memcmp(b, "DSD ", 4) != 0) return -1;
    if (le64(b + 12) != (uint64_t) size) return -1;              /* total file size */
    if (le32(b + 52) != (uint32_t) channels) return -1;
    if (le64(b + 20) != audio_start + audio_bytes) return -1;    /* ID3 pointer */
    if (le64(b + 64) != window_bits) return -1;                  /* fmt sample_count */
    if (le32(b + 72) != BLOCK) return -1;                        /* block size per channel */
    if (le64(b + 84) != 12 + audio_bytes) return -1;             /* data chunk length */

    /* DSF stores fixed blocks interleaved per channel: block k of every
       channel, then block k+1 - matching the production write order. */
    for (c = 0; c < channels; c++)
    {
        uint64_t j;
        for (j = 0; j < blocks * BLOCK; j++)
        {
            uint8_t expected;
            uint64_t block = j / BLOCK;
            uint64_t within = j % BLOCK;
            uint64_t off = audio_start + (block * (uint64_t) channels + c) * BLOCK + within;
            if (off >= (uint64_t) size) return -1;
            if (j < bytes_per_channel)
                expected = reverse_byte(expected_msb_byte(input, channels, c, begin, end, j));
            else
                expected = 0;                                    /* block remainder zeroed */
            if (b[off] != expected) return -1;
        }
    }
    if (memcmp(b + audio_start + audio_bytes, "ID3", 3) != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* DSDIFF verification                                                  */
/* ------------------------------------------------------------------ */

static int verify_dsdiff(const uint8_t *b, long size, int channels,
                         const uint8_t *input, uint64_t begin, uint64_t end)
{
    uint64_t window_bits = end - begin;
    uint64_t bytes_per_channel = window_bits / 8;
    uint64_t audio_bytes = bytes_per_channel * (uint64_t) channels;
    uint64_t p = 16;
    int found_dsd = 0, found_diin = 0, found_mark = 0;

    if (memcmp(b, "FRM8", 4) != 0) return -1;
    if (be64(b + 4) != (uint64_t) size - 12) return -1;

    while (p + 12 <= (uint64_t) size)
    {
        uint32_t id = be32(b + p);
        uint64_t n = be64(b + p + 4);
        if (n > (uint64_t) size - p - 12) return -1;

        if (id == be32((const uint8_t *) "DSD "))
        {
            uint64_t j;
            found_dsd = 1;
            if (n != audio_bytes) return -1;                     /* audio only, no pad byte */
            for (j = 0; j < audio_bytes; j++)
            {
                int c = (int) (j % channels);
                uint64_t out_byte = j / channels;
                if (b[p + 12 + j] !=
                    expected_msb_byte(input, channels, c, begin, end, out_byte))
                    return -1;
            }
        }

        if (id == be32((const uint8_t *) "DIIN"))
        {
            uint64_t q = p + 12, diin_end = p + 12 + n;
            found_diin = 1;
            while (q + 12 <= diin_end)
            {
                uint32_t sub = be32(b + q);
                uint64_t sub_n = be64(b + q + 4);
                if (sub == be32((const uint8_t *) "MARK"))
                {
                    uint64_t secs = window_bits / SAMPLE_RATE;
                    found_mark = 1;
                    if (be16(b + q + 12) != (uint16_t) (secs / 3600)) return -1;
                    if (b[q + 14] != (uint8_t) ((secs % 3600) / 60)) return -1;
                    if (b[q + 15] != (uint8_t) (secs % 60)) return -1;
                    if (be32(b + q + 16) != (uint32_t) (window_bits % SAMPLE_RATE)) return -1;
                    if (be16(b + q + 24) != 4) return -1;         /* INDEX_ENTRY */
                }
                q += 12 + sub_n + (sub_n & 1);
            }
        }

        p += 12 + n + (n & 1);
    }
    if (p != (uint64_t) size) return -1;
    if (!(found_dsd && found_diin && found_mark)) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* full-run helper                                                      */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint8_t *data;
    long size;
} blob_t;

/* Runs one windowed output and validates it. *blob is zeroed on failure. */
static int run_window(int dsf, int channels, uint64_t begin, uint64_t end,
                      int pattern, uint64_t extra_groups, blob_t *blob)
{
    test_env_t env;
    test_output_t out;
    uint64_t groups = end / 8 + 1 + extra_groups;
    uint8_t *input;
    int rc = -1;

    memset(blob, 0, sizeof(*blob));
    env_init(&env, channels);
    if (output_init(&out, &env, dsf, 1, begin, end) != 0)
        return -1;
    if (out.ft.handler.startwrite(&out.ft) != 0)
        goto cleanup;

    input = make_input(channels, groups);
    if (!input)
        goto cleanup;
    if (feed_pattern(&out, input, (size_t) (groups * channels), channels, pattern) < 0)
    {
        free(input);
        goto cleanup;
    }
    if (out.ft.handler.stopwrite(&out.ft) != 0)
    {
        free(input);
        goto cleanup;
    }

    blob->data = slurp(out.ft.fd, &blob->size);
    if (!blob->data)
    {
        free(input);
        goto cleanup;
    }
    if (dsf)
        rc = verify_dsf(blob->data, blob->size, channels, input, begin, end);
    else
        rc = verify_dsdiff(blob->data, blob->size, channels, input, begin, end);
    free(input);

cleanup:
    output_free(&out);
    if (rc != 0)
    {
        free(blob->data);
        memset(blob, 0, sizeof(*blob));
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* bits group: per-bit extraction correctness                           */
/* ------------------------------------------------------------------ */

static int case_dsf_nonbyte_start(void)
{
    blob_t blob;
    TASK_CHECK(run_window(1, 2, 3, 20, 0, 0, &blob) == 0);  /* 17 bits, partial last byte */
    TASK_CHECK(le64(blob.data + 64) == 17);
    free(blob.data);
    return 0;
}

static int case_dsf_subbyte_tail(void)
{
    blob_t blob;
    TASK_CHECK(run_window(1, 2, 0, 5, 0, 0, &blob) == 0);   /* only five bits per channel */
    TASK_CHECK(le64(blob.data + 64) == 5);
    TASK_CHECK((blob.data[92] & 0x07) == 0);                /* unused high bits zero */
    free(blob.data);
    return 0;
}

static int case_dsf_byte_aligned(void)
{
    blob_t blob;
    TASK_CHECK(run_window(1, 2, 8, 24, 0, 0, &blob) == 0);
    TASK_CHECK(le64(blob.data + 64) == 16);
    free(blob.data);
    return 0;
}

static int case_dsdiff_extract_bits(void)
{
    blob_t blob;
    TASK_CHECK(run_window(0, 2, 5, 29, 0, 0, &blob) == 0);  /* 24 bits = 3 bytes/channel */
    free(blob.data);
    return 0;
}

static int case_five_channel_distinct(void)
{
    blob_t blob;
    TASK_CHECK(run_window(1, 5, 11, 43, 0, 0, &blob) == 0); /* 32 bits, odd channel count */
    TASK_CHECK(le64(blob.data + 64) == 32);
    free(blob.data);
    return 0;
}

static int case_six_channel(void)
{
    blob_t dsf_blob, dff_blob;
    TASK_CHECK(run_window(1, 6, 7, 55, 0, 0, &dsf_blob) == 0);
    TASK_CHECK(run_window(0, 6, 8, 56, 0, 0, &dff_blob) == 0);
    free(dsf_blob.data);
    free(dff_blob.data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* containers group: blocks, lengths, padding, markers                  */
/* ------------------------------------------------------------------ */

static int case_dsf_cross_4096(void)
{
    /* 4098 output bytes/channel: spans a block boundary and leaves a
       partial final byte plus zeroed block remainder. */
    blob_t blob;
    uint64_t audio = 2 * 2 * BLOCK;
    TASK_CHECK(run_window(1, 2, 1, 1 + 4098 * 8, 0, 0, &blob) == 0);
    TASK_CHECK(le64(blob.data + 84) == 12 + audio);
    TASK_CHECK(le64(blob.data + 20) == 92 + audio);
    TASK_CHECK(le64(blob.data + 12) == (uint64_t) blob.size);
    free(blob.data);
    return 0;
}

static int case_dsf_five_channel_odd_payload(void)
{
    blob_t blob;
    TASK_CHECK(run_window(1, 5, 2, 15, 0, 0, &blob) == 0);  /* 13 bits/channel, 5 blocks */
    TASK_CHECK(le64(blob.data + 84) == 12 + BLOCK * 5);
    free(blob.data);
    return 0;
}

static int case_dsdiff_odd_audio_pad(void)
{
    /* One byte per channel * 5 channels = 5 audio bytes (odd): a pad byte
       must exist on disk but stay out of the DSD chunk length. */
    blob_t blob;
    TASK_CHECK(run_window(0, 5, 0, 8, 0, 0, &blob) == 0);
    TASK_CHECK(be64(blob.data + 4) == (uint64_t) blob.size - 12);
    free(blob.data);
    return 0;
}

static int case_dsdiff_marker_recalc(void)
{
    /* Crosses one second; the marker uses the output count and keeps the
       exact sub-second DSD residue (no 75 Hz frame rounding). The length
       stays byte aligned, as DSDIFF requires. */
    uint64_t begin = 100, length = (SAMPLE_RATE + 12344) & ~(uint64_t) 7;
    blob_t blob;
    TASK_CHECK(run_window(0, 2, begin, begin + length, 0, 0, &blob) == 0);
    free(blob.data);
    return 0;
}

static int case_fragmentation_equivalence(void)
{
    /* Non-byte-aligned begin; the length stays byte aligned (and spans a
       block boundary) so both containers accept the same window. */
    const uint64_t begin = 9, end = begin + BLOCK * 8 + 32;
    int dsf, pattern;

    for (dsf = 1; dsf >= 0; dsf--)
    {
        int channels = dsf ? 2 : 5;
        blob_t reference, fragmented;
        TASK_CHECK(run_window(dsf, channels, begin, end, 0, 0, &reference) == 0);
        for (pattern = 1; pattern <= 3; pattern++)
        {
            TASK_CHECK(run_window(dsf, channels, begin, end, pattern, 0, &fragmented) == 0);
            TASK_CHECK(fragmented.size == reference.size);
            TASK_CHECK(memcmp(fragmented.data, reference.data,
                              (size_t) fragmented.size) == 0);
            free(fragmented.data);
        }
        free(reference.data);
    }
    return 0;
}

static int case_data_after_window_ignored(void)
{
    blob_t exact, extra;
    TASK_CHECK(run_window(1, 2, 4, 36, 0, 0, &exact) == 0);
    TASK_CHECK(run_window(1, 2, 4, 36, 0, 4, &extra) == 0);
    TASK_CHECK(extra.size == exact.size);
    TASK_CHECK(memcmp(extra.data, exact.data, (size_t) exact.size) == 0);
    free(exact.data);
    free(extra.data);
    return 0;
}

static int case_dsf_full_block_plus_partial_tail(void)
{
    /* 4096 full output bytes exactly fill one block per channel, then a
       partial tail byte: the tail must flush a fresh block, not overflow. */
    blob_t blob;
    TASK_CHECK(run_window(1, 2, 0, 4096 * 8 + 3, 0, 0, &blob) == 0);
    TASK_CHECK(le64(blob.data + 64) == 4096 * 8 + 3);
    TASK_CHECK(le64(blob.data + 84) == 12 + 2 * 2 * BLOCK);
    free(blob.data);
    return 0;
}

static int case_dsdiff_flush_buffer_boundary(void)
{
    /* Output exactly fills the fixed 4096-byte flush buffer (2048 bytes per
       channel) and then continues across it. */
    blob_t blob;
    TASK_CHECK(run_window(0, 2, 0, 2048 * 8 + 16, 0, 0, &blob) == 0);
    free(blob.data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* state group: validation, short input, handle isolation               */
/* ------------------------------------------------------------------ */

static int case_short_input_dsf(void)
{
    test_env_t env;
    test_output_t out;
    uint8_t input[8] = {0xaa, 0x55, 0x12, 0x34, 0x78, 0x9a, 0xde, 0xf0};

    env_init(&env, 2);
    TASK_CHECK(output_init(&out, &env, 1, 1, 0, 64) == 0);
    TASK_CHECK(out.ft.handler.startwrite(&out.ft) == 0);
    TASK_CHECK(out.ft.handler.write(&out.ft, input, sizeof(input)) >= 0);
    TASK_CHECK(out.ft.handler.stopwrite(&out.ft) < 0);   /* window never completed */
    output_free(&out);
    return 0;
}

static int case_short_input_dsdiff(void)
{
    test_env_t env;
    test_output_t out;
    uint8_t input[4] = {1, 2, 3, 4};

    env_init(&env, 2);
    TASK_CHECK(output_init(&out, &env, 0, 1, 0, 32) == 0);
    TASK_CHECK(out.ft.handler.startwrite(&out.ft) == 0);
    TASK_CHECK(out.ft.handler.write(&out.ft, input, sizeof(input)) >= 0);
    TASK_CHECK(out.ft.handler.stopwrite(&out.ft) < 0);
    output_free(&out);
    return 0;
}

static int case_incomplete_group(void)
{
    test_env_t env;
    test_output_t out;
    uint8_t input[6] = {1, 2, 3, 4, 5, 6};

    env_init(&env, 5);
    TASK_CHECK(output_init(&out, &env, 1, 1, 0, 24) == 0);
    TASK_CHECK(out.ft.handler.startwrite(&out.ft) == 0);
    TASK_CHECK(out.ft.handler.write(&out.ft, input, 5) >= 0);       /* one full group */
    TASK_CHECK(out.ft.handler.write(&out.ft, input + 5, 1) >= 0);   /* partial next */
    TASK_CHECK(out.ft.handler.stopwrite(&out.ft) < 0);
    output_free(&out);
    return 0;
}

static int case_reject_begin_ge_end(void)
{
    test_env_t env;
    test_output_t dsf_out, dff_out;

    env_init(&env, 2);
    TASK_CHECK(output_init(&dsf_out, &env, 1, 1, 100, 100) == 0);
    TASK_CHECK(output_init(&dff_out, &env, 0, 1, 100, 99) == 0);
    TASK_CHECK(dsf_out.ft.handler.startwrite(&dsf_out.ft) < 0);
    TASK_CHECK(dff_out.ft.handler.startwrite(&dff_out.ft) < 0);
    TASK_CHECK(ftell(dsf_out.ft.fd) == 0);                          /* no header written */
    TASK_CHECK(ftell(dff_out.ft.fd) == 0);
    output_free(&dsf_out);
    output_free(&dff_out);
    return 0;
}

static int case_reject_bad_channels(void)
{
    test_env_t env;
    test_output_t dsf_out, dff_out;

    env_init(&env, 3);
    TASK_CHECK(output_init(&dsf_out, &env, 1, 1, 0, 8) == 0);
    TASK_CHECK(output_init(&dff_out, &env, 0, 1, 0, 8) == 0);
    TASK_CHECK(dsf_out.ft.handler.startwrite(&dsf_out.ft) < 0);
    TASK_CHECK(dff_out.ft.handler.startwrite(&dff_out.ft) < 0);
    TASK_CHECK(ftell(dsf_out.ft.fd) == 0);
    output_free(&dsf_out);
    output_free(&dff_out);
    return 0;
}

static int case_reject_nopad(void)
{
    test_env_t env;
    test_output_t out;

    env_init(&env, 2);
    env.sb.dsf_nopad = 1;
    TASK_CHECK(output_init(&out, &env, 1, 1, 0, 8) == 0);
    TASK_CHECK(out.ft.handler.startwrite(&out.ft) < 0);
    TASK_CHECK(ftell(out.ft.fd) == 0);
    output_free(&out);
    return 0;
}

static int case_reject_dst(void)
{
    test_env_t env;
    test_output_t out;

    env_init(&env, 2);
    TASK_CHECK(output_init(&out, &env, 0, 1, 0, 8) == 0);
    out.ft.dsd_encoded_export = 0;                                  /* DST framing */
    TASK_CHECK(out.ft.handler.startwrite(&out.ft) < 0);
    TASK_CHECK(ftell(out.ft.fd) == 0);
    output_free(&out);
    return 0;
}

static int case_reject_edit_master(void)
{
    test_env_t env;
    test_output_t out;

    env_init(&env, 2);
    memset(&out, 0, sizeof(out));
    out.handler = dsdiff_edit_master_format_fn();
    out.ft.sb_handle = &env.sb;
    out.ft.fd = tmpfile();
    out.ft.filename = "edit.dff";
    out.ft.dsd_encoded_export = 1;
    out.ft.handler = *out.handler;
    out.ft.priv = calloc(1, out.handler->priv_size);
    out.ft.sample_window_enabled = 1;
    out.ft.sample_window_begin = 0;
    out.ft.sample_window_end = 8;
    TASK_CHECK(out.ft.fd && out.ft.priv);
    TASK_CHECK(out.ft.handler.startwrite(&out.ft) < 0);
    TASK_CHECK(ftell(out.ft.fd) == 0);
    output_free(&out);
    return 0;
}

static int case_reject_dsdiff_nonbyte(void)
{
    test_env_t env;
    test_output_t out;

    env_init(&env, 2);
    TASK_CHECK(output_init(&out, &env, 0, 1, 0, 13) == 0);         /* 13 bits, not /8 */
    TASK_CHECK(out.ft.handler.startwrite(&out.ft) < 0);
    TASK_CHECK(ftell(out.ft.fd) == 0);
    output_free(&out);
    return 0;
}

static int case_interleaved_handles(void)
{
    test_env_t env;
    test_output_t dsf_out, dff_out;
    const uint64_t dsf_begin = 3, dsf_end = 101;
    const uint64_t dff_begin = 11, dff_end = 75;
    uint64_t groups = dsf_end / 8 + 2;
    uint8_t *input;
    size_t total;
    blob_t dsf_blob, dff_blob;
    long dsf_size, dff_size;

    env_init(&env, 2);
    TASK_CHECK(output_init(&dsf_out, &env, 1, 1, dsf_begin, dsf_end) == 0);
    TASK_CHECK(output_init(&dff_out, &env, 0, 1, dff_begin, dff_end) == 0);
    TASK_CHECK(dsf_out.ft.handler.startwrite(&dsf_out.ft) == 0);
    TASK_CHECK(dff_out.ft.handler.startwrite(&dff_out.ft) == 0);

    input = make_input(2, groups);
    TASK_CHECK(input != NULL);
    total = (size_t) (groups * 2);
    /* Each handle consumes the same complete stream, but through a different
       chunk schedule; calls alternate so state must stay per handle. */
    {
        size_t dsf_pos = 0, dff_pos = 0;
        while (dsf_pos < total || dff_pos < total)
        {
            size_t c1 = 3, c2 = 5;
            if (dsf_pos < total)
            {
                if (c1 > total - dsf_pos) c1 = total - dsf_pos;
                TASK_CHECK(dsf_out.ft.handler.write(&dsf_out.ft, input + dsf_pos, c1) >= 0);
                dsf_pos += c1;
            }
            if (dff_pos < total)
            {
                if (c2 > total - dff_pos) c2 = total - dff_pos;
                TASK_CHECK(dff_out.ft.handler.write(&dff_out.ft, input + dff_pos, c2) >= 0);
                dff_pos += c2;
            }
        }
    }
    TASK_CHECK(dsf_out.ft.handler.stopwrite(&dsf_out.ft) == 0);
    TASK_CHECK(dff_out.ft.handler.stopwrite(&dff_out.ft) == 0);

    dsf_blob.data = slurp(dsf_out.ft.fd, &dsf_size);
    dff_blob.data = slurp(dff_out.ft.fd, &dff_size);
    TASK_CHECK(dsf_blob.data && dff_blob.data);
    dsf_blob.size = dsf_size;
    dff_blob.size = dff_size;
    TASK_CHECK(verify_dsf(dsf_blob.data, dsf_size, 2, input, dsf_begin, dsf_end) == 0);
    TASK_CHECK(verify_dsdiff(dff_blob.data, dff_size, 2, input, dff_begin, dff_end) == 0);

    free(dsf_blob.data);
    free(dff_blob.data);
    free(input);
    output_free(&dsf_out);
    output_free(&dff_out);
    return 0;
}

static int case_zero_length_no_advance(void)
{
    blob_t plain, zeroed;
    TASK_CHECK(run_window(0, 2, 6, 38, 0, 0, &plain) == 0);
    TASK_CHECK(run_window(0, 2, 6, 38, 2, 0, &zeroed) == 0);     /* pattern 2 injects len-0 */
    TASK_CHECK(zeroed.size == plain.size);
    TASK_CHECK(memcmp(zeroed.data, plain.data, (size_t) plain.size) == 0);
    free(plain.data);
    free(zeroed.data);
    return 0;
}

static int case_window_disabled_full_track(void)
{
    /* The original behaviour must be preserved when no window is requested. */
    test_env_t env;
    test_output_t out;
    uint8_t input[16];
    unsigned i;
    long size;
    uint8_t *b;

    for (i = 0; i < sizeof(input); i++)
        input[i] = (uint8_t) (i * 29 + 7);
    env_init(&env, 2);
    TASK_CHECK(output_init(&out, &env, 1, 0, 0, 0) == 0);
    TASK_CHECK(out.ft.handler.startwrite(&out.ft) == 0);
    TASK_CHECK(out.ft.handler.write(&out.ft, input, sizeof(input)) >= 0);
    TASK_CHECK(out.ft.handler.stopwrite(&out.ft) == 0);
    b = slurp(out.ft.fd, &size);
    TASK_CHECK(b != NULL);
    TASK_CHECK(le64(b + 64) == 64);
    TASK_CHECK(le64(b + 84) == 8204);
    free(b);
    output_free(&out);
    return 0;
}

const struct task_case task_cases[] =
{
    /* bits */
    {"bits", "dsf_nonbyte_start",      case_dsf_nonbyte_start},
    {"bits", "dsf_subbyte_tail",       case_dsf_subbyte_tail},
    {"bits", "dsf_byte_aligned",       case_dsf_byte_aligned},
    {"bits", "dsdiff_extract_bits",    case_dsdiff_extract_bits},
    {"bits", "five_channel_distinct",  case_five_channel_distinct},
    {"bits", "six_channel",            case_six_channel},

    /* containers */
    {"containers", "dsf_cross_4096",            case_dsf_cross_4096},
    {"containers", "dsf_five_channel_odd",      case_dsf_five_channel_odd_payload},
    {"containers", "dsdiff_odd_audio_pad",      case_dsdiff_odd_audio_pad},
    {"containers", "dsdiff_marker_recalc",      case_dsdiff_marker_recalc},
    {"containers", "fragmentation_equivalence", case_fragmentation_equivalence},
    {"containers", "data_after_window_ignored", case_data_after_window_ignored},
    {"containers", "dsf_full_block_partial_tail", case_dsf_full_block_plus_partial_tail},
    {"containers", "dsdiff_flush_buffer_boundary", case_dsdiff_flush_buffer_boundary},

    /* state */
    {"state", "short_input_dsf",        case_short_input_dsf},
    {"state", "short_input_dsdiff",     case_short_input_dsdiff},
    {"state", "incomplete_group",       case_incomplete_group},
    {"state", "reject_begin_ge_end",    case_reject_begin_ge_end},
    {"state", "reject_bad_channels",    case_reject_bad_channels},
    {"state", "reject_nopad",           case_reject_nopad},
    {"state", "reject_dst",             case_reject_dst},
    {"state", "reject_edit_master",     case_reject_edit_master},
    {"state", "reject_dsdiff_nonbyte",  case_reject_dsdiff_nonbyte},
    {"state", "interleaved_handles",    case_interleaved_handles},
    {"state", "zero_length_no_advance", case_zero_length_no_advance},
    {"state", "window_disabled_track",  case_window_disabled_full_track},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
