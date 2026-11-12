/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package sse

import (
	"bytes"
	"context"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"gopkg.in/cenkalti/backoff.v1"
)

var (
	headerID    = []byte("id:")
	headerData  = []byte("data:")
	headerEvent = []byte("event:")
	headerRetry = []byte("retry:")

	// errSubscriptionStopped is used internally to unwind a connection
	// attempt after Unsubscribe has been called.
	errSubscriptionStopped = errors.New("sse: subscription stopped")
)

func ClientMaxBufferSize(s int) func(c *Client) {
	return func(c *Client) {
		c.maxBufferSize = s
	}
}

// ConnCallback defines a function to be called on a particular connection event
type ConnCallback func(c *Client)

// ResponseValidator validates a response
type ResponseValidator func(c *Client, resp *http.Response) error

// Client handles an incoming server stream
type Client struct {
	Retry             time.Time
	ReconnectStrategy backoff.BackOff
	disconnectcb      ConnCallback
	connectedcb       ConnCallback
	subscribed        map[chan *Event]*subscription
	Headers           map[string]string
	ReconnectNotify   backoff.Notify
	ResponseValidator ResponseValidator
	Connection        *http.Client
	URL               string
	LastEventID       atomic.Value // []byte
	maxBufferSize     int
	mu                sync.Mutex
	EncodingBase64    bool
	Connected         bool
}

// NewClient creates a new client
func NewClient(url string, opts ...func(c *Client)) *Client {
	c := &Client{
		URL:           url,
		Connection:    &http.Client{},
		Headers:       make(map[string]string),
		subscribed:    make(map[chan *Event]*subscription),
		maxBufferSize: 1 << 16,
	}

	for _, opt := range opts {
		opt(c)
	}

	return c
}

// Subscribe to a data stream
func (c *Client) Subscribe(stream string, handler func(msg *Event)) error {
	return c.SubscribeWithContext(context.Background(), stream, handler)
}

// SubscribeWithContext to a data stream with context
func (c *Client) SubscribeWithContext(ctx context.Context, stream string, handler func(msg *Event)) error {
	sub := newSubscription(c, ctx, stream, handler, nil)
	return sub.run()
}

// SubscribeChan sends all events to the provided channel
func (c *Client) SubscribeChan(stream string, ch chan *Event) error {
	return c.SubscribeChanWithContext(context.Background(), stream, ch)
}

// SubscribeChanWithContext sends all events to the provided channel with context
func (c *Client) SubscribeChanWithContext(ctx context.Context, stream string, ch chan *Event) error {
	sub := newSubscription(c, ctx, stream, nil, ch)
	sub.resultCh = make(chan error, 1)

	c.mu.Lock()
	c.subscribed[ch] = sub
	c.mu.Unlock()

	sub.start()

	// The subscription reports back once the first connection is established,
	// or with the final error when no connection could be established.
	return <-sub.resultCh
}

// Unsubscribe unsubscribes a channel. It is safe to call it repeatedly or
// concurrently; it returns only after this subscription has fully stopped and
// its bookkeeping has been removed. The caller's event channel is never closed.
func (c *Client) Unsubscribe(ch chan *Event) {
	c.mu.Lock()
	sub := c.subscribed[ch]
	c.mu.Unlock()

	if sub == nil {
		return
	}

	sub.stop()
	<-sub.done
}

// OnDisconnect specifies the function to run when the connection disconnects
func (c *Client) OnDisconnect(fn ConnCallback) {
	c.disconnectcb = fn
}

// OnConnect specifies the function to run when the connection is successful
func (c *Client) OnConnect(fn ConnCallback) {
	c.connectedcb = fn
}

// subscription is the lifetime of one Subscribe/SubscribeChan call. It owns
// the reconnection loop, the current response body and the delivery of events
// downstream. The LastEventID cursor is only advanced once an event has been
// handed to the caller (handler returned, or channel send succeeded).
type subscription struct {
	client   *Client
	stream   string
	handler  func(msg *Event)
	ch       chan *Event
	ctx      context.Context // caller supplied context
	runCtx   context.Context // canceled by either the caller or stop
	cancel   context.CancelFunc
	stopCh   chan struct{}
	stopOnce sync.Once
	done     chan struct{}

	connectOnce sync.Once
	connected   int32
	resultCh    chan error // non-nil for the channel entry points
}

