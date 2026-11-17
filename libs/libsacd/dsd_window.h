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

#ifndef DSD_WINDOW_H_INCLUDED
#define DSD_WINDOW_H_INCLUDED

#include <stdio.h>
#include <inttypes.h>
#include "scarletbook_output.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    DSD_WINDOW_DSF = 0,
    DSD_WINDOW_DSDIFF = 1
} dsd_window_container_t;

/* Streaming per-channel bit window over MSB-first interleaved DSD.
   The engine owns no audio buffer beyond a few bytes per channel: its
   memory use grows only with the (fixed) channel count. */
typedef struct
{
    int       channels;
    int       phase;                          /* channel of the next input byte */
    uint64_t  begin;                          /* first selected bit, per channel */
    uint64_t  end;                            /* one past last selected bit */
    uint64_t  pos;                            /* complete channel groups consumed */
    uint64_t  selected[6];                    /* bits selected per channel */
    uint8_t   carry[6];                      /* MSB-aligned bits awaiting output */
    uint8_t   carry_bits[6];                 /* valid bits in carry, 0..7 */
    int       done;                           /* full window received on all channels */
} dsd_window_t;

/* Validate the window configuration of ft for the given container.
   Returns 0 when usable, -1 otherwise. Never touches the output file. */
int dsd_window_validate(const scarletbook_output_format_t *ft, dsd_window_container_t container);

/* Bind a window engine to the configuration of ft (call after validation). */
void dsd_window_init(dsd_window_t *w, const scarletbook_output_format_t *ft);

/* Non zero once every channel has received its full window of bit samples. */
int dsd_window_complete(const dsd_window_t *w);

/* Feed one interleaved source byte (MSB-first DSD). When the byte
   completes an output byte for its channel, returns 1 and fills
   channel_out / sample_byte_out; otherwise returns 0. Bytes beyond the
   window, and all bytes once the window is complete, are ignored. */
int dsd_window_feed(dsd_window_t *w, uint8_t source_byte, int *channel_out, uint8_t *sample_byte_out);

/* Flush the pending partial byte of a channel (DSF only, where the window
   length may be non divisible by 8). Unused high bits are zero. Returns 1
   when a byte was produced, 0 when nothing is pending. */
int dsd_window_flush_partial(dsd_window_t *w, int channel, uint8_t *sample_byte_out);

#ifdef __cplusplus
};
#endif

#endif /* DSD_WINDOW_H_INCLUDED */
