// Package vnc implements a VNC client.
//
// References:
//
//	[PROTOCOL]: http://tools.ietf.org/html/rfc6143
package vnc

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"sync"
	"unicode"
)

type ClientConn struct {
	c      net.Conn
	config *ClientConfig

	// writeMu serializes writes to the connection so that concurrent
	// senders (user calls and the automatic protocol replies produced by
	// the receive loop) never interleave bytes from different messages.
	writeMu sync.Mutex

	// clipMu guards the extended clipboard negotiation and ownership
	// state below.
	clipMu          sync.Mutex
	clipPeerFormats uint32
	clipPeerActions uint32
	clipOwnText     bool
	clipLocalText   string

	// If the pixel format uses a color map, then this is the color
	// map that is used. This should not be modified directly, since
	// the data comes from the server.
	ColorMap [256]Color

	// Encodings supported by the client. This should not be modified
	// directly. Instead, SetEncodings should be used.
	Encs []Encoding

	// Width of the frame buffer in pixels, sent from the server.
	FrameBufferWidth uint16

	// Height of the frame buffer in pixels, sent from the server.
	FrameBufferHeight uint16

	// Name associated with the desktop, sent from the server.
	DesktopName string

	// The pixel format associated with the connection. This shouldn't
	// be modified. If you wish to set a new pixel format, use the
	// SetPixelFormat method.
	PixelFormat PixelFormat
}

// A ClientConfig structure is used to configure a ClientConn. After
// one has been passed to initialize a connection, it must not be modified.
type ClientConfig struct {
	// A slice of ClientAuth methods. Only the first instance that is
	// suitable by the server will be used to authenticate.
	Auth []ClientAuth

	// Exclusive determines whether the connection is shared with other
	// clients. If true, then all other clients connected will be
	// disconnected when a connection is established to the VNC server.
	Exclusive bool

	// The channel that all messages received from the server will be
	// sent on. If the channel blocks, then the goroutine reading data
	// from the VNC server may block indefinitely. It is up to the user
	// of the library to ensure that this channel is properly read.
	// If this is not set, then all messages will be discarded.
	ServerMessageCh chan<- ServerMessage

	// A slice of supported messages that can be read from the server.
	// This only needs to contain NEW server messages, and doesn't
	// need to explicitly contain the RFC-required messages.
	ServerMessages []ServerMessage

	// ExtendedClipboard enables the Extended Clipboard pseudo-encoding
	// (0xC0A1E5CE) on this connection. When enabled, the client
	// advertises the pseudo-encoding right after the handshake and
	// transparently handles the extended clipboard control messages, so
	// CutText and ServerCutTextMessage can carry UTF-8 text. When
	// disabled, no extra messages are sent and negative-length CutText
	// messages from the server are rejected instead of enabling the
	// extension.
	ExtendedClipboard bool
}

func Client(c net.Conn, cfg *ClientConfig) (*ClientConn, error) {
	conn := &ClientConn{
		c:      c,
		config: cfg,
	}

	if err := conn.handshake(); err != nil {
		conn.Close()
		return nil, err
	}

	if cfg.ExtendedClipboard {
		// Advertise the extended clipboard pseudo-encoding before any
		// other client message so the server can negotiate clipboard
		// formats as soon as it is ready.
		if err := conn.SetEncodings([]Encoding{new(RawEncoding)}); err != nil {
			conn.Close()
			return nil, err
		}
	}

	go conn.mainLoop()

	return conn, nil
}

func (c *ClientConn) Close() error {
	return c.c.Close()
}

// writeMessage writes one complete protocol message. Writes are serialized
// with every other message on the connection, so a message is never torn
// apart by another sender. A failure closes the connection, since the peer
// may already have seen a partial message.
func (c *ClientConn) writeMessage(p []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()

	if err := writeFull(c.c, p); err != nil {
		c.c.Close()
		return err
	}

	return nil
}

// writeFull writes all of p, completing legitimate short writes. A write
// that makes no progress is reported as io.ErrShortWrite; any other error
// is returned unchanged.
func writeFull(w io.Writer, p []byte) error {
	for len(p) > 0 {
		n, err := w.Write(p)
		if err != nil {
			return err
		}
		if n <= 0 {
			return io.ErrShortWrite
		}
		p = p[n:]
	}

	return nil
}

// CutText tells the server that the client has new text in its cut buffer.
//
// If the extended clipboard has been negotiated on this connection (see
// ClientConfig.ExtendedClipboard), the text may be any valid UTF-8 string
// without embedded NUL characters; the client announces ownership of the
// clipboard and provides the text when the server requests it.
//
// Otherwise the legacy ClientCutText message is used and the text string
// MUST only contain Latin-1 characters. This encoding is compatible with
// Go's native string format, but can only use up to unicode.MaxLatin1
// values.
//
// See RFC 6143 Section 7.5.6
func (c *ClientConn) CutText(text string) error {
	if c.clipboardNegotiated() {
		return c.cutTextExtended(text)
	}

	return c.cutTextLegacy(text)
}

