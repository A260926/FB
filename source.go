package geziyor

import (
	"context"
	"sync"

	"github.com/geziyor/geziyor/client"
	"github.com/geziyor/geziyor/internal"
)

// Source is a handle for an independent request lifecycle within a Geziyor
// scraper. It is created with Geziyor.NewSource.
//
// Requests submitted through a Source are scheduled exactly like requests
// submitted directly to Geziyor (same concurrency, delay and rate limits),
// but their URL deduplication is scoped to the handle and the whole handle
// can be retired with Close without disturbing other sources.
//
// The typical usage is creating a source inside StartRequestsFunc and
// submitting its start requests there; page callbacks may keep submitting
// follow-up requests through the same source.
type Source struct {
	g    *Geziyor
	name string

	mu     sync.Mutex
	closed bool
	wg     sync.WaitGroup

	cancelCh  chan struct{} // closed by Close: tells waiting tasks to give up
	doneCh    chan struct{} // closed once every accepted task fully finished
	closeOnce sync.Once
	ctx       context.Context // cancelled by Close, used while waiting for resources
	ctxCancel context.CancelFunc
}

// NewSource creates a new independent request source. The name is only used
// as a log label: every call creates a handle with its own lifecycle, and a
// new handle never inherits the cancellation or URL deduplication state of a
// previously closed handle, even one with the same name.
func (g *Geziyor) NewSource(name string) *Source {
	ctx, cancel := context.WithCancel(context.Background())
	return &Source{
		g:         g,
		name:      name,
		cancelCh:  make(chan struct{}),
		doneCh:    make(chan struct{}),
		ctx:       ctx,
		ctxCancel: cancel,
	}
}

// Get issues a GET to the specified URL through this source.
// It returns true if the request was accepted for scheduling. Accepted
// requests may still be dropped later (disallowed domain, robots.txt,
// duplicate URL, ...); false is returned only when the source is already
// closed or the request could not be constructed.
func (s *Source) Get(url string, callback func(g *Geziyor, r *client.Response)) bool {
	req, err := client.NewRequest("GET", url, nil)
	if err != nil {
		internal.Logger.Printf("Source %q request creating error %v\n", s.name, err)
		return false
	}
	return s.Do(req, callback)
}

// Do sends an HTTP request through this source.
// It returns true if the request was accepted for scheduling and false when
// the source is already closed. If req.Synchronized is set, Do blocks the
// calling goroutine until this request is fully processed (or dropped while
// waiting because the source was closed) and then returns.
func (s *Source) Do(req *client.Request, callback func(g *Geziyor, r *client.Response)) bool {
	// Admission boundary: the closed check and the task registration are
	// atomic with respect to Close, so every concurrent submission ends up
	// deterministically on exactly one side of the boundary.
	s.mu.Lock()
	if s.closed || s.g.shutdown {
		s.mu.Unlock()
		return false
	}
	s.wg.Add(1)
	s.g.wgRequests.Add(1)
	s.mu.Unlock()

	req.Source = s
	if req.Synchronized {
		s.g.do(req, callback, s)
	} else {
		go s.g.do(req, callback, s)
	}
	return true
}

// Close retires the source. It never blocks, is idempotent and always
// returns the same channel. After Close:
//   - new submissions through this source are rejected (Get/Do return false),
//   - accepted tasks that have not started request processing yet give up
//     waiting for scheduling resources and exit without touching the network
//     or the visited URL set,
//   - tasks that already started request processing run to completion,
//     including response middlewares, callbacks and exports.
//
// The returned channel is closed once all accepted tasks and their callbacks
// have truly finished. Closing a source never closes the shared Exports
// channel and never shuts the whole scraper down.
func (s *Source) Close() <-chan struct{} {
	s.closeOnce.Do(func() {
		s.mu.Lock()
		s.closed = true
		s.mu.Unlock()
		close(s.cancelCh)
		s.ctxCancel()
		internal.Logger.Printf("Source %q closed\n", s.name)
		go func() {
			s.wg.Wait()
			close(s.doneCh)
			internal.Logger.Printf("Source %q drained\n", s.name)
		}()
	})
	return s.doneCh
}
