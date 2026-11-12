package sse

import (
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"gopkg.in/cenkalti/backoff.v1"
)

// captureWait is only a deadlock guard. Scenario progress is driven by
// channels and barriers, never by sleeps.
const captureWait = 5 * time.Second

const (
	captureRecordOne   = "id: one\ndata: first\n\n"
	captureRecordTwo   = "id: two\ndata: second\n\n"
	captureRecordThree = "id: three\ndata: third\n\n"
)

func waitSignal(t *testing.T, ch <-chan struct{}, what string) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(captureWait):
		t.Fatalf("timed out waiting for %s", what)
	}
}

func waitError(t *testing.T, ch <-chan error, what string) error {
	t.Helper()
	select {
	case err := <-ch:
		return err
	case <-time.After(captureWait):
		t.Fatalf("timed out waiting for %s", what)
		return nil
	}
}

func awaitCaptureEvent(t *testing.T, ch <-chan *Event, what string) *Event {
	t.Helper()
	select {
	case ev := <-ch:
		return ev
	case <-time.After(captureWait):
		t.Fatalf("timed out waiting for %s", what)
		return nil
	}
}

func deliveredID(c *Client) string {
	v, ok := c.LastEventID.Load().([]byte)
	if !ok {
		return ""
	}
	return string(v)
}

type capturedRequest struct {
	index      int
	lastID     string
	stream     string
	site       string
	accept     string
	userHeader string
}

// captureGateway is a finite-response gateway. It serves the configured body
// for request N and an empty body for every later request. Each response ends
// normally, which is exactly the gateway behavior the client must resume from.
type captureGateway struct {
	mu       sync.Mutex
	bodies   []string
	requests []capturedRequest
	wake     chan struct{}
}

func newCaptureGateway(bodies ...string) *captureGateway {
	return &captureGateway{
		bodies: bodies,
		wake:   make(chan struct{}, 1),
	}
}

// newCaptureServer returns a local httptest gateway. Go's in-memory
// httptest network avoids a real port bind while still running a normal
// httptest.Server and its handler for every request.
func newCaptureServer(t *testing.T, handler http.Handler) (*httptest.Server, *http.Client) {
	t.Helper()
	srv := httptest.NewTestServer(t, handler)
	return srv, srv.Client()
}

func (g *captureGateway) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	g.mu.Lock()
	idx := len(g.requests)
	rec := capturedRequest{
		index:      idx,
		lastID:     r.Header.Get("Last-Event-ID"),
		stream:     r.URL.Query().Get("stream"),
		site:       r.URL.Query().Get("site"),
		accept:     r.Header.Get("Accept"),
		userHeader: r.Header.Get("X-Client"),
	}
	g.requests = append(g.requests, rec)
	body := ""
	if idx < len(g.bodies) {
		body = g.bodies[idx]
	}
	g.mu.Unlock()

	select {
	case g.wake <- struct{}{}:
	default:
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.WriteHeader(http.StatusOK)
	if body != "" {
		_, _ = io.WriteString(w, body)
	}
}

func (g *captureGateway) count() int {
	g.mu.Lock()
	defer g.mu.Unlock()
	return len(g.requests)
}

func (g *captureGateway) snapshot() []capturedRequest {
	g.mu.Lock()
	defer g.mu.Unlock()
	out := make([]capturedRequest, len(g.requests))
	copy(out, g.requests)
	return out
}

func (g *captureGateway) waitCount(t *testing.T, n int) {
	t.Helper()
	deadline := time.NewTimer(captureWait)
	defer deadline.Stop()
	for g.count() < n {
		select {
		case <-g.wake:
		case <-deadline.C:
			t.Fatalf("timed out waiting for %d requests, saw %d", n, g.count())
		}
	}
}

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) {
	return f(r)
}

