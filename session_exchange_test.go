package coap

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
	"time"

	"github.com/dustin/go-coap/examples/collect"
)

// ---------------------------------------------------------------------------
// Controllable clock: the retransmission/lifetime scheduling core is driven
// by advancing this clock instead of sleeping.
// ---------------------------------------------------------------------------

type fakeClock struct {
	mu     sync.Mutex
	now    time.Time
	timers []*fakeTimer
}

func newFakeClock() *fakeClock {
	return &fakeClock{now: time.Unix(1_700_000_000, 0)}
}

func (f *fakeClock) Now() time.Time {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.now
}

func (f *fakeClock) NewTimer(d time.Duration) sessionTimer {
	f.mu.Lock()
	defer f.mu.Unlock()
	t := &fakeTimer{clk: f, c: make(chan time.Time, 1), deadline: f.now.Add(d)}
	f.timers = append(f.timers, t)
	return t
}

// Advance moves the clock forward and fires every timer due by then.
func (f *fakeClock) Advance(d time.Duration) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.now = f.now.Add(d)
	remaining := f.timers[:0]
	for _, t := range f.timers {
		if t.stopped || t.deadline.After(f.now) {
			remaining = append(remaining, t)
			continue
		}
		t.stopped = true
		t.c <- f.now // buffered channel: one-shot, never blocks
	}
	f.timers = remaining
}

type fakeTimer struct {
	clk      *fakeClock
	c        chan time.Time
	deadline time.Time
	stopped  bool
}

func (t *fakeTimer) C() <-chan time.Time { return t.c }

func (t *fakeTimer) Stop() bool {
	t.clk.mu.Lock()
	defer t.clk.mu.Unlock()
	was := !t.stopped
	t.stopped = true
	return was
}

// ---------------------------------------------------------------------------
// Programmable net.PacketConn used as the datagram transport under test.
// ---------------------------------------------------------------------------

var errFakeClosed = errors.New("coap test: connection closed")

type fakeDatagram struct {
	data []byte
	addr net.Addr
}

type fakeConn struct {
	local net.Addr

	mu         sync.Mutex
	writes     []fakeDatagram
	closedFlag bool

	in        chan fakeDatagram
	readFail  chan error
	closed    chan struct{}
	closeOnce sync.Once

	// onWrite programs the device side: it is invoked synchronously
	// (from the session's send path) with every written datagram.
	onWrite func(d fakeDatagram)
}

func newFakeConn(local net.Addr) *fakeConn {
	return &fakeConn{
		local:    local,
		in:       make(chan fakeDatagram, 64),
		readFail: make(chan error, 1),
		closed:   make(chan struct{}),
	}
}

func (c *fakeConn) ReadFrom(p []byte) (int, net.Addr, error) {
	select {
	case d := <-c.in:
		return copy(p, d.data), d.addr, nil
	case err := <-c.readFail:
		return 0, nil, err
	case <-c.closed:
		return 0, nil, errFakeClosed
	}
}

func (c *fakeConn) WriteTo(p []byte, addr net.Addr) (int, error) {
	c.mu.Lock()
	if c.closedFlag {
		c.mu.Unlock()
		return 0, errFakeClosed
	}
	d := fakeDatagram{data: append([]byte(nil), p...), addr: addr}
	c.writes = append(c.writes, d)
	cb := c.onWrite
	c.mu.Unlock()
	if cb != nil {
		cb(d)
	}
	return len(p), nil
}

func (c *fakeConn) Close() error {
	c.closeOnce.Do(func() {
		c.mu.Lock()
		c.closedFlag = true
		c.mu.Unlock()
		close(c.closed)
	})
	return nil
}

func (c *fakeConn) LocalAddr() net.Addr              { return c.local }
func (c *fakeConn) SetDeadline(time.Time) error      { return nil }
func (c *fakeConn) SetReadDeadline(time.Time) error  { return nil }
func (c *fakeConn) SetWriteDeadline(time.Time) error { return nil }

func (c *fakeConn) inject(data []byte, from net.Addr) {
	c.in <- fakeDatagram{data: append([]byte(nil), data...), addr: from}
}

func (c *fakeConn) injectMsg(from net.Addr, m Message) {
	c.inject(mustWire(m), from)
}

// failReads makes the next read fail with err, simulating a broken
// connection.
func (c *fakeConn) failReads(err error) {
	c.readFail <- err
}

func (c *fakeConn) Writes() []fakeDatagram {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]fakeDatagram(nil), c.writes...)
}

func (c *fakeConn) writeCount() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.writes)
}

func (c *fakeConn) isClosed() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.closedFlag
}

// ---------------------------------------------------------------------------
// Shared test helpers
// ---------------------------------------------------------------------------

var (
	testPeerAddr     = &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 5683}
	testLocalAddr    = &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 49152}
	testStrangerAddr = &net.UDPAddr{IP: net.IPv4(10, 9, 8, 7), Port: 9999}
)

func mustWire(m Message) []byte {
	wire, err := m.MarshalBinary()
	if err != nil {
		panic(err)
	}
	return wire
}

func getRequest(mid uint16, token string, path ...string) Message {
	m := Message{Type: Confirmable, Code: GET, MessageID: mid, Token: []byte(token)}
	if len(path) > 0 {
		m.SetPath(path)
	}
	return m
}

func testConfig() SessionConfig {
	return SessionConfig{
		AckTimeout:       100 * time.Millisecond,
		MaxRetransmit:    4,
		ExchangeLifetime: 30 * time.Second,
	}
}

func newTestSession(t *testing.T, conn *fakeConn, clk *fakeClock, cfg SessionConfig) *Session {
	t.Helper()
	sess, err := newSession(conn, testPeerAddr, cfg, clk)
	if err != nil {
		t.Fatalf("newSession: %v", err)
	}
	t.Cleanup(func() { sess.Close() })
	return sess
}

func eventually(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("condition not met within 2s: %s", what)
}

func waitWrites(t *testing.T, c *fakeConn, n int) {
	t.Helper()
	eventually(t, fmt.Sprintf("%d datagrams written", n), func() bool { return c.writeCount() >= n })
}

