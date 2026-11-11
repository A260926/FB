package client

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"iter"
	"mime"
	"net/http"
	"net/url"
	"strings"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

// Request describes an HTTP request for the STAC client.
//
// It is a small convenience type for the common JSON-in / JSON-out STAC
// patterns. For arbitrary request shapes use Client.DoHTTP.
type Request struct {
	Method  string            // HTTP method (defaults to GET if empty)
	URL     string            // Relative or absolute URL
	Body    any               // Will be JSON-marshaled if non-nil
	Headers map[string]string // Additional headers
}

// Get creates a GET request for the given path.
func Get(path string) *Request { return &Request{URL: path} }

// Post creates a POST request with a JSON body.
func Post(path string, body any) *Request {
	return &Request{Method: http.MethodPost, URL: path, Body: body}
}

// PageResult contains decoded page data.
type PageResult[V any] struct {
	Items []*V
	Next  *Request // nil = no more pages

	// Optional STAC metadata populated by LinkDecoder.
	NumberMatched  *int
	NumberReturned *int
}

// PageDecoder decodes an HTTP response into a page result.
//
// Decoders that maintain state across pages (offset, page-number, cursor) are
// expected to be returned by factory functions so that each iteration call
// gets a fresh closure. See OffsetDecoder, PageNumberDecoder, CursorDecoder.
type PageDecoder[V any] func(resp *http.Response) (*PageResult[V], error)

// Iterate returns an iterator over all items across paginated responses.
func Iterate[V any](ctx context.Context, cli *Client, initial *Request, decoder PageDecoder[V]) iter.Seq2[*V, error] {
	return func(yield func(*V, error) bool) {
		for page, err := range IteratePages(ctx, cli, initial, decoder) {
			if err != nil {
				yield(nil, err)
				return
			}
			for _, item := range page.Items {
				if !yield(item, nil) {
					return
				}
			}
		}
	}
}

// IteratePages returns an iterator over pages of results.
//
// The iterator detects pagination cycles (a next-link revisiting a previously
// fetched URL+method+body) and enforces the client's MaxPages limit.
func IteratePages[V any](ctx context.Context, cli *Client, initial *Request, decoder PageDecoder[V]) iter.Seq2[*PageResult[V], error] {
	return func(yield func(*PageResult[V], error) bool) {
		seen := map[string]struct{}{}
		pages := 0
		req := initial
		for req != nil {
			if cli.maxPages > 0 && pages >= cli.maxPages {
				yield(nil, fmt.Errorf("%w (limit=%d)", ErrPaginationLimit, cli.maxPages))
				return
			}

			resolved, err := cli.resolveURL(req.URL)
			if err != nil {
				yield(nil, err)
				return
			}
			if pages > 0 && !cli.hostAllowed(resolved) {
				yield(nil, fmt.Errorf("stac: pagination next-link host %q not allowed", resolved.Host))
				return
			}

			key := pageKey(req.Method, resolved, req.Body)
			if _, dup := seen[key]; dup {
				yield(nil, fmt.Errorf("%w at %s", ErrPaginationCycle, resolved.String()))
				return
			}
			seen[key] = struct{}{}
			pages++

			resp, err := cli.do(ctx, req, resolved)
			if err != nil {
				yield(nil, err)
				return
			}

			page, err := readPage(resp, cli.maxBodyBytes, decoder)
			if err != nil {
				yield(nil, err)
				return
			}

			if !yield(page, nil) {
				return
			}

			req = page.Next
		}
	}
}

// readPage validates the response, applies the body-size limit, and decodes.
func readPage[V any](resp *http.Response, maxBytes int64, decoder PageDecoder[V]) (*PageResult[V], error) {
	defer resp.Body.Close()
	if err := checkStatus(resp); err != nil {
		return nil, err
	}
	if err := checkJSONContentType(resp); err != nil {
		return nil, err
	}
	if maxBytes > 0 {
		resp.Body = &limitedReadCloser{r: &limitedReader{r: resp.Body, max: maxBytes}, c: resp.Body}
	}
	page, err := decoder(resp)
	if err != nil {
		// distinguish size-limit error
		var lerr *limitedReadError
		if errors.As(err, &lerr) {
			return nil, ErrResponseTooLarge
		}
		return nil, fmt.Errorf("stac: decode error: %w", err)
	}
	return page, nil
}

