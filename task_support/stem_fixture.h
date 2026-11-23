#ifndef STEM_FRAME_FIXTURE_H
#define STEM_FRAME_FIXTURE_H
/*
Synthetic MPEG-1 Layer III frame generator for the dual-subband stem tests.

Everything here is input construction only: no downloaded audio and no frozen decoder
output. A stream is a sequence of independent frames described by per-granule block
layout and joint-stereo settings, with dense Huffman content in both the low and high
subband regions, an explicit bit-reservoir back pointer and controllable
padding/bitrate so the decoder exercises reservoir, VBR, short/mixed blocks and
joint stereo.
*/
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STEM_FIX_MAX_FRAME_BYTES   2400

typedef struct
{
    unsigned block_type;     /* 0 long, 1 start, 2 short, 3 stop. */
    int      mixed;          /* Mixed block flag, meaningful for short blocks. */
    unsigned table1_pairs[2]; /* Huffman table-1 big-value pairs per channel. */
    unsigned sign_seed;      /* Varies coefficient signs between granules. */
} stem_fix_gr;

typedef struct
{
    unsigned rate_index;    /* 0 = 44100, 1 = 48000, 2 = 32000 (fixed within a stream). */
    unsigned bitrate_index; /* MPEG-1 Layer III bitrate index 1..14. */
    unsigned channels;      /* 1 or 2. */
    unsigned padding;       /* MP3 padding slot. */
    int      crc_on;
    unsigned main_data_begin;  /* Reservoir back pointer in bytes. */
    int      ms_stereo;        /* M/S joint stereo (stereo only). */
    int      i_stereo;         /* Intensity joint stereo (stereo only). */
    stem_fix_gr gr[2];
} stem_fix_desc;

static const unsigned stem_fix_rates[3] = {44100, 48000, 32000};
static const unsigned stem_fix_bitrates[15] =
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};

/* Physical frame slot for a descriptor, including padding. */
static unsigned stem_fix_frame_size(const stem_fix_desc *d)
{
    return 144u * stem_fix_bitrates[d->bitrate_index] * 1000u
        / stem_fix_rates[d->rate_index] + d->padding;
}

static void stem_fix_bits(unsigned char *out, unsigned *pos, unsigned value, unsigned count)
{
    while (count)
    {
        --count;
        if ((value >> count) & 1u)
        {
            out[*pos / 8] |= (unsigned char)(1u << (7 - *pos % 8));
        }
        ++*pos;
    }
}

/* Side-info size for an MPEG-1 Layer III frame. */
static unsigned stem_fix_side_bytes(unsigned channels, int crc_on)
{
    /* 4 header bytes; mono side info 17 bytes, stereo 32; CRC adds 2. */
    return (channels == 1 ? 17u : 32u) + (crc_on ? 2u : 0u);
}

static unsigned stem_fix_crc_bit(unsigned crc, unsigned bit)
{
    unsigned msb = (crc >> 15) ^ bit;
    return ((crc << 1) ^ ((msb & 1) ? 0x8005u : 0u)) & 65535u;
}