func sessionActiveCount(s *Session) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	n := 0
	for _, ex := range s.exchanges {
		if ex.state == exchangeActive {
			n++
		}
	}
	return n
}

func sessionAcked(s *Session, mid uint16) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	ex := s.exchanges[mid]
	return ex != nil && ex.acked
}

// countType counts written datagrams of the given message type.
func countType(writes []fakeDatagram, typ COAPType) int {
	n := 0
	for _, w := range writes {
		if m, err := ParseMessage(w.data); err == nil && m.Type == typ {
			n++
		}
	}
	return n
}

type reqResult struct {
	resp *Message
	err  error
}

func requestAsync(sess *Session, ctx context.Context, req Message) chan reqResult {
	ch := make(chan reqResult, 1)
	go func() {
		resp, err := sess.Request(ctx, req)
		ch <- reqResult{resp, err}
	}()
	return ch
}

func await(t *testing.T, res chan reqResult) reqResult {
	t.Helper()
	select {
	case r := <-res:
		return r
	case <-time.After(2 * time.Second):
		t.Fatal("request did not complete in time")
		return reqResult{}
	}
}

// ---------------------------------------------------------------------------
// TestSessionCorrelation: message matching and response delivery.
// ---------------------------------------------------------------------------

func TestSessionCorrelation(t *testing.T) {
	t.Run("PiggybackResponse", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		res := requestAsync(sess, context.Background(), getRequest(0x0001, "t1", "reading"))
		waitWrites(t, conn, 1)
		want := []byte{0x42, 0x01, 0x00, 0x01, 't', '1', 0xB7, 'r', 'e', 'a', 'd', 'i', 'n', 'g'}
		if got := conn.Writes()[0].data; !bytes.Equal(got, want) {
			t.Fatalf("request wire bytes = %x, want %x", got, want)
		}

		resp := Message{
			Type:      Acknowledgement,
			Code:      Content,
			MessageID: 0x0001,
			Token:     []byte("t1"),
			Payload:   []byte("reading=42"),
		}
		resp.SetOption(ContentFormat, TextPlain)
		conn.injectMsg(testPeerAddr, resp)

		r := await(t, res)
		if r.err != nil {
			t.Fatalf("request: %v", r.err)
		}
		if r.resp.MessageID != 0x0001 {
			t.Errorf("response MID = %d, want %d", r.resp.MessageID, 0x0001)
		}
		if r.resp.Code != Content {
			t.Errorf("response code = %v, want Content", r.resp.Code)
		}
		if string(r.resp.Payload) != "reading=42" {
			t.Errorf("response payload = %q", r.resp.Payload)
		}
		if r.resp.Option(ContentFormat) != TextPlain {
			t.Errorf("content-format = %v, want %v", r.resp.Option(ContentFormat), TextPlain)
		}
		clk.Advance(30 * time.Second)
		if n := conn.writeCount(); n != 1 {
			t.Errorf("writes = %d, want 1 (no retransmission, no ACK for an ACK)", n)
		}
	})

	t.Run("SeparateResponseCON", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		res := requestAsync(sess, context.Background(), getRequest(0x0002, "t2", "reading"))
		waitWrites(t, conn, 1)

		// Empty ACK: stops retransmission only, does not deliver.
		conn.injectMsg(testPeerAddr, Message{Type: Acknowledgement, MessageID: 0x0002})
		eventually(t, "empty ACK recorded", func() bool { return sessionAcked(sess, 0x0002) })

		conn.injectMsg(testPeerAddr, Message{
			Type:      Confirmable,
			Code:      Content,
			MessageID: 50,
			Token:     []byte("t2"),
			Payload:   []byte("reading=42"),
		})
		r := await(t, res)
		if r.err != nil {
			t.Fatalf("request: %v", r.err)
		}
		if r.resp.MessageID != 50 || r.resp.Code != Content || string(r.resp.Payload) != "reading=42" {
			t.Errorf("response = mid %d code %v payload %q", r.resp.MessageID, r.resp.Code, r.resp.Payload)
		}
		// The empty ACK for the confirmable response must be on the
		// wire before the response is delivered.
		writes := conn.Writes()
		if len(writes) != 2 {
			t.Fatalf("writes = %d, want 2 (request + ACK)", len(writes))
		}
		if want := []byte{0x60, 0x00, 0x00, 0x32}; !bytes.Equal(writes[1].data, want) {
			t.Errorf("ACK for separate response = %x, want %x", writes[1].data, want)
		}
	})

	t.Run("SeparateResponseNON", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		res := requestAsync(sess, context.Background(), getRequest(0x0003, "t3", "reading"))
		waitWrites(t, conn, 1)
		conn.injectMsg(testPeerAddr, Message{Type: Acknowledgement, MessageID: 0x0003})
		eventually(t, "empty ACK recorded", func() bool { return sessionAcked(sess, 0x0003) })

		conn.injectMsg(testPeerAddr, Message{
			Type:      NonConfirmable,
			Code:      Content,
			MessageID: 51,
			Token:     []byte("t3"),
			Payload:   []byte("reading=42"),
		})
		r := await(t, res)
		if r.err != nil {
			t.Fatalf("request: %v", r.err)
		}
		if r.resp.MessageID != 51 || string(r.resp.Payload) != "reading=42" {
			t.Errorf("response = mid %d payload %q", r.resp.MessageID, r.resp.Payload)
		}
		if n := conn.writeCount(); n != 1 {
			t.Errorf("writes = %d, want 1 (a NON response is not ACKed)", n)
		}
	})

	t.Run("ReverseOrderResponses", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		resA := requestAsync(sess, context.Background(), getRequest(0x000A, "aa", "first"))
		resB := requestAsync(sess, context.Background(), getRequest(0x000B, "bb", "second"))
		waitWrites(t, conn, 2)

		// Answer B first (piggyback), then A (separate CON).
		conn.injectMsg(testPeerAddr, Message{
			Type:      Acknowledgement,
			Code:      Content,
			MessageID: 0x000B,
			Token:     []byte("bb"),
			Payload:   []byte("B"),
		})
		rb := await(t, resB)
		if rb.err != nil || string(rb.resp.Payload) != "B" || rb.resp.MessageID != 0x000B {
			t.Errorf("request B = %+v, want payload B", rb)
		}

		conn.injectMsg(testPeerAddr, Message{Type: Acknowledgement, MessageID: 0x000A})
		eventually(t, "empty ACK for A", func() bool { return sessionAcked(sess, 0x000A) })
		conn.injectMsg(testPeerAddr, Message{
			Type:      Confirmable,
			Code:      Content,
			MessageID: 77,
			Token:     []byte("aa"),
			Payload:   []byte("A"),
		})
		ra := await(t, resA)
		if ra.err != nil || string(ra.resp.Payload) != "A" || ra.resp.MessageID != 77 {
			t.Errorf("request A = %+v, want payload A mid 77", ra)
		}

		writes := conn.Writes()
		if len(writes) != 3 {
			t.Fatalf("writes = %d, want 3 (2 requests + 1 ACK)", len(writes))
		}
		if want := []byte{0x60, 0x00, 0x00, 0x4D}; !bytes.Equal(writes[2].data, want) {
			t.Errorf("ACK for A's separate response = %x, want %x", writes[2].data, want)
		}
	})

	t.Run("LateSeparateConAndDuplicate", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		res := requestAsync(sess, context.Background(), getRequest(0x000C, "lc", "reading"))
		waitWrites(t, conn, 1)
		conn.injectMsg(testPeerAddr, Message{Type: Acknowledgement, MessageID: 0x000C})
		eventually(t, "empty ACK recorded", func() bool { return sessionAcked(sess, 0x000C) })

		// Well past the first retransmission deadline: the empty ACK
		// stopped retransmission, the request keeps waiting.
		clk.Advance(250 * time.Millisecond)
		if n := conn.writeCount(); n != 1 {
			t.Fatalf("writes = %d, want 1 (retransmission after empty ACK)", n)
		}

		// The separate response arrives late, as a CON with its own MID.
		conn.injectMsg(testPeerAddr, Message{
			Type:      Confirmable,
			Code:      Content,
			MessageID: 88,
			Token:     []byte("lc"),
			Payload:   []byte("late"),
		})
		r := await(t, res)
		if r.err != nil || r.resp.MessageID != 88 || string(r.resp.Payload) != "late" {
			t.Errorf("response = %+v, want mid 88 payload late", r)
		}
		waitWrites(t, conn, 2)
		if want := []byte{0x60, 0x00, 0x00, 0x58}; !bytes.Equal(conn.Writes()[1].data, want) {
			t.Errorf("ACK for late CON = %x, want %x", conn.Writes()[1].data, want)
		}

		// The peer retransmits the CON: acknowledged again with the
		// same MID, but not delivered twice.
		conn.injectMsg(testPeerAddr, Message{
			Type:      Confirmable,
			Code:      Content,
			MessageID: 88,
			Token:     []byte("lc"),
			Payload:   []byte("late"),
		})
		waitWrites(t, conn, 3)
		if want := []byte{0x60, 0x00, 0x00, 0x58}; !bytes.Equal(conn.Writes()[2].data, want) {
			t.Errorf("ACK for duplicate CON = %x, want %x", conn.Writes()[2].data, want)
		}
		clk.Advance(time.Second)
		if n := conn.writeCount(); n != 3 {
			t.Errorf("writes = %d, want 3 (request + 2 ACKs)", n)
		}
	})

	t.Run("UnknownAndMalformedIgnored", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		res := requestAsync(sess, context.Background(), getRequest(0x0014, "uk", "x"))
		waitWrites(t, conn, 1)

		conn.inject([]byte{0xff, 0x00, 0x01}, testPeerAddr) // malformed: bad version
		// Valid response from the wrong source address.
		conn.injectMsg(testStrangerAddr, Message{
			Type: Acknowledgement, Code: Content, MessageID: 0x0014,
			Token: []byte("uk"), Payload: []byte("intruder"),
		})
		// Empty ACK for an unknown message ID.
		conn.injectMsg(testPeerAddr, Message{Type: Acknowledgement, MessageID: 0x0999})
		// Unknown confirmable response: rejected with a Reset.
		conn.injectMsg(testPeerAddr, Message{
			Type: Confirmable, Code: Content, MessageID: 777,
			Token: []byte("stranger"), Payload: []byte("x"),
		})
		waitWrites(t, conn, 2)
		if want := []byte{0x70, 0x00, 0x03, 0x09}; !bytes.Equal(conn.Writes()[1].data, want) {
			t.Fatalf("reset for unknown CON = %x, want %x", conn.Writes()[1].data, want)
		}
		// Unknown non-confirmable response: ignored.
		conn.injectMsg(testPeerAddr, Message{
			Type: NonConfirmable, Code: Content, MessageID: 778, Token: []byte("stranger"),
		})
		// Piggyback with the right MID but an unrelated token: ignored.
		conn.injectMsg(testPeerAddr, Message{
			Type: Acknowledgement, Code: Content, MessageID: 0x0014,
			Token: []byte("nope"), Payload: []byte("wrong"),
		})

		// None of the above completed or acknowledged the request: the
		// retransmission schedule is still running.
		clk.Advance(150 * time.Millisecond)
		waitWrites(t, conn, 3)
		if !bytes.Equal(conn.Writes()[2].data, conn.Writes()[0].data) {
			t.Errorf("retransmission = %x, want identical to %x", conn.Writes()[2].data, conn.Writes()[0].data)
		}

		// The genuine answer still completes the request.
		conn.injectMsg(testPeerAddr, Message{Type: Acknowledgement, MessageID: 0x0014})
		eventually(t, "empty ACK recorded", func() bool { return sessionAcked(sess, 0x0014) })
		conn.injectMsg(testPeerAddr, Message{
			Type: NonConfirmable, Code: Content, MessageID: 800,
			Token: []byte("uk"), Payload: []byte("reading=42"),
		})
		r := await(t, res)
		if r.err != nil || r.resp.MessageID != 800 || string(r.resp.Payload) != "reading=42" {
			t.Errorf("response = %+v, want mid 800 payload reading=42", r)
		}
	})

	t.Run("ResetMatchedByMID", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		res := requestAsync(sess, context.Background(), getRequest(0x0019, "rs", "reading"))
		waitWrites(t, conn, 1)

		// A Reset for some other message is ignored.
		conn.injectMsg(testPeerAddr, Message{Type: Reset, MessageID: 0x0999})
		clk.Advance(150 * time.Millisecond)
		waitWrites(t, conn, 2) // retransmission proves the request is still active

		conn.injectMsg(testPeerAddr, Message{Type: Reset, MessageID: 0x0019})
		r := await(t, res)
		if !errors.Is(r.err, ErrReset) {
			t.Fatalf("request error = %v, want ErrReset", r.err)
		}
		if r.resp != nil {
			t.Errorf("response = %v, want nil", r.resp)
		}
	})

	t.Run("ErrorResponseIsBusiness", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		res := requestAsync(sess, context.Background(), getRequest(0x001E, "nf", "missing"))
		waitWrites(t, conn, 1)
		conn.injectMsg(testPeerAddr, Message{
			Type: Acknowledgement, Code: NotFound, MessageID: 0x001E, Token: []byte("nf"),
		})
		r := await(t, res)
		if r.err != nil || r.resp.Code != NotFound {
			t.Errorf("4.xx response = %+v, want NotFound without error", r)
		}

		res2 := requestAsync(sess, context.Background(), getRequest(0x001F, "ue", "broken"))
		waitWrites(t, conn, 2)
		conn.injectMsg(testPeerAddr, Message{Type: Acknowledgement, MessageID: 0x001F})
		eventually(t, "empty ACK recorded", func() bool { return sessionAcked(sess, 0x001F) })
		conn.injectMsg(testPeerAddr, Message{
			Type: NonConfirmable, Code: ServiceUnavailable, MessageID: 90, Token: []byte("ue"),
		})
		r2 := await(t, res2)
		if r2.err != nil || r2.resp.Code != ServiceUnavailable || r2.resp.MessageID != 90 {
			t.Errorf("5.xx response = %+v, want ServiceUnavailable mid 90 without error", r2)
		}
	})

	// Combined check: real session requests drive the collect pipeline
	// (response parsing, dedup, JSON Lines file readback) while the
	// protocol bytes and clock boundaries are verified as usual.
	t.Run("CollectJSONL", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)

		voltDropped := 0
		conn.onWrite = func(d fakeDatagram) {
			msg, err := ParseMessage(d.data)
			if err != nil || msg.Type != Confirmable || msg.Code != GET {
				return // session control messages (ACKs) need no answer
			}
			switch msg.PathString() {
			case "volt":
				// Drop the first volt request to force a
				// retransmission; answer everything after.
				if voltDropped < 1 {
					voltDropped++
					return
				}
				conn.injectMsg(testPeerAddr, Message{
					Type: Acknowledgement, Code: Content, MessageID: msg.MessageID,
					Token: msg.Token, Payload: []byte("sample_id=v-1;value=230.5"),
				})
			case "curr":
				conn.injectMsg(testPeerAddr, Message{Type: Acknowledgement, MessageID: msg.MessageID})
				// The same confirmable separate response twice.
				for i := 0; i < 2; i++ {
					conn.injectMsg(testPeerAddr, Message{
						Type: Confirmable, Code: Content, MessageID: 777,
						Token: msg.Token, Payload: []byte("sample_id=c-1;value=4.2"),
					})
				}
			case "legacy":
				conn.injectMsg(testPeerAddr, Message{
					Type: Acknowledgement, Code: NotFound, MessageID: msg.MessageID, Token: msg.Token,
				})
			case "noisy":
				conn.injectMsg(testPeerAddr, Message{
					Type: Acknowledgement, Code: Content, MessageID: msg.MessageID,
					Token: msg.Token, Payload: []byte("not-a-reading"),
				})
			case "gone":
				// Never answered; the test cancels it.
			}
		}
		sess := newTestSession(t, conn, clk, SessionConfig{
			AckTimeout:       100 * time.Millisecond,
			MaxRetransmit:    2,
			ExchangeLifetime: 10 * time.Second,
		})

		goneCtx, cancelGone := context.WithCancel(context.Background())
		var reqID uint32
		querier := collect.QuerierFunc(func(ctx context.Context, resource string) (collect.Response, error) {
			qctx := ctx
			if resource == "gone" {
				qctx = goneCtx
			}
			id := atomic.AddUint32(&reqID, 1)
			req := Message{
				Type:      Confirmable,
				Code:      GET,
				MessageID: uint16(300 + id),
				Token:     []byte(fmt.Sprintf("tok-%d", id)),
			}
			req.SetPathString(resource)
			resp, err := sess.Request(qctx, req)
			if err != nil {
				return collect.Response{}, err
			}
			return collect.Response{Code: int(resp.Code), Payload: resp.Payload}, nil
		})

		resources := []string{"volt", "volt", "curr", "legacy", "noisy", "gone"}
		resultCh := make(chan *collect.Result, 1)
		go func() { resultCh <- collect.Collect(context.Background(), querier, resources) }()

		waitWrites(t, conn, 6) // every resource queried once
		cancelGone()           // one resource is cancelled mid-flight
		eventually(t, "only the dropped volt exchange remains active",
			func() bool { return sessionActiveCount(sess) == 1 })
		// Clock boundary: before the clock reaches AckTimeout there
		// are exactly the 6 initial requests on the wire (any further
		// writes are the session's ACKs for curr's CON responses).
		if n := countType(conn.Writes(), Confirmable); n != 6 {
			t.Fatalf("requests = %d before the clock advances, want 6 (no early retransmission)", n)
		}

		clk.Advance(100 * time.Millisecond) // AckTimeout: volt retransmits
		waitWrites(t, conn, 7)

		var result *collect.Result
		select {
		case result = <-resultCh:
		case <-time.After(2 * time.Second):
			t.Fatal("collect did not finish")
		}

		wantRecords := []collect.Record{
			{Resource: "curr", SampleID: "c-1", Value: 4.2},
			{Resource: "volt", SampleID: "v-1", Value: 230.5},
		}
		if !reflect.DeepEqual(result.Records, wantRecords) {
			t.Errorf("records = %+v, want %+v", result.Records, wantRecords)
		}
		if result.Duplicates != 1 {
			t.Errorf("duplicates = %d, want 1 (second volt reading dropped)", result.Duplicates)
		}
		if len(result.Failures) != 3 {
			t.Fatalf("failures = %+v, want 3", result.Failures)
		}
		failByResource := map[string]string{}
		for _, f := range result.Failures {
			failByResource[f.Resource] = f.Reason
		}
		if !strings.Contains(failByResource["gone"], "context canceled") {
			t.Errorf("gone failure = %q, want cancellation", failByResource["gone"])
		}
		if !strings.Contains(failByResource["legacy"], "4.04") {
			t.Errorf("legacy failure = %q, want 4.04", failByResource["legacy"])
		}
		if !strings.Contains(failByResource["noisy"], "malformed") {
			t.Errorf("noisy failure = %q, want malformed payload", failByResource["noisy"])
		}
		t.Logf("final records: %+v", result.Records)
		t.Logf("failure list: %+v", result.Failures)

		// JSON Lines file: write, read back, compare.
		path := filepath.Join(t.TempDir(), "readings.jsonl")
		if err := collect.WriteJSONL(path, result.Records); err != nil {
			t.Fatalf("WriteJSONL: %v", err)
		}
		back, err := collect.ReadJSONL(path)
		if err != nil {
			t.Fatalf("ReadJSONL: %v", err)
		}
		if !reflect.DeepEqual(back, wantRecords) {
			t.Errorf("read back = %+v, want %+v", back, wantRecords)
		}
		raw, err := os.ReadFile(path)
		if err != nil {
			t.Fatalf("read file: %v", err)
		}
		lines := strings.Split(strings.TrimRight(string(raw), "\n"), "\n")
		if len(lines) != 2 {
			t.Fatalf("jsonl lines = %d, want 2 (%q)", len(lines), raw)
		}
		for _, line := range lines {
			var obj map[string]interface{}
			if err := json.Unmarshal([]byte(line), &obj); err != nil {
				t.Fatalf("line %q: %v", line, err)
			}
			for _, key := range []string{"resource", "sample_id", "value"} {
				if _, ok := obj[key]; !ok {
					t.Errorf("line %q missing key %q", line, key)
				}
			}
		}

		// Protocol checks: 7 requests + 2 ACKs for the duplicated CON.
		eventually(t, "all 9 datagrams written", func() bool { return conn.writeCount() == 9 })
		writes := conn.Writes()
		var reqs, acks [][]byte
		voltByToken := map[string][]byte{}
		for _, w := range writes {
			m, err := ParseMessage(w.data)
			if err != nil {
				t.Fatalf("unparsable written datagram %x: %v", w.data, err)
			}
			switch m.Type {
			case Confirmable:
				reqs = append(reqs, w.data)
				if m.PathString() == "volt" {
					tok := string(m.Token)
					if first, ok := voltByToken[tok]; ok {
						if !bytes.Equal(first, w.data) {
							t.Errorf("volt retransmission = %x, want identical to %x", w.data, first)
						}
					} else {
						voltByToken[tok] = w.data
					}
				}
			case Acknowledgement:
				acks = append(acks, w.data)
			default:
				t.Errorf("unexpected session datagram type %v", m.Type)
			}
		}
		if len(reqs) != 7 {
			t.Errorf("requests sent = %d, want 7 (6 + 1 retransmission)", len(reqs))
		}
		if len(voltByToken) != 2 {
			t.Errorf("distinct volt tokens = %d, want 2", len(voltByToken))
		}
		if len(acks) != 2 {
			t.Fatalf("ACKs sent = %d, want 2 (duplicated CON)", len(acks))
		}
		for _, a := range acks {
			if want := []byte{0x60, 0x00, 0x03, 0x09}; !bytes.Equal(a, want) {
				t.Errorf("ACK = %x, want %x (mid 777)", a, want)
			}
		}
	})
}

