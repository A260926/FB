// Copyright IBM Corp. 2015, 2025
// SPDX-License-Identifier: MPL-2.0

package retryablehttp

import (
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/hashicorp/go-retryablehttp/support/unitcheck"
)

// fakeClock is a deterministically driven GateClock. Wait calls register
// timers that only fire when the test advances the clock; no real time passes
// and no test uses sleeps.
type fakeClock struct {
	mu      sync.Mutex
	cond    *sync.Cond
	now     time.Time
	pending map[int]*timerWait
	seq     int
	waitErr error
}

type timerWait struct {
	deadline time.Time
	fire     chan struct{}
	fired    bool
}

func newFakeClock(start time.Time) *fakeClock {
	fc := &fakeClock{now: start, pending: make(map[int]*timerWait)}
	fc.cond = sync.NewCond(&fc.mu)
	return fc
}

func (fc *fakeClock) Now() time.Time {
	fc.mu.Lock()
	defer fc.mu.Unlock()
	return fc.now
}

func (fc *fakeClock) Wait(ctx context.Context, d time.Duration) error {
	fc.mu.Lock()
	if fc.waitErr != nil {
		err := fc.waitErr
		fc.mu.Unlock()
		return err
	}
	if d <= 0 {
		err := ctx.Err()
		fc.mu.Unlock()
		return err
	}
	id := fc.seq
	fc.seq++
	w := &timerWait{deadline: fc.now.Add(d), fire: make(chan struct{})}
	fc.pending[id] = w
	fc.cond.Broadcast()
	fc.mu.Unlock()

	defer func() {
		fc.mu.Lock()
		delete(fc.pending, id)
		fc.cond.Broadcast()
		fc.mu.Unlock()
	}()

	select {
	case <-w.fire:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

// Advance moves the clock forward and fires every timer due at the new time.
func (fc *fakeClock) Advance(d time.Duration) {
	fc.mu.Lock()
	defer fc.mu.Unlock()
	fc.now = fc.now.Add(d)
	for _, w := range fc.pending {
		if !w.fired && !w.deadline.After(fc.now) {
			w.fired = true
			close(w.fire)
		}
	}
	fc.cond.Broadcast()
}

// waitForWaiters blocks until at least n clock timers are registered.
func (fc *fakeClock) waitForWaiters(t *testing.T, n int) {
	t.Helper()
	fc.mu.Lock()
	defer fc.mu.Unlock()
	for len(fc.pending) < n {
		fc.cond.Wait()
	}
}

func (fc *fakeClock) waiterCount() int {
	fc.mu.Lock()
	defer fc.mu.Unlock()
	return len(fc.pending)
}

// memCall records one entry into the in-memory transport.
type memCall struct {
	n      int
	url    string
	at     time.Time
	cookie string
	body   string
}

// memTransport is a scripted http.RoundTripper. Each call can be parked on a
// blocker channel and answered with a prepared response. Scripts may be keyed
// by call index for sequential traffic or by URL for concurrent traffic whose
// scheduling order is not guaranteed. Every RoundTrip entry is also announced
// on the buffered entered channel.
type memTransport struct {
	mu        sync.Mutex
	clock     *fakeClock
	calls     []memCall
	entered   chan memCall
	blocks    map[int]chan struct{}
	resps     map[int]*http.Response
	urlBlocks map[string]chan struct{}
	urlResps  map[string]*http.Response
	urlQueues map[string][]*http.Response
}

func newMemTransport(fc *fakeClock) *memTransport {
	return &memTransport{
		clock:     fc,
		entered:   make(chan memCall, 64),
		blocks:    make(map[int]chan struct{}),
		resps:     make(map[int]*http.Response),
		urlBlocks: make(map[string]chan struct{}),
		urlResps:  make(map[string]*http.Response),
		urlQueues: make(map[string][]*http.Response),
	}
}

func (m *memTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	m.mu.Lock()
	n := len(m.calls)
	body := ""
	if r.Body != nil {
		raw, err := io.ReadAll(r.Body)
		if err != nil {
			m.mu.Unlock()
			return nil, err
		}
		_ = r.Body.Close()
		body = string(raw)
	}
	call := memCall{n: n, url: r.URL.String(), at: m.clock.Now(), cookie: r.Header.Get("Cookie"), body: body}
	m.calls = append(m.calls, call)
	block := m.blocks[n]
	if b := m.urlBlocks[r.URL.String()]; b != nil {
		block = b
	}
	resp := m.resps[n]
	if pr := m.urlResps[r.URL.String()]; pr != nil {
		resp = pr
	}
	if q := m.urlQueues[r.URL.String()]; len(q) > 0 {
		resp = q[0]
		m.urlQueues[r.URL.String()] = q[1:]
	}
	m.mu.Unlock()

	m.entered <- call
	if block != nil {
		<-block
	}
	if resp == nil {
		resp = memResponse(r, http.StatusOK, nil)
	}
	resp.Request = r
	return resp, nil
}

func (m *memTransport) blockCall(n int) chan struct{} {
	m.mu.Lock()
	defer m.mu.Unlock()
	ch := make(chan struct{})
	m.blocks[n] = ch
	return ch
}

func (m *memTransport) blockURL(rawURL string) chan struct{} {
	m.mu.Lock()
	defer m.mu.Unlock()
	ch := make(chan struct{})
	m.urlBlocks[rawURL] = ch
	return ch
}

func (m *memTransport) setResp(n int, resp *http.Response) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.resps[n] = resp
}

// enqueueURLResp scripts one response for the next call to rawURL; queued
// responses are consumed in order and take precedence over other scripting.
func (m *memTransport) enqueueURLResp(rawURL string, resp *http.Response) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.urlQueues[rawURL] = append(m.urlQueues[rawURL], resp)
}

