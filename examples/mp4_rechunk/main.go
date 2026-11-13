// Command mp4_rechunk rewrites a finished MP4 into a fast-start layout: the
// moov box is moved to the front and media is re-interleaved into multi-sample
// chunks of at most the requested duration. Compressed samples and all timing
// tables are preserved; nothing is decoded or re-muxed.
package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"time"

	"github.com/nareix/joy4/format/mp4"
)

func main() {
	inPath := flag.String("in", "", "source MP4 file (plain, non-fragmented, self-contained)")
	outPath := flag.String("out", "", "destination MP4 file (created empty)")
	chunk := flag.Duration("chunk", time.Second, "maximum decode duration per output chunk")
	flag.Parse()

	if *inPath == "" || *outPath == "" {
		fmt.Fprintln(os.Stderr, "usage: mp4_rechunk -in input.mp4 -out output.mp4 [-chunk 1s]")
		flag.PrintDefaults()
		os.Exit(2)
	}

	in, err := os.Open(*inPath)
	if err != nil {
		log.Fatalf("open input: %v", err)
	}
	defer in.Close()

	out, err := os.Create(*outPath)
	if err != nil {
		log.Fatalf("create output: %v", err)
	}
	if err := mp4.Rechunk(in, out, *chunk); err != nil {
		out.Close()
		// On a validation error nothing has been written to out.
		log.Fatalf("rechunk: %v", err)
	}
	if err := out.Close(); err != nil {
		log.Fatalf("close output: %v", err)
	}

	fmt.Printf("rechunked %s -> %s (chunk duration %s)\n", *inPath, *outPath, *chunk)
}
