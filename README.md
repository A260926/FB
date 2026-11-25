Mini MP4
==========

[![Build Status](https://travis-ci.org/lieff/minimp4.svg)](https://travis-ci.org/lieff/minimp4)

Easy embeddable MP4 mux/demux library.

## Usage
#### Muxing

Muxing can be done using 3 modes.
Default mode uses one big mdat chunk:

![default](images/mux_mode_default.png?raw=true)

This is most efficient mode, but disadvantage is that we need go back and patch mdat chunk size.
This can be a problem in some cases, for example if stream transfered over network.
To workaround this sequential mode is used:

![default](images/mux_mode_sequential.png?raw=true)

This mode do not make any backwards seek.
And last mode is fragmented aka fMP4.

![default](images/mux_mode_fragmented.png?raw=true)

This mode stores track information first and spreads indexes across all stream, so decoding can start before whole stream available.
This mode is sequential too and usually used by browsers and HLS streaming.

#### Streaming demuxing (fragmented MP4 from a pipe)

`mp4d_stream.h` / `mp4d_stream.c` add an incremental demuxer for a
`ftyp`/`moov` initialization segment followed by consecutive `moof`/`mdat`
fragments. It parses the initialization segment with the existing `MP4D`
code, keeps only the current moof description and one sample buffer, and calls
back as soon as each complete sample has been fed -- no seeking and no wait for
EOF.

```c
#include "mp4d_stream.h"

static int on_sample(const MP4D_stream_sample_t *s, const unsigned char *payload,
                     void *token)
{
    /* payload is borrowed: copy it if it must outlive the callback. */
    printf("track %u ts %u dts %llu pts %lld dur %u size %u flags %08x\n",
        s->track_id, s->timescale, (unsigned long long)s->dts,
        (long long)s->pts, s->duration, s->size, s->flags);
    return 0; /* non-zero fails the stream permanently */
}

MP4D_stream_t *st = MP4D_stream_open(4 * 1024 * 1024, on_sample, NULL);
for (;;) {
    size_t n = fread(buf, 1, sizeof(buf), input);
    if (!n)
        break;                                  /* or a blocking pipe read */
    if (MP4D_stream_feed(st, buf, n) < 0)       /* any chunk size is fine */
        break;
}
if (MP4D_stream_finish(st) == 0)                /* detects truncation */
    /* ... */;
MP4D_stream_close(st);
```

`feed`/`finish` return `0` on success and a negative value on failure; a
failure (including a non-zero callback return) is permanent, so no further
samples are delivered. `finish` is idempotent and rejects a truncated stream.
Samples are delivered in media-address order, exactly once, and `flags` is the
resolved ISO `sample_flags` word. `dts` is a 64-bit unsigned value and `pts`
is a signed 64-bit value; both use the track's own timescale.

The `limit` passed to `MP4D_stream_open` bounds three things independently:
the retained init segment, the current moof description (including the derived
sample table), and the single sample buffer. Nothing scales with the total
media length.

Supported layout: one sample description per track, `mvex`/`trex`, at most one
`traf` per track and several `trun`s per `traf`, sparse tracks, ordinary and
extended-size boxes, `default-base-is-moof` or explicit `base-data-offset`,
signed `trun` data offsets, `tfdt` v0/v1 (omitted tfdt continues the previous
fragment's end), and signed/unsigned composition offsets. Encrypted tracks,
external data references, implicit tfhd bases and multiple sample descriptions
are rejected rather than guessed at.

See `examples/mp4frag.c` for a complete stdin-to-lines program. `mp4d_stream.c`
calls into the regular `MP4D` implementation, so exactly one translation unit
must define `MINIMP4_IMPLEMENTATION` (as `task_support/upstream.c` does):

```
cc -std=c99 -I. -c task_support/upstream.c -o upstream.o
cc -std=c99 -I. examples/mp4frag.c mp4d_stream.c upstream.o -lm -o mp4frag
mp4frag < fragmented.mp4
# 1 90000 0 0 3000 6 33554432 000000026580
```

## Bindings

 * https://github.com/darkskygit/minimp4.rs - rust bindings

## Interesting links

 * https://github.com/aspt/mp4
 * https://github.com/l30nnguyen/minimum_mp4_muxer
 * https://github.com/DolbyLaboratories/dlb_mp4demux
 * https://github.com/DolbyLaboratories/dlb_mp4base
 * https://github.com/ireader/media-server/tree/master/libmov
 * https://github.com/wlanjie/mp4
 * https://github.com/MPEGGroup/isobmff
 * http://www.itscj.ipsj.or.jp/sc29/open/29view/29n7644t.doc
 * http://atomicparsley.sourceforge.net/mpeg-4files.html
 * http://cpansearch.perl.org/src/JHAR/MP4-Info-1.12/Info.pm
 * https://developer.apple.com/library/archive/documentation/QuickTime/QTFF/QTFFPreface/qtffPreface.html
 * http://xhelmboyx.tripod.com/formats/mp4-layout.txt
