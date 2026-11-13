package gocrawl

import (
	"bytes"
	"compress/gzip"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"reflect"
	"sort"
	"strings"
	"sync"
	"testing"

	"github.com/PuerkitoBio/goquery"
)

// sitemapResponse describes how the fake fetcher serves a given URL.
type sitemapResponse struct {
	body     string // content served for this URL
	gzip     bool   // serve the body gzip-compressed (magic bytes, no Content-Encoding header)
	finalURL string // if set, the response's Request.URL, simulating a redirect followed by Fetch
	status   int    // HTTP status code, defaults to 200
}

// sitemapRecorder is an Extender that serves pre-programmed responses from
// its Fetch implementation and records every request and hook call, so that
// tests can assert on the crawl's behaviour.
type sitemapRecorder struct {
	DefaultExtender

	responses    map[string]sitemapResponse // key: host + path
	cachedRobots map[string]string          // host -> robots.txt content served from cache

	mu         sync.Mutex
	fetches    []string
	visits     []string
	filters    []string
	enqueued   []string
	disallowed []string
	errors     []*CrawlError
}

// sitemapTestKey builds the record key of a URL (host + path).
func sitemapTestKey(u *url.URL) string {
	if u == nil {
		return ""
	}
	return u.Host + u.Path
}

// sitemapMark is appended to a record key when the URL is a sitemap control
// request, so tests can verify the identity seen by the hooks.
const sitemapMark = "#sitemap"

func (r *sitemapRecorder) record(dst *[]string, key string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	*dst = append(*dst, key)
}

// Fetch serves the pre-programmed response for the requested URL, recording
// the request. Unknown URLs are served a 404.
func (r *sitemapRecorder) Fetch(ctx *URLContext, userAgent string, headRequest bool) (*http.Response, error) {
	u := ctx.URL()
	key := sitemapTestKey(u)
	r.record(&r.fetches, key)

	spec, ok := r.responses[key]
	if !ok {
		req, _ := http.NewRequest("GET", u.String(), nil)
		return &http.Response{
			StatusCode: 404,
			Status:     "404 Not Found",
			Header:     make(http.Header),
			Body:       io.NopCloser(strings.NewReader("not found")),
			Request:    req,
		}, nil
	}

	body := spec.body
	if spec.gzip {
		var buf bytes.Buffer
		zw := gzip.NewWriter(&buf)
		if _, err := zw.Write([]byte(body)); err != nil {
			return nil, err
		}
		if err := zw.Close(); err != nil {
			return nil, err
		}
		body = buf.String()
	}

	final := u.String()
	if spec.finalURL != "" {
		final = spec.finalURL
	}
	req, _ := http.NewRequest("GET", final, nil)
	status := spec.status
	if status == 0 {
		status = 200
	}
	return &http.Response{
		StatusCode: status,
		Status:     fmt.Sprintf("%d Status", status),
		Header:     make(http.Header),
		Body:       io.NopCloser(strings.NewReader(body)),
		Request:    req,
	}, nil
}

// RequestRobots serves robots.txt content from the cache when one is
// registered for the host, otherwise asks for a real fetch.
func (r *sitemapRecorder) RequestRobots(ctx *URLContext, robotAgent string) ([]byte, bool) {
	if body, ok := r.cachedRobots[ctx.URL().Host]; ok {
		return []byte(body), false
	}
	return nil, true
}

// Filter records the call and applies the default visited-based policy.
func (r *sitemapRecorder) Filter(ctx *URLContext, isVisited bool) bool {
	key := sitemapTestKey(ctx.URL())
	if ctx.IsSitemapURL() {
		key += sitemapMark
	}
	r.record(&r.filters, key)
	return !isVisited
}

// Enqueued records the call, flagging sitemap control requests.
func (r *sitemapRecorder) Enqueued(ctx *URLContext) {
	key := sitemapTestKey(ctx.URL())
	if ctx.IsSitemapURL() {
		key += sitemapMark
	}
	r.record(&r.enqueued, key)
}

// Visit records the call and lets gocrawl harvest the links of the document.
func (r *sitemapRecorder) Visit(ctx *URLContext, res *http.Response, doc *goquery.Document) (interface{}, bool) {
	r.record(&r.visits, sitemapTestKey(ctx.URL()))
	return nil, true
}

// Visited is a no-op.
func (r *sitemapRecorder) Visited(ctx *URLContext, harvested interface{}) {}

