// Command dvbsub decodes the DVB bitmap subtitles of a transport stream into
// PNG overlay frames and an index.json manifest.
//
// Usage:
//
//	go run ./examples/dvbsub input.ts output-dir
//
// The stream is read with fixed options: PID 256, subtitle page 1, an 8x8
// frame and a closed end at 4 seconds.
package main

import (
	"encoding/json"
	"fmt"
	"image/png"
	"os"
	"path/filepath"
	"time"

	"github.com/asticode/go-astisub"
)

type indexEntry struct {
	File    string `json:"file"`
	StartAt int64  `json:"start_at"`
	EndAt   int64  `json:"end_at"`
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintf(os.Stderr, "usage: %s input.ts output-dir\n", filepath.Base(os.Args[0]))
		os.Exit(1)
	}
	inputPath, outputDir := os.Args[1], os.Args[2]

	f, err := os.Open(inputPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "opening %s: %v\n", inputPath, err)
		os.Exit(1)
	}
	subs, err := astisub.ReadFromDVBSubtitles(f, astisub.DVBSubtitleOptions{
		PID:    256,
		PageID: 1,
		Width:  8,
		Height: 8,
		EndAt:  4 * time.Second,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "decoding DVB subtitles: %v\n", err)
		os.Exit(1)
	}
	if err = f.Close(); err != nil {
		fmt.Fprintf(os.Stderr, "closing %s: %v\n", inputPath, err)
		os.Exit(1)
	}

	entries := make([]indexEntry, 0, len(subs))
	for i, sub := range subs {
		name := fmt.Sprintf("frame-%04d.png", i+1)
		out, err := os.Create(filepath.Join(outputDir, name))
		if err != nil {
			fmt.Fprintf(os.Stderr, "creating %s: %v\n", name, err)
			os.Exit(1)
		}
		if err = png.Encode(out, sub.Image); err != nil {
			fmt.Fprintf(os.Stderr, "encoding %s: %v\n", name, err)
			os.Exit(1)
		}
		if err = out.Close(); err != nil {
			fmt.Fprintf(os.Stderr, "closing %s: %v\n", name, err)
			os.Exit(1)
		}
		entries = append(entries, indexEntry{
			File:    name,
			StartAt: sub.StartAt.Nanoseconds(),
			EndAt:   sub.EndAt.Nanoseconds(),
		})
	}

	index, err := os.Create(filepath.Join(outputDir, "index.json"))
	if err != nil {
		fmt.Fprintf(os.Stderr, "creating index.json: %v\n", err)
		os.Exit(1)
	}
	enc := json.NewEncoder(index)
	if err = enc.Encode(entries); err != nil {
		fmt.Fprintf(os.Stderr, "writing index.json: %v\n", err)
		os.Exit(1)
	}
	if err = index.Close(); err != nil {
		fmt.Fprintf(os.Stderr, "closing index.json: %v\n", err)
		os.Exit(1)
	}
}
