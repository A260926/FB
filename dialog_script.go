package rod

import (
	"context"
	"errors"
	"fmt"
	"sync/atomic"

	"github.com/go-rod/rod/lib/proto"
)

// DialogStep describes one JavaScript dialog expected by [Page.WithDialogScript].
// URL and Message are matched exactly against the values carried by
// [proto.PageJavascriptDialogOpening].
type DialogStep struct {
	// Type is the expected dialog type: alert, confirm, prompt or beforeunload.
	Type proto.PageDialogType

	// URL is the exact frame url of the expected dialog.
	URL string

	// Message is the exact message of the expected dialog.
	Message string

	// Accept decides whether the dialog is accepted or dismissed.
	Accept bool

	// PromptText is sent together with Accept. It's only allowed when Type is
	// [proto.PageDialogTypePrompt] and the dialog is accepted.
	PromptText string
}

// DialogResult records the actual protocol values observed while handling one
// [DialogStep]. Opening and Closed come from the Page.javascriptDialogOpening
// and Page.javascriptDialogClosed events; the closed values are never
// reconstructed from the Accept or PromptText we sent.
type DialogResult struct {
	Opening proto.PageJavascriptDialogOpening
	Closed  proto.PageJavascriptDialogClosed
}

// DialogMismatchError is returned when a dialog doesn't match the expected
// [DialogStep], or when an extra dialog pops up after all the steps are done.
// Index is the zero-based position of the step that was waiting for a dialog;
// it equals len(steps) when every step has already been handled, in which
// case Expected is nil.
type DialogMismatchError struct {
	Index    int
	Expected *proto.PageJavascriptDialogOpening
	Actual   proto.PageJavascriptDialogOpening
}

// Error implements error.
func (e *DialogMismatchError) Error() string {
	if e.Expected == nil {
		return fmt.Sprintf(
			"rod: unexpected dialog at step %d after all steps are done: type=%q url=%q message=%q",
			e.Index, e.Actual.Type, e.Actual.URL, e.Actual.Message,
		)
	}
	return fmt.Sprintf(
		"rod: dialog mismatch at step %d: expect type=%q url=%q message=%q, got type=%q url=%q message=%q",
		e.Index, e.Expected.Type, e.Expected.URL, e.Expected.Message,
		e.Actual.Type, e.Actual.URL, e.Actual.Message,
	)
}

// Is makes every [DialogMismatchError] discoverable with errors.Is and
// errors.As, including one wrapped by another error.
func (e *DialogMismatchError) Is(err error) bool {
	_, ok := err.(*DialogMismatchError)
	return ok
}

// dialogScriptRun is the per-session single-flight token of
// [Page.WithDialogScript].
type dialogScriptRun struct{}

func validateDialogSteps(steps []DialogStep) error {
	if len(steps) == 0 {
		return errors.New("rod: WithDialogScript requires at least one DialogStep")
	}

	for i, step := range steps {
		switch step.Type {
		case proto.PageDialogTypeAlert,
			proto.PageDialogTypeConfirm,
			proto.PageDialogTypePrompt,
			proto.PageDialogTypeBeforeunload:
		default:
			return fmt.Errorf(
				"rod: invalid DialogStep.Type %q at %d, only alert, confirm, prompt and beforeunload are allowed",
				step.Type, i,
			)
		}

		if step.PromptText != "" && !(step.Type == proto.PageDialogTypePrompt && step.Accept) {
			return fmt.Errorf(
				"rod: DialogStep.PromptText at %d can only be used when accepting a prompt",
				i,
			)
		}
	}

	return nil
}

func (s DialogStep) expectedOpening() *proto.PageJavascriptDialogOpening {
	return &proto.PageJavascriptDialogOpening{
		URL:     s.URL,
		Message: s.Message,
		Type:    s.Type,
	}
}

func (s DialogStep) match(e *proto.PageJavascriptDialogOpening) bool {
	return e.Type == s.Type && e.URL == s.URL && e.Message == s.Message
}