// ---------------------------------------------------------------------------
// TestSessionRetransmit: retransmission schedule, boundaries, config.
// ---------------------------------------------------------------------------

func TestSessionRetransmit(t *testing.T) {
	t.Run("Schedule", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		var sendTimes []time.Time
		conn.onWrite = func(d fakeDatagram) {
			sendTimes = append(sendTimes, clk.Now())
		}
		sess := newTestSession(t, conn, clk, SessionConfig{
			AckTimeout:       100 * time.Millisecond,
			MaxRetransmit:    2,
			ExchangeLifetime: 10 * time.Second,
		})

		res := requestAsync(sess, context.Background(), getRequest(0x0100, "sched"))
		waitWrites(t, conn, 1)
		clk.Advance(100 * time.Millisecond)
		waitWrites(t, conn, 2)
		clk.Advance(200 * time.Millisecond)
		waitWrites(t, conn, 3)
		clk.Advance(400 * time.Millisecond) // final wait exhausted

		r := await(t, res)
		if !errors.Is(r.err, ErrTimeout) {
			t.Fatalf("request error = %v, want ErrTimeout", r.err)
		}
		if len(sendTimes) != 3 {
			t.Fatalf("sends = %d, want 3 (1 + MaxRetransmit)", len(sendTimes))
		}
		base := sendTimes[0]
		for i, want := range []time.Duration{0, 100 * time.Millisecond, 300 * time.Millisecond} {
			if got := sendTimes[i].Sub(base); got != want {
				t.Errorf("send %d at +%v, want +%v", i, got, want)
			}
		}
		writes := conn.Writes()
		for i := 1; i < len(writes); i++ {
			if !bytes.Equal(writes[i].data, writes[0].data) {
				t.Errorf("retransmission %d = %x, want identical to %x", i, writes[i].data, writes[0].data)
			}
		}
	})

	t.Run("StopsOnEmptyAck", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		res := requestAsync(sess, context.Background(), getRequest(0x0101, "ack"))
		waitWrites(t, conn, 1)
		conn.injectMsg(testPeerAddr, Message{Type: Acknowledgement, MessageID: 0x0101})
		eventually(t, "empty ACK recorded", func() bool { return sessionAcked(sess, 0x0101) })

		clk.Advance(5 * time.Second) // many retransmission windows pass
		if n := conn.writeCount(); n != 1 {
			t.Fatalf("writes = %d, want 1 (empty ACK stopped retransmission)", n)
		}

		conn.injectMsg(testPeerAddr, Message{
			Type: NonConfirmable, Code: Content, MessageID: 600,
			Token: []byte("ack"), Payload: []byte("reading=42"),
		})
		r := await(t, res)
		if r.err != nil || string(r.resp.Payload) != "reading=42" {
			t.Errorf("response = %+v, want reading=42", r)
		}
		clk.Advance(30 * time.Second)
		if n := conn.writeCount(); n != 1 {
			t.Errorf("writes = %d, want 1", n)
		}
	})

	t.Run("StopsOnPiggyback", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		res := requestAsync(sess, context.Background(), getRequest(0x0102, "pg"))
		waitWrites(t, conn, 1)
		clk.Advance(100 * time.Millisecond)
		waitWrites(t, conn, 2) // one retransmission happened
		conn.injectMsg(testPeerAddr, Message{
			Type: Acknowledgement, Code: Content, MessageID: 0x0102,
			Token: []byte("pg"), Payload: []byte("reading=42"),
		})
		r := await(t, res)
		if r.err != nil || string(r.resp.Payload) != "reading=42" {
			t.Errorf("response = %+v, want reading=42", r)
		}
		clk.Advance(30 * time.Second)
		if n := conn.writeCount(); n != 2 {
			t.Errorf("writes = %d, want 2 (no retransmission after delivery)", n)
		}
	})

	t.Run("CancelAtRetransmitBoundary", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		ctx, cancel := context.WithCancel(context.Background())
		res := requestAsync(sess, ctx, getRequest(0x0103, "cx"))
		waitWrites(t, conn, 1)

		cancel()                            // cancel exactly when the retransmission is due
		clk.Advance(100 * time.Millisecond) // fires the retransmit timer

		r := await(t, res)
		if !errors.Is(r.err, context.Canceled) {
			t.Fatalf("request error = %v, want context.Canceled", r.err)
		}
		if n := conn.writeCount(); n != 1 {
			t.Errorf("writes = %d, want 1 (cancelled request not revived by the timer)", n)
		}
		clk.Advance(30 * time.Second)
		if n := conn.writeCount(); n != 1 {
			t.Errorf("writes = %d after lifetime, want 1", n)
		}
	})

	t.Run("CompletedNotRevived", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		res := requestAsync(sess, context.Background(), getRequest(0x0104, "dn"))
		waitWrites(t, conn, 1)
		conn.injectMsg(testPeerAddr, Message{
			Type: Acknowledgement, Code: Content, MessageID: 0x0104,
			Token: []byte("dn"), Payload: []byte("reading=42"),
		})
		if r := await(t, res); r.err != nil {
			t.Fatalf("request: %v", r.err)
		}
		clk.Advance(30 * time.Second)
		if n := conn.writeCount(); n != 1 {
			t.Errorf("writes = %d, want 1 (completed request not revived)", n)
		}
	})

	t.Run("LifetimeCapsWaitAfterEmptyAck", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, SessionConfig{
			AckTimeout:       100 * time.Millisecond,
			MaxRetransmit:    5,
			ExchangeLifetime: 300 * time.Millisecond,
		})

		res := requestAsync(sess, context.Background(), getRequest(0x0105, "lt"))
		waitWrites(t, conn, 1)
		conn.injectMsg(testPeerAddr, Message{Type: Acknowledgement, MessageID: 0x0105})
		eventually(t, "empty ACK recorded", func() bool { return sessionAcked(sess, 0x0105) })

		clk.Advance(300 * time.Millisecond) // exchange lifetime from first send
		r := await(t, res)
		if !errors.Is(r.err, ErrTimeout) {
			t.Fatalf("request error = %v, want ErrTimeout", r.err)
		}
		if n := conn.writeCount(); n != 1 {
			t.Errorf("writes = %d, want 1 (acked, so no retransmissions)", n)
		}
	})

	t.Run("LifetimeCapsUnackedExchange", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, SessionConfig{
			AckTimeout:       100 * time.Millisecond,
			MaxRetransmit:    5,
			ExchangeLifetime: 250 * time.Millisecond,
		})

		res := requestAsync(sess, context.Background(), getRequest(0x0106, "lu"))
		waitWrites(t, conn, 1)
		clk.Advance(100 * time.Millisecond)
		waitWrites(t, conn, 2) // retransmission at +100ms
		clk.Advance(150 * time.Millisecond)
		// Lifetime (250ms) is reached before the next retransmission
		// would be due (300ms).
		r := await(t, res)
		if !errors.Is(r.err, ErrTimeout) {
			t.Fatalf("request error = %v, want ErrTimeout", r.err)
		}
		if n := conn.writeCount(); n != 2 {
			t.Errorf("writes = %d, want 2 (no send at or after the lifetime boundary)", n)
		}
	})

	t.Run("SnapshotRetransmission", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		req := Message{
			Type:      Confirmable,
			Code:      POST,
			MessageID: 0x0107,
			Token:     []byte("snap"),
			Payload:   []byte("pay1"),
		}
		req.SetPathString("reading")
		res := requestAsync(sess, context.Background(), req)
		waitWrites(t, conn, 1)
		first := append([]byte(nil), conn.Writes()[0].data...)

		// Mutating the caller's message after acceptance must not
		// affect what is retransmitted.
		req.Payload[0] = 'X'
		req.Token[0] = 'X'
		req.SetPathString("mutated")

		clk.Advance(100 * time.Millisecond)
		waitWrites(t, conn, 2)
		if got := conn.Writes()[1].data; !bytes.Equal(got, first) {
			t.Errorf("retransmission = %x, want the original snapshot %x", got, first)
		}

		conn.injectMsg(testPeerAddr, Message{
			Type: NonConfirmable, Code: Content, MessageID: 900,
			Token: []byte("snap"), Payload: []byte("ok"),
		})
		if r := await(t, res); r.err != nil {
			t.Fatalf("request: %v", r.err)
		}
	})

	t.Run("ConfigValidation", func(t *testing.T) {
		if _, err := NewSession(nil, testPeerAddr, SessionConfig{}); err == nil {
			t.Error("nil packet connection: expected error")
		}
		if _, err := NewSession(newFakeConn(testLocalAddr), nil, SessionConfig{}); err == nil {
			t.Error("nil peer: expected error")
		}

		def, err := NewSession(newFakeConn(testLocalAddr), testPeerAddr, SessionConfig{})
		if err != nil {
			t.Fatalf("zero config: %v", err)
		}
		if def.cfg.AckTimeout != DefaultAckTimeout ||
			def.cfg.MaxRetransmit != DefaultMaxRetransmit ||
			def.cfg.ExchangeLifetime != DefaultExchangeLifetime {
			t.Errorf("zero config resolved to %+v, want the library defaults", def.cfg)
		}
		def.Close()

		bad := []SessionConfig{
			{AckTimeout: -time.Second, MaxRetransmit: 1, ExchangeLifetime: time.Second},
			{AckTimeout: time.Second, MaxRetransmit: 1, ExchangeLifetime: 0},
			{AckTimeout: time.Second, MaxRetransmit: -1, ExchangeLifetime: time.Second},
		}
		for i, cfg := range bad {
			if _, err := NewSession(newFakeConn(testLocalAddr), testPeerAddr, cfg); err == nil {
				t.Errorf("config %d (%+v): expected error", i, cfg)
			}
		}

		ok, err := NewSession(newFakeConn(testLocalAddr), testPeerAddr, SessionConfig{
			AckTimeout:       time.Second,
			MaxRetransmit:    0,
			ExchangeLifetime: time.Second,
		})
		if err != nil {
			t.Errorf("valid explicit config: %v", err)
		} else {
			ok.Close()
		}
	})
}

