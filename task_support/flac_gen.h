/*
flac_gen.h - A small, auditable native FLAC stream generator for timeline regression tests.

This is deliberately not an encoder: every frame uses SUBFRAME_VERBATIM, so the bytes on disk are exactly the samples handed in,
prefixed by a hand-built frame header and followed by a real CRC-8 and CRC-16. Everything is written MSB-first through one bit
writer, and both CRCs use the FLAC polynomials (CRC-8 poly 0x07, CRC-16 poly 0x8005), which makes each produced stream independently
reviewable against the FLAC framing rules without any external encoder.

Only the subset exercised by the tests is supported:
  * native FLAC with a single STREAMINFO metadata block;
  * 16-bit samples, 1..8 independent (non-side) channels;
  * frame and sample coordinates below 127, so frame numbers / absolute sample numbers encode as a single UTF-8 byte;
  * fixed block size streams (block size code 6, 8-bit size-1) and variable block size streams (code 7, 16-bit size-1);
  * verbatim subframes, which are byte aligned, so no frame padding bits are required.
*/
#ifndef FLAC_GEN_H
#define FLAC_GEN_H

#include <stddef.h>
#include <string.h>

#define FLAC_GEN_MAX_BYTES 65536u

typedef struct
{
    unsigned char data[FLAC_GEN_MAX_BYTES];
    unsigned int bitPos;
} flac_gen_bw;

/* Writes nbits (1..64) of value, most significant bit first. */
static void flac_gen_put(flac_gen_bw* bw, unsigned long long value, unsigned int nbits)
{
    while (nbits > 0) {
        nbits -= 1;
        bw->data[bw->bitPos >> 3] |= (unsigned char)(((value >> nbits) & 1ull) << (7u - (bw->bitPos & 7u)));
        bw->bitPos += 1;
    }
}

static unsigned int flac_gen_byte_pos(const flac_gen_bw* bw)
{
    return bw->bitPos / 8u;
}

/* FLAC CRC-8, polynomial 0x07, init 0. The running value is held in the top 8 bits of a 32-bit accumulator. */
static unsigned int flac_gen_crc8(const unsigned char* p, unsigned int n)
{
    unsigned int crc = 0;
    unsigned int i;
    for (i = 0; i < n; ++i) {
        unsigned int bit;
        crc ^= (unsigned int)p[i] << 24;
        for (bit = 0; bit < 8; ++bit) {
            if (crc & 0x80000000u) {
                crc = (crc << 1) ^ 0x07000000u;
            } else {
                crc <<= 1;
            }
        }
    }
    return (crc >> 24) & 0xFFu;
}

/* FLAC CRC-16, polynomial 0x8005, init 0. */
static unsigned int flac_gen_crc16(const unsigned char* p, unsigned int n)
{
    unsigned int crc = 0;
    unsigned int i;
    for (i = 0; i < n; ++i) {
        unsigned int bit;
        crc ^= (unsigned int)p[i] << 8;
        for (bit = 0; bit < 8; ++bit) {
            crc <<= 1;
            if (crc & 0x10000u) {
                crc ^= 0x8005u;
            }
        }
    }
    return crc & 0xFFFFu;
}

/* Deterministic, always non-zero (for the tested range) sample value for global frame f and channel c. */
static int flac_gen_sample(unsigned long long f, int c)
{
    int v = (int)(((f * 11ull) + (unsigned int)(c * 137) + 53ull) % 9000ull) - 4500;
    return (v == 0) ? 1 : v;
}

/* Writes the "fLaC" marker and one last STREAMINFO block. totalSamples == 0 records an unknown total length on purpose. */
static void flac_gen_begin_stream(flac_gen_bw* bw, int channels, int totalSamples, int variable, int minBlock, int maxBlock)
{
    memset(bw->data, 0, sizeof(bw->data));
    bw->bitPos = 0;

    flac_gen_put(bw, 0x664C6143ull, 32);  /* "fLaC" */
    flac_gen_put(bw, 1, 1);               /* last metadata block flag */
    flac_gen_put(bw, 0, 7);               /* block type 0 = STREAMINFO */
    flac_gen_put(bw, 34, 24);             /* STREAMINFO length */

    flac_gen_put(bw, (unsigned long long)minBlock, 16);
    flac_gen_put(bw, (unsigned long long)maxBlock, 16);
    flac_gen_put(bw, 0, 24);              /* min frame size: unknown */
    flac_gen_put(bw, 0, 24);              /* max frame size: unknown */
    flac_gen_put(bw, 44100, 20);          /* sample rate */
    flac_gen_put(bw, (unsigned long long)(channels - 1), 3);
    flac_gen_put(bw, 15, 5);              /* bits per sample - 1 = 16 bit */
    flac_gen_put(bw, (unsigned long long)totalSamples, 36);
    flac_gen_put(bw, 0, 64);              /* MD5 signature, unused */
    flac_gen_put(bw, 0, 64);
    (void)variable;
}

