package gocrawl

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"reflect"
	"sort"
	"sync"
	"testing"
	"time"

	"github.com/PuerkitoBio/goquery"
)

// signalGate is a channel-based gate that can be closed (released) at most
// once and waited upon with an escape hatch, so that a misbehaving crawl
// cannot hang the test forever.
type signalGate struct {
	ch   chan struct{}
	once sync.Once
}

func newSignalGate() *signalGate {
	return &signalGate{ch: make(chan struct{})}
}

// release closes the gate, idempotently.
func (g *signalGate) release() {
	g.once.Do(func() { close(g.ch) })
}

// wait blocks until the gate is released or the crawl is aborted. It reports
// whether the gate was released.
func (g *signalGate) wait(abort <-chan struct{}) bool {
	select {
	case <-g.ch:
		return true
	case <-abort:
		return false
	}
}

// requestCounter counts the requests received by a test server, keyed by
// "METHOD /path".
type requestCounter struct {
	mu     sync.Mutex
	counts map[string]int
}

func newRequestCounter() *requestCounter {
	return &requestCounter{counts: make(map[string]int)}
}

func (rc *requestCounter) handler(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		rc.mu.Lock()
		rc.counts[r.Method+" "+r.URL.Path]++
		rc.mu.Unlock()
		next.ServeHTTP(w, r)
	})
}

func (rc *requestCounter) get(key string) int {
	rc.mu.Lock()
	defer rc.mu.Unlock()
	return rc.counts[key]
}

func (rc *requestCounter) total() int {
	rc.mu.Lock()
	defer rc.mu.Unlock()
	n := 0
	for _, c := range rc.counts {
		n += c
	}
	return n
}

// sourcesExtender is the Extender used by the RunSources tests. It serves
// robots.txt from cache (so no robots.txt request hits the servers), counts
// Visit and Visited calls per normalized URL, records errors and Start/End
// calls, and forwards Enqueued/Visited notifications to optional hooks used
// to orchestrate the crawl.
type sourcesExtender struct {
	DefaultExtender

	mu      sync.Mutex
	visits  map[string]int
	visited map[string]int
	errs    []*CrawlError
	starts  int
	ends    int

	onEnqueued func(*URLContext)
	onVisited  func(*URLContext)
}

func newSourcesExtender() *sourcesExtender {
	return &sourcesExtender{
		visits:  make(map[string]int),
		visited: make(map[string]int),
	}
}

func (x *sourcesExtender) RequestRobots(*URLContext, string) ([]byte, bool) {
	return []byte("User-agent: *\nAllow: /\n"), false
}

func (x *sourcesExtender) Start(seeds interface{}) interface{} {
	x.mu.Lock()
	x.starts++
	x.mu.Unlock()
	return seeds
}

func (x *sourcesExtender) End(error) {
	x.mu.Lock()
	x.ends++
	x.mu.Unlock()
}

func (x *sourcesExtender) Error(err *CrawlError) {
	x.mu.Lock()
	x.errs = append(x.errs, err)
	x.mu.Unlock()
}

func (x *sourcesExtender) Visit(ctx *URLContext, res *http.Response, doc *goquery.Document) (interface{}, bool) {
	x.mu.Lock()
	x.visits[ctx.NormalizedURL().String()]++
	x.mu.Unlock()
	// Let gocrawl harvest the links of the page.
	return nil, true
}

func (x *sourcesExtender) Visited(ctx *URLContext, harvested interface{}) {
	x.mu.Lock()
	x.visited[ctx.NormalizedURL().String()]++
	x.mu.Unlock()
	if x.onVisited != nil {
		x.onVisited(ctx)
	}
}

func (x *sourcesExtender) Enqueued(ctx *URLContext) {
	if x.onEnqueued != nil {
		x.onEnqueued(ctx)
	}
}

func (x *sourcesExtender) visitCount(u string) int {
	x.mu.Lock()
	defer x.mu.Unlock()
	return x.visits[u]
}

func (x *sourcesExtender) hookCounts() (starts, ends, errs int) {
	x.mu.Lock()
	defer x.mu.Unlock()
	return x.starts, x.ends, len(x.errs)
}

