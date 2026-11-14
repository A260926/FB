package godet_test

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/raff/godet"
	"github.com/raff/godet/task_support/taskcheck"
)

// check records an assertion through c.Check and returns the outcome, so the
// test body can bail out early on broken preconditions.
func check(c *taskcheck.C, ok bool, message string, args ...interface{}) bool {
	c.Check(ok, message, args...)
	return ok
}

// fakeBrowser is a local stand-in for a Chrome instance: it serves the HTTP
// endpoints godet needs and records every command received on each page
// socket. All synchronization is explicit (channels), never sleeps.
type fakeBrowser struct {
	server *httptest.Server

	mu        sync.Mutex
	tabs      []map[string]interface{}
	commands  []map[string]interface{}
	onCommand func(*fakeConn, map[string]interface{})

	connCh    chan *fakeConn
	activated chan string
}

// fakeConn is one websocket connection to a page.
type fakeConn struct {
	page     string
	ws       *websocket.Conn
	wmu      sync.Mutex // serializes server-side writes
	received chan map[string]interface{}
	closed   chan struct{}
	once     sync.Once
}

func newFakeBrowser(t *testing.T, pages ...string) *fakeBrowser {
	b := &fakeBrowser{
		connCh:    make(chan *fakeConn, 16),
		activated: make(chan string, 16),
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/json/list", func(w http.ResponseWriter, r *http.Request) {
		b.mu.Lock()
		tabs := b.tabs
		b.mu.Unlock()
		json.NewEncoder(w).Encode(tabs)
	})
	mux.HandleFunc("/json/activate/", func(w http.ResponseWriter, r *http.Request) {
		b.activated <- strings.TrimPrefix(r.URL.Path, "/json/activate/")
		w.Write([]byte("OK"))
	})
	mux.HandleFunc("/page/", func(w http.ResponseWriter, r *http.Request) {
		b.servePage(strings.TrimPrefix(r.URL.Path, "/page/"), w, r)
	})

	b.server = httptest.NewServer(mux)
	t.Cleanup(b.server.Close)

	b.mu.Lock()
	for _, id := range pages {
		b.tabs = append(b.tabs, map[string]interface{}{
			"id":                   id,
			"type":                 "page",
			"webSocketDebuggerUrl": "ws" + strings.TrimPrefix(b.server.URL, "http") + "/page/" + id,
		})
	}
	b.mu.Unlock()

	return b
}

// setOnCommand installs the hook called (in the server read loop) for every
// command received from the client.
func (b *fakeBrowser) setOnCommand(h func(*fakeConn, map[string]interface{})) {
	b.mu.Lock()
	b.onCommand = h
	b.mu.Unlock()
}

func (b *fakeBrowser) servePage(page string, w http.ResponseWriter, r *http.Request) {
	ws, err := (&websocket.Upgrader{}).Upgrade(w, r, nil)
	if err != nil {
		return
	}

	fc := &fakeConn{
		page:     page,
		ws:       ws,
		received: make(chan map[string]interface{}, 128),
		closed:   make(chan struct{}),
	}

	b.connCh <- fc

	defer func() {
		ws.Close()
		fc.once.Do(func() { close(fc.closed) })
	}()

	for {
		var q map[string]interface{}
		if err := ws.ReadJSON(&q); err != nil {
			return
		}

		b.mu.Lock()
		b.commands = append(b.commands, q)
		handler := b.onCommand
		b.mu.Unlock()

		fc.received <- q

		if handler != nil {
			handler(fc, q)
		}
	}
}

// addr returns the debugger address suitable for godet.Connect.
func (b *fakeBrowser) addr() string {
	return strings.TrimPrefix(b.server.URL, "http://")
}

// waitConn returns the next page connection accepted by the server.
func (b *fakeBrowser) waitConn(c *taskcheck.C) *fakeConn {
	select {
	case fc := <-b.connCh:
		return fc
	case <-time.After(5 * time.Second):
		c.Check(false, "timeout waiting for page connection")
		return nil
	}
}

// count returns how many commands with the given method were received.
func (b *fakeBrowser) count(method string) int {
	b.mu.Lock()
	defer b.mu.Unlock()
	n := 0
	for _, q := range b.commands {
		if q["method"] == method {
			n++
		}
	}
	return n
}

