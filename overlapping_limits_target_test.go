// Copyright 2026 Adam Tauber
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
	"io"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// manualClock is an internally advanceable time source. Delay / RandomDelay
// holds create timers through it, so release timing can be asserted by
// advancing the clock instead of comparing wall-clock durations or reading
// private counters.
type manualClock struct {
	mu        sync.Mutex
	now       time.Time
	timers    map[*manualTimer]struct{}
	durations []time.Duration
}

type manualTimer struct {
	at      time.Time
	ch      chan time.Time
	clock   *manualClock
	stopped atomic.Bool
}

func newManualClock() *manualClock {
	return &manualClock{
		timers: make(map[*manualTimer]struct{}),
		now:    time.Unix(1, 0),
	}
}

func (m *manualClock) Sleep(d time.Duration) {
	if d <= 0 {
		return
	}
	ch, stop := m.NewTimer(d)
	defer stop()
	<-ch
}

func (m *manualClock) NewTimer(d time.Duration) (<-chan time.Time, func() bool) {
	m.mu.Lock()
	t := &manualTimer{at: m.now.Add(d), ch: make(chan time.Time, 1), clock: m}
	m.timers[t] = struct{}{}
	m.durations = append(m.durations, d)
	m.mu.Unlock()
	return t.ch, t.stop
}

func (t *manualTimer) stop() bool {
	if !t.stopped.CompareAndSwap(false, true) {
		return false
	}
	t.clock.mu.Lock()
	delete(t.clock.timers, t)
	t.clock.mu.Unlock()
	return true
}

// Advance moves the clock forward and fires every timer due at the new
// instant.
func (m *manualClock) Advance(d time.Duration) {
	m.mu.Lock()
	m.now = m.now.Add(d)
	due := make([]*manualTimer, 0)
	for t := range m.timers {
		if !t.at.After(m.now) {
			due = append(due, t)
		}
	}
	for _, t := range due {
		delete(m.timers, t)
	}
	now := m.now
	m.mu.Unlock()
	for _, t := range due {
		t.stopped.Store(true)
		select {
		case t.ch <- now:
		default:
		}
	}
}

func (m *manualClock) timerCount() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.timers)
}

// waitTimerCount blocks until at least want delay timers are pending.
func (m *manualClock) waitTimerCount(t *testing.T, want int) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for m.timerCount() < want {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %d active delay timers, got %d", want, m.timerCount())
		}
		time.Sleep(time.Millisecond)
	}
}

func (m *manualClock) snapshotDurations() []time.Duration {
	m.mu.Lock()
	defer m.mu.Unlock()
	return append([]time.Duration(nil), m.durations...)
}

// roundTripCall controls a single in-flight RoundTrip: tests wait for
// entered and unblock it by closing release. roundTripErr fails the
// RoundTrip itself; bodyReadErr yields a response whose body errors while
// Colly reads it.
type roundTripCall struct {
	path         string
	entered      chan struct{}
	release      chan struct{}
	roundTripErr error
	bodyReadErr  error
	once         sync.Once
}

func (c *roundTripCall) signalEntered() {
	c.once.Do(func() { close(c.entered) })
}

// blockingTransport is a RoundTripper that observes real request starts,
// per-host and overall in-flight peaks, and holds downloads open until the
// test releases them. No network port is involved.
type blockingTransport struct {
	mu         sync.Mutex
	calls      map[string]*roundTripCall
	hits       map[string]int
	entered    map[string]struct{}
	active     atomic.Int64
	peak       atomic.Int64
	hostActive map[string]*atomic.Int64
	hostPeak   map[string]*atomic.Int64
}

func newBlockingTransport() *blockingTransport {
	return &blockingTransport{
		calls:      make(map[string]*roundTripCall),
		hits:       make(map[string]int),
		entered:    make(map[string]struct{}),
		hostActive: make(map[string]*atomic.Int64),
		hostPeak:   make(map[string]*atomic.Int64),
	}
}

func (t *blockingTransport) register(path string) *roundTripCall {
	c := &roundTripCall{
		path:    path,
		entered: make(chan struct{}),
		release: make(chan struct{}),
	}
	t.mu.Lock()
	t.calls[path] = c
	t.mu.Unlock()
	return c
}

func (t *blockingTransport) call(path string) *roundTripCall {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.calls[path]
}

func (t *blockingTransport) hitCount(path string) int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.hits[path]
}

// enteredCount reports how many distinct requests have reached the
// transport so far.
func (t *blockingTransport) enteredCount() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return len(t.entered)
}

// enteredPaths snapshots the requests currently or previously in transport.
func (t *blockingTransport) enteredPaths() []string {
	t.mu.Lock()
	defer t.mu.Unlock()
	paths := make([]string, 0, len(t.entered))
	for p := range t.entered {
		paths = append(paths, p)
	}
	return paths
}