// runSources runs RunSources in a goroutine and fails the test if the crawl
// does not finish within a generous delay, closing abort so that gated
// handlers can unblock and the deferred server Close calls can proceed.
func runSources(t *testing.T, c *Crawler, abort chan struct{}, seeds map[string][]string) (map[string][]string, error) {
	t.Helper()
	type outcome struct {
		res map[string][]string
		err error
	}
	done := make(chan outcome, 1)
	go func() {
		res, err := c.RunSources(seeds)
		done <- outcome{res, err}
	}()
	select {
	case o := <-done:
		return o.res, o.err
	case <-time.After(30 * time.Second):
		close(abort)
		select {
		case <-done:
		case <-time.After(10 * time.Second):
		}
		t.Fatal("crawl did not finish in time")
		return nil, nil
	}
}

// assertSourcesEqual compares a RunSources result with the expected map,
// sorting the expected lists first (RunSources returns sorted lists).
func assertSourcesEqual(t *testing.T, got, want map[string][]string) {
	t.Helper()
	for _, list := range want {
		sort.Strings(list)
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("unexpected sources result:\n got: %v\nwant: %v", got, want)
	}
}

func copySources(src map[string][]string) map[string][]string {
	out := make(map[string][]string, len(src))
	for k, v := range src {
		cp := make([]string, len(v))
		copy(cp, v)
		out[k] = cp
	}
	return out
}

func assertRequestCounts(t *testing.T, rc *requestCounter, want map[string]int) {
	t.Helper()
	for k, w := range want {
		if got := rc.get(k); got != w {
			t.Errorf("request count %q: got %d, want %d", k, got, w)
		}
	}
}

func assertVisitCounts(t *testing.T, ext *sourcesExtender, want map[string]int) {
	t.Helper()
	for u, w := range want {
		if got := ext.visitCount(u); got != w {
			t.Errorf("Visit count for %s: got %d, want %d", u, got, w)
		}
	}
}

func newSourcesCrawler(ext *sourcesExtender) *Crawler {
	opts := NewOptions(ext)
	opts.CrawlDelay = 0
	opts.SameHostOnly = false
	opts.LogFlags = LogNone
	return NewCrawlerWithOptions(opts)
}