func okResponse(req *http.Request, body io.ReadCloser) *http.Response {
	return &http.Response{
		StatusCode: http.StatusOK,
		Status:     "200 OK",
		Header:     make(http.Header),
		Body:       body,
		Request:    req,
	}
}

type readerNopCloser struct {
	io.Reader
}

func (readerNopCloser) Close() error { return nil }

type countingCloser struct {
	reader  io.Reader
	onClose func()
	once    sync.Once
}

func (b *countingCloser) Read(p []byte) (int, error) {
	return b.reader.Read(p)
}

func (b *countingCloser) Close() error {
	b.once.Do(func() {
		if b.onClose != nil {
			b.onClose()
		}
	})
	return nil
}

func TestCaptureSessionResume(t *testing.T) {
	t.Run("callback_entry_resumes_after_eof_and_exhausts_strategy", func(t *testing.T) {
		g := newCaptureGateway(captureRecordOne, captureRecordTwo)
		srv, httpClient := newCaptureServer(t, g)

		c := NewClient(srv.URL + "?site=east")
		c.Connection = httpClient
		c.Headers["X-Client"] = "resume"
		c.ReconnectStrategy = backoff.WithMaxTries(&backoff.ZeroBackOff{}, 5)

		ctx, cancel := context.WithTimeout(context.Background(), captureWait)
		defer cancel()

		var data []string
		err := c.SubscribeWithContext(ctx, "weather", func(e *Event) {
			data = append(data, string(e.Data))
		})

		if !errors.Is(err, io.EOF) {
			t.Fatalf("callback entry should end with io.EOF after the strategy is exhausted, got %v", err)
		}
		if len(data) != 2 || data[0] != "first" || data[1] != "second" {
			t.Fatalf("unexpected delivery order: %v", data)
		}

		reqs := g.snapshot()
		// WithMaxTries(5) allows six connection attempts: two with records
		// and four empty finite responses.
		if len(reqs) != 6 {
			t.Fatalf("expected 6 requests (two records + four empty responses), got %d", len(reqs))
		}
		for i, req := range reqs {
			if req.stream != "weather" || req.site != "east" {
				t.Fatalf("request %d lost query parameters: %+v", i, req)
			}
			if req.accept != "text/event-stream" {
				t.Fatalf("request %d lost Accept header: %q", i, req.accept)
			}
			if req.userHeader != "resume" {
				t.Fatalf("request %d lost user header: %q", i, req.userHeader)
			}
		}
		wantIDs := []string{"", "one", "two", "two", "two", "two"}
		for i, want := range wantIDs {
			if reqs[i].lastID != want {
				t.Fatalf("request %d Last-Event-ID = %q, want %q", i, reqs[i].lastID, want)
			}
		}
	})

	t.Run("channel_entry_resumes_after_empty_eof", func(t *testing.T) {
		g := newCaptureGateway(captureRecordOne, captureRecordTwo)
		srv, httpClient := newCaptureServer(t, g)

		c := NewClient(srv.URL + "?site=east")
		c.Connection = httpClient
		c.ReconnectStrategy = backoff.WithMaxTries(&backoff.ZeroBackOff{}, 20)
		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()

		ch := make(chan *Event)
		if err := c.SubscribeChanWithContext(ctx, "weather", ch); err != nil {
			t.Fatalf("subscribe failed: %v", err)
		}

		first := awaitCaptureEvent(t, ch, "first record")
		second := awaitCaptureEvent(t, ch, "second record")
		if string(first.ID) != "one" || string(first.Data) != "first" {
			t.Fatalf("unexpected first record: id=%q data=%q", first.ID, first.Data)
		}
		if string(second.ID) != "two" || string(second.Data) != "second" {
			t.Fatalf("unexpected second record: id=%q data=%q", second.ID, second.Data)
		}

		// The third request is the empty response that must trigger another
		// reconnect rather than silently ending the subscription.
		g.waitCount(t, 3)
		reqs := g.snapshot()
		if reqs[1].lastID != "one" || reqs[2].lastID != "two" {
			t.Fatalf("resume headers not based on delivered records: %+v", reqs[:3])
		}
		if deliveredID(c) != "two" {
			t.Fatalf("cursor after delivered records = %q, want two", deliveredID(c))
		}

		c.Unsubscribe(ch)
	})
}