// waitEnteredCount blocks until want distinct requests have entered the
// transport. It does not assume which requests get admitted first.
func (t *blockingTransport) waitEnteredCount(tst *testing.T, want int) {
	tst.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for t.enteredCount() < want {
		if time.Now().After(deadline) {
			tst.Fatalf("timed out waiting for %d requests in transport, got %d: %v",
				want, t.enteredCount(), t.enteredPaths())
		}
		time.Sleep(time.Millisecond)
	}
}

// waitForEntered blocks until one of the given paths reaches the transport.
func (t *blockingTransport) waitForEntered(tst *testing.T, paths ...string) {
	tst.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for {
		for _, p := range paths {
			if t.hasEntered(p) {
				return
			}
		}
		if time.Now().After(deadline) {
			tst.Fatalf("timed out waiting for any of %v to enter the transport: %v",
				paths, t.enteredPaths())
		}
		time.Sleep(time.Millisecond)
	}
}

func (t *blockingTransport) hasEntered(path string) bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	_, ok := t.entered[path]
	return ok
}

func (t *blockingTransport) hostPeakValue(host string) int64 {
	t.mu.Lock()
	defer t.mu.Unlock()
	if p, ok := t.hostPeak[host]; ok {
		return p.Load()
	}
	return 0
}

func (t *blockingTransport) hostCounter(m map[string]*atomic.Int64, host string) *atomic.Int64 {
	c, ok := m[host]
	if !ok {
		c = &atomic.Int64{}
		m[host] = c
	}
	return c
}

func updatePeak(peak *atomic.Int64, n int64) {
	for {
		p := peak.Load()
		if n <= p || peak.CompareAndSwap(p, n) {
			return
		}
	}
}

func (t *blockingTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	t.mu.Lock()
	call := t.calls[req.URL.Path]
	t.hits[req.URL.Path]++
	t.entered[req.URL.Path] = struct{}{}
	host := req.URL.Host
	ha := t.hostCounter(t.hostActive, host)
	hp := t.hostCounter(t.hostPeak, host)
	t.mu.Unlock()

	n := t.active.Add(1)
	updatePeak(&t.peak, n)
	hn := ha.Add(1)
	updatePeak(hp, hn)

	if call != nil {
		call.signalEntered()
		<-call.release
		if call.roundTripErr != nil {
			t.active.Add(-1)
			ha.Add(-1)
			return nil, call.roundTripErr
		}
	}

	t.active.Add(-1)
	ha.Add(-1)

	body := io.NopCloser(strings.NewReader("ok"))
	if call != nil && call.bodyReadErr != nil {
		body = io.NopCloser(&errReader{err: call.bodyReadErr})
	}
	return &http.Response{
		StatusCode: http.StatusOK,
		Header:     make(http.Header),
		Body:       body,
		Request:    req,
	}, nil
}

type errReader struct{ err error }

func (r *errReader) Read([]byte) (int, error) { return 0, r.err }

// peakTransport answers every request from a shared release barrier and
// only records the overall and per-host concurrency peaks. The barrier
// lets a test hold admitted requests open until the expected number are
// concurrently in flight, making overlap deterministic.
type peakTransport struct {
	active     atomic.Int64
	peak       atomic.Int64
	hostActive sync.Map // host -> *atomic.Int64
	hostPeak   sync.Map // host -> *atomic.Int64
	hold       <-chan struct{}
}

func loadAtomic(m *sync.Map, host string) *atomic.Int64 {
	v, _ := m.LoadOrStore(host, &atomic.Int64{})
	return v.(*atomic.Int64)
}

func (p *peakTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	ha := loadAtomic(&p.hostActive, req.URL.Host)
	hp := loadAtomic(&p.hostPeak, req.URL.Host)
	n := p.active.Add(1)
	updatePeak(&p.peak, n)
	hn := ha.Add(1)
	updatePeak(hp, hn)
	if p.hold != nil {
		<-p.hold
	}
	p.active.Add(-1)
	ha.Add(-1)
	return &http.Response{
		StatusCode: http.StatusOK,
		Header:     make(http.Header),
		Body:       io.NopCloser(strings.NewReader("ok")),
		Request:    req,
	}, nil
}

func (p *peakTransport) hostPeakValue(host string) int64 {
	if v, ok := p.hostPeak.Load(host); ok {
		return v.(*atomic.Int64).Load()
	}
	return 0
}

func waitEntered(t *testing.T, c *roundTripCall, what string) {
	t.Helper()
	select {
	case <-c.entered:
	case <-time.After(3 * time.Second):
		t.Fatalf("timed out waiting for %s to reach the transport", what)
	}
}

func assertNotEntered(t *testing.T, c *roundTripCall, what string) {
	t.Helper()
	select {
	case <-c.entered:
		t.Fatalf("%s unexpectedly reached the transport", what)
	default:
	}
}

