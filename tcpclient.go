package gos7

// Copyright 2018 Trung Hieu Le. All rights reserved.
// This software may be modified and distributed under the terms
// of the BSD license. See the LICENSE file for details.
import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	// Default TCP timeout is not set
	tcpTimeout     = 10 * time.Second
	tcpIdleTimeout = 60 * time.Second
	tcpMaxLength   = 2084
	//messages
	pduSizeRequested = 480
	isoTCP           = 102 //default isotcp port
	isoHSize         = 7   // TPKT+COTP Header Size
	// Client Connection Type
	connectionTypePG    = 1 // Connect to the PLC as a PG
	connectionTypeOP    = 2 // Connect to the PLC as an OP
	connectionTypeBasic = 3 // Basic connection
	// S7 PDU header behind TPKT and COTP
	s7ProtocolID   = 0x32 // First byte of every S7 PDU
	s7PDURefOffset = 11   // PDU reference, 2 bytes big endian, echoed by the PLC
	// COTP (ISO 8073) TPDU types and flags
	cotpDataTPDU    = 0xF0 // DT Data
	cotpConfirmTPDU = 0xD0 // CC Connection Confirm
	cotpEOT         = 0x80 // End of TSDU: last DT fragment of a telegram
	cotpSizeParam   = 0xC0 // TPDU-size parameter of the connection confirm
	// The TPDU size offered in the connection request and used when the
	// confirm names none. It covers the whole COTP TPDU, the 3 byte DT
	// header included, but not the 4 byte TPKT header.
	cotpDefaultTPDUSize = 1024
	cotpMinTPDUExponent = 7  // 128 bytes
	cotpMaxTPDUExponent = 10 // 1024 bytes, the offer of the connection request
)

// TCPClientHandler implements Packager and Transporter interface.
type TCPClientHandler struct {
	tcpPackager
	tcpTransporter
}

// NewTCPClientHandler allocates a new TCPClientHandler.
func NewTCPClientHandler(address string, rack int, slot int) *TCPClientHandler {
	h := &TCPClientHandler{}
	h.Address = address
	h.Timeout = tcpTimeout
	h.IdleTimeout = tcpIdleTimeout
	h.ConnectionType = connectionTypePG // Connect to the PLC as a PG
	remoteTSAP := uint16(h.ConnectionType)<<8 + (uint16(rack) * 0x20) + uint16(slot)
	h.setConnectionParameters(address, 0x0100, remoteTSAP)
	return h
}

// NewTCPClientHandlerWithConnectType allocates a new TCPClientHandler with connection type.
func NewTCPClientHandlerWithConnectType(address string, rack int, slot int, connectType int) *TCPClientHandler {
	h := &TCPClientHandler{}
	h.Address = address
	h.Timeout = tcpTimeout
	h.IdleTimeout = tcpIdleTimeout
	h.ConnectionType = connectType
	remoteTSAP := uint16(h.ConnectionType)<<8 + (uint16(rack) * 0x20) + uint16(slot)
	h.setConnectionParameters(address, 0x0100, remoteTSAP)
	return h
}

// TCPClient creator for a TCP client with address, rack and slot, implement from interface client
func TCPClient(address string, rack int, slot int) Client {
	handler := NewTCPClientHandler(address, rack, slot)
	return NewClient(handler)
}

// TCPClientWithConnectType creator for a TCP client with address, rack, slot and connect type, implement from interface client
func TCPClientWithConnectType(address string, rack int, slot int, connectType int) Client {
	handler := NewTCPClientHandlerWithConnectType(address, rack, slot, connectType)
	return NewClient(handler)
}

// tcpPackager implements Packager interface. Its Verify matches the response
// to the request by the S7 PDU reference.
type tcpPackager struct{}

// tcpTransporter implements Transporter interface.
type tcpTransporter struct {
	// Connect string
	Address string
	// Connect & Read timeout
	Timeout time.Duration
	// Idle timeout to close the connection
	IdleTimeout time.Duration
	// Transmission logger
	Logger *log.Logger

	// TCP connection
	mu           sync.Mutex
	conn         net.Conn
	closeTimer   *time.Timer
	lastActivity time.Time

	localTSAP, remoteTSAP uint16

	localTSAPHigh, localTSAPLow   byte
	remoteTSAPHigh, remoteTSAPLow byte
	ConnectionType                int
	LastPDUType                   byte

	// PDULength is the S7 layer negotiation result: the number of S7
	// protocol bytes one PDU may carry.
	PDULength int
	// tpduSize is the COTP TPDU size negotiated during ISO connect. It
	// bounds every DT fragment on the wire and is independent of the S7
	// PDULength. Zero means the 1024 bytes offered in the connection
	// request. It belongs to one connection and is never carried over
	// to the next.
	tpduSize int
}

