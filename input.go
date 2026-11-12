package rod

import (
	"fmt"
	"sync"

	"github.com/go-rod/rod/lib/input"
	"github.com/go-rod/rod/lib/proto"
	"github.com/go-rod/rod/lib/utils"
	"github.com/ysmood/gson"
)

// inputState is shared by every [Page] clone of the same real page.
// It only tracks the input state that has been acknowledged by a successful
// CDP dispatch, so a rejected or timed out dispatch never changes what the
// following actions observe. Page clones created with [Page.Context],
// [Page.Timeout] or [Page.WithCancel] keep sharing the same instance, while
// different pages or sessions never do.
type inputState struct {
	keyboardLock sync.Mutex
	// pressed keys must be released before it can be pressed again.
	// The left and right variants of a modifier (such as ShiftLeft and
	// ShiftRight) are tracked separately, so releasing one never clears the
	// modifier bit held down by the other.
	pressed map[input.Key]struct{}

	mouseLock sync.Mutex
	id        string // mouse svg dom element id
	pos       proto.Point
	// the buttons is currently being pressed, reflects the press order
	buttons []proto.InputMouseButton
}

func newInputState() *inputState {
	return &inputState{
		pressed: map[input.Key]struct{}{},
		id:      utils.RandString(8),
	}
}

// Keyboard represents the keyboard on a page, it's always related the main frame.
type Keyboard struct {
	// page is the page clone the keyboard was obtained from. Input dispatched
	// through it uses this page's context, not the context of the page that
	// first created the shared input state.
	page *Page

	state *inputState
}

// bindInputDevices makes the page's Keyboard, Mouse and Touch dispatch through
// the page itself. The shared input state (including its maps and locks) is
// preserved, so clones created by [Page.Context] and friends keep observing
// each other's acknowledged input while each one uses its own context.
func (p *Page) bindInputDevices() {
	p.Keyboard = &Keyboard{page: p, state: p.input}
	p.Mouse = &Mouse{page: p, state: p.input}
	p.Touch = &Touch{page: p}
}

func (k *Keyboard) getModifiers() int {
	k.state.keyboardLock.Lock()
	defer k.state.keyboardLock.Unlock()
	return k.modifiers()
}

// modifiers must be called with k.state.keyboardLock held.
func (k *Keyboard) modifiers() int {
	ms := 0
	for key := range k.state.pressed {
		ms |= key.Modifier()
	}
	return ms
}

// modifiersWithout is like modifiers but pretends the release key is already
// up, without touching the pressed set. Iterating the set instead of clearing
// a bit keeps the other side of a paired modifier (such as ShiftRight while
// releasing ShiftLeft). Must be called with k.state.keyboardLock held.
func (k *Keyboard) modifiersWithout(release input.Key) int {
	ms := 0
	for key := range k.state.pressed {
		if key == release {
			continue
		}
		ms |= key.Modifier()
	}
	return ms
}

// Press the key down.
// To input characters that are not on the keyboard, such as Chinese or Japanese, you should
// use method like [Page.InsertText].
//
// The local pressed set is updated only after the key event has been
// acknowledged by the browser, so the outgoing modifiers already include the
// key being pressed, and a failed dispatch leaves the previously confirmed
// state untouched.
func (k *Keyboard) Press(key input.Key) error {
	defer k.page.tryTrace(TraceTypeInput, "press key: "+key.Info().Code)()
	k.page.browser.trySlowMotion()

	k.state.keyboardLock.Lock()
	defer k.state.keyboardLock.Unlock()

	// the modifiers of the keyDown event include the key that is going to be
	// pressed once the dispatch is acknowledged
	modifiers := k.modifiers() | key.Modifier()

	err := key.Encode(proto.InputDispatchKeyEventTypeKeyDown, modifiers).Call(k.page)
	if err != nil {
		return err
	}

	k.state.pressed[key] = struct{}{}
	return nil
}