// Do executes a Request, returning the raw http.Response. The caller is
// responsible for closing the body. Status codes are NOT checked; use this
// only when you need the raw response.
func (c *Client) Do(ctx context.Context, req *Request) (*http.Response, error) {
	if req == nil {
		return nil, fmt.Errorf("stac: request is nil")
	}
	resolved, err := c.resolveURL(req.URL)
	if err != nil {
		return nil, err
	}
	return c.do(ctx, req, resolved)
}

// DoHTTP is the low-level escape hatch: it dispatches an arbitrary
// *http.Request through the client's middleware chain and HTTP client. The
// per-request timeout (WithTimeout) is applied via context.
//
// Status codes are NOT checked; the caller owns the response body.
func (c *Client) DoHTTP(ctx context.Context, req *http.Request) (*http.Response, error) {
	if req == nil {
		return nil, fmt.Errorf("stac: request is nil")
	}
	if c.timeout > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, c.timeout)
		req = req.WithContext(ctx)
		// We can't return cancel directly; tie it to the response body.
		resp, err := c.dispatch(req)
		if err != nil {
			cancel()
			return nil, err
		}
		resp.Body = &cancelOnClose{rc: resp.Body, cancel: cancel}
		return resp, nil
	}
	req = req.WithContext(ctx)
	return c.dispatch(req)
}

func (c *Client) dispatch(req *http.Request) (*http.Response, error) {
	if req.Header.Get("User-Agent") == "" && c.userAgent != "" {
		req.Header.Set("User-Agent", c.userAgent)
	}
	for _, mw := range c.middleware {
		if err := mw(req); err != nil {
			return nil, fmt.Errorf("stac: middleware: %w", err)
		}
	}
	return c.httpClient.Do(req)
}

// do is the internal entry point shared by Do and IteratePages.
func (c *Client) do(ctx context.Context, req *Request, resolved *url.URL) (*http.Response, error) {
	method := req.Method
	if method == "" {
		method = http.MethodGet
	}

	var body io.Reader
	if req.Body != nil {
		b, err := json.Marshal(req.Body)
		if err != nil {
			return nil, fmt.Errorf("stac: marshal body: %w", err)
		}
		body = bytes.NewReader(b)
	}

	if c.timeout > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, c.timeout)
		httpReq, err := http.NewRequestWithContext(ctx, method, resolved.String(), body)
		if err != nil {
			cancel()
			return nil, fmt.Errorf("stac: create request: %w", err)
		}
		c.applyHeaders(httpReq, req)
		resp, err := c.dispatch(httpReq)
		if err != nil {
			cancel()
			return nil, err
		}
		resp.Body = &cancelOnClose{rc: resp.Body, cancel: cancel}
		return resp, nil
	}

	httpReq, err := http.NewRequestWithContext(ctx, method, resolved.String(), body)
	if err != nil {
		return nil, fmt.Errorf("stac: create request: %w", err)
	}
	c.applyHeaders(httpReq, req)
	return c.dispatch(httpReq)
}

func (c *Client) applyHeaders(httpReq *http.Request, req *Request) {
	if req.Body != nil {
		httpReq.Header.Set("Content-Type", "application/json")
	}
	if httpReq.Header.Get("Accept") == "" {
		httpReq.Header.Set("Accept", "application/json, application/geo+json")
	}
	for k, v := range req.Headers {
		httpReq.Header.Set(k, v)
	}
}

// resolveURL parses path and resolves it against the base URL.
func (c *Client) resolveURL(path string) (*url.URL, error) {
	u, err := url.Parse(path)
	if err != nil {
		return nil, fmt.Errorf("stac: invalid URL %q: %w", path, err)
	}
	return c.baseURL.ResolveReference(u), nil
}

// LinkToRequest converts a STAC link to a Request, honoring optional
// "method", "body", and "headers" foreign members on the link.
func LinkToRequest(link *stac.Link) *Request {
	if link == nil {
		return nil
	}
	req := &Request{Method: http.MethodGet, URL: link.Href}
	if link.AdditionalFields == nil {
		return req
	}
	if m, ok := link.AdditionalFields["method"].(string); ok {
		req.Method = strings.ToUpper(m)
	}
	if b := link.AdditionalFields["body"]; b != nil {
		req.Body = b
	}
	if h, ok := link.AdditionalFields["headers"].(map[string]any); ok {
		req.Headers = make(map[string]string, len(h))
		for k, v := range h {
			if s, ok := v.(string); ok {
				req.Headers[k] = s
			}
		}
	}
	return req
}

