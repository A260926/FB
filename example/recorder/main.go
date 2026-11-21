// Command caf-recorder demonstrates streaming a variable bit rate Opus CAF
// recording with periodic checkpoints. Each written packet is a self-built
// legal Opus silence packet (a CELT-only 20 ms full-band TOC with one
// zero-length frame, which decodes as silence); no audio is buffered.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/pascoej/caf"
)

func main() {
	out := flag.String("out", "recording.caf", "output CAF file")
	seconds := flag.Int("seconds", 2, "recording length in seconds")
	reserve := flag.Int64("reserve", 64, "initial pakt/free reserved bytes (rounded up to a power of two)")
	flag.Parse()

	f, err := os.Create(*out)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer f.Close()

	format := caf.AudioFormat{
		SampleRate:        48000,
		FormatID:          caf.FourByteString{'o', 'p', 'u', 's'},
		FramesPerPacket:   960, // 20 ms at 48 kHz
		ChannelsPerPacket: 2,
	}
	// The magic cookie is optional and stored verbatim in the kuki chunk.
	cookie := []byte("example-cookie")

	rec, err := caf.NewRecorder(f, format, cookie, *reserve)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	// 0xF8: config 31 (CELT-only, 20 ms, full band), mono flag clear in the
	// TOC; a code-0 packet with a zero-length frame is legal Opus silence.
	silence := []byte{0xF8}
	packets := (*seconds) * int(format.SampleRate) / int(format.FramesPerPacket)
	for i := 0; i < packets; i++ {
		if err := rec.AppendPacket(silence); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		// A checkpoint every ten packets leaves an openable file at all times.
		if (i+1)%10 == 0 {
			if err := rec.Checkpoint(); err != nil {
				fmt.Fprintln(os.Stderr, err)
				os.Exit(1)
			}
		}
	}
	if err := rec.Close(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Printf("wrote %d Opus packets to %s\n", packets, *out)
}
