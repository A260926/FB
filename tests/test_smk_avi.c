#include "task_support/check.h"
#include "smk_avi.h"
#include "smk_export.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined by smk_export.c for the streaming-memory regression. */
extern unsigned long smk_export_debug_pcm_resident_peak(void);
extern unsigned long long smk_export_debug_pcm_streamed_bytes(void);

/* ------------------------------------------------------------------------- */
/* Independent tiny SMK constructor                                          */
/* ------------------------------------------------------------------------- */

#define TB_MAX_FRAMES 16
#define TB_MAX_RECORD 1024

struct tb_bits {
    unsigned char bytes[256];
    unsigned bit_count;
};

struct tb_track {
    int exists;
    uint32_t rate;
    unsigned channels;
    unsigned bits;
    int bink;
};

struct tb_frame {
    int palette;
    unsigned char color7[3];
    int audio_present[7];
    size_t audio_len[7];
    unsigned char audio[7][TB_MAX_RECORD];
};

struct tb_file {
    uint32_t width;
    uint32_t height;
    uint32_t frame_count;
    int32_t rate_raw;
    int ring;
    uint32_t extra_flags;
    int video_void;
    unsigned version;
    struct tb_track tracks[7];
    struct tb_frame frame[TB_MAX_FRAMES];
};

static void tb_put_bits(struct tb_bits *b, uint32_t value, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        if ((value >> i) & 1u)
            b->bytes[b->bit_count / 8u] |=
                (unsigned char)(1u << (b->bit_count % 8u));
        ++b->bit_count;
    }
}

static void tb_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void tb_write_le32(FILE *f, uint32_t v)
{
    unsigned char b[4];
    tb_le32(b, v);
    fwrite(b, 1, 4, f);
}

/* A Huff8 tree containing exactly one leaf. */
static void tb_huff8_one_leaf(struct tb_bits *b, unsigned value)
{
    tb_put_bits(b, 1, 1); /* tree present */
    tb_put_bits(b, 0, 1); /* leaf */
    tb_put_bits(b, value, 8);
    tb_put_bits(b, 0, 1); /* terminator */
}

/*
 * Build the TYPE tree.  Void mode has one leaf 0x0002 and writes no pixels,
 * which keeps odd-width padding tests inside the decoder's frame allocation.
 * Solid mode has two leaves 0x0703/0x0803 for orientation checks.
 */
static unsigned tb_type_tree(struct tb_bits *b, int void_mode)
{
    unsigned tree_bytes;
    int t;

    memset(b, 0, sizeof *b);

    for (t = 0; t < 3; ++t) {
        tb_put_bits(b, 0, 1); /* absent tree */
        tb_put_bits(b, 0, 1); /* expected final zero bit */
    }

    tb_put_bits(b, 1, 1); /* Huff16 tree present */
    if (void_mode) {
        tb_huff8_one_leaf(b, 2); /* low byte: type 2 (void) */
        tb_huff8_one_leaf(b, 0); /* high byte / typedata */
        for (t = 0; t < 3; ++t)
            tb_put_bits(b, 0xffffu, 16);
        tb_put_bits(b, 0, 1); /* one big-tree leaf */
        tb_put_bits(b, 0, 1); /* Huff16 terminator */
    } else {
        tb_huff8_one_leaf(b, 3); /* low byte of TYPE values */

        /* High byte Huff8 tree: bit 0 => 7, bit 1 => 8. */
        tb_put_bits(b, 1, 1); /* Huff8 present */
        tb_put_bits(b, 1, 1); /* root branch */
        tb_put_bits(b, 0, 1); /* left leaf */
        tb_put_bits(b, 7, 8);
        tb_put_bits(b, 0, 1); /* right leaf */
        tb_put_bits(b, 8, 8);
        tb_put_bits(b, 0, 1); /* Huff8 terminator */

        for (t = 0; t < 3; ++t)
            tb_put_bits(b, 0xffffu, 16);

        tb_put_bits(b, 1, 1); /* big-tree root branch */
        tb_put_bits(b, 0, 1); /* left Huff16 leaf */
        tb_put_bits(b, 0, 1); /* high-byte selector: 7 */
        tb_put_bits(b, 0, 1); /* right Huff16 leaf */
        tb_put_bits(b, 1, 1); /* high-byte selector: 8 */
        tb_put_bits(b, 0, 1); /* Huff16 terminator */
    }

    tree_bytes = (b->bit_count + 7u) / 8u;
    return tree_bytes;
}

static void tb_palette_void8(unsigned char out[8], const unsigned char color[3])
{
    out[0] = 2; /* record size 8 */
    out[1] = color[0];
    out[2] = color[1];
    out[3] = color[2];
    out[4] = 0xff; /* skip 128 entries */
    out[5] = 0xfe; /* skip 127 entries, reaching 256 */
    out[6] = 0;
    out[7] = 0;
}

static void tb_palette12(unsigned char out[12],
                         const unsigned char c7[3],
                         const unsigned char c8[3])
{
    unsigned i;
    out[0] = 3;       /* palette record is 3*4 = 12 bytes */
    out[1] = 0x86;    /* skip entries 0..6 */
    for (i = 0; i < 3; ++i) {
        out[2 + i] = c7[i];
        out[5 + i] = c8[i];
    }
    out[8] = 0xff;    /* skip 128 entries */
    out[9] = 0xf6;    /* skip 119 entries, reaching 256 */
    out[10] = 0;
    out[11] = 0;
}

