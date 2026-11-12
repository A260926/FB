package grab

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"
)

// stubClient is an in-memory HTTPClient that records the headers of every
// request and returns caller-controlled response streams.
type stubClient struct {
	mu   sync.Mutex
	reqs []recordedRequest
	fn   func(*http.Request) (*http.Response, error)
}

type recordedRequest struct {
	method  string
	headers http.Header
}

func (s *stubClient) Do(req *http.Request) (*http.Response, error) {
	s.mu.Lock()
	s.reqs = append(s.reqs, recordedRequest{
		method:  req.Method,
		headers: req.Header.Clone(),
	})
	fn := s.fn
	s.mu.Unlock()
	return fn(req)
}

func (s *stubClient) count() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.reqs)
}

func (s *stubClient) snapshot() []recordedRequest {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]recordedRequest, len(s.reqs))
	copy(out, s.reqs)
	return out
}

func stubResponse(req *http.Request, status int, headers http.Header, body []byte) *http.Response {
	// Canonicalize keys the way a real transport does: http.Header.Get only
	// finds canonical MIME keys, and literals such as "ETag" would otherwise
	// be invisible to it.
	h := http.Header{}
	for k, vs := range headers {
		for _, v := range vs {
			h.Add(k, v)
		}
	}
	return &http.Response{
		StatusCode:    status,
		Header:        h,
		Body:          io.NopCloser(bytes.NewReader(body)),
		ContentLength: int64(len(body)),
		Request:       req,
	}
}

// mpPart is one part of a multipart/byteranges response.
type mpPart struct {
	contentRange string
	body         []byte
}

func multipartBody(boundary string, parts []mpPart) ([]byte, string) {
	var b strings.Builder
	for _, p := range parts {
		fmt.Fprintf(&b, "--%s\r\n", boundary)
		fmt.Fprintf(&b, "Content-Range: %s\r\n", p.contentRange)
		b.WriteString("Content-Type: application/octet-stream\r\n\r\n")
		b.Write(p.body)
		b.WriteString("\r\n")
	}
	fmt.Fprintf(&b, "--%s--\r\n", boundary)
	return []byte(b.String()), "multipart/byteranges; boundary=" + boundary
}

// sparseWriter stores only the bytes it receives, keyed by absolute offset.
// A huge logical offset therefore costs only the bytes actually written.
type sparseWriter struct {
	mu     sync.Mutex
	cells  map[int64]byte
	maxOff int64
	writes int
}

func newSparseWriter() *sparseWriter {
	return &sparseWriter{cells: map[int64]byte{}}
}

func (w *sparseWriter) WriteAt(p []byte, off int64) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	for i, b := range p {
		w.cells[off+int64(i)] = b
	}
	if end := off + int64(len(p)) - 1; end > w.maxOff {
		w.maxOff = end
	}
	w.writes++
	return len(p), nil
}

func (w *sparseWriter) string(n int) string {
	w.mu.Lock()
	defer w.mu.Unlock()
	out := make([]byte, n)
	for i := range out {
		b, ok := w.cells[int64(i)]
		if !ok {
			b = '.'
		}
		out[i] = b
	}
	return string(out)
}

func (w *sparseWriter) allocatedBytes() int {
	w.mu.Lock()
	defer w.mu.Unlock()
	return len(w.cells)
}

// shortWriter returns a short write with a nil error on every call.
type shortWriter struct {
	inner *sparseWriter
}

func (w shortWriter) WriteAt(p []byte, off int64) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	if _, err := w.inner.WriteAt(p[:1], off); err != nil {
		return 0, err
	}
	return 1, nil
}

func newTemplate(t *testing.T) *http.Request {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, "http://example.test/resource", nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("X-Trace", "kept")
	return req
}

func assertNoSpoolLeft(t *testing.T, dir string) {
	t.Helper()
	left, err := filepath.Glob(filepath.Join(dir, "grab-sparse-*.tmp"))
	if err != nil {
		t.Fatal(err)
	}
	if len(left) != 0 {
		t.Fatalf("temporary spool files were not cleaned up: %v", left)
	}
}

