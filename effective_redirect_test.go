package colly

import (
	"bytes"
	"crypto/sha1"
	"encoding/gob"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"testing"
)

// These regression tests cover the effective (final) request exposed by
// Collector after redirects, for both the live network path and cache replay.
//
// The origin is a real httptest server whenever the environment allows a
// loopback listener. Where local port binding is unavailable (for example a
// restricted sandbox), the very same handler is served through an in-process
// http.RoundTripper instead: net/http's real client, redirect policy and
// method/body transformation still run, and every request is recorded with
// its exact method, URL, body, host and count. Collectors are always driven
// through their public entry points.
//
// All collectors set IgnoreRobotsTxt() so only page traffic reaches the
// origin.

// observedRequest is one request as seen by the origin.
type observedRequest struct {
	Method string
	Path   string
	Query  string
	Body   string
	Host   string
}

func (o observedRequest) String() string {
	return fmt.Sprintf("%s %s%s body=%q host=%s", o.Method, o.Path, o.Query, o.Body, o.Host)
}

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

// origin serves a route table and records every request reaching it.
type origin struct {
	base string

	mu       sync.Mutex
	requests []observedRequest

	handler http.Handler
	server  *httptest.Server // set when a real httptest server is in use
	rt      http.RoundTripper
}

// newOrigin starts a real httptest server for the given routes. If the process
// is not permitted to bind a loopback port, it falls back to serving the same
// handler in-process.
func newOrigin(t *testing.T, setup func(mux *http.ServeMux)) *origin {
	t.Helper()
	mux := http.NewServeMux()
	setup(mux)

	o := &origin{}
	o.handler = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body := ""
		if r.Body != nil && r.Body != http.NoBody {
			b, err := io.ReadAll(r.Body)
			if err != nil {
				http.Error(w, err.Error(), http.StatusBadRequest)
				return
			}
			body = string(b)
			// The handler downstream may parse the form; restore the body.
			r.Body = io.NopCloser(bytes.NewReader(b))
		}
		o.mu.Lock()
		o.requests = append(o.requests, observedRequest{
			Method: r.Method,
			Path:   r.URL.Path,
			Query:  r.URL.RawQuery,
			Body:   body,
			Host:   effectiveHost(r),
		})
		o.mu.Unlock()
		mux.ServeHTTP(w, r)
	})

	if listener, err := net.Listen("tcp", "127.0.0.1:0"); err == nil {
		srv := httptest.NewUnstartedServer(o.handler)
		srv.Listener = listener
		srv.Start()
		o.server = srv
		o.base = srv.URL
	} else {
		// Loopback binding is unavailable. Keep exercising net/http's real
		// redirect machinery by serving the same handler in-process.
		o.base = "http://origin.test"
	}
	t.Cleanup(o.Close)
	return o
}

func (o *origin) Close() {
	if o.server != nil {
		o.server.Close()
	}
}

func (o *origin) Requests() []observedRequest {
	o.mu.Lock()
	defer o.mu.Unlock()
	out := make([]observedRequest, len(o.requests))
	copy(out, o.requests)
	return out
}

func (o *origin) Count() int { return len(o.Requests()) }

func (o *origin) Reset() {
	o.mu.Lock()
	o.requests = nil
	o.mu.Unlock()
}

// Transport returns nil when a real server is used, so net/http's default
// transport reaches it. Otherwise it returns the in-process transport.
func (o *origin) Transport() http.RoundTripper {
	if o.server != nil {
		return nil
	}
	if o.rt == nil {
		o.rt = roundTripFunc(func(req *http.Request) (*http.Response, error) {
			rec := newResponseRecorder(req)
			o.handler.ServeHTTP(rec, req)
			return rec.result(), nil
		})
	}
	return o.rt
}

func (o *origin) collector(opts ...CollectorOption) *Collector {
	c := NewCollector(append(opts, IgnoreRobotsTxt())...)
	if tr := o.Transport(); tr != nil {
		c.WithTransport(tr)
	}
	return c
}

func (o *origin) URL(path string) string { return o.base + path }

// effectiveHost is the host the origin sees: net/http writes the Host header
// from Request.Host when set, and from the URL otherwise. Redirect hops
// created by net/http leave Request.Host empty.
func effectiveHost(req *http.Request) string {
	if req.Host != "" {
		return req.Host
	}
	return req.URL.Host
}

// responseRecorder captures a handler's response in memory for the in-process
// transport.
type responseRecorder struct {
	req     *http.Request
	header  http.Header
	code    int
	body    []byte
	written bool
}

func newResponseRecorder(req *http.Request) *responseRecorder {
	return &responseRecorder{req: req, header: http.Header{}}
}

func (r *responseRecorder) Header() http.Header { return r.header }

func (r *responseRecorder) WriteHeader(status int) {
	if r.written {
		return
	}
	r.code = status
	r.written = true
}

func (r *responseRecorder) Write(b []byte) (int, error) {
	if !r.written {
		r.WriteHeader(http.StatusOK)
	}
	r.body = append(r.body, b...)
	return len(b), nil
}

func (r *responseRecorder) result() *http.Response {
	code := r.code
	if code == 0 {
		code = http.StatusOK
	}
	var body io.ReadCloser = http.NoBody
	if len(r.body) > 0 {
		body = io.NopCloser(strings.NewReader(string(r.body)))
	}
	return &http.Response{
		StatusCode: code,
		Header:     r.header,
		Body:       body,
		Request:    r.req,
	}
}

func redirectRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/entry", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/catalog/page/", http.StatusFound)
	})
	mux.HandleFunc("/catalog/page/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("X-Origin", "catalog")
		fmt.Fprint(w, `<a href="item">item</a>`)
	})
	mux.HandleFunc("/catalog/page/item", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprint(w, `<a href="nested">nested</a>`)
	})
	mux.HandleFunc("/base-entry", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/based/dir/", http.StatusFound)
	})
	mux.HandleFunc("/based/dir/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprint(w, `<base href="../assets/"><a href="leaf">leaf</a>`)
	})
	mux.HandleFunc("/submit", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/receipt/", http.StatusSeeOther)
	})
	mux.HandleFunc("/receipt/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprint(w, `<p>receipt</p>`)
	})
	mux.HandleFunc("/temp", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/kept/", http.StatusTemporaryRedirect)
	})
	mux.HandleFunc("/permanent", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/kept/", http.StatusPermanentRedirect)
	})
	mux.HandleFunc("/kept/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprint(w, `<p>kept</p>`)
	})
	mux.HandleFunc("/head", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/head-final/", http.StatusSeeOther)
	})
	mux.HandleFunc("/head-final/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
	})
	for _, code := range []int{http.StatusMovedPermanently, http.StatusFound} {
		code := code
		mux.HandleFunc(fmt.Sprintf("/convert-%d", code), func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, "/kept/", code)
		})
	}
	mux.HandleFunc("/cookie-entry", func(w http.ResponseWriter, r *http.Request) {
		http.SetCookie(w, &http.Cookie{Name: "sid", Value: "abc", Path: "/"})
		http.Redirect(w, r, "/cookie-final/", http.StatusFound)
	})
	mux.HandleFunc("/cookie-final/", func(w http.ResponseWriter, r *http.Request) {
		if cookie, err := r.Cookie("sid"); err != nil || cookie.Value != "abc" {
			http.Error(w, "missing sid cookie", http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
	})
	mux.HandleFunc("/abort-target", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("X-Abortable", "yes")
		fmt.Fprint(w, `<a href="child">child</a>`)
	})
}

func bodyString(r *Request) string {
	if r.Body == nil {
		return ""
	}
	b, _ := io.ReadAll(r.Body)
	return string(b)
}

type unseekableReader struct{ data string }

func (u *unseekableReader) Read(p []byte) (int, error) {
	if u.data == "" {
		return 0, io.EOF
	}
	n := copy(p, u.data)
	u.data = u.data[n:]
	return n, nil
}

