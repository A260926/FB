package rod_test

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/go-rod/rod"
	"github.com/go-rod/rod/lib/cdp"
	"github.com/go-rod/rod/lib/proto"
)

// scriptCall is one recorded protocol request.
type scriptCall struct {
	session string
	method  string
	params  interface{}
}

// dialogRecorder is a rod.CDPClient that drives a real rod.Browser/Page
// without a browser. Channels control when opening events, handle-command
// responses and closed events happen.
type dialogRecorder struct {
	events chan *cdp.Event

	mu    sync.Mutex
	calls []scriptCall

	// Configuration of the recorder; guarded separately because the test
	// mutates it while protocol calls are in flight.
	cfgMu sync.RWMutex
	// handleGate, when non-nil, blocks every Page.handleJavaScriptDialog call
	// until it is closed, so a closed event can overtake the response.
	handleGate chan struct{}
	// handleErr fails every handle command when non-nil.
	handleErr error
	// dismissErr fails only dismiss (Accept == false) handle commands.
	dismissErr error

	handleCount int32
}

func newDialogRecorder() *dialogRecorder {
	return &dialogRecorder{events: make(chan *cdp.Event)}
}

func (r *dialogRecorder) Event() <-chan *cdp.Event { return r.events }

func (r *dialogRecorder) setGate(gate chan struct{}) {
	r.cfgMu.Lock()
	defer r.cfgMu.Unlock()
	r.handleGate = gate
}

func (r *dialogRecorder) closeGate() {
	r.cfgMu.RLock()
	gate := r.handleGate
	r.cfgMu.RUnlock()
	close(gate)
}

func (r *dialogRecorder) setHandleErr(err error) {
	r.cfgMu.Lock()
	defer r.cfgMu.Unlock()
	r.handleErr = err
}

func (r *dialogRecorder) Call(ctx context.Context, sessionID, method string, params interface{}) ([]byte, error) {
	r.mu.Lock()
	r.calls = append(r.calls, scriptCall{session: sessionID, method: method, params: params})
	r.mu.Unlock()

	if method == (proto.PageHandleJavaScriptDialog{}).ProtoReq() {
		atomic.AddInt32(&r.handleCount, 1)

		r.cfgMu.RLock()
		gate := r.handleGate
		handleErr := r.handleErr
		dismissErr := r.dismissErr
		r.cfgMu.RUnlock()

		if gate != nil {
			select {
			case <-gate:
			case <-ctx.Done():
				return nil, ctx.Err()
			}
		}

		req := params.(proto.PageHandleJavaScriptDialog)
		if !req.Accept && dismissErr != nil {
			return nil, dismissErr
		}
		if handleErr != nil {
			return nil, handleErr
		}
	}

	return []byte(`{}`), nil
}

func (r *dialogRecorder) snapshot() []scriptCall {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]scriptCall, len(r.calls))
	copy(out, r.calls)
	return out
}

func (r *dialogRecorder) countMethod(method string) int {
	n := 0
	for _, c := range r.snapshot() {
		if c.method == method {
			n++
		}
	}
	return n
}

// waitForCall blocks until the nth (zero-based) call of method is recorded.
func (r *dialogRecorder) waitForCall(ctx context.Context, t *testing.T, method string, nth int) scriptCall {
	t.Helper()
	for {
		calls := r.snapshot()
		seen := 0
		for _, c := range calls {
			if c.method == method {
				if seen == nth {
					return c
				}
				seen++
			}
		}
		select {
		case <-ctx.Done():
			t.Fatalf("wait for %s #%d timed out, calls: %+v", method, nth, calls)
		case <-time.After(time.Millisecond * 2):
		}
	}
}

func (r *dialogRecorder) emitOpening(ctx context.Context, session string, e proto.PageJavascriptDialogOpening) {
	data, err := json.Marshal(e)
	if err != nil {
		panic(err)
	}
	select {
	case r.events <- &cdp.Event{SessionID: session, Method: e.ProtoEvent(), Params: data}:
	case <-ctx.Done():
	}
}

