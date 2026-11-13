package gocrawl

import (
	"fmt"
	"net/url"
	"sort"
)

// RunSources runs a single joint crawl for several named sources and reports,
// for each source, the normalized URLs of the pages that were successfully
// visited (that is, that triggered the Visit extender hook) and that are
// attributable to that source.
//
// The seeds argument maps a source name to its seed URLs. Source names must
// be non-empty and every seed must be an absolute http or https URL,
// otherwise an error is returned before any request is made. An empty seeds
// map returns an empty result immediately, without starting a crawl.
//
// This is one joint crawl, not one Run per source: a given normalized URL is
// physically scheduled at most once and goes through the usual host worker,
// robots.txt, HEAD/GET and Visit/Visited flow, and MaxVisits keeps counting
// actually visited pages. A page is attributable to a source if it is
// reachable from that source's seeds through the links that were actually
// harvested (or the redirections that were actually followed), along edges
// allowed by the crawling policies (absolute, http(s) and same-host rules).
// Sources that discover an already-known URL still inherit its descendants,
// without any re-fetch or extra Visit call. Pages that did not trigger Visit
// (robots.txt, failed fetches, non-2xx statuses, pages dropped after a HEAD
// request, filtered-out URLs and redirect sources) are never reported.
//
// Unlike Run, the seeds are used exactly as provided: the Start hook is not
// called to rewrite them. Every other hook behaves as it does in Run, and
// End is notified exactly once when this crawl ends. The returned map holds
// an entry for every source name, with a sorted, duplicate-free list of
// normalized URLs (possibly empty). When the crawl stops early, the error
// follows Run's semantics (ErrMaxVisits, ErrInterrupted) and the map holds
// the partial results gathered so far. The next Run or RunSources starts
// from a fresh source state and never mutates a previously returned map.
func (c *Crawler) RunSources(seeds map[string][]string) (map[string][]string, error) {
	if len(seeds) == 0 {
		// No sources at all: return an empty result right away, without
		// starting a crawl or calling any hook.
		return map[string][]string{}, nil
	}

	// Validate everything before issuing any request.
	names := make([]string, 0, len(seeds))
	for name, urls := range seeds {
		if name == "" {
			return nil, fmt.Errorf("gocrawl: source names must not be empty")
		}
		for _, raw := range urls {
			u, err := url.Parse(raw)
			if err != nil {
				return nil, fmt.Errorf("gocrawl: invalid seed URL %q for source %q: %s", raw, name, err)
			}
			if !u.IsAbs() || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
				return nil, fmt.Errorf("gocrawl: seed URL %q for source %q is not an absolute http(s) URL", raw, name)
			}
		}
		names = append(names, name)
	}
	// Sort the source names so that the joint crawl enqueues seeds in a
	// deterministic order.
	sort.Strings(names)

	c.logFunc = getLogFunc(c.Options.Extender, c.Options.LogFlags, -1)

	// Build the seed contexts and attribute them to their sources. The seeds
	// are used exactly as passed in: the Start hook is intentionally not
	// called to rewrite them.
	tracker := newSourceTracker(names)
	ctxs := make([]*URLContext, 0, len(seeds))
	for _, name := range names {
		for _, raw := range seeds[name] {
			ctx, err := c.stringToURLContext(raw, nil)
			if err != nil {
				// Cannot happen, the seeds were validated above.
				return nil, fmt.Errorf("gocrawl: invalid seed URL %q for source %q: %s", raw, name, err)
			}
			tracker.addSource(ctx.normalizedURL.String(), name)
			ctxs = append(ctxs, ctx)
		}
	}

	c.init(ctxs)
	// Activate source tracking only after init, which resets it for a plain
	// Run.
	c.srcTracker = tracker

	// Start with the seeds, and loop till death, exactly like Run.
	c.enqueueUrls(ctxs)
	err := c.collectUrls()

	c.Options.Extender.End(err)
	return tracker.results(), err
}

// sourceTracker records, during a RunSources crawl, which sources each
// normalized URL is attributable to, and propagates those attributions along
// the discovery edges (harvested links and followed redirections) as they
// are found - including edges discovered after the target URL was already
// processed (late-joining sources). It is only used from the crawler's
// goroutine, so it needs no locking.
type sourceTracker struct {
	srcOf   map[string]map[string]struct{} // normalized URL -> set of source names
	kids    map[string][]string            // normalized URL -> discovery-edge children
	visited map[string]struct{}            // normalized URLs that triggered Visit
	names   []string                       // all source names, sorted
}

// newSourceTracker returns a sourceTracker ready to track the given source
// names (which must already be sorted).
func newSourceTracker(names []string) *sourceTracker {
	return &sourceTracker{
		srcOf:   make(map[string]map[string]struct{}),
		kids:    make(map[string][]string),
		visited: make(map[string]struct{}),
		names:   names,
	}
}

// addSource attributes the normalized URL to the given source, and
// propagates the attribution to everything already known to be reachable
// from it through discovery edges. Cycles and diamonds terminate because an
// attribution is added at most once per (URL, source) pair.
func (t *sourceTracker) addSource(normURL, name string) {
	// Iterative depth-first propagation, so that long link chains cannot
	// overflow the goroutine stack.
	pending := []string{normURL}
	for len(pending) > 0 {
		u := pending[len(pending)-1]
		pending = pending[:len(pending)-1]

		set := t.srcOf[u]
		if set == nil {
			set = make(map[string]struct{})
			t.srcOf[u] = set
		}
		if _, dup := set[name]; dup {
			// Already attributed: its children already received this source.
			continue
		}
		set[name] = struct{}{}
		pending = append(pending, t.kids[u]...)
	}
}

// addEdge records a discovery edge from parent to child (both normalized
// URLs) and propagates the parent's current sources to the child. Sources
// attributed to the parent later on flow through this edge via addSource.
func (t *sourceTracker) addEdge(parent, child string) {
	if parent == child {
		// A self-edge adds no attribution.
		return
	}
	for _, k := range t.kids[parent] {
		if k == child {
			// Edge already known, attributions already propagated.
			return
		}
	}
	t.kids[parent] = append(t.kids[parent], child)
	for name := range t.srcOf[parent] {
		t.addSource(child, name)
	}
}

// markVisited records that the normalized URL triggered a successful Visit.
func (t *sourceTracker) markVisited(normURL string) {
	t.visited[normURL] = struct{}{}
}

// results builds the per-source lists of normalized URLs that were
// successfully visited. Every source gets an entry, holding a sorted,
// duplicate-free, non-nil list. The returned map and slices are freshly
// allocated, so later runs cannot mutate them.
func (t *sourceTracker) results() map[string][]string {
	out := make(map[string][]string, len(t.names))
	for _, name := range t.names {
		list := make([]string, 0)
		for u, set := range t.srcOf {
			if _, ok := t.visited[u]; !ok {
				continue
			}
			if _, ok := set[name]; ok {
				list = append(list, u)
			}
		}
		sort.Strings(list)
		out[name] = list
	}
	return out
}