func TestEffectiveRedirectLive(t *testing.T) {
	o := newOrigin(t, redirectRoutes)

	t.Run("OnRequest is initial, later callbacks use the final request", func(t *testing.T) {
		o.Reset()
		c := o.collector()
		var onRequestURL, onRequestMethod string
		var headerURL, headerMethod, headerHost, headerOrigin string
		var headerCtx *Context
		var headerID uint32
		var headerDepth int
		var responseURL, responseMethod, scrapedURL string
		var htmlURL, htmlLink string
		var htmlDepth int
		c.OnRequest(func(r *Request) {
			onRequestURL, onRequestMethod = r.URL.Path, r.Method
		})
		c.OnResponseHeaders(func(r *Response) {
			headerURL, headerMethod, headerHost = r.Request.URL.String(), r.Request.Method, r.Request.Host
			headerOrigin = r.Headers.Get("X-Origin")
			headerCtx, headerID, headerDepth = r.Ctx, r.Request.ID, r.Request.Depth
		})
		c.OnResponse(func(r *Response) {
			responseURL, responseMethod = r.Request.URL.String(), r.Request.Method
		})
		c.OnHTML("a", func(e *HTMLElement) {
			htmlURL = e.Request.URL.String()
			htmlDepth = e.Request.Depth
			htmlLink = e.Request.AbsoluteURL(e.Attr("href"))
		})
		c.OnScraped(func(r *Response) { scrapedURL = r.Request.URL.String() })
		if err := c.Visit(o.URL("/entry")); err != nil {
			t.Fatalf("Visit: %v", err)
		}

		if onRequestURL != "/entry" || onRequestMethod != "GET" {
			t.Fatalf("OnRequest = %s %s, want GET /entry", onRequestMethod, onRequestURL)
		}
		if want := o.URL("/catalog/page/"); headerURL != want || headerMethod != "GET" {
			t.Fatalf("OnResponseHeaders = %s %s, want GET %s", headerMethod, headerURL, want)
		}
		if want := hostOf(t, o.URL("/catalog/page/")); headerHost != want {
			t.Fatalf("OnResponseHeaders Host = %q, want %q", headerHost, want)
		}
		if headerOrigin != "catalog" {
			t.Fatalf("OnResponseHeaders X-Origin = %q, want catalog", headerOrigin)
		}
		if headerCtx == nil || headerID == 0 || headerDepth != 1 {
			t.Fatalf("OnResponseHeaders Ctx/ID/Depth = %v/%d/%d, want non-nil/1", headerCtx, headerID, headerDepth)
		}
		if want := o.URL("/catalog/page/"); responseURL != want || responseMethod != "GET" {
			t.Fatalf("OnResponse = %s %s, want GET %s", responseMethod, responseURL, want)
		}
		if want := o.URL("/catalog/page/"); htmlURL != want || htmlDepth != 1 {
			t.Fatalf("OnHTML URL/Depth = %q/%d, want %q/1", htmlURL, htmlDepth, want)
		}
		if want := o.URL("/catalog/page/item"); htmlLink != want {
			t.Fatalf("relative link = %q, want %q", htmlLink, want)
		}
		if want := o.URL("/catalog/page/"); scrapedURL != want {
			t.Fatalf("OnScraped URL = %q, want %q", scrapedURL, want)
		}
		want := []observedRequest{
			{Method: "GET", Path: "/entry", Host: hostOf(t, o.URL("/entry"))},
			{Method: "GET", Path: "/catalog/page/", Host: hostOf(t, o.URL("/catalog/page/"))},
		}
		if got := o.Requests(); !reflect.DeepEqual(got, want) {
			t.Fatalf("origin requests = %#v, want %#v", got, want)
		}
	})

	t.Run("a relative link visited from OnHTML uses the final URL", func(t *testing.T) {
		o.Reset()
		c := o.collector()
		var childURL string
		c.OnHTML("a", func(e *HTMLElement) {
			if e.Attr("href") == "item" {
				if err := e.Request.Visit(e.Attr("href")); err != nil {
					t.Fatalf("Visit child: %v", err)
				}
			}
		})
		c.OnResponse(func(r *Response) {
			if r.Request.URL.Path == "/catalog/page/item" {
				childURL = r.Request.URL.String()
			}
		})
		if err := c.Visit(o.URL("/entry")); err != nil {
			t.Fatalf("Visit: %v", err)
		}
		if want := o.URL("/catalog/page/item"); childURL != want {
			t.Fatalf("child request URL = %q, want %q", childURL, want)
		}
	})

	t.Run("POST 303 becomes body-less GET and Retry repeats it", func(t *testing.T) {
		o.Reset()
		c := o.collector()
		var seen []string
		var retries int
		c.OnResponse(func(r *Response) {
			seen = append(seen, fmt.Sprintf("%s %s body=%q", r.Request.Method, r.Request.URL.Path, bodyString(r.Request)))
			if r.Request.Method != "GET" {
				t.Fatalf("OnResponse Method = %q, want GET after 303", r.Request.Method)
			}
			if r.Request.URL.Path != "/receipt/" {
				t.Fatalf("OnResponse URL = %q, want /receipt/", r.Request.URL.Path)
			}
			if retries == 0 {
				retries++
				if err := r.Request.Retry(); err != nil {
					t.Fatalf("Retry: %v", err)
				}
			}
		})
		if err := c.PostRaw(o.URL("/submit"), []byte("q=red")); err != nil {
			t.Fatalf("PostRaw: %v", err)
		}
		want := []observedRequest{
			{Method: "POST", Path: "/submit", Body: "q=red", Host: hostOf(t, o.URL("/submit"))},
			{Method: "GET", Path: "/receipt/", Host: hostOf(t, o.URL("/receipt/"))},
			{Method: "GET", Path: "/receipt/", Host: hostOf(t, o.URL("/receipt/"))},
		}
		if got := o.Requests(); !reflect.DeepEqual(got, want) {
			t.Fatalf("origin requests = %#v, want %#v", got, want)
		}
		if len(seen) != 2 {
			t.Fatalf("OnResponse ran %d times, want 2 (original + one retry)", len(seen))
		}
		for i, v := range seen {
			if v != `GET /receipt/ body=""` {
				t.Fatalf("OnResponse[%d] request = %s, want a body-less GET", i, v)
			}
		}
	})

	t.Run("POST through 301 and 302 becomes body-less GET", func(t *testing.T) {
		for _, code := range []int{http.StatusMovedPermanently, http.StatusFound} {
			o.Reset()
			c := o.collector()
			var method, body, path string
			c.OnResponse(func(r *Response) {
				method, body, path = r.Request.Method, bodyString(r.Request), r.Request.URL.Path
			})
			if err := c.PostRaw(fmt.Sprintf("%s/convert-%d", o.base, code), []byte("x=1")); err != nil {
				t.Fatalf("PostRaw %d: %v", code, err)
			}
			if method != "GET" || body != "" || path != "/kept/" {
				t.Fatalf("status %d final request = %s %s body=%q, want GET /kept/ empty", code, method, path, body)
			}
		}
	})

	t.Run("307 keeps method and body and two consecutive Retries replay it", func(t *testing.T) {
		o.Reset()
		c := o.collector()
		var retries, responses int
		c.OnResponse(func(r *Response) {
			responses++
			if r.Request.Method != "POST" {
				t.Fatalf("OnResponse Method = %q, want POST after 307", r.Request.Method)
			}
			if r.Request.URL.Path != "/kept/" {
				t.Fatalf("OnResponse URL = %q, want /kept/", r.Request.URL.Path)
			}
			if retries < 2 {
				retries++
				if err := r.Request.Retry(); err != nil {
					t.Fatalf("Retry %d: %v", retries, err)
				}
			}
		})
		payload := "q=blue&zero=0&padding=" + strings.Repeat("z", 64)
		if err := c.PostRaw(o.URL("/temp"), []byte(payload)); err != nil {
			t.Fatalf("PostRaw: %v", err)
		}
		want := []observedRequest{
			{Method: "POST", Path: "/temp", Body: payload, Host: hostOf(t, o.URL("/temp"))},
			{Method: "POST", Path: "/kept/", Body: payload, Host: hostOf(t, o.URL("/kept/"))},
			{Method: "POST", Path: "/kept/", Body: payload, Host: hostOf(t, o.URL("/kept/"))},
			{Method: "POST", Path: "/kept/", Body: payload, Host: hostOf(t, o.URL("/kept/"))},
		}
		if got := o.Requests(); !reflect.DeepEqual(got, want) {
			t.Fatalf("origin requests = %#v, want %#v", got, want)
		}
		if responses != 3 {
			t.Fatalf("OnResponse ran %d times, want 3", responses)
		}
	})

	t.Run("308 replays an explicitly zero byte body", func(t *testing.T) {
		o.Reset()
		c := o.collector()
		var retries int
		c.OnResponse(func(r *Response) {
			if retries == 0 {
				retries++
				if err := r.Request.Retry(); err != nil {
					t.Fatalf("Retry: %v", err)
				}
			}
		})
		if err := c.PostRaw(o.URL("/permanent"), []byte{}); err != nil {
			t.Fatalf("PostRaw: %v", err)
		}
		want := []observedRequest{
			{Method: "POST", Path: "/permanent", Host: hostOf(t, o.URL("/permanent"))},
			{Method: "POST", Path: "/kept/", Host: hostOf(t, o.URL("/kept/"))},
			{Method: "POST", Path: "/kept/", Host: hostOf(t, o.URL("/kept/"))},
		}
		if got := o.Requests(); !reflect.DeepEqual(got, want) {
			t.Fatalf("origin requests = %#v, want %#v", got, want)
		}
	})

	t.Run("HEAD stays HEAD through 303", func(t *testing.T) {
		o.Reset()
		c := o.collector()
		var method, path string
		c.OnResponse(func(r *Response) {
			method, path = r.Request.Method, r.Request.URL.Path
		})
		if err := c.Head(o.URL("/head")); err != nil {
			t.Fatalf("Head: %v", err)
		}
		if method != "HEAD" || path != "/head-final/" {
			t.Fatalf("final request = %s %s, want HEAD /head-final/", method, path)
		}
		want := []observedRequest{
			{Method: "HEAD", Path: "/head", Host: hostOf(t, o.URL("/head"))},
			{Method: "HEAD", Path: "/head-final/", Host: hostOf(t, o.URL("/head-final/"))},
		}
		if got := o.Requests(); !reflect.DeepEqual(got, want) {
			t.Fatalf("origin requests = %#v, want %#v", got, want)
		}
	})

	t.Run("no redirect keeps the request issued by this scrape", func(t *testing.T) {
		o.Reset()
		c := o.collector()
		var onRequest *Request
		c.OnRequest(func(r *Request) { onRequest = r })
		var same, depthOK, idOK bool
		c.OnResponse(func(r *Response) {
			same = r.Request == onRequest
			depthOK = r.Request.Depth == 1
			idOK = r.Request.ID != 0
		})
		if err := c.Visit(o.URL("/catalog/page/")); err != nil {
			t.Fatalf("Visit: %v", err)
		}
		if !same || !depthOK || !idOK {
			t.Fatalf("no-redirect identity/depth/id = %v/%v/%v", same, depthOK, idOK)
		}
		if got := o.Requests(); len(got) != 1 || got[0].Path != "/catalog/page/" {
			t.Fatalf("origin requests = %#v", got)
		}
	})

	t.Run("unseekable body still returns ErrRetryBodyUnseekable", func(t *testing.T) {
		o.Reset()
		c := o.collector()
		var retryErr error
		c.OnResponse(func(r *Response) { retryErr = r.Request.Retry() })
		if err := c.Request("POST", o.URL("/kept/"), &unseekableReader{data: "q=unseekable"}, nil, nil); err != nil {
			t.Fatalf("Request: %v", err)
		}
		if !errors.Is(retryErr, ErrRetryBodyUnseekable) {
			t.Fatalf("Retry error = %v, want ErrRetryBodyUnseekable", retryErr)
		}
		if got := o.Requests(); len(got) != 1 || got[0].Body != "q=unseekable" {
			t.Fatalf("origin requests = %#v", got)
		}
	})

	t.Run("cookies are still updated across redirects", func(t *testing.T) {
		o.Reset()
		c := o.collector()
		if err := c.Visit(o.URL("/cookie-entry")); err != nil {
			t.Fatalf("Visit: %v", err)
		}
		if cookies := c.Cookies(o.URL("/cookie-final/")); len(cookies) != 1 || cookies[0].Value != "abc" {
			t.Fatalf("cookies = %#v, want sid=abc", cookies)
		}
	})
}

