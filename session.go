package coap

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"time"
)

// Default SessionConfig parameters, applied when a zero SessionConfig is
// passed to NewSession.
const (
	// DefaultAckTimeout is the base spacing of the retransmission
	// schedule.  It matches the library's historical ResponseTimeout.
	DefaultAckTimeout = ResponseTimeout
	// DefaultMaxRetransmit is the maximum number of retransmissions of
	// a confirmable message, matching the library default.
	DefaultMaxRetransmit = MaxRetransmit
	// DefaultExchangeLifetime is the default upper bound on a single
	// exchange, from RFC 7252 section 4.8.2 (EXCHANGE_LIFETIME).
	DefaultExchangeLifetime = 247 * time.Second
)

// Session errors.
var (
	// ErrTimeout is returned by Session.Request when the
	// retransmission schedule or the exchange lifetime is exhausted
	// without a response.
	ErrTimeout = errors.New("coap: exchange timed out")
	// ErrReset is returned by Session.Request when the peer answers
	// the request with a Reset message.
	ErrReset = errors.New("coap: reset by peer")
	// ErrSessionClosed is returned by Session.Request when the session
	// is closed while the request is in flight.
	ErrSessionClosed = errors.New("coap: session closed")
	// ErrMessageIDInUse is returned by Session.Request when the
	// caller-provided message ID belongs to an active exchange or one
	// whose exchange lifetime has not yet expired.
	ErrMessageIDInUse = errors.New("coap: message ID in use")
	// ErrTokenInUse is returned by Session.Request when the
	// caller-provided token belongs to an active exchange or one whose
	// exchange lifetime has not yet expired.
	ErrTokenInUse = errors.New("coap: token in use")
	// ErrInvalidRequest is returned by Session.Request when the
	// request message does not satisfy the session requirements
	// (confirmable type, request code, token length).
	ErrInvalidRequest = errors.New("coap: invalid request")
)

// SessionConfig tunes the retransmission and expiry behaviour of a
// Session.  A zero SessionConfig selects the library defaults
// (DefaultAckTimeout, DefaultMaxRetransmit, DefaultExchangeLifetime).
// An explicitly configured session requires positive AckTimeout and
// ExchangeLifetime and a non-negative MaxRetransmit.
type SessionConfig struct {
	// AckTimeout is the base interval after which an unacknowledged
	// confirmable request is retransmitted.  The interval doubles after
	// every retransmission.
	AckTimeout time.Duration
	// MaxRetransmit is the maximum number of retransmissions of the
	// original message (so at most 1+MaxRetransmit datagrams are sent).
	MaxRetransmit int
	// ExchangeLifetime bounds a single exchange from its first
	// transmission.  It applies whether or not an empty ACK has been
	// received, and it also determines how long the message ID and
	// token of a finished exchange stay reserved.
	ExchangeLifetime time.Duration
}

func (c SessionConfig) resolved() (SessionConfig, error) {
	if c == (SessionConfig{}) {
		return SessionConfig{
			AckTimeout:       DefaultAckTimeout,
			MaxRetransmit:    DefaultMaxRetransmit,
			ExchangeLifetime: DefaultExchangeLifetime,
		}, nil
	}
	if c.AckTimeout <= 0 {
		return SessionConfig{}, fmt.Errorf("coap: AckTimeout must be positive, got %v", c.AckTimeout)
	}
	if c.ExchangeLifetime <= 0 {
		return SessionConfig{}, fmt.Errorf("coap: ExchangeLifetime must be positive, got %v", c.ExchangeLifetime)
	}
	if c.MaxRetransmit < 0 {
		return SessionConfig{}, fmt.Errorf("coap: MaxRetransmit must be non-negative, got %v", c.MaxRetransmit)
	}
	return c, nil
}

// sessionTimer abstracts time.Timer so tests can drive the
// retransmission schedule with a controllable clock.
type sessionTimer interface {
	C() <-chan time.Time
	Stop() bool
}

// sessionClock abstracts the clock used by a Session for scheduling.
type sessionClock interface {
	Now() time.Time
	NewTimer(d time.Duration) sessionTimer
}

