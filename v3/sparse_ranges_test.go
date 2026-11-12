package grab

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// recordedRangeRequest captures the headers of one request exactly as the
// HTTPClient received them.
type recordedRangeRequest struct {
	method         string
	rangeHeader    string
	ifMatch        string
	acceptEncoding string
	userAgent      string
	header         http.Header
}

// trackBody records whether a response body was read to EOF and closed.
type trackBody struct {
	io.Reader
	mu     sync.Mutex
	eof    bool
	closed bool
}

func (t *trackBody) Read(p []byte) (int, error) {
	n, err := t.Reader.Read(p)
	if err == io.EOF {
		t.mu.Lock()
		t.eof = true
		t.mu.Unlock()
	}
	return n, err
}

func (t *trackBody) Close() error {
	t.mu.Lock()
	t.closed = true
	t.mu.Unlock()
	return nil
}

func (t *trackBody) state() (eof, closed bool) {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.eof, t.closed
}

// scriptedHTTPClient is an in-memory HTTPClient that records the real
// request headers it receives and serves programmable responses.
type scriptedHTTPClient struct {
	mu      sync.Mutex
	records []recordedRangeRequest
	bodies  []*trackBody
	handle  func(call int, r *http.Request) (*http.Response, error)
}

func (s *scriptedHTTPClient) Do(r *http.Request) (*http.Response, error) {
	s.mu.Lock()
	s.records = append(s.records, recordedRangeRequest{
		method:         r.Method,
		rangeHeader:    r.Header.Get("Range"),
		ifMatch:        r.Header.Get("If-Match"),
		acceptEncoding: r.Header.Get("Accept-Encoding"),
		userAgent:      r.Header.Get("User-Agent"),
		header:         r.Header.Clone(),
	})
	call := len(s.records)
	h := s.handle
	s.mu.Unlock()

	resp, err := h(call, r)
	if err != nil || resp == nil {
		return resp, err
	}
	if resp.Body == nil {
		resp.Body = http.NoBody
	}
	tb := &trackBody{Reader: resp.Body}
	resp.Body = tb
	s.mu.Lock()
	s.bodies = append(s.bodies, tb)
	s.mu.Unlock()
	return resp, nil
}

func (s *scriptedHTTPClient) calls() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.records)
}

func (s *scriptedHTTPClient) record(i int) recordedRangeRequest {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.records[i]
}

func (s *scriptedHTTPClient) assertAllBodiesClosed(t *testing.T) {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	for i, b := range s.bodies {
		if _, closed := b.state(); !closed {
			t.Errorf("response body %d was not closed", i+1)
		}
	}
}

// testPart describes one section of a synthetic multipart/byteranges body.
// body may deliberately differ from the declared span to exercise truncation
// and trailing-garbage detection.
type testPart struct {
	start, end int64 // inclusive, as declared in Content-Range
	total      int64 // declared resource total
	body       []byte
}

func multipartResponse(boundary, etag string, parts []testPart) *http.Response {
	var buf bytes.Buffer
	for _, p := range parts {
		fmt.Fprintf(&buf, "--%s\r\nContent-Range: bytes %d-%d/%d\r\n\r\n",
			boundary, p.start, p.end, p.total)
		buf.Write(p.body)
		buf.WriteString("\r\n")
	}
	fmt.Fprintf(&buf, "--%s--\r\n", boundary)
	raw := buf.Bytes()
	h := http.Header{}
	h.Set("Content-Type", "multipart/byteranges; boundary="+boundary)
	if etag != "" {
		h.Set("ETag", etag)
	}
	return &http.Response{
		StatusCode:    http.StatusPartialContent,
		Header:        h,
		Body:          io.NopCloser(bytes.NewReader(raw)),
		ContentLength: int64(len(raw)),
	}
}

func singleRangeResponse(start, end, total int64, body []byte, etag string) *http.Response {
	h := http.Header{}
	h.Set("Content-Type", "application/octet-stream")
	h.Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", start, end, total))
	if etag != "" {
		h.Set("ETag", etag)
	}
	return &http.Response{
		StatusCode:    http.StatusPartialContent,
		Header:        h,
		Body:          io.NopCloser(bytes.NewReader(body)),
		ContentLength: int64(len(body)),
	}
}

func fullResponse(body []byte, etag string) *http.Response {
	h := http.Header{}
	h.Set("Content-Type", "application/octet-stream")
	if etag != "" {
		h.Set("ETag", etag)
	}
	return &http.Response{
		StatusCode:    http.StatusOK,
		Header:        h,
		Body:          io.NopCloser(bytes.NewReader(body)),
		ContentLength: int64(len(body)),
	}
}