func TestSparseRangeCapture(t *testing.T) {
	const resource = "ABCDEFGHIJ" // 10 bytes

	t.Run("normalizes the plan without touching the input", func(t *testing.T) {
		tmp := t.TempDir()
		client := &Client{HTTPClient: &stubClient{fn: func(*http.Request) (*http.Response, error) {
			t.Fatal("no request expected for an empty plan")
			return nil, nil
		}}, UserAgent: DefaultUserAgent}

		// An empty plan sends nothing and writes nothing.
		res, err := client.FetchRanges(context.Background(), newTemplate(t), newSparseWriter(), nil, RangeOptions{BatchSize: 4, TempDir: tmp})
		if err != nil {
			t.Fatal(err)
		}
		if res.Size != -1 {
			t.Fatalf("empty-plan Size = %d, want -1", res.Size)
		}
		assertNoSpoolLeft(t, tmp)

		// The caller's slice is never modified by normalization.
		in := []ByteRange{{8, 10}, {0, 2}, {1, 3}, {0, 2}}
		orig := append([]ByteRange(nil), in...)
		defer func() {
			if !slices.Equal(in, orig) {
				t.Fatalf("caller slice modified: %v -> %v", orig, in)
			}
		}()

		// Non-empty normalization: overlapping and adjacent ranges merge.
		var calls int
		client.HTTPClient = &stubClient{fn: func(req *http.Request) (*http.Response, error) {
			calls++
			if got := req.Header.Get("Range"); got != "bytes=0-2,8-9" {
				t.Fatalf("Range header = %q, want bytes=0-2,8-9", got)
			}
			body, contentType := multipartBody("b", []mpPart{
				{contentRange: "bytes 0-2/10", body: []byte("ABC")},
				{contentRange: "bytes 8-9/10", body: []byte("IJ")},
			})
			return stubResponse(req, http.StatusPartialContent, http.Header{
				"Content-Type": []string{contentType},
				"ETag":         []string{`"v1"`},
			}, body), nil
		}}
		dst := newSparseWriter()
		res, err = client.FetchRanges(context.Background(), newTemplate(t), dst, in, RangeOptions{BatchSize: 4, TempDir: tmp})
		if err != nil {
			t.Fatal(err)
		}
		wantPlan := []ByteRange{{0, 3}, {8, 10}}
		if fmt.Sprint(res.Ranges) != fmt.Sprint(wantPlan) {
			t.Fatalf("normalized plan = %v, want %v", res.Ranges, wantPlan)
		}
		if calls != 1 || dst.string(10) != "ABC.....IJ" {
			t.Fatalf("calls=%d dst=%q", calls, dst.string(10))
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("multipart parts may be out of order and merge across gaps", func(t *testing.T) {
		tmp := t.TempDir()
		stub := &stubClient{}
		// One request carries two ranges; the server answers with a single
		// part that spans the unrequested gap [2,4). Parts may also arrive in
		// any order - here a second, earlier part comes first.
		stub.fn = func(req *http.Request) (*http.Response, error) {
			if got := req.Header.Get("Range"); got != "bytes=0-1,4-5,8-9" {
				t.Fatalf("Range = %q", got)
			}
			body, contentType := multipartBody("xyz", []mpPart{
				{contentRange: "bytes 8-9/10", body: []byte("IJ")},
				{contentRange: "bytes 0-5/10", body: []byte("ABCDEF")},
			})
			return stubResponse(req, http.StatusPartialContent, http.Header{
				"Content-Type": []string{contentType},
				"ETag":         []string{`"v1"`},
			}, body), nil
		}
		client := &Client{HTTPClient: stub, UserAgent: DefaultUserAgent}
		dst := newSparseWriter()
		res, err := client.FetchRanges(context.Background(), newTemplate(t), dst,
			[]ByteRange{{0, 2}, {4, 6}, {8, 10}}, RangeOptions{BatchSize: 3, TempDir: tmp})
		if err != nil {
			t.Fatal(err)
		}
		if res.Size != 10 || res.ETag != `"v1"` {
			t.Fatalf("result = %+v", res)
		}
		if got := dst.string(10); got != "AB..EF..IJ" {
			t.Fatalf("dst = %q, gap bytes must not be published", got)
		}
		if stub.count() != 1 {
			t.Fatalf("requests = %d, want 1", stub.count())
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("single Content-Range body restores original offsets", func(t *testing.T) {
		tmp := t.TempDir()
		stub := &stubClient{}
		stub.fn = func(req *http.Request) (*http.Response, error) {
			if got := req.Header.Get("Accept-Encoding"); got != "identity" {
				t.Fatalf("Accept-Encoding = %q, want identity", got)
			}
			if got := req.Header.Get("X-Trace"); got != "kept" {
				t.Fatalf("custom header lost: %q", got)
			}
			return stubResponse(req, http.StatusPartialContent, http.Header{
				"Content-Type":  []string{"application/octet-stream"},
				"Content-Range": []string{"bytes 2-4/10"},
			}, []byte("CDE")), nil
		}
		client := &Client{HTTPClient: stub, UserAgent: DefaultUserAgent}
		dst := newSparseWriter()
		res, err := client.FetchRanges(context.Background(), newTemplate(t), dst,
			[]ByteRange{{2, 5}}, RangeOptions{BatchSize: 1, TempDir: tmp})
		if err != nil {
			t.Fatal(err)
		}
		if res.Size != 10 || res.ETag != "" {
			t.Fatalf("result = %+v", res)
		}
		if got := dst.string(10); got != "..CDE....." {
			t.Fatalf("dst = %q", got)
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("first response 200 falls back to the full representation", func(t *testing.T) {
		tmp := t.TempDir()
		stub := &stubClient{}
		stub.fn = func(req *http.Request) (*http.Response, error) {
			return stubResponse(req, http.StatusOK, http.Header{
				"Content-Type": []string{"application/octet-stream"},
			}, []byte(resource)), nil
		}
		client := &Client{HTTPClient: stub, UserAgent: DefaultUserAgent}
		dst := newSparseWriter()
		res, err := client.FetchRanges(context.Background(), newTemplate(t), dst,
			[]ByteRange{{0, 2}, {8, 10}}, RangeOptions{BatchSize: 1, TempDir: tmp})
		if err != nil {
			t.Fatal(err)
		}
		if res.Size != 10 {
			t.Fatalf("Size = %d, want 10", res.Size)
		}
		if stub.count() != 1 {
			t.Fatalf("requests = %d, the 200 fallback must stop further batches", stub.count())
		}
		if got := dst.string(10); got != "AB......IJ" {
			t.Fatalf("dst = %q", got)
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("200 on a later batch is rejected and not published", func(t *testing.T) {
		tmp := t.TempDir()
		stub := &stubClient{}
		stub.fn = func(req *http.Request) (*http.Response, error) {
			if stub.count() == 1 {
				return stubResponse(req, http.StatusPartialContent, http.Header{
					"Content-Range": []string{"bytes 0-1/10"},
					"ETag":          []string{`"v1"`},
				}, []byte("AB")), nil
			}
			return stubResponse(req, http.StatusOK, http.Header{}, []byte(resource)), nil
		}
		client := &Client{HTTPClient: stub, UserAgent: DefaultUserAgent}
		dst := newSparseWriter()
		_, err := client.FetchRanges(context.Background(), newTemplate(t), dst,
			[]ByteRange{{0, 2}, {8, 10}}, RangeOptions{BatchSize: 1, TempDir: tmp})
		if err == nil || dst.allocatedBytes() != 0 {
			t.Fatalf("err=%v published=%d", err, dst.allocatedBytes())
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("truncated and oversized parts fail and publish nothing", func(t *testing.T) {
		cases := map[string]func(*http.Request) (*http.Response, error){
			"truncated": func(req *http.Request) (*http.Response, error) {
				return stubResponse(req, http.StatusPartialContent, http.Header{
					"Content-Range": []string{"bytes 0-2/10"},
				}, []byte("AB")), nil // declares 3 bytes, sends 2
			},
			"extra bytes": func(req *http.Request) (*http.Response, error) {
				return stubResponse(req, http.StatusPartialContent, http.Header{
					"Content-Range": []string{"bytes 0-1/10"},
				}, []byte("ABC")), nil // declares 2 bytes, sends 3
			},
			"irrelevant part": func(req *http.Request) (*http.Response, error) {
				return stubResponse(req, http.StatusPartialContent, http.Header{
					"Content-Range": []string{"bytes 5-6/10"},
				}, []byte("FG")), nil
			},
		}
		for name, fn := range cases {
			t.Run(name, func(t *testing.T) {
				tmp := t.TempDir()
				client := &Client{HTTPClient: &stubClient{fn: fn}, UserAgent: DefaultUserAgent}
				dst := newSparseWriter()
				_, err := client.FetchRanges(context.Background(), newTemplate(t), dst,
					[]ByteRange{{0, 2}}, RangeOptions{BatchSize: 1, TempDir: tmp})
				if err == nil {
					t.Fatal("expected an error")
				}
				if !errors.Is(err, ErrBadLength) && name != "irrelevant part" {
					t.Fatalf("err = %v, want ErrBadLength", err)
				}
				if dst.allocatedBytes() != 0 {
					t.Fatalf("destination changed on failure: %d bytes", dst.allocatedBytes())
				}
				assertNoSpoolLeft(t, tmp)
			})
		}
	})

	t.Run("overlapping response parts fail", func(t *testing.T) {
		tmp := t.TempDir()
		fn := func(req *http.Request) (*http.Response, error) {
			body, contentType := multipartBody("b", []mpPart{
				{contentRange: "bytes 0-2/10", body: []byte("ABC")},
				{contentRange: "bytes 2-3/10", body: []byte("CD")},
			})
			return stubResponse(req, http.StatusPartialContent, http.Header{
				"Content-Type": []string{contentType},
			}, body), nil
		}
		client := &Client{HTTPClient: &stubClient{fn: fn}, UserAgent: DefaultUserAgent}
		dst := newSparseWriter()
		_, err := client.FetchRanges(context.Background(), newTemplate(t), dst,
			[]ByteRange{{0, 4}}, RangeOptions{BatchSize: 1, TempDir: tmp})
		if err == nil || dst.allocatedBytes() != 0 {
			t.Fatalf("err=%v published=%d", err, dst.allocatedBytes())
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("WriterAt short write errors without rollback requirement", func(t *testing.T) {
		tmp := t.TempDir()
		fn := func(req *http.Request) (*http.Response, error) {
			return stubResponse(req, http.StatusPartialContent, http.Header{
				"Content-Range": []string{"bytes 0-3/10"},
			}, []byte("ABCD")), nil
		}
		client := &Client{HTTPClient: &stubClient{fn: fn}, UserAgent: DefaultUserAgent}
		dst := newSparseWriter()
		_, err := client.FetchRanges(context.Background(), newTemplate(t),
			shortWriter{inner: dst}, []ByteRange{{0, 4}}, RangeOptions{BatchSize: 1, TempDir: tmp})
		if !errors.Is(err, io.ErrShortWrite) {
			t.Fatalf("err = %v, want io.ErrShortWrite", err)
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("rejects bad plans and requests before any I/O", func(t *testing.T) {
		tmp := t.TempDir()
		client := &Client{HTTPClient: &stubClient{fn: func(*http.Request) (*http.Response, error) {
			t.Fatal("no request expected")
			return nil, nil
		}}, UserAgent: DefaultUserAgent}
		dst := newSparseWriter()

		bad := []struct {
			name   string
			tmpl   func() *http.Request
			ranges []ByteRange
			opts   RangeOptions
		}{
			{"negative start", func() *http.Request { return newTemplate(t) }, []ByteRange{{-1, 2}}, RangeOptions{BatchSize: 1}},
			{"empty range", func() *http.Request { return newTemplate(t) }, []ByteRange{{3, 3}}, RangeOptions{BatchSize: 1}},
			{"inverted range", func() *http.Request { return newTemplate(t) }, []ByteRange{{5, 4}}, RangeOptions{BatchSize: 1}},
			{"zero batch size", func() *http.Request { return newTemplate(t) }, []ByteRange{{0, 1}}, RangeOptions{BatchSize: 0}},
			{"preset Range", func() *http.Request {
				r := newTemplate(t)
				r.Header.Set("Range", "bytes=0-1")
				return r
			}, []ByteRange{{0, 1}}, RangeOptions{BatchSize: 1}},
			{"preset If-Match", func() *http.Request {
				r := newTemplate(t)
				r.Header.Set("If-Match", `"v1"`)
				return r
			}, []ByteRange{{0, 1}}, RangeOptions{BatchSize: 1}},
			{"preset If-Range", func() *http.Request {
				r := newTemplate(t)
				r.Header.Set("If-Range", `"v1"`)
				return r
			}, []ByteRange{{0, 1}}, RangeOptions{BatchSize: 1}},
			{"request body", func() *http.Request {
				r := newTemplate(t)
				r.Body = io.NopCloser(strings.NewReader("x"))
				return r
			}, []ByteRange{{0, 1}}, RangeOptions{BatchSize: 1}},
		}
		for _, tc := range bad {
			t.Run(tc.name, func(t *testing.T) {
				if _, err := client.FetchRanges(context.Background(), tc.tmpl(), dst, tc.ranges, tc.opts); err == nil {
					t.Fatal("expected validation error")
				}
			})
		}
		postReq, _ := http.NewRequest(http.MethodPost, "http://example.test/", nil)
		if _, err := client.FetchRanges(context.Background(), postReq, dst,
			[]ByteRange{{0, 1}}, RangeOptions{BatchSize: 1}); err == nil {
			t.Fatal("POST request must be rejected")
		}
		if dst.allocatedBytes() != 0 {
			t.Fatal("validation failures must not touch the destination")
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("baseline Do still streams the raw multipart body", func(t *testing.T) {
		raw, contentType := multipartBody("b", []mpPart{
			{contentRange: "bytes 0-1/10", body: []byte("AB")},
			{contentRange: "bytes 8-9/10", body: []byte("IJ")},
		})
		client := &Client{HTTPClient: &stubClient{fn: func(req *http.Request) (*http.Response, error) {
			return stubResponse(req, http.StatusPartialContent, http.Header{
				"Content-Type": []string{contentType},
			}, raw), nil
		}}, UserAgent: DefaultUserAgent}

		hreq, err := http.NewRequest(http.MethodGet, "http://example.test/resource", nil)
		if err != nil {
			t.Fatal(err)
		}
		req := &Request{HTTPRequest: hreq, NoStore: true}
		resp := client.Do(req)
		got, err := resp.Bytes()
		if err != nil {
			t.Fatal(err)
		}
		// Current behavior: Do does not understand multipart/byteranges and
		// hands back the wire body verbatim instead of restoring offsets.
		if !bytes.Equal(got, raw) {
			t.Fatalf("Do changed behavior: got %q want raw %q", got, raw)
		}
		if !strings.Contains(string(got), "Content-Range:") ||
			!strings.Contains(string(got), "--b--") {
			t.Fatal("baseline body lost its MIME framing")
		}
	})
}

func TestSparseRangeConsistency(t *testing.T) {
	t.Run("carries If-Match across batches and pins ETag and size", func(t *testing.T) {
		tmp := t.TempDir()
		stub := &stubClient{}
		stub.fn = func(req *http.Request) (*http.Response, error) {
			n := stub.count()
			if n == 1 {
				if req.Header.Get("If-Match") != "" {
					t.Fatal("first request must not carry If-Match")
				}
			} else if got := req.Header.Get("If-Match"); got != `"v1"` {
				t.Fatalf("request %d If-Match = %q", n, got)
			}
			if got := req.Header.Get("User-Agent"); got != DefaultUserAgent {
				t.Fatalf("User-Agent = %q", got)
			}
			ranges := map[int][2]int64{1: {0, 2}, 2: {4, 6}, 3: {8, 10}}[n]
			return stubResponse(req, http.StatusPartialContent, http.Header{
				"Content-Range": []string{fmt.Sprintf("bytes %d-%d/10", ranges[0], ranges[1]-1)},
				"ETag":          []string{`"v1"`},
			}, []byte("ABCDEFGHIJ"[ranges[0]:ranges[1]])), nil
		}
		client := &Client{HTTPClient: stub, UserAgent: DefaultUserAgent}
		tmpl := newTemplate(t)
		dst := newSparseWriter()
		res, err := client.FetchRanges(context.Background(), tmpl, dst,
			[]ByteRange{{0, 2}, {4, 6}, {8, 10}}, RangeOptions{BatchSize: 1, TempDir: tmp})
		if err != nil {
			t.Fatal(err)
		}
		if stub.count() != 3 || res.Size != 10 || res.ETag != `"v1"` {
			t.Fatalf("count=%d res=%+v", stub.count(), res)
		}
		if got := dst.string(10); got != "AB..EF..IJ" {
			t.Fatalf("dst = %q", got)
		}
		// The shared template and its header map are never mutated.
		if tmpl.Header.Get("Range") != "" || tmpl.Header.Get("If-Match") != "" {
			t.Fatal("template headers were modified")
		}
		for _, r := range stub.snapshot() {
			if r.method != http.MethodGet {
				t.Fatalf("method = %s", r.method)
			}
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("412, ETag change and size change are representation errors", func(t *testing.T) {
		cases := map[string]func(*http.Request, int) (*http.Response, error){
			"412": func(req *http.Request, _ int) (*http.Response, error) {
				return stubResponse(req, http.StatusPreconditionFailed, http.Header{}, nil), nil
			},
			"etag changed": func(req *http.Request, n int) (*http.Response, error) {
				etag := `"v1"`
				if n == 2 {
					etag = `"v2"`
				}
				return stubResponse(req, http.StatusPartialContent, http.Header{
					"Content-Range": []string{"bytes 8-9/10"},
					"ETag":          []string{etag},
				}, []byte("IJ")), nil
			},
			"size changed": func(req *http.Request, n int) (*http.Response, error) {
				total := 10
				if n == 2 {
					total = 11
				}
				return stubResponse(req, http.StatusPartialContent, http.Header{
					"Content-Range": []string{fmt.Sprintf("bytes 8-9/%d", total)},
					"ETag":          []string{`"v1"`},
				}, []byte("IJ")), nil
			},
			"etag missing later": func(req *http.Request, n int) (*http.Response, error) {
				h := http.Header{"Content-Range": []string{"bytes 8-9/10"}}
				if n == 1 {
					h.Set("ETag", `"v1"`)
				}
				return stubResponse(req, http.StatusPartialContent, h, []byte("IJ")), nil
			},
		}
		for name, build := range cases {
			t.Run(name, func(t *testing.T) {
				tmp := t.TempDir()
				stub := &stubClient{}
				stub.fn = func(req *http.Request) (*http.Response, error) {
					n := stub.count()
					if n == 1 {
						return stubResponse(req, http.StatusPartialContent, http.Header{
							"Content-Range": []string{"bytes 0-1/10"},
							"ETag":          []string{`"v1"`},
						}, []byte("AB")), nil
					}
					return build(req, n)
				}
				client := &Client{HTTPClient: stub, UserAgent: DefaultUserAgent}
				dst := newSparseWriter()
				_, err := client.FetchRanges(context.Background(), newTemplate(t), dst,
					[]ByteRange{{0, 2}, {8, 10}}, RangeOptions{BatchSize: 1, TempDir: tmp})
				if !errors.Is(err, ErrRangeRepresentationChanged) {
					t.Fatalf("err = %v, want ErrRangeRepresentationChanged", err)
				}
				if dst.allocatedBytes() != 0 {
					t.Fatal("representation change must not publish any bytes")
				}
				assertNoSpoolLeft(t, tmp)
			})
		}
	})

	t.Run("weak or missing ETag allowed for one batch only", func(t *testing.T) {
		tmp := t.TempDir()
		fn := func(weak bool) func(*http.Request) (*http.Response, error) {
			return func(req *http.Request) (*http.Response, error) {
				h := http.Header{"Content-Range": []string{"bytes 0-1/10"}}
				if weak {
					h.Set("ETag", `W/"v1"`)
				}
				return stubResponse(req, http.StatusPartialContent, h, []byte("AB")), nil
			}
		}
		dst := newSparseWriter()
		client := &Client{HTTPClient: &stubClient{fn: fn(false)}, UserAgent: DefaultUserAgent}
		if _, err := client.FetchRanges(context.Background(), newTemplate(t), dst,
			[]ByteRange{{0, 2}}, RangeOptions{BatchSize: 1, TempDir: tmp}); err != nil {
			t.Fatalf("single batch without ETag failed: %v", err)
		}
		if got := dst.string(10); got != "AB........" {
			t.Fatalf("dst = %q", got)
		}

		var err error
		client.HTTPClient = &stubClient{fn: fn(true)}
		_, err = client.FetchRanges(context.Background(), newTemplate(t), newSparseWriter(),
			[]ByteRange{{0, 2}, {8, 10}}, RangeOptions{BatchSize: 1, TempDir: tmp})
		if !errors.Is(err, ErrRangeRepresentationChanged) {
			t.Fatalf("multi-batch weak ETag err = %v, want ErrRangeRepresentationChanged", err)
		}

		client.HTTPClient = &stubClient{fn: fn(false)}
		_, err = client.FetchRanges(context.Background(), newTemplate(t), newSparseWriter(),
			[]ByteRange{{0, 2}, {8, 10}}, RangeOptions{BatchSize: 1, TempDir: tmp})
		if !errors.Is(err, ErrRangeRepresentationChanged) {
			t.Fatalf("multi-batch missing ETag err = %v, want ErrRangeRepresentationChanged", err)
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("cancellation leaves the destination and temp dir untouched", func(t *testing.T) {
		tmp := t.TempDir()
		entered := make(chan struct{})
		stub := &stubClient{}
		stub.fn = func(req *http.Request) (*http.Response, error) {
			close(entered)
			<-req.Context().Done()
			return nil, req.Context().Err()
		}
		client := &Client{HTTPClient: stub, UserAgent: DefaultUserAgent}
		ctx, cancel := context.WithCancel(context.Background())
		dst := newSparseWriter()
		errCh := make(chan error, 1)
		go func() {
			_, err := client.FetchRanges(ctx, newTemplate(t), dst,
				[]ByteRange{{0, 2}, {8, 10}}, RangeOptions{BatchSize: 1, TempDir: tmp})
			errCh <- err
		}()
		<-entered
		cancel()
		select {
		case err := <-errCh:
			if !errors.Is(err, context.Canceled) {
				t.Fatalf("err = %v, want context.Canceled", err)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("FetchRanges did not return after cancellation")
		}
		if dst.allocatedBytes() != 0 {
			t.Fatal("cancellation published bytes")
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("concurrent calls keep independent spools and progress", func(t *testing.T) {
		tmp := t.TempDir()
		stub := &stubClient{}
		stub.fn = func(req *http.Request) (*http.Response, error) {
			// Two resources, each a ten-byte alphabet with its own ETag.
			var body []byte
			var etag string
			switch req.URL.Path {
			case "/a":
				body, etag = []byte("ABCDEFGHIJ"), `"a"`
			case "/b":
				body, etag = []byte("0123456789"), `"b"`
			default:
				t.Fatalf("unexpected path %q", req.URL.Path)
			}
			// Every batch here is a single range.
			rng := strings.TrimPrefix(req.Header.Get("Range"), "bytes=")
			parts := strings.SplitN(rng, "-", 2)
			s := mustAtoi(t, parts[0])
			e := mustAtoi(t, parts[1])
			return stubResponse(req, http.StatusPartialContent, http.Header{
				"Content-Range": []string{fmt.Sprintf("bytes %d-%d/10", s, e)},
				"ETag":          []string{etag},
			}, body[s:e+1]), nil
		}
		client := &Client{HTTPClient: stub, UserAgent: DefaultUserAgent}

		mkReq := func(path string) *http.Request {
			r, err := http.NewRequest(http.MethodGet, "http://example.test"+path, nil)
			if err != nil {
				t.Fatal(err)
			}
			return r
		}
		dstA, dstB := newSparseWriter(), newSparseWriter()
		var wg sync.WaitGroup
		errs := make(chan error, 2)
		wg.Add(2)
		go func() {
			defer wg.Done()
			_, err := client.FetchRanges(context.Background(), mkReq("/a"), dstA,
				[]ByteRange{{0, 2}, {8, 10}}, RangeOptions{BatchSize: 1, TempDir: tmp})
			errs <- err
		}()
		go func() {
			defer wg.Done()
			_, err := client.FetchRanges(context.Background(), mkReq("/b"), dstB,
				[]ByteRange{{2, 4}, {6, 8}}, RangeOptions{BatchSize: 1, TempDir: tmp})
			errs <- err
		}()
		wg.Wait()
		close(errs)
		for err := range errs {
			if err != nil {
				t.Fatal(err)
			}
		}
		if got := dstA.string(10); got != "AB......IJ" {
			t.Fatalf("dstA = %q", got)
		}
		if got := dstB.string(10); got != "..23..67.." {
			t.Fatalf("dstB = %q", got)
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("huge logical offsets allocate only the fetched bytes", func(t *testing.T) {
		tmp := t.TempDir()
		const huge = int64(1) << 50
		fn := func(req *http.Request) (*http.Response, error) {
			return stubResponse(req, http.StatusPartialContent, http.Header{
				"Content-Range": []string{fmt.Sprintf("bytes %d-%d/%d", huge, huge+3, huge+4)},
			}, []byte("WXYZ")), nil
		}
		client := &Client{HTTPClient: &stubClient{fn: fn}, UserAgent: DefaultUserAgent}
		dst := newSparseWriter()
		res, err := client.FetchRanges(context.Background(), newTemplate(t), dst,
			[]ByteRange{{huge, huge + 4}}, RangeOptions{BatchSize: 1, TempDir: tmp})
		if err != nil {
			t.Fatal(err)
		}
		if res.Size != huge+4 {
			t.Fatalf("Size = %d", res.Size)
		}
		if dst.maxOff != huge+3 {
			t.Fatalf("maxOff = %d", dst.maxOff)
		}
		if dst.allocatedBytes() != 4 {
			t.Fatalf("allocated %d bytes for a 4-byte range", dst.allocatedBytes())
		}
		if got := dst.cells[huge]; got != 'W' || dst.cells[huge+3] != 'Z' {
			t.Fatalf("bytes not restored at the original offset: %q %q", dst.cells[huge], dst.cells[huge+3])
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("plan beyond the resource length is rejected", func(t *testing.T) {
		tmp := t.TempDir()
		fn := func(req *http.Request) (*http.Response, error) {
			return stubResponse(req, http.StatusPartialContent, http.Header{
				"Content-Range": []string{"bytes 0-1/10"},
			}, []byte("AB")), nil
		}
		client := &Client{HTTPClient: &stubClient{fn: fn}, UserAgent: DefaultUserAgent}
		dst := newSparseWriter()
		_, err := client.FetchRanges(context.Background(), newTemplate(t), dst,
			[]ByteRange{{0, 2}, {9, 11}}, RangeOptions{BatchSize: 2, TempDir: tmp})
		if !errors.Is(err, ErrBadLength) || dst.allocatedBytes() != 0 {
			t.Fatalf("err=%v published=%d", err, dst.allocatedBytes())
		}
		assertNoSpoolLeft(t, tmp)
	})

	t.Run("unknown Content-Encoding is rejected", func(t *testing.T) {
		tmp := t.TempDir()
		fn := func(req *http.Request) (*http.Response, error) {
			return stubResponse(req, http.StatusPartialContent, http.Header{
				"Content-Range":    []string{"bytes 0-1/10"},
				"Content-Encoding": []string{"gzip"},
			}, []byte("AB")), nil
		}
		client := &Client{HTTPClient: &stubClient{fn: fn}, UserAgent: DefaultUserAgent}
		dst := newSparseWriter()
		_, err := client.FetchRanges(context.Background(), newTemplate(t), dst,
			[]ByteRange{{0, 2}}, RangeOptions{BatchSize: 1, TempDir: tmp})
		if err == nil || !strings.Contains(err.Error(), "Content-Encoding") {
			t.Fatalf("err = %v", err)
		}
		assertNoSpoolLeft(t, tmp)
	})
}

func mustAtoi(t *testing.T, s string) int64 {
	t.Helper()
	var n int64
	for _, c := range s {
		if c < '0' || c > '9' {
			t.Fatalf("not a number: %q", s)
		}
		n = n*10 + int64(c-'0')
	}
	return n
}
