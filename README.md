[![GoReportCard](http://goreportcard.com/badge/github.com/asticode/go-astisub)](http://goreportcard.com/report/github.com/asticode/go-astisub)
[![GoDoc](https://godoc.org/github.com/asticode/go-astisub?status.svg)](https://godoc.org/github.com/asticode/go-astisub)
[![Test](https://github.com/asticode/go-astisub/actions/workflows/test.yml/badge.svg)](https://github.com/asticode/go-astisub/actions/workflows/test.yml)
[![Coveralls](https://coveralls.io/repos/github/asticode/go-astisub/badge.svg?branch=master)](https://coveralls.io/github/asticode/go-astisub)

This is a Golang library to manipulate subtitles. 

It allows you to manipulate `srt`, `stl`, `ttml`, `ssa/ass`, `webvtt` and `teletext` files for now.

It also decodes the 8-bit bitmap subtitle subset of the DVB subtitling standard
(`BitmapSubtitle`) directly into `*image.NRGBA` frames. See [DVB bitmap subtitles](#dvb-bitmap-subtitles).

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
- [ ] .smi

# DVB bitmap subtitles

`NewDVBSubtitleDecoder` incrementally decodes one DVB subtitle page (ETSI
EN 300 743) into `BitmapSubtitle` intervals carrying an `*image.NRGBA` frame.
`Push` takes a complete PES payload without its PES header; regions, objects
and CLUT updates arriving across PES are kept in the same display set and only
affect the visible frame at end-of-display-set. `Flush` closes the current
frame. `ReadFromDVBSubtitles` runs the same incremental decoder over the
private_stream_1 PES of a selected PID in a transport stream, using the first
selected PES that carries a PTS as time zero.

Supported scope and the exact field layout follow `task_support/dvb-contract.md`.
In short, only the 8-bit (`region_depth = 3`) subset is handled:

- `data_identifier` 0x20, subtitle stream id 0, one fixed page and one fixed
  frame size (1..2048); DDS windows are not processed.
- object type/provider 0, non-compressed pixel data (`coding_method = 0`),
  full-range CLUT entries only.
- interlaced top/bottom fields, with a missing bottom field reusing the top
  field; non-modifying colour code 1 keeps the previous pixels.
- normal-case region updates preserve unredrawn indexes; fill and
  acquisition/mode-change epochs rebuild the background.
- standalone CLUT updates recolour pixels already in the indexed plane.

Deliberately out of scope and rejected with an error: text/character coding,
2-bit and 4-bit pixel depths, compressed objects, PCS state 3, unsupported
object or CLUT flags, PTS wraparound/regressions, and segments of other pages.
`.ts` files opened through `Open` keep using the Teletext path; bitmap
subtitles are only reachable through the dedicated DVB entry points. The
example in `examples/dvbsub` renders a fixed 8x8 stream to PNG frames plus an
`index.json` manifest.