/*
Build one frame. Returns its byte size, or 0 on an impossible descriptor. The caller
must ensure main_data_begin bytes of reservoir are available; the generator simply
records the value, so a stream with a back pointer is produced by prefixing enough
earlier frames (the tests arrange them in order with non-empty reservoirs).
*/
static size_t stem_fix_frame(unsigned char *out, const stem_fix_desc *d)
{
    unsigned side, pos, end, frame_size, gr, ch, g;
    uint32_t head;
    unsigned mode_ext;

    if (d->rate_index > 2 || (d->channels != 1 && d->channels != 2) ||
        d->bitrate_index < 1 || d->bitrate_index > 14)
    {
        return 0;
    }
    for (g = 0; g < 2; g++)
    {
        if (d->gr[g].block_type > 3)
        {
            return 0;
        }
    }

    frame_size = stem_fix_frame_size(d);
    if (frame_size > STEM_FIX_MAX_FRAME_BYTES)
    {
        return 0;
    }
    memset(out, 0, frame_size);

    /* Header. mode 0b11 = mono, 0b01 = joint stereo. */
    head = 0xfffb0000u | (d->bitrate_index << 12) | (d->rate_index << 10) | (d->padding << 9);
    if (d->channels == 1)
    {
        head |= (3u << 6);
    } else
    {
        mode_ext = (d->i_stereo ? 1u : 0u) | (d->ms_stereo ? 2u : 0u);
        head |= (1u << 6) | (mode_ext << 4);
    }
    if (d->crc_on)
    {
        head &= ~(1u << 16);
    }
    for (g = 0; g < 4; g++)
    {
        out[g] = (unsigned char)(head >> (24 - 8 * g));
    }

    side = 4 + (d->crc_on ? 2u : 0u);
    pos = side * 8;

    /* Side info. */
    stem_fix_bits(out, &pos, d->main_data_begin, 9);
    stem_fix_bits(out, &pos, 0, d->channels == 1 ? 5 : 3);  /* private/unused. */
    for (ch = 0; ch < d->channels; ch++)
    {
        stem_fix_bits(out, &pos, 0, 4);  /* scfsi. */
    }

    for (gr = 0; gr < 2; gr++)
    {
        for (ch = 0; ch < d->channels; ch++)
        {
            const stem_fix_gr *gd = &d->gr[gr];
            unsigned pairs = gd->table1_pairs[ch];
            unsigned part23;

            if (pairs > 288)
            {
                pairs = 288;
            }
            /* Each table-1 pair is one 2-bit code + one 1-bit sign. */
            part23 = pairs * 3;

            stem_fix_bits(out, &pos, part23, 12);            /* part2_3_length. */
            stem_fix_bits(out, &pos, pairs, 9);              /* big_values. */
            stem_fix_bits(out, &pos, 180, 8);                /* global_gain. */
            stem_fix_bits(out, &pos, 0, 4);                  /* scalefac_compress. */

            if (gd->block_type == 0)
            {
                stem_fix_bits(out, &pos, 0, 1);              /* window switching off. */
                /* table_select[0]=1 so the big-value pairs (table-1 codes) land in the
                   low subband region and energize the low stem. Region 0 spans the first
                   16 long scalefactor bands (~284 lines); regions 1/2 use table 0. */
                stem_fix_bits(out, &pos, 1 << 10, 15);       /* tables. */
                stem_fix_bits(out, &pos, 16, 4);             /* region0 count. */
                stem_fix_bits(out, &pos, 5, 3);              /* region1 count. */
            } else
            {
                stem_fix_bits(out, &pos, 1, 1);              /* window switching on. */
                stem_fix_bits(out, &pos, gd->block_type, 2);
                stem_fix_bits(out, &pos, gd->mixed ? 1u : 0u, 1);
                stem_fix_bits(out, &pos, 0, 10);             /* tables: all table 1. */
                stem_fix_bits(out, &pos, 0, 3);              /* subblock gain 0. */
                stem_fix_bits(out, &pos, 0, 3);              /* subblock gain 1. */
                stem_fix_bits(out, &pos, 0, 3);              /* subblock gain 2. */
            }
            stem_fix_bits(out, &pos, 0, 1);                  /* preflag. */
            stem_fix_bits(out, &pos, 0, 1);                  /* scalefac_scale. */
            stem_fix_bits(out, &pos, 0, 1);                  /* count1_table. */
        }
    }

    end = side + (d->channels == 1 ? 17u : 32u);
    if (pos != end * 8)
    {
        return 0;
    }

    /* Main data: table-1 big-value pairs with alternating signs. Table 1 encodes
       (1,0) as the 2-bit code 0b01, followed by one sign bit for the non-zero value.
       The pairs land in increasing subbands, so dense pairs energize both the low and
       high subband groups. */
    for (gr = 0; gr < 2; gr++)
    {
        for (ch = 0; ch < d->channels; ch++)
        {
            unsigned pairs = d->gr[gr].table1_pairs[ch];
            unsigned k;
            if (pairs > 288)
            {
                pairs = 288;
            }
            for (k = 0; k < pairs; k++)
            {
                stem_fix_bits(out, &pos, 1, 2);  /* 01 => (1,0). */
                stem_fix_bits(out, &pos, (d->gr[gr].sign_seed + gr + ch + k) & 1u, 1);
            }
        }
    }

    if (d->crc_on)
    {
        unsigned crc = 65535, i;
        for (i = 16; i < 32; i++)
        {
            crc = stem_fix_crc_bit(crc, (head >> (31 - i)) & 1u);
        }
        for (i = side * 8; i < end * 8; i++)
        {
            crc = stem_fix_crc_bit(crc, (out[i / 8] >> (7 - i % 8)) & 1u);
        }
        out[4] = (unsigned char)(crc >> 8);
        out[5] = (unsigned char)crc;
    }

    return frame_size;
}

