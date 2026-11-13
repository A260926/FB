package discordgo

import (
	"context"
	"errors"
	"io/ioutil"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// lockResult carries the outcome of a LockBucketContext call back to the
// test goroutine.
type lockResult struct {
	bucket *Bucket
	err    error
}

// lockBucketContext runs LockBucketContext in the background and reports
// the result on the returned channel.
func lockBucketContext(rl *RateLimiter, ctx context.Context, bucketID string) chan lockResult {
	done := make(chan lockResult, 1)
	go func() {
		b, err := rl.LockBucketContext(ctx, bucketID)
		done <- lockResult{b, err}
	}()
	return done
}

// resetHeaders returns headers that exhaust a bucket and push its reset the
// given number of seconds into the future.
func resetHeaders(after string) http.Header {
	headers := http.Header{}
	headers.Set("X-RateLimit-Remaining", "0")
	headers.Set("X-RateLimit-Reset-After", after)
	return headers
}

func TestRateWaitContext(t *testing.T) {
	// A waiter blocked on a held bucket lock must return as soon as its
	// context is cancelled, without consuming Remaining, and other callers
	// must proceed once the holder releases the lock.
	t.Run("CancelWhileWaitingForHolder", func(t *testing.T) {
		rl := NewRatelimiter()

		// The holder keeps the bucket locked for the whole test.
		holder := rl.LockBucket("wait-holder")

		ctx, cancel := context.WithCancel(context.Background())
		done := lockBucketContext(rl, ctx, "wait-holder")

		cancel()

		select {
		case r := <-done:
			if !errors.Is(r.err, context.Canceled) {
				t.Errorf("err = %v, want context.Canceled", r.err)
			}
			if r.bucket != nil {
				t.Error("bucket must be nil when the wait is cancelled")
			}
		case <-time.After(5 * time.Second):
			t.Fatal("LockBucketContext did not return after cancel")
		}

		if got := rl.GetBucket("wait-holder").Remaining; got != 0 {
			t.Errorf("Remaining = %d, want 0: a cancelled waiter must not consume it", got)
		}

		// Once the holder releases, other callers proceed normally.
		holder.Release(nil)
		ctx2, cancel2 := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel2()
		b, err := rl.LockBucketContext(ctx2, "wait-holder")
		if err != nil {
			t.Fatalf("LockBucketContext after holder release: %v", err)
		}
		b.Release(nil)
	})

	// A waiter blocked on a bucket reset must hand the bucket lock back when
	// cancelled so the bucket does not stall for everyone else.
	t.Run("CancelWhileWaitingForReset", func(t *testing.T) {
		rl := NewRatelimiter()

		b := rl.LockBucket("wait-reset")
		if err := b.Release(resetHeaders("30")); err != nil {
			t.Fatal(err)
		}

		ctx, cancel := context.WithCancel(context.Background())
		done := lockBucketContext(rl, ctx, "wait-reset")

		cancel()

		select {
		case r := <-done:
			if !errors.Is(r.err, context.Canceled) {
				t.Errorf("err = %v, want context.Canceled", r.err)
			}
			if r.bucket != nil {
				t.Error("bucket must be nil when the wait is cancelled")
			}
		case <-time.After(5 * time.Second):
			t.Fatal("LockBucketContext did not return after cancel")
		}

		if got := rl.GetBucket("wait-reset").Remaining; got != 0 {
			t.Errorf("Remaining = %d, want 0: a cancelled waiter must not consume it", got)
		}

		// The bucket lock must have been handed back: locking it directly
		// has to succeed instead of blocking forever.
		locked := make(chan struct{})
		go func() {
			rl.GetBucket("wait-reset").Lock()
			close(locked)
		}()
		select {
		case <-locked:
			rl.GetBucket("wait-reset").Unlock()
		case <-time.After(5 * time.Second):
			t.Fatal("bucket lock was not released after cancellation")
		}
	})

	// A bounded deadline must interrupt the wait instead of letting it run
	// until the bucket reset.
	t.Run("DeadlineExceededWhileWaiting", func(t *testing.T) {
		rl := NewRatelimiter()

		b := rl.LockBucket("wait-deadline")
		if err := b.Release(resetHeaders("30")); err != nil {
			t.Fatal(err)
		}

		ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
		defer cancel()

		start := time.Now()
		got, err := rl.LockBucketContext(ctx, "wait-deadline")
		elapsed := time.Since(start)

		if !errors.Is(err, context.DeadlineExceeded) {
			t.Errorf("err = %v, want context.DeadlineExceeded", err)
		}
		if got != nil {
			t.Error("bucket must be nil when the deadline is exceeded")
		}
		if elapsed >= 5*time.Second {
			t.Errorf("wait ran for %v despite the 20ms deadline", elapsed)
		}
		if got := rl.GetBucket("wait-deadline").Remaining; got != 0 {
			t.Errorf("Remaining = %d, want 0", got)
		}
	})

	// A request carrying a context must not wait out the bucket reset once
	// the context deadline passes; no HTTP request is sent at all.
	t.Run("RequestDeadlineBeatsBucketReset", func(t *testing.T) {
		s, err := New("")
		if err != nil {
			t.Fatal(err)
		}

		var calls int32
		s.Client.Transport = roundTripperFunc(func(r *http.Request) (*http.Response, error) {
			atomic.AddInt32(&calls, 1)
			return &http.Response{
				Status:     "200 OK",
				StatusCode: http.StatusOK,
				Header:     http.Header{},
				Body:       ioutil.NopCloser(strings.NewReader(`[]`)),
				Request:    r,
			}, nil
		})

		// Exhaust the bucket and push its reset far out.
		b := s.Ratelimiter.LockBucket("request-deadline")
		if err := b.Release(resetHeaders("30")); err != nil {
			t.Fatal(err)
		}

		ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
		defer cancel()

		start := time.Now()
		_, err = s.RequestWithBucketID("GET", "https://discord.com/api/v9/channels/3", nil, "request-deadline", WithContext(ctx))
		elapsed := time.Since(start)

		if !errors.Is(err, context.DeadlineExceeded) {
			t.Errorf("err = %v, want context.DeadlineExceeded", err)
		}
		if elapsed >= 5*time.Second {
			t.Errorf("request waited %v despite the 20ms deadline", elapsed)
		}
		if got := atomic.LoadInt32(&calls); got != 0 {
			t.Errorf("sent %d requests, want 0", got)
		}
	})
}

func TestRateGlobalRecheck(t *testing.T) {
	// Admission must wait for the latest still-active deadline, not just the
	// bucket reset.
	t.Run("GlobalDeadlineExtendsBeyondLocalReset", func(t *testing.T) {
		rl := NewRatelimiter()

		b := rl.LockBucket("global-max")
		if err := b.Release(resetHeaders("0.1")); err != nil {
			t.Fatal(err)
		}
		// The global limit outlives the 100ms bucket reset.
		atomic.StoreInt64(rl.global, time.Now().Add(400*time.Millisecond).UnixNano())

		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		start := time.Now()
		got, err := rl.LockBucketContext(ctx, "global-max")
		elapsed := time.Since(start)
		if err != nil {
			t.Fatalf("LockBucketContext: %v", err)
		}
		got.Release(nil)

		if elapsed < 300*time.Millisecond {
			t.Errorf("admitted after %v, before the 400ms global deadline", elapsed)
		}
	})

	// A global limit extended while a waiter sleeps must be picked up when
	// the old timer fires, before Remaining is consumed.
	t.Run("GlobalExtendedDuringWait", func(t *testing.T) {
		rl := NewRatelimiter()

		b := rl.LockBucket("global-extend")
		if err := b.Release(resetHeaders("0.15")); err != nil {
			t.Fatal(err)
		}

		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		start := time.Now()
		done := lockBucketContext(rl, ctx, "global-extend")

		// Extend the global limit while the waiter is blocked on the bucket
		// reset. The 150ms timer waking up must not admit the request.
		atomic.StoreInt64(rl.global, start.Add(600*time.Millisecond).UnixNano())

		select {
		case r := <-done:
			if r.err != nil {
				t.Fatalf("LockBucketContext: %v", r.err)
			}
			r.bucket.Release(nil)
		case <-time.After(5 * time.Second):
			t.Fatal("waiter did not finish")
		}

		if elapsed := time.Since(start); elapsed < 500*time.Millisecond {
			t.Errorf("admitted after %v; global extension to 600ms was ignored", elapsed)
		}
	})

	// Cancelling one waiter must not lift the global limit for other
	// buckets, and must not stall its own bucket either.
	t.Run("LocalCancelKeepsGlobalLimit", func(t *testing.T) {
		rl := NewRatelimiter()
		atomic.StoreInt64(rl.global, time.Now().Add(500*time.Millisecond).UnixNano())

		ctx, cancel := context.WithCancel(context.Background())
		done := lockBucketContext(rl, ctx, "global-cancel-a")
		cancel()

		select {
		case r := <-done:
			if !errors.Is(r.err, context.Canceled) {
				t.Errorf("err = %v, want context.Canceled", r.err)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("waiter did not return after cancel")
		}

		// Another bucket is still limited by the global deadline: a short
		// deadline expires instead of admitting the request.
		ctx2, cancel2 := context.WithTimeout(context.Background(), 50*time.Millisecond)
		defer cancel2()
		start := time.Now()
		_, err := rl.LockBucketContext(ctx2, "global-cancel-b")
		if !errors.Is(err, context.DeadlineExceeded) {
			t.Errorf("err = %v, want context.DeadlineExceeded while the global limit holds", err)
		}
		if elapsed := time.Since(start); elapsed >= 400*time.Millisecond {
			t.Errorf("waited %v; the global limit should still be in effect", elapsed)
		}

		// The cancelled waiter's bucket recovers once the global limit passes.
		ctx3, cancel3 := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel3()
		b, err := rl.LockBucketContext(ctx3, "global-cancel-a")
		if err != nil {
			t.Fatalf("bucket of cancelled waiter did not recover: %v", err)
		}
		b.Release(nil)
	})
}

func TestRateRetryContext(t *testing.T) {
	// Cancelling the context while the client waits out a 429 Retry-After
	// must abort the call without sending another request.
	t.Run("CancelDuringRateLimitRetryWait", func(t *testing.T) {
		var calls int32
		responded := make(chan struct{})
		var once sync.Once

		s, err := New("")
		if err != nil {
			t.Fatal(err)
		}
		s.Client.Transport = roundTripperFunc(func(r *http.Request) (*http.Response, error) {
			n := atomic.AddInt32(&calls, 1)
			once.Do(func() { close(responded) })
			resp := &http.Response{
				Header:  http.Header{"Content-Type": {"application/json"}},
				Request: r,
			}
			if n == 1 {
				resp.Status = "429 Too Many Requests"
				resp.StatusCode = http.StatusTooManyRequests
				resp.Body = ioutil.NopCloser(strings.NewReader(`{"message":"rate limited","retry_after":5}`))
			} else {
				resp.Status = "200 OK"
				resp.StatusCode = http.StatusOK
				resp.Body = ioutil.NopCloser(strings.NewReader(`{}`))
			}
			return resp, nil
		})

		var optCalls int32
		opt := func(cfg *RequestConfig) { atomic.AddInt32(&optCalls, 1) }

		ctx, cancel := context.WithCancel(context.Background())
		done := make(chan error, 1)
		go func() {
			_, err := s.RequestWithBucketID("GET", "https://discord.com/api/v9/channels/1", nil, "retry-cancel", WithContext(ctx), opt)
			done <- err
		}()

		// The 429 response is in; the client is now waiting out Retry-After.
		<-responded
		cancel()

		select {
		case err := <-done:
			if !errors.Is(err, context.Canceled) {
				t.Errorf("err = %v, want context.Canceled", err)
			}
		case <-time.After(10 * time.Second):
			t.Fatal("request did not return after cancel")
		}

		if got := atomic.LoadInt32(&calls); got != 1 {
			t.Errorf("sent %d requests, want 1: no re-send after cancel", got)
		}
		if got := atomic.LoadInt32(&optCalls); got != 1 {
			t.Errorf("option ran %d times, want 1", got)
		}
	})

	// A retry after a 429 must reuse the same context, URL, body and
	// configuration, and request options must run exactly once per call.
	t.Run("RetryReusesRequestAndOptionsOnce", func(t *testing.T) {
		type ctxKey struct{}
		type recorded struct {
			method string
			body   string
			ctxVal interface{}
			header string
		}

		var mu sync.Mutex
		var got []recorded

		s, err := New("")
		if err != nil {
			t.Fatal(err)
		}
		s.Client.Transport = roundTripperFunc(func(r *http.Request) (*http.Response, error) {
			body, _ := ioutil.ReadAll(r.Body)
			r.Body.Close()
			mu.Lock()
			got = append(got, recorded{r.Method, string(body), r.Context().Value(ctxKey{}), r.Header.Get("X-Custom-Header")})
			n := len(got)
			mu.Unlock()

			resp := &http.Response{
				Header:  http.Header{"Content-Type": {"application/json"}},
				Request: r,
			}
			if n == 1 {
				resp.Status = "429 Too Many Requests"
				resp.StatusCode = http.StatusTooManyRequests
				resp.Body = ioutil.NopCloser(strings.NewReader(`{"message":"rate limited","retry_after":0.05}`))
			} else {
				resp.Status = "200 OK"
				resp.StatusCode = http.StatusOK
				resp.Body = ioutil.NopCloser(strings.NewReader(`{"ok":true}`))
			}
			return resp, nil
		})

		ctx := context.WithValue(context.Background(), ctxKey{}, "ctx-value")

		var optCalls int32
		opt := func(cfg *RequestConfig) {
			atomic.AddInt32(&optCalls, 1)
			cfg.Request.Header.Set("X-Custom-Header", "custom")
		}

		resp, err := s.RequestRaw("POST", "https://discord.com/api/v9/channels/1/messages", "application/json", []byte(`{"ping":1}`), "retry-reuse", 0, WithContext(ctx), opt)
		if err != nil {
			t.Fatalf("RequestRaw: %v", err)
		}
		if string(resp) != `{"ok":true}` {
			t.Errorf("response = %s, want %s", resp, `{"ok":true}`)
		}
		if got := atomic.LoadInt32(&optCalls); got != 1 {
			t.Errorf("option ran %d times, want 1", got)
		}

		mu.Lock()
		defer mu.Unlock()
		if len(got) != 2 {
			t.Fatalf("sent %d requests, want 2 (one retry)", len(got))
		}
		for i, r := range got {
			if r.method != "POST" {
				t.Errorf("request %d method = %s, want POST", i, r.method)
			}
			if r.body != `{"ping":1}` {
				t.Errorf("request %d body = %q, want %q", i, r.body, `{"ping":1}`)
			}
			if r.ctxVal != "ctx-value" {
				t.Errorf("request %d context value = %v, want ctx-value", i, r.ctxVal)
			}
			if r.header != "custom" {
				t.Errorf("request %d X-Custom-Header = %q, want custom", i, r.header)
			}
		}
	})

	// Cancelling the context while a retryable 5xx response waits for bucket
	// admission must stop the retry: no further request is sent.
	t.Run("CancelStopsServerErrorRetry", func(t *testing.T) {
		var calls int32
		responded := make(chan struct{})
		var once sync.Once

		s, err := New("")
		if err != nil {
			t.Fatal(err)
		}
		s.Client.Transport = roundTripperFunc(func(r *http.Request) (*http.Response, error) {
			atomic.AddInt32(&calls, 1)
			once.Do(func() { close(responded) })
			return &http.Response{
				Status:     "500 Internal Server Error",
				StatusCode: http.StatusInternalServerError,
				Header:     resetHeaders("2"),
				Body:       ioutil.NopCloser(strings.NewReader(`{}`)),
				Request:    r,
			}, nil
		})

		ctx, cancel := context.WithCancel(context.Background())
		done := make(chan error, 1)
		start := time.Now()
		go func() {
			_, err := s.RequestRaw("GET", "https://discord.com/api/v9/channels/2", "", nil, "retry-500", 0, WithContext(ctx))
			done <- err
		}()

		// The 500 response is in; the retry now waits for the bucket reset.
		<-responded
		cancel()

		select {
		case err := <-done:
			if !errors.Is(err, context.Canceled) {
				t.Errorf("err = %v, want context.Canceled", err)
			}
		case <-time.After(8 * time.Second):
			t.Fatal("request did not return after cancel")
		}

		if elapsed := time.Since(start); elapsed >= 1500*time.Millisecond {
			t.Errorf("retry wait was not interrupted, took %v", elapsed)
		}
		if got := atomic.LoadInt32(&calls); got != 1 {
			t.Errorf("sent %d requests, want 1: cancel must stop the retry", got)
		}
	})
}
