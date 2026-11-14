package rpcc

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"strings"
	"sync"
)

// CDP methods used to tunnel messages through a parent connection that
// speaks the non-flattened Target session protocol.
const (
	methodSendMessageToTarget       = "Target.sendMessageToTarget"
	methodReceivedMessageFromTarget = "Target.receivedMessageFromTarget"
	methodDetachedFromTarget        = "Target.detachedFromTarget"
)

// ErrSessionExists indicates that a session Conn was already created for
// the session ID on this parent Conn. Closed sessions keep their ID
// reserved and are never reused.
var ErrSessionExists = errors.New("rpcc: session already exists")

// ErrSessionDetached indicates that the session ended because the remote
// sent a Target.detachedFromTarget event for it.
var ErrSessionDetached = &closeError{msg: "rpcc: the session was detached"}

// sessionProtocolError describes an encapsulated session message that
// could not be processed.
type sessionProtocolError struct {
	sessionID string
	err       error
}

func (e *sessionProtocolError) Error() string {
	return fmt.Sprintf("rpcc: session %s protocol error: %v", e.sessionID, e.err)
}
func (e *sessionProtocolError) Unwrap() error { return e.err }

// sessionState marks a Conn as a child session of another Conn. A root
// Conn leaves this nil. A child Conn owns its request sequence, pending
// calls and streams but has no physical connection of its own.
type sessionState struct {
	parent *Conn
	id     string
}

// sessionTunnel routes Target session messages between a parent Conn and
// the child session Conns created from it. A single tunnel per parent Conn
// is created lazily by NewSession, before any child Conn exists, so that
// no encapsulated message can arrive before the receive path is ready.
type sessionTunnel struct {
	parent *Conn

	// Subscriptions on the parent Conn. The parent's own subscribers
	// keep receiving the full events because notifications are fanned
	// out to every stream.
	messages Stream
	detached Stream

	mu       sync.Mutex // Protects following.
	sessions map[string]*Conn
	closed   bool
}

// newSessionTunnel starts listening for encapsulated messages on the
// parent Conn. Both receive subscriptions are established before any
// session Conn can be handed out.
func newSessionTunnel(parent *Conn) (*sessionTunnel, error) {
	t := &sessionTunnel{
		parent:   parent,
		sessions: make(map[string]*Conn),
	}

	messages, err := NewStream(parent.ctx, methodReceivedMessageFromTarget, parent)
	if err != nil {
		return nil, err
	}
	t.messages = messages

	detached, err := NewStream(parent.ctx, methodDetachedFromTarget, parent)
	if err != nil {
		_ = messages.Close()
		return nil, err
	}
	t.detached = detached

	go t.recvMessages()
	go t.recvDetached()

	return t, nil
}

// create registers a new session Conn for sessionID. It returns
// ErrSessionExists if the ID was already used, even by a closed session.
func (t *sessionTunnel) create(sessionID string) (*Conn, error) {
	t.mu.Lock()
	defer t.mu.Unlock()

	if t.closed {
		return nil, t.parent.err
	}
	if _, ok := t.sessions[sessionID]; ok {
		return nil, fmt.Errorf("%w: %s", ErrSessionExists, sessionID)
	}

	child := newSessionConn(t.parent, sessionID)
	t.sessions[sessionID] = child
	return child, nil
}

// lookup returns the session Conn for sessionID, if any.
func (t *sessionTunnel) lookup(sessionID string) (*Conn, bool) {
	t.mu.Lock()
	defer t.mu.Unlock()
	s, ok := t.sessions[sessionID]
	return s, ok
}

