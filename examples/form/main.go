// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Command form demonstrates a cancellable business form entry flow on top of
// expect.ExpectBatchContext.
//
// The automation reads fixed structured input, drives a local child process
// that prompts for a ticket number, an amount and a final confirmation, and
// reports per-batch completed/canceled/failed status together with the steps
// that were completed. The child simulates the business service: it keeps the
// raw bytes it received and only appends a business record when a confirmation
// was actually submitted. Everything runs locally; no real system is contacted.
package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	expect "github.com/google/goexpect"
)

const (
	formChildEnv  = "GOEXPECT_FORM_CHILD"
	formLogEnv    = "GOEXPECT_FORM_LOG"
	formGateEnv   = "GOEXPECT_FORM_GATE"
	promptTimeout = 5 * time.Second
)

// formInput is the fixed structured input read by the automation.
type formInput struct {
	Ticket string
	Amount string
	// RevokeAfterAmount simulates the upper layer withdrawing the request
	// after the amount was entered but before the confirmation is sent.
	RevokeAfterAmount bool
}

// batchOutcome is the per-batch report printed by the automation.
type batchOutcome struct {
	Status string // completed, canceled or failed
	Steps  []string
	Res    []expect.BatchRes
	Err    error
}

func main() {
	if os.Getenv(formChildEnv) == "1" {
		os.Exit(runFormChild())
	}

	// Fixed structured input. In a real system this comes from the request;
	// here it is fixed so the demo is reproducible.
	input := formInput{
		Ticket:            "T-1042",
		Amount:            "250.00",
		RevokeAfterAmount: len(os.Args) > 1 && os.Args[1] == "revoke",
	}

	svc, err := startFormService(input.RevokeAfterAmount)
	if err != nil {
		fmt.Fprintf(os.Stderr, "start form service: %v\n", err)
		os.Exit(1)
	}
	defer svc.Close()

	steps := map[int]string{
		0: "ticket prompt matched",
		2: "amount prompt matched",
		4: "confirm prompt matched",
		6: "receipt matched",
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	first := submitFirstBatch(ctx, cancel, svc, input)
	printOutcome("first batch", first, steps)

	if errors.Is(first.Err, context.Canceled) && input.RevokeAfterAmount {
		// The withdrawn request must not send the confirmation, so no new
		// business record may exist at this point.
		fmt.Printf("records after canceled batch: %v\n", svc.Records())
		if n := len(svc.Records()); n != 0 {
			fmt.Fprintf(os.Stderr, "unexpected record after cancellation: %d\n", n)
			os.Exit(1)
		}

		// If the withdrawal is later lifted, the same session can finish the
		// entry. The restored (partial) confirmation prompt feeds the next
		// batch once the service releases the rest.
		svc.releaseConfirmationPrompt()
		resumedCtx, resumeCancel := context.WithTimeout(context.Background(), 3*promptTimeout)
		defer resumeCancel()
		resumed := resumeConfirmation(resumedCtx, svc)
		printOutcome("resumed batch", resumed, map[int]string{0: "confirm prompt matched", 2: "receipt matched"})
		if resumed.Err != nil {
			os.Exit(1)
		}
	} else if first.Err != nil {
		os.Exit(1)
	}

	fmt.Printf("records: %v\n", svc.Records())
	fmt.Printf("raw input bytes: %q\n", svc.RawInput())
}

// submitFirstBatch drives the ticket, amount and confirmation steps. When the
// request is revoked the context is canceled after the amount is entered, so
// the confirmation send is never submitted.
func submitFirstBatch(ctx context.Context, cancel context.CancelFunc, svc *formService, input formInput) batchOutcome {
	batch := []expect.Batcher{
		&expect.BExp{R: `TICKET> `},
		&expect.BSnd{S: input.Ticket + "\n"},
		&expect.BExp{R: `AMOUNT> `},
		&expect.BSnd{S: input.Amount + "\n"},
		&expect.BExp{R: `CONFIRM> `},
		&expect.BSnd{S: "yes\n"},
		&expect.BExp{R: `RECEIPT> `},
	}
	if input.RevokeAfterAmount {
		// The upper layer withdraws the request once the service reports that
		// the amount was read and is holding the confirmation prompt. This is
		// a real ordering handshake, not a fixed sleep.
		svc.AfterAmountReceived(func() {
			cancel()
		})
	}
	res, err := svc.ExpectBatchContext(ctx, batch, promptTimeout)
	return classify(res, err)
}

// resumeConfirmation finishes the form entry on the same session.
func resumeConfirmation(ctx context.Context, svc *formService) batchOutcome {
	res, err := svc.ExpectBatchContext(ctx, []expect.Batcher{
		&expect.BExp{R: `CONFIRM> `},
		&expect.BSnd{S: "yes\n"},
		&expect.BExp{R: `RECEIPT> `},
	}, promptTimeout)
	return classify(res, err)
}

// classify maps a batch result to the completed/canceled/failed vocabulary and
// keeps every completed Expect step. The batch's last entry may be a canceled
// waiting step with a nil Match.
func classify(res []expect.BatchRes, err error) batchOutcome {
	out := batchOutcome{Res: res, Err: err}
	for _, r := range res {
		if r.Match != nil {
			out.Steps = append(out.Steps, fmt.Sprintf("idx=%d output=%q match=%q", r.Idx, r.Output, strings.Join(r.Match, "|")))
		}
	}
	switch {
	case err == nil:
		out.Status = "completed"
	case errors.Is(err, context.Canceled), errors.Is(err, context.DeadlineExceeded):
		out.Status = "canceled"
	default:
		out.Status = "failed"
	}
	return out
}

func printOutcome(label string, o batchOutcome, names map[int]string) {
	fmt.Printf("%s: %s\n", label, o.Status)
	for _, r := range o.Res {
		name := names[r.Idx]
		if name == "" {
			name = "step"
		}
		if r.Match == nil {
			fmt.Printf("  idx=%d %s: pending output=%q (match=nil)\n", r.Idx, name, r.Output)
			continue
		}
		fmt.Printf("  idx=%d %s: match=%q output=%q\n", r.Idx, name, r.Match[0], r.Output)
	}
	if o.Err != nil {
		fmt.Printf("  error: %v\n", o.Err)
	}
}

// -----------------------------------------------------------------------------
// Simulated form service: a local child process.
// -----------------------------------------------------------------------------

// runFormChild is the service side. It prompts for ticket, amount and
// confirmation, keeps the raw bytes it read, and appends a business record only
// for a confirmed form.
func runFormChild() int {
	logPath := os.Getenv(formLogEnv)
	logFile, err := os.OpenFile(logPath, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0600)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open %q: %v\n", logPath, err)
		return 2
	}
	defer logFile.Close()

	r := bufio.NewReader(os.Stdin)
	var raw strings.Builder
	readLine := func() (string, bool) {
		line, err := r.ReadString('\n')
		raw.WriteString(line)
		if err != nil {
			return strings.TrimSuffix(line, "\n"), false
		}
		return strings.TrimSuffix(line, "\n"), true
	}
	prompt := func(s string) bool {
		_, err := io.WriteString(os.Stdout, s)
		return err == nil
	}

	if !prompt("TICKET> ") {
		return 2
	}
	ticket, ok := readLine()
	if !ok {
		return 2
	}
	if !prompt("AMOUNT> ") {
		return 2
	}
	amount, ok := readLine()
	if !ok {
		return 2
	}
	// In revoke mode the service emits the first half of the confirmation
	// prompt, then holds the rest back until the automation's cancellation has
	// landed. This is a deliberate handshake so the demo shows the
	// canceled-between-amount-and-confirm case deterministically.
	if !prompt("CONFIRM") {
		return 2
	}
	if gate := os.Getenv(formGateEnv); gate != "" {
		if err := os.WriteFile(gate+".amount", []byte("received"), 0600); err != nil {
			fmt.Fprintf(os.Stderr, "write gate marker: %v\n", err)
			return 2
		}
		for {
			if _, err := os.Stat(gate + ".release"); err == nil {
				break
			}
			time.Sleep(time.Millisecond)
		}
	}
	if !prompt("> ") {
		return 2
	}
	confirm, ok := readLine()
	if !ok {
		return 2
	}
	if confirm == "yes" {
		fmt.Fprintf(logFile, "COMMIT ticket=%s amount=%s\n", ticket, amount)
	}
	// Keep the raw bytes next to the records so the demo can show exactly what
	// the service received, including bytes from a canceled batch.
	if err := os.WriteFile(logPath+".raw", []byte(raw.String()), 0600); err != nil {
		fmt.Fprintf(os.Stderr, "write raw log: %v\n", err)
		return 2
	}
	if !prompt("RECEIPT> ") {
		return 2
	}
	return 0
}

