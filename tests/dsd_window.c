/*
 * Sample-window tests for the DSF / DSDIFF production handlers.
 *
 * The source DSD is built independently per channel as MSB-first, one byte per
 * channel in clustered-frame order. Expected window bits are extracted straight
 * from that bit sequence and compared against the parsed container output; the
 * output blocks are walked independently of the production buffering logic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../task_support/check.h"
#include "scarletbook_output.h"
#include "dsf.h"
#include "dsdiff.h"
#include "dsd_window.h"

extern const scarletbook_format_handler_t *dsf_format_fn(void);
extern const scarletbook_format_handler_t *dsdiff_format_fn(void);
extern const scarletbook_format_handler_t *dsdiff_edit_master_format_fn(void);

#define BLOCK 4096
#define DSF_HEADER_SIZE 92

/* ----------------------------- small helpers ----------------------------- */

static uint64_t rd_le64(const uint8_t *p)
{
    uint64_t n = 0;
    int i;
    for (i = 7; i >= 0; i--)
        n = (n << 8) | p[i];
    return n;
}

static uint32_t rd_le32(const uint8_t *p)
{
    uint32_t n = 0;
    int i;
    for (i = 3; i >= 0; i--)
        n = (n << 8) | p[i];
    return n;
}

static uint64_t rd_be64(const uint8_t *p)
{
    uint64_t n = 0;
    int i;
    for (i = 0; i < 8; i++)
        n = (n << 8) | p[i];
    return n;
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
         | ((uint32_t) p[2] << 8)  |  (uint32_t) p[3];
}

static uint16_t rd_be16(const uint8_t *p)
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
        n = (uint8_t) (n >> 1);
    }
    return r;
}

/* Distinct, channel-sensitive source stream: clustered frame byte k. */
static uint8_t source_byte(size_t k, int channels)
{
    int channel = (int) (k % (size_t) channels);
    return (uint8_t) ((k * 37u + (size_t) channel * 0x55u + 0xA5u) & 0xFFu);
}

static void make_source(uint8_t *buf, size_t frames, int channels)
{
    size_t k;
    for (k = 0; k < frames * (size_t) channels; k++)
        buf[k] = source_byte(k, channels);
}

/* Bit sample `sample` of channel `channel`, taken straight from the source. */
static int src_bit(const uint8_t *src, int channels, uint64_t sample, int channel)
{
    size_t byte_index = (size_t) (sample / 8) * (size_t) channels + (size_t) channel;
    return (src[byte_index] >> (7 - (sample % 8))) & 1;
}

/* Expected window output byte, MSB-first, for channel / window byte index. */
static uint8_t expected_msb(const uint8_t *src, int channels,
                            uint64_t begin, uint64_t window_len,
                            int channel, uint64_t byte_index)
{
    uint8_t v = 0;
    int b;
    for (b = 0; b < 8; b++)
    {
        uint64_t w = byte_index * 8 + (uint64_t) b;
        if (w < window_len && src_bit(src, channels, begin + w, channel))
            v = (uint8_t) (v | (1u << (7 - b)));
    }
    return v;
}

/* ------------------------------ fixture setup ----------------------------- */

struct fixture
{
    scarletbook_handle_t  sb;
    area_toc_t            toc;
    master_toc_t          master;
    area_tracklist_t      times;
};

static void fixture_init(struct fixture *fx, int channels, int extra_settings)
{
    memset(fx, 0, sizeof(*fx));
    fx->toc.channel_count = (uint8_t) channels;
    fx->toc.track_count = 1;
    fx->toc.extra_settings = (uint8_t) extra_settings;
    fx->sb.area[0].area_toc = &fx->toc;
    fx->sb.area[0].area_tracklist_time = &fx->times;
    fx->sb.master_toc = &fx->master;
    fx->sb.id3_tag_mode = 2;
}

struct out
{
    scarletbook_output_format_t ft;
    uint8_t *file_bytes;
    long     file_size;
};

