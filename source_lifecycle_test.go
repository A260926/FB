package geziyor_test

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"runtime"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/geziyor/geziyor"
	"github.com/geziyor/geziyor/client"
	"github.com/geziyor/geziyor/export"
	"github.com/geziyor/geziyor/task_support/taskcheck"
)

// waitGuard only bounds failure paths; the success paths below are fully
// synchronized through channels and never wait on it.
const waitGuard = 10 * time.Second

// waitClosed waits for ch to be closed and fails the check if it does not happen.
func waitClosed(c *taskcheck.C, ch <-chan struct{}, what string) {
	select {
	case <-ch:
	case <-time.After(waitGuard):
		c.Check(false, "timed out waiting for %s", what)
	}
}

// checkOpen asserts that ch is not closed right now.
func checkOpen(c *taskcheck.C, ch <-chan struct{}, what string) {
	select {
	case <-ch:
		c.Check(false, "%s closed unexpectedly", what)
	default:
		c.Check(true, "%s is open as expected", what)
	}
}

func recordMax(max *int32, v int32) {
	for {
		old := atomic.LoadInt32(max)
		if v <= old || atomic.CompareAndSwapInt32(max, old, v) {
			return
		}
	}
}

// collectExporter records everything it receives from the Exports channel.
type collectExporter struct {
	mu    sync.Mutex
	items []interface{}
}

func (e *collectExporter) Export(exports chan interface{}) error {
	for item := range exports {
		e.mu.Lock()
		e.items = append(e.items, item)
		e.mu.Unlock()
	}
	return nil
}

func (e *collectExporter) has(want interface{}) bool {
	e.mu.Lock()
	defer e.mu.Unlock()
	for _, item := range e.items {
		if item == want {
			return true
		}
	}
	return false
}

// TestSourceIsolation verifies that URL deduplication is scoped to the source
// handle: different sources, rebuilt same-name sources and the classic
// source-less entry points never eat each other's requests, while duplicates
// inside one scope are still filtered.
func TestSourceIsolation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var getCount, postCount int32
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.Method == http.MethodPost {
				atomic.AddInt32(&postCount, 1)
			} else {
				atomic.AddInt32(&getCount, 1)
			}
			fmt.Fprint(w, "ok")
		}))
		defer srv.Close()

		var cbAlpha, cbBeta, cbGlobal, cbRebuild, cbPost int32
		alphaGetDone := make(chan struct{})
		alphaPostsDone := make(chan struct{})

		g := geziyor.NewGeziyor(&geziyor.Options{
			RobotsTxtDisabled: true,
			LogDisabled:       true,
			StartRequestsFunc: func(g *geziyor.Geziyor) {
				alpha := g.NewSource("alpha")
				beta := g.NewSource("beta")

				// Same URL twice within one source: both accepted, one filtered.
				alphaCb := func(_ *geziyor.Geziyor, _ *client.Response) {
					if atomic.AddInt32(&cbAlpha, 1) == 1 {
						close(alphaGetDone)
					}
				}
				c.Check(alpha.Get(srv.URL+"/page", alphaCb), "alpha first GET accepted")
				c.Check(alpha.Get(srv.URL+"/page", alphaCb), "alpha duplicate GET still accepted at submission")

				// Same URL from another source and from the source-less entry.
				c.Check(beta.Get(srv.URL+"/page", func(_ *geziyor.Geziyor, _ *client.Response) {
					atomic.AddInt32(&cbBeta, 1)
				}), "beta GET for the same URL accepted")
				g.Get(srv.URL+"/page", func(_ *geziyor.Geziyor, _ *client.Response) {
					atomic.AddInt32(&cbGlobal, 1)
				})
				g.Get(srv.URL+"/page", func(_ *geziyor.Geziyor, _ *client.Response) {
					atomic.AddInt32(&cbGlobal, 1)
				})

				// Non-GET requests keep the existing behavior: no deduplication.
				postCb := func(_ *geziyor.Geziyor, _ *client.Response) {
					if atomic.AddInt32(&cbPost, 1) == 2 {
						close(alphaPostsDone)
					}
				}
				req1, err1 := client.NewRequest("POST", srv.URL+"/page", nil)
				req2, err2 := client.NewRequest("POST", srv.URL+"/page", nil)
				if err1 != nil || err2 != nil {
					c.Check(false, "POST requests constructed: %v %v", err1, err2)
				} else {
					c.Check(alpha.Do(req1, postCb), "alpha first POST accepted")
					c.Check(alpha.Do(req2, postCb), "alpha second POST accepted")
				}

				// Construction failure is reported as not accepted.
				c.Check(!beta.Get("://invalid-url", nil), "invalid URL rejected")

				// Retire alpha once its fetches finished, then rebuild it.
				waitClosed(c, alphaGetDone, "alpha GET callback")
				waitClosed(c, alphaPostsDone, "alpha POST callbacks")
				doneAlpha := alpha.Close()
				c.Check(doneAlpha == alpha.Close(), "Close returns the same channel")
				c.Check(!alpha.Get(srv.URL+"/other", nil), "closed alpha rejects new requests")
				waitClosed(c, doneAlpha, "alpha drained")

				alpha2 := g.NewSource("alpha")
				c.Check(alpha2.Get(srv.URL+"/page", func(_ *geziyor.Geziyor, _ *client.Response) {
					atomic.AddInt32(&cbRebuild, 1)
				}), "rebuilt same-name source accepts the same URL")
			},
		})
		g.Start()

		c.Check(atomic.LoadInt32(&cbAlpha) == 1, "duplicate GET inside one source filtered, callbacks=%d", cbAlpha)
		c.Check(atomic.LoadInt32(&cbBeta) == 1, "beta request eaten by alpha's visited set, callbacks=%d", cbBeta)
		c.Check(atomic.LoadInt32(&cbGlobal) == 1, "source-less duplicate GET filtered, callbacks=%d", cbGlobal)
		c.Check(atomic.LoadInt32(&cbRebuild) == 1, "rebuilt same-name source did not refetch, callbacks=%d", cbRebuild)
		c.Check(atomic.LoadInt32(&cbPost) == 2, "non-GET requests must not be deduplicated, callbacks=%d", cbPost)
		c.Check(atomic.LoadInt32(&getCount) == 4, "server saw %d GETs, want 4 (alpha, beta, global, rebuild)", getCount)
		c.Check(atomic.LoadInt32(&postCount) == 2, "server saw %d POSTs, want 2", postCount)
	})
}