// Release the key.
//
// Releasing a key that is not held down sends nothing. The local pressed set
// is updated only after the key event has been acknowledged by the browser:
// the outgoing keyUp modifiers already exclude the released key (while keeping
// any other held modifier), and a failed dispatch leaves the previously
// confirmed set untouched.
func (k *Keyboard) Release(key input.Key) error {
	defer k.page.tryTrace(TraceTypeInput, "release key: "+key.Info().Code)()

	k.state.keyboardLock.Lock()
	defer k.state.keyboardLock.Unlock()

	if _, has := k.state.pressed[key]; !has {
		return nil
	}

	// the modifiers of the keyUp event exclude the key that is going to be
	// released; it is only removed from the set once the dispatch is
	// acknowledged
	err := key.Encode(proto.InputDispatchKeyEventTypeKeyUp, k.modifiersWithout(key)).Call(k.page)
	if err != nil {
		return err
	}

	delete(k.state.pressed, key)
	return nil
}

// Type releases the key after the press.
func (k *Keyboard) Type(keys ...input.Key) (err error) {
	for _, key := range keys {
		err = k.Press(key)
		if err != nil {
			return
		}
		err = k.Release(key)
		if err != nil {
			return
		}
	}
	return
}

// KeyActionType enum.
type KeyActionType int

// KeyActionTypes.
const (
	KeyActionPress KeyActionType = iota
	KeyActionRelease
	KeyActionTypeKey
)

// KeyAction to perform.
type KeyAction struct {
	Type KeyActionType
	Key  input.Key
}

// KeyActions to simulate.
type KeyActions struct {
	keyboard *Keyboard

	Actions []KeyAction
}

// KeyActions simulates the type actions on a physical keyboard.
// Useful when input shortcuts like ctrl+enter .
func (p *Page) KeyActions() *KeyActions {
	return &KeyActions{keyboard: p.Keyboard}
}

// Press keys is guaranteed to have a release at the end of actions.
func (ka *KeyActions) Press(keys ...input.Key) *KeyActions {
	for _, key := range keys {
		ka.Actions = append(ka.Actions, KeyAction{KeyActionPress, key})
	}
	return ka
}

// Release keys.
func (ka *KeyActions) Release(keys ...input.Key) *KeyActions {
	for _, key := range keys {
		ka.Actions = append(ka.Actions, KeyAction{KeyActionRelease, key})
	}
	return ka
}

// Type will release the key immediately after the pressing.
func (ka *KeyActions) Type(keys ...input.Key) *KeyActions {
	for _, key := range keys {
		ka.Actions = append(ka.Actions, KeyAction{KeyActionTypeKey, key})
	}
	return ka
}

// Do the actions. It stops at the first dispatch error and keeps the
// successfully dispatched prefix, there is no rollback or replay.
func (ka *KeyActions) Do() (err error) {
	for _, a := range ka.balance() {
		switch a.Type {
		case KeyActionPress:
			err = ka.keyboard.Press(a.Key)
		case KeyActionRelease:
			err = ka.keyboard.Release(a.Key)
		case KeyActionTypeKey:
			err = ka.keyboard.Type(a.Key)
		}
		if err != nil {
			return
		}
	}
	return
}

// Make sure there's at least one release after the presses, such as:
//
//	p1,p2,p1,r1 => p1,p2,p1,r1,r2
func (ka *KeyActions) balance() []KeyAction {
	actions := ka.Actions

	h := map[input.Key]bool{}
	for _, a := range actions {
		switch a.Type {
		case KeyActionPress:
			h[a.Key] = true
		case KeyActionRelease, KeyActionTypeKey:
			h[a.Key] = false
		}
	}

	for key, needRelease := range h {
		if needRelease {
			actions = append(actions, KeyAction{KeyActionRelease, key})
		}
	}

	return actions
}

// InsertText is like pasting text into the page.
func (p *Page) InsertText(text string) error {
	defer p.tryTrace(TraceTypeInput, "insert text "+text)()
	p.browser.trySlowMotion()

	err := proto.InputInsertText{Text: text}.Call(p)
	return err
}