// ---------------------------------------------------------------------------
// TestSessionRetirement: identity retention, close, connection failure.
// ---------------------------------------------------------------------------

func TestSessionRetirement(t *testing.T) {
	t.Run("IdentityReuse", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, SessionConfig{
			AckTimeout:       500 * time.Millisecond,
			MaxRetransmit:    1,
			ExchangeLifetime: time.Second,
		})

		res := requestAsync(sess, context.Background(), getRequest(0x0064, "id1"))
		waitWrites(t, conn, 1)
		clk.Advance(400 * time.Millisecond)
		conn.injectMsg(testPeerAddr, Message{
			Type: Acknowledgement, Code: Content, MessageID: 0x0064,
			Token: []byte("id1"), Payload: []byte("v"),
		})
		if r := await(t, res); r.err != nil {
			t.Fatalf("request: %v", r.err)
		}

		// The exchange finished at t=400ms, but its identity is
		// retained until the ExchangeLifetime from the first send
		// (t=1000ms).
		if _, err := sess.Request(context.Background(), getRequest(0x0064, "zz")); !errors.Is(err, ErrMessageIDInUse) {
			t.Errorf("mid reuse at t=400: %v, want ErrMessageIDInUse", err)
		}
		if _, err := sess.Request(context.Background(), getRequest(0x0065, "id1")); !errors.Is(err, ErrTokenInUse) {
			t.Errorf("token reuse at t=400: %v, want ErrTokenInUse", err)
		}
		clk.Advance(599 * time.Millisecond) // t=999
		if _, err := sess.Request(context.Background(), getRequest(0x0064, "zz")); !errors.Is(err, ErrMessageIDInUse) {
			t.Errorf("mid reuse at t=999: %v, want ErrMessageIDInUse", err)
		}
		if _, err := sess.Request(context.Background(), getRequest(0x0065, "id1")); !errors.Is(err, ErrTokenInUse) {
			t.Errorf("token reuse at t=999: %v, want ErrTokenInUse", err)
		}
		clk.Advance(time.Millisecond) // t=1000: lifetime from the first send reached
		res2 := requestAsync(sess, context.Background(), getRequest(0x0064, "id1"))
		waitWrites(t, conn, 2)
		conn.injectMsg(testPeerAddr, Message{
			Type: Acknowledgement, Code: Content, MessageID: 0x0064,
			Token: []byte("id1"), Payload: []byte("v2"),
		})
		if r := await(t, res2); r.err != nil || string(r.resp.Payload) != "v2" {
			t.Errorf("reused identity request = %+v, want payload v2", r)
		}
	})

	t.Run("RetiredDuplicateConThenResetAfterExpiry", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, SessionConfig{
			AckTimeout:       500 * time.Millisecond,
			MaxRetransmit:    1,
			ExchangeLifetime: time.Second,
		})

		res := requestAsync(sess, context.Background(), getRequest(0x006E, "dup"))
		waitWrites(t, conn, 1)
		conn.injectMsg(testPeerAddr, Message{Type: Acknowledgement, MessageID: 0x006E})
		eventually(t, "empty ACK recorded", func() bool { return sessionAcked(sess, 0x006E) })
		conn.injectMsg(testPeerAddr, Message{
			Type: Confirmable, Code: Content, MessageID: 900,
			Token: []byte("dup"), Payload: []byte("v"),
		})
		if r := await(t, res); r.err != nil || string(r.resp.Payload) != "v" {
			t.Fatalf("request = %+v, want payload v", r)
		}
		waitWrites(t, conn, 2)
		if want := []byte{0x60, 0x00, 0x03, 0x84}; !bytes.Equal(conn.Writes()[1].data, want) {
			t.Fatalf("ACK = %x, want %x", conn.Writes()[1].data, want)
		}

		// Within the retention window the duplicate is acknowledged
		// again with the same MID, without being delivered twice.
		clk.Advance(400 * time.Millisecond)
		conn.injectMsg(testPeerAddr, Message{
			Type: Confirmable, Code: Content, MessageID: 900,
			Token: []byte("dup"), Payload: []byte("v"),
		})
		waitWrites(t, conn, 3)
		if want := []byte{0x60, 0x00, 0x03, 0x84}; !bytes.Equal(conn.Writes()[2].data, want) {
			t.Errorf("ACK for duplicate = %x, want %x", conn.Writes()[2].data, want)
		}

		// After the exchange lifetime the record is gone: the same
		// message is unknown and gets a Reset.
		clk.Advance(600 * time.Millisecond)
		conn.injectMsg(testPeerAddr, Message{
			Type: Confirmable, Code: Content, MessageID: 900,
			Token: []byte("dup"), Payload: []byte("v"),
		})
		waitWrites(t, conn, 4)
		if want := []byte{0x70, 0x00, 0x03, 0x84}; !bytes.Equal(conn.Writes()[3].data, want) {
			t.Errorf("message after expiry = %x, want Reset %x", conn.Writes()[3].data, want)
		}
	})

	t.Run("CancelledIdentityRetained", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, SessionConfig{
			AckTimeout:       500 * time.Millisecond,
			MaxRetransmit:    1,
			ExchangeLifetime: time.Second,
		})

		ctx, cancel := context.WithCancel(context.Background())
		res := requestAsync(sess, ctx, getRequest(0x0078, "cx"))
		waitWrites(t, conn, 1)
		cancel()
		if r := await(t, res); !errors.Is(r.err, context.Canceled) {
			t.Fatalf("request error = %v, want context.Canceled", r.err)
		}

		if _, err := sess.Request(context.Background(), getRequest(0x0078, "zz")); !errors.Is(err, ErrMessageIDInUse) {
			t.Errorf("mid reuse: %v, want ErrMessageIDInUse", err)
		}
		if _, err := sess.Request(context.Background(), getRequest(0x0079, "cx")); !errors.Is(err, ErrTokenInUse) {
			t.Errorf("token reuse: %v, want ErrTokenInUse", err)
		}
		clk.Advance(time.Second)
		res2 := requestAsync(sess, context.Background(), getRequest(0x0078, "cx"))
		waitWrites(t, conn, 2)
		conn.injectMsg(testPeerAddr, Message{
			Type: Acknowledgement, Code: Content, MessageID: 0x0078,
			Token: []byte("cx"), Payload: []byte("v"),
		})
		if r := await(t, res2); r.err != nil {
			t.Errorf("reused identity request: %v", r.err)
		}
	})

	t.Run("CloseEndsWaiting", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		res := requestAsync(sess, context.Background(), getRequest(0x0082, "cl"))
		waitWrites(t, conn, 1)
		if err := sess.Close(); err != nil {
			t.Errorf("Close: %v", err)
		}
		if r := await(t, res); !errors.Is(r.err, ErrSessionClosed) {
			t.Errorf("waiting request error = %v, want ErrSessionClosed", r.err)
		}
		if !conn.isClosed() {
			t.Error("owned connection was not closed")
		}
		if err := sess.Close(); err != nil {
			t.Errorf("repeated Close: %v", err)
		}
		if _, err := sess.Request(context.Background(), getRequest(0x0083, "cl2")); !errors.Is(err, ErrSessionClosed) {
			t.Errorf("request after Close = %v, want ErrSessionClosed", err)
		}
	})

	t.Run("ReadFailurePropagates", func(t *testing.T) {
		clk := newFakeClock()
		conn := newFakeConn(testLocalAddr)
		sess := newTestSession(t, conn, clk, testConfig())

		resA := requestAsync(sess, context.Background(), getRequest(0x008C, "ra"))
		resB := requestAsync(sess, context.Background(), getRequest(0x008D, "rb"))
		waitWrites(t, conn, 2)

		boom := errors.New("boom")
		conn.failReads(boom)
		for i, res := range []chan reqResult{resA, resB} {
			if r := await(t, res); !errors.Is(r.err, boom) {
				t.Errorf("waiting request %d error = %v, want the original %v", i, r.err, boom)
			}
		}
		if _, err := sess.Request(context.Background(), getRequest(0x008E, "rc")); !errors.Is(err, boom) {
			t.Errorf("request after read failure = %v, want the original %v", err, boom)
		}
	})
}

