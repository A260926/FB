package grab

import (
	"context"
	"fmt"
	"net/http"
	"os"
)

// ExampleClient_FetchRanges fetches only a file header and its trailing index
// instead of downloading a whole large file. The destination is a sparse file,
// so the two pieces land at their original offsets while everything in between
// keeps its previous (empty) contents.
//
// The offset of the trailing index depends on the resource length, which is
// only learned from the first response: the first call fetches the header and
// reports Size, and the second call fetches the tail relative to it.
func ExampleClient_FetchRanges() {
	client := NewClient()

	template, err := http.NewRequest(http.MethodGet, "http://example.com/archive.bin", nil)
	if err != nil {
		panic(err)
	}

	const (
		headerSize = 4096 // leading metadata block
		indexSize  = 8192 // trailing index block
	)

	dst, err := os.CreateTemp("", "sparse-*.bin")
	if err != nil {
		panic(err)
	}
	defer os.Remove(dst.Name())
	defer dst.Close()

	// First pass: capture the header and learn the total length and ETag.
	head, err := client.FetchRanges(
		context.Background(),
		template,
		dst,
		[]ByteRange{{Start: 0, End: headerSize}},
		RangeOptions{BatchSize: 1},
	)
	if err != nil {
		panic(err)
	}

	if head.Size <= indexSize {
		return
	}

	// Second pass: fetch the tail index relative to the reported size.
	tail, err := client.FetchRanges(
		context.Background(),
		template,
		dst,
		[]ByteRange{{Start: head.Size - indexSize, End: head.Size}},
		RangeOptions{BatchSize: 1},
	)
	if err != nil {
		panic(err)
	}

	// The two calls are independent, so pin the representation through the
	// strong validators they report when the server provides one.
	if head.ETag != "" && head.ETag != tail.ETag {
		panic("resource changed between the header and index fetches")
	}

	fmt.Printf("resource %d bytes; header at [0,%d), index at [%d,%d), ETag %s\n",
		tail.Size, headerSize, tail.Size-indexSize, tail.Size, tail.ETag)
}