func mustParseURL(t *testing.T, raw string) *url.URL {
	t.Helper()
	u, err := url.Parse(raw)
	if err != nil {
		t.Fatalf("parse %q: %v", raw, err)
	}
	return u
}

func hostOf(t *testing.T, raw string) string { return mustParseURL(t, raw).Host }

// cacheEntryPath returns the on-disk path of a cache entry for a requested URL.
func cacheEntryPath(cacheDir, requestedURL string) string {
	sum := sha1.Sum([]byte(requestedURL))
	hash := hex.EncodeToString(sum[:])
	return filepath.Join(cacheDir, hash[:2], hash)
}

// legacyCacheEntry matches the field names and types of the historical
// *Response gob encoding: exactly StatusCode, Body and Headers.
type legacyCacheEntry struct {
	StatusCode int
	Body       []byte
	Headers    *http.Header
}

func writeLegacyCacheEntry(t *testing.T, cacheDir, requestedURL string, statusCode int, body string, header http.Header) {
	t.Helper()
	filename := cacheEntryPath(cacheDir, requestedURL)
	if err := os.MkdirAll(filepath.Dir(filename), 0o750); err != nil {
		t.Fatalf("mkdir cache shard: %v", err)
	}
	f, err := os.Create(filename)
	if err != nil {
		t.Fatalf("create legacy cache entry: %v", err)
	}
	defer f.Close()
	if err := gob.NewEncoder(f).Encode(&legacyCacheEntry{StatusCode: statusCode, Body: []byte(body), Headers: &header}); err != nil {
		t.Fatalf("encode legacy entry: %v", err)
	}
}