func (r *dialogRecorder) emitClosed(ctx context.Context, session string, e proto.PageJavascriptDialogClosed) {
	data, err := json.Marshal(e)
	if err != nil {
		panic(err)
	}
	select {
	case r.events <- &cdp.Event{SessionID: session, Method: e.ProtoEvent(), Params: data}:
	case <-ctx.Done():
	}
}

func newScriptBrowser(t *testing.T, ctx context.Context, rec rod.CDPClient) *rod.Browser {
	t.Helper()
	b := rod.New().Context(ctx).ControlURL("").NoDefaultDevice().Client(rec)
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	return b
}

func scriptOpeningURL() string { return "https://form.invalid/" }

func scriptSteps() []rod.DialogStep {
	return []rod.DialogStep{
		{Type: proto.PageDialogTypeConfirm, URL: scriptOpeningURL(), Message: "sure?", Accept: true},
		{Type: proto.PageDialogTypePrompt, URL: scriptOpeningURL(), Message: "name?", Accept: true, PromptText: "sent-name"},
		{Type: proto.PageDialogTypeAlert, URL: scriptOpeningURL(), Message: "done", Accept: false},
	}
}

// scriptOpenings are the opening events emitted for scriptSteps.
func scriptOpenings() []proto.PageJavascriptDialogOpening {
	return []proto.PageJavascriptDialogOpening{
		{URL: scriptOpeningURL(), Message: "sure?", Type: proto.PageDialogTypeConfirm},
		{URL: scriptOpeningURL(), Message: "name?", Type: proto.PageDialogTypePrompt},
		{URL: scriptOpeningURL(), Message: "done", Type: proto.PageDialogTypeAlert},
	}
}

// scriptClosed carry actual values that deliberately differ from the
// Accept/PromptText we send, so the test proves results come from events.
func scriptClosed() []proto.PageJavascriptDialogClosed {
	return []proto.PageJavascriptDialogClosed{
		{Result: true, UserInput: ""},
		{Result: true, UserInput: "actual-name"},
		{Result: false, UserInput: ""},
	}
}

func assertHandleCall(t *testing.T, c scriptCall, session string, accept bool, promptText string) {
	t.Helper()
	if c.session != session {
		t.Fatalf("handle session = %q, want %q", c.session, session)
	}
	req := c.params.(proto.PageHandleJavaScriptDialog)
	if req.Accept != accept || req.PromptText != promptText {
		t.Fatalf("handle params = %+v, want accept=%v promptText=%q", req, accept, promptText)
	}
}

// runScript runs WithDialogScript concurrently and returns a result channel.
func runScript(p *rod.Page, steps []rod.DialogStep, action func(*rod.Page) error) <-chan struct {
	results []rod.DialogResult
	err     error
} {
	ch := make(chan struct {
		results []rod.DialogResult
		err     error
	}, 1)
	go func() {
		results, err := p.WithDialogScript(steps, action)
		ch <- struct {
			results []rod.DialogResult
			err     error
		}{results, err}
	}()
	return ch
}

func waitScript(t *testing.T, ctx context.Context, ch <-chan struct {
	results []rod.DialogResult
	err     error
}) ([]rod.DialogResult, error) {
	t.Helper()
	select {
	case r := <-ch:
		return r.results, r.err
	case <-ctx.Done():
		t.Fatalf("WithDialogScript timed out: %v", ctx.Err())
		return nil, ctx.Err()
	}
}

