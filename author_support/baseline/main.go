package main

import (
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"reflect"
	"strings"

	colly "github.com/gocolly/colly/v2"
)

func must(ok bool, msg string) {
	if !ok {
		panic(msg)
	}
}
func main() {
	cache, err := os.MkdirTemp(".build", "redirect-cache-")
	if err != nil {
		panic(err)
	}
	defer os.RemoveAll(cache)
	var received []string
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, e := io.ReadAll(r.Body)
		if e != nil {
			panic(e)
		}
		received = append(received, r.Method+" "+r.URL.Path+" body="+string(b))
		switch r.URL.Path {
		case "/entry":
			http.Redirect(w, r, "/catalog/page/", http.StatusFound)
		case "/submit":
			http.Redirect(w, r, "/receipt/", http.StatusSeeOther)
		case "/keep":
			http.Redirect(w, r, "/kept/", http.StatusTemporaryRedirect)
		default:
			w.Header().Set("Content-Type", "text/html")
			fmt.Fprint(w, `<a href="item">item</a>`)
		}
	}))
	defer s.Close()
	collect := func() (string, string) {
		c := colly.NewCollector(colly.IgnoreRobotsTxt(), colly.CacheDir(cache))
		var u, link string
		c.OnResponse(func(r *colly.Response) { u = r.Request.URL.Path })
		c.OnHTML("a", func(e *colly.HTMLElement) { link = e.Request.AbsoluteURL(e.Attr("href")) })
		if err := c.Visit(s.URL + "/entry"); err != nil {
			panic(err)
		}
		return u, strings.TrimPrefix(link, s.URL)
	}
	u, l := collect()
	must(u == "/catalog/page/" && l == "/catalog/page/item", "live redirect normal control")
	fmt.Printf("LIVE_GET url=%s link=%s\n", u, l)
	n := len(received)
	u, l = collect()
	must(len(received) == n, "cache unexpectedly used network")
	must(u == "/entry" && l == "/item", "cached URL defect changed")
	fmt.Printf("CACHED_GET url=%s link=%s network_delta=%d\n", u, l, len(received)-n)
	c := colly.NewCollector(colly.IgnoreRobotsTxt())
	retried := false
	c.OnResponse(func(r *colly.Response) {
		if !retried {
			retried = true
			must(r.Request.Method == "POST" && r.Request.URL.Path == "/receipt/", "303 metadata defect changed")
			if err := r.Request.Retry(); err != nil {
				panic(err)
			}
		}
	})
	received = nil
	if err := c.PostRaw(s.URL+"/submit", []byte("q=red")); err != nil {
		panic(err)
	}
	must(reflect.DeepEqual(received, []string{"POST /submit body=q=red", "GET /receipt/ body=", "POST /receipt/ body=q=red"}), fmt.Sprint(received))
	fmt.Printf("POST_303_RETRY %q\n", received)
	c = colly.NewCollector(colly.IgnoreRobotsTxt())
	retried = false
	received = nil
	c.OnResponse(func(r *colly.Response) {
		if !retried {
			retried = true
			if err := r.Request.Retry(); err != nil {
				panic(err)
			}
		}
	})
	if err := c.PostRaw(s.URL+"/keep", []byte("q=blue")); err != nil {
		panic(err)
	}
	must(reflect.DeepEqual(received, []string{"POST /keep body=q=blue", "POST /kept/ body=q=blue", "POST /kept/ body=q=blue"}), fmt.Sprint(received))
	fmt.Printf("POST_307_CONTROL %q\n", received)
	fmt.Println("BASELINE_OK Z1153")
}
