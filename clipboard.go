package vnc

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
	"strings"
	"unicode/utf8"
)

// pseudoEncExtendedClipboard is the "Extended Clipboard" pseudo-encoding
// (0xC0A1E5CE) used to negotiate rich clipboard transfers.
const pseudoEncExtendedClipboard int32 = -1063131698 // 0xC0A1E5CE

// Extended clipboard flags: formats occupy the low 16 bits, actions bits
// 24-28.
const (
	clipFormatText uint32 = 1 << iota
)

const (
	clipActionCaps uint32 = 1 << (24 + iota)
	clipActionRequest
	clipActionPeek
	clipActionNotify
	clipActionProvide
)

const (
	clipFormatMask = 0x0000FFFF
	clipActionMask = 0x1F000000

	// maxClipboardPayload bounds a single extended clipboard payload, a
	// decompressed Provide body, and a legacy cut text body. The limit is
	// always checked before memory is allocated.
	maxClipboardPayload = 1 << 20
)

// extendedClipboardPseudoEncoding is a placeholder Encoding used to
// advertise the extended clipboard pseudo-encoding in SetEncodings.
type extendedClipboardPseudoEncoding struct{}

func (extendedClipboardPseudoEncoding) Type() int32 { return pseudoEncExtendedClipboard }

func (extendedClipboardPseudoEncoding) Read(*ClientConn, *Rectangle, io.Reader) (Encoding, error) {
	return nil, fmt.Errorf("vnc: extended clipboard pseudo-encoding carries no rectangle data")
}

// clipboardNegotiated reports whether the peer has declared support for
// text and the Request/Notify/Provide actions, enabling the extended
// clipboard flow on this connection.
func (c *ClientConn) clipboardNegotiated() bool {
	if !c.config.ExtendedClipboard {
		return false
	}

	c.clipMu.Lock()
	defer c.clipMu.Unlock()

	return c.clipPeerFormats&clipFormatText != 0 &&
		c.clipPeerActions&(clipActionRequest|clipActionNotify|clipActionProvide) ==
			clipActionRequest|clipActionNotify|clipActionProvide
}

// cutTextExtended announces local clipboard ownership with a Notify
// message; the text itself is only sent when the peer asks for it.
func (c *ClientConn) cutTextExtended(text string) error {
	if !utf8.ValidString(text) {
		return fmt.Errorf("vnc: clipboard text is not valid UTF-8")
	}
	if strings.IndexByte(text, 0) >= 0 {
		return fmt.Errorf("vnc: clipboard text contains an embedded NUL")
	}

	if err := c.writeMessage(buildExtendedClipboard(clipActionNotify|clipFormatText, nil)); err != nil {
		return err
	}

	// Take ownership only after the whole announcement has been written.
	c.clipMu.Lock()
	c.clipLocalText = text
	c.clipOwnText = true
	c.clipMu.Unlock()

	return nil
}

// buildExtendedClipboard builds an extended ClientCutText message: the
// regular header with a negative length whose absolute value covers the
// flags and the data.
func buildExtendedClipboard(flags uint32, data []byte) []byte {
	msg := make([]byte, 12+len(data))
	msg[0] = 6
	binary.BigEndian.PutUint32(msg[4:], uint32(-int32(4+len(data))))
	binary.BigEndian.PutUint32(msg[8:], flags)
	copy(msg[12:], data)

	return msg
}

// localCapsMessage is the extended clipboard Caps message this client
// sends in response to the peer's Caps: only text is supported, with all
// four actions, and unsolicited Provides are not accepted (size zero).
func localCapsMessage() []byte {
	return buildExtendedClipboard(
		clipActionCaps|clipActionRequest|clipActionPeek|clipActionNotify|clipActionProvide|clipFormatText,
		[]byte{0, 0, 0, 0})
}

// provideTextMessage builds a Provide message carrying the given text in
// wire form (UTF-8, CRLF line endings, NUL terminated).
func provideTextMessage(text string) []byte {
	wire := strings.ReplaceAll(text, "\n", "\r\n") + "\x00"
	raw := make([]byte, 4+len(wire))
	binary.BigEndian.PutUint32(raw, uint32(len(wire)))
	copy(raw[4:], wire)

	var compressed bytes.Buffer
	zw := zlib.NewWriter(&compressed)
	zw.Write(raw)
	zw.Close()

	return buildExtendedClipboard(clipActionProvide|clipFormatText, compressed.Bytes())
}