// cutTextLegacy sends the text as a legacy Latin-1 ClientCutText message.
// The text is validated and converted before anything is written.
func (c *ClientConn) cutTextLegacy(text string) error {
	latin1 := make([]byte, 0, len(text))
	for _, char := range text {
		if char > unicode.MaxLatin1 {
			return fmt.Errorf("Character %U is not valid Latin-1", char)
		}

		latin1 = append(latin1, uint8(char))
	}

	msg := make([]byte, 8+len(latin1))
	msg[0] = 6
	binary.BigEndian.PutUint32(msg[4:], uint32(len(latin1)))
	copy(msg[8:], latin1)

	return c.writeMessage(msg)
}

// Requests a framebuffer update from the server. There may be an indefinite
// time between the request and the actual framebuffer update being
// received.
//
// See RFC 6143 Section 7.5.3
func (c *ClientConn) FramebufferUpdateRequest(incremental bool, x, y, width, height uint16) error {
	var msg [10]byte
	msg[0] = 3
	if incremental {
		msg[1] = 1
	}
	binary.BigEndian.PutUint16(msg[2:], x)
	binary.BigEndian.PutUint16(msg[4:], y)
	binary.BigEndian.PutUint16(msg[6:], width)
	binary.BigEndian.PutUint16(msg[8:], height)

	return c.writeMessage(msg[:])
}

// KeyEvent indiciates a key press or release and sends it to the server.
// The key is indicated using the X Window System "keysym" value. Use
// Google to find a reference of these values. To simulate a key press,
// you must send a key with both a down event, and a non-down event.
//
// See 7.5.4.
func (c *ClientConn) KeyEvent(keysym uint32, down bool) error {
	var msg [8]byte
	msg[0] = 4
	if down {
		msg[1] = 1
	}
	binary.BigEndian.PutUint32(msg[4:], keysym)

	return c.writeMessage(msg[:])
}

// PointerEvent indicates that pointer movement or a pointer button
// press or release.
//
// The mask is a bitwise mask of various ButtonMask values. When a button
// is set, it is pressed, when it is unset, it is released.
//
// See RFC 6143 Section 7.5.5
func (c *ClientConn) PointerEvent(mask ButtonMask, x, y uint16) error {
	var msg [6]byte
	msg[0] = 5
	msg[1] = uint8(mask)
	binary.BigEndian.PutUint16(msg[2:], x)
	binary.BigEndian.PutUint16(msg[4:], y)

	return c.writeMessage(msg[:])
}

// SetEncodings sets the encoding types in which the pixel data can
// be sent from the server. After calling this method, the encs slice
// given should not be modified.
//
// When the extended clipboard is enabled, the pseudo-encoding is added
// to the message if the caller did not include it, exactly once, without
// reordering or modifying the caller's slice.
//
// See RFC 6143 Section 7.5.2
func (c *ClientConn) SetEncodings(encs []Encoding) error {
	send := encs
	if c.config.ExtendedClipboard {
		send = make([]Encoding, 0, len(encs)+1)
		havePseudo := false
		for _, enc := range encs {
			if enc.Type() == pseudoEncExtendedClipboard {
				// The pseudo-encoding must appear exactly once.
				if havePseudo {
					continue
				}
				havePseudo = true
			}
			send = append(send, enc)
		}
		if !havePseudo {
			send = append(send, extendedClipboardPseudoEncoding{})
		}
	}

	msg := make([]byte, 4+4*len(send))
	msg[0] = 2
	binary.BigEndian.PutUint16(msg[2:], uint16(len(send)))
	for i, enc := range send {
		binary.BigEndian.PutUint32(msg[4+4*i:], uint32(enc.Type()))
	}

	if err := c.writeMessage(msg); err != nil {
		return err
	}

	// Update the connection's encoding list only once the whole message
	// has been written.
	c.Encs = send

	return nil
}

// SetPixelFormat sets the format in which pixel values should be sent
// in FramebufferUpdate messages from the server.
//
// See RFC 6143 Section 7.5.1
func (c *ClientConn) SetPixelFormat(format *PixelFormat) error {
	var msg [20]byte
	msg[0] = 0

	pfBytes, err := writePixelFormat(format)
	if err != nil {
		return err
	}

	// Copy the pixel format bytes into the proper slice location
	copy(msg[4:], pfBytes)

	// Send the data down the connection
	if err := c.writeMessage(msg[:]); err != nil {
		return err
	}

	// Reset the color map as according to RFC.
	var newColorMap [256]Color
	c.ColorMap = newColorMap

	return nil
}

const pvLen = 12 // ProtocolVersion message length.

func parseProtocolVersion(pv []byte) (uint, uint, error) {
	var major, minor uint

	if len(pv) < pvLen {
		return 0, 0, fmt.Errorf("ProtocolVersion message too short (%v < %v)", len(pv), pvLen)
	}

	l, err := fmt.Sscanf(string(pv), "RFB %d.%d\n", &major, &minor)
	if l != 2 {
		return 0, 0, fmt.Errorf("error parsing ProtocolVersion.")
	}
	if err != nil {
		return 0, 0, err
	}

	return major, minor, nil
}