static int tb_build(const char *path, const struct tb_file *cfg)
{
    struct tb_bits trees;
    unsigned tree_size;
    unsigned total_frames;
    unsigned fidx, t;
    uint32_t chunk_sizes[TB_MAX_FRAMES];
    unsigned char frame_types[TB_MAX_FRAMES];
    unsigned char chunks[TB_MAX_FRAMES][8192];
    uint32_t max_buffer[7];
    FILE *f;

    if ((cfg->width != 3 && cfg->width != 4) || cfg->height != 8)
        return -1;
    total_frames = cfg->frame_count + (cfg->ring ? 1u : 0u);
    if (total_frames > TB_MAX_FRAMES)
        return -1;

    tree_size = tb_type_tree(&trees, cfg->video_void);

    for (t = 0; t < 7; ++t) {
        unsigned q;
        size_t largest = 64;
        for (q = 0; q < total_frames; ++q) {
            if (cfg->frame[q].audio_present[t] &&
                cfg->frame[q].audio_len[t] > largest)
                largest = cfg->frame[q].audio_len[t];
        }
        max_buffer[t] = (uint32_t)largest;
    }

    for (fidx = 0; fidx < total_frames; ++fidx) {
        const struct tb_frame *fr = &cfg->frame[fidx];
        unsigned char *chunk = chunks[fidx];
        size_t used = 0;
        uint32_t aligned;

        frame_types[fidx] = fr->palette ? 0x01 : 0x00;
        if (fr->palette) {
            if (cfg->video_void) {
                unsigned char pal[8];
                tb_palette_void8(pal, fr->color7);
                memcpy(chunk + used, pal, sizeof pal);
                used += sizeof pal;
            } else {
                static const unsigned char white[3] = {63, 63, 63};
                unsigned char pal[12];
                tb_palette12(pal, fr->color7, white);
                memcpy(chunk + used, pal, sizeof pal);
                used += sizeof pal;
            }
        }

        for (t = 0; t < 7; ++t) {
            uint32_t record_size;
            if (!fr->audio_present[t])
                continue;
            frame_types[fidx] |= (unsigned char)(0x02u << t);
            if (used + 4 + fr->audio_len[t] + 4 > sizeof chunks[fidx])
                return -1;
            record_size = (uint32_t)(fr->audio_len[t] + 4u);
            tb_le32(chunk + used, record_size);
            used += 4;
            memcpy(chunk + used, fr->audio[t], fr->audio_len[t]);
            used += fr->audio_len[t];
        }

        /* Void mode ignores bits; solid mode selects 7 then 8 for height 8. */
        chunk[used++] = cfg->video_void ? 0x00 : 0x02;
        chunk[used++] = 0;
        chunk[used++] = 0;
        chunk[used++] = 0;
        aligned = (uint32_t)((used + 3u) & ~(size_t)3u);
        if (aligned > sizeof chunks[fidx])
            return -1;
        while (used < aligned)
            chunk[used++] = 0;
        chunk_sizes[fidx] = aligned | 1u;
    }

    f = fopen(path, "wb");
    if (!f)
        return -1;

    fwrite("SMK", 1, 3, f);
    fputc(cfg->version == 2 ? '2' : '4', f);
    tb_write_le32(f, cfg->width);
    tb_write_le32(f, cfg->height);
    tb_write_le32(f, cfg->frame_count);
    tb_write_le32(f, (uint32_t)cfg->rate_raw);
    tb_write_le32(f, (uint32_t)(cfg->ring ? 1u : 0u) | cfg->extra_flags);

    for (t = 0; t < 7; ++t)
        tb_write_le32(f, cfg->tracks[t].exists ? max_buffer[t] : 0);

    tb_write_le32(f, tree_size);
    tb_write_le32(f, 16); /* MMAP */
    tb_write_le32(f, 16); /* MCLR */
    tb_write_le32(f, 16); /* FULL */
    tb_write_le32(f, cfg->video_void ? 16 : 24); /* TYPE */

    for (t = 0; t < 7; ++t) {
        uint32_t word = 0;
        const struct tb_track *tr = &cfg->tracks[t];
        if (tr->exists) {
            word = UINT32_C(0x40000000) | (tr->rate & 0x00ffffffu);
            if (tr->bits == 16)
                word |= UINT32_C(0x20000000);
            if (tr->channels == 2)
                word |= UINT32_C(0x10000000);
            if (tr->bink)
                word |= UINT32_C(0x0c000000);
        }
        tb_write_le32(f, word);
    }

    tb_write_le32(f, 0);
    for (fidx = 0; fidx < total_frames; ++fidx)
        tb_write_le32(f, chunk_sizes[fidx]);
    for (fidx = 0; fidx < total_frames; ++fidx)
        fputc(frame_types[fidx], f);
    fwrite(trees.bytes, 1, tree_size, f);
    for (fidx = 0; fidx < total_frames; ++fidx)
        fwrite(chunks[fidx], 1, chunk_sizes[fidx] & ~3u, f);

    if (ferror(f) || fclose(f) != 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Independent RIFF/AVI reader                                               */
/* ------------------------------------------------------------------------- */

struct rr_chunk {
    unsigned char id[4];
    uint32_t size;
    size_t header_at;
    size_t data_at;
};

struct rr_index {
    unsigned char id[4];
    uint32_t flags;
    uint32_t offset;
    uint32_t length;
};

struct rr_strl {
    size_t strh_data;
    size_t strf_data;
};

struct rr_avi {
    unsigned char *data;
    size_t size;
    struct rr_chunk hdrl;
    struct rr_chunk movi;
    struct rr_chunk idx1;
    struct rr_chunk media[8192];
    unsigned media_count;
    struct rr_index entries[8192];
    unsigned index_count;
    struct rr_strl strl[8];
    unsigned strl_count;
    size_t movi_type_at;
};

static uint32_t rr_u32(const struct rr_avi *a, size_t off)
{
    return (uint32_t)a->data[off] |
           ((uint32_t)a->data[off + 1] << 8) |
           ((uint32_t)a->data[off + 2] << 16) |
           ((uint32_t)a->data[off + 3] << 24);
}

static uint16_t rr_u16(const struct rr_avi *a, size_t off)
{
    return (uint16_t)((uint16_t)a->data[off] |
                      ((uint16_t)a->data[off + 1] << 8));
}

static int rr_id(const struct rr_avi *a, size_t off, const char *id)
{
    return off + 4 <= a->size && memcmp(a->data + off, id, 4) == 0;
}

static int rr_load(const char *path, struct rr_avi *a)
{
    FILE *f;
    long length;
    size_t pos;

    memset(a, 0, sizeof *a);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    a->size = (size_t)length;
    a->data = (unsigned char *)malloc(a->size ? a->size : 1);
    if (!a->data || fread(a->data, 1, a->size, f) != a->size ||
        fclose(f) != 0) {
        free(a->data);
        memset(a, 0, sizeof *a);
        return -1;
    }

    if (a->size < 12 || !rr_id(a, 0, "RIFF") || !rr_id(a, 8, "AVI ") ||
        rr_u32(a, 4) != (uint32_t)(a->size - 8))
        return -1;

    pos = 12;
    while (pos + 8 <= a->size) {
        struct rr_chunk c;
        size_t end;
        memcpy(c.id, a->data + pos, 4);
        c.size = rr_u32(a, pos + 4);
        c.header_at = pos;
        c.data_at = pos + 8;
        if ((uint64_t)c.data_at + c.size > a->size)
            return -1;
        end = c.data_at + c.size + (c.size & 1u);
        if (end > a->size)
            return -1;

        if (rr_id(a, pos, "LIST") && c.size >= 4) {
            if (rr_id(a, c.data_at, "hdrl"))
                a->hdrl = c;
            if (rr_id(a, c.data_at, "movi"))
                a->movi = c;
        } else if (rr_id(a, pos, "idx1")) {
            a->idx1 = c;
        }
        pos = end;
    }

    if (!a->hdrl.data_at || !a->movi.data_at || !a->idx1.data_at)
        return -1;

    /* Walk stream lists in hdrl. */
    pos = a->hdrl.data_at + 4;
    while (pos + 8 <= a->hdrl.data_at + a->hdrl.size) {
        uint32_t sz = rr_u32(a, pos + 4);
        size_t data = pos + 8;
        size_t next = data + sz + (sz & 1u);
        if (next > a->size)
            return -1;
        if (rr_id(a, pos, "LIST") && sz >= 4 && rr_id(a, data, "strl") &&
            a->strl_count < 8) {
            size_t q = data + 4;
            size_t stop = data + sz;
            struct rr_strl *s = &a->strl[a->strl_count++];
            while (q + 8 <= stop) {
                uint32_t qsz = rr_u32(a, q + 4);
                if (rr_id(a, q, "strh"))
                    s->strh_data = q + 8;
                if (rr_id(a, q, "strf"))
                    s->strf_data = q + 8;
                q += 8 + qsz + (qsz & 1u);
            }
        }
        pos = next;
    }

    a->movi_type_at = a->movi.data_at;
    pos = a->movi.data_at + 4;
    while (pos + 8 <= a->movi.data_at + a->movi.size) {
        struct rr_chunk *c;
        uint32_t sz = rr_u32(a, pos + 4);
        if (a->media_count >= sizeof a->media / sizeof a->media[0])
            return -1;
        c = &a->media[a->media_count++];
        memcpy(c->id, a->data + pos, 4);
        c->size = sz;
        c->header_at = pos;
        c->data_at = pos + 8;
        if ((uint64_t)c->data_at + sz > a->size)
            return -1;
        pos = c->data_at + sz + (sz & 1u);
    }

    if (a->idx1.size % 16u != 0 ||
        a->idx1.size / 16u > sizeof a->entries / sizeof a->entries[0])
        return -1;
    a->index_count = a->idx1.size / 16u;
    for (unsigned i = 0; i < a->index_count; ++i) {
        size_t p = a->idx1.data_at + i * 16u;
        memcpy(a->entries[i].id, a->data + p, 4);
        a->entries[i].flags = rr_u32(a, p + 4);
        a->entries[i].offset = rr_u32(a, p + 8);
        a->entries[i].length = rr_u32(a, p + 12);
    }
    return 0;
}

static void rr_free(struct rr_avi *a)
{
    free(a->data);
    a->data = NULL;
    a->size = 0;
}

static void test_path(char *buf, size_t n, const char *name)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = ".";
    snprintf(buf, n, "%s%s%s", dir,
             dir[strlen(dir) - 1] == '/' ? "" : "/", name);
}

static int test_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static uint64_t expected_count(uint64_t frame, uint32_t rate, uint64_t us)
{
    uint64_t start = (frame * rate * us) / UINT64_C(1000000);
    uint64_t end = ((frame + 1u) * rate * us) / UINT64_C(1000000);
    return end - start;
}

static unsigned char pal6_to_8(unsigned value)
{
    static const unsigned char map[64] = {
        0x00,0x04,0x08,0x0c,0x10,0x14,0x18,0x1c,
        0x20,0x24,0x28,0x2c,0x30,0x34,0x38,0x3c,
        0x41,0x45,0x49,0x4d,0x51,0x55,0x59,0x5d,
        0x61,0x65,0x69,0x6d,0x71,0x75,0x79,0x7d,
        0x82,0x86,0x8a,0x8e,0x92,0x96,0x9a,0x9e,
        0xa2,0xa6,0xaa,0xae,0xb2,0xb6,0xba,0xbe,
        0xc3,0xc7,0xcb,0xcf,0xd3,0xd7,0xdb,0xdf,
        0xe3,0xe7,0xeb,0xef,0xf3,0xf7,0xfb,0xff
    };
    return map[value & 63u];
}

static void combo_config(struct tb_file *cfg)
{
    static const unsigned char white6[3] = {63, 63, 63};
    unsigned i, q;

    memset(cfg, 0, sizeof *cfg);
    cfg->width = 3;
    cfg->height = 8;
    cfg->frame_count = 5;
    cfg->rate_raw = -3333; /* U = 33330 us */
    cfg->ring = 1;
    cfg->video_void = 1; /* odd width is safe and exercises BGR row padding */

    cfg->tracks[0].exists = 1;
    cfg->tracks[0].rate = 1000;
    cfg->tracks[0].channels = 1;
    cfg->tracks[0].bits = 8;

    cfg->tracks[3].exists = 1;
    cfg->tracks[3].rate = 1000;
    cfg->tracks[3].channels = 2;
    cfg->tracks[3].bits = 16;

    cfg->tracks[6].exists = 1;
    cfg->tracks[6].rate = 500;
    cfg->tracks[6].channels = 2;
    cfg->tracks[6].bits = 8;

    for (q = 0; q < cfg->frame_count + 1; ++q) {
        for (i = 0; i < 7; ++i)
            memcpy(cfg->frame[q].color7, white6, 3);
    }

    cfg->frame[0].palette = 1;
    cfg->frame[0].color7[0] = 63;
    cfg->frame[0].color7[1] = 0;
    cfg->frame[0].color7[2] = 0;

    cfg->frame[2].palette = 1;
    cfg->frame[2].color7[0] = 0;
    cfg->frame[2].color7[1] = 63;
    cfg->frame[2].color7[2] = 32;

    cfg->frame[4].palette = 1;
    cfg->frame[4].color7[0] = 16;
    cfg->frame[4].color7[1] = 0;
    cfg->frame[4].color7[2] = 63;

    /* Track 0: 100 front bytes, an empty record, then 40 more bytes. */
    cfg->frame[0].audio_present[0] = 1;
    cfg->frame[0].audio_len[0] = 100;
    for (i = 0; i < 100; ++i)
        cfg->frame[0].audio[0][i] = (unsigned char)(11 + i * 7);

    cfg->frame[2].audio_present[0] = 1;
    cfg->frame[2].audio_len[0] = 0;

    cfg->frame[3].audio_present[0] = 1;
    cfg->frame[3].audio_len[0] = 40;
    for (i = 0; i < 40; ++i)
        cfg->frame[3].audio[0][i] = (unsigned char)(0x80 + i);

    /* Track 3: 16-bit stereo, split across non-aligned AVI chunk boundaries. */
    cfg->frame[0].audio_present[3] = 1;
    cfg->frame[0].audio_len[3] = 120;
    for (i = 0; i < 120; ++i)
        cfg->frame[0].audio[3][i] = (unsigned char)(i ^ 0x55);

    cfg->frame[1].audio_present[3] = 1;
    cfg->frame[1].audio_len[3] = 0;

    cfg->frame[2].audio_present[3] = 1;
    cfg->frame[2].audio_len[3] = 120;
    for (i = 0; i < 120; ++i)
        cfg->frame[2].audio[3][i] = (unsigned char)(0x40 + i);

    cfg->frame[4].audio_present[3] = 1;
    cfg->frame[4].audio_len[3] = 500;
    for (i = 0; i < 500; ++i)
        cfg->frame[4].audio[3][i] = (unsigned char)(i * 3 + 1);

    /* Sparse track 6: 8-bit stereo. */
    cfg->frame[0].audio_present[6] = 1;
    cfg->frame[0].audio_len[6] = 10;
    for (i = 0; i < 10; ++i)
        cfg->frame[0].audio[6][i] = (unsigned char)(0x20 + i);

    cfg->frame[3].audio_present[6] = 1;
    cfg->frame[3].audio_len[6] = 20;
    for (i = 0; i < 20; ++i)
        cfg->frame[3].audio[6][i] = (unsigned char)(0xa0 + i);

    /* The ring chunk has distinctive audio that must never be exported. */
    cfg->frame[5].audio_present[0] = 1;
    cfg->frame[5].audio_len[0] = 32;
    memset(cfg->frame[5].audio[0], 0xee, 32);
    cfg->frame[5].audio_present[3] = 1;
    cfg->frame[5].audio_len[3] = 32;
    memset(cfg->frame[5].audio[3], 0xcd, 32);
}

static struct tb_file combo_expected_cfg;

static size_t combo_source_record_for_test(unsigned frame, unsigned track,
                                          unsigned char *out, size_t capacity)
{
    size_t len = combo_expected_cfg.frame[frame].audio_len[track];
    if (len > capacity)
        return (size_t)-1;
    memcpy(out, combo_expected_cfg.frame[frame].audio[track], len);
    return len;
}

static int check_index_targets(const struct rr_avi *a)
{
    unsigned i;
    TASK_CHECK(a->index_count == a->media_count);
    for (i = 0; i < a->index_count; ++i) {
        const struct rr_index *e = &a->entries[i];
        const struct rr_chunk *c = &a->media[i];
        size_t pointed = a->movi_type_at + e->offset;
        TASK_CHECK(memcmp(e->id, c->id, 4) == 0);
        TASK_CHECK(e->length == c->size);
        if (c->size & 1u)
            TASK_CHECK(a->data[c->data_at + c->size] == 0);
        TASK_CHECK(pointed == c->header_at);
        TASK_CHECK(e->offset == (uint32_t)(c->header_at - a->movi_type_at));
        TASK_CHECK(rr_u32(a, pointed + 4) == e->length);
        if (memcmp(e->id, "00db", 4) == 0)
            TASK_CHECK(e->flags == 0x10);
        else
            TASK_CHECK(e->flags == 0);
    }
    return 0;
}

static int check_combo_output(const char *avi_path)
{
    struct rr_avi avi;
    static const unsigned tracks[3] = {0, 3, 6};
    static const uint32_t rates[3] = {1000, 1000, 500};
    static const unsigned block_align[3] = {1, 4, 2};
    static const unsigned bits[3] = {8, 16, 8};
    unsigned char source[3][4096];
    size_t source_len[3] = {0, 0, 0};
    unsigned f, j;
    size_t p;
    uint32_t avih_data;
    uint64_t totals[3] = {0, 0, 0};
    uint32_t max_chunk[3] = {0, 0, 0};

    TASK_CHECK(rr_load(avi_path, &avi) == 0);
    TASK_CHECK(avi.strl_count == 4);
    TASK_CHECK(avi.media_count == 20);
    TASK_CHECK(avi.index_count == 20);
    TASK_CHECK(check_index_targets(&avi) == 0);

    TASK_CHECK(rr_id(&avi, avi.hdrl.data_at + 4, "avih"));
    avih_data = avi.hdrl.data_at + 12;
    TASK_CHECK(rr_u32(&avi, avih_data + 0) == 33330);
    TASK_CHECK(rr_u32(&avi, avih_data + 16) == 5);
    TASK_CHECK(rr_u32(&avi, avih_data + 24) == 4);

    TASK_CHECK(rr_u32(&avi, avi.strl[0].strh_data + 20) == 3333);
    TASK_CHECK(rr_u32(&avi, avi.strl[0].strh_data + 24) == 100000);
    TASK_CHECK(rr_u32(&avi, avi.strl[0].strh_data + 32) == 5);
    TASK_CHECK(rr_u32(&avi, avi.strl[0].strh_data + 36) == 96);
    TASK_CHECK(rr_u32(&avi, avi.strl[0].strh_data + 44) == 0);
    TASK_CHECK(rr_u32(&avi, avi.strl[0].strf_data + 4) == 3);
    TASK_CHECK(rr_u32(&avi, avi.strl[0].strf_data + 8) == 8);
    TASK_CHECK(rr_u32(&avi, avi.strl[0].strf_data + 20) == 96);
    TASK_CHECK(avi.data[avi.strl[0].strf_data + 14] == 24);

    for (j = 0; j < 3; ++j) {
        unsigned original = tracks[j];
        for (f = 0; f < 5; ++f) {
            size_t n;
            n = combo_source_record_for_test(f, original,
                                            source[j] + source_len[j],
                                            sizeof(source[j]) - source_len[j]);
            source_len[j] += n;
        }
        TASK_CHECK(source_len[j] < sizeof source[j]);
    }

    for (f = 0; f < 5; ++f) {
        static const unsigned char c7_codes[5][3] = {
            {63, 0, 0}, {63, 0, 0}, {0, 63, 32},
            {0, 63, 32}, {16, 0, 63}
        };
        unsigned char dib[96];
        const struct rr_chunk *vc = &avi.media[f * 4u];
        unsigned dib_row, x;

        memset(dib, 0, sizeof dib);
        for (dib_row = 0; dib_row < 8; ++dib_row) {
            unsigned char bgr[3];
            if (combo_expected_cfg.video_void) {
                /* Void blocks leave every indexed pixel at calloc-zero. */
                bgr[0] = pal6_to_8(c7_codes[f][2]);
                bgr[1] = pal6_to_8(c7_codes[f][1]);
                bgr[2] = pal6_to_8(c7_codes[f][0]);
            } else if (dib_row < 4) {
                /* Bottom-up rows 0..3 are SMK rows 7..4 and palette index 8. */
                bgr[0] = bgr[1] = bgr[2] = 255;
            } else {
                bgr[0] = pal6_to_8(c7_codes[f][2]);
                bgr[1] = pal6_to_8(c7_codes[f][1]);
                bgr[2] = pal6_to_8(c7_codes[f][0]);
            }
            for (x = 0; x < 3; ++x) {
                dib[dib_row * 12u + x * 3u + 0u] = bgr[0];
                dib[dib_row * 12u + x * 3u + 1u] = bgr[1];
                dib[dib_row * 12u + x * 3u + 2u] = bgr[2];
            }
        }

        TASK_CHECK(memcmp(vc->id, "00db", 4) == 0);
        TASK_CHECK(vc->size == sizeof dib);
        TASK_CHECK(memcmp(avi.data + vc->data_at, dib, sizeof dib) == 0);

        for (j = 0; j < 3; ++j) {
            uint64_t start_samples = (uint64_t)f * rates[j] * 33330ull / 1000000ull;
            uint64_t samples = expected_count(f, rates[j], 33330);
            uint32_t bytes = (uint32_t)(samples * block_align[j]);
            const struct rr_chunk *ac = &avi.media[f * 4u + 1u + j];

            TASK_CHECK(ac->id[0] == '0');
            TASK_CHECK(ac->id[1] == (unsigned char)('1' + j));
            TASK_CHECK(memcmp(ac->id + 2, "wb", 2) == 0);
            TASK_CHECK(ac->size == bytes);
            totals[j] += samples;
            if (bytes > max_chunk[j])
                max_chunk[j] = bytes;

            for (p = 0; p < bytes; ++p) {
                uint64_t global_byte = start_samples * block_align[j] + p;
                unsigned char wanted;
                if (global_byte < source_len[j])
                    wanted = source[j][(size_t)global_byte];
                else
                    wanted = bits[j] == 8 ? 0x80 : 0;
                TASK_CHECK(avi.data[ac->data_at + p] == wanted);
            }
        }
    }

    TASK_CHECK(rr_u32(&avi, avi.strl[1].strh_data + 20) == 1);
    TASK_CHECK(rr_u32(&avi, avi.strl[1].strh_data + 24) == 1000);
    TASK_CHECK(rr_u32(&avi, avi.strl[1].strh_data + 32) == (uint32_t)totals[0]);
    TASK_CHECK(rr_u32(&avi, avi.strl[1].strh_data + 36) == max_chunk[0]);
    TASK_CHECK(rr_u32(&avi, avi.strl[1].strh_data + 44) == 1);
    TASK_CHECK(rr_u16(&avi, avi.strl[1].strf_data + 2) == 1);
    TASK_CHECK(rr_u32(&avi, avi.strl[1].strf_data + 4) == 1000);
    TASK_CHECK(rr_u16(&avi, avi.strl[1].strf_data + 12) == 1);
    TASK_CHECK(avi.data[avi.strl[1].strf_data + 14] == 8);

    TASK_CHECK(rr_u32(&avi, avi.strl[2].strh_data + 20) == 4);
    TASK_CHECK(rr_u32(&avi, avi.strl[2].strh_data + 24) == 4000);
    TASK_CHECK(rr_u32(&avi, avi.strl[2].strh_data + 32) == (uint32_t)totals[1]);
    TASK_CHECK(rr_u32(&avi, avi.strl[2].strh_data + 36) == max_chunk[1]);
    TASK_CHECK(rr_u32(&avi, avi.strl[2].strh_data + 44) == 4);
    TASK_CHECK(rr_u16(&avi, avi.strl[2].strf_data + 2) == 2);
    TASK_CHECK(rr_u16(&avi, avi.strl[2].strf_data + 12) == 4);
    TASK_CHECK(avi.data[avi.strl[2].strf_data + 14] == 16);

    TASK_CHECK(rr_u32(&avi, avi.strl[3].strh_data + 20) == 2);
    TASK_CHECK(rr_u32(&avi, avi.strl[3].strh_data + 24) == 1000);
    TASK_CHECK(rr_u32(&avi, avi.strl[3].strh_data + 32) == (uint32_t)totals[2]);
    TASK_CHECK(rr_u32(&avi, avi.strl[3].strh_data + 36) == max_chunk[2]);
    TASK_CHECK(rr_u32(&avi, avi.strl[3].strh_data + 44) == 2);
    TASK_CHECK(rr_u16(&avi, avi.strl[3].strf_data + 2) == 2);
    TASK_CHECK(rr_u32(&avi, avi.strl[3].strf_data + 4) == 500);
    TASK_CHECK(avi.data[avi.strl[3].strf_data + 14] == 8);

    rr_free(&avi);
    return 0;
}

static int case_pcm_combo(void)
{
    char smk_path[1024], avi_path[1024];

    test_path(smk_path, sizeof smk_path, "task_combo.smk");
    test_path(avi_path, sizeof avi_path, "task_combo.avi");
    remove(smk_path);
    remove(avi_path);

    combo_config(&combo_expected_cfg);
    TASK_CHECK(tb_build(smk_path, &combo_expected_cfg) == 0);
    TASK_CHECK(smk_export_avi(smk_path, avi_path, 0x49) == 0);
    TASK_CHECK(check_combo_output(avi_path) == 0);

    remove(smk_path);
    remove(avi_path);
    return 0;
}

static int case_layout_no_audio(void)
{
    struct tb_file cfg;
    char smk_path[1024], avi_path[1024];
    struct rr_avi avi;
    unsigned i;

    test_path(smk_path, sizeof smk_path, "task_noaudio.smk");
    test_path(avi_path, sizeof avi_path, "task_noaudio.avi");
    remove(smk_path);
    remove(avi_path);

    combo_config(&cfg);
    TASK_CHECK(tb_build(smk_path, &cfg) == 0);
    TASK_CHECK(smk_export_avi(smk_path, avi_path, 0) == 0);
    TASK_CHECK(rr_load(avi_path, &avi) == 0);
    TASK_CHECK(avi.strl_count == 1);
    TASK_CHECK(avi.media_count == 5);
    TASK_CHECK(avi.index_count == 5);
    TASK_CHECK(check_index_targets(&avi) == 0);
    for (i = 0; i < avi.media_count; ++i) {
        TASK_CHECK(memcmp(avi.media[i].id, "00db", 4) == 0);
        TASK_CHECK(avi.media[i].size == 96);
        TASK_CHECK(avi.entries[i].flags == 0x10);
    }
    rr_free(&avi);

    remove(smk_path);
    remove(avi_path);
    return 0;
}

static int case_smk2_version(void)
{
    struct tb_file cfg;
    char smk_path[1024], avi_path[1024];
    struct rr_avi avi;

    test_path(smk_path, sizeof smk_path, "task_smk2.smk");
    test_path(avi_path, sizeof avi_path, "task_smk2.avi");
    remove(smk_path);
    remove(avi_path);

    combo_config(&cfg);
    cfg.version = 2;
    TASK_CHECK(tb_build(smk_path, &cfg) == 0);
    TASK_CHECK(smk_export_avi(smk_path, avi_path, 0x49) == 0);
    TASK_CHECK(rr_load(avi_path, &avi) == 0);
    TASK_CHECK(avi.data[0] == 'R');
    TASK_CHECK(avi.media_count == 20);
    TASK_CHECK(check_index_targets(&avi) == 0);
    rr_free(&avi);

    remove(smk_path);
    remove(avi_path);
    return 0;
}

static int case_layout_orientation(void)
{
    struct tb_file cfg;
    char smk_path[1024], avi_path[1024];
    struct rr_avi avi;
    unsigned char expected[96];
    unsigned row, x;

    test_path(smk_path, sizeof smk_path, "task_orientation.smk");
    test_path(avi_path, sizeof avi_path, "task_orientation.avi");
    remove(smk_path);
    remove(avi_path);

    combo_config(&cfg);
    cfg.video_void = 0;
    cfg.width = 4;
    cfg.frame_count = 2;
    TASK_CHECK(tb_build(smk_path, &cfg) == 0);
    TASK_CHECK(smk_export_avi(smk_path, avi_path, 0) == 0);
    TASK_CHECK(rr_load(avi_path, &avi) == 0);
    TASK_CHECK(avi.media_count == 2);

    /* DIB rows 0..3 are SMK bottom rows and palette index 8 (white).
       DIB rows 4..7 are SMK top rows and palette index 7 (red). */
    memset(expected, 0, sizeof expected);
    for (row = 0; row < 8; ++row) {
        unsigned char b = row < 4 ? 255 : 0;
        unsigned char g = row < 4 ? 255 : 0;
        unsigned char r = 255;
        for (x = 0; x < 4; ++x) {
            expected[row * 12u + x * 3u + 0u] = b;
            expected[row * 12u + x * 3u + 1u] = g;
            expected[row * 12u + x * 3u + 2u] = r;
        }
    }
    TASK_CHECK(avi.media[0].size == sizeof expected);
    TASK_CHECK(memcmp(avi.data + avi.media[0].data_at, expected,
                      sizeof expected) == 0);
    TASK_CHECK(memcmp(avi.data + avi.media[1].data_at, expected,
                      sizeof expected) == 0);
    rr_free(&avi);

    remove(smk_path);
    remove(avi_path);
    return 0;
}

static int case_failure_cleanup(void)
{
    struct tb_file cfg;
    char smk_path[1024], avi_path[1024], bad_path[1100];
    const char *tmpdir = getenv("TMPDIR");

    test_path(smk_path, sizeof smk_path, "task_bad_y.smk");
    test_path(avi_path, sizeof avi_path, "task_failure.avi");
    remove(smk_path);
    remove(avi_path);

    combo_config(&cfg);
    cfg.extra_flags = 0x02; /* Y double, explicitly unsupported */
    TASK_CHECK(tb_build(smk_path, &cfg) == 0);
    TASK_CHECK(smk_export_avi(smk_path, avi_path, 0x49) != 0);
    TASK_CHECK(!test_file_exists(avi_path));

    combo_config(&cfg);
    cfg.tracks[0].bink = 1;
    TASK_CHECK(tb_build(smk_path, &cfg) == 0);
    TASK_CHECK(smk_export_avi(smk_path, avi_path, 0x01) != 0);
    TASK_CHECK(!test_file_exists(avi_path));

    TASK_CHECK(smk_export_avi(smk_path, avi_path, 0x80) != 0);
    TASK_CHECK(!test_file_exists(avi_path));

    /* A legal header but an interval too large for a 32-bit chunk. */
    combo_config(&cfg);
    cfg.frame_count = 1;
    cfg.ring = 0;
    cfg.rate_raw = 1000000; /* U = 1,000,000,000 us */
    cfg.tracks[0].rate = 16000000;
    TASK_CHECK(tb_build(smk_path, &cfg) == 0);
    TASK_CHECK(smk_export_avi(smk_path, avi_path, 0x01) != 0);
    TASK_CHECK(!test_file_exists(avi_path));

    if (!tmpdir || !*tmpdir)
        tmpdir = ".";
    snprintf(bad_path, sizeof bad_path,
             "%s/no-such-task-directory/out.avi", tmpdir);
    TASK_CHECK(smk_export_avi(smk_path, bad_path, 0x01) != 0);
    TASK_CHECK(smk_export_avi(smk_path, smk_path, 0x01) != 0);

    remove(smk_path);
    return 0;
}

static int write_long_smk(const char *path, unsigned frames, unsigned record_len)
{
    struct tb_bits trees;
    unsigned tree_size = tb_type_tree(&trees, 0);
    FILE *f = fopen(path, "wb");
    unsigned frame;

    if (!f || record_len % 4u != 0)
        return -1;

    fwrite("SMK4", 1, 4, f);
    tb_write_le32(f, 4);
    tb_write_le32(f, 8);
    tb_write_le32(f, frames);
    tb_write_le32(f, (uint32_t)-3333);
    tb_write_le32(f, 0);

    for (frame = 0; frame < 7; ++frame)
        tb_write_le32(f, frame == 0 ? record_len : 0);

    tb_write_le32(f, tree_size);
    tb_write_le32(f, 16);
    tb_write_le32(f, 16);
    tb_write_le32(f, 16);
    tb_write_le32(f, 24);

    for (frame = 0; frame < 7; ++frame) {
        if (frame == 0)
            tb_write_le32(f, UINT32_C(0x40000000) | 1000u);
        else
            tb_write_le32(f, 0);
    }
    tb_write_le32(f, 0);

    for (frame = 0; frame < frames; ++frame) {
        uint32_t chunk_size = record_len + 8u + (frame == 0 ? 12u : 0u);
        tb_write_le32(f, chunk_size | 1u);
    }
    for (frame = 0; frame < frames; ++frame)
        fputc(frame == 0 ? 0x03 : 0x02, f);

    fwrite(trees.bytes, 1, tree_size, f);

    for (frame = 0; frame < frames; ++frame) {
        unsigned char pal[12];
        unsigned k;
        static const unsigned char red[3] = {63, 0, 0};
        static const unsigned char white[3] = {63, 63, 63};

        if (frame == 0) {
            tb_palette12(pal, red, white);
            fwrite(pal, 1, sizeof pal, f);
        }

        tb_write_le32(f, record_len + 4u);
        for (k = 0; k < record_len; ++k) {
            unsigned char sample =
                (unsigned char)((frame * 131u + k * 7u + 3u) & 0xffu);
            fputc(sample, f);
        }
        fputc(0x02, f); /* top index 7, bottom index 8 */
        fputc(0, f);
        fputc(0, f);
        fputc(0, f);
    }

    if (ferror(f) || fclose(f) != 0)
        return -1;
    return 0;
}

static int case_streaming_long(void)
{
    enum { RECORD_LEN = 40, SHORT_FRAMES = 200, LONG_FRAMES = 2000 };
    char short_smk[1024], short_avi[1024];
    char long_smk[1024], long_avi[1024];
    unsigned long peak_short, peak_long;
    unsigned long long streamed_short, streamed_long;
    struct rr_avi avi;
    unsigned f;

    test_path(short_smk, sizeof short_smk, "task_long_short.smk");
    test_path(short_avi, sizeof short_avi, "task_long_short.avi");
    test_path(long_smk, sizeof long_smk, "task_long.smk");
    test_path(long_avi, sizeof long_avi, "task_long.avi");
    remove(short_smk); remove(short_avi);
    remove(long_smk); remove(long_avi);

    TASK_CHECK(write_long_smk(short_smk, SHORT_FRAMES, RECORD_LEN) == 0);
    TASK_CHECK(smk_export_avi(short_smk, short_avi, 1) == 0);
    peak_short = smk_export_debug_pcm_resident_peak();
    streamed_short = smk_export_debug_pcm_streamed_bytes();
    TASK_CHECK(peak_short == RECORD_LEN);
    TASK_CHECK(streamed_short == 6666u);

    TASK_CHECK(write_long_smk(long_smk, LONG_FRAMES, RECORD_LEN) == 0);
    TASK_CHECK(smk_export_avi(long_smk, long_avi, 1) == 0);
    peak_long = smk_export_debug_pcm_resident_peak();
    streamed_long = smk_export_debug_pcm_streamed_bytes();

    /* Ten times the duration must not enlarge resident PCM state. */
    TASK_CHECK(peak_long == peak_short);
    TASK_CHECK(peak_long <= RECORD_LEN);
    TASK_CHECK(streamed_long == streamed_short * 10u);

    TASK_CHECK(rr_load(long_avi, &avi) == 0);
    TASK_CHECK(avi.strl_count == 2);
    TASK_CHECK(avi.media_count == 2u * LONG_FRAMES);
    TASK_CHECK(avi.index_count == 2u * LONG_FRAMES);
    TASK_CHECK(check_index_targets(&avi) == 0);

    for (f = 0; f < LONG_FRAMES; ++f) {
        const struct rr_chunk *v = &avi.media[2u * f];
        const struct rr_chunk *a = &avi.media[2u * f + 1u];
        uint64_t start = (uint64_t)f * 1000u * 33330ull / 1000000ull;
        uint64_t samples = expected_count(f, 1000, 33330);
        uint64_t k;

        TASK_CHECK(memcmp(v->id, "00db", 4) == 0);
        TASK_CHECK(v->size == 96);
        TASK_CHECK(memcmp(a->id, "01wb", 4) == 0);
        TASK_CHECK(a->size == samples);

        for (k = 0; k < samples; ++k) {
            uint64_t global_sample = start + k;
            unsigned source_frame = (unsigned)(global_sample / RECORD_LEN);
            unsigned offset = (unsigned)(global_sample % RECORD_LEN);
            unsigned char wanted = (unsigned char)(
                (source_frame * 131u + offset * 7u + 3u) & 0xffu);
            TASK_CHECK(avi.data[a->data_at + (size_t)k] == wanted);
        }
    }

    rr_free(&avi);
    remove(short_smk); remove(short_avi);
    remove(long_smk); remove(long_avi);
    return 0;
}

const struct task_case task_cases[] = {
    {"pcm", "front_loaded_empty_cross_record_tails", case_pcm_combo},
    {"layout", "dib_palette_riff_idx", case_pcm_combo},
    {"layout", "smk2_version", case_smk2_version},
    {"layout", "bottom_up_orientation", case_layout_orientation},
    {"layout", "no_audio_only_video", case_layout_no_audio},
    {"layout", "unsupported_and_output_cleanup", case_failure_cleanup},
    {"streaming", "long_film_constant_pcm_residency", case_streaming_long}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);


