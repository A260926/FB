/*
 * Example: cut a per-channel sample window straight out of a raw, MSB-first,
 * one-byte-per-channel interleaved DSD stream and write a DSF or uncompressed
 * DSDIFF file without ever converting to PCM.
 *
 * The window is the half-open range [sample_window_begin, sample_window_end)
 * counted in DSD bit samples per channel. The same byte stream is fed to the
 * handler's startwrite / write / stopwrite callbacks; write() may split the
 * input at any byte boundary, even in the middle of a channel group.
 *
 * Build against the production archive produced by task_support/build.sh:
 *   cc -std=gnu11 -I. -Ilibs/libsacd -Ilibs/libcommon -Ilibs/libdstdec \
 *      -Ilibs/libid3 examples/dsd_window_example.c .task-build/production.a \
 *      -liconv -o dsd_window_example
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "scarletbook_output.h"
#include "scarletbook.h"
#include "dsf.h"
#include "dsdiff.h"

extern const scarletbook_format_handler_t *dsf_format_fn(void);
extern const scarletbook_format_handler_t *dsdiff_format_fn(void);

#define CHANNELS 2

/* Distinct, deterministic raw DSD bytes: clustered frame k alternates channels. */
static uint8_t raw_dsd_byte(size_t k)
{
    int channel = (int) (k % CHANNELS);
    return (uint8_t) ((k * 37u + (size_t) channel * 0x55u + 0xA5u) & 0xFFu);
}

static int write_windowed(const scarletbook_format_handler_t *handler,
                          uint64_t begin, uint64_t end)
{
    /* Minimal scarletbook handle: only the pieces the format handlers touch. */
    scarletbook_handle_t  sb;
    area_toc_t            toc;
    master_toc_t          master;
    area_tracklist_t      times;
    scarletbook_output_format_t ft;

    memset(&sb, 0, sizeof(sb));
    memset(&toc, 0, sizeof(toc));
    memset(&master, 0, sizeof(master));
    memset(&times, 0, sizeof(times));
    memset(&ft, 0, sizeof(ft));

    toc.channel_count = CHANNELS;
    toc.track_count   = 1;
    sb.area[0].area_toc            = &toc;
    sb.area[0].area_tracklist_time = &times;
    sb.master_toc                  = &master;
    sb.id3_tag_mode                = 2;

    ft.area                 = 0;
    ft.track                = 0;
    ft.filename             = "dsd_window_example";
    ft.dsd_encoded_export   = 1;                 /* uncompressed DSD, never DST */
    ft.sb_handle            = &sb;
    ft.handler              = *handler;
    ft.priv                 = calloc(1, handler->priv_size);
    ft.fd                   = tmpfile();
    if (!ft.priv || !ft.fd)
        return -1;

    /* Turn on the sample window: per-channel DSD bits [begin, end). */
    ft.sample_window_enabled = 1;
    ft.sample_window_begin   = begin;
    ft.sample_window_end     = end;

    if (ft.handler.startwrite(&ft) < 0)
    {
        fprintf(stderr, "startwrite rejected the window configuration\n");
        fclose(ft.fd);
        free(ft.priv);
        return -1;
    }

    /*
     * Feed the raw stream in arbitrary fragments (here 13 bytes at a time) to
     * show that splitting across channel groups is supported. Zero-length
     * calls are legal and do not advance state.
     */
    size_t total_frames = (size_t) (end / 8 + 2);
    size_t total_bytes  = total_frames * CHANNELS;
    size_t sent         = 0;
    while (sent < total_bytes)
    {
        size_t n = total_bytes - sent;
        if (n > 13)
            n = 13;

        uint8_t chunk[13];
        for (size_t i = 0; i < n; i++)
            chunk[i] = raw_dsd_byte(sent + i);

        int physical = ft.handler.write(&ft, chunk, n);
        if (physical < 0)
        {
            fclose(ft.fd);
            free(ft.priv);
            return -1;
        }
        sent += n;
    }

    /* Fails (and frees resources) if the full end channel group never arrived. */
    if (ft.handler.stopwrite(&ft) < 0)
    {
        fprintf(stderr, "stopwrite: incomplete window, input ended too early\n");
        fclose(ft.fd);
        free(ft.priv);
        return -1;
    }

    fseek(ft.fd, 0, SEEK_END);
    long size = ftell(ft.fd);
    printf("%s: window [%llu,%llu) -> %ld physical bytes\n",
           handler->name,
           (unsigned long long) begin, (unsigned long long) end, size);

    fclose(ft.fd);
    free(ft.priv);
    return 0;
}

int main(void)
{
    /* DSF accepts non-byte-aligned windows and pads the final block/byte. */
    if (write_windowed(dsf_format_fn(), 3, 70) < 0)
        return 1;

    /* DSDIFF has no valid-tail-bits field, so the window length must be a
     * multiple of 8 bits. 40..104 is 64 bits per channel. */
    if (write_windowed(dsdiff_format_fn(), 40, 104) < 0)
        return 1;

    return 0;
}
