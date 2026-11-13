package ftp

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"net"
	"net/textproto"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/jlaffaye/ftp/support/unitcheck"
)

// fxpStep is one turn of a scripted FTP control peer.
//
// When want is set the step first reads a command line and checks it; when
// gate is also set the read is performed before the gate, so the step doubles
// as a synchronization point for a reply that must never be sent.
type fxpStep struct {
	want       string          // exact expected command line, "" = unsolicited reply
	lines      []string        // reply lines, one FTP reply per line
	gate       <-chan struct{} // wait before sending the reply
	signal     chan<- struct{} // signal after the reply was sent
	closeAfter bool            // close the control connection after reading want
}

// asyncWriteConn keeps net.Pipe as the transport but makes writes buffered,
// mirroring the full-duplex buffering of a real TCP socket: a reply the server
// sends while the client is itself writing (e.g. a queued 226 before ABOR)
// must not deadlock the two unbuffered pipe writes. Reads stay synchronous so
// blocking control-reply waits and cancellation are still exercised.
type asyncWriteConn struct {
	net.Conn
	writes chan []byte
	once   sync.Once
	closed chan struct{}
}

func newAsyncWriteConn(inner net.Conn) *asyncWriteConn {
	c := &asyncWriteConn{
		Conn:   inner,
		writes: make(chan []byte, 256),
		closed: make(chan struct{}),
	}
	go func() {
		for {
			select {
			case b := <-c.writes:
				if _, err := inner.Write(b); err != nil {
					return
				}
			case <-c.closed:
				return
			}
		}
	}()
	return c
}

func (c *asyncWriteConn) Write(b []byte) (int, error) {
	cp := append([]byte(nil), b...)
	select {
	case c.writes <- cp:
		return len(b), nil
	case <-c.closed:
		return 0, errors.New("fxp test: write on closed buffered connection")
	}
}

func (c *asyncWriteConn) Close() error {
	c.once.Do(func() { close(c.closed) })
	return c.Conn.Close()
}

// deadlineConn records the most recent deadline so tests can prove CopyTo
// leaves no stale deadline behind.
type deadlineConn struct {
	net.Conn
	mu           sync.Mutex
	lastDeadline time.Time
}

func (d *deadlineConn) SetDeadline(t time.Time) error {
	d.mu.Lock()
	d.lastDeadline = t
	d.mu.Unlock()
	return d.Conn.SetDeadline(t)
}

func (d *deadlineConn) SetReadDeadline(t time.Time) error {
	d.mu.Lock()
	d.lastDeadline = t
	d.mu.Unlock()
	return d.Conn.SetReadDeadline(t)
}

func (d *deadlineConn) SetWriteDeadline(t time.Time) error {
	d.mu.Lock()
	d.lastDeadline = t
	d.mu.Unlock()
	return d.Conn.SetWriteDeadline(t)
}

func (d *deadlineConn) lastIsZero() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.lastDeadline.IsZero()
}

// fxpMock drives one end of a net.Pipe as a scripted FTP server and records
// every command line the client sent.
type fxpMock struct {
	t        *testing.T
	name     string
	client   net.Conn // the side handed to the ServerConn
	deadline *deadlineConn
	peer     net.Conn // the side driven by the mock
	mu       sync.Mutex
	commands []string
	script   []fxpStep
	done     chan error
	release  chan struct{} // closed on cleanup to unblock hanging gates
}

func newFXPMock(t *testing.T, name string, script []fxpStep) *fxpMock {
	clientPipe, peerPipe := net.Pipe()
	// Buffer writes on both ends so concurrent client/server writes behave
	// like a real full-duplex socket.
	peer := newAsyncWriteConn(peerPipe)
	dc := &deadlineConn{Conn: newAsyncWriteConn(clientPipe)}
	m := &fxpMock{
		t:        t,
		name:     name,
		client:   dc,
		deadline: dc,
		peer:     peer,
		script:   script,
		release:  make(chan struct{}),
	}
	t.Cleanup(m.cleanup)
	return m
}

