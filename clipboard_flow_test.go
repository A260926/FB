package vnc

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/mitchellh/go-vnc/task_support/taskcheck"
)

// Extended clipboard protocol constants, re-declared here so the tests
// pack and unpack the wire format independently of the implementation.
const (
	wireFormatText uint32 = 1
	wireActCaps    uint32 = 1 << 24
	wireActRequest uint32 = 1 << 25
	wireActPeek    uint32 = 1 << 26
	wireActNotify  uint32 = 1 << 27
	wireActProvide uint32 = 1 << 28
)

// testPseudoEnc is 0xC0A1E5CE as a signed int32.
const testPseudoEnc = -1063131698

// ---------------------------------------------------------------------------
// Test server harness: drives a real Client over net.Pipe and unpacks the
// client-to-server messages on its own.
// ---------------------------------------------------------------------------

// peerMessage is a client-to-server message as decoded by the test server.
type peerMessage struct {
	typ uint8
	raw []byte // exact wire bytes of the whole message

	encs        []int32 // SetEncodings
	keysym      uint32  // KeyEvent
	down        bool    // KeyEvent
	mask        uint8   // PointerEvent
	x, y        uint16  // PointerEvent / FramebufferUpdateRequest
	width       uint16  // FramebufferUpdateRequest
	height      uint16  // FramebufferUpdateRequest
	incremental bool    // FramebufferUpdateRequest

	length int32  // CutText length as on the wire (negative = extended)
	flags  uint32 // extended CutText flags
	data   []byte // legacy text bytes, or extended payload after the flags
}

// readPeerMessage parses exactly one client-to-server message.
func readPeerMessage(r io.Reader) (peerMessage, error) {
	var m peerMessage
	var raw bytes.Buffer
	tr := io.TeeReader(r, &raw)

	readN := func(n int) ([]byte, error) {
		b := make([]byte, n)
		_, err := io.ReadFull(tr, b)
		return b, err
	}

	typ, err := readN(1)
	if err != nil {
		return m, err
	}
	m.typ = typ[0]

	switch m.typ {
	case 0: // SetPixelFormat
		if _, err := readN(19); err != nil {
			return m, err
		}
	case 2: // SetEncodings
		hdr, err := readN(3)
		if err != nil {
			return m, err
		}
		n := int(binary.BigEndian.Uint16(hdr[1:]))
		body, err := readN(4 * n)
		if err != nil {
			return m, err
		}
		for i := 0; i < n; i++ {
			m.encs = append(m.encs, int32(binary.BigEndian.Uint32(body[4*i:])))
		}
	case 3: // FramebufferUpdateRequest
		body, err := readN(9)
		if err != nil {
			return m, err
		}
		m.incremental = body[0] != 0
		m.x = binary.BigEndian.Uint16(body[1:])
		m.y = binary.BigEndian.Uint16(body[3:])
		m.width = binary.BigEndian.Uint16(body[5:])
		m.height = binary.BigEndian.Uint16(body[7:])
	case 4: // KeyEvent
		body, err := readN(7)
		if err != nil {
			return m, err
		}
		m.down = body[0] != 0
		m.keysym = binary.BigEndian.Uint32(body[3:])
	case 5: // PointerEvent
		body, err := readN(5)
		if err != nil {
			return m, err
		}
		m.mask = body[0]
		m.x = binary.BigEndian.Uint16(body[1:])
		m.y = binary.BigEndian.Uint16(body[3:])
	case 6: // CutText
		hdr, err := readN(7)
		if err != nil {
			return m, err
		}
		m.length = int32(binary.BigEndian.Uint32(hdr[3:]))
		if m.length < 0 {
			size := -int64(m.length)
			if size < 4 || size > 4<<20 {
				return m, fmt.Errorf("test: bad extended cut text size %d", size)
			}
			payload, err := readN(int(size))
			if err != nil {
				return m, err
			}
			m.flags = binary.BigEndian.Uint32(payload[:4])
			m.data = payload[4:]
		} else {
			if m.length > 4<<20 {
				return m, fmt.Errorf("test: bad cut text length %d", m.length)
			}
			if m.data, err = readN(int(m.length)); err != nil {
				return m, err
			}
		}
	default:
		return m, fmt.Errorf("test: unexpected client message type %d", m.typ)
	}

	m.raw = raw.Bytes()
	return m, nil
}

// boundaryLen is the exact wire length a message of the given parsed
// content must have.
func boundaryLen(m peerMessage) int {
	switch m.typ {
	case 0:
		return 20
	case 2:
		return 4 + 4*len(m.encs)
	case 3:
		return 10
	case 4:
		return 8
	case 5:
		return 6
	case 6:
		if m.length < 0 {
			return 8 + int(-m.length)
		}
		return 8 + int(m.length)
	}
	return -1
}

type peerEvent struct {
	msg peerMessage
	err error
}

// serverEnd is the server side of the pipe: a goroutine unpacks every
// client message and queues it for the test.
type serverEnd struct {
	conn   net.Conn
	events chan peerEvent
}

func (s *serverEnd) readLoop() {
	for {
		m, err := readPeerMessage(s.conn)
		if err != nil {
			s.events <- peerEvent{err: err}
			return
		}
		s.events <- peerEvent{msg: m}
	}
}

// recv returns the next client message; ok is false once the connection
// is closed or breaks.
func (s *serverEnd) recv(t *testing.T) (peerMessage, bool) {
	t.Helper()
	select {
	case ev := <-s.events:
		if ev.err != nil {
			return peerMessage{}, false
		}
		return ev.msg, true
	case <-time.After(10 * time.Second):
		t.Fatal("timed out waiting for a client message")
		return peerMessage{}, false
	}
}

// write sends a server-to-client message.
func (s *serverEnd) write(t *testing.T, b []byte) {
	t.Helper()
	if _, err := s.conn.Write(b); err != nil {
		t.Fatalf("server write failed: %v", err)
	}
}