static int open_out(struct fixture *fx, const scarletbook_format_handler_t *handler,
                    struct out *o, uint64_t begin, uint64_t end, int window_on,
                    int dsd_export, int nopad)
{
    memset(o, 0, sizeof(*o));
    o->ft.sb_handle = &fx->sb;
    o->ft.area = 0;
    o->ft.track = 0;
    o->ft.filename = "window-test";
    o->ft.dsd_encoded_export = dsd_export;
    o->ft.handler = *handler;
    o->ft.priv = calloc(1, handler->priv_size);
    o->ft.fd = tmpfile();
    if (!o->ft.priv || !o->ft.fd)
        return -1;
    if (window_on)
    {
        o->ft.sample_window_enabled = 1;
        o->ft.sample_window_begin = begin;
        o->ft.sample_window_end = end;
    }
    fx->sb.dsf_nopad = nopad;
    return o->ft.handler.startwrite(&o->ft);
}

static int slurp(struct out *o)
{
    if (fseek(o->ft.fd, 0, SEEK_END) != 0)
        return -1;
    o->file_size = ftell(o->ft.fd);
    if (o->file_size < 0)
        return -1;
    rewind(o->ft.fd);
    o->file_bytes = malloc((size_t) o->file_size + 1);
    if (!o->file_bytes)
        return -1;
    if (fread(o->file_bytes, 1, (size_t) o->file_size, o->ft.fd) != (size_t) o->file_size)
        return -1;
    return 0;
}

static void close_out(struct out *o)
{
    if (o->ft.fd)
        fclose(o->ft.fd);
    free(o->ft.priv);
    free(o->file_bytes);
    memset(o, 0, sizeof(*o));
}

/* Walk every byte of the DSF audio region against the bit-level reference.
 * The handler emits 4096-byte blocks block-interleaved across channels
 * (c0 block0, c1 block0, c0 block1, ...), exactly like the non-window path. */
static int check_dsf_audio(const uint8_t *file, const uint8_t *src, int channels,
                           uint64_t begin, uint64_t window_len,
                           uint64_t *audio_size_out)
{
    uint64_t per_channel_bytes = (window_len + 7) / 8;
    uint64_t blocks = (per_channel_bytes + BLOCK - 1) / BLOCK;
    uint64_t audio_size = blocks * BLOCK * (uint64_t) channels;
    uint64_t off;

    if (audio_size_out)
        *audio_size_out = audio_size;

    for (off = 0; off < audio_size; off++)
    {
        uint64_t global_block = off / BLOCK;
        uint64_t j = off % BLOCK;
        int channel = (int) (global_block % (uint64_t) channels);
        uint64_t block = global_block / (uint64_t) channels;
        uint64_t window_byte_index = block * BLOCK + j;
        uint8_t expected;

        if (window_byte_index < per_channel_bytes)
            expected = reverse_byte(expected_msb(src, channels, begin, window_len,
                                                  channel, window_byte_index));
        else
            expected = 0; /* block remainder is zero padded */

        if (file[DSF_HEADER_SIZE + off] != expected)
            return -1;
    }
    return 0;
}

/* Locate a top-level (or nested) chunk by four-char id. */
static const uint8_t *find_chunk(const uint8_t *base, size_t len, const char id[4])
{
    size_t p = 0;
    while (p + 12 <= len)
    {
        uint64_t n = rd_be64(base + p + 4);
        if (!memcmp(base + p, id, 4))
            return base + p;
        if (n > len - p - 12)
            break;
        p += 12 + (size_t) n + ((n & 1) ? 1 : 0);
    }
    return NULL;
}