type realClock struct{}

func (realClock) Now() time.Time { return time.Now() }

func (realClock) NewTimer(d time.Duration) sessionTimer {
	return realTimer{time.NewTimer(d)}
}

type realTimer struct{ t *time.Timer }

func (r realTimer) C() <-chan time.Time { return r.t.C }
func (r realTimer) Stop() bool          { return r.t.Stop() }

type exchangeState uint8

const (
	exchangeActive exchangeState = iota
	exchangeRetired
)

// exchange tracks a single request/response exchange.  Records are kept
// (in retired state) until the exchange lifetime from the first send
// expires so message IDs and tokens cannot be reused prematurely and
// duplicate responses can still be acknowledged.
type exchange struct {
	mid    uint16
	token  string
	start  time.Time
	expiry time.Time

	state exchangeState
	acked bool
	ackd  chan struct{} // closed when the first empty ACK arrives
	done  chan struct{} // closed by dispatch once a result is published
	resp  *Message
	err   error

	// deliveredCON/respMID remember a delivered confirmable separate
	// response so its retransmissions can be acknowledged again without
	// being delivered twice.
	deliveredCON bool
	respMID      uint16
}

// Session drives concurrent CoAP request/response exchanges with a
// single peer over one packet connection.
//
// A Session separates "transmission confirmed" from "response
// received": an empty ACK only stops retransmission of the request,
// while Request keeps waiting for the response, which may arrive as a
// piggybacked ACK or as a separate CON/NON message with a different
// message ID.  This is the behaviour the older Conn.Send/Receive pair
// (which is unchanged and independent of Session) cannot express,
// because Send only ever waits for the next single message.
//
// Ownership: NewSession takes over the supplied packet connection.  The
// caller must not read from, write to, or close it directly once the
// session is created; Close shuts the connection down.  The session
// only accepts datagrams from the peer address it was created with.
//
// Wire format: the read loop, retransmission scheduling and message
// matching all use the package's existing Message encoding
// (MarshalBinary/ParseMessage); no new framing is introduced.
//
// Session calls pc.WriteTo while holding its internal lock, so the
// packet connection must not call back into the session.
type Session struct {
	pc   net.PacketConn
	peer net.Addr
	cfg  SessionConfig
	clk  sessionClock

	// connected is true when pc is a connected *net.UDPConn, in which
	// case datagrams are sent with Write instead of WriteTo.
	connected bool

	mu        sync.Mutex
	exchanges map[uint16]*exchange // by request message ID
	byToken   map[string]*exchange // by request token
	closed    bool
	readErr   error // original read-loop failure, propagated to waiters
	done      chan struct{}

	closeOnce sync.Once
	closeErr  error
	wg        sync.WaitGroup
}

// NewSession creates a Session exchanging messages with peer over pc,
// taking ownership of pc (see the Session documentation).  pc is
// typically an unconnected UDP socket (net.ListenUDP); a connected
// *net.UDPConn is also accepted.  Close must be called to release the
// connection.
func NewSession(pc net.PacketConn, peer net.Addr, cfg SessionConfig) (*Session, error) {
	return newSession(pc, peer, cfg, realClock{})
}

func newSession(pc net.PacketConn, peer net.Addr, cfg SessionConfig, clk sessionClock) (*Session, error) {
	if pc == nil || peer == nil {
		return nil, errors.New("coap: nil packet connection or peer address")
	}
	rcfg, err := cfg.resolved()
	if err != nil {
		return nil, err
	}
	s := &Session{
		pc:        pc,
		peer:      peer,
		cfg:       rcfg,
		clk:       clk,
		exchanges: make(map[uint16]*exchange),
		byToken:   make(map[string]*exchange),
		done:      make(chan struct{}),
	}
	if uc, ok := pc.(*net.UDPConn); ok && uc.RemoteAddr() != nil {
		s.connected = true
	}
	s.wg.Add(1)
	go s.readLoop()
	return s, nil
}