// serverHandshake performs the server side of the RFB handshake.
func serverHandshake(conn net.Conn) error {
	if _, err := io.WriteString(conn, "RFB 003.008\n"); err != nil {
		return err
	}
	buf := make([]byte, 12)
	if _, err := io.ReadFull(conn, buf); err != nil {
		return err
	}
	if string(buf) != "RFB 003.008\n" {
		return fmt.Errorf("test: unexpected protocol version %q", buf)
	}
	// One security type: none.
	if _, err := conn.Write([]byte{1, 1}); err != nil {
		return err
	}
	if _, err := io.ReadFull(conn, buf[:1]); err != nil {
		return err
	}
	if buf[0] != 1 {
		return fmt.Errorf("test: unexpected security type %d", buf[0])
	}
	// SecurityResult: OK.
	if _, err := conn.Write([]byte{0, 0, 0, 0}); err != nil {
		return err
	}
	// ClientInit.
	if _, err := io.ReadFull(conn, buf[:1]); err != nil {
		return err
	}
	// ServerInit: 320x200, 32bpp true color, name "test".
	init := []byte{
		0x01, 0x40, 0x00, 0xC8,
		32, 24, 0, 1,
		0, 255, 0, 255, 0, 255,
		16, 8, 0,
		0, 0, 0,
		0, 0, 0, 4,
	}
	init = append(init, []byte("test")...)
	_, err := conn.Write(init)
	return err
}

// newTestClient runs a real Client handshake over net.Pipe and returns the
// connection plus a server end that continuously unpacks client messages.
func newTestClient(t *testing.T, cfg *ClientConfig) (*ClientConn, *serverEnd) {
	t.Helper()
	cli, srv := net.Pipe()
	if err := cli.SetDeadline(time.Now().Add(15 * time.Second)); err != nil {
		t.Fatal(err)
	}
	if err := srv.SetDeadline(time.Now().Add(15 * time.Second)); err != nil {
		t.Fatal(err)
	}
	se := &serverEnd{conn: srv, events: make(chan peerEvent, 1024)}
	hs := make(chan error, 1)
	go func() {
		if err := serverHandshake(srv); err != nil {
			hs <- err
			return
		}
		hs <- nil
		se.readLoop()
	}()
	cc, err := Client(cli, cfg)
	if err != nil {
		t.Fatalf("Client failed: %v", err)
	}
	if err := <-hs; err != nil {
		t.Fatalf("server handshake failed: %v", err)
	}
	t.Cleanup(func() {
		cc.Close()
		srv.Close()
	})
	return cc, se
}

// ---------------------------------------------------------------------------
// Server-to-client message builders (independent encoding).
// ---------------------------------------------------------------------------

// srvExt builds an extended ServerCutText message.
func srvExt(flags uint32, data []byte) []byte {
	b := make([]byte, 12+len(data))
	b[0] = 3
	binary.BigEndian.PutUint32(b[4:], uint32(-int32(4+len(data))))
	binary.BigEndian.PutUint32(b[8:], flags)
	copy(b[12:], data)
	return b
}

// srvCaps builds a Caps message with one unsolicited size per size given.
func srvCaps(formats, actions uint32, sizes ...uint32) []byte {
	data := make([]byte, 4*len(sizes))
	for i, s := range sizes {
		binary.BigEndian.PutUint32(data[4*i:], s)
	}
	return srvExt(actions|formats, data)
}

// srvProvide builds a Provide message; entries must be in format-bit order.
func srvProvide(formats uint32, entries ...[]byte) []byte {
	var raw bytes.Buffer
	for _, e := range entries {
		var lenBuf [4]byte
		binary.BigEndian.PutUint32(lenBuf[:], uint32(len(e)))
		raw.Write(lenBuf[:])
		raw.Write(e)
	}
	return srvProvideRaw(formats, raw.Bytes())
}

// srvProvideRaw builds a Provide message from an already serialized body.
func srvProvideRaw(formats uint32, body []byte) []byte {
	return srvExt(wireActProvide|formats, zipBody(body))
}

// zipBody compresses a Provide body.
func zipBody(body []byte) []byte {
	var z bytes.Buffer
	zw := zlib.NewWriter(&z)
	zw.Write(body)
	zw.Close()
	return z.Bytes()
}

// srvLegacyCutText builds a legacy ServerCutText message.
func srvLegacyCutText(b []byte) []byte {
	msg := make([]byte, 8+len(b))
	msg[0] = 3
	binary.BigEndian.PutUint32(msg[4:], uint32(len(b)))
	copy(msg[8:], b)
	return msg
}

// fullServerCaps declares text plus every action, the complete capability
// set the client needs for the extended flow.
func fullServerCaps() []byte {
	return srvCaps(wireFormatText, wireActCaps|wireActRequest|wireActPeek|wireActNotify|wireActProvide, 0)
}

// wireText converts a CRLF-form string to the NUL-terminated wire form.
func wireText(s string) []byte {
	return []byte(s + "\x00")
}

// provideBody is the expected decompressed body of a text Provide.
func provideBody(textCRLF string) []byte {
	wire := wireText(textCRLF)
	b := make([]byte, 4+len(wire))
	binary.BigEndian.PutUint32(b, uint32(len(wire)))
	copy(b[4:], wire)
	return b
}

// ---------------------------------------------------------------------------
// Shared expectations.
// ---------------------------------------------------------------------------

// recvServerMsg reads one message from the business channel.
func recvServerMsg(t *testing.T, ch chan ServerMessage) ServerMessage {
	t.Helper()
	select {
	case m := <-ch:
		return m
	case <-time.After(10 * time.Second):
		t.Fatal("timed out waiting for a server message")
		return nil
	}
}

// expectClientCaps reads and verifies the client's Caps answer.
func expectClientCaps(t *testing.T, c *taskcheck.C, se *serverEnd) {
	t.Helper()
	m, ok := se.recv(t)
	isCaps := ok && m.typ == 6 && m.length == -8
	c.Check(isCaps,
		"client caps: expected an extended cut text of 8 payload bytes, got %+v", m)
	if !isCaps {
		return
	}
	wantFlags := wireActCaps | wireActRequest | wireActPeek | wireActNotify | wireActProvide | wireFormatText
	c.Check(m.flags == wantFlags,
		"client caps flags = %#x, want %#x (text + all actions)", m.flags, wantFlags)
	c.Check(len(m.data) == 4 && binary.BigEndian.Uint32(m.data) == 0,
		"client caps sizes = %x, want a single zero uint32 for text", m.data)
	wantRaw := []byte{6, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xF8, 0x1F, 0x00, 0x00, 0x01, 0, 0, 0, 0}
	c.Check(bytes.Equal(m.raw, wantRaw), "client caps raw = %x, want %x", m.raw, wantRaw)
}