// Disallowed records the call.
func (r *sitemapRecorder) Disallowed(ctx *URLContext) {
	r.record(&r.disallowed, sitemapTestKey(ctx.URL()))
}

// Error records the crawling error.
func (r *sitemapRecorder) Error(err *CrawlError) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.errors = append(r.errors, err)
}

// crawl runs a crawler against the recorder's fake web, with sitemap
// discovery enabled unless overridden by configure.
func (r *sitemapRecorder) crawl(t *testing.T, seeds interface{}, configure func(*Options)) error {
	t.Helper()
	opts := NewOptions(r)
	opts.CrawlDelay = 0
	opts.LogFlags = 0
	opts.SameHostOnly = false
	opts.DiscoverSitemaps = true
	if configure != nil {
		configure(opts)
	}
	return NewCrawlerWithOptions(opts).Run(seeds)
}

// fetchCounts returns the number of requests made for each URL key. It must
// be called once the crawl is over.
func (r *sitemapRecorder) fetchCounts() map[string]int {
	counts := make(map[string]int, len(r.fetches))
	for _, f := range r.fetches {
		counts[f]++
	}
	return counts
}

// sitemapIndexXML builds a sitemap index document referencing the given
// sitemap URLs, in the standard namespace if ns is true, in no namespace
// otherwise.
func sitemapIndexXML(ns bool, locs ...string) string {
	var b strings.Builder
	if ns {
		b.WriteString(`<?xml version="1.0" encoding="UTF-8"?><sitemapindex xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">`)
	} else {
		b.WriteString(`<sitemapindex>`)
	}
	for _, loc := range locs {
		b.WriteString("<sitemap><loc>")
		b.WriteString(loc)
		b.WriteString("</loc></sitemap>")
	}
	b.WriteString(`</sitemapindex>`)
	return b.String()
}

// sitemapURLSetXML builds a URL set document referencing the given page
// URLs, in the standard namespace if ns is true, in no namespace otherwise.
func sitemapURLSetXML(ns bool, locs ...string) string {
	var b strings.Builder
	if ns {
		b.WriteString(`<?xml version="1.0" encoding="UTF-8"?><urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">`)
	} else {
		b.WriteString(`<urlset>`)
	}
	for _, loc := range locs {
		b.WriteString("<url><loc>")
		b.WriteString(loc)
		b.WriteString("</loc></url>")
	}
	b.WriteString(`</urlset>`)
	return b.String()
}

// sortedCopy returns a sorted copy of the given slice.
func sortedCopy(s []string) []string {
	out := append([]string(nil), s...)
	sort.Strings(out)
	return out
}

