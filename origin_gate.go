// Copyright IBM Corp. 2015, 2025
// SPDX-License-Identifier: MPL-2.0

package retryablehttp

import (
	"context"
	"errors"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

// GateClock abstracts the time operations used by an OriginGate so that tests
// can drive scheduling deterministically. A nil GateClock passed to
// NewOriginGate selects a real-time implementation.
type GateClock interface {
	// Now returns the current wall-clock time.
	Now() time.Time

	// Wait blocks for d or until ctx is done. It returns ctx.Err() (or
	// another error) when the wait is interrupted before d elapses.
	Wait(ctx context.Context, d time.Duration) error
}

// gateRoundTripper wraps the configured http.Client's transport so the gate
// is consulted before every individual RoundTrip: the initial attempt, retry
// attempts and each redirect hop followed by the standard library. The
// wrapped transport and the http.Client's other behavior are left untouched.
type gateRoundTripper struct {
	gate *OriginGate
	next http.RoundTripper
}

func (g *gateRoundTripper) RoundTrip(req *http.Request) (*http.Response, error) {
	if err := g.gate.Wait(req.Context(), req); err != nil {
		return nil, err
	}
	resp, err := g.next.RoundTrip(req)
	if err == nil {
		g.gate.Observe(resp)
	}
	return resp, err
}

// gateTransport returns a transport that enforces the gate around each
// RoundTrip, or the client's configured transport unchanged when no gate is
// set. The caller's http.Client is never modified.
func (c *Client) gateTransport() http.RoundTripper {
	transport := c.HTTPClient.Transport
	if transport == nil {
		transport = http.DefaultTransport
	}
	if c.OriginGate == nil {
		return transport
	}
	return &gateRoundTripper{gate: c.OriginGate, next: transport}
}

// systemClock is the GateClock used when NewOriginGate receives a nil clock.
type systemClock struct{}

func (systemClock) Now() time.Time { return time.Now() }

func (systemClock) Wait(ctx context.Context, d time.Duration) error {
	if d <= 0 {
		// Honor an already canceled context even when there is nothing to wait for.
		return ctx.Err()
	}
	timer := time.NewTimer(d)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return nil
	}
}

// OriginGate coordinates per-origin pauses learned from Retry-After response
// headers. Requests that share a gate honor the same pause for an origin, so a
// Retry-After received by one request delays every other request headed for the
// same origin, including requests belonging to different Clients.
//
// A gate keeps no global state: pauses recorded on one gate are invisible to
// any other gate. The zero value is not usable; create one with NewOriginGate.
type OriginGate struct {
	clock    GateClock
	maxDelay time.Duration

	mu       sync.Mutex
	deadline map[string]time.Time
}

// NewOriginGate creates an OriginGate. maxDelay caps the waiting time a single
// Retry-After hint may contribute and must be greater than zero. A nil clock
// selects the real system clock.
func NewOriginGate(maxDelay time.Duration, clock GateClock) (*OriginGate, error) {
	if maxDelay <= 0 {
		return nil, errors.New("retryablehttp: NewOriginGate maxDelay must be greater than zero")
	}
	if clock == nil {
		clock = systemClock{}
	}
	return &OriginGate{
		clock:    clock,
		maxDelay: maxDelay,
		deadline: make(map[string]time.Time),
	}, nil
}

// originKey builds the map key for an outgoing request from its scheme,
// lower-cased host name and effective port. Userinfo, path and query never
// participate, and the default ports for http (80) and https (443) collapse
// onto their omitted-port forms. Different schemes are never merged.
func originKey(u *url.URL) string {
	host := u.Hostname()
	if host == "" {
		return ""
	}
	host = strings.ToLower(host)

	port := u.Port()
	if port == "" {
		switch strings.ToLower(u.Scheme) {
		case "http":
			port = "80"
		case "https":
			port = "443"
		}
	}

	scheme := strings.ToLower(u.Scheme)
	if port == "" {
		return scheme + "://" + host
	}
	return scheme + "://" + host + ":" + port
}