// inflateCheck independently decompresses the client's Provide payload and
// verifies the zlib stream is self-contained.
func inflateCheck(t *testing.T, c *taskcheck.C, data []byte) []byte {
	t.Helper()
	br := bytes.NewReader(data)
	zr, err := zlib.NewReader(br)
	if err != nil {
		c.Check(false, "client provide: bad zlib stream: %v", err)
		return nil
	}
	raw, err := io.ReadAll(zr)
	c.Check(err == nil, "client provide: inflate failed: %v", err)
	c.Check(zr.Close() == nil, "client provide: zlib stream did not finish cleanly")
	c.Check(br.Len() == 0, "client provide: %d bytes trail the zlib stream", br.Len())
	return raw
}

// negotiatedClient connects with the extended clipboard enabled and runs
// the capability exchange.
func negotiatedClient(t *testing.T, c *taskcheck.C, ch chan ServerMessage) (*ClientConn, *serverEnd) {
	t.Helper()
	cc, se := newTestClient(t, &ClientConfig{ExtendedClipboard: true, ServerMessageCh: ch})
	m, ok := se.recv(t)
	advertised := ok && m.typ == 2 && len(m.encs) == 2 && m.encs[0] == 0 && uint32(m.encs[1]) == 0xC0A1E5CE
	c.Check(advertised,
		"initial SetEncodings = %+v, want [raw 0xC0A1E5CE]", m)
	if !advertised {
		t.Fatal("cannot continue without the initial SetEncodings")
	}
	se.write(t, fullServerCaps())
	expectClientCaps(t, c, se)
	return cc, se
}

// ---------------------------------------------------------------------------
// Extra encodings for SetEncodings tests.
// ---------------------------------------------------------------------------

// testPseudoEncoding is an independent Encoding with the extended
// clipboard pseudo-encoding number.
type testPseudoEncoding struct{}

func (testPseudoEncoding) Type() int32 { return testPseudoEnc }

func (testPseudoEncoding) Read(*ClientConn, *Rectangle, io.Reader) (Encoding, error) {
	return nil, fmt.Errorf("test: no rectangles")
}

// testEncoding is an arbitrary encoding number for SetEncodings tests.
type testEncoding struct{ typ int32 }

func (e testEncoding) Type() int32 { return e.typ }

func (e testEncoding) Read(*ClientConn, *Rectangle, io.Reader) (Encoding, error) {
	return nil, fmt.Errorf("test: no rectangles")
}

// ---------------------------------------------------------------------------
// scriptConn: a controllable net.Conn for write-path tests.
// ---------------------------------------------------------------------------

type scriptConn struct {
	mu        sync.Mutex
	readBuf   []byte
	closed    chan struct{}
	closeOnce sync.Once

	maxChunk     int   // >0: limit bytes accepted per Write
	zeroProgress bool  // Write returns (0, nil)
	writeErr     error // next Write fails after a partial write

	written []byte
	writes  chan []byte
}

func newScriptConn(serverBytes []byte) *scriptConn {
	return &scriptConn{
		readBuf: serverBytes,
		closed:  make(chan struct{}),
		writes:  make(chan []byte, 64),
	}
}

func (s *scriptConn) Read(p []byte) (int, error) {
	s.mu.Lock()
	if len(s.readBuf) > 0 {
		n := copy(p, s.readBuf)
		s.readBuf = s.readBuf[n:]
		s.mu.Unlock()
		return n, nil
	}
	s.mu.Unlock()
	<-s.closed
	return 0, io.EOF
}

func (s *scriptConn) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	select {
	case <-s.closed:
		return 0, io.ErrClosedPipe
	default:
	}
	if s.zeroProgress {
		return 0, nil
	}
	if s.writeErr != nil {
		err := s.writeErr
		s.writeErr = nil
		n := len(p) / 2
		s.written = append(s.written, p[:n]...)
		return n, err
	}
	n := len(p)
	if s.maxChunk > 0 && n > s.maxChunk {
		n = s.maxChunk
	}
	s.written = append(s.written, p[:n]...)
	s.writes <- append([]byte(nil), p[:n]...)
	return n, nil
}

func (s *scriptConn) Close() error {
	s.closeOnce.Do(func() { close(s.closed) })
	return nil
}

func (s *scriptConn) LocalAddr() net.Addr              { return dummyAddr("local") }
func (s *scriptConn) RemoteAddr() net.Addr             { return dummyAddr("remote") }
func (s *scriptConn) SetDeadline(time.Time) error      { return nil }
func (s *scriptConn) SetReadDeadline(time.Time) error  { return nil }
func (s *scriptConn) SetWriteDeadline(time.Time) error { return nil }

type dummyAddr string

func (a dummyAddr) Network() string { return string(a) }
func (a dummyAddr) String() string  { return string(a) }

func (s *scriptConn) setMaxChunk(n int) {
	s.mu.Lock()
	s.maxChunk = n
	s.mu.Unlock()
}

func (s *scriptConn) setZeroProgress(on bool) {
	s.mu.Lock()
	s.zeroProgress = on
	s.mu.Unlock()
}

func (s *scriptConn) setWriteError(err error) {
	s.mu.Lock()
	s.writeErr = err
	s.mu.Unlock()
}

func (s *scriptConn) isClosed() bool {
	select {
	case <-s.closed:
		return true
	default:
		return false
	}
}

func (s *scriptConn) writtenBytes() []byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]byte(nil), s.written...)
}

// waitWrites blocks until n writes have been observed.
func (s *scriptConn) waitWrites(t *testing.T, n int) {
	t.Helper()
	for i := 0; i < n; i++ {
		select {
		case <-s.writes:
		case <-time.After(10 * time.Second):
			t.Fatalf("timed out waiting for write %d of %d", i+1, n)
		}
	}
}