/*
Starts a frame and returns the byte offset of the sync code. coord is the frame number for fixed block size streams and the
absolute sample number for variable block size streams; it must be below 127 so the UTF-8 coded number is a single byte.
*/
static unsigned int flac_gen_begin_frame(flac_gen_bw* bw, int variable, int channels, int blockSize, unsigned long long coord)
{
    unsigned int start = flac_gen_byte_pos(bw);
    int blockSizeCode = (blockSize <= 256) ? 6 : 7;
    unsigned int headerCrc;

    flac_gen_put(bw, 0x3FFEull, 14);              /* sync code */
    flac_gen_put(bw, 0, 1);                       /* reserved */
    flac_gen_put(bw, variable ? 1ull : 0ull, 1);  /* blocking strategy */
    flac_gen_put(bw, (unsigned long long)blockSizeCode, 4);
    flac_gen_put(bw, 9, 4);                       /* sample rate table: 44100 */
    flac_gen_put(bw, (unsigned long long)(channels - 1), 4);  /* independent channels */
    flac_gen_put(bw, 4, 3);                       /* 16 bits per sample */
    flac_gen_put(bw, 0, 1);                       /* reserved */
    flac_gen_put(bw, coord, 8);                   /* single-byte UTF-8 coded number */

    if (blockSizeCode == 6) {
        flac_gen_put(bw, (unsigned long long)(blockSize - 1), 8);
    } else {
        flac_gen_put(bw, (unsigned long long)(blockSize - 1), 16);
    }

    headerCrc = flac_gen_crc8(bw->data + start, flac_gen_byte_pos(bw) - start);
    flac_gen_put(bw, headerCrc, 8);
    return start;
}

/* Writes one verbatim subframe per channel using flac_gen_sample() coordinates. */
static void flac_gen_put_verbatim(flac_gen_bw* bw, int channels, int blockSize, unsigned long long firstSample)
{
    int c;
    for (c = 0; c < channels; ++c) {
        int i;
        flac_gen_put(bw, 0x02, 8);   /* zero wasted-bits flag, subframe type 000010 = VERBATIM */
        for (i = 0; i < blockSize; ++i) {
            flac_gen_put(bw, (unsigned long long)(unsigned short)flac_gen_sample(firstSample + (unsigned long long)i, c), 16);
        }
    }
}

/* Appends the frame CRC-16 and returns the byte just past the frame. */
static unsigned int flac_gen_end_frame(flac_gen_bw* bw, unsigned int start)
{
    unsigned int crc = flac_gen_crc16(bw->data + start, flac_gen_byte_pos(bw) - start);
    flac_gen_put(bw, crc, 16);
    return flac_gen_byte_pos(bw);
}

/* Records the [start,end) byte span of every frame so tests can corrupt or delete exactly one frame. */
typedef struct
{
    unsigned int start;
    unsigned int end;
    unsigned long long firstSample;
    int blockSize;
} flac_gen_frame_span;

/* FLAC frame-header bits-per-sample table code: index 4 = 16 bit, index 7 = 32 bit. */
static int flac_gen_bps_code(int bitsPerSample)
{
    if (bitsPerSample == 32) return 7;
    return 4;   /* 16 bit */
}

/* 32-bit variant of flac_gen_begin_stream: writes the chosen bits-per-sample code into STREAMINFO. */
static void flac_gen_begin_stream_bps(flac_gen_bw* bw, int channels, int totalSamples, int minBlock, int maxBlock, int bitsPerSample)
{
    memset(bw->data, 0, sizeof(bw->data));
    bw->bitPos = 0;

    flac_gen_put(bw, 0x664C6143ull, 32);  /* "fLaC" */
    flac_gen_put(bw, 1, 1);               /* last metadata block flag */
    flac_gen_put(bw, 0, 7);               /* block type 0 = STREAMINFO */
    flac_gen_put(bw, 34, 24);             /* STREAMINFO length */

    flac_gen_put(bw, (unsigned long long)minBlock, 16);
    flac_gen_put(bw, (unsigned long long)maxBlock, 16);
    flac_gen_put(bw, 0, 24);              /* min frame size: unknown */
    flac_gen_put(bw, 0, 24);              /* max frame size: unknown */
    flac_gen_put(bw, 44100, 20);          /* sample rate */
    flac_gen_put(bw, (unsigned long long)(channels - 1), 3);
    flac_gen_put(bw, (unsigned long long)(bitsPerSample - 1), 5);
    flac_gen_put(bw, (unsigned long long)totalSamples, 36);
    flac_gen_put(bw, 0, 64);              /* MD5 signature, unused */
    flac_gen_put(bw, 0, 64);
}

