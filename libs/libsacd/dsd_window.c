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
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <string.h>

#include "scarletbook.h"
#include "dsd_window.h"

int dsd_window_prepare(scarletbook_output_format_t *ft, int container, dsd_window_t *window)
{
    int channel_count;

    memset(window, 0, sizeof(*window));

    if (!ft->sample_window_enabled)
        return 0;

    if (ft->sample_window_begin >= ft->sample_window_end)
        return -1;

    if (!ft->sb_handle || !ft->sb_handle->area[ft->area].area_toc)
        return -1;

    channel_count = ft->sb_handle->area[ft->area].area_toc->channel_count;
    if (channel_count != 2 && channel_count != 5 && channel_count != 6)
        return -1;

    // The windowing path is DSD only: no zero-pad carry over, no DST frames, no edit masters.
    if (ft->sb_handle->dsf_nopad)
        return -1;
    if (!ft->dsd_encoded_export)
        return -1;
    if (ft->handler.flags & OUTPUT_FLAG_EDIT_MASTER)
        return -1;

    if (container == DSD_WINDOW_CONTAINER_DSDIFF
        && (ft->sample_window_end - ft->sample_window_begin) % 8 != 0)
        return -1;

    window->begin    = ft->sample_window_begin;
    window->end      = ft->sample_window_end;
    window->channels = channel_count;

    return 0;
}

int dsd_window_feed(dsd_window_t *window, scarletbook_output_format_t *ft,
                    const uint8_t *buf, size_t len, dsd_window_sink_t sink)
{
    size_t i;

    if (len == 0)
        return 0;

    // Interleaved byte position one past the last channel byte of the group
    // holding sample end-1; the window is only fully fed once this many input
    // bytes have arrived, even if the final selected bit lives in an earlier channel.
    uint64_t needed_bytes = (((window->end - 1) / 8) + 1) * (uint64_t) window->channels;

    for (i = 0; i < len; i++)
    {
        uint64_t abs_byte = window->input_bytes;
        int      channel  = (int) (abs_byte % (uint64_t) window->channels);
        uint64_t frame    = abs_byte / (uint64_t) window->channels; // per-channel sample byte
        const uint8_t in  = buf[i];
        int      bit;

        window->input_bytes++;

        // Bytes at or past the group containing end-1 can only hold dropped samples
        // when they belong to later groups; the group itself still counts for completeness.
        if (frame * 8 < window->end)
        {
            for (bit = 7; bit >= 0; bit--)
            {
                uint64_t sample = frame * 8 + (7 - bit);

                if (sample >= window->end)
                    break; // remaining bits of this byte are past the window
                if (sample < window->begin)
                    continue;

                dsd_window_accum_t *acc = &window->acc[channel];
                acc->byte = (uint8_t) (acc->byte | (((in >> bit) & 1) << (7 - acc->bits)));
                acc->bits++;

                if (acc->bits == 8)
                {
                    if (sink(ft, channel, acc->byte, 8) < 0)
                        return -1;
                    acc->byte = 0;
                    acc->bits = 0;
                }
            }
        }

        if (!window->finished && window->input_bytes >= needed_bytes)
            window->finished = 1;
    }

    return 0;
}

int dsd_window_finish(dsd_window_t *window, scarletbook_output_format_t *ft,
                      dsd_window_sink_t sink)
{
    int channel;

    if (!window->end)
        return 0;

    if (!window->finished)
        return -1;

    for (channel = 0; channel < window->channels; channel++)
    {
        dsd_window_accum_t *acc = &window->acc[channel];

        if (acc->bits > 0)
        {
            if (sink(ft, channel, acc->byte, acc->bits) < 0)
                return -1;
            acc->byte = 0;
            acc->bits = 0;
        }
    }

    return 0;
}