/* Walk the DSDIFF DSD chunk payload against the MSB-first reference. */
static int check_dsdiff_audio(const uint8_t *file, long file_size, const uint8_t *src,
                              int channels, uint64_t begin, uint64_t window_len,
                              uint64_t *dsd_size_out, const uint8_t **mark_out)
{
    const uint8_t *dsd, *diin, *mark;
    uint64_t n, p;

    if (memcmp(file, "FRM8", 4) != 0)
        return -1;
    if (rd_be64(file + 4) != (uint64_t) file_size - 12)
        return -1;

    dsd = find_chunk(file + 16, (size_t) file_size - 16, "DSD ");
    if (!dsd)
        return -1;
    n = rd_be64(dsd + 4);
    if (dsd_size_out)
        *dsd_size_out = n;
    if (n != (window_len / 8) * (uint64_t) channels) /* DSD chunk length is audio only */
        return -1;

    for (p = 0; p < n; p++)
    {
        int channel = (int) (p % (uint64_t) channels);
        uint64_t j = p / (uint64_t) channels;
        if (dsd[12 + p] != expected_msb(src, channels, begin, window_len, channel, j))
            return -1;
    }

    /* Odd audio length gets one pad byte that is not part of the chunk length. */
    if (n & 1)
    {
        if (dsd[12 + n] != 0)
            return -1;
        if ((size_t) ((dsd + 12 + n + 1) - file) % 2 != 0)
            return -1;
    }

    diin = find_chunk(file + 16, (size_t) file_size - 16, "DIIN");
    if (!diin)
        return -1;
    mark = find_chunk(diin + 12, (size_t) rd_be64(diin + 4), "MARK");
    if (!mark)
        return -1;
    if (mark_out)
        *mark_out = mark;
    return 0;
}

/* ------------------------------- bits group ------------------------------- */

static int case_bits_dsf_unaligned(void)
{
    struct fixture fx;
    struct out o;
    const int channels = 2;
    const uint64_t begin = 3, end = 21; /* non-byte start and end */
    uint8_t src[64];
    uint64_t audio_size;

    fixture_init(&fx, channels, 0);
    make_source(src, sizeof(src) / channels, channels);

    TASK_CHECK(open_out(&fx, dsf_format_fn(), &o, begin, end, 1, 1, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, src, sizeof(src)) >= 0);
    TASK_CHECK(o.ft.handler.stopwrite(&o.ft) == 0);
    TASK_CHECK(slurp(&o) == 0);

    TASK_CHECK(!memcmp(o.file_bytes, "DSD ", 4));
    TASK_CHECK(rd_le64(o.file_bytes + 12) == (uint64_t) o.file_size); /* total file size */
    TASK_CHECK(rd_le64(o.file_bytes + 64) == end - begin); /* sample_count = selected bits */
    TASK_CHECK(check_dsf_audio(o.file_bytes, src, channels, begin, end - begin, &audio_size) == 0);
    TASK_CHECK(rd_le32(o.file_bytes + 72) == BLOCK); /* block size per channel */
    TASK_CHECK(rd_le64(o.file_bytes + 84) == 12 + audio_size); /* data chunk counts physical blocks */
    TASK_CHECK(rd_le64(o.file_bytes + 20) == DSF_HEADER_SIZE + audio_size); /* ID3 pointer */
    TASK_CHECK(rd_le64(o.file_bytes + 12) == (uint64_t) o.file_size);
    TASK_CHECK(!memcmp(o.file_bytes + DSF_HEADER_SIZE + audio_size, "ID3", 3));

    close_out(&o);
    return 0;
}

static int case_bits_dsf_partial_last_byte(void)
{
    struct fixture fx;
    struct out o;
    const int channels = 2;
    const uint64_t begin = 0, end = 11; /* 11 bits: last byte carries 3 valid bits */
    uint8_t src[32];

    fixture_init(&fx, channels, 0);
    make_source(src, sizeof(src) / channels, channels);

    TASK_CHECK(open_out(&fx, dsf_format_fn(), &o, begin, end, 1, 1, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, src, sizeof(src)) >= 0);
    TASK_CHECK(o.ft.handler.stopwrite(&o.ft) == 0);
    TASK_CHECK(slurp(&o) == 0);
    TASK_CHECK(check_dsf_audio(o.file_bytes, src, channels, begin, end - begin, NULL) == 0);
    TASK_CHECK(rd_le64(o.file_bytes + 64) == end - begin);

    close_out(&o);
    return 0;
}