func newSubscription(c *Client, ctx context.Context, stream string, handler func(*Event), ch chan *Event) *subscription {
	runCtx, cancel := context.WithCancel(ctx)
	return &subscription{
		client:  c,
		stream:  stream,
		handler: handler,
		ch:      ch,
		ctx:     ctx,
		runCtx:  runCtx,
		cancel:  cancel,
		stopCh:  make(chan struct{}),
		done:    make(chan struct{}),
	}
}

func (s *subscription) start() {
	go func() {
		// Delete registration before publishing done so that an Unsubscribe
		// returning from <-done can never observe stale bookkeeping.
		defer close(s.done)
		defer func() {
			s.client.mu.Lock()
			if s.ch != nil && s.client.subscribed[s.ch] == s {
				delete(s.client.subscribed, s.ch)
			}
			s.client.mu.Unlock()
		}()

		err := s.run()

		// If we never established a connection, report the final error (or
		// nil when the subscription was merely stopped) to the caller.
		if s.resultCh != nil && atomic.LoadInt32(&s.connected) == 0 {
			s.resultCh <- err
		}
	}()
}

func (s *subscription) stop() {
	s.stopOnce.Do(func() {
		close(s.stopCh)
		s.cancel()
	})
}

func (s *subscription) stopped() bool {
	select {
	case <-s.stopCh:
		return true
	default:
		return false
	}
}

func (s *subscription) run() error {
	defer s.cancel()

	strategy := s.client.ReconnectStrategy
	if strategy == nil {
		strategy = backoff.NewExponentialBackOff()
	}
	// Reset once at the beginning of the subscription, never per event.
	strategy.Reset()

	var lastErr error
	for {
		if s.stopped() {
			return nil
		}
		if err := s.ctx.Err(); err != nil {
			return err
		}

		lastErr = s.session()

		if s.stopped() {
			return nil
		}
		if err := s.ctx.Err(); err != nil {
			return err
		}
		if lastErr == nil {
			return nil
		}

		// Match RetryNotify's handling of permanent errors: do not retry.
		if permanent, ok := lastErr.(*backoff.PermanentError); ok {
			return permanent.Err
		}

		next := strategy.NextBackOff()
		if next == backoff.Stop {
			if s.stopped() {
				return nil
			}
			if err := s.ctx.Err(); err != nil {
				return err
			}
			// Report the error from the last connection attempt. A normal
			// end of stream therefore surfaces as io.EOF.
			return lastErr
		}

		if s.client.ReconnectNotify != nil {
			s.client.ReconnectNotify(lastErr, next)
		}

		if next <= 0 {
			continue
		}

		timer := time.NewTimer(next)
		select {
		case <-s.runCtx.Done():
			timer.Stop()
			if s.stopped() {
				return nil
			}
			if err := s.ctx.Err(); err != nil {
				return err
			}
			return lastErr
		case <-timer.C:
		}
	}
}

