// Example: collect metadata only from the page's main article instead of
// merging every JSON-LD object (including recommended articles) on the page.
package main

import (
	"fmt"
	"log"
	nurl "net/url"
	"strings"

	"github.com/markusmobius/go-trafilatura"
)

const samplePage = `<!DOCTYPE html>
<html>
<head>
	<script type="application/ld+json">
	{
		"@context": "https://schema.org",
		"@graph": [
			{
				"@type": "WebPage",
				"@id": "https://example.invalid/story",
				"mainEntity": {"@id": "#main"}
			},
			{
				"@type": "NewsArticle",
				"@id": "#recommend",
				"headline": "Recommended article",
				"author": {"@type": "Person", "name": "Other Writer"}
			},
			{
				"@type": "NewsArticle",
				"@id": "#main",
				"headline": "Main article",
				"author": {"@id": "#writer"}
			},
			{"@type": "Person", "@id": "#writer", "name": "Main Writer"}
		]
	}
	</script>
</head>
<body>
	<article>
		<p>The main article body is extracted exactly as usual; only the JSON-LD
		metadata is restricted to the article referenced by the WebPage's
		mainEntity relation.</p>
	</article>
</body>
</html>`

func main() {
	// UseMainEntity requires an absolute HTTP(S) URL: it is the page identity
	// used to resolve relative @id values and to match the WebPage.
	parsedURL, err := nurl.ParseRequestURI("https://example.invalid/story")
	if err != nil {
		log.Fatalf("invalid url: %v", err)
	}

	opts := trafilatura.Options{
		OriginalURL:   parsedURL,
		UseMainEntity: true,
	}

	result, err := trafilatura.Extract(strings.NewReader(samplePage), opts)
	if err != nil {
		log.Fatalf("failed to extract: %v", err)
	}

	// Without UseMainEntity the title would be "Recommended article".
	fmt.Println("title: ", result.Metadata.Title)
	fmt.Println("author:", result.Metadata.Author)
}
