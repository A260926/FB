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
	"bufio"
	"context"
	"crypto/sha1"
	"encoding/gob"
	"encoding/hex"
	"io"
	"math/rand"
	"net/http"
	"os"
	"path"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"compress/gzip"

	"github.com/gobwas/glob"
)

type httpBackend struct {
	LimitRules []*LimitRule
	Client     *http.Client
	lock       *sync.RWMutex
	// useAllMatchingLimits selects the admission strategy used by Do:
	// false (default) keeps the historical first-matching-rule behavior,
	// true makes every request acquire a slot in every matching rule.
	// The flag belongs to the shared backend, so collectors produced by
	// Collector.Clone keep the same mode.
	useAllMatchingLimits atomic.Bool
	// clock drives the Delay / RandomDelay hold; production code uses the
	// real wall clock while tests can install an advanceable fake.
	clock limitClock
}

// limitClock is the time source used when holding a LimitRule slot for its
// Delay / RandomDelay after a download. Tests substitute an advanceable
// implementation instead of relying on wall-clock sleeps.
type limitClock interface {
	Sleep(time.Duration)
}

type realClock struct{}

func (realClock) Sleep(d time.Duration) { time.Sleep(d) }

// limitGate is the admission control for a single LimitRule. It wraps the
// rule's existing token channel so that first-matching and all-matching
// admission — even when different collectors share one *LimitRule —
// always contend for the same Parallelism capacity. A request holds one
// token for the whole lifetime of its download (including the optional
// Delay / RandomDelay hold after the body has been handled) and returns
// exactly one token when the lease is released.
//
// Every rule owns an independent channel. A request blocks on a channel
// receive/send instead of a global lock, so unrelated rules are never
// serialized, and no lock is held while a network transfer is in flight.
type limitGate struct {
	tokens chan bool
}

func newLimitGate(tokens chan bool) *limitGate {
	return &limitGate{tokens: tokens}
}

// tryAcquire reserves a token when one is immediately available, without
// blocking.
func (g *limitGate) tryAcquire() bool {
	select {
	case g.tokens <- true:
		return true
	default:
		return false
	}
}

