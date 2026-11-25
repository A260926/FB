// Command defragmentgaps converts a fragmented MP4 file into a progressive
// MP4 file while preserving recording gaps as blank movie time (empty edits)
// instead of extending the previous sample's duration.
//
// Usage:
//
//	defragmentgaps <inFile> <outFile>
package main

import (
	"fmt"
	"os"

	"github.com/Eyevinn/mp4ff/mp4"
)

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintf(os.Stderr, "usage: %s <inFile> <outFile>\n", os.Args[0])
		os.Exit(2)
	}
	if err := run(os.Args[1], os.Args[2]); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
}

func run(inPath, outPath string) error {
	ifh, err := os.Open(inPath)
	if err != nil {
		return fmt.Errorf("could not open input file: %w", err)
	}
	defer ifh.Close()
	parsed, err := mp4.DecodeFile(ifh, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
	if err != nil {
		return fmt.Errorf("could not decode mp4 file: %w", err)
	}
	ofh, err := os.Create(outPath)
	if err != nil {
		return fmt.Errorf("could not create output file: %w", err)
	}
	if err := mp4.DefragmentPreserveGaps(parsed, ifh, ofh); err != nil {
		ofh.Close()
		return fmt.Errorf("could not preserve gaps while defragmenting: %w", err)
	}
	if err := ofh.Close(); err != nil {
		return fmt.Errorf("could not close output file: %w", err)
	}
	return nil
}