// Request sends a confirmable request and waits for its response.
//
// The request must be Confirmable, carry one of the four request codes
// (GET, POST, PUT, DELETE) and a token of 1 to 8 bytes.  The message ID
// and token are provided by the caller; they must not belong to an
// active exchange or to one whose exchange lifetime has not expired
// (ErrMessageIDInUse/ErrTokenInUse, reported before anything is sent).
//
// Once accepted, the request is snapshotted (options and payload
// included); later mutation of the caller's Message does not affect
// retransmissions.  If no ACK or response arrives, the identical
// datagram is retransmitted after AckTimeout, then after doubling
// intervals, at most MaxRetransmit extra times; ErrTimeout is returned
// when the final wait or the ExchangeLifetime (from the first send) is
// exhausted.
//
// Matching: an empty ACK or Reset matches by message ID, a response
// matches by token, and a piggybacked ACK carrying the response must
// match both.  An empty ACK only stops retransmission; Request keeps
// waiting for a separate response.  A confirmable separate response is
// acknowledged (empty ACK) before it is delivered.  Response codes in
// the 4.xx/5.xx range are ordinary responses, not transport errors.
// A Reset matching the request yields ErrReset.  Cancelling ctx ends
// only this request.
func (s *Session) Request(ctx context.Context, req Message) (*Message, error) {
	if err := validateRequest(req); err != nil {
		return nil, err
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	snap := snapshotRequest(req)
	wire, err := snap.MarshalBinary()
	if err != nil {
		return nil, err
	}
	token := string(snap.Token)

	s.mu.Lock()
	if s.closed {
		err := s.closedErrLocked()
		s.mu.Unlock()
		return nil, err
	}
	now := s.clk.Now()
	s.purgeLocked(now)
	if _, ok := s.exchanges[req.MessageID]; ok {
		s.mu.Unlock()
		return nil, fmt.Errorf("coap: message ID %d: %w", req.MessageID, ErrMessageIDInUse)
	}
	if _, ok := s.byToken[token]; ok {
		s.mu.Unlock()
		return nil, fmt.Errorf("coap: token %x: %w", token, ErrTokenInUse)
	}
	ex := &exchange{
		mid:    req.MessageID,
		token:  token,
		start:  now,
		expiry: now.Add(s.cfg.ExchangeLifetime),
		state:  exchangeActive,
		ackd:   make(chan struct{}),
		done:   make(chan struct{}),
	}
	s.exchanges[ex.mid] = ex
	s.byToken[token] = ex
	if err := s.sendLocked(wire); err != nil {
		delete(s.exchanges, ex.mid)
		delete(s.byToken, token)
		s.mu.Unlock()
		return nil, err
	}
	s.mu.Unlock()

	wait := s.cfg.AckTimeout
	rt := s.clk.NewTimer(wait)
	rtC := rt.C()
	lt := s.clk.NewTimer(s.cfg.ExchangeLifetime)
	defer rt.Stop()
	defer lt.Stop()
	retransmits := 0

	for {
		select {
		case <-ex.done:
			// Result published by dispatch.
			s.mu.Lock()
			resp, err := ex.resp, ex.err
			s.mu.Unlock()
			return resp, err
		case <-ex.ackd:
			// Empty ACK: stop retransmitting, keep waiting for
			// the separate response until the exchange lifetime
			// runs out.
			rt.Stop()
			rtC = nil
		case <-rtC:
			// A request already cancelled, finished or expired at
			// this boundary must not be revived by the timer.
			if err := ctx.Err(); err != nil {
				return s.finishSelf(ex, nil, err)
			}
			s.mu.Lock()
			if ex.state != exchangeActive || ex.acked || !s.clk.Now().Before(ex.expiry) {
				s.mu.Unlock()
				rtC = nil
				continue
			}
			if retransmits >= s.cfg.MaxRetransmit {
				s.mu.Unlock()
				return s.finishSelf(ex, nil, fmt.Errorf("coap: message %d: %w", ex.mid, ErrTimeout))
			}
			// Best effort: a broken connection is reported to all
			// waiters through the read loop.
			s.sendLocked(wire)
			s.mu.Unlock()
			retransmits++
			wait *= 2
			rt = s.clk.NewTimer(wait)
			rtC = rt.C()
		case <-lt.C():
			if err := ctx.Err(); err != nil {
				return s.finishSelf(ex, nil, err)
			}
			return s.finishSelf(ex, nil, fmt.Errorf("coap: message %d: %w", ex.mid, ErrTimeout))
		case <-ctx.Done():
			return s.finishSelf(ex, nil, ctx.Err())
		case <-s.done:
			s.mu.Lock()
			if ex.state == exchangeActive {
				ex.state = exchangeRetired
				ex.err = s.closedErrLocked()
			}
			resp, err := ex.resp, ex.err
			s.mu.Unlock()
			return resp, err
		}
	}
}

// Close ends all waiting requests (with ErrSessionClosed unless the
// connection already failed) and closes the packet connection the
// session owns.  It is safe to call Close more than once.
func (s *Session) Close() error {
	s.terminate(nil)
	s.closeOnce.Do(func() {
		s.closeErr = s.pc.Close()
	})
	s.wg.Wait()
	return s.closeErr
}

// terminate releases all waiters.  A non-nil err is a connection read
// failure and is propagated to current and future callers unchanged.
func (s *Session) terminate(readErr error) {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return
	}
	s.closed = true
	s.readErr = readErr
	s.mu.Unlock()
	close(s.done)
}