// formService wraps a spawned child process and exposes its records.
type formService struct {
	exp     *expect.GExpect
	done    <-chan error
	logPath string
	gate    string
	// release tells the wait function that the demo is done with the child.
	release chan<- error

	mu  sync.Mutex
	raw strings.Builder
}

func startFormService(revoke bool) (*formService, error) {
	exe, err := os.Executable()
	if err != nil {
		return nil, err
	}
	logPath, err := os.CreateTemp("", "goexpect-form-*.log")
	if err != nil {
		return nil, err
	}
	logPath.Close()
	name := logPath.Name()
	os.Remove(name)
	release := make(chan error, 1)

	cmd := exec.Command(exe)
	cmd.Stderr = os.Stderr

	svc := &formService{logPath: name, release: release}
	if revoke {
		svc.gate = name + ".gate"
	}
	env := append(os.Environ(), formChildEnv+"=1", formLogEnv+"="+name)
	if svc.gate != "" {
		env = append(env, formGateEnv+"="+svc.gate)
	}
	cmd.Env = env
	in, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	out, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}

	rec := &recordingWriter{w: in, raw: &svc.raw, mu: &svc.mu}
	var alive int32 = 1
	exp, done, err := expect.SpawnGeneric(&expect.GenOptions{
		In:  rec,
		Out: out,
		Wait: func() error {
			<-release
			err := cmd.Wait()
			atomic.StoreInt32(&alive, 0)
			return err
		}, Close: func() error { return rec.Close() },
		Check: func() bool { return atomic.LoadInt32(&alive) != 0 },
	}, time.Minute, expect.CheckDuration(time.Millisecond), expect.PartialMatch(true))
	if err != nil {
		return nil, err
	}
	svc.exp = exp
	svc.done = done
	return svc, nil
}

