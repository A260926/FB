// Command defragmenter converts a fragmented MP4 file into a progressive
// (moov-before-mdat) MP4 file. Sample bytes are copied verbatim, and the
// gradual-decoding-recovery sample groups roll and prol are preserved on the
// samples that survive any retransmission overlap.
//
// Usage:
//
//	defragmenter [-tracks id,id,...] <infile> <outfile>
package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"strconv"
	"strings"

	"github.com/Eyevinn/mp4ff/mp4"
)

func main() {
	if err := run(os.Args); err != nil {
		fmt.Fprintf(os.Stderr, "defragmenter: %v\n", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	fs := flag.NewFlagSet(args[0], flag.ContinueOnError)
	tracks := fs.String("tracks", "", "comma-separated track IDs to keep (default: all tracks)")
	if err := fs.Parse(args[1:]); err != nil {
		return err
	}
	if fs.NArg() != 2 {
		return errors.New("usage: defragmenter [-tracks id,id,...] <infile> <outfile>")
	}

	in, err := os.Open(fs.Arg(0))
	if err != nil {
		return fmt.Errorf("open input: %w", err)
	}
	defer in.Close()

	parsed, err := mp4.DecodeFile(in, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
	if err != nil {
		return fmt.Errorf("decode input: %w", err)
	}

	out, err := os.Create(fs.Arg(1))
	if err != nil {
		return fmt.Errorf("create output: %w", err)
	}
	defer out.Close()

	trackIDs, err := parseTrackIDs(*tracks)
	if err != nil {
		return err
	}
	if len(trackIDs) == 0 {
		err = mp4.Defragment(parsed, in, out)
	} else {
		err = mp4.DefragmentTracks(parsed, in, out, trackIDs...)
	}
	if err != nil {
		return fmt.Errorf("defragment: %w", err)
	}
	return nil
}

func parseTrackIDs(value string) ([]uint32, error) {
	if strings.TrimSpace(value) == "" {
		return nil, nil
	}
	var ids []uint32
	for _, part := range strings.Split(value, ",") {
		id, err := strconv.ParseUint(strings.TrimSpace(part), 10, 32)
		if err != nil {
			return nil, fmt.Errorf("invalid track ID %q: %w", part, err)
		}
		ids = append(ids, uint32(id))
	}
	return ids, nil
}
