// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package expect

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
	"time"

	"github.com/google/goexpect/task_support/taskcheck"
)

// pipeSession drives a SpawnGeneric session over local pipes. The test owns the
// device side: it emits prompts/output and records the exact bytes the
// automation handed to the write goroutine.
type pipeSession struct {
	exp *GExpect

	fromDevice *io.PipeWriter // device -> automation
	input      chan string    // bytes observed by the device reader
	waitCh     chan error
	closed     int32

	mu   sync.Mutex
	sent strings.Builder
}

func newPipeSession(t *testing.T, opts ...Option) *pipeSession {
	t.Helper()
	deviceIn, autoOut := io.Pipe() // automation writes to autoOut; device reads deviceIn
	autoIn, deviceOut := io.Pipe() // device writes to deviceOut; automation reads autoIn

	s := &pipeSession{
		fromDevice: deviceOut,
		input:      make(chan string, 256),
		waitCh:     make(chan error, 1),
	}
	baseOpts := []Option{CheckDuration(time.Millisecond)}
	baseOpts = append(baseOpts, opts...)
	exp, _, err := SpawnGeneric(&GenOptions{
		In:  autoOut,
		Out: autoIn,
		Wait: func() error {
			return <-s.waitCh
		},
		Close: func() error {
			atomic.StoreInt32(&s.closed, 1)
			return autoOut.Close()
		},
		Check: func() bool { return true },
	}, time.Second, baseOpts...)
	if err != nil {
		t.Fatalf("SpawnGeneric failed: %v", err)
	}
	s.exp = exp

	// Device reader records every byte the automation sends.
	go func() {
		buf := make([]byte, 1024)
		for {
			nr, err := deviceIn.Read(buf)
			if nr > 0 {
				chunk := string(buf[:nr])
				s.mu.Lock()
				s.sent.WriteString(chunk)
				s.mu.Unlock()
				select {
				case s.input <- chunk:
				default:
				}
			}
			if err != nil {
				return
			}
		}
	}()
	return s
}

func (s *pipeSession) write(text string) {
	_, _ = io.WriteString(s.fromDevice, text)
}

func (s *pipeSession) sentBytes() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.sent.String()
}

func (s *pipeSession) buffered() int {
	s.exp.mu.Lock()
	defer s.exp.mu.Unlock()
	return s.exp.out.Len()
}

// expectInput waits a bounded time for the device to observe want.
func (s *pipeSession) expectInput(want string) bool {
	deadline := time.After(3 * time.Second)
	var got strings.Builder
	for {
		select {
		case chunk := <-s.input:
			got.WriteString(chunk)
			if got.String() == want {
				return true
			}
			if strings.HasPrefix(want, got.String()) {
				continue
			}
			return false
		case <-deadline:
			return false
		}
	}
}

func (s *pipeSession) noInputWithin(d time.Duration) bool {
	select {
	case <-s.input:
		return false
	case <-time.After(d):
		return true
	}
}

func (s *pipeSession) close() {
	select {
	case s.waitCh <- nil:
	default:
	}
	s.exp.Close()
}

func cancelAfter(s *pipeSession, cancel context.CancelFunc, want string) {
	go func() {
		if s.expectInput(want) {
			cancel()
		}
	}()
}

func resIndices(res []BatchRes) []int {
	out := make([]int, 0, len(res))
	for _, r := range res {
		out = append(out, r.Idx)
	}
	return out
}

func joinOutputs(res []BatchRes) string {
	var b strings.Builder
	for _, r := range res {
		b.WriteString(r.Output)
	}
	return b.String()
}

// -----------------------------------------------------------------------------
// Boundary group: pre-canceled entry, a send channel that is temporarily unable
// to accept, SwitchCase auto-answer/retry, and a total deadline under
// continuous output.
// -----------------------------------------------------------------------------