// WithDialogScript runs action while handling a fixed sequence of JavaScript
// dialogs on the page. The dialogs are handled in the order of steps, each one
// exactly once, and the returned [DialogResult] list holds the actual opening
// and closed protocol events.
//
// All arguments are validated before the Page domain is enabled or action is
// called. action receives a clone of the page sharing the same real session,
// and the original page stays usable after WithDialogScript returns. The
// event subscription is established before action runs, and action is called
// exactly once. For every matched opening, Page.handleJavaScriptDialog is
// sent once; both its success response and the corresponding
// Page.javascriptDialogClosed event, arriving in either order, are required
// before the next step starts. The waiting is bounded by the page's context.
//
// A dialog whose type, url or message doesn't match, or an extra dialog after
// the steps are exhausted, is dismissed once and reported as
// [DialogMismatchError] together with the successfully handled prefix. At most
// one WithDialogScript can run at a time per session, including on
// [Page.Context] clones; different sessions never interfere.
func (p *Page) WithDialogScript(
	steps []DialogStep,
	action func(*Page) error,
) (results []DialogResult, err error) {
	// All argument validation happens before any domain is enabled and before
	// action can run.
	if err := validateDialogSteps(steps); err != nil {
		return nil, err
	}
	if action == nil {
		return nil, errors.New("rod: WithDialogScript requires a non-nil action")
	}

	root := p.root
	if root == nil {
		root = p
	}
	if root.dialogScriptLock == nil {
		root.dialogScriptLock = &atomic.Pointer[dialogScriptRun]{}
	}

	// Single flight per session; Context clones share the same root page.
	if !root.dialogScriptLock.CompareAndSwap(nil, &dialogScriptRun{}) {
		return nil, errors.New("rod: another WithDialogScript is already running on this session")
	}
	defer root.dialogScriptLock.Store(nil)

	ctx, cancel := context.WithCancel(p.ctx)
	defer cancel()

	// Keep the Page domain in its previous state: if it was already on we do
	// nothing, otherwise we enable it here and disable it on exit. The restore
	// call goes through the original page so it doesn't inherit the canceled
	// child context.
	var pageEnable proto.PageEnable
	pageDomainWasEnabled := p.LoadState(&pageEnable)
	if !pageDomainWasEnabled {
		if err := pageEnable.Call(p.Context(ctx)); err != nil {
			return nil, err
		}
		defer func() {
			_ = proto.PageDisable{}.Call(p)
		}()
	}

	// Subscribe through rod's own browser event facilities before action runs,
	// so no opening event can be missed. We filter by session ourselves (the
	// same pattern Page.Close uses), which also covers pages created with
	// Browser.PageFromSession; the raw CDPClient event channel is untouched.
	eventCtx, eventCancel := context.WithCancel(ctx)
	messages := p.browser.Context(eventCtx).Event()
	defer func() {
		eventCancel()
		// Drain until the subscription goroutine closes the channel.
		for range messages {
		}
	}()

	type dialogEvent struct {
		opening *proto.PageJavascriptDialogOpening
		closed  *proto.PageJavascriptDialogClosed
	}
	// The buffer can hold every event of the whole script, so protocol order
	// is preserved even while a handle command is in flight.
	events := make(chan dialogEvent, len(steps)*2+2)
	go func() {
		defer close(events)
		for msg := range messages {
			if msg.SessionID != p.SessionID {
				continue
			}
			var opening proto.PageJavascriptDialogOpening
			var closed proto.PageJavascriptDialogClosed
			switch {
			case msg.Load(&opening):
				select {
				case events <- dialogEvent{opening: &opening}:
				case <-eventCtx.Done():
					return
				}
			case msg.Load(&closed):
				select {
				case events <- dialogEvent{closed: &closed}:
				case <-eventCtx.Done():
					return
				}
			}
		}
	}()

	pageForAction := p.Context(ctx)
	results = make([]DialogResult, 0, len(steps))

	// Per-step state. A step becomes in flight when its opening is matched and
	// reaches the success prefix only after both the handle-command response
	// and its own closed event have arrived, in whichever order the protocol
	// delivers them.
	var (
		inFlight      bool
		cmdDone       bool
		pendingClose  *proto.PageJavascriptDialogClosed
		stepOpening   *proto.PageJavascriptDialogOpening
		parkedOpening *proto.PageJavascriptDialogOpening
	)

	completeStep := func() {
		results = append(results, DialogResult{Opening: *stepOpening, Closed: *pendingClose})
		inFlight, cmdDone, pendingClose, stepOpening = false, false, nil, nil
	}

	// The handle command runs in its own goroutine so the main loop can keep
	// receiving the closed event whether it arrives before or after the
	// command response.
	cmdResult := make(chan error, 1)
	startHandle := func(req proto.PageHandleJavaScriptDialog) {
		go func() {
			cmdResult <- req.Call(pageForAction)
		}()
	}

	dismissMismatch := func(mismatch *DialogMismatchError) ([]DialogResult, error) {
		// No further steps run: dismiss the current dialog exactly once. If
		// the dismiss itself fails the mismatch still wins and stays
		// discoverable through errors.As, with both errors traceable.
		if e := (proto.PageHandleJavaScriptDialog{Accept: false}).Call(pageForAction); e != nil {
			return results, errors.Join(mismatch, fmt.Errorf("dismiss mismatched dialog: %w", e))
		}
		return results, mismatch
	}

	// processOpening matches an opening against the current step, or reports a
	// mismatch. The returned bool tells whether the caller must return.
	processOpening := func(opening *proto.PageJavascriptDialogOpening) ([]DialogResult, error, bool) {
		index := len(results)
		if index == len(steps) || !steps[index].match(opening) {
			mismatch := &DialogMismatchError{Index: index, Actual: *opening}
			if index < len(steps) {
				mismatch.Expected = steps[index].expectedOpening()
			}
			r, e := dismissMismatch(mismatch)
			return r, e, true
		}

		// Matched opening: handle it exactly once with this step's own
		// response settings.
		stepOpening = opening
		inFlight = true
		startHandle(proto.PageHandleJavaScriptDialog{
			Accept:     steps[index].Accept,
			PromptText: steps[index].PromptText,
		})
		return nil, nil, false
	}

	actionDone := make(chan error, 1)
	go func() {
		actionDone <- action(pageForAction)
	}()

	actionFinished := false
	var actionErr error

	for {
		// An opening parked while the previous step's command ack was still
		// pending becomes the current step once that step has completed.
		if !inFlight && parkedOpening != nil {
			opening := parkedOpening
			parkedOpening = nil
			if r, e, done := processOpening(opening); done {
				return r, e
			}
			continue
		}

		if len(results) == len(steps) && actionFinished {
			// An opening may already have traveled through the event pipeline
			// before the last step completed; catch it instead of returning
			// on top of an unhandled extra dialog.
			select {
			case ev, ok := <-events:
				if !ok {
					return results, ctx.Err()
				}
				if ev.closed != nil {
					return results, fmt.Errorf("rod: unexpected javascriptDialogClosed event: %+v", *ev.closed)
				}
				return dismissMismatch(&DialogMismatchError{Index: len(results), Actual: *ev.opening})
			default:
				return results, actionErr
			}
		}

		select {
		case <-ctx.Done():
			return results, ctx.Err()

		case actionErr = <-actionDone:
			actionFinished = true
			if actionErr != nil {
				// Don't replay action: hand back the prefix and the original error.
				return results, actionErr
			}

		case e := <-cmdResult:
			cmdDone = true
			if e != nil {
				// The original command failure is wrapped, not replaced.
				return results, fmt.Errorf("rod: handle dialog at step %d: %w", len(results), e)
			}
			// The closed event may have arrived first; the step only enters
			// the success prefix once both signals are in.
			if pendingClose != nil {
				completeStep()
			}

		case ev, ok := <-events:
			if !ok {
				return results, ctx.Err()
			}

			if ev.closed != nil {
				if !inFlight {
					// A closed event without one of our matched openings can
					// never belong to a scheduled step.
					return results, fmt.Errorf("rod: unexpected javascriptDialogClosed event: %+v", *ev.closed)
				}
				pendingClose = ev.closed
				if cmdDone {
					completeStep()
				}
				continue
			}

			opening := ev.opening

			if inFlight {
				if pendingClose != nil {
					// The current dialog is already closed but its command
					// ack is still pending; this opening belongs to a later
					// step and must wait instead of replacing the current one.
					parkedOpening = opening
				} else {
					// An opening before the current dialog closed can never be
					// one of the scheduled steps, and a sibling session's
					// events never reach this channel.
					return dismissMismatch(&DialogMismatchError{
						Index:    len(results),
						Expected: steps[len(results)].expectedOpening(),
						Actual:   *opening,
					})
				}
				continue
			}

			if r, e, done := processOpening(opening); done {
				return r, e
			}
		}
	}
}
