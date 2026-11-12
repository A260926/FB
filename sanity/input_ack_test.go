package main

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"sync"
	"testing"

	"github.com/go-rod/rod"
	"github.com/go-rod/rod/lib/cdp"
	"github.com/go-rod/rod/lib/input"
	"github.com/go-rod/rod/lib/proto"
)

// errFakeDispatch is the untouched error the fake transport returns when it
// rejects a dispatch. Tests match it with errors.Is to make sure the original
// error is preserved instead of being swallowed or wrapped away.
var errFakeDispatch = errors.New("fake cdp dispatch rejected")

type fakeCall struct {
	ctx       context.Context
	sessionID string
	method    string
	params    json.RawMessage
}

func (c fakeCall) keyEvent(t *testing.T) proto.InputDispatchKeyEvent {
	t.Helper()
	var e proto.InputDispatchKeyEvent
	if err := json.Unmarshal(c.params, &e); err != nil {
		t.Fatalf("unmarshal key event: %v", err)
	}
	return e
}

func (c fakeCall) mouseEvent(t *testing.T) proto.InputDispatchMouseEvent {
	t.Helper()
	var e proto.InputDispatchMouseEvent
	if err := json.Unmarshal(c.params, &e); err != nil {
		t.Fatalf("unmarshal mouse event: %v", err)
	}
	return e
}

func (c fakeCall) touchEvent(t *testing.T) proto.InputDispatchTouchEvent {
	t.Helper()
	var e proto.InputDispatchTouchEvent
	if err := json.Unmarshal(c.params, &e); err != nil {
		t.Fatalf("unmarshal touch event: %v", err)
	}
	return e
}

// fakeCDP is a deterministic in-memory rod.CDPClient. It records every call
// (including the context and session), can reject a single dispatch of a given
// method, and answers the few protocol methods the rod entry points need.
type fakeCDP struct {
	mu sync.Mutex

	events chan *cdp.Event

	calls []fakeCall

	// rejectOnce marks methods whose next call must fail with errFakeDispatch.
	rejectOnce map[string]bool

	sessionSeq int
}

func newFakeCDP() *fakeCDP {
	return &fakeCDP{
		events:     make(chan *cdp.Event),
		rejectOnce: map[string]bool{},
	}
}

var _ rod.CDPClient = (*fakeCDP)(nil)

func (f *fakeCDP) Event() <-chan *cdp.Event { return f.events }

func (f *fakeCDP) Call(ctx context.Context, sessionID, method string, params interface{}) ([]byte, error) {
	var raw json.RawMessage
	if params != nil {
		b, err := json.Marshal(params)
		if err != nil {
			return nil, err
		}
		raw = b
	}

	f.mu.Lock()
	f.calls = append(f.calls, fakeCall{ctx: ctx, sessionID: sessionID, method: method, params: raw})
	reject := f.rejectOnce[method]
	if reject {
		f.rejectOnce[method] = false
	}
	f.mu.Unlock()

	// A transport that sees a finished context refuses the command.
	if err := ctx.Err(); err != nil {
		return nil, err
	}

	if reject {
		return nil, errFakeDispatch
	}

	switch method {
	case "Target.attachToTarget":
		f.mu.Lock()
		f.sessionSeq++
		id := f.sessionSeq
		f.mu.Unlock()
		return json.Marshal(map[string]string{"sessionId": "s" + strconv.Itoa(id)})
	case "Runtime.evaluate":
		// getJSCtxID asks for a reference to the window object.
		return []byte(`{"result":{"objectId":"remote-object","type":"object"}}`), nil
	case "Runtime.callFunctionOn":
		// By-value evals drive the element readiness waits: answer true so
		// they resolve instead of retrying forever. Reference evals get a
		// stand-in remote object id.
		var req struct {
			ReturnByValue bool `json:"returnByValue"`
		}
		_ = json.Unmarshal(raw, &req)
		if req.ReturnByValue {
			return []byte(`{"result":{"value":true,"type":"boolean"}}`), nil
		}
		return []byte(`{"result":{"objectId":"remote-object","type":"object"}}`), nil
	default:
		return []byte(`{}`), nil
	}
}