func (m *fxpMock) run() {
	m.done = make(chan error, 1)
	go func() {
		p := textproto.NewConn(m.peer)
		for _, st := range m.script {
			if st.want != "" {
				full, err := p.ReadLine()
				if err != nil {
					m.done <- fmt.Errorf("%s peer read: %w", m.name, err)
					_ = m.peer.Close()
					return
				}
				m.mu.Lock()
				m.commands = append(m.commands, full)
				m.mu.Unlock()
				if full != st.want {
					m.done <- fmt.Errorf("%s: command = %q, want %q", m.name, full, st.want)
					_ = m.peer.Close()
					return
				}
				if st.closeAfter {
					_ = m.peer.Close()
					m.done <- nil
					return
				}
			}
			if st.gate != nil {
				select {
				case <-st.gate:
				case <-m.release:
				}
			}
			for _, line := range st.lines {
				if err := p.PrintfLine("%s", line); err != nil {
					m.done <- fmt.Errorf("%s reply %q: %w", m.name, line, err)
					_ = m.peer.Close()
					return
				}
			}
			if st.signal != nil {
				select {
				case st.signal <- struct{}{}:
				case <-m.release:
				}
			}
		}
		m.done <- nil
	}()
}

func (m *fxpMock) cleanup() {
	close(m.release)
	_ = m.peer.Close()
	_ = m.client.Close()
	if m.done == nil {
		return
	}
	select {
	case err := <-m.done:
		if err != nil {
			m.t.Errorf("mock %s: %v", m.name, err)
		}
	case <-time.After(2 * time.Second):
		m.t.Errorf("%s mock runner did not stop", m.name)
	}
}

func (m *fxpMock) commandLines() []string {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]string, len(m.commands))
	copy(out, m.commands)
	return out
}

func (m *fxpMock) keywords() []string {
	lines := m.commandLines()
	out := make([]string, 0, len(lines))
	for _, line := range lines {
		out = append(out, strings.Fields(line)[0])
	}
	return out
}

// newFXPClient wraps the mock side of a pipe in a logged-in-looking
// ServerConn. Its data dialer always fails: FXP must never open a data
// connection locally.
func newFXPClient(m *fxpMock, host string, customize func(*ServerConn)) (*ServerConn, *int32) {
	var dataDials int32
	failDial := func(network, address string) (net.Conn, error) {
		atomic.AddInt32(&dataDials, 1)
		return nil, errors.New("fxp test: a local data connection must not be dialed")
	}
	c := &ServerConn{
		options:  &dialOptions{dialFunc: failDial},
		conn:     textproto.NewConn(m.client),
		netConn:  m.client,
		host:     host,
		features: map[string]string{},
	}
	if customize != nil {
		customize(c)
	}
	return c, &dataDials
}

type fxpPair struct {
	src, dst *ServerConn
	srcMock  *fxpMock
	dstMock  *fxpMock
	srcDials *int32
	dstDials *int32
}

func newFXPPair(t *testing.T, srcHost, dstHost string, srcScript, dstScript []fxpStep,
	srcOpt, dstOpt func(*ServerConn)) *fxpPair {
	srcMock := newFXPMock(t, "source", srcScript)
	dstMock := newFXPMock(t, "destination", dstScript)
	srcMock.run()
	dstMock.run()
	src, srcDials := newFXPClient(srcMock, srcHost, srcOpt)
	dst, dstDials := newFXPClient(dstMock, dstHost, dstOpt)
	return &fxpPair{
		src: src, dst: dst,
		srcMock: srcMock, dstMock: dstMock,
		srcDials: srcDials, dstDials: dstDials,
	}
}

func fxpSignal() chan struct{} { return make(chan struct{}, 1) }

