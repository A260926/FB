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

## Presentation timestamps (B-frames)

Encoders that deliver AVC frames in decoding order (with B-frames the display
order differs) can attach a per-sample composition offset while muxing:

    MP4E_put_sample_cts(mux, track, data, bytes, duration, kind, composition_offset);

`composition_offset` is in track timescale units; the presentation time of a
sample is DTS + offset. Samples are still submitted, stored in `mdat` and
marked as sync points in decoding order, and `duration = 0` still falls back
to the track default. `MP4E_put_sample()` is exactly `MP4E_put_sample_cts()`
with a zero offset. Offsets are run-length encoded into a `ctts` box
(version 1 if any offset is negative, version 0 otherwise); a track whose
offsets are all zero gets no `ctts` box at all, so files written through the
old entry point stay byte-identical. A `MP4E_SAMPLE_CONTINUATION` slice only
extends the previous sample's bytes and must carry the same offset.
`MP4E_put_sample_cts()` applies to the default and sequential mux modes; in
fragmentation mode it returns `MP4E_STATUS_BAD_ARGUMENTS`.

On the demux side, per-sample timing of non-fragmented files is available as:

    uint64_t dts; int64_t pts; uint32_t duration;
    if (MP4D_frame_timing(&mp4, track, sample, &dts, &pts, &duration)) ...

Sample numbers start at 0; `pts` is `dts` plus the `ctts` offset (version 0
offsets are unsigned, version 1 signed), or equals `dts` when the file has no
`ctts` box. The function fails (returning 0, outputs untouched) on an invalid
index or an unrepresentable timestamp. `MP4D_frame_offset()` keeps its
existing meaning. Fragmented-file reading, edit lists and presentation-order
reordering are out of scope.

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
