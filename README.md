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

## Streaming HEVC input

`h265_stream.h` provides an alternative to `mp4_h26x_write_nal()` for HEVC:
it accepts an Annex-B byte stream in arbitrary chunks and writes one MP4
sample per picture (access unit), so multi-slice pictures and SEI NAL units
no longer become separate samples. Chunk boundaries are meaningless:
start codes, NAL headers and slice headers may be split across feeds.
Timestamps are derived from an exact rational clock on the 90 kHz
timescale: frame `n` starts at `floor(n*90000*fps_den/fps_num)`, so rates
like 24000/1001 yield the correct alternating 3753/3754 durations.
The writer requires a single-layer stream (`nuh_layer_id == 0`,
`nuh_temporal_id_plus1 == 1`), VPS/SPS/PPS before the first VCL NAL unit,
and an IRAP first picture. It works with the default and the sequential
mux modes; fragmented (fMP4) mode is not supported.

```c
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
#include "h265_stream.h"

static int write_callback(int64_t offset, const void *buffer, size_t size, void *token)
{
    FILE *f = (FILE *)token;
    fseek(f, (long)offset, SEEK_SET);
    return fwrite(buffer, 1, size, f) != size;
}

/* Record a chunked HEVC Annex-B stream into an MP4 file. */
int record_hevc(FILE *out, const unsigned char *chunks[], const size_t sizes[], int nchunks)
{
    int i, err = MP4E_STATUS_OK, rc = 1;
    MP4E_mux_t *mux = MP4E_open(0 /* sequential_mode */, 0 /* fragmentation */, out, write_callback);
    MP4H265_stream_t *hevc = mux ? MP4H265_stream_open(mux, 1280, 720, 24000, 1001 /* fps */) : NULL;
    if (!hevc)
    {
        if (mux)
            MP4E_close(mux);
        return 1;
    }
    for (i = 0; i < nchunks; i++)
        if ((err = MP4H265_stream_feed(hevc, chunks[i], sizes[i])) != MP4E_STATUS_OK)
            break;      /* broken instance: stop feeding */
    if (err == MP4E_STATUS_OK)
        err = MP4H265_stream_finish(hevc);  /* commits the last picture */
    MP4H265_stream_close(hevc);             /* frees the writer only ... */
    if (MP4E_close(mux) == MP4E_STATUS_OK && err == MP4E_STATUS_OK)
        rc = 0;                             /* ... mux closed separately */
    return rc;
}
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