// TestDialogScriptBothOrders covers the full confirm/prompt/alert sequence
// twice: once with the closed event after the command response and once
// before it. It checks ordering, single handling, actual event values, the
// success prefix, domain restore and reuse of the original page.
func TestDialogScriptBothOrders(t *testing.T) {
	for _, closedFirst := range []bool{false, true} {
		t.Run(fmt.Sprintf("closedFirst=%v", closedFirst), func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()

			rec := newDialogRecorder()
			if closedFirst {
				rec.setGate(make(chan struct{}))
			}
			b := newScriptBrowser(t, ctx, rec)
			page := b.PageFromSession("form")

			actionEntered := make(chan struct{}, 1)
			releaseAction := make(chan struct{})
			var actionRuns int32
			ch := runScript(page, scriptSteps(), func(p *rod.Page) error {
				atomic.AddInt32(&actionRuns, 1)
				if p.GetSessionID() != page.GetSessionID() {
					t.Errorf("action page session = %q, want %q", p.GetSessionID(), page.GetSessionID())
				}
				// A normal command issued from inside the action must still work.
				if err := (proto.RuntimeEnable{}).Call(p); err != nil {
					return err
				}
				actionEntered <- struct{}{}
				<-releaseAction
				return nil
			})

			select {
			case <-actionEntered:
			case <-ctx.Done():
				t.Fatalf("action never started: %v", ctx.Err())
			}

			openings := scriptOpenings()
			closed := scriptClosed()
			for i := range scriptSteps() {
				if closedFirst {
					// Block each handle command until its own closed event has
					// overtaken the response.
					rec.setGate(make(chan struct{}))
				}
				rec.emitOpening(ctx, "form", openings[i])
				handleCall := rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", i)
				step := scriptSteps()[i]
				assertHandleCall(t, handleCall, "form", step.Accept, step.PromptText)

				if closedFirst {
					// The blocked handle command has no response yet; the
					// closed event must overtake it and still be recorded.
					rec.emitClosed(ctx, "form", closed[i])
					rec.closeGate()
				} else {
					rec.emitClosed(ctx, "form", closed[i])
				}
			}

			if atomic.LoadInt32(&rec.handleCount) != int32(len(scriptSteps())) {
				t.Fatalf("handle count = %d, want %d", rec.handleCount, len(scriptSteps()))
			}

			close(releaseAction)
			results, err := waitScript(t, ctx, ch)
			if err != nil {
				t.Fatalf("WithDialogScript: %v", err)
			}
			if atomic.LoadInt32(&actionRuns) != 1 {
				t.Fatalf("action ran %d times, want exactly 1", actionRuns)
			}

			if len(results) != len(scriptSteps()) {
				t.Fatalf("results len = %d, want %d", len(results), len(scriptSteps()))
			}
			for i, res := range results {
				if res.Opening != openings[i] {
					t.Fatalf("result %d opening = %+v, want %+v", i, res.Opening, openings[i])
				}
				if res.Closed != closed[i] {
					t.Fatalf("result %d closed = %+v, want %+v", i, res.Closed, closed[i])
				}
			}
			// The prompt result must be the observed value, not our sent text.
			if results[1].Closed.UserInput == scriptSteps()[1].PromptText {
				t.Fatal("closed UserInput must come from the event, not PromptText")
			}
			if results[1].Closed.UserInput != "actual-name" {
				t.Fatalf("closed UserInput = %q", results[1].Closed.UserInput)
			}

			// Page domain was off (PageFromSession doesn't enable it): the
			// script enables it once and restores it once.
			if n := rec.countMethod("Page.enable"); n != 1 {
				t.Fatalf("Page.enable count = %d, want 1", n)
			}
			if n := rec.countMethod("Page.disable"); n != 1 {
				t.Fatalf("Page.disable count = %d, want 1", n)
			}

			// The original page must keep serving ordinary protocol commands.
			if err := (proto.PageStopLoading{}).Call(page); err != nil {
				t.Fatalf("page unusable after script: %v", err)
			}
			last := rec.snapshot()[len(rec.snapshot())-1]
			if last.session != "form" || last.method != "Page.stopLoading" {
				t.Fatalf("post-script command not routed to the page: %+v", last)
			}
		})
	}
}

