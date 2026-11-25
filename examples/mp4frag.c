/*
 * Minimal fragmented-MP4 stdin demuxer example.
 *
 * Reads a stream of ftyp/moov + moof/mdat fragments from stdin and prints one
 * line per complete sample:
 *
 *   track_id timescale dts pts duration size flags payload_hex
 *
 * Sample lines are flushed immediately so that a downstream consumer sees
 * deliveries before EOF.
 */
#include "mp4d_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int print_sample(const MP4D_stream_sample_t *s, const unsigned char *payload,
    void *token)
{
    FILE *out = (FILE *)token;
    uint32_t i;
    if (fprintf(out, "%u %u %llu %lld %u %u %u ",
        s->track_id, s->timescale, (unsigned long long)s->dts,
        (long long)s->pts, s->duration, s->size, s->flags) < 0)
        return 1;
    for (i = 0; i < s->size; ++i)
        if (fprintf(out, "%02x", payload[i]) < 0)
            return 1;
    if (fputc('\n', out) == EOF)
        return 1;
    return fflush(out) != 0;
}

int main(int argc, char **argv)
{
    unsigned char buf[4096];
    size_t n;
    MP4D_stream_t *st;
    int rc = 0;

    (void)argv;
    if (argc != 1)
    {
        fprintf(stderr, "usage: %s < fragmented.mp4\n", argv[0]);
        return 2;
    }
    st = MP4D_stream_open(8u * 1024u * 1024u, print_sample, stdout);
    if (!st)
        return 2;

    while ((n = fread(buf, 1, sizeof(buf), stdin)) != 0)
    {
        /* Deliver one chunk at a time; the callback flushes each sample. */
        if (MP4D_stream_feed(st, buf, n) < 0)
        {
            rc = 1;
            break;
        }
    }
    if (!rc && ferror(stdin))
        rc = 1;
    if (!rc && MP4D_stream_finish(st) < 0)
        rc = 1;
    MP4D_stream_close(st);
    return rc;
}