func (s *Session) closedErrLocked() error {
	if s.readErr != nil {
		return s.readErr
	}
	return ErrSessionClosed
}

// finishSelf retires ex from the requesting goroutine (cancellation,
// timeout).  The first completion wins: if dispatch already published a
// result, that result is returned instead.
func (s *Session) finishSelf(ex *exchange, resp *Message, err error) (*Message, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if ex.state == exchangeActive {
		ex.state = exchangeRetired
		ex.resp = resp
		ex.err = err
	}
	return ex.resp, ex.err
}

// purgeLocked drops retired exchanges whose lifetime (from the first
// send) has expired, freeing their message IDs and tokens for reuse.
func (s *Session) purgeLocked(now time.Time) {
	for mid, ex := range s.exchanges {
		if ex.state == exchangeRetired && !now.Before(ex.expiry) {
			delete(s.exchanges, mid)
			delete(s.byToken, ex.token)
		}
	}
}

func (s *Session) activeByMIDLocked(mid uint16) *exchange {
	if ex, ok := s.exchanges[mid]; ok && ex.state == exchangeActive {
		return ex
	}
	return nil
}

// sendLocked transmits a datagram to the peer.  It must be called with
// s.mu held.
func (s *Session) sendLocked(wire []byte) error {
	if s.connected {
		if w, ok := s.pc.(io.Writer); ok {
			_, err := w.Write(wire)
			return err
		}
	}
	_, err := s.pc.WriteTo(wire, s.peer)
	return err
}

// sendControlLocked transmits an empty ACK or Reset for mid.  It must
// be called with s.mu held.
func (s *Session) sendControlLocked(typ COAPType, mid uint16) {
	wire, err := (&Message{Type: typ, MessageID: mid}).MarshalBinary()
	if err != nil {
		return
	}
	s.sendLocked(wire)
}

func addrEqual(a, b net.Addr) bool {
	return a != nil && b != nil && a.String() == b.String()
}

func isResponseCode(c COAPCode) bool { return c>>5 >= 2 }

// readLoop receives datagrams from the peer until the connection fails
// or is closed.  A read failure is propagated to every waiting request.
func (s *Session) readLoop() {
	defer s.wg.Done()
	buf := make([]byte, maxPktLen)
	for {
		n, addr, err := s.pc.ReadFrom(buf)
		if err != nil {
			s.terminate(err)
			return
		}
		if !addrEqual(addr, s.peer) {
			continue
		}
		// ParseMessage retains references to the input (payload and
		// opaque option values), so give each datagram its own copy.
		data := make([]byte, n)
		copy(data, buf[:n])
		msg, err := ParseMessage(data)
		if err != nil {
			// Malformed datagrams never complete a wait.
			continue
		}
		s.dispatch(msg)
	}
}