// methods returns the ordered list of received command methods.
func (b *fakeBrowser) methods() []string {
	b.mu.Lock()
	defer b.mu.Unlock()
	var out []string
	for _, q := range b.commands {
		out = append(out, fmt.Sprintf("%v", q["method"]))
	}
	return out
}

// next returns the next command received on this connection.
func (fc *fakeConn) next(c *taskcheck.C) (map[string]interface{}, bool) {
	select {
	case q := <-fc.received:
		return q, true
	case <-time.After(5 * time.Second):
		c.Check(false, "timeout waiting for command on page %s", fc.page)
		return nil, false
	}
}

// reply sends a command reply to the client.
func (fc *fakeConn) reply(id interface{}, result map[string]interface{}) error {
	fc.wmu.Lock()
	defer fc.wmu.Unlock()
	return fc.ws.WriteJSON(map[string]interface{}{"id": id, "result": result})
}

// event sends an event notification to the client.
func (fc *fakeConn) event(method string, params map[string]interface{}) error {
	fc.wmu.Lock()
	defer fc.wmu.Unlock()
	return fc.ws.WriteJSON(map[string]interface{}{"method": method, "params": params})
}

// close drops the connection server-side.
func (fc *fakeConn) close() {
	fc.ws.Close()
}

// autoReply replies to every command; Runtime.evaluate gets a numeric result.
func autoReply(b *fakeBrowser) {
	b.setOnCommand(func(fc *fakeConn, q map[string]interface{}) {
		result := map[string]interface{}{}
		if q["method"] == "Runtime.evaluate" {
			result["result"] = map[string]interface{}{"type": "number", "value": 42}
		}
		fc.reply(q["id"], result)
	})
}

// TestDispatchReentry proves that a callback can synchronously send commands
// while events keep queueing (within capacity), and that callbacks keep
// running serially in receive order.
func TestDispatchReentry(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		browser := newFakeBrowser(t, "one")
		autoReply(browser)

		remote, err := godet.Connect(browser.addr(), false)
		if !check(c, err == nil, "connect: %v", err) {
			return
		}
		defer remote.Close()

		fc := browser.waitConn(c)
		if fc == nil {
			return
		}

		const nevents = 10

		entered := make(chan int, nevents)
		release := make(chan struct{})
		done := make(chan int, nevents)
		var mu sync.Mutex
		var cbErrs []error

		remote.CallbackEvent("Test.event", func(p godet.Params) {
			seq := p.Int("seq")
			entered <- seq
			if seq == 1 {
				<-release // block the first callback so events pile up in the queue
			}
			// re-enter the debugger from inside the callback
			err := remote.MouseEvent(godet.MousePress, 10+seq, 20+seq, godet.LeftButton())
			mu.Lock()
			cbErrs = append(cbErrs, err)
			mu.Unlock()
			done <- seq
		})

		if !check(c, remote.PageEvents(true) == nil, "enable page events") {
			return
		}
		if q, ok := fc.next(c); !ok {
			return
		} else {
			c.Check(q["method"] == "Page.enable", "first command = %v", q["method"])
		}

		// queue nevents events; the first one blocks inside the callback
		for i := 1; i <= nevents; i++ {
			if err := fc.event("Test.event", map[string]interface{}{"seq": i}); !check(c, err == nil, "send event %d: %v", i, err) {
				return
			}
		}

		select {
		case seq := <-entered:
			c.Check(seq == 1, "first callback seq = %d", seq)
		case <-time.After(5 * time.Second):
			c.Check(false, "callback not invoked")
			return
		}
		close(release)

		for want := 1; want <= nevents; want++ {
			select {
			case seq := <-done:
				c.Check(seq == want, "callback order: got %d want %d", seq, want)
			case <-time.After(5 * time.Second):
				c.Check(false, "callback %d not invoked", want)
				return
			}
		}

		mu.Lock()
		nErrs := len(cbErrs)
		var firstErr error
		for _, e := range cbErrs {
			if e != nil {
				firstErr = e
				break
			}
		}
		mu.Unlock()
		c.Check(nErrs == nevents, "callback count = %d", nErrs)
		c.Check(firstErr == nil, "callback command error = %v", firstErr)

		// every callback re-entry produced exactly one mouse command, in order
		for want := 1; want <= nevents; want++ {
			q, ok := fc.next(c)
			if !ok {
				return
			}
			c.Check(q["method"] == "Input.dispatchMouseEvent", "command %d = %v", want, q["method"])
			p, _ := q["params"].(map[string]interface{})
			c.Check(p["type"] == "mousePressed" && p["button"] == "left" &&
				p["x"] == float64(10+want) && p["y"] == float64(20+want),
				"mouse params %d = %v", want, p)
		}

		// the connection is still usable for normal actions
		value, err := remote.Evaluate("6*7")
		c.Check(err == nil && value == float64(42), "evaluate = %v, %v", value, err)
		if q, ok := fc.next(c); !ok {
			return
		} else {
			c.Check(q["method"] == "Runtime.evaluate", "evaluate command = %v", q["method"])
		}

		closed := make(chan godet.Params, 1)
		remote.CallbackEvent(godet.EventClosed, func(p godet.Params) { closed <- p })
		c.Check(remote.Close() == nil, "close")
		select {
		case <-closed:
		case <-time.After(5 * time.Second):
			c.Check(false, "no closed notification")
		}
	})
}