static int case_bits_dsdiff_byte_aligned(void)
{
    struct fixture fx;
    struct out o;
    const int channels = 2;
    const uint64_t begin = 8, end = 40; /* byte aligned */
    uint8_t src[64];
    uint64_t dsd_size = 0;
    const uint8_t *mark = NULL;

    fixture_init(&fx, channels, 0);
    make_source(src, sizeof(src) / channels, channels);

    TASK_CHECK(open_out(&fx, dsdiff_format_fn(), &o, begin, end, 1, 1, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, src, sizeof(src)) >= 0);
    TASK_CHECK(o.ft.handler.stopwrite(&o.ft) == 0);
    TASK_CHECK(slurp(&o) == 0);
    TASK_CHECK(check_dsdiff_audio(o.file_bytes, o.file_size, src, channels,
                                  begin, end - begin, &dsd_size, &mark) == 0);
    TASK_CHECK(dsd_size == ((end - begin) / 8) * (uint64_t) channels);
    TASK_CHECK(rd_be16(mark + 12) == 0);       /* hours */
    TASK_CHECK(mark[14] == 0);                 /* minutes */
    TASK_CHECK(mark[15] == 0);                 /* seconds */
    TASK_CHECK(rd_be32(mark + 16) == (uint32_t) (end - begin)); /* samples, not 75 Hz quantized */

    close_out(&o);
    return 0;
}

static int case_bits_dsdiff_odd_rejected(void)
{
    struct fixture fx;
    struct out o;
    uint8_t src[16] = {0};

    fixture_init(&fx, 2, 0);
    TASK_CHECK(open_out(&fx, dsdiff_format_fn(), &o, 0, 13, 1, 1, 0) < 0);
    TASK_CHECK(o.ft.fd != NULL);
    TASK_CHECK(fseek(o.ft.fd, 0, SEEK_END) == 0);
    TASK_CHECK(ftell(o.ft.fd) == 0); /* nothing was written */
    close_out(&o);
    (void) src;
    return 0;
}

/* ---------------------------- containers group ---------------------------- */

static int case_container_dsf_block_crossing(void)
{
    struct fixture fx;
    struct out o;
    const int channels = 2;
    const uint64_t begin = 3;
    const uint64_t window_len = BLOCK * 8 + 13; /* spans several 4096-byte blocks */
    const uint64_t end = begin + window_len;
    size_t frames = (size_t) (end / 8 + 2);
    uint8_t *src = malloc(frames * channels);
    uint64_t audio_size = 0;
    uint64_t physical_seen = 0;

    TASK_CHECK(src != NULL);
    fixture_init(&fx, channels, 0);
    make_source(src, frames, channels);

    TASK_CHECK(open_out(&fx, dsf_format_fn(), &o, begin, end, 1, 1, 0) == 0);
    {
        size_t sent = 0;
        const size_t cuts[] = {1, 1, 3, 7, 2, 5, 4096, 4099, 11, 0};
        size_t ci = 0;
        while (sent < frames * (size_t) channels)
        {
            size_t n = cuts[ci % (sizeof(cuts) / sizeof(cuts[0]))];
            int r;
            if (n > frames * (size_t) channels - sent)
                n = frames * (size_t) channels - sent;
            r = o.ft.handler.write(&o.ft, src + sent, n);
            TASK_CHECK(r >= 0);
            if (r > 0)
            {
                TASK_CHECK(r % BLOCK == 0); /* physical DSF writes are whole blocks */
                physical_seen += r;
            }
            sent += n;
            ci++;
        }
    }
    TASK_CHECK(o.ft.handler.stopwrite(&o.ft) == 0);
    TASK_CHECK(slurp(&o) == 0);
    TASK_CHECK(check_dsf_audio(o.file_bytes, src, channels, begin, window_len, &audio_size) == 0);
    /* write() only flushes whole blocks; the final per-channel blocks leave at stopwrite */
    TASK_CHECK(physical_seen <= audio_size);
    TASK_CHECK((audio_size - (uint64_t) physical_seen) == (uint64_t) channels * BLOCK);
    TASK_CHECK(rd_le64(o.file_bytes + 12) == (uint64_t) o.file_size);
    TASK_CHECK(rd_le64(o.file_bytes + 84) == 12 + audio_size); /* data chunk: physical blocks */
    TASK_CHECK(rd_le64(o.file_bytes + 20) == DSF_HEADER_SIZE + audio_size); /* ID3 pointer */
    TASK_CHECK(!memcmp(o.file_bytes + DSF_HEADER_SIZE + audio_size, "ID3", 3));

    free(src);
    close_out(&o);
    return 0;
}