// TestSourceRetirement verifies the lifecycle of a retired source: waiting
// tasks exit without network access, in-flight tasks and callbacks complete,
// the done channel closes only after real completion, Close inside a callback
// does not deadlock, and a synchronized submission waiting for resources
// returns when the source is closed.
func TestSourceRetirement(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Scenario 1: waiting task exits, in-flight task completes, other
		// sources and the shared Exports channel keep working.
		var blockSeen, neverSeen, afterSeen int32
		enteredBlock := make(chan struct{})
		releaseBlock := make(chan struct{})
		srv1 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/block":
				atomic.AddInt32(&blockSeen, 1)
				close(enteredBlock)
				<-releaseBlock
			case "/never":
				atomic.AddInt32(&neverSeen, 1)
			case "/after":
				atomic.AddInt32(&afterSeen, 1)
			}
			fmt.Fprint(w, "ok")
		}))
		defer srv1.Close()

		exporter := &collectExporter{}
		g1 := geziyor.NewGeziyor(&geziyor.Options{
			ConcurrentRequests: 1,
			RobotsTxtDisabled:  true,
			LogDisabled:        true,
			Exporters:          []export.Exporter{exporter},
			StartRequestsFunc: func(g *geziyor.Geziyor) {
				keep := g.NewSource("keep")
				gone := g.NewSource("gone")

				c.Check(keep.Get(srv1.URL+"/block", func(g *geziyor.Geziyor, _ *client.Response) {
					g.Exports <- "keep-export"
				}), "blocker accepted")
				waitClosed(c, enteredBlock, "blocker entered handler and holds the only slot")

				c.Check(gone.Get(srv1.URL+"/never", nil), "waiting task accepted")
				doneGone := gone.Close()
				c.Check(doneGone == gone.Close(), "Close returns the same channel")
				waitClosed(c, doneGone, "gone drained while its task was waiting")
				c.Check(atomic.LoadInt32(&neverSeen) == 0, "waiting task reached the network, hits=%d", neverSeen)
				c.Check(!gone.Get(srv1.URL+"/never", nil), "closed source rejects new requests")

				// Closing one source must not shut the others down.
				late := g.NewSource("late")
				c.Check(late.Get(srv1.URL+"/after", nil), "other source still accepts after gone closed")

				doneKeep := keep.Close()
				checkOpen(c, doneKeep, "keep done channel while its task is in flight")
				close(releaseBlock)
				waitClosed(c, doneKeep, "keep drained after the in-flight task completed")
			},
		})
		g1.Start()
		c.Check(atomic.LoadInt32(&blockSeen) == 1, "blocker fetched %d times, want 1", blockSeen)
		c.Check(atomic.LoadInt32(&neverSeen) == 0, "cancelled waiting task fetched %d times, want 0", neverSeen)
		c.Check(atomic.LoadInt32(&afterSeen) == 1, "later source fetched %d times, want 1", afterSeen)
		c.Check(exporter.has("keep-export"), "exports from a closing source still drained")

		// Scenario 2: Close inside the source's own callback neither deadlocks
		// nor finishes the source before the callback returns.
		enteredCb := make(chan struct{})
		releaseCb := make(chan struct{})
		doneOut := make(chan (<-chan struct{}), 1)
		srv2 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, "ok")
		}))
		defer srv2.Close()

		g2 := geziyor.NewGeziyor(&geziyor.Options{
			RobotsTxtDisabled: true,
			LogDisabled:       true,
			StartRequestsFunc: func(g *geziyor.Geziyor) {
				src := g.NewSource("self-close")
				c.Check(src.Get(srv2.URL+"/cb", func(_ *geziyor.Geziyor, _ *client.Response) {
					close(enteredCb)
					done := src.Close()
					checkOpen(c, done, "done channel while own callback is executing")
					c.Check(!src.Get(srv2.URL+"/again", nil), "recursive Get after Close rejected")
					doneOut <- done
					<-releaseCb
				}), "self-closing request accepted")
				waitClosed(c, enteredCb, "callback started")
				var done <-chan struct{}
				select {
				case done = <-doneOut:
				case <-time.After(waitGuard):
					c.Check(false, "callback did not hand out the done channel")
					return
				}
				checkOpen(c, done, "done channel while callback is still blocked")
				close(releaseCb)
				waitClosed(c, done, "done channel closed after the callback returned")
			},
		})
		g2.Start()

		// Scenario 3: synchronized Do waits for its own task, and a
		// synchronized submission stuck waiting for resources returns when the
		// source is closed.
		enteredHold := make(chan struct{})
		releaseHold := make(chan struct{})
		var syncSeen, syncCbRan int32
		srv3 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/hold":
				close(enteredHold)
				<-releaseHold
			case "/sync":
				atomic.AddInt32(&syncSeen, 1)
			}
			fmt.Fprint(w, "ok")
		}))
		defer srv3.Close()

		g3 := geziyor.NewGeziyor(&geziyor.Options{
			ConcurrentRequests: 1,
			RobotsTxtDisabled:  true,
			LogDisabled:        true,
			StartRequestsFunc: func(g *geziyor.Geziyor) {
				// A synchronized request is fully processed before Do returns.
				src := g.NewSource("sync")
				req, err := client.NewRequest("GET", srv3.URL+"/plain", nil)
				if err != nil {
					c.Check(false, "sync request constructed: %v", err)
					return
				}
				req.Synchronized = true
				accepted := src.Do(req, func(_ *geziyor.Geziyor, _ *client.Response) {
					atomic.StoreInt32(&syncCbRan, 1)
				})
				c.Check(accepted, "synchronized Do accepted")
				c.Check(atomic.LoadInt32(&syncCbRan) == 1, "synchronized Do returned before its callback finished")
				waitClosed(c, src.Close(), "idle source drained")

				// A synchronized submission waiting for a busy global slot
				// must return once its source is closed.
				holder := g.NewSource("holder")
				holder.Get(srv3.URL+"/hold", nil)
				waitClosed(c, enteredHold, "blocker holds the only global slot")

				exercised := false
			retry:
				for attempt := 0; attempt < 100; attempt++ {
					cand := g.NewSource("candidate")
					candReq, err := client.NewRequest("GET", srv3.URL+"/sync", nil)
					if err != nil {
						c.Check(false, "candidate request constructed: %v", err)
						break retry
					}
					candReq.Synchronized = true
					started := make(chan struct{})
					doReturned := make(chan bool, 1)
					go func() {
						close(started)
						doReturned <- cand.Do(candReq, nil)
					}()
					<-started
					runtime.Gosched() // let the submission reach the admission boundary
					done := cand.Close()
					select {
					case accepted := <-doReturned:
						if !accepted {
							// Close won the admission race; retry until the
							// submission is accepted first and cancelled while
							// waiting for the slot.
							continue
						}
						exercised = true
						c.Check(atomic.LoadInt32(&syncSeen) == 0, "cancelled synchronized task reached the network")
						waitClosed(c, done, "cancelled synchronized source drained")
						break retry
					case <-time.After(waitGuard):
						c.Check(false, "synchronized Do did not return after Close")
						break retry
					}
				}
				c.Check(exercised, "synchronized submission cancelled while waiting for resources")
				close(releaseHold)
			},
		})
		g3.Start()
		c.Check(atomic.LoadInt32(&syncSeen) == 0, "cancelled synchronized task fetched %d times, want 0", syncSeen)
	})
}

