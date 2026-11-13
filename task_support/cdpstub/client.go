package cdpstub

import (
	"context"
	"encoding/json"
	"github.com/go-rod/rod/lib/cdp"
	"sync"
)

type Record struct {
	Session, Method string
	Params          json.RawMessage
}
type Client struct {
	mu      sync.Mutex
	records []Record
	events  chan *cdp.Event
	Called  chan Record
}

func New() *Client                         { return &Client{events: make(chan *cdp.Event, 64), Called: make(chan Record, 128)} }
func (c *Client) Event() <-chan *cdp.Event { return c.events }
func (c *Client) Call(ctx context.Context, s, m string, p interface{}) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	b, err := json.Marshal(p)
	if err != nil {
		return nil, err
	}
	r := Record{s, m, b}
	c.mu.Lock()
	c.records = append(c.records, r)
	c.mu.Unlock()
	c.Called <- r
	return []byte(`{}`), nil
}
func (c *Client) Records() []Record {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]Record(nil), c.records...)
}
func (c *Client) Emit(s, m string, p interface{}) error {
	b, e := json.Marshal(p)
	if e != nil {
		return e
	}
	c.events <- &cdp.Event{SessionID: s, Method: m, Params: b}
	return nil
}
