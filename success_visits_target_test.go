// Copyright 2018 Adam Tauber
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package colly

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"testing"

	"github.com/gocolly/colly/v2/storage"
)

// The tests below use a small local HTTP server served over an in-memory
// listener instead of a TCP socket. HTTP parsing, redirect resolution and
// cookie handling are all performed by net/http; only the transport is
// in-process, which keeps the request/callback interleaving deterministic.
const localTestURL = "http://successful.visits.test"

// pipeListener hands connections created by pipeDialer to an http.Server.
type pipeListener struct {
	conns chan net.Conn
	done  chan struct{}
	once  sync.Once
}

func newPipeListener() *pipeListener {
	return &pipeListener{conns: make(chan net.Conn), done: make(chan struct{})}
}

func (l *pipeListener) Accept() (net.Conn, error) {
	select {
	case c := <-l.conns:
		return c, nil
	case <-l.done:
		return nil, net.ErrClosed
	}
}

func (l *pipeListener) Close() error {
	l.once.Do(func() { close(l.done) })
	return nil
}

func (l *pipeListener) Addr() net.Addr { return pipeAddr{} }

type pipeAddr struct{}

func (pipeAddr) Network() string { return "pipe" }
func (pipeAddr) String() string  { return "pipe" }

type pipeDialer struct{ l *pipeListener }

func (d pipeDialer) dialContext(ctx context.Context, _, _ string) (net.Conn, error) {
	clientConn, serverConn := net.Pipe()
	select {
	case d.l.conns <- serverConn:
		return clientConn, nil
	case <-ctx.Done():
		clientConn.Close()
		serverConn.Close()
		return nil, ctx.Err()
	}
}

// localTestServer serves handler on an in-memory listener.
type localTestServer struct {
	listener  *pipeListener
	server    *http.Server
	transport *http.Transport
}

func newLocalTestServer(t *testing.T, handler http.Handler) *localTestServer {
	t.Helper()
	l := newPipeListener()
	srv := &http.Server{Handler: handler}
	go func() { _ = srv.Serve(l) }()
	transport := &http.Transport{
		DialContext:       pipeDialer{l: l}.dialContext,
		DisableKeepAlives: true,
	}
	ts := &localTestServer{listener: l, server: srv, transport: transport}
	t.Cleanup(func() {
		transport.CloseIdleConnections()
		_ = srv.Close()
		_ = l.Close()
	})
	return ts
}

// collector creates a collector wired to this server. The SuccessfulVisits
// option is enabled unless another collector kind is requested.
func (ts *localTestServer) collector(t *testing.T, options ...CollectorOption) *Collector {
	t.Helper()
	options = append([]CollectorOption{SuccessfulVisits(), IgnoreRobotsTxt()}, options...)
	return ts.newCollector(t, options...)
}

// defaultModeCollector creates a collector with Colly's default visit
// tracking behavior.
func (ts *localTestServer) defaultModeCollector(t *testing.T) *Collector {
	t.Helper()
	return ts.newCollector(t, IgnoreRobotsTxt())
}

func (ts *localTestServer) newCollector(t *testing.T, options ...CollectorOption) *Collector {
	t.Helper()
	c := NewCollector(options...)
	c.WithTransport(ts.transport)
	return c
}

// roundTripFunc answers HTTP requests without a server, used for transport
// failures.
type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) {
	return f(r)
}

// response writes a plain-text HTTP response.
func response(w http.ResponseWriter, status int, body string) {
	w.Header().Set("Content-Type", "text/plain")
	w.WriteHeader(status)
	io.WriteString(w, body)
}

// failOnceStorage writes through to an InMemoryStorage but fails the first
// Visited call to simulate a persistent storage that is temporarily down.
type failOnceStorage struct {
	storage.Storage
	fail atomic.Bool
}

func (s *failOnceStorage) Visited(requestID uint64) error {
	if s.fail.CompareAndSwap(true, false) {
		return errors.New("storage unavailable")
	}
	return s.Storage.Visited(requestID)
}

func isAlreadyVisited(err error) bool {
	var alreadyVisited *AlreadyVisitedError
	return errors.As(err, &alreadyVisited)
}