func (m *memTransport) nextCall() memCall {
	return <-m.entered
}

func (m *memTransport) callCount() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.calls)
}

func (m *memTransport) snapshot() []memCall {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]memCall, len(m.calls))
	copy(out, m.calls)
	return out
}

func memResponse(r *http.Request, code int, header http.Header) *http.Response {
	if header == nil {
		header = make(http.Header)
	}
	return &http.Response{
		StatusCode: code,
		Header:     header,
		Body:       io.NopCloser(strings.NewReader("mem-body")),
		Request:    r,
	}
}

func retryAfterResp(code int, values ...string) *http.Response {
	header := make(http.Header)
	for _, v := range values {
		header.Add("Retry-After", v)
	}
	return memResponse(nil, code, header)
}

func redirectResp(location string) *http.Response {
	header := make(http.Header)
	header.Set("Location", location)
	return memResponse(nil, http.StatusFound, header)
}

// newGateClient builds a client backed by the in-memory transport with zero
// backoff so the clock is the only source of waiting.
func newGateClient(gate *OriginGate, tr http.RoundTripper, retryMax int) *Client {
	c := NewClient()
	c.Logger = nil
	c.HTTPClient = &http.Client{Transport: tr}
	c.RetryMax = retryMax
	c.RetryWaitMin = time.Millisecond
	c.RetryWaitMax = 10 * time.Millisecond
	c.Backoff = func(time.Duration, time.Duration, int, *http.Response) time.Duration { return 0 }
	c.OriginGate = gate
	return c
}

type doResult struct {
	resp *http.Response
	err  error
}

func doAsync(c *Client, req *Request) chan doResult {
	ch := make(chan doResult, 1)
	go func() {
		resp, err := c.Do(req)
		ch <- doResult{resp: resp, err: err}
	}()
	return ch
}

func mustGate(t *testing.T, maxDelay time.Duration, clock GateClock) *OriginGate {
	t.Helper()
	gate, err := NewOriginGate(maxDelay, clock)
	if err != nil {
		t.Fatalf("NewOriginGate: %v", err)
	}
	return gate
}