// Mouse represents the mouse on a page, it's always related the main frame.
type Mouse struct {
	// page is the page clone the mouse was obtained from. Input dispatched
	// through it uses this page's context, not the context of the page that
	// first created the shared input state.
	page *Page

	state *inputState
}

// Position of current cursor.
func (m *Mouse) Position() proto.Point {
	m.state.mouseLock.Lock()
	defer m.state.mouseLock.Unlock()
	return m.state.pos
}

// MoveTo the absolute position.
//
// The cursor position is updated only after the event has been acknowledged by
// the browser, so a failed dispatch does not move the local state.
func (m *Mouse) MoveTo(p proto.Point) error {
	m.state.mouseLock.Lock()
	defer m.state.mouseLock.Unlock()

	button, buttons := input.EncodeMouseButton(m.state.buttons)

	m.page.browser.trySlowMotion()

	err := proto.InputDispatchMouseEvent{
		Type:      proto.InputDispatchMouseEventTypeMouseMoved,
		X:         p.X,
		Y:         p.Y,
		Button:    button,
		Buttons:   gson.Int(buttons),
		Modifiers: m.page.Keyboard.getModifiers(),
	}.Call(m.page)
	if err != nil {
		return err
	}

	// to make sure set only when call is successful
	m.state.pos = p

	if m.page.browser.trace {
		if !m.updateMouseTracer() {
			m.initMouseTracer()
			m.updateMouseTracer()
		}
	}

	return nil
}

// MoveAlong the guide function.
// Every time the guide function is called it should return the next position, return true to stop.
// Read the source code of [Mouse.MoveLinear] as an example to use this method.
func (m *Mouse) MoveAlong(guide func() (proto.Point, bool)) error {
	for {
		p, stop := guide()
		if stop {
			return m.MoveTo(p)
		}

		err := m.MoveTo(p)
		if err != nil {
			return err
		}
	}
}

// MoveLinear to the absolute position with the given steps.
// Such as move from (0,0) to (6,6) with 3 steps, the mouse will first move to (2,2) then (4,4) then (6,6).
func (m *Mouse) MoveLinear(to proto.Point, steps int) error {
	p := m.Position()
	step := to.Minus(p).Scale(1 / float64(steps))
	count := 0

	return m.MoveAlong(func() (proto.Point, bool) {
		count++
		if count == steps {
			return to, true
		}

		p = p.Add(step)
		return p, false
	})
}

// Scroll the relative offset with specified steps.
func (m *Mouse) Scroll(offsetX, offsetY float64, steps int) error {
	m.state.mouseLock.Lock()
	defer m.state.mouseLock.Unlock()

	defer m.page.tryTrace(TraceTypeInput, fmt.Sprintf("scroll (%.2f, %.2f)", offsetX, offsetY))()
	m.page.browser.trySlowMotion()

	if steps < 1 {
		steps = 1
	}

	button, buttons := input.EncodeMouseButton(m.state.buttons)

	stepX := offsetX / float64(steps)
	stepY := offsetY / float64(steps)

	for i := 0; i < steps; i++ {
		err := proto.InputDispatchMouseEvent{
			Type:      proto.InputDispatchMouseEventTypeMouseWheel,
			Button:    button,
			Buttons:   gson.Int(buttons),
			Modifiers: m.page.Keyboard.getModifiers(),
			DeltaX:    stepX,
			DeltaY:    stepY,
			X:         m.state.pos.X,
			Y:         m.state.pos.Y,
		}.Call(m.page)
		if err != nil {
			return err
		}
	}

	return nil
}

// Down holds the button down. The local button set is extended only after the
// press has been acknowledged, so a failed dispatch leaves it unchanged.
func (m *Mouse) Down(button proto.InputMouseButton, clickCount int) error {
	m.state.mouseLock.Lock()
	defer m.state.mouseLock.Unlock()

	toButtons := append(append([]proto.InputMouseButton{}, m.state.buttons...), button)

	_, buttons := input.EncodeMouseButton(toButtons)

	err := proto.InputDispatchMouseEvent{
		Type:       proto.InputDispatchMouseEventTypeMousePressed,
		Button:     button,
		Buttons:    gson.Int(buttons),
		ClickCount: clickCount,
		Modifiers:  m.page.Keyboard.getModifiers(),
		X:          m.state.pos.X,
		Y:          m.state.pos.Y,
	}.Call(m.page)
	if err != nil {
		return err
	}
	m.state.buttons = toButtons
	return nil
}