// Wait blocks until any recorded pause for the request origin has elapsed or
// ctx is canceled. The shared deadline is re-read after every wake-up, so a
// pause extended while waiting extends this call as well. The gate lock is
// never held while waiting, and requests for other origins proceed normally.
//
// A non-nil error is returned unwrapped from the clock (so errors.Is keeps
// working) and the underlying transport must not be entered in that case.
func (g *OriginGate) Wait(ctx context.Context, req *http.Request) error {
	if g == nil || req == nil || req.URL == nil {
		return nil
	}
	key := originKey(req.URL)
	if key == "" {
		return nil
	}

	for {
		g.mu.Lock()
		deadline, ok := g.deadline[key]
		g.mu.Unlock()

		now := g.clock.Now()
		if !ok || !deadline.After(now) {
			return nil
		}

		if err := g.clock.Wait(ctx, deadline.Sub(now)); err != nil {
			return err
		}
		// Re-read the shared deadline before sending: another request may
		// have extended the pause while this one was waiting.
	}
}

// Observe records any Retry-After hints carried by a 429 or 503 response.
// Every distinct header field value may contribute a hint; commas inside HTTP
// dates are field content, not value separators. Only the latest valid hint
// matters, hints are measured relative to the response arrival time, and a
// single hint can never extend the deadline by more than maxDelay. New hints
// only ever push the recorded deadline later: older responses, shorter hints
// and successful responses can never shorten it. Responses with any other
// status code leave the gate untouched, even when they carry the header.
func (g *OriginGate) Observe(resp *http.Response) {
	if g == nil || resp == nil || resp.Request == nil || resp.Request.URL == nil {
		return
	}
	switch resp.StatusCode {
	case http.StatusTooManyRequests, http.StatusServiceUnavailable:
	default:
		return
	}

	key := originKey(resp.Request.URL)
	if key == "" {
		return
	}
	now := g.clock.Now()

	var latest time.Time
	for _, value := range resp.Header[http.CanonicalHeaderKey("Retry-After")] {
		if hint, ok := parseGateRetryAfter(value, now, g.maxDelay); ok && hint.After(latest) {
			latest = hint
		}
	}
	if latest.IsZero() {
		return
	}

	g.mu.Lock()
	if current := g.deadline[key]; latest.After(current) {
		g.deadline[key] = latest
	}
	g.mu.Unlock()
}

// parseGateRetryAfter interprets one Retry-After field value relative to now.
// Both the unsigned decimal seconds form and the HTTP-date form accepted by
// http.ParseTime are supported. The returned time is capped at now+maxDelay,
// so an oversized seconds value can never overflow into the past. Zero,
// already-past and invalid values contribute nothing.
func parseGateRetryAfter(value string, now time.Time, maxDelay time.Duration) (time.Time, bool) {
	value = strings.TrimSpace(value)
	if value == "" {
		return time.Time{}, false
	}

	// Retry-After: 120 — unsigned decimal seconds. strconv.ParseUint rejects
	// signs and whitespace; compare against the cap before any conversion so
	// an oversized value cannot wrap a time.Duration into the past.
	if seconds, err := strconv.ParseUint(value, 10, 64); err == nil {
		if seconds == 0 {
			return time.Time{}, false
		}
		capAt := now.Add(maxDelay)
		maxSeconds := uint64(maxDelay / time.Second)
		hint := capAt
		if seconds <= maxSeconds {
			hint = now.Add(time.Duration(seconds) * time.Second)
			if hint.After(capAt) {
				hint = capAt
			}
		}
		if !hint.After(now) {
			return time.Time{}, false
		}
		return hint, true
	}

	// Retry-After: Fri, 31 Dec 1999 23:59:59 GMT — commas belong to the date.
	date, err := http.ParseTime(value)
	if err != nil {
		return time.Time{}, false
	}
	if !date.After(now) {
		return time.Time{}, false
	}
	if cap := now.Add(maxDelay); date.After(cap) {
		date = cap
	}
	return date, true
}