func (mb *tcpTransporter) setConnectionParameters(address string, localTSAP uint16, remoteTSAP uint16) {
	locTSAP := localTSAP & 0x0000FFFF
	remTSAP := remoteTSAP & 0x0000FFFF
	if len(strings.Split(address, ":")) < 2 {
		mb.Address = address + ":" + strconv.Itoa(isoTCP) //ip:102
	} else {
		mb.Address = address
	}
	mb.localTSAPHigh = byte(locTSAP >> 8)
	mb.localTSAPLow = byte(locTSAP & 0x00FF)
	mb.remoteTSAPHigh = byte(remTSAP >> 8)
	mb.remoteTSAPLow = byte(remTSAP & 0x00FF)
}

// Send sends data to server and ensures response length is greater than header length.
func (mb *tcpTransporter) Send(request []byte) (response []byte, err error) {
	mb.mu.Lock()
	defer mb.mu.Unlock()
	// Set timer to close when idle
	mb.lastActivity = time.Now()
	mb.startCloseTimer()
	// One deadline covers the whole exchange: every fragment of the
	// request and of the response shares the timeout of this Send, it is
	// not renewed when a fragment arrives.
	var timeout time.Time
	if mb.Timeout > 0 {
		timeout = mb.lastActivity.Add(mb.Timeout)
	}
	if mb.conn == nil {
		err = fmt.Errorf("Connection to address %s is null", mb.Address)
		return
	}
	if err = mb.conn.SetDeadline(timeout); err != nil {
		return
	}
	// Send data, split into as many DT fragments as the negotiated COTP
	// TPDU size requires
	mb.logf("s7: sending % x", request)
	if err = mb.sendFragments(request); err != nil {
		return nil, err
	}
	for {
		if response, err = mb.receiveTSDU(); err != nil {
			// The telegram boundaries on the wire can no longer be told
			// apart, so this connection is done; the caller has to
			// connect again explicitly.
			mb.close()
			return nil, err
		}
		mb.logf("s7: received % x\n", response)
		// A reply carrying another PDU reference answers an earlier request,
		// typically one that timed out. Taking it for the answer to this
		// request would hand the caller foreign data, so drop it and keep
		// waiting for the matching reply until the deadline expires. Only
		// complete TSDUs reach this check, never a fragment of one.
		if !pduReferenceMismatch(request, response) {
			return response, nil
		}
		mb.logf("s7: discarding reply with PDU reference 0x%04x, request has 0x%04x", pduReference(response), pduReference(request))
	}
}

// sendFragments writes request to the wire. The S7 bytes behind the 7 byte
// TPKT/COTP shell of a DT telegram are split into as many TPDUs as the
// negotiated TPDU size needs. Every fragment gets its own TPKT header with
// the length of that fragment, the COTP header is 02 F0 and only the last
// fragment sets the EOT flag; a payload that fits exactly gets no empty
// tail fragment. The request buffer is not modified.
func (mb *tcpTransporter) sendFragments(request []byte) error {
	// Anything but a DT telegram with a payload (the connection request,
	// say) always fits one TPDU and goes out unchanged.
	if len(request) <= isoHSize || request[4] != 2 || request[5] != cotpDataTPDU {
		return mb.writeFull(request)
	}
	payload := request[isoHSize:]
	maxData := mb.tpduSizeLimit() - 3 // the 3 byte DT header is part of the TPDU
	for offset := 0; offset < len(payload); {
		chunk := len(payload) - offset
		if chunk > maxData {
			chunk = maxData
		}
		offset += chunk
		frame := make([]byte, isoHSize+chunk)
		frame[0] = 3
		binary.BigEndian.PutUint16(frame[2:], uint16(len(frame)))
		frame[4], frame[5] = 2, cotpDataTPDU
		if offset == len(payload) {
			frame[6] = cotpEOT // only the last fragment ends the TSDU
		}
		copy(frame[isoHSize:], payload[offset-chunk:offset])
		if err := mb.writeFull(frame); err != nil {
			return err
		}
	}
	return nil
}

// writeFull writes all of frame, however the underlying connection shortens
// the writes. A write that fails or makes no progress is an error.
func (mb *tcpTransporter) writeFull(frame []byte) error {
	for len(frame) > 0 {
		n, err := mb.conn.Write(frame)
		if err != nil {
			return err
		}
		if n <= 0 {
			return fmt.Errorf("s7: connection write of %d bytes made no progress", len(frame))
		}
		frame = frame[n:]
	}
	return nil
}

