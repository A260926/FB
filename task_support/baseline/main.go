package main

import (
	"fmt"
	"github.com/temoto/robotstxt"
	"io"
	"net/http"
	"strings"
)

type observed struct {
	r          io.Reader
	max, total int
}

func (r *observed) Read(p []byte) (int, error) {
	if len(p) > r.max {
		r.max = len(p)
	}
	n, e := r.r.Read(p)
	r.total += n
	return n, e
}
func (*observed) Close() error { return nil }
func main() {
	text := "\xef\xbb\xbfUser-agent: a\r\nUser-agent: b\r\nDisallow: /私有\r\nAllow: /私有/public\r\nCrawl-delay: 0.25\r\nSitemap: https://site.test/map.xml\r\n" + strings.Repeat("# ignored padding\n", 200000) + "User-agent: b\nDisallow: /admin"
	r := &observed{r: strings.NewReader(text)}
	data, err := robotstxt.FromResponse(&http.Response{StatusCode: 200, Body: r})
	if err != nil {
		panic(err)
	}
	if data.TestAgent("/私有/secret", "a") || !data.TestAgent("/私有/public/page", "b") || data.TestAgent("/admin", "b") {
		panic("policy mismatch")
	}
	if len(data.Sitemaps) != 1 || data.FindGroup("a").CrawlDelay.String() != "250ms" {
		panic("metadata mismatch")
	}
	fmt.Println("NORMAL_POLICY=3")
	fmt.Printf("INPUT_BYTES=%d MAX_READ_REQUEST=%d\n", r.total, r.max)
	fmt.Println("CURRENT_PATH=FromResponse/ReadAll/FromBytes/scanAll/parseAll")
}