static int case_container_dsf_five_channels_odd(void)
{
    struct fixture fx;
    struct out o;
    const int channels = 5;
    const uint64_t begin = 1, end = 22; /* 21 bits, not a multiple of 8 */
    uint8_t src[64];
    uint64_t audio_size = 0;

    fixture_init(&fx, channels, 3);
    make_source(src, sizeof(src) / channels, channels);

    TASK_CHECK(open_out(&fx, dsf_format_fn(), &o, begin, end, 1, 1, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, src, 13) >= 0);  /* split inside a channel group */
    TASK_CHECK(o.ft.handler.write(&o.ft, src + 13, sizeof(src) - 13) >= 0);
    TASK_CHECK(o.ft.handler.stopwrite(&o.ft) == 0);
    TASK_CHECK(slurp(&o) == 0);
    TASK_CHECK(rd_le32(o.file_bytes + 52) == (uint32_t) channels);
    TASK_CHECK(rd_le64(o.file_bytes + 64) == end - begin);
    TASK_CHECK(check_dsf_audio(o.file_bytes, src, channels, begin, end - begin, &audio_size) == 0);
    TASK_CHECK(audio_size == (uint64_t) channels * BLOCK);

    close_out(&o);
    return 0;
}

static int case_container_dsf_six_channels(void)
{
    struct fixture fx;
    struct out o;
    const int channels = 6;
    const uint64_t begin = 0, end = 16;
    uint8_t src[64];

    fixture_init(&fx, channels, 4);
    make_source(src, sizeof(src) / channels, channels);

    TASK_CHECK(open_out(&fx, dsf_format_fn(), &o, begin, end, 1, 1, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, src, sizeof(src)) >= 0);
    TASK_CHECK(o.ft.handler.stopwrite(&o.ft) == 0);
    TASK_CHECK(slurp(&o) == 0);
    TASK_CHECK(check_dsf_audio(o.file_bytes, src, channels, begin, end - begin, NULL) == 0);

    close_out(&o);
    return 0;
}

static int case_container_dsdiff_layout_and_marker(void)
{
    struct fixture fx;
    struct out o;
    const int channels = 2;
    const uint64_t begin = 8;
    const uint64_t window_len = (uint64_t) SACD_SAMPLING_FREQUENCY + 12344; /* 1s + remainder */
    const uint64_t end = begin + window_len;
    size_t frames = (size_t) (end / 8 + 2);
    uint8_t *src = malloc(frames * channels);
    uint64_t dsd_size = 0;
    const uint8_t *mark = NULL;

    TASK_CHECK(src != NULL);
    fixture_init(&fx, channels, 0);
    make_source(src, frames, channels);

    TASK_CHECK(open_out(&fx, dsdiff_format_fn(), &o, begin, end, 1, 1, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, src, frames * channels) >= 0);
    TASK_CHECK(o.ft.handler.stopwrite(&o.ft) == 0);
    TASK_CHECK(slurp(&o) == 0);
    TASK_CHECK(check_dsdiff_audio(o.file_bytes, o.file_size, src, channels,
                                  begin, window_len, &dsd_size, &mark) == 0);
    TASK_CHECK(rd_be16(mark + 12) == 0);
    TASK_CHECK(mark[14] == 0);
    TASK_CHECK(mark[15] == 1);                                     /* seconds recomputed */
    TASK_CHECK(rd_be32(mark + 16) == 12344);                       /* plain sample count */
    TASK_CHECK((rd_be32(mark + 16) % (SAMPLES_PER_FRAME * 64)) != 0);

    free(src);
    close_out(&o);
    return 0;
}