// letSettle gives waiting goroutines time to block on admission; it is only
// a scheduling grace, never the basis of a timing assertion.
func letSettle() { time.Sleep(30 * time.Millisecond) }

// waitForPeak blocks until the transport observes want concurrent
// downloads, which can only happen once admission has allowed that many.
func waitForPeak(t *testing.T, tr *blockingTransport, want int64) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for tr.peak.Load() < want {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %d concurrent downloads, peak got %d", want, tr.peak.Load())
		}
		time.Sleep(time.Millisecond)
	}
}

func newAllMatchingCollector(t *testing.T, tr http.RoundTripper, rules ...*LimitRule) *Collector {
	t.Helper()
	c := NewCollector(Async(true), IgnoreRobotsTxt(), AllowURLRevisit())
	c.SetClient(&http.Client{Transport: tr})
	c.UseAllMatchingLimits()
	if err := c.Limits(rules); err != nil {
		t.Fatalf("Limits: %v", err)
	}
	return c
}

func mustVisit(t *testing.T, c *Collector, u string) {
	t.Helper()
	if err := c.Visit(u); err != nil {
		t.Fatalf("Visit(%q): %v", u, err)
	}
}

func releaseCalls(calls ...*roundTripCall) {
	for _, c := range calls {
		close(c.release)
	}
}