// TestDialogScriptParkedOpening covers the ordering where the next opening
// arrives after the current dialog closed but while its handle command is
// still unacknowledged. The next opening must wait for the success prefix
// instead of being dismissed as a mismatch.
func TestDialogScriptParkedOpening(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	rec := newDialogRecorder()
	gate := make(chan struct{})
	rec.setGate(gate)
	b := newScriptBrowser(t, ctx, rec)
	page := b.PageFromSession("form")

	steps := []rod.DialogStep{
		{Type: proto.PageDialogTypeConfirm, URL: scriptOpeningURL(), Message: "sure?", Accept: true},
		{Type: proto.PageDialogTypePrompt, URL: scriptOpeningURL(), Message: "name?", Accept: true, PromptText: "sent-name"},
	}
	entered := make(chan struct{}, 1)
	ch := runScript(page, steps, func(*rod.Page) error {
		entered <- struct{}{}
		return nil
	})
	<-entered

	// First dialog: opening, then closed, but the handle command stays blocked.
	rec.emitOpening(ctx, "form", scriptOpenings()[0])
	rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 0)
	rec.emitClosed(ctx, "form", scriptClosed()[0])

	// Second opening arrives before the first command is acknowledged.
	rec.emitOpening(ctx, "form", scriptOpenings()[1])
	time.Sleep(20 * time.Millisecond)
	if atomic.LoadInt32(&rec.handleCount) != 1 {
		t.Fatalf("parked opening must not trigger another command, count=%d", rec.handleCount)
	}

	// Unblock the first command: step 0 completes and the parked opening is
	// handled with the second step's own settings.
	close(gate)
	second := rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 1)
	assertHandleCall(t, second, "form", true, "sent-name")
	rec.emitClosed(ctx, "form", scriptClosed()[1])

	results, err := waitScript(t, ctx, ch)
	if err != nil {
		t.Fatalf("WithDialogScript: %v", err)
	}
	if len(results) != 2 {
		t.Fatalf("results len = %d, want 2", len(results))
	}
	if results[0].Opening != scriptOpenings()[0] || results[1].Opening != scriptOpenings()[1] {
		t.Fatalf("openings not matched in order: %+v", results)
	}
	if atomic.LoadInt32(&rec.handleCount) != 2 {
		t.Fatalf("handle count = %d, want 2 (no dismiss)", rec.handleCount)
	}
}

// TestDialogScriptActionReturnsFirst proves steps still complete when action
// returns before every dialog has been handled.
func TestDialogScriptActionReturnsFirst(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	rec := newDialogRecorder()
	b := newScriptBrowser(t, ctx, rec)
	page := b.PageFromSession("form")

	entered := make(chan struct{}, 1)
	ch := runScript(page, scriptSteps()[:1], func(*rod.Page) error {
		entered <- struct{}{}
		return nil // action finishes before the single dialog appears
	})
	<-entered

	rec.emitOpening(ctx, "form", scriptOpenings()[0])
	rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 0)
	rec.emitClosed(ctx, "form", scriptClosed()[0])

	results, err := waitScript(t, ctx, ch)
	if err != nil {
		t.Fatalf("WithDialogScript: %v", err)
	}
	if len(results) != 1 || results[0].Closed != scriptClosed()[0] {
		t.Fatalf("unexpected results: %+v", results)
	}
}

// TestDialogScriptTwoPrompts handles two consecutive prompts with different
// input texts and checks each command and each actual closed value.
func TestDialogScriptTwoPrompts(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	rec := newDialogRecorder()
	b := newScriptBrowser(t, ctx, rec)
	page := b.PageFromSession("form")

	steps := []rod.DialogStep{
		{Type: proto.PageDialogTypePrompt, URL: scriptOpeningURL(), Message: "p1", Accept: true, PromptText: "one"},
		{Type: proto.PageDialogTypePrompt, URL: scriptOpeningURL(), Message: "p2", Accept: true, PromptText: "two"},
	}
	entered := make(chan struct{}, 1)
	ch := runScript(page, steps, func(*rod.Page) error {
		entered <- struct{}{}
		return nil
	})
	<-entered

	rec.emitOpening(ctx, "form", proto.PageJavascriptDialogOpening{
		URL: scriptOpeningURL(), Message: "p1", Type: proto.PageDialogTypePrompt,
	})
	first := rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 0)
	assertHandleCall(t, first, "form", true, "one")
	rec.emitClosed(ctx, "form", proto.PageJavascriptDialogClosed{Result: true, UserInput: "echo-one"})

	rec.emitOpening(ctx, "form", proto.PageJavascriptDialogOpening{
		URL: scriptOpeningURL(), Message: "p2", Type: proto.PageDialogTypePrompt,
	})
	second := rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 1)
	assertHandleCall(t, second, "form", true, "two")
	rec.emitClosed(ctx, "form", proto.PageJavascriptDialogClosed{Result: true, UserInput: "echo-two"})

	results, err := waitScript(t, ctx, ch)
	if err != nil {
		t.Fatalf("WithDialogScript: %v", err)
	}
	if len(results) != 2 {
		t.Fatalf("results len = %d", len(results))
	}
	if results[0].Closed.UserInput != "echo-one" || results[1].Closed.UserInput != "echo-two" {
		t.Fatalf("closed values not matched per step: %+v", results)
	}
	if atomic.LoadInt32(&rec.handleCount) != 2 {
		t.Fatalf("handle count = %d, want 2", rec.handleCount)
	}
}