// ---------------------------------------------------------------------------
// Loopback UDP round trip over a real socket (skipped where the sandbox
// forbids binding UDP sockets).
// ---------------------------------------------------------------------------

func TestSessionUDPLoopback(t *testing.T) {
	srv, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		if errors.Is(err, syscall.EPERM) || errors.Is(err, os.ErrPermission) {
			t.Skipf("UDP loopback unavailable in this environment: %v", err)
		}
		t.Fatalf("listen: %v", err)
	}
	defer srv.Close()

	// Device side: empty ACK first, then the reading as a separate
	// NON message with its own MID (mirrors task_support/baseline).
	done := make(chan error, 1)
	go func() {
		buf := make([]byte, maxPktLen)
		srv.SetReadDeadline(time.Now().Add(3 * time.Second))
		n, addr, err := srv.ReadFromUDP(buf)
		if err != nil {
			done <- err
			return
		}
		q, err := ParseMessage(buf[:n])
		if err != nil {
			done <- err
			return
		}
		if err := Transmit(srv, addr, Message{Type: Acknowledgement, MessageID: q.MessageID}); err != nil {
			done <- err
			return
		}
		done <- Transmit(srv, addr, Message{
			Type:      NonConfirmable,
			Code:      Content,
			MessageID: 91,
			Token:     q.Token,
			Payload:   []byte("reading=42"),
		})
	}()

	pc, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		if errors.Is(err, syscall.EPERM) || errors.Is(err, os.ErrPermission) {
			t.Skipf("UDP loopback unavailable in this environment: %v", err)
		}
		t.Fatalf("listen: %v", err)
	}
	sess, err := NewSession(pc, srv.LocalAddr(), SessionConfig{
		AckTimeout:       300 * time.Millisecond,
		MaxRetransmit:    2,
		ExchangeLifetime: 5 * time.Second,
	})
	if err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	defer sess.Close()

	resp, err := sess.Request(context.Background(), getRequest(7, "t", "reading"))
	if err != nil {
		t.Fatalf("Request: %v", err)
	}
	if resp.Code != Content || resp.MessageID != 91 || string(resp.Payload) != "reading=42" {
		t.Errorf("response = code %v mid %d payload %q, want Content 91 reading=42",
			resp.Code, resp.MessageID, resp.Payload)
	}
	if err := <-done; err != nil {
		t.Fatalf("device: %v", err)
	}
}