// TestSourcesLateJoin checks that sources discovering an already-known URL
// still get credited with it and its whole subtree - without any re-fetch or
// extra Visit call - whether they arrive while that URL is still in flight
// or after its subtree has been fully crawled.
//
// Two loopback servers are used: source A, B and C seeds live on server 1,
// the shared subtree (shared -> leaf, plus bonly) lives on server 2. Channels
// orchestrate the late arrivals:
//
//	/a (srv1) -> /shared (srv2)            [source A]
//	/b (srv1) -> /shared, /bonly (srv2)    [source B, discovers shared while in flight]
//	/c (srv1) -> /shared (srv2)            [source C, discovers shared after leaf was visited]
//	/shared (srv2) -> /leaf (srv2)
func TestSourcesLateJoin(t *testing.T) {
	abort := make(chan struct{})

	sharedStarted := newSignalGate() // /shared fetch started (still blocked)
	releaseShared := newSignalGate() // allow /shared to respond
	bonlyEnqueued := newSignalGate() // /bonly enqueued, i.e. /b's response processed
	leafVisited := newSignalGate()   // /leaf visited

	rc1, rc2 := newRequestCounter(), newRequestCounter()
	var srv1, srv2 *httptest.Server

	mux1 := http.NewServeMux()
	mux1.HandleFunc("/a", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<a href="%s/shared">shared</a>`, srv2.URL)
	})
	mux1.HandleFunc("/b", func(w http.ResponseWriter, r *http.Request) {
		// B discovers shared only once shared's fetch has started, i.e.
		// while shared is still in flight.
		if !sharedStarted.wait(abort) {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<a href="%[1]s/shared">shared</a><a href="%[1]s/bonly">bonly</a>`, srv2.URL)
	})
	mux1.HandleFunc("/c", func(w http.ResponseWriter, r *http.Request) {
		// C discovers shared only after leaf was visited, i.e. after the
		// whole subtree under shared completed.
		if !leafVisited.wait(abort) {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<a href="%s/shared">shared</a>`, srv2.URL)
	})

	mux2 := http.NewServeMux()
	mux2.HandleFunc("/shared", func(w http.ResponseWriter, r *http.Request) {
		sharedStarted.release()
		if !releaseShared.wait(abort) {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<a href="%s/leaf">leaf</a>`, srv2.URL)
	})
	mux2.HandleFunc("/leaf", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, `leaf`)
	})
	mux2.HandleFunc("/bonly", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, `bonly`)
	})

	srv1 = httptest.NewServer(rc1.handler(mux1))
	defer srv1.Close()
	srv2 = httptest.NewServer(rc2.handler(mux2))
	defer srv2.Close()

	ext := newSourcesExtender()
	ext.onEnqueued = func(ctx *URLContext) {
		// bonly is enqueued while the crawler processes b's response, so
		// when this fires the b -> shared edge is already recorded and
		// shared can be allowed to complete.
		if ctx.NormalizedURL().String() == srv2.URL+"/bonly" {
			bonlyEnqueued.release()
		}
	}
	ext.onVisited = func(ctx *URLContext) {
		if ctx.NormalizedURL().String() == srv2.URL+"/leaf" {
			leafVisited.release()
		}
	}
	go func() {
		bonlyEnqueued.wait(abort)
		releaseShared.release()
	}()

	c := newSourcesCrawler(ext)
	res, err := runSources(t, c, abort, map[string][]string{
		"A": {srv1.URL + "/a"},
		"B": {srv1.URL + "/b"},
		"C": {srv1.URL + "/c"},
	})
	if err != nil {
		t.Fatalf("RunSources returned error: %v", err)
	}

	// Both late-joining sources inherit shared and its descendant leaf, on
	// top of their own seed; B also gets its private page bonly.
	assertSourcesEqual(t, res, map[string][]string{
		"A": {srv1.URL + "/a", srv2.URL + "/shared", srv2.URL + "/leaf"},
		"B": {srv1.URL + "/b", srv2.URL + "/bonly", srv2.URL + "/shared", srv2.URL + "/leaf"},
		"C": {srv1.URL + "/c", srv2.URL + "/shared", srv2.URL + "/leaf"},
	})

	// The joint crawl physically fetched every page exactly once.
	assertRequestCounts(t, rc1, map[string]int{"GET /a": 1, "GET /b": 1, "GET /c": 1})
	assertRequestCounts(t, rc2, map[string]int{"GET /shared": 1, "GET /leaf": 1, "GET /bonly": 1})
	if got := rc1.total() + rc2.total(); got != 6 {
		t.Errorf("total request count: got %d, want 6", got)
	}

	// ...and every page triggered Visit exactly once.
	assertVisitCounts(t, ext, map[string]int{
		srv1.URL + "/a":      1,
		srv1.URL + "/b":      1,
		srv1.URL + "/c":      1,
		srv2.URL + "/shared": 1,
		srv2.URL + "/leaf":   1,
		srv2.URL + "/bonly":  1,
	})

	// RunSources does not call Start, notifies End exactly once, and no
	// crawl error occurred.
	if starts, ends, errs := ext.hookCounts(); starts != 0 || ends != 1 || errs != 0 {
		t.Errorf("hook counts: got starts=%d ends=%d errs=%d, want 0/1/0", starts, ends, errs)
	}
}