// recvMessages consumes Target.receivedMessageFromTarget notifications
// from the parent Conn until the parent Conn closes.
func (t *sessionTunnel) recvMessages() {
	for {
		var ev struct {
			SessionID string `json:"sessionId"`
			Message   string `json:"message"`
		}
		err := t.messages.RecvMsg(&ev)
		if err != nil {
			// Parent Conn closed (or stream ended); nothing more can
			// be delivered. Parent close ends every session.
			return
		}

		child, ok := t.lookup(ev.SessionID)
		if !ok {
			// Unknown session, ignore the encapsulated event.
			continue
		}

		if ev.Message == "" {
			child.closeSession(&sessionProtocolError{
				sessionID: ev.SessionID,
				err:       errors.New("missing encapsulated message"),
			})
			continue
		}

		// The encapsulated payload must be a single, self-contained
		// JSON value. Reject streams and trailing data so fragment
		// boundaries can never corrupt a session.
		dec := json.NewDecoder(strings.NewReader(ev.Message))
		var head json.RawMessage
		if err := dec.Decode(&head); err != nil {
			child.closeSession(&sessionProtocolError{
				sessionID: ev.SessionID,
				err:       fmt.Errorf("decoding encapsulated message: %w", err),
			})
			continue
		}
		var extra json.RawMessage
		if err := dec.Decode(&extra); err != io.EOF {
			cause := err
			if cause == nil {
				cause = errors.New("encapsulated message contains multiple JSON values")
			}
			child.closeSession(&sessionProtocolError{
				sessionID: ev.SessionID,
				err:       cause,
			})
			continue
		}

		child.deliverSession(ev.SessionID, head)
	}
}

// recvDetached consumes Target.detachedFromTarget notifications and ends
// the matching session Conn with ErrSessionDetached.
func (t *sessionTunnel) recvDetached() {
	for {
		var ev struct {
			SessionID string `json:"sessionId"`
		}
		err := t.detached.RecvMsg(&ev)
		if err != nil {
			return
		}
		if child, ok := t.lookup(ev.SessionID); ok {
			child.closeSession(ErrSessionDetached)
		}
	}
}

// close ends all session Conns and stops the parent event subscriptions.
// Called when the parent Conn closes or when a losing tunnel from a
// concurrent NewSession race is discarded.
func (t *sessionTunnel) close(err error) {
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return
	}
	t.closed = true
	sessions := t.sessions
	t.sessions = make(map[string]*Conn)
	t.mu.Unlock()

	for _, child := range sessions {
		child.closeSession(err)
	}
	_ = t.messages.Close()
	_ = t.detached.Close()
}

// newSessionConn builds a child Conn multiplexed over parent without
// opening any physical connection.
func newSessionConn(parent *Conn, sessionID string) *Conn {
	c := &Conn{
		sessionPending: make(map[uint64]*sessionCall),
		streams:        make(map[string]*streamClients),
		session: &sessionState{
			parent: parent,
			id:     sessionID,
		},
	}
	c.ctx, c.cancel = context.WithCancel(parent.ctx)
	return c
}

// sessionCall is one in-flight tunneled command. call is the user-facing
// rpcCall owned by Invoke and is never mutated; the tunnel signals the
// private inner channel instead. Capacity 2 means a result followed by a
// session close can never block the sender.
type sessionCall struct {
	call  *rpcCall
	inner chan error
}

