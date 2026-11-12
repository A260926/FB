package grab

import (
	"context"
	"fmt"
	"net/http"
	"os"
)

// ExampleClient_FetchRanges captures the header and the trailing index of a
// large archive without downloading the whole file: only the two slices are
// fetched, staged and written to their original offsets in a sparse local
// file, which the caller can then parse.
func ExampleClient_FetchRanges() {
	const archiveURL = "https://example.com/catalog.tar"

	// The archive layout is known up front, e.g. from a previous HEAD
	// request: a header block at the start and an index at the very end.
	const (
		headerLen = 512
		indexLen  = 64 * 1024
	)
	archiveSize := int64(1 << 40)

	template, err := http.NewRequest(http.MethodGet, archiveURL, nil)
	if err != nil {
		panic(err)
	}

	// The destination stays sparse: only the fetched slices are written,
	// at their original offsets, and the file is neither truncated nor
	// closed by FetchRanges.
	dst, err := os.CreateTemp("", "archive-slices-*")
	if err != nil {
		panic(err)
	}
	defer os.Remove(dst.Name())
	defer dst.Close()

	client := NewClient()
	result, err := client.FetchRanges(context.Background(), template, dst, []ByteRange{
		{Start: 0, End: headerLen},                        // file header
		{Start: archiveSize - indexLen, End: archiveSize}, // trailing index
	}, RangeOptions{BatchSize: 2, TempDir: os.TempDir()})
	if err != nil {
		panic(err)
	}

	fmt.Printf("captured %d slices of a %d byte archive (ETag %s)\n",
		len(result.Ranges), result.Size, result.ETag)
}
