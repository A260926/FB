/*
 * examples/dsd_window_example.c
 *
 * Minimal C example: extract a per-channel DSD bit window straight from a
 * raw MSB-first interleaved DSD stream into a DSF or an uncompressed
 * DSDIFF file, without any PCM conversion or buffering the whole recording.
 *
 * Build (from the repository root):
 *   cc -std=gnu11 -I. -Ilibs/libsacd -Ilibs/libcommon -Ilibs/libdstdec \
 *      -Ilibs/libid3 examples/dsd_window_example.c .task-build/production.a \
 *      -liconv -o dsd_window_example
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "scarletbook_output.h"
#include "scarletbook.h"
#include "dsf.h"
#include "dsdiff.h"

extern const scarletbook_format_handler_t *dsf_format_fn(void);
extern const scarletbook_format_handler_t *dsdiff_format_fn(void);

/* One byte per channel, channels interleaved, MSB-first DSD bits. */
static size_t feed_some(scarletbook_output_format_t *ft,
                        const uint8_t *raw, size_t len)
{
    /* The handler accepts arbitrary fragmentation, including calls that
       split a channel group; zero-length calls are harmless. */
    int written = ft->handler.write(ft, raw, len);
    return written < 0 ? 0 : (size_t) written;
}

static int extract_window(int use_dsf, int channel_count,
                          uint64_t begin, uint64_t end,
                          const char *path)
{
    scarletbook_handle_t sb;
    area_toc_t toc;
    master_toc_t master;
    area_tracklist_t times;
    scarletbook_output_format_t ft;
    const scarletbook_format_handler_t *handler;
    FILE *fd;
    int rc = -1;

    memset(&sb, 0, sizeof(sb));
    memset(&toc, 0, sizeof(toc));
    memset(&master, 0, sizeof(master));
    memset(&times, 0, sizeof(times));
    memset(&ft, 0, sizeof(ft));

    toc.channel_count = (uint8_t) channel_count;     /* 2, 5 or 6 */
    toc.track_count = 1;
    sb.area[0].area_toc = &toc;
    sb.area[0].area_tracklist_time = &times;
    sb.master_toc = &master;
    sb.id3_tag_mode = 2;

    fd = fopen(path, "wb+");
    if (!fd)
    {
        perror(path);
        return -1;
    }

    handler = use_dsf ? dsf_format_fn() : dsdiff_format_fn();

    ft.sb_handle = &sb;
    ft.fd = fd;
    ft.filename = (char *) path;
    ft.dsd_encoded_export = 1;                        /* uncompressed DSD */
    ft.handler = *handler;
    ft.priv = calloc(1, handler->priv_size);
    if (!ft.priv)
        goto out;

    /* Request the half-open per-channel bit window [begin, end). */
    ft.sample_window_enabled = 1;
    ft.sample_window_begin = begin;
    ft.sample_window_end = end;

    /* Validation happens before any header byte is written. */
    if (ft.handler.startwrite(&ft) != 0)
    {
        fprintf(stderr, "window configuration rejected (check begin/end, "
                        "channel count and container alignment)\n");
        goto out;
    }

    /* Fabricate enough interleaved source data and feed it in small,
       arbitrarily split chunks. A real program would pipe its stream here. */
    {
        uint64_t groups = end / 8 + 1;
        uint64_t g;
        size_t total = (size_t) (groups * (uint64_t) channel_count);
        uint8_t *raw = malloc(total);
        size_t pos = 0;
        if (!raw)
            goto out;
        for (g = 0; g < groups; g++)
        {
            int c;
            for (c = 0; c < channel_count; c++)
                raw[g * channel_count + c] =
                    (uint8_t) ((g * 131u + c * 71u + 17u) ^ (c * 0x55));
        }
        while (pos < total)
        {
            size_t chunk = total - pos;
            if (chunk > 7)
                chunk = 7;                          /* arbitrary fragmentation */
            feed_some(&ft, raw + pos, chunk);
            pos += chunk;
        }
        free(raw);
    }

    /* stopwrite fails if the full window never arrived on every channel. */
    if (ft.handler.stopwrite(&ft) != 0)
        fprintf(stderr, "input ended before the window was complete\n");
    else
        rc = 0;

out:
    free(ft.priv);
    fclose(fd);
    return rc;
}

int main(void)
{
    /* DSF accepts any bit length; the tail byte's unused high bits and the
       block remainder are zeroed. */
    if (extract_window(1, 2, 3, 20, "example_window.dsf") != 0)
        return 1;

    /* DSDIFF needs a window length that is a multiple of 8. */
    if (extract_window(0, 2, 8, 40, "example_window.dff") != 0)
        return 1;

    puts("wrote example_window.dsf and example_window.dff");
    return 0;
}