// receiveTSDU reads DT fragments and joins their user data until the EOT
// flag ends the TSDU. What returns is one telegram for the S7 layer: the
// whole payload behind a single TPKT/COTP header 02 F0 80 whose length
// field holds the logical total. Standalone empty DT telegrams are
// keepalives and are skipped. Every protocol violation is an error; the
// caller closes the connection because the telegram boundaries can no
// longer be determined.
func (mb *tcpTransporter) receiveTSDU() ([]byte, error) {
	// The S7 layer accepts as many reassembled payload bytes as the
	// negotiated PDU length, or the requested 480 before the negotiation.
	limit := mb.PDULength
	if limit <= 0 {
		limit = pduSizeRequested
	}
	var payload []byte
	for {
		tpdu, err := mb.readTPKT()
		if err != nil {
			return nil, err
		}
		if tpdu[0] != 2 || tpdu[1] != cotpDataTPDU {
			return nil, fmt.Errorf("s7: unexpected COTP TPDU % x, want DT", tpdu)
		}
		if size := len(tpdu); size > mb.tpduSizeLimit() {
			return nil, fmt.Errorf("s7: TPDU of %d bytes exceeds the negotiated %d", size, mb.tpduSizeLimit())
		}
		fragment, eot := tpdu[3:], tpdu[2]&cotpEOT != 0
		if len(fragment) == 0 {
			if len(payload) == 0 {
				continue // standalone empty DT: a keepalive, skip it
			}
			if !eot {
				return nil, fmt.Errorf("s7: empty DT fragment without EOT makes no progress")
			}
			break // an empty EOT fragment ends the TSDU
		}
		payload = append(payload, fragment...)
		if len(payload) > limit {
			return nil, fmt.Errorf("s7: reassembled S7 PDU of %d bytes exceeds the limit of %d", len(payload), limit)
		}
		if eot {
			break
		}
	}
	telegram := make([]byte, isoHSize+len(payload))
	telegram[0] = 3
	binary.BigEndian.PutUint16(telegram[2:], uint16(len(telegram)))
	telegram[4], telegram[5], telegram[6] = 2, cotpDataTPDU, cotpEOT
	copy(telegram[isoHSize:], payload)
	return telegram, nil
}

// readTPKT reads one TPKT frame from the wire and returns the COTP TPDU it
// carries. The whole declared frame is consumed, however many TCP reads it
// takes, so the next read starts exactly at the next frame.
func (mb *tcpTransporter) readTPKT() ([]byte, error) {
	header := make([]byte, 4)
	if _, err := io.ReadFull(mb.conn, header); err != nil {
		return nil, err
	}
	if header[0] != 3 {
		return nil, fmt.Errorf("s7: invalid TPKT version %d", header[0])
	}
	length := int(binary.BigEndian.Uint16(header[2:]))
	if length < isoHSize || length > tcpMaxLength {
		return nil, fmt.Errorf("s7: invalid TPKT length %d", length)
	}
	tpdu := make([]byte, length-4)
	if _, err := io.ReadFull(mb.conn, tpdu); err != nil {
		return nil, err
	}
	mb.LastPDUType = tpdu[1] // Stores PDU Type, we need it
	return tpdu, nil
}

// tpduSizeLimit is the COTP TPDU size negotiated for this connection, or
// the 1024 bytes offered in the connection request when no negotiation
// happened (yet). It bounds one TPDU on the wire, DT header included.
func (mb *tcpTransporter) tpduSizeLimit() int {
	if mb.tpduSize <= 0 {
		return cotpDefaultTPDUSize
	}
	return mb.tpduSize
}

// Connect establishes a new connection to the address in Address.
// Connect and Close are exported so that multiple requests can be done with one session
func (mb *tcpTransporter) Connect() error {
	return mb.ConnectContext(context.Background())
}

// ConnectContext establishes a new connection to the address in Address.
// It is the same as Connect but accepts a context that can be used to
// cancel or set a deadline on the TCP dial and subsequent protocol handshake.
func (mb *tcpTransporter) ConnectContext(ctx context.Context) error {
	//first stage: TCP connection
	err := mb.tcpConnect(ctx)
	if err != nil {
		return err
	}
	//second stage: ISOTCP (ISO 8073) Connection
	err = mb.isoConnect()
	if err != nil {
		mb.Close()
		return err
	}
	// Third stage : S7 protocol data unit negotiation
	return mb.negotiatePduLength()
}

