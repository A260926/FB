[![GoReportCard](http://goreportcard.com/badge/github.com/asticode/go-astisub)](http://goreportcard.com/report/github.com/asticode/go-astisub)
[![GoDoc](https://godoc.org/github.com/asticode/go-astisub?status.svg)](https://godoc.org/github.com/asticode/go-astisub)
[![Test](https://github.com/asticode/go-astisub/actions/workflows/test.yml/badge.svg)](https://github.com/asticode/go-astisub/actions/workflows/test.yml)
[![Coveralls](https://coveralls.io/repos/github/asticode/go-astisub/badge.svg?branch=master)](https://coveralls.io/github/asticode/go-astisub)

This is a Golang library to manipulate subtitles. 

It allows you to manipulate `srt`, `stl`, `ttml`, `ssa/ass`, `webvtt`, `teletext` and DVB bitmap subtitles for now.

Available operations are `parsing`, `writing`, `applying linear correction`, `syncing`, `fragmenting`, `unfragmenting`, `merging` and `optimizing`.

# Installation

To install the library:

    go get github.com/asticode/go-astisub

To install the CLI:

    go install github.com/asticode/go-astisub/cmd/astisub        

# Using the library in your code

WARNING: the code below doesn't handle errors for readibility purposes. However you SHOULD!

```go
// Open subtitles
s1, _ := astisub.OpenFile("/path/to/example.ttml")
s2, _ := astisub.ReadFromSRT(bytes.NewReader([]byte("1\n00:01:00.000 --> 00:02:00.000\nCredits")))

// Add a duration to every subtitles (syncing)
s1.Add(-2*time.Second)

// Fragment the subtitles
s1.Fragment(2*time.Second)

// Merge subtitles
s1.Merge(s2)

// Optimize subtitles
s1.Optimize()

// Unfragment the subtitles
s1.Unfragment()

// Apply linear correction
s1.ApplyLinearCorrection(1*time.Second, 2*time.Second, 5*time.Second, 7*time.Second)

// Write subtitles
s1.Write("/path/to/example.srt")
var buf = &bytes.Buffer{}
s2.WriteToTTML(buf)
```

# DVB bitmap subtitles

DVB bitmap subtitles (ETSI EN 300 743) are decoded to RGBA frames instead of
text. They cannot be read as Teletext character pages, and `Open`/`ReadFromTeletext`
keep their existing Teletext behaviour (`.ts` files are not auto-detected).

The incremental decoder consumes complete `PES.Data` payloads (without PES
header), keeps segment state across PES packets, and publishes a frame only at
an end-of-display-set (EODS) segment:

```go
d, _ := astisub.NewDVBSubtitleDecoder(1, 1920, 1080) // pageID, canvas width/height
var frames []astisub.BitmapSubtitle
for _, pes := range pesPayloads {                    // raw PES.Data, no header
    got, err := d.Push(pes, at)
    if err != nil { return err }
    frames = append(frames, got...)
}
got, err := d.Flush(end)                              // closes the visible frame
frames = append(frames, got...)
```

`BitmapSubtitle` exposes `StartAt`, `EndAt` (`time.Duration`) and `Image`
(`*image.NRGBA`). Each frame is a full-canvas composite of the page's regions,
already source-over blended on transparency, so the returned image never shares
mutable storage with the decoder or another result.

For a transport stream:

```go
f, _ := os.Open("recording.ts")
frames, err := astisub.ReadFromDVBSubtitles(f, astisub.DVBSubtitleOptions{
    PID: 256, PageID: 1, Width: 1920, Height: 1080,
    EndAt: 30 * time.Second,
})
```

`ReadFromDVBSubtitles` demuxes the selected PID's `private_stream_1` PES with
the existing `go-astits` dependency, uses the first selected PES that has a PTS
as the zero point, skips PES without a PTS, and requires non-wrapping,
non-decreasing PTS values. `EndAt` is a closed relative time passed to `Flush`.
It drives the incremental decoder above rather than a separate whole-file path.

Supported subset: `data_identifier=0x20`, `subtitle_stream_id=0`, segment types
PCS/RCS/CLUT/ODS/EODS, `region_depth=3` (8-bit indices), object type/provider 0,
`object_coding_method=0`, `full_range_flag=1`, and 8-bit pixel strings only
(`0x12`, `0xf0`). Text encoding, 2/4-bit pixels and compressed objects are out of
scope and return errors.

Decoding semantics:

- The index plane of each region is persistent. Region updates without `fill`
  keep existing pixels; `fill` and acquisition/mode-change display sets
  re-establish the background from `background8`.
- Object top and bottom fields interleave (rows 0,2,… and 1,3,…); when the
  bottom field is empty the top field is reused one row down.
- A non-modifying object does not write its index-1 colour codes, leaving the
  old pixels in place.
- A CLUT-only update recolours indices already on the plane, not just future
  objects.
- Objects reused by several regions keep independent positions and pixels.
- Object versions are compared per object id by their 4-bit number: a change
  from 15 to 0 is a new version, and a retransmission of the same version does
  not repeat a fill.
- Every referenced region, object and CLUT must resolve at EODS; nothing is
  published until then, and frames published before a later page or palette
  change keep their own snapshot.

See `task_support/dvb-contract.md` for the exact field layouts and interval
rules, and `examples/dvbsub` for a PNG exporter. The standard reference is
[ETSI EN 300 743 V1.6.1](https://www.etsi.org/deliver/etsi_EN/300700_300799/300743/01.06.01_60/en_300743v010601p.pdf)
(sections 7 and 11); only the subset documented above is implemented.

# Using the CLI

If **astisub** has been installed properly you can:

- convert any type of subtitle to any other type of subtitle:

        astisub convert -i example.srt -o example.ttml

- apply linear correction to any type of subtitle:

        astisub apply-linear-correction -i example.srt -a1 1s -d1 2s -a2 5s -d2 7s -o example.out.srt

- fragment any type of subtitle:

        astisub fragment -i example.srt -f 2s -o example.out.srt

- merge any type of subtitle into any other type of subtitle:

        astisub merge -i example.srt -i example.ttml -o example.out.srt

- optimize any type of subtitle:

        astisub optimize -i example.srt -o example.out.srt

- unfragment any type of subtitle:

        astisub unfragment -i example.srt -o example.out.srt

- sync any type of subtitle:

        astisub sync -i example.srt -s "-2s" -o example.out.srt

# Features and roadmap

- [x] parsing
- [x] writing
- [x] syncing
- [x] fragmenting/unfragmenting
- [x] merging
- [x] ordering
- [x] optimizing
- [x] linear correction
- [x] .srt
- [x] .ttml
- [x] .vtt
- [x] .stl
- [x] .ssa/.ass
- [x] .teletext
- [x] DVB bitmap subtitles (8-bit subset)
- [ ] .smi