// TestOverlappingLimitsAdmission covers the admission semantics of
// UseAllMatchingLimits: every matching rule is enforced at once regardless
// of order, waiting never occupies partial slots, duplicate rule pointers
// count once while distinct rules count separately, and requests without a
// matching rule pass through.
func TestOverlappingLimitsAdmission(t *testing.T) {
	t.Run("blocked site does not consume wide-rule slots", func(t *testing.T) {
		tr := newBlockingTransport()
		a1 := tr.register("/a1")
		a2 := tr.register("/a2")
		b1 := tr.register("/b1")

		c := newAllMatchingCollector(t, tr,
			&LimitRule{DomainGlob: "*", Parallelism: 2},
			&LimitRule{DomainGlob: "a.test", Parallelism: 1},
		)

		mustVisit(t, c, "http://a.test/a1")
		mustVisit(t, c, "http://a.test/a2")
		mustVisit(t, c, "http://b.test/b1")

		// Exactly two downloads run: b1 plus one of the a.test requests.
		// The other a.test request waits on the a.test rule.
		tr.waitEnteredCount(t, 2)
		letSettle()
		if got := tr.enteredCount(); got != 2 {
			t.Fatalf("want exactly 2 downloads admitted, got %d: %v", got, tr.enteredPaths())
		}
		if !tr.hasEntered("/b1") {
			t.Fatalf("b.test request must take the wide rule's free slot: %v", tr.enteredPaths())
		}
		enteredA := 0
		for _, p := range []string{"/a1", "/a2"} {
			if tr.hasEntered(p) {
				enteredA++
			}
		}
		if enteredA != 1 {
			t.Fatalf("want exactly one a.test download admitted, got %d: %v", enteredA, tr.enteredPaths())
		}
		if got := tr.active.Load(); got != 2 {
			t.Fatalf("want 2 downloads in flight (one a.test + b1), got %d", got)
		}
		if got := tr.peak.Load(); got != 2 {
			t.Fatalf("want overall peak 2, got %d", got)
		}
		if got := tr.hostPeakValue("a.test"); got != 1 {
			t.Fatalf("want a.test peak 1, got %d", got)
		}

		// Unblock everything: the waiting a.test request then starts while
		// b1 may still hold the other wide-rule slot. a.test concurrency
		// must never exceed one.
		releaseCalls(a1, a2, b1)
		tr.waitEnteredCount(t, 3)
		if got := tr.hostPeakValue("a.test"); got != 1 {
			t.Fatalf("a.test peak must stay 1, got %d", got)
		}
		c.Wait()
	})

	t.Run("rule order does not change capacity", func(t *testing.T) {
		tr := newBlockingTransport()
		a1 := tr.register("/a1")
		a2 := tr.register("/a2")
		b1 := tr.register("/b1")

		// Site-specific rule registered before the wide rule.
		c := newAllMatchingCollector(t, tr,
			&LimitRule{DomainGlob: "a.test", Parallelism: 1},
			&LimitRule{DomainGlob: "*", Parallelism: 2},
		)

		mustVisit(t, c, "http://a.test/a1")
		mustVisit(t, c, "http://a.test/a2")
		mustVisit(t, c, "http://b.test/b1")

		tr.waitEnteredCount(t, 2)
		letSettle()
		if got := tr.enteredCount(); got != 2 {
			t.Fatalf("want exactly 2 downloads admitted with reversed rules, got %d: %v", got, tr.enteredPaths())
		}
		if !tr.hasEntered("/b1") {
			t.Fatalf("b.test request must be admitted with reversed rules: %v", tr.enteredPaths())
		}
		enteredA := 0
		for _, p := range []string{"/a1", "/a2"} {
			if tr.hasEntered(p) {
				enteredA++
			}
		}
		if enteredA != 1 {
			t.Fatalf("want exactly one a.test download with reversed rules, got %d: %v", enteredA, tr.enteredPaths())
		}
		if got := tr.peak.Load(); got != 2 {
			t.Fatalf("want overall peak 2 with reversed rules, got %d", got)
		}
		if got := tr.hostPeakValue("a.test"); got != 1 {
			t.Fatalf("want a.test peak 1 with reversed rules, got %d", got)
		}

		releaseCalls(a1, a2, b1)
		tr.waitEnteredCount(t, 3)
		c.Wait()
	})

	t.Run("duplicate rule pointer counts once", func(t *testing.T) {
		tr := newBlockingTransport()
		wide := &LimitRule{DomainGlob: "*", Parallelism: 3}
		c := newAllMatchingCollector(t, tr, wide, wide)

		calls := make([]*roundTripCall, 0, 5)
		for i := 0; i < 5; i++ {
			path := "/b" + string(rune('1'+i))
			calls = append(calls, tr.register(path))
			mustVisit(t, c, "http://b.test"+path)
		}
		waitForPeak(t, tr, 3)
		letSettle()
		if got := tr.peak.Load(); got != 3 {
			t.Fatalf("same rule pointer registered twice must still cap at 3, got peak %d", got)
		}
		releaseCalls(calls...)
		c.Wait()
	})

	t.Run("distinct rules with identical scope both apply", func(t *testing.T) {
		tr := newBlockingTransport()
		c := newAllMatchingCollector(t, tr,
			&LimitRule{DomainGlob: "*", Parallelism: 2},
			&LimitRule{DomainGlob: "*", Parallelism: 3},
		)

		calls := make([]*roundTripCall, 0, 5)
		for i := 0; i < 5; i++ {
			path := "/b" + string(rune('1'+i))
			calls = append(calls, tr.register(path))
			mustVisit(t, c, "http://b.test"+path)
		}
		waitForPeak(t, tr, 2)
		letSettle()
		// Every download needs a slot in both rules; the tighter rule caps
		// concurrency at 2 even though the other one allows 3.
		if got := tr.peak.Load(); got != 2 {
			t.Fatalf("two distinct matching rules must both apply, want peak 2, got %d", got)
		}
		releaseCalls(calls...)
		c.Wait()
	})

	t.Run("request without matching rule is not gated", func(t *testing.T) {
		tr := newBlockingTransport()
		c1 := tr.register("/c1")
		c := newAllMatchingCollector(t, tr,
			&LimitRule{DomainGlob: "a.test", Parallelism: 1},
		)
		mustVisit(t, c, "http://c.test/c1")
		waitEntered(t, c1, "c1")
		close(c1.release)
		c.Wait()
	})

	t.Run("default mode still uses the first matching rule only", func(t *testing.T) {
		tr := newBlockingTransport()
		a1 := tr.register("/a1")
		a2 := tr.register("/a2")

		c := NewCollector(Async(true), IgnoreRobotsTxt(), AllowURLRevisit())
		c.SetClient(&http.Client{Transport: tr})
		// No UseAllMatchingLimits: first match ("*", parallelism 2) wins,
		// the a.test rule is ignored.
		if err := c.Limits([]*LimitRule{
			{DomainGlob: "*", Parallelism: 2},
			{DomainGlob: "a.test", Parallelism: 1},
		}); err != nil {
			t.Fatalf("Limits: %v", err)
		}

		mustVisit(t, c, "http://a.test/a1")
		mustVisit(t, c, "http://a.test/a2")
		waitEntered(t, a1, "a1")
		waitEntered(t, a2, "a2")
		if got := tr.hostPeakValue("a.test"); got != 2 {
			t.Fatalf("first-match mode must ignore the tighter second rule, got a.test peak %d", got)
		}
		releaseCalls(a1, a2)
		c.Wait()
	})
}