static int case_container_dsdiff_five_channels_pad(void)
{
    struct fixture fx;
    struct out o;
    const int channels = 5;
    const uint64_t begin = 0, end = 24; /* 3 bytes per channel, 15 audio bytes: odd */
    uint8_t src[64];
    uint64_t dsd_size = 0;

    fixture_init(&fx, channels, 3);
    make_source(src, sizeof(src) / channels, channels);

    TASK_CHECK(open_out(&fx, dsdiff_format_fn(), &o, begin, end, 1, 1, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, src, sizeof(src)) >= 0);
    TASK_CHECK(o.ft.handler.stopwrite(&o.ft) == 0);
    TASK_CHECK(slurp(&o) == 0);
    TASK_CHECK(check_dsdiff_audio(o.file_bytes, o.file_size, src, channels,
                                  begin, end - begin, &dsd_size, NULL) == 0);
    TASK_CHECK(dsd_size == 15);

    close_out(&o);
    return 0;
}

/* ------------------------------- state group ------------------------------ */

/* Same window, whole versus arbitrary fragments: audio payloads must match. */
static int case_state_fragmentation_equivalence(void)
{
    struct fixture fx;
    struct out whole, frag;
    const int channels = 2;
    const uint64_t begin = 8, end = 56; /* byte-aligned so it is valid in both containers */
    uint8_t src[128];
    const size_t cuts[] = {1, 0, 1, 3, 7, 2, 5, 0, 11, 4};
    size_t ci = 0, sent = 0;
    int dsf;

    fixture_init(&fx, channels, 0);
    make_source(src, sizeof(src) / channels, channels);

    for (dsf = 1; dsf >= 0; dsf--)
    {
        const scarletbook_format_handler_t *h = dsf ? dsf_format_fn() : dsdiff_format_fn();
        TASK_CHECK(open_out(&fx, h, &whole, begin, end, 1, 1, 0) == 0);
        TASK_CHECK(open_out(&fx, h, &frag, begin, end, 1, 1, 0) == 0);

        TASK_CHECK(whole.ft.handler.write(&whole.ft, src, sizeof(src)) >= 0);
        while (sent < sizeof(src))
        {
            size_t n = cuts[ci++ % (sizeof(cuts) / sizeof(cuts[0]))];
            int r;
            if (n > sizeof(src) - sent)
                n = sizeof(src) - sent;
            r = frag.ft.handler.write(&frag.ft, src + sent, n);
            TASK_CHECK(r >= 0);
            sent += n;
        }

        TASK_CHECK(whole.ft.handler.stopwrite(&whole.ft) == 0);
        TASK_CHECK(frag.ft.handler.stopwrite(&frag.ft) == 0);
        TASK_CHECK(slurp(&whole) == 0 && slurp(&frag) == 0);
        TASK_CHECK(whole.file_size == frag.file_size);
        if (dsf)
            TASK_CHECK(!memcmp(whole.file_bytes + DSF_HEADER_SIZE,
                               frag.file_bytes + DSF_HEADER_SIZE,
                               (size_t) whole.file_size - DSF_HEADER_SIZE));
        else
            TASK_CHECK(!memcmp(whole.file_bytes, frag.file_bytes, (size_t) whole.file_size));

        close_out(&whole);
        close_out(&frag);
        sent = 0;
        ci = 0;
    }
    return 0;
}