func statusResponse(code int) *http.Response {
	return &http.Response{
		StatusCode: code,
		Header:     http.Header{},
		Body:       io.NopCloser(strings.NewReader("")),
	}
}

// sparseStore is a WriterAt whose logical offsets may be enormous while only
// written bytes consume memory.
type sparseStore struct {
	mu   sync.Mutex
	data map[int64]byte
}

func newSparseStore() *sparseStore {
	return &sparseStore{data: make(map[int64]byte)}
}

func (s *sparseStore) WriteAt(p []byte, off int64) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for i, b := range p {
		s.data[off+int64(i)] = b
	}
	return len(p), nil
}

func (s *sparseStore) fill(start, end int64, b byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for i := start; i < end; i++ {
		s.data[i] = b
	}
}

func (s *sparseStore) str(start, end int64) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]byte, end-start)
	for i := range out {
		out[i] = s.data[start+int64(i)]
	}
	return string(out)
}

func (s *sparseStore) len() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.data)
}

// shortWriterAt reports success while writing fewer bytes than requested.
type shortWriterAt struct{ inner *sparseStore }

func (w shortWriterAt) WriteAt(p []byte, off int64) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	return w.inner.WriteAt(p[:len(p)-1], off)
}

// errWriterAt fails every write with a fixed error.
type errWriterAt struct{ err error }

func (w errWriterAt) WriteAt(p []byte, off int64) (int, error) {
	return 0, w.err
}

func sparseTemplate(t *testing.T, url string) *http.Request {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("X-Custom", "keep")
	return req
}

func tempDirWithKeepFile(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "keep.txt"), []byte("keep"), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

// assertOnlyKeepFile verifies that a TempDir holds exactly the pre-existing
// keep.txt file: every staging artifact of the call must be gone and nothing
// pre-existing may be touched.
func assertOnlyKeepFile(t *testing.T, dir string) {
	t.Helper()
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 || entries[0].Name() != "keep.txt" {
		names := make([]string, 0, len(entries))
		for _, e := range entries {
			names = append(names, e.Name())
		}
		t.Fatalf("TempDir holds %v, want only keep.txt", names)
	}
}

