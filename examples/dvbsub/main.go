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

func run(input, outputDir string) error {
	f, err := os.Open(input)
	if err != nil {
		return err
	}
	defer f.Close()
	frames, err := astisub.ReadFromDVBSubtitles(f, astisub.DVBSubtitleOptions{
		PID:    256,
		PageID: 1,
		Width:  8,
		Height: 8,
		EndAt:  4 * time.Second,
	})
	if err != nil {
		return err
	}
	var entries []indexEntry
	for i, frame := range frames {
		name := fmt.Sprintf("frame-%04d.png", i+1)
		out, err := os.Create(filepath.Join(outputDir, name))
		if err != nil {
			return err
		}
		if err := png.Encode(out, frame.Image); err != nil {
			out.Close()
			return err
		}
		if err := out.Close(); err != nil {
			return err
		}
		entries = append(entries, indexEntry{
			File:    name,
			StartAt: frame.StartAt.Nanoseconds(),
			EndAt:   frame.EndAt.Nanoseconds(),
		})
	}
	if entries == nil {
		entries = []indexEntry{}
	}
	b, err := json.Marshal(entries)
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(outputDir, "index.json"), b, 0644)
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: dvbsub input.ts output-dir")
		os.Exit(2)
	}
	if err := run(os.Args[1], os.Args[2]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
