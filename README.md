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

#### Embedded WebVTT subtitles

Alongside the audio/video API, a non-fragmented MP4 can carry an embedded
WebVTT subtitle track. The library does not parse `.vtt` text: the caller
supplies the track header and the cue intervals/text, and the library emits a
real `wvtt` track (text handler, `nmhd`, `wvtt`/`vttC` sample entry and the
per-interval `vttc`/`vtte` samples).

`MP4E_add_webvtt_track()` takes a 3-letter language code and the UTF-8 WebVTT
header bytes (stored verbatim in `vttC`; no extra NUL is implied). Both are
copied by the library. `MP4E_write_webvtt()` then submits the whole cue list at
once: cues may be in any order and may overlap, `total_ms` must be in
`[1, INT32_MAX]`, and each cue needs `0 <= start_ms < end_ms <= total_ms`.
Sample boundaries are `0`, `total_ms` and every cue start/end point; each
adjacent pair becomes one sample whose payload holds the cues active in that
half-open interval, in input order. Empty intervals get a `vtte` box. Only
default and sequential modes are supported, and a WebVTT track cannot be
extended with `MP4E_put_sample()`.

Muxing and demuxing are both available; on the read side `MP4D_track_t.sample_entry`
is `0x77767474` (`'wvtt'`) and `dsi`/`dsi_bytes` return the raw `vttC` bytes.

```c
#include <stdio.h>
#include <string.h>
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

static int write_cb(int64_t offset, const void *buf, size_t size, void *token)
{
    FILE *f = (FILE*)token;
    if (fseek(f, (long)offset, SEEK_SET)) return 1;
    return fwrite(buf, 1, size, f) != size;
}

int main(void)
{
    static const char header[] = "WEBVTT\nKind: captions\nLanguage: en\n";
    static const MP4E_webvtt_cue_t cues[] = {
        { 1000, 3000, "intro", "line:0", "Hello" },
        { 2000, 4000, NULL,    "line:1", "World" }
    };
    FILE *f = fopen("out.mp4", "wb");
    MP4E_mux_t *mux = MP4E_open(0, 0, f, write_cb);
    int track = MP4E_add_webvtt_track(mux, "eng", header, sizeof(header) - 1);

    if (track >= 0)
        MP4E_write_webvtt(mux, track, cues, sizeof(cues)/sizeof(cues[0]), 5000);
    MP4E_close(mux);
    fclose(f);
    return 0;
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
