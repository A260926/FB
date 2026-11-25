// Command avcedits exports MP4 edit-list segments as independently startable
// Annex B AVC (.h264) output plus a JSON AVCEditReport.
//
// Usage:
//
//	avcedits -track ID input.mp4 output.h264 output.json
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"

	mp4 "github.com/abema/go-mp4"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "avcedits:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	fs := flag.NewFlagSet("avcedits", flag.ContinueOnError)
	var trackID uint
	fs.UintVar(&trackID, "track", 0, "track ID to export (required)")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if trackID == 0 {
		return errors.New("-track ID is required")
	}
	rest := fs.Args()
	if len(rest) != 3 {
		return errors.New("usage: avcedits -track ID input.mp4 output.h264 output.json")
	}
	inputPath, h264Path, jsonPath := rest[0], rest[1], rest[2]

	in, err := os.Open(inputPath)
	if err != nil {
		return err
	}
	defer in.Close()

	out, err := os.Create(h264Path)
	if err != nil {
		return err
	}
	report, exportErr := mp4.ExportAVCEdits(in, out, uint32(trackID))
	closeErr := out.Close()
	if exportErr != nil {
		return exportErr
	}
	if closeErr != nil {
		return closeErr
	}

	js, err := json.MarshalIndent(report, "", "  ")
	if err != nil {
		return err
	}
	js = append(js, '\n')
	if err := os.WriteFile(jsonPath, js, 0o644); err != nil {
		return err
	}
	return nil
}
