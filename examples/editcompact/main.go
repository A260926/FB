// editcompact rewrites a progressive mp4 file so that only the media its
// edit lists reference remains in the file: the sample tables are rebuilt
// and the mdat holds just the used samples. See mp4.CompactEditedFile.
//
// Usage:
//
//	editcompact inFile outFile
package main

import (
	"bufio"
	"fmt"
	"os"

	"github.com/Eyevinn/mp4ff/mp4"
)

const appName = "editcompact"

func main() {
	if err := run(os.Args); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	if len(args) != 3 {
		return fmt.Errorf("usage: %s inFile outFile", appName)
	}
	inPath, outPath := args[1], args[2]
	ifh, err := os.Open(inPath)
	if err != nil {
		return fmt.Errorf("opening input file: %w", err)
	}
	defer ifh.Close()
	ofh, err := os.Create(outPath)
	if err != nil {
		return fmt.Errorf("creating output file: %w", err)
	}
	ow := bufio.NewWriter(ofh)
	if err := mp4.CompactEditedFile(ifh, ow); err != nil {
		ofh.Close()
		return fmt.Errorf("compacting %s: %w", inPath, err)
	}
	if err := ow.Flush(); err != nil {
		ofh.Close()
		return fmt.Errorf("writing %s: %w", outPath, err)
	}
	return ofh.Close()
}
