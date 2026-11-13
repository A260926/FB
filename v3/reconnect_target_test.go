package grab

import (
	"bytes"
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// scriptedResponse is one canned reply served by scriptedTransport.
type scriptedResponse struct {
	status int
	header http.Header
	// cl is the advertised Content-Length of the reply.
	cl int64
	// body, when non-nil, is served for the reply.
	body *scriptBody
	// err makes Do fail at the transport level.
	err error
	// block waits for the request context to finish and then returns its
	// error, emulating a reconnect request interrupted by Cancel.
	block bool
}

// scriptBody returns the given prefix and then err (or io.EOF at the end when
// err is nil), and records whether it was closed.
type scriptBody struct {
	data   []byte
	pos    int
	err    error
	closed atomic.Bool
}

func newScriptBody(data []byte, err error) *scriptBody {
	return &scriptBody{data: data, err: err}
}

func (b *scriptBody) Read(p []byte) (int, error) {
	if b.pos >= len(b.data) {
		if b.err != nil {
			return 0, b.err
		}
		return 0, io.EOF
	}
	n := copy(p, b.data[b.pos:])
	b.pos += n
	if b.pos >= len(b.data) && b.err != nil {
		return n, b.err
	}
	return n, nil
}

func (b *scriptBody) Close() error {
	b.closed.Store(true)
	return nil
}

// recordedRequest captures the parts of an outgoing request under test.
type recordedRequest struct {
	method         string
	url            string
	rangeHeader    string
	ifRange        string
	acceptEncoding string
}

// scriptedTransport is a socket-free HTTPClient serving canned replies in
// order, which lets the reconnect state machine run where local port binding
// is unavailable.
type scriptedTransport struct {
	mu       sync.Mutex
	t        *testing.T
	script   []scriptedResponse
	requests []recordedRequest
}

func (tr *scriptedTransport) Do(req *http.Request) (*http.Response, error) {
	tr.mu.Lock()
	tr.requests = append(tr.requests, recordedRequest{
		method:         req.Method,
		url:            req.URL.String(),
		rangeHeader:    req.Header.Get("Range"),
		ifRange:        req.Header.Get("If-Range"),
		acceptEncoding: req.Header.Get("Accept-Encoding"),
	})
	if len(tr.script) == 0 {
		tr.mu.Unlock()
		tr.t.Fatalf("scriptedTransport: unexpected %s request", req.Method)
	}
	next := tr.script[0]
	tr.script = tr.script[1:]
	tr.mu.Unlock()

	if next.block {
		<-req.Context().Done()
		return nil, req.Context().Err()
	}
	if next.err != nil {
		return nil, next.err
	}
	resp := &http.Response{
		StatusCode:    next.status,
		Status:        fmt.Sprintf("%d %s", next.status, http.StatusText(next.status)),
		Header:        next.header,
		Body:          next.body,
		ContentLength: next.cl,
		Request:       req,
		ProtoMajor:    1,
		ProtoMinor:    1,
	}
	return resp, nil
}

func (tr *scriptedTransport) calls() []recordedRequest {
	tr.mu.Lock()
	defer tr.mu.Unlock()
	out := make([]recordedRequest, len(tr.requests))
	copy(out, tr.requests)
	return out
}

func (tr *scriptedTransport) getCount() int {
	tr.mu.Lock()
	defer tr.mu.Unlock()
	n := 0
	for _, r := range tr.requests {
		if r.method == http.MethodGet {
			n++
		}
	}
	return n
}

// redirectTransport wraps a scriptedTransport and rewrites the request
// recorded on each served response to finalURL, emulating a redirect whose
// final address grab must reuse for the reconnect.
type redirectTransport struct {
	inner    *scriptedTransport
	finalURL string
}

func (tr *redirectTransport) Do(req *http.Request) (*http.Response, error) {
	resp, err := tr.inner.Do(req)
	if err != nil {
		return resp, err
	}
	final, err := url.Parse(tr.finalURL)
	if err != nil {
		return nil, err
	}
	finalReq := req.Clone(req.Context())
	finalReq.URL = final
	finalReq.Host = final.Host
	resp.Request = finalReq
	return resp, nil
}

// testNetError is a net.Error used to prove network-typed failures trigger a
// reconnect.
type testNetError struct{}

func (*testNetError) Error() string   { return "simulated network failure" }
func (*testNetError) Timeout() bool   { return false }
func (*testNetError) Temporary() bool { return true }

var _ net.Error = (*testNetError)(nil)

// script builders

func baseHeaders(etag string) http.Header {
	h := http.Header{}
	h.Set("ETag", etag)
	h.Set("Accept-Ranges", "bytes")
	return h
}

func scriptHEAD(total int64, etag string) scriptedResponse {
	h := baseHeaders(etag)
	return scriptedResponse{
		status: http.StatusOK,
		header: h,
		cl:     total,
		body:   newScriptBody(nil, nil),
	}
}

// scriptOK is a full 200 response. When deliver < len(content) the body breaks
// after deliver bytes with err.
func scriptOK(content []byte, etag string, deliver int64, err error) scriptedResponse {
	if deliver < 0 || deliver > int64(len(content)) {
		deliver = int64(len(content))
	}
	return scriptedResponse{
		status: http.StatusOK,
		header: baseHeaders(etag),
		cl:     int64(len(content)),
		body:   newScriptBody(content[:deliver], err),
	}
}

// scriptPartial is a 206 response for full[start:]. When deliver < the suffix
// length the body breaks after deliver suffix bytes with err.
func scriptPartial(full []byte, start int64, etag string, deliver int64, err error) scriptedResponse {
	suffix := full[start:]
	if deliver < 0 || deliver > int64(len(suffix)) {
		deliver = int64(len(suffix))
	}
	h := baseHeaders(etag)
	h.Set("Content-Range",
		fmt.Sprintf("bytes %d-%d/%d", start, len(full)-1, len(full)))
	return scriptedResponse{
		status: http.StatusPartialContent,
		header: h,
		cl:     int64(len(suffix)),
		body:   newScriptBody(suffix[:deliver], err),
	}
}

func newScriptedClient(t *testing.T, script ...scriptedResponse) (*Client, *scriptedTransport) {
	tr := &scriptedTransport{t: t, script: script}
	client := NewClient()
	client.HTTPClient = tr
	return client, tr
}

func reconnectRequest(t *testing.T, dir string, url string) *Request {
	t.Helper()
	req := mustNewRequest(filepath.Join(dir, "target.bin"), url)
	return req
}

// countHooks returns BeforeCopy/AfterCopy hooks counting their invocations.
func countHooks() (Hook, Hook, *atomic.Int32, *atomic.Int32) {
	var before, after atomic.Int32
	return func(*Response) error {
			before.Add(1)
			return nil
		}, func(*Response) error {
			after.Add(1)
			return nil
		}, &before, &after
}

// countingLimiter records every byte offered to it.
type countingLimiter struct {
	n   int
	err error
}

func (l *countingLimiter) WaitN(_ context.Context, n int) error {
	l.n += n
	return l.err
}

// progressReader hammers the progress methods until the response completes, so
// a race detector can catch data races around transfer swaps.
func progressReader(resp *Response, done chan struct{}) {
	for {
		select {
		case <-done:
			return
		default:
		}
		_ = resp.BytesComplete()
		_ = resp.Size()
		_ = resp.Progress()
		_ = resp.BytesPerSecond()
		_ = resp.ETA()
		if resp.IsComplete() {
			return
		}
	}
}

func sha256Sum(b []byte) []byte {
	sum := sha256.Sum256(b)
	return sum[:]
}

// TestReconnectContinuation covers mid-body interruptions that are healed with
// 206 Range responses, including a pre-existing local prefix, repeated breaks,
// the different eligible read errors and NoStore.
func TestReconnectContinuation(t *testing.T) {
	const etag = `"v1"`
	v1 := bytes.Repeat([]byte("A"), 30)

	t.Run("LocalPrefixThenSingle206", func(t *testing.T) {
		dir := t.TempDir()
		prefix := v1[:8]
		if err := os.WriteFile(filepath.Join(dir, "target.bin"), prefix, 0666); err != nil {
			t.Fatal(err)
		}
		client, tr := newScriptedClient(t,
			scriptHEAD(int64(len(v1)), etag),
			scriptPartial(v1, 8, etag, 6, io.ErrUnexpectedEOF), // writes 8..13
			scriptPartial(v1, 14, etag, -1, nil),               // 206 heals 14..29
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 2
		req.SetChecksum(sha256.New(), sha256Sum(v1), false)
		lim := &countingLimiter{}
		req.RateLimiter = lim
		before, after, beforeN, afterN := countHooks()
		req.BeforeCopy, req.AfterCopy = before, after

		progressDone := make(chan struct{})
		resp := client.Do(req)
		go progressReader(resp, progressDone)

		err := resp.Err()
		close(progressDone)
		if err != nil {
			t.Fatalf("expected reconnect success, got: %v", err)
		}

		data, err := os.ReadFile(resp.Filename)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(data, v1) {
			t.Fatalf("content mismatch: got %d bytes %q", len(data), data)
		}
		if resp.Size() != int64(len(v1)) || resp.BytesComplete() != int64(len(v1)) {
			t.Errorf("progress %d/%d", resp.BytesComplete(), resp.Size())
		}
		if resp.Progress() != 1 {
			t.Errorf("progress = %v, want 1", resp.Progress())
		}
		if !resp.DidResume {
			t.Error("expected DidResume to remain true")
		}
		if beforeN.Load() != 1 {
			t.Errorf("BeforeCopy called %d times, want 1", beforeN.Load())
		}
		if afterN.Load() != 1 {
			t.Errorf("AfterCopy called %d times, want 1", afterN.Load())
		}
		// Only the 22 bytes not present in the local prefix pass the limiter.
		if lim.n != len(v1)-len(prefix) {
			t.Errorf("limiter saw %d bytes, want %d", lim.n, len(v1)-len(prefix))
		}

		calls := tr.calls()
		if len(calls) != 3 || tr.getCount() != 2 {
			t.Fatalf("expected HEAD + 2 GETs, got: %+v", calls)
		}
		if calls[1].rangeHeader != "bytes=8-" {
			t.Errorf("initial GET range = %q, want bytes=8-", calls[1].rangeHeader)
		}
		if calls[2].rangeHeader != "bytes=14-" || calls[2].ifRange != etag {
			t.Errorf("reconnect GET headers wrong: %+v", calls[2])
		}
		if calls[2].acceptEncoding != "identity" {
			t.Errorf("reconnect Accept-Encoding = %q, want identity", calls[2].acceptEncoding)
		}
		// Internal headers must not leak into the caller's request object.
		if req.HTTPRequest.Header.Get("If-Range") != "" {
			t.Error("If-Range leaked into caller request headers")
		}
		if req.HTTPRequest.Header.Get("Accept-Encoding") != "" {
			t.Error("Accept-Encoding leaked into caller request headers")
		}
	})

	t.Run("TwoBroken206WithinBudget", func(t *testing.T) {
		dir := t.TempDir()
		client, tr := newScriptedClient(t,
			scriptOK(v1, etag, 10, io.ErrUnexpectedEOF),         // 0..9
			scriptPartial(v1, 10, etag, 5, io.ErrUnexpectedEOF), // 10..14
			scriptPartial(v1, 15, etag, -1, nil),                // 15..29
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 2

		resp := client.Do(req)
		if err := resp.Err(); err != nil {
			t.Fatalf("expected success after two reconnects, got: %v", err)
		}
		if tr.getCount() != 3 {
			t.Errorf("expected 3 GETs, got %d", tr.getCount())
		}
		data, err := os.ReadFile(resp.Filename)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(data, v1) {
			t.Errorf("content mismatch: %q", data)
		}
	})

	t.Run("NetErrorAndEarlyEOFTriggers", func(t *testing.T) {
		cases := []struct {
			name string
			err  error
		}{
			{"NetError", &testNetError{}},
			{"UnexpectedEOF", io.ErrUnexpectedEOF},
			{"EarlyEOF", io.EOF},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				dir := t.TempDir()
				client, tr := newScriptedClient(t,
					scriptOK(v1, etag, 7, tc.err),
					scriptPartial(v1, 7, etag, -1, nil),
				)
				req := reconnectRequest(t, dir, "http://example.test/file")
				req.MaxReconnects = 1

				resp := client.Do(req)
				if err := resp.Err(); err != nil {
					t.Fatalf("expected %s to reconnect, got: %v", tc.name, err)
				}
				if tr.getCount() != 2 {
					t.Errorf("expected 2 GETs, got %d", tr.getCount())
				}
			})
		}
	})

	t.Run("NoStoreContinuation", func(t *testing.T) {
		dir := t.TempDir()
		client, _ := newScriptedClient(t,
			scriptOK(v1, etag, 11, io.ErrUnexpectedEOF),
			scriptPartial(v1, 11, etag, -1, nil),
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.NoStore = true
		req.MaxReconnects = 1
		req.SetChecksum(sha256.New(), sha256Sum(v1), false)

		resp := client.Do(req)
		if err := resp.Err(); err != nil {
			t.Fatalf("expected NoStore reconnect success, got: %v", err)
		}
		got, err := resp.Bytes()
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(got, v1) {
			t.Errorf("NoStore bytes mismatch: %q", got)
		}
	})

	t.Run("ZeroValuePreservesLegacyBehavior", func(t *testing.T) {
		dir := t.TempDir()
		client, tr := newScriptedClient(t,
			scriptOK(v1, etag, 3, io.ErrUnexpectedEOF),
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		// MaxReconnects left at zero.

		resp := client.Do(req)
		if err := resp.Err(); !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("expected io.ErrUnexpectedEOF, got: %v", err)
		}
		if tr.getCount() != 1 {
			t.Errorf("expected exactly 1 GET, got %d", tr.getCount())
		}
		data, err := os.ReadFile(resp.Filename)
		if err != nil {
			t.Fatal(err)
		}
		if string(data) != "AAA" {
			t.Errorf("partial prefix not retained, got %q", data)
		}
		if resp.BytesComplete() != 3 || resp.Size() != int64(len(v1)) {
			t.Errorf("progress %d/%d", resp.BytesComplete(), resp.Size())
		}
	})

	t.Run("MissingMetadataDisablesReconnect", func(t *testing.T) {
		dir := t.TempDir()
		// Unknown length and no ETag: the interruption must end the transfer.
		client, tr := newScriptedClient(t,
			scriptedResponse{
				status: http.StatusOK,
				header: http.Header{},
				cl:     -1,
				body:   newScriptBody(v1[:9], io.ErrUnexpectedEOF),
			},
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 3

		resp := client.Do(req)
		if err := resp.Err(); !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("expected original error, got: %v", err)
		}
		if tr.getCount() != 1 {
			t.Errorf("expected no reconnect, got %d GETs", tr.getCount())
		}
	})

	t.Run("WeakETagDisablesReconnect", func(t *testing.T) {
		dir := t.TempDir()
		h := http.Header{}
		h.Set("ETag", `W/"weak"`)
		client, tr := newScriptedClient(t,
			scriptedResponse{
				status: http.StatusOK,
				header: h,
				cl:     int64(len(v1)),
				body:   newScriptBody(v1[:9], io.ErrUnexpectedEOF),
			},
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 3

		if err := client.Do(req).Err(); !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("expected original error, got: %v", err)
		}
		if tr.getCount() != 1 {
			t.Errorf("weak ETag must not enable reconnect, got %d GETs", tr.getCount())
		}
	})

	t.Run("NonIdentityEncodingDisablesReconnect", func(t *testing.T) {
		dir := t.TempDir()
		client, tr := newScriptedClient(t,
			scriptOK(v1, etag, 9, io.ErrUnexpectedEOF),
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 3
		req.HTTPRequest.Header.Set("Accept-Encoding", "gzip")

		if err := client.Do(req).Err(); !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("expected original error, got: %v", err)
		}
		if tr.getCount() != 1 {
			t.Errorf("non-identity encoding must not reconnect, got %d GETs", tr.getCount())
		}
	})

	t.Run("UsesFinalRedirectURL", func(t *testing.T) {
		dir := t.TempDir()
		origin := "http://origin.test/file"
		finalURL := "http://final.test/object"
		inner := &scriptedTransport{t: t, script: []scriptedResponse{
			scriptOK(v1, etag, 9, io.ErrUnexpectedEOF),
			scriptPartial(v1, 9, etag, -1, nil),
		}}
		wrap := &redirectTransport{inner: inner, finalURL: finalURL}
		client := NewClient()
		client.HTTPClient = wrap

		req := reconnectRequest(t, dir, origin)
		req.MaxReconnects = 1
		if err := client.Do(req).Err(); err != nil {
			t.Fatalf("expected success, got: %v", err)
		}
		calls := inner.calls()
		if len(calls) != 2 {
			t.Fatalf("expected 2 requests, got %d", len(calls))
		}
		if calls[1].url != finalURL {
			t.Errorf("reconnect URL = %q, want %q", calls[1].url, finalURL)
		}
	})
}

// TestReconnectRestart covers a 200 answer to a Range request, meaning the
// representation changed and the download must restart from zero.
func TestReconnectRestart(t *testing.T) {
	const etag1, etag2 = `"v1"`, `"v2"`
	v1 := bytes.Repeat([]byte("A"), 30)
	v2 := bytes.Repeat([]byte("B"), 24)

	t.Run("RestartFromZeroWithHooks", func(t *testing.T) {
		dir := t.TempDir()
		if err := os.WriteFile(filepath.Join(dir, "target.bin"), v1[:8], 0666); err != nil {
			t.Fatal(err)
		}
		client, tr := newScriptedClient(t,
			scriptHEAD(int64(len(v1)), etag1),
			scriptPartial(v1, 8, etag1, 4, io.ErrUnexpectedEOF), // 8..11 written
			scriptOK(v2, etag2, -1, nil),                        // 200: new representation
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 2
		req.SetChecksum(sha256.New(), sha256Sum(v2), false)
		lim := &countingLimiter{}
		req.RateLimiter = lim
		before, after, beforeN, afterN := countHooks()
		req.BeforeCopy, req.AfterCopy = before, after

		resp := client.Do(req)
		if err := resp.Err(); err != nil {
			t.Fatalf("expected restart success, got: %v", err)
		}

		data, err := os.ReadFile(resp.Filename)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(data, v2) {
			t.Fatalf("expected only new content, got %q", data)
		}
		if resp.Size() != int64(len(v2)) || resp.BytesComplete() != int64(len(v2)) {
			t.Errorf("progress %d/%d", resp.BytesComplete(), resp.Size())
		}
		if resp.DidResume {
			t.Error("expected DidResume reset after restart")
		}
		if beforeN.Load() != 2 {
			t.Errorf("BeforeCopy called %d times, want 2 (initial + restart)", beforeN.Load())
		}
		if afterN.Load() != 1 {
			t.Errorf("AfterCopy called %d times, want 1", afterN.Load())
		}
		// 4 bytes from the first body, then all 24 bytes of the new content.
		if lim.n != 4+len(v2) {
			t.Errorf("limiter saw %d bytes, want %d", lim.n, 4+len(v2))
		}
		calls := tr.calls()
		if calls[2].rangeHeader != "bytes=12-" || calls[2].ifRange != etag1 {
			t.Errorf("reconnect request wrong: %+v", calls[2])
		}
	})

	t.Run("NoStoreRestartDropsOldBytes", func(t *testing.T) {
		dir := t.TempDir()
		client, _ := newScriptedClient(t,
			scriptOK(v1, etag1, 12, io.ErrUnexpectedEOF),
			scriptOK(v2, etag2, -1, nil),
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.NoStore = true
		req.MaxReconnects = 1

		resp := client.Do(req)
		if err := resp.Err(); err != nil {
			t.Fatalf("expected NoStore restart success, got: %v", err)
		}
		got, err := resp.Bytes()
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(got, v2) {
			t.Errorf("NoStore buffer must hold only new content, got %q", got)
		}
		if resp.BytesComplete() != int64(len(v2)) {
			t.Errorf("BytesComplete = %d, want %d", resp.BytesComplete(), len(v2))
		}
	})

	t.Run("BeforeCopyRejectionKeepsOldPrefix", func(t *testing.T) {
		dir := t.TempDir()
		prefix := v1[:8]
		if err := os.WriteFile(filepath.Join(dir, "target.bin"), prefix, 0666); err != nil {
			t.Fatal(err)
		}
		hookErr := errors.New("restart rejected")
		client, tr := newScriptedClient(t,
			scriptHEAD(int64(len(v1)), etag1),
			scriptPartial(v1, 8, etag1, 4, io.ErrUnexpectedEOF),
			scriptOK(v2, etag2, -1, nil),
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 2
		var calls atomic.Int32
		req.BeforeCopy = func(*Response) error {
			if calls.Add(1) == 2 {
				return hookErr
			}
			return nil
		}

		resp := client.Do(req)
		if err := resp.Err(); !errors.Is(err, hookErr) {
			t.Fatalf("expected hook error, got: %v", err)
		}
		// No further request after the rejected restart.
		if tr.getCount() != 2 {
			t.Errorf("expected 2 GETs, got %d", tr.getCount())
		}
		data, err := os.ReadFile(resp.Filename)
		if err != nil {
			t.Fatal(err)
		}
		// The 8-byte local prefix plus the 4 bytes appended before the break.
		if !bytes.Equal(data, v1[:12]) {
			t.Errorf("old prefix not retained verbatim, got %q", data)
		}
	})

	t.Run("RestartMissingETagRejected", func(t *testing.T) {
		dir := t.TempDir()
		client, _ := newScriptedClient(t,
			scriptOK(v1, etag1, 9, io.ErrUnexpectedEOF),
			scriptedResponse{
				status: http.StatusOK,
				header: http.Header{},
				cl:     int64(len(v2)),
				body:   newScriptBody(v2, nil),
			},
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 1

		resp := client.Do(req)
		if err := resp.Err(); !errors.Is(err, ErrBadReconnect) {
			t.Fatalf("expected ErrBadReconnect, got: %v", err)
		}
		data, err := os.ReadFile(resp.Filename)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(data, v1[:9]) {
			t.Errorf("old prefix must survive a rejected restart, got %q", data)
		}
	})

	t.Run("RestartSizeMismatchRejected", func(t *testing.T) {
		dir := t.TempDir()
		client, _ := newScriptedClient(t,
			scriptOK(v1, etag1, 9, io.ErrUnexpectedEOF),
			scriptOK(v2, etag2, -1, nil),
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 1
		req.Size = int64(len(v1)) // v2 has a different length

		if err := client.Do(req).Err(); !errors.Is(err, ErrBadReconnect) {
			t.Fatalf("expected ErrBadReconnect, got: %v", err)
		}
	})

	t.Run("RestartIgnoresSuggestedFilename", func(t *testing.T) {
		dir := t.TempDir()
		restartReply := scriptOK(v2, etag2, -1, nil)
		restartReply.header.Set("Content-Disposition", `attachment;filename="other.bin"`)
		client, _ := newScriptedClient(t,
			scriptOK(v1, etag1, 9, io.ErrUnexpectedEOF),
			restartReply,
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 1

		resp := client.Do(req)
		if err := resp.Err(); err != nil {
			t.Fatalf("expected restart success, got: %v", err)
		}
		wantFilename := filepath.Join(dir, "target.bin")
		if resp.Filename != wantFilename {
			t.Errorf("Filename changed to %q, want %q", resp.Filename, wantFilename)
		}
		data, err := os.ReadFile(wantFilename)
		if err != nil {
			t.Fatalf("new content must land in the original file: %v", err)
		}
		if !bytes.Equal(data, v2) {
			t.Errorf("content mismatch: %q", data)
		}
		if _, err := os.Stat(filepath.Join(dir, "other.bin")); !os.IsNotExist(err) {
			t.Errorf("server-suggested filename must not open a second file, stat: %v", err)
		}
	})

	t.Run("TwoRestartsResetEachTime", func(t *testing.T) {
		dir := t.TempDir()
		const etag3 = `"v3"`
		v3 := bytes.Repeat([]byte("C"), 18)
		client, tr := newScriptedClient(t,
			scriptOK(v1, etag1, 6, io.ErrUnexpectedEOF), // 6 of v1
			scriptOK(v2, etag2, 5, io.ErrUnexpectedEOF), // 5 of v2 after first restart
			scriptOK(v3, etag3, -1, nil),                // full v3 after second restart
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 3
		req.SetChecksum(sha256.New(), sha256Sum(v3), false)
		var before, after atomic.Int32
		req.BeforeCopy = func(*Response) error { before.Add(1); return nil }
		req.AfterCopy = func(*Response) error { after.Add(1); return nil }

		resp := client.Do(req)
		if err := resp.Err(); err != nil {
			t.Fatalf("expected success across two restarts, got: %v", err)
		}
		data, err := os.ReadFile(resp.Filename)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(data, v3) {
			t.Fatalf("expected only final representation v3, got %q", data)
		}
		if resp.Size() != int64(len(v3)) || resp.BytesComplete() != int64(len(v3)) {
			t.Errorf("progress %d/%d", resp.BytesComplete(), resp.Size())
		}
		if before.Load() != 3 {
			t.Errorf("BeforeCopy called %d times, want 3", before.Load())
		}
		if after.Load() != 1 {
			t.Errorf("AfterCopy called %d times, want 1", after.Load())
		}
		if tr.getCount() != 3 {
			t.Errorf("expected 3 GETs, got %d", tr.getCount())
		}
	})
}

// TestReconnectFailures covers allowance exhaustion, error-source
// discrimination, cancellation, malformed reconnect responses and request
// validation.
func TestReconnectFailures(t *testing.T) {
	const etag = `"v1"`
	v1 := bytes.Repeat([]byte("A"), 30)

	t.Run("AllowanceExhaustedReturnsLastReadError", func(t *testing.T) {
		dir := t.TempDir()
		client, tr := newScriptedClient(t,
			scriptOK(v1, etag, 6, io.ErrUnexpectedEOF),
			scriptPartial(v1, 6, etag, 4, io.ErrUnexpectedEOF),
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 1
		var afterN atomic.Int32
		req.AfterCopy = func(*Response) error {
			afterN.Add(1)
			return nil
		}

		resp := client.Do(req)
		err := resp.Err()
		if !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("expected last read error, got: %v", err)
		}
		if tr.getCount() != 2 {
			t.Errorf("expected 2 GETs, got %d", tr.getCount())
		}
		if afterN.Load() != 0 {
			t.Error("AfterCopy must not run on failure")
		}
		if !resp.IsComplete() {
			t.Error("Done must be closed once the transfer ends")
		}
		data, ferr := os.ReadFile(resp.Filename)
		if ferr != nil {
			t.Fatal(ferr)
		}
		if len(data) != 10 {
			t.Errorf("expected 10-byte prefix retained, got %d", len(data))
		}
	})

	t.Run("WriterErrorSameTypeIsNotReconnected", func(t *testing.T) {
		// The writer returns the same sentinel a broken reader would, but the
		// transfer tags it as a write failure.
		resp := &Response{
			Request:        &Request{NoStore: true, MaxReconnects: 3},
			writer:         &failWriter{err: io.ErrUnexpectedEOF},
			Done:           make(chan struct{}),
			repKnown:       true,
			repTotal:       10,
			reconnectsLeft: 3,
			transfer: newTransfer(
				context.Background(), nil,
				&failWriter{err: io.ErrUnexpectedEOF},
				bytes.NewReader(v1), nil),
		}
		_, err := (&Client{}).copyFile(resp)
		if !errors.Is(err, errTransferWrite) {
			t.Errorf("expected write sentinel, got: %v", err)
		}
		if !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Errorf("underlying error should remain visible, got: %v", err)
		}
		if resp.reconnectsLeft != 3 {
			t.Errorf("write failure consumed %d reconnects, want 0", 3-resp.reconnectsLeft)
		}
	})

	t.Run("RateLimiterErrorSameTypeIsNotReconnected", func(t *testing.T) {
		dir := t.TempDir()
		client, tr := newScriptedClient(t,
			scriptOK(v1, etag, -1, nil),
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 3
		req.BufferSize = 4
		// A limiter failing with the same error type a dropped connection
		// produces must end the transfer, not trigger a reconnect.
		req.RateLimiter = &countingLimiter{err: io.ErrUnexpectedEOF}

		resp := client.Do(req)
		err := resp.Err()
		if !errors.Is(err, errTransferLimit) {
			t.Fatalf("expected limiter sentinel, got: %v", err)
		}
		if !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Errorf("underlying error should remain visible, got: %v", err)
		}
		if tr.getCount() != 1 {
			t.Errorf("limiter failure must not reconnect, got %d GETs", tr.getCount())
		}
	})

	t.Run("ReadErrorClassification", func(t *testing.T) {
		if !reconnectReadError(io.ErrUnexpectedEOF, true) {
			t.Error("ErrUnexpectedEOF should be reconnectable")
		}
		if !reconnectReadError(&testNetError{}, false) {
			t.Error("net.Error should be reconnectable")
		}
		if !reconnectReadError(io.EOF, true) {
			t.Error("early EOF with known length should be reconnectable")
		}
		if reconnectReadError(io.EOF, false) {
			t.Error("EOF with unknown length must not reconnect")
		}
		if reconnectReadError(context.Canceled, true) {
			t.Error("context cancellation must not reconnect")
		}
		if reconnectReadError(context.DeadlineExceeded, true) {
			t.Error("context deadline must not reconnect")
		}
		if reconnectReadError(
			fmt.Errorf("%w: %w", errTransferWrite, io.ErrUnexpectedEOF), true) {
			t.Error("tagged write error must not reconnect")
		}
		if reconnectReadError(
			fmt.Errorf("%w: %w", errTransferLimit, &testNetError{}), true) {
			t.Error("tagged limiter error must not reconnect")
		}
		if reconnectReadError(nil, true) {
			t.Error("nil error must not reconnect")
		}
	})

	t.Run("CancelInterruptsReconnectRequest", func(t *testing.T) {
		dir := t.TempDir()
		tr := &scriptedTransport{t: t}
		client := NewClient()
		client.HTTPClient = tr
		// First reply breaks immediately; the reconnect call blocks until
		// cancellation.
		tr.script = []scriptedResponse{
			scriptOK(v1, etag, 6, io.ErrUnexpectedEOF),
			{status: http.StatusOK, block: true},
		}
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 2

		resp := client.Do(req)
		// Let the first body deliver its bytes and enter the reconnect call,
		// which blocks, before cancelling.
		deadline := time.Now().Add(2 * time.Second)
		for resp.BytesComplete() < 6 && time.Now().Before(deadline) {
			time.Sleep(time.Millisecond)
		}
		if resp.BytesComplete() < 6 {
			t.Fatalf("expected 6 bytes before cancel, got %d", resp.BytesComplete())
		}
		go func() {
			if err := resp.Cancel(); err != nil &&
				!errors.Is(err, context.Canceled) {
				t.Errorf("unexpected cancel error: %v", err)
			}
		}()

		err := resp.Err()
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("expected context.Canceled, got: %v", err)
		}
		if tr.getCount() != 2 {
			t.Errorf("expected initial + reconnect GET, got %d", tr.getCount())
		}
		if !resp.IsComplete() {
			t.Error("Done must close after cancellation")
		}
		data, ferr := os.ReadFile(resp.Filename)
		if ferr != nil {
			t.Fatal(ferr)
		}
		if len(data) != 6 {
			t.Errorf("prefix should be retained after cancel, got %d bytes", len(data))
		}
	})

	t.Run("Bad206Responses", func(t *testing.T) {
		cases := []struct {
			name  string
			reply scriptedResponse
		}{
			{
				name: "WrongContentRange",
				reply: scriptedResponse{
					status: http.StatusPartialContent,
					header: http.Header{
						"ETag":          []string{etag},
						"Content-Range": []string{"bytes 10-29/30"},
					},
					cl:   20,
					body: newScriptBody(v1[10:], nil),
				},
			},
			{
				name: "WeakETag",
				reply: scriptedResponse{
					status: http.StatusPartialContent,
					header: http.Header{
						"ETag":          []string{`W/"v1"`},
						"Content-Range": []string{"bytes 9-29/30"},
					},
					cl:   21,
					body: newScriptBody(v1[9:], nil),
				},
			},
			{
				name: "ETagMismatch",
				reply: scriptedResponse{
					status: http.StatusPartialContent,
					header: http.Header{
						"ETag":          []string{`"other"`},
						"Content-Range": []string{"bytes 9-29/30"},
					},
					cl:   21,
					body: newScriptBody(v1[9:], nil),
				},
			},
			{
				name: "WrongContentLength",
				reply: scriptedResponse{
					status: http.StatusPartialContent,
					header: http.Header{
						"ETag":          []string{etag},
						"Content-Range": []string{"bytes 9-29/30"},
					},
					cl:   99,
					body: newScriptBody(v1[9:], nil),
				},
			},
			{
				name: "MissingContentRange",
				reply: scriptedResponse{
					status: http.StatusPartialContent,
					header: http.Header{
						"ETag": []string{etag},
					},
					cl:   21,
					body: newScriptBody(v1[9:], nil),
				},
			},
			{
				name: "Compressed",
				reply: scriptedResponse{
					status: http.StatusPartialContent,
					header: http.Header{
						"ETag":             []string{etag},
						"Content-Range":    []string{"bytes 9-29/30"},
						"Content-Encoding": []string{"gzip"},
					},
					cl:   21,
					body: newScriptBody(v1[9:], nil),
				},
			},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				dir := t.TempDir()
				client, tr := newScriptedClient(t,
					scriptOK(v1, etag, 9, io.ErrUnexpectedEOF),
					tc.reply,
				)
				req := reconnectRequest(t, dir, "http://example.test/file")
				req.MaxReconnects = 2
				// Bad reconnect checks must not be bypassable.
				req.IgnoreBadStatusCodes = true

				err := client.Do(req).Err()
				if !errors.Is(err, ErrBadReconnect) {
					t.Fatalf("expected ErrBadReconnect, got: %v", err)
				}
				if tr.getCount() != 2 {
					t.Errorf("expected no further retry, got %d GETs", tr.getCount())
				}
				if !tc.reply.body.closed.Load() {
					t.Error("rejected response body must be closed")
				}
			})
		}
	})

	t.Run("UnexpectedStatusNotBypassed", func(t *testing.T) {
		dir := t.TempDir()
		client, tr := newScriptedClient(t,
			scriptOK(v1, etag, 9, io.ErrUnexpectedEOF),
			scriptedResponse{
				status: http.StatusInternalServerError,
				header: http.Header{},
				cl:     0,
				body:   newScriptBody(nil, nil),
			},
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 2
		req.IgnoreBadStatusCodes = true

		if err := client.Do(req).Err(); !errors.Is(err, ErrBadReconnect) {
			t.Fatalf("expected ErrBadReconnect, got: %v", err)
		}
		if tr.getCount() != 2 {
			t.Errorf("expected no further retry, got %d GETs", tr.getCount())
		}
	})

	t.Run("NegativeMaxReconnectsFailsEarly", func(t *testing.T) {
		dir := t.TempDir()
		tr := &scriptedTransport{t: t}
		client := NewClient()
		client.HTTPClient = tr
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = -1

		resp := client.Do(req)
		if err := resp.Err(); err == nil {
			t.Fatal("expected validation error for negative MaxReconnects")
		}
		if len(tr.calls()) != 0 {
			t.Errorf("no request should be sent, got %d", len(tr.calls()))
		}
		if _, err := os.Stat(resp.Filename); !os.IsNotExist(err) {
			t.Errorf("no file should be created, stat error: %v", err)
		}
	})

	t.Run("RequestBodyDisablesReconnect", func(t *testing.T) {
		dir := t.TempDir()
		client, tr := newScriptedClient(t,
			scriptOK(v1, etag, 9, io.ErrUnexpectedEOF),
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 2
		// A request carrying a body is not eligible for transparent reconnect.
		req.HTTPRequest.Body = io.NopCloser(bytes.NewReader([]byte("payload")))
		req.HTTPRequest.GetBody = func() (io.ReadCloser, error) {
			return io.NopCloser(bytes.NewReader([]byte("payload"))), nil
		}

		if err := client.Do(req).Err(); !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("expected original read error, got: %v", err)
		}
		if tr.getCount() != 1 {
			t.Errorf("expected no reconnect, got %d GETs", tr.getCount())
		}
	})

	t.Run("ResponseBodiesAreClosed", func(t *testing.T) {
		dir := t.TempDir()
		first := scriptOK(v1, etag, 9, io.ErrUnexpectedEOF)
		second := scriptPartial(v1, 9, etag, -1, nil)
		client, _ := newScriptedClient(t, first, second)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 1
		if err := client.Do(req).Err(); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if !first.body.closed.Load() || !second.body.closed.Load() {
			t.Error("expected every response body to be closed")
		}
	})

	t.Run("ReconnectRequestTransportError", func(t *testing.T) {
		dir := t.TempDir()
		transportErr := &testNetError{}
		client, tr := newScriptedClient(t,
			scriptOK(v1, etag, 9, io.ErrUnexpectedEOF),
			scriptedResponse{err: transportErr},
		)
		req := reconnectRequest(t, dir, "http://example.test/file")
		req.MaxReconnects = 2

		err := client.Do(req).Err()
		var netErr net.Error
		if !errors.As(err, &netErr) {
			t.Fatalf("expected transport net.Error, got: %v", err)
		}
		if tr.getCount() != 2 {
			t.Errorf("expected initial + one reconnect GET, got %d", tr.getCount())
		}
		// The allowance was consumed even though the reconnect failed: no
		// background retries follow.
		data, ferr := os.ReadFile(filepath.Join(dir, "target.bin"))
		if ferr != nil {
			t.Fatal(ferr)
		}
		if len(data) != 9 {
			t.Errorf("written prefix should be retained, got %d bytes", len(data))
		}
	})
}

// failWriter always fails with its configured error.
type failWriter struct {
	err error
}

func (w *failWriter) Write([]byte) (int, error) { return 0, w.err }