// waitAcquire blocks until a token is available or ctx is done. On
// cancellation no token is reserved and ctx.Err() (an errors.Is-recognizable
// context error) is returned, so the caller never enters the transport and
// never has to release a slot it did not get.
func (g *limitGate) waitAcquire(ctx context.Context) error {
	select {
	case g.tokens <- true:
		// Both cases may be ready together; never keep a token after the
		// context has already been cancelled.
		if err := ctx.Err(); err != nil {
			<-g.tokens
			return err
		}
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

// release hands one token back, unblocking at most one waiting request.
func (g *limitGate) release() { <-g.tokens }

// limitLease tracks one acquisition of a single rule by a single request.
// release is run at most once no matter how the download ends.
type limitLease struct {
	gate  *limitGate
	rule  *LimitRule
	taken bool
}

// release hands the token back immediately. It is used while admission is
// still in progress, where no download has happened yet and no Delay hold
// applies.
func (l *limitLease) release() {
	if !l.taken {
		return
	}
	l.taken = false
	l.gate.release()
}

// releaseAfterDownload keeps the rule's slot occupied for Delay plus a
// random fraction of RandomDelay after the download has been handled,
// preserving the pre-existing hold semantics and the [0, RandomDelay)
// range, then returns the token.
func (l *limitLease) releaseAfterDownload(clock limitClock) {
	if !l.taken {
		return
	}
	l.taken = false
	hold := l.rule.Delay
	if l.rule.RandomDelay != 0 {
		hold += time.Duration(rand.Int63n(int64(l.rule.RandomDelay)))
	}
	if hold > 0 {
		clock.Sleep(hold)
	}
	l.gate.release()
}

type checkResponseHeadersFunc func(req *http.Request, statusCode int, header http.Header) bool
type checkRequestHeadersFunc func(req *http.Request) bool

// LimitRule provides connection restrictions for domains.
// Both DomainRegexp and DomainGlob can be used to specify
// the included domains patterns, but at least one is required.
// There can be two kind of limitations:
//   - Parallelism: Set limit for the number of concurrent requests to matching domains
//   - Delay: Wait specified amount of time between requests (parallelism is 1 in this case)
type LimitRule struct {
	// DomainRegexp is a regular expression to match against domains
	DomainRegexp string
	// DomainGlob is a glob pattern to match against domains
	DomainGlob string
	// Delay is the duration to wait before creating a new request to the matching domains
	Delay time.Duration
	// RandomDelay is the extra randomized duration to wait added to Delay before creating a new request
	RandomDelay time.Duration
	// Parallelism is the number of the maximum allowed concurrent requests of the matching domains
	Parallelism int
	// waitChan keeps the historical token channel used by the default
	// first-matching-rule admission. The all-matching mode uses gate
	// instead; both represent the same Parallelism capacity.
	waitChan       chan bool
	gate           *limitGate
	compiledRegexp *regexp.Regexp
	compiledGlob   *glob.Pattern
	lock           sync.Mutex
}

// Init initializes the private members of LimitRule.
//
// Init is idempotent: once a LimitRule has been successfully initialized,
// subsequent calls are no-ops. This makes it safe to share a single
// *LimitRule across multiple Collectors via Collector.Limit — without this,
// the second Collector's Limit() call would (a) replace waitChan and
// orphan the first Collector's already-acquired slots, deadlocking its
// in-flight requests' defer <-r.waitChan, and (b) race with concurrent
// Match() reads of compiledRegexp / compiledGlob.
func (r *LimitRule) Init() error {
	r.lock.Lock()
	defer r.lock.Unlock()

	if r.waitChan != nil {
		return nil
	}
	hasPattern := false
	if r.DomainRegexp != "" {
		c, err := regexp.Compile(r.DomainRegexp)
		if err != nil {
			return err
		}
		r.compiledRegexp = c
		hasPattern = true
	}
	if r.DomainGlob != "" {
		c, err := glob.Compile(r.DomainGlob)
		if err != nil {
			return err
		}
		r.compiledGlob = c
		hasPattern = true
	}
	if !hasPattern {
		return ErrNoPattern
	}
	r.waitChan = make(chan bool, max(r.Parallelism, 1))
	// The gate drives all-matching admission on top of the same token
	// channel first-matching mode uses, so a rule shared between the two
	// modes has a single real capacity.
	r.gate = newLimitGate(r.waitChan)
	return nil
}

// Clone returns a shallow copy of the LimitRule containing only its
// exported fields. Unexported state (waitChan, gate, compiled patterns,
// lock) is left zero so the returned rule is independent of the original
// and must be re-initialized via Init before use.
func (r *LimitRule) Clone() *LimitRule {
	return &LimitRule{
		DomainRegexp: r.DomainRegexp,
		DomainGlob:   r.DomainGlob,
		Delay:        r.Delay,
		RandomDelay:  r.RandomDelay,
		Parallelism:  r.Parallelism,
	}
}

func (h *httpBackend) Init(jar http.CookieJar) {
	h.Client = &http.Client{
		Jar:     jar,
		Timeout: 10 * time.Second,
	}
	h.lock = &sync.RWMutex{}
	h.clock = realClock{}
}

// Match checks that the domain parameter triggers the rule
func (r *LimitRule) Match(domain string) bool {
	match := false
	if r.compiledRegexp != nil && r.compiledRegexp.MatchString(domain) {
		match = true
	}
	if r.compiledGlob != nil && r.compiledGlob.Match(domain) {
		match = true
	}
	return match
}

func (h *httpBackend) GetMatchingRule(domain string) *LimitRule {
	if h.LimitRules == nil {
		return nil
	}
	h.lock.RLock()
	defer h.lock.RUnlock()
	for _, r := range h.LimitRules {
		if r.Match(domain) {
			return r
		}
	}
	return nil
}

// GetMatchingRules returns every registered rule matching domain. A rule
// pointer registered more than once on the backend is returned only once;
// two distinct rules with identical configuration both count.
func (h *httpBackend) GetMatchingRules(domain string) []*LimitRule {
	if h.LimitRules == nil {
		return nil
	}
	h.lock.RLock()
	rules := make([]*LimitRule, 0, len(h.LimitRules))
	seen := make(map[*LimitRule]struct{}, len(h.LimitRules))
	for _, r := range h.LimitRules {
		if _, dup := seen[r]; dup {
			continue
		}
		if r.Match(domain) {
			seen[r] = struct{}{}
			rules = append(rules, r)
		}
	}
	h.lock.RUnlock()
	return rules
}

// acquireAll obtains one slot in every rule, or returns an errors.Is-
// recognizable context error without reserving anything.
//
// Invariants:
//   - While a goroutine is blocked waiting on a rule it holds no slot at
//     all: slots taken earlier in the attempt are returned first. So a
//     request waiting on a site-specific rule cannot occupy a slot of the
//     wider rule that another host could use, and with no hold-and-wait
//     there is no circular wait, hence no deadlock even when collectors
//     register shared rules in different orders.
//   - A slot obtained by a blocking waitAcquire is retained, never dropped
//     and retried. Every blocking wait therefore ends on a slot released
//     by an actively downloading request and makes real progress; there is
//     no acquire/release busy loop.
//   - On context cancellation nothing is held, so no release is scheduled
//     for a slot that was never acquired.
func (h *httpBackend) acquireAll(ctx context.Context, rules []*LimitRule) ([]*limitLease, error) {
	held := make(map[*LimitRule]*limitLease, len(rules))
	for len(held) < len(rules) {
		for _, r := range rules {
			if _, ok := held[r]; ok {
				continue
			}
			lease := &limitLease{gate: r.gate, rule: r}
			if lease.gate.tryAcquire() {
				lease.taken = true
				held[r] = lease
				continue
			}
			// The rule is full: hand back every slot held by this attempt
			// before blocking, so the wait occupies no slot at all.
			for _, l := range held {
				l.release()
			}
			clear(held)
			if err := r.gate.waitAcquire(ctx); err != nil {
				return nil, err
			}
			// Retain the slot the wait returned and continue with the
			// remaining rules; the next outer pass re-fills rules released
			// above.
			lease.taken = true
			held[r] = lease
		}
	}
	leases := make([]*limitLease, 0, len(rules))
	for _, r := range rules {
		leases = append(leases, held[r])
	}
	return leases, nil
}

// releaseAllAfterDownload returns every lease after the download has been
// handled. Each rule waits out its own Delay / RandomDelay independently,
// measured from the moment the download finished: a rule with a short delay
// gives its slot back without waiting for a sibling rule's longer delay.
// Each lease is released exactly once.
func (h *httpBackend) releaseAllAfterDownload(leases []*limitLease) {
	var wg sync.WaitGroup
	for _, l := range leases {
		wg.Add(1)
		go func(l *limitLease) {
			defer wg.Done()
			l.releaseAfterDownload(h.clock)
		}(l)
	}
	wg.Wait()
}

func (h *httpBackend) Cache(request *http.Request, bodySize int, checkRequestHeadersFunc checkRequestHeadersFunc, checkResponseHeadersFunc checkResponseHeadersFunc, cacheDir string, cacheExpiration time.Duration) (*Response, error) {
	if cacheDir == "" || request.Method != "GET" || request.Header.Get("Cache-Control") == "no-cache" {
		return h.Do(request, bodySize, checkRequestHeadersFunc, checkResponseHeadersFunc)
	}
	sum := sha1.Sum([]byte(request.URL.String()))
	hash := hex.EncodeToString(sum[:])
	dir := path.Join(cacheDir, hash[:2])
	filename := path.Join(dir, hash)

	if fileInfo, err := os.Stat(filename); err == nil && cacheExpiration > 0 {
		if time.Since(fileInfo.ModTime()) > cacheExpiration {
			_ = os.Remove(filename)
		}
	}

	if file, err := os.Open(filename); err == nil {
		resp := new(Response)
		err := gob.NewDecoder(file).Decode(resp)
		file.Close()
		checkResponseHeadersFunc(request, resp.StatusCode, *resp.Headers)
		if resp.StatusCode < 500 {
			return resp, err
		}
	}
	resp, err := h.Do(request, bodySize, checkRequestHeadersFunc, checkResponseHeadersFunc)
	if err != nil || resp.StatusCode >= 500 {
		return resp, err
	}
	if _, err := os.Stat(dir); err != nil {
		if err := os.MkdirAll(dir, 0750); err != nil {
			return resp, err
		}
	}
	file, err := os.Create(filename + "~")
	if err != nil {
		return resp, err
	}
	if err := gob.NewEncoder(file).Encode(resp); err != nil {
		file.Close()
		return resp, err
	}
	file.Close()
	return resp, os.Rename(filename+"~", filename)
}

func (h *httpBackend) Do(request *http.Request, bodySize int, checkRequestHeadersFunc checkRequestHeadersFunc, checkResponseHeadersFunc checkResponseHeadersFunc) (*Response, error) {
	if h.useAllMatchingLimits.Load() {
		// All-matching mode: hold a slot in every rule matching the
		// original request host for the whole download. A request with no
		// matching rule is sent straight to the transport.
		rules := h.GetMatchingRules(request.URL.Host)
		if len(rules) > 0 {
			leases, err := h.acquireAll(request.Context(), rules)
			if err != nil {
				// Admission was cancelled: never reach the transport and
				// nothing was acquired, so there is nothing to release.
				return nil, err
			}
			// A single defer covers normal completion, transport errors,
			// body read errors and both header abort paths; every lease
			// releases exactly once.
			defer h.releaseAllAfterDownload(leases)
		}
	} else {
		// Default first-matching-rule mode, unchanged apart from the
		// clock used for the delay hold.
		r := h.GetMatchingRule(request.URL.Host)
		if r != nil {
			r.waitChan <- true
			defer func(r *LimitRule) {
				randomDelay := time.Duration(0)
				if r.RandomDelay != 0 {
					randomDelay = time.Duration(rand.Int63n(int64(r.RandomDelay)))
				}
				h.clock.Sleep(r.Delay + randomDelay)
				<-r.waitChan
			}(r)
		}
	}
	if !checkRequestHeadersFunc(request) {
		return nil, ErrAbortedBeforeRequest
	}
	res, err := h.Client.Do(request)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()

	finalRequest := request
	if res.Request != nil {
		finalRequest = res.Request
	}
	if !checkResponseHeadersFunc(finalRequest, res.StatusCode, res.Header) {
		// closing res.Body (see defer above) without reading it aborts
		// the download
		return nil, ErrAbortedAfterHeaders
	}

	var bodyReader io.Reader = res.Body
	if bodySize > 0 {
		bodyReader = io.LimitReader(bodyReader, int64(bodySize))
	}
	contentEncoding := strings.ToLower(res.Header.Get("Content-Encoding"))
	if !res.Uncompressed && (strings.Contains(contentEncoding, "gzip") || (contentEncoding == "" && strings.Contains(strings.ToLower(res.Header.Get("Content-Type")), "gzip")) || (strings.HasSuffix(strings.ToLower(finalRequest.URL.Path), ".xml.gz") && res.StatusCode >= 200 && res.StatusCode < 300)) {
		// Even if URL contains .xml.gz, it doesn't mean that we get gzip
		// compressed data back. We might get 404 error page instead,
		// for example. So check gzip magic bytes.
		bufReader := bufio.NewReader(bodyReader)
		bodyReader = bufReader
		magic, err := bufReader.Peek(2)
		switch err {
		case io.EOF:
			// less than 2 bytes, do nothing
		case nil:
			// gzip magic, as specified in RFC 1952
			if magic[0] == 0x1f && magic[1] == 0x8b {
				bodyReader, err = gzip.NewReader(bufReader)
				if err != nil {
					return nil, err
				}
				defer bodyReader.(*gzip.Reader).Close()
			}
		default:
			return nil, err
		}
	}
	body, err := io.ReadAll(bodyReader)
	if err != nil {
		return nil, err
	}
	return &Response{
		StatusCode: res.StatusCode,
		Body:       body,
		Headers:    &res.Header,
	}, nil
}

func (h *httpBackend) Limit(rule *LimitRule) error {
	h.lock.Lock()
	if h.LimitRules == nil {
		h.LimitRules = make([]*LimitRule, 0, 8)
	}
	h.LimitRules = append(h.LimitRules, rule)
	h.lock.Unlock()
	return rule.Init()
}

func (h *httpBackend) Limits(rules []*LimitRule) error {
	for _, r := range rules {
		if err := h.Limit(r); err != nil {
			return err
		}
	}
	return nil
}