// dispatch matches a syntactically valid message from the peer against
// the known exchanges.  Only responses are handled; the session does
// not implement a server-side request handler.
func (s *Session) dispatch(msg Message) {
	s.mu.Lock()
	s.purgeLocked(s.clk.Now())
	var doneToClose *exchange
	switch msg.Type {
	case Acknowledgement:
		switch {
		case msg.Code == 0:
			// Empty ACK: matched by message ID, only stops
			// retransmission.
			if ex := s.activeByMIDLocked(msg.MessageID); ex != nil && !ex.acked {
				ex.acked = true
				close(ex.ackd)
			}
		case isResponseCode(msg.Code):
			// Piggybacked response: must match both the request
			// message ID and its token.
			if ex := s.activeByMIDLocked(msg.MessageID); ex != nil && ex.token == string(msg.Token) {
				m := msg
				ex.state = exchangeRetired
				ex.resp = &m
				doneToClose = ex
			}
		}
		// Any other ACK is unknown and ignored.
	case Reset:
		if ex := s.activeByMIDLocked(msg.MessageID); ex != nil {
			ex.state = exchangeRetired
			ex.err = fmt.Errorf("coap: message %d: %w", ex.mid, ErrReset)
			doneToClose = ex
		}
		// Unknown Resets are ignored.
	case Confirmable, NonConfirmable:
		if !isResponseCode(msg.Code) {
			// Requests and empty messages are not handled here.
			break
		}
		ex := s.byToken[string(msg.Token)]
		switch {
		case ex != nil && ex.state == exchangeActive:
			m := msg
			ex.state = exchangeRetired
			ex.resp = &m
			if msg.Type == Confirmable {
				// Acknowledge before delivering the response.
				ex.deliveredCON = true
				ex.respMID = msg.MessageID
				s.sendControlLocked(Acknowledgement, msg.MessageID)
			}
			doneToClose = ex
		case ex != nil:
			// Retired exchange: a retransmitted confirmable
			// response that was already delivered is acknowledged
			// again but not delivered twice.  Anything else
			// confirmable is rejected.
			if msg.Type == Confirmable {
				if ex.deliveredCON && ex.respMID == msg.MessageID {
					s.sendControlLocked(Acknowledgement, msg.MessageID)
				} else {
					s.sendControlLocked(Reset, msg.MessageID)
				}
			}
		default:
			// Unknown confirmable responses get a Reset; unknown
			// non-confirmable ones are ignored.
			if msg.Type == Confirmable {
				s.sendControlLocked(Reset, msg.MessageID)
			}
		}
	}
	s.mu.Unlock()
	if doneToClose != nil {
		close(doneToClose.done)
	}
}

func validateRequest(req Message) error {
	if req.Type != Confirmable {
		return fmt.Errorf("coap: request type must be Confirmable, got %s: %w", req.Type, ErrInvalidRequest)
	}
	switch req.Code {
	case GET, POST, PUT, DELETE:
	default:
		return fmt.Errorf("coap: request code %s is not one of GET, POST, PUT, DELETE: %w", req.Code, ErrInvalidRequest)
	}
	if len(req.Token) < 1 || len(req.Token) > 8 {
		return fmt.Errorf("coap: request token length %d out of range [1, 8]: %w", len(req.Token), ErrInvalidTokenLen)
	}
	return nil
}

// snapshotRequest deep-copies a request so later mutation of the
// caller's message cannot affect retransmissions.
func snapshotRequest(req Message) Message {
	cp := Message{
		Type:      req.Type,
		Code:      req.Code,
		MessageID: req.MessageID,
	}
	cp.Token = append([]byte(nil), req.Token...)
	cp.Payload = append([]byte(nil), req.Payload...)
	if req.opts != nil {
		cp.opts = make(options, len(req.opts))
		for i, o := range req.opts {
			if b, ok := o.Value.([]byte); ok {
				o.Value = append([]byte(nil), b...)
			}
			cp.opts[i] = o
		}
	}
	return cp
}