/* Convenience: a long-block descriptor with dense content in both bands. */
static stem_fix_desc stem_fix_long(unsigned rate_index, unsigned channels,
                                    unsigned padding, int crc_on, unsigned pairs,
                                    int ms, int ist, unsigned seed)
{
    stem_fix_desc d;
    memset(&d, 0, sizeof(d));
    d.rate_index = rate_index;
    d.bitrate_index = 9;  /* 128 kbps. */
    d.channels = channels;
    d.padding = padding;
    d.crc_on = crc_on;
    d.main_data_begin = 0;
    d.ms_stereo = ms;
    d.i_stereo = ist;
    d.gr[0].block_type = 0; d.gr[0].table1_pairs[0] = pairs; d.gr[0].table1_pairs[1] = pairs; d.gr[0].sign_seed = seed;
    d.gr[1].block_type = 0; d.gr[1].table1_pairs[0] = pairs; d.gr[1].table1_pairs[1] = pairs; d.gr[1].sign_seed = seed + 1;
    return d;
}

/*
Stream builder with genuine bit-reservoir back references.

Each item is one frame descriptor plus a `backref` flag. A backref frame's Huffman
main data is written into the TAIL of the previous frame's slot and its
main_data_begin points back to it; this is exactly the MP3 reservoir mechanism. The
previous frame must leave enough reservoir bytes. Backref main data is kept byte
aligned (the builder pads the prior frame's own data to a byte boundary), so the
back pointer is an exact byte count.
*/
typedef struct
{
    stem_fix_desc desc;
    int backref;       /* 1: this frame's main data lives in the previous frame's tail. */
} stem_fix_item;

/* Total main-data bit length of a descriptor (2 granules, per channel). */
static unsigned stem_fix_md_bits(const stem_fix_desc *d)
{
    unsigned bits = 0, gr, ch;
    for (gr = 0; gr < 2; gr++)
    {
        for (ch = 0; ch < d->channels; ch++)
        {
            unsigned pairs = d->gr[gr].table1_pairs[ch];
            if (pairs > 288) pairs = 288;
            bits += pairs * 3;
        }
    }
    return bits;
}

/* Write a descriptor's Huffman main data at an explicit bit position. */
static void stem_fix_write_md(unsigned char *out, unsigned *pos, const stem_fix_desc *d)
{
    unsigned gr, ch, k;
    for (gr = 0; gr < 2; gr++)
    {
        for (ch = 0; ch < d->channels; ch++)
        {
            unsigned pairs = d->gr[gr].table1_pairs[ch];
            if (pairs > 288) pairs = 288;
            for (k = 0; k < pairs; k++)
            {
                stem_fix_bits(out, pos, 1, 2);  /* 01 => (1,0). */
                stem_fix_bits(out, pos, (d->gr[gr].sign_seed + gr + ch + k) & 1u, 1);
            }
        }
    }
}

