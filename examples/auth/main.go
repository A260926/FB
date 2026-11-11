// Authentication example: bearer token + typed error handling.
package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"time"

	stacclient "github.com/robert-malhotra/go-stac-client/pkg/client"
)

func main() {
	token := os.Getenv("STAC_TOKEN")
	cli, err := stacclient.NewClient("https://example.com/stac/v1",
		stacclient.WithBearerToken(token),
		stacclient.WithTimeout(15*time.Second),
	)
	if err != nil {
		log.Fatal(err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	_, err = cli.GetItem(ctx, "secret-collection", "secret-item")
	switch {
	case err == nil:
		fmt.Println("got the item")
	case errors.Is(err, stacclient.ErrUnauthorized):
		fmt.Fprintln(os.Stderr, "auth failed; refresh token")
		os.Exit(1)
	case errors.Is(err, stacclient.ErrNotFound):
		fmt.Fprintln(os.Stderr, "item not found")
		os.Exit(1)
	case errors.Is(err, stacclient.ErrRateLimited):
		var herr *stacclient.HTTPError
		_ = errors.As(err, &herr)
		fmt.Fprintf(os.Stderr, "rate limited; Retry-After=%s\n", herr.RetryAfter)
		os.Exit(1)
	default:
		log.Fatal(err)
	}
}