// TestSitemapDispatch exercises the discovery and dispatch of sitemap control
// requests through the crawler: robots.txt declarations, nested indexes,
// loops, dual page/sitemap identity of a same URL, cached robots.txt content,
// resolution against the response URL, and the MaxVisits/completion
// behaviour when sitemaps are mixed with pages.
func TestSitemapDispatch(t *testing.T) {
	t.Run("DiscoveryAcrossHosts", func(t *testing.T) {
		rec := &sitemapRecorder{
			responses: map[string]sitemapResponse{
				// The robots.txt response comes from a "redirected" URL, so
				// the relative Sitemap declaration resolves to
				// http://a.test/r/maps/index.xml (and not /maps/index.xml).
				"a.test/robots.txt": {
					body:     "User-agent: *\nAllow: /\nSitemap: maps/index.xml\n",
					finalURL: "http://a.test/r/robots.txt",
				},
				"a.test/start":    {body: `<html><body><a href="/ordinary">o</a></body></html>`},
				"a.test/ordinary": {body: `<html><body>ordinary</body></html>`},
				"a.test/r/maps/index.xml": {body: sitemapIndexXML(true,
					"http://a.test/r/maps/child.xml",
					"/r/maps/index.xml",             // loop back onto itself
					"http://b.test/maps/shared.xml", // sitemap on another host
					"http://a.test/r/maps/broken.xml",
					"  http://a.test/r/maps/gz.xml  ", // padded loc, gzipped body
				)},
				"a.test/r/maps/child.xml": {body: sitemapIndexXML(false, // no namespace
					"http://a.test/r/maps/index.xml", // loop back to the parent
					"http://a.test/r/maps/gc.xml",
				)},
				"a.test/r/maps/gc.xml": {
					// The loc values resolve against this final URL, not
					// against the requested /r/maps/gc.xml.
					finalURL: "http://a.test/final/gc.xml",
					body: sitemapURLSetXML(true,
						"http://a.test/p1",
						"p2",                             // resolves to http://a.test/final/p2
						"http://a.test/r/maps/child.xml", // also a sitemap: dual identity
						"ftp://a.test/nope",              // unsupported scheme, skipped
						"",                               // empty loc, skipped
						"http://a.test/%zz",              // unparseable, skipped
					),
				},
				// Truncated document: the loc read before the error must
				// not produce a page request.
				"a.test/r/maps/broken.xml": {body: `<urlset><url><loc>http://a.test/early</loc></url`},
				"a.test/r/maps/gz.xml":     {gzip: true, body: sitemapURLSetXML(true, "http://a.test/p3")},
				"a.test/p1":                {body: `<html><body>p1</body></html>`},
				"a.test/final/p2":          {body: `<html><body>p2</body></html>`},
				"a.test/p3":                {body: `<html><body>p3</body></html>`},
				"b.test/maps/shared.xml":   {body: sitemapURLSetXML(true, "http://b.test/q1")},
				"b.test/maps/cached.xml":   {body: sitemapURLSetXML(true, "http://b.test/q2")},
				"b.test/q1":                {body: `<html><body>q1</body></html>`},
				"b.test/q2":                {body: `<html><body>q2</body></html>`},
			},
			// b.test's robots.txt comes from the RequestRobots cache, so its
			// relative declarations resolve against the robots.txt URL itself.
			cachedRobots: map[string]string{
				"b.test": "User-agent: *\nAllow: /\nSitemap: maps/shared.xml\nSitemap: maps/cached.xml\n",
			},
		}

		if err := rec.crawl(t, "http://a.test/start", nil); err != nil {
			t.Fatalf("expected crawl to complete with nil error, got %v", err)
		}

		// Every URL is fetched exactly once per identity: sitemap loops and
		// duplicate declarations are downloaded once, and child.xml is fetched
		// both as a sitemap and as a page.
		wantFetches := map[string]int{
			"a.test/robots.txt":        1,
			"a.test/start":             1,
			"a.test/ordinary":          1,
			"a.test/r/maps/index.xml":  1,
			"a.test/r/maps/child.xml":  2, // once as a sitemap, once as a page
			"a.test/r/maps/broken.xml": 1,
			"a.test/r/maps/gz.xml":     1,
			"a.test/r/maps/gc.xml":     1,
			"a.test/p1":                1,
			"a.test/final/p2":          1,
			"a.test/p3":                1,
			"b.test/maps/shared.xml":   1,
			"b.test/maps/cached.xml":   1,
			"b.test/q1":                1,
			"b.test/q2":                1,
		}
		if got := rec.fetchCounts(); !reflect.DeepEqual(got, wantFetches) {
			t.Errorf("fetch counts mismatch:\n got: %v\nwant: %v", got, wantFetches)
		}

		// Only pages are visited: sitemap control requests never trigger Visit.
		wantVisits := []string{
			"a.test/ordinary",
			"a.test/p1",
			"a.test/final/p2",
			"a.test/p3",
			"a.test/r/maps/child.xml", // visited in its page identity
			"a.test/start",
			"b.test/q1",
			"b.test/q2",
		}
		if got, want := sortedCopy(rec.visits), sortedCopy(wantVisits); !reflect.DeepEqual(got, want) {
			t.Errorf("visits mismatch:\n got: %v\nwant: %v", got, want)
		}

		// The truncated sitemap is reported exactly once, as a sitemap error.
		if len(rec.errors) != 1 {
			t.Fatalf("expected 1 error, got %d: %v", len(rec.errors), rec.errors)
		}
		if rec.errors[0].Kind != CekParseSitemap {
			t.Errorf("expected error kind %s, got %s", CekParseSitemap, rec.errors[0].Kind)
		}
		if got := sitemapTestKey(rec.errors[0].Ctx.URL()); got != "a.test/r/maps/broken.xml" {
			t.Errorf("expected error on a.test/r/maps/broken.xml, got %s", got)
		}

		// Enqueued is notified for control requests too, and IsSitemapURL
		// identifies them.
		var mapEnqueued, otherEnqueued []string
		for _, e := range rec.enqueued {
			if strings.HasSuffix(e, sitemapMark) {
				mapEnqueued = append(mapEnqueued, strings.TrimSuffix(e, sitemapMark))
			} else {
				otherEnqueued = append(otherEnqueued, e)
			}
		}
		wantMaps := []string{
			"a.test/r/maps/broken.xml",
			"a.test/r/maps/child.xml",
			"a.test/r/maps/gc.xml",
			"a.test/r/maps/gz.xml",
			"a.test/r/maps/index.xml",
			"b.test/maps/cached.xml",
			"b.test/maps/shared.xml",
		}
		if got := sortedCopy(mapEnqueued); !reflect.DeepEqual(got, wantMaps) {
			t.Errorf("enqueued sitemaps mismatch:\n got: %v\nwant: %v", got, wantMaps)
		}
		// 8 pages + 2 robots.txt (one per host worker)
		if len(otherEnqueued) != 10 {
			t.Errorf("expected 10 non-sitemap enqueued notifications, got %d: %v", len(otherEnqueued), otherEnqueued)
		}

		// Filter only ever sees regular pages, never control requests.
		for _, f := range rec.filters {
			if strings.HasSuffix(f, sitemapMark) {
				t.Errorf("Filter called for sitemap control request %s", f)
			}
		}
		if len(rec.filters) != len(wantVisits) {
			t.Errorf("expected %d Filter calls, got %d: %v", len(wantVisits), len(rec.filters), rec.filters)
		}
	})

	// newMaxVisitsRecorder builds a fake host where the seed page is
	// disallowed by robots.txt, so that every visit comes from the sitemap:
	// robots.txt -> sitemap index -> urlset -> single page.
	newMaxVisitsRecorder := func() *sitemapRecorder {
		return &sitemapRecorder{
			responses: map[string]sitemapResponse{
				"m.test/robots.txt":   {body: "User-agent: *\nDisallow: /seed\nSitemap: http://m.test/sm/index.xml\n"},
				"m.test/sm/index.xml": {body: sitemapIndexXML(true, "http://m.test/sm/map.xml")},
				"m.test/sm/map.xml":   {body: sitemapURLSetXML(true, "http://m.test/only")},
				"m.test/only":         {body: `<html><body>only</body></html>`},
			},
		}
	}
	wantMaxVisitsFetches := []string{"m.test/robots.txt", "m.test/sm/index.xml", "m.test/sm/map.xml", "m.test/only"}

	t.Run("MaxVisitsIgnoresSitemaps", func(t *testing.T) {
		rec := newMaxVisitsRecorder()
		err := rec.crawl(t, "http://m.test/seed", func(o *Options) { o.MaxVisits = 1 })
		if err != ErrMaxVisits {
			t.Fatalf("expected ErrMaxVisits, got %v", err)
		}
		// The sitemap control requests did not consume the visit budget: the
		// single page discovered through the sitemap was visited.
		if !reflect.DeepEqual(rec.fetches, wantMaxVisitsFetches) {
			t.Errorf("fetches mismatch:\n got: %v\nwant: %v", rec.fetches, wantMaxVisitsFetches)
		}
		if !reflect.DeepEqual(rec.visits, []string{"m.test/only"}) {
			t.Errorf("expected exactly 1 visit of m.test/only, got %v", rec.visits)
		}
		if !reflect.DeepEqual(rec.disallowed, []string{"m.test/seed"}) {
			t.Errorf("expected m.test/seed to be disallowed, got %v", rec.disallowed)
		}
	})

	t.Run("CompletionWithSitemaps", func(t *testing.T) {
		rec := newMaxVisitsRecorder()
		err := rec.crawl(t, "http://m.test/seed", nil)
		if err != nil {
			t.Fatalf("expected crawl to complete with nil error, got %v", err)
		}
		if !reflect.DeepEqual(rec.fetches, wantMaxVisitsFetches) {
			t.Errorf("fetches mismatch:\n got: %v\nwant: %v", rec.fetches, wantMaxVisitsFetches)
		}
		if !reflect.DeepEqual(rec.visits, []string{"m.test/only"}) {
			t.Errorf("expected exactly 1 visit of m.test/only, got %v", rec.visits)
		}
	})
}

