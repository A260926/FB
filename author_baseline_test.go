package sse

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"

	"gopkg.in/cenkalti/backoff.v1"
)

// This is a witness for the unmodified starting behavior, not a post-fix test.
func TestAuthorBaselineFiniteEOF(t *testing.T) {
	for _, tc := range []struct {
		name, body string
		events     int
	}{
		{"one_record", "id: station-1\ndata: temperature=21\n\n", 1},
		{"empty_stream", "", 0},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var requests int32
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				atomic.AddInt32(&requests, 1)
				if r.URL.Query().Get("stream") != "weather" || r.URL.Query().Get("site") != "east" || r.Header.Get("Accept") != "text/event-stream" {
					t.Errorf("unexpected request path or headers")
				}
				w.Header().Set("Content-Type", "text/event-stream")
				fmt.Fprint(w, tc.body)
			}))
			defer srv.Close()
			c := NewClient(srv.URL + "?site=east")
			c.ReconnectStrategy = backoff.WithMaxTries(&backoff.ZeroBackOff{}, 1)
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()
			got := 0
			err := c.SubscribeWithContext(ctx, "weather", func(e *Event) {
				got++
				if string(e.ID) != "station-1" || string(e.Data) != "temperature=21" {
					t.Errorf("unexpected normal record: id=%q data=%q", e.ID, e.Data)
				}
			})
			if err != nil || got != tc.events || atomic.LoadInt32(&requests) != 1 {
				t.Fatalf("starting behavior changed: requests=%d events=%d err=%v", requests, got, err)
			}
			fmt.Printf("BASELINE_FINITE_EOF case=%s requests=%d events=%d return=nil\n", tc.name, requests, got)
		})
	}
	fmt.Println("BASELINE_OK Z1154")
}