func (mb *tcpTransporter) tcpConnect(ctx context.Context) error {
	mb.mu.Lock()
	defer mb.mu.Unlock()
	if mb.conn == nil {
		dialer := net.Dialer{Timeout: mb.Timeout}
		conn, err := dialer.DialContext(ctx, "tcp", mb.Address)
		if err != nil {
			if conn != nil {
				_ = conn.Close()
			}
			return err
		}
		mb.conn = conn
	}
	return nil
}

func (mb *tcpTransporter) isoConnect() error {
	msg := make([]byte, len(isoConnectionRequestTelegram))
	copy(msg, isoConnectionRequestTelegram)
	msg[16] = mb.localTSAPHigh
	msg[17] = mb.localTSAPLow
	msg[20] = mb.remoteTSAPHigh
	msg[21] = mb.remoteTSAPLow

	mb.mu.Lock()
	defer mb.mu.Unlock()
	// The TPDU size belongs to one connection: start from the default so
	// a smaller limit of a previous connection is never inherited,
	// whatever the confirm below negotiates and whether it parses.
	mb.tpduSize = cotpDefaultTPDUSize
	if mb.conn == nil {
		return fmt.Errorf("Connection to address %s is null", mb.Address)
	}
	if mb.Timeout > 0 {
		if err := mb.conn.SetDeadline(time.Now().Add(mb.Timeout)); err != nil {
			return err
		}
	}
	// Sends the connection request telegram
	mb.logf("s7: sending % x", msg)
	if err := mb.writeFull(msg); err != nil {
		return err
	}
	tpdu, err := mb.readTPKT()
	if err != nil {
		return err
	}
	mb.logf("s7: received % x\n", tpdu)
	// The confirm echoes the source reference of the request as its
	// destination reference.
	tpduSize, err := parseConnectionConfirm(tpdu, binary.BigEndian.Uint16(msg[8:]))
	if err != nil {
		return err
	}
	mb.tpduSize = tpduSize
	return nil
}

// parseConnectionConfirm parses the TPDU of a COTP class 0 Connection
// Confirm: the fixed part and the TLV parameters behind it, in whatever
// order they come. wantDstRef is the source reference of the connection
// request, which the confirm has to echo as its destination reference.
// Unknown parameters are skipped. The result is the negotiated TPDU size:
// the C0 parameter, or the 1024 bytes of the request when it is absent.
// Malformed, truncated or repeated TPDU-size parameters fail the connect.
func parseConnectionConfirm(tpdu []byte, wantDstRef uint16) (int, error) {
	if len(tpdu) < 7 {
		return 0, fmt.Errorf("%s: connection confirm of %d bytes", ErrorText(errIsoInvalidPDU), len(tpdu))
	}
	if li := int(tpdu[0]); li != len(tpdu)-1 {
		return 0, fmt.Errorf("%s: length indicator %d, TPDU holds %d bytes", ErrorText(errIsoInvalidPDU), li, len(tpdu)-1)
	}
	if tpdu[1] != cotpConfirmTPDU {
		return 0, fmt.Errorf("%s: COTP TPDU 0x%02x is not a connection confirm", ErrorText(errIsoConnect), tpdu[1])
	}
	if dstRef := binary.BigEndian.Uint16(tpdu[2:]); dstRef != wantDstRef {
		return 0, fmt.Errorf("%s: destination reference 0x%04x, want 0x%04x", ErrorText(errIsoConnect), dstRef, wantDstRef)
	}
	if tpdu[6]&0xF0 != 0 {
		return 0, fmt.Errorf("%s: transport class %d, want class 0", ErrorText(errIsoConnect), tpdu[6]>>4)
	}
	tpduSize := cotpDefaultTPDUSize
	seenSize := false
	params := tpdu[7:]
	for len(params) > 0 {
		if len(params) < 2 {
			return 0, fmt.Errorf("%s: truncated parameter % x", ErrorText(errIsoInvalidPDU), params)
		}
		code, length := params[0], int(params[1])
		if len(params) < 2+length {
			return 0, fmt.Errorf("%s: parameter 0x%02x truncated", ErrorText(errIsoInvalidPDU), code)
		}
		if code == cotpSizeParam {
			if seenSize {
				return 0, fmt.Errorf("%s: repeated TPDU-size parameter", ErrorText(errIsoInvalidPDU))
			}
			seenSize = true
			if length != 1 {
				return 0, fmt.Errorf("%s: TPDU-size parameter of %d bytes", ErrorText(errIsoInvalidPDU), length)
			}
			exponent := int(params[2])
			if exponent < cotpMinTPDUExponent || exponent > cotpMaxTPDUExponent {
				return 0, fmt.Errorf("%s: TPDU-size exponent %d", ErrorText(errIsoInvalidPDU), exponent)
			}
			tpduSize = 1 << uint(exponent)
		}
		params = params[2+length:]
	}
	return tpduSize, nil
}
func (mb *tcpTransporter) negotiatePduLength() error {
	// Set PDU Size Requested //lth
	pduSizePackage := make([]byte, len(s7PDUNegogiationTelegram))
	copy(pduSizePackage, s7PDUNegogiationTelegram)
	binary.BigEndian.PutUint16(pduSizePackage[23:], uint16(pduSizeRequested))
	// Sends the connection request telegram
	response, err := mb.Send(pduSizePackage)
	length := len(response)
	if length == 27 && response[17] == 0 && response[18] == 0 { // 20 = size of Negotiate Answer
		// Get PDU Size Negotiated
		mb.PDULength = int(binary.BigEndian.Uint16(response[25:]))
		if mb.PDULength <= 0 {
			err = fmt.Errorf(ErrorText(errCliNegotiatingPDU))
		}
	} else {
		err = fmt.Errorf(ErrorText(errCliNegotiatingPDU))
	}
	return err
}
func (mb *tcpTransporter) startCloseTimer() {
	if mb.IdleTimeout <= 0 {
		return
	}

	if mb.closeTimer == nil {
		mb.closeTimer = time.AfterFunc(mb.IdleTimeout, mb.closeIdle)
	} else {
		mb.closeTimer.Reset(mb.IdleTimeout)
	}
}