// TestSourcesRedirect checks that a redirection records the edge from the
// page that actually triggers the redirect to the redirect target - even
// though cloneForRedirect keeps the original SourceURL - and that the
// redirect source itself, which never triggers Visit, is not reported.
//
//	/home (srv1) -> /r (srv1)              [source R1 seed]
//	/r (srv1) -- 302 --> /target (srv2)    [source R2 seeds /r directly]
//	/target (srv2) -> /child (srv2)
//
// /r is attributable to R1 (via /home) and R2 (seed). If the redirect edge
// were guessed from SourceURL (which points at /home), the target would only
// be credited to R1; the real /r -> /target edge credits it to both.
func TestSourcesRedirect(t *testing.T) {
	abort := make(chan struct{})

	rc1, rc2 := newRequestCounter(), newRequestCounter()
	var srv1, srv2 *httptest.Server

	mux1 := http.NewServeMux()
	mux1.HandleFunc("/home", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<a href="%s/r">r</a>`, srv1.URL)
	})
	mux1.HandleFunc("/r", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, srv2.URL+"/target", http.StatusFound)
	})

	mux2 := http.NewServeMux()
	mux2.HandleFunc("/target", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<a href="%s/child">child</a>`, srv2.URL)
	})
	mux2.HandleFunc("/child", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, `child`)
	})

	srv1 = httptest.NewServer(rc1.handler(mux1))
	defer srv1.Close()
	srv2 = httptest.NewServer(rc2.handler(mux2))
	defer srv2.Close()

	ext := newSourcesExtender()
	c := newSourcesCrawler(ext)
	res, err := runSources(t, c, abort, map[string][]string{
		"R1": {srv1.URL + "/home"},
		"R2": {srv1.URL + "/r"},
	})
	if err != nil {
		t.Fatalf("RunSources returned error: %v", err)
	}

	// The redirect source /r never triggered Visit, so it is reported for
	// nobody; its sources flow to the redirect target and beyond.
	assertSourcesEqual(t, res, map[string][]string{
		"R1": {srv1.URL + "/home", srv2.URL + "/target", srv2.URL + "/child"},
		"R2": {srv2.URL + "/target", srv2.URL + "/child"},
	})

	// /r was fetched once (the 302), never visited.
	assertRequestCounts(t, rc1, map[string]int{"GET /home": 1, "GET /r": 1})
	assertRequestCounts(t, rc2, map[string]int{"GET /target": 1, "GET /child": 1})
	assertVisitCounts(t, ext, map[string]int{
		srv1.URL + "/home":   1,
		srv1.URL + "/r":      0,
		srv2.URL + "/target": 1,
		srv2.URL + "/child":  1,
	})

	if starts, ends, errs := ext.hookCounts(); starts != 0 || ends != 1 || errs != 0 {
		t.Errorf("hook counts: got starts=%d ends=%d errs=%d, want 0/1/0", starts, ends, errs)
	}
}