// TestOverlappingLimitsShared verifies that collectors sharing LimitRules
// contend for one real capacity across both admission modes and reversed
// registration orders, without deadlock or overshoot, and that Clone keeps
// the all-matching mode of the shared backend.
func TestOverlappingLimitsShared(t *testing.T) {
	run := func(t *testing.T, reverseOrder bool) {
		tr := newBlockingTransport()
		wide := &LimitRule{DomainGlob: "*", Parallelism: 2}
		site := &LimitRule{DomainGlob: "a.test", Parallelism: 1}

		// Collector A uses all-matching admission.
		a := NewCollector(Async(true), IgnoreRobotsTxt(), AllowURLRevisit())
		a.SetClient(&http.Client{Transport: tr})
		a.UseAllMatchingLimits()
		rules := []*LimitRule{wide, site}
		if reverseOrder {
			rules = []*LimitRule{site, wide}
		}
		if err := a.Limits(rules); err != nil {
			t.Fatalf("a.Limits: %v", err)
		}

		// Collector B stays on default first-matching admission but shares
		// the exact same wide-rule pointer.
		b := NewCollector(Async(true), IgnoreRobotsTxt(), AllowURLRevisit())
		b.SetClient(&http.Client{Transport: tr})
		if err := b.Limit(wide); err != nil {
			t.Fatalf("b.Limit: %v", err)
		}

		aCalls := []*roundTripCall{tr.register("/a1"), tr.register("/a2"), tr.register("/a3")}
		bCalls := []*roundTripCall{tr.register("/b1"), tr.register("/b2"), tr.register("/b3")}
		mustVisit(t, a, "http://a.test/a1")
		mustVisit(t, a, "http://a.test/a2")
		mustVisit(t, a, "http://a.test/a3")
		mustVisit(t, b, "http://b.test/b1")
		mustVisit(t, b, "http://b.test/b2")
		mustVisit(t, b, "http://b.test/b3")

		// Wait until an a.test download is actually admitted; until then
		// the wide-rule slots might all be held by b.test requests.
		tr.waitForEntered(t, "/a1", "/a2", "/a3")
		// The second wide-rule slot is taken by a waiting b.test request.
		waitForPeak(t, tr, 2)
		letSettle()
		if got := tr.peak.Load(); got != 2 {
			t.Fatalf("shared rule must cap combined concurrency at 2, got peak %d", got)
		}
		if got := tr.hostPeakValue("a.test"); got != 1 {
			t.Fatalf("a.test site rule must cap A's a.test downloads at 1, got %d", got)
		}

		releaseCalls(append(aCalls, bCalls...)...)
		a.Wait()
		b.Wait()

		// All three a.test downloads eventually ran, but never two at once.
		if got := tr.hostPeakValue("a.test"); got != 1 {
			t.Fatalf("a.test peak must stay 1 for the whole run, got %d", got)
		}
		for _, p := range []string{"/a1", "/a2", "/a3", "/b1", "/b2", "/b3"} {
			if got := tr.hitCount(p); got != 1 {
				t.Fatalf("path %s reached transport %d times, want 1", p, got)
			}
		}
	}

	t.Run("shared rule same order", func(t *testing.T) { run(t, false) })
	t.Run("shared rule reversed order", func(t *testing.T) { run(t, true) })

	t.Run("clone shares the all-matching backend", func(t *testing.T) {
		tr := newBlockingTransport()
		c := newAllMatchingCollector(t, tr, &LimitRule{DomainGlob: "*", Parallelism: 1})
		clone := c.Clone()
		if !clone.backend.useAllMatchingLimits.Load() {
			t.Fatal("Clone must inherit UseAllMatchingLimits from the shared backend")
		}
		if clone.backend != c.backend {
			t.Fatal("Clone must share the HTTP backend")
		}

		c1 := tr.register("/c1")
		c2 := tr.register("/c2")
		mustVisit(t, c, "http://x.test/c1")
		mustVisit(t, clone, "http://x.test/c2")
		tr.waitEnteredCount(t, 1)
		letSettle()
		if got := tr.enteredCount(); got != 1 {
			t.Fatalf("shared backend rule must admit only one download, got %d", got)
		}
		// Releasing both calls is safe: whichever request was admitted
		// finishes, and the other passes straight through when admitted.
		releaseCalls(c1, c2)
		tr.waitEnteredCount(t, 2)
		c.Wait()
		clone.Wait()
	})

	t.Run("opposite rule orders never deadlock or overshoot", func(t *testing.T) {
		hold := make(chan struct{})
		tr := &peakTransport{hold: hold}
		wide := &LimitRule{DomainGlob: "*", Parallelism: 3}
		site := &LimitRule{DomainGlob: "a.test", Parallelism: 1}

		// Two collectors share both rules but register them in opposite
		// orders, so their attempt sequences walk the gates in reverse.
		a := NewCollector(Async(true), IgnoreRobotsTxt(), AllowURLRevisit())
		a.SetClient(&http.Client{Transport: tr})
		a.UseAllMatchingLimits()
		if err := a.Limits([]*LimitRule{wide, site}); err != nil {
			t.Fatalf("a.Limits: %v", err)
		}

		b := NewCollector(Async(true), IgnoreRobotsTxt(), AllowURLRevisit())
		b.SetClient(&http.Client{Transport: tr})
		b.UseAllMatchingLimits()
		if err := b.Limits([]*LimitRule{site, wide}); err != nil {
			t.Fatalf("b.Limits: %v", err)
		}

		for i := 0; i < 40; i++ {
			mustVisit(t, a, "http://a.test/a")
			mustVisit(t, b, "http://a.test/a")
			mustVisit(t, a, "http://b.test/b")
			mustVisit(t, b, "http://c.test/c")
		}

		// Admitted requests block on the barrier until the wide rule is
		// provably full, forcing genuine overlap instead of racing through.
		deadline := time.Now().Add(5 * time.Second)
		for tr.active.Load() < 3 {
			if time.Now().After(deadline) {
				close(hold)
				t.Fatalf("wide rule never filled, active=%d peak=%d", tr.active.Load(), tr.peak.Load())
			}
			time.Sleep(time.Millisecond)
		}
		if got := tr.peak.Load(); got > 3 {
			close(hold)
			t.Fatalf("wide rule oversubscribed while filling: peak %d > 3", got)
		}
		close(hold)

		done := make(chan struct{})
		go func() {
			a.Wait()
			b.Wait()
			close(done)
		}()
		select {
		case <-done:
		case <-time.After(15 * time.Second):
			t.Fatal("collectors sharing rules in opposite orders deadlocked")
		}

		// The shared wide rule caps total concurrency at 3 and the shared
		// site rule caps a.test at 1, regardless of registration order.
		if got := tr.peak.Load(); got != 3 {
			t.Fatalf("wide rule must admit exactly its capacity, want peak 3, got %d", got)
		}
		if got := tr.hostPeakValue("a.test"); got != 1 {
			t.Fatalf("site rule must serialize a.test at 1, got peak %d", got)
		}
	})
}