/* 32-bit variant of flac_gen_begin_frame. */
static unsigned int flac_gen_begin_frame_bps(flac_gen_bw* bw, int variable, int channels, int blockSize,
                                             unsigned long long coord, int bitsPerSample)
{
    unsigned int start = flac_gen_byte_pos(bw);
    int blockSizeCode = (blockSize <= 256) ? 6 : 7;
    unsigned int headerCrc;

    flac_gen_put(bw, 0x3FFEull, 14);              /* sync code */
    flac_gen_put(bw, 0, 1);                       /* reserved */
    flac_gen_put(bw, variable ? 1ull : 0ull, 1);  /* blocking strategy */
    flac_gen_put(bw, (unsigned long long)blockSizeCode, 4);
    flac_gen_put(bw, 9, 4);                       /* sample rate table: 44100 */
    flac_gen_put(bw, (unsigned long long)(channels - 1), 4);  /* independent channels */
    flac_gen_put(bw, (unsigned long long)flac_gen_bps_code(bitsPerSample), 3);
    flac_gen_put(bw, 0, 1);                       /* reserved */
    flac_gen_put(bw, coord, 8);                   /* single-byte UTF-8 coded number */

    if (blockSizeCode == 6) {
        flac_gen_put(bw, (unsigned long long)(blockSize - 1), 8);
    } else {
        flac_gen_put(bw, (unsigned long long)(blockSize - 1), 16);
    }

    headerCrc = flac_gen_crc8(bw->data + start, flac_gen_byte_pos(bw) - start);
    flac_gen_put(bw, headerCrc, 8);
    return start;
}

/* Signed sample generator for 32-bit streams, kept small and never zero so silence remains distinguishable. */
static long long flac_gen_sample32(unsigned long long f, int c)
{
    long long v = (long long)(((f * 65537ull) + (unsigned int)(c * 7919ull) + 12345ull) % 2000000000ull) - 1000000000ll;
    return (v == 0) ? 1 : v;
}

/* Verbatim subframes at the chosen bit depth (16 or 32). */
static void flac_gen_put_verbatim_bps(flac_gen_bw* bw, int channels, int blockSize,
                                      unsigned long long firstSample, int bitsPerSample)
{
    int c;
    for (c = 0; c < channels; ++c) {
        int i;
        flac_gen_put(bw, 0x02, 8);   /* zero wasted-bits flag, subframe type 000010 = VERBATIM */
        for (i = 0; i < blockSize; ++i) {
            if (bitsPerSample == 32) {
                flac_gen_put(bw, (unsigned long long)flac_gen_sample32(firstSample + (unsigned long long)i, c), 32);
            } else {
                flac_gen_put(bw, (unsigned long long)(unsigned short)flac_gen_sample(firstSample + (unsigned long long)i, c), 16);
            }
        }
    }
}

/*
Builds a stream at 16 or 32 bits per sample. Same coordinate rules as flac_gen_build(); the 32-bit form exercises the decoder's
64-bit decoded-sample storage (pS64) and the s64 output conversion paths.
*/
static unsigned int flac_gen_build_bps(flac_gen_bw* bw, int variable, int channels, const int* blockSizes,
                                       const unsigned long long* coords, int frameCount, int totalSamples,
                                       int bitsPerSample, flac_gen_frame_span* spans)
{
    int minBlock = 65535;
    int maxBlock = 0;
    int k;
    unsigned long long running = 0;

    for (k = 0; k < frameCount; ++k) {
        if (blockSizes[k] < minBlock) minBlock = blockSizes[k];
        if (blockSizes[k] > maxBlock) maxBlock = blockSizes[k];
    }

    flac_gen_begin_stream_bps(bw, channels, totalSamples, minBlock, maxBlock, bitsPerSample);
    for (k = 0; k < frameCount; ++k) {
        unsigned int start = flac_gen_begin_frame_bps(bw, variable, channels, blockSizes[k], coords[k], bitsPerSample);
        flac_gen_put_verbatim_bps(bw, channels, blockSizes[k], running, bitsPerSample);
        spans[k].start = start;
        spans[k].end = flac_gen_end_frame(bw, start);
        spans[k].firstSample = running;
        spans[k].blockSize = blockSizes[k];
        running += (unsigned long long)blockSizes[k];
    }

    return flac_gen_byte_pos(bw);
}