// handshakeServerBytes returns the canned server bytes for a handshake,
// followed by any extra server-to-client messages.
func handshakeServerBytes(extra ...[]byte) []byte {
	var b bytes.Buffer
	b.WriteString("RFB 003.008\n")
	b.Write([]byte{1, 1})       // one security type: none
	b.Write([]byte{0, 0, 0, 0}) // security result: OK
	b.Write([]byte{0x01, 0x40, 0x00, 0xC8})
	b.Write([]byte{32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0})
	b.Write([]byte{0, 0, 0, 4})
	b.WriteString("test")
	for _, e := range extra {
		b.Write(e)
	}
	return b.Bytes()
}

// ---------------------------------------------------------------------------
// TestClipboardNegotiation: the ExtendedClipboard switch, the initial and
// subsequent SetEncodings, and the Caps exchange.
// ---------------------------------------------------------------------------

func TestClipboardNegotiation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var m peerMessage
		var ok bool

		// --- Disabled: no post-handshake chatter, negative length rejected.
		ccOff, seOff := newTestClient(t, &ClientConfig{})
		if err := ccOff.KeyEvent(0x61, true); err != nil {
			t.Fatalf("KeyEvent: %v", err)
		}
		m, ok = seOff.recv(t)
		c.Check(ok && m.typ == 4 && m.keysym == 0x61 && m.down,
			"disabled: first client message = %+v, want the key event (no extra messages after the handshake)", m)
		c.Check(len(ccOff.Encs) == 0,
			"disabled: %d encodings set automatically, want none", len(ccOff.Encs))

		// An explicit SetEncodings does not gain the pseudo-encoding.
		if err := ccOff.SetEncodings([]Encoding{new(RawEncoding)}); err != nil {
			t.Fatalf("SetEncodings: %v", err)
		}
		m, ok = seOff.recv(t)
		c.Check(ok && m.typ == 2 && len(m.encs) == 1 && m.encs[0] == 0,
			"disabled: SetEncodings = %+v, want [raw] only", m)

		// A negative-length CutText must not enable the extension; the
		// connection drops without answering Caps. Send only the header:
		// the client must reject it right there.
		badHdr := make([]byte, 8)
		badHdr[0] = 3
		negLen := int32(-8)
		binary.BigEndian.PutUint32(badHdr[4:], uint32(negLen))
		seOff.write(t, badHdr)
		_, ok = seOff.recv(t)
		c.Check(!ok, "disabled: a negative-length message must drop the connection without enabling the extension")
		ccOff.Close()
		seOff.conn.Close()

		// --- Enabled: the initial SetEncodings advertises Raw + pseudo.
		ch := make(chan ServerMessage, 16)
		cc, se := newTestClient(t, &ClientConfig{ExtendedClipboard: true, ServerMessageCh: ch})
		m, ok = se.recv(t)
		c.Check(ok && m.typ == 2, "enabled: first client message = %+v, want SetEncodings", m)
		c.Check(len(m.encs) == 2 && m.encs[0] == 0 && uint32(m.encs[1]) == 0xC0A1E5CE,
			"enabled: initial encodings = %v, want [0 0xC0A1E5CE]", m.encs)
		wantRaw := []byte{2, 0, 0, 2, 0, 0, 0, 0, 0xC0, 0xA1, 0xE5, 0xCE}
		c.Check(bytes.Equal(m.raw, wantRaw),
			"enabled: initial SetEncodings raw = %x, want %x", m.raw, wantRaw)
		c.Check(len(cc.Encs) == 2,
			"enabled: conn encodings = %d, want 2 after the initial SetEncodings", len(cc.Encs))

		// Later SetEncodings keep the caller's order, add the
		// pseudo-encoding exactly once, and leave the caller's slice alone.
		caller := []Encoding{new(RawEncoding)}
		if err := cc.SetEncodings(caller); err != nil {
			t.Fatalf("SetEncodings: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && len(m.encs) == 2 && m.encs[0] == 0 && uint32(m.encs[1]) == 0xC0A1E5CE,
			"SetEncodings([raw]) sent %v, want [raw pseudo]", m.encs)
		c.Check(len(caller) == 1,
			"SetEncodings modified the caller's slice: len = %d, want 1", len(caller))

		dup := []Encoding{testPseudoEncoding{}, testPseudoEncoding{}, new(RawEncoding)}
		if err := cc.SetEncodings(dup); err != nil {
			t.Fatalf("SetEncodings: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && len(m.encs) == 2 && uint32(m.encs[0]) == 0xC0A1E5CE && m.encs[1] == 0,
			"SetEncodings([pseudo pseudo raw]) sent %v, want [pseudo raw]", m.encs)
		c.Check(len(dup) == 3,
			"SetEncodings modified the caller's slice: len = %d, want 3", len(dup))

		// Caps with an unknown format: the sizes are consumed in
		// format-bit order and the client answers with its own Caps.
		se.write(t, srvCaps(wireFormatText|0x08, wireActCaps|wireActRequest|wireActPeek|wireActNotify|wireActProvide, 0, 4096))
		expectClientCaps(t, c, se)

		// The unknown format's size must not leak into the next message:
		// a Bell right after the Caps still parses.
		se.write(t, []byte{2})
		got := recvServerMsg(t, ch)
		_, isBell := got.(*BellMessage)
		c.Check(isBell, "after caps with an unknown format, got %T, want *BellMessage", got)

		// Negotiation is complete: CutText goes out as an extended Notify.
		if err := cc.CutText("abc"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && m.typ == 6 && m.length == -4 && m.flags == wireActNotify|wireFormatText && len(m.data) == 0,
			"negotiated CutText = %+v, want an extended Notify(text)", m)

		// A repeated Caps replaces the peer record: without text the
		// connection falls back to legacy cut text.
		se.write(t, srvCaps(0, wireActCaps|wireActRequest|wireActNotify|wireActProvide))
		expectClientCaps(t, c, se)
		if err := cc.CutText("abc"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && m.typ == 6 && m.length == 3 && string(m.data) == "abc",
			"after caps replacement CutText = %+v, want legacy 'abc'", m)

		// Re-negotiate: full caps again, and the extended flow resumes.
		se.write(t, fullServerCaps())
		expectClientCaps(t, c, se)
		if err := cc.CutText("abc"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && m.length == -4 && m.flags == wireActNotify|wireFormatText,
			"re-negotiated CutText = %+v, want an extended Notify", m)

		// Partial caps (Provide missing) do not complete the negotiation.
		cc2, se2 := newTestClient(t, &ClientConfig{ExtendedClipboard: true})
		m, ok = se2.recv(t)
		c.Check(ok && m.typ == 2, "partial caps: initial SetEncodings = %+v", m)
		se2.write(t, srvCaps(wireFormatText, wireActCaps|wireActRequest|wireActNotify, 0))
		expectClientCaps(t, c, se2)
		if err := cc2.CutText("abc"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se2.recv(t)
		c.Check(ok && m.length == 3 && string(m.data) == "abc",
			"partial caps: CutText = %+v, want legacy 'abc'", m)

		// Negotiation is per connection: the first connection is
		// negotiated, the second is not.
		if err := cc.CutText("x"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && m.length == -4 && m.flags == wireActNotify|wireFormatText,
			"negotiated connection CutText = %+v, want an extended Notify", m)
		if err := cc2.CutText("x"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se2.recv(t)
		c.Check(ok && m.length == 1 && string(m.data) == "x",
			"unnegotiated connection CutText = %+v, want legacy 'x'", m)
	})
}

// ---------------------------------------------------------------------------
// TestClipboardOwnership: local ownership, remote ownership, and the
// Request/Notify/Provide flow.
// ---------------------------------------------------------------------------

func TestClipboardOwnership(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var m peerMessage
		var ok bool

		ch := make(chan ServerMessage, 16)
		cc, se := negotiatedClient(t, c, ch)

		// Two local copies, then a Request: the server must get the SECOND.
		if err := cc.CutText("first"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && m.length == -4 && m.flags == wireActNotify|wireFormatText && len(m.data) == 0,
			"first CutText = %+v, want a bare Notify(text), not a Provide", m)
		if err := cc.CutText("second\n行"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActNotify|wireFormatText,
			"second CutText = %+v, want a bare Notify(text)", m)

		se.write(t, srvExt(wireActRequest|wireFormatText, nil))
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActProvide|wireFormatText,
			"Request answer = %+v, want Provide(text)", m)
		raw := inflateCheck(t, c, m.data)
		c.Check(bytes.Equal(raw, provideBody("second\r\n行")),
			"provided body = %q, want the CRLF+NUL form of the second copy", raw)

		// Copying an empty string still owns the text format.
		if err := cc.CutText(""); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActNotify|wireFormatText,
			"empty CutText = %+v, want a Notify(text)", m)
		se.write(t, srvExt(wireActRequest|wireFormatText, nil))
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActProvide|wireFormatText,
			"empty copy: Request answer = %+v, want a Provide, not an empty Notify", m)
		raw = inflateCheck(t, c, m.data)
		c.Check(bytes.Equal(raw, provideBody("")),
			"empty copy provide body = %x, want a lone NUL", raw)

		// A remote Notify(text) revokes ownership and triggers one Request.
		se.write(t, srvExt(wireActNotify|wireFormatText, nil))
		m, ok = se.recv(t)
		c.Check(ok && m.length == -4 && m.flags == wireActRequest|wireFormatText,
			"remote Notify(text) answer = %+v, want Request(text)", m)
		wantReq := []byte{6, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFC, 0x02, 0x00, 0x00, 0x01}
		c.Check(bytes.Equal(m.raw, wantReq), "client Request raw = %x, want %x", m.raw, wantReq)

		// Ownership is gone: a Request now gets an empty Notify.
		se.write(t, srvExt(wireActRequest|wireFormatText, nil))
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActNotify && len(m.data) == 0,
			"after a remote Notify, Request answer = %+v, want an empty Notify", m)

		// A valid Provide(text) is published normalized, and the received
		// text is not echoed back as a local copy.
		se.write(t, srvProvide(wireFormatText, wireText("héllo\r\nworld\r\n")))
		got := recvServerMsg(t, ch)
		sct, isCut := got.(*ServerCutTextMessage)
		c.Check(isCut && sct.Text == "héllo\nworld\n",
			"published text = %#v, want %q", got, "héllo\nworld\n")
		se.write(t, srvExt(wireActRequest|wireFormatText, nil))
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActNotify,
			"received text must not be echoed: Request answer = %+v, want an empty Notify", m)

		// A remote Notify without text does not trigger a Request; the
		// ownership is still revoked. The Peek right after proves both:
		// its answer must arrive first and list no formats.
		if err := cc.CutText("mine"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActNotify|wireFormatText,
			"re-own CutText = %+v, want a Notify(text)", m)
		se.write(t, srvExt(wireActNotify|0x02, nil)) // remote has rtf only
		se.write(t, srvExt(wireActPeek|wireFormatText, nil))
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActNotify,
			"Peek after a remote rtf Notify = %+v, want an empty Notify (and no Request in between)", m)

		// Peek with local ownership reports text.
		if err := cc.CutText("owned"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActNotify|wireFormatText,
			"owned CutText = %+v, want a Notify(text)", m)
		se.write(t, srvExt(wireActPeek|wireFormatText, nil))
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActNotify|wireFormatText,
			"Peek with ownership = %+v, want Notify(text)", m)

		// Legacy server cut text still arrives after negotiation, is
		// published, and revokes ownership.
		if err := cc.CutText("loc"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActNotify|wireFormatText,
			"loc CutText = %+v, want a Notify(text)", m)
		se.write(t, srvLegacyCutText([]byte("legacy")))
		got = recvServerMsg(t, ch)
		sct, isCut = got.(*ServerCutTextMessage)
		c.Check(isCut && sct.Text == "legacy",
			"legacy publish = %#v, want %q", got, "legacy")
		se.write(t, srvExt(wireActRequest|wireFormatText, nil))
		m, ok = se.recv(t)
		c.Check(ok && m.flags == wireActNotify,
			"legacy receive must revoke ownership: Request answer = %+v, want an empty Notify", m)

		// A nil business channel does not stop negotiation or replies.
		ccNil, seNil := newTestClient(t, &ClientConfig{ExtendedClipboard: true})
		m, ok = seNil.recv(t)
		c.Check(ok && m.typ == 2, "nil channel: initial SetEncodings = %+v", m)
		seNil.write(t, fullServerCaps())
		expectClientCaps(t, c, seNil)
		seNil.write(t, srvExt(wireActNotify|wireFormatText, nil))
		m, ok = seNil.recv(t)
		c.Check(ok && m.flags == wireActRequest|wireFormatText,
			"nil channel: remote Notify answered with %+v, want Request(text)", m)
		seNil.write(t, srvProvide(wireFormatText, wireText("x"))) // published nowhere
		if err := ccNil.KeyEvent(0x61, true); err != nil {
			t.Fatalf("KeyEvent: %v", err)
		}
		m, ok = seNil.recv(t)
		c.Check(ok && m.typ == 4 && m.keysym == 0x61,
			"nil channel: connection still usable, got %+v", m)
	})
}