func gateDeadline(g *OriginGate, rawURL string) time.Time {
	u, err := url.Parse(rawURL)
	if err != nil {
		return time.Time{}
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.deadline[originKey(u)]
}

// observeHint feeds one synthetic response through the gate's public API.
func observeHint(g *OriginGate, rawURL string, code int, values ...string) {
	req, _ := http.NewRequest(http.MethodGet, rawURL, nil)
	resp := retryAfterResp(code, values...)
	resp.Request = req
	g.Observe(resp)
}

var testBase = time.Date(2026, time.November, 13, 12, 0, 0, 0, time.UTC)

// gateSubtest is one named scenario. The outer test wraps all subtests in a
// single unitcheck.Run so a parent-level UNIT_DONE line is always emitted,
// while each subtest keeps its own checker for precise failure attribution.
type gateSubtest struct {
	name string
	body func(t *testing.T, C *unitcheck.C)
}

func runGateSubtests(t *testing.T, subtests []gateSubtest) {
	t.Helper()
	unitcheck.Run(t, func(C *unitcheck.C) {
		for _, subtest := range subtests {
			ok := t.Run(subtest.name, func(t *testing.T) {
				unitcheck.Run(t, func(SC *unitcheck.C) {
					subtest.body(t, SC)
				})
			})
			// Runs only after the subtest has fully completed.
			C.Check(t, ok, "subtest %s completed cleanly", subtest.name)
		}
	})
}

func TestOriginGateSchedule(t *testing.T) {
	runGateSubtests(t, []gateSubtest{
		{
			name: "two_same_origin_requests_share_pause_other_origin_proceeds",
			body: func(t *testing.T, C *unitcheck.C) {
				fc := newFakeClock(testBase)
				gate := mustGate(t, 30*time.Second, fc)
				tr := newMemTransport(fc)
				client := newGateClient(gate, tr, 1)

				// First request is parked in the transport and answers 429 with
				// a ten-second Retry-After.
				release0 := tr.blockCall(0)
				tr.setResp(0, retryAfterResp(http.StatusTooManyRequests, "10"))

				reqA, err := NewRequest(http.MethodGet, "https://example.com/a", nil)
				if err != nil {
					t.Fatal(err)
				}
				resA := doAsync(client, reqA)
				first := tr.nextCall()
				C.Check(t, first.url == "https://example.com/a" && first.at.Equal(testBase),
					"first call url=%s at=%v want %s at=%v", first.url, first.at, "https://example.com/a", testBase)

				// Releasing it records the pause; A's retry must wait at the gate.
				close(release0)
				fc.waitForWaiters(t, 1)

				// A second same-origin request joins the same pause.
				reqB, err := NewRequest(http.MethodGet, "https://example.com/b?x=1", nil)
				if err != nil {
					t.Fatal(err)
				}
				resB := doAsync(client, reqB)
				fc.waitForWaiters(t, 2)

				// An unrelated origin is not blocked and completes immediately.
				reqOther, err := NewRequest(http.MethodGet, "https://other.invalid/c", nil)
				if err != nil {
					t.Fatal(err)
				}
				resOther := doAsync(client, reqOther)
				otherCall := tr.nextCall()
				C.Check(t, otherCall.url == "https://other.invalid/c" && otherCall.at.Equal(testBase),
					"other origin call url=%s at=%v", otherCall.url, otherCall.at)
				other := <-resOther
				C.Check(t, other.err == nil && other.resp.StatusCode == http.StatusOK,
					"other origin result err=%v resp=%v", other.err, other.resp)

				C.Check(t, tr.callCount() == 2, "only first attempt and other origin sent, got %d calls", tr.callCount())

				// Advancing past the deadline releases both parked requests.
				fc.Advance(10 * time.Second)
				urls := map[string]memCall{}
				for i := 0; i < 2; i++ {
					call := tr.nextCall()
					urls[call.url] = call
				}
				callA, okA := urls["https://example.com/a"]
				callB, okB := urls["https://example.com/b?x=1"]
				C.Check(t, okA && okB, "retry and queued request both sent, got %v", urls)
				C.Check(t, callA.at.Equal(testBase.Add(10*time.Second)) && callB.at.Equal(testBase.Add(10*time.Second)),
					"parked calls released at %v and %v, want %v", callA.at, callB.at, testBase.Add(10*time.Second))

				a := <-resA
				b := <-resB
				C.Check(t, a.err == nil && a.resp.StatusCode == http.StatusOK, "request A err=%v resp=%v", a.err, a.resp)
				C.Check(t, b.err == nil && b.resp.StatusCode == http.StatusOK, "request B err=%v resp=%v", b.err, b.resp)
				C.Check(t, tr.callCount() == 4, "total transport calls = %d, want 4", tr.callCount())
			},
		},
		{
			name: "hint_extended_while_waiting_re_reads_shared_deadline",
			body: func(t *testing.T, C *unitcheck.C) {
				fc := newFakeClock(testBase)
				gate := mustGate(t, 60*time.Second, fc)
				tr := newMemTransport(fc)
				client := newGateClient(gate, tr, 2)

				urlA := "https://example.com/a"
				urlB := "https://example.com/b"

				// Two in-flight requests, parked before any hint exists. Their
				// entry order is not guaranteed, so scripts are keyed by URL.
				releaseA := tr.blockURL(urlA)
				releaseB := tr.blockURL(urlB)
				tr.enqueueURLResp(urlA, retryAfterResp(http.StatusTooManyRequests, "10"))
				tr.enqueueURLResp(urlA, memResponse(nil, http.StatusOK, nil))
				tr.enqueueURLResp(urlB, retryAfterResp(http.StatusServiceUnavailable, "20"))
				tr.enqueueURLResp(urlB, memResponse(nil, http.StatusOK, nil))

				reqA, _ := NewRequest(http.MethodGet, urlA, nil)
				reqB, _ := NewRequest(http.MethodGet, urlB, nil)
				resA := doAsync(client, reqA)
				resB := doAsync(client, reqB)
				firstCalls := map[string]memCall{}
				for i := 0; i < 2; i++ {
					call := tr.nextCall()
					firstCalls[call.url] = call
				}
				C.Check(t, firstCalls[urlA].at.Equal(testBase) && firstCalls[urlB].at.Equal(testBase),
					"both in flight at base time, got %v", firstCalls)

				// A's 429 sets a pause to base+10 and A's retry waits on it.
				close(releaseA)
				fc.waitForWaiters(t, 1)
				C.Check(t, gateDeadline(gate, "https://example.com/").Equal(testBase.Add(10*time.Second)),
					"first hint deadline=%v", gateDeadline(gate, "https://example.com/"))

				// B is still in flight; its 503 extends the shared pause to base+20
				// while A's retry is already waiting.
				close(releaseB)
				fc.waitForWaiters(t, 2)
				C.Check(t, gateDeadline(gate, "https://example.com/").Equal(testBase.Add(20*time.Second)),
					"deadline extended during a wait to %v", gateDeadline(gate, "https://example.com/"))

				// At base+10 A's timer fires, but the re-read deadline is base+20:
				// nothing may reach the transport yet.
				fc.Advance(10 * time.Second)
				C.Check(t, tr.callCount() == 2, "re-read after wake keeps request parked, calls=%d", tr.callCount())

				// Reaching the extended deadline releases both retries together.
				fc.Advance(10 * time.Second)
				retries := map[string]memCall{}
				for i := 0; i < 2; i++ {
					call := tr.nextCall()
					retries[call.url] = call
				}
				for rawURL, call := range retries {
					C.Check(t, call.at.Equal(testBase.Add(20*time.Second)),
						"retry of %s sent at %v, want %v", rawURL, call.at, testBase.Add(20*time.Second))
				}
				a := <-resA
				b := <-resB
				C.Check(t, a.err == nil && a.resp.StatusCode == http.StatusOK, "request A %v %v", a.err, a.resp)
				C.Check(t, b.err == nil && b.resp.StatusCode == http.StatusOK, "request B %v %v", b.err, b.resp)
				C.Check(t, tr.callCount() == 4, "total transport calls = %d, want 4", tr.callCount())

				// A hint that has already expired can never shorten the pause.
				observeHint(gate, "https://example.com/", http.StatusServiceUnavailable,
					testBase.Add(-time.Second).UTC().Format("Mon, 02 Jan 2006 15:04:05 GMT"))
				C.Check(t, gateDeadline(gate, "https://example.com/").Equal(testBase.Add(20*time.Second)),
					"past later hint cannot move deadline %v", gateDeadline(gate, "https://example.com/"))
			},
		},
		{
			name: "origin_key_normalization",
			body: func(t *testing.T, C *unitcheck.C) {
				key := func(rawURL string) string {
					u, err := url.Parse(rawURL)
					if err != nil {
						t.Fatal(err)
					}
					return originKey(u)
				}
				C.Check(t, key("http://example.com/") == "http://example.com:80", "omit 80")
				C.Check(t, key("http://example.com:80/path?q=1") == "http://example.com:80", "explicit 80 with path/query")
				C.Check(t, key("http://user:pass@example.com:80/") == "http://example.com:80", "userinfo ignored")
				C.Check(t, key("https://example.com/") == "https://example.com:443", "omit 443")
				C.Check(t, key("https://example.com:443/a") == "https://example.com:443", "explicit 443")
				C.Check(t, key("https://EXAMPLE.com/") == "https://example.com:443", "hostname lowercased")
				C.Check(t, key("https://example.com:8443/") == "https://example.com:8443", "non-default port kept")
				C.Check(t, key("http://example.com/") != key("https://example.com/"), "schemes not merged")
				C.Check(t, key("https://example.com/") != key("https://other.com/"), "hosts not merged")
			},
		},
		{
			name: "huge_seconds_capped_without_overflow",
			body: func(t *testing.T, C *unitcheck.C) {
				fc := newFakeClock(testBase)
				gate := mustGate(t, 5*time.Second, fc)
				tr := newMemTransport(fc)
				client := newGateClient(gate, tr, 0)

				tr.setResp(0, retryAfterResp(http.StatusTooManyRequests, "99999999999"))
				req, _ := NewRequest(http.MethodGet, "https://example.com/huge", nil)
				res := doAsync(client, req)
				tr.nextCall()
				first := <-res
				C.Check(t, first.err != nil, "RetryMax=0 gives up, got err=%v", first.err)
				C.Check(t, gateDeadline(gate, "https://example.com/").Equal(testBase.Add(5*time.Second)),
					"huge hint capped at maxDelay, deadline=%v", gateDeadline(gate, "https://example.com/"))

				// The next request waits exactly for the capped deadline.
				next, _ := NewRequest(http.MethodGet, "https://example.com/next", nil)
				resNext := doAsync(client, next)
				fc.waitForWaiters(t, 1)
				C.Check(t, tr.callCount() == 1, "next request held behind gate, calls=%d", tr.callCount())
				fc.Advance(5 * time.Second)
				tr.nextCall()
				got := <-resNext
				C.Check(t, got.err == nil && got.resp.StatusCode == http.StatusOK, "next result %v %v", got.err, got.resp)
			},
		},
		{
			name: "standard_client_goes_through_gate",
			body: func(t *testing.T, C *unitcheck.C) {
				fc := newFakeClock(testBase)
				gate := mustGate(t, 30*time.Second, fc)
				tr := newMemTransport(fc)
				client := newGateClient(gate, tr, 0)
				std := client.StandardClient()

				observeHint(gate, "https://standard.example/", http.StatusTooManyRequests, "10")
				res := make(chan doResult, 1)
				go func() {
					resp, err := std.Get("https://standard.example/path")
					res <- doResult{resp: resp, err: err}
				}()
				fc.waitForWaiters(t, 1)
				C.Check(t, tr.callCount() == 0, "standard client held before first RoundTrip")
				fc.Advance(10 * time.Second)
				call := tr.nextCall()
				C.Check(t, call.url == "https://standard.example/path" && call.at.Equal(testBase.Add(10*time.Second)),
					"standard client call %v", call)
				got := <-res
				C.Check(t, got.err == nil && got.resp.StatusCode == http.StatusOK, "standard result %v %v", got.err, got.resp)
			},
		},
	})
}

func TestOriginGateRedirect(t *testing.T) {
	runGateSubtests(t, []gateSubtest{
		{
			name: "each_redirect_hop_waits_for_its_origin",
			body: func(t *testing.T, C *unitcheck.C) {
				fc := newFakeClock(testBase)
				gate := mustGate(t, 30*time.Second, fc)
				tr := newMemTransport(fc)
				client := newGateClient(gate, tr, 0)

				// The redirect target is already paused when the redirect happens.
				observeHint(gate, "https://target.example/next", http.StatusServiceUnavailable, "10")
				tr.setResp(0, redirectResp("https://target.example/next"))

				req, _ := NewRequest(http.MethodGet, "https://source.example/start", nil)
				res := doAsync(client, req)

				hop0 := tr.nextCall()
				C.Check(t, hop0.url == "https://source.example/start", "first hop url=%s", hop0.url)
				fc.waitForWaiters(t, 1)
				C.Check(t, tr.callCount() == 1, "redirect hop held at target origin gate")

				fc.Advance(10 * time.Second)
				hop1 := tr.nextCall()
				C.Check(t, hop1.url == "https://target.example/next" && hop1.at.Equal(testBase.Add(10*time.Second)),
					"redirect hop %v", hop1)
				got := <-res
				C.Check(t, got.err == nil && got.resp.StatusCode == http.StatusOK, "redirect result %v %v", got.err, got.resp)
				C.Check(t, got.resp.Request.URL.String() == "https://target.example/next", "final request url=%s", got.resp.Request.URL)
			},
		},
		{
			name: "client_redirect_cookie_and_timeout_settings_preserved",
			body: func(t *testing.T, C *unitcheck.C) {
				fc := newFakeClock(testBase)
				gate := mustGate(t, 30*time.Second, fc)
				tr := newMemTransport(fc)

				jar, err := cookiejar.New(nil)
				if err != nil {
					t.Fatal(err)
				}
				checkRedirect := func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
				hc := &http.Client{
					Transport:     tr,
					CheckRedirect: checkRedirect,
					Jar:           jar,
					Timeout:       7 * time.Second,
				}
				client := NewClient()
				client.Logger = nil
				client.HTTPClient = hc
				client.RetryMax = 0
				client.OriginGate = gate

				// ErrUseLastResponse: the 302 is returned and the redirect is not followed.
				tr.setResp(0, redirectResp("https://elsewhere.example/"))
				resp, err := client.Get("https://jar.example/start")
				C.Check(t, err == nil && resp.StatusCode == http.StatusFound, "redirect policy result %v %v", err, resp)
				C.Check(t, tr.callCount() == 1, "redirect not followed, calls=%d", tr.callCount())

				// The cookie jar still applies to the caller's http.Client.
				cookieHeader := make(http.Header)
				cookieHeader.Set("Set-Cookie", "sid=1; Path=/")
				tr.setResp(1, memResponse(nil, http.StatusOK, cookieHeader))
				resp2, err := client.Get("https://jar.example/")
				C.Check(t, err == nil && resp2.StatusCode == http.StatusOK, "cookie set result %v %v", err, resp2)
				resp3, err := client.Get("https://jar.example/later")
				C.Check(t, err == nil && resp3.StatusCode == http.StatusOK, "cookie reuse result %v %v", err, resp3)
				calls := tr.snapshot()
				C.Check(t, calls[2].cookie == "sid=1", "jar sent cookie %q on later call", calls[2].cookie)

				// The supplied client is never mutated.
				C.Check(t, hc.Transport == tr, "transport pointer unchanged, got %T", hc.Transport)
				C.Check(t, hc.CheckRedirect != nil, "CheckRedirect retained")
				C.Check(t, hc.Jar == jar, "jar retained")
				C.Check(t, hc.Timeout == 7*time.Second, "timeout retained, got %v", hc.Timeout)
			},
		},
	})
}

func TestOriginGateErrors(t *testing.T) {
	runGateSubtests(t, []gateSubtest{
		{
			name: "constructor_validation",
			body: func(t *testing.T, C *unitcheck.C) {
				g, err := NewOriginGate(0, nil)
				C.Check(t, err != nil && g == nil, "zero maxDelay rejected, g=%v err=%v", g, err)
				g, err = NewOriginGate(-time.Second, newFakeClock(testBase))
				C.Check(t, err != nil && g == nil, "negative maxDelay rejected, g=%v err=%v", g, err)
				g, err = NewOriginGate(time.Second, nil)
				C.Check(t, err == nil && g != nil, "nil clock selects real clock, g=%v err=%v", g, err)
				g2, err := NewOriginGate(time.Second, newFakeClock(testBase))
				C.Check(t, err == nil && g2 != nil, "explicit clock accepted, g=%v err=%v", g2, err)
			},
		},
		{
			name: "wait_interrupted_before_transport",
			body: func(t *testing.T, C *unitcheck.C) {
				fc := newFakeClock(testBase)
				gate := mustGate(t, 30*time.Second, fc)
				tr := newMemTransport(fc)
				client := newGateClient(gate, tr, 1)
				observeHint(gate, "https://cancel.example/", http.StatusTooManyRequests, "10")

				ctx, cancel := context.WithCancel(context.Background())
				req, _ := NewRequestWithContext(ctx, http.MethodGet, "https://cancel.example/x", nil)
				res := doAsync(client, req)
				fc.waitForWaiters(t, 1)
				cancel()
				got := <-res
				C.Check(t, got.err != nil && errors.Is(got.err, context.Canceled),
					"canceled wait error unwraps, got %v", got.err)
				C.Check(t, tr.callCount() == 0, "transport never entered on wait error, calls=%d", tr.callCount())

				// A clock-level error is likewise reachable through errors.Is.
				sentinel := errors.New("clock stopped")
				fc.waitErr = sentinel
				req2, _ := NewRequest(http.MethodGet, "https://cancel.example/y", nil)
				client.RetryMax = 0
				got2 := <-doAsync(client, req2)
				C.Check(t, got2.err != nil && errors.Is(got2.err, sentinel),
					"clock error unwraps, got %v", got2.err)
				C.Check(t, tr.callCount() == 0, "transport never entered on clock error, calls=%d", tr.callCount())
			},
		},
		{
			name: "zero_retries_still_records_hint",
			body: func(t *testing.T, C *unitcheck.C) {
				fc := newFakeClock(testBase)
				gate := mustGate(t, 30*time.Second, fc)
				tr := newMemTransport(fc)
				client := newGateClient(gate, tr, 0)

				tr.setResp(0, retryAfterResp(http.StatusTooManyRequests, "10"))
				req, _ := NewRequest(http.MethodGet, "https://zero.example/a", nil)
				first := <-doAsync(client, req)
				C.Check(t, first.err != nil, "no retries configured, request gives up: %v", first.err)
				C.Check(t, gateDeadline(gate, "https://zero.example/").Equal(testBase.Add(10*time.Second)),
					"hint recorded despite RetryMax=0, deadline=%v", gateDeadline(gate, "https://zero.example/"))

				next, _ := NewRequest(http.MethodGet, "https://zero.example/b", nil)
				resNext := doAsync(client, next)
				fc.waitForWaiters(t, 1)
				C.Check(t, tr.callCount() == 1, "later request blocked by recorded hint, calls=%d", tr.callCount())
				fc.Advance(10 * time.Second)
				tr.nextCall()
				got := <-resNext
				C.Check(t, got.err == nil && got.resp.StatusCode == http.StatusOK, "later request %v %v", got.err, got.resp)
			},
		},
		{
			name: "request_body_replayed_and_hooks_not_duplicated",
			body: func(t *testing.T, C *unitcheck.C) {
				fc := newFakeClock(testBase)
				gate := mustGate(t, 30*time.Second, fc)
				tr := newMemTransport(fc)
				client := newGateClient(gate, tr, 1)
				var hookCalls int
				client.RequestLogHook = func(Logger, *http.Request, int) { hookCalls++ }

				// 503 without Retry-After: no gate pause, ordinary retry only.
				tr.setResp(0, memResponse(nil, http.StatusServiceUnavailable, nil))
				req, _ := NewRequest(http.MethodPost, "https://replay.example/ingest", []byte("filter=active"))
				got := <-doAsync(client, req)
				C.Check(t, got.err == nil && got.resp.StatusCode == http.StatusOK, "replay result %v %v", got.err, got.resp)
				calls := tr.snapshot()
				C.Check(t, len(calls) == 2, "two attempts, got %d", len(calls))
				C.Check(t, calls[0].body == "filter=active" && calls[1].body == calls[0].body,
					"body replayed identically, bodies=%q", []string{calls[0].body, calls[1].body})
				C.Check(t, hookCalls == 2, "one hook call per attempt, got %d", hookCalls)
				C.Check(t, fc.waiterCount() == 0, "gate waiting added no attempts or timers")
			},
		},
		{
			name: "only_429_and_503_record_hints",
			body: func(t *testing.T, C *unitcheck.C) {
				fc := newFakeClock(testBase)
				gate := mustGate(t, 30*time.Second, fc)
				tr := newMemTransport(fc)
				client := newGateClient(gate, tr, 0)

				tr.setResp(0, retryAfterResp(http.StatusOK, "10"))
				tr.setResp(1, retryAfterResp(http.StatusBadGateway, "10"))
				tr.setResp(2, redirectResp("https://ignored.example/"))
				tr.resps[2].Header.Set("Retry-After", "10")
				// Keep the 302 from generating an extra redirect RoundTrip.
				client.HTTPClient.CheckRedirect = func(*http.Request, []*http.Request) error {
					return http.ErrUseLastResponse
				}

				for i, rawURL := range []string{
					"https://status.example/ok",
					"https://status.example/bad-gateway",
					"https://status.example/redirect",
				} {
					req, _ := NewRequest(http.MethodGet, rawURL, nil)
					got := <-doAsync(client, req)
					switch i {
					case 1:
						// RetryMax=0: the 5xx response is drained and surfaced as an error.
						C.Check(t, got.err != nil && got.resp == nil, "502 exhausted retries, got %v %v", got.err, got.resp)
					default:
						C.Check(t, got.err == nil && got.resp != nil, "call %d returned a response", i)
					}
				}
				C.Check(t, gateDeadline(gate, "https://status.example/").IsZero(),
					"non-eligible statuses left gate untouched, deadline=%v", gateDeadline(gate, "https://status.example/"))

				// A following request goes straight to the transport.
				follow, _ := NewRequest(http.MethodGet, "https://status.example/follow", nil)
				resFollow := doAsync(client, follow)
				tr.nextCall()
				got := <-resFollow
				C.Check(t, got.err == nil, "no pause recorded, err=%v", got.err)
				C.Check(t, fc.waiterCount() == 0, "no timers registered")
			},
		},
		{
			name: "retry_after_value_rules",
			body: func(t *testing.T, C *unitcheck.C) {
				fc := newFakeClock(testBase)
				gate := mustGate(t, 30*time.Second, fc)
				tr := newMemTransport(fc)
				client := newGateClient(gate, tr, 0)
				origin := "https://values.example/"

				// Zero, malformed, past-date and out-of-range values add nothing.
				// Each uses its own origin so none of them self-blocks later calls.
				invalid := []string{
					"https://zero.example/",
					"https://garbage.example/",
					"https://past.example/",
					"https://overflow.example/",
				}
				tr.setResp(0, retryAfterResp(http.StatusTooManyRequests, "0"))
				tr.setResp(1, retryAfterResp(http.StatusTooManyRequests, "not-a-number"))
				tr.setResp(2, retryAfterResp(http.StatusTooManyRequests, "Fri, 31 Dec 1999 23:59:59 GMT"))
				tr.setResp(3, retryAfterResp(http.StatusTooManyRequests, "99999999999999999999999"))
				for i, rawURL := range invalid {
					req, _ := NewRequest(http.MethodGet, rawURL, nil)
					got := <-doAsync(client, req)
					C.Check(t, got.err != nil, "hint call %d exhausted retries", i)
					C.Check(t, gateDeadline(gate, rawURL).IsZero(), "invalid value on %s recorded no deadline", rawURL)
				}

				// Multiple field values on one response: the latest valid one wins.
				tr.setResp(4, retryAfterResp(http.StatusTooManyRequests, "3", "10"))
				req, _ := NewRequest(http.MethodGet, origin, nil)
				<-doAsync(client, req)
				C.Check(t, gateDeadline(gate, origin).Equal(testBase.Add(10*time.Second)),
					"latest of multiple values wins, deadline=%v", gateDeadline(gate, origin))

				// A shorter later hint cannot shorten the recorded deadline.
				observeHint(gate, origin, http.StatusServiceUnavailable, "2")
				C.Check(t, gateDeadline(gate, origin).Equal(testBase.Add(10*time.Second)),
					"shorter hint kept old deadline=%v", gateDeadline(gate, origin))

				// An HTTP-date with commas is one field value and parses as a whole date.
				future := testBase.Add(15 * time.Second).UTC().Format("Mon, 02 Jan 2006 15:04:05 GMT")
				observeHint(gate, "https://date.example/", http.StatusTooManyRequests, future)
				C.Check(t, gateDeadline(gate, "https://date.example/").Equal(testBase.Add(15*time.Second)),
					"comma-containing date parsed whole, deadline=%v", gateDeadline(gate, "https://date.example/"))

				// The pause from the multi-value header is still enforced.
				next, _ := NewRequest(http.MethodGet, origin, nil)
				resNext := doAsync(client, next)
				fc.waitForWaiters(t, 1)
				fc.Advance(10 * time.Second)
				tr.nextCall()
				got := <-resNext
				C.Check(t, got.err == nil && got.resp.StatusCode == http.StatusOK, "paused request %v %v", got.err, got.resp)
			},
		},
	})
}
