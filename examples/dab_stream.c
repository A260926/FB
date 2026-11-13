/*
 * Example: library-managed DAB ScF-CRC streaming.
 *
 * Encodes a short synthetic programme (three whole frames plus a partial
 * frame of non-zero PCM) and writes the complete MP2 file.  The caller feeds
 * PCM in arbitrary chunk sizes; the library patches each frame's ScF-CRC
 * into the previous frame before handing it back.
 *
 * Usage: dab_stream <output.mp2>
 */

#include "twolame.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S TWOLAME_SAMPLES_PER_FRAME
#define TOTAL_SAMPLES (3 * S + 173)
#define OUT_ROOM ((size_t) 7200 + 2 * (size_t) TOTAL_SAMPLES)

static void die(const char *what)
{
    fprintf(stderr, "dab_stream example: %s\n", what);
    exit(1);
}

int main(int argc, char **argv)
{
    twolame_options *opt;
    FILE *fp;
    short *left, *right;
    unsigned char *pcm_out, *frame;
    size_t room = OUT_ROOM;
    int i, pos, total = 0;
    uint32_t rnd = 0x13579bdfu;
    static const int chunks[] = { 1000, 300, 1152, 1000, 1, 500, 37, 2049 };

    if (argc != 2) {
        fprintf(stderr, "usage: %s <output.mp2>\n", argv[0]);
        return 1;
    }

    opt = twolame_init();
    if (opt == NULL)
        die("twolame_init failed");

    /* All parameters must be set before twolame_init_params(). */
    twolame_set_num_channels(opt, 2);
    twolame_set_in_samplerate(opt, 48000);
    twolame_set_out_samplerate(opt, 48000);
    twolame_set_bitrate(opt, 192);
    twolame_set_mode(opt, TWOLAME_STEREO);
    twolame_set_padding(opt, TWOLAME_PAD_ALL);
    twolame_set_verbosity(opt, 0);
    twolame_set_DAB(opt, 1);
    twolame_set_DAB_crc_length(opt, 4);
    /* 4 CRC bytes + 2 F-PAD bytes, with one spare byte of ancillary data. */
    twolame_set_num_ancillary_bits(opt, 8 * (4 + 2 + 1));
    if (twolame_set_DAB_streaming(opt, 1) != 0)
        die("could not enable DAB streaming");
    if (twolame_init_params(opt) != 0)
        die("twolame_init_params failed");

    left = (short *) malloc(sizeof(short) * TOTAL_SAMPLES);
    right = (short *) malloc(sizeof(short) * TOTAL_SAMPLES);
    pcm_out = (unsigned char *) malloc(room);
    if (!left || !right || !pcm_out)
        die("out of memory");

    for (i = 0; i < TOTAL_SAMPLES; i++) {
        rnd = rnd * 1664525u + 1013904223u;
        left[i] = (short) (int) ((rnd >> 16) & 0x7fff) - 16384;
        rnd = rnd * 1664525u + 1013904223u;
        right[i] = (short) (int) ((rnd >> 16) & 0x7fff) - 16384;
    }

    /* Feed arbitrary sized chunks, copying each result away immediately. */
    frame = (unsigned char *) malloc(room);
    if (!frame)
        die("out of memory");
    pos = 0;
    i = 0;
    while (pos < TOTAL_SAMPLES) {
        int k = chunks[i % (int) (sizeof(chunks) / sizeof(chunks[0]))];
        int n;
        size_t chunk_room = 7200 + 2 * (size_t) k;

        if (k > TOTAL_SAMPLES - pos)
            k = TOTAL_SAMPLES - pos;
        if (chunk_room > room)
            chunk_room = room;
        n = twolame_encode_buffer(opt, left + pos, right + pos, k, frame, (int) chunk_room);
        if (n < 0)
            die("twolame_encode_buffer failed");
        memcpy(pcm_out + total, frame, (size_t) n);
        total += n;
        pos += k;
        i++;
    }

    {
        int n = twolame_encode_flush(opt, frame, 7200);

        if (n < 0)
            die("twolame_encode_flush failed");
        memcpy(pcm_out + total, frame, (size_t) n);
        total += n;
    }
    if (twolame_encode_flush(opt, frame, 7200) != 0)
        die("second flush was not a no-op");
    twolame_close(&opt);

    fp = fopen(argv[1], "wb");
    if (fp == NULL)
        die("could not open output file");
    if (fwrite(pcm_out, 1, (size_t) total, fp) != (size_t) total)
        die("short write");
    fclose(fp);

    printf("dab_stream example: wrote %d bytes to %s\n", total, argv[1]);

    free(left);
    free(right);
    free(pcm_out);
    free(frame);
    return 0;
}