// TestSitemapDocument runs the crawler against sitemap documents of various
// shapes and checks which page requests are produced and which errors are
// reported, through the full fetch pipeline (not just the parsing function).
func TestSitemapDocument(t *testing.T) {
	// A body larger than the decompressed content limit
	tooLargeBody := `<urlset>` + strings.Repeat("a", maxSitemapBodySize) + `</urlset>`

	// A document with more valid loc entries than the limit
	var manyLocs strings.Builder
	manyLocs.WriteString(`<urlset>`)
	for i := 0; i < maxSitemapLocCount+1; i++ {
		fmt.Fprintf(&manyLocs, "<url><loc>http://t.test/p%d</loc></url>", i)
	}
	manyLocs.WriteString(`</urlset>`)

	cases := []struct {
		name       string
		body       string
		gzip       bool
		extra      map[string]sitemapResponse // additional served URLs (e.g. child sitemaps)
		wantPages  []string                   // expected page requests, as paths
		wantErrors int
	}{
		{
			name:      "StandardNamespaceURLSet",
			body:      sitemapURLSetXML(true, "http://t.test/a", "http://t.test/b"),
			wantPages: []string{"/a", "/b"},
		},
		{
			name:      "NoNamespaceURLSet",
			body:      sitemapURLSetXML(false, "http://t.test/a", "http://t.test/b"),
			wantPages: []string{"/a", "/b"},
		},
		{
			name: "StandardNamespaceIndex",
			body: sitemapIndexXML(true, "http://t.test/sm/child.xml"),
			extra: map[string]sitemapResponse{
				"t.test/sm/child.xml": {body: sitemapURLSetXML(true, "http://t.test/a")},
			},
			wantPages: []string{"/a"},
		},
		{
			name: "NoNamespaceIndex",
			body: sitemapIndexXML(false, "http://t.test/sm/child.xml"),
			extra: map[string]sitemapResponse{
				"t.test/sm/child.xml": {body: sitemapURLSetXML(false, "http://t.test/a")},
			},
			wantPages: []string{"/a"},
		},
		{
			name: "ExtensionNamespaceLocIgnored",
			body: `<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9" xmlns:x="http://example.com/x">` +
				`<url><loc>http://t.test/a</loc><x:loc>http://t.test/ext</x:loc></url></urlset>`,
			wantPages: []string{"/a"},
		},
		{
			name: "NestedExtensionLocIgnored",
			body: `<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9" xmlns:image="http://www.google.com/schemas/sitemap-image/1.1">` +
				`<url><loc>http://t.test/a</loc><image:image><image:loc>http://t.test/img</image:loc></image:image></url></urlset>`,
			wantPages: []string{"/a"},
		},
		{
			name:      "NestedLocIgnored",
			body:      `<urlset><url><loc>http://t.test/a</loc><wrap><loc>http://t.test/nested</loc></wrap></url></urlset>`,
			wantPages: []string{"/a"},
		},
		{
			name:      "EntryTypeMustMatchRoot",
			body:      `<urlset><sitemap><loc>http://t.test/a</loc></sitemap></urlset>`,
			wantPages: nil,
		},
		{
			name:       "UnexpectedRootElement",
			body:       `<feed xmlns="http://www.w3.org/2005/Atom"></feed>`,
			wantErrors: 1,
		},
		{
			name:       "UnexpectedRootNamespace",
			body:       `<urlset xmlns="http://example.com/not-sitemap"><url><loc>http://t.test/a</loc></url></urlset>`,
			wantErrors: 1,
		},
		{
			name:       "NotXML",
			body:       `<html><body>not a sitemap</body></html>`,
			wantErrors: 1,
		},
		{
			name:       "EmptyDocument",
			body:       ``,
			wantErrors: 1,
		},
		{
			// The loc read before the truncation must not be emitted.
			name:       "Truncated",
			body:       `<urlset><url><loc>http://t.test/early</loc></url`,
			wantErrors: 1,
		},
		{
			name:      "GzipBody",
			body:      sitemapURLSetXML(true, "http://t.test/a"),
			gzip:      true,
			wantPages: []string{"/a"},
		},
		{
			name:       "CorruptGzip",
			body:       "\x1f\x8bthis is not a gzip stream",
			wantErrors: 1,
		},
		{
			name:       "BodyTooLarge",
			body:       tooLargeBody,
			wantErrors: 1,
		},
		{
			// The whole document is discarded, no page request is made.
			name:       "TooManyLocs",
			body:       manyLocs.String(),
			wantErrors: 1,
		},
		{
			name: "BadEntriesSkipped",
			body: `<urlset>` +
				`<url><loc></loc></url>` +
				`<url><loc>   </loc></url>` +
				`<url><loc>http://t.test/%zz</loc></url>` +
				`<url><loc>ftp://t.test/x</loc></url>` +
				`<url><loc>mailto:a@b.test</loc></url>` +
				`<url><loc>http://t.test/ok</loc></url>` +
				`</urlset>`,
			wantPages: []string{"/ok"},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			rec := &sitemapRecorder{
				responses: map[string]sitemapResponse{
					"t.test/robots.txt": {body: "User-agent: *\nAllow: /\nSitemap: http://t.test/sm/map.xml\n"},
					"t.test/start":      {body: `<html><body>start</body></html>`},
					"t.test/sm/map.xml": {body: tc.body, gzip: tc.gzip},
				},
			}
			// Serve every page the sitemap is expected to produce
			for _, p := range tc.wantPages {
				rec.responses["t.test"+p] = sitemapResponse{body: `<html></html>`}
			}
			// Additional served URLs (child sitemaps)
			skip := []string{"t.test/robots.txt", "t.test/start", "t.test/sm/map.xml"}
			for k, v := range tc.extra {
				rec.responses[k] = v
				skip = append(skip, k)
			}

			// The crawl always completes, even when the sitemap is faulty
			if err := rec.crawl(t, "http://t.test/start", nil); err != nil {
				t.Fatalf("expected crawl to complete with nil error, got %v", err)
			}

			if len(rec.errors) != tc.wantErrors {
				t.Fatalf("expected %d error(s), got %d: %v", tc.wantErrors, len(rec.errors), rec.errors)
			}
			for _, ce := range rec.errors {
				if ce.Kind != CekParseSitemap {
					t.Errorf("expected error kind %s, got %s (%v)", CekParseSitemap, ce.Kind, ce)
				}
			}

			// Collect the page requests, i.e. everything but the seed, the
			// robots.txt and the sitemap control requests
			control := make(map[string]bool, len(skip))
			for _, k := range skip {
				control[k] = true
			}
			var gotPages []string
			for _, f := range rec.fetches {
				if !control[f] {
					gotPages = append(gotPages, strings.TrimPrefix(f, "t.test"))
				}
			}
			want := sortedCopy(tc.wantPages)
			if got := sortedCopy(gotPages); !reflect.DeepEqual(got, want) {
				t.Errorf("page requests mismatch:\n got: %v\nwant: %v", got, want)
			}
		})
	}
}