// Up releases the button. The local button set is trimmed only after the
// release has been acknowledged, so a failed dispatch leaves it unchanged.
func (m *Mouse) Up(button proto.InputMouseButton, clickCount int) error {
	m.state.mouseLock.Lock()
	defer m.state.mouseLock.Unlock()

	toButtons := []proto.InputMouseButton{}
	for _, btn := range m.state.buttons {
		if btn == button {
			continue
		}
		toButtons = append(toButtons, btn)
	}

	_, buttons := input.EncodeMouseButton(toButtons)

	err := proto.InputDispatchMouseEvent{
		Type:       proto.InputDispatchMouseEventTypeMouseReleased,
		Button:     button,
		Buttons:    gson.Int(buttons),
		ClickCount: clickCount,
		Modifiers:  m.page.Keyboard.getModifiers(),
		X:          m.state.pos.X,
		Y:          m.state.pos.Y,
	}.Call(m.page)
	if err != nil {
		return err
	}
	m.state.buttons = toButtons
	return nil
}

// Click the button. It's the combination of [Mouse.Down] and [Mouse.Up].
func (m *Mouse) Click(button proto.InputMouseButton, clickCount int) error {
	m.page.browser.trySlowMotion()

	err := m.Down(button, clickCount)
	if err != nil {
		return err
	}

	return m.Up(button, clickCount)
}

// Touch presents a touch device, such as a hand with fingers, each finger is a [proto.InputTouchPoint].
// Touch events is stateless, we use the struct here only as a namespace to make the API style unified.
type Touch struct {
	page *Page
}

// Start a touch action.
func (t *Touch) Start(points ...*proto.InputTouchPoint) error {
	// TODO: https://crbug.com/613219
	_ = t.page.WaitRepaint()
	_ = t.page.WaitRepaint()

	return proto.InputDispatchTouchEvent{
		Type:        proto.InputDispatchTouchEventTypeTouchStart,
		TouchPoints: points,
		Modifiers:   t.page.Keyboard.getModifiers(),
	}.Call(t.page)
}

// Move touch points. Use the [proto.InputTouchPoint.ID] (Touch.identifier) to track points.
// Doc: https://developer.mozilla.org/en-US/docs/Web/API/Touch_events
func (t *Touch) Move(points ...*proto.InputTouchPoint) error {
	return proto.InputDispatchTouchEvent{
		Type:        proto.InputDispatchTouchEventTypeTouchMove,
		TouchPoints: points,
		Modifiers:   t.page.Keyboard.getModifiers(),
	}.Call(t.page)
}

// End touch action.
func (t *Touch) End() error {
	return proto.InputDispatchTouchEvent{
		Type:        proto.InputDispatchTouchEventTypeTouchEnd,
		TouchPoints: []*proto.InputTouchPoint{},
		Modifiers:   t.page.Keyboard.getModifiers(),
	}.Call(t.page)
}

// Cancel touch action.
func (t *Touch) Cancel() error {
	return proto.InputDispatchTouchEvent{
		Type:        proto.InputDispatchTouchEventTypeTouchCancel,
		TouchPoints: []*proto.InputTouchPoint{},
		Modifiers:   t.page.Keyboard.getModifiers(),
	}.Call(t.page)
}

// Tap dispatches a touchstart and touchend event.
func (t *Touch) Tap(x, y float64) error {
	defer t.page.tryTrace(TraceTypeInput, "touch")()
	t.page.browser.trySlowMotion()

	p := &proto.InputTouchPoint{X: x, Y: y}

	err := t.Start(p)
	if err != nil {
		return err
	}

	return t.End()
}