func (s *formService) ExpectBatchContext(ctx context.Context, batch []expect.Batcher, timeout time.Duration) ([]expect.BatchRes, error) {
	return s.exp.ExpectBatchContext(ctx, batch, timeout)
}

// AfterAmountReceived invokes fn once the service has read the amount and is
// holding the confirmation prompt. It is the demo's ordering handshake for the
// withdrawal.
func (s *formService) AfterAmountReceived(fn func()) {
	go func() {
		deadline := time.Now().Add(promptTimeout)
		for time.Now().Before(deadline) {
			if _, err := os.Stat(s.gate + ".amount"); err == nil {
				fn()
				return
			}
			time.Sleep(time.Millisecond)
		}
	}()
}

// releaseConfirmationPrompt lets the service continue with the rest of the
// confirmation prompt after a withdrawal, so the same session can resume.
func (s *formService) releaseConfirmationPrompt() {
	if s.gate == "" {
		return
	}
	_ = os.WriteFile(s.gate+".release", []byte("go"), 0600)
}

func (s *formService) RawInput() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.raw.String()
}

func (s *formService) Records() []string {
	data, err := os.ReadFile(s.logPath)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		return []string{"<error: " + err.Error() + ">"}
	}
	var out []string
	for _, line := range strings.Split(strings.TrimSpace(string(data)), "\n") {
		if line != "" {
			out = append(out, line)
		}
	}
	return out
}

func (s *formService) Close() error {
	select {
	case s.release <- nil:
	default:
	}
	return s.exp.Close()
}

// recordingWriter keeps the exact bytes the write goroutine handed to the
// service's stdin.
type recordingWriter struct {
	w   io.WriteCloser
	raw *strings.Builder
	mu  *sync.Mutex
}

func (r *recordingWriter) Write(p []byte) (int, error) {
	r.mu.Lock()
	r.raw.Write(p)
	r.mu.Unlock()
	return r.w.Write(p)
}

func (r *recordingWriter) Close() error { return r.w.Close() }
