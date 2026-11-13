package discordgo

import (
	"context"
	"math"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// customRateLimit holds information for defining a custom rate limit
type customRateLimit struct {
	suffix   string
	requests int
	reset    time.Duration
}

// RateLimiter holds all ratelimit buckets
type RateLimiter struct {
	sync.Mutex
	global           *int64
	buckets          map[string]*Bucket
	globalRateLimit  time.Duration
	customRateLimits []*customRateLimit
}

// NewRatelimiter returns a new RateLimiter
func NewRatelimiter() *RateLimiter {

	return &RateLimiter{
		buckets: make(map[string]*Bucket),
		global:  new(int64),
		customRateLimits: []*customRateLimit{
			{
				suffix:   "//reactions//",
				requests: 1,
				reset:    200 * time.Millisecond,
			},
		},
	}
}

// GetBucket retrieves or creates a bucket
func (r *RateLimiter) GetBucket(key string) *Bucket {
	r.Lock()
	defer r.Unlock()

	if bucket, ok := r.buckets[key]; ok {
		return bucket
	}

	b := &Bucket{
		Remaining: 1,
		Key:       key,
		global:    r.global,
	}

	// Check if there is a custom ratelimit set for this bucket ID.
	for _, rl := range r.customRateLimits {
		if strings.HasSuffix(b.Key, rl.suffix) {
			b.customRateLimit = rl
			break
		}
	}

	r.buckets[key] = b
	return b
}

// GetWaitTime returns the duration you should wait for a Bucket
func (r *RateLimiter) GetWaitTime(b *Bucket, minRemaining int) time.Duration {
	now := time.Now()
	var wait time.Duration

	// If we ran out of calls and the reset time is still ahead of us
	// then we need to take it easy and relax a little
	if b.Remaining < minRemaining && b.reset.After(now) {
		wait = b.reset.Sub(now)
	}

	// Check for global ratelimits. A request may only go out once both the
	// bucket and the global limit have expired, so the latest deadline
	// that is still in effect wins.
	if sleepTo := time.Unix(0, atomic.LoadInt64(r.global)); now.Before(sleepTo) {
		if globalWait := sleepTo.Sub(now); globalWait > wait {
			wait = globalWait
		}
	}

	return wait
}

// LockBucket Locks until a request can be made
func (r *RateLimiter) LockBucket(bucketID string) *Bucket {
	return r.LockBucketObject(r.GetBucket(bucketID))
}

// LockBucketObject Locks an already resolved bucket until a request can be made
func (r *RateLimiter) LockBucketObject(b *Bucket) *Bucket {
	b.Lock()

	// The wait time is re-evaluated after every sleep so that a limit
	// extended while we waited (e.g. a global ratelimit) is still honored
	// before the remaining count is consumed.
	for {
		if wait := r.GetWaitTime(b, 1); wait > 0 {
			time.Sleep(wait)
		} else {
			break
		}
	}

	b.Remaining--
	return b
}

// LockBucketContext Locks until a request can be made or ctx is done.
// On success the returned bucket is locked and must be released by the
// caller with Release. On failure it returns nil and the context error;
// no remaining count is consumed and no lock is held.
func (r *RateLimiter) LockBucketContext(ctx context.Context, bucketID string) (*Bucket, error) {
	return r.LockBucketObjectContext(ctx, r.GetBucket(bucketID))
}

// LockBucketObjectContext Locks an already resolved bucket until a request
// can be made or ctx is done. See LockBucketContext for the contract.
func (r *RateLimiter) LockBucketObjectContext(ctx context.Context, b *Bucket) (*Bucket, error) {
	if err := b.lockContext(ctx); err != nil {
		return nil, err
	}

	// Same admission rules as LockBucketObject: the wait time is
	// re-evaluated after every wake-up, before the remaining count is
	// consumed, so a limit extended while we waited is still honored.
	for {
		wait := r.GetWaitTime(b, 1)
		if wait <= 0 {
			break
		}

		timer := time.NewTimer(wait)
		select {
		case <-timer.C:
		case <-ctx.Done():
			timer.Stop()
			b.Unlock()
			return nil, ctx.Err()
		}
	}

	b.Remaining--
	return b, nil
}

// Bucket represents a ratelimit bucket, each bucket gets ratelimited individually (-global ratelimits)
type Bucket struct {
	sync.Mutex
	Key       string
	Remaining int
	limit     int
	reset     time.Time
	global    *int64

	lastReset       time.Time
	customRateLimit *customRateLimit
	Userdata        interface{}

	// sem holds a single token while the bucket is unlocked. It backs the
	// Lock/Unlock methods so that acquisition can also be cancelled through
	// lockContext without stranding a goroutine inside Mutex.Lock.
	sem chan struct{}
}

// semChan returns the channel backing the bucket lock, creating it on first
// use so that a zero-value Bucket stays usable. The embedded Mutex is only
// used to guard this lazy initialization.
func (b *Bucket) semChan() chan struct{} {
	b.Mutex.Lock()
	defer b.Mutex.Unlock()
	if b.sem == nil {
		b.sem = make(chan struct{}, 1)
		b.sem <- struct{}{}
	}
	return b.sem
}

// Lock blocks until the bucket lock is acquired. It shares the same mutual
// exclusion as Unlock, Release and the RateLimiter locking helpers.
func (b *Bucket) Lock() {
	<-b.semChan()
}

// Unlock releases the bucket lock.
func (b *Bucket) Unlock() {
	select {
	case b.semChan() <- struct{}{}:
	default:
		panic("sync: Unlock of unlocked Bucket")
	}
}

// lockContext blocks until the bucket lock is acquired or ctx is done.
// A failed acquisition never touches the lock state.
func (b *Bucket) lockContext(ctx context.Context) error {
	sem := b.semChan()

	// Fast path: take the lock at once if it is free.
	select {
	case <-sem:
		return nil
	default:
	}

	select {
	case <-sem:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

// Release unlocks the bucket and reads the headers to update the buckets ratelimit info
// and locks up the whole thing in case if there's a global ratelimit.
func (b *Bucket) Release(headers http.Header) error {
	defer b.Unlock()

	// Check if the bucket uses a custom ratelimiter
	if rl := b.customRateLimit; rl != nil {
		if time.Now().Sub(b.lastReset) >= rl.reset {
			b.Remaining = rl.requests - 1
			b.lastReset = time.Now()
		}
		if b.Remaining < 1 {
			b.reset = time.Now().Add(rl.reset)
		}
		return nil
	}

	if headers == nil {
		return nil
	}

	remaining := headers.Get("X-RateLimit-Remaining")
	reset := headers.Get("X-RateLimit-Reset")
	global := headers.Get("X-RateLimit-Global")
	resetAfter := headers.Get("X-RateLimit-Reset-After")

	// Update global and per bucket reset time if the proper headers are available
	// If global is set, then it will block all buckets until after Retry-After
	// If Retry-After without global is provided it will use that for the new reset
	// time since it's more accurate than X-RateLimit-Reset.
	// If Retry-After after is not proided, it will update the reset time from X-RateLimit-Reset
	if resetAfter != "" {
		parsedAfter, err := strconv.ParseFloat(resetAfter, 64)
		if err != nil {
			return err
		}

		whole, frac := math.Modf(parsedAfter)
		resetAt := time.Now().Add(time.Duration(whole) * time.Second).Add(time.Duration(frac*1000) * time.Millisecond)

		// Lock either this single bucket or all buckets
		if global != "" {
			atomic.StoreInt64(b.global, resetAt.UnixNano())
		} else {
			b.reset = resetAt
		}
	} else if reset != "" {
		// Calculate the reset time by using the date header returned from discord
		discordTime, err := http.ParseTime(headers.Get("Date"))
		if err != nil {
			return err
		}

		unix, err := strconv.ParseFloat(reset, 64)
		if err != nil {
			return err
		}

		// Calculate the time until reset and add it to the current local time
		// some extra time is added because without it i still encountered 429's.
		// The added amount is the lowest amount that gave no 429's
		// in 1k requests
		whole, frac := math.Modf(unix)
		delta := time.Unix(int64(whole), 0).Add(time.Duration(frac*1000)*time.Millisecond).Sub(discordTime) + time.Millisecond*250
		b.reset = time.Now().Add(delta)
	}

	// Udpate remaining if header is present
	if remaining != "" {
		parsedRemaining, err := strconv.ParseInt(remaining, 10, 32)
		if err != nil {
			return err
		}
		b.Remaining = int(parsedRemaining)
	}

	return nil
}