// ---------------------------------------------------------------------------
// TestClipboardWire: exact wire images, legacy conversion, malformed
// messages, and whole-message write boundaries.
// ---------------------------------------------------------------------------

func TestClipboardWire(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var m peerMessage
		var ok bool

		ch := make(chan ServerMessage, 16)
		cc, se := negotiatedClient(t, c, ch)

		// Exact wire image of an extended Notify.
		if err := cc.CutText("hi"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = se.recv(t)
		wantNotify := []byte{6, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFC, 0x08, 0x00, 0x00, 0x01}
		c.Check(ok && bytes.Equal(m.raw, wantNotify),
			"extended Notify raw = %x, want %x", m.raw, wantNotify)

		// The Provide answer is one self-contained zlib stream.
		se.write(t, srvExt(wireActRequest|wireFormatText, nil))
		m, ok = se.recv(t)
		c.Check(ok && m.typ == 6 && m.length < 0 && m.flags == wireActProvide|wireFormatText,
			"Provide header = %+v, want an extended Provide(text)", m)
		c.Check(ok && bytes.Equal(m.raw[1:4], []byte{0, 0, 0}),
			"Provide padding = %x, want three zero bytes", m.raw[1:4])
		raw := inflateCheck(t, c, m.data)
		c.Check(bytes.Equal(raw, provideBody("hi")),
			"Provide body = %x, want %x", raw, provideBody("hi"))

		// Control messages do not desync the stream: a Bell afterwards
		// still arrives.
		se.write(t, []byte{2})
		got := recvServerMsg(t, ch)
		_, isBell := got.(*BellMessage)
		c.Check(isBell, "after control messages, got %T, want *BellMessage", got)

		// --- Legacy send: multibyte Go characters become single Latin-1
		// bytes, and non Latin-1 fails before anything is written.
		chL := make(chan ServerMessage, 4)
		ccL, seL := newTestClient(t, &ClientConfig{ServerMessageCh: chL})
		if err := ccL.CutText("café"); err != nil {
			t.Fatalf("CutText café: %v", err)
		}
		m, ok = seL.recv(t)
		c.Check(ok && m.length == 4 && bytes.Equal(m.data, []byte{'c', 'a', 'f', 0xE9}),
			"legacy café = %+v, want 4 Latin-1 bytes", m)

		err := ccL.CutText("中文")
		c.Check(err != nil, "legacy CutText(中文) must fail, got nil")
		if err := ccL.KeyEvent(0x61, true); err != nil {
			t.Fatalf("KeyEvent: %v", err)
		}
		m, ok = seL.recv(t)
		c.Check(ok && m.typ == 4,
			"the failed CutText wrote something: next message = %+v, want the key event", m)

		// Legacy receive: three padding bytes, Latin-1 to Go string.
		seL.write(t, []byte{3, 0, 0, 0, 0, 0, 0, 2, 0xE9, 0x0A})
		got = recvServerMsg(t, chL)
		sct, isCut := got.(*ServerCutTextMessage)
		c.Check(isCut && sct.Text == "é\n",
			"legacy receive = %#v, want %q", got, "é\n")

		// A legacy body over 1 MiB is rejected on the header, before the
		// body is allocated or read.
		var hdr [8]byte
		hdr[0] = 3
		binary.BigEndian.PutUint32(hdr[4:], (1<<20)+1)
		seL.write(t, hdr[:])
		_, ok = seL.recv(t)
		c.Check(!ok, "an oversize legacy cut text must drop the connection")
		ccL.Close()
		seL.conn.Close()

		// --- Malformed extended messages: each must drop the connection
		// without publishing anything.
		lengthMismatch := []byte{0, 0, 0, 10, 'a', 'b'}
		trailingBody := append(provideBody("ok"), 0xEE)
		oversizeHdr := make([]byte, 8)
		oversizeHdr[0] = 3
		oversizeLen := int32(-((1 << 20) + 1))
		binary.BigEndian.PutUint32(oversizeHdr[4:], uint32(oversizeLen))
		bigBody := make([]byte, (1<<20)+1)
		badCases := []struct {
			name string
			msg  []byte
		}{
			{"corrupt zlib", srvExt(wireActProvide|wireFormatText, []byte{1, 2, 3, 4, 5})},
			{"missing terminator", srvProvide(wireFormatText, []byte("abc"))},
			{"invalid utf-8", srvProvide(wireFormatText, []byte{0xFF, 0xFE, 0x00})},
			{"embedded NUL", srvProvide(wireFormatText, []byte{'a', 0, 'b', 0})},
			{"trailing after zlib", srvExt(wireActProvide|wireFormatText, append(zipBody(provideBody("ok")), 0))},
			{"length mismatch", srvProvideRaw(wireFormatText, lengthMismatch)},
			{"trailing in decompressed", srvProvideRaw(wireFormatText, trailingBody)},
			{"two actions", srvExt(wireActRequest|wireActProvide|wireFormatText, nil)},
			{"no action", srvExt(wireFormatText, nil)},
			{"oversize payload", oversizeHdr},
			{"oversize decompressed", srvProvideRaw(wireFormatText, bigBody)},
			{"caps missing size", srvCaps(wireFormatText|0x02, wireActCaps, 0)},
			{"caps trailing", srvCaps(wireFormatText, wireActCaps, 0, 0)},
			{"request trailing", srvExt(wireActRequest|wireFormatText, []byte{0})},
		}
		for _, tc := range badCases {
			chBad := make(chan ServerMessage, 4)
			ccBad, seBad := newTestClient(t, &ClientConfig{ExtendedClipboard: true, ServerMessageCh: chBad})
			m, ok = seBad.recv(t)
			c.Check(ok && m.typ == 2, "%s: initial SetEncodings = %+v", tc.name, m)
			seBad.write(t, tc.msg)
			_, ok = seBad.recv(t)
			c.Check(!ok, "%s: the malformed message must drop the connection", tc.name)
			select {
			case published := <-chBad:
				c.Check(false, "%s: published %#v despite the parse failure", tc.name, published)
			default:
			}
			ccBad.Close()
			seBad.conn.Close()
		}

		// --- Unknown formats are fully parsed but not published.
		chU := make(chan ServerMessage, 16)
		_, seU := negotiatedClient(t, c, chU)
		seU.write(t, srvProvide(wireFormatText|0x04, wireText("hi"), []byte("<b>hi</b>")))
		got = recvServerMsg(t, chU)
		sct, isCut = got.(*ServerCutTextMessage)
		c.Check(isCut && sct.Text == "hi",
			"provide with html: published = %#v, want the text only", got)

		seU.write(t, srvProvide(0x04, []byte("<b>only</b>"))) // no text at all
		seU.write(t, []byte{2})                               // proves the stream is still aligned
		got = recvServerMsg(t, chU)
		_, isBell = got.(*BellMessage)
		c.Check(isBell, "provide without text published nothing; next = %#v, want *BellMessage", got)

		// --- Concurrent user input and automatic replies share
		// whole-message write boundaries.
		ccC, seC := negotiatedClient(t, c, nil)
		if err := ccC.CutText("sync"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		m, ok = seC.recv(t)
		c.Check(ok && m.flags == wireActNotify|wireFormatText,
			"concurrent: CutText = %+v, want a Notify(text)", m)

		const (
			workers    = 4
			perWorker  = 5
			nKey       = workers * perWorker
			nPtr       = 3
			nUpdate    = 2
			nEnc       = 2
			nPixFmt    = 1
			nProvide   = 1
			totalMsgs  = nKey + nPtr + nUpdate + nEnc + nPixFmt + nProvide
			keysymBase = 0x1000
		)
		type collectResult struct {
			msgs []peerMessage
			err  error
		}
		results := make(chan collectResult, 1)
		go func() {
			msgs := make([]peerMessage, 0, totalMsgs)
			var rerr error
			for len(msgs) < totalMsgs {
				ev := <-seC.events
				if ev.err != nil {
					rerr = ev.err
					break
				}
				msgs = append(msgs, ev.msg)
				if len(msgs) == 1 {
					// Trigger an automatic Provide reply in the
					// middle of the concurrent user input.
					if _, err := seC.conn.Write(srvExt(wireActRequest|wireFormatText, nil)); err != nil {
						rerr = err
						break
					}
				}
			}
			results <- collectResult{msgs, rerr}
		}()

		errs := make(chan error, nKey+nPtr+nUpdate+nEnc+nPixFmt)
		var wg sync.WaitGroup
		for w := 0; w < workers; w++ {
			wg.Add(1)
			go func(w int) {
				defer wg.Done()
				for i := 0; i < perWorker; i++ {
					keysym := uint32(keysymBase + w*perWorker + i)
					if err := ccC.KeyEvent(keysym, true); err != nil {
						errs <- err
					}
				}
			}(w)
		}
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < nPtr; i++ {
				if err := ccC.PointerEvent(ButtonLeft, 1, 2); err != nil {
					errs <- err
				}
			}
		}()
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < nUpdate; i++ {
				if err := ccC.FramebufferUpdateRequest(true, 0, 0, 10, 10); err != nil {
					errs <- err
				}
			}
		}()
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < nEnc; i++ {
				if err := ccC.SetEncodings([]Encoding{new(RawEncoding)}); err != nil {
					errs <- err
				}
			}
		}()
		wg.Add(1)
		go func() {
			defer wg.Done()
			if err := ccC.SetPixelFormat(&PixelFormat{BPP: 32, Depth: 24, TrueColor: true,
				RedMax: 255, GreenMax: 255, BlueMax: 255, RedShift: 16, GreenShift: 8}); err != nil {
				errs <- err
			}
		}()
		wg.Wait()
		close(errs)
		for err := range errs {
			c.Check(false, "concurrent input failed: %v", err)
		}

		var res collectResult
		select {
		case res = <-results:
		case <-time.After(10 * time.Second):
			t.Fatal("timed out collecting the concurrent messages")
		}
		c.Check(res.err == nil, "collector read error: %v", res.err)
		c.Check(len(res.msgs) == totalMsgs,
			"collector got %d messages, want %d", len(res.msgs), totalMsgs)

		var gotKey, gotPtr, gotUpdate, gotEnc, gotPixFmt, gotProvide int
		torn := 0
		keysyms := make(map[uint32]int)
		for _, mm := range res.msgs {
			if len(mm.raw) != boundaryLen(mm) {
				torn++
			}
			switch mm.typ {
			case 4:
				gotKey++
				keysyms[mm.keysym]++
			case 5:
				gotPtr++
			case 3:
				gotUpdate++
			case 2:
				gotEnc++
			case 0:
				gotPixFmt++
			case 6:
				if mm.flags == wireActProvide|wireFormatText {
					gotProvide++
					body := inflateCheck(t, c, mm.data)
					c.Check(bytes.Equal(body, provideBody("sync")),
						"concurrent provide body = %x, want %x", body, provideBody("sync"))
				} else {
					torn++
				}
			default:
				torn++
			}
		}
		c.Check(torn == 0, "%d messages had torn boundaries", torn)
		c.Check(gotKey == nKey && gotPtr == nPtr && gotUpdate == nUpdate && gotEnc == nEnc && gotPixFmt == nPixFmt && gotProvide == nProvide,
			"message counts: key=%d ptr=%d update=%d enc=%d pixfmt=%d provide=%d, want %d/%d/%d/%d/%d/%d",
			gotKey, gotPtr, gotUpdate, gotEnc, gotPixFmt, gotProvide,
			nKey, nPtr, nUpdate, nEnc, nPixFmt, nProvide)
		allKeys := len(keysyms) == nKey
		for w := 0; w < workers; w++ {
			for i := 0; i < perWorker; i++ {
				if keysyms[uint32(keysymBase+w*perWorker+i)] != 1 {
					allKeys = false
				}
			}
		}
		c.Check(allKeys, "key events arrived torn or duplicated: %v", keysyms)

		// --- Short writes are completed; zero progress and write errors
		// are reported and close the connection.
		sc := newScriptConn(handshakeServerBytes())
		ccS, err := Client(sc, &ClientConfig{})
		if err != nil {
			t.Fatalf("Client over scriptConn: %v", err)
		}
		defer ccS.Close()
		sc.setMaxChunk(3)
		before := len(sc.writtenBytes())
		if err := ccS.KeyEvent(0x61, true); err != nil {
			t.Fatalf("short-write KeyEvent: %v", err)
		}
		gotB := sc.writtenBytes()[before:]
		c.Check(bytes.Equal(gotB, []byte{4, 1, 0, 0, 0, 0, 0, 0x61}),
			"short-write KeyEvent bytes = %x, want the complete 8-byte message", gotB)
		before = len(sc.writtenBytes())
		if err := ccS.CutText("hé"); err != nil {
			t.Fatalf("short-write CutText: %v", err)
		}
		gotB = sc.writtenBytes()[before:]
		c.Check(bytes.Equal(gotB, []byte{6, 0, 0, 0, 0, 0, 0, 2, 'h', 0xE9}),
			"short-write CutText bytes = %x, want the complete message", gotB)

		scZ := newScriptConn(handshakeServerBytes())
		ccZ, err := Client(scZ, &ClientConfig{})
		if err != nil {
			t.Fatalf("Client over scriptConn: %v", err)
		}
		defer ccZ.Close()
		scZ.setZeroProgress(true)
		err = ccZ.KeyEvent(0x61, true)
		c.Check(errors.Is(err, io.ErrShortWrite),
			"zero-progress write = %v, want io.ErrShortWrite", err)
		c.Check(scZ.isClosed(), "a zero-progress write must close the connection")

		sentinel := errors.New("boom")
		scE := newScriptConn(handshakeServerBytes())
		ccE, err := Client(scE, &ClientConfig{})
		if err != nil {
			t.Fatalf("Client over scriptConn: %v", err)
		}
		defer ccE.Close()
		scE.setWriteError(sentinel)
		err = ccE.KeyEvent(0x61, true)
		c.Check(errors.Is(err, sentinel),
			"failed write = %v, want the original error", err)
		c.Check(scE.isClosed(), "a failed write must close the connection")

		// --- Encs and clipboard ownership update only after the whole
		// message is written.
		scN := newScriptConn(handshakeServerBytes())
		ccN, err := Client(scN, &ClientConfig{ExtendedClipboard: true})
		if err != nil {
			t.Fatalf("Client over scriptConn: %v", err)
		}
		defer ccN.Close()
		c.Check(len(ccN.Encs) == 2 && ccN.Encs[1].Type() == testPseudoEnc,
			"Encs after the initial SetEncodings = %v, want [raw pseudo]", ccN.Encs)
		if err := ccN.SetEncodings([]Encoding{testEncoding{typ: 99}}); err != nil {
			t.Fatalf("SetEncodings: %v", err)
		}
		c.Check(len(ccN.Encs) == 2 && ccN.Encs[0].Type() == 99,
			"Encs after a successful SetEncodings = %v, want [99 pseudo]", ccN.Encs)
		scN.setWriteError(sentinel)
		err = ccN.SetEncodings([]Encoding{new(RawEncoding)})
		c.Check(errors.Is(err, sentinel), "failed SetEncodings = %v, want the original error", err)
		c.Check(len(ccN.Encs) == 2 && ccN.Encs[0].Type() == 99,
			"Encs changed after a failed SetEncodings: %v, want [99 pseudo]", ccN.Encs)

		// Ownership: a failed CutText must not take ownership, a
		// successful one must.
		scO := newScriptConn(handshakeServerBytes(fullServerCaps()))
		ccO, err := Client(scO, &ClientConfig{ExtendedClipboard: true})
		if err != nil {
			t.Fatalf("Client over scriptConn: %v", err)
		}
		defer ccO.Close()
		// version, security type, client init, initial SetEncodings, caps reply
		scO.waitWrites(t, 5)
		scO.setWriteError(sentinel)
		err = ccO.CutText("nope")
		c.Check(errors.Is(err, sentinel), "failed CutText = %v, want the original error", err)
		ccO.clipMu.Lock()
		ownAfterFail := ccO.clipOwnText
		ccO.clipMu.Unlock()
		c.Check(!ownAfterFail, "ownership must not be set after a failed CutText")

		scP := newScriptConn(handshakeServerBytes(fullServerCaps()))
		ccP, err := Client(scP, &ClientConfig{ExtendedClipboard: true})
		if err != nil {
			t.Fatalf("Client over scriptConn: %v", err)
		}
		defer ccP.Close()
		scP.waitWrites(t, 5)
		if err := ccP.CutText("ok"); err != nil {
			t.Fatalf("CutText: %v", err)
		}
		ccP.clipMu.Lock()
		ownAfterOK := ccP.clipOwnText
		localText := ccP.clipLocalText
		ccP.clipMu.Unlock()
		c.Check(ownAfterOK && localText == "ok",
			"ownership after a written CutText = %v/%q, want true/%q", ownAfterOK, localText, "ok")
	})
}