func TestSparseRangeCapture(t *testing.T) {
	t.Run("merged and out-of-order parts publish at original offsets", func(t *testing.T) {
		sc := &scriptedHTTPClient{}
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			switch call {
			case 1:
				// Out-of-order sections: [4,4] arrives before [0,1].
				return multipartResponse("bb1", `"v1"`, []testPart{
					{start: 4, end: 4, total: 14, body: []byte("E")},
					{start: 0, end: 1, total: 14, body: []byte("AB")},
				}), nil
			case 2:
				// One section coalescing both requested ranges; the gap
				// bytes KL must not be written.
				return multipartResponse("bb2", `"v1"`, []testPart{
					{start: 8, end: 13, total: 14, body: []byte("IJKLMN")},
				}), nil
			}
			return nil, fmt.Errorf("unexpected request %d", call)
		}
		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		tempDir := tempDirWithKeepFile(t)
		template := sparseTemplate(t, "https://archive.example/data.bin")
		templateHeader := template.Header.Clone()

		input := []ByteRange{{12, 14}, {0, 2}, {4, 5}, {1, 2}, {8, 10}, {4, 5}}
		inputSnapshot := append([]ByteRange(nil), input...)

		dst := newSparseStore()
		dst.fill(0, 14, '.')

		res, err := client.FetchRanges(context.Background(), template, dst, input,
			RangeOptions{BatchSize: 2, TempDir: tempDir})
		if err != nil {
			t.Fatalf("FetchRanges: %v", err)
		}

		// The plan is normalized and the caller's slice is untouched.
		wantPlan := []ByteRange{{0, 2}, {4, 5}, {8, 10}, {12, 14}}
		if !reflect.DeepEqual(res.Ranges, wantPlan) {
			t.Errorf("Ranges = %v, want %v", res.Ranges, wantPlan)
		}
		if !reflect.DeepEqual(input, inputSnapshot) {
			t.Errorf("caller's slice was modified: %v", input)
		}
		// Size comes from Content-Range totals, not the larger outer
		// multipart Content-Length.
		if res.Size != 14 {
			t.Errorf("Size = %d, want 14", res.Size)
		}
		if res.ETag != `"v1"` {
			t.Errorf("ETag = %q, want %q", res.ETag, `"v1"`)
		}

		// Bytes land at their original offsets; holes are preserved.
		if got := dst.str(0, 14); got != "AB..E...IJ..MN" {
			t.Errorf("dst = %q, want %q", got, "AB..E...IJ..MN")
		}

		// Exactly two requests carried the normalized batches.
		if sc.calls() != 2 {
			t.Fatalf("calls = %d, want 2", sc.calls())
		}
		r1, r2 := sc.record(0), sc.record(1)
		if r1.rangeHeader != "bytes=0-1,4-4" {
			t.Errorf("request 1 Range = %q, want %q", r1.rangeHeader, "bytes=0-1,4-4")
		}
		if r2.rangeHeader != "bytes=8-9,12-13" {
			t.Errorf("request 2 Range = %q, want %q", r2.rangeHeader, "bytes=8-9,12-13")
		}
		if r1.ifMatch != "" {
			t.Errorf("request 1 If-Match = %q, want empty", r1.ifMatch)
		}
		if r2.ifMatch != `"v1"` {
			t.Errorf("request 2 If-Match = %q, want %q", r2.ifMatch, `"v1"`)
		}
		for i, rec := range []recordedRangeRequest{r1, r2} {
			if rec.method != http.MethodGet {
				t.Errorf("request %d method = %q, want GET", i+1, rec.method)
			}
			if rec.acceptEncoding != "identity" {
				t.Errorf("request %d Accept-Encoding = %q, want identity", i+1, rec.acceptEncoding)
			}
			if rec.userAgent != "test-agent" {
				t.Errorf("request %d User-Agent = %q, want test-agent", i+1, rec.userAgent)
			}
			if rec.header.Get("X-Custom") != "keep" {
				t.Errorf("request %d lost the template's X-Custom header", i+1)
			}
		}

		// Neither the template nor the Client was modified.
		if !reflect.DeepEqual(template.Header, templateHeader) {
			t.Errorf("template headers modified: %v", template.Header)
		}
		if template.Header.Get("Range") != "" || template.Header.Get("If-Match") != "" {
			t.Errorf("template gained conditional headers: %v", template.Header)
		}
		if client.UserAgent != "test-agent" {
			t.Errorf("client UserAgent modified: %q", client.UserAgent)
		}

		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("first 200 falls back to the full representation", func(t *testing.T) {
		sc := &scriptedHTTPClient{}
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			if call != 1 {
				return nil, fmt.Errorf("unexpected request %d", call)
			}
			return fullResponse([]byte("ABCDEFGHIJ"), `"full"`), nil
		}
		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		tempDir := tempDirWithKeepFile(t)

		dst := newSparseStore()
		dst.fill(0, 10, '.')

		// The plan spans two batches, but a 200 to the first request
		// supplies everything and no second request may be sent.
		res, err := client.FetchRanges(context.Background(),
			sparseTemplate(t, "https://archive.example/data.bin"), dst,
			[]ByteRange{{2, 4}, {6, 8}}, RangeOptions{BatchSize: 1, TempDir: tempDir})
		if err != nil {
			t.Fatalf("FetchRanges: %v", err)
		}
		if sc.calls() != 1 {
			t.Fatalf("calls = %d, want 1", sc.calls())
		}
		if res.Size != 10 {
			t.Errorf("Size = %d, want 10", res.Size)
		}
		if res.ETag != `"full"` {
			t.Errorf("ETag = %q, want %q", res.ETag, `"full"`)
		}
		if got := dst.str(0, 10); got != "..CD..GH.." {
			t.Errorf("dst = %q, want %q", got, "..CD..GH..")
		}
		// The full body must have been consumed to EOF to verify its length.
		if eof, _ := sc.bodies[0].state(); !eof {
			t.Errorf("full representation was not read to EOF")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("full representation with a lying Content-Length is rejected", func(t *testing.T) {
		sc := &scriptedHTTPClient{}
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			resp := fullResponse([]byte("ABCDEFGHIJ"), "")
			resp.ContentLength = 5
			return resp, nil
		}
		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 10, '.')

		_, err := client.FetchRanges(context.Background(),
			sparseTemplate(t, "https://archive.example/data.bin"), dst,
			[]ByteRange{{0, 2}}, RangeOptions{BatchSize: 1, TempDir: tempDir})
		if err == nil {
			t.Fatal("expected an error for a Content-Length mismatch")
		}
		if got := dst.str(0, 10); got != ".........." {
			t.Errorf("dst = %q, want unchanged %q", got, "..........")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("full representation shorter than the plan is rejected", func(t *testing.T) {
		sc := &scriptedHTTPClient{}
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			return fullResponse([]byte("ABCDEFGHIJ"), ""), nil
		}
		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 10, '.')

		_, err := client.FetchRanges(context.Background(),
			sparseTemplate(t, "https://archive.example/data.bin"), dst,
			[]ByteRange{{2, 4}, {20, 25}}, RangeOptions{BatchSize: 1, TempDir: tempDir})
		if err == nil {
			t.Fatal("expected an error for a plan exceeding the representation")
		}
		if got := dst.str(0, 10); got != ".........." {
			t.Errorf("dst = %q, want unchanged %q", got, "..........")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("empty plan sends no request and writes nothing", func(t *testing.T) {
		sc := &scriptedHTTPClient{}
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			return nil, fmt.Errorf("unexpected request %d", call)
		}
		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		dst := newSparseStore()

		res, err := client.FetchRanges(context.Background(),
			sparseTemplate(t, "https://archive.example/data.bin"), dst,
			nil, RangeOptions{BatchSize: 4, TempDir: t.TempDir()})
		if err != nil {
			t.Fatalf("FetchRanges: %v", err)
		}
		if res.Size != -1 {
			t.Errorf("Size = %d, want -1", res.Size)
		}
		if len(res.Ranges) != 0 {
			t.Errorf("Ranges = %v, want empty", res.Ranges)
		}
		if sc.calls() != 0 {
			t.Errorf("calls = %d, want 0", sc.calls())
		}
		if dst.len() != 0 {
			t.Errorf("dst received %d bytes, want 0", dst.len())
		}
	})

	t.Run("invalid plans and templates are rejected before any IO", func(t *testing.T) {
		good := []ByteRange{{0, 2}}
		cases := []struct {
			name   string
			mutate func(*http.Request)
			ranges []ByteRange
			batch  int
		}{
			{name: "negative start", ranges: []ByteRange{{-1, 2}}, batch: 1},
			{name: "empty range", ranges: []ByteRange{{2, 2}}, batch: 1},
			{name: "inverted range", ranges: []ByteRange{{5, 2}}, batch: 1},
			{name: "zero batch size", ranges: good, batch: 0},
			{name: "negative batch size", ranges: good, batch: -2},
			{name: "POST method", mutate: func(r *http.Request) { r.Method = http.MethodPost }, ranges: good, batch: 1},
			{name: "request body", mutate: func(r *http.Request) {
				r.Body = io.NopCloser(strings.NewReader("x"))
			}, ranges: good, batch: 1},
			{name: "preset Range", mutate: func(r *http.Request) {
				r.Header.Set("Range", "bytes=0-1")
			}, ranges: good, batch: 1},
			{name: "preset If-Range", mutate: func(r *http.Request) {
				r.Header.Set("If-Range", `"x"`)
			}, ranges: good, batch: 1},
			{name: "preset If-Match", mutate: func(r *http.Request) {
				r.Header.Set("If-Match", `"x"`)
			}, ranges: good, batch: 1},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				sc := &scriptedHTTPClient{}
				sc.handle = func(call int, r *http.Request) (*http.Response, error) {
					return nil, fmt.Errorf("unexpected request %d", call)
				}
				client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
				template := sparseTemplate(t, "https://archive.example/data.bin")
				if tc.mutate != nil {
					tc.mutate(template)
				}
				dst := newSparseStore()
				_, err := client.FetchRanges(context.Background(), template, dst,
					tc.ranges, RangeOptions{BatchSize: tc.batch, TempDir: t.TempDir()})
				if err == nil {
					t.Fatal("expected an error")
				}
				if sc.calls() != 0 {
					t.Errorf("calls = %d, want 0", sc.calls())
				}
				if dst.len() != 0 {
					t.Errorf("dst received %d bytes, want 0", dst.len())
				}
			})
		}
	})

	t.Run("huge logical offsets do not drive allocation", func(t *testing.T) {
		const base = int64(1) << 40 // 1 TiB
		sc := &scriptedHTTPClient{}
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			if call != 1 {
				return nil, fmt.Errorf("unexpected request %d", call)
			}
			return singleRangeResponse(base, base+4, base+100, []byte("HELLO"), ""), nil
		}
		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()

		res, err := client.FetchRanges(context.Background(),
			sparseTemplate(t, "https://archive.example/huge.bin"), dst,
			[]ByteRange{{base, base + 5}}, RangeOptions{BatchSize: 1, TempDir: tempDir})
		if err != nil {
			t.Fatalf("FetchRanges: %v", err)
		}
		if res.Size != base+100 {
			t.Errorf("Size = %d, want %d", res.Size, base+100)
		}
		if got := dst.str(base, base+5); got != "HELLO" {
			t.Errorf("dst[%d:%d] = %q, want %q", base, base+5, got, "HELLO")
		}
		if dst.len() != 5 {
			t.Errorf("dst holds %d bytes, want 5", dst.len())
		}
		if rec := sc.record(0); rec.rangeHeader != fmt.Sprintf("bytes=%d-%d", base, base+4) {
			t.Errorf("Range = %q, want bytes=%d-%d", rec.rangeHeader, base, base+4)
		}
		assertOnlyKeepFile(t, tempDir)
	})

	t.Run("short write to the destination is reported", func(t *testing.T) {
		sc := &scriptedHTTPClient{}
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			return singleRangeResponse(0, 1, 10, []byte("AB"), ""), nil
		}
		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		tempDir := tempDirWithKeepFile(t)

		inner := newSparseStore()
		_, err := client.FetchRanges(context.Background(),
			sparseTemplate(t, "https://archive.example/data.bin"),
			shortWriterAt{inner}, []ByteRange{{0, 2}},
			RangeOptions{BatchSize: 1, TempDir: tempDir})
		if !errors.Is(err, io.ErrShortWrite) {
			t.Fatalf("err = %v, want io.ErrShortWrite", err)
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("destination write error is reported", func(t *testing.T) {
		boom := errors.New("disk on fire")
		sc := &scriptedHTTPClient{}
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			return singleRangeResponse(0, 1, 10, []byte("AB"), ""), nil
		}
		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		tempDir := tempDirWithKeepFile(t)

		_, err := client.FetchRanges(context.Background(),
			sparseTemplate(t, "https://archive.example/data.bin"),
			errWriterAt{boom}, []ByteRange{{0, 2}},
			RangeOptions{BatchSize: 1, TempDir: tempDir})
		if !errors.Is(err, boom) {
			t.Fatalf("err = %v, want %v", err, boom)
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("truncated and overlong parts are rejected without publishing", func(t *testing.T) {
		cases := []struct {
			name   string
			parts  []testPart
			single *testPart
		}{
			{name: "multipart part truncated", parts: []testPart{
				{start: 0, end: 1, total: 10, body: []byte("A")}, // declares 2, carries 1
			}},
			{name: "multipart part overlong", parts: []testPart{
				{start: 0, end: 0, total: 10, body: []byte("AB")}, // declares 1, carries 2
			}},
			{name: "single range truncated", single: &testPart{
				start: 0, end: 1, total: 10, body: []byte("A"),
			}},
			{name: "single range overlong", single: &testPart{
				start: 0, end: 0, total: 10, body: []byte("AB"),
			}},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				sc := &scriptedHTTPClient{}
				sc.handle = func(call int, r *http.Request) (*http.Response, error) {
					if tc.single != nil {
						p := tc.single
						resp := singleRangeResponse(p.start, p.end, p.total, p.body, "")
						// Lie about Content-Length so the body itself
						// decides what is actually delivered.
						resp.ContentLength = p.end - p.start + 1
						return resp, nil
					}
					return multipartResponse("bb", "", tc.parts), nil
				}
				client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
				tempDir := tempDirWithKeepFile(t)
				dst := newSparseStore()
				dst.fill(0, 10, '.')

				_, err := client.FetchRanges(context.Background(),
					sparseTemplate(t, "https://archive.example/data.bin"), dst,
					[]ByteRange{{0, 2}}, RangeOptions{BatchSize: 1, TempDir: tempDir})
				if err == nil {
					t.Fatal("expected an error")
				}
				if got := dst.str(0, 10); got != ".........." {
					t.Errorf("dst = %q, want unchanged %q", got, "..........")
				}
				assertOnlyKeepFile(t, tempDir)
				sc.assertAllBodiesClosed(t)
			})
		}
	})

	t.Run("cancellation during transfer leaves the destination untouched", func(t *testing.T) {
		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()
		sc := &scriptedHTTPClient{}
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			cancel() // cancel while the response body is still open
			resp := singleRangeResponse(0, 1, 10, nil, "")
			resp.Body = io.NopCloser(&cancelReader{ctx: ctx})
			resp.ContentLength = 2
			return resp, nil
		}
		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 10, '.')

		_, err := client.FetchRanges(ctx,
			sparseTemplate(t, "https://archive.example/data.bin"), dst,
			[]ByteRange{{0, 2}}, RangeOptions{BatchSize: 1, TempDir: tempDir})
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("err = %v, want context.Canceled", err)
		}
		if got := dst.str(0, 10); got != ".........." {
			t.Errorf("dst = %q, want unchanged %q", got, "..........")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("concurrent calls share no staging or progress", func(t *testing.T) {
		contents := map[string]string{
			"/a.bin": "0123456789ABCDEF",
			"/b.bin": "abcdefghijklmnop",
		}
		etags := map[string]string{
			"/a.bin": `"etag-a"`,
			"/b.bin": `"etag-b"`,
		}
		sc := &scriptedHTTPClient{}
		var arrived int32
		gate := make(chan struct{})
		var gateOnce sync.Once
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			var a, b int64
			if _, err := fmt.Sscanf(r.Header.Get("Range"), "bytes=%d-%d", &a, &b); err != nil {
				return nil, fmt.Errorf("bad Range header %q: %w", r.Header.Get("Range"), err)
			}
			// Hold both calls' first requests until both have arrived, so
			// their staging and batch state genuinely overlap in time.
			if atomic.AddInt32(&arrived, 1) >= 2 {
				gateOnce.Do(func() { close(gate) })
			}
			select {
			case <-gate:
			case <-time.After(10 * time.Second):
				return nil, errors.New("timed out waiting for the concurrent call")
			}
			body := contents[r.URL.Path]
			return singleRangeResponse(a, b, int64(len(body)), []byte(body[a:b+1]), etags[r.URL.Path]), nil
		}
		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		tempDir := tempDirWithKeepFile(t)

		templateA := sparseTemplate(t, "https://archive.example/a.bin")
		templateB := sparseTemplate(t, "https://archive.example/b.bin")
		dstA, dstB := newSparseStore(), newSparseStore()
		dstA.fill(0, 16, '.')
		dstB.fill(0, 16, '.')

		var wg sync.WaitGroup
		var resA, resB RangeResult
		var errA, errB error
		wg.Add(2)
		go func() {
			defer wg.Done()
			resA, errA = client.FetchRanges(context.Background(), templateA, dstA,
				[]ByteRange{{0, 2}, {4, 6}}, RangeOptions{BatchSize: 1, TempDir: tempDir})
		}()
		go func() {
			defer wg.Done()
			resB, errB = client.FetchRanges(context.Background(), templateB, dstB,
				[]ByteRange{{10, 12}, {14, 16}}, RangeOptions{BatchSize: 1, TempDir: tempDir})
		}()
		wg.Wait()

		if errA != nil || errB != nil {
			t.Fatalf("errA = %v, errB = %v", errA, errB)
		}
		if got := dstA.str(0, 16); got != "01..45.........." {
			t.Errorf("dstA = %q, want %q", got, "01..45..........")
		}
		if got := dstB.str(0, 16); got != "..........kl..op" {
			t.Errorf("dstB = %q, want %q", got, "..........kl..op")
		}
		if resA.ETag != `"etag-a"` || resB.ETag != `"etag-b"` {
			t.Errorf("ETags = %q, %q; want %q, %q", resA.ETag, resB.ETag, `"etag-a"`, `"etag-b"`)
		}
		if resA.Size != 16 || resB.Size != 16 {
			t.Errorf("Sizes = %d, %d; want 16, 16", resA.Size, resB.Size)
		}
		if sc.calls() != 4 {
			t.Errorf("calls = %d, want 4", sc.calls())
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})
}

// cancelReader blocks until its context is cancelled and then fails.
type cancelReader struct{ ctx context.Context }

func (c *cancelReader) Read(p []byte) (int, error) {
	<-c.ctx.Done()
	return 0, c.ctx.Err()
}

func TestSparseRangeConsistency(t *testing.T) {
	// twoBatchClient returns a scripted client serving the plan
	// [{0,2},{4,6}] in two batches from the 6-byte resource "ABCDEF",
	// pinned to the strong ETag "r1". mutate may alter the second
	// response.
	twoBatchClient := func(mutate func(call int, resp **http.Response)) *scriptedHTTPClient {
		sc := &scriptedHTTPClient{}
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			var resp *http.Response
			switch call {
			case 1:
				resp = singleRangeResponse(0, 1, 6, []byte("AB"), `"r1"`)
			case 2:
				resp = singleRangeResponse(4, 5, 6, []byte("EF"), `"r1"`)
			default:
				return nil, fmt.Errorf("unexpected request %d", call)
			}
			if mutate != nil {
				mutate(call, &resp)
			}
			return resp, nil
		}
		return sc
	}

	fetch := func(sc *scriptedHTTPClient, dst io.WriterAt, tempDir string) (RangeResult, error) {
		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		return client.FetchRanges(context.Background(),
			sparseTemplate(t, "https://archive.example/data.bin"), dst,
			[]ByteRange{{0, 2}, {4, 6}}, RangeOptions{BatchSize: 1, TempDir: tempDir})
	}

	t.Run("strong ETag pins all batches of one representation", func(t *testing.T) {
		sc := twoBatchClient(nil)
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 6, '.')

		res, err := fetch(sc, dst, tempDir)
		if err != nil {
			t.Fatalf("FetchRanges: %v", err)
		}
		if got := dst.str(0, 6); got != "AB..EF" {
			t.Errorf("dst = %q, want %q", got, "AB..EF")
		}
		if res.Size != 6 || res.ETag != `"r1"` {
			t.Errorf("Size, ETag = %d, %q; want 6, %q", res.Size, res.ETag, `"r1"`)
		}
		if sc.calls() != 2 {
			t.Fatalf("calls = %d, want 2", sc.calls())
		}
		if rec := sc.record(0); rec.ifMatch != "" {
			t.Errorf("request 1 If-Match = %q, want empty", rec.ifMatch)
		}
		if rec := sc.record(1); rec.ifMatch != `"r1"` {
			t.Errorf("request 2 If-Match = %q, want %q", rec.ifMatch, `"r1"`)
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("ETag change between batches aborts without publishing", func(t *testing.T) {
		sc := twoBatchClient(func(call int, resp **http.Response) {
			if call == 2 {
				(*resp).Header.Set("ETag", `"r2"`)
			}
		})
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 6, '.')

		_, err := fetch(sc, dst, tempDir)
		if !errors.Is(err, ErrRangeRepresentationChanged) {
			t.Fatalf("err = %v, want ErrRangeRepresentationChanged", err)
		}
		if got := dst.str(0, 6); got != "......" {
			t.Errorf("dst = %q, want unchanged %q", got, "......")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("ETag loss between batches aborts without publishing", func(t *testing.T) {
		sc := twoBatchClient(func(call int, resp **http.Response) {
			if call == 2 {
				(*resp).Header.Del("ETag")
			}
		})
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 6, '.')

		_, err := fetch(sc, dst, tempDir)
		if !errors.Is(err, ErrRangeRepresentationChanged) {
			t.Fatalf("err = %v, want ErrRangeRepresentationChanged", err)
		}
		if got := dst.str(0, 6); got != "......" {
			t.Errorf("dst = %q, want unchanged %q", got, "......")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("weak ETag cannot pin a multi-batch fetch", func(t *testing.T) {
		sc := twoBatchClient(func(call int, resp **http.Response) {
			if call == 1 {
				(*resp).Header.Set("ETag", `W/"r1"`)
			}
		})
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 6, '.')

		_, err := fetch(sc, dst, tempDir)
		if !errors.Is(err, ErrRangeRepresentationChanged) {
			t.Fatalf("err = %v, want ErrRangeRepresentationChanged", err)
		}
		if sc.calls() != 1 {
			t.Errorf("calls = %d, want 1: the second batch must never be sent", sc.calls())
		}
		if got := dst.str(0, 6); got != "......" {
			t.Errorf("dst = %q, want unchanged %q", got, "......")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("total size change between batches aborts without publishing", func(t *testing.T) {
		sc := twoBatchClient(func(call int, resp **http.Response) {
			if call == 2 {
				*resp = singleRangeResponse(4, 5, 7, []byte("EF"), `"r1"`)
			}
		})
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 6, '.')

		_, err := fetch(sc, dst, tempDir)
		if !errors.Is(err, ErrRangeRepresentationChanged) {
			t.Fatalf("err = %v, want ErrRangeRepresentationChanged", err)
		}
		if got := dst.str(0, 6); got != "......" {
			t.Errorf("dst = %q, want unchanged %q", got, "......")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("412 aborts without publishing", func(t *testing.T) {
		sc := twoBatchClient(func(call int, resp **http.Response) {
			if call == 2 {
				*resp = statusResponse(http.StatusPreconditionFailed)
			}
		})
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 6, '.')

		_, err := fetch(sc, dst, tempDir)
		if !errors.Is(err, ErrRangeRepresentationChanged) {
			t.Fatalf("err = %v, want ErrRangeRepresentationChanged", err)
		}
		if got := dst.str(0, 6); got != "......" {
			t.Errorf("dst = %q, want unchanged %q", got, "......")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("200 on a later batch is rejected", func(t *testing.T) {
		sc := twoBatchClient(func(call int, resp **http.Response) {
			if call == 2 {
				*resp = fullResponse([]byte("ABCDEF"), `"r1"`)
			}
		})
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 6, '.')

		_, err := fetch(sc, dst, tempDir)
		if err == nil {
			t.Fatal("expected an error")
		}
		if got := dst.str(0, 6); got != "......" {
			t.Errorf("dst = %q, want unchanged %q", got, "......")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("single batch does not require an ETag", func(t *testing.T) {
		sc := &scriptedHTTPClient{}
		sc.handle = func(call int, r *http.Request) (*http.Response, error) {
			if call != 1 {
				return nil, fmt.Errorf("unexpected request %d", call)
			}
			return singleRangeResponse(0, 1, 6, []byte("AB"), ""), nil
		}
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 6, '.')

		client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
		res, err := client.FetchRanges(context.Background(),
			sparseTemplate(t, "https://archive.example/data.bin"), dst,
			[]ByteRange{{0, 2}}, RangeOptions{BatchSize: 4, TempDir: tempDir})
		if err != nil {
			t.Fatalf("FetchRanges: %v", err)
		}
		if res.ETag != "" {
			t.Errorf("ETag = %q, want empty", res.ETag)
		}
		if res.Size != 6 {
			t.Errorf("Size = %d, want 6", res.Size)
		}
		if got := dst.str(0, 6); got != "AB...." {
			t.Errorf("dst = %q, want %q", got, "AB....")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})

	t.Run("malformed partial responses are rejected without publishing", func(t *testing.T) {
		cases := []struct {
			name  string
			parts []testPart
		}{
			{name: "overlapping parts", parts: []testPart{
				{start: 0, end: 1, total: 6, body: []byte("AB")},
				{start: 1, end: 2, total: 6, body: []byte("BC")},
			}},
			{name: "duplicate parts", parts: []testPart{
				{start: 0, end: 1, total: 6, body: []byte("AB")},
				{start: 0, end: 1, total: 6, body: []byte("AB")},
			}},
			{name: "part outside the requested batch", parts: []testPart{
				{start: 0, end: 1, total: 10, body: []byte("AB")},
				{start: 8, end: 9, total: 10, body: []byte("YZ")},
			}},
			{name: "part not intersecting the batch", parts: []testPart{
				{start: 0, end: 1, total: 6, body: []byte("AB")},
				{start: 2, end: 3, total: 6, body: []byte("CD")},
			}},
			{name: "missing coverage", parts: []testPart{
				{start: 0, end: 1, total: 6, body: []byte("AB")},
			}},
			{name: "inconsistent totals", parts: []testPart{
				{start: 0, end: 1, total: 6, body: []byte("AB")},
				{start: 4, end: 5, total: 7, body: []byte("EF")},
			}},
			{name: "range beyond resource", parts: []testPart{
				{start: 0, end: 1, total: 6, body: []byte("AB")},
				{start: 4, end: 5, total: 6, body: []byte("EF")},
			}},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				sc := &scriptedHTTPClient{}
				sc.handle = func(call int, r *http.Request) (*http.Response, error) {
					if call != 1 {
						return nil, fmt.Errorf("unexpected request %d", call)
					}
					return multipartResponse("bb", "", tc.parts), nil
				}
				tempDir := tempDirWithKeepFile(t)
				dst := newSparseStore()
				dst.fill(0, 6, '.')

				client := &Client{HTTPClient: sc, UserAgent: "test-agent"}
				ranges := []ByteRange{{0, 2}, {4, 6}}
				if tc.name == "range beyond resource" {
					ranges = []ByteRange{{0, 2}, {4, 8}}
				}
				_, err := client.FetchRanges(context.Background(),
					sparseTemplate(t, "https://archive.example/data.bin"), dst,
					ranges, RangeOptions{BatchSize: 2, TempDir: tempDir})
				if err == nil {
					t.Fatal("expected an error")
				}
				if got := dst.str(0, 6); got != "......" {
					t.Errorf("dst = %q, want unchanged %q", got, "......")
				}
				assertOnlyKeepFile(t, tempDir)
				sc.assertAllBodiesClosed(t)
			})
		}
	})

	t.Run("unknown content encoding is rejected", func(t *testing.T) {
		sc := twoBatchClient(func(call int, resp **http.Response) {
			if call == 1 {
				(*resp).Header.Set("Content-Encoding", "gzip")
			}
		})
		tempDir := tempDirWithKeepFile(t)
		dst := newSparseStore()
		dst.fill(0, 6, '.')

		_, err := fetch(sc, dst, tempDir)
		if err == nil {
			t.Fatal("expected an error")
		}
		if got := dst.str(0, 6); got != "......" {
			t.Errorf("dst = %q, want unchanged %q", got, "......")
		}
		assertOnlyKeepFile(t, tempDir)
		sc.assertAllBodiesClosed(t)
	})
}
