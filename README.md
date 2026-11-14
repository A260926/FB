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

## Bindings

 * https://github.com/darkskygit/minimp4.rs - rust bindings

## Streaming HEVC (H.265) Annex-B input

`h265_stream.h` wraps the muxer for raw HEVC byte streams that arrive in
arbitrary chunks (a start code or a NAL header may span two feeds). It groups
all slices and the suffix SEI of one coded picture into a single MP4 sample,
so a picture is never split into several samples. Default and sequential mux
modes are supported; fragmented mode is rejected. The legacy
`mp4_h26x_write_nal()` entry point is unchanged.

```c
#include <stdio.h>
#include "minimp4.h"
#include "h265_stream.h"

static int wr_cb(int64_t off, const void *buf, size_t n, void *token)
{
    FILE *f = (FILE *)token;
    return fseek(f, (long)off, SEEK_SET) || fwrite(buf, 1, n, f) != n;
}

int h265_file_to_mp4(FILE *in, FILE *out, int width, int height,
                     int fps_num, int fps_den)
{
    unsigned char chunk[16 * 1024];
    size_t n;
    int rc;

    /* 0 = default mode; pass 1 to MP4E_open() for sequential (no back-seek). */
    MP4E_mux_t *mux = MP4E_open(0, 0, out, wr_cb);
    if (!mux)
        return -1;

    /* Creates the HEVC track itself, with a 90000 Hz time base. */
    MP4H265_stream_t *st = MP4H265_stream_open(mux, width, height,
                                               fps_num, fps_den);
    if (!st)
    {
        MP4E_close(mux);
        return -1;
    }

    /* Where each feed is split carries no meaning; empty feeds are fine. */
    while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0)
    {
        rc = MP4H265_stream_feed(st, chunk, n);
        if (rc != MP4E_STATUS_OK)
        {
            MP4H265_stream_close(st);   /* frees the wrapper only ... */
            MP4E_close(mux);            /* ... the muxer is closed separately */
            return rc;
        }
    }

    rc = MP4H265_stream_finish(st);     /* flush the last picture, idempotent */
    MP4H265_stream_close(st);           /* does not close `mux` */
    if (MP4E_close(mux) != MP4E_STATUS_OK && rc == MP4E_STATUS_OK)
        rc = MP4E_STATUS_FILE_WRITE_ERROR;
    return rc;
}
```

VPS/SPS/PPS are recorded in the track's `hvcC` configuration (parameter sets
are only accepted before the first slice), AUD is never written into a
sample, and the first VCL NAL must be an IRAP with all three parameter sets
already present. Sample timing follows `floor(n*90000*fps_den/fps_num)` per
frame, so non-integer rates such as 24000/1001 produce alternating 3753/3754
durations rather than accumulated rounding error.

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