// ExampleSuccessfulVisits shows how to make a scraping job record only
// requests that were processed successfully, so transient failures do not
// block a later attempt.
func ExampleSuccessfulVisits() {
	c := NewCollector(SuccessfulVisits())

	// Failed requests leave no visited mark, so retrying with an ordinary
	// Visit is allowed and reaches the server again.
	c.OnError(func(r *Response, err error) {
		_ = c.Visit(r.Request.URL.String())
	})

	_ = c.Visit("https://example.com/")
}

// ExampleSuccessfulVisits_shared shows that a Collector and its Clone share
// in-flight identities: only one of them processes a URL at a time.
func ExampleSuccessfulVisits_shared() {
	c := NewCollector(SuccessfulVisits())
	clone := c.Clone() // same storage and in-flight reservations

	// While c is fetching a URL, the same submission through clone returns
	// AlreadyVisitedError instead of waiting or sharing the response.
	_ = c.Visit("https://example.com/")
	_ = clone.Visit("https://example.com/")
}

// TestSuccessfulVisitsLifecycle covers the core behaviour of the
// SuccessfulVisits option: failed requests can be retried with an ordinary
// Visit, identities become visible only after the callbacks have finished,
// Clone coordination, AllowURLRevisit and the unchanged default mode.
func TestSuccessfulVisitsLifecycle(t *testing.T) {
	t.Run("failed request can be retried with plain Visit", func(t *testing.T) {
		var hits atomic.Int32
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			hits.Add(1)
			response(w, http.StatusServiceUnavailable, "retry later")
		}))
		c := ts.collector(t)

		if err := c.Visit(localTestURL + "/fail"); err == nil {
			t.Fatal("Visit returned nil for a 503 response")
		}
		if visited, err := c.HasVisited(localTestURL + "/fail"); err != nil || visited {
			t.Fatalf("HasVisited after failure = %v, %v; want false, nil", visited, err)
		}

		err := c.Visit(localTestURL + "/fail")
		if isAlreadyVisited(err) {
			t.Fatalf("ordinary retry was rejected as already visited: %v", err)
		}
		if err == nil {
			t.Fatal("second Visit returned nil for a 503 response")
		}
		if got := hits.Load(); got != 2 {
			t.Fatalf("server hits = %d, want 2", got)
		}
	})

	t.Run("identity is committed only after OnScraped", func(t *testing.T) {
		var hits atomic.Int32
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			hits.Add(1)
			response(w, http.StatusOK, "ok")
		}))
		c := ts.collector(t)

		var visitedInResponse, visitedInScraped bool
		var retryInResponse, retryInScraped error
		c.OnResponse(func(r *Response) {
			visitedInResponse, _ = c.HasVisited(r.Request.URL.String())
			retryInResponse = r.Request.Retry()
		})
		c.OnScraped(func(r *Response) {
			visitedInScraped, _ = c.HasVisited(r.Request.URL.String())
			retryInScraped = r.Request.Retry()
		})

		if err := c.Visit(localTestURL + "/ok"); err != nil {
			t.Fatalf("Visit returned %v, want nil", err)
		}
		if visitedInResponse {
			t.Error("HasVisited inside OnResponse reported an in-flight request")
		}
		if visitedInScraped {
			t.Error("HasVisited inside OnScraped reported a request before it was committed")
		}
		if visited, err := c.HasVisited(localTestURL + "/ok"); err != nil || !visited {
			t.Fatalf("HasVisited after success = %v, %v; want true, nil", visited, err)
		}
		for name, err := range map[string]error{"OnResponse": retryInResponse, "OnScraped": retryInScraped} {
			if !isAlreadyVisited(err) {
				t.Errorf("Retry in %s returned %v, want AlreadyVisitedError", name, err)
			}
		}
		if err := c.Visit(localTestURL + "/ok"); !isAlreadyVisited(err) {
			t.Fatalf("revisit returned %v, want AlreadyVisitedError", err)
		}
		if got := hits.Load(); got != 1 {
			t.Fatalf("server hits = %d, want 1", got)
		}
	})

	t.Run("ParseHTTPErrorResponse commits accepted statuses", func(t *testing.T) {
		var hits atomic.Int32
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			hits.Add(1)
			response(w, http.StatusInternalServerError, "error page")
		}))
		c := ts.collector(t)
		c.ParseHTTPErrorResponse = true

		if err := c.Visit(localTestURL + "/error"); err != nil {
			t.Fatalf("Visit with ParseHTTPErrorResponse returned %v, want nil", err)
		}
		if visited, err := c.HasVisited(localTestURL + "/error"); err != nil || !visited {
			t.Fatalf("HasVisited for accepted HTTP error = %v, %v; want true, nil", visited, err)
		}

		// Without ParseHTTPErrorResponse the same status is a failure and
		// must not be recorded.
		c2 := ts.collector(t)
		if err := c2.Visit(localTestURL + "/error"); err == nil {
			t.Fatal("Visit returned nil for an unaccepted HTTP error")
		}
		if visited, err := c2.HasVisited(localTestURL + "/error"); err != nil || visited {
			t.Fatalf("HasVisited for unaccepted HTTP error = %v, %v; want false, nil", visited, err)
		}
		if got := hits.Load(); got != 2 {
			t.Fatalf("server hits = %d, want 2", got)
		}
	})

	t.Run("aborts and transport errors leave no record", func(t *testing.T) {
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			response(w, http.StatusOK, "ok")
		}))

		t.Run("abort in OnRequest", func(t *testing.T) {
			c := ts.collector(t)
			c.OnRequest(func(r *Request) { r.Abort() })
			if err := c.Visit(localTestURL + "/abort-request"); err != nil {
				t.Fatalf("Visit returned %v, want nil for an aborted request", err)
			}
			if visited, err := c.HasVisited(localTestURL + "/abort-request"); err != nil || visited {
				t.Fatalf("HasVisited after OnRequest abort = %v, %v; want false, nil", visited, err)
			}
			// The aborted request must not keep the identity reserved.
			c.OnRequest(func(*Request) {})
			if err := c.Visit(localTestURL + "/abort-request"); err != nil {
				t.Fatalf("retry after OnRequest abort returned %v, want nil", err)
			}
		})

		t.Run("abort in OnResponseHeaders", func(t *testing.T) {
			var abort atomic.Bool
			abort.Store(true)
			c := ts.collector(t)
			c.OnResponseHeaders(func(r *Response) {
				if abort.Load() {
					r.Request.Abort()
				}
			})
			if err := c.Visit(localTestURL + "/abort-headers"); err == nil {
				t.Fatal("Visit returned nil for a request aborted after headers")
			}
			if visited, err := c.HasVisited(localTestURL + "/abort-headers"); err != nil || visited {
				t.Fatalf("HasVisited after header abort = %v, %v; want false, nil", visited, err)
			}
			abort.Store(false)
			if err := c.Visit(localTestURL + "/abort-headers"); err != nil {
				t.Fatalf("retry after header abort returned %v, want nil", err)
			}
		})

		t.Run("parser error", func(t *testing.T) {
			var fail atomic.Bool
			fail.Store(true)
			ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "application/xml")
				if fail.Load() {
					io.WriteString(w, "<root><unclosed></root>")
					return
				}
				io.WriteString(w, "<root><item>ok</item></root>")
			}))
			c := ts.collector(t)
			c.OnXML("//item", func(*XMLElement) {})

			if err := c.Visit(localTestURL + "/parse"); err == nil {
				t.Fatal("Visit returned nil for a malformed XML response")
			}
			if visited, err := c.HasVisited(localTestURL + "/parse"); err != nil || visited {
				t.Fatalf("HasVisited after parser error = %v, %v; want false, nil", visited, err)
			}
			fail.Store(false)
			if err := c.Visit(localTestURL + "/parse"); err != nil {
				t.Fatalf("retry after parser error returned %v, want nil", err)
			}
			if visited, err := c.HasVisited(localTestURL + "/parse"); err != nil || !visited {
				t.Fatalf("HasVisited after successful parse = %v, %v; want true, nil", visited, err)
			}
		})

		t.Run("network error", func(t *testing.T) {
			failing := roundTripFunc(func(req *http.Request) (*http.Response, error) {
				return nil, errors.New("network down")
			})
			c := NewCollector(SuccessfulVisits(), IgnoreRobotsTxt())
			c.WithTransport(failing)
			if err := c.Visit(localTestURL + "/network"); err == nil {
				t.Fatal("Visit returned nil for a transport error")
			}
			if visited, err := c.HasVisited(localTestURL + "/network"); err != nil || visited {
				t.Fatalf("HasVisited after transport error = %v, %v; want false, nil", visited, err)
			}
			c.WithTransport(ts.transport)
			if err := c.Visit(localTestURL + "/network"); err != nil {
				t.Fatalf("retry after transport error returned %v, want nil", err)
			}
		})
	})

	t.Run("Clone competes for the same identity", func(t *testing.T) {
		started := make(chan struct{})
		release := make(chan struct{})
		var hits atomic.Int32
		var once sync.Once
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if hits.Add(1) == 1 {
				once.Do(func() { close(started) })
				<-release
			}
			response(w, http.StatusOK, "ok")
		}))
		c := ts.collector(t)
		clone := c.Clone()

		firstErr := make(chan error, 1)
		go func() { firstErr <- c.Visit(localTestURL + "/race") }()
		<-started

		if err := clone.Visit(localTestURL + "/race"); !isAlreadyVisited(err) {
			t.Fatalf("clone returned %v, want AlreadyVisitedError", err)
		}
		if got := hits.Load(); got != 1 {
			t.Fatalf("server hits while the first request is in flight = %d, want 1", got)
		}

		// An independent collector does not coordinate with the Clone group.
		independent := ts.collector(t)
		if err := independent.Visit(localTestURL + "/race"); err != nil {
			t.Fatalf("independent collector returned %v, want nil", err)
		}
		if got := hits.Load(); got != 2 {
			t.Fatalf("server hits after independent collector = %d, want 2", got)
		}

		close(release)
		if err := <-firstErr; err != nil {
			t.Fatalf("first Visit returned %v, want nil", err)
		}
		if visited, err := clone.HasVisited(localTestURL + "/race"); err != nil || !visited {
			t.Fatalf("clone HasVisited after success = %v, %v; want true, nil", visited, err)
		}
	})

	t.Run("AllowURLRevisit bypasses coordination", func(t *testing.T) {
		var hits atomic.Int32
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			hits.Add(1)
			response(w, http.StatusOK, "ok")
		}))
		c := ts.collector(t, AllowURLRevisit())

		for i := 0; i < 2; i++ {
			if err := c.Visit(localTestURL + "/revisit"); err != nil {
				t.Fatalf("Visit %d returned %v, want nil", i, err)
			}
		}
		if got := hits.Load(); got != 2 {
			t.Fatalf("server hits = %d, want 2", got)
		}
	})

	t.Run("default mode keeps eager recording", func(t *testing.T) {
		var hits atomic.Int32
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			hits.Add(1)
			response(w, http.StatusServiceUnavailable, "retry later")
		}))
		c := ts.defaultModeCollector(t)

		if err := c.Visit(localTestURL + "/fail"); err == nil {
			t.Fatal("Visit returned nil for a 503 response")
		}
		if visited, err := c.HasVisited(localTestURL + "/fail"); err != nil || !visited {
			t.Fatalf("default mode HasVisited after failure = %v, %v; want true, nil", visited, err)
		}
		if err := c.Visit(localTestURL + "/fail"); !isAlreadyVisited(err) {
			t.Fatalf("default mode retry returned %v, want AlreadyVisitedError", err)
		}
		if got := hits.Load(); got != 1 {
			t.Fatalf("default mode server hits = %d, want 1", got)
		}
	})

	t.Run("storage failure is reported and releases the reservation", func(t *testing.T) {
		var hits atomic.Int32
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			hits.Add(1)
			response(w, http.StatusOK, "ok")
		}))
		c := ts.collector(t)

		failing := &failOnceStorage{Storage: &storage.InMemoryStorage{}}
		failing.fail.Store(true)
		if err := c.SetStorage(failing); err != nil {
			t.Fatalf("SetStorage failed: %v", err)
		}

		if err := c.Visit(localTestURL + "/storage"); err == nil {
			t.Fatal("Visit returned nil, want the storage error")
		}
		if visited, err := c.HasVisited(localTestURL + "/storage"); err != nil || visited {
			t.Fatalf("HasVisited after storage failure = %v, %v; want false, nil", visited, err)
		}
		if err := c.Visit(localTestURL + "/storage"); err != nil {
			t.Fatalf("retry after storage failure returned %v, want nil", err)
		}
		if visited, err := c.HasVisited(localTestURL + "/storage"); err != nil || !visited {
			t.Fatalf("HasVisited after successful retry = %v, %v; want true, nil", visited, err)
		}
		if got := hits.Load(); got != 2 {
			t.Fatalf("server hits = %d, want 2", got)
		}
	})
}

