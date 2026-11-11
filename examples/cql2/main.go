// CQL2 example: build a filter expression with go-cql2 and search by it.
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"time"

	cql2 "github.com/exergy-dev/go-cql2"
	cql2json "github.com/exergy-dev/go-cql2/json"
	stacclient "github.com/robert-malhotra/go-stac-client/pkg/client"
)

func main() {
	cli, err := stacclient.NewClient("https://earth-search.aws.element84.com/v1")
	if err != nil {
		log.Fatal(err)
	}

	expr := cql2.And(
		cql2.Lt("eo:cloud_cover", 5),
		cql2.Eq("collection", "sentinel-2-l2a"),
	)
	filterJSON, err := cql2json.Encode(expr.Node())
	if err != nil {
		log.Fatal(err)
	}

	params := stacclient.SearchParams{
		Filter:     json.RawMessage(filterJSON),
		FilterLang: "cql2-json",
		Limit:      5,
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	for page, err := range cli.SearchPages(ctx, params) {
		if err != nil {
			log.Fatalf("search: %v", err)
		}
		if page.NumberMatched != nil {
			fmt.Printf("matched=%d returned=%d\n", *page.NumberMatched, *page.NumberReturned)
		}
		for _, it := range page.Items {
			fmt.Printf("- %s\n", it.ID)
		}
		break // just the first page
	}
}