func TestEffectiveRedirectCache(t *testing.T) {
	o := newOrigin(t, redirectRoutes)
	cacheDir := t.TempDir()

	// Warm the cache on the network path.
	first := o.collector(CacheDir(cacheDir))
	var firstURL, firstLink string
	first.OnResponse(func(r *Response) { firstURL = r.Request.URL.String() })
	first.OnHTML("a", func(e *HTMLElement) { firstLink = e.Request.AbsoluteURL(e.Attr("href")) })
	if err := first.Visit(o.URL("/entry")); err != nil {
		t.Fatalf("warm Visit: %v", err)
	}
	if want := o.URL("/catalog/page/"); firstURL != want {
		t.Fatalf("network URL = %q, want %q", firstURL, want)
	}
	if want := o.URL("/catalog/page/item"); firstLink != want {
		t.Fatalf("network relative link = %q, want %q", firstLink, want)
	}
	o.Reset()

	// A different Collector reading the same CacheDir must not touch the
	// network, must expose the same effective URL and relative resolution, and
	// must only see headers belonging to this call.
	second := o.collector(CacheDir(cacheDir), Headers(map[string]string{
		"Accept":     "text/html",
		"User-Agent": "second-reader",
	}))
	var cachedURL, cachedMethod, cachedHost, cachedLink string
	var cachedHeaders http.Header
	var callbackTrace []string
	second.OnRequest(func(r *Request) { callbackTrace = append(callbackTrace, "request") })
	second.OnResponseHeaders(func(r *Response) { callbackTrace = append(callbackTrace, "responseHeaders") })
	second.OnResponse(func(r *Response) {
		callbackTrace = append(callbackTrace, "response")
		cachedURL = r.Request.URL.String()
		cachedMethod = r.Request.Method
		cachedHost = r.Request.Host
		cachedHeaders = r.Request.Headers.Clone()
	})
	second.OnHTML("a", func(e *HTMLElement) {
		callbackTrace = append(callbackTrace, "html")
		cachedLink = e.Request.AbsoluteURL(e.Attr("href"))
		if e.Request.URL.String() != o.URL("/catalog/page/") {
			t.Fatalf("cached OnHTML URL = %q", e.Request.URL.String())
		}
	})
	second.OnScraped(func(r *Response) { callbackTrace = append(callbackTrace, "scraped") })
	if err := second.Visit(o.URL("/entry")); err != nil {
		t.Fatalf("cached Visit: %v", err)
	}
	if got := o.Count(); got != 0 {
		t.Fatalf("cache hit made %d network requests: %#v", got, o.Requests())
	}
	if want := o.URL("/catalog/page/"); cachedURL != want {
		t.Fatalf("cached URL = %q, want %q", cachedURL, want)
	}
	if cachedMethod != "GET" {
		t.Fatalf("cached Method = %q, want GET", cachedMethod)
	}
	if host := hostOf(t, o.URL("/")); cachedHost != host {
		t.Fatalf("cached Host = %q, want %q", cachedHost, host)
	}
	if want := o.URL("/catalog/page/item"); cachedLink != want {
		t.Fatalf("cached relative link = %q, want %q", cachedLink, want)
	}
	if got, want := strings.Join(callbackTrace, ","), "request,responseHeaders,response,html,scraped"; got != want {
		t.Fatalf("callback trace = %q, want %q", got, want)
	}
	if got, want := cachedHeaders.Get("User-Agent"), "second-reader"; got != want {
		t.Fatalf("cached User-Agent = %q, want this call's %q", got, want)
	}
	if got, want := cachedHeaders.Get("Accept"), "text/html"; got != want {
		t.Fatalf("cached Accept = %q, want this call's %q", got, want)
	}
	if cachedHeaders.Get("Cookie") != "" {
		t.Fatalf("cached request inherited Cookie = %q", cachedHeaders.Get("Cookie"))
	}

	// A relative <base href> resolves against the final document URL, for both
	// the network and the cached path.
	networkBase := o.collector(CacheDir(cacheDir))
	var baseLink string
	networkBase.OnHTML("a", func(e *HTMLElement) { baseLink = e.Request.AbsoluteURL(e.Attr("href")) })
	if err := networkBase.Visit(o.URL("/base-entry")); err != nil {
		t.Fatalf("warm base Visit: %v", err)
	}
	if want := o.URL("/based/assets/leaf"); baseLink != want {
		t.Fatalf("network base link = %q, want %q", baseLink, want)
	}
	o.Reset()
	cachedBase := o.collector(CacheDir(cacheDir))
	var baseCachedURL, baseCachedLink string
	cachedBase.OnResponse(func(r *Response) { baseCachedURL = r.Request.URL.String() })
	cachedBase.OnHTML("a", func(e *HTMLElement) { baseCachedLink = e.Request.AbsoluteURL(e.Attr("href")) })
	if err := cachedBase.Visit(o.URL("/base-entry")); err != nil {
		t.Fatalf("cached base Visit: %v", err)
	}
	if got := o.Count(); got != 0 {
		t.Fatalf("base cache hit made %d network requests", got)
	}
	if want := o.URL("/based/dir/"); baseCachedURL != want {
		t.Fatalf("cached base document URL = %q, want %q", baseCachedURL, want)
	}
	if want := o.URL("/based/assets/leaf"); baseCachedLink != want {
		t.Fatalf("cached relative base link = %q, want %q", baseCachedLink, want)
	}

	// A legacy gob entry has no final URL. It must still be readable, and the
	// requested URL is the only fallback — no redirect target is guessed and
	// the network is never used.
	legacyDir := t.TempDir()
	legacyKey := o.URL("/legacy-entry")
	writeLegacyCacheEntry(t, legacyDir, legacyKey, 200, `<a href="item">item</a>`, http.Header{
		"Content-Type": []string{"text/html; charset=utf-8"},
	})
	o.Reset()
	legacy := o.collector(CacheDir(legacyDir))
	var legacyURL, legacyLink string
	legacy.OnResponse(func(r *Response) { legacyURL = r.Request.URL.String() })
	legacy.OnHTML("a", func(e *HTMLElement) { legacyLink = e.Request.AbsoluteURL(e.Attr("href")) })
	if err := legacy.Visit(legacyKey); err != nil {
		t.Fatalf("legacy Visit: %v", err)
	}
	if got := o.Count(); got != 0 {
		t.Fatalf("legacy cache hit forced %d network requests", got)
	}
	if legacyURL != legacyKey {
		t.Fatalf("legacy cached URL = %q, want fallback %q", legacyURL, legacyKey)
	}
	if want := o.URL("/item"); legacyLink != want {
		t.Fatalf("legacy relative link = %q, want %q", legacyLink, want)
	}

	// The entry written for /entry must exist under the requested URL's key,
	// and it must only carry the document replay data: no request headers,
	// cookies, request body or *http.Request may be stored.
	entry := readCacheEntry(t, cacheDir, o.URL("/entry"))
	if entry.FinalURL != o.URL("/catalog/page/") {
		t.Fatalf("stored FinalURL = %q, want %q", entry.FinalURL, o.URL("/catalog/page/"))
	}
	encoded, err := os.ReadFile(cacheEntryPath(cacheDir, o.URL("/entry")))
	if err != nil {
		t.Fatalf("read cache entry: %v", err)
	}
	if strings.Contains(string(encoded), "second-reader") || strings.Contains(string(encoded), "User-Agent") {
		t.Fatalf("cache entry persisted request headers: %q", encoded)
	}

	// A legacy 500 entry is not served as a hit: it is refreshed over the
	// network, and the refreshed entry still records the final URL.
	refreshDir := t.TempDir()
	refreshURL := o.URL("/catalog/page/")
	writeLegacyCacheEntry(t, refreshDir, refreshURL, 500, "legacy failure", http.Header{
		"Content-Type": []string{"text/html; charset=utf-8"},
	})
	o.Reset()
	refresher := o.collector(CacheDir(refreshDir))
	var refreshedStatus int
	refresher.OnResponse(func(r *Response) { refreshedStatus = r.StatusCode })
	if err := refresher.Visit(refreshURL); err != nil {
		t.Fatalf("refresh Visit: %v", err)
	}
	if got := o.Count(); got != 1 {
		t.Fatalf("legacy 500 entry should have been refreshed, got %d network requests", got)
	}
	if refreshedStatus != 200 {
		t.Fatalf("refreshed status = %d, want 200", refreshedStatus)
	}
	if got := readCacheEntry(t, refreshDir, refreshURL).FinalURL; got != refreshURL {
		t.Fatalf("refreshed FinalURL = %q, want %q", got, refreshURL)
	}
}