// pageKey builds a stable key for a request: METHOD + canonical URL +
// sha256(body) so that two requests with identical effects collide.
func pageKey(method string, u *url.URL, body any) string {
	if method == "" {
		method = http.MethodGet
	}
	u2 := *u
	u2.Fragment = ""
	if body == nil {
		return method + " " + u2.String()
	}
	b, err := json.Marshal(body)
	if err != nil {
		// If we can't marshal, fall back to URL only — the cycle guard is
		// best-effort.
		return method + " " + u2.String()
	}
	sum := sha256.Sum256(b)
	return method + " " + u2.String() + " " + hex.EncodeToString(sum[:])
}

// checkStatus validates that resp is a 2xx response, otherwise builds a
// typed *HTTPError.
func checkStatus(resp *http.Response) error {
	if resp.StatusCode >= 200 && resp.StatusCode < 300 {
		return nil
	}
	body, _ := io.ReadAll(io.LimitReader(resp.Body, MaxErrorBodyBytes))
	herr := &HTTPError{
		Status:     resp.StatusCode,
		Method:     resp.Request.Method,
		URL:        resp.Request.URL.Redacted(),
		Body:       body,
		RetryAfter: parseRetryAfter(resp.Header.Get("Retry-After")),
	}
	// Try to decode the body as a STAC API error for richer context.
	if isJSONContentType(resp.Header.Get("Content-Type")) && len(body) > 0 {
		var api APIError
		if err := json.Unmarshal(body, &api); err == nil && (api.Code != 0 || api.Description != "" || api.Type != "") {
			herr.API = &api
		}
	}
	return herr
}

// checkJSONContentType validates that the response is JSON-ish.
//
// Accepts application/json, application/geo+json, and any vendor extension
// with a +json suffix. Empty Content-Type is tolerated (some upstream proxies
// strip the header).
func checkJSONContentType(resp *http.Response) error {
	ct := resp.Header.Get("Content-Type")
	if ct == "" || isJSONContentType(ct) {
		return nil
	}
	return fmt.Errorf("%w: %q for %s %s",
		ErrUnexpectedContentType, ct, resp.Request.Method, resp.Request.URL.Redacted())
}

func isJSONContentType(ct string) bool {
	if ct == "" {
		return false
	}
	mt, _, err := mime.ParseMediaType(ct)
	if err != nil {
		mt = strings.ToLower(strings.TrimSpace(strings.SplitN(ct, ";", 2)[0]))
	}
	mt = strings.ToLower(mt)
	switch mt {
	case "application/json",
		"application/geo+json",
		"application/schema+json",
		"application/problem+json",
		"text/json":
		return true
	}
	return strings.HasSuffix(mt, "+json")
}

// limitBody wraps the body with a size limit. The body is replaced with a
// reader that returns *limitedReadError once the limit is exceeded.
func limitBody(resp *http.Response, max int64) io.Reader {
	if max <= 0 {
		return resp.Body
	}
	return &limitedReader{r: resp.Body, max: max}
}

// limitedReadCloser couples a limitedReader with the underlying body's
// Close method.
type limitedReadCloser struct {
	r io.Reader
	c io.Closer
}

func (l *limitedReadCloser) Read(p []byte) (int, error) { return l.r.Read(p) }
func (l *limitedReadCloser) Close() error               { return l.c.Close() }

type limitedReader struct {
	r     io.Reader
	max   int64
	count int64
}

func (l *limitedReader) Read(p []byte) (int, error) {
	if l.count >= l.max {
		return 0, &limitedReadError{Limit: l.max}
	}
	if int64(len(p)) > l.max-l.count {
		p = p[:l.max-l.count+1] // allow one extra byte to detect overflow
	}
	n, err := l.r.Read(p)
	l.count += int64(n)
	if l.count > l.max {
		return n, &limitedReadError{Limit: l.max}
	}
	return n, err
}

type limitedReadError struct{ Limit int64 }

func (e *limitedReadError) Error() string {
	return fmt.Sprintf("stac: response body exceeded %d bytes", e.Limit)
}

// cancelOnClose ties a context.CancelFunc to a response body's lifecycle so
// that closing the body releases the request-scoped context.
type cancelOnClose struct {
	rc     io.ReadCloser
	cancel context.CancelFunc
	once   bool
}

func (c *cancelOnClose) Read(p []byte) (int, error) { return c.rc.Read(p) }
func (c *cancelOnClose) Close() error {
	err := c.rc.Close()
	if !c.once && c.cancel != nil {
		c.cancel()
		c.once = true
	}
	return err
}