// TestSuccessfulVisitsRedirect checks that a redirect chain is committed as
// one unit and that every URL on an in-flight chain is reserved.
func TestSuccessfulVisitsRedirect(t *testing.T) {
	t.Run("failed chain leaves no records and can be retried", func(t *testing.T) {
		var chainHits, targetHits atomic.Int32
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/chain":
				chainHits.Add(1)
				http.Redirect(w, r, "/target", http.StatusFound)
			case "/target":
				if targetHits.Add(1) == 1 {
					response(w, http.StatusServiceUnavailable, "retry later")
					return
				}
				response(w, http.StatusOK, "ok")
			default:
				response(w, http.StatusNotFound, "missing")
			}
		}))
		c := ts.collector(t)

		var redirects atomic.Int32
		c.SetRedirectHandler(func(req *http.Request, via []*http.Request) error {
			redirects.Add(1)
			return nil
		})

		if err := c.Visit(localTestURL + "/chain"); err == nil {
			t.Fatal("Visit returned nil for a chain ending in 503")
		}
		for _, u := range []string{"/chain", "/target"} {
			if visited, err := c.HasVisited(localTestURL + u); err != nil || visited {
				t.Fatalf("HasVisited(%s) after failed chain = %v, %v; want false, nil", u, visited, err)
			}
		}

		if err := c.Visit(localTestURL + "/chain"); err != nil {
			t.Fatalf("retry of failed chain returned %v, want nil", err)
		}
		for _, u := range []string{"/chain", "/target"} {
			if visited, err := c.HasVisited(localTestURL + u); err != nil || !visited {
				t.Fatalf("HasVisited(%s) after successful chain = %v, %v; want true, nil", u, visited, err)
			}
		}
		if got, want := chainHits.Load(), int32(2); got != want {
			t.Fatalf("chain hits = %d, want %d", got, want)
		}
		if got, want := targetHits.Load(), int32(2); got != want {
			t.Fatalf("target hits = %d, want %d", got, want)
		}
		if got, want := redirects.Load(), int32(2); got != want {
			t.Fatalf("redirect handler calls = %d, want %d", got, want)
		}

		if err := c.Visit(localTestURL + "/chain"); !isAlreadyVisited(err) {
			t.Fatalf("revisit of committed chain returned %v, want AlreadyVisitedError", err)
		}
	})

	t.Run("in-flight chain identities are reserved", func(t *testing.T) {
		started := make(chan struct{})
		release := make(chan struct{})
		var hits atomic.Int32
		var once sync.Once
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/slow":
				http.Redirect(w, r, "/slow-target", http.StatusFound)
			case "/slow-target":
				hits.Add(1)
				once.Do(func() { close(started) })
				<-release
				response(w, http.StatusOK, "ok")
			default:
				response(w, http.StatusNotFound, "missing")
			}
		}))

		c := ts.collector(t)
		clone := c.Clone()

		firstErr := make(chan error, 1)
		go func() { firstErr <- c.Visit(localTestURL + "/slow") }()
		<-started

		for _, u := range []string{"/slow", "/slow-target"} {
			if err := clone.Visit(localTestURL + u); !isAlreadyVisited(err) {
				t.Fatalf("clone Visit(%s) returned %v, want AlreadyVisitedError", u, err)
			}
		}
		if got := hits.Load(); got != 1 {
			t.Fatalf("target hits while chain is in flight = %d, want 1", got)
		}

		close(release)
		if err := <-firstErr; err != nil {
			t.Fatalf("chain Visit returned %v, want nil", err)
		}
		for _, u := range []string{"/slow", "/slow-target"} {
			if visited, err := c.HasVisited(localTestURL + u); err != nil || !visited {
				t.Fatalf("HasVisited(%s) after successful chain = %v, %v; want true, nil", u, visited, err)
			}
		}
	})

	t.Run("redirect cycle is followed once", func(t *testing.T) {
		const cookieName = "session_id"
		var hits atomic.Int32
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			// The page sets a cookie and redirects to itself, a redirect
			// cycle. The cycle exception must keep it from being rejected as
			// already visited.
			hits.Add(1)
			if _, err := r.Cookie(cookieName); err == http.ErrNoCookie {
				http.SetCookie(w, &http.Cookie{Name: cookieName, Value: "1"})
				http.Redirect(w, r, "/cycle", http.StatusFound)
				return
			}
			response(w, http.StatusOK, "ok")
		}))
		c := ts.collector(t)

		if err := c.Visit(localTestURL + "/cycle"); err != nil {
			t.Fatalf("Visit returned %v, want nil", err)
		}
		if got, want := hits.Load(), int32(2); got != want {
			t.Fatalf("server hits = %d, want %d", got, want)
		}
		if visited, err := c.HasVisited(localTestURL + "/cycle"); err != nil || !visited {
			t.Fatalf("HasVisited after redirect cycle = %v, %v; want true, nil", visited, err)
		}
	})

	t.Run("default mode records the whole chain eagerly", func(t *testing.T) {
		var entryHits, leafHits atomic.Int32
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/entry":
				entryHits.Add(1)
				http.Redirect(w, r, "/leaf", http.StatusFound)
			case "/leaf":
				leafHits.Add(1)
				response(w, http.StatusServiceUnavailable, "retry later")
			}
		}))
		c := ts.defaultModeCollector(t)

		if err := c.Visit(localTestURL + "/entry"); err == nil {
			t.Fatal("Visit returned nil for a chain ending in 503")
		}
		for _, u := range []string{"/entry", "/leaf"} {
			if visited, err := c.HasVisited(localTestURL + u); err != nil || !visited {
				t.Fatalf("default mode HasVisited(%s) = %v, %v; want true, nil", u, visited, err)
			}
		}
		if err := c.Visit(localTestURL + "/entry"); !isAlreadyVisited(err) {
			t.Fatalf("default mode retry returned %v, want AlreadyVisitedError", err)
		}
		if got, want := entryHits.Load(), int32(1); got != want {
			t.Fatalf("entry hits = %d, want %d", got, want)
		}
		if got, want := leafHits.Load(), int32(1); got != want {
			t.Fatalf("leaf hits = %d, want %d", got, want)
		}
	})
}

