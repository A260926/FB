/**
 * SACD Ripper - https://github.com/sacd-ripper/
 *
 * Copyright (c) 2010-2015 by respective authors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */

/*
 * Streaming per-channel bit window for raw DSD.
 *
 * The input is the production DSD stream: MSB-first, one byte per channel,
 * channels interleaved. A window [begin, end) of per-channel bit samples is
 * extracted directly from that bit sequence; no whole recording is buffered.
 * The only per-channel state is a single partial output byte.
 */

#include <string.h>

#include "dsd_window.h"

int dsd_window_validate(const scarletbook_output_format_t *ft, dsd_window_container_t container)
{
    int channel_count;

    if (!ft->sample_window_enabled)
        return 0;

    if (ft->sample_window_begin >= ft->sample_window_end)
        return -1;

    if (!ft->sb_handle || !ft->sb_handle->area[ft->area].area_toc)
        return -1;

    channel_count = ft->sb_handle->area[ft->area].area_toc->channel_count;
    if (channel_count != 2 && channel_count != 5 && channel_count != 6)
        return -1;

    /* Windowing relies on the plain per-track DSD paths; the nopad carry over,
       DST framing and edited-master marker tables all assume full tracks. */
    if (ft->sb_handle->dsf_nopad)
        return -1;
    if (!ft->dsd_encoded_export)
        return -1;
    if (ft->handler.flags & OUTPUT_FLAG_EDIT_MASTER)
        return -1;

    if (container == DSD_WINDOW_DSDIFF)
    {
        /* DSDIFF has no valid-trailing-bits field: every channel byte must be
           complete, so the window length has to be a whole number of bytes. */
        if ((ft->sample_window_end - ft->sample_window_begin) & 0x7ULL)
            return -1;
    }

    return 0;
}

void dsd_window_init(dsd_window_t *w, const scarletbook_output_format_t *ft)
{
    memset(w, 0, sizeof(*w));
    w->channels = ft->sb_handle->area[ft->area].area_toc->channel_count;
    w->begin = ft->sample_window_begin;
    w->end = ft->sample_window_end;
}

static uint64_t dsd_window_channel_bits(const dsd_window_t *w, int channel)
{
    return w->selected[channel] + w->carry_bits[channel];
}

int dsd_window_complete(const dsd_window_t *w)
{
    uint64_t const window_length = w->end - w->begin;
    int channel;

    for (channel = 0; channel < w->channels; channel++)
    {
        if (dsd_window_channel_bits(w, channel) != window_length)
            return 0;
    }
    return 1;
}

int dsd_window_feed(dsd_window_t *w, uint8_t source_byte, int *channel_out, uint8_t *sample_byte_out)
{
    int channel;
    uint64_t group;
    uint64_t const window_length = w->end - w->begin;
    int bit;
    int produced = 0;

    if (w->done)
        return 0;

    channel = w->phase;
    group = w->pos;

    /* Advance the interleaved stream position; a call may stop mid group. */
    if (++w->phase == w->channels)
    {
        w->phase = 0;
        w->pos++;
    }

    /* Whole byte lies before the window, or this channel already has enough. */
    if ((group + 1) * 8 <= w->begin || dsd_window_channel_bits(w, channel) >= window_length)
        return 0;

    for (bit = 0; bit < 8; bit++)
    {
        uint64_t bit_position = group * 8 + (uint64_t) bit;

        if (bit_position < w->begin || bit_position >= w->end)
            continue;

        w->carry[channel] = (uint8_t) ((w->carry[channel] << 1) | ((source_byte >> (7 - bit)) & 0x1));
        if (++w->carry_bits[channel] == 8)
        {
            /* At most one output byte can complete per source byte: the carry
               held at most 7 bits and one source byte adds at most 8. */
            *channel_out = channel;
            *sample_byte_out = w->carry[channel];
            w->carry[channel] = 0;
            w->carry_bits[channel] = 0;
            w->selected[channel] += 8;
            produced = 1;
        }
    }

    if (dsd_window_complete(w))
        w->done = 1;

    return produced;
}

int dsd_window_flush_partial(dsd_window_t *w, int channel, uint8_t *sample_byte_out)
{
    if (w->carry_bits[channel] == 0)
        return 0;

    /* Unused high bits of the last byte are zero. */
    *sample_byte_out = (uint8_t) (w->carry[channel] << (8 - w->carry_bits[channel]));
    w->selected[channel] += w->carry_bits[channel];
    w->carry[channel] = 0;
    w->carry_bits[channel] = 0;
    return 1;
}