// readExtendedClipboard handles an extended (negative length) ServerCutText
// message. Control messages are handled internally and return a nil
// ServerMessage; a valid Provide of text is returned as a
// ServerCutTextMessage so it can be delivered on the usual channel.
func (c *ClientConn) readExtendedClipboard(negLen int32, r io.Reader) (ServerMessage, error) {
	size := -int64(negLen)
	if size < 4 || size > maxClipboardPayload {
		return nil, fmt.Errorf("vnc: invalid extended clipboard payload size %d", size)
	}

	payload := make([]byte, size)
	if _, err := io.ReadFull(r, payload); err != nil {
		return nil, err
	}

	flags := binary.BigEndian.Uint32(payload[:4])
	data := payload[4:]
	actions := flags & clipActionMask
	formats := flags & clipFormatMask

	if actions == 0 {
		return nil, fmt.Errorf("vnc: extended clipboard message has no action (flags %#x)", flags)
	}

	// A Caps message may carry several action bits; every other message
	// must carry exactly one.
	if actions&clipActionCaps != 0 {
		return nil, c.handleClipboardCaps(actions, formats, data)
	}
	if actions&(actions-1) != 0 {
		return nil, fmt.Errorf("vnc: extended clipboard message has multiple actions (flags %#x)", flags)
	}

	switch actions {
	case clipActionRequest:
		return nil, c.handleClipboardRequest(formats, data)
	case clipActionPeek:
		return nil, c.handleClipboardPeek(formats, data)
	case clipActionNotify:
		return nil, c.handleClipboardNotify(formats, data)
	case clipActionProvide:
		return c.handleClipboardProvide(formats, data)
	default:
		return nil, fmt.Errorf("vnc: unknown extended clipboard action (flags %#x)", flags)
	}
}

// handleClipboardCaps records the peer's formats and actions, replacing
// whatever it declared before, and answers with the local Caps.
func (c *ClientConn) handleClipboardCaps(actions, formats uint32, data []byte) error {
	// Each advertised format is followed by a big-endian uint32 with the
	// maximum unsolicited Provide size, in format-bit order from low to
	// high. Sizes of formats we do not understand must be consumed as
	// well so they do not leak into the next message.
	offset := 0
	for bit := uint(0); bit < 16; bit++ {
		if formats&(1<<bit) == 0 {
			continue
		}
		if offset+4 > len(data) {
			return fmt.Errorf("vnc: extended clipboard caps truncated: missing size for format bit %d", bit)
		}
		offset += 4
	}
	if offset != len(data) {
		return fmt.Errorf("vnc: extended clipboard caps has %d trailing bytes", len(data)-offset)
	}

	c.clipMu.Lock()
	c.clipPeerFormats = formats
	c.clipPeerActions = actions
	c.clipMu.Unlock()

	return c.writeMessage(localCapsMessage())
}

// handleClipboardRequest answers a peer's Request with a Provide of the
// local text, or with an empty Notify when there is nothing to provide.
func (c *ClientConn) handleClipboardRequest(formats uint32, data []byte) error {
	if len(data) != 0 {
		return fmt.Errorf("vnc: extended clipboard request has %d trailing bytes", len(data))
	}

	c.clipMu.Lock()
	ownText := c.clipOwnText
	text := c.clipLocalText
	c.clipMu.Unlock()

	if ownText && formats&clipFormatText != 0 {
		return c.writeMessage(provideTextMessage(text))
	}

	return c.writeMessage(buildExtendedClipboard(clipActionNotify, nil))
}

// handleClipboardPeek answers a Peek with a Notify listing the formats
// currently owned locally.
func (c *ClientConn) handleClipboardPeek(formats uint32, data []byte) error {
	if len(data) != 0 {
		return fmt.Errorf("vnc: extended clipboard peek has %d trailing bytes", len(data))
	}

	c.clipMu.Lock()
	ownText := c.clipOwnText
	c.clipMu.Unlock()

	var have uint32
	if ownText {
		have = clipFormatText
	}

	return c.writeMessage(buildExtendedClipboard(clipActionNotify|have, nil))
}