func (c *ClientConn) handshake() error {
	var protocolVersion [pvLen]byte

	// 7.1.1, read the ProtocolVersion message sent by the server.
	if _, err := io.ReadFull(c.c, protocolVersion[:]); err != nil {
		return err
	}

	maxMajor, maxMinor, err := parseProtocolVersion(protocolVersion[:])
	if err != nil {
		return err
	}
	if maxMajor < 3 {
		return fmt.Errorf("unsupported major version, less than 3: %d", maxMajor)
	}
	if maxMinor < 8 {
		return fmt.Errorf("unsupported minor version, less than 8: %d", maxMinor)
	}

	// Respond with the version we will support
	if _, err = c.c.Write([]byte("RFB 003.008\n")); err != nil {
		return err
	}

	// 7.1.2 Security Handshake from server
	var numSecurityTypes uint8
	if err = binary.Read(c.c, binary.BigEndian, &numSecurityTypes); err != nil {
		return err
	}

	if numSecurityTypes == 0 {
		return fmt.Errorf("no security types: %s", c.readErrorReason())
	}

	securityTypes := make([]uint8, numSecurityTypes)
	if err = binary.Read(c.c, binary.BigEndian, &securityTypes); err != nil {
		return err
	}

	clientSecurityTypes := c.config.Auth
	if clientSecurityTypes == nil {
		clientSecurityTypes = []ClientAuth{new(ClientAuthNone)}
	}

	var auth ClientAuth
FindAuth:
	for _, curAuth := range clientSecurityTypes {
		for _, securityType := range securityTypes {
			if curAuth.SecurityType() == securityType {
				// We use the first matching supported authentication
				auth = curAuth
				break FindAuth
			}
		}
	}

	if auth == nil {
		return fmt.Errorf("no suitable auth schemes found. server supported: %#v", securityTypes)
	}

	// Respond back with the security type we'll use
	if err = binary.Write(c.c, binary.BigEndian, auth.SecurityType()); err != nil {
		return err
	}

	if err = auth.Handshake(c.c); err != nil {
		return err
	}

	// 7.1.3 SecurityResult Handshake
	var securityResult uint32
	if err = binary.Read(c.c, binary.BigEndian, &securityResult); err != nil {
		return err
	}

	if securityResult == 1 {
		return fmt.Errorf("security handshake failed: %s", c.readErrorReason())
	}

	// 7.3.1 ClientInit
	var sharedFlag uint8 = 1
	if c.config.Exclusive {
		sharedFlag = 0
	}

	if err = binary.Write(c.c, binary.BigEndian, sharedFlag); err != nil {
		return err
	}

	// 7.3.2 ServerInit
	if err = binary.Read(c.c, binary.BigEndian, &c.FrameBufferWidth); err != nil {
		return err
	}

	if err = binary.Read(c.c, binary.BigEndian, &c.FrameBufferHeight); err != nil {
		return err
	}

	// Read the pixel format
	if err = readPixelFormat(c.c, &c.PixelFormat); err != nil {
		return err
	}

	var nameLength uint32
	if err = binary.Read(c.c, binary.BigEndian, &nameLength); err != nil {
		return err
	}

	nameBytes := make([]uint8, nameLength)
	if err = binary.Read(c.c, binary.BigEndian, &nameBytes); err != nil {
		return err
	}

	c.DesktopName = string(nameBytes)

	return nil
}

// mainLoop reads messages sent from the server and routes them to the
// proper channels for users of the client to read.
func (c *ClientConn) mainLoop() {
	defer c.Close()

	// Build the map of available server messages
	typeMap := make(map[uint8]ServerMessage)

	defaultMessages := []ServerMessage{
		new(FramebufferUpdateMessage),
		new(SetColorMapEntriesMessage),
		new(BellMessage),
		new(ServerCutTextMessage),
	}

	for _, msg := range defaultMessages {
		typeMap[msg.Type()] = msg
	}

	if c.config.ServerMessages != nil {
		for _, msg := range c.config.ServerMessages {
			typeMap[msg.Type()] = msg
		}
	}

	for {
		var messageType uint8
		if err := binary.Read(c.c, binary.BigEndian, &messageType); err != nil {
			break
		}

		msg, ok := typeMap[messageType]
		if !ok {
			// Unsupported message type! Bad!
			break
		}

		parsedMsg, err := msg.Read(c, c.c)
		if err != nil {
			break
		}

		// Extended clipboard control messages are handled internally
		// and produce no message to publish.
		if parsedMsg == nil {
			continue
		}

		if c.config.ServerMessageCh == nil {
			continue
		}

		c.config.ServerMessageCh <- parsedMsg
	}
}

func (c *ClientConn) readErrorReason() string {
	var reasonLen uint32
	if err := binary.Read(c.c, binary.BigEndian, &reasonLen); err != nil {
		return "<error>"
	}

	reason := make([]uint8, reasonLen)
	if err := binary.Read(c.c, binary.BigEndian, &reason); err != nil {
		return "<error>"
	}

	return string(reason)
}
