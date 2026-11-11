// Basic example: list collections and run a bbox+datetime search.
package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"time"

	stacclient "github.com/robert-malhotra/go-stac-client/pkg/client"
)

func main() {
	cli, err := stacclient.NewClient("https://earth-search.aws.element84.com/v1",
		stacclient.WithTimeout(15*time.Second),
	)
	if err != nil {
		log.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	for col, err := range cli.GetCollections(ctx) {
		if err != nil {
			log.Fatalf("list collections: %v", err)
		}
		fmt.Printf("- %s\n", col.ID)
	}

	params := stacclient.SearchParams{
		Collections: []string{"sentinel-2-l2a"},
		Bbox:        []float64{-123.3, 45.2, -122.5, 46.0},
		Datetime:    "2024-01-01T00:00:00Z/2024-02-01T00:00:00Z",
		Limit:       10,
	}
	count := 0
	for it, err := range cli.Search(ctx, params) {
		if errors.Is(err, context.DeadlineExceeded) {
			fmt.Println("deadline reached")
			return
		}
		if err != nil {
			log.Fatalf("search: %v", err)
		}
		count++
		if count >= 20 {
			break
		}
		fmt.Printf("item %s\n", it.ID)
	}
	fmt.Printf("retrieved %d items\n", count)
}