// TestDialogScriptMismatch covers a wrong dialog and an extra dialog after
// the steps are exhausted, including a failing dismiss.
func TestDialogScriptMismatch(t *testing.T) {
	t.Run("wrong-message", func(t *testing.T) {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		rec := newDialogRecorder()
		b := newScriptBrowser(t, ctx, rec)
		page := b.PageFromSession("form")

		steps := scriptSteps()[:1]
		entered := make(chan struct{}, 1)
		ch := runScript(page, steps, func(*rod.Page) error {
			entered <- struct{}{}
			return nil
		})
		<-entered

		wrong := proto.PageJavascriptDialogOpening{
			URL:     scriptOpeningURL(),
			Message: "different",
			Type:    proto.PageDialogTypeConfirm,
		}
		rec.emitOpening(ctx, "form", wrong)

		results, err := waitScript(t, ctx, ch)
		var mismatch *rod.DialogMismatchError
		if !errors.As(err, &mismatch) {
			t.Fatalf("want DialogMismatchError, got %v", err)
		}
		if mismatch.Index != 0 {
			t.Fatalf("mismatch index = %d, want 0", mismatch.Index)
		}
		if mismatch.Expected == nil || mismatch.Expected.Message != "sure?" || mismatch.Actual != wrong {
			t.Fatalf("mismatch details = %+v", mismatch)
		}
		if len(results) != 0 {
			t.Fatalf("success prefix = %d, want 0", len(results))
		}
		// Exactly one dismiss for the current dialog.
		dismiss := rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 0)
		assertHandleCall(t, dismiss, "form", false, "")
		if atomic.LoadInt32(&rec.handleCount) != 1 {
			t.Fatalf("handle count = %d, want 1", rec.handleCount)
		}
		if err := (proto.PageStopLoading{}).Call(page); err != nil {
			t.Fatalf("session unusable after mismatch: %v", err)
		}
	})

	t.Run("extra-dialog", func(t *testing.T) {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		rec := newDialogRecorder()
		b := newScriptBrowser(t, ctx, rec)
		page := b.PageFromSession("form")

		steps := scriptSteps()[:1]
		entered := make(chan struct{}, 1)
		releaseAction := make(chan struct{})
		ch := runScript(page, steps, func(*rod.Page) error {
			entered <- struct{}{}
			<-releaseAction
			return nil
		})
		<-entered

		rec.emitOpening(ctx, "form", scriptOpenings()[0])
		rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 0)
		rec.emitClosed(ctx, "form", scriptClosed()[0])

		extra := proto.PageJavascriptDialogOpening{
			URL: scriptOpeningURL(), Message: "extra", Type: proto.PageDialogTypeAlert,
		}
		rec.emitOpening(ctx, "form", extra)

		results, err := waitScript(t, ctx, ch)
		close(releaseAction)
		var mismatch *rod.DialogMismatchError
		if !errors.As(err, &mismatch) {
			t.Fatalf("want DialogMismatchError, got %v", err)
		}
		if mismatch.Index != 1 || mismatch.Expected != nil || mismatch.Actual != extra {
			t.Fatalf("mismatch details = %+v", mismatch)
		}
		if len(results) != 1 {
			t.Fatalf("success prefix = %d, want 1", len(results))
		}
		// One accept plus one dismiss, nothing more.
		if atomic.LoadInt32(&rec.handleCount) != 2 {
			t.Fatalf("handle count = %d, want 2", rec.handleCount)
		}
	})

	t.Run("dismiss-fails", func(t *testing.T) {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		rec := newDialogRecorder()
		rec.dismissErr = errors.New("dismiss boom") // set before start, no concurrent call
		b := newScriptBrowser(t, ctx, rec)
		page := b.PageFromSession("form")

		entered := make(chan struct{}, 1)
		ch := runScript(page, scriptSteps()[:1], func(*rod.Page) error {
			entered <- struct{}{}
			return nil
		})
		<-entered
		rec.emitOpening(ctx, "form", proto.PageJavascriptDialogOpening{
			URL: scriptOpeningURL(), Message: "different", Type: proto.PageDialogTypeConfirm,
		})

		_, err := waitScript(t, ctx, ch)
		var mismatch *rod.DialogMismatchError
		if !errors.As(err, &mismatch) {
			t.Fatalf("mismatch must stay discoverable after dismiss failure, got %v", err)
		}
	})
}