func (f *fakeCDP) rejectNext(method string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.rejectOnce[method] = true
}

func (f *fakeCDP) snapshot() []fakeCall {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make([]fakeCall, len(f.calls))
	copy(out, f.calls)
	return out
}

func (f *fakeCDP) callsOf(method string) []fakeCall {
	var out []fakeCall
	for _, c := range f.snapshot() {
		if c.method == method {
			out = append(out, c)
		}
	}
	return out
}

func (f *fakeCDP) lastOf(t *testing.T, method string) fakeCall {
	t.Helper()
	calls := f.callsOf(method)
	if len(calls) == 0 {
		t.Fatalf("no recorded call for %q", method)
	}
	return calls[len(calls)-1]
}

func (f *fakeCDP) last() fakeCall {
	calls := f.snapshot()
	return calls[len(calls)-1]
}

func mustOK(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
}

func newFakePage(t *testing.T, f *fakeCDP, targetID string) *rod.Page {
	t.Helper()
	b := rod.New().Context(context.Background()).Client(f).NoDefaultDevice()
	mustOK(t, b.Connect())
	p, err := b.PageFromTarget(proto.TargetTargetID(targetID))
	mustOK(t, err)
	return p
}

// TestInputAcknowledgement pins the input state chain: only acknowledged
// dispatches change the shared state, page clones own their context while
// sharing the confirmed state, and rejected or cancelled commands never
// pollute what the next action observes.
func TestInputAcknowledgement(t *testing.T) {
	t.Run("rejected_press", func(t *testing.T) {
		f := newFakeCDP()
		p := newFakePage(t, f, "t1")

		// The rejected keyDown is the only dispatch for this press and it
		// already carries the modifier that would be held once acknowledged.
		f.rejectNext("Input.dispatchKeyEvent")
		err := p.Keyboard.Press(input.ControlLeft)
		if !errors.Is(err, errFakeDispatch) {
			t.Fatalf("expected errFakeDispatch, got %v", err)
		}

		keyDowns := f.callsOf("Input.dispatchKeyEvent")
		if len(keyDowns) != 1 {
			t.Fatalf("rejected press must send exactly one dispatch, got %d", len(keyDowns))
		}
		rejected := keyDowns[0].keyEvent(t)
		if rejected.Type != proto.InputDispatchKeyEventTypeRawKeyDown ||
			rejected.Code != "ControlLeft" || rejected.WindowsVirtualKeyCode != 17 ||
			rejected.Modifiers != input.ModifierControl {
			t.Fatalf("unexpected rejected key event: %+v", rejected)
		}

		// The failed press is not acknowledged: the next mouse move must not
		// carry the phantom Control modifier.
		mustOK(t, p.Mouse.MoveTo(proto.Point{X: 1, Y: 2}))
		move := f.lastOf(t, "Input.dispatchMouseEvent")
		if me := move.mouseEvent(t); me.Modifiers != 0 || me.X != 1 || me.Y != 2 {
			t.Fatalf("phantom modifier after rejected press: %+v", me)
		}

		// A later successful press is acknowledged and becomes visible.
		mustOK(t, p.Keyboard.Press(input.ControlLeft))
		if ke := f.callsOf("Input.dispatchKeyEvent")[1].keyEvent(t); ke.Modifiers != input.ModifierControl {
			t.Fatalf("acknowledged keyDown modifier = %d, want %d", ke.Modifiers, input.ModifierControl)
		}
		mustOK(t, p.Mouse.MoveTo(proto.Point{X: 3, Y: 4}))
		if me := f.lastOf(t, "Input.dispatchMouseEvent").mouseEvent(t); me.Modifiers != input.ModifierControl {
			t.Fatalf("mouse modifier after acknowledged press = %d", me.Modifiers)
		}

		// The keyUp excludes the key being released.
		mustOK(t, p.Keyboard.Release(input.ControlLeft))
		keyUps := f.callsOf("Input.dispatchKeyEvent")
		if ke := keyUps[len(keyUps)-1].keyEvent(t); ke.Type != proto.InputDispatchKeyEventTypeKeyUp || ke.Modifiers != 0 {
			t.Fatalf("unexpected keyUp event: %+v", ke)
		}
		mustOK(t, p.Mouse.MoveTo(proto.Point{X: 5, Y: 6}))
		if me := f.lastOf(t, "Input.dispatchMouseEvent").mouseEvent(t); me.Modifiers != 0 {
			t.Fatalf("modifier leaked after release: %d", me.Modifiers)
		}
	})

	t.Run("rejected_release", func(t *testing.T) {
		f := newFakeCDP()
		p := newFakePage(t, f, "t1")

		mustOK(t, p.Keyboard.Press(input.ShiftLeft))

		f.rejectNext("Input.dispatchKeyEvent")
		err := p.Keyboard.Release(input.ShiftLeft)
		if !errors.Is(err, errFakeDispatch) {
			t.Fatalf("expected errFakeDispatch, got %v", err)
		}
		keyCalls := f.callsOf("Input.dispatchKeyEvent")
		if ke := keyCalls[len(keyCalls)-1].keyEvent(t); ke.Modifiers != 0 {
			t.Fatalf("rejected keyUp must exclude the released key, got modifiers %d", ke.Modifiers)
		}

		// Shift is still confirmed down, so the next typed letter keeps it.
		mustOK(t, p.Keyboard.Type('a'))
		keyCalls = f.callsOf("Input.dispatchKeyEvent")
		for _, c := range keyCalls[len(keyCalls)-2:] {
			ke := c.keyEvent(t)
			if ke.Key != "a" || ke.Text != "a" || ke.Modifiers != input.ModifierShift {
				t.Fatalf("shift lost after rejected release, event: %+v", ke)
			}
		}

		// The acknowledged release finally clears it.
		mustOK(t, p.Keyboard.Release(input.ShiftLeft))
		mustOK(t, p.Keyboard.Type('b'))
		keyCalls = f.callsOf("Input.dispatchKeyEvent")
		for _, c := range keyCalls[len(keyCalls)-2:] {
			if ke := c.keyEvent(t); ke.Modifiers != 0 {
				t.Fatalf("shift survived acknowledged release: %+v", ke)
			}
		}

		// Releasing a key that is not held sends nothing.
		before := len(f.snapshot())
		mustOK(t, p.Keyboard.Release(input.ShiftLeft))
		if len(f.snapshot()) != before {
			t.Fatal("release of an unpressed key must not dispatch anything")
		}
	})

	t.Run("clone_context", func(t *testing.T) {
		f := newFakeCDP()
		p := newFakePage(t, f, "t1")
		rootCtx := p.GetContext()

		dead, stop := context.WithCancel(context.Background())
		stop()
		clone := p.Context(dead)

		// Input from the cancelled clone is rejected with the clone context.
		err := clone.Keyboard.Type('b')
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("expected context.Canceled, got %v", err)
		}
		cancelledCall := f.last()
		if cancelledCall.ctx.Err() == nil {
			t.Fatal("clone input did not use the clone's cancelled context")
		}
		if cancelledCall.sessionID != string(p.SessionID) {
			t.Fatalf("session = %q, want %q", cancelledCall.sessionID, p.SessionID)
		}

		// Cancelling the clone never cancels the original page.
		if p.GetContext().Err() != nil || p.GetContext() != rootCtx {
			t.Fatal("original page context was affected by the clone")
		}
		mustOK(t, p.Keyboard.Type('c'))
		if live := f.last(); live.ctx.Err() != nil || live.ctx != rootCtx {
			t.Fatal("original page input no longer uses its live context")
		}

		// The clone and the original still share the acknowledged state, and a
		// rejected command on the clone does not pollute it.
		mustOK(t, p.Keyboard.Press(input.ShiftLeft))
		err = clone.Mouse.MoveTo(proto.Point{X: 9, Y: 9})
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("expected context.Canceled from clone, got %v", err)
		}
		cloneMove := f.lastOf(t, "Input.dispatchMouseEvent").mouseEvent(t)
		if cloneMove.Modifiers != input.ModifierShift || cloneMove.X != 9 || cloneMove.Y != 9 {
			t.Fatalf("clone mouse did not observe shared shift state: %+v", cloneMove)
		}
		mustOK(t, p.Mouse.MoveTo(proto.Point{X: 10, Y: 10}))
		liveMove := f.lastOf(t, "Input.dispatchMouseEvent").mouseEvent(t)
		if liveMove.Modifiers != input.ModifierShift {
			t.Fatalf("cancelled clone polluted the shared state: %+v", liveMove)
		}

		// WithCancel produces the same ownership rules.
		pc, cancel := p.WithCancel()
		cancel()
		if err = pc.Keyboard.Press(input.AltLeft); !errors.Is(err, context.Canceled) {
			t.Fatalf("expected context.Canceled, got %v", err)
		}
		mustOK(t, p.Keyboard.Press(input.AltLeft))
		mustOK(t, p.Keyboard.Release(input.AltLeft))
		mustOK(t, p.Keyboard.Release(input.ShiftLeft))
	})

	t.Run("shared_state", func(t *testing.T) {
		f := newFakeCDP()
		p := newFakePage(t, f, "t1")
		other := p.Context(context.Background())

		// Modifier confirmed through one clone is visible through another.
		mustOK(t, p.Keyboard.Press(input.ShiftLeft))
		mustOK(t, other.Mouse.MoveTo(proto.Point{X: 1, Y: 1}))
		move := f.lastOf(t, "Input.dispatchMouseEvent").mouseEvent(t)
		if move.Modifiers != input.ModifierShift {
			t.Fatalf("clone mouse did not share keyboard state: %+v", move)
		}
		if got := other.Mouse.Position(); got.X != 1 || got.Y != 1 {
			t.Fatalf("clone did not share cursor position: %+v", got)
		}

		// Mouse buttons are shared and encoded with the press order.
		mustOK(t, other.Mouse.Down(proto.InputMouseButtonLeft, 1))
		mustOK(t, p.Mouse.MoveTo(proto.Point{X: 2, Y: 2}))
		move = f.lastOf(t, "Input.dispatchMouseEvent").mouseEvent(t)
		if move.Buttons == nil || *move.Buttons != 1 {
			t.Fatalf("clone button press not shared: %+v", move)
		}
		mustOK(t, p.Mouse.Up(proto.InputMouseButtonLeft, 1))

		// Left and right variants of a modifier are tracked independently:
		// releasing one side keeps the other side's bit.
		mustOK(t, p.Keyboard.Press(input.ShiftRight))
		mustOK(t, p.Keyboard.Release(input.ShiftLeft))
		keyCalls := f.callsOf("Input.dispatchKeyEvent")
		if ke := keyCalls[len(keyCalls)-1].keyEvent(t); ke.Modifiers != input.ModifierShift {
			t.Fatalf("releasing ShiftLeft cleared ShiftRight: %+v", ke)
		}
		mustOK(t, p.Keyboard.Release(input.ShiftRight))
		if ke := f.callsOf("Input.dispatchKeyEvent")[len(f.callsOf("Input.dispatchKeyEvent"))-1].keyEvent(t); ke.Modifiers != 0 {
			t.Fatalf("final ShiftRight release modifier = %d, want 0", ke.Modifiers)
		}

		// Touch reads the same shared keyboard state and session.
		mustOK(t, p.Keyboard.Press(input.ControlLeft))
		mustOK(t, p.Touch.Tap(3, 4))
		touchCalls := f.callsOf("Input.dispatchTouchEvent")
		if len(touchCalls) == 0 {
			t.Fatal("no touch event recorded")
		}
		if te := touchCalls[0].touchEvent(t); te.Modifiers != input.ModifierControl {
			t.Fatalf("touch did not share keyboard modifiers: %+v", te)
		}
		if touchCalls[0].sessionID != string(p.SessionID) {
			t.Fatalf("touch session = %q, want %q", touchCalls[0].sessionID, p.SessionID)
		}
		mustOK(t, p.Keyboard.Release(input.ControlLeft))

		// A different page/session gets fully isolated input state.
		p2 := newFakePage(t, f, "t2")
		if p2.SessionID == p.SessionID {
			t.Fatal("fake sessions should differ")
		}
		mustOK(t, p2.Keyboard.Press(input.ShiftLeft))
		if c := f.callsOf("Input.dispatchKeyEvent"); c[len(c)-1].sessionID != string(p2.SessionID) {
			t.Fatal("second page dispatch used the wrong session")
		}
		mustOK(t, p.Mouse.MoveTo(proto.Point{X: 7, Y: 7}))
		if me := f.lastOf(t, "Input.dispatchMouseEvent").mouseEvent(t); me.Modifiers != 0 {
			t.Fatalf("input state leaked across sessions: %+v", me)
		}
		mustOK(t, p2.Mouse.MoveTo(proto.Point{X: 8, Y: 8}))
		if me := f.lastOf(t, "Input.dispatchMouseEvent").mouseEvent(t); me.Modifiers != input.ModifierShift {
			t.Fatalf("second page lost its own modifier: %+v", me)
		}
	})

	t.Run("element_context", func(t *testing.T) {
		f := newFakeCDP()
		p := newFakePage(t, f, "t1")

		el, err := p.ElementFromObject(&proto.RuntimeRemoteObject{ObjectID: "element-1"})
		mustOK(t, err)

		// Successful element input flows through the element context handoff.
		elementCtx := el.GetContext()
		mustOK(t, el.Type('a'))
		var sawA bool
		for _, c := range f.callsOf("Input.dispatchKeyEvent") {
			ke := c.keyEvent(t)
			if ke.Key != "a" {
				continue
			}
			sawA = true
			if c.ctx != elementCtx || c.sessionID != string(p.SessionID) {
				t.Fatalf("element key call wrong ctx/session: %+v", c)
			}
		}
		if !sawA {
			t.Fatal("element Type produced no key events")
		}

		// KeyActions honours the same handoff, including the balancing release.
		ka, err := el.KeyActions()
		mustOK(t, err)
		mustOK(t, ka.Press(input.ControlLeft).Type(input.Enter).Do())
		keyCalls := f.callsOf("Input.dispatchKeyEvent")
		lastFour := keyCalls[len(keyCalls)-4:]
		want := []struct {
			code      string
			modifiers int
		}{
			{"ControlLeft", input.ModifierControl},
			{"Enter", input.ModifierControl},
			{"Enter", input.ModifierControl},
			{"ControlLeft", 0},
		}
		for i, w := range want {
			ke := lastFour[i].keyEvent(t)
			if ke.Code != w.code || ke.Modifiers != w.modifiers || lastFour[i].ctx != elementCtx {
				t.Fatalf("element KeyActions event %d = %+v, want code %q modifiers %d", i, ke, w.code, w.modifiers)
			}
		}

		// A rejected element dispatch keeps the original error and leaves the
		// shared state untouched.
		f.rejectNext("Input.dispatchKeyEvent")
		err = el.Type('q')
		if !errors.Is(err, errFakeDispatch) {
			t.Fatalf("expected errFakeDispatch, got %v", err)
		}
		if rejected := f.callsOf("Input.dispatchKeyEvent"); rejected[len(rejected)-1].ctx != elementCtx {
			t.Fatal("rejected element dispatch did not use the element context")
		}
		mustOK(t, p.Mouse.MoveTo(proto.Point{X: 11, Y: 11}))
		if me := f.lastOf(t, "Input.dispatchMouseEvent").mouseEvent(t); me.Modifiers != 0 {
			t.Fatalf("rejected element dispatch polluted shared state: %+v", me)
		}

		// A cancelled element context rejects before any key dispatch, without
		// touching the underlying page.
		dead, stop := context.WithCancel(context.Background())
		stop()
		deadEl := el.Context(dead)
		before := len(f.callsOf("Input.dispatchKeyEvent"))
		if err = deadEl.Type('z'); !errors.Is(err, context.Canceled) {
			t.Fatalf("expected context.Canceled, got %v", err)
		}
		if len(f.callsOf("Input.dispatchKeyEvent")) != before {
			t.Fatal("cancelled element context must not dispatch key events")
		}
		if p.GetContext().Err() != nil {
			t.Fatal("cancelling an element cancelled its page")
		}
		mustOK(t, p.Keyboard.Type('d'))
	})
}