func TestBatchContextBoundary(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("preCanceled", func(t *testing.T) {
			s := newPipeSession(t)
			defer s.close()
			ctx, cancel := context.WithCancel(context.Background())
			cancel()
			res, err := s.exp.ExpectBatchContext(ctx, []Batcher{
				&BSnd{S: "never\n"},
				&BExp{R: "PROMPT>"},
			}, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "pre-canceled must return context.Canceled, got %v", err)
			c.Check(len(res) == 0, "pre-canceled batch must not fabricate results, got %+v", res)
			c.Check(s.noInputWithin(150*time.Millisecond), "pre-canceled batch must not send anything, got %q", s.sentBytes())
		})

		t.Run("sendChannelNotReady", func(t *testing.T) {
			deviceIn, autoOut := io.Pipe() // automation -> device
			autoIn, deviceOut := io.Pipe() // device -> automation
			var alive int32 = 1
			var deviceBytes bytes.Buffer
			var deviceMu sync.Mutex
			deviceRead := make(chan struct{}, 1)
			finished := make(chan struct{})
			exp, _, err := SpawnGeneric(&GenOptions{
				In:  autoOut,
				Out: autoIn,
				Wait: func() error {
					<-finished
					return nil
				},
				Close: func() error {
					atomic.StoreInt32(&alive, 0)
					close(finished)
					return autoOut.Close()
				},
				Check: func() bool { return atomic.LoadInt32(&alive) != 0 },
			}, time.Second, CheckDuration(time.Millisecond))
			c.Check(err == nil, "SpawnGeneric: %v", err)
			if err != nil {
				return
			}
			// Device reads one byte then stalls: the session write goroutine stays
			// blocked in Write, so the send channel cannot accept.
			stall := make(chan struct{})
			go func() {
				buf := make([]byte, 1)
				nr, _ := deviceIn.Read(buf)
				deviceMu.Lock()
				deviceBytes.Write(buf[:nr])
				deviceMu.Unlock()
				deviceRead <- struct{}{}
				<-stall
			}()
			if err := exp.Send("first"); err != nil {
				c.Check(false, "first send should be accepted: %v", err)
				return
			}
			select {
			case <-deviceRead:
			case <-time.After(2 * time.Second):
				c.Check(false, "device never observed the first write")
				return
			}
			// The device is stalled mid-Write, so a following Send cannot be
			// received by the write goroutine until the stall is released. Cancel
			// while the batch waits on the send channel.
			ctx, cancel := context.WithCancel(context.Background())
			go func() {
				// The write goroutine is already blocked in Write (the device
				// observed the first byte and has not read further). This delay
				// only bounds how long we wait before observing cancellation; it
				// does not establish any data ordering.
				time.Sleep(30 * time.Millisecond)
				cancel()
			}()
			res, err := exp.ExpectBatchContext(ctx, []Batcher{
				&BSnd{S: "second"},
				&BSnd{S: "third"},
			}, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "blocked send must report context.Canceled, got %v", err)
			c.Check(len(res) == 0, "send cancellation must not fabricate BatchRes, got %+v", res)
			deviceMu.Lock()
			got := deviceBytes.String()
			deviceMu.Unlock()
			c.Check(got == "f", "only the already submitted byte may reach the device, got %q", got)
			// Release the device and tear the session down; in particular no later
			// batch action ("third") was queued.
			close(stall)
			atomic.StoreInt32(&alive, 0)
			_ = exp.Close()
			_ = deviceIn.Close()
			_ = deviceOut.Close()
			_ = autoIn.Close()
		})

		t.Run("switchCaseAutoAnswerAndRetry", func(t *testing.T) {
			s := newPipeSession(t)
			defer s.close()
			ctx, cancel := context.WithCancel(context.Background())
			go func() {
				s.write("USER: ")
				if !s.expectInput("alice\n") {
					cancel()
					return
				}
				s.write("PASS: ")
				if !s.expectInput("secret\n") {
					cancel()
					return
				}
				// The Continue-tagged PASS case retries; cancel while the batch
				// waits for a prompt that never arrives.
				cancel()
			}()
			res, err := s.exp.ExpectBatchContext(ctx, []Batcher{
				&BCas{C: []Caser{
					&Case{R: regexp.MustCompile(`USER: `), S: "alice\n", T: Continue(NewStatus(0, "retry user")), Rt: 1},
					&Case{R: regexp.MustCompile(`USER: `), S: "alice\n", T: Continue(NewStatus(0, "retry user")), Rt: 1},
					&Case{R: regexp.MustCompile(`PASS: `), S: "secret\n", T: Continue(NewStatus(0, "retry pass")), Rt: 1},
					&Case{R: regexp.MustCompile(`DONE> `), S: "", T: OK()},
				}},
			}, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "switchcase retry cancel must be context.Canceled, got %v", err)
			c.Check(s.sentBytes() == "alice\nsecret\n", "auto answers must reach the device exactly once, got %q", s.sentBytes())
			c.Check(len(res) == 1, "canceled switchcase must report one pending step, got %+v", res)
			if len(res) == 1 {
				c.Check(res[0].Idx == 0, "pending step idx must be 0, got %d", res[0].Idx)
				c.Check(res[0].Match == nil, "pending step match must be nil, got %v", res[0].Match)
			}
			c.Check(s.noInputWithin(150*time.Millisecond), "no later action may be delivered after cancellation")
		})

		t.Run("deadlineWithContinuousOutput", func(t *testing.T) {
			s := newPipeSession(t)
			defer s.close()
			stop := make(chan struct{})
			stopped := make(chan struct{})
			go func() {
				defer close(stopped)
				ticker := time.NewTicker(time.Millisecond)
				defer ticker.Stop()
				for {
					select {
					case <-stop:
						return
					case <-ticker.C:
						s.write(".")
					}
				}
			}()
			ctx, cancel := context.WithTimeout(context.Background(), 120*time.Millisecond)
			defer cancel()
			start := time.Now()
			res, err := s.exp.ExpectBatchContext(ctx, []Batcher{
				&BExp{R: "NEVER_MATCHES"},
			}, 30*time.Second) // per-wait timeout far longer than the ctx deadline
			elapsed := time.Since(start)
			close(stop)
			<-stopped
			c.Check(errors.Is(err, context.DeadlineExceeded), "continuous output must still hit ctx deadline, got %v", err)
			c.Check(elapsed < 3*time.Second, "ctx deadline must not be extended by output, took %v", elapsed)
			c.Check(len(res) == 1, "canceled expect step must appear in results, got %+v", res)
			if len(res) == 1 {
				c.Check(res[0].Match == nil, "canceled step match must be nil, got %v", res[0].Match)
				c.Check(len(res[0].Output) > 0 && strings.HasPrefix(res[0].Output, "."), "canceled output should hold gathered dots, got %q", res[0].Output)
			}
		})

		t.Run("finishedBatchStaysSuccessful", func(t *testing.T) {
			s := newPipeSession(t)
			defer s.close()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			go func() {
				s.write("READY> ")
				if s.expectInput("go\n") {
					s.write("MORE> ")
				}
			}()
			res, err := s.exp.ExpectBatchContext(ctx, []Batcher{
				&BExp{R: `READY> `},
				&BSnd{S: "go\n"},
				&BExp{R: `MORE> `},
			}, 2*time.Second)
			cancel()
			c.Check(err == nil, "a finished batch stays successful after a late cancel: %v", err)
			c.Check(fmt.Sprint(resIndices(res)) == "[0 2]", "finished batch should report both Expect steps, got %v", resIndices(res))
			if len(res) == 2 {
				c.Check(res[0].Match != nil && res[0].Match[0] == "READY> ", "READY match: %v", res[0].Match)
				c.Check(res[1].Match != nil && res[1].Match[0] == "MORE> ", "MORE match: %v", res[1].Match)
			}
		})

		t.Run("noLeftoverBatchWork", func(t *testing.T) {
			s := newPipeSession(t)
			defer s.close()
			ctx, cancel := context.WithCancel(context.Background())
			go func() {
				s.write("partial")
				time.Sleep(30 * time.Millisecond)
				cancel()
			}()
			res, err := s.exp.ExpectBatchContext(ctx, []Batcher{
				&BExp{R: `never`},
				&BSnd{S: "LATE\n"},
			}, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled, got %v (res %+v)", err, res)
			// After the method returned no later batch action may be delivered.
			c.Check(s.noInputWithin(150*time.Millisecond), "a later batch action was delivered after cancellation: %q", s.sentBytes())
			// A single ordinary Expect still works and sees exactly the restored text.
			got, _, err := s.exp.Expect(regexp.MustCompile(`partial`), 2*time.Second)
			c.Check(err == nil, "Expect after cancel failed: %v", err)
			c.Check(got == "partial", "Expect after cancel = %q, want %q", got, "partial")
		})

		t.Run("signalChecksContext", func(t *testing.T) {
			s := newPipeSession(t)
			defer s.close()
			ctx, cancel := context.WithCancel(context.Background())
			cancel()
			res, err := s.exp.ExpectBatchContext(ctx, []Batcher{
				&BSig{S: syscall.SIGTERM},
			}, time.Second)
			c.Check(errors.Is(err, context.Canceled), "pre-canceled signal batch: got %v, want context.Canceled (res %+v)", err, res)
			c.Check(len(res) == 0, "pre-canceled signal batch must not fabricate results, got %+v", res)
		})
	})
}

