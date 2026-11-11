# go-wav ![workflow status](https://github.com/youpy/go-wav/actions/workflows/go.yml/badge.svg)

A Go library to read/write WAVE(RIFF waveform Audio) Format

## Usage

```go
package main

import (
	"flag"
	"fmt"
	"github.com/youpy/go-wav"
	"io"
	"os"
)

func main() {
	infile_path := flag.String("infile", "", "wav file to read")
	flag.Parse()

	file, _ := os.Open(*infile_path)
	reader := wav.NewReader(file)

  	defer file.Close()

	for {
		samples, err := reader.ReadSamples()
		if err == io.EOF {
			break
		}

		for _, sample := range samples {
			fmt.Printf("L/R: %d/%d\n", reader.IntValue(sample, 0), reader.IntValue(sample, 1))
		}
	}
}
```

## Supported format

Format

- PCM
- IEEE float (read-only)
- G.711 A-law (read-only)
- G.711 µ-law (read-only)

Audio data is read either from a single top-level `data` chunk or from a
single `LIST` chunk whose form type is `wavl` (a wave list). A wave list
stores the recording as a sequence of `data` and `slnt` sub-chunks. `slnt`
repeats the previous complete audio frame for the number of frames given in its
four-byte little-endian payload; a `slnt` before the first `data` produces digital
silence (128 for 8-bit unsigned samples, zero otherwise). To the caller the
result is one continuous stream, so `Read`, `ReadSamples` and `Duration` work
exactly as they do for a plain `data` chunk, and `WavData.Size` reports the size of
the expanded audio.

Wave lists are only supported for integer PCM, mono or stereo, at 8, 16, 24 or
32 bits per sample. The writer is not extended and emits a single `data`
chunk.

Number of channels

- 1(mono)
- 2(stereo)

Bits per sample

- 32-bit
- 24-bit
- 16-bit
- 8-bit

## Mixing `Read` and `ReadSamples`

`Read` and `ReadSamples` share a single playback position. `ReadSamples` operates on
whole frames (one frame is `BlockAlign` bytes), so if a previous `Read` stopped in
the middle of a frame, `ReadSamples` returns an error and does not consume any more
bytes. Finish the current frame with `Read` until the number of bytes read is a
multiple of `BlockAlign`, and frame-based reading can then resume.

## Documentation

- https://godoc.org/github.com/youpy/go-wav

## See Also

- http://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html