func TestCaptureSessionDelivery(t *testing.T) {
	t.Run("cursor_follows_delivery_not_reads_and_callback_id_copy", func(t *testing.T) {
		pr, pw := io.Pipe()

		var reqMu sync.Mutex
		var headers []string
		var reqCount int32
		reqWake := make(chan struct{}, 1)
		transport := roundTripFunc(func(req *http.Request) (*http.Response, error) {
			n := atomic.AddInt32(&reqCount, 1)
			reqMu.Lock()
			headers = append(headers, req.Header.Get("Last-Event-ID"))
			reqMu.Unlock()
			select {
			case reqWake <- struct{}{}:
			default:
			}
			if n == 1 {
				return okResponse(req, pr), nil
			}
			// Keep later connections open so the subscription does not spin.
			closed := make(chan struct{})
			blocked := &countingCloser{
				reader:  &stuckReader{closed: closed},
				onClose: func() { close(closed) },
			}
			return okResponse(req, blocked), nil
		})

		c := NewClient("http://gateway.test/")
		c.Connection = &http.Client{Transport: transport}
		c.ReconnectStrategy = &backoff.ZeroBackOff{}

		firstStarted := make(chan struct{})
		releaseFirst := make(chan struct{})
		secondStarted := make(chan struct{})
		releaseSecond := make(chan struct{})
		thirdStarted := make(chan struct{})
		releaseThird := make(chan struct{})

		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()
		done := make(chan error, 1)
		go func() {
			done <- c.SubscribeWithContext(ctx, "", func(e *Event) {
				switch string(e.ID) {
				case "one":
					close(firstStarted)
					<-releaseFirst
				case "two":
					close(secondStarted)
					<-releaseSecond
				case "three":
					close(thirdStarted)
					<-releaseThird
					// Mutating the delivered ID must not corrupt the cursor.
					e.ID = []byte("mutated")
				}
			})
		}()

		// First record is read and its handler is blocked.
		if _, err := pw.Write([]byte(captureRecordOne)); err != nil {
			t.Fatalf("writing first record: %v", err)
		}
		waitSignal(t, firstStarted, "first handler")
		if got := deliveredID(c); got != "" {
			t.Fatalf("cursor advanced before first delivery: %q", got)
		}

		// Second record is read while the first has not been delivered.
		if _, err := pw.Write([]byte(captureRecordTwo)); err != nil {
			t.Fatalf("writing second record: %v", err)
		}
		if got := deliveredID(c); got != "" {
			t.Fatalf("cursor advanced on a read that was not delivered: %q", got)
		}

		close(releaseFirst)
		waitSignal(t, secondStarted, "second handler")
		if got := deliveredID(c); got != "one" {
			t.Fatalf("cursor = %q while second record is undelivered, want one", got)
		}

		close(releaseSecond)
		if _, err := pw.Write([]byte(captureRecordThree)); err != nil {
			t.Fatalf("writing third record: %v", err)
		}
		waitSignal(t, thirdStarted, "third handler")
		if got := deliveredID(c); got != "two" {
			t.Fatalf("cursor = %q while third record is undelivered, want two", got)
		}

		close(releaseThird)
		if err := pw.Close(); err != nil {
			t.Fatalf("closing first response body: %v", err)
		}

		// The reconnect request must carry the delivered ID, not the value
		// the handler wrote into Event.ID.
		deadline := time.NewTimer(captureWait)
		defer deadline.Stop()
		for atomic.LoadInt32(&reqCount) < 2 {
			select {
			case <-reqWake:
			case <-deadline.C:
				t.Fatalf("timed out waiting for reconnect request")
			}
		}
		reqMu.Lock()
		gotHeader := headers[1]
		reqMu.Unlock()
		if gotHeader != "three" {
			t.Fatalf("reconnect Last-Event-ID = %q, want three (not the mutated ID)", gotHeader)
		}

		cancel()
		if err := waitError(t, done, "callback return after cancel"); !errors.Is(err, context.Canceled) {
			t.Fatalf("callback entry returned %v, want context.Canceled", err)
		}
	})

	t.Run("undelivered_record_is_refetched_after_stop_and_resubscribe", func(t *testing.T) {
		pr, pw := io.Pipe()

		var reqMu sync.Mutex
		var headers []string
		var reqCount int32
		transport := roundTripFunc(func(req *http.Request) (*http.Response, error) {
			n := atomic.AddInt32(&reqCount, 1)
			hdr := req.Header.Get("Last-Event-ID")
			reqMu.Lock()
			headers = append(headers, hdr)
			reqMu.Unlock()

			if n == 1 {
				return okResponse(req, pr), nil
			}
			if hdr == "one" {
				return okResponse(req, readerNopCloser{strings.NewReader(captureRecordTwo)}), nil
			}
			return okResponse(req, readerNopCloser{strings.NewReader("")}), nil
		})

		c := NewClient("http://gateway.test/")
		c.Connection = &http.Client{Transport: transport}
		c.ReconnectStrategy = &backoff.ZeroBackOff{}

		ch := make(chan *Event)
		if err := c.SubscribeChanWithContext(context.Background(), "weather", ch); err != nil {
			t.Fatalf("first subscribe failed: %v", err)
		}

		if _, err := pw.Write([]byte(captureRecordOne)); err != nil {
			t.Fatalf("writing first record: %v", err)
		}
		one := awaitCaptureEvent(t, ch, "first delivered record")
		if string(one.ID) != "one" {
			t.Fatalf("unexpected first record ID: %q", one.ID)
		}

		// The second record is read but the send to the unbuffered caller
		// channel is blocked, so it must not advance the cursor.
		if _, err := pw.Write([]byte(captureRecordTwo)); err != nil {
			t.Fatalf("writing second record: %v", err)
		}
		if got := deliveredID(c); got != "one" {
			t.Fatalf("cursor = %q with second record undelivered, want one", got)
		}

		c.Unsubscribe(ch)
		if got := deliveredID(c); got != "one" {
			t.Fatalf("cursor advanced on a canceled, undelivered record: %q", got)
		}
		_ = pw.Close()

		// Resubscribe with the same Client and channel: the request must
		// resume from the last delivered record and refetch the missing one.
		if err := c.SubscribeChanWithContext(context.Background(), "weather", ch); err != nil {
			t.Fatalf("resubscribe failed: %v", err)
		}
		two := awaitCaptureEvent(t, ch, "refetched record")
		if string(two.ID) != "two" || string(two.Data) != "second" {
			t.Fatalf("unexpected refetched record: id=%q data=%q", two.ID, two.Data)
		}
		c.Unsubscribe(ch)

		reqMu.Lock()
		defer reqMu.Unlock()
		if len(headers) < 2 || headers[0] != "" || headers[1] != "one" {
			t.Fatalf("unexpected resume headers across resubscribe: %v", headers)
		}
	})
}