/*
Builds a complete stream from per-frame block sizes, header coordinates and explicit payload sample coordinates. payloadStart[k]
is the global sample index used to generate frame k's verbatim samples, independent of the header coordinate; this lets a test
fabricate a frame whose declared position moves backwards or overlaps another while its audio still decodes. For well-formed
streams pass payloadStart[k] = coords[k] (fixed) or the running sample offset (variable).
*/
static unsigned int flac_gen_build_ex(flac_gen_bw* bw, int variable, int channels, const int* blockSizes,
                                      const unsigned long long* coords, const unsigned long long* payloadStart,
                                      int frameCount, int totalSamples, flac_gen_frame_span* spans)
{
    int minBlock = 65535;
    int maxBlock = 0;
    int k;

    for (k = 0; k < frameCount; ++k) {
        if (blockSizes[k] < minBlock) minBlock = blockSizes[k];
        if (blockSizes[k] > maxBlock) maxBlock = blockSizes[k];
    }

    flac_gen_begin_stream(bw, channels, totalSamples, variable, minBlock, maxBlock);
    for (k = 0; k < frameCount; ++k) {
        unsigned int start = flac_gen_begin_frame(bw, variable, channels, blockSizes[k], coords[k]);
        flac_gen_put_verbatim(bw, channels, blockSizes[k], payloadStart[k]);
        spans[k].start = start;
        spans[k].end = flac_gen_end_frame(bw, start);
        spans[k].firstSample = payloadStart[k];
        spans[k].blockSize = blockSizes[k];
    }

    return flac_gen_byte_pos(bw);
}

/*
Builds a complete stream from per-frame block sizes and coordinates. For fixed block size streams pass variable=0 and coord[k]=k;
for variable block size streams pass variable=1 and coord[k]=absolute first sample. Returns the total stream size in bytes.
*/
static unsigned int flac_gen_build(flac_gen_bw* bw, int variable, int channels, const int* blockSizes,
                                   const unsigned long long* coords, int frameCount, int totalSamples,
                                   flac_gen_frame_span* spans)
{
    unsigned long long payloadStart[64];
    unsigned long long running = 0;
    int k;

    for (k = 0; k < frameCount; ++k) {
        payloadStart[k] = running;
        running += (unsigned long long)blockSizes[k];
    }

    return flac_gen_build_ex(bw, variable, channels, blockSizes, coords, payloadStart, frameCount, totalSamples, spans);
}

/* Flips one bit in the frame's CRC-16 (its final byte), breaking only the frame checksum. */
static void flac_gen_corrupt_frame_crc16(flac_gen_bw* bw, const flac_gen_frame_span* span)
{
    bw->data[span->end - 1] ^= 0x01u;
}

/*
Rewrites two payload sample bytes of a frame into an 0xFF 0xF8 pseudo sync pattern while leaving its frame header and both CRC
bytes untouched. The verbatim subframes still decode, so the only possible failure is the frame CRC-16: a sync code plus a correct
header CRC-8 must never be accepted on their own.
*/
static void flac_gen_make_pseudo_sync_frame(flac_gen_bw* bw, const flac_gen_frame_span* span)
{
    unsigned int headerBytes = (span->blockSize <= 256) ? 7u : 8u;  /* code 6 vs code 7 frame header, including CRC-8 */
    unsigned int firstSampleByte = span->start + headerBytes + 1u;  /* skip the subframe type byte */
    if (firstSampleByte + 1u < span->end - 2u) {
        bw->data[firstSampleByte] = 0xFFu;
        bw->data[firstSampleByte + 1u] = 0xF8u;
    }
}

/* Removes exactly one frame's bytes and closes the gap, simulating a fully missing frame. Returns the new stream size. */
static unsigned int flac_gen_delete_frame(flac_gen_bw* bw, const flac_gen_frame_span* span)
{
    unsigned int length = span->end - span->start;
    unsigned int total = flac_gen_byte_pos(bw);
    memmove(bw->data + span->start, bw->data + span->end, (size_t)(total - span->end));
    bw->bitPos = (total - length) * 8u;
    return total - length;
}

#endif /* FLAC_GEN_H */