// NewSession returns a child Conn that controls the non-flattened CDP
// session identified by sessionID over this Conn. The session must already
// be attached; NewSession neither attaches nor starts Chrome and it does
// not open a new physical connection.
//
// The returned Conn supports the generated protocol clients and NewStream
// exactly like a regular Conn. Commands are encoded as ordinary CDP
// messages and sent with Target.sendMessageToTarget, and encapsulated
// Target.receivedMessageFromTarget messages are dispatched back to it.
//
// An empty sessionID is rejected locally. A sessionID can only be claimed
// once per parent Conn; repeat creation (also after the child was closed)
// returns an error wrapping ErrSessionExists. Closing the child ends only
// its own in-flight commands and streams; it does not detach the session
// and leaves the parent Conn and sibling sessions untouched.
func (c *Conn) NewSession(sessionID string) (*Conn, error) {
	if sessionID == "" {
		return nil, errors.New("rpcc: NewSession: sessionID must not be empty")
	}

	c.mu.Lock()
	if c.closed {
		err := c.err
		c.mu.Unlock()
		return nil, err
	}
	if c.session != nil {
		c.mu.Unlock()
		return nil, errors.New("rpcc: NewSession: nested sessions are not supported")
	}
	tunnel := c.tunnel
	c.mu.Unlock()

	if tunnel == nil {
		// Register the receive path before any child Conn exists.
		created, err := newSessionTunnel(c)
		if err != nil {
			return nil, err
		}

		c.mu.Lock()
		switch {
		case c.closed:
			err = c.err
			c.mu.Unlock()
			created.close(err)
			return nil, err
		case c.tunnel != nil:
			// Lost a race with a concurrent NewSession, discard ours.
			tunnel = c.tunnel
			c.mu.Unlock()
			created.close(ErrConnClosing)
		default:
			c.tunnel = created
			tunnel = created
			c.mu.Unlock()
		}
	}

	return tunnel.create(sessionID)
}

// sendSession encodes the child command as an ordinary CDP request and
// tunnels it through the parent Conn with Target.sendMessageToTarget. The
// outer request is a local value owned by this call, so the parent's
// reusable Request buffer is never referenced by asynchronous work.
//
// Invoke only returns once both the outer acceptance and the inner result
// are available. An outer ResponseError always takes precedence, even if
// an inner result arrived first. Multiple commands of the same session can
// be awaiting outer confirmation concurrently; the parent's write lock is
// only held while a request is encoded.
func (c *Conn) sendSession(ctx context.Context, call *rpcCall) (err error) {
	defer func() {
		// Give precedence for user cancellation.
		select {
		case <-ctx.Done():
			err = ctx.Err()
		default:
		}
	}()

	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return c.err
	}
	c.reqSeq++
	innerID := c.reqSeq
	// The tunnel (deliverSession, closeSession) signals the private
	// inner channel; Invoke keeps waiting on call.Error and only ever
	// sees the single, gated outcome handed to it by finish.
	sc := &sessionCall{
		call:  call,
		inner: make(chan error, 2),
	}
	c.sessionPending[innerID] = sc
	c.mu.Unlock()

	finish := func(err error) error {
		c.mu.Lock()
		delete(c.sessionPending, innerID)
		c.mu.Unlock()
		call.Error <- err
		return err
	}

	// Encode the inner CDP request up front from a local value.
	inner, err := json.Marshal(&Request{
		ID:     innerID,
		Method: call.Method,
		Args:   call.Args,
	})
	if err != nil {
		return finish(err)
	}

	outerArgs := &sendMessageToTargetArgs{
		Message:   string(inner),
		SessionID: c.session.id,
	}

	// The outer request is canceled when the child Conn ends so that an
	// acknowledgement that never arrives cannot leave a parent call (and
	// this goroutine) dangling after a detach or close.
	outerCtx, cancelOuter := context.WithCancel(ctx)
	stopCloseWatch := context.AfterFunc(c.ctx, cancelOuter)
	defer func() {
		stopCloseWatch()
		cancelOuter()
	}()

	// Buffered so the parent reply can never block on this goroutine
	// while the inner result is being awaited.
	outerErr := make(chan error, 1)
	go func() {
		outerErr <- Invoke(outerCtx, methodSendMessageToTarget, outerArgs, nil, c.session.parent)
	}()

	var outer error
	outerDone := false
	var innerErr error
	innerParked := false
	innerCh := sc.inner

	// Wait for the outer acceptance, parking any inner result that
	// arrives first. The inner result must never overtake an outstanding
	// outer confirmation; a nil channel blocks forever in the select so
	// the parked value is only read again after the outer settles. The
	// child context unblocks the wait when the session is closed,
	// detached or its parent Conn closes, even if the outer
	// acknowledgement never arrives.
	for !outerDone {
		select {
		case <-ctx.Done():
			return finish(ctx.Err())
		case <-c.ctx.Done():
			return finish(c.sessionCloseErr())
		case outer = <-outerErr:
			outerDone = true
		case err := <-innerCh:
			innerErr = err
			innerParked = true
			innerCh = nil
		}
	}

	// The outer request settled. A session ending while the outer
	// acknowledgement was outstanding always wins: its terminal error
	// (detach, bad message, parent close) takes precedence over both the
	// outer failure and any inner answer that merely arrived earlier.
	if err := c.sessionCloseErr(); err != nil {
		return finish(err)
	}

	// The session is still open, so an outer failure (including a
	// ResponseError) takes precedence over a parked inner result.
	if outer != nil {
		return finish(outer)
	}

	// Outer acceptance succeeded; return the parked inner result or wait
	// for it to arrive.
	if innerParked {
		return finish(innerErr)
	}
	select {
	case <-ctx.Done():
		return finish(ctx.Err())
	case <-c.ctx.Done():
		return finish(c.sessionCloseErr())
	case err := <-innerCh:
		return finish(err)
	}
}