// session runs one request/response connection until it ends. A clean EOF is
// returned as io.EOF; the caller decides whether to reconnect.
func (s *subscription) session() error {
	resp, err := s.client.request(s.runCtx, s.stream)
	if err != nil {
		// http.Client guarantees the body is already closed when both a
		// response and an error are returned, but close defensively anyway.
		if resp != nil && resp.Body != nil {
			_ = resp.Body.Close()
		}
		return err
	}

	body := &onceCloseBody{ReadCloser: resp.Body}
	defer body.Close()

	// Closing the body is what interrupts a read that is blocked inside the
	// scanner when the context is canceled or Unsubscribe is called.
	watcherDone := make(chan struct{})
	defer close(watcherDone)
	go func() {
		select {
		case <-s.runCtx.Done():
			body.Close()
		case <-watcherDone:
		}
	}()

	if validator := s.client.ResponseValidator; validator != nil {
		if err := validator(s.client, resp); err != nil {
			return err
		}
	} else if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("could not connect to stream: %s", http.StatusText(resp.StatusCode))
	}

	s.signalConnected()

	reader := NewEventStreamReader(body, s.client.maxBufferSize)
	eventCh := make(chan *Event)
	errCh := make(chan error, 1)
	readDone := make(chan struct{})
	go func() {
		defer close(readDone)
		s.readLoop(reader, eventCh, errCh)
	}()

	// Always close the body first (unblocking a pending read), then wait for
	// the read goroutine so that this session leaves no reader behind.
	defer func() {
		body.Close()
		<-readDone
	}()

	for {
		select {
		case <-s.runCtx.Done():
			if s.stopped() {
				return errSubscriptionStopped
			}
			return s.ctx.Err()
		case err := <-errCh:
			return err
		case msg := <-eventCh:
			if err := s.deliver(msg); err != nil {
				return err
			}
		}
	}
}

// deliver hands one event downstream and only then advances the resume
// cursor. The ID is snapshotted first so that callers mutating Event.ID cannot
// corrupt the Last-Event-ID header used by later requests.
func (s *subscription) deliver(msg *Event) error {
	id := append([]byte(nil), msg.ID...)

	if s.handler != nil {
		// Do not start a handler once the subscription is stopping.
		select {
		case <-s.runCtx.Done():
			if s.stopped() {
				return errSubscriptionStopped
			}
			return s.ctx.Err()
		default:
		}

		s.handler(msg)
		s.advance(id)
		return nil
	}

	select {
	case s.ch <- msg:
		// A successful send, including to a buffered channel, counts as
		// delivered.
		s.advance(id)
		return nil
	case <-s.runCtx.Done():
		if s.stopped() {
			return errSubscriptionStopped
		}
		return s.ctx.Err()
	}
}

func (s *subscription) advance(id []byte) {
	if len(id) == 0 {
		return
	}
	// id is already a private copy; never share it with a caller's Event.ID.
	s.client.LastEventID.Store(id)
}

func (s *subscription) signalConnected() {
	atomic.StoreInt32(&s.connected, 1)
	s.connectOnce.Do(func() {
		if s.resultCh != nil {
			s.resultCh <- nil
		}
	})
}

func (s *subscription) readLoop(reader *EventStreamReader, outCh chan<- *Event, erChan chan<- error) {
	c := s.client
	lastID := c.lastDeliveredID()

	for {
		// Read each new line and process the type of event
		event, err := reader.ReadEvent()
		if err != nil {
			if err == io.EOF {
				// A normal end of stream is still a reconnectable event.
				erChan <- io.EOF
				return
			}
			// run user specified disconnect function
			if c.disconnectcb != nil {
				c.Connected = false
				c.disconnectcb(c)
			}
			erChan <- err
			return
		}

		if !c.Connected && c.connectedcb != nil {
			c.Connected = true
			c.connectedcb(c)
		}

		// If we get an error, ignore it.
		msg, err := c.processEvent(event)
		if err != nil {
			continue
		}

		// Preserve the original inheritance rule: an event without an ID
		// keeps the ID of the preceding record. This is only used for the
		// Event handed downstream; the resume cursor is advanced on delivery.
		if len(msg.ID) > 0 {
			lastID = append([]byte(nil), msg.ID...)
		} else if len(lastID) > 0 {
			msg.ID = append([]byte(nil), lastID...)
		}

		// Send downstream if the event has something useful
		if msg.hasContent() {
			select {
			case outCh <- msg:
			case <-s.runCtx.Done():
				return
			}
		}
	}
}

func (c *Client) lastDeliveredID() []byte {
	v, ok := c.LastEventID.Load().([]byte)
	if !ok || len(v) == 0 {
		return nil
	}
	return append([]byte(nil), v...)
}

// SubscribeRaw to an sse endpoint
func (c *Client) SubscribeRaw(handler func(msg *Event)) error {
	return c.Subscribe("", handler)
}