// TestDispatchTermination exercises queue overflow, pending-command wakeups,
// concurrent Close and server-side disconnects.
func TestDispatchTermination(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		//
		// Event queue overflow terminates the connection.
		//
		{
			browser := newFakeBrowser(t, "one")
			browser.setOnCommand(func(fc *fakeConn, q map[string]interface{}) {
				if q["method"] == "Page.enable" {
					fc.reply(q["id"], map[string]interface{}{})
				}
				// Runtime.evaluate is left without a reply: the command stays pending
			})

			remote, err := godet.Connect(browser.addr(), false)
			if !check(c, err == nil, "connect: %v", err) {
				return
			}
			defer remote.Close()

			fc := browser.waitConn(c)
			if fc == nil {
				return
			}

			entered := make(chan struct{}, 2)
			release := make(chan struct{})
			action := make(chan error, 1)
			remote.CallbackEvent("Test.event", func(p godet.Params) {
				entered <- struct{}{}
				<-release
				// after the overflow this must fail without touching the socket
				action <- remote.MouseEvent(godet.MousePress, 1, 1)
			})
			disconnected := make(chan godet.Params, 1)
			remote.CallbackEvent(godet.EventDisconnect, func(p godet.Params) { disconnected <- p })
			closed := make(chan godet.Params, 1)
			remote.CallbackEvent(godet.EventClosed, func(p godet.Params) { closed <- p })

			if !check(c, remote.PageEvents(true) == nil, "enable page events") {
				return
			}
			if _, ok := fc.next(c); !ok {
				return
			}

			// a command that stays pending across the overflow
			pending := make(chan error, 1)
			go func() {
				_, err := remote.Evaluate("1+1")
				pending <- err
			}()
			if q, ok := fc.next(c); !ok {
				return
			} else {
				c.Check(q["method"] == "Runtime.evaluate", "pending command = %v", q["method"])
			}

			// the first event blocks the dispatcher inside the user callback
			if !check(c, fc.event("Test.event", map[string]interface{}{"seq": 1}) == nil, "send first event") {
				return
			}
			select {
			case <-entered:
			case <-time.After(5 * time.Second):
				c.Check(false, "callback not entered")
				return
			}

			// flood subscribed events beyond the pending capacity (256)
			for i := 0; i < 400; i++ {
				if fc.event("Test.event", map[string]interface{}{"seq": i + 2}) != nil {
					break // the client closed the connection on overflow
				}
			}

			select {
			case err := <-pending:
				c.Check(errors.Is(err, godet.ErrorEventOverflow), "pending command error = %v", err)
			case <-time.After(5 * time.Second):
				c.Check(false, "pending command not woken by overflow")
				return
			}

			// the blocked callback can still return; its action fails with the same reason
			close(release)
			select {
			case err := <-action:
				c.Check(errors.Is(err, godet.ErrorEventOverflow), "callback action error = %v", err)
			case <-time.After(5 * time.Second):
				c.Check(false, "blocked callback did not return")
				return
			}

			// exactly one termination notification, after the callback returned
			select {
			case p := <-disconnected:
				c.Check(p.String("reason") == "event queue overflow", "disconnect reason = %q", p.String("reason"))
			case <-time.After(5 * time.Second):
				c.Check(false, "no disconnect notification")
				return
			}
			select {
			case <-entered:
				c.Check(false, "queued events dispatched after overflow")
			default:
			}
			select {
			case <-closed:
				c.Check(false, "unexpected closed notification")
			default:
			}

			// the server saw the connection close and no extra commands were written
			select {
			case <-fc.closed:
			case <-time.After(5 * time.Second):
				c.Check(false, "server connection still open")
				return
			}
			c.Check(browser.count("Input.dispatchMouseEvent") == 0, "mouse actions written = %d", browser.count("Input.dispatchMouseEvent"))
			c.Check(browser.count("Page.enable") == 1 && browser.count("Runtime.evaluate") == 1,
				"commands = %v", browser.methods())

			// commands after termination fail immediately with the same reason
			_, err = remote.Evaluate("2+2")
			c.Check(errors.Is(err, godet.ErrorEventOverflow), "post-termination error = %v", err)
			c.Check(remote.Close() == nil, "close after overflow")
			_, err = remote.Evaluate("3+3")
			c.Check(errors.Is(err, godet.ErrorEventOverflow), "first reason must win, got = %v", err)
			c.Check(browser.count("Runtime.evaluate") == 1, "command written after termination")
			select {
			case <-closed:
				c.Check(false, "closed notification after overflow")
			default:
			}
		}

		//
		// Close wakes pending commands even while a callback is blocked,
		// and concurrent Close calls are safe.
		//
		{
			browser := newFakeBrowser(t, "one")
			browser.setOnCommand(func(fc *fakeConn, q map[string]interface{}) {
				if q["method"] == "Page.enable" {
					fc.reply(q["id"], map[string]interface{}{})
				}
			})

			remote, err := godet.Connect(browser.addr(), false)
			if !check(c, err == nil, "connect: %v", err) {
				return
			}
			defer remote.Close()

			fc := browser.waitConn(c)
			if fc == nil {
				return
			}

			entered := make(chan struct{}, 1)
			release := make(chan struct{})
			remote.CallbackEvent("Test.event", func(p godet.Params) {
				entered <- struct{}{}
				<-release
			})
			closed := make(chan godet.Params, 1)
			remote.CallbackEvent(godet.EventClosed, func(p godet.Params) { closed <- p })

			if !check(c, remote.PageEvents(true) == nil, "enable page events") {
				return
			}
			if _, ok := fc.next(c); !ok {
				return
			}
			if !check(c, fc.event("Test.event", map[string]interface{}{"seq": 1}) == nil, "send event") {
				return
			}
			select {
			case <-entered:
			case <-time.After(5 * time.Second):
				c.Check(false, "callback not entered")
				return
			}

			const nwait = 3
			results := make(chan error, nwait)
			for i := 0; i < nwait; i++ {
				go func(i int) {
					_, err := remote.Evaluate(fmt.Sprintf("expr-%d", i))
					results <- err
				}(i)
			}
			for i := 0; i < nwait; i++ {
				if _, ok := fc.next(c); !ok {
					return // all commands received and left pending
				}
			}

			closes := make(chan error, 4)
			for i := 0; i < 3; i++ {
				go func() { closes <- remote.Close() }()
			}
			closes <- remote.Close()
			for i := 0; i < 4; i++ {
				c.Check(<-closes == nil, "concurrent close")
			}

			// pending commands are woken with ErrorClose without waiting for the callback
			for i := 0; i < nwait; i++ {
				select {
				case err := <-results:
					c.Check(errors.Is(err, godet.ErrorClose), "pending command error = %v", err)
				case <-time.After(5 * time.Second):
					c.Check(false, "pending command not woken by close")
					return
				}
			}

			// the termination notification is delivered only after the callback returns
			select {
			case <-closed:
				c.Check(false, "notification delivered while callback blocked")
			default:
			}
			close(release)
			select {
			case <-closed:
			case <-time.After(5 * time.Second):
				c.Check(false, "no closed notification")
				return
			}
			select {
			case <-closed:
				c.Check(false, "duplicate closed notification")
			default:
			}

			_, err = remote.Evaluate("x")
			c.Check(errors.Is(err, godet.ErrorClose), "post-close error = %v", err)
			c.Check(remote.Close() == nil, "second close")

			select {
			case <-fc.closed:
			case <-time.After(5 * time.Second):
				c.Check(false, "server connection still open")
				return
			}
			c.Check(browser.count("Runtime.evaluate") == nwait, "evaluates written = %d", browser.count("Runtime.evaluate"))
			c.Check(browser.count("Page.enable") == 1, "commands = %v", browser.methods())
		}

		//
		// A server-side disconnect fails pending commands with
		// ErrorDisconnect but does not unsettle completed ones.
		//
		{
			browser := newFakeBrowser(t, "one")
			browser.setOnCommand(func(fc *fakeConn, q map[string]interface{}) {
				switch q["method"] {
				case "Page.enable":
					fc.reply(q["id"], map[string]interface{}{})
				case "Runtime.evaluate":
					params, _ := q["params"].(map[string]interface{})
					if params["expression"] == "ok" {
						fc.reply(q["id"], map[string]interface{}{
							"result": map[string]interface{}{"type": "number", "value": 7},
						})
					}
					// expression "hang" is left pending
				}
			})

			remote, err := godet.Connect(browser.addr(), false)
			if !check(c, err == nil, "connect: %v", err) {
				return
			}
			defer remote.Close()

			fc := browser.waitConn(c)
			if fc == nil {
				return
			}

			disconnected := make(chan godet.Params, 1)
			remote.CallbackEvent(godet.EventDisconnect, func(p godet.Params) { disconnected <- p })
			closed := make(chan godet.Params, 1)
			remote.CallbackEvent(godet.EventClosed, func(p godet.Params) { closed <- p })

			if !check(c, remote.PageEvents(true) == nil, "enable page events") {
				return
			}
			if _, ok := fc.next(c); !ok {
				return
			}

			type evalResult struct {
				v   interface{}
				err error
			}
			settled := make(chan evalResult, 1)
			go func() {
				v, err := remote.Evaluate("ok")
				settled <- evalResult{v, err}
			}()
			pending := make(chan error, 1)
			go func() {
				_, err := remote.Evaluate("hang")
				pending <- err
			}()

			got := 0
			for got < 2 {
				q, ok := fc.next(c)
				if !ok {
					return
				}
				if q["method"] == "Runtime.evaluate" {
					got++
				}
			}

			fc.close() // the server drops the connection

			select {
			case r := <-settled:
				c.Check(r.err == nil && r.v == float64(7), "settled command = %v, %v", r.v, r.err)
			case <-time.After(5 * time.Second):
				c.Check(false, "settled command not resolved")
				return
			}
			select {
			case err := <-pending:
				c.Check(errors.Is(err, godet.ErrorDisconnect), "pending error = %v", err)
				c.Check(!errors.Is(err, godet.ErrorEventOverflow) && !errors.Is(err, godet.ErrorClose),
					"wrong termination reason = %v", err)
			case <-time.After(5 * time.Second):
				c.Check(false, "pending command not woken by disconnect")
				return
			}
			select {
			case p := <-disconnected:
				c.Check(p.String("reason") == "", "unexpected disconnect reason %q", p.String("reason"))
			case <-time.After(5 * time.Second):
				c.Check(false, "no disconnect notification")
				return
			}
			select {
			case <-closed:
				c.Check(false, "unexpected closed notification")
			default:
			}

			_, err = remote.Evaluate("again")
			c.Check(errors.Is(err, godet.ErrorDisconnect), "post-disconnect error = %v", err)
			c.Check(remote.Close() == nil, "close after disconnect")
			select {
			case <-closed:
				c.Check(false, "closed notification after disconnect")
			default:
			}

			select {
			case <-fc.closed:
			case <-time.After(5 * time.Second):
				c.Check(false, "server connection still open")
				return
			}
			c.Check(browser.count("Runtime.evaluate") == 2, "evaluates written = %d", browser.count("Runtime.evaluate"))
		}

		//
		// A callback may call Close: Close does not wait for the callback.
		//
		{
			browser := newFakeBrowser(t, "one")
			autoReply(browser)

			remote, err := godet.Connect(browser.addr(), false)
			if !check(c, err == nil, "connect: %v", err) {
				return
			}
			defer remote.Close()

			fc := browser.waitConn(c)
			if fc == nil {
				return
			}

			closed := make(chan godet.Params, 1)
			remote.CallbackEvent(godet.EventClosed, func(p godet.Params) { closed <- p })

			type cbResult struct{ closeErr, cmdErr error }
			result := make(chan cbResult, 1)
			remote.CallbackEvent("Test.event", func(p godet.Params) {
				r := cbResult{closeErr: remote.Close()} // must not wait for this callback
				_, r.cmdErr = remote.Evaluate("x")
				result <- r
			})

			if !check(c, fc.event("Test.event", map[string]interface{}{"seq": 1}) == nil, "send event") {
				return
			}
			select {
			case r := <-result:
				c.Check(r.closeErr == nil, "close from callback: %v", r.closeErr)
				c.Check(errors.Is(r.cmdErr, godet.ErrorClose), "command in callback after close = %v", r.cmdErr)
			case <-time.After(5 * time.Second):
				c.Check(false, "callback did not return (Close deadlocked)")
				return
			}
			select {
			case <-closed:
			case <-time.After(5 * time.Second):
				c.Check(false, "no closed notification")
				return
			}
			select {
			case <-fc.closed:
			case <-time.After(5 * time.Second):
				c.Check(false, "server connection still open")
				return
			}
			c.Check(browser.count("Runtime.evaluate") == 0, "commands written after close = %d", browser.count("Runtime.evaluate"))
		}
	})
}