// TestSitemapCompatibility verifies that with DiscoverSitemaps left at its
// default (false), the legacy behaviour is strictly unchanged: same requests
// in the same order, same hook calls, and the sitemap declared in robots.txt
// is ignored.
func TestSitemapCompatibility(t *testing.T) {
	if NewOptions(new(DefaultExtender)).DiscoverSitemaps {
		t.Fatal("DiscoverSitemaps must default to false")
	}

	rec := &sitemapRecorder{
		responses: map[string]sitemapResponse{
			"a.test/robots.txt": {body: "User-agent: *\nAllow: /\nSitemap: http://a.test/maps/index.xml\n"},
			"a.test/root":       {body: `<a href="/ordinary">normal</a>`},
			"a.test/ordinary":   {body: `<html><body>ordinary</body></html>`},
		},
	}

	opts := NewOptions(rec)
	opts.CrawlDelay = 0
	opts.LogFlags = 0
	opts.URLNormalizationFlags = 0
	// DiscoverSitemaps is deliberately left at its default value.
	if err := NewCrawlerWithOptions(opts).Run("http://a.test/root"); err != nil {
		t.Fatalf("expected crawl to complete with nil error, got %v", err)
	}

	// Same requests, in the same order as before the option existed: the
	// declared sitemap is never fetched.
	wantFetches := []string{"a.test/robots.txt", "a.test/root", "a.test/ordinary"}
	if !reflect.DeepEqual(rec.fetches, wantFetches) {
		t.Errorf("request sequence mismatch:\n got: %v\nwant: %v", rec.fetches, wantFetches)
	}

	wantVisits := []string{"a.test/root", "a.test/ordinary"}
	if !reflect.DeepEqual(rec.visits, wantVisits) {
		t.Errorf("visits mismatch:\n got: %v\nwant: %v", rec.visits, wantVisits)
	}

	// Hook counts: Filter for the 2 pages, Enqueued for the 2 pages and the
	// robots.txt, no error, no sitemap identity anywhere.
	if len(rec.filters) != 2 {
		t.Errorf("expected 2 Filter calls, got %d: %v", len(rec.filters), rec.filters)
	}
	if len(rec.enqueued) != 3 {
		t.Errorf("expected 3 Enqueued calls, got %d: %v", len(rec.enqueued), rec.enqueued)
	}
	if len(rec.errors) != 0 {
		t.Errorf("expected no error, got %v", rec.errors)
	}
	for _, e := range rec.enqueued {
		if strings.HasSuffix(e, sitemapMark) {
			t.Errorf("unexpected sitemap control request with the option off: %s", e)
		}
	}
}
