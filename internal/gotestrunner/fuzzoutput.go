package gotestrunner

// Keyed fuzzing output path. The batch RunFuzzTargets path owns lineWriter
// (console labels, prefixes); this file owns the per-attempt event stream the
// FuzzController sinks. Both reuse the same engine-line interpretation — the
// progress regexp, the 1MB partial-line cap and the "To re-run:" filtering —
// so a line means the same thing in the console and in an event.

import (
	"bytes"
	"strconv"
	"sync"
)

// fuzzStream is one stdout/stderr reader behind a fuzz attempt. It splits its
// writes into complete lines exactly like lineWriter — same cap, same
// continuation marker — and feeds each line to the attempt's dispatcher. The
// progress/deadline parsing runs here too, on the raw stream, so an attempt's
// metrics stay attributable to that attempt even once its events are revoked.
type fuzzStream struct {
	stream string
	label  string
	out    *fuzzEventOut
	buf    []byte

	skipNext     bool // the line after "To re-run:" is the engine's go test command
	afterFail    bool // the previous line was the target's own "--- FAIL:"
	deadlineFail bool // that failure's only message was "context deadline exceeded"
	progress     FuzzProgress
}

// newFuzzStream builds one stream reader. label is the target's Func, matching
// lineWriter's "--- FAIL: <Func>" anchoring.
func newFuzzStream(stream, label string, out *fuzzEventOut) *fuzzStream {
	return &fuzzStream{stream: stream, label: label, out: out}
}

// Write always consumes all of p and never errors: event delivery is
// best-effort, exactly as console emission is for lineWriter.
func (s *fuzzStream) Write(p []byte) (int, error) {
	s.buf = append(s.buf, p...)

	for {
		i := bytes.IndexByte(s.buf, '\n')
		if i < 0 {
			break
		}
		s.dispatch(s.buf[:i], false)
		s.buf = s.buf[i+1:]
	}

	// Same bound as lineWriter: an unterminated line past the cap is forced
	// out in capped, visibly-continued chunks instead of growing without end.
	for len(s.buf) > lineWriterMaxBuf {
		s.dispatch(s.buf[:lineWriterMaxBuf], true)
		s.buf = s.buf[lineWriterMaxBuf:]
	}
	return len(p), nil
}

// Close flushes the remaining partial line once the process and its output
// copies have finished. ManagedProcess guarantees no Write follows Close.
func (s *fuzzStream) Close() error {
	if len(s.buf) > 0 {
		s.dispatch(s.buf, false)
		s.buf = nil
	}
	return nil
}

// dispatch interprets one line's engine markers, then hands the filtered
// payload to the attempt's event dispatcher.
func (s *fuzzStream) dispatch(line []byte, continued bool) {
	text := string(line)
	if continued {
		// The continuation marker is part of the existing payload contract;
		// keep it on the event text rather than hiding the forced split.
		text = "(line continued) " + text
	} else {
		trimmed := bytes.TrimSpace(line)
		if s.skipNext {
			s.skipNext = false
			if bytes.HasPrefix(trimmed, []byte("go test -run=")) {
				return
			}
		}
		if bytes.Equal(trimmed, []byte("To re-run:")) {
			s.skipNext = true
			return
		}
		if s.afterFail {
			s.afterFail = false
			s.deadlineFail = bytes.Equal(trimmed, []byte("context deadline exceeded"))
		} else if bytes.HasPrefix(trimmed, []byte("--- FAIL: "+s.label+" (")) {
			s.afterFail = true
		}
		if m := fuzzProgressRe.FindSubmatch(trimmed); m != nil {
			execs, _ := strconv.ParseInt(string(m[1]), 10, 64)
			interesting, _ := strconv.Atoi(string(m[2]))
			s.progress = FuzzProgress{Execs: execs, Interesting: interesting}
		}
	}
	s.out.publish(s.stream, text)
}

// fuzzEventOut is the attempt-side half of event delivery: it decides whether
// a line still belongs to a live ticket and, while it does, hands it to the
// controller's serial sink with a per-attempt sequence number.
type fuzzEventOut struct {
	c       *FuzzController
	attempt FuzzAttempt

	mu      sync.Mutex
	revoked bool   // replacement or successful Cancel returned; no further events
	seq     uint64 // sequence across both streams, starting at 1
}

func (o *fuzzEventOut) publish(stream, text string) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.revoked {
		return
	}
	o.seq++
	// Delivery stays inside the attempt lock: the serial sink therefore sees
	// events in sequence order across the two streams, and per-stream order is
	// preserved by the lock itself. Sinks must return promptly and must not
	// call back into the controller, so holding the lock across delivery
	// cannot deadlock the scheduler's state lock.
	o.c.deliver(FuzzEvent{Attempt: o.attempt, Stream: stream, Text: text, Sequence: o.seq})
}

// revoke shuts the ticket's events off. It is idempotent. Because publish
// holds the same lock across the sink call, once revoke returns no callback
// for the ticket can start or remain in flight: the revocation barrier a
// Submit replacement or successful Cancel returns behind.
func (o *fuzzEventOut) revoke() {
	o.mu.Lock()
	o.revoked = true
	o.mu.Unlock()
}

// fuzzEventSink serializes every sink invocation across the whole controller,
// outside the controller state lock. A nil sink means live output is
// suppressed for this API while the ordering machinery still runs.
type fuzzEventSink struct {
	mu   sync.Mutex
	sink func(FuzzEvent)
}

func (s *fuzzEventSink) emit(ev FuzzEvent) {
	if s.sink == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.sink(ev)
}