// SubscribeRawWithContext to an sse endpoint with context
func (c *Client) SubscribeRawWithContext(ctx context.Context, handler func(msg *Event)) error {
	return c.SubscribeWithContext(ctx, "", handler)
}

// SubscribeChanRaw sends all events to the provided channel
func (c *Client) SubscribeChanRaw(ch chan *Event) error {
	return c.SubscribeChan("", ch)
}

// SubscribeChanRawWithContext sends all events to the provided channel with context
func (c *Client) SubscribeChanRawWithContext(ctx context.Context, ch chan *Event) error {
	return c.SubscribeChanWithContext(ctx, "", ch)
}

func (c *Client) request(ctx context.Context, stream string) (*http.Response, error) {
	req, err := http.NewRequest("GET", c.URL, nil)
	if err != nil {
		return nil, err
	}
	req = req.WithContext(ctx)

	// Setup request, specify stream to connect to
	if stream != "" {
		query := req.URL.Query()
		query.Add("stream", stream)
		req.URL.RawQuery = query.Encode()
	}

	req.Header.Set("Cache-Control", "no-cache")
	req.Header.Set("Accept", "text/event-stream")
	req.Header.Set("Connection", "keep-alive")

	lastID, exists := c.LastEventID.Load().([]byte)
	if exists && lastID != nil {
		req.Header.Set("Last-Event-ID", string(lastID))
	}

	// Add user specified headers
	for k, v := range c.Headers {
		req.Header.Set(k, v)
	}

	return c.Connection.Do(req)
}

func (c *Client) processEvent(msg []byte) (event *Event, err error) {
	var e Event

	if len(msg) < 1 {
		return nil, errors.New("event message was empty")
	}

	// Normalize the crlf to lf to make it easier to split the lines.
	// Split the line by "\n" or "\r", per the spec.
	for _, line := range bytes.FieldsFunc(msg, func(r rune) bool { return r == '\n' || r == '\r' }) {
		switch {
		case bytes.HasPrefix(line, headerID):
			e.ID = append([]byte(nil), trimHeader(len(headerID), line)...)
		case bytes.HasPrefix(line, headerData):
			// The spec allows for multiple data fields per event, concatenated them with "\n".
			e.Data = append(e.Data[:], append(trimHeader(len(headerData), line), byte('\n'))...)
		// The spec says that a line that simply contains the string "data" should be treated as a data field with an empty body.
		case bytes.Equal(line, bytes.TrimSuffix(headerData, []byte(":"))):
			e.Data = append(e.Data, byte('\n'))
		case bytes.HasPrefix(line, headerEvent):
			e.Event = append([]byte(nil), trimHeader(len(headerEvent), line)...)
		case bytes.HasPrefix(line, headerRetry):
			e.Retry = append([]byte(nil), trimHeader(len(headerRetry), line)...)
		default:
			// Ignore any garbage that doesn't match what we're looking for.
		}
	}

	// Trim the last "\n" per the spec.
	e.Data = bytes.TrimSuffix(e.Data, []byte("\n"))

	if c.EncodingBase64 {
		buf := make([]byte, base64.StdEncoding.DecodedLen(len(e.Data)))

		n, err := base64.StdEncoding.Decode(buf, e.Data)
		if err != nil {
			err = fmt.Errorf("failed to decode event message: %s", err)
		}
		e.Data = buf[:n]
	}
	return &e, err
}

// onceCloseBody makes the response body close idempotent so that concurrent
// cancellation and normal session teardown cannot double-close it.
type onceCloseBody struct {
	io.ReadCloser
	once sync.Once
	err  error
}

func (b *onceCloseBody) Close() error {
	b.once.Do(func() {
		b.err = b.ReadCloser.Close()
	})
	return b.err
}

func trimHeader(size int, data []byte) []byte {
	if data == nil || len(data) < size {
		return data
	}

	data = data[size:]
	// Remove optional leading whitespace
	if len(data) > 0 && data[0] == 32 {
		data = data[1:]
	}
	// Remove trailing new line
	if len(data) > 0 && data[len(data)-1] == 10 {
		data = data[:len(data)-1]
	}
	return data
}