// Close closes current connection.
func (mb *tcpTransporter) Close() error {
	mb.mu.Lock()
	defer mb.mu.Unlock()

	return mb.close()
}

// flush flushes pending data in the connection,
// returns io.EOF if connection is closed.
func (mb *tcpTransporter) flush(b []byte) (err error) {
	if err = mb.conn.SetReadDeadline(time.Now()); err != nil {
		return
	}
	// Timeout setting will be reset when reading
	if _, err = mb.conn.Read(b); err != nil {
		// Ignore timeout error
		if netError, ok := err.(net.Error); ok && netError.Timeout() {
			err = nil
		}
	}
	return
}

func (mb *tcpTransporter) logf(format string, v ...interface{}) {
	if mb.Logger != nil {
		mb.Logger.Printf(format, v...)
	}
}

// closeLocked closes current connection. Caller must hold the mutex before calling this method.
func (mb *tcpTransporter) close() (err error) {
	// The negotiated TPDU size dies with its connection.
	mb.tpduSize = 0
	if mb.conn != nil {
		err = mb.conn.Close()
		mb.conn = nil
	}
	return
}

// closeIdle closes the connection if last activity is passed behind IdleTimeout.
func (mb *tcpTransporter) closeIdle() {
	mb.mu.Lock()
	defer mb.mu.Unlock()

	if mb.IdleTimeout <= 0 {
		return
	}
	idle := time.Now().Sub(mb.lastActivity)
	if idle >= mb.IdleTimeout {
		mb.logf("s7: closing connection due to idle timeout: %v", idle)
		mb.close()
	}
}

// isS7PDU reports whether the telegram carries an S7 PDU behind TPKT and COTP.
// COTP connection requests/confirms and truncated telegrams do not.
func isS7PDU(telegram []byte) bool {
	return len(telegram) >= s7PDURefOffset+2 && telegram[isoHSize] == s7ProtocolID
}

// pduReference returns the PDU reference of a telegram for which isS7PDU holds.
func pduReference(telegram []byte) uint16 {
	return binary.BigEndian.Uint16(telegram[s7PDURefOffset:])
}

// pduReferenceMismatch reports whether response is an S7 PDU that does not
// echo the PDU reference of the S7 request, i.e. it answers another request.
func pduReferenceMismatch(request, response []byte) bool {
	return isS7PDU(request) && isS7PDU(response) && pduReference(request) != pduReference(response)
}

// Verify checks that the response answers the request: the PDU reference the
// client stamped into the request header must come back in the response.
// A response with another reference belongs to an earlier request, for
// example a late answer after a timeout, and must not be interpreted as the
// answer to this one.
func (mb *tcpPackager) Verify(request []byte, response []byte) (err error) {
	if pduReferenceMismatch(request, response) {
		err = fmt.Errorf("%s: request 0x%04x, response 0x%04x", ErrorText(errCliPDUReferenceMismatch), pduReference(request), pduReference(response))
	}
	return
}