// TestDialogScriptHandleFailure checks that a failed handle command returns
// the wrapped original error and the empty prefix, sends no second command
// and leaves the session usable.
func TestDialogScriptHandleFailure(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	rec := newDialogRecorder()
	handleErr := errors.New("handle boom")
	rec.setHandleErr(handleErr)
	b := newScriptBrowser(t, ctx, rec)
	page := b.PageFromSession("form")

	actionEntered := make(chan struct{}, 1)
	releaseAction := make(chan struct{})
	ch := runScript(page, scriptSteps(), func(*rod.Page) error {
		actionEntered <- struct{}{}
		<-releaseAction
		return nil
	})

	<-actionEntered
	rec.emitOpening(ctx, "form", scriptOpenings()[0])

	results, err := waitScript(t, ctx, ch)
	close(releaseAction)

	if !errors.Is(err, handleErr) {
		t.Fatalf("want original handle error, got %v", err)
	}
	if len(results) != 0 {
		t.Fatalf("prefix len = %d, want 0", len(results))
	}
	if atomic.LoadInt32(&rec.handleCount) != 1 {
		t.Fatalf("handle count = %d, want 1 (no replay)", rec.handleCount)
	}

	rec.setHandleErr(nil)
	if err := (proto.PageStopLoading{}).Call(page); err != nil {
		t.Fatalf("session unusable after handle failure: %v", err)
	}
}

// TestDialogScriptActionFailure returns the action's own error without
// replaying it or any dialog.
func TestDialogScriptActionFailure(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	rec := newDialogRecorder()
	b := newScriptBrowser(t, ctx, rec)
	page := b.PageFromSession("form")

	actionErr := errors.New("action boom")
	var runs int32
	_, err := page.WithDialogScript(scriptSteps()[:1], func(*rod.Page) error {
		atomic.AddInt32(&runs, 1)
		return actionErr
	})
	if !errors.Is(err, actionErr) {
		t.Fatalf("want action error, got %v", err)
	}
	if atomic.LoadInt32(&runs) != 1 {
		t.Fatalf("action ran %d times", runs)
	}
	if atomic.LoadInt32(&rec.handleCount) != 0 {
		t.Fatalf("no dialog was expected, handled %d", rec.handleCount)
	}
	if err := (proto.PageStopLoading{}).Call(page); err != nil {
		t.Fatalf("session unusable after action failure: %v", err)
	}
}