func readCacheEntry(t *testing.T, cacheDir, requestedURL string) cacheEntry {
	t.Helper()
	f, err := os.Open(cacheEntryPath(cacheDir, requestedURL))
	if err != nil {
		t.Fatalf("open cache entry: %v", err)
	}
	defer f.Close()
	var entry cacheEntry
	if err := gob.NewDecoder(f).Decode(&entry); err != nil {
		t.Fatalf("decode cache entry: %v", err)
	}
	return entry
}

func TestEffectiveRedirectAbort(t *testing.T) {
	o := newOrigin(t, redirectRoutes)
	cacheDir := t.TempDir()

	run := func(t *testing.T, c *Collector) {
		t.Helper()
		var events []string
		var errorErr error
		c.OnResponseHeaders(func(r *Response) {
			events = append(events, "responseHeaders")
			if r.Headers.Get("X-Abortable") == "yes" {
				r.Request.Abort()
			}
		})
		c.OnResponse(func(r *Response) { events = append(events, "response") })
		c.OnHTML("a", func(e *HTMLElement) { events = append(events, "html") })
		c.OnScraped(func(r *Response) { events = append(events, "scraped") })
		c.OnError(func(r *Response, err error) {
			events = append(events, "error")
			errorErr = err
			if r == nil {
				t.Fatalf("OnError received a nil Response")
			}
		})
		if err := c.Visit(o.URL("/abort-target")); err != nil && !errors.Is(err, ErrAbortedAfterHeaders) {
			t.Fatalf("Visit: %v", err)
		}
		if !errors.Is(errorErr, ErrAbortedAfterHeaders) {
			t.Fatalf("OnError err = %v, want ErrAbortedAfterHeaders", errorErr)
		}
		if len(events) != 2 || events[0] != "responseHeaders" || events[1] != "error" {
			t.Fatalf("callback trace = %v, want [responseHeaders error]", events)
		}
	}

	t.Run("live", func(t *testing.T) {
		run(t, o.collector())
	})

	t.Run("cached", func(t *testing.T) {
		// Warm the cache with a normal read, then abort a cached replay. The
		// entry must remain available for the next normal read.
		warm := o.collector(CacheDir(cacheDir))
		if err := warm.Visit(o.URL("/abort-target")); err != nil {
			t.Fatalf("warm Visit: %v", err)
		}
		o.Reset()

		run(t, o.collector(CacheDir(cacheDir)))
		if got := o.Count(); got != 0 {
			t.Fatalf("aborting cached read used the network %d times", got)
		}

		after := o.collector(CacheDir(cacheDir))
		var trace []string
		after.OnResponseHeaders(func(r *Response) { trace = append(trace, "responseHeaders") })
		after.OnResponse(func(r *Response) { trace = append(trace, "response") })
		after.OnHTML("a", func(e *HTMLElement) { trace = append(trace, "html") })
		after.OnScraped(func(r *Response) { trace = append(trace, "scraped") })
		if err := after.Visit(o.URL("/abort-target")); err != nil {
			t.Fatalf("second Visit: %v", err)
		}
		if got := o.Count(); got != 0 {
			t.Fatalf("cache entry was not reusable: %d network requests", got)
		}
		if got, want := strings.Join(trace, ","), "responseHeaders,response,html,scraped"; got != want {
			t.Fatalf("after abort trace = %q, want %q", got, want)
		}
	})
}