// sessionCloseErr returns the terminal session error once the child Conn
// is closed, nil while it is still open.
func (c *Conn) sessionCloseErr() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return c.err
	}
	return nil
}

// sendMessageToTargetArgs are the params for Target.sendMessageToTarget,
// encoded locally so that rpcc does not depend on the generated protocol
// packages.
type sendMessageToTargetArgs struct {
	Message   string `json:"message"`
	SessionID string `json:"sessionId"`
}

// deliverSession feeds one encapsulated JSON-RPC message into the child
// Conn, dispatching responses to pending calls and notifications to its
// streams. Malformed responses only close this session Conn.
func (c *Conn) deliverSession(sessionID string, data []byte) {
	var resp Response
	if err := json.Unmarshal(data, &resp); err != nil {
		c.closeSession(&sessionProtocolError{
			sessionID: sessionID,
			err:       fmt.Errorf("decoding encapsulated response: %w", err),
		})
		return
	}

	if resp.Method != "" {
		c.notify(resp.Method, resp.Args)
		return
	}

	c.mu.Lock()
	sc := c.sessionPending[resp.ID]
	delete(c.sessionPending, resp.ID)
	closed := c.closed
	c.mu.Unlock()

	if closed {
		// Late response after close (e.g. detach); it must never
		// settle a call twice.
		return
	}

	switch {
	case sc == nil:
		// No pending call, this could mean the request was canceled
		// or an unexpected response arrived.
		if enableDebug {
			log.Println("rpcc: no pending session call: " + resp.String())
		}
	case resp.Error != nil:
		sc.inner <- resp.Error
	default:
		var err error
		if sc.call.Reply != nil {
			if err = json.Unmarshal(resp.Result, sc.call.Reply); err != nil {
				err = fmt.Errorf("rpcc: decoding %s: %s", sc.call.Method, err.Error())
			}
		}
		sc.inner <- err
	}
}

// closeSession ends a child session Conn with err. No detach is sent, the
// parent Conn and sibling sessions are untouched. Safe to call repeatedly;
// subsequent calls return the error that closed the Conn.
func (c *Conn) closeSession(err error) error {
	c.mu.Lock()
	if c.closed {
		closedErr := c.err
		c.mu.Unlock()
		if closedErr == ErrConnClosing {
			return nil
		}
		return closedErr
	}
	c.closed = true
	if err == nil {
		err = ErrConnClosing
	} else {
		err = &closeError{msg: ErrConnClosing.msg, err: err}
	}
	c.err = err
	pending := c.sessionPending
	c.sessionPending = make(map[uint64]*sessionCall)
	// Stop all streams by signaling that the connection is closed.
	c.streams = nil
	c.mu.Unlock()

	for _, sc := range pending {
		sc.inner <- err
	}

	// Delay cancel until c.err has settled, at this point any active
	// streams will be closed.
	c.cancel()

	if err == ErrConnClosing {
		return nil
	}
	return err
}