// TestSuccessfulVisitsReentry checks that a failed request releases its
// identities before OnError runs, so callbacks can retry immediately, while a
// retry that is still in flight keeps blocking further submissions.
func TestSuccessfulVisitsReentry(t *testing.T) {
	t.Run("synchronous Retry inside OnError", func(t *testing.T) {
		var attempts atomic.Int32
		retryReached := make(chan struct{})
		retryRelease := make(chan struct{})
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch attempts.Add(1) {
			case 1:
				response(w, http.StatusServiceUnavailable, "retry later")
			case 2:
				close(retryReached)
				<-retryRelease
				response(w, http.StatusOK, "ok")
			default:
				response(w, http.StatusOK, "unexpected extra request")
			}
		}))
		c := ts.collector(t)

		var reentered atomic.Bool
		var nestedErr error
		var visitedInError bool
		c.OnError(func(r *Response, err error) {
			if !reentered.CompareAndSwap(false, true) {
				return
			}
			visitedInError, _ = c.HasVisited(r.Request.URL.String())
			nestedErr = r.Request.Retry()
		})

		firstErr := make(chan error, 1)
		go func() { firstErr <- c.Visit(localTestURL + "/flaky") }()
		<-retryReached

		// The re-entrant request is still blocked in the server: another
		// submission of the same identity must not be allowed through.
		if err := c.Visit(localTestURL + "/flaky"); !isAlreadyVisited(err) {
			t.Fatalf("third submission returned %v, want AlreadyVisitedError", err)
		}
		if got := attempts.Load(); got != 2 {
			t.Fatalf("server attempts before release = %d, want 2", got)
		}

		close(retryRelease)
		if err := <-firstErr; err == nil {
			t.Fatal("outer Visit returned nil, want the original failure")
		}
		if visitedInError {
			t.Error("HasVisited inside OnError reported the failed request")
		}
		if nestedErr != nil {
			t.Fatalf("re-entrant Retry returned %v, want nil", nestedErr)
		}
		if visited, err := c.HasVisited(localTestURL + "/flaky"); err != nil || !visited {
			t.Fatalf("HasVisited after re-entrant retry = %v, %v; want true, nil", visited, err)
		}
		if got := attempts.Load(); got != 2 {
			t.Fatalf("server attempts = %d, want 2", got)
		}
	})

	t.Run("Async old request does not clear re-entrant reservation", func(t *testing.T) {
		var attempts atomic.Int32
		retryReached := make(chan struct{})
		retryRelease := make(chan struct{})
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch attempts.Add(1) {
			case 1:
				response(w, http.StatusServiceUnavailable, "retry later")
			case 2:
				close(retryReached)
				<-retryRelease
				response(w, http.StatusOK, "ok")
			default:
				response(w, http.StatusOK, "unexpected extra request")
			}
		}))
		c := ts.collector(t, Async(true))

		reentryErr := make(chan error, 1)
		var once sync.Once
		c.OnError(func(r *Response, err error) {
			once.Do(func() {
				// In Async mode this returns as soon as the request is
				// scheduled; the old fetch returns while it is still running.
				reentryErr <- c.Visit(localTestURL + "/flaky")
			})
		})

		if err := c.Visit(localTestURL + "/flaky"); err != nil {
			t.Fatalf("first Visit returned %v, want nil in Async mode", err)
		}
		<-retryReached

		if err := c.Visit(localTestURL + "/flaky"); !isAlreadyVisited(err) {
			t.Fatalf("third submission returned %v, want AlreadyVisitedError", err)
		}

		close(retryRelease)
		c.Wait()

		if err := <-reentryErr; err != nil {
			t.Fatalf("re-entrant Visit returned %v, want nil", err)
		}
		if visited, err := c.HasVisited(localTestURL + "/flaky"); err != nil || !visited {
			t.Fatalf("HasVisited after Wait = %v, %v; want true, nil", visited, err)
		}
		if got := attempts.Load(); got != 2 {
			t.Fatalf("server attempts = %d, want 2", got)
		}
	})

	t.Run("OnError re-entry uses Visit", func(t *testing.T) {
		var attempts atomic.Int32
		ts := newLocalTestServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if attempts.Add(1) == 1 {
				response(w, http.StatusServiceUnavailable, "retry later")
				return
			}
			response(w, http.StatusOK, fmt.Sprintf("attempt %d", attempts.Load()))
		}))
		c := ts.collector(t)

		var nestedErr error
		var reentered bool
		c.OnError(func(r *Response, err error) {
			if reentered {
				return
			}
			reentered = true
			nestedErr = c.Visit(r.Request.URL.String())
		})

		if err := c.Visit(localTestURL + "/visit"); err == nil {
			t.Fatal("outer Visit returned nil, want the original failure")
		}
		if nestedErr != nil {
			t.Fatalf("re-entrant Visit returned %v, want nil", nestedErr)
		}
		if visited, err := c.HasVisited(localTestURL + "/visit"); err != nil || !visited {
			t.Fatalf("HasVisited after re-entrant Visit = %v, %v; want true, nil", visited, err)
		}
		if got := attempts.Load(); got != 2 {
			t.Fatalf("server attempts = %d, want 2", got)
		}
	})
}