/* Two handles (one per container) driven alternately must not share state. */
static int case_state_interleaved_handles(void)
{
    struct fixture fx;
    struct out a, b;
    const int channels = 2;
    uint8_t src[256];
    size_t pa = 0, pb = 0;

    fixture_init(&fx, channels, 0);
    make_source(src, sizeof(src) / channels, channels);

    TASK_CHECK(open_out(&fx, dsf_format_fn(), &a, 3, 70, 1, 1, 0) == 0);
    TASK_CHECK(open_out(&fx, dsdiff_format_fn(), &b, 8, 72, 1, 1, 0) == 0);

    /* Independent cursors, unequal fragments and zero-length calls, interleaved. */
    while (pa < sizeof(src) || pb < sizeof(src))
    {
        if (pa < sizeof(src))
        {
            size_t na = 1 + (pa % 5);
            if (na > sizeof(src) - pa) na = sizeof(src) - pa;
            TASK_CHECK(a.ft.handler.write(&a.ft, src + pa, na) >= 0);
            pa += na;
        }
        TASK_CHECK(b.ft.handler.write(&b.ft, NULL, 0) == 0);
        if (pb < sizeof(src))
        {
            size_t nb = 1 + (pb % 7);
            if (nb > sizeof(src) - pb) nb = sizeof(src) - pb;
            TASK_CHECK(b.ft.handler.write(&b.ft, src + pb, nb) >= 0);
            pb += nb;
        }
        TASK_CHECK(a.ft.handler.write(&a.ft, NULL, 0) == 0);
    }

    TASK_CHECK(a.ft.handler.stopwrite(&a.ft) == 0);
    TASK_CHECK(b.ft.handler.stopwrite(&b.ft) == 0);
    TASK_CHECK(slurp(&a) == 0 && slurp(&b) == 0);
    TASK_CHECK(check_dsf_audio(a.file_bytes, src, channels, 3, 70 - 3, NULL) == 0);
    TASK_CHECK(check_dsdiff_audio(b.file_bytes, b.file_size, src, channels,
                                  8, 72 - 8, NULL, NULL) == 0);

    close_out(&a);
    close_out(&b);
    return 0;
}

/* Input ending before the full end group: stopwrite fails and frees resources. */
static int case_state_short_input(void)
{
    struct fixture fx;
    struct out o;
    const int channels = 2;
    const uint64_t begin = 0, end = 17; /* last sample sits in frame 2 channel 0 */
    uint8_t src[16];

    fixture_init(&fx, channels, 0);
    make_source(src, sizeof(src) / channels, channels);

    TASK_CHECK(open_out(&fx, dsf_format_fn(), &o, begin, end, 1, 1, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, src, 5) >= 0);  /* 5 of the 6 needed group bytes */
    TASK_CHECK(o.ft.handler.stopwrite(&o.ft) < 0);
    close_out(&o);

    TASK_CHECK(open_out(&fx, dsdiff_format_fn(), &o, begin, end - 1, 1, 1, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, src, 3) >= 0);  /* only half the groups */
    TASK_CHECK(o.ft.handler.stopwrite(&o.ft) < 0);
    close_out(&o);
    return 0;
}