// TestDialogScriptReentry ensures a second script on a same-session Context
// clone fails before its action can run, while the first script completes.
func TestDialogScriptReentry(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	rec := newDialogRecorder()
	b := newScriptBrowser(t, ctx, rec)
	page := b.PageFromSession("form")
	clone := page.Context(ctx)

	firstEntered := make(chan struct{}, 1)
	releaseFirst := make(chan struct{})
	ch := runScript(page, scriptSteps()[:1], func(*rod.Page) error {
		firstEntered <- struct{}{}
		<-releaseFirst
		return nil
	})
	<-firstEntered

	var secondActionRan int32
	_, err := clone.WithDialogScript(scriptSteps()[:1], func(*rod.Page) error {
		atomic.StoreInt32(&secondActionRan, 1)
		return nil
	})
	if err == nil {
		t.Fatal("reentrant WithDialogScript must fail")
	}
	if atomic.LoadInt32(&secondActionRan) != 0 {
		t.Fatal("the second action must not run")
	}

	// The first script is unaffected and finishes normally.
	rec.emitOpening(ctx, "form", scriptOpenings()[0])
	rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 0)
	rec.emitClosed(ctx, "form", scriptClosed()[0])
	close(releaseFirst)

	if _, err := waitScript(t, ctx, ch); err != nil {
		t.Fatalf("first script: %v", err)
	}

	// After release, another script on the same session can run again.
	secondEntered := make(chan struct{}, 1)
	ch2 := runScript(page, scriptSteps()[:1], func(*rod.Page) error {
		secondEntered <- struct{}{}
		return nil
	})
	<-secondEntered
	rec.emitOpening(ctx, "form", scriptOpenings()[0])
	rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 1)
	rec.emitClosed(ctx, "form", scriptClosed()[0])
	if _, err := waitScript(t, ctx, ch2); err != nil {
		t.Fatalf("second script after release: %v", err)
	}
}

// TestDialogScriptTwoSessions runs identical-message dialogs on two sessions
// of one browser at the same time; events and commands must not cross.
func TestDialogScriptTwoSessions(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	rec := newDialogRecorder()
	b := newScriptBrowser(t, ctx, rec)
	pageA := b.PageFromSession("A")
	pageB := b.PageFromSession("B")

	steps := []rod.DialogStep{{
		Type: proto.PageDialogTypeConfirm, URL: scriptOpeningURL(), Message: "same", Accept: true,
	}}
	opening := proto.PageJavascriptDialogOpening{
		URL: scriptOpeningURL(), Message: "same", Type: proto.PageDialogTypeConfirm,
	}
	// Distinct closed values per session prove a sibling's closed event can
	// never complete another session's step.
	closedA := proto.PageJavascriptDialogClosed{Result: true, UserInput: "from-A"}
	closedB := proto.PageJavascriptDialogClosed{Result: false, UserInput: "from-B"}

	var wg sync.WaitGroup
	errs := make(chan error, 2)
	enteredA := make(chan struct{}, 1)
	enteredB := make(chan struct{}, 1)
	wg.Add(2)
	go func() {
		defer wg.Done()
		var entered bool
		results, err := pageA.WithDialogScript(steps, func(*rod.Page) error {
			if !entered {
				entered = true
				close(enteredA)
			}
			return nil
		})
		if err == nil && (len(results) != 1 || results[0].Closed != closedA) {
			err = fmt.Errorf("A unexpected results: %+v", results)
		}
		errs <- err
	}()
	go func() {
		defer wg.Done()
		var entered bool
		results, err := pageB.WithDialogScript(steps, func(*rod.Page) error {
			if !entered {
				entered = true
				close(enteredB)
			}
			return nil
		})
		if err == nil && (len(results) != 1 || results[0].Closed != closedB) {
			err = fmt.Errorf("B unexpected results: %+v", results)
		}
		errs <- err
	}()

	// Don't emit until both sessions have subscribed, otherwise the goob
	// observable would drop the early event.
	select {
	case <-enteredA:
	case <-ctx.Done():
		t.Fatal("A never started")
	}
	select {
	case <-enteredB:
	case <-ctx.Done():
		t.Fatal("B never started")
	}

	// Interleave identical openings but distinct closed events across sessions.
	rec.emitOpening(ctx, "A", opening)
	rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 0)
	rec.emitOpening(ctx, "B", opening)
	rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 1)
	rec.emitClosed(ctx, "B", closedB)
	rec.emitClosed(ctx, "A", closedA)

	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}

	sessions := map[string]int{}
	for _, c := range rec.snapshot() {
		if c.method == "Page.handleJavaScriptDialog" {
			sessions[c.session]++
			assertHandleCall(t, c, c.session, true, "")
		}
	}
	if sessions["A"] != 1 || sessions["B"] != 1 {
		t.Fatalf("per-session handling = %+v, want one each", sessions)
	}
}

