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

#ifndef DSD_WINDOW_H_INCLUDED
#define DSD_WINDOW_H_INCLUDED

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "scarletbook_output.h"

// Container selectors for dsd_window_prepare().
enum
{
    DSD_WINDOW_CONTAINER_DSF    = 1,
    DSD_WINDOW_CONTAINER_DSDIFF = 2
};

// Per-channel bit accumulator. Selected bits are packed MSB-first, i.e. the
// oldest selected bit ends up in bit 7 of the output byte.
typedef struct
{
    uint8_t byte;
    int     bits;       // number of valid bits, 0..7
} dsd_window_accum_t;

// Streaming sample-window state. Lives inside the format handler private data,
// so storage only scales with the fixed maximum channel count.
typedef struct
{
    uint64_t            input_bytes;   // interleaved input bytes already consumed
    uint64_t            begin;         // first per-channel DSD sample of the window
    uint64_t            end;           // one past the last per-channel DSD sample
    int                 channels;      // 2, 5 or 6 once prepared
    int                 finished;      // full channel group holding end-1 received
    dsd_window_accum_t  acc[MAX_CHANNEL_COUNT];
} dsd_window_t;

// Validates the window configuration on ft and initializes *window. Returns 0
// when the window is disabled or usable, a negative value on a rejected
// configuration. Must run before any file header is written.
int dsd_window_prepare(scarletbook_output_format_t *ft, int container, dsd_window_t *window);

// Sink for produced output bytes. channel is the output channel index, byte_msb
// carries the selected bits MSB-first and valid_bits is 8 for complete bytes
// (1..7 only for the trailing partial byte, whose upper MSB positions are zero).
typedef int (*dsd_window_sink_t)(scarletbook_output_format_t *ft, int channel,
                                 uint8_t byte_msb, int valid_bits);

// Feeds one chunk of MSB-first interleaved raw DSD. Zero-length calls do not
// move the state; data after the completed window is ignored. Returns 0 on
// success and a negative value when the sink reports a write failure.
int dsd_window_feed(dsd_window_t *window, scarletbook_output_format_t *ft,
                    const uint8_t *buf, size_t len, dsd_window_sink_t sink);

// Flushes the optional trailing partial bytes and verifies that the complete
// window was received. Returns 0 on success, negative when the input ended
// before the channel group holding sample end-1 was complete.
int dsd_window_finish(dsd_window_t *window, scarletbook_output_format_t *ft,
                      dsd_window_sink_t sink);

#endif /* DSD_WINDOW_H_INCLUDED */