// handleClipboardNotify records that the peer owns the clipboard now and
// requests the text if the peer has any.
func (c *ClientConn) handleClipboardNotify(formats uint32, data []byte) error {
	if len(data) != 0 {
		return fmt.Errorf("vnc: extended clipboard notify has %d trailing bytes", len(data))
	}

	c.clipMu.Lock()
	c.clipOwnText = false
	c.clipMu.Unlock()

	if formats&clipFormatText != 0 {
		return c.writeMessage(buildExtendedClipboard(clipActionRequest|clipFormatText, nil))
	}

	return nil
}

// handleClipboardProvide unpacks a Provide message. Every advertised
// format is parsed in format-bit order; only text is published, and a
// published text means the peer owns the clipboard now.
func (c *ClientConn) handleClipboardProvide(formats uint32, data []byte) (ServerMessage, error) {
	raw, err := inflateClipboardData(data)
	if err != nil {
		return nil, err
	}

	var text string
	haveText := false
	offset := 0
	for bit := uint(0); bit < 16; bit++ {
		if formats&(1<<bit) == 0 {
			continue
		}
		if offset+4 > len(raw) {
			return nil, fmt.Errorf("vnc: extended clipboard provide truncated: missing length for format bit %d", bit)
		}
		n := binary.BigEndian.Uint32(raw[offset:])
		offset += 4
		if uint64(n) > uint64(len(raw)-offset) {
			return nil, fmt.Errorf("vnc: extended clipboard provide format bit %d length %d exceeds the remaining data", bit, n)
		}
		entry := raw[offset : offset+int(n)]
		offset += int(n)

		if bit == 0 {
			text, err = normalizeClipboardText(entry)
			if err != nil {
				return nil, err
			}
			haveText = true
		}
		// Unknown formats are fully parsed above but never published.
	}
	if offset != len(raw) {
		return nil, fmt.Errorf("vnc: extended clipboard provide has %d trailing bytes", len(raw)-offset)
	}

	if !haveText {
		return nil, nil
	}

	// The peer owns the clipboard now; the received text must not be
	// echoed back as if it had been copied locally.
	c.clipMu.Lock()
	c.clipOwnText = false
	c.clipMu.Unlock()

	return &ServerCutTextMessage{Text: text}, nil
}

// inflateClipboardData decompresses one complete zlib stream, rejecting
// corrupt streams, trailing garbage, and results over the size limit.
func inflateClipboardData(data []byte) ([]byte, error) {
	br := bytes.NewReader(data)
	zr, err := zlib.NewReader(br)
	if err != nil {
		return nil, fmt.Errorf("vnc: extended clipboard provide: %v", err)
	}

	raw, err := io.ReadAll(io.LimitReader(zr, maxClipboardPayload+1))
	if err != nil {
		zr.Close()
		return nil, fmt.Errorf("vnc: extended clipboard provide: %v", err)
	}
	if err := zr.Close(); err != nil {
		return nil, fmt.Errorf("vnc: extended clipboard provide: %v", err)
	}
	if len(raw) > maxClipboardPayload {
		return nil, fmt.Errorf("vnc: extended clipboard provide exceeds %d bytes", maxClipboardPayload)
	}
	if br.Len() != 0 {
		return nil, fmt.Errorf("vnc: extended clipboard provide has %d bytes after the zlib stream", br.Len())
	}

	return raw, nil
}

// normalizeClipboardText converts wire clipboard text (UTF-8, CRLF line
// endings, NUL terminated) into the local form (LF line endings).
func normalizeClipboardText(b []byte) (string, error) {
	if len(b) == 0 || b[len(b)-1] != 0 {
		return "", fmt.Errorf("vnc: extended clipboard text is missing its NUL terminator")
	}
	b = b[:len(b)-1]
	if bytes.IndexByte(b, 0) >= 0 {
		return "", fmt.Errorf("vnc: extended clipboard text contains an embedded NUL")
	}
	if !utf8.Valid(b) {
		return "", fmt.Errorf("vnc: extended clipboard text is not valid UTF-8")
	}

	return strings.ReplaceAll(string(b), "\r\n", "\n"), nil
}

// latin1ToString converts legacy cut buffer bytes to a Go string, one
// Latin-1 character per byte.
func latin1ToString(b []byte) string {
	var sb strings.Builder
	sb.Grow(len(b))
	for _, c := range b {
		sb.WriteRune(rune(c))
	}

	return sb.String()
}