// TestDialogScriptContextEnd proves that a missing dialog waits on the page's
// context and returns its cancellation error with the prefix so far.
func TestDialogScriptContextEnd(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	rec := newDialogRecorder()
	b := newScriptBrowser(t, ctx, rec)
	page := b.PageFromSession("form")

	subCtx, subCancel := context.WithCancel(ctx)
	pageWithDeadline := page.Context(subCtx)

	ch := runScript(pageWithDeadline, scriptSteps()[:1], func(*rod.Page) error { return nil })
	subCancel()

	results, err := waitScript(t, ctx, ch)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("want context.Canceled, got %v", err)
	}
	if len(results) != 0 {
		t.Fatalf("prefix len = %d, want 0", len(results))
	}

	// The original page keeps working.
	if err := (proto.PageStopLoading{}).Call(page); err != nil {
		t.Fatalf("original page unusable: %v", err)
	}
}

// TestDialogScriptKeepsEnabledDomain leaves an already-on Page domain on.
func TestDialogScriptKeepsEnabledDomain(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	rec := newDialogRecorder()
	b := newScriptBrowser(t, ctx, rec)
	page := b.PageFromSession("form")

	if err := (proto.PageEnable{}).Call(page); err != nil {
		t.Fatal(err)
	}

	entered := make(chan struct{}, 1)
	ch := runScript(page, scriptSteps()[:1], func(*rod.Page) error {
		entered <- struct{}{}
		return nil
	})
	<-entered
	rec.emitOpening(ctx, "form", scriptOpenings()[0])
	rec.waitForCall(ctx, t, "Page.handleJavaScriptDialog", 0)
	rec.emitClosed(ctx, "form", scriptClosed()[0])
	if _, err := waitScript(t, ctx, ch); err != nil {
		t.Fatal(err)
	}

	if n := rec.countMethod("Page.enable"); n != 1 {
		t.Fatalf("Page.enable count = %d, want only the pre-existing one", n)
	}
	if n := rec.countMethod("Page.disable"); n != 0 {
		t.Fatalf("Page.disable count = %d, domain must stay enabled", n)
	}
}

// TestDialogScriptValidation checks every invalid argument combination fails
// up front without enabling any domain or running the action.
func TestDialogScriptValidation(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	good := rod.DialogStep{Type: proto.PageDialogTypeAlert, URL: "u", Message: "m"}

	cases := []struct {
		name  string
		steps []rod.DialogStep
	}{
		{"empty", nil},
		{"bad-type", []rod.DialogStep{{Type: "popup", URL: "u", Message: "m"}}},
		{"prompt-text-on-confirm", []rod.DialogStep{{
			Type: proto.PageDialogTypeConfirm, URL: "u", Message: "m", Accept: true, PromptText: "x",
		}}},
		{"prompt-text-on-dismissed-prompt", []rod.DialogStep{{
			Type: proto.PageDialogTypePrompt, URL: "u", Message: "m", Accept: false, PromptText: "x",
		}}},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			rec := newDialogRecorder()
			b := newScriptBrowser(t, ctx, rec)
			page := b.PageFromSession("form")
			baseline := len(rec.snapshot()) // Connect already issued its own call

			var ran int32
			_, err := page.WithDialogScript(tc.steps, func(*rod.Page) error {
				atomic.StoreInt32(&ran, 1)
				return nil
			})
			if err == nil {
				t.Fatal("want validation error")
			}
			if atomic.LoadInt32(&ran) != 0 {
				t.Fatal("action must not run")
			}
			if len(rec.snapshot()) != baseline {
				t.Fatalf("no extra protocol call expected, got %+v", rec.snapshot()[baseline:])
			}
		})
	}

	t.Run("nil-action", func(t *testing.T) {
		rec := newDialogRecorder()
		b := newScriptBrowser(t, ctx, rec)
		page := b.PageFromSession("form")
		baseline := len(rec.snapshot())
		if _, err := page.WithDialogScript([]rod.DialogStep{good}, nil); err == nil {
			t.Fatal("want error for nil action")
		}
		if len(rec.snapshot()) != baseline {
			t.Fatalf("no extra protocol call expected, got %+v", rec.snapshot()[baseline:])
		}
	})
}