// -----------------------------------------------------------------------------
// Buffer group: cancellation puts the current step's uncommitted local text back
// at the front of the session buffer; PartialMatch's suffix is restored once.
// -----------------------------------------------------------------------------

func TestBatchContextBuffer(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("restoreAcrossSegments", func(t *testing.T) {
			s := newPipeSession(t)
			defer s.close()
			ctx, cancel := context.WithCancel(context.Background())
			// "hello " arrives and is gathered by the canceled step; "world" arrives
			// after cancellation. The next Expect matches across both.
			go func() {
				s.write("hello ")
				time.Sleep(30 * time.Millisecond)
				cancel()
				time.Sleep(20 * time.Millisecond)
				s.write("world")
			}()
			res, err := s.exp.ExpectBatchContext(ctx, []Batcher{
				&BExp{R: `hello world`},
			}, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled, got %v", err)
			c.Check(len(res) == 1 && res[0].Output == "hello ", "canceled output must carry gathered text, got %+v", res)
			if len(res) == 1 {
				c.Check(res[0].Idx == 0 && res[0].Match == nil, "pending result idx/match: %d/%v", res[0].Idx, res[0].Match)
			}
			// Wait for the trailing bytes to land before the follow-up Expect.
			deadline := time.Now().Add(2 * time.Second)
			for s.buffered() < len("hello world") && time.Now().Before(deadline) {
				time.Sleep(time.Millisecond)
			}
			out, match, err := s.exp.Expect(regexp.MustCompile(`hello world`), 2*time.Second)
			c.Check(err == nil, "next Expect must match across restored and new bytes: %v", err)
			c.Check(out == "hello world", "next Expect output must contain each byte once, got %q", out)
			c.Check(match != nil && match[0] == "hello world", "next Expect match: %v", match)
		})

		t.Run("segmentsRestoredExactlyOnce", func(t *testing.T) {
			s := newPipeSession(t)
			defer s.close()
			ctx, cancel := context.WithCancel(context.Background())
			go func() {
				s.write("AAA")
				time.Sleep(40 * time.Millisecond)
				s.write("BBB")
				time.Sleep(40 * time.Millisecond)
				cancel()
			}()
			res, err := s.exp.ExpectBatchContext(ctx, []Batcher{
				&BExp{R: `never`},
			}, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled, got %v", err)
			pending := joinOutputs(res)
			c.Check(len(res) == 1, "one pending result expected, got %+v", res)
			c.Check(strings.Count(pending, "AAA") == 1 && strings.Count(pending, "BBB") == 1,
				"gathered segments must appear exactly once, got %q", pending)
			got, _, err := s.exp.Expect(regexp.MustCompile(`AAABBB`), 2*time.Second)
			c.Check(err == nil, "restored buffer must match AAABBB: %v", err)
			c.Check(got == "AAABBB", "restored buffer must not duplicate bytes, got %q", got)
		})

		t.Run("partialMatchSuffixOnce", func(t *testing.T) {
			s := newPipeSession(t, PartialMatch(true))
			defer s.close()
			ctx, cancel := context.WithCancel(context.Background())
			go func() {
				s.write("prompt> ")
				s.write("stray-bytes")
				time.Sleep(40 * time.Millisecond)
				cancel()
			}()
			// The first step matches "prompt> " and is committed; the second waits
			// for "done" and is canceled. PartialMatch leaves "stray-bytes" as the
			// unmatched suffix exactly once.
			res, err := s.exp.ExpectBatchContext(ctx, []Batcher{
				&BExp{R: `prompt> `},
				&BExp{R: `done`},
			}, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled, got %v", err)
			c.Check(len(res) == 2, "first step completed, second canceled: got %+v", res)
			if len(res) == 2 {
				c.Check(res[0].Idx == 0 && res[1].Idx == 1, "indices should be 0,1 got %v", resIndices(res))
				c.Check(res[0].Match != nil && res[0].Match[0] == "prompt> ", "completed step keeps its match, got %v", res[0].Match)
				c.Check(res[1].Match == nil, "canceled step has nil match, got %v", res[1].Match)
			}
			pending := joinOutputs(res[1:])
			c.Check(strings.Count(pending, "stray-bytes") == 1, "partial suffix must be reported once, got %q", pending)
			got, _, err := s.exp.Expect(regexp.MustCompile(`stray-bytes`), 2*time.Second)
			c.Check(err == nil, "next Expect must consume the restored suffix: %v", err)
			c.Check(got == "stray-bytes", "suffix must be restored exactly once, got %q", got)
			rest, _, _ := s.exp.Expect(regexp.MustCompile(`x`), 0)
			c.Check(rest == "", "no bytes should remain duplicated, got %q", rest)
		})

		t.Run("committedMatchesStayConsumed", func(t *testing.T) {
			// PartialMatch keeps the unmatched "two" available for the next step
			// instead of discarding it with the first match.
			s := newPipeSession(t, PartialMatch(true))
			defer s.close()
			ctx, cancel := context.WithCancel(context.Background())
			go func() {
				s.write("onetwo")
				time.Sleep(40 * time.Millisecond)
				cancel()
			}()
			res, err := s.exp.ExpectBatchContext(ctx, []Batcher{
				&BExp{R: `one`},
				&BExp{R: `three`},
			}, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled, got %v", err)
			c.Check(len(res) == 2, "expected completed + canceled results, got %+v", res)
			if len(res) == 2 {
				c.Check(res[0].Output == "one", "completed step should keep its output, got %q", res[0].Output)
				c.Check(res[1].Output == "two", "canceled step should carry only uncommitted text, got %q", res[1].Output)
				c.Check(res[1].Match == nil, "canceled step match nil, got %v", res[1].Match)
			}
			got, _, err := s.exp.Expect(regexp.MustCompile(`two`), 2*time.Second)
			c.Check(err == nil, "uncommitted 'two' must be available again: %v", err)
			c.Check(got == "two", "committed 'one' must not be replayed, got %q", got)
		})
	})
}

// -----------------------------------------------------------------------------
// Session group: a full business form workflow over a real local subprocess, plus
// revocation and auto-answer interleavings on local pipes.
// -----------------------------------------------------------------------------

// formChild runs the child form service. It prompts for a ticket, an amount and a
// final confirmation, and appends one line per genuinely submitted business
// record to FORMTEST_LOG.
func formChild() int {
	logPath := os.Getenv("FORMTEST_LOG")
	logFile, err := os.OpenFile(logPath, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0600)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open log: %v\n", err)
		return 2
	}
	defer logFile.Close()

	reader := newLineReader(os.Stdin)
	read := func() (string, bool) {
		line, ok := reader.readLine()
		return strings.TrimSuffix(line, "\n"), ok
	}
	write := func(s string) bool {
		_, err := io.WriteString(os.Stdout, s)
		return err == nil
	}

	if !write("TICKET> ") {
		return 2
	}
	ticket, ok := read()
	if !ok {
		return 2
	}
	if !write("AMOUNT> ") {
		return 2
	}
	amount, ok := read()
	if !ok {
		return 2
	}
	// Emit the beginning of the confirmation prompt, then announce that the
	// amount was received and hold the final "> " until the test releases the
	// gate. The automation can therefore be canceled while it waits mid-prompt,
	// with "CONFIRM" gathered locally, genuinely between the amount and the
	// confirmation send.
	if !write("CONFIRM") {
		return 2
	}
	if gate := os.Getenv("FORMTEST_GATE"); gate != "" {
		if err := os.WriteFile(gate+".amount", []byte("received"), 0666); err != nil {
			fmt.Fprintf(os.Stderr, "write gate marker: %v\n", err)
			return 2
		}
		release := gate + ".release"
		for {
			if _, err := os.Stat(release); err == nil {
				break
			}
			time.Sleep(time.Millisecond)
		}
	}
	if !write("> ") {
		return 2
	}
	confirm, ok := read()
	if !ok {
		return 2
	}
	if confirm == "yes" {
		fmt.Fprintf(logFile, "COMMIT %s %s\n", ticket, amount)
	}
	if !write("RECEIPT> ") {
		return 2
	}
	return 0
}

type lineReader struct {
	buf []byte
	in  io.Reader
}

func newLineReader(r io.Reader) *lineReader { return &lineReader{in: r} }

func (l *lineReader) readLine() (string, bool) {
	for {
		if i := bytes.IndexByte(l.buf, '\n'); i >= 0 {
			line := string(l.buf[:i+1])
			l.buf = l.buf[i+1:]
			return line, true
		}
		tmp := make([]byte, 256)
		nr, err := l.in.Read(tmp)
		if nr > 0 {
			l.buf = append(l.buf, tmp[:nr]...)
		}
		if err != nil {
			if len(l.buf) > 0 {
				line := string(l.buf)
				l.buf = nil
				return line, true
			}
			return "", false
		}
	}
}

// byteRecorder wraps the session input and records exactly what the write
// goroutine delivered to the service.
type byteRecorder struct {
	mu  sync.Mutex
	buf bytes.Buffer
	w   io.WriteCloser
}

func (r *byteRecorder) Write(p []byte) (int, error) {
	r.mu.Lock()
	r.buf.Write(p)
	r.mu.Unlock()
	return r.w.Write(p)
}

func (r *byteRecorder) Close() error { return r.w.Close() }

func (r *byteRecorder) String() string {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.buf.String()
}

// formService is a real local subprocess form service plus the raw-bytes and
// business-record views used for assertions.
type formService struct {
	exp       *GExpect
	done      <-chan error
	rec       *byteRecorder
	logPath   string
	gatePath  string
	releaseFn func()
}

func newFormService(t *testing.T) *formService {
	t.Helper()
	dir := t.TempDir()
	logPath := dir + "/records.log"
	gatePath := dir + "/confirm.gate"
	releaseCh := make(chan struct{})
	exe, err := os.Executable()
	if err != nil {
		t.Fatalf("os.Executable: %v", err)
	}
	cmd := exec.Command(exe, "-test.run=^TestFormChildEntry$", "--", "form-child")
	cmd.Env = append(os.Environ(), "FORMTEST_LOG="+logPath, "FORMTEST_GATE="+gatePath)
	in, err := cmd.StdinPipe()
	if err != nil {
		t.Fatalf("StdinPipe: %v", err)
	}
	out, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatalf("StdoutPipe: %v", err)
	}
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatalf("start child: %v", err)
	}
	rec := &byteRecorder{w: in}
	var alive int32 = 1
	exp, done, err := SpawnGeneric(&GenOptions{
		In:  rec,
		Out: out,
		Wait: func() error {
			err := cmd.Wait()
			atomic.StoreInt32(&alive, 0)
			return err
		},
		Close: func() error { return rec.Close() },
		Check: func() bool { return atomic.LoadInt32(&alive) != 0 },
	}, time.Second, CheckDuration(time.Millisecond), PartialMatch(true))
	if err != nil {
		t.Fatalf("SpawnGeneric(form): %v", err)
	}
	gateStarted := false
	releaseFn := func() {
		if gateStarted {
			return
		}
		gateStarted = true
		_ = os.WriteFile(gatePath+".release", []byte("go"), 0666)
		close(releaseCh)
	}
	return &formService{exp: exp, done: done, rec: rec, logPath: logPath, gatePath: gatePath, releaseFn: releaseFn}
}

