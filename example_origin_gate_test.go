// Copyright IBM Corp. 2015, 2025
// SPDX-License-Identifier: MPL-2.0

package retryablehttp

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// exampleTransport answers the first request to an origin with 429 and a
// Retry-After header and every later request with 200, so the example needs
// no real network listener.
type exampleTransport struct {
	throttled map[string]bool
}

func (t *exampleTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	code := http.StatusOK
	header := make(http.Header)
	if !t.throttled[req.URL.Host] {
		t.throttled[req.URL.Host] = true
		code = http.StatusServiceUnavailable
		header.Set("Retry-After", "1")
	}
	return &http.Response{
		StatusCode: code,
		Header:     header,
		Body:       io.NopCloser(strings.NewReader("example body")),
		Request:    req,
	}, nil
}

// ExampleNewOriginGate shows how one OriginGate is shared by several clients
// so that a Retry-After pause learned by one request delays every other
// request headed for the same origin. Clients that are not given the gate are
// completely unaffected.
func ExampleNewOriginGate() {
	// A single hint may pause an origin for at most one minute. A nil clock
	// means the real wall clock is used.
	gate, err := NewOriginGate(time.Minute, nil)
	if err != nil {
		fmt.Println("gate error:", err)
		return
	}

	transport := &exampleTransport{throttled: make(map[string]bool)}

	// Any number of Clients can share the same gate, including clients with
	// different retry settings.
	collectorA := NewClient()
	collectorA.Logger = nil
	collectorA.HTTPClient = &http.Client{Transport: transport}
	collectorA.RetryMax = 2
	collectorA.OriginGate = gate

	collectorB := NewClient()
	collectorB.Logger = nil
	collectorB.HTTPClient = &http.Client{Transport: transport}
	collectorB.RetryMax = 0
	collectorB.OriginGate = gate

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	// The first request receives 503 Retry-After: 1; its retry, and every
	// other request sharing the gate for that origin, waits until the pause
	// elapses before touching the transport again.
	reqA, _ := NewRequestWithContext(ctx, http.MethodGet, "https://example.invalid/feed/a", nil)
	respA, err := collectorA.Do(reqA)
	if err != nil {
		fmt.Println("request A error:", err)
		return
	}
	_ = respA.Body.Close()

	reqB, _ := NewRequestWithContext(ctx, http.MethodGet, "https://example.invalid/feed/b", nil)
	respB, err := collectorB.Do(reqB)
	if err != nil {
		fmt.Println("request B error:", err)
		return
	}
	_ = respB.Body.Close()

	fmt.Printf("final statuses: A=%d B=%d\n", respA.StatusCode, respB.StatusCode)
	// Output:
	// final statuses: A=200 B=200
}
