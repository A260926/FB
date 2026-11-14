// Example frames collects a page together with the documents embedded in its
// iframes and prints each frame's address, base and heading.
package main

import (
	"context"
	"fmt"
	"os"

	"github.com/anaskhan96/soup"
)

func main() {
	page, err := soup.CollectFramesWithClient(context.Background(), "https://vendor.example/catalog", nil)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	printPage(page, 0)
}

func printPage(page *soup.FramePage, depth int) {
	indent := ""
	for i := 0; i < depth; i++ {
		indent += "  "
	}
	fmt.Printf("%s%s (base %s)", indent, page.URL, page.BaseURL)
	if page.Cycle {
		fmt.Println(" [cycle]")
		return
	}
	if h := page.Document.Find("h1"); h.Error == nil {
		fmt.Printf(" %q", h.Text())
	}
	fmt.Println()
	for i := range page.Frames {
		printPage(&page.Frames[i], depth+1)
	}
}