// TestSourceScheduling verifies that source requests obey the global and
// per-host concurrency limits: a host that is saturated must not block other
// hosts through the global slots, the per-host channel is created only once
// even for concurrent first accesses, and cancelled waiters neither lose nor
// return extra slots.
func TestSourceScheduling(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Scenario 1: tasks queued behind a saturated host must not hold
		// global slots that other hosts could use.
		var aCount, bCount, aCur, aMax, gCur, gMax int32
		enteredA1 := make(chan struct{})
		releaseA := make(chan struct{})
		enteredA2 := make(chan struct{})
		enteredA3 := make(chan struct{})
		enteredB1 := make(chan struct{})
		enterA := func() {
			recordMax(&aMax, atomic.AddInt32(&aCur, 1))
			recordMax(&gMax, atomic.AddInt32(&gCur, 1))
		}
		leaveA := func() {
			atomic.AddInt32(&aCur, -1)
			atomic.AddInt32(&gCur, -1)
		}
		srvA := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&aCount, 1)
			enterA()
			defer leaveA()
			switch r.URL.Path {
			case "/a1":
				close(enteredA1)
				<-releaseA
			case "/a2":
				close(enteredA2)
			case "/a3":
				close(enteredA3)
			}
			fmt.Fprint(w, "ok")
		}))
		defer srvA.Close()
		srvB := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&bCount, 1)
			recordMax(&gMax, atomic.AddInt32(&gCur, 1))
			defer atomic.AddInt32(&gCur, -1)
			close(enteredB1)
			fmt.Fprint(w, "ok")
		}))
		defer srvB.Close()

		g1 := geziyor.NewGeziyor(&geziyor.Options{
			ConcurrentRequests:          2,
			ConcurrentRequestsPerDomain: 1,
			RobotsTxtDisabled:           true,
			LogDisabled:                 true,
			StartRequestsFunc: func(g *geziyor.Geziyor) {
				sA := g.NewSource("site-a")
				sB := g.NewSource("site-b")
				c.Check(sA.Get(srvA.URL+"/a1", nil), "a1 accepted")
				waitClosed(c, enteredA1, "a1 running, holding site A and one global slot")
				c.Check(sA.Get(srvA.URL+"/a2", nil), "a2 accepted")
				c.Check(sA.Get(srvA.URL+"/a3", nil), "a3 accepted")
				// Let the queued site-A tasks reach their wait states before
				// site B competes for the remaining global slot. The passing
				// path never depends on this; it only makes a wrong
				// global-slot-first implementation reliably grab the slot.
				for i := 0; i < 100; i++ {
					runtime.Gosched()
				}
				c.Check(sB.Get(srvB.URL+"/b1", nil), "b1 accepted")
				waitClosed(c, enteredB1, "b1 ran while a2/a3 were queued behind saturated site A")
				close(releaseA)
				waitClosed(c, enteredA2, "a2 ran after a1 released site A")
				waitClosed(c, enteredA3, "a3 ran after a2 released site A")
			},
		})
		g1.Start()
		c.Check(atomic.LoadInt32(&aCount) == 3, "site A saw %d requests, want 3", aCount)
		c.Check(atomic.LoadInt32(&bCount) == 1, "site B saw %d requests, want 1", bCount)
		c.Check(atomic.LoadInt32(&aMax) == 1, "site A concurrency peaked at %d, want 1", aMax)
		c.Check(atomic.LoadInt32(&gMax) <= 2, "global concurrency peaked at %d, want <= 2", gMax)

		// Scenario 2: two sources hitting the same fresh host concurrently
		// share a single per-host channel.
		enteredX1 := make(chan struct{})
		releaseX := make(chan struct{})
		var x2Seen int32
		srvX := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/x1":
				close(enteredX1)
				<-releaseX
			case "/x2":
				atomic.AddInt32(&x2Seen, 1)
			}
			fmt.Fprint(w, "ok")
		}))
		defer srvX.Close()

		g2 := geziyor.NewGeziyor(&geziyor.Options{
			ConcurrentRequestsPerDomain: 1,
			RobotsTxtDisabled:           true,
			LogDisabled:                 true,
			StartRequestsFunc: func(g *geziyor.Geziyor) {
				sX := g.NewSource("x-one")
				sY := g.NewSource("x-two")
				c.Check(sX.Get(srvX.URL+"/x1", nil), "x1 accepted")
				waitClosed(c, enteredX1, "x1 running, holding the only slot of the host")
				c.Check(sY.Get(srvX.URL+"/x2", nil), "x2 from a second source accepted")
				// If the second source got its own host channel, its task
				// would enter the handler instead of waiting, and this done
				// channel would stay open.
				waitClosed(c, sY.Close(), "second source drained while waiting on the shared host slot")
				c.Check(atomic.LoadInt32(&x2Seen) == 0, "waiting task of second source reached the network")
				close(releaseX)
			},
		})
		g2.Start()
		c.Check(atomic.LoadInt32(&x2Seen) == 0, "second source fetched %d times, want 0", x2Seen)

		// Scenario 2b: many sources racing at the first access of the same
		// host still share one per-host channel, so the host limit holds.
		// Each round uses a fresh scraper (fresh host channels) and a fresh
		// server (fresh host); the passing path is deterministic.
		for round := 0; round < 30; round++ {
			enteredR := make(chan struct{})
			releaseR := make(chan struct{})
			var inside, maxInside, enteredCount int32
			srvR := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				recordMax(&maxInside, atomic.AddInt32(&inside, 1))
				defer atomic.AddInt32(&inside, -1)
				if atomic.AddInt32(&enteredCount, 1) == 1 {
					close(enteredR)
				}
				<-releaseR
				fmt.Fprint(w, "ok")
			}))
			gR := geziyor.NewGeziyor(&geziyor.Options{
				ConcurrentRequestsPerDomain: 1,
				RobotsTxtDisabled:           true,
				LogDisabled:                 true,
				StartRequestsFunc: func(g *geziyor.Geziyor) {
					for _, name := range []string{"r1", "r2", "r3", "r4"} {
						src := g.NewSource(name)
						c.Check(src.Get(srvR.URL+"/r", nil), "racing request accepted")
					}
					waitClosed(c, enteredR, "first racing request entered")
					// Give a wrongly duplicated host channel a chance to let a
					// second request in while the first one is held.
					for i := 0; i < 100; i++ {
						runtime.Gosched()
					}
					close(releaseR)
				},
			})
			gR.Start()
			c.Check(atomic.LoadInt32(&maxInside) == 1, "host concurrency peaked at %d with racing first accesses, want 1", maxInside)
			c.Check(atomic.LoadInt32(&enteredCount) == 4, "host saw %d requests, want 4", enteredCount)
			srvR.Close()
		}

		// Scenario 3: a task cancelled while waiting for the global slot
		// releases the per-host slot it already took; no slot is lost or
		// returned twice.
		enteredHold := make(chan struct{})
		releaseHold := make(chan struct{})
		enteredFree := make(chan struct{})
		var waitingSeen, freeSeen int32
		srvH := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			close(enteredHold)
			<-releaseHold
			fmt.Fprint(w, "ok")
		}))
		defer srvH.Close()
		srvO := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/waiting":
				atomic.AddInt32(&waitingSeen, 1)
			case "/free":
				atomic.AddInt32(&freeSeen, 1)
				close(enteredFree)
			}
			fmt.Fprint(w, "ok")
		}))
		defer srvO.Close()

		g3 := geziyor.NewGeziyor(&geziyor.Options{
			ConcurrentRequests:          1,
			ConcurrentRequestsPerDomain: 1,
			RobotsTxtDisabled:           true,
			LogDisabled:                 true,
			StartRequestsFunc: func(g *geziyor.Geziyor) {
				sH := g.NewSource("holder")
				sW := g.NewSource("waiting")
				c.Check(sH.Get(srvH.URL+"/hold", nil), "holder accepted")
				waitClosed(c, enteredHold, "holder running, holding the only global slot")
				c.Check(sW.Get(srvO.URL+"/waiting", nil), "waiting task accepted")
				// The waiting task takes srvO's host slot and then blocks on
				// the global slot; closing must release the host slot too.
				waitClosed(c, sW.Close(), "waiting task drained after close")
				c.Check(atomic.LoadInt32(&waitingSeen) == 0, "waiting task reached the network")
				sF := g.NewSource("free")
				c.Check(sF.Get(srvO.URL+"/free", nil), "follow-up task accepted")
				close(releaseHold)
				waitClosed(c, enteredFree, "follow-up ran, so both slots were accounted correctly")
			},
		})
		g3.Start()
		c.Check(atomic.LoadInt32(&waitingSeen) == 0, "waiting task fetched %d times, want 0", waitingSeen)
		c.Check(atomic.LoadInt32(&freeSeen) == 1, "follow-up task fetched %d times, want 1", freeSeen)
	})
}
