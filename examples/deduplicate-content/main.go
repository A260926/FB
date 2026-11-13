//go:build ignore
// +build ignore

// This example shows how Parser.DeduplicateContent removes a second,
// device-specific copy of the same article body. It works fully offline
// on an in-memory HTML document. Run it with:
//
//	go run examples/deduplicate-content/main.go
package main

import (
	"fmt"
	"net/url"
	"strings"

	readability "github.com/go-shiori/go-readability"
)

func paragraph(id, marker string) string {
	return fmt.Sprintf(`<p id="%s">%s %s</p>`, id, marker,
		strings.Repeat("Field measurements, collection constraints and independent observations in ordinary prose. ", 5))
}

func main() {
	// A page that ships the same body twice, in different shells.
	source := `<html><head><title>Field report</title></head><body><main><article><div>` +
		paragraph("a1", "ALPHA_MARK") + paragraph("b1", "BETA_MARK") + paragraph("c1", "GAMMA_MARK") +
		`</div><section>` +
		paragraph("a2", "ALPHA_MARK") + paragraph("b2", "BETA_MARK") + paragraph("c2", "GAMMA_MARK") +
		`</section></article></main></body></html>`

	pageURL, _ := url.Parse("https://capture.invalid/reports/current")

	// Default behavior: both copies survive.
	defaultParser := readability.NewParser()
	defaultArticle, err := defaultParser.Parse(strings.NewReader(source), pageURL)
	if err != nil {
		panic(err)
	}

	// Opt in: duplicated runs of three or more identical <p>/<pre>
	// units are removed before candidate scoring, and in-page "#id"
	// links are repointed to the kept copy when that is unambiguous.
	dedupParser := readability.NewParser()
	dedupParser.DeduplicateContent = true
	dedupArticle, err := dedupParser.Parse(strings.NewReader(source), pageURL)
	if err != nil {
		panic(err)
	}

	fmt.Printf("default parser : ALPHA_MARK x%d, length %d\n",
		strings.Count(defaultArticle.TextContent, "ALPHA_MARK"), defaultArticle.Length)
	fmt.Printf("dedup parser   : ALPHA_MARK x%d, length %d\n",
		strings.Count(dedupArticle.TextContent, "ALPHA_MARK"), dedupArticle.Length)
	fmt.Printf("kept anchor present: %v\n", strings.Contains(dedupArticle.Content, `id="a1"`))
	fmt.Printf("duplicate removed  : %v\n", !strings.Contains(dedupArticle.Content, `id="a2"`))
}