// waitAmountReceived blocks until the child reports it read the amount, bounded.
func (f *formService) waitAmountReceived(t *testing.T) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(f.gatePath + ".amount"); err == nil {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Logf("child never reported the amount; proceeding")
}

func (f *formService) records() []string {
	data, err := os.ReadFile(f.logPath)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		return []string{"<read error: " + err.Error() + ">"}
	}
	var out []string
	for _, line := range strings.Split(strings.TrimSpace(string(data)), "\n") {
		if line != "" {
			out = append(out, line)
		}
	}
	return out
}

// TestFormChildEntry is the child process entry point; it does real work only when
// explicitly invoked with the trailing "form-child" argument.
func TestFormChildEntry(t *testing.T) {
	if len(os.Args) < 2 || os.Args[len(os.Args)-1] != "form-child" {
		t.Skip("helper process entry")
	}
	os.Exit(formChild())
}

func TestBatchContextSession(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("businessFormCancelBeforeConfirm", func(t *testing.T) {
			f := newFormService(t)
			defer f.exp.Close()
			defer f.releaseFn()

			// Fixed structured input for the automation.
			type formInput struct {
				Ticket            string
				Amount            string
				RevokeAfterAmount bool
			}
			input := formInput{Ticket: "T-1042", Amount: "250.00", RevokeAfterAmount: true}

			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			// Ordering handshake: the child announces that it read the amount and
			// is holding the confirmation prompt open. Cancel only then, so the
			// batch is genuinely waiting mid-CONFIRM>.
			if input.RevokeAfterAmount {
				go func() {
					f.waitAmountReceived(t)
					cancel()
				}()
			}

			res, err := f.exp.ExpectBatchContext(ctx, []Batcher{
				&BExp{R: `TICKET> `},
				&BSnd{S: input.Ticket + "\n"},
				&BExp{R: `AMOUNT> `},
				&BSnd{S: input.Amount + "\n"},
				&BExp{R: `CONFIRM> `},
				&BSnd{S: "yes\n"},
				&BExp{R: `RECEIPT> `},
			}, 5*time.Second)

			c.Check(errors.Is(err, context.Canceled), "cancel before confirm must be context.Canceled, got %v", err)
			c.Check(fmt.Sprint(resIndices(res)) == "[0 2 4]", "BatchRes indices must describe completed and pending steps, got %v", resIndices(res))
			if len(res) == 3 {
				c.Check(res[0].Match != nil && res[0].Match[0] == "TICKET> ", "ticket match preserved, got %v", res[0].Match)
				c.Check(res[1].Match != nil && res[1].Match[0] == "AMOUNT> ", "amount match preserved, got %v", res[1].Match)
				c.Check(res[2].Match == nil, "pending step must have nil Match, got %v", res[2].Match)
				c.Check(strings.Count(res[2].Output, "CONFIRM") == 1, "pending output should hold the partial CONFIRM prompt once, got %q", res[2].Output)
			}
			// The simulated service must not have a new business record, and the
			// confirmation must never have reached it.
			c.Check(len(f.records()) == 0, "canceled batch must not submit a business record, got %v", f.records())
			raw := f.rec.String()
			c.Check(strings.Contains(raw, "T-1042\n"), "ticket bytes should have been sent, got %q", raw)
			c.Check(strings.Contains(raw, "250.00\n"), "amount bytes should have been sent, got %q", raw)
			c.Check(!strings.Contains(raw, "yes"), "confirmation must never be sent, got %q", raw)
			c.Check(raw == "T-1042\n250.00\n", "raw bytes after cancellation must be exactly the two entered fields, got %q", raw)

			// The service now releases the rest of the prompt; the same session
			// continues the entry, matching across the restored "CONFIRM" and the
			// newly arrived "> ".
			f.releaseFn()
			ctx2, cancel2 := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel2()
			res2, err := f.exp.ExpectBatchContext(ctx2, []Batcher{
				&BExp{R: `CONFIRM> `},
				&BSnd{S: "yes\n"},
				&BExp{R: `RECEIPT> `},
			}, 5*time.Second)
			c.Check(err == nil, "resumed batch should complete: %v", err)
			c.Check(fmt.Sprint(resIndices(res2)) == "[0 2]", "resumed indices: %v", resIndices(res2))
			if len(res2) == 2 {
				c.Check(res2[0].Match != nil && res2[0].Match[0] == "CONFIRM> ", "resumed CONFIRM match: %v", res2[0].Match)
				c.Check(res2[1].Match != nil && res2[1].Match[0] == "RECEIPT> ", "resumed RECEIPT match: %v", res2[1].Match)
			}
			recs := f.records()
			c.Check(len(recs) == 1, "resumed batch must submit exactly one record, got %v", recs)
			if len(recs) == 1 {
				c.Check(recs[0] == "COMMIT T-1042 250.00", "business record mismatch: %q", recs[0])
			}
			c.Check(strings.Contains(f.rec.String(), "yes\n"), "confirmation bytes should be sent after resume, got %q", f.rec.String())
			c.Check(f.rec.String() == "T-1042\n250.00\nyes\n", "full raw byte record across both batches mismatch: %q", f.rec.String())
			select {
			case <-f.done:
			case <-time.After(3 * time.Second):
				c.Check(false, "child service did not exit")
			}
		})

		t.Run("revocationAndAutoAnswer", func(t *testing.T) {
			s := newPipeSession(t, PartialMatch(true))
			defer s.close()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			go func() {
				s.write("AUTH> ")
				if !s.expectInput("token-7\n") {
					cancel()
					return
				}
				// The case auto-answered and retries waiting for TICKET>; revoke now.
				cancel()
			}()
			res, err := s.exp.ExpectBatchContext(ctx, []Batcher{
				&BCas{C: []Caser{
					&Case{R: regexp.MustCompile(`AUTH> `), S: "token-7\n", T: Continue(NewStatus(0, "answered")), Rt: 1},
					&Case{R: regexp.MustCompile(`TICKET> `), S: "", T: OK()},
				}},
			}, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "revoked auto-answer batch must be context.Canceled, got %v", err)
			c.Check(s.sentBytes() == "token-7\n", "auto answer must be sent exactly once, got %q", s.sentBytes())
			c.Check(len(res) == 1 && res[0].Idx == 0, "pending step result: %+v", res)
			if len(res) == 1 {
				c.Check(res[0].Match == nil, "pending step match must be nil, got %v", res[0].Match)
			}
			c.Check(s.noInputWithin(150*time.Millisecond), "no later action may be delivered after revocation")
		})
	})
}