// TestOverlappingLimitsRelease covers slot release: independent per-rule
// delay holds, body-read/transport/abort failures releasing exactly once,
// cancellation while waiting for admission, random-delay range, and cache
// hits taking no network slot.
func TestOverlappingLimitsRelease(t *testing.T) {
	t.Run("each rule releases after its own delay", func(t *testing.T) {
		clk := newManualClock()
		tr := newBlockingTransport()
		c := newAllMatchingCollector(t, tr,
			&LimitRule{DomainGlob: "*", Parallelism: 1, Delay: 10 * time.Millisecond},
			&LimitRule{DomainGlob: "a.test", Parallelism: 1, Delay: 30 * time.Millisecond},
		)
		c.backend.clock = clk

		a1 := tr.register("/a1")
		a2 := tr.register("/a2")
		b1 := tr.register("/b1")

		mustVisit(t, c, "http://a.test/a1")
		waitEntered(t, a1, "a1")

		// Finish a1's download. Its two leases now hold for 10ms and 30ms.
		close(a1.release)
		clk.waitTimerCount(t, 2)

		// b1 only needs the wide rule, whose 10ms hold elapses first.
		mustVisit(t, c, "http://b.test/b1")
		letSettle()
		assertNotEntered(t, b1, "b1 before the wide rule's hold elapses")
		clk.Advance(10 * time.Millisecond)
		waitEntered(t, b1, "b1 after wide-rule release")

		// a2 also needs the a.test rule, which stays held until 30ms.
		mustVisit(t, c, "http://a.test/a2")
		letSettle()
		assertNotEntered(t, a2, "a2 while the a.test rule hold is active")

		// Finish b1: its wide-rule slot is then held for another 10ms while
		// the a.test rule remains on a1's 30ms hold.
		close(b1.release)
		// Pending timers are a1's 30ms site hold plus b1's 10ms wide hold.
		clk.waitTimerCount(t, 2)
		// Advancing to the 30ms mark releases both holds a2 waits on.
		clk.Advance(20 * time.Millisecond)
		waitEntered(t, a2, "a2 after a.test-rule release")

		close(a2.release)
		// Let a2's leases' holds elapse so Wait can return.
		clk.waitTimerCount(t, 2)
		clk.Advance(30 * time.Millisecond)
		c.Wait()
	})

	t.Run("random delay keeps its original range", func(t *testing.T) {
		// Delay + RandomDelay produces a hold in [Delay, Delay+RandomDelay).
		clk := newManualClock()
		rule := &LimitRule{
			DomainGlob:  "*",
			Parallelism: 1,
			Delay:       5 * time.Millisecond,
			RandomDelay: 10 * time.Millisecond,
		}
		if err := rule.Init(); err != nil {
			t.Fatalf("Init: %v", err)
		}
		for i := 0; i < 100; i++ {
			if !rule.gate.tryAcquire() {
				t.Fatal("gate should be empty before each lease")
			}
			lease := &limitLease{gate: rule.gate, rule: rule, taken: true}
			done := make(chan struct{})
			go func() {
				lease.releaseAfterDownload(clk)
				close(done)
			}()
			clk.waitTimerCount(t, 1)
			clk.Advance(30 * time.Millisecond)
			<-done
		}
		for _, d := range clk.snapshotDurations() {
			if d < 5*time.Millisecond || d >= 15*time.Millisecond {
				t.Fatalf("delay hold %v outside [Delay, Delay+RandomDelay) = [5ms, 15ms)", d)
			}
		}

		// RandomDelay alone keeps the historical [0, RandomDelay) range.
		clk2 := newManualClock()
		rule2 := &LimitRule{DomainGlob: "*", Parallelism: 1, RandomDelay: 7 * time.Millisecond}
		if err := rule2.Init(); err != nil {
			t.Fatalf("Init: %v", err)
		}
		for i := 0; i < 50; i++ {
			if !rule2.gate.tryAcquire() {
				t.Fatal("gate should be empty before each lease")
			}
			lease := &limitLease{gate: rule2.gate, rule: rule2, taken: true}
			done := make(chan struct{})
			go func() {
				lease.releaseAfterDownload(clk2)
				close(done)
			}()
			clk2.waitTimerCount(t, 1)
			clk2.Advance(10 * time.Millisecond)
			<-done
		}
		for _, d := range clk2.snapshotDurations() {
			if d < 0 || d >= 7*time.Millisecond {
				t.Fatalf("random hold %v outside [0, RandomDelay) = [0, 7ms)", d)
			}
		}
	})

	t.Run("every failure path releases each slot exactly once", func(t *testing.T) {
		// Parallelism 2: after two failed downloads both slots must be fully
		// usable again, proving each slot was released exactly once.
		tr := newBlockingTransport()
		tr.register("/transport-error").roundTripErr = errors.New("connection reset")
		tr.register("/body-error").bodyReadErr = io.ErrUnexpectedEOF
		ok1 := tr.register("/ok1")
		ok2 := tr.register("/ok2")

		c := newAllMatchingCollector(t, tr, &LimitRule{DomainGlob: "*", Parallelism: 2})
		var errMu sync.Mutex
		var gotErrs []error
		c.OnError(func(_ *Response, err error) {
			errMu.Lock()
			gotErrs = append(gotErrs, err)
			errMu.Unlock()
		})

		mustVisit(t, c, "http://a.test/transport-error")
		mustVisit(t, c, "http://a.test/body-error")
		te := tr.call("/transport-error")
		be := tr.call("/body-error")
		waitEntered(t, te, "transport-error request")
		waitEntered(t, be, "body-error request")
		releaseCalls(te, be)
		c.Wait()
		if len(gotErrs) != 2 {
			t.Fatalf("want 2 reported errors, got %d: %v", len(gotErrs), gotErrs)
		}

		// Both slots must have been returned: two fresh downloads start.
		mustVisit(t, c, "http://a.test/ok1")
		mustVisit(t, c, "http://a.test/ok2")
		waitEntered(t, ok1, "ok1")
		waitEntered(t, ok2, "ok2")
		if got := tr.peak.Load(); got != 2 {
			t.Fatalf("want peak 2 after failed downloads released their slots, got %d", got)
		}
		releaseCalls(ok1, ok2)
		c.Wait()

		// Header abort paths also release exactly once.
		t.Run("request header abort", func(t *testing.T) {
			tr2 := newBlockingTransport()
			c2 := newAllMatchingCollector(t, tr2, &LimitRule{DomainGlob: "*", Parallelism: 1})
			// OnRequestHeaders runs inside the backend after admission has
			// taken the slot, so aborting here must release it again.
			c2.OnRequestHeaders(func(r *Request) {
				if strings.Contains(r.URL.Path, "aborted") {
					r.Abort()
				}
			})
			next := tr2.register("/next")
			mustVisit(t, c2, "http://a.test/aborted")
			c2.Wait()
			mustVisit(t, c2, "http://a.test/next")
			waitEntered(t, next, "next after request abort")
			close(next.release)
			c2.Wait()
		})

		t.Run("response header abort", func(t *testing.T) {
			tr3 := newBlockingTransport()
			c3 := newAllMatchingCollector(t, tr3, &LimitRule{DomainGlob: "*", Parallelism: 1})
			c3.OnResponseHeaders(func(r *Response) {
				if strings.Contains(r.Request.URL.Path, "aborted") {
					r.Request.Abort()
				}
			})
			aborted := tr3.register("/aborted")
			next := tr3.register("/next")
			mustVisit(t, c3, "http://a.test/aborted")
			waitEntered(t, aborted, "aborted")
			close(aborted.release)
			c3.Wait()
			mustVisit(t, c3, "http://a.test/next")
			waitEntered(t, next, "next after response header abort")
			close(next.release)
			c3.Wait()
		})
	})

	t.Run("cancellation while waiting for admission", func(t *testing.T) {
		ctx, cancel := context.WithCancel(context.Background())
		tr := newBlockingTransport()
		rule := &LimitRule{DomainGlob: "*", Parallelism: 1}
		c := NewCollector(Async(true), IgnoreRobotsTxt(), AllowURLRevisit(), StdlibContext(ctx))
		c.SetClient(&http.Client{Transport: tr})
		c.UseAllMatchingLimits()
		if err := c.Limit(rule); err != nil {
			t.Fatalf("Limit: %v", err)
		}

		var errMu sync.Mutex
		var gotErrs []error
		c.OnError(func(_ *Response, err error) {
			errMu.Lock()
			gotErrs = append(gotErrs, err)
			errMu.Unlock()
		})

		holder := tr.register("/holder")
		waiting := tr.register("/waiting")
		mustVisit(t, c, "http://a.test/holder")
		waitEntered(t, holder, "holder occupying the only slot")

		mustVisit(t, c, "http://a.test/waiting")
		letSettle()
		assertNotEntered(t, waiting, "waiting request")

		cancel()
		deadline := time.Now().Add(3 * time.Second)
		for {
			errMu.Lock()
			n := len(gotErrs)
			errMu.Unlock()
			if n == 1 {
				break
			}
			if time.Now().After(deadline) {
				t.Fatal("cancelled waiter never reported its context error")
			}
			time.Sleep(time.Millisecond)
		}
		errMu.Lock()
		err := gotErrs[0]
		errMu.Unlock()
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("want errors.Is(err, context.Canceled), got %v", err)
		}
		letSettle()
		assertNotEntered(t, waiting, "cancelled waiter must never reach the transport")
		if got := tr.hitCount("/waiting"); got != 0 {
			t.Fatalf("cancelled waiter hit the transport %d times", got)
		}

		// The holder's slot is still valid and returns normally.
		close(holder.release)
		c.Wait()

		// Nothing was over-released: a fresh collector reusing the same
		// *LimitRule pointer fills its single capacity again.
		c2 := NewCollector(Async(true), IgnoreRobotsTxt(), AllowURLRevisit())
		c2.SetClient(&http.Client{Transport: tr})
		c2.UseAllMatchingLimits()
		if err := c2.Limit(rule); err != nil {
			t.Fatalf("Limit: %v", err)
		}
		fresh := tr.register("/fresh")
		mustVisit(t, c2, "http://a.test/fresh")
		waitEntered(t, fresh, "fresh request after cancellation")
		close(fresh.release)
		c2.Wait()

		// The cancelled waiter leaked no token: the rule still admits
		// exactly one download at a time.
		second := tr.register("/second")
		mustVisit(t, c2, "http://a.test/second")
		waitEntered(t, second, "second request")
		letSettle()
		if got := tr.peak.Load(); got != 1 {
			t.Fatalf("shared rule capacity corrupted by cancellation, peak %d", got)
		}
		close(second.release)
		c2.Wait()
	})

	t.Run("cache hit takes no network slot", func(t *testing.T) {
		clk := newManualClock()
		tr := newBlockingTransport()
		c := NewCollector(Async(true), IgnoreRobotsTxt(), AllowURLRevisit(), CacheDir(t.TempDir()))
		c.SetClient(&http.Client{Transport: tr})
		c.UseAllMatchingLimits()
		if err := c.Limit(&LimitRule{DomainGlob: "*", Parallelism: 1, Delay: 50 * time.Millisecond}); err != nil {
			t.Fatalf("Limit: %v", err)
		}
		c.backend.clock = clk

		cached := tr.register("/cached")
		blocker := tr.register("/blocker")

		// Populate the cache first.
		mustVisit(t, c, "http://a.test/cached")
		waitEntered(t, cached, "initial cache fill")
		close(cached.release)
		clk.waitTimerCount(t, 1)
		clk.Advance(time.Second)
		c.Wait()

		// Occupy the only network slot with a long-running download.
		mustVisit(t, c, "http://a.test/blocker")
		waitEntered(t, blocker, "blocker")

		// A cache hit is served by Cache before Do runs admission, so it
		// completes even though every rule slot is occupied.
		scraped := make(chan struct{}, 1)
		c.OnScraped(func(*Response) { scraped <- struct{}{} })
		mustVisit(t, c, "http://a.test/cached")
		select {
		case <-scraped:
		case <-time.After(3 * time.Second):
			t.Fatal("cache hit should complete without waiting for a network slot")
		}
		if got := tr.hitCount("/cached"); got != 1 {
			t.Fatalf("cache hit must not add a network download, got %d hits", got)
		}

		close(blocker.release)
		clk.waitTimerCount(t, 1)
		clk.Advance(time.Second)
		c.Wait()
	})
}
