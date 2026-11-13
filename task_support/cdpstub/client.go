package cdpstub

import (
	"context"
	"encoding/json"
	"sync"

	"github.com/go-rod/rod/lib/cdp"
)

// Record is one protocol command sent through the stub client.
type Record struct {
	Session, Method string
	Params          json.RawMessage
	// Err is the failure injected for this attempt, nil for successful calls.
	Err error
}

// Client is a protocol recorder and injection boundary for tests: it implements
// rod.CDPClient, records every outgoing command attempt, and feeds injected
// events. Routing logic is not part of the stub.
type Client struct {
	mu      sync.Mutex
	records []Record
	events  chan *cdp.Event
	Called  chan Record

	// callMu guards the failure and gate hooks.
	callMu sync.Mutex
	// fails maps a protocol method to the error every following Call returns.
	fails map[string]error
	// gates blocks Calls of a method until the channel is released.
	gates map[string]chan struct{}
}

// New creates an empty stub client.
func New() *Client {
	return &Client{
		events: make(chan *cdp.Event, 64),
		Called: make(chan Record, 128),
		fails:  map[string]error{},
		gates:  map[string]chan struct{}{},
	}
}

// Event implements rod.CDPClient.
func (c *Client) Event() <-chan *cdp.Event { return c.events }

// Call implements rod.CDPClient. Matching gates block the command first, then
// the attempt is recorded; configured failures are returned afterwards so that
// the command count still includes the rejected attempt.
func (c *Client) Call(ctx context.Context, s, m string, p interface{}) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}

	c.callMu.Lock()
	gate := c.gates[m]
	c.callMu.Unlock()

	if gate != nil {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-gate:
		}
	}

	b, err := json.Marshal(p)
	if err != nil {
		return nil, err
	}

	c.callMu.Lock()
	fail := c.fails[m]
	c.callMu.Unlock()

	r := Record{Session: s, Method: m, Params: b, Err: fail}
	c.mu.Lock()
	c.records = append(c.records, r)
	c.mu.Unlock()

	select {
	case c.Called <- r:
	default:
	}

	if fail != nil {
		return nil, fail
	}

	return []byte(`{}`), nil
}

// Fail makes every following Call of the method return err until Pass is called.
func (c *Client) Fail(method string, err error) {
	c.callMu.Lock()
	c.fails[method] = err
	c.callMu.Unlock()
}

// Pass restores successful Calls for the method.
func (c *Client) Pass(method string) {
	c.callMu.Lock()
	delete(c.fails, method)
	c.callMu.Unlock()
}

// Gate blocks every following Call of the method. The returned release function
// unblocks the waiting calls and removes the gate.
func (c *Client) Gate(method string) (release func()) {
	ch := make(chan struct{})

	c.callMu.Lock()
	c.gates[method] = ch
	c.callMu.Unlock()

	return func() {
		c.callMu.Lock()
		delete(c.gates, method)
		c.callMu.Unlock()
		close(ch)
	}
}

// Records returns a snapshot of all command attempts, including rejected ones.
func (c *Client) Records() []Record {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]Record(nil), c.records...)
}

// Emit feeds one browser event bound to the session.
func (c *Client) Emit(s, m string, p interface{}) error {
	b, e := json.Marshal(p)
	if e != nil {
		return e
	}
	c.events <- &cdp.Event{SessionID: s, Method: m, Params: b}
	return nil
}