func fxpExpectCode(chk *unitcheck.C, t *testing.T, err error, want int) {
	t.Helper()
	var te *textproto.Error
	if !errors.As(err, &te) {
		chk.Check(t, false, "want textproto.Error in %v", err)
		return
	}
	chk.Check(t, te.Code == want, "reply code = %d, want %d (msg %q)", te.Code, want, te.Msg)
}

func fxpNoOp(chk *unitcheck.C, t *testing.T, c *ServerConn, wantOK bool) {
	t.Helper()
	err := c.NoOp()
	if wantOK {
		chk.Check(t, err == nil, "NoOp after transfer: %v", err)
	} else {
		chk.Check(t, err != nil, "NoOp on closed session must fail")
	}
}

func fxpCopyError(chk *unitcheck.C, t *testing.T, err error) *CopyError {
	t.Helper()
	var ce *CopyError
	chk.Check(t, errors.As(err, &ce), "want *CopyError, got %T: %v", err, err)
	return ce
}

func containsKeyword(keywords []string, want string) bool {
	for _, k := range keywords {
		if k == want {
			return true
		}
	}
	return false
}

func TestFXPTransfer(t *testing.T) {
	unitcheck.Run(t, func(chk *unitcheck.C) {
		t.Run("IPv6_EPRT_success", func(t *testing.T) {
			srcAccepted, dstAccepted := fxpSignal(), fxpSignal()
			p := newFXPPair(t, "2001:db8::1", "2001:db8::2",
				[]fxpStep{
					{want: "EPRT |2|2001:db8::2|5000|", lines: []string{"200 EPRT ok"}},
					{want: "RETR /incoming/src.bin", lines: []string{"150 Data connection ready"}, signal: srcAccepted},
					{want: "", lines: []string{"226 Transfer complete"}},
					{want: "NOOP", lines: []string{"200 NOOP ok"}},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR /archive/dest.bin", lines: []string{"150 Opening data connection"}, signal: dstAccepted},
					{want: "", lines: []string{"226 Transfer complete"}},
					{want: "NOOP", lines: []string{"200 NOOP ok"}},
				}, nil, nil)

			err := p.src.CopyTo(context.Background(), p.dst, "/incoming/src.bin", "/archive/dest.bin")
			chk.Check(t, err == nil, "CopyTo: %v", err)
			<-srcAccepted
			<-dstAccepted

			chk.Check(t, fmt.Sprint(p.srcMock.keywords()) == "[EPRT RETR]",
				"source commands = %v", p.srcMock.keywords())
			chk.Check(t, fmt.Sprint(p.dstMock.keywords()) == "[EPSV STOR]",
				"destination commands = %v", p.dstMock.keywords())
			chk.Check(t, atomic.LoadInt32(p.srcDials) == 0, "source data dialed %d times", *p.srcDials)
			chk.Check(t, atomic.LoadInt32(p.dstDials) == 0, "destination data dialed %d times", *p.dstDials)
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
			chk.Check(t, p.srcMock.deadline.lastIsZero(), "stale deadline left on source")
			chk.Check(t, p.dstMock.deadline.lastIsZero(), "stale deadline left on destination")
		})

		t.Run("IPv4_EPRT_success", func(t *testing.T) {
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 EPRT ok"}},
					{want: "RETR src file.txt", lines: []string{"125 Data connection already open"}},
					{want: "", lines: []string{"250 File action ok"}},
					{want: "NOOP", lines: []string{"200 NOOP ok"}},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR dest file.txt", lines: []string{"125 Data connection already open"}},
					{want: "", lines: []string{"250 File action ok"}},
					{want: "NOOP", lines: []string{"200 NOOP ok"}},
				}, nil, nil)

			err := p.src.CopyTo(context.Background(), p.dst, "src file.txt", "dest file.txt")
			chk.Check(t, err == nil, "CopyTo: %v", err)
			chk.Check(t, fmt.Sprint(p.srcMock.keywords()) == "[EPRT RETR]", "source commands = %v", p.srcMock.keywords())
			chk.Check(t, fmt.Sprint(p.dstMock.keywords()) == "[EPSV STOR]", "destination commands = %v", p.dstMock.keywords())
			chk.Check(t, atomic.LoadInt32(p.srcDials) == 0 && atomic.LoadInt32(p.dstDials) == 0,
				"data dial happened: src=%d dst=%d", *p.srcDials, *p.dstDials)
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("IPv4_PORT_fallback", func(t *testing.T) {
			disableEPSV := func(c *ServerConn) { c.options.disableEPSV = true }
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"502 EPRT not implemented"}},
					{want: "PORT 127,0,0,2,19,136", lines: []string{"200 PORT ok"}},
					{want: "RETR f", lines: []string{"150 ready"}},
					{want: "", lines: []string{"226 done"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				},
				[]fxpStep{
					{want: "PASV", lines: []string{"227 Entering Passive Mode (127,0,0,2,19,136)."}},
					{want: "STOR g", lines: []string{"150 ready"}},
					{want: "", lines: []string{"226 done"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				}, nil, disableEPSV)

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			chk.Check(t, err == nil, "CopyTo: %v", err)
			chk.Check(t, fmt.Sprint(p.srcMock.keywords()) == "[EPRT PORT RETR]",
				"source commands = %v", p.srcMock.keywords())
			chk.Check(t, fmt.Sprint(p.dstMock.keywords()) == "[PASV STOR]",
				"destination commands = %v", p.dstMock.keywords())
			chk.Check(t, atomic.LoadInt32(p.srcDials) == 0 && atomic.LoadInt32(p.dstDials) == 0,
				"data dial happened: src=%d dst=%d", *p.srcDials, *p.dstDials)
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("EPRT_other_error_no_fallback", func(t *testing.T) {
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"501 syntax error in parameters"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				}, nil, nil)

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			chk.Check(t, err != nil, "want error for 501 EPRT")
			fxpExpectCode(chk, t, err, 501)
			var se *fxpSideError
			chk.Check(t, errors.As(err, &se) && se.side == "source", "error not attributed to source: %v", err)
			chk.Check(t, fmt.Sprint(p.srcMock.keywords()) == "[EPRT]",
				"PORT must not be tried, commands = %v", p.srcMock.keywords())
			chk.Check(t, fmt.Sprint(p.dstMock.keywords()) == "[EPSV]",
				"STOR must not be sent, commands = %v", p.dstMock.keywords())
			// Fully consumed negative replies keep both sessions reusable.
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("parameter_validation", func(t *testing.T) {
			// Mocks without runners: validation must fail before any command.
			srcMock := newFXPMock(t, "source", nil)
			dstMock := newFXPMock(t, "destination", nil)
			src, _ := newFXPClient(srcMock, "127.0.0.1", nil)
			dst, _ := newFXPClient(dstMock, "127.0.0.2", nil)

			chk.Check(t, src.CopyTo(nil, dst, "a", "b") != nil, "nil context must fail")
			chk.Check(t, src.CopyTo(context.Background(), nil, "a", "b") != nil, "nil destination must fail")
			chk.Check(t, src.CopyTo(context.Background(), src, "a", "b") != nil, "same connection must fail")

			cancelled, cancel := context.WithCancel(context.Background())
			cancel()
			err := src.CopyTo(cancelled, dst, "a", "b")
			chk.Check(t, errors.Is(err, context.Canceled), "cancelled context: %v", err)

			err = src.CopyTo(context.Background(), dst, "a\rb", "b")
			chk.Check(t, errors.Is(err, ErrInvalidCommand), "CR in source path: %v", err)
			err = src.CopyTo(context.Background(), dst, "a", "b\nc")
			chk.Check(t, errors.Is(err, ErrInvalidCommand), "LF in destination path: %v", err)

			tlsSrc, _ := newFXPClient(srcMock, "127.0.0.1", func(c *ServerConn) {
				c.options.tlsConfig = &tls.Config{}
			})
			chk.Check(t, src.CopyTo(context.Background(), tlsSrc, "a", "b") != nil, "TLS source must be rejected")
			tlsDst, _ := newFXPClient(dstMock, "127.0.0.2", func(c *ServerConn) {
				c.options.tlsConfig = &tls.Config{}
			})
			chk.Check(t, src.CopyTo(context.Background(), tlsDst, "a", "b") != nil, "TLS destination must be rejected")

			pretSrc, _ := newFXPClient(srcMock, "127.0.0.1", func(c *ServerConn) { c.usePRET = true })
			chk.Check(t, pretSrc.CopyTo(context.Background(), dst, "a", "b") != nil, "PRET source must be rejected")
			pretDst, _ := newFXPClient(dstMock, "127.0.0.2", func(c *ServerConn) { c.usePRET = true })
			chk.Check(t, src.CopyTo(context.Background(), pretDst, "a", "b") != nil, "PRET destination must be rejected")

			chk.Check(t, len(srcMock.commandLines()) == 0, "source received commands: %v", srcMock.commandLines())
			chk.Check(t, len(dstMock.commandLines()) == 0, "destination received commands: %v", dstMock.commandLines())
		})
	})
}

func TestFXPFinalReplies(t *testing.T) {
	unitcheck.Run(t, func(chk *unitcheck.C) {
		// baseScripts builds a pair where the unsolicited final replies can be
		// gated independently, letting the test order or synchronize them.
		baseScripts := func(srcFinal, dstFinal []string, srcGate, dstGate <-chan struct{},
			srcSignal, dstSignal chan struct{}) ([]fxpStep, []fxpStep) {
			src := []fxpStep{
				{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 ok"}},
				{want: "RETR f", lines: []string{"150 ready"}, signal: srcSignal},
				{want: "", gate: srcGate, lines: srcFinal},
				{want: "NOOP", lines: []string{"200 ok"}},
			}
			dst := []fxpStep{
				{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
				{want: "STOR g", lines: []string{"150 ready"}, signal: dstSignal},
				{want: "", gate: dstGate, lines: dstFinal},
				{want: "NOOP", lines: []string{"200 ok"}},
			}
			return src, dst
		}

		t.Run("both_226", func(t *testing.T) {
			src, dst := baseScripts(
				[]string{"226 Transfer complete"}, []string{"226 Transfer complete"},
				nil, nil, nil, nil)
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2", src, dst, nil, nil)
			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			chk.Check(t, err == nil, "CopyTo: %v", err)
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("both_250", func(t *testing.T) {
			src, dst := baseScripts(
				[]string{"250 File action ok"}, []string{"250 File action ok"},
				nil, nil, nil, nil)
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2", src, dst, nil, nil)
			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			chk.Check(t, err == nil, "CopyTo: %v", err)
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("source_final_before_destination", func(t *testing.T) {
			dstGate := make(chan struct{})
			dstAccepted := fxpSignal()
			src, dst := baseScripts(
				[]string{"226 Transfer complete"}, []string{"226 Transfer complete"},
				nil, dstGate, nil, dstAccepted)
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2", src, dst, nil, nil)

			go func() {
				<-dstAccepted
				time.Sleep(100 * time.Millisecond)
				close(dstGate)
			}()

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			chk.Check(t, err == nil, "CopyTo: %v", err)
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
			chk.Check(t, fmt.Sprint(p.dstMock.keywords()) == "[EPSV STOR NOOP]", "commands = %v", p.dstMock.keywords())
		})

		t.Run("simultaneous_finals", func(t *testing.T) {
			finalGate := make(chan struct{})
			srcAccepted, dstAccepted := fxpSignal(), fxpSignal()
			src, dst := baseScripts(
				[]string{"226 Transfer complete"}, []string{"226 Transfer complete"},
				finalGate, finalGate, srcAccepted, dstAccepted)
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2", src, dst, nil, nil)

			go func() {
				<-srcAccepted
				<-dstAccepted
				close(finalGate)
			}()

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			chk.Check(t, err == nil, "CopyTo: %v", err)
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("source_550_destination_226", func(t *testing.T) {
			src, dst := baseScripts(
				[]string{"550 File unavailable"}, []string{"226 Transfer complete"},
				nil, nil, nil, nil)
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2", src, dst, nil, nil)
			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			ce := fxpCopyError(chk, t, err)
			if ce != nil {
				fxpExpectCode(chk, t, ce.Source, 550)
				chk.Check(t, ce.Destination == nil, "destination error = %v", ce.Destination)
			}
			chk.Check(t, !containsKeyword(p.dstMock.keywords(), "ABOR"), "negative final must not trigger ABOR")
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("source_226_destination_426", func(t *testing.T) {
			src, dst := baseScripts(
				[]string{"226 Transfer complete"}, []string{"426 Connection closed; transfer aborted"},
				nil, nil, nil, nil)
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2", src, dst, nil, nil)
			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			ce := fxpCopyError(chk, t, err)
			if ce != nil {
				chk.Check(t, ce.Source == nil, "source error = %v", ce.Source)
				fxpExpectCode(chk, t, ce.Destination, 426)
			}
			chk.Check(t, !containsKeyword(p.dstMock.keywords(), "ABOR"), "negative final must not trigger ABOR")
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("both_negative", func(t *testing.T) {
			src, dst := baseScripts(
				[]string{"550 File unavailable"}, []string{"426 transfer aborted"},
				nil, nil, nil, nil)
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2", src, dst, nil, nil)
			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			ce := fxpCopyError(chk, t, err)
			if ce != nil {
				fxpExpectCode(chk, t, ce.Source, 550)
				fxpExpectCode(chk, t, ce.Destination, 426)
			}
			// Both original textproto errors must remain reachable.
			var te *textproto.Error
			chk.Check(t, errors.As(err, &te) && te.Code == 550, "errors.As must reach source 550")
			chk.Check(t, !containsKeyword(p.srcMock.keywords(), "ABOR"), "source must not receive ABOR")
			chk.Check(t, !containsKeyword(p.dstMock.keywords(), "ABOR"), "destination must not receive ABOR")
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})
	})
}

func TestFXPAbort(t *testing.T) {
	unitcheck.Run(t, func(chk *unitcheck.C) {
		t.Run("stor_rejected_no_retr_no_abor", func(t *testing.T) {
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 ok"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR g", lines: []string{"550 permission denied"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				}, nil, nil)

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			// STOR was rejected in the send phase: a side-attributed error,
			// not a CopyError.
			var se *fxpSideError
			chk.Check(t, errors.As(err, &se) && se.side == "destination",
				"want destination side error, got %T: %v", err, err)
			fxpExpectCode(chk, t, err, 550)
			chk.Check(t, !containsKeyword(p.srcMock.keywords(), "RETR"), "RETR must not be sent: %v", p.srcMock.keywords())
			chk.Check(t, !containsKeyword(p.dstMock.keywords(), "ABOR"), "ABOR must not be sent: %v", p.dstMock.keywords())
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("retr_rejected_single_225", func(t *testing.T) {
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 ok"}},
					{want: "RETR f", lines: []string{"550 File unavailable"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR g", lines: []string{"150 ready"}},
					{want: "ABOR", lines: []string{"225 Aborting"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				}, nil, nil)

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			ce := fxpCopyError(chk, t, err)
			if ce != nil {
				fxpExpectCode(chk, t, ce.Source, 550)
				chk.Check(t, ce.Destination == nil, "destination error = %v", ce.Destination)
			}
			chk.Check(t, fmt.Sprint(p.dstMock.keywords()) == "[EPSV STOR ABOR]",
				"destination commands = %v", p.dstMock.keywords())
			chk.Check(t, fmt.Sprint(p.srcMock.keywords()) == "[EPRT RETR]",
				"source must not receive ABOR, commands = %v", p.srcMock.keywords())
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("abort_426_then_226", func(t *testing.T) {
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 ok"}},
					{want: "RETR f", lines: []string{"426 connection aborted"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR g", lines: []string{"150 ready"}},
					{want: "ABOR", lines: []string{"426 Connection closed", "226 Abort successful"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				}, nil, nil)

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			ce := fxpCopyError(chk, t, err)
			if ce != nil {
				fxpExpectCode(chk, t, ce.Source, 426)
				chk.Check(t, ce.Destination == nil, "destination error = %v", ce.Destination)
			}
			chk.Check(t, fmt.Sprint(p.dstMock.keywords()) == "[EPSV STOR ABOR]",
				"destination commands = %v", p.dstMock.keywords())
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("queued_226_then_abor_225", func(t *testing.T) {
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 ok"}},
					{want: "RETR f", lines: []string{"550 File unavailable"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR g", lines: []string{"150 ready"}},
					// Queued transfer completion arrives before ABOR.
					{want: "", lines: []string{"226 Transfer complete"}},
					{want: "ABOR", lines: []string{"225 Abort command successful"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				}, nil, nil)

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			ce := fxpCopyError(chk, t, err)
			if ce != nil {
				fxpExpectCode(chk, t, ce.Source, 550)
				chk.Check(t, ce.Destination == nil, "destination error = %v", ce.Destination)
			}
			chk.Check(t, fmt.Sprint(p.dstMock.keywords()) == "[EPSV STOR ABOR]",
				"destination commands = %v", p.dstMock.keywords())
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("abort_226_then_225", func(t *testing.T) {
			// A lone 226 after ABOR is ambiguous: the client must keep reading
			// until the 225 that belongs to ABOR.
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 ok"}},
					{want: "RETR f", lines: []string{"550 File unavailable"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR g", lines: []string{"150 ready"}},
					{want: "ABOR", lines: []string{"226 Transfer complete", "225 Abort command successful"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				}, nil, nil)

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			ce := fxpCopyError(chk, t, err)
			if ce != nil {
				fxpExpectCode(chk, t, ce.Source, 550)
				chk.Check(t, ce.Destination == nil, "destination error = %v", ce.Destination)
			}
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("abort_226_without_225_closes", func(t *testing.T) {
			// 226 followed by something other than 225: the ABOR boundary can
			// not be confirmed, the destination session must be dropped.
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 ok"}},
					{want: "RETR f", lines: []string{"550 File unavailable"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR g", lines: []string{"150 ready"}},
					{want: "ABOR", lines: []string{"226 Transfer complete", "200 unexpected"}},
				}, nil, nil)

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			ce := fxpCopyError(chk, t, err)
			if ce != nil {
				fxpExpectCode(chk, t, ce.Source, 550)
				chk.Check(t, ce.Destination != nil, "abort boundary failure must be recorded on destination")
			}
			chk.Check(t, fmt.Sprint(p.srcMock.keywords()) == "[EPRT RETR]", "source commands = %v", p.srcMock.keywords())
			chk.Check(t, fmt.Sprint(p.dstMock.keywords()) == "[EPSV STOR ABOR]", "destination commands = %v", p.dstMock.keywords())
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, false)
		})

		t.Run("retr_reply_io_error", func(t *testing.T) {
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 ok"}},
					{want: "RETR f", closeAfter: true},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR g", lines: []string{"150 ready"}},
					{want: "ABOR", lines: []string{"225 Aborting"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				}, nil, nil)

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			ce := fxpCopyError(chk, t, err)
			if ce != nil {
				chk.Check(t, ce.Source != nil, "source I/O error missing")
				var te *textproto.Error
				chk.Check(t, !errors.As(ce.Source, &te), "I/O error must not be a protocol reply")
				chk.Check(t, ce.Destination == nil, "destination error = %v", ce.Destination)
			}
			chk.Check(t, fmt.Sprint(p.dstMock.keywords()) == "[EPSV STOR ABOR]",
				"destination commands = %v", p.dstMock.keywords())
			fxpNoOp(chk, t, p.src, false)
			fxpNoOp(chk, t, p.dst, true)
		})

		t.Run("abort_reply_io_error_closes_destination", func(t *testing.T) {
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 ok"}},
					{want: "RETR f", lines: []string{"550 File unavailable"}},
					{want: "NOOP", lines: []string{"200 ok"}},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR g", lines: []string{"150 ready"}},
					{want: "ABOR", closeAfter: true},
				}, nil, nil)

			err := p.src.CopyTo(context.Background(), p.dst, "f", "g")
			ce := fxpCopyError(chk, t, err)
			if ce != nil {
				fxpExpectCode(chk, t, ce.Source, 550)
				chk.Check(t, ce.Destination != nil, "abort I/O error must stay in destination chain")
			}
			fxpNoOp(chk, t, p.src, true)
			fxpNoOp(chk, t, p.dst, false)
		})

		t.Run("cancel_during_final_wait", func(t *testing.T) {
			// Steps with a gate that is never released: the mock reads the
			// command (so keywords are recorded) but never sends the final
			// reply, and CopyTo must be unblocked by cancellation.
			never := make(chan struct{})
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 ok"}},
					{want: "RETR f", lines: []string{"150 ready"}},
					{want: "", gate: never},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR g", lines: []string{"150 ready"}},
					{want: "", gate: never},
				}, nil, nil)

			ctx, cancel := context.WithCancel(context.Background())
			done := make(chan error, 1)
			go func() { done <- p.src.CopyTo(ctx, p.dst, "f", "g") }()

			// Wait until both transfers were accepted, then cancel.
			deadline := time.Now().Add(2 * time.Second)
			for time.Now().Before(deadline) &&
				(!containsKeyword(p.srcMock.keywords(), "RETR") || !containsKeyword(p.dstMock.keywords(), "STOR")) {
				time.Sleep(5 * time.Millisecond)
			}
			chk.Check(t, containsKeyword(p.srcMock.keywords(), "RETR"), "RETR was not accepted")
			chk.Check(t, containsKeyword(p.dstMock.keywords(), "STOR"), "STOR was not accepted")
			cancel()

			select {
			case err := <-done:
				chk.Check(t, err != nil, "cancelled CopyTo must return an error")
			case <-time.After(2 * time.Second):
				t.Fatal("CopyTo did not return after context cancellation")
			}
			chk.Check(t, !containsKeyword(p.srcMock.keywords(), "ABOR"), "no ABOR on cancellation")
			chk.Check(t, !containsKeyword(p.dstMock.keywords(), "ABOR"), "no ABOR on cancellation")
			fxpNoOp(chk, t, p.src, false)
			fxpNoOp(chk, t, p.dst, false)
		})

		t.Run("deadline_during_final_wait", func(t *testing.T) {
			never := make(chan struct{})
			p := newFXPPair(t, "127.0.0.1", "127.0.0.2",
				[]fxpStep{
					{want: "EPRT |1|127.0.0.2|5000|", lines: []string{"200 ok"}},
					{want: "RETR f", lines: []string{"150 ready"}},
					{want: "", gate: never},
				},
				[]fxpStep{
					{want: "EPSV", lines: []string{"229 Entering Extended Passive Mode (|||5000|)"}},
					{want: "STOR g", lines: []string{"150 ready"}},
					{want: "", gate: never},
				}, nil, nil)

			ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
			defer cancel()
			err := p.src.CopyTo(ctx, p.dst, "f", "g")
			chk.Check(t, err != nil, "expired context must fail CopyTo")
			chk.Check(t, errors.Is(err, context.DeadlineExceeded), "deadline error expected, got %v", err)
			fxpNoOp(chk, t, p.src, false)
			fxpNoOp(chk, t, p.dst, false)
		})
	})
}
