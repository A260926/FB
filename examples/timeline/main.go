// Command timeline renders a short two-track arrangement to a stereo PCM16
// WAV file, then swaps in a second arrangement partway through.
//
// Usage:
//
//	go run ./examples/timeline output.wav
package main

import (
	"fmt"
	"math"
	"os"

	"github.com/gopxl/beep/v2"
	"github.com/gopxl/beep/v2/wav"
)

const (
	sampleRate = beep.SampleRate(8000)
	amplitude  = 0.3
	totalLen   = 2048
)

// tone returns a clip that plays a sine of the given frequency at the given
// phase. Each Open call builds a brand new reader over fresh material, as
// Timeline requires.
func tone(freq, phase float64, start, duration, material int) beep.TimelineClip {
	open := func() (beep.StreamSeekCloser, error) {
		buf := beep.NewBuffer(beep.Format{SampleRate: sampleRate, NumChannels: 2, Precision: 2})
		pos := 0
		buf.Append(beep.Take(material, beep.StreamerFunc(func(samples [][2]float64) (int, bool) {
			for i := range samples {
				x := math.Sin(2*math.Pi*freq*float64(pos)/float64(sampleRate) + phase)
				samples[i] = [2]float64{x, -x}
				pos++
			}
			return len(samples), true
		})))
		return bufferReader{buf.Streamer(0, buf.Len())}, nil
	}
	return beep.TimelineClip{
		Open:     open,
		Start:    start,
		In:       0,
		Out:      material,
		Duration: duration,
		Gain:     amplitude,
	}
}

// bufferReader adapts a Buffer streamer, which is in memory, to the
// StreamSeekCloser interface Timeline expects.
type bufferReader struct {
	beep.StreamSeeker
}

func (bufferReader) Close() error { return nil }

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintf(os.Stderr, "usage: %s output.wav\n", os.Args[0])
		os.Exit(2)
	}
	if err := run(os.Args[1]); err != nil {
		fmt.Fprintln(os.Stderr, "timeline:", err)
		os.Exit(1)
	}
}

func run(path string) error {
	// Two misaligned tracks in the first arrangement: a higher tone starts
	// later and is shorter than a lower tone.
	first := []beep.TimelineClip{
		tone(440, 0, 0, 1400, 1400),
		tone(660, math.Pi/3, 320, 900, 900),
	}
	timeline, err := beep.NewTimeline(sampleRate, first)
	if err != nil {
		return err
	}
	defer timeline.Close()

	// A second arrangement of the same total length is scheduled at frame 512
	// with a short cross-fade, so the output never falls silent mid-file.
	second := []beep.TimelineClip{
		tone(330, 0, 0, 2048, 2048),
		tone(880, math.Pi/2, 256, 1200, 1200),
	}
	if err := timeline.Schedule(second, 512, 128); err != nil {
		return err
	}

	f, err := os.Create(path)
	if err != nil {
		return err
	}

	format := beep.Format{SampleRate: sampleRate, NumChannels: 2, Precision: 2}
	if err := wav.Encode(f, beep.Take(totalLen, timeline), format); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}