/* All rejected configurations: negative return, no header byte on disk. */
static int case_state_rejections(void)
{
    struct fixture fx;
    struct out o;

    fixture_init(&fx, 2, 0);

    TASK_CHECK(open_out(&fx, dsf_format_fn(), &o, 10, 10, 1, 1, 0) < 0); /* begin >= end */
    TASK_CHECK(ftell(o.ft.fd) == 0);
    close_out(&o);

    TASK_CHECK(open_out(&fx, dsf_format_fn(), &o, 0, 8, 1, 1, 1) < 0);  /* nopad + window */
    TASK_CHECK(ftell(o.ft.fd) == 0);
    close_out(&o);

    TASK_CHECK(open_out(&fx, dsf_format_fn(), &o, 0, 8, 1, 0, 0) < 0);  /* DST export + window */
    TASK_CHECK(ftell(o.ft.fd) == 0);
    close_out(&o);

    TASK_CHECK(open_out(&fx, dsdiff_format_fn(), &o, 0, 8, 1, 0, 0) < 0); /* DST + window */
    TASK_CHECK(ftell(o.ft.fd) == 0);
    close_out(&o);

    TASK_CHECK(open_out(&fx, dsdiff_edit_master_format_fn(), &o, 0, 8, 1, 1, 0) < 0);
    TASK_CHECK(ftell(o.ft.fd) == 0);
    close_out(&o);

    fixture_init(&fx, 3, 0); /* unsupported channel count */
    TASK_CHECK(open_out(&fx, dsf_format_fn(), &o, 0, 8, 1, 1, 0) < 0);
    TASK_CHECK(ftell(o.ft.fd) == 0);
    close_out(&o);

    fixture_init(&fx, 2, 0);
    TASK_CHECK(open_out(&fx, dsdiff_format_fn(), &o, 0, 7, 1, 1, 0) < 0); /* odd DSDIFF length */
    TASK_CHECK(ftell(o.ft.fd) == 0);
    close_out(&o);
    return 0;
}

/* Zero-length calls don't advance state; bytes after the window are dropped. */
static int case_state_zero_len_and_trailing(void)
{
    struct fixture fx;
    struct out o;
    const int channels = 2;
    const uint64_t begin = 0, end = 16;
    uint8_t src[128];
    uint64_t audio_size = 0;

    fixture_init(&fx, channels, 0);
    make_source(src, sizeof(src) / channels, channels);

    TASK_CHECK(open_out(&fx, dsf_format_fn(), &o, begin, end, 1, 1, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, NULL, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, src, 4) >= 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, NULL, 0) == 0);
    TASK_CHECK(o.ft.handler.write(&o.ft, src + 4, sizeof(src) - 4) >= 0);
    TASK_CHECK(o.ft.handler.stopwrite(&o.ft) == 0);
    TASK_CHECK(slurp(&o) == 0);
    TASK_CHECK(check_dsf_audio(o.file_bytes, src, channels, begin, end - begin, &audio_size) == 0);
    TASK_CHECK(rd_le64(o.file_bytes + 64) == end - begin);
    close_out(&o);
    return 0;
}

const struct task_case task_cases[] =
{
    { "bits",       "dsf_unaligned_bits",            case_bits_dsf_unaligned              },
    { "bits",       "dsf_partial_last_byte",         case_bits_dsf_partial_last_byte      },
    { "bits",       "dsdiff_byte_aligned",           case_bits_dsdiff_byte_aligned        },
    { "bits",       "dsdiff_odd_length_rejected",    case_bits_dsdiff_odd_rejected        },

    { "containers", "dsf_block_crossing",            case_container_dsf_block_crossing    },
    { "containers", "dsf_five_channel_odd_payload",  case_container_dsf_five_channels_odd },
    { "containers", "dsf_six_channels",              case_container_dsf_six_channels      },
    { "containers", "dsdiff_layout_and_marker",      case_container_dsdiff_layout_and_marker },
    { "containers", "dsdiff_five_channel_pad",       case_container_dsdiff_five_channels_pad },

    { "state",      "fragmentation_equivalence",     case_state_fragmentation_equivalence },
    { "state",      "interleaved_handles",           case_state_interleaved_handles       },
    { "state",      "short_input_fails",             case_state_short_input               },
    { "state",      "rejections_leave_file_empty",   case_state_rejections                },
    { "state",      "zero_length_and_trailing_data", case_state_zero_len_and_trailing     },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