/*
Build a whole stream. Returns total bytes (0 on error). Each frame's fixed slot is
derived from its rate/padding. The caller provides a scratch array of per-frame slot
boundaries internally; out must hold at least count * STEM_FIX_MAX_FRAME_BYTES.
*/
static size_t stem_fix_build(unsigned char *out, const stem_fix_item *items, size_t count)
{
    size_t total = 0;
    size_t i;

    for (i = 0; i < count; i++)
    {
        const stem_fix_desc *d = &items[i].desc;
        size_t slot = stem_fix_frame_size(d);
        unsigned side, pos, end;
        size_t frame_start = total;
        unsigned md_bits, md_bytes;

        if (slot > STEM_FIX_MAX_FRAME_BYTES) return 0;
        memset(out + frame_start, 0, slot);

        /* A backref frame needs a preceding frame. */
        if (items[i].backref && i == 0) return 0;

        side = 4u + (d->crc_on ? 2u : 0u);
        md_bits = stem_fix_md_bits(d);
        md_bytes = (md_bits + 7u) / 8u;

        {
            uint32_t head;
            unsigned mode_ext, g, ch, gr;

            head = 0xfffb0000u | (d->bitrate_index << 12) | (d->rate_index << 10) | (d->padding << 9);
            if (d->channels == 1)
            {
                head |= (3u << 6);
            } else
            {
                mode_ext = (d->i_stereo ? 1u : 0u) | (d->ms_stereo ? 2u : 0u);
                head |= (1u << 6) | (mode_ext << 4);
            }
            if (d->crc_on) head &= ~(1u << 16);
            for (g = 0; g < 4; g++)
            {
                out[frame_start + g] = (unsigned char)(head >> (24 - 8 * g));
            }

            pos = side * 8;
            stem_fix_bits(out + frame_start, &pos,
                items[i].backref ? md_bytes : d->main_data_begin, 9);
            stem_fix_bits(out + frame_start, &pos, 0, d->channels == 1 ? 5 : 3);
            for (ch = 0; ch < d->channels; ch++) stem_fix_bits(out + frame_start, &pos, 0, 4);

            for (gr = 0; gr < 2; gr++)
            {
                for (ch = 0; ch < d->channels; ch++)
                {
                    const stem_fix_gr *gd = &d->gr[gr];
                    unsigned pairs = gd->table1_pairs[ch];
                    unsigned part23;
                    if (pairs > 288) pairs = 288;
                    part23 = pairs * 3;

                    stem_fix_bits(out + frame_start, &pos, part23, 12);
                    stem_fix_bits(out + frame_start, &pos, pairs, 9);
                    stem_fix_bits(out + frame_start, &pos, 180, 8);
                    stem_fix_bits(out + frame_start, &pos, 0, 4);
                    if (gd->block_type == 0)
                    {
                        stem_fix_bits(out + frame_start, &pos, 0, 1);
                        /* table_select[0]=1 energizes the low subband big-value region. */
                        stem_fix_bits(out + frame_start, &pos, 1 << 10, 15);
                        stem_fix_bits(out + frame_start, &pos, 16, 4);
                        stem_fix_bits(out + frame_start, &pos, 5, 3);
                    } else
                    {
                        stem_fix_bits(out + frame_start, &pos, 1, 1);
                        stem_fix_bits(out + frame_start, &pos, gd->block_type, 2);
                        stem_fix_bits(out + frame_start, &pos, gd->mixed ? 1u : 0u, 1);
                        stem_fix_bits(out + frame_start, &pos, 0, 10);
                        stem_fix_bits(out + frame_start, &pos, 0, 3);
                        stem_fix_bits(out + frame_start, &pos, 0, 3);
                        stem_fix_bits(out + frame_start, &pos, 0, 3);
                    }
                    stem_fix_bits(out + frame_start, &pos, 0, 1);
                    stem_fix_bits(out + frame_start, &pos, 0, 1);
                    stem_fix_bits(out + frame_start, &pos, 0, 1);
                }
            }
            end = side + (d->channels == 1 ? 17u : 32u);
            if (pos != end * 8) return 0;

            if (items[i].backref)
            {
                /* Main data was placed in the previous frame's tail; nothing to write
                   in this slot's payload. */
            } else
            {
                unsigned md_pos = end * 8;
                stem_fix_write_md(out + frame_start, &md_pos, d);
            }

            if (d->crc_on)
            {
                unsigned crc = 65535, b;
                for (b = 16; b < 32; b++)
                    crc = stem_fix_crc_bit(crc, (head >> (31 - b)) & 1u);
                for (b = side * 8; b < end * 8; b++)
                    crc = stem_fix_crc_bit(crc, (out[frame_start + b / 8] >> (7 - b % 8)) & 1u);
                out[frame_start + 4] = (unsigned char)(crc >> 8);
                out[frame_start + 5] = (unsigned char)crc;
            }
        }

        /* Place a backref frame's main data in the tail of the previous slot. The
           previous frame's own main data sits at the head of its payload, so a large
           enough slot leaves these tail bytes in the bit reservoir unconsumed. */
        if (items[i].backref)
        {
            size_t prev = 0, j;
            size_t prev_slot;
            unsigned tail_pos;
            for (j = 0; j < i; j++)
            {
                prev += stem_fix_frame_size(&items[j].desc);
            }
            prev_slot = stem_fix_frame_size(&items[i - 1].desc);
            if (md_bytes > prev_slot - side) return 0;
            tail_pos = (unsigned)((prev_slot - md_bytes) * 8);
            stem_fix_write_md(out + prev, &tail_pos, d);
        }

        total += slot;
    }

    return total;
}

#endif