// TestDispatchCompatibility is the control group: normal actions, ignored
// events, duplicate replies and sequential tab activation keep working.
func TestDispatchCompatibility(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		browser := newFakeBrowser(t, "one", "two")
		browser.setOnCommand(func(fc *fakeConn, q map[string]interface{}) {
			result := map[string]interface{}{}
			if q["method"] == "Runtime.evaluate" {
				params, _ := q["params"].(map[string]interface{})
				if params["expression"] == "dup" {
					return // replied manually by the test
				}
				result["result"] = map[string]interface{}{"type": "number", "value": 42}
			}
			fc.reply(q["id"], result)
		})

		remote, err := godet.Connect(browser.addr(), false)
		if !check(c, err == nil, "connect: %v", err) {
			return
		}
		defer remote.Close()

		fc1 := browser.waitConn(c)
		if fc1 == nil {
			return
		}

		disconnected := make(chan godet.Params, 1)
		remote.CallbackEvent(godet.EventDisconnect, func(p godet.Params) { disconnected <- p })
		closed := make(chan godet.Params, 1)
		remote.CallbackEvent(godet.EventClosed, func(p godet.Params) { closed <- p })

		// plain actions still work and carry the expected payload
		if !check(c, remote.PageEvents(true) == nil, "enable page events") {
			return
		}
		if q, ok := fc1.next(c); !ok {
			return
		} else {
			c.Check(q["method"] == "Page.enable", "first command = %v", q["method"])
		}

		c.Check(remote.MouseEvent(godet.MousePress, 7, 9, godet.LeftButton(), godet.Clicks(2)) == nil, "mouse event")
		if q, ok := fc1.next(c); !ok {
			return
		} else {
			c.Check(q["method"] == "Input.dispatchMouseEvent", "mouse command = %v", q["method"])
			p, _ := q["params"].(map[string]interface{})
			c.Check(p["type"] == "mousePressed" && p["button"] == "left" &&
				p["x"] == float64(7) && p["y"] == float64(9) && p["clickCount"] == float64(2),
				"mouse params = %v", p)
		}

		value, err := remote.Evaluate("6*7")
		c.Check(err == nil && value == float64(42), "evaluate = %v, %v", value, err)
		if q, ok := fc1.next(c); !ok {
			return
		} else {
			c.Check(q["method"] == "Runtime.evaluate", "evaluate command = %v", q["method"])
			p, _ := q["params"].(map[string]interface{})
			c.Check(p["expression"] == "6*7" && p["returnByValue"] == true, "evaluate params = %v", p)
		}

		// events without a registered handler are ignored and do not consume capacity
		for i := 0; i < 300; i++ {
			if err := fc1.event("Noise.event", map[string]interface{}{"i": i}); !check(c, err == nil, "send noise %d: %v", i, err) {
				return
			}
		}
		seen := make(chan int, 1)
		remote.CallbackEvent("Test.event", func(p godet.Params) { seen <- p.Int("seq") })
		if !check(c, fc1.event("Test.event", map[string]interface{}{"seq": 5}) == nil, "send registered event") {
			return
		}
		select {
		case seq := <-seen:
			c.Check(seq == 5, "event seq = %d", seq)
		case <-time.After(5 * time.Second):
			c.Check(false, "registered event not delivered")
			return
		}
		value, err = remote.Evaluate("6*7")
		c.Check(err == nil && value == float64(42), "evaluate after noise = %v, %v", value, err)
		if _, ok := fc1.next(c); !ok {
			return
		}

		// duplicate and unknown replies are dropped without disturbing the loop
		evalDone := make(chan interface{}, 1)
		go func() {
			v, err := remote.Evaluate("dup")
			if err != nil {
				evalDone <- err
			} else {
				evalDone <- v
			}
		}()
		q, ok := fc1.next(c)
		if !ok {
			return
		}
		c.Check(q["method"] == "Runtime.evaluate", "dup command = %v", q["method"])
		id := q["id"]
		fc1.reply(id, map[string]interface{}{"result": map[string]interface{}{"type": "number", "value": 42}})
		fc1.reply(id, map[string]interface{}{"result": map[string]interface{}{"type": "number", "value": 42}})    // duplicate
		fc1.reply(12345, map[string]interface{}{"result": map[string]interface{}{"type": "number", "value": 42}}) // unknown id
		select {
		case v := <-evalDone:
			c.Check(v == float64(42), "evaluate after duplicate = %v", v)
		case <-time.After(5 * time.Second):
			c.Check(false, "command not settled after duplicate replies")
			return
		}
		value, err = remote.Evaluate("6*7")
		c.Check(err == nil && value == float64(42), "evaluate after duplicates = %v, %v", value, err)
		if _, ok := fc1.next(c); !ok {
			return
		}

		// sequential tab activation: the old socket closing is not a disconnect
		tabs, err := remote.TabList("page")
		if !check(c, err == nil && len(tabs) == 2, "tab list = %v, %v", tabs, err) {
			return
		}
		c.Check(tabs[0].ID == "one" && tabs[1].ID == "two", "tabs = %v, %v", tabs[0].ID, tabs[1].ID)
		if !check(c, remote.ActivateTab(tabs[1]) == nil, "activate tab") {
			return
		}
		select {
		case id := <-browser.activated:
			c.Check(id == "two", "activated = %q", id)
		case <-time.After(5 * time.Second):
			c.Check(false, "tab not activated")
			return
		}
		fc2 := browser.waitConn(c)
		if fc2 == nil {
			return
		}
		select {
		case <-fc1.closed:
		case <-time.After(5 * time.Second):
			c.Check(false, "old connection not closed")
			return
		}
		if q, ok := fc2.next(c); !ok {
			return
		} else {
			c.Check(q["method"] == "Page.enable", "re-enabled command = %v", q["method"])
		}
		value, err = remote.Evaluate("6*7")
		c.Check(err == nil && value == float64(42), "evaluate on new tab = %v, %v", value, err)
		if _, ok := fc2.next(c); !ok {
			return
		}
		select {
		case <-disconnected:
			c.Check(false, "old socket reported as disconnect")
		default:
		}

		// explicit Close still produces EventClosed
		c.Check(remote.Close() == nil, "close")
		select {
		case <-closed:
		case <-time.After(5 * time.Second):
			c.Check(false, "no closed notification")
			return
		}
		select {
		case <-fc2.closed:
		case <-time.After(5 * time.Second):
			c.Check(false, "new connection not closed")
		}
		select {
		case <-disconnected:
			c.Check(false, "unexpected disconnect notification")
		default:
		}
	})
}