// TestSourcesIsolation checks that a joint crawl with a shared closed loop
// (p <-> q, seeded by two different sources) and a diamond (d -> e,f -> g)
// terminates and attributes correctly, that consecutive RunSources/Run calls
// on the same crawler each start from an empty source state without mutating
// previously returned results, and that invalid inputs are rejected before
// any request is made.
func TestSourcesIsolation(t *testing.T) {
	abort := make(chan struct{})

	rc1, rc2 := newRequestCounter(), newRequestCounter()
	var srv1, srv2 *httptest.Server

	mux1 := http.NewServeMux()
	mux1.HandleFunc("/p", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<a href="%s/q">q</a>`, srv2.URL)
	})
	mux1.HandleFunc("/d", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<a href="%[1]s/e">e</a><a href="%[1]s/f">f</a>`, srv1.URL)
	})
	mux1.HandleFunc("/e", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<a href="%s/g">g</a>`, srv2.URL)
	})
	mux1.HandleFunc("/f", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<a href="%s/g">g</a>`, srv2.URL)
	})

	mux2 := http.NewServeMux()
	mux2.HandleFunc("/q", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<a href="%s/p">p</a>`, srv1.URL)
	})
	mux2.HandleFunc("/g", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, `g`)
	})

	srv1 = httptest.NewServer(rc1.handler(mux1))
	defer srv1.Close()
	srv2 = httptest.NewServer(rc2.handler(mux2))
	defer srv2.Close()

	ext := newSourcesExtender()
	c := newSourcesCrawler(ext)

	// Invalid inputs are rejected before any request is made.
	invalid := []map[string][]string{
		{"": {srv1.URL + "/p"}},        // empty source name
		{"X": {"not-an-absolute-url"}}, // relative URL
		{"X": {"//example.com/x"}},     // scheme-relative URL
		{"X": {"ftp://example.com/x"}}, // non http(s) scheme
		{"X": {"http://"}},             // missing host
	}
	for i, seeds := range invalid {
		res, err := c.RunSources(seeds)
		if err == nil {
			t.Errorf("invalid case %d: got nil error, want non-nil (res=%v)", i, res)
		}
		if res != nil {
			t.Errorf("invalid case %d: got result %v, want nil", i, res)
		}
	}
	if got := rc1.total() + rc2.total(); got != 0 {
		t.Errorf("invalid inputs triggered %d requests, want 0", got)
	}

	// An empty seeds map returns an empty result immediately; a source with
	// no seed URL keeps an entry with an empty list.
	res, err := c.RunSources(map[string][]string{})
	if err != nil {
		t.Errorf("empty seeds: got error %v, want nil", err)
	}
	if len(res) != 0 {
		t.Errorf("empty seeds: got result %v, want empty map", res)
	}
	res, err = runSources(t, c, abort, map[string][]string{"EMPTY": {}})
	if err != nil {
		t.Errorf("seedless source: got error %v, want nil", err)
	}
	assertSourcesEqual(t, res, map[string][]string{"EMPTY": []string{}})

	// First joint crawl: shared closed loop (p <-> q) plus diamond
	// (d -> e,f -> g). Cycles and diamonds must terminate, and every page
	// must be fetched exactly once.
	res1, err := runSources(t, c, abort, map[string][]string{
		"S1": {srv1.URL + "/p"},
		"S2": {srv2.URL + "/q"},
		"D":  {srv1.URL + "/d"},
	})
	if err != nil {
		t.Fatalf("RunSources #1 returned error: %v", err)
	}
	assertSourcesEqual(t, res1, map[string][]string{
		"S1": {srv1.URL + "/p", srv2.URL + "/q"},
		"S2": {srv1.URL + "/p", srv2.URL + "/q"},
		"D":  {srv1.URL + "/d", srv1.URL + "/e", srv1.URL + "/f", srv2.URL + "/g"},
	})
	assertRequestCounts(t, rc1, map[string]int{"GET /p": 1, "GET /d": 1, "GET /e": 1, "GET /f": 1})
	assertRequestCounts(t, rc2, map[string]int{"GET /q": 1, "GET /g": 1})
	snapshot1 := copySources(res1)

	// A second RunSources on the same crawler starts from an empty source
	// state: it re-fetches (fresh crawl) and only knows about its own
	// sources, and it must not mutate the first result.
	res2, err := runSources(t, c, abort, map[string][]string{
		"S3": {srv2.URL + "/q"},
	})
	if err != nil {
		t.Fatalf("RunSources #2 returned error: %v", err)
	}
	assertSourcesEqual(t, res2, map[string][]string{
		"S3": {srv1.URL + "/p", srv2.URL + "/q"},
	})
	assertRequestCounts(t, rc1, map[string]int{"GET /p": 2, "GET /d": 1, "GET /e": 1, "GET /f": 1})
	assertRequestCounts(t, rc2, map[string]int{"GET /q": 2, "GET /g": 1})
	if !reflect.DeepEqual(res1, snapshot1) {
		t.Errorf("first result mutated by second run:\n got: %v\nwant: %v", res1, snapshot1)
	}

	// A plain Run still works with all its hooks (Start included) and does
	// not touch previously returned source results.
	if err := c.Run(srv1.URL + "/d"); err != nil {
		t.Fatalf("Run returned error: %v", err)
	}
	assertRequestCounts(t, rc1, map[string]int{"GET /p": 2, "GET /d": 2, "GET /e": 2, "GET /f": 2})
	assertRequestCounts(t, rc2, map[string]int{"GET /q": 2, "GET /g": 2})
	if !reflect.DeepEqual(res1, snapshot1) {
		t.Errorf("first result mutated by Run:\n got: %v\nwant: %v", res1, snapshot1)
	}

	// RunSources never called Start; End was notified once per crawl
	// (seedless source, RunSources x2), and Run called both once.
	if starts, ends, errs := ext.hookCounts(); starts != 1 || ends != 4 || errs != 0 {
		t.Errorf("hook counts: got starts=%d ends=%d errs=%d, want 1/4/0", starts, ends, errs)
	}

	// MaxVisits keeps counting actually visited pages, and RunSources
	// returns the partial results along with ErrMaxVisits.
	extMax := newSourcesExtender()
	optsMax := NewOptions(extMax)
	optsMax.CrawlDelay = 0
	optsMax.SameHostOnly = false
	optsMax.LogFlags = LogNone
	optsMax.MaxVisits = 1
	cMax := NewCrawlerWithOptions(optsMax)
	resMax, err := runSources(t, cMax, abort, map[string][]string{"M": {srv1.URL + "/d"}})
	if err != ErrMaxVisits {
		t.Errorf("MaxVisits crawl: got error %v, want ErrMaxVisits", err)
	}
	assertSourcesEqual(t, resMax, map[string][]string{"M": {srv1.URL + "/d"}})
}