// stuckReader blocks until its body is closed.
type stuckReader struct {
	closed <-chan struct{}
}

func (r *stuckReader) Read([]byte) (int, error) {
	<-r.closed
	return 0, io.EOF
}

// stagedBody serves one record, signals when the record bytes have been read,
// and then holds the connection open until it is closed.
type stagedBody struct {
	record    []byte
	pos       int
	served    chan struct{}
	unblock   chan struct{}
	once      sync.Once
	closeOnce sync.Once
	onClose   func()
}

func (b *stagedBody) Read(p []byte) (int, error) {
	if b.pos < len(b.record) {
		n := copy(p, b.record[b.pos:])
		b.pos += n
		if b.pos == len(b.record) {
			b.once.Do(func() { close(b.served) })
		}
		return n, nil
	}
	<-b.unblock
	return 0, io.EOF
}

func (b *stagedBody) Close() error {
	b.closeOnce.Do(func() {
		if b.onClose != nil {
			b.onClose()
		}
		close(b.unblock)
	})
	return nil
}

func TestCaptureSessionStop(t *testing.T) {
	t.Run("stop_while_reading_response", func(t *testing.T) {
		var reqs, opens, closes int32
		reading := make(chan struct{})
		unblock := make(chan struct{})
		var readOnce sync.Once

		transport := roundTripFunc(func(req *http.Request) (*http.Response, error) {
			atomic.AddInt32(&reqs, 1)
			atomic.AddInt32(&opens, 1)
			body := &countingCloser{
				reader: &signalBlockingReader{
					entered: reading,
					unblock: unblock,
					once:    &readOnce,
				},
				onClose: func() {
					atomic.AddInt32(&closes, 1)
					close(unblock)
				},
			}
			return okResponse(req, body), nil
		})

		c := NewClient("http://gateway.test/")
		c.Connection = &http.Client{Transport: transport}
		c.ReconnectStrategy = &backoff.ZeroBackOff{}
		ch := make(chan *Event)

		if err := c.SubscribeChanWithContext(context.Background(), "", ch); err != nil {
			t.Fatalf("subscribe failed: %v", err)
		}
		waitSignal(t, reading, "blocked response read")

		stopConcurrently(t, c, ch)

		if got := atomic.LoadInt32(&reqs); got != 1 {
			t.Fatalf("requests after stop while reading = %d, want 1", got)
		}
		if got := atomic.LoadInt32(&opens); got != 1 {
			t.Fatalf("opened response bodies = %d, want 1", got)
		}
		if got := atomic.LoadInt32(&closes); got != 1 {
			t.Fatalf("closed response bodies = %d, want 1", got)
		}
		if got := deliveredID(c); got != "" {
			t.Fatalf("cursor advanced on a canceled read: %q", got)
		}

		// The same Client and channel can be used again after stopping.
		var reuseReqs int32
		reuseTransport := roundTripFunc(func(req *http.Request) (*http.Response, error) {
			if atomic.AddInt32(&reuseReqs, 1) == 1 {
				return okResponse(req, readerNopCloser{strings.NewReader(captureRecordOne)}), nil
			}
			unblock := make(chan struct{})
			body := &countingCloser{
				reader:  &signalBlockingReader{entered: make(chan struct{}), unblock: unblock, once: &sync.Once{}},
				onClose: func() { close(unblock) },
			}
			return okResponse(req, body), nil
		})
		c.Connection = &http.Client{Transport: reuseTransport}
		if err := c.SubscribeChanWithContext(context.Background(), "", ch); err != nil {
			t.Fatalf("same-channel resubscribe failed: %v", err)
		}
		ev := awaitCaptureEvent(t, ch, "event after same-channel reuse")
		if string(ev.ID) != "one" {
			t.Fatalf("unexpected event after reuse: %q", ev.ID)
		}
		c.Unsubscribe(ch)
	})

	t.Run("stop_while_send_is_blocked", func(t *testing.T) {
		var reqs, opens, closes int32
		served := make(chan struct{})
		unblock := make(chan struct{})
		transport := roundTripFunc(func(req *http.Request) (*http.Response, error) {
			atomic.AddInt32(&reqs, 1)
			atomic.AddInt32(&opens, 1)
			body := &stagedBody{
				record:  []byte(captureRecordOne),
				served:  served,
				unblock: unblock,
				onClose: func() { atomic.AddInt32(&closes, 1) },
			}
			return okResponse(req, body), nil
		})

		c := NewClient("http://gateway.test/")
		c.Connection = &http.Client{Transport: transport}
		c.ReconnectStrategy = &backoff.ZeroBackOff{}
		ch := make(chan *Event) // unbuffered, no receiver until after stop

		if err := c.SubscribeChanWithContext(context.Background(), "", ch); err != nil {
			t.Fatalf("subscribe failed: %v", err)
		}
		waitSignal(t, served, "record read into blocked send")

		stopConcurrently(t, c, ch)

		if got := atomic.LoadInt32(&reqs); got != 1 {
			t.Fatalf("requests after blocked-send stop = %d, want 1", got)
		}
		if got := atomic.LoadInt32(&opens); got != 1 || atomic.LoadInt32(&closes) != 1 {
			t.Fatalf("body opens/closes = %d/%d, want 1/1", opens, closes)
		}
		if got := deliveredID(c); got != "" {
			t.Fatalf("cursor advanced on an undelivered blocked send: %q", got)
		}
	})

	t.Run("stop_while_waiting_in_long_backoff", func(t *testing.T) {
		var reqs, opens, closes int32
		entered := make(chan struct{})
		strategy := &barrierBackoff{entered: entered, delay: time.Hour}

		transport := roundTripFunc(func(req *http.Request) (*http.Response, error) {
			atomic.AddInt32(&reqs, 1)
			atomic.AddInt32(&opens, 1)
			body := &countingCloser{
				reader:  strings.NewReader(""),
				onClose: func() { atomic.AddInt32(&closes, 1) },
			}
			return &http.Response{
				StatusCode: http.StatusInternalServerError,
				Status:     "500 Internal Server Error",
				Header:     make(http.Header),
				Body:       body,
				Request:    req,
			}, nil
		})

		c := NewClient("http://gateway.test/")
		c.Connection = &http.Client{Transport: transport}
		c.ReconnectStrategy = strategy
		ch := make(chan *Event)

		// This subscription never connects, so SubscribeChan keeps waiting
		// for the final outcome in a separate goroutine.
		done := make(chan error, 1)
		go func() {
			done <- c.SubscribeChanWithContext(context.Background(), "", ch)
		}()

		waitSignal(t, entered, "long backoff wait")
		stopConcurrently(t, c, ch)

		if err := waitError(t, done, "SubscribeChan return after backoff stop"); err != nil {
			t.Fatalf("stopped SubscribeChan returned %v, want nil", err)
		}
		if got := atomic.LoadInt32(&reqs); got != 1 {
			t.Fatalf("requests after backoff stop = %d, want 1", got)
		}
		if got := atomic.LoadInt32(&opens); got != 1 || atomic.LoadInt32(&closes) != 1 {
			t.Fatalf("body opens/closes = %d/%d, want 1/1", opens, closes)
		}
	})
}

func stopConcurrently(t *testing.T, c *Client, ch chan *Event) {
	t.Helper()
	var wg sync.WaitGroup
	for i := 0; i < 2; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			c.Unsubscribe(ch)
		}()
	}
	finished := make(chan struct{})
	go func() {
		wg.Wait()
		close(finished)
	}()
	waitSignal(t, finished, "concurrent Unsubscribe calls")
	// A repeated stop after the subscription already finished returns
	// immediately.
	c.Unsubscribe(ch)
}

type signalBlockingReader struct {
	entered chan struct{}
	unblock <-chan struct{}
	once    *sync.Once
}

func (r *signalBlockingReader) Read([]byte) (int, error) {
	r.once.Do(func() { close(r.entered) })
	<-r.unblock
	return 0, io.EOF
}

type barrierBackoff struct {
	entered chan struct{}
	once    sync.Once
	delay   time.Duration
}

func (b *barrierBackoff) NextBackOff() time.Duration {
	b.once.Do(func() { close(b.entered) })
	return b.delay
}

func (b *barrierBackoff) Reset() {}