func TestEffectiveRedirectIsolation(t *testing.T) {
	o := newOrigin(t, redirectRoutes)
	cacheDir := t.TempDir()

	// Warm the cache once on the network path.
	warm := o.collector(CacheDir(cacheDir))
	var warmLink string
	warm.OnHTML("a", func(e *HTMLElement) { warmLink = e.Request.AbsoluteURL(e.Attr("href")) })
	if err := warm.Visit(o.URL("/entry")); err != nil {
		t.Fatalf("warm Visit: %v", err)
	}
	if want := o.URL("/catalog/page/item"); warmLink != want {
		t.Fatalf("warm link = %q, want %q", warmLink, want)
	}
	o.Reset()

	// Controlled concurrency: every reader uses its own Collector and cache
	// entry lookup, and must observe its own Ctx, Depth, ID, headers and
	// callback order without touching the network or another reader's state.
	const readers = 6
	type result struct {
		url      string
		link     string
		ctx      *Context
		ctxValue string
		depth    int
		onReqID  uint32
		respID   uint32
		header   string
		trace    string
		err      error
	}
	results := make([]result, readers)
	var wg sync.WaitGroup
	wg.Add(readers)
	for i := 0; i < readers; i++ {
		i := i
		go func() {
			defer wg.Done()
			c := o.collector(CacheDir(cacheDir), Headers(map[string]string{
				"X-Reader": fmt.Sprintf("reader-%d", i),
			}))
			var mu sync.Mutex
			var trace []string
			c.OnRequest(func(r *Request) {
				mu.Lock()
				trace = append(trace, "request")
				mu.Unlock()
				r.Ctx.Put("reader", fmt.Sprintf("ctx-%d", i))
				results[i].onReqID = r.ID
			})
			c.OnResponseHeaders(func(r *Response) {
				mu.Lock()
				trace = append(trace, "responseHeaders")
				mu.Unlock()
			})
			c.OnResponse(func(r *Response) {
				mu.Lock()
				trace = append(trace, "response")
				mu.Unlock()
				results[i].url = r.Request.URL.String()
				results[i].ctx = r.Ctx
				results[i].ctxValue = r.Ctx.Get("reader")
				results[i].depth = r.Request.Depth
				results[i].respID = r.Request.ID
				results[i].header = r.Request.Headers.Get("X-Reader")
			})
			c.OnHTML("a", func(e *HTMLElement) {
				mu.Lock()
				trace = append(trace, "html")
				mu.Unlock()
				results[i].link = e.Request.AbsoluteURL(e.Attr("href"))
			})
			c.OnScraped(func(r *Response) {
				mu.Lock()
				trace = append(trace, "scraped")
				mu.Unlock()
			})
			results[i].err = c.Visit(o.URL("/entry"))
			results[i].trace = strings.Join(trace, ",")
		}()
	}
	wg.Wait()
	if got := o.Count(); got != 0 {
		t.Fatalf("concurrent cache readers used the network %d times", got)
	}
	for i := range results {
		r := results[i]
		if r.err != nil {
			t.Fatalf("reader %d: %v", i, r.err)
		}
		if want := o.URL("/catalog/page/"); r.url != want {
			t.Fatalf("reader %d URL = %q, want %q", i, r.url, want)
		}
		if want := o.URL("/catalog/page/item"); r.link != want {
			t.Fatalf("reader %d link = %q, want %q", i, r.link, want)
		}
		if r.ctxValue != fmt.Sprintf("ctx-%d", i) || r.ctx == nil {
			t.Fatalf("reader %d Ctx = %q (nil=%v), want ctx-%d", i, r.ctxValue, r.ctx == nil, i)
		}
		if r.ctx.Get("reader") != fmt.Sprintf("ctx-%d", i) {
			t.Fatalf("reader %d Ctx was shared with another reader", i)
		}
		if r.depth != 1 {
			t.Fatalf("reader %d Depth = %d, want 1", i, r.depth)
		}
		if r.respID == 0 || (r.onReqID != 0 && r.respID != r.onReqID) {
			t.Fatalf("reader %d request IDs: OnRequest=%d response=%d", i, r.onReqID, r.respID)
		}
		if want := fmt.Sprintf("reader-%d", i); r.header != want {
			t.Fatalf("reader %d header = %q, want %q", i, r.header, want)
		}
		if r.trace != "request,responseHeaders,response,html,scraped" {
			t.Fatalf("reader %d trace = %q", i, r.trace)
		}
	}

	// Async mode: the same guarantees hold with overlapping goroutines.
	for i := 0; i < 4; i++ {
		i := i
		c := o.collector(CacheDir(cacheDir), Async(), AllowURLRevisit(), Headers(map[string]string{
			"X-Reader": fmt.Sprintf("async-%d", i),
		}))
		var mu sync.Mutex
		var trace []string
		var gotURL, gotCtxValue, gotHeader string
		var gotDepth int
		var gotID uint32
		c.OnRequest(func(r *Request) {
			mu.Lock()
			trace = append(trace, "request")
			mu.Unlock()
			r.Ctx.Put("reader", fmt.Sprintf("async-ctx-%d", i))
		})
		c.OnResponseHeaders(func(r *Response) {
			mu.Lock()
			trace = append(trace, "responseHeaders")
			mu.Unlock()
		})
		c.OnResponse(func(r *Response) {
			mu.Lock()
			trace = append(trace, "response")
			mu.Unlock()
			gotURL = r.Request.URL.String()
			gotCtxValue = r.Ctx.Get("reader")
			gotHeader = r.Request.Headers.Get("X-Reader")
			gotDepth = r.Request.Depth
			gotID = r.Request.ID
		})
		c.OnHTML("a", func(e *HTMLElement) {
			mu.Lock()
			trace = append(trace, "html")
			mu.Unlock()
		})
		c.OnScraped(func(r *Response) {
			mu.Lock()
			trace = append(trace, "scraped")
			mu.Unlock()
		})
		if err := c.Visit(o.URL("/entry")); err != nil {
			t.Fatalf("async %d Visit: %v", i, err)
		}
		c.Wait()
		if want := o.URL("/catalog/page/"); gotURL != want {
			t.Fatalf("async %d URL = %q, want %q", i, gotURL, want)
		}
		if gotCtxValue != fmt.Sprintf("async-ctx-%d", i) {
			t.Fatalf("async %d Ctx = %q", i, gotCtxValue)
		}
		if want := fmt.Sprintf("async-%d", i); gotHeader != want {
			t.Fatalf("async %d header = %q, want %q", i, gotHeader, want)
		}
		if gotDepth != 1 || gotID == 0 {
			t.Fatalf("async %d Depth/ID = %d/%d", i, gotDepth, gotID)
		}
		if got := strings.Join(trace, ","); got != "request,responseHeaders,response,html,scraped" {
			t.Fatalf("async %d trace = %q", i, got)
		}
	}
	if got := o.Count(); got != 0 {
		t.Fatalf("async cache readers used the network %d times", got)
	}

	// Clone: an exact copy of a Collector has no inherited callbacks and
	// keeps its own per-request state, while sharing CacheDir.
	source := o.collector(CacheDir(cacheDir))
	var sourceCalls int
	source.OnResponse(func(r *Response) { sourceCalls++ })
	clone := source.Clone()
	if len(clone.responseCallbacks) != 0 {
		t.Fatalf("Clone inherited %d response callbacks", len(clone.responseCallbacks))
	}
	clone.Headers = &http.Header{}
	clone.Headers.Set("X-Reader", "clone")
	var cloneURL, cloneCtxValue string
	var cloneTrace []string
	clone.OnResponseHeaders(func(r *Response) { cloneTrace = append(cloneTrace, "responseHeaders") })
	clone.OnResponse(func(r *Response) {
		cloneTrace = append(cloneTrace, "response")
		cloneURL = r.Request.URL.String()
		cloneCtxValue = r.Ctx.Get("reader")
	})
	clone.OnHTML("a", func(e *HTMLElement) { cloneTrace = append(cloneTrace, "html") })
	clone.OnScraped(func(r *Response) { cloneTrace = append(cloneTrace, "scraped") })
	if err := clone.Visit(o.URL("/entry")); err != nil {
		t.Fatalf("clone Visit: %v", err)
	}
	if got := o.Count(); got != 0 {
		t.Fatalf("clone used the network %d times", got)
	}
	if want := o.URL("/catalog/page/"); cloneURL != want {
		t.Fatalf("clone URL = %q, want %q", cloneURL, want)
	}
	if cloneCtxValue != "" {
		t.Fatalf("clone inherited stale Ctx value %q", cloneCtxValue)
	}
	if sourceCalls != 0 {
		t.Fatalf("clone callbacks leaked into the source (%d calls)", sourceCalls)
	}
	if got, want := strings.Join(cloneTrace, ","), "responseHeaders,response,html,scraped"; got != want {
		t.Fatalf("clone trace = %q, want %q", got, want)
	}
	if clone.CacheDir != cacheDir {
		t.Fatalf("clone CacheDir = %q, want %q", clone.CacheDir, cacheDir)
	}
}
